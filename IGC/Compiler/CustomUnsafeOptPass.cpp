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

/*========================== CustomUnsafeOptPass.cpp ==========================

 This file contains CustomUnsafeOptPass and EarlyOutPatterns
 
 CustomUnsafeOptPass does peephole optimizations which might affect precision.
 This pass combines things like:
    x * 0 = 0               or
    y + x - x = y           or
    fdiv = fmul + inv       or
    fmul+fsub+fcmp = fcmp (if condition allowed)


 EarlyOutPatterns does a few early out cases that adds control flow to
 avoid heavy computation that is not needed. 
 For example, if a long/expensive sequence of instruction result is 0 when 
 one of the input r0 is 0, we can modify the sequence to
     if(r0==0)
     {
        result = 0;
     }
     else
     {
        do the actual calculation
     }
 The cases are added with existing workload analysis and should be limited to
 the target shader since adding control flow can add overhead to other shaders.
 
=============================================================================*/


#include "Compiler/CustomUnsafeOptPass.hpp"
#include "Compiler/CISACodeGen/helper.h"
#include "Compiler/IGCPassSupport.h"
#include "common/debug/Debug.hpp"
#include "common/igc_regkeys.hpp"
#include "common/LLVMWarningsPush.hpp"
#include "GenISAIntrinsics/GenIntrinsics.h"

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Pass.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/LoopPass.h>
#include "common/LLVMWarningsPop.hpp"

#include <set>

using namespace llvm;
using namespace IGC;

// Register pass to igc-opt
#define PASS_FLAG "igc-custom-unsafe-opt-pass"
#define PASS_DESCRIPTION "Unsafe Optimizations Pass"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(CustomUnsafeOptPass, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_END(CustomUnsafeOptPass, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char CustomUnsafeOptPass::ID = 0;

#define DEBUG_TYPE "CustomUnSafeOptPass"

STATISTIC(Stat_FcmpRemoved,  "Number of insts removed in FCmp Opt");
STATISTIC(Stat_FloatRemoved,  "Number of insts removed in Float Opt");
STATISTIC(Stat_DiscardRemoved,  "Number of insts removed in Discard Opt");

CustomUnsafeOptPass::CustomUnsafeOptPass()
    : FunctionPass(ID),
      m_disableReorderingOpt(0),
      m_ctx(nullptr),
      m_pMdUtils(nullptr)
{
    initializeCustomUnsafeOptPassPass(*PassRegistry::getPassRegistry());
}

bool CustomUnsafeOptPass::runOnFunction(Function &F)
{
    if (IGC_IS_FLAG_ENABLED(DisableCustomUnsafeOpt))
    {
        return false;
    }

    m_ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
    m_pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();

    m_disableReorderingOpt = false;

    if (m_ctx->type == ShaderType::VERTEX_SHADER)
    {
        m_disableReorderingOpt = true;
    }
    if (m_ctx->type == ShaderType::COMPUTE_SHADER && m_ctx->m_floatDenormMode64 == FLOAT_DENORM_RETAIN)
    {
        m_disableReorderingOpt = true;
    }

    int iterCount = 0;

    m_isChanged = true;
    // re-run the pass if the shader is changed within the pass.
    // also set a iterCount<=10 to make sure it doesn't run into infinate loop unexpectedly.
    while (m_isChanged && iterCount<=10)
    {
        iterCount++;
        m_isChanged = false;
        visit(F);
    }

    // Do reassociate to emit more mad.
    reassociateMulAdd(F);

    return true;
}

void CustomUnsafeOptPass::visitInstruction(Instruction &I)
{
    // nothing
}

bool CustomUnsafeOptPass::possibleForFmadOpt( llvm::Instruction *inst )
{
    if( inst->getOpcode() == Instruction::FAdd )
    {
        if(BinaryOperator* src0 = llvm::dyn_cast<llvm::BinaryOperator>(inst->getOperand(0)))
        {
            if(src0->getOpcode() == Instruction::FMul)
            {
                return true;
            }
        }
        if(BinaryOperator* src1 = llvm::dyn_cast<llvm::BinaryOperator>(inst->getOperand(1)))
        {
            if(src1->getOpcode() == Instruction::FMul)
            {
                return true;
            }
        }
    }
    return false;
}

bool CustomUnsafeOptPass::visitBinaryOperatorFmulFaddPropagation(BinaryOperator &I)
{
    if (m_disableReorderingOpt)
    {
        return false;
    }

/*
    Pattern 1:
        From
            %r2.x_1 = fmul float %r2.x_, %r1.y_, !dbg !10
            %r2.y_2 = fmul float %r2.y_, %r1.y_, !dbg !10    -> Src1
            %r2.z_3 = fmul float %r2.z_, %r1.y_, !dbg !10

            %oC0.x_ = fmul float %r2.x_1, 0x3FE3333340000000, !dbg !12
            %oC0.y_ = fmul float %r2.y_2, 0x3FE3333340000000, !dbg !12  -> Base
            %oC0.z_ = fmul float %r2.z_3, 0x3FE3333340000000, !dbg !12

        To
            %r2.x_1 = fmul float 0x3FE3333340000000, %r1.y_, !dbg !10
            %oC0.x_ = fmul float %r2.x_1, %r2.x_, !dbg !12
            %oC0.y_ = fmul float %r2.x_1, %r2.y_, !dbg !12
            %oC0.z_ = fmul float %r2.x_1, %r2.z_, !dbg !12

     Pattern 2:
        From
            %r0.x_ = fmul float %r1.x_, %r2.z_, !dbg !10
            %r0.y_ = fmul float %r1.y_, %r2.z_, !dbg !10                -> Src1
            %r0.z_ = fmul float %r1.z_, %r2.z_, !dbg !10
            %r2.x_1 = fmul float %r2.x_, 0x3FE6666660000000, !dbg !12
            %r2.y_2 = fmul float %r2.y_, 0x3FE6666660000000, !dbg !12   -> Src2
            %r2.z_3 = fmul float %r2.z_, 0x3FE6666660000000, !dbg !12
            %oC0.x_ = fmul float %r0.x_, %r2.x_1, !dbg !14
            %oC0.y_ = fmul float %r0.y_, %r2.y_2, !dbg !14              -> Base
            %oC0.z_ = fmul float %r0.z_, %r2.z_3, !dbg !14

        To
            %r0.x_ = fmul float 0x3FE6666660000000, %r2.z_, !dbg !10
            %r2.x_1 = fmul float %r2.x_, %r0.x_, !dbg !12
            %r2.y_2 = fmul float %r2.y_, %r0.x_, !dbg !12
            %r2.z_3 = fmul float %r2.z_, %r0.x_, !dbg !12
            %oC0.x_ = fmul float %r1.x_, %r2.x_1, !dbg !14
            %oC0.y_ = fmul float %r1.y_, %r2.y_2, !dbg !14
            %oC0.z_ = fmul float %r1.z_, %r2.z_3, !dbg !14
*/
    llvm::Instruction::BinaryOps opcode = I.getOpcode();

    // only fmul or fadd can call into this function
    assert( opcode == Instruction::FMul || opcode == Instruction::FAdd );

    llvm::Instruction* instBase[4];
    llvm::Instruction* instSrc1[4];
    llvm::Instruction* instSrc2[4];
    int sameSrcIdBase = 0;
    int sameSrcId1 = 0;
    int sameSrcId2 = 0;
    int numOfSet = 0;
    bool matchPattern1 = false;
    bool matchPattern2 = false;

    for( int i=0; i<4; i++ )
    {
        instBase[i] = NULL;
        instSrc1[i] = NULL;
        instSrc2[i] = NULL;
    }

    instBase[0] = llvm::dyn_cast<llvm::Instruction>( &I );
    if( instBase[0] == nullptr ||
        instBase[0]->getOperand(0) == instBase[0]->getOperand(1) )
    {
        return false;
    }

    instBase[1] = GetNextInstruction(instBase[0]);

    if (instBase[1] &&
        instBase[1]->getOpcode() != opcode)
    {
        instBase[1] = GetNextInstruction(instBase[1]);
    }

    if( instBase[1] == nullptr ||
        instBase[1]->getOpcode() != opcode ||
        instBase[1]->getOperand(0) == instBase[1]->getOperand(1) )
    {
        return false;
    }

    if( instBase[0]->getOperand(0) == instBase[1]->getOperand(0) )
    {
        sameSrcIdBase = 0;
        numOfSet = 2;
        matchPattern1 = true;
    }
    else if( instBase[0]->getOperand(1) == instBase[1]->getOperand(1) )
    {
        sameSrcIdBase = 1;
        numOfSet = 2;
        matchPattern1 = true;
    }
    else
    {
        matchPattern2 = true;
    }
    for( int i=2; i<4; i++ )
    {
        instBase[i] = GetNextInstruction(instBase[i-1]);

        if (instBase[i] &&
            instBase[i - 1]->getOpcode() != instBase[i]->getOpcode())
        {
            instBase[i] = GetNextInstruction(instBase[i]);
        }

        if( !instBase[i] ||
            instBase[i]->getOpcode() != opcode ||
            instBase[i]->getOperand(0) == instBase[i]->getOperand(1) ||
            possibleForFmadOpt( instBase[i] ) )
        {
            break;
        }
        numOfSet = i+1;
    }

    if( numOfSet < 2 )
    {
        return false;
    }

    if( matchPattern1 )
    {
        for( int i=0; i<numOfSet; i++ )
        {
            if( i > 0 &&
                instBase[i]->getOperand(sameSrcIdBase) != instBase[0]->getOperand(sameSrcIdBase) )
            {
                numOfSet = i;
                break;
            }

            instSrc1[i] = llvm::dyn_cast<llvm::Instruction>( instBase[i]->getOperand( 1-sameSrcIdBase ) );

            if( !instSrc1[i] ||
                !instSrc1[i]->hasOneUse() ||
                instSrc1[i]->getOpcode() != opcode ||
                possibleForFmadOpt( instSrc1[i] ) )
            {
                numOfSet = i;
                break;
            }
        }

        if( numOfSet > 1 )
        {
            if( instSrc1[0]->getOperand(0) == instSrc1[1]->getOperand(0) )
            {
                sameSrcId1 = 0;
            }
            else if( instSrc1[0]->getOperand(1) == instSrc1[1]->getOperand(1) )
            {
                sameSrcId1 = 1;
            }
            else
            {
                return false;
            }

            // instructions for the pattern can not overlap with each other
            for (int si = 0; si < numOfSet; si++)
            {
                for (int sj = 0; sj < numOfSet; sj++)
                {
                    if (instBase[si] == instSrc1[sj])
                    {
                        return false;
                    }
                }
            }

            for( int i=2; i<numOfSet; i++ )
            {
                if( instSrc1[i]->getOperand(sameSrcId1) != instSrc1[0]->getOperand(sameSrcId1) )
                {
                    numOfSet = i;
                    break;
                }
            }
        }

        if( numOfSet > 1 &&
            opcode == Instruction::FMul )
        {
            if( !dyn_cast<ConstantFP>( instSrc1[0]->getOperand( sameSrcId1 ) ) )
            {
                llvm::Instruction *tempInstr =
                    llvm::dyn_cast<llvm::Instruction>( instSrc1[0]->getOperand( sameSrcId1 ) );

                if( tempInstr->getOpcode() == Instruction::FDiv )
                {
                    ConstantFP *C0 = dyn_cast<ConstantFP>( tempInstr->getOperand(0) );
                    if( C0 && C0->isExactlyValue( 1.0 ) )
                    {
                        numOfSet = 0;
                    }
                }

                IGC::EOPCODE intrinsic_name = IGC::GetOpCode(tempInstr);
                if(intrinsic_name==IGC::llvm_exp)
                {
                    numOfSet = 0;
                }
            }
        }

        // start the optimization for pattern 1
        if( numOfSet > 1 )
        {
            Value* tempOp = instBase[0]->getOperand(sameSrcIdBase);
            for( int i=0; i<numOfSet; i++ )
            {
                instBase[i]->setOperand( 1-sameSrcIdBase, instBase[0]->getOperand(1-sameSrcIdBase) );
            }
            for( int i=0; i<numOfSet; i++ )
            {
                instBase[i]->setOperand( sameSrcIdBase, instSrc1[i]->getOperand(1-sameSrcId1) );
            }
            instSrc1[0]->setOperand( 1-sameSrcId1, tempOp );
            // move instSrc1[0] to before base
            instSrc1[0]->moveBefore( instBase[0] );
            return true;
        }
    }
    else  // check pattern 2
    {
        for( int i=0; i < numOfSet; i++ )
        {
            if( instBase[i]->getOperand(0) == instBase[i]->getOperand(1) ||
                dyn_cast<ConstantFP>( instBase[i]->getOperand(0) ) ||
                dyn_cast<ConstantFP>( instBase[i]->getOperand(1) ) ||
                possibleForFmadOpt( instBase[i] ) )
            {
                numOfSet = i;
                break;
            }
        }

        if( numOfSet > 1 )
        {
            for( int i=0; i < numOfSet; i++ )
            {
                instSrc1[i] = llvm::dyn_cast<llvm::Instruction>( instBase[i]->getOperand( 0 ) );
                instSrc2[i] = llvm::dyn_cast<llvm::Instruction>( instBase[i]->getOperand( 1 ) );
                if( !instSrc1[i] || !instSrc1[i]->hasOneUse() || instSrc1[i]->getOpcode() != opcode ||
                    !instSrc2[i] || !instSrc2[i]->hasOneUse() || instSrc2[i]->getOpcode() != opcode ||
                    possibleForFmadOpt( instSrc1[i] ) || possibleForFmadOpt( instSrc2[i] ) )
                {
                    numOfSet = i;
                    break;
                }
            }
        }

        if( numOfSet > 1 )
        {
            if( instSrc1[0]->getOperand(0) == instSrc1[1]->getOperand(0) )
            {
                sameSrcId1 = 0;
            }
            else if( instSrc1[0]->getOperand(1) == instSrc1[1]->getOperand(1) )
            {
                sameSrcId1 = 1;
            }
            else
            {
                return false;
            }

            if( instSrc2[0]->getOperand(0) == instSrc2[1]->getOperand(0) )
            {
                sameSrcId2 = 0;
            }
            else if( instSrc2[0]->getOperand(1) == instSrc2[1]->getOperand(1) )
            {
                sameSrcId2 = 1;
            }
            else
            {
                return false;
            }

            // instructions for the pattern can not overlap with each other
            for (int si = 0; si < numOfSet; si++)
            {
                for (int sj = 0; sj < numOfSet; sj++)
                {
                    for (int sk = 0; sk < numOfSet; sk++)
                    {
                        if (instBase[si] == instSrc1[sj] || instBase[si] == instSrc2[sk] || instSrc1[sj] == instSrc2[sk])
                        {
                            return false;
                        }
                    }
                }
            }

            for( int i=2; i < numOfSet; i++ )
            {
                if( instSrc1[0]->getOperand(sameSrcId1) != instSrc1[i]->getOperand(sameSrcId1) ||
                    instSrc2[0]->getOperand(sameSrcId2) != instSrc2[i]->getOperand(sameSrcId2) )
                {
                    numOfSet = i;
                    break;
                }
            }
        }

        // start the optimization for pattern 2
        if( numOfSet > 1 )
        {
            Value* tempOp = instSrc2[0]->getOperand(sameSrcId2);

            for( int i=0; i<numOfSet; i++ )
            {
                instSrc2[i]->setOperand( sameSrcId2, instBase[0]->getOperand(0) );
            }

            for( int i=0; i<numOfSet; i++ )
            {
                instBase[i]->setOperand( 0, instSrc1[i]->getOperand(1-sameSrcId1) );
            }

            instSrc1[0]->setOperand( 1-sameSrcId1, tempOp );

            for( int i=0; i<numOfSet; i++ )
            {
                instSrc2[i]->moveBefore( instBase[0] );
            }
            instSrc1[0]->moveBefore( instSrc2[0] );
            return true;
        }
    }
    return false;
}

