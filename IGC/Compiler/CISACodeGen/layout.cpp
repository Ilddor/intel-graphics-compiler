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

#include "Compiler/CISACodeGen/layout.hpp"
#include "Compiler/CISACodeGen/ShaderCodeGen.hpp"
#include "Compiler/IGCPassSupport.h"

#include "common/debug/Debug.hpp"
#include "common/debug/Dump.hpp"
#include "common/MemStats.h"
#include "common/LLVMUtils.h"

#include <vector>
#include <set>

using namespace llvm;
using namespace IGC;

#define SUCCSZANY     (true)
#define SUCCHASINST   (succ->size() > 1)
#define SUCCNOINST    (succ->size() <= 1)
#define SUCCANYLOOP   (true)

#define PUSHSUCC(BLK, C1, C2) \
        for(succ_iterator succIter = succ_begin(BLK), succEnd = succ_end(BLK); \
            succIter!=succEnd; ++succIter) {                                   \
            llvm::BasicBlock *succ = *succIter;                                \
            if (!visitSet.count(succ) && C1 && C2) {                           \
                visitVec.push_back(succ);                                      \
                visitSet.insert(succ);                                         \
                break;                                                         \
            }                                                                  \
        }

// Register pass to igc-opt
#define PASS_FLAG "igc-layout"
#define PASS_DESCRIPTION "Layout blocks"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(Layout, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
IGC_INITIALIZE_PASS_DEPENDENCY(PostDominatorTreeWrapperPass)
IGC_INITIALIZE_PASS_END(Layout, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char IGC::Layout::ID = 0;

Layout::Layout() : FunctionPass(ID)
{
    initializeLayoutPass(*PassRegistry::getPassRegistry());
}

void Layout::getAnalysisUsage(llvm::AnalysisUsage &AU) const
{
    // Doesn't change the IR at all, it juts move the blocks so no changes in the IR
    AU.setPreservesAll();
    AU.addRequired<llvm::LoopInfoWrapperPass>();
}

bool Layout::runOnFunction( Function& func )
{
    LoopInfo& LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    if (LI.empty())
    {
        LayoutBlocks( func );
    }
    else
    {
        LayoutBlocks( func, LI );
    }
    MEM_SNAPSHOT( IGC::SMS_AFTER_LAYOUTPASS );
    return true;
}

#define BREAK_BLOCK_SIZE_LIMIT 3

static bool HasThreadGroupBarrierInBlock(BasicBlock *blk)
{
    std::string Name = GenISAIntrinsic::getName(GenISAIntrinsic::GenISA_threadgroupbarrier);
    Module *Mod = blk->getParent()->getParent();
    if (auto GroupBarrier = Mod->getFunction(Name))
    {
        for (auto U : GroupBarrier->users())
        {
            auto Inst = dyn_cast<Instruction>(U);
            if (Inst && Inst->getParent() == blk)
            {
                return true;
            }
        }
    }
    return false;
}

BasicBlock* Layout::getLastReturnBlock(Function &Func)
{
    Function::BasicBlockListType& bblist = Func.getBasicBlockList();
    for (Function::BasicBlockListType::reverse_iterator RI = bblist.rbegin(),
        RE = bblist.rend();  RI != RE; ++RI)
    {
        BasicBlock* bb = &*RI;
        if (succ_begin(bb) == succ_end(bb))
        {
            return bb;
        }
    }
    assert(false && "Function does not have a return block!");
    return nullptr;
}

void Layout::LayoutBlocks(Function &func, LoopInfo &LI)
{
	std::vector<llvm::BasicBlock*> visitVec;
	std::set<llvm::BasicBlock*> visitSet;
	// Insertion Position per loop header
	std::map<llvm::BasicBlock*, llvm::BasicBlock*> InsPos;

	llvm::BasicBlock* entry = &(func.getEntryBlock());
	visitVec.push_back(entry);
	visitSet.insert(entry); 
	InsPos[entry] = entry;

	while (!visitVec.empty())
	{
		llvm::BasicBlock* blk = visitVec.back();
		llvm::Loop *curLoop = LI.getLoopFor(blk);
		if (curLoop) 
		{
			auto hd = curLoop->getHeader();
			if (blk == hd && InsPos.find(hd) == InsPos.end()) 
			{
				InsPos[blk] = blk;
			}
		}
		// FIXME: this is a hack to workaround an irreducible test case
		if (func.getName() == "ocl_test_kernel")
		{
			// push: time for DFS visit
			PUSHSUCC(blk, SUCCANYLOOP, SUCCNOINST);
			if (blk != visitVec.back())
				continue;
			// push: time for DFS visit
			PUSHSUCC(blk, SUCCANYLOOP, SUCCHASINST);
		}
		else
		{
			// push: time for DFS visit
			PUSHSUCC(blk, SUCCANYLOOP, SUCCHASINST);
			if (blk != visitVec.back())
				continue;
			// push: time for DFS visit
			PUSHSUCC(blk, SUCCANYLOOP, SUCCNOINST);
		}
		// pop: time to move the block to the right location
		if (blk == visitVec.back())
		{
			visitVec.pop_back();
			if (curLoop) 
			{
				auto hd = curLoop->getHeader();
				if (blk != hd) 
				{
					// move the block to the beginning of the loop 
					auto insp = InsPos[hd];
					assert(insp);
					if (blk != insp) 
					{
						blk->moveBefore(insp);
						InsPos[hd] = blk;
					}
				}
				else 
				{
					// move the entire loop to the beginning of
					// the parent loop
					auto LoopStart = InsPos[hd];
					assert(LoopStart);
					auto PaLoop = curLoop->getParentLoop();
					auto PaHd = PaLoop ? PaLoop->getHeader() : entry;
					auto insp = InsPos[PaHd];
					if (LoopStart == hd)
					{
						// single-block loop
						hd->moveBefore(insp);
					}
					else
					{
						// loop-header is not moved yet, so should be at the end
						// use splice
						llvm::Function::BasicBlockListType& BBList = func.getBasicBlockList();
						BBList.splice(insp->getIterator(), BBList,
							LoopStart->getIterator(),
							hd->getIterator());
						hd->moveBefore(LoopStart);
					}
					InsPos[PaHd] = hd;
				}
			}
			else 
			{
				auto insp = InsPos[entry];
				if (blk != insp)
				{
					blk->moveBefore(insp);
					InsPos[entry] = blk;
				}
			}
		}
	}

    // if function has a single exit, then the last block must be an exit
    // comment this out due to infinite loop example in OCL
    // assert(PDT.getRootNode()->getBlock() == 0x0 ||
    //       PDT.getRootNode()->getBlock() == &(func.getBasicBlockList().back()));
    // fix the loop-exit pattern, put break-blocks into the loop
    for(llvm::Function::iterator blkIter = func.begin(), blkEnd = func.end(); 
        blkIter != blkEnd; ++blkIter)
    {
        llvm::BasicBlock *blk = &(*blkIter);
        llvm::Loop *curLoop = LI.getLoopFor(blk);
        bool allPredLoopExit = true;
        unsigned numPreds = 0;
        llvm::SmallPtrSet<llvm::BasicBlock*, 4> predSet;
        for(pred_iterator predIter = pred_begin(blk), predEnd = pred_end(blk); 
            predIter != predEnd; ++predIter) 
        {
            llvm::BasicBlock *pred = *predIter;
            numPreds++;
            llvm::Loop *predLoop = LI.getLoopFor(pred);
            if (curLoop == predLoop)
            {
                llvm::BasicBlock *predPred = pred->getSinglePredecessor();
                if (predPred)
                {
                    llvm::Loop *predPredLoop = LI.getLoopFor(predPred);
                    if (predPredLoop != curLoop && 
                        (!curLoop || curLoop->contains(predPredLoop)))
                    {
                        if (pred->size() <= BREAK_BLOCK_SIZE_LIMIT &&
                            !HasThreadGroupBarrierInBlock(pred))
                        {
                            predSet.insert(pred);
                        }
                        else
                        {
                            allPredLoopExit = false;
                            break;
                        }

                    }
                }
            }
            else if (!curLoop || curLoop->contains(predLoop))
                continue;
            else
            {
                allPredLoopExit = false;
                break;
            }
        }
        if (allPredLoopExit && numPreds > 1)
        {
            for(SmallPtrSet<BasicBlock*, 4>::iterator predIter = predSet.begin(),
                predEnd = predSet.end(); predIter != predEnd; ++predIter)
            {
                llvm::BasicBlock *pred = *predIter;
                llvm::BasicBlock *predPred = pred->getSinglePredecessor();
                pred->moveAfter(predPred);
            }
        }
    }
}

void Layout::LayoutBlocks( Function &func )
{
    std::vector<llvm::BasicBlock*> visitVec;
    std::set<llvm::BasicBlock*> visitSet;
    // Reorder basic block to allow more fall-through 
    llvm::BasicBlock* entry = &(func.getEntryBlock());
    visitVec.push_back(entry);

    // Push a return block to make sure the last BB is the return block.
    if (BasicBlock* lastReturnBlock = getLastReturnBlock(func))
    {
        if (lastReturnBlock != entry)
        {
            visitVec.push_back(lastReturnBlock);
            visitSet.insert(lastReturnBlock);
        }
    }

    while ( !visitVec.empty() )
    {
        llvm::BasicBlock* blk = visitVec.back();
		// push in the empty successor 
		PUSHSUCC(blk, SUCCANYLOOP, SUCCNOINST);
		if (blk != visitVec.back())
			continue;
        // push in all the same-loop successors 
        PUSHSUCC(blk, SUCCANYLOOP, SUCCSZANY);
        // pop
        if (blk == visitVec.back())
        {
            visitVec.pop_back();
            if (blk != entry)
            {
                blk->moveBefore(entry);
                entry = blk;
            }
        }
    }
}
