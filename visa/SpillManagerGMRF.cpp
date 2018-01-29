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

#include "SpillManagerGMRF.h"
#include "Gen4_IR.hpp"
#include "Mem_Manager.h"
#include "FlowGraph.h"
#include "GraphColor.h"
#include "BuildIR.h"
#include "DebugInfo.h"

#include <math.h>
#include <sstream>
#include <fstream>

using namespace std;
using namespace vISA;

// Configurations

#define ADDRESS_SENSITIVE_SPILLS_IMPLEMENTED
//#define DISABLE_SPILL_MEMORY_COMPRESSION
//#define VERIFY_SPILL_ASSIGNMENTS

// Constant declarations

static const unsigned DWORD_BYTE_SIZE						= 4;
static const unsigned OWORD_BYTE_SIZE						= 16;
static const unsigned HWORD_BYTE_SIZE						= 32;
static const unsigned PAYLOAD_INPUT_REG_OFFSET				= 0;
static const unsigned PAYLOAD_INPUT_SUBREG_OFFSET			= 0;
static const unsigned OWORD_PAYLOAD_SPOFFSET_REG_OFFSET		= 0;
static const unsigned OWORD_PAYLOAD_SPOFFSET_SUBREG_OFFSET	= 2;
static const unsigned DWORD_PAYLOAD_SPOFFSET_REG_OFFSET		= 1;
static const unsigned DWORD_PAYLOAD_SPOFFSET_SUBREG_OFFSET	= 0;
static const unsigned OWORD_PAYLOAD_WRITE_REG_OFFSET		= 1;
static const unsigned OWORD_PAYLOAD_WRITE_SUBREG_OFFSET		= 0;
// dword scatter is always in SIMD8 mode
static const unsigned DWORD_PAYLOAD_WRITE_REG_OFFSET		= 2;
static const unsigned DWORD_PAYLOAD_WRITE_SUBREG_OFFSET		= 0;
static const unsigned OWORD_PAYLOAD_HEADER_MIN_HEIGHT		= 1;
static const unsigned DWORD_PAYLOAD_HEADER_MIN_HEIGHT		= 2;
static const unsigned OWORD_PAYLOAD_HEADER_MAX_HEIGHT		= 1;
static const unsigned DWORD_PAYLOAD_HEADER_MAX_HEIGHT		= 3;
static const unsigned REG_DWORD_SIZE						= 8;
static const unsigned REG_BYTE_SIZE							= 32;
static const unsigned SCALAR_EXEC_SIZE						= 1;
static const unsigned DEF_HORIZ_STRIDE						= 1;
static const unsigned REG_ORIGIN							= 0;
static const unsigned SUBREG_ORIGIN							= 0;

static const unsigned SEND_GT_READ_TYPE_BIT_OFFSET			= 13;
static const unsigned SEND_GT_WRITE_TYPE_BIT_OFFSET			= 13;
static const unsigned SEND_GT_DESC_DATA_SIZE_BIT_OFFSET		= 8;
static const unsigned SEND_GT_OW_READ_TYPE					= 0;
static const unsigned SEND_GT_OW_WRITE_TYPE					= 8;
static const unsigned SEND_GT_SC_READ_TYPE					= 6;
static const unsigned SEND_GT_SC_WRITE_TYPE					= 11;
static const unsigned SEND_GT_DP_RD_EX_DESC_IMM             = 5;
static const unsigned SEND_GT_DP_SC_RD_EX_DESC_IMM          = 4;    //scatter reads go to sampler cache
static const unsigned SEND_GT_DP_WR_EX_DESC_IMM             = 5;

static const unsigned SEND_IVB_MSG_TYPE_BIT_OFFSET         = 14;
static const unsigned SEND_IVB_OW_READ_TYPE                = 0;
static const unsigned SEND_IVB_OW_WRITE_TYPE               = 8;
static const unsigned SEND_IVB_SC_READ_TYPE                = 3;
static const unsigned SEND_IVB_SC_WRITE_TYPE               = 11;
static const unsigned SEND_IVB_DP_RD_EX_DESC_IMM           = 10; //data cache
static const unsigned SEND_IVB_DP_WR_EX_DESC_IMM           = 10; //data cache

// Scratch msg
static const unsigned SCRATCH_PAYLOAD_HEADER_MAX_HEIGHT		= 1;
static const unsigned SCRATCH_MSG_DESC_CATEORY				= 18;
static const unsigned SCRATCH_MSG_DESC_OPERATION_MODE		= 17;
static const unsigned SCRATCH_MSG_DESC_CHANNEL_MODE			= 16;
static const unsigned SCRATCH_MSG_INVALIDATE_AFTER_READ		= 15;
static const unsigned SCRATCH_MSG_DESC_BLOCK_SIZE			= 12;

static const uint32_t GRF_ALIGN_MASK = 0xFFFFFFE0;

// Macros

#define LIMIT_SEND_EXEC_SIZE(EXEC_SIZE)(((EXEC_SIZE) > 16)? 16: (EXEC_SIZE))
#define ROUND(x,y)	((x) + ((y - x % y) % y))
#define SPILL_PAYLOAD_HEIGHT_LIMIT 4

extern unsigned int getStackCallRegSize(bool reserveStackCallRegs);

// spill/fill temps are always GRF-aligned, and are also even/odd aligned
// following the original declare's alignment
static void setNewDclAlignment(G4_Declare* newDcl, G4_Align origAlign)
{
    newDcl->setSubRegAlign(Sixteen_Word);
    if (origAlign != Either)
    {
        newDcl->setAlign(origAlign);
    }
}

// Constructor

SpillManagerGMRF::SpillManagerGMRF (
	GlobalRA&                        g,
	unsigned						 spillAreaOffset,
	unsigned                         varIdCount,
	const LivenessAnalysis *         lvInfo,
	LiveRange **                     lrInfo,
	Interference *                   intf,
	std::vector<EDGE> &              prevIntfEdges,
	LR_LIST &                        spilledLRs,
	unsigned                         iterationNo,
    bool                             failSafeSpill,
    unsigned                         spillRegSize,
	unsigned                         indrSpillRegSize,
	bool					         enableSpillSpaceCompression,
    bool                             useScratchMsg
) : builder_ (g.kernel.fg.builder), varIdCount_ (varIdCount), latestImplicitVarIdCount_ (0),
    lvInfo_ (lvInfo), lrInfo_ (lrInfo), prevIntfEdges_ (prevIntfEdges), spilledLRs_ (spilledLRs), 
	nextSpillOffset_ (spillAreaOffset), iterationNo_ (iterationNo), failSafeSpill_ (failSafeSpill), 
	doSpillSpaceCompression(enableSpillSpaceCompression), useScratchMsg_(useScratchMsg), bbId_(UINT_MAX), inSIMDCFContext_(false), mem_(1024),
    spillIntf_(intf), numGRFSpill(0), numGRFFill(0), numGRFMove(0), gra(g)
{
	const unsigned size = sizeof (unsigned) * varIdCount;
	spillRangeCount_ = (unsigned *) allocMem (size);
	memset (spillRangeCount_, 0, size);
	fillRangeCount_ = (unsigned *) allocMem (size);
	memset (fillRangeCount_, 0, size);
	tmpRangeCount_ = (unsigned *) allocMem (size);
	memset (tmpRangeCount_, 0, size);
	msgSpillRangeCount_ = (unsigned *) allocMem (size);
	memset (msgSpillRangeCount_, 0, size);
	msgFillRangeCount_ = (unsigned *) allocMem (size);
	memset (msgFillRangeCount_, 0, size);
	spillAreaOffset_ = spillAreaOffset;
	if (enableSpillSpaceCompression)
    {
	    computeSpillIntf ();
	}
	builder_->instList.clear();
    spillRegStart_ = builder_->getOptions()->getuInt32Option(vISA_TotalGRFNum);
	indrSpillRegStart_ = spillRegStart_;
    spillRegOffset_ = spillRegStart_;
    if(failSafeSpill)
    {
        unsigned int stackCallRegSize = getStackCallRegSize(builder_->kernel.fg.getHasStackCalls() || builder_->kernel.fg.getIsStackCallFunc());
		indrSpillRegStart_ -= (stackCallRegSize + indrSpillRegSize);
		spillRegStart_ = indrSpillRegStart_ - spillRegSize;
    }
    curInst = NULL;

    globalScratchOffset = builder_->getOptions()->getuInt32Option(vISA_SpillMemOffset);
    if (builder_->getIsKernel())
    {
        // reserve space for file scope variables
        globalScratchOffset += (builder_->kernel.fg.fileScopeSaveAreaSize * 16);
    }
    if (canDoSLMSpill())
    {
        if (!builder_->hasBlockedSLMMessage() && !builder_->getBuiltinSLMSpillAddr())
        {
            builder_->initBuiltinSLMSpillAddr(maxSLMScratchSize);
        }
    }
}

// Compute the interference graph for intereference of the memory segments
// occupied by the spilled live ranges.

void
SpillManagerGMRF::computeSpillIntf (
)
{
	// Apply previous interferences that are relevant for this iteration.

	for (auto& edge : prevIntfEdges_)
    {

		if (shouldSpillRegister (getRegVar (edge.first)) ||
			shouldSpillRegister (getRegVar (edge.second))) {
            spillIntf_->checkAndSetIntf(edge.first, edge.second);
		}
	}

    LR_LIST::const_iterator ltEnd = spilledLRs_.end();
    for (LR_LIST::const_iterator lt = spilledLRs_.begin();
        lt != ltEnd; ++lt)
            {
		LiveRange* lr = (*lt);
        unsigned int i = lr->getVar()->getId();

        std::vector<unsigned int>& intfs = spillIntf_->getSparseIntfForVar(i);
        for (auto it : intfs)
        {
            EDGE tempEdge;
            tempEdge.first = it;
            tempEdge.second = i;
            prevIntfEdges_.push_back(tempEdge);
        }
        }
    }

// Get the base regvar for the source or destination region.

template <class REGION_TYPE>
inline G4_RegVar *
SpillManagerGMRF::getRegVar (
	REGION_TYPE * region
) const
{
	G4_RegVar * spilledRegVar = (G4_RegVar *) region->getBase();
	return spilledRegVar;
}

// Get the representative regvar that will be assigned a unique spill
// disp and not a relative spill disp.

inline G4_RegVar *
SpillManagerGMRF::getReprRegVar (
	G4_RegVar * regVar
) const
{
	G4_RegVar * absBase = regVar->getAbsBaseRegVar ();
	if (absBase->isAliased ())
		return getReprRegVar(absBase->getDeclare ()->getAliasDeclare ()->getRegVar ());
	else
		return absBase;
}

// Obtain the register file type of the regvar.

inline G4_RegFileKind
SpillManagerGMRF::getRFType (
	G4_RegVar * regvar
) const
{
	return regvar->getDeclare ()->getRegFile ();
}

// Obtain the register file type of the region.

template <class REGION_TYPE>
inline G4_RegFileKind
SpillManagerGMRF::getRFType (
	REGION_TYPE * region
) const
{
	if (region->getBase ()->isRegVar ())
		return getRFType (region->getBase ()->asRegVar ());
	else if (region->getBase ()->isGreg ())
		return G4_GRF;
	else
        return G4_ADDRESS;
}

// Get the byte offset of the origin of the source or destination region.
// The row offset component is calculated based on the the parameters of
// the corresponding declare directive, while the column offset is calculated
// based on the region parameters.

template <class REGION_TYPE>
inline unsigned
SpillManagerGMRF::getRegionOriginOffset (
	REGION_TYPE * region
) const
{
	unsigned rowOffset = REG_BYTE_SIZE * region->getRegOff ();
	unsigned columnOffset = region->getSubRegOff () * region->getElemSize ();
	return rowOffset + columnOffset;
}

// Check if the destination region is discontiguous or not.
// A destination region is discontiguous if there are portions of the
// region that are not written and unaffected.

bool isDisContRegion (
	G4_DstRegRegion * region,
	unsigned          execSize
)
{
	// If the horizontal stride is greater than 1, then it has gaps.
	// NOTE: Horizontal stride of 0 is not allowed for destination regions.
	return region->getHorzStride() != 1;

}

// Check if the source region is discontiguous or not.
// A source region is discontiguous in there are portions of the region
// that are not read.

bool isDisContRegion (
	G4_SrcRegRegion * region,
	unsigned          execSize
)
{
	RegionDesc * regionDesc = region->getRegion ();

	return regionDesc->isContiguous(execSize);
}

// Get an hexal word mask with the lower 5 bits zeroed.

inline unsigned
SpillManagerGMRF::hwordMask () const
{
	unsigned mask = 0;
	mask = (mask - 1);
	mask = mask << 5;
	return mask;
}

// Get an octal word mask with the lower 4 bits zeroed.

inline unsigned
SpillManagerGMRF::owordMask () const
{
	unsigned mask = 0;
	mask = (mask - 1);
	mask = mask << 4;
	return mask;
}

// Get an dword word mask with the lower 2 bits zeroed.

inline unsigned
SpillManagerGMRF::dwordMask () const
{
	unsigned mask = 0;
	mask = (mask - 1);
	mask = mask << 2;
	return mask;
}

// Test of the offset is oword aligned.

inline bool
SpillManagerGMRF::owordAligned (
	unsigned offset
) const
{
	return (offset & owordMask ()) == offset;
}

// Test of the offset is oword aligned.

inline bool
SpillManagerGMRF::dwordAligned (
	unsigned offset
) const
{
	return (offset & dwordMask ()) == offset;
}

// Get the ceil of the division.

inline unsigned
SpillManagerGMRF::cdiv (
	unsigned dvd,
	unsigned dvr
) const
{
	return (dvd / dvr) + ((dvd % dvr)? 1: 0);
}

// Get the live range corresponding to id.

inline bool
SpillManagerGMRF::shouldSpillRegister (
	G4_RegVar * regVar
) const
{

    if (getRFType (regVar) == G4_ADDRESS)
    {
        return false;
    }
    G4_RegVar * actualRegVar =
        (regVar->getDeclare ()->getAliasDeclare ())?
        regVar->getDeclare ()->getAliasDeclare ()->getRegVar ():
        regVar;
    if (actualRegVar->getId () == UNDEFINED_VAL)
        return false;
    else if (regVar->isRegVarTransient () || regVar->isRegVarTmp ())
        return false;
#ifndef ADDRESS_SENSITIVE_SPILLS_IMPLEMENTED
    else if	(lvInfo_->isAddressSensitive (regVar->getId ()))
        return false;
#endif

    else if (builder_->kernel.fg.isPseudoVCADcl(actualRegVar->getDeclare()) || 
        builder_->kernel.fg.isPseudoVCEDcl(actualRegVar->getDeclare()))
        return false;
    else
        return lrInfo_ [actualRegVar->getId ()]->getPhyReg () == NULL;
}

// Get the regvar with the id.

inline G4_RegVar *
SpillManagerGMRF::getRegVar (
	unsigned id
) const
{
	return (lvInfo_->vars)[id];
}

// Get the byte size of the live range.

inline unsigned
SpillManagerGMRF::getByteSize (
	G4_RegVar * regVar
) const
{
	unsigned normalizedRowSize =
		(regVar->getDeclare ()->getNumRows () > 1)?
		REG_BYTE_SIZE:
		regVar->getDeclare ()->getNumElems () *
		regVar->getDeclare ()->getElemSize ();
	return
		normalizedRowSize * regVar->getDeclare ()->getNumRows ();
}

// Check if the lifetime of the spill/fill memory of live range i interferes
// with the lifetime of the spill/fill memory of live range j

bool
SpillManagerGMRF::spillMemLifetimeInterfere (
	unsigned i,
	unsigned j
) const
{
	G4_RegVar * ireg = getRegVar (i);
	G4_RegVar * jreg = getRegVar (j);
	G4_RegVar * irep = getReprRegVar (ireg);
	G4_RegVar * jrep = getReprRegVar (jreg);
	G4_RegVar * inont = ireg->getNonTransientBaseRegVar ();
	G4_RegVar * jnont = jreg->getNonTransientBaseRegVar ();

	if (ireg->isRegVarTmp ()) {
		return
			ireg->getBaseRegVar () == jrep ||
			spillMemLifetimeInterfere (ireg->getBaseRegVar ()->getId (), j);
	}

	else if (jreg->isRegVarTmp ()) {
		return
			jreg->getBaseRegVar () == irep ||
			spillMemLifetimeInterfere (jreg->getBaseRegVar ()->getId (), i);
	}

	else if (inont->isRegVarTmp ()) {
		return
			inont->getBaseRegVar () == jrep ||
			spillMemLifetimeInterfere (inont->getBaseRegVar ()->getId (), j);

	}

	else if (jnont->isRegVarTmp ()) {
		return
			jnont->getBaseRegVar () == irep ||
			spillMemLifetimeInterfere (jnont->getBaseRegVar ()->getId (), i);
	}

	else {
		if (spillIntf_->interfereBetween (irep->getId (), jrep->getId ()))
			return true;
		else if (getRFType (irep) != getRFType (jrep))
			return true;
		else
#ifdef DISABLE_SPILL_MEMORY_COMPRESSION
			return irep != jrep;
#else
			return false;
#endif
	}
}

// Calculate the spill memory displacement for the regvar.

unsigned
SpillManagerGMRF::calculateSpillDisp (
	G4_RegVar *   regVar
) const
{
	assert (regVar->getDisp () == UINT_MAX);

	// Locate the blocked locations calculated from the interfering
	// spilled live ranges and put them into a list in ascending order.

	typedef std::list < G4_RegVar * > LocList;
	LocList locList;
	unsigned lrId =
		(regVar->getId () >= varIdCount_)?
		regVar->getBaseRegVar ()->getId (): regVar->getId ();
	assert (lrId < varIdCount_);

	for (unsigned i = 0; i < varIdCount_; i++) {

		if (spillMemLifetimeInterfere (lrId, i)) {
			G4_RegVar * intfRegVar = getRegVar (i);
			assert (getRegVar (i)->isAliased () == false);
			if (intfRegVar->isRegVarTransient ()) continue;
			unsigned iDisp = intfRegVar->getDisp ();
			if (iDisp == UINT_MAX) continue;
			LocList::iterator loc;
			for (loc = locList.begin ();
				 loc != locList.end () && (*loc)->getDisp () < iDisp;
				 ++loc);
			if (loc != locList.end ())
				locList.insert (loc, intfRegVar);
			else
				locList.push_back (intfRegVar);
		}
	}

	// Find a spill slot for lRange within the locList.
	// we always start searching from 0 to facilitate cross-iteration reuse
    unsigned regVarLocDisp = 0;
	unsigned regVarSize = getByteSize (regVar);

	for (LocList::iterator curLoc = locList.begin (); curLoc != locList.end ();
		++curLoc) {
		unsigned curLocDisp = (*curLoc)->getDisp ();
		if (regVarLocDisp < curLocDisp &&
			regVarLocDisp + regVarSize <= curLocDisp)
			break;
		unsigned curLocEnd = curLocDisp + getByteSize (*curLoc);
        {
			if (useScratchMsg_)
			{
				if(curLocEnd % G4_GRF_REG_NBYTES != 0)
					curLocEnd = (curLocEnd&(owordMask()<<1)) + G4_GRF_REG_NBYTES;
			}
			else
			{
				if (owordAligned (curLocEnd) == false) {
					curLocEnd =
						(curLocEnd & owordMask ()) + OWORD_BYTE_SIZE;
				}
			}
		}

		regVarLocDisp = (regVarLocDisp > curLocEnd)? regVarLocDisp: curLocEnd;
	}

	return regVarLocDisp;
}

// Get the spill/fill displacement of the segment containing the region.
// A segment is the smallest dword or oword aligned portion of memory
// containing the destination or source operand that can be read or saved.

template <class REGION_TYPE>
inline unsigned
SpillManagerGMRF::getSegmentDisp (
	REGION_TYPE * region,
	unsigned      execSize
)
{
	assert (region->getElemSize () && execSize);
	if (isUnalignedRegion (region, execSize))
		return getEncAlignedSegmentDisp (region, execSize);
	else
		return getRegionDisp (region);
}

// Get the spill/fill displacement of the regvar.

