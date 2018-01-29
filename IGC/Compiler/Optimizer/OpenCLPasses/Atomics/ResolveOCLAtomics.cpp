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

#include "Compiler/Optimizer/OpenCLPasses/Atomics/ResolveOCLAtomics.hpp"
#include "Compiler/IGCPassSupport.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/ADT/StringExtras.h>
#include "common/LLVMWarningsPop.hpp"

#include "GenISAIntrinsics/GenIntrinsics.h"

using namespace llvm;
using namespace IGC;

// Register pass to igc-opt
#define PASS_FLAG "igc-resolve-atomics"
#define PASS_DESCRIPTION "Resolve atomic built-ins"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(ResolveOCLAtomics, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_END(ResolveOCLAtomics, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char ResolveOCLAtomics::ID = 0;

ResolveOCLAtomics::ResolveOCLAtomics() : ModulePass(ID)
{
    initializeResolveOCLAtomicsPass(*PassRegistry::getPassRegistry());
    initResolveOCLAtomics();
}

OCLAtomicAttrs ResolveOCLAtomics::genAtomicAttrs(AtomicOp   op,
                                                 BufferType bufType)
{
    //              bufType
    //                 |
    //       not used  V   op
    //       |-------|---|---|
    //    0 x 0 0 0 0 0 0 0 0 
    return op | (bufType << ATTR_BUFFER_TYPE_SHIFT);
}

AtomicOp ResolveOCLAtomics::getAtomicOp(StringRef name)
{
    OCLAtomicAttrs  attrs = m_AtomicDescMap[name];
    return (AtomicOp) (attrs & 0xFF);
}

BufferType ResolveOCLAtomics::getBufType(StringRef name)
{
    OCLAtomicAttrs  attrs = m_AtomicDescMap[name];
    return (BufferType) ((attrs >> ATTR_BUFFER_TYPE_SHIFT) & 0xFF);
}

void ResolveOCLAtomics::initResolveOCLAtomics()
{
    initOCLAtomicsMap();
}

void ResolveOCLAtomics::initOCLAtomicsMap()
{
#define DEF_OCL_IGC_ATOMIC(name, op, buf_type) \
    m_AtomicDescMap[StringRef(name)] = genAtomicAttrs(op, buf_type);
#include "OCLAtomicsDef.hpp"
#undef DEF_OCL_IGC_ATOMIC
}

bool ResolveOCLAtomics::runOnModule(Module &M)
{
    m_pModule  = &M;
    m_Int32Ty = Type::getInt32Ty(m_pModule->getContext());

    int pointerSize = M.getDataLayout().getPointerSizeInBits();
    assert(pointerSize == 64 || pointerSize == 32);

    if( pointerSize == 64 )
    {
        m_64bitPointer = true;
    }
    else
    {
        m_64bitPointer = false;
    }
    
    m_changed = false;

    // Visit all call instuctions in the function F.
    visit(M);

    return m_changed;
}

void ResolveOCLAtomics::visitCallInst(CallInst &callInst)
{
    if (!callInst.getCalledFunction())
    {
        return;
    }

    StringRef funcName = callInst.getCalledFunction()->getName();
    if( funcName.startswith("__builtin_IB_atomic") )
    {
        assert(m_AtomicDescMap.count(funcName) && "Unexpected IGC atomic function name.");
        processOCLAtomic(callInst, getAtomicOp(funcName), getBufType(funcName));
        m_changed = true;
    }
}

