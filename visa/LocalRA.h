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

#ifndef _INC_LOCALRA_H_
#define _INC_LOCALRA_H_

#include <list>
#include "G4_Opcode.h"
#include "FlowGraph.h"
#include "BuildIR.h"
#include "BitSet.h"

// Forward decls
namespace vISA
{
class G4_Declare;
class G4_INST;
class G4_BB;
class LinearScan;
class LocalLiveRange;
class PhyRegLocalRA;
class PhyRegsManager;
class PhyRegSummary;
class BankConflictPass;
class GlobalRA;
}

vISA::G4_Declare* GetTopDclFromRegRegion(vISA::G4_Operand* opnd);

typedef std::map<vISA::LocalLiveRange*, std::vector<std::pair<INST_LIST_ITER, unsigned int>>> LLR_USE_MAP;
typedef std::map<vISA::LocalLiveRange*, std::vector<std::pair<INST_LIST_ITER, unsigned int>>>::iterator LLR_USE_MAP_ITER;

// Each declaration will have a LocalLiveRange object allocated for it
namespace vISA
{
    class PhyRegsLocalRA;
    class InputLiveRange;

    class LocalRA
    {
    private:
        G4_Kernel& kernel;
        IR_Builder& builder;
        PhyRegsLocalRA* pregs = nullptr;
        LLR_USE_MAP LLRUseMap;
        unsigned int numRegLRA = 0;
        bool& highInternalConflict;
        unsigned int globalLRSize = 0;
        bool doSplitLLR = false;
        Mem_Manager& mem;
        std::list<InputLiveRange*, std_arena_based_allocator<InputLiveRange*>> inputIntervals;
        BankConflictPass& bc;
        GlobalRA& gra;

        G4_Align getBankAlignForUniqueAssign(G4_Declare *dcl);
        bool hasBackEdge();
        void evenAlign();
        void preLocalRAAnalysis();
        void trivialAssignRA(bool& needGlobalRA, bool threeSourceCandidate);
        bool localRAPass(bool doRoundRobin, bool doBankConflictReduction, bool doSplitLLR);
        void resetMasks();
        void blockOutputPhyRegs();
        void removeUnrequiredLifetimeOps();
        bool assignUniqueRegisters(bool twoBanksRA, bool twoDirectionsAssign);
        bool unassignedRangeFound();
        void updateRegUsage(PhyRegSummary* summary, unsigned int& numRegsUsed);
        void localRAOptReport();
        void markReferencesInOpnd(G4_Operand* opnd, bool isEOT, INST_LIST_ITER inst_it,
            unsigned int pos);
        void markReferencesInInst(INST_LIST_ITER inst_it);
        void setLexicalID();
        void markReferences(unsigned int& numRowsEOT, bool& lifetimeOpFound);
        void calculateInputIntervals();
        void calculateLiveIntervals(G4_BB* bb, std::vector<LocalLiveRange*>& liveIntervals);
        void printAddressTakenDecls();
        void printLocalRACandidates();
        void printLocalLiveIntervals(G4_BB* bb, std::vector<LocalLiveRange*>& liveIntervals);
        void printInputLiveIntervals();
        bool countLiveIntervals();
        
        // scratch fields used for parameter passing
        G4_BB* curBB = nullptr;

    public:
        static void getRowInfo(int size, int& nrows, int& lastRowSize);
        static void findRegisterCandiateWithAlignForward(int &i, G4_Align align, bool evenAlign);
        static void findRegisterCandiateWithAlignBackward(int &i, G4_Align align, bool evenAlign);
        static unsigned int convertSubRegOffFromWords(G4_Declare* dcl, int subregnuminwords);
        static unsigned int convertSubRegOffToWords(G4_Declare* dcl, int subregnum);
        static void countLocalLiveIntervals(std::vector<LocalLiveRange*>& liveIntervals);
        static void printLocalLiveIntervalDistribution(unsigned int numScalars,
            unsigned int numHalfGRF, unsigned int numOneGRF, unsigned int numTwoOrMoreGRF,
            unsigned int numTotal);

        LocalRA(G4_Kernel&, bool&, BankConflictPass&, GlobalRA&);
        bool localRA(bool& doRoundRobin, bool& doBankConflict);
        void insertDecls();
        void undoLocalRAAssignments(bool clearInterval, DECLARE_LIST_ITER* firstRealDclIter);
    };

class LocalLiveRange
{
private:
	G4_Declare* topdcl;
	G4_INST* firstRef;
	G4_INST* lastRef;
	unsigned int lrStartIdx, lrEndIdx;
	bool isIndirectAccess;
	G4_VarBase* preg;
	// pregoff is stored in word here
	// But subreg offset stored in regvar should be in units of dcl's element size
	int pregoff;

