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

#include "LVN.h"

using namespace std;
using namespace vISA;

//#define DEBUG_LVN_ON

#define DUMMY_HSTRIDE_2_2_0     0x8000
#define DUMMY_HSTRIDE_4_4_0     0x4000
#define DUMMY_HSTRIDE_8_8_0     0xc000
#define DUMMY_HSTRIDE_16_16_0   0x2000

bool isSpecialRegion(RegionDesc* desc, uint16_t& hstride)
{
    bool isSpecial = false;

    // Look for regions like <N;N,0> in general,
    // except for <1;1,0> since it is contiguous.
    // DS uses N=4 case.
    if (desc->vertStride == desc->width &&
        desc->vertStride != 1 &&
        desc->horzStride == 0)
    {
        isSpecial = true;

        // Update hstride
        switch (desc->vertStride)
        {
        case 2:
            hstride = DUMMY_HSTRIDE_2_2_0;
            break;
        case 4:
            hstride = DUMMY_HSTRIDE_4_4_0;
            break;
        case 8:
            hstride = DUMMY_HSTRIDE_8_8_0;
            break;
        case 16:
            hstride = DUMMY_HSTRIDE_16_16_0;
        default:
            MUST_BE_TRUE(false, "Unexpected special hstride seen");
        }
    }

    return isSpecial;
}

// This function looks at region description and execution size
// and using API of RegionDesc class, decides what normalized
// hstride is. Overlapping regions or non-uniform regions, ie
// disconnected regions are not expected in LVN.
uint16_t getActualHStride(G4_SrcRegRegion* srcRgn)
{
    RegionDesc* desc = srcRgn->getRegion();
    uint32_t execSize = srcRgn->getInst()->getExecSize();
    uint16_t stride = desc->horzStride;
    bool isSpecialRgn = isSpecialRegion(desc, stride);

    if (!isSpecialRgn)
    {
        desc->isSingleStride(execSize, stride);
    }

    return stride;
}

bool LVN::getDstData(int64_t srcImm, G4_Type srcType, int64_t& dstImm, G4_Type dstType, bool& canNegate)
{
    // mov (...) V..:dstType       0x...:srcType
    // Given srcImm data, compute what dst register will contain
    bool dstImmValid = false;
    int64_t andMask = 0;
    canNegate = (srcImm == 0 ? false : true);
    andMask = (0xffffffffffffffff >> ((sizeof(int64_t)* 8) - G4_Type_Table[dstType].bitSize));

    if (srcType == Type_W || srcType == Type_UW ||
        srcType == Type_D || srcType == Type_UD ||
        srcType == Type_Q || srcType == Type_UQ)
    {
        dstImm = srcImm;
        if (IS_TYPE_INT(dstType))
        {
            dstImm &= andMask;
            dstImmValid = true;
        }
        else if (srcImm == 0)
        {
            // Make special case for following type
            // conversion:
            // mov (..) V1:f    0:w/uw/d/ud
            dstImm &= andMask;
            dstImmValid = true;
        }
        else if (dstType == Type_F)
        {
            union
            {
                int32_t i;
                float f;
            } ftod;
            int32_t intSrc = (int32_t)srcImm;
            ftod.f = (float)intSrc;
            dstImm = (int64_t)ftod.i;
            dstImm &= andMask;

            // Now ensure the int is within representable
            // precision range of float. So reverse
            // convert and compare with original data.
            int32_t reverseInt;
            reverseInt = (int32_t)ftod.f;
            if (reverseInt == intSrc)
            {
                dstImmValid = true;
            }
        }
    }
    else if (srcType == Type_HF || srcType == Type_F ||
        srcType == Type_DF)
    {
        dstImm = srcImm;
        if (dstType == srcType ||
            srcImm == 0)
        {
            dstImm &= andMask;
            dstImmValid = true;
        }
    }
    else if (IS_VINTTYPE(srcType))
    {
        dstImm = srcImm;
        dstImmValid = true;
        canNegate = false;
    }

    return dstImmValid;
}

bool LVN::getAllUses(G4_INST* def, UseList& uses)
{
    bool defFound = false;
    for (auto use_it = def->use_begin();
        use_it != def->use_end();
        use_it++)
    {
        auto&& curUse = (*use_it);
        UseInfo useData;
        useData.first = curUse.first;
        useData.second = curUse.second;
        uses.push_back(useData);
        defFound = true;
    }

    return defFound;
}

