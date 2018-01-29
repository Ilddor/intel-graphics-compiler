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
#include "HullShaderLowering.hpp"
#include "Compiler/CISACodeGen/helper.h"
#include "Compiler/CISACodeGen/HullShaderCodeGen.hpp"
#include "Compiler/MetaDataUtilsWrapper.h"
#include "Compiler/IGCPassSupport.h"

namespace IGC
{
using namespace llvm;
using namespace IGCMD;

class HullShaderLowering : public llvm::FunctionPass
{
public:
    HullShaderLowering();
    static char         ID;
    virtual bool runOnFunction(llvm::Function &F) override;

    virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const override
    {
        AU.setPreservesCFG();
        AU.addRequired<MetaDataUtilsWrapper>();
        AU.addRequired<CollectHullShaderProperties>();
    }

private:
    void LowerIntrinsicInputOutput(llvm::Function &F);

    llvm::GenIntrinsicInst* AddURBWriteControlPointOutputs(
        Value* mask,
        Value* data[8],
        Instruction* prev);

    llvm::GenIntrinsicInst* AddURBWrite(
        llvm::Value* offset,
        llvm::Value* mask,
        llvm::Value* data[8],
        llvm::Instruction* prev);


    void AddURBRead(Value* index, Value* offset, Instruction* prev);
    llvm::Module* m_module;