bool CustomUnsafeOptPass::removeCommonMultiplier(llvm::Value *I, llvm::Value *commonMultiplier)
{
    Value *numerator = NULL;
    Value *denumerator = NULL;
    if (isFDiv(I, numerator, denumerator))
    {
        llvm::Instruction* multiplier = llvm::dyn_cast<llvm::Instruction>(numerator);
        if (multiplier && multiplier->getOpcode() == Instruction::FMul &&
            multiplier->getOperand(1) == commonMultiplier)
        {
            multiplier->setOperand(1, ConstantFP::get(multiplier->getType(), 1.0));
            return true;
        }
    }
    return false;
}

bool CustomUnsafeOptPass::visitBinaryOperatorExtractCommonMultiplier(BinaryOperator &I)
{
    bool patternFound = false;

    if (m_disableReorderingOpt || !I.hasOneUse())
    {
        return patternFound;
    }

    llvm::Instruction* src0 = llvm::dyn_cast<llvm::Instruction>(I.getOperand(0));

    Value *numerator0 = NULL;
    Value *denumerator0 = NULL;

    if (src0 && src0->hasOneUse() && isFDiv(src0, numerator0, denumerator0))
    {
        if (llvm::Instruction *inst = llvm::dyn_cast<llvm::Instruction>(numerator0))
        {
            if (inst->getOpcode() == Instruction::FMul)
            {
                if (llvm::Instruction* commonMultiplier = llvm::dyn_cast<llvm::Instruction>(inst->getOperand(1)))
                {
                    llvm::Instruction* sumComponent = llvm::dyn_cast<llvm::Instruction>(I.getOperand(1));
                    llvm::Instruction* currentRoot = &I;
                    llvm::Instruction* previousRoot = nullptr;
                    while (1)
                    {
                        if (sumComponent && sumComponent->hasOneUse())
                        {
                            previousRoot = currentRoot;
                            if (removeCommonMultiplier(sumComponent, commonMultiplier))
                            {
                                currentRoot = dyn_cast<Instruction>(*currentRoot->user_begin());
                                sumComponent = llvm::dyn_cast<llvm::Instruction>(currentRoot->getOperand(1));
                                patternFound = true;
                            }
                            else
                            {
                                break;
                            }
                        }
                        else
                        {
                            break;
                        }
                    }
                    if (patternFound)
                    {
                        Instruction *newResult = BinaryOperator::CreateFMul(commonMultiplier, previousRoot, "");
                        newResult->insertAfter(previousRoot);
                        previousRoot->replaceAllUsesWith(newResult);
                        newResult->setOperand(1, previousRoot);
                        inst->setOperand(1, ConstantFP::get(inst->getType(), 1.0));
                    }
                }
            }
        }
    }

    return patternFound;
}


bool CustomUnsafeOptPass::visitBinaryOperatorToFmad(BinaryOperator &I)
{
    if (m_disableReorderingOpt)
    {
        return false;
    }

    /*
    // take care of the case: C1*(a + C0) = a*C1 + C0*C1
    from
        %38 = fadd float %30, 0x3FAC28F5C0000000
        %39 = fmul float %38, 0x3FEE54EDE0000000
    to
        %38 = fmul float %33, 0x3FEE54EDE0000000
        %39 = fadd float %38, 0x3FAAB12340000000

    fmul+fadd can be replaced with fmad later in matchMad()
    */

    assert(I.getOpcode() == Instruction::FMul);

    Instruction* mulInst = (Instruction*)(&I);
    Instruction* addInst = dyn_cast<Instruction>(I.getOperand(0));

    if (!addInst || (addInst->getOpcode() != Instruction::FAdd && addInst->getOpcode() != Instruction::FSub))
    {
        return false;
    }

    ConstantFP* C0 = dyn_cast<ConstantFP>(addInst->getOperand(1));
    ConstantFP* C1 = dyn_cast<ConstantFP>(mulInst->getOperand(1));

    if(!C0 || !C1 || !addInst->hasOneUse())
    {
        return false;
    }

    Value *op0 = BinaryOperator::CreateFMul(addInst->getOperand(0), C1, "", &I);

    APFloat C0Float = C0->getValueAPF();
    C0Float.multiply(C1->getValueAPF(), llvm::APFloat::rmNearestTiesToEven);
    Value *op1 = ConstantFP::get(C0->getContext(), C0Float);

    if (addInst->getOpcode() == Instruction::FAdd)
    {
        I.replaceAllUsesWith(copyIRFlags(BinaryOperator::CreateFAdd(op0, op1, "", &I), &I));
    }
    else
    {
        I.replaceAllUsesWith(copyIRFlags(BinaryOperator::CreateFSub(op0, op1, "", &I), &I));
    }

    m_isChanged = true;

    return true;
}

bool CustomUnsafeOptPass::visitBinaryOperatorFmulToFmad(BinaryOperator &I)
{
    bool patternFound = false;

    if (m_disableReorderingOpt || (m_ctx->type == ShaderType::PIXEL_SHADER) || (m_ctx->type == ShaderType::COMPUTE_SHADER))
    {
        return patternFound;
    }

    /*
    // take care of the case: x*(1 - a) = x - x*a
    // needed for a OGL lrp pattern match, also enable more cases for the fmad optimization later

    from
        %6 = fsub float 1.000000e+00, %res_s1
        %7 = fmul float %res_s2, %6
    to
        %6 = fmul float %res_s2, %res_s1
        %7 = fsub float %res_s2, %6
    */

    assert(I.getOpcode() == Instruction::FMul);

    // check for x*(1 +/- a), (1 +/- a)*x, x*(a +/- 1), (a +/- 1)*x
    // also checks if x is a constant. 1 can be any other constant in this case.
    bool enableOpt = false;
    uint xIndex = 0;
    uint immOneIndex = 0;
    llvm::Instruction* FsubOrFaddInst = nullptr;
    for (xIndex = 0; xIndex < 2; xIndex++)
    {
        FsubOrFaddInst = llvm::dyn_cast<llvm::Instruction>(I.getOperand(1 - xIndex));
        if (FsubOrFaddInst &&
            FsubOrFaddInst->hasOneUse() &&
            (FsubOrFaddInst->getOpcode() == Instruction::FSub || FsubOrFaddInst->getOpcode() == Instruction::FAdd) )
        {
            ConstantFP *Cx = dyn_cast<ConstantFP>(I.getOperand(xIndex));
            ConstantFP *C0 = dyn_cast<ConstantFP>(FsubOrFaddInst->getOperand(0));
            if (C0 && !C0->isZero() && (C0->isExactlyValue(1.f) || Cx))
            {
                enableOpt = true;
                immOneIndex = 0;
                break;
            }
            ConstantFP *C1 = dyn_cast<ConstantFP>(FsubOrFaddInst->getOperand(1));
            if (C1 && !C1->isZero() && (C1->isExactlyValue(1.f) || Cx))
            {
                enableOpt = true;
                immOneIndex = 1;
                break;
            }
        }
    }

    // start optimization
    if (enableOpt)
    {
        Value *op1 = nullptr;
        Value *op2 = nullptr;
        ConstantFP *Cx = dyn_cast<ConstantFP>(I.getOperand(xIndex));

        if (immOneIndex == 0)
        {
            if (Cx)
            {
                ConstantFP *C0 = dyn_cast<ConstantFP>(FsubOrFaddInst->getOperand(0));
                APFloat CxFloat = Cx->getValueAPF();
                CxFloat.multiply(C0->getValueAPF(), llvm::APFloat::rmNearestTiesToEven);
                op1 = ConstantFP::get(C0->getContext(), CxFloat);
            }
            else
            {
                op1 = I.getOperand(xIndex);
            }
            op2 = BinaryOperator::CreateFMul(I.getOperand(xIndex), FsubOrFaddInst->getOperand(1), "", &I);
        }
        else
        {
            op1 = BinaryOperator::CreateFMul(I.getOperand(xIndex), FsubOrFaddInst->getOperand(0), "", &I);
            if (Cx)
            {
                ConstantFP *C1 = dyn_cast<ConstantFP>(FsubOrFaddInst->getOperand(1));
                APFloat CxFloat = Cx->getValueAPF();
                CxFloat.multiply(C1->getValueAPF(), llvm::APFloat::rmNearestTiesToEven);
                op2 = ConstantFP::get(C1->getContext(), CxFloat);
            }
            else
            {
                op2 = I.getOperand(xIndex);
            }
        }

        if (FsubOrFaddInst->getOpcode() == Instruction::FSub)
        {
            I.replaceAllUsesWith(
                copyIRFlags(BinaryOperator::CreateFSub(op1, op2, "", &I), &I));
        }
        else
        {
            I.replaceAllUsesWith(
                copyIRFlags(BinaryOperator::CreateFAdd(op1, op2, "", &I), &I));
        }
        m_isChanged = true;
        patternFound = true;
    }
    return patternFound;
}

bool CustomUnsafeOptPass::isFDiv(Value *I, Value *&numerator, Value *&denominator)
{
    bool result = false;
    if (llvm::Instruction *div = llvm::dyn_cast<llvm::Instruction>(I))
    {
        if (div->getOpcode() == Instruction::FDiv)
        {
            numerator = div->getOperand(0);
            denominator = div->getOperand(1);
            result = true;
        }
        else if (div->getOpcode() == Instruction::FMul)
        {
            if (llvm::Instruction *inv = llvm::dyn_cast<llvm::Instruction>(div->getOperand(1)))
            {
                if (inv->getOpcode() == Instruction::FDiv &&
                    dyn_cast<ConstantFP>(inv->getOperand(0)) &&
                    dyn_cast<ConstantFP>(inv->getOperand(0))->isExactlyValue(1.0))
                {
                    numerator = div->getOperand(0);
                    denominator = inv->getOperand(1);
                    result = true;
                }
            }
        }
    }
    return result;
}

