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

#include "Compiler/CISACodeGen/CVariable.hpp"
#include "Compiler/CISACodeGen/PushAnalysis.hpp"
#include "Compiler/CISACodeGen/helper.h"
#include "Compiler/CISACodeGen/CISACodeGen.h"
#include "Compiler/CISACodeGen/CISABuilder.hpp"
#include "Compiler/CISACodeGen/LiveVars.hpp"
#include "Compiler/CISACodeGen/WIAnalysis.hpp"
#include "Compiler/CISACodeGen/CoalescingEngine.hpp"
#include "Compiler/CodeGenPublic.h"
#include "Compiler/MetaDataApi/MetaDataApi.h"

// Needed for SConstantGatherEntry
#include "usc_gen7.h"

#include "common/Types.hpp"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/MapVector.h>
#include "common/LLVMWarningsPop.hpp"

#include "common/debug/Dump.hpp"

#include <map>
#include <vector>

namespace llvm
{
    class Value;
    class PHINode;
    class Function;
    class BasicBlock;
}

namespace IGC
{
class DeSSA;
class CoalescingEngine;
class GenXFunctionGroupAnalysis;
class VariableReuseAnalysis;

struct PushInfo;

// Helper Function
VISA_Type GetType(llvm::Type* pType, CodeGenContext* pDataLayout);
uint64_t GetImmediateVal(llvm::Value* Const);
e_alignment GetPreferredAlignment(llvm::Value *Val, WIAnalysis *WIA, CodeGenContext* pContext);

class CShaderProgram;

///--------------------------------------------------------------------------------------------------------
class CShader
{
public:
    friend class CShaderProgram;
    CShader(llvm::Function*, CShaderProgram* pProgram);
    virtual ~CShader();
    void        Destroy();
    virtual void InitEncoder(SIMDMode simdMode, bool canAbortOnSpill, ShaderDispatchMode shaderMode = ShaderDispatchMode::NOT_APPLICABLE);
    virtual void PreCompile() {}
    virtual void ParseShaderSpecificOpcode(llvm::Instruction* inst) {}
    virtual void AllocatePayload() {}
    virtual void AddPrologue() {}
    void PreAnalysisPass();
    virtual void ExtractGlobalVariables() {}
    void         EmitEOTURBWrite();
    void         EOTRenderTarget();
    virtual void AddEpilogue(llvm::ReturnInst* ret);

    virtual CVariable* GetURBOutputHandle() 
    { 
        assert(!"Should be overridden in a derived class!"); 
        return nullptr;
    }
    virtual CVariable* GetURBInputHandle(CVariable* pVertexIndex) 
    { 
        assert(!"Should be overridden in a derived class!"); 
        return nullptr; 
    }
    virtual QuadEltUnit GetFinalGlobalOffet(QuadEltUnit globalOffset) { return QuadEltUnit(0); }
    virtual bool hasReadWriteImage(llvm::Function &F) { return false; }
    virtual bool CompileSIMDSize(SIMDMode simdMode, EmitPass &EP, llvm::Function &F) { return true; }
    CVariable*  LazyCreateCCTupleBackingVariable(
        CoalescingEngine::CCTuple* ccTuple,
        VISA_Type baseType = ISA_TYPE_UD);
    CVariable*  GetSymbol(llvm::Value* value, bool fromConstantPool = false);
    CVariable*  GetPhiTemp(llvm::PHINode* node);
    void        AddSetup(uint index, CVariable* var);
    void        AddPatchConstantSetup(uint index, CVariable* var);
    CVariable*  GetNewVariable(uint16_t nbElement, VISA_Type type, e_alignment align, bool uniform = false, uint16_t numberInstance = 1);
    CVariable*  GetNewVariable(const CVariable* from);
    CVariable*  GetNewAddressVariable(uint16_t nbElement, VISA_Type type, bool uniform, bool vectorUniform);
    CVariable*  GetNewVector(llvm::Value* val, e_alignment preferredAlign = EALIGN_AUTO);
    CVariable*  GetNewAlias(CVariable* var, VISA_Type type, uint16_t offset, uint16_t numElements);
    CVariable*  GetNewAlias(CVariable* var, VISA_Type type, uint16_t offset, uint16_t numElements, bool uniform);
    // Allow to create an alias of a variable handpicking a slice to be able to do cross lane in SIMD32
    CVariable*  GetVarHalf(CVariable* var, unsigned int half);
    