	unsigned int numRefsInFG;
	G4_BB* prevBBRef;
	bool eot;

	bool assigned;
    bool isSplit;

public:
	LocalLiveRange()
	{
		topdcl = NULL;
		firstRef = lastRef = NULL;
		lrStartIdx = lrEndIdx = 0;
		isIndirectAccess = false;
		numRefsInFG = 0;
		prevBBRef = NULL;
		preg = NULL;
		pregoff = 0;
		assigned = false;
		eot = false;
        isSplit = false;
	}

	// A reference to this live range exists in bb basic block, record it
	void recordRef( G4_BB* );

	void markIndirectRef() { isIndirectAccess = true; }

	// A live range is local if it is never accessed indirectly (via address taken register) and
	// only a single basic block references the range
	bool isLiveRangeLocal();

    bool isLiveRangeGlobal();

	bool isGRFRegAssigned();

	void setTopDcl( G4_Declare* dcl )
	{
		MUST_BE_TRUE( topdcl == NULL, "Redefining top dcl");
		topdcl = dcl;
	}

	G4_Declare* getTopDcl() { return topdcl; }

	void* operator new(size_t sz, Mem_Manager& m) {return m.alloc(sz);}

	bool hasIndirectAccess() { return isIndirectAccess; }

    void setFirstRef(G4_INST* inst, unsigned int idx)
    {
        firstRef = inst;
        lrStartIdx = idx;
    }

    G4_INST* getFirstRef(unsigned int& idx)
    {
        idx = lrStartIdx;
        return firstRef;
    }

    void setLastRef(G4_INST* inst, unsigned int idx)
    {
        lastRef = inst;
        lrEndIdx = idx;
    }

    G4_INST* getLastRef(unsigned int& idx)
    {
        idx = lrEndIdx;
        return lastRef;
    }

	void setPhyReg( G4_VarBase* pr, int subreg ) { preg = pr; pregoff = subreg; }
	G4_VarBase* getPhyReg(int& subreg) { subreg = pregoff; return preg; }

	unsigned int getSizeInWords();

	void setAssigned(bool a) { assigned = a; }
	bool getAssigned() { return assigned; }

	void markEOT() { eot = true; }
    bool isEOT() { return eot; }

    void markSplit() { isSplit = true; }
    bool getSplit() { return isSplit; }
};

class InputLiveRange
{
private:
    unsigned int regWordIdx;
    unsigned int lrEndIdx;

public:
	InputLiveRange(unsigned int regId, unsigned int endId) : regWordIdx(regId), lrEndIdx(endId)
	{

	}

    void* operator new(size_t sz, Mem_Manager& m) {return m.alloc(sz);}

    unsigned int getRegWordIdx() { return regWordIdx; }
    unsigned int getLrEndIdx() { return lrEndIdx; }
};
}
#define NUM_WORDS_PER_GRF 16
#define SECOND_HALF_BANK_START_GRF 64

enum
{
	WORD_FREE = 0,
	WORD_BUSY = 1,
};

#define REG_UNAVAILABLE 0xffff0000

namespace vISA
{
class PhyRegsLocalRA
{
private:
	unsigned int numRegs;
	// nth bit represents whether the register's nth word is free/busy
	// 1 - busy, 0 - free
	// It is possible to use bit-vector in place of this array
	// bitvector does not provide coarse grained access to mark
	// entire grf busy/available
    std::vector<uint32_t> regBusyVector;
    std::vector<int32_t> regLastUse;

    int lastUseSum1;
    int lastUseSum2;
    int bank1AvailableRegNum;
    int bank2AvailableRegNum;

    bool twoBanksRA;
    bool simpleGRFAvailable;
    bool r0Forbidden;
    bool r1Forbidden;

public:
	PhyRegsLocalRA(uint32_t nregs) : numRegs(nregs)
	{
		uint32_t grfFree = 0;

        regBusyVector.resize(numRegs);
        regLastUse.resize(numRegs);

		for( int i = 0; i < (int) nregs; i++ )
		{
			regBusyVector[i] = grfFree;
            regLastUse[i] = 0;
		}

		lastUseSum1 = 0;
		lastUseSum2 = 0;
		bank1AvailableRegNum = 0;
		bank2AvailableRegNum = 0;

		twoBanksRA = false;
		simpleGRFAvailable = false;
		r0Forbidden = false;
		r1Forbidden = false;
	}

	void* operator new(size_t sz, Mem_Manager& m) {return m.alloc(sz);}

	void setGRFBusy( int which );
	void setGRFBusy( int which, int howmany );
	void setGRFNotBusy( int which, int instID );
	void setH1GRFBusy( int which );
	void setH2GRFBusy( int which );
	void setWordBusy( int whichgrf, int word );
	void setWordBusy( int whichgrf, int word, int howmany );
	void setWordNotBusy( int whichgrf, int word, int instID );

