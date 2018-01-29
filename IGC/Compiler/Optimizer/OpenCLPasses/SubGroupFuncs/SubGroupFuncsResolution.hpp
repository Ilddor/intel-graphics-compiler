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
#include <llvm/IR/DataLayout.h>
#include "Compiler/Optimizer/OCLBIUtils.h"
#include "common/LLVMWarningsPush.hpp"
#include <llvm/Pass.h>
#include <llvm/IR/InstVisitor.h>
#include "common/LLVMWarningsPop.hpp"
#include "IGC/common/Types.hpp"

namespace IGC
{

    /// @brief  SubGroupFuncsResolution pass used for resolving OpenCL Sub Group functions.
    class SubGroupFuncsResolution : public llvm::FunctionPass, public llvm::InstVisitor<SubGroupFuncsResolution, void>
    {
    public:
        // Pass identification, replacement for typeid
        static char ID;

        /// @brief  Constructor
        SubGroupFuncsResolution();

        /// @brief  Destructor
        ~SubGroupFuncsResolution() {}

        /// @brief  Provides name of pass
        virtual llvm::StringRef getPassName() const override
        {
            return "SubGroupFuncsResolution";
        }

        void getAnalysisUsage(llvm::AnalysisUsage &AU) const override
        {
            AU.addRequired<CodeGenContextWrapper>();
            AU.addRequired<MetaDataUtilsWrapper>();
        }

        /// @brief  Main entry point.
        ///         Finds all OpenCL Sub Group function calls and resolve them into an llvm sequence
        /// @param  F The destination function.
        virtual bool runOnFunction(llvm::Function &F) override;

        /// @brief  Call instructions visitor.
        ///         Checks for OpenCL Sub Group  functions and resolves them into appropriate sequence of code
        /// @param  CI The call instruction.
        void visitCallInst(llvm::CallInst &CI);

        void BTIHelper(llvm::CallInst &CI);

        void mediaBlockRead(llvm::CallInst &CI);
        void mediaBlockWrite(llvm::CallInst &CI);

        void simdBlockReadGlobal(llvm::CallInst &CI);
        void simdBlockWriteGlobal(llvm::CallInst &CI);

        void pushMediaBlockArgs(llvm::SmallVector<llvm::Value*, 5> &args, llvm::CallInst &CI );

        void CheckMediaBlockInstError(llvm::GenIntrinsicInst* inst, bool isRead);

        void subGroupReduce(WaveOps op, llvm::CallInst &CI);
        void subGroupScan(WaveOps op, llvm::CallInst &CI);

        static const llvm::StringRef GET_MAX_SUB_GROUP_SIZE;
        static const llvm::StringRef GET_SUB_GROUP_LOCAL_ID;
        static const llvm::StringRef SUB_GROUP_SHUFFLE;
        static const llvm::StringRef SUB_GROUP_SHUFFLE_F;
        static const llvm::StringRef SUB_GROUP_SHUFFLE_H;
        static const llvm::StringRef SUB_GROUP_SHUFFLE_DOWN;
        static const llvm::StringRef SUB_GROUP_SHUFFLE_DOWN_US;
        static const llvm::StringRef SIMD_BLOCK_READ_1_GBL;
        static const llvm::StringRef SIMD_BLOCK_READ_2_GBL;
        static const llvm::StringRef SIMD_BLOCK_READ_4_GBL;
        static const llvm::StringRef SIMD_BLOCK_READ_8_GBL;
        static const llvm::StringRef SIMD_BLOCK_READ_1_GBL_H;
        static const llvm::StringRef SIMD_BLOCK_READ_2_GBL_H;
        static const llvm::StringRef SIMD_BLOCK_READ_4_GBL_H;
        static const llvm::StringRef SIMD_BLOCK_READ_8_GBL_H;
        static const llvm::StringRef SIMD_BLOCK_READ_16_GBL_H;
        static const llvm::StringRef SIMD_BLOCK_WRITE_1_GBL;
        static const llvm::StringRef SIMD_BLOCK_WRITE_2_GBL;
        static const llvm::StringRef SIMD_BLOCK_WRITE_4_GBL;
        static const llvm::StringRef SIMD_BLOCK_WRITE_8_GBL;
        static const llvm::StringRef SIMD_BLOCK_WRITE_1_GBL_H;
        static const llvm::StringRef SIMD_BLOCK_WRITE_2_GBL_H;
        static const llvm::StringRef SIMD_BLOCK_WRITE_4_GBL_H;
        static const llvm::StringRef SIMD_BLOCK_WRITE_8_GBL_H;
        static const llvm::StringRef SIMD_BLOCK_WRITE_16_GBL_H;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_READ_1;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_READ_2;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_READ_4;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_READ_8;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_READ_1_H;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_READ_2_H;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_READ_4_H;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_READ_8_H;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_READ_16_H;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_WRITE_1;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_WRITE_2;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_WRITE_4;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_WRITE_8;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_WRITE_1_H;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_WRITE_2_H;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_WRITE_4_H;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_WRITE_8_H;
        static const llvm::StringRef SIMD_MEDIA_BLOCK_WRITE_16_H;

        static const llvm::StringRef MEDIA_BLOCK_READ;
        static const llvm::StringRef MEDIA_BLOCK_WRITE;

        static const llvm::StringRef MEDIA_BLOCK_RECTANGLE_READ;

        static const llvm::StringRef GET_IMAGE_BTI;

        static const llvm::StringRef SUB_GROUP_REDUCE_ADD;
        static const llvm::StringRef SUB_GROUP_REDUCE_IMIN;
        static const llvm::StringRef SUB_GROUP_REDUCE_UMIN;
        static const llvm::StringRef SUB_GROUP_REDUCE_IMAX;
        static const llvm::StringRef SUB_GROUP_REDUCE_UMAX;
        static const llvm::StringRef SUB_GROUP_REDUCE_FADD;
        static const llvm::StringRef SUB_GROUP_REDUCE_FMAX;
        static const llvm::StringRef SUB_GROUP_REDUCE_FMIN;

        static const llvm::StringRef SUB_GROUP_SCAN_ADD;
        static const llvm::StringRef SUB_GROUP_SCAN_IMIN;
        static const llvm::StringRef SUB_GROUP_SCAN_UMIN;
        static const llvm::StringRef SUB_GROUP_SCAN_IMAX;
        static const llvm::StringRef SUB_GROUP_SCAN_UMAX;
        static const llvm::StringRef SUB_GROUP_SCAN_FADD;
        static const llvm::StringRef SUB_GROUP_SCAN_FMAX;
        static const llvm::StringRef SUB_GROUP_SCAN_FMIN;


    private:
        /// @brief  Container for instructions to be deleted after visiting a function.
        llvm::SmallVector<llvm::Instruction*, 16>  m_instsToDelete;
        
        /// @brief - maps image and sampler kernel parameters to BTIs
        CImagesBI::ParamMap m_argIndexMap;

        /// @brief  Indicates if the pass changed the processed function
        bool m_changed;

        /// @brief examine metadata for intel_reqd_sub_group_size
        int32_t GetSIMDSize(llvm::Function *F);

        /// @brief emits the given error message in SIMD32.  Used on subgroup functions
        // that aren't currently supported in SIMD32.
        void CheckSIMDSize(llvm::Instruction &I, llvm::StringRef msg);
    };

} // namespace IGC