    std::map<Value*, std::vector<GenIntrinsicInst *>>  m_pControlPointOutputs;
    QuadEltUnit m_headerSize;
    CollectHullShaderProperties* m_hullShaderInfo;

};

#define PASS_FLAG "igc-collect-hull-shader-properties"
#define PASS_DESCRIPTION "Collect information related to hull shader"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS true
IGC_INITIALIZE_PASS_BEGIN(CollectHullShaderProperties, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_END(CollectHullShaderProperties, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

// undef macros to avoid redefinition compiler warnings
#undef PASS_FLAG
#undef PASS_DESCRIPTION
#undef PASS_ANALYSIS

#define PASS_FLAG "igc-hull-shader-lowering"
#define PASS_DESCRIPTION "Lower inputs outputs for hull shader"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(HullShaderLowering, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(CollectHullShaderProperties)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
IGC_INITIALIZE_PASS_END(HullShaderLowering, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char HullShaderLowering::ID = 0;
char CollectHullShaderProperties::ID = 0;

HullShaderLowering::HullShaderLowering() : FunctionPass(ID)
{
    initializeHullShaderLoweringPass(*PassRegistry::getPassRegistry());
}

bool HullShaderLowering::runOnFunction(llvm::Function &F)
{
    m_headerSize = QuadEltUnit(2);
    m_hullShaderInfo = &getAnalysis<CollectHullShaderProperties>();
    // Collect Hull shader information
    m_hullShaderInfo->gatherInformation(&F);

    m_module = F.getParent();
    
    LowerIntrinsicInputOutput(F);
    return false;
}

void HullShaderLowering::LowerIntrinsicInputOutput(Function& F)
{
    SmallVector<Instruction*, 10> instructionToRemove;

    IRBuilder<> builder(F.getContext());

    for(auto BI = F.begin(), BE = F.end(); BI != BE; BI++)
    {
        m_pControlPointOutputs.clear();
        for(auto II = BI->begin(), IE = BI->end(); II!=IE; II++)
        {
            if(GenIntrinsicInst* inst = dyn_cast<GenIntrinsicInst>(II))
            {
                GenISAIntrinsic::ID IID = inst->getIntrinsicID();
                if(IID == GenISAIntrinsic::GenISA_DCL_HSinputVec)
                {
                    Value* index = nullptr;
                    if (llvm::isa<ConstantInt>(inst->getOperand(0)))
                    {
                        // In case of direct access of HSInputVec we need to be sure to not use vertex index 
                        // bigger than number of declared ICP.
                        // This might happen in OGL, when number of Input Control Points might not be known
                        // during first compilation.
                        uint32_t usedIndex = int_cast<unsigned int>(llvm::cast<ConstantInt>(inst->getOperand(0))->getZExtValue());
                        uint32_t validIndex = 
                            iSTD::Min(usedIndex, m_hullShaderInfo->GetProperties().m_pInputControlPointCount - 1);

                        index = builder.getInt32(validIndex);
                    }
                    else
                    {
                        index = inst->getOperand(0);
                    }

                    AddURBRead(index, inst->getOperand(1), inst);
                    instructionToRemove.push_back(inst);
                }

                if(IID == GenISAIntrinsic::GenISA_PatchConstantOutput)
                {
                    // handle GenISA_OUTPUT intrinsic instructions
                    const uint patchConstantOutputIndex = 4;
                    Value* offsetVal = nullptr;
                    llvm::Value* pPatchConstantOffset = inst->getOperand(patchConstantOutputIndex);

                    // lower patch constant outputs to URBWrite
                    if (auto pPCOffsetIdx = llvm::dyn_cast<llvm::ConstantInt>(pPatchConstantOffset))
                    {
                        // patch constant output index is a constant.
                        const uint offsetIndex = int_cast<unsigned int>(pPCOffsetIdx->getZExtValue());
                        const QuadEltUnit staticOffset = QuadEltUnit(offsetIndex) + OctEltUnit(1); // Add 1 for vertex header
                        Value* staticOffsetVal = builder.getInt32(staticOffset.Count());
                        offsetVal = staticOffsetVal;
                    }
                    else
                    {
                        // patch constant output is indirect output
                        const QuadEltUnit staticOffset = OctEltUnit(1); // Add 1 for vertex header
                        Value* staticOffsetVal = builder.getInt32(staticOffset.Count());

                        Instruction* sum = BinaryOperator::CreateAdd(pPatchConstantOffset, staticOffsetVal);
                        sum->insertBefore(inst);
                        offsetVal = sum;
                    }

                    Value* undef = llvm::UndefValue::get(Type::getFloatTy(F.getContext()));
                    Value* data[8] = 
                    {
                        inst->getOperand(0),
                        inst->getOperand(1),
                        inst->getOperand(2),
                        inst->getOperand(3),
                        undef,
                        undef,
                        undef,
                        undef,
                    };
                    AddURBWrite(
                        offsetVal,
                        inst->getOperand(5),
                        data,
                        inst);
                    instructionToRemove.push_back(inst);
                }

                if(IID == GenISAIntrinsic::GenISA_OutputTessControlPoint)
                {
                    // for each BB handle OutputHSControlPoint intrinsic instructions
                    Value* undef = llvm::UndefValue::get(Type::getFloatTy(F.getContext()));
                    Value* data[8] = 
                    {
                        inst->getOperand(0),
                        inst->getOperand(1),
                        inst->getOperand(2),
                        inst->getOperand(3),
                        undef,
                        undef,
                        undef,
                        undef,
                    };
                    AddURBWriteControlPointOutputs(inst->getOperand(6), data, inst);
                    instructionToRemove.push_back(inst);
                }

                if ((IID == GenISAIntrinsic::GenISA_OuterScalarTessFactors) ||
                    (IID == GenISAIntrinsic::GenISA_InnerScalarTessFactors))
                {
                    // The URB Location for tessellation factors spans the first two offsets 
                    // offset 0 and 1. The tessellation factors occupy the two offsets as mentioned below
                    // Quad domain has 4 outer and 2 inner tessellation factors
                    // Triangle domain has 3 outer and 1 inner tessellation factor
                    // Isolines have 2 outer tessellation factors
                    //
                    //----------------------------------------------------------------------------------
                    //| URB Offset 1.3    | URB Offset 1.2     | URB Offset 1.1    | URB Offset 1.0     | 
                    //----------------------------------------------------------------------------------
                    //| OUTER_QUAD_U_EQ_0 | OUTER_QUAD_V_EQ_0  | OUTER_QUAD_U_EQ_1 | OUTER_QUAD_V_EQ_1  |
                    //----------------------------------------------------------------------------------
                    //| OUTER_TRI_U_EQ_0  | OUTER_TRI_V_EQ_0   | OUTER_TRI_W_EQ_0  | INNER_TRI_INSIDE   |
                    //----------------------------------------------------------------------------------
                    //| OUTER_LINE_DETAIL | OUTER_LINE_DENSITY | 	               |                    |
                    //----------------------------------------------------------------------------------
                    //------------------------------------------------------------------------------------
                    //| URB Offset 0.3      | URB Offset 0.2            | URB Offset 0.1 | URB Offset 0.0 |
                    //------------------------------------------------------------------------------------
                    //| INNER_QUAD_U_INSIDE | INNER_OUTER_QUAD_V_INSIDE |                |                |
                    //------------------------------------------------------------------------------------
                    //|  			        |    			            |                |                |
                    //------------------------------------------------------------------------------------
                    //| 			        |  					        | 	             |                |
                    //------------------------------------------------------------------------------------

                    uint32_t tessShaderDomain = USC::TESSELLATOR_DOMAIN_ISOLINE;
                    llvm::NamedMDNode *pMetaData = m_module->getOrInsertNamedMetadata("TessellationShaderDomain");
                    if (pMetaData && (pMetaData->getNumOperands() == 1))
                    {
                        llvm::MDNode* pTessShaderDomain = pMetaData->getOperand(0);
                        if (pTessShaderDomain)
                        {
                            tessShaderDomain = int_cast<uint32_t>(
                                mdconst::dyn_extract<ConstantInt>(pTessShaderDomain->getOperand(0))->getZExtValue());
                        }
                    }

                    // offset into URB is 1 for outerScalarTessFactors and 
                    // 1 if its triangle domain and inner scalar tessellation factor
                    // 0 if its the quad domain inner tessellation factor
                    int offset = (IID == GenISAIntrinsic::GenISA_OuterScalarTessFactors) ? 1 :
                        (tessShaderDomain == USC::TESSELLATOR_DOMAIN_TRI) ? 1 : 0;
                    Value* pOffsetVal = builder.getInt32(offset);

                    Value* data[8] =
                    {
                        inst->getArgOperand(1),
                        inst->getArgOperand(1),
                        inst->getArgOperand(1),
                        inst->getArgOperand(1),
                        inst->getArgOperand(1),
                        inst->getArgOperand(1),
                        inst->getArgOperand(1),
                        inst->getArgOperand(1)
                    };

                    if (llvm::isa<ConstantInt>(inst->getOperand(0)))
                    {
                        unsigned int tessFactor = int_cast<unsigned int>(llvm::cast<ConstantInt>(inst->getOperand(0))->getZExtValue());

                        tessFactor = ((IID == GenISAIntrinsic::GenISA_InnerScalarTessFactors)
                            && (tessShaderDomain == USC::TESSELLATOR_DOMAIN_TRI)) ? 3 : tessFactor;

                        AddURBWrite(pOffsetVal,
                            builder.getInt32(1 << (3 - tessFactor)),
                            data,
                            inst);
                    }
                    else
                    {
                        builder.SetInsertPoint(inst);
                        Value* pSubRes = nullptr;
                        if ((IID == GenISAIntrinsic::GenISA_InnerScalarTessFactors)
                            && (tessShaderDomain == USC::TESSELLATOR_DOMAIN_TRI))
                        {
                            pSubRes = inst->getOperand(0);
                        }
                        else
                        {
                            pSubRes = builder.CreateSub(
                                builder.getInt32(3),
                                inst->getOperand(0));
                        }

                        Value* pShiftVal = builder.CreateShl(
                            builder.getInt32(1),
                            pSubRes);

                        AddURBWrite(pOffsetVal, pShiftVal, data, inst);
                    }
                    instructionToRemove.push_back(inst);
                }

                // Tessellation factors can be written individually so we have scalar output
                // We will try to merge them into a a single URB write if possible
                if(IID == GenISAIntrinsic::GenISA_ScalarTessFactors)
                {
                    // extract tessellation factors and store in m_ScalarTessFactors
                    const unsigned int tessFactor = int_cast<unsigned int>(llvm::cast<ConstantInt>(inst->getOperand(0))->getZExtValue());
                    Value* undef = llvm::UndefValue::get(Type::getFloatTy(F.getContext()));
                    Value* offsetVal = builder.getInt32(0);
                    Value* offsetVal1 = builder.getInt32(1);

                    switch(tessFactor)
                    {
                    case SHADER_OUTPUT_TYPE_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR:
                    case SHADER_OUTPUT_TYPE_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR:
                    case SHADER_OUTPUT_TYPE_FINAL_LINE_DETAIL_TESSFACTOR:
                    {
                        Value* data[8] =
                        {
                            undef,
                            undef,
                            undef,
                            inst->getArgOperand(1),
                            undef,
                            undef,
                            undef,
                            undef
                        };
                        AddURBWrite(
                            offsetVal1,
                            builder.getInt32(0x8),
                            data,
                            inst);
                        break;
                    }
                    case SHADER_OUTPUT_TYPE_FINAL_QUAD_V_EQ_0_EDGE_TESSFACTOR:
                    case SHADER_OUTPUT_TYPE_FINAL_TRI_V_EQ_0_EDGE_TESSFACTOR:
                    case SHADER_OUTPUT_TYPE_FINAL_LINE_DENSITY_TESSFACTOR:
                    {
                        Value* data[8] =
                        {
                            undef,
                            undef,
                            inst->getArgOperand(1),
                            undef,
                            undef,
                            undef,
                            undef,
                            undef,
                        };
                        AddURBWrite(
                            offsetVal1,
                            builder.getInt32(0x4),
                            data,
                            inst);
                        break;
                    }
                    case SHADER_OUTPUT_TYPE_FINAL_QUAD_U_EQ_1_EDGE_TESSFACTOR:
                    case SHADER_OUTPUT_TYPE_FINAL_TRI_W_EQ_0_EDGE_TESSFACTOR:
                    {
                        Value* data[8] =
                        {
                            undef,
                            inst->getArgOperand(1),
                            undef,
                            undef,
                            undef,
                            undef,
                            undef,
                            undef,
                        };
                        AddURBWrite(
                            offsetVal1,
                            builder.getInt32(0x2),
                            data,
                            inst);
                        break;
                    }
                    case SHADER_OUTPUT_TYPE_FINAL_QUAD_V_EQ_1_EDGE_TESSFACTOR:
                    case SHADER_OUTPUT_TYPE_FINAL_TRI_INSIDE_TESSFACTOR:
                    {
                        Value* data[8] =
                        {
                            inst->getArgOperand(1),
                            undef,
                            undef,
                            undef,
                            undef,
                            undef,
                            undef,
                            undef,
                        };
                        AddURBWrite(
                            offsetVal1,
                            builder.getInt32(0x1),
                            data,
                            inst);
                        break;
                    }
                    case SHADER_OUTPUT_TYPE_FINAL_QUAD_U_INSIDE_TESSFACTOR:
                    {
                        Value* data[8] =
                        {
                            undef,
                            undef,
                            undef,
                            inst->getArgOperand(1),
                            undef,
                            undef,
                            undef,
                            undef,
                        };
                        AddURBWrite(
                            offsetVal,
                            builder.getInt32(0x8),
                            data,
                            inst);
                        break;
                    }
                    case SHADER_OUTPUT_TYPE_FINAL_QUAD_V_INSIDE_TESSFACTOR:
                    {
                        Value* data[8] =
                        {
                            undef,
                            undef,
                            inst->getArgOperand(1),
                            undef,
                            undef,
                            undef,
                            undef,
                            undef,
                        };
                        AddURBWrite(
                            offsetVal,
                            builder.getInt32(0x4),
                            data,
                            inst);
                        break;
                    }
                    }
                    instructionToRemove.push_back(inst);
                }
            }
        }
    }

    for (unsigned int i = 0; i < instructionToRemove.size(); i++)
    {
        instructionToRemove[i]->eraseFromParent();
    }
}

llvm::GenIntrinsicInst* HullShaderLowering::AddURBWriteControlPointOutputs(Value* mask, Value* data[8], Instruction* prev)
{
    llvm::IRBuilder<> builder(m_module->getContext());
    builder.SetInsertPoint(prev);

    // Now calculate the correct offset. This would be 
    // CPID * maxAttrIndex + maxPatchConstantOutputs + patchHeaderSize + attributeOffset
    // Step1: mulRes = CPID * maxAttrIndex 
    llvm::GlobalVariable* pGlobal = m_module->getGlobalVariable("MaxNumOfOutputSignatureEntries");
    uint32_t m_pMaxOutputSignatureCount = int_cast<uint32_t>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());
    llvm::Value* controlPtId = prev->getOperand(5);
    llvm::Value* m_pMulRes = nullptr;
    llvm::Value* m_pFinalOffset = nullptr;
    bool isOutputControlPointIdImmed = llvm::isa<llvm::ConstantInt>(controlPtId);
    uint32_t outputControlPointid;

    if(isOutputControlPointIdImmed)
    {
        outputControlPointid = int_cast<uint32_t>(llvm::cast<llvm::ConstantInt>(controlPtId)->getZExtValue());
    }

    if(QuadEltUnit(m_pMaxOutputSignatureCount).Count() != 1)
    {
        if( isOutputControlPointIdImmed )
        {
            m_pMulRes = builder.getInt32(outputControlPointid * QuadEltUnit(m_pMaxOutputSignatureCount).Count());
        }
        else 
        {
            m_pMulRes = builder.CreateMul(controlPtId, builder.getInt32(QuadEltUnit(m_pMaxOutputSignatureCount).Count()));
        }
    }

    // Step2: m_pAddedPatchConstantOutput = maxPatchConstantOutputs + patchHeaderSize + attributeOffset 
    pGlobal = m_module->getGlobalVariable("MaxNumOfPatchConstantSignatureEntries");
    const uint32_t m_pMaxPatchConstantSignatureDeclarations = int_cast<uint32_t>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());
    const uint numPatchConstantsPadded = iSTD::Align(m_pMaxPatchConstantSignatureDeclarations, 2);
    llvm::Value* attributeOffset = prev->getOperand(4);
    bool isAttributeOffsetImmed = llvm::isa<llvm::ConstantInt>(attributeOffset);
    uint32_t immedAttributeOffset = 0;

    if(isAttributeOffsetImmed)
    {
        immedAttributeOffset = int_cast<uint32_t>(llvm::cast<llvm::ConstantInt>(prev->getOperand(4))->getZExtValue());
    }
    // patch constant header is 2 QuadEltUnits
    llvm::Value* m_pAddedPatchConstantOutput = builder.getInt32((QuadEltUnit(numPatchConstantsPadded + 2 + immedAttributeOffset)).Count());
    if(!isAttributeOffsetImmed)
    {
        m_pAddedPatchConstantOutput = builder.CreateAdd(m_pAddedPatchConstantOutput, attributeOffset);
    }

    // Step3: 
    // finalOffset = ( mulRes + m_addedPatchConstantOutput )
    if(m_pMulRes != nullptr) 
    {
        if(isOutputControlPointIdImmed && isAttributeOffsetImmed)
        {
            uint32_t mulRes = int_cast<uint32_t>(llvm::cast<llvm::ConstantInt>(m_pMulRes)->getZExtValue());
            uint32_t addRes = int_cast<uint32_t>(llvm::cast<llvm::ConstantInt>(m_pAddedPatchConstantOutput)->getZExtValue());
            m_pFinalOffset = builder.getInt32( mulRes + addRes);
        }
        else
        {
            m_pFinalOffset = builder.CreateAdd(m_pMulRes, m_pAddedPatchConstantOutput);
        }
    }
    else if(isOutputControlPointIdImmed && isAttributeOffsetImmed)
    {
        uint32_t addRes = int_cast<uint32_t>(llvm::cast<llvm::ConstantInt>(m_pAddedPatchConstantOutput)->getZExtValue());
        m_pFinalOffset = builder.getInt32(outputControlPointid + addRes);
    }
    else
    {
        m_pFinalOffset = builder.CreateAdd(controlPtId, m_pAddedPatchConstantOutput);
    }

    llvm::CallInst* write = AddURBWrite(
                                m_pFinalOffset,
                                mask,
                                data,
                                prev);
    return (llvm::GenIntrinsicInst*)write;
}

/// Inserts new URBWrite instruction with given mask and arguments before 
/// instuction 'prev'. 
/// TODO: This should be a common function for all Lowering passes.
llvm::GenIntrinsicInst* HullShaderLowering::AddURBWrite(
    llvm::Value* offset, 
    llvm::Value* mask,
    llvm::Value* data[8], 
    llvm::Instruction* prev)
{
    Value* arguments[] = 
    {
        offset,
        mask, 
        data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]
    };

    Value* write = GenIntrinsicInst::Create(
        GenISAIntrinsic::getDeclaration(m_module, GenISAIntrinsic::GenISA_URBWrite), 
        arguments, 
        "",
        prev);

    return (llvm::GenIntrinsicInst*)write;
}

void HullShaderLowering::AddURBRead(Value* index, Value* offset, Instruction* prev)
{
    Value* arguments[] = 
    { 
        index, 
        offset
    };

    Instruction* urbRead = GenIntrinsicInst::Create(
        GenISAIntrinsic::getDeclaration(m_module, GenISAIntrinsic::GenISA_URBRead), 
        arguments, 
        "",
        prev);

    Value* vec4 = nullptr;
    while(!prev->use_empty())
    {
        auto I = prev->user_begin();
        if(ExtractElementInst* elem = dyn_cast<ExtractElementInst>(*I))
        {
            Instruction* newExt = ExtractElementInst::Create(urbRead, elem->getIndexOperand(), "", elem);
            elem->replaceAllUsesWith(newExt);
            elem->eraseFromParent();
        }
        else
        {
            // the vector is used directly, extract the first 4 elements and recreate a vec4
            if (vec4 == nullptr)
            {
                Value *data[4] = { nullptr, nullptr, nullptr, nullptr };
                Type *int32Ty = Type::getInt32Ty(m_module->getContext());

                VectorToElement(urbRead, data, int32Ty, prev, 4);
                vec4 = ElementToVector(data, int32Ty, prev, 4);
            }

            (*I)->replaceUsesOfWith(prev, vec4);
        }
    }
}

CollectHullShaderProperties::CollectHullShaderProperties() : llvm::ImmutablePass(ID)
{
    initializeCollectHullShaderPropertiesPass(*PassRegistry::getPassRegistry());
}

void CollectHullShaderProperties::gatherInformation(llvm::Function* kernel)
{
    llvm::Module *module = kernel->getParent();

    llvm::GlobalVariable* pGlobal = module->getGlobalVariable("HSOutputControlPointCount");
    m_hsProps.m_pOutputControlPointCount = int_cast<unsigned int>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());

