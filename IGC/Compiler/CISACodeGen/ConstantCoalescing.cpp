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
#include "common/debug/Debug.hpp"
#include "common/debug/Dump.hpp"
#include "common/LLVMUtils.h"

#include "Compiler/CISACodeGen/ConstantCoalescing.hpp"
#include "Compiler/CISACodeGen/CISACodeGen.h"
#include "Compiler/CodeGenContextWrapper.hpp"
#include "Compiler/IGCPassSupport.h"
#include "common/IGCIRBuilder.h"

#include <list>

/// @brief ConstantCoalescing merges multiple constant loads into one load
/// of larger quantity
/// - change to oword loads if the address is uniform
/// - change to gather4 or sampler loads if the address is not uniform

using namespace llvm;
using namespace IGC;

// Register pass to igc-opt
#define PASS_FLAG "igc-constant-coalescing"
#define PASS_DESCRIPTION "Constant Coalescing merges multiple constant loads into one load of larger quantity"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(ConstantCoalescing, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
IGC_INITIALIZE_PASS_DEPENDENCY(WIAnalysis)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
IGC_INITIALIZE_PASS_DEPENDENCY(CodeGenContextWrapper)
IGC_INITIALIZE_PASS_END(ConstantCoalescing, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

ConstantCoalescing::ConstantCoalescing() : FunctionPass(ID)
{
    curFunc = NULL;
    irBuilder = NULL;
    initializeConstantCoalescingPass(*PassRegistry::getPassRegistry());
}

bool ConstantCoalescing::runOnFunction(Function &func)
{
    CodeGenContextWrapper* pCtxWrapper = &getAnalysis<CodeGenContextWrapper>();
    m_TT = &getAnalysis<TranslationTable>();

    m_ctx = pCtxWrapper->getCodeGenContext();
    IGCMD::MetaDataUtils *pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils();
    if (pMdUtils->findFunctionsInfoItem(&func) != pMdUtils->end_FunctionsInfo())
    {
        ProcessFunction(&func);
    }
    return true;
}

void ConstantCoalescing::ProcessFunction( Function* function )
{
    curFunc = function;
    irBuilder = new IRBuilderWrapper(function->getContext(), m_TT);
    dataLayout = &function->getParent()->getDataLayout();
    wiAns = &getAnalysis<WIAnalysis>();

    // clean up unnecessary lcssa-phi
    for (Function::iterator I = function->begin(), E = function->end();
         I != E; ++I)
    {
        if (I->getSinglePredecessor())
        {
            for (BasicBlock::iterator BBI = I->begin(), BBE = I->end(); BBI != BBE; )
            {
                Instruction *PHI = &(*BBI++);
                if (!isa<PHINode>(PHI))
                {
                    break;
                }
                assert(PHI->getNumOperands() <= 1);
                Value *src = PHI->getOperand(0);
                if(wiAns->whichDepend(src) == wiAns->whichDepend(PHI))
                {
                    PHI->replaceAllUsesWith(src);
                    PHI->eraseFromParent();
                } 
            }
        }
    }

    // get the dominator-tree to traverse
    DominatorTree &dom_tree = getAnalysis<DominatorTreeWrapperPass>().getDomTree();

    // separate the loads into 3 streams to speed up process
    std::vector<BufChunk*> dircb_owloads;
    std::vector<BufChunk*> indcb_owloads;
    std::vector<BufChunk*> indcb_gathers;
    for( df_iterator<DomTreeNode*> dom_it = df_begin( dom_tree.getRootNode() ),
         dom_end = df_end( dom_tree.getRootNode() ); dom_it != dom_end; ++dom_it )
    {
        BasicBlock *cur_blk = dom_it->getBlock();
        // pop out all the chunks that do not dominate this block
        while( !dircb_owloads.empty() )
        {
            BufChunk *top_chunk = dircb_owloads.back();
            BasicBlock *top_blk = top_chunk->chunkIO->getParent();
            if( dom_tree.dominates( top_blk, cur_blk ) )
                break;
            dircb_owloads.pop_back();
            delete top_chunk;
        }
        while( !indcb_owloads.empty() )
        {
            BufChunk *top_chunk = indcb_owloads.back();
            BasicBlock *top_blk = top_chunk->chunkIO->getParent();
            if( dom_tree.dominates( top_blk, cur_blk ) )
                break;
            //ChangePTRtoOWordBased(top_chunk);
            indcb_owloads.pop_back();
            delete top_chunk;
        }
        while( !indcb_gathers.empty() )
        {
            BufChunk *top_chunk = indcb_gathers.back();
            BasicBlock *top_blk = top_chunk->chunkIO->getParent();
            if( dom_tree.dominates( top_blk, cur_blk ) )
                break;
            indcb_gathers.pop_back();
            delete top_chunk;
        }
        // scan and rewrite cb-load in this block
        ProcessBlock( cur_blk, dircb_owloads, indcb_owloads, indcb_gathers );
        CleanupExtract(cur_blk);
        VectorizePrep(cur_blk);
    }

    // clean up
    while( !dircb_owloads.empty() )
    {
        BufChunk *top_chunk = dircb_owloads.back();
        dircb_owloads.pop_back();
        delete top_chunk;
    }
    while( !indcb_owloads.empty() )
    {
        BufChunk *top_chunk = indcb_owloads.back();
        indcb_owloads.pop_back();
        //ChangePTRtoOWordBased(top_chunk);
        delete top_chunk;
    }
    while( !indcb_gathers.empty() )
    {
        BufChunk *top_chunk = indcb_gathers.back();
        indcb_gathers.pop_back();
        delete top_chunk;
    }
    curFunc = nullptr;
    delete irBuilder;
    irBuilder = nullptr;
}

static void checkInsertExtractMatch(InsertElementInst* insertInst, Value* base, SmallVector<bool, 4>& mask)
{
    auto vectorBase = insertInst->getOperand(0);
    auto extractElt = dyn_cast<ExtractElementInst>(insertInst->getOperand(1));
    auto index = dyn_cast<ConstantInt>(insertInst->getOperand(2));
    if (!index || !extractElt || extractElt->getOperand(0) != base)
    {
        return;
    }
    int indexVal = (int)index->getZExtValue();
    auto extractIndex = dyn_cast<ConstantInt>(extractElt->getOperand(1));
    if (!extractIndex || indexVal != extractIndex->getZExtValue())
    {
        return;
    }
    if (mask[indexVal])
    {
        return;
    }
    mask[indexVal] = true;
    if (auto prevInsert = dyn_cast<InsertElementInst>(vectorBase))
    {
        checkInsertExtractMatch(prevInsert, base, mask);
    }
};

static bool canReplaceInsert(InsertElementInst* insertElt)
{
    VectorType* VTy = cast<VectorType>(insertElt->getOperand(0)->getType());
    ConstantInt* index = dyn_cast<ConstantInt>(insertElt->getOperand(2));
    if (!index || index->getZExtValue() != VTy->getNumElements() - 1)
    {
        return false;
    }
    auto extractElt = dyn_cast<ExtractElementInst>(insertElt->getOperand(1));
    if (!extractElt || extractElt->getOperand(0)->getType() != insertElt->getType())
    {
        return false;
    }
    auto vectorBase = extractElt->getOperand(0);

    int size = (int)VTy->getNumElements();
    llvm::SmallVector<bool, 4> mask;
    for (int i = 0; i < size; ++i)
    {
        mask.push_back(false);
    }
    checkInsertExtractMatch(insertElt, vectorBase, mask);
    for (int i = 0; i < size; ++i)
    {
        if (!mask[i])
        {
            return false;
        }
    }
    return true;
}

// pattern match away redundant insertElt/extractElt pairs introduced by coalescing
//
//  %26 = load <2 x float>, <2 x float> addrspace(65546)* %chunkPtr36, align 4
//  % 27 = extractelement <2 x float> % 26, i32 1
//  % 28 = extractelement <2 x float> % 26, i32 0
//  %29 = insertelement <2 x float> undef, float %28, i32 0
//  %30 = insertelement <2 x float> % 29, float %27, i32 1
//  We can fold the extract/insert and directly use the %26 
bool ConstantCoalescing::CleanupExtract(llvm::BasicBlock* bb)
{
    bool changed = false;
    for (auto I = bb->rbegin(), IEnd = bb->rend(); I != IEnd; ++I)
    {
        // we assume the last element is also the last one to be inserted to the vector
        if (auto insertElt = dyn_cast<InsertElementInst>(&(*I)))
        {     
            if (canReplaceInsert(insertElt))
            {
                changed = true;
                //insertElt->dump();
                auto vectorBase = dyn_cast<ExtractElementInst>(insertElt->getOperand(1))->getOperand(0);
                insertElt->replaceAllUsesWith(vectorBase);
            }
        }   
    }
 
    return changed;
}

// reorder the extract and its use so that vISA can merge single movs into vector movs
//
// %43 = extractelement <4 x float> % 42, i32 2
// %44 = extractelement <4 x float> % 42, i32 1
// %45 = extractelement <4 x float> % 42, i32 0
// ...
// %src0_s111 = fptrunc float %45 to half
// ...
// %src0_s112 = fptrunc float %44 to half
// ...
// %src0_s113 = fptrunc float %43 to half
//
//        to ordered and adjacent sequence:
//
// %43 = extractelement <4 x float> % 42, i32 0
// %src0_s111 = fptrunc float %43 to half
// %44 = extractelement <4 x float> % 42, i32 1
// %src0_s112 = fptrunc float %44 to half
// %45 = extractelement <4 x float> % 42, i32 2
// %src0_s113 = fptrunc float %45 to half
void ConstantCoalescing::VectorizePrep(llvm::BasicBlock* bb)
{
    uint32_t srcNElts = 0;
    for (auto I = bb->rbegin(), IEnd = bb->rend(); I != IEnd; ++I)
    {
        if (LoadInst *load = dyn_cast<LoadInst>(&(*I)))
        {
            if (load->getType()->isVectorTy() && (wiAns->whichDepend(load) == WIAnalysis::UNIFORM))
            {
                srcNElts = load->getType()->getVectorNumElements();
                DenseMap<uint64_t, Instruction*> extractElementMap;
              
                for (auto iter = load->user_begin(); iter != load->user_end(); iter++)
                {
                    ExtractElementInst *extractElt = dyn_cast<ExtractElementInst>(*iter);
                    if (extractElt && extractElt->getParent() == bb)
                    {
                        if (ConstantInt *C = dyn_cast<ConstantInt>(extractElt->getOperand(1)))
                        {
                            extractElementMap[C->getZExtValue()] = extractElt;
                        }
                    }
                }
          
                for (int ie = srcNElts - 1; ie >= 0; ie--)
                {
                    if (Instruction *extEle = extractElementMap[ie])
                    {
                        if (extEle->hasOneUse() && safeToMoveInstUp(extEle, load))
                        {
                            extEle->moveBefore(load->getNextNode());
                            Instruction* extractUse = cast<Instruction>(*extEle->user_begin());
                            if (safeToMoveInstUp(extractUse, extEle))
                            {
                                extractUse->moveBefore(extEle->getNextNode());
                            }
                        }
                    }
                }
            }
        }
    }
}

// check if it is safe to move "inst" to right after "newLocation"
bool ConstantCoalescing::safeToMoveInstUp(Instruction* inst, Instruction* newLocation)
{
    if (inst->mayHaveSideEffects() || inst->mayReadFromMemory() || inst->getParent() != newLocation->getParent())
    {
        return false;
    }

    Instruction* current = inst;
    while (current && current != newLocation)
    {
        if (current->getParent() != newLocation->getParent())
        {
            break;
        }

        for (uint i = 0; i < inst->getNumOperands(); i++)
        {
            if (inst->getOperand(i) == current)
            {
                return false;
            }
        }
        current = current->getPrevNode();
    }

    return true;
}


//32 dwords, meaning 8 owords; This would allow us to use 4 GRFs.
//Hardware limit is 8 GRFs but could build pressure on RA. Hence keeping it to 4 for now.
#define MAX_OWLOAD_SIZE 32

#define MAX_GATHER_SIZE  4  // 4 dwords
#define MAX_VECTOR_INPUT 4  // 4 element

void ConstantCoalescing::ProcessBlock(
    BasicBlock *blk, 
    std::vector<BufChunk*> &dircb_owloads,
    std::vector<BufChunk*> &indcb_owloads,
    std::vector<BufChunk*> &indcb_gathers)
{
    // get work-item analysis, need to update uniformness information
    for( BasicBlock::iterator BBI = blk->begin(), BBE = blk->end();
         BBI != BBE; ++BBI )
    {

        // skip dead instructions
        if(BBI->use_empty())
        {
            continue;
        }
        // bindless case
        if(LdRawIntrinsic* ldRaw = dyn_cast<LdRawIntrinsic>(BBI))
        {
            bool directIdx = false;
            unsigned int bufId = 0;
            BufferType bufType = DecodeAS4GFXResource(
                ldRaw->getResourceValue()->getType()->getPointerAddressSpace(), directIdx, bufId);
            if(bufType != BINDLESS_READONLY)
            {
                continue;
            }
            ConstantInt *elt_idx = dyn_cast<ConstantInt>(ldRaw->getOffsetValue());
            if(elt_idx)
            {   // direct access
                uint eltid = (uint)elt_idx->getZExtValue();
                if((int32_t)eltid >= 0)
                {
                    assert((eltid % 4) == 0);
                    eltid = (eltid >> 2); // bytes to dwords
                    if(wiAns->whichDepend(ldRaw) == WIAnalysis::UNIFORM)
                    {
                        uint maxEltPlus = 1;
                        if(ldRaw->getType()->isVectorTy())
                        {
                            // \todo, another parameter to tune
                            if(ldRaw->getType()->getVectorNumElements() > MAX_VECTOR_INPUT)
                                continue;
                            maxEltPlus = CheckVectorElementUses(ldRaw);
                            // maxEltPlus == 0, means that vector may be used with index or as-vector,
                            // skip it for now
                            if(maxEltPlus == 0)
                                continue;
                        }
                        MergeUniformLoad(ldRaw, ldRaw->getResourceValue(), 0, nullptr, eltid, maxEltPlus, dircb_owloads);
                    }
                }
            }
            continue;
        }
        LoadInst *inst = dyn_cast<LoadInst>(BBI);
        // skip load on struct or array type
        if (!inst || inst->getType()->isAggregateType())
        {
            continue;
        }
        Type *loadType = inst->getType();
        Type *elemType = loadType->getScalarType();
        // right now, only work on load with dword element-type
        if (elemType->getPrimitiveSizeInBits() != SIZE_DWORD * 8)
        {
            continue;
        }

        // stateless path: use int2ptr, and support vector-loads
        Value* elt_idxv = nullptr;
        Value* buf_idxv = nullptr;
        if (inst->getPointerAddressSpace() == ADDRESS_SPACE_CONSTANT)
        {
            // another limit: load has to be dword-aligned
            if (inst->getAlignment() % 4)
                continue;
            uint maxEltPlus = 1;
            if (loadType->isVectorTy())
            {
                // \todo, another parameter to tune
                if (loadType->getVectorNumElements() > MAX_VECTOR_INPUT)
                    continue;
                maxEltPlus = CheckVectorElementUses(inst);
                // maxEltPlus == 0, means that vector may be used with index or as-vector,
                // skip it for now
                if (maxEltPlus == 0)
                    continue;
            }
            uint eltid = 0;
            if (DecomposePtrExp(inst->getPointerOperand(), buf_idxv, elt_idxv, eltid))
            {
                // TODO: Disabling constant coalescing when we see that the offset to the constant buffer is negtive
                // As we handle all negative offsets as uint and some arithmetic operations do not work well. Needs more detailed fix
                if ((int32_t)eltid >= 0)
                {
                    if (wiAns->whichDepend(inst) == WIAnalysis::UNIFORM)
                    {   // uniform
                        if (elt_idxv)
                            MergeUniformLoad(inst, buf_idxv, 0, elt_idxv, eltid, maxEltPlus, indcb_owloads);
                        else
                            MergeUniformLoad(inst, buf_idxv, 0, nullptr, eltid, maxEltPlus, dircb_owloads);
                    }
                    else
                    {   // not uniform
                        MergeScatterLoad(inst, buf_idxv, 0, elt_idxv, eltid, maxEltPlus, indcb_gathers);
                    }
                }
            }
            continue;
        }
              

        uint bufId = 0;
        Value *elt_ptrv = nullptr;
        // \todo, handle dynamic buffer-indexing if necessary
        BufferType bufType = BUFFER_TYPE_UNKNOWN;
        bool is_cbload = IsReadOnlyLoadDirectCB(inst, bufId, elt_ptrv, bufType);
        if( is_cbload )
        {
            uint addrSpace = inst->getPointerAddressSpace();
            uint maxEltPlus = 1;
            if(loadType->isVectorTy())
            {
                // \todo, another parameter to tune
                if(loadType->getVectorNumElements() > MAX_VECTOR_INPUT)
                    continue;
                maxEltPlus = CheckVectorElementUses(inst);
                // maxEltPlus == 0, means that vector may be used with index or as-vector,
                // skip it for now
                if(maxEltPlus == 0)
                    continue;
            }
            if (isa<ConstantPointerNull>(elt_ptrv))
            {
                MergeUniformLoad( inst, nullptr, addrSpace, nullptr, 0, maxEltPlus, dircb_owloads );
            }
            else if (isa<IntToPtrInst>(elt_ptrv))
            {
                Value *elt_idxv = cast<Instruction>(elt_ptrv)->getOperand(0);
                ConstantInt *elt_idx = dyn_cast<ConstantInt>( elt_idxv );
                if( elt_idx )
                {   // direct access
                    uint eltid = (uint)elt_idx->getZExtValue();
                    // TODO: Disabling constant coalescing when we see that the offset to the constant buffer is negtive
                    // As we handle all negative offsets as uint and some arithmetic operations do not work well. Needs more detailed fix
                    if ((int32_t)eltid >= 0)
                    {
                        assert((eltid % 4) == 0);
                        eltid = (eltid >> 2); // bytes to dwords
                        MergeUniformLoad(inst, nullptr, addrSpace, nullptr, eltid, maxEltPlus, dircb_owloads);
                    }
                }
                else
                {   // indirect access
                    uint eltid = 0;
                    elt_idxv = SimpleBaseOffset(elt_idxv, eltid);
                    // TODO: Disabling constant coalescing when we see that the offset to the constant buffer is negtive
                    // As we handle all negative offsets as uint and some arithmetic operations do not work well. Needs more detailed fix
                    if ((int32_t)eltid >= 0)
                    {
                        assert((eltid % 4) == 0);
                        eltid = (eltid >> 2); // bytes to dwords
                        if (wiAns->whichDepend(inst) == WIAnalysis::UNIFORM)
                        {   // uniform
                            MergeUniformLoad(inst, nullptr, addrSpace, elt_idxv, eltid, maxEltPlus, indcb_owloads);
                        }
                        else
                        {   // not uniform
#ifdef SUPPORT_GATHER4
                            MergeScatterLoad(inst, nullptr, bufid, elt_idxv, eltid, 1, indcb_gathers);
#else
                            if (m_ctx->m_DriverInfo.UsesTypedConstantBuffers() && 
                                bufType == CONSTANT_BUFFER &&
                                !inst->getType()->isVectorTy())
                                ScatterToSampler(inst, nullptr, addrSpace, elt_idxv, eltid, indcb_gathers);
#endif
                        }
                    }
                }
            }
        }  // end of if gfx cbload handling
    } // loop over inst in block
}

/// check if two access have the same buffer-base
bool ConstantCoalescing::CompareBufferBase(Value *bufIdxV1, uint addrSpace1, Value *bufIdxV2, uint addrSpace2)
{
    if (bufIdxV1 == bufIdxV2)
    {
        return (addrSpace1 == addrSpace2);
    }
    if (bufIdxV1 && bufIdxV2)
    {
        // both are valid value, and they are not equal
        if (isa<PtrToIntInst>(bufIdxV1))
            bufIdxV1 = dyn_cast<PtrToIntInst>(bufIdxV1)->getOperand(0);
        if (isa<PtrToIntInst>(bufIdxV2))
            bufIdxV2 = dyn_cast<PtrToIntInst>(bufIdxV2)->getOperand(0);
        return (bufIdxV1 == bufIdxV2);
    }
    return false;
}

void ConstantCoalescing::MergeScatterLoad( Instruction *load,
                                Value *bufIdxV, uint addrSpace,
                                Value *eltIdxV, uint eltid, 
                                uint maxEltPlus,
                                std::vector<BufChunk*> &chunk_vec )
{
    BufChunk *cov_chunk = nullptr;
    for( std::vector<BufChunk*>::reverse_iterator rit = chunk_vec.rbegin(),
         rie = chunk_vec.rend(); rit != rie; ++rit )
    {
        BufChunk *cur_chunk = *rit;
        if (CompareBufferBase(cur_chunk->bufIdxV , cur_chunk->addrSpace, bufIdxV, addrSpace) &&
            cur_chunk->baseIdxV == eltIdxV &&
            cur_chunk->chunkIO->getType()->getScalarType() == load->getType()->getScalarType())
        {
            uint lb = std::min(eltid, cur_chunk->chunkStart);
            uint ub = std::max(eltid + maxEltPlus, cur_chunk->chunkStart + cur_chunk->chunkSize);
            if (ub - lb <= (SIZE_OWORD / 4))
            {
                cov_chunk = cur_chunk;
                break;
            }
        }
    }

    if( !cov_chunk )
    {
        cov_chunk = new BufChunk();
        cov_chunk->bufIdxV = bufIdxV;
        cov_chunk->addrSpace = addrSpace;
        cov_chunk->baseIdxV = eltIdxV;
        cov_chunk->chunkStart = eltid;
        cov_chunk->chunkSize = maxEltPlus;
        cov_chunk->chunkIO = load;
        chunk_vec.push_back( cov_chunk );
    }
    else if( !cov_chunk->chunkIO->getType()->isVectorTy() )
    {
        // combine the initial scalar loads with this incoming load (which can be a vector-load),
        // then add extracts
        CombineTwoLoads( cov_chunk, load, eltid, maxEltPlus );
    }
    else if (load->getType()->isVectorTy())
    {
        // just to modify all the extract, and connect it to the chunk-load
        uint lb = std::min(eltid, cov_chunk->chunkStart);
        uint ub = std::max(eltid + maxEltPlus, cov_chunk->chunkStart + cov_chunk->chunkSize);
        uint start_adj = cov_chunk->chunkStart - lb;
        uint size_adj = ub - lb - cov_chunk->chunkSize;
        if (start_adj == 0)
        {
            if (size_adj)
            {
                EnlargeChunk(cov_chunk, size_adj);
            }
        }
        else
        {
            AdjustChunk(cov_chunk, start_adj, size_adj);
        }
        MoveExtracts(cov_chunk, load, (eltid - cov_chunk->chunkStart));
    }
    else
    {
        Instruction *splitter = nullptr;
        uint start_adj = 0;
        uint size_adj = 0;
        if( eltid < cov_chunk->chunkStart )
        {
            start_adj = cov_chunk->chunkStart - eltid;
            size_adj = start_adj;
        }
        else if( eltid >= cov_chunk->chunkStart + cov_chunk->chunkSize )
        {
            size_adj = eltid - cov_chunk->chunkStart - cov_chunk->chunkSize + 1;
        }

        if( start_adj == 0 && size_adj == 0 )
        {
            splitter = FindOrAddChunkExtract( cov_chunk, eltid );
        }
        else if( start_adj > 0 )
        {
            splitter = AdjustChunkAddExtract( cov_chunk, start_adj, size_adj, eltid );
        }
        else if( size_adj > 0 )
        {
            splitter = EnlargeChunkAddExtract( cov_chunk, size_adj, eltid );
        }
        wiAns->incUpdateDepend( splitter, WIAnalysis::RANDOM );
        load->replaceAllUsesWith( splitter );
    }

}

Value *ConstantCoalescing::FormChunkAddress(BufChunk *chunk)
{
    assert(chunk->bufIdxV);
    WIAnalysis::WIDependancy uniformness = wiAns->whichDepend(chunk->bufIdxV);
    Value *eac = chunk->baseIdxV;
    if (chunk->chunkStart && chunk->baseIdxV)
    {
        assert(chunk->baseIdxV->getType()->isIntegerTy());
        Value *cv_start = ConstantInt::get(chunk->baseIdxV->getType(), chunk->chunkStart << 2);
        eac = irBuilder->CreateAdd(chunk->baseIdxV, cv_start);
        wiAns->incUpdateDepend(eac, wiAns->whichDepend(chunk->baseIdxV));
        if (wiAns->whichDepend(chunk->baseIdxV) != WIAnalysis::UNIFORM)
        {
            uniformness = WIAnalysis::RANDOM;
        }
    }
    Value *bufsrc = chunk->bufIdxV;
    if (chunk->bufIdxV->getType()->isPointerTy())
    {
        Type *ptrIntTy = dataLayout->getIntPtrType(cast<PointerType>(chunk->bufIdxV->getType()));
        bufsrc = irBuilder->CreatePtrToInt(chunk->bufIdxV, ptrIntTy);
        wiAns->incUpdateDepend(bufsrc, wiAns->whichDepend(chunk->bufIdxV));
    }
    else
    {
        assert(bufsrc->getType()->isIntegerTy());
    }
    if (eac)
    {
        if (eac->getType()->getPrimitiveSizeInBits() <
            bufsrc->getType()->getPrimitiveSizeInBits())
        {
            eac = irBuilder->CreateZExt(eac, bufsrc->getType());
            wiAns->incUpdateDepend(eac, uniformness);
        }
        assert(eac->getType() == bufsrc->getType());
        eac = irBuilder->CreateAdd(bufsrc, eac);
        wiAns->incUpdateDepend(eac, uniformness);
    }
    else if (chunk->chunkStart)
    {
        eac = ConstantInt::get(bufsrc->getType(), chunk->chunkStart << 2);
        eac = irBuilder->CreateAdd(bufsrc, eac);
        wiAns->incUpdateDepend(eac, uniformness);
    }
    else
    {
        eac = bufsrc;
    }
    return eac;
}

void ConstantCoalescing::CombineTwoLoads( BufChunk *cov_chunk, Instruction *load, uint eltid, uint numelt )
{
    uint eltid0 = cov_chunk->chunkStart;
    uint lb = std::min(eltid0, eltid);
    uint ub = std::max(eltid0, eltid + numelt - 1);
    cov_chunk->chunkStart = lb;
    cov_chunk->chunkSize = ub - lb  + 1;
    Instruction *load0 = cov_chunk->chunkIO;
    // remove redundant load
    if (cov_chunk->chunkSize <= 1)
    {
        load->replaceAllUsesWith(load0);
        return;
    }
    Type *vty = VectorType::get( cov_chunk->chunkIO->getType()->getScalarType(), cov_chunk->chunkSize );
    irBuilder->SetInsertPoint(load0);
    if(isa<LoadInst>(cov_chunk->chunkIO))
    {
        Value *addr_ptr = cov_chunk->chunkIO->getOperand(0);
        unsigned addrSpace = (cast<PointerType>(addr_ptr->getType()))->getAddressSpace();
        if(addrSpace == ADDRESS_SPACE_CONSTANT)
        {
            // no GEP, OCL path
            assert(isa<IntToPtrInst>(addr_ptr));
            Value *eac = cast<Instruction>(addr_ptr)->getOperand(0);
            // non-uniform, address must be non-uniform
            assert(isa<Instruction>(eac));
            // modify the address calculation if the chunk-start is changed
            if(eltid0 != cov_chunk->chunkStart)
            {
                eac = FormChunkAddress(cov_chunk);
            }
            // new IntToPtr and new load
            // cannot use irbuilder to create IntToPtr. It may create ConstantExpr instead of instruction
            Value *ptrcast = IntToPtrInst::Create(Instruction::IntToPtr, eac, PointerType::get(vty, addrSpace), "twoScalar", load0);
            m_TT->RegisterNewValueAndAssignID(ptrcast);
            wiAns->incUpdateDepend(ptrcast, WIAnalysis::RANDOM);
            cov_chunk->chunkIO = irBuilder->CreateLoad(ptrcast, false);
            cast<LoadInst>(cov_chunk->chunkIO)->setAlignment(4); // \todo, more precise
            wiAns->incUpdateDepend(cov_chunk->chunkIO, WIAnalysis::RANDOM);
        }
        else
        {
            assert(isa<IntToPtrInst>(addr_ptr));
            addr_ptr->mutateType(PointerType::get(vty, addrSpace));
            cov_chunk->chunkIO = irBuilder->CreateLoad(addr_ptr, false);
            cast<LoadInst>(cov_chunk->chunkIO)->setAlignment(4);  // \todo, more precise
            wiAns->incUpdateDepend(cov_chunk->chunkIO, WIAnalysis::RANDOM);
            // modify the address calculation if the chunk-start is changed
            if(eltid0 != cov_chunk->chunkStart)
            {
                assert(cov_chunk->baseIdxV);
                // src0 is the buffer base pointer, src1 is the address calculation
                Value *eac = cast<Instruction>(addr_ptr)->getOperand(0);
                assert(isa<Instruction>(eac));
                if(cast<Instruction>(eac)->getOpcode() == Instruction::Add ||
                    cast<Instruction>(eac)->getOpcode() == Instruction::Or)
                {
                    Value *cv_start = ConstantInt::get(irBuilder->getInt32Ty(), cov_chunk->chunkStart << 2);
                    cast<Instruction>(eac)->setOperand(1, cv_start);
                }
            }
        }
    }
    else
    {
        // bindless case
        assert(0 && "TODO");
    }

    // add two splitters
    Instruction *splitter;
    splitter = AddChunkExtract( cov_chunk->chunkIO, eltid0 - cov_chunk->chunkStart );
    load0->replaceAllUsesWith( splitter );
    wiAns->incUpdateDepend( splitter, WIAnalysis::RANDOM );
    if (numelt <= 1)
    {
        splitter = AddChunkExtract(cov_chunk->chunkIO, eltid - cov_chunk->chunkStart);
        load->replaceAllUsesWith(splitter);
        wiAns->incUpdateDepend(splitter, WIAnalysis::RANDOM);
    }
    else
    {
        // move all the extract from the input load to the chunk-load
        MoveExtracts(cov_chunk, load, eltid - cov_chunk->chunkStart);
    }
}

void ConstantCoalescing::MergeUniformLoad( Instruction *load,
                                Value *bufIdxV, uint addrSpace,
                                Value *eltIdxV, uint eltid, 
                                uint maxEltPlus,
                                std::vector<BufChunk*> &chunk_vec)
{
    BufChunk *cov_chunk = nullptr;
    for( std::vector<BufChunk*>::reverse_iterator rit = chunk_vec.rbegin(),
         rie = chunk_vec.rend(); rit != rie; ++rit )
    {
        BufChunk *cur_chunk = *rit;
        if (CompareBufferBase(cur_chunk->bufIdxV, cur_chunk->addrSpace, bufIdxV, addrSpace) &&
            cur_chunk->baseIdxV == eltIdxV &&
            cur_chunk->chunkIO->getType()->getScalarType() == load->getType()->getScalarType() )
        {
            uint lb = std::min(eltid, cur_chunk->chunkStart);
            uint ub = std::max(eltid + maxEltPlus, cur_chunk->chunkStart + cur_chunk->chunkSize);
            if(ub - lb <= MAX_OWLOAD_SIZE)
            {
                cov_chunk = cur_chunk;
                break;
            }
        }
    }
    if( !cov_chunk )
    {
        cov_chunk = new BufChunk();
        cov_chunk->bufIdxV = bufIdxV;
        cov_chunk->addrSpace = addrSpace;
        cov_chunk->baseIdxV = eltIdxV;
        cov_chunk->chunkStart = eltid;
        cov_chunk->chunkSize = iSTD::RoundPower2((DWORD)maxEltPlus);
        cov_chunk->chunkIO = CreateChunkLoad( load, cov_chunk, eltid );
        chunk_vec.push_back(cov_chunk);
    }
    else if (load->getType()->isVectorTy())
    {
        // just to modify all the extract, and connect it to the chunk-load
        uint lb = std::min(eltid, cov_chunk->chunkStart);
        uint ub = std::max(eltid + maxEltPlus, cov_chunk->chunkStart + cov_chunk->chunkSize);
        uint start_adj = cov_chunk->chunkStart - lb;
        uint size_adj = ub - lb - cov_chunk->chunkSize;
        // Gen only has 1, 2, 4 or 8 oword loads round up
        size_adj = iSTD::RoundPower2((DWORD)(size_adj + cov_chunk->chunkSize)) - cov_chunk->chunkSize;
        if (start_adj == 0)
        {
            if (size_adj)
                EnlargeChunk(cov_chunk, size_adj);
        }
        else
        {
            AdjustChunk(cov_chunk, start_adj, size_adj);
        }
        MoveExtracts(cov_chunk, load, eltid - cov_chunk->chunkStart);
    }
    else
    {
        Instruction *splitter = nullptr;
        uint start_adj = 0;
        uint size_adj = 0;
        if( eltid < cov_chunk->chunkStart )
        {
            start_adj = cov_chunk->chunkStart - eltid;
            size_adj = start_adj;
        }
        else if( eltid >= cov_chunk->chunkStart + cov_chunk->chunkSize )
        {
            size_adj = iSTD::RoundPower2((DWORD)(eltid + 1 - cov_chunk->chunkStart)) - cov_chunk->chunkSize;
        }
        // Gen only has 1, 2, 4 or 8 oword loads round up
        size_adj = iSTD::RoundPower2((DWORD)(size_adj + cov_chunk->chunkSize)) - cov_chunk->chunkSize;

        if( start_adj == 0 && size_adj == 0 )
        {
            splitter = FindOrAddChunkExtract( cov_chunk, eltid );
        }
        else if( start_adj > 0 )
        {
            splitter = AdjustChunkAddExtract( cov_chunk, start_adj, size_adj, eltid );
        }
        else if( size_adj > 0 )
        {
            splitter = EnlargeChunkAddExtract( cov_chunk, size_adj, eltid );
        }
        load->replaceAllUsesWith(splitter);
        wiAns->incUpdateDepend(splitter, WIAnalysis::UNIFORM);
    }
}

Value *ConstantCoalescing::SimpleBaseOffset( Value *elt_idxv, uint &offset )
{
    // in case expression comes from a smaller type arithmetic 
    if(ZExtInst* reducedOffset = dyn_cast<ZExtInst>(elt_idxv))
    {
        elt_idxv = reducedOffset->getOperand(0);
    }
    Instruction* expr = dyn_cast<Instruction>(elt_idxv);
    if( !expr )
    {
        offset = 0;
        return elt_idxv;
    }
    assert(!isa<IntToPtrInst>(expr));
    if( !expr->getType()->isIntegerTy() )
    {
        offset = 0;
        return elt_idxv;
    }
    if( expr->getOpcode() == Instruction::Add )
    {	  
        Value *src0 = expr->getOperand(0);
        Value *src1 = expr->getOperand(1);
        if( isa<ConstantInt>(src1) )
        {
            offset = (uint)cast<ConstantInt>(src1)->getZExtValue();
            return src0;
        }
        else if( isa<ConstantInt>(src0) )
        {
            offset = (uint)cast<ConstantInt>(src0)->getZExtValue();
            return src1;
        }
    }
    else if( expr->getOpcode() == Instruction::Or )
    {
        Value *src0 = expr->getOperand(0);
        Value *src1 = expr->getOperand(1);
        Instruction* or_inst0 = dyn_cast<Instruction>(src0);
        ConstantInt* or_csrc1 = dyn_cast<ConstantInt>(src1);
        if (or_inst0 && or_csrc1)
        {
            uint or_offset = int_cast<uint>(or_csrc1->getZExtValue());

            Instruction* inst0 = or_inst0;
            unsigned inst0_op = inst0->getOpcode();                        
            
            offset = or_offset;

            // Example of pattern handled below:
            //  %27 = shl i32 %26, 4
            //  %168 = add i32 %27, 32
            //  %fromGBP33 = inttoptr i32 %168 to float addrspace(65538)*
            //  %ldrawidx34 = load float, float addrspace(65538)* %fromGBP33, align 16
            //  %173 = or i32 %168, 4
            //  %fromGBP35 = inttoptr i32 %173 to float addrspace(65538)*
            //  %ldrawidx36 = load float, float addrspace(65538)* %fromGBP35, align 4
            if (inst0_op == Instruction::Add &&
                isa<Instruction>(inst0->getOperand(0)) &&
                isa<ConstantInt>(inst0->getOperand(1)))
            {
                uint add_offset = int_cast<uint>(cast<ConstantInt>(inst0->getOperand(1))->getZExtValue());
                if (or_offset < add_offset && 
                    ((iSTD::RoundPower2((DWORD)or_offset) - 1) & add_offset) == 0)
                {
                    offset = or_offset + add_offset;
                    src0 = inst0->getOperand(0);
                    inst0 = cast<Instruction>(src0);                    
                    inst0_op = inst0->getOpcode();
                }
            }

            if (inst0_op == Instruction::And ||
                inst0_op == Instruction::Mul)
            {   
                Value* ptr_adj = inst0->getOperand(1);
                if (ConstantInt* adj_val = cast<ConstantInt>(ptr_adj))
                {
                    uint imm = (uint)adj_val->getZExtValue();
                    if ((inst0_op == Instruction::And && or_offset < int_cast<unsigned int>(1 << iSTD::bsf(imm))) ||
                        (inst0_op == Instruction::Mul && or_offset < imm))
                    {
                        return src0;
                    }
                }
            }
            else if (inst0_op == Instruction::Shl)
            {
                Value* ptr_adj = inst0->getOperand(1);
                if (ConstantInt* adj_val = dyn_cast<ConstantInt>(ptr_adj))
                {
                    uint imm = (uint)adj_val->getZExtValue();
                    uint shl_base = 1;
                    if (Instruction* shl_inst0 = dyn_cast<Instruction>(inst0->getOperand(0)))
                    {
                        uint shl_inst0_op = shl_inst0->getOpcode();
                        if (shl_inst0_op == Instruction::And && isa<ConstantInt>(shl_inst0->getOperand(1)))
                        {
                            ConstantInt* and_mask_val = cast<ConstantInt>(shl_inst0->getOperand(1));
                            uint and_mask = (uint)and_mask_val->getZExtValue();
                            shl_base = (1 << iSTD::bsf(and_mask));
                        }
                    }

                    if (or_offset < int_cast<unsigned int>(shl_base << imm))
                    {
                        return src0;
                    }
                }
            }
        }
    }
    offset = 0;
    return elt_idxv;
}

bool ConstantCoalescing::DecomposePtrExp(Value *ptr_val, Value*& buf_idxv, Value*& elt_idxv, uint &offset)
{
    if (isa<Argument>(ptr_val))
    {
        buf_idxv = ptr_val;
        elt_idxv = nullptr;
        offset = 0;
        return true;
    }
    IntToPtrInst *i2p = dyn_cast<IntToPtrInst>(ptr_val);
    if (!i2p)
        return false;
    // get the int-type address computation
    Instruction *expr = dyn_cast<Instruction>(i2p->getOperand(0));
    if (!expr || !expr->getType()->isIntegerTy())
    {
        return false;
    }
    // look for the buf_idxv from a ptr2int
    if (isa<PtrToIntInst>(expr))
    {
        buf_idxv = expr;
        elt_idxv = nullptr;
        offset = 0;
        return true;
    }
    if (expr->getOpcode() == Instruction::Add)
    {
        Value *src0 = expr->getOperand(0);
        Value *src1 = expr->getOperand(1);
        if (isa<PtrToIntInst>(src0))
        {
            buf_idxv = src0;
            ConstantInt *elt_idx = dyn_cast<ConstantInt>(src1);
            if (elt_idx)
            {   // direct access
                offset = (uint)elt_idx->getZExtValue();
                elt_idxv = nullptr;
            }
            else
            {
                elt_idxv = SimpleBaseOffset(src1, offset);
            }
            if (offset % 4)
                return false;
            offset = offset >> 2; // convert from byte to dword
            return true;
        }
        else if (isa<PtrToIntInst>(src1))
        {
            buf_idxv = src1;
            ConstantInt *elt_idx = dyn_cast<ConstantInt>(src0);
            if (elt_idx)
            {   // direct access
                offset = (uint)elt_idx->getZExtValue();
                elt_idxv = nullptr;
            }
            else
            {
                elt_idxv = SimpleBaseOffset(src0, offset);
            }
            if (offset % 4)
                return false;
            offset = offset >> 2;
            return true;
        }
        else if (ConstantInt *elt_idx = dyn_cast<ConstantInt>(src1))
        {
            offset = (uint)elt_idx->getZExtValue();
            if (offset % 4)
                return false;
            offset = offset >> 2;
            elt_idxv = nullptr;
            buf_idxv = src0;
            return true;
        }
        else if (ConstantInt *elt_idx = dyn_cast<ConstantInt>(src0))
        {
            offset = (uint)elt_idx->getZExtValue();
            if (offset % 4)
                return false;
            offset = offset >> 2;
            elt_idxv = nullptr;
            buf_idxv = src1;
            return true;
        }
    }
    buf_idxv = expr;
    elt_idxv = nullptr;
    offset = 0;
    return true;
}

/// look at all the uses of a vector load. If they are all extract-elements,
uint ConstantCoalescing::CheckVectorElementUses(Instruction *load)
{
    uint maxEltPlus = 0;
    for (auto I = load->user_begin(), E = load->user_end(); (I != E); ++I)
    {
        if (llvm::ExtractElementInst* extract = llvm::dyn_cast<llvm::ExtractElementInst>(*I))
        {
            if (llvm::ConstantInt* index = llvm::dyn_cast<ConstantInt>(extract->getIndexOperand()))
            {
                uint cv = static_cast<uint>(index->getZExtValue());
                if (cv + 1 > maxEltPlus)
                    maxEltPlus = cv + 1;
            }
            else
            {
                return 0;
            }
        }
        else
        {
            return 0;
        }
    }
    return maxEltPlus;
}

Instruction *ConstantCoalescing::CreateChunkLoad(Instruction *seedi, BufChunk *chunk, uint eltid)
{
    irBuilder->SetInsertPoint(seedi);
    if(LoadInst *load = dyn_cast<LoadInst>(seedi))
    {
        LoadInst* chunkLoad;
        Value *cb_ptr = load->getPointerOperand();

        Value *eac;
        // ocl case: no gep
        if(load->getPointerAddressSpace() == ADDRESS_SPACE_CONSTANT)
        {
            eac = cb_ptr;
            if(eltid == chunk->chunkStart && isa<IntToPtrInst>(eac))
                eac = dyn_cast<IntToPtrInst>(eac)->getOperand(0);
            else
                eac = FormChunkAddress(chunk);
        }
        else
        {
            // gfx case
            eac = ConstantInt::get(irBuilder->getInt32Ty(), chunk->chunkStart << 2);
            if(chunk->baseIdxV)
            {
                if(chunk->chunkStart)
                {
                    eac = irBuilder->CreateAdd(chunk->baseIdxV, eac);
                    wiAns->incUpdateDepend(eac, WIAnalysis::UNIFORM);
                }
                else
                {
                    eac = chunk->baseIdxV;
                }
            }
        }
        Type *vty = VectorType::get(load->getType()->getScalarType(), chunk->chunkSize);
        unsigned addrSpace = (cast<PointerType>(cb_ptr->getType()))->getAddressSpace();
        PointerType* pty = PointerType::get(vty, addrSpace);
        // cannot use irbuilder to create IntToPtr. It may create ConstantExpr instead of instruction
        Instruction *ptr = IntToPtrInst::Create(Instruction::IntToPtr, eac, pty, "chunkPtr", seedi);
        m_TT->RegisterNewValueAndAssignID(ptr);
        // Update debug location
        ptr->setDebugLoc(irBuilder->getCurrentDebugLocation());
        wiAns->incUpdateDepend(ptr, WIAnalysis::UNIFORM);
        chunkLoad = irBuilder->CreateLoad(ptr);
        chunkLoad->setAlignment(4);  // \todo, need to be more precise
        chunk->chunkIO = chunkLoad;
    }
    else
    {
        // bindless case
        LdRawIntrinsic* ldRaw = cast<LdRawIntrinsic>(seedi);
        Value* eac = irBuilder->getInt32(chunk->chunkStart << 2);
        if(chunk->baseIdxV)
        {
            if(chunk->chunkStart)
            {
                eac = irBuilder->CreateAdd(chunk->baseIdxV, eac);
                wiAns->incUpdateDepend(eac, WIAnalysis::UNIFORM);
            }
            else
            {
                eac = chunk->baseIdxV;
            }
        }
        Type *vty = VectorType::get(ldRaw->getType()->getScalarType(), chunk->chunkSize);
        Type* types[] =
        {
            vty,
            ldRaw->getResourceValue()->getType(),
        };
        Function* ldRawFn = GenISAIntrinsic::getDeclaration(
            curFunc->getParent(),
            GenISAIntrinsic::GenISA_ldrawvector_indexed,
            types);
        chunk->chunkIO = irBuilder->CreateCall2(ldRawFn, ldRaw->getResourceValue(), eac);
    }

    wiAns->incUpdateDepend(chunk->chunkIO, WIAnalysis::UNIFORM);

    if(!seedi->getType()->isVectorTy())
    {
        Instruction *splitter = AddChunkExtract(chunk->chunkIO, eltid - chunk->chunkStart);
        seedi->replaceAllUsesWith(splitter);
        wiAns->incUpdateDepend(splitter, WIAnalysis::UNIFORM);
    }
    else
    {
        MoveExtracts(chunk, seedi, eltid - chunk->chunkStart);
    }
    return chunk->chunkIO;
}

Instruction *ConstantCoalescing::AddChunkExtract( Instruction *load, uint eltid )
{
    irBuilder->SetInsertPoint( load->getNextNode() );
    Value* cv_eltid = ConstantInt::get( irBuilder->getInt32Ty(), eltid );
    Value* extract = irBuilder->CreateExtractElement( load, cv_eltid );
    wiAns->incUpdateDepend(extract, wiAns->whichDepend(load));
    return cast<Instruction>( extract );
}

Instruction *ConstantCoalescing::FindOrAddChunkExtract( BufChunk *cov_chunk, uint eltid )
{
    Instruction *splitter = nullptr;
    // look for the splitter in existing uses
    Value::user_iterator use_it = cov_chunk->chunkIO->user_begin();
    Value::user_iterator use_e  = cov_chunk->chunkIO->user_end();
    for( ; use_it != use_e; ++use_it )
    {
        Instruction *usei = dyn_cast<Instruction>(*use_it);
        assert( usei && isa<ExtractElementInst>(usei) );
        ConstantInt *e_idx = dyn_cast<ConstantInt>( usei->getOperand(1) );
        assert( e_idx );
        uint val = (uint)e_idx->getZExtValue();
        if( val == eltid - cov_chunk->chunkStart )
        {
            splitter = usei;
            break;
        }
    }
    // if a splitter does not exist, add a new splitter
    if( !splitter )
    {
        splitter = AddChunkExtract( cov_chunk->chunkIO, eltid - cov_chunk->chunkStart );
    }
    return splitter;
}

void ConstantCoalescing::AdjustChunk( BufChunk *cov_chunk, uint start_adj, uint size_adj)
{
    cov_chunk->chunkSize += size_adj;
    cov_chunk->chunkStart -= start_adj;
    // mutateType to change array-size
    Type* originalType = cov_chunk->chunkIO->getType();
    Type *vty = VectorType::get( cov_chunk->chunkIO->getType()->getScalarType(), cov_chunk->chunkSize );
    cov_chunk->chunkIO->mutateType( vty );
    // change the dest ptr-type on bitcast
    if(isa<LoadInst>(cov_chunk->chunkIO))
    {
        Value *addr_ptr = cov_chunk->chunkIO->getOperand(0);
        unsigned addrSpace = (cast<PointerType>(addr_ptr->getType()))->getAddressSpace();
        if(addrSpace == ADDRESS_SPACE_CONSTANT)
        {
            // ocl path
            assert(isa<IntToPtrInst>(addr_ptr));
            irBuilder->SetInsertPoint(dyn_cast<Instruction>(addr_ptr));
            addr_ptr->mutateType(PointerType::get(vty, addrSpace));
            Value *eac = cast<Instruction>(addr_ptr)->getOperand(0);
            Instruction *expr = dyn_cast<Instruction>(eac);
            bool foundOffset = false;
            if(expr && expr->getOpcode() == Instruction::Add && expr->hasOneUse())
            {
                uint srcIdx = 2;
                if(expr->getOperand(0) == cov_chunk->bufIdxV)
                    srcIdx = 1;
                else if(expr->getOperand(1) == cov_chunk->bufIdxV)
                    srcIdx = 0;
                if(srcIdx == 0 || srcIdx == 1)
                {
                    if(isa<ConstantInt>(expr->getOperand(srcIdx)))
                    {
                        Value *cv_start = ConstantInt::get(expr->getType(), cov_chunk->chunkStart << 2);
                        expr->setOperand(srcIdx, cv_start);
                        foundOffset = true;
                    }
                    else
                    {
                        Instruction *expr2 = dyn_cast<Instruction>(expr->getOperand(srcIdx));
                        if(expr2 && expr2->hasOneUse())
                        {
                            if (isa<ZExtInst>(expr2) && isa<BinaryOperator>(expr2->getOperand(0)))
                                expr2 = cast<Instruction>(expr2->getOperand(0));
                            assert(isa<BinaryOperator>(expr2));

                            if(expr2->getOperand(0) == cov_chunk->baseIdxV &&
                                isa<ConstantInt>(expr2->getOperand(1)))
                            {
                                Value *cv_start = ConstantInt::get(expr2->getType(), cov_chunk->chunkStart << 2);
                                expr2->setOperand(1, cv_start);
                                foundOffset = true;
                            }
                            else if(expr2->getOperand(1) == cov_chunk->baseIdxV &&
                                isa<ConstantInt>(expr2->getOperand(0)))
                            {
                                Value *cv_start = ConstantInt::get(expr->getType(), cov_chunk->chunkStart << 2);
                                expr2->setOperand(0, cv_start);
                                foundOffset = true;
                            }
                        }
                    }
                }
            }
            if(!foundOffset)
            {
                // if we cannot modify the offset, create a new chain of address calculation
                eac = FormChunkAddress(cov_chunk);
                cast<Instruction>(addr_ptr)->setOperand(0, eac);
            }
        }
        else
        {
            // gfx path
            assert(isa<IntToPtrInst>(addr_ptr));
            addr_ptr->mutateType(PointerType::get(vty, addrSpace));
            if(cov_chunk->baseIdxV == nullptr)
            {
                Value *cv_start = ConstantInt::get(irBuilder->getInt32Ty(), cov_chunk->chunkStart << 2);
                cast<Instruction>(addr_ptr)->setOperand(0, cv_start);
            }
            else
            {
                Value *eac = cast<Instruction>(addr_ptr)->getOperand(0);
                assert(isa<Instruction>(eac));
                assert(cast<Instruction>(eac)->getOpcode() == Instruction::Add ||
                    cast<Instruction>(eac)->getOpcode() == Instruction::Or);
                Value *cv_start = ConstantInt::get(irBuilder->getInt32Ty(), cov_chunk->chunkStart << 2);
                cast<Instruction>(eac)->setOperand(1, cv_start);
            }
        }
    }
    else
    {
        // bindless case
        LdRawIntrinsic* ldRaw = cast<LdRawIntrinsic>(cov_chunk->chunkIO);
        Type* types[] =
        {
            vty,
            ldRaw->getResourceValue()->getType(),
        };
        Function* ldRawFn = GenISAIntrinsic::getDeclaration(
            curFunc->getParent(),
            GenISAIntrinsic::GenISA_ldrawvector_indexed,
            types);
        ldRaw->setCalledFunction(ldRawFn);
        ldRaw->mutateType(vty);
        if(cov_chunk->baseIdxV == nullptr)
        {
            Value *cv_start = irBuilder->getInt32(cov_chunk->chunkStart << 2);
            ldRaw->setOffsetValue(cv_start);
        }
        else
        {
            Value *eac = ldRaw->getOffsetValue();
            assert(isa<Instruction>(eac));
            assert(cast<Instruction>(eac)->getOpcode() == Instruction::Add ||
                cast<Instruction>(eac)->getOpcode() == Instruction::Or);
            Value *cv_start = irBuilder->getInt32(cov_chunk->chunkStart << 2);
            cast<Instruction>(eac)->setOperand(1, cv_start);
        }
    }

    SmallVector<Instruction*, 4> use_set;
    // adjust all the splitters
    Value::user_iterator use_it = cov_chunk->chunkIO->user_begin();
    Value::user_iterator use_e = cov_chunk->chunkIO->user_end();
    for(; use_it != use_e; ++use_it)
    {
        if(ExtractElementInst *usei = dyn_cast<ExtractElementInst>(*use_it))
        {
            if(llvm::ConstantInt *e_idx = llvm::dyn_cast<llvm::ConstantInt>(usei->getOperand(1)))
            {
                uint val = (uint)e_idx->getZExtValue();
                val += start_adj;
                // update the index source
                e_idx = ConstantInt::get(irBuilder->getInt32Ty(), val);
                usei->setOperand(1, e_idx);
                continue;
            }
        }
        use_set.push_back(llvm::cast<Instruction>(*use_it));
    }
    if(use_set.size() > 0)
    {
        WIAnalysis::WIDependancy loadDep = wiAns->whichDepend(cov_chunk->chunkIO);
        irBuilder->SetInsertPoint(cov_chunk->chunkIO->getNextNode());
        Value* vec = UndefValue::get(originalType);
        for(unsigned i = 0; i < originalType->getVectorNumElements(); i++)
        {
            Value* channel = irBuilder->CreateExtractElement(
                cov_chunk->chunkIO, irBuilder->getInt32(i + start_adj));
            wiAns->incUpdateDepend(channel, loadDep);
            vec = irBuilder->CreateInsertElement(vec, channel, irBuilder->getInt32(i));
            wiAns->incUpdateDepend(vec, loadDep);
        }
        for(auto it : use_set)
        {
            it->replaceUsesOfWith(cov_chunk->chunkIO, vec);
        }
    }
}

Instruction *ConstantCoalescing::AdjustChunkAddExtract(BufChunk *cov_chunk, uint start_adj, uint size_adj, uint eltid)
{
    AdjustChunk(cov_chunk, start_adj, size_adj);
    return AddChunkExtract(cov_chunk->chunkIO, eltid - cov_chunk->chunkStart);
}

void ConstantCoalescing::MoveExtracts(BufChunk *cov_chunk, Instruction *load, uint start_adj)
{
    // modify the extract-elements from the load, and move it to the chunk
    Value::user_iterator use_it = load->user_begin();
    Value::user_iterator use_e = load->user_end();
    bool noneDirectExtract = false; 
    std::vector<Instruction*> use_set;
    for (; use_it != use_e; ++use_it)
    {
        Instruction *usei = cast<Instruction>(*use_it);
        if(!isa<ExtractElementInst>(usei) || !isa<ConstantInt>(usei->getOperand(1)))
        {
            noneDirectExtract = true;
            break;
        }
        use_set.push_back(usei);
    }
    if(noneDirectExtract == false)
    {
        uint num_uses = use_set.size();
        for(uint i = 0; i < num_uses; ++i)
        {
            Instruction *usei = use_set[i];
            if(start_adj)
            {
                llvm::ConstantInt *e_idx = cast<ConstantInt>(usei->getOperand(1));
                uint val = (uint)e_idx->getZExtValue();
                val += start_adj;
                // update the index source
                e_idx = ConstantInt::get(irBuilder->getInt32Ty(), val);
                usei->setOperand(1, e_idx);
            }
            usei->setOperand(0, cov_chunk->chunkIO);
        }
    }
    else
    {
        if(start_adj || cov_chunk->chunkIO->getType() != load->getType())
        {
            WIAnalysis::WIDependancy loadDep = wiAns->whichDepend(load);
            irBuilder->SetInsertPoint(load->getNextNode());
            Type* vecType = load->getType();
            Value* vec = UndefValue::get(vecType);
            for(unsigned i = 0; i < vecType->getVectorNumElements(); i++)
            {
                Value* channel = irBuilder->CreateExtractElement(
                    cov_chunk->chunkIO, irBuilder->getInt32(i + start_adj));
                wiAns->incUpdateDepend(channel, loadDep);
                vec = irBuilder->CreateInsertElement(vec, channel, irBuilder->getInt32(i));
                wiAns->incUpdateDepend(vec, loadDep);
            }
            load->replaceAllUsesWith(vec);
        }
        else
        {
            load->replaceAllUsesWith(cov_chunk->chunkIO);
        }
    }
}

void ConstantCoalescing::EnlargeChunk(BufChunk *cov_chunk, uint size_adj)
{
    cov_chunk->chunkSize += size_adj;
    // mutateType to change array-size
    Type* originalType = cov_chunk->chunkIO->getType();
    Type *vty = VectorType::get(cov_chunk->chunkIO->getType()->getScalarType(), cov_chunk->chunkSize);
    cov_chunk->chunkIO->mutateType(vty);
    if(isa<LoadInst>(cov_chunk->chunkIO))
    {
        // change the dest ptr-type on bitcast
        Value *addr_ptr = cov_chunk->chunkIO->getOperand(0);
        unsigned addrSpace = (cast<PointerType>(addr_ptr->getType()))->getAddressSpace();
        if(isa<BitCastInst>(addr_ptr) || isa<IntToPtrInst>(addr_ptr))
        {
            addr_ptr->mutateType(PointerType::get(vty, addrSpace));
        }
    }
    else
    {
        LdRawIntrinsic* ldRaw = cast<LdRawIntrinsic>(cov_chunk->chunkIO);
        Type* types[] =
        {
            vty,
            ldRaw->getResourceValue()->getType(),
        };
        Function* ldRawFn = GenISAIntrinsic::getDeclaration(
            curFunc->getParent(),
            GenISAIntrinsic::GenISA_ldrawvector_indexed,
            types);
        ldRaw->setCalledFunction(ldRawFn);
    }
    // for none extract uses correct the use
    Value::user_iterator use_it = cov_chunk->chunkIO->user_begin();
    Value::user_iterator use_e = cov_chunk->chunkIO->user_end();
    SmallVector<Instruction*, 4> use_set;
    for(; use_it != use_e; ++use_it)
    {
        if(!isa<ExtractElementInst>(*use_it))
        {
            use_set.push_back(cast<Instruction>(*use_it));
        }
    }
    if(use_set.size() > 0)
    {
        WIAnalysis::WIDependancy loadDep = wiAns->whichDepend(cov_chunk->chunkIO);
        irBuilder->SetInsertPoint(cov_chunk->chunkIO->getNextNode());
        Value* vec = UndefValue::get(originalType);
        for(unsigned i = 0; i < originalType->getVectorNumElements(); i++)
        {
            Value* channel = irBuilder->CreateExtractElement(
                cov_chunk->chunkIO, irBuilder->getInt32(i));
            wiAns->incUpdateDepend(channel, loadDep);
            vec = irBuilder->CreateInsertElement(vec, channel, irBuilder->getInt32(i));
            wiAns->incUpdateDepend(vec, loadDep);
        }
        for(auto it : use_set)
        {
            it->replaceUsesOfWith(cov_chunk->chunkIO, vec);
        }
    }
}

Instruction *ConstantCoalescing::EnlargeChunkAddExtract( BufChunk *cov_chunk, uint size_adj, uint eltid )
{
    EnlargeChunk(cov_chunk, size_adj);
    // add a new splitter
    return AddChunkExtract( cov_chunk->chunkIO, eltid - cov_chunk->chunkStart );
}


// Returns true if input address is 16 bytes aligned.
bool ConstantCoalescing::IsSamplerAlignedAddress(Value* addr) const
{
    Instruction* inst = dyn_cast<Instruction>(addr);
    if (inst &&
        (inst->getOpcode() == Instruction::Shl ||
         inst->getOpcode() == Instruction::Mul ||
         inst->getOpcode() == Instruction::And ||
         inst->getOpcode() == Instruction::Add))
    {
        ConstantInt* src1ConstVal = dyn_cast<ConstantInt>(inst->getOperand(1));
        unsigned int constant = src1ConstVal ? int_cast<unsigned int>(src1ConstVal->getZExtValue()) : 0;

        if (inst->getOpcode() == Instruction::Shl && src1ConstVal)
        {
            if (constant >= 4)
            {
                return true;
            }
        }
        else if (inst->getOpcode() == Instruction::Mul && src1ConstVal)
        {
            if ((constant % 16) == 0)
            {
                return true;
            }
        }
        else if (inst->getOpcode() == Instruction::And && src1ConstVal)
        {
            if ((constant & 0xf) == 0)
            {
                return true;
            }
        }
        else if (inst->getOpcode() == Instruction::Add)
        {
            if (IsSamplerAlignedAddress(inst->getOperand(0)) &&
                IsSamplerAlignedAddress(inst->getOperand(1)))
            {
                return true;
            }
        }
    }
    else if (ConstantInt* constant = dyn_cast<ConstantInt>(addr))
    {
        if ((int_cast<uint>(constant->getZExtValue()) % 16) == 0)
        {
            return true;
        }
    }
    return false;
}


// Calculates the address in 4DWORD units, ready to be used in 
// sampler ld message. Input value is in bytes.
Value* ConstantCoalescing::GetSamplerAlignedAddress(Value* addr)
{ 
    assert(IsSamplerAlignedAddress(addr));

    Value* elementIndex = nullptr;
    Instruction* inst = dyn_cast<Instruction>(addr);

    if (inst &&
        (inst->getOpcode() == Instruction::Shl ||
         inst->getOpcode() == Instruction::Mul ||
         inst->getOpcode() == Instruction::And) &&
        isa<ConstantInt>(inst->getOperand(1)))
    {
        irBuilder->SetInsertPoint(inst);
        unsigned int constant = int_cast<unsigned int>(cast<ConstantInt>(inst->getOperand(1))->getZExtValue());
        elementIndex = inst->getOperand(0);

        if (inst->getOpcode() == Instruction::Shl)
        {
            assert(constant >= 4);

            if (constant > 4 && constant < 32)
            {
                elementIndex = irBuilder->CreateShl(elementIndex, irBuilder->getInt32(constant - 4));
                wiAns->incUpdateDepend(elementIndex, WIAnalysis::RANDOM);
            }
        }
        else if (inst->getOpcode() == Instruction::Mul)
        {
            assert(constant % 16 == 0);

            if (constant != 16)
            {
                elementIndex = irBuilder->CreateMul(elementIndex, irBuilder->getInt32(constant / 16));
                wiAns->incUpdateDepend(elementIndex, WIAnalysis::RANDOM);

            }
        }
        else
        {
            assert(inst->getOpcode() == Instruction::And);
            assert((constant & 0xf) == 0);

            elementIndex = irBuilder->CreateLShr(elementIndex, irBuilder->getInt32(4));
            wiAns->incUpdateDepend(elementIndex, WIAnalysis::RANDOM);

            if (constant != 0xFFFFFFF0)
            {
                elementIndex = irBuilder->CreateAnd(elementIndex, irBuilder->getInt32((constant >> 4)));
                wiAns->incUpdateDepend(elementIndex, WIAnalysis::RANDOM);
            }
        }
    }
    else if (inst &&
             inst->getOpcode() == Instruction::Add)
    {
        Value* a = GetSamplerAlignedAddress(inst->getOperand(0));
        Value* b = GetSamplerAlignedAddress(inst->getOperand(1));

        irBuilder->SetInsertPoint(inst);

        elementIndex = irBuilder->CreateAdd(a, b);
        wiAns->incUpdateDepend(elementIndex, WIAnalysis::RANDOM);
    }
    else if (ConstantInt* constant = dyn_cast<ConstantInt>(addr))
    {
        uint offset = int_cast<uint>(constant->getZExtValue());
        assert((offset % 16) == 0);

        elementIndex = irBuilder->getInt32(offset / 16);
    }

    return elementIndex;
}

/// replace non-uniform scatter load with sampler load messages
void ConstantCoalescing::ScatterToSampler( Instruction *load,
                                      Value *bufIdxV, uint addrSpace,
                                      Value *eltIdxV, uint eltid, 
                                      std::vector<BufChunk*> &chunk_vec )
{
    Instruction* ishl = dyn_cast<Instruction>(eltIdxV);
    if( ishl != nullptr && IsSamplerAlignedAddress(ishl))
    {
        irBuilder->SetInsertPoint( ishl );

        Value* elementIndex = GetSamplerAlignedAddress(ishl);

        WIAnalysis::WIDependancy ishlDep = wiAns->whichDepend(ishl);
        // it is possible that ishl is uniform, yet load is non-uniform due to the use location of load
        if (elementIndex != ishl->getOperand(0))
        {
            Value* newVal = irBuilder->CreateShl(elementIndex, irBuilder->getInt32(4));
            wiAns->incUpdateDepend(newVal, ishlDep);
            ishl->replaceAllUsesWith(newVal);
        }
        else if (wiAns->whichDepend(elementIndex) != ishlDep)
        {
            // quick fix for a special case: elementIndex is uniform and ishl is not uniform.
            // If we use ishl-src0 (elementIndx) directly at cf-join point by this transform,
            // we can change the uniformness of elementIndex
            elementIndex = irBuilder->CreateShl(elementIndex, irBuilder->getInt32(0));
            wiAns->incUpdateDepend(elementIndex, ishlDep);
            Value* newVal = irBuilder->CreateShl(elementIndex, irBuilder->getInt32(4));
            wiAns->incUpdateDepend(newVal, ishlDep);
            ishl->replaceAllUsesWith(newVal);
        }
        BufChunk *cov_chunk = nullptr;
        for( std::vector<BufChunk*>::reverse_iterator rit = chunk_vec.rbegin(),
            rie = chunk_vec.rend(); rit != rie; ++rit )
        {
            BufChunk *cur_chunk = *rit;
            if (CompareBufferBase(cur_chunk->bufIdxV, cur_chunk->addrSpace, bufIdxV, addrSpace) &&
                cur_chunk->baseIdxV == elementIndex &&
                cur_chunk->chunkIO->getType()->getScalarSizeInBits() == load->getType()->getScalarSizeInBits())
            {
               if(eltid>= cur_chunk->chunkStart && eltid<cur_chunk->chunkStart + cur_chunk->chunkSize)
               {                
                   cov_chunk = cur_chunk;
                   break;
               }
            }
        }
        irBuilder->SetInsertPoint( load );
        Instruction* ld = nullptr;
        if( !cov_chunk )
        {
            cov_chunk = new BufChunk();
            cov_chunk->bufIdxV = bufIdxV;
            cov_chunk->addrSpace = addrSpace;
            cov_chunk->baseIdxV = elementIndex;
            cov_chunk->chunkStart = iSTD::RoundDown(eltid,4);
            cov_chunk->chunkSize = 4;
            if(eltid>=4)
            {
                elementIndex = irBuilder->CreateAdd(elementIndex, irBuilder->getInt32(eltid/4));
                wiAns->incUpdateDepend( elementIndex, WIAnalysis::RANDOM );
            }
            elementIndex = irBuilder->CreateAnd(elementIndex, irBuilder->getInt32(0x0FFFFFFF));
            wiAns->incUpdateDepend(elementIndex, WIAnalysis::RANDOM);
            ld = CreateSamplerLoad(elementIndex, addrSpace);
            cov_chunk->chunkIO = ld;
            chunk_vec.push_back( cov_chunk );
        }
        else
        {
            ld = cov_chunk->chunkIO;
        }

        Instruction *splitter = 
            cast<Instruction>(irBuilder->CreateExtractElement(ld, irBuilder->getInt32(eltid%4)));
        wiAns->incUpdateDepend( splitter, WIAnalysis::RANDOM );
        if(splitter->getType() != load->getType()->getScalarType())
        {
            splitter = cast<Instruction>(irBuilder->CreateBitCast(splitter, load->getType()->getScalarType()));
            wiAns->incUpdateDepend( splitter, WIAnalysis::RANDOM );
        }
        load->replaceAllUsesWith( splitter );
    }
}

Instruction* ConstantCoalescing::CreateSamplerLoad(Value* index, uint addrSpace)
{
    unsigned int AS = addrSpace;
    PointerType* resourceType = PointerType::get(irBuilder->getFloatTy(), AS);
    Type* types[] = { llvm::VectorType::get(irBuilder->getFloatTy(), 4), resourceType };
    Function* l = GenISAIntrinsic::getDeclaration(curFunc->getParent(),
        llvm::GenISAIntrinsic::GenISA_ldptr, 
        types);
    Value* attr[] =
    {
        index,
        irBuilder->getInt32(0),
        irBuilder->getInt32(0),
        irBuilder->getInt32(0),
        ConstantPointerNull::get(resourceType),
        irBuilder->getInt32(0),
        irBuilder->getInt32(0),
        irBuilder->getInt32(0),
    };
    Instruction* ld = irBuilder->CreateCall(l, attr);
    wiAns->incUpdateDepend( ld, WIAnalysis::RANDOM );
    return ld;
}


/// change GEP to oword-based for oword-aligned load in order to avoid SHL
void ConstantCoalescing::ChangePTRtoOWordBased(BufChunk *chunk)
{
    assert(chunk && chunk->chunkIO);
    LoadInst *load = dyn_cast<LoadInst>(chunk->chunkIO);
    assert(load);
    // has to be a 3d-load for now.
    // Argument pointer coming from OCL may not be oword-aligned
    uint addrSpace = load->getPointerAddressSpace();
    if (addrSpace == ADDRESS_SPACE_CONSTANT)
    {
        return;
    }
    // element index must be a SHL-by-4
    // chunk-start must be oword-aligned
    if (!(chunk->baseIdxV) || chunk->chunkStart % 4)
    {
        return;
    }
    Instruction *ishl = dyn_cast<Instruction>(chunk->baseIdxV);
    if (!ishl ||
        ishl->getOpcode() != Instruction::Shl ||
        !isa<ConstantInt>(ishl->getOperand(1)))
    {
        return;
    }
    unsigned int constant = int_cast<unsigned int>(cast<ConstantInt>(ishl->getOperand(1))->getZExtValue());
    if (constant != 4)
    {
        return;
    }
    Value* owordIndex = ishl->getOperand(0);
    // want the exact pattern
    Value *ptrV = load->getPointerOperand();
    if (!(isa<IntToPtrInst>(ptrV) && ptrV->hasOneUse()))
    {
        return;
    }
    // do different add, owordIndex + chunkStart/4;
    if (chunk->chunkStart != 0)
    {
        Instruction *pInsert = dyn_cast<Instruction>(cast<IntToPtrInst>(ptrV)->getOperand(0));
        if (!pInsert)
        {
            pInsert = cast<IntToPtrInst>(ptrV);
        }
        irBuilder->SetInsertPoint(pInsert);
        owordIndex = irBuilder->CreateAdd(owordIndex, ConstantInt::get(irBuilder->getInt32Ty(), chunk->chunkStart/4));
        wiAns->incUpdateDepend(owordIndex, WIAnalysis::UNIFORM);
    }
    Type *vty = VectorType::get(load->getType()->getScalarType(), 4);
    Function* l = GenISAIntrinsic::getDeclaration(curFunc->getParent(),
        llvm::GenISAIntrinsic::GenISA_OWordPtr,
        PointerType::get(vty, addrSpace));
    Value* attr[] =
    {
        owordIndex
    };
    irBuilder->SetInsertPoint(load);
    Instruction* owordPtr = irBuilder->CreateCall(l, attr);
    wiAns->incUpdateDepend(owordPtr, WIAnalysis::UNIFORM);
    load->setOperand(0, owordPtr);
    load->setAlignment(16);
}

char IGC::ConstantCoalescing::ID=0;