    void        CopyVariable(CVariable* dst, CVariable* src, uint dstSubVar = 0, uint srcSubVar = 0);
    void        PackAndCopyVariable(CVariable* dst, CVariable* src, uint subVar = 0);
    bool        IsValueUsed(llvm::Value* value);
    uint        GetNbElementAndMask(llvm::Value* value, uint32_t &mask);
    void        CreatePayload(uint regCount, uint idxOffset, CVariable*& payload, llvm::Instruction* inst, uint paramOffset, uint8_t hfFactor);
    uint        GetNbVectorElementAndMask(llvm::Value* value, uint32_t &mask);
    uint32_t    GetExtractMask(llvm::Value* value);
    uint16_t    AdjustExtractIndex(llvm::Value* value, uint16_t elemIndex);
    bool        GetIsUniform(llvm::Value* v) const;
    bool        InsideDivergentCF(llvm::Instruction* inst);
    CEncoder&   GetEncoder();
    CVariable*  GetR0();
    CVariable*  GetNULL();
    CVariable*  GetTSC();
    CVariable*  GetSR0();
    CVariable*  GetCR0();
    CVariable*  GetCE0();
    CVariable*  GetDBG();
    CVariable*  GetHWTID();
    CVariable*  GetSP();
    CVariable*  GetARGV();
    CVariable*  GetRETV();
    CVariable*  CreateSP(bool ptr64bits);
    /// init stack-pointer at the beginning of the kernel
    void InitKernelStack(bool ptr64bits);
    /// save the stack-pointer when entering a stack-call function
    void SaveSP();
    /// restore the stack-pointer when exiting a stack-call function
    void RestoreSP();

    void        AllocateInput(CVariable* var, uint offset, uint instance = 0);
    void        AllocateOutput(CVariable* var, uint offset, uint instance = 0);
    CVariable*  ImmToVariable(uint64_t immediate, VISA_Type type);
    CVariable*  GetConstant(llvm::Value* c);
    CVariable*  GetScalarConstant(llvm::Value* c);
    CVariable*  GetUndef(VISA_Type type);
    llvm::Constant*  findCommonConstant(llvm::Constant *C, uint elts, uint currentEmitElts, bool &allSame);
    virtual unsigned int GetGlobalMappingValue(llvm::Value* c);
    virtual CVariable* GetGlobalMapping(llvm::Value* c);
    CVariable*  BitCast(CVariable* var, VISA_Type newType);
    void        ResolveAlias(CVariable* var);
	void        CacheArgumentsList();
    void        MapPushedInputs();
    void        CreateGatherMap();
    void        CreateConstantBufferOutput(SKernelProgram *pKernelProgram);

    void        CreateImplicitArgs();
    uint        GetBlockId(llvm::BasicBlock* block);
    uint        GetNumSBlocks() { return m_numBlocks; }

    void        SetUniformHelper(WIAnalysis* WI) { m_WI = WI;}
    void        SetDeSSAHelper(DeSSA* deSSA) { m_deSSA = deSSA; }
    void        SetCoalescingEngineHelper(CoalescingEngine* ce) { m_coalescingEngine = ce; }
    void        SetCodeGenHelper(CodeGenPatternMatch* CG) { m_CG = CG; }
	void        SetPushInfoHelper(PushInfo* PI) { pushInfo = *PI; }
    void        SetDominatorTreeHelper(llvm::DominatorTree* DT) { m_DT = DT; }
    void        SetDataLayout(const llvm::DataLayout* DL) { m_DL = DL; }
    void        SetFunctionGroupAnalysis(GenXFunctionGroupAnalysis *FGA) { m_FGA = FGA; }
    void        SetVariableReuseAnalysis(VariableReuseAnalysis *VRA) { m_VRA = VRA; }
    void        SetMetaDataUtils(IGC::IGCMD::MetaDataUtils *pMdUtils) { m_pMdUtils = pMdUtils; }
    IGCMD::MetaDataUtils *GetMetaDataUtils() { return m_pMdUtils; }

    virtual  void SetShaderSpecificHelper(EmitPass* emitPass) {}

    void        AllocateConstants(uint& offset);
    void        AllocateStatelessConstants(uint& offset);
    void        AllocateSimplePushConstants(uint& offset);
    void        AllocateNOSConstants(uint& offset);
    void        AllocateConstants3DShader(uint& offset);
    ShaderType  GetShaderType() const { return GetContext()->type; }
    bool        IsValueCoalesced(llvm::Value* v);
    void        ConstantBufferAccesed(uint index);

