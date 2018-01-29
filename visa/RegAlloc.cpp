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

#include <vector>
#include <limits.h>
#include "Mem_Manager.h"
#include "FlowGraph.h"
#include "RegAlloc.h"
#include <bitset>
#include "GraphColor.h"
#include "Timer.h"
#include <fstream>
#include <math.h>
#include "DebugInfo.h"

using namespace std;
using namespace vISA;

#define GRAPH_COLOR

PointsToAnalysis::PointsToAnalysis( DECLARE_LIST &declares, unsigned int numBB ) :
numBBs(numBB), numAddrs(0), indirectUses(NULL), pointsToSets(NULL), addrPointsToSetIndex(NULL)
{

    for (DECLARE_LIST_ITER di = declares.begin(); di != declares.end(); ++di)
    {
        G4_Declare* decl = *di;
        //add alias check, For Alias Dcl
        if( ( decl->getRegFile() == G4_ADDRESS ) &&
            decl->getAliasDeclare() == NULL )  // It is a base declaration, not alias
        {
            // participate liveness analysis
            decl->getRegVar()->setId(numAddrs++);
        }
        else
        {
            decl->getRegVar()->setId(UNDEFINED_VAL);
        }
    }

    // assign all addr aliases the same ID as its root
    for (DECLARE_LIST_ITER di = declares.begin(); di != declares.end(); ++di)
    {
        G4_Declare* decl = *di;
        if( ( decl->getRegFile() == G4_ADDRESS ) &&
            decl->getAliasDeclare() != NULL )
        {
            // participate liveness analysis
            decl->getRegVar()->setId(decl->getRegVar()->getId());
        }
    }
    indirectUses = new REGVAR_VECTOR[numBBs];

    if( numAddrs > 0 )
    {
		for( unsigned int i = 0; i < numAddrs; i++ )
			regVars.push_back(NULL);

		for (DECLARE_LIST_ITER di = declares.begin(); di != declares.end(); ++di)
		{
			G4_Declare* decl = *di;
			if( ( decl->getRegFile() == G4_ADDRESS ) &&
				decl->getAliasDeclare() == NULL &&
				decl->getRegVar()->getId() != UNDEFINED_VAL)
			{
				regVars[decl->getRegVar()->getId()] = decl->getRegVar();
			}
		}

        pointsToSets = new REGVAR_VECTOR[numAddrs];
        addrPointsToSetIndex = new unsigned[numAddrs];
        // initially each address variable has its own points-to set
        for( unsigned i = 0; i < numAddrs; i++ )
        {
            addrPointsToSetIndex[i] = i;
        }
    }
}

PointsToAnalysis::~PointsToAnalysis()
{
    delete[] pointsToSets;
    delete[] addrPointsToSetIndex;
    delete[] indirectUses;
}

//
//  A flow-insensitive algroithm to compute the register usage for indirect accesses.
//  The algorithm is divided into two parts:
//  1. We go through every basic block computing the points-to set for each adddress
//     variable.  This happens when we see an instruction like
//     mov (8) A0 &R0
//
//  2. We go through each basic block again, and for each r[A0] expression
//     we mark variables in A0's points-to set as used in the block
//
//  The algorithm is conservative but should work well for our inputs since
//  the front end pretty much always uses a fresh address variable when taking
//  the address of a GRF variable, wtih the exception of call-by-reference parameters
//  It's performed only once at the beginning of RA, at the point where all variables
//  are virtual and no spill code (either for address or GRF) has been inserted.
//
void PointsToAnalysis::doPointsToAnalysis(FlowGraph & fg)
{

    if( numAddrs == 0 )
    {
        return;
    }

    // keep a list of address taken variables
    std::vector<G4_RegVar*> addrTakenVariables;
    for (BB_LIST_ITER it = fg.BBs.begin(), itend = fg.BBs.end(); it != itend; ++it)
	{
		G4_BB* bb = (*it);
		for (INST_LIST_ITER iter = bb->instList.begin(), iterEnd = bb->instList.end(); iter != iterEnd; ++iter)
		{
            G4_INST* inst = (*iter);
            for (int i = 0; i < G4_MAX_SRCS; i++)
            {
                G4_Operand* src = inst->getSrc(i);
                if (src != NULL && src->isAddrExp())
                {
                    addrTakenVariables.push_back(src->asAddrExp()->getRegVar());
                }
            }
        }
    }

    // first compute the points-to set for each address variable
	for (BB_LIST_ITER it = fg.BBs.begin(), itend = fg.BBs.end(); it != itend; ++it)
	{
		G4_BB* bb = (*it);
		for (INST_LIST_ITER iter = bb->instList.begin(), iterEnd = bb->instList.end(); iter != iterEnd; ++iter)
		{
			G4_INST* inst = (*iter);

            if(inst->isPseudoKill() || inst->isLifeTimeEnd())
            {
                // No need to consider these lifetime placeholders for points2analysis
                continue;
            }

            G4_DstRegRegion* dst = inst->getDst();
            if (dst != NULL && dst->getRegAccess() == Direct && dst->getType() != Type_UD)
            {
                G4_VarBase* ptr = dst->getBase();
                if (ptr->isRegVar() && ptr->asRegVar()->getDeclare()->getRegFile() == G4_ADDRESS &&
                    !ptr->asRegVar()->getDeclare()->getIsExDesc())
                {

                    // dst is an address variable.  ExDesc A0 may be ignored since they are never used in indirect access
                    if( inst->isMov() )
                    {
                        G4_Operand* src = inst->getSrc(0);
                        if( src->isAddrExp() )
                        {
                             // case 1:  mov A0 &GRF
                            G4_RegVar* addrTaken = src->asAddrExp()->getRegVar();
                            if (addrTaken != NULL)
                            {
                                addToPointsToSet( ptr->asRegVar(), addrTaken );
                            }
                        }
                        else
                        {
                            //G4_Operand* srcPtr = src->isSrcRegRegion() ? src->asSrcRegRegion()->getBase() : src;
                            G4_VarBase* srcPtr = src->isSrcRegRegion() ? src->asSrcRegRegion()->getBase() : nullptr;

                            if (srcPtr && srcPtr->isRegVar() && srcPtr->asRegVar()->getDeclare()->getRegFile() == G4_ADDRESS)
                            {
                                // case 2:  mov A0 A1
                                // merge the two addr's points-to set together
                                if( ptr->asRegVar()->getId() != srcPtr->asRegVar()->getId() )
                                {
                                    mergePointsToSet( srcPtr->asRegVar(), ptr->asRegVar() );
                                }
                            }
                            else
                            {
                                // case 3:  mov A0 V2
                                // conservatively assume address can point to anything
                                DEBUG_MSG("unexpected addr move for pointer analysis:\n");
                                DEBUG_EMIT(inst);
                                DEBUG_MSG("\n")
                                for (int i = 0, size = (int)addrTakenVariables.size(); i < size; i++)
                                {
                                    addToPointsToSet( ptr->asRegVar(), addrTakenVariables[i] );
                                }
                            }
                        }
                    }
                    else if( inst->isArithmetic() )
                    {
                        G4_Operand* src0 = inst->getSrc(0);
                        G4_Operand* src1 = inst->getSrc(1);
                        bool src0addr = false;
                        if( src0->isAddrExp() )
                        {
                            src0addr = true;
                        }
                        else if( src0->isSrcRegRegion() && src0->getRegAccess() == Direct )
                        {
                            if( src0->isAddress() )
                            {
                                src0addr = true;
                            }
                        }

                        bool src1addr = false;
                        if( src1->isAddrExp() )
                        {
                            src1addr = true;
                        }
                        else if( src1->isSrcRegRegion() && src1->getRegAccess() == Direct )
                        {
                            if( src1->isAddress() )
                            {
                                src1addr = true;
                            }
                        }

                        if (src0addr ^ src1addr)
                        {
                            G4_Operand* src = src0addr ? src0 : src1;

                            if( src->isAddrExp() )
                            {
                                // case 4:  add/mul A0 &GRF src1
                                G4_RegVar* addrTaken = src->asAddrExp()->getRegVar();
                                addToPointsToSet( ptr->asRegVar(), addrTaken );
                            }
                            else
                            {
                                G4_VarBase* srcPtr = src->isSrcRegRegion() ? src->asSrcRegRegion()->getBase() : nullptr;
                                // case 5:  add/mul A0 A1 src1
                                // merge the two addr's points-to set together
                                if (srcPtr && (ptr->asRegVar()->getId() != srcPtr->asRegVar()->getId()) )
                                {
                                    mergePointsToSet( srcPtr->asRegVar(), ptr->asRegVar() );
                                }
                            }
                        }
                        else if (ptr->isRegVar() && ptr->asRegVar()->isPhyRegAssigned())
                        {
                           // OK, using builtin a0 or a0.2 directly.
                        }
                        else
                        {
                            // case 6:  add/mul A0 V1 V2
                            DEBUG_MSG("unexpected addr add/mul for pointer analysis:\n");
                            DEBUG_EMIT(inst);
                            DEBUG_MSG("\n")
                            for (int i = 0, size = (int)addrTakenVariables.size(); i < size; i++)
                            {
                                addToPointsToSet( ptr->asRegVar(), addrTakenVariables[i] );
                            }
                        }
                    }
                    else
                    {
                        // case 7: A0 = ???
                        DEBUG_MSG("unexpected instruction with address destination:\n");
                        DEBUG_EMIT(inst);
                        DEBUG_MSG("\n");
                        for (int i = 0, size = (int)addrTakenVariables.size(); i < size; i++)
                        {
                            addToPointsToSet( ptr->asRegVar(), addrTakenVariables[i] );
                        }
                    }
                }
            }
        }
    }

#ifdef DEBUG_VERBOSE_ON
    DEBUG_VERBOSE("Results of points-to analysis:\n");
    for( unsigned int i = 0; i < numAddrs; i++ )
    {
        DEBUG_VERBOSE("Addr " << i);
        REGVAR_VECTOR grfVec = pointsToSets[addrPointsToSetIndex[i]];
        for( unsigned int j = 0; j < grfVec.size(); j++ )
        {
            DEBUG_EMIT(grfVec[j]);
            DEBUG_VERBOSE("\t");
        }
        DEBUG_VERBOSE("\n");
    }
#endif

    // mark GRF that may be used indirect access as live in the block
    // This includes all GRFs in the address's points-to set
    for (BB_LIST_ITER it = fg.BBs.begin(); it != fg.BBs.end(); ++it)
    {
        G4_BB* bb = (*it);
        for (INST_LIST_ITER iter = bb->instList.begin(); iter != bb->instList.end(); ++iter)
        {
            G4_INST* inst = (*iter);
            G4_DstRegRegion* dst = inst->getDst();

            if( dst != NULL &&
                dst->getRegAccess() == IndirGRF )
            {
                G4_VarBase* dstptr = dst->getBase();
                MUST_BE_TRUE( dstptr->isRegVar() && dstptr->asRegVar()->getDeclare()->getRegFile() == G4_ADDRESS,
                    "base must be address" );
                addPointsToSetToBB( bb->getId(), dstptr->asRegVar() );
            }

            for( unsigned j = 0; j < G4_MAX_SRCS; j++ )
            {
                //
                // look for indirect reg access r[ptr] which refers addrTaken reg var
                //
                if (inst->getSrc(j) == NULL || !inst->getSrc(j)->isSrcRegRegion()) {
                    continue;
                }

                G4_SrcRegRegion* src = (G4_SrcRegRegion*)inst->getSrc(j);

                if( src->getRegAccess() == IndirGRF )
                {
                    G4_VarBase* srcptr = src->getBase();
                    MUST_BE_TRUE( srcptr->isRegVar() && srcptr->asRegVar()->getDeclare()->getRegFile() == G4_ADDRESS,
                        "base must be address" );
                    addPointsToSetToBB( bb->getId(), srcptr->asRegVar() );
                }
            }
        }
    }

#ifdef DEBUG_VERBOSE_ON
    for( unsigned int i = 0; i < numBBs; i++ )
    {
        DEBUG_VERBOSE("Indirect uses for BB" << i << "\t");
        REGVAR_VECTOR grfVec = getIndrUseVectorForBB( i );
        for( unsigned int j = 0; j < grfVec.size(); j++ )
        {
            DEBUG_EMIT( grfVec[j] );
            DEBUG_VERBOSE("\t");
        }
        DEBUG_VERBOSE("\n");
    }
#endif

}

LivenessAnalysis::LivenessAnalysis(
        GlobalRA& g,
        unsigned char kind,
		bool doIPA,
        bool verifyRA) :
        performIPA(doIPA), numVarId(0), numSplitVar(0), numSplitStartID(0), numUnassignedVarId(0), numAddrId(0), selectedRF(kind), m(4096),
		fg(g.kernel.fg), pointsToAnalysis(g.pointsToAnalysis), gra(g)
{
	//
	// NOTE:
	// The maydef sets are simply aliases to the mayuse sets, since their uses are
	// mutually exclusive.
	//
	// Go over each reg var if it's a liveness candidate, assign id for bitset.
	//
    bool areAllPhyRegAssigned = true;

    DECLARE_LIST_ITER di = gra.kernel.Declares.begin();
    while (di != gra.kernel.Declares.end())
	{
		G4_Declare* decl = *di;
		if (livenessCandidate(decl, verifyRA) && decl->getAliasDeclare() == NULL )
		{
            if (decl->getIsSplittedDcl())
            {
                decl->setSplitVarStartID(0);
            }
            if (decl->getIsPartialDcl())
            {
                auto declSplitDcl = gra.getSplittedDeclare(decl);
                if (declSplitDcl->getIsSplittedDcl())
                {
	                if (numSplitStartID == 0)
	                {
	                    numSplitStartID = numVarId;
	                }
	                
	                if (declSplitDcl->getSplitVarStartID() == 0)
	                {
                        declSplitDcl->setSplitVarStartID(numVarId);
	                }
	                numSplitVar ++;
                }
                else
                {
                    //If parent declare is marked as not splitted any more, destroy the split declares
		            DECLARE_LIST_ITER old_it = di;
		            di++;
		            gra.kernel.Declares.erase( old_it );
		            continue;
                }
            }

			// participate liveness analysis
			decl->getRegVar()->setId(numVarId++);

            if (decl->getRegVar()->getPhyReg() == NULL && !decl->getIsPartialDcl())
                numUnassignedVarId++;

			//
			// dump Reg Var info for debugging
			//

            if( decl->getRegVar()->isPhyRegAssigned() == false )
            {
                areAllPhyRegAssigned = false;
            }
#ifdef DEBUG_VERBOSE_ON
            DEBUG_EMIT(decl->getRegVar());
			DEBUG_VERBOSE(" id = " << decl->getRegVar()->getId() << std::endl);
#endif
		}
		//
		// those reg vars that are not candidates, set their id to
		// undefined value
		//
		else
		{
			decl->getRegVar()->setId(UNDEFINED_VAL);
		}
        di++;
	}

	// For Alias Dcl
	for (DECLARE_LIST_ITER di = gra.kernel.Declares.begin(); di != gra.kernel.Declares.end(); ++di)
	{
		G4_Declare* decl = *di;
		if (livenessCandidate(decl, verifyRA) && (decl)->getAliasDeclare() != NULL)
		{
			// It is an alias declaration. Set its id = base declaration id
			(decl)->getRegVar()->setId((decl)->getAliasDeclare()->getRegVar()->getId());
		}
#ifdef DEBUG_VERBOSE_ON
		DEBUG_EMIT((decl)->getRegVar());
		DEBUG_VERBOSE(" id = " << (decl)->getRegVar()->getId() << std::endl);
#endif
	}

	//
	// if no chosen candidate for reg allocation return
	//
	if (numVarId == 0 ||
        (verifyRA == false &&
         areAllPhyRegAssigned == true))
	{
        // If all variables have physical register assignments
        // there are no candidates for allocation
        numVarId = 0;
		return;
	}

	//
	// put selected reg vars into vars[]
	//
	vars.resize(numVarId);
	for (auto dcl : gra.kernel.Declares)
	{
		if (livenessCandidate(dcl, verifyRA) &&
			dcl->getAliasDeclare() == NULL)
		{
			G4_RegVar* var = dcl->getRegVar();
			vars[var->getId()] = var;

			if (var->getDeclare()->getHasFileScope())
			{
				fileScopeVars.push_back(var);
			}
		}
	}

    addr_taken = BitSet(numVarId, false);

	numBBId = (unsigned) fg.BBs.size();

	def_in.resize(numBBId);
    def_out.resize(numBBId);
    use_in.resize(numBBId);
    use_out.resize(numBBId);
    use_gen.resize(numBBId);
    use_kill.resize(numBBId);
    indr_use.resize(numBBId);

	for (unsigned i = 0; i < numBBId; i++)
	{
		def_in[i]  = BitSet(numVarId, false);
		def_out[i] = BitSet(numVarId, false);
		use_in[i]  = BitSet(numVarId, false);
		use_out[i] = BitSet(numVarId, false);
		use_gen[i] = BitSet(numVarId, false);
		use_kill[i]= BitSet(numVarId, false);
		indr_use[i]= BitSet(numVarId, false);
	}

	numFnId = (unsigned) fg.funcInfoTable.size();
    maydef.resize(numFnId);
}

