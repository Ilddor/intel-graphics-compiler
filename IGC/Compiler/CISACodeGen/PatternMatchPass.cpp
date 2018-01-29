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

#include "Compiler/CISACodeGen/PatternMatchPass.hpp"
#include "Compiler/CISACodeGen/EmitVISAPass.hpp"
#include "Compiler/CISACodeGen/DeSSA.hpp"
#include "Compiler/MetaDataApi/IGCMetaDataHelper.h"
#include "Compiler/MetaDataApi/IGCMetaDataDefs.h"
#include "common/igc_regkeys.hpp"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/PatternMatch.h>
#include "common/LLVMWarningsPop.hpp"
#include "GenISAIntrinsics/GenIntrinsicInst.h"
#include "Compiler/IGCPassSupport.h"
#include "Compiler/InitializePasses.h"

using namespace llvm;
using namespace IGC;
using namespace IGC::IGCMD;

char CodeGenPatternMatch::ID = 0;
#define PASS_FLAG "CodeGenPatternMatch"
#define PASS_DESCRIPTION "Does pattern matching"
#define PASS_CFG_ONLY true
#define PASS_ANALYSIS true
IGC_INITIALIZE_PASS_BEGIN( CodeGenPatternMatch, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS )
IGC_INITIALIZE_PASS_DEPENDENCY( WIAnalysis )
IGC_INITIALIZE_PASS_DEPENDENCY( LiveVarsAnalysis )
IGC_INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
IGC_INITIALIZE_PASS_DEPENDENCY( DominatorTreeWrapperPass )
IGC_INITIALIZE_PASS_DEPENDENCY( MetaDataUtilsWrapper )
IGC_INITIALIZE_PASS_DEPENDENCY(PositionDepAnalysis)
IGC_INITIALIZE_PASS_END(CodeGenPatternMatch, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

namespace IGC
{

CodeGenPatternMatch::CodeGenPatternMatch( ) : FunctionPass( ID ),
m_rootIsSubspanUse( false ),
m_blocks( nullptr ),
m_numBlocks( 0 ),
m_root( nullptr ),
m_currentPattern( nullptr ),
m_DL( 0 ),
m_WI( nullptr ),
m_LivenessInfo(nullptr),
m_Platform(),
m_AllowContractions( true ),
m_NeedVMask( false ),
m_samplertoRenderTargetEnable( false ),
m_ctx( nullptr ),
DT( nullptr ),
LI( nullptr )
{
    initializeCodeGenPatternMatchPass( *PassRegistry::getPassRegistry() );
}

CodeGenPatternMatch::~CodeGenPatternMatch( )
{
    delete [] m_blocks;
}

void CodeGenPatternMatch::CodeGenNode( llvm::DomTreeNode* node )
{
    // Process blocks by processing the dominance tree depth first
    for(uint i=0;i<node->getNumChildren();i++)
    {
        CodeGenNode(node->getChildren()[i]);
    }
    llvm::BasicBlock* bb = node->getBlock();
    CodeGenBlock(bb);
} 


bool CodeGenPatternMatch::runOnFunction(llvm::Function &F)
{
    m_blockMap.clear();
    ConstantPlacement.clear();
    PairOutputMap.clear();
    
    delete[] m_blocks;
    m_blocks = nullptr;
    
    m_ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();

    MetaDataUtils *pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    ModuleMetaData *modMD = getAnalysis<MetaDataUtilsWrapper>().getModuleMetaData();
    if (pMdUtils->findFunctionsInfoItem(&F) == pMdUtils->end_FunctionsInfo())
    {
        return false;
    }

    m_AllowContractions = true;
    if(m_ctx->m_DriverInfo.NeedCheckContractionAllowed())
    {
        m_AllowContractions = 
            modMD->compOpt.FastRelaxedMath ||
            modMD->compOpt.MadEnable;
    }
    m_Platform = m_ctx->platform;

    DT = &getAnalysis<llvm::DominatorTreeWrapperPass>().getDomTree();
    LI = &getAnalysis<llvm::LoopInfoWrapperPass>().getLoopInfo();
    m_DL = &F.getParent()->getDataLayout();
    m_WI = &getAnalysis<WIAnalysis>();
    m_PosDep = &getAnalysis<PositionDepAnalysis>();
    // pattern match will update liveness held by LiveVar, which needs
    // WIAnalysis result for uniform variable
    m_LivenessInfo = &getAnalysis<LiveVarsAnalysis>().getLiveVars();
    CreateBasicBlocks(&F);
    CodeGenNode(DT->getRootNode());
    return false;
}

inline bool HasSideEffect(llvm::Instruction& inst)
{
    if(inst.mayWriteToMemory() || llvm::isa<llvm::TerminatorInst>(&inst))
    {
        return true;
    }
    return false;
}


inline bool HasPhiUse(llvm::Value& inst)
{
    for (auto UI = inst.user_begin(), E = inst.user_end(); UI != E; ++UI) 
    {
        llvm::User *U = *UI;
        if( llvm::isa<llvm::PHINode>(U))
        {
            return true;
        }
    }
    return false;
}

inline bool IsDbgInst(llvm::Instruction& inst)
{
    if(llvm::isa<llvm::DbgInfoIntrinsic>(&inst))
    {
        return true;
    }
    return false;
}

bool CodeGenPatternMatch::NeedInstruction(llvm::Instruction& I)
{
    if(HasPhiUse(I) || HasSideEffect(I) || IsDbgInst(I) ||
       (m_usedInstructions.find(&I) != m_usedInstructions.end()))
    {
        return true;
    }
    return false;
}

void CodeGenPatternMatch::AddToConstantPool(llvm::BasicBlock *UseBlock,
                                            llvm::Value *Val) {
    Constant *C = dyn_cast_or_null<Constant>(Val);
    if (!C)
        return;

    BasicBlock *LCA = UseBlock;
    // Determine where we put the constant initialization.
    // Choose loop pre-header as LICM.
    // XXX: Further investigation/tuning is needed to see whether
    // we need to hoist constant initialization out of the
    // top-level loop within a nested loop. So far, we only hoist
    // one level up.
    if (Loop *L = LI->getLoopFor(LCA)) {
        if (BasicBlock *Preheader = L->getLoopPreheader())
            LCA = Preheader;
    }
    // Find the common dominator as CSE.
    if (BasicBlock *BB = ConstantPlacement.lookup(C))
        LCA = DT->findNearestCommonDominator(LCA, BB);
    assert(LCA && "LCA always exists for reachable BBs within a function!");
    ConstantPlacement[C] = LCA;
}

void CodeGenPatternMatch::CodeGenBlock(llvm::BasicBlock* bb)
{
    llvm::BasicBlock::InstListType &instructionList = bb->getInstList();
    llvm::BasicBlock::InstListType::reverse_iterator I,E;
    auto it = m_blockMap.find(bb);
    assert(it!=m_blockMap.end());
    SBasicBlock* block = it->second;

    // loop through instructions bottom up
    for (I = instructionList.rbegin(), E = instructionList.rend(); I != E; ++I)
    {
        llvm::Instruction& inst = (*I);

        if(NeedInstruction(inst))
        {
            SetPatternRoot(inst);
            Pattern* pattern = Match(inst);
            if(pattern)
            {
                block->m_dags.push_back(SDAG(pattern, m_root));
            }
        }
    }
}

void CodeGenPatternMatch::CreateBasicBlocks(llvm::Function * pLLVMFunc)
{
    m_numBlocks = pLLVMFunc->size();
    m_blocks = new SBasicBlock[m_numBlocks];
    uint i = 0;
    for ( BasicBlock& bb : *pLLVMFunc )
    {
        m_blocks[i].id = i;
        m_blocks[i].bb = &bb;
        m_blockMap.insert(std::pair<llvm::BasicBlock*,SBasicBlock*>(&bb,&m_blocks[i]));
        i++;
    }
}
Pattern* CodeGenPatternMatch::Match(llvm::Instruction& inst)
{
    m_currentPattern = nullptr;
    visit(inst);
    return m_currentPattern;
}

void CodeGenPatternMatch::SetPatternRoot(llvm::Instruction& inst)
{
    m_root = &inst;
    m_rootIsSubspanUse = IsSubspanUse(m_root);
}

template<typename Op_t, typename ConstTy>
struct ClampWithConstants_match {
    typedef ConstTy *ConstPtrTy;

    Op_t Op;
    ConstPtrTy &CMin, &CMax;

    ClampWithConstants_match(const Op_t &OpMatch,
                             ConstPtrTy &Min, ConstPtrTy &Max)
        : Op(OpMatch), CMin(Min), CMax(Max) {}

    template<typename OpTy>
    bool match(OpTy *V) {
        GenIntrinsicInst *GII = dyn_cast<GenIntrinsicInst>(V);
        if (!GII)
            return false;

        GenISAIntrinsic::ID GIID = GII->getIntrinsicID();
        if (GIID != GenISAIntrinsic::GenISA_max &&
            GIID != GenISAIntrinsic::GenISA_min)
            return false;

        Value *X = GII->getOperand(0);
        Value *C = GII->getOperand(1);
        if (isa<ConstTy>(X))
            std::swap(X, C);

        ConstPtrTy C0 = dyn_cast<ConstTy>(C);
        if (!C0)
            return false;

        GenIntrinsicInst *GII2 = dyn_cast<GenIntrinsicInst>(X);
        if (!GII2)
            return false;

        GenISAIntrinsic::ID GIID2 = GII2->getIntrinsicID();
        if (!(GIID == GenISAIntrinsic::GenISA_min &&
              GIID2 == GenISAIntrinsic::GenISA_max) &&
            !(GIID == GenISAIntrinsic::GenISA_max &&
              GIID2 == GenISAIntrinsic::GenISA_min))
            return false;

        X = GII2->getOperand(0);
        C = GII2->getOperand(1);
        if (isa<ConstTy>(X))
            std::swap(X, C);

        ConstPtrTy C1 = dyn_cast<ConstTy>(C);
        if (!C1)
            return false;

        if (!Op.match(X))
            return false;

        CMin = (GIID2 == GenISAIntrinsic::GenISA_min) ? C0 : C1;
        CMax = (GIID2 == GenISAIntrinsic::GenISA_min) ? C1 : C0;
        return true;
    }
};

template<typename OpTy, typename ConstTy>
inline ClampWithConstants_match<OpTy, ConstTy>
m_ClampWithConstants(const OpTy &Op, ConstTy *&Min, ConstTy *&Max) {
    return ClampWithConstants_match<OpTy, ConstTy>(Op, Min, Max);
}

template<typename Op_t>
struct IsNaN_match {
  Op_t Op;

  IsNaN_match(const Op_t &OpMatch) : Op(OpMatch) {}

  template<typename OpTy>
  bool match(OpTy *V) {
    using namespace llvm::PatternMatch;

    FCmpInst *FCI = dyn_cast<FCmpInst>(V);
    if (!FCI)
      return false;

    switch (FCI->getPredicate()) {
    case FCmpInst::FCMP_UNE:
      return FCI->getOperand(0) == FCI->getOperand(1) &&
             Op.match(FCI->getOperand(0));
    case FCmpInst::FCMP_UNO:
      return m_Zero().match(FCI->getOperand(1)) &&
             Op.match(FCI->getOperand(0));
    default:
      break;
    }

    return false;
  }
};

template<typename OpTy>
inline IsNaN_match<OpTy> m_IsNaN(const OpTy &Op) {
  return IsNaN_match<OpTy>(Op);
}

std::tuple<Value *, unsigned, VISA_Type>
CodeGenPatternMatch::isFPToIntegerSatWithExactConstant(llvm::CastInst *I) {
    using namespace llvm::PatternMatch; // Scoped using declaration.

    unsigned Opcode = I->getOpcode();
    assert(Opcode == Instruction::FPToSI || Opcode == Instruction::FPToUI);

    unsigned BitWidth = I->getDestTy()->getIntegerBitWidth();
    APFloat FMin(I->getSrcTy()->getFltSemantics());
    APFloat FMax(I->getSrcTy()->getFltSemantics());
    if (Opcode == Instruction::FPToSI) {
        if (FMax.convertFromAPInt(APInt::getSignedMaxValue(BitWidth), true,
                                  APFloat::rmNearestTiesToEven) != APFloat::opOK)
            return std::make_tuple(nullptr, 0, ISA_TYPE_F);
        if (FMin.convertFromAPInt(APInt::getSignedMinValue(BitWidth), true,
                                  APFloat::rmNearestTiesToEven) != APFloat::opOK)
            return std::make_tuple(nullptr, 0, ISA_TYPE_F);
    } else {
        if (FMax.convertFromAPInt(APInt::getMaxValue(BitWidth), false,
                                  APFloat::rmNearestTiesToEven) != APFloat::opOK)
            return std::make_tuple(nullptr, 0, ISA_TYPE_F);
        if (FMin.convertFromAPInt(APInt::getMinValue(BitWidth), false,
                                  APFloat::rmNearestTiesToEven) != APFloat::opOK)
            return std::make_tuple(nullptr, 0, ISA_TYPE_F);
    }

    llvm::ConstantFP *CMin, *CMax;
    llvm::Value *X = nullptr;

    if (!match(I->getOperand(0), m_ClampWithConstants(m_Value(X), CMin, CMax)))
        return std::make_tuple(nullptr, 0, ISA_TYPE_F);

    if (!CMin->isExactlyValue(FMin) || !CMax->isExactlyValue(FMax))
        return std::make_tuple(nullptr, 0, ISA_TYPE_F);

    return std::make_tuple(X, Opcode, GetType(I->getType(), m_ctx));
}

// The following pattern matching is targeted to the conversion from FP values
// to INTEGER values with saturation where the MAX and/or MIN INTEGER values
// cannot be represented in FP values exactly. E.g., UINT_MAX (2**32-1) in
// 'unsigned' cannot be represented in 'float', where only 23 significant bits
// are available but UINT_MAX needs 32 significant bits. We cannot simply
// express that conversion with saturation as
//
//  o := fptoui(clamp(x, float(UINT_MIN), float(UINT_MAX));
//
// as, in LLVM, fptoui is undefined when the 'unsigned' source cannot fit in
// 'float', where clamp(x, MIN, MAX) is defined as max(min(x, MAX), MIN),
//
// Hence, OCL use the following sequence (over-simplified by excluding the NaN
// case.)
//
//  o := select(fptoui(x), UINT_MIN, x < float(UINT_MIN));
//  o := select(o,         UINT_MAX, x > float(UINT_MAX));
//
// (We SHOULD use 'o := select(o, UINTMAX, x >= float(UINT_MAX))' as
// 'float(UINT_MAX)' will be rounded to UINT_MAX+1, i.e. 2 ** 32, and the next
// smaller value than float(UINT_MAX) in 'float' is (2 ** 24 - 1) << 8. For
// 'int', that's also true for INT_MIN.)

std::tuple<Value *, unsigned, VISA_Type>
CodeGenPatternMatch::isFPToSignedIntSatWithInexactConstant(llvm::SelectInst *SI) {
    using namespace llvm::PatternMatch; // Scoped using declaration.

    // TODO
    return std::make_tuple(nullptr, 0, ISA_TYPE_F);
}

std::tuple<Value *, unsigned, VISA_Type>
CodeGenPatternMatch::isFPToUnsignedIntSatWithInexactConstant( llvm::SelectInst *SI )
{
    using namespace llvm::PatternMatch; // Scoped using declaration.

    Constant *C0 = dyn_cast<Constant>(SI->getTrueValue());
    if (!C0)
        return std::make_tuple(nullptr, 0, ISA_TYPE_F);
    if (!isa<ConstantFP>(C0) && !isa<ConstantInt>(C0))
        return std::make_tuple(nullptr, 0, ISA_TYPE_F);
    Value *Cond = SI->getCondition();

    SelectInst *SI2 = dyn_cast<SelectInst>(SI->getFalseValue());
    if (!SI2)
        return std::make_tuple(nullptr, 0, ISA_TYPE_F);
    Constant *C1 = dyn_cast<Constant>(SI2->getTrueValue());
    if (!C1)
        return std::make_tuple(nullptr, 0, ISA_TYPE_F);
    if (!isa<ConstantFP>(C1) && !isa<ConstantInt>(C1))
        return std::make_tuple(nullptr, 0, ISA_TYPE_F);
    Value *Cond2 = SI2->getCondition();

    Value *X = SI2->getFalseValue();
    Type *Ty = X->getType();
    if (Ty->isFloatTy()) {
        BitCastInst *BC = dyn_cast<BitCastInst>(X);
        if (!BC)
            return std::make_tuple(nullptr, 0, ISA_TYPE_F);
        X = BC->getOperand(0);
        Ty = X->getType();
        C1 = ConstantExpr::getBitCast(C1, Ty);
        C0 = ConstantExpr::getBitCast(C0, Ty);
    }
    IntegerType *ITy = dyn_cast<IntegerType>(Ty);
    if (!ITy)
        return std::make_tuple(nullptr, 0, ISA_TYPE_F);
    unsigned BitWidth = ITy->getBitWidth();
    FPToUIInst *CI = dyn_cast<FPToUIInst>(X);
    if (!CI)
        return std::make_tuple(nullptr, 0, ISA_TYPE_F);
    Ty = CI->getSrcTy();
    if (!(Ty->isFloatTy() && BitWidth == 32) &&
        !(Ty->isDoubleTy() && BitWidth == 64))
        return std::make_tuple(nullptr, 0, ISA_TYPE_F);
    X = CI->getOperand(0);

    ConstantInt *CMin = dyn_cast<ConstantInt>(C0);
    ConstantInt *CMax = dyn_cast<ConstantInt>(C1);
    if (!CMax || !CMin || !CMax->isMaxValue(false) || !CMin->isMinValue(false))
        return std::make_tuple(nullptr, 0, ISA_TYPE_F);

    Constant* FMin = ConstantExpr::getUIToFP(CMin, Ty);
    Constant* FMax = ConstantExpr::getUIToFP(CMax, Ty);

    FCmpInst::Predicate Pred = FCmpInst::FCMP_FALSE;
    if (!match(Cond2, m_FCmp(Pred, m_Specific(X), m_Specific(FMax))))
        return std::make_tuple(nullptr, 0, ISA_TYPE_F);
    if (Pred != FCmpInst::FCMP_OGT) // FIXME: We should use OGE instead of OGT.
        return std::make_tuple(nullptr, 0, ISA_TYPE_F);

    FCmpInst::Predicate Pred2 = FCmpInst::FCMP_FALSE;
    if (!match(Cond,
               m_Or(m_FCmp(Pred, m_Specific(X), m_Specific(FMin)),
                    m_FCmp(Pred2, m_Specific(X), m_Specific(X))))) {
        if (!match(Cond,
                   m_Or(m_FCmp(Pred, m_Specific(X), m_Specific(FMin)),
                        m_Zero()))) {
            return std::make_tuple(nullptr, 0, ISA_TYPE_F);
        }
        // Special case where the staturatured result is bitcasted into float
        // again (due to typedwrite only accepts `float`. So the isNaN(X) is
        // reduced to `false`.
        Pred2 = FCmpInst::FCMP_UNE;
    }
    if (Pred != FCmpInst::FCMP_OLT || Pred2 != FCmpInst::FCMP_UNE)
        return std::make_tuple(nullptr, 0, ISA_TYPE_F);

    VISA_Type type = GetType(CI->getType(), m_ctx);

    // Fold extra clamp.
    Value *X2 = nullptr;
    ConstantFP *CMin2, *CMax2;
    if (match(X, m_ClampWithConstants(m_Value(X2), CMin2, CMax2))) {
        if (CMin2 == FMin) {
            if (CMax2->isExactlyValue(255.0)) {
                X = X2;
                type = ISA_TYPE_B;
            } else if (CMax2->isExactlyValue(65535.0)) {
                X = X2;
                type = ISA_TYPE_W;
            }
        }
    }

    return std::make_tuple(X, Instruction::FPToUI, type);
}

bool CodeGenPatternMatch::MatchFPToIntegerWithSaturation(llvm::Instruction &I) {
    Value *X;
    unsigned Opcode;
    VISA_Type type;

    if (CastInst *CI = dyn_cast<CastInst>(&I)) {
        std::tie(X, Opcode, type) = isFPToIntegerSatWithExactConstant(CI);
        if (!X)
            return false;
    } else if (SelectInst *SI = dyn_cast<SelectInst>(&I)) {
        std::tie(X, Opcode, type) = isFPToSignedIntSatWithInexactConstant(SI);
        if (!X) {
            std::tie(X, Opcode, type) = isFPToUnsignedIntSatWithInexactConstant(SI);
            if (!X)
                return false;
        }
    } else {
        return false;
    }

    // Match!
    assert(Opcode == Instruction::FPToSI || Opcode == Instruction::FPToUI);

    struct FPToIntegerWithSaturationPattern : public Pattern {
        bool isUnsigned, needBitCast;
        VISA_Type type;
        SSource src;
        virtual void Emit(EmitPass *pass, const DstModifier &dstMod) {
            pass->EmitFPToIntWithSat(isUnsigned, needBitCast, type, src, dstMod);
        }
    };

    bool isUnsigned = (Opcode == Instruction::FPToUI);
    FPToIntegerWithSaturationPattern *pat
        = new (m_allocator) FPToIntegerWithSaturationPattern();
    pat->isUnsigned = isUnsigned;
    pat->needBitCast = !I.getType()->isIntegerTy();
    pat->type = type;
    pat->src = GetSource(X, !isUnsigned, false);
    AddPattern(pat);

    return true;
}

std::tuple<Value *, bool, bool>
CodeGenPatternMatch::isIntegerSatTrunc(llvm::SelectInst *SI) {
    using namespace llvm::PatternMatch; // Scoped using declaration.

    ICmpInst *Cmp = dyn_cast<ICmpInst>(SI->getOperand(0));
    if (!Cmp)
        return std::make_tuple(nullptr, false, false);

    ICmpInst::Predicate Pred = Cmp->getPredicate();
    if (Pred != ICmpInst::ICMP_SGT && Pred != ICmpInst::ICMP_UGT)
        return std::make_tuple(nullptr, false, false);

    ConstantInt *CI = dyn_cast<ConstantInt>(Cmp->getOperand(1));
    if (!CI)
        return std::make_tuple(nullptr, false, false);

    // Truncate into unsigned integer by default.
    bool isSignedDst = false;
    unsigned DstBitWidth = SI->getType()->getIntegerBitWidth();
    unsigned SrcBitWidth = Cmp->getOperand(0)->getType()->getIntegerBitWidth();
    APInt UMax = APInt::getMaxValue(DstBitWidth);
    APInt UMin = APInt::getMinValue(DstBitWidth);
    APInt SMax = APInt::getSignedMaxValue(DstBitWidth);
    APInt SMin = APInt::getSignedMinValue(DstBitWidth);
    if (SrcBitWidth > DstBitWidth) {
        UMax = UMax.zext(SrcBitWidth);
        UMin = UMin.zext(SrcBitWidth);
        SMax = SMax.sext(SrcBitWidth);
        SMin = SMin.sext(SrcBitWidth);
    }
    else
    {
        // SrcBitwidth should be always wider than DstBitwidth,
        // since src is a source of a trunc instruction, and dst
        // have the same width as its destination.
        return std::make_tuple(nullptr, false, false);
    }  

    if (CI->getValue() != UMax && CI->getValue() != SMax)
        return std::make_tuple(nullptr, false, false);
    if (CI->getValue() == SMax) // Truncate into signed integer.
        isSignedDst = true;

    APInt MinValue = isSignedDst ? SMin : UMin;
    CI = dyn_cast<ConstantInt>(SI->getOperand(1));
    if (!CI || !CI->isMaxValue(isSignedDst))
        return std::make_tuple(nullptr, false, false);

    TruncInst *TI = dyn_cast<TruncInst>(SI->getOperand(2));
    if (!TI)
        return std::make_tuple(nullptr, false, false);

    Value *Val = TI->getOperand(0);
    if (Val != Cmp->getOperand(0))
        return std::make_tuple(nullptr, false, false);

    // Truncate from unsigned integer.
    if (Pred == ICmpInst::ICMP_UGT)
        return std::make_tuple(Val, isSignedDst, false);

    // Truncate from signed integer. Need to check further for lower bound.
    Value *LHS, *RHS;
    if (!match(Val, m_SMax(m_Value(LHS), m_Value(RHS))))
        return std::make_tuple(nullptr, false, false);

    if (isa<ConstantInt>(LHS))
        std::swap(LHS, RHS);

    CI = dyn_cast<ConstantInt>(RHS);
    if (!CI || CI->getValue() != MinValue)
        return std::make_tuple(nullptr, false, false);

    return std::make_tuple(LHS, isSignedDst, true);
}

bool CodeGenPatternMatch::MatchIntegerSatModifier(llvm::SelectInst &I) {
    // Only match BYTE or WORD.
    if (!I.getType()->isIntegerTy(8) && !I.getType()->isIntegerTy(16))
        return false;
    Value *Src;
    bool isSignedDst, isSignedSrc;
    std::tie(Src, isSignedDst, isSignedSrc) = isIntegerSatTrunc(&I);
    if (!Src)
        return false;

    struct IntegerSatTruncPattern : public Pattern {
        SSource src;
        bool isSignedDst;
        bool isSignedSrc;
        virtual void Emit(EmitPass *pass, const DstModifier &dstMod) {
            pass->EmitIntegerTruncWithSat(isSignedDst, isSignedSrc, src, dstMod);
        }
    };

    IntegerSatTruncPattern *pat = new (m_allocator) IntegerSatTruncPattern();
    pat->src = GetSource(Src, isSignedSrc, false);
    pat->isSignedDst = isSignedDst;
    pat->isSignedSrc = isSignedSrc;
    AddPattern(pat);

    return true;
}

void CodeGenPatternMatch::visitFPToSIInst(llvm::FPToSIInst &I) {
    bool match = MatchFPToIntegerWithSaturation(I) || MatchModifier(I);
    assert(match && "Pattern match Failed\n");
}

void CodeGenPatternMatch::visitFPToUIInst(llvm::FPToUIInst &I) {
    bool match = MatchFPToIntegerWithSaturation(I) || MatchModifier(I);
    assert(match && "Pattern match Failed\n");
}

bool CodeGenPatternMatch::MatchSIToFPZExt(llvm::SIToFPInst *S2FI) {
    ZExtInst *ZEI = dyn_cast<ZExtInst>(S2FI->getOperand(0));
    if (!ZEI)
        return false;
    if (!ZEI->getSrcTy()->isIntegerTy(1))
        return false;

    struct SIToFPExtPattern : public Pattern {
        SSource src;
        virtual void Emit(EmitPass *pass, const DstModifier &dstMod) {
            pass->EmitSIToFPZExt(src, dstMod);
        }
    };

    SIToFPExtPattern *pat = new (m_allocator) SIToFPExtPattern();
    pat->src = GetSource(ZEI->getOperand(0), false, false);
    AddPattern(pat);

    return true;
}

void CodeGenPatternMatch::visitCastInst(llvm::CastInst &I)
{
    bool match = 0;
    if( I.getOpcode() == Instruction::SExt )
    {
        match = MatchCmpSext(I) ||
                MatchModifier(I);
    } else if (I.getOpcode() == Instruction::SIToFP)
    {
        match = MatchSIToFPZExt(cast<SIToFPInst>(&I)) || MatchModifier(I);
    }
    else if (I.getOpcode() == Instruction::Trunc)
    {
        match = 
            MatchModifier(I);
    }
    else
    {
        match = MatchModifier(I);
    }
}

bool CodeGenPatternMatch::NeedVMask()
{
    return m_NeedVMask;
}

bool CodeGenPatternMatch::HasUseOutsideLoop(llvm::Value* v)
{
    if(Instruction* inst = dyn_cast<Instruction>(v))
    {
        if(Loop* L = LI->getLoopFor(inst->getParent()))
        {
            for(auto UI = inst->user_begin(), E = inst->user_end(); UI != E; ++UI)
            {
                if(!L->contains(cast<Instruction>(*UI)))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

void CodeGenPatternMatch::HandleSubspanUse(llvm::Value* v)
{
    assert(m_root!=nullptr);
    if (m_ctx->type != ShaderType::PIXEL_SHADER)
    {
        return;
    }
    if(!isa<Constant>(v) && m_WI->whichDepend(v) != WIAnalysis::UNIFORM)
    {
        if(isa<PHINode>(v) || HasUseOutsideLoop(v))
        {
            // If a phi is used in a subspan we cannot propagate the subspan use and need to use VMask
            m_NeedVMask = true;
        }
        else
        {
            m_subSpanUse.insert(v);
            if (LoadInst* load = dyn_cast<LoadInst>(v))
            {
                if (load->getPointerAddressSpace() == ADDRESS_SPACE_PRIVATE)
                {
                    m_NeedVMask = true;
                }
            }
            if(HasPhiUse(*v) && m_WI->insideDivergentCF(m_root))
            {
                // \todo, more accurate condition for force-isolation
                ForceIsolate(v);
            }
        }
    }
}

bool CodeGenPatternMatch::MatchMinMax(llvm::SelectInst &SI) {
    // Pattern to emit.
    struct MinMaxPattern : public Pattern {
        SSource srcs[2];
        bool isMin, isUnsigned;
        virtual void Emit(EmitPass *pass, const DstModifier &dstMod) {
            // FIXME: We should tell umax/umin from max/min as integers in LLVM
            // have no sign!
            pass->EmitMinMax(isMin, isUnsigned, srcs, dstMod);
        }
    };

    // Skip min/max pattern matching on FP, which needs to either explicitly
    // use intrinsics or convert them into intrinsic in GenIRLower pass.
    if (SI.getType()->isFloatingPointTy())
      return false;

    bool isMin, isUnsigned;
    llvm::Value *LHS, *RHS;

    if (!isMinOrMax(&SI, LHS, RHS, isMin, isUnsigned))
        return false;

    MinMaxPattern *pat = new (m_allocator) MinMaxPattern();
    // FIXME: We leave unsigned operand without source modifier so far. When
    // its behavior is detailed and correcty modeled, consider to add source
    // modifier support.
    pat->srcs[0] = GetSource(LHS, !isUnsigned, false);
    pat->srcs[1] = GetSource(RHS, !isUnsigned, false);
    pat->isMin = isMin;
    pat->isUnsigned = isUnsigned;
    AddPattern(pat);

    return true;
}

void CodeGenPatternMatch::visitSelectInst(SelectInst &I)
{
    bool match = MatchSatModifier(I) ||
                 MatchIntegerSatModifier(I) ||
                 MatchAbsNeg(I)      ||
                 MatchFPToIntegerWithSaturation(I) ||
                 MatchMinMax(I)      ||
                 /*MatchPredicate(I)   ||*/
                 MatchSelectModifier(I);
    assert(match && "Pattern Match failed\n");
}

void CodeGenPatternMatch::visitBinaryOperator(llvm::BinaryOperator &I)
{

    bool match = false;
    switch(I.getOpcode())
    {
    case Instruction::FSub:
        match = MatchFrc(I) ||
                MatchLrp(I) ||
                MatchMad(I) ||
                MatchAbsNeg(I) ||
                MatchModifier(I);
        break;
    case Instruction::Sub:
        match =
                MatchAbsNeg(I) ||
                MatchMulAdd16(I) ||
                MatchModifier(I);
        break;
    case Instruction::Mul:
        match = MatchFullMul32(I) ||
                MatchMulAdd16(I) ||
                MatchModifier(I);
        break;
    case Instruction::Add:
        match =
                MatchMulAdd16(I) ||
                MatchModifier(I);
        break;
    case Instruction::UDiv:
    case Instruction::SDiv:
    case Instruction::AShr:
        match = MatchAvg(I) ||
                MatchModifier(I);
        break;
    case Instruction::FMul:
    case Instruction::URem:
    case Instruction::SRem:
    case Instruction::FRem:
    case Instruction::Shl:
        match = MatchModifier(I);
        break;
    case Instruction::LShr:
        match = MatchModifier(I, false);
        break;
    case Instruction::FDiv:
        match = MatchRsqrt(I) ||
                MatchModifier(I);
        break;
    case Instruction::FAdd:
        match = MatchLrp(I) ||
                MatchMad(I) ||
                MatchModifier(I);
        break;
    case Instruction::And:
        match = MatchBoolOp(I) ||
                MatchLogicAlu(I);
        break;
    case Instruction::Or:
        match = MatchBoolOp(I) ||
                MatchLogicAlu(I);
        break;
    case Instruction::Xor:
        match = MatchLogicAlu(I);
        break;
    default:
        assert(0 && "unknown binary instruction");
        break;
    }
    assert(match == true);
}

void CodeGenPatternMatch::visitCmpInst(llvm::CmpInst &I)
{
    bool match = MatchCondModifier(I) || 
        MatchModifier(I);
    assert(match);
}

void CodeGenPatternMatch::visitBranchInst(llvm::BranchInst& I)
{
    MatchBranch(I);
}

void CodeGenPatternMatch::visitCallInst(CallInst &I)
{
    bool match = false;
    using namespace GenISAIntrinsic;
    if(GenIntrinsicInst *CI = llvm::dyn_cast<GenIntrinsicInst>(&I) )
    {
        switch(CI->getIntrinsicID())
        {
        case GenISAIntrinsic::GenISA_min:
        case GenISAIntrinsic::GenISA_max:
            match = MatchSatModifier(I) ||
                MatchModifier(I);
            break;
        case GenISAIntrinsic::GenISA_ROUNDNE:
        case GenISAIntrinsic::GenISA_imulH:
        case GenISAIntrinsic::GenISA_umulH:
        case GenISAIntrinsic::GenISA_uaddc:
        case GenISAIntrinsic::GenISA_usubb:
        case GenISAIntrinsic::GenISA_bfrev:
        case GenISAIntrinsic::GenISA_IEEE_Sqrt:
        case GenISAIntrinsic::GenISA_IEEE_Divide:
        case GenISAIntrinsic::GenISA_rsq:
            match = MatchModifier(I);
            break;
        case GenISAIntrinsic::GenISA_intatomicraw:
        case GenISAIntrinsic::GenISA_floatatomicraw:
        case GenISAIntrinsic::GenISA_intatomicrawA64:
        case GenISAIntrinsic::GenISA_floatatomicrawA64:
        case GenISAIntrinsic::GenISA_icmpxchgatomicraw:
        case GenISAIntrinsic::GenISA_fcmpxchgatomicraw:
        case GenISAIntrinsic::GenISA_icmpxchgatomicrawA64:
        case GenISAIntrinsic::GenISA_fcmpxchgatomicrawA64:
        case GenISAIntrinsic::GenISA_dwordatomicstructured:
        case GenISAIntrinsic::GenISA_floatatomicstructured:
        case GenISAIntrinsic::GenISA_cmpxchgatomicstructured:
        case GenISAIntrinsic::GenISA_fcmpxchgatomicstructured:
        case GenISAIntrinsic::GenISA_intatomictyped:
        case GenISAIntrinsic::GenISA_icmpxchgatomictyped:
        case GenISAIntrinsic::GenISA_typedread:
        case GenISAIntrinsic::GenISA_typedwrite:
        case GenISAIntrinsic::GenISA_ldstructured:
        case GenISAIntrinsic::GenISA_storestructured1:
        case GenISAIntrinsic::GenISA_storestructured2:
        case GenISAIntrinsic::GenISA_storestructured3:
        case GenISAIntrinsic::GenISA_storestructured4:
        case GenISAIntrinsic::GenISA_atomiccounterinc:
        case GenISAIntrinsic::GenISA_atomiccounterpredec:
        case GenISAIntrinsic::GenISA_ldptr:
        case GenISAIntrinsic::GenISA_ldrawvector_indexed:
        case GenISAIntrinsic::GenISA_ldraw_indexed:
        case GenISAIntrinsic::GenISA_storerawvector_indexed:
        case GenISAIntrinsic::GenISA_storeraw_indexed:
            match = MatchSingleInstruction(I);
            break;
        case GenISAIntrinsic::GenISA_GradientX:
        case GenISAIntrinsic::GenISA_GradientY:
        case GenISAIntrinsic::GenISA_GradientXfine:
        case GenISAIntrinsic::GenISA_GradientYfine:
            match = MatchGradient(*CI);
            break;
        case GenISAIntrinsic::GenISA_sample:
        case GenISAIntrinsic::GenISA_sampleptr:
        case GenISAIntrinsic::GenISA_sampleB:
        case GenISAIntrinsic::GenISA_sampleBptr:
        case GenISAIntrinsic::GenISA_sampleBC:
        case GenISAIntrinsic::GenISA_sampleBCptr:
        case GenISAIntrinsic::GenISA_sampleC:
        case GenISAIntrinsic::GenISA_sampleCptr:
        case GenISAIntrinsic::GenISA_lod:
        case GenISAIntrinsic::GenISA_lodptr:
        case GenISAIntrinsic::GenISA_sampleKillPix:
            match = MatchSampleDerivative(*CI);
            break;
        case GenISAIntrinsic::GenISA_fsat:
            match = MatchSatModifier(I);
            break;

        case GenISAIntrinsic::GenISA_RTWrite:
            //Sampler to RT EU bypass optimization for CHV+
            match = MatchSamplerToRT(I) ||
                MatchSingleInstruction(I);
            break;
        case GenISAIntrinsic::GenISA_WaveShuffleIndex:
            match = MatchRegisterRegion(*CI) ||
                MatchShuffleBroadCast(*CI) ||
                MatchWaveShuffleIndex(*CI);
            break;
        default:
            match = MatchSingleInstruction(I);
            // no pattern for the rest of the intrinsics
            break;
        }
        assert(match && "no pattern found for GenISA intrinsic");
    }
    else if (Function *Callee = I.getCalledFunction())
    {
        // Only match direct calls and skip declarations.
        if (!Callee->isDeclaration())
        {
            match = MatchSingleInstruction(I);
        }
    }

    assert(match && "no match for this call");
}

void CodeGenPatternMatch::visitUnaryInstruction(llvm::UnaryInstruction &I)
{
    bool match = false;
    switch(I.getOpcode())
    {
    case Instruction::Alloca:
    case Instruction::Load:
    case Instruction::ExtractValue:
        match = MatchSingleInstruction(I);
        break;
    }
    assert(match);
}

void CodeGenPatternMatch::visitIntrinsicInst(llvm::IntrinsicInst &I)
{
    bool match = false;
    switch(I.getIntrinsicID())
    {
    case Intrinsic::sqrt:
    case Intrinsic::log2:
    case Intrinsic::cos:
    case Intrinsic::sin:
    case Intrinsic::pow:
    case Intrinsic::floor:
    case Intrinsic::ceil:
    case Intrinsic::trunc:
    case Intrinsic::ctpop:
    case Intrinsic::ctlz:
    case Intrinsic::cttz:
        match = MatchModifier(I);
        break;
    case Intrinsic::exp2:
        match = MatchPow(I) ||
            MatchModifier(I);
        break;
    case Intrinsic::fabs:
        match = MatchAbsNeg(I);
        break;
    case Intrinsic::fma:
        match = MatchFMA(I);
        break;
    default:
        match = MatchSingleInstruction(I);
        // no pattern for the rest of the intrinsics
        break;
    }
    assert(match && "no pattern found");
}  

void CodeGenPatternMatch::visitStoreInst(StoreInst &I)
{
    bool match = false;
    // we try to fold some pointer values in GFX path, not OCL path
    if(m_ctx->m_DriverInfo.WALoadStorePatternMatch())
    {
        match = MatchSingleInstruction(I);
    }
    else
    {
        match = MatchLoadStorePointer(I, *(I.getPointerOperand())) ||
                MatchSingleInstruction(I);
    }
    assert(match);
}

void CodeGenPatternMatch::visitLoadInst(LoadInst &I)
{
    bool match = false;
    // we try to fold some pointer values in GFX path, not OCL path
    if (m_ctx->m_DriverInfo.WALoadStorePatternMatch())
    {
        match = MatchSingleInstruction(I);
    }
    else
    {
        match = MatchLoadStorePointer(I, *(I.getPointerOperand())) ||
                MatchSingleInstruction(I);
    }
    assert(match);
}

void CodeGenPatternMatch::visitInstruction(llvm::Instruction &I)
{
    // use default pattern
    MatchSingleInstruction(I);
}

void CodeGenPatternMatch::visitExtractElementInst(llvm::ExtractElementInst &I)
{
    Value *VecOpnd = I.getVectorOperand();
    if (isa<Constant>(VecOpnd))
    {
        const Function *F = I.getParent()->getParent();
        unsigned NUse = 0;
        for (auto User : VecOpnd->users())
        {
            if (auto Inst = dyn_cast<Instruction>(User))
            {
                NUse += (Inst->getParent()->getParent() == F);
            }
        }

        // Only add it to pool when there are multiple uses within this
        // function; otherwise no benefit but to hurt RP.
        if (NUse > 1)
            AddToConstantPool(I.getParent(), VecOpnd);
    }
    MatchSingleInstruction(I);
}

void CodeGenPatternMatch::visitPHINode(PHINode &I)
{
     // nothing to do
}

void CodeGenPatternMatch::visitBitCastInst(BitCastInst &I)
{
    MatchSingleInstruction(I);
}

void CodeGenPatternMatch::visitIntToPtrInst(IntToPtrInst &I) {
    MatchSingleInstruction(I);
}

void CodeGenPatternMatch::visitPtrToIntInst(PtrToIntInst &I) {
    MatchSingleInstruction(I);
}

void CodeGenPatternMatch::visitAddrSpaceCast(AddrSpaceCastInst &I)
{
    MatchSingleInstruction(I);
}

void CodeGenPatternMatch::visitDbgInfoIntrinsic(DbgInfoIntrinsic &I)
{
    MatchDbgInstruction(I);
}

void CodeGenPatternMatch::visitExtractValueInst(ExtractValueInst &I) {
    bool Match = false;

    Match = matchAddPair(&I) ||
            matchSubPair(&I) ||
            matchMulPair(&I) ||
            matchPtrToPair(&I);

    assert(Match && "Unknown `extractvalue` instruction!");
}

bool CodeGenPatternMatch::matchAddPair(ExtractValueInst *Ex) {
    Value *V = Ex->getOperand(0);
    GenIntrinsicInst *GII = dyn_cast<GenIntrinsicInst>(V);
    if (!GII || GII->getIntrinsicID() != GenISAIntrinsic::GenISA_add_pair)
        return false;

    if (Ex->getNumIndices() != 1)
        return false;
    unsigned Idx = *Ex->idx_begin();
    if (Idx != 0 && Idx != 1)
        return false;

    struct AddPairPattern : public Pattern {
        GenIntrinsicInst *GII;
        SSource Sources[4]; // L0, H0, L1, H1
        virtual void Emit(EmitPass *Pass, const DstModifier &DstMod) {
            Pass->EmitAddPair(GII, Sources, DstMod);
        }
    };

    struct AddPairSubPattern : public Pattern {
        virtual void Emit(EmitPass *Pass, const DstModifier &Mod) {
            // DO NOTHING. Dummy pattern.
        }
    };

    PairOutputMapTy::iterator MI;
    bool New;
    std::tie(MI, New) = PairOutputMap.insert(std::make_pair(GII, PairOutputTy()));
    if (New) {
        AddPairPattern *Pat = new (m_allocator) AddPairPattern();
        Pat->GII = GII;
        Pat->Sources[0] = GetSource(GII->getOperand(0), false, false);
        Pat->Sources[1] = GetSource(GII->getOperand(1), false, false);
        Pat->Sources[2] = GetSource(GII->getOperand(2), false, false);
        Pat->Sources[3] = GetSource(GII->getOperand(3), false, false);
        AddPattern(Pat);
    } else {
        AddPairSubPattern *Pat = new (m_allocator) AddPairSubPattern();
        AddPattern(Pat);
    }
    if (Idx == 0)
        MI->second.first = Ex;
    else
        MI->second.second = Ex;

    return true;
}

bool CodeGenPatternMatch::matchSubPair(ExtractValueInst *Ex) {
    Value *V = Ex->getOperand(0);
    GenIntrinsicInst *GII = dyn_cast<GenIntrinsicInst>(V);
    if (!GII || GII->getIntrinsicID() != GenISAIntrinsic::GenISA_sub_pair)
        return false;

    if (Ex->getNumIndices() != 1)
        return false;
    unsigned Idx = *Ex->idx_begin();
    if (Idx != 0 && Idx != 1)
        return false;

    struct SubPairPattern : public Pattern {
        GenIntrinsicInst *GII;
        SSource Sources[4]; // L0, H0, L1, H1
        virtual void Emit(EmitPass *Pass, const DstModifier &DstMod) {
            Pass->EmitSubPair(GII, Sources, DstMod);
        }
    };

    struct SubPairSubPattern : public Pattern {
        virtual void Emit(EmitPass *Pass, const DstModifier &Mod) {
            // DO NOTHING. Dummy pattern.
        }
    };

    PairOutputMapTy::iterator MI;
    bool New;
    std::tie(MI, New) = PairOutputMap.insert(std::make_pair(GII, PairOutputTy()));
    if (New) {
        SubPairPattern *Pat = new (m_allocator) SubPairPattern();
        Pat->GII = GII;
        Pat->Sources[0] = GetSource(GII->getOperand(0), false, false);
        Pat->Sources[1] = GetSource(GII->getOperand(1), false, false);
        Pat->Sources[2] = GetSource(GII->getOperand(2), false, false);
        Pat->Sources[3] = GetSource(GII->getOperand(3), false, false);
        AddPattern(Pat);
    } else {
        SubPairSubPattern *Pat = new (m_allocator) SubPairSubPattern();
        AddPattern(Pat);
    }
    if (Idx == 0)
        MI->second.first = Ex;
    else
        MI->second.second = Ex;

    return true;
}

bool CodeGenPatternMatch::matchMulPair(ExtractValueInst *Ex) {
    Value *V = Ex->getOperand(0);
    GenIntrinsicInst *GII = dyn_cast<GenIntrinsicInst>(V);
    if (!GII || GII->getIntrinsicID() != GenISAIntrinsic::GenISA_mul_pair)
        return false;

    if (Ex->getNumIndices() != 1)
        return false;
    unsigned Idx = *Ex->idx_begin();
    if (Idx != 0 && Idx != 1)
        return false;

    struct MulPairPattern : public Pattern {
        GenIntrinsicInst *GII;
        SSource Sources[4]; // L0, H0, L1, H1
        virtual void Emit(EmitPass *Pass, const DstModifier &DstMod) {
            Pass->EmitMulPair(GII, Sources, DstMod);
        }
    };

    struct MulPairSubPattern : public Pattern {
        virtual void Emit(EmitPass *Pass, const DstModifier &Mod) {
            // DO NOTHING. Dummy pattern.
        }
    };

    PairOutputMapTy::iterator MI;
    bool New;
    std::tie(MI, New) = PairOutputMap.insert(std::make_pair(GII, PairOutputTy()));
    if (New) {
        MulPairPattern *Pat = new (m_allocator) MulPairPattern();
        Pat->GII = GII;
        Pat->Sources[0] = GetSource(GII->getOperand(0), false, false);
        Pat->Sources[1] = GetSource(GII->getOperand(1), false, false);
        Pat->Sources[2] = GetSource(GII->getOperand(2), false, false);
        Pat->Sources[3] = GetSource(GII->getOperand(3), false, false);
        AddPattern(Pat);
    } else {
        MulPairSubPattern *Pat = new (m_allocator) MulPairSubPattern();
        AddPattern(Pat);
    }
    if (Idx == 0)
        MI->second.first = Ex;
    else
        MI->second.second = Ex;

    return true;
}

bool CodeGenPatternMatch::matchPtrToPair(ExtractValueInst *Ex) {
    Value *V = Ex->getOperand(0);
    GenIntrinsicInst *GII = dyn_cast<GenIntrinsicInst>(V);
    if (!GII || GII->getIntrinsicID() != GenISAIntrinsic::GenISA_ptr_to_pair)
        return false;

    if (Ex->getNumIndices() != 1)
        return false;
    unsigned Idx = *Ex->idx_begin();
    if (Idx != 0 && Idx != 1)
        return false;

    struct PtrToPairPattern : public Pattern {
        GenIntrinsicInst *GII;
        SSource Sources[1]; // Ptr
        virtual void Emit(EmitPass *Pass, const DstModifier &DstMod) {
            Pass->EmitPtrToPair(GII, Sources, DstMod);
        }
    };

    struct PtrToPairSubPattern : public Pattern {
        virtual void Emit(EmitPass *Pass, const DstModifier &Mod) {
            // DO NOTHING. Dummy pattern.
        }
    };

    PairOutputMapTy::iterator MI;
    bool New;
    std::tie(MI, New) = PairOutputMap.insert(std::make_pair(GII, PairOutputTy()));
    if (New) {
        PtrToPairPattern *Pat = new (m_allocator) PtrToPairPattern();
        Pat->GII = GII;
        Pat->Sources[0] = GetSource(GII->getOperand(0), false, false);
        AddPattern(Pat);
    } else {
        PtrToPairSubPattern *Pat = new (m_allocator) PtrToPairSubPattern();
        AddPattern(Pat);
    }
    if (Idx == 0)
        MI->second.first = Ex;
    else
        MI->second.second = Ex;

    return true;
}

bool CodeGenPatternMatch::MatchAbsNeg(llvm::Instruction& I)
{
    struct MovModifierPattern : public Pattern
    {
        SSource source;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            pass->Mov(source, modifier);
        }
    };
    bool match = false;
    e_modifier mod;
    Value* source;
    if(GetModifier(I, mod,  source))
    {
        MovModifierPattern *pattern = new (m_allocator) MovModifierPattern();
        pattern->source = GetSource(source, mod, false);
        match = true;
        AddPattern(pattern);
    }
    return match;
}

bool CodeGenPatternMatch::MatchFrc(llvm::BinaryOperator& I)
{
    if(m_ctx->m_DriverInfo.DisableMatchFrcPatternMatch())
    {
        return false;
    }

    struct FrcPattern : public Pattern
    {
        SSource source;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            pass->Frc(source, modifier);
        }
    };
    assert(I.getOpcode() == Instruction::FSub);
    llvm::Value* source0 = I.getOperand(0);
    llvm::IntrinsicInst* source1 = llvm::dyn_cast<llvm::IntrinsicInst>(I.getOperand(1));
    bool found = false;
    if(source1 && source1->getIntrinsicID() == Intrinsic::floor)
    {
        if(source1->getOperand(0) == source0)
        {
            found = true;
        }
    }
    if(found)
    {
        FrcPattern *pattern = new (m_allocator) FrcPattern();
        pattern->source = GetSource(source0, true, false);
        AddPattern(pattern);
    }
    return found;
}

SSource CodeGenPatternMatch::GetSource(llvm::Value* value, bool modifier, bool regioning) 
{
    llvm::Value* sourceValue = value;
    e_modifier mod = EMOD_NONE;
    if(modifier)
    {
        GetModifier(*sourceValue, mod, sourceValue);
    }
    return GetSource(sourceValue, mod, regioning);
}

SSource CodeGenPatternMatch::GetSource(llvm::Value* value, e_modifier mod, bool regioning)
{
    SSource source;
    GetRegionModifier(source, value, regioning);
    source.value = value;
    source.mod = mod;
    MarkAsSource(value);
    return source;
}

void CodeGenPatternMatch::MarkAsSource(llvm::Value* v)
{
    // update liveness of the sources
    if(isa<Instruction>(v) || isa<Argument>(v))
    {
        m_LivenessInfo->HandleVirtRegUse(v, m_root->getParent(), m_root);
    }
    // mark the source as used so that we know we need to generate this value
    if(llvm::Instruction* inst = llvm::dyn_cast<Instruction>(v))
    {
        m_usedInstructions.insert(inst);
    }
    if(m_rootIsSubspanUse)
    {
        HandleSubspanUse(v);
    }
}

bool CodeGenPatternMatch::IsSubspanUse(llvm::Value* v)
{
    return m_subSpanUse.find(v) != m_subSpanUse.end();
}

bool CodeGenPatternMatch::MatchFMA( llvm::IntrinsicInst& I )
{
    struct FMAPattern : Pattern
    {
        SSource sources[3];
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            pass->Mad(sources, modifier);
        }
    };

    FMAPattern *pattern = new (m_allocator)FMAPattern();
    for (int i = 0; i < 3; i++)
    {
        AddToConstantPool(I.getParent(), I.getOperand(i));
        pattern->sources[i] = GetSource(I.getOperand(i), true, false);
    }
    AddPattern(pattern);

    return true;
}

bool CodeGenPatternMatch::MatchMad( llvm::BinaryOperator& I )
{
    struct MadPattern : Pattern
    {
        SSource sources[3];
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            pass->Mad(sources, modifier);
        }
    };
    
	if (m_ctx->getModuleMetaData()->isPrecise)
	{
		return false;
	}

    if (m_ctx->type == ShaderType::VERTEX_SHADER &&
        m_ctx->m_DriverInfo.DisabeMatchMad())
    {
        return false;
    }

    bool found = false;
    llvm::Value* sources[3];
    e_modifier src_mod[3];

    if (m_AllowContractions == false || IGC_IS_FLAG_ENABLED(DisableMatchMad))
    {
        return false;
    }

    assert(I.getOpcode() == Instruction::FAdd || I.getOpcode() == Instruction::FSub);
    if(I.getOperand(0) != I.getOperand(1))
    {
        for(uint i=0; i<2; i++)
        {
            Value* src = I.getOperand(i);
            if (FPExtInst *fpextInst = llvm::dyn_cast<llvm::FPExtInst>(src))
            {
                if (!m_Platform.supportMixMode() && fpextInst->getSrcTy()->getTypeID() == llvm::Type::HalfTyID)
                {
                    // no mix mode instructions
                }
                else if (fpextInst->getSrcTy()->getTypeID() != llvm::Type::DoubleTyID &&
                    fpextInst->getDestTy()->getTypeID() != llvm::Type::DoubleTyID)
                {
                    src = fpextInst->getOperand(0);
                }
            }
            llvm::BinaryOperator* mul = llvm::dyn_cast<llvm::BinaryOperator>(src);

            if(mul && mul->getOpcode() == Instruction::FMul)
            {
                // in case we know we won't be able to remove the mul we don't merge it
                if(!m_PosDep->PositionDependsOnInst(mul) && NeedInstruction(*mul))
                    continue;
                sources[2] = I.getOperand(1 - i);
                sources[1] = mul->getOperand(0);
                sources[0] = mul->getOperand(1);
                GetModifier(*sources[0], src_mod[0], sources[0]);
                GetModifier(*sources[1], src_mod[1], sources[1]);
                GetModifier(*sources[2], src_mod[2], sources[2]);
                if(I.getOpcode() == Instruction::FSub)
                {
                    if(i==0)
                    {
                        src_mod[2] = CombineModifier(EMOD_NEG, src_mod[2]);
                    }
                    else
                    {
                        if(llvm::isa<llvm::ConstantFP>(sources[0]))
                        {
                            src_mod[1] = CombineModifier(EMOD_NEG, src_mod[1]);
                        }
                        else
                        {
                            src_mod[0] = CombineModifier(EMOD_NEG, src_mod[0]);
                        }
                    }
                }
                found = true;
                break;
            }
        }
    }
    if(found)
    {
        MadPattern *pattern = new (m_allocator) MadPattern();
        for(int i=0; i<3; i++)
        {
            //CNL+ mad instruction allows 16 bit immediate for src0 and src2
            pattern->sources[i] = GetSource(sources[i], src_mod[i], false);
            if (!m_Platform.support16BitImmSrcForMad() || 
                (sources[i]->getType()->getTypeID() != llvm::Type::HalfTyID) ||
                i == 1)
            {
                AddToConstantPool(I.getParent(), sources[i]);
                pattern->sources[i].fromConstantPool = true;
            }
        }
        AddPattern(pattern);
    }
    return found;
}

bool CodeGenPatternMatch::MatchLoadStorePointer(llvm::Instruction& I, llvm::Value& ptrVal)
{
    struct LoadStorePointerPattern : public Pattern
    {
        Instruction* inst;
        Value* offset;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            if (isa<LoadInst>(inst))
            {
                pass->emitVectorLoad(cast<LoadInst>(inst), offset);
            }
            else if (isa<StoreInst>(inst))
            {
                pass->emitStore3D(cast<StoreInst>(inst), offset);
            }
        }
    };
    GenIntrinsicInst* ptr = dyn_cast<GenIntrinsicInst>(&ptrVal);
    IntToPtrInst *i2p = dyn_cast<IntToPtrInst>(&ptrVal);
    if (ptrVal.getType()->getPointerAddressSpace() == ADDRESS_SPACE_GLOBAL ||
        ptrVal.getType()->getPointerAddressSpace() == ADDRESS_SPACE_CONSTANT)
    {
        return false;
    }
    if (i2p || (ptr && ptr->getIntrinsicID() == GenISAIntrinsic::GenISA_OWordPtr))
    {
        LoadStorePointerPattern *pattern = new (m_allocator) LoadStorePointerPattern();
        pattern->inst = &I;
        uint numSources = GetNbSources(I);
        for (uint i = 0; i<numSources; i++)
        {
            if (I.getOperand(i) != &ptrVal)
            {
                MarkAsSource(I.getOperand(i));
            }
        }
        pattern->offset = cast<Instruction>(&ptrVal)->getOperand(0);
        MarkAsSource(pattern->offset);
        AddPattern(pattern);
        return true;
    }
    return false;
}

bool CodeGenPatternMatch::MatchLrp(llvm::BinaryOperator& I)
{
    struct LRPPattern : public Pattern
    {
        SSource sources[3];
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            pass->Lrp(sources, modifier);
        }
    };

    if (!I.getType()->isFloatTy())
        return false;
    if(!m_Platform.supportLRPInstruction())
        return false;

	if (m_ctx->getModuleMetaData()->isPrecise)
	{
		return false;
	}

    bool found = false;
    llvm::Value* sources[3];
    e_modifier   src_mod[3];

    if (m_AllowContractions == false)
    {
        return false;
    }

    assert(I.getOpcode() == Instruction::FAdd || I.getOpcode() == Instruction::FSub);

    bool startPatternIsAdd = false;
    if (I.getOpcode() == Instruction::FAdd)
    {
        startPatternIsAdd = true;
    }

    // match the case: dst = src0 (src1 - src2)  + src2;
    for(uint i=0;i<2;i++)
    {
        llvm::BinaryOperator* mul = llvm::dyn_cast<llvm::BinaryOperator>(I.getOperand(i));
        if(mul && mul->getOpcode() == Instruction::FMul)
        {
            for(uint j=0;j<2;j++)
            {
                llvm::BinaryOperator* sub = llvm::dyn_cast<llvm::BinaryOperator>(mul->getOperand(j));
                if (sub)
                {
                    llvm::ConstantFP *zero = llvm::dyn_cast<llvm::ConstantFP>(sub->getOperand(0));
                    if (zero && zero->isExactlyValue(0.f))
                    {
                        // in this case we can optimize the pattern into fmad and give better result
                        continue;
                    }

                    if (( startPatternIsAdd && sub->getOpcode() == Instruction::FSub) ||
                        (!startPatternIsAdd && i == 0 && sub->getOpcode() == Instruction::FAdd))
                    {
                        if (sub->getOperand(1) == I.getOperand(1 - i) &&
                            mul->getOperand(0) != mul->getOperand(1))
                        {
                            sources[0] = mul->getOperand(1 - j);
                            sources[1] = sub->getOperand(0);
                            sources[2] = sub->getOperand(1);
                            GetModifier(*sources[0], src_mod[0], sources[0]);
                            GetModifier(*sources[1], src_mod[1], sources[1]);
                            GetModifier(*sources[2], src_mod[2], sources[2]);

                            if (!startPatternIsAdd && i == 0)
                            {
                                // handle patterns like this:
                                // dst = src0 (src1 + src2) - src2;
                                src_mod[2] = CombineModifier(EMOD_NEG, src_mod[2]);
                            }

                            found = true;
                            break;
                        }
                    }
                }
            }
        }
        if(found)
        {
            break;
        }
    }

    // match the case: dst = src0 * src1 + src2 * (1.0 - src0);
    if (!found)
    {
        llvm::BinaryOperator* mul[2];
        mul[0] = llvm::dyn_cast<llvm::BinaryOperator>(I.getOperand(0));
        mul[1] = llvm::dyn_cast<llvm::BinaryOperator>(I.getOperand(1));
        if (mul[0] && mul[0]->getOpcode() == Instruction::FMul &&
            mul[1] && mul[1]->getOpcode() == Instruction::FMul &&
            !llvm::isa<llvm::ConstantFP>(mul[0]->getOperand(0)) &&
            !llvm::isa<llvm::ConstantFP>(mul[0]->getOperand(1)) &&
            !llvm::isa<llvm::ConstantFP>(mul[1]->getOperand(0)) &&
            !llvm::isa<llvm::ConstantFP>(mul[1]->getOperand(1)))
        {
            for (uint i = 0; i < 2; i++)
            {
                for (uint j = 0; j < 2; j++)
                {
                    llvm::BinaryOperator* sub = llvm::dyn_cast<llvm::BinaryOperator>(mul[i]->getOperand(j));
                    if (sub && sub->getOpcode() == Instruction::FSub)
                    {
                        llvm::ConstantFP *one = llvm::dyn_cast<llvm::ConstantFP>(sub->getOperand(0));
                        if (one && one->isExactlyValue(1.f))
                        {
                            for (uint k = 0; k < 2; k++)
                            {
                                if (sub->getOperand(1) == mul[1 - i]->getOperand(k))
                                {
                                    sources[0] = sub->getOperand(1);
                                    sources[1] = mul[1 - i]->getOperand(1 - k);
                                    sources[2] = mul[i]->getOperand(1 - j);
                                    GetModifier(*sources[0], src_mod[0], sources[0]);
                                    GetModifier(*sources[1], src_mod[1], sources[1]);
                                    GetModifier(*sources[2], src_mod[2], sources[2]);
                                    if (!startPatternIsAdd)
                                    {
                                        if (i == 1)
                                        {
                                            // handle patterns like this:
                                            // dst = (src1 * src0) - (src2 * (1.0 - src0))
                                            src_mod[2] = CombineModifier(EMOD_NEG, src_mod[2]);
                                        }
                                        else
                                        {
                                            // handle patterns like this:
                                            // dst = (src2 * (1.0 - src0)) - (src1 * src0)
                                            src_mod[1] = CombineModifier(EMOD_NEG, src_mod[1]);
                                        }
                                    }
                                    found = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (found)
                    {
                        break;
                    }
                }
                if (found)
                {
                    break;
                }
            }
        }
    }

    if (!found)
    {
        // match the case: dst = src2 - (src0 * src2) + (src0 * src1); 
        // match the case: dst = (src0 * src1) + src2 - (src0 * src2); 
        // match the case: dst = src2 + (src0 * src1) - (src0 * src2); 
        if (I.getOpcode() == Instruction::FAdd || I.getOpcode() == Instruction::FSub)
        {
            // dst = op[0] +/- op[1] +/- op[2]
            llvm::Instruction * op[3];
            llvm::Instruction * addSub1 = llvm::dyn_cast<llvm::Instruction>(I.getOperand(0));
            if (addSub1 && (addSub1->getOpcode() == Instruction::FSub || addSub1->getOpcode() == Instruction::FAdd))
            {
                op[0] = llvm::dyn_cast<llvm::Instruction>(addSub1->getOperand(0));
                op[1] = llvm::dyn_cast<llvm::Instruction>(addSub1->getOperand(1));
                op[2] = llvm::dyn_cast<llvm::Instruction>(I.getOperand(1));

                if (op[0] && op[1] && op[2])
                {
                    for (uint casei = 0; casei < 3; casei++)
                    {
                        // i, j, k are the index for op[]
                        uint i = (casei == 2 ? 1 : 0);
                        uint j = (casei == 0 ? 1 : 2);
                        uint k = 2 - casei;

                        //op[i] and op[j] should be fMul, and op[k] is src2
                        if (op[i]->getOpcode() == Instruction::FMul && op[j]->getOpcode() == Instruction::FMul)
                        {
                            for (uint srci = 0; srci < 2; srci++)
                            {
                                for (uint srcj = 0; srcj < 2; srcj++)
                                {
                                    // op[i] and op[j] needs to have one common source. this common source will be src0
                                    if (op[i]->getOperand(srci) == op[j]->getOperand(srcj))
                                    {
                                        // one of the non-common source from op[i] and op[j] needs to be the same as op[k], which is src2
                                        if (op[k] == op[i]->getOperand(1 - srci) ||
                                            op[k] == op[j]->getOperand(1 - srcj))
                                        {
                                            // disable if any of the sources is immediate
                                            if (llvm::isa<llvm::ConstantFP>(op[i]->getOperand(srci)) ||
                                                llvm::isa<llvm::ConstantFP>(op[i]->getOperand(1-srci)) ||
                                                llvm::isa<llvm::ConstantFP>(op[j]->getOperand(srcj)) ||
                                                llvm::isa<llvm::ConstantFP>(op[j]->getOperand(1 - srcj)) ||
                                                llvm::isa<llvm::ConstantFP>(op[k]))
                                            {
                                                break;
                                            }

                                            // check the add/sub cases and add negate to the sources when needed.
                                            /*
                                            ( src0src1, -src0src2, src2 )   okay
                                            ( src0src1, -src0src2, -src2 )  skip
                                            ( src0src1, src0src2, src2 )    skip
                                            ( src0src1, src0src2, -src2 )   negate src2
                                            ( -src0src1, -src0src2, src2 )  negate src1
                                            ( -src0src1, -src0src2, -src2 ) skip
                                            ( -src0src1, src0src2, src2 )   skip
                                            ( -src0src1, src0src2, -src2 )  negate src1 src2
                                            */

                                            bool SignPositiveOp[3];
                                            SignPositiveOp[0] = true;
                                            SignPositiveOp[1] = (addSub1->getOpcode() == Instruction::FAdd);
                                            SignPositiveOp[2] = (I.getOpcode() == Instruction::FAdd);

                                            uint mulSrc0Src1Index = op[k] == op[i]->getOperand(1 - srci) ? j : i;
                                            uint mulSrc0Src2Index = op[k] == op[i]->getOperand(1 - srci) ? i : j;

                                            if (SignPositiveOp[mulSrc0Src2Index] == SignPositiveOp[k] )
                                            {
                                                // abort the cases marked as "skip" in the comment above
                                                break;
                                            }
                                            
                                            sources[0] = op[i]->getOperand(srci);
                                            sources[1] = op[k] == op[i]->getOperand(1 - srci) ? op[j]->getOperand(1 - srcj) : op[i]->getOperand(1 - srci);
                                            sources[2] = op[k];
                                            GetModifier(*sources[0], src_mod[0], sources[0]);
                                            GetModifier(*sources[1], src_mod[1], sources[1]);
                                            GetModifier(*sources[2], src_mod[2], sources[2]);

                                            if (SignPositiveOp[mulSrc0Src1Index] == false )
                                            {
                                                src_mod[1] = CombineModifier(EMOD_NEG, src_mod[1]);
                                            }
                                            if (SignPositiveOp[k] == false)
                                            {
                                                src_mod[2] = CombineModifier(EMOD_NEG, src_mod[2]);
                                            }

                                            found = true;
                                        }
                                    }
                                }
                                if (found)
                                {
                                    break;
                                }
                            }
                        }
                        if (found)
                        {
                            break;
                        }
                    }
                }
            }
        }
    }

    if(found)
    {
        LRPPattern *pattern = new (m_allocator) LRPPattern();
        for(int i=0; i<3; i++)
        {
            pattern->sources[i] = GetSource(sources[i], src_mod[i], false);
        }
        AddPattern(pattern);
    }
    return found;
}

bool CodeGenPatternMatch::MatchCmpSext(llvm::Instruction& I)
{
/*
    %res_s42 = icmp eq i32 %src1_s41, 0
    %17 = sext i1 %res_s42 to i32
        to
    %res_s42 (i32) = icmp eq i32 %src1_s41, 0


    %res_s73 = fcmp oge float %res_s61, %42
    %46 = sext i1 %res_s73 to i32
        to
    %res_s73 (i32) = fcmp oge float %res_s61, %42
*/

    struct CmpSextPattern : Pattern
    {
        llvm::CmpInst* inst;
        SSource sources[2];
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            pass->Cmp( inst->getPredicate(), sources, modifier );
        }
    };
    bool match = false; 

    if( CmpInst* cmpInst = dyn_cast<CmpInst>(I.getOperand(0)) )
    {
        if( cmpInst->getOperand(0)->getType()->getPrimitiveSizeInBits() == I.getType()->getPrimitiveSizeInBits() )
        {
            CmpSextPattern *pattern = new (m_allocator) CmpSextPattern();
            bool supportModifer = SupportsModifier(cmpInst);
            
            pattern->inst = cmpInst;
            pattern->sources[0] = GetSource(cmpInst->getOperand(0), supportModifer, false);
            pattern->sources[1] = GetSource(cmpInst->getOperand(1), supportModifer, false);
            AddPattern(pattern);
            match = true; 
        }
    }

    return match;
}

// Match the pattern of 32 x 32 = 64, a full 32-bit multiplication.
bool CodeGenPatternMatch::MatchFullMul32(llvm::Instruction &I) {
    using namespace llvm::PatternMatch; // Scoped namespace using.

    struct FullMul32Pattern : public Pattern {
        SSource srcs[2];
        bool isUnsigned;
        virtual void Emit(EmitPass *pass, const DstModifier &dstMod)
        {
            pass->EmitFullMul32(isUnsigned, srcs, dstMod);
        }
    };

    assert(I.getOpcode() == llvm::Instruction::Mul && "Mul instruction is expected!");

    if (!I.getType()->isIntegerTy(64))
        return false;

    llvm::Value *LHS = I.getOperand(0);
    llvm::Value *RHS = I.getOperand(1);

    // Swap operand to ensure the constant is always RHS.
    if (isa<ConstantInt>(LHS))
        std::swap(LHS, RHS);

    bool IsUnsigned = false;
    llvm::Value *L = nullptr;
    llvm::Value *R = nullptr;

    // Check LHS
    if (match(LHS, m_SExt(m_Value(L)))) {
        // Bail out if there's non 32-bit integer.
        if (!L->getType()->isIntegerTy(32))
            return false;
    } else if (match(LHS, m_ZExt(m_Value(L)))) {
        // Bail out if there's non 32-bit integer.
        if (!L->getType()->isIntegerTy(32))
            return false;
        IsUnsigned = true;
    } else {
        // Bailout if it's unknown that LHS have less significant bits than the
        // product.
        // NOTE we don't assert the case where LHS is an constant to prevent
        // the assertion in O0 mode. Otherwise, we expect there's at most 1
        // constant operand.
        return false;
    }

    // Check RHS
    if (match(RHS, m_SExt(m_Value(R)))) {
        // Bail out if there's signedness mismatch or non 32-bit integer.
        if (IsUnsigned || !R->getType()->isIntegerTy(32))
            return false;
    } else if (match(RHS, m_ZExt(m_Value(R)))) {
        // Bail out if there's signedness mismatch or non 32-bit integer.
        if (!IsUnsigned || !R->getType()->isIntegerTy(32))
            return false;
        IsUnsigned = true;
    } else if (ConstantInt *CI = dyn_cast<ConstantInt>(RHS)) {
        APInt Val = CI->getValue();
        // 31-bit unsigned integer could be used as either signed or
        // unsigned one. Otherwise, we need special check how MSB is used.
        if (!Val.isIntN(31)) {
            if (!(IsUnsigned && Val.isIntN(32)) &&
                !(!IsUnsigned && Val.isSignedIntN(32))) {
                return false;
            }
        }
        R = ConstantExpr::getTrunc(CI, L->getType());
    } else {
        // Bailout if it's unknown that RHS have less significant bits than the
        // product.
        return false;
    }

    FullMul32Pattern *Pat = new (m_allocator) FullMul32Pattern();
    Pat->srcs[0] = GetSource(L, !IsUnsigned, false);
    Pat->srcs[1] = GetSource(R, !IsUnsigned, false);
    Pat->isUnsigned = IsUnsigned;
    AddPattern(Pat);

    return true;
}

// For 32 bit integer mul/add/sub, use 16bit operands if possible. Thus,
// This will match 16x16->32, 16x32->32, the same for add/sub.
//
// For example:
//   1.  before:
//        %9 = ashr i32 %8, 16
//        %10 = mul nsw i32 %9, -1024
//        ( asr (16|M0)  r19.0<1>:d  r19.0<8;8,1>:d  16:w
//          mul (16|M0)  r19.0<1>:d  r19.0<8;8,1>:d  -1024:w )
//
//      after:
//      --> %10:d = mul %9.1<16;8:2>:w -1024:w
//          (  mul (16|M0)  r23.0<1>:d   r19.1<2;1,0>:w   -1024:w )
//
//  2. before:
//        %9  = lshr i32 %8, 16
//        %10 = and i32 %8, 65535
//        %11 = mul nuw i32 %9, %10
//        ( shr  (16|M0)   r14.0<1>:d  r12.0<8;8,1>:ud   16:w
//          and(16 | M0)   r12.0<1>:d  r12.0<8;8,1>:d  65535:d
//          mul(16 | M0)   r14.0<1>:d  r14.0<8;8,1>:d  r12.0<8;8,1>:d )
//
//     after:
//     --> %11:d = mul %8.1<16;8,2>:uw 65535:uw
//         ( mul (16|M0)  r14.0<1>:d   r12.1<2;1,0>:uw   r12.0<2;1,0>:uw )
//
bool CodeGenPatternMatch::MatchMulAdd16(Instruction& I) {
    using namespace llvm::PatternMatch;

    struct Oprd16Pattern : public Pattern {
        SSource srcs[2];
        Instruction *rootInst;
        virtual void Emit(EmitPass *pass, const DstModifier &dstMod)
        {
            pass->emitMulAdd16(rootInst, srcs, dstMod);
        }
    };

    // The code is under the control of registry key EnableMixIntOperands.
    if (IGC_IS_FLAG_DISABLED(EnableMixIntOperands))
    {
        return false;
    }

    unsigned opc = I.getOpcode();
    assert((opc == Instruction::Mul ||
            opc == Instruction::Add ||
            opc == Instruction::Sub) &&
           "Mul instruction is expected!");

    // Handle 32 bit integer mul/add/sub only.
    if (!I.getType()->isIntegerTy(32))
    {
        return false;
    }

    // Try to replace any source operands with ones of type short if any. As vISA
    // allows the mix of any integer type, each operand is considered separately.
    struct {
        Value *src;
        bool useLower;
        bool isSigned;
    } oprdInfo[2];
    bool isCandidate = false;

    for (int i=0; i < 2; ++i)
    {
        Value *oprd = I.getOperand(i);
        Value *L;

        // oprdInfo[i].src == null --> no W operand replacement.
        oprdInfo[i].src = nullptr;
        if (ConstantInt *CI = dyn_cast<ConstantInt>(oprd))
        {
            int64_t val =
                CI->isNegative() ? CI->getSExtValue() : CI->getZExtValue();
            // If src needs to be negated (y = x - a = x + (-a), as gen only
            // has add), need to check if the negated src fits into W/UW.
            bool isNegSrc = (opc == Instruction::Sub && i == 1);
            if (isNegSrc)
            {
                val = -val;
            }
            if (INT16_MIN <= val && val <= INT16_MAX)
            {
                oprdInfo[i].src = oprd;
                oprdInfo[i].useLower = true; // does not matter for const
                oprdInfo[i].isSigned = true;
                isCandidate = true;
            }
            else if (0 <= val && val <= UINT16_MAX)
            {
                oprdInfo[i].src = oprd;
                oprdInfo[i].useLower = true; // does not matter for const
                oprdInfo[i].isSigned = false;
                isCandidate = true;
            }
        }
        else if (match(oprd, m_And(m_Value(L), m_SpecificInt(0xFFFF))))
        {
            oprdInfo[i].src = L;
            oprdInfo[i].useLower = true;
            oprdInfo[i].isSigned = false;
            isCandidate = true;
        }
        else if (match(oprd, m_LShr(m_Value(L), m_SpecificInt(16))))
        {
            oprdInfo[i].src = L;
            oprdInfo[i].useLower = false;
            oprdInfo[i].isSigned = false;
            isCandidate = true;
        }
        else if (match(oprd, m_AShr(m_Shl(m_Value(L), m_SpecificInt(16)),
                             m_SpecificInt(16))))
        {
            oprdInfo[i].src = L;
            oprdInfo[i].useLower = true;
            oprdInfo[i].isSigned = true;
            isCandidate = true;
        }
        else if (match(oprd, m_AShr(m_Value(L), m_SpecificInt(16))))
        {
            oprdInfo[i].src = L;
            oprdInfo[i].useLower = false;
            oprdInfo[i].isSigned = true;
            isCandidate = true;
        }
    }

    if (!isCandidate) {
        return false;
    }

    Oprd16Pattern *Pat = new (m_allocator)Oprd16Pattern();
    for (int i=0; i < 2; ++i)
    {
        if (oprdInfo[i].src)
        {
            Pat->srcs[i] = GetSource(oprdInfo[i].src, false, false);
            SSource& thisSrc = Pat->srcs[i];

            // for now, Use W/UW only if region_set is false or the src is scalar
            if (thisSrc.region_set &&
                !(thisSrc.region[0] == 0 && thisSrc.region[1] == 1 && thisSrc.region[2] == 0))
            {
                Pat->srcs[i] = GetSource(I.getOperand(i), true, false);
            }
            else
            {
                // Note that SSource's type, if set by GetSource(), should be 32bit type. It's
                // safe to override it with either UW or W. But for SSource's offset, need to
                // re-calculate in term of 16bit, not 32bit.
                thisSrc.type = oprdInfo[i].isSigned ? ISA_TYPE_W : ISA_TYPE_UW;
                thisSrc.elementOffset = (2 * thisSrc.elementOffset) + (oprdInfo[i].useLower ? 0 : 1);
            }
        }
        else
        {
            Pat->srcs[i] = GetSource(I.getOperand(i), true, false);
        }
    }
    Pat->rootInst = &I;
    AddPattern(Pat);

    return true;
}


bool CodeGenPatternMatch::BitcastSearch(SSource& source, llvm::Value*& value, bool broadcast)
{
    if (auto elemInst = dyn_cast<ExtractElementInst>(value))
    {
        if (auto bTInst = dyn_cast<BitCastInst>(elemInst->getOperand(0)))
        {
            // Pattern Matching (Instruction) + ExtractElem + (Vector)Bitcast
            // 
            // In order to set the regioning for the ALU operand
            // I require three things:
            //      -The first is the source number of elements
            //      -The second is the destination number of elements
            //      -The third is the index from the extract element
            //      
            // For example if I have <4 x i32> to <16 x i8> all I need is
            // the 4 (vstride) and the i8 (b) in this case the operand would look 
            // like this -> r22.x <4;1,0>:b 
            // x is calculated below and later on using the simdsize

            uint32_t index, srcNElts, dstNElts, nEltsRatio;
            llvm::Type *srcTy = bTInst->getOperand(0)->getType();
            llvm::Type *dstTy = bTInst->getType();

            srcNElts = (srcTy->isVectorTy()) ? srcTy->getVectorNumElements() : 1;
            dstNElts = (dstTy->isVectorTy()) ? dstTy->getVectorNumElements() : 1;

            if (srcNElts < dstNElts && srcTy->getScalarSizeInBits() < 64 )
            {
                if (isa<ConstantInt>(elemInst->getIndexOperand()))
                {
                    index = int_cast<uint>(cast<ConstantInt>(elemInst->getIndexOperand())->getZExtValue());
                    nEltsRatio = dstNElts / srcNElts;
                    source.value = bTInst->getOperand(0);
                    source.SIMDOffset = iSTD::RoundDownNonPow2(index, nEltsRatio);
                    source.elementOffset = source.elementOffset * nEltsRatio + index % nEltsRatio;
                    value = source.value;
                    if(!broadcast)
                    {
                        source.region_set = true;
                        if(m_WI->whichDepend(value) == WIAnalysis::UNIFORM)
                            source.region[0] = 0;
                        else
                            source.region[0] = (unsigned char)nEltsRatio;
                        source.region[1] = 1;
                        source.region[2] = 0;
                    }
                    return true;
                }
            }
        }
    }
    return false;
}
    

bool CodeGenPatternMatch::MatchModifier(llvm::Instruction& I, bool SupportSrc0Mod)
{
    struct ModifierPattern : public Pattern
    {
        SSource sources[2];
        llvm::Instruction* instruction;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            pass->BinaryUnary(instruction, sources, modifier);
        }
    };

    ModifierPattern *pattern = new (m_allocator)ModifierPattern();
    pattern->instruction = &I;

    bool supportModifer = SupportsModifier(&I);
    bool supportRegioning = SupportsRegioning(&I);
    uint nbSources = GetNbSources(I);
    pattern->sources[0] = GetSource(I.getOperand(0), supportModifer && SupportSrc0Mod, supportRegioning);
    if (nbSources>1)
    {
        pattern->sources[1] = GetSource(I.getOperand(1), supportModifer, supportRegioning);
    }
    AddPattern(pattern);

    return true;
}

bool CodeGenPatternMatch::MatchSingleInstruction(llvm::Instruction& I)
{
    struct SingleInstPattern : Pattern
    {
        llvm::Instruction* inst;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            assert(modifier.sat == false && modifier.flag == nullptr);
            pass->EmitNoModifier(inst);
        }
    };
    SingleInstPattern *pattern = new (m_allocator) SingleInstPattern();
    pattern->inst = &I;
    uint numSources = GetNbSources(I);
    for(uint i =0; i<numSources;i++)
    {
        MarkAsSource(I.getOperand(i));
    }
    AddPattern(pattern);
    return true;   
}

bool CodeGenPatternMatch::MatchBranch(llvm::BranchInst& I)
{
    struct CondBrInstPattern : Pattern
    {
        SSource cond;
        llvm::BranchInst* inst;
        e_predMode predMode = EPRED_NORMAL;
        bool isDiscardBranch = false;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            if (isDiscardBranch)
            {
                pass->emitDiscardBranch(inst, &cond);
            }
            else
            {
                pass->emitBranch(inst, &cond, predMode);
            }
        }
    };

    if (I.isUnconditional())
    {
        return MatchSingleInstruction(I);
    }
    else
    {
        CondBrInstPattern* pattern = new (m_allocator) CondBrInstPattern();
        pattern->inst = &I;

        Value* cond = I.getCondition();
        ICmpInst* icmp = dyn_cast<ICmpInst>(cond);
        bool predMatched = false;

        if (GenIntrinsicInst* intrin = dyn_cast<GenIntrinsicInst>(cond,
            GenISAIntrinsic::GenISA_UpdateDiscardMask))
        {
            pattern->isDiscardBranch = true;
        }
        else
        if (icmp && cond->hasOneUse())
        {
            GenIntrinsicInst* intrin = dyn_cast<GenIntrinsicInst>(
                icmp->getOperand(0), GenISAIntrinsic::GenISA_WaveBallot);
            ConstantInt* constCmp = dyn_cast<ConstantInt>(icmp->getOperand(1));

            if (intrin && constCmp)
            {
                if (icmp->getPredicate() == ICmpInst::ICMP_NE &&
                    constCmp->getZExtValue() == 0)
                {
                    pattern->predMode = EPRED_ANY;
                    pattern->cond = GetSource(intrin->getArgOperand(0), false, false);
                    predMatched = true;
                }
                else
                if (icmp->getPredicate() == ICmpInst::ICMP_EQ &&
                    constCmp->isMinusOne())
                {
                    pattern->predMode = EPRED_ALL;
                    pattern->cond = GetSource(intrin->getArgOperand(0), false, false);
                    predMatched = true;
                }
            }
        }

        if (!predMatched)
        {
            pattern->cond = GetSource(I.getCondition(), false, false);
        }
        AddPattern(pattern);
        return true;
    }

}

