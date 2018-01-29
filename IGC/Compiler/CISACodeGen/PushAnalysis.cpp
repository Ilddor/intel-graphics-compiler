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
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include "common/LLVMWarningsPop.hpp"

#include "common/LLVMUtils.h"

#include "LLVMWarningsPush.hpp"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Function.h"
#include "LLVMWarningsPop.hpp"

#include "Compiler/CISACodeGen/PushAnalysis.hpp"
#include "Compiler/CISACodeGen/ShaderCodeGen.hpp"
#include "Compiler/CISACodeGen/PixelShaderCodeGen.hpp"
#include "Compiler/CISACodeGen/CISACodeGen.h"
#include "Compiler/CISACodeGen/WIAnalysis.hpp"
#include "common/igc_regkeys.hpp"
#include "Compiler/IGCPassSupport.h"
#include "LLVM3DBuilder/MetadataBuilder.h"
#include "Compiler/MetaDataUtilsWrapper.h"

#include "common/debug/Debug.hpp"

#include <list>

/***********************************************************************************
This File contains the logic to decide for each inputs and constant if the data 
should be pushed or pull.
In case the data is pushed we remove the load instruction replace the value by 
a function argument so that the liveness calculated is correct

************************************************************************************/
using namespace llvm;
using namespace IGC::IGCMD;

