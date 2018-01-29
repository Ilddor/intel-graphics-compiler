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
#include "Compiler/CISACodeGen/PixelShaderCodeGen.hpp"

#include "Compiler/CISACodeGen/messageEncoding.hpp"
#include "Compiler/MetaDataApi/IGCMetaDataDefs.h"
#include "common/allocator.h"
#include <iStdLib/utility.h>
#include "common/LLVMWarningsPush.hpp"
#include "llvm/ADT/StringSwitch.h"
#include "common/LLVMWarningsPop.hpp"
#include "common/secure_mem.h"
#include "Simd32Profitability.hpp"
#include "EmitVISAPass.hpp"

/***********************************************************************************
This file contains the code specific to pixel shader
************************************************************************************/
using namespace llvm;
using namespace IGC::IGCMD;

namespace IGC
{

    CVariable* CPixelShader::GetR1()
    {
        return m_R1;
    }

    void CPixelShader::AllocatePayload()
    {
        if (m_phase == PSPHASE_COARSE)
        {
            CreatePassThroughVar();
        }
        switch (m_phase)
        {
        case PSPHASE_LEGACY:
        case PSPHASE_COARSE:
            AllocatePSPayload();
            break;
        case PSPHASE_PIXEL:
            AllocatePixelPhasePayload();
            break;
        default:
            assert(0 && "unknown phase");
        }
    }

    void CPixelShader::AllocatePixelPhasePayload()
    {
        for (uint i = 0; i < setup.size(); i++)
        {
            if (setup[i])
            {
                uint subRegOffset = 0;
                // PS uniform inputs are stored in the 3rd subreg
                if (setup[i]->GetSize() == SIZE_DWORD)
                {
                    subRegOffset = 3 * SIZE_DWORD;
                }
                assert(m_Signature != nullptr);
                uint offset = GetDispatchSignature().inputOffset[i];
                AllocateInput(setup[i], offset + subRegOffset);
            }
        }
        if (m_ZDelta || m_WDelta)
        {
            unsigned int offset = GetDispatchSignature().ZWDelta;
            if (m_ZDelta)
            {
                AllocateInput(m_ZDelta, offset);
            }
            if (m_WDelta)
            {
                AllocateInput(m_WDelta, offset + SIZE_OWORD);
            }
        }
        if (m_SampleOffsetX || m_SampleOffsetY)
        {
            unsigned int offset = GetDispatchSignature().pixelOffset;
            if (m_SampleOffsetX)
            {
                AllocateInput(m_SampleOffsetX, offset);
            }
            if (m_SampleOffsetY)
            {
                AllocateInput(m_SampleOffsetY, offset + SIZE_OWORD);
            }
        }
        for (auto it = m_CoarseInput.begin(), ie = m_CoarseInput.end(); it != ie; ++it)
        {
            uint offset = GetDispatchSignature().PSOutputOffset.find(it->first)->second;
            AllocateInput(it->second, offset);
        }
        if (m_CoarseMaskInput)
        {
            uint offset = GetDispatchSignature().oMaskOffset;
            AllocateInput(m_CoarseMaskInput, offset);
        }
    }

    void CPixelShader::AllocatePSPayload()
    {
        uint offset = 0;
        //R0 is always allocated as a predefined variable. Increase offset for R0
        assert(m_R0);
        offset += SIZE_GRF;

        assert(m_R1);
        for (uint i = 0; i < m_R1->GetNumberInstance(); i++)
        {
            AllocateInput(m_R1, offset, i);
            offset += SIZE_GRF;
        }

        for (uint i = 0; i < m_numberInstance; i++)
        {
            // allocate size for bary
            if (m_PerspectivePixel)
            {
                AllocateInput(m_PerspectivePixel, offset, i);
                offset += m_PerspectivePixel->GetSize();
            }
            if (m_PerspectiveCentroid)
            {
                AllocateInput(m_PerspectiveCentroid, offset, i);
                offset += m_PerspectiveCentroid->GetSize();
            }
            if (m_PerspectiveSample)
            {
                AllocateInput(m_PerspectiveSample, offset, i);
                offset += m_PerspectiveSample->GetSize();
            }
            if (m_NoPerspectivePixel)
            {
                AllocateInput(m_NoPerspectivePixel, offset, i);
                offset += m_NoPerspectivePixel->GetSize();
            }
            if (m_NoPerspectiveCentroid)
            {
                AllocateInput(m_NoPerspectiveCentroid, offset, i);
                offset += m_NoPerspectiveCentroid->GetSize();
            }
            if (m_NoPerspectiveSample)
            {
                AllocateInput(m_NoPerspectiveSample, offset, i);
                offset += m_NoPerspectiveSample->GetSize();
            }

            // Add support for POSITION_Z
            if (m_pPositionZPixel)
            {
                AllocateInput(m_pPositionZPixel, offset, i);
                offset += m_pPositionZPixel->GetSize();
            }

            // Add support for POSITION_W
            if (m_pPositionWPixel)
            {
                AllocateInput(m_pPositionWPixel, offset, i);
                offset += m_pPositionWPixel->GetSize();
            }

            //Add support for POSITION_XY_OFFSET
            if (m_pPositionXYOffset)
            {
                AllocateInput(m_pPositionXYOffset, offset, i);
                offset += m_pPositionXYOffset->GetSize();
            }

            // Add support for input coverage mask
            if (m_pInputCoverageMask)
            {
                AllocateInput(m_pInputCoverageMask, offset, i);
                offset += m_pInputCoverageMask->GetSize();
            }

            if (m_pCPSRequestedSizeX || m_pCPSRequestedSizeY)
            {
                if (m_pCPSRequestedSizeX)
                {
                    AllocateInput(m_pCPSRequestedSizeX, offset, i);
                }
                if (m_pCPSRequestedSizeY)
                {
                    AllocateInput(m_pCPSRequestedSizeY, offset + SIZE_OWORD, i);
                }
                offset += SIZE_GRF;
            }
            if (m_ZDelta || m_WDelta)
            {
                if (m_ZDelta)
                {
                    AllocateInput(m_ZDelta, offset, i);
                }
                if (m_WDelta)
                {
                    AllocateInput(m_WDelta, offset + SIZE_OWORD, i);
                }
                if (m_Signature)
                {
                    GetDispatchSignature().ZWDelta = offset;
                }
                offset += SIZE_GRF;
            }
            if (m_SampleOffsetX || m_SampleOffsetY)
            {
                if (m_SampleOffsetX)
                {
                    AllocateInput(m_SampleOffsetX, offset, i);
                }
                if (m_SampleOffsetY)
                {
                    AllocateInput(m_SampleOffsetY, offset + SIZE_OWORD, i);
                }
                if (m_Signature)
                {
                    GetDispatchSignature().pixelOffset = offset;
                }
                offset += SIZE_GRF;
            }
        }

        assert(offset%SIZE_GRF == 0);
        // need to return the starting grf for constant to client
        ProgramOutput()->m_startReg = offset / SIZE_GRF;

        // allocate space for NOS constants and pushed constants
        AllocateConstants3DShader(offset);

        assert(offset%SIZE_GRF == 0);
        unsigned int payloadEnd = offset;
        //Allocate size for values coming from VS
        for (uint i = 0; i < setup.size(); i++)
        {
            if (setup[i] && setup[i]->GetAlias() == NULL)
            {
                uint subRegOffset = 0;
                // PS uniform inputs are stored in the 3rd subreg
                if (setup[i]->GetSize() == SIZE_DWORD)
                {
                    subRegOffset = 3 * SIZE_DWORD;
                }
                AllocateInput(setup[i], offset + subRegOffset);
                if (m_Signature)
                {
                    GetDispatchSignature().inputOffset[i] = offset;
                }
                payloadEnd = offset;
            }
            else
            {
                offset += 4 * SIZE_DWORD;
            }
        }
        
        offset = payloadEnd;
        // create output registers for coarse phase
        for (auto it = m_CoarseOutput.begin(), ie = m_CoarseOutput.end(); it != ie; ++it)
        {
            CVariable* output = it->second;
            offset = iSTD::Align(offset, alignmentSize[output->GetAlign()]);
            AllocateOutput(output, offset);
            if (m_Signature)
            {
                GetDispatchSignature().PSOutputOffset[it->first] = offset;
            }
            offset += output->GetSize();
        }
        if (m_CoarseoMask)
        {
            offset = iSTD::Align(offset, alignmentSize[m_CoarseoMask->GetAlign()]);
            AllocateOutput(m_CoarseoMask, offset);
            if (m_Signature)
            {
                GetDispatchSignature().oMaskOffset = offset;
                GetDispatchSignature().CoarseMask = true;
            }
            offset += m_CoarseoMask->GetSize();
        }
    }