bool LVN::canReplaceUses(INST_LIST_ITER inst_it, UseList& uses, G4_INST* lvnInst, bool negMatch, bool noPartialUse)
{
    // Look up all uses of current instruction.
    // Then check if each use is fully defined by current
    // instruction. Return true if this is true for all
    // uses.
    G4_INST* defInst = (*inst_it);
    G4_DstRegRegion* def = defInst->getDst();
    G4_DstRegRegion* lvnDst = lvnInst->getDst();
    G4_Declare* lvnDstTopDcl = lvnDst->getTopDcl();

#ifdef DEBUG_LVN_ON
    std::cerr << "Inst with same value in LVN Table:" << std::endl;
    lvnInst->emit(std::cerr);
    std::cerr << " #" << lvnInst->getLineNo() << ":$" << lvnInst->getCISAOff();
    std::cerr << std::endl;
    std::cerr << "Current inst:" << std::endl;
    defInst->emit(std::cerr);
    std::cerr << " #" << defInst->getLineNo() << ":$" << defInst->getCISAOff();
    std::cerr << std::endl << "Uses:" << std::endl;

    char shouldReplace[2];
#endif

    // Check if def defines each use fully
    unsigned int lb = def->getLeftBound();
    unsigned int rb = def->getRightBound();
    unsigned int hs = def->getHorzStride();

    bool canReplace = true;
    for (auto use_it = uses.begin(); use_it != uses.end(); use_it++)
    {
        G4_INST* useInst = (*use_it).first;
        Gen4_Operand_Number opndNum = (*use_it).second;
        G4_Operand* use = useInst->getOperand(opndNum);
        unsigned int use_lb = use->getLeftBound();
        unsigned int use_rb = use->getRightBound();

        // Ensure a single def flows in to the use
        if (useInst->getSingleDef(opndNum) == NULL)
        {
            canReplace = false;
            break;
        }

        if (bb->isInSimdFlow())
        {
            auto defCoversUseEmask = defInst->getMaskOffset() <= useInst->getMaskOffset() &&
                (defInst->getMaskOffset() + defInst->getExecSize() >= useInst->getMaskOffset() + useInst->getExecSize());
            if (!defInst->isWriteEnableInst() &&
                (!defCoversUseEmask || useInst->isWriteEnableInst()))
            {
                // if defInst does not fully cover useInst, it's generally unsafe to do LVN
                canReplace = false;
                break;
            }
        }

        // Compute a single positive stride if exists.
        unsigned int use_hs = 0;
        {
            uint16_t stride = 0;
            RegionDesc *rd = use->asSrcRegRegion()->getRegion();
            if (rd->isSingleStride(useInst->getExecSize(), stride))
            {
                use_hs = stride;
            }
        }

        if (noPartialUse)
        {
            if (lb != use_lb ||
                rb != use_rb ||
                (hs != use_hs && defInst->getExecSize() > 1))
            {
                canReplace = false;
                break;
            }
        }
        else
        {
            // ok as long as def covers entire use
            if (lb > use_lb || rb < use_rb)
            {
                canReplace = false;
                break;
            }
        }

        if (useInst->isSend())
        {
            // send operand doesn't take subreg, so the operand has to be GRF-aligned
            if (!builder.isOpndAligned(lvnDst, GENX_GRF_REG_SIZ))
            {
                canReplace = false;
                break;
            }
        }

        if (useInst->isSplitSend())
        {
            // Ensure both src opnds of split are not the same
            if ((opndNum == Opnd_src0 &&
                useInst->getSrc(1)->getTopDcl() == lvnDstTopDcl) ||
                (opndNum == Opnd_src1 &&
                useInst->getSrc(0)->getTopDcl() == lvnDstTopDcl))
            {
                canReplace = false;
                break;
            }
        }

        if (negMatch)
        {
            if (useInst->isSend())
            {
                // send src opnd doesnt support negate modifier.
                // So if LVN found a pattern that requires the
                // modifier then this optimization is invalid.
                canReplace = false;
                break;
            }

            if (useInst->isRawMov() &&
                G4_Type_Table[useInst->getDst()->getType()].byteSize == 1)
            {
                // For byte-type raw movs, src modifier '-' is invalid
                canReplace = false;
                break;
            }

            if (IS_UNSIGNED_INT(use->getType()))
            {
                // Bug fix for following sequence:
                // a:w = 1:w
                // b:uw = 0xffff:uw
                // c:d = x:d + b:uw
                //
                // It is incorrect to replace b:uw above with -a:uw.
                canReplace = false;
                break;
            }
            if (use->getType() != lvnInst->getDst()->getType())
            {
                //
                // negate's meaning can change based on type
                // 
                canReplace = false;
                break;
            }
        }

        // Iterate to ensure there is no WAR/WAW
        auto fwdInst_it = inst_it;
        fwdInst_it++;
        bool lvnTopDclAddresses = lvnDstTopDcl->getAddressed();
        bool lvnTopDclGRFAssigned = (lvnDstTopDcl->getRegVar()->getPhyReg() != NULL);
        while ((*fwdInst_it) != useInst)
        {
            G4_DstRegRegion* curDst = (*fwdInst_it)->getDst();

            if (curDst)
            {
                if (lvnTopDclAddresses &&
                    curDst->isIndirect())
                {
                    canReplace = false;
                    break;
                }

                auto dstRgn = curDst;
                auto topdcl = dstRgn->getTopDcl();

                unsigned int lvnlb = lvnDst->getLeftBound();
                unsigned int lvnrb = lvnDst->getRightBound();

                unsigned int curlb = curDst->getLeftBound();
                unsigned int currb = curDst->getRightBound();

                if (topdcl == lvnDstTopDcl &&
                    ((lvnlb <= curlb && lvnrb >= curlb) ||
                    (curlb <= lvnlb && currb >= lvnlb)))
                {
                    canReplace = false;
                    break;
                }

                if (lvnTopDclGRFAssigned &&
                    curDst->getTopDcl()->getRegVar()->getPhyReg() != NULL)
                {
                    canReplace = sameGRFRef(lvnDstTopDcl, curDst->getTopDcl());
                    if (!canReplace)
                    {
                        break;
                    }
                }
            }

            fwdInst_it++;
        }
    }

    if (canReplace)
    {
        // Ensure lvnInst's dst type and that of current inst is the same
        G4_Type lvnDstType = lvnDst->getType();
        G4_Type defType = def->getType();
        // For non-integers, they must have the same type to be replaced
        if (lvnDstType != defType &&
            (!IS_TYPE_INT(lvnDstType) || !IS_TYPE_INT(defType)))
        {
            canReplace = false;
        }
        // For integers, they must have the same type width to be replaced
        if ((IS_TYPE_INT(lvnDstType) && IS_TYPE_INT(defType)) &&
            G4_Type_Table[lvnDstType].byteSize != G4_Type_Table[defType].byteSize)
        {
            canReplace = false;
        }

        if (canReplace &&
            lvnDst->getHorzStride() !=
            def->getHorzStride())
        {
            // Catch the case:
            // mov (8) V0<1>:d    V3
            // mov (8) V1<2>:d    V3
            // add (8) V2<1>:q    V2    V1<16;8,2>
            //
            // => Dont replace V1<16;8,2> with V0<8;8,1> because it would make
            // code HW non-conformant.
            canReplace = false;
        }
    }

    if (canReplace &&
        lvnInst->getExecSize() < defInst->getExecSize())
    {
        // Allow following case:
        // mov (8) V1(0,0) x -- x is an immdiate
        // op (8) ... V1(0,0)
        // mov (1) V2(0,0) x => remove
        // op (1) ... V2(0,0) => op (1) ... V1(0,0)
        //
        // But disallow following case:
        // mov (1) V1(0,0) x <-- lvnInst
        // op (1) ... V1(0,0)
        // mov (8) V2(0,0) x <-- defInst
        // op (8) ... V2(0,0) => Cant replace V2 with V1
        //

        canReplace = false;
    }

    if (canReplace &&
        defInst->getExecSize() > 1)
    {
        // Check whether alignment matches for vectors
        // mov (8) V2(0,6)    V1(0,1) ... <-- lvnInst
        // ...
        // mov (8) V3(0,0)    V2(0,6)
        // op (8) V4          V3(0,0)   ...
        //
        // In general, V3(0,0) src in op cannot be replaced by
        // V2(0,6).
        if (lb %GENX_GRF_REG_SIZ != lvnDst->getLeftBound() % GENX_GRF_REG_SIZ)
        {
            canReplace = false;
        }
    }

#ifdef DEBUG_LVN_ON
    if (canReplace)
    {
        for (auto use_it = uses.begin();
            use_it != uses.end();
            use_it++)
        {
            (*use_it).first->emit(std::cerr);
            std::cerr << std::endl;
        }

        std::cerr << std::endl << std::endl << "Replace this occurence?";
        scanf("%s", &shouldReplace);

        canReplace = (shouldReplace[0] == 'y' ? true : false);
        if (canReplace)
            std::cerr << "Ok, will replace" << std::endl;
    }
#endif

    return canReplace;
}

// transfer alignment of fromDcl to toDcl if the former is more restrictive
void LVN::transferAlign(G4_Declare* toDcl, G4_Declare* fromDcl)
{
    if (toDcl->getAlign() == Either)
    {
        toDcl->setAlign(fromDcl->getAlign());
    }
    else if (toDcl->getAlign() != fromDcl->getAlign() && fromDcl->getAlign() != Either)
    {
        // toDcl and fromDcl are not compatible
        // ToDo: we should move this code as part of LVN legality check instead of assert
        assert(false && "incompatible alignment");
    }

    G4_SubReg_Align align1 = toDcl->getSubRegAlign();
    G4_SubReg_Align align2 = fromDcl->getSubRegAlign();
    G4_SubReg_Align ret = align1;

    // Compute most constrained sub-reg alignment and assign that
    // to lvnDst dcl since it will replace curDst based operands.
    if (align1 == align2)
    {
        return;
    }

    switch (align1)
    {
    case Any:
        ret = align2;
        break;
    case Even_Word:
        if (align2 != Any)
        {
            ret = align2;
        }
        break;
    case Four_Word:
        if (align2 == Eight_Word || align2 == Sixteen_Word)
        {
            ret = align2;
        }
        break;
    case Eight_Word:
        if (align2 == Sixteen_Word)
        {
            ret = align2;
        }
        break;
    case Sixteen_Word:
        break;
    default:
        MUST_BE_TRUE(false, "Unimplemented sub-reg align condition hit");
        break;
    }

    toDcl->setSubRegAlign(ret);
}

