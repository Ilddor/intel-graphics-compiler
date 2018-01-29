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

#ifndef __PHYREGUSAGE_H__
#define __PHYREGUSAGE_H__

#include "BuildIR.h"
#include "G4_Opcode.h"
#include "Gen4_IR.hpp"

enum ColorHeuristic {FIRST_FIT, ROUND_ROBIN};

// forward declares
namespace vISA
{
class VarBasis;
class LiveRange;
class GlobalRA;

class PhyRegUsageParms
{
public:
    GlobalRA& gra;
    G4_RegFileKind rFile;
    unsigned int maxGRFCanBeUsed;
    unsigned int& startARFReg;
    unsigned int& startFlagReg;
    unsigned int& startGRFReg;
    unsigned int& bank1_start;
    unsigned int& bank1_end;
    unsigned int& bank2_start;
    unsigned int& bank2_end;
    bool doBankConflict;
    bool* availableGregs;
    uint16_t* availableSubRegs;
    bool* availableAddrs;
    bool* availableFlags;
    uint8_t* weakEdgeUsage;

    PhyRegUsageParms(GlobalRA& g, G4_RegFileKind r, unsigned int m, unsigned int& startARF, unsigned int& startFlag, unsigned int& startGRF,
        unsigned int& bank1_s, unsigned int& bank1_e, unsigned int& bank2_s, unsigned int& bank2_e, bool doBC, bool* avaGReg,
        uint16_t* avaSubReg, bool* avaAddrs, bool* avaFlags, uint8_t* weakEdges) : gra(g), startARFReg(startARF), startFlagReg(startFlag),
        startGRFReg(startGRF), bank1_start(bank1_s), bank1_end(bank1_e), bank2_start(bank2_s), bank2_end(bank2_e)
    {
        doBankConflict = doBC;
        availableGregs = avaGReg;
        availableSubRegs = avaSubReg;
        availableAddrs = avaAddrs;
        availableFlags = avaFlags;
        weakEdgeUsage = weakEdges;
        maxGRFCanBeUsed = m;
        rFile = r;
    }
};

//
// track which registers are currently in use (cannot be assigned to other variables)
// For sub reg allocation, the granularity is UW/W (2 bytes). Doing so, we only need to
// handle even and odd alignment.
//
class PhyRegUsage
{
    GlobalRA& gra;
	unsigned maxGRFCanBeUsed;
	bool *availableGregs;         	// true if the reg is available for allocation
	uint16_t *availableSubRegs;     // each entry is a 16-bit value of the words that are free in a GRF
	bool *availableAddrs;		                // true if the reg is available for allocation
    bool *availableFlags;
    uint8_t* weakEdgeUsage;

	ColorHeuristic colorHeuristic;   // perform register assignment in first-fit/round-robin for GRFs
    G4_RegFileKind regFile;

	unsigned& startARFReg;						// round-robin reg  start bias
    unsigned& startFLAGReg;
	unsigned& startGRFReg;						// round-robin reg  start bias


    unsigned &bank1_start;
    unsigned &bank2_start;
    unsigned &bank1_end;
    unsigned &bank2_end;

    unsigned totalGRFNum;

    bool honorBankBias;       // whether we honor the bank bias assigned by the bank conflict avoidance heuristic
    bool overlapTest; // set to true only when current dcl has compatible ranges marked by augmentation

    struct PhyReg
    {
        int reg;
        int subreg; // in unit of words (0-15)
    }; // return type for findGRFSubReg

    PhyReg findGRFSubReg(const bool forbidden[],
        bool callerSaveBias,
        bool callerSaverBias,
        G4_Align align,
        G4_SubReg_Align subAlign,
        unsigned nwords);
    
	void findGRFSubRegFromRegs(int startReg,
	    int endReg,
	    int step,
	    PhyReg *phyReg,
	    G4_SubReg_Align subAlign,
	    unsigned nwords,
	    const bool forbidden[],
	    bool fromPartialOccupiedReg);
    
	PhyReg findGRFSubRegFromBanks(G4_Declare *dcl,
        const bool forbidden[],
        bool oneGRFBankDivision);
	
	void freeGRFSubReg(unsigned regNum, unsigned regOff, unsigned nwords, G4_Type ty);
	void freeContiguous(bool availRegs[], unsigned start, unsigned numReg, unsigned maxRegs);
	bool canGRFSubRegAlloc(G4_Declare* decl);
	bool findContiguousNoWrapGRF(bool availRegs[],
                                 const bool forbidden[],
								 G4_Align align,
								 unsigned numRegNeeded,
								 unsigned startPos,
								 unsigned endPos,
								 unsigned& idx);

