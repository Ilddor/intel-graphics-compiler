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
#pragma once
#include "Compiler/CISACodeGen/DeSSA.hpp"
#include "Compiler/CISACodeGen/PatternMatchPass.hpp"
#include "Compiler/MetaDataUtilsWrapper.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/Pass.h>
#include "common/LLVMWarningsPop.hpp"

namespace IGC
{

class CodeGenContextWrapper;

class BlockCoalescing : public llvm::FunctionPass
{
public:
    static char ID;

    BlockCoalescing();
    
    virtual void getAnalysisUsage(llvm::AnalysisUsage& AU) const override {
        AU.setPreservesAll();
        AU.addRequired<DeSSA>();
        AU.addRequired<CodeGenPatternMatch>( );
        AU.addRequired<MetaDataUtilsWrapper>();
    }

    bool runOnFunction(llvm::Function& F) override;

    virtual llvm::StringRef getPassName() const override {
        return "BlockCoalescing";
    }
    bool IsEmptyBlock(llvm::BasicBlock* bb);
    /// look for the next none-empty basick block
    llvm::BasicBlock* SkipEmptyBasicBlock(llvm::BasicBlock* bb);
    /// look for the destination to jump to if the block is empty
    llvm::BasicBlock* FollowEmptyBlock(llvm::BasicBlock* bb);
protected:
    llvm::DenseSet<llvm::BasicBlock*> m_emptyBlocks;

private:
    bool hasEmptyBlockLoop(llvm::BasicBlock *EmptyBlock);
};
}