namespace IGC
{
#define PASS_FLAG "igc-push-value-info"
#define PASS_DESCRIPTION "hold push value information"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS true

//undef macros to avoid redefinition warnings 
#undef PASS_FLAG
#undef PASS_DESCRIPTION
#undef PASS_ANALYSIS

#define PASS_FLAG "igc-push-analysis"
#define PASS_DESCRIPTION "promotes the values to be arguments"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(PushAnalysis, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(PostDominatorTreeWrapperPass)
IGC_INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
IGC_INITIALIZE_PASS_DEPENDENCY(PullConstantHeuristics)
IGC_INITIALIZE_PASS_END(PushAnalysis, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

PushAnalysis::PushAnalysis()
    : ModulePass(ID)
    , m_cbToLoad((uint)-1)
    , m_maxStatelessOffset(0)
    , m_PDT(nullptr)
{
    initializePushAnalysisPass(*PassRegistry::getPassRegistry());
}

const uint32_t PushAnalysis::MaxConstantBufferIndexSize = 256;

/// The maximum number of input attributes that will be pushed as part of the payload.
/// One attribute is 4 dwords, so e.g. 16 means we allocate max 64 GRFs for input payload.
const uint32_t PushAnalysis::MaxNumOfPushedInputs = 24;

/// The size occupied by the tessellation header in dwords
const uint32_t PushAnalysis::TessFactorsURBHeader = 8;

/// Maximum number of attributes pushed
const uint32_t PushAnalysis::m_pMaxNumOfVSPushedInputs = 24;

const uint32_t PushAnalysis::m_pMaxNumOfHSPushedInputs = 24;

const uint32_t PushAnalysis::m_pMaxNumOfDSPushedInputs = 24;

const uint32_t PushAnalysis::m_pMaxNumOfGSPushedInputs = 24;

template < typename T > std::string to_string(const T& n)
{
    std::ostringstream stm;
    stm << n;
    return stm.str();
}

llvm::Argument* PushAnalysis::addArgumentAndMetadata(llvm::Type *pType, std::string argName, IGC::WIAnalysis::WIDependancy dependency)
{
    llvm::Argument *pArg = new llvm::Argument(pType, argName, m_pFunction);
    ModuleMetaData* modMD = getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();
    IGC::ArgDependencyInfoMD argDepInfoMD;
    argDepInfoMD.argDependency = dependency;
	modMD->pushInfo.pushAnalysisWIInfos.push_back(argDepInfoMD);

	//Add it to arglist and increment Argument Index
	m_argList.push_back(pArg);
	m_argIndex++;

	m_funcTypeChanged = true;

    return pArg;
}

bool PushAnalysis::IsStatelessCBLoad(
    llvm::Instruction *inst,
    llvm::GenIntrinsicInst* &pBaseAddress,
    unsigned int& offset)
{
    /*
        %8 = call float @genx.GenISA.RuntimeValue(i32 2)
        %9 = call float @genx.GenISA.RuntimeValue(i32 3)
        %10 = insertelement <2 x float> undef, float %8, i32 0
        %11 = insertelement <2 x float> %10, float %9, i32 1
        %12 = bitcast <2 x float> %11 to i64
        %13 = add i64 %12, 16
        %14 = inttoptr i64 %13 to <3 x float> addrspace(2)*
        %15 = load <3 x float> addrspace(2)* %14, align 16
    */
    if (!llvm::isa<llvm::LoadInst>(inst))
        return false;

    // %15 = load <3 x float> addrspace(2)* %14, align 16
    llvm::LoadInst *pLoad = llvm::cast<llvm::LoadInst>(inst);
    uint address_space = pLoad->getPointerAddressSpace();
    if (address_space != ADDRESS_SPACE_CONSTANT)
        return false;

    // % 14 = inttoptr i64 % 13 to <3 x float> addrspace(2)*
    llvm::IntToPtrInst *pAddress = llvm::dyn_cast<llvm::IntToPtrInst>(pLoad->getOperand(pLoad->getPointerOperandIndex()));
    if (pAddress == nullptr)
        return false;

    // % 13 = add i64 % 12, 16
    // This add might or might not be present necessarily.
    llvm::Instruction *pAdd = llvm::dyn_cast<llvm::Instruction>(pAddress->getOperand(0));
    llvm::Instruction *pBitcast = nullptr;
    llvm::ConstantInt *pConst = nullptr;

    if (pAdd != nullptr && pAdd->getOpcode() == llvm::Instruction::Add)
    {
        pBitcast = llvm::dyn_cast<llvm::BitCastInst>(pAdd->getOperand(0));
        pConst = llvm::dyn_cast<llvm::ConstantInt>(pAdd->getOperand(1));

        if (!pBitcast || !pConst)
            return false;

        offset = (uint)pConst->getZExtValue();
    }
    else
    {
        pBitcast = llvm::dyn_cast<llvm::BitCastInst>(pAddress->getOperand(0));
        if (!pBitcast)
            return false;

        offset = 0;
    }

    if (pBitcast == nullptr)
        return false;

    // % 12 = bitcast <2 x float> % 11 to i64
    llvm::InsertElementInst *pInsert1 = llvm::dyn_cast<llvm::InsertElementInst>(pBitcast->getOperand(0));
    if (pInsert1 == nullptr)
        return false;

    // %10 = insertelement <2 x float> undef, float %8, i32 0
    // %11 = insertelement <2 x float> % 10, float %9, i32 1
    llvm::VectorType *pVecTy = llvm::VectorType::get(llvm::Type::getFloatTy(inst->getContext()), 2);
    if (!pVecTy || pInsert1->getType() != pVecTy)
        return false;

    llvm::InsertElementInst *pInsert0 = llvm::dyn_cast<llvm::InsertElementInst>(pInsert1->getOperand(0));
    if (pInsert0 == nullptr)
        return false;

    llvm::GenIntrinsicInst *pRuntimeVal1 = llvm::dyn_cast<llvm::GenIntrinsicInst>(pInsert1->getOperand(1));
    llvm::GenIntrinsicInst *pRuntimeVal0 = llvm::dyn_cast<llvm::GenIntrinsicInst>(pInsert0->getOperand(1));

    if (pRuntimeVal0 == nullptr || pRuntimeVal1 == nullptr)
        return false;

    if (pRuntimeVal0->getIntrinsicID() != llvm::GenISAIntrinsic::GenISA_RuntimeValue ||
        pRuntimeVal1->getIntrinsicID() != llvm::GenISAIntrinsic::GenISA_RuntimeValue)
        return false;

    uint runtimeval0 = (uint)llvm::cast<llvm::ConstantInt>(pRuntimeVal0->getOperand(0))->getZExtValue();

    PushInfo& pushInfo = m_context->getModuleMetaData()->pushInfo;

    // first check if it's already picked up
    if (m_cbToLoad == runtimeval0)
    {
        pBaseAddress = pRuntimeVal0;
        return true;
    }
    else
    if (m_cbToLoad != (uint)-1)
    {
        // another cb is pickup and we only can do 1 cb
        return false;
    }

    // then check for static root descriptor so that we can do push safely
    for (auto it : pushInfo.pushableAddresses)
    {
        if (runtimeval0 * 4 == it.addressOffset && it.isStatic)
        {
            pBaseAddress = pRuntimeVal0;
            return true;
        }
    }

    // otherwise the descriptor could bound to uninitialized buffer and we
    // need to avoid pushing in control flow
    // Find the return BB or the return BB before discard lowering.

    bool searchForRetBBBeforeDiscard = false;
    BasicBlock* retBB = m_PDT->getRootNode()->getBlock();

    for (auto& II : m_pFunction->getEntryBlock())
    {
        if (isa<GenIntrinsicInst>(&II, GenISAIntrinsic::GenISA_InitDiscardMask))
        {
            searchForRetBBBeforeDiscard = true;
            break;
        }
    }
    
    if (searchForRetBBBeforeDiscard)
    {
        for (auto it = pred_begin(retBB), ie = pred_end(retBB); it != ie; ++it)
        {
            BasicBlock* predBB = *it;
            BranchInst* br = cast<BranchInst>(predBB->getTerminator());
            if (br->isUnconditional())
            {
                retBB = predBB;
                break;
            }
        }
    }

    if (m_DT->dominates(inst->getParent(), retBB))
    {
        for (auto it : pushInfo.pushableAddresses)
        {
            if (runtimeval0 * 4 == it.addressOffset)
            {
                pBaseAddress = pRuntimeVal0;
                return true;
            }
        }
    }

    return false;
}

void PushAnalysis::ReplaceStatelessCBLoad(
    llvm::Instruction *inst,
    llvm::GenIntrinsicInst* &pBaseAddress,
    const unsigned int& offset)
{
    assert(llvm::isa<llvm::LoadInst>(inst) && "Expected a load instruction");

    // Extract the 2 runtime values to figure out the buffer it came from
    const llvm::GenIntrinsicInst *pRuntimeVal0 = pBaseAddress;

    uint runtimeval0 = (uint)llvm::cast<llvm::ConstantInt>(pRuntimeVal0->getOperand(0))->getZExtValue();

    // Current driver support allows only 1 CB. So if one cb is already loaded then only allow those loads
    if (m_cbToLoad != (uint)-1 && m_cbToLoad != runtimeval0)
        return;

    m_cbToLoad = runtimeval0;

    // We need to extract the runtimeval0 so we would need to extract it correctly.
    uint64_t base_address = (uint64_t)runtimeval0 << 32;

    uint64_t address = base_address + offset;

	PushInfo &pushInfo = m_context->getModuleMetaData()->pushInfo;

    unsigned int num_elms = 1;
    llvm::Type *pTypeToPush = inst->getType();
    llvm::Value *pReplacedInst = nullptr;
    llvm::Type *pScalarTy = inst->getType();

    if (inst->getType()->isVectorTy())
    {
        num_elms = inst->getType()->getVectorNumElements();
        pTypeToPush = inst->getType()->getVectorElementType();
        llvm::Type *pVecTy = llvm::VectorType::get(pTypeToPush, num_elms);
        pReplacedInst = llvm::UndefValue::get(pVecTy);
        pScalarTy = pVecTy->getVectorElementType();
    }

    bool replaced = false;
    SmallVector< SmallVector<ExtractElementInst*, 1>, 4> extracts(num_elms);

    bool allExtract = LoadUsedByConstExtractOnly(cast<LoadInst>(inst), extracts);

    for (unsigned int i = 0; i < num_elms; ++i)
    {
        uint64_t final_address = address + i * (pScalarTy->getPrimitiveSizeInBits() / 8);
        auto it = pushInfo.statelessLoads.find(final_address);
        llvm::Value* value = nullptr;

        if (it != pushInfo.statelessLoads.end() || (offset < m_pullConstantHeuristics->getPushConstantThreshold(m_pFunction) * 8))
        {
            // The sum of all the 4 constant buffer read lengths must be <= size of 64 units. 
            // Each unit is of size 256-bit units. In some UMDs we program the 
            // ConstantRegisters and Constant Buffers in the ConstantBuffer0ReadLength. And this causes
            // shaders to crash when the read length is > 64 units. To be safer we program the total number of 
            // GRF's used to 32 registers.
            if (it == pushInfo.statelessLoads.end())
            {
                // We now put the Value as an argument to make sure its liveness starts 
                // at the beginning of the function and then we remove the instruction
                // We now put the Value as an argument to make sure its liveness starts 
                // at the beginning of the function and then we remove the instruction
                m_maxStatelessOffset = std::max(m_maxStatelessOffset, offset);
                value = addArgumentAndMetadata(pTypeToPush,
                    VALUE_NAME(std::string("cb_stateless_") + to_string(runtimeval0) + std::string("_")
                    + std::string("_offset_") + to_string(offset) + std::string("_elm") + to_string(i)),
                    WIAnalysis::UNIFORM);

                if (pTypeToPush != value->getType())
                    value = CastInst::CreateZExtOrBitCast(value, pTypeToPush, "", inst);

                pushInfo.statelessLoads[final_address] = m_argIndex;
            }
            else
            {
				assert((it->second <= m_argIndex) && "Function arguments list and metadata are out of sync!");
                value = m_argList[it->second];
                if (pTypeToPush != value->getType())
                    value = CastInst::CreateZExtOrBitCast(value, pTypeToPush, "", inst);
            }

            if (inst->getType()->isVectorTy())
            {
                if (!allExtract)
                {
                    pReplacedInst = llvm::InsertElementInst::Create(
                        pReplacedInst, value, llvm::ConstantInt::get(llvm::IntegerType::get(inst->getContext(), 32), i), "", inst);
                }
                else
                {
                    for (auto II : extracts[i])
                    {
                        II->replaceAllUsesWith(value);
                    }
                }
            }
            else
            {
                pReplacedInst = value;
            }

            replaced = true;
        }
    }

    if (replaced && !allExtract)
    {
        inst->replaceAllUsesWith(pReplacedInst);
    }
}

//
// Calculate the offset in buffer relative to the dynamic uniform buffer offset.
//
// Below is an example of pattern we're looking for:
//
//   %runtime_value_1 = call fast float @genx.GenISA.RuntimeValue(i32 1)
//   %spv.bufferOffset.mdNode0.cr1 = bitcast float %runtime_value_1 to i32
//   %1 = and i32 %spv.bufferOffset.mdNode1.cr1, -16
//   %2 = add i32 %1, 24
//   %fromGBP45 = inttoptr i32 %2 to float addrspace(65536)* // BTI = 0
//   %ldrawidx46 = load float, float addrspace(65536)* %fromGBP45, align 4
// 
// For the above example the the function will:
//  - check if uniform buffer with bti=0 has its dynamic offset in 
//    runtime value 1
//  - return relativeOffsetInBytes = 24
// This method will return true if the input UBO is a dynamic uniform buffer
// and the input offset value is a sum of buffer's dynamic offset and an 
// immediate constant value.
// 
bool PushAnalysis::GetConstantOffsetForDynamicUniformBuffer(
    uint   bufferId, // buffer id (BTI)
    Value *offsetValue, // total buffer offset i.e. starting from 0
    uint  &relativeOffsetInBytes) // offset in bytes starting from dynamic offset in buffer
{
    if (ConstantInt *constOffset = dyn_cast<ConstantInt>(offsetValue))
    {
        relativeOffsetInBytes = int_cast<uint>(constOffset->getZExtValue());
        return true;
    }
    else if (Operator::getOpcode(offsetValue) == Instruction::Add)
    {
        const Instruction *addInst = cast<Instruction>(offsetValue);
        uint offset0 = 0, offset1 = 0;

        if (GetConstantOffsetForDynamicUniformBuffer(bufferId, addInst->getOperand(0), offset0) &&
            GetConstantOffsetForDynamicUniformBuffer(bufferId, addInst->getOperand(1), offset1))
        {
            relativeOffsetInBytes = offset0 + offset1;
            return true;
        }
    }
    else if (Operator::getOpcode(offsetValue) == Instruction::Mul)
    {
        const Instruction *mulInst = cast<Instruction>(offsetValue);
        uint a = 0, b = 0;

        if (GetConstantOffsetForDynamicUniformBuffer(bufferId, mulInst->getOperand(0), a) &&
            GetConstantOffsetForDynamicUniformBuffer(bufferId, mulInst->getOperand(1), b))
        {
            relativeOffsetInBytes = a * b;
            return true;
        }

    }
    else if (Operator::getOpcode(offsetValue) == Instruction::Shl)
    {
        const Instruction *shlInst = cast<Instruction>(offsetValue);
        uint offset = 0, bitShift = 0;

        if (GetConstantOffsetForDynamicUniformBuffer(bufferId, shlInst->getOperand(0), offset) &&
            GetConstantOffsetForDynamicUniformBuffer(bufferId, shlInst->getOperand(1), bitShift))
        {
            relativeOffsetInBytes = offset << bitShift;
            return true;
        }
    }
    else if (Operator::getOpcode(offsetValue) == Instruction::And)
    {
        const Instruction *andInst = cast<Instruction>(offsetValue);
        ConstantInt* src1 = dyn_cast<ConstantInt>(andInst->getOperand(1));
        if (src1 && 
            (int_cast<uint>(src1->getZExtValue()) == 0xFFFFFFE0 || int_cast<uint>(src1->getZExtValue()) == 0xFFFFFFF0) &&
            isa<BitCastInst>(andInst->getOperand(0)))
        {
            uint offset = 0;
            if (GetConstantOffsetForDynamicUniformBuffer(bufferId, andInst->getOperand(0), offset) &&
                offset == 0)
            {
                relativeOffsetInBytes = 0;
                return true;
            }
        }
    }
    else if (Operator::getOpcode(offsetValue) == Instruction::Or)
    {
        Instruction *orInst = cast<Instruction>(offsetValue);
        Instruction *src0 = dyn_cast<Instruction>(orInst->getOperand(0));
        ConstantInt* src1 = dyn_cast<ConstantInt>(orInst->getOperand(1));
        if (src1 && src0 && 
            src0->getOpcode() == Instruction::And &&
            isa<ConstantInt>(src0->getOperand(1)))
        { 
            uint orOffset = int_cast<uint>(src1->getZExtValue());
            uint andMask  = int_cast<uint>(cast<ConstantInt>(src0->getOperand(1))->getZExtValue());
            uint offset = 0;
            if ((orOffset & andMask) == 0 &&
                GetConstantOffsetForDynamicUniformBuffer(bufferId, src0->getOperand(0), offset) &&
                offset == 0)
            {
                relativeOffsetInBytes = orOffset;
                return true;
            }
        }
    }
    else if (BitCastInst *bitCast = dyn_cast<BitCastInst>(offsetValue))
    {
        if (GenIntrinsicInst* genIntr = dyn_cast<GenIntrinsicInst>(bitCast->getOperand(0)))
        {
            if (genIntr->getIntrinsicID() == GenISAIntrinsic::GenISA_RuntimeValue)
            {
                if (MDNode* bufIdMd = genIntr->getMetadata("dynamicBufferOffset.bufferId"))
                {
                    ConstantInt* bufIdMdVal = mdconst::extract<ConstantInt>(bufIdMd->getOperand(0));
                    if (bufferId == int_cast<uint>(bufIdMdVal->getZExtValue()))
                    {
                        relativeOffsetInBytes = 0;
                        return true;
                    }
                }
            }
        }
    }
    else if (IntToPtrInst *i2p = dyn_cast<IntToPtrInst>(offsetValue))
    {
        return GetConstantOffsetForDynamicUniformBuffer(bufferId, i2p->getOperand(0), relativeOffsetInBytes);
    }

    return false;
}

/// The constant-buffer id and element id must be compile-time immediate
/// in order to be added to thread-payload
bool PushAnalysis::IsPushableShaderConstant(Instruction *inst, uint &bufId, uint &eltId)
{
    Value *eltPtrVal = nullptr;

    if (inst->getType()->isVectorTy())
    {
        if (!(inst->getType()->getVectorElementType()->isFloatTy() ||
            inst->getType()->getVectorElementType()->isIntegerTy(32)))
            return false;
    }
    else
    {
        if (!(inst->getType()->isFloatTy() || inst->getType()->isIntegerTy(32)))
            return false;
    }

    // \todo, not support vector-load yet
    if (IsLoadFromDirectCB(inst, bufId, eltPtrVal))
    {
        if(bufId == getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData()->pushInfo.inlineConstantBufferSlot)
        {
            return false;
        }
        if (isa<ConstantPointerNull>(eltPtrVal))
        {
            eltId = 0;
            return true;
        }
        if (IntToPtrInst *i2p = dyn_cast<IntToPtrInst>(eltPtrVal))
        {
            Value *eltIdxVal = i2p->getOperand(0);
            if (ConstantInt *eltIdx = dyn_cast<ConstantInt>(eltIdxVal))
            {
                eltId = int_cast<uint>(eltIdx->getZExtValue());
                if((eltId % 4) == 0)
                {
                    eltId = eltId >> 2;
                }
                return true;
            }
        }

        if (m_context->m_DriverInfo.SupportsDynamicUniformBuffers() && IGC_IS_FLAG_DISABLED(DisableSimplePushWithDynamicUniformBuffers))
        {
            unsigned int relativeOffsetInBytes = 0; // offset in bytes starting from dynamic buffer offset
            if (GetConstantOffsetForDynamicUniformBuffer(bufId, eltPtrVal, relativeOffsetInBytes))
            {
                // Change to DWORDs
                eltId = relativeOffsetInBytes >> 2;
                return true;        
            }
        }
        return false;
    }
    return false;
}

bool PushAnalysis::AreUniformInputsBasedOnDispatchMode()
{
    if(m_context->type == ShaderType::DOMAIN_SHADER)
    {
        return true;
    }
    else if(m_context->type == ShaderType::HULL_SHADER)
    {
        if(m_hsProps->GetProperties().m_pShaderDispatchMode != EIGHT_PATCH_DISPATCH_MODE)
        {
            return true;
        }
    }
    return false;
}

bool PushAnalysis::DispatchGRFHardwareWAForHSAndGSDisabled()
{
    // If the WA does not apply, we can push constants.
    if(!m_context->platform.WaDispatchGRFHWIssueInGSAndHSUnit()) 
    {
        return true;
    }

    if (m_context->type == ShaderType::HULL_SHADER)
    {
        if (m_hsProps->GetProperties().m_pShaderDispatchMode == EIGHT_PATCH_DISPATCH_MODE)
        {
            auto tooManyHandles =
                (m_hsProps->GetProperties().m_pInputControlPointCount >= 29);
            // Can push constants if urb handles don't take too many registers.
            return !tooManyHandles;
        }
        return true;
    }
    else if (m_context->type == ShaderType::GEOMETRY_SHADER)
    {
        // If we need to consider the WA, do the computations
        auto inputVertexCount = m_gsProps->GetProperties().Input().VertexCount();
        auto tooManyHandles =
            inputVertexCount > 13 || (inputVertexCount > 12 && m_gsProps->GetProperties().Input().HasPrimitiveID());

        // Can push constants if urb handles don't take too many registers.
        return !tooManyHandles;
    }
    return true;
}



bool PushAnalysis::CanPushConstants()
{
    if(IGC_IS_FLAG_ENABLED(DisablePushConstant))
    {
        return false;
    }

    if (!m_context->getModuleMetaData()->compOpt.PushConstantsEnable)
    {
        return false;
    }

   
    switch(m_context->type)
    {
    case ShaderType::VERTEX_SHADER:
        if(m_context->platform.WaDisableVSPushConstantsInFusedDownModeWithOnlyTwoSubslices())
        {
            return false;
        }
        return true;

    case ShaderType::HULL_SHADER:
        return DispatchGRFHardwareWAForHSAndGSDisabled() &&
            !m_context->m_DriverInfo.WaDisablePushConstantsForHS();

    case ShaderType::GEOMETRY_SHADER:
        return DispatchGRFHardwareWAForHSAndGSDisabled();

    case ShaderType::DOMAIN_SHADER:
        if(m_context->platform.WaDisableDSPushConstantsInFusedDownModeWithOnlyTwoSubslices())
        {
            return false;
        }
        return true;

    case ShaderType::PIXEL_SHADER:
    {
        NamedMDNode* coarseNode = m_pFunction->getParent()->getNamedMetadata("coarse_phase");
        NamedMDNode* pixelNode = m_pFunction->getParent()->getNamedMetadata("pixel_phase");
        if(coarseNode && pixelNode)
        {
            Function* pixelPhase = llvm::mdconst::dyn_extract<Function>(pixelNode->getOperand(0)->getOperand(0));
            if(pixelPhase == m_pFunction)
            {
                // no push constants for pixel phase
                return false;
            }
        }
    }
        return true;
    default:
        break;
    }
    return false;
}

unsigned int PushAnalysis::GetMaxNumberOfPushedInputs()
{
    switch(m_context->type)
    {
    case ShaderType::VERTEX_SHADER:
        return m_pMaxNumOfVSPushedInputs;
    case ShaderType::HULL_SHADER:
        return m_hsProps->GetProperties().GetMaxInputPushed();
    case ShaderType::DOMAIN_SHADER:
        return m_pMaxNumOfDSPushedInputs;
    case ShaderType::GEOMETRY_SHADER:
    {
        const unsigned int MaxGSPayloadGRF = 96;
        const unsigned int totalPayloadSize = m_gsProps->GetProperties().Input().VertexCount() *
            Unit<Element>(m_gsProps->GetProperties().Input().PerVertex().Size()).Count();

        // For now we either return the vertex size (so we push all attributes) or zero
        // (so we use pure pull model).
        return totalPayloadSize <= MaxGSPayloadGRF ?
            m_gsProps->GetProperties().Input().PerVertex().Size().Count() : 0;
    }
    default:
        break;
    }
    return 0;
}

unsigned int PushAnalysis::AllocatePushedConstant(
    Instruction* load, unsigned int cbIdx, unsigned int offset, unsigned int maxSizeAllowed)
{
    unsigned int size = load->getType()->getPrimitiveSizeInBits() / 8;
    assert(isa<LoadInst>(load) && "Expected a load instruction");
	PushInfo &pushInfo = m_context->getModuleMetaData()->pushInfo;
    
    bool canPromote = false;
    unsigned int sizeGrown = 0;
    // greedy allocation for now
    // first check if we are already pushing from the buffer
	unsigned int piIndex;
	bool cbIdxFound = false;
	for (piIndex = 0; piIndex < pushInfo.simplePushBufferUsed; piIndex++)
    {
        if(pushInfo.simplePushInfoArr[piIndex].cbIdx == cbIdx)
        {
			cbIdxFound = true;
            break;
        }
    }
	if(cbIdxFound)
    {
		unsigned int newStartOffset = iSTD::RoundDown(std::min(offset, pushInfo.simplePushInfoArr[piIndex].offset), SIZE_GRF);
		unsigned int newEndOffset = iSTD::Round(std::max(offset + size, pushInfo.simplePushInfoArr[piIndex].offset + pushInfo.simplePushInfoArr[piIndex].size), SIZE_GRF);
        unsigned int newSize = newEndOffset - newStartOffset;
        
		if (newSize - pushInfo.simplePushInfoArr[piIndex].size <= maxSizeAllowed)
        {
            sizeGrown = newSize - pushInfo.simplePushInfoArr[piIndex].size;
            canPromote = true;
			pushInfo.simplePushInfoArr[piIndex].offset = newStartOffset;
			pushInfo.simplePushInfoArr[piIndex].size = newSize;
        }
    }

	const unsigned int maxNumberOfPushedBuffers = pushInfo.MaxNumberOfPushedBuffers;

    // we couldn't add it to an existing buffer try to add a new one if there is a slot available
    if(canPromote == false && 
        maxSizeAllowed > 0 && 
        pushInfo.simplePushBufferUsed < maxNumberOfPushedBuffers)
    {
        unsigned int newStartOffset = iSTD::RoundDown(offset, SIZE_GRF);
        unsigned int newEndOffset = iSTD::Round(offset + size, SIZE_GRF);
        unsigned int newSize = newEndOffset - newStartOffset;

        if(newSize <= maxSizeAllowed)
        {
            canPromote = true;
            sizeGrown = newSize;

			piIndex = pushInfo.simplePushBufferUsed;
			pushInfo.simplePushInfoArr[piIndex].cbIdx = cbIdx;
			pushInfo.simplePushInfoArr[piIndex].offset = newStartOffset;
			pushInfo.simplePushInfoArr[piIndex].size = newSize;
			
            pushInfo.simplePushBufferUsed++;
        }
    }

    if(canPromote)
    {
        // promote the load to be pushed
        PromoteLoadToSimplePush(load, pushInfo.simplePushInfoArr[piIndex], offset);
    }
    return sizeGrown;
}

void PushAnalysis::PromoteLoadToSimplePush(Instruction* load, SimplePushInfo& info, unsigned int offset)
{
    unsigned int num_elms = 1;
    llvm::Type *pTypeToPush = load->getType();
    llvm::Value *pReplacedInst = nullptr;
    llvm::Type *pScalarTy = pTypeToPush;

    if(pTypeToPush->isVectorTy())
    {
        num_elms = pTypeToPush->getVectorNumElements();
        pTypeToPush = pTypeToPush->getVectorElementType();
        llvm::Type *pVecTy = llvm::VectorType::get(pTypeToPush, num_elms);
        pReplacedInst = llvm::UndefValue::get(pVecTy);
        pScalarTy = pVecTy->getVectorElementType();
    }

    SmallVector< SmallVector<ExtractElementInst*, 1>, 4> extracts(num_elms);
    bool allExtract = LoadUsedByConstExtractOnly(cast<LoadInst>(load), extracts);

    for(unsigned int i = 0; i < num_elms; ++i)
    {
        uint address = offset + i * (pScalarTy->getPrimitiveSizeInBits() / 8);
        auto it = info.simplePushLoads.find(address);
        llvm::Value* value = nullptr;
        if(it != info.simplePushLoads.end())
        {
            // Value is already getting pushed
			assert((it->second <= m_argIndex) && "Function arguments list and metadata are out of sync!");
            value = m_argList[it->second];
            if(pTypeToPush != value->getType())
                value = CastInst::CreateZExtOrBitCast(value, pTypeToPush, "", load);
        }
        else
        {
            value = addArgumentAndMetadata(pTypeToPush, VALUE_NAME("cb"), WIAnalysis::UNIFORM);
            if(pTypeToPush != value->getType())
                value = CastInst::CreateZExtOrBitCast(value, pTypeToPush, "", load);
            info.simplePushLoads.insert(std::make_pair(address, m_argIndex));
        }

        if(load->getType()->isVectorTy())
        {
            if(!allExtract)
            {
                pReplacedInst = llvm::InsertElementInst::Create(
                    pReplacedInst, value, llvm::ConstantInt::get(llvm::IntegerType::get(load->getContext(), 32), i), "", load);
            }
            else
            {
                for(auto II : extracts[i])
                {
                    II->replaceAllUsesWith(value);
                }
            }
        }
        else
        {
            pReplacedInst = value;
        }

    }
    if(!allExtract)
    {
        load->replaceAllUsesWith(pReplacedInst);
    }
}


void PushAnalysis::BlockPushConstants()
{
    // push up to 31 GRF
    static const unsigned int cthreshold = m_pullConstantHeuristics->getPushConstantThreshold(m_pFunction) * SIZE_GRF;
    unsigned int sizePushed = 0;
    m_entryBB = &m_pFunction->getEntryBlock();

    // Runtime values are changed to intrinsics. So we need to do it before.
    for (auto bb = m_pFunction->begin(), be = m_pFunction->end(); bb != be; ++bb)
    {
        for (auto i = bb->begin(), ie = bb->end(); i != ie; ++i)
        {
            unsigned int cbId = 0;
            unsigned int offset = 0;
            if (IsPushableShaderConstant(&(*i), cbId, offset))
            {
                // convert offset in bytes
                offset = offset << 2;
                sizePushed += AllocatePushedConstant(&(*i), cbId, offset, cthreshold - sizePushed);
            }
        }
    }
}


void PushAnalysis::StatlessPushConstant()
{
    // skip it if we are not allowed to push or if there are no pushable pointers
    if(IGC_IS_FLAG_DISABLED(DisableStatelessPushConstant) &&
        m_context->getModuleMetaData()->pushInfo.pushableAddresses.size() > 0)
    {
        m_PDT = &getAnalysis<PostDominatorTreeWrapperPass>(*m_pFunction).getPostDomTree();
        m_DT = &getAnalysis<DominatorTreeWrapperPass>(*m_pFunction).getDomTree();
        m_entryBB = &m_pFunction->getEntryBlock();

        // Runtime values are changed to intrinsics. So we need to do it before.
        for(auto bb = m_pFunction->begin(), be = m_pFunction->end(); bb != be; ++bb)
        {
            for(auto i = bb->begin(), ie = bb->end(); i != ie; ++i)
            {
                llvm::SmallVector<llvm::GenIntrinsicInst*, 2> baseAddress;
                uint offset = 0;

                llvm::GenIntrinsicInst *pBaseAddress = nullptr;
                if(IsStatelessCBLoad(&(*i), pBaseAddress, offset))
                    ReplaceStatelessCBLoad(&(*i), pBaseAddress, offset);
            }
        }
    }
}

PushConstantMode PushAnalysis::GetPushConstantMode()
{
    PushConstantMode pushConstantMode = PushConstantMode::NO_PUSH_CONSTANT;
    if(CanPushConstants())
    {
        if(IGC_IS_FLAG_ENABLED(forcePushConstantMode))
        {
            pushConstantMode = (PushConstantMode)IGC_GET_FLAG_VALUE(forcePushConstantMode);
        }
        else if(m_context->m_DriverInfo.SupportsSimplePushOnly())
        {
            pushConstantMode = PushConstantMode::SIMPLE_PUSH;
        }
        else if(m_context->platform.supportsHardwareResourceStreamer())
        {
            pushConstantMode = PushConstantMode::GATHER_CONSTANT;
        }
        else if(m_context->m_DriverInfo.SupportsHWResourceStreameAndSimplePush())
        {
            pushConstantMode = PushConstantMode::SIMPLE_PUSH;
        }
        else
        {
            //CPU copy
            pushConstantMode = PushConstantMode::GATHER_CONSTANT;
        }
    }
    return pushConstantMode;
}


//// Max number of control point inputs in 8 patch beyond which we always pull
/// and do not try to use a hybrid approach of pull and push
void PushAnalysis::ProcessFunction()
{
    m_context = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
    // if it's GS, get the properties object and find out if we use instancing
    // since then payload is laid out differently.
    const bool gsInstancingUsed = m_gsProps && m_gsProps->GetProperties().Input().HasInstancing();

    uint32_t vsUrbReadIndexForInstanceIdSGV = 0;
    bool vsHasConstantBufferIndexedWithInstanceId = false;

	m_funcTypeChanged = false;	// Reset flag at the beginning of processing every function
    if(m_context->type == ShaderType::VERTEX_SHADER)
    {
        llvm::NamedMDNode *pMetaData = m_pFunction->getParent()->getNamedMetadata("ConstantBufferIndexedWithInstanceId");

        if (pMetaData != nullptr)
        {
            llvm::MDNode *pMdNode = pMetaData->getOperand(0);
            if (pMdNode)
            {
                ConstantInt* pURBReadIndexForInstanceIdSGV = mdconst::dyn_extract<ConstantInt>(pMdNode->getOperand(0));
                vsUrbReadIndexForInstanceIdSGV = int_cast<uint32_t>(pURBReadIndexForInstanceIdSGV->getZExtValue());
                vsHasConstantBufferIndexedWithInstanceId = true;
            }
        }
    }

    PushConstantMode pushConstantMode = GetPushConstantMode();

    if(m_context->type == ShaderType::DOMAIN_SHADER)
    {
        llvm::Argument* valueU = addArgumentAndMetadata(Type::getFloatTy(m_pFunction->getContext()), VALUE_NAME("DS_U"), WIAnalysis::RANDOM);
        llvm::Argument* valueV = addArgumentAndMetadata(Type::getFloatTy(m_pFunction->getContext()), VALUE_NAME("DS_V"), WIAnalysis::RANDOM);
        llvm::Argument* valueW = addArgumentAndMetadata(Type::getFloatTy(m_pFunction->getContext()), VALUE_NAME("DS_W"), WIAnalysis::RANDOM);
        m_dsProps->SetDomainPointUArgu(valueU);
        m_dsProps->SetDomainPointVArgu(valueV);
        m_dsProps->SetDomainPointWArgu(valueW);
    }

	PushInfo &pushInfo = m_context->getModuleMetaData()->pushInfo;
    if (pushConstantMode == PushConstantMode::SIMPLE_PUSH)
    {
        StatlessPushConstant();
        BlockPushConstants();
    }

    for (auto BB = m_pFunction->begin(), E = m_pFunction->end(); BB != E; ++BB)
    {
        llvm::BasicBlock::InstListType &instructionList = BB->getInstList();
        for (auto instIter = instructionList.begin(), endInstIter = instructionList.end(); instIter != endInstIter; ++instIter)
        {
            llvm::Instruction* inst = &(*instIter);
            // skip dead instructions
            if( inst->use_empty() )
            {
                continue;
            }
            // TODO: we can find a better heuristic to figure out which constant we want to push            
            if(m_context->type == ShaderType::DOMAIN_SHADER)
            {
                if( dyn_cast<GenIntrinsicInst>(inst) &&
                    dyn_cast<GenIntrinsicInst>(inst)->getIntrinsicID() == GenISAIntrinsic::GenISA_DCL_SystemValue)
                {
                    SGVUsage usage = static_cast<SGVUsage>(llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(0))->getZExtValue());
                    if (usage == DOMAIN_POINT_ID_X || usage == DOMAIN_POINT_ID_Y || usage == DOMAIN_POINT_ID_Z)
                    {
                        if( usage == DOMAIN_POINT_ID_X )
                        {
                            inst->replaceAllUsesWith(m_dsProps->GetProperties().m_UArg);
                        }
                        else if( usage == DOMAIN_POINT_ID_Y )
                        {
                            inst->replaceAllUsesWith(m_dsProps->GetProperties().m_VArg);
                        }
                        else if( usage == DOMAIN_POINT_ID_Z )
                        {
                            inst->replaceAllUsesWith(m_dsProps->GetProperties().m_WArg);
                        }
                    }
                }
            }
            // code to push constant-buffer value into thread-payload
            uint bufId, eltId;
            if(pushConstantMode == PushConstantMode::GATHER_CONSTANT && 
                IsPushableShaderConstant(inst, bufId, eltId))
            {
                if (!m_context->m_DriverInfo.Uses3DSTATE_DX9_CONSTANT() && (eltId + inst->getType()->getPrimitiveSizeInBits() / 8) >= (MaxConstantBufferIndexSize * 4))
                {
                    // Hardware supports pushing more than 256 constants
                    // in case 3DSTATE_DX9_CONSTANT mode is used
                    continue;
                }
                else if (bufId > 15)
                {
                    // resource streamer cannot push above buffer slot 15 and driver doesn't allow 
                    // pushing inlined constant buffer
                    //should not be pushed and should always be pulled
                    continue;
                }

                unsigned int num_elms = 
                    inst->getType()->isVectorTy() ? inst->getType()->getVectorNumElements() : 1;
                llvm::Type *pTypeToPush = inst->getType();
                llvm::Value *replaceVector = nullptr;
                unsigned int numberChannelReplaced = 0;
                SmallVector< SmallVector<ExtractElementInst*, 1>, 4> extracts(num_elms);
                bool allExtract = false;
                if (inst->getType()->isVectorTy())
                {
                    allExtract = LoadUsedByConstExtractOnly(cast<LoadInst>(inst), extracts);
                    pTypeToPush = inst->getType()->getVectorElementType();
                }

                for (unsigned int i = 0; i < num_elms; ++i)
                {
					ConstantAddress address;
					address.bufId = bufId;
					address.eltId = eltId + i;

                    auto it = pushInfo.constants.find(address);
                    if (it != pushInfo.constants.end() || (pushInfo.constantReg.size() + pushInfo.constants.size() < m_pullConstantHeuristics->getPushConstantThreshold(m_pFunction) * 8))
                    {
                        llvm::Value *value = nullptr;

                        // The sum of all the 4 constant buffer read lengths must be <= size of 64 units. 
                        // Each unit is of size 256-bit units. In some UMDs we program the 
                        // ConstantRegisters and Constant Buffers in the ConstantBuffer0ReadLength. And this causes
                        // shaders to crash when the read length is > 64 units. To be safer we program the total number of 
                        // GRF's used to 32 registers.
                        if (it == pushInfo.constants.end())
                        {
                            // We now put the Value as an argument to make sure its liveness starts 
                            // at the beginning of the function and then we remove the instruction
                            // We now put the Value as an argument to make sure its liveness starts 
                            // at the beginning of the function and then we remove the instruction
                            value = addArgumentAndMetadata(pTypeToPush,
                                VALUE_NAME(std::string("cb_") + to_string(bufId) + std::string("_") + to_string(eltId) + std::string("_elm") + to_string(i)),
                                WIAnalysis::UNIFORM);
                            if (pTypeToPush != value->getType())
                                value = CastInst::CreateZExtOrBitCast(value, pTypeToPush, "", inst);

                            pushInfo.constants[address] = m_argIndex;
                        }
                        else
                        {
							assert((it->second <= m_argIndex) && "Function arguments list and metadata are out of sync!");
                            value = m_argList[it->second];
                            if (pTypeToPush != value->getType())
                                value = CastInst::CreateZExtOrBitCast(value, pTypeToPush, "", inst);
                        }

                        if(inst->getType()->isVectorTy())
                        {
                            if(!allExtract)
                            {
                                numberChannelReplaced++;
                                if(replaceVector == nullptr)
                                {
                                    replaceVector = llvm::UndefValue::get(inst->getType());
                                }
                                replaceVector = llvm::InsertElementInst::Create(replaceVector, value, llvm::ConstantInt::get(llvm::IntegerType::get(inst->getContext(), 32), i), "", inst);
                            }
                            else
                            {
                                for(auto II : extracts[i])
                                {
                                    II->replaceAllUsesWith(value);
                                }
                            }
                        }
                        else
                        {
                            inst->replaceAllUsesWith(value);
                        }
                    }
                    if(replaceVector != nullptr && numberChannelReplaced == num_elms)
                    {
                        // for vector replace, we only replace the load if we were going to push
                        // all the channels
                        inst->replaceAllUsesWith(replaceVector);
                    }
                }
            }
            else if(GetOpCode(inst) == llvm_input)
            {
                e_interpolation mode = (e_interpolation) llvm::cast<llvm::ConstantInt>(inst->getOperand(1))->getZExtValue();
                if(
                    mode == EINTERPOLATION_VERTEX)
                {
                    // inputs which get pushed are set as function arguments in order to have the correct liveness
                    if (llvm::ConstantInt* pIndex = llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(0)))
                    {
                        SInputDesc input;
                        input.index = static_cast<uint>(pIndex->getZExtValue());
                        input.interpolationMode = mode;
                        // if we know input is uniform, update WI analysis results.
                        const bool uniformInput =
                            (mode == EINTERPOLATION_CONSTANT) || gsInstancingUsed;

                        auto it = pushInfo.inputs.find(input.index);
                        if( it == pushInfo.inputs.end())
                        {
                            assert(inst->getType()->isHalfTy() || inst->getType()->isFloatTy());
                            llvm::Type* floatTy = Type::getFloatTy(m_pFunction->getContext());
                            addArgumentAndMetadata(floatTy, VALUE_NAME(std::string("input_") + to_string(input.index)), uniformInput ? WIAnalysis::UNIFORM : WIAnalysis::RANDOM);
							input.argIndex = m_argIndex;
                            pushInfo.inputs[input.index] = input;
                        }
                        else
                        {
							assert((it->second.argIndex <= m_argIndex) && "Function arguments list and metadata are out of sync!");
                            input.argIndex = it->second.argIndex;
                        }
						llvm::Value* replacementValue = m_argList[input.argIndex];
                        if (inst->getType()->isHalfTy() && replacementValue->getType()->isFloatTy())
                        {
                            // Input is accessed using the half version of intrinsic, e.g.:
                            //     call half @genx.GenISA.DCL.inputVec.f16 (i32 13, i32 2)
                            replacementValue = CastInst::CreateFPCast(replacementValue, inst->getType(), "",  inst);
                        }
                        inst->replaceAllUsesWith(replacementValue);
                    }
                }
            }
            else if(GetOpCode(inst) == llvm_shaderinputvec)
            {
                // If the input index of llvm_shaderinputvec is a constant
                if( llvm::ConstantInt* pIndex = llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(0)) )
                {
                    uint inputIndex = static_cast<uint>(pIndex->getZExtValue());
                    e_interpolation mode = (e_interpolation) llvm::cast<llvm::ConstantInt>(inst->getOperand(1))->getZExtValue();

                    // If the input index of llvm_shaderinputvec is a constant then we pull inputs if inputIndex <= MaxNumOfPushedInputs
                    if( pIndex && inputIndex <= MaxNumOfPushedInputs )
                    {
                        if(mode==EINTERPOLATION_CONSTANT || mode == EINTERPOLATION_VERTEX)
                        {
                            for(auto I = inst->user_begin(), E = inst->user_end(); I!=E; ++I)
                            {
                                if(llvm::ExtractElementInst* extract = llvm::dyn_cast<llvm::ExtractElementInst>(*I))
                                {
                                    SInputDesc input;
                                    // if input is i1.xyzw, inputIndex = 1*4, extract->getIndexOperand() is the component
                                    input.index = inputIndex * 4 +
                                        int_cast<uint>(llvm::cast<ConstantInt>(extract->getIndexOperand())->getZExtValue());
                                    input.interpolationMode = mode;
                                    // if we know input is uniform, update WI analysis results.
                                    const bool uniformInput =
                                        (mode == EINTERPOLATION_CONSTANT) || gsInstancingUsed;

                                    auto it = pushInfo.inputs.find(input.index);
                                    if( it == pushInfo.inputs.end())
                                    {
                                        addArgumentAndMetadata(extract->getType(), VALUE_NAME(std::string("pushedinput_") + to_string(input.index)), uniformInput ? WIAnalysis::UNIFORM : WIAnalysis::RANDOM);
										input.argIndex = m_argIndex;
                                        pushInfo.inputs[input.index] = input;
                                    }
                                    else
                                    {
										assert((it->second.argIndex <= m_argIndex) && "Function arguments list and metadata are out of sync!");
                                        input.argIndex = it->second.argIndex;
                                    }

                                    extract->replaceAllUsesWith(m_argList[input.argIndex]);
                                }
                            }
                        }
                    }
                    else
                    {
                        // This should never happen for geometry shader since we leave GS specific
                        // intrinsic if we want pull model earlier in GS lowering.
                        assert(m_context->type != ShaderType::GEOMETRY_SHADER);
                    }
                }
            }
            else if(GenIntrinsicInst* intrinsic = dyn_cast<GenIntrinsicInst>(inst))
            {
                if ( DispatchGRFHardwareWAForHSAndGSDisabled() &&
                     (intrinsic->getIntrinsicID() == GenISAIntrinsic::GenISA_URBRead) && 
                     (m_context->type != ShaderType::DOMAIN_SHADER))
                {
                    if( llvm::ConstantInt* pVertexIndex = llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(0)) )
                    {
                        uint vertexIndex = static_cast<uint>(pVertexIndex->getZExtValue());
                        uint numberOfElementsPerVertexThatAreGoingToBePushed = GetMaxNumberOfPushedInputs();

                        // If the input index of llvm_shaderinputvec is a constant
                        if(llvm::ConstantInt* pElementIndex = llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1)))
                        {
                            uint elementIndex = static_cast<uint>(pElementIndex->getZExtValue());

                            uint currentElementIndex = (vertexIndex * numberOfElementsPerVertexThatAreGoingToBePushed * 4) +
                                (elementIndex * 4);

                            bool pushCondition = (elementIndex < numberOfElementsPerVertexThatAreGoingToBePushed);

                            // If the attribute index of URBRead is a constant then we pull 
                            // inputs if elementIndex <= minElementsPerVertexThatCanBePushed
                            if(pElementIndex && pushCondition)
                            {
                                for(auto I = inst->user_begin(), E = inst->user_end(); I != E; ++I)
                                {
                                    llvm::ExtractElementInst* extract = llvm::dyn_cast<llvm::ExtractElementInst>(*I);
                                    if (extract && llvm::isa<ConstantInt>(extract->getIndexOperand()))
                                    {
                                        SInputDesc input;
                                        // if input is i1.xyzw, elementIndex = 1*4, extract->getIndexOperand() is the component
                                        input.index =
                                            currentElementIndex +
                                            int_cast<uint>(cast<ConstantInt>(extract->getIndexOperand())->getZExtValue());
                                        input.interpolationMode = AreUniformInputsBasedOnDispatchMode() ?
                                            EINTERPOLATION_CONSTANT : EINTERPOLATION_VERTEX;

                                        const bool uniformInput =
                                            AreUniformInputsBasedOnDispatchMode() ||
                                            (vsHasConstantBufferIndexedWithInstanceId &&
                                            vsUrbReadIndexForInstanceIdSGV == input.index) ||
                                            gsInstancingUsed;

                                        auto it = pushInfo.inputs.find(input.index);
                                        if(it == pushInfo.inputs.end())
                                        {	
                                            addArgumentAndMetadata(extract->getType(), VALUE_NAME(std::string("urb_read_") + to_string(input.index)), uniformInput ? WIAnalysis::UNIFORM : WIAnalysis::RANDOM);
											input.argIndex = m_argIndex;
                                            pushInfo.inputs[input.index] = input;
                                        }
                                        else
                                        {
											assert((it->second.argIndex <= m_argIndex) && "Function arguments list and metadata are out of sync!");
                                            input.argIndex = it->second.argIndex;
                                        }
                                        extract->replaceAllUsesWith(m_argList[input.argIndex]);
                                    }
                                }
                            }
                        }
                    }
                }
                else if(intrinsic->getIntrinsicID() == GenISAIntrinsic::GenISA_RuntimeValue)
                {
                    uint index = (uint)llvm::cast<llvm::ConstantInt>(intrinsic->getOperand(0))->getZExtValue();
                    auto it = pushInfo.constantReg.find(index);
                    Value* arg = nullptr;
                    Value* runtimeValue = intrinsic;
                    if(it == pushInfo.constantReg.end())
                    {
                        while(runtimeValue->hasOneUse() &&
                            (isa<BitCastInst>(runtimeValue->user_back()) || 
                            isa<IntToPtrInst>(runtimeValue->user_back())))
                        {
                            Type* type = runtimeValue->user_back()->getType();
                            if (type->isPointerTy() && IGC::isA64Ptr(cast<PointerType>(type), m_context))
                            {
                                // Can't push 64bit pointer types
                                break;
                            }
                            runtimeValue = runtimeValue->user_back();
                        }
                        arg = addArgumentAndMetadata(
                            runtimeValue->getType(), 
                            VALUE_NAME(std::string("runtime_value_") + to_string(index)), 
                            WIAnalysis::UNIFORM);
                        pushInfo.constantReg[index] = m_argIndex;
                    }
                    else
                    {
						assert((it->second <= m_argIndex) && "Function arguments list and metadata are out of sync!");
                        arg = m_argList[it->second];
                        while(arg->getType() != runtimeValue->getType() &&
                            runtimeValue->hasOneUse() &&
                            (isa<BitCastInst>(runtimeValue->user_back()) ||
                            isa<IntToPtrInst>(runtimeValue->user_back())))
                        {
                            runtimeValue = runtimeValue->user_back();
                        }
                        if(arg->getType() != runtimeValue->getType())
                        {
                            if(arg->getType()->isPointerTy())
                            {
                                arg = CastInst::CreateBitOrPointerCast(
                                    arg, Type::getInt32Ty(arg->getContext()), "", cast<Instruction>(runtimeValue));
                            }
                            arg = CastInst::CreateBitOrPointerCast(
                                arg, runtimeValue->getType(), "", cast<Instruction>(runtimeValue));
                        }
                    }
                    runtimeValue->replaceAllUsesWith(arg);
                }
            }
        }
    }

	if (m_funcTypeChanged)
	{
		m_isFuncTypeChanged[m_pFunction] = true;
	}
	else
	{
		m_isFuncTypeChanged[m_pFunction] = false;
	}

    m_pMdUtils->save(m_pFunction->getContext());
}