void ResolveOCLAtomics::processOCLAtomic(CallInst &callInst, AtomicOp op, BufferType bufType)
{
    const DebugLoc &DL = callInst.getDebugLoc();

    Value *src1 = nullptr;
    // Generate a call to GenISA_dwordatomic intrinsic.
    GenISAIntrinsic::ID genIsaIntrinID;

    const bool noSources = (callInst.getNumArgOperands() == 1);
    // For atomics w/o sources (atomic_inc and atomic_dec), src0 should be absent.
    // However, we cannot pass nullptr as argument, so we set src0 = "0" and it
    // will be ignored in EmitPass::emitAtomicRaw.
    Value *src0 = noSources ?
        ConstantInt::get(callInst.getType(), 0) :
        callInst.getOperand(1);

    const bool floatArgs = !noSources && src0->getType()->isFloatingPointTy();
    const bool is64bit = m_64bitPointer && bufType != SLM;

    // Cmpxchg intrinsic has 2 sources.
    if( op == EATOMIC_CMPXCHG || op == EATOMIC_FCMPWR )
    {
        src1 = callInst.getOperand(2);
        // For 64-bit pointers, we have to use the A64 versions of GenISA atomic intrinsics.
        if (is64bit)
        {
            genIsaIntrinID = floatArgs ?
                GenISAIntrinsic::GenISA_fcmpxchgatomicrawA64 :
                GenISAIntrinsic::GenISA_icmpxchgatomicrawA64;
        }
        else
        {
            genIsaIntrinID = floatArgs ?
                GenISAIntrinsic::GenISA_fcmpxchgatomicraw :
                GenISAIntrinsic::GenISA_icmpxchgatomicraw;
        }
    }
    else
    {
        // All other atomic intrinsics has 1 source, and we pass
        // the operation as the last argument.
        src1 = ConstantInt::get(m_Int32Ty, op);
        if (is64bit)
        {
            genIsaIntrinID = floatArgs ?
                GenISAIntrinsic::GenISA_floatatomicrawA64 :
                GenISAIntrinsic::GenISA_intatomicrawA64;
        }
        else
        {
            genIsaIntrinID = floatArgs ?
                GenISAIntrinsic::GenISA_floatatomicraw :
                GenISAIntrinsic::GenISA_intatomicraw;
        }
    }

    Value *dstBuffer = callInst.getOperand(0);
    Value *dst  = callInst.getOperand(0);

    // We will use 64-bit dst only for 64-bit global pointers.
    if (!is64bit)
    {
        bool createDstCast = true;
        if( CastInst *castInst = dyn_cast<CastInst>(dst) )
        {
            Type *srcType = castInst->getSrcTy();
            // If dst is a "int32 -> ptr" conversion, we can use its src instead
            // of creating reverse conversion.
            if( srcType->isIntegerTy(32) )
            {
                dst = castInst->getOperand(0);
                createDstCast = false;
            }
        }
        if( createDstCast )
        {
            Instruction *pCast = CastInst::CreatePointerCast(dst, m_Int32Ty, "PtrDstToInt", &callInst);
            pCast->setDebugLoc(DL);
            dst = pCast;
        }
    }

    SmallVector<Value*, 8> args;

    // Prepare the arguments and create a call.
    args.push_back(dstBuffer);
	args.push_back(dst);
	args.push_back(src0);
    args.push_back(src1);

    SmallVector<Type*, 4> intrinArgTypes
    {
        callInst.getType(),
        dstBuffer->getType(),
        dst->getType()
    };

    Function *isaIntrin = GenISAIntrinsic::getDeclaration(m_pModule, genIsaIntrinID , intrinArgTypes);
    CallInst *isaIntrinCall = CallInst::Create(isaIntrin, args, callInst.getName(), &callInst);
    isaIntrinCall->setDebugLoc(DL);

    // Replace the __builtin_IB_atomic call with a call to created GenISA intrinsic.
    callInst.replaceAllUsesWith(isaIntrinCall);
    callInst.eraseFromParent();
}

CallInst* ResolveOCLAtomics::genGetBufferPtr(CallInst &callInst, BufferType bufType)
{
    ConstantInt *bufIndexVal = ConstantInt::get(m_Int32Ty, 0);
    ConstantInt *bufTypeVal = ConstantInt::get(m_Int32Ty, bufType);

    unsigned int addressSpace;
    if( bufType == SLM )
    {
        addressSpace = ADDRESS_SPACE_LOCAL;
    }
    else
    {
        addressSpace = ADDRESS_SPACE_GLOBAL;
    }
    Type *ptrType = PointerType::get(m_Int32Ty, addressSpace);
    Function  *getBufferPtr = GenISAIntrinsic::getDeclaration(m_pModule, GenISAIntrinsic::GenISA_GetBufferPtr, ptrType);
    
    // Generate a call to GenISA.GetBufferPtr intrinsic:
    //   %base_ptr = call float* @llvm.GenISA.GetBufferPtr(i32 %bufIdx, i32 %type)
    llvm::SmallVector<Value*, 2> getBufferPtrArgs;
    getBufferPtrArgs.push_back(bufIndexVal);
    getBufferPtrArgs.push_back(bufTypeVal);
    
    return CallInst::Create(getBufferPtr, getBufferPtrArgs, callInst.getName(), &callInst);
}