    void        SampleHeader(CVariable* payload, uint offset, uint writeMask, uint rti);

    bool        GetHasBarrier() const { return m_HasBarrier; } 
    void        SetHasBarrier() { m_HasBarrier = true; }

    bool        GetDSDualPatch() const { return m_ShaderDispatchMode == ShaderDispatchMode::DUAL_PATCH; }

    void        GetSimdOffsetBase(CVariable*& pVar);
    /// Returns a simd8 register filled with values [24, 20, 16, 12, 8, 4, 0]
    /// that are used to index subregisters of a GRF when counting offsets in bytes.
    /// Used e.g. for indirect addressing via a0 register.
    CVariable* GetPerLaneOffsetsReg(uint typeSizeInBytes);

    void        GetPayloadElementSymbols(llvm::Value *inst, CVariable *payload[], int vecWidth);

    CodeGenContext*   GetContext() const { return m_ctx; }

    SProgramOutput*   ProgramOutput();

    bool CanTreatAsAlias(llvm::ExtractElementInst *inst);
    bool CanTreatScalarSourceAsAlias(llvm::InsertElementInst *);

    bool VMECoalescePattern(llvm::GenIntrinsicInst*);

    bool isUnpacked(llvm::Value* value);

    void FillGTPinRequest(SKernelProgram* pKernelProgram);

    llvm::Function* entry;
    const CBTILayout* m_pBtiLayout;
    const CPlatform*  m_Platform;
    const CDriverInfo* m_DriverInfo;
    
    ModuleMetaData* m_ModuleMetadata;

    /// Dispatch size is the number of logical threads running in one hardware thread
    SIMDMode m_dispatchSize;
    /// SIMD Size is the default size of instructions
    ShaderDispatchMode m_ShaderDispatchMode;
    SIMDMode m_SIMDSize;
    uint8_t m_numberInstance;
	PushInfo pushInfo;
    bool isInputsPulled; //true if any input is pulled, false otherwise
    bool isMessageTargetDataCacheDataPort;
    uint m_sendStallCycle;
    uint m_staticCycle;
    unsigned m_spillSize = 0;
    float m_spillCost = 0;          // num weighted spill inst / total inst

	std::vector<llvm::Value*> m_argListCache;

    /// The size in byte used by igc (non-spill space). And this
    /// is the value passed to VISA so that VISA's spill, if any,
    /// will go after this space.
    uint m_ScratchSpaceSize;

    ShaderStats *m_shaderStats;

    // Number of binding table entries per cache line.
    static const DWORD cBTEntriesPerCacheLine = 32;
    // Max BTI value that can increase binding table count.
    // SampleEngine:    Binding Table Index is set to 252 specifies the bindless surface offset.
    // DataPort:        The special entry 255 is used to reference Stateless A32 or A64 address model, 
    //                  and the special entry 254 is used to reference the SLM address model.
    //                  The special entry 252 is used to reference bindless resource operation.
    static const DWORD MAX_BINDING_TABLE_INDEX = 251;

    CVariable* GetCCTupleToVariableMapping(CoalescingEngine::CCTuple* ccTuple)
    {
        return ccTupleMapping[ccTuple];
    }

    void addConstantInPool(llvm::Constant *C, CVariable *Var) {
        ConstantPool[C] = Var;
    }

    CVariable *lookupConstantInPool(llvm::Constant *C) {
        return ConstantPool.lookup(C);
    }

    /// Initialize per function status.
    void BeginFunction(llvm::Function *F);
    /// This method is used to create the vISA variable for function F's formal return value 
    CVariable* getOrCreateReturnSymbol(llvm::Function *F);
    /// This method is used to create the vISA variable for function F's formal argument 
    CVariable* getOrCreateArgumentSymbol(llvm::Argument *Arg, bool useStackCall = false);
    VISA_Type GetType(llvm::Type* type);       

    /// Evaluate constant expression and return the result immediate value.
    uint64_t GetConstantExpr(llvm::ConstantExpr *C);


    uint32_t GetMaxUsedBindingTableEntryCount(void) const
    {
        if (m_BindingTableUsedEntriesBitmap != 0)
        {
            // m_BindingTableEntryCount is index; '+ 1' due to calculate total used count.
            return (m_BindingTableEntryCount + 1);
        }
        return 0;
    }

