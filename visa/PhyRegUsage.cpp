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

#include "PhyRegUsage.h"
#include "FlowGraph.h"
#include "GraphColor.h"

using namespace std;
using namespace vISA;

PhyRegUsage::PhyRegUsage(PhyRegUsageParms& p) :
    colorHeuristic(FIRST_FIT),
    startARFReg(p.startARFReg),
    startFLAGReg(p.startFlagReg),
    startGRFReg(p.startGRFReg),
    bank1_start(p.bank1_start),
    bank1_end(p.bank1_end),
    bank2_start(p.bank2_start),
    bank2_end(p.bank2_end),
    honorBankBias(p.doBankConflict),
    builder(*p.gra.kernel.fg.builder),
    regPool(p.gra.regPool),
    availableGregs(p.availableGregs),
    availableSubRegs(p.availableSubRegs),
    availableAddrs(p.availableAddrs),
    availableFlags(p.availableFlags),
    gra(p.gra)
{
    maxGRFCanBeUsed = p.maxGRFCanBeUsed;
    regFile = p.rFile;

    totalGRFNum = p.gra.kernel.getOptions()->getuInt32Option(vISA_TotalGRFNum);

    weakEdgeUsage = p.weakEdgeUsage;
    overlapTest = false;

    memset(availableGregs, true, sizeof(bool)* totalGRFNum);
    memset(availableSubRegs, 0xff, sizeof(uint16_t)*totalGRFNum);
    if (weakEdgeUsage)
    {
        memset(weakEdgeUsage, 0, sizeof(uint8_t)* totalGRFNum);
    }

    auto numAddrRegs = getNumAddrRegisters();
    for (unsigned i = 0; i < numAddrRegs; i++)
        availableAddrs[i] = true;

    auto numFlags = getNumFlagRegisters();
    for (unsigned i = 0; i < numFlags; i++)
        availableFlags[i] = true;
}

void PhyRegUsage::markBusyForDclSplit(G4_RegFileKind kind,
    unsigned regNum,
    unsigned regOff,
    unsigned nunits,  //word units
    unsigned numRows)
{
    MUST_BE_TRUE(numRows > 0 && nunits > 0, ERROR_INTERNAL_ARGUMENT);
    MUST_BE_TRUE(regNum + numRows <= maxGRFCanBeUsed, ERROR_UNKNOWN);

    unsigned start_GRF = (regNum * G4_GRF_REG_SIZE + regOff) / G4_GRF_REG_SIZE;
    unsigned end_GRF = (regNum * G4_GRF_REG_SIZE + regOff + nunits) / G4_GRF_REG_SIZE;

    unsigned start_sub_GRF = (regNum * G4_GRF_REG_SIZE + regOff) % G4_GRF_REG_SIZE;
    unsigned end_sub_GRF = (regNum * G4_GRF_REG_SIZE + regOff + nunits) % G4_GRF_REG_SIZE;

    for (unsigned i = start_GRF; i < end_GRF; i++)
    {
        availableGregs[i] = false;
        availableSubRegs[i] = 0; //Is this right?
    }

    if (end_sub_GRF)
    {
        availableGregs[end_GRF] = false;
        if (start_GRF == end_GRF)
        {
            uint16_t subregMask = getSubregBitMask(start_sub_GRF, nunits);
            availableSubRegs[end_GRF] &= ~subregMask;
        }
        else
        {
            uint16_t subregMask = getSubregBitMask(0, end_sub_GRF);
            availableSubRegs[end_GRF] &= ~subregMask;
        }
    }
}
//
// mark availRegs[start ... start+num-1] free again
//
void PhyRegUsage::freeContiguous(bool availRegs[],
    unsigned start,
    unsigned num,
    unsigned maxRegs)
{
    for (unsigned i = start; i < start + num; i++)
    {
        MUST_BE_TRUE(i < maxRegs && availRegs[i] == false,
            ERROR_UNKNOWN);
        availRegs[i] = true;
    }
}
//
// return true if all entries are true
//
bool PhyRegUsage::allFree(bool availRegs[], unsigned maxRegs)
{
    for (unsigned i = 0; i < maxRegs; i++)
    {
        if (availRegs[i] == false)
            return false;
    }
    return true;
}

//
// mark sub reg [regOff .. regOff + nbytes -1] of the reg regNum free
//
void PhyRegUsage::freeGRFSubReg(unsigned regNum,
    unsigned regOff,
    unsigned nwords,
    G4_Type  ty)
{
    //
    // adjust regOff to its corresponding word position
    //

    int startWord = regOff*G4_Type_Table[ty].byteSize / G4_WSIZE;
    uint16_t subregMask = getSubregBitMask(startWord, nwords);
    availableSubRegs[regNum] |= subregMask;

    //
    // if all sub regs of regNum are free, then unlink the reg
    //
    if (availableSubRegs[regNum] == 0xFFFF)
    {
        MUST_BE_TRUE(!availableGregs[regNum],
            ERROR_UNKNOWN);
        availableGregs[regNum] = true;
    }
}

