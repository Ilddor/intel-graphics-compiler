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

#include <iostream>
#include <sstream>
#include <list>

#include "visa_igc_common_header.h"
#include "Common_ISA_util.h"
#include "Common_ISA_framework.h"
#include "JitterDataStruct.h"
#include "VISAKernel.h"
#include "BuildIR.h"
#include "FlowGraph.h"
#include "common.h"
#include "Timer.h"

#include <cmath>  // std::ceil

using namespace vISA;
#define MESSAGE_SPECIFIC_CONTROL 8
#define SET_DATAPORT_MESSAGE_TYPE(dest, value)\
    dest |= value << 14;

#define START_ASSERT_CHECK \
    defined(_DEBUG) || defined(_INTERNAL)

#define MESSAGE_PRECISION_SUBTYPE_OFFSET 30

static const uint8_t mapExecSizeToNumElts[6] = {1, 2, 4, 8, 16, 32};

unsigned int IR_Builder::sampler8x8_group_id = 0;

static uint32_t createSamplerMsgDesc(
    VISASampler3DSubOpCode samplerOp,
    uint8_t execSize,
    bool isFP16Return,
    bool isFP16Input)
{
    // Now create message descriptor
    // 7:0 - BTI
    // 11:8 - Sampler Index
    // 16:12 - Message Type
    // 18:17 - SIMD Mode[0:1]
    // 19 - Header Present
    // 24:20 - Response Length
    // 28:25 - Message Length
    // 29 - SIMD Mode[2]
    // 30 - Return Format
    // 31 - CPS Message LOD Compensation Enable
    // We only set message type, SIMD mode, and return format here.  The other fields
    // are set in Create_Send_Inst_For_CISA as they are common with other send messages
    uint32_t fc = 0;

    fc |= ((uint32_t)samplerOp & 0x1f) << 12;

    if (execSize == 8)
    {
        fc |= (1 << 17);
    }
    else if (execSize == 16)
    {
        fc |= (2 << 17);
    }

    if (isFP16Return)
    {
        // 16-bit return type.  Note that this doesn't change the return length
        fc |= (1 << MESSAGE_PRECISION_SUBTYPE_OFFSET);
    }

    if (isFP16Input)
    {
#define SIMD_MODE_2_OFFSET 29
        fc |= (1 << SIMD_MODE_2_OFFSET);
    }

    return fc;
}

// IsSLMSurface - Check whether the given surface is SLM surface.
static bool IsSLMSurface(G4_Operand *surface) {
    // So far, it's only reliable to check an immediate surface.
    return surface->isImm() && surface->asImm()->getImm() == PREDEF_SURF_0;
}

// IsStatelessSurface - Check whether the give surface is statelesss surface.
static bool IsStatelessSurface(G4_Operand *surface) {
    // So far, it's only reliable to check an immediate surface.
    return surface->isImm() &&
        (surface->asImm()->getImm() == PREDEF_SURF_255 || surface->asImm()->getImm() == PREDEF_SURF_253);
}

static bool IsBindlessSurface(IR_Builder& builder, G4_Operand* surface)
{
	return surface->isSrcRegRegion() &&
		surface->asSrcRegRegion()->getBase()->asRegVar()->getDeclare() == builder.getBuiltinT252();
}

static bool IsNoMask(Common_VISA_EMask_Ctrl eMask) {
    switch (eMask) {
    case vISA_EMASK_M1_NM:
    case vISA_EMASK_M2_NM:
    case vISA_EMASK_M3_NM:
    case vISA_EMASK_M4_NM:
    case vISA_EMASK_M5_NM:
    case vISA_EMASK_M6_NM:
    case vISA_EMASK_M7_NM:
    case vISA_EMASK_M8_NM:
        return true;
    default:
        return false;
    }
}

int IR_Builder::translateVISAAddrInst(ISA_Opcode opcode, Common_ISA_Exec_Size executionSize,
                                      Common_VISA_EMask_Ctrl emask, G4_DstRegRegion *dstOpnd, G4_Operand *src0Opnd, G4_Operand *src1Opnd)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    unsigned int instOpt = 0;
    uint8_t exsize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    instOpt |= Get_Gen4_Emask(emask, exsize);

    if( src1Opnd                &&
        src0Opnd->isAddrExp()   &&
        src1Opnd->isImm()       )
    {
        src0Opnd->asAddrExp()->setOffset( src0Opnd->asAddrExp()->getOffset() + (int)src1Opnd->asImm()->getInt() );
        src1Opnd = NULL;
    }

    if( src0Opnd->isAddrExp() &&
        src1Opnd == NULL  )
        //if(0)
    {
        last_inst = createInst(
            NULL,
            G4_mov,
            NULL,
            false,
            exsize,
            dstOpnd,
            src0Opnd,
            NULL,
            instOpt,
            0);
    }
    else
    {
        last_inst = createInst(
            NULL,
            Get_G4_Opcode_From_Common_ISA_Opcode((ISA_Opcode)opcode),
            NULL,
            false,
            exsize,
            dstOpnd,
            src0Opnd,
            src1Opnd,
            instOpt,
            0);
    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

void IR_Builder::expandFdiv(uint8_t exsize, G4_Predicate *predOpnd, bool saturate,
    G4_DstRegRegion *dstOpnd, G4_Operand *src0Opnd, G4_Operand *src1Opnd, uint32_t instOpt)
{
    // math.fdiv dst src0 src1
    // -->
    // math.inv tmp src1
    // mul dst src0 tmp
    G4_MathOp mathOp = MATH_INV;
    G4_Type invType = src1Opnd->getType();
    if (IS_VFTYPE(invType))
    {
        invType = Type_F;
    }
    G4_Declare* invResult = createTempVar(exsize, invType, Either, Any);
    G4_DstRegRegion* invDst = Create_Dst_Opnd_From_Dcl(invResult, 1);
    createMathInst(predOpnd, false, exsize, invDst, src1Opnd, createNullSrc(invType), mathOp, instOpt);
    G4_SrcRegRegion* invSrc = Create_Src_Opnd_From_Dcl(invResult, getRegionStride1());
    last_inst = createInst(duplicateOperand(predOpnd), G4_mul, nullptr, saturate, exsize, dstOpnd, src0Opnd, invSrc, instOpt);
}

void IR_Builder::expandPow(uint8_t exsize, G4_Predicate *predOpnd, bool saturate,
    G4_DstRegRegion *dstOpnd, G4_Operand *src0Opnd, G4_Operand *src1Opnd, uint32_t instOpt)
{
    // math.pow dst src0 src1
    // -->
    // math.log tmp abs(src0)
    // mul dst tmp tmp src1
    // math.exp dst tmp
    G4_Type mathType = src0Opnd->getType();
    G4_Declare* tmpVar = createTempVar(exsize, mathType, Either, Any);
    G4_DstRegRegion* logDst = Create_Dst_Opnd_From_Dcl(tmpVar, 1);
    G4_Operand* logSrc = src0Opnd;
    // make sure log source is positive
    if (src0Opnd->isSrcRegRegion())
    {
        G4_SrcRegRegion* srcRegion = src0Opnd->asSrcRegRegion();
        switch (srcRegion->getModifier())
        {
        case Mod_src_undef:
            srcRegion->setModifier(Mod_Abs);
            break;
        case Mod_Abs:
            // do nothing
            break;
        default:
        {
            G4_Declare* tmpLogSrc = createTempVar(exsize, src0Opnd->getType(), Either, Any);
            Create_MOV_Inst(tmpLogSrc, 0, 0, exsize, nullptr, nullptr, src0Opnd);
            logSrc = Create_Src_Opnd_From_Dcl(tmpLogSrc, getRegionStride1());
            logSrc->asSrcRegRegion()->setModifier(Mod_Abs);
        }
        }
    }
    else
    {
        switch (src0Opnd->getType())
        {
            case Type_F:
            {
                float val = src0Opnd->asImm()->getFloat();
                if (val < 0)
                {
                    logSrc = createImm(std::abs(val));
                }
                break;
            }
            case Type_HF:
            {
                uint16_t val = (uint16_t) src0Opnd->asImm()->getImm();
                if (val & 0x8000)
                {
                    logSrc = createImm(val & 0x7FFFF, Type_HF);
                }
                break;
            }
            default:
                assert(false && "unexpected src0 type for pow");     
        }
    }
    createMathInst(predOpnd, false, exsize, logDst, logSrc, createNullSrc(mathType), MATH_LOG, instOpt);
    G4_SrcRegRegion* mulSrc = Create_Src_Opnd_From_Dcl(tmpVar, getRegionStride1());
    G4_DstRegRegion* mulDst = Create_Dst_Opnd_From_Dcl(tmpVar, 1);
    createInst(duplicateOperand(predOpnd), G4_mul, nullptr, false, exsize, mulDst, mulSrc, src1Opnd, instOpt);
    G4_SrcRegRegion* expSrc = Create_Src_Opnd_From_Dcl(tmpVar, getRegionStride1());
    last_inst = createMathInst(duplicateOperand(predOpnd), saturate, exsize, dstOpnd, expSrc, createNullSrc(mathType), MATH_EXP, instOpt);
}



int IR_Builder::translateVISAArithmeticInst(ISA_Opcode opcode, Common_ISA_Exec_Size executionSize,
                                            Common_VISA_EMask_Ctrl emask, G4_Predicate *predOpnd,
                                            bool saturate, G4_CondMod* condMod, G4_DstRegRegion *dstOpnd, G4_Operand *src0Opnd, G4_Operand *src1Opnd, G4_Operand *src2Opnd, G4_DstRegRegion *carryBorrow)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    unsigned int instOpt = 0;
    uint8_t exsize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    instOpt |= Get_Gen4_Emask(emask, exsize);

    if( IsMathInst(opcode) )
    {
        if( src1Opnd == NULL )
        {
            // create a null operand
            src1Opnd = createNullSrc(src0Opnd->getType());
        }

        G4_MathOp mathOp = Get_MathFuncCtrl( opcode, dstOpnd->getType() );

        if (!hasFdivPow() && mathOp == MATH_FDIV)
        {
            expandFdiv(exsize, predOpnd, saturate, dstOpnd, src0Opnd, src1Opnd, instOpt);
        }
        else if (!hasFdivPow() && mathOp == MATH_POW)
        {
            expandPow(exsize, predOpnd, saturate, dstOpnd, src0Opnd, src1Opnd, instOpt);
        } 
        else
        {
            last_inst = createMathInst(
                predOpnd,
                saturate,
                exsize,
                dstOpnd,
                src0Opnd,
                src1Opnd,
                mathOp,
                instOpt);
        }
    }
    else if( ISA_Inst_Table[opcode].n_srcs == 3 )
    {
        // do not check type of sources, float and integer are supported
        last_inst = createInst(
            predOpnd,
            Get_G4_Opcode_From_Common_ISA_Opcode(opcode),
            condMod,
            saturate,
            exsize,
            dstOpnd,
            src0Opnd,
            src1Opnd,
            src2Opnd,
            instOpt,
            0);
    }
    else
    {
        // create inst
        last_inst = createInst(
            predOpnd,
            Get_G4_Opcode_From_Common_ISA_Opcode(opcode),
            condMod,   
            saturate,
            exsize,
            dstOpnd,
            src0Opnd,
            src1Opnd,
            instOpt,
            0);

        if(opcode == ISA_ADDC || opcode == ISA_SUBB)
        {
            G4_DstRegRegion *accDstOpnd = createDstRegRegion( Direct,
                phyregpool.getAcc0Reg(),
                0,
                0,
                1,
                dstOpnd->getType() );

            last_inst->setImplAccDst( accDstOpnd );
            last_inst->setOptionOn(InstOpt_AccWrCtrl);

            //mov dst acc
			G4_SrcRegRegion *accSrcOpnd = createSrcRegRegion(Mod_src_undef,
				Direct,
				phyregpool.getAcc0Reg(),
				0,
				0,
                getRegionStride1(),
				dstOpnd->getType());

            createInst(
                NULL,
                G4_mov,
                NULL,
                false,
                exsize,
                carryBorrow,
                accSrcOpnd,
                NULL,
                instOpt,
                last_inst->getLineNo());
        }

    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

//
// convert src into a direct packed region
//
static G4_SrcRegRegion* operandToDirectSrcRegRegion(
    IR_Builder& builder, G4_Operand* src, uint8_t exsize)
{
    if (src->isSrcRegRegion())
    {
        G4_SrcRegRegion* srcRegion = src->asSrcRegRegion();
        if (srcRegion->getRegAccess() == IndirGRF)
        {
            G4_Declare* dcl = builder.createTempVarWithNoSpill(exsize, src->getType(), Either, Any);
            builder.Create_MOV_Inst(dcl, 0, 0, exsize, nullptr, nullptr, src, true);
            return builder.Create_Src_Opnd_From_Dcl(dcl, builder.getRegionStride1());
        }
        return src->asSrcRegRegion();
    }
    else
    {
        //src is an immediate
        MUST_BE_TRUE(src->isImm(), "expect immediate operand");
        G4_Declare *tmpSrc = builder.createTempVarWithNoSpill(exsize, src->getType(), Either, Any);
        builder.Create_MOV_Inst(tmpSrc, 0, 0, exsize, nullptr, nullptr, src, true);
        return builder.Create_Src_Opnd_From_Dcl(tmpSrc, builder.getRegionStride1());
    }
}

int IR_Builder::translateVISAArithmeticDoubleInst(ISA_Opcode opcode, Common_ISA_Exec_Size executionSize,
                                                  Common_VISA_EMask_Ctrl emask, G4_Predicate *predOpnd,
                                                  bool saturate, G4_CondMod* condMod, G4_DstRegRegion *dstOpnd, G4_Operand *src0Opnd, G4_Operand *src1Opnd)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    G4_INST* inst;
    uint8_t instExecSize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    const uint8_t exsize = 4; // exsize is a constant and never changed
    unsigned int instOpt = Get_Gen4_Emask(emask, 4); // for insts of execution size of element_size before the loop
    RegionDesc *srcRegionDesc = getRegionStride1();
    RegionDesc *rdAlign16 = getRegionStride1();
    uint8_t element_size;       // element_size is set according to instExecSize
    unsigned int loopCount;

    int line_no = last_inst ? last_inst->getLineNo() : 0;
    G4_Imm *dbl_constant_0 = createDFImm(0.0);
    G4_Imm *dbl_constant_1 = createDFImm(1.0);
    G4_Align reg_align = Either;
    if ( instExecSize == 1 || instExecSize == 4 )
    {
        element_size = 4;
        loopCount = 1;
    }
    else
    {
        ASSERT_USER(instExecSize == 8, "simd2 and simd16 support will be added later");
        element_size = 8;
        loopCount = 2;
    }

    // pred and conModifier
    G4_Declare *tmpFlag = createTempFlag(2);
    G4_Predicate_Control predCtrlValue = PRED_DEFAULT;

    // temp registers
    G4_Declare *t6  = createTempVarWithNoSpill( element_size, Type_DF, reg_align, Any );
    G4_Declare *t7  = createTempVarWithNoSpill( element_size, Type_DF, reg_align, Any );
    G4_Declare *t8  = createTempVarWithNoSpill( element_size, Type_DF, reg_align, Any );
    G4_Declare *t9  = createTempVarWithNoSpill( element_size, Type_DF, reg_align, Any );
    G4_Declare *t10 = createTempVarWithNoSpill( element_size, Type_DF, reg_align, Any );
    G4_Declare *t11 = createTempVarWithNoSpill( element_size, Type_DF, reg_align, Any );
    G4_Declare *t12 = createTempVarWithNoSpill( element_size, Type_DF, reg_align, Any );
    G4_Declare *t13 = createTempVarWithNoSpill( element_size, Type_DF, reg_align, Any );

    // cr0.0 register
    G4_Declare *regCR0  = createTempVarWithNoSpill( 1, Type_UD, reg_align, Any );
    G4_DstRegRegion regDstCR0(Direct, phyregpool.getCr0Reg(), 0, 0, 1, Type_UD );
    G4_DstRegRegion tmpDstRegForCR0(Direct, regCR0->getRegVar(), 0, 0, 1, Type_UD );
    G4_SrcRegRegion regSrcCR0(Mod_src_undef, Direct, phyregpool.getCr0Reg() ,0, 0, getRegionScalar(), Type_UD );
    G4_SrcRegRegion tmpSrcRegForCR0(Mod_src_undef, Direct, regCR0->getRegVar(),0, 0, getRegionScalar(), Type_UD );

    // 0.0:df and 1.0:df constants
    G4_Declare *t0  = createTempVarWithNoSpill( 4, Type_DF, reg_align, Any );
    G4_Declare *t1  = createTempVarWithNoSpill( 4, Type_DF, reg_align, Any );

	inst = createPseudoKills({ t0, t1, t6, t7, t8, t9, t10, t11, t12, t13 });

    G4_DstRegRegion tdst0(Direct, t0->getRegVar(), 0, 0, 1, Type_DF );
    G4_DstRegRegion tdst1(Direct, t1->getRegVar(), 0, 0, 1, Type_DF );
    G4_SrcRegRegion tsrc0(Mod_src_undef, Direct, t0->getRegVar(), 0, 0, srcRegionDesc, Type_DF );
    G4_SrcRegRegion tsrc1(Mod_src_undef, Direct, t1->getRegVar(), 0, 0, srcRegionDesc, Type_DF );

    G4_DstRegRegion *t0DstOpnd = createDstRegRegion(tdst0);
    G4_DstRegRegion *t1DstOpnd = createDstRegRegion(tdst1);

    // those are for drcp
    G4_SrcRegRegion valueOneScalarReg(Mod_src_undef, Direct, t1->getRegVar(), 0, 0, getRegionScalar(), Type_DF );
    G4_Operand *valueOneOpnd = createSrcRegRegion(valueOneScalarReg); // it is used in drcp

    // r0 = 0.0:df, r1 = 1.0:df
    // NOTE: 'NoMask' is required as constants are required for splitting
    // parts. Once they are in diverged branches, it won't be properly
    // initialized without 'NoMask'.
    inst = createInst(NULL, G4_mov, NULL, false, exsize, t0DstOpnd, dbl_constant_0,
        NULL, InstOpt_WriteEnable, line_no); // (4) {NoMask}

    createInst(NULL, G4_mov, NULL, false, exsize, t1DstOpnd, dbl_constant_1,
        NULL, InstOpt_WriteEnable, line_no); // (4) {NoMask}

    if ( src0Opnd == NULL )
    {
        src0Opnd = valueOneOpnd;
    }

    G4_SrcRegRegion* src0RR = operandToDirectSrcRegRegion(*this, src0Opnd, element_size);
    G4_SrcRegRegion* src1RR = operandToDirectSrcRegRegion(*this, src1Opnd, element_size);

    // src operand registers
    G4_DstRegRegion tdst_src0(Direct, t6->getRegVar(), 0, 0, 1, Type_DF );
    G4_DstRegRegion tdst_src1(Direct, t7->getRegVar(), 0, 0, 1, Type_DF );

    bool needsSrc0Move = src0RR->isScalar() || src0RR->getModifier() != Mod_src_undef;
    if (needsSrc0Move)
    {
        if (opcode == ISA_DIV || opcode == ISA_DIVM)
        {
            G4_DstRegRegion *t6_dst_src0_opnd = createDstRegRegion(tdst_src0);
            inst = createInst(NULL, G4_mov, NULL, false, element_size, t6_dst_src0_opnd, src0RR,
                NULL, instOpt, line_no); // mov (element_size) t6_dst_src0_opnd, src0RR {Q1/N1}
        }
    }
    bool needsSrc1Move = src1RR->isScalar() || src1RR->getModifier() != Mod_src_undef;
    if (needsSrc1Move)
    {
        G4_DstRegRegion *t7_dst_src1_opnd = createDstRegRegion(tdst_src1);
        inst = createInst(NULL, G4_mov, NULL, false, element_size, t7_dst_src1_opnd, src1RR,
            NULL, instOpt, line_no); // mov (element_size) t7_dst_src1_opnd, src1RR {Q1/N1}
    }

    // final result is at r8.noacc
    G4_SrcRegRegion tsrc8_final(Mod_src_undef, Direct, t8->getRegVar(), 0, 0, getRegionStride1(), t8->getElemType() );
    G4_SrcRegRegion *t8_src_opnd_final = createSrcRegRegion(tsrc8_final);

    // each madm only handles 4 channel double data
    Common_VISA_EMask_Ctrl currEMask = emask;
    for (uint16_t regIndex = 0; currEMask != vISA_NUM_EMASK && regIndex < loopCount;
        ++regIndex, currEMask = Get_Next_EMask(currEMask, exsize))
    {
        instOpt = Get_Gen4_Emask(currEMask, exsize);
        instOpt |= IsNoMask(emask) ? InstOpt_WriteEnable : 0; // setting channels for non-mad insts
        unsigned int madmInstOpt = instOpt; // setting channels for mad insts

        G4_DstRegRegion tdst6(Direct, t6->getRegVar(), regIndex, 0, 1, Type_DF);
        G4_DstRegRegion tdst7(Direct, t7->getRegVar(), regIndex, 0, 1, Type_DF );
        G4_DstRegRegion tdst8(Direct, t8->getRegVar(), regIndex, 0, 1, Type_DF );
        G4_DstRegRegion tdst9(Direct, t9->getRegVar(), regIndex, 0, 1, Type_DF );
        G4_DstRegRegion tdst10(Direct, t10->getRegVar(), regIndex, 0, 1, Type_DF );
        G4_DstRegRegion tdst11(Direct, t11->getRegVar(), regIndex, 0, 1, Type_DF );
        G4_DstRegRegion tdst12(Direct, t12->getRegVar(), regIndex, 0, 1, Type_DF );
        G4_DstRegRegion tdst13(Direct, t13->getRegVar(), regIndex, 0, 1, Type_DF );

        /* below 2 are prepared for G4_math with Align16, so the region 2;2,1 is used not 4;4,1.*/
        G4_SrcRegRegion tsrc6_0(Mod_src_undef, Direct, t6->getRegVar(), regIndex, 0, rdAlign16, Type_DF );
        G4_SrcRegRegion tsrc7_0(Mod_src_undef, Direct, t7->getRegVar(), regIndex, 0, rdAlign16, Type_DF );

        G4_SrcRegRegion tsrc6(Mod_src_undef, Direct, t6->getRegVar(), regIndex, 0, srcRegionDesc, Type_DF );
        G4_SrcRegRegion tsrc7(Mod_src_undef, Direct, t7->getRegVar(), regIndex, 0, srcRegionDesc, Type_DF );
        G4_SrcRegRegion tsrc8(Mod_src_undef, Direct, t8->getRegVar(), regIndex, 0, srcRegionDesc, Type_DF );
        G4_SrcRegRegion tsrc9(Mod_src_undef, Direct, t9->getRegVar(), regIndex, 0, srcRegionDesc, Type_DF );
        G4_SrcRegRegion tsrc10(Mod_src_undef, Direct, t10->getRegVar(), regIndex, 0, srcRegionDesc, Type_DF );
        G4_SrcRegRegion tsrc11(Mod_src_undef, Direct, t11->getRegVar(), regIndex, 0, srcRegionDesc, Type_DF );
        G4_SrcRegRegion tsrc12(Mod_src_undef, Direct, t12->getRegVar(), regIndex, 0, srcRegionDesc, Type_DF );
        G4_SrcRegRegion tsrc13(Mod_src_undef, Direct, t13->getRegVar(), regIndex, 0, srcRegionDesc, Type_DF );

        G4_DstRegRegion *t8DstOpnd0 = createDstRegRegion(tdst8);
        G4_DstRegRegion *t8DstOpnd1 = createDstRegRegion(tdst8);
        G4_DstRegRegion *t8DstOpnd2 = createDstRegRegion(tdst8);
        G4_DstRegRegion *t9DstOpnd0 = createDstRegRegion(tdst9);
        G4_DstRegRegion *t9DstOpnd1 = createDstRegRegion(tdst9);
        G4_DstRegRegion *t10DstOpnd0 = createDstRegRegion(tdst10);
        G4_DstRegRegion *t11DstOpnd0 = createDstRegRegion(tdst11);
        G4_DstRegRegion *t11DstOpnd1 = createDstRegRegion(tdst11);
        G4_DstRegRegion *t12DstOpnd0 = createDstRegRegion(tdst12);
        G4_DstRegRegion *t12DstOpnd1 = createDstRegRegion(tdst12);
        G4_DstRegRegion *t13DstOpnd0 = createDstRegRegion(tdst13);

        // src oprands passed by function calls
        // for INV instruction, src0 should be 1, contant value.
        /* below 2 are prepared for G4_math with Align16, so the region 2;2,1 is used not 4;4,1.*/
        G4_SrcRegRegion fsrc0_0(Mod_src_undef, Direct, src0RR->getBase(), src0RR->asSrcRegRegion()->getRegOff() + ((opcode == ISA_INV) ? 0 : regIndex), 0, rdAlign16, Type_DF);
        G4_SrcRegRegion fsrc1_0(Mod_src_undef, Direct, src1RR->getBase(), src1RR->asSrcRegRegion()->getRegOff() + regIndex, 0, rdAlign16, Type_DF);

        G4_SrcRegRegion fsrc0(Mod_src_undef, Direct, src0RR->getBase(), src0RR->asSrcRegRegion()->getRegOff() + ((opcode == ISA_INV) ? 0 : regIndex), 0, srcRegionDesc, Type_DF);
        G4_SrcRegRegion fsrc1(Mod_src_undef, Direct, src1RR->getBase(), src1RR->asSrcRegRegion()->getRegOff() + regIndex, 0, srcRegionDesc, Type_DF);

        G4_SrcRegRegion *t6SrcOpnd0 = NULL;
        G4_SrcRegRegion *t6SrcOpnd1 = NULL;
        G4_SrcRegRegion *t6SrcOpnd2 = NULL;
        G4_SrcRegRegion *t6SrcOpnd3 = NULL;
        G4_SrcRegRegion *t7SrcOpnd0 = NULL;
        G4_SrcRegRegion *t8SrcOpnd0x0 = createSrcRegRegion(tsrc8);
        G4_SrcRegRegion *t8SrcOpnd0x1 = createSrcRegRegion(tsrc8);
        G4_SrcRegRegion *t8SrcOpnd0x2 = createSrcRegRegion(tsrc8);
        G4_SrcRegRegion *t8SrcOpnd0x3 = createSrcRegRegion(tsrc8);
        G4_SrcRegRegion *t8SrcOpnd0x4 = createSrcRegRegion(tsrc8);
        G4_SrcRegRegion *t8SrcOpnd1 = createSrcRegRegion(tsrc8);
        G4_SrcRegRegion *t9SrcOpnd0x0 = createSrcRegRegion(tsrc9);
        G4_SrcRegRegion *t9SrcOpnd0x1 = createSrcRegRegion(tsrc9);
        G4_SrcRegRegion *t9SrcOpnd1x0 = createSrcRegRegion(tsrc9);
        G4_SrcRegRegion *t9SrcOpnd1x1 = createSrcRegRegion(tsrc9);
        G4_SrcRegRegion *t10SrcOpnd0 = createSrcRegRegion(tsrc10);
        G4_SrcRegRegion *t10SrcOpnd1 = createSrcRegRegion(tsrc10);
        G4_SrcRegRegion *t11SrcOpnd0 = createSrcRegRegion(tsrc11);
        G4_SrcRegRegion *t11SrcOpnd1 = createSrcRegRegion(tsrc11);
        G4_SrcRegRegion *t12SrcOpnd0x0 = createSrcRegRegion(tsrc12);
        G4_SrcRegRegion *t12SrcOpnd0x1 = createSrcRegRegion(tsrc12);
        G4_SrcRegRegion *t12SrcOpnd0x2 = createSrcRegRegion(tsrc12);
        G4_SrcRegRegion *t12SrcOpnd0x3 = createSrcRegRegion(tsrc12);
        G4_SrcRegRegion *t12SrcOpnd1 = createSrcRegRegion(tsrc12);
        G4_SrcRegRegion *t13SrcOpnd0 = createSrcRegRegion(tsrc13);

        G4_DstRegRegion *cr0DstRegOpndForAndInst = createDstRegRegion(regDstCR0);
        G4_DstRegRegion *cr0DstRegOpndForOrInst = createDstRegRegion(regDstCR0);
        G4_DstRegRegion *cr0DstRegOpndForRestoreIfInst = createDstRegRegion(regDstCR0);
        G4_DstRegRegion *cr0DstRegOpndForRestoreElseInst = createDstRegRegion(regDstCR0);
        G4_SrcRegRegion *cr0SrcRegOpndForSaveInst = createSrcRegRegion(regSrcCR0);
        G4_SrcRegRegion *cr0SrcRegOpndForAndInst = createSrcRegRegion(regSrcCR0);
        G4_SrcRegRegion *cr0SrcRegOpndForOrInst = createSrcRegRegion(regSrcCR0);

        G4_DstRegRegion *tmpDstRegOpndForCR0= createDstRegRegion(tmpDstRegForCR0);
        G4_SrcRegRegion *tmpSrcOpndForCR0OnIf = createSrcRegRegion(tmpSrcRegForCR0);
        G4_SrcRegRegion *tmpSrcOpndForCR0OnElse = createSrcRegRegion(tmpSrcRegForCR0);

        // save cr0.0

        inst = createInst(NULL, G4_mov, NULL, false, 1, tmpDstRegOpndForCR0, cr0SrcRegOpndForSaveInst,
            NULL, InstOpt_WriteEnable, line_no); // {NoMask}

        // set rounding mod in CR0 to RNE
        inst = createInst(NULL, G4_and, NULL, false, 1, cr0DstRegOpndForAndInst, cr0SrcRegOpndForAndInst,
            createImm(0xffffffcf, Type_UD), InstOpt_WriteEnable, line_no); // {NoMask}

        // set double precision denorm mode to 1
        inst = createInst(NULL, G4_or, NULL, false, 1, cr0DstRegOpndForOrInst, cr0SrcRegOpndForOrInst,
            createImm(0x40, Type_UD), InstOpt_WriteEnable, line_no); // {NoMask}

        if (needsSrc0Move)
        {
            if (opcode == ISA_INV)
            {
                t6SrcOpnd0 = createSrcRegRegion(fsrc0_0);
                t6SrcOpnd1 = createSrcRegRegion(fsrc0);
                t6SrcOpnd2 = createSrcRegRegion(fsrc0);
                t6SrcOpnd3 = createSrcRegRegion(fsrc0);
            }
            else
            {
                t6SrcOpnd0 = createSrcRegRegion(tsrc6_0);
                t6SrcOpnd1 = createSrcRegRegion(tsrc6);
                t6SrcOpnd2 = createSrcRegRegion(tsrc6);
                t6SrcOpnd3 = createSrcRegRegion(tsrc6);
            }
        }
        else
        {
            t6SrcOpnd0 = createSrcRegRegion(fsrc0_0);
            t6SrcOpnd1 = createSrcRegRegion(fsrc0);
            t6SrcOpnd2 = createSrcRegRegion(fsrc0);
            t6SrcOpnd3 = createSrcRegRegion(fsrc0);
        }

        if (needsSrc1Move)
        {
            t7SrcOpnd0 = createSrcRegRegion(tsrc7_0);
        }
        else
        {
            t7SrcOpnd0 = createSrcRegRegion(fsrc1_0);
        }

        // create -r7.noacc
        G4_SrcRegRegion tsrc7_neg( Mod_Minus,
            t7SrcOpnd0->asSrcRegRegion()->getRegAccess(),
            t7SrcOpnd0->asSrcRegRegion()->getBase(),
            t7SrcOpnd0->asSrcRegRegion()->getRegOff(),
            t7SrcOpnd0->asSrcRegRegion()->getSubRegOff(),
            t7SrcOpnd0->asSrcRegRegion()->getRegion(),
            t7SrcOpnd0->getType() );
        G4_SrcRegRegion *t7SrcOpndNeg0 = createSrcRegRegion( tsrc7_neg );
        G4_SrcRegRegion *t7SrcOpndNeg1 = createSrcRegRegion( tsrc7_neg );
        G4_SrcRegRegion *t7SrcOpndNeg2 = createSrcRegRegion( tsrc7_neg );
        G4_SrcRegRegion *t7SrcOpndNeg3 = createSrcRegRegion( tsrc7_neg );

        // math.e0.f0.0 (4) r8.acc2 r6.noacc r7.noacc 0xe {Align16, N1/N2}
        t8DstOpnd0->setAccRegSel( ACC2 );
        t6SrcOpnd0->setAccRegSel( NOACC );
        t7SrcOpnd0->setAccRegSel( NOACC );
        inst = createMathInst(NULL, false, exsize, t8DstOpnd0, t6SrcOpnd0,
            t7SrcOpnd0, MATH_INVM, madmInstOpt, line_no);
        G4_CondMod *condModOverflow = createCondMod(Mod_o, tmpFlag->getRegVar(), 0);
        inst->setCondMod(condModOverflow);

        // if
        G4_Predicate *predicateFlagReg = createPredicate(PredState_Minus, tmpFlag->getRegVar(), 0, predCtrlValue);
        inst = createInst(predicateFlagReg, G4_if, NULL, false, exsize, NULL, NULL, NULL, NULL, instOpt, line_no);

        // madm (4) r9.acc3 r0.noacc r6.noacc r8.acc2 {Align16, N1/N2}
        G4_SrcRegRegion *t0SrcOpnd = createSrcRegRegion(tsrc0);
        t9DstOpnd0->setAccRegSel( ACC3 );
        t0SrcOpnd->setAccRegSel( NOACC );
        t6SrcOpnd1->setAccRegSel( NOACC );
        t8SrcOpnd0x0->setAccRegSel( ACC2 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t9DstOpnd0, t0SrcOpnd,
            t6SrcOpnd1, t8SrcOpnd0x0, madmInstOpt, line_no);

        // madm (4) r10.acc4 r1.noacc -r7.noacc r8.acc2 {Align16, N1/N2}
        G4_SrcRegRegion *t1SrcOpnd0 = createSrcRegRegion(tsrc1);
        t10DstOpnd0->setAccRegSel( ACC4 );
        t1SrcOpnd0->setAccRegSel( NOACC );
        t7SrcOpndNeg0->setAccRegSel( NOACC );
        t8SrcOpnd0x1->setAccRegSel( ACC2 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t10DstOpnd0, t1SrcOpnd0,
            t7SrcOpndNeg0, t8SrcOpnd0x1, madmInstOpt, line_no);

        // madm (4) r11.acc5 r6.noacc -r7.noacc r9.acc3 {Align16, N1/N2}
        t11DstOpnd0->setAccRegSel( ACC5 );
        t6SrcOpnd2->setAccRegSel( NOACC );
        t7SrcOpndNeg1->setAccRegSel( NOACC );
        t9SrcOpnd0x0->setAccRegSel( ACC3 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t11DstOpnd0, t6SrcOpnd2,
            t7SrcOpndNeg1, t9SrcOpnd0x0, madmInstOpt, line_no);

        // madm (4) r12.acc6 r8.acc2 r10.acc4 r8.acc2 {Align16, N1/N2}
        t12DstOpnd0->setAccRegSel( ACC6 );
        t8SrcOpnd0x2->setAccRegSel( ACC2 );
        t10SrcOpnd0->setAccRegSel( ACC4 );
        t8SrcOpnd0x3->setAccRegSel( ACC2 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t12DstOpnd0, t8SrcOpnd0x2,
            t10SrcOpnd0, t8SrcOpnd0x3, madmInstOpt, line_no);

        // madm (4) r13.acc7 r1.noacc -r7.noacc r12.acc6 {Align16, N1/N2}
        G4_SrcRegRegion *t1SrcOpnd1 = createSrcRegRegion(tsrc1);
        t13DstOpnd0->setAccRegSel( ACC7 );
        t1SrcOpnd1->setAccRegSel( NOACC );
        t7SrcOpndNeg2->setAccRegSel( NOACC );
        t12SrcOpnd0x0->setAccRegSel( ACC6 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t13DstOpnd0, t1SrcOpnd1,
            t7SrcOpndNeg2, t12SrcOpnd0x0, madmInstOpt, line_no);

        // madm (4) r8.acc8 r8.acc2 r10.acc4 r12.acc6 {Align16, N1/N2}
        t8DstOpnd1->setAccRegSel( ACC8 );
        t8SrcOpnd0x4->setAccRegSel( ACC2 );
        t10SrcOpnd1->setAccRegSel( ACC4 );
        t12SrcOpnd0x1->setAccRegSel( ACC6 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t8DstOpnd1, t8SrcOpnd0x4,
            t10SrcOpnd1, t12SrcOpnd0x1, madmInstOpt, line_no);

        // madm (4) r9.acc9 r9.acc3 r11.acc5 r12.acc6 {Align16, N1/N2}
        t9DstOpnd1->setAccRegSel( ACC9 );
        t9SrcOpnd0x1->setAccRegSel( ACC3 );
        t11SrcOpnd0->setAccRegSel( ACC5 );
        t12SrcOpnd0x2->setAccRegSel( ACC6 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t9DstOpnd1, t9SrcOpnd0x1,
            t11SrcOpnd0, t12SrcOpnd0x2, madmInstOpt, line_no);

        // madm (4) r12.acc2 r12.acc6 r8.acc8 r13.acc7 {Align16, N1/N2}
        t12DstOpnd1->setAccRegSel( ACC2 );
        t12SrcOpnd0x3->setAccRegSel( ACC6 );
        t8SrcOpnd1->setAccRegSel( ACC8 );
        t13SrcOpnd0->setAccRegSel( ACC7 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t12DstOpnd1, t12SrcOpnd0x3,
            t8SrcOpnd1, t13SrcOpnd0, madmInstOpt, line_no);

        // madm (4) r11.acc3 r6.noacc -r7.noacc r9.acc9 {Align16, N1/N2}
        t11DstOpnd1->setAccRegSel( ACC3 );
        t6SrcOpnd3->setAccRegSel( NOACC );
        t7SrcOpndNeg3->setAccRegSel( NOACC );
        t9SrcOpnd1x0->setAccRegSel( ACC9 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t11DstOpnd1, t6SrcOpnd3,
            t7SrcOpndNeg3, t9SrcOpnd1x0, madmInstOpt, line_no);

        // restore cr0.0
        inst = createInst(NULL, G4_mov, NULL, false, 1, cr0DstRegOpndForRestoreIfInst, tmpSrcOpndForCR0OnIf,
            NULL, InstOpt_WriteEnable, line_no);

        // madm (4) r8.noacc r9.acc9 r11.acc3 r12.acc2 {Align16, N1/N2}
        t8DstOpnd2->setAccRegSel( NOACC );
        t9SrcOpnd1x1->setAccRegSel( ACC9 );
        t11SrcOpnd1->setAccRegSel( ACC3 );
        t12SrcOpnd1->setAccRegSel( ACC2 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t8DstOpnd2, t9SrcOpnd1x1,
            t11SrcOpnd1, t12SrcOpnd1, madmInstOpt, line_no);

        // else (8) {Q1/Q2}
        inst = createInst(NULL, G4_else, NULL, false, exsize, NULL, NULL, NULL, NULL, instOpt, line_no);

        // restore cr0.0 {NoMask}
        inst = createInst(NULL, G4_mov, NULL, false, 1, cr0DstRegOpndForRestoreElseInst, tmpSrcOpndForCR0OnElse,
            NULL, InstOpt_WriteEnable, line_no);

        // endif (8) {Q1/Q2}
        inst = createInst(NULL, G4_endif, NULL, false, exsize, NULL, NULL, NULL, NULL, instOpt, line_no);
    };

    // make final copy to dst
    // dst = r8:df     mov (instExecSize) dstOpnd, t8_src_opnd_final {Q1/N1}
    t8_src_opnd_final->setAccRegSel(ACC_UNDEFINED);
    inst = createInst(predOpnd, G4_mov, condMod, saturate, instExecSize, dstOpnd, t8_src_opnd_final,
        NULL, Get_Gen4_Emask(emask, instExecSize), line_no);

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISAArithmeticSingleDivideIEEEInst(ISA_Opcode opcode, Common_ISA_Exec_Size executionSize,
                                                  Common_VISA_EMask_Ctrl emask, G4_Predicate *predOpnd,
                                                  bool saturate, G4_CondMod* condMod, G4_DstRegRegion *dstOpnd, G4_Operand *src0Opnd, G4_Operand *src1Opnd)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    G4_INST* inst;
    unsigned int instOpt = 0;
    unsigned madmInstOpt = 0;
    uint8_t instExecSize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    uint8_t element_size = 8; // element_size is changed according to insstExecSize
    uint8_t exsize = 8; // exsize is a constant and never changed
    unsigned int loopCount = 1;
    instOpt |= Get_Gen4_Emask(emask, 8); // for those execution size: element_size before the loop
    madmInstOpt |= Get_Gen4_Emask(emask, 8); // only used in the loop
    RegionDesc *srcRegionDesc = getRegionStride1();

    int line_no = last_inst ? last_inst->getLineNo() : 0;
    G4_Imm *flt_constant_0 = createImm(float(0.0));
    G4_Imm *flt_constant_1 = createImm(float(1.0));
    G4_Align reg_align = Either;
    if ( instExecSize == 1 || instExecSize == 8 )
    {
        element_size = 8;
        loopCount = 1;
    }
    else if ( instExecSize == 16 )
    {
        element_size = 16;
        instOpt = Get_Gen4_Emask(emask, 16);
        loopCount = 2;
    }

    // pred and conModifier
    G4_Declare *tmpFlag = createTempFlag(2);
    G4_Predicate_Control predCtrlValue = PRED_DEFAULT;

    // temp registers
    G4_Declare *t1  = createTempVarWithNoSpill( element_size, Type_F, reg_align, Any );
    G4_Declare *t4  = createTempVarWithNoSpill( element_size, Type_F, reg_align, Any );
    G4_Declare *t6  = createTempVarWithNoSpill( element_size, Type_F, reg_align, Any );
    G4_Declare *t8  = createTempVarWithNoSpill( element_size, Type_F, reg_align, Any );
    G4_Declare *t9  = createTempVarWithNoSpill( element_size, Type_F, reg_align, Any );
    G4_Declare *t10 = createTempVarWithNoSpill( element_size, Type_F, reg_align, Any );
    G4_Declare *t11 = createTempVarWithNoSpill( element_size, Type_F, reg_align, Any );

    // cr0.0 register
    G4_Declare *regCR0  = createTempVarWithNoSpill( 1, Type_UD, reg_align, Any );
    // Temporary Var for saving/restoring CR0 before/after setting denorm bits
    G4_Declare *regCR0Denorm = createTempVarWithNoSpill(1, Type_UD, reg_align, Any);

    // 0.0:f and 1.0:f constants
    G4_Declare *t2  = createTempVarWithNoSpill( 8, Type_F, reg_align, Any );
    G4_Declare *t5  = createTempVarWithNoSpill( 8, Type_F, reg_align, Any );

	inst = createPseudoKills({ t1, t2, t4, t5, t6, t8, t9, t10, t11 });

    // those are for drcp
    G4_SrcRegRegion valueOneScalarReg(Mod_src_undef, Direct, t2->getRegVar(), 0, 0, getRegionScalar(), Type_F );
    G4_Operand *valueOneOpnd = createSrcRegRegion(valueOneScalarReg); // it is used in drcp

    if ( src0Opnd == NULL )
    {
        src0Opnd = valueOneOpnd;
    }

    G4_SrcRegRegion* src0RR = operandToDirectSrcRegRegion(*this, src0Opnd, element_size);
    G4_SrcRegRegion* src1RR = operandToDirectSrcRegRegion(*this, src1Opnd, element_size);

    if (src0RR->isScalar() || src0RR->getModifier() != Mod_src_undef)
    {
        G4_DstRegRegion *tmp = Create_Dst_Opnd_From_Dcl(t6, 1);
        inst = createInst(NULL, G4_mov, NULL, false, element_size, tmp, src0RR,
            NULL, instOpt, line_no); // mov (element_size) t6, src0RR {Q1/H1}
        src0RR = Create_Src_Opnd_From_Dcl(t6, getRegionStride1());
    }
    if (src1RR->isScalar() || src1RR->getModifier() != Mod_src_undef)
    {
        G4_DstRegRegion *tmp = Create_Dst_Opnd_From_Dcl(t4, 1);
        inst = createInst(NULL, G4_mov, NULL, false, element_size, tmp, src1RR,
            NULL, instOpt, line_no); // mov (element_size) t4, src1RR {Q1/H1}
        src1RR = Create_Src_Opnd_From_Dcl(t4, getRegionStride1());
    }

    // cr0.0 register
    G4_DstRegRegion tmpDenormDstRegForCR0(Direct, regCR0Denorm->getRegVar(), 0, 0, 1, Type_UD);
    G4_SrcRegRegion tmpDenormSrcRegForCR0(
        Mod_src_undef, Direct, regCR0Denorm->getRegVar(), 0, 0, getRegionScalar(), Type_UD);
    G4_DstRegRegion regDstCR0(Direct, phyregpool.getCr0Reg(), 0, 0, 1, Type_UD );
    G4_DstRegRegion tmpDstRegForCR0(Direct, regCR0->getRegVar(), 0, 0, 1, Type_UD );
    G4_SrcRegRegion regSrcCR0(Mod_src_undef, Direct, phyregpool.getCr0Reg(), 0, 0, getRegionScalar(), Type_UD );
    G4_SrcRegRegion tmpSrcRegForCR0(Mod_src_undef, Direct, regCR0->getRegVar(), 0, 0, getRegionScalar(), Type_UD );

    // t2 and t5 are constants
    G4_DstRegRegion tdst2(Direct, t2->getRegVar(), 0, 0, 1, Type_F );
    G4_DstRegRegion tdst5(Direct, t5->getRegVar(), 0, 0, 1, Type_F );
    G4_SrcRegRegion tsrc2(Mod_src_undef, Direct, t2->getRegVar(), 0, 0, srcRegionDesc, Type_F );
    G4_SrcRegRegion tsrc5(Mod_src_undef, Direct, t5->getRegVar(), 0, 0, srcRegionDesc, Type_F );

    G4_DstRegRegion *t2DstOpnd = createDstRegRegion(tdst2);
    G4_DstRegRegion *t5DstOpnd = createDstRegRegion(tdst5);

    // r0 = 0.0:f, r1 = 1.0:f
    // NOTE: 'NoMask' is required as constants are required for splitting
    // parts. Once they are in diverged branches, it won't be properly
    // initialized without 'NoMask'.
    inst = createInst(NULL, G4_mov, NULL, false, exsize, t2DstOpnd, flt_constant_0,
        NULL, InstOpt_WriteEnable, line_no); // mov (8) r4.0<1>:f 0:f {NoMask}

    inst = createInst(NULL, G4_mov, NULL, false, exsize, t5DstOpnd, flt_constant_1,
        NULL, InstOpt_WriteEnable, line_no); // mov (8) r104.0<1>:f 0x3f800000:f {NoMask}

    G4_SrcRegRegion tsrc8_final(Mod_src_undef, Direct, t8->getRegVar(), 0, 0,
        getRegionStride1(), t8->getElemType() );
    G4_SrcRegRegion *t8_src_opnd_final = createSrcRegRegion(tsrc8_final);

    G4_DstRegRegion *tmpDenormDstRegOpndForCR0 = createDstRegRegion(tmpDenormDstRegForCR0);
    G4_SrcRegRegion *cr0DenormSrcRegOpndForSaveInst = createSrcRegRegion(regSrcCR0);
    G4_DstRegRegion *cr0DstRegOpndForOrInst = createDstRegRegion(regDstCR0);
    G4_SrcRegRegion *cr0SrcRegOpndForOrInst = createSrcRegRegion(regSrcCR0);
    G4_DstRegRegion *cr0DenormDstRegOpndForRestore = createDstRegRegion(regDstCR0);
    G4_SrcRegRegion *tmpDenormSrcRegOpndForCR0 = createSrcRegRegion(tmpDenormSrcRegForCR0);

    // IEEset float precision denorm mode to 1

    // save cr0.0 and make sure to set denorm bits
    inst = createInst(NULL, G4_mov, NULL, false, 1, tmpDenormDstRegOpndForCR0, cr0DenormSrcRegOpndForSaveInst,
        NULL, InstOpt_WriteEnable, line_no);

    inst = createInst(NULL, G4_or, NULL, false, 1, cr0DstRegOpndForOrInst, cr0SrcRegOpndForOrInst,
        createImm(0x80, Type_UD), InstOpt_WriteEnable, line_no); // {NoMask}

    Common_VISA_EMask_Ctrl currEMask = emask;
    for (uint16_t regIndex = 0; currEMask != vISA_NUM_EMASK && regIndex < loopCount;
            ++regIndex, currEMask = Get_Next_EMask(currEMask, exsize))
    {
        // set Q1 for insts within the 1st loop and Q2 for the 2nd, if inside SIMD CF
        instOpt = Get_Gen4_Emask(currEMask, exsize);
        instOpt |= IsNoMask(emask) ? InstOpt_WriteEnable : 0; // setting channels for non-mad insts
        madmInstOpt = instOpt; // setting channels for mad insts

        //1, 6, 8, 9, 10, 11
        G4_DstRegRegion tdst1(Direct, t1->getRegVar(), regIndex, 0, 1, Type_F );
        G4_DstRegRegion tdst6(Direct, t6->getRegVar(), regIndex, 0, 1, Type_F );
        G4_DstRegRegion tdst8(Direct, t8->getRegVar(), regIndex, 0, 1, Type_F );
        G4_DstRegRegion tdst9(Direct, t9->getRegVar(), regIndex, 0, 1, Type_F );
        G4_DstRegRegion tdst10(Direct, t10->getRegVar(), regIndex, 0, 1, Type_F );
        G4_DstRegRegion tdst11(Direct, t11->getRegVar(), regIndex, 0, 1, Type_F );

        G4_SrcRegRegion tsrc1(Mod_src_undef, Direct, t1->getRegVar(), regIndex, 0, srcRegionDesc, Type_F );
        G4_SrcRegRegion tsrc4(Mod_src_undef, Direct, t4->getRegVar(), regIndex, 0, srcRegionDesc, Type_F );
        G4_SrcRegRegion tsrc6(Mod_src_undef, Direct, t6->getRegVar(), regIndex, 0, srcRegionDesc, Type_F );
        G4_SrcRegRegion tsrc8(Mod_src_undef, Direct, t8->getRegVar(), regIndex, 0, srcRegionDesc, Type_F );
        G4_SrcRegRegion tsrc9(Mod_src_undef, Direct, t9->getRegVar(), regIndex, 0, srcRegionDesc, Type_F );
        G4_SrcRegRegion tsrc10(Mod_src_undef, Direct, t10->getRegVar(), regIndex, 0, srcRegionDesc, Type_F );
        G4_SrcRegRegion tsrc11(Mod_src_undef, Direct, t11->getRegVar(), regIndex, 0, srcRegionDesc, Type_F );

        G4_DstRegRegion *t8DstOpnd0 = createDstRegRegion(tdst8);
        G4_DstRegRegion *t9DstOpnd0 = createDstRegRegion(tdst9);
        G4_DstRegRegion *t10DstOpnd0 = createDstRegRegion(tdst10);
        G4_DstRegRegion *t1DstOpnd0 = createDstRegRegion(tdst1);
        G4_DstRegRegion *t11DstOpnd0 = createDstRegRegion(tdst11);
        G4_DstRegRegion *t9DstOpnd1 = createDstRegRegion(tdst9);
        G4_DstRegRegion *t6DstOpnd0 = createDstRegRegion(tdst6);
        G4_DstRegRegion *t8DstOpnd1 = createDstRegRegion(tdst8);

        // src oprands passed by function calls
        G4_SrcRegRegion fsrc0(Mod_src_undef, Direct, src0RR->getBase(), src0RR->asSrcRegRegion()->getRegOff() + regIndex, 0, srcRegionDesc, Type_F );
        G4_SrcRegRegion fsrc1(Mod_src_undef, Direct, src1RR->getBase(), src1RR->asSrcRegRegion()->getRegOff() + regIndex, 0, srcRegionDesc, Type_F );

        G4_SrcRegRegion *t4SrcOpnd0 = NULL;
        G4_SrcRegRegion *t6SrcOpnd0 = NULL;
        G4_SrcRegRegion *t6SrcOpnd1 = NULL;
        G4_SrcRegRegion *t6SrcOpnd2 = NULL;
        G4_SrcRegRegion *t6SrcOpnd3 = NULL;

        G4_SrcRegRegion *t1SrcOpnd0 = createSrcRegRegion(tsrc1);
        G4_SrcRegRegion *t1SrcOpnd1 = createSrcRegRegion(tsrc1);
        G4_SrcRegRegion *t6SrcOpnd4 = createSrcRegRegion(tsrc6);
        G4_SrcRegRegion *t8SrcOpnd0x0 = createSrcRegRegion(tsrc8);
        G4_SrcRegRegion *t8SrcOpnd0x1 = createSrcRegRegion(tsrc8);
        G4_SrcRegRegion *t8SrcOpnd0x2 = createSrcRegRegion(tsrc8);
        G4_SrcRegRegion *t8SrcOpnd0x3 = createSrcRegRegion(tsrc8);
        G4_SrcRegRegion *t9SrcOpnd0x0 = createSrcRegRegion(tsrc9);
        G4_SrcRegRegion *t9SrcOpnd0x1 = createSrcRegRegion(tsrc9);
        G4_SrcRegRegion *t9SrcOpnd1x0 = createSrcRegRegion(tsrc9);
        G4_SrcRegRegion *t9SrcOpnd1x1 = createSrcRegRegion(tsrc9);
        G4_SrcRegRegion *t10SrcOpnd0 = createSrcRegRegion(tsrc10);
        G4_SrcRegRegion *t11SrcOpnd0 = createSrcRegRegion(tsrc11);

        G4_DstRegRegion *cr0DstRegOpndForAndInst = createDstRegRegion(regDstCR0);
        G4_DstRegRegion *cr0DstRegOpndForRestoreIfInst = createDstRegRegion(regDstCR0);
        G4_DstRegRegion *cr0DstRegOpndForRestoreElseInst = createDstRegRegion(regDstCR0);
        G4_SrcRegRegion *cr0SrcRegOpndForSaveInst = createSrcRegRegion(regSrcCR0);
        G4_SrcRegRegion *cr0SrcRegOpndForAndInst = createSrcRegRegion(regSrcCR0);

        G4_DstRegRegion *tmpDstRegOpndForCR0 = createDstRegRegion(tmpDstRegForCR0);
        G4_SrcRegRegion *tmpSrcOpndForCR0OnIf = createSrcRegRegion(tmpSrcRegForCR0);
        G4_SrcRegRegion *tmpSrcOpndForCR0OnElse = createSrcRegRegion(tmpSrcRegForCR0);

        // save cr0.0: mov (1) r116.2<1>:ud cr0.0<0;1,0>:ud {NoMask}
        inst = createInst(NULL, G4_mov, NULL, false, 1, tmpDstRegOpndForCR0, cr0SrcRegOpndForSaveInst,
            NULL, InstOpt_WriteEnable, line_no);

        // set rounding mod in CR0 to RNE: and (1) cr0.0<1>:ud cr0.0<0;1,0>:ud 0xffffffcf:ud {NoMask}
        inst = createInst(NULL, G4_and, NULL, false, 1, cr0DstRegOpndForAndInst, cr0SrcRegOpndForAndInst,
            createImm(0xffffffcf, Type_UD), InstOpt_WriteEnable, line_no);

        t6SrcOpnd0 = createSrcRegRegion(fsrc0);
        t6SrcOpnd1 = createSrcRegRegion(fsrc0);
        t6SrcOpnd2 = createSrcRegRegion(fsrc0);
        t6SrcOpnd3 = createSrcRegRegion(fsrc0);

        t4SrcOpnd0 = createSrcRegRegion(fsrc1);


        // create -r4.noacc
        G4_SrcRegRegion tsrc4_neg( Mod_Minus,
            t4SrcOpnd0->asSrcRegRegion()->getRegAccess(),
            t4SrcOpnd0->asSrcRegRegion()->getBase(),
            t4SrcOpnd0->asSrcRegRegion()->getRegOff(),
            t4SrcOpnd0->asSrcRegRegion()->getSubRegOff(),
            t4SrcOpnd0->asSrcRegRegion()->getRegion(),
            t4SrcOpnd0->getType() );
        G4_SrcRegRegion *t4SrcOpndNeg0 = createSrcRegRegion( tsrc4_neg );
        G4_SrcRegRegion *t4SrcOpndNeg1 = createSrcRegRegion( tsrc4_neg );
        G4_SrcRegRegion *t4SrcOpndNeg2 = createSrcRegRegion( tsrc4_neg );

        // math.e0.f0.0 (8) r8.acc2 r6.noacc r4.noacc 0xe {Align16, Q1/Q2}
        t8DstOpnd0->setAccRegSel( ACC2 );
        t6SrcOpnd0->setAccRegSel( NOACC );
        t4SrcOpnd0->setAccRegSel( NOACC );
        inst = createMathInst(NULL, false, exsize, t8DstOpnd0, t6SrcOpnd0,
            t4SrcOpnd0, MATH_INVM, madmInstOpt, line_no);
        G4_CondMod *condModOverflow = createCondMod(Mod_o, tmpFlag->getRegVar(), 0);
        inst->setCondMod(condModOverflow);

        // (-f0.1) if (8) k0__AUTO_GENERATED_IF_LABEL__0 k0__AUTO_GENERATED_ELSE_LABEL__1 {Q1/Q2}
        G4_Predicate *predicateFlagReg = createPredicate(PredState_Minus, tmpFlag->getRegVar(), 0, predCtrlValue);
        inst = createInst(predicateFlagReg, G4_if, NULL, false, 8, NULL, NULL, NULL, NULL, instOpt, line_no);

        // madm (8) r9.acc3 r2.noacc r6.noacc r8.acc2 {Align16, Q1/Q2}
        G4_SrcRegRegion *t2SrcOpnd = createSrcRegRegion(tsrc2);
        t9DstOpnd0->setAccRegSel( ACC3 );
        t2SrcOpnd->setAccRegSel( NOACC );
        t6SrcOpnd1->setAccRegSel( NOACC );
        t8SrcOpnd0x0->setAccRegSel( ACC2 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t9DstOpnd0, t2SrcOpnd,
            t6SrcOpnd1, t8SrcOpnd0x0, madmInstOpt, line_no);

        // madm (8) r10.acc4 r5.noacc -r4.noacc r8.acc2 {Align16, Q1/Q2}
        G4_SrcRegRegion *t5SrcOpnd0 = createSrcRegRegion(tsrc5);
        t10DstOpnd0->setAccRegSel( ACC4 );
        t5SrcOpnd0->setAccRegSel( NOACC );
        t4SrcOpndNeg0->setAccRegSel( NOACC );
        t8SrcOpnd0x1->setAccRegSel( ACC2 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t10DstOpnd0, t5SrcOpnd0,
            t4SrcOpndNeg0, t8SrcOpnd0x1, madmInstOpt, line_no);

        // madm (8) r1.acc5 r8.acc2 r10.acc4 r8.acc2 {Align16, Q1/Q2}
        t1DstOpnd0->setAccRegSel( ACC5 );
        t8SrcOpnd0x2->setAccRegSel( ACC2 );
        t10SrcOpnd0->setAccRegSel( ACC4 );
        t8SrcOpnd0x3->setAccRegSel( ACC2 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t1DstOpnd0, t8SrcOpnd0x2,
            t10SrcOpnd0, t8SrcOpnd0x3, madmInstOpt, line_no);
        

        // madm (8) r11.acc6 r6.noacc -r4.noacc r9.acc3 {Align16, Q1/Q2}
        t11DstOpnd0->setAccRegSel( ACC6 );
        t6SrcOpnd2->setAccRegSel( NOACC );
        t4SrcOpndNeg1->setAccRegSel( NOACC );
        t9SrcOpnd0x0->setAccRegSel( ACC3 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t11DstOpnd0, t6SrcOpnd2,
            t4SrcOpndNeg1, t9SrcOpnd0x0, madmInstOpt, line_no);
        

        // madm (8) r9.acc7 r9.acc3 r11.acc6 r1.acc5 {Align16, Q1/Q2}
        t9DstOpnd1->setAccRegSel( ACC7 );
        t9SrcOpnd0x1->setAccRegSel( ACC3 );
        t11SrcOpnd0->setAccRegSel( ACC6 );
        t1SrcOpnd0->setAccRegSel( ACC5 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t9DstOpnd1, t9SrcOpnd0x1,
            t11SrcOpnd0, t1SrcOpnd0, madmInstOpt, line_no);
        

        // madm (8) r6.acc8 r6.noacc -r4.noacc r9.acc7 {Align16, Q1/Q2}
        t6DstOpnd0->setAccRegSel( ACC8 );
        t6SrcOpnd3->setAccRegSel( NOACC );
        t4SrcOpndNeg2->setAccRegSel( NOACC );
        t9SrcOpnd1x0->setAccRegSel( ACC7 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t6DstOpnd0, t6SrcOpnd3,
            t4SrcOpndNeg2, t9SrcOpnd1x0, madmInstOpt, line_no);
        

        // restore cr0.0: mov (1) cr0.0<1>:ud r116.1<0;1,0>:ud {NoMask}
        inst = createInst(NULL, G4_mov, NULL, false, 1, cr0DstRegOpndForRestoreIfInst, tmpSrcOpndForCR0OnIf,
            NULL, InstOpt_WriteEnable, line_no);
        

        // madm (8) r8.noacc r9.acc7 r6.acc8 r1.acc5 {Align16, Q1/Q2}
        t8DstOpnd1->setAccRegSel( NOACC );
        t9SrcOpnd1x1->setAccRegSel( ACC7 );
        t6SrcOpnd4->setAccRegSel( ACC8 );
        t1SrcOpnd1->setAccRegSel( ACC5 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t8DstOpnd1, t9SrcOpnd1x1,
            t6SrcOpnd4, t1SrcOpnd1, madmInstOpt, line_no);
        

        // else (8) k0__AUTO_GENERATED_ELSE_LABEL__1 k0__AUTO_GENERATED_ELSE_LABEL__1 {Q1/Q2}
        inst = createInst(NULL, G4_else, NULL, false, 8, NULL, NULL, NULL, NULL, instOpt, line_no);
        

        // restore cr0.0: mov (1) cr0.0<1>:ud r116.2<0;1,0>:ud {NoMask}
        inst = createInst(NULL, G4_mov, NULL, false, 1, cr0DstRegOpndForRestoreElseInst, tmpSrcOpndForCR0OnElse,
            NULL, InstOpt_WriteEnable, line_no);
        

        // endif (8) {Q1/Q2}
        inst = createInst(NULL, G4_endif, NULL, false, 8, NULL, NULL, NULL, NULL, instOpt, line_no);
        

    };

    // restore CR0 to the value prior to setting denorm bit.
    inst = createInst(NULL, G4_mov, NULL, false, 1, cr0DenormDstRegOpndForRestore, tmpDenormSrcRegOpndForCR0,
        NULL, InstOpt_WriteEnable, line_no);
    

    // make final copy to dst
    // dst = r8:f  mov (instExecSize) r20.0<1>:f r110.0<8;8,1>:f {Q1/H1}
    t8_src_opnd_final->setAccRegSel(ACC_UNDEFINED);
    inst = createInst(predOpnd, G4_mov, condMod, saturate, instExecSize, dstOpnd, t8_src_opnd_final,
        NULL, Get_Gen4_Emask(emask, instExecSize), line_no);

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISAArithmeticSingleSQRTIEEEInst(ISA_Opcode opcode, Common_ISA_Exec_Size executionSize,
                                                  Common_VISA_EMask_Ctrl emask, G4_Predicate *predOpnd,
                                                  bool saturate, G4_CondMod* condMod, G4_DstRegRegion *dstOpnd, G4_Operand *src0Opnd)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    G4_INST* inst;
    unsigned int instOpt = 0;
    unsigned madmInstOpt = 0;
    uint8_t instExecSize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    uint8_t element_size = 8; // element_size is dynamic, changed according to instExecSize
    const uint8_t exsize = 8; // // exsize is a constant and never changed
    unsigned int loopCount = 1;
    instOpt |= Get_Gen4_Emask(emask, 8); // for those insts of execution size of element_size
    RegionDesc *srcRegionDesc = getRegionStride1();

    int line_no = last_inst ? last_inst->getLineNo() : 0;
    G4_Imm *flt_constant_0 = createImm(float(0.0));
    G4_Imm *flt_constant_05 = createImm(float(0.5));
    G4_Align reg_align = Either;
    if ( instExecSize == 1 || instExecSize == 8 )
    {
        element_size = 8;
        loopCount = 1;
    }
    else if ( instExecSize == 16 )
    {
        element_size = 16;
        instOpt = Get_Gen4_Emask(emask, 16);
        loopCount = 2;
    }

    // pred and conModifier
    G4_Declare *tmpFlag = createTempFlag(2);
    G4_Predicate_Control predCtrlValue = PRED_DEFAULT;

    // temp registers
    G4_Declare *t6  = createTempVarWithNoSpill( element_size, Type_F, reg_align, Any );
    G4_Declare *t7  = createTempVarWithNoSpill( element_size, Type_F, reg_align, Any );
    G4_Declare *t9  = createTempVarWithNoSpill( element_size, Type_F, reg_align, Any );
    G4_Declare *t10 = createTempVarWithNoSpill( element_size, Type_F, reg_align, Any );
    G4_Declare *t11 = createTempVarWithNoSpill( element_size, Type_F, reg_align, Any );

    // cr0.0 register
    G4_Declare *regCR0  = createTempVarWithNoSpill( 1, Type_UD, reg_align, Any );
    // Temporary Var for saving/restoring CR0 before/after setting denorm bits
    G4_Declare *regCR0Denorm = createTempVarWithNoSpill(1, Type_UD, reg_align, Any);

    // 0.0:f and 0.5:f constants
    G4_Declare *t0  = createTempVarWithNoSpill( 8, Type_F, reg_align, Any );
    G4_Declare *t8  = createTempVarWithNoSpill( 8, Type_F, reg_align, Any );

	inst = createPseudoKills ({ t0, t6, t7, t8, t9, t10, t11 });
	
    G4_SrcRegRegion* src0RR = operandToDirectSrcRegRegion(*this, src0Opnd, element_size);


    if (src0RR->isScalar() || src0RR->getModifier() != Mod_src_undef)
    {
        // expand src0 to vector src
        G4_DstRegRegion *t6_dst_src0_opnd = Create_Dst_Opnd_From_Dcl(t6, 1);
        inst = createInst(NULL, G4_mov, NULL, false, element_size, t6_dst_src0_opnd, src0RR,
	        NULL, instOpt, line_no); // mov (element_size) t6, src0RR {Q1/H1}
        
        src0RR = Create_Src_Opnd_From_Dcl(t6, getRegionStride1());
    }

    // cr0.0 register
    G4_SrcRegRegion tmpDenormSrcRegForCR0(
        Mod_src_undef, Direct, regCR0Denorm->getRegVar(), 0, 0, getRegionScalar(), Type_UD);
    G4_DstRegRegion tmpDenormDstRegForCR0(Direct, regCR0Denorm->getRegVar(), 0, 0, 1, Type_UD);
    G4_DstRegRegion regDstCR0(Direct, phyregpool.getCr0Reg(), 0, 0, 1, Type_UD );
    G4_DstRegRegion tmpDstRegForCR0(Direct, regCR0->getRegVar(), 0, 0, 1, Type_UD );
    G4_SrcRegRegion regSrcCR0(Mod_src_undef, Direct, phyregpool.getCr0Reg(), 0, 0, getRegionScalar(), Type_UD );
    G4_SrcRegRegion tmpSrcRegForCR0(Mod_src_undef, Direct, regCR0->getRegVar(), 0, 0, getRegionScalar(), Type_UD );

    G4_DstRegRegion tdst0(Direct, t0->getRegVar(), 0, 0, 1, Type_F );
    G4_DstRegRegion tdst8(Direct, t8->getRegVar(), 0, 0, 1, Type_F );
    G4_SrcRegRegion tsrc0(Mod_src_undef, Direct, t0->getRegVar(), 0, 0, srcRegionDesc, Type_F );
    G4_SrcRegRegion tsrc8(Mod_src_undef, Direct, t8->getRegVar(), 0, 0, srcRegionDesc, Type_F );

    G4_DstRegRegion *t0DstOpnd = createDstRegRegion(tdst0);
    G4_DstRegRegion *t8DstOpnd = createDstRegRegion(tdst8);

    // r0 = 0.0:f, r8 = 0.5:f
    // NOTE: 'NoMask' is required as constants are required for splitting
    // parts. Once they are in diverged branches, it won't be properly
    // initialized without 'NoMask'.
    inst = createInst(NULL, G4_mov, NULL, false, exsize, t0DstOpnd, flt_constant_0,
        NULL, InstOpt_WriteEnable, line_no); //mov (8) r0.0<1>:f 0:f {NoMask}
    
    createInst(NULL, G4_mov, NULL, false, exsize, t8DstOpnd, flt_constant_05,
        NULL, InstOpt_WriteEnable, line_no); // mov (8) r8.0<1>:f 0x3f000000:f {NoMask}
    

    G4_SrcRegRegion tsrc7_final(Mod_src_undef, Direct, t7->getRegVar(), 0, 0,
        getRegionStride1(), t7->getElemType() );
    G4_SrcRegRegion *t7_src_opnd_final = createSrcRegRegion(tsrc7_final);

    G4_DstRegRegion *tmpDenormDstRegOpndForCR0 = createDstRegRegion(tmpDenormDstRegForCR0);
    G4_SrcRegRegion *cr0DenormSrcRegOpndForSaveInst = createSrcRegRegion(regSrcCR0);
    G4_DstRegRegion *cr0DstRegOpndForOrInst = createDstRegRegion(regDstCR0);
    G4_SrcRegRegion *cr0SrcRegOpndForOrInst = createSrcRegRegion(regSrcCR0);
    G4_DstRegRegion *cr0DenormDstRegOpndForRestore = createDstRegRegion(regDstCR0);
    G4_SrcRegRegion *tmpDenormSrcRegOpndForCR0 = createSrcRegRegion(tmpDenormSrcRegForCR0);

    // set IEEE float precision denorm mode to 1.

    // save cr0.0 and make sure to set denorm bits
    inst = createInst(NULL, G4_mov, NULL, false, 1, tmpDenormDstRegOpndForCR0, cr0DenormSrcRegOpndForSaveInst,
        NULL, InstOpt_WriteEnable, line_no);

    inst = createInst(NULL, G4_or, NULL, false, 1, cr0DstRegOpndForOrInst, cr0SrcRegOpndForOrInst,
        createImm(0x80, Type_UD), InstOpt_WriteEnable, line_no); // {NoMask}
    

    Common_VISA_EMask_Ctrl currEMask = emask;
    for (uint16_t regIndex = 0; currEMask != vISA_NUM_EMASK && regIndex < loopCount;
            ++regIndex, currEMask = Get_Next_EMask(currEMask, exsize))
    {
        instOpt = Get_Gen4_Emask(currEMask, exsize);
        instOpt |= IsNoMask(emask) ? InstOpt_WriteEnable : 0;
        madmInstOpt = instOpt;

        //7, 9, 10, 11
        G4_DstRegRegion tdst7(Direct, t7->getRegVar(), regIndex, 0, 1, Type_F );
        G4_DstRegRegion tdst9(Direct, t9->getRegVar(), regIndex, 0, 1, Type_F );
        G4_DstRegRegion tdst10(Direct, t10->getRegVar(), regIndex, 0, 1, Type_F );
        G4_DstRegRegion tdst11(Direct, t11->getRegVar(), regIndex, 0, 1, Type_F );

        G4_SrcRegRegion tsrc7(Mod_src_undef, Direct, t7->getRegVar(), regIndex, 0, srcRegionDesc, Type_F );
        G4_SrcRegRegion tsrc9(Mod_src_undef, Direct, t9->getRegVar(), regIndex, 0, srcRegionDesc, Type_F );
        G4_SrcRegRegion tsrc10(Mod_src_undef, Direct, t10->getRegVar(), regIndex, 0, srcRegionDesc, Type_F );
        G4_SrcRegRegion tsrc11(Mod_src_undef, Direct, t11->getRegVar(), regIndex, 0, srcRegionDesc, Type_F );

        G4_DstRegRegion *t7DstOpnd0 = createDstRegRegion(tdst7);
        G4_DstRegRegion *t7DstOpnd1 = createDstRegRegion(tdst7);
        G4_DstRegRegion *t7DstOpnd2 = createDstRegRegion(tdst7);
        G4_DstRegRegion *t9DstOpnd0 = createDstRegRegion(tdst9);
        G4_DstRegRegion *t9DstOpnd1 = createDstRegRegion(tdst9);
        G4_DstRegRegion *t10DstOpnd0 = createDstRegRegion(tdst10);
        G4_DstRegRegion *t10DstOpnd1 = createDstRegRegion(tdst10);
        G4_DstRegRegion *t11DstOpnd0 = createDstRegRegion(tdst11);

        // src oprands passed by function calls
        G4_SrcRegRegion fsrc0(Mod_src_undef, Direct, src0RR->getBase(), src0RR->asSrcRegRegion()->getRegOff() + regIndex, 0, srcRegionDesc, Type_F );

        G4_SrcRegRegion *t6SrcOpnd0 = NULL;
        G4_SrcRegRegion *t6SrcOpnd1 = NULL;
        G4_SrcRegRegion *t6SrcOpnd2 = NULL;

        G4_SrcRegRegion *t7SrcOpnd0 = createSrcRegRegion(tsrc7);
        G4_SrcRegRegion *t7SrcOpnd1 = createSrcRegRegion(tsrc7);
        G4_SrcRegRegion *t7SrcOpnd2x0 = createSrcRegRegion(tsrc7);
        G4_SrcRegRegion *t7SrcOpnd2x1 = createSrcRegRegion(tsrc7);
        G4_SrcRegRegion *t7SrcOpnd3 = createSrcRegRegion(tsrc7);

        G4_SrcRegRegion *t9SrcOpnd0 = createSrcRegRegion(tsrc9);
        G4_SrcRegRegion *t9SrcOpnd1x0 = createSrcRegRegion(tsrc9);
        G4_SrcRegRegion *t9SrcOpnd1x1 = createSrcRegRegion(tsrc9);
        G4_SrcRegRegion *t9SrcOpnd2 = createSrcRegRegion(tsrc9);

        G4_SrcRegRegion *t10SrcOpnd0 = createSrcRegRegion(tsrc10);
        G4_SrcRegRegion *t10SrcOpnd1 = createSrcRegRegion(tsrc10);
        G4_SrcRegRegion *t10SrcOpnd2 = createSrcRegRegion(tsrc10);

        G4_SrcRegRegion *t11SrcOpnd0 = createSrcRegRegion(tsrc11);
        G4_SrcRegRegion *t11SrcOpnd1x0 = createSrcRegRegion(tsrc11);
        G4_SrcRegRegion *t11SrcOpnd1x1 = createSrcRegRegion(tsrc11);

        G4_DstRegRegion *cr0DstRegOpndForAndInst = createDstRegRegion(regDstCR0);
        G4_DstRegRegion *cr0DstRegOpndForRestoreIfInst = createDstRegRegion(regDstCR0);
        G4_DstRegRegion *cr0DstRegOpndForRestoreElseInst = createDstRegRegion(regDstCR0);
        G4_SrcRegRegion *cr0SrcRegOpndForSaveInst = createSrcRegRegion(regSrcCR0);
        G4_SrcRegRegion *cr0SrcRegOpndForAndInst = createSrcRegRegion(regSrcCR0);

        G4_DstRegRegion *tmpDstRegOpndForCR0 = createDstRegRegion(tmpDstRegForCR0);
        G4_SrcRegRegion *tmpSrcOpndForCR0OnIf = createSrcRegRegion(tmpSrcRegForCR0);
        G4_SrcRegRegion *tmpSrcOpndForCR0OnElse = createSrcRegRegion(tmpSrcRegForCR0);

        // save cr0.0: mov (1) r108.0<1>:ud cr0.0<0;1,0>:ud {NoMask}
        inst = createInst(NULL, G4_mov, NULL, false, 1, tmpDstRegOpndForCR0, cr0SrcRegOpndForSaveInst,
            NULL, InstOpt_WriteEnable, line_no);
        

        // set rounding mod in CR0 to RNE: and (1) cr0.0<1>:ud cr0.0<0;1,0>:ud 0xffffffcf:ud {NoMask}
        inst = createInst(NULL, G4_and, NULL, false, 1, cr0DstRegOpndForAndInst, cr0SrcRegOpndForAndInst,
            createImm(0xffffffcf, Type_UD), InstOpt_WriteEnable, line_no);
        

        t6SrcOpnd0 = createSrcRegRegion(fsrc0);
        t6SrcOpnd1 = createSrcRegRegion(fsrc0);
        t6SrcOpnd2 = createSrcRegRegion(fsrc0);

        //math.eo.f0.0 (8) r7.acc2 r6.noacc null 0xF {Aligned16, Q1/Q2}
        t7DstOpnd0->setAccRegSel( ACC2 );
        t6SrcOpnd0->setAccRegSel( NOACC );

        G4_SrcRegRegion *null_src_opnd = createNullSrc(Type_F);

        inst = createMathInst(NULL, false, exsize, t7DstOpnd0, t6SrcOpnd0,
            null_src_opnd, MATH_RSQRTM, madmInstOpt, line_no);
        G4_CondMod *condModOverflow = createCondMod(Mod_o, tmpFlag->getRegVar(), 0);
        inst->setCondMod(condModOverflow);

        // (-f1.0) if (8) k0__AUTO_GENERATED_IF_LABEL__0 k0__AUTO_GENERATED_IF_LABEL__0 {Q1/Q2}
        G4_Predicate *predicateFlagReg = createPredicate(PredState_Minus, tmpFlag->getRegVar(), 0, predCtrlValue);
        inst = createInst(predicateFlagReg, G4_if, NULL, false, 8, NULL, NULL, NULL, NULL, instOpt, line_no);
        

        //madm (8) r9.acc3 r0.noacc r8.noacc r7.acc2 {Aligned16, Q1/Q2}
        G4_SrcRegRegion *t0SrcOpnd0 = createSrcRegRegion(tsrc0);
        G4_SrcRegRegion *t8SrcOpnd0 = createSrcRegRegion(tsrc8);
        t9DstOpnd0->setAccRegSel( ACC3 );
        t0SrcOpnd0->setAccRegSel( NOACC );
        t8SrcOpnd0->setAccRegSel( NOACC );
        t7SrcOpnd0->setAccRegSel( ACC2 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t9DstOpnd0, t0SrcOpnd0,
            t8SrcOpnd0, t7SrcOpnd0, madmInstOpt, line_no);
        

        //madm (8) r11.acc4 r0.noacc r6.noacc r7.acc2 {Aligned16, Q1/Q2}
        G4_SrcRegRegion *t0SrcOpnd1 = createSrcRegRegion(tsrc0);
        t11DstOpnd0->setAccRegSel( ACC4 );
        t0SrcOpnd1->setAccRegSel( NOACC );
        t6SrcOpnd1->setAccRegSel( NOACC );
        t7SrcOpnd1->setAccRegSel( ACC2 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t11DstOpnd0, t0SrcOpnd1,
            t6SrcOpnd1, t7SrcOpnd1, madmInstOpt, line_no);
        

        // create -r11.noacc
        G4_SrcRegRegion tsrc11_neg( Mod_Minus,
            t11SrcOpnd0->asSrcRegRegion()->getRegAccess(),
            t11SrcOpnd0->asSrcRegRegion()->getBase(),
            t11SrcOpnd0->asSrcRegRegion()->getRegOff(),
            t11SrcOpnd0->asSrcRegRegion()->getSubRegOff(),
            t11SrcOpnd0->asSrcRegRegion()->getRegion(),
            t11SrcOpnd0->getType() );
        G4_SrcRegRegion *t11SrcOpndNeg0 = createSrcRegRegion( tsrc11_neg );
        //madm (8) r10.acc5 r8.noacc -r11.acc4 r9.acc3 {Aligned16, Q1/Q2}
        G4_SrcRegRegion *t8SrcOpnd1 = createSrcRegRegion(tsrc8);
        t10DstOpnd0->setAccRegSel( ACC5 );
        t8SrcOpnd1->setAccRegSel( NOACC );
        t11SrcOpndNeg0->setAccRegSel( ACC4 );
        t9SrcOpnd0->setAccRegSel( ACC3 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t10DstOpnd0, t8SrcOpnd1,
            t11SrcOpndNeg0, t9SrcOpnd0, madmInstOpt, line_no);
        

        //madm (8) r9.acc6 r9.acc3 r10.acc5 r9.acc3 {Aligned16, Q1/Q2}
        t9DstOpnd1->setAccRegSel( ACC6 );
        t9SrcOpnd1x0->setAccRegSel( ACC3 );
        t10SrcOpnd0->setAccRegSel( ACC5 );
        t9SrcOpnd1x1->setAccRegSel( ACC3 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t9DstOpnd1, t9SrcOpnd1x0,
            t10SrcOpnd0, t9SrcOpnd1x1, madmInstOpt, line_no);
        

        //madm (8) r7.acc7 r11.acc4 r10.acc5 r11.acc4 {Aligned16, Q1/Q2}
        t7DstOpnd1->setAccRegSel( ACC7 );
        t11SrcOpnd1x0->setAccRegSel( ACC4 );
        t10SrcOpnd1->setAccRegSel( ACC5 );
        t11SrcOpnd1x1->setAccRegSel( ACC4 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t7DstOpnd1, t11SrcOpnd1x0,
            t10SrcOpnd1, t11SrcOpnd1x1, madmInstOpt, line_no);
        

        // create -r7.noacc
        G4_SrcRegRegion tsrc7_neg( Mod_Minus,
            t7SrcOpnd2x0->asSrcRegRegion()->getRegAccess(),
            t7SrcOpnd2x0->asSrcRegRegion()->getBase(),
            t7SrcOpnd2x0->asSrcRegRegion()->getRegOff(),
            t7SrcOpnd2x0->asSrcRegRegion()->getSubRegOff(),
            t7SrcOpnd2x0->asSrcRegRegion()->getRegion(),
            t7SrcOpnd2x0->getType() );
        G4_SrcRegRegion *t7SrcOpndNeg0 = createSrcRegRegion( tsrc7_neg );
        //madm (8) r10.acc8 r6.noacc -r7.acc7 r7.acc7 {Aligned16, Q1/Q2}
        t10DstOpnd1->setAccRegSel( ACC8 );
        t6SrcOpnd2->setAccRegSel( NOACC );
        t7SrcOpndNeg0->setAccRegSel( ACC7 );
        t7SrcOpnd2x1->setAccRegSel( ACC7 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t10DstOpnd1, t6SrcOpnd2,
            t7SrcOpndNeg0, t7SrcOpnd2x1, madmInstOpt, line_no);
        

        // restore cr0.0: mov (1) cr0.0<1>:ud r106.0<0;1,0>:ud {NoMask}
        inst = createInst(NULL, G4_mov, NULL, false, 1, cr0DstRegOpndForRestoreIfInst, tmpSrcOpndForCR0OnIf,
            NULL, InstOpt_WriteEnable, line_no);
        

        //madm (8) r7.noacc r7.acc7 r9.acc6 r10.acc8 {Aligned16, Q1/Q2}
        t7DstOpnd2->setAccRegSel( NOACC );
        t7SrcOpnd3->setAccRegSel( ACC7 );
        t9SrcOpnd2->setAccRegSel( ACC6 );
        t10SrcOpnd2->setAccRegSel( ACC8 );
        inst = createInst(NULL, G4_madm, NULL, false, exsize, t7DstOpnd2, t7SrcOpnd3,
            t9SrcOpnd2, t10SrcOpnd2, madmInstOpt, line_no);
        

        // else
        inst = createInst(NULL, G4_else, NULL, false, 8, NULL, NULL, NULL, NULL, instOpt, line_no);
        

        // restore cr0.0: mov (1) cr0.0<1>:ud r108.0<0;1,0>:ud {NoMask}
        inst = createInst(NULL, G4_mov, NULL, false, 1, cr0DstRegOpndForRestoreElseInst, tmpSrcOpndForCR0OnElse,
            NULL, InstOpt_WriteEnable, line_no);
        

        // endif (8) {Q1/Q2}
        inst = createInst(NULL, G4_endif, NULL, false, 8, NULL, NULL, NULL, NULL, instOpt, line_no);
        

    };

    // restore CR0 to the value prior to setting denorm bit.
    inst = createInst(NULL, G4_mov, NULL, false, 1, cr0DenormDstRegOpndForRestore, tmpDenormSrcRegOpndForCR0,
        NULL, InstOpt_WriteEnable, line_no);
    

    // make final copy to dst
    // dst = r8:df   mov (instExecSize) r86.0<1>:f r8.0<8;8,1>:f {Q1/H1}
    t7_src_opnd_final->setAccRegSel(ACC_UNDEFINED);
    inst = createInst(predOpnd, G4_mov, condMod, saturate, instExecSize, dstOpnd, t7_src_opnd_final,
        NULL, Get_Gen4_Emask(emask, instExecSize), line_no);

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISAArithmeticDoubleSQRTInst(ISA_Opcode opcode, Common_ISA_Exec_Size executionSize,
    Common_VISA_EMask_Ctrl emask, G4_Predicate *predOpnd,
    bool saturate, G4_CondMod* condMod, G4_DstRegRegion *dstOpnd, G4_Operand *src0Opnd)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    G4_INST* inst;
    uint8_t instExecSize = (uint8_t)Get_Common_ISA_Exec_Size(executionSize);
    const uint8_t exsize = 4; // exsize is a constant and never changed
    unsigned int instOpt = Get_Gen4_Emask(emask, 4); // for insts of execution size of element_size before the loop
    RegionDesc *srcRegionDesc = getRegionStride1();
    RegionDesc *rdAlign16 = getRegionStride1();
    unsigned int loopCount;
    uint8_t element_size;   // element_size is set according to instExecSize

    G4_DstRegRegion *dst0 = nullptr;
    G4_SrcRegRegion *src0 = nullptr;
    G4_SrcRegRegion *src1 = nullptr;
    G4_SrcRegRegion *src2 = nullptr;
    G4_SrcRegRegion *neg_src1 = nullptr;
    G4_Imm *immData = nullptr;

    int line_no = last_inst ? last_inst->getLineNo() : 0;
    G4_Align reg_align = Either;
    if (instExecSize == 1 || instExecSize == 4)
    {
        element_size = 4;
        loopCount = 1;
    }
    else
    {
        ASSERT_USER(instExecSize == 8, "simd2 and simd16 support will be added later");
        element_size = 8;
        loopCount = 2;
    }

    // pred and conModifier
    G4_Declare *flagReg = createTempFlag(2);
    G4_Predicate_Control predCtrlValue = PRED_DEFAULT;

    // temp registers
    G4_Declare *t0  = createTempVarWithNoSpill(4, Type_DF, reg_align, Any);
    G4_Declare *t1  = createTempVarWithNoSpill(4, Type_DF, reg_align, Any);
    G4_Declare *t2  = createTempVarWithNoSpill(4, Type_DF, reg_align, Any);
    G4_Declare *t6  = createTempVarWithNoSpill(element_size, Type_DF, reg_align, Any);
    G4_Declare *t7  = createTempVarWithNoSpill(element_size, Type_DF, reg_align, Any);
    G4_Declare *t8  = createTempVarWithNoSpill(element_size, Type_DF, reg_align, Any);
    G4_Declare *t9  = createTempVarWithNoSpill(element_size, Type_DF, reg_align, Any);
    G4_Declare *t10 = createTempVarWithNoSpill(element_size, Type_DF, reg_align, Any);
    G4_Declare *t11 = createTempVarWithNoSpill(element_size, Type_DF, reg_align, Any);

	inst = createPseudoKills({ t0, t1, t2, t6, t7, t8, t9, t10, t11 });

    G4_SrcRegRegion* src0RR = operandToDirectSrcRegRegion(*this, src0Opnd, element_size);

    bool IsSrc0Moved = src0RR->getRegion()->isScalar() || src0RR->getModifier() != Mod_src_undef;
    if (IsSrc0Moved)
    {
        // expand scale src0 to vector src
        G4_DstRegRegion tdst_src0(Direct, t6->getRegVar(), 0, 0, 1, Type_DF);
        dst0 = createDstRegRegion(tdst_src0);
        // mov (element_size) t6_dst_src0_opnd, src0RR {Q1/H1}
        inst = createInst(NULL, G4_mov, NULL, false, element_size, dst0, src0RR, NULL, instOpt, line_no);
        
    }

    // cr0.0 register
    G4_DstRegRegion regDstCR0(Direct, phyregpool.getCr0Reg(), 0, 0, 1, Type_UD);
    G4_SrcRegRegion regSrcCR0(Mod_src_undef, Direct, phyregpool.getCr0Reg(), 0, 0, getRegionScalar(), Type_UD);

    // temporary reg for saving/restoring cr0.0
    G4_Declare *tmpRegCR0 = createTempVarWithNoSpill(1, Type_UD, reg_align, Any);
    G4_DstRegRegion tmpDstRegCR0(Direct, tmpRegCR0->getRegVar(), 0, 0, 1, Type_UD);
    G4_SrcRegRegion tmpSrcRegCR0(Mod_src_undef, Direct, tmpRegCR0->getRegVar(), 0, 0, getRegionScalar(), Type_UD);

    // constants

    // r0 = 0.0:df, r1 = 1.0:df, r2(r8) = 0.5:df
    // NOTE: 'NoMask' is required as constants are required for splitting
    // parts. Once they are in diverged branches, it won't be properly
    // initialized without 'NoMask'.

    // one GRF
    G4_SrcRegRegion csrc0(Mod_src_undef, Direct, t0->getRegVar(), 0, 0, srcRegionDesc, Type_DF);
    G4_SrcRegRegion csrc1(Mod_src_undef, Direct, t1->getRegVar(), 0, 0, srcRegionDesc, Type_DF);
    G4_SrcRegRegion csrc2(Mod_src_undef, Direct, t2->getRegVar(), 0, 0, srcRegionDesc, Type_DF);
    G4_DstRegRegion cdst0(Direct, t0->getRegVar(), 0, 0, 1, Type_DF);
    G4_DstRegRegion cdst1(Direct, t1->getRegVar(), 0, 0, 1, Type_DF);
    G4_DstRegRegion cdst2(Direct, t2->getRegVar(), 0, 0, 1, Type_DF);

    immData = createDFImm(0.0);
    dst0 = createDstRegRegion(cdst0);
    //mov (4) r0.0<1>:df 0.0:df {NoMask}
    inst = createInst(NULL, G4_mov, NULL, false, exsize, dst0, immData, NULL, InstOpt_WriteEnable, line_no);
    

    immData = createDFImm(1.0);
    dst0 = createDstRegRegion(cdst1);
    //mov (4) r1.0<1>:df 1.0:df {NoMask}
    inst = createInst(NULL, G4_mov, NULL, false, exsize, dst0, immData, NULL, InstOpt_WriteEnable, line_no);
    

    immData = createDFImm(0.5);
    dst0 = createDstRegRegion(cdst2);
    //mov (4) r2.0<1>:df 0.5:df {NoMask}  --- r8.0<1>
    inst = createInst(NULL, G4_mov, NULL, false, exsize, dst0, immData, NULL, InstOpt_WriteEnable, line_no);
    

    // final result is at r7.noacc
    G4_SrcRegRegion tsrc7_final(Mod_src_undef, Direct, t7->getRegVar(), 0, 0, getRegionStride1(), 
        t7->getElemType());

    // each madm only handles 4 channel double data
    Common_VISA_EMask_Ctrl currEMask = emask;
    for (uint16_t regIndex = 0; currEMask != vISA_NUM_EMASK && regIndex < loopCount;
        ++regIndex, currEMask = Get_Next_EMask(currEMask, exsize))
    {
        instOpt = Get_Gen4_Emask(currEMask, exsize);
        instOpt |= IsNoMask(emask) ? InstOpt_WriteEnable : 0; // setting channels for non-mad insts
        unsigned int madmInstOpt = instOpt; // setting channels for mad insts

        // dst : 7, 8, 9, 10 11
        G4_DstRegRegion tdst7(Direct, t7->getRegVar(), regIndex, 0, 1, Type_DF);
        G4_DstRegRegion tdst8(Direct, t8->getRegVar(), regIndex, 0, 1, Type_DF);
        G4_DstRegRegion tdst9(Direct, t9->getRegVar(), regIndex, 0, 1, Type_DF);
        G4_DstRegRegion tdst10(Direct, t10->getRegVar(), regIndex, 0, 1, Type_DF);
        G4_DstRegRegion tdst11(Direct, t11->getRegVar(), regIndex, 0, 1, Type_DF);

        // source of inst.
        G4_SrcRegRegion fsrc0_math(Mod_src_undef, Direct, src0RR->getBase(), src0RR->asSrcRegRegion()->getRegOff() + regIndex, 0, rdAlign16, Type_DF);
        G4_SrcRegRegion tsrc6_math(Mod_src_undef, Direct, t6->getRegVar(), src0RR->asSrcRegRegion()->getRegOff() + regIndex, 0, rdAlign16, Type_DF);

        // src : 6, 7, 8, 9, 10, 11
        G4_SrcRegRegion fsrc0(Mod_src_undef, Direct, src0RR->getBase(), src0RR->asSrcRegRegion()->getRegOff() + regIndex, 0, srcRegionDesc, Type_DF);
        G4_SrcRegRegion tsrc6(Mod_src_undef, Direct, t6->getRegVar(), regIndex, 0, srcRegionDesc, Type_DF);
        G4_SrcRegRegion tsrc7(Mod_src_undef, Direct, t7->getRegVar(), regIndex, 0, srcRegionDesc, Type_DF);
        G4_SrcRegRegion tsrc8(Mod_src_undef, Direct, t8->getRegVar(), regIndex, 0, srcRegionDesc, Type_DF);
        G4_SrcRegRegion tsrc9(Mod_src_undef, Direct, t9->getRegVar(), regIndex, 0, srcRegionDesc, Type_DF);
        G4_SrcRegRegion tsrc10(Mod_src_undef, Direct, t10->getRegVar(), regIndex, 0, srcRegionDesc, Type_DF);
        G4_SrcRegRegion tsrc11(Mod_src_undef, Direct, t11->getRegVar(), regIndex, 0, srcRegionDesc, Type_DF);

        // save cr0.0
        dst0 = createDstRegRegion(tmpDstRegCR0);
        src0 = createSrcRegRegion(regSrcCR0);
        // mov (1) r108.0<1>:ud cr0.0<0;1,0>:ud {NoMask}
        inst = createInst(NULL, G4_mov, NULL, false, 1, dst0, src0, NULL, InstOpt_WriteEnable, line_no);
        

        // set rounding mod in CR0 to RNE
        dst0 = createDstRegRegion(regDstCR0);
        src0 = createSrcRegRegion(regSrcCR0);
        immData = createImm(0xffffffcf, Type_UD);
        // and (1) cr0.0<0;1,0>:ud cr0.0<0;1,0>:ud 0xffffffcf:ud {NoMask}
        inst = createInst(NULL, G4_and, NULL, false, 1, dst0, src0, immData, InstOpt_WriteEnable, line_no);
        

        // set double precision denorm mode to 1
        dst0 = createDstRegRegion(regDstCR0);
        src0 = createSrcRegRegion(regSrcCR0);
        immData = createImm(0x40, Type_UD);
        // or (1) cr0.0<0;1,0>:ud cr0.0<0;1,0>:ud 0x40:ud {NoMask}
        inst = createInst(NULL, G4_or, NULL, false, 1, dst0, src0, immData, InstOpt_WriteEnable, line_no);
        

        // math.e0.f0.0 (4) r7.acc2 r6.noacc NULL 0xf {Align16, N1/N2}
        dst0 = createDstRegRegion(tdst7); dst0->setAccRegSel(ACC2);
        if (IsSrc0Moved)
        {
            src0 = createSrcRegRegion(tsrc6_math);
        }
        else
        {
            src0 = createSrcRegRegion(fsrc0_math);
        }
        src0->setAccRegSel(NOACC);
        src1 = createNullSrc(Type_DF);
        G4_CondMod* condModOverflow = createCondMod(Mod_o, flagReg->getRegVar(), 0);
        inst = createMathInst(NULL, false, exsize, dst0, src0, src1, MATH_RSQRTM, madmInstOpt, line_no);
        inst->setCondMod(condModOverflow);

        // if
        G4_Predicate* predicateFlagReg = createPredicate(PredState_Minus, flagReg->getRegVar(), 0, predCtrlValue);
        inst = createInst(predicateFlagReg, G4_if, NULL, false, exsize, NULL, NULL, NULL, NULL, instOpt, line_no);
        

        // madm (4) r9.acc3 r0.noacc r2(r8).noacc r7.acc2 {Align16, N1/N2}
        dst0 = createDstRegRegion(tdst9); dst0->setAccRegSel(ACC3);
        src0 = createSrcRegRegion(csrc0); src0->setAccRegSel(NOACC);
        src1 = createSrcRegRegion(csrc2); src1->setAccRegSel(NOACC);
        src2 = createSrcRegRegion(tsrc7); src2->setAccRegSel(ACC2);
        inst = createInst(NULL, G4_madm, NULL, false, exsize, dst0, src0, src1, src2, madmInstOpt, line_no);
        

        // madm (4) r11.acc4 r0.noacc r6.noacc r7.acc2 {Align16, N1/N2}
        dst0 = createDstRegRegion(tdst11); dst0->setAccRegSel(ACC4);
        src0 = createSrcRegRegion(csrc0); src0->setAccRegSel(NOACC);
        if (IsSrc0Moved)
        {
            src1 = createSrcRegRegion(tsrc6);
        }
        else
        {
            src1 = createSrcRegRegion(fsrc0);
        }
        src1->setAccRegSel(NOACC);
        src2 = createSrcRegRegion(tsrc7); src2->setAccRegSel(ACC2);
        inst = createInst(NULL, G4_madm, NULL, false, exsize, dst0, src0, src1, src2, madmInstOpt, line_no);
        

        // madm (4) r10.acc5 r2(r8).noacc -r11.acc4 r9.acc3 {Align16, N1/N2}
        dst0 = createDstRegRegion(tdst10); dst0->setAccRegSel(ACC5);
        src0 = createSrcRegRegion(csrc2); src0->setAccRegSel(NOACC);
        src1 = createSrcRegRegion(tsrc11); src1->setAccRegSel(ACC4);
        src2 = createSrcRegRegion(tsrc9); src2->setAccRegSel(ACC3);
        G4_SrcRegRegion neg_srcRegion(Mod_Minus,
            src1->asSrcRegRegion()->getRegAccess(),
            src1->asSrcRegRegion()->getBase(),
            src1->asSrcRegRegion()->getRegOff(),
            src1->asSrcRegRegion()->getSubRegOff(),
            src1->asSrcRegRegion()->getRegion(),
            src1->getType());
        neg_src1 = createSrcRegRegion(neg_srcRegion);
        neg_src1->setAccRegSel(src1->getAccRegSel());
        inst = createInst(NULL, G4_madm, NULL, false, exsize, dst0, src0, neg_src1, src2, madmInstOpt, line_no);
        

        // madm (4) r8.acc6 r1.noacc r2(r8).noacc r1.noacc {Align16, N1/N2}
        dst0 = createDstRegRegion(tdst8); dst0->setAccRegSel(ACC6);
        src0 = createSrcRegRegion(csrc1); src0->setAccRegSel(NOACC);
        src1 = createSrcRegRegion(csrc2); src1->setAccRegSel(NOACC);
        src2 = createSrcRegRegion(csrc1); src2->setAccRegSel(NOACC);
        inst = createInst(NULL, G4_madm, NULL, false, exsize, dst0, src0, src1, src2, madmInstOpt, line_no);
        

        // madm (4) r8.acc7 r1.noacc r8.acc6 r10.acc5 {Align16, N1/N2}
        dst0 = createDstRegRegion(tdst8); dst0->setAccRegSel(ACC7);
        src0 = createSrcRegRegion(csrc1); src0->setAccRegSel(NOACC);
        src1 = createSrcRegRegion(tsrc8); src1->setAccRegSel(ACC6);
        src2 = createSrcRegRegion(tsrc10); src2->setAccRegSel(ACC5);
        inst = createInst(NULL, G4_madm, NULL, false, exsize, dst0, src0, src1, src2, madmInstOpt, line_no);
        

        // madm (4) r7.acc8 r0.noacc r10.acc5 r11.acc4 {Align16, N1/N2}
        dst0 = createDstRegRegion(tdst7); dst0->setAccRegSel(ACC8);
        src0 = createSrcRegRegion(csrc0); src0->setAccRegSel(NOACC);
        src1 = createSrcRegRegion(tsrc10); src1->setAccRegSel(ACC5);
        src2 = createSrcRegRegion(tsrc11); src2->setAccRegSel(ACC4);
        inst = createInst(NULL, G4_madm, NULL, false, exsize, dst0, src0, src1, src2, madmInstOpt, line_no);
        

        // madm (4) r10.acc9 r0.noacc r10.acc5 r9.acc3 {Align16, N1/N2}
        dst0 = createDstRegRegion(tdst10); dst0->setAccRegSel(ACC9);
        src0 = createSrcRegRegion(csrc0); src0->setAccRegSel(NOACC);
        src1 = createSrcRegRegion(tsrc10); src1->setAccRegSel(ACC5);
        src2 = createSrcRegRegion(tsrc9); src2->setAccRegSel(ACC3);
        inst = createInst(NULL, G4_madm, NULL, false, exsize, dst0, src0, src1, src2, madmInstOpt, line_no);
        

        // madm (4) r7.acc8 r11.acc4 r8.acc7 r7.acc8 {Align16, N1/N2}
        dst0 = createDstRegRegion(tdst7); dst0->setAccRegSel(ACC8);
        src0 = createSrcRegRegion(tsrc11); src0->setAccRegSel(ACC4);
        src1 = createSrcRegRegion(tsrc8); src1->setAccRegSel(ACC7);
        src2 = createSrcRegRegion(tsrc7); src2->setAccRegSel(ACC8);
        inst = createInst(NULL, G4_madm, NULL, false, exsize, dst0, src0, src1, src2, madmInstOpt, line_no);
        

        // madm (4) r8.acc7 r9.acc3 r8.acc7 r10.acc9 {Align16, N1/N2}
        dst0 = createDstRegRegion(tdst8); dst0->setAccRegSel(ACC7);
        src0 = createSrcRegRegion(tsrc9); src0->setAccRegSel(ACC3);
        src1 = createSrcRegRegion(tsrc8); src1->setAccRegSel(ACC7);
        src2 = createSrcRegRegion(tsrc10); src2->setAccRegSel(ACC9);
        inst = createInst(NULL, G4_madm, NULL, false, exsize, dst0, src0, src1, src2, madmInstOpt, line_no);
        

        // madm (4) r9.acc3 r6.noacc -r7.acc8 r7.acc8 {Align16, N1/N2}
        dst0 = createDstRegRegion(tdst9); dst0->setAccRegSel(ACC3);
        if (IsSrc0Moved)
        {
            src0 = createSrcRegRegion(tsrc6);
        }
        else
        {
            src0 = createSrcRegRegion(fsrc0);
        }
        src0->setAccRegSel(NOACC);
        src1 = createSrcRegRegion(tsrc7); src1->setAccRegSel(ACC8);
        src2 = createSrcRegRegion(tsrc7); src2->setAccRegSel(ACC8);
        G4_SrcRegRegion neg_srcRegion1(Mod_Minus,
            src1->asSrcRegRegion()->getRegAccess(),
            src1->asSrcRegRegion()->getBase(),
            src1->asSrcRegRegion()->getRegOff(),
            src1->asSrcRegRegion()->getSubRegOff(),
            src1->asSrcRegRegion()->getRegion(),
            src1->getType());
        neg_src1 = createSrcRegRegion(neg_srcRegion1);
        neg_src1->setAccRegSel(src1->getAccRegSel());
        inst = createInst(NULL, G4_madm, NULL, false, exsize, dst0, src0, neg_src1, src2, madmInstOpt, line_no);
        

        // restore cr0.0
        dst0 = createDstRegRegion(regDstCR0);
        src0 = createSrcRegRegion(tmpSrcRegCR0);
        // mov (1) cr0.0<0;1,0>:ud r108.0<1>:ud {NoMask}
        inst = createInst(NULL, G4_mov, NULL, false, 1, dst0, src0, NULL, InstOpt_WriteEnable, line_no);
        

        // madm (4) r7.noacc r7.acc8 r9.acc3 r8.acc7 {Align16, N1/N2}
        dst0 = createDstRegRegion(tdst7); dst0->setAccRegSel(NOACC);
        src0 = createSrcRegRegion(tsrc7); src0->setAccRegSel(ACC8);
        src1 = createSrcRegRegion(tsrc9); src1->setAccRegSel(ACC3);
        src2 = createSrcRegRegion(tsrc8); src2->setAccRegSel(ACC7);
        inst = createInst(NULL, G4_madm, NULL, false, exsize, dst0, src0, src1, src2, madmInstOpt, line_no);
        

        // else (8) {Q1/Q2}
        inst = createInst(NULL, G4_else, NULL, false, exsize, NULL, NULL, NULL, NULL, instOpt, line_no);
        

        // restore cr0.0 {NoMask}
        dst0 = createDstRegRegion(regDstCR0);
        src0 = createSrcRegRegion(tmpSrcRegCR0);
        // mov (1) cr0.0<0;1,0>:ud r108.0<1>:ud {NoMask}
        inst = createInst(NULL, G4_mov, NULL, false, 1, dst0, src0, NULL, InstOpt_WriteEnable, line_no);
        

        // endif (8) {Q1/Q2}
        inst = createInst(NULL, G4_endif, NULL, false, exsize, NULL, NULL, NULL, NULL, instOpt, line_no);
        
    };

    // make final copy to dst
    // src = r7:df
    src0 = createSrcRegRegion(tsrc7_final); src0->setAccRegSel(ACC_UNDEFINED);
    // mov (instExecSize) r20.0<1>:df r7.0<8;8,1>:df {Q1/H1}
    inst = createInst(predOpnd, G4_mov, condMod, saturate, instExecSize, dstOpnd, src0, NULL, Get_Gen4_Emask(emask, instExecSize), line_no);

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

// create a fence instruction to the data cache
// flushParam -- 
//              bit 0 -- commit enable
//              bit 1-4 -- L3 flush parameters
//              bit 5 -- global/SLM
//              bit 6 -- L1 flush
G4_INST* IR_Builder::createFenceInstruction( uint8_t flushParam, bool commitEnable, bool globalMemFence,
                                             bool isSendc = false )
{
#define L1_FLUSH_MASK 0x40

    int flushBits = (flushParam >> 1) & 0xF;
    bool L1Flush = (flushParam & L1_FLUSH_MASK) != 0;

    int desc = 0x7 << 14 | ((commitEnable ? 1 : 0) << 13 );

    desc |= flushBits << 9;

    if (L1Flush)
    {
#define L1_FLUSH_BIT_LOC 8
        desc |= 1 << L1_FLUSH_BIT_LOC;
    }

    G4_Declare *srcDcl = getBuiltinR0();
    G4_Declare *dstDcl = createTempVar( 8, Type_UD, Either, Any );
    G4_DstRegRegion *sendDstOpnd = commitEnable ? Create_Dst_Opnd_From_Dcl(dstDcl, 1) : createNullDst(Type_UD);
    G4_SrcRegRegion *sendSrcOpnd = Create_Src_Opnd_From_Dcl(srcDcl, getRegionStride1());
    uint8_t BTI = 0x0;

    if (hasSLMFence())
    {
        // we must choose either GLOBAL_MEM_FENCE or SLM_FENCE
        BTI = globalMemFence ? 0 : 0xfe;
    }

    // commitEnable = true: msg length = 1, response length = 1, dst == src
    // commitEnable = false: msg length = 1, response length = 0, dst == null
    return Create_Send_Inst_For_CISA( nullptr, sendDstOpnd, sendSrcOpnd, 1, (commitEnable ? 1 : 0), 8,
        desc, SFID_DP_DC, false, true, true, true, createImm(BTI, Type_UD), nullptr, InstOpt_NoOpt, isSendc);

}

int IR_Builder::translateVISAWaitInst(G4_Operand* mask)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    // clear TDR if mask is not null and not zero
    if (mask != NULL && !(mask->isImm() && mask->asImm()->getInt() == 0))
    {
        // mov (1) f0.0<1>:uw <TDR_bits>:ub {NoMask}
        G4_Declare* tmpFlagDcl = createTempFlag(1);
        G4_DstRegRegion* newPredDef = Create_Dst_Opnd_From_Dcl(tmpFlagDcl, 1);
        createInst( NULL, G4_mov, NULL, false, 1, newPredDef, mask, NULL, InstOpt_WriteEnable, 0);

        // (f0.0) and (8) tdr0.0<1>:uw tdr0.0<8;8,1>:uw 0x7FFF:uw {NoMask}
        G4_Predicate* predOpnd = createPredicate(PredState_Plus, tmpFlagDcl->getRegVar(), 0, PRED_DEFAULT);
        G4_DstRegRegion* TDROpnd = createDstRegRegion( Direct, phyregpool.getTDRReg(), 0, 0, 1, Type_UW);
        G4_SrcRegRegion* TDRSrc = createSrcRegRegion( Mod_src_undef, Direct, phyregpool.getTDRReg(), 0, 0, getRegionStride1(), Type_UW);
        createInst(predOpnd, G4_and, NULL, false, 8, TDROpnd, TDRSrc, createImm(0x7FFF, Type_UW), InstOpt_WriteEnable, 0);
    }

    createIntrinsicInst(nullptr, Intrinsic::Wait, 1, nullptr, nullptr, nullptr, nullptr, InstOpt_WriteEnable);

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISASyncInst(ISA_Opcode opcode, unsigned int mask)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    switch( opcode )
    {
    case ISA_BARRIER:
        {
            //BARRIER (no arguments)
            int exdesc = SFID_GATEWAY;
            // 1 message length, 0 response length, no header, no ack
            int desc = (0x1 << 25) + 0x4;

            //get barrier id
            G4_Declare *dcl = Create_MRF_Dcl( GENX_DATAPORT_IO_SZ, Type_UD );

            G4_SrcRegRegion* r0_src_opnd = createSrcRegRegion(
                Mod_src_undef,
                Direct,
                builtinR0->getRegVar(),
                0,
                2,
                getRegionScalar(),
                Type_UD );

            G4_DstRegRegion *mrf_dst1_opnd = Create_Dst_Opnd_From_Dcl(dcl, 1);

            bool enableBarrierInstCounterBits = kernel.getOption(VISA_EnableBarrierInstCounterBits);
            int mask = getBarrierMask(enableBarrierInstCounterBits);

            G4_Imm *g4Imm = createImm(mask, Type_UD);

            createInst(     NULL,
                G4_and,
                NULL,
                false,
                8,
                mrf_dst1_opnd,
                r0_src_opnd,
                g4Imm,
                InstOpt_WriteEnable,
                0 );

            // Generate the barrier send message
            G4_DstRegRegion *post_dst_opnd = createNullDst( Type_UD );
            G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
            createSendInst(
                NULL,
                G4_send,
                1,
                post_dst_opnd,
                payload,
                createImm( exdesc, Type_UD ),
                createImm( desc, Type_UD ),
                InstOpt_WriteEnable,
                true,
                true,
                NULL,
                0 );

            // Generate the wait message
            // wait n0.0<0;1,0>:ud
            {
                G4_SrcRegRegion* srcOpnd = createSrcRegRegion(Mod_src_undef, Direct,
                    phyregpool.getN0Reg(), 0, 0, getRegionScalar(), Type_UD);
                last_inst = createInst( NULL,
                    G4_wait,
                    NULL,
                    false,
                    1,
                    NULL,
                    srcOpnd,
                    NULL,
                    InstOpt_WriteEnable,
                    0);
            }
        }
        break;
    case ISA_SAMPLR_CACHE_FLUSH:
        {
            // msg length = 1, response length = 1, header_present = 1,
            // Bit 16-12 = 11111 for Sampler Message Type
            // Bit 18-17 = 11 for SIMD32 mode
            int desc = (1 << 25) + (1 << 20) + (1 << 19) + (0x3 << 17) + (0x1F << 12);

            G4_Declare *dcl = getBuiltinR0();
            G4_Declare *dstDcl = createTempVar( 8, Type_UD, Either, Any );
            G4_DstRegRegion* sendDstOpnd = Create_Dst_Opnd_From_Dcl( dstDcl, 1);
            G4_SrcRegRegion* sendMsgOpnd = Create_Src_Opnd_From_Dcl( dcl, getRegionStride1());

            last_inst = createSendInst( NULL, G4_send, 8, sendDstOpnd, sendMsgOpnd,
                createImm(SFID_SAMPLER, Type_UD), createImm(desc, Type_UD), 0, true, true, NULL, 0);

            G4_SrcRegRegion* moveSrcOpnd = createSrcRegRegion(Mod_src_undef, Direct, dstDcl->getRegVar(), 0, 0, getRegionStride1(), Type_UD);
            Create_MOV_Inst( dstDcl, 0, 0, 8, NULL, NULL, moveSrcOpnd);
        }
        break;
    case ISA_WAIT:
        {
            //This should be handled by translateVISAWait() now
            MUST_BE_TRUE(false, "Should not reach here");
        }
        break;
    case ISA_YIELD:
        {
            if (last_inst->opcode() != G4_label)
            {
                last_inst->setOptions(last_inst->getOption() | InstOpt_Switch);
            }
            else
            {
                G4_SrcRegRegion* srcOpnd = createSrcRegRegion(Mod_src_undef, Direct, getBuiltinR0()->getRegVar(), 0, 0, getRegionScalar(), Type_UD);
                G4_DstRegRegion* dstOpnd = createDstRegRegion(Direct, getBuiltinR0()->getRegVar(), 0, 0, 1, Type_UD);

                G4_INST* nop = createInst(NULL, G4_mov, NULL, false, 1, dstOpnd, srcOpnd, NULL, 0, 0);
                nop->setOptions(nop->getOption() | InstOpt_Switch);
            }
        }
        break;
    case ISA_FENCE:
        {
#define GLOBAL_MASK 0x20
            bool globalFence = (mask & GLOBAL_MASK) == 0;
            if (VISA_WA_CHECK(m_pWaTable, WADisableWriteCommitForPageFault))
            {
                // write commit does not work under page fault
                // so we generate a fence without commit, followed by a read surface info to BTI 0
                createFenceInstruction((uint8_t) mask & 0xFF, false, globalFence);
                G4_Imm* surface = createImm(0, Type_UD);
                G4_Declare* zeroLOD = createTempVar(8, Type_UD, Either, Any);
                Create_MOV_Inst(zeroLOD, 0, 0, 8, NULL, NULL, createImm(0, Type_UD));
                G4_SrcRegRegion* sendSrc = Create_Src_Opnd_From_Dcl(zeroLOD, getRegionStride1());
                G4_DstRegRegion* sendDst = Create_Dst_Opnd_From_Dcl(zeroLOD, 1);
                ChannelMask maskR = ChannelMask::createFromAPI(CHANNEL_MASK_R);
                translateVISAResInfoInst(EXEC_SIZE_8, vISA_EMASK_M1, maskR, surface, sendSrc, sendDst);
            }
            else
            {
                createFenceInstruction((uint8_t) mask & 0xFF, ( mask & 0x1 ) == 0x1, globalFence);
                // The move to ensure the fence is actually complete will be added at the end of compilation,
                // in Optimizer::HWWorkaround()
            }
            break;
        }
    default:
        RELEASE_MSG( "Unsupported ISA opcode");
        return CM_FAILURE;
    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISAPredBarrierInst(G4_Operand *mask, G4_DstRegRegion *dst) {
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    G4_Declare *msg = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ, Type_UD);
    G4_SrcRegRegion *R02 = createSrcRegRegion(Mod_src_undef, Direct,
                                              getBuiltinR0()->getRegVar(),
                                              0, 2, getRegionScalar(),
                                              Type_UD);
    G4_SrcRegRegion *msg2Src = createSrcRegRegion(Mod_src_undef, Direct,
                                                  msg->getRegVar(),0, 2,
                                                  getRegionScalar(),
                                                  Type_UD);
    G4_DstRegRegion *msg2Dst = createDstRegRegion(Direct, msg->getRegVar(),
                                                  0, 2, 1, Type_UD);
    G4_DstRegRegion *msg2Dst1 = createDstRegRegion(Direct, msg->getRegVar(),
                                                   0, 2, 1, Type_UD);
    G4_DstRegRegion *msg3Dst = createDstRegRegion(Direct, msg->getRegVar(),
                                                  0, 3, 1, Type_UD);

    // Prepare the payload.
    int barrierIDMask = getBarrierIDMask();
    int predMaskEnable = getPredMaskEnableBit();
    // Copy barrier ID from R0.
    createInst(NULL, G4_and, NULL, false, 1, msg2Dst,
               R02, createImm(barrierIDMask, Type_UD), InstOpt_WriteEnable);
    // Set 'Predicate Mask Enable' as well as the 'Predicate Mask'.
    createInst(NULL, G4_or, NULL, false, 1, msg2Dst1,
               msg2Src, createImm((int64_t)1 << predMaskEnable, Type_UD), InstOpt_WriteEnable);
    // Copy predicate mask.
    createInst(NULL, G4_mov, NULL, false, 1, msg3Dst, mask,
               NULL, InstOpt_WriteEnable);

    CISA_SHARED_FUNCTION_ID SFID = SFID_GATEWAY;
    unsigned MD = 0;
    MD |= MDC_GW_BARRIER_MSG;
    MD |= 1 << 14; // Ack is required.

    G4_SrcRegRegion *src = createSrcRegRegion(Mod_src_undef, Direct,
                                              msg->getRegVar(), 0, 0,
                                              getRegionStride1(),
                                              Type_UD);
    last_inst = Create_Send_Inst_For_CISA(NULL, dst,
                                          src, 1,
                                          1,
                                          8,
                                          MD, SFID,
                                          false, false,
                                          true, true,
                                          NULL, NULL,
                                          InstOpt_WriteEnable, false);

    // Wait for the notification.
    // wait n0.0<0;1,0>:ud
    G4_SrcRegRegion* N0
        = createSrcRegRegion(Mod_src_undef, Direct, phyregpool.getN0Reg(),
                             0, 0, getRegionScalar(), Type_UD);
    last_inst = createInst(NULL, G4_wait, NULL, false, 1, NULL, N0, NULL,
                           InstOpt_WriteEnable);
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

static bool needs32BitFlag(uint32_t opt)
{
    switch (opt & InstOpt_QuarterMasks)
    {
    case InstOpt_M16:
    case InstOpt_M20:
    case InstOpt_M24:
    case InstOpt_M28:
        return true;
    default:
        return false;
    }
}

int IR_Builder::translateVISACompareInst(ISA_Opcode opcode, Common_ISA_Exec_Size execsize, Common_VISA_EMask_Ctrl emask, Common_ISA_Cond_Mod relOp,
                                         G4_DstRegRegion *dstOpnd, G4_Operand *src0Opnd, G4_Operand *src1Opnd)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    G4_CondMod* condMod = NULL;
    uint8_t exsize = (uint8_t) Get_Common_ISA_Exec_Size(execsize);
    unsigned int inst_opt = Get_Gen4_Emask(emask, exsize);
    const char *varName = "";

#ifdef _DEBUG
    char buf[256];
    SNPRINTF(buf, 256, "PTemp%d", 0);
    varName = buf;
#endif

    uint8_t numWords = (exsize + 15)/16;
    if (needs32BitFlag(inst_opt))
    {
        // for H2, Q3, etc. we must use 32-bit flag regardless of execution size
        numWords = 2;
    }
    //TODO: Can eliminate the flag temp creation. Might need further changes
    G4_Declare *dcl = createDeclareNoLookup(
            createStringCopy(varName, mem),
            G4_FLAG,
            numWords,
            1,
            Type_UW );
    dcl->setNumberFlagElements(exsize);

    condMod = createCondMod(
        Get_G4_CondModifier_From_Common_ISA_CondModifier(relOp),
        dcl->getRegVar(),
        0);

    last_inst = createInst(
        NULL,
        Get_G4_Opcode_From_Common_ISA_Opcode((ISA_Opcode)opcode),
        condMod,
        false,
        exsize,
        dstOpnd,
        src0Opnd,
        src1Opnd,
        inst_opt,
        0);
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISACompareInst(ISA_Opcode opcode, Common_ISA_Exec_Size execsize, Common_VISA_EMask_Ctrl emask, Common_ISA_Cond_Mod relOp,
                                         G4_Predicate *dstOpnd, G4_Operand *src0Opnd, G4_Operand *src1Opnd)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    uint8_t exsize = (uint8_t) Get_Common_ISA_Exec_Size(execsize);
    unsigned int inst_opt = Get_Gen4_Emask(emask, exsize);
    /*
        If it's mix mode HF,F, it will be split down the road anyway, so behavior doesn't change.
    */
    G4_Type src0Type = src0Opnd->getType();
    G4_Type src1Type = src1Opnd->getType();
    G4_Type dstType = (exsize == 16 && !(src0Type == Type_HF || src1Type == Type_HF)) ? Type_W :
        (G4_Type_Table[src0Type].byteSize > G4_Type_Table[src1Type].byteSize) ? src0Type : src1Type;
    if (IS_VTYPE(dstType))
    {
        dstType = Type_UD;
    }
    G4_DstRegRegion *null_dst_opnd = createNullDst( dstType );

    G4_CondMod* condMod = createCondMod(
        Get_G4_CondModifier_From_Common_ISA_CondModifier(relOp),
        dstOpnd->asDstRegRegion()->getBase(), 0);

    last_inst = createInst(
        NULL,
        Get_G4_Opcode_From_Common_ISA_Opcode(opcode),
        condMod,
        false,
        exsize,
        null_dst_opnd,
        src0Opnd,
        src1Opnd,
        inst_opt,
        0);
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISACFSwitchInst(G4_Operand *indexOpnd, uint8_t numLabels, G4_Label ** labels )
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    // offsets are in bytes so we have to multiply everything by 16
    // FIXME: this assumes the jmpi instructions will never be compacted.
    if( indexOpnd->isImm() )
    {
        indexOpnd = createImm( indexOpnd->asImm()->getInt() * 16, Type_W );
    }
    else
    {
        G4_Declare *tmpVar = createTempVar(1, Type_D, Either, Any );
        G4_DstRegRegion* dstOpnd = Create_Dst_Opnd_From_Dcl( tmpVar, 1);
        createInst( NULL, G4_shl, NULL, false, 1, dstOpnd, indexOpnd,
            createImm(4, Type_UW), 0, 0 );
        indexOpnd = Create_Src_Opnd_From_Dcl( tmpVar, getRegionScalar() );
    }
    G4_INST* indirectJmp = NULL;
    // indirect jmp
    indirectJmp = createInst( NULL, G4_jmpi, NULL, false, 1, NULL, indexOpnd, NULL, 0, 0);

    for( int i = 0; i < numLabels; i++ )
    {
        indirectJmp->asCFInst()->addIndirectJmpLabel( labels[i] );
    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISACFLabelInst(G4_Label* lab)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    last_inst = createInst(NULL, G4_label, NULL, false, UNDEFINED_EXEC_SIZE, NULL, lab, NULL, 0, 0);

    if( lab->isFuncLabel() )
    {
        func_id++;
    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISACFCallInst(Common_ISA_Exec_Size execsize, Common_VISA_EMask_Ctrl emask, G4_Predicate *predOpnd, G4_Label* lab)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    G4_opcode callOpToUse = Get_G4_Opcode_From_Common_ISA_Opcode((ISA_Opcode)ISA_CALL);
    G4_DstRegRegion* dstOpndToUse = NULL;
    unsigned char execSize = (uint8_t) Get_Common_ISA_Exec_Size(execsize);
    G4_Label* srcLabel = lab;

    if(lab->isFCLabel() == true)
    {
        callOpToUse = G4_pseudo_fc_call;
        getFCPatchInfo()->setHasFCCalls(true);

        input_info_t *RetIP = getRetIPArg();
        G4_Declare *FCRet = createTempVar(2, Type_UD, Either, Four_Word);
        FCRet->setAliasDeclare(RetIP->dcl, 0);
        dstOpndToUse = Create_Dst_Opnd_From_Dcl(FCRet, 1);

        execSize = 2;
    }

    last_inst = createInst(
        predOpnd,
        callOpToUse,
        NULL,
        false,
        execSize,
        dstOpndToUse,
        srcLabel,
        NULL,
        0,
        0);

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISACFJumpInst(G4_Predicate *predOpnd, G4_Label* lab)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    last_inst = createInst(
        predOpnd,
        Get_G4_Opcode_From_Common_ISA_Opcode((ISA_Opcode)ISA_JMP),
        NULL,
        false,
        1,
        NULL,
        lab,
        NULL,
        0,
        0);
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISACFFCallInst(Common_ISA_Exec_Size execsize, Common_VISA_EMask_Ctrl emask, G4_Predicate *predOpnd, uint16_t functionID, uint8_t argSize, uint8_t returnSize)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    kernel.fg.setHasStackCalls();

    if( this->getArgSize() < argSize )
    {
        this->setArgSize( argSize );
    }

    if( this->getRetVarSize() < returnSize )
    {
        this->setRetVarSize( returnSize );
    }

    uint8_t exsize = (uint8_t)Get_Common_ISA_Exec_Size(execsize);

    last_inst = createInst(
        predOpnd,
        G4_pseudo_fcall,
        NULL,
        false,
        exsize,
        NULL,
        NULL,
        NULL,
        0,
        0);

    uint32_t calleeIndex = 0xFF;

    // When reading a CISA file, relocation table is present. But when
    // invoked via builder API, assume relocation is already done by
    // caller. So resolved index = symbolic index.
    if( getFuncRelocTable() != NULL )
    {
        // Resolve calleeIndex
        for(int i = 0; i < getFuncRelocTable()->num_syms; i++) {
            if(getFuncRelocTable()->reloc_syms[i].symbolic_index == functionID) {
                calleeIndex = getFuncRelocTable()->reloc_syms[i].resolved_index;
                break;
            }
        }
    }
    else
    {
        calleeIndex = functionID;
    }

    m_fcallInfo[last_inst] = new (mem) G4_FCALL(argSize, returnSize);

    // Push resolved index to list of callees
    callees.push_back(calleeIndex);
    last_inst->asCFInst()->setCalleeIndex(calleeIndex);
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISACFFretInst(Common_ISA_Exec_Size executionSize, Common_VISA_EMask_Ctrl emask, G4_Predicate *predOpnd)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    unsigned int instOpt = 0;
    uint8_t exsize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    instOpt |= Get_Gen4_Emask(emask, exsize);

    kernel.fg.setIsStackCallFunc();

    last_inst = createInst(
        predOpnd,
        G4_pseudo_fret,
        NULL,
        false,
        exsize,
        NULL,
        NULL, //src0Opnd
        NULL,
        instOpt,
        0);
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISACFRetInst(Common_ISA_Exec_Size executionSize, Common_VISA_EMask_Ctrl emask, G4_Predicate *predOpnd)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    unsigned int instOpt = InstOpt_NoOpt;
    uint8_t exsize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    instOpt |= Get_Gen4_Emask(emask, exsize);
    if(getFCPatchInfo()->getIsCallableKernel() == true)
    {
        if (tmpFCRet == nullptr) {
            input_info_t *RetIP = getRetIPArg();
            tmpFCRet = createTempVar(2, Type_UD, Either, Four_Word);
            tmpFCRet->setAliasDeclare(RetIP->dcl, 0);
        }
        G4_SrcRegRegion* srcOpndToUse = createSrcRegRegion(Mod_src_undef,
            Direct, tmpFCRet->getRegVar(), 0, 0, getRegionStride1(),
            Type_UD);

        last_inst = createInst(predOpnd,
            G4_pseudo_fc_ret,
            NULL,
            false,
            2,
            createNullDst(Type_UD),
            srcOpndToUse,
            NULL,
            instOpt,
            0);
    }
    else if( func_id == 0 )
    {
        // this will be lowered during CFG construction
        last_inst = createInst(
            predOpnd,
            G4_pseudo_exit,
            NULL,
            false,
            exsize,
            NULL,
            NULL,
            NULL,
            instOpt,
            0);
    }
    else
    {
        // subroutine return
        last_inst = createInst(
            predOpnd,
            Get_G4_Opcode_From_Common_ISA_Opcode(ISA_RET),
            NULL,
            false,
            exsize,
            NULL,
            NULL,
            NULL,
            instOpt,
            0);
    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}


// IsHeaderOptional - Check whether the message header is optional.
static bool IsMessageHeaderOptional(IR_Builder* builder, G4_Operand *surface,
                                    G4_Operand *Offset)
{

    // Message header is require for T255 stateless surface on pre-SKL devices
    // as a workaround for HW issue.
    if (builder->needsA32MsgHeader() && IsStatelessSurface(surface))
    {
        return false;
    }

    // Message Header is optional when offset is 0.
    // When GlobalOffset is 0, message header is optional.
    // "If the header is not present, behavior is as if the message was sent
    // with all fields in the header set to zero."
    return Offset->isImm() && Offset->asImm()->isZero();
}

static void BuildStatelessSurfaceMessageHeader(IR_Builder *IRB,
                                               G4_Declare *Header) {
    // For A32, clearing off scratch space offset or Buffer Base Address is
    // always required once header is present.

    G4_Type ElemTy = Header->getElemType();

    // R0.5<31:10> is defined as Scratch Space Offset.
    // R0.5<8:0> is defined as FF Thread ID (FFTID) in SKL+ devices.
    // R0.5<7:0> is defined as FF Thread ID (FFTID) in pre-SKL devices.
    // We increase the bit range to <9:0> to copy reserved bits as well.
    const unsigned FFTID_Mask = 0x3ff;

    // Rx.5[31:0] = 0 | R0.5[9:0]
    G4_DstRegRegion *DstOpnd = IRB->createDstRegRegion(Direct, Header->getRegVar(), 0, 5, 1, ElemTy);
    // R0.5
    G4_SrcRegRegion *SrcOpnd = IRB->createSrcRegRegion(Mod_src_undef, Direct,
                        IRB->getBuiltinR0()->getRegVar(), 0, 5,
                        IRB->getRegionScalar(), ElemTy);
    // Mask
    G4_Imm *Mask = IRB->createImm(FFTID_Mask, Type_UD);
    IRB->createInst(NULL, G4_and, NULL, false, 1, DstOpnd, SrcOpnd, Mask,
                    InstOpt_WriteEnable);
}

static void BuildUntypedStatelessSurfaceMessageHeader(IR_Builder *IRB,
                                                      G4_Declare *Header) {
    // Set PSM (Pixel Sample Mask) in MH1_A32_PSM
    G4_Type ElemTy = Header->getElemType();

    // R0.7<31:0> is defined as MHC_PSM where the lower 16 bits specify the
    // pixel sample mask.
    const unsigned PSM_Mask = 0xffff;

    // Rx.7[31:0] = 0xFFFF
    G4_DstRegRegion *DstOpnd = IRB->createDstRegRegion(Direct, Header->getRegVar(), 0, 7, 1, ElemTy);
    // Mask
    G4_Imm *Mask = IRB->createImm(PSM_Mask, Type_UD);
    IRB->createInst(NULL, G4_mov, NULL, false, 1, DstOpnd, Mask, NULL,
                    InstOpt_WriteEnable);

    BuildStatelessSurfaceMessageHeader(IRB, Header);
}

// if surface is PRED_SURF_255, lower it to PRED_SURF_253 so that it's non IA-coherent
// the surface is not changed otherwise
static G4_Operand* lowerSurface255To253(G4_Operand* surface, IR_Builder& builder)
{
    // disable due to OCL SVM atomics regression
#if 0
    if (surface && surface->isImm() && surface->asImm()->getImm() == PREDEF_SURF_255)
    {
        return builder.createImm(PREDEF_SURF_253, Type_UW);
    }
    else
#endif
    {
        return surface;
    }
}

static uint32_t setOwordForDesc(uint32_t desc, int numOword)
{
    switch (numOword)
    {   
    case 1: 
        return desc;
    case 2: 
        return desc | (0x2 << MESSAGE_SPECIFIC_CONTROL);
    case 4: 
        return desc | (0x3 << MESSAGE_SPECIFIC_CONTROL); 
    case 8: 
        return desc | (0x4 << MESSAGE_SPECIFIC_CONTROL);
    default:
        /// TODO(move to verifier): default: ASSERT_USER(false, "OWord block size must be 1/2/4/8.");
        return desc;
    }
}

/*
* Translates OWord Block read CISA inst.
*
* For GT, assume size is 8 then the code should look like
*
* .declare  VX Base=m ElementSize=4 Type=ud Total=8
* .declare  VY Base=r ElementSize=4 Type=ud Total=8
*
* mov  (8)     VX(0,0)<1>,  r0:ud
* mov  (1)     VX(0,2)<1>,  P
* send (8)     VY(0,0)<1>,  VX(0,0),    0x5,  0x02180200
* mov  (8)     v(0,0)<1>,   VY(0,0)
*
* P: M0.2 in the message header (Global offset)
*
* 0x5 == 0 (Not the EOT)
*
* 0x02180200 == Bit 31-29: 000 (Reserved)
*               Bit 28-25: 0001 (Msg. leng. = 1)
*               Bit 24-20: 00001 (Response msg. leng. = 1)
*               Bit 19:    1 (Header present)
*               Bit 18:    0 (Ignored) 
*               Bit 17:    0 (Send write commit message; ignored for read message
*               Bit 16-13: 0000 (Msg. type = OWord block read - for Render Cache)
*               Bit 12-8:  00010 (Block size = 2 OWords) - can only be 1/2/4/8 for sampler/render cache
*               Bit 7-0:   00000000 + I (Binding table index) 
*
*/
int IR_Builder::translateVISAOwordLoadInst(
    ISA_Opcode opcode,
    bool modified,
    G4_Operand* surface,
    Common_ISA_Oword_Num size,
    G4_Operand* offOpnd,
    G4_DstRegRegion* dstOpnd)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    surface = lowerSurface255To253(surface, *this);

    unsigned num_oword = Get_Common_ISA_Oword_Num( (Common_ISA_Oword_Num)size );
	bool unaligned = (opcode == ISA_OWORD_LD_UNALIGNED);

    // create dcl for VX
    G4_Declare *dcl = Create_MRF_Dcl( GENX_DATAPORT_IO_SZ, Type_UD );

    if (IsStatelessSurface(surface))
	{
        // Build stateless surface message header.
        BuildStatelessSurfaceMessageHeader(this, dcl);
    }

    /* mov (1)      VX(0,2)<1>,    P  */
    if (unaligned && (kernel.major_version == 3 && kernel.minor_version <= 1))
    {
        // for vISA3.1 and earlier
        // the offset for unaligned OW load is in unit of DW, tranlate it into BYTE.
        if (offOpnd->isImm())
        {
            // imm type must be UD as the result of shift could overflow word type
            G4_Imm *new_src_opnd1 = createImm(
                offOpnd->asImm()->getInt() << 2, Type_UD);
            Create_MOV_Inst(dcl, 0, 2, 1, NULL, NULL, new_src_opnd1, true);
        }
        else
        {
            G4_DstRegRegion dst(Direct, dcl->getRegVar(), 0, 2, 1, dcl->getElemType());
            G4_DstRegRegion* dstOpnd = createDstRegRegion(dst);

            createInst(NULL, G4_shl, NULL, false, 1, dstOpnd, offOpnd,
                createImm(2, Type_UW), InstOpt_WriteEnable);
        }
    }
    else
    {
        dcl->setCapableOfReuse();
        Create_MOV_Inst(dcl, 0, 2, 1, NULL, NULL, offOpnd, true);
    }
    // send's operands preparation
    G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
    G4_DstRegRegion* d = Check_Send_Dst(dstOpnd->asDstRegRegion());

    uint32_t temp = 0;

    if (unaligned)
    {
        SET_DATAPORT_MESSAGE_TYPE(temp, DC_UNALIGNED_OWORD_BLOCK_READ)
    }

    // Set bit 12-8 for the message descriptor
    temp = setOwordForDesc(temp, num_oword);

    // !!!WHY???
    if (num_oword > 2)
    {
        // redefine the type and offset of post dst.
        if ((d->getType() != Type_W) &&
            (d->getType() != Type_UW)) {
            short new_SubRegOff = dstOpnd->asDstRegRegion()->getSubRegOff();
            if (dstOpnd->getRegAccess() == Direct){
                new_SubRegOff = (dstOpnd->asDstRegRegion()->getSubRegOff() * G4_Type_Table[dstOpnd->getType()].byteSize) / G4_Type_Table[Type_W].byteSize;
            }
            G4_DstRegRegion new_dst(
                dstOpnd->getRegAccess(),
                dstOpnd->asDstRegRegion()->getBase(),
                dstOpnd->asDstRegRegion()->getRegOff(),
                new_SubRegOff,
                1,
                Type_W);
            d = createDstRegRegion(new_dst);
        }
    }

    CISA_SHARED_FUNCTION_ID tf_id =  SFID_DP_DC;

    unsigned send_exec_size = FIX_OWORD_SEND_EXEC_SIZE(num_oword);
    bool forceSplitSend = IsBindlessSurface(*this, surface);

    if (!forceSplitSend) 
    {
        last_inst = Create_Send_Inst_For_CISA(
            NULL, d,
            payload,
            1,
            (num_oword - 1) / 2 + 1,
            send_exec_size,
            temp,
            tf_id,
            0,
            1,
            true,
            false,
            surface,
            NULL,
            InstOpt_WriteEnable,
            false);
    }
    else {
        G4_SrcRegRegion *m0 = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
        last_inst = Create_SplitSend_Inst_For_CISA(
            NULL, d, m0, 1,
            createNullSrc(Type_UD), 0,
            (num_oword - 1) / 2 + 1,
            send_exec_size,
            temp,
            0,
            tf_id,
            false,
            true,
            true,
            false,
            surface,
            NULL,
            InstOpt_WriteEnable,
            false);
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

/*
* Translates OWord Block write intrinsic.
*
* write(I, P, vector<int, S> v)
*
* For GT, assume S = 8 then the code should look like
*
* .declare  VX Base=m ElementSize=4 Type=ud Total=16
* .declare  VY Base=m ElementSize=4 Type=ud Total=8  ALIAS(VX,8)
*
* mov  (8)     VX(0,0)<1>,  r0:ud
* mov  (8)     VY(0,0)<1>,  v       // mov  (8)     VX(1,0)<1>,  v
* mov  (1)     VX(0,2)<2>,  P
* send (8)     null<1>,  VX(0,0),  0x5,   0x04090200
*
* P: M0.2 in the message header (Global offset) 
*
* 0x5 == 0 (Not the EOT)
*        0101 (Target Function ID: DP Render Cache) 
*
* 0x04090200 == Bit 31-29: 000 (Reserved)
*               Bit 28-25: 0010 (Msg. leng. = 2)
*               Bit 24-20: 00000 (Response msg. leng. = 0)
*               Bit 19:    1 (Header present)
*               Bit 18:    0 (Ignored) 
*               Bit 17:    0 (Send write commit message
*               Bit 16-13: 1000 (Msg. type = OWord block read - for Render Cache) 
*               Bit 12-8:  00010 (Block size = 2 OWords) - can only be 1/2/4/8 for sampler/render cache
*               Bit 7-0:   00000000 + I (Binding table index)
*
*/

int IR_Builder::translateVISAOwordStoreInst(
    G4_Operand* surface,
    Common_ISA_Oword_Num size,
    G4_Operand* offOpnd,
    G4_SrcRegRegion* srcOpnd )
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    surface = lowerSurface255To253(surface, *this);

    unsigned num_oword = Get_Common_ISA_Oword_Num( (Common_ISA_Oword_Num)size );
    unsigned obj_size = num_oword * 16; // size of obj in bytes

    unsigned funcCtrl = DC_OWORD_BLOCK_WRITE << 14;

    // Set bit 12-8 for the message descriptor
    funcCtrl = setOwordForDesc(funcCtrl, num_oword);
    
    if (useSends())
    {
        G4_Declare *headerDcl = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ, Type_UD);

        if (IsStatelessSurface(surface))
        {
            // Build stateless surface message header.
            BuildStatelessSurfaceMessageHeader(this, headerDcl);
        }

        /* mov (1)     VX(0,2)<1>,   P  */
        Create_MOV_Inst( headerDcl, 0, 2, 1, NULL, NULL, offOpnd, true );

        unsigned msgDesc = funcCtrl;
        unsigned extMsgLength = (num_oword - 1) / 2 + 1;
        uint16_t extFuncCtrl = 0;

        // message length = 1, response length = 0, header present = 1
        msgDesc += (1 << getSendMsgLengthBitOffset()) + (1 << getSendHeaderPresentBitOffset());

        G4_SendMsgDescriptor* desc = createSendMsgDesc( msgDesc, 0, 1, SFID_DP_DC,
            false, extMsgLength, extFuncCtrl, false, true, surface, NULL );

        uint8_t sendSize = FIX_OWORD_SEND_EXEC_SIZE(num_oword);

        G4_SrcRegRegion* src0 = Create_Src_Opnd_From_Dcl(headerDcl, getRegionStride1());
        G4_DstRegRegion* dst = createNullDst( sendSize > 8 ? Type_UW: Type_UD );
        G4_Operand* msgOpnd = NULL;
        if (surface->isImm())
        {
            msgDesc += (unsigned) surface->asImm()->getInt();
            msgOpnd = createImm(msgDesc, Type_UD);
        }
        else
        {
            G4_DstRegRegion *addrDst = Create_Dst_Opnd_From_Dcl( builtinA0, 1 );
            //add (1) a0.0:ud bti:ud desc:ud {NoMask}
            createInst(
                NULL,
                G4_add,
                NULL,
                false,
                1,
                addrDst,
                surface,
                createImm( msgDesc, Type_UD ),
                InstOpt_WriteEnable,
                0 );
            msgOpnd = Create_Src_Opnd_From_Dcl( builtinA0, getRegionScalar());
        }
        createSplitSendInst( NULL, G4_sends, sendSize, dst, src0, srcOpnd, msgOpnd, InstOpt_WriteEnable, desc, NULL, 0);
    }
    else
    {

        /* Size of whole mrf opnd in UINT elements */
        uint32_t temp =  obj_size/G4_Type_Table[Type_UD].byteSize + GENX_DATAPORT_IO_SZ;

        // decl for mrfs
        G4_Declare *dcl = Create_MRF_Dcl(temp, Type_UD);

        /* mov  (c*r)    VX(1,0)<1>,  V */
        temp =  obj_size/G4_Type_Table[Type_UD].byteSize;

        Create_MOV_Send_Src_Inst( dcl, 1, 0, temp, srcOpnd, InstOpt_WriteEnable );

        if (IsStatelessSurface(surface)) {
            // Build stateless surface message header.
            BuildStatelessSurfaceMessageHeader(this, dcl);
        } else {
            // Copy R0 header.
            Create_MOVR0_Inst( dcl, 0, 0, true );
        }

        /* mov (1)     VX(0,2)<1>,   P  */
        Create_MOV_Inst( dcl, 0, 2, 1, NULL, NULL, offOpnd, true );

        // send's operands preparation
        /* Size of whole operand in UINT elements */
        G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());

        unsigned send_size = FIX_OWORD_SEND_EXEC_SIZE(num_oword);
        G4_DstRegRegion *post_dst_opnd = createNullDst( send_size > 8 ? Type_UW: Type_UD );

        G4_INST *send_inst = Create_Send_Inst_For_CISA(
            NULL,
            post_dst_opnd,
            payload,
            ((num_oword - 1) / 2 + 1) + 1,
            0,
            send_size,
            funcCtrl,
            SFID_DP_DC,
            false,
            true,
            false,
            true,
            surface,
            NULL,
            InstOpt_WriteEnable,
            false );
        last_inst = send_inst;
    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

/*
* Translates Media Block read CISA inst.
*
* read(I, X, Y, matrix<int,C,R> M)
* Assume C = R = 8 then code shoud look like
*
* .declare  VX Base=m ElementSize=4 Type=ud Total=8
* .declare  VY Base=r ElementSize=4 Type=ud Total=8
*
* mov  (8)     VX(0,0)<1>,  r0.0:ud
* mov  (1)     VX(0,2)<1>,  0x0007001f   // 8 rows, 32 bytes
* mov  (1)     VX(0,1)<1>,  Y
* mov  (1)     VX(0,0)<1>,  X
* send (8)     VY(0,0)<1>,  VX(0,0),    null,  0x04186000
* mov  (8)     M(0,0)<1>,   VY(0,0)
*
* 0x0007001f == (R-1)<<16 + C * sizeof(el_type) - 1;
*
* 0x04186000 ==
*  (((ObjectSize - 1) / GENX_GRF_REG_SIZ + 1)) << 16 +
*          0x4100000 + 0x6000 + I;
*
* ObjectSize = RoundUpPow2( C ) * R * sizeof(el_type);
*/
int IR_Builder::translateVISAMediaLoadInst(
    MEDIA_LD_mod mod,
    G4_Operand* surface,
    unsigned planeID,
    unsigned blockWidth,
    unsigned blockHeight,
    G4_SrcRegRegion* xOffOpnd,
    G4_SrcRegRegion* yOffOpnd,
    G4_DstRegRegion* dstOpnd )
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    unsigned temp;

    unsigned objWidth = 0;
    if (blockWidth != 0)
    {
        objWidth = this->getObjWidth(blockWidth, blockHeight, dstOpnd->getBase()->asRegVar()->getDeclare());
    }
    unsigned obj_size = objWidth * blockHeight;

    /* mov (8)      VX(0,0)<1>,  r0:ud  */
    // add dcl for VX
    G4_Declare *dcl = Create_MRF_Dcl( GENX_DATAPORT_IO_SZ, Type_UD );

    // create MOV inst
    Create_MOVR0_Inst( dcl, 0, 0, true );
    /* mov (1)      VX(0,2)<1>,    CONST[R,C]  */
    temp = (blockHeight - 1) << 16 | (blockWidth - 1);
    Create_MOV_Inst( dcl, 0, 2, 1, NULL, NULL, createImm( temp, Type_UD ), true );
    /* mov (1)     VX(0,0)<1>,    X  */
    Create_MOV_Inst( dcl, 0, 0, 1, NULL, NULL, xOffOpnd, true );
    /* mov (1)     VX(0,1)<1>,   Y  */
    Create_MOV_Inst( dcl, 0, 1, 1, NULL, NULL, yOffOpnd, true );

    // send's operands preparation
    // create a currDst for VX
    G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl( dcl, getRegionStride1() );

    // mediaread overwrites entire GRF
    bool via_temp = false;
    G4_Operand *original_dst = NULL;
    G4_Declare *new_dcl = NULL;

    if (obj_size < GENX_GRF_REG_SIZ)
    {
        via_temp = true;
    }
    else
    {
        unsigned byte_subregoff = dstOpnd->asDstRegRegion()->getSubRegOff() * G4_Type_Table[dstOpnd->getType()].byteSize;
        G4_VarBase *base = dstOpnd->asDstRegRegion()->getBase();
        G4_Declare *dcl = base->asRegVar()->getDeclare();

        if (byte_subregoff  % G4_GRF_REG_NBYTES != 0)
        {
            via_temp = true;
        }
        else
        {
            G4_Declare *aliasdcl = dcl;
            bool false_alias_align = false;
            while (aliasdcl->getAliasDeclare()){
                if (aliasdcl->getAliasOffset() % G4_GRF_REG_NBYTES != 0){
                    false_alias_align = true;
                    break;
                }
                aliasdcl = aliasdcl->getAliasDeclare();
            }
            if (false_alias_align){
                via_temp = true;
            }
        }
    }

    if( via_temp == true )
    {

        original_dst = dstOpnd;
        new_dcl = createTempVar( GENX_GRF_REG_SIZ/G4_Type_Table[Type_UD].byteSize,
            Type_UD, Either, Sixteen_Word );
        G4_DstRegRegion tmp_dst( Direct,
            new_dcl->getRegVar(),
            0,
            0,
            1,
            Type_UD);

        G4_DstRegRegion *tmp_dst_opnd = createDstRegRegion( tmp_dst );
        dstOpnd = tmp_dst_opnd;
    }

    G4_DstRegRegion* d = Check_Send_Dst( dstOpnd->asDstRegRegion());

    temp = 0;
    if((mod == MEDIA_LD_top) || (mod == MEDIA_LD_top_mod)) {
        temp += 0x6 << MESSAGE_SPECIFIC_CONTROL;    // Read top fields
    } else if((mod == MEDIA_LD_bottom) || (mod == MEDIA_LD_bottom_mod)) {
        temp += 0x7 << MESSAGE_SPECIFIC_CONTROL;    // Read bottom fields
    }

    SET_DATAPORT_MESSAGE_TYPE(temp, DC1_MEDIA_BLOCK_READ)

    temp += planeID;

    unsigned send_exec_size = GENX_DATAPORT_IO_SZ;
    if( IS_WTYPE(d->getType()) )
    {
        send_exec_size *= 2;
    }

    G4_INST *send_inst = Create_Send_Inst_For_CISA(
        NULL,
        d,
        payload,
        1,
        (obj_size - 1) / GENX_GRF_REG_SIZ + 1,
        send_exec_size,
        temp,
        SFID_DP_DC1,
        0,
        1,
        true,
        false,
        surface,
        NULL,
        InstOpt_WriteEnable,
        false );
    last_inst = send_inst;

    if( via_temp )
    {
        G4_Declare *new_dcl2 = createTempVar(
            GENX_GRF_REG_SIZ/G4_Type_Table[original_dst->getType()].byteSize,
            original_dst->getType(), Either, Sixteen_Word );

        new_dcl2->setAliasDeclare( new_dcl, 0 );

        unsigned short remained_ele = obj_size / G4_Type_Table[original_dst->getType()].byteSize;
        // max execution size is 32
        unsigned char curr_exec_size = 16;
        unsigned char curr_offset = 0;

        G4_Type dstType = original_dst->getType();
        while( curr_exec_size >= 1 )
        {
            short dst_regoff = original_dst->asDstRegRegion()->getRegOff();
            short dst_subregoff = original_dst->asDstRegRegion()->getSubRegOff();
            if( remained_ele >= curr_exec_size )
            {
                G4_SrcRegRegion *tmp_src_opnd = createSrcRegRegion(
                    Mod_src_undef,
                    Direct,
                    new_dcl2->getRegVar(),
                    0,
                    curr_offset,
                    curr_exec_size == 1 ? getRegionScalar() : getRegionStride1(),
                    original_dst->getType());

                dst_subregoff += curr_offset;
                short ele_per_grf = G4_GRF_REG_NBYTES/G4_Type_Table[dstType].byteSize;
                if( dst_subregoff >= ele_per_grf )
                {
                    dst_regoff += 1;
                    dst_subregoff -= ele_per_grf;
                }
                G4_DstRegRegion tmp_dst(
                    Direct,
                    original_dst->asDstRegRegion()->getBase(),
                    dst_regoff,
                    dst_subregoff,
                    1,
                    original_dst->getType());

                G4_DstRegRegion *tmp_dst_opnd = createDstRegRegion( tmp_dst );

                last_inst = createInst( NULL, G4_mov, NULL, false, curr_exec_size, tmp_dst_opnd, tmp_src_opnd, NULL, InstOpt_WriteEnable );
                curr_offset += curr_exec_size;
                remained_ele -= curr_exec_size;
            }
            curr_exec_size /= 2;
        }
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

/*
* Translates Media Block write CISA inst.
*
* write(I, X, Y, matrix<int,C,R> M)
* Assume C = R = 8 then code shoud look like
*
* .declare  VX Base=m ElementSize=4 Type=ud Total=72
* .declare  VY Base=m ElementSize=4 Type=ud Total=64 ALIAS(VX,32)
*
* mov  (8)     VX(0,0)<1>,  r0.0:ud
* mov  (64)    VY(0,0)<1>,  M
* mov  (1)     VX(0,2)<1>,  0x0007001f   // 8 rows, 32 bytes
* mov  (1)     VX(0,1)<1>,  Y
* mov  (1)     VX(0,0)<1>,  X
* send (8)     null<1>,  VX(0,0),  null,   0x05902000
*
* 72 = 8 + C * R
* 0x0007001f is (R-1)<<16 + C * sizeof(el_type) - 1
*
* 0x05902000 ==
*  ((((ObjectSize - 1) / GENX_GRF_REG_SIZ + 1)) + 1)<<20 +
*          0x5000000 + 0x2000 + I
* ObjectSize = RoundUpPow2( C ) * R * sizeof(el_type)
*/
int IR_Builder::translateVISAMediaStoreInst(
    MEDIA_ST_mod mod,
    G4_Operand* surface,
    unsigned planeID,
    unsigned blockWidth,
    unsigned blockHeight,
    G4_SrcRegRegion* xOffOpnd,
    G4_SrcRegRegion* yOffOpnd,
    G4_SrcRegRegion* srcOpnd )
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    int objWidth = 0;
    if( blockWidth != 0 )
    {
        objWidth = this->getObjWidth(blockWidth, blockHeight, srcOpnd->getBase()->asRegVar()->getDeclare());
    }
    unsigned obj_size = objWidth * blockHeight;
    unsigned int new_obj_size = obj_size;

    auto setTopBottomForDesc = [](uint32_t desc, MEDIA_ST_mod mod)
    {
        if (mod == MEDIA_ST_top)
        {
            return desc + (0x6 << MESSAGE_SPECIFIC_CONTROL);    // Write top fields
        }
        else if (mod == MEDIA_ST_bottom)
        {
            return desc + (0x7 << MESSAGE_SPECIFIC_CONTROL);    // Write bottom fields
        }
        return desc;
    };

    //TODO: Use higher level API for split send
    if (useSends())
    {
        // use split send
        G4_Declare *headerDcl = Create_MRF_Dcl(8, Type_UD);
        Create_MOVR0_Inst( headerDcl, 0, 0, true );
        /* mov (1)      VX(0,2)<1>,    CONST[R,C]  */
        uint32_t temp = (blockHeight - 1) << 16 | (blockWidth - 1);
        Create_MOV_Inst( headerDcl, 0, 2, 1, NULL, NULL, createImm( temp, Type_UD ), true);

        /* mov (1)     VX(0,0)<1>,    X  */
        Create_MOV_Inst( headerDcl, 0, 0, 1, NULL, NULL, xOffOpnd, true );

        /* mov (1)     VX(0,1)<1>,   Y  */
        Create_MOV_Inst( headerDcl, 0, 1, 1, NULL, NULL, yOffOpnd, true );

        G4_SrcRegRegion* headerOpnd = Create_Src_Opnd_From_Dcl(headerDcl, getRegionStride1());

        unsigned msgDesc = setTopBottomForDesc(0, mod);
        SET_DATAPORT_MESSAGE_TYPE(msgDesc, DC1_MEDIA_BLOCK_WRITE)

        msgDesc += planeID;
        // message length = 1, response length = 0, header present = 1
        msgDesc += (1 << getSendMsgLengthBitOffset()) + (1 << getSendHeaderPresentBitOffset());
        G4_DstRegRegion *dstOpnd = createNullDst( Type_UD );

        unsigned extMsgLength = (obj_size - 1) / GENX_GRF_REG_SIZ + 1;
        uint16_t extFuncCtrl = 0;

        G4_SendMsgDescriptor* desc = createSendMsgDesc( msgDesc, 0, 1, SFID_DP_DC1,
            false, extMsgLength, extFuncCtrl, false, true, surface, NULL);

        G4_Operand* msgOpnd = NULL;
        if (surface->isImm())
        {
            msgDesc += (unsigned) surface->asImm()->getInt();
            msgOpnd = createImm(msgDesc, Type_UD);
        }
        else
        {
            G4_DstRegRegion *addrDst = Create_Dst_Opnd_From_Dcl( builtinA0, 1 );
            //add (1) a0.0:ud bti:ud desc:ud
            // create source for bti
            createInst(
                NULL,
                G4_add,
                NULL,
                false,
                1,
                addrDst,
                surface,
                createImm( msgDesc, Type_UD ),
                InstOpt_WriteEnable,
                0 );
            msgOpnd = Create_Src_Opnd_From_Dcl( builtinA0, getRegionScalar());
        }

        createSplitSendInst( NULL, G4_sends, 8, dstOpnd, headerOpnd, srcOpnd, msgOpnd, InstOpt_WriteEnable, desc, NULL, 0);
    }
    else
    {
        /* Size of whole mrf opnd in UINT elements */
        uint32_t temp =  new_obj_size/G4_Type_Table[Type_UD].byteSize + GENX_DATAPORT_IO_SZ;

        // decl for mrfs
        G4_Declare *dcl = Create_MRF_Dcl(temp, Type_UD);

        /* mov  (c*r)    VX(1,0)<1>,  M */
        /* decl for data to write */
        temp =  obj_size/G4_Type_Table[Type_UD].byteSize;

        Create_MOV_Send_Src_Inst( dcl, 1, 0, temp, srcOpnd, InstOpt_WriteEnable );

        // Decl for i/o MRF
        Create_MOVR0_Inst( dcl, 0, 0, true );

        /* mov (1)      VX(0,2)<1>,    CONST[R,C]  */
        temp = (blockHeight - 1) << 16 | (blockWidth - 1);
        Create_MOV_Inst( dcl, 0, 2, 1, NULL, NULL, createImm( temp, Type_UD ), true);

        /* mov (1)     VX(0,0)<1>,    X  */
        Create_MOV_Inst( dcl, 0, 0, 1, NULL, NULL, xOffOpnd, true );

        /* mov (1)     VX(0,1)<1>,   Y  */
        Create_MOV_Inst( dcl, 0, 1, 1, NULL, NULL, yOffOpnd, true );

        // send's operands preparation
        /* Size of whole operand in UINT elements */
        G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());

        uint32_t funcCtrl = setTopBottomForDesc(0, mod);
        SET_DATAPORT_MESSAGE_TYPE(funcCtrl, DC1_MEDIA_BLOCK_WRITE);

        funcCtrl += planeID;
        G4_DstRegRegion *post_dst_opnd = createNullDst( Type_UD );

        last_inst = Create_Send_Inst_For_CISA(
            NULL,
            post_dst_opnd,
            payload,
            ((obj_size - 1) / GENX_GRF_REG_SIZ + 1) + 1,
            0,
            GENX_DATAPORT_IO_SZ,
            funcCtrl,
            SFID_DP_DC1,
            0,
            1,
            false,
            true,
            surface,
            NULL,
            InstOpt_WriteEnable,
            false );
    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

/*
* Translates scattered read intrinsic.
*
* For GT, assume N = 8 then the code should look like
*
* .declare  VX Base=m ElementSize=4 Type=ud Total=16
* .declare  VY Base=r ElementSize=4 Type=ud Total=8
*
* mov  (8)     VX(0,0)<1>,  r0:ud
* mov  (1)     VX(0,2)<1>,  P
* mov  (8)     VX(1,0)<1>,  E
* send (8)     VY(0,0)<1>,  VX(0,0),    0x5,  0x0418C200
*
* P: M0.2 in the message header (Global offset) 
* E: M1 in the message payload (Element offsets) 
* 0x5 == 0 (Not the EOT)
*        0101 (Target Function ID: DP Render Cache)
*
* 0x0418C200 == Bit 31-29: 000 (Reserved)
*               Bit 28-25: 0010 (Msg. leng. = 2) 
*               Bit 24-20: 00001 (Response msg. leng. = 1) 
*               Bit 19:    1 (Header present)
*               Bit 18:    0 (Ignored)
*               Bit 17:    0 (Send write commit message; ignored for read message 
*               Bit 16-13: 0110 (Msg. type = DWord Scattered read - for Render Cache)
*               Bit 12-10: 010 Specifies the data size for each slot. 0: 1 byte; 1: 2 bytes; 2: 4 bytes; 3: Reserved
*               Bit 9-8:  00 (Block size = 8 DWords) 
*               Bit 7-0:   00000000 + I (Binding table index) 
*
*/
int IR_Builder::translateVISAGatherInst(
    Common_VISA_EMask_Ctrl emask,
    bool modified,
    GATHER_SCATTER_ELEMENT_SIZE eltSize,
    Common_ISA_Exec_Size executionSize,
    G4_Operand* surface,
    G4_Operand* gOffOpnd,
    G4_SrcRegRegion* eltOffOpnd,
    G4_DstRegRegion* dstOpnd
    )
{
    surface = lowerSurface255To253(surface, *this);

    // Before GEN10, we translate DWORD GATHER on SLM to untyped GATHER4 on
    // SLM with only R channel enabled. The later is considered more
    // efficient without recalculating offsets in BYTE.
    if (eltSize == GATHER_SCATTER_DWORD && IsSLMSurface(surface)) {
        return translateVISAGather4Inst(emask, modified,
                                        ChannelMask::createFromAPI(CHANNEL_MASK_R),
                                        executionSize, surface, gOffOpnd,
                                        eltOffOpnd, dstOpnd);
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    uint8_t exsize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    unsigned int instOpt = Get_Gen4_Emask(emask, exsize);
    bool headerLess = IsMessageHeaderOptional(this, surface, gOffOpnd);
    // Element size in gather/scatter message. Initially, we assume it's the
    // same as the request.
    GATHER_SCATTER_ELEMENT_SIZE msgEltSize = eltSize;

    // SLM access
    //              HEADLESS    BYTE    WORD    DWORD
    // BDW          Opt         YES     NO      NO
    // SKL          Req         YES     NO      NO
    // CNL          Req         YES     NO      YES

    G4_Predicate* pred = NULL; // for SIMD1 gather
    uint8_t numElt = mapExecSizeToNumElts[executionSize];
    // we need to treat simd1 as simd8 in several places during code gen
    uint8_t effectiveNumElt = (numElt == 1 ? 8 : numElt);

    if (!headerLess && noSLMMsgHeader() && IsSLMSurface(surface)) 
    {
        // From SKL, SLM messages forbid message header. Recalculate offset by
        // adding global offset and force headerLess.
        G4_Declare *dcl = Create_MRF_Dcl(numElt, eltOffOpnd->getType());
        dcl->setSubRegAlign(Sixteen_Word);
        G4_DstRegRegion *newEltOffOpnd = Create_Dst_Opnd_From_Dcl(dcl, 1);
        createInst(NULL, G4_add, NULL, false, numElt, newEltOffOpnd, eltOffOpnd, gOffOpnd, instOpt);
        eltOffOpnd = Create_Src_Opnd_From_Dcl(dcl, numElt == 1 ? getRegionScalar() : getRegionStride1());
        headerLess = true;
    }

    bool useSplitSend = useSends();
    // When header is not required, split-send is not needed as there's only
    // one part in the message. When header is present, we will split the
    // message as (header, offset).
    if (headerLess)
        useSplitSend = false;

    G4_Declare *header = 0;
    G4_Declare *offset = Create_MRF_Dcl(numElt, Type_UD);
    offset->setSubRegAlign(Sixteen_Word);

    if (useSplitSend) 
    {
        ASSERT_USER(!headerLess, "SplitSend should not be used when header is not required!");
        // Without header, it's unnecessary to split the message.
        header = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ, Type_UD);
    } 
    else if (!headerLess) 
    {
        header = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ + effectiveNumElt, Type_UD);
        offset->setAliasDeclare(header, GENX_MRF_REG_SIZ);
    }

    G4_SrcRegRegion* msgSrcOpnd = NULL;

    if( headerLess )
    {
        ASSERT_USER(!header, "'header' should not be allocated when header is not required!");

        if (eltSize == GATHER_SCATTER_WORD ||
            (eltSize != GATHER_SCATTER_BYTE && IsSLMSurface(surface))) 
        {
                // Use byte gather for WORD gather as well as SLM surfaces (only supports byte gather)
                // need a shift to make the offset to be byte offset
                // shl (8) tmp<1>:ud elt_off<8;8,1>:ud 0x2:uw
                // Don't do this for Dword because we use the dword scatter message instead
                G4_DstRegRegion* tmpDstOpnd = Create_Dst_Opnd_From_Dcl(offset, 1);
                createInst( NULL, G4_shl, NULL, false, numElt, tmpDstOpnd, eltOffOpnd,
                    createImm( unsigned(eltSize), Type_UD ), instOpt );
                msgSrcOpnd = Create_Src_Opnd_From_Dcl(offset, getRegionStride1());
                msgEltSize = GATHER_SCATTER_BYTE;
        }
        else
        {
            msgSrcOpnd = eltOffOpnd;
        }
    }
    else
    {
        if (IsStatelessSurface(surface)) {
            // Build stateless surface message header.
            BuildStatelessSurfaceMessageHeader(this, header);
        } else {
            // Copy R0 header.
            Create_MOVR0_Inst(header, 0, 0, true);
        }

        G4_DstRegRegion* dst1_opnd = createDstRegRegion(Direct, offset->getRegVar(), 0, 0, 1, offset->getElemType());

        if (eltSize == GATHER_SCATTER_WORD || IsSLMSurface(surface)) 
        {
                // For non-SLM surface, WORD gather/scatter has no hardware
                // supportr and must be translated into BYTE gather/scatter.
                //
                // SLM surface supports only BYTE gather/scatter
                // support and also needs translating into BYTE gather/scatter.
                //
                /* mov (1)     VX(0,2)<1>,   P  */
                if( gOffOpnd->isImm() )
                {
                    G4_Imm *new_src_opnd1 = createImm(
                        gOffOpnd->asImm()->getInt() * ( eltSize == GATHER_SCATTER_WORD ? 2 : 4 ),
                        gOffOpnd->getType() );
                    Create_MOV_Inst(header, 0, 2, 1, NULL, NULL, new_src_opnd1, true);
                }
                else
                {
                    G4_DstRegRegion dst2(Direct, header->getRegVar(), 0, 2, 1, header->getElemType());
                    G4_DstRegRegion* dst2_opnd = createDstRegRegion( dst2 );

                    createInst( NULL, G4_shl, NULL, false, 1, dst2_opnd, gOffOpnd,
                        createImm( (unsigned)eltSize, Type_UD ), InstOpt_WriteEnable );
                }
                createInst( NULL, G4_shl, NULL, false, numElt, dst1_opnd, eltOffOpnd,
                    createImm( (unsigned)eltSize, Type_UD ), instOpt );
                msgEltSize = GATHER_SCATTER_BYTE;
        }
        else
        {
            /* mov (1)     VX(0,2)<1>,   P  */
            Create_MOV_Inst(header, 0, 2, 1, NULL, NULL, gOffOpnd, true);
            /* mov  (numElt)    VX(1,0)<1>,  E */
            createInst( NULL, G4_mov, NULL, false, numElt, dst1_opnd,
                eltOffOpnd, NULL, instOpt);
        }

        // Create a <8;8,1> src region for the send payload
        msgSrcOpnd = Create_Src_Opnd_From_Dcl(header, getRegionStride1());
    }

    G4_DstRegRegion* d = dstOpnd->asDstRegRegion();

    CISA_SHARED_FUNCTION_ID tf_id = SFID_DP_DC;
    unsigned temp = 0;
    // Set bit 9-8 for the message descriptor
    if (msgEltSize == GATHER_SCATTER_DWORD)
    {
        if (effectiveNumElt == 8)
        {
            temp += 2 << 8;
        }
        else {
            temp += 3 << 8;
        }
        temp += DC_DWORD_SCATTERED_READ << 14; // '0011' for DWORD scattered read
    }
    else
    {
        if (effectiveNumElt == 16)
        {
            temp += 1 << 8;
        }
        temp += (unsigned char)eltSize << 10;
        temp += DC_BYTE_SCATTERED_READ << 14; 
    }

    if (useSplitSend) {
        ASSERT_USER(!headerLess, "SplitSend should only be used when header is required!");

        G4_SrcRegRegion *m0 = Create_Src_Opnd_From_Dcl(header, getRegionStride1());
        G4_SrcRegRegion *m1 = Create_Src_Opnd_From_Dcl(offset, getRegionStride1());
        last_inst = Create_SplitSend_Inst_For_CISA(pred, d,
                                                   m0, 1,
                                                   m1, effectiveNumElt/GENX_DATAPORT_IO_SZ,
                                                   effectiveNumElt/GENX_DATAPORT_IO_SZ,
                                                   numElt,
                                                   temp, 0,
                                                   tf_id, false, true,
                                                   true, false,
                                                   surface, NULL, instOpt, false);
    } else {
        last_inst = Create_Send_Inst_For_CISA(
            pred,
            d,
            msgSrcOpnd,
            headerLess ? effectiveNumElt/GENX_DATAPORT_IO_SZ : effectiveNumElt/GENX_DATAPORT_IO_SZ + 1,
            effectiveNumElt/GENX_DATAPORT_IO_SZ,
            numElt,
            temp,
            tf_id,
            0,
            !headerLess,
            true,
            false,
            surface,
            NULL,
            instOpt,
            false );
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

/*
* Translates scattered write intrinsic.
*
* For GT, assume N = 8 then the code should look like
*
* .declare  VX Base=m ElementSize=4 Type=ud Total=24
*
* mov  (8)     VX(0,0)<1>,  r0:ud
* mov  (1)     VX(0,2)<1>,  P
* mov  (8)     VX(1,0)<1>,  E
* mov  (8)     VX(2,0)<1>,  V
* send (8)     null<1>,     VX(0,0),    0x5,  0x06096200
*
* P: M0.2 in the message header (Global offset)
* E: M1 in the message payload (Element offsets)
* v: M2 in the message payload (written data) 
*
* 0x5 == 0 (Not the EOT)
*        0101 (Target Function ID: DP Render Cache)
*
* 0x06096200 == Bit 31-29: 000 (Reserved)
*               Bit 28-25: 0011 (Msg. leng. = 3)
*               Bit 24-20: 00000 (Response msg. leng. = 0)
*               Bit 19:    1 (Header present)
*               Bit 18:    0 (Ignored)
*               Bit 17:    0 (Send write commit message)
*               Bit 16-13: 1011 (Msg. type = DWord Scattered write - for Render Cache)
*               Bit 12-8:  00010 (Block size = 8 DWords)
*               Bit 7-0:   00000000 + I (Binding table index)
*
*/
int IR_Builder::translateVISAScatterInst(
    Common_VISA_EMask_Ctrl emask,
    GATHER_SCATTER_ELEMENT_SIZE eltSize,
    Common_ISA_Exec_Size executionSize,
    G4_Operand* surface,
    G4_Operand* gOffOpnd,
    G4_SrcRegRegion* eltOffOpnd,
    G4_SrcRegRegion* srcOpnd )
{
    surface = lowerSurface255To253(surface, *this);
    // Before GEN10, we translate DWORD SCATTER on SLM to untyped GATHER4 on
    // SLM with only R channel enabled. The later is considered more
    // efficient without recalculating offsets in BYTE.
    if (eltSize == GATHER_SCATTER_DWORD && IsSLMSurface(surface)) {
        return translateVISAScatter4Inst(emask,
                                         ChannelMask::createFromAPI(CHANNEL_MASK_R),
                                         executionSize, surface, gOffOpnd,
                                         eltOffOpnd, srcOpnd);
    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    uint8_t exsize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    unsigned int instOpt = Get_Gen4_Emask(emask, exsize);
    G4_Predicate *pred = NULL;
    // Element size in gather/scatter message. Initially, we assume it's the same as the request.
    GATHER_SCATTER_ELEMENT_SIZE msgEltSize = eltSize;

    uint8_t numElt = mapExecSizeToNumElts[executionSize];
    // we need to treat simd1 as simd8 in several places during code gen
    uint8_t effectiveNumElt = (numElt == 1 ? 8 : numElt);

    bool headerLess = IsMessageHeaderOptional(this, surface, gOffOpnd);
    G4_SrcRegRegion* msgSrcOpnd = NULL;

    // SLM access
    //              HEADLESS    BYTE    WORD    DWORD
    // BDW          Opt         YES     NO      NO
    // SKL          Req         YES     NO      NO
    // CNL          Req         YES     NO      YES

    if (!headerLess && noSLMMsgHeader() && IsSLMSurface(surface)) {
        // From SKL, SLM messages forbid message header. Recalculate offset by
        // adding global offset and force headerLess.
        G4_Declare *dcl = Create_MRF_Dcl(numElt, eltOffOpnd->getType());
        G4_DstRegRegion *newEltOffOpnd = Create_Dst_Opnd_From_Dcl(dcl, 1);
        createInst(NULL, G4_add, NULL, false, numElt, newEltOffOpnd, eltOffOpnd, gOffOpnd, instOpt);
        eltOffOpnd = Create_Src_Opnd_From_Dcl(dcl, numElt == 1 ? getRegionScalar() : getRegionStride1());
        headerLess = true;
    }

    if( headerLess )
    {
        // header size = 2 * #elt
        G4_Declare *dcl = Create_MRF_Dcl( effectiveNumElt * 2, Type_UD );
        G4_DstRegRegion* tmpDstOpnd = Create_Dst_Opnd_From_Dcl( dcl, 1);
        if (eltSize == GATHER_SCATTER_WORD ||
            (eltSize != GATHER_SCATTER_BYTE && IsSLMSurface(surface))) 
        {
                // For non-SLM surface,
                // need a shift to make the offset to be byte offset
                // shl (esize) tmp.0<1>:ud elt_off<8;8,1>:ud 0x2:uw
                // Don't do this for Dword because we use the dword scatter message instead
                //
                // SLM surface has only BYTE scattered
                // read/write support. Always use BYTE scater.
                createInst( NULL, G4_shl, NULL, false, numElt, tmpDstOpnd, eltOffOpnd,
                    createImm( unsigned(eltSize), Type_UD ), instOpt );
                msgEltSize = GATHER_SCATTER_BYTE;
        }
        else
        {
            createInst( NULL, G4_mov, NULL, false, numElt, tmpDstOpnd, eltOffOpnd, NULL, instOpt);
        }

        Create_MOV_Send_Src_Inst( dcl, effectiveNumElt/8, 0, numElt, srcOpnd, instOpt );
        msgSrcOpnd = Create_Src_Opnd_From_Dcl( dcl, getRegionStride1());
    }
    else
    {
        // mov (8)      VX(0,0)<1>,  r0:ud
        // add dcl for VX
        G4_Declare *dcl = Create_MRF_Dcl( GENX_DATAPORT_IO_SZ + effectiveNumElt * 2, Type_UD );

        if (IsStatelessSurface(surface)) {
            // Build stateless surface message header.
            BuildStatelessSurfaceMessageHeader(this, dcl);
        } else {
            // Copy R0 header.
            Create_MOVR0_Inst(dcl, 0, 0, true);
        }

        G4_DstRegRegion dst1( Direct, dcl->getRegVar(), 1, 0, 1, dcl->getElemType() );
        G4_DstRegRegion* dst1_opnd = createDstRegRegion( dst1 );

        if (eltSize == GATHER_SCATTER_WORD || IsSLMSurface(surface)) 
        {
                // For non-SLM surface, WORD gather/scatter has no hardware
                // supportr and must be translated into BYTE gather/scatter.
                //
                // For SLM surface, gen9 devices has only BYTE gather/scatter
                // support and also needs translating into BYTE gather/scatter.
                //
                /* mov (1)     VX(0,2)<1>,   P  */
                if( gOffOpnd->isImm() )
                {
                    G4_Imm *new_src_opnd1 = createImm(
                        gOffOpnd->asImm()->getInt() * ( eltSize == GATHER_SCATTER_WORD ? 2 : 4 ),
                        gOffOpnd->getType() );
                    Create_MOV_Inst( dcl, 0, 2, 1, NULL, NULL, new_src_opnd1, true );
                }
                else
                {
                    G4_DstRegRegion dst2( Direct, dcl->getRegVar(), 0, 2, 1, dcl->getElemType() );
                    G4_DstRegRegion* dst2_opnd = createDstRegRegion( dst2 );

                    createInst( NULL, G4_shl, NULL, false, 1, dst2_opnd, gOffOpnd,
                        createImm( (unsigned)eltSize, Type_UD ), InstOpt_WriteEnable );
                }
                createInst( NULL, G4_shl, NULL, false, numElt, dst1_opnd, eltOffOpnd,
                    createImm( (unsigned)eltSize, Type_UD ), instOpt );
                msgEltSize = GATHER_SCATTER_BYTE;
        }
        else
        {
            /* mov (1)     VX(0,2)<1>,   P  */
            Create_MOV_Inst( dcl, 0, 2, 1, NULL, NULL, gOffOpnd, true );
            /* mov  (numElt)    VX(1,0)<1>,  E */
            createInst( NULL, G4_mov, NULL, false, numElt, dst1_opnd,
                eltOffOpnd, NULL, instOpt);
        }

        /* mov  (numElt)    VX(numElt/8+1,0)<1>,  V */
        Create_MOV_Send_Src_Inst( dcl, (effectiveNumElt/8+1), 0, numElt, srcOpnd, instOpt );

        // send's operands preparation
        // create a currDst for VX
        msgSrcOpnd = Create_Src_Opnd_From_Dcl( dcl, getRegionStride1());
    }

    unsigned temp = 0;

    // Set bit 9-8 for the message descriptor
    if (msgEltSize == GATHER_SCATTER_DWORD)
    {
        if (effectiveNumElt == 8)
        {
            temp += 2 << 8;
        }
        else {
            temp += 3 << 8;
        }
        temp += DC_DWORD_SCATTERED_WRITE << 14; 
    }
    else
    {
        if (effectiveNumElt == 16)
        {
            temp += 1 << 8;
        }
        temp += (unsigned char)eltSize << 10;
        temp += DC_BYTE_SCATTERED_WRITE << 14; 
    }

    G4_DstRegRegion *post_dst_opnd = createNullDst( effectiveNumElt > 8 ? Type_UW : Type_UD);

    G4_INST *send_inst = Create_Send_Inst_For_CISA(
        pred,
        post_dst_opnd,
        msgSrcOpnd,
        headerLess ? effectiveNumElt/GENX_DATAPORT_IO_SZ * 2 :
        effectiveNumElt/GENX_DATAPORT_IO_SZ * 2 + 1,
        0,
        numElt,
        temp,
        SFID_DP_DC,
        0,
        !headerLess,
        false,
        true,
        surface,
        NULL,
        instOpt,
        false );
    last_inst = send_inst;
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

/*
* Translates untyped surface read.
*
* For GT, assume N = 8 then the code should look like
*
* .declare  VX Base=m ElementSize=4 Type=ud Total=16
* .declare  VY Base=r ElementSize=4 Type=ud Total=8
*
* mov  (8)     VX(0,0)<1>,  r0:ud
* mov  (8)     VX(1,0)<1>,  P+E
* send (8)     VY(0,0)<1>,  VX(0,0),    0x5,  0x0418C200
*
* E: M1 in the message payload (Element offsets in BYTEs)
* 1010 (Target Function ID: Data Cache)
*
* 0x0418C200 == Bit 31-29: 000 (Reserved) 
*               Bit 28-25: 0010 (Msg. leng. = 2) 
*               Bit 24-20: 00001 (Response msg. leng. = 1)
*               Bit 19:    1 (Header present)
*               Bit 18:    0 (Ignored)
*               Bit 17-14: 1101 (Msg. type = untyped write - for data Cache)
*               Bit 13-12:  0010 (SIMD mode = 8 )
*               Bit 11-8:  0000 (masked channels )
*               Bit 7-0:   00000000 + I (Binding table index)
*
*/
int IR_Builder::translateVISAGather4Inst(
    Common_VISA_EMask_Ctrl emask,
    bool modified,
    ChannelMask chMask,
    Common_ISA_Exec_Size executionSize,
    G4_Operand* surface,
    G4_Operand* gOffOpnd,
    G4_SrcRegRegion* eltOffOpnd,
    G4_DstRegRegion* dstOpnd )
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    surface = lowerSurface255To253(surface, *this);

    uint8_t exsize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    unsigned int instOpt = Get_Gen4_Emask(emask, exsize);
    unsigned int num_channel = chMask.getNumEnabledChannels();

    uint8_t numElt = mapExecSizeToNumElts[executionSize];
    uint8_t hdrSize = 0;

    bool useSplitSend = useSends();

    G4_Declare *header = 0;
    G4_Declare *offset = Create_MRF_Dcl(numElt, Type_UD);

    if (surface && IsStatelessSurface(surface) && needsA32MsgHeader()) 
    {
        // Header is required to work around a HW issue on pre-SKL devices.
        hdrSize = GENX_DATAPORT_IO_SZ;
        if (useSplitSend) {
            header = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ, Type_UD);
        } else {
            header = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ + numElt, Type_UD);
            offset->setAliasDeclare(header, GENX_MRF_REG_SIZ);
        }
    } else {
        // When the surface is not stateless one, header is not used and therefore
        // split-send is not used.
        useSplitSend = false;
    }

    if (header) {
        // With 'header' allocated, we need prepare the header for the
        // (stateless) surface.
        ASSERT_USER(IsStatelessSurface(surface), "With 'header' allocated, stateless surface is expected!");
        // Build stateless surface message header.
        BuildUntypedStatelessSurfaceMessageHeader(this, header);
    }

    // convert to byte address
    // shl (esize) offset<1>:ud elt_off<8;8,1>:ud 2:uw
    G4_DstRegRegion* dst1_opnd = createDstRegRegion(Direct, offset->getRegVar(), 0, 0, 1, offset->getElemType());

    G4_Declare *tmp_dcl = createTempVar( numElt, Type_UD, Either, Sixteen_Word );
    G4_DstRegRegion dst3( Direct, tmp_dcl->getRegVar(), 0, 0, 1, tmp_dcl->getElemType() );
    G4_DstRegRegion* dst3_opnd = createDstRegRegion( dst3 );

    createInst( NULL, G4_shl, NULL, false, numElt, dst3_opnd, eltOffOpnd, createImm( 2, Type_UW ), instOpt );

    G4_SrcRegRegion* src2_opnd = createSrcRegRegion(Mod_src_undef, Direct, tmp_dcl->getRegVar(), 0, 0,
        getRegionStride1(), tmp_dcl->getElemType());

    // As untyped surface message use MH_IGNORE based header, if global offset
    // is non-zero, we need recalculate element offsets.
    if( gOffOpnd->isImm()  )
    {
        if( gOffOpnd->asImm()->getInt() != 0 )
        {
            gOffOpnd = createImm(
                gOffOpnd->asImm()->getInt() * 4,
                gOffOpnd->getType() );
            createInst( NULL, G4_add, NULL, false, numElt, dst1_opnd, src2_opnd, gOffOpnd, instOpt );
        }
        else
        {
            createInst( NULL, G4_mov, NULL, false, numElt, dst1_opnd, src2_opnd, NULL, instOpt );
        }
    }
    else
    {
        G4_Declare *tmp_dcl1 = createTempVar( 1, gOffOpnd->getType(), Either, Any );
        G4_DstRegRegion dst2( Direct, tmp_dcl1->getRegVar(), 0, 0, 1, tmp_dcl1->getElemType() );
        G4_DstRegRegion* dst2_opnd = createDstRegRegion( dst2 );

        createInst( NULL, G4_shl, NULL, false, 1, dst2_opnd, gOffOpnd, createImm( 2, Type_UW ), InstOpt_WriteEnable );

        G4_SrcRegRegion* src1Opnd = createSrcRegRegion(Mod_src_undef, Direct, tmp_dcl1->getRegVar(), 0, 0,
            getRegionScalar(), tmp_dcl1->getElemType() );

        createInst( NULL, G4_add, NULL, false, numElt, dst1_opnd, src2_opnd, src1Opnd, instOpt );
    }

    // send's operands preparation

    G4_DstRegRegion* d = Check_Send_Dst( dstOpnd->asDstRegRegion());

    unsigned temp = 0;

    // Set bit 13-12 for the message descriptor
    if(numElt == 8)
    {
        temp += 2 << 12;
    }
    else
    {
        temp += 1 << 12;
    }

    CISA_SHARED_FUNCTION_ID tf_id = SFID_DP_DC1;
    temp += DC1_UNTYPED_SURFACE_READ << 14;

    // bits 11-8: channel mask
    // HW defines 0 to mean the channel is on, so we have to flip it
    temp += chMask.getHWEncoding() << 8;

    if( surface == NULL )
    {
        temp |= 0xFE;
    }

    if (useSplitSend) {
        ASSERT_USER(header, "'header' should be allocated when split-send is to be used.");

        G4_SrcRegRegion *m0 = Create_Src_Opnd_From_Dcl(header, getRegionStride1());
        G4_SrcRegRegion *m1 = Create_Src_Opnd_From_Dcl(offset, getRegionStride1());
        last_inst = Create_SplitSend_Inst_For_CISA(NULL, d,
                                                   m0, 1, m1, numElt/GENX_DATAPORT_IO_SZ,
                                                   (numElt/GENX_DATAPORT_IO_SZ) * num_channel,
                                                   numElt, temp, 0, tf_id, false, hdrSize != 0,
                                                   true, false,
                                                   surface, NULL, instOpt, false);
    }
    else
    {
        G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl(header ? header : offset, getRegionStride1());
        last_inst = Create_Send_Inst_For_CISA(
            NULL,
            d,
            payload,
            (hdrSize + numElt)/GENX_DATAPORT_IO_SZ,
            (numElt/GENX_DATAPORT_IO_SZ) * num_channel,
            numElt,
            temp,
            tf_id,
            0,
            hdrSize != 0,
            true,
            false,
            surface,
            NULL,
            instOpt,
            false );
    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

/*
* Translates untyped surface write intrinsic.
*
* For GT, assume N = 8 then the code should look like
*
* .declare  VX Base=m ElementSize=4 Type=ud Total=24
*
* mov  (8)     VX(0,0)<1>,  r0:ud
* mov  (8)     VX(1,0)<1>,  E + P
* mov  (8)     VX(2,0)<1>,  V
* send (8)     null<1>,     VX(0,0),    0x5,  0x06096200
*
* E: M1 in the message payload (Element offsets)
* v: M2 in the message payload (written data)
*
* 1010 (Target Function ID: DP Data Cache)
*
* 0x06096200 == Bit 31-29: 000 (Reserved)
*               Bit 28-25: 0011 (Msg. leng. = 3) 
*               Bit 24-20: 00000 (Response msg. leng. = 0)
*               Bit 19:    1 (Header present)
*               Bit 18:    0 (Ignored)
*               Bit 17-14: 1101 (Msg. type = untyped write - for data Cache) 
*               Bit 13-12:  0010 (SIMD mode = 8 ) 
*                  Bit 11-8:  0000 (masked channels )
*               Bit 7-0:   00000000 + I (Binding table index) 
*
*/
int IR_Builder::translateVISAScatter4Inst(
    Common_VISA_EMask_Ctrl emask,
    ChannelMask chMask,
    Common_ISA_Exec_Size executionSize,
    G4_Operand* surface,
    G4_Operand* gOffOpnd,
    G4_SrcRegRegion* eltOffOpnd,
    G4_SrcRegRegion* srcOpnd )
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    surface = lowerSurface255To253(surface, *this);

    uint8_t exsize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    unsigned int instOpt = Get_Gen4_Emask(emask, exsize);

    unsigned int num_channel = chMask.getNumEnabledChannels();

    uint8_t numElt = mapExecSizeToNumElts[executionSize];
    uint8_t hdrSize = 0;

    unsigned int data_size = numElt * num_channel;
    G4_Declare *src_dcl = srcOpnd->asSrcRegRegion()->getBase()->asRegVar()->getDeclare();

    int payload_size = numElt + data_size;

    bool useSplitSend = useSends();

    G4_Declare *header = 0;
    G4_Declare *offset = 0;
    G4_Declare *data = Create_MRF_Dcl(data_size, Type_UD);

    if (surface && IsStatelessSurface(surface) && needsA32MsgHeader())
    {
        // Header is required to work around a HW issue on pre-SKL devices.
        hdrSize = GENX_DATAPORT_IO_SZ;
        offset = Create_MRF_Dcl(numElt, Type_UD);
        if (useSplitSend) {
            // When header is required, we split the message as
            // (header, offset + data) if split-send is supported.
            header = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ, Type_UD);
            offset = Create_MRF_Dcl(payload_size, Type_UD);
            data->setAliasDeclare(offset, (numElt/8) * GENX_MRF_REG_SIZ);
        } else {
            header = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ + payload_size, Type_UD);
            offset->setAliasDeclare(header, GENX_MRF_REG_SIZ);
            data->setAliasDeclare(header, GENX_MRF_REG_SIZ * ((numElt/8) + 1));
        }
    } else {
        if (useSplitSend) {
            // When header is not required, we split the message as (offset, data)
            // if split-send is supported.
            offset = Create_MRF_Dcl(numElt, Type_UD);
        } else {
            offset = Create_MRF_Dcl(payload_size, Type_UD);
            data->setAliasDeclare(offset, (numElt/8) * GENX_MRF_REG_SIZ);
        }
    }

    if (header) {
        // With 'header' allocated, we need prepare the header for the
        // (stateless) surface.
        ASSERT_USER(IsStatelessSurface(surface), "With 'header' allocated, stateless surface is expected!");
        // Build stateless surface message header.
        BuildUntypedStatelessSurfaceMessageHeader(this, header);
    }

    if ( !header && useSplitSend )
    {
        data = src_dcl;
    } else
    {
        // Copy data from src operand.
        for (unsigned i = 0; i != num_channel; ++i)
        {
            G4_SrcRegRegion *s2_opnd = createSrcRegRegion(Mod_src_undef, Direct, src_dcl->getRegVar(), (i * numElt) / 8, 0, getRegionStride1(), src_dcl->getElemType());
            Create_MOV_Send_Src_Inst(data, (i * numElt) / 8, 0, numElt, s2_opnd, instOpt);
        }
    }

    // mov  VX(0,0)<1>, r0
    // Create_MOVR0_Inst( header, 0, 0, true );

    G4_DstRegRegion* dst1_opnd = createDstRegRegion(Direct, offset->getRegVar(), 0, 0, 1, offset->getElemType());

    G4_Declare *tmp_dcl = createTempVar( numElt, Type_UD, Either, Sixteen_Word );
    G4_DstRegRegion dst3( Direct, tmp_dcl->getRegVar(), 0, 0, 1, tmp_dcl->getElemType() );
    G4_DstRegRegion* dst3_opnd = createDstRegRegion( dst3 );

    createInst( NULL, G4_shl, NULL, false, numElt, dst3_opnd, eltOffOpnd, createImm( 2, Type_UW ), instOpt );

    G4_SrcRegRegion* src2_opnd = createSrcRegRegion(Mod_src_undef, Direct, tmp_dcl->getRegVar(), 0, 0, getRegionStride1(), tmp_dcl->getElemType() );

    if( gOffOpnd->isImm()  )
    {
        if( gOffOpnd->asImm()->getInt() != 0 )
        {
            gOffOpnd = createImm(
                gOffOpnd->asImm()->getInt() * 4,
                gOffOpnd->getType() );
            createInst( NULL, G4_add, NULL, false, numElt, dst1_opnd, src2_opnd, gOffOpnd, instOpt );
        }
        else
        {
            createInst( NULL, G4_mov, NULL, false, numElt, dst1_opnd, src2_opnd, NULL, instOpt );
        }
    }
    else
    {
        G4_Declare *tmp_dcl1 = createTempVar( 1, gOffOpnd->getType(), Either, Any );
        G4_DstRegRegion dst2( Direct, tmp_dcl1->getRegVar(), 0, 0, 1, tmp_dcl1->getElemType() );
        G4_DstRegRegion* dst2_opnd = createDstRegRegion( dst2 );

        createInst( NULL, G4_shl, NULL, false, 1, dst2_opnd, gOffOpnd, createImm( 2, Type_UW ), InstOpt_WriteEnable );

        G4_SrcRegRegion* src1Opnd = createSrcRegRegion(Mod_src_undef, Direct, tmp_dcl1->getRegVar(), 0, 0,
            getRegionScalar(), tmp_dcl1->getElemType() );

        createInst( NULL, G4_add, NULL, false, numElt, dst1_opnd, src2_opnd, src1Opnd, instOpt );
    }

    // send's operands preparation
    unsigned temp = 0;

    // Set bit 13-12 for the message descriptor
    if(numElt == 8) {
        temp += 2 << 12;
    } else {
        temp += 1 << 12;
    }

    CISA_SHARED_FUNCTION_ID tf_id = SFID_DP_DC1;
    temp += DC1_UNTYPED_SURFACE_WRITE << 14;
    // bits 11-8: channel mask
    temp += chMask.getHWEncoding() << 8;

    // Set bit 9-8 for the message descriptor

    if( surface == NULL )
    {
        temp |= 0xFF - 1;
    }

    G4_DstRegRegion *post_dst_opnd = createNullDst(numElt > 8 ? Type_UW : Type_UD );

    if (useSplitSend) {
        G4_SrcRegRegion *m0 = 0; unsigned m0Len = 0;
        G4_SrcRegRegion *m1 = 0; unsigned m1Len = 0;
        if (header) {
            m0 = Create_Src_Opnd_From_Dcl(header, getRegionStride1());
            m0Len = 1;
            m1 = Create_Src_Opnd_From_Dcl(offset, getRegionStride1());
            m1Len = payload_size / GENX_DATAPORT_IO_SZ;
        } else {
            m0 = Create_Src_Opnd_From_Dcl(offset, getRegionStride1());
            m0Len = numElt / GENX_DATAPORT_IO_SZ;
            m1 = Create_Src_Opnd_From_Dcl(data, getRegionStride1());
            m1Len = data_size / GENX_DATAPORT_IO_SZ;
        }
        last_inst = Create_SplitSend_Inst_For_CISA(NULL, post_dst_opnd,
                                                   m0, m0Len, m1, m1Len, 0,
                                                   numElt,
                                                   temp, 0, tf_id, false, hdrSize != 0,
                                                   false, true,
                                                   surface, NULL,
                                                   instOpt, false);
    }
    else
    {
        G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl(header ? header : offset, getRegionStride1());
        last_inst = Create_Send_Inst_For_CISA(
            NULL,
            post_dst_opnd,
            payload,
            (numElt * (num_channel + 1) + hdrSize)/GENX_DATAPORT_IO_SZ,
            0,
            numElt,
            temp,
            tf_id,
            0,
            hdrSize != 0,
            false,
            true,
            surface,
            NULL,
            instOpt,
            false );
    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

/*
* Translates DWord atomic write intrinsic.
*
* write(I, OP, P, vector<uint, 8> E, vector<T, N> S, vector<T, 8> &V)
*
* For GT, assume OP = ATOMIC_ADD, N = 8 then the code should look like
*
* .declare  VX Base=m ElementSize=4 Type=ud Total=24
*
* mov  (8)     VX(0,0)<1>,  r0:ud
* mov  (1)     VX(0,2)<1>,  P  // if headerless == false
* mov  (8)     VX(1,0)<1>,  E
* mov  (8)     VX(2,0)<1>,  S0
* mov  (8)     VX(3,0)<1>,  S1         // if necessary
* send (8)     V(0,0)<1>,   VX(0,0),    0x5,  0x0618E000
*
* P: M0.2 in the message header (Global offset) 
* E: M1 in the message payload (Element offsets) 
* v: M2 in the message payload (written data) 
* m0 -- message header
* m1 -- offsets
* m2, m3 -- source (if applicable)
* 0x5 == 0 (Not the EOT)
*        0101 (Target Function ID: DP Render Cache)
*
* 0x0618E000 == Bit 31-29: 000 (Reserved) 
*               Bit 28-25: 0011 (Msg. leng. = 3)
*               Bit 24-20: 00001 (Response msg. leng. = 1) 
*               Bit 19:    1 (Header present) 
*               Bit 18:    0 (Ignored)
*               Bit 17:    0 (Send write commit message)
*               Bit 16-13: 0111 (Msg. type = DWord atomic write)
*               Bit 12-8:  00000 (OP = ATOMIC_ADD) 
*               Bit 7-0:   00000000 + I (Binding table index) 
*
*/
int IR_Builder::translateVISADwordAtomicInst(
    CMAtomicOperations atomicOp,
    bool is16Bit,
    Common_VISA_EMask_Ctrl emask,
    Common_ISA_Exec_Size executionSize,
    G4_Operand* surface,
    G4_Operand* gOffOpnd,
    G4_SrcRegRegion* eltOffOpnd,
    G4_SrcRegRegion* src0Opnd,
    G4_SrcRegRegion* src1Opnd,
    G4_DstRegRegion* dstOpnd)

{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    surface = lowerSurface255To253(surface, *this);

    unsigned op = Get_Atomic_Op(atomicOp);
    unsigned int instOpt = Get_Gen4_Emask(emask, (uint8_t) Get_Common_ISA_Exec_Size(executionSize));
    // add dcl for VX
    bool header_present;
    unsigned int mrf_size;
    int num_sources = 0;

    uint8_t numElt = mapExecSizeToNumElts[executionSize];

    if( src0Opnd && !src0Opnd->isNullReg() )
    {
        num_sources++;
    }

    if( src1Opnd && !src1Opnd->isNullReg() )
    {
        num_sources++;
    }
    //m0 -- message header
    //m1 -- offsets
    //m2, m3 -- source (if applicable)
    //headerless msg for untyped atomic op
    header_present = false;
    mrf_size = (num_sources + 1) * (numElt == 16 ? 2 : 1) * GENX_DATAPORT_IO_SZ;

    bool useSplitSend = useSends();

    G4_Declare *header = 0;
    G4_Declare *offset = 0;
    G4_Declare *data = 0;

    // When no data source is specified, SplitSend should not be used as only
    // 'offset' need packing 
    if (num_sources) {
        data = Create_MRF_Dcl(num_sources * (numElt == 16 ? 2 : 1) * GENX_DATAPORT_IO_SZ, Type_UD);
    } else {
        useSplitSend = false;
    }

    if (useSplitSend) {
        // Message is split as (offset, data).
        ASSERT_USER(!header_present, "Header should not be present when split-send is available!");
        offset = Create_MRF_Dcl(mrf_size, Type_UD);
        if (data) {
            data->setAliasDeclare(offset, GENX_MRF_REG_SIZ * (numElt == 16 ? 2 : 1));
        }
    } else {
        offset = Create_MRF_Dcl(mrf_size - (header_present ? GENX_DATAPORT_IO_SZ : 0), Type_UD);
        if (header_present) {
            header = Create_MRF_Dcl(mrf_size, Type_UD);
            offset->setAliasDeclare(header, GENX_MRF_REG_SIZ);
            if (data) {
                data->setAliasDeclare(header, (numElt == 16 ? 2 : 1) * GENX_MRF_REG_SIZ + GENX_MRF_REG_SIZ);
            }
        } else {
            if (data) {
                data->setAliasDeclare(offset, (numElt == 16 ? 2 : 1) * GENX_MRF_REG_SIZ);
            }
        }
    }

    /* mov  (numElt)    VX(1,0)<1>,  E */
    if( header_present )
    {
        ASSERT_USER(header, "'header' is not allocated when message header is present!");
        Create_MOV_Send_Src_Inst(header, 1, 0, numElt, eltOffOpnd, instOpt);
    }
    else
    {
        //use byte offset for untyped atomic operations, also add the global offset to it
        /* add  (numElt)     VZ(0,0)<1>, E, P */
        G4_Declare *tmp_dcl = createTempVar( numElt, Type_UD, Either, Any );
        G4_DstRegRegion* tmp_dst_opnd = Create_Dst_Opnd_From_Dcl(tmp_dcl, 1);

        createInst(
            NULL,
            G4_add,
            NULL,
            false,
            numElt,
            tmp_dst_opnd,
            eltOffOpnd,
            gOffOpnd,
            instOpt,
            0 );

        // DW offset should be used
        // shl  (numElt)     VZ(0,0)<1>, VZ(0,0), 2
        G4_SrcRegRegion* tmp_src_opnd = createSrcRegRegion(
            Mod_src_undef,
            Direct,
            tmp_dcl->getRegVar(),
            0,
            0,
            getRegionStride1(),
            Type_UD );

        G4_DstRegRegion* second_tmp_dst_opnd = Create_Dst_Opnd_From_Dcl(tmp_dcl, 1);

        createInst(
            NULL,
            G4_shl,
            NULL,
            false,
            numElt,
            second_tmp_dst_opnd,
            tmp_src_opnd,
            createImm( 2, Type_UD ),
            instOpt,
            0 );
        Create_MOV_Send_Src_Inst(offset, 0, 0, numElt, tmp_src_opnd, instOpt);
    }

    /* mov  (numElt)    VX(numElt/8+1,0)<1>,  s0 */
    if( src0Opnd && !src0Opnd->isNullReg() ){
        if( !header_present ){
            // we need to swap source 0 and source 1
            if (atomicOp == ATOMIC_CMPXCHG)
            {
                Create_MOV_Send_Src_Inst( data, 0, 0, numElt, src1Opnd, instOpt );
            }
            else
            {
                Create_MOV_Send_Src_Inst( data, 0, 0, numElt, src0Opnd, instOpt );
            }
        }else{
            Create_MOV_Send_Src_Inst( data, 0, 0, numElt, src0Opnd, instOpt );
        }
    }
    /* mov  (numElt)    VX(numElt/4+1,0)<1>,  s1 */
    if( src1Opnd && !src1Opnd->isNullReg() )
    {
        if (!header_present)
        {
            if (atomicOp == ATOMIC_CMPXCHG)
            {
                Create_MOV_Send_Src_Inst( data, (numElt == 16 ? 2 : 1), 0, numElt, src0Opnd, instOpt );
            }
            else
            {
                Create_MOV_Send_Src_Inst( data, (numElt == 16 ? 2 : 1), 0, numElt, src1Opnd, instOpt );
            }
        }else{
            Create_MOV_Send_Src_Inst( data, (numElt == 16 ? 2 : 1), 0, numElt, src1Opnd, instOpt );
        }
    }

    // mov  VX(0,0)<1>, r0
    // mov  VX(0,2)<1>, P
    if( header_present )
    {
        ASSERT_USER(header, "'header' is not allocated when message header is present!");
        Create_MOVR0_Inst(header, 0, 0);
        Create_MOV_Inst(header, 0, 2, 1, NULL, NULL, gOffOpnd);
    }

    // send's operands preparation
    int dstLength = dstOpnd->isNullReg() ? 0 : numElt/GENX_DATAPORT_IO_SZ;
    unsigned msgDesc = op << MESSAGE_SPECIFIC_CONTROL;

    if (dstLength > 0)
    {
        msgDesc |= 1 << 13;
    }

    // bit12: simd16 -- 0, simd8 -- 1
    msgDesc |= (numElt == 16 ? 0 : 1) << 12;

    if (is16Bit)
    {
        SET_DATAPORT_MESSAGE_TYPE(msgDesc, DC1_UNTYPED_HALF_INTEGER_ATOMIC);
    }
    else
    {
        SET_DATAPORT_MESSAGE_TYPE(msgDesc, DC1_UNTYPED_ATOMIC);
    }

    CISA_SHARED_FUNCTION_ID tf_id = SFID_DP_DC1;

    int msgLength = mrf_size/GENX_DATAPORT_IO_SZ;

    if (useSplitSend) {
        G4_SrcRegRegion *m0 = Create_Src_Opnd_From_Dcl(offset, getRegionStride1());
        G4_SrcRegRegion *m1 = Create_Src_Opnd_From_Dcl(data, getRegionStride1());
        last_inst = Create_SplitSend_Inst_For_CISA(NULL, dstOpnd,
                                                   m0, (numElt == 16 ? 2 : 1),
                                                   m1, (numElt == 16 ? 2 : 1) * num_sources,
                                                   dstLength,
                                                   numElt, msgDesc, 0, tf_id,
                                                   false, header_present,
                                                   true, true,
                                                   surface, NULL, instOpt, false);
    } else {
        G4_SrcRegRegion* payloadOpnd = Create_Src_Opnd_From_Dcl((header_present ? header : offset), getRegionStride1());
        last_inst = Create_Send_Inst_For_CISA(
            NULL,
            dstOpnd,
            payloadOpnd,
            msgLength,
            dstLength,
            numElt,
            msgDesc,
            tf_id,
            0,
            (header_present?1:0),
            true,
            true,
            surface,
            NULL,
            instOpt,
            false);
    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

static void
BuildMH1_A32_PSM(IR_Builder *IRB, G4_Declare *header) {
    // Clear header. Ignore PSM so far.
    G4_DstRegRegion *h = IRB->createDstRegRegion(Direct, header->getRegVar(),
        0, 0, 1, Type_UD);
    IRB->createInst(NULL, G4_mov, NULL, false, 8, h,
        IRB->createImm(0, Type_UD), NULL, InstOpt_WriteEnable);
    // Set PSM to all 1s.
    G4_DstRegRegion *h0_7 =
        IRB->createDstRegRegion(Direct, header->getRegVar(), 0, 7, 1, Type_UD);
    G4_Imm *Mask = IRB->createImm(0xFFFF, Type_UD);
    IRB->createInst(NULL, G4_mov, NULL, false, 1, h0_7, Mask, NULL,
        InstOpt_WriteEnable);
}

static bool IsFloatAtomicOps(CMAtomicOperations op) {
    return (op == ATOMIC_FMAX || op == ATOMIC_FMIN || op == ATOMIC_FCMPWR);
}

// This version takes byte offsets and predicates
int IR_Builder::translateVISADwordAtomicInst(CMAtomicOperations atomicOp,
                                             bool is16Bit,
                                             G4_Predicate *pred,
                                             Common_ISA_Exec_Size execSize,
                                             Common_VISA_EMask_Ctrl eMask,
                                             G4_Operand* surface,
                                             G4_SrcRegRegion* offsets,
                                             G4_SrcRegRegion* src0,
                                             G4_SrcRegRegion* src1,
                                             G4_DstRegRegion* dst) {
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    ASSERT_USER(!IsFloatAtomicOps(atomicOp) || hasFloatAtomics(),
                "Float atomic operations are only supported on SKL+ devices!");

    surface = lowerSurface255To253(surface, *this);

    Common_ISA_Exec_Size instExecSize = execSize;
    if (execSize == EXEC_SIZE_1 || execSize == EXEC_SIZE_2 || execSize == EXEC_SIZE_4)
    {
        execSize = EXEC_SIZE_8;
    }

    // always 8 or 16
    unsigned exSize = Get_Common_ISA_Exec_Size(execSize);
    // can be 1 for scalar atomics
    unsigned instExSize = Get_Common_ISA_Exec_Size(instExecSize);
    unsigned instOpt = Get_Gen4_Emask(eMask, instExSize);
    unsigned subOpc = Get_Atomic_Op(atomicOp);


    bool useSplitSend = useSends();
    bool hasRet = !dst->isNullReg();

    if (atomicOp == ATOMIC_CMPXCHG)
    {
        std::swap(src0, src1);
    }

    payloadSource sources[4]; // optional header + offsets + [src0] + [src1]
    unsigned len = 0;

    bool useHeader = needsA32MsgHeader() && surface && IsStatelessSurface(surface);
    if (useHeader) {
        G4_Declare *dcl = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ, Type_UD);

        BuildMH1_A32_PSM(this, dcl);

        G4_SrcRegRegion *header
            = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
        sources[len].opnd = header;
        sources[len].execSize = 8;
        sources[len].instOpt = InstOpt_WriteEnable;
        ++len;
    }

    sources[len].opnd = offsets;
    sources[len].execSize = exSize;
    sources[len].instOpt = instOpt;
    ++len;

    if (src0 && !src0->isNullReg()) {
        sources[len].opnd = src0;
        sources[len].execSize = exSize;
        sources[len].instOpt = instOpt;
        ++len;
    }

    if (src1 && !src1->isNullReg()) {
        sources[len].opnd = src1;
        sources[len].execSize = exSize;
        sources[len].instOpt = instOpt;
        ++len;
    }

    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, exSize, useSplitSend, sources, len);

    CISA_SHARED_FUNCTION_ID SFID = SFID_DP_DC1;
    unsigned MD = 0;
    bool IsFloatOp = IsFloatAtomicOps(atomicOp);

    // Bit 12 specifies the SIMD mode.
    MD |= (execSize == EXEC_SIZE_8 ? MDC_SM2R_SIMD8 : MDC_SM2R_SIMD16) << 12;
    if (is16Bit)
    {
        MD |= (IsFloatOp ? static_cast<unsigned>(DC1_UNTYPED_HALF_FLOAT_ATOMIC)
                         : static_cast<unsigned>(DC1_UNTYPED_HALF_INTEGER_ATOMIC))
            << 14;
    }
    else
    {
        MD |= (IsFloatOp ? static_cast<unsigned>(DC1_UNTYPED_FLOAT_ATOMIC)
                         : static_cast<unsigned>(DC1_UNTYPED_ATOMIC))
              << 14;
    }
    MD |= (hasRet ? 1 : 0) << 13;
    MD |= subOpc << 8;

    unsigned resLen = hasRet ? (exSize / GENX_DATAPORT_IO_SZ) : 0;
    bool forceSplitSend = IsBindlessSurface(*this, surface);
    if (msgs[1] == 0 && !forceSplitSend) {
        ASSERT_USER(sizes[1] == 0, "Expect the 2nd part of the payload has zero size!");
        last_inst = Create_Send_Inst_For_CISA(pred, dst,
                                              msgs[0], sizes[0],
                                              resLen,
                                              instExSize,
                                              MD, SFID,
                                              false, useHeader,
                                              true, false,
                                              surface, NULL, 
                                              instOpt, false);
    } else {
        last_inst = Create_SplitSend_Inst_For_CISA(pred, dst,
                                                   msgs[0], sizes[0], msgs[1], sizes[1],
                                                   resLen,
                                                   instExSize,
                                                   MD, 0, SFID,
                                                   false, useHeader,
                                                   true, false,
                                                   surface, NULL,
                                                   instOpt, false);
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

/*
* Translates Media Block read CISA inst.
*
* read_transpose(I, Height, Width, X, Y, matrix<int,C,R> M)
* Assume C = R = 8 then code shoud look like
*
* .declare  VX Base=m ElementSize=4 Type=ud Total=8
* .declare  VY Base=r ElementSize=4 Type=ud Total=8
*
* mov  (8)     VX(0,0)<1>,  r0.0:ud
* mov  (1)     VX(0,2)<1>,  1:0 Width 21:20 Height
* mov  (1)     VX(0,1)<1>,  Y
* mov  (1)     VX(0,0)<1>,  X
* send (8)     VY(0,0)<1>,  VX(0,0),    null,  0x04186000
* mov  (8)     M(0,0)<1>,   VY(0,0)
*
* 0x0007001f == (R-1)<<16 + C * sizeof(el_type) - 1;
*
* 0x04186000 ==
*  (((ObjectSize - 1) / GENX_GRF_REG_SIZ + 1)) << 16 +
*          0x4100000 + 0x6000 + I;
*
* ObjectSize = RoundUpPow2( C ) * R * sizeof(el_type);
*/
int IR_Builder::translateTransposeVISALoadInst(
    G4_Operand* surface,
    unsigned blockWidth,
    unsigned blockHeight,
    G4_Operand* xOffOpnd,
    G4_Operand* yOffOpnd,
    G4_DstRegRegion* dstOpnd )
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    unsigned temp;
    unsigned objWidth = this->getObjWidth(blockWidth, blockHeight, dstOpnd->getBase()->asRegVar()->getDeclare());
    int num_grf = Transpose_Read_Block_size[blockWidth];
    uint8_t num_elements_in_grf = (uint8_t) Transpose_Read_Block_size[blockHeight];
    //block width is the height of the matrix passed in.
    unsigned obj_size = objWidth * num_grf;

    /* mov (8)      VX(0,0)<1>,  r0:ud  */
    // add dcl for VX
    G4_Declare *dcl = Create_MRF_Dcl( GENX_DATAPORT_IO_SZ, Type_UD );

    // create MOV inst
    Create_MOVR0_Inst( dcl, 0, 0, true );
    /* mov (1)      VX(0,2)<1>,    CONST[R,C]  */
    temp = (blockHeight << 20) | blockWidth;
    Create_MOV_Inst( dcl, 0, 2, 1, NULL, NULL, createImm( temp, Type_UD ), true );

    if (xOffOpnd->isImm())
    {
        /* mov (1)     VX(0,0)<1>,    X  */
        Create_MOV_Inst( dcl, 0, 0, 1, NULL, NULL, xOffOpnd, true );
    }else
    {

        G4_DstRegRegion dst2(
            Direct,
            dcl->getRegVar(),
            0,
            0, //offset
            1,
            dcl->getElemType() );
        G4_DstRegRegion* dst2_opnd = createDstRegRegion( dst2 );

        createInst(
            NULL,
            G4_shl,
            NULL,
            false,
            1,
            dst2_opnd,
            xOffOpnd,
            createImm( blockWidth+2, Type_UD ),
            InstOpt_WriteEnable,
            0 );
    }

    if (yOffOpnd->isImm())
    {
        /* mov (1)     VX(0,1)<1>,   Y  */
        Create_MOV_Inst( dcl, 0, 1, 1, NULL, NULL, yOffOpnd, true );
    }else
    {
        G4_DstRegRegion dst2(
            Direct,
            dcl->getRegVar(),
            0,
            1, //offset
            1,
            dcl->getElemType() );
        G4_DstRegRegion* dst2_opnd = createDstRegRegion( dst2 );

        createInst(
            NULL,
            G4_shl,
            NULL,
            false,
            1,
            dst2_opnd,
            yOffOpnd,
            createImm( blockHeight, Type_UD ),
            InstOpt_WriteEnable,
            0 );
    }

    // send's operands preparation
    // create a currDst for VX
    G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl( dcl, getRegionStride1());

    //mediaread overwrites entire GRF
    bool via_temp = false;
    G4_Operand *original_dst = NULL;
    G4_Declare *new_dcl = NULL;
    //mabe later once packing is enabled.

    if( Transpose_Read_Block_size[blockHeight] < 8 )
    {
        via_temp = true;

        original_dst = dstOpnd;
        new_dcl = createTempVar( 8 * Transpose_Read_Block_size[blockWidth],
            Type_UD, Either, Sixteen_Word );
        G4_DstRegRegion tmp_dst( Direct,
            new_dcl->getRegVar(),
            0,
            0,
            1,
            Type_UD);
        tmp_dst.setRightBound( GENX_GRF_REG_SIZ - 1 );
        tmp_dst.setBitVecL( 0xffffffff );
        G4_DstRegRegion *tmp_dst_opnd = createDstRegRegion( tmp_dst );
        dstOpnd = tmp_dst_opnd;
    }

    G4_DstRegRegion* d = Check_Send_Dst( dstOpnd->asDstRegRegion());

    temp = 0;
    SET_DATAPORT_MESSAGE_TYPE(temp, DC_OWORD_BLOCK_READ)

        unsigned send_exec_size = GENX_DATAPORT_IO_SZ;
    if( d->getType() == Type_W || d->getType() == Type_UW ){
        send_exec_size *= 2;
    }

    G4_INST *send_inst = Create_Send_Inst_For_CISA(
        NULL,
        d,
        payload,
        1,
        Transpose_Read_Block_size[blockWidth],
        send_exec_size,
        temp,
        SFID_DP_DC1,
        0,
        1,
        true,
        false,
        surface,
        NULL,
        InstOpt_WriteEnable,
        false );
    last_inst = send_inst;


    if( via_temp )
    {
        G4_Declare *new_dcl2 = createTempVar(
            GENX_GRF_REG_SIZ/G4_Type_Table[original_dst->getType()].byteSize,
            original_dst->getType(), Either, Sixteen_Word );

        new_dcl2->setAliasDeclare( new_dcl, 0 );

        unsigned short width, remained_ele = obj_size / G4_Type_Table[original_dst->getType()].byteSize, vs, hs;
        // max execution size is 32
        unsigned char curr_exec_size = num_elements_in_grf * 2;
        unsigned char curr_offset = 0;
        unsigned char curr_src_offset = 0;
        short dst_regoff = original_dst->asDstRegRegion()->getRegOff();
        short dst_subregoff = original_dst->asDstRegRegion()->getSubRegOff();
        G4_Type dstType = original_dst->getType();

        if(num_grf == 1)
        {
            curr_exec_size = num_elements_in_grf;
            width = num_elements_in_grf;
            hs = 1;
            vs = num_elements_in_grf;
        }else
        {
            width = num_elements_in_grf;
            hs = 1;
            vs = 8;
        }
        for(int num_iter = 0; num_iter < Transpose_Read_Block_size[blockWidth]; num_iter+=2 )
        {
            G4_SrcRegRegion *tmp_src_opnd = createSrcRegRegion(
                Mod_src_undef,
                Direct,
                new_dcl2->getRegVar(),
                0,
                curr_src_offset,
                createRegionDesc( vs, width, hs ),
                original_dst->getType() );

            short ele_per_grf = G4_GRF_REG_NBYTES/G4_Type_Table[dstType].byteSize;
            dst_subregoff = curr_offset - (dst_regoff * ele_per_grf);

            if( dst_subregoff >= ele_per_grf )
            {
                dst_regoff += 1;
                dst_subregoff = 0;
            }
            G4_DstRegRegion tmp_dst(
                Direct,
                original_dst->asDstRegRegion()->getBase(),
                dst_regoff,
                dst_subregoff,
                1,
                original_dst->getType() );
            G4_DstRegRegion *tmp_dst_opnd = createDstRegRegion( tmp_dst );
            last_inst = createInst( NULL, G4_mov, NULL, false, curr_exec_size, tmp_dst_opnd, tmp_src_opnd, NULL, InstOpt_WriteEnable );
            curr_offset += curr_exec_size;
            curr_src_offset += 16;
            remained_ele -= curr_exec_size;
        }
    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

static void
BuildMH1_BTS_PSM(IR_Builder *IRB, G4_Declare *header) {
    // Clear header
    G4_DstRegRegion *h = IRB->createDstRegRegion(Direct, header->getRegVar(),
                                                 0, 0, 1, Type_UD);
    IRB->createInst(NULL, G4_mov, NULL, false, 8, h,
                    IRB->createImm(0, Type_UD), NULL, InstOpt_WriteEnable);
    // Set PSM to 0xFFFF so far.
    G4_Operand *maskImm = IRB->createImm(0xFFFF, Type_UD);
    G4_DstRegRegion *pitchDst = IRB->createDstRegRegion(Direct,
                                                        header->getRegVar(),
                                                        0, 7, 1, Type_UD);
    IRB->createInst(NULL, G4_mov, NULL, false, 1, pitchDst,
                    maskImm, NULL, InstOpt_WriteEnable);
}

// build the address payload for typed messages (read/write/atomic)
// sources stores the address payload, and its length len is also updated
void IR_Builder::buildTypedSurfaceAddressPayload(
    G4_SrcRegRegion* uOffsetOpnd,
    G4_SrcRegRegion* vOffsetOpnd,
    G4_SrcRegRegion* rOffsetOpnd,
    G4_SrcRegRegion* lodOpnd,
    uint32_t exSize,
    uint32_t instOpt,
    payloadSource sources[],
    uint32_t& len)
{
    // Valid address payload pattern are listed below:
    // (* means the parameter is ignored by HW but must be included in payload)
    // U
    // U, V
    // U, V, R
    // U, *, *, LOD
    // U, V, *, LOD
    // U, V, R, LOD

    // Append U
    sources[len].opnd = uOffsetOpnd;
    sources[len].execSize = exSize;
    sources[len].instOpt = instOpt;
    ++len;

    // Append V if any.
    if (!vOffsetOpnd->isNullReg()) {
        sources[len].opnd = vOffsetOpnd;
        sources[len].execSize = exSize;
        sources[len].instOpt = instOpt;
        ++len;
    }
    else if (!lodOpnd->isNullReg()) {
        G4_SrcRegRegion *nullVOffset = createNullSrc(Type_UD);
        sources[len].opnd = nullVOffset;
        sources[len].execSize = exSize;
        sources[len].instOpt = instOpt;
        ++len;
    }

    // Append R if any.
    if (!rOffsetOpnd->isNullReg()) {
        ASSERT_USER(!vOffsetOpnd->isNullReg(),
            "r offset must be NULL if v offset is NULL");
        sources[len].opnd = rOffsetOpnd;
        sources[len].execSize = exSize;
        sources[len].instOpt = instOpt;
        ++len;
    }
    else if (!lodOpnd->isNullReg()) {
        G4_SrcRegRegion *nullROffset = createNullSrc(Type_UD);
        sources[len].opnd = nullROffset;
        sources[len].execSize = exSize;
        sources[len].instOpt = instOpt;
        ++len;
    }

    // Append LOD if any.
    if (!lodOpnd->isNullReg()) {
        sources[len].opnd = lodOpnd;
        sources[len].execSize = exSize;
        sources[len].instOpt = instOpt;
        ++len;
    }
}

// u must not be V0. v and r are allowed to be V0, in which case they will be
// skipped in payload.
int IR_Builder::translateVISAGather4TypedInst(G4_Predicate           *pred,
                                              Common_VISA_EMask_Ctrl emask,
                                              ChannelMask chMask,
                                              G4_Operand *surface,
                                              Common_ISA_Exec_Size executionSize,
                                              G4_SrcRegRegion *uOffsetOpnd,
                                              G4_SrcRegRegion *vOffsetOpnd,
                                              G4_SrcRegRegion *rOffsetOpnd,
                                              G4_SrcRegRegion *lodOpnd,
                                              G4_DstRegRegion *dstOpnd)
{

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    unsigned exSize = executionSize == EXEC_SIZE_16 ? 16 : 8;
    assert((exSize == 8 || hasSIMD16TypedRW()) && "only simd8 is supported");
    unsigned int instOpt = Get_Gen4_Emask(emask, exSize);
    int numEnabledChannels = chMask.getNumEnabledChannels();

    bool useSplitSend = useSends();

    bool hasHeader = (getGenxPlatform() <= GENX_BDW);

    payloadSource sources[5]; // (maybe header) + maximal 4 addresses
    unsigned len = 0;

    if (hasHeader)
    {
        // Build header
        G4_Declare *dcl = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ, Type_UD);
        BuildMH1_BTS_PSM(this, dcl);

        // Append header
        G4_SrcRegRegion *header
            = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
        sources[len].opnd = header;
        sources[len].execSize = 8;
        sources[len].instOpt = InstOpt_WriteEnable;
        ++len;
    }

    buildTypedSurfaceAddressPayload(uOffsetOpnd, vOffsetOpnd, rOffsetOpnd, lodOpnd, exSize, instOpt, sources, len);
    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, exSize, useSplitSend, sources, len);

    //bit 8-11: RGBA channel enable
    unsigned msgDesc = chMask.getHWEncoding() << 8;
    CISA_SHARED_FUNCTION_ID sfId;

    // DC1
    // bit14-17: 0101 (read), 1101 (write)
    msgDesc |= DC1_TYPED_SURFACE_READ << 14;
    // bit12-13: 01 (use low 8 slot)
    msgDesc |= MDC_SG3_SG8L << 12;
    sfId = SFID_DP_DC1;

	bool forceSplitSend = IsBindlessSurface(*this, surface);
	if (msgs[1] == 0 && !forceSplitSend) {
        ASSERT_USER(sizes[1] == 0, "Expect the 2nd part of the payload has zero size!");
        last_inst = Create_Send_Inst_For_CISA(pred, dstOpnd,
                                              msgs[0], sizes[0],
                                              numEnabledChannels,
                                              exSize,
                                              msgDesc, sfId,
                                              false, hasHeader,
                                              true, false,
                                              surface, NULL,
                                              instOpt, false);
    } else {
        last_inst = Create_SplitSend_Inst_For_CISA(pred, dstOpnd,
                                                   msgs[0], sizes[0], msgs[1], sizes[1],
                                                   numEnabledChannels,
                                                   exSize,
                                                   msgDesc, 0, sfId,
                                                   false, hasHeader,
                                                   true, false,
                                                   surface, NULL,
                                                   instOpt, false);
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

// u must not be V0. v and r are allowed to be V0, in which case they will be
// skipped in payload.
int IR_Builder::translateVISAScatter4TypedInst(G4_Predicate           *pred,
                                               Common_VISA_EMask_Ctrl emask,
                                               ChannelMask chMask,
                                               G4_Operand *surface,
                                               Common_ISA_Exec_Size executionSize,
                                               G4_SrcRegRegion *uOffsetOpnd,
                                               G4_SrcRegRegion *vOffsetOpnd,
                                               G4_SrcRegRegion *rOffsetOpnd,
                                               G4_SrcRegRegion *lodOpnd,
                                               G4_SrcRegRegion *srcOpnd)
{

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    unsigned exSize = executionSize == EXEC_SIZE_16 ? 16 : 8;
    assert((exSize == 8 || hasSIMD16TypedRW()) && "only simd8 is supported");
    unsigned int instOpt = Get_Gen4_Emask(emask, exSize);
    int numEnabledChannels = chMask.getNumEnabledChannels();

    bool useSplitSend = useSends();

    bool hasHeader = (getGenxPlatform() <= GENX_BDW);

    payloadSource sources[6]; // (maybe header) + maximal 4 addresses + source
    unsigned len = 0;

    if (hasHeader)
    {
        // Build header
        G4_Declare *dcl = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ, Type_UD);
        BuildMH1_BTS_PSM(this, dcl);

        // Append header
        G4_SrcRegRegion *header
            = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
        sources[len].opnd = header;
        sources[len].execSize = 8;
        sources[len].instOpt = InstOpt_WriteEnable;
        ++len;
    }

    buildTypedSurfaceAddressPayload(uOffsetOpnd, vOffsetOpnd, rOffsetOpnd, lodOpnd, exSize, instOpt, sources, len);

    // Append source
    sources[len].opnd = srcOpnd;
    sources[len].execSize = exSize * numEnabledChannels;
    sources[len].instOpt = instOpt;
    ++len;

    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, exSize, useSplitSend, sources, len);

    //bit 8-11: RGBA channel enable
    unsigned msgDesc = 0;
    CISA_SHARED_FUNCTION_ID sfId;

    // DC1
    // bit14-17: 0101 (read), 1101 (write)
    msgDesc |= DC1_TYPED_SURFACE_WRITE << 14;
    // bit12-13: 01 (use low 8 slot)
    msgDesc |= MDC_SG3_SG8L << 12;
    sfId = SFID_DP_DC1;

    msgDesc |= chMask.getHWEncoding() << 8;

    G4_DstRegRegion* dstOpnd = createNullDst(Type_UD);

	bool forceSplitSend = IsBindlessSurface(*this, surface);
	if (msgs[1] == 0 && !forceSplitSend) {
        ASSERT_USER(sizes[1] == 0, "Expect the 2nd part of the payload has zero size!");
        last_inst = Create_Send_Inst_For_CISA(pred, dstOpnd,
                                              msgs[0], sizes[0],
                                              0,
                                              exSize,
                                              msgDesc, sfId,
                                              false, hasHeader,
                                              false, true,
                                              surface, NULL,
                                              instOpt, false);
    } else {
        last_inst = Create_SplitSend_Inst_For_CISA(pred, dstOpnd,
                                                   msgs[0], sizes[0], msgs[1], sizes[1],
                                                   0,
                                                   exSize,
                                                   msgDesc, 0, sfId,
                                                   false, hasHeader,
                                                   false, true,
                                                   surface, NULL,
                                                   instOpt, false);
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISATypedAtomicInst(
    CMAtomicOperations atomicOp,
    bool is16Bit,
    G4_Predicate           *pred,
    Common_VISA_EMask_Ctrl emask,
    Common_ISA_Exec_Size execSize,
    G4_Operand *surface,
    G4_SrcRegRegion *uOffsetOpnd,
    G4_SrcRegRegion *vOffsetOpnd,
    G4_SrcRegRegion *rOffsetOpnd,
    G4_SrcRegRegion *lodOpnd,
    G4_SrcRegRegion *src0,
    G4_SrcRegRegion *src1,
    G4_DstRegRegion *dst)
{

    Common_ISA_Exec_Size instExecSize = execSize;
	MUST_BE_TRUE(execSize == EXEC_SIZE_1 || 
		         execSize == EXEC_SIZE_2 || 
				 execSize == EXEC_SIZE_4 || 
				 execSize == EXEC_SIZE_8, "send exec size must be 1, 2, 4 or 8 for typed atomic messages");

    unsigned op = Get_Atomic_Op(atomicOp);

    unsigned exSize = Get_Common_ISA_Exec_Size(execSize);
    unsigned instExSize = Get_Common_ISA_Exec_Size(instExecSize);
    unsigned int instOpt = Get_Gen4_Emask(emask, instExSize);

    if (atomicOp == ATOMIC_CMPXCHG)
    {
        // we have to swap src0 and src1 since vISA has them in different order from HW 
        G4_SrcRegRegion* tmp = src0;
        src0 = src1;
        src1 = tmp;
    }

    bool useSplitSend = useSends();

    payloadSource sources[6]; // u, v, r, lod, src0, src1
    unsigned len = 0;

    buildTypedSurfaceAddressPayload(uOffsetOpnd, vOffsetOpnd, rOffsetOpnd, lodOpnd, exSize, instOpt, sources, len);

    if (src0 != nullptr && !src0->isNullReg())
    {
        sources[len].opnd = src0;
        sources[len].execSize = exSize;
        sources[len].instOpt = instOpt;
        ++len;
    }

    if (src1 != nullptr && !src1->isNullReg())
    {
        sources[len].opnd = src1;
        sources[len].execSize = exSize;
        sources[len].instOpt = instOpt;
        ++len;
    }

    G4_SrcRegRegion *msgs[2] = { 0, 0 };
    unsigned sizes[2] = { 0, 0 };
    preparePayload(msgs, sizes, exSize, useSplitSend, sources, len);

    unsigned dstLength = dst->isNullReg() ? 0 : 1;

    unsigned msgDesc = 0;
    // BTI is filled later
    msgDesc |= op << 8;
    msgDesc |= (dstLength != 0 ? 1 : 0) << 13;

    if (is16Bit)
    {
        msgDesc |= DC1_TYPED_HALF_INTEGER_ATOMIC << 14;
    }
    else
    {
        msgDesc |= DC1_TYPED_ATOMIC << 14;
    }

    bool forceSplitSend = IsBindlessSurface(*this, surface);
    if (msgs[1] == 0 && !forceSplitSend)
    {
        ASSERT_USER(sizes[1] == 0, "Expect the 2nd part of the payload has zero size!");
        last_inst = Create_Send_Inst_For_CISA(pred, dst,
            msgs[0], sizes[0], dstLength, exSize,
            msgDesc, SFID_DP_DC1,
            false, false,
            true, true,
            surface, NULL, 
            instOpt, false);
    }
    else
    {
        last_inst = Create_SplitSend_Inst_For_CISA(pred, dst,
            msgs[0], sizes[0], msgs[1], sizes[1],
            dstLength, exSize,
            msgDesc, 0, SFID_DP_DC1,
            false, false,
            true, true,
            surface, NULL,
            instOpt, false);
    }

    return CM_SUCCESS;

}


static void
BuildMH2_A32_PSM(IR_Builder *IRB, G4_Declare *header,
                 uint16_t scale, G4_Operand *globalOffset) {
    // Clear header
    G4_DstRegRegion *h = IRB->createDstRegRegion(Direct, header->getRegVar(),
                                                 0, 0, 1, Type_UD);
    IRB->createInst(NULL, G4_mov, NULL, false, 8, h,
                    IRB->createImm(0, Type_UD), NULL, InstOpt_WriteEnable);
    // Copy global offset if necessary.
    if (!(globalOffset->isImm() && globalOffset->asImm()->isZero())) {
        G4_DstRegRegion *gOffDst = IRB->createDstRegRegion(Direct,
                                                           header->getRegVar(),
                                                           0, 5, 1, Type_UD);
        IRB->createInst(NULL, G4_mov, NULL, false, 1, gOffDst,
                        globalOffset, NULL, InstOpt_WriteEnable);
    }
    // Copy scale pitch if necessary.
    if (scale != 0) {
        G4_Operand *scaleImm = IRB->createImm(scale, Type_UD);
        G4_DstRegRegion *pitchDst = IRB->createDstRegRegion(Direct,
                                                            header->getRegVar(),
                                                            0, 0, 1, Type_UD);
        IRB->createInst(NULL, G4_mov, NULL, false, 1, pitchDst,
                        scaleImm, NULL, InstOpt_WriteEnable);
    }
    // Copy PSM which is set to 0xFFFF so far.
    G4_Operand *maskImm = IRB->createImm(0xFFFF, Type_UD);
    G4_DstRegRegion *pitchDst = IRB->createDstRegRegion(Direct,
                                                        header->getRegVar(),
                                                        0, 7, 1, Type_UD);
    IRB->createInst(NULL, G4_mov, NULL, false, 1, pitchDst,
                    maskImm, NULL, InstOpt_WriteEnable);
}

// apply the sideband offset (can be either imm or variable) to the message descriptor
void IR_Builder::applySideBandOffset(G4_Operand* sideBand, G4_SendMsgDescriptor* sendMsgDesc)
{
#define SIDEBAND_OFFSET_IN_EXDESC 12

	if (sideBand->isImm())
	{
		// mov (1) a0.0 sideband << 0xC
		uint32_t sidebandInDesc = (uint32_t)(sideBand->asImm()->getImm() << SIDEBAND_OFFSET_IN_EXDESC);
		G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(builtinA0, 1);
		createInst(nullptr, G4_mov, nullptr, false, 1, dst,
			createImm(sidebandInDesc, Type_UD), nullptr, InstOpt_WriteEnable);
	}
	else
	{
		MUST_BE_TRUE(sideBand->isSrcRegRegion(), "sideband offset should be a srcRegRegion");
		// shl (1) a0.0 sideband 0xC
		G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(builtinA0, 1);
		createInst(nullptr, G4_shl, nullptr, false, 1, dst, sideBand,
			createImm(SIDEBAND_OFFSET_IN_EXDESC, Type_UW), InstOpt_WriteEnable);
	}

	// add (1) a0.0 a0.0 MD
	G4_DstRegRegion* a0Dst = Create_Dst_Opnd_From_Dcl(builtinA0, 1);
	G4_SrcRegRegion* a0Src = Create_Src_Opnd_From_Dcl(builtinA0, getRegionScalar());
	createInst(nullptr, G4_add, nullptr, false, 1, a0Dst, a0Src,
		createImm(sendMsgDesc->getExtendedDesc(), Type_UD), InstOpt_WriteEnable);
}

static void
BuildMH2_A32(IR_Builder *IRB, G4_Declare *header,
             uint16_t scale, G4_Operand *globalOffset) {
    // Clear header
    G4_DstRegRegion *h = IRB->createDstRegRegion(Direct, header->getRegVar(),
                                                 0, 0, 1, Type_UD);
    IRB->createInst(NULL, G4_mov, NULL, false, 8, h,
                    IRB->createImm(0, Type_UD), NULL, InstOpt_WriteEnable);
    // Copy global offset if necessary.
    if (!(globalOffset->isImm() && globalOffset->asImm()->isZero())) {
        G4_DstRegRegion *gOffDst = IRB->createDstRegRegion(Direct,
                                                           header->getRegVar(),
                                                           0, 5, 1, Type_UD);
        IRB->createInst(NULL, G4_mov, NULL, false, 1, gOffDst,
                        globalOffset, NULL, InstOpt_WriteEnable);
    }
    // Copy scale pitch if necessary.
    if (scale != 0) {
        G4_Operand *scaleImm = IRB->createImm(scale, Type_UD);
        G4_DstRegRegion *pitchDst = IRB->createDstRegRegion(Direct,
                                                            header->getRegVar(),
                                                            0, 0, 1, Type_UD);
        IRB->createInst(NULL, G4_mov, NULL, false, 1, pitchDst,
                        scaleImm, NULL, InstOpt_WriteEnable);
    }
}

int IR_Builder::translateVISASLMUntypedScaledInst(
	bool isRead,
	G4_Predicate *pred,
	Common_ISA_Exec_Size   execSize,
	Common_VISA_EMask_Ctrl eMask,
	ChannelMask            chMask,
	uint16_t               scale,
	G4_Operand             *sideBand,
	G4_SrcRegRegion        *offsets,
	G4_Operand			   *srcOrDst)
{
	unsigned exSize = Get_Common_ISA_Exec_Size(execSize);
	unsigned instOpt = Get_Gen4_Emask(eMask, exSize);

	payloadSource sources[2]; // Maximal 2 sources, offset + source
	unsigned len = 0;

	sources[len].opnd = offsets;
	sources[len].execSize = exSize;
	sources[len].instOpt = instOpt;
	++len;
	if (!isRead)
	{
		sources[len].opnd = srcOrDst->asSrcRegRegion();
		sources[len].execSize = exSize * chMask.getNumEnabledChannels();
		sources[len].instOpt = instOpt;
		++len;
	}

	G4_SrcRegRegion *msgs[2] = { 0, 0 };
	unsigned sizes[2] = { 0, 0 };
	preparePayload(msgs, sizes, exSize, true, sources, len);

	CISA_SHARED_FUNCTION_ID SFID = SFID_DP_DC2;

	unsigned MD = 0;
	// Leave sidebind scale offset 0 as it is not used now.
	MD |= (isRead ? DC2_UNTYPED_SURFACE_READ : DC2_UNTYPED_SURFACE_WRITE) << 14;
	MD |= (execSize == EXEC_SIZE_8 ? MDC_SM3_SIMD8 : MDC_SM3_SIMD16) << 12;
	MD |= chMask.getHWEncoding() << 8;
	// SLM encodes scale pitch in MD.
	MD |= 1 << 7;
	MD |= scale & 0x7F;

	G4_DstRegRegion *dst = isRead ? srcOrDst->asDstRegRegion() : createNullDst(Type_UD);
	unsigned resLen = isRead ? (exSize / GENX_DATAPORT_IO_SZ) *
		chMask.getNumEnabledChannels() : 0;

	uint32_t exFuncCtrl = 0;
	G4_SendMsgDescriptor *sendMsgDesc = createSendMsgDesc(MD, resLen, sizes[0], SFID,
		false, sizes[1], (uint16_t)exFuncCtrl, isRead, !isRead, nullptr, nullptr);

	applySideBandOffset(sideBand, sendMsgDesc);

	createSplitSendInst(
		pred,
		G4_sends,
		(uint8_t) exSize,
		dst,
		msgs[0],
		msgs[1],
		createImm(sendMsgDesc->getDesc(), Type_UD),
		instOpt,
		sendMsgDesc,
		Create_Src_Opnd_From_Dcl(builtinA0, getRegionScalar()));

	return CM_SUCCESS;
}

int IR_Builder::translateVISAGather4ScaledInst(G4_Predicate           *pred,
                                               Common_ISA_Exec_Size   execSize,
                                               Common_VISA_EMask_Ctrl eMask,
                                               ChannelMask            chMask,
                                               G4_Operand             *surface,
                                               G4_Operand             *globalOffset,
                                               G4_SrcRegRegion        *offsets,
                                               G4_DstRegRegion        *dst) {

    surface = lowerSurface255To253(surface, *this);

    return translateGather4Inst(pred, execSize, eMask, chMask, surface,
        globalOffset, offsets, dst);
}

int IR_Builder::translateVISAScatter4ScaledInst(G4_Predicate           *pred,
                                                Common_ISA_Exec_Size   execSize,
                                                Common_VISA_EMask_Ctrl eMask,
                                                ChannelMask            chMask,
                                                G4_Operand             *surface,
                                                G4_Operand             *globalOffset,
                                                G4_SrcRegRegion        *offsets,
                                                G4_SrcRegRegion        *src)
{
    surface = lowerSurface255To253(surface, *this);
    return translateScatter4Inst(pred, execSize, eMask, chMask, surface,
        globalOffset, offsets, src);
}

int IR_Builder::translateGather4Inst(G4_Predicate           *pred,
                                     Common_ISA_Exec_Size   execSize,
                                     Common_VISA_EMask_Ctrl eMask,
                                     ChannelMask            chMask,
                                     G4_Operand             *surface,
                                     G4_Operand             *globalOffset,
                                     G4_SrcRegRegion        *offsets,
                                     G4_DstRegRegion        *dst) {
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    ASSERT_USER(execSize == EXEC_SIZE_8 || execSize == EXEC_SIZE_16,
                "Only support SIMD8 or SIMD16!");

    unsigned exSize = Get_Common_ISA_Exec_Size(execSize);
    unsigned instOpt = Get_Gen4_Emask(eMask, exSize);

    bool useSplitSend = useSends();
    bool useHeader = needsA32MsgHeader() && surface && IsStatelessSurface(surface);

    // In case non-zero global offset is specified, we need to recalculate
    // offsets.
    if (!globalOffset->isImm() || globalOffset->asImm()->getImm() != 0) {
        G4_Declare *dcl = Create_MRF_Dcl(exSize, offsets->getType());
        G4_DstRegRegion *tmp = Create_Dst_Opnd_From_Dcl(dcl, 1);
        createInst(pred, G4_add, 0, false, exSize, tmp, offsets, globalOffset, instOpt);
        offsets = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
    }

    payloadSource sources[2]; // Maximal 2 sources, optional header + offsets
    unsigned len = 0;

    if (useHeader) {
        G4_Declare *dcl = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ, Type_UD);

        BuildMH1_A32_PSM(this, dcl);

        G4_SrcRegRegion *header
            = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
        sources[len].opnd = header;
        sources[len].execSize = 8;
        sources[len].instOpt = InstOpt_WriteEnable;
        ++len;
    }

    sources[len].opnd = offsets;
    sources[len].execSize = exSize;
    sources[len].instOpt = instOpt;
    ++len;

    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, exSize, useSplitSend, sources, len);

    CISA_SHARED_FUNCTION_ID SFID = SFID_DP_DC1;

    unsigned MD = 0;
    // Leave sidebind scale offset 0 as it is not used now.
    MD |= DC1_UNTYPED_SURFACE_READ << 14;
    MD |= (execSize == EXEC_SIZE_8 ? MDC_SM3_SIMD8 : MDC_SM3_SIMD16) << 12;
    MD |= chMask.getHWEncoding() << 8;

    unsigned resLen = (exSize / GENX_DATAPORT_IO_SZ) *
                      chMask.getNumEnabledChannels();

	bool forceSplitSend = IsBindlessSurface(*this, surface);
	if (msgs[1] == 0 && !forceSplitSend) {
        ASSERT_USER(sizes[1] == 0, "Expect the 2nd part of the payload has zero size!");
        last_inst = Create_Send_Inst_For_CISA(pred, dst,
                                              msgs[0], sizes[0],
                                              resLen,
                                              exSize,
                                              MD, SFID,
                                              false, useHeader,
                                              true, false,
                                              surface, NULL,
                                              instOpt, false);
    } else {
        last_inst = Create_SplitSend_Inst_For_CISA(pred, dst,
                                                   msgs[0], sizes[0], msgs[1], sizes[1],
                                                   resLen,
                                                   exSize,
                                                   MD, 0, SFID,
                                                   false, useHeader,
                                                   true, false,
                                                   surface, NULL,
                                                   instOpt, false);
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateScatter4Inst(G4_Predicate           *pred,
                                      Common_ISA_Exec_Size   execSize,
                                      Common_VISA_EMask_Ctrl eMask,
                                      ChannelMask            chMask,
                                      G4_Operand             *surface,
                                      G4_Operand             *globalOffset,
                                      G4_SrcRegRegion        *offsets,
                                      G4_SrcRegRegion        *src) {
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    ASSERT_USER(execSize == EXEC_SIZE_8 || execSize == EXEC_SIZE_16,
                "Only support SIMD8 or SIMD16!");

    unsigned exSize = Get_Common_ISA_Exec_Size(execSize);
    unsigned instOpt = Get_Gen4_Emask(eMask, exSize);

    bool useSplitSend = useSends();
    bool useHeader = needsA32MsgHeader() && surface && IsStatelessSurface(surface);

    // In case non-zero global offset is specified, we need to recalculate
    // offsets.
    if (!globalOffset->isImm() || globalOffset->asImm()->getImm() != 0) {
        G4_Declare *dcl = Create_MRF_Dcl(exSize, offsets->getType());
        G4_DstRegRegion *tmp = Create_Dst_Opnd_From_Dcl(dcl, 1);
        createInst(pred, G4_add, 0, false, exSize, tmp, offsets, globalOffset, instOpt);
        offsets = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
    }

    payloadSource sources[3]; // Maximal 3 sources, optional header + offsets + src
    unsigned len = 0;

    if (useHeader) {
        G4_Declare *dcl = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ, Type_UD);

        // TODO: Get PSM supported on demand.
        BuildMH1_A32_PSM(this, dcl);

        G4_SrcRegRegion *header
            = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
        sources[len].opnd = header;
        sources[len].execSize = 8;
        sources[len].instOpt = InstOpt_WriteEnable;
        ++len;
    }

    sources[len].opnd = offsets;
    sources[len].execSize = exSize;
    sources[len].instOpt = instOpt;
    ++len;
    sources[len].opnd = src;
    sources[len].execSize = exSize * chMask.getNumEnabledChannels();
    sources[len].instOpt = instOpt;
    ++len;

    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, exSize, useSplitSend, sources, len);

    CISA_SHARED_FUNCTION_ID SFID = SFID_DP_DC1;

    unsigned MD = 0;
    // Leave sidebind scale offset 0 as it is not used now.
    MD |= DC1_UNTYPED_SURFACE_WRITE << 14;
    MD |= (execSize == EXEC_SIZE_8 ? MDC_SM3_SIMD8 : MDC_SM3_SIMD16) << 12;
    MD |= chMask.getHWEncoding() << 8;

    G4_DstRegRegion *dst = createNullDst(Type_UD);
	bool forceSplitSend = IsBindlessSurface(*this, surface);
	if (msgs[1] == 0 && !forceSplitSend) {
        ASSERT_USER(sizes[1] == 0, "Expect the 2nd part of the payload has zero size!");
        last_inst = Create_Send_Inst_For_CISA(pred, dst,
                                              msgs[0], sizes[0],
                                              0,
                                              exSize,
                                              MD, SFID,
                                              false, useHeader,
                                              false, true,
                                              surface, NULL, 
                                              instOpt, false);
    } else {
        last_inst = Create_SplitSend_Inst_For_CISA(pred, dst,
                                                   msgs[0], sizes[0], msgs[1], sizes[1],
                                                   0,
                                                   exSize,
                                                   MD, 0, SFID,
                                                   false, useHeader,
                                                   false, true,
                                                   surface, NULL,
                                                   instOpt, false);
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISAStrBufLdScaledInst(G4_Predicate           *pred,
                                                Common_ISA_Exec_Size   execSize,
                                                Common_VISA_EMask_Ctrl eMask,
                                                ChannelMask            chMask,
                                                G4_Operand             *surface,
                                                G4_SrcRegRegion        *uOffsets,
                                                G4_SrcRegRegion        *vOffsets,
                                                G4_DstRegRegion        *dst) {
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    ASSERT_USER(execSize == EXEC_SIZE_8 || execSize == EXEC_SIZE_16,
                "Only support SIMD8 or SIMD16!");
    ASSERT_USER(!IsSLMSurface(surface) && !IsStatelessSurface(surface),
                "Expect surfaces of neither SLM nor A32!");

    surface = lowerSurface255To253(surface, *this);
    // NOTE: scale pitch is in state of that surface (STRBUF).
    unsigned exSize = Get_Common_ISA_Exec_Size(execSize);
    unsigned instOpt = Get_Gen4_Emask(eMask, exSize);

    // As PSM is filled with default value, header is not used so far.
    bool useHeader = false;
    bool useSplitSend = useSends();

    payloadSource sources[2]; // Maximal 2 sources, U + V
    unsigned len = 0;

    sources[len].opnd = uOffsets;
    sources[len].execSize = exSize;
    sources[len].instOpt = instOpt;
    ++len;
    sources[len].opnd = vOffsets;
    sources[len].execSize = exSize;
    sources[len].instOpt = instOpt;
    ++len;

    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, exSize, useSplitSend, sources, len);

    CISA_SHARED_FUNCTION_ID SFID = SFID_DP_DC1;

    unsigned MD = 0;
    // Leave sidebind scale offset 0 as it is not used now.
    MD |= DC1_UNTYPED_SURFACE_READ << 14;
    MD |= (execSize == EXEC_SIZE_8 ? MDC_SM3_SIMD8 : MDC_SM3_SIMD16) << 12;
    MD |= chMask.getHWEncoding() << 8;

    unsigned resLen = (exSize / GENX_DATAPORT_IO_SZ) *
                      chMask.getNumEnabledChannels();
	bool forceSplitSend = IsBindlessSurface(*this, surface);
	if (msgs[1] == 0 && !forceSplitSend) {
        ASSERT_USER(sizes[1] == 0, "Expect the 2nd part of the payload has zero size!");
        last_inst = Create_Send_Inst_For_CISA(pred, dst,
                                              msgs[0], sizes[0],
                                              resLen,
                                              exSize,
                                              MD, SFID,
                                              false, useHeader,
                                              true, false,
                                              surface, NULL, 
                                              instOpt, false);
    } else {
        last_inst = Create_SplitSend_Inst_For_CISA(pred, dst,
                                                   msgs[0], sizes[0], msgs[1], sizes[1],
                                                   resLen,
                                                   exSize,
                                                   MD, 0, SFID,
                                                   false, useHeader,
                                                   true, false,
                                                   surface, NULL,
                                                   instOpt, false);
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISAStrBufStScaledInst(G4_Predicate           *pred,
                                                Common_ISA_Exec_Size   execSize,
                                                Common_VISA_EMask_Ctrl eMask,
                                                ChannelMask            chMask,
                                                G4_Operand             *surface,
                                                G4_SrcRegRegion        *uOffsets,
                                                G4_SrcRegRegion        *vOffsets,
                                                G4_SrcRegRegion        *src) {
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    ASSERT_USER(execSize == EXEC_SIZE_8 || execSize == EXEC_SIZE_16,
                "Only support SIMD8 or SIMD16!");
    ASSERT_USER(!IsSLMSurface(surface) && !IsStatelessSurface(surface),
                "Expect surface of neither SLM nor A32!");

    surface = lowerSurface255To253(surface, *this);
    // NOTE: scale pitch is in state of that surface (STRBUF).
    unsigned exSize = Get_Common_ISA_Exec_Size(execSize);
    unsigned instOpt = Get_Gen4_Emask(eMask, exSize);

    // As PSM is filled with default value, header is not used so far.
    bool useHeader = false;
    bool useSplitSend = useSends();

    payloadSource sources[3]; // Maximal 3 sources, U + V + src
    unsigned len = 0;

    sources[len].opnd = uOffsets;
    sources[len].execSize = exSize;
    sources[len].instOpt = instOpt;
    ++len;
    sources[len].opnd = vOffsets;
    sources[len].execSize = exSize;
    sources[len].instOpt = instOpt;
    ++len;
    sources[len].opnd = src;
    sources[len].execSize = exSize * chMask.getNumEnabledChannels();
    sources[len].instOpt = instOpt;
    ++len;

    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, exSize, useSplitSend, sources, len);

    CISA_SHARED_FUNCTION_ID SFID = SFID_DP_DC1;

    unsigned MD = 0;
    // Leave sidebind scale offset 0 as it is not used now.
    MD |= DC1_UNTYPED_SURFACE_WRITE << 14;
    MD |= (execSize == EXEC_SIZE_8 ? MDC_SM3_SIMD8 : MDC_SM3_SIMD16) << 12;
    MD |= chMask.getHWEncoding() << 8;

    G4_DstRegRegion *dst = createNullDst(Type_UD);
	bool forceSplitSend = IsBindlessSurface(*this, surface);
	if (msgs[1] == 0 && !forceSplitSend) {
        ASSERT_USER(sizes[1] == 0, "Expect the 2nd part of the payload has zero size!");
        last_inst = Create_Send_Inst_For_CISA(pred, dst,
                                              msgs[0], sizes[0],
                                              0,
                                              exSize,
                                              MD, SFID,
                                              false, useHeader,
                                              false, true,
                                              surface, NULL, 
                                              instOpt, false);
    } else {
        last_inst = Create_SplitSend_Inst_For_CISA(pred, dst,
                                                   msgs[0], sizes[0], msgs[1], sizes[1],
                                                   0,
                                                   exSize,
                                                   MD, 0, SFID,
                                                   false, useHeader,
                                                   false, true,
                                                   surface, NULL,
                                                   instOpt, false);
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

/// GetNumBatch() - return the number of batches required to copy the raw
/// operand to message payload
static unsigned GetNumBatch(Common_ISA_SVM_Block_Type blockSize,
                            Common_ISA_SVM_Block_Num  numBlocks) {
    switch (blockSize) {
    case SVM_BLOCK_TYPE_BYTE:
        switch (numBlocks) {
        case SVM_BLOCK_NUM_1:
        case SVM_BLOCK_NUM_2:
        case SVM_BLOCK_NUM_4:
            return 1;
        case SVM_BLOCK_NUM_8:
            return 2;
        }
        break;
    case SVM_BLOCK_TYPE_DWORD:
        return Get_Common_ISA_SVM_Block_Num(numBlocks);
    case SVM_BLOCK_TYPE_QWORD:
        return Get_Common_ISA_SVM_Block_Num(numBlocks);
    }
    ASSERT_USER(false, "Unhandled sizes/numbers of block/element!");
    return 0;
}

int IR_Builder::translateVISAGatherScaledInst(G4_Predicate              *pred,
                                              Common_ISA_Exec_Size      execSize,
                                              Common_VISA_EMask_Ctrl    eMask,
                                              Common_ISA_SVM_Block_Num  numBlocks,
                                              G4_Operand                *surface,
                                              G4_Operand                *globalOffset,
                                              G4_SrcRegRegion           *offsets,
                                              G4_DstRegRegion           *dst)
{
    surface = lowerSurface255To253(surface, *this);

    return translateByteGatherInst(pred, execSize, eMask, numBlocks,
        surface, globalOffset, offsets, dst);
}

int IR_Builder::translateVISAScatterScaledInst(G4_Predicate              *pred,
                                               Common_ISA_Exec_Size      execSize,
                                               Common_VISA_EMask_Ctrl    eMask,
                                               Common_ISA_SVM_Block_Num  numBlocks,
                                               G4_Operand                *surface,
                                               G4_Operand                *globalOffset,
                                               G4_SrcRegRegion           *offsets,
                                               G4_SrcRegRegion           *src)
{

    surface = lowerSurface255To253(surface, *this);
    return translateByteScatterInst(pred, execSize, eMask, numBlocks,
        surface, globalOffset, offsets, src);
}

//
// For a SLM byte scaled inst with non-zero sideband, we must generate a split send
// and store the sideband into the extended message descriptor.  ExDesc must be indirect
// in this case
//
int IR_Builder::translateVISASLMByteScaledInst(bool isRead,
	G4_Predicate *pred,
	Common_ISA_Exec_Size execSize,
	Common_VISA_EMask_Ctrl eMask,
	Common_ISA_SVM_Block_Type blockSize,
	Common_ISA_SVM_Block_Num numBlocks,
	uint8_t scale,
	G4_Operand *sideBand,
	G4_SrcRegRegion *offsets,
	G4_Operand *srcOrDst)
{

	unsigned exSize = Get_Common_ISA_Exec_Size(execSize);
	unsigned instOpt = Get_Gen4_Emask(eMask, exSize);
	unsigned numBatch = GetNumBatch(blockSize, numBlocks);

	uint16_t exFuncCtrl = 0;
	payloadSource sources[3];
	unsigned len = 0;

	sources[len].opnd = offsets;
	sources[len].execSize = exSize;
	sources[len].instOpt = instOpt;
	++len;
	if (!isRead)
	{
		sources[len].opnd = srcOrDst->asSrcRegRegion();
		sources[len].execSize = exSize * numBatch;
		sources[len].instOpt = instOpt;
		++len;
	}

	G4_SrcRegRegion *msgs[2] = { 0, 0 };
	unsigned sizes[2] = { 0, 0 };
	preparePayload(msgs, sizes, exSize, true, sources, len);

	CISA_SHARED_FUNCTION_ID SFID = SFID_DP_DC2;

	unsigned MD = 0;
	// Leave sidebind scale offset 0 as it is not used now.
	MD |= (isRead ? DC2_BYTE_SCATTERED_READ : DC2_BYTE_SCATTERED_WRITE) << 14;
	MD |= numBlocks << 10;
	MD |= (execSize == EXEC_SIZE_8 ? MDC_SM2_SIMD8 : MDC_SM2_SIMD16) << 8;
	MD |= 1 << 7;
	MD |= scale & 0x7F;

	G4_DstRegRegion *dst = isRead ? srcOrDst->asDstRegRegion() : createNullDst(Type_UD);
	unsigned resLen = isRead ? (exSize / GENX_DATAPORT_IO_SZ) * numBatch : 0;

	G4_SendMsgDescriptor *sendMsgDesc = createSendMsgDesc(MD, resLen, sizes[0], SFID,
		false, sizes[1], exFuncCtrl, isRead, !isRead, nullptr, nullptr);

	applySideBandOffset(sideBand, sendMsgDesc);

	createSplitSendInst(
		pred,
		G4_sends,
		(uint8_t)exSize,
		dst,
		msgs[0],
		msgs[1],
		createImm(sendMsgDesc->getDesc(), Type_UD),
		instOpt,
		sendMsgDesc,
		Create_Src_Opnd_From_Dcl(builtinA0, getRegionScalar()));

	return CM_SUCCESS;
}

static void
BuildMH_A32_GO(IR_Builder *IRB, G4_Declare *header, G4_Operand *globalOffset = 0) {
    // Clear header
    G4_DstRegRegion *h = IRB->createDstRegRegion(Direct, header->getRegVar(),
                                                 0, 0, 1, Type_UD);
    IRB->createInst(NULL, G4_mov, NULL, false, 8, h,
                    IRB->createImm(0, Type_UD), NULL, InstOpt_WriteEnable);
    // Copy global offset if necessary.
    if (globalOffset &&
        !(globalOffset->isImm() &&
          globalOffset->asImm()->isZero())) {
        G4_DstRegRegion *gOffDst = IRB->createDstRegRegion(Direct,
                                                           header->getRegVar(),
                                                           0, 2, 1, Type_UD);
        IRB->createInst(NULL, G4_mov, NULL, false, 1, gOffDst,
                        globalOffset, NULL, InstOpt_WriteEnable);
    }
}

int IR_Builder::translateByteGatherInst(G4_Predicate *pred,
                                        Common_ISA_Exec_Size execSize,
                                        Common_VISA_EMask_Ctrl eMask,
                                        Common_ISA_SVM_Block_Num numBlocks,
                                        G4_Operand *surface,
                                        G4_Operand *globalOffset,
                                        G4_SrcRegRegion *offsets,
                                        G4_DstRegRegion *dst) {
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    ASSERT_USER(execSize == EXEC_SIZE_1 || execSize == EXEC_SIZE_2 ||
                execSize == EXEC_SIZE_4 || execSize == EXEC_SIZE_8 ||
                execSize == EXEC_SIZE_16,
                "Only support SIMD1, SIMD2, SIMD4, SIMD8 or SIMD16!");
    ASSERT_USER(numBlocks == SVM_BLOCK_NUM_1 ||
                numBlocks == SVM_BLOCK_NUM_2 ||
                numBlocks == SVM_BLOCK_NUM_4,
                "Byte gather ONLY supports 1, 2, and 4 elements per slot!");

    Common_ISA_Exec_Size instExecSize = execSize;
    if (execSize == EXEC_SIZE_1 || execSize == EXEC_SIZE_2 ||
        execSize == EXEC_SIZE_4) {
        execSize = EXEC_SIZE_8;
    }

    unsigned exSize = Get_Common_ISA_Exec_Size(execSize);
    unsigned instExSize = Get_Common_ISA_Exec_Size(instExecSize);
    unsigned instOpt = Get_Gen4_Emask(eMask, instExSize);
    unsigned numBatch = GetNumBatch(SVM_BLOCK_TYPE_BYTE, numBlocks);

    bool isSLM = IsSLMSurface(surface);
    // SLM forbids header. Header is optional in A32 when both scale and global
    // offset are 0s.
    bool useHeader = !isSLM && needsA32MsgHeader();
    bool useSplitSend = useSends();

    // In case non-zero global offset is specified, we need to recalculate
    // offsets.
    //
    // NOTE: Even though pre-SKL devices require header, eliminating global
    //       offset by adjusting offsets will simplify the header generation.
    if (!globalOffset->isImm() || globalOffset->asImm()->getImm() != 0)
    {
        G4_Declare *dcl = Create_MRF_Dcl(exSize, offsets->getType());
        G4_DstRegRegion *tmp = Create_Dst_Opnd_From_Dcl(dcl, 1);
        createInst(nullptr, G4_add, 0, false, exSize, tmp, offsets, globalOffset, instOpt);
        offsets = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
    }

    payloadSource sources[2]; // Maximal 2 sources, optional header + offsets
    unsigned len = 0;

    if (useHeader) {
        G4_Declare *dcl = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ, Type_UD);

        // TODO: Get BTS supported on demand.
        BuildMH_A32_GO(this, dcl);

        G4_SrcRegRegion *header
            = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
        sources[len].opnd = header;
        sources[len].execSize = 8;
        sources[len].instOpt = InstOpt_WriteEnable;
        ++len;
    }

    sources[len].opnd = offsets;
    sources[len].execSize = exSize;
    sources[len].instOpt = instOpt;
    ++len;

    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, exSize, useSplitSend, sources, len);

    CISA_SHARED_FUNCTION_ID SFID = SFID_DP_DC;

    unsigned MD = 0;
    MD |= DC_BYTE_SCATTERED_READ << 14;
    MD |= numBlocks << 10;
    MD |= (execSize == EXEC_SIZE_8 ? MDC_SM2_SIMD8 : MDC_SM2_SIMD16) << 8;

    unsigned resLen = (exSize / GENX_DATAPORT_IO_SZ) * numBatch;
	bool forceSplitSend = IsBindlessSurface(*this, surface);
	if (msgs[1] == 0 && !forceSplitSend) {
        ASSERT_USER(sizes[1] == 0, "Expect the 2nd part of the payload has zero size!");
        last_inst = Create_Send_Inst_For_CISA(pred, dst,
                                              msgs[0], sizes[0],
                                              resLen,
                                              instExSize,
                                              MD, SFID,
                                              false, useHeader,
                                              true, false,
                                              surface, NULL,
                                              instOpt, false);
    } else {
        last_inst = Create_SplitSend_Inst_For_CISA(pred, dst,
                                                   msgs[0], sizes[0], msgs[1], sizes[1],
                                                   resLen,
                                                   instExSize,
                                                   MD, 0, SFID,
                                                   false, useHeader,
                                                   true, false,
                                                   surface, NULL,
                                                   instOpt, false);
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateByteScatterInst(G4_Predicate *pred,
                                         Common_ISA_Exec_Size execSize,
                                         Common_VISA_EMask_Ctrl eMask,
                                         Common_ISA_SVM_Block_Num numBlocks,
                                         G4_Operand *surface,
                                         G4_Operand *globalOffset,
                                         G4_SrcRegRegion *offsets,
                                         G4_SrcRegRegion *src) {
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    ASSERT_USER(execSize == EXEC_SIZE_1 || execSize == EXEC_SIZE_2 ||
                execSize == EXEC_SIZE_4 || execSize == EXEC_SIZE_8 ||
                execSize == EXEC_SIZE_16,
                "Only support SIMD1, SIMD2, SIMD4, SIMD8 or SIMD16!");
    ASSERT_USER(numBlocks == SVM_BLOCK_NUM_1 ||
                numBlocks == SVM_BLOCK_NUM_2 ||
                numBlocks == SVM_BLOCK_NUM_4,
                "Byte scatter ONLY supports 1, 2, and 4 elements per slot!");

    Common_ISA_Exec_Size instExecSize = execSize;
    if (execSize == EXEC_SIZE_1 || execSize == EXEC_SIZE_2 ||
        execSize == EXEC_SIZE_4) {
        execSize = EXEC_SIZE_8;
    }

    unsigned exSize = Get_Common_ISA_Exec_Size(execSize);
    unsigned instExSize = Get_Common_ISA_Exec_Size(instExecSize);
    unsigned instOpt = Get_Gen4_Emask(eMask, exSize);
    unsigned numBatch = GetNumBatch(SVM_BLOCK_TYPE_BYTE, numBlocks);

    bool isSLM = IsSLMSurface(surface);
    // SLM forbids header. Header is optional in A32 when both scale and global
    // offset are 0s.
    bool useHeader = !isSLM && needsA32MsgHeader();
    bool useSplitSend = useSends();

    // In case non-zero global offset is specified, we need to recalculate
    // offsets.
    //
    // NOTE: Even though pre-SKL devices require header, eliminating global
    //       offset by adjusting offsets will simplify the header generation.
    if (!globalOffset->isImm() || globalOffset->asImm()->getImm() != 0)
    {
        G4_Declare *dcl = Create_MRF_Dcl(exSize, offsets->getType());
        G4_DstRegRegion *tmp = Create_Dst_Opnd_From_Dcl(dcl, 1);
        createInst(nullptr, G4_add, 0, false, exSize, tmp, offsets, globalOffset, instOpt);
        offsets = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
    }

    payloadSource sources[3]; // Maximal 2 sources, optional header + offsets + src
    unsigned len = 0;

    if (useHeader) {
        G4_Declare *dcl = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ, Type_UD);

        // TODO: Get BTS supported on demand.
        BuildMH_A32_GO(this, dcl);

        G4_SrcRegRegion *header
            = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
        sources[len].opnd = header;
        sources[len].execSize = 8;
        sources[len].instOpt = InstOpt_WriteEnable;
        ++len;
    }

    sources[len].opnd = offsets;
    sources[len].execSize = exSize;
    sources[len].instOpt = instOpt;
    ++len;
    sources[len].opnd = src;
    sources[len].execSize = exSize * numBatch;
    sources[len].instOpt = instOpt;
    ++len;

    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, exSize, useSplitSend, sources, len);

    CISA_SHARED_FUNCTION_ID SFID = SFID_DP_DC;

    unsigned MD = 0;
    // Leave sidebind scale offset 0 as it is not used now.
    MD |= DC_BYTE_SCATTERED_WRITE << 14;
    MD |= numBlocks << 10;
    MD |= (execSize == EXEC_SIZE_8 ? MDC_SM2_SIMD8 : MDC_SM2_SIMD16) << 8;

    G4_DstRegRegion *dst = createNullDst(Type_UD);
	bool forceSplitSend = IsBindlessSurface(*this, surface);
	if (msgs[1] == 0 && !forceSplitSend) {
        ASSERT_USER(sizes[1] == 0, "Expect the 2nd part of the payload has zero size!");
        last_inst = Create_Send_Inst_For_CISA(pred, dst,
                                              msgs[0], sizes[0],
                                              0,
                                              instExSize,
                                              MD, SFID,
                                              false, useHeader,
                                              false, true,
                                              surface, NULL,
                                              instOpt, false);
    } else {
        last_inst = Create_SplitSend_Inst_For_CISA(pred, dst,
                                                   msgs[0], sizes[0], msgs[1], sizes[1],
                                                   0,
                                                   instExSize,
                                                   MD, 0, SFID,
                                                   false, useHeader,
                                                   false, true,
                                                   surface, NULL,
                                                   instOpt, false);
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISALogicInst(ISA_Opcode opcode, G4_Predicate *predOpnd, bool saturate, Common_ISA_Exec_Size executionSize,
                                       Common_VISA_EMask_Ctrl emask, G4_DstRegRegion* dst, G4_Operand* src0, G4_Operand* src1,
                                       G4_Operand* src2, G4_Operand* src3)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    uint8_t exsize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    unsigned int inst_opt = Get_Gen4_Emask(emask, exsize);
    G4_Operand *g4Srcs[COMMON_ISA_MAX_NUM_SRC] = {src0, src1, src2, src3};

    G4_opcode g4_op = Get_G4_Opcode_From_Common_ISA_Opcode(opcode);
    if (dst->getBase() && dst->getBase()->isFlag())
    {
        g4_op = Get_Pseudo_Opcode( opcode );
        if (g4_op == G4_illegal)
        {
            return CM_FAILURE;
        }
    }

    for(int i = 0; i < ISA_Inst_Table[opcode].n_srcs; i++)
    {

        if (g4Srcs[i]->isSrcRegRegion() &&
            !isShiftOp(opcode) &&
            ( g4Srcs[i]->asSrcRegRegion()->getModifier() == Mod_Minus || g4Srcs[i]->asSrcRegRegion()->getModifier() == Mod_Minus_Abs ))
        {
            G4_Type tmpType = g4Srcs[i]->asSrcRegRegion()->getType();
            G4_Declare *tempDcl = createTempVar( exsize, tmpType, Either, Any );
            G4_DstRegRegion tmp_dst( Direct, tempDcl->getRegVar(), 0, 0, 1, tmpType );

            G4_DstRegRegion *tmp_dst_opnd = createDstRegRegion( tmp_dst );
            uint16_t vs = exsize;
            if( exsize * G4_Type_Table[g4Srcs[i]->asSrcRegRegion()->getType()].byteSize > GENX_GRF_REG_SIZ )
            {
                vs /= 2;
            }

            createInst(NULL, G4_mov, NULL, false, exsize, tmp_dst_opnd, g4Srcs[i], NULL, inst_opt);

            g4Srcs[i] = Create_Src_Opnd_From_Dcl( tempDcl, getRegionStride1());
        }
    }

    if (opcode == ISA_BFI || opcode == ISA_BFE || opcode == ISA_BFREV)
    {
        // convert all immediates to D or UD as required by HW
        // ToDo: maybe we should move this to HW conformity?
        for (int i = 0; i < 4; i++)
        {
            if (g4Srcs[i] != NULL && g4Srcs[i]->isImm())
            {
                G4_Imm* imm = g4Srcs[i]->asImm();
                switch (imm->getType())
                {
                case Type_W:
                    g4Srcs[i] = createImm(imm->getInt(), Type_D);
                    break;
                case Type_UW:
                    g4Srcs[i] = createImm(imm->getInt(), Type_UD);
                    break;
                default:
                    // ignore other types to be consistent with old behavior
                    break;
                }
            }
        }
    }

    if(opcode == ISA_BFI)
    {
        // split into
        // bfi1 tmp src0 src1
        // bfi2 dst tmp src2 src3
        G4_Declare* tmpDcl = createTempVar( exsize, g4Srcs[0]->getType(), Either, Sixteen_Word);
        G4_DstRegRegion* tmpDst = Create_Dst_Opnd_From_Dcl( tmpDcl, 1);
        last_inst = createInst(
            predOpnd,
            g4_op,
            NULL,
            saturate,
            exsize,        // it is number of bits for predicate logic op
            tmpDst,
            g4Srcs[0],
            g4Srcs[1],
            inst_opt,
            0);

        G4_SrcRegRegion* src0 = Create_Src_Opnd_From_Dcl(tmpDcl,
            (exsize == 1) ? getRegionScalar() : getRegionStride1());
        last_inst = createInst(
            predOpnd,
            G4_bfi2,
            NULL,
            saturate,
            exsize,        // it is number of bits for predicate logic op
            dst,
            src0,
            g4Srcs[2],
            g4Srcs[3],
            inst_opt,
            0);
    }
    else
    {
        // create inst
        last_inst = createInst(
            predOpnd,
            g4_op,
            NULL,
            saturate,
            exsize,        // it is number of bits for predicate logic op
            dst,
            g4Srcs[0],
            g4Srcs[1],
            g4Srcs[2],
            inst_opt,
            0);
    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISAVmeImeInst(
    uint8_t stream_mode,
    uint8_t search_ctrl,
    G4_Operand* surfaceOpnd,
    G4_Operand* uniInputOpnd,
    G4_Operand* imeInputOpnd,
    G4_Operand* ref0Opnd,
    G4_Operand* ref1Opnd,
    G4_Operand* costCenterOpnd,
    G4_DstRegRegion* outputOpnd)

{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    // add dcl for VX
    unsigned input_size_dw;

    unsigned uni_input_size;

    uni_input_size = 4;

    if((COMMON_ISA_VME_STREAM_MODE) stream_mode != VME_STREAM_IN &&
        (COMMON_ISA_VME_STREAM_MODE) stream_mode != VME_STREAM_IN_OUT) {
            input_size_dw = (uni_input_size + 2)*32/G4_Type_Table[Type_UD].byteSize;
    } else if((COMMON_ISA_VME_SEARCH_CTRL) search_ctrl == VME_SEARCH_DUAL_REF_DUAL_REC) {
        input_size_dw = (uni_input_size + 6)*32/G4_Type_Table[Type_UD].byteSize;
    } else {
        input_size_dw = (uni_input_size + 4)*32/G4_Type_Table[Type_UD].byteSize;
    }

    G4_Declare *dcl = Create_MRF_Dcl( input_size_dw, Type_UD );

    // mov  (96)    VX(0,0)<1>,  UNIInput
    Create_MOV_Send_Src_Inst( dcl, 0, 0, uni_input_size*32/G4_Type_Table[Type_UD].byteSize, uniInputOpnd, InstOpt_WriteEnable );

    // mov  (192)   VX(3,0)<1>,  IMEInput
    Create_MOV_Send_Src_Inst( dcl, (short) uni_input_size, 0, (input_size_dw - uni_input_size*32/G4_Type_Table[Type_UD].byteSize), imeInputOpnd, InstOpt_WriteEnable );

    // and  (1)     VX(0,13)<1>, VX(0,13):ub, 0xF8
    G4_DstRegRegion *tmp_dst1_opnd = createDstRegRegion(
        Direct,
        dcl->getRegVar(),
        0,
        13,
        1,
        Type_UB);

    G4_SrcRegRegion *tmp_src1_opnd = createSrcRegRegion(
        Mod_src_undef,
        Direct,
        dcl->getRegVar(),
        0,
        13,
        getRegionScalar(),
        Type_UB );

    createInst( NULL, G4_and, NULL, false, 1, tmp_dst1_opnd, tmp_src1_opnd,
        createImm( 0xF8, Type_UW ), InstOpt_WriteEnable );

    // or   (1)     VX(0,13)<1>, VX(0,13):ub, searchCtrl
    G4_DstRegRegion *tmp_dst2_opnd = createDstRegRegion(
        Direct,
        dcl->getRegVar(),
        0,
        13,
        1,
        Type_UB);

    G4_SrcRegRegion *tmp_src2_opnd = createSrcRegRegion(
        Mod_src_undef,
        Direct,
        dcl->getRegVar(),
        0,
        13,
        getRegionScalar(),
        Type_UB );

    createInst( NULL, G4_or, NULL, false, 1, tmp_dst2_opnd, tmp_src2_opnd,
        createImm( search_ctrl, Type_UW ), InstOpt_WriteEnable );

    // mov  (2)     VA(0,0)<1>,  ref0
    // since ref0 is converted from UW to UD, move it as 1 UD
    Create_MOV_Send_Src_Inst( dcl, 0, 0, 1, ref0Opnd, InstOpt_WriteEnable );

	Create_MOV_Send_Src_Inst(dcl, 0, 1, 1, ref1Opnd, InstOpt_WriteEnable);

	Create_MOV_Send_Src_Inst(dcl, 3, 0, 8, costCenterOpnd, InstOpt_WriteEnable);

    // send's operands preparation
    // create a currDst for VX
    G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());

    G4_DstRegRegion* d = Check_Send_Dst( outputOpnd->asDstRegRegion());

    unsigned temp = 0;            // Bit 7-0 of message descriptor
    temp += 0x2 << 13;            // Bit 14-13 of message descriptor
    temp += stream_mode << 15;     // Bit 16-15 of message descriptor

    unsigned regs2rcv;

    if((COMMON_ISA_VME_STREAM_MODE) stream_mode != VME_STREAM_OUT &&
        (COMMON_ISA_VME_STREAM_MODE) stream_mode != VME_STREAM_IN_OUT) {
            regs2rcv = 224/GENX_GRF_REG_SIZ;
    } else if((COMMON_ISA_VME_SEARCH_CTRL) search_ctrl == VME_SEARCH_DUAL_REF_DUAL_REC) {
        regs2rcv = 352/GENX_GRF_REG_SIZ;
    } else {
        regs2rcv = 288/GENX_GRF_REG_SIZ;
    }

    G4_INST *send_inst = Create_Send_Inst_For_CISA(
        NULL,
        d,
        payload,
        input_size_dw / GENX_DATAPORT_IO_SZ,
        regs2rcv,
        GENX_DATAPORT_IO_SZ,
        temp,
        SFID_VME,
        0,
        true,
        true,
        false,
        surfaceOpnd,
        NULL,
        InstOpt_WriteEnable,
        false);
    last_inst = send_inst;
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISAVmeSicInst(
    G4_Operand* surfaceOpnd,
    G4_Operand* uniInputOpnd,
    G4_Operand* sicInputOpnd,
    G4_DstRegRegion* outputOpnd)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    unsigned uni_input_size;

    uni_input_size = 4;

    // add dcl for VX
    unsigned input_size_dw = (uni_input_size + 4)*32/G4_Type_Table[Type_UD].byteSize;

    G4_Declare *dcl = NULL;
    G4_Declare *topDcl = uniInputOpnd->getTopDcl();

    // check if uniInputOpnd and sicInputOpnd are alias to the
    // same top level decl with consistent payload layout
    if( (topDcl == sicInputOpnd->getTopDcl()) &&
        (uniInputOpnd->getByteOffset() == 0) &&
        (sicInputOpnd->getByteOffset() == uni_input_size*32) &&
        (topDcl->getByteSize() >= uni_input_size*32 + 128) )
    {
        dcl = topDcl;
    }
    else
    {
        dcl = Create_MRF_Dcl( input_size_dw, Type_UD );
        // mov  (96)    VX(0,0)<1>,  UNIInput
        Create_MOV_Send_Src_Inst( dcl, 0, 0, uni_input_size*32/G4_Type_Table[Type_UD].byteSize, uniInputOpnd, InstOpt_WriteEnable );
        // mov  (128)   VX(3,0)<1>,  SICInput
        Create_MOV_Send_Src_Inst( dcl, (short) uni_input_size, 0, 128/G4_Type_Table[Type_UD].byteSize, sicInputOpnd, InstOpt_WriteEnable );
    }

    // send's operands preparation
    // create a currDst for VX
    G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());

    G4_DstRegRegion* d = Check_Send_Dst( outputOpnd->asDstRegRegion());

    unsigned temp = 0;            // Bit 7-0 of message descriptor
    temp += 0x1 << 13;            // Bit 14-13 of message descriptor

    unsigned regs2rcv = 7;

    // dst is already UW
    G4_INST *send_inst = Create_Send_Inst_For_CISA(
        NULL,
        d,
        payload,
        input_size_dw / GENX_DATAPORT_IO_SZ,
        regs2rcv,
        GENX_DATAPORT_IO_SZ,
        temp,
        SFID_CRE,
        0,
        true,
        true,
        false,
        surfaceOpnd,
        NULL,
        InstOpt_WriteEnable,
        false);
    last_inst = send_inst;
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISAVmeFbrInst(
    G4_Operand* surfaceOpnd,
    G4_Operand* unitInputOpnd,
    G4_Operand* fbrInputOpnd,
    G4_Operand* fbrMbModOpnd,
    G4_Operand* fbrSubMbShapeOpnd,
    G4_Operand* fbrSubPredModeOpnd,
    G4_DstRegRegion* outputOpnd)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    unsigned uni_input_size;

    uni_input_size = 4;

    // add dcl for VX
    unsigned input_size_dw = (uni_input_size + 4)*32/G4_Type_Table[Type_UD].byteSize;

    G4_Declare *dcl = Create_MRF_Dcl( input_size_dw, Type_UD );

    // mov  (96)    VX(0,0)<1>,  UNIInput
    Create_MOV_Send_Src_Inst( dcl, 0, 0, uni_input_size*32/G4_Type_Table[Type_UD].byteSize, unitInputOpnd, InstOpt_WriteEnable );

    // mov  (128)   VX(3,0)<1>,  FBRInput
    Create_MOV_Send_Src_Inst( dcl, (short) uni_input_size, 0, 128/G4_Type_Table[Type_UD].byteSize, fbrInputOpnd, InstOpt_WriteEnable );

    // mov  (1)     VX(2,20)<1>, FBRMbMode
    G4_DstRegRegion tmp_dst1(
        Direct,
        dcl->getRegVar(),
        2,
        20,
        1,
        Type_UB);
    G4_DstRegRegion *tmp_dst1_opnd = createDstRegRegion( tmp_dst1 );

    createInst(
        NULL,
        G4_mov,
        NULL,
        false,
        1,
        tmp_dst1_opnd,
        fbrMbModOpnd,
        NULL,
        InstOpt_WriteEnable);

    // mov  (1)     VX(2,21)<1>, FBRSubMbShape
    G4_DstRegRegion tmp_dst2(
        Direct,
        dcl->getRegVar(),
        2,
        21,
        1,
        Type_UB);
    G4_DstRegRegion *tmp_dst2_opnd = createDstRegRegion( tmp_dst2 );

    createInst(
        NULL,
        G4_mov,
        NULL,
        false,
        1,
        tmp_dst2_opnd,
        fbrSubMbShapeOpnd,
        NULL,
        InstOpt_WriteEnable);


    //  mov  (1)     VX(2,22)<1>, FBRSubPredMode
    G4_DstRegRegion tmp_dst3(
        Direct,
        dcl->getRegVar(),
        2,
        22,
        1,
        Type_UB);
    G4_DstRegRegion *tmp_dst3_opnd = createDstRegRegion( tmp_dst3 );

    createInst(
        NULL,
        G4_mov,
        NULL,
        false,
        1,
        tmp_dst3_opnd,
        fbrSubPredModeOpnd,
        NULL,
        InstOpt_WriteEnable);

    // send's operands preparation
    // create a currDst for VX
    G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());

    G4_DstRegRegion* d = Check_Send_Dst( outputOpnd->asDstRegRegion());

    unsigned temp = 0;            // Bit 7-0 of message descriptor
    temp += 0x3 << 13;            // Bit 14-13 of message descriptor

    unsigned regs2rcv = 7;

    G4_INST *send_inst = Create_Send_Inst_For_CISA(
        NULL,
        d,
        payload,
        input_size_dw / GENX_DATAPORT_IO_SZ,
        regs2rcv,
        GENX_DATAPORT_IO_SZ,
        temp,
        SFID_CRE,
        0,
        true,  //head_present?
        true,
        false,
        surfaceOpnd,
        NULL,
        InstOpt_WriteEnable,
        false);
    last_inst = send_inst;
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISAVmeIdmInst(
    G4_Operand* surfaceOpnd,
    G4_Operand* unitInputOpnd,
    G4_Operand* idmInputOpnd,
    G4_DstRegRegion* outputOpnd)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    unsigned uni_input_size;

    uni_input_size = 4;

    // add dcl for VX
    unsigned input_size_dw = (uni_input_size + 1)*32/G4_Type_Table[Type_UD].byteSize;

    G4_Declare *dcl = Create_MRF_Dcl( input_size_dw, Type_UD );

    // mov  (128)    VX(0,0)<1>,  UNIInput
    Create_MOV_Send_Src_Inst( dcl, 0, 0, uni_input_size*32/G4_Type_Table[Type_UD].byteSize, unitInputOpnd, InstOpt_WriteEnable );

    // mov  (32)   VX(3,0)<1>,  IDMInput
    Create_MOV_Send_Src_Inst( dcl, (short) uni_input_size, 0, 32/G4_Type_Table[Type_UD].byteSize, idmInputOpnd, InstOpt_WriteEnable );

    // send's operands preparation
    // create a currDst for VX
    G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());

    G4_DstRegRegion* d = Check_Send_Dst( outputOpnd->asDstRegRegion());

    unsigned temp = 0;            // Bit 7-0 of message descriptor
    // temp += 0x0 << 13;            // Bit 14-13 of message descriptor

    unsigned regs2rcv = 16;

    // dst is already UW
    G4_INST *send_inst = Create_Send_Inst_For_CISA(
        NULL,
        d,
        payload,
        input_size_dw / GENX_DATAPORT_IO_SZ,
        regs2rcv,
        GENX_DATAPORT_IO_SZ,
        temp,
        SFID_VME,
        0,
        true,
        true,
        false,
        surfaceOpnd,
        NULL,
        InstOpt_WriteEnable,
        false);
    last_inst = send_inst;
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISARawSendInst(G4_Predicate *predOpnd, Common_ISA_Exec_Size executionSize,
                                         Common_VISA_EMask_Ctrl emask, uint8_t modifiers, unsigned int exDesc, uint8_t numSrc,
                                         uint8_t numDst, G4_Operand* msgDescOpnd, G4_SrcRegRegion* msgOpnd, G4_DstRegRegion* dstOpnd)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    uint8_t exsize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    unsigned int inst_opt = Get_Gen4_Emask(emask, exsize);

    if (msgDescOpnd->isSrcRegRegion())
    {
        // mov (1) a0.0<1>:ud src<0;1,0>:ud {NoMask}
        G4_DstRegRegion *dstOpnd = Create_Dst_Opnd_From_Dcl( builtinA0, 1);
        createInst( NULL, G4_mov, NULL, false, 1, dstOpnd, msgDescOpnd, NULL, InstOpt_WriteEnable, 0 );
        msgDescOpnd = Create_Src_Opnd_From_Dcl( builtinA0, getRegionScalar() );
    }

    uint32_t desc = 0;
    if (msgDescOpnd->isImm())
    {
        desc = (uint32_t) msgDescOpnd->asImm()->getImm();
    }
    else
    {
        desc = G4_SendMsgDescriptor::createDesc(0, false, numSrc, numDst);
    }
    G4_SendMsgDescriptor *sendMsgDesc = createSendMsgDesc(desc, exDesc, true, true, NULL, NULL);

    // sanity check on srcLen/dstLen
    MUST_BE_TRUE(sendMsgDesc->MessageLength() <= numSrc, "message length mismatch for raw send");
    MUST_BE_TRUE(sendMsgDesc->ResponseLength() <= numDst, "response length mismatch for raw send");

    createSendInst(
        predOpnd,
        (modifiers & 1) ? G4_sendc : G4_send,
        exsize,
        dstOpnd,
        msgOpnd,
        createImm( exDesc, Type_UD ),
        msgDescOpnd,
        inst_opt,
        true,
        true,
        sendMsgDesc,
        0);
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISARawSendsInst(G4_Predicate *predOpnd, Common_ISA_Exec_Size executionSize,
                                         Common_VISA_EMask_Ctrl emask, uint8_t modifiers, G4_Operand* ex, uint8_t numSrc0, uint8_t numSrc1,
                                         uint8_t numDst, G4_Operand* msgDescOpnd, G4_Operand* src0, G4_Operand* src1, G4_DstRegRegion* dstOpnd, unsigned ffid)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    uint8_t exsize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    unsigned int inst_opt = Get_Gen4_Emask(emask, exsize);

    if(msgDescOpnd->isSrcRegRegion())
    {
        // mov (1) a0.0<1>:ud src<0;1,0>:ud {NoMask}
        G4_DstRegRegion *dstOpnd = Create_Dst_Opnd_From_Dcl( builtinA0, 1);
        createInst( NULL, G4_mov, NULL, false, 1, dstOpnd, msgDescOpnd, NULL, InstOpt_WriteEnable, 0 );
        msgDescOpnd = Create_Src_Opnd_From_Dcl( builtinA0, getRegionScalar() );
    }

    uint32_t exDescVal = 0;
    G4_SrcRegRegion *temp_exdesc_src = NULL;
    if ( ex->isImm() )
    {
        exDescVal = (unsigned)ex->asImm()->getInt();
    }

	// bit [6:10] store the extended message length, and when it's >= 16 we have to use indirect
	uint32_t extLength = (exDescVal >> 6) & 0x1F;
    if (ex->isSrcRegRegion() || extLength >= 16)
    {
        // mov (1) a0.2<1>:ud src<0;1,0>:ud {NoMask} ;
		// to hold the dynamic ext msg descriptor
        G4_Declare *dcl_temp = createDeclareNoLookup(
            "temp_exdesc",
            G4_ADDRESS,
            1,
            1,
            Type_UD );
        dcl_temp->getRegVar()->setPhyReg( phyregpool.getAddrReg(), 2 );
        G4_DstRegRegion *temp_exdesc_dst = createDstRegRegion(Direct, dcl_temp->getRegVar(), 0, 0, 1, Type_UD);
        createInst( NULL, G4_mov, NULL, false, 1, temp_exdesc_dst, ex, NULL, InstOpt_WriteEnable, 0 );
        temp_exdesc_src = createSrcRegRegion(Mod_src_undef,Direct,dcl_temp->getRegVar(), 0, 0, getRegionScalar(), Type_UD);

		if (exDescVal == 0)
		{
			exDescVal = G4_SendMsgDescriptor::createExtDesc((CISA_SHARED_FUNCTION_ID)ffid, false, numSrc1);
		}
    }

    uint32_t descVal = 0;
    if (msgDescOpnd->isImm())
    {
        descVal = (uint32_t) msgDescOpnd->asImm()->getImm();
    }
    else
    {
        descVal = G4_SendMsgDescriptor::createDesc(0, false, numSrc0, numDst);
    }

    G4_SendMsgDescriptor *sendMsgDesc = createSendMsgDesc(descVal, exDescVal, true, true, NULL, NULL);

    MUST_BE_TRUE(sendMsgDesc->MessageLength() == numSrc0, "message length mismatch for raw sends");
    MUST_BE_TRUE(sendMsgDesc->ResponseLength() <= numDst, "response length mismatch for raw sends");
    MUST_BE_TRUE(sendMsgDesc->extMessageLength() <= numSrc1, "extended message length mismatch for raw sends");


    createSplitSendInst(
        predOpnd,
        (modifiers & 1) ? G4_sendsc : G4_sends,
        exsize,
        dstOpnd,
        src0->asSrcRegRegion(),
        src1->asSrcRegRegion(),
        msgDescOpnd,
        inst_opt,
        sendMsgDesc,
        temp_exdesc_src,
        0);

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISASamplerVAGenericInst(
    G4_Operand*   surface     , G4_Operand*   sampler     ,
    G4_Operand*   uOffOpnd  , G4_Operand*   vOffOpnd  ,
    G4_Operand*   vSizeOpnd , G4_Operand*   hSizeOpnd ,
    G4_Operand*   mmfMode    , unsigned char cntrl       ,
    unsigned char msgSeq     , VA_fopcode    fopcode     ,
    G4_DstRegRegion*   dstOpnd    , G4_Type       dstType    ,
    unsigned      dstSize,
    bool isBigKernel)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    G4_Declare* dcl  = Create_MRF_Dcl( 2 * GENX_SAMPLER_IO_SZ , Type_UD );
    G4_Declare *dcl1 = Create_MRF_Dcl( 8                      , Type_UD );
    G4_Declare *dclF = Create_MRF_Dcl( 8                      , Type_F  );
    dcl1->setAliasDeclare ( dcl, GENX_MRF_REG_SIZ );
    dclF->setAliasDeclare ( dcl, GENX_MRF_REG_SIZ );

    /// Message Sequence Setup:
    /// When Functionality is MINMAX/BoolCentroid/Centroid, value is binary 1x.
    switch (fopcode)
    {
    case       MINMAX_FOPCODE:
    case     Centroid_FOPCODE:
    case BoolCentroid_FOPCODE:
        msgSeq = 0x2;
        break;
    default:
        break; // Prevent gcc warning
    }

    /// Message Header Setup
    /// 19:18 output control format | 15 Alpha Write Channel Mask ARGB = 1101 = 0xD for sampler8x8
    unsigned msg_header = (cntrl << 18) + (0xD << 12);

    /// Media Payload Setup
    /// M1.7: 31:28 (Functionality) | 27 (IEF) | 26:25 (MSG_SEQ) | 24:23 (MMF_MODE) | 22:0 (Group ID Number)
    G4_Operand* mediaPayld_var = createImm(0, Type_UD);
    G4_Operand* mediaPayld_imm = NULL;

    if( fopcode ==  Convolve_FOPCODE )
    {
        mediaPayld_imm = createImm((((unsigned)fopcode) << 28)                   |
            (      0 << 27)                               |
            (msgSeq << 25)                               |
            (isBigKernel << 23), Type_UD);

    }
    else if( fopcode == MINMAX_FOPCODE || fopcode == MINMAXFILTER_FOPCODE )
    {
        mediaPayld_imm = createImm((((unsigned)fopcode) << 28)                   |
            (      0 << 27)                               |
            (msgSeq << 25)                               |
            (((mmfMode && mmfMode->isImm()) ?
            mmfMode->asImm()->getInt()    : 0) << 23), Type_UD);

        /// Support non-constant MMF_ENABLE parameters.
        /// Reuse for non-constant exec/control modes.
        if (mmfMode && !mmfMode->isImm())
        {
            G4_DstRegRegion  media_payload_dst( Direct, dcl1->getRegVar(), 0, 7, 1, Type_UD);
            mediaPayld_var = createSrcRegRegion(Mod_src_undef, Direct, dcl1->getRegVar(), 0, 7, getRegionScalar(), Type_UD);
            createInst( NULL, G4_shl, NULL, false, 1, createDstRegRegion(media_payload_dst), mmfMode, createImm(23, Type_UD), InstOpt_WriteEnable);
        }
    }
    else
    {
        mediaPayld_imm = createImm((((unsigned)fopcode) << 28)                   |
            (      0 << 27)                               |
            (msgSeq << 25)                               |
            (0x3 << 23), Type_UD);
    }

    /// Message Descriptor Setup
    unsigned msg_descriptor = (0x3 << 17) + (0xB  << 12);

    Create_MOVR0_Inst(dcl, 0, 0, true);
    Create_MOV_Inst(dcl, 0, 2, 1, NULL, NULL, createImm(msg_header, Type_UD), true); /// mov msg_header
	if (hasBindlessSampler())
	{
		// clear M0.3 bit 0 (sampler state base address select)
		// and (1) M0.3<1>:ud M0.3<0;1,0>:ud 0xFFFFFFFE:ud
		G4_SrcRegRegion* src0 = createSrcRegRegion(Mod_src_undef, Direct, dcl->getRegVar(), 0, 3,
			getRegionScalar(), Type_UD);
		G4_Imm* src1 = createImm(0xFFFFFFFE, Type_UD);
		G4_DstRegRegion* dst = createDstRegRegion(Direct, dcl->getRegVar(), 0, 3, 1, Type_UD);
		(void) createInst(nullptr, G4_and, nullptr, false, 1, dst, src0, src1, InstOpt_WriteEnable);
	}
    Create_MOV_Inst   (dcl1, 0, 0, 8, NULL, NULL, createImm(0, Type_UD), true); /// zero out
    Create_MOV_Inst   (dclF, 0, 2, 1, NULL, NULL, uOffOpnd, true); /// mov u opnd
    Create_MOV_Inst   (dclF, 0, 3, 1, NULL, NULL, vOffOpnd, true); /// mov v opnd
    Create_ADD_Inst   (dcl1, 0, 7, 1, NULL, NULL, mediaPayld_var, mediaPayld_imm, InstOpt_WriteEnable); /// store payload bits
	G4_SrcRegRegion* src = createSrcRegRegion(Mod_src_undef, Direct, dcl1->getRegVar(), 0, 7,
		getRegionScalar(), Type_UD);

	Create_ADD_Inst(dcl1, 0, 7, 1, NULL, NULL, src,
		Create_Src_Opnd_From_Dcl(builtinHWTID, getRegionScalar()), InstOpt_WriteEnable);
    // later phases need FFTID
    preDefVars.setHasPredefined(PreDefinedVarsInternal::HW_TID, true);
    /// M1.0: [DevBDW+] Function = Centroid/BoolCentroid v/h direction size.
    if (vSizeOpnd)
    {
        G4_Operand* h_sz_shl_opnd = NULL;

        if (!hSizeOpnd || hSizeOpnd->isImm())
            h_sz_shl_opnd = createImm((hSizeOpnd ? (hSizeOpnd->asImm()->getInt() << 4) : 0), Type_UD);
        else
        {
            h_sz_shl_opnd = createSrcRegRegion(Mod_src_undef, Direct, dcl1->getRegVar(), 0, 0, getRegionScalar(), Type_UD);
            G4_DstRegRegion* temp_dst = createDstRegRegion(Direct, dcl1->getRegVar(), 0, 0, 1, Type_UD);
            createInst(NULL, G4_shl, NULL, false, 1, temp_dst, hSizeOpnd,
				createImm(4, Type_UD), InstOpt_WriteEnable);
        }
        Create_ADD_Inst(dcl1, 0, 0, 1, NULL, NULL, vSizeOpnd, h_sz_shl_opnd, InstOpt_WriteEnable);
    }

    G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
    G4_DstRegRegion* post_dst = Check_Send_Dst(dstOpnd->asDstRegRegion());
    int reg_receive = dstSize/GENX_GRF_REG_SIZ;
    if(reg_receive < 1)
        reg_receive = 1;
    last_inst = Create_Send_Inst_For_CISA(NULL, post_dst, payload, 2, reg_receive, 8,
        msg_descriptor, SFID_SAMPLER, 0, 1, true, false, surface, sampler, InstOpt_WriteEnable, false);
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

/*
* Translates Sampler API intrinsic.
*output matrix, ChannelMask, SurfaceIndex, SamplerIndex, u, v, deltaU, deltaV
*u2d, OutputFormatControl=0, v2d=0.0, AVSExecMode=0, EIFbypass=false
* sample8x8AVS(matrix<unsigned short, N, 64> &M, samplerType,  channelMask, surfIndex, samplerIndex, u, v, deltaU, deltaV, u2d,
OutputFormatControl=0, v2d, AVSExecMode, EIFbypass=false);
*
* Assuming: N = 4, channelMask=CM_ABGR_ENABLE, surfIndex = 0x21, samplerIndex = 0x4,
*           then the generated code should look like the following for GT:
*
* .declare  VX Base=m ElementSize=4 Type=ud Total=16
* .declare  VA Base=m ElementSize=4 Type=f Total=8  ALIAS(VX,8)
* .declare  VY Base=r ElementSize=2 Type=uw Total=256
*
* mov  (8)     VX(0,0)<1>,  r0:ud
* mov  (1)     VX(0,2)<1>,  0 channel mask [12,15], output format control [16,17] 0 
* mov  (1)     VA(0,0)<1>,  v2d
* mov  (1)     VA(0,1)<1>,  vertical block number
* mov  (1)     VA(0,2)<1>,  u
* mov  (1)     VA(0,3)<1>,  v
* mov  (1)     VA(0,4)<1>,  deltaU
* mov  (1)     VA(0,5)<1>,  deltaV
* mov  (1)     VA(0,6)<1>,  u2d
* mov  (1)     VA(0,7)<1>,
[0:22]  GroupID
[23:24] Reserved
[25:26] 1x - 16x8
0x - 16x4
[27]    EIF Bypass
[28:31] 0000 - AVS Scaling
* send (16)    VY(0,0)<1>,  VX(0,0),    0x2,   0x048bc421
* mov  (256)   M(0,0)<1>,   VY(0,0)
*
* VX: message header
*
* VA: SIMD32 media payload
*
* ex_desc: 0x2 == 0010 (Target Function ID: Sampling Engine)
*
* desc: 0x050EB000 == Bit 31-29: 000 (Reserved)
*                     Bit 28-25: 0010 (Message Length = 2)
*                     Bit 24-20: 10000 (Response Message Length = 16)
*                     Bit 19:    1 (Header present)
*                     Bit 18-17: 11 (SIMD Mode = SIMD32/64)
*                     Bit 16-12: 01011 (Message Type = sample8x8 Media layout)
*                     Bit 11-8:  0000 + samplerIndex  (Sampler Index)
*                     Bit 7-0:   00000000 + surfIndex (Binding Table Index)
*
*/
int IR_Builder::translateVISAAvsInst(
    G4_Operand* surface,
    G4_Operand* sampler,
    ChannelMask channel,
    unsigned numEnabledChannels,
    G4_Operand* deltaUOpnd,
    G4_Operand* uOffOpnd,
    G4_Operand* deltaVOpnd,
    G4_Operand* vOffOpnd,
    G4_Operand* u2dOpnd,
    G4_Operand* groupIDOpnd,
    G4_Operand* verticalBlockNumberOpnd,
    unsigned char cntrl,
    G4_Operand* v2dOpnd,
    unsigned char execMode,
    G4_Operand* eifbypass,
    G4_DstRegRegion* dstOpnd )
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    {
        /*
        * mov  (8)     VX(0,0)<1>,  r0:ud
        * mov  (1)     VX(0,2)<1>,  0 channel mask [12,15], output format control [16,17] 0
        * mov  (1)     VA(0,0)<1>,  v2d
        * mov  (1)     VA(0,1)<1>,  vertical block number
        * mov  (1)     VA(0,2)<1>,  u
        * mov  (1)     VA(0,3)<1>,  v
        * mov  (1)     VA(0,4)<1>,  deltaU
        * mov  (1)     VA(0,5)<1>,  deltaV
        * mov  (1)     VA(0,6)<1>,  u2d
        * mov  (1)     VA(0,7)<1>,
        [0:22]  GroupID
        [23:24] Reserved
        [25:26] 1x - 16x8
        0x - 16x4
        [27]    EIF Bypass
        [28:31] 0000 - AVS Scaling
        */
        unsigned int number_elements_returned = 64;
        G4_Type output_type = Type_UW;

        if(cntrl > 1)
            output_type = Type_UB;


        if (execMode == CM_AVS_16x8)
        {
            number_elements_returned = 128;
            numEnabledChannels *= 2;
        }

        if (execMode == CM_AVS_8x4)
        {
            number_elements_returned = 32;
        }

        if (execMode == CM_AVS_4x4)
        {
            number_elements_returned = 16;
        }

        unsigned obj_size = number_elements_returned*numEnabledChannels*G4_Type_Table[output_type].byteSize;
        // mov (8)      VX(0,0)<1>,  r0:ud
        // add dcl for VX
        G4_Declare *dcl = Create_MRF_Dcl( 2 * GENX_SAMPLER_IO_SZ, Type_UD );

        // mov  VX(0,0)<1>, r0
        Create_MOVR0_Inst( dcl, 0, 0, true );
        /* mov (1)     VX(0,2)<1>,   0  */
        unsigned cmask = channel.getHWEncoding() << 12;
        cmask += cntrl<<18;
        Create_MOV_Inst( dcl, 0, 2, 1, NULL, NULL, createImm( cmask, Type_UD ), true );

        G4_Declare *dcl1 = Create_MRF_Dcl( 8, Type_F );
        dcl1->setAliasDeclare(dcl, GENX_MRF_REG_SIZ);

        /*
        Keeping destination type as UD, otherwise w-->f conversion happens,
        which affects the results.
        */
        G4_Declare *dcl1_ud = Create_MRF_Dcl( 8, Type_UD );
        dcl1_ud->setAliasDeclare(dcl, GENX_MRF_REG_SIZ);

        // mov  (1)     VA(0,0)<1>,  v2d
        Create_MOV_Inst(dcl1, 0, 0, 1, NULL, NULL, v2dOpnd, true);

        // mov  (1)     VA(0,1)<1>,  vertical block number
        Create_MOV_Inst( dcl1_ud, 0, 1, 1, NULL, NULL, verticalBlockNumberOpnd, true );
        // mov  (1)     VA(1,2)<1>,  u
        Create_MOV_Inst( dcl1, 0, 2, 1, NULL, NULL, uOffOpnd, true );
        // mov  (1)     VA(1,3)<1>,  v
        Create_MOV_Inst( dcl1, 0, 3, 1, NULL, NULL, vOffOpnd, true );
        // mov  (1)     VA(1,4)<1>,  deltaU
        Create_MOV_Inst( dcl1, 0, 4, 1, NULL, NULL, deltaUOpnd, true );
        // mov  (1)     VA(1,5)<1>,  deltaV
        Create_MOV_Inst( dcl1, 0, 5, 1, NULL, NULL, deltaVOpnd, true );
        // mov  (1)     VA(0,6)<1>,  U2d
        Create_MOV_Inst( dcl1, 0, 6, 1, NULL, NULL, u2dOpnd, true );

        {
            /*
            [23:24] Reserved
            [25:26] 1x - 16x8
            0x - 16x4
            [27]    EIF Bypass
            [28:31] 0000 - AVS Scaling
            */
            unsigned int upper_bits = 0;
            upper_bits += execMode << 25;

            if (eifbypass->isImm())
            {
                upper_bits += (eifbypass->asImm()->getInt() & 1) << 27;

                G4_DstRegRegion* dst2_opnd = createDstRegRegion( Direct, dcl1_ud->getRegVar(), 0, 7, 1, Type_UD );
                createInst( NULL, G4_add, NULL, false, 1, dst2_opnd, groupIDOpnd, createImm(upper_bits, Type_UD), 0, InstOpt_WriteEnable );
            }
            else
            {
                // extract lsb of eifbypass
                G4_DstRegRegion* dst2_opnd = createDstRegRegion( Direct, dcl1_ud->getRegVar(), 0, 7, 1, Type_UD );
                createInst( NULL, G4_and, NULL, false, 1, dst2_opnd, eifbypass, createImm(1, Type_UD), 0, InstOpt_WriteEnable );

                // eifbypass << 27
                G4_SrcRegRegion* src2_opnd = createSrcRegRegion( Mod_src_undef, Direct, dcl1_ud->getRegVar(), 0, 7, getRegionScalar(), dcl1_ud->getElemType() );
                G4_DstRegRegion* dst3_opnd = createDstRegRegion( Direct, dcl1_ud->getRegVar(), 0, 7, 1, Type_UD );
                createInst( NULL, G4_shl, NULL, false, 1, dst3_opnd, src2_opnd, createImm(27, Type_UD), 0, InstOpt_WriteEnable);

                // upper_bits + (eifbypass << 27)
                G4_SrcRegRegion* src3_opnd = createSrcRegRegion( Mod_src_undef, Direct, dcl1_ud->getRegVar(), 0, 7, getRegionScalar(), dcl1_ud->getElemType() );
                G4_DstRegRegion* dst4_opnd = createDstRegRegion( Direct, dcl1_ud->getRegVar(), 0, 7, 1, Type_UD );
                createInst( NULL, G4_add, NULL, false, 1, dst4_opnd, src3_opnd, createImm(upper_bits, Type_UD), 0, InstOpt_WriteEnable);

                G4_DstRegRegion* dst5_opnd = createDstRegRegion( Direct, dcl1_ud->getRegVar(), 0, 7, 1, Type_UD );
                G4_SrcRegRegion* src_opnd = createSrcRegRegion( Mod_src_undef, Direct, dcl1_ud->getRegVar(), 0, 7, getRegionScalar(), dcl1_ud->getElemType() );
                createInst( NULL, G4_add, NULL, false, 1, dst5_opnd, groupIDOpnd, src_opnd, 0, InstOpt_WriteEnable );

            }
        }

        /*
        * desc: 0x050EB000 == Bit 31-29: 000 (Reserved)
        *                     Bit 28-25: 0010 (Message Length = 2)
        *                     Bit 24-20: 10000 (Response Message Length = 16)
        *                     Bit 19:    1 (Header present)
        *                     Bit 18-17: 11 (SIMD Mode = SIMD32/64)
        *                     Bit 16-12: 01011 (Message Type = sample8x8 Media layout)
        *                     Bit 11-8:  0000 + samplerIndex  (Sampler Index)
        *                     Bit 7-0:   00000000 + surfIndex (Binding Table Index)
        */

        // Set bit 9-8 for the message descriptor
        unsigned temp = 0;
        temp += 0xB << 12;  // Bit 15-12 = 1100 for Sampler Message Type
        temp += 0x3 << 17;  // Bit 17-16 = 11 for SIMD32 mode

        // send's operands preparation
        // create a currDst for VX
        G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());

        G4_DstRegRegion* d = Check_Send_Dst( dstOpnd->asDstRegRegion());

        // dst is already UW
        G4_INST *send_inst = Create_Send_Inst_For_CISA(
            NULL,
            d,
            payload,
            2,
            obj_size/GENX_GRF_REG_SIZ,
            16,
            temp,
            SFID_SAMPLER,
            0,
            1,
            true,
            false,
            surface,
            sampler,
            InstOpt_WriteEnable,
            false);
        last_inst = send_inst;
    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISADataMovementInst(ISA_Opcode opcode,
                                              CISA_MIN_MAX_SUB_OPCODE subOpcode,
                                              G4_Predicate *predOpnd,
                                              Common_ISA_Exec_Size executionSize,
                                              Common_VISA_EMask_Ctrl emask,
                                              bool saturate,
                                              G4_DstRegRegion *dstOpnd,
                                              G4_Operand *src0Opnd,
                                              G4_Operand *src1Opnd)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    uint8_t exsize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    unsigned int inst_opt = Get_Gen4_Emask(emask, exsize);
    G4_CondMod* condMod = NULL;

    if( opcode == ISA_MOVS )
    {
        if (src0Opnd->isSrcRegRegion())
            src0Opnd->asSrcRegRegion()->setType(Type_UD);
        dstOpnd->setType(Type_UD);
        last_inst = createInst(
            predOpnd,
            G4_mov,
            NULL,
            false,
            exsize,
            dstOpnd,
            src0Opnd,
            NULL,
            inst_opt,
            0);
    }
    else if( opcode == ISA_SETP )
    {
        // Src0 must have integer type.  If src0 is a general or indirect operand,
        // the LSB in each src0 element determines the corresponding dst element's Bool value.
        // If src0 is an immediate operand, each of its bits from the LSB to MSB is used
        // to set the Bool value in the corresponding dst element.
        // Predication is not supported for this instruction.


        /*
        * 1. Mask operand is const or scalar
        *   mov (1) f0.0 src {NoMask}
        * 2. Mask operand is stream.
        *   and.nz.f0.0 (n) null src 0x1:uw
        */

        // vISA spec does not allow 1 as the execution size anymore.
        // This is a hack to allow execution size 1
        // and we make sure it is a scalar region in this case.
        if (getOptions()->isTargetCM())
        {
            if (exsize == 1 && src0Opnd->isSrcRegRegion())
            {
                G4_SrcRegRegion *region = src0Opnd->asSrcRegRegion();
                if (!region->isScalar())
                    region->setRegion(getRegionScalar());
            }
        }

        if (src0Opnd->isImm() || (src0Opnd->isSrcRegRegion() &&
                                 (src0Opnd->asSrcRegRegion()->isScalar())))
        {
            dstOpnd->setType(exsize == 32 ? Type_UD: Type_UW);
			if (emask == vISA_EMASK_M5_NM)
			{
				// write to f0.1/f1.1 instead
				MUST_BE_TRUE(dstOpnd->getTopDcl()->getNumberFlagElements() == 32, "Dst must have 32 flag elements");
				dstOpnd->setSubRegOff(1);
			}
            last_inst = createInst(
                predOpnd,
                G4_mov,
                NULL,
                saturate,
                1,
                dstOpnd,
                src0Opnd,
                NULL,
                InstOpt_WriteEnable,
                0);
        }
        else if( src0Opnd->isSrcRegRegion() && src0Opnd->asSrcRegRegion()->isScalar() == false )
        {
            G4_DstRegRegion *null_dst_opnd = createNullDst(Type_UD);
            condMod = createCondMod(
                Mod_ne,
                dstOpnd->asDstRegRegion()->getBase()->asRegVar(),
                0);

            last_inst = createInst(
                predOpnd,
                G4_and,
                condMod,
                saturate,
                exsize,
                null_dst_opnd,
                src0Opnd,
                createImm( 1, Type_UW ),
                inst_opt,
                0);
        }
        else
        {
            return CM_FAILURE;
        }
    }
    else
    {
        if ( opcode == ISA_FMINMAX )
        {
            condMod = createCondMod(
                subOpcode == CISA_DM_FMAX ? Mod_ge : Mod_l,
                nullptr,
                0);
        }

        if (opcode == ISA_MOV && src0Opnd->isSrcRegRegion() && src0Opnd->asSrcRegRegion()->isFlag())
        {
            // src0 is a flag
            // mov (1) dst src0<0;1:0>:uw (ud if flag has 32 elements)
            G4_Declare* flagDcl = src0Opnd->getTopDcl();
            src0Opnd->asSrcRegRegion()->setType(flagDcl->getNumberFlagElements() > 16 ? Type_UD : Type_UW);
        }

        last_inst = createInst(
            predOpnd,
            Get_G4_Opcode_From_Common_ISA_Opcode(opcode),
            condMod,
            saturate,
            exsize,
            dstOpnd,
            src0Opnd,
            src1Opnd,
            inst_opt,
            0);
    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

/*
* Translates Sampler intrinsic.
*
* Assuming: N = 4, channelMask=CM_ABGR_ENABLE, surfIndex = 0x21, samplerIndex = 0x4,
*           then the generated code should look like the following for GT:
*
* .declare  VX Base=m ElementSize=4 Type=f Total=72
* .declare  VY Base=r ElementSize=4 Type=f Total=64
* .declare  VZ Base=r ElementSize=2 Type=w Total=128 ALIAS(VY,0)
*
* mov  (8)     VX(0,0)<1>,  r0:ud
* mov  (1)     VX(0,2)<1>,  0
* mov  (16)    VX(1,0)<1>,  u
* mov  (16)    VX(3,0)<1>,  v
* mov  (16)    VX(5,0)<1>,  r
* mov  (16)    VX(7,0)<1>,  0
* send (16)    VY(0,0)<1>,  VX(0,0),    0x2,  0x128a0421
* mov  (64)    M(0,0)<1>,   VY(0,0)
*
* ex_desc: 0x2 == 0010 (Target Function ID: Sampling Engine)
*
* desc: 0x128a0421 == Bit 31-29: 000 (Reserved)
*                     Bit 28-25: 1001 (Message Length = 9 (1+2*4 for SIMD16))
*                     Bit 24-20: 01000 (Response Message Length = 8)
*                     Bit 19:    1 (Header present)
*                     Bit 18:    0 (Reserved)
*                     Bit 17-16: 10 (SIMD Mode = SIMD16)
*                     Bit 15-12: 0000 (Message Type = Sample)
*                     Bit 11-8:  0000 + samplerIndex  (Sampler Index)
*                     Bit 7-0:   00000000 + surfIndex (Binding Table Index)
*
*/
int IR_Builder::translateVISASamplerInst(
    unsigned simdMode,
    G4_Operand* surface,
    G4_Operand* sampler,
    ChannelMask channel,
    unsigned numEnabledChannels,
    G4_Operand* uOffOpnd,
    G4_Operand* vOffOpnd,
    G4_Operand* rOffOpnd,
    G4_DstRegRegion* dstOpnd )

{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    // mov (8)      VX(0,0)<1>,  r0:ud
    // add dcl for VX
    unsigned num_payload_elt = simdMode/2 * GENX_MRF_REG_SIZ/G4_Type_Table[Type_UD].byteSize;
    G4_Declare *dcl = Create_MRF_Dcl( num_payload_elt + GENX_SAMPLER_IO_SZ, Type_UD );

    // mov  VX(0,0)<1>, r0
    Create_MOVR0_Inst( dcl, 0, 0 );
    unsigned cmask = channel.getHWEncoding() << 12;
    /* mov (1)     VX(0,2)<1>,   0  */
    Create_MOV_Inst( dcl, 0, 2, 1, NULL, NULL, createImm( cmask, Type_UD ) );

    // set up the message payload
    // lod is always uninitialized for us as we don't support it.
    G4_Declare *dcl1 = Create_MRF_Dcl( num_payload_elt, Type_UD );
    dcl1->setAliasDeclare(dcl, GENX_MRF_REG_SIZ);
    /* mov  (sample_mode)    VX(0,0)<1>,  u */
    Create_MOV_Send_Src_Inst( dcl1, 0, 0, simdMode, uOffOpnd, 0 );
    if( sampler == NULL )
    {
        // ld
        if (getGenxPlatform() < GENX_SKL)
        {
            // the order of paramters is
            // u    lod        v    r
            /* mov  (sample_mode)    VX(sample_mode/8, 0)<1>,  lod */
            Create_MOV_Send_Src_Inst( dcl1, simdMode/8, 0, simdMode, createImm( 0, Type_UD ), 0 );
            /* mov  (sample_mode)    VX(2*sample_mode/8, 0)<1>,  v */
            Create_MOV_Send_Src_Inst( dcl1, 2*simdMode/8, 0, simdMode, vOffOpnd, 0 );
            /* mov  (sample_mode)    VX(3*sampler_mode/8, 0)<1>,  r */
            Create_MOV_Send_Src_Inst( dcl1, 3*simdMode/8, 0, simdMode, rOffOpnd, 0 );
        }
        else
        {
            // SKL+: the order of paramters is
            // u    v   lod r
            /* mov  (sample_mode)    VX(sample_mode/8, 0)<1>,  v */
            Create_MOV_Send_Src_Inst( dcl1, simdMode/8, 0, simdMode, vOffOpnd, 0 );
            /* mov  (sample_mode)    VX(2*sample_mode/8, 0)<1>,  lod */
            Create_MOV_Send_Src_Inst( dcl1, 2*simdMode/8, 0, simdMode, createImm( 0, Type_UD ), 0 );
            /* mov  (sample_mode)    VX(3*sampler_mode/8, 0)<1>,  r */
            Create_MOV_Send_Src_Inst( dcl1, 3*simdMode/8, 0, simdMode, rOffOpnd, 0 );
        }
    }
    else
    {
        // sample
        /* mov  (sample_mode)    VX(1 + sample_mode/8, 0)<1>,  v */
        Create_MOV_Send_Src_Inst( dcl1, simdMode/8, 0, simdMode, vOffOpnd, 0 );
        /* mov  (sample_mode)    VX(3,0)<1>,  r */
        Create_MOV_Send_Src_Inst( dcl1, 2*simdMode/8, 0, simdMode, rOffOpnd, 0 );
        /* mov  (sample_mode)    VX(5,0)<1>,  0 */
        Create_MOV_Send_Src_Inst( dcl1, 3*simdMode/8, 0, simdMode, createImm( 0, Type_UD ), 0 );
    }
    // send's operands preparation
    // create a currDst for VX
    G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());

    G4_DstRegRegion* d = Check_Send_Dst( dstOpnd->asDstRegRegion());

    // Set bit 9-8 for the message descriptor
    unsigned temp = 0;

    //Bit 17-18 = 10 for SIMD mode
    if (simdMode == 8)
    {
        temp += 0x1 << 17;
    }
    else
    {
        temp += 0x2 << 17;
    }

    if( sampler == NULL )
    {
#define SAMPLER_MESSAGE_TYPE_OFFSET    12
        //LD message
        temp += VISASampler3DSubOpCode::VISA_3D_LD << SAMPLER_MESSAGE_TYPE_OFFSET;
    }

    if(simdMode == 16) {
        // redefine the type and offset of post dst.
        if( (d->getType() != Type_W) &&
            (d->getType() != Type_UW) ) {
                short new_SubRegOff = dstOpnd->asDstRegRegion()->getSubRegOff();
                if( dstOpnd->getRegAccess() == Direct ){
                    new_SubRegOff = (dstOpnd->asDstRegRegion()->getSubRegOff() * G4_Type_Table[dstOpnd->getType()].byteSize) / G4_Type_Table[Type_W].byteSize;
                }
                G4_DstRegRegion new_dst(
                    dstOpnd->getRegAccess(),
                    dstOpnd->asDstRegRegion()->getBase(),
                    dstOpnd->asDstRegRegion()->getRegOff(),
                    new_SubRegOff,
                    1,
                    Type_W);
                d = createDstRegRegion( new_dst );
        }
    }

    G4_INST *send_inst = Create_Send_Inst_For_CISA(
        NULL,
        d,
        payload,
        1 + simdMode/2,
        ((simdMode == 8)?32:(numEnabledChannels*16))*G4_Type_Table[Type_F].byteSize/GENX_GRF_REG_SIZ,
        simdMode,
        temp,
        SFID_SAMPLER,
        0,
        1,
        true,
        false,
        surface,
        sampler,
        0,
        false);
    last_inst = send_inst;
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISAVaSklPlusGeneralInst(
    ISA_VA_Sub_Opcode sub_opcode,
    G4_Operand* surface                 , G4_Operand* sampler     , unsigned char mode, unsigned char functionality,
    G4_Operand* uOffOpnd              , G4_Operand* vOffOpnd  ,

    //1pixel convolve
    G4_Operand * offsetsOpnd,

    //FloodFill
    G4_Operand* loopCountOpnd         , G4_Operand* pixelHMaskOpnd ,
    G4_Operand* pixelVMaskLeftOpnd    , G4_Operand* pixelVMaskRightOpnd ,

    //LBP Correlation
    G4_Operand* disparityOpnd          ,

    //Correlation Search
    G4_Operand* verticalOriginOpnd    , G4_Operand* horizontalOriginOpnd  ,
    G4_Operand* xDirectionSizeOpnd   , G4_Operand* yDirectionSizeOpnd   ,
    G4_Operand* xDirectionSearchSizeOpnd , G4_Operand* yDirectionSearchSizeOpnd ,

    G4_DstRegRegion* dstOpnd    , G4_Type dstType    , unsigned dstSize ,

    //HDC
    unsigned char pixelSize            , G4_Operand* dstSurfaceOpnd ,
    G4_Operand *dstXOpnd              , G4_Operand* dstYOpnd,
    bool hdcMode)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    G4_Declare* dcl  = NULL;
    G4_Declare *dcl_offsets = NULL;

    unsigned int reg_to_send = 2;
    //for offsets
    if((sub_opcode == VA_OP_CODE_1PIXEL_CONVOLVE && mode == CM_CONV_16x1) ||
        sub_opcode == ISA_HDC_1PIXELCONV)
    {
        dcl = Create_MRF_Dcl( 4 * GENX_SAMPLER_IO_SZ , Type_UD );
        //16 pairs of x,y coordinates
        dcl_offsets = Create_MRF_Dcl( 32                      , Type_W  );
        dcl_offsets->setAliasDeclare( dcl, GENX_MRF_REG_SIZ * 2 );
        reg_to_send = 4;
    }
    else
        dcl = Create_MRF_Dcl( 2 * GENX_SAMPLER_IO_SZ , Type_UD );

    G4_Declare *dcl_payload_UD = Create_MRF_Dcl( 8                      , Type_UD );
    G4_Declare *dcl_payload_F = Create_MRF_Dcl( 8                      , Type_F  );
    G4_Declare *dcl_payload_UW = Create_MRF_Dcl( 16                      , Type_UW  );

    dcl_payload_UD->setAliasDeclare ( dcl,  GENX_MRF_REG_SIZ );
    dcl_payload_F->setAliasDeclare ( dcl, GENX_MRF_REG_SIZ );
    dcl_payload_UW->setAliasDeclare ( dcl, GENX_MRF_REG_SIZ );

    /// Message Header Setup
    /// 19:18 output control format | 15 Alpha Write Channel Mask ARGB = 1101 = 0xD for sampler8x8
    unsigned msg_header = (0xD << 12);

    //if MMF based on pixel size set output format control.
    if( sub_opcode == ISA_HDC_MMF && pixelSize )
    {
        msg_header = msg_header + (0x2 << 18);
    }

    //I guess this is still needed just to be sure payload is really initiazlied.
    //since full register initalization is conservative some registers
    //can still be not initialized and then used for payload
    if( m_options->getOption(vISA_InitPayload) )
    {
        Create_MOV_Inst( dcl_payload_UD, 0, 0, 8, NULL, NULL, createImm( 0, Type_UD ) );
    }
    // mov  VX(0,0)<1>, r0
    Create_MOVR0_Inst( dcl, 0, 0 );
    Create_MOV_Inst( dcl, 0, 2, 1, NULL, NULL, createImm( msg_header, Type_UD ) );

    //set dst BTI, In M0.2 bits 24:31
    if(hdcMode)
    {
        G4_Declare *dcl_temp = createDeclareNoLookup(
            "tmp_shl_dst_bti",
            G4_GRF ,
            1,
            1,
            Type_UD );

        //Creating dst of the shift to be used in shift instruction
        //Creating src of src to use in the subsequent add instruction
        G4_Operand* shift_immed = createSrcRegRegion(Mod_src_undef, Direct, dcl_temp->getRegVar(), 0, 0, getRegionScalar(), Type_UD);
        G4_DstRegRegion* temp_dst = createDstRegRegion(Direct, dcl_temp->getRegVar(), 0, 0,1, Type_UD);

        //creating a src and for m0.2
        G4_SrcRegRegion* m0_2_src = createSrcRegRegion(Mod_src_undef,Direct,dcl->getRegVar(), 0, 2, getRegionScalar(), Type_UD);
        G4_DstRegRegion* m0_2_dst = createDstRegRegion(Direct, dcl->getRegVar(), 0, 2, 1, Type_UD);



        createInst(NULL, G4_shl, NULL, false, 1, temp_dst, dstSurfaceOpnd, createImm(24, Type_UD), 0);

        createInst(NULL, G4_add, NULL, false, 1, m0_2_dst, m0_2_src, shift_immed, 0);
    }

    /*


    set x_offset In M0.4 0:15
    set y_offset In M0.4 16:31
    */
    if(hdcMode)
    {
        G4_Declare *dcl_temp = createDeclareNoLookup(
            "tmp_shl_y_offset",
            G4_GRF ,
            1,
            1,
            Type_UD );

        //Creating dst of the shift to be used in shift instruction
        //Creating src of src to use in the subsequent add instruction
        G4_Operand * shift_immed = createSrcRegRegion(Mod_src_undef, Direct, dcl_temp->getRegVar(), 0, 0, getRegionScalar(), Type_UD);
        G4_DstRegRegion* temp_dst = createDstRegRegion(Direct, dcl_temp->getRegVar(), 0, 0,1, Type_UD);

        //creating a src and for m0.4
        G4_DstRegRegion* m0_4_dst = createDstRegRegion(Direct, dcl->getRegVar(), 0, 4, 1, Type_UD);


        createInst(NULL, G4_shl, NULL, false, 1, temp_dst, dstYOpnd, createImm(16, Type_UD), 0);

        createInst(NULL, G4_add, NULL, false, 1, m0_4_dst, dstXOpnd, shift_immed, 0);
    }

    //set dst surface format based on pixel size M0.5 0:4
    if(hdcMode)
    {
        int surface_format = 0;
        if( pixelSize == 0 )
        {
            surface_format = 6; //PLANAR_Y16_SNORM
        }else if( pixelSize == 1 )
        {
            surface_format = 5; //PLANAR_Y8_UNORM
        }else
        {
            ASSERT_USER(false,
                "Invalid surface format for SKL+ VA HDC");
        }
        Create_MOV_Inst( dcl, 0, 5, 1, NULL, NULL, createImm( surface_format, Type_UD ) );
    }

    //setting M2.1 vertical  block offset to 0
    //for LBP correlation setting M2.0 to 0, since only upper 16 bits are set
    //later by adding to shl result
    Create_MOV_Inst( dcl_payload_UD, 0, 1, 1, NULL, NULL, createImm( 0, Type_UD ) );

    //setting up M1.7

    unsigned int m1_7 = IR_Builder::sampler8x8_group_id++;

    ISA_VA_Sub_Opcode originalSubOpcode = sub_opcode;

    /*
    HDC uses the same sub opcodes as regular VA,
    but with return register set to 0.
    */
    switch(sub_opcode)
    {
    case ISA_HDC_CONV:
        sub_opcode = Convolve_FOPCODE;
        break;
    case ISA_HDC_MMF:
        sub_opcode = MINMAXFILTER_FOPCODE;
        break;
    case ISA_HDC_ERODE:
        sub_opcode = ERODE_FOPCODE;
        break;
    case ISA_HDC_DILATE:
        sub_opcode = Dilate_FOPCODE;
        break;
    case ISA_HDC_LBPCORRELATION:
        sub_opcode = VA_OP_CODE_LBP_CORRELATION;
        break;
    case ISA_HDC_LBPCREATION:
        sub_opcode = VA_OP_CODE_LBP_CREATION;
        break;
    case ISA_HDC_1DCONV_H:
        sub_opcode = VA_OP_CODE_1D_CONVOLVE_HORIZONTAL;
        break;
    case ISA_HDC_1DCONV_V:
        sub_opcode = VA_OP_CODE_1D_CONVOLVE_VERTICAL;
        break;
    case ISA_HDC_1PIXELCONV:
        sub_opcode = VA_OP_CODE_1PIXEL_CONVOLVE;
        break;
    default:
        break; // Prevent gcc warning
    }
    //setting VA operation
    m1_7 |= (unsigned int)sub_opcode<<28;

    //setting IEF bypass to 1
    m1_7 |= 0x1<<27;

    //setting message sequence
    m1_7 |= (mode & 0x3) << 25;

    //setting functionality
    m1_7 |= (functionality & 0x3) << 23;
    Create_MOV_Inst( dcl_payload_UD, 0, 7, 1, NULL, NULL, createImm( m1_7, Type_UD ) );

    /*
    case VA_OP_CODE_1D_CONVOLVE_HORIZONTAL:
    case VA_OP_CODE_1D_CONVOLVE_VERTICAL:
    case VA_OP_CODE_1PIXEL_CONVOLVE:
    case VA_OP_CODE_FLOOD_FILL:
    case VA_OP_CODE_LBP_CREATION:
    case VA_OP_CODE_LBP_CORRELATION:
    case VA_OP_CODE_CORRELATION_SEARCH:
    */

    //setting m1_5 and m1_4
    if(sub_opcode == VA_OP_CODE_CORRELATION_SEARCH)
    {
        Create_MOV_Inst( dcl_payload_F, 0, 5, 1, NULL, NULL, verticalOriginOpnd );
        Create_MOV_Inst( dcl_payload_F, 0, 4, 1, NULL, NULL, horizontalOriginOpnd );
    }

    //setting m1_3
    if(vOffOpnd != NULL)
    {
        Create_MOV_Inst( dcl_payload_F, 0, 3, 1, NULL, NULL, vOffOpnd );
    }

    //setting m1_2
    if(uOffOpnd != NULL)
    {
        Create_MOV_Inst( dcl_payload_F, 0, 2, 1, NULL, NULL, uOffOpnd );
    }

    if(sub_opcode == VA_OP_CODE_FLOOD_FILL)
    {
        Create_MOV_Send_Src_Inst( dcl_payload_UD,0,2,5,pixelHMaskOpnd, 0 );
    }

    if( (sub_opcode == VA_OP_CODE_1PIXEL_CONVOLVE  && mode == CM_CONV_16x1) ||
        originalSubOpcode == ISA_HDC_1PIXELCONV)
    {
        RegionDesc *rd = getRegionStride1();
        G4_Operand *offsets_opnd_temp = createSrcRegRegion(
            Mod_src_undef,
            Direct,
            offsetsOpnd->asSrcRegRegion()->getBase(),
            0,
            0,
            rd,
            Type_W);

        Create_MOV_Inst(dcl_offsets,0,0,32,NULL, NULL, offsets_opnd_temp);
    }

    //creating temp for intermediate computations
    G4_Declare *dcl_temp = createDeclareNoLookup(
        "tmp_shl",
        G4_GRF ,
        1,
        1,
        Type_UD);
    G4_SrcRegRegion temp_src(Mod_src_undef,Direct,dcl_temp->getRegVar(), 0, 0, getRegionScalar(), Type_UD);
    G4_DstRegRegion temp_dst(Direct, dcl_temp->getRegVar(), 0, 0,1, Type_UD);

    //creating a src and for m1.0
    G4_SrcRegRegion m1_0_src(Mod_src_undef,Direct,dcl_payload_UD->getRegVar(), 0, 0, getRegionScalar(), Type_UD);
    G4_DstRegRegion m1_0_dst(Direct, dcl_payload_UD->getRegVar(), 0, 0, 1, Type_UD);

    G4_Operand * shift_immed = NULL;

    //setting m1_0
    switch(sub_opcode)
    {
    case VA_OP_CODE_FLOOD_FILL:
        {
            Create_MOV_Inst(dcl_payload_UD,0,0,1,NULL, NULL,pixelVMaskLeftOpnd);

            if(pixelVMaskRightOpnd->isImm())
            {
                shift_immed = createImm(pixelVMaskRightOpnd->asImm()->getInt() << 10,Type_UD);
                createInst(NULL, G4_mov, NULL, false, 1, createDstRegRegion(m1_0_dst), shift_immed, NULL, 0);
            }else
            {

                createInst(NULL, G4_shl, NULL, false, 1, createDstRegRegion(temp_dst), pixelVMaskRightOpnd, createImm(10, Type_UD), 0);
                shift_immed = createSrcRegRegion(temp_src);
                createInst(NULL, G4_add, NULL, false, 1, createDstRegRegion(m1_0_dst), createSrcRegRegion(m1_0_src), shift_immed, 0);
            }

            if(loopCountOpnd->isImm())
            {
                shift_immed = createImm(loopCountOpnd->asImm()->getInt() << 24, Type_UD);
            }else
            {
                createInst(NULL, G4_shl, NULL, false, 1, createDstRegRegion(temp_dst), loopCountOpnd, createImm(24, Type_UD), 0);
                shift_immed = createSrcRegRegion(temp_src);
            }
            createInst(NULL, G4_add, NULL, false, 1, createDstRegRegion(m1_0_dst), createSrcRegRegion(m1_0_src), shift_immed, 0);
            break;
        }
    case VA_OP_CODE_LBP_CORRELATION:
        {
            //setting disparity
            if(disparityOpnd->isImm())
            {
                shift_immed = createImm(disparityOpnd->asImm()->getInt() << 16, Type_UD);
                createInst(NULL, G4_mov, NULL, false, 1, createDstRegRegion(m1_0_dst), shift_immed, NULL, 0);
            }else
            {
                createInst(NULL, G4_shl, NULL, false, 1, createDstRegRegion(m1_0_dst), disparityOpnd, createImm(16, Type_UD), 0);
            }

            break;
        }
    case VA_OP_CODE_CORRELATION_SEARCH:
        {
            /*
            G4_Operand* verticalOriginOpnd    , G4_Operand* horizontalOriginOpnd  ,
            G4_Operand* xDirectionSizeOpnd   , G4_Operand* yDirectionSizeOpnd   ,
            G4_Operand* xDirectionSearchSizeOpnd , G4_Operand* yDirectionSearchSizeOpnd ,
            */
            Create_MOV_Inst(dcl_payload_UD,0,0,1,NULL, NULL, xDirectionSizeOpnd);

            //setting y-direction size of the source for correlation.
            if(yDirectionSizeOpnd->isImm())
            {
                shift_immed = createImm(yDirectionSizeOpnd->asImm()->getInt() << 4, Type_UD);
                createInst(NULL, G4_mov, NULL, false, 1, createDstRegRegion(m1_0_dst), shift_immed, NULL, 0);
            }else
            {
                createInst(NULL, G4_shl, NULL, false, 1, createDstRegRegion(temp_dst), yDirectionSizeOpnd, createImm(4, Type_UD), 0);
                shift_immed = createSrcRegRegion(temp_src);
                createInst(NULL, G4_add, NULL, false, 1, createDstRegRegion(m1_0_dst), createSrcRegRegion(m1_0_src), shift_immed, 0);
            }


            //31:16 reserved

            //setting x-direction search size
            if(xDirectionSearchSizeOpnd->isImm())
            {
                shift_immed = createImm(xDirectionSearchSizeOpnd->asImm()->getInt() << 8, Type_UD);
            }else
            {
                createInst(NULL, G4_shl, NULL, false, 1, createDstRegRegion(temp_dst), xDirectionSearchSizeOpnd, createImm(8, Type_UD), 0);
                shift_immed = createSrcRegRegion(temp_src);
            }
            createInst(NULL, G4_add, NULL, false, 1, createDstRegRegion(m1_0_dst), createSrcRegRegion(m1_0_src), shift_immed, 0);

            //setting y-direction search size.
            if(yDirectionSearchSizeOpnd->isImm())
            {
                shift_immed = createImm(yDirectionSearchSizeOpnd->asImm()->getInt() << 16, Type_UD);
            }else
            {
                createInst(NULL, G4_shl, NULL, false, 1, createDstRegRegion(temp_dst), yDirectionSearchSizeOpnd, createImm(16, Type_UD), 0);
                shift_immed = createSrcRegRegion(temp_src);
            }
            createInst(NULL, G4_add, NULL, false, 1, createDstRegRegion(m1_0_dst), createSrcRegRegion(m1_0_src), shift_immed, 0);

            break;
        }
    default:
        break; // Prevent gcc warning
    }

    G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
    G4_DstRegRegion* post_dst = NULL;

    unsigned int reg_to_receive = 0;

    if( !hdcMode )
    {
        post_dst = Check_Send_Dst(dstOpnd);
        if((dstSize %  GENX_GRF_REG_SIZ) != 0)
        {
            reg_to_receive = (unsigned int) std::ceil((double)dstSize/GENX_GRF_REG_SIZ);
        }else
        {
            reg_to_receive = dstSize/GENX_GRF_REG_SIZ;
        }
    }else
    {
        post_dst = createNullDst( Type_UD );
    }

    /// Message Descriptor Setup
    /// 18:17 SIMD Mode (SIMD32/64 = 3)  |  16:12 Message Type (sampler8x8 = 01011 = 0xB)
    unsigned msg_descriptor = (0x3 << 17) + (0xB  << 12);
    last_inst = Create_Send_Inst_For_CISA(NULL, post_dst, payload, reg_to_send, reg_to_receive, 8,
        msg_descriptor, SFID_SAMPLER, 0, 1, true, false, surface, sampler, 0, false);
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

/*
* Translates Sampler Norm API intrinsic.
*
* Assuming: N = 4, channelMask=CM_ABGR_ENABLE, surfIndex = 0x21, samplerIndex = 0x4,
*           then the generated code should look like the following for GT:
*
* .declare  VX Base=m ElementSize=4 Type=ud Total=16
* .declare  VY Base=r ElementSize=2 Type=uw Total=128
*
* mov  (8)     VX(0,0)<1>,  r0:ud
* mov  (1)     VX(0,2)<1>,  0
* mov  (1)     VX(1,1)<1>,  deltaU
* mov  (1)     VX(1,2)<1>,  u
* mov  (1)     VX(1,5)<1>,  deltaV
* mov  (1)     VX(1,6)<1>,  v
* send (16)    VY(0,0)<1>,  VX(0,0),    0x2,   0x048bc421
* mov  (128)   M(0,0)<1>,   VY(0,0)
*
* VX(0,0): message header
*
* VX(1,0): SIMD32 media payload
*
* ex_desc: 0x2 == 0010 (Target Function ID: Sampling Engine)
*
* desc: 0x048bc421 == Bit 31-29: 000 (Reserved)
*                     Bit 28-25: 0010 (Message Length = )
*                     Bit 24-20: 01000 (Response Message Length = 8)
*                     Bit 19:    1 (Header present)
*                     Bit 18:    0 (Reserved)
*                     Bit 17-16: 11 (SIMD Mode = SIMD32)
*                     Bit 15-12: 1100 (Message Type = sample_unorm media)
*                     Bit 11-8:  0000 + samplerIndex  (Sampler Index)
*                     Bit 7-0:   00000000 + surfIndex (Binding Table Index)
*
*/
int IR_Builder::translateVISASamplerNormInst(
    G4_Operand* surface,
    G4_Operand* sampler,
    ChannelMask channel,
    unsigned numEnabledChannels,
    G4_Operand* deltaUOpnd,
    G4_Operand* uOffOpnd,
    G4_Operand* deltaVOpnd,
    G4_Operand* vOffOpnd,
    G4_DstRegRegion* dst_opnd )
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    // mov (8)      VX(0,0)<1>,  r0:ud
    // add dcl for VX
    G4_Declare *dcl = Create_MRF_Dcl( 2 * GENX_SAMPLER_IO_SZ, Type_UD );

    // mov  VX(0,0)<1>, r0
    Create_MOVR0_Inst( dcl, 0, 0 );
    /* mov (1)     VX(0,2)<1>,   0  */
    unsigned cmask = channel.getHWEncoding() << 12;
    Create_MOV_Inst( dcl, 0, 2, 1, NULL, NULL, createImm( cmask, Type_UD ) );

    G4_Declare *dcl1 = Create_MRF_Dcl( 8, Type_F );
    dcl1->setAliasDeclare(dcl, GENX_MRF_REG_SIZ);

    // mov  (1)     VX(1,4)<1>,  deltaU
    Create_MOV_Inst( dcl1, 0, 4, 1, NULL, NULL, deltaUOpnd );
    // mov  (1)     VX(1,2)<1>,  u
    Create_MOV_Inst( dcl1, 0, 2, 1, NULL, NULL, uOffOpnd );
    // mov  (1)     VX(1,5)<1>,  deltaV
    Create_MOV_Inst( dcl1, 0, 5, 1, NULL, NULL, deltaVOpnd );
    // mov  (1)     VX(1,3)<1>,  v
    Create_MOV_Inst( dcl1, 0, 3, 1, NULL, NULL, vOffOpnd );

    // send's operands preparation
    // create a currDst for VX
    G4_SrcRegRegion* payload = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());

    G4_DstRegRegion* d = Check_Send_Dst( dst_opnd->asDstRegRegion());

    // Set bit 12-17 for the message descriptor
    unsigned temp = 0;
    temp += 0xc << 12;   // Bit 16-12 = 1100 for Sampler Message Type
    temp += 0x3 << 17;   // Bit 18-17 = 11 for SIMD32 mode

    // dst is already UW
    G4_INST *send_inst = Create_Send_Inst_For_CISA(
        NULL,
        d,
        payload,
        2,
        32*numEnabledChannels*G4_Type_Table[Type_UW].byteSize/GENX_GRF_REG_SIZ,
        32,
        temp,
        SFID_SAMPLER,
        0,
        1,
        true,
        false,
        surface,
        sampler,
        0,
        false);
    last_inst = send_inst;
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISASimdInst(ISA_Opcode opcode, G4_Predicate *predOpnd,
                                      Common_ISA_Exec_Size executionSize, Common_VISA_EMask_Ctrl emask, G4_Label *label)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    uint8_t exsize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    unsigned int instOpt = Get_Gen4_Emask(emask, exsize);

    // create Gen4 Inst
    G4_Operand *src0 = NULL, *src1 = NULL;
    G4_CondMod *condmod = NULL;

    last_inst = createInst(
        predOpnd,
        Get_G4_Opcode_From_Common_ISA_Opcode((ISA_Opcode)opcode),
        condmod,
        false,
        exsize,
        NULL,
        src0,
        src1,
        instOpt,
        0);

    if( opcode == ISA_GOTO )
    {
        last_inst->asCFInst()->setUip( label );
    }
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISASampleInfoInst(
    Common_ISA_Exec_Size executionSize,
    Common_VISA_EMask_Ctrl emask,
    ChannelMask chMask,
    G4_Operand* surface,
    G4_DstRegRegion* dst)
{
    uint8_t execSize = (uint8_t) Get_Common_ISA_Exec_Size( executionSize );
    uint32_t instOpt = Get_Gen4_Emask( emask, execSize );
    VISAChannelMask channels = chMask.getAPI();
    bool useFakeHeader = (getGenxPlatform() < GENX_SKL) ? false :
        (channels == CHANNEL_MASK_R);
    bool preEmption = forceSamplerHeader();
    bool forceSplitSend = IsBindlessSurface(*this, surface);
    bool useHeader = true;
    // SAMPLEINFO has 0 parameters so its only header

    unsigned int numRows = 1;

    G4_Declare *msg = NULL;
    G4_SrcRegRegion *m0 = NULL;

    if (!useFakeHeader || forceSplitSend || preEmption)
    {
        msg = getSamplerHeader(false);

        unsigned int secondDword = chMask.getHWEncoding() << 12;

        G4_Imm* immOpndSecondDword = createImm(secondDword, Type_UD);

        // mov (1) msg(0,2) immOpndSecondDword
        G4_DstRegRegion payloadDst(Direct, msg->getRegVar(), 0, 2, 1, Type_UD);
        G4_DstRegRegion* payloadDstRgn = createDstRegRegion(payloadDst);

        G4_INST* movInst = createInst(NULL, G4_mov, NULL, false, 1, payloadDstRgn, immOpndSecondDword, NULL, 0, 0);
        movInst->setOptionOn(InstOpt_WriteEnable);

        m0 = Create_Src_Opnd_From_Dcl(msg, getRegionStride1());
    }
    else
    {
        useHeader = false;
        msg = createTempVar(8, Type_UD, Either, Any);
        G4_DstRegRegion *dst = createDstRegRegion(Direct, msg->getRegVar(), 0, 0, 1, Type_UD);
        G4_Imm* src0Imm = createImm(0, Type_UD);
        auto temp = createInst(NULL, G4_mov, NULL, false, 8, dst, src0Imm, NULL, 0, 0);
        temp->setOptionOn(InstOpt_WriteEnable);
        m0 = createSrcRegRegion(Mod_src_undef, Direct, msg->getRegVar(), 0, 0, getRegionStride1(), Type_UD);
    }
    // Now create message descriptor
    // 7:0 - BTI
    // 11:8 - Sampler Index
    // 16:12 - Message Type
    // 18:17 - SIMD Mode
    // 19 - Header Present
    // 24:20 - Response Length
    // 28:25 - Message Length
    // 29 - SIMD Mode
    // 30 - Return Format
    // 31 - CPS Message LOD Compensation Enable
    unsigned int fc = 0;

    fc |= ( (unsigned int) VISA_3D_SAMPLEINFO & 0x1f ) << 12;

    if( execSize == 8 )
    {
        fc |= ( 1 << 17 );
    }
    else if (execSize == 16)
    {
        fc |= (2 << 17);
    }

    uint32_t retSize = (execSize == 8 ? chMask.getNumEnabledChannels() : chMask.getNumEnabledChannels() * 2);

    if (forceSplitSend)
    {
        last_inst = Create_SplitSend_Inst_For_CISA(NULL, dst, m0, numRows,
            createNullSrc(Type_UD), 0, retSize,
            execSize, fc, 0, SFID_SAMPLER, false, useHeader, true, false, surface, NULL, instOpt, false);
    }
    else
    {
        last_inst = Create_Send_Inst_For_CISA(NULL, dst, m0, numRows, retSize,
            execSize, fc, SFID_SAMPLER, false, useHeader, true, false, surface, NULL, instOpt, false);
    }

    return CM_SUCCESS;
}

int IR_Builder::translateVISAResInfoInst(
    Common_ISA_Exec_Size executionSize,
    Common_VISA_EMask_Ctrl emask,
    ChannelMask chMask,
    G4_Operand* surface,
    G4_SrcRegRegion* lod,
    G4_DstRegRegion* dst )
{
    uint8_t execSize = (uint8_t) Get_Common_ISA_Exec_Size( executionSize );
    uint32_t instOpt = Get_Gen4_Emask( emask, execSize );
    //For SKL if channels are continuous don't need header

    VISAChannelMask channels = chMask.getAPI();
    bool preEmption = forceSamplerHeader();
    bool useHeader = preEmption || (getGenxPlatform() < GENX_SKL) ? channels != CHANNEL_MASK_RGBA :
        (channels != CHANNEL_MASK_R && channels != CHANNEL_MASK_RG && channels != CHANNEL_MASK_RGB && channels != CHANNEL_MASK_RGBA);

    // Setup number of rows = ( header + lod ) by default
    unsigned int numRows = (execSize == 8 ? 1 : 2); 
    if (useHeader)
    {
        numRows++;
    }
    unsigned int regOff = 0;
    uint32_t returnLength = (execSize == 8 ? chMask.getNumEnabledChannels() : chMask.getNumEnabledChannels() * 2);

    bool useSplitSend = useSends();

    G4_Declare *msg = NULL;
    G4_Declare *payloadUD = NULL;
    if (useSplitSend)
    {
        if (useHeader)
        {
            --numRows;
        }
        unsigned int numElts = numRows * GENX_GRF_REG_SIZ/G4_Type_Table[Type_F].byteSize;
        msg = getSamplerHeader(false);
        payloadUD = Create_MRF_Dcl(numElts, Type_UD);
    }
    else
    {
        unsigned int numElts = numRows * GENX_GRF_REG_SIZ/G4_Type_Table[Type_F].byteSize;
        msg = Create_MRF_Dcl(numElts, Type_UD);
        payloadUD = Create_MRF_Dcl(numElts - (useHeader ? GENX_SAMPLER_IO_SZ : 0), Type_UD);
        payloadUD->setAliasDeclare(msg, useHeader ? GENX_MRF_REG_SIZ : 0);

        if (useHeader)
        {
            // Both SAMPLEINFO and RESINFO use header
            Create_MOVR0_Inst(msg, 0, 0, true);
        }
    }

    if( useHeader )
    {
        unsigned int secondDword = 0;
        secondDword |= ( chMask.getHWEncoding() << 12 );

        G4_Imm* immOpndSecondDword = createImm( secondDword, Type_UD );

        // mov (1) msg(0,2) immOpndSecondDword
        G4_DstRegRegion payloadDst( Direct, msg->getRegVar(), 0, 2, 1, Type_UD );
        G4_DstRegRegion* payloadDstRgn = createDstRegRegion( payloadDst );

        G4_INST* movInst = createInst( NULL, G4_mov, NULL, false, 1, payloadDstRgn, immOpndSecondDword, NULL, 0, 0 );
        movInst->setOptionOn( InstOpt_WriteEnable );
    }

    // Copy over lod vector operand to payload's 1st row
    Copy_SrcRegRegion_To_Payload( payloadUD, regOff, lod, execSize, instOpt );

    // Now create message descriptor
    // 7:0 - BTI
    // 11:8 - Sampler Index
    // 16:12 - Message Type
    // 18:17 - SIMD Mode
    // 19 - Header Present
    // 24:20 - Response Length
    // 28:25 - Message Length
    // 29 - SIMD Mode
    // 30 - Return Format
    // 31 - CPS Message LOD Compensation Enable
    unsigned int fc = 0;

    fc |= ( (unsigned int) VISA_3D_RESINFO & 0x1f ) << 12;

    if( execSize == 8 )
    {
        fc |= ( 1 << 17 );
    }
    else if( execSize == 16 )
    {
        fc |= ( 2 << 17 );
    }

    if (useSplitSend)
    {
        G4_SrcRegRegion *m0 = nullptr;
        G4_SrcRegRegion *m1 = nullptr;
        unsigned int src0Size = 0;
        unsigned int src1Size = 0;

        if (useHeader)
        {
            m0 = Create_Src_Opnd_From_Dcl(msg, getRegionStride1());
            m1 = Create_Src_Opnd_From_Dcl(payloadUD, getRegionStride1());
            src0Size = 1;
            src1Size = numRows;
        }
        else
        {
            m0 = Create_Src_Opnd_From_Dcl(payloadUD, getRegionStride1());
            m1 = createNullSrc(Type_UD);
            src0Size = numRows;
            src1Size = 0;
        }
        last_inst = Create_SplitSend_Inst_For_CISA(NULL, dst, m0, src0Size, m1, src1Size, returnLength,
            execSize, fc, 0, SFID_SAMPLER, false, useHeader, true, false, surface, NULL, instOpt, false);
    }
    else
    {
        G4_SrcRegRegion *m = Create_Src_Opnd_From_Dcl(msg, getRegionStride1());
        last_inst = Create_Send_Inst_For_CISA( NULL, dst, m, numRows, returnLength,
            execSize, fc, SFID_SAMPLER, false, useHeader, true, false, surface, NULL, instOpt, false );
    }

    return CM_SUCCESS;
}



// generate a URB_SIMD8* message
// urbHandle -- 1 GRF holding 8 URB handles.  This is the header of the message
// perSlotOffset -- 1 GRF holding 8 DWord offsets.  If present, it must be immediately after the header
// channelMask -- 1 GRF holding 8 8-bit masks.  In vISA spec they have constant values and must be
//                identical.  If present,  occurs after the per slot message phase if the per slot
//                message phase exists else it occurs after the header.

int IR_Builder::translateVISAURBWrite3DInst(
    G4_Predicate* pred,
    Common_ISA_Exec_Size executionSize,
    Common_VISA_EMask_Ctrl emask,
    uint8_t numOut,
    uint16_t globalOffset,
    G4_SrcRegRegion* channelMask,
    G4_SrcRegRegion* urbHandle,
    G4_SrcRegRegion* perSlotOffset,
    G4_SrcRegRegion* vertexData )
{
    uint8_t execSize = (uint8_t) Get_Common_ISA_Exec_Size( executionSize );
    uint32_t instOpt = Get_Gen4_Emask( emask, execSize );

    if (numOut == 0)
    {
        MUST_BE_TRUE(vertexData->isNullReg(), "vertex payload must be null ARF when numOut is 0");
    }

    // header + channelMask + numOut
    unsigned int numRows = 2 + numOut;
    const bool useHeader = true;
    bool usePerSlotIndex = false;
    bool useChannelMask = true;

    if( !perSlotOffset->isNullReg() )
    {
        usePerSlotIndex = true;
        numRows++;
    }

    if (channelMask->isNullReg())
    {
        useChannelMask = false;
        numRows--;
    }

    bool useSplitSend = useSends();
    // So far, we don't have a obvious cut except for header. As the result,
    // split-send is disabled once there's no header in the message.
    if (!useHeader)
        useSplitSend = false;

    if (numOut == 0)
    {
        // no split send if payload is null
        useSplitSend = false;
    }

    // msg is the header for split send, or the entire payload for regular send
    G4_Declare *msg = NULL;
    G4_Declare* payloadF = NULL;
    G4_Declare* payloadD = NULL;
    G4_Declare* payloadUD = NULL;
    if (useSplitSend)
    {
        ASSERT_USER(useHeader, "So far, split-send is only used when header is present!");
        --numRows;
        if (numRows > 0)
        {
            unsigned int numElts = numRows * GENX_GRF_REG_SIZ/G4_Type_Table[Type_F].byteSize;
            // we can use the urb handle directly since URB write will not modify its header
            //msg = Create_MRF_Dcl(GENX_SAMPLER_IO_SZ, Type_UD);
            payloadUD = Create_MRF_Dcl(numElts, Type_UD);
            payloadF = Create_MRF_Dcl(numElts, Type_F);
            payloadD = Create_MRF_Dcl(numElts, Type_D);
            payloadF->setAliasDeclare(payloadUD, 0);
            payloadD->setAliasDeclare(payloadUD, 0);
        }
    }
    else
    {
        unsigned int numElts = numRows * GENX_GRF_REG_SIZ/G4_Type_Table[Type_F].byteSize;
        msg = Create_MRF_Dcl(numElts, Type_UD);
        if (numRows > 1)
        {
            payloadUD = Create_MRF_Dcl(numElts - (useHeader ? GENX_SAMPLER_IO_SZ : 0), Type_UD);
            payloadF = Create_MRF_Dcl(numElts - (useHeader ? GENX_SAMPLER_IO_SZ : 0), Type_F);
            payloadD = Create_MRF_Dcl(numElts - (useHeader ? GENX_SAMPLER_IO_SZ : 0), Type_D);
            payloadUD->setAliasDeclare(msg, useHeader ? GENX_MRF_REG_SIZ : 0);
            payloadF->setAliasDeclare(msg, useHeader ? GENX_MRF_REG_SIZ : 0);
            payloadD->setAliasDeclare(msg, useHeader ? GENX_MRF_REG_SIZ : 0);
        }
    }

    unsigned int regOff = 0;
    // Setup header
    if( useHeader && msg != NULL)
    {
        unsigned ignoredOff = 0;
        Copy_SrcRegRegion_To_Payload( msg, ignoredOff, urbHandle, 8, instOpt );
    }

    if( usePerSlotIndex )
    {
        Copy_SrcRegRegion_To_Payload( payloadUD, regOff, perSlotOffset, 8, instOpt );
    }

    if( useChannelMask )
    {

        // shl (8) M2.0<1>:ud cmask<8;8,1>:ud 0x10:uw
        G4_DstRegRegion payloadUDRegRow2( Direct, payloadUD->getRegVar(), regOff++, 0, 1, Type_UD );
        G4_DstRegRegion* payloadUDRegRgnRow2 = createDstRegRegion( payloadUDRegRow2 );

        G4_INST* channelMaskInst = createInst(nullptr, G4_shl, nullptr, false, 8, payloadUDRegRgnRow2, channelMask, createImm(16, Type_UW), 0);
        channelMaskInst->setOptionOn( instOpt );
    }

    G4_Declare* vertexDataDcl = numOut == 0 ? NULL : vertexData->getBase()->asRegVar()->getDeclare();

    bool needsDataMove = (!useSplitSend || usePerSlotIndex || useChannelMask);
    if (needsDataMove)
    {
        // we have to insert moves to make payload contiguous
        unsigned int startSrcRow = vertexData->getRegOff();

        for( int i = 0; i < numOut; i++ )
        {
            G4_DstRegRegion payloadTypedRegRowi( Direct, payloadF->getRegVar(), regOff++, 0, 1, Type_F );
            G4_DstRegRegion* payloadTypedRegRowRgni = createDstRegRegion( payloadTypedRegRowi );

            G4_SrcRegRegion* vertexSrcRegRgnRowi = createSrcRegRegion(Mod_src_undef, Direct, vertexDataDcl->getRegVar(), startSrcRow++, 0, getRegionStride1(), Type_F);

            G4_INST* vertexDataMovInst = createInst( NULL, G4_mov, NULL, false, 8, payloadTypedRegRowRgni, vertexSrcRegRgnRowi, NULL, 0 );
            vertexDataMovInst->setOptionOn( instOpt );
        }
    }
    else
    {
        payloadUD = vertexDataDcl;
    }

    // Msg descriptor
    unsigned int fc = 0;

    fc |= 0x7;

    fc |= ( globalOffset << 4 );

    if( useChannelMask )
    {
        fc |= ( 0x1 << 15 );
    }

    if( usePerSlotIndex )
    {
        fc |= ( 0x1 << 17 );
    }

    if (useSplitSend)
    {
        G4_SrcRegRegion *m0 = urbHandle;
        G4_SrcRegRegion *m1 = nullptr;

        if (needsDataMove)
        {
            m1 = Create_Src_Opnd_From_Dcl(payloadUD, getRegionStride1());
        }
        else
        {
            ASSERT_USER(payloadUD == vertexDataDcl,
                "If there is no need for data move then payloadUD == vertexDataDcl must hold!");

            m1 = createSrcRegRegion(
                Mod_src_undef,
                Direct,
                payloadUD->getRegVar(),
                vertexData->getRegOff(),
                vertexData->getSubRegOff(),
                getRegionStride1(),
                payloadUD->getElemType());
        }

        last_inst = Create_SplitSend_Inst_For_CISA(pred, createNullDst(Type_UD), m0, 1, m1, numRows, 0,
            execSize, fc, 0, SFID_URB, false, useHeader, false, true, NULL, NULL, instOpt, false);
    } else {
        G4_SrcRegRegion *m = Create_Src_Opnd_From_Dcl(msg, getRegionStride1());
        last_inst = Create_Send_Inst_For_CISA( pred, createNullDst( Type_UD ), m, numRows, 0,
            execSize, fc, SFID_URB, false, useHeader, false, true, nullptr, nullptr, instOpt, false );
    }
    return CM_SUCCESS;
}

/*****************************************************************************\
ENUM: EU_GEN6_DATA_PORT_RENDER_TARGET_WRITE_CONTROL
\*****************************************************************************/
enum EU_GEN6_DATA_PORT_RENDER_TARGET_WRITE_CONTROL
{
    EU_GEN6_DATA_PORT_RENDER_TARGET_WRITE_CONTROL_SIMD16_SINGLE_SOURCE = 0,
    EU_GEN6_DATA_PORT_RENDER_TARGET_WRITE_CONTROL_SIMD16_SINGLE_SOURCE_REPLICATED = 1,
    EU_GEN6_DATA_PORT_RENDER_TARGET_WRITE_CONTROL_SIMD8_DUAL_SOURCE_LOW = 2,
    EU_GEN6_DATA_PORT_RENDER_TARGET_WRITE_CONTROL_SIMD8_DUAL_SOURCE_HIGH = 3,
    EU_GEN6_DATA_PORT_RENDER_TARGET_WRITE_CONTROL_SIMD8_SINGLE_SOURCE_LOW = 4,
    EU_GEN6_DATA_PORT_RENDER_TARGET_WRITE_CONTROL_SIMD8_IMAGE_WRITE = 5
};

int IR_Builder::translateVISARTWrite3DInst(
                        G4_Predicate* pred,
                        Common_ISA_Exec_Size executionSize,
                        Common_VISA_EMask_Ctrl emask,
                        G4_Operand *surface,
                        G4_SrcRegRegion *r1HeaderOpnd,
                        G4_Operand *rtIndex,
                        vISA_RT_CONTROLS cntrls,
                        G4_SrcRegRegion *sampleIndexOpnd,
                        G4_Operand *cpsCounter,
                        unsigned int numParms,
                        G4_SrcRegRegion ** msgOpnds)
{
    uint8_t execSize = (uint8_t) Get_Common_ISA_Exec_Size( executionSize );
    uint32_t instOpt = Get_Gen4_Emask( emask, execSize );
    bool useHeader = false;

    uint8_t varOffset = 0;
    G4_SrcRegRegion * s0a = NULL;
    //oMask
    G4_SrcRegRegion * oM  = NULL;
    if(cntrls.s0aPresent)
    {
        s0a = msgOpnds[varOffset];
        ++varOffset;
    }
    if(cntrls.oMPresent)
    {
        oM = msgOpnds[varOffset];
        ++varOffset;
    }

    G4_SrcRegRegion * R = msgOpnds[varOffset++];
    G4_SrcRegRegion * G = msgOpnds[varOffset++];
    G4_SrcRegRegion * B = msgOpnds[varOffset++];
    G4_SrcRegRegion * A = msgOpnds[varOffset++];
    //depth
    G4_SrcRegRegion * Z = NULL;

    if(cntrls.zPresent)
        Z = msgOpnds[varOffset++];

    //stencil
    G4_SrcRegRegion * S = NULL;
    if(cntrls.isStencil)
    {
        S = msgOpnds[varOffset++];
    }

    if(varOffset != numParms)
    {
        assert( 0 );
        return CM_FAILURE;
    }

    bool FP16Data = R->getType() == Type_HF;
    if (FP16Data)
    {
        MUST_BE_TRUE( (G->isNullReg() || G->getType() == Type_HF) &&
                      (B->isNullReg() || B->getType() == Type_HF) &&
                      (A->isNullReg() || A->getType() == Type_HF),
                        "R,G,B,A for RT write must have the same type");
    }

    auto mult = (execSize == 8)? 1 : 2;
    mult = (FP16Data)? 1 : mult;

    //RGBA sr0Alpha take up one GRF in SIMD8 and SIMD16 modes.
    //in SIMD8 upper DWORDs are reserved
    unsigned int numRows = numParms * mult;

    //Depth is always Float
    //For SIMD16 it is 2 grfs
    //For SIMD8  it is 1 grf
    if (FP16Data && cntrls.zPresent && executionSize == EXEC_SIZE_16)
    {
        ++numRows;
    }

    if( cntrls.oMPresent && mult == 2 )
    {
        // oM is always 1 row irrespective of execSize
        numRows--;
    }

    //although for now HW only supports stencil in SIMD8 mode
    if ( cntrls.isStencil  && mult == 2 )
    {
        // stencil is always 1 row irrespective of execSize
        numRows--;
    }

#define HEADER_OFFSET       GENX_GRF_REG_SIZ * 2
#define HEADER_SIZE         GENX_SAMPLER_IO_SZ * 2
#define RT_HEADER_SIZE      2
    uint8_t headerSizeInDwords = HEADER_SIZE;
    if (emask == vISA_EMASK_M5_NM || emask == vISA_EMASK_M5)
    {
        //For SIMD32 case when RT Write is split in to two SIMD16
        //header information is expected in R0/R2 registers
        headerSizeInDwords += GENX_SAMPLER_IO_SZ;
    }

    /*
        All other values should be set by default.
        Most of the time when renderTargetIndex != 0, src0Alpha is present also
    */
    bool isRTIdxNonzero = cntrls.RTIndexPresent &&
        (rtIndex->isSrcRegRegion() || (rtIndex->isImm() && rtIndex->asImm()->getImm() != 0));
    bool isRTIdxDynamic = cntrls.RTIndexPresent && rtIndex->isSrcRegRegion();
    bool needsHeaderForMRT = isRTIdxDynamic || cntrls.s0aPresent || (!hasHeaderlessMRTWrite() && isRTIdxNonzero);
    if (needsHeaderForMRT || cntrls.isSampleIndex)
    {
        useHeader = true;
        numRows += RT_HEADER_SIZE;
    }

    bool useSplitSend = useSends();
    // So far, we don't have a obvious cut except for header. As the result,
    // split-send is disabled once there's no header in the message.

    G4_SrcRegRegion* srcToUse   = NULL;
    G4_Declare *msg             = NULL;
    G4_Declare *msgF            = NULL;
    G4_Declare *payloadUD       = NULL;
    G4_Declare *payloadUW       = NULL;
    G4_Declare *payloadFOrHF    = NULL;
    G4_Declare *payloadF        = NULL;

    if (useSplitSend)
    {
        if (useHeader)
        {
            //subtracting Header
            numRows -= RT_HEADER_SIZE;
            //creating header
            msg = Create_MRF_Dcl(GENX_SAMPLER_IO_SZ * RT_HEADER_SIZE, Type_UD);
            msgF = Create_MRF_Dcl(GENX_SAMPLER_IO_SZ * RT_HEADER_SIZE, Type_F);
            msgF->setAliasDeclare(msg, 0);
        }
        //creating payload
        unsigned int numElts = numRows * GENX_GRF_REG_SIZ / G4_Type_Table[Type_F].byteSize;
        payloadUD = Create_MRF_Dcl(numElts, Type_UD);
        payloadFOrHF = Create_MRF_Dcl(numElts, FP16Data ? Type_HF : Type_F);
        payloadUW = Create_MRF_Dcl(numElts, Type_UW);
        payloadF = Create_MRF_Dcl(numElts, Type_F);

        payloadFOrHF->setAliasDeclare(payloadUD, 0);
        payloadUW->setAliasDeclare(payloadUD, 0);
        payloadF->setAliasDeclare(payloadUD, 0);
    }
    else
    {
        unsigned int numElts = numRows * GENX_GRF_REG_SIZ/G4_Type_Table[Type_F].byteSize;
        //creating enough space for header + payload
        msg = Create_MRF_Dcl(numElts, Type_UD);
        msgF = Create_MRF_Dcl(GENX_SAMPLER_IO_SZ * 2, Type_F);
        msgF->setAliasDeclare(msg, 0);

        //creating payload declarations.
        payloadUD = Create_MRF_Dcl(numElts - (useHeader ? HEADER_SIZE : 0), Type_UD);
        payloadFOrHF = Create_MRF_Dcl(numElts - (useHeader ? HEADER_SIZE : 0), FP16Data ? Type_HF : Type_F);
        payloadUW = Create_MRF_Dcl(numElts - (useHeader ? HEADER_SIZE : 0), Type_UW);
        payloadF = Create_MRF_Dcl(numElts, Type_F);

        //setting them to alias a top level decl with offset past the header
        payloadUD->setAliasDeclare(msg, useHeader ? HEADER_OFFSET : 0);
        payloadFOrHF->setAliasDeclare(msg, useHeader ? HEADER_OFFSET : 0);
        payloadUW->setAliasDeclare(msg, useHeader ? HEADER_OFFSET : 0);
        payloadF->setAliasDeclare(payloadUD, 0);
    }

    if( useHeader )
    {
        ASSERT_USER(r1HeaderOpnd, "Second GRF for header that was passed in is NULL.");
        G4_DstRegRegion* payloadRegRgn = createDstRegRegion(Direct, msg->getRegVar(), 0, 0, 1, Type_UD);

        G4_Declare* r0 = getBuiltinR0();
        G4_SrcRegRegion* r0RegRgn = createSrcRegRegion(Mod_src_undef, Direct, r0->getRegVar(), 0, 0, getRegionStride1(), Type_UD);

        //moves data from r0 to header portion of the message
        G4_INST* movInst = createInst(NULL, G4_mov, NULL, false, 8, payloadRegRgn, r0RegRgn, NULL, 0, 0);
        movInst->setOptionOn(InstOpt_WriteEnable);

        payloadRegRgn = createDstRegRegion(Direct, msg->getRegVar(), 1, 0, 1, Type_UD);
		r1HeaderOpnd->setType(Type_UD);
        movInst = createInst(NULL, G4_mov, NULL, false, 8, payloadRegRgn, r1HeaderOpnd, NULL, 0, 0);
        movInst->setOptionOn(InstOpt_WriteEnable);

#define SAMPLE_INDEX_OFFSET 6
        if (cntrls.isSampleIndex)
        {
            G4_Declare *tmpDcl = createTempVar(2, Type_UD, Either, Any);
            G4_DstRegRegion *tmpDst = createDstRegRegion(Direct, tmpDcl->getRegVar(), 0, 0, 1, Type_UD);

            G4_INST* shiftInst = createInst(NULL, G4_shl, NULL, false, 1, tmpDst, sampleIndexOpnd, createImm(SAMPLE_INDEX_OFFSET, Type_UD), 0);
            shiftInst->setOptionOn(InstOpt_WriteEnable);

            G4_DstRegRegion* payloadUDRegRgn = createDstRegRegion(Direct, msg->getRegVar(), 0, 0, 1, Type_UD);
            G4_SrcRegRegion *tmpSrc = createSrcRegRegion(Mod_src_undef, Direct, tmpDcl->getRegVar(), 0, 0, getRegionScalar(), Type_UD);
            G4_SrcRegRegion *payloadSrc = createSrcRegRegion(Mod_src_undef, Direct, msg->getRegVar(), 0, 0, getRegionScalar(), Type_UD);
            G4_INST *orInst = createInst(NULL, G4_or, NULL, false, 1, payloadUDRegRgn, payloadSrc, tmpSrc, 0);
            orInst->setOptionOn(InstOpt_WriteEnable);

        }

        if (isRTIdxNonzero)
        {
            G4_DstRegRegion dstRTI( Direct, msg->getRegVar(), 0, 2, 1, Type_UD );
            G4_DstRegRegion* dstRTIRgn = createDstRegRegion( dstRTI );

            G4_INST* rtiMovInst = createInst(NULL, G4_mov, NULL, false, 1, dstRTIRgn, rtIndex, NULL, 0);
            rtiMovInst->setOptionOn( InstOpt_WriteEnable );
        }

        //if header is used, then predication value will need to be stored
        //in the header
        if(useHeader && (pred || cntrls.isHeaderMaskfromCe0))
        {
            //moving pixelMask in to payload
            G4_DstRegRegion* dstPixelMaskRgn = createDstRegRegion(
                Direct, msg->getRegVar(), 1, 14, 1, Type_UW);

            G4_SrcRegRegion *pixelMask = NULL;
            if (emask == vISA_EMASK_M5_NM || emask == vISA_EMASK_M5)
            {
                if (pred)
                {
                    //this is a Second half of a SIMD32 RT write. We need to get second half of flag register.
                    //mov whole register in to GRF, move second word of it in to payload.

                    G4_SrcRegRegion *pixelMaskTmp = createSrcRegRegion(
                            Mod_src_undef, Direct,
                            pred->getBase()->asRegVar(), 0, 0,
                            getRegionScalar(), Type_UD);
                    G4_Declare *tmpDcl = createTempVar(1, Type_UD, Either, Any);
                    G4_DstRegRegion *tmpDst = createDstRegRegion(Direct, tmpDcl->getRegVar(), 0, 0, 1, Type_UD);
                    createInst(NULL, G4_mov, NULL, false, 1, tmpDst, pixelMaskTmp, NULL, InstOpt_WriteEnable);

                    pixelMask = createSrcRegRegion(Mod_src_undef, Direct,
                        tmpDcl->getRegVar(), 0, 1, getRegionScalar(), Type_UW);

                    // move from temp register to header
                    createInst(NULL, G4_mov, NULL, false, 1, dstPixelMaskRgn,
                        pixelMask, NULL, InstOpt_WriteEnable);
                }
                else
                {
                    G4_SrcRegRegion *ce0 = createSrcRegRegion(
                        Mod_src_undef, Direct,
                        phyregpool.getMask0Reg(), 0, 0,
                        getRegionScalar(), Type_UD);

                    // shr .14<1>:uw ce0:ud 16:uw
                    createInst(NULL, G4_shr, NULL, false, 1, dstPixelMaskRgn,
                        ce0, createImm(16, Type_UW), InstOpt_WriteEnable);
                }
            }
            else
            {
                if (pred)
                {
                    pixelMask = createSrcRegRegion(Mod_src_undef, Direct,
                        pred->getBase()->asRegVar(), 0, 0,
                        getRegionScalar(), Type_UW);
                }
                else
                {
                    G4_SrcRegRegion *ce0 = createSrcRegRegion(
                        Mod_src_undef, Direct,
                        phyregpool.getMask0Reg(), 0, 0,
                        getRegionScalar(), Type_UD);
                    // mov .14<1>:uw ce0:ud
                    pixelMask = ce0;
                }
                //clearing lower 15 bits
                createInst(NULL, G4_mov, NULL, false, 1, dstPixelMaskRgn,
                    pixelMask, NULL, InstOpt_WriteEnable);
            }

            pred = NULL;

        }
        unsigned int orImmVal = 0;

        //setting first DWORD of MHC_RT_C0 - Render Target Message Header Control

        if( cntrls.isStencil )
        {
            orImmVal = ( 0x1 << 14 );
        }

        if( cntrls.zPresent )
        {
            orImmVal = ( 0x1 << 13 );
        }

        if( cntrls.oMPresent )
        {
            orImmVal |= ( 0x1 << 12 );
        }

        if( cntrls.s0aPresent )
        {
            orImmVal |= ( 0x1 << 11 );
        }

        if( orImmVal != 0 )
        {
            G4_SrcRegRegion* immSrcRegRgn = createSrcRegRegion(Mod_src_undef, Direct, msg->getRegVar(), 0, 0, getRegionScalar(), Type_UD);

            G4_DstRegRegion* immDstRegRgn = createDstRegRegion(Direct, msg->getRegVar(), 0, 0, 1, Type_UD);

            G4_INST* immOrInst = createInst( NULL, G4_or, NULL, false, 1, immDstRegRgn, immSrcRegRgn, createImm( orImmVal, Type_UD ), 0, 0 );
            immOrInst->setOptionOn( InstOpt_WriteEnable );
        }
    }

    // Check whether coalescing is possible
#define UNINITIALIZED_DWORD 0xffffffff
    unsigned int offset = UNINITIALIZED_DWORD;
    // If the header is not present or split-send is available, we will try to
    // coalesc payload by checking whether the source is already prepared in a
    // continuous region. If so, we could reuse the source region directly
    // instead of copying it again.
    bool canCoalesce = !useHeader || useSplitSend;
    G4_SrcRegRegion* prevRawOpnd = NULL;

    if( R->isNullReg()  ||
        G->isNullReg()  ||
        B->isNullReg()  ||
        A->isNullReg())
        canCoalesce = false;

    if( canCoalesce && cntrls.s0aPresent)
    {
        prevRawOpnd = s0a;
        offset = getByteOffsetSrcRegion(s0a);
    }

    if( canCoalesce && cntrls.oMPresent )
    {
        //by default it will check based on first opnd type, but that can be HF, F, we need second operand type
        //according to spec oM is UW
        canCoalesce = checkIfRegionsAreConsecutive( prevRawOpnd, oM, execSize, oM->getType() );
        prevRawOpnd = oM;
        if( offset == UNINITIALIZED_DWORD )
        {
            offset = getByteOffsetSrcRegion(oM);
        }
    }

    if( canCoalesce )
    {
        if( execSize == 16 && cntrls.oMPresent )
        {
            // oM is 1 GRF for SIMD16 since it is UW type
            canCoalesce = checkIfRegionsAreConsecutive( oM, R, execSize, Type_UW );
            prevRawOpnd = R;
        }
        else
        {
            canCoalesce = checkIfRegionsAreConsecutive( prevRawOpnd, R, execSize );
            prevRawOpnd = R;
        }

        if( offset == UNINITIALIZED_DWORD )
        {
            offset = getByteOffsetSrcRegion(prevRawOpnd);
        }

        if( canCoalesce )
        {
            auto tempExecSize = execSize;
            if (FP16Data && execSize == 8)
                tempExecSize = 16;
            canCoalesce = checkIfRegionsAreConsecutive(prevRawOpnd, G, tempExecSize) &&
                checkIfRegionsAreConsecutive(G, B, tempExecSize) &&
                checkIfRegionsAreConsecutive(B, A, tempExecSize);
            prevRawOpnd = A;
            if( offset == UNINITIALIZED_DWORD )
            {
                offset = getByteOffsetSrcRegion(A);
                if (FP16Data && execSize == 8)
                    offset += 8;
            }
        }
    }

    if( canCoalesce && cntrls.zPresent )
    {
        canCoalesce = checkIfRegionsAreConsecutive( prevRawOpnd, Z, execSize );
        prevRawOpnd = Z;
    }

    if( canCoalesce && cntrls.isStencil )
    {
        canCoalesce = checkIfRegionsAreConsecutive( prevRawOpnd, S, execSize );
        prevRawOpnd = S;
    }

    if( canCoalesce == false )
    {
        // Copy parms to payload
        unsigned regOff = 0;

        if( cntrls.s0aPresent )
        {

            Copy_SrcRegRegion_To_Payload( payloadFOrHF, regOff, s0a, execSize, instOpt );
        }

        if( cntrls.oMPresent )
        {
            Copy_SrcRegRegion_To_Payload( payloadUW, regOff, oM, execSize, instOpt );
            //Copy_SrcRegRegion_To_Payload increments regOff by 1 if byteSize ==2
            //works for oM since in SIMD16 it occupies one GRF
        }

         /*
            When RT write is HF s0a,R, G, B, A are allowed to be HF.
            In SIMD8 upper DWORDS are reserved.
            In SIMD16 uppder DOWRDS contain second grf worth of values if type was F.

            Output can be only Depth, so V0 is passed in if RGBA don't need to be outputted
        */
        auto offIncrement = 2;
        if(execSize == 8 || FP16Data)
            offIncrement = 1;

        if(!R->isNullReg())
            Copy_SrcRegRegion_To_Payload( payloadFOrHF, regOff, R, execSize, instOpt );
        else
                regOff+= offIncrement;

        if(!G->isNullReg())
            Copy_SrcRegRegion_To_Payload( payloadFOrHF, regOff, G, execSize, instOpt );
        else
                regOff+= offIncrement;

        if(!B->isNullReg())
            Copy_SrcRegRegion_To_Payload( payloadFOrHF, regOff, B, execSize, instOpt );
        else
                regOff+= offIncrement;

        if(!A->isNullReg())
            Copy_SrcRegRegion_To_Payload( payloadFOrHF, regOff, A, execSize, instOpt );
        else
                regOff+= offIncrement;

        if( cntrls.zPresent )
        {
            Copy_SrcRegRegion_To_Payload( payloadF, regOff, Z, execSize, instOpt );
        }

        if( cntrls.isStencil )
        {
            Copy_SrcRegRegion_To_Payload( payloadFOrHF, regOff, S, execSize, instOpt );
        }

        srcToUse = Create_Src_Opnd_From_Dcl(payloadUD, getRegionStride1());
    }
    else
    {
        // Coalesce and directly use original raw operand
        G4_Declare *dcl = R->getBase()->asRegVar()->getDeclare();
        srcToUse = createSrcRegRegion(Mod_src_undef, Direct, dcl->getRegVar(), offset / 32, 0, getRegionStride1(), R->getType());
    }

    // Now create message message descriptor
    // 7:0 - BTI
    // 10:8 - Render Target Message Subtype
    // 11 - Slot Group Select
    // 12 - Last Render Target Select
    // 13 - Reserved (DevBDW)
    // 13 - Per-Sample PS Outputs Enable (DevSKL+)
    // 17:14 - Message Type
    // 18 - Reserved
    // 19 - Header Present
    // 24:20 - Response Length
    // 28:25 - Message Length
    // 29 - Reserved
    // 30 - Message Precision Subtype (DevBDW+)
    // 31 - Reserved (MBZ)
    unsigned int fc = 0;

    //making explicit
    EU_GEN6_DATA_PORT_RENDER_TARGET_WRITE_CONTROL messageType =
        (executionSize == EXEC_SIZE_8)
        ? EU_GEN6_DATA_PORT_RENDER_TARGET_WRITE_CONTROL_SIMD8_SINGLE_SOURCE_LOW
        : EU_GEN6_DATA_PORT_RENDER_TARGET_WRITE_CONTROL_SIMD16_SINGLE_SOURCE;

#define RENDER_TARGET_MESSAGE_SUBTYPE_OFFSET 8
    fc |= (messageType << RENDER_TARGET_MESSAGE_SUBTYPE_OFFSET);

#define SLOT_GROUP_SELECT_OFFSET 11
    //for SIMD32 for second RT Write setting this bit
    if (emask == vISA_EMASK_M5_NM || emask == vISA_EMASK_M5)
        fc |= (0x1 << SLOT_GROUP_SELECT_OFFSET);

    if( cntrls.isLastWrite )
    {
#define LAST_RENDER_TARGET_SELECT_OFFSET 12
        fc |= ( 0x1 << LAST_RENDER_TARGET_SELECT_OFFSET );
    }

    if( cntrls.isPerSample )
    {
#define PER_SAMPLE_PS_ENABLE_OFFSET 13
        fc += (0x1 << PER_SAMPLE_PS_ENABLE_OFFSET);
    }


    if (FP16Data)
    {
        fc |= 0x1 << MESSAGE_PRECISION_SUBTYPE_OFFSET;
    }

#define MESSAGE_TYPE 14
    fc |= ( 0xc << MESSAGE_TYPE );

#define COARSE_PIXEL_OUTPUT_ENABLE 18
    if(cntrls.isCoarseMode)
            fc |= 0x1 << COARSE_PIXEL_OUTPUT_ENABLE;
#define CPS_COUNTER_EXT_MSG_DESC_OFFSET 16
    if (useSplitSend || cpsCounter)
    {
        G4_SendMsgDescriptor *msgDesc = NULL;
        G4_SrcRegRegion *m0 = NULL;
        if (useHeader)
        {
            m0 = Create_Src_Opnd_From_Dcl(msg, getRegionStride1());
            msgDesc = createSendMsgDesc(fc, 0, RT_HEADER_SIZE, SFID_DP_WRITE, false, numRows,
                0, false, true, surface, NULL);
            msgDesc->setHeaderPresent(useHeader);
        }
        else
        {
            if (!isRTIdxNonzero && !cntrls.s0aPresent)
            {
                // direct imm is a-ok
                msgDesc = createSendMsgDesc(fc, 0, numRows, SFID_DP_WRITE, false, 0,
                        0, false, true, surface, NULL);
            }
            else
            {
                // we must use a0 for extended msg desc in this case as bit[11:15] is forced to 0 when it's imm
                assert(rtIndex->isImm() && "RTIndex must be imm at this point");
                uint8_t RTIndex = (uint8_t)rtIndex->asImm()->getImm() & 0x7;
                uint32_t desc = G4_SendMsgDescriptor::createDesc(fc, false, numRows, 0);
                uint32_t extDesc = G4_SendMsgDescriptor::createMRTExtDesc(cntrls.s0aPresent, RTIndex,
                    false, 0);
                // mov (1) a0.2:ud extDesc
                G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(getBuiltinA0Dot2(), 1);
                createInst(nullptr, G4_mov, nullptr, false, 1, dst, createImm(extDesc, Type_UD), nullptr, InstOpt_WriteEnable);
                G4_SrcRegRegion* extDescOpnd = Create_Src_Opnd_From_Dcl(getBuiltinA0Dot2(), getRegionScalar());
                msgDesc = createSendMsgDesc(desc, extDesc, false, true, surface, nullptr);
                msgDesc->setExtMsgDesc(extDescOpnd);
            }
        }

        /*
            If we need to set cps counter then ext_message descriptor
            needs to be a register.
        */
        if(cpsCounter)
        {
            ASSERT_USER(hasCPS(), "CPS counter is not supported");
            //getting lower bits.
            unsigned msgDescValue = msgDesc->getExtendedDesc();

            G4_Declare *extDescDcl = getBuiltinA0Dot2();

            //shifting CPS counter by appropriate number of bits and storing in ext_descriptor operand

            G4_DstRegRegion *dstMove2 = Create_Dst_Opnd_From_Dcl(extDescDcl, 1);
            G4_Imm *immedOpnd = createImm(msgDescValue, Type_UD);

            ///setting lower bits
            createInst(NULL, G4_or, NULL, false, 1, dstMove2, cpsCounter, immedOpnd, NULL, InstOpt_WriteEnable, 0);

            //creating message descriptor
            G4_SrcRegRegion *extMsgDescOpnd  = Create_Src_Opnd_From_Dcl(extDescDcl, getRegionScalar());
            msgDesc->setExtMsgDesc(extMsgDescOpnd);
        }

        if (!useHeader)
        {
            m0 = srcToUse;
            srcToUse = createNullSrc(Type_UD);
        }
        last_inst = Create_SplitSend_Inst_For_CISA(
            pred,                       //predicate
            createNullDst(Type_UD),     //dst
            m0,                         //src1
            srcToUse,                   //src2
            execSize,                   //execsize
            msgDesc,
            instOpt,                    //instOpt
            true );                     //is_sendc
    }
    else
    {
        G4_SrcRegRegion *m = srcToUse;
        if (useHeader)
            m = Create_Src_Opnd_From_Dcl(msg, getRegionStride1());
        last_inst = Create_Send_Inst_For_CISA( pred, createNullDst( Type_UD ), m, numRows, 0,
            execSize, fc, SFID_DP_WRITE, false, useHeader, false, true, surface, NULL, instOpt, true );
    }
    return CM_SUCCESS;

}

// return the contents of M0.2 for sampler messages.  It must be an immediate value
static uint32_t createSampleHeader0Dot2(VISASampler3DSubOpCode op,
                                        bool pixelNullMask,
                                        uint16_t aoffimmi,
                                        ChannelMask channels,
                                        IR_Builder* builder)
{
    uint32_t secondDword = aoffimmi & 0xfff;
    switch (op)
    {
    case VISA_3D_GATHER4:
    case VISA_3D_GATHER4_PO:
        //gather4 source channel select
        secondDword |= (channels.getSingleChannel() << 16);
        break;
    case VISA_3D_GATHER4_C:
    case VISA_3D_GATHER4_PO_C:
        // do nothing as channle must be Red (0)
        break;
    default:
        // RGBA write channel mask
        secondDword |= ( channels.getHWEncoding() << 12 );
        break;
    }

    // M0.2:23, Pixel Null Mask Enable.
    // Only valid for SKL+, and ignored otherwise.
    if (builder->hasPixelNullMask() && pixelNullMask) 
    {
        secondDword |= 1 << 23;
    }

    return secondDword;
}

//
// Coarse Pixel Shading(CPS) LOD compensation enable.
//
// - must be disabled if the response length of the message is zero;
// - must be disabled if the messages is from a 32-pixel dispatch thread;
// - must be disabled unless SIMD Mode is SIMD8* or SIMD16*;
// - only available for sample, sample_b, sample_bc, sample_c, and LOD.
//
static void checkCPSEnable(VISASampler3DSubOpCode op,
                           unsigned reponseLength,
                           unsigned execSize)
{

    ASSERT_USER(reponseLength > 0,
               "CPS LOD Compensation Enable must be disabled if the "
               "response length is zero");

    ASSERT_USER(execSize == 8 || execSize == 16,
                "CPS LOD Compensation Enable only valid for SIMD8* or SIMD16*");

    ASSERT_USER(op == VISA_3D_SAMPLE ||
                op == VISA_3D_SAMPLE_B ||
                op == VISA_3D_SAMPLE_C ||
                op == VISA_3D_SAMPLE_B_C ||
                op == VISA_3D_LOD,
                "CPD LOD Compensation Enable only available for "
                "sample, sample_b, sample_bc, sample_c and LOD");
}

#define CPS_LOD_COMPENSATION_ENABLE 11

static G4_Operand* createSampleHeader(IR_Builder* builder, G4_Declare* header, VISASampler3DSubOpCode actualop,
    bool pixelNullMask, G4_Operand* aoffimmi, ChannelMask srcChannel, G4_Operand* sampler)
{

    G4_Operand* retSampler = sampler;
    uint16_t aoffimmiVal = aoffimmi->isImm() ? (uint16_t)aoffimmi->asImm()->getInt() : 0;

    unsigned int secondDword = createSampleHeader0Dot2(actualop, pixelNullMask, aoffimmiVal, srcChannel, builder);

    G4_Imm* immOpndSecondDword = builder->createImm(secondDword, Type_UD);
    G4_DstRegRegion* payloadDstRgn = builder->createDstRegRegion(Direct, header->getRegVar(), 0, 2, 1, Type_UD);
    if (aoffimmi->isImm())
    {
        // mov (1) payload(0,2) immOpndSecondDword
        builder->createInst(NULL, G4_mov, NULL, false, 1, payloadDstRgn,
            immOpndSecondDword, NULL, NULL, InstOpt_WriteEnable, 0);
    }
    else
    {
        // or (1) payload(0,2) aoffimmi<0;1,0>:uw immOpndSeconDword
        builder->createInst(nullptr, G4_or, nullptr, false, 1, payloadDstRgn,
            aoffimmi, immOpndSecondDword, nullptr, InstOpt_WriteEnable, 0);
    }

    if (sampler != nullptr)
    {
        builder->doSamplerHeaderMove(header, sampler);

        // Use bit 15 of aoffimmi to tell VISA the sample index could be greater
        // than 15.  In this case, we need to use msg header, and setup M0.3
        // to point to next 16 sampler state.
        if (aoffimmiVal & 0x8000)
        {
            retSampler = builder->emitSampleIndexGE16(sampler, header);
        }
    }

    return retSampler;
}

static bool needsNoMaskCoordinates(VISASampler3DSubOpCode opcode)
{
    return opcode == VISA_3D_SAMPLE || opcode == VISA_3D_SAMPLE_B || opcode == VISA_3D_SAMPLE_C ||
        opcode == VISA_3D_SAMPLE_B_C || opcode == VISA_3D_LOD || opcode == VISA_3D_SAMPLE_KILLPIX;
}

static uint8_t getUPosition(VISASampler3DSubOpCode opcode)
{
    uint8_t position = 0;
    switch (opcode)
    {
    case VISA_3D_SAMPLE:
    case VISA_3D_LOD:
    case VISA_3D_SAMPLE_D:
    case VISA_3D_SAMPLE_LZ:
    case VISA_3D_SAMPLE_KILLPIX:
        position = 0;
        break;
    case VISA_3D_SAMPLE_B:
    case VISA_3D_SAMPLE_L:
    case VISA_3D_SAMPLE_C:
    case VISA_3D_SAMPLE_D_C:
    case VISA_3D_SAMPLE_C_LZ:
        position = 1;
        break;
    case VISA_3D_SAMPLE_B_C:
    case VISA_3D_SAMPLE_L_C:
        position = 2;
        break;
    default:
        MUST_BE_TRUE(false, "unexpected sampler operation");
        return 0;
    }
    return position;
}

static void setUniformSampler(G4_INST* sendInst, bool uniformSampler)
{
    assert(sendInst->isSend() && "expect send inst");
}

/*
Need to split sample_d and sample_dc in to two simd8 sends since HW doesn't support it.
Also need to split any sample instruciton that has more then 5 parameters. Since there is a limit on msg length.
*/
static int splitSampleInst(VISASampler3DSubOpCode actualop,
                           bool pixelNullMask,
                           bool cpsEnable,
                           G4_Predicate* pred,
                           ChannelMask srcChannel,
                           int numChannels,
                           G4_Operand *aoffimmi,
                           G4_Operand *sampler,
                           G4_Operand *surface,
                           G4_DstRegRegion* dst,
                           Common_VISA_EMask_Ctrl emask,
                           bool useHeader,
                           unsigned numRows, // msg length for each simd8
                           IR_Builder *builder,
                           unsigned int numParms,
                           G4_SrcRegRegion ** params,
                           bool uniformSampler = true)
{
    int status = CM_SUCCESS;
    G4_SrcRegRegion *secondHalf[12];

    //FIXME: consider enabling split send for this function too

    bool isHalfReturn = G4_Type_Table[dst->getType()].byteSize == 2;
    const bool halfInput = G4_Type_Table[params[0]->getType()].byteSize == 2;

    // Now, depending on message type emit out parms to payload
    unsigned regOff = ( useHeader ? 1 : 0 );
    G4_SrcRegRegion* temp = NULL;
    uint8_t execSize = 8;
    uint16_t numElts = numRows * GENX_GRF_REG_SIZ/G4_Type_Table[Type_F].byteSize;
    G4_Declare* payloadF = builder->Create_MRF_Dcl( numElts, Type_F );
    G4_Declare* payloadUD = builder->createTempVar( numElts, Type_UD, Either, Sixteen_Word );
    payloadUD->setAliasDeclare( payloadF, 0 );
    G4_SrcRegRegion* srcToUse = builder->createSrcRegRegion(Mod_src_undef, Direct, payloadUD->getRegVar(), 0, 0, builder->getRegionStride1(), Type_UD);

    // even though we only use lower half of the GRF, we have to allocate full GRF
    G4_Declare* payloadHF = builder->createTempVar(numElts * 2, Type_HF, Either, Any);
    payloadHF->setAliasDeclare( payloadF, 0 );

    /********* Creating temp destination, since results are interleaved **************/
    G4_DstRegRegion *dst1 = builder->createNullDst(dst->getType());;
    G4_Declare * originalDstDcl = nullptr;
    G4_Declare* tempDstDcl = nullptr;
    bool pixelNullMaskEnable = false;
    unsigned tmpDstRows = 0;
    if(!dst->isNullReg())
    {
        originalDstDcl = dst->getBase()->asRegVar()->getDeclare();
        tmpDstRows = numChannels;

        // If Pixel Null Mask is enabled, then one extra GRF is needed for the
        // write back message.
        pixelNullMaskEnable = builder->hasPixelNullMask() && pixelNullMask;
        if(pixelNullMaskEnable) {
            ASSERT_USER(useHeader, "pixel null mask requires a header");
            ++tmpDstRows;
        }

        tempDstDcl = builder->createDeclareNoLookup("TmpSmplDst",
            originalDstDcl->getRegFile(),
            originalDstDcl->getNumElems(),
            (uint16_t)tmpDstRows,
            originalDstDcl->getElemType());

        dst1 = builder->createDstRegRegion(dst->getRegAccess(),
            tempDstDcl->getRegVar(),
            0,
            0,
            1,
            dst->getType());
    }
    /********* End creating temp destination ***********************/
    if (useHeader)
    {
        builder->Create_MOVR0_Inst(payloadUD, 0, 0, true);
        sampler = createSampleHeader(builder, payloadUD, actualop, pixelNullMask, aoffimmi, srcChannel, sampler);
    }

    uint32_t instOpt = Get_Gen4_Emask( emask, execSize );
    for (unsigned paramCounter = 0; paramCounter < numParms; ++paramCounter)
    {
        temp = params[paramCounter];
        uint32_t MovInstOpt = InstOpt_WriteEnable;
        if (G4_Type_Table[temp->getType()].byteSize == 2)
        {
            // we should generate
            // mov (8) dst<1>:hf src.0<8;8,1>:hf
            G4_DstRegRegion* dstHF = builder->createDstRegRegion(
                Direct, payloadHF->getRegVar(), regOff++, 0, 1, temp->getType());
            temp->setRegion(builder->getRegionStride1());
            builder->createInst(NULL, G4_mov, NULL, false, 8, dstHF, temp, NULL, MovInstOpt);
        }
        else
        {
            builder->Copy_SrcRegRegion_To_Payload( payloadF, regOff, temp, execSize, MovInstOpt );
        }
    }

    // For SKL+, if Pixel Null Mask is enabled, then the response length must
    // be set to a value one larger.
    //
    // FIXME: numChannels is in fact the reponse length, which is misleading.
    unsigned int reponseLength = dst->isNullReg() ? 0 : numChannels;
    if (pixelNullMaskEnable) {
        ++reponseLength;
    }

    uint32_t fc = createSamplerMsgDesc(actualop, execSize, isHalfReturn, halfInput);
    uint32_t desc = G4_SendMsgDescriptor::createDesc(fc, useHeader, numRows, reponseLength);
    uint32_t extDesc = G4_SendMsgDescriptor::createExtDesc(SFID_SAMPLER);

    if (cpsEnable)
    {
        checkCPSEnable(actualop, reponseLength, 8);
        extDesc |= (1 << CPS_LOD_COMPENSATION_ENABLE);
    }
    G4_SendMsgDescriptor *msgDesc = builder->createSendMsgDesc(desc, extDesc, true, false, surface, sampler);

    G4_INST* sendInst = nullptr;
    bool forceSplitSend = IsBindlessSurface(*builder, surface);

    if (forceSplitSend)
    {
        sendInst = builder->Create_SplitSend_Inst_For_CISA(
            pred, dst1, srcToUse, builder->createNullSrc(Type_UD), execSize, msgDesc, instOpt, false);
    }
    else
    {
        sendInst = builder->Create_Send_Inst_For_CISA(
            pred, dst1, srcToUse, execSize, msgDesc, instOpt, false);
    }
    setUniformSampler(sendInst, uniformSampler);

    // SKL+
    // For SIMD8
    //
    // W4.7:1 Reserved (not written): This W4 is only delivered when Pixel Null
    //        Mask Enable is enabled.
    //
    // W4.0  32:8 Reserved: always written as 0xffffff
    //        7:0 Pixel Null Mask: This field has the bit for all pixels set
    //            to 1 except those pixels in which a null page was source for
    //            at least one texel.
    //
    // Need to combine the results from the above two writewback messages.
    // Denote by U0[W4:0] the last row of the first writeback message, and
    // by U1[W4:0] the last row of the second writeback message. Then the last
    // row of the whole writeback message is to take the bitwise OR of
    // U0[W4:0] and U1[W4:0].
    G4_Declare *tempDstUD = 0;
    G4_Declare *tempDst2UD = 0;
    G4_Declare *origDstUD = 0;

    // temp dst for the second send
    G4_DstRegRegion *dst2 = builder->createNullDst(dst->getType());
    G4_Declare* tempDstDcl2 = nullptr;
    if(!dst->isNullReg())
    {
        tempDstDcl2 = builder->createDeclareNoLookup("TmpSmplDst2",
            originalDstDcl->getRegFile(),
            originalDstDcl->getNumElems(),
            (uint16_t)tmpDstRows,
            originalDstDcl->getElemType());

        if(pixelNullMaskEnable)
        {
            unsigned int numElts = tempDstDcl->getNumElems() * tempDstDcl->getNumRows();
            tempDstUD = builder->createTempVar(numElts, Type_UD, Either, Sixteen_Word);
            tempDstUD->setAliasDeclare(tempDstDcl, 0);

            numElts = tempDstDcl2->getNumElems() * tempDstDcl2->getNumRows();
            tempDst2UD = builder->createTempVar(numElts, Type_UD, Either, Sixteen_Word);
            tempDst2UD->setAliasDeclare(tempDstDcl2, 0);

            numElts = originalDstDcl->getNumElems() * originalDstDcl->getNumRows();
            origDstUD = builder->createTempVar(numElts, Type_UD, Either, Sixteen_Word);
            origDstUD->setAliasDeclare(originalDstDcl, 0);
        }

        dst2 = builder->createDstRegRegion(dst->getRegAccess(),
            tempDstDcl2->getRegVar(),
            0,
            0,
            1,
            dst->getType());

    }
    // update emask
    emask = Get_Next_EMask(emask, execSize);
    uint32_t instOpt2 = Get_Gen4_Emask(emask, execSize);

    G4_SrcRegRegion* header = builder->Create_Src_Opnd_From_Dcl(payloadUD, builder->getRegionStride1());
    {
        /**************** SECOND HALF OF THE SEND *********************/
        // re-create payload declare so the two sends may be issued independently
        G4_Declare* payloadF = builder->Create_MRF_Dcl(numElts, Type_F);
        G4_Declare* payloadUD = builder->createTempVar(numElts, Type_UD, Either, Sixteen_Word);
        payloadUD->setAliasDeclare(payloadF, 0);

        // even though we only use lower half of the GRF, we have to allocate full GRF
        G4_Declare* payloadHF = builder->createTempVar(numElts * 2, Type_HF, Either, Any);
        payloadHF->setAliasDeclare(payloadF, 0);

        G4_SrcRegRegion *srcToUse2 = builder->createSrcRegRegion(Mod_src_undef, Direct, payloadUD->getRegVar(), 0, 0, builder->getRegionStride1(), Type_UD);

        if (useHeader)
        {
            builder->Create_MOV_Inst(payloadUD, 0, 0, 8, nullptr, nullptr, header, true);
        }

        for (unsigned int i = 0; i < numParms; i++)
        {
            if (params[i]->isNullReg())
            {
                secondHalf[i] = params[i];
            }
            else if (G4_Type_Table[params[i]->getType()].byteSize == 2)
            {
                // V1(0,8)<8;8,1>
                secondHalf[i] = builder->createSrcRegRegion(*(params[i]));
                secondHalf[i]->setSubRegOff(8);
            }
            else
            {
                // V1(1,0)<8;8,1>
                secondHalf[i] = builder->createSrcRegRegion(Mod_src_undef,
                    params[i]->getRegAccess(),
                    params[i]->getBase(),
                    params[i]->getRegOff() + 1,
                    params[i]->getSubRegOff(),
                    params[i]->getRegion(),
                    params[i]->getType());
            }
        }

        regOff = (useHeader ? 1 : 0);
        for (unsigned paramCounter = 0; paramCounter < numParms; ++paramCounter)
        {
            temp = secondHalf[paramCounter];
            uint32_t MovInstOpt = InstOpt_WriteEnable;

            if (G4_Type_Table[temp->getType()].byteSize == 2)
            {
                // we should generate
                // mov (8) dst<1>:hf src.8<8;8,1>:hf
                G4_DstRegRegion* dstHF = builder->createDstRegRegion(
                    Direct, payloadHF->getRegVar(), regOff++, 0, 1, temp->getType());
                builder->createInst(NULL, G4_mov, NULL, false, execSize, dstHF, temp, NULL, MovInstOpt);
            }
            else
            {
                builder->Copy_SrcRegRegion_To_Payload(payloadF, regOff, temp, execSize, MovInstOpt);
            }
        }

        G4_Operand *surface2 = builder->duplicateOperand(surface);

        // sampler may be null for 3d load (specifically ld2dms_w)
        G4_Operand* sampler2 = sampler == nullptr ? nullptr : builder->duplicateOperand(sampler);

        G4_Predicate*   pred2 = NULL;
        if (pred != NULL)
        {
            pred2 = builder->createPredicate(
                pred->getState(),
                pred->getBase(),
                0);
        }

        G4_SendMsgDescriptor *msgDesc2 = builder->createSendMsgDesc(desc, extDesc, true, false, surface2, sampler2);
        msgDesc2->setHeaderPresent(useHeader);

        if (forceSplitSend)
        {
            sendInst = builder->Create_SplitSend_Inst_For_CISA(
                pred, dst2, srcToUse2, builder->createNullSrc(Type_UD), execSize, msgDesc2, instOpt2, false);
        }
        else
        {
            sendInst = builder->Create_Send_Inst_For_CISA(
                pred2, dst2, srcToUse2, execSize, msgDesc2, instOpt2, false);
        }
        setUniformSampler(sendInst, uniformSampler);
    }

    {

        /**************** MOVING FROM TEMP TO DST, 1st half *********************/
        regOff = 0;
        for (unsigned i = 0; i < tmpDstRows; i++, regOff += 1)
        {
            // If Pixel Null Mask is enabled, then only copy the last double word.
            if (pixelNullMaskEnable && i == tmpDstRows - 1)
            {
                G4_DstRegRegion *origDstPtr = builder->createDstRegRegion(Direct, origDstUD->getRegVar(), short(regOff), 0, 1, Type_UD);
                G4_SrcRegRegion *src0Ptr = builder->createSrcRegRegion(Mod_src_undef, Direct, tempDstUD->getRegVar(),
                    short(i), 0, builder->getRegionScalar(),
                    Type_UD);

                // Copy the write mask message W4.0 into the dst. (No mask?)
                builder->createInst(pred, G4_mov, NULL, false, 1, origDstPtr, src0Ptr,
                    NULL, NULL, InstOpt_WriteEnable, 0);
                // Skip the remaining part of the loop.
                break;
            }

            G4_SrcRegRegion *tmpSrcPnt = builder->createSrcRegRegion(
                Mod_src_undef, Direct, tempDstDcl->getRegVar(), (short)i, 0, builder->getRegionStride1(), tempDstDcl->getElemType());

            uint32_t MovInstOpt = instOpt;
            if (isHalfReturn)
            {
                // mov (8) dst(0,0)<1>:hf tmp(0,0)<8;8,1>:hf {Q1}
                G4_DstRegRegion* dst = builder->createDstRegRegion(Direct,
                    originalDstDcl->getRegVar(), (short)regOff, 0, 1, originalDstDcl->getElemType());
                builder->createInst(NULL, G4_mov, NULL, false, execSize, dst, tmpSrcPnt, NULL, MovInstOpt);
            }
            else
            {
                builder->Copy_SrcRegRegion_To_Payload(originalDstDcl, regOff, tmpSrcPnt, execSize, MovInstOpt);
            }
        }
    }

    {
        /**************** MOVING FROM TEMP TO DST, 2nd half *********************/
        regOff = isHalfReturn ? 0 : 1;
        for (unsigned i = 0; i < tmpDstRows; i++, regOff += 1)
        {
            // If Pixel Null Mask is enabled, write the 8 bits to bits 8-15 in the originai dst
            if (pixelNullMaskEnable && i == tmpDstRows - 1) {
                G4_DstRegRegion *origDstPtr = builder->createDstRegRegion(Direct, origDstUD->getRegVar(), regOff - 1, 1, 1, Type_UB);
                G4_SrcRegRegion *src0Ptr = builder->createSrcRegRegion(Mod_src_undef, Direct, tempDst2UD->getRegVar(),
                    short(i), 0, builder->getRegionScalar(),
                    Type_UB);
                // write to dst.0[8:15]
                builder->createInst(pred, G4_mov, NULL, false, 1, origDstPtr, src0Ptr, NULL, InstOpt_WriteEnable);

                // Skip the remaining part of the loop.
                break;
            }

            G4_SrcRegRegion *tmpSrcPnt = builder->createSrcRegRegion(
                Mod_src_undef, Direct, tempDstDcl2->getRegVar(), (short)i, 0, builder->getRegionStride1(), tempDstDcl->getElemType());

            uint32_t MovInstOpt = instOpt2;
            if (isHalfReturn)
            {
                // mov (8) dst(0,8)<1>:hf tmp(0,0)<8;8,1>:hf {Q2}
                G4_DstRegRegion* dst = builder->createDstRegRegion(Direct,
                    originalDstDcl->getRegVar(), (short)regOff, 8, 1, originalDstDcl->getElemType());
                builder->createInst(NULL, G4_mov, NULL, false, execSize, dst, tmpSrcPnt, NULL, MovInstOpt);
            }
            else
            {
                builder->Copy_SrcRegRegion_To_Payload(originalDstDcl, regOff, tmpSrcPnt, execSize, MovInstOpt);
            }
        }
    }
    return status;
}

void IR_Builder::doSamplerHeaderMove(G4_Declare* headerDcl, G4_Operand* sampler)
{
	if (isBindlessSampler(sampler))
	{
        // sampler index in msg desc will be 0, manipulate the sampler offset instead
        // mov (1) M0.3<1>:ud sampler<0;1,0>:ud the driver will send the handle with bit 0 already set
        G4_DstRegRegion* dst = createDstRegRegion(Direct, headerDcl->getRegVar(), 0, 3, 1, Type_UD);
        createInst(nullptr, G4_mov, nullptr, false, 1, dst, sampler, nullptr, InstOpt_WriteEnable);
	}
}

//
// generate the r0 move for the sampler message header, and return the dcl
// for CNL+, also set SSP to dynamic if message is not bindless
//
G4_Declare* IR_Builder::getSamplerHeader(bool isBindlessSampler)
{
    G4_Declare* dcl = nullptr;
    if (m_options->getOption(vISA_cacheSamplerHeader) && !isBindlessSampler)
    {
        dcl = builtinSamplerHeader;
        if (!usesSampler)
        {
            usesSampler = true;
            if (hasBindlessSampler())
            {
                // make sure we set bit 0 of M0.3:ud to be 0
                // and (1) M0.6<1>:uw M0.6<1>:uw 0xFFFE
                G4_DstRegRegion* dst = createDstRegRegion(Direct, dcl->getRegVar(), 0, 6, 1, Type_UW);
                G4_SrcRegRegion* src0 = createSrcRegRegion(Mod_src_undef, Direct, dcl->getRegVar(), 0, 6, getRegionScalar(), Type_UW);
                G4_INST* SSPMove = createInternalInst(nullptr, G4_and, nullptr, false, 1, dst, src0,
                    createImm(0xFFFE, Type_UW), InstOpt_WriteEnable);
                instList.push_front(SSPMove);
            }
            G4_INST* r0Move = createInternalInst(nullptr, G4_mov, nullptr, false, 8,
                Create_Dst_Opnd_From_Dcl(dcl, 1),
                Create_Src_Opnd_From_Dcl(builtinR0, getRegionStride1()),
                nullptr, InstOpt_WriteEnable);
            instList.push_front(r0Move);
        }
    }
    else
    {
        dcl = Create_MRF_Dcl(8, Type_UD);
        dcl->setCapableOfReuse();
        Create_MOVR0_Inst(dcl, 0, 0, true);
        if (hasBindlessSampler() && !isBindlessSampler)
        {
            // make sure we set bit 0 of M0.3:ud to be 0
            // and (1) M0.6<1>:uw M0.6<1>:uw 0xFFFE
            G4_DstRegRegion* dst = createDstRegRegion(Direct, dcl->getRegVar(), 0, 6, 1, Type_UW);
            G4_SrcRegRegion* src0 = createSrcRegRegion(Mod_src_undef, Direct, dcl->getRegVar(), 0, 6, getRegionScalar(), Type_UW);
            createInst(nullptr, G4_and, nullptr, false, 1, dst, src0, createImm(0xFFFE, Type_UW), InstOpt_WriteEnable);
        }
    }

    return dcl;
}

uint32_t IR_Builder::getSamplerResponseLength(int numChannels, bool isFP16, int execSize, 
    bool pixelNullMask, bool nullDst)
{
    if (nullDst)
    {
        hasNullReturnSampler = true;
        return 0;
    }
    uint32_t responseLength = (isFP16 || execSize == 8) ? numChannels : numChannels * 2;

    if (pixelNullMask)
    {
        ++responseLength;
    }
    return responseLength;
}

static bool needSamplerHeader(IR_Builder* builder, bool pixelNullMask, bool nonZeroAoffImmi,
    bool needHeaderForChannels, bool bindlessSampler, bool simd16HFReturn)
{
    return builder->forceSamplerHeader() ||
        (pixelNullMask && builder->hasPixelNullMask()) ||
        nonZeroAoffImmi || needHeaderForChannels || bindlessSampler ||
        (simd16HFReturn && VISA_WA_CHECK(builder->getPWaTable(), WaHeaderRequiredOnSimd16Sample16bit));
}

/*
This function assumes there are no gaps in parameter array. e.g. NULL pointers
If there is a gap it must be RawOperand with value 0.
*/

int IR_Builder::translateVISASampler3DInst(
    VISASampler3DSubOpCode actualop,
    bool pixelNullMask,
    bool cpsEnable,
    bool uniformSampler,
    G4_Predicate* pred,
    Common_ISA_Exec_Size executionSize,
    Common_VISA_EMask_Ctrl emask,
    ChannelMask chMask,
    G4_Operand *aoffimmi,
    G4_Operand *sampler,
    G4_Operand *surface,
    G4_DstRegRegion* dst,
    unsigned int numParms,
    G4_SrcRegRegion ** params)
{
    /*
    in vISA      1 means channel will be written, 0 means channel will not be written
    in GEN ISA   0 means channel will be written, 1 means channel will not be written
    */
    uint8_t execSize = (uint8_t) Get_Common_ISA_Exec_Size( executionSize );
    uint32_t instOpt = Get_Gen4_Emask( emask, execSize );

    // First setup message header and message payload

    // Message header and payload size is numParms GRFs

    const bool FP16Return = G4_Type_Table[dst->getType()].byteSize == 2;
    const bool FP16Input = params[0]->getType() == Type_HF;

    bool useHeader = false;
    unsigned int numRows = 0;

    if (FP16Input)
    {
        numRows = numParms;
    }
    else
    {
        numRows = numParms * (execSize == 8 ? 1 : 2);
    }

    VISAChannelMask channels = chMask.getAPI();
    // For SKL+ channel mask R, RG, RGB, and RGBA may be derived from response length
    bool needHeaderForChannels = (getGenxPlatform() < GENX_SKL) ? channels != CHANNEL_MASK_RGBA :
        (channels != CHANNEL_MASK_R && channels != CHANNEL_MASK_RG && channels != CHANNEL_MASK_RGB && channels != CHANNEL_MASK_RGBA);

    bool nonZeroAoffImmi = !(aoffimmi->isImm() && aoffimmi->asImm()->getInt() == 0);
    bool simd16HFReturn = FP16Return && execSize == 16;
    if (needSamplerHeader(this, pixelNullMask, nonZeroAoffImmi, needHeaderForChannels,
        isBindlessSampler(sampler), simd16HFReturn))
    {
        useHeader = true;
        ++numRows;
    }

    int numChannels = chMask.getNumEnabledChannels();

    if (execSize == 16 &&
        (numRows > 11 || actualop == VISA_3D_SAMPLE_D || actualop == VISA_3D_SAMPLE_D_C || actualop == VISA_3D_SAMPLE_KILLPIX))
    {
        // decrementing since we will produce SIMD8 code.
        // don't do this for SIMD16H since its message length is the same as SIMD8H
        if (!FP16Input)
        {
            numRows -= numParms;
        }

        return splitSampleInst(actualop, pixelNullMask, cpsEnable, pred, chMask,
                               numChannels, aoffimmi, sampler, surface, dst,
                               emask, useHeader, numRows, this, numParms, params, uniformSampler);
    }

    bool useSplitSend = useSends();

    G4_SrcRegRegion *header = 0;
    G4_Operand* samplerIdx = sampler;

    if( useHeader )
    {
        G4_Declare *dcl = getSamplerHeader(isBindlessSampler(sampler));
        samplerIdx = createSampleHeader(this, dcl, actualop, pixelNullMask, aoffimmi, chMask, sampler);
        header = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
    }

    // Collect payload sources.
    unsigned len = numParms + (header ? 1 : 0);
	std::vector<payloadSource> sources(len);
    unsigned i = 0;
    // Collect header if present.
    if (header) {
        sources[i].opnd = header;
        sources[i].execSize = 8;
        sources[i].instOpt = InstOpt_WriteEnable;
        ++i;
    }
    // Collect all parameters.
    bool needNoMask = needsNoMaskCoordinates(actualop);
    unsigned uPos = needNoMask ? getUPosition(actualop) : ~0u;
    for (unsigned j = 0; j != numParms; ++j) {
        sources[i].opnd = params[j];
        sources[i].execSize = execSize;
        sources[i].instOpt = (needNoMask && (uPos <= j && j < (uPos + 3))) ?
                             InstOpt_WriteEnable : instOpt;
        ++i;
    }
    ASSERT_USER(i == len, "There's mismatching during payload source collecting!");

    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, execSize, useSplitSend, sources.data(), len);

    uint32_t responseLength = getSamplerResponseLength(numChannels, FP16Return, execSize,
        hasPixelNullMask() && pixelNullMask, dst->isNullReg());

    // Check if CPS LOD Compensation Enable is valid.
    if (cpsEnable)
    {
        checkCPSEnable(actualop, responseLength, execSize);
    }

    uint32_t fc = createSamplerMsgDesc(actualop, execSize, FP16Return, FP16Input);
    uint32_t desc = G4_SendMsgDescriptor::createDesc(fc, useHeader, sizes[0], responseLength);

	bool forceSplitSend = IsBindlessSurface(*this, surface);
	if (msgs[1] == 0 && !forceSplitSend)
    {
        ASSERT_USER(sizes[1] == 0, "Expect the 2nd part of the payload has zero size!");
        uint32_t extDesc = G4_SendMsgDescriptor::createExtDesc(SFID_SAMPLER);
        if (cpsEnable)
        {
            extDesc |= (1 << CPS_LOD_COMPENSATION_ENABLE);
        }
        G4_SendMsgDescriptor *msgDesc = createSendMsgDesc(desc, extDesc, true, false, surface, samplerIdx);

        last_inst = Create_Send_Inst_For_CISA(pred, dst, msgs[0], execSize,
                                              msgDesc, instOpt, false);
    }
    else
    {
        uint32_t extDesc = G4_SendMsgDescriptor::createExtDesc(SFID_SAMPLER, false, sizes[1]);
        if (cpsEnable)
        {
            extDesc |= (1 << CPS_LOD_COMPENSATION_ENABLE);
        }
        G4_SendMsgDescriptor *msgDesc = createSendMsgDesc(desc, extDesc, true, false, surface, samplerIdx);
        last_inst = Create_SplitSend_Inst_For_CISA(pred, dst, msgs[0], msgs[1],
                                                   execSize, msgDesc, instOpt, false);
    }
    setUniformSampler(last_inst, uniformSampler);
    return CM_SUCCESS;
}

int IR_Builder::translateVISALoad3DInst(
    VISASampler3DSubOpCode actualop,
    bool pixelNullMask,
    G4_Predicate *pred_opnd,
    Common_ISA_Exec_Size executionSize,
    Common_VISA_EMask_Ctrl em,
    ChannelMask channelMask,
    G4_Operand* aoffimmi,
    G4_Operand* surface,
    G4_DstRegRegion* dst,
    uint8_t numParms,
    G4_SrcRegRegion ** opndArray)
{
    bool useHeader = false;

    uint8_t execSize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    uint32_t instOpt = Get_Gen4_Emask(em, execSize);

    const bool halfReturn = G4_Type_Table[dst->getType()].byteSize == 2;
    const bool halfInput = G4_Type_Table[opndArray[0]->getType()].byteSize == 2;

    unsigned int numRows = numParms * ((halfInput || execSize == 8) ? 1 : 2);

    VISAChannelMask channels = channelMask.getAPI();
    // For SKL+ channel mask R, RG, RGB, and RGBA may be derived from response length
    bool needHeaderForChannels = (getGenxPlatform() < GENX_SKL) ? channels != CHANNEL_MASK_RGBA :
        (channels != CHANNEL_MASK_R && channels != CHANNEL_MASK_RG && channels != CHANNEL_MASK_RGB && channels != CHANNEL_MASK_RGBA);

    bool nonZeroAoffImmi = !(aoffimmi->isImm() && aoffimmi->asImm()->getInt() == 0);
    bool simd16HFReturn = halfReturn && execSize == 16;
    if (needSamplerHeader(this, pixelNullMask, nonZeroAoffImmi, needHeaderForChannels, false,
        simd16HFReturn))
    {
        useHeader = true;
        ++numRows;
    }

    int numChannels = channelMask.getNumEnabledChannels();
    if (execSize == 16 && numRows > 11)
    {
        // decrementing since we will produce SIMD8 code.
        // don't do this for SIMD16H since its message length is the same as SIMD8H
        if (!halfInput)
        {
            numRows -= numParms;
        }
        return splitSampleInst(actualop, pixelNullMask, /*cpsEnable*/false,
                               pred_opnd, channelMask, numChannels, aoffimmi,
                               NULL, surface, dst, em, useHeader, numRows,
                               this, numParms, opndArray);
    }

    bool useSplitSend = useSends();

    G4_SrcRegRegion *header = 0;
    if (useHeader)
    {
        G4_Declare *dcl = getSamplerHeader(false);
        (void) createSampleHeader(this, dcl, actualop, pixelNullMask, aoffimmi, channelMask, nullptr);
        header = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
    }

    // Collect payload sources.
    unsigned len = numParms + (header ? 1 : 0);
	std::vector<payloadSource> sources(len);
    unsigned i = 0;
    // Collect header if present.
    if (header) {
        sources[i].opnd = header;
        sources[i].execSize = 8;
        sources[i].instOpt = InstOpt_WriteEnable;
        ++i;
    }
    // Collect all parameters.
    bool needNoMask = needsNoMaskCoordinates(actualop);
    unsigned uPos = needNoMask ? getUPosition(actualop) : ~0u;
    for (unsigned j = 0; j != numParms; ++j) {
        sources[i].opnd = opndArray[j];
        sources[i].execSize = execSize;
        sources[i].instOpt = (needNoMask && (uPos <= j && j < (uPos + 3))) ?
                             InstOpt_WriteEnable : instOpt;
        ++i;
    }
    ASSERT_USER(i == len, "There's mismatching during payload source collecting!");

    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, execSize, useSplitSend, sources.data(), len);

    uint32_t fc = createSamplerMsgDesc(actualop, execSize, halfReturn, halfInput);

    uint32_t responseLength = getSamplerResponseLength(numChannels, halfReturn, execSize,
        hasPixelNullMask() && pixelNullMask, dst->isNullReg());

	bool forceSplitSend = IsBindlessSurface(*this, surface);
	if (msgs[1] == 0 && !forceSplitSend)
    {
        last_inst = Create_Send_Inst_For_CISA(pred_opnd, dst,
                                              msgs[0], sizes[0],
                                              responseLength,
                                              execSize, fc, SFID_SAMPLER,
                                              false, useHeader,
                                              true, false, surface, NULL,
                                              instOpt, false);
    }
    else
    {
        last_inst = Create_SplitSend_Inst_For_CISA(pred_opnd, dst,
                                                   msgs[0], sizes[0], msgs[1], sizes[1],
                                                   responseLength,
                                                   execSize, fc, 0, SFID_SAMPLER,
                                                   false, useHeader,
                                                   true, false,
                                                   surface, NULL,
                                                   instOpt, false);
    }

    return CM_SUCCESS;
}

int IR_Builder::translateVISAGather3dInst(
    VISASampler3DSubOpCode actualop,
    bool pixelNullMask,
    G4_Predicate* pred,
    Common_ISA_Exec_Size executionSize,
    Common_VISA_EMask_Ctrl em,
    ChannelMask channelMask,
    G4_Operand* aoffimmi,
    G4_Operand* sampler,
    G4_Operand* surface,
    G4_DstRegRegion* dst,
    unsigned int numOpnds,
    G4_SrcRegRegion ** opndArray )
{
    bool useHeader = false;

    uint8_t execSize = (uint8_t) Get_Common_ISA_Exec_Size(executionSize);
    uint32_t instOpt = Get_Gen4_Emask(em, execSize);

    const bool FP16Return = G4_Type_Table[dst->getType()].byteSize == 2;
    const bool FP16Input = opndArray[0]->getType() == Type_HF;

    unsigned int numRows = 0;

    if (FP16Input)
    {
        numRows = numOpnds;
    }
    else
    {
        numRows = numOpnds * (execSize == 8 ? 1 : 2);
    }

    bool nonZeroAoffImmi = !(aoffimmi->isImm() && aoffimmi->asImm()->getInt() == 0);
    bool needHeaderForChannels = channelMask.getSingleChannel() != VISA_3D_GATHER4_CHANNEL_R;
    bool simd16HFReturn = FP16Return && execSize == 16;

    if (needSamplerHeader(this, pixelNullMask, nonZeroAoffImmi, needHeaderForChannels,
        isBindlessSampler(sampler), simd16HFReturn))
    {
        useHeader = true;
        ++numRows;
    }

    if (execSize == 16 && numRows > 11)
    {
        // decrementing since we will produce SIMD8 code.
        // don't do this for SIMD16H since its message length is the same as SIMD8H
        if (!FP16Input)
        {
            numRows-= numOpnds;
        }

        return splitSampleInst(actualop, pixelNullMask, /*cpsEnable*/false,
                               pred, channelMask, 4, aoffimmi, sampler,
                               surface, dst, em, useHeader, numRows, this,
                               numOpnds, opndArray);
    }

    bool useSplitSend = useSends();

    G4_SrcRegRegion *header = 0;
    G4_Operand* samplerIdx = sampler;

    if (useHeader)
    {
        G4_Declare *dcl = getSamplerHeader(isBindlessSampler(sampler));
        samplerIdx = createSampleHeader(this, dcl, actualop, pixelNullMask, aoffimmi, channelMask, sampler);
        header = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
    }

    // Collect payload sources.
    unsigned len = numOpnds + (header ? 1 : 0);
	std::vector<payloadSource> sources(len);
    unsigned i = 0;
    // Collect header if present.
    if (header) {
        sources[i].opnd = header;
        sources[i].execSize = 8;
        sources[i].instOpt = InstOpt_WriteEnable;
        ++i;
    }
    // Collect all parameters.
    bool needNoMask = needsNoMaskCoordinates(actualop);
    unsigned uPos = needNoMask ? getUPosition(actualop) : ~0u;
    for (unsigned j = 0; j != numOpnds; ++j) {
        sources[i].opnd = opndArray[j];
        sources[i].execSize = execSize;
        sources[i].instOpt = (needNoMask && (uPos <= j && j < (uPos + 3))) ?
                             InstOpt_WriteEnable : instOpt;
        ++i;
    }
    ASSERT_USER(i == len, "There's mismatching during payload source collecting!");

    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, execSize, useSplitSend, sources.data(), len);

    uint32_t fc = createSamplerMsgDesc(actualop, execSize, FP16Return, FP16Input);
    uint32_t responseLength = getSamplerResponseLength(4, FP16Return, execSize,
        hasPixelNullMask() && pixelNullMask, dst->isNullReg());

	bool forceSplitSend = IsBindlessSurface(*this, surface);
	if (msgs[1] == 0 && !forceSplitSend)
    {
        last_inst = Create_Send_Inst_For_CISA(pred, dst, msgs[0], sizes[0],
                                              responseLength,
                                              execSize, fc, SFID_SAMPLER,
                                              false, useHeader,
                                              true, false,
                                              surface, samplerIdx,
                                              instOpt, false);
    }
    else
    {
        last_inst = Create_SplitSend_Inst_For_CISA(pred, dst,
                                                   msgs[0], sizes[0], msgs[1], sizes[1],
                                                   responseLength,
                                                   execSize, fc, 0, SFID_SAMPLER,
                                                   false, useHeader,
                                                   true, false,
                                                   surface, samplerIdx,
                                                   instOpt, false);
    }

    return CM_SUCCESS;
}

///
/// Bits 31-29: Reserved
/// Bits 28-25: Message Length: Total 256bit registers expected to be sent.
/// Bits 24-20: Response Length: Total 256bit registers expected in response.
/// Bit  19:    Does this Message Descriptor have a header? 1 Yes, 0 No.
/// Bits 18-14: Message Type: 10100: A64 Block Read, 10101: A64 Block Write
/// Bit  13:    Ignore
/// Bits 12-11: Message sub-type (00 for OWord Block Read/Write, 01 for Unaligned OWord Block Read/Write)
/// Bits 10-8:  Block Size, 000 for 1 OWord, 001 for 2 OWords, 010 for 4 OWords, 100 for 8 OWords.
/// Bits 7-0:   Binding Table Index: Set to 0xFF for stateless memory space used bu A64 SVM Data Port.
int IR_Builder::translateVISASVMBlockReadInst(
    Common_ISA_Oword_Num size,
    bool unaligned,
    G4_Operand* address,
    G4_DstRegRegion* dst)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    unsigned numOword = Get_Common_ISA_Oword_Num(size);
    G4_Declare* dcl = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ, Type_UD);
    if (no64bitType()) 
    {
        G4_SrcRegRegion *region = address->asSrcRegRegion();
        G4_SrcRegRegion *tmp;
        tmp = createSrcRegRegion(Mod_src_undef,
                                 region->getRegAccess(),
                                 region->getBase(),
                                 region->getRegOff(),
                                 region->getSubRegOff() * 2,
                                 region->getRegion(), Type_UD);
        Create_MOV_Inst(dcl, 0, 0, 1, NULL, NULL, tmp, true );
        tmp = createSrcRegRegion(Mod_src_undef,
                                 region->getRegAccess(),
                                 region->getBase(),
                                 region->getRegOff(),
                                 region->getSubRegOff() * 2 + 1,
                                 region->getRegion(), Type_UD);
        Create_MOV_Inst(dcl, 0, 1, 1, NULL, NULL, tmp, true );
    } 
    else 
    {
        G4_Declare* dclAsUQ = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ / 2, Type_UQ);
        dclAsUQ->setAliasDeclare(dcl, 0);
        Create_MOV_Inst(dclAsUQ, 0, 0, 1, NULL, NULL, address, true );
    }

    G4_SrcRegRegion* src = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());

    DATA_CACHE1_MESSAGES msgSubOpcode = DC1_A64_BLOCK_READ;
    unsigned rspLength = ((numOword - 1) / 2 + 1);

    unsigned desc = getA64BTI() |
        (unaligned ? A64_BLOCK_MSG_OWORD_UNALIGNED_READ : A64_BLOCK_MSG_OWORD_RW) << A64_BLOCK_MSG_SUBTYPE_OFFSET |
        msgSubOpcode << SEND_GT_MSG_TYPE_BIT;

    desc = setOwordForDesc(desc, numOword);

    uint8_t sendExecSize = FIX_OWORD_SEND_EXEC_SIZE(numOword);
    dst->setType(Type_UD);

    last_inst = Create_Send_Inst_For_CISA( NULL, dst, src, 1, rspLength, sendExecSize, desc,
        SFID_DP_DC1, false, true, true, false, NULL, NULL, InstOpt_WriteEnable, false );

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISASVMBlockWriteInst(
    Common_ISA_Oword_Num size,
    G4_Operand* address,
    G4_SrcRegRegion* src)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    unsigned numOword = Get_Common_ISA_Oword_Num(size);
    unsigned srcNumGRF = ((numOword - 1) / 2 + 1);
    uint8_t sendExecSize = FIX_OWORD_SEND_EXEC_SIZE(numOword);

    // FIXME: may want to apply this to FIX_OWORD_SEND_EXEC_SIZE instead
    if (sendExecSize < 8)
    {
        sendExecSize = 8;
    }

    G4_Declare* dcl = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ, Type_UD);
    if (no64bitType()) 
    {
        G4_SrcRegRegion *region = address->asSrcRegRegion();
        G4_SrcRegRegion *tmp;
        tmp = createSrcRegRegion(Mod_src_undef,
                                 region->getRegAccess(),
                                 region->getBase(),
                                 region->getRegOff(),
                                 region->getSubRegOff() * 2,
                                 region->getRegion(), Type_UD);
        Create_MOV_Inst(dcl, 0, 0, 1, NULL, NULL, tmp, true );
        tmp = createSrcRegRegion(Mod_src_undef,
                                 region->getRegAccess(),
                                 region->getBase(),
                                 region->getRegOff(),
                                 region->getSubRegOff() * 2 + 1,
                                 region->getRegion(), Type_UD);
        Create_MOV_Inst(dcl, 0, 1, 1, NULL, NULL, tmp, true );
    } else {
        G4_Declare* dclAsUQ = Create_MRF_Dcl(GENX_DATAPORT_IO_SZ / 2, Type_UQ);
        dclAsUQ->setAliasDeclare(dcl, 0);
        Create_MOV_Inst(dclAsUQ, 0, 0, 1, NULL, NULL, address, true );
    }

    bool useSplitSend = useSends();
    payloadSource sources[2];
    unsigned len = 0;

    sources[len].opnd = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
    sources[len].execSize = 8;
    sources[len].instOpt = InstOpt_WriteEnable;
    ++len;

    sources[len].opnd = src;

    switch (src->getElemSize()) {
        case 2: sources[len].execSize = 16 * srcNumGRF; break;
        case 4: sources[len].execSize =  8 * srcNumGRF; break;
        case 8: sources[len].execSize =  4 * srcNumGRF; break;
    }

    sources[len].instOpt = InstOpt_WriteEnable;
    ++len;

    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, sendExecSize, useSplitSend, sources, len);

    DATA_CACHE1_MESSAGES msgSubOpcode = DC1_A64_BLOCK_WRITE;

    unsigned desc = getA64BTI() |
        A64_BLOCK_MSG_OWORD_RW << A64_BLOCK_MSG_SUBTYPE_OFFSET |
        msgSubOpcode << SEND_GT_MSG_TYPE_BIT;

    desc = setOwordForDesc(desc, numOword);

    G4_DstRegRegion* sendDst = createNullDst(Type_UD);

    if (msgs[1] == 0) {
        last_inst = Create_Send_Inst_For_CISA(NULL, sendDst,
                                              msgs[0], sizes[0],
                                              0, sendExecSize,
                                              desc, SFID_DP_DC1,
                                              false, true,
                                              false, true,
                                              NULL, NULL,
                                              InstOpt_WriteEnable, false);
    } else {
        last_inst = Create_SplitSend_Inst_For_CISA(NULL, sendDst,
                                                   msgs[0], sizes[0],
                                                   msgs[1], sizes[1],
                                                   0, sendExecSize,
                                                   desc, 0, SFID_DP_DC1,
                                                   false, true,
                                                   false, true,
                                                   NULL, NULL,
                                                   InstOpt_WriteEnable, false);
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISASVMScatterReadInst(
    Common_ISA_Exec_Size execSize,
    Common_VISA_EMask_Ctrl eMask,
    G4_Predicate* pred,
    Common_ISA_SVM_Block_Type blockSize,
    Common_ISA_SVM_Block_Num numBlocks,
    G4_SrcRegRegion* addresses,
    G4_DstRegRegion* dst)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    ASSERT_USER(execSize == EXEC_SIZE_1 || execSize == EXEC_SIZE_2 ||
                execSize == EXEC_SIZE_4 || execSize == EXEC_SIZE_8 ||
                execSize == EXEC_SIZE_16,
                "Only support SIMD1, SIMD2, SIMD4, SIMD8 or SIMD16!");

    Common_ISA_Exec_Size instExecSize = execSize;
    if (execSize == EXEC_SIZE_1 || execSize == EXEC_SIZE_2 ||
        execSize == EXEC_SIZE_4) {
        execSize = EXEC_SIZE_8;
    }
    unsigned exSize = Get_Common_ISA_Exec_Size(execSize);
    unsigned instExSize = Get_Common_ISA_Exec_Size(instExecSize);
    unsigned int instOpt = Get_Gen4_Emask(eMask, instExSize);
    uint32_t messageLength = 2 * (exSize / 8);
    uint32_t responseLength = 0;

    // ToDo: remove this as it should be done in HWConformity
    if (instExSize < 8 && VISA_WA_CHECK(getPWaTable(), WaDisableSendSrcDstOverlap))
    {
        // as message length is set to 2 (HW requirements),
        // we have to even align both src/dst to satisfy the WA
        G4_Declare* srcDcl = addresses->getTopDcl()->getRootDeclare();
        if (srcDcl->getByteSize() <= GENX_GRF_REG_SIZ)
        {
            srcDcl->setAlign(Even);
        }
        G4_Declare* dstDcl = dst->getTopDcl()->getRootDeclare();
        if (dstDcl->getByteSize() <= GENX_GRF_REG_SIZ)
        {
            dstDcl->setAlign(Even);
        }
    }

    switch (blockSize)
    {
    case SVM_BLOCK_TYPE_BYTE:
        responseLength = (numBlocks == SVM_BLOCK_NUM_8) ? 2 : 1;
        break;
    case SVM_BLOCK_TYPE_DWORD:
        responseLength = Get_Common_ISA_SVM_Block_Num(numBlocks);
        break;
    case SVM_BLOCK_TYPE_QWORD:
        responseLength = Get_Common_ISA_SVM_Block_Num(numBlocks) * 2;
        break;
    default:
        MUST_BE_TRUE(false, "Illegal SVM block type");
    }
    responseLength *= (exSize / 8);

    unsigned desc = 0;
    desc |= getA64BTI();
    desc |= blockSize << 8;
    desc |= numBlocks << 10;
    desc |= (exSize == 8 ? 0 : 1) << 12;
    desc |= DC1_A64_SCATTERED_READ << 14;

    last_inst = Create_Send_Inst_For_CISA( pred, dst, addresses, messageLength, responseLength, instExSize, desc,
        SFID_DP_DC1, false, false, true, false, NULL, NULL, instOpt, false );

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISASVMScatterWriteInst(
    Common_ISA_Exec_Size execSize,
    Common_VISA_EMask_Ctrl eMask,
    G4_Predicate* pred,
    Common_ISA_SVM_Block_Type blockSize,
    Common_ISA_SVM_Block_Num numBlocks,
    G4_SrcRegRegion* addresses,
    G4_SrcRegRegion* src)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    ASSERT_USER(execSize == EXEC_SIZE_1 || execSize == EXEC_SIZE_2 ||
                execSize == EXEC_SIZE_4 || execSize == EXEC_SIZE_8 ||
                execSize == EXEC_SIZE_16,
                "Only support SIMD1, SIMD2, SIMD4, SIMD8 or SIMD16!");

    Common_ISA_Exec_Size instExecSize = execSize;
    if (execSize == EXEC_SIZE_1 || execSize == EXEC_SIZE_2 ||
        execSize == EXEC_SIZE_4) {
        execSize = EXEC_SIZE_8;
    }
    unsigned exSize = Get_Common_ISA_Exec_Size(execSize);
    unsigned instExSize = Get_Common_ISA_Exec_Size(instExecSize);
    unsigned int instOpt = Get_Gen4_Emask(eMask, instExSize);
    bool useSplitSend = useSends();

    payloadSource sources[2]; // Maximal 2 sources, optional header + offsets
    unsigned len = 0;

    sources[len].opnd = addresses;
    sources[len].execSize = exSize;
    sources[len].instOpt = instOpt;
    ++len;

    unsigned numElems = 1;
    // NOTE that BYTE scatter always has numElems set to 1 as
    // - when the number of data elements is 1, 2, or 4, the writeback payload
    //   is always 1 MDP_DW_SIMD8/_SIMD16.
    // - when the number of data elements is 8, the write payload is always 1
    //   MDP_QW_SIMD8/_SIMD16.
    // This ALSO implies the RAW operand should be in type of UQ when the
    // number of data elements is 8.
    if (blockSize != SVM_BLOCK_TYPE_BYTE)
        numElems = Get_Common_ISA_SVM_Block_Num(numBlocks);

    sources[len].opnd = src;
    sources[len].execSize = exSize * numElems;
    sources[len].instOpt = instOpt;
    ++len;

    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, exSize, useSplitSend, sources, len);

    unsigned desc = 0;
    desc |= getA64BTI();
    desc |= blockSize << 8;
    desc |= numBlocks << 10;
    desc |= (exSize == 8 ? 0 : 1) << 12;
    desc |= DC1_A64_SCATTERED_WRITE << 14;

    G4_DstRegRegion* dst = createNullDst(Type_UD);
    if (msgs[1] == 0) {
        ASSERT_USER(sizes[1] == 0, "Expect the 2nd part of the payload has zero size!");
        last_inst = Create_Send_Inst_For_CISA(pred, dst,
                                              msgs[0], sizes[0],
                                              0, instExSize,
                                              desc, SFID_DP_DC1,
                                              false, false,
                                              false, true,
                                              NULL, NULL,
                                              instOpt, false);
    } else {
        last_inst = Create_SplitSend_Inst_For_CISA(pred, dst,
                                                   msgs[0], sizes[0],
                                                   msgs[1], sizes[1],
                                                   0, instExSize,
                                                   desc, 0, SFID_DP_DC1,
                                                   false, false,
                                                   false, true,
                                                   NULL, NULL,
                                                   instOpt, false);
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

// is16Bit indicates if this is a 16bit atomic op. The input source (if
// any) and the writeback (if any) have the same datalayout as dword messages.
// Only the lower 16 bits of each dword is used.
//
static void FillSVMAtomicMsgDesc(bool is16Bit, bool isFloatOp, uint32_t &msgDesc)
{
    if (is16Bit)
    {
        if (isFloatOp)
        {
            msgDesc |= DC1_A64_UNTYPED_HALF_FLOAT_ATOMIC << 14;
        }
        else
        {
            msgDesc |= DC1_A64_UNTYPED_HALF_INTEGER_ATOMIC << 14;
        }
    }
    else
    {
        if (isFloatOp)
        {
            MUST_BE_TRUE(getGenxPlatform() >= GENX_SKL, "FP atomics are supported for SKL+");
            msgDesc |= DC1_A64_UNTYPED_FLOAT_ATOMIC << 14;
        }
        else
        {
            msgDesc |= DC1_A64_ATOMIC << 14;
        }
    }
}

int IR_Builder::translateVISASVMAtomicInst(
    CMAtomicOperations atomicOp,
    bool is16Bit,
    Common_ISA_Exec_Size execSize,
    Common_VISA_EMask_Ctrl emask,
    G4_Predicate* pred,
    G4_SrcRegRegion* addresses,
    G4_SrcRegRegion* src0,
    G4_SrcRegRegion* src1,
    G4_DstRegRegion* dst)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    Common_ISA_Exec_Size instExecSize = execSize;
    if (execSize == EXEC_SIZE_1 || execSize == EXEC_SIZE_2 || execSize == EXEC_SIZE_4)
    {
        execSize = EXEC_SIZE_8;
    }
    MUST_BE_TRUE(execSize == EXEC_SIZE_8, "execution size must be 8 for SVM atomic messages");

    bool isDWord = G4_Type_Table[dst->getType()].byteSize == 4;

    unsigned op = Get_Atomic_Op(atomicOp);

    unsigned exSize = Get_Common_ISA_Exec_Size(execSize);
    unsigned instExSize = Get_Common_ISA_Exec_Size(instExecSize);
    unsigned int instOpt = Get_Gen4_Emask(emask, instExSize);

    if (atomicOp == ATOMIC_CMPXCHG)
    {
        // we have to swap src0 and src1 since vISA has them in different order from HW 
        G4_SrcRegRegion* tmp = src0;
        src0 = src1;
        src1 = tmp;
    }

    bool useSplitSend = useSends();

    payloadSource sources[3]; // addresses, src0, and src1
    unsigned len = 0;

    sources[len].opnd = addresses;
    sources[len].execSize = exSize;
    sources[len].instOpt = instOpt;
    ++len;

    if (src0 != NULL && !src0->isNullReg())
    {
        sources[len].opnd = src0;
        sources[len].execSize = exSize;
        sources[len].instOpt = instOpt;
        ++len;
    }

    if (src1 != NULL && !src1->isNullReg())
    {
        sources[len].opnd = src1;
        sources[len].execSize = exSize;
        sources[len].instOpt = instOpt;
        ++len;
    }

    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, exSize, useSplitSend, sources, len);

    unsigned dstLength = dst->isNullReg() ? 0 : (isDWord ? 1 : 2);
    unsigned msgDesc = 0;
    msgDesc |= getA64BTI();
    msgDesc |= op << 8;
#define A64_ATOMIC_RETURN_DATA_CONTROL_BIT 13
    msgDesc |= (dstLength ? 1 : 0) << A64_ATOMIC_RETURN_DATA_CONTROL_BIT;
    msgDesc |= (isDWord ? 0 : 1) << 12;

    // Fill remaining bits.
    FillSVMAtomicMsgDesc(is16Bit, IsFloatAtomicOps(atomicOp), msgDesc);

    if (msgs[1] == 0) {
        last_inst = Create_Send_Inst_For_CISA(pred, dst,
                                              msgs[0], sizes[0], dstLength,
                                              instExSize,
                                              msgDesc, SFID_DP_DC1,
                                              false, false,
                                              true, true,
                                              NULL, NULL,
                                              instOpt, false);
    } else {
        last_inst = Create_SplitSend_Inst_For_CISA(pred, dst,
                                              msgs[0], sizes[0],
                                              msgs[1], sizes[1],
                                              dstLength,
                                              instExSize,
                                              msgDesc, 0, SFID_DP_DC1,
                                              false, false,
                                              true, true,
                                              NULL, NULL,
                                              instOpt, false);
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}
int IR_Builder::translateSVMGather4Inst(Common_ISA_Exec_Size    execSize,
                                        Common_VISA_EMask_Ctrl  eMask,
                                        ChannelMask             chMask,
                                        G4_Predicate            *pred,
                                        G4_Operand              *globalOffset,
                                        G4_SrcRegRegion         *offsets,
                                        G4_DstRegRegion         *dst)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    ASSERT_USER(execSize == EXEC_SIZE_8 || execSize == EXEC_SIZE_16,
                "Only support SIMD8 or SIMD16!");

    unsigned exSize = Get_Common_ISA_Exec_Size(execSize);
    unsigned instOpt = Get_Gen4_Emask(eMask, exSize);

    bool useSplitSend = useSends();

    // In case non-zero global offset is specified, we need to recalculate
    // offsets.
    if (!globalOffset->isImm() || globalOffset->asImm()->getImm() != 0) {
        G4_Declare *dcl = Create_MRF_Dcl(exSize, offsets->getType());
        G4_DstRegRegion *tmp = Create_Dst_Opnd_From_Dcl(dcl, 1);
        createInst(pred, G4_add, 0, false, exSize, tmp, offsets, globalOffset, instOpt);
        offsets = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
    }

    payloadSource sources[1]; // Maximal 1 sources, offsets
    unsigned len = 0;

    sources[len].opnd = offsets;
    sources[len].execSize = exSize;
    sources[len].instOpt = instOpt;
    ++len;

    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, exSize, useSplitSend, sources, len);

    CISA_SHARED_FUNCTION_ID SFID = SFID_DP_DC1;

    unsigned FC = 0;
    // Leave sidebind scaled offset 0 as it is not used now.
    FC |= DC1_A64_UNTYPED_SURFACE_READ << 14;
    FC |= (execSize == EXEC_SIZE_8 ? MDC_SM3_SIMD8 : MDC_SM3_SIMD16) << 12;
    FC |= chMask.getHWEncoding() << 8;
    FC |= getA64BTI();

    unsigned resLen = (exSize / GENX_DATAPORT_IO_SZ) *
                      chMask.getNumEnabledChannels();
    if (msgs[1] == 0) {
        ASSERT_USER(sizes[1] == 0, "Expect the 2nd part of the payload has zero size!");
        last_inst = Create_Send_Inst_For_CISA(pred, dst,
                                              msgs[0], sizes[0],
                                              resLen,
                                              exSize,
                                              FC, SFID,
                                              false, false,
                                              true, false,
                                              NULL, NULL,
                                              instOpt, false);
    } else {
        last_inst = Create_SplitSend_Inst_For_CISA(pred, dst,
                                                   msgs[0], sizes[0], msgs[1], sizes[1],
                                                   resLen,
                                                   exSize,
                                                   FC, 0, SFID,
                                                   false, false,
                                                   true, false,
                                                   NULL, NULL,
                                                   instOpt, false);
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateSVMScatter4Inst(Common_ISA_Exec_Size   execSize,
                                         Common_VISA_EMask_Ctrl eMask,
                                         ChannelMask            chMask,
                                         G4_Predicate           *pred,
                                         G4_Operand             *globalOffset,
                                         G4_SrcRegRegion        *offsets,
                                         G4_SrcRegRegion        *src)
{
#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    startTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif

    ASSERT_USER(execSize == EXEC_SIZE_8 || execSize == EXEC_SIZE_16,
                "Only support SIMD8 or SIMD16!");

    unsigned exSize = Get_Common_ISA_Exec_Size(execSize);
    unsigned instOpt = Get_Gen4_Emask(eMask, exSize);
    bool useSplitSend = useSends();

    // In case non-zero global offset is specified, we need to recalculate
    // offsets.
    if (!globalOffset->isImm() || globalOffset->asImm()->getImm() != 0) {
        G4_Declare *dcl = Create_MRF_Dcl(exSize, offsets->getType());
        G4_DstRegRegion *tmp = Create_Dst_Opnd_From_Dcl(dcl, 1);
        createInst(pred, G4_add, 0, false, exSize, tmp, offsets, globalOffset, instOpt);
        offsets = Create_Src_Opnd_From_Dcl(dcl, getRegionStride1());
    }

    payloadSource sources[2]; // Maximal 2 sources, offsets + src
    unsigned len = 0;

    sources[len].opnd = offsets;
    sources[len].execSize = exSize;
    sources[len].instOpt = instOpt;
    ++len;
    sources[len].opnd = src;
    sources[len].execSize = exSize * chMask.getNumEnabledChannels();
    sources[len].instOpt = instOpt;
    ++len;

    G4_SrcRegRegion *msgs[2] = {0, 0};
    unsigned sizes[2] = {0, 0};
    preparePayload(msgs, sizes, exSize, useSplitSend, sources, len);

    CISA_SHARED_FUNCTION_ID SFID = SFID_DP_DC1;

    unsigned FC = 0;
    // Leave sidebind scaled offset 0 as it is not used now.
    FC |= DC1_A64_UNTYPED_SURFACE_WRITE << 14;
    FC |= (execSize == EXEC_SIZE_8 ? MDC_SM3_SIMD8 : MDC_SM3_SIMD16) << 12;
    FC |= chMask.getHWEncoding() << 8;
    FC |= getA64BTI();

    G4_DstRegRegion *dst = createNullDst(Type_UD);
    if (msgs[1] == 0) {
        ASSERT_USER(sizes[1] == 0, "Expect the 2nd part of the payload has zero size!");
        last_inst = Create_Send_Inst_For_CISA(pred, dst,
                                              msgs[0], sizes[0],
                                              0,
                                              exSize,
                                              FC, SFID,
                                              false, false,
                                              false, true,
                                              NULL, NULL,
                                              instOpt, false);
    } else {
        last_inst = Create_SplitSend_Inst_For_CISA(pred, dst,
                                                   msgs[0], sizes[0], msgs[1], sizes[1],
                                                   0,
                                                   exSize,
                                                   FC, 0, SFID,
                                                   false, false,
                                                   false, true,
                                                   NULL, NULL,
                                                   instOpt, false);
    }

#if defined(MEASURE_COMPILATION_TIME) && defined(TIME_IR_CONSTRUCTION)
    stopTimer(TIMER_VISA_BUILDER_IR_CONSTRUCTION);
#endif
    return CM_SUCCESS;
}

int IR_Builder::translateVISASVMGather4ScaledInst(Common_ISA_Exec_Size      execSize,
                                                  Common_VISA_EMask_Ctrl    eMask,
                                                  ChannelMask               chMask,
                                                  G4_Predicate              *pred,
                                                  G4_Operand                *globalOffset,
                                                  G4_SrcRegRegion           *offsets,
                                                  G4_DstRegRegion           *dst)
{
    return translateSVMGather4Inst(execSize, eMask, chMask, pred,
        globalOffset, offsets, dst);
}

int IR_Builder::translateVISASVMScatter4ScaledInst(Common_ISA_Exec_Size     execSize,
                                                   Common_VISA_EMask_Ctrl   eMask,
                                                   ChannelMask              chMask,
                                                   G4_Predicate             *pred,
                                                   G4_Operand               *globalOffset,
                                                   G4_SrcRegRegion          *offsets,
                                                   G4_SrcRegRegion          *src)
{
    return translateSVMScatter4Inst(execSize, eMask, chMask, pred,
        globalOffset, offsets, src);
}

int IR_Builder::translateVISALifetimeInst(uint8_t properties, G4_Operand* var)
{
    // Lifetime.start/end are two variants of this instruction
    createImm(properties & 0x1, Type_UB);
    unsigned short varType = (properties >> 4) & 0x3;

    // varType encodings:
    // 0 - general
    // 1 - address
    // 2 - predicate

    Common_ISA_Var_Class varClass = GENERAL_VAR;
    if(varType == 1)
    {
        varClass = ADDRESS_VAR;
    }
    else if(varType == 2)
    {
        varClass = PREDICATE_VAR;
    }

    if((properties & 0x1) == LIFETIME_START)
    {
        G4_DstRegRegion* varDstRgn = createDstRegRegion(Direct, var->getBase(), 0, 0, 1, Type_UD);

        createInst(NULL, G4_pseudo_kill, NULL, false, 1, varDstRgn, NULL, NULL, 0, 0);
    }
    else
    {
        G4_SrcRegRegion* varSrcRgn = createSrcRegRegion(Mod_src_undef, Direct, var->getBase(), 0, 0, getRegionScalar(), Type_UD);

        createInst(NULL, G4_pseudo_lifetime_end, NULL, false, 1, NULL, varSrcRgn, NULL, 0, 0);
    }

    // We dont treat lifetime.end specially for now because lifetime.start
    // is expected to halt propagation of liveness upwards. lifetime.start
    // would prevent loop local variables/sub-rooutine local variables
    // from being live across entire loop/sub-routine.

    return CM_SUCCESS;
}

static G4_Declare *getDeclare(G4_SrcRegRegion *src) {
    G4_Declare *dcl = src->getBase()->asRegVar()->getDeclare();

    while (G4_Declare *parentDcl = dcl->getAliasDeclare())
        dcl = parentDcl;

    return dcl;
}

/// getSplitEMask() calculates the new mask after splitting from the current
/// execution mask at the given execution size.
/// It only works with masks covering whole GRF and thus won't generate/consume
/// nibbles.
static uint32_t getSplitEMask(unsigned execSize, uint32_t eMask, bool isLo) {
    const uint32_t qhMasks = InstOpt_M0 | InstOpt_M8 |
                             InstOpt_M16 | InstOpt_M24;
    uint32_t other = eMask & ~qhMasks;
    uint32_t qh = eMask & qhMasks;

    switch (execSize) {
    case 16: // Split SIMD16 into SIMD8
        switch (qh) {
        case 0: // instOpt not specified, treat as 1H
        case InstOpt_M0:
            return (isLo ? InstOpt_M0 : InstOpt_M8) | other;
        case InstOpt_M16:
            return (isLo ? InstOpt_M16 : InstOpt_M24) | other;
        }
        break;
    case 32: // Split SIMD32 into SIMD16.
        switch (qh) {
        case 0:
            return (isLo ? InstOpt_M0 : InstOpt_M16) | other;
        }
        break;
    }

    ASSERT_USER(false, "Unhandled cases for EMask splitting!");
    return ~0U;
}

static uint32_t getSplitLoEMask(unsigned execSize, uint32_t eMask) {
    return getSplitEMask(execSize, eMask, true);
}

static uint32_t getSplitHiEMask(unsigned execSize, uint32_t eMask) {
    return getSplitEMask(execSize, eMask, false);
}

/// CopySrcToMsgPayload() performs a single batch of copy source into message
/// payload. If that single batch needs copy more than 2 GRFs, it will be split
/// into 2 parts recursively. That implies the a single batch copy MUST have
/// the size of power-of-2 multiple GRFs.
static void CopySrcToMsgPayload(IR_Builder *IRB,
                                unsigned execSize, uint32_t eMask,
                                G4_Declare *msg, unsigned msgRegOff,
                                G4_SrcRegRegion *src, unsigned srcRegOff)
{
    uint32_t numRegs = (src->getElemSize() * execSize) /
            COMMON_ISA_GRF_REG_SIZE;
    if (numRegs == 0)
    {
        // always copy at least one GRF
        numRegs = 1;
    }

    ASSERT_USER((numRegs & (numRegs - 1)) == 0,
                "The batch size of a source message copy (i.e., native raw "
                "operand size) MUST be power-of-2 multiple of GRFs!");

    if (numRegs > 2) {
        // Copying of 2+ GRFs needs splitting. The splitting algorithm is
        // designed to be as general as possible to cover all possible valid
        // cases for message payload copying, i.e.,
        //
        // <32 x i32> -> 2 * <16 x i32>
        // <16 x i64> -> 2 * < 8 x i64>
        // <32 x i64> -> 2 * <16 x i64> -> 4 * < 8 x i64>
        //
        unsigned newExecSize = execSize >> 1;
        unsigned splitOff = numRegs >> 1;
        uint32_t loEMask = getSplitLoEMask(execSize, eMask);
        uint32_t hiEMask = getSplitHiEMask(execSize, eMask);
        // Copy Lo
        CopySrcToMsgPayload(IRB, newExecSize, loEMask,
                            msg, msgRegOff,
                            src, srcRegOff);
        // Copy Hi
        CopySrcToMsgPayload(IRB, newExecSize, hiEMask,
                            msg, msgRegOff + splitOff,
                            src, srcRegOff + splitOff);
        return;
    }

    G4_DstRegRegion *dstRegion
        = IRB->createDstRegRegion(Direct, msg->getRegVar(),
                                  (short)msgRegOff, 0, 1,
                                  src->getType());
    G4_SrcRegRegion *srcRegion
        = IRB->createSrcRegRegion(src->getModifier(),
                                  src->getRegAccess(),
                                  src->getBase(),
                                  src->getRegOff() + srcRegOff,
                                  src->getSubRegOff(),
                                  src->getRegion(),
                                  src->getType());
    IRB->createInst(NULL, G4_mov, NULL, false, execSize, dstRegion,
                    srcRegion, NULL, NULL, eMask, 0);
}

static void Copy_Source_To_Payload(IR_Builder *IRB, unsigned batchExSize,
                                   G4_Declare *msg, unsigned &regOff,
                                   G4_SrcRegRegion *source, unsigned execSize,
                                   uint32_t eMask) {
    ASSERT_USER(batchExSize == 1 || batchExSize == 2 || batchExSize == 4 ||
				batchExSize == 8 || batchExSize == 16 || batchExSize == 32,
                "Invalid execution size for message payload copy!");


    unsigned srcRegOff = 0;
    unsigned batchSize = std::min(batchExSize, execSize);
    uint32_t numSrcRegs = (source->getElemSize() * batchSize) /
            COMMON_ISA_GRF_REG_SIZE;
    if (numSrcRegs == 0)
    {
        // always copy at least one GRF
        numSrcRegs = 1;
    }

    for (unsigned i = 0; i < execSize; i += batchSize) {
        if (!source->isNullReg()) {
            CopySrcToMsgPayload(IRB, batchSize, eMask,
                                msg, regOff, source, srcRegOff);
        }
        regOff += numSrcRegs;
        srcRegOff += numSrcRegs;
    }
}

void IR_Builder::preparePayload(G4_SrcRegRegion *msgs[2],
                                unsigned sizes[2],
                                unsigned batchExSize,
                                bool splitSendEnabled,
                                payloadSource srcs[], unsigned len) {
    G4_Declare *dcls[2] = {0, 0};
    unsigned msgSizes[2] = {0, 0};
    unsigned current = 0;
    unsigned offset = 0;
    unsigned splitPos = 0;

    // Loop through all source regions to check whether they forms one
    // consecutive regions or one/two consecutive regions if splitIndex is
    // non-zero.
    unsigned i;
    for (i = 0; i != len; ++i) {
        G4_SrcRegRegion *srcReg = srcs[i].opnd;

        if (srcReg->isNullReg()) {
            break;
        }

        G4_Declare *srcDcl = getDeclare(srcReg);
        ASSERT_USER(srcDcl, "Declaration is missing!");

        unsigned regionSize = srcs[i].execSize *
                              G4_Type_Table[srcReg->getType()].byteSize;

        if (regionSize < COMMON_ISA_GRF_REG_SIZE) {
            // FIXME: Need a better solution to decouple the value type from
            // the container type to generate better COPY if required.
            // round up to 1 GRF
            regionSize = COMMON_ISA_GRF_REG_SIZE;
        }

        if (srcDcl == dcls[current]) {
            unsigned srcOff = getByteOffsetSrcRegion(srcReg);
            // Check offset if they have the same declaration.
            if (offset == srcOff) {
                // Advance offset to next expected one.
                offset += regionSize;
                msgSizes[current] += regionSize;
                continue;
            }
            // Check whether there are overlaps if split-send is enabled.
            if (splitSendEnabled && current == 0 && srcOff < offset) {
                // The source overlaps with the previous sources prepared.
                // Force to copy all sources from the this source for the 2nd
                // part in the split message.
                ++current;

                ASSERT_USER(i > 0, "Split position MUST NOT be at index 0!");
                splitPos = i;
                break;
            }
        }

        if (dcls[current] == 0) {
            // First time checking the current region.
            offset = getByteOffsetSrcRegion(srcReg);
            offset += regionSize;
            msgSizes[current] += regionSize;
            dcls[current] = srcDcl;
            continue;
        }

        // Bail out if more than 1 consecutive regions are needed but
        // split-send is not enabled.
        if (!splitSendEnabled)
            break;

        // Bail out if more than 2 consecutive regions will be needed.
        if (current != 0)
            break;

        // Check one more consecutive regions.
        ++current;

        ASSERT_USER(i > 0, "Split position MUST NOT be at index 0!");

        // Record the 2nd consecutive region.
        splitPos = i;
        offset = getByteOffsetSrcRegion(srcReg);
        offset += regionSize;
        msgSizes[current] += regionSize;
        dcls[current] = srcDcl;
    }

    if (i == len) {
        // All sources are checked and they are fit into one or two consecutive
        // regions.
        msgs[0] = srcs[0].opnd;
        msgs[1] = (splitPos == 0) ? 0 : srcs[splitPos].opnd;
        sizes[0] = msgSizes[0] / GENX_MRF_REG_SIZ;
        sizes[1] = msgSizes[1] / GENX_MRF_REG_SIZ;

        return;
    }

    // Count remaining message size.
    for (; i != len; ++i) {
        G4_SrcRegRegion *srcReg = srcs[i].opnd;
        unsigned regionSize = srcs[i].execSize *
                              G4_Type_Table[srcReg->getType()].byteSize;
        if (regionSize < COMMON_ISA_GRF_REG_SIZE) {
            // FIXME: Need a better solution to decouple the value type from
            // the container type to generate better COPY if required.
            // round up to 1 GRF
            regionSize = COMMON_ISA_GRF_REG_SIZE;
        }
        msgSizes[current] += regionSize;
    }

    // Allocate a new large enough GPR to copy in the payload.
    G4_Declare *msg =
        Create_MRF_Dcl(msgSizes[current]/G4_Type_Table[Type_UD].byteSize, Type_UD);

    // Copy sources.
    unsigned regOff = 0;
    for (i = splitPos; i != len; ++i)
    {
        Copy_Source_To_Payload(this, batchExSize, msg, regOff, srcs[i].opnd,
                               srcs[i].execSize, srcs[i].instOpt);
    }

    i = 0;
    if (current > 0) {
        msgs[i] = srcs[0].opnd;
        sizes[i] = msgSizes[0] / GENX_MRF_REG_SIZ;
        ++i;
    }
    msgs[i] = Create_Src_Opnd_From_Dcl(msg, getRegionStride1());
    sizes[i] = msgSizes[current] / GENX_MRF_REG_SIZ;
}

G4_Operand* IR_Builder::duplicateOpndImpl( G4_Operand* opnd )
{
    if( !opnd || opnd->isImm() )
        return opnd;
    if( opnd->isSrcRegRegion() )
    {
        return createSrcRegRegion( *(opnd->asSrcRegRegion()) );
    }else if( opnd->isDstRegRegion() )
    {
        return createDstRegRegion( *(opnd->asDstRegRegion()) );
    }else if( opnd->isPredicate() )
    {
        return createPredicate( *(opnd->asPredicate()) );
    }else if( opnd->isCondMod() )
    {
        return createCondMod( *(opnd->asCondMod()) );
    }else
    {
        return opnd;
    }
}

/***
*
*  this does two things:
*  -- If send has exec size 16, its destination must have Type W.
*  -- avoid using Q/UQ type on CHV/BXT
*/
void IR_Builder::fixSendDstType(G4_DstRegRegion* dst, uint8_t execSize)
{
    MUST_BE_TRUE( dst->getRegAccess() == Direct, "Send dst must be a direct operand");

	MUST_BE_TRUE(dst->getSubRegOff() == 0, "dst may not have a non-zero subreg offset");

	// normally we should create a new alias for dst's declare, but since it's a send
	// type mismatch between operand and decl should not matter
	if (execSize == 16 && dst->getType() != Type_W && dst->getType() != Type_UW)
	{
		dst->setType(Type_W);
	}

    if (dst->getType() == Type_HF)
    {
        dst->setType(Type_W);
    }
}


void IR_Builder::predefinedVarRegAssignment(uint8_t inputSize)
{
    uint32_t preDefinedStart = ((inputSize + G4_DSIZE - 1) / G4_DSIZE) * G4_DSIZE;
    if (preDefinedStart == 0)
    {
        preDefinedStart = GENX_GRF_REG_SIZ;
    }
    for (PreDefinedVarsInternal i : allPreDefVars)
    {
        if (!predefinedVarNeedGRF(i))
        {
            continue;
        }

        G4_Type ty = Get_G4_Type_From_Common_ISA_Type(getPredefinedVarType(i));
        G4_Declare *dcl = preDefVars.getPreDefinedVar((PreDefinedVarsInternal)i);
        if (!isPredefinedVarInR0((PreDefinedVarsInternal)i))
        {
            unsigned short new_offset = preDefinedStart + getPredefinedVarByteOffset(i);
            unsigned int regNum = new_offset / GENX_GRF_REG_SIZ;
            unsigned int subRegNum = (new_offset % GENX_GRF_REG_SIZ) / G4_Type_Table[ty].byteSize;
            dcl->getRegVar()->setPhyReg(phyregpool.getGreg(regNum), subRegNum);
        }
        else
        {
            unsigned int regNum = 0;
            unsigned int subRegNum = getPredefinedVarByteOffset(i) / G4_Type_Table[ty].byteSize;
            dcl->getRegVar()->setPhyReg(phyregpool.getGreg(regNum), subRegNum);
        }
    }
}

// Expand some of the pre-defined variables at kernel entry
// -- replace pre-defined V17 (hw_tid)
// -- replace pre-defined V22 (color)
// -- replace pre-defined V1 (thread_x)
// -- replace pre-defined V2 (thread_y)
void IR_Builder::expandPredefinedVars()
{

	// Use FFTID from msg header
	// and (1) hw_tid, r0.5, 0x3ff
	//
	// 9:0     FFTID. This ID is assigned by TS and is a unique identifier for the thread in
	// comparison to other concurrent root threads. It is used to free up resources used
	// by the thread upon thread completion.
	//
	// [Pre-DevBDW]: Format = U8. Bits 9:8 are Reserved, MBZ.
	//
	// For BDW format is U9
	// For SKL format is U10
	//

    // first non-label instruction
    auto iter = std::find_if(instList.begin(), instList.end(), [](G4_INST* inst) { return !inst->isLabel(); });

    if (preDefVars.isHasPredefined(PreDefinedVarsInternal::HW_TID))
	{
		G4_SrcRegRegion* src = createSrcRegRegion(Mod_src_undef, Direct, realR0->getRegVar(),
			0, 5, getRegionScalar(), Type_UD);
		G4_Imm* mask1 = this->createImm(0x3ff, Type_UD);
		G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(builtinHWTID, 1);
        G4_INST* inst = this->createInternalInst(NULL, G4_and, NULL, false, 1, dst, src, mask1,
            InstOpt_WriteEnable, 0, UNMAPPABLE_VISA_INDEX, NULL);
		instList.insert(iter, inst);
	}

    if (preDefVars.isHasPredefined(PreDefinedVarsInternal::X))
    {
        if (useNewR0Format())
        {
            // x -> and (1) thread_x<1>:uw r0.1:ud 0xFFF
            G4_SrcRegRegion* r0Dot1UD = createSrcRegRegion(Mod_src_undef, Direct,
                realR0->getRegVar(), 0, 1, getRegionScalar(), Type_UD);
            G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(preDefVars.getPreDefinedVar(PreDefinedVarsInternal::X), 1);
            G4_INST* inst = createInternalInst(nullptr, G4_and, nullptr, false, 1, dst, r0Dot1UD,
                createImm(0xFFF, Type_UW), InstOpt_WriteEnable);
            instList.insert(iter, inst);
        }
        else
        {
            //  We insert the new instruction
            //  and (1) thread_x<1>:uw, r0.2:uw, 0x01FF
            G4_SrcRegRegion* r0Dot2UW = createSrcRegRegion(Mod_src_undef, Direct,
                realR0->getRegVar(), 0, 2, getRegionScalar(), Type_UW);
            int64_t mask = getThreadIDMask();
            G4_Imm* src1 = createImm(mask, Type_UW);
            G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(preDefVars.getPreDefinedVar(PreDefinedVarsInternal::X), 1);
            G4_INST* inst = createInternalInst(nullptr, G4_and, nullptr, false, 1, dst, r0Dot2UW, src1, InstOpt_WriteEnable);
            instList.insert(iter, inst);
        }
    }

    if (preDefVars.isHasPredefined(PreDefinedVarsInternal::Y))
    {
        if (useNewR0Format())
        {
            // y -> shr (1) thread_y<1>:uw r0.1:ud 12
            //      and (1) thread_y<1>:uw thread_y:uw 0xFFF
            G4_SrcRegRegion* r0Dot1UD = createSrcRegRegion(Mod_src_undef, Direct,
                realR0->getRegVar(), 0, 1, getRegionScalar(), Type_UD);

            G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(preDefVars.getPreDefinedVar(PreDefinedVarsInternal::Y), 1);
            G4_INST* inst1 = createInternalInst(nullptr, G4_shr, nullptr, false, 1, dst, r0Dot1UD,
                createImm(12, Type_UW), InstOpt_WriteEnable);
            instList.insert(iter, inst1);
            dst = Create_Dst_Opnd_From_Dcl(preDefVars.getPreDefinedVar(PreDefinedVarsInternal::Y), 1);
            G4_INST* inst2 = createInternalInst(nullptr, G4_and, nullptr, false, 1, dst,
                Create_Src_Opnd_From_Dcl(preDefVars.getPreDefinedVar(PreDefinedVarsInternal::Y), getRegionScalar()),
                createImm(0xFFF, Type_UW), InstOpt_WriteEnable);
            instList.insert(iter, inst2);
        }
        else
        {
            //  We insert the new instruction
            //  and (1) thread_y<1>:uw, r0.3:uw, 0x01FF
            G4_SrcRegRegion* r0Dot3UW = createSrcRegRegion(Mod_src_undef, Direct,
                realR0->getRegVar(), 0, 3, getRegionScalar(), Type_UW);
            int64_t mask = getThreadIDMask();
            G4_Imm* src1 = createImmWithLowerType(mask, Type_UW);
            G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(preDefVars.getPreDefinedVar(PreDefinedVarsInternal::Y), 1);
            G4_INST* inst = createInternalInst(nullptr, G4_and, nullptr, false, 1, dst, r0Dot3UW, src1, InstOpt_WriteEnable);
            instList.insert(iter, inst);
        }
    }

    // color bit
    if (preDefVars.isHasPredefined(PreDefinedVarsInternal::COLOR))
    {
        if (useNewR0Format())
        {
            // r0.1[31:24]
            // shr (1) color<2>:uw r0.1<0;1,0>:ud 24
            G4_SrcRegRegion* src = createSrcRegRegion(Mod_src_undef, Direct, realR0->getRegVar(),
                0, 1, getRegionScalar(), Type_UD);
            G4_Imm* shift = createImm(24, Type_UW);
            G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(preDefVars.getPreDefinedVar(PreDefinedVarsInternal::COLOR), 2);
            G4_INST* inst = createInternalInst(nullptr, G4_shr, nullptr, false, 1, dst, src, shift,
                InstOpt_WriteEnable, 0, UNMAPPABLE_VISA_INDEX, nullptr);
            instList.insert(iter, inst);
        }
        else
        {
            // else: r0.2[3:0]
            // and (1) color<2>:uw r0.2<0;1,0>:ud 0xF
            G4_SrcRegRegion* src = createSrcRegRegion(Mod_src_undef, Direct, realR0->getRegVar(),
                0, 2, getRegionScalar(), Type_UD);
            G4_Imm* mask = createImm(0xF, Type_UW);
            G4_DstRegRegion* dst = Create_Dst_Opnd_From_Dcl(preDefVars.getPreDefinedVar(PreDefinedVarsInternal::COLOR), 2);
            G4_INST* inst = createInternalInst(nullptr, G4_and, nullptr, false, 1, dst, src, mask,
                InstOpt_WriteEnable, 0, UNMAPPABLE_VISA_INDEX, nullptr);
            instList.insert(iter, inst);
        }
    }
}

void IR_Builder::Copy_SrcRegRegion_To_Payload( G4_Declare* payload, unsigned int& regOff, G4_SrcRegRegion* src, unsigned int exec_size, uint32_t emask )
{
    G4_DstRegRegion payloadDst( Direct, payload->getRegVar(), (short)regOff, 0, 1, payload->getElemType() );
    G4_DstRegRegion* payloadDstRgn = createDstRegRegion( payloadDst );

    G4_SrcRegRegion* srcRgn = createSrcRegRegion( *src );
    srcRgn->setType( payload->getElemType() );
    G4_INST* refCopy = createInst( NULL, G4_mov, NULL, false, exec_size, payloadDstRgn, srcRgn, NULL, NULL, 0, 0 );
    refCopy->setOptionOn(emask);
    if (G4_Type_Table[payload->getElemType()].byteSize == 2)
    {
        // for half float each source occupies 1 GRF regardless of execution size
        regOff++;
    }
    else
    {
        regOff += (exec_size/8);
    }
}

unsigned int IR_Builder::getByteOffsetSrcRegion( G4_SrcRegRegion* srcRegion )
{
    unsigned int offset = ( srcRegion->getRegOff() * G4_GRF_REG_NBYTES ) + ( srcRegion->getSubRegOff() * G4_Type_Table[srcRegion->getType()].byteSize );

    if( srcRegion->getBase() &&
        srcRegion->getBase()->isRegVar() )
    {
        G4_Declare* dcl = srcRegion->getBase()->asRegVar()->getDeclare();

        if( dcl != NULL )
        {
            while( dcl->getAliasDeclare() != NULL )
            {
                offset += dcl->getAliasOffset();
                dcl = dcl->getAliasDeclare();
            }
        }
    }

    return offset;
}

bool IR_Builder::checkIfRegionsAreConsecutive( G4_SrcRegRegion* first, G4_SrcRegRegion* second, unsigned int exec_size )
{
    bool isConsecutive = false;

    if( first == NULL || second == NULL )
    {
        isConsecutive = true;
    }
    else
    {
        G4_Declare* firstDcl = getDeclare(first);
        G4_Declare* secondDcl = getDeclare(second);

        unsigned int firstOff = getByteOffsetSrcRegion( first );
        unsigned int secondOff = getByteOffsetSrcRegion( second );

        if( firstDcl == secondDcl )
        {
            if( ( firstOff + ( exec_size * G4_Type_Table[first->getType()].byteSize ) ) == secondOff )
            {
                isConsecutive = true;
            }
        }
    }

    return isConsecutive;
}

bool IR_Builder::checkIfRegionsAreConsecutive( G4_SrcRegRegion* first, G4_SrcRegRegion* second, unsigned int exec_size, G4_Type type )
{
    bool isConsecutive = false;

    if( first == NULL || second == NULL )
    {
        isConsecutive = true;
    }
    else
    {
        G4_Declare* firstDcl = getDeclare(first);
        G4_Declare* secondDcl = getDeclare(second);

        unsigned int firstOff = getByteOffsetSrcRegion( first );
        unsigned int secondOff = getByteOffsetSrcRegion( second );

        if( firstDcl == secondDcl )
        {
            if( ( firstOff + ( exec_size * G4_Type_Table[type].byteSize ) ) == secondOff )
            {
                isConsecutive = true;
            }
        }
    }

    return isConsecutive;
}