void LVN::replaceAllUses(G4_INST* defInst, bool negate, UseList& uses, G4_INST* lvnInst, bool keepRegion)
{
    G4_Declare* dstTopDcl = lvnInst->getDst()->getTopDcl();
    const unsigned int regOff = lvnInst->getDst()->getRegOff();
    const unsigned int subRegOff = lvnInst->getDst()->getSubRegOff();

    // Ensure most constrained alignment gets applied to lvnInst's dst
    transferAlign(dstTopDcl, defInst->getDst()->getTopDcl());

#ifdef DEBUG_VERBOSE_ON
    std::cerr << "Inst with same value in LVN Table:" << std::endl;
    lvnInst->emit(std::cerr);
    std::cerr << " #" << lvnInst->getLineNo() << ":$" << lvnInst->getCISAOff();
    std::cerr << std::endl;
    std::cerr << "Current inst:" << std::endl;
    defInst->emit(std::cerr);
    std::cerr << " #" << defInst->getLineNo() << ":$" << defInst->getCISAOff();
    std::cerr << std::endl << "Uses:" << std::endl;
#endif

    for (auto use : uses)
    {
        G4_INST* useInst = use.first;
        G4_SrcRegRegion* srcToReplace = useInst->getOperand(use.second)->asSrcRegRegion();
        G4_SrcModifier srcMod = srcToReplace->getModifier();
        if (negate == true)
        {
            // Negate will be true only when value is based off an immediate operand
            // so there is no need to check for combination of src modifiers.
            if (srcMod == Mod_src_undef)
            {
            	srcMod = Mod_Minus;
            }
            else if (srcMod == Mod_Minus)
            {
                srcMod = Mod_src_undef;
            }
            else
            {
                MUST_BE_TRUE(false, "Unexpected src modifier found in LVN");
            }
        }

        G4_SrcRegRegion* srcRgn = nullptr;
        if (keepRegion)
        {
            // new offset should include the offset between the use and its original def
            assert(srcToReplace->getLeftBound() >= defInst->getDst()->getLeftBound() && "orig dst does not fully define use");
            int offsetFromOrigDst = srcToReplace->getLeftBound() - defInst->getDst()->getLeftBound();
            // we can replace the regVar directly without changing the rest of the region
            auto typeSize = getTypeSize(srcToReplace->getType());
            int offset = regOff * GENX_GRF_REG_SIZ + subRegOff * getTypeSize(lvnInst->getDst()->getType()) + offsetFromOrigDst;
            short newRegOff = offset / GENX_GRF_REG_SIZ;
            short newSubRegOff = (offset % GENX_GRF_REG_SIZ) / typeSize;
           
            srcRgn = builder.createSrcRegRegion(srcMod, Direct, lvnInst->getDst()->getBase()->asRegVar(),
                newRegOff, newSubRegOff, srcToReplace->getRegion(), srcToReplace->getType());
        }
        else
        {
            unsigned short vstride = srcToReplace->getRegion()->vertStride;
            unsigned short width = srcToReplace->getRegion()->width;
            unsigned short hstride = getActualHStride(srcToReplace);
            G4_Type type = srcToReplace->getType();

            unsigned int subRegOffScaled = subRegOff * G4_Type_Table[lvnInst->getDst()->getType()].byteSize / G4_Type_Table[type].byteSize;

            srcRgn = builder.createSrcRegRegion(srcMod, Direct,
                lvnInst->getDst()->getBase()->asRegVar(),
                (short)regOff,
                (short)subRegOffScaled,
                builder.createRegionDesc(vstride, width, hstride),
                type);
        }

        unsigned int srcIndex = G4_INST::getSrcNum(use.second);
        useInst->setSrc(srcRgn, srcIndex);
    }
}

void LVN::removeVirtualVarRedefs(G4_DstRegRegion* dst)
{
    auto dstTopDcl = dst->getTopDcl();
    if (!dstTopDcl)
        return;

    auto dclId = dstTopDcl->getDeclId();
    auto it = lvnTable.find(dclId);

    if (it != lvnTable.end())
    {
        for (auto second = it->second.begin();
            second != it->second.end();
            )
        {
            auto potentialRedef = (*second);
#define IS_VAR_REDEFINED(origopnd, opnd) \
    (((origopnd->getLeftBound() <= opnd->getLeftBound() && origopnd->getRightBound() >= opnd->getLeftBound()) || \
    (opnd->getLeftBound() <= origopnd->getLeftBound() && opnd->getRightBound() >= origopnd->getLeftBound())))

            if (potentialRedef->dstTopDcl == dstTopDcl &&
                IS_VAR_REDEFINED(dst, potentialRedef->inst->getDst()))
            {
                potentialRedef->active = false;
            }
            else
            {
                for (unsigned int i = 0; i < G4_MAX_SRCS; i++)
                {
                    if (potentialRedef->srcTopDcls[i] == dstTopDcl &&
                        IS_VAR_REDEFINED(dst, potentialRedef->inst->getSrc(i)))
                    {
                        potentialRedef->active = false;
                    }
                }
            }

            if (!potentialRedef->active)
            {
                second = it->second.erase(second);
                continue;
            }

            second++;
        }
    }

    if (dst->getTopDcl()->getAddressed())
    {
        // Iterate over entire LVN table and remove any instruction with
        // src indirect operand that point to current dst. For eg,
        // A0 = &V10
        // V20 = r[A0] <-- inst1
        // ...
        // V10 = 0 <-- Current instruction - Invalidate inst1
        // V30 = r[A0] <-- inst1 != this inst
        for (auto dcls : lvnTable)
        {
            for (auto second = dcls.second.begin();
                second != dcls.second.end();
                )
            {
                auto lvnItems = (*second);

                for (unsigned int i = 0; i < G4_MAX_SRCS; i++)
                {
                    if (lvnItems->srcTopDcls[i] &&
                        lvnItems->inst->getSrc(i)->isSrcRegRegion() &&
                        lvnItems->inst->getSrc(i)->asSrcRegRegion()->isIndirect())
                    {
                        if (p2a.isPresentInPointsTo(lvnItems->srcTopDcls[i]->getRegVar(), dst->getTopDcl()->getRegVar()))
                        {
                            lvnItems->active = false;
                            second = dcls.second.erase(second);
                            continue;
                        }
                    }
                }
                if (second != dcls.second.end())
                {
                    // Increment iterator only if dcls.second is not empty.
                    // Erase operation in earlier loop can result in this.
                    second++;
                }
            }
        }
    }
}

