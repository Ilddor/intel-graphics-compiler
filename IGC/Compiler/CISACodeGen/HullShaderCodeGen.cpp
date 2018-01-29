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
#include "Compiler/CISACodeGen/HullShaderCodeGen.hpp"
#include "Compiler/CISACodeGen/EmitVISAPass.hpp"
#include "Compiler/CISACodeGen/messageEncoding.hpp"

#include "common/debug/Debug.hpp"
#include "common/debug/Dump.hpp"
#include "common/secure_mem.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/IRBuilder.h>
#include "common/LLVMWarningsPop.hpp"

#include <iStdLib/utility.h>

/***********************************************************************************
This file contains the code specific to hull shader
************************************************************************************/

#define UseRawSendForURB 0

using namespace llvm;
namespace IGC
{
const uint32_t CHullShader::m_pMaxNumOfPushedInputs = 96;

CHullShader::CHullShader(llvm::Function *pFunc, CShaderProgram* pProgram) :
    CShader(pFunc, pProgram),
    m_R1(nullptr),
    m_R2(nullptr),
    m_pURBWriteHandleReg(nullptr),
    m_pURBReadHandlesReg(nullptr),
    m_HasPrimitiveIDInstruction(false),
    m_pNumURBReadHandleGRF(0),
    m_pBarrierEncountered(false)
{
}

CHullShader::~CHullShader()
{
}

CVariable* CHullShader::GetR1()
{
    return m_R1;
}

CVariable* CHullShader::GetR2()
{
    return m_R2;
}

HullShaderDispatchModes CHullShader::GetShaderDispatchMode()
{
    return m_properties.m_pShaderDispatchMode;
}

void CHullShader::ParseShaderSpecificOpcode(llvm::Instruction* inst)
{
    switch(GetOpCode(inst))
    {
    case llvm_sgv:
        {
            SGVUsage usage = static_cast<SGVUsage>(llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(0))->getZExtValue());
            if(usage == PRIMITIVEID)
            {
                m_HasPrimitiveIDInstruction = true;
            }
            break;
        }
    default:
        break;
    }
}

void CHullShader::PreCompile()
{
    CreateImplicitArgs();

    m_pURBWriteHandleReg = GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_D, EALIGN_GRF);

    if(m_HasPrimitiveIDInstruction)
    {
        m_R2 = GetNewVariable((SIZE_GRF>>2), ISA_TYPE_D, EALIGN_GRF, false, 1);
    }
}

void CHullShader::AddPrologue()
{
    if (m_properties.m_ForcedDispatchMask != 0)
    {
        encoder.SetDstSubReg(2);
        encoder.Cast(GetSR0(), ImmToVariable(m_properties.m_ForcedDispatchMask, ISA_TYPE_UW));
        encoder.Push();
    }

    switch(m_properties.m_pShaderDispatchMode)
    {
    case SINGLE_PATCH_DISPATCH_MODE:
        // First fetch the URBHandle from R0.0
        encoder.SetSrcRegion(0, 0, 1, 0);
        encoder.SetSrcSubReg(0,0);
        encoder.Copy(m_pURBWriteHandleReg, GetR0());
        encoder.Push();

        // calculate the number of URB read Handles GRF
        m_pNumURBReadHandleGRF = (m_properties.m_pInputControlPointCount - 1) / 8 + 1;
        break;
    case EIGHT_PATCH_DISPATCH_MODE:
        // URB handles are in R1
        m_R1 = GetNewVariable((SIZE_GRF>>2), ISA_TYPE_D, EALIGN_GRF, false, 1);
        m_pURBWriteHandleReg = m_R1;

        // calculate the number of URB read Handles GRF
        m_pNumURBReadHandleGRF = m_properties.m_pInputControlPointCount;
        break;
    default:
        assert(0 && "Dispatch mode does not exist");
        break;
    }
}

void CHullShader::AllocatePayload()
{
    switch(m_properties.m_pShaderDispatchMode)
    {
        case SINGLE_PATCH_DISPATCH_MODE:
            AllocateSinglePatchPayload();
            break;
        case EIGHT_PATCH_DISPATCH_MODE:
            AllocateEightPatchPayload();
            break;
        default:
            assert(false && "dispatch mode does not exist");
            break;
    }
}