    pGlobal = module->getGlobalVariable("TessInputControlPointCount");
    m_hsProps.m_pInputControlPointCount = int_cast<unsigned int>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());

    pGlobal = module->getGlobalVariable("MaxNumOfInputSignatureEntries");
    m_hsProps.m_pMaxInputSignatureCount = int_cast<unsigned int>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());

    pGlobal = module->getGlobalVariable("MaxNumOfOutputSignatureEntries");
    m_hsProps.m_pMaxOutputSignatureCount = int_cast<unsigned int>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());

    pGlobal = module->getGlobalVariable("MaxNumOfPatchConstantSignatureEntries");
    m_hsProps.m_pMaxPatchConstantSignatureDeclarations = int_cast<unsigned int>(llvm::cast<llvm::ConstantInt>(pGlobal->getInitializer())->getZExtValue());

    // Dispatch mode might be also determined based on MetaData (which might be treated as Global Variable).
    m_hsProps.m_pShaderDispatchMode = DetermineDispatchMode(kernel);

    m_hsProps.m_ForcedDispatchMask = GetForcedDispatchMask(kernel);

    pGlobal = module->getGlobalVariable("ShaderHasClipCullInput");
    auto clipCullAsInput = (pGlobal == nullptr) ? false : true;
    IGC::CodeGenContext* ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
    if (ctx->m_DriverInfo.HasFixedURBHeaderSize()){
        // In case we have no linking information we need the URB header to have a fixed size
        clipCullAsInput = true; 
    }
    
    m_hsProps.m_HasClipCullAsInput = clipCullAsInput;
}

