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

//===- ConstantProp.cpp - Code to perform Simple Constant Propagation -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

/*========================== CustomUnsafeOptPass.cpp ==========================

This file contains IGC custom optimizations that are arithmetically safe.
The passes are
    CustomSafeOptPass
    GenSpecificPattern
    IGCConstProp
    IGCIndirectICBPropagaion
    CustomLoopInfo
    GenStrengthReduction
    FlattenSmallSwitch

CustomSafeOptPass does peephole optimizations 
For example, reduce the alloca size so there is a chance to promote indexed temp.

GenSpecificPattern reverts llvm changes back to what is needed. 
For example, llvm changes AND to OR, and GenSpecificPaattern changes it back to 
allow more optimizations

IGCConstProp was originated from llvm copy-prop code with one addition for 
shader-constant replacement. So llvm copyright is added above.

IGCIndirectICBPropagaion reads the immediate constant buffer from meta data and 
use them as immediates instead of using send messages to read from buffer.

CustomLoopInfo returns true if there is any sampleL in a loop for the driver.

GenStrengthReduction performs a fdiv optimization.

FlattenSmallSwitch flatten the if/else or switch structure and use cmp+sel 
instead if the structure is small.

=============================================================================*/

#include "Compiler/CustomSafeOptPass.hpp"
#include "Compiler/CISACodeGen/helper.h"
#include "Compiler/CodeGenPublic.h"
#include "Compiler/IGCPassSupport.h"
#include "GenISAIntrinsics/GenIntrinsics.h"
#include "GenISAIntrinsics/GenIntrinsicInst.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/ADT/Statistic.h>
#include <llvm/ADT/SetVector.h>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Analysis/ValueTracking.h>
#include "common/LLVMWarningsPop.hpp"

#include <set>
#include "../inc/common/secure_mem.h"

using namespace std;
using namespace llvm;
using namespace IGC;
using namespace GenISAIntrinsic;

// Register pass to igc-opt
#define PASS_FLAG1 "igc-custom-safe-opt"
#define PASS_DESCRIPTION1 "Custom Pass Optimization"
#define PASS_CFG_ONLY1 false
#define PASS_ANALYSIS1 false
IGC_INITIALIZE_PASS_BEGIN(CustomSafeOptPass, PASS_FLAG1, PASS_DESCRIPTION1, PASS_CFG_ONLY1, PASS_ANALYSIS1)
IGC_INITIALIZE_PASS_END(CustomSafeOptPass, PASS_FLAG1, PASS_DESCRIPTION1, PASS_CFG_ONLY1, PASS_ANALYSIS1)

char CustomSafeOptPass::ID = 0;

CustomSafeOptPass::CustomSafeOptPass() : FunctionPass(ID)
{
    initializeCustomSafeOptPassPass(*PassRegistry::getPassRegistry());
}

#if 0
// In some cases we link LLVM with NDEBUG set with IGC without NDEBUG set, this causes this function to not be missing during linking
// Once we switch to CMAKE this code can be removed
#if (defined(_INTERNAL) && defined(NDEBUG)) && ( !defined( LLVM_ENABLE_THREADS ) || LLVM_ENABLE_THREADS == 0 || ( defined( IGC_CMAKE ) && defined( NDEBUG ) ) || ( !defined( IGC_CMAKE ) && !defined( NDEBUG ) ) )
void AnnotateHappensBefore(const char *file, int line,
                           const volatile void *cv) {}
void AnnotateHappensAfter(const char *file, int line,
                          const volatile void *cv) {}
void AnnotateIgnoreWritesBegin(const char *file, int line) {}
void AnnotateIgnoreWritesEnd(const char *file, int line) {}
#endif
#endif

#define DEBUG_TYPE "CustomSafeOptPass"

STATISTIC(Stat_FcmpRemoved,  "Number of insts removed in FCmp Opt");
STATISTIC(Stat_FloatRemoved,  "Number of insts removed in Float Opt");
STATISTIC(Stat_DiscardRemoved,  "Number of insts removed in Discard Opt");

bool CustomSafeOptPass::runOnFunction(Function &F)
{
    psHasSideEffect = getAnalysis<CodeGenContextWrapper>().getCodeGenContext()->m_instrTypes.psHasSideEffect;
    visit(F);
    return true;
}

void CustomSafeOptPass::visitInstruction(Instruction &I)
{
    // nothing
}


void CustomSafeOptPass::visitAllocaInst(AllocaInst &I)
{
    // reduce the alloca size so there is a chance to promote indexed temp.

    // ex:                                                                  to:
    // dcl_indexable_temp x1[356], 4                                        dcl_indexable_temp x1[2], 4
    // mov x[1][354].xyzw, l(1f, 0f, -1f, 0f)                               mov x[1][0].xyzw, l(1f, 0f, -1f, 0f)
    // mov x[1][355].xyzw, l(1f, 1f, 0f, -1f)                               mov x[1][1].xyzw, l(1f, 1f, 0f, -1f)
    // mov r[1].xy, x[1][r[1].x + 354].xyxx                                 mov r[1].xy, x[1][r[1].x].xyxx

    // in llvm:                                                             to:
    // %outarray_x1 = alloca[356 x float], align 4                          %31 = alloca[2 x float]
    // %outarray_y2 = alloca[356 x float], align 4                          %32 = alloca[2 x float]
    // %27 = getelementptr[356 x float] * %outarray_x1, i32 0, i32 352      %35 = getelementptr[2 x float] * %31, i32 0, i32 0
    // store float 0.000000e+00, float* %27, align 4                        store float 0.000000e+00, float* %35, align 4
    // %28 = getelementptr[356 x float] * %outarray_y2, i32 0, i32 352      %36 = getelementptr[2 x float] * %32, i32 0, i32 0
    // store float 0.000000e+00, float* %28, align 4                        store float 0.000000e+00, float* %36, align 4
    // %43 = add nsw i32 %selRes_s, 354
    // %44 = getelementptr[356 x float] * %outarray_x1, i32 0, i32 %43      %51 = getelementptr[2 x float] * %31, i32 0, i32 %selRes_s
    // %45 = load float* %44, align 4                                       %52 = load float* %51, align 4

    llvm::Type* pType = I.getType()->getPointerElementType();
    if (!pType->isArrayTy() ||
        static_cast<ADDRESS_SPACE>(I.getType()->getAddressSpace()) != ADDRESS_SPACE_PRIVATE)
    {
        return;
    }

    if (!(pType->getArrayElementType()->isFloatingPointTy() ||
        pType->getArrayElementType()->isIntegerTy() ||
        pType->getArrayElementType()->isPointerTy() ))
    {
        return;
    }

    int index_lb = int_cast<int>(pType->getArrayNumElements());
    int index_ub = 0;

    // Find all uses of this alloca
    for (Value::user_iterator it = I.user_begin(), e = I.user_end(); it != e; ++it)
    {
        if (GetElementPtrInst *pGEP = llvm::dyn_cast<GetElementPtrInst>(*it))
        {
            ConstantInt *C0 = dyn_cast<ConstantInt>(pGEP->getOperand(1));
            if (!C0 || !C0->isZero())
            {
                return;
            }
            for (Value::user_iterator use_it = pGEP->user_begin(), use_e = pGEP->user_end(); use_it != use_e; ++use_it)
            {
                if (llvm::LoadInst* pLoad = llvm::dyn_cast<llvm::LoadInst>(*use_it))
                {
                }
                else if (llvm::StoreInst* pStore = llvm::dyn_cast<llvm::StoreInst>(*use_it))
                {
                    llvm::Value* pValueOp = pStore->getValueOperand();
                    if (pValueOp == *it)
                    {
                        // GEP instruction is the stored value of the StoreInst (not supported case)
                        return;
                    }
                    if (dyn_cast<ConstantInt>(pGEP->getOperand(2)) && pGEP->getOperand(2)->getType()->isIntegerTy(32))
                    {
                        int currentIndex = int_cast<int>(
                            dyn_cast<ConstantInt>(pGEP->getOperand(2))->getZExtValue());
                        index_lb = (currentIndex < index_lb) ? currentIndex : index_lb;
                        index_ub = (currentIndex > index_ub) ? currentIndex : index_ub;

                    }
                    else
                    {
                        return;
                    }
                }
                else
                {
                    // This is some other instruction. Right now we don't want to handle these
                    return;
                }
            }
        }
        else
        {
            if (!IsBitCastForLifetimeMark(dyn_cast<Value>(*it)))
            {
                return;
            }
        }
    }

    unsigned int newSize = index_ub + 1 - index_lb;
    if (newSize >= pType->getArrayNumElements())
    {
        return;
    }
    // found a case to optimize
    IRBuilder<> IRB(&I);
    llvm::ArrayType* allocaArraySize = llvm::ArrayType::get(pType->getArrayElementType(), newSize);
    llvm::Value* newAlloca = IRB.CreateAlloca( allocaArraySize, 0);
    llvm::Value* gepArg1;

    for (Value::user_iterator it = I.user_begin(), e = I.user_end(); it != e; ++it)
    {
        if (GetElementPtrInst *pGEP = llvm::dyn_cast<GetElementPtrInst>(*it))
        {
            if (dyn_cast<ConstantInt>(pGEP->getOperand(2)))
            {
                // pGEP->getOperand(2) is constant. Reduce the constant value directly
                int newIndex = int_cast<int>(dyn_cast<ConstantInt>(pGEP->getOperand(2))->getZExtValue())
                    - index_lb;
                gepArg1 = IRB.getInt32(newIndex);
            }
            else
            {
                // pGEP->getOperand(2) is not constant. create a sub instruction to reduce it
                gepArg1 = BinaryOperator::CreateSub(pGEP->getOperand(2), IRB.getInt32(index_lb), "reducedIndex", pGEP);
            }
            llvm::Value* gepArg[] = { pGEP->getOperand(1), gepArg1 };
            llvm::Value* pGEPnew = GetElementPtrInst::Create(nullptr, newAlloca, gepArg, "", pGEP);
            pGEP->replaceAllUsesWith(pGEPnew);
        }
    }
}

void CustomSafeOptPass::visitCallInst(CallInst &C)
{
    // discard optimizations
    if(llvm::GenIntrinsicInst* inst = llvm::dyn_cast<GenIntrinsicInst>(&C))
    {
        GenISAIntrinsic::ID id = inst->getIntrinsicID();
        // try to prune the destination size
        switch (id)
        {
        case GenISAIntrinsic::GenISA_discard:
        {
            Value *srcVal0 = C.getOperand(0);
            if(ConstantInt *CI = dyn_cast<ConstantInt>(srcVal0))
            {
                if(CI->isZero()){ // i1 is false
                    C.eraseFromParent();
                    ++Stat_DiscardRemoved;
                }
                else if(!psHasSideEffect)
                {
                    BasicBlock *blk = C.getParent();
                    BasicBlock *pred = blk->getSinglePredecessor();
                    if(blk && pred)
                    {
                        BranchInst *cbr = dyn_cast<BranchInst>(pred->getTerminator());
                        if(cbr && cbr->isConditional())
                        {
                            if(blk == cbr->getSuccessor(0))
                            {
                                C.setOperand(0, cbr->getCondition());
                                C.removeFromParent();
                                C.insertBefore(cbr);
                            }
                            else if(blk == cbr->getSuccessor(1))
                            {
                                Value *flipCond = llvm::BinaryOperator::CreateNot(cbr->getCondition(), "", cbr);
                                C.setOperand(0, flipCond);
                                C.removeFromParent();
                                C.insertBefore(cbr);
                            }
                        }
                    }
                }
            }
            break;
        }

        case GenISAIntrinsic::GenISA_bfi:
            visitBfi(inst);
            break;

        case GenISAIntrinsic::GenISA_f32tof16_rtz:
        {
            visitf32tof16(inst);
            break;
        }

        case GenISAIntrinsic::GenISA_sampleBptr:
        {
            visitSampleBptr(llvm::cast<llvm::SampleIntrinsic>(inst));
            break;
        }

        case GenISAIntrinsic::GenISA_umulH:
        {
            visitMulH(inst, false);
            break;
        }

        case GenISAIntrinsic::GenISA_imulH:
        {
            visitMulH(inst, true);
            break;
        }

        default:
            break;
        }
    }
}

