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
#include "Compiler/Optimizer/OpenCLPasses/PrivateMemory/PrivateMemoryResolution.hpp"
#include "Compiler/Optimizer/OpenCLPasses/KernelArgs.hpp"
#include "Compiler/MetaDataUtilsWrapper.h"
#include "Compiler/IGCPassSupport.h"
#include "Compiler/CISACodeGen/GenCodeGenModule.h"

#include "common/LLVMWarningsPush.hpp"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "common/LLVMWarningsPop.hpp"

using namespace llvm;
using namespace IGC;

namespace IGC {

/// \brief Analyze alloca instructions and determine the size and offset of
/// each alloca and the total amount of private memory needed by each kernel.
class ModuleAllocaInfo {
public:
  ModuleAllocaInfo(Module *M, const DataLayout *DL,
                   GenXFunctionGroupAnalysis *FGA = nullptr)
      : M(M), DL(DL), FGA(FGA) {
    analyze();
  }

  ~ModuleAllocaInfo() {
    for (auto I = InfoMap.begin(), E = InfoMap.end(); I != E; ++I)
      delete I->second;
  }

  ModuleAllocaInfo(const ModuleAllocaInfo &) = delete;
  ModuleAllocaInfo &operator=(const ModuleAllocaInfo &) = delete;

  /// \brief Return the offset of alloca instruction in private memory buffer.
  unsigned getBufferOffset(AllocaInst *AI) {
    Function *F = AI->getParent()->getParent();
    return getFuncAllocaInfo(F)->AllocaDesc[AI].first;
  }

  /// \brief Return the size of alloca instruction in private memory buffer.
  unsigned getBufferSize(AllocaInst *AI) {
    Function *F = AI->getParent()->getParent();
    return getFuncAllocaInfo(F)->AllocaDesc[AI].second;
  }

  /// \brief Return all alloca instructions of a given function.
  SmallVector<AllocaInst *, 8> &getAllocaInsts(Function *F) {
    return getFuncAllocaInfo(F)->Allocas;
  }

  /// \brief Return the total private memory size per WI of a given function.
  unsigned getTotalPrivateMemPerWI(Function *F) {
    auto FI = getFuncAllocaInfo(F);
    return FI ? FI->TotalSize : 0;
  }

private:
  /// \brief The module being analyzed.
  Module *M;

  /// \brief The DataLayout object.
  const DataLayout *DL;

  /// \brief The optional function group analysis.
  GenXFunctionGroupAnalysis *FGA;

  struct FunctionAllocaInfo {
    FunctionAllocaInfo() : TotalSize(0) {}

    void setAllocaDesc(AllocaInst *AI, unsigned Offset, unsigned Size) {
      AllocaDesc[AI] = std::make_pair(Offset, Size);
    }

    /// \brief Total amount of private memory size per kernel. All functions in
    /// a kernel will have the same size.
    unsigned TotalSize;

    /// \brief Alloca instructions for a function.
    SmallVector<AllocaInst *, 8> Allocas;

    /// \brief Alloca instruction, its offset and size in buffer.
    DenseMap<AllocaInst *, std::pair<unsigned, unsigned>> AllocaDesc;
  };

  FunctionAllocaInfo *getFuncAllocaInfo(Function *F) {
    auto Iter = InfoMap.find(F);
    if (Iter != InfoMap.end())
      return Iter->second;
    return nullptr;
  }

  FunctionAllocaInfo *getOrCreateFuncAllocaInfo(Function *F) {
    auto Iter = InfoMap.find(F);
    if (Iter != InfoMap.end())
      return Iter->second;

    auto AllocaInfo = new FunctionAllocaInfo;
    InfoMap[F] = AllocaInfo;
    return AllocaInfo;
  }

  /// \brief Analyze the module and fill function alloca info map.
  void analyze();

  /// \brief When function group analysis is available, visit group by group.
  void analyze(FunctionGroup *FG);

  /// \brief Analyze individual functions.
  void analyze(Function *F, unsigned &gOffset, unsigned &gAlignment);

  /// \brief Each function has an entry that describes its private memory
  /// usage information.
  DenseMap<Function *, FunctionAllocaInfo *> InfoMap;
};

} // namespace IGC

