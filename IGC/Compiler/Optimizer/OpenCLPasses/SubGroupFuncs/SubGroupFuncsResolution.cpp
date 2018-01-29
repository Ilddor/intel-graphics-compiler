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

#include "Compiler/Optimizer/OpenCLPasses/SubGroupFuncs/SubGroupFuncsResolution.hpp"
#include "Compiler/Optimizer/OpenCLPasses/KernelArgs.hpp"
#include "Compiler/IGCPassSupport.h"
#include "Compiler/MetaDataApi/IGCMetaDataDefs.h"
#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include "common/LLVMWarningsPop.hpp"


using namespace llvm;
using namespace IGC;

// Register pass to igc-opt
#define PASS_FLAG "igc-sub-group-func-resolution"
#define PASS_DESCRIPTION "Resolves sub group functions"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(SubGroupFuncsResolution, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
IGC_INITIALIZE_PASS_END(SubGroupFuncsResolution, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char SubGroupFuncsResolution::ID = 0;

const llvm::StringRef SubGroupFuncsResolution::GET_MAX_SUB_GROUP_SIZE       = "__builtin_IB_get_simd_size";
const llvm::StringRef SubGroupFuncsResolution::GET_SUB_GROUP_LOCAL_ID       = "__builtin_IB_get_simd_id";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_SHUFFLE            = "__builtin_IB_simd_shuffle";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_SHUFFLE_F          = "__builtin_IB_simd_shuffle_f";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_SHUFFLE_H          = "__builtin_IB_simd_shuffle_h";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_SHUFFLE_DOWN       = "__builtin_IB_simd_shuffle_down";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_SHUFFLE_DOWN_US    = "__builtin_IB_simd_shuffle_down_us";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_READ_1_GBL        = "__builtin_IB_simd_block_read_1_global";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_READ_2_GBL        = "__builtin_IB_simd_block_read_2_global";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_READ_4_GBL        = "__builtin_IB_simd_block_read_4_global";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_READ_8_GBL        = "__builtin_IB_simd_block_read_8_global";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_READ_1_GBL_H      = "__builtin_IB_simd_block_read_1_global_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_READ_2_GBL_H      = "__builtin_IB_simd_block_read_2_global_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_READ_4_GBL_H      = "__builtin_IB_simd_block_read_4_global_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_READ_8_GBL_H      = "__builtin_IB_simd_block_read_8_global_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_READ_16_GBL_H     = "__builtin_IB_simd_block_read_16_global_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_WRITE_1_GBL       = "__builtin_IB_simd_block_write_1_global";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_WRITE_2_GBL       = "__builtin_IB_simd_block_write_2_global";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_WRITE_4_GBL       = "__builtin_IB_simd_block_write_4_global";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_WRITE_8_GBL       = "__builtin_IB_simd_block_write_8_global";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_WRITE_1_GBL_H     = "__builtin_IB_simd_block_write_1_global_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_WRITE_2_GBL_H     = "__builtin_IB_simd_block_write_2_global_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_WRITE_4_GBL_H     = "__builtin_IB_simd_block_write_4_global_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_WRITE_8_GBL_H     = "__builtin_IB_simd_block_write_8_global_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_BLOCK_WRITE_16_GBL_H    = "__builtin_IB_simd_block_write_16_global_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_1      = "__builtin_IB_simd_media_block_read_1";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_2      = "__builtin_IB_simd_media_block_read_2";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_4      = "__builtin_IB_simd_media_block_read_4";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_8      = "__builtin_IB_simd_media_block_read_8";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_1_H    = "__builtin_IB_simd_media_block_read_1_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_2_H    = "__builtin_IB_simd_media_block_read_2_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_4_H    = "__builtin_IB_simd_media_block_read_4_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_8_H    = "__builtin_IB_simd_media_block_read_8_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_16_H   = "__builtin_IB_simd_media_block_read_16_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_1     = "__builtin_IB_simd_media_block_write_1";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_2     = "__builtin_IB_simd_media_block_write_2";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_4     = "__builtin_IB_simd_media_block_write_4";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_8     = "__builtin_IB_simd_media_block_write_8";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_1_H   = "__builtin_IB_simd_media_block_write_1_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_2_H   = "__builtin_IB_simd_media_block_write_2_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_4_H   = "__builtin_IB_simd_media_block_write_4_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_8_H   = "__builtin_IB_simd_media_block_write_8_h";
const llvm::StringRef SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_16_H  = "__builtin_IB_simd_media_block_write_16_h";

const llvm::StringRef SubGroupFuncsResolution::MEDIA_BLOCK_READ             = "__builtin_IB_media_block_read";
const llvm::StringRef SubGroupFuncsResolution::MEDIA_BLOCK_WRITE            = "__builtin_IB_media_block_write";

const llvm::StringRef SubGroupFuncsResolution::MEDIA_BLOCK_RECTANGLE_READ   = "__builtin_IB_media_block_rectangle_read";
const llvm::StringRef SubGroupFuncsResolution::GET_IMAGE_BTI                = "__builtin_IB_get_image_bti";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_REDUCE_ADD         = "__builtin_IB_sub_group_reduce_OpGroupIAdd";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_REDUCE_IMAX        = "__builtin_IB_sub_group_reduce_OpGroupSMax";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_REDUCE_UMAX        = "__builtin_IB_sub_group_reduce_OpGroupUMax";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_REDUCE_IMIN        = "__builtin_IB_sub_group_reduce_OpGroupSMin";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_REDUCE_UMIN        = "__builtin_IB_sub_group_reduce_OpGroupUMin";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_REDUCE_FADD        = "__builtin_IB_sub_group_reduce_OpGroupFAdd";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_REDUCE_FMAX        = "__builtin_IB_sub_group_reduce_OpGroupFMax";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_REDUCE_FMIN        = "__builtin_IB_sub_group_reduce_OpGroupFMin";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_SCAN_ADD           = "__builtin_IB_sub_group_scan_OpGroupIAdd";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_SCAN_IMAX          = "__builtin_IB_sub_group_scan_OpGroupSMax";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_SCAN_UMAX          = "__builtin_IB_sub_group_scan_OpGroupUMax";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_SCAN_IMIN          = "__builtin_IB_sub_group_scan_OpGroupSMin";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_SCAN_UMIN          = "__builtin_IB_sub_group_scan_OpGroupUMin";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_SCAN_FADD          = "__builtin_IB_sub_group_scan_OpGroupFAdd";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_SCAN_FMAX          = "__builtin_IB_sub_group_scan_OpGroupFMax";
const llvm::StringRef SubGroupFuncsResolution::SUB_GROUP_SCAN_FMIN          = "__builtin_IB_sub_group_scan_OpGroupFMin";

SubGroupFuncsResolution::SubGroupFuncsResolution(void) : FunctionPass( ID )
{
    initializeSubGroupFuncsResolutionPass( *PassRegistry::getPassRegistry() );
}

bool SubGroupFuncsResolution::runOnFunction( Function &F )
{
    m_argIndexMap.clear();
    m_instsToDelete.clear();
    m_changed = false;

    visit( F );

    for (Instruction * inst : m_instsToDelete) {
        inst->eraseFromParent();
    }

    return m_changed;
}

// Debug line info helper function
static void updateDebugLoc( Instruction *pOrigin, Instruction *pNew )
{
    assert( pOrigin && pNew && "Expect valid instructions" );
    pNew->setDebugLoc( pOrigin->getDebugLoc() );
}

// Helps to obtain temporary index corresponding to the kernel argument.
// This index will be used during codegen to resolve BTIs for Images (SRVs and UAVs).
void SubGroupFuncsResolution::BTIHelper( llvm::CallInst &CI )
{
    Function *F = CI.getParent()->getParent();
    IGC::IGCMD::MetaDataUtils *pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    IGC::IGCMD::ResourceAllocMetaDataHandle resourceAllocInfo = pMdUtils->getFunctionsInfoItem( F )->getResourceAlloc();

    for ( Function::arg_iterator arg = F->arg_begin(), e = F->arg_end(); arg != e; ++arg )
    {
        int argNo = ( *arg ).getArgNo();
        IGC::IGCMD::ArgAllocMetaDataHandle argAllocaInfo = resourceAllocInfo->getArgAllocsItem( argNo );
        IGC::IGCMD::ResourceTypeEnum argType = ( IGC::IGCMD::ResourceTypeEnum ) argAllocaInfo->getType();
        m_argIndexMap[ &(*arg) ] = CImagesBI::ParamInfo(
            argAllocaInfo->getIndex(),
            argType,
            (IGC::IGCMD::ResourceExtensionTypeEnum)argAllocaInfo->getExtenstionType());
    }
}

int32_t SubGroupFuncsResolution::GetSIMDSize(Function *F)
{
    auto *pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    auto funcInfoMD = pMdUtils->getFunctionsInfoItem(F);
    int32_t simdSize = funcInfoMD->getSubGroupSize()->getSIMD_size();

    return simdSize;
}

void SubGroupFuncsResolution::CheckSIMDSize(Instruction &I, StringRef msg)
{
    int32_t simdSize = GetSIMDSize(I.getParent()->getParent());

    if (simdSize == 32 || IGC_GET_FLAG_VALUE(ForceOCLSIMDWidth) == 32)
    {
        auto ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
        ctx->EmitError(std::string(msg).c_str());
    }
}

void SubGroupFuncsResolution::mediaBlockRead(llvm::CallInst &CI)
{
    // Creates intrinsics that will be lowered in the CodeGen and will handle the simd_media_block_read
    SmallVector<Value*, 5> args;
    pushMediaBlockArgs(args, CI);

    // Check if the only use of CI is conversion to float. If so, use float version of intrinsic and remove the cast instruction.
    
    Value* use = NULL;
    if (CI.hasOneUse())
    {
        use = *(CI.user_begin());
    }

    if (use && isa<BitCastInst>(use) && (use->getType()->getScalarType()->isFloatTy() || use->getType()->getScalarType()->isHalfTy()))
    {
        BitCastInst * bitCast = cast<BitCastInst>(use);
        Function*  simdMediaBlockReadFunc = GenISAIntrinsic::getDeclaration(
                                                CI.getCalledFunction()->getParent(),
                                                GenISAIntrinsic::GenISA_simdMediaBlockRead,
                                                use->getType());
        Instruction* simdMediaBlockRead = CallInst::Create(simdMediaBlockReadFunc, args, "", &CI);
        use->replaceAllUsesWith(simdMediaBlockRead);
        m_instsToDelete.push_back(bitCast);
        m_instsToDelete.push_back(&CI);
    }
    else {
        Function*  simdMediaBlockReadFunc = GenISAIntrinsic::getDeclaration(
                                                CI.getCalledFunction()->getParent(),
                                                GenISAIntrinsic::GenISA_simdMediaBlockRead, 
                                                CI.getType());
        Instruction* simdMediaBlockRead = CallInst::Create(simdMediaBlockReadFunc, args, "", &CI);
        CI.replaceAllUsesWith(simdMediaBlockRead);
        CI.eraseFromParent();
    }
    
}

void SubGroupFuncsResolution::mediaBlockWrite(llvm::CallInst &CI)
{
    SmallVector<Value*, 5> args;
    pushMediaBlockArgs(args, CI);
    args.push_back(CI.getArgOperand(2)); // push data

    Function* simdMediaBlockWriteFunc = GenISAIntrinsic::getDeclaration(
                                            CI.getCalledFunction()->getParent(),
                                            GenISAIntrinsic::GenISA_simdMediaBlockWrite,
                                            CI.getArgOperand(2)->getType());
    Instruction* simdMediaBlockWrite = CallInst::Create(simdMediaBlockWriteFunc, args, "", &CI);

    CI.replaceAllUsesWith(simdMediaBlockWrite);
    CI.eraseFromParent();
}

void SubGroupFuncsResolution::simdBlockReadGlobal(llvm::CallInst &CI)
{
    // Creates intrinsics that will be lowered in the CodeGen and will handle the simd_block_read
    LLVMContext& C = CI.getCalledFunction()->getContext();
    SmallVector<Value*, 1> args;
    args.push_back(CI.getArgOperand(0));
    SmallVector<Type*, 2>  types; 
    types.push_back(nullptr); types.push_back(nullptr);
    GenISAIntrinsic::ID  genIntrinID = GenISAIntrinsic::GenISA_simdBlockReadGlobal;

    if (CI.getType()->getScalarType()->getScalarSizeInBits() == 16)
    {
        types[1] = Type::getInt16PtrTy(C, ADDRESS_SPACE_GLOBAL);
    }
    else
    {
        types[1] = (Type::getInt32PtrTy(C, ADDRESS_SPACE_GLOBAL));
    }

    // Check if the only use of CI is conversion to float. If so, use float version of intrinsic and remove the cast instruction.
    
    Value* use = NULL;
    if (CI.hasOneUse())
    {
        use = *(CI.user_begin());
    }

    if (use && isa<BitCastInst>(use) && use->getType()->getScalarType()->isFloatTy())
    {
        BitCastInst * bitCast = cast<BitCastInst>(use);
        types[0] = bitCast->getType();
        Function    * simdMediaBlockReadFunc = GenISAIntrinsic::getDeclaration(
                                                    CI.getCalledFunction()->getParent(),
                                                    genIntrinID,
                                                    types);
        Instruction * simdMediaBlockRead = CallInst::Create(simdMediaBlockReadFunc, args, "", &CI);
        use->replaceAllUsesWith(simdMediaBlockRead);
        m_instsToDelete.push_back(bitCast);
        m_instsToDelete.push_back(&CI);
    }
    else {
        types[0] = CI.getType();
        Function    * simdMediaBlockReadFunc = GenISAIntrinsic::getDeclaration(
                                                   CI.getCalledFunction()->getParent(),
                                                   genIntrinID,
                                                   types);
        Instruction * simdMediaBlockRead = CallInst::Create(simdMediaBlockReadFunc, args, "", &CI);
        CI.replaceAllUsesWith(simdMediaBlockRead);
        CI.eraseFromParent();
    }
}

void SubGroupFuncsResolution::simdBlockWriteGlobal(llvm::CallInst &CI)
{
    LLVMContext& C = CI.getCalledFunction()->getContext();

    SmallVector<Value*, 2> args;
    SmallVector<Type*, 2>  types;
    Value * dataArg = CI.getArgOperand(1);

    args.push_back(CI.getArgOperand(0));
    args.push_back(dataArg);

    if (dataArg->getType()->getScalarType()->getScalarSizeInBits() == 16)
    {
        types.push_back(Type::getInt16PtrTy(C, ADDRESS_SPACE_GLOBAL));
    }
    else
    {
        types.push_back(Type::getInt32PtrTy(C, ADDRESS_SPACE_GLOBAL));
    }

    types.push_back(dataArg->getType());
    Function*    simdBlockWrite1GlobalFunc = GenISAIntrinsic::getDeclaration(CI.getCalledFunction()->getParent(), 
                                                                             GenISAIntrinsic::GenISA_simdBlockWriteGlobal, types);
    Instruction* simdBlockWrite1Global = CallInst::Create(simdBlockWrite1GlobalFunc, args, "", &CI);

    CI.replaceAllUsesWith(simdBlockWrite1Global);
    CI.eraseFromParent();
}

void SubGroupFuncsResolution::pushMediaBlockArgs( llvm::SmallVector<llvm::Value*, 5> &args, llvm::CallInst &CI )
{
    LLVMContext& C = CI.getCalledFunction()->getContext();

    if ( m_argIndexMap.empty() )
    {
        BTIHelper( CI );
    }

    Argument *pImg = nullptr;
    ConstantInt* imageIndex     = IGC::CImagesBI::CImagesUtils::getImageIndex( &m_argIndexMap, &CI, 0, pImg );

    ConstantInt* constIndex     = ConstantInt::get( ( Type::getInt32Ty( C ) ), 0 );
    Instruction* xOffset        = ExtractElementInst::Create( CI.getArgOperand( 1 ), constIndex, "xOffset", &CI );

    ConstantInt* constIndex2    = ConstantInt::get( ( Type::getInt32Ty( C ) ), 1 );
    Instruction* yOffset        = ExtractElementInst::Create( CI.getArgOperand( 1 ), constIndex2, "yOffset", &CI );

    BufferType   imageType      = IGC::CImagesBI::CImagesUtils::getImageType( &m_argIndexMap, &CI, 0 );
    uint32_t     isUAV          = imageType == UAV ? 1 : 0;
    ConstantInt* isImageTypeUAV = ConstantInt::get( ( Type::getInt32Ty( C ) ), isUAV );

    updateDebugLoc( &CI, xOffset );
    updateDebugLoc( &CI, yOffset );

    args.push_back( imageIndex );
    args.push_back( xOffset );
    args.push_back( yOffset );
    args.push_back( isImageTypeUAV );
}

void SubGroupFuncsResolution::subGroupReduce(WaveOps op, CallInst &CI)
{
    IRBuilder<> IRB(&CI);
    Value* arg = CI.getArgOperand(0);
    Value* opVal = IRB.getInt8((uint8_t)op);
    Value* args[2] = { arg, opVal };
    Function* waveAll = GenISAIntrinsic::getDeclaration(CI.getCalledFunction()->getParent(), 
        GenISAIntrinsic::GenISA_WaveAll,
        arg->getType());
    Instruction* waveAllCall = IRB.CreateCall(waveAll, args);
    CI.replaceAllUsesWith(waveAllCall);
}

void SubGroupFuncsResolution::subGroupScan(WaveOps op, CallInst &CI)
{
    IRBuilder<> IRB(&CI);
    Value* arg = CI.getArgOperand(0);
    Value* opVal = IRB.getInt8((uint8_t)op);
    Value* args[3] = { arg, opVal, IRB.getInt1(false) };
    Function* waveScan = GenISAIntrinsic::getDeclaration(CI.getCalledFunction()->getParent(),
        GenISAIntrinsic::GenISA_WavePrefix,
        arg->getType());
    Instruction* waveScanCall = IRB.CreateCall(waveScan, args);
    CI.replaceAllUsesWith(waveScanCall);
}

void SubGroupFuncsResolution::visitCallInst( CallInst &CI )
{
    Function *func = CI.getCalledFunction();
    if (!func)
        return;
    StringRef funcName = func->getName();
    LLVMContext& Ctx = CI.getCalledFunction()->getContext();

    auto reduceTypeStr = { "i16", "i32", "i64" };

    if ( funcName.equals( SubGroupFuncsResolution::GET_MAX_SUB_GROUP_SIZE ) )
    {
        int32_t simdSize = GetSIMDSize(CI.getParent()->getParent());
        if (simdSize == 8 || simdSize == 16 || simdSize == 32)
        {
            auto *C = ConstantInt::get(Type::getInt32Ty(Ctx), simdSize);
            CI.replaceAllUsesWith(C);
        }
        else
        {
            // Creates intrinsics that will be lowered in the CodeGen and will handle the sub_group size
            Function*    simdSizeFunc = GenISAIntrinsic::getDeclaration(CI.getCalledFunction()->getParent(), GenISAIntrinsic::GenISA_simdSize);
            Instruction*  simdSize = CallInst::Create(simdSizeFunc, "simdSize", &CI);
            CI.replaceAllUsesWith(simdSize);
        }
        CI.eraseFromParent();
    }
    else if ( funcName.equals( SubGroupFuncsResolution::GET_SUB_GROUP_LOCAL_ID ) )
    {
        // Creates intrinsics that will be lowered in the CodeGen and will handle the sub_group_local_id
        IntegerType* typeInt32 = Type::getInt32Ty( Ctx );

        Function*    simdLaneIdFunc = GenISAIntrinsic::getDeclaration( CI.getCalledFunction()->getParent(), GenISAIntrinsic::GenISA_simdLaneId );
        Instruction* simdLaneId16   = CallInst::Create( simdLaneIdFunc, "simdLaneId16", &CI );
        Instruction* simdLaneId     = ZExtInst::CreateIntegerCast( simdLaneId16, typeInt32, false, "simdLaneId", &CI );

        CI.replaceAllUsesWith( simdLaneId );
        CI.eraseFromParent();
    }
    else if ( funcName.equals(SubGroupFuncsResolution::SUB_GROUP_SHUFFLE )    ||
              funcName.equals( SubGroupFuncsResolution::SUB_GROUP_SHUFFLE_F ) ||
              funcName.equals( SubGroupFuncsResolution::SUB_GROUP_SHUFFLE_H ) )
    {
        CheckSIMDSize(CI, "Shuffle not supported in SIMD32");

        // Creates intrinsics that will be lowered in the CodeGen and will handle the sub_group_shuffle function
        Value*  args[2];
        args[0] = CI.getArgOperand(0);
        args[1] = CI.getArgOperand(1);

        Function*    simdShuffleFunc = GenISAIntrinsic::getDeclaration(CI.getCalledFunction()->getParent(), 
                                                                       GenISAIntrinsic::GenISA_WaveShuffleIndex, args[0]->getType());
        Instruction* simdShuffle = CallInst::Create(simdShuffleFunc, args, "simdShuffle", &CI);

        CI.replaceAllUsesWith(simdShuffle);
        CI.eraseFromParent();
    }
    else if ( funcName.equals( SubGroupFuncsResolution::SUB_GROUP_SHUFFLE_DOWN ) || 
              funcName.equals( SubGroupFuncsResolution::SUB_GROUP_SHUFFLE_DOWN_US ) )
    {
        CheckSIMDSize(CI, "Shuffle Down not supported in SIMD32");

        // Creates intrinsics that will be lowered in the CodeGen and will handle the sub_group_shuffle_down function
        Value*  args[3];
        args[0] = CI.getArgOperand( 0 );
        args[1] = CI.getArgOperand( 1 );
        args[2] = CI.getArgOperand( 2 );

        Function*    simdShuffleDownFunc = GenISAIntrinsic::getDeclaration( CI.getCalledFunction()->getParent(), 
                                                                            GenISAIntrinsic::GenISA_simdShuffleDown,
                                                                            args[0]->getType());
        Instruction* simdShuffleDown = CallInst::Create( simdShuffleDownFunc, args, "simdShuffleDown", &CI );

        CI.replaceAllUsesWith( simdShuffleDown );
        CI.eraseFromParent();
    }
    else if ( funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_READ_1_GBL ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_READ_2_GBL ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_READ_4_GBL ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_READ_8_GBL ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_READ_1_GBL_H ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_READ_2_GBL_H ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_READ_4_GBL_H ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_READ_8_GBL_H ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_READ_16_GBL_H ) )
    {
        simdBlockReadGlobal(CI);
    }
    else if ( funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_WRITE_1_GBL ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_WRITE_2_GBL ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_WRITE_4_GBL ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_WRITE_8_GBL ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_WRITE_1_GBL_H ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_WRITE_2_GBL_H ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_WRITE_4_GBL_H ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_WRITE_8_GBL_H ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_BLOCK_WRITE_16_GBL_H ) )
    {
        simdBlockWriteGlobal(CI);
    }
    else if ( funcName.equals( SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_1 ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_2 ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_4 ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_8 ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_1_H ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_2_H ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_4_H ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_8_H ) ||
              funcName.equals( SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_READ_16_H ) )
    {
        CheckSIMDSize(CI, "SIMD Media Block Read not supported in SIMD32");
        mediaBlockRead(CI);
    }
    else if ( funcName.equals(SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_1 ) ||
              funcName.equals(SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_2 ) ||
              funcName.equals(SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_4 ) ||
              funcName.equals(SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_8 ) ||
              funcName.equals(SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_1_H ) ||
              funcName.equals(SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_2_H ) ||
              funcName.equals(SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_4_H ) ||
              funcName.equals(SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_8_H ) ||
              funcName.equals(SubGroupFuncsResolution::SIMD_MEDIA_BLOCK_WRITE_16_H) )
    {
        CheckSIMDSize(CI, "SIMD Media Block Write not supported in SIMD32");
        mediaBlockWrite(CI);
    }
    else if (funcName.startswith(SubGroupFuncsResolution::MEDIA_BLOCK_READ))
    {
        // Creates intrinsics that will be lowered in the CodeGen and will handle the media_block_read

        SmallVector<Value*, 5> args;
        pushMediaBlockArgs(args, CI);

        // The spec requires that the width and height are compile-time constants.
        if (!isa<ConstantInt>(CI.getOperand(2)))
        {
            getAnalysis<CodeGenContextWrapper>().getCodeGenContext()->EmitError("width argument supplied to intel_media_block_read*() must be constant.");
            return;
        }

        if (!isa<ConstantInt>(CI.getOperand(3)))
        {
            getAnalysis<CodeGenContextWrapper>().getCodeGenContext()->EmitError("height argument supplied to intel_media_block_read*() must be constant.");
            return;
        }

        args.push_back(CI.getArgOperand(2)); // blockWidth
        args.push_back(CI.getArgOperand(3)); // blockHeight

        Function* MediaBlockReadFunc = GenISAIntrinsic::getDeclaration(
            CI.getCalledFunction()->getParent(),
            GenISAIntrinsic::GenISA_MediaBlockRead,
            CI.getCalledFunction()->getReturnType());

        auto* MediaBlockRead = cast<GenIntrinsicInst>(
			CallInst::Create(MediaBlockReadFunc, args, "", &CI));
        MediaBlockRead->setDebugLoc(CI.getDebugLoc());

        CheckMediaBlockInstError(MediaBlockRead, true);
        //Return if any error
        if (!(getAnalysis<CodeGenContextWrapper>().getCodeGenContext()->oclErrorMessage.empty()))
        {
            return;
        }

        CI.replaceAllUsesWith(MediaBlockRead);
        CI.eraseFromParent();
    }
    else if (funcName.startswith(SubGroupFuncsResolution::MEDIA_BLOCK_WRITE))
    {
        // Creates intrinsics that will be lowered in the CodeGen and will handle the media_block_write

        SmallVector<Value*, 5> args;
        pushMediaBlockArgs(args, CI);

        // The spec requires that the width and height are compile-time constants.
        if (!isa<ConstantInt>(CI.getOperand(2)))
        {
            getAnalysis<CodeGenContextWrapper>().getCodeGenContext()->EmitError("width argument supplied to intel_media_block_write*() must be constant.");
            return;
        }

        if (!isa<ConstantInt>(CI.getOperand(3)))
        {
            getAnalysis<CodeGenContextWrapper>().getCodeGenContext()->EmitError("height argument supplied to intel_media_block_write*() must be constant.");
            return;
        }

        args.push_back(CI.getArgOperand(2)); // blockWidth
        args.push_back(CI.getArgOperand(3)); // blockHeight
        args.push_back(CI.getArgOperand(4)); // pixels

        Function* MediaBlockWriteFunc = GenISAIntrinsic::getDeclaration(
            CI.getCalledFunction()->getParent(),
            GenISAIntrinsic::GenISA_MediaBlockWrite,
            CI.getArgOperand(4)->getType());

        auto* MediaBlockWrite = cast<GenIntrinsicInst>(
			CallInst::Create(MediaBlockWriteFunc, args, "", &CI));
        MediaBlockWrite->setDebugLoc(CI.getDebugLoc());

        CheckMediaBlockInstError(MediaBlockWrite, false);
        //Return if any error
        if (!(getAnalysis<CodeGenContextWrapper>().getCodeGenContext()->oclErrorMessage.empty()))
        {
            return;
        }

        CI.replaceAllUsesWith(MediaBlockWrite);
        CI.eraseFromParent();
    }
    else if (funcName.equals(SubGroupFuncsResolution::MEDIA_BLOCK_RECTANGLE_READ))
    {
        // Creates intrinsics that will be lowered in the CodeGen and will handle the simd_media_block_read_8
        SmallVector<Value*, 5> args;
        pushMediaBlockArgs(args, CI);

        args.push_back(CI.getArgOperand(2)); // blockWidth
        args.push_back(CI.getArgOperand(3)); // blockHeight
        args.push_back(CI.getArgOperand(4)); // destination

        Function*    MediaBlockRectangleReadFunc = GenISAIntrinsic::getDeclaration(CI.getCalledFunction()->getParent(), GenISAIntrinsic::GenISA_MediaBlockRectangleRead);
        Instruction* MediaBlockRectangleRead = CallInst::Create(MediaBlockRectangleReadFunc, args, "", &CI);

        CI.replaceAllUsesWith(MediaBlockRectangleRead);
        CI.eraseFromParent();
    }
    else if (funcName.equals(SubGroupFuncsResolution::GET_IMAGE_BTI))
    {
        if (m_argIndexMap.empty())
        {
            BTIHelper(CI);
        }

        Argument *pImg = nullptr;
        ConstantInt* imageIndex = IGC::CImagesBI::CImagesUtils::getImageIndex(&m_argIndexMap, &CI, 0, pImg);

        CI.replaceAllUsesWith(imageIndex);
        CI.eraseFromParent();
    }
    else if (funcName.startswith(SubGroupFuncsResolution::SUB_GROUP_REDUCE_ADD))
    {
        return subGroupReduce(WaveOps::SUM, CI);
    }
    else if (funcName.startswith(SubGroupFuncsResolution::SUB_GROUP_REDUCE_IMAX))
    {
        return subGroupReduce(WaveOps::IMAX, CI);
    }
    else if (funcName.startswith(SubGroupFuncsResolution::SUB_GROUP_REDUCE_IMIN))
    {
        return subGroupReduce(WaveOps::IMIN, CI);
    }
    else if (funcName.startswith(SubGroupFuncsResolution::SUB_GROUP_REDUCE_UMAX))
    {
        return subGroupReduce(WaveOps::UMAX, CI);
    }
    else if (funcName.startswith(SubGroupFuncsResolution::SUB_GROUP_REDUCE_UMIN))
    {
        return subGroupReduce(WaveOps::UMIN, CI);
    }
    else if (funcName.startswith(SubGroupFuncsResolution::SUB_GROUP_REDUCE_FADD))
    {
        return subGroupReduce(WaveOps::FSUM, CI);
    }
    else if (funcName.startswith(SubGroupFuncsResolution::SUB_GROUP_REDUCE_FMAX))
    {
        return subGroupReduce(WaveOps::FMAX, CI);
    }
    else if (funcName.startswith(SubGroupFuncsResolution::SUB_GROUP_REDUCE_FMIN))
    {
        return subGroupReduce(WaveOps::FMIN, CI);
    }
    else if (funcName.startswith(SubGroupFuncsResolution::SUB_GROUP_SCAN_ADD))
    {
        return subGroupScan(WaveOps::SUM, CI);
    }
    else if (funcName.startswith(SubGroupFuncsResolution::SUB_GROUP_SCAN_IMAX))
    {
        return subGroupScan(WaveOps::IMAX, CI);
    }
    else if (funcName.startswith(SubGroupFuncsResolution::SUB_GROUP_SCAN_IMIN))
    {
        return subGroupScan(WaveOps::IMIN, CI);
    }
    else if (funcName.startswith(SubGroupFuncsResolution::SUB_GROUP_SCAN_UMAX))
    {
        return subGroupScan(WaveOps::UMAX, CI);
    }
    else if (funcName.startswith(SubGroupFuncsResolution::SUB_GROUP_SCAN_UMIN))
    {
        return subGroupScan(WaveOps::UMIN, CI);
    }
    else if (funcName.startswith(SubGroupFuncsResolution::SUB_GROUP_SCAN_FADD))
    {
        return subGroupScan(WaveOps::FSUM, CI);
    }
    else if (funcName.startswith(SubGroupFuncsResolution::SUB_GROUP_SCAN_FMAX))
    {
        return subGroupScan(WaveOps::FMAX, CI);
    }
    else if (funcName.startswith(SubGroupFuncsResolution::SUB_GROUP_SCAN_FMIN))
    {
        return subGroupScan(WaveOps::FMIN, CI);
    }
    else
    {
        // Non Sub Group function, do nothing
        return;
    }
    m_changed = true;
}

