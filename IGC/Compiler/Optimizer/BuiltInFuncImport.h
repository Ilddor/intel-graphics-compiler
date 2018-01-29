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

#include "common/LLVMWarningsPush.hpp"
#include <llvm/Pass.h>
#include "common/LLVMWarningsPop.hpp"

#include <vector>
#include <set>
#include <queue>

namespace IGC
{
    /// This pass imports built-in functions from source module to destination module.
    class BIImport : public llvm::ModulePass
    {
    protected:
        // Type used to hold a vector of Functions and augment it during traversal.
        typedef std::vector<llvm::Function*>       TFunctionsVec;

    public:
        // Pass identification, replacement for typeid.
        static char ID;

        /// @brief Constructor
        BIImport(std::unique_ptr<llvm::Module> pGenericModule = nullptr,
            std::unique_ptr<llvm::Module> pSizeModule = nullptr);

        /// @brief analyses used
        virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const override
        {
            AU.addRequired<MetaDataUtilsWrapper>();
            AU.addRequired<CodeGenContextWrapper>();
        }

        /// @brief Provides name of pass
        virtual llvm::StringRef getPassName() const override
        {
            return "BIImport";
        }

        /// @brief Main entry point.
        ///        Find all builtins to import, and import them along with callees and globals.
        /// @param M The destination module.
        bool runOnModule(llvm::Module &M) override;

    protected:
        /// @brief Get all the functions called by given function.
        /// @param [IN] pFunc The given function.
        /// @param [OUT] calledFuncs The list of all functions called by pFunc.
        static void GetCalledFunctions(const llvm::Function* pFunc, TFunctionsVec& calledFuncs);

        /// @brief  Remove function bitcasts that sometimes may appear due to the changed in the way
        ///         the BiFs are linked. We can remove this code once llvm implements typeless pointers.
        void removeFunctionBitcasts(llvm::Module &M);

        /// @brief  Initialize values for global flags needed for the built-ins (FlushDenormal).
        ///         Only initializes flags that the built-ins need.
        void InitializeBIFlags(llvm::Module &M);

        /// @brief  Search through all builtin modules for the specified function.
        /// @param  funcName - name of func to search for.
        llvm::Function *GetBuiltinFunction(llvm::StringRef funcName) const;

    protected:
        /// Builtin module - contains the source function definition to import
        std::unique_ptr<llvm::Module> m_GenericModule;
        std::unique_ptr<llvm::Module> m_SizeModule;
    };

} // namespace IGC

extern "C" llvm::ModulePass *createBuiltInImportPass(
    std::unique_ptr<llvm::Module> pGenericModule, std::unique_ptr<llvm::Module> pSizeModule);

namespace IGC
{
    class PreBIImportAnalysis : public llvm::ModulePass
    {
    public:
        // Pass identification, replacement for typeid
        static char ID;

        /// @brief  Constructor
        PreBIImportAnalysis();

        /// @brief  Destructor
        ~PreBIImportAnalysis() {}

        /// @brief  Provides name of pass
        virtual llvm::StringRef getPassName() const override
        {
            return "PreBIImportAnalysis";
        }

        void getAnalysisUsage(llvm::AnalysisUsage &AU) const override
        {
            AU.addRequired<MetaDataUtilsWrapper>();
        }

        /// @brief  Main entry point.
        /// @param  M The destination module.
        virtual bool runOnModule(llvm::Module &M) override;

        static const llvm::StringRef OCL_GET_GLOBAL_OFFSET;
        static const llvm::StringRef OCL_GET_LOCAL_ID;
        static const llvm::StringRef OCL_GET_GROUP_ID;
    };

} // namespace IGC