//
// free registers that are held by intv
//
void PhyRegUsage::freeRegs(VarBasis* varBasis)
{
    G4_Declare* decl = varBasis->getVar()->getDeclare();
    G4_RegFileKind kind = decl->getRegFile();
    MUST_BE_TRUE(varBasis->getPhyReg(),
        ERROR_UNKNOWN);
    if (decl->useGRF())
    {
        MUST_BE_TRUE(varBasis->getPhyReg()->isGreg(), ERROR_UNKNOWN);
        if (canGRFSubRegAlloc(decl))
        {
            freeGRFSubReg(((G4_Greg*)varBasis->getPhyReg())->getRegNum(), varBasis->getPhyRegOff(),
                numAllocUnit(decl->getNumElems(), decl->getElemType()), decl->getElemType());
        }
        else
        {
            freeContiguous(availableGregs, ((G4_Greg*)varBasis->getPhyReg())->getRegNum(),
                decl->getNumRows(), totalGRFNum);
        }
    }
    else if (kind == G4_ADDRESS)
    {
        MUST_BE_TRUE(varBasis->getPhyReg()->isAreg(), ERROR_UNKNOWN);
        freeContiguous(availableAddrs, varBasis->getPhyRegOff(),
            numAllocUnit(decl->getNumElems(), decl->getElemType()), getNumAddrRegisters());
    }
    else if (kind == G4_FLAG)
    {
        MUST_BE_TRUE(varBasis->getPhyReg()->isFlag(), ERROR_UNKNOWN);
        freeContiguous(availableFlags, varBasis->getPhyRegOff(),
            numAllocUnit(decl->getNumElems(), decl->getElemType()), getNumFlagRegisters());
    }
    else // not yet handled
        MUST_BE_TRUE(false, ERROR_UNKNOWN);
}

static int getStepAccordingSubAlign(G4_SubReg_Align subAlign)
{
    switch(subAlign)
    {
        case Even_Word:
        	return 2;
        case Four_Word:
        	return 4;
        case Eight_Word:
        	return 8;
        case Sixteen_Word:
         	return 16;
        default:
        	return 1;
    }
    return 1;
}

// returns the starting word index if we find enough free contiguous words satisfying alignment,
// -1 otherwise
int PhyRegUsage::findContiguousWords(
    uint16_t words,
    G4_SubReg_Align subAlign,
    int numWords) const
{
    // early exit in (false?) hope of saving compile time
    if (words == 0)
    {
        return -1;
    }

    int step = getStepAccordingSubAlign(subAlign);
    int startWord = 0;

    for (int i = startWord; i + numWords <= 16; i += step)
    {
        uint16_t bitMask = getSubregBitMask(i, numWords);
        if ((bitMask & words) == bitMask)
        {
            return i;
        }
    }

    return -1;
}

//
// look for contiguous available regs starting from startPos
//
bool PhyRegUsage::findContiguousGRF(bool availRegs[],
    const bool forbidden[],
    G4_Align align,
    unsigned numRegNeeded,
    unsigned maxRegs,
    unsigned& startPos,
    unsigned& idx,
    bool isCalleeSaveBias,
    bool isEOTSrc)
{
    unsigned startPosRunOne = startPos;
    unsigned endPosRunOne = maxRegs;

    if (isEOTSrc && (startPosRunOne >= maxRegs))
    {
        return false;
    }
    else
    {
        MUST_BE_TRUE(startPosRunOne < maxRegs, ERROR_UNKNOWN);
    }
    bool found =
        findContiguousNoWrapGRF(
        availRegs, forbidden, align, numRegNeeded, startPosRunOne, endPosRunOne, idx);

    if (startPosRunOne > 0 && found == false && !isEOTSrc && !isCalleeSaveBias)
    {
        unsigned startPosRunTwo = 0;
        unsigned endPosRunTwo = startPos + numRegNeeded;
        endPosRunTwo = std::min(endPosRunTwo, maxRegs);
        MUST_BE_TRUE(endPosRunTwo > 0 && endPosRunTwo <= maxRegs, ERROR_UNKNOWN);
        found =
            findContiguousNoWrapGRF(
            availRegs, forbidden, align, numRegNeeded, startPosRunTwo, endPosRunTwo, idx);
    }

    if (found)
    {
        MUST_BE_TRUE(idx < maxRegs && idx + numRegNeeded <= maxRegs, ERROR_UNKNOWN);

        if (colorHeuristic == ROUND_ROBIN) {
            startPos = (idx + numRegNeeded) % maxRegs;
        }
    }

    return found;
}

//
// look for contiguous available regs starting from startPos
//
bool PhyRegUsage::findContiguousAddrFlag(bool availRegs[],
    const bool forbidden[],
    G4_SubReg_Align subAlign,
    unsigned numRegNeeded,
    unsigned maxRegs,
    unsigned& startPos,
    unsigned& idx,
    bool isCalleeSaveBias,
    bool isEOTSrc)
{
    unsigned startPosRunOne = startPos;
    unsigned endPosRunOne = maxRegs;

    if (isEOTSrc && (startPosRunOne >= maxRegs))
    {
        return false;
    }
    else
    {
        MUST_BE_TRUE(startPosRunOne < maxRegs, ERROR_UNKNOWN);
    }
    bool found =
        findContiguousNoWrapAddrFlag(
        availRegs, forbidden, subAlign, numRegNeeded, startPosRunOne, endPosRunOne, idx);

    if (startPosRunOne > 0 && found == false && !isEOTSrc && !isCalleeSaveBias)
    {
        unsigned startPosRunTwo = 0;
        unsigned endPosRunTwo = startPos + numRegNeeded;
        endPosRunTwo = std::min(endPosRunTwo, maxRegs);
        MUST_BE_TRUE(endPosRunTwo > 0 && endPosRunTwo <= maxRegs, ERROR_UNKNOWN);
        found =
            findContiguousNoWrapAddrFlag(
            availRegs, forbidden, subAlign, numRegNeeded, startPosRunTwo, endPosRunTwo, idx);
    }

    if (found)
    {
        MUST_BE_TRUE(idx < maxRegs && idx + numRegNeeded <= maxRegs, ERROR_UNKNOWN);

        if (colorHeuristic == ROUND_ROBIN) {
            startPos = (idx + numRegNeeded) % maxRegs;
        }
    }

    return found;
}