void CHullShader::AllocateEightPatchPayload()
{
    uint offset = 0;

    //R0 is always allocated as a predefined variable. Increase offset for R0
    assert(m_R0);
    offset += SIZE_GRF;

    assert(m_R1);
    AllocateInput(m_R1,offset);
    offset += SIZE_GRF;

    if(m_HasPrimitiveIDInstruction)
    {
        assert(m_R2);
        AllocateInput(m_R2,offset);
        offset += SIZE_GRF;
    }

    // if m_pURBReadHandlesReg != nullptr, then we need to allocate ( (m_pOutputControlPointCount - 1)/8 + 1 ) registers for input handles
    if(NeedVertexHandle())
    {
        if(m_pURBReadHandlesReg)
        {
            AllocateInput(m_pURBReadHandlesReg, offset);
        }
        // the variable has the size equal to (m_pInputControlPointCount % 8)
        offset += (m_properties.m_pInputControlPointCount)* SIZE_GRF;
    }

    assert(offset % SIZE_GRF == 0);
    ProgramOutput()->m_startReg = offset / SIZE_GRF;
    
    // allocate space for NOS constants and pushed constants
    AllocateConstants3DShader(offset);;

    assert(offset % SIZE_GRF == 0);

    // Allocate space for vertex element data
    for (uint i = 0; i < setup.size(); ++i)
    {
        if (setup[i])
        {
            AllocateInput(setup[i], offset);
        }
        offset += SIZE_GRF;
    }
}

void CHullShader::AllocateSinglePatchPayload()
{
    uint offset = 0;

    //R0 is always allocated as a predefined variable. Increase offset for R0
    assert(m_R0); 
    offset += SIZE_GRF;

    // if m_pURBReadHandlesReg != nullptr, then we need to allocate ( (m_pOutputControlPointCount - 1)/8 + 1 ) registers for input handles
    if(NeedVertexHandle())
    {
        if(m_pURBReadHandlesReg)
        {
            AllocateInput(m_pURBReadHandlesReg, offset);
        }
        // the variable has the size equal to (m_pInputControlPointCount % 8)
        offset += ((m_properties.m_pInputControlPointCount - 1) / 8 + 1) * SIZE_GRF;
    }

    assert(offset % SIZE_GRF == 0);
    ProgramOutput()->m_startReg = offset / SIZE_GRF;

    // allocate space for NOS constants and pushed constants
    AllocateConstants3DShader(offset);

    assert(offset % SIZE_GRF == 0);

    // Allocate space for vertex element data
    for (uint i = 0; i < setup.size(); ++i)
    {
        if (setup[i])
        {
            AllocateInput(setup[i], offset);
        }
        offset += SIZE_DWORD;
    }
}

OctEltUnit CHullShader::GetURBAllocationSize() const
{
    return std::max((OctEltUnit((m_properties.m_pMaxOutputSignatureCount + 1) / 2)), OctEltUnit(1));
}

OctEltUnit CHullShader::GetPatchConstantURBSize() const
{
    return std::max((OctEltUnit((m_properties.m_pMaxPatchConstantSignatureDeclarations + 1) / 2)), OctEltUnit(1));
}

OctEltUnit CHullShader::GetVertexURBEntryReadLength() const
{
    QuadEltUnit numOfPushedAttributesPerVertex = QuadEltUnit(m_properties.GetMaxInputPushed());
    return round_up<OctElement>(numOfPushedAttributesPerVertex);
}

void CShaderProgram::FillProgram(SHullShaderKernelProgram* pKernelProgram)
{
    CHullShader* pShader = static_cast<CHullShader*>(GetShader(SIMDMode::SIMD8));
    pShader->FillProgram(pKernelProgram);
}