unsigned
SpillManagerGMRF::getDisp(
G4_RegVar * regVar
)
{
    // Already calculated spill memory disp

    if (regVar->getDisp() != UINT_MAX) {
        // Do nothing.
    }

    // If it is an aliased regvar then calculate the disp for the
    // actual regvar and then calculate the disp of the aliased regvar
    // based on it.

    else if (regVar->isAliased()) {
        G4_Declare * regVarDcl = regVar->getDeclare();
        return
            getDisp(regVarDcl->getAliasDeclare()->getRegVar()) +
            regVarDcl->getAliasOffset();
    }

    // If its base regvar has been assigned a disp, then the spill memory
    // has already been allocated for it, simply calculate the disp based
    // on the enclosing segment disp.

    else if (regVar->isRegVarTransient() &&
        getDisp(regVar->getBaseRegVar()) != UINT_MAX) {
        assert(regVar->getBaseRegVar() != regVar);
        unsigned itsDisp;

        if (regVar->isRegVarSpill()) {
            G4_RegVarTransient * tRegVar = static_cast <G4_RegVarTransient*> (regVar);
            assert(
                getSegmentByteSize(
                tRegVar->getDstRepRegion(), tRegVar->getExecSize()) <=
                getByteSize(tRegVar));
            itsDisp =
                getSegmentDisp(
                tRegVar->getDstRepRegion(), tRegVar->getExecSize());
        }

        else if (regVar->isRegVarFill()) {
            G4_RegVarTransient * tRegVar = static_cast <G4_RegVarTransient*> (regVar);
            assert(
                getSegmentByteSize(
                tRegVar->getSrcRepRegion(), tRegVar->getExecSize()) <=
                getByteSize(tRegVar));
            itsDisp =
                getSegmentDisp(
                tRegVar->getSrcRepRegion(), tRegVar->getExecSize());
        }

        else {
            MUST_BE_TRUE(false, "Incorrect spill/fill ranges.");
            itsDisp = 0;
        }

        regVar->setDisp(itsDisp);
    }

    // Allocate the spill and evaluate its disp

    else {
		if (doSpillSpaceCompression)
        {
            assert(regVar->isRegVarTransient() == false);
            regVar->setDisp(calculateSpillDisp(regVar));
        }
        else
        {
            assert(regVar->isRegVarTransient() == false);
            if (regVar->getId() >= varIdCount_)
            {
                if (regVar->getBaseRegVar()->getDisp() != UINT_MAX)
                {
                    regVar->setDisp(regVar->getBaseRegVar()->getDisp());
                    return regVar->getDisp();
                }
            }

            if ((spillAreaOffset_) % G4_GRF_REG_NBYTES != 0)
            {
                (spillAreaOffset_) = ((spillAreaOffset_)&(owordMask() << 1)) + G4_GRF_REG_NBYTES;
            }

            if (canDoSLMSpill())
            {
                // don't have variables that cross the SLM/scratch boundary, makes our life a bit easier
                // FIXME: may want to consider spill costs and put the important variables in SLM
                if (spillAreaOffset_ < maxSLMScratchSize &&
                    spillAreaOffset_ + getByteSize(regVar) > maxSLMScratchSize)
                {
                    spillAreaOffset_ = maxSLMScratchSize;
                }
            }

            regVar->setDisp(spillAreaOffset_);
            spillAreaOffset_ += getByteSize(regVar);
        }
    }

    return regVar->getDisp();
}

// Get the spill/fill displacement of the region.

template <class REGION_TYPE>
inline unsigned
SpillManagerGMRF::getRegionDisp (
	REGION_TYPE * region
)
{
	return getDisp (getRegVar (region)) +  getRegionOriginOffset (region);
}

// Get the type of send message to use to spill/fill the region.
// The type can be either on oword read/write or a scatter read/write.
// If the segment corresponding to the region is dword sized then a
// dword read/write is used else an oword read/write is used.

template <class REGION_TYPE>
inline unsigned
SpillManagerGMRF::getMsgType (
	REGION_TYPE * region,
	unsigned      execSize
)
{
	unsigned regionDisp = getRegionDisp (region);
	unsigned regionByteSize = getRegionByteSize (region, execSize);
    if (owordAligned (regionDisp) && owordAligned (regionByteSize))
		return owordMask ();
	else
		return getEncAlignedSegmentMsgType (region, execSize);
}

// Determine if the region is unaligned w.r.t spill/fill memory read/writes.
// If the exact region cannot be read/written from spill/fill memory using
// one send instruction, then it is unaligned.

template <class REGION_TYPE>
inline bool
SpillManagerGMRF::isUnalignedRegion (
	REGION_TYPE * region,
	unsigned      execSize
)
{
	unsigned regionDisp = getRegionDisp (region);
	unsigned regionByteSize = getRegionByteSize (region, execSize);

	if( useScratchMsg_)
	{
		if( regionDisp%G4_GRF_REG_NBYTES == 0 && regionByteSize%G4_GRF_REG_NBYTES == 0 )
			return
				regionByteSize / G4_GRF_REG_NBYTES != 1 &&
				regionByteSize / G4_GRF_REG_NBYTES != 2 &&
				regionByteSize / G4_GRF_REG_NBYTES != 4;
		else
			return true;
	}
	else
	{
		if (owordAligned (regionDisp) && owordAligned (regionByteSize))
			return
				regionByteSize / OWORD_BYTE_SIZE != 1 &&
				regionByteSize / OWORD_BYTE_SIZE != 2 &&
				regionByteSize / OWORD_BYTE_SIZE != 4;
		else
			return true;

	}
}

// Calculate the smallest aligned segment encompassing the region.

template <class REGION_TYPE>
void
SpillManagerGMRF::calculateEncAlignedSegment (
	REGION_TYPE * region,
	unsigned      execSize,
	unsigned &    start,
	unsigned &    end,
	unsigned &    type
)
{
	unsigned regionDisp = getRegionDisp (region);
	unsigned regionByteSize = getRegionByteSize (region, execSize);

	if( useScratchMsg_ )
    {
		unsigned hwordLB = regionDisp & hwordMask ();
		unsigned hwordRB = hwordLB + HWORD_BYTE_SIZE;
		unsigned blockSize = HWORD_BYTE_SIZE;

		while (regionDisp + regionByteSize > hwordRB) {
			hwordRB += blockSize;
		}

		assert ((hwordRB - hwordLB)/ REG_BYTE_SIZE <= 4);
		start = hwordLB;
		end = hwordRB;
		type = hwordMask ();
    }
    else
    {
		unsigned owordLB = regionDisp & owordMask ();
		unsigned owordRB = owordLB + OWORD_BYTE_SIZE;
		unsigned blockSize = OWORD_BYTE_SIZE;

		while (regionDisp + regionByteSize > owordRB) {
			owordRB += blockSize;
			blockSize *= 2;
		}

		assert ((owordRB - owordLB)/ REG_BYTE_SIZE <= 4);
		start = owordLB;
		end = owordRB;
		type = owordMask ();
	}
}

// Get the byte size of the aligned segment for the region.

template <class REGION_TYPE>
inline unsigned
SpillManagerGMRF::getEncAlignedSegmentByteSize (
	REGION_TYPE * region,
	unsigned      execSize
)
{
	unsigned start, end, type;
	calculateEncAlignedSegment (region, execSize, start, end, type);
	return end - start;
}

// Get the start offset of the aligned segment for the region.

template <class REGION_TYPE>
inline unsigned
SpillManagerGMRF::getEncAlignedSegmentDisp (
	REGION_TYPE * region,
	unsigned      execSize
)
{
	unsigned start, end, type;
	calculateEncAlignedSegment (region, execSize, start, end, type);
	return start;
}

// Get the type of message to be used to read/write the enclosing aligned
// segment for the region.

template <class REGION_TYPE>
inline unsigned
SpillManagerGMRF::getEncAlignedSegmentMsgType (
	REGION_TYPE * region,
	unsigned      execSize
)
{
	unsigned start, end, type;
	calculateEncAlignedSegment (region, execSize, start, end, type);
	return type;
}

// Get the byte size of the segment for the region.

template <class REGION_TYPE>
inline unsigned
SpillManagerGMRF::getSegmentByteSize (
	REGION_TYPE * region,
	unsigned      execSize
)
{
	assert (region->getElemSize () && execSize);
	if (isUnalignedRegion (region, execSize))
		return getEncAlignedSegmentByteSize (region, execSize);
	else
		return getRegionByteSize (region, execSize);
}

// Get the byte size of the destination region.

inline unsigned
SpillManagerGMRF::getRegionByteSize (
	G4_DstRegRegion * region,
	unsigned          execSize
) const
{
	unsigned size = region->getHorzStride() * region->getElemSize() *
        (execSize - 1) + region->getElemSize();

	return size;
}

// Get the byte size of the source region.

inline unsigned
SpillManagerGMRF::getRegionByteSize (
	G4_SrcRegRegion * region,
	unsigned          execSize
) const
{
	assert (execSize % region->getRegion ()->width == 0);
	unsigned nRows = execSize / region->getRegion ()->width;
	unsigned size = 0;

	for (unsigned int i = 0; i < nRows - 1; i++) {
		size += region->getRegion ()->vertStride * region->getElemSize ();
	}

	size +=
		region->getRegion ()->horzStride * region->getElemSize () *
		(region->getRegion ()->width - 1) + region->getElemSize ();
	return size;
}

// Get the max exec size on a 256 bit vector for the input operand.

inline unsigned
SpillManagerGMRF::getMaxExecSize (
	G4_Operand * operand
) const
{
	const unsigned size = Type_UNDEF + 1;
	static unsigned maxExecSize [size] = {8, 8, 16, 16, 16, 16, 8, 8, 0};
	return maxExecSize [operand->getType ()];
}

// Check if the instruction is a SIMD 16 or 32 instruction that is logically
// equivalent to two instructions the second of which uses register operands
// at the following row with the same sub-register index.

inline bool
SpillManagerGMRF::isComprInst (
	G4_INST * inst
) const
{
	return inst->isComprInst ();
}

// Check if the source in a compressed instruction operand occupies a second
// register.

bool
SpillManagerGMRF::isMultiRegComprSource (
    G4_SrcRegRegion* src,
	G4_INST *        inst
) const
{
    if (inst->isComprInst () == false) {
        return false;
    }

    else if (isScalarReplication(src)) {
        return false;
    }

    else if (inst->getExecSize() <= 8) {
        return false;
    }
    else if (!src->asSrcRegRegion()->crossGRF())
    {
        return false;
    }

    else if (inst->getExecSize () == 16 &&
             inst->getDst () &&
             G4_Type_Table[inst->getDst ()->getType ()].byteSize == 4 &&
             inst->getDst()->getHorzStride () == 1 ) {

		if (G4_Type_Table[src->getType()].byteSize == 2 &&
			src->isNativePackedRegion()) {
            return false;
        }

        else {
            return true;
        }
    }

    else {
        return true;
    }
}

// Send message information query

inline unsigned
SpillManagerGMRF::getSendRspLengthBitOffset () const
{
    return SEND_GT_RSP_LENGTH_BIT_OFFSET;
}

// Send message information query

inline unsigned
SpillManagerGMRF::getSendMaxResponseLength () const
{
    //return SEND_GT_MAX_RESPONSE_LENGTH;
	return 8;
}

// Send message information query

inline unsigned
SpillManagerGMRF::getSendMsgLengthBitOffset () const
{
    return SEND_GT_MSG_LENGTH_BIT_OFFSET;
}

// Send message information query

inline unsigned
SpillManagerGMRF::getSendMaxMessageLength () const
{
    return SEND_GT_MAX_MESSAGE_LENGTH;
}

// Send message information query

inline unsigned
SpillManagerGMRF::getSendDescDataSizeBitOffset () const
{
    return SEND_GT_DESC_DATA_SIZE_BIT_OFFSET;
}

// Send message information query

inline unsigned
SpillManagerGMRF::getSendReadTypeBitOffset () const
{
    return SEND_IVB_MSG_TYPE_BIT_OFFSET;
}

// Send message information query

inline unsigned
SpillManagerGMRF::getSendWriteTypeBitOffset () const
{
    return SEND_IVB_MSG_TYPE_BIT_OFFSET;
}

// Send message information query

inline unsigned
SpillManagerGMRF::getSendScReadType () const
{
    return SEND_IVB_SC_READ_TYPE;
}

// Send message information query

inline unsigned
SpillManagerGMRF::getSendScWriteType () const
{
    return SEND_IVB_SC_WRITE_TYPE;
}

// Send message information query

inline unsigned
SpillManagerGMRF::getSendOwordReadType () const
{
    return SEND_IVB_OW_READ_TYPE;
}

// Send message information query

inline unsigned
SpillManagerGMRF::getSendOwordWriteType () const
{
    return SEND_IVB_OW_WRITE_TYPE;
}

inline unsigned
SpillManagerGMRF::getSendExDesc( bool isWrite, bool isScatter ) const
{
    return isWrite ? SEND_IVB_DP_WR_EX_DESC_IMM : SEND_IVB_DP_RD_EX_DESC_IMM;
}

// Custom memory allocator

inline void *
SpillManagerGMRF::allocMem (
	unsigned size
) const
{
	return builder_->mem.alloc (size);
}

bool SpillManagerGMRF::useSplitSend() const
{
	return builder_->useSends();
}

// Get a unique spill range index for regvar.

inline unsigned
SpillManagerGMRF::getSpillIndex (
	G4_RegVar *  spilledRegVar
)
{
	return spillRangeCount_ [spilledRegVar->getId ()]++;
}

// Get a unique fill range index for regvar.

inline unsigned
SpillManagerGMRF::getFillIndex (
	G4_RegVar *  spilledRegVar
)
{
	return fillRangeCount_ [spilledRegVar->getId ()]++;
}

// Get a unique tmp index for spilled regvar.

inline unsigned
SpillManagerGMRF::getTmpIndex (
	G4_RegVar *  spilledRegVar
)
{
	return tmpRangeCount_ [spilledRegVar->getId ()]++;
}

// Get a unique msg index for spilled regvar.

inline unsigned
SpillManagerGMRF::getMsgSpillIndex (
	G4_RegVar *  spilledRegVar
)
{
	return msgSpillRangeCount_ [spilledRegVar->getId ()]++;
}

// Get a unique msg index for filled regvar.

inline unsigned
SpillManagerGMRF::getMsgFillIndex (
	G4_RegVar *  spilledRegVar
)
{
	return msgFillRangeCount_ [spilledRegVar->getId ()]++;
}

// Create a unique name for a regvar representing a spill/fill/msg live range.

inline const char *
SpillManagerGMRF::createImplicitRangeName (
	const char * baseName,
	G4_RegVar *  spilledRegVar,
	unsigned     index
)
{
	stringstream nameStrm;
	nameStrm << baseName << "_" << spilledRegVar->getName ()
			 << "_" << index << ends;
    int nameLen = unsigned(nameStrm.str().length()) + 1;
	char * name = (char *) allocMem (nameLen);
	strcpy_s(name, nameLen, nameStrm.str().c_str ());
	return name;
}

// Check if the region is a scalar replication region.

inline bool
SpillManagerGMRF::isScalarReplication (
	G4_SrcRegRegion * region
) const
{
	return region->isScalar();
}

// Check if we have to repeat the simd16 source in the simd8 equivalents.
// The BPSEC mentions that if a replicated scalar appears in an simd16
// instruction, logically we need to repeat the source region used in
// the first simd8 instruction in the second simd8 instruction as well
// (i.e. the reg no is not incremented by one for the second).

inline bool
SpillManagerGMRF::repeatSIMD16or32Source (
	G4_SrcRegRegion * region
) const
{
	return isScalarReplication (region);
}

// Create a declare directive for a new live range (spill/fill/msg)
// introduced as part of the spill code generation.

G4_Declare *
SpillManagerGMRF::createRangeDeclare (
	const char*    name,
	G4_RegFileKind regFile,
    unsigned short nElems,
	unsigned short nRows,
	G4_Type        type,
	RegionDesc *   srcRgn,
	unsigned short dstRgn,
	DeclareType    kind,
	G4_RegVar *    base,
	G4_Operand *   repRegion,
	unsigned       execSize
)
{
	G4_Declare * rangeDeclare =
		builder_->createDeclareNoLookup (
			name, regFile, nElems, nRows, type, kind,
			base, repRegion, execSize);
	rangeDeclare->getRegVar ()->setId (
		varIdCount_ + latestImplicitVarIdCount_++);
	gra.setBBId(rangeDeclare, bbId_);
	return rangeDeclare;
}

// Create a GRF regvar and its declare directive to represent the spill/fill
// live range.
// The size of the regvar is calculated from the size of the spill/fill
// region. If the spill/fill region fits into one row, then width of the
// regvar is exactly as needed for the spill/fill segment, else it is
// made to occupy exactly two full rows. In either case the regvar is made
// to have 16 word alignment requirement. This is to satisfy the requirements
// of the send instruction used to save/load the value from memory. For
// region's in simd16 instruction contexts we multiply the height by 2
// except for source region's with scalar replication.

template <class REGION_TYPE>
G4_Declare *
SpillManagerGMRF::createTransientGRFRangeDeclare (
	REGION_TYPE * region,
	const char  * baseName,
	unsigned      index,
	unsigned      execSize,
    G4_INST     * inst
)
{
	const char * name =
		createImplicitRangeName (baseName, getRegVar (region), index);
	G4_Type type = region->getType ();
	unsigned segmentByteSize = getSegmentByteSize (region, execSize);
	DeclareType regVarKind =
        (region->isDstRegRegion ())? DeclareType::Spill : DeclareType::Fill;
	unsigned short width, height;

	if (segmentByteSize > REG_BYTE_SIZE || region->crossGRF()) {
		assert (REG_BYTE_SIZE % region->getElemSize () == 0);
		width = REG_BYTE_SIZE / region->getElemSize ();
		assert (segmentByteSize / REG_BYTE_SIZE <= 2);
		height = 2;
	}

	else {
		assert (segmentByteSize % region->getElemSize () == 0);
		width = segmentByteSize / region->getElemSize ();
		height = 1;
	}

	if( useScratchMsg_ )
	{
		// Read/write size when using scratch msg descriptor is 32-bytes
		if( height == 1 && width < REG_BYTE_SIZE )
			width = REG_BYTE_SIZE/region->getElemSize();
	}

	G4_Declare * transientRangeDeclare =
		createRangeDeclare(
			name, G4_GRF, width, height, type, NULL, DEF_HORIZ_STRIDE,
			regVarKind, region->getBase ()->asRegVar (), region, execSize);

    if( failSafeSpill_ )
    {
        transientRangeDeclare->getRegVar()->setPhyReg(builder_->phyregpool.getGreg(spillRegOffset_), 0);
        spillRegOffset_ += height;
    }

    // FIXME: We should take the original declare's alignment too, but I'm worried 
    // we may get perf regression if FE is over-aligning or the alignment is not necessary for this inst. 
    // So Either is used for now and we can change it later if there are bugs
    setNewDclAlignment(transientRangeDeclare, Either);
	return transientRangeDeclare;
}

// Create a regvar and its declare directive to represent the spill live
// range that appears as a send instruction post destination GRF.
// The type of the regvar is set as dword and its width 8. The type of
// the post destination does not matter, so we just use type dword, and
// a width of 8 so that a row corresponds to a physical register.

G4_Declare *
SpillManagerGMRF::createPostDstSpillRangeDeclare (
	G4_INST *         sendOut,
	G4_DstRegRegion * spilledRegion
)
{
	G4_RegVar * spilledRegVar = getRegVar (spilledRegion);
	const char * name =
		createImplicitRangeName (
			"SP_GRF", spilledRegVar, getSpillIndex (spilledRegVar));
	unsigned short nRows;

    G4_SendMsgDescriptor* msgDesc = sendOut->getMsgDesc();
    if( msgDesc ) {
        nRows = msgDesc->ResponseLength();
    }

	// Otherwise assume all following grfs (limited to 8) in the virtual
	// register

	else {
		nRows =
			spilledRegVar->getDeclare ()->getNumRows () -
			spilledRegion->getRegOff ();

		if (nRows > getSendMaxResponseLength ()) {
			nRows = (unsigned short) getSendMaxResponseLength ();
		}
	}

    G4_DstRegRegion * normalizedPostDst = builder_->createDstRegRegion(
		Direct, spilledRegVar, spilledRegion->getRegOff (), SUBREG_ORIGIN,
		DEF_HORIZ_STRIDE, Type_UD);

	// We use the width as the user specified, the height however is
	// calculated based on the message descriptor to limit register
	// pressure induced by the spill range.

	G4_Declare * transientRangeDeclare =
		createRangeDeclare (
			name, G4_GRF, REG_DWORD_SIZE, nRows, Type_UD, NULL, 0,
            DeclareType::Spill, spilledRegVar, normalizedPostDst, REG_DWORD_SIZE);

    if( failSafeSpill_ )
    {
        if( useSplitSend() )
        {
            transientRangeDeclare->getRegVar()->setPhyReg(builder_->phyregpool.getGreg(spillRegStart_), 0);
            spillRegOffset_ += nRows;
        }
        else
        {
            transientRangeDeclare->getRegVar()->setPhyReg(builder_->phyregpool.getGreg(spillRegStart_+1), 0);
            spillRegOffset_ += nRows + 1;
        }
    }

	return transientRangeDeclare;
}

// Create a regvar and its declare directive to represent the spill live range.

inline G4_Declare *
SpillManagerGMRF::createSpillRangeDeclare (
    G4_DstRegRegion * spilledRegion,
    unsigned          execSize,
    G4_INST         * inst
)
{
	return
		createTransientGRFRangeDeclare (
			spilledRegion, "SP_GRF",
			getSpillIndex (getRegVar (spilledRegion)),
			execSize, inst);
}