    uint32_t GetBindingTableEntryBitmap(void) const
    {
        return m_BindingTableUsedEntriesBitmap;
    }

    void SetBindingTableEntryCountAndBitmap(bool directIdx, uint32_t bti = 0)
    {
        if (bti <= MAX_BINDING_TABLE_INDEX)
        {
            if (directIdx)
            {
                m_BindingTableEntryCount = (bti <= m_pBtiLayout->GetBindingTableEntryCount()) ? (std::max(bti, m_BindingTableEntryCount)) : m_BindingTableEntryCount;
                m_BindingTableUsedEntriesBitmap |= BIT(bti / cBTEntriesPerCacheLine);
            }
            else
            {
                // Indirect addressing, set the maximum BTI.
                m_BindingTableEntryCount = m_pBtiLayout->GetBindingTableEntryCount();
                m_BindingTableUsedEntriesBitmap |= BITMASK_RANGE(0, (m_BindingTableEntryCount / cBTEntriesPerCacheLine));
            }
        }
    }

    /// Evaluate the Sampler Count field value.
    unsigned int GetSamplerCount(unsigned int samplerCount);

    static unsigned GetIMEReturnPayloadSize(llvm::GenIntrinsicInst* I);

    // When debug info is enabled, this vector stores mapping of
    // VISA index->Gen ISA offset. Currently, some APIs uses this
    // to dump out elf.
    std::vector<std::pair<unsigned int, unsigned int>> m_VISAIndexToGenISAOff;
    void addCVarsForVectorBC(llvm::BitCastInst* BCI, llvm::SmallVector<CVariable*, 8> CVars)
    {
        assert (m_VectorBCItoCVars.find(BCI) == std::end(m_VectorBCItoCVars) && 
            "a variable already exists for this vector bitcast");
        m_VectorBCItoCVars.try_emplace(BCI, CVars);
    }

    CVariable* getCVarForVectorBCI(llvm::BitCastInst* BCI, int index)
    {
        auto iter = m_VectorBCItoCVars.find(BCI);
        if (iter == m_VectorBCItoCVars.end())
        {
            return nullptr;
        }
        return (*iter).second[index];
    }

private:
    // Return DefInst's CVariable if it could be reused for UseInst, and return
    // nullptr otherwise.
    CVariable *reuseSourceVar(llvm::Instruction *UseInst,
                              llvm::Instruction *DefInst,
                              e_alignment preferredAlign);

    // Return nullptr if no source variable is reused. Otherwise return a
    // CVariable from its source operand.
    CVariable *GetSymbolFromSource(llvm::Instruction *UseInst,
                                   e_alignment preferredAlign);

protected:
    CShaderProgram* m_parent;
    CodeGenContext* m_ctx;
    WIAnalysis* m_WI;
    DeSSA* m_deSSA;
    CoalescingEngine* m_coalescingEngine;
    CodeGenPatternMatch* m_CG;
    llvm::DominatorTree* m_DT;
    const llvm::DataLayout* m_DL;
    GenXFunctionGroupAnalysis *m_FGA;
    VariableReuseAnalysis *m_VRA;

    uint m_currentBlock;
    uint m_numBlocks;
    IGC::IGCMD::MetaDataUtils *m_pMdUtils;

    llvm::BumpPtrAllocator Allocator;

    // Mapping from formal argument to its variable or from function to its
    // return variable. Per kernel mapping. Used when llvm functions are
    // compiled into vISA subroutine
    llvm::DenseMap<llvm::Value *, CVariable*> globalSymbolMapping;

    llvm::DenseMap<llvm::Value*, CVariable*> symbolMapping;
    // for phi destruction, we need another mapping from each phi to a temp
    llvm::DenseMap<llvm::PHINode*, CVariable*> phiMapping;
    // we also need another mapping from a congruent class to a temp
    // because root-value may be isolated, so it may not belong to that class
    llvm::DenseMap<llvm::Value*, CVariable*> rootMapping;
    // Yet another map: a mapping from ccTuple to its corresponding root variable.
    // Variables that participate in congruence class tuples will be defined as
    // aliases (with respective offset) to the root variable.
    llvm::DenseMap<CoalescingEngine::CCTuple*, CVariable*> ccTupleMapping;
    // Constant pool.
    llvm::DenseMap<llvm::Constant *, CVariable *> ConstantPool;