bool CustomUnsafeOptPass::visitBinaryOperatorDivAddDiv(BinaryOperator &I)
{
    // A/B +C/D can be changed to (A * D +C * B)/(B * D).
    if (m_disableReorderingOpt)
    {
        return false;
    }

    Value *numerator0 = NULL;
    Value *numerator1 = NULL;
    Value *denumerator0 = NULL;
    Value *denumerator1 = NULL;

    if (isFDiv(I.getOperand(0), numerator0, denumerator0) &&
        isFDiv(I.getOperand(1), numerator1, denumerator1) &&
        denumerator0 != denumerator1)
    {
        Value *mul0 = BinaryOperator::CreateFMul(numerator0, denumerator1, "", &I);
        Value *mul1 = BinaryOperator::CreateFMul(numerator1, denumerator0, "", &I);
        Value *mul2 = BinaryOperator::CreateFMul(denumerator0, denumerator1, "", &I);
        Value *mul2inv = BinaryOperator::CreateFDiv(ConstantFP::get(mul2->getType(), 1.0), mul2, "", &I);
        Value *add_mul0_mul1 = BinaryOperator::CreateFAdd(mul0, mul1, "", &I);
        I.replaceAllUsesWith(BinaryOperator::CreateFMul(add_mul0_mul1, mul2inv, "", &I));
        return true;
    }
    return false;
}
    
bool CustomUnsafeOptPass::visitBinaryOperatorDivDivOp(BinaryOperator &I)
{
    if (m_disableReorderingOpt)
    {
        return false;
    }

    llvm::Instruction* prevInst = llvm::dyn_cast<llvm::Instruction>( I.getOperand( 1 ) );
    bool patternFound = false;

    if( prevInst && prevInst->getOpcode() == Instruction::FDiv )
    {
        Value * prevInstOp = prevInst->getOperand( 0 );
        ConstantFP *C0 = dyn_cast<ConstantFP>(prevInstOp);

        if (C0 && C0->isExactlyValue(1.0))
        {
            ConstantFP * Iconst = dyn_cast<ConstantFP>(I.getOperand(0));
            if (Iconst && Iconst->isExactlyValue(1.0))
            {
                // a = 1 / b
                // c = 1 / a
                //    =>
                // c = b
                I.replaceAllUsesWith(prevInst->getOperand(1));
            }
            else
            {
                // a = 1 / b
                // d = c / a
                //    =>
                // d = c * b
                I.replaceAllUsesWith(
                    copyIRFlags(BinaryOperator::CreateFMul(I.getOperand(0), prevInst->getOperand(1), "", &I), &I));
            }
            ++Stat_FloatRemoved;
            patternFound = true;
            m_isChanged = true;
        }
    }

    return patternFound;
}

bool CustomUnsafeOptPass::visitBinaryOperatorAddSubOp(BinaryOperator &I)
{
    bool patternFound = false;
    if (m_disableReorderingOpt)
    {
        return patternFound;
    }

    // a = b + x
    // d = a - x
    //    =>
    // d = b
    assert( I.getOpcode() == Instruction::FAdd || I.getOpcode() == Instruction::FSub );

    Value* op[2];
    op[0] = I.getOperand(0);
    op[1] = I.getOperand(1);

    for( int i=0; i<2; i++)
    {
        llvm::Instruction* sourceInst = llvm::dyn_cast<llvm::Instruction>( op[i] );
        if( !sourceInst )
        {
            continue;
        }

        if( I.getOpcode() == Instruction::FAdd && sourceInst->getOpcode() == Instruction::FSub)
        {
            // a = b - x
            // d = a + x
            //    =>
            // d = b
            if (op[1 - i] == sourceInst->getOperand(1))
            {
                I.replaceAllUsesWith( sourceInst->getOperand(0) );
                ++Stat_FloatRemoved;
                m_isChanged = true;
                patternFound = true;
                break;
            }
        }
        else if( I.getOpcode() == Instruction::FSub && sourceInst->getOpcode() == Instruction::FSub)
        {
            // a = x - b
            // d = x - a
            //    =>
            // d = b
            if( i == 1 && op[0] == sourceInst->getOperand(0) )
            {
                I.replaceAllUsesWith(sourceInst->getOperand(1));
                ++Stat_FloatRemoved;
                m_isChanged = true;
                patternFound = true;
            }
        }
        else if( I.getOpcode() == Instruction::FSub && sourceInst->getOpcode() == Instruction::FAdd)
        {
            Value* srcOp[2];
            srcOp[0] = sourceInst->getOperand(0);
            srcOp[1] = sourceInst->getOperand(1);
            if( i == 0 )
            {
                for( int srci=0; srci<2; srci++ )
                {
                    if( op[1-i] == srcOp[srci] )
                    {
                        I.replaceAllUsesWith( srcOp[1-srci] );
                        ++Stat_FloatRemoved;
                        m_isChanged = true;
                        patternFound = true;
                        break;
                    }
                }
            }
            else if( i == 1 )
            {
                for( int srci=0; srci<2; srci++ )
                {
                    if( op[1-i] == srcOp[srci] )
                    {
                        I.replaceAllUsesWith(
                            copyIRFlags(BinaryOperator::CreateFSub(
                                            ConstantFP::get(op[0]->getType(), 0), srcOp[1-srci], "", &I),
                                        &I));
                        ++Stat_FloatRemoved;
                        m_isChanged = true;
                        patternFound = true;
                        break;
                    }
                }
            }
        }
        else
        {
            continue;
        }
    }
    return patternFound;
}

bool CustomUnsafeOptPass::visitBinaryOperatorPropNegate(BinaryOperator &I)
{
    if (m_disableReorderingOpt)
    {
        return false;
    }

    bool foundPattern = false;

    // a = 0 - b
    // c = a + d
    //    =>
    // c = d - b
    assert( I.getOpcode() == Instruction::FAdd );

    for( int i=0; i<2; i++ )
    {
        llvm::Instruction* prevInst = llvm::dyn_cast<llvm::Instruction>( I.getOperand( i ) );
        if( prevInst && prevInst->getOpcode() == Instruction::FSub )
        {
            ConstantFP *fp0 = dyn_cast<ConstantFP>( prevInst->getOperand( 0 ) );
            if( fp0 && fp0->isZero() )
            {
                I.replaceAllUsesWith(
                    copyIRFlags(BinaryOperator::CreateFSub(
                                    I.getOperand(1 - i), prevInst->getOperand(1), "", &I),
                                &I));
                ++Stat_FloatRemoved;
                m_isChanged = true;
                foundPattern = true;
                break;
            }
        }
    }
    return foundPattern;
}

bool CustomUnsafeOptPass::visitBinaryOperatorNegateMultiply(BinaryOperator &I)
{
    // a = b * c
    // d = 0 - a
    //    =>
    // d = (-b) * c
    assert(I.getOpcode() == Instruction::FSub);
    bool patternFound = false;
    bool replaced = false;

    if (I.getOperand(1)->hasOneUse())
    {
        llvm::Instruction* fmulInst = llvm::dyn_cast<llvm::Instruction>(I.getOperand(1));
        if (fmulInst && fmulInst->getOpcode() == Instruction::FMul)
        {
            // if one of the mul src is imm, apply the negate on that imm
            for (int i = 0; i < 2; i++)
            {
                if (llvm::Instruction* fmulSrc = llvm::dyn_cast<llvm::Instruction>(fmulInst->getOperand(i)))
                {
                    if (ConstantFP *fp = dyn_cast<ConstantFP>(fmulSrc))
                    {
                        APFloat newConstantFloat = fp->getValueAPF();
                        newConstantFloat.changeSign();
                        Constant* newConstant = ConstantFP::get(fmulSrc->getContext(), newConstantFloat);
                        fmulSrc->setOperand(i, newConstant);
                        I.replaceAllUsesWith(fmulInst);
                        replaced = true;
                        break;
                    }
                }
            }

            // otherwise replace mul src0 with the negate
            if (!replaced)
            {
                fmulInst->setOperand(0, BinaryOperator::CreateFSub(ConstantFP::get(fmulInst->getType(), 0), fmulInst->getOperand(0), "", fmulInst));
                I.replaceAllUsesWith(fmulInst);
            }
            ++Stat_FloatRemoved;
            m_isChanged = true;
            patternFound = true;
        }
    }
    return patternFound;
}

bool CustomUnsafeOptPass::visitBinaryOperatorTwoConstants(BinaryOperator &I)
{
    bool patternFound = false;

    if (m_disableReorderingOpt)
    {
        return patternFound;
    }

    // a = b + C0
    // d = a + C1
    //    =>
    // d = b + ( C0 + C1 )

    // this optimization works on fadd, fsub, and fmul

    assert( dyn_cast<ConstantFP>( I.getOperand(0) ) || dyn_cast<ConstantFP>( I.getOperand(1) ) );

    llvm::Instruction::BinaryOps opcode = I.getOpcode();
    assert( opcode == Instruction::FAdd || opcode == Instruction::FSub || opcode == Instruction::FMul );

    int regSrcNum = (dyn_cast<ConstantFP>(I.getOperand(0))) ? 1 : 0;
    Value * Iop = I.getOperand( regSrcNum );

    llvm::Instruction* prevInst = llvm::dyn_cast<llvm::Instruction>( Iop );

    if (!prevInst)
    {
        return patternFound;
    }

    if( prevInst->getOpcode() != Instruction::FMul &&
        prevInst->getOpcode() != Instruction::FAdd &&
        prevInst->getOpcode() != Instruction::FSub )
    {
        return patternFound;
    }

    // check if prevInst has one constant in the srcs.
    if( dyn_cast<ConstantFP>( prevInst->getOperand(0) ) || dyn_cast<ConstantFP>( prevInst->getOperand(1) ) )
    {
        if (!prevInst->hasOneUse() &&
            I.getOpcode() == Instruction::FSub )
        {
            ConstantFP *ConstantZero = dyn_cast<ConstantFP>(I.getOperand(0));
            if (ConstantZero && ConstantZero->isZeroValue())
            {
                return patternFound;
            }
        }

        int prevInstRegSrcNum = (dyn_cast<ConstantFP>( prevInst->getOperand(0)) ) ? 1 : 0;

        Value * prevInstOp = prevInst->getOperand( prevInstRegSrcNum );

        ConstantFP *C0 = dyn_cast<ConstantFP>( prevInst->getOperand( 1-prevInstRegSrcNum ) );
        ConstantFP *C1 = dyn_cast<ConstantFP>( I.getOperand( 1-regSrcNum ) );

        assert( C0 && C1 );

        APFloat newConstantFloat(0.0);
        bool orderConstantFirst = false;
        bool changeOpToAdd = false;
        bool changeOpToSub = false;
        if( prevInst->getOpcode() == Instruction::FMul && opcode == Instruction::FMul )
        {
            newConstantFloat = C0->getValueAPF();
            newConstantFloat.multiply(C1->getValueAPF(), llvm::APFloat::rmNearestTiesToEven);
        }
        else if( prevInst->getOpcode() == Instruction::FAdd && opcode == Instruction::FAdd )
        {
            newConstantFloat = C0->getValueAPF();
            newConstantFloat.add(C1->getValueAPF(), llvm::APFloat::rmNearestTiesToEven);
        }
        else if( prevInst->getOpcode() == Instruction::FSub && opcode == Instruction::FSub )
        {
            if( prevInstRegSrcNum == 0 && regSrcNum == 0)
            {
                newConstantFloat = C0->getValueAPF();
                newConstantFloat.add(C1->getValueAPF(), llvm::APFloat::rmNearestTiesToEven);
            }
            else if( prevInstRegSrcNum == 0 && regSrcNum == 1)
            {
                newConstantFloat = C0->getValueAPF();
                newConstantFloat.add(C1->getValueAPF(), llvm::APFloat::rmNearestTiesToEven);
                orderConstantFirst = true;
            }
            else if( prevInstRegSrcNum == 1 && regSrcNum == 0)
            {
                newConstantFloat = C0->getValueAPF();
                newConstantFloat.subtract(C1->getValueAPF(), llvm::APFloat::rmNearestTiesToEven);
                orderConstantFirst = true;
            }
            else if( prevInstRegSrcNum == 1 && regSrcNum == 1)
            {
                newConstantFloat = C1->getValueAPF();
                newConstantFloat.subtract(C0->getValueAPF(), llvm::APFloat::rmNearestTiesToEven);
                changeOpToAdd = true;
            }

        }
        else if( prevInst->getOpcode() == Instruction::FAdd && opcode == Instruction::FSub )
        {
            if( regSrcNum == 0 )
            {
                newConstantFloat = C0->getValueAPF();
                newConstantFloat.subtract(C1->getValueAPF(), llvm::APFloat::rmNearestTiesToEven);
                changeOpToAdd = true;
            }
            else
            {
                newConstantFloat = C1->getValueAPF();
                newConstantFloat.subtract(C0->getValueAPF(), llvm::APFloat::rmNearestTiesToEven);
                orderConstantFirst = true;
            }
        }
        else if( prevInst->getOpcode() == Instruction::FSub && opcode == Instruction::FAdd )
        {
            if( prevInstRegSrcNum == 0 )
            {
                newConstantFloat = C1->getValueAPF();
                newConstantFloat.subtract(C0->getValueAPF(), llvm::APFloat::rmNearestTiesToEven);
                changeOpToAdd = true;
            }
            else
            {
                newConstantFloat = C1->getValueAPF();
                newConstantFloat.add(C0->getValueAPF(), llvm::APFloat::rmNearestTiesToEven);
                changeOpToSub = true;
                orderConstantFirst = true;
            }
        }
        else
        {
            return patternFound;
        }

        ++Stat_FloatRemoved;
        Constant* newConstant = ConstantFP::get(C1->getContext(), newConstantFloat);
        if( newConstant->isZeroValue() && !orderConstantFirst)
        {
            if( opcode == Instruction::FAdd || opcode == Instruction::FSub )
            {
                I.replaceAllUsesWith( prevInstOp );
                patternFound = true;
                m_isChanged = true;
            }
            else if( opcode == Instruction::FMul )
            {
                I.replaceAllUsesWith( ConstantFP::get( Iop->getType(), 0 ) );
                patternFound = true;
                m_isChanged = true;
            }
        }
        else
        {
            if (changeOpToAdd)
            {
               I.replaceAllUsesWith(
                   copyIRFlags(BinaryOperator::CreateFAdd(prevInstOp, newConstant, "", &I), &I));
            }
            else if( changeOpToSub )
            {
               if( orderConstantFirst )
               {
                   I.replaceAllUsesWith(
                       copyIRFlags(BinaryOperator::CreateFSub(newConstant, prevInstOp, "", &I), &I));
               }
               else
               {
                   I.replaceAllUsesWith(
                       copyIRFlags(BinaryOperator::CreateFSub(prevInstOp, newConstant, "", &I), &I));
               }
            }
            else
            {
                I.setOperand( orderConstantFirst, prevInstOp );
                I.setOperand( 1-orderConstantFirst, newConstant );
            }
            patternFound = true;
        }
    }
    return patternFound;
}