LivenessAnalysis::~LivenessAnalysis()
{
	//
	// if no chosen candidate for reg allocation return
	//
	if (numVarId == 0)
	{
		return;
	}

    for (auto it : neverDefinedRows)
    {
        it.second->~BitSet();
    }
}

bool LivenessAnalysis::livenessCandidate(G4_Declare* decl, bool verifyRA)
{
    LocalLiveRange* declLR = nullptr;
    if (verifyRA == false && (declLR = gra.getLocalLR(decl)) && declLR->getAssigned() && !declLR->isEOT())
    {
        return false;
    }
    else if ((selectedRF & decl->getRegFile()))
    {
        if ((selectedRF & G4_GRF) && (decl->getRegFile() & G4_INPUT))
        {
            if (decl->getRegVar()->isPhyRegAssigned() && !decl->getRegVar()->isGreg())
            {
                return false;
            }
            if (decl->getByteSize() == 0)
            {
                // regrettably, this can happen for arg/retval pre-defined variable
                return false;
            }
        }
        return true;
    }
    else
    {
        return false;
    }
}

void LivenessAnalysis::updateKillSetForDcl(G4_Declare* dcl, BitSet* curBBGen, BitSet* curBBKill, G4_BB* curBB, BitSet* entryBBGen, BitSet* entryBBKill, G4_BB* entryBB, unsigned scopeID)
{
	if (scopeID != 0 &&
		scopeID != UINT_MAX &&
		dcl->getScopeID() == scopeID)
	{
		entryBBKill->set(dcl->getRegVar()->getId(), true);
		entryBBGen->set(dcl->getRegVar()->getId(), false);
#ifdef DEBUG_VERBOSE_ON
		DEBUG_VERBOSE("Killed sub-routine scope " << dcl->getName() << " at bb with id = " << entryBB->getId() << std::endl);
#endif
	}
}

// Scoping info is stored per decl. A variable can be either global scope (default),
// sub-routine scope, or basic block scope. This function iterates over all
// instructions and their operands in curBB and if scoping for it is set in symbol
// table then it marks kills accordingly. A bb scope variable is killed in the bb it appears
// and a sub-routine local variable is killed in entry block of the sub-routine. No
// error check is performed currently so if variable scoping information is incorrect
// then generated code will be so too.
void LivenessAnalysis::performScoping(BitSet* curBBGen, BitSet* curBBKill, G4_BB* curBB, BitSet* entryBBGen, BitSet* entryBBKill, G4_BB* entryBB)
{
	unsigned scopeID = curBB->getScopeID();
    for( INST_LIST_ITER it = curBB->instList.begin();
        it != curBB->instList.end();
        it++ )
    {
        G4_INST* inst = (*it);

        G4_DstRegRegion* dst = inst->getDst();

        if( dst &&
            dst->getBase()->isRegAllocPartaker() )
        {
            G4_Declare* dcl = GetTopDclFromRegRegion( dst );
            updateKillSetForDcl( dcl, curBBGen, curBBKill, curBB, entryBBGen, entryBBKill, entryBB, scopeID );
        }

        for( int i = 0; i < G4_MAX_SRCS; i++ )
        {
            G4_Operand* src = inst->getSrc(i);

            if( src )
            {
                if( src->isSrcRegRegion() &&
                    src->asSrcRegRegion()->getBase()->isRegAllocPartaker() )
                {
                    G4_Declare* dcl = GetTopDclFromRegRegion( src );
                    updateKillSetForDcl( dcl, curBBGen, curBBKill, curBB, entryBBGen, entryBBKill, entryBB, scopeID );
                }
                else if( src->isAddrExp() &&
                    src->asAddrExp()->getRegVar()->isRegAllocPartaker() )
                {
                    G4_Declare* dcl = src->asAddrExp()->getRegVar()->getDeclare();

                    while( dcl->getAliasDeclare() != NULL )
                    {
                        dcl = dcl->getAliasDeclare();
                    }

                    updateKillSetForDcl( dcl, curBBGen, curBBKill, curBB, entryBBGen, entryBBKill, entryBB, scopeID );
                }
            }
        }
    }
}

void LivenessAnalysis::detectNeverDefinedVarRows()
{
    // This function records variables and its rows that are never defined
    // in the kernel. This information helps detect kills for partial
    // writes when VISA optimizer optimizes away some rows of a variable.
    // In interest of compile time we only look for full rows that are
    // not defined rather than sub-regs.
    std::map<G4_Declare*, BitSet*> largeDefs;

    // Populate largeDefs map with dcls > 1 GRF size
    for (auto dcl : gra.kernel.Declares)
    {
        if (dcl->getAliasDeclare() || dcl->getIsPartialDcl() || dcl->getAddressed())
            continue;

        if (dcl->getRegFile() != G4_GRF)
            continue;

        unsigned int dclNumRows = dcl->getNumRows();

        if (dclNumRows < 2)
            continue;

        BitSet* bitset = new (m) BitSet(dclNumRows, false);

        largeDefs.insert(std::make_pair(dcl, bitset));
    }

    if (largeDefs.size() == 0)
        return;

    // Update row usage of each dcl in largeDefs
    for (auto bb : gra.kernel.fg.BBs)
    {
        for (auto inst : bb->instList)
        {
            auto dst = inst->getDst();
            if (!dst)
                continue;

            auto dstTopDcl = dst->getTopDcl();
            
            if (dstTopDcl)
            {
                auto it = largeDefs.find(dstTopDcl);

                if (it == largeDefs.end())
                {
                    continue;
                }

                unsigned int lb = dst->getLeftBound();
                unsigned int rb = dst->getRightBound();

                unsigned int rowStart = lb / G4_GRF_REG_NBYTES;
                unsigned int rowEnd = rb / G4_GRF_REG_NBYTES;

                it->second->set(rowStart, rowEnd);
            }
        }
    }

    // Propagate largeDefs to neverDefinedRows bit vector to later bitwise OR it
    for (auto it : largeDefs)
    {
        bool allSet = true;
        unsigned int numRows = it.first->getNumRows();
        for (unsigned int i = 0; i < numRows; i++)
        {
            if (!it.second->isSet(i))
            {
                allSet = false;
                break;
            }
        }

        if (allSet)
            continue;

        BitSet* undefinedRows = new (m) BitSet(it.first->getByteSize(), false);
        
        for (unsigned int i = 0; i < numRows; i++)
        {
            if (!it.second->isSet(i))
            {
                undefinedRows->set((i*G4_GRF_REG_NBYTES), ((i+1)*G4_GRF_REG_NBYTES) - 1);
            }
        }

        neverDefinedRows.insert(std::make_pair(it.first, undefinedRows));
    }

    for (auto it : largeDefs)
    {
        it.second->~BitSet();
    }
}

