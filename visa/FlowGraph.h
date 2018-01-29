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

#ifndef _CONTROLFLOW_H_
#define _CONTROLFLOW_H_

#include <map>
#include <string>
#include <iomanip>
#include <unordered_map>
#include <set>

#include "cm_portability.h"

#include "Gen4_IR.hpp"

namespace vISA
{
class IR_Builder;
class PhyRegSummary;
class KernelDebugInfo;

//
// Forward definitions
//

class G4_BB;
class FlowGraph;

//
// FuncInfo - Function CFG information
//    This class maintains a CFG summary of the function (its INIT block, EXIT block and
//    number of call sites). The functions's INIT block will contain a pointer to its
//    related FuncInfo object. The FuncInfo definition is used for inter-procedural liveness
//    analysis (IPA).
//

class FuncInfo
{
private:

    unsigned id;        // the function id
    G4_BB*   initBB;    // the init node
    G4_BB*   exitBB;    // the exit node
    unsigned callCount; // the number of call sites

	std::vector<G4_BB*>  BBList;      // the list of BBs
	std::list<FuncInfo *>  callees; // the list of callees
	unsigned scopeID;               // the function scope ID

	bool visited;
	unsigned preID;
	unsigned postID;

public:

    FuncInfo(unsigned p_id, G4_BB* p_initBB, G4_BB* p_exitBB)
		: id(p_id), initBB(p_initBB), exitBB(p_exitBB), callCount(1), scopeID(0), visited(false), preID(0), postID(0)
    {
    }

    ~FuncInfo()
    {
        BBList.clear();
        callees.clear();
    }

    void *operator new(size_t sz, Mem_Manager& m)    {return m.alloc(sz);}

    bool doIPA() const
    {
        return callCount > 1;
    }

    unsigned getId() const
    {
        return id;
    }

    void setId(unsigned val)
    {
        id = val;
    }

    G4_BB* getInitBB() const
    {
        return initBB;
    }

    G4_BB* getExitBB() const
    {
        return exitBB;
    }

    void incrementCallCount()
    {
        ++callCount;
    }

    void updateExitBB(G4_BB* p_exitBB)
    {
        exitBB = p_exitBB;
    }

	void addCallee(FuncInfo *fn)
	{
		callees.push_back(fn);
	}

	std::list<FuncInfo *>&  getCallees()
	{
		return callees;
	}

	void addBB(G4_BB* bb)
	{
		BBList.push_back(bb);
	}

	std::vector<G4_BB*>&  getBBList()
	{
		return BBList;
	}

	unsigned getScopeID()
	{
		return scopeID;
	}

	void setScopeID(unsigned id)
	{
		scopeID = id;
	}

	bool getVisited()
	{
		return visited;
	}

	void setVisited()
	{
		visited = true;
	}

	unsigned getPreID()
	{
		return preID;
	}

	void setPreID(unsigned id)
	{
		preID = id;
	}

	unsigned getPostID()
	{
		return postID;
	}

	void setPostID(unsigned id)
	{
		postID = id;
	}
};
}

//
// A table mapping the subroutine (INIT) block id's to their FuncInfo nodes.
//
typedef std::unordered_map<int, vISA::FuncInfo*> FuncInfoHashTable;

typedef std::map<std::string, vISA::G4_BB*> Label_BB_Map;
typedef std::list<vISA::G4_BB*>             BB_LIST;
typedef std::list<vISA::G4_BB*>::iterator   BB_LIST_ITER;
typedef std::list<vISA::G4_BB*>::const_iterator   BB_LIST_CITER;
typedef std::list<vISA::G4_BB*>::reverse_iterator        BB_LIST_RITER;

//
// Block types (relevant for inter-procedural analysis).
//
enum G4_BB_TYPE
{
    G4_BB_NONE_TYPE   = 0x00,
    G4_BB_CALL_TYPE   = 0x01,
    G4_BB_RETURN_TYPE = 0x02,
    G4_BB_INIT_TYPE   = 0x04,
    G4_BB_EXIT_TYPE   = 0x08
};

namespace vISA
{
class G4_BB
{
    //
    // flag to tell if a subroutine shares code with others
    // It is used when subRetLoc is not UNDEFINED_VAL (that is when
    // the block is an entry of a subroutine).
    //
    bool subShareCode;
    //
    // basic block id
    //
    unsigned id;
    //
    // preorder block id
    //
    unsigned preId;
    //
    // reverse postorder block id
    //
    unsigned rpostId;
    //
    // the location id for saving the return address of a subroutine call
    // set to UNDEFINED_VAL if the block is not the entry of a subroutine.
    // register allocation will assign a reg to hold the return addr later
    //
    unsigned subRetLoc;
    //
    // traversal is for traversing control flow graph (to indicate the
    // block is visited)
    //
    unsigned traversal;
    //
    // its immediate dominator
    //
    G4_BB* idom;
    //
    // if the current BB is the return block after a CALL subroutine, then beforeCall points
    // to the BB before the subroutine call.
    //
    G4_BB* beforeCall;
    //
    // if the current BB ends with a CALL subroutine, then afterCall points
    // to the BB after the subroutine returns.
    //
    G4_BB* afterCall;
    //
    // its following block in reverse post order layout
    //
    G4_BB* nextRPOBlock;
    //
    // if the current BB ends with a CALL subroutine, then the calleeInfo points
    // to the FuncInfo node corresponding to the called function.
    // else if the block is an INIT/EXIT block of a function, then the funcInfo
    // points to the FuncInfo node of its function.
    //
    union {
        FuncInfo* calleeInfo;
        FuncInfo* funcInfo;
    };

    //
    // the block classification
    //
    unsigned BBType;

    // indicates if the block is part of a natural loop or not
    bool inNaturalLoop;
    bool hasSendInBB;

    // indicate the nest level of the loop
    unsigned char loopNestLevel;

	// indicates the scoping info in call graph
	unsigned scopeID;

    // if the block is under simd flow control
    bool inSimdFlow;