bool PhyRegUsage::findContiguousGRFFromBanks(G4_Declare *dcl,
    bool availRegs[],
    const bool forbidden[],
    G4_Align orgAlign,
    unsigned& idx,
    bool oneGRFBankDivision)
{   // EOT is not handled in this function
    bool found = false;
    unsigned numRegNeeded = dcl->getNumRows();
    G4_Align align = orgAlign;
    auto dclBC = gra.getBankConflict(dcl);
    bool gotoSecondBank = (dclBC == BANK_CONFLICT_SECOND_HALF_EVEN ||
        dclBC == BANK_CONFLICT_SECOND_HALF_ODD) && (dcl->getNumRows() > 1);

    if ((dclBC != BANK_CONFLICT_NONE) &&
    	(align == Either) &&
    	(dcl->getNumRows() <= 1))
    {
        align = gra.getBankAlign(dcl);
    }

    ASSERT_USER(bank1_end < 128 && bank1_start < 128 && bank2_start < 128 && bank2_end < 128,
        "Wrong bank boundaries value");

    if (colorHeuristic == ROUND_ROBIN)
    {
        //For round robin, bank1_end and bank2_end are fixed.
        if (gotoSecondBank)  //For odd aligned varaibe, we put them to a specific sections.
        {
            //From maxGRFCanBeUsed - 1 to bank2_end
            ASSERT_USER(bank2_start >= bank2_end, "Second bank's start can not less than end\n");

            if ((bank2_start - bank2_end + 1) >= numRegNeeded) //3 - 2 + 1 >= 2
            {
                found = findFreeRegs(
                    availRegs, forbidden, align, numRegNeeded, bank2_start, bank2_end, idx, gotoSecondBank, oneGRFBankDivision);
            }

            if (!found)
            {
                if (maxGRFCanBeUsed - 1 >= bank2_start + numRegNeeded)
                {
                    found = findFreeRegs(
                        availRegs, forbidden, align, numRegNeeded, maxGRFCanBeUsed - 1, bank2_start + 1, idx, gotoSecondBank, oneGRFBankDivision);
                }
                else
                {
                    return false;
                }
            }

            if (found)
            {
                bank2_start = idx - 1;
                if (bank2_start < bank2_end)
                {
                    bank2_start = maxGRFCanBeUsed - 1;
                }
            }
        }
        else
        {   //From 0 to bank1_end
            if (bank1_end - bank1_start + 1 >= numRegNeeded)
            {
                found = findFreeRegs(
                    availRegs, forbidden, Even, numRegNeeded, bank1_start, bank1_end, idx, gotoSecondBank, oneGRFBankDivision);
            }

            if (!found)
            {
                if (bank1_start >= numRegNeeded)
                {
                    found = findFreeRegs(
                        availRegs, forbidden, Even, numRegNeeded, 0, bank1_start - 2 + numRegNeeded, idx, gotoSecondBank, oneGRFBankDivision);
                }
            }

            if (found)
            {
                bank1_start = idx + numRegNeeded;
                if (bank1_start > bank1_end)
                {
                    bank1_start = 0;
                }
            }
        }
    }
    else
    {
        //For first fit, the bank1_start and bank2_start are fixed. bank2_end and bank1_end are dynamically decided, but can not change in one direction (MIN or MAX).
        if (gotoSecondBank)  //For odd aligned varaibe, we put them to a specific sections.
        {
            found = findFreeRegs(
                availRegs, forbidden, align, numRegNeeded, maxGRFCanBeUsed - 1, 0, idx, gotoSecondBank, oneGRFBankDivision);

            if (found)
            {
                bank2_end = std::min(idx, bank2_end);
            }
        }
        else
        {
            found = findFreeRegs(
                availRegs, forbidden, align, numRegNeeded, 0, maxGRFCanBeUsed - 1, idx, gotoSecondBank, oneGRFBankDivision);
            if (found)
            {
                bank1_end = std::max(idx + numRegNeeded - 1, bank1_end);
            }
        }

        if (bank2_end <= bank1_end)
        {
            found = false;
        }

    }

    return found;
}

bool PhyRegUsage::isOverlapValid(unsigned int reg, unsigned int numRegs)
{
    for (unsigned int i = reg; i < (reg + numRegs); i++)
    {
        auto k = getWeakEdgeUse(i);
        if (!(k == 0 ||
            k == (i - reg + 1)))
        {
            // This condition will be taken when there is a partial
            // overlap.
            return false;
        }
    }

    return true;
}

