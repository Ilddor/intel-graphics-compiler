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

#include "Compiler/MetaDataUtilsWrapper.h"
#include "Compiler/CodeGenContextWrapper.hpp"
#include "GenISAIntrinsics/GenIntrinsicInst.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/Pass.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/ADT/SmallSet.h>
#include "common/LLVMWarningsPop.hpp"
#include "common/IGCIRBuilder.h"

namespace IGC
{
class WorkaroundAnalysis : public llvm::FunctionPass, public llvm::InstVisitor<WorkaroundAnalysis>
{
    llvm::IGCIRBuilder<> *m_builder;
    bool m_enableFMaxFMinPlusZero;
public:
    static char ID;

    WorkaroundAnalysis(bool enableFMaxFMinPlusZero = false);

    ~WorkaroundAnalysis() {}

    virtual bool runOnFunction(llvm::Function &F) override;

    virtual llvm::StringRef getPassName() const override
    {
        return "WorkaroundAnalysis Pass";
    }

    virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const override
    {
        AU.addRequired<llvm::PostDominatorTreeWrapperPass>();
        AU.addRequired<MetaDataUtilsWrapper>();
        AU.addRequired<CodeGenContextWrapper>();
    }

    void visitCallInst(llvm::CallInst &I);

private:
    void generateDummyLoad(llvm::DomTreeNode* pPDTRoot);
    void GatherOffsetWorkaround(llvm::SamplerGatherIntrinsic* gatherpo);

    const llvm::DataLayout   *m_pDataLayout;
    llvm::Module* m_pModule;
    CodeGenContextWrapper*    m_pCtxWrapper;
};

} // namespace IGC