    //list of all the basic blocks in the function
    //of which this is the first basic block.
    //the list may contain blocks that are disconnected
    //from CFG
    std::map<int, G4_BB*> BBlist;
    G4_BB * start_block;

    G4_BB* backEdgeTopmostDst;

    // the physical pred/succ for this block (i.e., the pred/succ for this block in the BB list)
    // Note that some transformations may rearrange BB layout, so for safety it's best to recompute
    // this
    G4_BB* physicalPred;
    G4_BB* physicalSucc;

    FlowGraph* parent;

public:
    INST_LIST    instList;

    INST_LIST_ITER  begin() { return instList.begin(); }
    INST_LIST_ITER end() { return instList.end(); }

    //
    // Important invariant: fall-through BB must be at the front of Succs.
    // If we don't maintain this property, extra checking (e.g., label
    // comparison) is needed to retrieve fallThroughBB
    //
    BB_LIST    Preds;
    BB_LIST    Succs;

    G4_BB(INST_LIST_NODE_ALLOCATOR& alloc, unsigned i, FlowGraph* fg) :
        subShareCode(false), id(i), preId(0), rpostId(0),
        subRetLoc(UNDEFINED_VAL), traversal(0), idom(NULL), beforeCall(NULL),
        afterCall(NULL), nextRPOBlock(NULL), calleeInfo(NULL), BBType(G4_BB_NONE_TYPE),
        inNaturalLoop(false), loopNestLevel(0), scopeID(0), inSimdFlow(false),
        start_block(NULL), physicalPred(NULL), physicalSucc(NULL), parent(fg), 
        instList(alloc), hasSendInBB(false)
    {
        backEdgeTopmostDst = NULL;
    }

    ~G4_BB()
    {
        instList.clear();
    }

    G4_BB* getBackEdgeTopmostDst() { return backEdgeTopmostDst; }
    void setBackEdgeTopmostDst( G4_BB* bb ) { backEdgeTopmostDst = bb; }

    void    addToBBList(int key, G4_BB* b){BBlist[key] = b;}
    void    clearBBList(){BBlist.clear();}
    bool    existsInBBList(int key){ return BBlist.find(key) != BBlist.end();}
    std::map<int, G4_BB*>::iterator getBBListStart(){return BBlist.begin();}
    std::map<int, G4_BB*>::iterator getBBListEnd(){return BBlist.end();}
    void removeBlockFromBBList(int key) { BBlist.erase(key); }
    void setStartBlock(G4_BB * b) {start_block = b;}
    G4_BB * getStartBlock() {return start_block;}

    bool     isSubShareCode()  {return subShareCode;}
    void     setSubShareCode() {subShareCode = true;}
    void     resetSubShareCode() { subShareCode = false; }
    bool     isLastInstEOT();    // to check if the last instruction in list is EOT
    G4_opcode    getLastOpcode() const;
    unsigned getId()               {return id;}
    void     setId(unsigned i)     {id = i;}
    unsigned getPreId()            {return preId;}
    void     setPreId(unsigned i)  {preId = i;}
    unsigned getRPostId()           {return rpostId;}
    void     setRPostId(unsigned i) {rpostId = i;}
    void     markTraversed(unsigned num)      {traversal = num;}
    bool     isAlreadyTraversed(unsigned num) {return traversal >= num;}
    void     removeSuccEdge(G4_BB* succ);
    void     removePredEdge(G4_BB* pred);
    void     writeBBId(std::ostream& cout)    {cout << "BB" << id;}
    unsigned getSubRetLoc()                      {return subRetLoc;}
    void     setSubRetLoc(unsigned n)          {subRetLoc = n;}
    G4_BB*   fallThroughBB();
    G4_BB*   getIDom()                        {return idom;}
    void     setIDom(G4_BB* dom)              {idom = dom;}
    G4_BB*   BBBeforeCall()                   {return beforeCall;}
    G4_BB*   BBAfterCall()                    {return afterCall;}
    void     setBBBeforeCall(G4_BB* before)   {beforeCall = before;}
    void     setBBAfterCall(G4_BB* after)     {afterCall = after;}
    G4_BB*   getNextRPOBlock()                {return nextRPOBlock;}
    void     setNextRPOBlock(G4_BB* next)     {nextRPOBlock = next;}
    FuncInfo*  getCalleeInfo() const          {return calleeInfo;}
    void       setCalleeInfo(FuncInfo* callee){calleeInfo = callee;}
    FuncInfo*  getFuncInfo() const            {return funcInfo;}
    void       setFuncInfo(FuncInfo* func)    {funcInfo = func;}
    int        getBBType() const              {return BBType;}
    void       setBBType(int type)            {BBType |= type;}
    void       unsetBBType(G4_BB_TYPE type)   {BBType &= ~unsigned(type);}
    void     setInNaturalLoop(bool val)       {inNaturalLoop = val;}
    bool     isInNaturalLoop()                {return inNaturalLoop;}

    void     setSendInBB(bool val)        { hasSendInBB = val; }
    bool     isSendInBB()                { return hasSendInBB; }

    void     setNestLevel()                   {loopNestLevel ++;}
    unsigned char getNestLevel()              {return loopNestLevel;}
    void     resetNestLevel()                 { loopNestLevel = 0; }
    void     setInSimdFlow(bool val)          {inSimdFlow = val;}
    bool     isInSimdFlow()                   {return inSimdFlow;}
	unsigned getScopeID()                     { return scopeID; }
	void setScopeID(unsigned id)              { scopeID = id; }

    G4_BB* getPhysicalPred() const     { return physicalPred; }
    G4_BB* getPhysicalSucc() const     { return physicalSucc; }
    void setPhysicalPred(G4_BB* pred)  { physicalPred = pred; }
    void setPhysicalSucc(G4_BB* succ)  { physicalSucc = succ; }