void ModuleAllocaInfo::analyze() {
  if (FGA && FGA->getModule()) {
    assert(FGA->getModule() == M);
    for (auto I = FGA->begin(), E = FGA->end(); I != E; ++I)
      analyze(*I);
  } else {
    for (auto &F : M->getFunctionList()) {
      if (F.empty())
        continue;

      unsigned Offset = 0;
      unsigned Alignment = 0;
      analyze(&F, Offset, Alignment);
      if (Alignment > 0)
        Offset = iSTD::Align(Offset, Alignment);
      getOrCreateFuncAllocaInfo(&F)->TotalSize = Offset;
    }
  }
}
void ModuleAllocaInfo::analyze(FunctionGroup *FG) 
{
    // Calculate the size of private-memory we need to allocate to
    // every function-sub-group. Eache sub-group is led by a kernel or
    // a stack-call function.
    // Note that the function order does affect the final total amount of
    // private memory due to possible alignment constraints.
    //
    for (auto SubGI = FG->Functions.begin(), SubGE = FG->Functions.end(); SubGI != SubGE; ++SubGI) {
        unsigned Offset = 0;
        unsigned Alignment = 0;
        for (auto I = (*SubGI)->begin(), E = (*SubGI)->end(); I != E; ++I) {
            Function *F = *I;
            if (F->empty())
                continue;
            analyze(F, Offset, Alignment);
        }

        // Use the final offset as the total size.
        if (Alignment > 0)
            Offset = iSTD::Align(Offset, Alignment);

        // All functions in this group will get the same final size.
        for (auto I = (*SubGI)->begin(), E = (*SubGI)->end(); I != E; ++I) {
            Function *F = *I;
            if (F->empty())
                continue;
            getOrCreateFuncAllocaInfo(F)->TotalSize = Offset;
        }
    }
}

void ModuleAllocaInfo::analyze(Function *F, unsigned &Offset,
                               unsigned &MaxAlignment) {
  // Create alloca info even when there is no alloca, so that each function gets
  // an info entry.
  FunctionAllocaInfo *AllocaInfo = getOrCreateFuncAllocaInfo(F);

  // Collect allocas.
  SmallVector<AllocaInst *, 8> Allocas;
  for (auto &BB : F->getBasicBlockList()) {
    for (auto &Inst : BB.getInstList()) {
      if (AllocaInst *AI = dyn_cast<AllocaInst>(&Inst)) {
        if (AI->use_empty())
          continue;
        Allocas.push_back(AI);
      }
    }
  }

  if (Allocas.empty())
    return;

  // Group by alignment and smallest first.
  auto getAlignment = [=](AllocaInst *AI) -> unsigned {
   unsigned Alignment = AI->getAlignment();
    if (Alignment == 0)
      Alignment = DL->getABITypeAlignment(AI->getAllocatedType());
    return Alignment;
  };
  std::sort(Allocas.begin(), Allocas.end(),
            [=](AllocaInst *AI1, AllocaInst *AI2) {
    return getAlignment(AI1) < getAlignment(AI2);
  });

  for (auto AI : Allocas) {
    // Align alloca offset.
    unsigned Alignment = getAlignment(AI);
    Offset = iSTD::Align(Offset, Alignment);

    // Keep track of the maximal alignment seen so far.
    if (Alignment > MaxAlignment)
      MaxAlignment = Alignment;

    // Compute alloca size.
    assert(isa<ConstantInt>(AI->getArraySize()));
    ConstantInt *SizeVal = cast<ConstantInt>(AI->getArraySize());
    unsigned CurSize = (unsigned)(SizeVal->getZExtValue() *
                                  DL->getTypeAllocSize(AI->getAllocatedType()));
    AllocaInfo->setAllocaDesc(AI, Offset, CurSize);

    // Increment the current offset for the next alloca.
    Offset += CurSize;
  }

  // Update collected allocas into the function alloca info object.
  AllocaInfo->Allocas.swap(Allocas);
}