	inline bool isGRFBusy( int which ) const
    {
        MUST_BE_TRUE(isGRFAvailable(which), "Invalid register");
        return (regBusyVector[which] != 0);
    }

    inline bool isGRFBusy( int which, int howmany ) const
    {
        bool retval = false;

        for (int i = 0; i < howmany && !retval; i++)
        {
            retval |= isGRFBusy(which + i);
        }

        return retval;
    }

	bool isH1GRFBusy( int which );
	bool isH2GRFBusy( int which );
	inline bool isWordBusy( int whichgrf, int word );
	inline bool isWordBusy( int whichgrf, int word, int howmany );

	void markPhyRegs( G4_Declare* topdcl );

	// Available/unavailable is different from busy/free
	// Unavailable GRFs are not available for allocation
	void setGRFUnavailable( int which ) { regBusyVector[which] = REG_UNAVAILABLE; }
    bool isGRFAvailable(int which) const
    {
       
        if (simpleGRFAvailable)
        {
            if (which > 1)
            {
                return true;
            }
            else
            {
                if (r0Forbidden && which == 0)
                {
                	return false;
                }

                if (r1Forbidden && which <= 1)
                {
                    return false;
                }
                return true;
            }
        }
        else
        {
            MUST_BE_TRUE(which < (int) numRegs, "invalid GRF");
            return (!((regBusyVector[which] & REG_UNAVAILABLE) == REG_UNAVAILABLE));
        }
    }
    
	bool isGRFAvailable( int which, int howmany) const
    {
        if (simpleGRFAvailable)
        {
            if (which > 1)
            {
                return true;
            }
            else
            {
                if (r0Forbidden && which == 0)
                {
                	return false;
                }

                if (r1Forbidden && which <= 1)
                {
                    return false;
                }
                return true;
            }
        }
        else
        {
	        for (int i = 0; i < howmany; i++)
	        {
	            if (!isGRFAvailable(which + i))
	            {
	                return false;
	            }
	        }
        }
        
        return true;
    }

	void printBusyRegs();
	int getRegLastUse(int reg) {return regLastUse[reg];}

    int getLastUseSum1() {return lastUseSum1;}
    int getLastUseSum2() {return lastUseSum2;}
    int getBank1AvailableRegNum() {return bank1AvailableRegNum;}
    int getBank2AvailableRegNum() {return bank2AvailableRegNum;}

    void setBank1AvailableRegNum(int avail1) { bank1AvailableRegNum = avail1;}
    void setBank2AvailableRegNum(int avail2) { bank2AvailableRegNum = avail2;}

    void setTwoBanksRA(bool twoBanks) { twoBanksRA = twoBanks;}
    void setSimpleGRFAvailable(bool simple) {simpleGRFAvailable = simple; }
    void setR0Forbidden() {r0Forbidden = true;}
    void setR1Forbidden() {r1Forbidden = true;}
    bool findFreeMultipleRegsForward(int regIdx, G4_Align align, int &regnum, int nrows, int lastRowSize, int endReg, int instID, bool isHybridAlloc);
    bool findFreeMultipleRegsBackward(int regIdx, G4_Align align, int &regnum, int nrows, int lastRowSize, int endReg, int instID, bool isHybridAlloc);
    bool findFreeSingleReg( int regIdx, G4_SubReg_Align subalign, int &regnum, int &subregnum, int size);
    bool findFreeSingleReg(int regIdx, int size, G4_Align align, G4_SubReg_Align subalign, int &regnum, int &subregnum, int endReg, int instID, bool isHybridAlloc, bool forward);
    
};

class PhyRegsManager
{
private:
	PhyRegsLocalRA availableRegs;
    bool twoBanksRA;

public:
    PhyRegsManager(PhyRegsLocalRA pregs, bool _twoBanksRA) : availableRegs(pregs), twoBanksRA(_twoBanksRA)
	{
	    availableRegs.setTwoBanksRA(_twoBanksRA);
	}

	int findFreeRegs( int numwords, G4_Align align, G4_SubReg_Align subalign, int& regnum, int& subregnum,
                       int startRegNum, int endRegNum, unsigned int instID, bool isHybridAlloc);

	void freeRegs( int regnum, int subregnum, int numwords, int instID);
    PhyRegsLocalRA * getAvaialableRegs() { return &availableRegs; }
};

class LinearScan
{
private:
    GlobalRA& gra;
    IR_Builder& builder;
	Mem_Manager& mem;
	PhyRegsManager& pregManager;
    PhyRegsLocalRA& initPregs;
	std::vector<LocalLiveRange*>& liveIntervals;
    std::list<InputLiveRange*, std_arena_based_allocator<InputLiveRange*>>& inputIntervals;
	std::list<LocalLiveRange*> active;
	PhyRegSummary* summary;