bool CodeGenPatternMatch::MatchSamplerToRT(llvm::Instruction& I)
{
    struct SamplerToRT : Pattern
    {
        llvm::GenIntrinsicInst* inst;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            assert(modifier.sat == false && modifier.flag == nullptr);
            pass->emitSampleToRTInstruction(inst);
        }
    };
    // Supported only for CHV+
    // Early return if the optimization is disabled or platform doesn't support it
    //EU Bypass does not work when SVM IOMMU is enabled
    if (IGC_IS_FLAG_ENABLED(DisableEUBypass) || !m_Platform.supportSamplerToRT() 
        || m_Platform.WaDisableEuBypass() || m_Platform.supportFtrWddm2Svm ())
    {
        return false;
    }

    if(m_ctx->getModule()->getNamedMetadata("KillPixel") || 
        m_ctx->getModule()->getNamedMetadata("coarse_phase") || 
        m_ctx->getModule()->getNamedMetadata("pixel_phase"))
    {
        return false;
    }

    llvm::ExtractElementInst* redOutput = llvm::dyn_cast<ExtractElementInst>(I.getOperand(2));
    llvm::ExtractElementInst* greenOutput = llvm::dyn_cast<ExtractElementInst>(I.getOperand(3));
    llvm::ExtractElementInst* blueOutput = llvm::dyn_cast<ExtractElementInst>(I.getOperand(4));
    llvm::ExtractElementInst* alphaOutput = llvm::dyn_cast<ExtractElementInst>(I.getOperand(5));

    //  Check for the pattern immediately preceeded by %0 coming from a supported sample instruction
    //  %oC0.x_ = extractelement <4 x float> %0, i32 0, !dbg !2
    //  %oC0.y_ = extractelement <4 x float> %0, i32 1, !dbg !2
    //  %oC0.z_ = extractelement <4 x float> %0, i32 2, !dbg !2
    //  %oC0.w_ = extractelement <4 x float> %0, i32 3, !dbg !2
    if(redOutput && 
        redOutput->hasOneUse() && 
        greenOutput && 
        greenOutput->hasOneUse() &&  
        blueOutput && 
        blueOutput->hasOneUse() && 
        alphaOutput &&
        alphaOutput->hasOneUse())
    {
        //Skip if there is a swizzle on response from sampler unit
        llvm::ConstantInt* xChannel = llvm::dyn_cast<llvm::ConstantInt>(redOutput->getOperand(1));
        llvm::ConstantInt* yChannel = llvm::dyn_cast<llvm::ConstantInt>(greenOutput->getOperand(1));
        llvm::ConstantInt* zChannel = llvm::dyn_cast<llvm::ConstantInt>(blueOutput->getOperand(1));
        llvm::ConstantInt* wChannel = llvm::dyn_cast<llvm::ConstantInt>(alphaOutput->getOperand(1));

        if (!(xChannel && xChannel->isZero() &&
             yChannel && yChannel->isOne() &&
             zChannel && zChannel->equalsInt(2) &&
             wChannel && wChannel->equalsInt(3)))
        {
            return false;
        }
        llvm::GenIntrinsicInst* inst = llvm::dyn_cast<llvm::GenIntrinsicInst>(redOutput->getOperand(0));
        if(inst && isa<llvm::ReturnInst>(I.getNextNode()))
        {
            //Check if they are from same sample instruction
            if(blueOutput->getOperand(0) == inst   &&
               greenOutput->getOperand(0) == inst  &&
               alphaOutput->getOperand(0) == inst)
            {
                GenISAIntrinsic::ID id = inst->getIntrinsicID();

                //Check for sample instruction type this optimization is supported for
                if( id == GenISAIntrinsic::GenISA_sample ||
                    id == GenISAIntrinsic::GenISA_sampleB ||
                    id == GenISAIntrinsic::GenISA_sampleC ||
                    id == GenISAIntrinsic::GenISA_sampleL ||
                    id == GenISAIntrinsic::GenISA_sampleBC ||
                    id == GenISAIntrinsic::GenISA_sampleLC )
                {
                    llvm::ConstantInt* rti = llvm::dyn_cast<llvm::ConstantInt>(I.getOperand(8));
                    llvm::ConstantInt* hasMask = llvm::dyn_cast<llvm::ConstantInt>(I.getOperand(9));
                    llvm::ConstantInt* hasDepth = llvm::dyn_cast<llvm::ConstantInt>(I.getOperand(10));
                    llvm::ConstantInt* hasStencil = llvm::dyn_cast<llvm::ConstantInt>(I.getOperand(11));

                    if(rti->isZero() && hasMask->isZero() && hasDepth->isZero() && hasStencil->isZero())
                    {
                        uint numSources = inst->getNumArgOperands();
                        llvm::SampleIntrinsic* sampleInst = 
                            llvm::cast<llvm::SampleIntrinsic>(redOutput->getOperand(0));

                        if (!isa<ConstantInt>(sampleInst->getTextureValue()) ||
                            !isa<ConstantInt>(sampleInst->getSamplerValue()))
                        {
                            return false;
                        }

                        SamplerToRT *pattern = new (m_allocator) SamplerToRT();
                        pattern->inst = sampleInst;
                        HandleSampleDerivative(*sampleInst);

                        for(uint i =0; i<numSources;i++)
                        {
                            MarkAsSource(sampleInst->getOperand(i));
                        }
                        AddPattern(pattern);
                        m_samplertoRenderTargetEnable = true;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

bool CodeGenPatternMatch::MatchSatModifier(llvm::Instruction& I)
{
    struct SatPattern : Pattern
    {
        Pattern* pattern;
        SSource source;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            DstModifier mod = modifier;
            mod.sat = true;
            if(pattern)
            {
                pattern->Emit(pass, mod);
            }
            else
            {
                pass->Mov(source, mod);
            }            
        }
    };
    bool match = false;    
    llvm::Value* source;
    if(isSat(&I, source))
    {
        SatPattern *satPattern = new (m_allocator) SatPattern();
        if(llvm::Instruction* inst = llvm::dyn_cast<Instruction>(source))
        {
            // As an heuristic we only match saturate if the instruction has one use 
            // to avoid duplicating expensive instructions and increasing reg pressure 
            // without improve code quality this may be refined in the future
            if(inst->hasOneUse() &&  SupportsSaturate(inst))
            {
                satPattern->pattern = Match(*inst);
                assert(satPattern->pattern && "Failed to match pattern");
                match = true;
            }
        }
        if(!match)
        {
            satPattern->pattern = nullptr;
            satPattern->source = GetSource(source, true, false);
            match = true;
        }
        AddPattern(satPattern);
    }
    return match;
}

bool CodeGenPatternMatch::MatchPredicate(llvm::SelectInst& I)
{
    struct PredicatePattern : Pattern
    {
        bool invertFlag;
        Pattern* patternNotPredicated;
        Pattern* patternPredicated;
        SSource flag;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            DstModifier mod = modifier;
            patternNotPredicated->Emit(pass, mod);
            mod.flag = &flag;
            mod.invertFlag = invertFlag;
            patternPredicated->Emit(pass, mod);
        }
    };
    bool match = false;
    bool invertFlag = false;
    llvm::Instruction* source0 = llvm::dyn_cast<llvm::Instruction>(I.getTrueValue());
    llvm::Instruction* source1 = llvm::dyn_cast<llvm::Instruction>(I.getFalseValue());
    if(source0 && source0->hasOneUse() && source1 && source1->hasOneUse())
    {
        if(SupportsPredicate(source0))
        {
            // temp fix until we find the best solution for this case
            if(!isa<ExtractElementInst>(source1))
            {
                match = true;
            }
        }
        else if(SupportsPredicate(source1))
        {
            if(!isa<ExtractElementInst>(source0))
            {
                std::swap(source0, source1);
                invertFlag = true;
                match = true;
            }
        }
    }
    if(match==true)
    {
        PredicatePattern *pattern = new (m_allocator) PredicatePattern();
        pattern->flag = GetSource(I.getCondition(), false, false);
        pattern->invertFlag = invertFlag;
        pattern->patternNotPredicated = Match(*source1);
        pattern->patternPredicated = Match(*source0);
        assert(pattern->patternNotPredicated &&
               pattern->patternPredicated && "Failed to match pattern");
        AddPattern(pattern);
    }
    return match;
}

bool CodeGenPatternMatch::MatchSelectModifier(llvm::SelectInst& I)
{
    struct SelectPattern : Pattern
    {
        SSource sources[3];
        e_predMode predMode;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            DstModifier modf = modifier;
            modf.predMode = predMode;
            pass->Select(sources, modf);
        }
    };
    SelectPattern *pattern = new (m_allocator) SelectPattern();
    pattern->predMode = EPRED_NORMAL;

    /**
     * Match the IR for blend to fill
     *   %1 = WaveBallot(i1 %cond)      ; return uniform i32
     *   %2 = icmp ne i31 %1, i32 0
     *   %3 = select i1 %2, i32 %x, i32 %y
     *   ->
     *   cmp f0.0 ...
     *   ifany_f0.0 select ...
     */
    Value* cond = I.getCondition();
    ICmpInst* icmp = dyn_cast<ICmpInst>(cond);
    bool predMatched = false;
    if (icmp != nullptr && cond->hasOneUse() &&
        m_WI->whichDepend(cond) == WIAnalysis::UNIFORM &&
        icmp->getPredicate() == ICmpInst::ICMP_NE)
    {
        GenIntrinsicInst* intrin = dyn_cast<GenIntrinsicInst>(
            icmp->getOperand(0), GenISAIntrinsic::GenISA_WaveBallot);
        ConstantInt* const0 = dyn_cast<ConstantInt>(icmp->getOperand(1));
        if (intrin && const0 && const0->getZExtValue() == 0)
        {
            pattern->sources[0] = GetSource(intrin->getArgOperand(0), false, false);
            pattern->predMode = EPRED_ANY;
            predMatched = true;
        }
    }

    if (!predMatched)
    {
        pattern->sources[0] = GetSource(I.getCondition(), false, false);
    }
    pattern->sources[1] = GetSource(I.getTrueValue(), true, false);
    pattern->sources[2] = GetSource(I.getFalseValue(), true, false);
    AddPattern(pattern);
    return true;
}

static bool IsPositiveFloat(Value* v, unsigned int depth = 0)
{
    if(depth > 3)
    {
        // limit the depth of recursion to avoid compile time problem
        return false;
    }
    if(ConstantFP* cst = dyn_cast<ConstantFP>(v))
    {
        if(!cst->getValueAPF().isNegative())
        {
            return true;
        }
    }
    else if(Instruction* I = dyn_cast<Instruction>(v))
    {
        switch(I->getOpcode())
        {
        case Instruction::FMul:
        case Instruction::FAdd:
            return IsPositiveFloat(I->getOperand(0), depth + 1) && IsPositiveFloat(I->getOperand(1), depth + 1);
        case Instruction::Call:
            if(IntrinsicInst* intrinsicInst = dyn_cast<IntrinsicInst>(I))
            {
                if(intrinsicInst->getIntrinsicID() == Intrinsic::fabs)
                {
                    return true;
                }
            }
            else if(isa<GenIntrinsicInst>(I, GenISAIntrinsic::GenISA_fsat))
            {
                return true;
            }
            break;
        default:
            break;
        }
    }
    return false;
}

bool CodeGenPatternMatch::MatchPow(llvm::IntrinsicInst& I)
{
    if (IGC_IS_FLAG_ENABLED(DisableMatchPow))
    {
        return false;
    }

    // For this pattern match exp(log(x) * y) = pow
    // if x < 0 and y is an integer (ex: 1.0)
    // with pattern match : pow(x, 1.0) = x
    // without pattern match : exp(log(x) * 1.0) = NaN because log(x) is NaN.
    //
    // Since pow is 2x slower than exp/log, disabling this optimization might not hurt much.
    // Keep the code and disable MatchPow to track any performance change for now.
    struct PowPattern : public Pattern
    {
        SSource sources[2];
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            pass->Pow(sources, modifier);
        }
    };
    bool found = false;   
    llvm::Value* source0 = NULL;
    llvm::Value* source1 = NULL;
    if(I.getIntrinsicID() == Intrinsic::exp2)
    {
        llvm::BinaryOperator* mul = dyn_cast<BinaryOperator>(I.getOperand((0)));
        if(mul && mul->getOpcode() == Instruction::FMul)
        {
            for(uint j=0;j<2;j++)
            {
                llvm::IntrinsicInst* log = dyn_cast<IntrinsicInst>(mul->getOperand(j));
                if(log && log->getIntrinsicID() == Intrinsic::log2)
                {
                    if(IsPositiveFloat(log->getOperand(0)))
                    {
                        source0 = log->getOperand(0);
                        source1 = mul->getOperand(1 - j);
                        found = true;
                        break;
                    }
                }
            }
        }    
    }
    if(found)
    {
        PowPattern *pattern = new (m_allocator) PowPattern();
        pattern->sources[0] = GetSource(source0, true, false);
        pattern->sources[1] = GetSource(source1, true, false);
        AddPattern(pattern);
    }
    return found;
}

// We match this pattern
// %1 = add %2 %3
// %b = %cmp %1 0
// right now we don't match if the alu has more than 1 use has it could generate worse code
bool CodeGenPatternMatch::MatchCondModifier(llvm::CmpInst& I)
{
    struct CondModifierPattern : Pattern
    {
        Pattern* pattern;
        Instruction* alu;
        CmpInst* cmp;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            assert(modifier.flag == nullptr && modifier.sat == false);
            pass->emitAluConditionMod(pattern, alu, cmp);
        }
    };
    bool found = false;
    for(uint i=0;i<2;i++)
    {
        if (IsZero(I.getOperand(i)))
        {
            llvm::Instruction* alu = dyn_cast<Instruction>(I.getOperand(1-i));
            if(alu && alu->hasOneUse() && SupportsCondModifier(alu))
            {
                CondModifierPattern *pattern = new (m_allocator) CondModifierPattern();
                pattern->pattern = Match(*alu);
                assert(pattern->pattern && "Failed to match pattern");
                pattern->alu = alu;
                pattern->cmp = &I;
                AddPattern(pattern);
                found = true;
                break;
            }
        }
    }
    return found;
}

