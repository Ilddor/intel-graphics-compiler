/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/

#include <cmath>

#include "HWConformity.h"
#include "Optimizer.h"
#include "visa_wa.h"
#include "DebugInfo.h"
#include "G4Verifier.h"

using namespace vISA;

static G4_CondModifier getReverseCondMod( G4_CondModifier mod )
{
    switch(mod)
    {
    case Mod_z:
        return Mod_z;
    case Mod_e:
        return Mod_e;
    case Mod_nz:
        return Mod_nz;
    case Mod_ne:
        return Mod_ne;
    case Mod_g:
        return Mod_l;
    case Mod_ge:
        return Mod_le;
    case Mod_l:
        return Mod_g;
    case Mod_le:
        return Mod_ge;
    default:
        MUST_BE_TRUE( 0, "Invalid conditional modifier input for reversed conditional modifier." );
    }
    return Mod_cond_undef;
}

static bool isCompressedInst( G4_INST *inst ){
    return inst->isComprInst();
}

#define isUnitRegionRow( opnd, exec_size )      \
        ( opnd->isImm() ||      \
        opnd->isSrcRegRegion() && opnd->asSrcRegRegion()->getRegion()->width == exec_size || \
        opnd->isSrcRegRegion() && opnd->asSrcRegRegion()->getRegion()->vertStride == 0 )

G4_Align HWConformity::getDclAlignment( int opndBytes, G4_INST *inst, bool isScalar, G4_SubReg_Align &subAlign )
{
    G4_Align align = Either;
    subAlign = Get_G4_SubRegAlign_From_Size( (uint16_t) opndBytes );
    bool hasAccSrc = inst->hasACCSrc();

    if( hasAccSrc && subAlign < Sixteen_Word )
    {
        subAlign = Sixteen_Word;
    }

    if (!isScalar)
    {
        // certain instructions have additional alignment requirements for non-scalar sources
        if (!builder.hasAlign1Ternary() && inst->getNumSrc() == 3 && !inst->isSend() && subAlign < Eight_Word)
        {
            subAlign = Eight_Word;
        }
        if (inst->isMath())
        {
            subAlign = Sixteen_Word;
        }
    }

    return align;
}
/*
 *  create a new mov instruction and insert it before iter
 *  mov (esize) dst tmp:type
 *  where esize is "inst"'s execution size and insert it after "inst"
 *  return value is the new temp variable as a dst operand
 *  If dstAlign is specified, the new temp will at least be aligend to that size
 */
G4_DstRegRegion* HWConformity::insertMovAfter( INST_LIST_ITER& it, G4_DstRegRegion* dst, G4_Type type, G4_BB *bb, G4_SubReg_Align dstAlign )
{
    G4_INST* inst = *it;

    if( !dst )
    {
        return dst;
    }

    if (inst->hasNULLDst() )
    {
        return builder.createDstRegRegion(Direct,
                                dst->getBase(),
                                0,
                                0,
                                1,
                                type);
    }

    INST_LIST_ITER iter = it;
    iter++;
    unsigned char exec_size = inst->getExecSize();
    G4_Type execType = inst->isRawMov() ? dst->getType() : inst->getExecType();
    bool scalarSrc = true;

    for (int i = 0, numSrc = inst->getNumSrc(); i < numSrc; i++)
    {
        G4_Operand *src = inst->getSrc(i);
        if( !src->isImm() )
        {
            if (!(inst->isMath() && i == 1 && src->isNullReg()) &&
                (src->isSrcRegRegion() && !src->asSrcRegRegion()->isScalar()))
            {
                scalarSrc = false;
            }
        }
        else if( IS_VINTTYPE(src->getType()) || IS_VFTYPE(src->getType()) )
        {
            scalarSrc = false;
        }
    }

    uint8_t newExecSize = ((inst->opcode() == G4_sel || inst->getImplAccSrc() || !scalarSrc) ? exec_size : 1);

    uint32_t opExecWidthBytes = newExecSize * G4_Type_Table[execType].byteSize;
    if( execType == Type_DF && IS_BTYPE( type ) )
    {
        type = ( type == Type_UB ? Type_UW : Type_W );
    }
    uint16_t dstWidthBytes = newExecSize * G4_Type_Table[type].byteSize;
    uint16_t scale = G4_Type_Table[execType].byteSize / G4_Type_Table[type].byteSize;
    /*   so according to comments in function that call it MAD needs to have packed format.
        It ends up with hStride 2, due to DefHoisting.
        So it is trying to undo it.
        For every other type if srcType > dstCype we need to adjust regions.
        This is not necessary for HF. It's already packed.

        The src region of move is wrong. Since for HF it is packed, unlike other data types.
        mad (8) r56.0.xyzw:hf -r37.0.xyzw:f r59.0.xyzw:hf r58.0.xyzw:hf {Align16, NoMask}
        mov (16) r44.0<2>:hf r56.0<16;8,2>:hf {Align1, H1} // #??:$39:%66
    */
    if( scale == 0 || (getGenxPlatform() >= GENX_CHV && execType == Type_F && type == Type_HF))
    {
        scale = 1;
    }

    G4_SubReg_Align subAlign; // set by getDclAlignment
    G4_Align align = getDclAlignment( opExecWidthBytes > dstWidthBytes ? opExecWidthBytes : dstWidthBytes,
        inst, newExecSize == 1, subAlign );

    if (subAlign < dstAlign)
    {
        subAlign = dstAlign;
    }

    RegionDesc* region = newExecSize > 1 ? builder.createRegionDesc(scale, 1, 0) : builder.getRegionScalar();

    G4_Declare* dcl = builder.createTempVar( newExecSize == 1 ? 1 : newExecSize * scale, type, align, subAlign );

    G4_SrcRegRegion *srcRegion = builder.Create_Src_Opnd_From_Dcl( dcl, region );
    G4_Predicate *pred = NULL;

    if (inst->opcode() != G4_sel)
    {
        pred = inst->getPredicate();
        inst->setPredicate( NULL );
        // maintainDU4TempMov will update def-use
    }

    unsigned int new_option = inst->getMaskOption();
    G4_INST* newInst = builder.createInternalInst( pred, G4_mov, NULL, inst->getSaturate(),
        exec_size, dst, srcRegion, NULL, new_option, inst->getLineNo(), inst->getCISAOff(),
        inst->getSrcFilename() );
    bb->instList.insert( iter, newInst );

    // update propagation info
    maintainDU4TempMov( inst, newInst );

    if( type == dst->getType() )
    {
        newInst->setSaturate( false );
    }
    else if (type == Type_F || type == Type_DF)
    {
        inst->setSaturate(false);
    }

    inst->setExecSize( newExecSize );
    if (newExecSize == 1)
    {
        inst->setOptions((inst->getOption() & ~InstOpt_Masks ) | InstOpt_WriteEnable);
    }

    return builder.Create_Dst_Opnd_From_Dcl( dcl, scale);
}

//
// replace instruction (*it)' source srcPos, which must be a scalar/immediate,
// with a temp variable after inserting
// mov (esize) tmp<1>:type imm/scalar {options}
// before the instruction
// This is like insertMovBefore(), except that the latter will always use
// simd1 move for scalar/imm values, which may not be what we want
// NOTE: This does not check for redundant moves.  We are counting on a later LVN pass
// to clean them up
//
void HWConformity::broadcast(
    G4_BB* bb, INST_LIST_ITER it, int srcPos, G4_SubReg_Align align)
 {
     G4_INST* inst = *it;
     G4_Operand* src = inst->getSrc(srcPos);
     MUST_BE_TRUE(src->isImm() ||
         (src->isSrcRegRegion() && src->asSrcRegRegion()->isScalar()),
         "source must be an immediate or scalar");
     G4_Type type = src->getType();

     uint8_t execSize = inst->getExecSize();
     uint32_t instMask = inst->getMaskOption();

     // avoid simd16 Qword moves
     MUST_BE_TRUE(execSize * G4_Type_Table[type].byteSize <= 2 * GENX_GRF_REG_SIZ,
         "move can't exceed 2 GRFs");

     G4_Declare* dcl = builder.createTempVar( execSize, type, Either, align );
     G4_DstRegRegion* dst = builder.createDstRegRegion(
         Direct,
         dcl->getRegVar(),
         0,
         0,
         1,
         type);
     G4_INST* newInst = builder.createInternalInst( NULL, G4_mov, NULL, false,
         execSize, dst, src, NULL, instMask,
         inst->getLineNo(), inst->getCISAOff(), inst->getSrcFilename() );

     bb->instList.insert(it, newInst);

     RegionDesc* srcRegion = builder.getRegionStride1();
     G4_SrcRegRegion* newSrc = builder.Create_Src_Opnd_From_Dcl(dcl, srcRegion);
     inst->setSrc(newSrc, srcPos);
     newInst->addDefUse(inst, inst->getSrcOperandNum(srcPos));

 }

//
// A simplified version of insertMovBefore(), this copies raw bytes from source to a temp
// and replaces the original source with tmp.  This is primarily used to ensure operand alignment and region restrictions
// op (esize) ... (mod) src<region>:type
// -->
// mov (esize) tmp<1>:type src<region>:type
// op (esize) ... (mod) tmp<1;1,0>:type
//
// source must be a G4_SrcRegRegion (direct or indirect), immediates are not supported
// note that modifier is propagated from source to tmp, but region is not
//
//
G4_SrcRegRegion* HWConformity::insertCopyBefore(INST_LIST_ITER it, uint32_t srcNum,
    G4_SubReg_Align tmpAlign, G4_BB *bb)
{
    G4_INST* inst = *it;
    G4_Operand* src = inst->getSrc(srcNum);
    MUST_BE_TRUE(src != nullptr && src->isSrcRegRegion(), "source must be a SrcRegRegion");
    G4_SrcRegRegion* origSrc = src->asSrcRegRegion();

    uint8_t newExecSize = origSrc->isScalar() ? 1 : inst->getExecSize();
    G4_Declare* dcl = builder.createTempVar(newExecSize, origSrc->getType(), Either, tmpAlign);
    G4_SrcModifier modifier = origSrc->getModifier();
    origSrc->setModifier(Mod_src_undef);
    G4_DstRegRegion* dst = builder.Create_Dst_Opnd_From_Dcl(dcl, 1);

    G4_INST* movInst = builder.createInternalInst(nullptr, G4_mov, nullptr, false,
        newExecSize, dst, origSrc, nullptr, InstOpt_WriteEnable,
        inst->getLineNo(), inst->getCISAOff(), inst->getSrcFilename());

    bb->instList.insert(it, movInst);
    G4_SrcRegRegion* newSrc = builder.createSrcRegRegion(modifier, Direct, dcl->getRegVar(),
        0, 0, newExecSize == 1 ? builder.getRegionScalar() : builder.getRegionStride1(),
        dcl->getElemType());

    return newSrc;
}

/*
 *  create a new mov instruction
 *  mov (esize) tmp<1>:type src
 *  where esize is "inst"'s execution size and insert it before "inst"
 *  return value is the new temp variable as a source operand
 */
G4_Operand* HWConformity::insertMovBefore(
    INST_LIST_ITER it, uint32_t srcNum, G4_Type type, G4_BB *bb,
    G4_SubReg_Align tmpAlign )
{
    G4_INST* inst = *it;
    G4_Align align = Either;
    G4_SubReg_Align subAlign;
    RegionDesc* region = NULL;
    unsigned short vs = 0, hs = 0, wd = 1;
    unsigned char exec_size = inst->getExecSize();
    G4_Operand *src = inst->getSrc( srcNum );
    unsigned short scale = IS_BTYPE( src->getType() ) && src->getType() == type ? 2 : 1;

    uint8_t newExecSize = ((src->isImm() && !IS_VTYPE(src->getType())) ||
                        (src->isSrcRegRegion() && src->asSrcRegRegion()->isScalar()))
                        ? 1 : exec_size;

    if( newExecSize > 1 )
    {
        if (scale == 1 && !IS_VTYPE(src->getType()))
        {
            scale = (unsigned short) (G4_Type_Table[src->getType()].byteSize / G4_Type_Table[type].byteSize);
        }
        if( scale == 0 )
        {
            scale = 1;
        }
        hs = scale;
        if( isCompressedInst(inst) || G4_Type_Table[type].byteSize * exec_size * hs > G4_GRF_REG_NBYTES )
        {
                wd = exec_size / 2;
        }
        else
        {
            wd = exec_size;
        }
        vs = wd * hs;
    }
    else
    {
        vs = 0;
        wd = 1;
        hs = 0;
        scale = (unsigned short)(G4_Type_Table[src->getType()].byteSize / G4_Type_Table[type].byteSize);
        if (scale == 0)
        {
            scale = 1;
        }
    }

    region = builder.createRegionDesc(vs, wd, hs);

    int opExecWidthBytes = IS_VINTTYPE(src->getType()) ?
                            G4_GRF_REG_NBYTES/2 * ( exec_size > 8 ? exec_size/8 : 1 ) :
                            ( src->getType() == Type_VF ?
                            G4_GRF_REG_NBYTES/2 * ( exec_size > 4 ? exec_size/4 : 1 ) :
                            newExecSize * G4_Type_Table[type].byteSize * scale );

    align = getDclAlignment( opExecWidthBytes, inst, newExecSize == 1, subAlign );

    if (subAlign < tmpAlign)
    {
        subAlign = tmpAlign;
    }

    uint32_t newInstEMask = newExecSize == 1 ? InstOpt_WriteEnable : inst->getMaskOption();

    // due to old BDW regioning rule we need NoMask inst here so they can be split
    if (builder.getOptions()->isTargetCM() && getGenxPlatform() == GENX_BDW)
    {
        if (bb->isInSimdFlow())
        {
            newInstEMask = InstOpt_WriteEnable;
        }
    }

    G4_Declare* dcl = builder.createTempVar( newExecSize == 1 ? 1 : newExecSize * scale, type, align, subAlign );
    G4_DstRegRegion *dstRegion = builder.Create_Dst_Opnd_From_Dcl(dcl, scale);
    G4_INST* newInst = builder.createInternalInst(nullptr, G4_mov, nullptr, false,
        newExecSize, dstRegion, builder.duplicateOperand(src), nullptr, newInstEMask,
        inst->getLineNo(), inst->getCISAOff(), inst->getSrcFilename() );
    bb->instList.insert( it, newInst );
    inst->transferDef( newInst, Gen4_Operand_Number(srcNum + 1), Opnd_src0 );
    newInst->addDefUse(inst, Gen4_Operand_Number(srcNum + 1));

    G4_SrcModifier modifier = Mod_src_undef;
    if (src->isSrcRegRegion() && src->asSrcRegRegion()->getModifier() == Mod_Not)
    {
        // mov doesn't support logic modifiers, so we keep it on the new source
        modifier = Mod_Not;
        newInst->getSrc(0)->asSrcRegRegion()->setModifier(Mod_src_undef);
    }

    return builder.createSrcRegRegion(
                        modifier,
                        Direct,
                        dcl->getRegVar(),
                        0,
                        0,
                        region,
                        dcl->getElemType());
}

void HWConformity::fixPackedSource(INST_LIST_ITER it, G4_BB *bb, G4_Type extype)
{
    G4_INST* inst = *it;

    bool nonTypeWFound = false, nonTypeFFound = false, incompatibleTypeFound = false;

    for( int i = 0; i < G4_Inst_Table[inst->opcode()].n_srcs; i++ )
    {
        G4_Operand *src = inst->getSrc(i);
        if( !src || !(IS_VTYPE(src->getType())))
        {
            // Make sure other src operands are of word type only as this is a HW requirement
            if( src &&
                ( src->getType() != Type_W &&
                src->getType() != Type_UW ) )
            {
                nonTypeWFound = true;
            }
            if( src &&
                ( src->getType() != Type_F ) )
            {
                nonTypeFFound = true;
            }


            continue;
        }
        G4_Type target_type = Type_W;
        if( src->getType() == Type_VF )
        {
            target_type = Type_F;
        }

        if( target_type == Type_W && nonTypeWFound == true )
        {
            // non-word type src is not allowed to co-exist with :v src
            incompatibleTypeFound = true;
        }
        else if( target_type == Type_F && nonTypeFFound == true )
        {
            // non-float type src is not allowed to co-exist with :vf src
            incompatibleTypeFound = true;
        }

        // Insert a move only if immediate operand is not
        // last src operand
        if( i != G4_Inst_Table[inst->opcode()].n_srcs - 1 ||
            incompatibleTypeFound == true )
        {
            inst->setSrc( insertMovBefore( it, i, target_type, bb), i );
        }
    }
}
/*
 * fixMathInst() checks the following:
 * The math instruction can only use GRF registers as source(s) and destination.
 * The math instruction does not support indirect addressing modes.
 * When Align1 mode is used,  source horizontal stride must be 1 with the exception of scalar sources and destination horizontal stride must be always 1.
 * Source and destination offset must be the same, except the case of scalar source
 * DW and UD is the only source format supported for INT DIV, float32 is the only source format supported for all the other functions.
 * Mixed DW and UD sources are not allowed for the INT DIV function.
 * For single source math function, <src1> must be programmed as ARF-NULL register.
 * The FDIV function is not supported in ALT_MODE.
 */
bool HWConformity::fixMathInst(INST_LIST_ITER it, G4_BB *bb)
{
    G4_INST* inst = *it;
    G4_DstRegRegion *dst = inst->getDst();
    G4_Operand *src0 = inst->getSrc(0), *src1 = inst->getSrc(1);
    bool mov_dst = false;

    MUST_BE_TRUE(inst->isMath(), "Expect math instruction");

    if (inst->asMathInst()->getMathCtrl() == MATH_INVM ||
        inst->asMathInst()->getMathCtrl() == MATH_RSQRTM)
    {
        return false;
    }

    // SKIP mixed mode instructions which are already handled by fixMixedHFInst.

    // covers MATH_INT_DIV, MATH_INT_DIV_QUOT, MATH_INT_DIV_REM
    bool isIntDivide = inst->asMathInst()->isMathIntDiv();
    bool hasSameOffset = hasSameSubregOffset(inst);

    // check if the source needs a move and if so the new move type 
    auto needsMove = [this, inst, isIntDivide, hasSameOffset](int srcID, G4_Type& newType)
    {
        assert((srcID == 0 || srcID == 1) && "math can have at most two sources");
        G4_Operand* src = inst->getSrc(srcID);
        newType = src->getType();
        if (isIntDivide)
        {
            G4_Type divType = IS_UNSIGNED_INT(inst->getSrc(0)->getType()) && IS_UNSIGNED_INT(inst->getSrc(1)->getType()) ? 
                Type_UD : Type_D;
            if (newType != divType)
            {
                newType = divType;
                return true;
            }
        }
        else if ((src->getType() != Type_F && src->getType() != Type_VF) &&
                 (getGenxPlatform() == GENX_BDW || src->getType() != Type_HF))
        {
            // CHV+ supports F/HF math, while BDW only supports F math
            // mix mode math is handled in fixMixedHFInst()
            newType = Type_F;
            return true;
        }

        if (src->isImm())
        {
            if (srcID == 0 && inst->asMathInst()->getMathCtrl() >= MATH_FDIV)
            {
                return true;
            }
        }
        else if (src->isSrcRegRegion())
        {
            G4_SrcRegRegion *srcRegion = src->asSrcRegRegion();
            RegionDesc *rd = srcRegion->getRegion();
            if (srcRegion->getModifier() != Mod_src_undef && isIntDivide)
            {
                // no source modifer for int divide 
                return true;
            }
            else if (srcRegion->getRegAccess() != Direct)
            {
                return true;
            }
            else if (!srcRegion->isScalar())
            {
                if (!hasSameOffset && !builder.isOpndAligned(srcRegion, GENX_GRF_REG_SIZ))
                {
                    return true;
                }
                else if (!rd->isContiguous(inst->getExecSize()))
                {
                    return true;
                }
            }
        }
        else
        {
            ASSERT_USER(false, "Unexpected math source!");
        }
        return false;
    };

    if (src0)
    {
        G4_Type src0_type = src0->getType();
        bool needsSrc0Mov = needsMove(0, src0_type);
        if (needsSrc0Mov)
        {
            inst->setSrc(insertMovBefore(it, 0, src0->isImm() ? getNonVectorType(src0_type) : src0_type, bb), 0);
            src0 = inst->getSrc(0);
        }
    }

    bool nullSrc1 = src1 && src1->isNullReg();
    if (!nullSrc1 && src1)
    {
        G4_Type src1_type = src1->getType();
        bool needsSrc1Move = needsMove(1, src1_type);

        if (needsSrc1Move)
        {
            if (isIntDivide && src1->isImm() && !IS_VINTTYPE(src1->getType()))
            {
                // just change the immediate's type
                uint32_t immVal = (uint32_t)src1->asImm()->getImm();
                inst->setSrc(builder.createImm(immVal, src1_type), 1);
            }
            else
            {
                inst->setSrc(insertMovBefore(it, 1, src1->isImm() ? getNonVectorType(src1_type) : src1_type, bb), 1);  
            }
            src1 = inst->getSrc(1);
        }
    }

    if (nullSrc1 && src0 && src1->getType() != src0->getType())
    {
        G4_SrcRegRegion *src1_opnd = builder.createNullSrc(inst->getSrc(0)->getType());
        inst->setSrc(src1_opnd, 1);
    }

    // recompute as src0 and src1 may have been modified
    hasSameOffset = hasSameSubregOffset(inst);
    G4_Type extype = inst->getExecType2();
    bool cond1 = (dst->getType() != extype && !(dst->getType() == Type_UD && extype == Type_D));
    if (dst->getRegAccess() != Direct || dst->getHorzStride() != 1 || cond1 ||
        (!hasSameOffset && inst->getExecSize() != 1 && !builder.isOpndAligned(dst, GENX_GRF_REG_SIZ)))
    {
        mov_dst = true;
        G4_DstRegRegion *new_dst = insertMovAfter(it, dst, extype, bb);
        inst->setDest(new_dst);
    }
    return mov_dst;
}

//  find a common (integer) type for constant folding.  The rules are:
//  -- both types must be int
//  -- Q and UQ are not folded
//  -- UD if one of the type is UD
//  -- D otherwise
//
//  returns Type_UNDEF if no appropriate type can be found
//
static G4_Type findConstFoldCommonType( G4_Type type1, G4_Type type2 )
{
     if (IS_TYPE_INT(type1) && IS_TYPE_INT(type2))
     {
         if (G4_Type_Table[type1].byteSize == 8 || G4_Type_Table[type2].byteSize == 8)
         {
             return Type_UNDEF;
         }
         if (type1 == Type_UD || type2 == Type_UD)
         {
             return Type_UD;
         }
         else
         {
             return Type_D;
         }
     }
     return Type_UNDEF;
}

