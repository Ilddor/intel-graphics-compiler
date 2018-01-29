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
#include "Compiler/CISACodeGen/DomainShaderCodeGen.hpp"
#include "Compiler/CISACodeGen/EmitVISAPass.hpp"
#include "Compiler/CISACodeGen/messageEncoding.hpp"

#include "common/debug/Debug.hpp"
#include "common/debug/Dump.hpp"
#include "common/igc_regkeys.hpp"
#include "common/secure_mem.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/IRBuilder.h>
#include "common/LLVMWarningsPop.hpp"

#include <iStdLib/utility.h>

using namespace llvm;

/***********************************************************************************
This file contains the code specific to domain shader
************************************************************************************/
namespace IGC
{
//size in dwords of m_pTessFactorURBHeader
const uint32_t CDomainShader::m_pTessFactorsURBHeader = 8;

CDomainShader::CDomainShader(llvm::Function *pFunc, CShaderProgram* pProgram) :
    CShader(pFunc, pProgram)
    , m_pURBWriteHandleReg(nullptr)
    , m_pMaxInputSignatureCount(0)
    , m_pMaxOutputSignatureCount(0)
    , m_pMaxPatchConstantSignatureDeclarations(0)
    , m_pInputControlPointCount(0)
    , m_pURBReadHandleReg(nullptr)
    , m_hasPrimitiveIdInput(false)
{
}

CDomainShader::~CDomainShader()
{

}

void CDomainShader::PreCompile()
{
    CreateImplicitArgs();

    // allocate register for urb write handles
    m_pURBWriteHandleReg = GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_D, EALIGN_GRF);
    m_pURBReadHandleReg = GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_D, EALIGN_GRF);

    if(m_ShaderDispatchMode == ShaderDispatchMode::DUAL_PATCH)
    {
        m_pPatchPrimitiveId = GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_D, EALIGN_GRF);
    }
}

CVariable* CDomainShader::GetPrimitiveID()
{
    return m_pPatchPrimitiveId;
}

ShaderDispatchMode CDomainShader::GetShaderDispatchMode() const
{
    return m_ShaderDispatchMode;
}

CVariable* CDomainShader::GetInputDelta(uint index)
{
    CVariable* inputVar = nullptr;
    if (GetShaderDispatchMode() == ShaderDispatchMode::DUAL_PATCH)
    {
        if (setup.size() <= index / 4 || setup[index / 4] == nullptr)
        {
            inputVar = GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_F, EALIGN_GRF, false);
            AddSetup(index / 4, inputVar);
        }
        else
        {
            inputVar = setup[index / 4];
        }
    }
    else
    {
        if (setup.size() <= index || setup[index] == nullptr)
        {
            inputVar = GetNewVariable(1, ISA_TYPE_F, EALIGN_DWORD, true);
            AddSetup(index, inputVar);
        }
        else
        {
            inputVar = setup[index];
        }
    }
    return inputVar;
}

void CDomainShader::ExtractGlobalVariables()
{
    llvm::Module *module = GetContext()->getModule();

    llvm::GlobalVariable* pGlobal = module->getGlobalVariable("TessInputControlPointCount");
    m_pInputControlPointCount = int_cast<unsigned int>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());

    pGlobal = module->getGlobalVariable("MaxNumOfInputSignatureEntries");
    m_pMaxInputSignatureCount = int_cast<unsigned int>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());

    pGlobal = module->getGlobalVariable("MaxNumOfOutputSignatureEntries");
    m_pMaxOutputSignatureCount = int_cast<unsigned int>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());

    pGlobal = module->getGlobalVariable("MaxNumOfPatchConstantSignatureEntries");
    m_pMaxPatchConstantSignatureDeclarations = int_cast<unsigned int>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());
}

OctEltUnit CDomainShader::GetURBAllocationSize() const
{
    return std::max( (OctEltUnit( ( m_pMaxOutputSignatureCount + 1 ) / 2 )), OctEltUnit(1) );
}

OctEltUnit CDomainShader::GetVertexURBEntryReadLength() const
{
    return  GetShaderDispatchMode() == ShaderDispatchMode::DUAL_PATCH ?
        OctEltUnit(setup.size()) : round_up<OctElement>(EltUnit(setup.size()));
}


OctEltUnit CDomainShader::GetURBHeaderSize() const
{
    return (m_properties.m_hasClipDistance ? OctEltUnit(2) : OctEltUnit(1));
}

CVariable* CDomainShader::GetURBInputHandle(CVariable* pVertexIndex)
{
    if(m_ShaderDispatchMode == ShaderDispatchMode::SINGLE_PATCH)
    {
        // First fetch the URBHandle from R0.0
        encoder.SetSrcRegion(0, 0, 1, 0);
        encoder.SetSrcSubReg(0, 0);
        encoder.Copy(m_pURBReadHandleReg, GetR0());
        encoder.Push();
    }
    else if(m_ShaderDispatchMode == ShaderDispatchMode::DUAL_PATCH)
    {
        encoder.SetSimdSize(SIMDMode::SIMD8);
        encoder.SetSrcRegion(0, 1, 4, 0);
        encoder.SetSrcSubReg(0, 0);
            
        encoder.Copy(m_pURBReadHandleReg, GetR0());
        encoder.Push();
    }
    return m_pURBReadHandleReg;
}