// we match the following pattern
// %f = cmp %1 %2
// %o = or/and %f %g
bool CodeGenPatternMatch::MatchBoolOp(llvm::BinaryOperator& I)
{
    struct BoolOpPattern : public Pattern
    {
        llvm::BinaryOperator* boolOp;
        llvm::CmpInst::Predicate predicate;
        SSource cmpSource[2];
        SSource binarySource;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            pass->CmpBoolOp(boolOp, predicate, cmpSource, binarySource, modifier);
        }
    };

    assert(I.getOpcode() == Instruction::Or || I.getOpcode() == Instruction::And);
    bool found = false;
    if(I.getType()->isIntegerTy(1))
    {
        for(uint i=0;i<2;i++)
        {
            
            if(CmpInst* cmp = llvm::dyn_cast<CmpInst>(I.getOperand(i)))
            {
                BoolOpPattern *pattern = new (m_allocator) BoolOpPattern();
                pattern->boolOp = &I;
                pattern->predicate = cmp->getPredicate();
                pattern->cmpSource[0] = GetSource(cmp->getOperand(0), true, false);
                pattern->cmpSource[1] = GetSource(cmp->getOperand(1), true, false);
                pattern->binarySource = GetSource(I.getOperand(1 - i), false, false);
                AddPattern(pattern);
                found = true;
                break;                
            }
        }
    }
    return found;
}