void PushAnalysis::updateNewFuncArgs(llvm::Function* pFunc, llvm::Function* pNewFunc)
{
	// Loop over the argument list, transferring uses of the old arguments over to
	// the new arguments, also transferring over the names as well.
	std::map<void*, unsigned int> argMap;
	std::vector<std::pair<llvm::Instruction*, unsigned int>> newAddr;
	Function::arg_iterator I = pFunc->arg_begin();
	Function::arg_iterator E = pFunc->arg_end();
	Function::arg_iterator currArg = pNewFunc->arg_begin();
	llvm::Argument* oldArg;
	llvm::Argument* newArg;

	while (I != E)
	{
		if (currArg == pNewFunc->arg_end())
			break;

		oldArg = &(*I);
		newArg = &(*currArg);

		assert(oldArg->getType() == newArg->getType());
		
		// Move the name and users over to the new version.
		I->replaceAllUsesWith(newArg);
		currArg->takeName(&(*I));
        
        if (m_dsProps)
        {
            if (m_dsProps->GetDomainPointUArgu() == oldArg)
                m_dsProps->SetDomainPointUArgu(newArg);
            if (m_dsProps->GetDomainPointVArgu() == oldArg)
                m_dsProps->SetDomainPointVArgu(newArg);
            if (m_dsProps->GetDomainPointWArgu() == oldArg)
                m_dsProps->SetDomainPointWArgu(newArg);
        }
		++I; ++currArg;
	}
	while (I != E)
	{
		oldArg = &(*I);
		llvm::Argument* newArg = new llvm::Argument(oldArg->getType(), oldArg->getName(), pNewFunc);
		// Move the name and users over to the new version.
		I->replaceAllUsesWith(newArg);
		newArg->takeName(&(*I));

        if (m_dsProps)
        {
            if (m_dsProps->GetDomainPointUArgu() == oldArg)
                m_dsProps->SetDomainPointUArgu(newArg);
            if (m_dsProps->GetDomainPointVArgu() == oldArg)
                m_dsProps->SetDomainPointVArgu(newArg);
            if (m_dsProps->GetDomainPointWArgu() == oldArg)
                m_dsProps->SetDomainPointWArgu(newArg);
        }
		++I;
	}
	
}