    PSSignature::DispatchSignature& CPixelShader::GetDispatchSignature()
    {
        switch (m_dispatchSize)
        {
        case SIMDMode::SIMD8:
            return m_Signature->dispatchSign[0];
        case SIMDMode::SIMD16:
            return m_Signature->dispatchSign[1];
        case SIMDMode::SIMD32:
            return m_Signature->dispatchSign[2];
        default:
            assert(0 && "bad dispatch size");
            break;
        }
        return m_Signature->dispatchSign[0];
    }

    CVariable* CPixelShader::GetBaryReg(e_interpolation mode)
    {
        CVariable* baryReg = 0;
        switch (mode)
        {
        case EINTERPOLATION_LINEAR:
            if (!m_PerspectivePixel){
                m_PerspectivePixel =
                    GetNewVariable(2 * numLanes(m_SIMDSize), ISA_TYPE_F, EALIGN_GRF, false, m_numberInstance);
            }
            baryReg = m_PerspectivePixel;
            break;
        case EINTERPOLATION_LINEARCENTROID:
            if (!m_PerspectiveCentroid){
                m_PerspectiveCentroid =
                    GetNewVariable(2 * numLanes(m_SIMDSize), ISA_TYPE_F, EALIGN_GRF, false, m_numberInstance);
            }
            baryReg = m_PerspectiveCentroid;
            break;
        case EINTERPOLATION_LINEARSAMPLE:
            if (!m_PerspectiveSample){
                m_PerspectiveSample =
                    GetNewVariable(2 * numLanes(m_SIMDSize), ISA_TYPE_F, EALIGN_GRF, false, m_numberInstance);
            }
            baryReg = m_PerspectiveSample;
            break;
        case EINTERPOLATION_LINEARNOPERSPECTIVE:
            if (!m_NoPerspectivePixel){
                m_NoPerspectivePixel =
                    GetNewVariable(2 * numLanes(m_SIMDSize), ISA_TYPE_F, EALIGN_GRF, false, m_numberInstance);
            }
            baryReg = m_NoPerspectivePixel;
            break;
        case EINTERPOLATION_LINEARNOPERSPECTIVECENTROID:
            if (!m_NoPerspectiveCentroid){
                m_NoPerspectiveCentroid =
                    GetNewVariable(2 * numLanes(m_SIMDSize), ISA_TYPE_F, EALIGN_GRF, false, m_numberInstance);
            }
            baryReg = m_NoPerspectiveCentroid;
            break;
        case EINTERPOLATION_LINEARNOPERSPECTIVESAMPLE:
            if (!m_NoPerspectiveSample){
                m_NoPerspectiveSample =
                    GetNewVariable(2 * numLanes(m_SIMDSize), ISA_TYPE_F, EALIGN_GRF, false, m_numberInstance);
            }
            baryReg = m_NoPerspectiveSample;
            break;
        default:
            assert(0);
        }
        return baryReg;
    }

    CVariable* CPixelShader::GetBaryRegLowered(e_interpolation mode)
    {
        CVariable* baryReg = 0;
        switch (mode)
        {
        case EINTERPOLATION_LINEAR:
            if (!m_PerspectivePixelLowered){
                m_PerspectivePixelLowered =
                    GetNewVariable(2 * numLanes(m_SIMDSize), ISA_TYPE_HF, EALIGN_GRF, false, m_numberInstance);
            }
            baryReg = m_PerspectivePixelLowered;
            break;
        case EINTERPOLATION_LINEARCENTROID:
            if (!m_PerspectiveCentroidLowered){
                m_PerspectiveCentroidLowered =
                    GetNewVariable(2 * numLanes(m_SIMDSize), ISA_TYPE_HF, EALIGN_GRF, false, m_numberInstance);
            }
            baryReg = m_PerspectiveCentroidLowered;
            break;
        case EINTERPOLATION_LINEARSAMPLE:
            if (!m_PerspectiveSampleLowered){
                m_PerspectiveSampleLowered =
                    GetNewVariable(2 * numLanes(m_SIMDSize), ISA_TYPE_HF, EALIGN_GRF, false, m_numberInstance);
            }
            baryReg = m_PerspectiveSampleLowered;
            break;
        case EINTERPOLATION_LINEARNOPERSPECTIVE:
            if (!m_NoPerspectivePixelLowered){
                m_NoPerspectivePixelLowered =
                    GetNewVariable(2 * numLanes(m_SIMDSize), ISA_TYPE_HF, EALIGN_GRF, false, m_numberInstance);
            }
            baryReg = m_NoPerspectivePixelLowered;
            break;
        case EINTERPOLATION_LINEARNOPERSPECTIVECENTROID:
            if (!m_NoPerspectiveCentroidLowered){
                m_NoPerspectiveCentroidLowered =
                    GetNewVariable(2 * numLanes(m_SIMDSize), ISA_TYPE_HF, EALIGN_GRF, false, m_numberInstance);
            }
            baryReg = m_NoPerspectiveCentroidLowered;
            break;
        case EINTERPOLATION_LINEARNOPERSPECTIVESAMPLE:
            if (!m_NoPerspectiveSampleLowered){
                m_NoPerspectiveSampleLowered =
                    GetNewVariable(2 * numLanes(m_SIMDSize), ISA_TYPE_HF, EALIGN_GRF, false, m_numberInstance);
            }
            baryReg = m_NoPerspectiveSampleLowered;
            break;
        default:
            assert(0);
        }
        return baryReg;
    }
    