QuadEltUnit CDomainShader::GetFinalGlobalOffet(QuadEltUnit globalOffset) 
{ 
    return globalOffset;
}

uint32_t CDomainShader::GetMaxInputSignatureCount()
{
    return m_pMaxInputSignatureCount;
}

void CDomainShader::AllocatePayload()
{
    uint offset = 0;

    //R0 is always allocated as a predefined variable. Increase offset for R0
    assert(m_R0);
    offset += SIZE_GRF;
        
    if(m_ShaderDispatchMode == ShaderDispatchMode::DUAL_PATCH)
    {
        AllocateInput(m_pPatchPrimitiveId, offset);
        offset += SIZE_GRF;
    }

    AllocateInput(GetSymbol(m_properties.m_UArg), offset );
    offset += SIZE_GRF;

    AllocateInput(GetSymbol(m_properties.m_VArg), offset);
    offset += SIZE_GRF;        

    AllocateInput(GetSymbol(m_properties.m_WArg), offset);
    offset += SIZE_GRF;

    // allocate input for URB return handles
    assert(m_pURBWriteHandleReg);
    AllocateInput(m_pURBWriteHandleReg, offset);
    offset += SIZE_GRF;

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

        offset += (m_ShaderDispatchMode == ShaderDispatchMode::DUAL_PATCH) ? SIZE_GRF : SIZE_DWORD;
    }
    offset = iSTD::Align(offset, SIZE_GRF);
}

CVariable* CDomainShader::GetURBOutputHandle()
{
    return m_pURBWriteHandleReg;
}

void CShaderProgram::FillProgram(SDomainShaderKernelProgram* pKernelProgram)
{
    CDomainShader* pShader = static_cast<CDomainShader*>(GetShader(SIMDMode::SIMD8));
    pShader->FillProgram(pKernelProgram);
    pKernelProgram->simd8 = *pShader->ProgramOutput();

    CDomainShader* dualPatchShader = static_cast<CDomainShader*>(GetShader(SIMDMode::SIMD8, ShaderDispatchMode::DUAL_PATCH));
    if(dualPatchShader)
    {
        pKernelProgram->simd8DualPatch = *dualPatchShader->ProgramOutput();
        pKernelProgram->DispatchMode = DomainShaderDispatchModes::DS_DUAL_PATCH_DISPATCH_MODE;
    }
    else
    {
        pKernelProgram->DispatchMode = DomainShaderDispatchModes::DS_SINGLE_PATCH_DISPATCH_MODE;
    }
}

void CDomainShader::FillProgram(SDomainShaderKernelProgram* pKernelProgram)
{
    ProgramOutput()->m_scratchSpaceUsedByShader = m_ScratchSpaceSize;
    CreateGatherMap();
    CreateConstantBufferOutput(pKernelProgram);
    FillGTPinRequest(pKernelProgram);

    pKernelProgram->NOSBufferSize = m_NOSBufferSize/SIZE_GRF; // in 256 bits
    pKernelProgram->hasControlFlow = m_numBlocks > 1 ? true : false;

    pKernelProgram->MaxNumberOfThreads               = m_Platform->getMaxDomainShaderThreads();
    pKernelProgram->ComputeWAttribute                = !m_properties.m_WArg->use_empty();
    pKernelProgram->URBAllocationSize                = GetURBAllocationSize() + GetURBHeaderSize();
    pKernelProgram->VertexURBEntryOutputLength       = GetURBAllocationSize();
    pKernelProgram->VertexURBEntryReadLength         = GetVertexURBEntryReadLength();
    pKernelProgram->VertexURBEntryReadOffset         = OctEltUnit(0);
    pKernelProgram->VertexURBEntryOutputReadOffset   = GetURBHeaderSize(); 
    pKernelProgram->ConstantBufferLoaded             = m_constantBufferLoaded;
    pKernelProgram->DeclaresRTAIndex                 = m_properties.m_isRTAIndexDeclared;
    pKernelProgram->DeclaresVPAIndex                 = m_properties.m_isVPAIndexDeclared;
    pKernelProgram->HasClipCullAsOutput              = m_properties.m_hasClipDistance;
    pKernelProgram->isMessageTargetDataCacheDataPort = isMessageTargetDataCacheDataPort;
    pKernelProgram->bindingTableEntryCount           = this->GetMaxUsedBindingTableEntryCount();
    pKernelProgram->BindingTableEntryBitmap          = this->GetBindingTableEntryBitmap();
    pKernelProgram->HasPrimitiveIDInput              = this->m_hasPrimitiveIdInput;
}

void CDomainShader::AddEpilogue(llvm::ReturnInst* pRet)
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

void CDomainShader::SetShaderSpecificHelper(EmitPass* emitPass)
{
    m_properties = emitPass->getAnalysisIfAvailable<CollectDomainShaderProperties>()->GetProperties();
}

void CDomainShader::ParseShaderSpecificOpcode(Instruction* inst)
{
    switch (GetOpCode(inst))
    {
    case llvm_sgv:
    {
        SGVUsage usage = static_cast<SGVUsage>(
            dyn_cast<ConstantInt>(inst->getOperand(0))->getZExtValue());
        if (usage == PRIMITIVEID)
        {
            m_hasPrimitiveIdInput = true;
        }
        break;
    }
    default:
        break;
    }
}

} // namespace IGC