	bool findContiguousNoWrapAddrFlag(bool availRegs[],
		  					     const bool forbidden[],
								 G4_SubReg_Align subAlign,
								 unsigned numRegNeeded,
								 unsigned startPos,
								 unsigned endPos,
								 unsigned& idx);
    
	bool allFree(bool availRegs[], unsigned maxRegs);

    bool findFreeRegs(bool availRegs[],
        const bool forbidden[],
        G4_Align align,
        unsigned numRegNeeded,
        unsigned startRegNum,
        unsigned endRegNum,
        unsigned& idx,
        bool gotoSecondBank,
        bool oneGRFBankDivision);

public:
    IR_Builder& builder;

	PhyRegPool& regPool; // all Physical Reg Operands

    PhyRegUsage(PhyRegUsageParms&);

    bool isOverlapValid(unsigned int, unsigned int);

    void setWeakEdgeUse(unsigned int reg, uint8_t index)
    {
        // Consider V1 is allocated to r10, r11, r12, r13.
        // Then following will be set eventually to model
        // compatible ranges:
        //  weakEdgeUsage[10] = 1;
        //  weakEdgeUsage[11] = 2;
        //  weakEdgeUsage[12] = 3;
        //  weakEdgeUsage[13] = 4;
        // This means some other compatible range cannot start 
        // at r7, r8, r9, r11, r12, r13. Another compatible range
        // can either have no overlap at all with this range (strong
        // edge), or it can start at r10 to have full 
        // overlap (weak edge).
        weakEdgeUsage[reg] = index;
    }

    uint8_t getWeakEdgeUse(unsigned int reg)
    {
        return weakEdgeUsage[reg];
    }

    void runOverlapTest(bool t)
    {
        overlapTest = t;
    }

    ~PhyRegUsage()
    {

    }

	bool assignRegs(bool  isSIMD16, 
		            VarBasis* var,
					const bool* forbidden,
					G4_Align  align,
					G4_SubReg_Align subAlign,
					ColorHeuristic colorHeuristic,
					float			 spillCost);

    bool assignGRFRegsFromBanks(VarBasis*	 varBasis,
         		             G4_Align  align,
							 const bool*     forbidden,
							 ColorHeuristic  heuristic,
							 bool oneGRFBankDivision);

    void markBusyForDclSplit(G4_RegFileKind kind,
        unsigned regNum,
        unsigned regOff,
        unsigned nunits,
        unsigned numRows);

	void markBusyGRF(unsigned regNum,
				  unsigned regOff,
				  unsigned nunits,
				  unsigned numRows)
	{
		MUST_BE_TRUE(numRows > 0 && nunits > 0, ERROR_INTERNAL_ARGUMENT);

		MUST_BE_TRUE(regNum + numRows <= maxGRFCanBeUsed,
			ERROR_UNKNOWN);

		//
		// sub reg allocation (allocation unit is word)
		//
		if (numRows == 1 && regOff + nunits < G4_GRF_REG_SIZE)
		{
			availableGregs[regNum] = false;
			uint16_t subregMask = getSubregBitMask(regOff, nunits);
			availableSubRegs[regNum] &= ~subregMask;
		}
		else // allocate whole registers
		{
			for (unsigned i = 0; i < numRows; i++)
			{
				availableGregs[regNum + i] = false;
				availableSubRegs[regNum + i] = 0;
			}
		}
	}

	void markBusyAddress(unsigned regNum,
				  unsigned regOff,
				  unsigned nunits,
		unsigned numRows)
	{
		MUST_BE_TRUE(regNum == 0 && regOff + nunits <= getNumAddrRegisters(),
			ERROR_UNKNOWN);
		for (unsigned i = regOff; i < regOff + nunits; i++)
			availableAddrs[i] = false;
	}

	void markBusyFlag(unsigned regNum,
		unsigned regOff,
		unsigned nunits,
		unsigned numRows)
	{
		MUST_BE_TRUE(regNum == 0 && regOff + nunits <= getNumFlagRegisters(),
			ERROR_UNKNOWN);
		for (unsigned i = regOff; i < regOff + nunits; i++)
			availableFlags[i] = false;
	}

	static unsigned numAllocUnit(unsigned nelems, G4_Type ty)
	{
		//
		// we allocate sub reg in 2-byte granularity
		//
		unsigned nbytes = nelems* G4_Type_Table[ty].byteSize;
		return nbytes/G4_WSIZE + nbytes%G4_WSIZE;
	}

	// translate offset to allocUnit
	static unsigned offsetAllocUnit(unsigned nelems, G4_Type ty)
	{

		unsigned nbytes = nelems* G4_Type_Table[ty].byteSize;
		//RA allocate register in unit of G4_WSIZE bytes
		//pre-assigned register may start from nbytes%G4_WSIZE != 0, i.e, within an allocUnit
		return nbytes/G4_WSIZE;
	}