    CVariable* CPixelShader::GetInputDelta(uint index, bool loweredInput)
    {
        CVariable* inputVar = setup[index];
        if(inputVar == nullptr)
        {
                if(loweredInput)
            {
                if(index % 2 == 0)
                {
                    inputVar = GetNewVariable(8, ISA_TYPE_F, EALIGN_GRF, true);
                    setup[index + 1] = GetNewAlias(inputVar, ISA_TYPE_F, 16, 4);
                }
                else
                {
                    inputVar = GetNewAlias(GetInputDelta(index - 1), ISA_TYPE_F, 16, 4);
                }
            }
            else
            {
                inputVar = GetNewVariable(4, ISA_TYPE_F, EALIGN_OWORD, true);
            }
            setup[index] = inputVar;
        }
        return inputVar;
    }

    CVariable* CPixelShader::GetInputDeltaLowered(uint index)
    {
        CVariable* inputVar = setupLowered[index];
        if(inputVar == nullptr)
        {
            assert(LowerPSInput());
            if(index % 2 == 0)
            {
                inputVar = GetNewVariable(8, ISA_TYPE_HF, EALIGN_OWORD, true);
            }
            else
            {
                if(setupLowered[index - 1])
                {
                    inputVar = GetNewAlias(setupLowered[index - 1], ISA_TYPE_HF, 8, 4);
                }
                else
                {
                    inputVar = GetNewVariable(4, ISA_TYPE_HF, EALIGN_OWORD, true);
                }
            }

            setupLowered[index] = inputVar;
        }
        return inputVar;
    }

    CVariable* CPixelShader::GetZDelta()
    {
        if (!m_ZDelta)
        {
            m_ZDelta =
                GetNewVariable(4, ISA_TYPE_F, EALIGN_OWORD, false, m_numberInstance);
        }
        return m_ZDelta;
    }

    CVariable* CPixelShader::GetWDelta()
    {
        if (!m_WDelta)
        {
            m_WDelta =
                GetNewVariable(4, ISA_TYPE_F, EALIGN_OWORD, false, m_numberInstance);
        }
        return m_WDelta;
    }