// Create a regvar and its declare directive to represent the GRF fill live
// range.

inline G4_Declare *
SpillManagerGMRF::createGRFFillRangeDeclare (
	G4_SrcRegRegion * fillRegion,
	unsigned          execSize,
    G4_INST         * inst
)
{
	assert (getRFType (fillRegion) == G4_GRF);
	G4_Declare * fillRangeDecl =
		createTransientGRFRangeDeclare (
			fillRegion, "FL_GRF", getFillIndex (getRegVar (fillRegion)),
			execSize, inst);
	return fillRangeDecl;
}

// Create a regvar and its declare directive to represent the MRF fill live
// range.

inline G4_Declare *
SpillManagerGMRF::createMRFFillRangeDeclare (
	G4_SrcRegRegion * filledRegion,
	G4_INST *         sendInst
)
{
    MUST_BE_TRUE ((sendInst->isSend() && (sendInst->getSrc(0)->asSrcRegRegion () == filledRegion)) ||
                  (sendInst->isSplitSend() && (sendInst->getSrc(1)->asSrcRegRegion () == filledRegion)),
                  "Error in createMRFFillRangeDeclare");

	G4_RegVar * filledRegVar = getRegVar (filledRegion);
	const char * name =
		createImplicitRangeName (
			"FL_MRF", filledRegVar, getFillIndex (filledRegVar));
	unsigned short nRows = 0;

    G4_SendMsgDescriptor* msgDesc = sendInst->getMsgDesc();
    if( msgDesc )
    {
        if (sendInst->isSplitSend() &&
            (sendInst->getSrc(1)->asSrcRegRegion () == filledRegion))
        {
            nRows = msgDesc->extMessageLength();
        }
        else
        {
            nRows = msgDesc->MessageLength();
        }
    }
	else
    {
		nRows =
			filledRegVar->getDeclare ()->getNumRows () -
			filledRegion->getRegOff ();

		if (nRows > getSendMaxMessageLength ()) {
			nRows = (unsigned short) getSendMaxMessageLength ();
		}
	}

    G4_SrcRegRegion * normalizedMRFSrc =
        builder_->createSrcRegRegion(
        filledRegion->getModifier(), Direct, filledRegVar,
        filledRegion->getRegOff(), SUBREG_ORIGIN, filledRegion->getRegion(),
        filledRegion->getType());
	unsigned short width = REG_BYTE_SIZE / filledRegion->getElemSize ();
	assert (REG_BYTE_SIZE % filledRegion->getElemSize () == 0);
	//assert (width == 32 || width == 16 || width == 8);

	// We use the width as the user specified, the height however is
	// calculated based on the message descriptor to limit register
	// pressure induced by the spill range.

	G4_Declare * transientRangeDeclare =
		createRangeDeclare(
		name,
		G4_GRF,
		width, nRows, filledRegion->getType(), NULL, 0,
		DeclareType::Fill, filledRegVar, normalizedMRFSrc,
		width);

    setNewDclAlignment(transientRangeDeclare, filledRegVar->getAlignment());

    if( failSafeSpill_ )
    {
        if (sendInst->isEOT() && builder_->hasEOTGRFBinding())
        {
            // make sure eot src is in last 16 GRF
            uint32_t eotStart = builder_->getOptions()->getuInt32Option(vISA_TotalGRFNum) - 16;
            if (spillRegOffset_ < eotStart)
            {
                spillRegOffset_ = eotStart;
            }
        }
        transientRangeDeclare->getRegVar()->setPhyReg(builder_->phyregpool.getGreg(spillRegOffset_), 0);
        spillRegOffset_ += nRows;
    }

	return transientRangeDeclare;
}

// Create a regvar and its declare directive to represent the temporary live
// range.

G4_Declare *
SpillManagerGMRF::createTemporaryRangeDeclare (
	G4_DstRegRegion * spilledRegion,
	unsigned          execSize,
	bool              forceSegmentAlignment
)
{
	const char * name =
		createImplicitRangeName (
			"TM_GRF", getRegVar (spilledRegion),
			getTmpIndex (getRegVar (spilledRegion)));
	unsigned byteSize =
		(forceSegmentAlignment)?
		getSegmentByteSize (spilledRegion, execSize):
		getRegionByteSize (spilledRegion, execSize);

	assert (byteSize <= 2 * REG_BYTE_SIZE);
	assert (byteSize % spilledRegion->getElemSize () == 0);

	G4_Type type = spilledRegion->getType ();
    DeclareType regVarKind = DeclareType::Tmp;

	unsigned short width, height;
	if( byteSize > REG_BYTE_SIZE )
	{
		height = 2;
		width = REG_BYTE_SIZE/spilledRegion->getElemSize();
	}
	else
	{
		height = 1;
		width = byteSize/spilledRegion->getElemSize();
	}

    G4_RegVar* spilledRegVar = getRegVar(spilledRegion);

	G4_Declare * temporaryRangeDeclare =
		createRangeDeclare(
			name, G4_GRF, width, height, type, NULL, DEF_HORIZ_STRIDE,
			regVarKind, spilledRegVar, NULL, 0);

    if( failSafeSpill_ )
    {
        temporaryRangeDeclare->getRegVar()->setPhyReg(builder_->phyregpool.getGreg(spillRegOffset_), 0);
        spillRegOffset_ += height;
    }

    setNewDclAlignment(temporaryRangeDeclare, Either);
	return temporaryRangeDeclare;
}

// Create a destination region that could be used in place of the spill regvar.
// If the region is unaligned then the origin of the destination region
// is the displacement of the orginal region from its segment, else the
// origin is 0.

G4_DstRegRegion *
SpillManagerGMRF::createSpillRangeDstRegion (
	G4_RegVar *       spillRangeRegVar,
	G4_DstRegRegion * spilledRegion,
	unsigned          execSize,
	unsigned          regOff
)
{
	if (isUnalignedRegion (spilledRegion, execSize)) {
		unsigned segmentDisp =
			getEncAlignedSegmentDisp (spilledRegion, execSize);
		unsigned regionDisp = getRegionDisp (spilledRegion);
		assert (regionDisp >= segmentDisp);
		unsigned short subRegOff =
			(regionDisp - segmentDisp) / spilledRegion->getElemSize ();
		assert (
			(regionDisp - segmentDisp) % spilledRegion->getElemSize () == 0);
		assert (subRegOff * spilledRegion->getElemSize () +
				getRegionByteSize (spilledRegion, execSize) <=
				2 * REG_BYTE_SIZE);

		if(useScratchMsg_ )
		{
			G4_Declare* parent_dcl = spilledRegion->getBase()->asRegVar()->getDeclare();
			unsigned off = 0;
			while( parent_dcl->getAliasDeclare() != NULL )
			{
				// off is in bytes
				off += parent_dcl->getAliasOffset();
				parent_dcl = parent_dcl->getAliasDeclare();
			}
			off = off%G4_GRF_REG_NBYTES;
			// sub-regoff is in units of element size
			subRegOff = spilledRegion->getSubRegOff() + off/spilledRegion->getElemSize();
		}

        return builder_->createDstRegRegion(
			Direct, spillRangeRegVar, (unsigned short) regOff, subRegOff,
			spilledRegion->getHorzStride (), spilledRegion->getType ());
	}

	else {
        return builder_->createDstRegRegion(
			Direct, spillRangeRegVar, (short) regOff, SUBREG_ORIGIN,
			spilledRegion->getHorzStride (), spilledRegion->getType ());
	}
}

// Create a source region that could be used to copy out the temporary range
// (that was created to replace the portion of the spilled live range appearing
// in an instruction destination) into the segment aligned spill range for the
// spilled live range that can be written out to spill memory.

G4_SrcRegRegion *
SpillManagerGMRF::createTemporaryRangeSrcRegion (
	G4_RegVar *       tmpRangeRegVar,
	G4_DstRegRegion * spilledRegion,
	uint16_t          execSize,
	unsigned          regOff
)
{
    uint16_t horzStride = spilledRegion->getHorzStride();
    // A scalar region is returned when execsize is 1.
    RegionDesc *rDesc = builder_->createRegionDesc(execSize, horzStride, 1, 0);

    return builder_->createSrcRegRegion(
		Mod_src_undef, Direct, tmpRangeRegVar, (short) regOff, SUBREG_ORIGIN,
		rDesc, spilledRegion->getType () );
}

// Create a source region that could be used in place of the fill regvar.
// If the region is unaligned then the origin of the destination region
// is the displacement of the orginal region from its segment, else the
// origin is 0.

G4_SrcRegRegion *
SpillManagerGMRF::createFillRangeSrcRegion (
	G4_RegVar *       fillRangeRegVar,
	G4_SrcRegRegion * filledRegion,
	unsigned          execSize
)
{
    // we need to preserve accRegSel if it's set
	if (isUnalignedRegion (filledRegion, execSize)) {
		unsigned segmentDisp =
			getEncAlignedSegmentDisp (filledRegion, execSize);
		unsigned regionDisp = getRegionDisp (filledRegion);
		assert (regionDisp >= segmentDisp);
		unsigned short subRegOff =
			(regionDisp - segmentDisp) / filledRegion->getElemSize ();
		assert (
			(regionDisp - segmentDisp) % filledRegion->getElemSize () == 0);
		assert (subRegOff * filledRegion->getElemSize () +
				getRegionByteSize (filledRegion, execSize) <=
				2 * REG_BYTE_SIZE);

        return builder_->createSrcRegRegion(
			filledRegion->getModifier (), Direct, fillRangeRegVar, REG_ORIGIN,
            subRegOff, filledRegion->getRegion(), filledRegion->getType(), filledRegion->getAccRegSel());
	}
	else
    {
        return builder_->createSrcRegRegion(
			filledRegion->getModifier (), Direct, fillRangeRegVar,
			REG_ORIGIN, SUBREG_ORIGIN, filledRegion->getRegion (),
			filledRegion->getType(), filledRegion->getAccRegSel());
	}
}

// Create a source region for the spill regvar that can be used as an operand
// for a mov instruction used to copy the value to an MRF write payload for
// an oword block write message. The spillRangeRegVar segment is guaranteed
// to start at an dword boundary and of a dword aligned size by construction.
// The whole spillRangeRegVar segment needs to be copied out to the MRF write
// payload. The source region generated is <4;4,1>:ud so that a row occupies
// a packed oword. The exec size used in the copy instruction needs to be a
// multiple of 4 depending on the size of the spill regvar - 4 or 8 for the
// the spill regvar appearing as the destination in a regulat 2 cycle
// instructions and 16 when appearing in simd16 instructions.

inline G4_SrcRegRegion *
SpillManagerGMRF::createBlockSpillRangeSrcRegion (
	G4_RegVar *       spillRangeRegVar,
	unsigned          regOff,
	unsigned          subregOff
)
{
	assert (getByteSize (spillRangeRegVar) % DWORD_BYTE_SIZE == 0);
	RegionDesc * rDesc =
		builder_->rgnpool.createRegion (DWORD_BYTE_SIZE, DWORD_BYTE_SIZE, 1);
    return builder_->createSrcRegRegion(
		Mod_src_undef, Direct, spillRangeRegVar, (short) regOff, (short) subregOff,
		rDesc, Type_UD);
}

// Create a MRF regvar and a declare directive for it, to represent an
// implicit MFR live range that will be used as the send message payload
// header and write payload for spilling a regvar to memory.

G4_Declare *
SpillManagerGMRF::createMRangeDeclare (
	G4_RegVar * regVar
)
{
    if (useSplitSend())
    {
        return builder_->getBuiltinR0();
    }

	G4_RegVar * repRegVar =
		(regVar->isRegVarTransient ())? regVar->getBaseRegVar (): regVar;
	const char * name =
		createImplicitRangeName (
			"SP_MSG", repRegVar, getMsgSpillIndex (repRegVar));
	unsigned regVarByteSize = getByteSize (regVar);
	unsigned writePayloadHeight = cdiv (regVarByteSize, REG_BYTE_SIZE);

	if (writePayloadHeight > SPILL_PAYLOAD_HEIGHT_LIMIT) {
		writePayloadHeight = SPILL_PAYLOAD_HEIGHT_LIMIT;
	}

	unsigned payloadHeaderHeight =
		(regVarByteSize != DWORD_BYTE_SIZE)?
		OWORD_PAYLOAD_HEADER_MAX_HEIGHT: DWORD_PAYLOAD_HEADER_MAX_HEIGHT;
	unsigned short height = payloadHeaderHeight + writePayloadHeight;
	unsigned short width = REG_DWORD_SIZE;

	// We should not find ourselves using dword scattered write
	if( useScratchMsg_ )
	{
		assert( payloadHeaderHeight != DWORD_PAYLOAD_HEADER_MAX_HEIGHT );
	}

	G4_Declare * msgRangeDeclare =
		createRangeDeclare (
			name,
            G4_GRF,
            width, height, Type_UD, NULL, DEF_HORIZ_STRIDE,
            DeclareType::Tmp, regVar->getNonTransientBaseRegVar (), NULL, 0);

    if( failSafeSpill_ )
    {
        msgRangeDeclare->getRegVar()->setPhyReg(builder_->phyregpool.getGreg(spillRegStart_), 0);
    }

	return msgRangeDeclare;
}

// Create a MRF regvar and a declare directive for it, to represent an
// implicit MFR live range that will be used as the send message payload
// header and write payload for spilling a regvar region to memory.

G4_Declare *
SpillManagerGMRF::createMRangeDeclare (
	G4_DstRegRegion * region,
	unsigned          execSize
)
{
    if (useSplitSend())
    {
        return builder_->getBuiltinR0();
    }

	const char * name =
		createImplicitRangeName (
			"SP_MSG", getRegVar (region),
			getMsgSpillIndex (getRegVar (region)));
	unsigned regionByteSize = getSegmentByteSize (region, execSize);
	unsigned writePayloadHeight = cdiv (regionByteSize, REG_BYTE_SIZE);
    unsigned msgType = getMsgType (region, execSize);
	unsigned payloadHeaderHeight =
		( msgType == owordMask () ||
          msgType == hwordMask () )?
		OWORD_PAYLOAD_HEADER_MAX_HEIGHT: DWORD_PAYLOAD_HEADER_MAX_HEIGHT;

	// We should not find ourselves using dword scattered write
	if( useScratchMsg_ )
	{
		assert( payloadHeaderHeight != DWORD_PAYLOAD_HEADER_MAX_HEIGHT );
	}

	unsigned height = payloadHeaderHeight + writePayloadHeight;
	unsigned short width = REG_DWORD_SIZE;
	G4_Declare * msgRangeDeclare =
		createRangeDeclare (
			name,
            G4_GRF,
            width, (unsigned short) height, Type_UD, NULL, DEF_HORIZ_STRIDE,
            DeclareType::Tmp, region->getBase ()->asRegVar (), NULL, 0);

    if( failSafeSpill_ )
    {
        msgRangeDeclare->getRegVar()->setPhyReg(builder_->phyregpool.getGreg(spillRegOffset_), 0);
        spillRegOffset_ += height;
    }

	return msgRangeDeclare;
}

// Create a MRF regvar and a declare directive for it, that will be used as
// the send message payload header and write payload for filling a regvar
// from memory.

G4_Declare *
SpillManagerGMRF::createMRangeDeclare (
	G4_SrcRegRegion * region,
	unsigned          execSize
)
{
    if (useSplitSend())
    {
        return builder_->getBuiltinR0();
    }

	const char * name =
		createImplicitRangeName (
			"FL_MSG", getRegVar (region),
			getMsgFillIndex (getRegVar (region)));
    getSegmentByteSize(region, execSize);
	unsigned payloadHeaderHeight =
		(getMsgType (region, execSize) == owordMask ())?
		OWORD_PAYLOAD_HEADER_MIN_HEIGHT: DWORD_PAYLOAD_HEADER_MIN_HEIGHT;

	// We should not find ourselves using dword scattered write
	if( useScratchMsg_ )
	{
		assert( payloadHeaderHeight != DWORD_PAYLOAD_HEADER_MAX_HEIGHT );
		// When using scratch msg descriptor we dont need to use a
		// separate GRF for payload. Source operand of send can directly
		// use r0.0.
		return builder_->getBuiltinR0();
	}

	unsigned height = payloadHeaderHeight;
	unsigned width = REG_DWORD_SIZE;
	G4_Declare * msgRangeDeclare =
		createRangeDeclare (
			name,
            G4_GRF,
            (unsigned short) width, (unsigned short) height, Type_UD, NULL, DEF_HORIZ_STRIDE,
            DeclareType::Tmp, region->getBase ()->asRegVar (), NULL, 0);

    if( failSafeSpill_ )
    {
        msgRangeDeclare->getRegVar()->setPhyReg(builder_->phyregpool.getGreg(spillRegOffset_), 0);
        spillRegOffset_ += height;
    }

	return msgRangeDeclare;
}

// Create a destination region for the MRF regvar for the write payload
// portion of the oword block send message (used for spill). The exec size
// can be either 4 or 8 for a regular 2 cycle instruction detination spills or
// 16 for simd16 instruction destination spills.

inline G4_DstRegRegion *
SpillManagerGMRF::createMPayloadBlockWriteDstRegion (
	G4_RegVar *       mrfRange,
	unsigned          regOff,
	unsigned          subregOff
)
{
	regOff += OWORD_PAYLOAD_WRITE_REG_OFFSET;
	subregOff += OWORD_PAYLOAD_WRITE_SUBREG_OFFSET;
    return builder_->createDstRegRegion(
		Direct, mrfRange, (short) regOff, (short) subregOff, DEF_HORIZ_STRIDE, Type_UD);
}

// Create a destination region for the MRF regvar for the input header
// payload portion of the send message to the data port. The exec size
// needs to be 8 for the mov instruction that uses this as a destination.

inline G4_DstRegRegion *
SpillManagerGMRF::createMHeaderInputDstRegion (
	G4_RegVar *       mrfRange,
	unsigned          subregOff
)
{
    return builder_->createDstRegRegion(
		Direct, mrfRange, PAYLOAD_INPUT_REG_OFFSET, (short) subregOff,
		DEF_HORIZ_STRIDE, Type_UD);
}

// Create a destination region for the MRF regvar for the payload offset
// portion of the oword block send message. The exec size needs to be 1
// for the mov instruction that uses this as a destination.

inline G4_DstRegRegion *
SpillManagerGMRF::createMHeaderBlockOffsetDstRegion (
	G4_RegVar *       mrfRange
)
{
    return builder_->createDstRegRegion(
		Direct, mrfRange, OWORD_PAYLOAD_SPOFFSET_REG_OFFSET,
		OWORD_PAYLOAD_SPOFFSET_SUBREG_OFFSET, DEF_HORIZ_STRIDE,
		Type_UD);
}

// Create a source region for the input payload (r0.0). The exec size
// needs to be 8 for the mov instruction that uses this as a source.

inline G4_SrcRegRegion *
SpillManagerGMRF::createInputPayloadSrcRegion ()
{
    G4_RegVar * inputPayloadDirectReg = builder_->getBuiltinR0()->getRegVar();
	RegionDesc * rDesc =
		builder_->rgnpool.createRegion (
			REG_DWORD_SIZE, REG_DWORD_SIZE, DEF_HORIZ_STRIDE);
    return builder_->createSrcRegRegion(
		Mod_src_undef, Direct, inputPayloadDirectReg,
		PAYLOAD_INPUT_REG_OFFSET, PAYLOAD_INPUT_SUBREG_OFFSET,
		rDesc, Type_UD);
}

// Create and initialize the message header for the send instruction for
// save/load of value to/from memory.
// The header includes the input payload and the offset (for spill disp).

template <class REGION_TYPE>
inline G4_Declare *
SpillManagerGMRF::createAndInitMHeader (
	REGION_TYPE * region,
	unsigned      execSize
)
{
	// Create the MRF live range for the message.
    if (canDoSLMSpill())
    {
        //SLM spill/fill functions create their own header
        return nullptr;
    }

	G4_Declare * mRangeDcl = createMRangeDeclare (region, execSize);
	return initMHeader (mRangeDcl, region, execSize);
}

// Initialize the message header for the send instruction for save/load
// of value to/from memory.
// The header includes the input payload and the offset (for spill disp).

