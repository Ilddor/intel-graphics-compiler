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


//===----------------------------------------------------------------------===//
//
// The purpose of this pass is replace instructions using halfs with
// corresponding float counterparts.
//
// All unnecessary conversions get cleaned up before code gen.
// 
//===----------------------------------------------------------------------===//


#include "HalfPromotion.h"
#include "Compiler/IGCPassSupport.h"
#include "GenISAIntrinsics/GenIntrinsics.h"
#include "IGCIRBuilder.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Function.h>
#include "common/LLVMWarningsPop.hpp"


using namespace llvm;
using namespace IGC;

#define PASS_FLAG "half-promotion"
#define PASS_DESCRIPTION "Promotion of halfs to floats"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(HalfPromotion, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_END(HalfPromotion, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char HalfPromotion::ID = 0;

HalfPromotion::HalfPromotion() : FunctionPass(ID)
{
    initializeHalfPromotionPass(*PassRegistry::getPassRegistry());
}

bool HalfPromotion::runOnFunction(Function &F) 
{
    visit(F);
    return m_changed;
}

void HalfPromotion::visitCallInst(llvm::CallInst &I)
{
    if(llvm::isa<llvm::IntrinsicInst>(I) && I.getType()->isHalfTy())
    {
        handleLLVMIntrinsic(llvm::cast<IntrinsicInst>(I));
    }
    else if(llvm::isa<GenIntrinsicInst>(I) && I.getType()->isHalfTy())
    {
        handleGenIntrinsic(llvm::cast<GenIntrinsicInst>(I));
    }
}

void IGC::HalfPromotion::handleLLVMIntrinsic(llvm::IntrinsicInst &I)
{
    Intrinsic::ID id = I.getIntrinsicID();
    if(id == Intrinsic::cos ||
        id == Intrinsic::sin ||
        id == Intrinsic::log2 ||
        id == Intrinsic::exp2 ||
        id == Intrinsic::sqrt ||
        id == Intrinsic::floor ||
        id == Intrinsic::ceil ||
        id == Intrinsic::fabs ||
        id == Intrinsic::pow ||
        id == Intrinsic::fma)
    {
        Module* M = I.getParent()->getParent()->getParent();
        llvm::IGCIRBuilder<> builder(&I);
        std::vector<llvm::Value*> arguments;

        Function* pNewFunc = Intrinsic::getDeclaration(
            M,
            I.getIntrinsicID(),
            builder.getFloatTy());

        for(unsigned i = 0; i < I.getNumArgOperands(); ++i)
        {
            Value* op = builder.CreateFPExt(I.getOperand(i), builder.getFloatTy());
            arguments.push_back(op);
        }

        Value* f32Val = builder.CreateCall(
            pNewFunc,
            arguments);
        Value* f16Val = builder.CreateFPTrunc(f32Val, builder.getHalfTy());
        I.replaceAllUsesWith(f16Val);
        m_changed = true;
    }
}

void IGC::HalfPromotion::handleGenIntrinsic(llvm::GenIntrinsicInst &I)
{
    GenISAIntrinsic::ID id = I.getIntrinsicID();
    if(id == GenISAIntrinsic::GenISA_fsat ||
        id == GenISAIntrinsic::GenISA_sqrt ||
        id == GenISAIntrinsic::GenISA_rsq ||
        id == GenISAIntrinsic::GenISA_GradientX ||
        id == GenISAIntrinsic::GenISA_GradientY ||
        id == GenISAIntrinsic::GenISA_min ||
        id == GenISAIntrinsic::GenISA_max ||
        id == GenISAIntrinsic::GenISA_WaveShuffleIndex)
    {
        Module* M = I.getParent()->getParent()->getParent();
        llvm::IGCIRBuilder<> builder(&I);
        std::vector<llvm::Value*> arguments;

        Function* pNewFunc = GenISAIntrinsic::getDeclaration(
            M,
            I.getIntrinsicID(),
            builder.getFloatTy());

        for(unsigned i = 0; i < I.getNumArgOperands(); ++i)
        {
            if(I.getOperand(i)->getType()->isHalfTy())
            {
                Value* op = builder.CreateFPExt(I.getOperand(i), builder.getFloatTy());
                arguments.push_back(op);
            }
            else
            {
                arguments.push_back(I.getOperand(i));
            }
        }

        Value* f32Val = builder.CreateCall(
            pNewFunc,
            arguments);
        Value* f16Val = builder.CreateFPTrunc(f32Val, builder.getHalfTy());
        I.replaceAllUsesWith(f16Val);
        m_changed = true;
    }
}

void HalfPromotion::visitFCmp(llvm::FCmpInst &CmpI)
{
    if(CmpI.getOperand(0)->getType()->isHalfTy())
    {
        llvm::IGCIRBuilder<> builder(&CmpI);
        Value* op1 = builder.CreateFPExt(CmpI.getOperand(0), builder.getFloatTy());
        Value* op2 = builder.CreateFPExt(CmpI.getOperand(1), builder.getFloatTy());
        Value* newOp = builder.CreateFCmp(CmpI.getPredicate(), op1, op2);
        CmpI.replaceAllUsesWith(newOp);
        m_changed = true;
    }
}