    void *operator new(size_t sz, Mem_Manager& m)    {return m.alloc(sz);}
    void emit(std::ostream& output);
    void emitInstruction(std::ostream& output, INST_LIST_ITER &it);
    void emitBasicInstruction(std::ostream& output, INST_LIST_ITER &it);
    void emitBasicInstructionIga(char* instSyntax, std::ostream& output, INST_LIST_ITER &it, G4_INST *prevInst, uint32_t &BCNum,  uint32_t &simd8Num);
    void emitInstructionInfo(std::ostream& output, INST_LIST_ITER &it);
    void emitBankConflict(std::ostream& output, G4_INST *inst);

    void emitDepInfo(std::ostream& output, G4_INST *inst, int offset);

    bool isEndWithCall() const { return getLastOpcode() == G4_call; }
    bool isEndWithFCall() const { return getLastOpcode() == G4_pseudo_fcall; }
    bool isEndWithFRet() const { return getLastOpcode() == G4_pseudo_fret; }
    bool isEndWithGoto() const { return getLastOpcode() == G4_goto; }
    bool isSuccBB(G4_BB* succ); // return true if succ is in bb's Succss

    G4_Label* getLabel()
    {
        //FIXME: For now not all BBs will start with a label (e.g.,
        //a block that follows a call).  We should fix it by getting rid
        //of the g4_label instruction and associate each label with a BB
        if( instList.size() > 0 && instList.front()->isLabel() )
        {
            return instList.front()->getLabel();
        }
        return NULL;
    }

    // Return the first non-label instruction if any.
    G4_INST *getFirstInst()
    {
        G4_INST *firstInst = nullptr;
        if (instList.size() > 0)
        {
            INST_LIST_ITER I = instList.begin();
            firstInst = *I;
            if (firstInst->isLabel())
            {
                // Only first inst can be label.
                ++I;
                firstInst = (I != instList.end()) ? *I : nullptr;
            }
        }
        return firstInst;
    }

	void addEOTSend(G4_INST* lastInst = NULL);

    /// Dump instructions into the standard error.
    void dump() const;
    void dumpDefUse() const;

    // reset this BB's instruction's local id so they are [0,..#BBInst-1]
    void resetLocalId();
};
}

typedef enum
{
    STRUCTURED_CF_IF = 0,
    STRUCTURED_CF_LOOP = 1
} STRUCTURED_CF_TYPE;

struct StructuredCF
{
    STRUCTURED_CF_TYPE mType;
    // for if this is the block that ends with if
    // for while this is the loop block
    vISA::G4_BB* mStartBB;
    // for if this is the endif block
    // for while this is the block that ends with while
    vISA::G4_BB* mEndBB;
    // it's possible for a BB to have multiple endifs, so we need
    // to know which endif corresponds to this CF
    vISA::G4_INST* mEndInst;

    StructuredCF* enclosingCF;

    //ToDo: can add more infor (else, break, cont, etc.) as needed later

    // endBB is set when we encounter the endif/while
    StructuredCF(STRUCTURED_CF_TYPE type, vISA::G4_BB* startBB) :
        mType(type), mStartBB(startBB), mEndBB(NULL), mEndInst(NULL), enclosingCF(NULL) {}

    void *operator new(size_t sz, vISA::Mem_Manager& m){ return m.alloc(sz); }

    void setEnd(vISA::G4_BB* endBB, vISA::G4_INST* endInst)
    {
        mEndBB = endBB;
        mEndInst = endInst;
    }
};

//return true to indicate do not visit the successor of the input bb
typedef bool (* fgVisitFP1) (vISA::G4_BB *, void*);
typedef bool (* fgVisitFP2) (vISA::G4_BB *, void*, int);
typedef void(*fgVisitInstFP1) (vISA::G4_INST*, vISA::G4_INST*, void*);

namespace vISA
{
///
/// A hashtable of <declare, node> where every node is a vector of {LB, RB}
/// A source opernad (either SrcRegRegion or Predicate) is considered to be global
/// if it is not fully defined in one BB
///
class GlobalOpndHashTable
{
    Mem_Manager& mem;
    std_arena_based_allocator<uint32_t> private_arena_allocator;

    static uint32_t packBound(uint16_t lb, uint16_t rb)
    {
        return (rb << 16) + lb;
    }

    static uint16_t getLB(uint32_t value)
    {
        return (uint16_t) (value & 0xFFFF);
    }
    static uint16_t getRB(uint32_t value)
    {
        return (uint16_t) (value >> 16);
    }

    struct HashNode
    {
        // each elements is {LB, RB} pair where [0:15] is LB and [16:31] is RB
        std::vector<uint32_t, std_arena_based_allocator<uint32_t>> bounds;

        HashNode(uint16_t lb, uint16_t rb, std_arena_based_allocator<uint32_t>& m)
            : bounds(m)
        {
            bounds.push_back(packBound(lb, rb));
        }

        void *operator new(size_t sz, Mem_Manager& m) {return m.alloc(sz);}
        void insert(uint16_t newLB, uint16_t newRB)
        {
            // check if the newLB/RB either subsumes or can be subsumed by an existing bound
            // ToDo: consider merging bound as well
            for (int i = 0, size = (int)bounds.size(); i < size; ++i)
            {
                uint16_t nodeLB = getLB(bounds[i]);
                uint16_t nodeRB = getRB(bounds[i]);
                if (newLB >= nodeLB && newRB <= nodeRB)
                {
                    return;
                }
                else if (newLB <= nodeLB && newRB >= nodeRB)
                {
                    bounds[i] = packBound(newLB, newRB);
                    return;
                }
            }
            bounds.push_back(packBound(newLB, newRB));
        }
        bool isInNode(uint16_t lb, uint16_t rb)
        {
            for (int i = 0, size = (int) bounds.size(); i < size; ++i)
            {
                uint16_t nodeLB = getLB(bounds[i]);
                uint16_t nodeRB = getRB(bounds[i]);
                if (lb <= nodeLB && rb >= nodeLB)
                {
                    return true;
                }
                else if (lb > nodeLB && lb <= nodeRB)
                {
                    return true;
                }
            }
            return false;
        }
    };