FunctionType* PushAnalysis::getNewFuncType(Function* pFunc)
{
	std::vector<Type *> newParamTypes;

	for (auto arg = pFunc->arg_begin(); arg != pFunc->arg_end(); ++arg)
	{
		newParamTypes.push_back((*&(arg))->getType());
	}
	
	// Create new function type with explicit and implicit parameter types
	return FunctionType::get(pFunc->getReturnType(), newParamTypes, pFunc->isVarArg());
}

bool PushAnalysis::runOnModule(llvm::Module& M)
{
	MapList<Function*, Function*> funcsMapping;
	bool retValue = false;

	for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
	{	
		Function* pFunc = &(*I);

		// Only handle functions defined in this module
		if (pFunc->isDeclaration()) continue;

		AnalyzeFunction(pFunc);

		// Find out if function pFunc's type/signature changed
		if (m_isFuncTypeChanged[pFunc])
		{
			retValue = true;

			// Create the new function body and insert it into the module
			FunctionType *pNewFTy = getNewFuncType(pFunc);
			Function* pNewFunc = Function::Create(pNewFTy, pFunc->getLinkage());
			pNewFunc->copyAttributesFrom(pFunc);
			pNewFunc->setSubprogram(pFunc->getSubprogram());
			pFunc->setSubprogram(nullptr);
			M.getFunctionList().insert(pFunc->getIterator(), pNewFunc);
			pNewFunc->takeName(pFunc);

			// Since we have now created the new function, splice the body of the old
			// function right into the new function, leaving the old body of the function empty.
			pNewFunc->getBasicBlockList().splice(pNewFunc->begin(), pFunc->getBasicBlockList());

			// Loop over the argument list, transferring uses of the old arguments over to
			// the new arguments
			updateNewFuncArgs(pFunc, pNewFunc);

			// Map old func to new func
			funcsMapping[pFunc] = pNewFunc;

			// This is a kernel function, so there should not be any call site
			assert(pFunc->use_empty());
		}
	}

	// Update IGC Metadata and shaders map
	// Function declarations are changing, this needs to be reflected in the metadata.
	MetadataBuilder mbuilder(&M);
	for (auto i : funcsMapping)
	{
		auto oldFuncIter = m_pMdUtils->findFunctionsInfoItem(i.first);
		m_pMdUtils->setFunctionsInfoItem(i.second, oldFuncIter->second);
		m_pMdUtils->eraseFunctionsInfoItem(oldFuncIter);

		mbuilder.UpdateShadingRate(i.first, i.second);
	}
	m_pMdUtils->save(M.getContext());

	// Go over all changed functions
	for (MapList<Function*, Function*>::const_iterator I = funcsMapping.begin(), E = funcsMapping.end(); I != E; ++I)
	{
		Function* pFunc = I->first;

		assert(pFunc->use_empty() && "Assume all user function are inlined at this point");

		// Now, after changing funciton signature,
		// and validate there are no calls to the old function we can erase it.
		pFunc->eraseFromParent();
	}

	DumpLLVMIR(m_context, "push_analysis");

	return retValue;

}
char PushAnalysis::ID=0;

}//namespace IGC