// Register pass to igc-opt
#define PASS_FLAG "igc-private-mem-resolution"
#define PASS_DESCRIPTION "Resolves private memory allocation"
#define PASS_CFG_ONLY true
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(PrivateMemoryResolution, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
IGC_INITIALIZE_PASS_END(PrivateMemoryResolution, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char PrivateMemoryResolution::ID = 0;

PrivateMemoryResolution::PrivateMemoryResolution() : ModulePass(ID)
{
    initializePrivateMemoryResolutionPass(*PassRegistry::getPassRegistry());
}

void PrivateMemoryResolution::getAnalysisUsage(llvm::AnalysisUsage &AU) const
{
    AU.setPreservesCFG();
    AU.addRequired<MetaDataUtilsWrapper>();
    AU.addRequired<CodeGenContextWrapper>();
}

bool PrivateMemoryResolution::safeToUseScratchSpace(llvm::Module &M) const
{
   ModuleMetaData &modMD = *getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();
   CodeGenContext &Ctx = *getAnalysis<CodeGenContextWrapper>().getCodeGenContext();

   //
   // For now, all APIs that use scratch space for private memory, must use scratch
   // memory except OpenCL, which can also use non-scratch space. For debugging
   // purpose, a registry key is used for OCL to turn ocl-use-scratch on/off.
   //
   if (modMD.compOpt.OptDisable
       || !Ctx.m_DriverInfo.usesScratchSpacePrivateMemory()
       || (Ctx.type == ShaderType::OPENCL_SHADER
           && !IGC_IS_FLAG_ENABLED(EnableOCLScratchPrivateMemory)))
   {
     return false;
   }

   //
   // Do not use scratch space if module has any stack call.
   //
   if (auto *FGA = getAnalysisIfAvailable<GenXFunctionGroupAnalysis>()) {
     if (FGA->getModule() == &M) {
       for (auto &I : *FGA) {
         if (I->hasStackCall())
           return false;
       }
     }
   }

   const llvm::DataLayout *DL = &M.getDataLayout();

   for (auto &F : M) {
     if (F.isDeclaration())
       continue;

     // Check each instr of this function.
     for (auto &BB : F) {
       for (auto &I : BB) {
         if (AddrSpaceCastInst *CI = dyn_cast<AddrSpaceCastInst>(&I)) {
           // It is not safe to use scratch space as private memory if kernel does
           // AS casting to ADDRESS_SPACE_GLOBAL_OR_PRIVATE or ADDRESS_SPACE_PRIVATE.
           // See speical hack CI code generated at ProgramScopeConstantResolution 
           const ADDRESS_SPACE targetAS = (ADDRESS_SPACE)(cast<PointerType>(CI->getType()))->getAddressSpace();
           if (targetAS == ADDRESS_SPACE_GLOBAL_OR_PRIVATE || targetAS == ADDRESS_SPACE_PRIVATE) {
             return false;
           }
         }
         else if (CallInst *CI = dyn_cast<CallInst>(&I)) {
           //
           // Do not use scratch space if kernel has function call that will use private memory.
           // Since the current implementation does not pass privateBase offset to callee, and
           // callee's write to scratch space could over-write caller's private data.
           //
           // TODO: provide a proper solution once other problmes are resolved.
           //
           if (Function *callee = CI->getCalledFunction()) {
             // No need to check m_ModAllocaInfo->getTotalPrivateMemPerWI(callee), since all
             // the calls of a call graph will have the same PM size... Need to fix
             if (!callee->isDeclaration())
               return false;
           }
         }
       }
     }

     if (m_pMdUtils->findFunctionsInfoItem(&F) == m_pMdUtils->end_FunctionsInfo())
       continue;

     //
     // OCL kernel arguments with type like queue_t and struct are expressed as
     // pointer type. Since there is no explicit AS associated with those pointers,
     // e.g., %opencl.queue_t*, to have both host and device use the same pointer
     // size for those arguments, it is better to disable the use of scratch memory.
     //
     // TODO: fixed those types (they should be in global address space)
     if (Ctx.type == ShaderType::OPENCL_SHADER) {
       if (!F.getArgumentList().empty()) {
         KernelArgs kernelArgs(F, DL, m_pMdUtils);
         for (auto arg : kernelArgs) {
           const KernelArg::ArgType argTy = arg.getArgType();
           if ((argTy == KernelArg::ArgType::PTR_DEVICE_QUEUE)
             || (argTy == KernelArg::ArgType::STRUCT))
           {
             return false;
           }
         }
       }
     }

     //
     // Each thread has up to 2 MB scratch space to use. That is, each WI
     // has up to (2*1024*1024 / 8) bytes of scratch space in SIMD8 mode.
     //
     auto funcInfoMD = m_pMdUtils->getFunctionsInfoItem(&F);

     // Start with simd16, which allows the medium size of space per WI
     // (simd8: largest, simd32, smallest). In doing so, there will be
     // some space left for spilling in simd8 if spilling happens.
     int32_t simd_size = 16;
     const int32_t subGrpSize = funcInfoMD->getSubGroupSize()->getSIMD_size();
     if (subGrpSize > simd_size)
       simd_size = std::min(subGrpSize, 32);
     int32_t groupSize = IGCMD::IGCMetaDataHelper::getThreadGroupSize(*m_pMdUtils, &F);
     if (groupSize == 0)
       groupSize = IGCMD::IGCMetaDataHelper::getThreadGroupSizeHint(*m_pMdUtils, &F);
     if (groupSize > simd_size)
       simd_size = std::min(groupSize, 32);

     unsigned maxScratchSpaceBytes = Ctx.m_DriverInfo.maxPerThreadScratchSpace();
     unsigned scratchSpaceLimitPerWI = maxScratchSpaceBytes / simd_size;
     //
     // If spill happens, since the offset of scratch block rw send message
     // has only 12b, an assertion will be triggered if used scratch space
     // size >= 128 KB, here 128 KB = 2^12 * 256b.
     //
     const unsigned int totalPrivateMemPerWI = m_ModAllocaInfo->getTotalPrivateMemPerWI(&F);

     if (totalPrivateMemPerWI > scratchSpaceLimitPerWI) {
       return false;
     }
   }

   // It is safe to use scratch space for private memory.
   return true;
}

bool PrivateMemoryResolution::runOnModule(llvm::Module &M)
{
    // Get the analysis
    m_pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    const llvm::DataLayout *DL = &M.getDataLayout();
    auto *FGA = getAnalysisIfAvailable<GenXFunctionGroupAnalysis>();
    bool changed = false;

    ModuleAllocaInfo MemInfo(&M, DL, FGA);
    m_ModAllocaInfo = &MemInfo;

    ModuleMetaData &modMD = *getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();

    // This is the only place to initialize and define UseScratchSpacePrivateMemory.
    // we do not use scratch-space if any kernel uses stack-call because,
    // in order to use scratch-space, we change data-layout for the module,
    // change pointer-size of AS-private to 32-bit.
    modMD.compOpt.UseScratchSpacePrivateMemory = safeToUseScratchSpace(M);

    for (Module::iterator I = M.begin(); I != M.end(); ++I)
    {
      m_currFunction = &*I;

      if (m_currFunction->isDeclaration())
      {
        continue;
      }
      if (m_pMdUtils->findFunctionsInfoItem(m_currFunction) ==
          m_pMdUtils->end_FunctionsInfo())
      {
        continue;
      }
      bool hasStackCall = (FGA && FGA->getGroup(m_currFunction)->hasStackCall());
      // Resolve collected alloca instructions for current function
      changed |= resolveAllocaInstuctions(hasStackCall);
    }

    if (changed)
    {
      m_pMdUtils->save(M.getContext());
    }

    return changed;
}

// Sink allocas into its first dominating use if possible. Alloca instructions
// are placed in the first basic block which dominates all other blocks. During
// alloca resolution, all address computations are done in the first block. And
// the address objects are live from the starting point. E.g.
//
//  int i = x;
//  foo(i);
//  int j = y;
//  bar(j);
//
// Variables i, j do not overlap in the source. When i and j are both in
// memory (optimizations disabled), llvm IR looks like
//
// [0] alloca i
// [1] alloca j
// [2] store x into &i
// [3] load i
// [4] foo(i)
// [5] store y into &j
// [6] load j
// [7] bar(j)
// Notice that address &i and &j overlap, [0-4) and [1-7) resp. Sinking allocas
// i and j to their lifetime start alleviates this issue.
//
// [0] alloca i
// [1] store x into &i
// [2] load i
// [3] foo(i)
// [4] alloca j
// [5] store y into &j
// [6] load j
// [7] bar(j)
//
static void sinkAllocas(SmallVectorImpl<AllocaInst *> &Allocas) {
  assert(!Allocas.empty());
  DominatorTree DT;
  bool Calcluated = false;

  // For each alloca, sink it if it has a use that dominates all other uses.
  // This use is called the dominating use.
  for (auto AI : Allocas) {
    if (AI->user_empty())
      continue;

    // If an alloca is used other than in an instruction, skip it.
    bool Skip = false;
    SmallVector<Instruction *, 8> UInsts;
    for (auto U : AI->users()) {
      auto UI = dyn_cast<Instruction>(U);
      if (!UI) {
        Skip = true;
        break;
      }
      UInsts.push_back(UI);
    }

    if (Skip)
      continue;

    // Compute dominator tree lazily.
    if (!Calcluated) {
      Function *F = AI->getParent()->getParent();
      DT.recalculate(*F);
      Calcluated = true;
    }

    // Sort according to dominating relation on a poset. If the first
    // element is the only minimal one, then this is a dominating use.
    std::sort(
        UInsts.begin(), UInsts.end(),
        [&](Instruction *A, Instruction *B) { return DT.dominates(A, B); });
    Instruction *DomUse = UInsts.front();
    for (unsigned i = 1; i < UInsts.size(); ++i) {
      if (!DT.dominates(DomUse, UInsts[i])) {
        DomUse = nullptr;
        break;
      }
    }

    if (DomUse)
      AI->moveBefore(DomUse);
  }
}

bool PrivateMemoryResolution::resolveAllocaInstuctions(bool stackCall)
{
    // It is possible that there is no alloca instruction in the caller but there
    // is alloca in the callee. Save the total private memory to the metadata.
    unsigned int totalPrivateMemPerWI = m_ModAllocaInfo->getTotalPrivateMemPerWI(m_currFunction);
    m_pMdUtils->getFunctionsInfoItem(m_currFunction)->setPrivateMemoryPerWI(totalPrivateMemPerWI);

	// This change is only till the FuncMD is ported to new MD framework
    ModuleMetaData *modMD = getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();
    assert(modMD && "Invalid metadata utils wrapper");
    modMD->privateMemoryPerWI = totalPrivateMemPerWI;

    SmallVector<AllocaInst *, 8> &allocaInsts = m_ModAllocaInfo->getAllocaInsts(m_currFunction);
    if (allocaInsts.empty())
    {
        // No alloca instructions to process.
        return false;
    }

    if (modMD->compOpt.OptDisable)
    {
        sinkAllocas(allocaInsts);
    }

    // If there are N+1 private buffers, and M+1 threads,
    // the layout representing the private memory will look like this:

    // [buffer0 thread0][buffer1 thread0]...[bufferN thread0]
    // [buffer0 thread1][buffer1 thread1]...[bufferN thread1]
    // ...
    // [buffer0 threadM][buffer1 threadM]...[bufferN threadM]
    // Note that for each thread, all SIMD lanes of the same buffers are 
    // laid out sequentially to preserve locality.
    // So, in fact, [buffer0 thread0] represents
    // [buffer0 lane0][buffer0 lane1]...[buffer0 laneK]
    // where the SIMD width is K-1.

    // Each row represent total private memory per thread

    // To get buffer i of thread j we need to calculate:
    // {buffer i ptr} = privateBase + 
    //                  threadId * {total private mem per thread} +
    //                  {buffer offset} +
    //                  {per lane offset}

    // Where:
    // privateBase                      = implicit argument, points to [buffer0 thread0]
    // {total private mem per thread}   = simdSize * {total private mem per WI} 
    // {buffer offset}                  = simdSize * {buffer i offset per WI}
    // {per lane offset}                = simdLaneId * sizeof(buffer i)

    // simdSize and simdOffsetBase are calculated using intrinsics that are planted in this pass
    // and resolved in the code gen

    LLVMContext& C = m_currFunction->getContext();
    CodeGenContext &Ctx = *getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
    IntegerType *typeInt32 = Type::getInt32Ty(C);
    // Creates intrinsics that will be lowered in the CodeGen and will handle the simd lane id
    Function *simdLaneIdFunc = GenISAIntrinsic::getDeclaration(m_currFunction->getParent(), GenISAIntrinsic::GenISA_simdLaneId);
    // Creates intrinsics that will be lowered in the CodeGen and will handle the simd size
    Function *simdSizeFunc = GenISAIntrinsic::getDeclaration(m_currFunction->getParent(), GenISAIntrinsic::GenISA_simdSize);

    Instruction *pEntryPoint = &(*m_currFunction->getEntryBlock().getFirstInsertionPt());
    ImplicitArgs implicitArgs(*m_currFunction, m_pMdUtils);

    // This declaration will invoke constructor of DebugLoc class and result in an empty DebugLoc
    // instance, ie with line and scope set to 0.
    IF_DEBUG_INFO(DebugLoc emptyDebugLoc);

    if (stackCall)
    {
        // Creates intrinsics that will be lowered in the CodeGen and will handle the stack-pointer
        Function *stackAllocaFunc = GenISAIntrinsic::getDeclaration(m_currFunction->getParent(), GenISAIntrinsic::GenISA_StackAlloca);
        Instruction *simdLaneId16 = CallInst::Create(simdLaneIdFunc, VALUE_NAME("simdLaneId16"), pEntryPoint);
        Instruction *simdLaneId = ZExtInst::CreateIntegerCast(simdLaneId16, typeInt32, false, VALUE_NAME("simdLaneId"), pEntryPoint);
        Instruction *simdSize = CallInst::Create(simdSizeFunc, VALUE_NAME("simdSize"), pEntryPoint);
        for (auto pAI : allocaInsts)
        {
            assert(!pAI->use_empty() && "Should not reach here with alloca instruction that has no usage!");

            llvm::IRBuilder<> builder(pAI);
            IF_DEBUG_INFO(builder.SetCurrentDebugLocation(emptyDebugLoc));

            // SP will be adjusted to include all the alloca space, therefore offset need to be adjusted back
            int scalarBufferOffset = m_ModAllocaInfo->getBufferOffset(pAI) - totalPrivateMemPerWI;
            unsigned int bufferSize = m_ModAllocaInfo->getBufferSize(pAI);

            Value* bufferOffset = builder.CreateMul(simdSize, ConstantInt::get(typeInt32, scalarBufferOffset), VALUE_NAME(pAI->getName() + ".SIMDBufferOffset"));
            Value* perLaneOffset = builder.CreateMul(simdLaneId, ConstantInt::get(typeInt32, bufferSize), VALUE_NAME("perLaneOffset"));
            Value* totalOffset = builder.CreateAdd(bufferOffset, perLaneOffset, VALUE_NAME(pAI->getName() + ".totalOffset"));
            Value* stackAlloca = builder.CreateCall(stackAllocaFunc, totalOffset, VALUE_NAME("stackAlloca"));
            Value* privateBuffer = builder.CreatePointerCast(stackAlloca, pAI->getType(), VALUE_NAME(pAI->getName() + ".privateBuffer"));
            // Replace all uses of original alloca with the bitcast
            pAI->replaceAllUsesWith(privateBuffer);
            pAI->eraseFromParent();
        }
        return true;
    }

    // What is the size limit of this scratch memory? If we use >= 128 KB for private data, then we have
    // no space left for later spilling.
    if (modMD && modMD->compOpt.UseScratchSpacePrivateMemory) {
        // We want to use this pass to lower alloca instruction
        // to remove some redundant instruction caused by alloca. For original approach,
        // different threads use the same private base. While for this approach, each
        // thread has its own private base, so we don't have to calculate the
        // private base from threadid as we did previously.  In this case, we only need
        // PrivateMemoryUsageAnalysis pass, no need to run AddImplicitArgs pass.

        Instruction *simdLaneId16 = CallInst::Create(simdLaneIdFunc, VALUE_NAME("simdLaneId16"), pEntryPoint);
        Instruction *simdLaneId = ZExtInst::CreateIntegerCast(simdLaneId16, typeInt32, false, VALUE_NAME("simdLaneId"), pEntryPoint);
        Instruction *simdSize = CallInst::Create(simdSizeFunc, VALUE_NAME("simdSize"), pEntryPoint);

        Argument* r0Arg = implicitArgs.getArgInFunc(*m_currFunction, ImplicitArg::R0);
        ExtractElementInst* r0_5 = ExtractElementInst::Create(r0Arg, ConstantInt::get(typeInt32, 5), VALUE_NAME("r0.5"), pEntryPoint);
        BinaryOperator* privateBase = BinaryOperator::CreateAnd(r0_5, ConstantInt::get(typeInt32, 0xFFffFc00), VALUE_NAME("privateBase"), pEntryPoint);

        for (auto pAI : allocaInsts)
        {
            assert(!pAI->use_empty() && "Should not reach here with alloca instruction that has no usage!");

            llvm::IRBuilder<> builder(pAI);
            // Post upgrade to LLVM 3.5.1, it was found that inliner propagates debug info of callee
            // in to the alloca. Further, those allocas are somehow hoisted to the top of program.
            // When those allocas are lowered to below sequence, they result in prolog instructions
            // pointing to a much later line of code. This causes a single src line to now have
            // multiple VISA offset mappings and prevents debugger from setting breakpoints
            // correctly. So instead, we clear DebugLoc for the instructions generated by lowering
            // alloca and so the incorrect mapping will not be emitted.
            // Note: As per Amjad, later LLVM version has a fix for this in llvm/lib/Transforms/Utils/InlineFunction.cpp.

            IF_DEBUG_INFO(builder.SetCurrentDebugLocation(emptyDebugLoc));

            // Get buffer information from the analysis
            unsigned int scalarBufferOffset = m_ModAllocaInfo->getBufferOffset(pAI);
            
            // Transpose the memory layout so it is more effecient for cache access. 
            // Cases to be handled here need to skip memopt to avoid merging load/store.
            bool TransposeMemLayout = false;
            Type* pTypeOfAccessedObject = nullptr;

            if (Ctx.m_DriverInfo.SupportTransposeLayoutForPrivateMemory())
            {
                TransposeMemLayout = true;

                for (auto pGEP = pAI->user_begin(); 
                    (pGEP != pAI->user_end() && TransposeMemLayout); 
                    ++pGEP)
                {
                    GetElementPtrInst *GEPinst = dyn_cast<GetElementPtrInst>(*pGEP);
                    if (GEPinst)
                    {
                        if (!llvm::dyn_cast<llvm::StoreInst>(*(GEPinst->user_begin())) &&
                            !llvm::dyn_cast<llvm::LoadInst>(*(GEPinst->user_begin())))
                        {
                            TransposeMemLayout = false;
                        }

                        if (pTypeOfAccessedObject == nullptr)
                        {
                            // This is the first GEP - remember the type of accessed object.
                            pTypeOfAccessedObject = GEPinst->getType()->getPointerElementType();
                        }
                        else if (pTypeOfAccessedObject != GEPinst->getType()->getPointerElementType())
                        {
                            // Memory layout transposition is only possible when
                            // all accessed objects are of the same type.
                            TransposeMemLayout = false;
                        }
                    }
                    else
                    {
                        TransposeMemLayout = false;
                    }
                }

                if (TransposeMemLayout)
                {
                    llvm::Type* pBaseType = pAI->getType()->getPointerElementType();
                    while (pBaseType->isArrayTy())
                    {
                        pBaseType = pBaseType->getArrayElementType();
                    }
                    if (pBaseType->isStructTy())
                    {
                        // This is a structure, do not transpose!
                        TransposeMemLayout = false;
                    }
                }
            }
            unsigned int bufferSize = 0;
            if (TransposeMemLayout)
            {
                assert(pTypeOfAccessedObject != nullptr);

                // The alignment padding should be considered and we cannot use
                // llvm type's default width as this buffer size. E.g. OGL sets
                // i16's alignment to 4.
                auto DL = &m_currFunction->getParent()->getDataLayout();
                bufferSize = (unsigned)DL->getTypeAllocSize(pTypeOfAccessedObject);

#if defined(_DEBUG)
                {
                    // Debug code to verify that the size of transposed memory 
                    // fits into the allocated scratch region.
                    Type* pTmpType = pAI->getType()->getPointerElementType();
                    uint64_t tmpAllocaSize = bufferSize;
                    while (pTmpType != pTypeOfAccessedObject)
                    {
                        if (pTmpType->isArrayTy())
                        {
                            tmpAllocaSize *= pTmpType->getArrayNumElements();
                        }
                        else if (pTmpType->isVectorTy())
                        {
                            tmpAllocaSize *= pTmpType->getVectorNumElements();
                        }
                        else
                        {
                            assert(!"Unsupported type for memory transposition.");
                            break;
                        }
                        pTmpType = pTmpType->getSequentialElementType();
                    }
                    assert(tmpAllocaSize <= m_ModAllocaInfo->getBufferSize(pAI));   
                }
#endif
            }
            else
            {
                bufferSize = m_ModAllocaInfo->getBufferSize(pAI);
            }

            Value* bufferOffset = builder.CreateMul(simdSize, ConstantInt::get(typeInt32, scalarBufferOffset), VALUE_NAME(pAI->getName() + ".SIMDBufferOffset"));
            Value* perLaneOffset = builder.CreateMul(simdLaneId, ConstantInt::get(typeInt32, bufferSize), VALUE_NAME("perLaneOffset"));
            Value* totalOffset = builder.CreateAdd(bufferOffset, perLaneOffset, VALUE_NAME(pAI->getName() + ".totalOffset"));
            Value* threadOffset = builder.CreateAdd(privateBase, totalOffset, VALUE_NAME(pAI->getName() + ".threadOffset"));
            Value* privateBufferPTR = builder.CreateIntToPtr(threadOffset, Type::getInt8Ty(C)->getPointerTo(ADDRESS_SPACE_PRIVATE), VALUE_NAME(pAI->getName() + ".privateBufferPTR"));
            Value* privateBuffer = builder.CreatePointerCast(privateBufferPTR, pAI->getType(), VALUE_NAME(pAI->getName() + ".privateBuffer"));

            if (TransposeMemLayout)
            {
                for (auto pGEP = pAI->user_begin(); pGEP != pAI->user_end(); pGEP++)
                {
                    GetElementPtrInst *GEPinst = dyn_cast<GetElementPtrInst>(*pGEP);
                    if (GEPinst)
                    {
                        if (llvm::dyn_cast<llvm::StoreInst>(*(GEPinst->user_begin())) ||
                            llvm::dyn_cast<llvm::LoadInst>(*(GEPinst->user_begin())))
                        {   
                            builder.SetInsertPoint(GEPinst);
                            for (uint i = 1; i < GEPinst->getNumOperands(); ++i)
                            {
                                llvm::Value* gepOp = GEPinst->getOperand(i);
                                llvm::Value* width = builder.CreateZExtOrTrunc(simdSize, gepOp->getType());
                                
                                Value* newIndex = builder.CreateMul(width, gepOp, "");
                                GEPinst->setOperand(i, newIndex);
                            }
                        }
                    }
                    else
                    {
                        assert(0);
                    }
                }
            }

            // Replace all uses of original alloca with the bitcast
            pAI->replaceAllUsesWith(privateBuffer);
            pAI->eraseFromParent();
        }

        return true;
    }


    // Find the implicit argument representing r0 and the private memory base.
    Argument* r0Arg = implicitArgs.getArgInFunc(*m_currFunction, ImplicitArg::R0);
    Argument* privateMemArg = implicitArgs.getArgInFunc(*m_currFunction, ImplicitArg::PRIVATE_BASE);

    // Resolve the call

    // Receives:
    // %privateMem = alloca ...

    // Create a GEP to get to the right offset from the private memory base implicit arg:

    // %simdLaneId16                = call i16 @llvm.gen.simdLaneId()
    // %simdLaneId                  = zext i16 simdLaneId16 to i32
    // %simdSize                    = call i32 @llvm.gen.simdSize()
    // %totalPrivateMemPerThread    = mul i32 %simdSize, <totalPrivateMemPerWI>

    // %r0.5                        = extractelement <8 x i32> %r0, i32 5
    // %threadId                    = and i32 %r0.5, 0x1FF|0x3FF   (Thread ID is in the lower 9 bits or 10 bit(KBL & CNL+) of r0.5)
    // %perThreadOffset             = mul i32 %threadId, %totalPrivateMemPerThread

    ConstantInt *totalPrivateMemPerWIValue = ConstantInt::get(typeInt32, totalPrivateMemPerWI);

    Instruction *simdLaneId16 = CallInst::Create(simdLaneIdFunc, VALUE_NAME("simdLaneId16"), pEntryPoint);
    Instruction *simdLaneId = ZExtInst::CreateIntegerCast(simdLaneId16, typeInt32, false, VALUE_NAME("simdLaneId"), pEntryPoint);
    Instruction *simdSize = CallInst::Create(simdSizeFunc, VALUE_NAME("simdSize"), pEntryPoint);
    BinaryOperator* totalPrivateMemPerThread = BinaryOperator::CreateMul(simdSize, totalPrivateMemPerWIValue, VALUE_NAME("totalPrivateMemPerThread"), pEntryPoint);
    ExtractElementInst* r0_5 = ExtractElementInst::Create(r0Arg, ConstantInt::get(typeInt32, 5), VALUE_NAME("r0.5"), pEntryPoint);
    ConstantInt *FFTIDMask = ConstantInt::get(typeInt32, Ctx.platform.getFFTIDBitMask());
    BinaryOperator* threadId = BinaryOperator::CreateAnd(r0_5, FFTIDMask, VALUE_NAME("threadId"), pEntryPoint);
    Instruction *perThreadOffset = BinaryOperator::CreateMul(threadId, totalPrivateMemPerThread, VALUE_NAME("perThreadOffset"), pEntryPoint);

    for (auto pAI : allocaInsts)
    {
        assert(!pAI->use_empty() && "Should not reach here with alloca instruction that has no usage!");

        // %bufferOffset                = mul i32 %simdSize, <scalarBufferOffset>
        // %bufferOffsetForThread       = add i32 %perThreadOffset, %bufferOffset
        // %perLaneOffset               = mul i32 %simdLaneId, <bufferSize>
        // %totalOffset                 = add i32 %bufferOffsetForThread, %perLaneOffset
        // %privateBufferGEP            = getelementptr i8* %privateBase, i32 %totalOffset
        // %privateBuffer               = bitcast i8* %offsettmp1 to <buffer type>

        llvm::IRBuilder<> builder(pAI);
        IF_DEBUG_INFO(builder.SetCurrentDebugLocation(emptyDebugLoc));

        // Get buffer information from the analysis
        unsigned int scalarBufferOffset = m_ModAllocaInfo->getBufferOffset(pAI);
        unsigned int bufferSize = m_ModAllocaInfo->getBufferSize(pAI);

        Value* bufferOffset = builder.CreateMul(simdSize, ConstantInt::get(typeInt32, scalarBufferOffset), VALUE_NAME(pAI->getName() + ".SIMDBufferOffset"));
        Value* bufferOffsetForThread = builder.CreateAdd(perThreadOffset, bufferOffset, VALUE_NAME(pAI->getName() + ".bufferOffsetForThread"));
        Value* perLaneOffset = builder.CreateMul(simdLaneId, ConstantInt::get(typeInt32, bufferSize), VALUE_NAME("perLaneOffset"));
        Value* totalOffset = builder.CreateAdd(bufferOffsetForThread, perLaneOffset, VALUE_NAME(pAI->getName() + ".totalOffset"));
        Value* privateBufferGEP = builder.CreateGEP(privateMemArg, totalOffset, VALUE_NAME(pAI->getName() + ".privateBufferGEP"));
        Value* privateBuffer = builder.CreatePointerCast(privateBufferGEP, pAI->getType(), VALUE_NAME(pAI->getName() + ".privateBuffer"));

        // Replace all uses of original alloca with the bitcast
        pAI->replaceAllUsesWith(privateBuffer);
        pAI->eraseFromParent();
    }

    return true;
}