//
// pattern match packing of two half float from f32tof16:
//
// % 43 = call float @genx.GenISA.f32tof16.rtz(float %res_s55.i)
// % 44 = call float @genx.GenISA.f32tof16.rtz(float %res_s59.i)
// % 47 = bitcast float %44 to i32
// % 49 = bitcast float %43 to i32
// %addres_s68.i = shl i32 % 47, 16
// % mulres_s69.i = add i32 %addres_s68.i, % 49
// % 51 = bitcast i32 %mulres_s69.i to float
// into
// %43 = call half @genx.GenISA_ftof_rtz(float %res_s55.i)
// %44 = call half @genx.GenISA_ftof_rtz(float %res_s59.i)
// %45 = insertelement <2 x half>undef, %43, 0
// %46 = insertelement <2 x half>%45, %44, 1
// %51 = bitcast <2 x half> %46 to float
void CustomSafeOptPass::visitf32tof16(llvm::CallInst* inst)
{
    if (!inst->hasOneUse())
    {
        return;
    }

    BitCastInst* bitcast = dyn_cast<BitCastInst>(*(inst->user_begin()));
    if (!bitcast || !bitcast->hasOneUse() || !bitcast->getType()->isIntegerTy(32))
    {
        return;
    }
    Instruction* addInst = dyn_cast<BinaryOperator>(*(bitcast->user_begin()));
    if (!addInst || addInst->getOpcode() != Instruction::Add || !addInst->hasOneUse())
    {
        return;
    }
    Instruction* lastValue = addInst;

    if (BitCastInst* finalBitCast = dyn_cast<BitCastInst>(*(addInst->user_begin())))
    {
        lastValue = finalBitCast;
    }

    // check the other half
    Value* otherOpnd = addInst->getOperand(0) == bitcast ? addInst->getOperand(1) : addInst->getOperand(0);
    Instruction* shiftOrMul = dyn_cast<BinaryOperator>(otherOpnd);

    if (!shiftOrMul ||
        (shiftOrMul->getOpcode() != Instruction::Shl && shiftOrMul->getOpcode() != Instruction::Mul))
    {
        return;
    }
    bool isShift = shiftOrMul->getOpcode() == Instruction::Shl;
    ConstantInt* constVal = dyn_cast<ConstantInt>(shiftOrMul->getOperand(1));
    if (!constVal || !constVal->equalsInt(isShift ? 16 : 65536))
    {
        return;
    }
    BitCastInst* bitcast2 = dyn_cast<BitCastInst>(shiftOrMul->getOperand(0));
    if (!bitcast2)
    {
        return;
    }
    llvm::GenIntrinsicInst* valueHi = dyn_cast<GenIntrinsicInst>(bitcast2->getOperand(0));
    if (!valueHi || valueHi->getIntrinsicID() != GenISA_f32tof16_rtz)
    {
        return;
    }

    IRBuilder<> builder(lastValue);
    Type* funcType[] = { Type::getHalfTy(builder.getContext()), Type::getFloatTy(builder.getContext()) };
    Function* f32tof16_rtz = GenISAIntrinsic::getDeclaration(inst->getParent()->getParent()->getParent(),
        GenISAIntrinsic::GenISA_ftof_rtz, funcType);
    Value* loVal = builder.CreateCall(f32tof16_rtz, inst->getArgOperand(0));
    Value* hiVal = builder.CreateCall(f32tof16_rtz, valueHi->getArgOperand(0));
    Type* halfx2 = VectorType::get(Type::getHalfTy(builder.getContext()), 2);
    Value* vector = builder.CreateInsertElement(UndefValue::get(halfx2), loVal, builder.getInt32(0));
    vector = builder.CreateInsertElement(vector, hiVal, builder.getInt32(1));
    vector = builder.CreateBitCast(vector, lastValue->getType());
    lastValue->replaceAllUsesWith(vector);
    lastValue->eraseFromParent();
}

void CustomSafeOptPass::visitBfi(llvm::CallInst* inst)
{
    assert(inst->getType()->isIntegerTy(32));
    ConstantInt* widthV = dyn_cast<ConstantInt>(inst->getOperand(0));
    ConstantInt* offsetV = dyn_cast<ConstantInt>(inst->getOperand(1));
    if(widthV && offsetV)
    {
        // transformation is beneficial if src3 is constant or if the offset is zero
        if(isa<ConstantInt>(inst->getOperand(3)) || offsetV->isZero())
        {
            unsigned int width = static_cast<unsigned int>(widthV->getZExtValue());
            unsigned int offset = static_cast<unsigned int>(offsetV->getZExtValue());
            unsigned int bitMask = ((1 << width) - 1) << offset;
            IRBuilder<> builder(inst);
            // dst = ((src2 << offset) & bitmask) | (src3 & ~bitmask)
            Value* firstTerm = builder.CreateShl(inst->getOperand(2), offsetV);
            firstTerm = builder.CreateAnd(firstTerm, builder.getInt32(bitMask));
            Value* secondTerm = builder.CreateAnd(inst->getOperand(3), builder.getInt32(~bitMask));
            Value* dst = builder.CreateOr(firstTerm, secondTerm);
            inst->replaceAllUsesWith(dst);
            inst->eraseFromParent();
        }
    }
}

void CustomSafeOptPass::visitMulH(CallInst* inst, bool isSigned)
{
    ConstantInt* src0 = dyn_cast<ConstantInt>(inst->getOperand(0));
    ConstantInt* src1 = dyn_cast<ConstantInt>(inst->getOperand(1));
    if (src0 && src1)
    {
        unsigned nbits = inst->getType()->getIntegerBitWidth();
        assert(nbits < 64);

        if (isSigned)
        {
            uint64_t ui0 = src0->getZExtValue();
            uint64_t ui1 = src1->getZExtValue();
            uint64_t r = ((ui0 * ui1) >> nbits);
            inst->replaceAllUsesWith(ConstantInt::get(inst->getType(), r));
        }
        else
        {
            int64_t si0 = src0->getSExtValue();
            int64_t si1 = src1->getSExtValue();
            int64_t r = ((si0 * si1) >> nbits);
            inst->replaceAllUsesWith(ConstantInt::get(inst->getType(), r, true));
        }
        inst->eraseFromParent();
    }
}

// if phi is used in a FPTrunc and the sources all come from fpext we can skip the conversions
void CustomSafeOptPass::visitFPTruncInst(FPTruncInst &I)
{
    if(PHINode* phi = dyn_cast<PHINode>(I.getOperand(0)))
    {
        bool foundPattern = true;
        unsigned int numSrc = phi->getNumIncomingValues();
        SmallVector<Value*, 6> newSources(numSrc);
        for(unsigned int i = 0; i < numSrc; i++)
        {
            FPExtInst* source = dyn_cast<FPExtInst>(phi->getIncomingValue(i));
            if(source && source->getOperand(0)->getType() == I.getType())
            {
                newSources[i] = source->getOperand(0);
            }
            else
            {
                foundPattern = false;
                break;
            }
        }
        if(foundPattern)
        {
            PHINode* newPhi = PHINode::Create(I.getType(), numSrc, "", phi);
            for(unsigned int i = 0; i < numSrc; i++)
            {
                newPhi->addIncoming(newSources[i], phi->getIncomingBlock(i));
            }
            
            I.replaceAllUsesWith(newPhi);
            I.eraseFromParent();
            // if phi has other uses we add a fpext to avoid having two phi
            if(!phi->use_empty())
            {
                IRBuilder<> builder(&(*phi->getParent()->getFirstInsertionPt()));
                Value* extV = builder.CreateFPExt(newPhi, phi->getType());
                phi->replaceAllUsesWith(extV);
            }
        }
    }
    
}

void CustomSafeOptPass::visitFPToUIInst(llvm::FPToUIInst& FPUII)
{
    if (llvm::IntrinsicInst* intrinsicInst = llvm::dyn_cast<llvm::IntrinsicInst>(FPUII.getOperand(0)))
    {
        if (intrinsicInst->getIntrinsicID() == Intrinsic::floor)
        {
            FPUII.setOperand(0, intrinsicInst->getOperand(0));
            if (intrinsicInst->use_empty())
            {
                intrinsicInst->eraseFromParent();
            }
        }
    }
}

/// This remove simplify bitcast across phi and select instruction
/// LLVM doesn't catch those case and it is common in DX10+ as the input language is not typed
/// TODO: support cases where some sources are constant
void CustomSafeOptPass::visitBitCast(BitCastInst &BC)
{
    if(SelectInst* sel = dyn_cast<SelectInst>(BC.getOperand(0)))
    {
        BitCastInst* trueVal = dyn_cast<BitCastInst>(sel->getTrueValue());
        BitCastInst* falseVal = dyn_cast<BitCastInst>(sel->getFalseValue());
        if(trueVal && falseVal)
        {
            Value* trueValOrignalType = trueVal->getOperand(0);
            Value* falseValOrignalType = falseVal->getOperand(0);
            if(trueValOrignalType->getType() == BC.getType() &&
                falseValOrignalType->getType() == BC.getType())
            {
                Value* cond = sel->getCondition();
                Value* newVal = SelectInst::Create(cond, trueValOrignalType, falseValOrignalType, "", sel);
                BC.replaceAllUsesWith(newVal);
                BC.eraseFromParent();
            }
        }
    }
    else if(PHINode* phi = dyn_cast<PHINode>(BC.getOperand(0)))
    {
        if(phi->hasOneUse())
        {
            bool foundPattern = true;
            unsigned int numSrc = phi->getNumIncomingValues();
            SmallVector<Value*, 6> newSources(numSrc);
            for(unsigned int i = 0; i < numSrc; i++)
            {
                BitCastInst* source = dyn_cast<BitCastInst>(phi->getIncomingValue(i));
                if(source && source->getOperand(0)->getType() == BC.getType())
                {
                    newSources[i] = source->getOperand(0);
                }
                else if(Constant* C = dyn_cast<Constant>(phi->getIncomingValue(i)))
                {
                    newSources[i] = ConstantExpr::getCast(Instruction::BitCast, C, BC.getType());
                }
                else
                {
                    foundPattern = false;
                    break;
                }
            }
            if(foundPattern)
            {
                PHINode* newPhi = PHINode::Create(BC.getType(), numSrc, "", phi);
                for(unsigned int i = 0; i < numSrc; i++)
                {
                    newPhi->addIncoming(newSources[i], phi->getIncomingBlock(i));
                }
                BC.replaceAllUsesWith(newPhi);
                BC.eraseFromParent();
            }
        }
    }
}