void CHullShader::FillProgram(SHullShaderKernelProgram* pKernelProgram)
{
    ProgramOutput()->m_scratchSpaceUsedByShader = m_ScratchSpaceSize;
    pKernelProgram->simd8 = *ProgramOutput();

    CreateGatherMap();
    CreateConstantBufferOutput(pKernelProgram);
    FillGTPinRequest(pKernelProgram);

    pKernelProgram->ConstantBufferLoaded           = m_constantBufferLoaded;
    pKernelProgram->NOSBufferSize = m_NOSBufferSize/SIZE_GRF; // in 256 bits

    pKernelProgram->MaxNumberOfThreads               = m_Platform->getMaxHullShaderThreads();
    pKernelProgram->IncludeVertexHandles             = NeedVertexHandle();
    pKernelProgram->URBAllocationSize                = GetURBAllocationSize();
    pKernelProgram->PatchConstantURBSize             = GetPatchConstantURBSize();
    pKernelProgram->VertexURBEntryReadLength         = GetVertexURBEntryReadLength();
    pKernelProgram->VertexURBEntryReadOffset         = GetURBHeaderSize();
    pKernelProgram->IncludePrimitiveIDEnable         = m_HasPrimitiveIDInstruction ? 1 : 0;
    pKernelProgram->DispatchMode                     = GetShaderDispatchMode();
    pKernelProgram->hasControlFlow                   = m_numBlocks > 1 ? true : false;
    pKernelProgram->InstanceCount                    = m_pBarrierEncountered ? DetermineInstanceCount() : 1;
    pKernelProgram->isMessageTargetDataCacheDataPort = isMessageTargetDataCacheDataPort;
    pKernelProgram->bindingTableEntryCount           = this->GetMaxUsedBindingTableEntryCount();
    pKernelProgram->BindingTableEntryBitmap          = this->GetBindingTableEntryBitmap();
}

/// Allocates a new variable that keeps input vertex URB handles.
/// The size of the variable depends on the number of vertices of the input patch:
/// Each vertex has a separate URB handle and they are placed one after another as part of the payload.
CVariable* CHullShader::GetURBReadHandlesReg()
{
    if (m_pURBReadHandlesReg == nullptr)
    {
        m_pURBReadHandlesReg = GetNewVariable(
            numLanes(m_SIMDSize) * ( m_pNumURBReadHandleGRF ),
            ISA_TYPE_UD, 
            EALIGN_GRF);
    }
    return m_pURBReadHandlesReg;
}

CVariable* CHullShader::GetURBInputHandle(CVariable* pVertexIndex)
{
    if (pVertexIndex->IsImmediate())
    {
        uint64_t vertexIndex = pVertexIndex->GetImmediateValue();
        CVariable* pSelectedHandles = GetNewAlias(GetURBReadHandlesReg(), ISA_TYPE_UD,
            (m_properties.m_pShaderDispatchMode == EIGHT_PATCH_DISPATCH_MODE) ? ((uint16_t)vertexIndex * SIZE_GRF) : ((uint16_t)vertexIndex * SIZE_DWORD),
            (m_properties.m_pShaderDispatchMode == EIGHT_PATCH_DISPATCH_MODE) ? numLanes(m_SIMDSize) : 1);
        return pSelectedHandles;
    }
    else
    {
        // index of the input vertex in the input URBHandle Array
        CVariable* pPerLaneOffsetsReg = GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_UW, EALIGN_GRF, false);
        CVariable* pVertexIndexWord = BitCast(pVertexIndex, ISA_TYPE_UW);
        // perLaneOffsets = 4 * pVertexIndex
        if (!pVertexIndex->IsUniform())
        {
            encoder.SetSrcRegion(0, 16, 8, 2);
        }

        CVariable* pConst = nullptr;
        if(m_properties.m_pShaderDispatchMode == SINGLE_PATCH_DISPATCH_MODE)
        {
            // Single Patch
            pConst = ImmToVariable(0x02, ISA_TYPE_UW);
        }
        else
        {
            // Eight Patch
            pConst = ImmToVariable(0x05, ISA_TYPE_UW);
        }

        encoder.Shl(pPerLaneOffsetsReg, pVertexIndexWord, pConst);
        encoder.Push();

        // selectedHandles = addressof(urbhandles) + pPerLaneOffsetsReg
        CVariable* pSelectedHandles = GetNewAddressVariable(numLanes(m_SIMDSize), ISA_TYPE_UD, false, false);

        if(m_properties.m_pShaderDispatchMode == EIGHT_PATCH_DISPATCH_MODE)
        {
            CVariable* pPerLaneOffsetsRaw = GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_UW, EALIGN_GRF);
            GetSimdOffsetBase(pPerLaneOffsetsRaw);
            encoder.Mad(pPerLaneOffsetsReg, pPerLaneOffsetsRaw, ImmToVariable(0x04, ISA_TYPE_UW), pPerLaneOffsetsReg);
            encoder.Push();
        }

        encoder.AddrAdd(pSelectedHandles, GetURBReadHandlesReg(), pPerLaneOffsetsReg);
        encoder.Push();
        return pSelectedHandles;
    }
}