//
// look for contiguous available regs from startPos to maxRegs
//
bool PhyRegUsage::findContiguousNoWrapGRF(bool availRegs[],
    const bool forbidden[],
    G4_Align align,
    unsigned numRegNeeded,
    unsigned startPos,
    unsigned endPos,
    unsigned& idx)
{
    unsigned i = startPos;
    while (i < endPos)
    {
        if (((i & 0x1) && align == Even) || // i is odd but intv needs to be even aligned
            ((i & 0x1) == 0 && align == Odd)) // i is even but intv needs to be odd aligned
        {
            i++;
        } else {
            if (align == Even2GRF)
            {
                while ((i % 4 >= 2) || ((numRegNeeded >= 2) && (i % 2 != 0)))
                {
                    i++;
                }
            }
            else if (align == Odd2GRF)
            {
                while ((i % 4 < 2) || ((numRegNeeded >= 2) && (i % 2 != 0)))
                {
                    i++;
                }
            }

            if (numRegNeeded == 0 ||
            	i + numRegNeeded > endPos)
                return false; // no available regs
            //
            // find contiguous numRegNeeded registers
            // forbidden != NULL then check forbidden
            //
            unsigned j = i;
            if (overlapTest &&
                !isOverlapValid(i, numRegNeeded))
            {
                i++;
            }
            else
            {
                for (; j < i + numRegNeeded && availRegs[j] && (forbidden == NULL || !forbidden[j]); j++);
                if (j == i + numRegNeeded)
                {
                    for (unsigned k = i; k < j; k++) availRegs[k] = false;
                    idx = i;
                    return true;
                }
                else
                    i = j + 1;
            }
        }
    }
    return false; // no available regs
}

//
// look for contiguous available regs from startPos to maxRegs
//
bool PhyRegUsage::findContiguousNoWrapAddrFlag(bool availRegs[],
    const bool forbidden[],
    G4_SubReg_Align subAlign, //Sub align is used only for Flag and Address registers
    unsigned numRegNeeded,
    unsigned startPos,
    unsigned endPos,
    unsigned& idx)
{
    unsigned i = startPos;
    while (i < endPos)
    {
        //
        // some register assignments need special alignment, we check
        // whether the alignment criteria is met.
        //
        if (subAlign == Sixteen_Word && i != 0)	{	// Sixteen_Word sub-align should have i=0
            return false;
        } else if ((subAlign > Eight_Word && subAlign < Sixteen_Word && i != (unsigned)(Sixteen_Word - subAlign)) ||	// 9_Word~15_Word align
            (subAlign == Eight_Word && i % 8 != 0) ||	// 8_Word align, i must be divided by 8
            (i & 0x1 && subAlign == Even_Word) || // i is odd but intv needs to be even aligned
            (subAlign == Four_Word && (i % 4 != 0))) // 4_word alignment
            i++;
        else
        {
            if (numRegNeeded == 0 ||
            	i + numRegNeeded > endPos)
            {
                return false; // no available regs
            }
            //
            // find contiguous numRegNeeded registers
            // forbidden != NULL then check forbidden
            //
            unsigned j = i;
            for (; j < i + numRegNeeded && availRegs[j] && (forbidden == NULL || !forbidden[j]); j++);
            if (j == i + numRegNeeded)
            {
                for (unsigned k = i; k < j; k++) availRegs[k] = false;
                idx = i;
                return true;
            } else {
                i = j + 1;
            }
        }
    }
    return false; // no available regs
}

bool PhyRegUsage::findFreeRegs(bool availRegs[],
    const bool forbidden[],
    G4_Align align,
    unsigned numRegNeeded,
    unsigned startRegNum,  //inclusive
    unsigned endRegNum, //inclusive: less and equal when startRegNum <= endRegNum, larger and equal when startRegNum > endRegNum
    unsigned& idx,
    bool gotoSecondBank,
    bool oneGRFBankDivision)
{
    bool forward = startRegNum <= endRegNum ? true : false;
    int startReg = forward ? startRegNum : startRegNum - numRegNeeded + 1;
    int endReg = forward ? endRegNum - numRegNeeded + 1 : endRegNum;
    int i = startReg;

    while (1)
    {
        if (forward)
        {
            if (i > endReg)
                break;
        }
        else
        {
            if (i < endReg)
                break;
        }

        if ((align == Even2GRF) && (i % 2 != 0 ||  i % 4 == 3))
        {
            if (forward) { i++; }
            else { i--; }
            continue;
        }
        else if ((align == Odd2GRF) && (i % 2 != 0 || i % 4 == 1))
        {
            if (forward) { i++; }
            else { i--; }
            continue;
        }
        else if ((((i & 0x1) && align == Even) || // i is odd but intv needs to be even aligned
            (((i & 0x1) == 0) && (align == Odd)))) // i is even but intv needs to be odd aligned
        {
            if (forward) { i++; }
            else { i--; }
            continue;
        }
        else
        {
            if ((forward && (i > endReg)) ||
                (!forward && (i < endReg)))
            {
                return false; // no available regs
            }

            if (regFile == G4_GRF &&
                overlapTest &&
                !isOverlapValid(i, numRegNeeded))
            {
                if (forward)
                    i++;
                else
                    i--;
            }
            else
            {
                // find contiguous numRegNeeded registers
                // forbidden != NULL then check forbidden
                //
                unsigned j = i;
                for (; j < i + numRegNeeded && availRegs[j] && (forbidden == NULL || !forbidden[j]); j++);
                if (j == i + numRegNeeded)
                {
                    for (unsigned k = i; k < j; k++) availRegs[k] = false;
                    idx = i;
                    return true;
                }
                else
                {   //Jump over the register region which a poke in the end
                    if (forward)
                    {
                        i = j + 1;
                    }
                    else
                    {
                        if (j > numRegNeeded)
                        {
                            i = j - numRegNeeded;
                        }
                        else
                        {
                            break;
                        }
                    }
                }
            }
        }
    }

    return false;
}