void SubGroupFuncsResolution::CheckMediaBlockInstError(llvm::GenIntrinsicInst* inst, bool isRead)
{
    Function* F = inst->getParent()->getParent();

    //Width and height must be supplied as compile time constants.
    uint blockWidth = (uint)cast<ConstantInt>(inst->getOperand(4))->getZExtValue();
    uint blockHeight = (uint)cast<ConstantInt>(inst->getOperand(5))->getZExtValue();

    //Code to extract subgroup size
    IGC::IGCMD::MetaDataUtils* pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    IGC::IGCMD::FunctionInfoMetaDataHandle funcInfoMD = pMdUtils->getFunctionsInfoItem(F);
    unsigned int subGrpSize = funcInfoMD->getSubGroupSize()->getSIMD_size();

    auto *pFunc = inst->getCalledFunction();
    auto *pDataType = isRead ? pFunc->getReturnType() : inst->getOperand(6)->getType();

    const llvm::DataLayout* DL = &F->getParent()->getDataLayout();

    uint typeSize = isa<VectorType>(pDataType) ?
        (uint)DL->getTypeSizeInBits(cast<VectorType>(pDataType)->getVectorElementType()) / 8 :
        (uint)DL->getTypeSizeInBits(pDataType) / 8;

    uint widthInBytes = blockWidth * typeSize;
    uint IOSize = widthInBytes * blockHeight;

    // Determine max rows that can be read by hardware for the given width.
    uint maxRows = 0;
    if (widthInBytes <= 4)
    {
        maxRows = 64;
    }
    else if (widthInBytes <= 8)
    {
        maxRows = 32;
    }
    else if (widthInBytes <= 16)
    {
        maxRows = 16;
    }
    else
    {
        maxRows = 8;
    }

    {
        std::string builtinPrefix = isRead ? "intel_media_block_read" : "intel_media_block_write";

        if (widthInBytes > 32) // hardware restriction on block read width
        {
            std::string output;
            raw_string_ostream S(output);
            S << "width for " << builtinPrefix << "*() must be <= " << 32 / typeSize;
            S.flush();
            CodeGenContext* ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
            ctx->EmitError(output.c_str());
            return;
        }

        if (blockHeight > maxRows) // hardware restriction on block read height
        {
            std::string output;
            raw_string_ostream S(output);
            S << "height for " << widthInBytes << " bytes wide "
                << builtinPrefix << "*() must be <= " << maxRows;
            S.flush();
            CodeGenContext* ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
            ctx->EmitError(output.c_str());
            return;
        }

		if (subGrpSize != 0)
		{
			uint maxIOSize = subGrpSize * ((uint)DL->getTypeSizeInBits(pDataType) / 8);

			if (IOSize > maxIOSize)
			{
				std::string output;
				raw_string_ostream S(output);
				S << builtinPrefix << "*() attempt of " << IOSize <<
					" bytes.  Must be <= " << maxIOSize << " bytes.";
				S.flush();
				CodeGenContext* ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
				ctx->EmitError(output.c_str());
				return;
			}
		}

        if (widthInBytes % 4 != 0)
        {
            std::string output;
            raw_string_ostream S(output);
            if (typeSize == 1)
            {
                S << builtinPrefix << "_uc*() widths must be quad pixel aligned.";
            }
            else
            {
                S << builtinPrefix << "_us*() widths must be dual pixel aligned.";
            }
            S.flush();
            CodeGenContext* ctx = getAnalysis<CodeGenContextWrapper>().getCodeGenContext();
            ctx->EmitError(output.c_str());
            return;
        }
    }
}