bool CodeGenPatternMatch::MatchLogicAlu(llvm::BinaryOperator& I)
{
    struct LogicInstPattern : public Pattern
    {
        SSource sources[2];
        llvm::Instruction* instruction;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            pass->BinaryUnary(instruction, sources, modifier);
        }
    };
    LogicInstPattern *pattern = new (m_allocator) LogicInstPattern();
    pattern->instruction = &I;
    for(unsigned int i = 0; i < 2; ++i)
    {
        e_modifier mod = EMOD_NONE;
        Value* src = I.getOperand(i);
        if(!I.getType()->isIntegerTy(1))
        {
            if(BinaryOperator* notInst = dyn_cast<BinaryOperator>(src))
            {
                if(notInst->getOpcode() == Instruction::Xor)
                {
                    if(ConstantInt* minusOne = dyn_cast<ConstantInt>(notInst->getOperand(1)))
                    {
                        if(minusOne->isMinusOne())
                        {
                            mod = EMOD_NOT;
                            src = notInst->getOperand(0);
                        }
                    }
                }
            }
        }
        pattern->sources[i] = GetSource(src, mod, false);
    }
    AddPattern(pattern);
    return true;
}

bool CodeGenPatternMatch::MatchRsqrt(llvm::BinaryOperator& I)
{
    struct RsqrtPattern : public Pattern
    {
        SSource source;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            pass->Rsqrt(source, modifier);
        }
    };

    bool found = false;
    llvm::Value* source = NULL;
    if(I.getOpcode() == Instruction::FDiv)
    {
        // by vISA document, rsqrt doesn't support double type
        if (isOne(I.getOperand(0)) && I.getType()->getTypeID() != Type::DoubleTyID)
        {
            if(llvm::IntrinsicInst* sqrt = dyn_cast<IntrinsicInst>(I.getOperand(1)))
            {
                if (sqrt->getIntrinsicID() == Intrinsic::sqrt)
                {
                    if (sqrt->hasOneUse())
                    {
                        source = sqrt->getOperand(0);
                        found = true;
                    }
                }
            }
            // OCL needs to emit a special sqrt because the LLVM intrinsic has undefined
            // behavior for negative numbers.
            else if (llvm::GenIntrinsicInst* sqrt = dyn_cast<GenIntrinsicInst>(I.getOperand(1)))
            {
                if (sqrt->getIntrinsicID() == GenISAIntrinsic::GenISA_sqrt)
                {
                    source = sqrt->getOperand(0);
                    found = true;
                }
            }
        }
    }
    if(found)
    {
        RsqrtPattern *pattern = new (m_allocator) RsqrtPattern();
        pattern->source = GetSource(source, true, false);
        AddPattern(pattern);
    }
    return found;
}