//
// compute liveness of reg vars
// In gen4, each reg var indicates a region within the register file. As such, the case in which two consecutive defs
// of a reg region without any use in between does not mean the second def overwrites the first one because the two defs
// may write different parts of the region. Def vectors are used to track which definitions of reg vars reach
// the entry and the end of a basic block, which tell us the first definitions of reg vars. Use vectors track which
// uses of reg vars are anticipated, which tell use the uses of reg vars.Def and Use vectors encapsulate the liveness
// of reg vars.
//
void LivenessAnalysis::computeLiveness(bool computePseudoKill)
{
	//
	// no reg var is selected, then no need to compute liveness
	//
	if (getNumSelectedVar() == 0)
	{
		return;
	}

#ifdef DEBUG_VERBOSE_ON
	std::vector<FuncInfo*>& fns = fg.funcInfoTable;
#endif
	//
	// mark input arguments live at the entry of kernel
    // mark output arguments live at the exit of kernel
	//
	BitSet inputDefs(numVarId, false);
	BitSet outputUses(numVarId, false);

    for (unsigned i = 0; i < numVarId; i++)
    {
        G4_Declare *decl = vars[i]->getDeclare();
        if (((decl->isInput() == true &&
             !(fg.builder->getFCPatchInfo() && 
               fg.builder->getFCPatchInfo()->getFCComposableKernel() &&
               !decl->isLiveIn())) &&
             !(decl->getIsPreDefArg() &&
               (fg.builder->getIsKernel() ||
                (fg.getIsStackCallFunc() &&
                 fg.builder->getArgSize() == 0)))) ||
			( decl->getHasFileScope() && fg.getIsStackCallFunc() ) ||
            (fg.builder->getOption(vISA_enablePreemption) &&
             decl == fg.builder->getBuiltinR0()) )
        {
            inputDefs.set( i, true );
#ifdef DEBUG_VERBOSE_ON
			DEBUG_VERBOSE("First def input = " << decl->getName() << std::endl);
#endif
        }
        if ((decl->isOutput() == true &&
            !(decl->getIsPreDefRet() &&
               (fg.builder->getIsKernel() ||
                (fg.getIsStackCallFunc() &&
                 fg.builder->getRetVarSize() == 0)))) ||
            (decl->getHasFileScope() &&
			(fg.getIsStackCallFunc() || fg.getHasStackCalls())) ||
			(fg.builder->getOption(vISA_enablePreemption) &&
			  decl == fg.builder->getBuiltinR0()))
        {
            outputUses.set( i, true );
#ifdef DEBUG_VERBOSE_ON
			DEBUG_VERBOSE("First def output	= " << decl->getName() << std::endl);
#endif
        }
    }

    //
    // clean up def_in & def_out that are used in markFirstDef
    //
    for (unsigned i = 0; i < numBBId; i++)
    {
        def_in[i].clear();
        def_out[i].clear();
    }

    if (livenessClass(G4_GRF))
        detectNeverDefinedVarRows();

    //
	// compute def_out and use_in vectors for each BB
	//
	for (BB_LIST_ITER it = fg.BBs.begin(); it != fg.BBs.end(); ++it)
	{
        G4_BB * bb = *it;
		unsigned id = bb->getId();
		if (computePseudoKill)
		{
			computeGenKillandPseudoKill((*it), def_out[id], use_in[id], use_gen[id], use_kill[id]);
		}
		else
		{
			computeGenKill((*it), def_out[id], use_in[id], use_gen[id], use_kill[id]);
		}

        //
        // exit block: mark output parameters live
        //
        if (bb->Succs.empty())
        {
            use_out[id] = outputUses;
        }
	}

    G4_BB* subEntryBB = NULL;
    BitSet* subEntryKill = NULL;
    BitSet* subEntryGen = NULL;

	if (fg.builder->getOptions()->getTarget() == VISA_CM)
	{
		//
		// Top-down order of BB list iteration guarantees that
		// entry BB of each sub-routine will be seen before any other
		// BBs belonging to that sub-routine. This assumes that BBs of
		// a sub-routine are laid out back to back in bb list.
		//
		for (BB_LIST_ITER it = fg.BBs.begin(); it != fg.BBs.end(); it++)
		{
			G4_BB* bb = (*it);
			unsigned id = bb->getId();

			if (bb->getScopeID() != 0 &&
				bb->getScopeID() != UINT_MAX)
			{
				subEntryBB = fg.sortedFuncTable[bb->getScopeID() - 1]->getInitBB();
				unsigned entryBBID = subEntryBB->getId();
				subEntryKill = &use_kill[entryBBID];
				subEntryGen = &use_gen[entryBBID];
			}

			//
			// Mark explicitly scoped variables as kills
			//
			performScoping(&use_gen[id], &use_kill[id], bb, subEntryGen, subEntryKill, subEntryBB);
		}
	}

	//
	// compute indr accesses
	//
    if( selectedRF & G4_GRF )
    {
        // only GRF variables can have their address taken
		for (BB_LIST_ITER it = fg.BBs.begin(); it != fg.BBs.end(); ++it)
        {
            G4_BB * bb = *it;
            const REGVAR_VECTOR* grfVecPtr = pointsToAnalysis.getIndrUseVectorPtrForBB( bb->getId() );
            for( unsigned i = 0; i < grfVecPtr->size(); i++ )
            {
                G4_RegVar* addrTaken =(*grfVecPtr)[i];
				indr_use[bb->getId()].set( addrTaken->getId(), true );
                addr_taken.set(addrTaken->getId(), true);
            }
        }
    }
	//
	// Perform inter-procedural context-sensitive flow analysis.
	// This is required when the CFG involves function calls with multiple calling
	// contexts for the same function, as peforming just a context-insensitive
	// analysis results in uses being propgated along paths that are not feasible
	// in the actual program.
	//

	if (performIPA) {
		//
		// Bitsets used to calcuate function summaries for inter-procedural liveness analysis.
		//
		std::vector<BitSet> bypass_in(numBBId);
		std::vector<BitSet> bypass_out(numBBId);
		std::vector<BitSet> bypass(numFnId);
		std::vector<BitSet>& mayuse_in(use_in);
		std::vector<BitSet>& mayuse_out(use_out);
		std::vector<BitSet> mayuse(numFnId);

		std::vector<BitSet> maydef_in(numBBId);
		std::vector<BitSet> maydef_out(numBBId);

		for (unsigned i = 0; i < numBBId; i++)
		{
			bypass_in[i]  = use_gen[i];
			bypass_out[i] = BitSet(numVarId, false);
			maydef_in[i]  = BitSet(numVarId, false);
			maydef_out[i] = def_out[i];
		}

		for (unsigned i = 0; i < numFnId; i++)
		{
			unsigned fid = fg.funcInfoTable[i]->getId();
			unsigned iid = fg.funcInfoTable[i]->getInitBB()->getId();
			unsigned eid = fg.funcInfoTable[i]->getExitBB()->getId();
			bypass[fid] = bypass_in[iid];
			mayuse[fid] = mayuse_in[iid];
			maydef[fid] = maydef_out[eid];
		}

		//
		// Determine use sets.
		//
		// Initialize set used for calculating function summaries for functions with multiple
		// callers.
		//
		std::list<G4_BB*>::iterator it = fg.BBs.begin();

		for ( ; it != fg.BBs.end(); ++it) {
			FuncInfo* funcInfoBB = (*it)->getCalleeInfo();

			if ((*it)->getBBType() & G4_BB_CALL_TYPE)
			{
				MUST_BE_TRUE(funcInfoBB != NULL, ERROR_REGALLOC);
				MUST_BE_TRUE((*it)->Succs.front()->getBBType() & G4_BB_INIT_TYPE, ERROR_REGALLOC);
				MUST_BE_TRUE((*it)->Succs.size() == 1, ERROR_REGALLOC);
			}
			else if ((*it)->getBBType() & G4_BB_RETURN_TYPE)
			{
				MUST_BE_TRUE((*it)->Preds.front()->getBBType() & G4_BB_EXIT_TYPE, ERROR_REGALLOC);
				MUST_BE_TRUE((*it)->Preds.size() == 1, ERROR_REGALLOC);
			}
			else if ((*it)->getBBType() & G4_BB_INIT_TYPE)
			{
				std::list<G4_BB*>::iterator jt = (*it)->Preds.begin();
				for ( ; jt != (*it)->Preds.end(); ++jt)
				{
					MUST_BE_TRUE((*jt)->getBBType() & G4_BB_CALL_TYPE, ERROR_REGALLOC);
				}
				if ((*it)->Preds.size() > 1)
				{
					MUST_BE_TRUE(
						(*it)->Preds.front()->getCalleeInfo()->doIPA() == true,
						 ERROR_REGALLOC);
				}
				else if ((*it)->Preds.size() > 0)
				{
					MUST_BE_TRUE(
						(*it)->Preds.front()->getCalleeInfo()->doIPA() == false,
						 ERROR_REGALLOC);
				}
			}
			else if ((*it)->getBBType() & G4_BB_EXIT_TYPE)
			{
				std::list<G4_BB*>::iterator jt = (*it)->Succs.begin();
				for ( ; jt != (*it)->Succs.end(); ++jt)
				{
					MUST_BE_TRUE((*jt)->getBBType() & G4_BB_RETURN_TYPE, ERROR_REGALLOC);
				}

				unsigned int bbid = (*it)->getId();
				//
				// Required pessimistic initialization for bypass out set.
				//
				bypass_out[bbid].setAll();
				//
				// Need to count in indir set to compute correct mayuse set
				//
				mayuse_out[bbid] = indr_use[bbid];
			}
		}
		//
		// Backward flow analysis to calculate use(live) sets.
		//    We perform three fixed point iterations.
		//    The first two are required to calculate function summaries.
		//    The function summaries are represented by two sets - the bypass set and the
		//    mayuse sets.
		//    The third one performs the actual liveness analysis considering each function
		//    call and its related subgraph as a blackbox, using just the calculated function
		//    summaries for it.
		//
		bool change = true;
		//
		// Phase (1) - determine bypass sets for each function
		//    The set of registers which if live at the RETURN NODE will be live at the
		//    CALL node. Typically these are the variables that are not used at all by
		//    the called function.
		//
		while (change)
		{
			change = false;
			BB_LIST::iterator rit = fg.BBs.end();
			do
			{
				//
				//    bypass_out[n] =
				//       (if type(n) == cgf_exit_block)
				//          indir[n] + output_uses[n]
				//       (if type(n) == call and f == callee[n])
				//			indir[n] + (bypass[f] and bypass_in[return_node(n)]
				//       (if type(n) != CALL and  type(n) != EXIT)
				//			indr_use[n] + bypass_in[s1] + bypass_in[s2] + ...
				//             where s1 s2 ... are the successors of n
				//    bypass_in[n]  = use[n] + (bypass_out - use_kill[n])
				//
				--rit;
				if (contextSensitiveBackwardDataAnalyze(
						(*rit), bypass_in, bypass_out, mayuse, bypass, outputUses, &bypass, G4_BB_EXIT_TYPE))
				{
					change = true;
				}
			}
			while(rit != fg.BBs.begin());
		}

		change = true;
		//
		// Phase (2) - determine mayuse sets for each function
		//    The set of registers that may be used by the called function. This describes
		//    the set of registers which are always live at INIT node independent of the
		//    calling context. Typically these are the registers that are used to pass
		//    arguments to the called function.
		//
		while (change)
		{
			change = false;
			BB_LIST::iterator rit = fg.BBs.end();
			do
			{
				//
				//    mayuse_out[n] =
				//       (if type(n) == cgf_exit_block)
				//          indir[n] + output_uses[n]
				//       (if type(n) == call and f == callee[n])
				//			indir[n] + mayuse[f] + (bypass[f] and mayuse_in[return_node(n)]
				//       (if type(n) != CALL and  type(n) != EXIT)
				//			indr_use[n] + mayuse_in[s1] + mayuse_in[s2] + ...
				//             where s1 s2 ... are the successors of n
				//    mayuse_in[n]  = use[n] + (mayuse_out - use_kill[n])
				//
				--rit;
				if (contextSensitiveBackwardDataAnalyze(
						(*rit), mayuse_in, mayuse_out, mayuse, bypass, outputUses, &mayuse, G4_BB_EXIT_TYPE))
				{
					change = true;
				}

			}
			while(rit != fg.BBs.begin());
		}
	//
	// dump vectors for debugging
	//
#ifdef DEBUG_VERBOSE_ON
	dump_bb_vector("BYPASS IN", fg.BBs, bypass_in);
	dump_bb_vector("BYPASS OUT", fg.BBs, bypass_out);
	dump_fn_vector("BYPASS", fns, bypass);
	dump_bb_vector("MAYUSE IN", fg.BBs, mayuse_in);
	dump_bb_vector("MAYUSE OUT", fg.BBs, mayuse_out);
	dump_fn_vector("MAYUSE", fns, mayuse);
#endif

		if (fg.builder->getOptions()->getTarget() == VISA_CM)
		{
			for (unsigned i = 0; i < numFnId; i++)
			{
				unsigned funcScopeID = fg.funcInfoTable[i]->getScopeID();
				for (unsigned j = 0; j < numVarId; j++)
				{
					G4_Declare *decl = vars[j]->getDeclare();
					unsigned declScopeID = decl->getScopeID();
					if (declScopeID != 0 &&
						funcScopeID >= declScopeID)
					{
						mayuse[i].set(j, false);
					}
				}
			}
		}

		//
		// The use_in/use_out sets will be initialized with the values of mayuse_in/mayuse_out
		// because the mayuse_in/mayuse_out sets are aliases to the use_in/use_out. This should
		// speed up the fixed-point iterations for use_in/use_out.
		//
		change = true;
		//
		// Phase (3) - determine use sets for each block
		//    Performs the actual liveness analysis considering each function call and its
		//    related subgraph as a blackbox, by using just the calculated function summaries
		//    for it.
		//
		while (change)
		{
			change = false;
			BB_LIST::iterator rit = fg.BBs.end();
			do
			{
				//
				//    live_out[n] =
				//       (if type(n) == cgf_exit_block)
				//          indir[n] + output_uses[n]
				//       (if type(n) == call and f == callee[n])
				//			indir[n] + mayuse[f] + (bypass[f] and live_in[return_node(n)]
				//       (if type(n) != CALL)
				//			indr_use[n] + use_in[s1] + use_in[s2] + ...
				//             where s1 s2 ... are the successors of n
				//    live_in[n]  = use[n] + (live_out - use_kill[n])
				//
				--rit;
				if (contextSensitiveBackwardDataAnalyze((*rit), use_in, use_out, mayuse, bypass, outputUses, NULL, 0))
				{
					change = true;
				}

			}
			while(rit != fg.BBs.begin());
		}

		//
		// Determine def sets.
		//

		//
		// Forward flow analysis to propagate defs.
		//    We perform two fixed iterations. The first is used to calculate the function
		//    summary required to propagate def. The function summary is represented by the
		//    maydef set.
		//    The second fixed point iteration performs the actual propagation of defs for the
		//    complete flow graph, considering each function call and its related subgraph as
		//    a blackbox, using just the calculated function summary for it.
		//
		change = true;
		//
		// Phase (1) - determine maydef sets for each function
		//    The maydef set represents the set of def that may be defined and thus propagated
		//    in the function along some path.
		//
		while (change)
		{
			change = false;
			for (BB_LIST::iterator it = fg.BBs.begin(); it != fg.BBs.end(); it++)
			{
				//
				// maydef_in[n] =
				//    (if type(n) == cgf_entry_block)
				//        input_defs[n]
				//    (if type(n) == return and f == callee[n])
				//        maydef[f] + maydef_out[call_node(n)]
				//           where type(n) == return and f == callee[n]
				//    (if type(n) != RETURN and  type(n) != INIT)
				//	      maydef_out[p1] + maydef_out[p2] + ...
				//           where p1 p2 ... are the predecessors of n
				//
				if (contextSensitiveForwardDataAnalyze(
						(*it), maydef_in, maydef_out, maydef, inputDefs, &maydef, G4_BB_INIT_TYPE))
				{
					change = true;
				}

			}
		}
	//
	// dump vectors for debugging
	//
#ifdef DEBUG_VERBOSE_ON
	dump_bb_vector("MAYDEF IN", fg.BBs, maydef_in);
	dump_bb_vector("MAYDEF OUT", fg.BBs, maydef_out);
	dump_fn_vector("MAYDEF", fns, maydef);
#endif

	    if (fg.builder->getOptions()->getTarget() == VISA_CM)
	    {
		    for (unsigned i = 0; i < numFnId; i++)
		    {
			    unsigned funcScopeID = fg.funcInfoTable[i]->getScopeID();
			    for (unsigned j = 0; j < numVarId; j++)
			    {
				    G4_Declare *decl = vars[j]->getDeclare();
				    unsigned declScopeID = decl->getScopeID();
					if (declScopeID != 0 &&
						funcScopeID >= declScopeID)
				    {
					    maydef[i].set(j, false);
				    }
			    }
            }
	    }

		change = true;
		//
		// Phase (2) - determine def sets for each block
		//    Performs the actual propagation of defs for the complete flow graph,
		//    considering each function call and its related subgraph as a blackbox,
		//    using just the calculated function summary for it.
		//
		while (change)
		{
			change = false;
			for (BB_LIST::iterator it = fg.BBs.begin(); it != fg.BBs.end(); it++)
			{
				//
				// def_in[n] =
				//    (if type(n) == cgf_entry_block)
				//        input_defs[n]
				//    (if type(n) == return and f == callee[n])
				//        def[f] + def_out[call_node(n)]
				//           where type(n) == return and f == callee[n]
				//    (if type(n) != RETURN)
				//	      def_out[p1] + def_out[p2] + ...
				//           where p1 p2 ... are the predecessors of n
				//
				if (contextSensitiveForwardDataAnalyze((*it), def_in, def_out, maydef, inputDefs, NULL, 0))
				{
					change = true;
				}
			}
		}
	}
	//
	// Peform intra-procedural context-insensitive flow analysis.
	//
	else {

        if (fg.builder->getOptions()->getTarget() == VISA_3D &&
            (selectedRF & G4_GRF || selectedRF & G4_FLAG) &&
            (numFnId > 0))
        {
            std::vector<BitSet> maydef_in(numBBId);
            std::vector<BitSet> maydef_out(numBBId);

            for (unsigned i = 0; i < numBBId; i++)
            {
                maydef_in[i] = BitSet(numVarId, false);
                maydef_out[i] = def_out[i];
            }

            for (unsigned i = 0; i < numFnId; i++)
            {
                unsigned fid = fg.funcInfoTable[i]->getId();
                unsigned eid = fg.funcInfoTable[i]->getExitBB()->getId();
                maydef[fid] = maydef_out[eid];
            }

            bool change = true;

            while (change)
            {
                change = false;
                for (BB_LIST::iterator it = fg.BBs.begin(); it != fg.BBs.end(); it++)
                {
                    //
                    // maydef_in[n] =
                    //    (if type(n) == cgf_entry_block)
                    //        input_defs[n]
                    //    (if type(n) == return and f == callee[n])
                    //        maydef[f] + maydef_out[call_node(n)]
                    //           where type(n) == return and f == callee[n]
                    //    (if type(n) != RETURN and  type(n) != INIT)
                    //	      maydef_out[p1] + maydef_out[p2] + ...
                    //           where p1 p2 ... are the predecessors of n
                    //
                    if (contextSensitiveForwardDataAnalyze(
                        (*it), maydef_in, maydef_out, maydef, inputDefs, &maydef, G4_BB_INIT_TYPE))
                    {
                        change = true;
                    }

                }
            }

            //
            // dump vectors for debugging
            //
#ifdef DEBUG_VERBOSE_ON
            dump_bb_vector("MAYDEF IN", fg.BBs, maydef_in);
            dump_bb_vector("MAYDEF OUT", fg.BBs, maydef_out);
            dump_fn_vector("MAYDEF", fns, maydef);
#endif
        }

		//
		// backward flow analysis to propagate uses (locate last uses)
		//
		bool change = true;
		while (change)
		{
			change = false;
			BB_LIST::iterator rit = fg.BBs.end();
			do
			{
				//
				// use_out = indr_use + use_in(s1) + use_in(s2) + ...
				// where s1 s2 ... are the successors of bb
				// use_in  = use_gen + (use_out - use_kill)
				//
				--rit;
				if (contextFreeUseAnalyze((*rit)))
				{
					change = true;
				}

			}
			while (rit != fg.BBs.begin());
		}
		//
		// forward flow analysis to propagate defs (locate first defs)
		//

		//
		// initialize entry block with payload input
		//
		def_in[fg.getEntryBB()->getId()] = inputDefs;
		change = true;
		while (change)
		{
			change = false;
			for (BB_LIST_ITER it = fg.BBs.begin(); it != fg.BBs.end(); it++)
			{
				G4_BB* bb = (*it);
				//
				// def_in   = def_out(p1) + def_out(p2) + ... where p1 p2 ... are the predecessors of bb
				// def_out |= def_in
				//
				if (contextFreeDefAnalyze(bb))
				{
					change = true;
				}
			}
		}
	}

	//
	// dump vectors for debugging
	//
#ifdef DEBUG_VERBOSE_ON
	dump_bb_vector("DEF IN", fg.BBs, def_in);
	dump_bb_vector("DEF OUT", fg.BBs, def_out);
	dump_bb_vector("USE IN", fg.BBs, use_in);
	dump_bb_vector("USE OUT", fg.BBs, use_out);
	dump_bb_vector("USE GEN", fg.BBs, use_gen);
	dump_bb_vector("USE KILL", fg.BBs, use_kill);
#endif
}