    std::map<G4_Declare*, HashNode*> globalOperands;

public:
    GlobalOpndHashTable(Mem_Manager& m) : mem(m)
    {
    }

    void addGlobalOpnd( G4_Operand *opnd);
    // check if a def is a global variable
    bool isOpndGlobal( G4_Operand *def );
    void clearHashTable()
    {
        for (auto iter = globalOperands.begin(); iter != globalOperands.end(); ++iter)
        {
            iter->second->~HashNode();
        }
        globalOperands.clear();
    }

    void dump();
};
}
typedef std::pair<BB_LIST_ITER, BB_LIST_ITER> GRAPH_CUT_BOUNDS;

namespace vISA
{
class G4_Kernel; // forward declaration
class FlowGraph
{
    // Data

private:

    G4_BB* entryBB;                             // entry block
    unsigned traversalNum;                      // used for flow graph traversals
    unsigned numBBId;                            // number of basic blocks
    bool     reducible;                            // reducibility of the graph
    bool     doIPA;                             // requires inter-procedural liveness analysis
    bool     hasStackCalls;                     // indicates that the flowgraph contains STACK_CALL calls
    bool     isStackCallFunc;                    // indicates the function itself is a STACK_CALL function
    unsigned loopLabelId;                       // used by matchLoop to assign loop label id
    unsigned int autoLabelId;
    G4_Kernel* pKernel;                         // back pointer to the kernel object

    // map each BB to its local RA GRF usage summary, populated in local RA
    std::map<G4_BB*, PhyRegSummary*> bbLocalRAMap;
    // vector of summaries created for each BB, needed for deallocation later
    std::vector<PhyRegSummary*> localRASummaries;


public:
    typedef std::pair<G4_BB*, G4_BB*> Edge;
    typedef std::set<G4_BB*> Blocks;
    typedef std::map<Edge, Blocks> Loop;

    Mem_Manager& mem;                            // mem mananger for creating BBs & starting IP table
    INST_LIST_NODE_ALLOCATOR& instListAlloc;     // a reference to dedicated mem allocator for holding instruction list nodes

    // This list maintains the ordering of the basic blocks (i.e., asm and binary emission will output
    // the blocks in list oder.
    // Important: Due to the nature of SIMD CF, it is unsafe to change the order of basic blocks
    // Once the list is populated in constructFlowGraph(), the only changes allowed are
    // 1. insertion of new exit BBs due to handleExit/handleReturn/handleFRet.  The exit BB
    //    must be the last BB for the kernel/subroutine/function it belongs to
    // 2. deletion of unreachable blocks
    // 3. merging of blocks that only contain one label with its (single) successor
    // If you need to change the block ordering for any reason, create another data structure instead of
    // modifying this one
    BB_LIST BBs;

    // list of all BBs ever created
                                                // This list only grows and is freed when the FlowGraph
                                                // is destroyed
    std::vector<G4_BB*> BBAllocList;
    std::list<Edge> backEdges;                  // list of all backedges (tail->head)
    std::list<Edge> unnaturalBackEdges;         // list of all unnatural backedges
    Loop naturalLoops;

    std::vector<FuncInfo*> funcInfoTable;       // the vector of function info nodes

	std::vector<FuncInfo *>  sortedFuncTable;   // the vector of functions sorted in topological order of the call graph
	FuncInfo* kernelInfo;                       // the call info for the kernel function

    IR_Builder *builder;                        // needed to create new instructions (mainly labels)
    GlobalOpndHashTable globalOpndHT;

    G4_Declare *            framePtrDcl;
    G4_Declare *            stackPtrDcl;
    G4_Declare *            scratchRegDcl;
    // ToDo: change to set if we have a lot of stack call sites
	std::vector<G4_Declare *> pseudoVCADclList;
    G4_Declare *            pseudoVCEDcl;
	std::vector<G4_Declare*> pseudoA0DclList;
	std::vector<G4_Declare*> pseudoFlagDclList;

    // vector of all subroutines in this CFG.  the kernel is always the first subroutine
    // FIXME: there's another data structure funcInfoTable that is used later by RA,
    // we should combine them into one
    std::vector<FuncInfo*> subroutines;

    // computed in markSimdBlocks()
    std::vector<StructuredCF*> structuredSimdCF;

    unsigned                    callerSaveAreaOffset;
    unsigned                    calleeSaveAreaOffset;
    unsigned                    fileScopeSaveAreaSize;
    unsigned                    paramOverflowAreaOffset;
    unsigned                    paramOverflowAreaSize;

    // Bank conflict statistics.
    struct BankConflictStatistics
    {
        unsigned NumOfGoodInsts;
        unsigned NumOfBadInsts;
        unsigned NumOfOKInsts;

        void addGood() { ++NumOfGoodInsts; }
        void addBad() { ++NumOfBadInsts; }
        void addOK() { ++NumOfOKInsts; }
        void clear()
        {
            NumOfGoodInsts = 0;
            NumOfBadInsts = 0;
            NumOfOKInsts = 0;
        }
    } BCStats;

public:

    G4_BB* getLabelBB(Label_BB_Map& map, const char* label);
    G4_BB* beginBB(Label_BB_Map& map, G4_INST* first);

    bool performIPA() const
    {
        return doIPA;
    }

    bool getHasStackCalls() const
    {
        return hasStackCalls;
    }

    void setHasStackCalls()
    {
        hasStackCalls = true;
    }

    bool getIsStackCallFunc() const
    {
        return isStackCallFunc;
    }

    void setIsStackCallFunc()
    {
        isStackCallFunc = true;
    }

    G4_Kernel* getKernel()
    {
        return pKernel;
    }

    void mergeFReturns();

    G4_Declare*& getFramePtrDcl()                   {return framePtrDcl;}
    G4_Declare*& getStackPtrDcl()                   {return stackPtrDcl;}
    G4_Declare*& getScratchRegDcl()                 {return scratchRegDcl;}

