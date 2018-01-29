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

#include "common/LLVMWarningsPush.hpp"
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/LoopUtils.h>
#include "common/LLVMWarningsPop.hpp"

#include "common/LLVMUtils.h"

#include "Compiler/CISACodeGen/ShaderCodeGen.hpp"
#include "Compiler/CustomLoopOpt.hpp"
#include "Compiler/IGCPassSupport.h"
#include "Compiler/MetaDataUtilsWrapper.h"


using namespace llvm;
using namespace IGC;

#define PASS_FLAG     "igc-custom-loop-opt"
#define PASS_DESC     "IGC Custom Loop Opt"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(CustomLoopVersioning, PASS_FLAG, PASS_DESC, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(CodeGenContextWrapper)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper);
IGC_INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass);
IGC_INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass);
IGC_INITIALIZE_PASS_DEPENDENCY(LCSSAWrapperPass)
IGC_INITIALIZE_PASS_END(CustomLoopVersioning, PASS_FLAG, PASS_DESC, PASS_CFG_ONLY, PASS_ANALYSIS)

char CustomLoopVersioning::ID = 0;

CustomLoopVersioning::CustomLoopVersioning() : FunctionPass(ID)
{
    initializeCustomLoopVersioningPass(*PassRegistry::getPassRegistry());
}

bool CustomLoopVersioning::isCBLoad(Value* val, unsigned& bufId, unsigned& offset)
{
    LoadInst* ld = dyn_cast<LoadInst>(val);
    if (!ld)
        return false;

    unsigned as = ld->getPointerAddressSpace();
    bool directBuf;
    BufferType bufType = DecodeAS4GFXResource(as, directBuf, bufId);
    if (!(bufType == CONSTANT_BUFFER && directBuf))
        return false;

    Value* ptr = ld->getPointerOperand();
    if (IntToPtrInst* itop = dyn_cast<IntToPtrInst>(ptr))
    {
        ConstantInt* ci = dyn_cast<ConstantInt>(
            itop->getOperand(0));
        if (ci)
        {
            offset = int_cast<unsigned>(ci->getZExtValue());
            return true;
        }
    }
    if (ConstantExpr* itop = dyn_cast<ConstantExpr>(ptr))
    {
        if (itop->getOpcode() == Instruction::IntToPtr)
        {
            offset = int_cast<unsigned>(
                cast<ConstantInt>(itop->getOperand(0))->getZExtValue());
            return true;
        }
    }
    return false;
}

bool CustomLoopVersioning::runOnFunction(Function& F)
{
    // Skip non-kernel function.                                                  
    IGCMD::MetaDataUtils *mdu = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    auto FII = mdu->findFunctionsInfoItem(&F);
    if (FII == mdu->end_FunctionsInfo())
        return false;

    m_cgCtx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
    m_LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    m_DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    m_function = &F;

    bool changed = false;
    for (auto& LI : *m_LI)
    {
        Loop* L = &(*LI);

        // only check while loop with single BB loop body
        if (L->isSafeToClone() && L->getLoopDepth() == 1 &&
            L->getNumBlocks() == 1 && L->getNumBackEdges() == 1 &&
            L->getHeader() == L->getExitingBlock() &&
            L->getLoopPreheader() && L->isLCSSAForm(*m_DT))
        {
            changed = processLoop(L);
            if (changed)
                break;
        }
    }

    if (changed)
    {
        m_cgCtx->getModuleMetaData()->psInfo.hasVersionedLoop = true;
        DumpLLVMIR(m_cgCtx, "customloop");
    }
    return changed;
}