template <class REGION_TYPE>
G4_Declare *
SpillManagerGMRF::initMHeader (
	G4_Declare *  mRangeDcl,
	REGION_TYPE * region,
	unsigned      execSize
)
{
	// Initialize the message header with the input payload.

	if( useScratchMsg_ )
	{
		if( mRangeDcl == builder_->getBuiltinR0() )
		{
			// mRangeDcl is NULL for fills
			return mRangeDcl;
		}
	}

    G4_DstRegRegion* dstOpnd = builder_->createDstRegRegion(Direct, mRangeDcl->getRegVar(), 0, 0, 1, Type_UD);
    auto newInst = builder_->createInst ( NULL, G4_pseudo_kill, NULL, false, 1, dstOpnd, NULL, NULL, 0 );
    newInst->setCISAOff(curInst->getCISAOff());

	G4_DstRegRegion * mHeaderInputDstRegion =
		createMHeaderInputDstRegion (mRangeDcl->getRegVar ());
	G4_SrcRegRegion * inputPayload = createInputPayloadSrcRegion ();
	createMovInst (REG_DWORD_SIZE, mHeaderInputDstRegion, inputPayload);
	numGRFMove ++;

	if( useScratchMsg_ )
	{
		// Initialize msg header when region is a spill
		// When using scratch msg description, we only need to copy
		// r0.0 in to msg header. Memory offset will be
		// specified in the msg descriptor.
	}
	else
	// Initialize the message header with the spill disp for block
	// read/write.
    {
		G4_DstRegRegion * mHeaderOffsetDstRegion =
			createMHeaderBlockOffsetDstRegion (mRangeDcl->getRegVar ());
        int offset = getSegmentDisp(region, execSize);
        getSpillOffset(offset);
		unsigned segmentDisp = offset / OWORD_BYTE_SIZE;
		G4_Imm * segmentDispImm = builder_->createImm (segmentDisp, Type_UD);
		G4_RegVar * baseRegVar = NULL;
		if (region->isSrcRegRegion())
		{
			baseRegVar = getReprRegVar(region->asSrcRegRegion()->getBase()->asRegVar());
		}
		else if (region->isDstRegRegion())
		{
			baseRegVar = getReprRegVar(region->asDstRegRegion()->getBase()->asRegVar());
		}
		else
		{
			MUST_BE_TRUE (false, ERROR_GRAPHCOLOR);
		}

		if (builder_->getIsKernel() == false && baseRegVar->getDeclare()->getHasFileScope() == false)
		{
			createAddFPInst (
				SCALAR_EXEC_SIZE, mHeaderOffsetDstRegion, segmentDispImm);
		}
		else
		{
			createMovInst (
				SCALAR_EXEC_SIZE, mHeaderOffsetDstRegion, segmentDispImm);
		}
        numGRFMove ++;
	}

	// Initialize the message header with the spill disp for scatter
	// read/write.
	return mRangeDcl;
}

// Create and initialize the message header for the send instruction.
// The header includes the input payload (for spill disp).

inline G4_Declare *
SpillManagerGMRF::createAndInitMHeader (
	G4_RegVar *			 regVar
)
{
	// Create the MRF live range for the message.

    if (canDoSLMSpill())
    {
        return nullptr;
    }

	G4_Declare * mRangeDcl = createMRangeDeclare (regVar);
	return initMHeader (mRangeDcl);
}

// Initialize the message header for the send instruction.
// The header includes the input payload (for spill disp).

G4_Declare *
SpillManagerGMRF::initMHeader (
	G4_Declare *		 mRangeDcl
)
{
	// Initialize the message header with the input payload.
	if( useScratchMsg_ )
	{
		if( mRangeDcl == builder_->getBuiltinR0() )
		{
			// mRangeDcl is NULL for fills
			return mRangeDcl;
		}
	}

    G4_DstRegRegion* dstOpnd = builder_->createDstRegRegion(Direct, mRangeDcl->getRegVar(), 0, 0, 1, Type_UD);
    auto newInst = builder_->createInst ( NULL, G4_pseudo_kill, NULL, false, 1, dstOpnd, NULL, NULL, 0 );
    newInst->setCISAOff(curInst->getCISAOff());

	G4_DstRegRegion * mHeaderInputDstRegion =
		createMHeaderInputDstRegion (mRangeDcl->getRegVar ());
	G4_SrcRegRegion * inputPayload = createInputPayloadSrcRegion ();
	createMovInst (REG_DWORD_SIZE, mHeaderInputDstRegion, inputPayload);
	numGRFMove ++;

	return mRangeDcl;
}

// Initialize the the write payload part of the message for spilled regvars.
// Either of the following restrictions for spillRangeDcl are assumed:
//		- the regvar element type is dword and its 2 <= width <= 8 and
//        height - regOff == 1
//		- the regvar element type is dword and its width = 8 and
//        2 <= height - regOff <= 8
//      - the regvar element type is dword and its width and height are 1

void
SpillManagerGMRF::initMWritePayload (
	G4_Declare *	  spillRangeDcl,
	G4_Declare *	  mRangeDcl,
	unsigned          regOff,
	unsigned          height
)
{
    if (useSplitSend())
    {
        // no need for payload moves if using sends
        return;
    }

	// We use an block write when the spilled regvars's segment is greater
	// than a dword. Generate a mov to copy the oword aligned segment into
	// the write payload part of the message.
    {
		unsigned nRows = height;

		for (unsigned i = 0; i < nRows; i++) {
			G4_SrcRegRegion * spillRangeSrcRegion =
				createBlockSpillRangeSrcRegion (
					spillRangeDcl->getRegVar (), i + regOff);
			G4_DstRegRegion * mPayloadWriteDstRegion =
				createMPayloadBlockWriteDstRegion (
					mRangeDcl->getRegVar (), i);
			unsigned char movExecSize =
				(nRows > 1)? REG_DWORD_SIZE: spillRangeDcl->getNumElems ();
			createMovInst (
				movExecSize, mPayloadWriteDstRegion, spillRangeSrcRegion);
            numGRFMove ++;
		}
	}
}

// Initialize the the write payload part of the message for spilled regions.

void
SpillManagerGMRF::initMWritePayload (
	G4_Declare *	  spillRangeDcl,
	G4_Declare *	  mRangeDcl,
	G4_DstRegRegion * spilledRangeRegion,
	unsigned          execSize,
	unsigned          regOff
)
{
	// We use an block write when the spilled region's segment is greater
	// than a dword. Generate a mov to copy the oword aligned segment into
	// the write payload part of the message.
    if (useSplitSend())
    {
        // no need for payload moves
        return;
    }
    else
    {
		G4_SrcRegRegion * spillRangeSrcRegion =
			createBlockSpillRangeSrcRegion (
				spillRangeDcl->getRegVar (), regOff);
		G4_DstRegRegion * mPayloadWriteDstRegion =
			createMPayloadBlockWriteDstRegion (mRangeDcl->getRegVar ());
		unsigned segmentByteSize =
			getSegmentByteSize (spilledRangeRegion, execSize);
		unsigned char movExecSize = segmentByteSize / DWORD_BYTE_SIZE;

		// Write entire GRF when using scratch msg descriptor
		if( useScratchMsg_)
		{
			if( movExecSize <= 8 )
				movExecSize = 8;
			else if( movExecSize < 16 )
				movExecSize = 16;
		}

		assert (segmentByteSize % DWORD_BYTE_SIZE == 0);
		assert (movExecSize <= 16);
		createMovInst (
			movExecSize, mPayloadWriteDstRegion, spillRangeSrcRegion);
    	numGRFMove ++;
	}
}

// Return the block size encoding for oword block reads.

inline unsigned
SpillManagerGMRF::blockSendBlockSizeCode (
	unsigned size
) const
{
	unsigned code;

	switch (size) {
		case 1:
			code = 0;
			break;
		case 2:
			code = 2;
			break;
		case 4:
			code = 3;
			break;
		case 8:
			code = 4;
			break;
		default:
			assert (0);
			code = 0;
	}

	return code << getSendDescDataSizeBitOffset ();
}

// Return the block size encoding for dword scatter reads.

inline unsigned
SpillManagerGMRF::scatterSendBlockSizeCode (
	unsigned size
) const
{
	unsigned code;

	switch (size) {
		case 1:
			// We will use an exec size of 1 to perform 1 dword read/write.
		case 8:
			code = 0x02;
			break;
		case 16:
			code = 0x03;
			break;
		default:
			assert (0);
			code = 0;
	}

	return code << getSendDescDataSizeBitOffset ();
}

static uint32_t getScratchBlocksizeEncoding(int size)
{

    unsigned blocksize_encoding = 0;
    if (size == 1)
    {
        blocksize_encoding = 0x0;
    }
    else if (size == 2)
    {
        blocksize_encoding = 0x1;
    }
    else if (size == 4)
    {
        blocksize_encoding = 0x2;
    }
    else if (size == 8)
    {
        assert(getGenxPlatform() >= GENX_SKL);
        blocksize_encoding = 0x3;
    }
    else
        assert(false);
    return blocksize_encoding;
}

// Create the message descriptor for a spill send instruction for spilled
// post destinations of send instructions.

G4_Imm *
SpillManagerGMRF::createSpillSendMsgDesc (
	unsigned    regOff,
	unsigned    height,
	unsigned &  execSize,
	G4_RegVar* base
)
{
	unsigned message = 0;

	if( useScratchMsg_)
	{
		unsigned headerPresent = 0x80000;
		message = headerPresent;
		unsigned msgLength = useSplitSend() ? SCRATCH_PAYLOAD_HEADER_MAX_HEIGHT : SCRATCH_PAYLOAD_HEADER_MAX_HEIGHT + height;
		message |= (msgLength << getSendMsgLengthBitOffset() );
		message |= (1 << SCRATCH_MSG_DESC_CATEORY);
        message |= (1 << SCRATCH_MSG_DESC_CHANNEL_MODE);
		message |= (1 << SCRATCH_MSG_DESC_OPERATION_MODE);
        unsigned blocksize_encoding = getScratchBlocksizeEncoding(height);
		message |= (blocksize_encoding << SCRATCH_MSG_DESC_BLOCK_SIZE);
        int offset = getDisp(base);
        getSpillOffset(offset);
		message |= (offset >> 5) + regOff;
		execSize = 16;
	}
	else
	{
		unsigned segmentByteSize = height * REG_BYTE_SIZE;
		unsigned writePayloadCount = cdiv (segmentByteSize, REG_BYTE_SIZE);
		unsigned statelessSurfaceIndex = 0xFF;
		message = statelessSurfaceIndex;

	    unsigned headerPresent = 0x80000;
		message |= headerPresent;
		unsigned messageType = getSendOwordWriteType();
		message |= messageType << getSendWriteTypeBitOffset ();
		unsigned payloadHeaderCount = OWORD_PAYLOAD_HEADER_MAX_HEIGHT;
		unsigned messageLength = useSplitSend() ? payloadHeaderCount : writePayloadCount + payloadHeaderCount;
		message |= messageLength << getSendMsgLengthBitOffset ();
		unsigned segmentOwordSize = cdiv(segmentByteSize, OWORD_BYTE_SIZE);
		message |= blockSendBlockSizeCode (segmentOwordSize);
		execSize = LIMIT_SEND_EXEC_SIZE (segmentOwordSize * DWORD_BYTE_SIZE);
	}
	return builder_->createImm (message, Type_UD);
}

// Create the message descriptor for a spill send instruction for spilled
// destination regions.

G4_Imm *
SpillManagerGMRF::createSpillSendMsgDesc (
	G4_DstRegRegion * spilledRangeRegion,
	unsigned &        execSize
)
{
	unsigned message = 0;

	if( useScratchMsg_)
	{
		/*
		bits	description
		18:0	function control
		19	Header present
		24:20	Response length
		28:25	Message length
		31:29	MBZ

		18:0
		11:0	Offset (12b hword offset)
		13:12	Block size (00 - 1 register, 01 - 2 regs, 10 - reserved, 11 - 4 regs)
		14	MBZ
		15	Invalidate after read (0 - no invalidate, 1 - invalidate)
		16	Channel mode (0 - oword, 1 - dword)
		17	Operation type (0 - read, 1 - write)
		18	Category (1 - scratch block read/write)
		*/
		unsigned segmentByteSize = getSegmentByteSize (spilledRangeRegion, execSize);
		unsigned writePayloadCount = cdiv (segmentByteSize, REG_BYTE_SIZE);
		unsigned headerPresent = 0x80000;
		message |= headerPresent;

		unsigned payloadHeaderCount = SCRATCH_PAYLOAD_HEADER_MAX_HEIGHT;
        // message length = 1 if we are using sends, 1 + payload otherwise
		unsigned messageLength = useSplitSend() ? payloadHeaderCount :
            writePayloadCount + payloadHeaderCount;
		message |= (messageLength << getSendMsgLengthBitOffset() );
		message |= (1 << SCRATCH_MSG_DESC_CATEORY); // category
        message |= (1 << SCRATCH_MSG_DESC_CHANNEL_MODE); // channel mode
		message |= (1 << SCRATCH_MSG_DESC_OPERATION_MODE); // write operation
		unsigned numGRFs = cdiv(segmentByteSize, G4_GRF_REG_NBYTES);

        unsigned blocksize_encoding = getScratchBlocksizeEncoding(numGRFs);

		message |= (blocksize_encoding << SCRATCH_MSG_DESC_BLOCK_SIZE);
        int offset = getRegionDisp(spilledRangeRegion);
        getSpillOffset(offset);
		message |= offset >> 5; 
        if (numGRFs > 1)
        {
            execSize = 16;
        }
        else
        {
            if (execSize > 8)
            {
                execSize = 16;
            }
            else
            {
                execSize = 8;
            }
        }
	}
	else
	{
		unsigned segmentByteSize =
			getSegmentByteSize (spilledRangeRegion, execSize);
		unsigned writePayloadCount = cdiv (segmentByteSize, REG_BYTE_SIZE);
		unsigned statelessSurfaceIndex = 0xFF;
		message = statelessSurfaceIndex;

		unsigned headerPresent = 0x80000;
	    message |= headerPresent;
		unsigned messageType = getSendOwordWriteType();
		message |= messageType << getSendWriteTypeBitOffset ();
		unsigned payloadHeaderCount = OWORD_PAYLOAD_HEADER_MAX_HEIGHT;
		unsigned messageLength = useSplitSend() ? payloadHeaderCount : writePayloadCount + payloadHeaderCount;
		message |= messageLength << getSendMsgLengthBitOffset ();
		unsigned segmentOwordSize = cdiv(segmentByteSize, OWORD_BYTE_SIZE);
		message |= blockSendBlockSizeCode (segmentOwordSize);
		execSize = LIMIT_SEND_EXEC_SIZE (segmentOwordSize * DWORD_BYTE_SIZE);
	}
	return builder_->createImm (message, Type_UD);
}

uint32_t
SpillManagerGMRF::getUntypedSLMMsgDesc(int numReg, bool isRead) const
{
    // SLM Untyped write
    uint32_t message = 0;
    unsigned responseLength = isRead ? numReg : 0;
    unsigned SLMIndex = 0xFE;
    message |= SLMIndex;
    uint32_t messageType = isRead ? DC1_UNTYPED_SURFACE_READ : DC1_UNTYPED_SURFACE_WRITE;
    message |= messageType << getSendReadTypeBitOffset();
    uint32_t simdMode = numReg == 1 ? MDC_SM3_SIMD8 : MDC_SM3_SIMD16;
    message |= simdMode << 12;
    uint32_t chMask = getChMaskForSpill(numReg).getHWEncoding();
    message |= chMask << 8;
    uint32_t messageLength = numReg == 1 ? 1 : 2;
    message |= messageLength << getSendMsgLengthBitOffset();
    message |= responseLength << getSendRspLengthBitOffset();

    return message;
}
// Create the message descriptor for a spill send instruction for spilled
// destination regions.

G4_Imm *
SpillManagerGMRF::createSpillSendMsgDesc(
    bool doSLMSpill,
    int size,
    int offset
)
{
    unsigned message = 0;

    if (doSLMSpill)
    {
        if (builder_->hasBlockedSLMMessage())
        {
            // SLM Hword aligned block write
            unsigned writePayloadCount = size;
            unsigned SLMIndex = 0xFE;
            message |= SLMIndex;
            unsigned dataElements = getHWordEncoding(writePayloadCount);
            message |= dataElements << 8;
            message |= 1 << 13; // HWord
            unsigned messageType = DC1_HWORD_ALIGNED_BLOCK_WRITE;
            message |= messageType << getSendReadTypeBitOffset();
            unsigned headerPresent = 0x80000;
            message |= headerPresent;
            unsigned messageLength = useSplitSend() ? 1 : writePayloadCount + 1;
            message |= messageLength << getSendMsgLengthBitOffset();
            unsigned responseLength = 0;
            message |= responseLength << getSendRspLengthBitOffset();
        }
        else
        {
            message = getUntypedSLMMsgDesc(size, false);
        }
    }
    else if (useScratchMsg_)
    {
        /*
        bits	description
        18:0	function control
        19	Header present
        24:20	Response length
        28:25	Message length
        31:29	MBZ

        18:0
        11:0	Offset (12b hword offset)
        13:12	Block size (00 - 1 register, 01 - 2 regs, 10 - reserved, 11 - 4 regs)
        14	MBZ
        15	Invalidate after read (0 - no invalidate, 1 - invalidate)
        16	Channel mode (0 - oword, 1 - dword)
        17	Operation type (0 - read, 1 - write)
        18	Category (1 - scratch block read/write)
        */
        unsigned writePayloadCount = size;
        unsigned headerPresent = 0x80000;
        message |= headerPresent;

        unsigned payloadHeaderCount = SCRATCH_PAYLOAD_HEADER_MAX_HEIGHT;
        // message length = 1 if we are using sends, 1 + payload otherwise
        unsigned messageLength = useSplitSend() ? payloadHeaderCount :
            writePayloadCount + payloadHeaderCount;
        message |= (messageLength << getSendMsgLengthBitOffset());
        message |= (1 << SCRATCH_MSG_DESC_CATEORY); // category
        message |= (1 << SCRATCH_MSG_DESC_CHANNEL_MODE); // channel mode
        message |= (1 << SCRATCH_MSG_DESC_OPERATION_MODE); // write operation
        unsigned numGRFs = size;

        unsigned blocksize_encoding = getScratchBlocksizeEncoding(numGRFs);

        message |= (blocksize_encoding << SCRATCH_MSG_DESC_BLOCK_SIZE);
        message |= (offset >> 5); // displacement
    }
    else
    {
        MUST_BE_TRUE(false, "should not reach here");
    }
    return builder_->createImm(message, Type_UD);
}

// Create an add instruction to add the FP needed for generating spill/fill code.
// We always set the NoMask flag and use a null conditional modifier.

inline G4_INST *
SpillManagerGMRF::createAddFPInst (
	unsigned char	  execSize,
	G4_DstRegRegion *	  dst,
    G4_Operand *	  src,
	G4_Predicate *    predicate
)
{
	RegionDesc* rDesc = builder_->getRegionScalar();
    G4_Operand* fp = builder_->createSrcRegRegion(
		Mod_src_undef, Direct, builder_->kernel.fg.framePtrDcl->getRegVar(),
		0, 0, rDesc, Type_UD);
    auto newInst = builder_->createInst (
        predicate, G4_add, NULL, false, execSize, dst,
        fp, src, InstOpt_WriteEnable);
    newInst->setCISAOff(curInst->getCISAOff());

    return newInst;

}

// Create a mov instruction needed for generating spill/fill code.
// We always set the NoMask flag and use a null conditional modifier.

inline G4_INST *
SpillManagerGMRF::createMovInst (
	unsigned char	  execSize,
	G4_DstRegRegion *	  dst,
    G4_Operand *	  src,
	G4_Predicate *    predicate,
	unsigned int      options
)
{
    auto newInst = builder_->createInst (
        predicate, G4_mov, NULL, false, execSize, dst,
        src, NULL, options);
    newInst->setCISAOff(curInst->getCISAOff());

    return newInst;
}

// Create a send instruction needed for generating spill/fill code.
// We always set the NoMask flag and use a null predicate and conditional
// modifier.

inline G4_INST *
SpillManagerGMRF::createSendInst(
    unsigned char	  execSize,
    G4_DstRegRegion *	  postDst,
    G4_SrcRegRegion *	  payload,
    G4_Imm *		  desc,
    CISA_SHARED_FUNCTION_ID funcID,
    bool              isWrite,
    unsigned          option
)
{
    G4_INST* sendInst;

    G4_Imm *exDesc = builder_->createImm(funcID, Type_UD);

    sendInst = builder_->createSendInst(
        NULL, G4_send, execSize, postDst,
        payload, exDesc, desc, option, !isWrite, isWrite, nullptr);
    sendInst->setCISAOff(curInst->getCISAOff());
    sendInst->setSpillOrFill();

    return sendInst;
}

// Create the send instructions to fill in the value of spillRangeDcl into
// fillRangeDcl in aligned portions.

static int getNextSize(int height, bool useHWordMsg)
{

    if (getGenxPlatform() >= GENX_SKL && height >= 8 && useHWordMsg)
    {
        return 8;
    }
    else if (height >= 4)
    {
        return 4;
    }
    else if (height >= 2)
    {
        return 2;
    }
    else if (height >= 1)
    {
        return 1;
    }
    return 0;
}