    bool isPseudoVCADcl(G4_Declare* dcl) const 
    { 
        return std::find(pseudoVCADclList.begin(), pseudoVCADclList.end(), dcl) != std::end(pseudoVCADclList); 
    }
    bool isPseudoVCEDcl(G4_Declare* dcl) const { return dcl == pseudoVCEDcl; }
    bool isPseudoA0Dcl(G4_Declare* dcl) const
    {
        return std::find(pseudoA0DclList.begin(), pseudoA0DclList.end(), dcl) != std::end(pseudoA0DclList);
    }
    bool isPseudoFlagDcl(G4_Declare* dcl) const
    {
        return std::find(pseudoFlagDclList.begin(), pseudoFlagDclList.end(), dcl) != std::end(pseudoFlagDclList);
    }
    bool isPseudoDcl(G4_Declare* dcl) const
    {
        if (!getHasStackCalls() && !getIsStackCallFunc())
        {
            return false;
        }
        return isPseudoVCADcl(dcl) || isPseudoVCEDcl(dcl) || isPseudoA0Dcl(dcl) || isPseudoFlagDcl(dcl);
    }

    //
    // Merge multiple returns into one, prepare for spill code insertion
    //
    void mergeReturn(Label_BB_Map& map, FuncInfoHashTable& funcInfoTable);
    void searchReturn(G4_BB* bb, G4_BB* returnAddr, BB_LIST & retBBList);
    G4_BB* mergeSubRoutineReturn(G4_BB* bb, G4_BB* returnAddr, BB_LIST & retBBList);
    void decoupleReturnBlock(G4_BB*);
    void decoupleInitBlock(G4_BB*, FuncInfoHashTable& funcInfoTable);
    void decoupleExitBlock(G4_BB*);
    void normalizeSubRoutineBB( FuncInfoHashTable& funcInfoTable );
    void processGoto(bool HasSIMDCF);
    void insertJoinToBB( G4_BB* bb, uint8_t execSize, G4_Label* jip );

    // functions for structure analysis
    G4_Kernel *getKernel() const { return pKernel; }
    G4_Label* insertEndif( G4_BB* bb, unsigned char execSize, bool createLabel );
    void setJIPForEndif( G4_INST* endif, G4_INST* target, G4_BB* targetBB);
    void convertGotoToJmpi(G4_INST *gotoInst)
    {
        gotoInst->setOpcode(G4_jmpi);
        gotoInst->setSrc(gotoInst->asCFInst()->getUip(), 0);
        gotoInst->asCFInst()->setJip(NULL);
        gotoInst->asCFInst()->setUip(NULL);
        gotoInst->setExecSize(1);
        gotoInst->setOptions(InstOpt_NoOpt | InstOpt_WriteEnable);
    }
    bool convertJmpiToGoto();

    unsigned getNumFuncs() const
    {
        return unsigned(funcInfoTable.size());
    }

    void handleReturn(std::map<std::string, G4_BB*>& map, FuncInfoHashTable& funcInfoTable);
    void linkReturnAddr(std::map<std::string, G4_BB*>& map, G4_BB* bb, G4_BB* returnAddr);

    void handleExit();
    void handleWait();

    void preprocess(INST_LIST& instlist);

    FlowGraph(INST_LIST_NODE_ALLOCATOR& alloc, G4_Kernel* kernel, Mem_Manager& m) : entryBB(NULL), traversalNum(0), numBBId(0), reducible(true),
      doIPA(false), hasStackCalls(false), isStackCallFunc(false), loopLabelId(0), autoLabelId(0),
      pKernel(kernel), mem(m), instListAlloc(alloc),
      builder(NULL), globalOpndHT(m), framePtrDcl(NULL), stackPtrDcl(NULL),
      scratchRegDcl(NULL), pseudoVCEDcl(NULL) {}

    ~FlowGraph();

    void setBuilder(IR_Builder *pBuilder )
    {
        builder = pBuilder;
    }

    void addPredSuccEdges(G4_BB* pred, G4_BB* succ, bool tofront=true)
    {
        if (tofront)
            pred->Succs.push_front(succ);
        else
            pred->Succs.push_back(succ);

        succ->Preds.push_front(pred);
    }

    void addUniquePredSuccEdges(G4_BB* pred, G4_BB* succ, bool tofront=true)
    {
        // like above, but check for duplicate edges
        auto iter = std::find_if(pred->Succs.begin(), pred->Succs.end(), [succ](G4_BB* bb) { return bb == succ; });
        if (iter == pred->Succs.end())
        {
            addPredSuccEdges(pred, succ, tofront);
        }
    }

    void removePredSuccEdges(G4_BB* pred, G4_BB* succ)
    {
        MUST_BE_TRUE(pred != NULL && succ != NULL, ERROR_INTERNAL_ARGUMENT);

        BB_LIST_ITER lt = pred->Succs.begin();
        for (; lt != pred->Succs.end(); ++lt){
            if( (*lt) == succ ){
                pred->Succs.erase( lt );
                break;
            }
        }

        lt = succ->Preds.begin();
        for (; lt != succ->Preds.end(); ++lt){
            if( (*lt) == pred ){
                succ->Preds.erase( lt );
                break;
            }
        }
    }

    G4_INST* createNewLabelInst(G4_Label* label, int lineNo, int CISAOff);

    G4_BB* createNewBB(bool insertInFG = true);
    int64_t insertDummyUUIDMov();
    //
    // Increase by one so that all BBs' traversal are less than traversalNum
    //
    void prepareTraversal()    {traversalNum++;}
    unsigned getTraversalNum() {return traversalNum;}