bool CodeGenPatternMatch::MatchGradient(llvm::GenIntrinsicInst& I)
{
    struct GradientPattern : public Pattern
    {
        SSource source;
        llvm::Instruction* instruction;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            pass->BinaryUnary(instruction, &source, modifier);
        }
    };
    GradientPattern *pattern = new (m_allocator) GradientPattern();
    pattern->instruction = &I;
    pattern->source = GetSource(I.getOperand(0), true, false);
    AddPattern(pattern);
    // mark the source as subspan use
    HandleSubspanUse(pattern->source.value);
    return true;
}

bool CodeGenPatternMatch::MatchSampleDerivative(llvm::GenIntrinsicInst& I)
{
    HandleSampleDerivative(I);
    return MatchSingleInstruction(I);
}

bool CodeGenPatternMatch::MatchDbgInstruction(llvm::DbgInfoIntrinsic& I)
{
    struct DbgInstPattern : Pattern
    {
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            // Nothing to emit.
        }
    };
    DbgInstPattern *pattern = new (m_allocator) DbgInstPattern();
    if (DbgDeclareInst *pDbgDeclInst = dyn_cast<DbgDeclareInst>(&I))
    {
        if (pDbgDeclInst->getAddress())
        {
            MarkAsSource(pDbgDeclInst->getAddress());
        }
    }
    else if (DbgValueInst *pDbgValInst = dyn_cast<DbgValueInst>(&I))
    {
        if (pDbgValInst->getValue())
        {
            MarkAsSource(pDbgValInst->getValue());
        }
    }
    else
    {
        assert(false && "Unhandled Dbg intrinsic");
    }
    AddPattern(pattern);
    return true;   
}