//
// return true, if the var can be allocated using sub reg
//
bool PhyRegUsage::canGRFSubRegAlloc(G4_Declare* decl)
{
    if (decl->getNumRows() != 1) // more than 1 row
        return false;
    if (numAllocUnit(decl->getNumElems(), decl->getElemType()) < G4_GRF_REG_SIZE)
        return true;
    return false;
}

void PhyRegUsage::findGRFSubRegFromRegs(int startReg,
    int endReg,
    int step,
    PhyReg *phyReg,
    G4_SubReg_Align subAlign,
    unsigned nwords,
    const bool forbidden[],
    bool fromPartialOccupiedReg)
{
    int idx = startReg;
    while (1)
    {
        if (step > 0)
        {
            if (idx > endReg)
            {
                break;
            }
        }
        else
        {
            if (idx < endReg)
            {
                break;
            }
        }

        if (forbidden && forbidden[idx])
        {
            idx += step;
            continue;
        }

        if (fromPartialOccupiedReg && availableSubRegs[idx] == 0xFFFF)
        {
            // favor partially allocated GRF first
            idx += step;
            continue;
        }

        int subreg = findContiguousWords(availableSubRegs[idx], subAlign, nwords);
        if (subreg != -1)
        {
            phyReg->reg = idx;
            phyReg->subreg = subreg;
            return;
        }

        idx += step;
    }

    return;
}

PhyRegUsage::PhyReg PhyRegUsage::findGRFSubRegFromBanks(G4_Declare *dcl,
    const bool forbidden[],
    bool oneGRFBankDivision)
{
    int startReg = 0, endReg = totalGRFNum;
    int step = 0;
    G4_SubReg_Align subAlign = dcl->getRegVar()->getSubRegAlignment();
    unsigned nwords = numAllocUnit(dcl->getNumElems(), dcl->getElemType());
    auto dclBC = gra.getBankConflict(dcl);
    bool gotoSecondBank = dclBC == BANK_CONFLICT_SECOND_HALF_EVEN ||
        dclBC == BANK_CONFLICT_SECOND_HALF_EVEN;

    if (gotoSecondBank && oneGRFBankDivision)
    {
        startReg = (maxGRFCanBeUsed - 1);
        startReg = startReg % 2 ? startReg : startReg - 1;
        if (colorHeuristic == ROUND_ROBIN)
        {
            endReg = bank2_end;
        }
        else
        {
            endReg = 0;
        }
        step = -2;
    }
    else if (gotoSecondBank && !oneGRFBankDivision)  //We will depends on low high, treated as even align
    {
        startReg = (maxGRFCanBeUsed - 1);
        startReg = startReg % 2 ? startReg - 1 : startReg;
        if (colorHeuristic == ROUND_ROBIN)
        {
            endReg = bank2_end;
        }
        else
        {
            endReg = 0;
        }
        step = -1;
    }
    else
    {
        if (colorHeuristic == ROUND_ROBIN)
        {
            startReg = 0;
            endReg = bank1_end;
        }
        else
        {
            startReg = 0;
            endReg = maxGRFCanBeUsed - 1;
        }
        if (oneGRFBankDivision)
        {
            step = 2;
        }
        else
        {
            step = 1;
        }
    }

    PhyReg phyReg = { -1, -1 };

    //Try to find sub register from the registers which are partially occupied already.
    findGRFSubRegFromRegs(startReg, endReg, step, &phyReg, subAlign, nwords, forbidden, true);

    //If failed or across the boundary of specified bank, try again and find from the registers which are totally free
    if (phyReg.reg == -1 || (gotoSecondBank && ((unsigned)phyReg.reg <= bank1_end)) || (!gotoSecondBank && ((unsigned)phyReg.reg >= bank2_end)))
    {
        findGRFSubRegFromRegs(startReg, endReg, step, &phyReg, subAlign, nwords, forbidden, false);
    }

    if (phyReg.reg != -1 && colorHeuristic == FIRST_FIT)
    {
        if (gotoSecondBank)
        {
            bank2_end = std::min((unsigned)phyReg.reg, bank2_end);
        }
        else
        {
            bank1_end = std::max((unsigned)phyReg.reg, bank1_end);
        }
        if (bank1_end >= bank2_end)
        {
            phyReg.reg = -1;
        }
    }

    return phyReg;
}

//
// return reg and subRegOff (subr)
// To support sub-reg alignment
//
PhyRegUsage::PhyReg PhyRegUsage::findGRFSubReg(const bool forbidden[],
    bool calleeSaveBias,
    bool callerSaveBias,
    G4_Align align,
    G4_SubReg_Align subAlign,
    unsigned nwords)
{

    int startReg = 0, endReg = totalGRFNum;
    PhyReg phyReg = { -1, -1 };
    if (calleeSaveBias)
    {
        startReg = builder.kernel.calleeSaveStart();
    }
    else if (callerSaveBias)
    {
        endReg = builder.kernel.calleeSaveStart();
    }

    int step = align == Even ? 2 : 1;

    for (int idx = startReg; idx < endReg; idx += step)
    {
        if (forbidden && forbidden[idx])
        {
            continue;
        }

        if (availableSubRegs[idx] == 0xFFFF)
        {
            // favor partially allocated GRF first
            continue;
        }
        int subreg = findContiguousWords(availableSubRegs[idx], subAlign, nwords);
        if (subreg != -1)
        {
            phyReg.reg = idx;
            phyReg.subreg = subreg;
            return phyReg;
        }
    }

    // full search as we skipped fully free GRFs earlier
    for (unsigned int idx = 0; idx < totalGRFNum; idx += step)
    {
        if (forbidden && forbidden[idx])  // forbidden != NULL check forbidden
        {
            continue;
        }

        int subreg = findContiguousWords(availableSubRegs[idx], subAlign, nwords);
        if (subreg != -1)
        {
            phyReg.reg = idx;
            phyReg.subreg = subreg;
            return phyReg;
        }
    }

    return phyReg;
}