//
// float t = ...;
// float nextT = t * CB_Load;
// [loop] while (t < loop_range_y)
// {
//     float val0 = max(t, loop_range_x);
//     float val1 = min(nextT, loop_range_y);
//     ...
//     t = nextT;
//     nextT *= CB_Load;
// }
//
// pre_header:
//   %cb = load float, float addrspace(65538)* ...
//   %nextT_start = fmul float %t_start, %cb
//
// loop_header:
//   %t = phi float [ %t_start, %then409 ], [ %nextT, %break_cont ]
//   %nextT = phi float [ %nextT_start, %then409 ], [ %res_s588, %break_cont ]
//   %cond = fcmp ult float %t, %loop_range_y
//   br i1 %cond, label %break_cont, label %after_loop
//
// loop_body:
//   %206 = call float @genx.GenISA.max.f32(float %loop_range_x, float %t)
//   %207 = call float @genx.GenISA.min.f32(float %loop_range_y, float %nextT)
//   ...
//   %258 = load float, float addrspace(65538)* ...
//   %res_s588 = fmul float %nextT, %258
//   br label %loop_entry
//   
// 
bool CustomLoopVersioning::detectLoop(Loop* loop,
    Value* &var_range_x, Value* &var_range_y,
    LoadInst* &var_CBLoad_preHdr,
    Value* &var_t_preHdr,
    Value* &var_nextT_preHdr)
{
    BasicBlock* preHdr = loop->getLoopPreheader();
    BasicBlock* header = loop->getHeader();
    BasicBlock* body = loop->getLoopLatch();

    Instruction* i0 = body->getFirstNonPHI();
    Instruction* i1 = GetNextInstruction(i0);

    GenIntrinsicInst* imax = dyn_cast<GenIntrinsicInst>(i0);
    GenIntrinsicInst* imin = i1 ? dyn_cast<GenIntrinsicInst>(i1) : nullptr;

    if (!(imax && imax->getIntrinsicID() == GenISAIntrinsic::GenISA_max &&
          imin && imin->getIntrinsicID() == GenISAIntrinsic::GenISA_min))
    {
        return false;
    }

    GenIntrinsicInst* interval_x = dyn_cast<GenIntrinsicInst>(
        imax->getArgOperand(0), GenISAIntrinsic::GenISA_max);
    GenIntrinsicInst* interval_y = dyn_cast<GenIntrinsicInst>(
        imin->getArgOperand(0), GenISAIntrinsic::GenISA_min);

    if (!interval_x || !interval_y)
    {
        return false;
    }
    var_range_x = interval_x;
    var_range_y = interval_y;

    PHINode* var_t;
    PHINode* var_nextT;

    var_t = dyn_cast<PHINode>(imax->getArgOperand(1));
    var_nextT = dyn_cast<PHINode>(imin->getArgOperand(1));
    if (var_t == nullptr || var_nextT == nullptr)
    {
        return false;
    }

    if (var_t->getParent() != header || var_nextT->getParent() != header)
    {
        return false;
    }

    // check for "nextT = t * CB_Load" before loop
    BinaryOperator* fmul = dyn_cast<BinaryOperator>(
        var_nextT->getIncomingValueForBlock(preHdr));
    if (!fmul)
    {
        return false;
    }
    if (fmul->getOperand(0) !=
        var_t->getIncomingValueForBlock(preHdr))
    {
        return false;
    }
    var_t_preHdr = var_t->getIncomingValueForBlock(preHdr);
    var_nextT_preHdr = var_nextT->getIncomingValueForBlock(preHdr);

    unsigned bufId, cbOffset;
    if (!isCBLoad(fmul->getOperand(1), bufId, cbOffset))
    {
        return false;
    }
    var_CBLoad_preHdr = cast<LoadInst>(fmul->getOperand(1));

    // check for "t = nextT" inside loop
    if (var_t->getIncomingValueForBlock(body) != var_nextT)
    {
        return false;
    }

    fmul = dyn_cast<BinaryOperator>(
        var_nextT->getIncomingValueForBlock(body));
    if (!fmul)
    {
        return false;
    }

    // check for "nextT *= CB_Load" inside loop
    Value* src0 = fmul->getOperand(0);
    if (src0 != var_nextT)
    {
        return false;
    }

    unsigned bufId2, cbOffset2;
    if (!isCBLoad(fmul->getOperand(1), bufId2, cbOffset2))
    {
        return false;
    }
    if (bufId != bufId2 || cbOffset != cbOffset2)
    {
        return false;
    }

    BranchInst* br = cast<BranchInst>(body->getTerminator());
    if (!br->isConditional())
    {
        return false;
    }

    // check for "while (t < loop_range_y)"
    FCmpInst* fcmp = dyn_cast<FCmpInst>(br->getCondition());
    if (!fcmp || fcmp->getOperand(0) != var_nextT)
    {
        return false;
    }

    if (fcmp->getOperand(1) != interval_y)
    {
        return false;
    }

    return true;
}