//
// returns true if all sources and dst in this inst have the same fixed subreg offset
// null src/dst, scalar sources and immediates are excluded from the check
//
bool HWConformity::hasSameSubregOffset(G4_INST* inst) const
{
    bool anyOffset = true; // true means offset is not fixed yet
    uint32_t byteOffset = 0;
    if (inst->getDst())
    {
        G4_DstRegRegion* dst = inst->getDst();
        if (dst->isNullReg())
        {
            // do nothing
        }
        else if (dst->hasFixedSubregOffset(byteOffset))
        {
            anyOffset = false;
        }
        else
        {
            return false;
        }
    }

    for (int i = 0; i < inst->getNumSrc(); ++i)
    {
        G4_Operand* src = inst->getSrc(i);
        if (src->isSrcRegRegion())
        {
            uint32_t srcOffset = 0;
            G4_SrcRegRegion* srcRegion = src->asSrcRegRegion();
            if (srcRegion->isNullReg() || srcRegion->isScalar())
            {
                continue;
            }
            else if (srcRegion->hasFixedSubregOffset(srcOffset))
            {
                if (anyOffset)
                {
                    byteOffset = srcOffset;
                    anyOffset = false;
                }
                else if (srcOffset != byteOffset)
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
        }
    }

    return true;
}

// Check the following rules
// -- src0 in 2 source instructions may not be immediate.  We try to swap for src0 and src1 for
//    commutative instructions in such cases
// -- ARF may not be in src1
void HWConformity::fixImmAndARFSrc(INST_LIST_ITER it, G4_BB *bb)
{
    G4_INST* inst = *it;
    if (inst->isSend())
    {
        return;
    }

    G4_Operand *src0, *src1, *src2;
    src0 = inst->getSrc(0);
    src1 = inst->getSrc(1);
    src2 = inst->getSrc(2);

    /* Check for usage of two constants in binary operations */
    if (src0 != NULL && (src0->isImm() || src0->isAddrExp()) && G4_Inst_Table[inst->opcode()].n_srcs == 2)
    {
        if (INST_COMMUTATIVE(inst->opcode()) && !src1->isImm())
        {
            //all commutative inst must have 2 sources
            if (inst->opcode() == G4_mul)
            {
                bool needConstMov;
                //for DW and W mul, src0 must be DW and src1 W
                needConstMov = IS_DTYPE(src0->getType()) && !IS_DTYPE(src1->getType());

                if (needConstMov)
                {
                    G4_Type tmpType = getNonVectorType(src0->getType());

                    G4_Operand* newSrc0 = insertMovBefore(it, 0, tmpType, bb);
                    inst->setSrc(newSrc0, 0);
                }
                else
                {
                    // swap operands
                    inst->setSrc(src1, 0);
                    inst->setSrc(src0, 1);
                    inst->swapDefUse();
                }
            }
            else
            {
                // swap operands
                inst->setSrc(src1, 0);
                inst->setSrc(src0, 1);
                inst->swapDefUse();
            }
        }
        /*
        * A select operation isn't commutative, but we may commute the
        * operands provided we perform a predicate inversion as well.
        * (v0)  sel ... const V1
        *    =>
        * (-v0) sel ... V1 const
        */
        else if (inst->opcode() == G4_sel && !src1->isImm())
        {
            bool SwapOpnd = false;
            G4_CondMod *cond = inst->getCondMod();
            if (cond != NULL)
            {
                switch (cond->getMod())
                {
                case Mod_ne:
                {
                    inst->setCondMod(builder.createCondMod(Mod_e, cond->getBase(), 0));
                    SwapOpnd = true;
                    break;
                }
                case Mod_e:
                {
                    inst->setCondMod(builder.createCondMod(Mod_ne, cond->getBase(), 0));
                    SwapOpnd = true;
                    break;
                }
                default:
                    break; // Prevent gcc warning
                }
            }
            else
            {
                G4_Predicate* pred = inst->getPredicate();
                MUST_BE_TRUE(pred != NULL, "predicate must not be null");
                G4_PredState reverse = pred->getState() == PredState_Minus ? PredState_Plus : PredState_Minus;
                inst->setPredicate(builder.createPredicate(
                    reverse, pred->getBase(), pred->getSubRegOff(), pred->getControl()));
                SwapOpnd = true;
            }

            if (SwapOpnd)
            {
                inst->setSrc(src1, 0);
                inst->setSrc(src0, 1);
                inst->swapDefUse();
            }
            else
            {
                G4_Type tmpType = getNonVectorType(src0->getType());
                G4_Operand* newSrc0 = insertMovBefore(it, 0, tmpType, bb);
                inst->setSrc(newSrc0, 0);
            }
        }
        else if (!inst->isMath())
        {
            // math immediate src0 is handled separately in fixMathInst()
            if ((inst->opcode() == G4_add || inst->opcode() == G4_mul) &&
                src0->isImm() && src1->isImm() &&
                IS_TYPE_INT(src0->getType()) && IS_TYPE_INT(src1->getType()) &&
                inst->getSaturate() == false)
            {
                // FIXME: this is duplicating the functionality of Optimizer::doConsFolding.
                G4_Type src0T = src0->getType(), src1T = src1->getType(), resultType = src0T;

                resultType = findConstFoldCommonType(src0T, src1T);
                if (resultType != Type_UNDEF)
                {
                    G4_Imm *newSrc = NULL;
                    int64_t res = inst->opcode() == G4_add ?
                        ((int64_t)(src0->asImm()->getInt()) + (int64_t)(src1->asImm()->getInt())) :
                        ((int64_t)(src0->asImm()->getInt()) * (int64_t)(src1->asImm()->getInt()));

                    // don't fold if the value overflows D/UD
                    if (G4_Imm::isInTypeRange(res, resultType))
                    {
                        newSrc = builder.createImmWithLowerType(res, resultType);

                        // change instruction into a MOV
                        inst->setOpcode(G4_mov);
                        inst->setSrc(newSrc, 0);
                        inst->setSrc(NULL, 1);
                        return;
                    }
                }
            }
            // If src0 is not 64-bit, src1 is 64-bit, swap them to save one move.
            if (INST_COMMUTATIVE(inst->opcode()) && src0->isImm() && src1->isImm() &&
                G4_Type_Table[src0->getType()].byteSize != 8 &&
                G4_Type_Table[src1->getType()].byteSize == 8)
            {
                inst->setSrc(src1, 0);
                inst->setSrc(src0, 1);
                inst->swapDefUse();
                src0 = inst->getSrc(0);
                src1 = inst->getSrc(1);
            }
            if (INST_COMMUTATIVE(inst->opcode()) && src0->isAddrExp() && src1->isImm())
            {
                // The original IR has both addr expr and immediate
                //   add(8) A0(0, 0)<1>:uw &V36 + 0 0xeca86420 : uv{ Align1, Q1 }
                // We insert a move for src1 which is an immediate
                //   mov(8) TV0(0, 0)<1> : uw 0xeca86420 : uv{ Align1 }
                //   add(8) A0(0, 0)<1> : uw &V36 + 0 TV0(0, 0)<8; 8, 1> : uw{ Align1, Q1 }
                G4_Type type = src1->getType();
                inst->setSrc(insertMovBefore(it, 1, getNonVectorType(type), bb), 1);
                // And we swap addr expr and the new variable
                //   add(8) A0(0, 0)<1> : uw TV0(0, 0)<8; 8, 1> : uw &V36 + 0 {Align1, Q1}
                // The final code sequence is
                //   mov(8) r13.0<1>:uw 0xeca86420 : uv{ Align1 } // #26:$9:%79
                //   add(8) a0.0<1> : uw r13.0<8; 8, 1> : uw 0x60 : uw{ Align1, Q1 }
                inst->setSrc(inst->getSrc(1), 0);
                inst->setSrc(src0, 1);
                inst->swapDefUse();
            }
            else
            {
                G4_Type newSrcType = inst->needsDWType() ? (IS_UNSIGNED_INT(src0->getType()) ? Type_UD : Type_D) : src0->getType();
                inst->setSrc(insertMovBefore(it, 0, newSrcType, bb), 0);
            }
        }
    }

    src0 = inst->getSrc(0);
    src1 = inst->getSrc(1);
    src2 = inst->getSrc(2);

    // check for non-mad 3src inst
    if (G4_Inst_Table[inst->opcode()].n_srcs == 3 && src1->isImm())
    {
        inst->setSrc(insertMovBefore(it, 1, INST_FLOAT_SRC_ONLY(inst->opcode()) ? Type_F : src1->getType(), bb), 1);
    }

    // Architecture registers may not appear as src1.
    auto isARF = [](G4_Operand* opnd) { return opnd->isAreg() || opnd->isFlag(); };
    if (src1 != nullptr && isARF(src1) && !src1->isNullReg())
    {
        /* See if we can swap the src1 */
        if (INST_COMMUTATIVE(inst->opcode()) && !isARF(src0))
        {
            inst->setSrc(src1, 0);
            inst->setSrc(src0, 1);
            inst->swapDefUse();
        }
        /* Otherwise introduce a tmp */
        inst->setSrc(insertMovBefore(it, 1, INST_FLOAT_SRC_ONLY(inst->opcode()) ? Type_F : src1->getType(), bb), 1);
    }

    src2 = inst->getSrc(2);

    /* 3 src instructions can't have any constants */
    if (!builder.hasAlign1Ternary() && src2 != nullptr && src2->isImm())
    {
        inst->setSrc(insertMovBefore(it, 2, src2->getType(), bb), 2);
    }
}

bool HWConformity::fixLine(INST_LIST_ITER it, G4_BB *bb)
{
    G4_INST* inst = *it;

    if (inst->opcode() == G4_line)
    {
        bool badRegion = false;
        G4_Operand* src0 = inst->getSrc(0);
        // assumption: there are 4 elements in src0
        if (src0->isSrcRegRegion())
        {
            RegionDesc *rd = src0->asSrcRegRegion()->getRegion();
            badRegion = (rd->vertStride != 0 || rd->width != 4 || rd->horzStride != 1);
        }
        if (!IS_FTYPE(src0->getType()) || src0->isImm() || badRegion ||
            !builder.isOpndAligned(src0, G4_GRF_REG_NBYTES / 2))
        {
            // insertMovBefore()  is not used here
            // due to the special region <0;4,1> of src0 of line
            G4_Declare *src0_dcl;
            G4_DstRegRegion *new_dst_opnd;
            G4_SrcRegRegion *new_src0_opnd;
            unsigned char mov_size = 4;

            src0_dcl = builder.createTempVar(mov_size, Type_F, Either, Eight_Word);
            /* Create temporary variable */
            // Actully we set region to be <0;4,1> directly here.
            RegionDesc *rd = builder.createRegionDesc(0, 4, 1);
            new_src0_opnd = builder.Create_Src_Opnd_From_Dcl(src0_dcl, rd);
            new_dst_opnd = builder.Create_Dst_Opnd_From_Dcl(src0_dcl, 1);

            G4_INST* newInst = builder.createInternalInst(NULL, G4_mov, NULL, false,
                mov_size, new_dst_opnd, src0, NULL, InstOpt_NoOpt, inst->getLineNo(),
                inst->getCISAOff(), inst->getSrcFilename());
            if (bb->isInSimdFlow())
            {
                newInst->setOptions((newInst->getOption() & ~InstOpt_Masks) | InstOpt_WriteEnable);
            }
            bb->instList.insert(it, newInst);
            inst->setSrc(new_src0_opnd, 0);
            return true;
        }
    }
    return false;
}

bool HWConformity::fixOpndType(INST_LIST_ITER it, G4_BB *bb)
{
    /*
    * Check for instruction that only accept float/int operands, as well as
    * instruction with mixed operand types.  Even though the CISA itself forbids
    * mixed type instructions, optimizations such as copy propagation
    * may reintroduce them and so we do the checks here
    */
    G4_INST* inst = *it;
    bool changed = false;
    int numSrc = inst->getNumSrc();
    bool has_float = false;
    bool has_int = false;

    if (inst->isSend())
    {
        return false;
    }

    for (int i = 0; i < numSrc; i++)
    {
        if (!inst->getSrc(i))
        {
            continue;
        }
        if (IS_FTYPE(inst->getSrc(i)->getType()) ||
            IS_VFTYPE(inst->getSrc(i)->getType()))
        {
            has_float = true;
        }
        else if (!IS_DFTYPE(inst->getSrc(i)->getType()) && !IS_HFTYPE(inst->getSrc(i)->getType()))
        {
            has_int = true;
        }
    }
    if (has_float && has_int)
    {
        for (int i = 0; i < numSrc; i++)
        {
            if (inst->getSrc(i) && !IS_FTYPE(inst->getSrc(i)->getType()) && !IS_DFTYPE(inst->getSrc(i)->getType()))
            {
                if (!((inst->opcode() == G4_smov) && (i == 1)))
                {
                    inst->setSrc(insertMovBefore(it, i, Type_F, bb), i);
                    changed = true;
                }
            }
        }
    }

    if (builder.noSrc1Byte())
    {
        if (numSrc > 1)
        {
            G4_Operand* src0 = inst->getSrc(0);
            G4_Operand* src1 = inst->getSrc(1);
            if (src0 != nullptr && src1 != nullptr && IS_BTYPE(src1->getType()))
            {
                if (!IS_BTYPE(src0->getType()) && inst->canSwapSource())
                {
                    inst->setSrc(src1, 0);
                    inst->setSrc(src0, 1);
                }
                else
                {
                    inst->setSrc(insertMovBefore(it, 1, Type_W, bb), 1);
                    changed = true;
                }
            }
        }
    }
    return changed;
}

/*
 * fixOpnds() looks for operands conformity:
 * 1. checks can operand be a constant.
 * 2. checks if operand's type is conformant to operation.
 * 3. check if only src0 uses VxH
 * 4. check if indirect scalar is used in compressed inst
 * It tries to fix these cases by changing operands order if possible
 * or by insertion if temporary location with appropriate conversion.
 */
void HWConformity::fixOpnds( INST_LIST_ITER it, G4_BB *bb, G4_Type& exType )
{
    G4_INST* inst = *it;
    if (inst->isSend())
    {
        return;
    }

    G4_Operand *src0, *src1, *src2;

    src0 = inst->getSrc(0);
    src1 = inst->getSrc(1);
    src2 = inst->getSrc(2);

    if( inst->opcode() == G4_mul )
    {
        if (IS_DTYPE(src1->getType()) &&
            !(IS_DTYPE(src0->getType()) || IS_FTYPE(src0->getType())))
        {
            // check if src0 uses VxH
            bool src0_use_VxH = false;

            if( src0->isSrcRegRegion() && src0->asSrcRegRegion()->getRegAccess() != Direct &&
                src0->asSrcRegRegion()->getRegion()->isRegionWH() ) // is this safe?
            {
                src0_use_VxH = true;
            }
            if( src0_use_VxH )
            {
                src0 = insertMovBefore( it, 0, src0->getType(), bb );
            }
            inst->setSrc( src1, 0 );
            inst->setSrc( src0, 1 );
            inst->swapDefUse();
            src0 = inst->getSrc(0);
            src1 = inst->getSrc(1);
        }

        if( src1->isSrcRegRegion() && src1->asSrcRegRegion()->getRegAccess() != Direct &&
            src1->asSrcRegRegion()->getRegion()->isRegionWH() )
        {
            if (IS_DTYPE(src0->getType()) &&
                !(IS_DTYPE(src1->getType()) || IS_FTYPE(src1->getType()) ) )
            {
                inst->setSrc( insertMovBefore( it, 1, src1->getType(), bb ), 1 );
            }
            else
            {
                inst->setSrc( src1, 0 );
                inst->setSrc( src0, 1 );
                inst->swapDefUse();
            }
            src0 = inst->getSrc(0);
            src1 = inst->getSrc(1);
        }
    }

    fixImmAndARFSrc(it, bb);

    src0 = inst->getSrc(0);
    src1 = inst->getSrc(1);
    src2 = inst->getSrc(2);

    // Vx1 and VxH can only be used for src0
    bool src0_use_VxH = false, src1_use_VxH = false;

    if( src2 &&
        src2->isSrcRegRegion() &&
        src2->asSrcRegRegion()->getRegion()->isRegionWH() ){
        inst->setSrc(insertMovBefore(it, 2, exType, bb), 2);
    }

    if( src0 != NULL &&
        src0->isSrcRegRegion() &&
        src0->asSrcRegRegion()->getRegion()->isRegionWH() ){
            src0_use_VxH = true;
    }

    if( src1 != NULL && !( inst->isMath() && src1->isNullReg() ) &&
        src1->isSrcRegRegion() &&
        src1->asSrcRegRegion()->getRegion()->isRegionWH() ){
            src1_use_VxH = true;
    }

    if( src1_use_VxH )
    {
        if( ( INST_COMMUTATIVE(inst->opcode()) || inst->opcode() == G4_cmp )
            && !src0_use_VxH &&
            ! ( inst->opcode() == G4_mul &&
            ( IS_DTYPE( src0->getType() ) ) ) )
        {
            inst->setSrc( src1, 0 );
            inst->setSrc( src0, 1 );
            if( inst->opcode() == G4_cmp )
            {
                // change condMod
                G4_CondMod *condMod = inst->getCondMod();
                if( condMod )
                {
                    G4_CondMod *newCondModOpnd = builder.createCondMod(
                        getReverseCondMod(condMod->getMod()), condMod->getBase(), condMod->getSubRegOff());
                    inst->setCondMod( newCondModOpnd );
                }
            }
        }
        else
        {
            inst->setSrc(insertMovBefore(it, 1, exType, bb), 1);
        }
    }

    if( inst->isComprInst() )
    {
        // check if there is indirect scalar or repeat region
        for( int i = 0; i < G4_Inst_Table[inst->opcode()].n_srcs; i++ )
        {
            G4_Operand *src = inst->getSrc(i);
            if( src && src->isSrcRegRegion() &&
                src->asSrcRegRegion()->getRegAccess() != Direct &&
                ( src->asSrcRegRegion()->isScalar() || src->asSrcRegRegion()->getRegion()->isRepeatRegion( inst->getExecSize() ) ) )
            {
                inst->setSrc(insertMovBefore(it, i, src->getType(), bb), i);
            }
        }
    }
}

void HWConformity::fixAlign13SrcInst(INST_LIST_ITER iter, G4_BB* bb)
{
    // again mad should already conform by construction
    G4_INST* inst = *iter;
    MUST_BE_TRUE(inst->getNumSrc() == 3 && !inst->isSend(), "expect 3src inst");

    if (inst->opcode() != G4_mad)
    {
        G4_DstRegRegion* dst = inst->getDst();
        if (!isGoodAlign1TernaryDst(inst))
        {
            auto alignment = builder.noSrc2Regioning() ? Sixteen_Word : Four_Word;
            G4_DstRegRegion* tmpDst = insertMovAfter(iter, dst, dst->getType(), bb, alignment);
            inst->setDest(tmpDst);
        }

        bool canBeImm = true;
        for (int i = 0; i < inst->getNumSrc(); ++i)
        {
            if (!isGoodAlign1TernarySrc(inst, i, canBeImm))
            {
                G4_SubReg_Align subalign = Any;
                if (i == 2)
                {
                    // when there's no scr2 regioning, 
                    // src2 has to hvae same offset as dst, we enforce it by making it GRF-aligned
                    subalign = builder.noSrc2Regioning() ? Sixteen_Word : Four_Word;
                }
                inst->setSrc(insertMovBefore(iter, i, inst->getSrc(i)->getType(), bb, subalign), i);
            }
            else
            {
                if (inst->getSrc(i)->isImm())
                {
                    canBeImm = false;
                }
            }
        }
    }
}

void HWConformity::fix3SrcInst(INST_LIST_ITER iter, G4_BB* bb)
{
    G4_INST* inst = *iter;
    if (inst->getNumSrc() != 3 || inst->isSend() || inst->opcode() == G4_madm)
    {
        return;
    }

    if (builder.hasAlign1Ternary())
    {
        fixAlign13SrcInst(iter, bb);
        return;
    }

    if (inst->opcode() != G4_mad)
    {
        // check that dst and srcs are legal for 3src.  We do not check
        // mad since they should already conform by construction
        uint8_t execSize = inst->getExecSize();
        G4_DstRegRegion* dst = inst->getDst();
        if (dst->getRegAccess() != Direct || dst->getHorzStride() != 1 ||
            !builder.isOpndAligned(dst, (execSize >= 8) ? 32 : execSize * 4))
        {
            G4_DstRegRegion* tmpDst = insertMovAfter(iter, dst, dst->getType(), bb);
            inst->setDest(tmpDst);
        }
        for (int i = 0; i < 3; i++)
        {
            if (!isGoodAlign16Src(inst, i))
            {
                inst->setSrc(
                    insertMovBefore(iter, i, inst->getSrc(i)->getType(), bb),
                    i);
            }
        }
    }

    //When it is set (Align16), the instruction uses 16-byte-aligned addressing for source and destination operands.
    if ((inst->getExecSize() == 1))
    {
        if (inst->getDst() &&
            inst->getDst()->getBase()->isRegVar())
        {
            if (!builder.isOpndAligned(inst->getDst(), 16))
            {
                G4_DstRegRegion *new_dst = insertMovAfter(iter, inst->getDst(), inst->getDst()->getType(), bb);
                G4_Declare* tmpDstDcl = new_dst->getTopDcl();
                tmpDstDcl->setSubRegAlign(Eight_Word);
                inst->setDest( new_dst );
            }
        }
    }

    if (inst->getExecSize() == 16)
    {
        /*
            According to Krishna, Narsim WA only applies if intruction is not contained in one GRF
        */
        bool wa3rc = (VISA_WA_CHECK(builder.getPWaTable(), WaDisableSIMD16On3SrcInstr)  &&
                        !(inst->getExecType() == Type_HF                                &&
                          inst->getOperand(Opnd_src1)->isSrcRegRegion()                 &&
                          inst->getOperand(Opnd_src1)->getType() == Type_HF             &&
                          !inst->getOperand(Opnd_src1)->asSrcRegRegion()->crossGRF()));

        if (wa3rc)
        {
            evenlySplitInst(iter, bb);
        }
    }
}
// return 1: packed word
bool HWConformity::isPackedWord( G4_Operand *src )
{
    if( !src || src->isSrcRegRegion() == false ||
        src->asSrcRegRegion()->getBase()->isNullReg() ){
            return false;
    }
    RegionDesc *rd = src->asSrcRegRegion()->getRegion();

    if( src->asSrcRegRegion()->getRegAccess() != Direct ||
        ( src->asSrcRegRegion()->getType() != Type_W && src->asSrcRegRegion()->getType() != Type_UW ) ||
        src->asSrcRegRegion()->getSubRegOff() != 0 ||
        rd->horzStride != 1 ||
        ( !( rd->width == 8 && rd->vertStride == 8 ) &&
        !( rd->width == 16 && rd->vertStride == 16 ) ) ){
            return false;
    }
    return true;
}

void HWConformity::fixCompareInst(
                                  INST_LIST_ITER i,
                                  G4_BB *bb,
                                  G4_Type exType,
                                  int dst_elsize )
{
    G4_INST *inst = *i;
    G4_Operand *dst = inst->getDst();

    if( dst && dst->isNullReg() )
    {
        // change dst hstride if necessary
        if( G4_Type_Table[exType].byteSize > G4_Type_Table[dst->getType()].byteSize )
        {
            // create a new dst with new stride
            G4_DstRegRegion *new_null = builder.createNullDst( exType );
            inst->setDest( new_null );
        }
        return;
    }
}

// For integer packing moves, we can replace the src type with the dst type instead of inserting
// a new move to satisfy dst alignment, since integer down conversion is based on truncation
// an inst has to satisfy the following properties:
// -- is a move (duh) and does not have conditional modifiers or saturation
// -- dst must be a direct DstRegRegion that is GRF-aligned
// -- src must be a direct SrcRegRegion with GRF base, no modifiers, and packed/scalar region
// -- both dst and src have integer type, with source stride > dst stride
// returns true if we have successfully down cast the src type
static bool canReplaceMovSrcType(IR_Builder& builder, G4_INST* inst, uint32_t extypesize)
{

    if (inst->opcode() != G4_mov || inst->getCondMod() != NULL || inst->getSaturate())
    {
        return false;
    }
    if (!inst->getSrc(0)->isSrcRegRegion())
    {
        return false;
    }

    G4_DstRegRegion* dst = inst->getDst();
    G4_SrcRegRegion* src0 = inst->getSrc(0)->asSrcRegRegion();
    int dstByteOffset = dst->getByteOffset();
    if (dstByteOffset % extypesize != 0 ||
        dst->getRegAccess() != Direct)
    {
        // don't do this if dst is not GRF aligned, since we have to fix it later anyway
        return false;
    }

    if (src0->getRegAccess() != Direct || src0->getModifier() != Mod_src_undef ||
        (src0->getTopDcl() == NULL || src0->getTopDcl()->getRegFile() != G4_GRF))
    {
        return false;
    }

    bool isIntPackingMove = false;
    if (IS_TYPE_INT(dst->getType()) && IS_TYPE_INT(src0->getType()))
    {
        uint32_t dstAlign = G4_Type_Table[dst->getType()].byteSize * dst->getHorzStride();
        if (dstAlign < G4_Type_Table[src0->getType()].byteSize)
        {
            isIntPackingMove = true;
        }
    }

    if (!isIntPackingMove)
    {
        return false;
    }

    // we only handle direct contiguous and scalar source region for now,
    // as VxH and strided regions are a bit harder to update
    if (src0->getRegion()->isContiguous(inst->getExecSize()))
    {
        uint16_t newHS = extypesize / G4_Type_Table[dst->getType()].byteSize;
        if (newHS > 4)
        {
            // rule out Q -> B moves if Q is not scalar
            return false;
        }
    }
    else if (!src0->isScalar())
    {
        // only handle scalar and contiguous regions for now
        return false;
    }

    // instead of inserting a move, we change src's type to be same as dst type
    // e.g.,
    // mov (8) r1.0<1>:b r2.4<8;8,1>:d
    // becomes
    // mov (8) r1.0<1>:b r2.16<32;8,4>:b
    // This is safe since integer down conversion is based on truncation
    uint32_t typeSizeRatio = extypesize / G4_Type_Table[dst->getType()].byteSize;
    uint32_t numElt = src0->isScalar() ? 1 : inst->getExecSize() * typeSizeRatio;
    G4_Declare* newDcl = builder.createTempVar(numElt, dst->getType(), Either, Any);
    newDcl->setAliasDeclare(src0->getBase()->asRegVar()->getDeclare(), 0);
    RegionDesc* region = src0->isScalar() ? builder.getRegionScalar() :
        builder.createRegionDesc((uint16_t)inst->getExecSize() * typeSizeRatio,
        inst->getExecSize(),
        (uint16_t)typeSizeRatio);
    G4_SrcRegRegion* newSrc = builder.createSrcRegRegion(
        Mod_src_undef,
        Direct,
        newDcl->getRegVar(),
        src0->getRegOff(),
        src0->getSubRegOff() * typeSizeRatio,
        region,
        dst->getType());
    inst->setSrc(newSrc, 0);
    return true;
}

// implement HW restrictions on mov
// -- There is no direct conversion from B/UB to DF or DF to B/UB.
//    Use two instructions and a word or DWord intermediate type.
// -- There is no direct conversion from B/UB to Q/UQ or Q/UQ to B/UB.
//    Use two instructions and a word or DWord intermediate integer type.
// -- There is no direct conversion from HF to DF or DF to HF.
//    Use two instructions and F (Float) as an intermediate type.
// -- There is no direct conversion from HF to Q/UQ or Q/UQ to HF.
//    Use two instructions and F (Float) or a word integer type or a DWord integer type as an intermediate type.
// returns true if a move is inserted
bool HWConformity::fixMov(INST_LIST_ITER i, G4_BB* bb)
{
    G4_INST* inst = *i;

    if (inst->opcode() != G4_mov)
    {
        return false;
    }

    G4_Type dstType = inst->getDst()->getType();
    G4_Type srcType = inst->getSrc(0)->getType();

    if (IS_BTYPE(dstType) && (IS_DFTYPE(srcType) || IS_QTYPE(srcType)))
    {
        // mov B Q/DF
        inst->setDest(insertMovAfter(i, inst->getDst(), Type_W, bb));
        return true;
    }
    else if (IS_BTYPE(srcType) && (IS_DFTYPE(dstType) || IS_QTYPE(dstType)))
    {
        // mov Q/DF B
        inst->setDest(insertMovAfter(i, inst->getDst(), Type_W, bb));
        return true;
    }
    else if (IS_HFTYPE(dstType) && (IS_DFTYPE(srcType) || IS_QTYPE(srcType)))
    {
        // mov HF Q/DF
        inst->setDest(insertMovAfter(i, inst->getDst(), Type_F, bb));
        return true;
    }
    else if (IS_HFTYPE(srcType) && (IS_DFTYPE(dstType) || IS_QTYPE(dstType)))
    {
        // mov Q/DF HF
        inst->setDest(insertMovAfter(i, inst->getDst(), Type_F, bb));
        return true;
    }
    return false;
}


bool HWConformity::fixDstAlignment( INST_LIST_ITER i, G4_BB* bb, G4_Type extype, unsigned int dst_elsize )
{
    G4_INST *inst = *i;
    bool insertMOV = false;

    unsigned char exec_size = inst->getExecSize();
    G4_DstRegRegion *dst = inst->getDst();
    G4_Operand *src0 = inst->getSrc(0);
    unsigned h_stride = dst->getHorzStride();
    unsigned int extypesize = G4_Type_Table[extype].byteSize;

    if (inst->hasNULLDst())
    {
        if (dst_elsize * h_stride < extypesize)
        {
            uint16_t newHStride = extypesize / dst_elsize;
            if (newHStride == 8)
            {
                // dst is a null byte, this can be produced by logical optimization
                // we chagne the type to W here; this should be safe since the conditional modifier
                // is either .ez or .nz
                MUST_BE_TRUE(dst_elsize == 1, "expect B/UB dst");
                dst->setType(dst->getType() == Type_B ? Type_W : Type_UW);
                dst->setHorzStride(4);
            }
            else
            {
                MUST_BE_TRUE(newHStride <= 4, "horizontal stride must be <=4");
                dst->setHorzStride(newHStride);
            }
        }

        return insertMOV;
    }

    // optimize initialization instructions
    if( inst->opcode() == G4_mov && src0->isImm() &&
        ( !bb->isInSimdFlow() || inst->isWriteEnableInst() ) &&
        !inst->getPredicate() &&
        dst->getRegAccess() == Direct &&
        dst->getHorzStride() == 1 &&
        inst->getSaturate() == false &&
        IS_BTYPE(dst->getType()) &&
        !IS_TYPE_F32_F64(src0->getType()) &&
        builder.isOpndAligned( dst, getTypeSize(src0->getType()) ) )
    {
        // inst is a mov with packed byte dst and int imm source
        int64_t value = src0->asImm()->getInt();
        uint64_t new_value = ( value & 0xFF ) | ( value << 0x8 );
        int scale = 2;

        if (IS_DTYPE(src0->getType()))
        {
            scale = 4;
            new_value = ( new_value & 0xFFFF ) | ( new_value << 0x10 );
        }

        if (exec_size >= scale)
        {
            G4_Type new_type = ( scale == 2 ) ? Type_UW : Type_UD;
            dst->setHorzStride( 1 );
            dst->setSubRegOff( (short) (dst->getSubRegOff() / scale) );
            dst->setType( new_type );
            inst->setSrc( builder.createImm( new_value, new_type ), 0 );
            inst->setExecSize( (unsigned char) (exec_size / scale) );
            return insertMOV;
        }
    }

    // 10.1.1 and 10.6.1
    bool byteDst = IS_BTYPE(dst->getType());

    // Byte can not be used as dstination of INT*INT
    if ((byteDst && inst->opcode() == G4_mul &&
        IS_TYPE_INT(inst->getSrc(0)->getType()) && IS_TYPE_INT(inst->getSrc(1)->getType())))
    {
        // change dst type to W
        inst->setDest( insertMovAfter( i, dst, Type_W, bb ) );
        return true;
    }

    if (byteDst && extypesize == 8)
    {
        // Gen doesn't support hstride 8, so we add a W move here
        inst->setDest(insertMovAfter(i, dst, Type_W, bb));
        return true;
    }

    unsigned short dst_byte_offset;
    builder.isOpndAligned(dst, dst_byte_offset, extypesize);
    if (!((dst_byte_offset % extypesize == 0)  ||
        (byteDst &&
            !VISA_WA_CHECK(builder.getPWaTable(), WaByteDstAlignRelaxedRule) &&
            (dst_byte_offset % extypesize == 1))
        ) ||
        /*
         * Dynamic offset can be odd for serialized instructions
         * or when horizontal offset is dynamic.
         * Probably we need the same for any dst with dynamic offsets.
         */
         (  dst_elsize < extypesize &&
            dst->getRegAccess() != Direct     &&
            !( byteDst && extypesize == 2 && exec_size == 1 )
         )                                                                  ||
         // 10.1.2
         ( exec_size > 1 && (dst_elsize * h_stride) < extypesize ) )
    {
        /*
         * 10.3
         * For byte dst type:
         * 1. no 1 horstride
         * 2. no odd start subreg
         * There is only one excpetion - raw mov op
         * Raw means src operand has no attribute.
         *
         * Note: Actually all these cases are now controlled
         *       by extypesize value.
         */

         if (inst->isRawMov() &&
             ( dst_byte_offset % extypesize == 0 ||
             ( byteDst && dst_byte_offset % extypesize == 1 ) ) )
         {
             return insertMOV;
         }

         if (canReplaceMovSrcType(builder, inst, extypesize))
         {
             return false;
         }
        
         if (inst->opcode() == G4_mov)
         {
             bool intHFConversion = false;
             G4_Operand* src0 = inst->getSrc(0);
             if (IS_HFTYPE(dst->getType()) && IS_TYPE_INT(src0->getType()))
             {
                 intHFConversion = true;
             }
             else if (IS_HFTYPE(src0->getType()) && IS_TYPE_INT(dst->getType()))
             {
                 intHFConversion = true;
             }
             // we allow pact destination for F to HF.
             if (getGenxPlatform() >= GENX_CHV && !intHFConversion && inst->isMixedMode())
             {
                 return insertMOV;
             }
         }     

         if( !VISA_WA_CHECK(builder.getPWaTable(), WaByteDstAlignRelaxedRule) )
         {
             if( splitInstListForByteDst( i, bb, (uint16_t) extypesize ) )
             {
                 return true;
             }
         }

         inst->setDest(insertMovAfter(i, dst, dst->getType(), bb));
         insertMOV = true;
    }

    return insertMOV;
}

/*
 * This function checks to see if the instruction's indirect operands
 * potentially require totally more than 8 distinct addr reg sub-registers, and
 * then determines which of the operands to spill into temporary GRFs so
 * as to limit total number of distinct sub-registers used by the instruction
 * to 8. This is a requirement imposed by the CM register allocator.
 *
 * NOTES:
 *    1. 3-src instructions do not support indirect oeprands.
 *    2. SIMD16 is not allowed when indirect operands are present.
 */

bool HWConformity::fixIndirectOpnd( INST_LIST_ITER i, G4_BB *bb )
{
    G4_INST *inst = *i;

    G4_Operand *src0 = inst->getSrc(0), *src1 = inst->getSrc(1);
    G4_DstRegRegion *dst = inst->getDst();
    bool null_dst = ( !dst || inst->hasNULLDst() );

    bool null_src0 = !src0;
    bool null_src1 = !src1 || ( inst->isMath() && src1->isNullReg() );

    const int addr_reg_max_count = 16;
    const int addr_reg_size      = G4_Type_Table[Type_UW].byteSize;
    int src_uniq_count           = 0;
    int src1_count               = 0;
    int src0_count               = 0;
    int dst_uniq_count           = 0;
    int dst_count                = 0;
    bool nospill_src1            = false;
    bool nospill_src0            = false;
    bool nospill_dst             = false;
    bool spill_src1              = false;
    bool spill_src0              = false;
    bool spill_dst               = false;
    G4_Declare *addr_dcl0 = NULL, *addr_dcl1 = NULL, *addr_dcl2 = NULL;
    if( !null_src0 && src0->isSrcRegRegion() &&
        src0->getRegAccess() != Direct && src0->asSrcRegRegion()->getBase()->isRegVar() ){
            addr_dcl0 = src0->asSrcRegRegion()->getBase()->asRegVar()->getDeclare();
            while( addr_dcl0->getAliasDeclare() != NULL ){
                addr_dcl0 = addr_dcl0->getAliasDeclare();
            }
            // is the following precise?
            src0_count = addr_dcl0->getNumElems() * addr_dcl0->getNumRows() * addr_dcl0->getElemSize() / addr_reg_size;
            MUST_BE_TRUE( src0_count <= addr_reg_max_count, "More than 8 address subregisters required for one oerand." );
            src_uniq_count += src0_count;
    }

    if( !null_src1 && src1->isSrcRegRegion() &&
        src1->getRegAccess() != Direct && src1->asSrcRegRegion()->getBase()->isRegVar() ){
            addr_dcl1 = src1->asSrcRegRegion()->getBase()->asRegVar()->getDeclare();
            while( addr_dcl1->getAliasDeclare() != NULL ){
                addr_dcl1 = addr_dcl1->getAliasDeclare();
            }
            src1_count = addr_dcl1->getNumElems() * addr_dcl1->getNumRows() * addr_dcl1->getElemSize() / addr_reg_size;
            MUST_BE_TRUE( src1_count <= addr_reg_max_count, "More than 8 address subregisters required for one oerand." );
            if (addr_dcl1 != addr_dcl0) {
                // should we use top level dcl here?
                src_uniq_count += src1_count;
            }
            else {
                nospill_src1 = true;
                nospill_src0 = true;
            }
    }

    if( !null_dst &&
        dst->getRegAccess() != Direct && dst->getBase()->isRegVar() )
    {
            addr_dcl2 = dst->getBase()->asRegVar()->getDeclare();
            while( addr_dcl2->getAliasDeclare() != NULL ){
                addr_dcl2 = addr_dcl2->getAliasDeclare();
            }
            dst_count = addr_dcl2->getNumElems() * addr_dcl2->getNumRows() * addr_dcl2->getElemSize() / addr_reg_size;
            MUST_BE_TRUE( dst_count <= addr_reg_max_count, "More than 8 address subregisters required for one oerand." );
            if (addr_dcl2 != addr_dcl0 && addr_dcl2 != addr_dcl1) {
                dst_uniq_count += dst_count;
            }
            else if( addr_dcl2 != addr_dcl0 ){
                nospill_dst = true;
                nospill_src0 = true;
            }else{
                nospill_dst = true;
                nospill_src1 = true;
            }
    }

    if (src_uniq_count > addr_reg_max_count) {
        if (src0_count > src1_count || nospill_src1) {
            MUST_BE_TRUE(nospill_src0 == false, "Address of source0 should be spilled." );
            spill_src0 = true;
            src_uniq_count -= src0_count;
        }
        else {
            MUST_BE_TRUE(nospill_src1 == false, "Address of source1 should be spilled.");
            spill_src1 = true;
            src_uniq_count -= src1_count;
        }
    }

    if (src_uniq_count + dst_uniq_count > addr_reg_max_count) {
        MUST_BE_TRUE(nospill_dst == false, "Address of dst should be spilled." );

        if (nospill_src1 && nospill_src0) {
            spill_dst = true;
            dst_uniq_count = 0;
        }
        else if (dst_uniq_count > src0_count && dst_uniq_count > src1_count) {
            spill_dst = true;
            dst_uniq_count = 0;
        }
        else if (spill_src0 ) {
            spill_src1 = true;
            src_uniq_count -= src1_count;
        }
        else if (spill_src1 ) {
            spill_src0 = true;
            src_uniq_count -= src0_count;
        }
        else if (src0_count > src1_count) {
            spill_src0 = true;
            src_uniq_count -= src0_count;
        }
        else {
            spill_src1 = true;
            src_uniq_count -= src1_count;
        }
    }

    MUST_BE_TRUE (src_uniq_count + dst_uniq_count <= addr_reg_max_count,
        "Remianed number of address registers should be no more than 8 after spill.");

    // Is this only for iselect?
    // What if a scalar with indirect addressing is used?
    if (spill_src0) {
        G4_Operand *new_src0 = insertMovBefore(i, 0, src0->getType(), bb);
        inst->setSrc( new_src0, 0 );
    }

    if (spill_src1 && src1) {
        G4_Operand *new_src1 = insertMovBefore(i, 1, src1->getType(), bb);
        inst->setSrc( new_src1, 1 );
    }

    if (spill_dst && dst)
    {
        G4_DstRegRegion *new_dst = insertMovAfter( i, dst, dst->getType(), bb );
        inst->setDest( new_dst );
        if( dst != new_dst &&
            ( IS_FTYPE(dst->getType()) || IS_DFTYPE(dst->getType()) ) )
        {
            inst->setSaturate( false );
        }
    }
    return spill_dst;
}
/*
 *     Two rules are checked here:
 * (1) When source(s) is/are of float type, destination must be of float
 *     type also. The exception is MOV instruction which can be used
 *     for explicit type conversion between float and integer.
 *
 * (2) For Gen6, only the following instructions can have
 *     interger sources and float destination:
 *     MOV, ADD, MUL, MAC, MAD, LINE
 */
bool HWConformity::fixDstType( INST_LIST_ITER i, G4_BB *bb, G4_Type extype )
{
    G4_INST *inst = *i;
    G4_DstRegRegion *dst = inst->getDst();

    if (!dst)
    {
        return false;
    }

    if( inst->hasNULLDst() ){
        if( inst->opcode() != G4_mov && IS_FTYPE( extype ) && !IS_FTYPE( dst->getType() ) ){
            // change type and stride of NULL dst
            G4_DstRegRegion *null_dst_opnd = builder.createNullDst( Type_F );
            inst->setDest( null_dst_opnd );
        }
    }
    else if( (
                (inst->opcode() != G4_mov && !inst->isSend())   &&
                (IS_FTYPE( extype ) || IS_HFTYPE(extype))       &&
                !(IS_FTYPE(dst->getType()) || IS_HFTYPE(dst->getType()))
             )                                                              ||
             (
                IS_FTYPE(dst->getType())                        &&
                //assumes checks for platform were already done for HF
                !(IS_FTYPE(extype) || IS_HFTYPE(extype))       &&
                !Opcode_int_src_float_dst_OK( inst->opcode() )
             )
          )
    {
            G4_DstRegRegion *new_dst = insertMovAfter(i, dst, extype, bb);
            // TODO: since cmp amd cmpn have no dst, we do not handle cmp/cmpn dst during MOV inst insertion.
            inst->setDest( new_dst );
            if( dst != new_dst &&
                ( IS_FTYPE(dst->getType()) || IS_DFTYPE(dst->getType()) ) )
            {
                inst->setSaturate( false );
            }
            return true;
    }
    return false;
}


// If an accumulator is an explicit source operand, its register region must match that of the 
// destination register with the exception(s) described below.
// When OWords of accumulators are accessed, the source and destination OWords may be different
bool HWConformity::fixAccSrc(INST_LIST_ITER iter, G4_BB* bb)
{
    G4_INST *inst = *iter;
    bool AccExplictUse = false;

    for (int i = 0; i < inst->getNumSrc(); ++i)
    {
        G4_Operand* src = inst->getSrc(i);
        if (src && src->isAccReg())
        {
            AccExplictUse = true;
            break;
        }
    }

    if (AccExplictUse &&
        inst->getDst() &&
        inst->getDst()->getBase() &&
        inst->getDst()->getBase()->isRegVar())
    {
        int alignByte = 16;
        if (!builder.isOpndAligned(inst->getDst(), alignByte))
        {
            G4_DstRegRegion *new_dst = insertMovAfter(iter, inst->getDst(), inst->getDst()->getType(), bb);
            G4_Declare* tmpDstDcl = new_dst->getTopDcl();
            G4_SubReg_Align subAlign = Eight_Word;
            tmpDstDcl->setSubRegAlign(subAlign);
            inst->setDest(new_dst);
            return true;
        }
    }

    return false;
}

// check for implicit acc region rules
// -- implicit acc should have same subreg offset dst (currently always 0)
// this should be done when creating the instruction with implicit acc source,
// but the code for generating mac is just way too complicated..
bool HWConformity::fixImplicitAcc(INST_LIST_ITER i, G4_BB* bb)
{
    G4_INST *inst = *i;
    if (inst->hasImplicitAccSrc())
    {
        G4_DstRegRegion* dst = inst->getDst();
        if (!builder.isOpndAligned(dst, GENX_GRF_REG_SIZ))
        {
            inst->setDest(insertMovAfter(i, dst, dst->getType(), bb, Sixteen_Word));
            return true;
        }
    }
    return false;
}

bool HWConformity::fixAccDst( INST_LIST_ITER i, G4_BB* bb )
{
    G4_INST *inst = *i;
    G4_DstRegRegion *dst = inst->getDst();
    int exec_size = inst->getExecSize( );
    bool compressed = isCompressedInst( inst );
    int uncompressed_exec_size = compressed ? exec_size / 2: exec_size;

    bool insertMov = false;
    unsigned short dst_hs = dst->getHorzStride();

    bool covers_whole_acc = ( dst_hs == 1 ) &&
        ( ( inst->getExecSize() * dst->getExecTypeSize() ) % G4_GRF_REG_NBYTES == 0 );

    bool non_null_dst = ( inst->opcode() == G4_mach && inst->getDst() && !inst->getDst()->isNullReg() );

    // Locate the only use corresponding to the ACC definition in
    // the same basic block.
    bool found_acc_use = false;
    INST_LIST_ITER iter = i;
    G4_INST *acc_use_op = NULL;
    iter++;
    for (auto useIter = inst->use_begin(); useIter != inst->use_end(); useIter++)
    {
        if( (*useIter).second == Opnd_pred || (*useIter).second == Opnd_dst )
        {
            continue;
        }
        if( (*useIter).second == Opnd_implAccSrc )
        {
            acc_use_op = (*useIter).first;
            found_acc_use = true;
            break;
        }
        G4_Operand *use = (*useIter).first->getSrc( (*useIter).second - 1 );
        if( use && use->isAccReg() )
        {
            acc_use_op = (*useIter).first;
            found_acc_use = true;
            break;
        }
    }

    MUST_BE_TRUE( found_acc_use || non_null_dst, "Defined ACC is not used in the same BB." );
    if( !found_acc_use )
    {
        return insertMov;
    }

    // get iterator of acc use inst
    while( (*iter) != acc_use_op )
    {
        iter++;
    }

    G4_DstRegRegion *acc_use_op_dst = acc_use_op->getDst();
    bool null_use_op_dst = acc_use_op->hasNULLDst();
    if( acc_use_op->opcode() == G4_mach && null_use_op_dst )
    {
        return insertMov;
    }
    // If the entire ACC, either acc0 or both acc0 and acc1, is not
    // covered by the ACC definition then we need to ensure the channel
    // corresponding to the only use's destination sub-register will
    // contain the value.

    if ( !covers_whole_acc )
    {
        bool need_replication;
        bool can_replicate;
        if( found_acc_use )
        {
            G4_Operand *src0 = inst->getSrc(0), *src1 = inst->getSrc(1), *src2 = inst->getSrc(2);

            // Decision making - start

            // If the acc_def_op has a unit exec_size while its use is not, then we
            // can and should replicate.

            if ( uncompressed_exec_size == 1 && acc_use_op->getExecSize() != 1 )
            {
                need_replication = true;
                can_replicate = true;
            }

            // If the acc def use destination has a dynamic offset then
            // it is not GenX conformant and we cannot guarantee safe
            // replication of ACC channels.

            else if ( acc_use_op_dst && acc_use_op_dst->getRegAccess() != Direct )
            {
                need_replication = true;
                can_replicate = false;
            }

            // If the destination is guaranteed to be GRF aligned then we
            // still have a chance to attempt replication if the sub-register
            // offset is not zero. If the offset is zero then we are already
            // GenX conformant and there is no need to replicate.
            else
            {
                unsigned short offset;
                bool opndGRFAligned = builder.isOpndAligned( acc_use_op_dst, offset, G4_GRF_REG_NBYTES );
                if ( !acc_use_op_dst || null_use_op_dst )
                {
                        opndGRFAligned = true;
                }
                if( opndGRFAligned )
                {
                    // If the offset of acc def use is not zero and the offset is
                    // a multiple of the acc def exec size.

                    if (offset % G4_GRF_REG_NBYTES != 0)
                    {
                        need_replication = true;
                        can_replicate = (offset % uncompressed_exec_size == 0);
                    }

                    else
                    {
                        need_replication = false;
                        can_replicate = false;
                    }
                }

                // The offset is at least guaranteed to be aligned w.r.t the
                // destination type. This implies that if the execution size
                // is one then we can always replicate.
                else if (uncompressed_exec_size == 1)
                {
                    need_replication = true;
                    can_replicate = true;
                }
                // Destination is not guaranteed to be GRF aligned and execution
                // size is not one either.

                else
                {
                    need_replication = true;
                    can_replicate = false;
                }
            }

            // Check if we can safely set the vertical stride of the
            // destination to zero.

            if (need_replication && can_replicate)
            {
                can_replicate =
                    ( (!src0 || isUnitRegionRow( src0, exec_size ) ) &&
                    (!src1 || isUnitRegionRow( src1, exec_size ) ) &&
                    (!src2 || isUnitRegionRow( src2, exec_size ) ) )    ;

                // If we replicated a mac/mach we need to match the implicit
                // ACC source region with its definition's region as
                // well. Make sure we can do that for a chain of mac/mach.

                INST_LIST_ITER def_inst_iter = i;

                while ( (*def_inst_iter)->hasImplicitAccSrc() )
                {
                    INST_LIST_ITER mac_acc_def_iter = def_inst_iter;

                    // Find the definition for the implicit ACC source which
                    // must be in the same basic block.
                    bool found = false;

                    for (auto def_iter = (*def_inst_iter)->def_begin();
                        def_iter != (*def_inst_iter)->def_end();
                        def_iter++ )
                    {
                        if( (*def_iter).second == Opnd_implAccSrc )
                        {
                            found = true;
                            break;
                        }
                    }

                    MUST_BE_TRUE( found, "Acc is not defined in the same BB." );

                    if ( ( *mac_acc_def_iter )->getExecSize() == exec_size ) {
                        break;
                    }
                    else {
                        G4_Operand *src0 = ( *mac_acc_def_iter )->getSrc(0), *src1 = ( *mac_acc_def_iter )->getSrc(1),
                            *src2 = ( *mac_acc_def_iter )->getSrc(2);
                        // Check if we can replicate this mac(h) in the chain.
                        if ( ( !src0 || isUnitRegionRow( src0, exec_size ) ) &&
                            ( !src1 || isUnitRegionRow( src1, exec_size ) ) &&
                            ( !src2 || isUnitRegionRow( src2, exec_size ) ) ){
                            def_inst_iter = mac_acc_def_iter;
                        }
                        else {
                            can_replicate = false;
                            break;
                        }
                    }
                }
            }

            // Decision making - end

            // Handle case (1) - perform replication across ACC channels.
            // all sources are SrcRegRegion now

            if (need_replication && can_replicate)
            {
                int acc_replication_factor =
                    G4_GRF_REG_NBYTES /
                    (uncompressed_exec_size * dst->getExecTypeSize() );

                exec_size *= acc_replication_factor;
                inst->setExecSize( (unsigned char) exec_size );

                if( src0 && src0->isSrcRegRegion() )
                {
                    RegionDesc *rd = builder.createRegionDesc( 0,
                        src0->asSrcRegRegion()->getRegion()->width,
                        src0->asSrcRegRegion()->getRegion()->horzStride );
                    src0->asSrcRegRegion()->setRegion( rd );
                }

                if( src1 && src1->isSrcRegRegion() )
                {
                    RegionDesc *rd = builder.createRegionDesc( 0,
                        src1->asSrcRegRegion()->getRegion()->width,
                        src1->asSrcRegRegion()->getRegion()->horzStride );
                    src1->asSrcRegRegion()->setRegion( rd );
                }

                if( src2 && src2->isSrcRegRegion() )
                {
                    RegionDesc *rd = builder.createRegionDesc( 0,
                        src2->asSrcRegRegion()->getRegion()->width,
                        src2->asSrcRegRegion()->getRegion()->horzStride );
                    src2->asSrcRegRegion()->setRegion( rd );
                }
                // If we replicated a mac/mach we to match the implicit
                // ACC source region with its definition's region as
                // well.

                INST_LIST_ITER def_inst_iter = i;

                while ( (*def_inst_iter)->hasImplicitAccSrc() )
                {
                    INST_LIST_ITER mac_acc_def_iter = def_inst_iter;

                    // Find the definition for the implicit ACC source which
                    // mus be in the same basic block.
                    bool found = false;
                    while( mac_acc_def_iter != bb->instList.begin() )
                    {
                        mac_acc_def_iter--;
                        if ( ( *mac_acc_def_iter )->getDst() &&
                            ( *mac_acc_def_iter )->getDst()->isAccReg() )
                        {
                                found = true;
                                break;
                        }
                    }

                    MUST_BE_TRUE( found, "Acc is not defined in the same BB." );

                    if ( ( *mac_acc_def_iter )->getExecSize() != exec_size)
                    {
                        ( *mac_acc_def_iter )->setExecSize( (unsigned char) exec_size );
                        G4_Operand *src0 = ( *mac_acc_def_iter )->getSrc(0), *src1 = ( *mac_acc_def_iter )->getSrc(1),
                            *src2 = ( *mac_acc_def_iter )->getSrc(2);

                        if( src0 && src0->isSrcRegRegion() )
                        {
                            RegionDesc *rd = builder.createRegionDesc( 0,
                                src0->asSrcRegRegion()->getRegion()->width,
                                src0->asSrcRegRegion()->getRegion()->horzStride );
                            src0->asSrcRegRegion()->setRegion( rd );
                        }

                        if( src1 && src1->isSrcRegRegion() )
                        {
                            RegionDesc *rd = builder.createRegionDesc( 0,
                                src1->asSrcRegRegion()->getRegion()->width,
                                src1->asSrcRegRegion()->getRegion()->horzStride );
                            src1->asSrcRegRegion()->setRegion( rd );
                        }

                        if( src2 && src2->isSrcRegRegion() )
                        {
                            RegionDesc *rd = builder.createRegionDesc( 0,
                                src2->asSrcRegRegion()->getRegion()->width,
                                src2->asSrcRegRegion()->getRegion()->horzStride );
                            src2->asSrcRegRegion()->setRegion( rd );
                        }
                    }
                    def_inst_iter = mac_acc_def_iter;
                }
            }

            // Handle case (2) - replace destination with an GRF boundary
            //                   aligned temporary.

            else if (need_replication && !can_replicate && acc_use_op_dst != NULL)
            {
                // Replace the destination of the acc use to be a temporary
                // GRF that is aligned to GRF boundary

                uint32_t inst_opt = acc_use_op->getOption();

                G4_Declare *aligned_grf_dcl = builder.createTempVar(
                    (unsigned short) (exec_size * acc_use_op_dst->getHorzStride()),
                    acc_use_op_dst->getType(),
                    Either,
                    Sixteen_Word );

                G4_DstRegRegion *aligned_grf_dst_opnd = builder.createDstRegRegion(
                    Direct,
                    aligned_grf_dcl->getRegVar(),
                    0,
                    0,
                    acc_use_op_dst->getHorzStride(),
                    acc_use_op_dst->getType());

                MUST_BE_TRUE( acc_use_op->getExecSize() == exec_size, "ACC def and use instructions have different execution size." );

                acc_use_op->setDest( aligned_grf_dst_opnd );

                // Insert a mov instruction to the original destination.
                unsigned short vs = aligned_grf_dst_opnd->getHorzStride();
                RegionDesc *rd = builder.createRegionDesc((uint16_t)exec_size, vs, 1, 0);
                G4_SrcRegRegion *mov_src_opnd = builder.createSrcRegRegion(
                        Mod_src_undef,
                        Direct,
                        aligned_grf_dcl->getRegVar(),
                        0,
                        0,
                        rd,
                        aligned_grf_dcl->getElemType());

                G4_INST *new_mov_inst = builder.createInternalInst(
                    acc_use_op->getPredicate(),
                    G4_mov,
                    acc_use_op->getCondMod(),
                    acc_use_op->getSaturate(),
                    (unsigned char) exec_size,
                    acc_use_op_dst,
                    mov_src_opnd,
                    NULL,
                    inst_opt,
                    acc_use_op->getLineNo(),
                    acc_use_op->getCISAOff(),
                    acc_use_op->getSrcFilename() );
                iter++;
                bb->instList.insert( iter, new_mov_inst );
                if( acc_use_op_dst->getType() == Type_F )
                {
                    acc_use_op->setSaturate(false);
                }
                acc_use_op->setPredicate( NULL );
                insertMov = true;
            }
        }
    }
    else
    {
        // it is possible that the def covers whole acc, but the dst of use inst is not aligned to GRF.
        // insert MOV for this case
        if( !builder.isOpndAligned( acc_use_op_dst, G4_GRF_REG_NBYTES ) )
        {
            while( *iter != acc_use_op )
            {
                iter++;
            }
            insertMov = true;
            acc_use_op->setDest( insertMovAfter( iter, acc_use_op_dst, acc_use_op_dst->getType(), bb ) );
        }
    }
    return insertMov;
}

/*
 * When operation execution size is 1, destination horizontal stride is set
 * according to rule 10.2:
 *
 * 10.1.2. If ExecSize is greater than 1, dst.HorzStride*sizeof(dst.Type) must
 *         be equal to or greater than the size of the execution data type.
 * 10.2. If ExecSize is 1, dst.HorzStride must not be 0. Note that this is
 *       relaxed from rule 10.1.2. Also note that this rule for destination
 *       horizontal stride is different from that for source as stated
 *       in rule #7.
 *
 * There are some instructions which work unpredictably if both ExecSize
 * and dst.HorzStride are 1. But they work fine if dst.HorzStride is set
 * according to rule 10.1.2. So we have to correct all such cases.
 *
 * This supposed to be the last operation before emitting final assembly code.
 */
void HWConformity::fixDstHstride( INST_LIST_ITER i, int extypesize )
{
    G4_INST *inst = *i;
    G4_DstRegRegion *dst = inst->getDst();
    int dst_elsize = G4_Type_Table[dst->getType()].byteSize;

    if (dst)
    {
        unsigned short hs = dst->getHorzStride();
        if( hs * dst_elsize < extypesize )
        {
            dst->setHorzStride( (unsigned short) (extypesize/dst_elsize) );
        }
    }
}

template<class T>
bool isPreAssignedRegOffsetNonZero(T* region)
{
    // T is non-NULL and either
    // G4_SrcRegRegion or G4_DstRegRegion
    bool ret = false;

    if ((region->isSrcRegRegion() || region->isDstRegRegion()) &&
        region->getBase() &&
        region->getBase()->isRegVar() &&
        region->getBase()->asRegVar()->isPhyRegAssigned() &&
        region->getBase()->asRegVar()->getPhyRegOff() != 0)
    {
        ret = true;
    }

    return ret;
}

void HWConformity::generateMacl(INST_LIST_ITER it, G4_BB* bb)
{
    G4_INST* mulInst = *it;
    MUST_BE_TRUE(mulInst->opcode() == G4_mul, "expect mul instruction");
    if (mulInst->getExecSize() == 16)
    {
        auto startIter = it; 
        bool isFirstInst = startIter == bb->instList.begin();
        if (!isFirstInst)
        {
            --startIter;
        }
        evenlySplitInst(it, bb);
        if (!isFirstInst)
        {
            ++startIter;
        }
        // startIter now points to first mul created by split
        auto endIter = it; 
        ++endIter;  
        // endIter points to the first inst after the original mul
        for (auto iter = startIter; iter != endIter;)
        {
            auto nextIter = iter;
            ++nextIter;
            G4_INST* currInst = *iter;
            if (currInst->opcode() == G4_mul)
            {
              doGenerateMacl(iter, bb);
            }
            iter = nextIter;
        }
    }
    else
    {
        doGenerateMacl(it, bb);
    }
}

// convert vISA mul (8) dst src0 src1 into
// mul (8) acc0.0<1>:d src0:d src1:w
// mach (8) dst:d src0:d src1:d
//
void HWConformity::doGenerateMacl(INST_LIST_ITER it, G4_BB *bb)
{
    G4_INST* mulInst = *it;
    MUST_BE_TRUE(mulInst->opcode() == G4_mul, "expect mul instruction");
    assert(mulInst->getExecSize() <= 8 && "expect simd8 or less inst");

    G4_Operand* src0 = mulInst->getSrc(0);
    G4_Operand* src1 = mulInst->getSrc(1);
    MUST_BE_TRUE(IS_DTYPE(src0->getType()) && IS_DTYPE(src1->getType()), "both sources must have dword type");

    if (src1->isSrcRegRegion())
    {
        G4_SrcRegRegion* src1Region = src1->asSrcRegRegion();
        if (src1Region->getModifier() != Mod_src_undef)
        {
            // need extra move for the modifier
            src1 = insertMovBefore(it, 1, src1->getType(), bb);
            mulInst->setSrc(src1, 1);
        }
    }

    // sat cannot be used at all in the macro sequence
    // this effectivly means sat is broken for mul D D D
    mulInst->setSaturate(false);

    G4_DstRegRegion* origDst = mulInst->getDst();
    G4_Type accType = (IS_UNSIGNED_INT(src0->getType()) && IS_UNSIGNED_INT(src1->getType())) ? Type_UD : Type_D;
    G4_DstRegRegion *accDstOpnd = builder.createDstRegRegion(Direct, builder.phyregpool.getAcc0Reg(), 0, 0, 1, accType);
    mulInst->setDest(accDstOpnd);

    uint32_t origOptions = mulInst->getOption();
    fixMulSrc1(it, bb);
    mulInst->setOptionOn(InstOpt_WriteEnable);

    G4_Predicate* predicate = mulInst->getPredicate();
    if (predicate != nullptr)
    {
        // move pred to mach
        mulInst->setPredicate(nullptr);
    }
    if (mulInst->getCondMod() != nullptr)
    {
        // conditional modifier cannot be used
        // when the MUL source operand is of dword type.
        MUST_BE_TRUE(false, "Dw multiply does not support conditional modifiers");
        mulInst->setCondMod(nullptr);
    }

    // create a mach inst
    G4_INST* machInst = builder.createInternalInst(predicate, G4_mach, nullptr, false, mulInst->getExecSize(),
        origDst, builder.duplicateOperand(src0), builder.duplicateOperand(src1), origOptions,
        mulInst->getLineNo(), mulInst->getCISAOff(), mulInst->getSrcFilename());

    // maintain du chain as fixAccDst uses it later
    G4_SrcRegRegion *accSrcOpnd = builder.createSrcRegRegion(Mod_src_undef, Direct,
        builder.phyregpool.getAcc0Reg(), 0, 0, builder.getRegionStride1(), accType);
    machInst->setImplAccSrc(accSrcOpnd);
    mulInst->addDefUse(machInst, Opnd_implAccSrc);

    INST_LIST_ITER machIter = it;
    machIter = bb->instList.insert(++machIter, machInst);

    if (!IS_DTYPE(origDst->getType()) || origDst->getHorzStride() != 1 ||
        !builder.isOpndAligned(origDst, 32))
    {
        // mach dst must be grf-aligned, packed D/UD as it is also used for the implicit acc source's region
        G4_DstRegRegion* tmpDst = insertMovAfter(machIter, origDst, accType, bb);
        machInst->setDest(tmpDst);
    }

}

// If both source operands of an MUL instruction are of dword integer type,
// only the lower 16 bits of data elements in src0 are used.
// The full precision multiplication results can be only produced together
// with the mach and mov instructions.

bool HWConformity::fixMULInst( INST_LIST_ITER &i, G4_BB *bb )
{
    bool insertedInst = false;
    G4_INST *inst = *i;
    G4_DstRegRegion *dst = inst->getDst();
    uint8_t exec_size = inst->getExecSize();
    bool srcExchanged = false;

    if (dst && dst->isAccReg())
    {
        return false;
    }

    uint32_t inst_opt = inst->getOption();
    G4_Operand *src0 = inst->getSrc(0), *src1 = inst->getSrc(1);

    // MUL is commutative and only
    // allows src1 to be a constant.
    // If src1 is a constant and src1
    // is not, they are swapped here.
    // If both are constants, they
    // will be fixed in checking HW conformity.
    // this is fixed in fixOpnd.

    if (src0->isImm() && !src1->isImm())
    {
        inst->setSrc( src1, 0 );
        inst->setSrc( src0, 1 );
        srcExchanged = true;
    }

    src0 = inst->getSrc(0);
    src1 = inst->getSrc(1);
    // Q dst needs 64-bit support regardless of src type
    bool isDMul = IS_QTYPE(dst->getType()) || (IS_DTYPE(src0->getType()) && IS_DTYPE(src1->getType()));

    if (!isDMul)
    {
        return false;
    }

    if (builder.hasMacl() && !IS_QTYPE(dst->getType()) &&
        (builder.no64bitType() || inst->getExecSize() > 1))
    {
        // use macl for D = D x D. We use macl when possible 
        // except on scalar inst on platforms that support native DMul
        generateMacl(i, bb);
        return true;
    }

    bool doNativeMul = false;
    if (!builder.no64bitRegioning())
    {
        // platform natively supports DW-DW multiply, no need to generate mul/mach/mov sequence
        doNativeMul = true;
    }
    else
    {
        if ((getGenxPlatform() == GENX_CHV || getGenxPlatform() == GENX_BXT))
        {
            if (inst->getExecSize() == 1)
            {
                // scalar insts are a-ok
                return false;
            }
            // ok if source is scalar or qword-aligned
            doNativeMul = (getTypeSize(dst->getType()) * dst->getHorzStride() == 8);
            auto isQWordStride = [inst, this](G4_SrcRegRegion* src)
            {
                RegionDesc* region = src->getRegion();
                if (!region->isScalar())
                {
                    uint16_t stride = 0;
                    (void) region->isSingleNonUnitStride(inst->getExecSize(), stride);
                    if (stride != 2)
                    {
                        return false;
                    }
                    // check that source is GRF-aligned to ensure that every element is qword-aligned
                    return builder.isOpndAligned(src, 32);
                }
                return true;
            };
            if (doNativeMul && src0->isSrcRegRegion())
            {
                doNativeMul = isQWordStride(src0->asSrcRegRegion());
            }
            if (doNativeMul && src1->isSrcRegRegion())
            {
                doNativeMul = isQWordStride(src1->asSrcRegRegion());
            }
        }
    }

    if (doNativeMul)
    {
        // promote source to D type if necessary
        if (IS_QTYPE(dst->getType()))
        {
            G4_Type newTy;
            G4_Operand* newOpnd;
            if (!IS_DTYPE(src0->getType()))
            {
                newTy = IS_SIGNED_INT(src0->getType()) ? Type_D : Type_UD;
                newOpnd = insertMovBefore(i, 0, newTy, bb);
                inst->setSrc(newOpnd, 0);
                insertedInst = true;
            }

            if (!IS_DTYPE(src1->getType()))
            {
                newTy = IS_SIGNED_INT(src1->getType()) ? Type_D : Type_UD;
                if (src1->isImm())
                {
                    newOpnd = builder.createImm(src1->asImm()->getImm(), newTy);
                }
                else
                {
                    newOpnd = insertMovBefore(i, 1, newTy, bb);
                }
                inst->setSrc(newOpnd, 1);
                insertedInst = true;
            }
        }
        return insertedInst;
    }

    // both sources are dword, replace with mul/mach/mov sequence
    // At this point, src0 and src1 are both DW, so we simply make
    // acc's type (i.e. dst_type) be DW/UD

    G4_CondMod *condmod = (G4_CondMod *)builder.duplicateOperand(inst->getCondMod());
    G4_Predicate *pred = (G4_Predicate *)builder.duplicateOperand(inst->getPredicate());

    // check if the following inst is mulh and uses the same srcs as this mul.
    // if true, translate them into
    // mul acc src0 src1
    // mach dst_mulh src0 src1
    // mov mul_dst src0 src1
    INST_LIST_ITER next_i = i;
    next_i++;
    G4_Type tmp_type = (IS_UNSIGNED_INT(src0->getType()) && IS_UNSIGNED_INT(src1->getType())) ? Type_UD : Type_D;
    bool isCompressed = isCompressedInst(inst);

    if (src1->isSrcRegRegion())
    {
        G4_SrcRegRegion* src1Region = src1->asSrcRegRegion();
        if (src1Region->getModifier() != Mod_src_undef)
        {
            // need extra move for the modifier
            src1 = insertMovBefore(i, 1, src1->getType(), bb);
            inst->setSrc(src1, 1);
        }
    }

    bool sat_mod = inst->getSaturate();
    inst->setSaturate(false);

    // see if we can combine this mul with a mulh following it
    if (next_i != bb->instList.end())
    {
        G4_INST *next_inst = *next_i;

        if (next_inst->opcode() == G4_mulh &&
            next_inst->getExecSize() == exec_size &&
            inst->getPredicate() == next_inst->getPredicate() &&
            ((srcExchanged &&
            src0->getType() == next_inst->getSrc(1)->getType() &&
            src0->compareOperand(next_inst->getSrc(1)) == Rel_eq &&
            src1->getType() == next_inst->getSrc(0)->getType() &&
            src1->compareOperand(next_inst->getSrc(0)) == Rel_eq) ||
            (!srcExchanged &&
            src0->getType() == next_inst->getSrc(0)->getType() &&
            src0->compareOperand(next_inst->getSrc(0)) == Rel_eq  &&
            src1->getType() == next_inst->getSrc(1)->getType() &&
            src1->compareOperand(next_inst->getSrc(1)) == Rel_eq)))
        {
            // change current mul inst
            G4_DstRegRegion *acc_dst_opnd = builder.createDstRegRegion(
                Direct,
                builder.phyregpool.getAcc0Reg(),
                0,
                0,
                1,
                tmp_type);

            inst->setDest(acc_dst_opnd);

            fixMulSrc1(i, bb);

            inst->transferUse(next_inst, true);
            inst->addDefUse(next_inst, Opnd_implAccSrc);
            // change mulh inst
            next_inst->setOpcode(G4_mach);

            G4_DstRegRegion *next_dst = next_inst->getDst();
            if (next_dst != NULL &&
                (next_inst->getSaturate() ||
                next_dst->getByteOffset() % GENX_GRF_REG_SIZ != 0 ||
                (bb->isInSimdFlow() && next_inst->isWriteEnableInst() == false) ||
                (next_dst &&
                ((next_dst->getExecTypeSize() > G4_Type_Table[Type_D].byteSize) ||
                isPreAssignedRegOffsetNonZero<G4_DstRegRegion>(next_dst)))))
            {
                // add a tmp mov
                G4_DstRegRegion *new_next_dst = insertMovAfter(next_i, next_dst, next_dst->getType(), bb);
                next_inst->setDest(new_next_dst);
            }

            // set implicit source/dst for MACH
            RegionDesc *rd = NULL;
            unsigned short vs = 0, wd = exec_size, hs = 0;
            if (exec_size > 1){
                if (exec_size == 16){
                    wd = wd / 2;
                }
                hs = 1;
                vs = wd;
            }
            rd = builder.createRegionDesc(vs, wd, hs);
            G4_SrcRegRegion *acc_src_opnd = builder.createSrcRegRegion(Mod_src_undef, Direct, builder.phyregpool.getAcc0Reg(), 0, 0, rd, tmp_type);
            next_inst->setImplAccSrc(acc_src_opnd);
            next_inst->setImplAccDst(builder.createDstRegRegion(*acc_dst_opnd));

            // create mov inst
            G4_SrcRegRegion* movAccSrc = builder.createSrcRegRegion(Mod_src_undef, Direct, builder.phyregpool.getAcc0Reg(), 0, 0, rd, tmp_type);
            G4_INST* newMov = builder.createInternalInst(pred, G4_mov, condmod, false, exec_size, dst, movAccSrc, NULL, inst_opt,
                inst->getLineNo(), inst->getCISAOff(), inst->getSrcFilename());

            INST_LIST_ITER iter = next_i;
            iter++;
            bb->instList.insert(iter, newMov);

            next_inst->addDefUse(newMov, Opnd_src0);

            INST_LIST_ITER last_iter = iter;
            last_iter--;

            if (dst != NULL &&
                (sat_mod ||
                (dst &&
                ((dst->getExecTypeSize() > G4_Type_Table[Type_D].byteSize) ||
                (isPreAssignedRegOffsetNonZero<G4_DstRegRegion>(dst))))))
            {
                // add a tmp mov
                iter--;
                G4_DstRegRegion *new_next_dst = insertMovAfter(iter, dst, dst->getType(), bb);
                newMov->setDest(new_next_dst);
                if (new_next_dst != dst && sat_mod)
                {
                    MUST_BE_TRUE(iter != bb->instList.end() && (*iter)->opcode() == G4_mov,
                        "Next instruciton should be the MOV generated for consistent Dst and ACC source region.");
                    (*iter)->setSaturate(false);
                }
            }

            next_inst->setOptionOn(InstOpt_AccWrCtrl);

            if (exec_size == 16)
            {
                splitDWMULInst(i, last_iter, bb);
            }
            return true;
        }
    }

    G4_DstRegRegion *acc_dst_opnd = builder.createDstRegRegion(Direct, builder.phyregpool.getAcc0Reg(), 0, 0, 1, tmp_type);
    inst->setDest(acc_dst_opnd);
    fixMulSrc1(i, bb);

    if (bb->isInSimdFlow())
    {
        inst->setOptions((inst->getOption() & ~InstOpt_Masks) | InstOpt_WriteEnable);
    }

    if (pred != NULL) {
        // conditional modifier cannot be used
        // when the MUL source operand is of dword type.
        inst->setCondMod(NULL);
    }

    // Dst is either null, or a temp D if the original dst is Q/UQ
    G4_DstRegRegion *machDst = NULL;
    G4_Declare* high32BitDcl = NULL;
    if (IS_QTYPE(dst->getType()))
    {
        high32BitDcl = builder.createTempVar(exec_size, Type_D, Either, Any);
        machDst = builder.Create_Dst_Opnd_From_Dcl(high32BitDcl, 1);
    }
    else
    {
        machDst = builder.createNullDst(Type_D);
    }

    // create a mach inst
    G4_INST* newInst = builder.createInternalInst(
        NULL,
        G4_mach,
        NULL,
        false,
        exec_size,
        machDst,
        builder.duplicateOperand(src0),
        builder.duplicateOperand(src1),
        inst_opt,
        inst->getLineNo(),
        inst->getCISAOff(),
        inst->getSrcFilename());

    newInst->setOptionOn(InstOpt_AccWrCtrl);

    INST_LIST_ITER iter = i;
    iter++;
    bb->instList.insert(iter, newInst);

    inst->setPredicate(NULL);

    inst->copyDef(newInst, Opnd_src0, Opnd_src0);
    inst->copyDef(newInst, Opnd_src1, Opnd_src1);
    inst->transferUse(newInst);
    inst->addDefUse(newInst, Opnd_implAccSrc);

    // create an implicit source for MACH
    RegionDesc *rd = NULL;
    unsigned short vs = 0, wd = exec_size, hs = 0;
    if (exec_size > 1){
        if (isCompressed){
            wd = wd / 2;
        }
        hs = 1;
        vs = wd;
    }
    rd = builder.createRegionDesc(vs, wd, hs);
    G4_SrcRegRegion *acc_src_opnd = builder.createSrcRegRegion(Mod_src_undef, Direct, 
        builder.phyregpool.getAcc0Reg(), 0, 0, rd, tmp_type);

    newInst->setImplAccSrc(acc_src_opnd);

    // set an implicit dst for MACH
    newInst->setImplAccDst(builder.createDstRegRegion(*acc_dst_opnd));

    insertedInst = true;

    if (IS_QTYPE(dst->getType()))
    {
        // we have to produce two additional moves to form the Q/UQ:
        // mul (8) acc0:d r2.0<8;8,1>:d r3.0<16;8,2>:uw
        // mach (8) r5.0<1>:d r2.0<8;8,1>:d r3.0<8;8,1>:d
        // mov (8) r6.0<1>:d acc0:d  // Low 32 bits.
        // mov (8) dst.0<2>:d r6.0<1>:d
        // mov (8) dst.1<2>:d r5.0<1>:d
        // Note that we don't try to combine the moves because of the HW restriction that
        // "If an accumulator is an explicit source operand, its register region must match that of the destination register"

        G4_Declare* low32BitDcl = builder.createTempVar(exec_size, Type_D, Either, Any);
        G4_INST* movInst = builder.createInternalInst(NULL, G4_mov, NULL, false, exec_size,
            builder.Create_Dst_Opnd_From_Dcl(low32BitDcl, 1),
            builder.createSrcRegRegion(*acc_src_opnd), NULL, inst_opt,
            inst->getLineNo(), inst->getCISAOff(), inst->getSrcFilename());
        bb->instList.insert(iter, movInst);

        G4_DstRegRegion* origDst = dst;
        bool needsExtraMov = origDst->getHorzStride() > 1 || condmod != NULL || sat_mod;

        G4_Declare* dstAlias = builder.createTempVar(exec_size * 2, Type_D, Either, Any);
        if (!needsExtraMov)
        {
            uint32_t aliasOffset = origDst->getRegOff() * GENX_GRF_REG_SIZ + origDst->getSubRegOff() * 8;
            dstAlias->setAliasDeclare(origDst->getBase()->asRegVar()->getDeclare(), aliasOffset);
        }
        G4_INST* lowMove = builder.createInternalInst(pred, G4_mov, NULL, false, exec_size,
            builder.Create_Dst_Opnd_From_Dcl(dstAlias, 2),
            builder.Create_Src_Opnd_From_Dcl(low32BitDcl, builder.getRegionStride1()),
            NULL, inst_opt);
        bb->instList.insert(iter, lowMove);

        MUST_BE_TRUE(high32BitDcl != NULL, "mach dst must not be null");
        G4_INST* highMove = builder.createInternalInst(pred, G4_mov, NULL, false, exec_size,
            builder.createDstRegRegion(Direct, dstAlias->getRegVar(), 0, 1, 2, dstAlias->getElemType()),
            builder.Create_Src_Opnd_From_Dcl(high32BitDcl, builder.getRegionStride1()),
            NULL, inst_opt);
        bb->instList.insert(iter, highMove);

        if (needsExtraMov)
        {
            // this will take care of non-packed dst/cond mod/saturate
            G4_Declare* dstAliasAsQ = builder.createTempVar(exec_size, Type_Q, Either, Any);
            dstAliasAsQ->setAliasDeclare(dstAlias, 0);
            G4_INST* moveInst = builder.createInternalInst(NULL, G4_mov, condmod, sat_mod, exec_size,
                dst,
                builder.Create_Src_Opnd_From_Dcl(dstAliasAsQ, builder.getRegionStride1()),
                NULL, inst_opt);
            bb->instList.insert(iter, moveInst);
        }

        return true;
    }

    INST_LIST_ITER last_iter;
    // create a mov inst
    if (sat_mod == false)
    {
        bool extra_mov = dst &&
            dst->getExecTypeSize() > G4_Type_Table[Type_D].byteSize;
        extra_mov |= (isPreAssignedRegOffsetNonZero<G4_DstRegRegion>(dst));

        G4_INST* movInst = builder.createInternalInst(pred, G4_mov, condmod, false, exec_size, dst,
            builder.createSrcRegRegion(*acc_src_opnd), NULL, inst_opt,
            inst->getLineNo(), inst->getCISAOff(), inst->getSrcFilename());

        newInst->transferUse(movInst);
        newInst->addDefUse(movInst, Opnd_src0);

        bb->instList.insert(iter, movInst);
        last_iter = iter;
        last_iter--;
        if (extra_mov)
        {
            // add a tmp mov
            iter--;
            G4_DstRegRegion *new_next_dst = insertMovAfter(iter, dst, dst->getType(), bb);
            movInst->setDest(new_next_dst);
            movInst->setPredicate(NULL);
        }
    }
    else
    {
        // create an extra mov inst
        G4_Declare *dcl = builder.createTempVar(
            exec_size,
            tmp_type,
            Either,
            Sixteen_Word);

        G4_DstRegRegion *tmp_dst_opnd = builder.createDstRegRegion(
            Direct,
            dcl->getRegVar(),
            0,
            0,
            1,
            tmp_type);
        G4_INST* movInst = builder.createInternalInst(NULL, G4_mov, condmod, false, exec_size, tmp_dst_opnd,
            builder.createSrcRegRegion(*acc_src_opnd), NULL, InstOpt_NoOpt,
            inst->getLineNo(), inst->getCISAOff(), inst->getSrcFilename());

        bb->instList.insert(iter, movInst);

        last_iter = iter;
        last_iter--;

        G4_SrcRegRegion *tmp_src_opnd = builder.createSrcRegRegion(Mod_src_undef, Direct, dcl->getRegVar(), 0, 0, rd, tmp_type);

        G4_INST *newInst2 = builder.createInternalInst(pred, G4_mov, condmod, sat_mod, exec_size, dst, tmp_src_opnd, NULL, inst_opt,
            inst->getLineNo(), inst->getCISAOff(), inst->getSrcFilename());

        newInst->transferUse(newInst2);
        newInst->addDefUse(movInst, Opnd_src0);
        movInst->addDefUse(newInst2, Opnd_src0);
        bb->instList.insert(iter, newInst2);
        iter++;
    }

    if (exec_size == 16)
    {
        splitDWMULInst(i, last_iter, bb);
    }

    return insertedInst;
}


// Translate MULH into
// MUL acc src0 src1
// MACH dst src0 src1
void HWConformity::fixMULHInst( INST_LIST_ITER &i, G4_BB *bb )
{
    G4_INST *inst = *i;
    INST_LIST_ITER iter = i;
    uint8_t exec_size = inst->getExecSize( );

    int inst_opt = inst->getOption();

    G4_Operand *src0 = inst->getSrc(0), *src1 = inst->getSrc(1);

    if ( src0->isImm() && !src1->isImm() ){
        inst->setSrc( src1, 0 );
        inst->setSrc( src0, 1 );

        src0 = inst->getSrc(0);
        src1 = inst->getSrc(1);
    }

    unsigned int instExecSize = inst->getExecSize();
    if (instExecSize <= 8 && !builder.no64bitRegioning())
    {
        // use mul Q D D to get the upper 32-bit
        // not that we don't do this for CHV/BXT due to the 64-bit type restrictions
        inst->setOpcode(G4_mul);
        G4_DstRegRegion *dst = inst->getDst();
        G4_Type dstType = dst->getType();

        if (dstType == Type_UD)
            dstType = Type_UQ;
        else
            dstType = Type_Q;
        G4_Declare *dstDcl = dst->getBase()->asRegVar()->getDeclare();
        G4_Declare *tmpDcl = builder.createTempVar(
                                        dstDcl->getNumElems(),
                                        dstType,
                                        dstDcl->getAlign(),
                                        dstDcl->getSubRegAlign(),
                                        "TV");

        G4_DstRegRegion* tmpDst = builder.Create_Dst_Opnd_From_Dcl(tmpDcl, 1);
        inst->setDest(tmpDst);

        //need move to cast back to D/UD type
        G4_SrcRegRegion *tmpSrc = builder.createSrcRegRegion(
            Mod_src_undef,
            Direct,
            tmpDcl->getRegVar(),
            0,
            1,
            instExecSize > 1 ? builder.getRegionStride2() : builder.getRegionScalar(),
            dst->getType());

        ++iter;

        G4_INST *tmpMov = builder.createInternalInst(
                builder.duplicateOperand(inst->getPredicate()),
                G4_mov,
                NULL,
                false,
                (unsigned char) instExecSize,
                dst,
                tmpSrc,
                NULL,
                NULL,
                inst->getOption(),
                inst->getLineNo(),
                inst->getCISAOff(),
                inst->getSrcFilename());

        bb->instList.insert(iter, tmpMov);
        //it will decrement back to mov
        i = iter;

        /*
            Need to remove dst from uses list of mulh, and add them to movInst useList
            add movInst to uselist of mulh.
            Add mulh to def instruction list of movInst
        */
        inst->transferUse(tmpMov);
        inst->addDefUse(tmpMov, Opnd_src0);
        return;
    }

    if(src1->isSrcRegRegion() && src1->asSrcRegRegion()->getModifier() != Mod_src_undef)
    {
        // WaAdditionalMovWhenSrc1ModOnMulMach
        G4_Declare *src1Dcl = src1->asSrcRegRegion()->getBase()->asRegVar()->getDeclare();
        G4_Declare *tmpDcl = builder.createTempVar(
                                        src1Dcl->getNumElems(),
                                        src1Dcl->getElemType(),
                                        src1Dcl->getAlign(),
                                        src1Dcl->getSubRegAlign(),
                                        "TV");

        G4_DstRegRegion* tmpDst = builder.createDstRegRegion(
                                                Direct,
                                                tmpDcl->getRegVar(),
                                                0,
                                                0,
                                                1,
                                                src1->asSrcRegRegion()->getType());

        RegionDesc * src1Desc = src1->asSrcRegRegion()->getRegion();

        G4_INST *tmpMov = builder.createInternalInst(
                                            NULL,
                                            G4_mov,
                                            NULL,
                                            false,
                                            (uint8_t) src1Desc->width,
                                            tmpDst,
                                            src1,
                                            NULL,
                                            NULL,
                                            InstOpt_WriteEnable,
                                            inst->getLineNo(),
                                            inst->getCISAOff(),
                                            inst->getSrcFilename());
        bb->instList.insert(iter, tmpMov);

        RegionDesc *tmpSrcDesc = NULL;

        if(src1Desc->width == 1)
            tmpSrcDesc = builder.getRegionScalar();
        else
            tmpSrcDesc = builder.createRegionDesc( src1Desc->vertStride, src1Desc->width, src1Desc->horzStride );

        G4_SrcRegRegion *srcTmp = builder.createSrcRegRegion(Mod_src_undef, Direct, tmpDcl->getRegVar(), 0, 0, tmpSrcDesc, src1->asSrcRegRegion()->getType());
        src1 = srcTmp;
        inst->setSrc(src1, 1);

        //Remove def instruction(s) from mulh
        //add them to the tmpMov
        //remove mulh from dev instructions, add tmpMov to them
        inst->transferDef(tmpMov, Opnd_src1, Opnd_src0);
        tmpMov->addDefUse(inst, Opnd_src1);
    }

    G4_Type tmp_type = ( IS_UNSIGNED_INT(src0->getType()) && IS_UNSIGNED_INT(src1->getType()) ) ? Type_UD : Type_D;

    assert(IS_DTYPE(src0->getType()) && "src0 must be DW type");

    G4_DstRegRegion* acc_dst_opnd = builder.createDstRegRegion(
        Direct,
        builder.phyregpool.getAcc0Reg(),
        0,
        0,
        1,
        tmp_type);
    G4_INST* newMul = builder.createInternalInst(nullptr, G4_mul, NULL, false, exec_size,
        acc_dst_opnd, builder.duplicateOperand(src0), builder.duplicateOperand(src1), inst_opt,
        inst->getLineNo(), inst->getCISAOff(), inst->getSrcFilename());

    bb->instList.insert(iter, newMul);
    inst->copyDefsTo(newMul, false);
    newMul->addDefUse(inst, Opnd_implAccSrc);

    iter = i;
    iter--;
    fixMulSrc1(iter, bb);

    if( bb->isInSimdFlow() )
    {
        newMul->setOptions( ( inst_opt & ~InstOpt_Masks ) | InstOpt_WriteEnable );
    }
    inst->setOpcode( G4_mach );

    if (src1->isImm() && src0->getType() != src1->getType()) {
        G4_Imm *oldImm = src1->asImm();
        // Ensure src1 has the same type as src0.
        G4_Imm *newImm = builder.createImm(oldImm->getInt(), src0->getType());
        inst->setSrc(newImm, 1);
    }

    //set implicit src/dst for mach
    RegionDesc *rd = NULL;
    unsigned short vs = 0, wd = exec_size, hs = 0;
    if( exec_size > 1 ){
        if( exec_size == 16 ){
            wd = wd/2;
        }
        hs = 1;
        vs = wd;
    }
    rd = builder.createRegionDesc( vs, wd, hs );
    G4_SrcRegRegion *acc_src_opnd = builder.createSrcRegRegion(Mod_src_undef, Direct, builder.phyregpool.getAcc0Reg(), 0, 0, rd, tmp_type);
    inst->setImplAccSrc( acc_src_opnd );
    inst->setImplAccDst( builder.createDstRegRegion( *acc_dst_opnd ) );

    INST_LIST_ITER end_iter = i;
    // check if the ACC source is aligned to mach dst
    G4_DstRegRegion *dst = inst->getDst();
    if ((inst->getSaturate()) ||
        (dst &&
        ((dst->getExecTypeSize() > G4_Type_Table[Type_D].byteSize) ||
        (isPreAssignedRegOffsetNonZero<G4_DstRegRegion>(dst)))))
    {
        // add a tmp mov
        inst->setDest( insertMovAfter( i, dst, dst->getType(), bb ) );
        end_iter++;
    }

    inst->setOptionOn(InstOpt_AccWrCtrl);

    if (exec_size == 16)
    {
        INST_LIST_ITER start_iter = i;
        start_iter--;
        splitDWMULInst( start_iter, end_iter, bb );
        i = end_iter;
    }
}

//
// insert move instructions to copy numDwords dwords from src to dst at the specified location
// a NoMask UD move is used.
// dst and src must be dword-aligned.
// srcOffset and dstOffset are in bytes
// numDwords must be one of {1,2,4,8,16}
// ToDo: may want to generalize this into a copyBytes function that selects the appropriate move type
// based on dst and src type
//
void HWConformity::copyDwords(G4_Declare* dst,
                              int dstOffset,
                              G4_Declare* src,
                              int srcOffset,
                              int numDwords,
                              G4_BB* bb,
                              INST_LIST_ITER iter)
{

    MUST_BE_TRUE(numDwords == 1 || numDwords == 2 || numDwords == 4 ||
        numDwords == 8 || numDwords == 16, "invalid number of dwords to copy");

    G4_Declare* newDst = dst;

    if (dst->getElemType() != Type_UD)
    {
        // create an alias with type UD
        newDst = builder.createTempVar(numDwords, Type_UD, Either, Any);
        newDst->setAliasDeclare(dst, 0);
    }

    G4_Declare* newSrc = src;
    if (src->getElemType() != Type_UD)
    {
        // create an alias with type UD
        newSrc = builder.createTempVar(numDwords, Type_UD, Either, Any);
        newSrc->setAliasDeclare(src, 0);
    }

    G4_SrcRegRegion* srcOpnd = builder.createSrcRegRegion(Mod_src_undef, Direct,
        newSrc->getRegVar(), srcOffset / GENX_GRF_REG_SIZ,
        (srcOffset % GENX_GRF_REG_SIZ) / G4_Type_Table[Type_UD].byteSize,
        builder.getRegionStride1(), Type_UD);
    G4_DstRegRegion* dstOpnd = builder.createDstRegRegion(Direct, newDst->getRegVar(),
        dstOffset / GENX_GRF_REG_SIZ,
        (dstOffset % GENX_GRF_REG_SIZ) / G4_Type_Table[Type_UD].byteSize, 1, Type_UD);

    G4_INST* movInst = builder.createInternalInst(NULL, G4_mov, NULL, false, (uint8_t) numDwords, dstOpnd, srcOpnd,
        NULL, InstOpt_WriteEnable);

    INST_LIST_ITER movPos = bb->instList.insert(iter, movInst);

    if (numDwords == 16 &&
        ((dstOffset % GENX_GRF_REG_SIZ) != 0 || (srcOffset % GENX_GRF_REG_SIZ) != 0))
    {
        // move crosses 2 GRF boundary, needs splitting
        evenlySplitInst(movPos, bb);
    }
}

// like the above, but source is an indirect 64-bit source and dst offset is always 0
// If source is Indirect 1x1, we generate
//  mov (esize*2) tmp<1>:ud r[A0]<1;1,0>:ud
//  ...     tmpSrc<region>:q
// If source is VxH indirect, we have to generate instead
//  mov (esize*2) tmp<1>:ud r[A0]<2,1>:ud
//  ...     tmpSrc<1;1,0>:q
// as we can't have the indirect region on the 64-bit type operand
// A0 is not changed otherwise
void HWConformity::copyDwordsIndirect(G4_Declare* dst,
    G4_SrcRegRegion* src,
    int numDwords,
    G4_BB* bb,
    INST_LIST_ITER iter)
{
    MUST_BE_TRUE(G4_Type_Table[dst->getElemType()].byteSize >= 4 &&
        G4_Type_Table[src->getType()].byteSize >= 4, "dst and src must have dword or qword type");

    MUST_BE_TRUE(numDwords == 1 || numDwords == 2 || numDwords == 4 ||
        numDwords == 8 || numDwords == 16, "invalid number of dwords to copy");

    MUST_BE_TRUE(src->getRegAccess() == IndirGRF, "source must be indirect GRF");

    G4_Declare* newDst = dst;

    if (dst->getElemType() != Type_UD)
    {
        // create an alias with type UD
        newDst = builder.createTempVar(numDwords, Type_UD, Either, Any);
        newDst->setAliasDeclare(dst, 0);
    }

    G4_SrcRegRegion* newSrc = builder.duplicateOperand(src);
    MUST_BE_TRUE(G4_Type_Table[newSrc->getType()].byteSize == 8, "only support 64-bit type source so far");
    newSrc->setType(Type_UD);
    if (newSrc->getRegion()->isRegionWH())
    {
        MUST_BE_TRUE(newSrc->getRegion()->width == 1, "only handle <1,0> region for now");
        newSrc->setRegion(builder.createRegionDesc(UNDEFINED_SHORT, 2, 1));
    }
    else
    {
        newSrc->setRegion(builder.getRegionStride1());
    }

    G4_DstRegRegion* dstOpnd = builder.createDstRegRegion(Direct, newDst->getRegVar(), 0, 0, 1, Type_UD);

    G4_INST* movInst = builder.createInternalInst(NULL, G4_mov, NULL, false, (uint8_t)numDwords, dstOpnd, newSrc,
        NULL, InstOpt_WriteEnable);

    bb->instList.insert(iter, movInst);
}

// copy numRegs GRFs from src[srcOffset] to dst[dstOffset]
// dst[dstOffset] and src[srcOffset] are both GRF-aligned
void HWConformity::copyRegs(G4_Declare* dst,
    int dstOffset,
    G4_Declare* src,
    int srcOffset,
    int numRegs,
    G4_BB* bb,
    INST_LIST_ITER iter)
{
    int numByteCopied = 0;
    for (; numRegs >= 2; numRegs -= 2, numByteCopied += 64)
    {
        copyDwords(dst, dstOffset + numByteCopied, src, srcOffset + numByteCopied, 16, bb, iter);
    }
    if (numRegs != 0)
    {
        copyDwords(dst, dstOffset + numByteCopied, src, srcOffset + numByteCopied, 8, bb, iter);
    }
}

void HWConformity::fix64bInst( INST_LIST_ITER iter, G4_BB* bb )
{

    // HW restrictions:
    // [DevCHV, DevBXT]: When source or destination datatype is 64b, indirect addressing must not be used.
    // the region rules are:
    // Source and Destination horizontal stride must be aligned to the execution datatype.
    // Example:
    // mov (4) r10.0:df r11.0<16;8,2>:f // Source stride must be 2 since datatype is smaller
    // move (4) r10.0<2>:f r11.0<4;4,1>:df // Destination stride must be 2 since datatype is smaller.
    // as this would require splitting in some cases
    // Regioning must ensure Src.Vstride = Src.Width * Src.Hstride
    // Source and Destination offset must be the same, except the case of scalar source
    // [DevCHV, DevBXT]: When source or destination datatype is 64b, indirect addressing must not be used.
    // [DevCHV, DevBXT]: ARF registers must never be used with 64b datatype.

    if (!builder.no64bitRegioning())
    {
        return;
    }

    G4_INST* inst = *iter;
    bool uses64BitType = false;
    bool isDWMultiply = false;
    uint8_t execSize = inst->getExecSize();

    if (inst->isSend())
    {
        return;
    }
    if (inst->getDst() != NULL && G4_Type_Table[inst->getDst()->getType()].byteSize == 8)
    {
        uses64BitType = true;
    }
    for (int i = 0; !uses64BitType && i < G4_Inst_Table[inst->opcode()].n_srcs; i++)
    {
        G4_Operand* src = inst->getSrc(i);
        if (src != NULL && G4_Type_Table[src->getType()].byteSize == 8)
        {
            uses64BitType = true;
        }
    }
    if (inst->opcode() == G4_mul && IS_DTYPE(inst->getSrc(0)->getType()) &&
        IS_DTYPE(inst->getSrc(1)->getType()))
    {
        //WA: dw*dw multiply is considered to use 64bit data type since the result is 64-bit
        uses64BitType = true;
        isDWMultiply = true;
    }

    if (uses64BitType)
    {
#if 0
//#ifdef DEBUG_VERBOSE_ON
        std::cout << "CHV 64b fix for:\n";
        inst->emit(std::cout);
        std::cout << "\n";
#endif
        int numSrc = G4_Inst_Table[inst->opcode()].n_srcs;

        // handle indirect sources first
        for (int i = 0; i < numSrc; ++i)
        {
            G4_Operand* src = inst->getSrc(i);
            if (src != nullptr && src->isSrcRegRegion() && src->asSrcRegRegion()->getRegAccess() == IndirGRF)
            {
                G4_SrcRegRegion* srcAsRegion = src->asSrcRegRegion();
                RegionDesc* region = srcAsRegion->getRegion();
                int byteSize = G4_Type_Table[srcAsRegion->getType()].byteSize;
                if (byteSize == 8)
                {
                    // right bound is not available for indirect operands
                    // FIXME: this code should be moved to getRightBound()
                    int rightBound = 0;
                    // we must change move type to UD

                    if (region->isScalar())
                    {
                        rightBound = byteSize;
                    }
                    else if (region->isRegionWH())
                    {
                        rightBound = inst->getExecSize() * byteSize;
                    }
                    else
                    {
                        int num_rows = inst->getExecSize() / region->width;
                        rightBound = (num_rows - 1) * region->vertStride * byteSize +
                                region->horzStride * (region->width - 1) * byteSize +
                                byteSize;
                    }

                    int numDwords = rightBound / G4_Type_Table[Type_UD].byteSize;
                    numDwords = Round_Up_Pow2(numDwords);
                    G4_Declare* tmpSrc = builder.createTempVar(numDwords / 2, src->getType(), Either, Sixteen_Word);
                    // new source's region varies depending on whether it's VxH or 1x1
                    RegionDesc* newRegion = region->isRegionWH() ? builder.getRegionStride1() : region;
                    copyDwordsIndirect(tmpSrc, srcAsRegion, numDwords, bb, iter);
                    G4_SrcRegRegion* tmpSrcOpnd = builder.createSrcRegRegion(srcAsRegion->getModifier(),
                        Direct, tmpSrc->getRegVar(), 0, 0, newRegion, tmpSrc->getElemType());
                    inst->setSrc(tmpSrcOpnd, i);
                }
                else
                {
                    // use the good ol' insertMovBefore
                    G4_Operand* tmpSrc = insertMovBefore(iter, i, src->getType(), bb);
                    G4_Declare* tmpSrcDcl = tmpSrc->getTopDcl();
                    tmpSrcDcl->setSubRegAlign(Sixteen_Word);
                    inst->setSrc(tmpSrc, i);
                }
            }
        }

        // now handle direct sources with bad region/alignment
        bool hasSameOffset = hasSameSubregOffset(inst);
        for (int i = 0; i < numSrc; i++)
        {
            G4_Operand* src = inst->getSrc(i);
            if (src != NULL && src->isSrcRegRegion())
            {
                G4_SrcRegRegion* srcAsRegion = src->asSrcRegRegion();
                RegionDesc* region = srcAsRegion->getRegion();
                int byteSize = G4_Type_Table[srcAsRegion->getType()].byteSize;

                if (!isDWMultiply && !region->isScalar() &&
                    (byteSize != 8 && (byteSize * region->horzStride) < 8))
                {
                    // source is not 8 byte aligned
                    // this can happen e.g. for
                    // mov (8) r1.0<1>:df (mod)r3<8;8,1>:f
                    // which we'd need to change to
                    // mov (8) r10.0<2>:f (mod)r3.0<8;8,1>:f
                    // mov (8) r1.0<1>:df r10.0<8;4,2>:f
                    // to satisfy rule 1
                    uint8_t exSize = inst->getExecSize();
                    uint16_t multFactor = (uint16_t)(8 / byteSize);
                    G4_Type tmpType = srcAsRegion->getType();
                    if (multFactor == 8)
                    {
                        // byte type needs special handling since we can't have stride 8
                        tmpType = (tmpType == Type_B) ? Type_W : Type_UW;
                        multFactor = 4;
                    }
                    MUST_BE_TRUE(multFactor != 8, "does not support 64b operation with byte source");
                    G4_Declare* tmp = builder.createTempVar(exSize * multFactor,
                        tmpType, Either, Sixteen_Word);
                    G4_DstRegRegion* tmpDst = builder.Create_Dst_Opnd_From_Dcl(tmp, multFactor);
                    G4_INST* movInst = builder.createInternalInst(NULL, G4_mov, NULL, false,
                        inst->getExecSize(), tmpDst, src, NULL, inst->getOption());
                    bb->instList.insert(iter, movInst);
                    uint16_t width = exSize;
                    if (width * 8 > GENX_GRF_REG_SIZ)
                    {
                        // can't have width cross GRF
                        width = 4;
                    }
                    G4_SrcRegRegion* newSrc = builder.Create_Src_Opnd_From_Dcl(tmp,
                        builder.createRegionDesc((uint16_t)multFactor * width, width, multFactor));
                    inst->setSrc(newSrc, i);
                }
                else if (region->isScalar())
                {
#if 0
                    // scalar region still must be aligned to qword, though it can be any qword
                    if (byteSize < 8 && !builder.isOpndAligned(srcAsRegion, 8))
                    {
                        G4_Operand* tmpSrc = insertCopyBefore(iter, i, Four_Word, bb);
                        inst->setSrc(tmpSrc, i);
                    }
#endif
                }
                else if (!hasSameOffset)
                {
                    // we need a temp src that is GRF-aligned
                    if (byteSize == 8)
                    {
                        // the same src/dst offset restriction applies to move as well, so we have to generate
                        // a packed move with UD type to work around the restriction
                        // e.g., for
                        // add (2) ... r1.1<4;2,2>:q
                        // we turn it into
                        // mov (8) r10.0<1>:ud r1.2<1;1,0>:ud {NoMask}
                        // add (2) ... r10.0<4;2,2>:q
                        int numDwords = (src->getRightBound() - src->getLeftBound() + 1) / G4_Type_Table[Type_UD].byteSize;
                        numDwords = Round_Up_Pow2(numDwords);
                        G4_Declare* tmpSrc = builder.createTempVar(numDwords / 2, src->getType(), Either, Sixteen_Word);
                        copyDwords(tmpSrc, 0, src->getTopDcl(), src->getLeftBound(), numDwords, bb, iter);
                        G4_SrcRegRegion* tmpSrcOpnd = builder.createSrcRegRegion(srcAsRegion->getModifier(),
                            Direct, tmpSrc->getRegVar(), 0, 0, srcAsRegion->getRegion(), tmpSrc->getElemType());
                        inst->setSrc(tmpSrcOpnd, i);
                    }
                    else
                    {
                        // use the good ol' insertMovBefore
                        G4_Operand* tmpSrc = insertMovBefore(iter, i, src->getType(), bb);
                        G4_Declare* tmpSrcDcl = tmpSrc->getTopDcl();
                        tmpSrcDcl->setSubRegAlign(Sixteen_Word);
                        inst->setSrc(tmpSrc, i);
                    }
                }
            }
        }

        for (int i = 0; i < numSrc; i++)
        {
            // rewrite <1;1,0> to <2;2,1> since HW does not like the former
            G4_Operand* src = inst->getSrc(i);
            if (src != nullptr && src->isSrcRegRegion())
            {
                G4_SrcRegRegion* srcAsRegion = src->asSrcRegRegion();
                RegionDesc* region = srcAsRegion->getRegion();
                if (!region->isRegionWH() && region->vertStride != region->horzStride * region->width)
                {
                    // see if we can fix the region to satisfy VS = W * HS
                    if (region->width == inst->getExecSize())
                    {
                        // vs is a don't care, change to <w*hs, w, hz>
                        srcAsRegion->setRegion(builder.createRegionDesc(region->width * region->horzStride, region->width, region->horzStride));
                    }
                    else if (region->width == 1)
                    {
                        // hs is a don't care, change it to <esize*vs, esize, vs>
                        MUST_BE_TRUE(region->vertStride <= 4, "illegal vertical stride");

                        uint16_t wd = inst->getExecSize();
                        uint16_t hs = region->vertStride;
                        if (src->crossGRF())
                        {
                            // Make sure the new hs does not cross GRF
                            uint32_t nbytesIn1stGRF = GENX_GRF_REG_SIZ - (src->getLeftBound() % GENX_GRF_REG_SIZ);
                            uint32_t eltBytes = G4_Type_Table[srcAsRegion->getType()].byteSize;
                            uint32_t neltsIn1stGRF = nbytesIn1stGRF / eltBytes;

                            MUST_BE_TRUE((nbytesIn1stGRF % eltBytes) == 0, "Bad region with element crossing GRF");
                            MUST_BE_TRUE((neltsIn1stGRF % hs) == 0, "hs cannot cross GRF");

                            wd = neltsIn1stGRF / hs;
                            // Get the largest powOfTwo that can divide wd
                            wd = wd & (-wd);
                            //MUST_BE_TRUE( wd > 1, "Cannot select non-1 width w/o crossing GRF");
                        }
                        srcAsRegion->setRegion(builder.createRegionDesc(wd * hs, wd, hs));
                    }

                    else
                    {
                        // FIXME: Both VS and HS are used by the region, so we have to either split inst or insert multiple moves to pack the source
                        // both are painful, so we assert for now and fix later if we encounter such a case
                        MUST_BE_TRUE(false, "Unhandled bad 64b region on CHV/BXT");
                    }

                }
            }
        }
        G4_DstRegRegion* dst = inst->getDst();
        if (dst != NULL && !dst->isNullReg())
        {
            bool needsTmpDst = dst->getRegAccess() != Direct ||
                (execSize > 1 && !hasSameOffset) ||
                dst->isAreg();
            if (needsTmpDst)
            {
                // we need to have a temp dst that is direct and GRF-aligned
                if (dst->getRegAccess() == Direct && G4_Type_Table[dst->getType()].byteSize == 8)
                {
                    // the same src/dst offset restriction applies to move as well, so we have to generate
                    // a move with UD type to work around the restriction
                    // e.g., for
                    // add (2) r1.2<1>:q ...
                    // we generate
                    // add (2) r3.0<1>:q ...
                    // mov (4) r1.4<1>:ud r3.0<1;1,0>:ud {NoMask}
                    // If dst is not contiguous, we additionally add a move to pre-load the old values:
                    // add (2) r1.2<2>:q ...
                    // becomes
                    // mov (8) r3.0<1>:ud r1.4<1;1,0>:ud {NoMask}
                    // add (2) r3.0<2>:q ...
                    // mov (8) r1.4<1>:ud r3.0<1;1,0>:ud {NoMask}
                    int numDwords = (dst->getRightBound() - dst->getLeftBound() + 1) / G4_Type_Table[Type_UD].byteSize;
                    numDwords = Round_Up_Pow2(numDwords);
                    G4_Declare* tmpDst = builder.createTempVar(numDwords / 2, dst->getType(), Either, Sixteen_Word);
                    if (numDwords > execSize * 2)
                    {
                        // dst is not packed, need a move to pre-load the dst value into tmp
                        copyDwords(tmpDst, 0, dst->getTopDcl(), dst->getLeftBound(), numDwords, bb, iter);
                    }
                    INST_LIST_ITER next = iter;
                    ++next;
                    copyDwords(dst->getTopDcl(), dst->getLeftBound(), tmpDst, 0, numDwords, bb, next);
                    inst->setDest(builder.Create_Dst_Opnd_From_Dcl(tmpDst, dst->getHorzStride()));
                }
                else
                {
                    // use the good ol' insertMoveAfter
                    G4_DstRegRegion* tmpDst = insertMovAfter(iter, dst, dst->getType(), bb);
                    G4_Declare* tmpDstDcl = tmpDst->getTopDcl();
                    tmpDstDcl->setSubRegAlign(Sixteen_Word);
                    inst->setDest(tmpDst);
                    if (G4_Type_Table[dst->getType()].byteSize == 8)
                    {
                        // tmpDst is indirect and thus still does not conform
                        // we rewrite
                        // mov (e) r[a0.0]<1>:q src<1;1,0>:q
                        // into
                        // mov (e*2) r[a0.0]<1>:ud src<1;1,0>:ud {NoMask}
                        INST_LIST_ITER movIter = iter;
                        ++iter;
                        G4_INST* movInst = *iter;
                        MUST_BE_TRUE(movInst->opcode() == G4_mov && movInst->getDst() == dst &&
                            movInst->getSrc(0)->isSrcRegRegion(),
                            "unexpected instruction created by insertMovAfter");
                        MUST_BE_TRUE(dst->getHorzStride() == 1, "only stride 1 is supported for now");
                        dst->setType(Type_UD);
                        G4_SrcRegRegion* src = movInst->getSrc(0)->asSrcRegRegion();
                        G4_Declare* tmpAsUD = builder.createTempVar(tmpDstDcl->getNumElems() * 2, Type_UD, Either, Any);
                        tmpAsUD->setAliasDeclare(tmpDstDcl, 0);
                        RegionDesc* newRegion = src->getRegion()->isScalar() ?
                            builder.createRegionDesc(0, 2, 1) : builder.getRegionStride1();
                        G4_SrcRegRegion* srcAsUD = builder.createSrcRegRegion(src->getModifier(),
                            src->getRegAccess(), tmpAsUD->getRegVar(), src->getRegOff(),
                            src->getSubRegOff() * 2, newRegion, tmpAsUD->getElemType());
                        movInst->setSrc(srcAsUD, 0);
                        movInst->setExecSize(inst->getExecSize() * 2);

                        // mov saturate/pred to the original inst
                        movInst->setOptionOn(InstOpt_WriteEnable);
                        if (movInst->getSaturate())
                        {
                            movInst->setSaturate(false);
                            inst->setSaturate(true);
                        }
                        G4_Predicate* pred = movInst->getPredicate();
                        if (pred)
                        {
                            MUST_BE_TRUE(inst->getPredicate() == nullptr, "both inst and movInst have predicates");
                            movInst->setPredicate(nullptr);
                            inst->setPredicate(pred);
                        }
                    }
                }
            }
        }
    }
}

//------------------------------------------------------------------------------
//
//  For BDW, 32 bits integer multiply is implemented as the following macro
//
//  mul (8) acc0:d     r2.0<8;8,1>d   r3.0<16;8,2>:uw
//  mach (8) rTemp<1>:d r2.0<8;8,1>d   r3.0<8;8,1>:d
//  mov (8) r5.0<1>:d   rTemp:d // hi-32bits
//  mov (8) r6.0<1>:d acc0:d // lo-32bits
//
//  Note that this only changes the mul instruction's src1, mach and mov is generated elsewhere
//------------------------------------------------------------------------------
void HWConformity::fixMulSrc1( INST_LIST_ITER i, G4_BB* bb )
{
    G4_INST *inst = *i;
    G4_Operand *src1 = inst->getSrc(1);

    if (!IS_DTYPE(src1->getType()))
    {
        // this could happen if dst is Q
        return;
    }

    if (src1->isImm())
    {
        uint64_t truncVal = src1->asImm()->getImm() & 0xFFFF;
        G4_Imm *new_src1 = builder.createImm(truncVal, Type_UW);
        inst->setSrc(new_src1, 1);
        return;
    }

    assert(src1->isSrcRegRegion() && "region expected");
    G4_SrcRegRegion *srcRegion = src1->asSrcRegRegion();
    RegionDesc *rd = srcRegion->getRegion();
    if (rd->horzStride >= 4)
    {
        G4_Operand* new_src1 = insertMovBefore(i, 1, Type_UW, bb);
        inst->setSrc(new_src1, 1);
    }
    else
    {
        // create a new opnd with type UW
        unsigned short scale = G4_Type_Table[Type_D].byteSize / G4_Type_Table[Type_UW].byteSize;
        unsigned short newHS = rd->horzStride * scale;
        unsigned short newVS = rd->vertStride * scale;
        RegionDesc *new_rd = builder.createRegionDesc(newVS, rd->width, newHS);
        short subRegOff = srcRegion->getSubRegOff();
        if (srcRegion->getRegAccess() == Direct)
        {
            subRegOff *= scale;
        }
        auto new_src1 = builder.createSrcRegRegion(
            srcRegion->getModifier(), srcRegion->getRegAccess(),
            srcRegion->getBase(), srcRegion->getRegOff(), subRegOff, new_rd,
            Type_UW);
        inst->setSrc(new_src1, 1);
        if (srcRegion->getRegAccess() != Direct)
        {
            new_src1->setImmAddrOff(srcRegion->getAddrImm());
        }
    }
}

/*
 *  only acc0 may be used in DWord operations, so we have to break a
 *  SIMD16 DWord multiply into two mul-mach-mov sequences.
 *
 *  Input:
 *  (f0) mul (16) dst:d  src0:d  src1:d
 *
 *  Output:
 *  mul (8) acc0:d  src0:d  src1:d
 *  mach    (8) null:d  src0:d  src1:d
 *  (f0) mov (8) dst:d acc0:d
 *  mul (8) acc0:d  src0+1:d  src1+1:d
 *  mach    (8) null:d  src0+1:d    src1+1:d
 *  (f1) mov (8) dst+1:d acc0:d
 *
 */
void HWConformity::splitDWMULInst( INST_LIST_ITER &start, INST_LIST_ITER &end, G4_BB *bb )
{
    // split simd16 inst into SIMD8 ones, since D is not supported for acc1
    INST_LIST_ITER iter = start, last_iter = end;
    //iter--;
    last_iter++;
    INST_LIST_ITER curr_iter;
    while( iter != end )
    {
        curr_iter = iter;
        evenlySplitInst( curr_iter, bb );
        // curr_iter points to the second half after instruction splitting
        G4_INST *expand_sec_half_op = *curr_iter;
        iter++;

        bb->instList.insert( last_iter, expand_sec_half_op );
        if( curr_iter == start )
        {
            start--;
        }
        bb->instList.erase( curr_iter );
    }
    // handle the last inst
    if( iter == end )
    {
        evenlySplitInst( iter, bb );
        G4_INST *expand_sec_half_op = *iter;
        bb->instList.insert( last_iter, expand_sec_half_op );
        end--;
        bb->instList.erase( iter );
    }
}

static bool isGoodMadType(G4_Type type)
{
    switch (type)
    {
    case Type_F:
    case Type_HF:
        return true;
    case Type_DF:
        return true;
    default:
        return false;
    }
}

bool HWConformity::isGoodAlign1TernaryDst(G4_INST* inst) const
{
    // Align1 MAD requirements:
    // -- dst must be direct GRF/acc with horizontal stride 1 or 2
    G4_Type execType = inst->getExecType();
    G4_DstRegRegion* dst = inst->getDst();

    MUST_BE_TRUE(!IS_QTYPE(dst->getType()) && !IS_BTYPE(dst->getType()), "3Src inst don't support Q and B dst types");

    if (!builder.hasMixMode() &&
        (dst->getType() == Type_HF && execType != Type_HF))
    {
        return false;
    }

    int alignInBytes = 8;
    // if src2 is not a scalar, then align it to 32 bytes.
    if (builder.noSrc2Regioning())
    {
        unsigned src2Pos = inst->opcode() == G4_pseudo_mad ? 0 : 2;
        auto src2 = inst->getSrc(src2Pos);
        if (src2->isSrcRegRegion() && !src2->asSrcRegRegion()->isScalar())
        {
            alignInBytes = 32;
        }
    }

    if (!builder.isOpndAligned(dst, alignInBytes))
    {
        // dst must be 8 byte aligned due to encoding issues
        return false;
    }

    uint32_t effectiveStride = dst->getHorzStride();
    if (G4_Type_Table[dst->getType()].byteSize < G4_Type_Table[execType].byteSize)
    {
        if (IS_TYPE_INT(dst->getType()))
        {
            effectiveStride *= G4_Type_Table[execType].byteSize / G4_Type_Table[dst->getType()].byteSize;
        }
        else
        {
            // we have mixed HF and F inst
            // dst can be packed HF, but then it must be oword aligned
            // this should be checked later for mixed mode inst
        }
    }

    return dst->getRegAccess() == Direct && effectiveStride <= 2;
}

//
// check for legal align1 ternary inst sources
//
bool HWConformity::isGoodAlign1TernarySrc(G4_INST* inst, int srcPos, bool canBeImm)
{
    MUST_BE_TRUE(srcPos >= 0 && srcPos < 3, "illegal source pos");

    uint8_t execSize = inst->getExecSize();
    G4_Operand* src = inst->getSrc(srcPos);
    // for pseudo_mad we have to swap src0 and src2
    bool isSrc2 = inst->opcode() == G4_pseudo_mad ? srcPos == 0 : srcPos == 2;

    if (!builder.hasMixMode())
    {
        G4_Type execType = inst->getExecType();
        if (src->getType() == Type_HF && execType != Type_HF)
        {
            return false;
        }
    }

    if (IS_QTYPE(src->getType()))
    {
        return false;
    }

    if (inst->opcode() == G4_pseudo_mad && isSrc2)
    {
        if (IS_DTYPE(src->getType()))
        {
            return false;
        }

        if (builder.noSrc2Regioning() && IS_BTYPE(src->getType()))
        {
            return false;
        }
    }

    if (src->isImm())
    {
        // either src0 or src2 can be 16b imm, but not both
        // permanent WA: simd16 inst can't have src0 imm. 
        // Instead of splitting, we just add a move

        if (canBeImm && (srcPos == 0 || srcPos == 2) && G4_Type_Table[src->getType()].byteSize <= 2)
        {
            if (VISA_WA_CHECK(builder.getPWaTable(), WaNoSimd16TernarySrc0Imm))
            {
                return !isSrc2 && inst->getExecSize() == 16 ? false : true;
            }
            return true;
        }
        return false;
    }
    else if (src->isSrcRegRegion())
    {

        if (src->asSrcRegRegion()->getRegAccess() != Direct)
        {
            return false;
        }

        auto checkSingleStrideRegion = [](G4_SrcRegRegion* src, int stride, IR_Builder& builder)
        {
            if (stride > 4)
            {
                return false;
            }
            else if ((src->getLeftBound() % GENX_GRF_REG_SIZ != 0) &&
                (src->getRightBound() - src->getLeftBound() >= GENX_GRF_REG_SIZ - 1))
            {
                // we have to make sure width is not being used to cross GRF, as <1;1,0> 
                // is not a legal region for align1 ternary source (vs 1 not supported)
                int minAlignment = G4_Type_Table[src->getType()].byteSize * stride * 2;
                return builder.isOpndAligned(src, minAlignment);
            }
            return true;
        };

        // the following regions are supported:
        // <N;N,0>
        // <0;1,0>
        // <W*H;W,H>
        RegionDesc* srcRegion = src->asSrcRegRegion()->getRegion();
        if (srcRegion->isScalar())
        {
            return true;
        }

        // src0 and src1 (for psuedo-mad, it's src1 and src2) may use the <N;N,0> region
        // as they come with a vStride in encoding
        // TODO: we may consider swapping src1 and src2 to catch more regions
        if (!isSrc2)
        {
            uint16_t stride = 0;
            if (srcRegion->isSingleStride(execSize, stride))
            {
                return checkSingleStrideRegion(src->asSrcRegRegion(), stride, builder);
            }
            // <4;4,0> and <8;8,0> are ok
            return srcRegion->vertStride == srcRegion->width &&
                srcRegion->horzStride == 0 && srcRegion->width < 8 && srcRegion->width != 2;
        }
        else
        {
            if (!builder.noSrc2Regioning())
            {
                // src2 (src0 for pseudo-mad) is without vstride, and its region must be
                // <esize*H;esize,H>, with vstride derived from exSize and hstride
                uint16_t stride = 0;
                if (srcRegion->isSingleStride(execSize, stride))
                {
                    return checkSingleStrideRegion(src->asSrcRegRegion(), stride, builder);
                }
            }
            else
            {
                // not a scalar, src2 must be GRF aligned.
                if (!builder.isOpndAligned(src, G4_GRF_REG_NBYTES))
                {
                    return false;
                }

                uint16_t stride = 0;
                if (srcRegion->isSingleStride(execSize, stride))
                {
                    unsigned short dstExecSize = inst->getDst()->getExecTypeSize();
                    unsigned short srcExecSize = stride * src->asSrcRegRegion()->getElemSize();
                    // Source 2 and destination stride must be aligned to the same execution type.
                    // E.g. mad (4) r10.0<1>:hf src0 src1 r13.0<1>:hf
                    //      mad (4) r10.0<2>:hf src0 src1 r13.0<1>:f
                    //      mad (4) r10.0<1>:f  src0 src1 r13.0<2>:hf
                    if (dstExecSize == srcExecSize)
                    {
                        return true;
                    }
                }
            }

            return false;
        }
    }

    return true;
}

//
// a source is good for align16 if:
// -- it is a direct srcRegRegion
// -- it has contiguous region and can be made either GRF-aligned (for exec size >= 8)
//    or oword aligned (for exec size == 4)
// -- or it has scalar region and is not non-simd1 double
bool HWConformity::isGoodAlign16Src(G4_INST* inst, int srcPos)
{
    MUST_BE_TRUE(srcPos >= 0 && srcPos < 3, "illegal source pos");

    uint8_t execSize = inst->getExecSize();
    G4_Operand* src = inst->getSrc(srcPos);
    G4_Type opnd_type = src->getType();

    // Constants are not allowed as MAD opnds.
    if (src->isSrcRegRegion())
    {
        RegionDesc* region = src->asSrcRegRegion()->getRegion();
        G4_RegAccess regAcc = src->asSrcRegRegion()->getRegAccess();

        if (regAcc != Direct)
        {
            return false;
        }

        if (region->isContiguous(execSize))
        {
            if (getGenxPlatform() == GENX_BDW && getTypeSize(opnd_type) < 4)
            {
                // BDW HF has to be 32-byte aligned
                if (!builder.isOpndAligned(src, 32))
                {
                    return false;
                }
            }
            else
            {
                if (execSize >= 8)
                {
                    // operand must be GRF aligned, or oword aligned for HF/W
                    uint32_t align = std::min<uint32_t>(execSize * getTypeSize(src->getType()), 32);
                    if (!builder.isOpndAligned(src, align))
                    {
                        return false;
                    }
                }
                else if (execSize == 4 || execSize == 2)
                {
                    // operand must be oword-aligned
                    if (!builder.isOpndAligned(src, 16))
                    {
                        return false;
                    }
                }
            }
        }
        else if (src->asSrcRegRegion()->isScalar())
        {
            if (opnd_type == Type_DF && execSize != 1)
            {
                // scalar region is illegal for DF since replicate is not supported
                return false;
            }

            if (opnd_type == Type_HF && getGenxPlatform() == GENX_BDW) {
                return false;
            }
        }
        else
        {
            // all other regions are illegal
            return false;
        }

        return true;
    }
    else
    {
        return false;
    }

}

//
// Move modifiers of src2 in pseudo_mad to its defining instruction.
//
// mul (16) V66(0,0)<1>:d V46(23,0)<16;16,1>:w 0x39db:w {Align1, H1}
// psuedo_mad (16) V67(0,0)<1>:d V469,0)<8;8,1>:w 0x1b5d:w -V66(0,0)<16;16,1>:d
//
// becomes
//
// mul (16) V66(0,0)<1>:d -V46(23,0)<16;16,1>:w 0x39db:w {Align1, H1}
// psuedo_mad (16) V67(0,0)<1>:d V469,0)<8;8,1>:w 0x1b5d:w V66(0,0)<16;16,1>:d
//
static void tryTransferSrcModifier(IR_Builder &builder, G4_INST *def,
                                   G4_Operand *src)
{
    // Only when def has no other users.
    if (!def->hasOneUse())
        return;

    // Only transfer for integer types.
    if (!IS_SIGNED_INT(src->getType()))
        return;

    // In case the use type is different from the def type.
    if (!def->getDst() || (def->getDst()->getType() != src->getType()))
        return;

    switch (def->opcode()) {
    default:
        break;

    // Probably this is the only interesting op, since G4_math will not be
    // used to generate mac.
    case G4_mul:
    {
        // Chances are src1 is an immediate.
        G4_Operand *defSrc1 = def->getSrc(1);
        if (!IS_SIGNED_INT(defSrc1->getType()))
            return;

        if (defSrc1->isImm())
        {
            G4_Imm *val = defSrc1->asImm();
            // Mod_Minus is assumed.
            G4_Imm *newVal = builder.createImm(-val->getInt(), val->getType());
            def->setSrc(newVal, 1);
            src->asSrcRegRegion()->setModifier(Mod_src_undef);
        }
        else if (defSrc1->isSrcRegRegion())
        {
            G4_SrcRegRegion *reg = defSrc1->asSrcRegRegion();
            if (reg->getModifier() == Mod_src_undef)
            {
                reg->setModifier(src->asSrcRegRegion()->getModifier());
                src->asSrcRegRegion()->setModifier(Mod_src_undef);
            }
            else if (reg->getModifier() == Mod_Minus)
            {
                reg->setModifier(Mod_src_undef);
                src->asSrcRegRegion()->setModifier(Mod_src_undef);
            }
        }
    } break;
    }
}

// Try to move source modifiers on MAD's src2 into its defintion. This allows
// pseudo_mad ops to be translated into mac ops.
void HWConformity::tryEliminateMadSrcModifier(IR_Builder &builder, G4_INST *inst)
{
    ASSERT_USER(inst->opcode() == G4_pseudo_mad, "not a speudo-mad");

    // For pseudo_mad, src2 is the major source operand to be examined later.
    // If there is no modifier on src2, then nothing to do.
    G4_Operand *src2 = inst->getSrc(2);
    if (!src2->isSrcRegRegion())
        return;

    // Currently, only handle modifier minus. To handle others, we may need
    // to insert extra instructions.
    if (src2->asSrcRegRegion()->getModifier() != Mod_Minus)
        return;

    // Only when src2 has a single definition.
    if (G4_INST *def = inst->getSingleDef(Opnd_src2, true))
    {
        tryTransferSrcModifier(builder, def, src2);
    }
}

/// Heuristic to decide whether this fp pseudo-mad should be lowered into a
/// GEN mad or not. Returns true if mad is preferred, false otherwise.
///
/// We flavor generating non-mad when this vISA mad is part of b2b mads that
/// share the same dst.
///
bool HWConformity::isFpMadPreferred(G4_BB *bb, INST_LIST_ITER iter)
{
    G4_INST *inst = *iter;
    G4_Operand *dst = inst->getDst();
    MUST_BE_TRUE(inst->opcode() == G4_pseudo_mad, "expect pseudo mad");

    // Check whether test_inst is sharing the same dst.
    auto equal_mad_dst = [](G4_INST *test_inst, G4_Operand *dst)
    {
        if (test_inst->opcode() == G4_pseudo_mad)
        {
            G4_Operand *test_dst = test_inst->getDst();
            if (test_dst->compareOperand(dst) == Rel_eq)
                return true;
        }
        return false;
    };

    auto next_iter = std::next(iter);
    if (next_iter != bb->instList.end())
    {
        G4_INST *next_inst = *next_iter;
        if (equal_mad_dst(next_inst, dst))
            return false;
    }
    if (iter != bb->instList.begin())
    {
        auto prev_iter = std::prev(iter);
        G4_INST *prev_inst = *prev_iter;
        if (equal_mad_dst(prev_inst, dst))
            return false;
    }

    // FIXME: remove possile duplicate calls to isGoodAlign16Src, Cm only.
    // This will go away if we use an extra opcode to represent muladd.
    unsigned extraMov = 0;
    for (int k = 0; k < inst->getNumSrc(); k++)
    {
        if (!isGoodAlign16Src(inst, k))
        {
            // If need to insert >1 number of moves, then do not use mad.
            if (++extraMov > 1)
                return false;
        }
    }

    return true;
}

// generate align1 mad, inserting moves if necessary
// returns true if conversion is successful
// for floating point mad this must succeed due to precision requirements
bool HWConformity::generateAlign1Mad(G4_BB* bb, INST_LIST_ITER iter)
{

    G4_INST* inst = *iter;
    MUST_BE_TRUE(inst->opcode() == G4_pseudo_mad, "expect pseudo mad");
    bool mustDoMad = IS_TYPE_FLOAT_ALL(inst->getDst()->getType());

    if (!isGoodAlign1TernaryDst(inst))
    {
        if (mustDoMad)
        {
            auto alignment = builder.noSrc2Regioning() ? Sixteen_Word : Four_Word;
            inst->setDest(insertMovAfter(iter, inst->getDst(), inst->getDst()->getType(), bb, alignment));
        }
        else
        {
            return false;
        }
    }

    // try swapping src0 and src1 if src0 is D, as MAD only supports D + D * W,
    // and pseudo-mad src0 becomes mad src2
    {
        G4_Operand* src0 = inst->getSrc(0);
        G4_Operand* src1 = inst->getSrc(1);
        if (IS_DTYPE(src0->getType()) && src0->isSrcRegRegion())
        {
            if (!IS_DTYPE(src1->getType()))
            {
                inst->setSrc(src1, 0);
                inst->setSrc(src0, 1);
            }
        }
        else if (src1->isImm() && getTypeSize(src1->getType()) == 2)
        {
            //swap src0 and src1 as src0 supports imm
            inst->setSrc(src1, 0);
            inst->setSrc(src0, 1);
        } else if (builder.noSrc2Regioning() &&
                   src0->isSrcRegRegion() && src1->isSrcRegRegion() &&
                   !src0->asSrcRegRegion()->isScalar() &&
                   src1->asSrcRegRegion()->isScalar()) {
            // Swap src0 and src1 if src1 is scalar but src0 is not when src2
            // regioning support is quite limited.
            inst->setSrc(src1, 0);
            inst->setSrc(src0, 1);
        }
    }

    // check src
    bool canBeImm = true;
    for (int k = 0; k < inst->getNumSrc(); k++)
    {
        G4_Operand* src = inst->getSrc(k);
        if (!isGoodAlign1TernarySrc(inst, k, canBeImm))
        {
            if (mustDoMad)
            {
                bool isSrc2 = (k == 0);
                if (builder.noSrc2Regioning() && isSrc2)
                {
                    // Promote src2's type to f if necessary.
                    //
                    // mad (4) r10.0<1>:f src0 src1 r12.0<1>:hf  --> f
                    // mad (4) r10.0<2>:hf src0 src1 r12.0<1>:hf --> f
                    // mad (4) r10.0<1>:hf src0 src1 r12.0<2>:hf --> hf
                    // mad (4) r10.0<2>:hf src0 src1 r12.1<2>:hf --> f
                    G4_Type srcTy = src->getType();
                    unsigned short dstEltSz = inst->getDst()->getExecTypeSize();
                    if (dstEltSz >= 4 && IS_HFTYPE(src->getType()))
                    {
                        srcTy = Type_F;
                    }
                    inst->setSrc(insertMovBefore(iter, k, srcTy, bb, Sixteen_Word), k);

                    // Check if dst stride aligns with src2.
                    if (dstEltSz != G4_Type_Table[srcTy].byteSize)
                    {
                        inst->setDest(insertMovAfter(iter, inst->getDst(), inst->getDst()->getType(), bb, Sixteen_Word));
                    }
                }
                else
                {
                    inst->setSrc(insertMovBefore(iter, k, src->getType(), bb), k);
                }
            }
            else
            {
                return false;
            }
        }
        else
        {
            if (src->isImm())
            {
                canBeImm = false;
            }
        }
    }

    inst->setOpcode(G4_mad);

    //swap src0 and src2 (vISA MAD is src0*src1+src2, while GEN MAD is src1*src2+src0)
    G4_Operand* src0 = inst->getSrc(0);
    G4_Operand* src2 = inst->getSrc(2);
    inst->setSrc(src2, 0);
    inst->setSrc(src0, 2);

    return true;
}

// convert a FP (HF/F/DF) pseudo-mad into a GEN mad,
// inserting moves if necessary
// returns true if conversion is successful
// note that this must return true for IGC due to precision requirements
bool HWConformity::generateFPMad(G4_BB* bb, INST_LIST_ITER iter)
{
    G4_INST* inst = *iter;
    MUST_BE_TRUE(inst->opcode() == G4_pseudo_mad, "expect pseudo mad");
    uint8_t execSize = inst->getExecSize();
    G4_DstRegRegion *dst = inst->getDst();
    MUST_BE_TRUE(dst->getType() == Type_HF || dst->getType() == Type_F ||
        dst->getType() == Type_DF, "inst must have FP type");

    // Align16 MAD requirements:
    // -- dst and all 3 srcs have the same F/HF/DF type (mixed F/HF is allowed on CHV+)
    // -- dst and all 3 srcs have direct access
    // -- execution size is 16/8/4/1
    // -- dst and src must be packed
    // -- if src region is not scalar, its subregister must be 16 byte aligned

    // do not force fma for CM since it doesn't have precision requirements
    bool preferFpMad = builder.getOption(vISA_forceFPMAD) && builder.getOptions()->getTarget() != VISA_CM;
    if (!preferFpMad)
    {
        preferFpMad = isFpMadPreferred(bb, iter);
    }

    auto alignMent = execSize * G4_Type_Table[dst->getType()].byteSize;
    alignMent = (alignMent > 32)  ? 32 : alignMent;
    alignMent = (alignMent < 16)  ? 16 : alignMent;

    if (dst->getRegAccess() != Direct || dst->getHorzStride() != 1 ||
        !builder.isOpndAligned(dst, alignMent))
    {
        if (preferFpMad)
        {
            G4_DstRegRegion* tmpDst = insertMovAfter(iter, dst, dst->getType(), bb);
            inst->setDest(tmpDst);
        }
        else
        {
            return false;
        }
    }

    // check src
    for (int k = 0; k < inst->getNumSrc(); k++)
    {
        G4_Type type = inst->getSrc(k)->getType();
        MUST_BE_TRUE(type == Type_HF || type == Type_F || type == Type_DF,
            "expect FP type");
        bool goodSrc = isGoodAlign16Src(inst, k);
        if (!goodSrc && preferFpMad)
        {
            // insert moves if type is legal mad type
            if (isGoodMadType(type))
            {
                G4_Operand* src = inst->getSrc(k);
                if ((type == Type_DF ||
                     (type == Type_HF && getGenxPlatform() == GENX_BDW)) &&
                    execSize > 1 &&
                    (src->isImm() || src->asSrcRegRegion()->isScalar()))
                {
                    // MAD DF does not support .r, so we have to broadcast the value
                    // '.r' on MAD HF on BDW is not a replication of that
                    // scalar element but a pair of half.
                    auto align = type == Type_HF ? Sixteen_Word : Eight_Word;
                    broadcast(bb, iter, k, align);
                }
                else
                {
                    inst->setSrc(insertMovBefore(iter, k, type, bb), k);
                }
                goodSrc = true;
            }
        }
        if (!goodSrc)
        {
            return false;
        }
    }

    inst->setOpcode(G4_mad);

    //swap src0 and src2 (vISA MAD is src0*src1+src2, while GEN MAD is src1*src2+src0)
    G4_Operand* src0 = inst->getSrc(0);
    G4_Operand* src2 = inst->getSrc(2);
    inst->setSrc(src2, 0);
    inst->setSrc(src0, 2);

    return true;
}

// If the LF MAD does not conform to Genx ISA semantics, then translate
// it into a valid GenX sequence - either an equivalent MUL/ADD sequence
// or an equivalent MAC.
// ASSUMPTION:
//    This phase must be called at the end of all other optimizations
//    phases and just prior to testing for ACC spilling.
void HWConformity::fixMADInst( BB_LIST_ITER it )
{
    G4_BB* bb = *it;
    INST_LIST expand_list;
    // trace the MAD instrcutions that may be converted into MAC later
    std::vector<G4_INST*> madList;

    bool doAlign1Mad = builder.hasAlign1Ternary();

    bb->resetLocalId();
    INST_LIST_ITER i = bb->instList.begin();

    for (auto iterEnd = bb->instList.end(); i != iterEnd; ++i )
    {

        G4_INST *inst = *i;
        // predicated mad is not allowed?
        if( inst->opcode() != G4_pseudo_mad )
        {
            continue;
        }

        tryEliminateMadSrcModifier(builder, inst);

        G4_DstRegRegion *dst = inst->getDst();
        int exec_size = inst->getExecSize( );
        G4_Operand *src0 = inst->getSrc(0), *src1 = inst->getSrc(1), *src2 = inst->getSrc(2);

        bool conforming_genx_mad = true;
        bool generate_genx_mac;

        if (exec_size == 32)
        {
            conforming_genx_mad = false;
        }
        else
        {
            // since copy prop and def-hoisting are not allowed to Align16 instructions,
            // sources of psuedo mad should use the same data type as in CISA input
            // so we only check dst type
            switch (dst->getType())
            {
            case Type_F:
            case Type_HF:
                break;
            case Type_DF:
                break;
            case Type_W:
            case Type_UW:
            case Type_D:
            case Type_UD:
                if (!doAlign1Mad)
                {
                    conforming_genx_mad = false;
                }
                break;
            default:
                conforming_genx_mad = false;
            }
        }

        if (conforming_genx_mad)
        {
            bool doMad = doAlign1Mad ?
                generateAlign1Mad(bb, i) : generateFPMad(bb, i);
            if (doMad)
            {
                // done with this pseudo-mad
                continue;
            }
        }

        // Translate the LF MAD to an equivalent GenX sequence.
        if (builder.getOption(vISA_LocalMACopt))
        {
            generate_genx_mac = true;
        }
        else
        {
            generate_genx_mac = false;
        }

        bool dstPackedHF = false ;
        bool dstIsFloat = false;
        checkHFMixModeRule4_11(*i, dstPackedHF, dstIsFloat);
        //not dealing with that mess. Shouldn't be a common code sequence.
        if(dstPackedHF || dstIsFloat)
            generate_genx_mac = false;

        if( generate_genx_mac )
        {
            int emask = inst->getMaskOption();
            if (emask != InstOpt_WriteEnable && inst->getMaskOffset() != 0)
            {
                generate_genx_mac = false;
            }
            // If either src1 or src0 are DWORD then we cannot generate a MAC.
            // ACC does not support B type
            if (generate_genx_mac &&
                (IS_BTYPE(src2->getType()) ||
                    IS_DTYPE(src0->getType()) ||
                    IS_DTYPE(src1->getType())))
            {
                generate_genx_mac = false;
            }

            // If there is a modifer for src2, or src2 is accessed somewhere indirectly then we will
            // not generate a MAC.
            if (generate_genx_mac)
            {
                if (src2->isImm() ||
                    (src2->isSrcRegRegion() &&
                    (src2->asSrcRegRegion()->getModifier() != Mod_src_undef ||
                        src2->asSrcRegRegion()->getRegAccess() != Direct ||
                        (src2->getTopDcl() && src2->getTopDcl()->getAddressed()))) ||
                    src2->getType() == Type_DF)
                {
                    generate_genx_mac = false;
                }
            }
        }
        // we can't do mac if src2 is global or it has >1 def or its single def is global
        if( generate_genx_mac )
        {
            G4_INST *mad_src2_def_inst = inst->getSingleDef(Opnd_src2);
            if (!mad_src2_def_inst || kernel.fg.globalOpndHT.isOpndGlobal(src2) ||
                kernel.fg.globalOpndHT.isOpndGlobal(mad_src2_def_inst->getDst()))
            {
                generate_genx_mac = false;
            }

            
            if( madList.size() > 0 && mad_src2_def_inst != madList.back())
            {
                // terminate the last mad list as this mad has a different definition
                int32_t lastMadId = madList.back()->getLocalId();
                bool macGenerated = convertMAD2MAC( i, madList, bb );
                madList.clear();
                if (generate_genx_mac && macGenerated && 
                    mad_src2_def_inst->getLocalId() < lastMadId)
                {
                    // mad's definition is before the last use of acc
                    generate_genx_mac = false;
                }
            }

            if( generate_genx_mac &&
                ( mad_src2_def_inst->getPredicate() ||
                mad_src2_def_inst->getSaturate() ||
                mad_src2_def_inst->isMath() ||
                mad_src2_def_inst->opcode() == G4_shl ||
                mad_src2_def_inst->opcode() == G4_mad ||
                !mad_src2_def_inst->hasOneUse() ||
                ( isCompressedInst(mad_src2_def_inst) ^ isCompressedInst(inst) )) )
            {
                generate_genx_mac = false;
            }

            if( generate_genx_mac &&
                madList.size() == 0 &&
                IS_DTYPE(mad_src2_def_inst->getExecType()) )
            {
                // We don't generate mac in this case since by default we use w type for acc,
                // and it would violate dst alignment restriction
                // if mad_src2_def_inst is itself a psuedo_mad, however, then it's ok
                // since both sources for mac must have word type.
                generate_genx_mac = false;
            }

            if( generate_genx_mac )
            {
                // We will try to generate a MAC if it is possible to hoist
                // the definition for src2 into ACC, otherwise we will need to
                // generate a MOV/MAC; in which case we might as well
                // generate a MUL/ADD sequence anyway.

                // If the src2_def_op does not immediately precede the
                // MAD then we will attempt to schedule backward op to
                // immediately after src2_def_op. This will increase
                // the MAC reduction opportunities as it has the
                // effect of keeping ACC live ranges to very
                // short intervals.
                // NOTE: We do not attempt to schedule the src2_def_op
                // to just before op, as src2_def_op may be a
                // previously scheduled MAD.

                INST_LIST_ITER mov_iter = i;
                mov_iter--;
                uint16_t movDist = 0;

                if ((*mov_iter) != mad_src2_def_inst) {
                    // check if src and dst of MAD are re-defined in between and
                    // if dst is used in between
                    if (!findHoistLocation(i, mov_iter, movDist, mad_src2_def_inst))
                    {
                        generate_genx_mac = false;
                    }
                    else
                    {
                        if (movDist > 0)
                        {
                            mov_iter++;
                            bb->instList.insert(mov_iter, inst);
                            INST_LIST_ITER tmpIter = i;
                            i--;
                            bb->instList.erase(tmpIter);
                        }
                    }
                }

                // if instruction moving is blocked by some re-def, we need to check if it is possible that the ACC def instruction
                // will be split later. If yes, we do not use ACC and MAC here.

                // push this decision to convertMAC2MAD

                if (generate_genx_mac)
                {
                    if (madList.size() == 0)
                    {
                        // push src2 def into list
                        madList.push_back(mad_src2_def_inst);
                    }
                    madList.push_back(inst);
                }
            }
        }

        // translate MAD into MUL/ADD
        if( !generate_genx_mac )
        {
            convertMAD2MulAdd(i, bb);
            i++;
        }
    }
    if( madList.size() > 0 )
    {
        i--;
        convertMAD2MAC( i, madList, bb );
    }
}

struct AccInterval
{
    G4_INST* inst;
    int lastUse;
    bool mustBeAcc0 = false;
    bool isPreAssigned = false;
    int assignedAcc = -1;

    AccInterval(G4_INST* inst_, int lastUse_, bool preAssigned = false) :
        inst(inst_), lastUse(lastUse_), isPreAssigned(preAssigned)
    {
        if (isPreAssigned)
        {
            mustBeAcc0 = true;
            assignedAcc = 0;
        }
    }

    double getSpillCost()
    {
        if (isPreAssigned)
        {
            // don't spill pre-assigned
            return (double) 1000000;
        }
        int dist = lastUse - inst->getLocalId();
        return std::pow((double) inst->use_size(), 3) / dist;
    }
};

// returns true if the inst is a candidate for acc substitution
// lastUse is also update to point to the last use id of the inst
static bool isAccCandidate(G4_INST* inst, G4_Kernel& kernel, int& lastUse, bool& mustBeAcc0)
{
    mustBeAcc0 = false;
    G4_DstRegRegion* dst = inst->getDst();
    if (!dst || kernel.fg.globalOpndHT.isOpndGlobal(dst) || !inst->canDstBeAcc(*kernel.fg.builder))
    {
        return false;
    }

    // check that every use may be replaced with acc
    int lastUseId = 0;
    std::vector<G4_INST*> madSrc0Use;
    for (auto I = inst->use_begin(), E = inst->use_end(); I != E; ++I)
    {
        auto&& use = *I;
        G4_INST* useInst = use.first;
        Gen4_Operand_Number opndNum = use.second;
        lastUseId = std::max(lastUseId, useInst->getLocalId());
        // acc may be src0 of two-source inst or src1 of three-source inst
        // ToDo: may swap source here
        if (useInst->getNumSrc() == 3)
        {
            if (opndNum != Opnd_src1)
            {
                mustBeAcc0 = true;
                bool goodMadSrc0 = useInst->opcode() == G4_mad && opndNum == Opnd_src0;
                if (!goodMadSrc0)
                {
                    return false;
                }
                if (useInst->getSrc(0)->getType() == Type_HF && useInst->getMaskOffset() == 16)
                {
                    // we must use acc1, and need to check that inst does not have an acc0 source
                    // so that dst and src won't have different acc source
                    if (inst->isAccSrcInst())
                    {
                        bool hasAcc0Src = false;
                        auto isAcc0 = [](G4_SrcRegRegion* src)
                        {
                            return src->getBase()->asAreg()->getArchRegType() == AREG_ACC0;
                        };
                        if (inst->getSrc(0)->isSrcRegRegion() && 
                            inst->getSrc(0)->asSrcRegRegion()->getBase()->isAccReg())
                        {
                            hasAcc0Src = isAcc0(inst->getSrc(0)->asSrcRegRegion());
                        }
                        else if (inst->getSrc(1)->isSrcRegRegion() &&
                            inst->getSrc(1)->asSrcRegRegion()->getBase()->isAccReg())
                        {
                            hasAcc0Src = isAcc0(inst->getSrc(1)->asSrcRegRegion());
                        }
                        if (hasAcc0Src)
                        {
                            return false;
                        }
                    }
                }
                madSrc0Use.push_back(useInst);
            }
        }
        else if (opndNum != Opnd_src0)
        {
            return false;
        }

        if (useInst->getSingleDef(opndNum) == nullptr)
        {
            // def must be the only define for this use
            return false;
        }

        int srcId = opndNum == Opnd_src0 ? 0 : 1;
        G4_Operand* src = useInst->getSrc(srcId);
        if (dst->getType() != src->getType() || kernel.fg.globalOpndHT.isOpndGlobal(src) ||
            dst->compareOperand(src) != Rel_eq)
        {
            return false;
        }
        if (!useInst->canSrcBeAcc(srcId, *kernel.fg.builder))
        {
            return false;
        }
    }

    // we have to avoid the case where the dst is used as both src0 and src1 of a mad
    for (auto madUse : madSrc0Use)
    {
        for (auto I = inst->use_begin(), E = inst->use_end(); I != E; ++I)
        {
            auto&& use = *I;
            G4_INST* useInst = use.first;
            Gen4_Operand_Number opndNum = use.second;
            if (madUse == useInst && opndNum == Opnd_src1)
            {
                return false;
            }
        }
    }

    lastUse = lastUseId;
    return true;
}

// replace an inst's dst and all of its (local) uses with acc
static void replaceDstWithAcc(G4_INST* inst, int accNum, IR_Builder& builder)
{
    G4_DstRegRegion* dst = inst->getDst();
    bool useAcc1 = false;  
    for (auto I = inst->use_begin(), E = inst->use_end(); I != E; ++I)
    {
        auto&& use = *I;
        G4_INST* useInst = use.first;
        if (useInst->opcode() == G4_mad && use.second == Opnd_src0)
        {
            // if we are replacing mad with mac, additionally check if acc1 needs to be used
            if (useInst->getMaskOffset() == 16 && dst->getType() == Type_HF)
            {
                useAcc1 = true;
                break;
            }
        }
    }

    G4_Areg* accReg = useAcc1 ? builder.phyregpool.getAcc1Reg() : builder.phyregpool.getAcc0Reg(); 
    G4_DstRegRegion* accDst = builder.createDstRegRegion(Direct, accReg,
        (short)accNum, 0, 1, dst->getType());
    inst->setDest(accDst);
    for (auto I = inst->use_begin(), E = inst->use_end(); I != E; ++I)
    {
        auto&& use = *I;
        G4_INST* useInst = use.first;
        int srcId = use.second == Opnd_src0 ? 0 : 1;
        G4_SrcRegRegion* oldSrc = useInst->getSrc(srcId)->asSrcRegRegion();
        G4_SrcRegRegion* accSrc = builder.createSrcRegRegion(oldSrc->getModifier(), Direct,
            accReg, (short)accNum, 0, builder.getRegionStride1(), dst->getType());
        if (useInst->opcode() == G4_mad && srcId == 0)
        {
            // change mad to mac as src0 of 3-src does not support acc
            auto updateDefSrcPos = [](G4_INST* useInst, Gen4_Operand_Number origPos)
            {
                for (auto DI = useInst->def_begin(), DE = useInst->def_end(); DI != DE; ++DI)
                {
                    auto&& def = *DI;
                    if (def.second == origPos)
                    {
                        for (auto UI = def.first->use_begin(), UE = def.first->use_end(); UI != UE; ++UI)
                        {
                            auto& use = *UI;
                            if (use.first == useInst && use.second == origPos)
                            {
                                switch (use.second)
                                {
                                case Opnd_src1:
                                    use.second = Opnd_src0;
                                    break;
                                case Opnd_src2:
                                    use.second = Opnd_src1;
                                    break;
                                default:
                                    assert(false && "unexpectd src pos");
                                }
                            }
                        }
                    }
                }
            };
            assert(accNum == 0 && "mad src0 may only use acc0");
            G4_Operand* macSrc0 = useInst->getSrc(1);
            updateDefSrcPos(useInst, Opnd_src1);
            G4_Operand* macSrc1 = useInst->getSrc(2);
            updateDefSrcPos(useInst, Opnd_src2);
            useInst->setSrc(macSrc0, 0);
            useInst->setSrc(macSrc1, 1);
            useInst->setOpcode(G4_mac);
            useInst->setImplAccSrc(accSrc);
        }
        else
        {
            useInst->setSrc(accSrc, srcId);
        }
    }
}

static uint32_t getNumACC(IR_Builder& builder)
{
    uint32_t numUserAcc = builder.getOptions()->getuInt32Option(vISA_numGeneralAcc);
    if (numUserAcc != 0)
    {
        // use user-provided value
        return numUserAcc;
    }
    return builder.getNumACC();
}

void HWConformity::multiAccSubstitution(G4_BB* bb)
{
    int numGeneralAcc = getNumACC(builder);

    std::vector<AccInterval*> intervals;

    //build intervals for potential acc candidates as well as pre-existing acc uses from mac/mach/addc/etc
    for (auto instIter = bb->instList.begin(), instEnd = bb->instList.end(); instIter != instEnd; ++instIter)
    {
        G4_INST* inst = *instIter;
        if (inst->defAcc())
        {
            // we should only have single def/use acc at this point, so any use would kill the def
            auto iter = instIter;
            auto useIter = std::find_if(++iter, instEnd, [](G4_INST* inst) { return inst->useAcc(); });
            int lastUseId = useIter == instEnd ? bb->instList.back()->getLocalId() : (*useIter)->getLocalId();
            AccInterval *newInterval = new AccInterval(inst, lastUseId, true);
            intervals.push_back(newInterval);
        }
        else
        {
            int lastUseId = 0;
            bool mustBeAcc0 = false;
            if (isAccCandidate(inst, kernel, lastUseId, mustBeAcc0))
            {
                // this is a potential candidate for acc substitution
                AccInterval *newInterval = new AccInterval(inst, lastUseId);
                newInterval->mustBeAcc0 = mustBeAcc0;
                intervals.push_back(newInterval);
            }
        }
    }

    //modified linear scan to assign free accs to intervals
    std::vector<bool> freeAccs;
    freeAccs.resize(numGeneralAcc, true);
    std::list<AccInterval*> activeIntervals;
    for (auto interval : intervals)
    {
        // expire intervals
        for (auto iter = activeIntervals.begin(), iterEnd = activeIntervals.end(); iter != iterEnd;)
        {
            AccInterval* active = *iter;
            if (active->lastUse <= interval->inst->getLocalId())
            {
                assert(!freeAccs[active->assignedAcc] && "active interval's acc should not be free");
                freeAccs[active->assignedAcc] = true;
                iter = activeIntervals.erase(iter);
            }
            else
            {
                ++iter;
            }
        }

        // assign interval/spill acc0 interval
        if (interval->isPreAssigned)
        {
            if (!freeAccs[0])
            {
                //spill active interval that is using acc0, if it exists
                auto acc0Iter = std::find_if(activeIntervals.begin(), activeIntervals.end(),
                    [](AccInterval* interval) { return interval->assignedAcc == 0; });
                assert(acc0Iter != activeIntervals.end() && "expect to find interval with acc0");
                assert(!(*acc0Iter)->isPreAssigned && "overlapping pre-assigned acc0");
                (*acc0Iter)->assignedAcc = -1;
                activeIntervals.erase(acc0Iter);
            }
            freeAccs[0] = false;
            activeIntervals.push_back(interval);
        }
        else
        {
            bool foundFreeAcc = false;
            for (int i = 0, end = interval->mustBeAcc0 ? 1 : (int)freeAccs.size(); i < end; ++i)
            {
                if (freeAccs[i])
                {
                    interval->assignedAcc = i;
                    freeAccs[i] = false;
                    activeIntervals.push_back(interval);
                    foundFreeAcc = true;
                    break;
                }
            }
            if (!foundFreeAcc)
            {
                // check if we should spill one of the active intervals
                auto spillCostCmp = [interval](AccInterval* intv1, AccInterval* intv2) 
                { 
                    if (!interval->mustBeAcc0)
                    {
                        return intv1->getSpillCost() < intv2->getSpillCost();
                    }

                    // different compr function if interval must use acc0
                    if (intv1->assignedAcc == 0 && intv2->assignedAcc == 0)
                    {
                        return intv1->getSpillCost() < intv2->getSpillCost();
                    }
                    else if (intv1->assignedAcc == 0)
                    {
                        return true;
                    }
                    return false; 
                };
                auto spillIter = std::min_element(activeIntervals.begin(), activeIntervals.end(),
                    spillCostCmp);
                auto spillCandidate = *spillIter;
                if (interval->getSpillCost() > spillCandidate->getSpillCost() &&
                    !spillCandidate->isPreAssigned && 
                    !(interval->mustBeAcc0 && spillCandidate->assignedAcc != 0))
                {
                    interval->assignedAcc = spillCandidate->assignedAcc;
                    spillCandidate->assignedAcc = -1;
                    activeIntervals.erase(spillIter);
                    activeIntervals.push_back(interval);
                }
            }
        }
    }

    for (auto interval : intervals)
    {
        if (!interval->isPreAssigned && interval->assignedAcc != -1)
        {
            G4_INST* inst = interval->inst;
            replaceDstWithAcc(inst, interval->assignedAcc * 2, builder);

            numAccSubDef++;
            numAccSubUse += (int)inst->use_size();
#if 0
            std::cout << "Acc sub def inst: \n";
            inst->emit(std::cout);
            std::cout << "[" << inst->getLocalId() << "]\n";
            std::cout << "Uses:\n";
            for (auto&& use : inst->useInstList)
            {
                std::cout << "\t";
                use.first->emit(std::cout);
                std::cout << "[" << use.first->getLocalId() << "]\n";
            }
#endif
        }
    }

    for (int i = 0, end = (int)intervals.size(); i < end; ++i)
    {
        delete intervals[i];
    }
}
// substitute local operands with acc when possible
void HWConformity::accSubstitution(G4_BB* bb)
{
    bb->resetLocalId();

    if (getNumACC(builder) > 1)
    {
        multiAccSubstitution(bb);
        return;
    }

    for (auto instIter = bb->instList.begin(), instEnd = bb->instList.end(); instIter != instEnd; ++instIter)
    {
        bool canDoAccSub = true;
        G4_INST* inst = *instIter;

        if (inst->defAcc())
        {
            // skip ahead till its single use
            // we should only have single def/use acc at this point, so any use would 
            // kill the def
            auto iter = instIter;
            auto useIter = std::find_if(++iter, instEnd, [](G4_INST* inst) { return inst->useAcc(); });
            if (useIter == instEnd)
            {
                return;
            }
            instIter = --useIter; // start at the use inst next time 
            continue;
        }

        int lastUseId = 0;
        bool mustBeAcc0 = false; //ignored
        if (!isAccCandidate(inst, kernel, lastUseId, mustBeAcc0))
        {
            continue;
        }

        // don't attempt acc sub if def and last use are too far apart
        // this is a crude way to avoid a long running life range from blocking 
        // other acc sub opportunities
        const int accWindow = 25;
        if (lastUseId == 0 || lastUseId - inst->getLocalId() > accWindow)
        {
            continue;
        }

        // check for intervening acc usage between inst and its last use
        auto subIter = instIter;
        ++subIter;
        for (int instId = inst->getLocalId() + 1; instId != lastUseId; ++subIter, ++instId)
        {
            G4_INST* anInst = *subIter;
            if (anInst->useAcc() || anInst->mayExpandToAccMacro(builder))
            {
                canDoAccSub = false;
                break;
            }
        }

        if (!canDoAccSub)
        {
            continue;
        }
        else
        {
            replaceDstWithAcc(inst, 0, builder);
            // advance iter to the last use of the acc
            instIter = subIter;
            --instIter;

            numAccSubDef++;
            numAccSubUse += (int)inst->use_size();

#if 0
            std::cout << "Acc sub def inst: \n";
            inst->emit(std::cout);
            std::cout << "[" << inst->getLocalId() << "]\n";
            std::cout << "Uses:\n";
            for (auto&& use : inst->useInstList)
            {
                std::cout << "\t";
                use.first->emit(std::cout);
                std::cout << "[" << use.first->getLocalId() << "]\n";
            }
#endif
        }
    }
}

// find the location for hoisting the inst pointed to by start
// boundary is the upper limit for hoisting
// if there is any ACC def/use between start and end, return false;
// otherwise, return true.
bool HWConformity::findHoistLocation(
    INST_LIST_ITER start, INST_LIST_ITER &end, uint16_t &movDist, G4_INST *boundary )
{
    bool canMov = true;
    G4_INST *inst = *start;
    end = start;
    end--;
    movDist = 0;

    if ((*end) != boundary)
    {
        // check if src and dst of MAD are re-defined in between and
        // if dst is used in between
        while ((*end) != boundary)
        {
            G4_INST *curInst = *end;
            if (curInst->hasACCOpnd() || curInst->mayExpandToAccMacro(builder))
            {
                canMov = false;
                break;
            }
 
            if (inst->isRAWdep(curInst) ||
                inst->isWAWdep(curInst) ||
                inst->isWARdep(curInst))
            {
                break;
            }
            movDist++;
            --end;
        }

        // check if acc is possibly updated between the new location and boundary
        if (canMov && ((*end) != boundary))
        {
            INST_LIST_ITER in_between_iter = end;
            ++in_between_iter;
            for (; (*in_between_iter) != boundary; --in_between_iter)
            {
                G4_INST *curInst = *in_between_iter;
                if (curInst->hasACCOpnd() || curInst->mayExpandToAccMacro(builder))
                {
                    canMov = false;
                    break;
                }
            }
        }
    }
    return canMov;
}

// for mac code gen we use W as acc type for int since it has enough precision for int
G4_Type HWConformity::getAccType( G4_Type ty )
{
    if( ty == Type_D )
    {
        return Type_W;
    }
    else if( ty == Type_UD )
    {
        return Type_UW;
    }
    else
    {
        return ty;
    }
}
// convert MAD in madList to MAC instructions
// iter is either the next pseudo-mad that does not belong to this list, or the last inst in the BB
// return true if the mad list is converted to mac
bool HWConformity::convertMAD2MAC( INST_LIST_ITER iter, std::vector<G4_INST*> &madList, G4_BB *bb )
{
    if( madList.size() == 1 )
    {
        // there is only one inst in list, it is not a MAD
        return false;
    }

    // find the iterator of the last mad in list
    G4_INST *lastMad = madList.back();
    INST_LIST_ITER movTarget, lastMadIter = iter;
    while ((*lastMadIter) != lastMad)
    {
        lastMadIter--;
    }
    movTarget = lastMadIter;

    bool changeType = false;
    bool dwDst = IS_TYPE_INT(lastMad->getDst()->getType());
    bool twoGRFDst = lastMad->hasNULLDst() ? false :
            ((lastMad->getDst()->getRightBound() - lastMad->getDst()->getLeftBound() + 1) > GENX_GRF_REG_SIZ );
    G4_Type newType = lastMad->getDst()->getType();
    // check if we can convert the type of MAC dst from DW to W,
    // such that we can avoid instruction splitting and improve code quality
    if (dwDst && lastMad->hasNULLDst())
    {
        // is this possible?
        changeType = true;
        lastMad->getDst()->setType( IS_SIGNED_INT( lastMad->getDst()->getType() ) ? Type_W : Type_UW );
    }
    else if( dwDst && twoGRFDst &&
        lastMad->hasOneUse() && 
        !kernel.fg.globalOpndHT.isOpndGlobal(lastMad->getDst()) )
    {
        // last mad has single use, see if we can replace the def-use pair with acc
        G4_INST *useInst = lastMad->use_front().first;
        if( useInst->getDst() &&
            ( IS_BTYPE( useInst->getDst()->getType() ) || IS_WTYPE( useInst->getDst()->getType() ) ) )
        {
            // check the use of last MAD dst
            INST_LIST_ITER useIter = lastMadIter;
            useIter++;
            while( (*useIter) != useInst )
            {
                useIter++;
            }

            uint16_t movDist, hs;
            if( lastMad->canUseACCOpt( false, true, hs, true, true, true ) && hs == 1 &&
                findHoistLocation( useIter, movTarget, movDist, lastMad ) &&
                (*movTarget) == lastMad )
            {
                changeType = true;
                if( movDist > 0 )
                {
                    movTarget++;
                    bb->instList.insert( movTarget, useInst );
                    bb->instList.erase( useIter );
                }
                uint32_t dstStrideSize = G4_Type_Table[useInst->getDst()->getType()].byteSize * useInst->getDst()->getHorzStride();
                uint32_t useTypeSize = G4_Type_Table[Type_UW].byteSize;
                // insert a temp mov
                if( dstStrideSize > useTypeSize )
                {
                    movTarget--;
                    insertMovAfter( movTarget,
                        (uint16_t)( useTypeSize / G4_Type_Table[useInst->getDst()->getType()].byteSize ),
                        bb->instList );
                }

                newType = getAccType( newType );
                // change src of useInst to ACC
                Gen4_Operand_Number srcNum = lastMad->use_front().second;

                ASSERT_USER(useInst->getSrc((uint32_t)srcNum - 1)->isSrcRegRegion(),
                            "Unexpected src to be changed!");

                G4_SrcRegRegion *accSrcOpnd = builder.createSrcRegRegion(
                    useInst->getSrc( (uint32_t)srcNum - 1 )->asSrcRegRegion()->getModifier(),
                    Direct,
                    builder.phyregpool.getAcc0Reg(),
                    0,
                    0,
                    builder.getRegionStride1(),
                    newType );

                useInst->setSrc( accSrcOpnd, (uint32_t)srcNum - 1 );

                // change dst of the last MAD
                G4_DstRegRegion *accDstOpnd = builder.createDstRegRegion(
                                Direct,
                                builder.phyregpool.getAcc0Reg(),
                                0,
                                0,
                                1,
                                newType);

                lastMad->setDest( accDstOpnd );
            }
        }
    }

    // if we can do type demotion or dst fits in 1GRF, we do not have to worry about inst splitting.
    if( !twoGRFDst || changeType )
    {
        // generate MAC directly
        auto madIter = madList.end();
        madIter--;
        G4_INST *curInst = (*madIter);

        G4_Type accType = getAccType(curInst->getSrc(2)->getType());
        uint32_t accTypeSize = getTypeSize(accType);
        // mac dst region has to match that of acc, which is always GRF-aligned
        // we also cannot have acc dst hstride > 4
        if (!builder.isOpndAligned(curInst->getDst(), GENX_GRF_REG_SIZ) || 
            (curInst->getDst()->getExecTypeSize() / accTypeSize) > 4)
        {
            // ToDo: store the iter in madInst?
            auto instIter = std::find(bb->instList.begin(), bb->instList.end(), curInst);
            auto newDst = insertMovAfter(instIter, curInst->getDst(), curInst->getDst()->getType(), bb, Sixteen_Word);
            curInst->setDest(newDst);
        }
        uint32_t dstByteStride = curInst->getDst()->getExecTypeSize();
        uint16_t stride = (uint16_t) (dstByteStride > accTypeSize ? dstByteStride / accTypeSize : 1);
        RegionDesc* region = builder.createRegionDesc(stride, 1, 0);

        G4_SrcRegRegion *accSrcOpnd = builder.createSrcRegRegion(
            Mod_src_undef, Direct, builder.phyregpool.getAcc0Reg(),
            0, 0, region, accType);

        curInst->setImplAccSrc(accSrcOpnd);
        curInst->setSrc(nullptr, 2);
        curInst->setOpcode( G4_mac );
        curInst->fixMACSrc2DefUse();

        do
        {
            // change all intermediate macs to use acc dst and src
            madIter--;
            curInst = (*madIter);
            bool changeSrc = curInst->opcode() == G4_pseudo_mad;
            addACCOpnd(curInst, changeSrc, stride, accType);
        }
        while( madIter != madList.begin() );
        return true;
    }

    // just split them into mul/add
    // assumption: all pseudo_mads from lastMadIter back to the first inst should be on madList

    auto madIter = lastMadIter;
    for (G4_INST* inst = *madIter; inst != madList.front(); inst = *(--madIter))
    {
        if (inst->opcode() == G4_pseudo_mad)
        {
            convertMAD2MulAdd(madIter, bb);
        }
    }   
    return false;
}

void HWConformity::convertComprInstSrcRegion( G4_INST *inst )
{
    for( int k = 0; k < 2; k++ )
    {
        G4_Operand *src = inst->getSrc( k );

        if (!src || src->isImm() || (inst->isMath() && k == 1 && src->isNullReg()))
        {
            continue;
        }

        if (!src->isSrcRegRegion()) {
            continue;
        }

        int w  = src->asSrcRegRegion()->getRegion()->width;
        int hs = src->asSrcRegRegion()->getRegion()->horzStride;
        int vs = src->asSrcRegRegion()->getRegion()->vertStride;

        if( w == 1 && hs == 0 && vs == 0 )
        {
            continue;
        }

        if( inst->getExecSize() < w )
        {
            RegionDesc *rd = builder.createRegionDesc( (uint16_t) (vs/2), (uint16_t) (w/2), (uint16_t) (hs/2) );
            src->asSrcRegRegion()->setRegion( rd );
        }
    }
}

// replace src/dst with ACC
void HWConformity::addACCOpnd(G4_INST *curInst, bool needACCSrc, int dstStride, G4_Type accTy)
{

    if (needACCSrc)
    {
        // change src2 to implicit ACC src.
        RegionDesc* region = nullptr;
        switch (dstStride)
        {
        case 1:
            region = builder.getRegionStride1();
            break;
        case 2:
            region = builder.getRegionStride2();
            break;
        case 4:
            region = builder.getRegionStride4();
            break;
        default:
            MUST_BE_TRUE(false, "unexpected stride value");
            break;
        }

        G4_SrcRegRegion *accSrcOpnd = builder.createSrcRegRegion(
            Mod_src_undef, Direct, builder.phyregpool.getAcc0Reg(),
            0, 0, region, accTy);

        curInst->setImplAccSrc( accSrcOpnd );
        curInst->setSrc( NULL, 2 );
        curInst->setOpcode( G4_mac );
        curInst->fixMACSrc2DefUse();
    }

    // change dst for all in between MAD
    G4_DstRegRegion *accDstOpnd = builder.createDstRegRegion(
        Direct, builder.phyregpool.getAcc0Reg(), 0,
        0, (unsigned short)dstStride, accTy);
    curInst->setDest(accDstOpnd);

}

// convert a psuedo mad inst into mul/add
// return the iterator pointing to add
void HWConformity::convertMAD2MulAdd( INST_LIST_ITER iter, G4_BB *bb )
{
    G4_INST *inst = *iter;
    assert(inst->opcode() == G4_pseudo_mad && "expect pseudo-mad");

    G4_DstRegRegion    *addOpDst     = inst->getDst();
    G4_Operand    *addOpnd2           = inst->getSrc(2);
    G4_Type mulOpDstType = addOpDst->getType();
    G4_Type mulOpExecType = inst->getExecType();
    // pick the widest type of mad's src and dst as the intermediate type
    if (G4_Type_Table[mulOpDstType].byteSize > G4_Type_Table[mulOpExecType].byteSize)
    {
        mulOpExecType = mulOpDstType;
    }

    mulOpDstType = mulOpExecType;

    G4_SubReg_Align     subAlign = Get_G4_SubRegAlign_From_Type( mulOpDstType );

    // Reuse the MAD op for MUL.
    inst->setOpcode(G4_mul);
    inst->setSrc(nullptr, 2);

    G4_Declare* mulDefDcl = builder.createTempVar(inst->getExecSize(), mulOpDstType, 
        G4_Align::Either, subAlign);

    G4_DstRegRegion* mulOpDst = builder.Create_Dst_Opnd_From_Dcl(mulDefDcl, 1);
    inst->setDest(mulOpDst);

    // Follow with an ADD.
    INST_LIST_ITER tIter = iter;
    tIter++;

    auto addOpnd1 = builder.Create_Src_Opnd_From_Dcl(mulDefDcl, builder.getRegionStride1());
    G4_INST* addOp = builder.createInternalInst(
        inst->getPredicate(),
        G4_add,
        inst->getCondMod(),
        inst->getSaturate(),
        inst->getExecSize(),
        addOpDst,
        addOpnd1,
        addOpnd2,
        nullptr,
        inst->getOption(),
        inst->getLineNo(),
        inst->getCISAOff(),
        inst->getSrcFilename() );

    auto addIter = bb->instList.insert( tIter, addOp );

    // predicate/condmod/saturate, if they exist, are propagated to the add instruction
    inst->setSaturate( false );
    inst->setPredicate( NULL );
    inst->setCondMod(nullptr);

    {
        inst->transferDef( addOp, Opnd_src2, Opnd_src1 );
        if( addOp->getPredicate() )
        {
            inst->transferDef( addOp, Opnd_pred, Opnd_pred );
        }
        inst->transferUse( addOp );
        inst->addDefUse(addOp, Opnd_src0);
    }
}

// See if we can convert the pseudo_sada2 instruction into an actual Gen sada2
// This can be done if the following conditions are met:
// -- We can find the definition of the pseudo sada2 instruction's source 2 in
//    the same basic block, and that
// -- it may be replaced by an acc (i.e., the src2 is its only use, the dst and
//    the src have identical regions, and there are no intervening instructions
//    that update acc)
//
// We additionally attempt to schedule up the sada2 instruction to be as close
// as possible to the src2 defining instruction (subject to the constraints of
// def-use chains for def, src0 and src1), so that more opportunites may be
// exposed for later sada2 instructions

void HWConformity::fixSADA2Inst( BB_LIST_ITER it )
{
    G4_BB* bb = *it;

    INST_LIST_ITER i = bb->instList.begin();
    while (i != bb->instList.end())
    {

        G4_INST *inst = *i;
        if( inst->opcode() != G4_pseudo_sada2 )
        {
            ++i;
            continue;
        }

        G4_Operand *src2 = inst->getSrc(2);

        bool canDoSada2 = true;
        G4_INST* src2Dst = NULL;

        int emask = inst->getMaskOption();
        if (bb->isInSimdFlow() &&
            emask != InstOpt_WriteEnable &&
            inst->getMaskOffset() != 0)
        {
            canDoSada2 = false;
        }

        G4_DstRegRegion *dst = inst->getDst();
        if( canDoSada2 )
        {
            if( src2->isSrcRegRegion() && src2->asSrcRegRegion()->getRegAccess() == Direct )
            {
                // check Src2
                if( kernel.fg.globalOpndHT.isOpndGlobal(src2 ) )
                {
                    // no sada2 if operand is global
                    canDoSada2 = false;
                }
                else if( src2->asSrcRegRegion()->getModifier() != Mod_src_undef )
                {
                    // no sada2 if src2 has a modifier
                    canDoSada2 = false;
                }
                else
                {
                    for (auto defIter = inst->def_begin(); defIter != inst->def_end(); ++defIter)
                    {
                        if((*defIter).second == Opnd_src2 )
                        {
                            if( src2Dst != NULL )
                            {
                                // no sada2 if src2 has >1 definition
                                canDoSada2 = false;
                                break;
                            }
                            src2Dst = (*defIter).first;
                        }
                    }

                    if( !src2Dst )
                    {
                        canDoSada2 = false;
                    }
                    else
                    {
                        if( !src2Dst->hasOneUse() )
                        {
                            // no sad2 if def has more than one use
                            canDoSada2 = false;
                        }
                        else
                        {
                            G4_DstRegRegion *src2DstOpnd = src2Dst->getDst();
                            G4_Type src2DstType = src2DstOpnd->getType();
                            if( src2DstOpnd->getRegAccess() != Direct
                                || (src2DstType != Type_W && src2DstType != Type_UW) )
                            {
                                // no sada2 if def's dst is indirect, or it type is not W or UW
                                canDoSada2 = false;
                            }
                            else if( src2DstOpnd->compareOperand( src2 ) !=
                                Rel_eq )
                            {
                                // no sada2 if src2Dst and src2 are not equal
                                canDoSada2 = false;
                            }
                        }
                    }
                }
            }
            else
            {
                canDoSada2 = false;
            }
        }

        // The new location of the sada2 after the conversion
        INST_LIST_ITER newSada2Iter = i;
        --newSada2Iter;
        if( canDoSada2 )
        {
            // try to schedule up the sada2 to be as close to the src2-defining instruction
            // as possible to expose more optmizaition opportunities
            for(; *newSada2Iter != src2Dst; --newSada2Iter )
            {
                if( inst->isRAWdep( *newSada2Iter ) ||
                    inst->isWAWdep( *newSada2Iter ) ||
                    inst->isWARdep( *newSada2Iter ) )
                {
                    break;
                }
            }

            // make sure there are no instructions between the sada2's new location
            // and the src2-defining instruction that updates acc
            for( INST_LIST_ITER iter = newSada2Iter; *iter != src2Dst; --iter )
            {
                G4_INST* aInst = *iter;
                if( aInst->isAccDstInst() || aInst->isAccWrCtrlInst() ||
                    ( aInst->opcode() == G4_mulh &&
                    IS_DTYPE(aInst->getSrc(0)->getType()) && IS_DTYPE(aInst->getSrc(1)->getType()) ) )
                {
                    canDoSada2 = false;
                    break;
                }
            }
        }

        if( canDoSada2 )
        {
            // We have verified all conditions and can convert this instruction to sada2.
            // replace the destination for src2Dst to be acc0.
            // The actual acc0 offset will be fixed in a later pass
            G4_DstRegRegion *accDstOpnd = builder.createDstRegRegion(
                Direct,
                builder.phyregpool.getAcc0Reg(),
                0,
                0,
                1,
                src2->getType());
            src2Dst->setDest( accDstOpnd );

            // create an implicit acc parameter for sada2
            inst->setOpcode( G4_sada2 );
            inst->setSrc( NULL, 2 );
            G4_SrcRegRegion *accSrcOpnd = builder.createSrcRegRegion(
                Mod_src_undef,
                Direct,
                builder.phyregpool.getAcc0Reg(),
                0,
                0,
                builder.getRegionStride1(),
                src2->getType());

            inst->setImplAccSrc( accSrcOpnd );

            ++newSada2Iter;
            bb->instList.insert( newSada2Iter, inst );
            i = bb->instList.erase(i);

            // maintain def-use

            for (auto tmpIter = src2Dst->use_begin(); tmpIter != src2Dst->use_end(); ++tmpIter)
            {
                if( (*tmpIter).first == inst && (*tmpIter).second == Opnd_src2 )
                {
                    (*tmpIter).second = Opnd_implAccSrc;
                    break;
                }
            }

            for (auto tmpIter = inst->def_begin(); tmpIter != inst->def_end(); ++tmpIter)
            {
                if( (*tmpIter).first == src2Dst && (*tmpIter).second == Opnd_src2 )
                {
                    (*tmpIter).second = Opnd_implAccSrc;
                    break;
                }
            }
        }
        else
        {
            // pseudo_sada2 (N) dst src0 src1 src2
            // becomes
            // sad2 (n) tmp<1>:w src0 src1
            // add (n) dst tmp<n;n,1>:w src2

            inst->setOpcode( G4_sad2 );
            inst->setSrc( NULL, 2 );

            G4_Align sad2TmpAlign = Either;
            G4_SubReg_Align sad2TmpSubAlign = Get_G4_SubRegAlign_From_Type( dst->getType() );

            if( inst->getExecSize() * G4_Type_Table[dst->getType()].byteSize > GENX_GRF_REG_SIZ )
            {
                // align to GRF
                sad2TmpSubAlign = Sixteen_Word;
            }
            // create a new temp variable as sad2's destination
            G4_Declare* sad2Tmp = builder.createTempVar( inst->getExecSize(), dst->getType(),
                sad2TmpAlign, sad2TmpSubAlign );
            G4_DstRegRegion* sad2Dst = builder.Create_Dst_Opnd_From_Dcl(sad2Tmp, 1);
            inst->setDest( sad2Dst );

            uint16_t srcVertStride, srcWidth, srcHorzStride;
            srcWidth = inst->getExecSize() > 8 ? 8 : inst->getExecSize();
            srcHorzStride = 1;
            srcVertStride = srcWidth;

            // opnd 0 for add is the new temp we've just created
            RegionDesc *rd = builder.createRegionDesc( srcVertStride, srcWidth, srcHorzStride );
            G4_Operand* addSrc0Opnd = builder.createSrcRegRegion(Mod_src_undef, Direct, sad2Dst->getBase(),
                0, 0, rd, sad2Dst->getType() );

            // opnd 1 is src2 of the pseudo_sada2
            // dst is the same as the pseudo_sada2
            G4_INST* addInst = builder.createInternalInst(
                inst->getPredicate(),
                G4_add,
                inst->getCondMod(),
                inst->getSaturate(),
                inst->getExecSize(),
                dst,
                addSrc0Opnd,
                src2,
                NULL,
                inst->getOption(),
                inst->getLineNo(),
                inst->getCISAOff(),
                inst->getSrcFilename() );

            INST_LIST_ITER addLoc = i;
            ++addLoc;
            bb->instList.insert( addLoc, addInst );

            // FIXME: redundant?
            inst->addDefUse(addInst, Opnd_src0);

            // The sad2 op should not have the SAT attribute set,
            // as this is intended only for the final result of the
            // SADA2 (and thus the add op will keep the SAT attribute).
            inst->setSaturate( false );
            inst->setPredicate( NULL );

            {
                inst->transferDef( addInst, Opnd_src2, Opnd_src1 );
                if( addInst->getPredicate() )
                {
                    inst->transferDef( addInst, Opnd_pred, Opnd_pred );
                }
                inst->transferUse( addInst );
                inst->addDefUse(addInst, Opnd_src0);
            }
            ++i;
        }
    }
}

void HWConformity::fixSendInst(BB_LIST_ITER it)
{
    G4_BB* bb = *it;

    for (INST_LIST_ITER i = bb->instList.begin(); i != bb->instList.end(); i++)
    {

        G4_INST *inst = *i;
        if (!inst->isSend())
        {
            continue;
        }

        if (inst->getExecSize() < 8)
        {
            // A64 messages require a minimum msg len of two for address (src0), which is inconsistent
            // with our input IR as it allows <2 GRF address variables (e.g., simd1 A64 scatter r/w). 
            // To avoid this causing overlap between send dst/src0/src1 (it is known to cause HW hang), 
            // we have to ensure they are all 2GRF-aligned
            G4_Declare* src0Dcl = inst->getSrc(0)->getTopDcl();
            // ToDo: check if dst/src1 may also exhibit such size mismatch
            bool sizeMismatch = inst->getMsgDesc()->MessageLength() == 2 &&
                (src0Dcl && src0Dcl->getRootDeclare()->getByteSize() < 2 * GENX_GRF_REG_SIZ);
            auto doEvenAlign = [](G4_Declare* dcl)
            {               
                if (dcl)
                {
                    dcl = dcl->getRootDeclare();
                    // variables >= 2 GRF don't need even alignment since they can't possibly overlap
                    if (dcl->getByteSize() < 2 * GENX_GRF_REG_SIZ)
                    {
                        dcl->setAlign(G4_Align::Even);
                    }
                }  
            };
            if (sizeMismatch)
            {
                doEvenAlign(inst->getSrc(0)->getTopDcl());
                if (inst->isSplitSend())
                {
                    doEvenAlign(inst->getSrc(1)->getTopDcl());
                }
                if (VISA_WA_CHECK(builder.getPWaTable(), WaDisableSendSrcDstOverlap))
                {
                    doEvenAlign(inst->getDst()->getTopDcl());
                }
            }
        }

        uint16_t offset = 0;
        if (!builder.isOpndAligned(inst->getDst(), offset, GENX_GRF_REG_SIZ))
        {
            inst->setDest(insertMovAfter(i, inst->getDst(), inst->getDst()->getType(), bb, Sixteen_Word));
        }

        G4_Operand *src0 = inst->getSrc(0);
        G4_Declare *src0TopDcl = src0->getTopDcl();

        // if src0 and src1 are hard-wired GRF, check that 
        // they satisfy EOT and preemption restrictions
        auto needsTempSrc = [this](G4_INST* inst, G4_Declare* dcl)
        {
            return dcl->getRegVar() && dcl->getRegVar()->getPhyReg() &&
                ((inst->isEOT() && builder.hasEOTGRFBinding() &&
                    dcl->getRegVar()->getPhyReg()->asGreg()->getRegNum() < 112) ||
                    (builder.getOption(vISA_enablePreemption) &&
                        dcl->getRegVar()->getPhyReg()->asGreg()->getRegNum() < 2));
        };

        if (needsTempSrc(inst, src0TopDcl))
        {
            uint16_t rows = inst->getMsgDesc()->MessageLength();
            G4_Type type = src0->getType();
            G4_Declare* dcl = builder.createTempVar(rows * 8, type, Either, Sixteen_Word);

            MUST_BE_TRUE(G4_Type_Table[type].byteSize == 4, "Invalid src0 opnd type for send.");

            RegionDesc* region = builder.getRegionStride1();
            G4_VarBase *base = src0->asSrcRegRegion()->getBase();
            short baseOff = src0->asSrcRegRegion()->getRegOff();
            short baseSubOff = src0->asSrcRegRegion()->getSubRegOff();
            for (uint16_t idx = 0; idx != rows; ++idx) {
                G4_SrcRegRegion *src = builder.createSrcRegRegion(Mod_src_undef, Direct, base, baseOff + idx, baseSubOff + 0, region, type);
                G4_DstRegRegion* dst = builder.createDstRegRegion(Direct, dcl->getRegVar(), idx, 0, 1, type);

                G4_INST* newInst = builder.createInternalInst(NULL, G4_mov, NULL, false,
                    8, dst, src, NULL, InstOpt_WriteEnable,
                    inst->getLineNo(), inst->getCISAOff(), inst->getSrcFilename());

                bb->instList.insert(i, newInst);
                inst->transferDef(newInst, Opnd_src0, Opnd_src0);
                newInst->addDefUse(inst, Opnd_src0);
            }

            G4_Operand *newSrc = builder.Create_Src_Opnd_From_Dcl(dcl, builder.getRegionStride1());
            inst->setSrc(newSrc, 0);
        }

        if (inst->isSplitSend() && !inst->getSrc(1)->isNullReg())
        {
            // src1 may be null because some messages (e.g., CPS) require split send
            if (!builder.isOpndAligned(inst->getSrc(1), GENX_GRF_REG_SIZ))
            {
                inst->setSrc(insertMovBefore(i, 1, inst->getSrc(1)->getType(), bb, Sixteen_Word), 1);
            }
            G4_Operand *src1 = inst->getSrc(1);
            G4_Declare *src1TopDcl = src1->getTopDcl();

            if (needsTempSrc(inst, src1TopDcl))
            {
                uint16_t rows = inst->getMsgDesc()->extMessageLength();
                G4_Type type = src1->getType();
                G4_Declare* dcl = builder.createTempVar(rows * 8, type, Either, Sixteen_Word);

                MUST_BE_TRUE(G4_Type_Table[type].byteSize == 4, "Invalid src1 opnd type for send.");

                RegionDesc* region = builder.getRegionStride1();
                G4_VarBase *base = src1->asSrcRegRegion()->getBase();
                short baseOff = src1->asSrcRegRegion()->getRegOff();
                short baseSubOff = src1->asSrcRegRegion()->getSubRegOff();
                for (uint16_t idx = 0; idx != rows; ++idx)
                {
                    G4_SrcRegRegion *src = builder.createSrcRegRegion(Mod_src_undef, Direct, base, baseOff + idx, baseSubOff + 0, region, type);
                    G4_DstRegRegion* dst = builder.createDstRegRegion(Direct, dcl->getRegVar(), idx, 0, 1, type);

                    G4_INST* newInst = builder.createInternalInst(NULL, G4_mov, NULL, false,
                        8, dst, src, NULL, InstOpt_WriteEnable,
                        inst->getLineNo(), inst->getCISAOff(), inst->getSrcFilename());

                    bb->instList.insert(i, newInst);
                    inst->transferDef(newInst, Opnd_src1, Opnd_src1);
                    newInst->addDefUse(inst, Opnd_src1);
                }

                G4_Operand *newSrc = builder.Create_Src_Opnd_From_Dcl(dcl, region);
                inst->setSrc(newSrc, 1);
            }
        }

        if (builder.getOption(vISA_enablePreemption))
        {
            G4_DstRegRegion *dst = inst->getDst();
            if (!dst->isNullReg())
            {
                G4_Declare *dstTopDcl = dst->getTopDcl();
                if (dstTopDcl != NULL &&
                    dstTopDcl->getRegVar() &&
                    dstTopDcl->getRegVar()->getPhyReg())
                {
                    MUST_BE_TRUE((dstTopDcl->getRegVar()->getPhyReg()->asGreg()->getRegNum() > 2), "Unexpected preg used for send destination.");
                }
            }
        }

        if (VISA_WA_CHECK(builder.getPWaTable(), WaDisableSendSrcDstOverlap))
        {
            // create copy if dst and src0/src1 overlap due to being the same variable
            bool src0Overlap = inst->getDst()->compareOperand(inst->getSrc(0)) != Rel_disjoint;
            bool src1Overlap = inst->isSplitSend() && inst->getDst()->compareOperand(inst->getSrc(1)) != Rel_disjoint;
            if (src0Overlap || src1Overlap)
            {
                int dstSize = inst->getMsgDesc()->ResponseLength();
                int src0Size = src0Overlap ? inst->getMsgDesc()->MessageLength() : 0;
                int src1Size = src1Overlap ? inst->getMsgDesc()->extMessageLength() : 0;
                if (dstSize > src0Size + src1Size)
                {
                    //copy src0/src1
                    if (src0Overlap)
                    {
                        G4_Declare* copyDst = builder.createTempVar(src0Size * 8, Type_UD, Either, Any);
                        copyRegs(copyDst, 0, inst->getSrc(0)->getBase()->asRegVar()->getDeclare(),
                            inst->getSrc(0)->asSrcRegRegion()->getRegOff() * 32, src0Size, bb, i);
                        inst->setSrc(builder.Create_Src_Opnd_From_Dcl(copyDst, builder.getRegionStride1()), 0);
                    }
                    if (src1Overlap)
                    {
                        G4_Declare* copyDst = builder.createTempVar(src1Size * 8, Type_UD, Either, Any);
                        copyRegs(copyDst, 0, inst->getSrc(1)->getBase()->asRegVar()->getDeclare(),
                            inst->getSrc(1)->asSrcRegRegion()->getRegOff() * 32, src1Size, bb, i);
                        inst->setSrc(builder.Create_Src_Opnd_From_Dcl(copyDst, builder.getRegionStride1()), 1);
                    }     
                }
                else
                {
                    // copy dst
                    auto copyIter = i;
                    ++copyIter;
                    G4_Declare* copySrc = builder.createTempVar(dstSize * 8, Type_UD, Either, Any);
                    copyRegs(inst->getDst()->getBase()->asRegVar()->getDeclare(), inst->getDst()->getRegOff() * 32,
                        copySrc, 0, dstSize, bb, copyIter);
                    inst->setDest(builder.Create_Dst_Opnd_From_Dcl(copySrc, 1));
                }
            }
        }

    }

}

//
// Fix sel and csel instructions:
//  -- set their cond mod to null as they don't modify it.  They will be hard-coded to f0.0 in Gen asm

void HWConformity::fixSelCsel(INST_LIST_ITER it, G4_BB* bb)
{
    G4_INST* inst = *it;
    if (inst->opcode() == G4_sel || inst->opcode() == G4_csel)
    {
        G4_CondMod *condMod = inst->getCondMod();
        if (condMod)
        {
            condMod->setBase(nullptr);
        }
    }
}

void HWConformity::conformBB( BB_LIST_ITER it)
{
    G4_BB *bb = *it;
    INST_LIST_ITER i = bb->instList.begin(), iEnd = bb->instList.end();
    INST_LIST_ITER next_iter = i;
    for ( ; i != iEnd; i = next_iter )
    {
        // by default we skip the newly inserted instructions as we assume they are already HW conformed
        // if a check may produce new instructions that violate HW rules, it must adjust the next_iter
        // to point to them
        ++next_iter;
        G4_INST *inst = *i;
        G4_opcode opcode = inst->opcode();
        if (opcode == G4_nop || opcode == G4_label)
        {
            continue;
        }

        // do this early since otherwise the moves inserted by other passes may still
        // inherit bad regions from the original inst
        fixSrcRegion(inst);

        bool changed = fixMov(i, bb);
        if (changed)
        {
            next_iter = i;
            next_iter++;
        }

        fixOpndType(i, bb);

        fixSelCsel(i, bb);

        if (inst->getExecSize() == 16)
        {
            if (inst->opcode() == G4_math               &&
                inst->getDst()->getType() == Type_HF    &&
                inst->getSrc(0)->getType() == Type_HF &&
                (!inst->getSrc(1) || inst->getSrc(1)->getType() == Type_HF))
            {
                // split pure HF math to simd8
                evenlySplitInst(i, bb);
            }
        }
        fix3SrcInst(i, bb);

        G4_Operand *dst = inst->getDst();

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif

         /* HW Check #2
         * First check sources for math instructions.
         * math only uses GRFs as operands and sub register number should be the same
         */
        if (inst->isMath())
        {
            if( fixMathInst( i, bb ) )
            {
                // check the newly added insts later
                next_iter = i;
                next_iter++;
            }
        }

        inst = *i;

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif
        /* HW Check #3 */
        if( inst->opcode() == G4_mul )
        {
            if( fixMULInst( i, bb ) )
            {
                // inserted mach and mov
                // check the newly added insts later ( MUL, MACH, MOV )
                next_iter = i;
                next_iter++;
            }
        }

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif
        /* HW Check #3a */
        if( inst->opcode() == G4_mulh )
        {
            fixMULHInst( i, bb );
            // inserted mul before
            // check the newly added MUL inst
            i--;
            next_iter = i;
            continue;
        }

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif

        // HW check #6: indirect operand spilling
        fixIndirectOpnd( i, bb );

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif
        // HW check #8: unsigned dst with execution type F
        /* If the execution type is F and the destination type if either UD, UW
         * or UB and the detination is not saturated, then we need to add an
         * intermediate type conversion to D.
         */
        inst = *i;
        opcode = inst->opcode();

        if (opcode == G4_cmp || opcode == G4_cmpn)
        {
            dst = inst->getDst();
            int dst_elsize = 0;
            bool null_dst = !dst || inst->hasNULLDst();
            if (!null_dst)
            {
                dst_elsize = dst->isPredicate() ? G4_Type_Table[Type_UW].byteSize : G4_Type_Table[dst->getType()].byteSize;
            }
            int extypesize;
            G4_Type extype = inst->getOpExecType( extypesize );
            fixCompareInst( i, bb, extype, dst_elsize );
        }
        dst = inst->getDst();
#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif
        {
            int extypesize;
            G4_Type extype = inst->getOpExecType( extypesize );
            /*
            * HW check #11: check destination type.
            *
            * (*) When source(s) is/are of float type, destination must be of float
            *     type also. The exception is MOV instruction which can be used
            *     for explicit type conversion between float and integer.
            */
            if (dst != NULL &&
                ( ( opcode != G4_mov && IS_FTYPE( extype ) && !IS_FTYPE( dst->getType() ) ) ||
                ( IS_FTYPE(dst->getType()) && !IS_FTYPE(extype) && !Opcode_int_src_float_dst_OK( opcode ) ) ) )
            {
                if(fixDstType( i, bb, extype ))
                {
                    next_iter = i;
                    next_iter++;
                }
            }
        }

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif
        if (fixImplicitAcc(i, bb))
        {
            next_iter = i;
            next_iter++;
        }

        if (fixAccSrc(i, bb))
        {
            next_iter = i;
            next_iter++;
        }

        /* HW check #13: check acc source */
        if ( (dst != NULL && dst->isAccReg()) || opcode == G4_mach )
        {
            if( fixAccDst( i, bb ) )
            {
                // TODO: we should fix inst with ACC src separately?
                next_iter = i;
                next_iter++;
            }
        }

        {
            dst = inst->getDst();
            G4_Type extype = inst->getExecType2();
            int extypesize = G4_Type_Table[extype].byteSize;
            int dst_elsize = 0;
            if (dst)
            {
                dst_elsize = G4_Type_Table[dst->getType()].byteSize;
            }
#ifdef _DEBUG
            verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif
            /* HW check #15 : DST HS */
            if (dst                         &&
                inst->getExecSize() == 1    &&
                dst_elsize < extypesize     &&
                !IS_VTYPE(extype)           &&
                !inst->isMixedMode())
            {
                fixDstHstride( i, extypesize );
            }
        }

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif

        bool planeDeleted = fixPlaneInst(i, bb);
        if (planeDeleted)
        {
            continue;
        }

        fixLine(i, bb);

        // CHV/BXT specific checks for 64b datatypes
        fix64bInst( i, bb);

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif
        fixImm64( i, bb ); // fixed immediates for DF4 in fixImm64()

        // FIXME: may be better to call fixDstAlign instead
        if (getGenxPlatform() == GENX_BDW)
        {
            fixPackedHFConversions(i, bb);
        }
    }
}

//
// SIMD16 addc/subb are illegal on GEN, since they write to acc and there are only 8 acc
// channels for D/UD type.  In vISA IR we should get something like
// addc (16) V0 V2 V3
// mov  (16) V1 acc0<8;8,1>:ud
// which needs to be translated to
// addc (8) V0(0) V2(0) V3(0) {Q1}
// mov (8) V1(0) acc0<8;8,1>:ud {Q1}
// addc (8) V0(1) V2(1) V3(1) {Q2}
// mov (8) V1(1) acc0<8;8,1>:ud {Q2}
//
// We do this first thing in HW conformity to avoid REXES from splitting addc/subb incorrectly
// We also count on previous opt to preserve the inst pair by not inserting any acc using inst in between;
// it should hopefully be the case since we generally don't optimize instructions with acc src/dst
//
// If exec size of addc is < 8, we also have to make sure both the addc's dst and the carry move's dst are
// GRF-aligned, since acc's channel is dependent on the dst's subreg offset.  In other words, we fix
// addc (1) r1.0 ...
// mov (1) r1.1 acc0.0<0;1,0>
// into
// addc (1) r1.0 ...
// mov (1) r2.0 acc0.0<0;1,0>
// mov (1) r1.1 r2.0
//
//
bool HWConformity::fixAddcSubb(G4_BB* bb)
{
    bool changed = false;
    for (auto iter = bb->instList.begin(), iterEnd = bb->instList.end();
        iter != iterEnd; ++iter)
    {
        G4_INST* inst = *iter;
        if ((inst->opcode() == G4_addc || inst->opcode() == G4_subb) &&
            inst->getExecSize() != 8)
        {
            // find the matching carry move
            G4_INST* carryMov = NULL;
            auto movIter = iter;
            for (++movIter; movIter != iterEnd; ++movIter)
            {
                G4_INST* inst2 = *movIter;
                if (inst2->opcode() == G4_mov && inst2->getExecSize() == inst->getExecSize() &&
                    inst2->getSrc(0)->isAccReg() && inst2->getSrc(0)->getType() == Type_UD)
                {
                    carryMov = inst2;
                    break;
                }
                else if (inst2->useAcc())
                {
                    break;
                }
            }

            if (carryMov == NULL)
            {
                // can't find the move using acc, skip this addc/subb
                continue;
            }

            if (inst->getExecSize() == 16)
            {
                evenlySplitInst(iter, bb);
                evenlySplitInst(movIter, bb);

                // movIter now points to the second half of move, and we want to move the first move to be
                // before the second half of the addc/subb, which is pointed by iter
                --movIter;
                G4_INST* mov1 = *movIter;
                bb->instList.erase(movIter);
                bb->instList.insert(iter, mov1);

                changed = true;
            }
            else
            {
                // we will need to GRF-align addc's dst as well as the move dst,
                // so that the acc will have the correct offset
                // note that insertMovAfter will align the tmp since addc/subb has implicit acc use
                if (!builder.isOpndAligned(inst->getDst(), 32))
                {
                    inst->setDest(
                        insertMovAfter(iter, inst->getDst(), inst->getDst()->getType(), bb));
                    changed = true;
                }
                if (!builder.isOpndAligned(carryMov->getDst(), 32))
                {
                    carryMov->setDest(
                        insertMovAfter(movIter, carryMov->getDst(), carryMov->getDst()->getType(), bb));
                    changed = true;
                }
            }
        }
    }
    return changed;
}

void HWConformity::chkHWConformity()
{
    fixDataLayout();

    for (BB_LIST_ITER it = kernel.fg.BBs.begin(); it != kernel.fg.BBs.end();it++)
    {
        // hw conformity #1
#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif
        fixAddcSubb(*it);
#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif

        fixMADInst( it );

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif
        // fix source operand first to avoid redundant MOVs if this fix is done after
        // reducing execution size.
        // used by 3d. Mainly to fix sel with two imm sources
        fixOpndTypeAlign( *it );

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif

        if (builder.getOption(vISA_accSubstitution) && 
            !builder.getOption(vISA_doAccSubAfterSchedule))
        {
            accSubstitution(*it);
        }

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif

        fixInstExecSize( it );

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif

        fixMixedHFInst( it );

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif
        fixSADA2Inst( it );

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif

        fixSendInst( it );

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif

        conformBB(it);

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif

#ifdef _DEBUG
        verifyG4Kernel(kernel, Optimizer::PI_HWConformityChk, false);
#endif
    }
}

bool HWConformity::hasBadRegion( G4_INST *inst )
{
    if( inst->getImplAccDst() || inst->getImplAccSrc() )
        return false;
    bool badRegion = false;
    for( unsigned int srcNum = 0; srcNum < G4_Inst_Table[inst->opcode()].n_srcs; srcNum++ )
    {
        if( !(inst->getSrc(srcNum)->isSrcRegRegion()) )
        {
            continue;
        }
        RegionDesc *rd = inst->getSrc(srcNum)->asSrcRegRegion()->getRegion();
        if( rd->isRegionWH() )
        {
            badRegion = true;
            break;
        }
        if( rd->horzStride == GENX_MAX_H_STRIDE && rd->width > 1 )
        {
            badRegion = true;
            break;
        }
        G4_SrcRegRegion *expandSrcRegion = inst->getSrc(srcNum)->asSrcRegRegion();
        if( expandSrcRegion->getRegAccess() != Direct )
        {
            RegionDesc* origRegion = expandSrcRegion->getRegion();
            short secondSubRegOffDiff = 0, secondAddrImmedDiff = 0;

            if( origRegion->width == 1 )
            {
                secondSubRegOffDiff = origRegion->vertStride;
            }
            else
            {
                secondSubRegOffDiff = origRegion->horzStride;
            }
            secondAddrImmedDiff = (short) (secondSubRegOffDiff * G4_Type_Table[expandSrcRegion->getType()].byteSize);
            if( (expandSrcRegion->getAddrImm() + secondAddrImmedDiff) > G4_MAX_ADDR_IMM )
            {
                badRegion = true;
                break;
            }
        }
    }
    return badRegion;
}
// check if we can split an inst
bool HWConformity::canSplitInst( G4_INST *inst, G4_INST *use_op )
{
    if( ( inst->getPredicate() && inst->getExecSize() < 16 ) || hasBadRegion( inst ) )
        return false;

    bool condModIsUsed = false;

    // make sure cond mod is only used by use_op
    G4_CondMod *condMod = inst->getCondMod();
    if( condMod )
    {
        for (auto use_iter = inst->use_begin(); use_iter != inst->use_end(); use_iter++)
        {
            if( (*use_iter).first == use_op )
            {
                if( (*use_iter).second != Opnd_pred )
                {
                    condModIsUsed = true;
                    break;
                }
                continue;
            }
             G4_CmpRelation rel = Rel_disjoint;
             if( (*use_iter).second == Opnd_pred )
             {
                 rel = condMod->compareOperand( (*use_iter).first->getPredicate() );
             }
             else if( (*use_iter).second == Opnd_dst )
             {
                 G4_Operand *use = (*use_iter).first->getDst();
                 if( use->isFlag() )
                     rel = condMod->compareOperand( use );
             }
             else
             {
                 G4_Operand *use = (*use_iter).first->getSrc( (*use_iter).second - 1 );
                 if( use->isFlag() )
                     rel = condMod->compareOperand( use );
             }

             if( rel != Rel_disjoint )
             {
                 condModIsUsed = true;
                 break;
             }
        }
    }
    if( condModIsUsed )
    {
        return false;
    }

    for (int i = 0; i < inst->getNumSrc(); i++)
    {
        G4_Operand *src = inst->getSrc(i);
        if (src->isAccReg())
        {
            // don't split inst with explicit acc
            return false;
        }
    }

    return true;
}

bool HWConformity::canSplitByteDst( G4_opcode op )
{
    switch( op )
    {
    case G4_mac:
    case G4_mach:
    case G4_cmp:
    case G4_mad:
    case G4_sad2:
    case G4_sada2:
    case G4_line:
    case G4_send:
    case G4_sendc:
        return false;
    default:
        return true;
    }
}
// split one instruction into 2 if its dstination is packed byte and execution type is W.
// for example:
// add <16> V1(0,0)<1>:b V1(0,0)<16;16,1>:w V2(0,0)<16;16,1>:w
// ==>
// add <8> V1(0,0)<2>:b V1(0,0)<16;8,2>:w V2(0,0)<16;8,2>:w
// add <8> V1(0,1)<2>:b V1(0,1)<16;8,2>:w V2(0,1)<16;8,2>:w

// if predicate is used for instruction, the definition of this predicate is tracked and the
// corresponding instruction is checked to see if it can do the same split.
bool HWConformity::splitInstListForByteDst( INST_LIST_ITER it, G4_BB *bb, uint16_t extypesize )
{
    G4_INST *inst = *it;
    G4_opcode inst_op = inst->opcode();
    G4_DstRegRegion *dst = inst->getDst();
    // check if we can split the inst
    if( !canSplitByteDst( inst_op ) ||
         inst->getExecSize() == 1 ||
        ( bb->isInSimdFlow() && !inst->isWriteEnableInst() ) ||
        dst->getByteOffset() % extypesize != 0 ||
        dst->getHorzStride() != 1 ||
        extypesize != G4_Type_Table[Type_W].byteSize)
    {
        return false;
    }

    if (inst->getPredicate() || inst->getCondMod())
    {
        return false;
    }

    // recursively the inst that defines its predicate can be split
    INST_LIST expandOpList;
    bool canSplit = canSplitInst( inst, NULL );
    if( canSplit )
    {
        expandOpList.push_back( inst );
    }

    G4_INST *currInst = inst;
    while( canSplit && currInst->getPredicate() )
    {
        // look for predicate def inst
        uint16_t defNum = 0;
        G4_INST *defInst = NULL;

        // FIXME: should be currInst->defInstList.begin()?
        for (auto def_iter = inst->def_begin(); def_iter != inst->def_end(); def_iter++)
        {
            if( (*def_iter).second == Opnd_pred )
            {
                defNum++;
                defInst = (*def_iter).first;
            }
        }
        if( defNum != 1 || !defInst->getCondMod() )
        {
            canSplit = false;
            break;
        }
        if( canSplit )
        {
            if( bb->isInSimdFlow() && !defInst->isWriteEnableInst() )
            {
                canSplit = false;
            }
            else
            {
                canSplit = canSplitInst( defInst, currInst );
            }
        }
        // check if def inst can be split
        if( !canSplit )
        {
            break;
        }
        else
        {
            expandOpList.push_back( defInst );
            currInst = defInst;
        }
    }

    // split inst into two
    INST_LIST_ITER new_iter = it;
    new_iter++;
    if( canSplit )
    {
        while( !expandOpList.empty() )
        {
            G4_INST *expand_op = expandOpList.front();
            expandOpList.pop_front();
            // find location of expand_op in instruction list
            do
            {
                new_iter--;
                if( (*new_iter) == expand_op )
                {
                    break;
                }
            }while( new_iter != bb->instList.begin() );

            MUST_BE_TRUE( new_iter != bb->instList.end(), "Cannot find predicate definition function in BB." );
            new_iter++;
            G4_INST *secondHalfOp = splitInstWithByteDst( expand_op );
            MUST_BE_TRUE( secondHalfOp, "Error in spliting instruction." );
            bb->instList.insert( new_iter, secondHalfOp );
        }
    }


    return canSplit;
}

G4_INST* HWConformity::splitInstWithByteDst( G4_INST *expand_op )
{
    unsigned char newExecSize = expand_op->getExecSize()/2;
    if( expand_op->getPredicate() )
    {
        expand_op->getPredicate()->splitPred();
    }
    if( expand_op->getCondMod() )
    {
        expand_op->getCondMod()->splitCondMod();
    }
    G4_INST *expand_sec_half_op = builder.createInternalInst(
        (G4_Predicate *)builder.duplicateOperand( expand_op->getPredicate() ),
        expand_op->opcode(),
        (G4_CondMod *)builder.duplicateOperand( expand_op->getCondMod() ),
        expand_op->getSaturate(),
        newExecSize,
        NULL,
        NULL,
        NULL,
        NULL,
        expand_op->getOption(),
        expand_op->getLineNo(),
        expand_op->getCISAOff(),
        expand_op->getSrcFilename() );
    MUST_BE_TRUE( expand_sec_half_op != NULL, ERROR_MEM_ALLOC );

    expand_op->setExecSize( newExecSize );

    if( expand_op->getDst() && !expand_op->hasNULLDst() )
    {
        G4_DstRegRegion *old_dst = expand_op->getDst();
        short secondSubRegOff = old_dst->getSubRegOff() + 1;

        G4_DstRegRegion *newDstOpnd = builder.createDstRegRegion(
            old_dst->getRegAccess(),
            old_dst->getBase(),
            old_dst->getRegOff(),
            old_dst->getSubRegOff(),
            old_dst->getHorzStride() * 2,
            old_dst->getType() );
        if( old_dst->getRegAccess() != Direct )
        {
            newDstOpnd->setImmAddrOff(old_dst->getAddrImm() );
            secondSubRegOff -= 1;
        }

        expand_op->setDest( newDstOpnd );

        G4_DstRegRegion *secondDstOpnd = builder.createDstRegRegion(
            old_dst->getRegAccess(),
            old_dst->getBase(),
            old_dst->getRegOff(),
            secondSubRegOff,
            old_dst->getHorzStride() * 2,
            old_dst->getType());

        if( old_dst->getRegAccess() != Direct )
        {
            secondDstOpnd->setImmAddrOff( old_dst->getAddrImm() + 1 );
        }

        expand_sec_half_op->setDest( secondDstOpnd );
    }
    else
    {
        expand_sec_half_op->setDest( expand_op->getDst() );
    }

    for( int k = 0; k < G4_Inst_Table[expand_op->opcode()].n_srcs; k++ )
    {
        G4_Operand *expand_src = expand_op->getSrc(k);

        if (!expand_src)
            continue;

        if ((expand_op->isMath() && k == 1 && expand_src->isNullReg()) ||
            expand_src->isImm()) {
            expand_sec_half_op->setSrc(expand_src, k);
        } else if (expand_src->isSrcRegRegion()) {
            G4_SrcRegRegion *expandSrcRegion = expand_src->asSrcRegRegion();

            if (expandSrcRegion->isScalar()) {
                expand_sec_half_op->setSrc(builder.duplicateOperand(expand_src), k);
            } else {
                short secondSubRegOffDiff = 0, secondAddrImmedDiff = 0;

                RegionDesc* origRegion = expandSrcRegion->getRegion();
                RegionDesc* newRegion = NULL;

                if( origRegion->width == 1 )
                {
                    newRegion = builder.createRegionDesc( origRegion->vertStride * 2, origRegion->width, origRegion->horzStride );
                    secondSubRegOffDiff = origRegion->vertStride;
                }
                else
                {
                    unsigned short newWD = origRegion->width/2;
                    secondSubRegOffDiff = origRegion->horzStride;
                    newRegion = builder.createRegionDesc( (newWD == 1 && newExecSize == 1) ? 0 : origRegion->vertStride,
                        newWD, (newWD== 1) ? 0 : origRegion->horzStride * 2 );
                }
                secondAddrImmedDiff = (short) (secondSubRegOffDiff * G4_Type_Table[expand_src->getType()].byteSize);
                expandSrcRegion->setRegion( newRegion );

                bool directSrc = ( expandSrcRegion->getRegAccess() == Direct );
                if( secondAddrImmedDiff >= GENX_GRF_REG_SIZ )
                {
                    secondSubRegOffDiff = (short) (( secondAddrImmedDiff - GENX_GRF_REG_SIZ ) / G4_Type_Table[expand_src->getType()].byteSize);
                }
                G4_SrcRegRegion *secondSrcOpnd = builder.createSrcRegRegion(
                    expandSrcRegion->getModifier(),
                    expandSrcRegion->getRegAccess(),
                    expandSrcRegion->getBase(),
                    expandSrcRegion->getRegOff() + ( ( directSrc && secondAddrImmedDiff >= GENX_GRF_REG_SIZ ) ? 1 : 0 ),
                    expandSrcRegion->getSubRegOff() + ( directSrc ? secondSubRegOffDiff : 0 ),
                    newRegion,
                    expandSrcRegion->getType());
                if (expandSrcRegion->getRegAccess() != Direct)
                {
                    secondSrcOpnd->setImmAddrOff( expandSrcRegion->getAddrImm() + secondAddrImmedDiff );
                }
                expand_sec_half_op->setSrc( secondSrcOpnd, k );
            }
        }
    }
    expand_sec_half_op->setLineNo(expand_op->getLineNo());

    if (expand_op->getPredicate() || expand_op->getCondMod())
    {
        if (expand_op->getMaskOffset() == 0)
        {
            expand_sec_half_op->setMaskOption(InstOpt_M8);
        }
        else if(expand_op->getMaskOffset() == 16)
        {
            expand_sec_half_op->setMaskOption(InstOpt_M24);
        }
        else if(!( expand_op->opcode() == G4_sel && !(expand_op->getPredicate()) && expand_op->getCondMod()))
        {
            expand_sec_half_op->setMaskOption( newExecSize > 8 ? InstOpt_M16 : InstOpt_M8 );
        }
    }
    return expand_sec_half_op;
}

// Fix up src regions in instructions:
//    In the component uncompressed instruction uop,
//       if exec size(uop) == width(src(uop)) && hstride(src(uop)) != 0
//          vstride(src(uop)) = width(src(uop)) * hstride(src(uop))
//    In the compressed instruction op,
//       if exec size(op) == width(src(op)) == vstride(src(op)) &&
//          hstride(src(op)) == 1
//          width(src(op)) = vstride(src(op)) = 8
// e.g.
//     mul (32) r60.0<1>:uw r76.0<32;16,1>:ub r78.0<0;1,0>:ub
//     =>
//     mul (32) r60.0<1>:uw r76.0<16;16,1>:ub r78.0<0;1,0>:ub
//     add (16) r60.0<1>:d r76.0<16;16,1>:d r78.0<0;1,0>:d
//     =>
//     add (16) r60.0<1>:d r76.0<8;8,1>:d r78.0<0;1,0>:d

//  in addition, fix the source region to follow the region restriction:
//  1. ExecSize must be greater than or equal to Width.  -- no check for this one
//  2. If ExecSize = Width and HorzStride ? 0, VertStride must be set to Width * HorzStride.
//  3. If ExecSize = Width and HorzStride = 0, there is no restriction on VertStride.
//  4. If Width = 1, HorzStride must be 0 regardless of the values of ExecSize and VertStride.
//  5. If ExecSize = Width = 1, both VertStride and HorzStride must be 0. This defines a scalar.
//  6. If VertStride = HorzStride = 0, Width must be 1 regardless of the value of ExecSize.
//  7. Dst.HorzStride must not be 0.        -- this needs not to be checked.
//  8. VertStride must be used to cross GRF register boundaries. This rule implies that
//      elements within a 'Width' cannot cross GRF boundaries.

void HWConformity::fixSrcRegion( G4_INST *inst )
{

    bool comprInst = isCompressedInst( inst );
    for (int i = 0; i < G4_MAX_SRCS; i++)
    {
        if (inst->getSrc(i) && inst->getSrc(i)->isSrcRegRegion() && !inst->getSrc(i)->isNullReg())
        {
            G4_SrcRegRegion *src = inst->getSrc(i)->asSrcRegRegion();
            RegionDesc* srcRegion = src->getRegion();
            if( srcRegion->isRegionWH() || srcRegion->isRegionV() || srcRegion->isRegionSW() )
                continue;
            uint16_t vs = srcRegion->vertStride, wd = srcRegion->width, hs = srcRegion->horzStride;
            uint8_t exSize = inst->getExecSize();
            MUST_BE_TRUE( inst->isSend() || exSize >= wd, " Bad source region: Width is greater than execution size." );
            if ( comprInst )
            {
                if (G4_Type_Table[inst->getSrc(i)->getType()].byteSize > G4_WSIZE &&
                    wd == exSize &&
                    vs == wd && hs == 1)
                {
                    vs = wd = exSize / 2;
                }
            }
            if( wd == exSize && hs != 0 && vs != wd * hs )
            {
                vs = wd * hs;
            }
            if( wd == 1 )
            {
                hs = 0;
                if( 1 == exSize )
                    vs = 0;
            }
            if( vs == 0 && hs == 0 )
            {
                wd = 1;
            }
            if( hs == 0 &&
                ((G4_Type_Table[inst->getSrc(i)->getType()].byteSize == G4_WSIZE &&
                  exSize == 32 && vs == 32 && wd == 32) ||
                 (G4_Type_Table[inst->getSrc(i)->getType()].byteSize == G4_DSIZE &&
                  exSize == 16 && vs == 16 && wd == 16)) )
            {
                vs = 0;
                wd = 1;
            }

            // check cross GRF (rule 2H)
            // TODO! for the following two cases, split the instruction:
            // source region is like<8;4,1>
            // source region is like<2;4,1>
            if( src->getRegAccess() == Direct && src->crossGRF() && hs != 0)
            {
                // TODO: this is a temp fix
                if( (getGenxPlatform() == GENX_BDW || getGenxPlatform() == GENX_CHV) && vs < wd * hs )
                    continue;
                // check number of elements in first GRF.
                uint16_t execTypeSize = hs * src->getElemSize();
                uint16_t sizeInFirstGRF = GENX_GRF_REG_SIZ - src->getLeftBound() % GENX_GRF_REG_SIZ;
                uint16_t vertSize = vs * G4_Type_Table[src->getType()].byteSize;
                uint16_t numEle = ( sizeInFirstGRF + execTypeSize - 1 ) / execTypeSize;
                uint16_t rowSize = wd * execTypeSize;

                if( sizeInFirstGRF <= vertSize )
                {
                    if( numEle >= wd )
                    {
                        numEle = wd;
                    }
                }
                else if( vs > wd )
                {
                    numEle = sizeInFirstGRF/vertSize * wd +
                        (( sizeInFirstGRF%vertSize > rowSize ) ? wd : ( sizeInFirstGRF%vertSize + execTypeSize - 1 ) / execTypeSize );
                }
                // wd is used to cross GRF, change to <vs;1,0>
                if( numEle < wd || ( wd >= vs && numEle % wd != 0 ) )
                {

                    wd = 1;
                    if( hs == 0 )
                    {
                        vs = 1;
                    }
                    else
                    {
                        vs = hs;
                    }
                    hs = 0;
                }
            }

            if( vs != srcRegion->vertStride || wd != srcRegion->width || hs != srcRegion->horzStride )
            {
                G4_SrcRegRegion *origSrc = inst->getSrc(i)->asSrcRegRegion();
                origSrc->setRegion( builder.createRegionDesc( vs, wd, hs ) );
            }
        }
    }
    if( inst->getDst() && !inst->hasNULLDst() )
    {
        MUST_BE_TRUE( inst->getDst()->getHorzStride() != 0,
            "Bad source region: Width is greater than execution size." );
    }
}

//
//single entry point for HW conformity checks
//
void HWConformityChk(IR_Builder& builder, G4_Kernel& kernel, Mem_Manager& mem )
{
    HWConformity conformity( builder, kernel, mem );
    conformity.chkHWConformity();
}

bool HWConformity::markPackedByteReference(G4_Kernel& kernel, G4_Operand* opnd, G4_INST* inst)
{
    G4_Declare *dcl = NULL, *topdcl = NULL;
    bool foundOptCandidate = false;

    if ((opnd->isSrcRegRegion() || opnd->isDstRegRegion()))
    {
        if (opnd->getBase() && opnd->getBase()->isRegVar())
        {
            dcl = opnd->getBase()->asRegVar()->getDeclare();
            topdcl = dcl->getRootDeclare();
        }
    }

    if (topdcl != NULL &&
        topdcl->getRegFile() == G4_GRF &&
        !(topdcl->getAddressed()))
    {
        if (topdcl->doNotWiden() || inst->isSend())
        {
            //send has no regioning so it is certainly illegal to change data layout
            setAccessPattern(topdcl, ACCESS_PATTERN_INVALID);
            return false;
        }

        if (opnd->isDstRegRegion() &&
            // check if the opnd has pre-assigned physical regsiter
            !(opnd->asDstRegRegion()->getBase()->asRegVar()->isPhyRegAssigned()) &&
            // check if the opnd is global
            !(kernel.fg.globalOpndHT.isOpndGlobal(opnd)) &&
            // check if the opnd is used as packed byte
            G4_Type_Table[opnd->getType()].byteSize == 1 &&
            dcl->getElemSize() == 1 &&
            opnd->asDstRegRegion()->getHorzStride() == 1 &&
            // check if the instruction is a raw mov
            !inst->isRawMov() &&
            // check if the instruction execution type is word
            // (This should be the most common case that can benefit
            //  from this optimization. It could be extended to other
            //  cases like D execution type).
            G4_Type_Table[inst->getExecType()].byteSize == 2 )
        {
            unsigned int leftBound = opnd->asDstRegRegion()->getLeftBound();
            unsigned int rightBound = opnd->asDstRegRegion()->getRightBound();

            if (((rightBound*2/G4_GRF_REG_NBYTES - leftBound*2/G4_GRF_REG_NBYTES) > 1) ||
                (getGenxPlatform() == GENX_BDW &&
                 (rightBound*2/G4_GRF_REG_NBYTES != leftBound*2/G4_GRF_REG_NBYTES)))
            {
                setAccessPattern(topdcl, ACCESS_PATTERN_INVALID);
            }
            else if (getAccessPattern(topdcl) == ACCESS_PATTERN_UNDEF)
            {
                setAccessPattern(topdcl, ACCESS_PATTERN_PACKED_BYTE);
                foundOptCandidate = true;
            }
        }
        else if (opnd->isSrcRegRegion() &&
                // check if the opnd has pre-assigned physical regsiter
                !(opnd->asSrcRegRegion()->getBase()->asRegVar()->isPhyRegAssigned()) &&
                // check if the opnd is global
                !(kernel.fg.globalOpndHT.isOpndGlobal(opnd)) &&
                // check if the opnd is used as packed byte
                G4_Type_Table[opnd->getType()].byteSize == 1 &&
                dcl->getElemSize() == 1 &&
                opnd->asSrcRegRegion()->getRegion()->isContiguous(inst->getExecSize()))
        {
            unsigned int leftBound = opnd->asSrcRegRegion()->getLeftBound();
            unsigned int rightBound = opnd->asSrcRegRegion()->getRightBound();

            if (((rightBound*2/G4_GRF_REG_NBYTES - leftBound*2/G4_GRF_REG_NBYTES) > 1) ||
                (getGenxPlatform() == GENX_BDW &&
                 (rightBound*2/G4_GRF_REG_NBYTES != leftBound*2/G4_GRF_REG_NBYTES)))
            {
                setAccessPattern(topdcl, ACCESS_PATTERN_INVALID);
            }
        }
        else
        {
            setAccessPattern(topdcl, ACCESS_PATTERN_INVALID);
        }
    }

    return foundOptCandidate;
}

G4_Operand* HWConformity::fixPackedByteReference(IR_Builder& builder, G4_Operand* opnd)
{
    G4_Operand* newOpnd = NULL;
    G4_Declare* topdcl = NULL;

    if (opnd->isDstRegRegion() ||
        opnd->isSrcRegRegion())
    {
        topdcl = GetTopDclFromRegRegion(opnd);
    }

    if (topdcl != NULL &&
        getAccessPattern(topdcl) == ACCESS_PATTERN_PACKED_BYTE)
    {
        if (opnd->isDstRegRegion())
        {
            short dst_regoff = opnd->asDstRegRegion()->getRegOff();
            short dst_subregoff = opnd->asDstRegRegion()->getSubRegOff();
            short off = (dst_regoff * G4_GRF_REG_NBYTES + dst_subregoff) * 2;

            dst_regoff = off / G4_GRF_REG_NBYTES;
            dst_subregoff = off % G4_GRF_REG_NBYTES;

            G4_DstRegRegion* newDstOpnd = builder.createDstRegRegion(
                Direct,
                opnd->getBase()->asRegVar(),
                dst_regoff,
                dst_subregoff,
                2,
                opnd->getType());
            newOpnd = newDstOpnd;
        }
        else if (opnd->isSrcRegRegion())
        {
            short src_regoff = opnd->asSrcRegRegion()->getRegOff();
            short src_subregoff = opnd->asSrcRegRegion()->getSubRegOff();
            short off = (src_regoff * G4_GRF_REG_NBYTES + src_subregoff) * 2;

            src_regoff = off / G4_GRF_REG_NBYTES;
            src_subregoff = off % G4_GRF_REG_NBYTES;

            RegionDesc *rd = builder.getRegionStride2();
            G4_SrcRegRegion* newSrcOpnd = builder.createSrcRegRegion(opnd->asSrcRegRegion()->getModifier(),
                Direct,
                opnd->getBase()->asRegVar(),
                src_regoff,
                src_subregoff,
                rd,
                opnd->getType());
            newOpnd = newSrcOpnd;
        }
    }

    return newOpnd;
}

void HWConformity::fixDataLayout( )
{
    bool changeDataLayout = false;

    for (auto &bb : kernel.fg.BBs)
    {
        for (auto &inst : bb->instList)
        {
            if (G4_Inst_Table[inst->opcode()].n_dst == 1)
            {
                G4_Operand* dst = inst->getDst();

                if (dst)
                {
                    bool foundOptCandidate = markPackedByteReference(kernel, dst, inst);
                    if (changeDataLayout == false && foundOptCandidate)
                    {
                        changeDataLayout = true;
                    }
                }
            }

            for (int i = 0; i < G4_Inst_Table[inst->opcode()].n_srcs; i++)
            {
                G4_Operand* src = inst->getSrc(i);

                if (src)
                {
                    markPackedByteReference(kernel, src, inst);
                }
            }
        }
    }

    if (changeDataLayout)
    {
        for (auto &dcl : kernel.Declares)
        {
            G4_Declare* topdcl = dcl->getRootDeclare();

            if (getAccessPattern(topdcl) == ACCESS_PATTERN_PACKED_BYTE)
            {
                dcl->setTotalElems(dcl->getTotalElems() * 2);

                if (dcl != topdcl)
                {
                    G4_Declare* aliasDcl = dcl->getAliasDeclare();
                    unsigned int aliasOffset = dcl->getAliasOffset();
                    dcl->setAliasDeclare(aliasDcl, aliasOffset * 2);
                }
            }
        }

        for (auto &bb : kernel.fg.BBs)
        {
            for (auto &inst : bb->instList)
            {
                if (G4_Inst_Table[inst->opcode()].n_dst == 1)
                {
                    G4_Operand* dst = inst->getDst();
                    G4_Operand* newDst = NULL;

                    if (dst)
                    {
                        newDst = fixPackedByteReference(builder, dst);
                        if (newDst)
                        {
                            inst->setDest(newDst->asDstRegRegion());
                        }
                    }
                }

                for (int i = 0; i < inst->getNumSrc(); i++)
                {
                    G4_Operand* src = inst->getSrc(i);
                    G4_Operand* newSrc = NULL;

                    if (src)
                    {
                        newSrc = fixPackedByteReference(builder, src);
                        if (newSrc)
                        {
                            inst->setSrc(newSrc, i);
                        }
                    }
                }
            }
        }
    }
}

// maintain def-use chain for current inst and the MOV inst generated for its dst
void HWConformity::maintainDU4TempMov( G4_INST *inst, G4_INST *newInst )
{
    if (newInst->getPredicate())
    {
        inst->transferDef(newInst, Opnd_pred, Opnd_pred);
    }

    inst->transferUse(newInst);

    inst->addDefUse(newInst, Opnd_src0);
}

static void expandPlaneMacro(IR_Builder& builder, INST_LIST_ITER it, G4_BB* bb, bool secondHalf)
{
    G4_INST* inst = *it;
    G4_DstRegRegion* dst = inst->getDst();
    G4_SrcRegRegion* src0 = inst->getSrc(0)->asSrcRegRegion();
    G4_SrcRegRegion* src1 = inst->getSrc(1)->asSrcRegRegion();

    G4_SrcRegRegion* srcP = builder.createSrcRegRegion(src0->getModifier(), Direct, src0->getBase(),
        src0->getRegOff(), src0->getSubRegOff(), builder.getRegionScalar(), src0->getType());
    G4_SrcRegRegion* srcQ = builder.createSrcRegRegion(src0->getModifier(), Direct, src0->getBase(),
        src0->getRegOff(), src0->getSubRegOff() + 1, builder.getRegionScalar(), src0->getType());
    G4_SrcRegRegion* srcR = builder.createSrcRegRegion(src0->getModifier(), Direct, src0->getBase(),
        src0->getRegOff(), src0->getSubRegOff() + 3, builder.getRegionScalar(), src0->getType());

    G4_SrcRegRegion* u = builder.duplicateOperand(src1);
    u->setRegOff(u->getRegOff() + (secondHalf ? 2 : 0));
    G4_SrcRegRegion* v = builder.duplicateOperand(src1);
    v->setRegOff(v->getRegOff() + (secondHalf ? 3 : 1));

    uint32_t options = inst->getOption();
    if (inst->getExecSize() == 16)
    {
        options &= ~InstOpt_QuarterMasks;
        int maskOffset = inst->getMaskOffset() + (secondHalf ? 8 : 0);
        switch (maskOffset)
        {
        case 0:
            options |= InstOpt_M0;
            break;
        case 8:
            options |= InstOpt_M8;
            break;
        case 16:
            options |= InstOpt_M16;
            break;
        case 24:
            options |= InstOpt_M24;
            break;
        default:
            MUST_BE_TRUE(false, "unexpected offset value");
        }
    }

    G4_Declare* tmpVal = builder.hasNFType() ? nullptr : builder.createTempVar(8, Type_F, Either, Any);
    G4_DstRegRegion* accDst = builder.hasNFType() ? 
        builder.createDstRegRegion(Direct, builder.phyregpool.getAcc0Reg(), 0, 0, 1, Type_NF) :
        builder.Create_Dst_Opnd_From_Dcl(tmpVal, 1);
    G4_INST* madInst = builder.createInternalInst(nullptr, G4_mad, nullptr, false, 8, accDst,
        srcR, u, srcP, options);
    bb->instList.insert(it, madInst);

    G4_Predicate* pred = inst->getPredicate() ? builder.duplicateOperand(inst->getPredicate()) : nullptr;
    G4_CondMod* condMod = inst->getCondMod() ? builder.duplicateOperand(inst->getCondMod()) : nullptr;
    G4_SrcRegRegion* accSrc = builder.hasNFType() ?
        builder.createSrcRegRegion(Mod_src_undef, Direct, builder.phyregpool.getAcc0Reg(), 0, 0, builder.getRegionStride1(), Type_NF) :
        builder.Create_Src_Opnd_From_Dcl(tmpVal, builder.getRegionStride1());
    G4_DstRegRegion* newDst = builder.createDstRegRegion(Direct, dst->getBase(),
        dst->getRegOff() + (secondHalf ? 1 : 0), dst->getSubRegOff(), dst->getHorzStride(), dst->getType());
    G4_INST* secondMadInst = builder.createInternalInst(pred, G4_mad, condMod, inst->getSaturate(), 8, newDst,
        accSrc, v, srcQ, options);
    bb->instList.insert(it, secondMadInst);
}

// Replace plane with a macro sequence:
// pln dest:f src0:f src1:f
// -->
// mad acc0:nf src0.3:f src1:f src0.0:f
// mad dest:f acc0:nf src1+1:f src0.1:f
// simd16 pln also needs to be split as the macro is simd8 only

void HWConformity::expandPlaneInst(INST_LIST_ITER it, G4_BB* bb)
{
    G4_INST* inst = *it;
    MUST_BE_TRUE(inst->opcode() == G4_pln, "expect a plane inst");
    MUST_BE_TRUE(inst->getSrc(0)->isSrcRegRegion(), "src0 must be source reg region");
    MUST_BE_TRUE(inst->getExecSize() == 8 || inst->getExecSize() == 16, " only size 8 and 16 are supported");

    G4_DstRegRegion* dst = inst->getDst();
    if (dst->getRegAccess() == IndirGRF || dst->getHorzStride() > 1)
    {
        inst->setDest(insertMovAfter(it, dst, dst->getType(), bb));
    }
    G4_SrcRegRegion* src0 = inst->getSrc(0)->asSrcRegRegion();
    if (src0->getRegAccess() == IndirGRF)
    {
        // insert move to make src0 direct
        inst->setSrc(insertMovBefore(it, 0, src0->getType(), bb), 0);
    }
    G4_SrcRegRegion* src1 = inst->getSrc(1)->asSrcRegRegion();
    if (src1->getRegAccess() == IndirGRF)
    {
        // insert move to make src1 direct
        inst->setSrc(insertMovBefore(it, 1, src1->getType(), bb), 1);
    }

    expandPlaneMacro(builder, it, bb, false);
    if (inst->getExecSize() == 16)
    {
        expandPlaneMacro(builder, it, bb, true);
    }

    it = bb->instList.erase(it);
}

// plane does not support pln with non-packed dst.
// also fix up plane sources, which don't support modifiers
// returns true if the original plane is deleted
bool HWConformity::fixPlaneInst(INST_LIST_ITER it, G4_BB* bb)
{

    G4_INST* inst = *it;
    if (inst->opcode() == G4_pln)
    {
        if (!builder.doPlane())
        {
            expandPlaneInst(it, bb);
            return true;
        }
        G4_DstRegRegion* dst = inst->getDst();
        if (dst->getHorzStride() != 1)
        {
            G4_DstRegRegion *newDst = insertMovAfter(it, dst, dst->getType(), bb);
            inst->setDest(newDst);
        }

        G4_Operand* src0 = inst->getSrc(0);
        G4_Operand* src1 = inst->getSrc(1);

        // Source modifiers are not supported for pln instruction
        if (src0 &&
            ((src0->isSrcRegRegion() &&
            src0->asSrcRegRegion()->getModifier() != Mod_src_undef) ||
            !builder.isOpndAligned(src0, 16)))
        {
            // src0 needs a temp
            G4_Declare* tmpDcl = builder.createTempVar(4, Type_F,
                Either, Sixteen_Word);

            // Before:
            // pln (16) dst, (mod)src0, src1
            //
            // After:
            // mov (4) tmp(0,0):f (mod)src0(r)<4;4,1>:f
            // pln (16) dst, tmp(0,0)<0;1,0>, src1
            G4_DstRegRegion* dstRgn = builder.createDstRegRegion(
                Direct,
                tmpDcl->getRegVar(),
                0,
                0,
                1,
                Type_F);

            RegionDesc* rd = builder.createRegionDesc(4, 4, 1);
            G4_SrcRegRegion* srcRgn = builder.createSrcRegRegion(
                src0->asSrcRegRegion()->getModifier(),
                Direct,
                src0->asSrcRegRegion()->getBase(),
                src0->asSrcRegRegion()->getRegOff(),
                src0->asSrcRegRegion()->getSubRegOff(),
                rd,
                Type_F);

            G4_INST* newInst = builder.createInternalInst(NULL, G4_mov,
                NULL, false, 4, dstRgn, srcRgn, NULL, 0);

            bb->instList.insert(it, newInst);

            rd = builder.getRegionScalar();
            G4_SrcRegRegion* newSrcRgn = builder.createSrcRegRegion(
                Mod_src_undef,
                Direct,
                tmpDcl->getRegVar(),
                0,
                0,
                rd,
                Type_F);

            inst->setSrc(newSrcRgn, 0);
            inst->transferDef(newInst, Opnd_src0, Opnd_src0);
            newInst->addDefUse(inst, Opnd_src0);
        }

        if (src1 && src1->isSrcRegRegion() && src1->asSrcRegRegion()->getModifier() != Mod_src_undef)
        {
            // src1 needs a temp
            // For pln instruction src2 is implied from src1 and exec_size
            // When exec_size = 8, src2 is 1 GRF after src1 with size = 1 GRF
            // When exec_size = 16, src2 is 2 GRFs after src1 with size = 2 GRFs
            unsigned short numGRFsToCopy = inst->getExecSize() == 8 ? 2 : 4;

            G4_Declare* tmpDcl = builder.createTempVar((unsigned short)(G4_GRF_REG_NBYTES / G4_Type_Table[Type_F].byteSize * numGRFsToCopy), Type_F,
                Either, Any);

            // Before:
            // pln (16) dst, src0, (mod)src1
            //
            // After:
            // mov (16) tmp(0,0):f (mod)src1(r)<8;8,1>:f
            // mov (16) tmp(2,0):f (mod)src1(r+2)<8;8,1>:f <-- only if exec_size = 16
            // pln (16) dst, src0, tmp(0,0)
            for (int i = 0; i < numGRFsToCopy; i += 2)
            {
                G4_DstRegRegion* dstRgn = builder.createDstRegRegion(
                    Direct,
                    tmpDcl->getRegVar(),
                    (short)i,
                    0,
                    1,
                    Type_F);

                RegionDesc* rd = builder.createRegionDesc(8, 8, 1);
                G4_SrcRegRegion* srcRgn = builder.createSrcRegRegion(
                    src1->asSrcRegRegion()->getModifier(),
                    Direct,
                    src1->asSrcRegRegion()->getBase(),
                    src1->asSrcRegRegion()->getRegOff() + i,
                    0,
                    rd,
                    Type_F);

                G4_INST* newInst = builder.createInternalInst(NULL, G4_mov,
                    NULL, false, 16, dstRgn, srcRgn, NULL, 0);

                bb->instList.insert(it, newInst);

                if (i == 0)
                {
                    G4_SrcRegRegion* newSrcRgn = builder.createSrcRegRegion(
                        Mod_src_undef,
                        Direct,
                        tmpDcl->getRegVar(),
                        0,
                        0,
                        rd,
                        Type_F);

                    inst->setSrc(newSrcRgn, 1);
                    inst->transferDef(newInst, Opnd_src1, Opnd_src0);
                }
                newInst->addDefUse(inst, Opnd_src1);
            }
        }
    }
    return false;
}

void HWConformity::fixImm64 ( INST_LIST_ITER i,
                              G4_BB* bb )
{
    G4_INST *inst = *i;
    for( int j = 0; j < G4_Inst_Table[inst->opcode()].n_srcs; j++ )
    {
        G4_Operand *src = inst->getSrc(j);
        if( !src                                    ||
            !(src->isImm() )                        ||
            G4_Type_Table[src->getType()].byteSize != 8 )
        {
            continue;
        }
        // a 64bit immediate is supported ONLY for a MOV operation
        bool needsSplit =  false;

        if( VISA_WA_CHECK(builder.getPWaTable(), WaDisallow64BitImmMov) )
        {
            needsSplit = true;
        }
        if (needsSplit)
        {
            char* immPtr = NULL;
            double dfValue = 0.0f;
            int64_t qValue = 0;

            if (IS_DFTYPE(src->getType()))
            {
                dfValue = src->asImm()->getDouble();
                immPtr = (char*) &dfValue;
            }
            else
            {
                qValue = src->asImm()->getInt();
                immPtr = (char*) &qValue;
            }
            unsigned int lowValue = *((unsigned int*)(immPtr));
            unsigned int highValue = *((unsigned int*)(immPtr+4));
            G4_Imm *lowImm = builder.createImm( (int64_t)lowValue, Type_UD);
            G4_Imm *highImm = builder.createImm( (int64_t)highValue, Type_UD);

            G4_Declare *defDcl = NULL;

            defDcl = builder.createTempVar(1, src->getType(), Either, Eight_Word);
            G4_Declare* dcl = builder.createTempVar( 2, Type_UD, Either, Eight_Word );
            dcl->setAliasDeclare(defDcl, 0);

            G4_DstRegRegion *dstRegion = builder.Create_Dst_Opnd_From_Dcl(dcl, 1);
            G4_INST* lowMovInst = builder.createInternalInst(NULL, G4_mov, NULL, false,
                1, dstRegion, lowImm, NULL, InstOpt_WriteEnable,
                inst->getLineNo(), inst->getCISAOff(), inst->getSrcFilename() );

            bb->instList.insert(i, lowMovInst);

            G4_DstRegRegion *dstRegionNext = builder.Create_Dst_Opnd_From_Dcl(dcl, 1);
            G4_INST *highMovInst = builder.createInternalInst( NULL, G4_mov, NULL, false,
                1, dstRegionNext, highImm, NULL, InstOpt_WriteEnable,
                inst->getLineNo(), inst->getCISAOff(), inst->getSrcFilename() );
            dstRegionNext->setSubRegOff(1);
            bb->instList.insert(i, highMovInst);

            inst->transferDef(lowMovInst, Gen4_Operand_Number(j + 1), Opnd_src0);
            lowMovInst->addDefUse(inst, Gen4_Operand_Number(j + 1));
            inst->transferDef(highMovInst, Gen4_Operand_Number(j + 1), Opnd_src0);
            highMovInst->addDefUse(inst, Gen4_Operand_Number(j + 1));

            unsigned short vs = 0, hs = 0, wd = 1; // gen7_5: always 0;1,0
            G4_SrcRegRegion *new_src = builder.Create_Src_Opnd_From_Dcl(defDcl,
                builder.createRegionDesc(vs, wd, hs));
            inst->setSrc( new_src, j );
        }
        else
        {
            if ( inst->opcode() != G4_mov )
            {
                inst->setSrc(insertMovBefore(i, j, src->getType(), bb), j);
            }
        }
    }
}

// Check if the source of def_inst is redefined before inst
G4_INST* HWConformity::checkSrcDefInst( G4_INST *inst,
                                        G4_INST *def_inst,
                                        uint32_t srcNum )
{
    G4_INST* valid_inst = def_inst;

    if( def_inst != NULL )
    {
        MUST_BE_TRUE( def_inst->opcode() == G4_mov, "def inst must be a mov instruction" );

        G4_INST* def_inst1 = NULL;
        for (auto def_it1 = inst->def_begin(); def_it1 != inst->def_end(); def_it1++ )
        {
            if((*def_it1).second == srcNum + 1 )
            {
                def_inst1 = (*def_it1).first;
            }
        }

        if( def_inst1 != NULL )
        {
            G4_INST* def_inst2 = NULL;
            for (auto def_it2 = def_inst->def_begin(); def_it2 != def_inst->def_end(); def_it2++ )
            {
                if((*def_it2).second == Opnd_src0 )
                {
                    def_inst2 = (*def_it2).first;
                }
            }

            if ( def_inst1 != def_inst2 )
            {
                valid_inst = NULL;
            }
        }
    }

    return valid_inst;
}

/*
    Helper function for fixMixedHFInst
    It assumes dst is not null and is of type DstRegRegion.
    This check must be done before this method is called.
*/
void HWConformity::helperGenerateTempDst(
                                G4_BB* bb,
                                INST_LIST_ITER instIter,
                                G4_INST *inst,
                                uint8_t hStride,
                                G4_Type tempDstType,
                                G4_SubReg_Align subAlign)
{
    G4_DstRegRegion *dst = inst->getDst();
    uint8_t execSize = inst->getExecSize();
    uint8_t dstSize = execSize * G4_Type_Table[tempDstType].byteSize;
    //create a new temp with horizontal stride of 1 (packed)
    //create a move to dst.

    uint32_t numElt = execSize == 1 ? 1 : execSize * hStride;
    if (numElt > 1 && tempDstType == Type_HF && hStride == 1 && subAlign < Eight_Word)
        subAlign = Eight_Word;
    G4_Align align = getDclAlignment( dstSize, inst, execSize == 1, subAlign );

    G4_Declare* dcl = builder.createTempVar( numElt, tempDstType, align , subAlign );


    G4_DstRegRegion *dstRegion = builder.Create_Dst_Opnd_From_Dcl(dcl, hStride);
    inst->setDest(dstRegion);

    RegionDesc* region = builder.createRegionDesc(execSize*hStride, execSize, hStride);
    G4_SrcRegRegion *srcRegion = builder.Create_Src_Opnd_From_Dcl(dcl, region);

    //creating a mov from temp dst to final destination using original options of fixed instruction
    G4_INST* movInst = builder.createInst( NULL, G4_mov, NULL, false, execSize, dst, srcRegion, NULL, inst->getMaskOption() );

    ++instIter;
    //inserting mov after fixed instruction
    bb->instList.insert( instIter, movInst );

    /*
    Need to remove dst from uses list of mulh, and add them to movInst useList
    add movInst to uselist of mulh.
    Add mulh to def instruction list of movInst
    */
    inst->transferUse(movInst);
    inst->addDefUse(movInst, Opnd_src0);
}

/*
    Not Implemented rules:

    3:  (Does this mean align1 doesn't support replication?)
        In Align16 mode, replicate is supported and is coissueable.

    4: (handled in reduce execution size)
        No simd16 in mixed mode when destination is packed f16 for both Align1 and Align16.

            mad(8) r3.xyzw:hf r4.xyzw:f r6.xyzw:hf r7.xyzw:hf

            add(8) r20.0<1>:hf r3<8;8,1>:f r6.0<8;8,1>:hf {Q1}

    5: (we are not producing this type of code)
        No accumulator read access for align16 mixed float

    6: (we do not generate code like this)
        [DevCHV, DevSKL+]: When source is float from accumulator register and destination is half float with a stride of 1, the source must register aligned. i.e., source must have offset zero.

    7: (doesn't seem like it is applicable to our code)
        In Align16, vertical stride can never be zero for f16

    8.a: (handled by another check)
        Math operations for mixed mode,
            - In Align16, only packed format is supported

    11. (handled in reduce execution size)
        [DevCHV, DevSKL, DevBXT]: No simd16 in mixed mode when destination is f32. Instruction Execution size must be no more than 8.

*/
void HWConformity::fixMixedHFInst( BB_LIST_ITER it )
{
    G4_BB* bb = *it;
    for (auto instIter = bb->instList.begin(); instIter != bb->instList.end(); ++instIter)
    {
        G4_INST *inst = *instIter;

        if (inst->isSend())
        {
            continue;
        }
        //In case of invalid ISA
        if (inst->isMath() && (inst->isMixedMode() || builder.getOption(vISA_DisableHFMath)))
        {
            auto src0 = inst->getSrc(0);
            auto src1 = inst->getSrc(1);
            auto dst = inst->getDst();
            if (src0 && src0->getType() == Type_HF)
            {
                inst->setSrc(insertMovBefore(instIter, 0, Type_F, bb), 0);
            }

            if (src1 && src1->getType() == Type_HF)
            {
                inst->setSrc(insertMovBefore(instIter, 1, Type_F, bb), 1);
            }

            if (dst && dst->getType() == Type_HF)
            {
                inst->setDest(insertMovAfter(instIter, dst, inst->getExecType2(), bb));
            }
            continue;
        }

        if (VISA_WA_CHECK(builder.getPWaTable(), WaSrc1ImmHfNotAllowed) && !inst->isSend())
        {
            G4_Operand *tSrc1 = inst->getSrc(1);
            if (tSrc1 && tSrc1->isImm() && tSrc1->getType() == Type_HF)
            {
                inst->setSrc(insertMovBefore(instIter, 1, Type_HF, bb), 1);
            }
        }


        // Restriction :
        // The execution size must be no more than 8 when half-floats are used in source or destination operand.
        if (inst->getExecSize() == 16)
        {
            if (inst->opcode() == G4_math               &&
                inst->getDst()->getType() == Type_HF    &&
                inst->getSrc(0)->getType() == Type_HF &&
                (!inst->getSrc(1) || inst->getSrc(1)->getType() == Type_HF))
            {
                evenlySplitInst(instIter, bb);
            }
        }
      
        if (inst->isMath() &&
            VISA_WA_CHECK(builder.getPWaTable(), WaDstSubRegNumNotAllowedWithLowPrecPacked))
        {
            G4_DstRegRegion* dst = inst->getDst();
            if (dst                         &&
                dst->getType() == Type_HF   &&
                dst->getSubRegOff() == 8)
            {
                helperGenerateTempDst(bb, instIter, inst, 1, Type_HF, Sixteen_Word);
            }
        }

        if (inst->isMath() && inst->isMixedMode()) 
        {
            // For `math`, additional GRF alignment checking for non-scalar
            // destination.
            G4_DstRegRegion* dst = inst->getDst();
            if (dst->getType() == Type_F &&
                inst->getExecSize() != 1 &&
                !builder.isOpndAligned(dst, G4_GRF_REG_NBYTES)) 
            {
                helperGenerateTempDst(bb, instIter, inst, 1, Type_F, Sixteen_Word);
            }
        }

        G4_DstRegRegion *dst = inst->getDst();
        if (INST_FLOAT_SRC_ONLY(inst->opcode()) && dst && !dst->isNullReg() && dst->getType() == Type_HF)
        {
            helperGenerateTempDst(bb, instIter, inst, 1, Type_F);
        }

        if (!inst->isMixedMode())
            continue;

        if (inst->getDst() && !inst->getDst()->isNullReg())
            dst = inst->getDst();

        if ((VISA_WA_CHECK(builder.getPWaTable(), WaMixModeSelInstDstNotPacked) ||
            VISA_WA_CHECK(builder.getPWaTable(), WaFloatMixedModeSelNotAllowedWithPackedDestination)) &&
            inst->opcode() == G4_sel                                        &&
            dst                                                             &&
            (VISA_WA_CHECK(builder.getPWaTable(), WaMixModeSelInstDstNotPacked) || dst->getHorzStride() == 1) &&
            dst->getType() == Type_HF)
        {
            helperGenerateTempDst(bb, instIter, inst, 1, Type_F);
        }

        if (!inst->isMixedMode())
            continue;
        /*
        Checks for mix mode HW conformity violations.
        */
        if (getGenxPlatform() >= GENX_CHV)
        {
            if(checkMixMode(instIter, bb))
            {
                //instruction was split, and new instruction inserted before
                //going back to previous instruction to double check it still confirms.
                --instIter;
                inst = *instIter;
            }
        }

        if (VISA_WA_CHECK(builder.getPWaTable(), WaDstSubRegNumNotAllowedWithLowPrecPacked) &&
            dst                                                                             &&
            dst->getType() == Type_HF                                                       &&
            dst->getSubRegOff() == 8                                                        &&
            inst->getExecSize() == 8)
        {
            helperGenerateTempDst(bb, instIter, inst, 1, dst->getType());
        }

        if( inst->isMath()       &&
            ((VISA_WA_CHECK(builder.getPWaTable(), WaDisableMixedModeLog) && inst->asMathInst()->getMathCtrl() == MATH_LOG) ||
            (VISA_WA_CHECK(builder.getPWaTable(), WaDisableMixedModeFdiv) && inst->asMathInst()->getMathCtrl() == MATH_FDIV) ||
            (VISA_WA_CHECK(builder.getPWaTable(), WaDisableMixedModePow) && inst->asMathInst()->getMathCtrl() == MATH_POW)))
        {
            if (dst && dst->getType() == Type_HF)
            {
                helperGenerateTempDst(bb, instIter, inst, 1, Type_F);
            }

            for (uint8_t i = 0; i < inst->getNumSrc(); ++i)
            {
                G4_Operand *tOpnd = inst->getSrc(i);

                if (tOpnd == NULL || !tOpnd->isSrcRegRegion() ||
                    tOpnd->asSrcRegRegion()->getType() != Type_HF)
                {
                    continue;
                }

                inst->setSrc(insertMovBefore(instIter, i, Type_F, bb), i);
            }
        }

        // - In Align1, f16 inputs need to be strided
        // math(8) r3<1>:hf r4.0<8;8,1>:f r6.0<8;4,2>:hf
        if (inst->isMath())
        {
            for (uint8_t i = 0; i < inst->getNumSrc(); ++i)
            {
                G4_Operand *tOpnd = inst->getSrc(i);

                if (tOpnd == NULL                                       ||
                    !tOpnd->isSrcRegRegion()                            ||
                    tOpnd->asSrcRegRegion()->getType() != Type_HF       ||
                    !tOpnd->asSrcRegRegion()->isNativePackedSrcRegion())
                {
                    continue;
                }

                inst->setSrc(insertMovBefore(instIter, i, Type_F, bb), i);
            }
        }

        if (inst->isMath() && inst->getSrc(0)->isImm())
        {
            bool nullSrc1 = inst->getSrc(1) == nullptr || inst->getSrc(1)->isNullReg();
            if (!nullSrc1)
            {
                inst->setSrc(insertMovBefore(instIter, 0, inst->getSrc(0)->getType(), bb), 0);
            }
        }

        for (uint8_t i = 0; i < inst->getNumSrc(); ++i)
        {
            G4_Operand *tOpnd = inst->getSrc(i);

            if (tOpnd == NULL || !tOpnd->isSrcRegRegion())
                continue;

            G4_SrcRegRegion *srcOpnd = tOpnd->asSrcRegRegion();

            // `math` instruction requires non-scalar float operand to be
            // GRF aligned.
            if (inst->isMath() &&
                srcOpnd->getType() == Type_F &&
                !srcOpnd->isScalar() &&
                !builder.isOpndAligned(tOpnd, G4_GRF_REG_NBYTES)) {
                inst->setSrc(insertMovBefore(instIter, i, Type_F, bb), i);
            }
            /*

            8: Math operations for mixed mode,
            - In Align1, f16 inputs need to be strided
            math(8) r3<1>:hf r4.0<8;8,1>:f r6.0<8;4,2>:hf

            If type is hf, and stride is 1, assume it is packed, generate move with stride 2.
            */
            if (inst->isMath() &&
                srcOpnd->getType() == Type_HF &&
                srcOpnd->getRegion()->horzStride == 1)
            {
                inst->setSrc(insertMovBefore(instIter, i, Type_F, bb), i);
            }
        }
        /*
            10. [DevCHV:A]: When packed f16 is used as destination datatype, the subregister MUST be 0.
        */
        if(getGenxPlatform() == GENX_CHV    &&
            GetStepping() == Step_A         &&
            dst                             &&
            dst->getHorzStride() ==1        &&
            dst->getSubRegOff() != 0)
        {
            helperGenerateTempDst(bb, instIter, inst, 1, dst->getType());
        }

        /*
            12: [DevCHV, DevSKL]: Indirect Addressing on source is not supported when source and destination data types are mixed float.
        */
        if (getGenxPlatform() == GENX_CHV || getGenxPlatform() == GENX_SKL)
        {
            for (uint8_t i = 0; i < inst->getNumSrc(); ++i)
            {
                G4_Operand* src = inst->getSrc(i);
                if (src == nullptr || !src->isSrcRegRegion() || !src->asSrcRegRegion()->isIndirect())
                {
                    continue;
                }
                inst->setSrc(insertMovBefore(instIter, i, src->getType(), bb), i);
            }
        }

        if (inst->getDst()->getBase()->isRegVar()                   &&
            inst->getDst()->getType() == Type_HF                    &&
            inst->getDst()->getHorzStride() == 1)
        {
            if (VISA_WA_CHECK(builder.getPWaTable(), WaDstSubRegNumNotAllowedWithLowPrecPacked))
                inst->getDst()->getBase()->asRegVar()->getDeclare()->setSubRegAlign(Sixteen_Word);
            else
                inst->getDst()->getBase()->asRegVar()->getDeclare()->setSubRegAlign(Eight_Word);
        }
    }
}

// Fix for packed half types on BDW.
// Conversions from F to packed HF are not supported on this platform, 
// only unpacked HF is supported on destination.
// When we encounter an instruction with HF type on destination with <1> stride
// and float on source, add an additional mov that handles unpacking.
void HWConformity::fixPackedHFConversions(INST_LIST_ITER it, G4_BB* bb)
{
    G4_INST *inst = *it;
    G4_DstRegRegion* dst = inst->getDst();
    if (dst && dst->getType() == Type_HF && dst->getHorzStride() == 1 &&
        getTypeSize(inst->getExecType()) > 2)
    {
        helperGenerateTempDst(bb, it, inst, 2, Type_HF);
    }
}