void
SpillManagerGMRF::sendInSpilledRegVarPortions (
	G4_Declare *	  fillRangeDcl,
	G4_Declare *	  mRangeDcl,
	unsigned          regOff,
	unsigned          height,
	unsigned		  srcRegOff
)
{

    if (canDoSLMSpill())
    {
        // break fills into 8/4/2/1 chunks
        int offset = 0;
        G4_RegVar* r = fillRangeDcl->getRegVar();
        G4_RegVarTmp* rvar = static_cast<G4_RegVarTmp*> (r);
        int memOffset = getDisp(rvar->getBaseRegVar()) & GRF_ALIGN_MASK;
        while (height > 0)
        {
            int size = getNextSize(height, true);
            memOffset += offset * 32;
            createFill(fillRangeDcl, offset, size, memOffset);
            height -= size;
            offset += size;
        }
        return;
    }

	if (useScratchMsg_)
	{
		// Skip initializing message header
	}
	else
	{
		// Initialize the message header with the spill disp for portion. 
        int offset = getDisp(fillRangeDcl->getRegVar()) + regOff * REG_BYTE_SIZE;
        getSpillOffset(offset);

		unsigned segmentDisp = offset / OWORD_BYTE_SIZE;
		G4_Imm * segmentDispImm = builder_->createImm (segmentDisp, Type_UD);
		G4_DstRegRegion * mHeaderOffsetDstRegion =
			createMHeaderBlockOffsetDstRegion (mRangeDcl->getRegVar ());

        if (builder_->getIsKernel() == false &&
			getReprRegVar(fillRangeDcl->getRegVar())->getDeclare()->getHasFileScope() == false)
		{
			createAddFPInst (
				SCALAR_EXEC_SIZE, mHeaderOffsetDstRegion, segmentDispImm);
		}
		else
		{
			createMovInst (SCALAR_EXEC_SIZE, mHeaderOffsetDstRegion, segmentDispImm);
		}
    	numGRFMove ++;
	}

    // Read in the portions using a greedy approach.
    int currentStride = getNextSize(height, useScratchMsg_);

    if (currentStride)
    {
        createFillSendInstr(fillRangeDcl, mRangeDcl, regOff, currentStride, srcRegOff);
        numGRFFill++;
		if (height - currentStride > 0)
        {
			sendInSpilledRegVarPortions (
				fillRangeDcl, mRangeDcl, regOff + currentStride, height -currentStride, srcRegOff + currentStride);
		}
    }
}

// Copy out the source regvar to the destination regvar starting at the dstOff
// row. The regvars have to be 256 bit wide.

void
SpillManagerGMRF::copyOut256BitWideRegVar (
	G4_Declare * dstRegDcl,
	G4_Declare * srcRegDcl,
	unsigned     dstOff
)
{
	assert (srcRegDcl->getNumElems () * srcRegDcl->getElemSize () ==
			REG_BYTE_SIZE &&
			dstRegDcl->getNumElems () * dstRegDcl->getElemSize () ==
			REG_BYTE_SIZE );
	int numCopies = dstRegDcl->getNumRows () - dstOff;

	for (int i = 0; i < numCopies; i++) {
		RegionDesc * rDesc =
			builder_->rgnpool.createRegion (REG_DWORD_SIZE, REG_DWORD_SIZE, 1);
        G4_SrcRegRegion * srcRegRegion =
            builder_->createSrcRegRegion(
			Mod_src_undef, Direct, srcRegDcl->getRegVar (),
			(short) i, SUBREG_ORIGIN, rDesc, Type_UD);
        G4_DstRegRegion * dstRegRegion = builder_->createDstRegRegion(
			Direct, dstRegDcl->getRegVar (), dstOff + i, SUBREG_ORIGIN,
			DEF_HORIZ_STRIDE, Type_UD);
		createMovInst (REG_DWORD_SIZE, dstRegRegion, srcRegRegion);
    	numGRFMove ++;
	}
}

// Check if we need to perform the pre-load of the spilled region's
// segment from spill memory. A pre-load is required under the following
// circumstances:
//		- for partial writes - horizontal stride greater than one, and when
//		  the emask and predicate can possibly disable channels (for now if
//        predicates or condition modofoers are present then we conservatively
//        assume a partial write)
//		- write's where the segment is larger than the actaully written region
//        (either because the spill offset for the region or its size is not
//         oword or dword aligned for writing the exact region)

bool
SpillManagerGMRF::shouldPreloadSpillRange (
	G4_DstRegRegion * spilledRangeRegion,
	uint8_t     execSize,
	G4_INST *         instContext
)
{
	// Check for partial and unaligned regions and add pre-load code, if
	// necessary.

	if (isPartialRegion (spilledRangeRegion, execSize) ||
		isUnalignedRegion (spilledRangeRegion, execSize) ||
		isPartialContext (spilledRangeRegion, instContext, inSIMDCFContext_))
    {
#if 0
        // special check for scalar variables: no need for pre-fill if instruction is not predicated
        // FIXME: need to update this if we ever decide to pack scalar variables in memory
        if (spilledRangeRegion->getTopDcl()->getNumElems() == 1 &&
            instContext->getPredicate() == nullptr)
        {
            return false;
        }
#endif
		return true;
	}

	// No pre-load for whole and aligned region writes

	else {
		return false;
	}
}

// Create the send instruction to perform the pre-load of the spilled region's
// segment into spill memory.

void SpillManagerGMRF::preloadSpillRange (
	G4_Declare *	  spillRangeDcl,
	G4_Declare *	  mRangeDcl,
	G4_DstRegRegion * spilledRangeRegion,
	uint8_t     execSize
)
{
    if (canDoSLMSpill())
    {
        int numGRF = spilledRangeRegion->crossGRF() ? 2 : 1;
        int offset = getRegionDisp(spilledRangeRegion) & GRF_ALIGN_MASK;
        createFill(spillRangeDcl, 0, numGRF, offset);
        return;
    }

    // When execSize is 32, regions <32, 32, 1> or <64; 32, 2> are invalid.
    // Use a uniform region descriptor <stride; 1, 0>. Note that stride could
    // be 0 when execsize is 1.
    uint16_t hstride = spilledRangeRegion->getHorzStride();
    RegionDesc *rDesc = builder_->createRegionDesc(execSize, hstride, 1, 0);

	if( useScratchMsg_)
	{
		// src region's base refers to the filled region's base
		// The size of src region is equal to number of rows that
		// have to be filled, starting at the reg offset specified
		// in the original operand. For eg,
		// Let the spilled operand be V40(3,3)
		//
		// => mov (1) V40(3,3)<1>:ud    V30(0,0)<0;1,0>:ud
		// When this will be replaced with a preload fill,
		// => mov (1) TM_GRF_V40_0(0,0)<1>:ud   V30(0,0)<0;1,0>:ud
		// => send (16) SP_V40_0(0,0)<1>:ud ...							<---  load V40's 3rd row in SP_V40_0
		// => mov (1) SP_V40_0(0,3)<1>:ud   TM_GRF_V40_0(0,0)<8;8,1>:ud <--- overlay
		// => send (16) null ...										<--- store V40's updated 3rd row to memory
		//
		// Since the filled register's register offset is 0,0 in first
		// send instruction, this change is made when creating the operand
		// itself.
        G4_SrcRegRegion * preloadRegion = builder_->createSrcRegRegion(
			Mod_src_undef, Direct, spillRangeDcl->getRegVar (),
            REG_ORIGIN, spilledRangeRegion->getSubRegOff(),
			rDesc, spilledRangeRegion->getType ());
		// Attach preloadRegion to dummy mov so getLeftBound/getRightBound won't crash when called from crossGRF in createFillSendMsgDesc
        builder_->createInternalInst(NULL, G4_mov, NULL, false, execSize, builder_->createNullDst(Type_UD), preloadRegion, NULL, 0);
        numGRFFill++;
		createFillSendInstr (
			spillRangeDcl, mRangeDcl, preloadRegion, execSize);
	}
	else
	{
        G4_SrcRegRegion * preloadRegion = builder_->createSrcRegRegion(
			Mod_src_undef, Direct, spillRangeDcl->getRegVar (),
			spilledRangeRegion->getRegOff (), spilledRangeRegion->getSubRegOff (),
			rDesc, spilledRangeRegion->getType ());

        numGRFFill++;
        createFillSendInstr(
				spillRangeDcl, mRangeDcl, preloadRegion, execSize, 0);
	}
}

// Create the send instruction to perform the spill of the spilled regvars's
// segment into spill memory.

G4_INST *
SpillManagerGMRF::createSpillSendInstr (
	G4_Declare *	  spillRangeDcl,
	G4_Declare *	  mRangeDcl,
	unsigned          regOff,
	unsigned          height,
	unsigned		  srcRegOff
)
{
	unsigned execSize (0);

	G4_Imm * messageDescImm = NULL;

	if( useScratchMsg_)
	{
		G4_RegVar* r = spillRangeDcl->getRegVar();
		G4_RegVarTmp* rvar = static_cast<G4_RegVarTmp*> (r);
		messageDescImm =
			createSpillSendMsgDesc (srcRegOff, height, execSize, rvar->getBaseRegVar());
#ifdef _DEBUG
        int offset = (messageDescImm->getInt() & 0xFFF) * GENX_GRF_REG_SIZ;
        MUST_BE_TRUE(offset >= globalScratchOffset, "incorrect offset");
#endif
	}
	else
	{
		messageDescImm =
			createSpillSendMsgDesc (regOff, height, execSize);
	}

    G4_DstRegRegion * postDst = builder_->createNullDst(execSize > 8 ? Type_UW : Type_UD);

    G4_INST* sendInst = NULL;
    if (useSplitSend())
    {
        unsigned extMsgLength = height;
        uint16_t extFuncCtrl = 0;
        // both scratch and block read use DC
        CISA_SHARED_FUNCTION_ID funcID = SFID_DP_DC;

        G4_SendMsgDescriptor* desc = builder_->createSendMsgDesc( messageDescImm->getInt() & 0x0007FFFFu, 0, 1, funcID, false, extMsgLength, extFuncCtrl, false, true, NULL, NULL);
        RegionDesc* region = builder_->getRegionStride1();
        G4_SrcRegRegion* headerOpnd = builder_->Create_Src_Opnd_From_Dcl(builder_->getBuiltinR0(), region);
        G4_SrcRegRegion* srcOpnd = createBlockSpillRangeSrcRegion(spillRangeDcl->getRegVar (), regOff);

        sendInst = builder_->createSplitSendInst( NULL, G4_sends, (unsigned char) execSize, postDst, headerOpnd, srcOpnd, messageDescImm, InstOpt_WriteEnable, desc, NULL, 0);
        sendInst->setSpillOrFill();
        sendInst->setCISAOff(curInst->getCISAOff());
    }
    else
    {
        G4_SrcRegRegion * payload = builder_->createSrcRegRegion(Mod_src_undef, Direct,
            mRangeDcl->getRegVar(), 0, 0, builder_->getRegionStride1(), Type_UD);
        sendInst = createSendInst ((unsigned char) execSize, postDst, payload, messageDescImm, SFID_DP_DC, true, InstOpt_WriteEnable);
    }

    return sendInst;
}

// Create the send instruction to perform the spill of the spilled region's
// segment into spill memory.

G4_INST *
SpillManagerGMRF::createSpillSendInstr (
	G4_Declare *	  spillRangeDcl,
	G4_Declare *	  mRangeDcl,
	G4_DstRegRegion * spilledRangeRegion,
	unsigned          execSize,
    unsigned          option
)
{
	G4_Imm * messageDescImm =
		createSpillSendMsgDesc (spilledRangeRegion, execSize);

#ifdef _DEBUG
    if (useScratchMsg_)
    {
        int offset = (messageDescImm->getInt() & 0xFFF) * GENX_GRF_REG_SIZ;
        MUST_BE_TRUE(offset >= globalScratchOffset, "incorrect offset");
    }
#endif

    G4_DstRegRegion * postDst = builder_->createNullDst(execSize > 8 ? Type_UW : Type_UD);

    G4_INST* sendInst = NULL;
    if (useSplitSend())
    {
        unsigned extMsgLength = spillRangeDcl->getNumRows();
        uint16_t extFuncCtrl = 0;
        // both scratch and block read use DC
        CISA_SHARED_FUNCTION_ID funcID = SFID_DP_DC;

        G4_SendMsgDescriptor* desc = builder_->createSendMsgDesc( messageDescImm->getInt() & 0x0007FFFFu, 0, 1, funcID, false, extMsgLength, extFuncCtrl, false, true, NULL, NULL);
        RegionDesc* region = builder_->getRegionStride1();
        G4_SrcRegRegion* headerOpnd = builder_->Create_Src_Opnd_From_Dcl(builder_->getBuiltinR0(), region);
        G4_SrcRegRegion* srcOpnd = builder_->Create_Src_Opnd_From_Dcl(spillRangeDcl, region);

        sendInst = builder_->createSplitSendInst( NULL, G4_sends, (unsigned char) execSize, postDst, headerOpnd, srcOpnd, messageDescImm, option, desc, NULL, 0);
        sendInst->setSpillOrFill();
        sendInst->setCISAOff(curInst->getCISAOff());
    }
    else
    {
        G4_SrcRegRegion * payload = builder_->createSrcRegRegion(Mod_src_undef, Direct,
            mRangeDcl->getRegVar(), 0, 0, builder_->getRegionStride1(), Type_UD);
	    sendInst = createSendInst ((unsigned char) execSize, postDst, payload, messageDescImm, SFID_DP_DC, true, option);
    }

    return sendInst;
}

void SpillManagerGMRF::createSpill(
    G4_Declare* spillDcl,
    int spillRegOff,
    int size,
    int logicalOffset,
    uint32_t spillMask,
    int oldExecSize
)
{
    int varOffset = logicalOffset;
    bool doSLMSpill = getSpillOffset(varOffset);
    G4_Imm * messageDescImm = createSpillSendMsgDesc(doSLMSpill, size, varOffset);

    CISA_SHARED_FUNCTION_ID funcID = SFID_DP_DC;
    G4_Declare* sendSrc0 = nullptr;
    int esize = size > 1 || oldExecSize > 8 ? 16 : 8;
    if (spillMask & InstOpt_WriteEnable)
    {
        spillMask = InstOpt_WriteEnable;
    }


    if (doSLMSpill)
    {
        if (builder_->hasBlockedSLMMessage())
        {
            // create 1 GRF header
            // mov (1) H0.2<1>:ud offset:uw {NoMask}
            // also add a psuedo kill to make sure the header's life range is properly terminated
            sendSrc0 = builder_->createDeclareNoLookup("Spill_Header", G4_GRF, 8, 1, Type_UD);
            G4_DstRegRegion* psuedoKill = builder_->Create_Dst_Opnd_From_Dcl(sendSrc0, 1);
            auto newInst = builder_->createInst(nullptr, G4_pseudo_kill, nullptr, false, 1, psuedoKill, nullptr, nullptr, InstOpt_NoOpt);
            newInst->setCISAOff(curInst->getCISAOff());

            G4_DstRegRegion* dst = builder_->createDstRegRegion(Direct, sendSrc0->getRegVar(), 0, 2,
                1, Type_UD);
            G4_Imm* imm = builder_->createImm(varOffset, Type_UW);
            createMovInst(1, dst, imm);
        }
        else
        {
            // esize is dependent on number of GRFs we spill. This should match the oldExecSize 
            // if pre-fill is not required
            esize = size == 1 ? 8 : 16;
            funcID = SFID_DP_DC1;
            sendSrc0 = createSLMSpillAddr(size, varOffset);
        }
    }
    else
    {
        sendSrc0 = builder_->getBuiltinR0();
    }

    unsigned extMsgLength = size;
    uint16_t extFuncCtrl = 0;
    G4_SendMsgDescriptor* desc = builder_->createSendMsgDesc(messageDescImm->getInt() & 0x0007FFFFu, 0, 1, funcID, false, extMsgLength, extFuncCtrl, false, true, NULL, NULL);
    G4_SrcRegRegion* headerOpnd = builder_->Create_Src_Opnd_From_Dcl(sendSrc0, builder_->getRegionStride1());
    G4_SrcRegRegion* srcOpnd = createBlockSpillRangeSrcRegion(spillDcl->getRegVar(), spillRegOff);

    G4_DstRegRegion * postDst = builder_->createNullDst(esize == 16 ? Type_UW : Type_UD);

    G4_INST* sendInst = builder_->createSplitSendInst(NULL, G4_sends, (unsigned char)esize, postDst, headerOpnd, srcOpnd, messageDescImm, spillMask, desc, NULL, 0);
    sendInst->setSpillOrFill();
    sendInst->setCISAOff(curInst->getCISAOff());
}

// Create the message description for a fill send instruction for filled
// regvars.

G4_Imm *
SpillManagerGMRF::createFillSendMsgDesc (
	unsigned          regOff,
	unsigned          height,
	unsigned &        execSize,
	G4_RegVar* base
)
{
	unsigned message = 0;

	if( useScratchMsg_)
	{
		unsigned segmentByteSize = height * REG_BYTE_SIZE;
		unsigned responseLength = cdiv (segmentByteSize, REG_BYTE_SIZE);
		message = responseLength << getSendRspLengthBitOffset ();
		unsigned headerPresent = 0x80000;
		message |= SCRATCH_PAYLOAD_HEADER_MAX_HEIGHT << getSendMsgLengthBitOffset ();
	    message |= headerPresent;

		message |= (1 << SCRATCH_MSG_DESC_CATEORY);
		message |= (0 << SCRATCH_MSG_INVALIDATE_AFTER_READ);
        unsigned blocksize_encoding = getScratchBlocksizeEncoding(height);

		message |= (blocksize_encoding << SCRATCH_MSG_DESC_BLOCK_SIZE);

        int offset = getDisp(base);
        getSpillOffset(offset);
		message |= ((offset >> 5) + regOff);

		execSize = 16;
	}
	else
	{
		unsigned segmentByteSize = height * REG_BYTE_SIZE;
		unsigned statelessSurfaceIndex = 0xFF;
		unsigned responseLength = cdiv (segmentByteSize, REG_BYTE_SIZE);
		responseLength = responseLength << getSendRspLengthBitOffset ();
		message = statelessSurfaceIndex | responseLength;

		unsigned headerPresent = 0x80000;
	    message |= headerPresent;
        unsigned messageType = getSendOwordReadType ();
        message |= messageType << getSendReadTypeBitOffset ();
		unsigned messageLength = OWORD_PAYLOAD_HEADER_MIN_HEIGHT;
		message |= messageLength << getSendMsgLengthBitOffset ();
		unsigned segmentOwordSize =
			cdiv (segmentByteSize, OWORD_BYTE_SIZE);
		assert (segmentOwordSize <= 8);
		message |= blockSendBlockSizeCode (segmentOwordSize);
		execSize = LIMIT_SEND_EXEC_SIZE (segmentOwordSize * DWORD_BYTE_SIZE);
	}
	return builder_->createImm (message, Type_UD);
}

// Create the message description for a fill send instruction for filled
// source regions.

template <class REGION_TYPE>
G4_Imm *
SpillManagerGMRF::createFillSendMsgDesc (
	REGION_TYPE * filledRangeRegion,
	unsigned &    execSize
)
{
	unsigned message = 0;

	if( useScratchMsg_)
	{
		unsigned segmentByteSize =
			getSegmentByteSize (filledRangeRegion, execSize);
		if (filledRangeRegion->crossGRF()) {
			segmentByteSize = 2 * REG_BYTE_SIZE;
		}

		unsigned responseLength = cdiv (segmentByteSize, REG_BYTE_SIZE);
		message = responseLength << getSendRspLengthBitOffset ();

	    unsigned headerPresent = 0x80000;
		message |= headerPresent;

		message |= (SCRATCH_PAYLOAD_HEADER_MAX_HEIGHT << getSendMsgLengthBitOffset());
		message |= (1 << SCRATCH_MSG_DESC_CATEORY);
		message |= (0 << SCRATCH_MSG_INVALIDATE_AFTER_READ);
		// Scratch msg descriptor requires a special encoding for block size
		/*
		00 - 1 GRF
		01 - 2 GRFs
		10 - reserved
		11 - 4 GRFs
		*/
        unsigned blocksize_encoding = getScratchBlocksizeEncoding(responseLength);

		message |= (blocksize_encoding << SCRATCH_MSG_DESC_BLOCK_SIZE);
        int offset = getRegionDisp(filledRangeRegion);
        getSpillOffset(offset);
        message |= offset >> 5;

		execSize = 16;
	}
	else
	{
		unsigned segmentByteSize =
			getSegmentByteSize (filledRangeRegion, execSize);
		unsigned statelessSurfaceIndex = 0xFF;
		unsigned responseLength = cdiv (segmentByteSize, REG_BYTE_SIZE);
		responseLength = responseLength << getSendRspLengthBitOffset ();
		message = statelessSurfaceIndex | responseLength;

	    unsigned headerPresent = 0x80000;
		message |= headerPresent;
        unsigned messageType = getSendOwordReadType ();
        message |= messageType << getSendReadTypeBitOffset ();
		unsigned messageLength = OWORD_PAYLOAD_HEADER_MIN_HEIGHT;
		message |= messageLength << getSendMsgLengthBitOffset ();
		unsigned segmentOwordSize =
			cdiv (segmentByteSize, OWORD_BYTE_SIZE);
		message |= blockSendBlockSizeCode (segmentOwordSize);
		execSize = LIMIT_SEND_EXEC_SIZE (segmentOwordSize * DWORD_BYTE_SIZE);
	}
	return builder_->createImm (message, Type_UD);
}


