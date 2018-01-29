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

#include "AdaptorCommon/ImplicitArgs.hpp"
#include "Compiler/Optimizer/OpenCLPasses/ProgramScopeConstants/ProgramScopeConstantAnalysis.hpp"
#include "Compiler/IGCPassSupport.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/Analysis/ValueTracking.h>
#include "common/LLVMWarningsPop.hpp"

using namespace llvm;
using namespace IGC;
using namespace IGC::IGCMD;

// Register pass to igc-opt
#define PASS_FLAG "igc-programscope-constant-analysis"
#define PASS_DESCRIPTION "Creates annotations for OpenCL program-scope structures"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(ProgramScopeConstantAnalysis, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
IGC_INITIALIZE_PASS_END(ProgramScopeConstantAnalysis, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char ProgramScopeConstantAnalysis::ID = 0;

ProgramScopeConstantAnalysis::ProgramScopeConstantAnalysis() : ModulePass(ID)
{
    initializeProgramScopeConstantAnalysisPass(*PassRegistry::getPassRegistry());
}

bool ProgramScopeConstantAnalysis::runOnModule(Module &M)
{
    DataVector inlineConstantBuffer;
    DataVector inlineGlobalBuffer;

    unsigned globalBufferAlignment   = 0;
    unsigned constantBufferAlignment = 0;

    BufferOffsetMap inlineProgramScopeOffsets;

    // maintains pointer information so we can patch in
    // actual pointer addresses in runtime.
    PointerOffsetInfoList pointerOffsetInfoList;

    LLVMContext& C = M.getContext();
    m_DL = &M.getDataLayout();

    for(Module::global_iterator I = M.global_begin(), E = M.global_end(); I != E; ++I)
    {
        GlobalVariable* globalVar = &(*I);
        
        PointerType* ptrType = cast<PointerType>(globalVar->getType());
        assert(ptrType && "The type of a global variable must be a pointer type");

        // Pointer's address space should be either constant or global
        // The ?: is a workaround for clang bug, clang creates string constants with private address sapce!
        // When clang bug is fixed it should become:
        // const unsigned AS = ptrType->getAddressSpace();
        const unsigned AS = ptrType->getAddressSpace() != ADDRESS_SPACE_PRIVATE ? ptrType->getAddressSpace() : ADDRESS_SPACE_CONSTANT;

        // local address space variables are also generated as GlobalVariables.
        // Ignore them here.
        if (AS == ADDRESS_SPACE_LOCAL)
        {
            continue;
        }

        if(AS != ADDRESS_SPACE_CONSTANT &&
           AS != ADDRESS_SPACE_GLOBAL)
        {
            assert(0 && "program scope variable with unexpected address space");
            continue;
        }

        DataVector &inlineProgramScopeBuffer = (AS == ADDRESS_SPACE_GLOBAL) ? inlineGlobalBuffer : inlineConstantBuffer;
        unsigned &bufferAlignment = (AS == ADDRESS_SPACE_GLOBAL) ? globalBufferAlignment : constantBufferAlignment;
        
        // The only way to get a null initializer is via an external variable.
        // Linking has already occurred; everything should be resolved.
        Constant* initializer = globalVar->getInitializer();
        assert(initializer && "Constant must be initialized");
        if (!initializer)
        {
            continue;
        }

        // For the constant address space, handle only composite types.
        // Scalar constants can be propagated, and putting them in the constant memory
        // breaks inline samplers.
        // With LLVM 4.0 PointerType is derived directly from Type, instead of SequentialType
        // Thus we need to check for !PointerType as well
        TODO("Handle scalars in constant address space more gracefully");
        if (AS == ADDRESS_SPACE_CONSTANT && 
            !isa<CompositeType>(ptrType->getElementType()) &&
            !isa<PointerType>(ptrType->getElementType()))
        {
            // Scalar constants are _usually_ propagated, but they won't be if they were
            // marked as volatile.  Samplers won't be marked as volatile, so if we see
            // a volatile load then this i32 was actually an int rather than an
            // inline sampler.
            bool volatileUse = false;
            for (auto U : globalVar->users())
            {
                if (auto *LI = dyn_cast<LoadInst>(U))
                {
                    if (LI->isVolatile())
                    {
                        volatileUse = true;
                        break;
                    }
                }
            }
            if (!volatileUse)
            {
                continue;
            }
        }

        // If this variable isn't used, don't add it to the buffer.
        if (globalVar->use_empty())
        {
            continue;
        }

        // Align the buffer.
        // If this is the first constant, set the initial alignment, otherwise add padding.
        if (!bufferAlignment)
        {
            bufferAlignment = m_DL->getPreferredAlignment(globalVar);
        }
        else
        {
            alignBuffer(inlineProgramScopeBuffer, m_DL->getPreferredAlignment(globalVar));
        }

        // Ok, buffer is aligned, remember where this inline variable starts.
        inlineProgramScopeOffsets[globalVar] = inlineProgramScopeBuffer.size();

        // Add the data to the buffer
        addData(initializer, inlineProgramScopeBuffer, pointerOffsetInfoList, inlineProgramScopeOffsets, AS);
    }

    MetaDataUtils *mdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
	ModuleMetaData *modMd = getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();

    if (inlineConstantBuffer.size() > 0)
    {
        // If we found something, add everything to the metadata.
        InlineProgramScopeBuffer ilpsb;
        for(unsigned char v : inlineConstantBuffer)
        {
            ilpsb.Buffer.push_back(v);
        }
        ilpsb.alignment = constantBufferAlignment;
        modMd->inlineConstantBuffers.push_back(ilpsb);

        // Just add the implicit argument to each function if a constant
        // buffer has been created.  This will technically burn a patch
        // token on kernels that don't actually use the buffer but it saves
        // us having to walk the def-use chain (we can't just check if a
        // constant is used in the kernel; for example, a global buffer
        // may contain pointers that in turn point into the constant
        // address space).
        for (auto &pFunc : M) 
        {
            if (pFunc.isDeclaration()) continue;
            
            SmallVector<ImplicitArg::ArgType, 1> implicitArgs;
            implicitArgs.push_back(ImplicitArg::CONSTANT_BASE);
            ImplicitArgs::addImplicitArgs(pFunc, implicitArgs, mdUtils);
        }

        mdUtils->save(C);
    }

    if (inlineGlobalBuffer.size() > 0)
    {
        // If we found something, add everything to the metadata.
        InlineProgramScopeBuffer ilpsb;
        for(unsigned char v : inlineGlobalBuffer)
        {
            ilpsb.Buffer.push_back(v);
        }
        ilpsb.alignment = globalBufferAlignment;
        modMd->inlineGlobalBuffers.push_back(ilpsb);

        for (auto &pFunc : M) 
        {
            if (pFunc.isDeclaration()) continue;
            
            SmallVector<ImplicitArg::ArgType, 1> implicitArgs;
            implicitArgs.push_back(ImplicitArg::GLOBAL_BASE);
            ImplicitArgs::addImplicitArgs(pFunc, implicitArgs, mdUtils);
        }

        mdUtils->save(C);
    }

    // Setup the metadata for pointer patch info to be utilized during
    // OCL codegen.

    if (pointerOffsetInfoList.size() > 0)
    {
        for (auto &info : pointerOffsetInfoList)
        {
            // We currently just use a single buffer at index 0; hardcode
            // the patch to reference it.

            if (info.AddressSpaceWherePointerResides == ADDRESS_SPACE_GLOBAL)
            {
				PointerProgramBinaryInfo ppbi;
				ppbi.PointerBufferIndex = 0;
				ppbi.PointerOffset = int_cast<int32_t>(info.PointerOffsetFromBufferBase);
				ppbi.PointeeBufferIndex = 0;
				ppbi.PointeeAddressSpace = info.AddressSpacePointedTo;
				modMd->GlobalPointerProgramBinaryInfos.push_back(ppbi);
            }
            else if (info.AddressSpaceWherePointerResides == ADDRESS_SPACE_CONSTANT)
            {
				PointerProgramBinaryInfo ppbi;
				ppbi.PointerBufferIndex = 0;
				ppbi.PointerOffset = int_cast<int32_t>(info.PointerOffsetFromBufferBase);
				ppbi.PointeeBufferIndex = 0;
				ppbi.PointeeAddressSpace = info.AddressSpacePointedTo;
				modMd->ConstantPointerProgramBinaryInfos.push_back(ppbi);
            }
            else
            {
                assert(0 && "trying to patch unsupported address space");
            }
        }

        mdUtils->save(C);
    }

    const bool changed = !inlineProgramScopeOffsets.empty();
    for (auto offset : inlineProgramScopeOffsets)
    {
        modMd->inlineProgramScopeOffsets[offset.first] = offset.second;
    }

    if (changed)
    {
        mdUtils->save(C);
    }

    return changed;
}

void ProgramScopeConstantAnalysis::alignBuffer(DataVector& buffer, unsigned int alignment)
{
    int bufferLen = buffer.size();
    int alignedLen = iSTD::Align(bufferLen, alignment);
    if (alignedLen > bufferLen)
    {
        buffer.insert(buffer.end(), alignedLen - bufferLen, 0);
    }
}

/////////////////////////////////////////////////////////////////
//
// WalkCastsToFindNamedAddrSpace()
//
// If a generic address space pointer is discovered, we attmept
// to walk back to find the named address space if we can.
//
static unsigned WalkCastsToFindNamedAddrSpace(const Value *val)
{
    assert(isa<PointerType>(val->getType()));

    const unsigned currAddrSpace = cast<PointerType>(val->getType())->getAddressSpace();

    if (currAddrSpace != ADDRESS_SPACE_GENERIC)
    {
        return currAddrSpace;
    }

    if (const Operator *op = dyn_cast<Operator>(val))
    {
        // look through the bitcast (to be addrspacecast in 3.4).
        if (op->getOpcode() == Instruction::BitCast ||
            op->getOpcode() == Instruction::AddrSpaceCast)
        {
            return WalkCastsToFindNamedAddrSpace(op->getOperand(0));
        }
        // look through the (inttoptr (ptrtoint @a)) combo.
        else if (op->getOpcode() == Instruction::IntToPtr)
        {
            if (const Operator *opop = dyn_cast<Operator>(op->getOperand(0)))
            {
                if (opop->getOpcode() == Instruction::PtrToInt)
                {
                    return WalkCastsToFindNamedAddrSpace(opop->getOperand(0));
                }
            }
        }
        // Just look through the gep if it does no offset arithmetic.
        else if (const GEPOperator *GEP = dyn_cast<GEPOperator>(op))
        {
            if (GEP->hasAllZeroIndices())
            {
                return WalkCastsToFindNamedAddrSpace(GEP->getPointerOperand());
            }
        }
    }

    return currAddrSpace;
}

void ProgramScopeConstantAnalysis::addData(Constant* initializer, 
                                           DataVector& inlineProgramScopeBuffer, 
                                           PointerOffsetInfoList &pointerOffsetInfoList,
                                           BufferOffsetMap &inlineProgramScopeOffsets,
                                           unsigned addressSpace)
{
    // Initial alignment padding before insert the current constant into the buffer.
    alignBuffer(inlineProgramScopeBuffer, m_DL->getABITypeAlignment(initializer->getType()));

    // We need to do extra work with pointers here: we don't know their actual addresses
    // at compile time so we find the offset from the base of the buffer they point to
    // so we can patch in the absolute address later.
    if (PointerType *ptrType = dyn_cast<PointerType>(initializer->getType()))
    {
        int64_t offset = 0;
        const unsigned int pointerSize = int_cast<unsigned int>(m_DL->getTypeAllocSize(ptrType));
        // This case is the most common: here, we look for a pointer that can be decomposed into
        // a base + offset with the base itself being another global variable previously defined.
        if (GlobalVariable *ptrBase = dyn_cast<GlobalVariable>(GetPointerBaseWithConstantOffset(initializer, offset, *m_DL)))
        {
            const unsigned pointedToAddrSpace = WalkCastsToFindNamedAddrSpace(initializer);

            assert(addressSpace == ADDRESS_SPACE_GLOBAL || addressSpace == ADDRESS_SPACE_CONSTANT);

            // We can only patch global and constant pointers.
            if ((pointedToAddrSpace == ADDRESS_SPACE_GLOBAL ||
                 pointedToAddrSpace == ADDRESS_SPACE_CONSTANT) &&
                (addressSpace == ADDRESS_SPACE_GLOBAL ||
                 addressSpace == ADDRESS_SPACE_CONSTANT))
            {
                auto iter = inlineProgramScopeOffsets.find(ptrBase);
                assert(iter != inlineProgramScopeOffsets.end());

                const uint64_t pointeeOffset = iter->second + offset;

                pointerOffsetInfoList.push_back(
                    PointerOffsetInfo(
                        addressSpace,
                        inlineProgramScopeBuffer.size(),
                        pointedToAddrSpace));

                // Insert just the offset of the pointer.  The base address of the buffer it points
                // to will be added to it at runtime.
                inlineProgramScopeBuffer.insert(
                    inlineProgramScopeBuffer.end(), (char*)&pointeeOffset, ((char*)&pointeeOffset) + pointerSize);
            }
            else
            {
                // Just insert zero here.  This may be some pointer to private that will be set sometime later
                // inside a kernel.  We can't patch it in so we just set it to zero here.
                inlineProgramScopeBuffer.insert(inlineProgramScopeBuffer.end(), pointerSize, 0);
            }
        }
        else if (ConstantPointerNull *CPN = dyn_cast<ConstantPointerNull>(initializer))
        {
            inlineProgramScopeBuffer.insert(inlineProgramScopeBuffer.end(), pointerSize, 0);
        }
        else if (isa<FunctionType>(ptrType->getElementType()))
        {
            // function pointers may be resolved anyway by the time we get to this pass?
            inlineProgramScopeBuffer.insert(inlineProgramScopeBuffer.end(), pointerSize, 0);
        }
        else if (ConstantExpr *ce = dyn_cast<ConstantExpr>(initializer))
        {
            if (ce->getOpcode() == Instruction::IntToPtr)
            {
                // intoptr can technically convert vectors of ints into vectors of pointers
                // in an LLVM sense but OpenCL has no vector of pointers type.
                if (isa<ConstantInt>(ce->getOperand(0))) {
                    uint64_t val = *cast<ConstantInt>(ce->getOperand(0))->getValue().getRawData();
                    inlineProgramScopeBuffer.insert(inlineProgramScopeBuffer.end(), (char*)&val, ((char*)&val) + pointerSize);
                } else {
                    addData(ce->getOperand(0), inlineProgramScopeBuffer, pointerOffsetInfoList, inlineProgramScopeOffsets, addressSpace);
                }
            }
            else if (GEPOperator *GEP = dyn_cast<GEPOperator>(ce))
            {
                for (auto &Op : GEP->operands())
                    if (Constant *C = dyn_cast<Constant>(&Op))
                        addData(C, inlineProgramScopeBuffer, pointerOffsetInfoList, inlineProgramScopeOffsets, addressSpace);
            }
            else
            {
                assert(0 && "unknown constant expression");
            }
        }
        else
        {
            // What other shapes can pointers take at the program scope?
            assert(0 && "unknown pointer shape encountered");
        }
    }
    else if (const UndefValue *UV = dyn_cast<UndefValue>(initializer))
    {
        // It's undef, just throw in zeros.
        const unsigned int zeroSize = int_cast<unsigned int>(m_DL->getTypeAllocSize(UV->getType()));
        inlineProgramScopeBuffer.insert(inlineProgramScopeBuffer.end(), zeroSize, 0);
    }
    // Must check for constant expressions before we start doing type-based checks
    else if (ConstantExpr* ce = dyn_cast<ConstantExpr>(initializer))
    {
        // Constant expressions are evil. We only handle a subset that we expect.
        // Right now, this means a bitcast, or a ptrtoint/inttoptr pair. 
        // Handle it by adding the source of the cast.
        if (ce->getOpcode() == Instruction::BitCast ||
            ce->getOpcode() == Instruction::AddrSpaceCast)
        {
            addData(ce->getOperand(0), inlineProgramScopeBuffer, pointerOffsetInfoList, inlineProgramScopeOffsets, addressSpace);
        }
        else if (ce->getOpcode() == Instruction::IntToPtr)
        {
            ConstantExpr* opExpr = dyn_cast<ConstantExpr>(ce->getOperand(0));
            assert(opExpr && opExpr->getOpcode() == Instruction::PtrToInt && "Unexpected operand of IntToPtr");
            addData(opExpr->getOperand(0), inlineProgramScopeBuffer, pointerOffsetInfoList, inlineProgramScopeOffsets, addressSpace);
        }
        else if (ce->getOpcode() == Instruction::PtrToInt)
        {
            addData(ce->getOperand(0), inlineProgramScopeBuffer, pointerOffsetInfoList, inlineProgramScopeOffsets, addressSpace);
        }
        else
        {
            assert(0 && "Unexpected constant expression type");
        }
    }
    else if (ConstantDataSequential* cds = dyn_cast<ConstantDataSequential>(initializer))
    {
        for (unsigned i=0; i < cds->getNumElements(); i++) {
            addData(cds->getElementAsConstant(i), inlineProgramScopeBuffer, pointerOffsetInfoList, inlineProgramScopeOffsets, addressSpace);
        }
    }
    else if (ConstantAggregateZero* cag = dyn_cast<ConstantAggregateZero>(initializer))
    {
        // Zero aggregates are filled with, well, zeroes.
        const unsigned int zeroSize = int_cast<unsigned int>(m_DL->getTypeAllocSize(cag->getType()));
        inlineProgramScopeBuffer.insert(inlineProgramScopeBuffer.end(), zeroSize, 0);
    }
    // If this is an sequential type which is not a CDS or zero, have to collect the values
    // element by element. Note that this is not exclusive with the two cases above, so the 
    // order of ifs is meaningful.
    else if (CompositeType* cmpType = dyn_cast<CompositeType>(initializer->getType()))
    {
        const int numElts = initializer->getNumOperands();
        for (int i = 0; i < numElts; ++i)
        {
            Constant* C = initializer->getAggregateElement(i);
            assert(C && "getAggregateElement returned null, unsupported constant");
            // Since the type may not be primitive, extra alignment is required.
            addData(C, inlineProgramScopeBuffer, pointerOffsetInfoList, inlineProgramScopeOffsets, addressSpace);
        }
    }
    // And, finally, we have to handle base types - ints and floats.
    else 
    {
        APInt intVal(32, 0, false);
        if (ConstantInt* ci = dyn_cast<ConstantInt>(initializer))
        {
            intVal = ci->getValue();
        }
        else if (ConstantFP* cfp = dyn_cast<ConstantFP>(initializer))
        {
            intVal = cfp->getValueAPF().bitcastToAPInt();
        }
        else
        {
            assert(0 && "Unsupported constant type");
        }
        
        int bitWidth = intVal.getBitWidth();
        assert((bitWidth % 8 == 0) && (bitWidth <= 64) && "Unsupported bitwidth");

        const uint64_t* val = intVal.getRawData();
        inlineProgramScopeBuffer.insert(inlineProgramScopeBuffer.end(), (char*)val, ((char*)val) + (bitWidth / 8));
    }    


    // final padding.  This gets used by the vec3 types that will insert zero padding at the
    // end after inserting the actual vector contents (this is due to sizeof(vec3) == 4 * sizeof(scalarType)).
    alignBuffer(inlineProgramScopeBuffer, m_DL->getABITypeAlignment(initializer->getType()));
}