bool CustomUnsafeOptPass::visitBinaryOperatorDivRsq(BinaryOperator &I)
{
    if (GenIntrinsicInst *genIntr = dyn_cast<GenIntrinsicInst>(I.getOperand(1)))
    {
        if (genIntr->getIntrinsicID() == GenISAIntrinsic::GenISA_rsq)
        {
            if (ConstantFP *fp0 = dyn_cast<ConstantFP>(I.getOperand(0)))
            {
                llvm::IRBuilder<> builder(I.getContext());
                llvm::CallInst* sqrt_call = llvm::IntrinsicInst::Create(
                    llvm::Intrinsic::getDeclaration(m_ctx->getModule(), Intrinsic::sqrt, builder.getFloatTy()), genIntr->getOperand(0), "", &I);

                if (fp0->isExactlyValue(1.0))
                {
                    // 1/rsq -> sqrt
                    I.replaceAllUsesWith(sqrt_call);
                }
                else
                {
                    // a/rsq -> a*sqrt
                    I.replaceAllUsesWith(
                        copyIRFlags(BinaryOperator::CreateFMul(I.getOperand(0), sqrt_call, "", &I), &I));
                }
                return true;
            }
        }
    }
    return false;
}

bool CustomUnsafeOptPass::visitBinaryOperatorAddDiv(BinaryOperator &I)
{
    llvm::Instruction* faddInst = llvm::dyn_cast<llvm::Instruction>(I.getOperand(0));

    if (faddInst &&
        (faddInst->getOpcode() == Instruction::FAdd ||
        faddInst->getOpcode() == Instruction::FSub) &&
        faddInst->hasOneUse())
    {
        for (int i = 0; i < 2; i++)
        {
            if (faddInst->getOperand(i) == I.getOperand(1))
            {
                Value *div = BinaryOperator::CreateFDiv(faddInst->getOperand(1-i), I.getOperand(1), "", faddInst);
                Value *one = ConstantFP::get(I.getType(), 1.0);

                if (faddInst->getOpcode() == Instruction::FAdd)
                {
                    if (i == 0)
                    {
                        I.replaceAllUsesWith(copyIRFlags(BinaryOperator::CreateFAdd(one, div, "", &I), &I));
                    }
                    else
                    {
                        I.replaceAllUsesWith(copyIRFlags(BinaryOperator::CreateFAdd(div, one, "", &I), &I));
                    }
                }
                else if (faddInst->getOpcode() == Instruction::FSub)
                {
                    if (i == 0)
                    {
                        I.replaceAllUsesWith(copyIRFlags(BinaryOperator::CreateFSub(one, div, "", &I), &I));
                    }
                    else
                    {
                        I.replaceAllUsesWith(copyIRFlags(BinaryOperator::CreateFSub(div, one, "", &I), &I));
                    }
                }
                return true;
            }
        }
    }
    return false;
}

bool CustomUnsafeOptPass::visitExchangeCB(llvm::BinaryOperator &I)
{
    // a = b x CB0
    // c = b x CB1
    // e = a + c
    //    =>
    // e = b x ( CB0 + CB1 )

    // CB can be constant buffer load or immediate constant. 
    // The goal is to put loop invariant calculations together and hoist it out of a loop. 

    Instruction* inst0 = dyn_cast<Instruction>(I.getOperand(0));
    Instruction* inst1 = dyn_cast<Instruction>(I.getOperand(1));

    if (!inst0 || !inst1 || inst0->getOpcode() != Instruction::FMul || inst1->getOpcode() != Instruction::FMul)
    {
        return false;
    }

    unsigned bufId;
    unsigned cbIndex0 = 0;
    unsigned cbIndex1 = 0;
    unsigned hasCB = 0;
    bool directBuf;

    for (int i = 0; i < 2; i++)
    {
        if (LoadInst* ld0 = dyn_cast<LoadInst>(inst0->getOperand(i)))
        {
            if (IGC::DecodeAS4GFXResource(ld0->getPointerAddressSpace(), directBuf, bufId) == CONSTANT_BUFFER && directBuf)
            {
                cbIndex0 = i;
                hasCB++;
            }
        }
        else if (dyn_cast<Constant>(inst0->getOperand(i)))
        {
            cbIndex0 = i;
            hasCB++;
        }
    }

    if (hasCB != 1)
        return false;

    hasCB = 0;
    for (int i = 0; i < 2; i++)
    {
        if (LoadInst* ld1 = dyn_cast<LoadInst>(inst1->getOperand(i)))
        {
            if (IGC::DecodeAS4GFXResource(ld1->getPointerAddressSpace(), directBuf, bufId) == CONSTANT_BUFFER && directBuf)
            {
                cbIndex1 = i;
                hasCB++;
            }
            else if (dyn_cast<Constant>(inst1->getOperand(i)))
            {
                cbIndex1 = i;
                hasCB++;
            }
        }
    }

    if (hasCB != 1)
        return false;
    
    if (inst0->getOperand(1 - cbIndex0) != inst1->getOperand(1 - cbIndex1))
        return false;

    // perform the change
    Value *CBsum = BinaryOperator::CreateFAdd(inst0->getOperand(cbIndex0), inst1->getOperand(cbIndex1), "", &I);
    I.replaceAllUsesWith(copyIRFlags(BinaryOperator::CreateFMul(inst0->getOperand(1 - cbIndex0), CBsum, "", &I), &I));
    
    return true;
}

void CustomUnsafeOptPass::visitBinaryOperator(BinaryOperator &I)
{
    if (I.use_empty())
    {
        return;
    }

    if (allowUnsafeMathOpt(I))
    {
        Value*	op0 = I.getOperand(0);
        Value*	op1 = I.getOperand(1);
        if (op0->getType()->isFPOrFPVectorTy() && op1->getType()->isFPOrFPVectorTy() )
        {
            ConstantFP *fp0 = dyn_cast<ConstantFP>( op0 );
            ConstantFP *fp1 = dyn_cast<ConstantFP>( op1 );
            Type *opType = op0->getType();

            switch (I.getOpcode())
            {
                case Instruction::FSub:
                    if (op0 == op1)
                    {
                        // X - X => 0
                        I.replaceAllUsesWith(ConstantFP::get(opType, 0));
                        ++Stat_FloatRemoved;
                        m_isChanged = true;
                    }
                    else if( fp1 && fp1->isZero() )
                    {
                        // X - 0 => X
                        I.replaceAllUsesWith(op0);
                        ++Stat_FloatRemoved;
                        m_isChanged = true;
                    }
                    else if( fp0 && fp0->isZero() )
                    {
                        m_isChanged |= visitBinaryOperatorNegateMultiply(I);
                    }
                    else
                    {
                        bool patternFound = false;
                        if (fp0 || fp1)
                        {
                            // a = b + C0
                            // d = a + C1
                            //    =>
                            // d = b + ( C0 + C1 )
                            patternFound = visitBinaryOperatorTwoConstants(I);
                        }
                        else
                        {
                            // a = b + x
                            // d = a - x
                            //    =>
                            // d = b
                            patternFound = visitBinaryOperatorAddSubOp(I);
                        }
                        m_isChanged |= patternFound;
                    }

                    break;

                case Instruction::FAdd:
                    if( fp0 && fp0->isZero() )
                    {
                        // 0 + X => X
                        I.replaceAllUsesWith( op1 );
                        ++Stat_FloatRemoved;
                        m_isChanged = true;
                    }
                    else if( fp1 && fp1->isZero() )
                    {
                        // X + 0 => X
                        I.replaceAllUsesWith( op0 );
                        ++Stat_FloatRemoved;
                        m_isChanged = true;
                    }
                    else
                    {
                        bool patternFound = false;
                        if (fp0 || fp1)
                        {
                            // a = b + C0
                            // d = a + C1
                            //    =>
                            // d = b + ( C0 + C1 )
                            patternFound = visitBinaryOperatorTwoConstants(I);
                        }

                        if (op0 != op1)
                        {
                            // a = b - x
                            // d = a + x
                            //    =>
                            // d = b
                            if (!patternFound)
                            {
                                patternFound = visitBinaryOperatorAddSubOp(I);
                            }

                            // a = 0 - b
                            // c = a + d
                            //    =>
                            // c = d - b
                            if (!patternFound)
                            {
                                patternFound = visitBinaryOperatorPropNegate(I);
                            }
                        }

                        // fmul/fadd propagation
                        if (!patternFound)
                        {
                            patternFound = visitBinaryOperatorFmulFaddPropagation(I);
                        }

                        // A/B +C/D can be changed to (A * D +C * B)/(B * D).
                        if (!patternFound && IGC_IS_FLAG_ENABLED(EnableSumFractions))
                        {
                            patternFound = visitBinaryOperatorDivAddDiv(I);
                        }

                        if (!patternFound && IGC_IS_FLAG_ENABLED(EnableExtractCommonMultiplier))
                        {
                            patternFound = visitBinaryOperatorExtractCommonMultiplier(I);
                        }

                        if (!patternFound)
                        {
                            patternFound = visitExchangeCB(I);
                        }
                        m_isChanged |= patternFound;
                    }
                    break;

                case Instruction::FMul:
                    if ( (fp0 && fp0->isZero()) ||
                         (fp1 && fp1->isZero()) )
                    {
                        // X * 0 => 0
                        // 0 * X => 0
                        I.replaceAllUsesWith(ConstantFP::get(opType, 0));
                        ++Stat_FloatRemoved;
                        m_isChanged = true;
                    }
                    else if ( fp0 && fp0->isExactlyValue( 1.0 ) )
                    {
                        // 1 * X => X
                        I.replaceAllUsesWith(op1);
                        ++Stat_FloatRemoved;
                        m_isChanged = true;
                    }
                    else if ( fp1 && fp1->isExactlyValue( 1.0 ) )
                    {
                        // X * 1 => X
                        I.replaceAllUsesWith(op0);
                        ++Stat_FloatRemoved;
                        m_isChanged = true;
                    }
                    // X * -1 => -X
                    else if( fp1 && fp1->isExactlyValue( -1.0 ) )
                    {
                        I.replaceAllUsesWith(
                            copyIRFlags(BinaryOperator::CreateFSub(ConstantFP::get(opType, 0), op0, "", &I), &I));
                        ++Stat_FloatRemoved;
                        m_isChanged = true;
                    }
                    else if( fp0 && fp0->isExactlyValue( -1.0 ) )
                    {
                        I.replaceAllUsesWith(
                            copyIRFlags(BinaryOperator::CreateFSub(ConstantFP::get(opType, 0), op1, "", &I), &I));
                        ++Stat_FloatRemoved;
                        m_isChanged = true;
                    }
                    else
                    {
                        bool patternFound = false;

                        if (fp0 || fp1)
                        {
                            // a = b * C0
                            // d = a * C1
                            //    =>
                            // d = b * ( C0 * C1 )
                            patternFound = visitBinaryOperatorTwoConstants(I);
                        }

                        // fmul/fadd propagation
                        if (!patternFound)
                        {
                            patternFound = visitBinaryOperatorFmulFaddPropagation(I);
                        }

                        //x*(1 - a) = x - x*a
                        if (!patternFound)
                        {
                            patternFound = visitBinaryOperatorFmulToFmad(I);
                        }

                        //C1*(a + C0) = a*C1 + C0*C1
                        if (!patternFound)
                        {
                            patternFound = visitBinaryOperatorToFmad(I);
                        }

                        m_isChanged |= patternFound;
                    }
                    break;

                case Instruction::FDiv:
                    if ( fp0 && fp0->isZero() )
                    {
                        // 0 / X => 0
                        I.replaceAllUsesWith(ConstantFP::get(opType, 0));
                        ++Stat_FloatRemoved;
                        m_isChanged = true;
                    }
                    else if ( fp1 && fp1->isExactlyValue( 1.0 ) )
                    {
                        // X / 1 => X
                        I.replaceAllUsesWith(op0);
                        ++Stat_FloatRemoved;
                        m_isChanged = true;
                    }
                    else
                    {
                        // a = 1 / b
                        // c = 1 / a
                        //    =>
                        // c = b
                        //     or
                        // a = 1 / b
                        // d = c / a
                        //    =>
                        // d = b * c
                        bool patternFound = false;
                        patternFound = visitBinaryOperatorDivDivOp(I);

                        // 1/rsq -> rsq or a/rsq -> a * sqrt
                        if (!patternFound)
                        {
                            patternFound = visitBinaryOperatorDivRsq(I);
                        }

                        // skip for double type.
                        if (opType->getTypeID() == llvm::Type::FloatTyID || opType->getTypeID() == llvm::Type::HalfTyID)
                        {
                            // add r6.x, -r6.y, |r6.x|
                            // div_sat r6.x, r6.x, r6.y
                            //     To
                            // div r6.y, l(1.000000, 1.000000, 1.000000, 1.000000), r6.y
                            // mad_sat r6.x, | r6.x | , r6.y, l(-1.000000)
                            if (!patternFound)
                            {
                                patternFound = visitBinaryOperatorAddDiv(I);
                            }

                            // FDIV to FMUL+INV
                            if (!patternFound)
                            {
                                if (!(fp0 && fp0->isExactlyValue(1.0)))
                                {
                                    Value *invOp = BinaryOperator::CreateFDiv(ConstantFP::get(opType, 1.0), op1, "", &I);
                                    I.replaceAllUsesWith(
                                        copyIRFlags(BinaryOperator::CreateFMul(op0, invOp, "", &I), &I));
                                    patternFound = true;
                                }
                            }
                        }
                        m_isChanged |= patternFound;
                    }
                    break;

                default:
                    break;
            }
        }
    }
}