    CVariable* CPixelShader::GetPositionZ()
    {
        if (!m_pPositionZPixel)
        {
            m_pPositionZPixel =
                GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_F, EALIGN_GRF, false, m_numberInstance);
        }
        return m_pPositionZPixel;
    }

    CVariable* CPixelShader::GetPositionW()
    {
        if (!m_pPositionWPixel)
        {
            m_pPositionWPixel =
                GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_F, EALIGN_GRF, false, m_numberInstance);
        }
        return m_pPositionWPixel;
    }

    CVariable* CPixelShader::GetPositionXYOffset()
    {
        if (!m_pPositionXYOffset)
        {
            m_pPositionXYOffset =
                GetNewVariable(32, ISA_TYPE_B, EALIGN_GRF, false, m_numberInstance);
        }
        return m_pPositionXYOffset;
    }

    CVariable* CPixelShader::GetInputCoverageMask()
    {
        if (!m_pInputCoverageMask)
        {
            m_pInputCoverageMask =
                GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_F, EALIGN_GRF, false, m_numberInstance);
        }
        return m_pInputCoverageMask;
    }

    CVariable* CPixelShader::GetSampleOffsetX()
    {
        if (!m_SampleOffsetX)
        {
            m_SampleOffsetX = GetNewVariable(16, ISA_TYPE_UB, EALIGN_OWORD, true);
        }
        return m_SampleOffsetX;
    }

    CVariable* CPixelShader::GetSampleOffsetY()
    {
        if (!m_SampleOffsetY)
        {
            m_SampleOffsetY = GetNewVariable(16, ISA_TYPE_UB, EALIGN_OWORD, true);
        }
        return m_SampleOffsetY;
    }

    CVariable* CPixelShader::GetCPSRequestedSizeX()
    {
        if (!m_pCPSRequestedSizeX)
        {
            m_pCPSRequestedSizeX =
                GetNewVariable(numLanes(m_SIMDSize) / 4, ISA_TYPE_F, EALIGN_OWORD, false, m_numberInstance);
        }
        return m_pCPSRequestedSizeX;
    }

    CVariable* CPixelShader::GetCPSRequestedSizeY()
    {
        if (!m_pCPSRequestedSizeY)
        {
            m_pCPSRequestedSizeY =
                GetNewVariable(numLanes(m_SIMDSize) / 4, ISA_TYPE_F, EALIGN_OWORD, false, m_numberInstance);
        }
        return m_pCPSRequestedSizeY;
    }

    CPixelShader::CPixelShader(llvm::Function *pFunc, CShaderProgram* pProgram) : CShader(pFunc, pProgram)
    {
        m_RenderTargetMask = 0;
        m_HasoDepth = false;
        m_HasoStencil = false;
        m_HasoMask = false;
        m_HasPositionXY = false;
        m_isPerSample = false;
        m_HasInputCoverageMask = false;
        m_HasPullBary = false;
        m_HasCoarseSize = false;
        m_HasDouble = false;
        m_hasDualBlendSource = false;
        m_HasDiscard = false;
        m_HasDiscardPixelMaskGenerated.firstHalf = false;
        m_HasDiscardPixelMaskGenerated.secondHalf = false;
        m_IsLastPhase = false;
        m_phase = PSPHASE_LEGACY;
        m_Signature = nullptr;
        m_samplerCount = 0;
        memset(modesUsed, 0, sizeof(modesUsed));
        setupLowered.clear();
        loweredSetupIndexes.clear();


        Function* coarsePhase = nullptr;
        Function* pixelPhase = nullptr;
        NamedMDNode* coarseNode = pFunc->getParent()->getNamedMetadata("coarse_phase");
        NamedMDNode* pixelNode = pFunc->getParent()->getNamedMetadata("pixel_phase");
        if (coarseNode)
        {
            coarsePhase = llvm::mdconst::dyn_extract<Function>(coarseNode->getOperand(0)->getOperand(0));
        }
        if (pixelNode)
        {
            pixelPhase = llvm::mdconst::dyn_extract<Function>(pixelNode->getOperand(0)->getOperand(0));
        }
        if (pFunc == coarsePhase)
        {
            m_phase = PSPHASE_COARSE;
        }
        if (coarsePhase && pixelPhase)
        {
            if (pFunc == pixelPhase)
            {
                m_phase = PSPHASE_PIXEL;
                m_IsLastPhase = true;
            }
        }
        else
        {
            m_IsLastPhase = true;
        }
    }

    CPixelShader::~CPixelShader()
    {
    }

    void CPixelShader::InitEncoder(SIMDMode simdMode, bool canAbortOnSpill, ShaderDispatchMode shaderMode)
    {
        m_R1 = NULL;
        m_PerspectivePixel = NULL;
        m_PerspectiveCentroid = NULL;
        m_PerspectiveSample = NULL;
        m_NoPerspectivePixel = NULL;
        m_NoPerspectiveCentroid = NULL;
        m_NoPerspectiveSample = NULL;
        m_PerspectivePixelLowered = NULL;
        m_PerspectiveCentroidLowered = NULL;
        m_PerspectiveSampleLowered = NULL;
        m_NoPerspectivePixelLowered = NULL;
        m_NoPerspectiveCentroidLowered = NULL;
        m_NoPerspectiveSampleLowered = NULL;
        m_KillPixelMask = NULL;
        m_HasDiscard = false;
        m_pPositionZPixel = NULL;
        m_pPositionWPixel = NULL;
        m_pPositionXYOffset = NULL;
        m_pInputCoverageMask = NULL;
        m_pCPSRequestedSizeX = NULL;
        m_pCPSRequestedSizeY = NULL;
        m_PixelPhasePayload = nullptr;
        m_PixelPhaseCounter = nullptr;
        m_SampleOffsetX = nullptr;
        m_SampleOffsetY = nullptr;
        m_ZDelta = nullptr;
        m_WDelta = nullptr;
        m_hasEOT = false;
        m_NeedPSSync = false;
        m_CoarseoMask = nullptr;
        m_CoarseMaskInput = nullptr;

        m_CoarseOutput.clear();
        m_CoarseInput.clear();
        rtWriteList.clear();
        setupLowered.clear();
        m_ShaderMode = shaderMode;
        loweredSetupIndexes.clear();
        memset(modesUsed, 0, sizeof(modesUsed));
        CShader::InitEncoder(simdMode, canAbortOnSpill, shaderMode);
    }

    void CShaderProgram::FillProgram(SPixelShaderKernelProgram* pKernelProgram)
    {
        const unsigned int InstCacheSize = 0xC000;
        CPixelShader* simd8Shader = static_cast<CPixelShader*>(GetShader(SIMDMode::SIMD8));
        CPixelShader* simd16Shader = static_cast<CPixelShader*>(GetShader(SIMDMode::SIMD16));
        CPixelShader* simd32Shader = static_cast<CPixelShader*>(GetShader(SIMDMode::SIMD32));
        CPixelShader* pShader = nullptr;
        if (simd32Shader)
        {
            if ((!simd8Shader && !simd16Shader) ||
                (simd32Shader->m_simdProgram.m_programSize > 0 &&
                simd32Shader->m_simdProgram.m_programSize < InstCacheSize))
            {
                pKernelProgram->simd32 = *simd32Shader->ProgramOutput();
                pShader = simd32Shader;
            }
        }
        if (simd16Shader)
        {
            if (!simd8Shader ||
                (simd16Shader->m_simdProgram.m_programSize > 0))
            {
                pKernelProgram->simd16 = *simd16Shader->ProgramOutput();
                pShader = simd16Shader;
            }
        }
        {
            if(simd8Shader)
            {
                pKernelProgram->simd8 = *simd8Shader->ProgramOutput();
                pShader = simd8Shader;
            }
        }
        pShader->FillProgram(pKernelProgram);
    }

    void CPixelShader::FillProgram(SPixelShaderKernelProgram* pKernelProgram)
    {
        const PixelShaderInfo &psInfo = GetContext()->getModuleMetaData()->psInfo;
        
        pKernelProgram->blendToFillEnabled = psInfo.blendToFillEnabled;
        pKernelProgram->forceEarlyZ = psInfo.forceEarlyZ;

        pKernelProgram->isCoarsePS = m_phase == PSPHASE_COARSE;
        pKernelProgram->hasCoarsePixelSize = m_HasCoarseSize;
        pKernelProgram->hasSampleOffset = m_SampleOffsetX || m_SampleOffsetY;
        pKernelProgram->hasZWDelta = m_ZDelta || m_WDelta;
        pKernelProgram->ConstantBufferLoaded = m_constantBufferLoaded;
        pKernelProgram->hasControlFlow = m_numBlocks > 1 ? true : false;
        pKernelProgram->MaxNumberOfThreads = m_Platform->getMaxPixelShaderThreads();
        pKernelProgram->needPerspectiveBary = m_PerspectivePixel ? true : false;
        pKernelProgram->needPerspectiveCentroidBary = m_PerspectiveCentroid ? true : false;
        pKernelProgram->needPerspectiveSampleBary = m_PerspectiveSample ? true : false;
        pKernelProgram->needNonPerspectiveBary = m_NoPerspectivePixel ? true : false;
        pKernelProgram->needNonPerspectiveCentroidBary = m_NoPerspectiveCentroid ? true : false;
        pKernelProgram->needNonPerspectiveSampleBary = m_NoPerspectiveSample ? true : false;
        pKernelProgram->killPixel = m_HasDiscard;
        pKernelProgram->needSourceDepth = m_pPositionZPixel != nullptr;
        pKernelProgram->needSourceW = m_pPositionWPixel != nullptr;
        pKernelProgram->outputDepth = m_HasoDepth;
        pKernelProgram->oMask = m_HasoMask;
        pKernelProgram->outputStencil = m_HasoStencil;
        pKernelProgram->hasSampleLInLoopPS = GetContext()->m_instrTypes.hasSampleLinLoop;
		pKernelProgram->sampleCmpToDiscardOptimizationPossible = GetContext()->m_instrTypes.sampleCmpToDiscardOptimizationPossible;
		pKernelProgram->sampleCmpToDiscardOptimizationSlot = GetContext()->m_instrTypes.sampleCmpToDiscardOptimizationSlot;
        pKernelProgram->needPSSync = m_NeedPSSync;
		pKernelProgram->hasInputCoverageMask = m_HasInputCoverageMask;
        pKernelProgram->hasPullBary = m_HasPullBary;
        pKernelProgram->isPerSample = IsPerSample();
        if (NamedMDNode* primIdNod = entry->getParent()->getNamedMetadata("PrimIdLocation"))
        {
            pKernelProgram->primIdLocation = int_cast<uint>(
                mdconst::dyn_extract<ConstantInt>(primIdNod->getOperand(0)->getOperand(0))->getZExtValue());
            pKernelProgram->hasPrimID = true;
        }

        pKernelProgram->posXYOffsetEnable = m_pPositionXYOffset ? true : false;
        pKernelProgram->VectorMask = m_VectorMask;
        pKernelProgram->samplerCount = GetSamplerCount(m_samplerCount);
        pKernelProgram->renderTargetMask = m_RenderTargetMask;
        pKernelProgram->constantInterpolationEnableMask = m_ConstantInterpolationMask;
        pKernelProgram->hasEvalSampler = m_HasEvalSampler;
        pKernelProgram->NOSBufferSize = m_NOSBufferSize / SIZE_GRF; // in 256 bits
        pKernelProgram->isMessageTargetDataCacheDataPort = isMessageTargetDataCacheDataPort;

        CreateGatherMap();
        CreateConstantBufferOutput(pKernelProgram);
        FillGTPinRequest(pKernelProgram);

        pKernelProgram->bindingTableEntryCount  = this->GetMaxUsedBindingTableEntryCount();
        pKernelProgram->BindingTableEntryBitmap = this->GetBindingTableEntryBitmap();

        // PS packed attributes
        for (uint i = 0; i < setup.size(); i = i + 4)
        {
            bool useComponent = true;
            if (((i + 3) < setup.size()) && setup[i + 3])
            {
                pKernelProgram->attributeActiveComponent[i / 4] = USC::GFX3DSTATE_SF_ATTRIBUTE_ACTIVE_COMPONENT_XYZW;
            }
            else if (((i + 2) < setup.size()) && setup[i + 2])
            {
                pKernelProgram->attributeActiveComponent[i / 4] = USC::GFX3DSTATE_SF_ATTRIBUTE_ACTIVE_COMPONENT_XYZ;
            }
            else if ((((i + 1) < setup.size()) && setup[i + 1]) || setup[i])
            {
                pKernelProgram->attributeActiveComponent[i / 4] = USC::GFX3DSTATE_SF_ATTRIBUTE_ACTIVE_COMPONENT_XY;
            }
            else
            {
                useComponent = false;
                pKernelProgram->attributeActiveComponent[i / 4] = USC::GFX3DSTATE_SF_ATTRIBUTE_ACTIVE_COMPONENT_DISABLED;
            }
            if(useComponent)
            {
                pKernelProgram->nbOfSFOutput = i / 4 + 1;
            }
        }

        for (unsigned i = 0; i < USC::NUM_PSHADER_OUTPUT_REGISTERS; i++)
        {
            pKernelProgram->OutputUseMask[i] = (unsigned char)psInfo.colorOutputMask[i];
        }
    }

    void CPixelShader::PreCompile()
    {
        CreateImplicitArgs();
        m_R1 = GetNewVariable((SIZE_GRF >> 2), ISA_TYPE_D, EALIGN_GRF, false, m_numberInstance);
        CodeGenContext *ctx = GetContext();

        // make sure the return block is properly set
        if (ctx->getModule()->getNamedMetadata("KillPixel"))
        {
            m_HasDiscard = true;
            m_HasDiscardPixelMaskGenerated.firstHalf = false;
            m_HasDiscardPixelMaskGenerated.secondHalf = false;
        }
        m_VectorMask = m_CG->NeedVMask();
        setup.resize(4 * g_c_Max_PS_attributes, nullptr);
        if(LowerPSInput())
        {
            setupLowered.resize(4 * g_c_Max_PS_attributes, nullptr);
        }
    }

    void CPixelShader::ParseShaderSpecificOpcode(llvm::Instruction* inst)
    {
        // temporary workaround to disable SIMD16 when double are present
        if (inst->getType()->isDoubleTy())
        {
            m_HasDouble = true;
        }
        if (GenIntrinsicInst* genIntr = dyn_cast<GenIntrinsicInst>(inst))
        {
            switch (genIntr->getIntrinsicID())
            {
            case GenISAIntrinsic::GenISA_RenderTargetRead:
                m_NeedPSSync = true;
                if (GetPhase() == PSPHASE_LEGACY)
                {
                    m_isPerSample = true;
                }
                break;
            case GenISAIntrinsic::GenISA_uavSerializeAll:
            case GenISAIntrinsic::GenISA_uavSerializeOnResID:
                m_NeedPSSync = true;
                break;
            case GenISAIntrinsic::GenISA_RTDualBlendSource:
                m_hasDualBlendSource = true;
                AddRenderTarget(
                    cast<RTDualBlendSourceIntrinsic>(genIntr)->getRTIndexImm());
                break;
            case GenISAIntrinsic::GenISA_RTWrite:
            {
                RTWritIntrinsic* rt = cast<RTWritIntrinsic>(genIntr);
                if (rt->getRTIndexImm() != -1)
                {
                    AddRenderTarget(rt->getRTIndexImm());
                }
                if (rt->hasStencil())
                {
                    OutputStencil();
                }
                break;
            }
            case GenISAIntrinsic::GenISA_DCL_ShaderInputVec:
            case GenISAIntrinsic::GenISA_DCL_inputVec:
            {
                uint setupIndex = (uint)llvm::cast<llvm::ConstantInt>(inst->getOperand(0))->getZExtValue();
                m_MaxSetupIndex = std::max(setupIndex, m_MaxSetupIndex);
                if (inst->getType()->isHalfTy())
                {
                    e_interpolation mode = (e_interpolation)llvm::cast<llvm::ConstantInt>(inst->getOperand(1))->getZExtValue();
                    if(mode != EINTERPOLATION_CONSTANT)
                    {
                        loweredSetupIndexes.insert(setupIndex);
                        modesUsed[mode] = true;
                    }
                }
                break;
            }
            case GenISAIntrinsic::GenISA_PullSampleIndexBarys:
            case GenISAIntrinsic::GenISA_PullSnappedBarys:
                m_HasPullBary = true;
                break;
            case GenISAIntrinsic::GenISA_evaluateSampler:
                m_HasEvalSampler = true;
                break;
            default:
                break;
            }
        }
    }

    void CPixelShader::AddRenderTarget(uint RTIndex)
    {
        m_RenderTargetMask |= 1 << RTIndex;
    }

    void CPixelShader::DeclareSGV(uint usage)
    {
        switch (usage)
        {
        case POSITION_X:
        case POSITION_Y:
            m_HasPositionXY = true;
            break;
        case POSITION_Z:
            break;
        case POSITION_W:
            break;
        case VFACE:
            break;
        case INPUT_COVERAGE_MASK:
            m_HasInputCoverageMask = true;
            break;
        case SAMPLEINDEX:
            m_isPerSample = true;
            break;
        case REQUESTED_COARSE_SIZE_X:
        case REQUESTED_COARSE_SIZE_Y:
            m_HasCoarseSize = true;
            break;
        default:
            break;
            // nothing to do
        }
    }

    void CPixelShader::PullPixelPhasePayload()
    {
        CVariable* payload = nullptr;
        bool oMask = false;
        if (GetDispatchSignature().CoarseMask)
        {
            payload = GetCoarseMask();
            oMask = true;
        }
        else
        {
            payload = GetNewVariable(8, ISA_TYPE_D, EALIGN_GRF);
        }
        uint messageDescriptor = PIPullPixelPayload(
            m_SIMDSize == SIMDMode::SIMD8 ? EU_PI_MESSAGE_SIMD8 : EU_PI_MESSAGE_SIMD16,
            m_PixelPhasePayload->GetSize() / SIZE_GRF,
            payload->GetSize() / SIZE_GRF,
            false,
            false,
            false,
            false,
            false,
            oMask);

        CVariable* desc = ImmToVariable(messageDescriptor, ISA_TYPE_UD);
        // save the current phase counter as it is needed by the RT write
        m_CurrentPhaseCounter = GetNewVariable(1, ISA_TYPE_UD, EALIGN_DWORD, true);
        encoder.SetSrcRegion(0, 0, 1, 0);
        encoder.Shl(m_CurrentPhaseCounter, m_PixelPhaseCounter, ImmToVariable(0x10, ISA_TYPE_UW));
        encoder.Push();
        CVariable* nextPhase = GetNewVariable(1, ISA_TYPE_UW, EALIGN_DWORD, true);
        encoder.SetSrcRegion(0, 0, 1, 0);
        encoder.Shl(nextPhase, m_PixelPhaseCounter, ImmToVariable(8, ISA_TYPE_D));
        encoder.Push();
        CVariable* a0 = GetNewVariable(1, ISA_TYPE_UD, EALIGN_DWORD, true);
        encoder.Or(a0, nextPhase, desc);
        encoder.Push();
        encoder.SetNoMask();
        encoder.Send(m_PixelPhasePayload, payload, EU_GEN7_MESSAGE_TARGET_PIXEL_INTERPOLATOR, a0);
        encoder.Push();
        CVariable* mask = BitCast(m_PixelPhasePayload, ISA_TYPE_UW);
        CVariable* f0 = GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_BOOL, EALIGN_DWORD, true);
        encoder.SetSrcSubReg(0, 14);
        encoder.SetSrcRegion(0, 0, 1, 0);
        encoder.Cmp(EPREDICATE_EQ, f0, mask, ImmToVariable(0, ISA_TYPE_UW));
        encoder.Push();
        m_epilogueLabel = encoder.GetNewLabelID();
        encoder.Jump(f0, m_epilogueLabel);
        encoder.Push();
        // override the execution mask in sr0.2
        encoder.SetSrcSubReg(0, 14);
        encoder.SetDstSubReg(2);
        encoder.SetSrcRegion(0, 0, 1, 0);
        encoder.Cast(GetSR0(), mask);
        encoder.Push();
    }

    void CPixelShader::AddPrologue()
    {
        if (m_phase == PSPHASE_PIXEL)
        {
            uint responseLength = 2;
            m_PixelPhasePayload = GetNewVariable(responseLength*(SIZE_GRF >> 2), ISA_TYPE_D, EALIGN_GRF);
            m_PixelPhaseCounter = GetNewAlias(m_PixelPhasePayload, ISA_TYPE_UW, 0, 1);
            m_CoarseParentIndex = GetNewAlias(m_PixelPhasePayload, ISA_TYPE_UW, SIZE_GRF, numLanes(m_SIMDSize));
            m_R1 = GetNewAlias(m_PixelPhasePayload, ISA_TYPE_D, 0, 8);
            encoder.SetNoMask();
            encoder.SetSimdSize(SIMDMode::SIMD1);
            encoder.Copy(m_PixelPhaseCounter, ImmToVariable(0, ISA_TYPE_UW));
            encoder.Push();
            m_pixelPhaseLabel = encoder.GetNewLabelID();
            encoder.Label(m_pixelPhaseLabel);
            encoder.Push();
            PullPixelPhasePayload();
        }
        {
            emitPSInputLowering();
        }
    }

    void CPixelShader::AddEpilogue(llvm::ReturnInst* ret)
    {
        if (!IsLastPhase() && m_KillPixelMask)
        {
            if (!m_CoarseoMask)
            {
                m_CoarseoMask = GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_UD, EALIGN_GRF);
                encoder.Copy(m_CoarseoMask, ImmToVariable(0xFFFFFFFF, ISA_TYPE_UD));
                encoder.Push();
            }
            encoder.SetPredicate(m_KillPixelMask);
            encoder.SetInversePredicate(true);
            encoder.Copy(m_CoarseoMask, ImmToVariable(0x0, ISA_TYPE_UD));
            encoder.Push();
        }
        if (m_phase == PSPHASE_PIXEL)
        {
            encoder.Label(m_epilogueLabel);
            encoder.Push();
            // next phase index is in the first dword of the payload
            CVariable* flag = GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_BOOL, EALIGN_BYTE, true);
            encoder.SetSrcRegion(0, 0, 1, 0);
            encoder.Cmp(EPREDICATE_NE, flag, m_PixelPhaseCounter, ImmToVariable(0, ISA_TYPE_UW));
            encoder.Push();
            encoder.Jump(flag, m_pixelPhaseLabel);
            encoder.Push();
            EOTRenderTarget();
        }
        if (IsLastPhase())
        {
            CShader::AddEpilogue(ret);
        }
    }

    void CPixelShader::AddCoarseOutput(CVariable* output, unsigned int index)
    {
        assert(m_CoarseOutput.find(index) == m_CoarseOutput.end());
        m_CoarseOutput[index] = output;
    }

    CVariable* CPixelShader::GetCoarseInput(unsigned int index)
    {
        auto it = m_CoarseInput.find(index);
        CVariable* coarseInput = nullptr;
        if (it == m_CoarseInput.end())
        {
            coarseInput = GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_F, EALIGN_GRF);
            m_CoarseInput[index] = coarseInput;
        }
        else
        {
            coarseInput = it->second;
        }
        return coarseInput;
    }

    void CPixelShader::SetCoarseoMask(CVariable* oMask)
    {
        m_CoarseoMask = oMask;
    }

    CVariable* CPixelShader::GetCoarseMask()
    {
        if (m_CoarseMaskInput == nullptr)
        {
            m_CoarseMaskInput = GetNewVariable(numLanes(m_SIMDSize), ISA_TYPE_F, EALIGN_GRF);
        }
        return m_CoarseMaskInput;
    }

    CVariable* CPixelShader::GetCoarseParentIndex()
    {
        return m_CoarseParentIndex;
    }

    CVariable* CPixelShader::GetCurrentPhaseCounter()
    {
        return m_CurrentPhaseCounter;
    }

    bool CPixelShader::CompileSIMDSize(SIMDMode simdMode, EmitPass &EP, llvm::Function &F)
    {
        if (m_HasoStencil && simdMode != SIMDMode::SIMD8)
        {
            return false;
        }
        if (m_HasDouble && simdMode != SIMDMode::SIMD8)
        {
            return false;
        }
        if (m_hasDualBlendSource && m_phase == PSPHASE_PIXEL && simdMode != SIMDMode::SIMD8)
        {
            // Spec restriction CPS multi-phase cannot use SIMD16 with dual source blending
            return false;
        }
        if (m_phase != PSPHASE_LEGACY && simdMode == SIMDMode::SIMD32)
        {
            // Coarse pixel shader doesn't support SIMD32
            return false;
        }
       if(simdMode == SIMDMode::SIMD16 && EP.m_ShaderMode == ShaderDispatchMode::NOT_APPLICABLE)
       {
           if(IGC_GET_FLAG_VALUE(ForcePixelShaderSIMDMode) & FLAG_PS_SIMD_MODE_FORCE_SIMD16)
           {
               return true;
           }
           CShader* simd8Program = m_parent->GetShader(SIMDMode::SIMD8);
           if(simd8Program == nullptr || simd8Program->ProgramOutput()->m_scratchSpaceUsedBySpills > 0)
           {
               return false;
           }
       }
       if (simdMode == SIMDMode::SIMD32)
        {
            uint32_t pixelShaderSIMDMode = IGC_GET_FLAG_VALUE(ForcePixelShaderSIMDMode);
            if (pixelShaderSIMDMode & FLAG_PS_SIMD_MODE_FORCE_SIMD32)
            {
                return true;
            }

            CShader* simd16Program = m_parent->GetShader(SIMDMode::SIMD16);
            if ((simd16Program == nullptr || 
                simd16Program->ProgramOutput()->m_programBin == 0 ||
                simd16Program->ProgramOutput()->m_scratchSpaceUsedBySpills > 0))
            {
                return false;
            }

            CodeGenContext* ctx = GetContext();
            const PixelShaderInfo &psInfo = ctx->getModuleMetaData()->psInfo;

            if (psInfo.ForceEnableSimd32) // UMD forced compilation of simd32.
            {
                return true;
            }

            if (!ctx->platform.enablePSsimd32())
            {
                return false;
            }
            
            if(iSTD::BitCount(m_RenderTargetMask) > 1)
            {
                // don't compile SIMD32 for MRT as we may trash the render cache
                return false;
            }

            Simd32ProfitabilityAnalysis &PA = EP.getAnalysis<Simd32ProfitabilityAnalysis>();
            if(PA.isSimd32Profitable())
            {
                return true;
            }

            if (simd16Program && static_cast<CPixelShader*>(simd16Program)->m_sendStallCycle == 0)
            {
                return false;
            }

            if (ctx->platform.psSimd32SkipStallHeuristic() && ctx->m_DriverInfo.AlwaysEnableSimd32())
            {
                return true;
            }

            if(simd16Program)
            {
                uint sendStallCycle = static_cast<CPixelShader*>(simd16Program)->m_sendStallCycle;
                uint staticCycle = static_cast<CPixelShader*>(simd16Program)->m_staticCycle;
                if(sendStallCycle / (float)staticCycle > 0.4)
                {
                    return true;
                }
            }
            return false;
        }
        return true;
    }

    void linkProgram(const SProgramOutput& cps, const SProgramOutput& ps, SProgramOutput& linked)
    {
        linked.m_unpaddedProgramSize =
            cps.m_unpaddedProgramSize + ps.m_unpaddedProgramSize;
        linked.m_scratchSpaceUsedByShader =
            cps.m_scratchSpaceUsedByShader + ps.m_scratchSpaceUsedByShader;
        linked.m_scratchSpaceUsedBySpills =
            cps.m_scratchSpaceUsedBySpills + ps.m_scratchSpaceUsedBySpills;
        linked.m_programSize = iSTD::Align(linked.m_unpaddedProgramSize, 64);
        linked.m_programBin = IGC::aligned_malloc(linked.m_programSize, 16);
        // Copy coarse phase
        memcpy_s(linked.m_programBin,
            cps.m_unpaddedProgramSize,
            cps.m_programBin,
            cps.m_unpaddedProgramSize);
        // Copy pixel phase
        memcpy_s((char*)linked.m_programBin + cps.m_unpaddedProgramSize,
            ps.m_unpaddedProgramSize,
            ps.m_programBin,
            ps.m_unpaddedProgramSize);
        memset((char*)linked.m_programBin + linked.m_unpaddedProgramSize,
            0,
            linked.m_programSize - linked.m_unpaddedProgramSize);
    }

    void linkCPS(SPixelShaderKernelProgram* output, SPixelShaderKernelProgram& linked, unsigned int numberPhases)
    {
        SPixelShaderKernelProgram CoarsePhaseOutput = output[0];
        SPixelShaderKernelProgram PixelPhaseOutput = output[1];
        linked = output[0];

        if (CoarsePhaseOutput.simd16.m_scratchSpaceUsedBySpills == 0 &&
            CoarsePhaseOutput.simd16.m_programBin != nullptr &&
            PixelPhaseOutput.simd16.m_scratchSpaceUsedBySpills == 0 &&
            PixelPhaseOutput.simd16.m_programBin != nullptr)
        {
            linkProgram(CoarsePhaseOutput.simd16, PixelPhaseOutput.simd16, linked.simd16);
        }
        else
        {
            linked.simd16.m_programBin = nullptr;
            linked.simd16.m_programSize = 0;
        }
        linkProgram(CoarsePhaseOutput.simd8, PixelPhaseOutput.simd8, linked.simd8);
        linked.hasPullBary = true;
        linked.renderTargetMask = (CoarsePhaseOutput.renderTargetMask || PixelPhaseOutput.renderTargetMask);
        assert(numberPhases == 2 && "maximum number of phases is 2");
    }

    void CodeGen(PixelShaderContext* ctx)
    {
        Function* coarsePhase = nullptr;
        Function* pixelPhase = nullptr;
        NamedMDNode* coarseNode = ctx->getModule()->getNamedMetadata("coarse_phase");
        NamedMDNode* pixelNode = ctx->getModule()->getNamedMetadata("pixel_phase");
        if (coarseNode)
        {
            coarsePhase = mdconst::dyn_extract<Function>(coarseNode->getOperand(0)->getOperand(0));
        }
        if (pixelNode)
        {
            pixelPhase = mdconst::dyn_extract<Function>(pixelNode->getOperand(0)->getOperand(0));
        }
        CShaderProgram::KernelShaderMap shaders;
        MetaDataUtils *pMdUtils = ctx->getMetaDataUtils();
        if (coarsePhase && pixelPhase)
        {
            //Multi stage PS, need to do separate compiler and link them
            unsigned int numStage = 2;
            PSSignature signature;
            SPixelShaderKernelProgram outputs[2];
            FunctionInfoMetaDataHandle coarseFI, pixelFI;
            coarseFI = pMdUtils->getFunctionsInfoItem(coarsePhase);
            pixelFI = pMdUtils->getFunctionsInfoItem(pixelPhase);

            memset(&outputs, 0, 2 * sizeof(SPixelShaderKernelProgram));
            for (unsigned int i = 0; i < numStage; i++)
            {
                Function *phaseFunc = (i == 0) ? coarsePhase : pixelPhase;
                FunctionInfoMetaDataHandle phaseFI = (i == 0) ? coarseFI : pixelFI;

                shaders.clear();


                pMdUtils->clearFunctionsInfo();
                pMdUtils->setFunctionsInfoItem(phaseFunc, phaseFI);
                pMdUtils->save(phaseFunc->getContext());
                CodeGen(ctx, shaders, &signature);

                // Read the phase function from metadata again as it could be changed in the PushAnalysis pass
                coarseNode = ctx->getModule()->getNamedMetadata("coarse_phase");
                pixelNode = ctx->getModule()->getNamedMetadata("pixel_phase");
                if (coarseNode)
                {
                    coarsePhase = mdconst::dyn_extract<Function>(coarseNode->getOperand(0)->getOperand(0));
                }
                if (pixelNode)
                {
                    pixelPhase = mdconst::dyn_extract<Function>(pixelNode->getOperand(0)->getOperand(0));
                }
                phaseFunc = (i == 0) ? coarsePhase : pixelPhase;
                shaders[phaseFunc]->FillProgram(&outputs[i]);
                COMPILER_SHADER_STATS_PRINT(shaders[phaseFunc]->m_shaderStats, ShaderType::PIXEL_SHADER, ctx->hash, "");
                COMPILER_SHADER_STATS_SUM(ctx->m_sumShaderStats, shaders[phaseFunc]->m_shaderStats, ShaderType::PIXEL_SHADER);
                COMPILER_SHADER_STATS_DEL(shaders[phaseFunc]->m_shaderStats);
                delete shaders[phaseFunc];
            }
            linkCPS(outputs, ctx->programOutput, numStage);
        }
        else
        {
            // Single PS
            CodeGen(ctx, shaders);
            // Assuming single shader information in metadata
            assert(pMdUtils->size_FunctionsInfo() == 1);
            Function *pFunc = pMdUtils->begin_FunctionsInfo()->first;
            // gather data to send back to the driver
            shaders[pFunc]->FillProgram(&ctx->programOutput);
            COMPILER_SHADER_STATS_PRINT(shaders[pFunc]->m_shaderStats, ShaderType::PIXEL_SHADER, ctx->hash, "");
            COMPILER_SHADER_STATS_SUM(ctx->m_sumShaderStats, shaders[pFunc]->m_shaderStats, ShaderType::PIXEL_SHADER);
            COMPILER_SHADER_STATS_DEL(shaders[pFunc]->m_shaderStats);
            delete shaders[pFunc];
        }

    }

    void CPixelShader::CreatePassThroughVar()
    {
        CodeGenContext* ctx = GetContext();
        NamedMDNode* pixelNode = ctx->getModule()->getNamedMetadata("pixel_phase");
        if (!pixelNode)
        {
            // if there is no pixel phase we have nothing to do
            return;
        }
        Function* pixelPhase = mdconst::dyn_extract<Function>(pixelNode->getOperand(0)->getOperand(0));
        for (auto BB = pixelPhase->begin(), BE = pixelPhase->end(); BB != BE; ++BB)
        {
            llvm::BasicBlock *pLLVMBB = &(*BB);
            llvm::BasicBlock::InstListType &instructionList = pLLVMBB->getInstList();
            for (auto I = instructionList.begin(), E = instructionList.end(); I != E; ++I)
            {
                if (GenIntrinsicInst* intr = dyn_cast<GenIntrinsicInst>(I))
                {
                    GenISAIntrinsic::ID IID = intr->getIntrinsicID();
                    if (IID == GenISAIntrinsic::GenISA_DCL_inputVec)
                    {
                        unsigned int setupIndex =
                            (uint)llvm::cast<llvm::ConstantInt>(intr->getOperand(0))->getZExtValue();
                        CVariable* input = GetInputDelta(setupIndex);
                        encoder.MarkAsOutput(input);
                    }
                    else if (IID == GenISAIntrinsic::GenISA_SampleOffsetX)
                    {
                        CVariable* offset = GetSampleOffsetX();
                        encoder.MarkAsOutput(offset);
                    }
                    else if (IID == GenISAIntrinsic::GenISA_SampleOffsetY)
                    {
                        CVariable* offset = GetSampleOffsetY();
                        encoder.MarkAsOutput(offset);
                    }
                    else if (IID == GenISAIntrinsic::GenISA_DCL_SystemValue)
                    {
                        SGVUsage usage = (SGVUsage)llvm::cast<llvm::ConstantInt>(intr->getOperand(0))->getZExtValue();
                        if (usage == POSITION_Z)
                        {
                            CVariable* offset = GetZDelta();
                            encoder.MarkAsOutput(offset);
                        }
                        if (usage == POSITION_W)
                        {
                            CVariable* offset = GetWDelta();
                            encoder.MarkAsOutput(offset);
                        }
                    }
                }
            }
        }
        GetDispatchSignature().inputOffset.resize(setup.size());
    }

    void CPixelShader::ExtractGlobalVariables()
    {
        llvm::Module *module = GetContext()->getModule();
        llvm::GlobalVariable* pGlobal = module->getGlobalVariable("SamplerCount");
        if (pGlobal)
        {
            auto samplerCount = int_cast<unsigned int>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());
            m_samplerCount = samplerCount;
        }
    }

    bool CPixelShader::IsReturnBlock(llvm::BasicBlock* bb)
    {
        return llvm::isa<llvm::ReturnInst>(bb->getTerminator());
    }

    bool CPixelShader::IsLastRTWrite(llvm::GenIntrinsicInst* inst)
    {
        bool isLastRT;
        isLastRT = llvm::isa<llvm::ReturnInst>(inst->getNextNode());

        return isLastRT && IsLastPhase() && GetPhase() != PSPHASE_PIXEL;
    }

    bool CPixelShader::LowerPSInput()
    {
        return (m_SIMDSize == SIMDMode::SIMD16 || !m_Platform->supportMixMode());        
    }

    void CPixelShader::emitPSInputLowering()
    {
        auto iterSetupIndex = loweredSetupIndexes.begin();
        auto iterSetupIndexEnd = loweredSetupIndexes.end();

        if (LowerPSInput())
        {
            for(; iterSetupIndex != iterSetupIndexEnd; ++iterSetupIndex)
            {
                bool combineTwoDelta = false;
                auto nextElemt = iterSetupIndex;
                nextElemt++;
                if(nextElemt != iterSetupIndexEnd && *iterSetupIndex % 2 == 0 && *iterSetupIndex + 1 == *nextElemt)
                {
                    combineTwoDelta = true;
                }
                unsigned int index = *iterSetupIndex;
                CVariable* inputVar = GetInputDelta(index, combineTwoDelta);
                CVariable* inputVarLowered = GetInputDeltaLowered(index);

                encoder.SetSrcRegion(0, 1, 1, 0);
                encoder.SetUniformSIMDSize(combineTwoDelta? SIMDMode::SIMD8 : SIMDMode::SIMD4);
                encoder.SetNoMask();
                encoder.Cast(inputVarLowered, inputVar);
                encoder.Push();
                if(combineTwoDelta)
                {
                    ++iterSetupIndex;
                }
            }

            for (uint i = EINTERPOLATION_LINEAR; i < NUMBER_EINTERPOLATION; ++i)
            {
                if (modesUsed[i])
                {
                    CVariable* baryVar = GetBaryReg((e_interpolation)i);
                    CVariable* baryVarLowered = GetBaryRegLowered((e_interpolation)i);

                    for (uint8_t i = 0; i < m_numberInstance; ++i)
                    {
                        encoder.SetSecondHalf(i == 1);

                        // mov SIMD8 U1/barry(0, 0) in to tmpU(0, 0)
                        // mov (8) r1.0<1>:hf r2.0<8;8,1>:f {Align1, Q1, NoMask} // #??:$27:%30
                        encoder.SetSimdSize(SIMDMode::SIMD8);
                        encoder.SetNoMask();
                        encoder.Cast(baryVarLowered, baryVar);
                        encoder.Push();

                        if (m_SIMDSize == SIMDMode::SIMD16)
                        {
                            // mov SIMD8 U2/barry(2, 0) in to tmpU(0, 8)
                            // mov (8) r1.8<1>:hf r4.0<8;8,1>:f {Align1, Q1, NoMask} // #??:$28:%31
                            encoder.SetSrcSubVar(0, 2);
                            encoder.SetDstSubReg(8);
                            encoder.SetSimdSize(SIMDMode::SIMD8);
                            encoder.SetNoMask();
                            encoder.Cast(baryVarLowered, baryVar);
                            encoder.Push();
                        }

                        // mov SIMD8 V1/barry(1, 0) in to tmpV(0, 0)
                        // mov (8) r12.0<1>:hf r3.0<8;8,1>:f {Align1, Q1, NoMask} // #??:$29:%32
                        encoder.SetSrcSubVar(0, 1);
                        encoder.SetSimdSize(SIMDMode::SIMD8);
                        encoder.SetNoMask();
                        encoder.SetDstSubReg(numLanes(m_SIMDSize));
                        encoder.Cast(baryVarLowered, baryVar);
                        encoder.Push();

                        if (m_SIMDSize == SIMDMode::SIMD16)
                        {
                            // mov SIMD8 V1/barry(3, 0) in to tmpV(0, 0)
                            // mov (8) r12.8<1>:hf r5.0<8;8,1>:f {Align1, Q1, NoMask} // #??:$30:%33
                            encoder.SetSrcSubVar(0, 3);
                            encoder.SetDstSubReg(8);
                            encoder.SetSimdSize(SIMDMode::SIMD8);
                            encoder.SetNoMask();
                            encoder.SetDstSubVar(1);
                            encoder.Cast(baryVarLowered, baryVar);
                            encoder.Push();
                        }
                        encoder.SetSecondHalf(false);
                    }
                }
            }
        }
    }

    void CPixelShader::MarkConstantInterpolation(unsigned int index)
    {
        m_ConstantInterpolationMask |= BIT(index / 4);
    }
} // namespace IGC

