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
#include "Compiler/IGCPassSupport.h"
#include "common/LLVMWarningsPush.hpp"
#include "llvm/IR/InstIterator.h"
#include "common/LLVMWarningsPop.hpp"
#include "VertexShaderLowering.hpp"
#include <array>

using namespace IGC::IGCMD;
using namespace llvm;

namespace IGC
{
#define PASS_FLAG "igc-collect-vertex-shader-properties"
#define PASS_DESCRIPTION "Collect information related to vertex shader"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS true
    IGC_INITIALIZE_PASS_BEGIN(CollectVertexShaderProperties, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
    IGC_INITIALIZE_PASS_END(CollectVertexShaderProperties, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

//undef macros to avoid redefinition warnings
#undef PASS_FLAG
#undef PASS_DESCRIPTION
#undef PASS_ANALYSIS

#define PASS_FLAG "igc-vertex-shader-lowering"
#define PASS_DESCRIPTION "Lower inputs outputs for vertex shader"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
        IGC_INITIALIZE_PASS_BEGIN(VertexShaderLowering, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
        IGC_INITIALIZE_PASS_DEPENDENCY(CollectVertexShaderProperties)
        IGC_INITIALIZE_PASS_END(VertexShaderLowering, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
char VertexShaderLowering::ID = 0;

VertexShaderLowering::VertexShaderLowering() : FunctionPass(ID)
{
    initializeVertexShaderLoweringPass(*PassRegistry::getPassRegistry());
}

bool VertexShaderLowering::runOnFunction(llvm::Function &F)
{
    m_context = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
    memset(m_inputUsed, 0, sizeof(m_inputUsed));
    m_headerSize = QuadEltUnit(2);
    m_isHeaderPresent = false;
    m_vsPropsPass = &getAnalysis<CollectVertexShaderProperties>();

    m_module = F.getParent();
    LowerIntrinsicInputOutput(F);
    return false;
}

unsigned int VertexShaderLowering::InsertInEmptySlot(Instruction* sgv)
{
    unsigned int index = GetUnusedInputSlot();
    m_inputUsed[index] = true;
    uint usage =
        (uint)llvm::cast<llvm::ConstantInt>(sgv->getOperand(0))->getZExtValue();

    if (usage == GS_INSTANCEID)
    {
        llvm::NamedMDNode *pMetaData = m_module->getNamedMetadata("ConstantBufferIndexedWithInstanceId");
        if (pMetaData != nullptr)
        {
            Constant* cval = ConstantInt::get(
                Type::getInt32Ty(m_module->getContext()), index);
            llvm::MDNode *pMdNode = llvm::MDNode::get(
                m_module->getContext(),
                ConstantAsMetadata::get(cval));

            pMetaData->addOperand(pMdNode);
        }
    }
    Value* zero = ConstantInt::get(Type::getInt32Ty(sgv->getContext()), 0);
    Value* arguments[] = 
    { 
        zero,
        ConstantInt::get(Type::getInt32Ty(sgv->getContext()), index/4), 
    };
    Instruction* urbRead = GenIntrinsicInst::Create(
        GenISAIntrinsic::getDeclaration(m_module, GenISAIntrinsic::GenISA_URBRead), 
        arguments, 
        "",
        sgv);
    Value* channel = ConstantInt::get(Type::getInt32Ty(sgv->getContext()), index%4);
    Instruction* newExt = ExtractElementInst::Create(urbRead, channel, "", sgv);
    sgv->replaceAllUsesWith(newExt);
    return index;
}

unsigned int VertexShaderLowering::GetUnusedInputSlot()
{    
    for(unsigned int i = 0; i<32*4; i++)
    {
        if(m_inputUsed[i] == false)
        {
            return i;
        }
    }
    assert(0 && "All input slots are already used, cannot find an empty one");
    return 32*4-1;
}

/// this is the heuristic pattern for IGC to tell driver to use single-instance vertex shader
bool VertexShaderLowering::determineIfInstructionUsingInstanceIDisConstantBuffer(
    Value* user )
{
    bool foundConstantBufferAccessedWithInstanceID = false;
    if (cast<Instruction>(user)->getOpcode() == Instruction::Shl)
    {
        // if instance_id is being used in a shl look for the shl instructions first use
        user = *((Instruction*)user)->user_begin();
    }

    if (cast<Instruction>(user)->getOpcode() == Instruction::Add)
    {
        // if instance_id is being used in a add look for the add instructions first use
        user = *((Instruction*)user)->user_begin();
    }

    if (isa<IntToPtrInst>(user))
    {
        uint as = cast<IntToPtrInst>(user)->getType()->getPointerAddressSpace();
        if (GetBufferType(as) == CONSTANT_BUFFER)
        {
            m_module->getOrInsertNamedMetadata("ConstantBufferIndexedWithInstanceId");
            foundConstantBufferAccessedWithInstanceID = true;
        }
    }
    return foundConstantBufferAccessedWithInstanceID;
}

void VertexShaderLowering::LowerIntrinsicInputOutput(Function& F)
{
    // This is quite inefficient but should go away once we are not using intrinsics anymore
    CalculateVertexHeaderSize(F);
    Instruction* vertexId = nullptr;
    Instruction* InstanceId = nullptr;
	std::array<Instruction*, 3> vertexFetchSGVExtendedParameters = {};
    SmallVector<Instruction*, 10> instructionToRemove;
    Value* zero = ConstantInt::get(Type::getInt32Ty(F.getContext()), 0);
    std::vector<llvm::Instruction*> offsetInst(MaxNumOfOutput + m_headerSize.Count(), nullptr);
    Value* undef = llvm::UndefValue::get(Type::getFloatTy(F.getContext()));
    
    Value* headerInitValue = undef;
    if (m_context->m_DriverInfo.NeedClearVertexHeader())
    {
        headerInitValue = ConstantFP::get(Type::getFloatTy(F.getContext()), 0);
    }

    //For holding RTAI, VPAI and Point Width
    Value* headerData[8] =
    {
        headerInitValue,
        headerInitValue,
        headerInitValue,
        headerInitValue,
        undef,
        undef,
        undef,
        undef,
    };
    bool hasHeaderData = false;
    GenIntrinsicInst* prevInst = nullptr;
    Value* headerOffset = nullptr;

    for(auto BI = F.begin(), BE = F.end(); BI != BE; BI++)
    {
        for(auto II = BI->begin(), IE = BI->end(); II!=IE; II++)
        {
            if(GenIntrinsicInst* inst = dyn_cast<GenIntrinsicInst>(II))
            {
                GenISAIntrinsic::ID IID = inst->getIntrinsicID();
                if(IID == GenISAIntrinsic::GenISA_OUTPUT)
                {
                    const ShaderOutputType usage = static_cast<ShaderOutputType>(
                        llvm::cast<llvm::ConstantInt>(inst->getOperand(4))->getZExtValue());
                    Value* attribut = inst->getOperand(5);
                  
                    unsigned int attribOffset = GetURBOffset(usage, attribut, inst);
                    Value* offset = ConstantInt::get(Type::getInt32Ty(m_module->getContext()), attribOffset);
                    offsetInst[attribOffset] = inst;
                    if (usage == SHADER_OUTPUT_TYPE_RENDER_TARGET_ARRAY_INDEX)
                    {
                        headerData[1] = inst->getOperand(0);
                        hasHeaderData = true;
                        prevInst = inst;
                        headerOffset = offset;
                    }
                    else if (usage == SHADER_OUTPUT_TYPE_VIEWPORT_ARRAY_INDEX)
                    {
                        headerData[2] = inst->getOperand(0);
                        hasHeaderData = true;
                        prevInst = inst;
                        headerOffset = offset;
                    }
                    else if (usage == SHADER_OUTPUT_TYPE_POINTWIDTH)
                    {
                        headerData[3] = inst->getOperand(0);
                        hasHeaderData = true;
                        prevInst = inst;
                        headerOffset = offset;   
                    }
                    else
                    {
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
                        AddURBWrite(offset, 0xF, data, inst);
                    }
                    instructionToRemove.push_back(inst);
                }
                else if(IID == GenISAIntrinsic::GenISA_DCL_ShaderInputVec)
                {
                    AddURBRead(zero, inst->getOperand(0), inst);
                    instructionToRemove.push_back(inst);
                }
                else if(IID == GenISAIntrinsic::GenISA_DCL_SystemValue)
                {
                    uint usage = 
                        (uint) llvm::cast<llvm::ConstantInt>(inst->getOperand(0))->getZExtValue();
					switch (usage)
					{
					case VERTEX_ID:
						vertexId = inst;
						break;

					case GS_INSTANCEID:
						InstanceId = inst;
						if (m_context->platform.supportSingleInstanceVertexShader() &&
							(IGC_IS_FLAG_ENABLED(EnableSingleVertexDispatch) ||
							(m_context->m_DriverInfo.SupportsSingleInstanceVertexDispatch())))
						{
							auto useIterBegin = inst->user_begin(), useIterEnd = inst->user_end();

							// if one use of instance_id with constant buffer is found we dont need to look for more
							bool foundConstantBufferAccessedWithInstanceID = false;
							while ((useIterBegin != useIterEnd) &&
								!foundConstantBufferAccessedWithInstanceID)
							{
								Value* user = *useIterBegin;

								if (llvm::isa<llvm::BitCastInst>(user))
								{
									// if instance_id is being used in a bitcast look for all the bitcast instructions uses
									// to make sure none of the bitcast instructions users use this to index into constanct buffers
									auto bitCastUserIterBegin = user->user_begin(), bitCastUserIterEnd = user->user_end();
									while ((bitCastUserIterBegin != bitCastUserIterEnd) &&
										!foundConstantBufferAccessedWithInstanceID)
									{
										foundConstantBufferAccessedWithInstanceID = determineIfInstructionUsingInstanceIDisConstantBuffer(*bitCastUserIterBegin);
										++bitCastUserIterBegin;
									}
								}
								else
								{
									foundConstantBufferAccessedWithInstanceID = determineIfInstructionUsingInstanceIDisConstantBuffer(user);
								}
								++useIterBegin;
							}
						}
						break;

					case VF_XP0:
					case VF_XP1:
					case VF_XP2:
						assert(m_context->platform.supportsDrawParametersSGVs());
						vertexFetchSGVExtendedParameters.at(usage - VF_XP0) = inst;
						break;

					default:
						break;
					}
                }
            }
        }
    }
    if (hasHeaderData)
    {
        AddURBWrite(headerOffset, 0xF, headerData, prevInst);
    }
    if(vertexId)
    {
        unsigned int slot = InsertInEmptySlot(vertexId);
        m_vsPropsPass->SetVertexIdSlot(slot);
    }
    if(InstanceId)
    {
        unsigned int slot = InsertInEmptySlot(InstanceId);
        m_vsPropsPass->SetInstanceIdSlot(slot);
    }
	for (size_t paramIndex = 0; paramIndex < vertexFetchSGVExtendedParameters.size(); ++paramIndex)
	{
		if (vertexFetchSGVExtendedParameters[paramIndex])
		{
			unsigned int slot = InsertInEmptySlot(vertexFetchSGVExtendedParameters[paramIndex]);
			m_vsPropsPass->SetShaderDrawParameter(paramIndex, slot);
		}
	}

    //URB padding to 32Byte offsets
    for(unsigned int i = 0; i < MaxNumOfOutput + m_headerSize.Count(); i++)
    {
        //If not aligned to 32Byte offset and has valid data
        if(offsetInst[i])
        {
            unsigned int adjacentSlot = i + (i % 2 == 0 ? 1 : -1);
            if(offsetInst[adjacentSlot])
            {
                //Valid data present
                continue;
            }
            else
            {
                //No need to pad URB at offset position 0 for OGL since it is always padded with 0
                if(i == 1 && m_context->m_DriverInfo.NeedClearVertexHeader())
                {
                    break;
                }
                else
                {
                    Value* undef = llvm::UndefValue::get(Type::getFloatTy(F.getContext()));

                    Value* data[8] = 
                    {
                        undef,
                        undef,
                        undef,
                        undef,
                        undef,
                        undef,
                        undef,
                        undef,
                    };

                    Value* offset = ConstantInt::get(Type::getInt32Ty(m_module->getContext()), adjacentSlot);
                    AddURBWrite(offset, 0xF, data, offsetInst[i]);
                }
            }
        }
    }

    for(unsigned int i = 0; i<instructionToRemove.size(); i++)
    {
        instructionToRemove[i]->eraseFromParent();
    }
}

unsigned int VertexShaderLowering::GetURBOffset(ShaderOutputType type, Value* attribute, Instruction* inst)
{
    switch(type)
    {
    case SHADER_OUTPUT_TYPE_POSITION:
        return 1;
    case SHADER_OUTPUT_TYPE_CLIPDISTANCE_LO:
        return (m_headerSize.Count() - 2);
    case  SHADER_OUTPUT_TYPE_CLIPDISTANCE_HI:
        return (m_headerSize.Count() - 1);
    case SHADER_OUTPUT_TYPE_DEFAULT:
        {
            ConstantInt* attributeInt = (ConstantInt*)(attribute);
            if(attributeInt)
            {
                uint attributeValue = (uint)attributeInt->getZExtValue();
                return (attributeValue + m_headerSize.Count());
            }
            else
            {
                assert(0 && "This case of indirect accessing of attribute  shouldn't be hit");
            }
        }
    case SHADER_OUTPUT_TYPE_POINTWIDTH:
        return 0;
        break;
    case SHADER_OUTPUT_TYPE_VIEWPORT_ARRAY_INDEX:
        m_vsPropsPass->DeclareVPAI();
        return 0;
        break;
    case SHADER_OUTPUT_TYPE_RENDER_TARGET_ARRAY_INDEX:
        m_vsPropsPass->DeclareRTAI();
        return 0;
        break;
    default:
        assert(!"unknown VS output type");
        break;
    }
    return 0;
}

void VertexShaderLowering::CalculateVertexHeaderSize(Function& F)
{
    if (m_context->m_DriverInfo.HasFixedURBHeaderSize())
    {
        m_headerSize = QuadEltUnit(4);
        m_vsPropsPass->DeclareClipDistance();
    }
    else
    {
        m_headerSize = QuadEltUnit(2); 
    }

    for (auto I = F.begin(), E = F.end(); I != E; ++I)
    {
        llvm::BasicBlock *pLLVMBB = &(*I);
        llvm::BasicBlock::InstListType &instructionList = pLLVMBB->getInstList();
        for (auto I = instructionList.begin(), E = instructionList.end(); I != E; ++I)
        {
            if (GenIntrinsicInst* inst = dyn_cast<GenIntrinsicInst>(I))
            {
                if (inst->getIntrinsicID() == GenISAIntrinsic::GenISA_OUTPUT)
                {
                    const ShaderOutputType usage = static_cast<ShaderOutputType>(
                        llvm::cast<llvm::ConstantInt>(inst->getOperand(4))->getZExtValue());
                    if (usage == SHADER_OUTPUT_TYPE_CLIPDISTANCE_LO ||
                        usage == SHADER_OUTPUT_TYPE_CLIPDISTANCE_HI)
                    {
                        m_headerSize = QuadEltUnit(4);
                        m_vsPropsPass->DeclareClipDistance();
                    }
                }
            }
        }
    }
}

void VertexShaderLowering::AddURBRead(Value* index, Value* offset, Instruction* prev)
{
    Value* arguments[] = 
    { 
        index, 
        offset
    };
    unsigned int inIndex = int_cast<unsigned int>(cast<ConstantInt>(offset)->getZExtValue());
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
            Value* extIdx = elem->getIndexOperand();
            Instruction* newExt = ExtractElementInst::Create(urbRead, extIdx, "", elem);
            if (isa<ConstantInt>(extIdx))
            {
                unsigned int channel = int_cast<unsigned int>(
                    cast<ConstantInt>(extIdx)->getZExtValue());
                m_inputUsed[inIndex * 4 + channel] = true;
            }
            else
            {
                m_inputUsed[inIndex * 4] =
                    m_inputUsed[inIndex * 4 + 1] =
                    m_inputUsed[inIndex * 4 + 2] =
                    m_inputUsed[inIndex * 4 + 3] = true;
            }
            elem->replaceAllUsesWith(newExt);
            elem->eraseFromParent();
        }
        else
        {
            // the vector is used directly, extract the first 4 elements and recreate a vec4
            if (vec4 == nullptr)
            {
                Value *data[4]={nullptr, nullptr, nullptr, nullptr};
                Type *int32Ty = Type::getInt32Ty(m_module->getContext());

                VectorToElement(urbRead, data, int32Ty, prev, 4);
                vec4 = ElementToVector(data, int32Ty, prev, 4);
                m_inputUsed[inIndex * 4] =
                    m_inputUsed[inIndex * 4 + 1] =
                    m_inputUsed[inIndex * 4 + 2] =
                    m_inputUsed[inIndex * 4 + 3] = true;
            }
            
            (*I)->replaceUsesOfWith(prev, vec4);
        }
    }
}

void VertexShaderLowering::AddURBWrite(Value* offset, uint mask, Value* data[8], Instruction* prev)
{
    if(!m_isHeaderPresent && 
        m_context->m_DriverInfo.NeedClearVertexHeader())
    {
        m_isHeaderPresent = true;
        AddInitializedHeader(prev);
    }
    assert(mask < 256 && "mask is an 8-bit bitmask and has to be in range 0..255");
    Value* arguments[] = 
    {
        offset,
        ConstantInt::get(Type::getInt32Ty(m_module->getContext()), mask), 
        data[0],
        data[1],
        data[2],
        data[3],
        data[4],
        data[5],
        data[6],
        data[7]
    };

    GenIntrinsicInst::Create(GenISAIntrinsic::getDeclaration(m_module, GenISAIntrinsic::GenISA_URBWrite), 
        arguments, 
        "",
        prev);
    if(ConstantInt* immOffset = dyn_cast<ConstantInt>(offset))
    {
        uint offset = int_cast<unsigned int>(immOffset->getZExtValue() + (mask <= 0xF ? 1 : 2));
        m_vsPropsPass->DeclareOutput(QuadEltUnit(offset));
    }
    else
    {
        m_vsPropsPass->DeclareOutput(QuadEltUnit(32));
    }
    
}

void VertexShaderLowering::AddInitializedHeader(Instruction* prev)
{
    Value* undef = llvm::UndefValue::get(Type::getFloatTy(m_module->getContext()));
    Value* zero = ConstantInt::get(Type::getInt32Ty(m_module->getContext()), 0);
    Value* zeroFloat = ConstantFP::get(Type::getFloatTy(m_module->getContext()), 0);
    Value* data[8] = 
    {
        zeroFloat,
        zeroFloat,
        zeroFloat,
        zeroFloat,
        undef,
        undef,
        undef,
        undef,
    };
    AddURBWrite(zero, 0xF, data, prev);
}

VertexShaderProperties::VertexShaderProperties() :
m_HasVertexID(false),
m_VID(0),
m_HasInstanceID(false),
m_IID(0),
m_VertexFetchSGVExtendedParameters(),
m_hasRTAI(false), 
m_hasVPAI(false), 
m_hasClipDistance(false), 
m_URBOutputLength(0)
{

}

CollectVertexShaderProperties::CollectVertexShaderProperties() : ImmutablePass(ID) 
{
    initializeCollectVertexShaderPropertiesPass(*PassRegistry::getPassRegistry());
}

void CollectVertexShaderProperties::DeclareOutput(QuadEltUnit newMaxOffset)
{
    if(m_vsProps.m_URBOutputLength < newMaxOffset)
    {
        m_vsProps.m_URBOutputLength = newMaxOffset;
    }
}

void CollectVertexShaderProperties::DeclareRTAI()
{
    m_vsProps.m_hasRTAI = true;
}


void CollectVertexShaderProperties::DeclareVPAI()
{
    m_vsProps.m_hasVPAI = true;
}

void CollectVertexShaderProperties::DeclareClipDistance()
{
    m_vsProps.m_hasClipDistance = true;
}

void CollectVertexShaderProperties::SetInstanceIdSlot(unsigned int IIDslot)
{
    m_vsProps.m_HasInstanceID = true;
    m_vsProps.m_IID = IIDslot;
}

void CollectVertexShaderProperties::SetVertexIdSlot(unsigned int VIDSlot)
{
    m_vsProps.m_HasVertexID = true;
    m_vsProps.m_VID = VIDSlot;
}

void CollectVertexShaderProperties::SetShaderDrawParameter(size_t paramIndex, unsigned int slot)
{
	assert(paramIndex < ARRAY_COUNT(m_vsProps.m_VertexFetchSGVExtendedParameters.extendedParameters));
	auto& parameter = m_vsProps.m_VertexFetchSGVExtendedParameters.extendedParameters[paramIndex];
	parameter.enabled = true;
	parameter.location = slot;
}


char CollectVertexShaderProperties::ID = 0;

}//namespace IGC