bool CustomUnsafeOptPass::visitFCmpInstFCmpFAddOp(FCmpInst &FC)
{
    //  %3 = fadd float %2, 0x40015C29
    //  %4 = fcmp uge float %3, 0.0
    //         =>
    //  %4 = fcmp uge float %2, -(0x40015C29)
    // only do the optimization if we add has only one use since it could prevent us to do
    // other optimization otherwise

    Value* fcmpOp1 = FC.getOperand(1);
    ConstantFP *fcmpConstant = dyn_cast<ConstantFP>(fcmpOp1);
    if( fcmpConstant )
    {
        llvm::Instruction* faddInst = llvm::dyn_cast<llvm::Instruction>( FC.getOperand(0) );
        if( faddInst &&
            ( faddInst->getOpcode() == Instruction::FAdd ||
            faddInst->getOpcode() == Instruction::FSub ) &&
            faddInst->hasOneUse())
        {
            Value* faddOp1 = faddInst->getOperand(1);
            ConstantFP *faddConstant = dyn_cast<ConstantFP>(faddOp1);
            if( faddConstant )
            {
                APFloat newConstantFloat(0.0);
                if( faddInst->getOpcode() == Instruction::FAdd )
                {
                    newConstantFloat = fcmpConstant->getValueAPF();
                    newConstantFloat.subtract(faddConstant->getValueAPF(), llvm::APFloat::rmNearestTiesToEven);
                }
                else
                {
                    newConstantFloat = fcmpConstant->getValueAPF();
                    newConstantFloat.add(faddConstant->getValueAPF(), llvm::APFloat::rmNearestTiesToEven);
                }

                ConstantFP* newConstant = ConstantFP::get(fcmpConstant->getContext(), newConstantFloat);
                FC.setOperand( 0, faddInst->getOperand(0) );
                FC.setOperand( 1, newConstant );
                ++Stat_FcmpRemoved;
                return true;
            }
        }
    }
    return false;
}

bool CustomUnsafeOptPass::visitFMulFCmpOp(FCmpInst &FC)
{
    // pattern match fmul+fsub+fcmp into fcmp
    bool patternFound = false;
    llvm::Instruction* prevInst[2];
    prevInst[0] = llvm::dyn_cast<llvm::Instruction>(FC.getOperand(0));
    prevInst[1] = llvm::dyn_cast<llvm::Instruction>(FC.getOperand(1));

    if (!prevInst[0] || !prevInst[1])
    {
        return false;
    }

    for (int i = 0; i < 2; i++)
    {
        if (prevInst[i]->getOpcode() != Instruction::FSub ||
            prevInst[1 - i]->getOpcode() != Instruction::FMul)
        {
            continue;
        }
        ConstantFP *fpc = dyn_cast<ConstantFP>(prevInst[i]->getOperand(0));
        if (!fpc || !fpc->isZero() || prevInst[i]->getOperand(1) != prevInst[1 - i])
        {
            continue;
        }
        // Found the following template:
        //   op_<1-i>: %mul = fmul float %x, %y
        //   op_<i>  : %sub = fsub float 0.000000e+00, %mul
        //             %cmp = fcmp <cmpOp> float %op_0, %op_1

        if (prevInst[1 - i]->getOperand(0) == prevInst[1 - i]->getOperand(1))
        {
            // %x == %y --> %mul =  (x^2) always >=0
            //              %sub = -(x^2) always <=0

            if ((FC.getPredicate() == FCmpInst::FCMP_OLT && i == 0) ||
                (FC.getPredicate() == FCmpInst::FCMP_OGT && i == 1))
            {
                // Optimize:
                //   %cmp = fcmp olt float %sub, %mul       -> cmp.lt -(x^2), (x^2)
                // or:
                //   %cmp = fcmp ogt float %mul, %sub       -> cmp.gt (x^2), -(x^2)
                // into:
                //   %cmp = fcmp one float %x, 0          -> cmp.ne (x^2), 0
                FC.setPredicate(FCmpInst::FCMP_ONE);
                FC.setOperand(0, prevInst[1 - i]->getOperand(0));
                FC.setOperand(1, ConstantFP::get(Type::getFloatTy(FC.getContext()), 0));
                patternFound = true;
                break;
            }
        }
        else if (ConstantFP *fmulConstant = dyn_cast<ConstantFP>(prevInst[1 - i]->getOperand(1)))
        {
            if (fmulConstant->isZeroValue())
            {
                continue;
            }
            // Optimize:
            //   %mul = fmul float %x, 5.000000e-01     ->   (x * constant)
            //   %sub = fsub float 0.000000e+00, %mul   ->  -(x * constant)
            //   %cmp = fcmp <cmpOp> float %mul, %sub
            // into:
            //   %cmp = fcmp <cmpOp> float %x, 0  [if (constant>0)]
            // or:
            //   %cmp = fcmp <cmpOp> float 0, %x  [if (constant<0)]

            if (fmulConstant->isNegative())
            {
                if (i == 0)
                {
                    //handling case:
                    //    %mul = fmul float %x, -5.000000e-01     ->   (x * constant) 
                    //    %sub = fsub float 0.000000e+00, %mul   ->  -(x * constant) 
                    //    %cmp = fcmp <cmpOp> float %sub, %mul
                    // into:
                    //    %cmp = fcmp <cmpOp> float %x, 0  [since (constant<0)]
                    FC.setOperand(1, ConstantFP::get(Type::getFloatTy(FC.getContext()), 0));
                    FC.setOperand(0, prevInst[1 - i]->getOperand(0));
                    patternFound = true;
                }
                else
                {
                    //handling case:
                    //    %mul = fmul float %x, -5.000000e-01     ->   (x * constant) 
                    //    %sub = fsub float 0.000000e+00, %mul   ->  -(x * constant) 
                    //    %cmp = fcmp <cmpOp> float %mul, %sub
                    // into:
                    //    %cmp = fcmp <cmpOp> float 0, %x  [since (constant<0)]
                    FC.setOperand(0, ConstantFP::get(Type::getFloatTy(FC.getContext()), 0));
                    FC.setOperand(1, prevInst[1 - i]->getOperand(0));
                    patternFound = true;
                }
            }
            else
            {
                //handling case:
                //    %mul = fmul float %x, 5.000000e-01     ->   (x * constant)
                //    %sub = fsub float 0.000000e+00, %mul   ->  -(x * constant)
                //    %cmp = fcmp <cmpOp> float %sub, %mul
                if (i == 0)
                {
                    FC.setOperand(0, ConstantFP::get(Type::getFloatTy(FC.getContext()), 0));
                    FC.setOperand(1, prevInst[1 - i]->getOperand(0));
                    patternFound = true;
                }
                else
                {
                    //handling case:
                    //    %mul = fmul float %x, 5.000000e-01     ->   (x * constant)
                    //    %sub = fsub float 0.000000e+00, %mul   ->  -(x * constant)
                    //    %cmp = fcmp <cmpOp> float %mul, %sub

                    FC.setOperand(0, prevInst[1 - i]->getOperand(0));
                    FC.setOperand(1, ConstantFP::get(Type::getFloatTy(FC.getContext()), 0));
                    patternFound = true;
                }
            }
            break;
        }
    }
    return patternFound;
}

bool CustomUnsafeOptPass::visitFCmpInstFCmpSelOp(FCmpInst &FC)
{
    //  %17 = fcmp ole float %16, 0.000000e+00
    //  %18 = select i1 %17, float 0.000000e+00, float 1.000000e+00
    //  %19 = fsub float -0.000000e+00, %18
    //  %20 = fcmp ueq float %18, %19
    //         =>
    //  %20 = fcmp ole float %16, 0.000000e+00
    llvm::Instruction* fSubInst = llvm::dyn_cast<llvm::Instruction>( FC.getOperand(1) );
    if( fSubInst &&
        fSubInst->getOpcode() == Instruction::FSub )
    {
        ConstantFP *fSubConstant = dyn_cast<ConstantFP>(fSubInst->getOperand(0));

        llvm::Instruction* selectInst = llvm::dyn_cast<llvm::Instruction>( FC.getOperand(0) );

        if( selectInst &&
            selectInst->getOpcode() == Instruction::Select &&
            selectInst == llvm::dyn_cast<llvm::Instruction>( fSubInst->getOperand(1) ) &&
            fSubConstant &&
            fSubConstant->isZero())
        {
            ConstantFP *selectConstant1 = dyn_cast<ConstantFP>(selectInst->getOperand(1));
            ConstantFP *selectConstant2 = dyn_cast<ConstantFP>(selectInst->getOperand(2));

            llvm::Instruction* fCmpInst = llvm::dyn_cast<llvm::Instruction>( selectInst->getOperand(0) );

            if (fCmpInst &&
                fCmpInst->getOpcode() == Instruction::FCmp &&
                selectConstant1 && selectConstant2 &&
                selectConstant1->isZero() && !selectConstant2->isZero() )
            {
                FC.setOperand(0,fCmpInst->getOperand(0));
                FC.setOperand(1,fCmpInst->getOperand(1));
                if( FC.getPredicate() == FCmpInst::FCMP_UNE )
                {
                    FC.setPredicate(dyn_cast<FCmpInst>(fCmpInst)->getInversePredicate());
                }
                else
                {
                    FC.setPredicate(dyn_cast<FCmpInst>(fCmpInst)->getPredicate());
                }
                Stat_FcmpRemoved+=3;
                return true;
            }
        }
    }
    return false;
}