bool LVN::sameGRFRef(G4_Declare* dcl1, G4_Declare* dcl2)
{
    bool overlap = false;

    unsigned int dst1LowGRF = dcl1->getRegVar()->getPhyReg()->asGreg()->getRegNum();
    unsigned int dst1HighGRF = dst1LowGRF + (dcl1->getNumRows());

    unsigned int dst2LowGRF = dcl2->getRegVar()->getPhyReg()->asGreg()->getRegNum();
    unsigned int dst2HighGRF = dst2LowGRF + (dcl2->getNumRows());

    if (dst1LowGRF <= dst2HighGRF &&
        dst1HighGRF >= dst2LowGRF)
    {
        overlap = true;
    }

    return overlap;
}

// Function assumes that dst's G4_RegVar has pre-defined
// physical register. We remove all those entries from
// LVN table that refer to same GRF.
void LVN::removePhysicalVarRedefs(G4_DstRegRegion* dst)
{
    G4_Declare* topdcl = dst->getTopDcl();
    for (auto all : lvnTable)
    {
        for (auto it = all.second.begin();
            it != all.second.end();
            )
        {
            auto item = (*it);
            bool erase = false;

            if (item->dstTopDcl->getRegVar()->isGreg())
            {
                if (sameGRFRef(topdcl, item->dstTopDcl))
                {
                    item->active = false;
                    erase = true;
                }
            }

            for (unsigned int i = 0; i < G4_MAX_SRCS; i++)
            {
                if (item->srcTopDcls[i] &&
                    item->srcTopDcls[i]->getRegVar()->isGreg())
                {
                    // Check if both physical registers have an overlap
                    if (sameGRFRef(topdcl, item->srcTopDcls[i]))
                    {
                        item->active = false;
                        erase = true;
                    }
                }
            }

            if (erase)
            {
                it = all.second.erase(it);
                continue;
            }

            it++;
        }
    }
}

bool LVN::checkIfInPointsTo(G4_RegVar* addr, G4_RegVar* var)
{
    // Check whether var is present in points2 of addr
    auto ptrToAllPointsTo = p2a.getAllInPointsTo(addr);
    if (ptrToAllPointsTo)
    {
        auto& allInPointsTo = *ptrToAllPointsTo;
        G4_RegVar* topRegVar = var->getDeclare()->getRootDeclare()->getRegVar();

        for (auto pointedTo : allInPointsTo)
        {
            if (pointedTo == topRegVar)
            {
                return true;
            }
        }
    }

    return false;
}

void LVN::removeAliases(G4_INST* inst)
{
    // inst uses indirect dst operand
    MUST_BE_TRUE(inst->getDst()->isIndirect(), "Expecting to see indirect operand in dst");

    auto dstPointsToPtr = p2a.getAllInPointsTo(inst->getDst()->getTopDcl()->getRegVar());
    if (!dstPointsToPtr)
        return;

    auto& dstPointsTo = *dstPointsToPtr;

    for (auto item : dstPointsTo)
    {
        auto dclId = item->getDeclare()->getRootDeclare()->getDeclId();

        auto it = lvnTable.find(dclId);

        if (it != lvnTable.end())
        {
            for (auto conflict : it->second)
            {
                conflict->active = false;
#ifdef DEBUG_VERBOSE_ON
                DEBUG_VERBOSE("Removing inst from LVN table for indirect dst conflict:");
                conflict->inst->emit(std::cerr);
                DEBUG_VERBOSE(" #" << conflict->inst->getLineNo() << ":$" << conflict->inst->getCISAOff() << std::endl);
                DEBUG_VERBOSE("Due to indirect dst in:" << std::endl);
                inst->emit(std::cerr);
                DEBUG_VERBOSE(" #" << inst->getLineNo() << ":$" << inst->getCISAOff() << std::endl << std::endl);
#endif
            }
            it->second.clear();
        }
    }
}

void LVN::removeRedefs(G4_INST* inst)
{
    G4_DstRegRegion* dst = inst->getDst();
    if (dst)
    {
        if (dst->isIndirect())
        {
            removeAliases(inst);
        }

        G4_DstRegRegion* dstRegRegion = dst;
        removeVirtualVarRedefs(dstRegRegion);

        G4_Declare* dstTopDcl = dstRegRegion->getTopDcl();
        if (dstTopDcl &&
            dstTopDcl->getRegVar()->isGreg())
        {
            removePhysicalVarRedefs(dstRegRegion);
        }
    }
}

int64_t LVN::getNegativeRepresentation(int64_t imm, G4_Type type)
{
    union
    {
        double ddata;
        float fdata;
        char bdata;
        short wdata;
        int dwdata;
        int64_t lldata;
    } d;
    d.lldata = imm;

    if (type == Type_B || type == Type_UB)
    {
        d.bdata = -d.bdata;
    }
    else if (type == Type_W || type == Type_UW)
    {
        d.wdata = -d.wdata;
    }
    else if (type == Type_D || type == Type_UD)
    {
        d.dwdata = -d.dwdata;
    }
    else if (type == Type_Q || type == Type_UQ)
    {
        d.lldata = -d.lldata;
    }
    else if (type == Type_F)
    {
        d.fdata = -d.fdata;
    }
    else if (type == Type_DF)
    {
        d.ddata = -d.ddata;
    }
    else if (type == Type_HF)
    {
        d.wdata = d.wdata ^ (1 << 15);
    }

    return d.lldata;
}

const char* LVN::getModifierStr(G4_SrcModifier srcMod)
{
    if (srcMod == Mod_src_undef)
    {
        return "";
    }
    else if (srcMod == Mod_Minus)
    {
        return "NEG";
    }
    else if (srcMod == Mod_Abs)
    {
        return "ABS";
    }
    else if (srcMod == Mod_Minus_Abs)
    {
        return "NEGABS";
    }
    else if (srcMod == Mod_Not)
    {
        return "NOT";
    }
    else
    {
        return "";
    }
}

void LVN::getValue(int64_t imm, G4_Operand* opnd, Value& value)
{
    value.hash = imm;
    value.opnd = opnd;
}

void LVN::getValue(G4_SrcRegRegion* src, G4_INST* inst, Value& value)
{
    G4_Declare* topdcl = src->getTopDcl();
    value.hash = topdcl->getDeclId() + src->getLeftBound() + src->getRightBound() + getActualHStride(src);
    value.opnd = src;
}

void LVN::getValue(G4_DstRegRegion* dst, G4_INST* inst, Value& value)
{
    G4_Declare* topdcl = dst->getTopDcl();
    value.hash = topdcl->getDeclId() + dst->getLeftBound() + dst->getRightBound() + dst->getHorzStride();
    value.opnd = dst;
}