//
// determine if the dst writes the whole region of target declare
//
bool LivenessAnalysis::writeWholeRegion(G4_BB* bb,
										G4_INST* inst,
										G4_DstRegRegion* dst,
                                        const Options *opt) const
{
	unsigned execSize = inst->getExecSize();
	MUST_BE_TRUE(dst->getBase()->isRegVar(), ERROR_REGALLOC);

    if( bb->isInSimdFlow() && !inst->isWriteEnableInst() && opt->getTarget() != VISA_3D )
    {
        // conservatively assume non-nomask instructions in simd control flow
        // may not write the whole region
        return false;
    }

    // Flags may be partially written when used as the destination
    // e.g., setp (M5_NM, 16) P11 V97(8,0)<0;1,0>
    // It can be only considered as a complete kill
    // if the computed bound diff matches with the number of flag elements
    if (dst->isFlag() == true )
    {
        if( (dst->getRightBound() - dst->getLeftBound() + 1) ==
            dst->getBase()->asRegVar()->getDeclare()->getNumberFlagElements() )
        {
        return true;
    }
        else
        {
            return false;
        }
    }

	//
	// Find Primary Variable Declare
	//

	G4_Declare* decl = ((G4_RegVar*)dst->getBase())->getDeclare();
	G4_Declare* primaryDcl = decl;
    while( primaryDcl->getAliasDeclare() )
    {
        primaryDcl = primaryDcl->getAliasDeclare();
    }

	//
	//  Cannot write whole register if
	//     * alias offset in non zero
	//     * reg or sub-reg offset is non zero
	//     * horiz stride is non zero
	//     * predicate is non null
	//
	if (decl->getAliasOffset() != 0 ||
		dst->getRegAccess() != Direct ||
		dst->getRegOff() != 0 ||
		dst->getSubRegOff() != 0 ||
		dst->getHorzStride() != 1 ||
		inst->isPartialWrite() ) {
		return false;
	}

     //
    // For CISA3, pseudo-callee-save and pseudo-caller-save insts
    // are kills
    //
    if (fg.isPseudoDcl(primaryDcl))
    {
        return true;
    }

   	//
    // If the region does not cover the whole declare then it does not write the whole region.
   	//

    if (G4_Type_Table[dst->getType()].byteSize * execSize !=
	    G4_Type_Table[primaryDcl->getElemType()].byteSize * primaryDcl->getNumElems() * primaryDcl->getNumRows()) {
   		return false;
    }

	return true;
}

//
// determine if the dst writes the whole region of target declare
//
bool LivenessAnalysis::writeWholeRegion(G4_BB* bb,
										G4_INST* inst,
                                        G4_VarBase* flagReg,
                                        const Options *opt)
{
    if( bb->isInSimdFlow() && !inst->isWriteEnableInst() && opt->getTarget() != VISA_3D )
    {
        // conservatively assume non-nomask instructions in simd control flow
        // may not write the whole region
        return false;
    }

    G4_Declare* decl = flagReg->asRegVar()->getDeclare();
    if( inst->getExecSize() != decl->getNumberFlagElements() )
    {
        return false;
    }

    return true;
}

// Set bits in dst footprint based on dst region's left/right bound
void LivenessAnalysis::footprintDst( G4_BB* bb,
                  G4_INST* i,
                  G4_Operand* opnd,
                  BitSet* dstfootprint,
                  bool isLocal)
{
    if( dstfootprint != NULL &&
        !(i->isPartialWrite()) &&
        ( ( isLocal ||
            bb->isInSimdFlow() == false ||
            i->isWriteEnableInst() == true ) ||
            gra.kernel.getOptions()->getTarget() == VISA_3D ) )
    {
        // Bitwise OR left-bound/right-bound with dst footprint to indicate
        // bytes that are written in to
        unsigned int lb, rb, bitvec;
        lb = opnd->getLeftBound();
        rb = opnd->getRightBound();
		if (lb % G4_GRF_REG_NBYTES == 0 && 
			(rb + 1) % G4_GRF_REG_NBYTES == 0)
		{
			unsigned idx = lb / G4_GRF_REG_NBYTES;
			unsigned endIdx = rb / G4_GRF_REG_NBYTES;

			bitvec = opnd->getBitVecL();
			dstfootprint->setElt(idx, bitvec);

			idx++;
			if (idx <= endIdx)
			{
				bitvec = opnd->getBitVecH();
				dstfootprint->setElt(idx, bitvec);
			}

			idx++;
			while (idx <= endIdx)
			{
				dstfootprint->setElt(idx, 0xFFFFFFFF);
				idx++;
			}
		}
		else
		{
			bitvec = opnd->getBitVecL();
			for (unsigned int j = lb,
				bit = 0; j <= rb; j++, bit++)
			{
				if (bit == 32)
				{
					bitvec = opnd->getBitVecH();
					bit = 0;
				}

				if (i->isSend() || (bitvec & (1 << bit)))
				{
					dstfootprint->set(j, true);
				}
			}
		}
    }
}

// Reset bits in srcfootprint based on src region's left/right bound
void LivenessAnalysis::footprintSrc( G4_INST* i,
                  G4_Operand *opnd,
                  BitSet* srcfootprint )
{
    // Reset bits in kill map footprint
    unsigned int lb, rb, bitvec;
    lb = opnd->getLeftBound();
	rb = opnd->getRightBound();
	if (lb % G4_GRF_REG_NBYTES == 0 && 
		(rb + 1) % G4_GRF_REG_NBYTES == 0)
	{
		unsigned idx = lb / G4_GRF_REG_NBYTES;
		unsigned endIdx = rb / G4_GRF_REG_NBYTES;

		bitvec = opnd->getBitVecL();
		srcfootprint->resetElt(idx, bitvec);

		idx++;
		if (idx <= endIdx)
		{
			bitvec = opnd->getBitVecH();
			srcfootprint->resetElt(idx, bitvec);
		}

		idx++;
		while (idx <= endIdx)
		{
			srcfootprint->resetElt(idx, 0xFFFFFFFF);
			idx++;
		}
	}
	else
	{
		bitvec = opnd->getBitVecL();
		for (unsigned int j = lb,
			bit = 0; j <= rb; j++, bit++)
		{
			if (bit == 32)
			{
				bitvec = opnd->getBitVecH();
				bit = 0;
			}

			if (i->isSend() || (bitvec & (1 << bit)))
			{
				srcfootprint->set(j, false);
			}
		}
	}
}

void LivenessAnalysis::computeGenKill(G4_BB* bb,
	BitSet& def_out,
	BitSet& use_in,
	BitSet& use_gen,
	BitSet& use_kill)
{
	//
	// Mark each fcall as using all globals and arg pre-defined var
	//
	if (bb->isEndWithFCall() && (selectedRF & G4_GRF))
	{
		for (auto globals = fileScopeVars.begin();
			globals != fileScopeVars.end();
			globals++)
		{
			use_gen.set((*globals)->getId(), true);
			use_kill.set((*globals)->getId(), true);
			def_out.set((*globals)->getId(), true);
		}

		G4_Declare* arg = fg.builder->getStackCallArg();
		G4_Declare* ret = fg.builder->getStackCallRet();

		G4_FCALL* fcall = fg.builder->getFcallInfo(bb->instList.back());
		MUST_BE_TRUE(fcall != NULL, "fcall info not found");

		// arg var is a use and a kill at each fcall
		if (fcall->getArgSize() != 0)
		{
			use_gen.set(arg->getRegVar()->getId(), true);
		}
		use_kill.set(arg->getRegVar()->getId(), true);

		// ret var is a kill at each fcall
		use_kill.set(ret->getRegVar()->getId(), true);
		if (fcall->getRetSize() != 0)
		{
			def_out.set(ret->getRegVar()->getId(), true);
		}
	}

	for (INST_LIST::reverse_iterator rit = bb->instList.rbegin(); rit != bb->instList.rend(); ++rit)
	{
		G4_INST* i = (*rit);
		G4_DstRegRegion* dst = i->getDst();

		if (i->opcode() == G4_pseudo_lifetime_end)
		{
			continue;
		}

		if (dst != NULL)
		{
			G4_DstRegRegion* dstrgn = (G4_DstRegRegion*)dst;

			if (dstrgn->getBase()->isRegAllocPartaker())
			{
				G4_Declare* topdcl = GetTopDclFromRegRegion(dstrgn);
				unsigned id = topdcl->getRegVar()->getId();

				if (i->opcode() == G4_pseudo_kill)
				{
					// Mark kill, reset gen
					use_kill.set(id, true);
					use_gen.set(id, false);

					continue;
				}

				if (dstrgn->getRegAccess() == Direct)
				{
					def_out.set(id, true);
					//
					// if the inst writes the whole region the var declared, we set use_kill
					// so that use of var will not pass through (i.e., var's interval starts
					// at this instruction.
					//
					if (writeWholeRegion(bb, i, dstrgn, fg.builder->getOptions()))
					{
						use_kill.set(id, true);
						use_gen.set(id, false);
					}
					else
					{
						use_gen.set(id, true);
					}
				}
				else
				{
					use_gen.set(id, true);
				}
			}
		}

		//
		// process each source operand
		//
		for (unsigned j = 0; j < G4_MAX_SRCS; j++)
		{
			G4_Operand* src = i->getSrc(j);

			if (src == NULL)
			{
				continue;
			}
			if (src->isSrcRegRegion())
			{
				G4_Declare* topdcl = GetTopDclFromRegRegion(src);
				G4_VarBase* base = (topdcl != NULL ? topdcl->getRegVar() :
					src->asSrcRegRegion()->getBase());

				if (base->isRegAllocPartaker())
				{
					use_gen.set(((G4_RegVar*)base)->getId(), true);
				}
			}
			//
			// treat the addr expr as both a use and a partial def
			//
			else if (src->isAddrExp() && // (&BLK)
				((G4_AddrExp*)src)->getRegVar()->isRegAllocPartaker() &&
				((G4_AddrExp*)src)->getRegVar()->isSpilled() == false)
			{
				unsigned srcId = ((G4_AddrExp*)src)->getRegVar()->getId();
				use_gen.set(srcId, true);
				def_out.set(srcId, true);
			}
		}

		//
		// Process condMod
		//
		G4_CondMod* mod = i->getCondMod();
		if (mod != NULL) {
			G4_VarBase *flagReg = mod->getBase();
			if (flagReg != NULL)
			{
				if (flagReg->asRegVar()->isRegAllocPartaker())
				{
					G4_Declare* topdcl = flagReg->asRegVar()->getDeclare();
					MUST_BE_TRUE(topdcl->getAliasDeclare() == NULL, "Invalid alias flag decl.");
					unsigned id = topdcl->getRegVar()->getId();

					def_out.set(id, true);

					if (writeWholeRegion(bb, i, flagReg, fg.builder->getOptions()))
					{
						use_kill.set(id, true);
						use_gen.set(id, false);
					}
					else
					{
						use_gen.set(id, true);
					}
				}
			}
			else
			{
				MUST_BE_TRUE((i->opcode() == G4_sel ||
					i->opcode() == G4_csel) &&
					i->getCondMod() != NULL,
					"Invalid CondMod");
			}
		}

		//
		// Process predicate
		//
		G4_Predicate* predicate = i->getPredicate();
		if (predicate != NULL) {
			G4_VarBase *flagReg = predicate->getBase();
			MUST_BE_TRUE(flagReg->asRegVar()->getDeclare()->getAliasDeclare() == NULL, "Invalid alias flag decl.");
			if (flagReg->asRegVar()->isRegAllocPartaker())
			{
				use_gen.set(((G4_RegVar*)flagReg)->getId(), true);
			}
		}
	}

	//
	// initialize use_in
	//
	use_in = use_gen;

	return;
}

