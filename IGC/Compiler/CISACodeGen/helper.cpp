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

#include "Compiler/CISACodeGen/helper.h"
#include "Compiler/CISACodeGen/CISACodeGen.h"
#include "Compiler/Optimizer/OpenCLPasses/KernelArgs.hpp"
#include "Compiler/MetaDataUtilsWrapper.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/Analysis/ValueTracking.h>
#include "common/LLVMWarningsPop.hpp"

#include "GenISAIntrinsics/GenIntrinsicInst.h"
#include "Compiler/CISACodeGen/ShaderCodeGen.hpp"

#include "common/secure_mem.h"

#include <stack>

using namespace llvm;
using namespace GenISAIntrinsic;

/************************************************************************
This file contains helper functions for the code generator
Many functions use X-MACRO, that allow us to separate data about encoding
to the logic of the helper functions

************************************************************************/

namespace IGC
{
typedef union _gfxResourceAddrSpace
{
    struct _bits
    {
        unsigned int       bufId                 : 16;
        unsigned int       bufType               : 4;
        unsigned int       indirect              : 1;     // bool
        unsigned int       reserved              : 11;
    } bits;
    uint32_t u32Val;
} GFXResourceAddrSpace;

unsigned EncodeAS4GFXResource(
    const llvm::Value& bufIdx,
    BufferType bufType,
    unsigned uniqueIndAS)
{
    GFXResourceAddrSpace temp;
    temp.u32Val = 0;
    assert( (bufType+1) < 16 );
    temp.bits.bufType = bufType + 1;
    if (bufType == SLM)
    {
        return ADDRESS_SPACE_LOCAL; // We use addrspace 3 for SLM
    }
    else if (bufType == STATELESS_READONLY)
    {
        return ADDRESS_SPACE_CONSTANT; 
    }
    else if (bufType == STATELESS)
    {
        return ADDRESS_SPACE_GLOBAL;
    }
    else if (llvm::isa<llvm::ConstantInt>(&bufIdx))
    {
        unsigned int bufId = (unsigned int)(llvm::cast<llvm::ConstantInt>(&bufIdx)->getZExtValue());
        assert( bufId < (1 << 31) );
        temp.bits.bufId = bufId;
        return temp.u32Val;
    }

    // if it is indirect-buf, it is front-end's job to give a proper(unique) address-space per access
    temp.bits.bufId = uniqueIndAS;
    temp.bits.indirect = 1;
    return temp.u32Val;
}
///
/// if you want resource-dimension, use GetBufferDimension()
///
BufferType DecodeAS4GFXResource(unsigned addrSpace, bool& directIndexing, unsigned& bufId)
{
    GFXResourceAddrSpace temp;
    temp.u32Val = addrSpace;

    directIndexing = (temp.bits.indirect == 0);
    bufId = temp.bits.bufId;

    if(addrSpace == ADDRESS_SPACE_LOCAL)
    {
        return SLM;
    }
    unsigned bufType = temp.bits.bufType - 1;
    if (bufType < BUFFER_TYPE_UNKNOWN)
    {
        return (BufferType)bufType;
    }
    return BUFFER_TYPE_UNKNOWN;
}
///
/// returns constant buffer load offset
///
int getConstantBufferLoadOffset(llvm::LoadInst *ld)
{
    int offset = 0;
    Value* ptr = ld->getPointerOperand();
    if (isa<ConstantPointerNull>(ptr))
    {
        offset = 0;
    }
    else if (IntToPtrInst* itop = dyn_cast<IntToPtrInst>(ptr))
    {
        ConstantInt* ci = dyn_cast<ConstantInt>(
            itop->getOperand(0));
        if (ci)
        {
            offset = int_cast<unsigned>(ci->getZExtValue());
        }
    }
    else if (ConstantExpr* itop = dyn_cast<ConstantExpr>(ptr))
    {
        if (itop->getOpcode() == Instruction::IntToPtr)
        {
            offset = int_cast<unsigned>(
                cast<ConstantInt>(itop->getOperand(0))->getZExtValue());
        }
    }
    return offset;
}
///
/// returns info if direct addressing is used
///
bool IsDirectIdx(unsigned addrSpace)
{
    GFXResourceAddrSpace temp;
    temp.u32Val = addrSpace;
    return (temp.bits.indirect == 0);
}

llvm::LoadInst* cloneLoad(llvm::LoadInst *Orig, llvm::Value *Ptr)
{
    llvm::LoadInst *LI = new llvm::LoadInst(Ptr, "", Orig);
    LI->setVolatile(Orig->isVolatile());
    LI->setAlignment(Orig->getAlignment());
    if (LI->isAtomic())
    {
        LI->setAtomic(Orig->getOrdering(), Orig->getSynchScope());
    }
    // Clone metadata
    llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> MDs;
    Orig->getAllMetadata(MDs);
    for (llvm::SmallVectorImpl<std::pair<unsigned, llvm::MDNode *> >::iterator
         MI = MDs.begin(), ME = MDs.end(); MI != ME; ++MI)
    {
        LI->setMetadata(MI->first, MI->second);
    }
    return LI;
}

llvm::StoreInst* cloneStore(llvm::StoreInst *Orig, llvm::Value *Val, llvm::Value *Ptr)
{
    llvm::StoreInst *SI = new llvm::StoreInst(Val, Ptr, Orig);
    SI->setVolatile(Orig->isVolatile());
    SI->setAlignment(Orig->getAlignment());
    if (SI->isAtomic())
    {
        SI->setAtomic(Orig->getOrdering(), Orig->getSynchScope());
    }
    // Clone metadata
    llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> MDs;
    Orig->getAllMetadata(MDs);
    for (llvm::SmallVectorImpl<std::pair<unsigned, llvm::MDNode *> >::iterator
         MI = MDs.begin(), ME = MDs.end(); MI != ME; ++MI)
    {
        SI->setMetadata(MI->first, MI->second);
    }
    return SI;
}

///
/// Tries to trace a resource pointer (texture/sampler/buffer) back to
/// the pointer source. Also returns a vector of all instructions in the search path
///
Value* TracePointerSource(Value* resourcePtr, bool seenPhi, bool fillList, std::vector<Value*> &instList)
{
    Value* srcPtr = nullptr;
    Value* baseValue = resourcePtr;

    while (true)
    {
        if (fillList)
        {
            instList.push_back(baseValue);
        }

        if (GenIntrinsicInst* pintr = dyn_cast<GenIntrinsicInst>(baseValue))
        {
            if (pintr->getIntrinsicID() == GenISAIntrinsic::GenISA_GetBufferPtr ||
                pintr->getIntrinsicID() == GenISAIntrinsic::GenISA_RuntimeValue)
            {
                // Source pointer instruction found
                srcPtr = baseValue;
                break;
            }
            else
            {
                break;
            }
        }
        else if (isa<Argument>(baseValue))
        {
            // For compute, resource comes from the kernel args
            srcPtr = baseValue;
            break;
        }
        else if (CastInst* inst = dyn_cast<CastInst>(baseValue))
        {
            baseValue = inst->getOperand(0);
        }
        else if (GetElementPtrInst* inst = dyn_cast<GetElementPtrInst>(baseValue))
        {
            baseValue = inst->getOperand(0);
        }
        else if (ExtractElementInst* inst = dyn_cast<ExtractElementInst>(baseValue))
        {
            baseValue = inst->getOperand(0);
        }
        else if (InsertElementInst* inst = dyn_cast<InsertElementInst>(baseValue))
        {
            baseValue = inst->getOperand(1);
        }
        else if (PHINode* inst = dyn_cast<PHINode>(baseValue))
        {
            if (seenPhi)
            {
                // Only support one PHI in the search path for now, since we might run
                // into a loop situation and where we encounter the same PHI and
                // end up in an infinite recursion
                break;
            }
            for(unsigned int i = 0; i < inst->getNumIncomingValues(); ++i)
            {
                // All phi paths must be trace-able and trace back to the same source
                Value* phiVal = inst->getIncomingValue(i);
                Value* phiSrcPtr = TracePointerSource(phiVal, true, fillList, instList);
                if (phiSrcPtr == nullptr)
                {
                    return nullptr;
                }
                else if (srcPtr == nullptr)
                {
                    srcPtr = phiSrcPtr;
                }
                else if (srcPtr != phiSrcPtr)
                {
                    return nullptr;
                }
            }
            break;
        }
        else
        {
            // Unsupported instruction in search chain. Don't continue.
            break;
        }
    }
    return srcPtr;
}

///
/// Only trace the GetBufferPtr instruction (ignore GetElementPtr)
///
Value* TracePointerSource(Value* resourcePtr)
{
    std::vector<Value*> tempList; //unused
    return TracePointerSource(resourcePtr, false, false, tempList);
}

bool GetResourcePointerInfo(Value* srcPtr, unsigned &resID, IGC::BufferType &resTy)
{
    if (GenIntrinsicInst* pIntr = dyn_cast<GenIntrinsicInst>(srcPtr))
    {
        if (pIntr->getIntrinsicID() == GenISAIntrinsic::GenISA_GetBufferPtr)
        {
            Value *bufIdV = pIntr->getOperand(0);
            Value *bufTyV = pIntr->getOperand(1);
            if (isa<ConstantInt>(bufIdV) && isa<ConstantInt>(bufTyV))
            {
                resID = (unsigned)(cast<ConstantInt>(bufIdV)->getZExtValue());
                resTy = (IGC::BufferType)(cast<ConstantInt>(bufTyV)->getZExtValue());
                return true;
            }
        }
        else if (pIntr->getIntrinsicID() == GenISAIntrinsic::GenISA_RuntimeValue)
        {
            MDNode* resID_md = pIntr->getMetadata("resID");
            MDNode* resTy_md = pIntr->getMetadata("resTy");
            if (resID_md && resTy_md)
            {
                resID = (unsigned) mdconst::dyn_extract<ConstantInt>(resID_md->getOperand(0))->getZExtValue();
                resTy = (BufferType) mdconst::dyn_extract<ConstantInt>(resTy_md->getOperand(0))->getZExtValue();
                return true;
            }
        }
    }
    return false;
}

///
/// Replaces oldPtr with newPtr in a sample/ld intrinsic's argument list. The new instrinsic will
/// replace the old one in the module
///
void ChangePtrTypeInIntrinsic(llvm::GenIntrinsicInst *&pIntr, llvm::Value* oldPtr, llvm::Value* newPtr)
{
    llvm::Module *pModule = pIntr->getParent()->getParent()->getParent();
    llvm::Function *pCalledFunc = pIntr->getCalledFunction();

    // Look at the intrinsic and figure out which pointer to change
    int num_ops = pIntr->getNumArgOperands();
    llvm::SmallVector<llvm::Value*, 5> args;

    for(int i = 0; i < num_ops; ++i)
    {
        if(pIntr->getArgOperand(i) == oldPtr)
            args.push_back(newPtr);
        else
            args.push_back(pIntr->getArgOperand(i));
    }

    llvm::Function *pNewIntr = nullptr;
    llvm::SmallVector<llvm::Type*, 4> overloadedTys;
    GenISAIntrinsic::ID id = pIntr->getIntrinsicID();
    switch(id)
    {
        case llvm::GenISAIntrinsic::GenISA_ldptr:
        case llvm::GenISAIntrinsic::GenISA_ldmsptr:
        case llvm::GenISAIntrinsic::GenISA_ldmcsptr:
            overloadedTys.push_back(pCalledFunc->getReturnType());
            overloadedTys.push_back(newPtr->getType());
            break;
        case llvm::GenISAIntrinsic::GenISA_resinfoptr:
        case llvm::GenISAIntrinsic::GenISA_readsurfaceinfoptr:
        case llvm::GenISAIntrinsic::GenISA_sampleinfoptr:
            overloadedTys.push_back(newPtr->getType());
            break;
        case llvm::GenISAIntrinsic::GenISA_sampleptr:
        case llvm::GenISAIntrinsic::GenISA_sampleBptr:
        case llvm::GenISAIntrinsic::GenISA_sampleCptr:
        case llvm::GenISAIntrinsic::GenISA_sampleDptr:
        case llvm::GenISAIntrinsic::GenISA_sampleLptr:
        case llvm::GenISAIntrinsic::GenISA_sampleBCptr:
        case llvm::GenISAIntrinsic::GenISA_sampleDCptr:
        case llvm::GenISAIntrinsic::GenISA_sampleLCptr:
        case llvm::GenISAIntrinsic::GenISA_gather4ptr:
        case llvm::GenISAIntrinsic::GenISA_gather4POptr:
        case llvm::GenISAIntrinsic::GenISA_gather4Cptr:
        case llvm::GenISAIntrinsic::GenISA_gather4POCptr:
        case llvm::GenISAIntrinsic::GenISA_lodptr:
        {
            // Figure out the intrinsic operands for texture & sampler
            llvm::Value *pTextureValue = nullptr, *pSamplerValue = nullptr;
            getTextureAndSamplerOperands(pIntr, pTextureValue, pSamplerValue);

            overloadedTys.push_back(pCalledFunc->getReturnType());
            overloadedTys.push_back(pIntr->getOperand(0)->getType());

            if(pTextureValue == oldPtr)
            {
                overloadedTys.push_back(newPtr->getType());
                if(pSamplerValue)
                {
                    // Samplerless messages will not have sampler in signature.
                    overloadedTys.push_back(pSamplerValue->getType());
                }
            }
            else if(pSamplerValue == oldPtr)
            {
                overloadedTys.push_back(pTextureValue->getType());
                overloadedTys.push_back(newPtr->getType());
            }

            break;
        }
        case llvm::GenISAIntrinsic::GenISA_typedread:
        case llvm::GenISAIntrinsic::GenISA_typedwrite:
            overloadedTys.push_back(newPtr->getType());
            break;
        case llvm::GenISAIntrinsic::GenISA_intatomicraw:
        case llvm::GenISAIntrinsic::GenISA_icmpxchgatomicraw:
        case GenISAIntrinsic::GenISA_intatomicrawA64:
        case GenISAIntrinsic::GenISA_icmpxchgatomicrawA64:
            overloadedTys.push_back(pIntr->getType());
            overloadedTys.push_back(newPtr->getType());
            if(id == GenISAIntrinsic::GenISA_intatomicrawA64)
            {
                args[0] = args[1];
                args[1] = CastInst::CreatePointerCast(args[1], Type::getInt32Ty(pModule->getContext()), "", pIntr);
                id = GenISAIntrinsic::GenISA_intatomicraw;
            }
            else if(id == GenISAIntrinsic::GenISA_icmpxchgatomicrawA64)
            {
                args[0] = args[1];
                args[1] = CastInst::CreatePointerCast(args[1], Type::getInt32Ty(pModule->getContext()), "", pIntr);
                id = GenISAIntrinsic::GenISA_icmpxchgatomicraw;
            }
            break;
        default:
            assert(0 && "Unknown intrinsic encountered while changing pointer types");
            break;
    }

    pNewIntr = llvm::GenISAIntrinsic::getDeclaration(
                                                     pModule,
                                                     id,
                                                     overloadedTys);

    llvm::CallInst *pNewCall = llvm::CallInst::Create(pNewIntr, args, "", pIntr);

    pIntr->replaceAllUsesWith(pNewCall);
    pIntr->eraseFromParent();

    pIntr = llvm::cast<llvm::GenIntrinsicInst>(pNewCall);
}

///
/// Returns the sampler/texture pointers for resource access intrinsics
///
void getTextureAndSamplerOperands(llvm::GenIntrinsicInst *pIntr, llvm::Value*& pTextureValue, llvm::Value*& pSamplerValue)
{
    if (llvm::SamplerLoadIntrinsic *pSamplerLoadInst = llvm::dyn_cast<llvm::SamplerLoadIntrinsic>(pIntr))
    {
        pTextureValue = pSamplerLoadInst->getTextureValue();
        pSamplerValue = nullptr;
    }
    else if (llvm::SampleIntrinsic *pSampleInst = llvm::dyn_cast<llvm::SampleIntrinsic>(pIntr))
    {
        pTextureValue = pSampleInst->getTextureValue();
        pSamplerValue = pSampleInst->getSamplerValue();
    }
    else if (llvm::SamplerGatherIntrinsic *pGatherInst = llvm::dyn_cast<llvm::SamplerGatherIntrinsic>(pIntr))
    {
        pTextureValue = pGatherInst->getTextureValue();
        pSamplerValue = pGatherInst->getSamplerValue();
    }
    else
    {
        pTextureValue = nullptr;
        pSamplerValue = nullptr;
        switch (pIntr->getIntrinsicID())
        {
            case llvm::GenISAIntrinsic::GenISA_resinfoptr:
            case llvm::GenISAIntrinsic::GenISA_readsurfaceinfoptr:
            case llvm::GenISAIntrinsic::GenISA_sampleinfoptr:
            case llvm::GenISAIntrinsic::GenISA_typedwrite:
            case llvm::GenISAIntrinsic::GenISA_typedread:
                pTextureValue = pIntr->getOperand(0);
                break;
            default:
                break;
        }
    }
}

EOPCODE GetOpCode(llvm::Instruction* inst)
{
    if(GenIntrinsicInst *CI = dyn_cast<GenIntrinsicInst>( inst ))
    {
        unsigned ID = CI->getIntrinsicID();
        return (EOPCODE)(OPCODE(ID,e_Intrinsic));
    }
    else if(llvm::IntrinsicInst *CI = llvm::dyn_cast<llvm::IntrinsicInst>( inst ))
    {
        unsigned ID = CI->getIntrinsicID();
        return (EOPCODE)(OPCODE(ID,e_Intrinsic));
    }
    return (EOPCODE)(OPCODE(inst->getOpcode(),e_Instruction));
}

BufferType GetBufferType(uint addrSpace)
{
    bool directIndexing = false;
    unsigned int bufId = 0;
    return DecodeAS4GFXResource(addrSpace, directIndexing, bufId);
}

bool IsReadOnlyLoadDirectCB(llvm::Instruction *pLLVMInst, uint& cbId, llvm::Value* &eltPtrVal, BufferType& bufType)
{
    LoadInst *inst = dyn_cast<LoadInst>(pLLVMInst);
    if(!inst)
    {
        return false;
    }
    unsigned as = inst->getPointerAddressSpace();
    bool directBuf;
    // cbId gets filled in the following call;
    bufType = IGC::DecodeAS4GFXResource(as, directBuf, cbId);
    if((bufType == CONSTANT_BUFFER || bufType == RESOURCE) && directBuf)
    {
        Value *ptrVal = inst->getPointerOperand();
        // skip bitcast and find the real address computation
        while(isa<BitCastInst>(ptrVal))
        {
            ptrVal = cast<BitCastInst>(ptrVal)->getOperand(0);
        }
        if(isa<ConstantPointerNull>(ptrVal) ||
            isa<IntToPtrInst>(ptrVal) ||
            isa<GetElementPtrInst>(ptrVal) ||
            isa<ConstantExpr>(ptrVal) ||
            isa<Argument>(ptrVal))
        {
            eltPtrVal = ptrVal;
            return true;
        }
    }
    return false;
}

bool IsLoadFromDirectCB(llvm::Instruction *pLLVMInst, uint& cbId, llvm::Value* &eltPtrVal)
{
    BufferType bufType = BUFFER_TYPE_UNKNOWN;
    bool isReadOnly = IsReadOnlyLoadDirectCB(pLLVMInst, cbId, eltPtrVal, bufType);
    return isReadOnly && bufType == CONSTANT_BUFFER;
}
    
/// this is texture-load not buffer-load
bool isLdInstruction(llvm::Instruction* inst)
{
    return isa<SamplerLoadIntrinsic>(inst);
}

// function returns the position of the texture operand for sample/ld instructions
llvm::Value* getTextureIndexArgBasedOnOpcode(llvm::Instruction* inst)
{
    if (isLdInstruction(inst))
    {
        return cast<SamplerLoadIntrinsic>(inst)->getTextureValue();
    }
    else if (isSampleInstruction(inst))
    {
        return cast<SampleIntrinsic>(inst)->getTextureValue();
    }
    else if (isGather4Instruction(inst))
    {
        return cast<SamplerGatherIntrinsic>(inst)->getTextureValue();
    }

    return nullptr;
}

int findSampleInstructionTextureIdx(llvm::Instruction* inst)
{
    // fetch the textureArgIdx.
    Value* ptr = getTextureIndexArgBasedOnOpcode(inst);
    unsigned textureIdx = -1;

    if (ptr && ptr->getType()->isPointerTy())
    {
        BufferType bufType = BUFFER_TYPE_UNKNOWN;
        if (!(isa<GenIntrinsicInst>(ptr) &&
            cast<GenIntrinsicInst>(ptr)->getIntrinsicID() == GenISAIntrinsic::GenISA_GetBufferPtr))
        {
            uint as = ptr->getType()->getPointerAddressSpace();
            bool directIndexing;
            bufType = DecodeAS4GFXResource(as, directIndexing, textureIdx);
            if (bufType == UAV)
            {
                // dont do any clustering on read/write images
                textureIdx = -1;
            }
        }
    }
    else if (ptr)
    {
        if (llvm::dyn_cast<llvm::ConstantInt>(ptr))
        {
            textureIdx = int_cast<unsigned>(GetImmediateVal(ptr));
        }
    }

    return textureIdx;
}

bool isSampleLoadGather4InfoInstruction(llvm::Instruction* inst)
{
    if (isa<GenIntrinsicInst>(inst))
    {
        switch ((cast<GenIntrinsicInst>(inst))->getIntrinsicID())
        {
        case GenISAIntrinsic::GenISA_sample:
        case GenISAIntrinsic::GenISA_sampleptr:
        case GenISAIntrinsic::GenISA_sampleB:
        case GenISAIntrinsic::GenISA_sampleBptr:
        case GenISAIntrinsic::GenISA_sampleC:
        case GenISAIntrinsic::GenISA_sampleCptr:
        case GenISAIntrinsic::GenISA_sampleD:
        case GenISAIntrinsic::GenISA_sampleDptr:
        case GenISAIntrinsic::GenISA_sampleDC:
        case GenISAIntrinsic::GenISA_sampleDCptr:
        case GenISAIntrinsic::GenISA_sampleL:
        case GenISAIntrinsic::GenISA_sampleLptr:
        case GenISAIntrinsic::GenISA_sampleLC:
        case GenISAIntrinsic::GenISA_sampleLCptr:
        case GenISAIntrinsic::GenISA_sampleBC:
        case GenISAIntrinsic::GenISA_sampleBCptr:
        case GenISAIntrinsic::GenISA_lod:
        case GenISAIntrinsic::GenISA_lodptr:
        case GenISAIntrinsic::GenISA_ld:
        case GenISAIntrinsic::GenISA_ldptr:
        case GenISAIntrinsic::GenISA_ldms:
        case GenISAIntrinsic::GenISA_ldmsptr:
        case GenISAIntrinsic::GenISA_ldmsptr16bit:
        case GenISAIntrinsic::GenISA_ldmcs:
        case GenISAIntrinsic::GenISA_ldmcsptr:
        case GenISAIntrinsic::GenISA_sampleinfo:
        case GenISAIntrinsic::GenISA_sampleinfoptr:
        case GenISAIntrinsic::GenISA_resinfo:
        case GenISAIntrinsic::GenISA_resinfoptr:
        case GenISAIntrinsic::GenISA_gather4:
        case GenISAIntrinsic::GenISA_gather4C:
        case GenISAIntrinsic::GenISA_gather4PO:
        case GenISAIntrinsic::GenISA_gather4POC:
        case GenISAIntrinsic::GenISA_gather4ptr:
        case GenISAIntrinsic::GenISA_gather4Cptr:
        case GenISAIntrinsic::GenISA_gather4POptr:
        case GenISAIntrinsic::GenISA_gather4POCptr:
            return true;
        default:
            return false;
        }
    }

    return false;
}

bool isSampleInstruction(llvm::Instruction* inst)
{
    return isa<SampleIntrinsic>(inst);
}

bool isInfoInstruction(llvm::Instruction* inst)
{
    return isa<InfoIntrinsic>(inst);
}

bool isGather4Instruction(llvm::Instruction* inst)
{
    return isa<SamplerGatherIntrinsic>(inst);
}

bool IsMediaIOIntrinsic(llvm::Instruction* inst)
{
    if (auto *pGI = dyn_cast<llvm::GenIntrinsicInst>(inst))
    {
        GenISAIntrinsic::ID id = pGI->getIntrinsicID();

        return id == GenISAIntrinsic::GenISA_MediaBlockRead ||
            id == GenISAIntrinsic::GenISA_MediaBlockWrite;
    }

    return false;
}

bool isSubGroupIntrinsic(const llvm::Instruction *I)
{
    const GenIntrinsicInst *GII = dyn_cast<GenIntrinsicInst>(I);
    if (!GII)
        return false;

    switch (GII->getIntrinsicID())
    {
    case GenISAIntrinsic::GenISA_WaveShuffleIndex:
    case GenISAIntrinsic::GenISA_simdShuffleDown:
    case GenISAIntrinsic::GenISA_simdBlockReadGlobal:
    case GenISAIntrinsic::GenISA_simdBlockWriteGlobal:
    case GenISAIntrinsic::GenISA_simdMediaBlockRead:
    case GenISAIntrinsic::GenISA_simdMediaBlockWrite:
    case GenISAIntrinsic::GenISA_MediaBlockWrite:
    case GenISAIntrinsic::GenISA_MediaBlockRead:
        return true;
    default:
        return false;
    }
}

bool isReadInput(llvm::Instruction *pLLVMInstr);

#define DECLARE_OPCODE(instName, llvmType, name, modifiers, sat, pred, condMod, mathIntrinsic, atomicIntrinsic, regioning) \
    case name:\
    return modifiers;
bool SupportsModifier(llvm::Instruction* inst)
{
    if(llvm::CmpInst* cmp = dyn_cast<llvm::ICmpInst>(inst))
    {
        // special case, cmp supports modifier unless it is unsigned
        return !cmp->isUnsigned();
    }
    switch(GetOpCode(inst))
    {
#include "opCode.h"
    default:
        return false;
    }
}
#undef DECLARE_OPCODE

#define DECLARE_OPCODE(instName, llvmType, name, modifiers, sat, pred, condMod, mathIntrinsic, atomicIntrinsic, regioning) \
    case name:\
    return sat;
bool SupportsSaturate(llvm::Instruction* inst)
{
    switch(GetOpCode(inst))
    {
#include "opCode.h"
    default:
        break;
    }
    return false;
}
#undef DECLARE_OPCODE

#define DECLARE_OPCODE(instName, llvmType, name, modifiers, sat, pred, condMod, mathIntrinsic, atomicIntrinsic, regioning) \
    case name:\
    return pred;
bool SupportsPredicate(llvm::Instruction* inst)
{
    switch(GetOpCode(inst))
    {
#include "opCode.h"
    default:
        return false;
    }
}
#undef DECLARE_OPCODE

#define DECLARE_OPCODE(instName, llvmType, name, modifiers, sat, pred, condMod, mathIntrinsic, atomicIntrinsic, regioning) \
    case name:\
    return condMod;
bool SupportsCondModifier(llvm::Instruction* inst)
{
    switch(GetOpCode(inst))
    {
#include "opCode.h"
    default:
        return false;
    }
}
#undef DECLARE_OPCODE

#define DECLARE_OPCODE(instName, llvmType, name, modifiers, sat, pred, condMod, mathIntrinsic, atomicIntrinsic, regioning) \
    case name:\
    return regioning;
bool SupportsRegioning(llvm::Instruction* inst)
{
    switch (GetOpCode(inst))
    {
#include "opCode.h"
    default:
        break;
    }
    return false;
}
#undef DECLARE_OPCODE

#define DECLARE_OPCODE(instName, llvmType, name, modifiers, sat, pred, condMod, mathIntrinsic, atomicIntrinsic, regioning) \
    case name:\
    return mathIntrinsic;
bool IsMathIntrinsic(EOPCODE opcode)
{
    switch(opcode)
    {
#include "opCode.h"
    default:
        return false;
    }
}
#undef DECLARE_OPCODE

#define DECLARE_OPCODE(instName, llvmType, name, modifiers, sat, pred, condMod, mathIntrinsic, atomicIntrinsic, regioning) \
    case name:\
    return atomicIntrinsic;
bool IsAtomicIntrinsic(EOPCODE opcode)
{
    switch (opcode)
    {
#include "opCode.h"
    default:
        return false;
    }
}
#undef DECLARE_OPCODE

// for now just include shuffle, reduce and scan,
// which have simd32 implementations and should not be split into two instances
bool IsSubGroupIntrinsicWithSimd32Implementation(EOPCODE opcode)
{
    return (opcode == llvm_waveAll || 
            opcode == llvm_wavePrefix || 
            opcode == llvm_waveShuffleIndex);
}


bool IsGradientIntrinsic(EOPCODE opcode)
{
    return(opcode == llvm_gradientX ||
        opcode == llvm_gradientY ||
        opcode == llvm_gradientXfine ||
        opcode == llvm_gradientYfine);
}

bool ComputesGradient(llvm::Instruction *inst)
{
    llvm::SampleIntrinsic *sampleInst = dyn_cast<llvm::SampleIntrinsic>(inst);
    if (sampleInst && sampleInst->IsDerivative())
    {
        return true;
    }
    if (IsGradientIntrinsic(GetOpCode(inst)))
    {
        return true;
    }
    return false;
}

llvm::Value* ExtractElementFromInsertChain(llvm::Value *inst, int pos)
{

    llvm::ConstantDataVector *cstV = llvm::dyn_cast<llvm::ConstantDataVector>(inst);
    if (cstV != NULL) {
        return cstV->getElementAsConstant(pos);
    }

    llvm::InsertElementInst *ie = llvm::dyn_cast<llvm::InsertElementInst>(inst);
    while (ie != NULL) {
        int64_t iOffset = llvm::dyn_cast<llvm::ConstantInt>(ie->getOperand(2))->getSExtValue();
        assert(iOffset>=0);
        if (iOffset == pos) {
            return ie->getOperand(1);
        }
        llvm::Value *insertBase = ie->getOperand(0);
        ie = llvm::dyn_cast<llvm::InsertElementInst>(insertBase);
    }
    return NULL;
}

bool ExtractVec4FromInsertChain(llvm::Value *inst, llvm::Value *elem[4], llvm::SmallVector<llvm::Instruction*, 10> &instructionToRemove)
{
    llvm::ConstantDataVector *cstV = llvm::dyn_cast<llvm::ConstantDataVector>(inst);
    if (cstV != NULL) {
        assert(cstV->getNumElements() == 4);
        for (int i = 0; i < 4; i++) {
            elem[i] = cstV->getElementAsConstant(i);
        }
        return true;
    }

    for (int i = 0; i<4; i++) {
        elem[i] = NULL;
    }
    
    int count = 0;
    llvm::InsertElementInst *ie = llvm::dyn_cast<llvm::InsertElementInst>(inst);
    while (ie != NULL) {
        int64_t iOffset = llvm::dyn_cast<llvm::ConstantInt>(ie->getOperand(2))->getSExtValue();
        assert(iOffset>=0);
        if (elem[iOffset] == NULL) {
            elem[iOffset] = ie->getOperand(1);
            count++;
            if (ie->hasOneUse()) {
                instructionToRemove.push_back(ie);
            }
        }
        llvm::Value *insertBase = ie->getOperand(0);
        ie = llvm::dyn_cast<llvm::InsertElementInst>(insertBase);
    }
    return (count == 4);
}

void VectorToElement(llvm::Value *inst, llvm::Value *elem[], llvm::Type *int32Ty, llvm::Instruction *insert_before, int vsize)
{
    for (int i = 0; i < vsize; i++) {
        if (elem[i] == nullptr) {
            // Create an ExtractElementInst
            elem[i] = llvm::ExtractElementInst::Create(inst, llvm::ConstantInt::get(int32Ty, i), "", insert_before);
        }
    }
}

llvm::Value* ElementToVector(llvm::Value *elem[], llvm::Type *int32Ty, llvm::Instruction *insert_before, int vsize)
{
    llvm::VectorType *vt = llvm::VectorType::get(elem[0]->getType(), vsize);
    llvm::Value *vecValue = llvm::UndefValue::get(vt);

    for (int i = 0; i < vsize; ++i)
    {
        vecValue = llvm::InsertElementInst::Create(vecValue, elem[i], llvm::ConstantInt::get(int32Ty, i), "", insert_before);
    }
    return vecValue;
}

bool IsUnsignedCmp(const llvm::CmpInst::Predicate Pred)
{
    switch (Pred) {
    case llvm::CmpInst::ICMP_UGT:
    case llvm::CmpInst::ICMP_UGE:
    case llvm::CmpInst::ICMP_ULT:
    case llvm::CmpInst::ICMP_ULE:
        return true;
    default:
        break;
    }
    return false;
}

bool IsSignedCmp(const llvm::CmpInst::Predicate Pred)
{
    switch (Pred)
    {
    case llvm::CmpInst::ICMP_SGT:
    case llvm::CmpInst::ICMP_SGE:
    case llvm::CmpInst::ICMP_SLT:
    case llvm::CmpInst::ICMP_SLE:
        return true;
    default:
        break;
    }
    return false;
}

// isA64Ptr - Queries whether given pointer type requires 64-bit representation in vISA
bool isA64Ptr(llvm::PointerType *PT, CodeGenContext* pContext)
{
    return pContext->getRegisterPointerSizeInBits(PT->getAddressSpace()) == 64;
}

bool IsBitCastForLifetimeMark(const llvm::Value *V)
{
    if (!V || !llvm::isa<llvm::BitCastInst>(V))
    {
        return false;
    }
    for (llvm::Value::const_user_iterator it = V->user_begin(), e = V->user_end(); it != e; ++it)
    {
        const llvm::IntrinsicInst *inst = llvm::dyn_cast<const llvm::IntrinsicInst>(*it);
        if (!inst)
        {
            return false;
        }
        llvm::Intrinsic::ID  IID = inst->getIntrinsicID();
        if (IID != llvm::Intrinsic::lifetime_start &&
            IID != llvm::Intrinsic::lifetime_end)
        {
            return false;
        }
    }
    return true;
}

Value* mutatePtrType(Value* ptrv, PointerType* newType,
    IRBuilder<>& builder, const Twine&)
{
    if (isa<ConstantPointerNull>(ptrv))
    {
        return ConstantPointerNull::get(newType);
    }
    else
    {
        if (ConstantExpr* cexpr = dyn_cast<ConstantExpr>(ptrv))
        {
            assert(cexpr->getOpcode() == Instruction::IntToPtr);
            Value* offset = cexpr->getOperand(0);
            ptrv = builder.CreateIntToPtr(offset, newType);
        }
        else
        {
            ptrv->mutateType(newType);
        }
    }
    return ptrv;
}

/*
cmp.l.f0.0 (8) null:d       r0.0<0;1,0>:w    0x0000:w         { Align1, N1, NoMask, NoCompact }
(-f0.0) jmpi Test
(-f0.0) sendc (8) null:ud      r120.0<0;1,0>:f  0x00000025  0x08031400:ud    { Align1, N1, EOT, NoCompact }
nop
Test :
nop

*/

static const unsigned int CRastHeader_SIMD8[] =
{
    0x05600010,0x20001a24,0x1e000000,0x00000000,
    0x00110020,0x34000004,0x0e001400,0x00000020,
    0x05710032,0x20003a00,0x06000f00,0x88031400,
    0x00000000,0x00000000,0x00000000,0x00000000,
};

/*
cmp.l.f0.0 (16) null:d       r0.0 < 0; 1, 0 > : w    0x0000 : w{ Align1, N1, NoMask, NoCompact }
(-f0.0) jmpi(1) Test { Align1, N1, NoMask, NoCompact }
(-f0.0) sendc(16) null : ud      r120.0 < 0; 1, 0 > : f  0x00000025 0x90031000 : ud{ Align1, N1, EOT, NoCompact }
nop
Test :
nop

*/
static const unsigned int CRastHeader_SIMD16[] =
{
    0x05800010, 0x20001A24, 0x1E000000, 0x00000000,
    0x00110020, 0x34000004, 0x0E001400, 0x00000020,
    0x05910032, 0x20003A00, 0x06000F00, 0x90031000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

/*
cmp.l.f0.0 (16) null:d       r0.0 < 0; 1, 0 > : w    0x0000 : w{ Align1, N1, NoMask, NoCompact }
(-f0.0) jmpi Test
(-f0.0) sendc(16) null : w r120.0 < 0; 1, 0 > : ud  0x00000005 0x10031000 : ud{ Align1, N1, NoCompact }
(-f0.0) sendc(16) null : w r120.0 < 0; 1, 0 > : f  0x00000025  0x10031800 : ud{ Align1, N5, EOT, NoCompact }
nop
Test :
nop

*/

static const unsigned int CRastHeader_SIMD32[] =
{
    0x05800010,0x20001a24,0x1e000000,0x00000000,
    0x00110020,0x34000004,0x0e001400,0x00000020,
    0x05910032,0x20000260,0x06000f00,0x10031000,
    0x05912032,0x20003a60,0x06000f00,0x90031800,
};


unsigned int AppendConservativeRastWAHeader(IGC::SProgramOutput* program, SIMDMode simdmode)
{
     unsigned int headerSize = 0;
     const unsigned int* pHeader = nullptr;

    if (program && (program->m_programSize > 0 ))
    {
        switch (simdmode)
        {
        case SIMDMode::SIMD8: 
            headerSize = sizeof(CRastHeader_SIMD8);
            pHeader = CRastHeader_SIMD8;
            break;

        case SIMDMode::SIMD16: 
            headerSize = sizeof(CRastHeader_SIMD16);
            pHeader = CRastHeader_SIMD16;
            break;

        case SIMDMode::SIMD32: 
            headerSize = sizeof(CRastHeader_SIMD32);
            pHeader = CRastHeader_SIMD32;
            break;

        default: assert("Invalid SIMD Mode for Conservative Raster WA");
                    break;
        }

        unsigned int newSize = program->m_programSize + headerSize;
        void* newBinary = IGC::aligned_malloc(newSize, 16);
        memcpy_s(newBinary, newSize, pHeader, headerSize);
        memcpy_s((char*)newBinary + headerSize, newSize, program->m_programBin, program->m_programSize);
        IGC::aligned_free(program->m_programBin);
        program->m_programBin = newBinary;
        program->m_programSize = newSize;
    }
    return headerSize;
}

bool DSDualPatchEnabled(class CodeGenContext* ctx)
{
    return ctx->platform.supportDSDualPatchDispatch() &&
        ctx->platform.WaDisableDSDualPatchMode() &&
        IGC_IS_FLAG_DISABLED(DisableDSDualPatch);
}

//
// Given a value, check if it is likely a positive number.
//
// This function works best if llvm.assume() is used in the bif libraries to
// give ValueTracking hints.  ex:
//
// size_t get_local_id(uint dim)
// {
//    size_t ret = __builtin_IB_get_local_id()
//    __builtin_assume(ret >= 0);
//    __builtin_assume(ret <= 0x0000ffff)
//    return ret;
// }
// 
// This implementation relies completly on native llvm functions
//
//
//
bool valueIsPositive(Value* V, const DataLayout *DL)
{
    bool isKnownNegative = false;
    bool isKnownPositive = false;
    llvm::ComputeSignBit(
        V, 
        isKnownPositive, 
        isKnownNegative, 
        *DL);

    return isKnownPositive;
}


} // namespace IGC
