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

#include "Compiler/CodeGenContextWrapper.hpp"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/Pass.h>
#include <llvm/IR/InstVisitor.h>
#include "common/LLVMWarningsPop.hpp"

namespace IGC
{
    /// @brief  This pass converts store/load on doubles into store/loads on i32 or float types.
    class HandleLoadStoreInstructions : public llvm::FunctionPass, public llvm::InstVisitor<HandleLoadStoreInstructions, void>
    {
    public:
        // Pass identification, replacement for typeid
        static char ID;

        /// @brief  Constructor
        HandleLoadStoreInstructions();

        /// @brief  Destructor
        ~HandleLoadStoreInstructions() {}

        /// @brief  Provides name of pass
        virtual llvm::StringRef getPassName() const override
        {
            return "HandleLoadStoreInstructionsPass";
        }

        /// @brief  Main entry point.
        /// @param  F The destination function.
        virtual bool runOnFunction(llvm::Function &F) override;
        void visitLoadInst(llvm::LoadInst &I);
        void visitStoreInst(llvm::StoreInst &I);

        virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const override
        {
        }

    protected:
        void HandleLoadStore();

        /// @brief  Indicates if the pass changed the processed function
        bool m_changed;
    };

} // namespace IGC