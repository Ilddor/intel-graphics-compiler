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
#pragma once
#include "Compiler/CISACodeGen/helper.h"
#include "Compiler/CISACodeGen/WIAnalysis.hpp"
#include "Compiler/MetaDataUtilsWrapper.h"
#include "Compiler/CISACodeGen/CollectGeometryShaderProperties.hpp"
#include "Compiler/CISACodeGen/VertexShaderLowering.hpp"
#include "Compiler/CISACodeGen/DomainShaderLowering.hpp"
#include "Compiler/CISACodeGen/HullShaderLowering.hpp"
#include "Compiler/CISACodeGen/PullConstantHeuristics.hpp"
#include "ShaderCodeGen.hpp"
#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/PassManager.h>
#include "common/LLVMWarningsPop.hpp"

#include <map>

namespace IGC
{
enum class PushConstantMode
{
    NO_PUSH_CONSTANT    = 0,
    SIMPLE_PUSH         = 1,
    GATHER_CONSTANT     = 2,
};

class PushAnalysis : public llvm::ModulePass
{
    const llvm::DataLayout *m_DL;
    static const uint32_t MaxConstantBufferIndexSize;
    static const uint32_t MaxNumOfPushedInputs;
    static const uint32_t TessFactorsURBHeader;
    static const uint32_t HSEightPatchMaxNumOfPushedControlPoints;
    static const uint32_t m_pMaxNumOfVSPushedInputs;
    static const uint32_t m_pMaxNumOfHSPushedInputs;
    static const uint32_t m_pMaxNumOfDSPushedInputs;
    static const uint32_t m_pMaxNumOfGSPushedInputs;

	bool m_funcTypeChanged;
	std::map <llvm::Function*, bool> m_isFuncTypeChanged;

    llvm::Module* m_module;
    llvm::Function *m_pFunction;
    IGCMD::MetaDataUtils *m_pMdUtils;
    llvm::PostDominatorTree* m_PDT;
    llvm::DominatorTree* m_DT;
    llvm::BasicBlock* m_entryBB;

    PullConstantHeuristics *m_pullConstantHeuristics;

    CollectHullShaderProperties* m_hsProps;
    CollectDomainShaderProperties* m_dsProps;
    CollectVertexShaderProperties* m_vsProps;
    CollectGeometryShaderProperties* m_gsProps;
    CodeGenContext* m_context;
    llvm::DenseMap<llvm::Instruction*, bool> m_statelessLoads;
    uint    m_cbToLoad;
    uint    m_maxStatelessOffset;
	int    m_argIndex;
	std::vector < llvm::Value* > m_argList;

    // Helper function
    /// Return true if the constant is in the range which we are allowed to push
    bool IsPushableShaderConstant(llvm::Instruction *inst, uint& bufId, uint& eltId);

    bool GetConstantOffsetForDynamicUniformBuffer(
        uint bufferId,
        llvm::Value *offsetValue,
        uint &relativeOffsetInBytes);
    
    /// process simple push for the function
    void BlockPushConstants();

    /// Try to push allocate space for the constant to be pushed
    unsigned int AllocatePushedConstant(
        llvm::Instruction* load, unsigned int cbIdx, unsigned int offset, unsigned int maxSizeAllowed);

    /// promote the load to function argument
    void PromoteLoadToSimplePush(llvm::Instruction* load, SimplePushInfo& info, unsigned int offset);

    /// process stateless push for the function
    void StatlessPushConstant();

    /// Return true if the Load is a stateless.
    bool IsStatelessCBLoad(
        llvm::Instruction *inst,
        llvm::GenIntrinsicInst* &pBaseAddress,
        unsigned int& offset);
    
    /// Push the Stateless CB
    void ReplaceStatelessCBLoad(
        llvm::Instruction *inst,
        llvm::GenIntrinsicInst* &pBaseAddress,
        const unsigned int& offset);

    /// return true if the inputs are uniform
    bool AreUniformInputsBasedOnDispatchMode();
    /// return true if we are allowed to push constants
    bool CanPushConstants();

    /// return the maximum number of inputs pushed for this kernel
    unsigned int GetMaxNumberOfPushedInputs();
    unsigned int GetHSMaxNumberOfPushedInputs();
    bool DispatchGRFHardwareWAForHSAndGSDisabled();
	void updateNewFuncArgs(llvm::Function* , llvm::Function* );
	llvm::FunctionType* getNewFuncType(llvm::Function* );


    /// return the push constant mode supported based on driver and platform support
    PushConstantMode GetPushConstantMode();
public:
    static char ID;
    PushAnalysis();
    virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const override
    {
        AU.setPreservesCFG();
        AU.addRequired<MetaDataUtilsWrapper>();
        AU.addRequired<CodeGenContextWrapper>();
        AU.addRequired<llvm::PostDominatorTreeWrapperPass>();
        AU.addRequired<llvm::DominatorTreeWrapperPass>();
        AU.addRequired<PullConstantHeuristics>();
    }

    llvm::Argument* addArgumentAndMetadata(llvm::Type *pType, std::string argName, IGC::WIAnalysis::WIDependancy dependency);
	bool runOnModule(llvm::Module& ) override;
    void ProcessFunction();
    void AnalyzeFunction(llvm::Function *F)
    {
        m_pFunction = F;
        m_module = F->getParent();

		// We need to initialize m_argIndex and m_argList appropriately as there might be some arguments added before push analysis stage
		m_argIndex = 0;
		m_argList.clear();
		for (auto arg = m_pFunction->arg_begin(); arg != m_pFunction->arg_end(); ++arg)
		{
			m_argList.push_back(&(*arg));
			m_argIndex++;
		}
		m_argIndex = m_argIndex - 1;
        m_DL = &F->getParent()->getDataLayout();
        m_pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
        m_pullConstantHeuristics = &getAnalysis<PullConstantHeuristics>();
        m_hsProps = getAnalysisIfAvailable<CollectHullShaderProperties>();
        m_dsProps = getAnalysisIfAvailable<CollectDomainShaderProperties>();
        m_gsProps = getAnalysisIfAvailable<CollectGeometryShaderProperties>();
        m_vsProps = getAnalysisIfAvailable<CollectVertexShaderProperties>();

        if (m_pMdUtils->findFunctionsInfoItem(F) != m_pMdUtils->end_FunctionsInfo())
        {
            // TODO: when doing codegen for cps shader.  We will run CodeGen twice, 
            // first for coarse_phase, then pixel_phase, see CodeGen().
            // While the pushinfo metadata is not stored using function as index.
            // So when doing codege for pixel_phase, we need to clear legacy
            // pushinfo metadata from coarse_phase. A better solution is to store
            // pushinfo with function index. It's tricky here, refactor in future.
            ModuleMetaData *modMD = getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();
            modMD->pushInfo.pushAnalysisWIInfos.clear();

            ProcessFunction();
        }
    }

    virtual llvm::StringRef getPassName() const override {
        return "PushAnalysis";
    }
};

}//namespace IGC