// size -- number of GRFs to read
// offset -- in bytes (may be either in scratch or SLM)
G4_Imm* SpillManagerGMRF::createFillSendMsgDesc(
    bool doSLMFill,
    int size,
    int offset)
{
    uint32_t message = 0;
    if (doSLMFill)
    {
        if (builder_->hasBlockedSLMMessage())
        {
            // SLM Hword block read
            unsigned responseLength = size;
            unsigned SLMIndex = 0xFE;
            message |= SLMIndex;
            unsigned dataElements = getHWordEncoding(responseLength);
            message |= dataElements << 8;
            message |= 1 << 13; // HWord
            unsigned messageType = DC1_HWORD_ALIGNED_BLOCK_READ;
            message |= messageType << getSendReadTypeBitOffset();
            unsigned headerPresent = 0x80000;
            message |= headerPresent;
            unsigned messageLength = 1;
            message |= messageLength << getSendMsgLengthBitOffset();
            message |= responseLength << getSendRspLengthBitOffset();
        }
        else
        {
            // SLM untyped read
            message = getUntypedSLMMsgDesc(size, true);

        }
    }
    else if (useScratchMsg_)
    {
        unsigned responseLength = size;
        message = responseLength << getSendRspLengthBitOffset();

        unsigned headerPresent = 0x80000;
        message |= headerPresent;

        message |= (SCRATCH_PAYLOAD_HEADER_MAX_HEIGHT << getSendMsgLengthBitOffset());
        message |= (1 << SCRATCH_MSG_DESC_CATEORY);
        message |= (0 << SCRATCH_MSG_INVALIDATE_AFTER_READ);
        // Scratch msg descriptor requires a special encoding for block size
        /*
        00 - 1 GRF
        01 - 2 GRFs
        10 - reserved
        11 - 4 GRFs
        */
        unsigned blocksize_encoding = getScratchBlocksizeEncoding(responseLength);

        message |= (blocksize_encoding << SCRATCH_MSG_DESC_BLOCK_SIZE);
        message |= (offset >> 5);
    }
    else
    {
        MUST_BE_TRUE(false, "should not reach here");
    }
    return builder_->createImm(message, Type_UD);
}

// Create the send instruction to perform the fill of the spilled regvars's
// segment from spill memory.

G4_INST *
SpillManagerGMRF::createFillSendInstr (
	G4_Declare *	  fillRangeDcl,
	G4_Declare *	  mRangeDcl,
	unsigned          regOff,
	unsigned          height,
	unsigned		  srcRegOff
)
{
	unsigned execSize (0);

	G4_Imm * messageDescImm = NULL;

	if( useScratchMsg_)
	{
		G4_RegVar* r = fillRangeDcl->getRegVar();
		G4_RegVarTmp* rvar = static_cast<G4_RegVarTmp*> (r);
		messageDescImm =
			createFillSendMsgDesc (srcRegOff, height, execSize, rvar->getBaseRegVar());
#ifdef _DEBUG
        int offset = (messageDescImm->getInt() & 0xFFF) * GENX_GRF_REG_SIZ;
        MUST_BE_TRUE(offset >= globalScratchOffset, "incorrect offset");
#endif
	}
	else
	{
		messageDescImm =
			createFillSendMsgDesc (regOff, height, execSize);
	}

    G4_DstRegRegion * postDst = builder_->createDstRegRegion(
		Direct, fillRangeDcl->getRegVar (), (short) regOff, SUBREG_ORIGIN,
		DEF_HORIZ_STRIDE, (execSize > 8)? Type_UW: Type_UD);

    G4_SrcRegRegion * payload = builder_->createSrcRegRegion(Mod_src_undef, Direct,
        mRangeDcl->getRegVar(), 0, 0, builder_->getRegionStride1(), Type_UD);

	G4_DstRegRegion* dstOpnd = builder_->createDstRegRegion(Direct, fillRangeDcl->getRegVar(), 0, 0, 1, Type_UD);
	builder_->createInst(NULL, G4_pseudo_kill, NULL, false, 1, dstOpnd, NULL, NULL, 0);

	return createSendInst ((unsigned char) execSize, postDst, payload, messageDescImm, SFID_DP_DC, false, InstOpt_WriteEnable);
}

// Create the send instruction to perform the fill of the filled region's
// segment into fill memory.

G4_INST *
SpillManagerGMRF::createFillSendInstr (
	G4_Declare *      fillRangeDcl,
	G4_Declare *      mRangeDcl,
	G4_SrcRegRegion * filledRangeRegion,
	unsigned          execSize,
	unsigned          regOff
)
{
	G4_Imm * messageDescImm =
		createFillSendMsgDesc (filledRangeRegion, execSize);

#ifdef _DEBUG
    if (useScratchMsg_)
    {
        int offset = (messageDescImm->getInt() & 0xFFF) * GENX_GRF_REG_SIZ;
        MUST_BE_TRUE(offset >= globalScratchOffset, "incorrect offset");
    }
#endif


	if( useScratchMsg_)
	{
		execSize = 16;
	}

    G4_DstRegRegion * postDst = builder_->createDstRegRegion(
		Direct, fillRangeDcl->getRegVar (), (short) regOff, SUBREG_ORIGIN,
		DEF_HORIZ_STRIDE, (execSize > 8)? Type_UW : Type_UD);

    G4_SrcRegRegion * payload = builder_->createSrcRegRegion(Mod_src_undef, Direct,
        mRangeDcl->getRegVar(), 0, 0, builder_->getRegionStride1(), Type_UD);

	G4_DstRegRegion* dstOpnd = builder_->createDstRegRegion(Direct, fillRangeDcl->getRegVar(), 0, 0, 1, Type_UD);
	builder_->createInst(NULL, G4_pseudo_kill, NULL, false, 1, dstOpnd, NULL, NULL, 0);

	return createSendInst ((unsigned char) execSize, postDst, payload, messageDescImm, SFID_DP_DC, false, InstOpt_WriteEnable);
}

/// compute the 8/16 addresses for SLM spill/fill if untyped message is used
G4_Declare* SpillManagerGMRF::createSLMSpillAddr(int numReg, uint32_t spillOffset)
{
    // 
    // r1 <-- builtinSLMSpillAddr([0, 4, 8, ... 60] + perThreadSLMStart), created in prolog
    // r3 <-- builtinImmVector4([0, 4, 8, ... 60]), created in prolog
    // if numReg > 2:
    //      mad (16) r2:ud r1:uw r3:uw (numReg / 2 - 1) 
    // add (16) r2:ud r1:uw offset
    G4_Declare* SLMSpillBase = builder_->getBuiltinSLMSpillAddr();
    int numAddr = numReg == 1 ? 8 : 16;
    G4_Declare* sendSrc = builder_->createTempVar(numAddr, Type_UD, Either, Any);
    if (numReg > 2)
    {
        G4_SrcRegRegion *madSrc0 = builder_->Create_Src_Opnd_From_Dcl(SLMSpillBase, builder_->getRegionStride1());
        G4_SrcRegRegion *madSrc1 = builder_->Create_Src_Opnd_From_Dcl(builder_->getBuiltinImmVector4(), builder_->getRegionStride1());
        uint32_t numChannels = getChMaskForSpill(numReg).getNumEnabledChannels();
        G4_Imm* madSrc2 = builder_->createImm(numChannels - 1, Type_UW);
        G4_DstRegRegion *dst = builder_->Create_Dst_Opnd_From_Dcl(sendSrc, 1);
        builder_->createInst(nullptr, G4_mad, nullptr, false, numAddr, dst, madSrc0, madSrc1, madSrc2, InstOpt_WriteEnable);
    }
    G4_SrcRegRegion* src = builder_->Create_Src_Opnd_From_Dcl(numReg > 2 ? sendSrc : SLMSpillBase, builder_->getRegionStride1());
    G4_Imm* offset = builder_->createImm(spillOffset, Type_UW);
    G4_DstRegRegion* address = builder_->Create_Dst_Opnd_From_Dcl(sendSrc, 1);
    builder_->createInst(nullptr, G4_add, nullptr, false, numAddr, address, src, offset, InstOpt_WriteEnable);

    return sendSrc;
}

// create the fill send instruction (with optional header moves) and append them to the end
// of the global inst list. They will be inserted to the correct code position by the caller.
// @fillDcl -- destination of the fill
// @fillRegOff -- GRF offset of the fill dcl
// @size -- number of GRFs to read
// @logicalOffset -- the logical byte offset to read from. This will be translated to the
//                   actual address in SLM/scratch
void SpillManagerGMRF::createFill(
    G4_Declare* fillDcl,
    int fillRegOff,
    int size,
    int logicalOffset
    )
{

    int varOffset = logicalOffset;
    bool doSLMFill = getSpillOffset(varOffset);

    G4_Imm * messageDescImm =
        createFillSendMsgDesc(doSLMFill, size, varOffset);

    CISA_SHARED_FUNCTION_ID funcID = SFID_DP_DC;

    G4_Declare* sendSrc = nullptr;
    int esize = 16;

    if (doSLMFill && builder_->hasBlockedSLMMessage())
    {
        // create 1 GRF header
        // mov (1) H0.2<1>:ud offset:uw {NoMask}
        // also create a pseudo kill so that the header's life range is properly terminated
        sendSrc = builder_->createDeclareNoLookup("Fill_Header", G4_GRF, 8, 1, Type_UD);
        G4_DstRegRegion* psuedoKill = builder_->Create_Dst_Opnd_From_Dcl(sendSrc, 1);
        auto newInst = builder_->createInst(nullptr, G4_pseudo_kill, nullptr, false, 1, psuedoKill, nullptr, nullptr, InstOpt_NoOpt);
        newInst->setCISAOff(curInst->getCISAOff());
        G4_DstRegRegion* dst = builder_->createDstRegRegion(Direct, sendSrc->getRegVar(), 0, 2,
            1, Type_UD);
        G4_Imm* imm = builder_->createImm(varOffset, Type_UW);
        createMovInst(1, dst, imm);
        numGRFMove++;
    }
    else if (doSLMFill)
    {
        esize = size == 1 ? 8 : 16;
        sendSrc = createSLMSpillAddr(size, varOffset);

        funcID = SFID_DP_DC1;
    }
    else
    {
        sendSrc = builder_->getBuiltinR0();
    }
  
    assert(sendSrc != nullptr);
    // 
    // add pseudo-kill to limit fill dst's life range (why is this necessary?)
    G4_DstRegRegion* dstOpnd = builder_->createDstRegRegion(Direct, fillDcl->getRegVar(), 0, 0, 1, Type_UD);
    builder_->createInst(NULL, G4_pseudo_kill, NULL, false, 1, dstOpnd, NULL, NULL, 0);

    G4_DstRegRegion * postDst = builder_->createDstRegRegion(
        Direct, fillDcl->getRegVar(), (short)fillRegOff, 0, 1, Type_UW);
    G4_SrcRegRegion* payload = builder_->Create_Src_Opnd_From_Dcl(sendSrc, builder_->getRegionStride1());
    G4_INST* fillInst = createSendInst((unsigned char)esize, postDst, payload, messageDescImm, funcID, false, InstOpt_WriteEnable);
    fillInst->setSpillOrFill();
 
    numGRFFill++;
}


// Replace the reference to the spilled region with a reference to an
// equivalent reference to the spill range region.

void
SpillManagerGMRF::replaceSpilledRange (
	G4_Declare *      spillRangeDcl,
	G4_DstRegRegion * spilledRegion,
	G4_INST *         spilledInst
)
{
    // we need to preserve accRegSel if it's set
    G4_DstRegRegion * tmpRangeDstRegion = builder_->createDstRegRegion(
		Direct, spillRangeDcl->getRegVar (), REG_ORIGIN, SUBREG_ORIGIN,
		spilledRegion->getHorzStride (), spilledRegion->getType(), spilledRegion->getAccRegSel() );
	spilledInst->setDest (tmpRangeDstRegion);
}

// Replace the reference to the filled region with a reference to an
// equivalent reference to the fill range region.

void
SpillManagerGMRF::replaceFilledRange (
	G4_Declare *      fillRangeDcl,
	G4_SrcRegRegion * filledRegion,
	G4_INST *         filledInst
)
{
    unsigned execSize =
        isMultiRegComprSource (filledRegion, filledInst)?
        filledInst->getExecSize () / 2:
        filledInst->getExecSize ();
	G4_SrcRegRegion * fillRangeSrcRegion =
		createFillRangeSrcRegion (
			fillRangeDcl->getRegVar (), filledRegion, execSize);

	for (int i = 0; i < G4_MAX_SRCS; i++) {
		G4_SrcRegRegion * src =
			(G4_SrcRegRegion *) filledInst->getSrc(i);
		if (src != NULL && *src == *filledRegion)
			filledInst->setSrc (fillRangeSrcRegion, i);
	}
}

// Create the send instructions to write out the spillRangeDcl in aligned
// portions.
void
SpillManagerGMRF::sendOutSpilledRegVarPortions (
	G4_Declare *	  spillRangeDcl,
	G4_Declare *	  mRangeDcl,
	unsigned          regOff,
	unsigned          height,
	unsigned		  srcRegOff
)
{
    if (canDoSLMSpill())
    {
        // break spills into 8/4/2/1 chunks
        int offset = 0;
        G4_RegVar* r = spillRangeDcl->getRegVar();
        G4_RegVarTmp* rvar = static_cast<G4_RegVarTmp*> (r);
        int memOffset = getDisp(rvar->getBaseRegVar()) & GRF_ALIGN_MASK;
        while (height > 0)
        {
            int size = getNextSize(height, true);
            memOffset += offset * 32;
            createSpill(spillRangeDcl, offset, size, memOffset, InstOpt_WriteEnable, 16);
            height -= size;
            offset += size;
        }
        return;
    }
	if( useScratchMsg_)
	{
		// No need to make a copy of offset because when using
		// scratch msg descriptor, the offset is part of send
		// msg descriptor and not the header.
	}
	else
	{
		// Initialize the message header with the spill disp for portion. 
        int offset = getDisp(spillRangeDcl->getRegVar()) + regOff * REG_BYTE_SIZE;
        getSpillOffset(offset);
		unsigned segmentDisp = offset / OWORD_BYTE_SIZE;

		G4_Imm * segmentDispImm = builder_->createImm (segmentDisp, Type_UD);
		G4_DstRegRegion * mHeaderOffsetDstRegion =
			createMHeaderBlockOffsetDstRegion (mRangeDcl->getRegVar ());

		if (builder_->getIsKernel() == false &&
			getReprRegVar(spillRangeDcl->getRegVar())->getDeclare()->getHasFileScope() == false)
		{
			createAddFPInst (
				SCALAR_EXEC_SIZE, mHeaderOffsetDstRegion, segmentDispImm);
		}
		else
		{
			createMovInst (SCALAR_EXEC_SIZE, mHeaderOffsetDstRegion, segmentDispImm);
		}
    	numGRFMove ++;
	}


	// Write out the portions using a greedy approach.
    int currentStride = getNextSize(height, useScratchMsg_);

	if (currentStride)
	{
    	initMWritePayload (spillRangeDcl, mRangeDcl, regOff, currentStride);
    	createSpillSendInstr (spillRangeDcl, mRangeDcl, regOff, currentStride, srcRegOff);
    	numGRFSpill++;

    	if (height - currentStride > 0) {
	    	sendOutSpilledRegVarPortions (
		    	spillRangeDcl, mRangeDcl, regOff + currentStride, height - currentStride, srcRegOff + currentStride);
    	}
	}
}

// Create the code to create the spill range and save it to spill memory.

INST_LIST::iterator
SpillManagerGMRF::insertSpillRangeCode (
	G4_DstRegRegion *   spilledRegion,
	INST_LIST::iterator spilledInstIter,
	INST_LIST &         instList
)
{
	unsigned char execSize = (*spilledInstIter)->getExecSize ();
	G4_Declare * replacementRangeDcl;
	builder_->instList.clear();

    bool optimizeSplitLLR = false;
    G4_INST* inst = *spilledInstIter;
    G4_INST* spillSendInst = NULL;

	// Handle send instructions (special treatment)
	// Create the spill range for the whole post destination, assign spill
	// offset to the spill range and create the instructions to load the
	// save the spill range to spill memory.

	if ((*spilledInstIter)->isSend ()) {
		INST_LIST::iterator sendOutIter = spilledInstIter;
		assert (getRFType (spilledRegion) == G4_GRF);
		G4_Declare * spillRangeDcl =
			createPostDstSpillRangeDeclare (*sendOutIter, spilledRegion);
		G4_Declare * mRangeDcl =
			createAndInitMHeader (
				(G4_RegVarTransient *) spillRangeDcl->getRegVar ());

		sendInSpilledRegVarPortions (
			spillRangeDcl, mRangeDcl, 0,
			spillRangeDcl->getNumRows (),
			spilledRegion->getRegOff());

		INST_LIST::iterator insertPos = sendOutIter;
		instList.splice (insertPos, builder_->instList);

		sendOutSpilledRegVarPortions (
			spillRangeDcl, mRangeDcl, 0, spillRangeDcl->getNumRows (),
			spilledRegion->getRegOff());

		replacementRangeDcl = spillRangeDcl;
	}

	// Handle other regular single/multi destination register instructions.
	// Create the spill range for the destination region, assign spill
	// offset to the spill range and create the instructions to load the
	// save the spill range to spill memory.
    else {
		// Create the segment aligned spill range

		G4_Declare * spillRangeDcl =
            createSpillRangeDeclare (
                spilledRegion, execSize, 
				*spilledInstIter);

		// Create and initialize the message header

		G4_Declare * mRangeDcl =
			createAndInitMHeader (spilledRegion, execSize);

		// Unaligned region specific handling.

		unsigned int spillSendOption = InstOpt_WriteEnable;
		if (shouldPreloadSpillRange (
				spilledRegion, execSize, *spilledInstIter)) {

			// Preload the segment aligned spill range from memory to use
			// as an overlay

			preloadSpillRange (
				spillRangeDcl, mRangeDcl, spilledRegion, execSize);

			// Create the temporary range to use as a replacement range.

			G4_Declare * tmpRangeDcl =
				createTemporaryRangeDeclare (spilledRegion, execSize);

			// Copy out the value in the temporary range into its
			// location in the spill range.

			G4_DstRegRegion * spillRangeDstRegion =
				createSpillRangeDstRegion (
					spillRangeDcl->getRegVar (), spilledRegion, execSize);

			G4_SrcRegRegion * tmpRangeSrcRegion =
				createTemporaryRangeSrcRegion (
					tmpRangeDcl->getRegVar (), spilledRegion, execSize);

			// NOTE: Never use a predicate for the final mov if the spilled
			//       instruction was a sel (even in a SIMD CF context).

			G4_Predicate* predicate =
				((*spilledInstIter)->opcode() != G4_sel)?
				(*spilledInstIter)->getPredicate () : nullptr;
			createMovInst (
				execSize, spillRangeDstRegion, tmpRangeSrcRegion,
				predicate != nullptr ? builder_->duplicateOperand(predicate) : predicate,
				(*spilledInstIter)->getMaskOption());
			numGRFMove ++;

			replacementRangeDcl = tmpRangeDcl;
		}

		// Aligned regions do not need a temporary range.

		else {
            LocalLiveRange* spilledLLR = gra.getLocalLR(spilledRegion->getBase()->asRegVar()->getDeclare());
            if (!canDoSLMSpill() && spilledLLR && spilledLLR->getSplit())
            {
                // if we are spilling the dest of a copy move introduced by local live-range splitting,
                // we can spill the source value instead and delete the move
                // ToDo: we should generalize this to cover all moves
                G4_SrcRegRegion* srcRegion = inst->getSrc(0)->asSrcRegRegion();
                G4_Declare* srcDcl = srcRegion->getBase()->asRegVar()->getDeclare();
                unsigned int lb = srcRegion->getLeftBound();
                unsigned int rb = srcRegion->getRightBound();

                G4_RegVar * regVar = NULL;
                if (srcRegion->getBase()->isRegVar())
                {
                    regVar = getRegVar(srcRegion);
                }

                if (srcDcl->getSubRegAlign() == Sixteen_Word &&
                    lb %  REG_BYTE_SIZE == 0 &&
                    (rb + 1) % REG_BYTE_SIZE == 0 &&
                    (rb - lb + 1) == spillRangeDcl->getByteSize() &&
                    regVar &&
                    !shouldSpillRegister(regVar))
                {
                    optimizeSplitLLR = true;
                }
            }

			replacementRangeDcl = spillRangeDcl;
			if (inSIMDCFContext_)
			{
				spillSendOption = (*spilledInstIter)->getMaskOption();
			}
		}

		// Save the spill range to memory.

        if (canDoSLMSpill())
        {
            int offset = getRegionDisp(spilledRegion) & GRF_ALIGN_MASK;
            int numGRF = spilledRegion->crossGRF() ? 2 : 1;
            createSpill(spillRangeDcl, 0, numGRF, offset, spillSendOption, execSize);
        }
        else
        {
            initMWritePayload(
                spillRangeDcl, mRangeDcl, spilledRegion, execSize);
            spillSendInst = createSpillSendInstr(
                spillRangeDcl, mRangeDcl, spilledRegion, execSize, spillSendOption);
            numGRFSpill++;
        }
        if (failSafeSpill_)
        {
            spillRegOffset_ = spillRegStart_;
        }
	}

	// Replace the spilled range with the spill range and insert spill
	// instructions.

	INST_LIST::iterator insertPos = spilledInstIter;
	insertPos++;
	replaceSpilledRange (replacementRangeDcl, spilledRegion, *spilledInstIter);
	INST_LIST::iterator nextIter = spilledInstIter;
	++nextIter;

	instList.splice (insertPos, builder_->instList);

    if (optimizeSplitLLR && spillSendInst && spillSendInst->isSplitSend())
    {
        // delete the move and spill the source instead. Note that we can't do this if split send 
        // is not enabled, as payload contains header 
        instList.erase(spilledInstIter);
        unsigned int pos = 1;
        spillSendInst->setSrc(inst->getSrc(0), pos);
    }
    else
    {
        INST_LIST::iterator pseudoKillPos = spilledInstIter;
        G4_DstRegRegion* dstOpnd = builder_->createDstRegRegion(Direct, replacementRangeDcl->getRegVar(), 0, 0, 1, Type_UD);
        auto newInst = builder_->createInst(NULL, G4_pseudo_kill, NULL, false, 1, dstOpnd, NULL, NULL, 0);
        newInst->setCISAOff(curInst->getCISAOff());
        instList.splice(pseudoKillPos, builder_->instList);
    }

	return nextIter;
}