bool CodeGenPatternMatch::MatchAvg(llvm::Instruction& I)
{
    // "Average value" pattern:
    // (x + y + 1) / 2  -->  avg(x, y)
    //
    // We're looking for patterns like this:
    //    % 14 = add nsw i32 % 10, % 13
    //    % 15 = add nsw i32 % 14, 1
    //    % 16 = ashr i32 % 15, 1

    struct AvgPattern : Pattern
    {
        SSource sources[2];
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            pass->Avg(sources, modifier);
        }
    };

    bool found = false;
    llvm::Value* sources[2];
    e_modifier   src_mod[2];

    assert(I.getOpcode() == Instruction::SDiv || I.getOpcode() == Instruction::UDiv || I.getOpcode() == Instruction::AShr);

    // We expect 2 for "div" and 1 for "right shift".
    int  expectedVal = ( I.getOpcode() == Instruction::SDiv ? 2 : 1 );
    Value * opnd1 = I.getOperand(1);   // Divisor or shift factor.
    if (!isa<ConstantInt>(opnd1) || (cast<ConstantInt>(opnd1))->getZExtValue() != expectedVal)
    {
        return false;
    }

    if (Instruction * divSrc = dyn_cast<Instruction>(I.getOperand(0)))
    {
        if (divSrc->getOpcode() == Instruction::Add && !NeedInstruction(*divSrc))
        {
            Instruction * instAdd = cast<Instruction>(divSrc);
            for (int i = 0; i < 2; i++)
            {
                if (ConstantInt * cnst = dyn_cast<ConstantInt>(instAdd->getOperand(i)))
                {
                    // "otherArg" is the second argument of "instAdd" (which is not constant).
                    Value * otherArg = instAdd->getOperand(i == 0 ? 1 : 0);
                    if (cnst->getZExtValue() == 1 && isa<AddOperator>(otherArg) && !NeedInstruction(*cast<Instruction>(otherArg)))
                    {
                        Instruction * firstAdd = cast<Instruction>(otherArg);
                        sources[0] = firstAdd->getOperand(0);
                        sources[1] = firstAdd->getOperand(1);
                        GetModifier(*sources[0], src_mod[0], sources[0]);
                        GetModifier(*sources[1], src_mod[1], sources[1]);
                        found = true;
                        break;
                    }
                }
            }
        }
    }

    if (found)
    {
        AvgPattern *pattern = new (m_allocator)AvgPattern();
        pattern->sources[0] = GetSource(sources[0], src_mod[0], false);
        pattern->sources[1] = GetSource(sources[1], src_mod[1], false);
        AddPattern(pattern);
    }
    return found;
}