void LivenessAnalysis::computeGenKillandPseudoKill(G4_BB* bb,
									 BitSet& def_out,
									 BitSet& use_in,
									 BitSet& use_gen,
									 BitSet& use_kill)
{
	std::vector<BitSet*> footprints;
	footprints.resize(numVarId, 0);
	std::vector<std::pair<G4_RegVar*, INST_LIST_RITER>> pseudoKills;
    std::stack<BitSet*> toDelete;

    //
    // Mark each fcall as using all globals and arg pre-defined var
    //
    if (bb->isEndWithFCall() && (selectedRF & G4_GRF))
    {
        for (auto globals = fileScopeVars.begin();
            globals != fileScopeVars.end();
            globals++)
        {
            use_gen.set((*globals)->getId(), true);
            use_kill.set((*globals)->getId(), true);
            def_out.set((*globals)->getId(), true);
        }

        G4_Declare* arg = fg.builder->getStackCallArg();
        G4_Declare* ret = fg.builder->getStackCallRet();

        G4_FCALL* fcall = fg.builder->getFcallInfo(bb->instList.back());
        MUST_BE_TRUE(fcall != NULL, "fcall info not found");

        if (arg->getByteSize() != 0)
        {
            // arg var is a use and a kill at each fcall
            if (fcall->getArgSize() != 0)
            {
                use_gen.set(arg->getRegVar()->getId(), true);
            }
            use_kill.set(arg->getRegVar()->getId(), true);
        }

        if (ret->getByteSize() != 0)
        {
            // ret var is a kill at each fcall
            use_kill.set(ret->getRegVar()->getId(), true);
            if (fcall->getRetSize() != 0)
            {
                def_out.set(ret->getRegVar()->getId(), true);
            }
        }
    }

	for (INST_LIST::reverse_iterator rit = bb->instList.rbegin(); rit != bb->instList.rend(); ++rit)
	{
		G4_INST* i = (*rit);
        G4_DstRegRegion* dst = i->getDst();

		if(i->opcode() == G4_pseudo_lifetime_end)
		{
			continue;
		}

		if (dst != NULL)
		{
			G4_DstRegRegion* dstrgn = dst;
            BitSet* dstfootprint = NULL;

			if (dstrgn->getBase()->isRegAllocPartaker())
			{
                G4_Declare* topdcl = GetTopDclFromRegRegion( dstrgn );
                unsigned id = topdcl->getRegVar()->getId();

				if(i->opcode() == G4_pseudo_kill)
				{
					// Mark kill, reset gen
					use_kill.set(id, true);
					use_gen.set(id, false);

					continue;
				}

				dstfootprint = footprints[id];

				if (dstfootprint == NULL)
                {
					// Write for dst was not seen before, so insert in to map
					// bitsetSize is in bytes
					unsigned int bitsetSize = (dstrgn->isFlag()) ? topdcl->getNumberFlagElements() : topdcl->getByteSize();

					BitSet* newBitSet;
					newBitSet = new (m) BitSet( bitsetSize, false );

                    auto it = neverDefinedRows.find(topdcl);
                    if (it != neverDefinedRows.end())
                    {
                        // Bitwise OR new bitset with never defined rows
                        *newBitSet |= *it->second;
                    }

					toDelete.push(newBitSet);
					pair<BitSet*, INST_LIST_RITER> second(newBitSet, bb->instList.rbegin() );
					footprints[id] = newBitSet;
					dstfootprint = newBitSet;
					if( gra.isBlockLocal(topdcl) &&
						topdcl->getAddressed() == false &&
						dstrgn->getRegAccess() == Direct )
					{
						// Local live ranges are never live-out of the only
						// basic block they are defined. So in top-down order
						// the first lexical definition is a kill irrespective
						// of the footprint. In cases when local live-range
						// def and use have h-stride != 1, the footprint at this
						// lexically first definition will not have all bits set.
						// This prevents that def to be seen as a kill. A simple
						// solution to this is to set all bits when initializing
						// the bitvector while iterating in bottom-up order. As
						// we traverse further up uses will reset bits and defs
						// will set bits. So when we encounter the lexically first
						// def, we will be guaranteed to find all bits set, thus
						// interpreting that def as a kill.
						dstfootprint->setAll();
					}
				}

				if (dstrgn->getRegAccess() == Direct)
				{
                    def_out.set( id, true );
					//
					// if the inst writes the whole region the var declared, we set use_kill
					// so that use of var will not pass through (i.e., var's interval starts
					// at this instruction.
					//
					if (writeWholeRegion(bb, i, dstrgn, fg.builder->getOptions()))
					{
						use_kill.set( id, true );
						use_gen.set( id, false );

                        dstfootprint->setAll();
					}
					else
					{
                        footprintDst( bb, i, dstrgn, dstfootprint,  gra.isBlockLocal(topdcl) );

						use_gen.set( id, true );
					}
				}
				else
				{
					use_gen.set( id, true );
				}
			}
            else if ((selectedRF & G4_GRF) && dst->isIndirect())
            {
                // conservatively add each variable potentially accessed by dst to gen
                auto pointsToSet = pointsToAnalysis.getAllInPointsTo(dst->getBase()->asRegVar());
                if (pointsToSet == nullptr)
                {
                    pointsToSet = pointsToAnalysis.getIndrUseVectorPtrForBB(bb->getId());
                }
                for (auto var : *pointsToSet)
                {
                    if (var->isRegAllocPartaker())
                    {
                        use_gen.set(var->getId(), true);
                    }
                }
            }
        }

        //
		// process each source operand
		//
		for (unsigned j = 0; j < G4_MAX_SRCS; j++)
		{
			G4_Operand* src = i->getSrc(j);
            BitSet* srcfootprint = NULL;

			if (src == NULL)
			{
				continue;
			}
			if (src->isSrcRegRegion())
			{
                G4_Declare* topdcl = GetTopDclFromRegRegion( src );
                G4_VarBase* base = (topdcl != NULL ? topdcl->getRegVar() :
                    src->asSrcRegRegion()->getBase());

				if (base->isRegAllocPartaker())
				{
					unsigned id = topdcl->getRegVar()->getId();
					srcfootprint = footprints[id];

					if (srcfootprint != NULL)
					{
                        footprintSrc( i, src->asSrcRegRegion(), srcfootprint );
                    }
                    else
                    {
                        unsigned int bitsetSize = (src->asSrcRegRegion()->isFlag()) ? topdcl->getNumberFlagElements() : topdcl->getByteSize();

                        BitSet* newBitSet;
                        newBitSet = new (m) BitSet( bitsetSize, false );

                        auto it = neverDefinedRows.find(topdcl);
                        if (it != neverDefinedRows.end())
                        {
                            // Bitwise OR new bitset with never defined rows
                            *newBitSet |= *it->second;
                        }

                        toDelete.push(newBitSet);
						footprints[id] = newBitSet;
                        srcfootprint = newBitSet;
                        if( gra.isBlockLocal(topdcl) &&
                            topdcl->getAddressed() == false &&
                            (topdcl->getRegFile() == G4_ADDRESS ||
                             src->asSrcRegRegion()->getRegAccess() == Direct) )
                        {
                            srcfootprint->setAll();
                        }
                        footprintSrc( i, src->asSrcRegRegion(), srcfootprint );
                    }

					use_gen.set( ((G4_RegVar*)base)->getId(), true );
				}

                if( (selectedRF & G4_GRF) && src->getRegAccess() == IndirGRF )
                {
                    int idx = 0;
                    G4_RegVar* grf;
                    G4_Declare* topdcl = GetTopDclFromRegRegion( src );

                    while( (grf = pointsToAnalysis.getPointsTo( topdcl->getRegVar(), idx++ )) != NULL )
                    {
                        // grf is a variable that src potentially points to
                        // since we dont know exactly which part of grf is sourced
                        // assume entire grf is sourced
                        // Also add grf to the gen set as it may be potentially used
						unsigned int id = grf->getId();
                        use_gen.set(id, true);
						srcfootprint = footprints[id];

						if (srcfootprint != NULL)
                        {
                            srcfootprint->clear();

                            DEBUG_VERBOSE("Found potential indirect use of " << grf->getDeclare()->getName() <<
                                " so resetting its footprint" << std::endl );
                        }
                    }
                }
			}
			//
			// treat the addr expr as both a use and a partial def
			//
			else if (src->isAddrExp() && // (&BLK)
					 ((G4_AddrExp*)src)->getRegVar()->isRegAllocPartaker() &&
                     ((G4_AddrExp*)src)->getRegVar()->isSpilled() == false)
			{
				unsigned srcId = ((G4_AddrExp*)src)->getRegVar()->getId();
				use_gen.set( srcId, true );
				def_out.set( srcId, true );
			}
        }

        //
        // Process condMod
        //
        G4_CondMod* mod = i->getCondMod();
        if(mod != NULL) {
            G4_VarBase *flagReg = mod->getBase();
            if( flagReg != NULL )
            {
			    if (flagReg->asRegVar()->isRegAllocPartaker())
			    {
                    BitSet* dstfootprint = NULL;

                    G4_Declare* topdcl = flagReg->asRegVar()->getDeclare();
                    MUST_BE_TRUE(topdcl->getAliasDeclare() == NULL, "Invalid alias flag decl.");
                    unsigned id = topdcl->getRegVar()->getId();

					dstfootprint = footprints[id];

					if (dstfootprint == NULL)
                    {
                        // Write for dst was not seen before, so insert in to map
                        // bitsetSize is in bits for flag
                        unsigned int bitsetSize = topdcl->getNumberFlagElements();

                        BitSet* newBitSet;
                        newBitSet = new (m) BitSet( bitsetSize, false );
                        toDelete.push(newBitSet);
                        pair<BitSet*, INST_LIST_RITER> second(newBitSet, bb->instList.rbegin() );
						footprints[id] = newBitSet;
                        dstfootprint = newBitSet;

                        if( gra.isBlockLocal(topdcl) )
                        {
                            dstfootprint->setAll();
                        }
                    }

                    def_out.set( id, true );

				    if (writeWholeRegion(bb, i, flagReg, fg.builder->getOptions()))
				    {
					    use_kill.set( id, true );
					    use_gen.set( id, false );

                        dstfootprint->setAll();
				    }
				    else
				    {
                        footprintDst( bb, i, mod, dstfootprint, gra.isBlockLocal(topdcl) );
					    use_gen.set( id, true );
				    }
			    }
            }
            else
            {
                MUST_BE_TRUE((i->opcode() == G4_sel ||
                              i->opcode() == G4_csel) &&
                              i->getCondMod() != NULL,
                             "Invalid CondMod");
            }
        }

        //
        // Process predicate
        //
        G4_Predicate* predicate = i->getPredicate();
        if(predicate != NULL) {
            G4_VarBase *flagReg = predicate->getBase();
            MUST_BE_TRUE(flagReg->asRegVar()->getDeclare()->getAliasDeclare() == NULL, "Invalid alias flag decl.");
			if (flagReg->asRegVar()->isRegAllocPartaker())
			{
				G4_Declare* topdcl = flagReg->asRegVar()->getDeclare();
				unsigned id = topdcl->getRegVar()->getId();
				BitSet* srcfootprint = footprints[id];

				if (srcfootprint != NULL)
                {
                    footprintSrc( i, predicate, srcfootprint );
                }
                else
                {
                    G4_Declare* topdcl = flagReg->asRegVar()->getDeclare();
                    unsigned int bitsetSize = topdcl->getNumberFlagElements();
					
					BitSet* newBitSet;
                    newBitSet = new (m) BitSet( bitsetSize, false );
                    toDelete.push(newBitSet);
                    pair<BitSet*, INST_LIST_RITER> second(newBitSet, bb->instList.rbegin() );
					footprints[id] = newBitSet;
                    srcfootprint = newBitSet;
                    if( gra.isBlockLocal(topdcl) )
                    {
                        srcfootprint->setAll();
                    }
                    footprintSrc( i, predicate, srcfootprint );
                }

				use_gen.set( ((G4_RegVar*)flagReg)->getId(), true );
			}
        }

        //
        // Check whether dst can is killed at this point
        // A block of code is said to kill a variable when union
        // of all partial writes causes all elements to be written
        // into and any reads in the block can be sourced from
        // writes within that block itself
        //
        if( dst != NULL && dst->getBase()->isRegAllocPartaker() )
        {
            G4_Declare* topdcl = GetTopDclFromRegRegion( dst );
			unsigned id = topdcl->getRegVar()->getId();
			BitSet* dstfootprint = footprints[id];

			if (dstfootprint != NULL)
            {
                // Found dst in map
                // Check whether all bits set
				// pseudo_kill for this dst was not found in this BB yet
				bool wholeRegionWritten = false;
				unsigned int first;
                LocalLiveRange* topdclLR = nullptr;

				if ((dstfootprint->isAllset() ||
                    // Check whether local RA marked this range
                    ( topdcl &&
                    (topdclLR = gra.getLocalLR(topdcl)) &&
                    topdclLR->isLiveRangeLocal() &&
                    topdclLR->getFirstRef( first ) == i ) ) &&
                    // If single inst writes whole region then dont insert pseudo_kill
                    ( ( wholeRegionWritten = LivenessAnalysis::writeWholeRegion( bb, i, dst, fg.builder->getOptions() ) ) == false) )
                {
                    bool foundKill = false;
                    INST_LIST::reverse_iterator nextIt = rit; 
                    ++nextIt;
                    if (nextIt != bb->instList.rend())
                    {
                        G4_INST* nextInst = (*nextIt);
                        if (nextInst->opcode() == G4_pseudo_kill)
                        {
                            G4_DstRegRegion* nextDst = nextInst->getDst();

                            if (nextDst != NULL &&
                                nextDst->isDstRegRegion() &&
                                nextDst->getBase()->isRegAllocPartaker() &&
                                topdcl == GetTopDclFromRegRegion(nextDst))
                {
                                foundKill = true;
                            }
                        }
                    }
                    if (!foundKill)
                    {
                    // All bytes of dst written at this point, so this is a good place to insert
                    // pseudo kill inst
					pseudoKills.push_back(std::pair<G4_RegVar*, INST_LIST_RITER>(topdcl->getRegVar(), rit));
                    }

                    // Reset gen
                    use_gen.set( dst->getBase()->asRegVar()->getId(), false );

                    // Set kill
                    use_kill.set( dst->getBase()->asRegVar()->getId(), true );
#ifdef DEBUG_VERBOSE_ON
                    DEBUG_VERBOSE( "Found kill at inst " );
                    INST_LIST_ITER fwdIter = rit.base();
                    fwdIter--;
                    (*fwdIter)->emit_inst(std::cout, false, NULL);
                    DEBUG_VERBOSE( " // $" << (*fwdIter)->getCISAOff() );
                    DEBUG_VERBOSE( std::endl );
#endif
                }
            }
        }

        if( mod != NULL && mod->getBase() != NULL && mod->getBase()->asRegVar()->isRegAllocPartaker() )
        {
            G4_VarBase *flagReg = mod->getBase();
            G4_Declare* topdcl = flagReg->asRegVar()->getDeclare();
			unsigned id = topdcl->getRegVar()->getId();
			BitSet* dstfootprint = footprints[id];

			if (dstfootprint != NULL)
            {
				bool wholeRegionWritten = false;
				unsigned int first;
                LocalLiveRange* topdclLR = nullptr;
				if ((dstfootprint->isAllset() ||
                    // Check whether local RA marked this range
                    // This may not be necessary as currently local RA is not performed for flags.
                    ( topdcl &&
                    (topdclLR = gra.getLocalLR(topdcl)) &&
                    topdclLR->isLiveRangeLocal() &&
                    topdclLR->getFirstRef( first ) == i ) ) &&
                    // If single inst writes whole region then dont insert pseudo_kill
                    ( ( wholeRegionWritten = LivenessAnalysis::writeWholeRegion( bb, i, flagReg, fg.builder->getOptions() ) ) == false) )
                {
                    // All bytes of dst written at this point, so this is a good place to insert
                    // pseudo kill inst
					pseudoKills.push_back(std::pair<G4_RegVar*, INST_LIST_RITER>(topdcl->getRegVar(), rit));

                    // Reset gen
                    use_gen.set( flagReg->asRegVar()->getId(), false );

                    // Set kill
                    use_kill.set( flagReg->asRegVar()->getId(), true );
#ifdef DEBUG_VERBOSE_ON
                    DEBUG_VERBOSE( "Found kill at inst " );
                    INST_LIST_ITER fwdIter = rit.base();
                    fwdIter--;
                    (*fwdIter)->emit_inst(std::cout, false, NULL);
                    DEBUG_VERBOSE( " // $" << (*fwdIter)->getCISAOff() );
                    DEBUG_VERBOSE( std::endl );
#endif
                }
            }
        }
	}

    //
    // Insert pseudo_kill nodes in BB
    //
    for( auto&& pseudoKill : pseudoKills )
    {
		if (pseudoKill.second != bb->instList.rbegin())
        {
			INST_LIST_ITER iterToInsert = pseudoKill.second.base();
            do
            {
                iterToInsert--;
            }
            while( (*iterToInsert)->isPseudoKill() );
			G4_DstRegRegion* dstOpnd = fg.builder->createDstRegRegion(Direct, pseudoKill.first, 0, 0, 1, Type_UD);
            G4_INST* killInst = fg.builder->createInternalInst( NULL, G4_pseudo_kill, NULL, false, 1, dstOpnd, NULL, NULL, 0 );
            bb->instList.insert( iterToInsert, killInst );
        }
    }

	//
	// initialize use_in
	//
	use_in = use_gen;

    //
    // Destroy bitsets allocated using mem manager
    //
    while( toDelete.size() > 0 )
    {
        toDelete.top()->~BitSet();
        toDelete.pop();
    }

	return;
}

//
// Context sensitive backward flow analysis used for IPA.
//
bool LivenessAnalysis::contextSensitiveBackwardDataAnalyze(
         G4_BB* bb,
		 std::vector<BitSet>& data_in,
		 std::vector<BitSet>& data_out,
		 std::vector<BitSet>& mayuse,
		 std::vector<BitSet>& bypass,
		 BitSet&               output_uses,
		 std::vector<BitSet>* summary,
		 int no_prop_types)
{
	bool changed  = false;
	unsigned bbid = bb->getId();

	//
	// Handle the exit block
	// data_out[n] = indir[n] + output_uses[n]
	//
	if (bb->Succs.empty())
	{
		data_out[bbid]  = output_uses;
        data_out[bbid] |= indr_use[bbid];
		changed = false;
	}
	//
	// Handle call blocks that belong to the same graph cut (inter-procedural boundaries)
	// data_out[n] = indir[n] + mayuse[f] + (bypass[f] & data_in[return_node(n)]
	// where type(n) == call and f == callee[n]
	//
	else if (bb->getBBType() & G4_BB_CALL_TYPE)
	{
		BitSet old(std::move(data_out[bbid]));

		data_out[bbid] = indr_use[bbid];

		FuncInfo* callee = bb->getCalleeInfo();
		data_out[bbid] |= mayuse[callee->getId()];

		BitSet prop_data(bypass[callee->getId()]);
		prop_data &= data_in[bb->BBAfterCall()->getId()];
		data_out[bbid] |= prop_data;

		changed = (old != data_out[bbid]);
	}
	//
	// Handle all other blocks acroos which we need to propagate flow information
	// data_out = indr_use + data_in(s1) + data_in(s2) + ...
	// where s1 s2 ... are the successors of bb
	//
	else if (!(bb->getBBType() & no_prop_types))
	{
		BitSet old(std::move(data_out[bbid]));

		data_out[bbid] = indr_use[bbid];

		for (BB_LIST_ITER it = bb->Succs.begin(); it != bb->Succs.end(); it++)
		{
			data_out[bbid] |= data_in[(*it)->getId()];
		}

		changed = (old != data_out[bbid]);
	}

	//
	// data_in = use_gen + (data_out - use_kill)
	//
	data_in[bbid] = data_out[bbid];
    data_in[bbid] -= use_kill[bbid];
    data_in[bbid] |= use_gen[bbid];

	//
	// summary = data_in[init_node(f)]
	//
	if (summary)
	{
		if ( bb->getBBType() == G4_BB_INIT_TYPE )
		{
			FuncInfo* itsFuncInfo = bb->getFuncInfo();
			MUST_BE_TRUE(itsFuncInfo->getInitBB() == bb, ERROR_REGALLOC);
			(*summary)[itsFuncInfo->getId()] = data_in[bbid];
		}
	}

	return changed;
}