    void updateRegUsage(LiveRange* lr, LiveRange* lrs[]);

    uint16_t getSubregBitMask(uint32_t start, uint32_t num) const
    {
        MUST_BE_TRUE(num > 0 && start+num <= 16, "illegal number of words");
        uint32_t mask = ((1 << num) - 1) << start;
        MUST_BE_TRUE(mask <= 0xFFFF, "illegal subreg mask");
        return (uint16_t) mask;
    }

    void emit(std::ostream& output)
    {
        output << "available GRFs: ";
        for (unsigned int i = 0; i < totalGRFNum; i++)
        {
            if (availableGregs[i])
            {
                output << i << " ";
            }
        }
        output << "\n";
    }

private:

	void freeRegs(VarBasis* var);

	bool findContiguousGRF(bool availRegs[],
	 					   const bool forbidden[],
						   G4_Align align,
						   unsigned numRegNeeded,
						   unsigned maxRegs,
						   unsigned& startReg, // inout
						   unsigned& idx,      // output
                           bool isCalleeSaveBias = false,
                           bool isEOTSrc = false);
	bool findContiguousAddrFlag(bool availRegs[],
	 					   const bool forbidden[],
                           G4_SubReg_Align subAlign,
						   unsigned numRegNeeded,
						   unsigned maxRegs,
						   unsigned& startReg, // inout
						   unsigned& idx,      // output
                           bool isCalleeSaveBias = false,
                           bool isEOTSrc = false);

    bool findContiguousGRFFromBanks(G4_Declare *dcl, bool availRegs[],
								 const bool forbidden[],
								 G4_Align align,
								 unsigned& idx,
								 bool oneGRFBankDivision);

    // find contiguous free words in a registers
    int findContiguousWords(uint16_t words, G4_SubReg_Align alignment, int numWord) const;
};

class VarBasis
{
	G4_RegVar* var;
	G4_RegFileKind regKind;
	bool* forbidden;
	bool calleeSaveBias; // indicates if the var is biased to get a callee-save assignment or not
	bool callerSaveBias; // indicates if the var is biased to get a caller-save assignment or not
    bool isEOTSrc; //Gen7 only, Whether the liveRange is the message source of an EOT send
    bool retIp;   // variable is the return ip and should not be spilled
    int numForbidden;

public:
	VarBasis(G4_RegVar* v, const Options *opt);
    void allocForbidden(vISA::Mem_Manager& mem, bool reserveStackCallRegs, unsigned reserveSpillSize, unsigned rerservedRegNum);
    void allocForbiddenCallerSave(vISA::Mem_Manager& mem, G4_Kernel* kernel);
    void allocForbiddenCalleeSave(vISA::Mem_Manager& mem, G4_Kernel* kernel);
	const bool* getForbidden()        {return forbidden;}
    void markForbidden(int reg, int numReg)
    {
        MUST_BE_TRUE(((int) getForbiddenVectorSize()) >= reg + numReg, "forbidden register is out of bound");
        for (int i = reg; i < reg + numReg; ++i)
        {
            forbidden[i] = true;
        }
        numForbidden = -1;
    }
    int getNumForbidden()
    {
        if (forbidden == nullptr)
        {
            return 0;
        }
        if (numForbidden == -1)
        {
            numForbidden = 0;
            for (int i = 0, size = getForbiddenVectorSize(); i < size; ++i)
            {
                if (forbidden[i])
                {
                    ++numForbidden;
                }
            }
        }
        return numForbidden;
    }
	G4_RegVar* getVar()			{return var;}
	virtual G4_VarBase* getPhyReg()		{return var->getPhyReg();}
	virtual unsigned    getPhyRegOff()  {return var->getPhyRegOff();}
	virtual void setPhyReg(G4_VarBase* pr, unsigned off)
	{
		var->setPhyReg(pr, off);
	}
	virtual void resetPhyReg() {return var->resetPhyReg();}
	G4_RegFileKind getRegKind() {return regKind;}
	void dump();
	virtual void emit(std::ostream& output, bool symbolreg=false)
	{
		var->emit (output, symbolreg);
	}

	void setCalleeSaveBias(bool v) {calleeSaveBias = v;}
	bool getCalleeSaveBias() {return calleeSaveBias;}

	void setCallerSaveBias(bool v) {callerSaveBias = v;}
	bool getCallerSaveBias() {return callerSaveBias;}

    void setEOTSrc() { isEOTSrc = true; }
    bool getEOTSrc() const { return isEOTSrc; }

    void setRetIp() { retIp = true; }
    bool isRetIp() const { return retIp; }

private:
    const Options *m_options;
	unsigned getForbiddenVectorSize();
    void allocForbiddenVector(vISA::Mem_Manager& mem);
};
}
#endif // __PHYREGUSAGE_H__
