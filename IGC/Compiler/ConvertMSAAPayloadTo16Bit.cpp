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
#include "GenISAIntrinsics/GenIntrinsicInst.h"
#include "ConvertMSAAPayloadTo16Bit.hpp"
#include "Compiler/IGCPassSupport.h"
#include "common/LLVMUtils.h"
#include "Compiler/CodeGenPublic.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/Support/CommandLine.h>
#include <llvm/IR/Function.h>
#include <llvm/ADT/SmallVector.h>
#include "common/LLVMWarningsPop.hpp"

using namespace llvm;
using namespace IGC;

// Register pass to igc-opt
#define PASS_FLAG "igc-ConverMSAAPayloadTo16Bit"
#define PASS_DESCRIPTION "Comvert normal MSAA intrinsics to 16 bit payload intrinsics"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS true
IGC_INITIALIZE_PASS_BEGIN(ConvertMSAAPayloadTo16Bit, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(CodeGenContextWrapper)
IGC_INITIALIZE_PASS_END(ConvertMSAAPayloadTo16Bit, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char ConvertMSAAPayloadTo16Bit::ID = 0;

ConvertMSAAPayloadTo16Bit::ConvertMSAAPayloadTo16Bit() : FunctionPass(ID)
{
    initializeWorkaroundAnalysisPass(*PassRegistry::getPassRegistry());
};

void ConvertMSAAPayloadTo16Bit::visitCallInst(CallInst &I)
{
    if (const GenIntrinsicInst* intr = dyn_cast<GenIntrinsicInst>(&I))
    {
        GenISAIntrinsic::ID intrID = intr->getIntrinsicID();
        switch (intrID)
        {
            case GenISAIntrinsic::GenISA_ldmsptr:
            {
                const GenIntrinsicInst* ldmcs = nullptr;
                const BitCastInst* bitcastInstr = dyn_cast<BitCastInst>(I.getOperand(2));
                CallInst* ldmcsCallInst;
                if(bitcastInstr)
                {
                    const ExtractElementInst* extractInst = dyn_cast<ExtractElementInst>(bitcastInstr->getOperand(0));
                    if (extractInst)
                    {
                        ldmcsCallInst = dyn_cast<CallInst>(extractInst->getOperand(0));
                        if (ldmcsCallInst)
                        {
                            ldmcs = dyn_cast<GenIntrinsicInst>(ldmcsCallInst);
                        }
                    }
                }

                assert(ldmcs!=NULL);

                Type* types_ldmcs[] = { 
                    VectorType::get(llvm::Type::getHalfTy(m_pModule->getContext()), 4), 
                    ldmcs->getOperand(4)->getType() };

                Function *func_ldmcs =
                    GenISAIntrinsic::getDeclaration(
                    I.getParent()->getParent()->getParent(),
                    GenISAIntrinsic::GenISA_ldmcsptr,
                    llvm::ArrayRef<Type*>(types_ldmcs, 2));

               Value * packed_tex_params_ldmcs[] = {
                    ldmcs->getOperand(0),
                    ldmcs->getOperand(1),
                    ldmcs->getOperand(2),
                    ldmcs->getOperand(3),
                    ldmcs->getOperand(4),
                    ldmcs->getOperand(5),
                    ldmcs->getOperand(6),
                    ldmcs->getOperand(7)
                };

               m_builder->SetInsertPoint(ldmcsCallInst);
               llvm::CallInst* new_mcs_call = m_builder->CreateCall(func_ldmcs, packed_tex_params_ldmcs);

               llvm::Value* float_mcs_ch0 = m_builder->CreateExtractElement(new_mcs_call, m_builder->getInt32(0));
               llvm::Value* mcs0 = m_builder->CreateBitCast(float_mcs_ch0, m_builder->getInt16Ty());
               llvm::Value* float_mcs_ch1 = m_builder->CreateExtractElement(new_mcs_call, m_builder->getInt32(1));
               llvm::Value* mcs1 = m_builder->CreateBitCast(float_mcs_ch1, m_builder->getInt16Ty());
               llvm::Value* float_mcs_ch2 = m_builder->CreateExtractElement(new_mcs_call, m_builder->getInt32(2));
               llvm::Value* mcs2 = m_builder->CreateBitCast(float_mcs_ch2, m_builder->getInt16Ty());
               llvm::Value* float_mcs_ch3 = m_builder->CreateExtractElement(new_mcs_call, m_builder->getInt32(3));
               llvm::Value* mcs3 = m_builder->CreateBitCast(float_mcs_ch3, m_builder->getInt16Ty());

               Type* types_ldms[] = { I.getType(), I.getOperand(7)->getType() };
               Function *func_ldms = GenISAIntrinsic::getDeclaration(
                   I.getParent()->getParent()->getParent(),
                   GenISAIntrinsic::GenISA_ldmsptr16bit,
                   ArrayRef<Type*>(types_ldms, 2));

               m_builder->SetInsertPoint(&I);
               llvm::Value * packed_tex_params_ldms[] = {
                    m_builder->CreateTrunc(I.getOperand(0), m_builder->getInt16Ty(), ""),
                    mcs0,
                    mcs1,
                    mcs2,
                    mcs3,
                    m_builder->CreateTrunc(I.getOperand(3), m_builder->getInt16Ty(), ""),
                    m_builder->CreateTrunc(I.getOperand(4), m_builder->getInt16Ty(), ""),
                    m_builder->CreateTrunc(I.getOperand(5), m_builder->getInt16Ty(), ""),
                    m_builder->CreateTrunc(I.getOperand(6), m_builder->getInt16Ty(), ""),
                    I.getOperand(7),
                    I.getOperand(8),
                    I.getOperand(9),
                    I.getOperand(10)
                };

                llvm::CallInst* new_ldms = m_builder->CreateCall(func_ldms, packed_tex_params_ldms);
                (&I)->replaceAllUsesWith(new_ldms);
                break;
            }         

            default:
                break;
        }
    }
}

bool ConvertMSAAPayloadTo16Bit::runOnFunction(Function &F)
{
    m_pCtxWrapper = &getAnalysis<CodeGenContextWrapper>();
    CodeGenContext* cgCtx = m_pCtxWrapper->getCodeGenContext();
    IRBuilder<> builder(F.getContext());
    m_builder = &builder;
    m_pModule = F.getParent();
    visit(F);
    DumpLLVMIR(cgCtx, "AfterMSAA16bitPayload");
    return true;
}