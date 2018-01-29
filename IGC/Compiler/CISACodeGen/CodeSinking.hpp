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
/**
 * Originated from llvm code-sinking, need add their copyright
 **/
//===-- Sink.cpp - Code Sinking -------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#pragma once
#include "common/LLVMWarningsPush.hpp"
#include <llvm/Analysis/PostDominators.h>
#include "common/LLVMWarningsPop.hpp"

namespace IGC {

#define CODE_SINKING_MIN_SIZE  32

class CodeSinking : public llvm::FunctionPass {
    llvm::DominatorTree *DT;
    llvm::PostDominatorTree *PDT;
    llvm::LoopInfo *LI;
    const llvm::DataLayout *DL;  // to estimate register pressure
public:
    static char ID; // Pass identification

    CodeSinking(bool generalSinking = false, unsigned pressureThreshold = 0);
    
    virtual bool runOnFunction(llvm::Function &F) override;

    virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
        AU.setPreservesCFG();
        AU.addRequired<llvm::DominatorTreeWrapperPass>();
        AU.addRequired<llvm::PostDominatorTreeWrapperPass>();
        AU.addRequired<llvm::LoopInfoWrapperPass>();
        AU.addRequired<CodeGenContextWrapper>();
        AU.addPreserved<llvm::DominatorTreeWrapperPass>();
        AU.addPreserved<llvm::PostDominatorTreeWrapperPass>();
        AU.addPreserved<llvm::LoopInfoWrapperPass>();
    }
private:
    bool ProcessBlock(llvm::BasicBlock &blk);
    bool SinkInstruction(llvm::Instruction *I,
        llvm::SmallPtrSetImpl<llvm::Instruction*> &Stores);
    bool AllUsesDominatedByBlock(llvm::Instruction *inst,
        llvm::BasicBlock *blk,
        llvm::SmallPtrSetImpl<llvm::Instruction*> &usesInBlk) const;
    bool FindLowestSinkTarget(llvm::Instruction *inst,
        llvm::BasicBlock* &blk,
        llvm::SmallPtrSetImpl<llvm::Instruction*> &usesInBlk, bool &outerLoop);
    bool isSafeToMove(llvm::Instruction *inst,
        bool &reducePressure, bool &hasAliasConcern,
        llvm::SmallPtrSetImpl<llvm::Instruction*> &Stores);

    /// local processing
    bool LocalSink(llvm::BasicBlock *blk);
    /// data members for local-sinking
    llvm::SmallPtrSet<llvm::BasicBlock*, 8> localBlkSet;
    llvm::SmallPtrSet<llvm::Instruction*, 8> localInstSet;
    /// data members for undo
    std::vector<llvm::Instruction*> movedInsts;
    std::vector<llvm::Instruction*> undoLocas;
    /// counting the number of gradient/sample operation sinked into CF
    unsigned totalGradientMoved;
    unsigned numGradientMovedOutBB;

    bool generalCodeSinking;
    unsigned registerPressureThreshold;
    // diagnosis variable: int numChanges;
};

}