void CustomUnsafeOptPass::visitFCmpInst(FCmpInst &FC)
{
    bool patternFound = false;
    if (FC.use_empty())
    {
        return;
    }
    if(FC.getPredicate()==CmpInst::FCMP_UNO)
    {
        if (m_ctx->m_DriverInfo.IgnoreNan())
        {
            FC.replaceAllUsesWith(ConstantInt::getFalse(FC.getType()));
            FC.eraseFromParent();
            ++Stat_FcmpRemoved;
            patternFound = true;
        }
    }
    else if(FC.getPredicate()==CmpInst::FCMP_ORD)
    {
        if (m_ctx->m_DriverInfo.IgnoreNan())
        {
            FC.replaceAllUsesWith(ConstantInt::getTrue(FC.getType()));
            FC.eraseFromParent();
            ++Stat_FcmpRemoved;
            patternFound = true;
        }
    }
    else
    {
        patternFound = visitFCmpInstFCmpFAddOp(FC);
        if (!patternFound &&
            (FC.getPredicate() == FCmpInst::FCMP_UEQ ||
             FC.getPredicate() == FCmpInst::FCMP_UNE ))
        {
            patternFound = visitFCmpInstFCmpSelOp(FC);
        }

        if (!patternFound)
        {
            patternFound = visitFMulFCmpOp(FC);
        }
    }
    m_isChanged |= patternFound;
}

void CustomUnsafeOptPass::visitSelectInst(SelectInst &I)
{
    if (llvm::FCmpInst* cmpInst = llvm::dyn_cast<llvm::FCmpInst>(I.getOperand(0)))
    {
        if (dyn_cast<FCmpInst>(cmpInst)->getPredicate() == FCmpInst::FCMP_OEQ)
        {
            if (ConstantFP* cmpConstant = dyn_cast<ConstantFP>(cmpInst->getOperand(1)))
            {
                if (cmpConstant->isZeroValue())
                {
                    /*
                    %16 = fmul float %15, %0
                    %17 = fadd float %16, %14
                    %24 = fcmp oeq float %0, 0.000000e+00
                    %25 = select i1 % 24, float %14, float %17
                        to
                    %25 = %17

                    */
                    bool foundPattern = false;
                    llvm::Instruction* addInst = llvm::dyn_cast<llvm::Instruction>(I.getOperand(2)); // %17
                    if (addInst && addInst->getOpcode() == Instruction::FAdd)
                    {
                        for (uint j = 0; j < 2; j++)
                        {
                            llvm::Instruction* mulInst = llvm::dyn_cast<llvm::Instruction>(addInst->getOperand(j)); // %16
                            if (mulInst && mulInst->getOpcode() == Instruction::FMul &&
                                addInst->getOperand(1-j) == I.getOperand(1) )
                            {
                                for (uint k = 0; k < 2; k++)
                                {
                                    if (mulInst->getOperand(k) == cmpInst->getOperand(0))
                                    {
                                        I.replaceAllUsesWith(I.getOperand(2));
                                        foundPattern = true;
                                        break;
                                    }
                                }
                                if (foundPattern)
                                    break;
                            }
                        }
                    }
                }
                else if (cmpConstant->isExactlyValue(1.f))
                {
                    /*
                    %21 = fsub float %8, %14
                    %22 = fmul float %21, %0
                    %23 = fadd float %22, %14
                    %24 = fcmp oeq float %0, 1.000000e+00
                    %27 = select i1 %24, float %8, float %23
                        to
                    %27 = %23

                    */
                    bool foundPattern = false;
                    llvm::Instruction* addInst = llvm::dyn_cast<llvm::Instruction>(I.getOperand(2)); // %23
                    if (addInst && addInst->getOpcode() == Instruction::FAdd)
                    {
                        for (uint j = 0; j < 2; j++)
                        {
                            llvm::Instruction* mulInst = llvm::dyn_cast<llvm::Instruction>(addInst->getOperand(j)); // %22
                            if (mulInst && mulInst->getOpcode() == Instruction::FMul)
                            {
                                for (uint k = 0; k < 2; k++)
                                {
                                    llvm::Instruction* subInst = llvm::dyn_cast<llvm::Instruction>(mulInst->getOperand(k)); // %21
                                    if (subInst &&
                                        subInst->getOpcode() == Instruction::FSub &&
                                        subInst->getOperand(0) == I.getOperand(1) &&
                                        subInst->getOperand(1) == addInst->getOperand(1-j) &&
                                        mulInst->getOperand(1 - k) == cmpInst->getOperand(0))
                                    {
                                        I.replaceAllUsesWith(I.getOperand(2));
                                        foundPattern = true;
                                        break;
                                    }
                                }
                                if (foundPattern)
                                    break;
                            }
                        }
                    }
                }
            }
        }
    }
}

void CustomUnsafeOptPass::strengthReducePow(
    IntrinsicInst* intrin, Value* exponent)
{
    Value* src = intrin->getOperand(0);

    if (exponent == ConstantFP::get(exponent->getType(), 0.5))
    {
        // pow(x, 0.5) -> sqrt(x)
        llvm::Function* sqrtIntr = llvm::Intrinsic::getDeclaration(
            m_ctx->getModule(), Intrinsic::sqrt, src->getType());
        llvm::CallInst* sqrt = llvm::IntrinsicInst::Create(
            sqrtIntr, src, "", intrin);
        intrin->replaceAllUsesWith(sqrt);
        intrin->eraseFromParent();
    }
    else
    if (exponent == ConstantFP::get(exponent->getType(), 1.0))
    {
        intrin->replaceAllUsesWith(src);
        intrin->eraseFromParent();
    }
    else
    if (exponent == ConstantFP::get(exponent->getType(), 2.0))
    {
        // pow(x, 2.0) -> x * x
        Value* x2 = BinaryOperator::CreateFMul(src, src, "", intrin);
        intrin->replaceAllUsesWith(x2);
        intrin->eraseFromParent();
    }
    else
    if (exponent == ConstantFP::get(exponent->getType(), 3.0))
    {
        // pow(x, 3.0) -> x * x * x
        Value* x2 = BinaryOperator::CreateFMul(src, src, "", intrin);
        Value* x3 = BinaryOperator::CreateFMul(x2, src, "", intrin);
        intrin->replaceAllUsesWith(x3);
        intrin->eraseFromParent();
    }
    else
    if (exponent == ConstantFP::get(exponent->getType(), 4.0))
    {
        // pow(x, 4.0) -> (x*x) * (x*x)
        Value* x2 = BinaryOperator::CreateFMul(src, src, "", intrin);
        Value* x4 = BinaryOperator::CreateFMul(x2, x2, "", intrin);
        intrin->replaceAllUsesWith(x4);
        intrin->eraseFromParent();
    }
    else
    if (exponent == ConstantFP::get(exponent->getType(), 5.0))
    {
        // pow(x, 5.0) -> (x*x) * (x*x) * x
        Value* x2 = BinaryOperator::CreateFMul(src, src, "", intrin);
        Value* x4 = BinaryOperator::CreateFMul(x2, x2, "", intrin);
        Value* x5 = BinaryOperator::CreateFMul(x4, src, "", intrin);
        intrin->replaceAllUsesWith(x5);
        intrin->eraseFromParent();
    }
    else
    if (exponent == ConstantFP::get(exponent->getType(), 6.0))
    {
        // pow(x, 6.0) -> (x*x) * (x*x) * (x*x)
        Value* x2 = BinaryOperator::CreateFMul(src, src, "", intrin);
        Value* x4 = BinaryOperator::CreateFMul(x2, x2, "", intrin);
        Value* x6 = BinaryOperator::CreateFMul(x4, x2, "", intrin);
        intrin->replaceAllUsesWith(x6);
        intrin->eraseFromParent();
    }
    else
    if (exponent == ConstantFP::get(exponent->getType(), 8.0))
    {
        // pow(x, 8.0) -> ((x*x) * (x*x)) * ((x*x) * (x*x))
        Value* x2 = BinaryOperator::CreateFMul(src, src, "", intrin);
        Value* x4 = BinaryOperator::CreateFMul(x2, x2, "", intrin);
        Value* x8 = BinaryOperator::CreateFMul(x4, x4, "", intrin);
        intrin->replaceAllUsesWith(x8);
        intrin->eraseFromParent();
    }
    else
    if (IGC_IS_FLAG_ENABLED(EnablePowToLogMulExp))
    {
        // pow(x, y) -> exp2(log2(x) * y)
        Function* logf = Intrinsic::getDeclaration(
            m_ctx->getModule(), Intrinsic::log2, src->getType());
        Function* expf = Intrinsic::getDeclaration(
            m_ctx->getModule(), Intrinsic::exp2, src->getType());
        CallInst* logv = IntrinsicInst::Create(logf, src, "", intrin);
        Value* mulv = BinaryOperator::CreateFMul(logv, exponent, "", intrin);
        CallInst* expv = IntrinsicInst::Create(expf, mulv, "", intrin);
        intrin->replaceAllUsesWith(expv);
        intrin->eraseFromParent();
    }
}

void CustomUnsafeOptPass::visitCallInst(llvm::CallInst &I)
{
    if(llvm::IntrinsicInst* intr = dyn_cast<llvm::IntrinsicInst>(&I))
    {
        llvm::Intrinsic::ID ID = intr->getIntrinsicID();
        if(ID == llvm::Intrinsic::pow)
        {
            strengthReducePow(intr, intr->getOperand(1));
        }
        else if (ID == llvm::Intrinsic::sqrt)
        {
            // y*y = x if y = sqrt(x).
            for (auto iter = intr->user_begin(); iter != intr->user_end(); iter++)
            {
                if (llvm::Instruction* mul = dyn_cast<Instruction>(*iter))
                {
                    if (mul->getOpcode() == Instruction::FMul &&
                        mul->getOperand(0) == mul->getOperand(1))
                    {
                        mul->replaceAllUsesWith(intr->getOperand(0));
                    }
                }
            }
        }
    }
}

// Search for reassociation candidate.
static bool searchFAdd(Instruction *DefI, Instruction *UseI, unsigned &level)
{
    // Could search further, however we need to rewrite
    // instructions along the path. So limit this two
    // levels, which should cover common cases.
    if (level >= 2)
        return false;
    if (DefI->getParent() != UseI->getParent() || !DefI->hasOneUse() ||
        UseI->user_empty())
        return false;
    if (UseI->getOpcode() != Instruction::FAdd &&
        UseI->getOpcode() != Instruction::FSub)
        return false;

    // Swap operands such DefI is always the LHS in UseI.
    Value *Op = UseI->getOperand(1);
    bool IsFAdd = UseI->getOpcode() == Instruction::FAdd;
    if (DefI == Op)
    {
        if (IsFAdd)
        {
            cast<BinaryOperator>(UseI)->swapOperands();
            Op = UseI->getOperand(1);
        }
        else
        {
            return false;
        }
    }

    // The rhs is not mul, so could be folded into a mad.
    auto RHS = dyn_cast<Instruction>(Op);
    if (RHS && RHS->getOpcode() != Instruction::FMul)
        return true;

    // For simplicity, only allow the last level to be fsub.
    if (!IsFAdd)
        return false;

    return searchFAdd(UseI, UseI->user_back(), ++level);
}