// while (t < loop_range_y)
//     float val0 = max(t, loop_range_x);
//     float val1 = min(nextT, loop_range_y);
// -->
// while (t < loop_range_x)
//     float val0 = loop_range_x;
//     float val1 = nextT;
void CustomLoopVersioning::rewriteLoopSeg1(Loop* loop,
    Value* interval_x, Value* interval_y)
{
    BasicBlock* header = loop->getHeader();
    BasicBlock* body = loop->getLoopLatch();

    BranchInst* br = cast<BranchInst>(header->getTerminator());
    FCmpInst* fcmp = dyn_cast<FCmpInst>(br->getCondition());
    assert(fcmp && fcmp->getOperand(1) == interval_y);

    fcmp->setOperand(1, interval_x);

    Instruction* i0 = body->getFirstNonPHI();
    Instruction* i1 = GetNextInstruction(i0);

    GenIntrinsicInst* imax = cast<GenIntrinsicInst>(i0);
    GenIntrinsicInst* imin = cast<GenIntrinsicInst>(i1);
    assert(imax && imin);

    imax->replaceAllUsesWith(interval_x);
    imin->replaceAllUsesWith(imin->getArgOperand(1));
}

void CustomLoopVersioning::hoistSeg2Invariant(Loop* loop,
    Instruction* fmul, Value* cbLoad)
{
    BasicBlock* preHdr = loop->getLoopPreheader();
    BasicBlock* body = loop->getLoopLatch();

    // detecting loop invariant and move it to header:
    //   %211 = call float @llvm.fabs.f32(float %210)
    //   %212 = call float @llvm.log2.f32(float %211)
    //   %res_s465 = fmul float %165, %212
    //   %213 = call float @llvm.exp2.f32(float %res_s465)
    IntrinsicInst* intrin_abs = nullptr;
    IntrinsicInst* intrin_log2 = nullptr;
    Instruction* fmul_log2 = nullptr;
    Value* fmul_log2_opnd = nullptr;

    for (auto* UI : fmul->users())
    {
        IntrinsicInst* intrin = dyn_cast<IntrinsicInst>(UI);
        if (intrin->getIntrinsicID() == Intrinsic::fabs &&
            intrin->getNumUses() == 1)
        {
            intrin_abs = intrin;
            break;
        }
    }

    if (intrin_abs && intrin_abs->getParent() == body)
    {
        IntrinsicInst* intrin = dyn_cast<IntrinsicInst>(
            *intrin_abs->users().begin());
        if (intrin &&
            intrin->getIntrinsicID() == Intrinsic::log2 &&
            intrin->getNumUses() == 1)
        {
            intrin_log2 = intrin;
        }
    }

    if (intrin_log2 && intrin_log2->getParent() == body)
    {
        Instruction* fmul = dyn_cast<Instruction>(
            *intrin_log2->users().begin());
        if (fmul &&
            fmul->getOpcode() == Instruction::FMul &&
            fmul->getNumUses() == 1)
        {
            unsigned id = fmul->getOperand(0) == intrin_log2 ? 1 : 0;
            // make sure another operand is coming from out of loop
            Instruction* i = dyn_cast<Instruction>(fmul->getOperand(id));
            if (i && !loop->contains(i->getParent()))
            {
                fmul_log2 = fmul;
                fmul_log2_opnd = fmul->getOperand(id);
            }
        }
    }

    if (fmul_log2 && fmul_log2->getParent() == body)
    {
        IntrinsicInst* intrin = dyn_cast<IntrinsicInst>(
            *fmul_log2->users().begin());
        if (intrin &&
            intrin->getIntrinsicID() == Intrinsic::exp2)
        {
            IRBuilder<> irb(preHdr->getFirstNonPHI());

            Function* flog =
                Intrinsic::getDeclaration(m_function->getParent(),
                    llvm::Intrinsic::log2, intrin_log2->getType());
            Function* fexp =
                Intrinsic::getDeclaration(m_function->getParent(),
                    llvm::Intrinsic::exp2, intrin_log2->getType());
            Value* v = irb.CreateCall(flog, cbLoad);
            v = irb.CreateFMul(fmul_log2_opnd, v);
            v = irb.CreateCall(fexp, v);
            intrin->replaceAllUsesWith(v);
        }
    }
    fmul->replaceAllUsesWith(cbLoad);
}