HullShaderDispatchModes CollectHullShaderProperties::DetermineDispatchMode(Function* kernel) const
{
    HullShaderDispatchModes shaderDispatchMode = SINGLE_PATCH_DISPATCH_MODE;
    llvm::NamedMDNode *pMetaData = kernel->getParent()->getNamedMetadata("HullShaderDispatchMode");
    if(pMetaData)
    {
        llvm::MDNode *pMdNode = pMetaData->getOperand(0);
        if(pMdNode)
        {
            llvm::Metadata *pShaderDispatchMode = pMdNode->getOperand(0);
            shaderDispatchMode = (HullShaderDispatchModes)
                (llvm::mdconst::dyn_extract<ConstantInt>(pShaderDispatchMode))->getZExtValue();
        }
    }
    return shaderDispatchMode;
}

unsigned CollectHullShaderProperties::GetForcedDispatchMask(Function* kernel) const
{
    unsigned dispatchMask = 0;
    llvm::NamedMDNode *pMetaData = kernel->getParent()->getNamedMetadata("HullShaderForcedDispatchMask");
    if (pMetaData)
    {
        llvm::MDNode *pMdNode = pMetaData->getOperand(0);
        if (pMdNode)
        {
            llvm::Metadata *pShaderForcedMask = pMdNode->getOperand(0);
            dispatchMask = static_cast<unsigned>((llvm::mdconst::dyn_extract<ConstantInt>(pShaderForcedMask))->getZExtValue());
        }
    }
    return dispatchMask;
}