void HalfPromotion::visitBinaryOperator(llvm::BinaryOperator &BI)
{
    if(BI.getType()->isHalfTy() &&
       (BI.getOpcode() == BinaryOperator::FAdd ||
        BI.getOpcode() == BinaryOperator::FSub ||
        BI.getOpcode() == BinaryOperator::FMul ||
        BI.getOpcode() == BinaryOperator::FDiv))
    {
        llvm::IGCIRBuilder<> builder(&BI);
        Value* op1 = builder.CreateFPExt(BI.getOperand(0), builder.getFloatTy());
        Value* op2 = builder.CreateFPExt(BI.getOperand(1), builder.getFloatTy());
        Value* newOp = builder.CreateBinOp(BI.getOpcode(), op1, op2);
        Value* f16Val = builder.CreateFPTrunc(newOp, builder.getHalfTy());
        BI.replaceAllUsesWith(f16Val);
        m_changed = true;
    }
}

/*

  What about casts like these?
  %162 = uitofp i32 %160 to half
  %163 = fpext half %162 to float
  %164 = fmul float %163, 1.600000e+01

  Is it safe to do this?
  %162 = uitofp i32 %160 to float
  %164 = fmul float %162, 1.600000e+01

*/

void HalfPromotion::visitCastInst(llvm::CastInst &CI)
{
    if(CI.getType()->isHalfTy() &&
        (CI.getOpcode() == CastInst::UIToFP ||
         CI.getOpcode() == CastInst::SIToFP))
    {
        llvm::IGCIRBuilder<> builder(&CI);
        Value* newOp = nullptr;
        if(CI.getOpcode() == CastInst::UIToFP)
        {
            newOp = builder.CreateUIToFP(CI.getOperand(0), builder.getFloatTy());
        }
        else
        {
            newOp = builder.CreateSIToFP(CI.getOperand(0), builder.getFloatTy());
        }
        Value* f16Val = builder.CreateFPTrunc(newOp, builder.getHalfTy());
        CI.replaceAllUsesWith(f16Val);
        m_changed = true;
    }
    else if(CI.getOperand(0)->getType()->isHalfTy() &&
            (CI.getOpcode() == CastInst::FPToUI ||
             CI.getOpcode() == CastInst::FPToSI))
    {
        llvm::IGCIRBuilder<> builder(&CI);
        Value* newOp = nullptr;
        Value* f32Val = builder.CreateFPExt(CI.getOperand(0), builder.getFloatTy());
        if(CI.getOpcode() == CastInst::FPToUI)
        {
            newOp = builder.CreateFPToUI(f32Val, CI.getType());
        }
        else
        {
            newOp = builder.CreateFPToSI(f32Val, CI.getType());
        }
        CI.replaceAllUsesWith(newOp);
        m_changed = true;
    }
}

void HalfPromotion::visitSelectInst(llvm::SelectInst &SI)
{
    if(SI.getTrueValue()->getType()->isHalfTy())
    {
        llvm::IGCIRBuilder<> builder(&SI);
        Value* opTrue = builder.CreateFPExt(SI.getTrueValue(), builder.getFloatTy());
        Value* opFalse = builder.CreateFPExt(SI.getFalseValue(), builder.getFloatTy());
        Value* newOp = builder.CreateSelect(SI.getCondition(), opTrue, opFalse);
        Value* f16Val = builder.CreateFPTrunc(newOp, builder.getHalfTy());
        SI.replaceAllUsesWith(f16Val);
        m_changed = true;
    }
}

void HalfPromotion::visitPHINode(llvm::PHINode &PHI)
{
    if(!PHI.getType()->isHalfTy())
    {
        return;
    }

    llvm::IGCIRBuilder<> builder(&PHI);
    llvm::PHINode *pNewPhi = llvm::PHINode::Create(builder.getFloatTy(), PHI.getNumIncomingValues(), "", &PHI);

    for(unsigned int i = 0; i < PHI.getNumIncomingValues(); ++i)
    {
        // For constants we do not need to set insert point.
        if(llvm::Instruction* I = dyn_cast<llvm::Instruction>(PHI.getIncomingValue(i)))
        {
            builder.SetInsertPoint(I->getNextNode());
        }
        Value* phiFloatValue = builder.CreateFPExt(PHI.getIncomingValue(i), builder.getFloatTy());
        pNewPhi->addIncoming(phiFloatValue, PHI.getIncomingBlock(i));
    }

    builder.SetInsertPoint(PHI.getParent()->getFirstNonPHI());
    Value* f16Val = builder.CreateFPTrunc(pNewPhi, builder.getHalfTy());
    PHI.replaceAllUsesWith(f16Val);
    PHI.eraseFromParent();
    m_changed = true;
}