template<class T, class K>
bool LVN::opndsMatch(T* opnd1, K* opnd2)
{
    bool match = true;

    // T,K can either be G4_SrcRegRegion/G4_DstRegRegion
    G4_Declare* topdcl1 = opnd1->getTopDcl();
    G4_Declare* topdcl2 = opnd2->getTopDcl();

    G4_INST* inst1 = opnd1->getInst();
    G4_INST* inst2 = opnd2->getInst();
    // Compare emask for opnd1, opnd2 instructions
    if (bb->isInSimdFlow())
    {
        if (inst1->isWriteEnableInst() != inst2->isWriteEnableInst())
        {
            match = false;
        }
        else
        {
            if (inst1->getMaskOffset() != inst2->getMaskOffset() &&
                !inst1->isWriteEnableInst())
            {
                match = false;
            }
        }
    }
    else
    {
        // Not in SIMD CF so dont care
    }

    if (match)
    {
        const char* mod1 = getModifierStr(Mod_src_undef);
        const char* mod2 = mod1;
        if (opnd1->isSrcRegRegion())
        {
            mod1 = getModifierStr(opnd1->asSrcRegRegion()->getModifier());
        }

        if (opnd2->isSrcRegRegion())
        {
            mod2 = getModifierStr(opnd2->asSrcRegRegion()->getModifier());
        }

        if (mod1 != mod2)
        {
            match = false;
        }
    }

    if (match)
    {
        if (opnd1->isIndirect() != opnd2->isIndirect())
        {
            match = false;
        }
        else
        {
            // In this branch, both operands are either direct or indirect
            if (opnd1->isIndirect())
            {
                if (opnd1->isSrcRegRegion() &&
                    opnd2->isSrcRegRegion())
                {
                    G4_SrcRegRegion* src1 = opnd1->asSrcRegRegion();
                    G4_SrcRegRegion* src2 = opnd2->asSrcRegRegion();

                    if (topdcl1 != topdcl2 ||
                        src1->getRegOff() != src2->getRegOff() ||
                        src1->getSubRegOff() != src2->getSubRegOff() ||
                        src1->getAddrImm() != src2->getAddrImm() ||
                        getActualHStride(src1) != getActualHStride(src2) ||
                        src1->getInst()->getExecSize() != src2->getInst()->getExecSize() ||
                        src1->getType() != src2->getType())
                    {
                        match = false;
                    }
                }
                else
                {
                    // opnd1 is a src region and opnd2 is dst region or
                    // vice-versa.
                    match = false;
                }
            }
            else
            {
                // opnd1/opnd2 can be either src/dst regions and may not of the same type.
                if (topdcl1 != topdcl2 ||
                    G4_Type_Table[opnd1->getType()].str != G4_Type_Table[opnd2->getType()].str)
                {
                    match = false;
                }
                else
                {
                    int op1lb = 0, op2lb = 0, op1rb = 0, op2rb = 0, op1hs = 0, op2hs = 0;
                    if (opnd1->isSrcRegRegion())
                    {
                        G4_SrcRegRegion* src1 = opnd1->asSrcRegRegion();
                        op1lb = src1->getLeftBound();
                        op1rb = src1->getRightBound();
                        op1hs = getActualHStride(src1);
                    }
                    else if (opnd1->isDstRegRegion())
                    {
                        G4_DstRegRegion* dst1 = opnd1->asDstRegRegion();
                        op1lb = dst1->getLeftBound();
                        op1rb = dst1->getRightBound();
                        op1hs = dst1->getHorzStride();
                    }

                    if (opnd2->isSrcRegRegion())
                    {
                        G4_SrcRegRegion* src2 = opnd2->asSrcRegRegion();
                        op2lb = src2->getLeftBound();
                        op2rb = src2->getRightBound();
                        op2hs = getActualHStride(src2);
                    }
                    else if (opnd2->isDstRegRegion())
                    {
                        G4_DstRegRegion* dst2 = opnd2->asDstRegRegion();
                        op2lb = dst2->getLeftBound();
                        op2rb = dst2->getRightBound();
                        op2hs = dst2->getHorzStride();
                    }

                    if (op1lb != op2lb ||
                        op1rb != op2rb ||
                        op1hs != op2hs)
                    {
                        match = false;
                    }
                }
            }
        }
    }

    return match;
}

// Given a G4_Operand, this function returns false if,
// operand is a src region that is either scalar <0;1,0> or
// has contiguous region like <8;8,1>. Otherwise, if
// src region is uniform like <8;4,2> then function returns
// false. Only in case of non-uniform region the function
// returns true. Non-uniform regions are regions that
// have overlapping elements or hstride between elements
// in not uniform.
//
// Examples of non-uniform region are
// (4) <1;2,2>, (8) <2;4,1>, (8) <8;4,1>.
// The last one <8;4,1> is actually used in FRC.
bool isNonUniformSrcRegion(G4_SrcRegRegion* srcRgn)
{
    auto execSize = srcRgn->getInst()->getExecSize();
    return !srcRgn->getRegion()->isSingleStride(execSize);
}

bool LVN::addValue(G4_INST* inst)
{
    if (inst->opcode() != G4_mov ||
        inst->getSaturate() ||
        inst->getDst()->isIndirect() ||
        inst->getPredicate() != NULL ||
        inst->getCondMod() != NULL)
    {
        return false;
    }

    G4_Operand* dst = inst->getDst();
    if (!dst->getBase() ||
        !dst->getBase()->isRegVar() ||
        dst->getBase()->asRegVar()->getDeclare()->getRegFile() != G4_GRF)
    {
        return false;
    }

    if (dst->getBase() &&
        dst->getTopDcl()->isOutput())
    {
        return false;
    }

    G4_Operand* src = inst->getSrc(0);
    if (src->isImm())
    {
        if (src->isRelocImm())
        {
            return false;
        }
        return true;
    }

    uint16_t stride = 0;
    if (src->isSrcRegRegion())
    {
        G4_SrcRegRegion* srcRgn = src->asSrcRegRegion();
        if (!srcRgn->getBase() ||
            !srcRgn->getBase()->isRegVar() ||
            (!srcRgn->getBase()->asRegVar()->getDeclare()->useGRF() && !srcRgn->isIndirect()) ||
            (isNonUniformSrcRegion(srcRgn) &&
            !isSpecialRegion(srcRgn->getRegion(), stride)))
        {
            return false;
        }

        if (srcRgn->getBase() &&
            srcRgn->getTopDcl()->isOutput())
        {
            return false;
        }
        return true;
    }

    // false for addrExp
    return false;

}