QuadEltUnit CHullShader::GetFinalGlobalOffet(QuadEltUnit globalOffset) 
{ 
    return globalOffset + GetURBHeaderSize();
}

uint32_t CHullShader::GetMaxNumOfPushedInputs() const
{ 
    uint numberOfPatches = (m_properties.m_pShaderDispatchMode == EIGHT_PATCH_DISPATCH_MODE) ? 8 : 1;

    // Determine how many of input attributes per InputControlPoint (Vertex) can be POTENTIALLY pushed 
    // in current dispatch mode for current topology ( InputPatch size ).
    uint32_t maxNumOfPushedInputAttributesPerICP = 
        m_pMaxNumOfPushedInputs / (m_properties.m_pInputControlPointCount*numberOfPatches);

    // Input attributes can be pushed only in pairs, so we need to round down the limit.
    maxNumOfPushedInputAttributesPerICP = iSTD::Align(maxNumOfPushedInputAttributesPerICP - 1, 2);

    // Determine required number of input attributes.
    // They can be pushed only in pairs.
    uint32_t reqNumOfInputAttributesPerICP = iSTD::Align(m_properties.m_pMaxInputSignatureCount, 2);

    // TODO: reqNumOfInputAttributesPerICP will have to be incremented by size of Vertex Header 
    // in case of SGV inputs have to be taken into consideration (will be done in next step).
    // reqNumOfInputAttributes += HeaderSize().Count();

    // Determine ACTUAL number of attributes that can be pushed.
    // If the required number of input attributes is less that maximum potential number,
    // than all of the will be pushed.
    uint32_t actualNumOfPushedInputAttributesPerICP = 
        iSTD::Min(reqNumOfInputAttributesPerICP, maxNumOfPushedInputAttributesPerICP);

    return actualNumOfPushedInputAttributesPerICP;
}

void CHullShader::EmitPatchConstantInput(llvm::Instruction* pInst, CVariable* pDest)
{
    bool readHeader = ((dyn_cast<GenIntrinsicInst>(pInst))->getIntrinsicID() == GenISAIntrinsic::GenISA_HSURBPatchHeaderRead);

    // patch constant input read
    llvm::Value* pIndirectVertexIdx = pInst->getOperand(0);

    CVariable* pPerSlotOffsetVar = nullptr;
    QuadEltUnit attributeOffset(0);

    // {BDW - WA, HS} Do not set pPerSlotOffset or change globalOffset to read TessFactors from URB.
    if (!readHeader)
    {
        if (llvm::ConstantInt* pConstAttribIdx = llvm::dyn_cast<llvm::ConstantInt>(pIndirectVertexIdx))
        {
            // attribute index is a constant, we can compute the URB read offset directly
            attributeOffset = QuadEltUnit(int_cast<unsigned int>(pConstAttribIdx->getZExtValue()));
        }
        else
        {
            // attribute is a runtime value, we need to pass it as per-slot offset to URB read
            if (QuadEltUnit(m_properties.m_pMaxOutputSignatureCount).Count() != 1)
            {
                CVariable* pVertexSize = ImmToVariable(QuadEltUnit(m_properties.m_pMaxPatchConstantSignatureDeclarations).Count(), ISA_TYPE_UD);
                pPerSlotOffsetVar = GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_UD, EALIGN_GRF);
                GetEncoder().Mul(pPerSlotOffsetVar, GetSymbol(pIndirectVertexIdx), pVertexSize);
            }
            else
            {
                pPerSlotOffsetVar = GetSymbol(pIndirectVertexIdx);
            }
        }

        attributeOffset = attributeOffset + GetURBHeaderSize();
    }

    URBReadPatchConstOrOutputCntrlPtInput(pPerSlotOffsetVar, attributeOffset, false, pDest);
}