// Create the code to create the GRF fill range and load it to spill memory.

INST_LIST::iterator
SpillManagerGMRF::insertFillGRFRangeCode (
	G4_SrcRegRegion *   filledRegion,
	INST_LIST::iterator filledInstIter,
	INST_LIST &         instList
)
{
	unsigned  execSize = (*filledInstIter)->getExecSize ();

	// Create the fill range, assign spill offset to the fill range and
	// create the instructions to load the fill range from spill memory.

	G4_Declare * fillRangeDcl = nullptr;

    bool optimizeSplitLLR = false;
    G4_INST* inst = *filledInstIter;
    G4_DstRegRegion* dstRegion = inst->getDst()->asDstRegRegion();
    G4_INST* fillSendInst = NULL;

    if (canDoSLMSpill())
    {
        // we fill either 1 or 2 GRF here
        //FIXME: do we need to explicitly mark this as a fill var?
        int dclRows = filledRegion->crossGRF() ? 2 : 1;
        int offset = getRegionDisp(filledRegion) & GRF_ALIGN_MASK;
        G4_RegVar * filledRegVar = getRegVar(filledRegion);
        const char* name = createImplicitRangeName("FL", filledRegVar, getFillIndex(filledRegVar));
        fillRangeDcl = builder_->createDeclareNoLookup(name, G4_GRF, 8, (unsigned short)dclRows, Type_UD);
        createFill(fillRangeDcl, 0, dclRows, offset);
    }
    else
    {
        fillRangeDcl =
            createGRFFillRangeDeclare(
                filledRegion, execSize,  
                *filledInstIter);
        G4_Declare * mRangeDcl =
            createAndInitMHeader(filledRegion, execSize);
        numGRFFill++;
        fillSendInst = createFillSendInstr(fillRangeDcl, mRangeDcl, filledRegion, execSize);

        LocalLiveRange* filledLLR = gra.getLocalLR(filledRegion->getBase()->asRegVar()->getDeclare());
        if (filledLLR && filledLLR->getSplit())
        {
            G4_Declare* dstDcl = dstRegion->getBase()->asRegVar()->getDeclare();
            unsigned int lb = dstRegion->getLeftBound();
            unsigned int rb = dstRegion->getRightBound();

            if (dstDcl->getSubRegAlign() == Sixteen_Word &&
                lb %  REG_BYTE_SIZE == 0 &&
                (rb + 1) % REG_BYTE_SIZE == 0 &&
                (rb - lb + 1) == fillRangeDcl->getByteSize())
            {
                optimizeSplitLLR = true;
            }
        }
    }

	// Replace the spilled range with the fill range and insert spill
	// instructions.

	replaceFilledRange (fillRangeDcl, filledRegion, *filledInstIter);
	INST_LIST::iterator insertPos = filledInstIter;

	instList.splice (insertPos, builder_->instList);
    if (optimizeSplitLLR)
    {
        INST_LIST::iterator nextIter = filledInstIter;
        INST_LIST::iterator prevIter = filledInstIter;
        nextIter++;
        prevIter--;
        prevIter--;
        instList.erase(filledInstIter);
        fillSendInst->setDest(dstRegion);
        G4_INST* prevInst = (*prevIter);
        if (prevInst->opcode() == G4_pseudo_kill &&
            GetTopDclFromRegRegion(prevInst->getDst()) == fillRangeDcl)
        {
            prevInst->setDest(builder_->createDstRegRegion(Direct, GetTopDclFromRegRegion(dstRegion)->getRegVar(), 0, 0, 1, Type_UD));
        }
        return nextIter;
    }
    else
    {
        return ++filledInstIter;
    }
}

// Create the code to create the MRF fill range and load it to spill memory.

INST_LIST::iterator
SpillManagerGMRF::insertFillMRFRangeCode (
	G4_SrcRegRegion *   filledRegion,
	INST_LIST::iterator filledInstIter,
	INST_LIST &         instList
)
{
	G4_INST * sendInst = *filledInstIter;

	unsigned width = REG_BYTE_SIZE / filledRegion->getElemSize();

	// Create the fill range, assign spill offset to the fill range

	G4_Declare * fillMRFRangeDcl =
		createMRFFillRangeDeclare(filledRegion, sendInst);

	// Create the instructions to load the fill range from spill memory.

	G4_Declare * mRangeDcl = createMRangeDeclare(filledRegion, width);
	initMHeader(mRangeDcl);
	sendInSpilledRegVarPortions(
		fillMRFRangeDcl, mRangeDcl, 0,
		fillMRFRangeDcl->getNumRows(), filledRegion->getRegOff());

	// Replace the spilled range with the fill range and insert spill
	// instructions.

	replaceFilledRange(fillMRFRangeDcl, filledRegion, *filledInstIter);
	INST_LIST::iterator insertPos = filledInstIter;

	instList.splice(insertPos, builder_->instList);

	// Return the next instruction

	return ++filledInstIter;
}

G4_Declare* getOrCreateSpillFillDcl(G4_Declare* spilledAddrTakenDcl, G4_Kernel* kernel)
{
    // If spilledAddrTakenDcl already has a spill/fill range created, return it.
    // Else create new one and return it.
    G4_Declare* temp = spilledAddrTakenDcl->getAddrTakenSpillFill();
    if (temp == NULL)
    {
#define ADDR_SPILL_FILL_NAME_SIZE 32
        char* dclName = kernel->fg.builder->getNameString(kernel->fg.mem, ADDR_SPILL_FILL_NAME_SIZE,
            "ADDR_SP_FL_V%d", spilledAddrTakenDcl->getDeclId());

        // temp is created of sub-class G4_RegVarTmp so that is
        // assigned infinite spill cost when coloring.
        temp = kernel->fg.builder->createDeclareNoLookup((const char*)dclName,
            G4_GRF, spilledAddrTakenDcl->getNumElems(),
            spilledAddrTakenDcl->getNumRows(), spilledAddrTakenDcl->getElemType() , DeclareType::Tmp, spilledAddrTakenDcl->getRegVar());
        spilledAddrTakenDcl->setAddrTakenSpillFill(temp);
    }

    return temp;
}

// For each address taken register spill find an available physical register
// and assign it to the decl. This physical register will be used for inserting
// spill/fill code for indirect reference instructions that point to the
// spilled range.
// Return true if enough registers found, false if sufficient registers unavailable.
bool SpillManagerGMRF::handleAddrTakenSpills( G4_Kernel * kernel, PointsToAnalysis& pointsToAnalysis )
{
	bool success = true;
    unsigned int numAddrTakenSpills = 0;

	for (LR_LIST::const_iterator lt = spilledLRs_.begin ();
		lt != spilledLRs_.end (); ++lt)
	{
		LiveRange* lr = (*lt);

        if( lr->getVar()->getDeclare()->getAddressed() )
        {
            getOrCreateSpillFillDcl(lr->getVar()->getDeclare(), kernel);
        }

		if( lvInfo_->isAddressSensitive( lr->getVar()->getId() ) )
		{
            numAddrTakenSpills++;
		}
	}

	if(numAddrTakenSpills > 0)
	{
		insertAddrTakenSpillFill( kernel, pointsToAnalysis );
		prunePointsTo( kernel, pointsToAnalysis );
	}

#ifdef _DEBUG
    if( success )
    {
    	// Verify that each spilled address taken has a spill/fill registers assigned
	    for (LR_LIST::const_iterator lt = spilledLRs_.begin ();
		    lt != spilledLRs_.end (); ++lt)
    	{
	    	if( (*lt)->getVar()->getDeclare()->getAddressed() )
		    	MUST_BE_TRUE( (*lt)->getVar()->getDeclare()->getAddrTakenSpillFill() != NULL, "Spilled addr taken does not have assigned spill/fill GRF");
    	}
    }
#endif

	return success;
}

// Insert spill and fill code for indirect GRF accesses
void SpillManagerGMRF::insertAddrTakenSpillAndFillCode( G4_Kernel* kernel, INST_LIST& instList, INST_LIST::iterator inst_it, G4_Operand* opnd, PointsToAnalysis& pointsToAnalysis, bool spill, unsigned int bbid )
{
    curInst = (*inst_it);
	INST_LIST::iterator next_inst_it = ++inst_it;
	inst_it--;

	// Check whether spill operand points to any spilled range
	for (LR_LIST::const_iterator lr_it = spilledLRs_.begin ();
		lr_it != spilledLRs_.end (); ++lr_it) {
		LiveRange* lr = (*lr_it);
		G4_RegVar* var = NULL;

		if( opnd->isDstRegRegion() && opnd->asDstRegRegion()->getBase()->asRegVar() )
			var = opnd->asDstRegRegion()->getBase()->asRegVar();

		if( opnd->isSrcRegRegion() && opnd->asSrcRegRegion()->getBase()->asRegVar() )
			var = opnd->asSrcRegRegion()->getBase()->asRegVar();

		MUST_BE_TRUE( var != NULL, "Fill operand is neither a source nor dst region");

		if( var &&
            pointsToAnalysis.isPresentInPointsTo( var,
			lr->getVar() ) )
		{
			unsigned int numrows = lr->getVar()->getDeclare()->getNumRows();
            G4_Declare* temp = getOrCreateSpillFillDcl(lr->getVar()->getDeclare(), kernel);

			if (failSafeSpill_ && 
				temp->getRegVar()->getPhyReg() == NULL)
			{
				temp->getRegVar()->setPhyReg(builder_->phyregpool.getGreg(spillRegOffset_), 0);
				spillRegOffset_ += numrows;
			}

            G4_DstRegRegion* dstOpnd = builder_->createDstRegRegion(Direct, temp->getRegVar(), 0, 0, 1, Type_UD);
            auto newInst = builder_->createInternalInst(NULL, G4_pseudo_kill, NULL, false, 1, dstOpnd, NULL, NULL, 0);
            instList.insert(inst_it, newInst);

			if( numrows > 1 || (lr->getVar()->getDeclare()->getNumElems() * lr->getVar()->getDeclare()->getElemSize() == 32) )
			{
	            if (useScratchMsg_ || useSplitSend())
	            {
	                G4_Declare * fillMRFRangeDcl = temp;
	                G4_Declare * mRangeDcl =
	                    createAndInitMHeader(
	                    (G4_RegVarTransient *)temp->getRegVar()->getBaseRegVar());

	                sendInSpilledRegVarPortions(
	                    fillMRFRangeDcl, mRangeDcl, 0,
	                    temp->getNumRows(), 0);

	                instList.splice(inst_it, builder_->instList);

	                if (spill)
	                {
						sendOutSpilledRegVarPortions (
	                        temp, mRangeDcl, 0, temp->getNumRows(),
							0);

	                    instList.splice(next_inst_it, builder_->instList);
	                }
	            }
	            else
	            {

					for( unsigned int i = 0; i < numrows; i++ )
					{
						G4_INST* inst;
						RegionDesc* rd = kernel->fg.builder->getRegionStride1();
						unsigned char curExSize = 8;

						if( (i + 1 ) < numrows )
							curExSize = 16;

	                    G4_SrcRegRegion* srcRex = kernel->fg.builder->createSrcRegRegion(Mod_src_undef, Direct, lr->getVar(), (short)i, 0, rd, Type_F);

	                    G4_DstRegRegion* dstRex = kernel->fg.builder->createDstRegRegion(Direct, temp->getRegVar(), (short)i, 0, 1, Type_F);

						inst = kernel->fg.builder->createInternalInst( NULL, G4_mov, NULL, false, curExSize,
								dstRex, srcRex, NULL, InstOpt_WriteEnable, curInst->getLineNo(), curInst->getCISAOff(), curInst->getSrcFilename() );

						instList.insert( inst_it, inst );

						if( spill )
						{
							// Also insert spill code
	                        G4_SrcRegRegion* srcRex = kernel->fg.builder->createSrcRegRegion(Mod_src_undef, Direct, temp->getRegVar(), (short)i, 0, rd, Type_F);

	                        G4_DstRegRegion* dstRex = kernel->fg.builder->createDstRegRegion(Direct, lr->getVar(), (short)i, 0, 1, Type_F);

							inst = kernel->fg.builder->createInternalInst( NULL, G4_mov, NULL, false, curExSize,
									dstRex, srcRex, NULL, InstOpt_WriteEnable, curInst->getLineNo(), curInst->getCISAOff(), curInst->getSrcFilename() );

							instList.insert( next_inst_it, inst );
						}

						// If 2 rows were processed then increment induction var suitably
						if(	curExSize == 16 )
							i++;
					}
                }

                // Update points to
                // Note: points2 set should be updated after inserting fill code,
                // however, this sets a bit in liveness bit-vector that
                // causes the temp variable to be marked as live-out from
                // that BB. A general fix should treat address taken variables
                // more accurately wrt liveness so they dont escape via
                // unfeasible paths.
				//pointsToAnalysis.addFillToPointsTo( bbid, var, temp->getRegVar() );
			}
			else if( numrows == 1 )
			{
				// Insert spill/fill when there decl uses a single row, that too not completely
				unsigned char curExSize = 16;
				unsigned short numbytes = lr->getVar()->getDeclare()->getNumElems() * lr->getVar()->getDeclare()->getElemSize();

				//temp->setAddressed();
				short off = 0;

				while( numbytes > 0 )
				{
					G4_INST* inst;
					G4_Type type = Type_W;

					if( numbytes >= 16 )
						curExSize = 8;
					else if( numbytes >= 8 && numbytes < 16 )
						curExSize = 4;
					else if( numbytes >= 4 && numbytes < 8 )
						curExSize = 2;
					else if( numbytes >= 2 && numbytes < 4 )
						curExSize = 1;
					else if( numbytes == 1 )
					{
						// If a region has odd number of bytes, copy last byte in final iteration
						curExSize = 1;
						type = Type_UB;
					}
					else {
						MUST_BE_TRUE( false, "Cannot emit SIMD1 for byte");
						curExSize = 0;
					}

					RegionDesc* rd = kernel->fg.builder->getRegionStride1();

                    G4_SrcRegRegion* srcRex = kernel->fg.builder->createSrcRegRegion(Mod_src_undef, Direct, lr->getVar(), 0, off, rd, type);

                    G4_DstRegRegion* dstRex = kernel->fg.builder->createDstRegRegion(Direct, temp->getRegVar(), 0, off, 1, type);

					inst = kernel->fg.builder->createInternalInst( NULL, G4_mov, NULL, false, curExSize,
							dstRex, srcRex, NULL, InstOpt_WriteEnable, curInst->getLineNo(), curInst->getCISAOff(), curInst->getSrcFilename() );

					instList.insert( inst_it, inst );

					if( spill )
					{
						// Also insert spill code
                        G4_SrcRegRegion* srcRex = kernel->fg.builder->createSrcRegRegion(Mod_src_undef, Direct, temp->getRegVar(), 0, off, rd, type);

                        G4_DstRegRegion* dstRex = kernel->fg.builder->createDstRegRegion(Direct, lr->getVar(), 0, off, 1, type);

						inst = kernel->fg.builder->createInternalInst( NULL, G4_mov, NULL, false, curExSize,
								dstRex, srcRex, NULL, InstOpt_WriteEnable, curInst->getLineNo(), curInst->getCISAOff(), curInst->getSrcFilename() );

						instList.insert( next_inst_it, inst );
					}

					off += curExSize;
					numbytes -= curExSize*2;
				}

				// Update points to
				//pointsToAnalysis.addFillToPointsTo( bbid, var, temp->getRegVar() );
			}

            if (!spill)
            {
                // Insert pseudo_use node so that liveness keeps the
                // filled variable live through the indirect access.
                // Not required for spill because for spill we will
                // anyway insert a ues of the variable to emit store.
                RegionDesc* rd = kernel->fg.builder->getRegionScalar();

                G4_SrcRegRegion* pseudoUseSrc = kernel->fg.builder->createSrcRegRegion(Mod_src_undef, Direct, temp->getRegVar(),
                    0, 0, rd, Type_F);

                G4_INST* pseudoUseInst = kernel->fg.builder->createInternalIntrinsicInst(nullptr, Intrinsic::Use, 1, nullptr, pseudoUseSrc, nullptr, nullptr, InstOpt_NoOpt);

                instList.insert(next_inst_it, pseudoUseInst);
            }

		}
	}
}

// Insert any spill/fills for address taken
void SpillManagerGMRF::insertAddrTakenSpillFill( G4_Kernel* kernel, PointsToAnalysis& pointsToAnalysis )
{
	for( BB_LIST_ITER bb_it = kernel->fg.BBs.begin();
		bb_it != kernel->fg.BBs.end();
		bb_it++ )
	{
		G4_BB* bb = (*bb_it);

		for( INST_LIST_ITER inst_it = bb->instList.begin();
			inst_it != bb->instList.end();
			inst_it++ )
		{
			G4_INST* curInst = (*inst_it);

			if (failSafeSpill_)
			{
				spillRegOffset_ = indrSpillRegStart_;
			}

			// Handle indirect destination
			G4_DstRegRegion* dst = curInst->getDst();

			if( dst && dst->getRegAccess() == IndirGRF )
			{
				insertAddrTakenSpillAndFillCode( kernel, bb->instList, inst_it, dst, pointsToAnalysis, true, bb->getId() );
			}

			for( int i = 0; i < G4_MAX_SRCS; i++ )
			{
				G4_Operand* src = curInst->getSrc(i);

				if( src && src->isSrcRegRegion() && src->asSrcRegRegion()->getRegAccess() == IndirGRF )
				{
					insertAddrTakenSpillAndFillCode( kernel, bb->instList, inst_it, src, pointsToAnalysis, false, bb->getId() );
				}
			}
		}
	}
}