HullShaderProperties::HullShaderProperties() :
m_pOutputControlPointCount(0),
m_pInputControlPointCount(0),
m_pMaxInputSignatureCount(0),
m_pMaxOutputSignatureCount(0),
m_pMaxPatchConstantSignatureDeclarations(0),
m_HasClipCullAsInput(false),
m_pShaderDispatchMode(SINGLE_PATCH_DISPATCH_MODE),
m_ForcedDispatchMask(0)
{}

unsigned int HullShaderProperties::GetMaxInputPushed() const
{
    const unsigned int maxNumOfHSPushedInputs = 96;
    uint numberOfPatches = (m_pShaderDispatchMode == EIGHT_PATCH_DISPATCH_MODE) ? 8 : 1;

    // Determine how many of input attributes per InputControlPoint (Vertex) can be POTENTIALLY pushed 
    // in current dispatch mode for current topology ( InputPatch size ).
    uint32_t maxNumOfPushedInputAttributesPerICP = 
        (m_pInputControlPointCount * numberOfPatches > 0)
        ? maxNumOfHSPushedInputs / (m_pInputControlPointCount * numberOfPatches)
        : maxNumOfHSPushedInputs;

    // Input attributes can be pushed only in pairs, so we need to round down the limit.
    maxNumOfPushedInputAttributesPerICP = iSTD::Align(maxNumOfPushedInputAttributesPerICP - 1, 2);

    // Determine required number of input attributes.
    // They can be pushed only in pairs.
    uint32_t reqNumOfInputAttributesPerICP = iSTD::Align(m_pMaxInputSignatureCount, 2);

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


llvm::FunctionPass* createHullShaderLoweringPass()
{
    return new HullShaderLowering();
}

} // namespace IGC