void CHullShader::EmitOutputControlPointInput(llvm::Instruction* pInst, CVariable* pDest)
{
    // patch constant input read
    llvm::Value* pIndirectVertexIdx = pInst->getOperand(0);
    llvm::Value* pAttribIdx = pInst->getOperand(1);

    CVariable* pPerSlotOffsetVar = nullptr;
    QuadEltUnit attributeOffset(GetPatchConstantOutputSize());

    // Compute offset from vertex index
    if (llvm::ConstantInt* pConstVertexIdx = llvm::dyn_cast<llvm::ConstantInt>(pIndirectVertexIdx))
    {
        // attribute index is a constant, we can compute the URB read offset directly
        attributeOffset = 
            attributeOffset + 
            QuadEltUnit(int_cast<unsigned int>(pConstVertexIdx->getZExtValue())) * m_properties.m_pMaxOutputSignatureCount;
    }
    else
    {
        // attribute is a runtime value, we need to pass it as per-slot offset to URB read
        if (QuadEltUnit(m_properties.m_pMaxOutputSignatureCount).Count() != 1)
        {
            CVariable* pVertexSize = ImmToVariable(QuadEltUnit(m_properties.m_pMaxOutputSignatureCount).Count(), ISA_TYPE_UD);
            pPerSlotOffsetVar =
                GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_UD, EALIGN_GRF);
            GetEncoder().Mul(pPerSlotOffsetVar, GetSymbol(pIndirectVertexIdx), pVertexSize);
        }
        else
        {
            pPerSlotOffsetVar = GetSymbol(pIndirectVertexIdx);
        }
    }

    // Compute additionall offset coming from atribute index
    if (llvm::ConstantInt* pConstAttribIdx = llvm::dyn_cast<llvm::ConstantInt>(pAttribIdx))
    {
        // attribute offset is a constant, we can compute the URB read offset directly
        attributeOffset =
            attributeOffset + QuadEltUnit(int_cast<unsigned int>(pConstAttribIdx->getZExtValue()));
    }
    else
    {
        // attribute offset is a runtime value, we need to pass it as per-slot offset to URB read
        if (pPerSlotOffsetVar != nullptr)
        {
            // variable is already present from vertex index, create ADD operation
            CVariable* pTempPerSlotOffsetVar =
                GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_UD, EALIGN_GRF);
            GetEncoder().Add(pTempPerSlotOffsetVar, pPerSlotOffsetVar, GetSymbol(pAttribIdx));
            pPerSlotOffsetVar = pTempPerSlotOffsetVar;
        }
        else
        {
            pPerSlotOffsetVar = GetSymbol(pAttribIdx);
        }
    }

    URBReadPatchConstOrOutputCntrlPtInput(pPerSlotOffsetVar, attributeOffset, false, pDest);
}

void CHullShader::URBReadPatchConstOrOutputCntrlPtInput(
    CVariable* pPerSlotOffsetVar,
    QuadEltUnit globalOffset,
    bool EOT,
    CVariable* pDest )
{
    CEncoder& encoder = GetEncoder();

    const bool hasPerSlotOffsets = pPerSlotOffsetVar != nullptr;
    // Payload size is just URB handles (1 GRF) or URB handles and per-slot offsets (2 GRFs).
    const Unit<Element> payloadSize(hasPerSlotOffsets ? 2 : 1);
    CVariable* pPayload = 
        GetNewVariable(payloadSize.Count() * numLanes(m_SIMDSize), ISA_TYPE_UD, EALIGN_GRF);
        
    // get the register with URBHandles
    CopyVariable(pPayload, m_pURBWriteHandleReg);

    // If we have runtime value in per-slot offsets, we need to copy per-slot offsets to payload
    if (hasPerSlotOffsets)
    {
        CopyVariable(pPayload, pPerSlotOffsetVar, 1);
    }

    const Unit<Element> messageLength = payloadSize;
    const Unit<Element> responseLength(pDest->GetNumberElement()/numLanes(m_SIMDSize));
    const uint desc = UrbMessage(
        messageLength.Count(),
        responseLength.Count(),
        EOT,
        hasPerSlotOffsets,
        false,
        globalOffset.Count(),
        EU_GEN8_URB_OPCODE_SIMD8_READ);

    const uint exDesc = EU_MESSAGE_TARGET_URB | (EOT ? 1 << 5 : 0);
    CVariable* pMessDesc = ImmToVariable(desc, ISA_TYPE_UD);

    encoder.Send(pDest, pPayload, exDesc, pMessDesc);
    encoder.Push();
}