//
// Context sensitive forward flow analysis
//
bool LivenessAnalysis::contextSensitiveForwardDataAnalyze(
         G4_BB* bb,
		 std::vector<BitSet>& data_in,
		 std::vector<BitSet>& data_out,
		 std::vector<BitSet>& maydef,
		 BitSet&               input_defs,
		 std::vector<BitSet>* summary,
		 int no_prop_types)
{
	bool changed  = false;
	unsigned bbid = bb->getId();

	//
	// Handle the entry block
	// data_in[n] = input_defs
	//
	if (bb->Preds.empty())
	{
		data_in[bbid] = input_defs;
		changed = false;
	}
	//
	// Handle call blocks that belong to the same graph cut (inter-procedural boundaries)
	// data_in[n] = maydef[f] + data_out[call_node(n)]
	// where type(n) == return and f == callee[n]
	//
	else if (bb->getBBType() & G4_BB_RETURN_TYPE)
	{
		BitSet old(std::move(data_in[bbid]));

		FuncInfo* callee = bb->BBBeforeCall()->getCalleeInfo();

		data_in[bbid] = maydef[callee->getId()];
		data_in[bbid] |= data_out[bb->BBBeforeCall()->getId()];

		changed = (old != data_in[bbid]);
	}
	//
	// Handle all other blocks across which we need to propagate flow information
	// data_in = data_out(p1) + data_out(p2) + ...
	// where p1 p2 ... are the predecessors of bb
	//
	else if (!(bb->getBBType() & no_prop_types))
	{
		BitSet old(std::move(data_in[bbid]));

		for (BB_LIST_ITER it = bb->Preds.begin(); it != bb->Preds.end(); it++)
		{
			data_in[bbid] |= data_out[(*it)->getId()];
		}

		changed = (old != data_in[bbid]);
	}

	//
	// data_out += data_in
	//
    data_out[bbid] |= data_in[bbid];

	//
	// summary = data_out[exit_node(f)]
	//
	if (summary)
	{
		if ( bb->getBBType() == G4_BB_EXIT_TYPE )
		{
			FuncInfo* itsFuncInfo = bb->getFuncInfo();
			MUST_BE_TRUE(itsFuncInfo->getExitBB() == bb, ERROR_REGALLOC);
			(*summary)[itsFuncInfo->getId()] = data_out[bbid];
		}
	}

	return changed;
}

//
// use_out = indr_use + use_in(s1) + use_in(s2) + ... where s1 s2 ... are the successors of bb
// use_in  = use_gen + (use_out - use_kill)
//
bool LivenessAnalysis::contextFreeUseAnalyze(G4_BB* bb)
{
	bool changed;

	unsigned bbid = bb->getId();

	if (bb->Succs.empty()) // exit block
	{
		changed = false;
	}

	else
	{
		BitSet old = use_out[bbid];

		for (BB_LIST_ITER it = bb->Succs.begin(); it != bb->Succs.end(); it++)
		{
			use_out[bbid] |= use_in[(*it)->getId()];
		}

		changed = (old != use_out[bbid]);
	}

	//
	// in = gen + (out - kill)
	//
	use_in[bbid] = use_out[bbid];
    use_in[bbid] -= use_kill[bbid];
    use_in[bbid] |= use_gen[bbid];

	return changed;
}

//
// def_in = def_out(p1) + def_out(p2) + ... where p1 p2 ... are the predecessors of bb
// def_out |= def_in
//
bool LivenessAnalysis::contextFreeDefAnalyze(G4_BB* bb)
{
	bool changed  = false;
	unsigned bbid = bb->getId();

	if (bb->Preds.empty())
	{
		changed = false;
	}
	else
	{
		BitSet old = def_in[bbid];

		for (BB_LIST_ITER it = bb->Preds.begin(); it != bb->Preds.end(); it++)
		{
			def_in[bbid] |= def_out[(*it)->getId()];
		}

		changed = (old != def_in[bbid]);
	}

	 def_out[bb->getId()] |= def_in[bb->getId()];

	 return changed;
}

void LivenessAnalysis::dump_bb_vector(char* vname, std::list<G4_BB*>& bbs, std::vector<BitSet>& vec)
{
	DEBUG_VERBOSE(vname << std::endl);
	for (BB_LIST_ITER it = fg.BBs.begin(); it != fg.BBs.end(); it++)
	{
		G4_BB* bb = (*it);
		DEBUG_VERBOSE("    BB" << bb->getId() << std::endl);
        const BitSet& in = vec[bb->getId()];
		DEBUG_VERBOSE("        ");
        for (unsigned i = 0; i < in.getSize(); i+= 10)
		{
			//
			// dump 10 bits a group
			//
			for (unsigned j = i; j < in.getSize() && j < i+10; j++)
			{
                DEBUG_VERBOSE(in.isSet(j) ? "1" : "0");
			}
			DEBUG_VERBOSE(" ");
		}
		DEBUG_VERBOSE(std::endl);
	}
}

void LivenessAnalysis::dump_fn_vector(char* vname, std::vector<FuncInfo*>& fns, std::vector<BitSet>& vec)
{
	DEBUG_VERBOSE(vname << std::endl);
	for (std::vector<FuncInfo*>::iterator it = fns.begin(); it != fns.end(); it++)
	{
		FuncInfo* funcInfo = (*it);

        DEBUG_VERBOSE("    FN" << funcInfo->getId() << std::endl);
        const BitSet& in = vec[funcInfo->getId()];
        DEBUG_VERBOSE("        ");
        for (unsigned i = 0; i < in.getSize(); i += 10)
        {
            //
            // dump 10 bits a group
            //
            for (unsigned j = i; j < in.getSize() && j < i + 10; j++)
            {
                DEBUG_VERBOSE(in.isSet(j) ? "1" : "0");
            }
            DEBUG_VERBOSE(" ");
        }
        DEBUG_VERBOSE(std::endl);
    }
}

//
// dump which vars are live at the entry of BB
//
void LivenessAnalysis::dump() const
{
    for (auto bb : fg.BBs)
    {
        std::cerr << "BB" << bb->getId() << "'s live in: ";
        unsigned total_size = 0;
        auto dumpVar = [&total_size](G4_RegVar* var)
        {
            int size = var->getDeclare()->getTotalElems() * var->getDeclare()->getElemSize();
            std::cerr << var->getName() << "(" << size << "), ";
            total_size += size;
        };

        unsigned count = 0;
        for (auto var : vars)
        {
            if (var->isRegAllocPartaker() && isLiveAtEntry(bb, var->getId()))
            {
                if (count++ % 10 == 0) std::cerr << "\n";
                dumpVar(var);
            }
        }
        std::cerr << "\nBB" << bb->getId() << "'s live in size: " << total_size / GENX_GRF_REG_SIZ << "\n\n";
        std::cerr << "BB" << bb->getId() << "'s live out: ";
        total_size = 0;
        count = 0;
        for (auto var : vars)
        {
            if (var->isRegAllocPartaker() && isLiveAtExit(bb, var->getId()))
            {
                if (count++ % 10 == 0) std::cerr << "\n";
                dumpVar(var);
            }
        }
        std::cerr << "\nBB" << bb->getId() << "'s live out size: " << total_size / GENX_GRF_REG_SIZ<< "\n\n";
    }
}

//
// return true if var is live at the entry of bb
// check both use_in and def_in, if one condition fails then var is not in the live range
//
bool LivenessAnalysis::isLiveAtEntry(G4_BB* bb, unsigned var_id) const
{
    return use_in[bb->getId()].isSet( var_id ) && def_in[bb->getId()].isSet( var_id );
}
//
// return true if var is live at the exit of bb
//
bool LivenessAnalysis::isLiveAtExit(G4_BB* bb, unsigned var_id) const
{
	return use_out[bb->getId()].isSet( var_id ) && def_out[bb->getId()].isSet( var_id );
}


void GlobalRA::markBlockLocalVar(G4_RegVar* var, unsigned bbId)
{
	G4_Declare* dcl = var->getDeclare();

    while( dcl->getAliasDeclare() != NULL )
    {
        dcl = dcl->getAliasDeclare();
    }

    if( dcl->getHasFileScope() ||
    	dcl->isInput() ||
     	dcl->isOutput())
    {
        // Filescope variables are never block local
        setBBId(dcl, UINT_MAX - 1);
    }
    else
    {
    	if (getBBId(dcl) == bbId)
	    {
		    // Do nothing.
    	}
	    else if (getBBId(dcl) == UINT_MAX)
    	{
    		setBBId(dcl, bbId);
	    }
    	else {
	    	setBBId(dcl, UINT_MAX - 1);
	    }
    }
}

void GlobalRA::markBlockLocalVars(G4_BB* bb, Mem_Manager& mem, bool doLocalRA)
{
	for (std::list<G4_INST*>::iterator it = bb->instList.begin(); it != bb->instList.end(); it++)
	{
		G4_INST* inst = *it;

		// Track direct dst references.

		G4_DstRegRegion* dst = inst->getDst();

		if (dst != NULL)
		{
            G4_DstRegRegion* dstRgn = dst->asDstRegRegion();

            if (dstRgn->getBase()->isRegVar()) {
                markBlockLocalVar(dstRgn->getBase()->asRegVar(), bb->getId());

                G4_Declare* topdcl = GetTopDclFromRegRegion(dst);
                if (topdcl)
                {
                    if (inst->isSend())
                    {
                        topdcl->setIsRefInSendDcl(true);
                    }
                    if (!doLocalRA || dst->isFlag() || dst->isAddress())
                {
                    LocalLiveRange* lr = GetOrCreateLocalLiveRange(topdcl, mem);
                    unsigned int startIdx;
                    if (lr->getFirstRef(startIdx) == NULL)
                    {
                        lr->setFirstRef(inst, 0);
                    }
                    lr->recordRef(bb);
                    recordRef(topdcl);
                }
            }
		}
        }

        G4_CondMod* condMod = inst->getCondMod();

        if( condMod != NULL &&
            condMod->getBase() != NULL )
        {
            if( condMod->getBase() && condMod->getBase()->isRegVar() )
            {
                markBlockLocalVar( condMod->getBase()->asRegVar(), bb->getId() );

                G4_Declare* topdcl = condMod->getBase()->asRegVar()->getDeclare();
                if (topdcl)
                {
                    LocalLiveRange* lr = GetOrCreateLocalLiveRange(topdcl, mem);
				    unsigned int startIdx;
				    if( lr->getFirstRef( startIdx ) == NULL )
				    {
				        lr->setFirstRef( inst, 0 );
                    }
                    lr->recordRef( bb );
                    recordRef(topdcl);
                }
            }
        }

		// Track direct src references.

		for (unsigned j = 0; j < G4_MAX_SRCS; j++)
		{
			G4_Operand* src = inst->getSrc(j);

			if (src == NULL)
			{
				// Do nothing.
			}
			else if (src->isSrcRegRegion() && src->asSrcRegRegion()->getBase()->isRegVar())
			{
				G4_SrcRegRegion* srcRgn = src->asSrcRegRegion();

				if (srcRgn->getBase()->isRegVar()) {
					markBlockLocalVar(src->asSrcRegRegion()->getBase()->asRegVar(), bb->getId());

                    G4_Declare* topdcl = GetTopDclFromRegRegion( src );
                    if (topdcl)
                    {
                        if (inst->isSend())
                        {
                            topdcl->setIsRefInSendDcl(true);
                        }

                        if (!doLocalRA || src->isFlag() || src->isAddress())
                    {
                        LocalLiveRange* lr = GetOrCreateLocalLiveRange(topdcl, mem);

                        lr->recordRef( bb );
                        recordRef(topdcl);
                            if (inst->isEOT())
                            {
                                lr->markEOT();
                            }
                        }
                    }
                }
			}
			else if (src->isAddrExp())
			{
                G4_RegVar* addExpVar = src->asAddrExp()->getRegVar();
				markBlockLocalVar(addExpVar, bb->getId());

                if (!doLocalRA)
                {
		            G4_Declare* topdcl = addExpVar->getDeclare();
		            while( topdcl->getAliasDeclare() != NULL)
			            topdcl = topdcl->getAliasDeclare();
		            MUST_BE_TRUE( topdcl != NULL, "Top dcl was null for addr exp opnd");

                    LocalLiveRange* lr = GetOrCreateLocalLiveRange(topdcl, mem);
                    lr->recordRef(bb);
                    lr->markIndirectRef();
                    recordRef(topdcl);
                }
			}
		}

        G4_Operand* pred = inst->getPredicate();

        if( pred != NULL )
        {
            if( pred->getBase() && pred->getBase()->isRegVar() )
            {
                markBlockLocalVar( pred->getBase()->asRegVar(), bb->getId() );
                G4_Declare* topdcl = pred->getBase()->asRegVar()->getDeclare();
                if (topdcl)
                {
                    LocalLiveRange* lr = GetOrCreateLocalLiveRange(topdcl, mem);
                    lr->recordRef( bb );
                    recordRef(topdcl);
                }
            }
        }

		// Track all indirect references.
		const REGVAR_VECTOR* grfVecPtr = pointsToAnalysis.getIndrUseVectorPtrForBB( bb->getId() );
		for( unsigned i = 0; i < grfVecPtr->size(); i++ )
		{
			markBlockLocalVar((*grfVecPtr)[i], bb->getId());
		}
	}
}

//
// Mark block local (temporary) variables.
//
void GlobalRA::markGraphBlockLocalVars()
{
    auto& fg = kernel.fg;
	for (std::list<G4_BB*>::iterator it = fg.BBs.begin(); it != fg.BBs.end(); ++it)
	{
        markBlockLocalVars(*it, fg.builder->mem, fg.builder->getOption(vISA_LocalRA));
	}
}