// Match and re-associate arithmetic computation to emit more
// mad instructions. E.g.
//
// a * b + c * d +/- e -> MUL, MAD, ADD
//
// After reassociation, this becomes
//
// a * b +/- e + c * d -> MAD, MAD
//
void CustomUnsafeOptPass::reassociateMulAdd(Function &F)
{
    if (m_disableReorderingOpt)
    {
        return;
    }

    auto modMD = getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();
    if (!modMD->compOpt.MadEnable)
    {
        return;
    }

    using namespace PatternMatch;

    for (auto &BB : F.getBasicBlockList())
    {
        for (auto I = BB.begin(); I != BB.end(); /*Empty*/)
        {
            Instruction *Inst = &*I++;
            Value *A, *B, *C, *D;
            // Match Exp = A * B + C * D with a single use so that
            // it is benefical to fold one FSub/FAdd with A * B.
            if (match(Inst, m_OneUse(m_FAdd(m_FMul(m_Value(A), m_Value(B)),
                                            m_FMul(m_Value(C), m_Value(D))))))
            {
                Instruction *L0 = Inst->user_back();
                unsigned level = 0;
                if (searchFAdd(Inst, L0, level))
                {
                    Value *T0 = Inst->getOperand(0);
                    Value *T1 = Inst->getOperand(1);

                    // rewrite the expression tree
                    if (level == 0)
                    {
                        // t0 = A * B
                        // t1 = C * D
                        // t2 = t0 + t1  // Inst
                        // t3 = t2 - E   // L0
                        //
                        // as
                        //
                        // t0 = A * B
                        // t1 = C * D
                        // t2 = t0 + t1   // Inst
                        // ...
                        // t2n = t0 - E   // new Inst
                        // t3n = t2n + t1 // new L0
                        // t3  = t2 - E   // L0
                        IRBuilder<> Builder(L0);
                        Value *E = L0->getOperand(1);
                        auto OpKind = BinaryOperator::BinaryOps(L0->getOpcode());
                        Value *NewInst = Builder.CreateBinOp(OpKind, T0, E, Inst->getName());
                        Value *NewL0 = Builder.CreateFAdd(NewInst, T1, L0->getName());
                        L0->replaceAllUsesWith(NewL0);
                        m_isChanged = true;
                    }
                    else if (level == 1)
                    {
                        // t0 = A * B
                        // t1 = C * D
                        // t2 = E * F
                        // t3 = t0 + t1 // Inst
                        // t4 = t3 + t2 // L0
                        // t5 = t4 - G  // L1
                        //
                        // as
                        //
                        // t0 = A * B
                        // t1 = C * D
                        // t2 = E * F
                        // t3 = t0 + t1  // Inst
                        // t4 = t3 + t2  // L0
                        // ...
                        // t3n = t0 - G   // NewInst
                        // t4n = t3n + t2 // NewL0
                        // t5n = t4n + t1 // NewL1
                        // t5  = t4 - G   // L1
                        Instruction *L1 = L0->user_back();
                        IRBuilder<> Builder(L1);
                        Value *T2 = L0->getOperand(1);
                        Value *G = L1->getOperand(1);

                        auto OpKind = BinaryOperator::BinaryOps(L1->getOpcode());
                        Value *NewInst = Builder.CreateBinOp(OpKind, T0, G, Inst->getName());
                        Value *NewL0 = Builder.CreateFAdd(NewInst, T2, L0->getName());
                        Value *NewL1 = Builder.CreateFAdd(NewL0, T1, L1->getName());
                        L1->replaceAllUsesWith(NewL1);
                        m_isChanged = true;
                    }
                }
            }
        }
    }
}

// This pass looks for potential patterns where, if some value evaluates
// to zero, then a long chain of computation will be zero as well and
// we can just skip it (a so called 'early out').  For example:
//
// before:
//
// a = some value; // might be zero
// result = a * expensive_operation();
//
// after:
// 
// a = some value;
// if (a == 0)
//   result = 0;
// else
//   result = a * expensive_operation();
//
// Currently this is used to target d3d workloads
// 
class EarlyOutPatterns : public FunctionPass
{
public:
    static char ID;

    EarlyOutPatterns() : FunctionPass(ID)
    {
    }
    virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const
    {
        AU.addRequired<CodeGenContextWrapper>();
    }

    virtual bool runOnFunction(Function &F);

    virtual llvm::StringRef getPassName() const
    {
        return "EarlyOutPatterns";
    }
private:
    static bool processBlock(BasicBlock* BB);
	static bool canOptimizeSampleInst(SmallVector<Instruction*, 4> &Channels, GenIntrinsicInst *GII);
	static bool canOptimizeDotProduct(SmallVector<Instruction*, 4> &Values, Instruction *I);
	static bool DotProductMatch(const Instruction *I);
	static bool DotProductSourceMatch(const Instruction *I);
	static BasicBlock* tryFoldAndSplit(
		ArrayRef<Instruction*> Values,
		Instruction *Root,
		const unsigned FoldThreshold,
		const unsigned FoldThresholdMultiChannel,
		const unsigned RatioNeeded);
    static bool trackAddSources(BinaryOperator* addInst);
    static DenseSet<const Value*> tryAndFoldValues(ArrayRef<Instruction*> Values);
    static BasicBlock* SplitBasicBlock(Instruction* inst, const DenseSet<const Value*> &FoldedVals);
    static bool FoldsToZero(const Instruction* inst, const Value* use, const DenseSet<const Value*> &FoldedVals);
    static void MoveOutputToConvergeBlock(BasicBlock* divergeBlock, BasicBlock* convergeBlock);
    static bool EarlyOutBenefit(
		const Instruction* earlyOutInst,
		const DenseSet<const Value*> &FoldedVals,
		const unsigned int ratioNeeded);
	static void foldFromAdd(SmallVector<Instruction*, 4> &Values, Instruction *&NewInsertPoint);
	static Instruction* moveToDef(Instruction *Def, ArrayRef<Instruction*> Users);
	static bool isSplitProfitable(
		const Instruction *Root,
		ArrayRef<Instruction*> Values,
		const DenseSet<const Value*> &FoldedVals,
		// Number of instructions which needs to be folded in order for the optimization to be worth it
		const unsigned FoldThreshold,
		// For cases where we need to AND several channels we have a higher threshold
		const unsigned FoldThresholdMultiChannel,
		const unsigned RatioNeeded);
	static BasicBlock* cmpAndSplitAtPoint(
		Instruction *Root,
		ArrayRef<Instruction*> Values,
		const DenseSet<const Value*> &FoldedVals);
};

char EarlyOutPatterns::ID = 0;

FunctionPass* IGC::CreateEarlyOutPatternsPass()
{
    return new EarlyOutPatterns();
}

bool EarlyOutPatterns::runOnFunction(Function &F)
{
    auto pCtx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
    if (IGC_IS_FLAG_ENABLED(DisableEarlyOutPatterns) ||
        pCtx->m_DriverInfo.WaNOSNotResolved())
    {
        return false;
    }
    bool changed = false;
    for(auto BI = F.begin(), BE = F.end(); BI != BE;)
    {
        BasicBlock* currentBB = &(*BI);
        ++BI;
        changed |= processBlock(currentBB);
    }
    return changed;
}

// Calculates whether the given 'use' evaluates to zero given that 'inst' is known to
// evaluate to zero.
bool EarlyOutPatterns::FoldsToZero(const Instruction* inst, const Value* use, const DenseSet<const Value*> &FoldedVals)
{
	auto isZero = [](const Value* V)
	{
		return isa<ConstantFP>(V) && cast<ConstantFP>(V)->isZero();
	};

	auto geZero = [](const Value* V)
	{
		if (auto *CFP = dyn_cast<ConstantFP>(V))
		{
			auto &APF = CFP->getValueAPF();
			if (CFP->getType()->isDoubleTy())
				return APF.convertToDouble() >= 0.0;
			else if (CFP->getType()->isFloatTy())
				return APF.convertToFloat() >= 0.0f;
		}

		return false;
	};

    if(auto *binInst = dyn_cast<BinaryOperator>(use))
    {
		switch (binInst->getOpcode())
		{
		case Instruction::FMul:
			return true;
			// watch out for the zero in the denominator
		case Instruction::FDiv:
			return inst != binInst->getOperand(1);
		case Instruction::FSub:
			return isZero(binInst->getOperand(0)) ||
				   isZero(binInst->getOperand(1));
		default:
			return false;
		}
    }
	else if (auto *SI = dyn_cast<SelectInst>(use))
	{
		// Assuming %x is 0, if the other operand is also
		// 0 the result of the select must be 0 as well.

		// select i1 %p, float 0.0, float %x
		bool zero0 = isZero(SI->getTrueValue());
		// select i1 %p, float %x, float 0.0
		bool zero1 = isZero(SI->getFalseValue());

		if (zero0 || zero1)
			return true;

		// If we have previously visited this select with a
		// folded value, check the map and allow the
		// select to be folded.
		auto *OtherOp = (inst == SI->getTrueValue()) ?
			SI->getFalseValue() :
			SI->getTrueValue();

		return FoldedVals.count(OtherOp) != 0;
	}
	else if (auto *GII = dyn_cast<GenIntrinsicInst>(use))
	{
		// if x == 0
		switch (GII->getIntrinsicID())
		{
		// max(0, x) or max(x, 0) == 0
		case GenISAIntrinsic::GenISA_max:
			return isZero(GII->getArgOperand(0)) ||
				   isZero(GII->getArgOperand(1));
		case GenISAIntrinsic::GenISA_min:
			return geZero(GII->getArgOperand(0)) ||
				   geZero(GII->getArgOperand(1));
		// Useful in matching dp3_sat for Dragon Age
		case GenISAIntrinsic::GenISA_fsat:
			return true;
		default:
			return false;
		}
	}

    return false;
}

// Count the number of instruction in the new block created if the ratio of instruction duplicated
// by instruction skipped is greater than the threshold return false
bool EarlyOutPatterns::EarlyOutBenefit(
	const Instruction* earlyOutInst,
	const DenseSet<const Value*> &FoldedVals,
	const unsigned int ratioNeeded)
{
    auto* BB = earlyOutInst->getParent();

    unsigned int numberOfInstruction = 0;
    unsigned int numberOfInstructionDuplicated = 0;

    DenseSet<const Value*> instDuplicated;
    instDuplicated.insert(BB->getTerminator());

    for(auto it = BB->rbegin(); &(*it) != earlyOutInst; ++it)
    { 
        numberOfInstruction++;
        const Instruction* inst = &(*it);

        if (FoldedVals.count(inst) != 0)
            continue;

        bool instNeeded = false;

		// We can't throw away side effects
        if(inst->mayWriteToMemory())
        {
            instNeeded = true;
        }
        else
        {
            for (auto *UI : inst->users())
            {
                if (auto *useInst = dyn_cast<Instruction>(UI))
                {
					// We must also keep the instruction if its use has
					// escaped into another BB or, transitively, because
					// its user must be kept.
                    if(useInst->getParent() != BB || instDuplicated.count(useInst) != 0)
                    {
                        instNeeded = true;
                        break;
                    }
                }
            }
        }

        if(instNeeded)
        {
            bool noOp = false;
            if(inst->getOpcode() == Instruction::FAdd)
            {
				// x + 0 = x, should be folded so don't add it
				// to the count.
                if(FoldedVals.count(inst->getOperand(0)) != 0 ||
				   FoldedVals.count(inst->getOperand(1)) != 0)
                {
                    noOp = true;
                }
            }
            if(!noOp)
            {
                numberOfInstructionDuplicated++;
            }
            instDuplicated.insert(inst);
        }
    }

    return numberOfInstructionDuplicated * ratioNeeded <= numberOfInstruction;
}

void EarlyOutPatterns::foldFromAdd(SmallVector<Instruction*, 4> &Values, Instruction *&NewInsertPoint)
{
	// if the sample has only one channel
	if (Values.size() == 1)
	{
		// if it has one use which is a add then try to fold from the add
		if (Values[0]->hasOneUse())
		{
			BinaryOperator* bin = dyn_cast<BinaryOperator>(*Values[0]->user_begin());
			if (bin && bin->getOpcode() == Instruction::FAdd)
			{
				if (trackAddSources(bin))
				{
					// try to fold the result of the add instead of the sample_C
					Values[0] = bin;
					NewInsertPoint = bin;
				}
			}
		}
	}
}

Instruction* EarlyOutPatterns::moveToDef(Instruction *Def, ArrayRef<Instruction*> Users)
{
	Instruction *insertPoint = Def;
	for (auto it : Users)
	{
		// move all the users right after the def instruction for simplicity
		it->moveBefore(insertPoint->getNextNode());
		insertPoint = it;
	}

	return insertPoint;
}

bool EarlyOutPatterns::isSplitProfitable(
	const Instruction *Root,
	ArrayRef<Instruction*> Values,
	const DenseSet<const Value*> &FoldedVals,
	const unsigned FoldThreshold,
	const unsigned FoldThresholdMultiChannel,
	const unsigned RatioNeeded)
{
	const unsigned NumInstFolded = FoldedVals.size();

	const bool SplitProfitable =
		(NumInstFolded > FoldThreshold) &&
		// Check if we folded, we need a higher threshold if we have to check more channels
		(Values.size() == 1 || NumInstFolded > FoldThresholdMultiChannel) &&
		EarlyOutBenefit(Root, FoldedVals, RatioNeeded);

	return SplitProfitable;
}

// Once a candidate position 'Root' has been determined to be
// a profitable splitting point, generate the == 0 comparison
// and split the basic block at that point.
BasicBlock* EarlyOutPatterns::cmpAndSplitAtPoint(
	Instruction *Root,
	ArrayRef<Instruction*> Values,
	const DenseSet<const Value*> &FoldedVals)
{
    IRBuilder<> builder(Root->getNextNode());
    auto *splitCondition = cast<Instruction>(
        builder.CreateFCmpOEQ(Values[0], ConstantFP::get(Values[0]->getType(), 0.0)));

    for (unsigned int i = 1; i < Values.size(); i++)
    {
        Value* cmp = builder.CreateFCmpOEQ(Values[i], ConstantFP::get(Values[i]->getType(), 0.0));
        splitCondition = cast<Instruction>(builder.CreateAnd(splitCondition, cmp));
    }

    auto* BB = SplitBasicBlock(splitCondition, FoldedVals);
	return BB;
}