bool CustomSafeOptPass::isEmulatedAdd(BinaryOperator &I)
{
    if (I.getOpcode() == Instruction::Or)
    {
        if (BinaryOperator *OrOp0 = dyn_cast<BinaryOperator>(I.getOperand(0)))
        {
            if (OrOp0->getOpcode() == Instruction::Shl)
            {
                // Check the SHl. If we have a constant Shift Left val then we can check
                // it to see if it is emulating an add.
                if (ConstantInt *pConstShiftLeft = dyn_cast<ConstantInt>(OrOp0->getOperand(1)))
                {
                    if (ConstantInt *pConstOrVal = dyn_cast<ConstantInt>(I.getOperand(1)))
                    {
                        int const_shift = int_cast<int>(pConstShiftLeft->getZExtValue());
                        int const_or_val = int_cast<int>(pConstOrVal->getZExtValue());
                        if ((1 << const_shift) > const_or_val)
                        {
                            // The value fits in the shl. So this is an emulated add.
                            return true;
                        }
                    }
                }
            }
            else if (OrOp0->getOpcode() == Instruction::Mul)
            {
                // Check to see if the Or is emulating and add.
                // If we have a constant Mul and a constant Or. The Mul constant needs to be divisible by the rounded up 2^n of Or value.
                if (ConstantInt *pConstMul = dyn_cast<ConstantInt>(OrOp0->getOperand(1)))
                {
                    if (ConstantInt *pConstOrVal = dyn_cast<ConstantInt>(I.getOperand(1)))
                    {
                        DWORD const_or_val = int_cast<DWORD>(pConstOrVal->getZExtValue());
                        DWORD nextPowerOfTwo = iSTD::RoundPower2(const_or_val + 1);
                        if (pConstMul->getZExtValue() % nextPowerOfTwo == 0)
                        {
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

void CustomSafeOptPass::visitBinaryOperator(BinaryOperator &I)
{
    // move immediate value in consecutive integer adds to the last added value.
    // this can allow more chance of doing CSE and memopt.
    //    a = b + 8
    //    d = a + c
    //        to
    //    a = b + c
    //    d = a + 8

    if (I.getType()->isIntegerTy())
    {
        if ((I.getOpcode() == Instruction::Add || isEmulatedAdd(I)) &&
            I.hasOneUse())
        {
            ConstantInt *src0imm = dyn_cast<ConstantInt>(I.getOperand(0));
            ConstantInt *src1imm = dyn_cast<ConstantInt>(I.getOperand(1));
            if (src0imm || src1imm)
            {
                llvm::Instruction* nextInst = llvm::dyn_cast<llvm::Instruction>(*(I.user_begin()));
                if (nextInst && nextInst->getOpcode() == Instruction::Add)
                {
                    ConstantInt *secondSrc0imm = dyn_cast<ConstantInt>(nextInst->getOperand(0));
                    ConstantInt *secondSrc1imm = dyn_cast<ConstantInt>(nextInst->getOperand(1));
                    // found 2 add instructions to swap srcs
                    if (!secondSrc0imm && !secondSrc1imm && nextInst->getOperand(0) != nextInst->getOperand(1))
                    {
                        for (int i = 0; i < 2; i++)
                        {
                            if (nextInst->getOperand(i) == &I)
                            {
                                Value * newAdd = BinaryOperator::CreateAdd(src0imm ? I.getOperand(1) : I.getOperand(0), nextInst->getOperand(1 - i), "", nextInst);
                                nextInst->setOperand(0, newAdd);
                                nextInst->setOperand(1, I.getOperand(src0imm ? 0 : 1));
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
}

void IGC::CustomSafeOptPass::visitSampleBptr(llvm::SampleIntrinsic* sampleInst)
{
    // sampleB with bias_value==0 -> sample
    llvm::ConstantFP* constBias = llvm::dyn_cast<llvm::ConstantFP>(sampleInst->getOperand(0));
    if(constBias && constBias->isZero())
    {
        // Copy args skipping bias operand:
        llvm::SmallVector<llvm::Value*, 10> args;
        for(unsigned int i = 1; i < sampleInst->getNumArgOperands(); i++)
        {
            args.push_back(sampleInst->getArgOperand(i));
        }

        // Copy overloaded types unchanged:
        llvm::SmallVector<llvm::Type*, 4> overloadedTys;
        overloadedTys.push_back(sampleInst->getCalledFunction()->getReturnType());
        overloadedTys.push_back(sampleInst->getOperand(0)->getType());
        overloadedTys.push_back(sampleInst->getTextureValue()->getType());
        overloadedTys.push_back(sampleInst->getSamplerValue()->getType());

        llvm::Function* sampleIntr = llvm::GenISAIntrinsic::getDeclaration(
            sampleInst->getParent()->getParent()->getParent(),
            GenISAIntrinsic::GenISA_sampleptr,
            overloadedTys);

        llvm::Value *newSample = llvm::CallInst::Create(sampleIntr, args, "", sampleInst);
        sampleInst->replaceAllUsesWith(newSample);
    }
}

// Register pass to igc-opt
#define PASS_FLAG2 "igc-gen-specific-pattern"
#define PASS_DESCRIPTION2 "LastPatternMatch Pass"
#define PASS_CFG_ONLY2 false
#define PASS_ANALYSIS2 false
IGC_INITIALIZE_PASS_BEGIN(GenSpecificPattern, PASS_FLAG2, PASS_DESCRIPTION2, PASS_CFG_ONLY2, PASS_ANALYSIS2)
IGC_INITIALIZE_PASS_END(GenSpecificPattern, PASS_FLAG2, PASS_DESCRIPTION2, PASS_CFG_ONLY2, PASS_ANALYSIS2)

char GenSpecificPattern::ID = 0;

GenSpecificPattern::GenSpecificPattern() : FunctionPass(ID)
{
    initializeGenSpecificPatternPass(*PassRegistry::getPassRegistry());
}

bool GenSpecificPattern::runOnFunction(Function &F)
{
    visit(F);
    return true;
}

// Lower SDiv to better code sequence if possible
void GenSpecificPattern::visitSDiv(llvm::BinaryOperator& I)
{
    if(ConstantInt* divisor = dyn_cast<ConstantInt>(I.getOperand(1)))
    {
        // signed division of power of 2 can be transformed to asr
        // For negative values we need to make sure we round correctly
        int log2Div = divisor->getValue().exactLogBase2();
        if(log2Div > 0)
        {
            unsigned int intWidth = divisor->getBitWidth();
            IRBuilder<> builder(&I);
            Value * signedBitOnly = I.getOperand(0);
            if(log2Div > 1)
            {
                signedBitOnly = builder.CreateAShr(signedBitOnly, builder.getIntN(intWidth, intWidth - 1));
            }
            Value* offset = builder.CreateLShr(signedBitOnly, builder.getIntN(intWidth, intWidth - log2Div));
            Value* offsetedValue = builder.CreateAdd(I.getOperand(0), offset);
            Value* newValue = builder.CreateAShr(offsetedValue, builder.getIntN(intWidth, log2Div));
            I.replaceAllUsesWith(newValue);
            I.eraseFromParent();
        }
    }
}

void GenSpecificPattern::visitBinaryOperator(BinaryOperator &I)
{
    /*
    llvm changes ADD to OR when possible, and this optimization changes it back and allow 2 ADDs to merge.
    This can avoid scattered read for constant buffer when the index is calculated by shl + or + add.

    ex:
    from
        %22 = shl i32 %14, 2
        %23 = or i32 %22, 3
        %24 = add i32 %23, 16
    to
        %22 = shl i32 %14, 2
        %23 = add i32 %22, 19
    */

    if (I.getOpcode() == Instruction::Or)
    {
        /*
        from
            % 22 = shl i32 % 14, 2
            % 23 = or i32 % 22, 3
        to
            % 22 = shl i32 % 14, 2
            % 23 = add i32 % 22, 3
        */
        ConstantInt *OrConstant = dyn_cast<ConstantInt>(I.getOperand(1));
        if (OrConstant)
        {
            llvm::Instruction* ShlInst = llvm::dyn_cast<llvm::Instruction>(I.getOperand(0));
            if (ShlInst && ShlInst->getOpcode() == Instruction::Shl)
            {
                ConstantInt *ShlConstant = dyn_cast<ConstantInt>(ShlInst->getOperand(1));
                if (ShlConstant)
                {
                    // if the constant bit width is larger than 64, we cannot store ShlIntValue and OrIntValue rawdata as uint64_t.
                    // will need a fix then
                    assert(ShlConstant->getBitWidth() <= 64);
                    assert(OrConstant->getBitWidth() <= 64);

                    uint64_t ShlIntValue = *(ShlConstant->getValue()).getRawData();
                    uint64_t OrIntValue = *(OrConstant->getValue()).getRawData();

                    if (OrIntValue < pow(2, ShlIntValue))
                    {
                        Value * newAdd = BinaryOperator::CreateAdd(I.getOperand(0), I.getOperand(1), "", &I);
                        I.replaceAllUsesWith(newAdd);
                    }
                }
            }
        }
    }
    else if (I.getOpcode() == Instruction::Add)
    {
        /*
        from
            %23 = add i32 %22, 3
            %24 = add i32 %23, 16
        to
            %24 = add i32 %22, 19
        */
        for (int ImmSrcId1 = 0; ImmSrcId1 < 2; ImmSrcId1++)
        {
            ConstantInt *IConstant = dyn_cast<ConstantInt>(I.getOperand(ImmSrcId1));
            if (IConstant)
            {
                llvm::Instruction* AddInst = llvm::dyn_cast<llvm::Instruction>(I.getOperand(1 - ImmSrcId1));
                if (AddInst && AddInst->getOpcode() == Instruction::Add)
                {
                    for (int ImmSrcId2 = 0; ImmSrcId2 < 2; ImmSrcId2++)
                    {
                        ConstantInt *AddConstant = dyn_cast<ConstantInt>(AddInst->getOperand(ImmSrcId2));
                        if (AddConstant)
                        {
                            llvm::APInt CombineAddValue = AddConstant->getValue() + IConstant->getValue();
                            I.setOperand(0, AddInst->getOperand(1 - ImmSrcId2));
                            I.setOperand(1, ConstantInt::get(I.getType(), CombineAddValue));
                        }
                    }
                }
            }
        }
    }
}

void GenSpecificPattern::visitCmpInst(CmpInst &I)
{
    CodeGenContext* pCtx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
    if (pCtx->m_DriverInfo.IgnoreNan())
    {
        if (I.getPredicate() == CmpInst::FCMP_ORD)
        {
            I.replaceAllUsesWith(ConstantInt::getTrue(I.getType()));
        }
    }
}

void GenSpecificPattern::visitSelectInst(SelectInst &I)
{
    /*
    from
        %res_s42 = icmp eq i32 %src1_s41, 0
        %src1_s81 = select i1 %res_s42, i32 15, i32 0
    to
        %res_s42 = icmp eq i32 %src1_s41, 0
        %17 = sext i1 %res_s42 to i32
        %18 = and i32 15, %17

               or

    from
        %res_s73 = fcmp oge float %res_s61, %42
        %res_s187 = select i1 %res_s73, float 1.000000e+00, float 0.000000e+00
    to
        %res_s73 = fcmp oge float %res_s61, %42
        %46 = sext i1 %res_s73 to i32
        %47 = and i32 %46, 1065353216
        %48 = bitcast i32 %47 to float
    */

    assert( I.getOpcode() == Instruction::Select );

    bool skipOpt = false;

    ConstantInt *Cint = dyn_cast<ConstantInt>( I.getOperand(2) );
    if( Cint && Cint->isZero() )
    {
        llvm::Instruction* cmpInst = llvm::dyn_cast<llvm::Instruction>( I.getOperand(0) );
        if( cmpInst &&
            cmpInst->getOpcode() == Instruction::ICmp &&
            I.getOperand(1) != cmpInst->getOperand(0) )
        {
            // disable the cases for csel or where we can optimize the instructions to such as add.ge.* later in vISA
            ConstantInt *C = dyn_cast<ConstantInt>( cmpInst->getOperand(1) );
            if( C && C->isZero() )
            {
                skipOpt = true;
            }

            if( !skipOpt )
            {
                // temporary disable the case where cmp is used in multiple sel, and not all of them have src2=0
                // we should remove this if we can allow both flag and grf dst for the cmp to be used.
                for(auto selI = cmpInst->user_begin(), E = cmpInst->user_end(); selI!=E; ++selI)
                {
                    if(llvm::SelectInst* selInst = llvm::dyn_cast<llvm::SelectInst>(*selI))
                    {
                        ConstantInt *C = dyn_cast<ConstantInt>( selInst->getOperand(2) );
                        if( !(C && C->isZero()) )
                        {
                            skipOpt = true;
                            break;
                        }
                    }
                }
            }

            if( !skipOpt )
            {
                Value * newValueSext = CastInst::CreateSExtOrBitCast( I.getOperand(0), I.getType(), "", &I );
                Value * newValueAnd = BinaryOperator::CreateAnd( I.getOperand(1), newValueSext, "", &I );
                I.replaceAllUsesWith( newValueAnd );
            }
        }
    }
    else
    {
        ConstantFP *Cfp = dyn_cast<ConstantFP>( I.getOperand(2) );
        if( Cfp && Cfp->isZero() )
        {
            llvm::Instruction* cmpInst = llvm::dyn_cast<llvm::Instruction>( I.getOperand(0) );
            if( cmpInst &&
                cmpInst->getOpcode() == Instruction::FCmp &&
                I.getOperand(1) != cmpInst->getOperand(0) )
            {
                // disable the cases for csel or where we can optimize the instructions to such as add.ge.* later in vISA
                ConstantFP *C = dyn_cast<ConstantFP>( cmpInst->getOperand(1) );
                if( C && C->isZero() )
                {
                    skipOpt = true;
                }

                if( !skipOpt )
                {
                    // temporary disable the case where cmp is used in multiple sel, and not all of them have src2=0
                    // we should remove this if we can allow both flag and grf dst for the cmp to be used.
                    for(auto selI = cmpInst->user_begin(), E = cmpInst->user_end(); selI!=E; ++selI)
                    {
                        if(llvm::SelectInst* selInst = llvm::dyn_cast<llvm::SelectInst>(*selI))
                        {
                            ConstantFP *C = dyn_cast<ConstantFP>( selInst->getOperand(2) );
                            if( !(C && C->isZero()) )
                            {
                                skipOpt = true;
                                break;
                            }
                        }
                    }
                }

                if( !skipOpt )
                {
                    ConstantFP *C1 = dyn_cast<ConstantFP>( I.getOperand(1) );
                    if (C1)
                    {
                        if (C1->getType()->isHalfTy())
                        {
                            Value * newValueSext = CastInst::CreateSExtOrBitCast(I.getOperand(0), Type::getInt16Ty(I.getContext()), "", &I);
                            Value * newConstant = ConstantInt::get(I.getContext(), C1->getValueAPF().bitcastToAPInt());
                            Value * newValueAnd = BinaryOperator::CreateAnd(newValueSext, newConstant, "", &I);
                            Value * newValueCastFP = CastInst::CreateZExtOrBitCast(newValueAnd, Type::getHalfTy(I.getContext()), "", &I);
                            I.replaceAllUsesWith(newValueCastFP);
                        }
                        else if (C1->getType()->isFloatTy())
                        {
                            Value * newValueSext = CastInst::CreateSExtOrBitCast(I.getOperand(0), Type::getInt32Ty(I.getContext()), "", &I);
                            Value * newConstant = ConstantInt::get(I.getContext(), C1->getValueAPF().bitcastToAPInt());
                            Value * newValueAnd = BinaryOperator::CreateAnd(newValueSext, newConstant, "", &I);
                            Value * newValueCastFP = CastInst::CreateZExtOrBitCast(newValueAnd, Type::getFloatTy(I.getContext()), "", &I);
                            I.replaceAllUsesWith(newValueCastFP);
                        }
                    }
                    else
                    {
                        if (I.getOperand(1)->getType()->isHalfTy())
                        {
                            Value * newValueSext = CastInst::CreateSExtOrBitCast(I.getOperand(0), Type::getInt16Ty(I.getContext()), "", &I);
                            Value * newValueBitcast = CastInst::CreateZExtOrBitCast(I.getOperand(1), Type::getInt16Ty(I.getContext()), "", &I);
                            Value * newValueAnd = BinaryOperator::CreateAnd(newValueSext, newValueBitcast, "", &I);
                            Value * newValueCastFP = CastInst::CreateZExtOrBitCast(newValueAnd, Type::getHalfTy(I.getContext()), "", &I); \
                            I.replaceAllUsesWith(newValueCastFP);
                        }
                        else if (I.getOperand(1)->getType()->isFloatTy())
                        {
                            Value * newValueSext = CastInst::CreateSExtOrBitCast(I.getOperand(0), Type::getInt32Ty(I.getContext()), "", &I);
                            Value * newValueBitcast = CastInst::CreateZExtOrBitCast(I.getOperand(1), Type::getInt32Ty(I.getContext()), "", &I);
                            Value * newValueAnd = BinaryOperator::CreateAnd(newValueSext, newValueBitcast, "", &I);
                            Value * newValueCastFP = CastInst::CreateZExtOrBitCast(newValueAnd, Type::getFloatTy(I.getContext()), "", &I); \
                            I.replaceAllUsesWith(newValueCastFP);
                        }
                    }
                }
            }
        }
    }

    /*
    from
        %230 = sdiv i32 %214, %scale
        %276 = trunc i32 %230 to i8
        %277 = icmp slt i32 %230, 255
        %278 = select i1 %277, i8 %276, i8 -1
    to
        %230 = sdiv i32 %214, %scale
        %277 = icmp slt i32 %230, 255
        %278 = select i1 %277, i32 %230, i32 255
        %279 = trunc i32 %278 to i8

        This tranform allows for min/max instructions to be
        picked up in the IsMinOrMax instruction in PatternMatchPass.cpp
    */
    if (auto *compInst = dyn_cast<ICmpInst>(I.getOperand(0)))
    {
        auto selOp1 = I.getOperand(1);
        auto selOp2 = I.getOperand(2);
        auto cmpOp0 = compInst->getOperand(0);
        auto cmpOp1 = compInst->getOperand(1);
        auto trunc1 = dyn_cast<TruncInst>(selOp1);
        auto trunc2 = dyn_cast<TruncInst>(selOp2);
        auto icmpType = compInst->getOperand(0)->getType();

        if (selOp1->getType()->isIntegerTy() &&
            icmpType->isIntegerTy() &&
            selOp1->getType()->getIntegerBitWidth() < icmpType->getIntegerBitWidth())
        {
            Value * newSelOp1 = NULL;
            Value * newSelOp2 = NULL;
            if (trunc1 &&
                (trunc1->getOperand(0) == cmpOp0 ||
                 trunc1->getOperand(0) == cmpOp1))
            {
                newSelOp1 = (trunc1->getOperand(0) == cmpOp0) ? cmpOp0 : cmpOp1;
            }

            if (trunc2 &&
                (trunc2->getOperand(0) == cmpOp0 ||
                 trunc2->getOperand(0) == cmpOp1))
            {
                newSelOp2 = (trunc2->getOperand(0) == cmpOp0) ? cmpOp0 : cmpOp1;
            }

            if (isa<llvm::ConstantInt>(selOp1) &&
                isa<llvm::ConstantInt>(cmpOp0) &&
                (cast<llvm::ConstantInt>(selOp1)->getZExtValue() ==
                 cast<llvm::ConstantInt>(cmpOp0)->getZExtValue()))
            {
                assert(newSelOp1 == NULL);
                newSelOp1 = cmpOp0;
            }

            if (isa<llvm::ConstantInt>(selOp1) &&
                isa<llvm::ConstantInt>(cmpOp1) &&
                (cast<llvm::ConstantInt>(selOp1)->getZExtValue() ==
                 cast<llvm::ConstantInt>(cmpOp1)->getZExtValue()))
            {
                assert(newSelOp1 == NULL);
                newSelOp1 = cmpOp1;
            }

            if (isa<llvm::ConstantInt>(selOp2) &&
                isa<llvm::ConstantInt>(cmpOp0) &&
                (cast<llvm::ConstantInt>(selOp2)->getZExtValue() ==
                 cast<llvm::ConstantInt>(cmpOp0)->getZExtValue()))
            {
                assert(newSelOp2 == NULL);
                newSelOp2 = cmpOp0;
            }

            if (isa<llvm::ConstantInt>(selOp2) &&
                isa<llvm::ConstantInt>(cmpOp1) &&
                (cast<llvm::ConstantInt>(selOp2)->getZExtValue() ==
                 cast<llvm::ConstantInt>(cmpOp1)->getZExtValue()))
            {
                assert(newSelOp2 == NULL);
                newSelOp2 = cmpOp1;
            }

            if (newSelOp1 && newSelOp2)
            {
                auto newSelInst = SelectInst::Create(I.getCondition(), newSelOp1, newSelOp2, "", &I);
                auto newTruncInst = TruncInst::CreateTruncOrBitCast(newSelInst, selOp1->getType(), "", &I);
                I.replaceAllUsesWith(newTruncInst);
                I.eraseFromParent();
            }
        }

    }

}

void GenSpecificPattern::visitZExtInst(ZExtInst &ZEI)
{
    CmpInst *Cmp = dyn_cast<CmpInst>(ZEI.getOperand(0));
    if (!Cmp)
        return;

    IRBuilder<> Builder(&ZEI);

    Value *S = Builder.CreateSExt(Cmp, ZEI.getType());
    Value *N = Builder.CreateNeg(S);
    ZEI.replaceAllUsesWith(N);
    ZEI.eraseFromParent();
}

void GenSpecificPattern::visitIntToPtr(llvm::IntToPtrInst& I)
{
    if(ZExtInst* zext = dyn_cast<ZExtInst>(I.getOperand(0)))
    {
        IRBuilder<> builder(&I);
        Value* newV = builder.CreateIntToPtr(zext->getOperand(0), I.getType());
        I.replaceAllUsesWith(newV);
        I.eraseFromParent();
    }
}

// Register pass to igc-opt
#define PASS_FLAG3 "igc-const-prop"
#define PASS_DESCRIPTION3 "Custom Const-prop Pass"
#define PASS_CFG_ONLY3 false
#define PASS_ANALYSIS3 false
IGC_INITIALIZE_PASS_BEGIN(IGCConstProp, PASS_FLAG3, PASS_DESCRIPTION3, PASS_CFG_ONLY3, PASS_ANALYSIS3)
IGC_INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
IGC_INITIALIZE_PASS_END(IGCConstProp, PASS_FLAG3, PASS_DESCRIPTION3, PASS_CFG_ONLY3, PASS_ANALYSIS3)

char IGCConstProp::ID = 0;

IGCConstProp::IGCConstProp(bool enableMathConstProp,
                           bool enableSimplifyGEP) :
    FunctionPass(ID),
    m_enableMathConstProp(enableMathConstProp),
    m_enableSimplifyGEP(enableSimplifyGEP),
    m_TD(nullptr), m_TLI(nullptr)
{
    initializeIGCConstPropPass(*PassRegistry::getPassRegistry());
}

static Constant* GetConstantValue(Type* type, char* rawData)
{
    unsigned int size_in_bytes = type->getPrimitiveSizeInBits() / 8;
    uint64_t returnConstant = 0;
    memcpy_s(&returnConstant, size_in_bytes, rawData, size_in_bytes);
    if(type->isIntegerTy())
    {
        return ConstantInt::get(type, returnConstant);
    }
    else if(type->isFloatingPointTy())
    {
        return  ConstantFP::get(type->getContext(),
            APFloat(type->getFltSemantics(), APInt(type->getPrimitiveSizeInBits(), returnConstant)));
    }
    return nullptr;
}

bool IGCConstProp::EvalConstantAddress(llvm::Value* address, unsigned int & offset)
{
    if(isa<ConstantPointerNull>(address))
    {
        offset = 0;
        return true;
    }
    else if(ConstantExpr *ptrExpr = dyn_cast<ConstantExpr>(address))
    {
        if(ptrExpr->getOpcode() == Instruction::BitCast)
        {
            return EvalConstantAddress(ptrExpr->getOperand(0), offset);
        }
        if(ptrExpr->getOpcode() == Instruction::IntToPtr)
        {
            Value *eltIdxVal = ptrExpr->getOperand(0);
            ConstantInt *eltIdx = dyn_cast<ConstantInt>(eltIdxVal);
            if(!eltIdx)
                return false;
            offset = int_cast<unsigned>(eltIdx->getZExtValue());
            return true;
        }
        else if(ptrExpr->getOpcode() == Instruction::GetElementPtr)
        {
            offset = 0;
            if(!EvalConstantAddress(ptrExpr->getOperand(0), offset))
            {
                return false;
            }
            Type *Ty = ptrExpr->getType();
            gep_type_iterator GTI = gep_type_begin(ptrExpr);
            for(auto OI = ptrExpr->op_begin() + 1, E = ptrExpr->op_end(); OI != E; ++OI, ++GTI) {
                Value *Idx = *OI;
                if(StructType *StTy = GTI.getStructTypeOrNull()) {
                    unsigned Field = int_cast<unsigned>(cast<ConstantInt>(Idx)->getZExtValue());
                    if(Field) {
                        offset += int_cast<unsigned int>(m_TD->getStructLayout(StTy)->getElementOffset(Field));
                    }
                    Ty = StTy->getElementType(Field);
                }
                else {
                    Ty = GTI.getIndexedType();
                    if(const ConstantInt *CI = dyn_cast<ConstantInt>(Idx)) {
                        offset += int_cast<unsigned int>(
                            m_TD->getTypeAllocSize(Ty) * CI->getSExtValue());
                    }
                    else
                    {
                        return false;
                    }
                }
            }
            return true;
        }
    }
    return false;
}

Constant *IGCConstProp::replaceShaderConstant(LoadInst *inst)
{
    unsigned as = inst->getPointerAddressSpace();
    bool directBuf;
    unsigned bufId;
	int size_in_bytes = 0;
    BufferType bufType = IGC::DecodeAS4GFXResource(as, directBuf, bufId);
	ModuleMetaData *modMD = getAnalysis<CodeGenContextWrapper>().getCodeGenContext()->getModuleMetaData();
    if(bufType == CONSTANT_BUFFER && 
        directBuf &&
        modMD && 
        modMD->immConstant.data.size() && 
        bufId == modMD->pushInfo.inlineConstantBufferSlot)
    {
        Value *ptrVal = inst->getPointerOperand();
        unsigned eltId = 0;
        size_in_bytes = inst->getType()->getPrimitiveSizeInBits() / 8;
        if(!EvalConstantAddress(ptrVal, eltId))
        {
            return nullptr;
        }

        if(size_in_bytes)
        {
            char *offset = &(modMD->immConstant.data[0]);
            if(inst->getType()->isVectorTy())
            {
                Type *srcEltTy = inst->getType()->getVectorElementType();
                uint32_t srcNElts = inst->getType()->getVectorNumElements();
                uint32_t eltSize_in_bytes = srcEltTy->getPrimitiveSizeInBits() / 8;
                IRBuilder<> builder(inst);
                Value *vectorValue = UndefValue::get(inst->getType());
                for(uint i = 0; i < srcNElts; i++)
                {
                    vectorValue = builder.CreateInsertElement(
                        vectorValue,
                        GetConstantValue(srcEltTy, offset + eltId + (i*eltSize_in_bytes)),
                        builder.getInt32(i));
                }
                return dyn_cast<Constant>(vectorValue);
            }
            else
            {
                return GetConstantValue(inst->getType(), offset + eltId);
            }
        }
    }
    return nullptr;
}

Constant *IGCConstProp::ConstantFoldCallInstruction(CallInst *inst)
{
    Constant *C = nullptr;
    if (inst)
    {
        llvm::Type * type = inst->getType();
        if (GenIntrinsicInst *genIntr = dyn_cast<GenIntrinsicInst>(inst))
        {
            // used for GenISA_sqrt and GenISA_rsq
            ConstantFP *C0 = dyn_cast<ConstantFP>(inst->getOperand(0));

            // special case of gen-intrinsic
            switch (genIntr->getIntrinsicID())
            {
            case GenISAIntrinsic::GenISA_sqrt:
                if (C0)
                {
                    auto APF = C0->getValueAPF();
                    double C0value = type->isFloatTy() ? APF.convertToFloat() :
                        APF.convertToDouble();
                    if (C0value > 0.0)
                    {
                        C = ConstantFP::get(type, sqrt(C0value));
                    }
                }
                break;
            case GenISAIntrinsic::GenISA_rsq:
                if (C0)
                {
                    auto APF = C0->getValueAPF();
                    double C0value = type->isFloatTy() ? APF.convertToFloat() :
                        APF.convertToDouble();
                    if (C0value > 0.0)
                    {
                        C = ConstantFP::get(type, 1. / sqrt(C0value));
                    }
                }
                break;
            case GenISAIntrinsic::GenISA_max:
                {
                    ConstantFP *CFP0 = dyn_cast<ConstantFP>(inst->getOperand(0));
                    ConstantFP *CFP1 = dyn_cast<ConstantFP>(inst->getOperand(1));
                    if (CFP0 && CFP1)
                    {
                        const APFloat &A = CFP0->getValueAPF();
                        const APFloat &B = CFP1->getValueAPF();
                        C = ConstantFP::get(inst->getContext(), maxnum(A, B));
                    }
                }
                break;
            case GenISAIntrinsic::GenISA_min:
                {
                    ConstantFP *CFP0 = dyn_cast<ConstantFP>(inst->getOperand(0));
                    ConstantFP *CFP1 = dyn_cast<ConstantFP>(inst->getOperand(1));
                    if (CFP0 && CFP1)
                    {
                        const APFloat &A = CFP0->getValueAPF();
                        const APFloat &B = CFP1->getValueAPF();
                        C = ConstantFP::get(inst->getContext(), minnum(A, B));
                    }
                }
                break;
            case GenISAIntrinsic::GenISA_fsat:
            {
                ConstantFP *CFP0 = dyn_cast<ConstantFP>(inst->getOperand(0));
                if(CFP0)
                {
                    const APFloat &A = CFP0->getValueAPF();
                    const APFloat &zero = cast<ConstantFP>(ConstantFP::get(type, 0.))->getValueAPF();
                    const APFloat &One = cast<ConstantFP>(ConstantFP::get(type, 1. ))->getValueAPF();
                    C = ConstantFP::get(inst->getContext(), minnum(One, maxnum(zero, A)));
                }
            }
            default:
                break;
            }
        }
        else if (m_enableMathConstProp && type->isFloatTy())
        {
            float C0value = 0;
            float C1value = 0;
            ConstantFP *C0 = dyn_cast<ConstantFP>(inst->getOperand(0));
            ConstantFP *C1 = nullptr;
            if (C0)
            {
                C0value = C0->getValueAPF().convertToFloat();

                switch (GetOpCode(cast<CallInst>(inst)))
                {
                case llvm_cos:
                    C = ConstantFP::get(type, cosf(C0value));
                    break;
                case llvm_sin:
                    C = ConstantFP::get(type, sinf(C0value));
                    break;
                case llvm_log:
                    // skip floating-point exception and keep the original instructions
                    if (C0value > 0.0f)
                    {
                        C = ConstantFP::get(type, log10f(C0value) / log10f(2.0f));
                    }
                    break;
                case llvm_exp:
                    C = ConstantFP::get(type, powf(2.0f, C0value));
                    break;
                case llvm_pow:
                    C1 = dyn_cast<ConstantFP>(inst->getOperand(1));
                    if (C1)
                    {
                        C1value = C1->getValueAPF().convertToFloat();
                        C = ConstantFP::get(type, powf(C0value, C1value));
                    }
                    break;
                case llvm_sqrt:
                    // Don't handle negative values
                    if (C0value > 0.0f)
                    {
                        C = ConstantFP::get(type, sqrtf(C0value));
                    }
                    break;
                case llvm_floor:
                    C = ConstantFP::get(type, floorf(C0value));
                    break;
                case llvm_ceil:
                    C = ConstantFP::get(type, ceilf(C0value));
                    break;
                default:
                    break;
                }
            }
        }
    }
    return C;
}

// constant fold the following code for any index:
//
// %95 = extractelement <4 x i16> <i16 3, i16 16, i16 21, i16 39>, i32 %94
// %96 = icmp eq i16 %95, 0
//
Constant *IGCConstProp::ConstantFoldCmpInst(CmpInst *CI)
{
    // Only handle scalar result.
    if (CI->getType()->isVectorTy())
        return nullptr;

    Value *LHS = CI->getOperand(0);
    Value *RHS = CI->getOperand(1);
    if (!isa<Constant>(RHS) && CI->isCommutative())
        std::swap(LHS, RHS);
    if (!isa<ConstantInt>(RHS) && !isa<ConstantFP>(RHS))
        return nullptr;

    auto EEI = dyn_cast<ExtractElementInst>(LHS);
    if (EEI && isa<Constant>(EEI->getVectorOperand()))
    {
        bool AllTrue = true, AllFalse = true;
        auto VecOpnd = cast<Constant>(EEI->getVectorOperand());
        unsigned N = VecOpnd->getType()->getVectorNumElements();
        for (unsigned i = 0; i < N; ++i)
        {
            Constant *Opnd = VecOpnd->getAggregateElement(i);
            assert(Opnd && "null entry");
            if (isa<UndefValue>(Opnd))
                continue;
            Constant *Result = ConstantFoldCompareInstOperands(
                CI->getPredicate(), Opnd, cast<Constant>(RHS), CI->getFunction()->getParent()->getDataLayout());
            if (!Result->isAllOnesValue())
                AllTrue = false;
            if (!Result->isNullValue())
                AllFalse = false;
        }

        if (AllTrue)
        {
            return ConstantInt::getTrue(CI->getType());
        }
        else if (AllFalse)
        {
            return ConstantInt::getFalse(CI->getType());
        }
    }

    return nullptr;
}

// constant fold the following code for any index:
//
// %93 = insertelement  <4 x float> <float 1.0, float 1.0, float 1.0, float 1.0>, float %v7.w_, i32 0
// %95 = extractelement <4 x float> %93, i32 1
//
// constant fold the selection of the same value in a vector component, e.g.:
// %Temp - 119.i.i.v.v = select i1 %Temp - 118.i.i, <2 x i32> <i32 0, i32 17>, <2 x i32> <i32 4, i32 17>
// %scalar9 = extractelement <2 x i32> %Temp - 119.i.i.v.v, i32 1
//
Constant *IGCConstProp::ConstantFoldExtractElement(ExtractElementInst *EEI)
{

    Constant *EltIdx = dyn_cast<Constant>(EEI->getIndexOperand());
    if (EltIdx)
    {
        if (InsertElementInst *IEI = dyn_cast<InsertElementInst>(EEI->getVectorOperand()))
    {
        Constant *InsertIdx = dyn_cast<Constant>(IEI->getOperand(2));
            // try to find the constant from a chain of InsertElement
        while(IEI && InsertIdx)
        {
            if (InsertIdx == EltIdx)
            {
                Constant *EltVal = dyn_cast<Constant>(IEI->getOperand(1));
                return EltVal;
            }
            else
            {
                Value *Vec = IEI->getOperand(0);
                if (isa<ConstantDataVector>(Vec))
                {
                    ConstantDataVector *CVec = cast<ConstantDataVector>(Vec);
                    return CVec->getAggregateElement(EltIdx);
                }
                else if (isa<InsertElementInst>(Vec))
                {
                    IEI = cast<InsertElementInst>(Vec);
                    InsertIdx = dyn_cast<Constant>(IEI->getOperand(2));
                }
                else
                {
                    break;
                }
            }
        }
    }
        else if (SelectInst *sel = dyn_cast<SelectInst>(EEI->getVectorOperand()))
        {
            Value *vec0 = sel->getOperand(1);
            Value *vec1 = sel->getOperand(2);

            assert(vec0->getType() == vec1->getType());

            if (isa<ConstantDataVector>(vec0) && isa<ConstantDataVector>(vec1))
            {
                ConstantDataVector *cvec0 = cast<ConstantDataVector>(vec0);
                ConstantDataVector *cvec1 = cast<ConstantDataVector>(vec1);
                Constant* cval0 = cvec0->getAggregateElement(EltIdx);
                Constant* cval1 = cvec1->getAggregateElement(EltIdx);
                
                if (cval0 == cval1)
                {
                    return cval0;
                }
            }
        }
    }
    return nullptr;
}

//  simplifyAdd() push any constants to the top of a sequence of Add instructions,
//  which makes CSE/GVN to do a better job.  For example,
//      (((A + 8) + B) + C) + 15
//  will become
//      ((A + B) + C) + 23
//  Note that the order of non-constant values remain unchanged throughout this
//  transformation.
//  (This was added to remove redundant loads. If the
//  the future LLVM does better job on this (reassociation), we should use LLVM's
//  instead.)
bool IGCConstProp::simplifyAdd(BinaryOperator *BO)
{
    // Only handle Add
    if (BO->getOpcode() != Instruction::Add)
    {
        return false;
    }

    Value *LHS = BO->getOperand(0);
    Value *RHS = BO->getOperand(1);
    bool changed = false;
    if (BinaryOperator *LBO = dyn_cast<BinaryOperator>(LHS))
    {
        if (simplifyAdd(LBO))
        {
            changed = true;
        }
    }
    if (BinaryOperator *RBO = dyn_cast<BinaryOperator>(RHS))
    {
        if (simplifyAdd(RBO))
        {
            changed = true;
        }
    }

    // Refresh LHS and RHS
    LHS = BO->getOperand(0);
    RHS = BO->getOperand(1);
    BinaryOperator *LHSbo = dyn_cast<BinaryOperator>(LHS);
    BinaryOperator *RHSbo = dyn_cast<BinaryOperator>(RHS);
    bool isLHSAdd = LHSbo && LHSbo->getOpcode() == Instruction::Add;
    bool isRHSAdd = RHSbo && RHSbo->getOpcode() == Instruction::Add;
    IRBuilder<> Builder(BO);
    if (isLHSAdd && isRHSAdd)
    {
        Value *A = LHSbo->getOperand(0);
        Value *B = LHSbo->getOperand(1);
        Value *C = RHSbo->getOperand(0);
        Value *D = RHSbo->getOperand(1);

        ConstantInt *C0 = dyn_cast<ConstantInt>(B);
        ConstantInt *C1 = dyn_cast<ConstantInt>(D);

        if (C0 || C1)
        {
            Value *R = nullptr;
            if (C0 && C1)
            {
                Value *newC = ConstantFoldBinaryOpOperands(Instruction::Add,
                    C0, C1, *m_TD);
                R = Builder.CreateAdd(A, C);
                R = Builder.CreateAdd(R, newC);
            }
            else if (C0)
            {
                R = Builder.CreateAdd(A, RHS);
                R = Builder.CreateAdd(R, B);
            }
            else
            {   // C1 is not nullptr
                R = Builder.CreateAdd(LHS, C);
                R = Builder.CreateAdd(R, D);
            }
            BO->replaceAllUsesWith(R);
            return true;
        }
    }
    else if (isLHSAdd)
    {
        Value *A = LHSbo->getOperand(0);
        Value *B = LHSbo->getOperand(1);
        Value *C = RHS;

        ConstantInt *C0 = dyn_cast<ConstantInt>(B);
        ConstantInt *C1 = dyn_cast<ConstantInt>(C);

        if (C0 && C1)
        {
            Value *newC = ConstantFoldBinaryOpOperands(Instruction::Add,
                C0, C1, *m_TD);
            Value *R = Builder.CreateAdd(A, newC);
            BO->replaceAllUsesWith(R);
            return true;
        }
        if (C0)
        {
            Value *R = Builder.CreateAdd(A, C);
            R = Builder.CreateAdd(R, B);
            BO->replaceAllUsesWith(R);
            return true;
        }
    }
    else if (isRHSAdd)
    {
        Value *A = LHS;
        Value *B = RHSbo->getOperand(0);
        Value *C = RHSbo->getOperand(1);

        ConstantInt *C0 = dyn_cast<ConstantInt>(A);
        ConstantInt *C1 = dyn_cast<ConstantInt>(C);

        if (C0 && C1)
        {
            Constant *Ops[] = { C0, C1 };
            Value *newC = ConstantFoldBinaryOpOperands(Instruction::Add,
                C0, C1, *m_TD);
            Value *R = Builder.CreateAdd(B, newC);
            BO->replaceAllUsesWith(R);
            return true;
        }
        if (C0)
        {
            Value *R = Builder.CreateAdd(RHS, A);
            BO->replaceAllUsesWith(R);
            return true;
        }
        if (C1)
        {
            Value *R = Builder.CreateAdd(A, B);
            R = Builder.CreateAdd(R, C);
            BO->replaceAllUsesWith(R);
            return true;
        }
    }
    else
    {
        if (ConstantInt *CLHS = dyn_cast<ConstantInt>(LHS))
        {
            if (ConstantInt *CRHS = dyn_cast<ConstantInt>(RHS))
            {
                Constant *Ops[] = { CLHS, CRHS };
                Value *newC = ConstantFoldBinaryOpOperands(Instruction::Add,
                    CLHS, CRHS, *m_TD);
                BO->replaceAllUsesWith(newC);
                return true;
            }

            // Constant is kept as RHS
            Value *R = Builder.CreateAdd(RHS, LHS);
            BO->replaceAllUsesWith(R);
            return true;
        }
    }
    return changed;
}

bool IGCConstProp::simplifyGEP(GetElementPtrInst *GEP)
{
    bool changed = false;
    for (int i = 0; i < (int)GEP->getNumIndices(); ++i)
    {
        Value *Index = GEP->getOperand(i + 1);
        BinaryOperator *BO = dyn_cast<BinaryOperator>(Index);
        if (!BO || BO->getOpcode() != Instruction::Add)
        {
            continue;
        }
        if (simplifyAdd(BO))
        {
            changed = true;
        }
    }
    return changed;
}

/**
* the following code is essentially a copy of llvm copy-prop code with one little
* addition for shader-constant replacement.
*
* we don't have to do this if llvm version uses a virtual function in place of calling
* ConstantFoldInstruction.
*/
bool IGCConstProp::runOnFunction(Function &F)
{
    module = F.getParent();
    // Initialize the worklist to all of the instructions ready to process...
    llvm::SetVector<Instruction*> WorkList;
    for (inst_iterator i = inst_begin(F), e = inst_end(F); i != e; ++i)
    {
        WorkList.insert(&*i);
    }
    bool Changed = false;
    m_TD = &F.getParent()->getDataLayout();
    m_TLI = &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
    while (!WorkList.empty())
    {
        Instruction *I = *WorkList.rbegin();
        WorkList.remove(I);    // Get an element from the worklist...
        if (I->use_empty())                  // Don't muck with dead instructions...
        {
            continue;
        }
        Constant *C = nullptr;
        C = ConstantFoldInstruction(I, *m_TD, m_TLI);

        if (!C && isa<CallInst>(I))
        {
            C = ConstantFoldCallInstruction(cast<CallInst>(I));
        }

        // replace shader-constant load with the known value
        if (!C && isa<LoadInst>(I))
        {
            C = replaceShaderConstant(cast<LoadInst>(I));
        }
        if (!C && isa<CmpInst>(I))
        {
            C = ConstantFoldCmpInst(cast<CmpInst>(I));
        }
        if (!C && isa<ExtractElementInst>(I))
        {
            C = ConstantFoldExtractElement(cast<ExtractElementInst>(I));
        }
        if (C)
        {
            // Add all of the users of this instruction to the worklist, they might
            // be constant propagatable now...
            for (Value::user_iterator UI = I->user_begin(), UE = I->user_end();
                UI != UE; ++UI)
            {
                WorkList.insert(cast<Instruction>(*UI));
            }

            // Replace all of the uses of a variable with uses of the constant.
            I->replaceAllUsesWith(C);

			if ( 0 /* isa<ConstantPointerNull>(C)*/) // disable optimization generating invalid IR until it gets re-written
			{
				// if we are changing function calls/ genisa intrinsics, then we need 
				// to fix the function declarations to account for the change in pointer address type
				for (Value::user_iterator UI = C->user_begin(), UE = C->user_end();
					UI != UE; ++UI)
				{
					if (GenIntrinsicInst *genIntr = dyn_cast<GenIntrinsicInst>(*UI))
					{
                                    GenISAIntrinsic::ID ID = genIntr->getIntrinsicID();
						if (ID == GenISAIntrinsic::GenISA_storerawvector_indexed)
						{
							llvm::Type* tys[2];
							tys[0] = genIntr->getOperand(0)->getType();
							tys[1] = genIntr->getOperand(2)->getType();
							GenISAIntrinsic::getDeclaration(F.getParent(),
								llvm::GenISAIntrinsic::GenISA_storerawvector_indexed,
								tys);
						}
						else if (ID == GenISAIntrinsic::GenISA_storeraw_indexed)
						{
                            llvm::Type* types[2] = {
                                genIntr->getOperand(0)->getType(),
                                genIntr->getOperand(1)->getType() };

							GenISAIntrinsic::getDeclaration(F.getParent(),
								llvm::GenISAIntrinsic::GenISA_storeraw_indexed,
                                types);
						}
						else if (ID == GenISAIntrinsic::GenISA_ldrawvector_indexed)
						{
							llvm::Type* tys[2];
							tys[0] = genIntr->getType();
							tys[1] = genIntr->getOperand(0)->getType();
							GenISAIntrinsic::getDeclaration(F.getParent(),
								llvm::GenISAIntrinsic::GenISA_ldrawvector_indexed,
								tys);
						}
						else if (ID == GenISAIntrinsic::GenISA_ldraw_indexed)
						{
							GenISAIntrinsic::getDeclaration(F.getParent(),
								llvm::GenISAIntrinsic::GenISA_ldraw_indexed,
								genIntr->getOperand(0)->getType());
						}
					}
				}
			}

            // Remove the dead instruction.
            I->eraseFromParent();

            // We made a change to the function...
            Changed = true;

            // I is erased, continue to the next one.
            continue;
        }

		if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(I))
		{
            if (m_enableSimplifyGEP && simplifyGEP(GEP))
            {
                Changed = true;
            }
        }
    }
    return Changed;
}


namespace {

	class IGCIndirectICBPropagaion : public FunctionPass
	{
	public:
		static char ID;
		IGCIndirectICBPropagaion() : FunctionPass(ID)
		{
			initializeIGCIndirectICBPropagaionPass(*PassRegistry::getPassRegistry());
		}
		virtual llvm::StringRef getPassName() const { return "Indirect ICB Propagaion"; }
		virtual bool runOnFunction(Function &F);
		virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const
		{
			AU.setPreservesCFG();
			AU.addRequired<CodeGenContextWrapper>();
		}
	};

} // namespace

char IGCIndirectICBPropagaion::ID = 0;
FunctionPass *IGC::createIGCIndirectICBPropagaionPass() { return new IGCIndirectICBPropagaion(); }

bool IGCIndirectICBPropagaion::runOnFunction(Function &F)
{
    CodeGenContext *ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
    ModuleMetaData *modMD = ctx->getModuleMetaData();

    //MaxImmConstantSizePushed = 256 by default. For float values, it will contains 64 numbers, and stored in 8 GRF
    if (modMD && 
        modMD->immConstant.data.size() &&
        modMD->immConstant.data.size() <= IGC_GET_FLAG_VALUE(MaxImmConstantSizePushed))
    {
        uint maxImmConstantSizePushed = modMD->immConstant.data.size();
        char *offset = &(modMD->immConstant.data[0]);
        IRBuilder<> m_builder(F.getContext());

        for (auto &BB : F)
        {
            for (auto BI = BB.begin(), BE = BB.end(); BI != BE;)
            {
                if (llvm::LoadInst* inst = llvm::dyn_cast<llvm::LoadInst>(&(*BI++)))
                {
                    unsigned as = inst->getPointerAddressSpace();
                    bool directBuf;
                    unsigned bufId;
                    BufferType bufType = IGC::DecodeAS4GFXResource(as, directBuf, bufId);
                    if (bufType == CONSTANT_BUFFER && directBuf && bufId == modMD->pushInfo.inlineConstantBufferSlot)
                    {
                        Value *ptrVal = inst->getPointerOperand();
                        ConstantExpr *ptrExpr = dyn_cast<ConstantExpr>(ptrVal);
                        IntToPtrInst *i2p = dyn_cast<IntToPtrInst>(ptrVal);
                        if (ptrExpr == nullptr && i2p)
                        {
                            m_builder.SetInsertPoint(inst);

                            int size_in_bytes = inst->getType()->getPrimitiveSizeInBits() / 8;
                            if (size_in_bytes)
                            {
                                Value* ICBbuffer = UndefValue::get(VectorType::get(inst->getType(), maxImmConstantSizePushed / size_in_bytes));
                                if (inst->getType()->isFloatingPointTy())
                                {
                                    float returnConstant = 0;
                                    for (unsigned int i = 0; i < maxImmConstantSizePushed; i += size_in_bytes)
                                    {
                                        memcpy_s(&returnConstant, size_in_bytes, offset + i, size_in_bytes);
                                        Value *fp = ConstantFP::get(inst->getType(), returnConstant);
                                        ICBbuffer = m_builder.CreateInsertElement(ICBbuffer, fp, m_builder.getInt32(i / size_in_bytes));
                                    }
                                    Value *eltIdxVal = i2p->getOperand(0);
                                    Value *div = m_builder.CreateLShr(eltIdxVal, m_builder.getInt32(2));
                                    Value *ICBvalue = m_builder.CreateExtractElement(ICBbuffer, div);
                                    inst->replaceAllUsesWith(ICBvalue);
                                }
                                else if (inst->getType()->isIntegerTy())
                                {
                                    int returnConstant = 0;
                                    for (unsigned int i = 0; i < maxImmConstantSizePushed; i += size_in_bytes)
                                    {
                                        memcpy_s(&returnConstant, size_in_bytes, offset + i, size_in_bytes);
                                        Value *fp = ConstantInt::get(inst->getType(), returnConstant);
                                        ICBbuffer = m_builder.CreateInsertElement(ICBbuffer, fp, m_builder.getInt32(i / size_in_bytes));
                                    }
                                    Value *eltIdxVal = i2p->getOperand(0);
                                    Value *div = m_builder.CreateLShr(eltIdxVal, m_builder.getInt32(2));
                                    Value *ICBvalue = m_builder.CreateExtractElement(ICBbuffer, div);
                                    inst->replaceAllUsesWith(ICBvalue);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

	return false;
}

IGC_INITIALIZE_PASS_BEGIN(IGCIndirectICBPropagaion, "IGCIndirectICBPropagaion",
    "IGCIndirectICBPropagaion", false, false)
IGC_INITIALIZE_PASS_END(IGCIndirectICBPropagaion, "IGCIndirectICBPropagaion",
    "IGCIndirectICBPropagaion", false, false)


char CustomLoopInfo::ID = 0;

CustomLoopInfo::CustomLoopInfo() : LoopPass(ID)
{
    initializeLoopInfoWrapperPassPass(*PassRegistry::getPassRegistry());
}

bool CustomLoopInfo::runOnLoop(Loop *L, LPPassManager &LPM)
{
    LoopInfo *LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    auto pCtx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
    if (!LI->empty())
    {
        llvm::BasicBlock::InstListType::iterator I;
        for (uint i = 0; i < L->getNumBlocks(); i++)
        {
            llvm::BasicBlock::InstListType &instructionList = L->getBlocks()[i]->getInstList();
            for (I = instructionList.begin(); I != instructionList.end(); I++)
            {
                if (llvm::GenIntrinsicInst *CI = dyn_cast<llvm::GenIntrinsicInst>(&(*I)))
                {
                    if (CI->getIntrinsicID() == GenISAIntrinsic::GenISA_sampleL)
                    {
                        pCtx->m_instrTypes.hasSampleLinLoop = true;
                        return false;
                    }
                }
            }
        }
    }

    return false;
}

namespace {

class GenStrengthReduction : public FunctionPass
{
public:
    static char ID;
    GenStrengthReduction() : FunctionPass(ID)
    {
        initializeGenStrengthReductionPass(*PassRegistry::getPassRegistry());
    }
    virtual llvm::StringRef getPassName() const { return "Gen strength reduction"; }
    virtual bool runOnFunction(Function &F);

private:
    bool processInst(Instruction *Inst);
    // Transform (extract-element (bitcast %vector) ...) to
    // (bitcast (extract-element %vector) ...) in order to help coalescing in DeSSA.
    bool optimizeVectorBitCast(Function &F) const;
};

} // namespace

char GenStrengthReduction::ID = 0;
FunctionPass *IGC::createGenStrengthReductionPass() { return new GenStrengthReduction(); }

bool GenStrengthReduction::runOnFunction(Function &F)
{
    bool Changed = false;
    for (auto &BB : F)
    {
        for (auto BI = BB.begin(), BE = BB.end(); BI != BE;)
        {
            Instruction *Inst = &(*BI++);
            if (isInstructionTriviallyDead(Inst))
            {
                Inst->eraseFromParent();
                Changed = true;
                continue;
            }
            Changed |= processInst(Inst);
        }
    }

    Changed |= optimizeVectorBitCast(F);

    return Changed;
}

// Check if this is a fdiv that allows reciprocal, and its divident is not known
// to be 1.0.
static bool isCandidateFDiv(Instruction *Inst)
{
    // Only floating points, and no vectors.
    if (!Inst->getType()->isFloatingPointTy() || Inst->use_empty())
        return false;

    auto Op = dyn_cast<FPMathOperator>(Inst);
    if (Op && Op->getOpcode() == Instruction::FDiv && Op->hasAllowReciprocal())
    {
        Value *Src0 = Op->getOperand(0);
        if (auto CFP = dyn_cast<ConstantFP>(Src0))
            return !CFP->isExactlyValue(1.0);
        return true;
    }
    return false;
}

bool GenStrengthReduction::processInst(Instruction *Inst)
{

    unsigned opc = Inst->getOpcode();
    auto Op = dyn_cast<FPMathOperator>(Inst);
    if (opc == Instruction::Select)
    {
        Value *oprd1 = Inst->getOperand(1);
        Value *oprd2 = Inst->getOperand(2);
        ConstantFP *CF1 = dyn_cast<ConstantFP>(oprd1);
        ConstantFP *CF2 = dyn_cast<ConstantFP>(oprd2);
        if (oprd1 == oprd2 ||
            (CF1 && CF2 && CF1->isExactlyValue(CF2->getValueAPF())))
        {
            Inst->replaceAllUsesWith(oprd1);
            Inst->eraseFromParent();
            return true;
        }
    }
    if (Op && 
        Op->hasNoNaNs() && 
        Op->hasNoInfs() &&
        Op->hasNoSignedZeros())
    {
        switch (opc)
        {
        case Instruction::FDiv :
          {
            Value *Oprd0 = Inst->getOperand(0);
            if (ConstantFP *CF = dyn_cast<ConstantFP>(Oprd0))
            {
                if (CF->isZero())
                {
                    Inst->replaceAllUsesWith(Oprd0);
                    Inst->eraseFromParent();
                    return true;
                }
            }
            break;
          }
        case Instruction::FMul :
          {
            for (int i=0; i < 2; ++i)
            {
                ConstantFP  *CF = dyn_cast<ConstantFP>(Inst->getOperand(i));
                if (CF && CF->isZero())
                {
                    Inst->replaceAllUsesWith(CF);
                    Inst->eraseFromParent();
                    return true;
                }
            }
            break;
          }
        case Instruction::FAdd :
          {
            for (int i = 0; i < 2; ++i)
            {
                ConstantFP  *CF = dyn_cast<ConstantFP>(Inst->getOperand(i));
                if (CF && CF->isZero())
                {
                    Value *otherOprd = Inst->getOperand(1-i);
                    Inst->replaceAllUsesWith(otherOprd);
                    Inst->eraseFromParent();
                    return true;
                }
            }
            break;
          }
        }
    }

    // fdiv -> inv + mul. On gen, fdiv seems always slower
    // than inv + mul. Do it if fdiv's fastMathFlag allows it.
    //
    // Rewrite
    // %1 = fdiv arcp float %x, %z
    // into
    // %1 = fdiv arcp float 1.0, %z
    // %2 = fmul arcp float %x, %1
    if (isCandidateFDiv(Inst))
    {
        Value *Src1 = Inst->getOperand(1);
        if (isa<Constant>(Src1))
        {
            // should not happen (but do see "fdiv  x / 0.0f"). Skip.
            return false;
        }

        Value *Src0 = ConstantFP::get(Inst->getType(), 1.0);
        Instruction *Inv = nullptr;

        // Check if there is any other (x / Src1). If so, commonize 1/Src1.
        for (auto UI = Src1->user_begin(), UE = Src1->user_end();
             UI != UE; ++UI)
        {
            Value *Val = *UI;
            Instruction *I = dyn_cast<Instruction>(Val);
            if (I && I != Inst && I->getOpcode() == Instruction::FDiv &&
                I->getOperand(1) == Src1 && isCandidateFDiv(I))
            {
                // special case
                if (ConstantFP *CF = dyn_cast<ConstantFP>(I->getOperand(0)))
                {
                    if (CF->isZero())
                    {
                        // Skip this one.
                        continue;
                    }
                }

                // Found another 1/Src1. Insert Inv right after the def of Src1
                // or in the entry BB if Src1 is an argument.
                if (!Inv)
                {
                    Instruction *insertBefore = dyn_cast<Instruction>(Src1);
                    if (insertBefore)
                    {
                        if (isa<PHINode>(insertBefore))
                        {
                            BasicBlock *BB = insertBefore->getParent();
                            insertBefore = &(*BB->getFirstInsertionPt());
                        }
                        else
                        {
                            // Src1 is an instruction
                            BasicBlock::iterator iter(insertBefore);
                            ++iter;
                            insertBefore = &(*iter);
                        }
                    }
                    else
                    {
                        // Src1 is an argument and insert at the begin of entry BB
                        BasicBlock &entryBB = Inst->getParent()->getParent()->getEntryBlock();
                        insertBefore = &(*entryBB.getFirstInsertionPt());
                    }
                    Inv = BinaryOperator::CreateFDiv(Src0, Src1, "", insertBefore);
                    Inv->setFastMathFlags(Inst->getFastMathFlags());
                }

                Instruction *Mul = BinaryOperator::CreateFMul(I->getOperand(0), Inv, "", I);
                Mul->setFastMathFlags(Inst->getFastMathFlags());
                I->replaceAllUsesWith(Mul);
                // Don't erase it as doing so would invalidate iterator in this func's caller
                // Instead, erase it in the caller.
                // I->eraseFromParent();
            }
        }

        if (!Inv)
        {
            // Only a single use of 1 / Src1. Create Inv right before the use.
            Inv = BinaryOperator::CreateFDiv(Src0, Src1, "", Inst);
            Inv->setFastMathFlags(Inst->getFastMathFlags());
        }

        auto Mul = BinaryOperator::CreateFMul(Inst->getOperand(0), Inv, "", Inst);
        Mul->setFastMathFlags(Inst->getFastMathFlags());
        Inst->replaceAllUsesWith(Mul);
        Inst->eraseFromParent();
        return true;
    }

    return false;
}

bool GenStrengthReduction::optimizeVectorBitCast(Function &F) const {
    IRBuilder<> Builder(F.getContext());

    bool Changed = false;
    for (auto &BB : F) {
        for (auto BI = BB.begin(), BE = BB.end(); BI != BE; /*EMPTY*/) {
            BitCastInst *BC = dyn_cast<BitCastInst>(&*BI++);
            if (!BC) continue;
            // Skip non-element-wise bitcast.
            VectorType *DstVTy = dyn_cast<VectorType>(BC->getType());
            VectorType *SrcVTy = dyn_cast<VectorType>(BC->getOperand(0)->getType());
            if (!DstVTy || !SrcVTy || DstVTy->getNumElements() != SrcVTy->getNumElements())
                continue;
            // Skip if it's not used only all extract-element.
            bool ExactOnly = true;
            for (auto User : BC->users()) {
                if (auto EEI = dyn_cast<ExtractElementInst>(User)) continue;
                ExactOnly = false;
                break;
            }
            if (!ExactOnly)
                continue;
            // Autobots, transform and roll out!
            Value *Src = BC->getOperand(0);
            Type *DstEltTy = DstVTy->getElementType();
            for (auto UI = BC->user_begin(), UE = BC->user_end(); UI != UE;
                 /*EMPTY*/) {
                auto EEI = cast<ExtractElementInst>(*UI++);
                Builder.SetInsertPoint(EEI);
                auto NewVal = Builder.CreateExtractElement(Src, EEI->getIndexOperand());
                NewVal = Builder.CreateBitCast(NewVal, DstEltTy);
                EEI->replaceAllUsesWith(NewVal);
                EEI->eraseFromParent();
            }
            BI = BC->eraseFromParent();
            Changed = true;
        }
    }

    return Changed;
}

IGC_INITIALIZE_PASS_BEGIN(GenStrengthReduction, "GenStrengthReduction",
                          "GenStrengthReduction", false, false)
IGC_INITIALIZE_PASS_END(GenStrengthReduction, "GenStrengthReduction",
                        "GenStrengthReduction", false, false)


/*========================== FlattenSmallSwitch ==============================

This class flatten small switch. For example,

before optimization:
    switch i32 %115, label %else229 [
    i32 1, label %then214
    i32 2, label %then222
    ]

    then214:                                          ; preds = %then153
    %150 = fdiv float 1.000000e+00, %res_s208
    %151 = fmul float %147, %150
    br label %ifcont237

    then222:                                          ; preds = %then153
    %152 = fsub float 1.000000e+00, %141
    br label %ifcont237

    else229:                                          ; preds = %then153
    %res_s230 = icmp eq i32 %115, 3
    %. = select i1 %res_s230, float 1.000000e+00, float 0.000000e+00
    br label %ifcont237

    ifcont237:                                        ; preds = %else229, %then222, %then214
    %"r[9][0].x.0" = phi float [ %151, %then214 ], [ %152, %then222 ], [ %., %else229 ]

after optimization:
    %res_s230 = icmp eq i32 %115, 3
    %. = select i1 %res_s230, float 1.000000e+00, float 0.000000e+00
    %150 = fdiv float 1.000000e+00, %res_s208
    %151 = fmul float %147, %150
    %152 = icmp eq i32 %115, 1
    %153 = select i1 %152, float %151, float %.
    %154 = fsub float 1.000000e+00, %141
    %155 = icmp eq i32 %115, 2
    %156 = select i1 %155, float %154, float %153

=============================================================================*/
namespace {
class FlattenSmallSwitch : public FunctionPass
{
public:
    static char ID;
    FlattenSmallSwitch() : FunctionPass(ID)
    {
        initializeFlattenSmallSwitchPass(*PassRegistry::getPassRegistry());
    }
    virtual llvm::StringRef getPassName() const { return "Flatten Small Switch"; }
    virtual bool runOnFunction(Function &F);
    bool processSwitchInst(SwitchInst *SI);
};

} // namespace

char FlattenSmallSwitch::ID = 0;
FunctionPass *IGC::createFlattenSmallSwitchPass() { return new FlattenSmallSwitch(); }

bool FlattenSmallSwitch::processSwitchInst(SwitchInst *SI) 
{
    const unsigned maxSwitchCases = 3;  // only apply to switch with 3 cases or less
    const unsigned maxCaseInsts = 3;    // only apply optimization when each case has 3 instructions or less.

    BasicBlock* Default = SI->getDefaultDest();
    Value *Val = SI->getCondition();  // The value we are switching on...
    IRBuilder<> builder(SI);

    if (SI->getNumCases() > maxSwitchCases || SI->getNumCases() == 0)
    {
        return false;
    }

	// Dest will be the block that the control flow from the switch merges to.
	// Currently, there are two options:
	// 1. The Dest block is the default block from the switch
	// 2. The Dest block is jumped to by all of the switch cases (and the default)
	BasicBlock *Dest = nullptr;
	{
		const auto *CaseSucc = SI->case_begin().getCaseSuccessor();
		auto *BI = dyn_cast<BranchInst>(CaseSucc->getTerminator());

		if (BI == nullptr)
			return false;

		if (BI->isConditional())
			return false;

		// We know the first case jumps to this block.  Now let's
		// see below whether all the cases jump to this same block.
		Dest = BI->getSuccessor(0);
	}

	// Does BB unconditionally branch to MergeBlock?
	auto branchPattern = [](const BasicBlock *BB, const BasicBlock *MergeBlock)
	{
		auto *br = dyn_cast<BranchInst>(BB->getTerminator());

		if (br == nullptr)
			return false;

		if (br->isConditional())
			return false;

		if (br->getSuccessor(0) != MergeBlock)
			return false;

		return true;
	};

	// We can speculatively execute a basic block if it
	// is small, unconditionally branches to Dest, and doesn't
	// have high latency or unsafe to speculate instructions.
	auto canSpeculateBlock = [&](BasicBlock *BB)
	{
		if (BB->size() > maxCaseInsts)
			return false;

		if (!branchPattern(BB, Dest))
			return false;

		for (auto &I : *BB)
		{
			auto *inst = &I;

			if (isa<BranchInst>(inst))
				continue;

			// if there is any high-latency instruction in the switch,
			// don't flatten it
			if (isSampleInstruction(inst)  ||
				isGather4Instruction(inst) ||
				isInfoInstruction(inst)    ||
				isLdInstruction(inst)      ||
				// If the instruction can't be speculated (e.g., phi node),
				// punt.
				!isSafeToSpeculativelyExecute(inst))
			{
				return false;
			}
		}

		return true;
	};

	for (auto &I : SI->cases())
	{
		BasicBlock *CaseDest = I.getCaseSuccessor();

		if (!canSpeculateBlock(CaseDest))
			return false;
	}

	// Is the default case of the switch the block
	// where all other cases meet?
	const bool DefaultMergeBlock = (Dest == Default);

	// If we merge to the default block, there is no block
	// we jump to beforehand so there is nothing to
	// speculate.
	if (!DefaultMergeBlock && !canSpeculateBlock(Default))
		return false;

    // Get all PHI nodes that needs to be replaced
    SmallVector<PHINode*, 4> PhiNodes;
    for (auto &I : *Dest)
    {
        auto *Phi = dyn_cast<PHINode>(&I);

        if (!Phi)
            break;

        if (Phi->getNumIncomingValues() != SI->getNumCases() + 1)
            return false;

        PhiNodes.push_back(Phi);
    }

    if (PhiNodes.empty())
        return false;

	// Move all instructions except the last (i.e., the branch)
	// from BB to the InsertPoint.
	auto splice = [](BasicBlock *BB, Instruction *InsertPoint)
	{
		Instruction* preIter = nullptr;
		for (auto &iter : *BB)
		{
			if (preIter)
			{
				preIter->moveBefore(InsertPoint);
			}
			preIter = cast<Instruction>(&iter);
		}
	};

    // move default block out
	if (!DefaultMergeBlock)
		splice(Default, SI);

    // move case blocks out
	for (auto &I : SI->cases())
    {
        BasicBlock *CaseDest = I.getCaseSuccessor();
		splice(CaseDest, SI);
    }

    // replaces PHI with select
    for (auto *Phi : PhiNodes)
    {
        Value *vTemp = Phi->getIncomingValueForBlock(
            DefaultMergeBlock ? SI->getParent() : Default);

        for (auto &I : SI->cases())
        {
            BasicBlock *CaseDest   = I.getCaseSuccessor();
            ConstantInt *CaseValue = I.getCaseValue();

            Value *selTrueValue = Phi->getIncomingValueForBlock(CaseDest);
            builder.SetInsertPoint(SI);
            Value *cmp = builder.CreateICmp(CmpInst::Predicate::ICMP_EQ, Val, CaseValue);
            Value *sel = builder.CreateSelect(cmp, selTrueValue, vTemp);
            vTemp = sel;
        }

        Phi->replaceAllUsesWith(vTemp);
    }

    // connect the original block and the phi node block with a pass through branch
    builder.CreateBr(Dest);

    // Remove the switch.
    BasicBlock *SelectBB = SI->getParent();
    for (unsigned i = 0, e = SI->getNumSuccessors(); i < e; ++i) 
    {
        BasicBlock *Succ = SI->getSuccessor(i);
        if (Succ == Dest)
        {
            continue;
        }
        Succ->removePredecessor(SelectBB);
    }
    SI->eraseFromParent();

    return true;
}

bool FlattenSmallSwitch::runOnFunction(Function &F)
{
    bool Changed = false;
    for (Function::iterator I = F.begin(), E = F.end(); I != E; ) 
    {
        BasicBlock *Cur = &*I++; // Advance over block so we don't traverse new blocks
        if (SwitchInst *SI = dyn_cast<SwitchInst>(Cur->getTerminator()))
        {
            Changed |= processSwitchInst(SI);
        }
    }
    return Changed;
}

IGC_INITIALIZE_PASS_BEGIN(FlattenSmallSwitch, "flattenSmallSwitch", "flattenSmallSwitch", false, false)
IGC_INITIALIZE_PASS_END(FlattenSmallSwitch, "flattenSmallSwitch", "flattenSmallSwitch", false, false)