bool PhyRegUsage::assignGRFRegsFromBanks(VarBasis*	 varBasis,
    G4_Align  align,
    const bool*     forbidden,
    ColorHeuristic  heuristic,
    bool oneGRFBankDivision)
{
    colorHeuristic = heuristic;
    G4_Declare* decl = varBasis->getVar()->getDeclare();

    //
    // if regs are allocated to intv, i is the reg number and off is the reg
    // offset for sub reg allocation
    //
    unsigned i = 0;   // avail reg number

    //
    // determine if we need to do sub reg allcoation
    //
    if (canGRFSubRegAlloc(decl))
    {
        bool retVal = false;

        PhyRegUsage::PhyReg phyReg = findGRFSubRegFromBanks(decl, forbidden, oneGRFBankDivision);
        if (phyReg.reg != -1)
        {
            // based on type, adjust sub reg off accordingly
            // word: stay the same, dword: *2, byte: /2
            // assign r_i.off
            varBasis->setPhyReg(regPool.getGreg(phyReg.reg),
                phyReg.subreg*G4_WSIZE / G4_Type_Table[decl->getElemType()].byteSize);
            retVal = true;
        }

        return retVal;
    }
    else
    {
        bool success = false;
        if (varBasis->getEOTSrc() && builder.hasEOTGRFBinding())
        {
            startGRFReg = totalGRFNum - 16;
            success = findContiguousGRF(availableGregs, forbidden, align, decl->getNumRows(), maxGRFCanBeUsed,
                startGRFReg, i, false, true);
        }
        else
        {
            success = findContiguousGRFFromBanks(decl, availableGregs, forbidden, align, i, oneGRFBankDivision);
        }

        if (success)
        {
            varBasis->setPhyReg(regPool.getGreg(i), 0);
        }

        return success;
    }

    return false;
}