/// Returns the size of the output vertex.
/// Unit: 16B = 4 DWORDs 
/// Note: The PatchConstantOutput size must be 32B-aligned when rendering is enabled
/// Therefore, the PatchConstantOutput size is also rounded up to a multiple of 2.
QuadEltUnit CHullShader::GetPatchConstantOutputSize() const
{
    const uint numPatchConstantsPadded = iSTD::Align(m_properties.m_pMaxPatchConstantSignatureDeclarations, 2);
    return QuadEltUnit(numPatchConstantsPadded + 2); // patch constant header is 2 QuadEltUnits
}

CVariable* CHullShader::GetURBOutputHandle()
{
    // return URBHandle
    return m_pURBWriteHandleReg;
}

OctEltUnit CHullShader::GetURBHeaderSize()
{
    if (m_properties.m_HasClipCullAsInput)
        return OctEltUnit(2);
    else
        return OctEltUnit(1);

}

void CHullShader::EmitPatchConstantHeader(
    CVariable* var[],
    bool EOT )
{
    CEncoder& encoder = GetEncoder();

    // We write the offset along with the payload.
    uint messageLength = 9; // 8 DWORDS of output + 1 GRF for URBHandles

    // Allocate payload with size = messageLength
    CVariable* pPayload = GetNewVariable(messageLength * numLanes(m_SIMDSize),
        ISA_TYPE_D, IGC::EALIGN_GRF);

    // Get the register with URBHandles
    CopyVariable(pPayload, m_pURBWriteHandleReg);

    for(uint j = 2; j < 8; ++j)
    {
        CopyVariable(pPayload, var[j], 1+j );
    }

    // Send message
    uint exDesc = EU_MESSAGE_TARGET_URB | (EOT ? 1 << 5 : 0);

    uint desc = UrbMessage(
        messageLength,
        0,
        EOT,
        false,
        false,
        0,
        EU_GEN8_URB_OPCODE_SIMD8_WRITE);

    CVariable* pMessDesc = ImmToVariable(desc, ISA_TYPE_D);

    encoder.Send(nullptr, pPayload, exDesc, pMessDesc);
    encoder.Push();
}

void CHullShader::AddEpilogue(llvm::ReturnInst* pRet)
{
    bool addDummyURB = true;
    if (pRet != &(*pRet->getParent()->begin()))
    {
        auto intinst = dyn_cast<GenIntrinsicInst>(pRet->getPrevNode());

        // if a URBWrite intrinsic is present no need to insert dummy urb write
        if (intinst && intinst->getIntrinsicID() == GenISAIntrinsic::GenISA_URBWrite)
        {
            addDummyURB = false;
        }
    }

    if (addDummyURB)
    {
        EmitEOTURBWrite();
    }

    CShader::AddEpilogue(pRet);
}

void CHullShader::SetShaderSpecificHelper(EmitPass* emitPass)
{
    m_properties = emitPass->getAnalysisIfAvailable<CollectHullShaderProperties>()->GetProperties();
}

void CHullShader::SetBarrierEncountered()
{
    m_pBarrierEncountered = true;
}

uint32_t CHullShader::DetermineInstanceCount()
{
    if (GetShaderDispatchMode()== EIGHT_PATCH_DISPATCH_MODE)
    {
        return m_properties.m_pOutputControlPointCount;
    }
    else if (GetShaderDispatchMode() == SINGLE_PATCH_DISPATCH_MODE)
    {
        return ((m_properties.m_pOutputControlPointCount - 1) / 8 + 1);
    }
    else
    {
        return 1; // For DualPatch mode instance count is always 1
    }
}

bool CHullShader::NeedVertexHandle()
{
    // HW restriction: if no inputs are pushed URB handles need to e requested
    if(m_pURBReadHandlesReg != nullptr || (GetVertexURBEntryReadLength().Count() == 0))
    {
        return true;
    }
    return false;
}

}