void LVN::computeValue(G4_INST* inst, bool negate, bool& canNegate, bool& isGlobal, int64_t& tmpPosImm, bool posValValid, Value& value)
{
    canNegate = false;
    isGlobal = false;
    value.initializeEmptyValue();

    G4_Operand* src = inst->getSrc(0);

    if (inst->opcode() == G4_mov)
    {
        if (src->isImm())
        {
            int64_t dstImm = 0;
            G4_Type srcType = src->asImm()->getType();
            G4_Type dstType = inst->getDst()->getType();

            if (negate && posValValid)
            {
                dstImm = getNegativeRepresentation(tmpPosImm, inst->getDst()->getType());
            }
            else
            {
                G4_Imm* srcImmOpnd = src->asImm();
                int64_t srcImmData = srcImmOpnd->getImm();
                bool success = getDstData(srcImmData, srcType, dstImm, dstType, canNegate);
                tmpPosImm = dstImm;
                if (success == false)
                {
                    // This means mov was unsupported for optimization,
                    // eg, a type-conversion mov.
                    return;
                }

                if (negate == true)
                {
                    dstImm = getNegativeRepresentation(dstImm, dstType);
                }
            }

            getValue(dstImm, src, value);
        }
        else
        {
            MUST_BE_TRUE(src->isSrcRegRegion(), "expect srcRegRegion");
            if (negate == false)
            {
                G4_SrcRegRegion* src0 = inst->getSrc(0)->asSrcRegRegion();
                bool valid = true;
                if (src0->isIndirect())
                {
                    if (!p2a.getAllInPointsTo(src0->getBase()->asRegVar()))
                    {
                        valid = false;
                    }
                }
                // Can also set canNegate here but not sure if we really want to
                // spend that much compile time per instruction then.
                if (valid)
                {
                    getValue(src->asSrcRegRegion(), inst, value);
                }
            }
        }
    }
    else
    {
        // Other patterns to detect using LVN can be added here
    }

    // Compute value for globals so we can insert it in LVN table.
    // But we dont want to apply optimization on such instructions.
    isGlobal = fg.globalOpndHT.isOpndGlobal(inst->getDst());
    isGlobal |= inst->getDst()->getTopDcl()->isOutput();
}

// Ordering of val1 and val2 parameters is important.
// val1 is current instruction in top-down order.
// val2 is earlier instruction having operand with same value.
// Ordering is important because we allow optimization when
// val2 instruction uses NoMask and val1 instruction uses
// a subset.
bool LVN::valuesMatch(Value& val1, Value& val2)
{
    G4_Operand* opnd1 = val1.opnd;
    G4_Operand* opnd2 = val2.opnd;
    bool match = false;

    if (opnd1->isImm() &&
        opnd2->isImm())
    {
        match = true;
        G4_Type type1 = opnd1->getType();
        G4_Type type2 = opnd2->getType();

        if ((type1 == Type_UV && type2 != Type_UV) || (type1 != Type_UV && type2 == Type_UV))
        {
            match = false;
        }
        else if ((type1 == Type_V && type2 != Type_V) || (type1 != Type_V && type2 == Type_V))
        {
            match = false;
        }

        if (match && bb->isInSimdFlow())
        {
            G4_INST* val1Inst = val1.getInst();
            G4_INST* val2Inst = val2.getInst();

            if (!val2Inst->isWriteEnableInst() &&
                val1Inst->getMaskOption() != val2Inst->getMaskOption())
            {
                match = false;
            }
        }
    }
    else if (opnd1->isSrcRegRegion())
    {
        if (opnd2->isSrcRegRegion())
        {
            match = opndsMatch(opnd1->asSrcRegRegion(), opnd2->asSrcRegRegion());
        }
        else if (opnd2->isDstRegRegion())
        {
            match = opndsMatch(opnd1->asSrcRegRegion(), opnd2->asDstRegRegion());
        }
    }
    else if (opnd1->isDstRegRegion())
    {
        if (opnd2->isDstRegRegion())
        {
            match = opndsMatch(opnd1->asDstRegRegion(), opnd2->asDstRegRegion());
        }
        else if (opnd2->isSrcRegRegion())
        {
            match = opndsMatch(opnd1->asDstRegRegion(), opnd2->asSrcRegRegion());
        }
    }

    return match;
}

bool LVN::isSameValue(Value& val1, Value& val2)
{
    if (val1.isEqualValueHash(val2))
    {
        // Do a detailed check whether values really match
        if (valuesMatch(val1, val2))
        {
            return true;
        }
    }

    return false;
}

LVNItemInfo* LVN::isValueInTable(Value& value)
{
    int64_t hash = 0;
    if (value.opnd->isImm())
    {
        hash = value.hash;
    }
    else if (value.opnd->isSrcRegRegion())
    {
        hash = value.opnd->asSrcRegRegion()->getTopDcl()->getDeclId();
    }
    else if (value.opnd->isDstRegRegion())
    {
        hash = value.opnd->asDstRegRegion()->getTopDcl()->getDeclId();
    }
    else
    {
        for (auto&& table : lvnTable)
        {
            for (auto it = table.second.begin();
                it != table.second.end();
                )
            {
                auto lvnItem = (*it);

                if (!lvnItem->active)
                {
                    it = table.second.erase(it);
                    continue;
                }

                if (isSameValue(value, lvnItem->value) ||
                    isSameValue(value, lvnItem->variable))
                {
                    return lvnItem;
                }

                it++;
            }
        }

        return nullptr;
    }

    auto bucket = lvnTable.find(hash);
    if (bucket != lvnTable.end())
    {
        for (auto it = bucket->second.rbegin();
            it != bucket->second.rend();
            )
        {
            auto lvnItem = (*it);

            if (lvnItem->active &&
                (isSameValue(value, lvnItem->value) ||
                isSameValue(value, lvnItem->variable)))
            {
                return lvnItem;
            }

            it++;
        }
    }

    return nullptr;
}

void LVN::addValueToTable(G4_INST* inst, Value& oldValue)
{
    Value varValue;
    varValue.initializeEmptyValue();
    LVNItemInfo* item = (LVNItemInfo*)mem.alloc(sizeof(LVNItemInfo));
    getValue(inst->getDst(), inst, varValue);
    item->inst = inst;
    item->value.copyValue(oldValue);
    item->variable.copyValue(varValue);
    item->dstTopDcl = inst->getDst()->getTopDcl();
    item->active = true;
    for (unsigned int i = 0; i < G4_MAX_SRCS; i++)
    {
        G4_Operand* src = inst->getSrc(i);

        item->srcTopDcls[i] = nullptr;
        if (src != NULL)
        {
            if (src->isSrcRegRegion())
            {
                item->srcTopDcls[i] = src->getTopDcl();
                auto it = lvnTable.find(src->getTopDcl()->getDeclId());
                if (it != lvnTable.end())
                {
                    it->second.push_back(item);
                }
                else
                {
                    std::list<vISA::LVNItemInfo*> second;
                    second.push_back(item);
                    lvnTable.insert(make_pair(src->getTopDcl()->getDeclId(), second));
                }

                if (src->asSrcRegRegion()->isIndirect())
                {
                    auto points2set = p2a.getAllInPointsTo(src->getTopDcl()->getRegVar());
                    if (points2set)
                    {
                        for (auto rvar : *points2set)
                        {
                            auto dcl = rvar->getDeclare()->getRootDeclare();
                            auto p2aDclId = dcl->getDeclId();

                            auto it = lvnTable.find(p2aDclId);
                            if (it != lvnTable.end())
                            {
                                it->second.push_back(item);
                            }
                            else
                            {
                                std::list<vISA::LVNItemInfo*> second;
                                second.push_back(item);
                                lvnTable.insert(make_pair(p2aDclId, second));
                            }
                        }
                    }
                }
            }
            else if (src->isImm())
            {
                auto imm = oldValue.hash;
                auto it = lvnTable.find(imm);
                if (it != lvnTable.end())
                {
                    it->second.push_back(item);
                }
                else
                {
                    std::list<vISA::LVNItemInfo*> second;
                    second.push_back(item);
                    lvnTable.insert(make_pair(imm, second));
                }
            }
        }
    }

    auto dstDclId = item->dstTopDcl->getDeclId();
    auto it = lvnTable.find(dstDclId);
    if (it != lvnTable.end())
    {
        it->second.push_back(item);
    }
    else
    {
        std::list<vISA::LVNItemInfo*> second;
        second.push_back(item);
        lvnTable.insert(make_pair(dstDclId, second));
    }

    auto dst = inst->getDst();
    if (dst->isIndirect())
    {
        auto points2set = p2a.getAllInPointsTo(dst->getTopDcl()->getRegVar());
        if (points2set)
        {
            for (auto rvar : *points2set)
            {
                auto dcl = rvar->getDeclare()->getRootDeclare();
                auto p2aDclId = dcl->getDeclId();

                auto it = lvnTable.find(p2aDclId);
                if (it != lvnTable.end())
                {
                    it->second.push_back(item);
                }
                else
                {
                    std::list<vISA::LVNItemInfo*> second;
                    second.push_back(item);
                    lvnTable.insert(make_pair(p2aDclId, second));
                }
            }
        }
    }
}