    //
    // Check if the graph is reducible
    //
    bool isReducible() { return reducible; }
    //
    // Get the list of back edges belonging to multiple entry loops
    //
    const std::list<Edge>&  getUnnaturalBackEdges() { return unnaturalBackEdges; }
    //
    // Remove any placeholder empty blocks that could have been inserted to aid analysis
    //
    void removeRedundantLabels();
    //
    // remove any mov with the same src and dst opnds
    //
    void removeRedundMov();
    //
    // Remove any placeholder empty blocks that could have been inserted to aid analysis.
    //
    void removeEmptyBlocks();
    //
    // Add a dummy BB for multiple-exit flow graph
    //
    void linkDummyBB();
    //
    // Re-assign block ID so that we can use id to determine the ordering of two blocks in
    // the code layout
    //
    void reassignBlockIDs();
    //
    // Remove blocks that are unreachable via control flow of program
    //
    void removeUnreachableBlocks();

    void constructFlowGraph(INST_LIST& instlist);
    bool matchBranch(int &sn, INST_LIST& instlist, INST_LIST_ITER &it);
    void matchLoop(INST_LIST& instlist);
    void localDataFlowAnalysis();
    unsigned getNumBB() const      {return numBBId;}
    G4_BB* getEntryBB()        {return entryBB;}
    void setEntryBB(G4_BB *entry) {entryBB = entry;}

    void doFilescopeVarLayout(IR_Builder& builder, DECLARE_LIST& declares, unsigned& fileScopeFrameOffset);
    void addFrameSetupDeclares(IR_Builder& builder, PhyRegPool& regPool);
    void addSaveRestorePseudoDeclares(IR_Builder& builder);
    void markSimdBlocks(std::map<std::string, G4_BB*>& labelMap, FuncInfoHashTable &FuncInfoMap);

    // Used for CISA 3.0
    void incrementNumBBs() { numBBId++ ; }
    G4_BB* getUniqueReturnBlock()
    {
        // Return block that has a return instruction
        // Return NULL if multiple return instructions found
        G4_BB* uniqueReturnBlock = NULL;

        for( BB_LIST_ITER bb_it = BBs.begin(); bb_it != BBs.end(); ++bb_it ) {
            G4_BB* curBB = *bb_it;
            G4_INST* last_inst = NULL;

            if( curBB->instList.size() > 0 ) {
                last_inst = curBB->instList.back();

                if( last_inst->opcode() == G4_pseudo_fret ) {
                    if( uniqueReturnBlock == NULL ) {
                        uniqueReturnBlock = curBB;
                    }
                    else {
                        uniqueReturnBlock = NULL;
                        break;
                    }
                }
            }
        }

        return uniqueReturnBlock;
    }

    void NormalizeFlowGraph();

    void setPhysicalPredSucc();

    void markRPOTraversal();

	void DFSTraverse(G4_BB* bb, unsigned &preId, unsigned &postId, FuncInfo* fn);

    void findBackEdges();

    void findNaturalLoops();

	void traverseFunc(FuncInfo* func, unsigned int *ptr);
	void sortFuncs();
	void findDominators(std::map<FuncInfo*, std::set<FuncInfo*>>& domMap);
	unsigned int resolveVarScope(G4_Declare* dcl, FuncInfo* func);
	void markVarScope(std::vector<G4_BB*>& BBList, FuncInfo* func);
	void markScope();

    void addSIMDEdges();

    void addBBLRASummary(G4_BB* bb, PhyRegSummary* summary)
    {
        bbLocalRAMap.insert(std::make_pair(bb, summary));
        localRASummaries.push_back(summary);
    }

    void clearBBLRASummaries()
    {
        bbLocalRAMap.clear();
    }

    PhyRegSummary* getBBLRASummary(G4_BB* bb) const
    {
        auto&& iter = bbLocalRAMap.find(bb);
        return iter != bbLocalRAMap.end() ? iter->second : nullptr;
    }

private:
    //
    // Flow group traversal routines
    //
    void AssignDFSBasedIds(G4_BB* bb, unsigned &preId, unsigned &postId, std::list<G4_BB*>& rpoBBList);
    void trackCutReferenceFilescopeVars(BB_LIST& graphCutBBs, DECLARE_LIST& refVars, unsigned numDcls);
    // Use normalized region descriptors for each source operand if possible.
    void normalizeRegionDescriptors();
    G4_BB *findLabelBB(char *label, int &label_offset);
};

}
#define RA_TYPE(DO) \
	DO(TRIVIAL_BC_RA) \
	DO(TRIVIAL_RA) \
	DO(LOCAL_ROUND_ROBIN_BC_RA) \
	DO(LOCAL_ROUND_ROBIN_RA) \
	DO(LOCAL_FIRST_FIT_BC_RA) \
	DO(LOCAL_FIRST_FIT_RA) \
	DO(HYBRID_BC_RA) \
	DO(HYBRID_RA) \
	DO(GRAPH_COLORING_RR_BC_RA) \
	DO(GRAPH_COLORING_FF_BC_RA) \
	DO(GRAPH_COLORING_RR_RA) \
	DO(GRAPH_COLORING_FF_RA) \
	DO(GRAPH_COLORING_SPILL_RR_BC_RA) \
	DO(GRAPH_COLORING_SPILL_FF_BC_RA) \
	DO(GRAPH_COLORING_SPILL_RR_RA) \
	DO(GRAPH_COLORING_SPILL_FF_RA) \
	DO(UNKNOWN_RA)

enum RA_Type
{
	RA_TYPE(MAKE_ENUM)
};

namespace vISA
{
class gtPinData
{
public:
    enum RAPass
    {
        FirstRAPass = 0,
        ReRAPass = 1
    };

    gtPinData(G4_Kernel& k) : kernel(k)
    {
        whichRAPass = FirstRAPass;
    }

    void *operator new(size_t sz, Mem_Manager& m){ return m.alloc(sz); }

    ~gtPinData(){}

    void markInst(G4_INST* i)
    {
        MUST_BE_TRUE(whichRAPass == FirstRAPass, "Unexpectedly marking in re-RA pass.");
        markedInsts.insert(i);
    }

    void markInsts();
    void clearMarkedInsts() { markedInsts.clear(); }
    void removeUnmarkedInsts();

