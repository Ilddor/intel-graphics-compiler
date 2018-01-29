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
#include "Compiler/CISACodeGen/CheckInstrTypes.hpp"
#include "Compiler/CISACodeGen/helper.h"
#include "Compiler/CodeGenPublic.h"

#include "common/debug/Debug.hpp"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include "common/LLVMWarningsPop.hpp"
#include "GenISAIntrinsics/GenIntrinsicInst.h"

using namespace llvm;
using namespace IGC;
using namespace GenISAIntrinsic;

#define PASS_FLAG "CheckInstrTypes"
#define PASS_DESCRIPTION "Check individual type of instructions"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS true
IGC_INITIALIZE_PASS_BEGIN(CheckInstrTypes, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_END(CheckInstrTypes, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char CheckInstrTypes::ID = 0;

CheckInstrTypes::CheckInstrTypes(IGC::SInstrTypes* instrList) : FunctionPass(ID), g_InstrTypes(instrList)
{
    initializeLoopInfoWrapperPassPass(*PassRegistry::getPassRegistry());	
	initializeCheckInstrTypesPass(*PassRegistry::getPassRegistry());

    instrList->CorrelatedValuePropagationEnable = false;
    instrList->hasLoop = false;
    instrList->hasMultipleBB = false;
    instrList->hasCmp = false;
    instrList->hasSwitch = false;
    instrList->hasPhi = false;
    instrList->hasLoadStore = false;
    instrList->hasCall = false;
    instrList->hasIndirectCall = false;
    instrList->hasIndirectBranch = false;
    instrList->hasFunctionAddressTaken = false;
    instrList->hasSel = false;
    instrList->hasPointer = false;
    instrList->hasLocalLoadStore = false;
    instrList->hasSubroutines = false;
    instrList->hasPrimitiveAlloca = false;
    instrList->hasNonPrimitiveAlloca = false;
    instrList->hasBuiltin = false;
    instrList->psHasSideEffect = false;
    instrList->hasDebugInfo = false;
    instrList->hasAtomics = false;
    instrList->hasSampleLinLoop = false;
    instrList->hasDiscard = false;
    instrList->mayHaveIndirectOperands = false;
	instrList->numSample = 0;
	instrList->numBB = 0;
	instrList->numLoopInsts = 0;
    instrList->numOfLoop = 0;
    instrList->numInsts = 0;
	instrList->sampleCmpToDiscardOptimizationPossible = false;
	instrList->sampleCmpToDiscardOptimizationSlot = 0;
}

void CheckInstrTypes::SetLoopFlags(Function &F)
{
    LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();

    if (!(LI->empty()))
    {
        // find how many instructions are used in loop
        for (auto it = LI->begin(); it != LI->end(); it++)
        {
            g_InstrTypes->numOfLoop++;
            Loop *L = (*it);
            for (uint i = 0; i < L->getNumBlocks(); i++)
            {
                g_InstrTypes->numLoopInsts += L->getBlocks()[i]->getInstList().size();
            }
        }
    }
}

bool CheckInstrTypes::runOnFunction(Function &F)
{
    LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    g_InstrTypes->hasLoop |= !(LI->empty());

    // check if module has debug info
    g_InstrTypes->hasDebugInfo = F.getParent()->getNamedMetadata("llvm.dbg.cu") != nullptr;

    visit(F);
	SetLoopFlags(F);
    return false;
}

void CheckInstrTypes::visitInstruction(llvm::Instruction &I) {
    if (!llvm::isa<llvm::DbgInfoIntrinsic>(&I))
    {
        g_InstrTypes->numInsts++;
    }
}

void CheckInstrTypes::visitCallInst(CallInst &C)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasCall = true;

    Function* calledFunc = C.getCalledFunction();

    if(calledFunc == NULL)
    {
        // calls to 'blocks' have a null Function object
        g_InstrTypes->hasSubroutines = true;
        g_InstrTypes->hasIndirectCall = true;
    }
    else if(!calledFunc->isDeclaration())
    {
        g_InstrTypes->hasSubroutines = true;
    }
    if(C.mayWriteToMemory())
    {
        if(GenIntrinsicInst *CI = dyn_cast<GenIntrinsicInst>(&C))
        {
            GenISAIntrinsic::ID IID = CI->getIntrinsicID();
            if(IID != GenISA_OUTPUT && IID != GenISA_discard)
            {
                g_InstrTypes->psHasSideEffect = true;
            }
        }
    }

    if (isSampleLoadGather4InfoInstruction(&C))
    {
        g_InstrTypes->numSample++;
    }

    if (GenIntrinsicInst *CI = llvm::dyn_cast<GenIntrinsicInst>(&C))
    {
        switch (CI->getIntrinsicID())
        {
        case GenISAIntrinsic::GenISA_atomiccounterinc:
        case GenISAIntrinsic::GenISA_atomiccounterpredec:
        case GenISAIntrinsic::GenISA_icmpxchgatomicraw:
        case GenISAIntrinsic::GenISA_icmpxchgatomicrawA64:
        case GenISAIntrinsic::GenISA_cmpxchgatomicstructured:
        case GenISAIntrinsic::GenISA_icmpxchgatomictyped:
        case GenISAIntrinsic::GenISA_intatomicraw:
        case GenISAIntrinsic::GenISA_intatomicrawA64:
        case GenISAIntrinsic::GenISA_dwordatomicstructured:
        case GenISAIntrinsic::GenISA_intatomictyped:
        case GenISAIntrinsic::GenISA_fcmpxchgatomicraw:
        case GenISAIntrinsic::GenISA_fcmpxchgatomicrawA64:
        case GenISAIntrinsic::GenISA_fcmpxchgatomicstructured:
        case GenISAIntrinsic::GenISA_floatatomicraw:
        case GenISAIntrinsic::GenISA_floatatomicrawA64:
        case GenISAIntrinsic::GenISA_floatatomicstructured:
            g_InstrTypes->hasAtomics = true;
            break;
        case GenISAIntrinsic::GenISA_discard:
            g_InstrTypes->hasDiscard = true;
            break;
        case GenISAIntrinsic::GenISA_WaveShuffleIndex:
            g_InstrTypes->mayHaveIndirectOperands = true;
            break;
        default:
            break;
        }
    }
}

void CheckInstrTypes::visitBranchInst(BranchInst &I)
{
    g_InstrTypes->numInsts++;
}

void CheckInstrTypes::visitSwitchInst(SwitchInst &I)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasSwitch = true;
}