//
// find registers for intv
// To support sub-reg alignment
//
bool PhyRegUsage::assignRegs(bool  highInternalConflict,
    VarBasis*		 varBasis,
    const bool*     forbidden,
    G4_Align		 align,
    G4_SubReg_Align subAlign,
    ColorHeuristic  heuristic,
    float			 spillCost)
{
    colorHeuristic = heuristic;

    G4_Declare* decl = varBasis->getVar()->getDeclare();
    G4_RegFileKind kind = decl->getRegFile();
    G4_Align bankAlign = Either;

    //
    // if regs are allocated to intv, i is the reg number and off is the reg
    // offset for sub reg allocation
    //
    unsigned i = 0;   // avail reg number

    if (kind == G4_GRF) // general register file
    {
        //
        // determine if we need to do sub reg allcoation
        //
        if (canGRFSubRegAlloc(decl))
        {
            bool retVal = false;
            int oldStartGRFReg = startGRFReg;
            BankConflict varBasisBC = gra.getBankConflict(varBasis->getVar()->asRegVar()->getDeclare());

            if (!builder.getOptions()->getuInt32Option(vISA_ReservedGRFNum) &&
                totalGRFNum == 128 &&
                honorBankBias &&
                varBasisBC != BANK_CONFLICT_NONE)
            {
                if (highInternalConflict)
                {
                    switch (varBasisBC)
                    {
                    case BANK_CONFLICT_FIRST_HALF_EVEN:
                    case BANK_CONFLICT_FIRST_HALF_ODD:
                        startGRFReg = 0;
                        break;
                    case BANK_CONFLICT_SECOND_HALF_EVEN:
                    case BANK_CONFLICT_SECOND_HALF_ODD:
                        startGRFReg = 64;
                        break;
                    default: break;
                    }
                }
                else
                {
                    bankAlign = gra.getBankAlign(varBasis->getVar()->asRegVar()->getDeclare());
                }
            }

            // If the var is biased to receive a callee-bias, start at r60 and wrap around.
            // NOTE: We are assuming a first-fit strategy when a callee-bias is present.
            if (varBasis->getCalleeSaveBias())
            {
                startGRFReg = 60;
            }

            PhyRegUsage::PhyReg phyReg = findGRFSubReg(forbidden, varBasis->getCalleeSaveBias(),
                varBasis->getCallerSaveBias(), bankAlign != Either ? bankAlign : align, subAlign,
                numAllocUnit(decl->getNumElems(), decl->getElemType()));
            if (phyReg.reg != -1)
            {
                // based on type, adjust sub reg off accordingly
                // word: stay the same, dword: *2, byte: /2
                // assign r_i.off
                varBasis->setPhyReg(regPool.getGreg(phyReg.reg),
                    phyReg.subreg*G4_WSIZE / G4_Type_Table[decl->getElemType()].byteSize);
                retVal = true;
            }

            if (varBasis->getCalleeSaveBias())
            {
                startGRFReg = oldStartGRFReg;
            }

            return retVal;
        }
        else
        {
            int oldStartGRFReg = startGRFReg;
            unsigned endGRFReg = maxGRFCanBeUsed; // round-robin reg  start bias
            BankConflict varBasisBC = gra.getBankConflict(varBasis->getVar()->asRegVar()->getDeclare());

            if (!builder.getOptions()->getuInt32Option(vISA_ReservedGRFNum) &&
                totalGRFNum == 128 &&
                honorBankBias &&
                varBasisBC != BANK_CONFLICT_NONE)
            {
                if (highInternalConflict)
                {
                    switch (varBasisBC)
                    {
                    case BANK_CONFLICT_FIRST_HALF_EVEN:
                    case BANK_CONFLICT_FIRST_HALF_ODD:
                        startGRFReg = 0;
                        break;
                    case BANK_CONFLICT_SECOND_HALF_EVEN:
                    case BANK_CONFLICT_SECOND_HALF_ODD:
                        startGRFReg = 64;
                        break;
                    default: break;
                    }
                }
                else
                {
                    bankAlign = gra.getBankAlign(varBasis->getVar()->asRegVar()->getDeclare());
                }
            }

            // If the var is biased to receive a callee-bias, start at r60 and wrap around.
            // NOTE: We are assuming a first-fit strategy when a callee-bias is present.
            if (varBasis->getCalleeSaveBias())
            {
                startGRFReg = builder.kernel.calleeSaveStart();
            }


            if (varBasis->getEOTSrc() && builder.hasEOTGRFBinding())
            {
                startGRFReg = totalGRFNum - 16;
            }

            bool success = findContiguousGRF(availableGregs, forbidden, bankAlign != Either ? bankAlign : align, decl->getNumRows(), endGRFReg,
                startGRFReg, i, varBasis->getCalleeSaveBias(), varBasis->getEOTSrc());
            if (success) {
                varBasis->setPhyReg(regPool.getGreg(i), 0);
            }

            if (varBasis->getEOTSrc())
            {
                startGRFReg = oldStartGRFReg;
            }

            if (varBasis->getCalleeSaveBias())
            {
                startGRFReg = oldStartGRFReg;
            }

            return success;
        }
    }
    else if (kind == G4_ADDRESS) // address register
    {
        MUST_BE_TRUE(decl->getNumRows() == 1, ERROR_UNKNOWN);
        //
        // determine alignment
        // if the number of reg needed is more than 1, then we go ahead
        //
        unsigned regNeeded = numAllocUnit(decl->getNumElems(), decl->getElemType());
        if (findContiguousAddrFlag(availableAddrs, forbidden, subAlign, regNeeded, getNumAddrRegisters(), startARFReg, i))
        {
            // subregoffset should consider the declare data type
            varBasis->setPhyReg(regPool.getAddrReg(), i*G4_WSIZE / G4_Type_Table[decl->getElemType()].byteSize);
            return true;
        }
        return false;
    }
    else if (kind == G4_FLAG) // Flag register
    {
        MUST_BE_TRUE(decl->getNumRows() == 1, ERROR_UNKNOWN);
        //
        // determine alignment
        // if the number of reg needed is more than 1, then we go ahead
        //
        unsigned regNeeded = numAllocUnit(decl->getNumElems(), decl->getElemType());
        if (findContiguousAddrFlag(availableFlags, forbidden, subAlign, regNeeded, getNumFlagRegisters(), startFLAGReg, i))
        {
            // subregoffset should consider the declare data type
            if (i >= 2)
            {
                varBasis->setPhyReg(regPool.getF1Reg(), i - 2);
            }
            else
            {
                varBasis->setPhyReg(regPool.getF0Reg(), i);
            }
            return true;
        }
        return false;
    }
    else // not handled yet
    {
        MUST_BE_TRUE(false, ERROR_UNKNOWN);
        return false;
    }
}

//
// allocate forbidden vectors
//
unsigned VarBasis::getForbiddenVectorSize()
{
    switch (regKind)
    {
    case G4_GRF:
    case G4_INPUT:
        return m_options->getuInt32Option(vISA_TotalGRFNum);
    case G4_ADDRESS:
        return getNumAddrRegisters();
    case G4_FLAG:
        return getNumFlagRegisters();
    default:
        assert(false && "illegal reg file");
        return 0;
    }
}

//
// allocate forbidden vectors
//
void VarBasis::allocForbiddenVector(Mem_Manager& mem)
{
    unsigned size = getForbiddenVectorSize();

    if (size > 0)
    {
        forbidden = (bool*)mem.alloc(sizeof(bool)*size);
        memset(forbidden, false, size);
    }
}

unsigned int getStackCallRegSize(bool reserveStackCallRegs)
{
    if (reserveStackCallRegs)
    {
        return 3;
    }
    else
    {
        return 0;
    }
}