//
// Pre-assign phy regs to stack call function return variable as per ABI.
//
void GlobalRA::setABIForStackCallFunctionCalls()
{
	// For each G4_pseudo_fcall inst, create dst of GRF type
	// with physical register 1.0 pre-assigned to it.
	// Similarly, for G4_pseudo_fret create src of GRF type
	// with physical register 1.0 pre-assigned to it.
	// Each will use 2 dwords of r1.0.
	int call_id = 0, ret_id = 0;

	for( BB_LIST_ITER it = kernel.fg.BBs.begin();
		it != kernel.fg.BBs.end();
		it++ )
	{
		G4_BB* bb = (*it);
		if( bb->isEndWithFCall() )
		{
            char* n = kernel.fg.builder->getNameString(kernel.fg.mem, 25,
                kernel.fg.builder->getIsKernel() ? "FCALL_RET_LOC_k_%d" : "FCALL_RET_LOC_f%d_%d", 
                kernel.fg.builder->getCUnitId(), call_id++);

			G4_INST* fcall = bb->instList.back();
			G4_Declare* r1_dst = kernel.fg.builder->createDeclareNoLookup(n, G4_GRF, 8, 1, Type_UD);
			r1_dst->getRegVar()->setPhyReg(regPool.getGreg(1), 0);
            G4_DstRegRegion* dstRgn = kernel.fg.builder->createDstRegRegion(Direct, r1_dst->getRegVar(), 0, 0, 1, Type_UD);
			fcall->setDest(dstRgn);
		}

		if( bb->isEndWithFRet() )
		{
            char* n = kernel.fg.builder->getNameString(kernel.fg.mem, 25,
                kernel.fg.builder->getIsKernel() ? "FRET_RET_LOC_k_%d" : "FRET_RET_LOC_f%d_%d",
                kernel.fg.builder->getCUnitId(), ret_id++);
			G4_INST* fret = bb->instList.back();
			RegionDesc* rd = kernel.fg.builder->createRegionDesc(2, 2, 1);
			G4_Declare* r1_src = kernel.fg.builder->createDeclareNoLookup(n, G4_INPUT, 8, 1, Type_UD);
			r1_src->setIsFretLoc();
			r1_src->getRegVar()->setPhyReg(regPool.getGreg(1), 0);
            G4_Operand* srcRgn = kernel.fg.builder->createSrcRegRegion(Mod_src_undef, Direct, r1_src->getRegVar(), 0, 0, rd, Type_UD);
			fret->setSrc(srcRgn, 0);
            if (fret->getExecSize() == 1)
            {
                //due to <2;2,1> regioning we must update exec size as well
                fret->setExecSize(2);
            }
            if (kernel.getOption(vISA_GenerateDebugInfo))
            {
                kernel.getKernelDebugInfo()->setFretVar(GetTopDclFromRegRegion(fret->getSrc(0)));
            }
		}
	}
}