bool CodeGenPatternMatch::MatchShuffleBroadCast(llvm::GenIntrinsicInst& I)
{
    // Match cases like:
    //    %84 = bitcast <2 x i32> %vCastload to <4 x half>
    //    %scalar269 = extractelement <4 x half> % 84, i32 0
    //    %simdShuffle = call half @genx.GenISA.simdShuffle.f.f16(half %scalar269, i32 0)
    //
    // to mov with region and offset
    struct BroadCastPattern : public Pattern
    {
        SSource source;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            pass->Mov(source, modifier);
        }
    };
    bool match = false;
    SSource source;
    Value* sourceV = &I;
    if(GetRegionModifier(source, sourceV, true))
    {
        BroadCastPattern *pattern = new (m_allocator) BroadCastPattern();
        GetModifier(*sourceV, source.mod, sourceV);
        source.value = sourceV;
        pattern->source = source;
        MarkAsSource(sourceV);
        match = true;
        AddPattern(pattern);
    }
    return match;
}

bool CodeGenPatternMatch::MatchWaveShuffleIndex(llvm::GenIntrinsicInst& I)
{
    HandleSubspanUse(I.getArgOperand(0));
    return MatchSingleInstruction(I);
}

bool CodeGenPatternMatch::MatchRegisterRegion(llvm::GenIntrinsicInst& I)
{
    struct MatchRegionPattern : public Pattern
    {
        SSource source;
        virtual void Emit(EmitPass* pass, const DstModifier& modifier)
        {
            pass->Mov(source, modifier);
        }
    };

    /*
    * Match case 1 - With SubReg Offset: Shuffle( data, (laneID << x) + y )
    *   %25 = call i16 @genx.GenISA.simdLaneId()
    *   %30 = zext i16 %25 to i32
    *   %31 = shl nuw nsw i32 %30, 1  - Current LaneID shifted by x
    *   %36 = add i32 %31, 1          - Current LaneID shifted by x + y  Shuffle( data, (laneID << x) + 1 )
    *   %37 = call float @genx.GenISA.WaveShuffleIndex.f32(float %21, i32 %36)

    * Match case 2(Special case of Match Case 1) - No SubReg Offset: Shuffle( data, (laneID << x) + 0 )
    *    %25 = call i16 @genx.GenISA.simdLaneId()
    *    %30 = zext i16 %25 to i32
    *    %31 = shl nuw nsw i32 %30, 1 - Current LaneID shifted by x
    *    %32 = call float @genx.GenISA.WaveShuffleIndex.f32(float %21, i32 %31)
    */

    Value* data = I.getOperand(0);
    Value* source = I.getOperand(1);
    bool isMatch = false;
    int subReg = 0;
    uint verticalStride = 1; //Default value for special case  Shuffle( data, (laneID << x) + y )  when x = 0

    if (auto binaryInst = dyn_cast<BinaryOperator>(source))
    {
        //Will be skipped for match case 2
        if (binaryInst->getOpcode() == Instruction::Add)
        {
            if (llvm::ConstantInt* simDOffSetInst = llvm::dyn_cast<llvm::ConstantInt>(binaryInst->getOperand(1)))
            {
                subReg = int_cast<int>(cast<ConstantInt>(simDOffSetInst)->getZExtValue());

                //Subregister must be a number between 0 and 15 for a valid region
                // We could support up to 31 but we need to handle reading from different SIMD16 var chunks
                if (subReg >= 0 || subReg < 16)
                {
                    source = binaryInst->getOperand(0);
                }
            }
        }
    }

    if (auto binaryInst = dyn_cast<BinaryOperator>(source))
    {
        if (binaryInst->getOpcode() == Instruction::Shl)
        {
            source = binaryInst->getOperand(0);

            if (llvm::ConstantInt* simDOffSetInst = llvm::dyn_cast<llvm::ConstantInt>(binaryInst->getOperand(1)))
            {
                uint shiftFactor = int_cast<uint>(simDOffSetInst->getZExtValue());
                //Check to make sure we dont end up with an invalid Vertical Stride.
                //Only 1, 2, 4, 8, 16 are supported. 
                if (shiftFactor <= 4)
                {
                    verticalStride = (int)pow(2, shiftFactor);
                }
            }
        }
    }

    if (auto zExtInst = llvm::dyn_cast<llvm::ZExtInst>(source))
    {
        source = zExtInst->getOperand(0);
    }

    llvm::GenIntrinsicInst* intrin = llvm::dyn_cast<llvm::GenIntrinsicInst>(source);

    //Finally check for simLaneID intrisic
    if (intrin && (intrin->getIntrinsicID() == GenISAIntrinsic::GenISA_simdLaneId))
    {
        MatchRegionPattern* pattern = new (m_allocator) MatchRegionPattern();
        pattern->source.elementOffset = subReg;

        //Set Region Parameters <VerString;Width,HorzString>
        pattern->source.region_set = true;
        pattern->source.region[0] = verticalStride;
        pattern->source.region[1] = 1;
        pattern->source.region[2] = 0;

        pattern->source.value = data;
        MarkAsSource(data);
        HandleSubspanUse(data);
        AddPattern(pattern);

        isMatch = true;
    }

    return isMatch;
}