    bool isFirstRAPass() { return whichRAPass == RAPass::FirstRAPass; }
    bool isReRAPass() { return whichRAPass == RAPass::ReRAPass; }
    void setRAPass(RAPass p) { whichRAPass = p; }

    // All following functions work on byte granularity of GRF file
    void clearFreeGlobalRegs() { globalFreeRegs.clear(); }
    unsigned int getNumFreeGlobalRegs() { return (unsigned int)globalFreeRegs.size(); }
    unsigned int getFreeGlobalReg(unsigned int n) { return globalFreeRegs[n]; }
    void addFreeGlobalReg(unsigned int n) { globalFreeRegs.push_back(n); }

    void dumpGlobalFreeGRFs()
    {
        printf("Global free regs:");
        for (unsigned int i = 0; i < globalFreeRegs.size(); i++)
        {
            printf("r%d.%d:b, ", globalFreeRegs[i]/G4_GRF_REG_NBYTES, globalFreeRegs[i]%G4_GRF_REG_NBYTES);
        }
        printf("\n");
    }

    // This function internally mallocs memory to hold buffer
    // of free GRFs. It is meant to be freed by caller after
    // last use of the buffer.
    void* getFreeGRFInfo(unsigned int& size);
    
private:
    G4_Kernel& kernel;
    std::set<G4_INST*> markedInsts;
    RAPass whichRAPass;
    // globalFreeRegs are in units of bytes in linearized register file.
    // Data is assumed to be sorted in ascending order during insertion.
    // Duplicates are not allowed.
    std::vector<unsigned int> globalFreeRegs;
};

class G4_Kernel
{
    const char* name;
    unsigned numRegTotal;
    unsigned int simdSize;
    bool hasAddrTaken;
    Options *m_options;

	RA_Type RAType;
    KernelDebugInfo* kernelDbgInfo;
    void dumpDotFileInternal(const char* appendix);
    void dumpPassInternal(const char *appendix);

    gtPinData* gtPinInfo;

    uint32_t asmInstCount;
    uint64_t kernelID;
    uint32_t tokenInstructionCount;
    uint32_t tokenReuseCount;
    uint32_t AWTokenReuseCount;
    uint32_t ARTokenReuseCount;
    uint32_t AATokenReuseCount;
    uint32_t mathInstCount;
    uint32_t syncInstCount;
    uint32_t mathReuseCount;
    uint32_t ARSyncInstCount;
    uint32_t AWSyncInstCount;

    uint32_t bank_good_num;
    uint32_t bank_ok_num;
    uint32_t bank_bad_num;

    unsigned int callerSaveLastGRF;

public:
    FlowGraph fg;
    DECLARE_LIST           Declares;

    unsigned char major_version;
    unsigned char minor_version;

    G4_Kernel(INST_LIST_NODE_ALLOCATOR& alloc,
              Mem_Manager &m, Options *options, unsigned char major, unsigned char minor)
              : m_options(options), RAType(RA_Type::UNKNOWN_RA), fg(alloc, this, m), 
              major_version(major), minor_version(minor), asmInstCount(0), kernelID(0), 
              tokenInstructionCount(0), tokenReuseCount(0), AWTokenReuseCount(0),
              ARTokenReuseCount(0), AATokenReuseCount(0), mathInstCount(0), syncInstCount(0),mathReuseCount(0),
              ARSyncInstCount(0), AWSyncInstCount(0),
              bank_good_num(0), bank_ok_num(0), bank_bad_num(0)
    {
        ASSERT_USER(
            major < COMMON_ISA_MAJOR_VER ||
                (major == COMMON_ISA_MAJOR_VER && minor <= COMMON_ISA_MINOR_VER),
            "CISA version not supported by this JIT-compiler");
        numRegTotal = UNDEFINED_VAL;
        name = NULL;
        simdSize = 0;
        hasAddrTaken = false;
        kernelDbgInfo = nullptr;
        if (options->getOption(vISAOptions::vISA_ReRAPostSchedule) ||
            options->getOption(vISAOptions::vISA_GetFreeGRFInfo))
        {
            allocGTPinData();
        }
        else
        {
            gtPinInfo = nullptr;
        }

        unsigned int totalGRFs = options->getuInt32Option(vISA_TotalGRFNum);
        callerSaveLastGRF = ((totalGRFs - 8) / 2) - 1;
    }

    ~G4_Kernel();

    void *operator new(size_t sz, Mem_Manager& m)    {return m.alloc(sz);}

    void setBuilder(IR_Builder *pBuilder )
    {
        fg.setBuilder(pBuilder);
    }

    void setAsmCount(int count) { asmInstCount = count; }
    uint32_t getAsmCount() const { return asmInstCount; }

    void setTokenInstructionCount(int count) {tokenInstructionCount = count; }
    uint32_t getTokenInstructionCount() {return tokenInstructionCount; }

    void setTokenReuseCount(int count) {tokenReuseCount= count; }
    uint32_t getTokenReuseCount() {return tokenReuseCount; }
    
    void setAWTokenReuseCount(int count) {AWTokenReuseCount= count; }
    uint32_t getAWTokenReuseCount() {return AWTokenReuseCount; }

    void setARTokenReuseCount(int count) {ARTokenReuseCount= count; }
    uint32_t getARTokenReuseCount() {return ARTokenReuseCount; }

    void setAATokenReuseCount(int count) {AATokenReuseCount= count; }
    uint32_t getAATokenReuseCount() {return AATokenReuseCount; }

    void setMathInstCount(int count) {mathInstCount= count; }
    uint32_t getMathInstCount() {return mathInstCount; }

    void setSyncInstCount(int count) {syncInstCount= count; }
    uint32_t getSyncInstCount() {return syncInstCount; }

    void setMathReuseCount(int count) {mathReuseCount= count; }
    uint32_t getMathReuseCount() {return mathReuseCount; }

    void setARSyncInstCount(int count) {ARSyncInstCount= count; }
    uint32_t getARSyncInstCount() {return ARSyncInstCount; }