void LVN::addUse(G4_DstRegRegion* dst, G4_INST* use, unsigned int srcIndex)
{
    Gen4_Operand_Number srcPos = Opnd_dst;
    if (srcIndex == 0)
    {
        srcPos = Opnd_src0;
    }
    else if (srcIndex == 1)
    {
        srcPos = Opnd_src1;
    }
    else if (srcIndex == 2)
    {
        srcPos = Opnd_src2;
    }

    dst->getInst()->addDefUse(use, srcPos);
}

void LVN::removeAddrTaken(G4_AddrExp* opnd)
{
    G4_Declare* opndTopDcl = opnd->getRegVar()->getDeclare()->getRootDeclare();

    auto range_it = activeDefs.equal_range(opndTopDcl->getDeclId());
    for (auto it = range_it.first;
        it != range_it.second;
        )
    {
        auto prev_it = it;
        (*prev_it).second.second->getInst()->removeAllUses();
        it++;
        activeDefs.erase(prev_it);
    }
}

void LVN::populateDuTable(INST_LIST_ITER inst_it)
{
    duTablePopulated = true;
    // Populate duTable from inst_it position
    ActiveDefMMap activeDefs;
    G4_INST* startInst = (*inst_it);
    G4_Operand* startInstDst = startInst->getDst();
    INST_LIST_ITER lastInstIt = bb->instList.end();
    unsigned int numEdgesAdded = 0;

    // Clear du/ud chains of all instructions in BB
    for (auto it = inst_it;
        it != lastInstIt;
        it++)
    {
        G4_INST* inst = (*it);

        inst->clearDef();
        inst->clearUse();
    }

    while (inst_it != lastInstIt)
    {
        G4_INST* curInst = (*inst_it);
        // First scan src operands and check if their def has been
        // added to activeDefs table already. If so, link them.
        for (unsigned int i = 0; i < G4_MAX_SRCS; i++)
        {
            G4_Operand* opnd = curInst->getSrc(i);
            if (opnd == NULL)
            {
                continue;
            }

            if (opnd->isAddrExp())
            {
                // Since this operand is address taken,
                // remove any defs corresponding to it
                // in DU and activeDefs tables.
                removeAddrTaken(opnd->asAddrExp());
            }

            if (opnd->isSrcRegRegion())
            {
                G4_Declare* topdcl = opnd->getTopDcl();
                if (topdcl != NULL)
                {
                    auto range_it = activeDefs.equal_range(topdcl->getDeclId());
                    if (range_it.first == range_it.second)
                    {
                        // No match found so move on to next src opnd
                        continue;
                    }

                    unsigned int lb = opnd->getLeftBound();
                    unsigned int rb = opnd->getRightBound();
                    unsigned int hs = getActualHStride(opnd->asSrcRegRegion());

                    auto start_it = range_it.second;
                    start_it--;
                    bool done = false;
                    for (auto it = start_it;
                        ;
                        )
                    {
                        G4_DstRegRegion* activeDst = (*it).second.second;

                        unsigned int lb_dst = activeDst->getLeftBound();
                        unsigned int rb_dst = activeDst->getRightBound();
                        unsigned int hs_dst = activeDst->getHorzStride();

                        if (lb_dst <= rb && rb_dst >= lb)
                        {
                            // Early return case
                            // This function is entered when an existing
                            // value is found in LVN table. startInst is
                            // the instruction that is expected to be
                            // optimized away. However, if it so happens
                            // that startInst defs a partial subset of
                            // data flowing in to a src operand, we can
                            // postpone doing DU for the BB. This helps
                            // large BBs with false starts for du
                            // computation.
                            if (activeDst == startInstDst &&
                                numEdgesAdded == 0)
                            {
                                if (!(lb_dst <= lb && rb_dst >= rb))
                                {
                                    duTablePopulated = false;
                                    return;
                                }
                            }

                            addUse(activeDst, curInst, i);
                            numEdgesAdded++;

                            if (activeDst->getInst()->getPredicate() == NULL &&
                                (lb_dst <= lb && rb_dst >= rb &&
                                (hs_dst == hs ||
                                (hs_dst == 1 && hs == 0 && activeDst->getInst()->getExecSize() == 1))))
                            {
                                // Active def (in bottom-up order)
                                // fully defines use of current inst. So no
                                // need to analyze earlier appearing defs.
                                break;
                            }
                        }

                        if (done ||
                            it == range_it.first)
                        {
                            // Last match reached
                            break;
                        }
                        it--;
                    }
                }
            }
        }

        G4_DstRegRegion* dst = curInst->getDst();
        if (dst)
        {
            G4_Declare* curDstTopDcl = dst->getTopDcl();
            if (curDstTopDcl != NULL)
            {
                // Check if already an overlapping dst region is active
                auto range_it = activeDefs.equal_range(curDstTopDcl->getDeclId());
                for (auto it = range_it.first;
                    it != range_it.second;
                    )
                {
                    G4_DstRegRegion* activeDst = (*it).second.second;

                    unsigned int lb = dst->getLeftBound();
                    unsigned int rb = dst->getRightBound();
                    unsigned int hs = dst->getHorzStride();

                    unsigned int lb_dst = activeDst->getLeftBound();
                    unsigned int rb_dst = activeDst->getRightBound();
                    unsigned int hs_dst = activeDst->getHorzStride();

                    if ((lb <= lb_dst && rb >= rb_dst) &&
                        hs == hs_dst)
                    {
                        if (curInst->getPredicate() == NULL)
                        {
                            // Current dst completely overlaps
                            // earlier def so retire earlier
                            // active def.
                            auto prev_it = it;
                            it++;
                            activeDefs.erase(prev_it);
                            continue;
                        }
                    }

                    it++;
                }

                bool addValueToDU = false;
                if (addValue(curInst))
                {
                    addValueToDU = true;
                }

                if (addValueToDU ||
                    (activeDefs.find(curDstTopDcl->getDeclId()) != activeDefs.end()))
                {
                    // mov (8) V10(0,0):d     r0.0:d - 1
                    // shr (1) V10(0,1):d     0x1:d  - 2
                    // send ... V10 ...              - 3
                    //
                    // Since inst 1 is a valid LVN candidate it will be inserted
                    // in activeDefs table. Inst 2 is not an LVN candidate but
                    // it clobbers def of inst 1, hence we have to insert it in
                    // the LVN table too. If first instruction wasnt an LVN
                    // candidate by itself, we wouldnt insert it or inst 2 in
                    // active def table.
                    ActiveDef newActiveDef;
                    newActiveDef.first = curDstTopDcl;
                    newActiveDef.second = dst;
                    activeDefs.insert(make_pair(curDstTopDcl->getDeclId(), newActiveDef));
                }
            }
        }

        inst_it++;
    }
}