// For address spill/fill code inserted remove from points of each indirect operand
// the original regvar that is spilled.
void SpillManagerGMRF::prunePointsTo( G4_Kernel* kernel, PointsToAnalysis& pointsToAnalysis )
{
	for( BB_LIST_ITER bb_it = kernel->fg.BBs.begin();
	bb_it != kernel->fg.BBs.end();
	bb_it++ )
	{
		G4_BB* bb = (*bb_it);

		for( INST_LIST_ITER inst_it = bb->instList.begin();
			inst_it != bb->instList.end();
			inst_it++ )
		{
			G4_INST* curInst = (*inst_it);
			std::stack<G4_Operand*> st;

			// Handle indirect destination
			G4_DstRegRegion* dst = curInst->getDst();

			if( dst && dst->getRegAccess() == IndirGRF )
			{
				st.push( dst );
			}

			for( int i = 0; i < G4_MAX_SRCS; i++ )
			{
				G4_Operand* src = curInst->getSrc(i);

				if( src && src->isSrcRegRegion() && src->asSrcRegRegion()->getRegAccess() == IndirGRF )
				{
					st.push( src );
				}
			}

			while (st.size() > 0 )
			{
				G4_Operand* cur = st.top();
				st.pop();

				// Check whether spill operand points to any spilled range
				for (LR_LIST::const_iterator lr_it = spilledLRs_.begin ();
				lr_it != spilledLRs_.end (); ++lr_it) {
					LiveRange* lr = (*lr_it);
					G4_RegVar* var = NULL;

					if( cur->isDstRegRegion() && cur->asDstRegRegion()->getBase()->asRegVar() )
						var = cur->asDstRegRegion()->getBase()->asRegVar();

					if( cur->isSrcRegRegion() && cur->asSrcRegRegion()->getBase()->asRegVar() )
						var = cur->asSrcRegRegion()->getBase()->asRegVar();

					MUST_BE_TRUE( var != NULL, "Operand is neither a source nor dst region");

					if( var &&
                        pointsToAnalysis.isPresentInPointsTo( var,
						lr->getVar() ) )
					{
						// Remove this from points to
						pointsToAnalysis.removeFromPointsTo( var, lr->getVar() );
					}
				}
			}
		}
	}
}

// Insert spill/fill code for all registers that have not been assigned
// physical registers in the current iteration of the graph coloring
// allocator.
// returns false if spill fails somehow

bool
SpillManagerGMRF::insertSpillFillCode (
	G4_Kernel * kernel, PointsToAnalysis& pointsToAnalysis
)
{

    auto refCount = [](LiveRange* l1, LiveRange* l2) { return l1->getRefCount() > l2->getRefCount(); };
    if (canDoSLMSpill())
    {
        // sort the spill LRs based on their reference count so that the more expensives ones
        // will land in SLM
        spilledLRs_.sort(refCount);
    }

	// Set the spill flag of all spilled regvars.
	for (LR_LIST::const_iterator lt = spilledLRs_.begin ();
		lt != spilledLRs_.end (); ++lt) {

        G4_Declare *dcl = (*lt)->getVar()->getDeclare();
        if (dcl->getIsSplittedDcl())
        {
            dcl->setIsSplittedDcl(false);
            gra.clearSubDcl(dcl);
        }
		// Ignore request to spill/fill the spill/fill ranges
		// as it does not help the allocator.
		if (shouldSpillRegister ((*lt)->getVar ()) == false)
        {
            bool needsEOTGRF = (*lt)->getEOTSrc() && builder_->hasEOTGRFBinding();
            if (failSafeSpill_ && needsEOTGRF &&
                ((*lt)->getVar()->isRegVarTransient() ||
                 (*lt)->getVar()->isRegVarTmp()))
            {
                (*lt)->getVar()->setPhyReg(builder_->phyregpool.getGreg(spillRegStart_ > (builder_->getOptions()->getuInt32Option(vISA_TotalGRFNum) - 16) ? spillRegStart_ : (builder_->getOptions()->getuInt32Option(vISA_TotalGRFNum) - 16)), 0);
                continue;
            }
            else if (lvInfo_->isAddressSensitive((*lt)->getVar()->getId())) {
				DEBUG_MSG("Register allocation warning: Spilling of variable("
					 << (*lt)->getVar ()->getDeclare ()->getName()
					 << ") whose address is taken!"
					 << endl);
			}
			else {
				DEBUG_MSG("Register allocation warning: Spilling infinite live range ("
					 << (*lt)->getVar ()->getDeclare ()->getName()
					 << ")!"
					 << endl);

			}
			return false;
		}
		else
        {
			(*lt)->getVar ()->getDeclare ()->setSpillFlag ();
            if (canDoSLMSpill())
            {
                getDisp((*lt)->getVar());
            }
		}
	}

	// Handle address taken spills
	bool success = handleAddrTakenSpills( kernel, pointsToAnalysis );

	if( !success )
	{
		DEBUG_MSG( "Enough physical register not available for handling address taken spills" << std::endl );
		return false;
	}

	// Insert spill/fill code for all basic blocks.

	FlowGraph& fg = kernel->fg;

	for (BB_LIST_ITER it = fg.BBs.begin(); it != fg.BBs.end(); it++)
	{
		inSIMDCFContext_ = (*it)->isInSimdFlow();
		bbId_ = (*it)->getId();
		INST_LIST::iterator jt = (*it)->instList.begin ();

		while (jt != (*it)->instList.end ()) {
			INST_LIST::iterator kt = jt;
			++kt;
			G4_INST * inst = *jt;

            curInst = inst;

            if (failSafeSpill_)
            {
                spillRegOffset_ = spillRegStart_;
            }

			// Insert spill code, when the target is a spilled register.

            if (inst->getDst())
            {
                G4_RegVar * regVar = NULL;
                if( inst->getDst()->getBase()->isRegVar() )
                {
					regVar = getRegVar (inst->getDst());
                }


				if (regVar && shouldSpillRegister (regVar))
                {
					if (getRFType (regVar) == G4_GRF)
                    {
						if(inst->isPseudoKill())
						{
							(*it)->instList.erase(jt);
							jt = kt;
							continue;
						}

						insertSpillRangeCode (
							inst->getDst ()->asDstRegRegion (),	jt,
							(*it)->instList);
					}
					else
                    {
						assert (0);
					}
				}
			}


			// Insert fill code, when the source is a spilled register.

			for (unsigned i = 0; i < G4_MAX_SRCS; i++)
            {
				if (inst->getSrc (i) &&
					inst->getSrc (i)->isSrcRegRegion ())
                {
					G4_RegVar * regVar = NULL;
                    if( inst->getSrc(i)->asSrcRegRegion()->getBase()->isRegVar() )
                    {
                        regVar = getRegVar (inst->getSrc (i)->asSrcRegRegion ());
                    }


					if (regVar && shouldSpillRegister (regVar))
					{
						if(inst->isLifeTimeEnd())
						{
							(*it)->instList.erase(jt);
							break;
						}
						if ((inst->isSend() && i == 0) ||
                            (inst->isSplitSend() && i == 1)) {
                            // treat it as MRF since we may need to spill >2 GRFs
							insertFillMRFRangeCode (
								inst->getSrc (i)->asSrcRegRegion (), jt,
								(*it)->instList);
						}
						else if (getRFType (regVar) == G4_GRF)
							insertFillGRFRangeCode (
								inst->getSrc (i)->asSrcRegRegion (), jt,
								(*it)->instList);
						else
							assert (0);
					}
				}
			}

			jt = kt;
		}
	}

	bbId_ = UINT_MAX;
	inSIMDCFContext_ = false;

	// Calculate the spill memory used in this iteration

	for (auto spill : spilledLRs_)
    {
		unsigned disp = spill->getVar ()->getDisp ();

		if (spill->getVar ()->isSpilled ()) 
        {
            if (disp != UINT_MAX)
			{
                nextSpillOffset_ = std::max(nextSpillOffset_, disp + getByteSize(spill->getVar()));
			}
		}
	}

	// Verify the spill memory assignments for spill ranges introduced

#if defined (_DEBUG) || defined (VERIFY_SPILL_ASSIGNMENTS)

	LIVERANGE_LIST::const_iterator kt = spilledLRs_.begin ();
	LIVERANGE_LIST::const_iterator ktEnd = spilledLRs_.end ();

	for (; kt != ktEnd; ++kt) {
		if ((*kt)->getVar ()->isSpilled () == false) continue;
		G4_RegVar * sRange1 = getReprRegVar ((*kt)->getVar ());
		unsigned sidx1 = sRange1->getId ();
		G4_RegVar * tRange1 =
			(*kt)->getVar ()->getNonTransientBaseRegVar ();
		unsigned tidx1 = tRange1->getId ();

		for (unsigned lidx = 0; lidx < varIdCount_; ++lidx) {
			if (getRegVar (lidx)->isSpilled() == false) continue;
			G4_RegVar * sRange2 = getReprRegVar (getRegVar (lidx));
			unsigned sidx2 = sRange2->getId ();
			G4_RegVar * tRange2 =
				getRegVar (lidx)->getNonTransientBaseRegVar ();
			unsigned tidx2 = tRange2->getId ();

			while (true) {

				if (spillMemLifetimeInterfere (sidx1, sidx2)) {
					unsigned disp1 = sRange1->getDisp ();
					unsigned size1 = getByteSize (sRange1);
					unsigned disp2 = sRange2->getDisp ();
					unsigned size2 = getByteSize (sRange2);

					if (disp1 == disp2) {
                        MUST_BE_TRUE(false, "Bad spill displacements !");
					}

					else if (disp1 < disp2) {

						if (disp1 + size1 > disp2) {
                            MUST_BE_TRUE(false, "Bad spill displacements !");
						}
					}

					else {

						if (disp2 + size2 > disp1) {
							MUST_BE_TRUE(false, "Bad spill displacements !");
						}
					}
				}

				if (sidx1 == tidx1 && sidx2 == tidx2) {
					break;
				}

				else if (sidx1 == tidx1) {
					sidx2 = tidx2;
					sRange2 = tRange2;
				}

				else {
					sidx1 = tidx1;
					sRange1 = tRange1;
				}
			}
		}
	}
#endif

	// Emit the instruction with the introduced spill/fill ranges in the
	// current iteration.

#ifndef NDEBUG
#ifdef DEBUG_VERBOSE_ON1
	std::stringstream fname;
	fname << "spill_code_" << iterationNo_++ << "_" << kernel->getName()
		  << ends;
	std::ofstream sout;
	sout.open (fname.str ().c_str ());
	kernel->emit_asm (sout, true, 0);
	sout.close ();
#endif
#endif

    return true;
}


// Replace Scratch Block Read/Write message with OWord Block Read/Write message
void
SpillManagerGMRF::fixSpillFillCode (
	G4_Kernel * kernel
)
{
	FlowGraph& fg = kernel->fg;

    unsigned statelessSurfaceIndex = 0xFF;

	for( BB_LIST_ITER it = fg.BBs.begin(); it != fg.BBs.end(); it++ )
	{
		INST_LIST::iterator jt = (*it)->instList.begin ();

		while( jt != (*it)->instList.end () )
        {
			INST_LIST::iterator kt = jt;
			++kt;
			G4_INST * inst = *jt;

            if( inst->isSend() &&
                 inst->getSpillOrFill() == true )
            {
                if( inst->getMsgDesc()->isScratchRead() )
                {
                    // Fix fill message
                    G4_Operand * curDst = inst->getSrc(0);
                    G4_Declare * mRangeDcl = NULL;

                    if( curDst->getTopDcl() == builder_->getBuiltinR0() )
                    {
                        G4_Operand * postDst = inst->getDst ();
                        G4_RegVar * fillRegVar = postDst->getTopDcl()->getRegVar()->getBaseRegVar ();

                        const char * name =
		                    createImplicitRangeName (
			                    "FL_MSG", fillRegVar,
			                    getMsgFillIndex (fillRegVar));

                        mRangeDcl =
		                    createRangeDeclare (
			                    name,
                                G4_GRF,
                                REG_DWORD_SIZE, 1, Type_UD, NULL, DEF_HORIZ_STRIDE,
                                DeclareType::Tmp, fillRegVar, NULL, 0);

	                    G4_DstRegRegion * mHeaderInputDstRegion =
		                    createMHeaderInputDstRegion (mRangeDcl->getRegVar ());
	                    G4_SrcRegRegion * inputPayload = createInputPayloadSrcRegion ();

					    G4_INST * movInst = builder_->createInternalInst( NULL, G4_mov, NULL, false, REG_DWORD_SIZE,
                            mHeaderInputDstRegion, inputPayload, NULL, InstOpt_WriteEnable, inst->getLineNo(), inst->getCISAOff(), inst->getSrcFilename() );
                        (*it)->instList.insert( jt, movInst );

                        curDst = createMHeaderInputDstRegion (mRangeDcl->getRegVar ());

                        G4_SrcRegRegion* curSrcOpnd = builder_->createSrcRegRegion(Mod_src_undef, Direct, mRangeDcl->getRegVar(), 0, 0,
                            builder_->getRegionStride1(), Type_UD );
                        inst->setSrc( curSrcOpnd, 0 );
                    }
                    else
                    {
                        mRangeDcl = curDst->getTopDcl();
                    }

		            unsigned offset = inst->getMsgDesc()->getScratchRWOffset();
                    offset = offset * (G4_GRF_REG_NBYTES / OWORD_BYTE_SIZE);
		            G4_Imm * offsetImm = builder_->createImm (offset, Type_UD);
		            G4_DstRegRegion * mHeaderOffsetDstRegion =
			            createMHeaderBlockOffsetDstRegion (mRangeDcl->getRegVar ());

                    G4_INST* mov_inst = builder_->createInternalInst (NULL, G4_mov, NULL, false, SCALAR_EXEC_SIZE, mHeaderOffsetDstRegion,
                        offsetImm, NULL, InstOpt_WriteEnable, inst->getLineNo(), inst->getCISAOff(), inst->getSrcFilename());
                    (*it)->instList.insert( jt, mov_inst );

		            unsigned segmentByteSize = inst->getMsgDesc()->ResponseLength() * REG_BYTE_SIZE;
		            unsigned responseLength = cdiv (segmentByteSize, REG_BYTE_SIZE);
                    responseLength = responseLength << getSendRspLengthBitOffset ();
                    unsigned message = statelessSurfaceIndex | responseLength;

		            unsigned headerPresent = 0x80000;
		            message |= headerPresent;
                    unsigned messageType = getSendOwordReadType ();
                    message |= messageType << getSendReadTypeBitOffset ();
		            unsigned messageLength = OWORD_PAYLOAD_HEADER_MIN_HEIGHT;
		            message |= messageLength << getSendMsgLengthBitOffset ();
		            unsigned segmentOwordSize =
			            cdiv (segmentByteSize, OWORD_BYTE_SIZE);
		            message |= blockSendBlockSizeCode (segmentOwordSize);
		            unsigned char execSize = LIMIT_SEND_EXEC_SIZE (segmentOwordSize * DWORD_BYTE_SIZE);

                    G4_Operand * msg = builder_->createImm (message, Type_UD);
		            unsigned int regs2snd = ( message >> getSendMsgLengthBitOffset() ) & 0xF;
		            unsigned int regs2rcv = ( message >> getSendRspLengthBitOffset() ) & 0x1F;
		            G4_SendMsgDescriptor * msgDesc = builder_->createSendMsgDesc( message,
			            regs2rcv, regs2snd, inst->getMsgDesc()->getFuncId(), inst->getMsgDesc()->isEOTInst(),
                        0, inst->getMsgDesc()->getExtFuncCtrl(), true, false, NULL, NULL);

                    inst->setSrc( msg, 1 );
                    inst->setMsgDesc( msgDesc );
                    inst->setExecSize( execSize );
                }
                else  if( inst->getMsgDesc()->isScratchWrite() )
                {
                    // Fix spill message
                    G4_Operand * curDst = inst->getSrc(0);
                    G4_Declare * mRangeDcl = curDst->getTopDcl();

		            unsigned offset = inst->getMsgDesc()->getScratchRWOffset();
                    offset = offset * (G4_GRF_REG_NBYTES / OWORD_BYTE_SIZE);
		            G4_Imm * offsetImm = builder_->createImm (offset, Type_UD);
		            G4_DstRegRegion * mHeaderOffsetDstRegion =
			            createMHeaderBlockOffsetDstRegion (mRangeDcl->getRegVar ());

                    G4_INST* mov_inst = builder_->createInternalInst (NULL, G4_mov, NULL, false, SCALAR_EXEC_SIZE, mHeaderOffsetDstRegion,
                        offsetImm, NULL, InstOpt_WriteEnable, inst->getLineNo(), inst->getCISAOff(), inst->getSrcFilename());
                    (*it)->instList.insert( jt, mov_inst );

		            unsigned segmentByteSize = (inst->getMsgDesc()->MessageLength() - SCRATCH_PAYLOAD_HEADER_MAX_HEIGHT) * REG_BYTE_SIZE;
		            unsigned writePayloadCount = cdiv (segmentByteSize, REG_BYTE_SIZE);
                    unsigned message = statelessSurfaceIndex;

		            unsigned headerPresent = 0x80000;
	                message |= headerPresent;
		            unsigned messageType = getSendOwordWriteType();
		            message |= messageType << getSendWriteTypeBitOffset ();
		            unsigned payloadHeaderCount = OWORD_PAYLOAD_HEADER_MAX_HEIGHT;
		            unsigned messageLength = writePayloadCount + payloadHeaderCount;
		            message |= messageLength << getSendMsgLengthBitOffset ();
		            unsigned segmentOwordSize = cdiv(segmentByteSize, OWORD_BYTE_SIZE);
		            message |= blockSendBlockSizeCode (segmentOwordSize);
		            unsigned char execSize = LIMIT_SEND_EXEC_SIZE (segmentOwordSize * DWORD_BYTE_SIZE);

                    G4_Operand * msg = builder_->createImm (message, Type_UD);
		            unsigned int regs2snd = ( message >> getSendMsgLengthBitOffset() ) & 0xF;
		            unsigned int regs2rcv = ( message >> getSendRspLengthBitOffset() ) & 0x1F;
		            G4_SendMsgDescriptor * msgDesc = builder_->createSendMsgDesc( message,
                        regs2rcv, regs2snd, inst->getMsgDesc()->getFuncId(), inst->getMsgDesc()->isEOTInst(), 0,
                        inst->getMsgDesc()->getExtFuncCtrl(), false, true, NULL, NULL );

                    inst->setSrc( msg, 1 );
                    inst->setMsgDesc( msgDesc );
                    inst->setExecSize( execSize );
                }
			}

			jt = kt;
		}
	}
}

uint32_t computeSpillMsgDesc(unsigned int payloadSize, unsigned int offsetInGrfUnits)
{
    // Compute msg descriptor given payload size and offset.
    unsigned headerPresent = 0x80000;
    uint32_t message = headerPresent;
    unsigned msgLength = SCRATCH_PAYLOAD_HEADER_MAX_HEIGHT;
    message |= (msgLength << getSendMsgLengthBitOffset());
    message |= (1 << SCRATCH_MSG_DESC_CATEORY);
    message |= (1 << SCRATCH_MSG_DESC_CHANNEL_MODE);
    message |= (1 << SCRATCH_MSG_DESC_OPERATION_MODE);
    unsigned blocksize_encoding = getScratchBlocksizeEncoding(payloadSize);
    message |= (blocksize_encoding << SCRATCH_MSG_DESC_BLOCK_SIZE);
    int offset = offsetInGrfUnits;
    message |= offset;

    return message;


    /*
    unsigned headerPresent = 0x80000;
    message = headerPresent;
    unsigned msgLength = useSplitSend() ? SCRATCH_PAYLOAD_HEADER_MAX_HEIGHT : SCRATCH_PAYLOAD_HEADER_MAX_HEIGHT + height;
    message |= (msgLength << getSendMsgLengthBitOffset() );
    message |= (1 << SCRATCH_MSG_DESC_CATEORY);
    message |= (1 << SCRATCH_MSG_DESC_CHANNEL_MODE);
    message |= (1 << SCRATCH_MSG_DESC_OPERATION_MODE);
    unsigned blocksize_encoding = getScratchBlocksizeEncoding(height);
    message |= (blocksize_encoding << SCRATCH_MSG_DESC_BLOCK_SIZE);
    int offset = getDisp(base);
    getSpillOffset(offset);
    message |= (offset >> 5) + regOff;
    execSize = 16;
    */

}

uint32_t computeFillMsgDesc(unsigned int payloadSize, unsigned int offsetInGrfUnits)
{
    // Compute msg descriptor given payload size and offset.
    unsigned headerPresent = 0x80000;
    uint32_t message = headerPresent;
    unsigned msgLength = 1;
    message |= (msgLength << getSendMsgLengthBitOffset());
    message |= (1 << SCRATCH_MSG_DESC_CATEORY);
    message |= (0 << SCRATCH_MSG_INVALIDATE_AFTER_READ);
    unsigned blocksize_encoding = getScratchBlocksizeEncoding(payloadSize);
    message |= (blocksize_encoding << SCRATCH_MSG_DESC_BLOCK_SIZE);
    message |= offsetInGrfUnits;

    return message;


    /*
    		unsigned headerPresent = 0x80000;
		message |= SCRATCH_PAYLOAD_HEADER_MAX_HEIGHT << getSendMsgLengthBitOffset ();
	    message |= headerPresent;

		message |= (1 << SCRATCH_MSG_DESC_CATEORY);
		message |= (0 << SCRATCH_MSG_INVALIDATE_AFTER_READ);
        unsigned blocksize_encoding = getScratchBlocksizeEncoding(height);

		message |= (blocksize_encoding << SCRATCH_MSG_DESC_BLOCK_SIZE);

        int offset = getDisp(base);
        getSpillOffset(offset);
		message |= ((offset >> 5) + regOff);

		execSize = 16;
*/

}