void getForbiddenGRFs(vector<unsigned int>& regNum, const Options *opt, unsigned stackCallRegSize, unsigned reserveSpillSize, unsigned rerservedRegNum)
{
    // Push forbidden register numbers to vector regNum
    //
    // r0 - Forbidden when platform is not 3d
    // rMax, rMax-1, rMax-2 - Forbidden in presence of stack call sites
    unsigned totalGRFNum = opt->getuInt32Option(vISA_TotalGRFNum);

    if (opt->getTarget() != VISA_3D ||
        opt->getOption(vISA_enablePreemption) ||
		reserveSpillSize > 0 ||
        stackCallRegSize > 0 ||
        opt->getOption(vISA_ReserveR0))
    {
        regNum.push_back(0);
    }

    if (opt->getOption(vISA_enablePreemption))
    {
        // r1 is reserved for SIP kernel
        regNum.push_back(1);
    }

    unsigned reservedRegSize = stackCallRegSize + reserveSpillSize;
    for (unsigned int i = 0; i < reservedRegSize; i++)
    {
        regNum.push_back(totalGRFNum - 1 - i);
    }

    unsigned largestNoneReservedReg = totalGRFNum - reservedRegSize - 1;
    if (totalGRFNum - reservedRegSize >= totalGRFNum - 16)
    {
        largestNoneReservedReg = totalGRFNum - 16 - 1;
    }


    if (totalGRFNum - reservedRegSize < rerservedRegNum)
    {
        MUST_BE_TRUE(false, "After reservation, there is not enough regiser!");
    }

    for (unsigned int i = 0; i < rerservedRegNum; i++)
    {
        regNum.push_back(largestNoneReservedReg - i);
    }
}

void getCallerSaveGRF(vector<unsigned int>& regNum, G4_Kernel* kernel)
{
    unsigned int startCalleeSave = kernel->calleeSaveStart();
    unsigned int endCalleeSave = startCalleeSave + kernel->getNumCalleeSaveRegs();
    // r60-r124 are caller save regs for SKL
    for (unsigned int i = startCalleeSave; i < endCalleeSave; i++)
    {
        regNum.push_back(i);
    }
}

void getCalleeSaveGRF(vector<unsigned int>& regNum, G4_Kernel* kernel)
{
    // r1-r59 are callee save regs for SKL
    unsigned int numCallerSaveGRFs = kernel->getCallerSaveLastGRF() + 1;
    for (unsigned int i = 1; i < numCallerSaveGRFs; i++)
    {
        regNum.push_back(i);
    }
}

//
// mark forbidden vectors
//
void VarBasis::allocForbidden(Mem_Manager& mem, bool reserveStackCallRegs, unsigned reserveSpillSize, unsigned rerservedRegNum)
{
    if (forbidden == NULL)
    {
        allocForbiddenVector(mem);
    }


    if (regKind == G4_GRF)
    {
        vector<unsigned int> forbiddenGRFs;
        unsigned int stackCallRegSize = getStackCallRegSize(reserveStackCallRegs);
        getForbiddenGRFs(forbiddenGRFs, m_options, stackCallRegSize, reserveSpillSize, rerservedRegNum);

        for (unsigned int i = 0; i < forbiddenGRFs.size(); i++)
        {
            unsigned int regNum = forbiddenGRFs[i];
            forbidden[regNum] = true;
        }
    }
}

//
// mark forbidden registers for caller-save pseudo var
//
void VarBasis::allocForbiddenCallerSave(Mem_Manager& mem, G4_Kernel* kernel)
{
    if (forbidden == NULL)
    {
        allocForbiddenVector(mem);
    }

    MUST_BE_TRUE(regKind == G4_GRF, ERROR_UNKNOWN);

    vector<unsigned int> callerSaveRegs;
    getCallerSaveGRF(callerSaveRegs, kernel);
    for (unsigned int i = 0; i < callerSaveRegs.size(); i++)
    {
        unsigned int callerSaveReg = callerSaveRegs[i];
        forbidden[callerSaveReg] = true;
    }
}

//
// mark forbidden registers for callee-save pseudo var
//
void VarBasis::allocForbiddenCalleeSave(Mem_Manager& mem, G4_Kernel* kernel)
{
    if (forbidden == NULL)
    {
        allocForbiddenVector(mem);
    }

    MUST_BE_TRUE(regKind == G4_GRF, ERROR_UNKNOWN);

    vector<unsigned int> calleeSaveRegs;
    getCalleeSaveGRF(calleeSaveRegs, kernel);
    for (unsigned int i = 0; i < calleeSaveRegs.size(); i++)
    {
        unsigned int calleeSaveReg = calleeSaveRegs[i];
        forbidden[calleeSaveReg] = true;
    }
}

//
// print assigned reg info
//
void VarBasis::dump()
{
    G4_Declare* decl = var->getDeclare();
    DEBUG_EMIT(this);
    DEBUG_MSG(" : ");
    //
    // print alignment
    //
    if (var->getAlignment() == Even)
    {
        DEBUG_MSG("even");
    }


    DEBUG_MSG("\t");
    if (var->getSubRegAlignment() == Even_Word) {
        DEBUG_MSG("Even_word SubReg_Align");
    }
    else if (var->getSubRegAlignment() == Any)
    {
        DEBUG_MSG("\t");
    }
    else {
        DEBUG_MSG(var->getSubRegAlignment() << "_words SubReg_Align");
    }
    //
    // dump number of registers that are needed
    //
    if (decl->getRegFile() == G4_ADDRESS)
    {
        DEBUG_MSG(" + " << (IS_DTYPE(decl->getElemType()) ? 2 * decl->getNumElems() : decl->getNumElems()) << " regs");
    }
    else
    {
        DEBUG_MSG("\t(" << decl->getNumRows() << "x" << decl->getNumElems() << "):"
            << G4_Type_Table[decl->getElemType()].str);
    }
}

VarBasis::VarBasis(G4_RegVar* v, const Options *options) : var(v), forbidden(NULL), calleeSaveBias(false), callerSaveBias(false), isEOTSrc(false), retIp(false), m_options(options), numForbidden(-1)
{
    regKind = v->getDeclare()->getRegFile();
}