void CheckInstrTypes::visitIndirectBrInst(IndirectBrInst &I)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasIndirectBranch = true;
}

void CheckInstrTypes::visitICmpInst(ICmpInst &I)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasCmp = true;
}

void CheckInstrTypes::visitFCmpInst(FCmpInst &I)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasCmp = true;
}

void CheckInstrTypes::visitAllocaInst(AllocaInst &I)
{
    g_InstrTypes->numInsts++;
    if (I.isArrayAllocation() ||
        I.getAllocatedType()->isArrayTy() ||
        I.getAllocatedType()->isStructTy() ||
        I.getAllocatedType()->isVectorTy())
    {
        g_InstrTypes->hasNonPrimitiveAlloca = true;
    }
    else
    {
        g_InstrTypes->hasPrimitiveAlloca = true;
    }
}

void CheckInstrTypes::visitLoadInst(LoadInst &I)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasLoadStore = true;
    if (I.getPointerAddressSpace() == ADDRESS_SPACE_LOCAL)
    {
        g_InstrTypes->hasLocalLoadStore = true;
    }
}

void CheckInstrTypes::visitStoreInst(StoreInst &I)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasLoadStore = true;
    uint as = I.getPointerAddressSpace();
    BufferType bufType = GetBufferType(as);
    if (bufType != BUFFER_TYPE_UNKNOWN)
    {
        g_InstrTypes->psHasSideEffect = true;
    }
    if (I.getPointerAddressSpace() == ADDRESS_SPACE_LOCAL)
    {
        g_InstrTypes->hasLocalLoadStore = true;
    }
}

void CheckInstrTypes::visitPHINode(PHINode &PN)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasPhi = true;
}

void CheckInstrTypes::visitSelectInst(SelectInst &I)
{
    g_InstrTypes->numInsts++;
    g_InstrTypes->hasSel = true;
}