// Return true for following pattern:
// mov (8) V1(0,0)<1>:d    r0.0<8;8,1>:d <-- lvnItem->inst
// mov (1) V1(0,2)<1>:d    0xsomeconst1:d
// send (16) null<1>:d     V1 ...
// ...
// mov (1) V1(0,2)<1>:d    0xsomeconst1:d <-- inst
// send (16) null<1>:d     V1 ...
//
// inst is redundant and this function returns true. Caller will
// eliminate inst.
//
// This pattern is a special case because V1(0,0) at inst is a
// partial write and will not be caught by regular value
// numbering code.
bool LVN::isRedundantMovToSelf(LVNItemInfo* lvnItem, G4_INST* inst)
{
    bool isRedundant = false;
    G4_INST* lvnInst = lvnItem->inst;

    if (lvnInst->getDst()->getTopDcl() == inst->getDst()->getTopDcl() &&
        lvnInst->getExecSize() == inst->getExecSize() &&
        lvnInst->getDst()->getLeftBound() == inst->getDst()->getLeftBound() &&
        lvnInst->getDst()->getRightBound() == inst->getDst()->getRightBound() &&
        lvnInst->getSrc(0)->isImm() &&
        inst->getSrc(0)->isImm())
    {
        Value instValue;
        bool uselessRef;
        int64_t imm = 0;
        computeValue(inst, false, uselessRef, uselessRef, imm, false, instValue);
        instValue.setInst(inst);
        if (isSameValue(instValue, lvnItem->value))
        {
            isRedundant = true;
        }
    }

    return isRedundant;
}

void LVN::doLVN()
{
    bb->resetLocalId();
    for (INST_LIST_ITER inst_it = bb->instList.begin(), inst_end_it = bb->instList.end();
        inst_it != inst_end_it;
        inst_it++)
    {
        G4_INST* inst = (*inst_it);
        bool negMatch = false;

        if (inst->getDst() == NULL)
        {
            continue;
        }

        bool canAdd = addValue(inst);
        Value value, oldValue;
        bool canNegate = false, isGlobal = false;
        G4_INST* lvnInst = NULL;
        LVNItemInfo* lvnItem = NULL;
        int64_t posVal = 0;
        bool addGlobalValueToTable = false;

        value.initializeEmptyValue();
        oldValue.initializeEmptyValue();
        if (canAdd)
        {
            // Compute value of current instruction
            computeValue(inst, false, canNegate, isGlobal, posVal, false, value);
            value.setInst(inst);
            oldValue.copyValue(value);

            if (isGlobal || value.isValueEmpty())
            {
                if (isGlobal && !value.isValueEmpty())
                {
                    // If variable is global, we want to add it to LVN table.
                    addGlobalValueToTable = true;
                }

                // If a value is global or is empty we cannot eliminate
                // the definition just because another variable holds
                // same data. So the logic of replacing operands is
                // skipped with canAdd == false.
                canAdd = false;
            }

            if (canAdd)
            {
                // Check if value exists in table
                lvnItem = isValueInTable(value);

                // If value doesnt exist, search for value's
                // negative representation (only for imm).

                if (canNegate &&
                    lvnItem == NULL)
                {
                    computeValue(inst, true, canNegate, isGlobal, posVal, true, value);
                    value.setInst(inst);
                    negMatch = true;

                    lvnItem = isValueInTable(value);
                }

                if (lvnItem != NULL)
                {
                    lvnInst = lvnItem->inst;
                    if (inst->getLocalId() - lvnInst->getLocalId() > LVN::MaxLVNDistance)
                    {
                        // do not do LVN to avoid register pressure increase
                        // removeRedef should get rid of this lvnInst later
                    }
                    else
                    {
                        auto dstRegionMatch = [](G4_DstRegRegion* dst1, G4_DstRegRegion *dst2)
                        {
                            return dst1->getType() == dst2->getType() && dst1->getRegOff() == dst2->getRegOff() &&
                                dst1->getSubRegOff() == dst2->getSubRegOff() && dst1->getHorzStride() == dst2->getHorzStride();

                        };
                        // if this and lvnInst has identical dst regions, we can replace partial uses as well
                        bool hasSameDstRegion = dstRegionMatch(inst->getDst(), lvnInst->getDst());
                        // How can duTablePopulated be false here? I don't understand the other comment???
                        if (duTablePopulated == false)
                        {
                            populateDuTable(inst_it);
                        }

                        bool removeInst = false;
                        if (duTablePopulated)
                        {
                            UseList uses;
                            bool defFound = getAllUses(inst, uses);
                            if (defFound)
                            {
                                bool canReplaceAllUses = canReplaceUses(inst_it, uses, lvnInst, negMatch, !hasSameDstRegion);

                                if (canReplaceAllUses)
                                {
                                    replaceAllUses(inst, negMatch, uses, lvnInst, hasSameDstRegion);
                                    removeInst = true;
                                }
                            }
                        }

                        if (!removeInst)
                        {
                            removeInst = isRedundantMovToSelf(lvnItem, inst);
                        }

                        if (removeInst)
                        {
                            INST_LIST_ITER prev_it = inst_it;
                            inst_it--;
                            bb->instList.erase(prev_it);

                            numInstsRemoved++;
                            continue;
                        }
                    }
                }
            }
        }

        if (addGlobalValueToTable)
        {
            // We are here because current instruction's dst
            // is a global but not empty. Even though current
            // inst's dst is a global we can eliminate current
            // instruction if data being written to dst is
            // same as what an earlier inst in current BB
            // wrote. This happens in case of samplerHeader
            // variables when only rx.2 is updated per BB
            // and rx.0<8;8,1>:d is copied from r0 in entry BB.
            lvnItem = isValueInTable(value);
            if (lvnItem)
            {
                // ToDo: can we move this to a separate pass or at least not nested with rest of LVN?
                // it'll make the code easier to read. 
                // Also don't we have other passes (cleanMessageHeader, cleanupBindless) doing
                // essentially the same thing? 
                bool removeInst = isRedundantMovToSelf(lvnItem, inst);

                if (removeInst)
                {
                    INST_LIST_ITER prev_it = inst_it;
                    inst_it--;
                    bb->instList.erase(prev_it);

                    numInstsRemoved++;
                    continue;
                }
            }
        }

        // Scan LVN table and remove all those var entries
        // that have same variable as current inst's dst.
        removeRedefs(inst);

        if (canAdd ||
            addGlobalValueToTable)
        {
            addValueToTable(inst, oldValue);
        }
    }
}