	void expireRanges( unsigned int );
    void expireInputRanges( unsigned int, unsigned int, unsigned int );
	void expireAllActive();
    bool allocateRegsFromBanks( LocalLiveRange* );
    bool allocateRegs(LocalLiveRange*, INST_LIST& instList, IR_Builder& builder, LLR_USE_MAP& LLRUseMap);
	void freeAllocedRegs( LocalLiveRange*, bool);
	void updateActiveList( LocalLiveRange* );
	void updateBitset( LocalLiveRange* );

	BitSet pregs;
	unsigned int simdSize;

    unsigned int globalLRSize;
    unsigned int *startGRFReg;
    unsigned int numRegLRA;

    unsigned int bank1StartGRFReg;
    unsigned int bank2StartGRFReg;
    unsigned int bank1_start;
    unsigned int bank1_end;
    unsigned int bank2_start;
    unsigned int bank2_end;

    bool useRoundRobin;
    bool doBankConflict;
    bool highInternalConflict;
    bool doSplitLLR;

public:
	LinearScan(GlobalRA& g, IR_Builder& pBuilder, std::vector<LocalLiveRange*>& localLiveIntervals, 
        std::list<InputLiveRange*, std_arena_based_allocator<InputLiveRange*>>& inputLivelIntervals, PhyRegsManager& pregMgr, PhyRegsLocalRA& pregs,
        Mem_Manager& memmgr, PhyRegSummary* s, unsigned int numReg, unsigned int glrs, 
        bool roundRobin, bool bankConflict, bool internalConflict, bool splitLLR, unsigned int simdS)
        : builder(pBuilder), mem(memmgr), pregManager(pregMgr), initPregs(pregs), 
        liveIntervals(localLiveIntervals), inputIntervals(inputLivelIntervals), summary(s), 
        pregs(pBuilder.getOptions()->getuInt32Option(vISA_TotalGRFNum) * NUM_WORDS_PER_GRF, false), simdSize(simdS),
        globalLRSize(glrs), numRegLRA(numReg), useRoundRobin(roundRobin), doBankConflict(bankConflict), highInternalConflict(internalConflict), doSplitLLR(splitLLR),
        gra(g)
	{

        //register number boundaries
        bank1_start = 0;
        bank1_end = SECOND_HALF_BANK_START_GRF - globalLRSize / 2 - 1;
        if (useRoundRobin)
        {//From middle to back
	        bank2_start = SECOND_HALF_BANK_START_GRF + (globalLRSize + 1) / 2;
	        bank2_end = numRegLRA - 1;
        }
        else
        { //From back to middle
	        bank2_start = numRegLRA - 1;
	        bank2_end = SECOND_HALF_BANK_START_GRF + (globalLRSize + 1) / 2;
        }

        //register number pointers
        bank1StartGRFReg = bank1_start;
        bank2StartGRFReg = bank2_start;

        //register pointer
        startGRFReg = &bank1StartGRFReg;

        int bank1AvailableRegNum = 0;
        for (int i = 0; i < SECOND_HALF_BANK_START_GRF; i++)
        {
            if (pregManager.getAvaialableRegs()->isGRFAvailable(i) && !pregManager.getAvaialableRegs()->isGRFBusy(i))
	            {
    	            bank1AvailableRegNum++;
	            }
        }
        pregManager.getAvaialableRegs()->setBank1AvailableRegNum(bank1AvailableRegNum);

        int bank2AvailableRegNum = 0;
        for (unsigned int i = SECOND_HALF_BANK_START_GRF; i < numRegLRA; i++)
        {
            if (pregManager.getAvaialableRegs()->isGRFAvailable(i) && !pregManager.getAvaialableRegs()->isGRFBusy(i))
	        {
	            bank2AvailableRegNum++;
	        }
        }
        pregManager.getAvaialableRegs()->setBank2AvailableRegNum(bank2AvailableRegNum);
	}

    void run( INST_LIST& instList, IR_Builder& builder, LLR_USE_MAP& LLRUseMap );

};

class PhyRegSummary
{
private:
    uint32_t totalNumGRF;
    std::vector<bool> GRFUsage;

public:
	PhyRegSummary(uint32_t numGRF) : totalNumGRF(numGRF)
	{
        GRFUsage.resize(totalNumGRF, false);
	}

    uint32_t getNumGRF() const { return totalNumGRF; }

	void* operator new(size_t sz, Mem_Manager& m) {return m.alloc(sz);}

	void markPhyRegs( G4_VarBase* pr, unsigned int words );

	bool isGRFBusy( int regnum ) { return GRFUsage[regnum]; }

	void printBusyRegs();
};
}
#endif // _INC_LOCALRA_H_