BasicBlock* EarlyOutPatterns::tryFoldAndSplit(
	ArrayRef<Instruction*> Values,
	Instruction *Root,
	const unsigned FoldThreshold,
	const unsigned FoldThresholdMultiChannel,
	const unsigned RatioNeeded)
{
	if (Values.empty())
		return nullptr;

	auto FoldedVals = tryAndFoldValues(Values);

	const bool SplitProfitable = isSplitProfitable(
		Root,
		Values,
		FoldedVals,
		FoldThreshold,
		FoldThresholdMultiChannel,
		RatioNeeded);

	return SplitProfitable ?
		cmpAndSplitAtPoint(Root, Values, FoldedVals) :
		nullptr;
}

bool EarlyOutPatterns::canOptimizeDotProduct(SmallVector<Instruction*, 4> &Values, Instruction *I)
{
	Values.push_back(I);
	return true;
}

// Matches the llvm instruction pattern we generate after decomposing
// a dot product.
bool EarlyOutPatterns::DotProductMatch(const Instruction *I)
{
	if (I->getOpcode() != Instruction::FAdd)
		return false;

    using namespace PatternMatch;

	Value *X1, *Y1, *Z1, *X2, *Y2, *Z2;

	// dp3

	return match(I,
		m_FAdd(
			m_FMul(m_Value(Z1), m_Value(Z2)),
			m_FAdd(
				m_FMul(m_Value(X1), m_Value(X2)),
				m_FMul(m_Value(Y1), m_Value(Y2)))));
}

// Does is a dot product pattern the source of this instruction?
bool EarlyOutPatterns::DotProductSourceMatch(const Instruction *I)
{
	if (auto *Src = dyn_cast<Instruction>(I->getOperand(0)))
		return DotProductMatch(Src);

	return false;
}

bool EarlyOutPatterns::canOptimizeSampleInst(SmallVector<Instruction*, 4> &Channels, GenIntrinsicInst *GII)
{
	auto ID = GII->getIntrinsicID();

	// -- Pattern, we are looking for sample instructions followed
	// by a large number of instructions which can be folded
	if (ID == GenISAIntrinsic::GenISA_sampleLptr ||
		ID == GenISAIntrinsic::GenISA_sampleLCptr ||
		ID == GenISAIntrinsic::GenISA_sampleptr)
	{
		bool canOptimize = false;

		for (auto I : GII->users())
		{
			if (auto *extract = dyn_cast<ExtractElementInst>(I))
			{
				if (GII->getParent() == extract->getParent() &&
					isa<ConstantInt>(extract->getIndexOperand()))
				{
					Channels.push_back(extract);
					canOptimize = true;
					continue;
				}
			}
			canOptimize = false;
			break;
		}

		if (ID == GenISAIntrinsic::GenISA_sampleLCptr)
		{
			if (Channels.size() != 1 || !cast<ConstantInt>(Channels[0]->getOperand(1))->isZero())
			{
				// for now allow multiple channels for everything except SampleLCptr
				// to reduce the scope
				canOptimize = false;
			}
		}

		// limit the number of channles to check to 3 for now
		if (Channels.size() > 3)
		{
			canOptimize = false;
		}

		return canOptimize;
	}

	return false;
}

bool EarlyOutPatterns::processBlock(BasicBlock* BB)
{
	bool Changed = false;
	bool BBSplit = true;

	// Each pattern below is given a bit to toggle on/off
	// to isolate the performance for each individual pattern.
	const bool SamplePatternEnable =
		(IGC_GET_FLAG_VALUE(EarlyOutPatternSelect) & 0x1) != 0;
	const bool DPMaxPatternEnable =
		(IGC_GET_FLAG_VALUE(EarlyOutPatternSelect) & 0x2) != 0;
	const bool DPFSatPatternEnable =
		(IGC_GET_FLAG_VALUE(EarlyOutPatternSelect) & 0x4) != 0;

	while (BBSplit)
	{
		BBSplit = false;
		for (auto &II : *BB)
		{
			SmallVector<Instruction*, 4> Values;
			bool OptCandidate = false;
			Instruction *Root = &II;

			unsigned FoldThreshold = 5;
			unsigned FoldThresholdMultiChannel = 10;
			unsigned RatioNeeded = 10;

			if (auto *SI = dyn_cast<SampleIntrinsic>(&II))
			{
				OptCandidate = SamplePatternEnable && canOptimizeSampleInst(Values, SI);

				if (!OptCandidate)
					continue;

				Root = moveToDef(SI, Values);
				foldFromAdd(Values, Root);
			}
			else if (auto *GII = dyn_cast<GenIntrinsicInst>(&II))
			{
				switch (GII->getIntrinsicID())
				{
				case GenISAIntrinsic::GenISA_max:
					OptCandidate = DPMaxPatternEnable &&
						DotProductSourceMatch(GII) && canOptimizeDotProduct(Values, &II);
					// Lower the ratio threshold for this case
					FoldThreshold = 9;
					RatioNeeded = 3;
					break;

				case GenISAIntrinsic::GenISA_fsat:
					OptCandidate = DPFSatPatternEnable &&
						DotProductSourceMatch(GII) && canOptimizeDotProduct(Values, &II);
					break;
				default:
					break;
				}
			}

			if (OptCandidate)
			{
				BB = tryFoldAndSplit(Values, Root,
					FoldThreshold, FoldThresholdMultiChannel, RatioNeeded);
				BBSplit = (BB != nullptr);

				if (BBSplit)
					break;
			}
		}

		Changed |= BBSplit;
	}

	return Changed;
}

bool EarlyOutPatterns::trackAddSources(BinaryOperator* addInst)
{
    for(unsigned int i = 0; i < 2; i++)
    {
        Value* source = addInst->getOperand(i);
        if(BinaryOperator* binSrc = dyn_cast<BinaryOperator>(source))
        {
            if(binSrc->getOpcode() == Instruction::FAdd)
            {
                if(trackAddSources(binSrc))
                {
                    continue;
                }
            }
        }
        else if(ExtractElementInst* ext = dyn_cast<ExtractElementInst>(source))
        {
            if(ConstantInt* index = dyn_cast<ConstantInt>(ext->getIndexOperand()))
            {
                if(index->isZero())
                {
                    if(GenIntrinsicInst* genIntr = dyn_cast<GenIntrinsicInst>(ext->getVectorOperand()))
                    {
                        GenISAIntrinsic::ID ID = genIntr->getIntrinsicID();
                        if(ID == GenISAIntrinsic::GenISA_sampleLCptr ||
                           ID == GenISAIntrinsic::GenISA_sampleptr)
                        {
                            continue;
                        }
                    }
                }
            }
        }
        // if any source is not a sample_LC or a add coming from sampleLC we shouldn't fold the value
        return false;
    }
    return true;
}

// Recursively walk the users of 'Values' and, under the assumption the the 'Values' are
// == 0, determine which other instructions could be folded away to 0 as well.
DenseSet<const Value*> EarlyOutPatterns::tryAndFoldValues(ArrayRef<Instruction*> Values)
{
	std::function<void(const Instruction*, DenseSet<const Value*> &)> fold =
		[&fold](const Instruction* inst, DenseSet<const Value*> &FoldedVals) -> void
	{
		for (auto UI : inst->users())
		{
			if (auto* useInst = dyn_cast<Instruction>(UI))
			{
				if (useInst->getParent() == inst->getParent())
				{
					if (FoldsToZero(inst, useInst, FoldedVals))
					{
						FoldedVals.insert(useInst);
						fold(useInst, FoldedVals);
					}
				}
			}
		}
	};

	// try to fold assuming all the channels are 0.f
    // right now only fold with 0, to replace with a map in case we want to
	// support more values
	DenseSet<const Value*> FoldedVals;
	for (auto I : Values)
	{
		fold(I, FoldedVals);
	}

	return FoldedVals;
}

// return the new block where the code after inst was moved
BasicBlock* EarlyOutPatterns::SplitBasicBlock(Instruction* inst, const DenseSet<const Value*> &FoldedVals)
{
    IRBuilder<> builder(inst->getContext());
    BasicBlock* currentBB = inst->getParent();
    BasicBlock* endifBlock = BasicBlock::Create(inst->getContext(), VALUE_NAME("EO_endif"), currentBB->getParent(), currentBB->getNextNode());
    BasicBlock* elseBlock = BasicBlock::Create(inst->getContext(), VALUE_NAME("EO_else"), currentBB->getParent(), currentBB->getNextNode());

    currentBB->replaceSuccessorsPhiUsesWith(endifBlock);

    // copy the end of the block to the else part
    elseBlock->getInstList().splice(elseBlock->begin(), 
        currentBB->getInstList(), 
        inst->getNextNode()->getIterator(), 
        currentBB->getTerminator()->getIterator());
    endifBlock->getInstList().splice(endifBlock->begin(), currentBB->getInstList(), currentBB->getTerminator());
    if(isa<ReturnInst>(endifBlock->getTerminator()))
    {
        MoveOutputToConvergeBlock(elseBlock, endifBlock);
    }
    builder.SetInsertPoint(elseBlock);
    builder.CreateBr(endifBlock);
    // split the blocks
    ValueToValueMapTy VMap;
    BasicBlock* ifBlock = CloneBasicBlock(elseBlock, VMap);
    ifBlock->setName(VALUE_NAME("EO_IF"));
    currentBB->getParent()->getBasicBlockList().insertAfter(currentBB->getIterator(), ifBlock);
    for(auto II = ifBlock->begin(), IE = ifBlock->end(); II != IE; ++II)
    {
        for(unsigned op = 0, E = II->getNumOperands(); op != E; ++op)
        {
            Value *Op = II->getOperand(op);
            ValueToValueMapTy::iterator It = VMap.find(Op);
            if(It != VMap.end())
                II->setOperand(op, It->second);
        }
    }
    // create phi instruction
    for(auto II = elseBlock->begin(), IE = elseBlock->end(); II != IE; ++II)
    {
        PHINode* newPhi = nullptr;
        for(auto UI = II->user_begin(), UE = II->user_end(); UI != UE; ++UI)
        {
            if(Instruction* useInst = dyn_cast<Instruction>(*UI))
            {
                if(useInst->getParent() != elseBlock)
                {
                    newPhi = PHINode::Create(II->getType(), 2, "", &(*endifBlock->begin()));
                    II->replaceUsesOutsideBlock(newPhi, elseBlock);
                    newPhi->addIncoming(&(*II), elseBlock);
                    newPhi->addIncoming(VMap[&(*II)], ifBlock);
                    break;
                }
            }
        }
    }
    // replace the folded values with 0
    for(auto it : FoldedVals)
    {
        VMap[it]->replaceAllUsesWith(ConstantFP::get(it->getType(), 0.0));
    }

    // branching
    builder.SetInsertPoint(currentBB);
    builder.CreateCondBr(inst, ifBlock, elseBlock);

    return elseBlock;
}

void EarlyOutPatterns::MoveOutputToConvergeBlock(BasicBlock* divergeBlock, BasicBlock* convergeBlock)
{
    for(auto it = divergeBlock->begin(), ie = divergeBlock->end(); it != ie; )
    {
        Instruction* I = &(*it);
        ++it;
        if(GenIntrinsicInst* intr = dyn_cast<GenIntrinsicInst>(I))
        {
            auto id = intr->getIntrinsicID();
            if (id == GenISAIntrinsic::GenISA_OUTPUT ||
                id == GenISAIntrinsic::GenISA_OUTPUTPSMASK)
            {
                intr->moveBefore(convergeBlock->getTerminator());
            }
        }
    }
}

class LowerFma : public FunctionPass
{
public:
    static char ID;

    LowerFma() : FunctionPass(ID) { }

    void getAnalysisUsage(llvm::AnalysisUsage& AU) const
    {
        AU.setPreservesCFG();
    }

    StringRef getPassName() const
    {
        return "LowerFma";
    }

    bool runOnFunction(Function& F);
};

char LowerFma::ID = 0;

FunctionPass* IGC::CreateLowerFmaPass()
{
    return new LowerFma();
}

bool LowerFma::runOnFunction(Function& F)
{
    bool changed = false;
    for (auto BI = F.begin(), BE = F.end(); BI != BE; BI++)
    {
        for (auto II = BI->begin(), IE = BI->end(); II != IE; II++)
        {
            IntrinsicInst* fmad = dyn_cast<IntrinsicInst>(II);
            if (fmad && fmad->getIntrinsicID() == Intrinsic::fma)
            {
                changed = true;
                IRBuilder<> irb(fmad);
                Value* v = irb.CreateFMul(fmad->getArgOperand(0),
                    fmad->getArgOperand(1));
                v = irb.CreateFAdd(v, fmad->getArgOperand(2));
                fmad->replaceAllUsesWith(v);
            }
        }
    }
    return changed;
}