bool CodeGenPatternMatch::GetRegionModifier(SSource& sourceMod, llvm::Value*& source, bool regioning)
{
    bool found = false;
    Value* OrignalSource = source;
    if(llvm::BitCastInst* bitCast = llvm::dyn_cast<BitCastInst>(source))
    {
        if(!bitCast->getType()->isVectorTy() && !bitCast->getOperand(0)->getType()->isVectorTy())
        {
            source = bitCast->getOperand(0);
            found = true;
        }
    }

    if(llvm::GenIntrinsicInst* intrin = llvm::dyn_cast<llvm::GenIntrinsicInst>(source))
    {
        GenISAIntrinsic::ID id = intrin->getIntrinsicID();
        if(id == GenISAIntrinsic::GenISA_WaveShuffleIndex)
        {
            if(llvm::ConstantInt* channelVal = llvm::dyn_cast<llvm::ConstantInt>(intrin->getOperand(1)))
            {
                unsigned int offset = int_cast<unsigned int>(channelVal->getZExtValue());
                if(offset < 16)
                {
                    sourceMod.elementOffset = offset;
                    // SIMD shuffle force region <0,1;0>
                    sourceMod.region_set = true;
                    sourceMod.region[0] = 0;
                    sourceMod.region[1] = 1;
                    sourceMod.region[2] = 0;
                    source = intrin->getOperand(0);
                    found = true;
                    BitcastSearch(sourceMod, source, true);
                }
            }
        }
    }
    if (regioning && !sourceMod.region_set)
    {
        found |= BitcastSearch(sourceMod, source, false);
    }
    if(found && sourceMod.type == VISA_Type::ISA_TYPE_NUM)
    {
        // keep the original type
        sourceMod.type = GetType(OrignalSource->getType(), m_ctx);
    }
    return found;
}

void CodeGenPatternMatch::HandleSampleDerivative(llvm::GenIntrinsicInst & I)
{
    switch(I.getIntrinsicID())
    {
    case GenISAIntrinsic::GenISA_sample:
    case GenISAIntrinsic::GenISA_sampleptr:
    case GenISAIntrinsic::GenISA_lod:
    case GenISAIntrinsic::GenISA_lodptr:
    case GenISAIntrinsic::GenISA_sampleKillPix:
        HandleSubspanUse(I.getOperand(0));
        HandleSubspanUse(I.getOperand(1));
        HandleSubspanUse(I.getOperand(2));
        break;
    case GenISAIntrinsic::GenISA_sampleB:
    case GenISAIntrinsic::GenISA_sampleBptr:
    case GenISAIntrinsic::GenISA_sampleC:
    case GenISAIntrinsic::GenISA_sampleCptr:
        HandleSubspanUse(I.getOperand(1));
        HandleSubspanUse(I.getOperand(2));
        HandleSubspanUse(I.getOperand(3));
        break;
    case GenISAIntrinsic::GenISA_sampleBC:
    case GenISAIntrinsic::GenISA_sampleBCptr:
        HandleSubspanUse(I.getOperand(2));
        HandleSubspanUse(I.getOperand(3));
        HandleSubspanUse(I.getOperand(4));
        break;
    default:
        break;
    }
}

// helper function for pattern match
static inline bool isLowerPredicate(llvm::CmpInst::Predicate pred)
{
    switch(pred)
    {
    case llvm::CmpInst::FCMP_ULT:
    case llvm::CmpInst::FCMP_ULE:
    case llvm::CmpInst::FCMP_OLT:
    case llvm::CmpInst::FCMP_OLE:
    case llvm::CmpInst::ICMP_ULT:
    case llvm::CmpInst::ICMP_ULE:
    case llvm::CmpInst::ICMP_SLT:
    case llvm::CmpInst::ICMP_SLE:
        return true;
    default:
        break;
    }
    return false;
}

// helper function for pattern match
static inline bool isGreaterOrLowerPredicate(llvm::CmpInst::Predicate pred)
{
    switch(pred)
    {
    case llvm::CmpInst::FCMP_UGT:
    case llvm::CmpInst::FCMP_UGE:
    case llvm::CmpInst::FCMP_ULT:
    case llvm::CmpInst::FCMP_ULE:
    case llvm::CmpInst::FCMP_OGT:
    case llvm::CmpInst::FCMP_OGE:
    case llvm::CmpInst::FCMP_OLT:
    case llvm::CmpInst::FCMP_OLE:
    case llvm::CmpInst::ICMP_UGT:
    case llvm::CmpInst::ICMP_UGE:
    case llvm::CmpInst::ICMP_ULT:
    case llvm::CmpInst::ICMP_ULE:
    case llvm::CmpInst::ICMP_SGT:
    case llvm::CmpInst::ICMP_SGE:
    case llvm::CmpInst::ICMP_SLT:
    case llvm::CmpInst::ICMP_SLE:
        return true;
    default:
        break;
    }
    return false;
}

static bool isIntegerAbs(SelectInst *SI, e_modifier &mod, Value* &source) {
    using namespace llvm::PatternMatch; // Scoped using declaration.

    Value *Cond = SI->getOperand(0);
    Value *TVal = SI->getOperand(1);
    Value *FVal = SI->getOperand(2);

    ICmpInst::Predicate IPred = FCmpInst::FCMP_FALSE;
    Value *LHS = nullptr;
    Value *RHS = nullptr;

    if (!match(Cond, m_ICmp(IPred, m_Value(LHS), m_Value(RHS))))
        return false;

    if (!ICmpInst::isSigned(IPred))
        return false;

    if (match(LHS, m_Zero())) {
        IPred = ICmpInst::getSwappedPredicate(IPred);
        std::swap(LHS, RHS);
    }

    if (!match(RHS, m_Zero()))
        return false;

    if (match(TVal, m_Neg(m_Specific(FVal)))) {
        IPred = ICmpInst::getInversePredicate(IPred);
        std::swap(TVal, FVal);
    }

    if (!match(FVal, m_Neg(m_Specific(TVal))))
        return false;

    if (LHS != TVal)
        return false;

    source = TVal;
    mod = (IPred == ICmpInst::ICMP_SGT || IPred == ICmpInst::ICMP_SGE) ? EMOD_ABS : EMOD_NEGABS;

    return true;
}

bool isAbs(llvm::Value* abs, e_modifier& mod, llvm::Value*& source)
{
    bool found = false;

    if(IntrinsicInst* intrinsicInst = dyn_cast<IntrinsicInst>(abs))
    {
        if( intrinsicInst->getIntrinsicID() == Intrinsic::fabs )
        {
            source = intrinsicInst->getOperand(0);
            mod = EMOD_ABS;
            return true;
        }
    }
    
    llvm::SelectInst* select = llvm::dyn_cast<llvm::SelectInst>(abs);
    if (!select)
        return false;

    // Try to find floating point abs first
    if(llvm::FCmpInst *cmp = llvm::dyn_cast<llvm::FCmpInst>(select->getOperand(0)))
    {
        llvm::CmpInst::Predicate pred = cmp->getPredicate();
        if(isGreaterOrLowerPredicate(pred))
        {
            for(int zeroIndex = 0; zeroIndex<2; zeroIndex++)
            {
                llvm::ConstantFP *zero = llvm::dyn_cast<llvm::ConstantFP>(cmp->getOperand(zeroIndex));
                if(zero && zero->isZero())
                {
                    llvm::Value* cmpSource = cmp->getOperand(1-zeroIndex);
                    for(int sourceIndex = 0; sourceIndex<2; sourceIndex++)
                    {
                        if(cmpSource == select->getOperand(1+sourceIndex))
                        {
                            llvm::BinaryOperator* negate = 
                                llvm::dyn_cast<llvm::BinaryOperator>(select->getOperand(1+(1-sourceIndex)));
                            llvm::Value* negateSource = NULL;
                            if(negate && IsNegate(*negate, negateSource) && negateSource == cmpSource)
                            {
                                found = true;
                                source = cmpSource;
                                // depending on the order source in cmp/select it can abs() or -abs()
                                bool isNegateAbs = (zeroIndex == 0) ^ isLowerPredicate(pred) ^ (sourceIndex == 1);
                                mod = isNegateAbs ? EMOD_NEGABS : EMOD_ABS;
                            }
                            break;
                        }
                    }                          
                    break;
                }
            }
        }
    }

    // If not found, try integer abs
    return found || isIntegerAbs(select, mod, source);
}

// combine two modifiers, this function is *not* communtative
e_modifier CombineModifier(e_modifier mod1, e_modifier mod2)
{
    e_modifier mod = EMOD_NONE;
    switch(mod1)
    {
    case EMOD_ABS:
    case EMOD_NEGABS:
        mod = mod1;
        break;
    case EMOD_NEG:
        if(mod2 == EMOD_NEGABS)
        {
            mod = EMOD_ABS;
        }
        else if(mod2 == EMOD_ABS)
        {
            mod = EMOD_NEGABS;
        }
        else if(mod2 == EMOD_NEG)
        {
            mod = EMOD_NONE;
        }
        else
        {
            mod = EMOD_NEG;
        }
        break;
    default:
        mod = mod2;
    }
    return mod;
}

bool GetModifier(llvm::Value& modifier, e_modifier& mod, llvm::Value*& source)
{
    mod = EMOD_NONE;
    if(llvm::Instruction* bin = llvm::dyn_cast<llvm::Instruction>(&modifier))
    {
        return GetModifier(*bin, mod, source);
    }
    return false;
}

bool GetModifier(llvm::Instruction& modifier, e_modifier& mod, llvm::Value*& source)
{
    llvm::Value* modifierSource = NULL;
    mod = EMOD_NONE;
    BinaryOperator* bin = dyn_cast<BinaryOperator>(&modifier);
    if(bin && IsNegate(*bin, modifierSource))
    {
        e_modifier absModifier = EMOD_NONE;
        llvm::Value* absSource = NULL;
        if( isAbs(modifierSource, absModifier, absSource))
        {
            source = absSource;
            mod = IGC::CombineModifier(EMOD_NEG, absModifier);
        }
        else
        {
            source = modifierSource;
            mod = EMOD_NEG;
        }
        return true;
    }
    else if(isAbs(&modifier, mod, modifierSource))
    {
        source = modifierSource;
        return true;
    }
    return false;
}

bool IsNegate(llvm::BinaryOperator& sub, llvm::Value*& negateSource)
{
    if(sub.getOpcode() == Instruction::FSub || sub.getOpcode() == Instruction::Sub)
    {
        if(IsZero(sub.getOperand(0)))
        {
            negateSource = sub.getOperand(1);
            return true;
        }
    }
    return false;
}

bool IsZero(llvm::Value* zero)
{
    if (llvm::ConstantFP *FCst = llvm::dyn_cast<llvm::ConstantFP>(zero))
    {
        if(FCst->isZero())
        {
            return true;
        }
    }
    if (llvm::ConstantInt *ICst = llvm::dyn_cast<llvm::ConstantInt>(zero))
    {
        if(ICst->isZero())
        {
            return true;
        }
    }
    return false;
}

inline bool isMinOrMax(llvm::Value* inst, llvm::Value*& source0, llvm::Value*& source1, bool& isMin, bool &isUnsigned)
{
    bool found = false;
    llvm::Instruction* max = llvm::dyn_cast<llvm::Instruction>(inst);
    if(GenIntrinsicInst* intrMinMax = dyn_cast<GenIntrinsicInst>(inst))
    {
        GenISAIntrinsic::ID IID = intrMinMax->getIntrinsicID();
        if(IID == GenISAIntrinsic::GenISA_max ||
            IID == GenISAIntrinsic::GenISA_min)
        {
            source0 = intrMinMax->getOperand(0);
            source1 = intrMinMax->getOperand(1);
            isUnsigned = false;
            isMin = (IID == GenISAIntrinsic::GenISA_min);
            return true;
        }
    }
    else if(max && GetOpCode(max)==llvm_select)
    {
        if (llvm::CmpInst *cmp = llvm::dyn_cast<llvm::CmpInst>(max->getOperand(0)))
        {
            if(isGreaterOrLowerPredicate(cmp->getPredicate()))
            {
                if ((cmp->getOperand(0) == max->getOperand(1) && cmp->getOperand(1) == max->getOperand(2)) ||
                    (cmp->getOperand(0) == max->getOperand(2) && cmp->getOperand(1) == max->getOperand(1)))
                {
                    source0 = max->getOperand(1);
                    source1 = max->getOperand(2);
                    isMin = isLowerPredicate(cmp->getPredicate()) ^ (cmp->getOperand(0) == max->getOperand(2));
                    isUnsigned = IsUnsignedCmp(cmp->getPredicate());
                    found = true;
                }
            }
        }
    }
    return found;
}

inline bool isMax(llvm::Value* max, llvm::Value*& source0, llvm::Value*& source1)
{
    bool isMin, isUnsigned;
    llvm::Value* maxSource0;
    llvm::Value* maxSource1;
    if(isMinOrMax(max, maxSource0, maxSource1, isMin, isUnsigned))
    {
        if(!isMin)
        {
            source0 = maxSource0;
            source1 = maxSource1;
            return true;
        }
    }
    return false;
}

inline bool isMin(llvm::Value* min, llvm::Value*& source0, llvm::Value*& source1)
{
    bool isMin, isUnsigned;
    llvm::Value* maxSource0;
    llvm::Value* maxSource1;
    if(isMinOrMax(min, maxSource0, maxSource1, isMin, isUnsigned))
    {
        if(isMin)
        {
            source0 = maxSource0;
            source1 = maxSource1;
            return true;
        }
    }
    return false;
}


bool isOne(llvm::Value* zero)
{
    if (llvm::ConstantFP *FCst = llvm::dyn_cast<llvm::ConstantFP>(zero))
    {
        if(FCst->isExactlyValue(1.f))
        {
            return true;
        }
    }
    if (llvm::ConstantInt *ICst = llvm::dyn_cast<llvm::ConstantInt>(zero))
    {
        if(ICst->isOne())
        {
            return true;
        }
    }
    return false;
}

bool isSat(llvm::Instruction* sat, llvm::Value*& source)
{
    bool found = false;
    llvm::Value* sources[2] = {0};
    bool typeMatch = sat->getType()->isFloatingPointTy();

    GenIntrinsicInst* intrin = dyn_cast<GenIntrinsicInst>(sat);
    if(intrin && intrin->getIntrinsicID() == GenISAIntrinsic::GenISA_fsat)
    {
            source = intrin->getOperand(0);
            found = true;
    }
    else if(typeMatch && isMax(sat, sources[0], sources[1]))
    {
        for(int i = 0; i<2; i++)
        {
            if(IsZero(sources[i]))
            {
                llvm::Value* maxSources[2] = {0};
                if(isMin(sources[1-i], maxSources[0], maxSources[1]))
                {
                    for(int j = 0; j<2; j++)
                    {
                        if(isOne(maxSources[j]))
                        {
                            found = true;
                            source = maxSources[1-j];
                            break;
                        }                       
                    }
                }
                break;
            }
        }
    }
    else if (typeMatch && isMin(sat, sources[0], sources[1]))
    {
        for(int i = 0; i<2; i++)
        {
            if(isOne(sources[i]))
            {
                llvm::Value* maxSources[2] = {0};
                if(isMax(sources[1-i], maxSources[0], maxSources[1]))
                {
                    for(int j = 0; j<2; j++)
                    {
                        if(IsZero(maxSources[j]))
                        {
                            found = true;
                            source = maxSources[1-j];
                            break;
                        }                       
                    }
                }
                break;
            }
        }
    }
    return found;
}

uint CodeGenPatternMatch::GetBlockId(llvm::BasicBlock* block)
{
    auto it = m_blockMap.find(block);
    assert(it!=m_blockMap.end());
    
    uint blockID = it->second->id;
    return blockID;
}
}//namespace IGC