    void setAWSyncInstCount(int count) {AWSyncInstCount= count; }
    uint32_t getAWSyncInstCount() {return AWSyncInstCount; }

    void setBankGoodNum(int num) {bank_good_num = num; }
    uint32_t getBankGoodNum() {return bank_good_num; }

    void setBankOkNum(int num) {bank_ok_num = num; }
    uint32_t getBankOkNum() {return bank_ok_num; }

    void setBankBadNum(int num) {bank_bad_num = num; }
    uint32_t getBankBadNum() {return bank_bad_num; }
    
    void setKernelID(uint64_t ID) { kernelID = ID; }
    uint64_t getKernelID() const { return kernelID; }

    Options *getOptions(){ return m_options; }
    bool getOption(vISAOptions opt) const { return m_options->getOption(opt); }
    void calculateSimdSize();
    unsigned int getSimdSize() { return simdSize; }

    void setHasAddrTaken(bool val) { hasAddrTaken = val; }
    bool getHasAddrTaken() { return hasAddrTaken;  }

    void setNumRegTotal(unsigned num)     {numRegTotal = num;}
    void setName(const char* n)                  {name = n;}
    const char*    getName()                      {return name;}
    const char*    getOrigCMName()              {return name + 2;}
    unsigned getNumRegTotal()             {return numRegTotal;}
    void emit_asm(std::ostream& output, bool beforeRegAlloc, void * binary, uint32_t binarySize);
    void emit_dep(std::ostream& output);
    
    void evalAddrExp(void);
    void dumpDotFile(const char* appendix);

    void setVersion( unsigned char major_ver, unsigned char minor_ver )
    {
        major_version = major_ver;
        minor_version = minor_ver;
    }

    int getVersionAsInt() const
    {
        return major_version * 100 + minor_version;
    }

    /// Dump this kernel into the standard error.
    void dump() const;

	void setRAType(RA_Type type) { RAType = type; }
	RA_Type getRAType() { return RAType; }

    void setKernelDebugInfo(KernelDebugInfo* k) { kernelDbgInfo = k; }
    KernelDebugInfo* getKernelDebugInfo();

    void use64BitFESP();

    gtPinData* getGTPinData() { return gtPinInfo; }

    void allocGTPinData()
    {
        gtPinInfo = new(fg.mem) gtPinData(*this);
    }

    unsigned int getCallerSaveLastGRF() { return callerSaveLastGRF; }

    // This function returns starting register number to use
    // for allocating FE/BE stack/frame ptrs.
    unsigned int getStackCallStartReg();
    unsigned int calleeSaveStart();
    static unsigned int getNumScratchRegs() { return 3; }
    unsigned int getNumCalleeSaveRegs();
};

class SCCAnalysis
{
    //
    // implements Tarjan's SCC algorithm
    //
    const FlowGraph& cfg;
    
    // node used during the SCC algorithm
    struct SCCNode
    {
        G4_BB* bb;
        int index;
        int lowLink;
        bool isOnStack;

        SCCNode(G4_BB* newBB, int curIndex) : bb(newBB), index(curIndex), lowLink(curIndex), isOnStack(true) {}
        void dump() const
        {
            std::cerr << "SCCNode: BB" << bb->getId() << ", (" << index << "," << lowLink << ")\n";
        }
    };

    std::stack<SCCNode*> SCCStack;
    int curIndex = 0;
    std::vector<SCCNode*> SCCNodes; // 1:1 mapping between SCCNode and BB, indexed by BBId

    class SCC
    {
        G4_BB* root;
        // list of BBs belonging to the SCC (including root as last BB)
        // assumption is SCC is small (10s of BBs) so membership test is cheap
        std::vector<G4_BB*> body; 

    public:
        SCC(G4_BB* bb) : root(bb) {} // root gets pushed to body just like other BBs in SCC
        void addBB(G4_BB* bb) { body.push_back(bb); }
        std::vector<G4_BB*>::iterator body_begin() { return body.begin(); }
        std::vector<G4_BB*>::iterator body_end() { return body.end(); }
        size_t getSize() const { return body.size(); }
        bool isMember(G4_BB* bb) const
        {
            return std::find(body.begin(), body.end(), bb) != body.end();
        }
        // get earliest BB in program order (this is not necessarily the root depending on DFS order)
        // assumption is reassingBBId() is called
        G4_BB* getEarliestBB() const 
        {
            auto result = std::min_element(body.begin(), body.end(), 
                [](G4_BB* bb1, G4_BB* bb2) {return bb1->getId() < bb2->getId(); });
            return *result;
        }
        void dump() const
        {
            std::cerr << "SCC (root = BB" << root->getId() << ", size = " << getSize() << "):\t";
            for (auto bodyBB : body)
            {
                std::cerr << "BB" << bodyBB->getId() << ", ";
            }
            std::cerr << "\n";
        }
    };

    // vector of SCCs
    std::vector<SCC> SCCs;

public:
    SCCAnalysis(const FlowGraph& fg) : cfg(fg) {}
    ~SCCAnalysis()
    {
        for (auto node : SCCNodes)
        {
            delete node;
        }
    }

    void run();
    void findSCC(SCCNode* node);

    SCCNode* createSCCNode(G4_BB* bb)
    {
        assert(SCCNodes[bb->getId()] == nullptr && "SCCNode already exists");
        SCCNode* newNode = new SCCNode(bb, curIndex++);
        SCCNodes[bb->getId()] = newNode;
        return newNode;
    }

    std::vector<SCC>::iterator SCC_begin() { return SCCs.begin(); }
    std::vector<SCC>::iterator SCC_end() { return SCCs.end(); }
    size_t getNumSCC() const { return SCCs.size(); }
    void dump() const
    {
        for (auto node : SCCNodes)
        {
            node->dump();
        }
        for (auto SCC : SCCs)
        {
            SCC.dump();
        }
    }
};
}
#endif