// Function to verify RA results
void GlobalRA::verifyRA(LivenessAnalysis & liveAnalysis)
{
    for (BB_LIST_ITER it = kernel.fg.BBs.begin(); it != kernel.fg.BBs.end(); ++it)
    {
        G4_BB * bb = *it;

        // Verify PREG assignment
        for (auto inst : bb->instList)
        {
            G4_DstRegRegion* dst = inst->getDst();
            if (dst != NULL &&
                dst->getBase()->isRegAllocPartaker())
            {
                MUST_BE_TRUE(dst->getBase()->asRegVar()->getPhyReg(), "RA verification error: No PREG assigned for variable " << GetTopDclFromRegRegion(dst)->getName() << "!");
            }

            for (unsigned j = 0; j < G4_MAX_SRCS; j++)
            {
                G4_Operand* src = inst->getSrc(j);
                if (src != NULL &&
                    src->isSrcRegRegion() &&
                    src->asSrcRegRegion()->getBase()->isRegAllocPartaker())
                {
                    MUST_BE_TRUE(src->asSrcRegRegion()->getBase()->asRegVar()->getPhyReg(),
                        "RA verification error: No PREG assigned for variable " << GetTopDclFromRegRegion(src->asSrcRegRegion())->getName() << "!");
                }
            }
        }

        int numGRF = kernel.getOptions()->getuInt32Option(vISA_TotalGRFNum);
        // Verify Live-in
        std::map<uint32_t, G4_Declare*> LiveInRegMap;
        std::map<uint32_t, G4_Declare*>::iterator LiveInRegMapIt;
        std::vector<uint32_t> liveInRegVec;
        liveInRegVec.resize(numGRF * G4_GRF_REG_SIZE, UINT_MAX);

        for (DECLARE_LIST_ITER dcl_it = kernel.Declares.begin();
            dcl_it != kernel.Declares.end();
            dcl_it++)
        {
            if ((*dcl_it)->getAliasDeclare() != NULL)
                continue;

            if ((*dcl_it)->getRegVar()->isRegAllocPartaker())
            {
                G4_Declare* dcl = (*dcl_it);
                G4_RegVar* var = dcl->getRegVar();
                uint32_t varID = var->getId();
                if (liveAnalysis.isLiveAtEntry((*it), (*dcl_it)->getRegVar()->getId()))
                {
                    MUST_BE_TRUE(var->getPhyReg()->isGreg(), "RA verification error: Invalid preg assignment for variable " << dcl->getName() << "!");

                    uint32_t regNum = var->getPhyReg()->asGreg()->getRegNum();
                    uint32_t regOff = var->getPhyRegOff();

                    uint32_t idx = regNum * G4_GRF_REG_SIZE +
                        (regOff * G4_Type_Table[dcl->getElemType()].byteSize) / G4_WSIZE;
                    for (uint32_t i = 0; i < dcl->getWordSize(); ++i, ++idx)
                    {
                        LiveInRegMapIt = LiveInRegMap.find(idx);
                        if (liveInRegVec[idx] != UINT_MAX)
                        {
                            MUST_BE_TRUE(LiveInRegMapIt != LiveInRegMap.end(), "RA verification error: Invalid entry in LiveInRegMap!");
                            if (dcl->isInput())
                            {
                                DEBUG_MSG("RA verification warning: Found conflicting input variables: " << dcl->getName()
                                    << " and " << (*LiveInRegMapIt).second->getName() << " assigned to r" << regNum
                                    << "." << regOff << "!" << endl);
                                liveInRegVec[idx] = varID;
                                (*LiveInRegMapIt).second = dcl;
                            }
                            else
                            {
                                MUST_BE_TRUE(false, "RA verification error: Found conflicting live-in variables: " << dcl->getName()
                                    << " and " << (*LiveInRegMapIt).second->getName() << " assigned to r" <<
                                    regNum << "." << regOff << "!" << endl);
                            }

                        }
                        else
                        {
                            liveInRegVec[idx] = varID;
                            MUST_BE_TRUE(LiveInRegMapIt == LiveInRegMap.end(), "RA verification error: Invalid entry in LiveInRegMap!");
                            LiveInRegMap.insert(make_pair(idx, dcl));
                        }
                    }
                }
            }
        }

        // Verify Live-out
        G4_Declare *ret = kernel.fg.builder->getStackCallRet();
        std::map<uint32_t, G4_Declare*> liveOutRegMap;
        std::map<uint32_t, G4_Declare*>::iterator liveOutRegMapIt;
        std::vector<uint32_t> liveOutRegVec;
        liveOutRegVec.resize(numGRF * G4_GRF_REG_SIZE, UINT_MAX);

        for (DECLARE_LIST_ITER dcl_it = kernel.Declares.begin();
            dcl_it != kernel.Declares.end();
            dcl_it++)
        {
            if ((*dcl_it)->getAliasDeclare() != NULL)
                continue;

            if ((*dcl_it)->getRegVar()->isRegAllocPartaker())
            {
                G4_Declare* dcl = (*dcl_it);
                G4_RegVar* var = dcl->getRegVar();
                uint32_t varID = var->getId();
                if (liveAnalysis.isLiveAtExit(bb, varID))
                {
                    MUST_BE_TRUE(var->getPhyReg()->isGreg(), "RA verification error: Invalid preg assignment for variable " << dcl->getName() << "!");

                    uint32_t regNum = var->getPhyReg()->asGreg()->getRegNum();
                    uint32_t regOff = var->getPhyRegOff();

                    uint32_t idx = regNum * G4_GRF_REG_SIZE +
                        (regOff * G4_Type_Table[dcl->getElemType()].byteSize) / G4_WSIZE;
                    for (uint32_t i = 0; i < dcl->getWordSize(); ++i, ++idx)
                    {
                        liveOutRegMapIt = liveOutRegMap.find(idx);
                        if (liveOutRegVec[idx] != UINT_MAX)
                        {
                            MUST_BE_TRUE(liveOutRegMapIt != liveOutRegMap.end(), "RA verification error: Invalid entry in liveOutRegMap!");
                            if (dcl->isInput())
                            {
                                DEBUG_MSG("RA verification warning: Found conflicting input variables: " << dcl->getName()
                                    << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" << regNum
                                    << "." << regOff << "!" << endl);
                                liveOutRegVec[idx] = varID;
                                (*liveOutRegMapIt).second = dcl;
                            }
                            else
                            {
                                MUST_BE_TRUE(false, "RA verification error: Found conflicting live-out variables: " << dcl->getName()
                                    << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" <<
                                    regNum << "." << regOff << "!" << endl);
                            }

                        }
                        else
                        {
                            liveOutRegVec[idx] = varID;
                            MUST_BE_TRUE(liveOutRegMapIt == liveOutRegMap.end(), "RA verification error: Invalid entry in liveOutRegMap!");
                            liveOutRegMap.insert(make_pair(idx, dcl));
                        }
                    }
                }
            }
        }

        for (INST_LIST::reverse_iterator rit = bb->instList.rbegin(); rit != bb->instList.rend(); ++rit)
        {
            G4_INST* inst = (*rit);

            G4_DstRegRegion* dst = inst->getDst();

            //
            // verify dst operand
            //
            if (dst != NULL)
            {
                if (dst->getBase()->isRegAllocPartaker())
                {
                    G4_DstRegRegion* dstrgn = dst;
                    G4_RegVar* var = dstrgn->getBase()->asRegVar();
                    uint32_t varID = var->getId();
                    G4_Declare* dcl = GetTopDclFromRegRegion(dstrgn);
                    MUST_BE_TRUE(dcl != nullptr, "Null declare found");
                    var = dcl->getRegVar();

                    MUST_BE_TRUE(var->getId() == varID, "RA verification error: Invalid regVar ID!");
                    MUST_BE_TRUE(var->getPhyReg()->isGreg(), "RA verification error: Invalid dst reg!");

                    uint32_t regNum = var->getPhyReg()->asGreg()->getRegNum();
                    uint32_t regOff = var->getPhyRegOff();

                    uint32_t idx = regNum * G4_GRF_REG_SIZE +
                        (regOff * G4_Type_Table[dcl->getElemType()].byteSize) / G4_WSIZE;
                    for (uint32_t i = 0; i < dcl->getWordSize(); ++i, ++idx)
                    {
                        liveOutRegMapIt = liveOutRegMap.find(idx);
                        if (liveOutRegVec[idx] == UINT_MAX)
                        {
                            MUST_BE_TRUE(liveOutRegMapIt == liveOutRegMap.end(), "RA verification error: Invalid entry in liveOutRegMap!");
                            DEBUG_MSG("RA verification warning: Found unused variable " << dcl->getName() << "!" << endl);
                        }
                        else
                        {
                            MUST_BE_TRUE(liveOutRegMapIt != liveOutRegMap.end(), "RA verification error: Invalid entry in liveOutRegMap!");
                            if (liveOutRegVec[idx] != varID)
                            {
                                const BitSet& indr_use = liveAnalysis.indr_use[bb->getId()];

                                if (strstr(dcl->getName(), GraphColor::StackCallStr) != NULL)
                                {
                                    DEBUG_MSG("RA verification warning: Found conflicting stackCall variable: " << dcl->getName()
                                        << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" <<
                                        regNum << "." << regOff << "!" << endl);
                                }
                                else if (indr_use.isSet(liveOutRegVec[idx]) == true)
                                {
                                    MUST_BE_TRUE(false, "RA verification warning: Found conflicting indirect variables: " << dcl->getName()
                                        << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" <<
                                        regNum << "." << regOff << "!" << endl);
                                }
                                else
                                {
                                    MUST_BE_TRUE(false, "RA verification error: Found conflicting variables: " << dcl->getName()
                                        << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" <<
                                        regNum << "." << regOff << "!" << endl);
                                }
                            }

                            if (liveAnalysis.writeWholeRegion(bb, inst, dstrgn, kernel.getOptions()) ||
                                inst->isPseudoKill()) {
                                liveOutRegVec[idx] = UINT_MAX;
                                MUST_BE_TRUE(liveOutRegMapIt != liveOutRegMap.end(), "RA verification error: Invalid entry in liveOutRegMap!");
                                liveOutRegMap.erase(liveOutRegMapIt);
                            }
                        }
                    }
                }
                else if (dst->getRegAccess() == IndirGRF)
                {
                    G4_DstRegRegion* dstrgn = dst;
                    G4_Declare* addrdcl = GetTopDclFromRegRegion(dstrgn);
                    G4_RegVar* ptvar = NULL;
                    int vid = 0;

                    while ((ptvar = pointsToAnalysis.getPointsTo(addrdcl->getRegVar(), vid++)) != NULL)
                    {
                        uint32_t varID = ptvar->getId();
                        G4_Declare* dcl = ptvar->getDeclare();
                        G4_RegVar* var = NULL;
                        MUST_BE_TRUE(dcl != nullptr, "Null declare found");
                        while (dcl->getAliasDeclare())
                        {
                            dcl = dcl->getAliasDeclare();
                        }
                        var = dcl->getRegVar();

                        MUST_BE_TRUE(var->getId() == varID, "RA verification error: Invalid regVar ID!");
                        MUST_BE_TRUE(var->getPhyReg()->isGreg(), "RA verification error: Invalid dst reg!");

                        uint32_t regNum = var->getPhyReg()->asGreg()->getRegNum();
                        uint32_t regOff = var->getPhyRegOff();

                        uint32_t idx = regNum * G4_GRF_REG_SIZE +
                            (regOff * G4_Type_Table[dcl->getElemType()].byteSize) / G4_WSIZE;
                        for (uint32_t i = 0; i < dcl->getWordSize(); ++i, ++idx)
                        {
                            liveOutRegMapIt = liveOutRegMap.find(idx);
                            if (liveOutRegVec[idx] == UINT_MAX)
                            {
                                MUST_BE_TRUE(liveOutRegMapIt == liveOutRegMap.end(), "RA verification error: Invalid entry in liveOutRegMap!");
                                DEBUG_MSG("RA verification warning: Found unused variable " << dcl->getName() << "!" << endl);
                            }
                            else
                            {
                                MUST_BE_TRUE(liveOutRegMapIt != liveOutRegMap.end(), "RA verification error: Invalid entry in liveOutRegMap!");
                                if (liveOutRegVec[idx] != varID)
                                {
                                    const BitSet& indr_use = liveAnalysis.indr_use[bb->getId()];

                                    if (strstr(dcl->getName(), GraphColor::StackCallStr) != NULL)
                                    {
                                        DEBUG_MSG("RA verification warning: Found conflicting stackCall variables: " << dcl->getName()
                                            << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" <<
                                            regNum << "." << regOff << "!" << endl);
                                    }
                                    else if (indr_use.isSet(liveOutRegVec[idx]) == true)
                                    {
                                        MUST_BE_TRUE(false, "RA verification warning: Found conflicting indirect variables: " << dcl->getName()
                                            << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" <<
                                            regNum << "." << regOff << "!" << endl);
                                    }
                                    else
                                    {
                                        MUST_BE_TRUE(false, "RA verification error: Found conflicting variables: " << dcl->getName()
                                            << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" <<
                                            regNum << "." << regOff << "!" << endl);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (inst->opcode() == G4_pseudo_fcall)
            {
                if (ret != NULL && ret->getRegVar() != NULL)
                {
                    G4_RegVar* var = ret->getRegVar();
                    uint32_t varID = var->getId();
                    MUST_BE_TRUE(var->getId() == varID, "RA verification error: Invalid regVar ID!");
                    MUST_BE_TRUE(var->getPhyReg()->isGreg(), "RA verification error: Invalid dst reg!");

                    uint32_t regNum = var->getPhyReg()->asGreg()->getRegNum();
                    uint32_t regOff = var->getPhyRegOff();

                    uint32_t idx = regNum * G4_GRF_REG_SIZE +
                        (regOff * G4_Type_Table[ret->getElemType()].byteSize) / G4_WSIZE;
                    for (uint32_t i = 0; i < ret->getWordSize(); ++i, ++idx)
                    {
                        liveOutRegMapIt = liveOutRegMap.find(idx);
                        liveOutRegVec[idx] = UINT_MAX;
                        MUST_BE_TRUE(liveOutRegMapIt != liveOutRegMap.end(), "RA verification error: Invalid entry in liveOutRegMap!");
                        liveOutRegMap.erase(liveOutRegMapIt);
                    }
                }
            }

            //
            // verify each source operand
            //
            for (unsigned j = 0; j < G4_MAX_SRCS; j++)
            {
                G4_Operand* src = inst->getSrc(j);
                if (src == NULL)
                {
                    continue;
                }
                if (src->isAddrExp() && src->asAddrExp()->isRegAllocPartaker())
                {
                    G4_RegVar* var = src->asAddrExp()->getRegVar();
                    uint32_t varID = UINT_MAX;
                    G4_Declare* dcl = NULL;

                    varID = var->getId();
                    dcl = var->getDeclare();

                    uint32_t regNum = var->getPhyReg()->asGreg()->getRegNum();
                    uint32_t regOff = var->getPhyRegOff();

                    uint32_t idx = regNum * G4_GRF_REG_SIZE +
                        (regOff * G4_Type_Table[dcl->getElemType()].byteSize) / G4_WSIZE;
                    for (uint32_t i = 0; i < dcl->getWordSize(); ++i, ++idx)
                    {
                        liveOutRegMapIt = liveOutRegMap.find(idx);
                        if (liveOutRegVec[idx] == UINT_MAX)
                        {
                            liveOutRegVec[idx] = varID;
                            MUST_BE_TRUE(liveOutRegMapIt == liveOutRegMap.end(), "RA verification error: Invalid entry in liveOutRegMap!");
                            liveOutRegMap.insert(make_pair(idx, dcl));
                        }
                        else
                        {
                            if (liveOutRegVec[idx] != varID)
                            {
                                const BitSet& indr_use = liveAnalysis.indr_use[bb->getId()];

                                if (dcl->isInput())
                                {
                                    DEBUG_MSG("RA verification warning: Found conflicting input variables: " << dcl->getName()
                                        << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" << regNum
                                        << "." << regOff << "!" << endl);
                                    liveOutRegVec[idx] = varID;
                                    (*liveOutRegMapIt).second = dcl;
                                }
                                else if (strstr(dcl->getName(), GraphColor::StackCallStr) != NULL)
                                {
                                    DEBUG_MSG("RA verification warning: Found conflicting stackCall variables: " << dcl->getName()
                                        << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" <<
                                        regNum << "." << regOff << "!" << endl);
                                }
                                else if (indr_use.isSet(liveOutRegVec[idx]) == true)
                                {
                                    MUST_BE_TRUE(false, "RA verification warning: Found conflicting indirect variables: " << dcl->getName()
                                        << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" <<
                                        regNum << "." << regOff << "!" << endl);
                                }
                                else
                                {
                                    INST_LIST::reverse_iterator succ = rit;
                                    ++succ;
                                    bool idMismatch = false;
                                    G4_Declare* topdcl = GetTopDclFromRegRegion((*succ)->getDst());
                                    if (topdcl != nullptr &&
                                        liveOutRegVec[idx] != topdcl->getRegVar()->getId())
                                    {
                                        idMismatch = true;
                                    }
                                    if (succ == bb->instList.rend() ||
                                        (*succ)->opcode() != G4_pseudo_kill ||
                                        (*succ)->getDst() == NULL ||
                                        idMismatch)
                                    {
                                        MUST_BE_TRUE(false, "RA verification error: Found conflicting variables: " << dcl->getName()
                                            << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" <<
                                            regNum << "." << regOff << "!" << endl);
                                    }
                                }
                            }
                        }
                    }
                }
                else if (src->isSrcRegRegion() && ((G4_SrcRegRegion*)src)->getBase()->isRegAllocPartaker())
                {
                    G4_SrcRegRegion* srcrgn = src->asSrcRegRegion();
                    G4_RegVar* var = srcrgn->getBase()->asRegVar();
                    uint32_t varID = var->getId();
                    G4_Declare* dcl = GetTopDclFromRegRegion(srcrgn);
                    var = dcl->getRegVar();

                    MUST_BE_TRUE(var->getId() == varID, "RA verification error: Invalid regVar ID!");
                    MUST_BE_TRUE(var->getPhyReg()->isGreg(), "RA verification error: Invalid dst reg!");

                    if (inst->opcode() != G4_pseudo_lifetime_end)
                    {
                        uint32_t regNum = var->getPhyReg()->asGreg()->getRegNum();
                        uint32_t regOff = var->getPhyRegOff();

                        uint32_t idx = regNum * G4_GRF_REG_SIZE +
                            (regOff * G4_Type_Table[dcl->getElemType()].byteSize) / G4_WSIZE;
                        for (uint32_t i = 0; i < dcl->getWordSize(); ++i, ++idx)
                        {
                            liveOutRegMapIt = liveOutRegMap.find(idx);
                            if (liveOutRegVec[idx] == UINT_MAX)
                            {
                                liveOutRegVec[idx] = varID;
                                MUST_BE_TRUE(liveOutRegMapIt == liveOutRegMap.end(), "RA verification error: Invalid entry in liveOutRegMap!");
                                liveOutRegMap.insert(make_pair(idx, dcl));
                            }
                            else
                            {
                                if (liveOutRegVec[idx] != varID)
                                {
                                    const BitSet& indr_use = liveAnalysis.indr_use[bb->getId()];

                                    if (dcl->isInput())
                                    {
                                        DEBUG_MSG("RA verification warning: Found conflicting input variables: " << dcl->getName()
                                            << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" << regNum
                                            << "." << regOff << "!" << endl);
                                        liveOutRegVec[idx] = varID;
                                        (*liveOutRegMapIt).second = dcl;
                                    }
                                    else if (strstr(dcl->getName(), GraphColor::StackCallStr) != NULL)
                                    {
                                        DEBUG_MSG("RA verification warning: Found conflicting stackCall variables: " << dcl->getName()
                                            << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" <<
                                            regNum << "." << regOff << "!" << endl);
                                    }
                                    else if (indr_use.isSet(liveOutRegVec[idx]) == true)
                                    {
                                        MUST_BE_TRUE(false, "RA verification warning: Found conflicting indirect variables: " << dcl->getName()
                                            << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" <<
                                            regNum << "." << regOff << "!" << endl);
                                    }
                                    else
                                    {
                                        INST_LIST::reverse_iterator succ = rit;
                                        ++succ;
                                        bool idMismatch = false;
                                        G4_Declare* topdcl = GetTopDclFromRegRegion((*succ)->getDst());
                                        if (topdcl != nullptr &&
                                            liveOutRegVec[idx] != topdcl->getRegVar()->getId())
                                        {
                                            idMismatch = true;
                                        }
                                        if (succ == bb->instList.rbegin() ||
                                            (*succ)->opcode() != G4_pseudo_kill ||
                                            (*succ)->getDst() == NULL ||
                                            idMismatch)
                                        {
                                            MUST_BE_TRUE(false, "RA verification error: Found conflicting variables: " << dcl->getName()
                                                << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" <<
                                                regNum << "." << regOff << "!" << endl);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    else
                    {
                        uint32_t regNum = var->getPhyReg()->asGreg()->getRegNum();
                        uint32_t regOff = var->getPhyRegOff();

                        uint32_t idx = regNum * G4_GRF_REG_SIZE +
                            (regOff * G4_Type_Table[dcl->getElemType()].byteSize) / G4_WSIZE;
                        for (uint32_t i = 0; i < dcl->getWordSize(); ++i, ++idx)
                        {
                            if (liveOutRegVec[idx] != UINT_MAX)
                            {
                                liveOutRegMapIt = liveOutRegMap.find(idx);
                                MUST_BE_TRUE(liveOutRegMapIt != liveOutRegMap.end(), "RA verification error: Invalid entry in liveOutRegMap!");
                                MUST_BE_TRUE(false, "RA verification error: Found live variable: " << dcl->getName()
                                    << " after lifetime_end " << " assigned to r" << regNum << "." << regOff << "!" << endl);
                            }
                        }
                    }

                    // verify EOT source
                    if (inst->isEOT() && kernel.fg.builder->hasEOTGRFBinding())
                    {
                        uint32_t regNum = var->getPhyReg()->asGreg()->getRegNum();
                        MUST_BE_TRUE(regNum >= 112,
                            "RA verification error: EOT source: " << dcl->getName()
                            << " is assigned to r." << regNum << endl);
                    }
                }
                else if (src->isSrcRegRegion() && src->getRegAccess() == IndirGRF)
                {
                    G4_SrcRegRegion* srcrgn = src->asSrcRegRegion();
                    G4_Declare* addrdcl = GetTopDclFromRegRegion(srcrgn);
                    G4_RegVar* ptvar = NULL;
                    int vid = 0;

                    while ((ptvar = pointsToAnalysis.getPointsTo(addrdcl->getRegVar(), vid++)) != NULL)
                    {
                        uint32_t varID = ptvar->getId();
                        G4_Declare* dcl = ptvar->getDeclare();
                        G4_RegVar* var = NULL;

                        while (dcl->getAliasDeclare())
                        {
                            dcl = dcl->getAliasDeclare();
                        }
                        var = dcl->getRegVar();

                        uint32_t regNum = var->getPhyReg()->asGreg()->getRegNum();
                        uint32_t regOff = var->getPhyRegOff();

                        uint32_t idx = regNum * G4_GRF_REG_SIZE +
                            (regOff * G4_Type_Table[dcl->getElemType()].byteSize) / G4_WSIZE;
                        for (uint32_t i = 0; i < dcl->getWordSize(); ++i, ++idx)
                        {
                            liveOutRegMapIt = liveOutRegMap.find(idx);
                            if (liveOutRegVec[idx] == UINT_MAX)
                            {
                                liveOutRegVec[idx] = varID;
                                MUST_BE_TRUE(liveOutRegMapIt == liveOutRegMap.end(), "RA verification error: Invalid entry in liveOutRegMap!");
                                liveOutRegMap.insert(make_pair(idx, dcl));
                            }
                            else
                            {
                                if (liveOutRegVec[idx] != varID)
                                {
                                    const BitSet& indr_use = liveAnalysis.indr_use[bb->getId()];

                                    if (dcl->isInput())
                                    {
                                        DEBUG_MSG("RA verification warning: Found conflicting input variables: " << dcl->getName()
                                            << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" << regNum
                                            << "." << regOff << "!" << endl);
                                        liveOutRegVec[idx] = varID;
                                        (*liveOutRegMapIt).second = dcl;
                                    }
                                    else if (indr_use.isSet(liveOutRegVec[idx]) == true)
                                    {
                                        MUST_BE_TRUE(false, "RA verification warning: Found conflicting indirect variables: " << dcl->getName()
                                            << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" <<
                                            regNum << "." << regOff << "!" << endl);
                                    }
                                    else
                                    {
                                        MUST_BE_TRUE(false, "RA verification error: Found conflicting variables: " << dcl->getName()
                                            << " and " << (*liveOutRegMapIt).second->getName() << " assigned to r" <<
                                            regNum << "." << regOff << "!" << endl);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}


int regAlloc(IR_Builder& builder, PhyRegPool& regPool, G4_Kernel& kernel)
{
	//
	// if no .reg_count_total, set 128 as the default value
	//
	if(kernel.getNumRegTotal() == UNDEFINED_VAL)
	{
        kernel.setNumRegTotal(builder.getOptions()->getuInt32Option(vISA_TotalGRFNum));
	}

	if (kernel.fg.getHasStackCalls() || kernel.fg.getIsStackCallFunc())
	{
	    if (builder.getOptions()->getuInt32Option(vISA_TotalGRFNum) < G4_DEFAULT_GRF_NUM)
	    {
            MUST_BE_TRUE(false, "total GRF number <128, cannot handle stack call!");
	    }
	}


    kernel.fg.reassignBlockIDs();
    //kernel.fg.markSimdBlocks();
    //kernel.fg.findBackEdges();

    if (kernel.getOptions()->getTarget() == VISA_3D)
    {

        kernel.fg.findNaturalLoops();

#ifdef DEBUG_VERBOSE_ON
        for (auto backedge : kernel.fg.backEdges)
        {
            DEBUG_VERBOSE("\n===> Found natural loop: ");
            for (auto block : kernel.fg.naturalLoops[backedge])
            {
                DEBUG_VERBOSE("\tBB" << block->getId());
            }
            DEBUG_VERBOSE(std::endl);
        }
#endif

    }

	if (builder.getOption(vISA_DumpDotAll))
    {
        kernel.dumpDotFile("PreRegAlloc");
    }

	kernel.fg.callerSaveAreaOffset = kernel.fg.calleeSaveAreaOffset = kernel.fg.paramOverflowAreaOffset =
		kernel.fg.paramOverflowAreaSize = kernel.fg.fileScopeSaveAreaSize = 0;

    //
    // Perform flow-insensitive points-to-analysis.
    //
    PointsToAnalysis pointsToAnalysis(kernel.Declares, kernel.fg.getNumBB());
    pointsToAnalysis.doPointsToAnalysis(kernel.fg);

    GlobalRA gra(kernel, regPool, pointsToAnalysis);

	if (kernel.fg.getHasStackCalls() || kernel.fg.getIsStackCallFunc())
	{
		// REVIEW
		// For now the following are disabled if the graph contains stack calls.
		unsigned fileScopeAreaSize;

		gra.setABIForStackCallFunctionCalls();

		//
		// Localize each filescope variable reference to each cut, i.e. have one representative
		// per cut, but each cut representative will be assigned the same frame offset in the
		// GENX_MAIN frame.
		//
		kernel.fg.addFrameSetupDeclares(builder, regPool);
		kernel.fg.doFilescopeVarLayout(builder, kernel.Declares, fileScopeAreaSize);
		kernel.fg.fileScopeSaveAreaSize = fileScopeAreaSize;

		kernel.fg.NormalizeFlowGraph();
	}

  //FIXME: here is a temp WA
  if (kernel.fg.subroutines.size() > 1 && 
      kernel.fg.builder->getOptions()->getTarget() == VISA_3D)
  {
    kernel.getOptions()->setOption(vISAOptions::vISA_LocalRA, false);
  }

	//
	// Mark block local variables for the whole graph prior to performing liveness analysis.
	//
	gra.markGraphBlockLocalVars();
    if(!(kernel.fg.builder->getOption(vISA_LocalRA)))
    {
         gra.removeUnreferencedDcls();
    }

	if (kernel.fg.builder->getOptions()->getTarget() == VISA_CM)
	{
		kernel.fg.markScope();
	}

	//
	// perform graph coloring for whole program
	//

	if(kernel.fg.getHasStackCalls() || kernel.fg.getIsStackCallFunc())
	{
		kernel.fg.addSaveRestorePseudoDeclares(builder);
	}

	int status = gra.coloringRegAlloc();

	if (status != CM_SUCCESS)
	{
		return status;
	}

    if (builder.getOption(vISA_VerifyRA))
    {
        LivenessAnalysis liveAnalysis(gra,
            G4_GRF | G4_INPUT,
            builder.getOption(vISA_IPA) && kernel.fg.performIPA(), true);
        liveAnalysis.computeLiveness(false);

        gra.verifyRA(liveAnalysis);
    }


	return status;
}