// while (t < loop_range_y)
//     float val0 = max(t, loop_range_x);
//     float val1 = min(nextT, loop_range_y);
// -->
// while (t < loop_range_y/CB_Load)
//     float val0 = t;
//     float val1 = next;
void CustomLoopVersioning::rewriteLoopSeg2(Loop* loop,
    Value* interval_y, Value* cbLoad)
{
    BasicBlock* header = loop->getHeader();
    BasicBlock* body = loop->getLoopLatch();

    BranchInst* br = cast<BranchInst>(header->getTerminator());
    FCmpInst* fcmp = dyn_cast<FCmpInst>(br->getCondition());
    assert(fcmp && fcmp->getOperand(1) == interval_y);

    Value* v = BinaryOperator::Create(Instruction::FDiv,
        interval_y, cbLoad, "", fcmp);
    fcmp->setOperand(1, v);

    Instruction* i0 = body->getFirstNonPHI();
    Instruction* i1 = GetNextInstruction(i0);

    GenIntrinsicInst* imax = cast<GenIntrinsicInst>(i0);
    GenIntrinsicInst* imin = cast<GenIntrinsicInst>(i1);
    assert(imax && imin);

    // find
    //   %206 = call float @genx.GenISA.max.f32()
    //   %207 = call float @genx.GenISA.min.f32()
    //   %209 = fdiv float 1.000000e+00, % 206
    //   %210 = fmul float %207, % 209
    Instruction* fmul = nullptr;
    for (auto* max_Users : imax->users())
    {
        if (Instruction* fdiv = dyn_cast<BinaryOperator>(max_Users))
        {
            if (ConstantFP *cf = dyn_cast<ConstantFP>(fdiv->getOperand(0)))
            {
                if (cf->isExactlyValue(1.0))
                {
                    for (auto* UI : fdiv->users())
                    {
                        if ((fmul = dyn_cast<BinaryOperator>(UI)))
                        {
                            if (fmul->getOperand(0) == imin ||
                                (fmul->getOperand(1) == imin &&
                                    fmul->getParent() == body))
                            {
                                // find val1/val0
                                hoistSeg2Invariant(loop, fmul, cbLoad);

                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    imax->replaceAllUsesWith(imax->getArgOperand(1));
    imin->replaceAllUsesWith(imin->getArgOperand(1));
}

//     float val0 = max(t, loop_range_x);
//     float val1 = min(nextT, loop_range_y);
// -->
//     float val0 = t;
//     float val1 = loop_range_y;
void CustomLoopVersioning::rewriteLoopSeg3(BasicBlock* bb,
    Value* interval_y)
{
    Instruction* i0 = bb->getFirstNonPHI();
    Instruction* i1 = GetNextInstruction(i0);

    GenIntrinsicInst* imax = cast<GenIntrinsicInst>(i0);
    GenIntrinsicInst* imin = cast<GenIntrinsicInst>(i1);
    assert(imax && imin);

    imax->replaceAllUsesWith(imax->getArgOperand(1));
    imin->replaceAllUsesWith(interval_y);

    auto II = bb->begin();
    auto IE = BasicBlock::iterator(bb->getFirstNonPHI());

    while (II != IE)
    {
        PHINode* PN = cast<PHINode>(II);

        assert(PN->getNumIncomingValues() == 2);
        for (unsigned i = 0; i < PN->getNumIncomingValues(); i++)
        {
            if (PN->getIncomingBlock(i) != bb)
            {
                PN->replaceAllUsesWith(PN->getIncomingValue(i));
            }
        }
        ++II;
        PN->eraseFromParent();
    }
}

void CustomLoopVersioning::linkLoops(
    Loop* loopSeg1, Loop* loopSeg2,
    BasicBlock* afterLoop)
{
    // we are handling do/while loop
    assert(loopSeg1->getHeader() == loopSeg1->getLoopLatch());
    assert(loopSeg2->getHeader() == loopSeg2->getLoopLatch());

    BasicBlock* seg1Body = loopSeg1->getLoopLatch();
    BasicBlock* seg2PreHdr = loopSeg2->getLoopPreheader();
    BasicBlock* seg2Body = loopSeg2->getLoopLatch();

    BranchInst* br = cast<BranchInst>(seg1Body->getTerminator());
    unsigned idx = br->getSuccessor(0) == afterLoop ? 0 : 1;
    br->setSuccessor(idx, loopSeg2->getLoopPreheader());

    auto II_1 = seg1Body->begin(), II_2 = seg2Body->begin();
    auto IE_2 = BasicBlock::iterator(seg2Body->getFirstNonPHI());

    for (; II_2 != IE_2; ++II_2, ++II_1)
    {
        PHINode* PN2 = cast<PHINode>(II_2);
        PHINode* PN1 = cast<PHINode>(II_1);
        Value* liveOut = nullptr;

        for (unsigned i = 0; i < PN1->getNumIncomingValues(); i++)
        {
            if (PN1->getIncomingBlock(i) == seg1Body)
            {
                liveOut = PN1->getIncomingValue(i);
                break;
            }
        }

        assert(liveOut != nullptr);
        for (unsigned i = 0; i < PN2->getNumIncomingValues(); i++)
        {

            if (PN2->getIncomingBlock(i) != seg2Body)
            {
                PN2->setIncomingValue(i, liveOut);
                PN2->setIncomingBlock(i, seg2PreHdr);
            }
        }
    }

}

bool CustomLoopVersioning::processLoop(Loop* loop)
{
    Value* var_range_x;
    Value* var_range_y;
    LoadInst* var_CBLoad_preHdr;
    Value* var_t_preHdr;
    Value* var_nextT_preHdr;
    bool found = false;

    found = detectLoop(loop, var_range_x, var_range_y,
        var_CBLoad_preHdr, var_t_preHdr, var_nextT_preHdr);

    if (!found)
        return false;

    const SmallVectorImpl<Instruction*>& liveOut =
        llvm::findDefsUsedOutsideOfLoop(loop);

    BasicBlock* preHdr = loop->getLoopPreheader();

    // apply the transformation
    BasicBlock* PH = llvm::SplitBlock(preHdr, preHdr->getTerminator(), m_DT, m_LI);

    // create loop seg 1 and insert before orig loop
    SmallVector<BasicBlock *, 8> seg1Blocks;
    Loop* loopSeg1 = llvm::cloneLoopWithPreheader(
        PH, preHdr, loop, m_vmapToSeg1, ".seg1", m_LI, m_DT, seg1Blocks);
    llvm::remapInstructionsInBlocks(seg1Blocks, m_vmapToSeg1);

    // create the check for fast loop
    // if (CB_Load > 1.0 &&
    //     loop_range_x * CB_Load < loop_range_y)
    //     fast version;
    // else
    //     orig version;
    preHdr->getTerminator()->eraseFromParent();

    IRBuilder<> irb(preHdr);

    Value* cond0 = irb.CreateFCmpOGT(
        var_CBLoad_preHdr, ConstantFP::get(irb.getFloatTy(), 1.0));

    Value* cond1 = irb.CreateFCmpOLT(
        irb.CreateFMul(var_range_x, var_CBLoad_preHdr),
        var_range_y);


    irb.CreateCondBr(irb.CreateAnd(cond0, cond1),
        loopSeg1->getLoopPreheader(),
        loop->getLoopPreheader());

    BasicBlock* afterLoop = loop->getExitBlock();
    assert(afterLoop && "No single successor to loop exit block");

    // create loop seg 2 and insert before orig loop (after loop seg 1)
    SmallVector<BasicBlock *, 8> seg2Blocks;
    Loop* loopSeg2 = llvm::cloneLoopWithPreheader(
        PH, loopSeg1->getHeader(), loop, m_vmapToSeg2, ".seg2", m_LI, m_DT, seg2Blocks);
    llvm::remapInstructionsInBlocks(seg2Blocks, m_vmapToSeg2);

    // rewrite loop seg 1
    rewriteLoopSeg1(loopSeg1, var_range_x, var_range_y);

    // link loop seg1 to loop seg2
    linkLoops(loopSeg1, loopSeg2, afterLoop);

    // create seg3 after seg2 before changing loop2 body
    SmallVector<BasicBlock *, 8> seg3Blocks;
    Loop* loopSeg3 = llvm::cloneLoopWithPreheader(
        PH, loopSeg2->getHeader(), loop, m_vmapToSeg3, ".seg3", m_LI, m_DT, seg3Blocks);
    llvm::remapInstructionsInBlocks(seg3Blocks, m_vmapToSeg3);
    BasicBlock* bbSeg3 = loopSeg3->getLoopLatch();

    // rewrite loop seg2
    rewriteLoopSeg2(loopSeg2, var_range_y, var_CBLoad_preHdr);

    // link seg2 -> seg3 -> after_loop
    linkLoops(loopSeg2, loopSeg3, afterLoop);

    bbSeg3->getTerminator()->eraseFromParent();
    BranchInst::Create(afterLoop, bbSeg3);

    rewriteLoopSeg3(bbSeg3, var_range_y);

    addPhiNodes(liveOut, loopSeg1, loopSeg2, bbSeg3, loop);

    return true;
}

void CustomLoopVersioning::addPhiNodes(
    const SmallVectorImpl<Instruction*> &liveOuts,
    Loop* loopSeg1, Loop* loopSeg2, BasicBlock* bbSeg3, Loop* origLoop)
{
    BasicBlock* phiBB = origLoop->getExitBlock();
    assert(phiBB && "No single successor to loop exit block");

    for (auto* Inst : liveOuts)
    {
        Value* seg3Val = m_vmapToSeg3[Inst];
        PHINode* phi;

        phi = PHINode::Create(Inst->getType(), 2, "", &phiBB->front());
        SmallVector<Instruction*, 8> instToDel;
        for (auto* User : Inst->users())
        {
            PHINode* pu = dyn_cast<PHINode>(User);
            if (pu && pu->getParent() == phiBB)
            {
                // replace LCSSA phi with newly created phi node
                pu->replaceAllUsesWith(phi);
                instToDel.push_back(pu);
            }
        }
        for (auto* I : instToDel)
        {
            I->eraseFromParent();
        }
        phi->addIncoming(seg3Val, bbSeg3);
        phi->addIncoming(Inst, origLoop->getExitingBlock());
    }
}