    // keep a map when we generate accurate mask for vector value
    // in order to reduce register usage
    llvm::DenseMap<llvm::Value*, uint32_t> extractMasks;

    CEncoder encoder;
    std::vector<CVariable*> setup;
    std::vector<CVariable*> patchConstantSetup;

    uint m_maxBlockId;

    CVariable* m_R0;
    CVariable* m_NULL;
    CVariable* m_TSC;
    CVariable* m_SR0;
    CVariable* m_CR0;
    CVariable* m_CE0;
    CVariable* m_DBG;
    CVariable* m_HW_TID;
    CVariable* m_SP;
    CVariable* m_SavedSP;
    CVariable* m_ARGV;
    CVariable* m_RETV;

    std::vector<USC::SConstantGatherEntry> gatherMap;
    uint m_ConstantBufferLength;
    uint m_constantBufferMask;
    uint m_constantBufferLoaded;
    int  m_cbSlot;
    uint m_statelessCBPushedSize;
    uint m_NOSBufferSize;

    /// holds max number of inputs that can be pushed for this shader unit 
    static const uint32_t m_pMaxNumOfPushedInputs;

    bool m_HasBarrier;
    SProgramOutput m_simdProgram;

    // Holds max used binding table entry index.
    uint32_t m_BindingTableEntryCount;

    // Holds binding table entries bitmap.
    uint32_t m_BindingTableUsedEntriesBitmap;

    // for each vector BCI whose uses are all extractElt with imm offset, 
    // we store the CVariables for each index
    llvm::DenseMap<llvm::Instruction*, llvm::SmallVector<CVariable*, 8>> m_VectorBCItoCVars;

};

/// This class contains the information for the different SIMD version
/// of a kernel. Each kernel in the module is associated to one CShaderProgram
class CShaderProgram
{
public:
    typedef llvm::MapVector<llvm::Function*, CShaderProgram*> KernelShaderMap;
    CShaderProgram(CodeGenContext* ctx, llvm::Function* kernel);
    ~CShaderProgram();
    CShader* GetOrCreateShader(SIMDMode simd, ShaderDispatchMode mode = ShaderDispatchMode::NOT_APPLICABLE);
    CShader* GetShader(SIMDMode simd, ShaderDispatchMode mode = ShaderDispatchMode::NOT_APPLICABLE);
    CodeGenContext* GetContext() { return m_context; }
    void FillProgram(SVertexShaderKernelProgram* pKernelProgram);
    void FillProgram(SHullShaderKernelProgram* pKernelProgram);
    void FillProgram(SDomainShaderKernelProgram* pKernelProgram);
    void FillProgram(SGeometryShaderKernelProgram* pKernelProgram);
    void FillProgram(SPixelShaderKernelProgram* pKernelProgram);
    void FillProgram(SComputeShaderKernelProgram* pKernelProgram);
    void FillProgram(SOpenCLProgramInfo* pKernelProgram);
    ShaderStats *m_shaderStats;

protected:
    CShader*& GetShaderPtr(SIMDMode simd, ShaderDispatchMode mode);
    CShader* CreateNewShader(SIMDMode simd);
    void ClearShaderPtr(SIMDMode simd);

    inline bool hasShaderOutput(CShader* shader)
    {
        return (shader && shader->ProgramOutput()->m_programSize > 0);
    }

    inline void freeShaderOutput(CShader* shader)
    {
        if (hasShaderOutput(shader))
        {
            IGC::aligned_free(shader->ProgramOutput()->m_programBin);
            shader->ProgramOutput()->m_programSize = 0;
        }
    }

    CodeGenContext* m_context;
    llvm::Function*  m_kernel;
    CShader*        m_SIMDshaders[4];
};

struct SInstContext
{
    CVariable* flag;
    e_modifier dst_mod;
    bool invertFlag;
    void init()
    {
        flag = NULL;
        dst_mod = EMOD_NONE;
        invertFlag = false;
    }
};

static const SInstContext g_InitContext = 
{
    NULL,
    EMOD_NONE,
    false,
};

void unify_opt_PreProcess( CodeGenContext* pContext );
// Forward declaration
struct PSSignature; 
void CodeGen(PixelShaderContext* ctx, CShaderProgram::KernelShaderMap &shaders, PSSignature* pSignature = nullptr);
void CodeGen(OpenCLProgramContext* ctx, CShaderProgram::KernelShaderMap &shaders);
}
