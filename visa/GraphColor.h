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

#ifndef __GRAPHCOLOR_H__
#define __GRAPHCOLOR_H__

#include "RegAlloc.h"
#include "Gen4_IR.hpp"
#include "SpillManagerGMRF.h"
#include <list>
#include <unordered_set>
#include <limits>
#include "RPE.h"

#include "BitSet.h"

#define BITS_DWORD 32
#define SCRATCH_MSG_LIMIT (128 * 1024)

extern unsigned int BitMask[BITS_DWORD];
namespace vISA
{
    const float MAXSPILLCOST = (std::numeric_limits<float>::max());
    const float MINSPILLCOST = -(std::numeric_limits<float>::max());

    class BankConflictPass
    {
    private:
        GlobalRA& gra;

        BankConflict setupBankAccordingToSiblingOperand(BankConflict assignedBank, unsigned int offset, bool oneGRFBank);
        bool hasInternalConflict2Srcs(BankConflict*srcBC);
        void setupBankConflictsForDecls(G4_Declare* dcl_1, G4_Declare* dcl_2, unsigned int offset1, unsigned int offset2,
            BankConflict &srcBC1, BankConflict &srcBC2, int &bank1RegNum, int &bank2RegNum, float GRFRatio, bool oneGRFBank);
        void setupEvenOddBankConflictsForDecls(G4_Declare * dcl_1, G4_Declare * dcl_2, unsigned int offset1, unsigned int offset2,
            BankConflict &srcBC1, BankConflict &srcBC2);
        void setupBankConflictsOneGRFOld(G4_INST* inst, int &bank1RegNum, int &bank2RegNum, float GRFRatio, unsigned int &internalConflict);
        void setupBankConflictsforTwoGRFs(G4_INST* inst);
        void setupBankConflictsForBB(G4_BB* bb, unsigned int &threeSourceInstNum, unsigned int &sendInstNum, unsigned int numRegLRA,
            unsigned int & internalConflict);
        bool hasInternalConflict3Srcs(BankConflict *srcBC);
        void setupBankForSrc0(G4_INST* inst, G4_INST* prevInst);
        void setupToEvenBank(G4_INST* inst);
        void getBanks(G4_INST* inst, BankConflict *srcBC, G4_Declare **dcls, G4_Declare **opndDcls, unsigned int *offset);
        void getPrevBanks(G4_INST* inst, BankConflict *srcBC, G4_Declare **dcls, G4_Declare **opndDcls, unsigned int *offset);



    public:
        bool setupBankConflictsForKernel(G4_Kernel& kernel, bool doLocalRR, bool &threeSourceCandidate, unsigned int numRegLRA, bool &highInternalConflict);

        BankConflictPass(GlobalRA& g) : gra(g)
        {

        }
    };

class LiveRange final : public VarBasis
{
    unsigned numRegNeeded;
    unsigned degree;
    unsigned refCount;
    AssignedReg reg;
    float spillCost;
    bool active;
    bool isInfiniteCost;
    bool isCandidate;
    bool isPseudoNode;
    GlobalRA& gra;

public:

    LiveRange(G4_RegVar* v, GlobalRA&, const Options *opt);

    void* operator new(size_t sz, vISA::Mem_Manager& m){ return m.alloc(sz); }

    void setDegree(unsigned d)  {degree = d;}
    unsigned getDegree()        {return degree;}

    unsigned getNumRegNeeded()  {return numRegNeeded;}

    void subtractDegree(unsigned d)
    {
        MUST_BE_TRUE(d <= degree, ERROR_INTERNAL_ARGUMENT);
        degree -= d;
    }

    void setActive(bool v) {active = v;}
    bool getActive() {return active;}

    void dump()
    {
        VarBasis::dump();
    }

    virtual void emit(std::ostream& output, bool symbolreg=false)
    {
        output << getVar ()->getDeclare ()->getName();
        if (reg.phyReg != NULL)
        {
            output << "(";
            reg.phyReg->emit(output);
            output << '.' << reg.subRegOff << ':';
            output << G4_Type_Table[getVar ()->getDeclare()->getElemType()].str << ")";
        }
        output << "(size = " << getVar()->getDeclare()->getByteSize() <<
            ", spill cost = " << getSpillCost() << ", degree = " << getDegree() << ")";
    }

    unsigned getRefCount()  {return refCount;}
    void setRefCount(unsigned count) {refCount = count;}

    float getSpillCost()  {return spillCost;}
    void setSpillCost(float cost) {spillCost = cost;}

    bool getIsInfiniteSpillCost() { return isInfiniteCost; }
    void checkForInfiniteSpillCost(INST_LIST& instList, std::list<G4_INST*>::reverse_iterator& it);

    G4_VarBase* getPhyReg()
    {
        return reg.phyReg;
    }

    virtual unsigned getPhyRegOff()
    {
        return reg.subRegOff;
    }

    void setPhyReg(G4_VarBase* pr, unsigned off)
    {
        MUST_BE_TRUE(pr->isPhyReg(), ERROR_UNKNOWN);
        reg.phyReg = pr;
        reg.subRegOff = off;
    }

    virtual void resetPhyReg()
    {
        reg.phyReg = NULL;
        reg.subRegOff = 0;
    }

    bool getIsPseudoNode() const { return isPseudoNode; }
    void setIsPseudoNode() { isPseudoNode = true; }
};
}
typedef std::list<vISA::LiveRange*> LIVERANGE_LIST;
typedef std::list<vISA::LiveRange*>::iterator LIVERANGE_LIST_ITER;

// A mapping from the pseudo decl created for caller save/restore, to the ret val
// This is used in augmentIntfGraph to prune interference edges for fcall ret val
typedef std::map<vISA::G4_Declare*, vISA::G4_Declare*> FCALL_RET_MAP;
typedef std::map<vISA::G4_Declare*, vISA::G4_Declare*>::iterator FCALL_RET_MAP_ITER;

typedef std::map<vISA::G4_Declare*, std::pair<vISA::G4_INST*, unsigned int>> CALL_DECL_MAP;
typedef std::map<vISA::G4_Declare*, std::pair<vISA::G4_INST*, unsigned int>>::iterator CALL_DECL_MAP_ITER;

//
// A bit array records all interference information.
// (2D matrix is flatten to 1D array)
// Since the interference information is symmetric, we can use only
// half of the size. To simplify the implementation, we use the full
// size of the bit array.
//
namespace vISA
{
    class Augmentation
    {
    private:
        G4_Kernel& kernel;
        Interference& intf;
        GlobalRA& gra;
        LivenessAnalysis& liveAnalysis;
        LiveRange** lrs;
        FCALL_RET_MAP& fcallRetMap;
        CALL_DECL_MAP callDclMap;
        std::vector<G4_Declare*> sortedIntervals;
        std::list<G4_Declare*> defaultMask;
        std::list<G4_Declare*> nonDefaultMask;
        Mem_Manager& m;

        bool updateDstMaskForScatter(G4_INST* inst, unsigned char* mask);
        void updateDstMask(G4_INST* inst, bool checkCmodOnly);
        static unsigned int getByteSizeFromMask(AugmentationMasks type);
        bool isDefaultMaskDcl(G4_Declare* dcl, unsigned int simdSize, AugmentationMasks type);
        bool isDefaultMaskSubDeclare(unsigned char* mask, unsigned int lb, unsigned int rb, G4_Declare* dcl, unsigned int simdSize);
        void markNonDefaultMaskForSubDcl(G4_Declare *dcl, unsigned lb, unsigned rb, unsigned int simdSize);
        bool verifyMaskIfInit(G4_Declare* dcl, AugmentationMasks mask);
        bool checkGRFPattern3(G4_Declare* dcl, G4_DstRegRegion* dst, unsigned maskOff,
            unsigned int lb, unsigned int rb, unsigned int execSize);
        bool checkGRFPattern2(G4_Declare* dcl, G4_DstRegRegion* dst, unsigned maskOff,
            unsigned int lb, unsigned int rb, unsigned int execSize);
        bool checkGRFPattern1(G4_Declare* dcl, G4_DstRegRegion* dst, unsigned maskOff,
            unsigned int lb, unsigned int rb, unsigned int execSize);
        void markNonDefaultDstRgn(G4_INST* inst, G4_Operand* opnd);
        bool markNonDefaultMaskDef();
        G4_BB* getTopmostBBDst(G4_BB* src, G4_BB* end, G4_BB* origSrc, unsigned int traversal);
        void updateStartIntervalForSubDcl(G4_Declare* dcl, G4_INST* curInst, G4_Operand *opnd);
        void updateEndIntervalForSubDcl(G4_Declare* dcl, G4_INST* curInst, G4_Operand *opnd);
        void updateStartInterval(G4_Declare* dcl, G4_INST* curInst);
        void updateEndInterval(G4_Declare* dcl, G4_INST* curInst);
        void updateStartIntervalForLocal(G4_Declare* dcl, G4_INST* curInst, G4_Operand *opnd);
        void updateEndIntervalForLocal(G4_Declare* dcl, G4_INST* curInst, G4_Operand *opnd);
        void buildLiveIntervals();
        void clearIntervalInfo();
        void sortLiveIntervals();
        unsigned int getEnd(G4_Declare*& dcl);
        bool isNoMask(G4_Declare* dcl, unsigned int size);
        bool isConsecutiveBits(G4_Declare* dcl, unsigned int size);
        bool isCompatible(G4_Declare* testDcl, G4_Declare* biggerDcl);
        void buildInterferenceIncompatibleMask();
        void expireIntervals(unsigned int startIdx);
        void buildSIMDIntfDcl(G4_Declare* newDcl, bool isCall);
        void buildSIMDIntfAll(G4_Declare* newDcl);
        void handleSIMDIntf(G4_Declare* firstDcl, G4_Declare* secondDcl, bool isCall);

    public:
        Augmentation(G4_Kernel& k, Interference& i, LivenessAnalysis& l, LiveRange* ranges[], GlobalRA& g);

        void augmentIntfGraph();
    };

    class Interference
    {
        friend class Augmentation;

    protected:
        // This stores compatible ranges for each variable. Such
        // compatible ranges will not be present in sparseIntf set.
        // We store G4_Declare* instead of id is because variables
        // allocated by LRA will not have a valid id.
        std::map<G4_Declare*, std::vector<G4_Declare*>> compatibleSparseIntf;

        // list of fake declares for physical GRF
        std::vector<G4_Declare*> localRADcls;

    private:
        GlobalRA& gra;
        G4_Kernel& kernel;
        LiveRange**& lrs;
        IR_Builder& builder;
        unsigned maxId;
        unsigned splitStartId;
        unsigned splitNum;
        unsigned int* matrix;
        LivenessAnalysis* liveAnalysis = nullptr;

        std::vector<std::vector<unsigned int>> sparseIntf;

        // sparse intefernece matrix.
        // we don't directly update spraseIntf to ensure uniqueness
        // like dense matrix, interference is not symmetric (that is, if v1 and v2 interfere and v1 < v2,
        // we insert (v1, v2) but not (v2, v1)) for better cache behavior
        std::vector<std::unordered_set<uint32_t> > sparseMatrix;
        const uint32_t denseMatrixLimit = 32768;

        void updateLiveness(BitSet& live, uint32_t id, bool val)
        {
            live.set(id, val);
        }

        // used by hybrid RA, this marks the first decl in kernel.Declares
        // that is not a fake declare used to model physical GRF liveness
        G4_Declare* firstOrigDcl = nullptr;

        void getLocalRADcls();

    public:
        Interference(LivenessAnalysis* l, LiveRange**& lr, unsigned n, unsigned ns, unsigned nm,
            G4_Declare* firstOrigDcl, GlobalRA& g);

        ~Interference()
        {
            if (useDenseMatrix())
            {
                delete[] matrix;
            }
        }

        std::vector<G4_Declare*>* getCompatibleSparseIntf(G4_Declare* d)
        {
            if (compatibleSparseIntf.size() > 0)
            {
                auto it = compatibleSparseIntf.find(d);
                if (it == compatibleSparseIntf.end())
                {
                    return nullptr;
                }
                return &((*it).second);
            }
            return nullptr;
        }

        void init(vISA::Mem_Manager& m)
        {
            if (useDenseMatrix())
            {
                unsigned N = getRowSize() * maxId;
                matrix = new uint32_t[N];
                memset(matrix, 0, N * sizeof(int));
            }
            else
            {
                sparseMatrix.resize(maxId);
            }
        }

        bool useDenseMatrix() const
        {
            return maxId < denseMatrixLimit;
        }

        // Clean data filled while computing interference.
        void clear()
        {
            sparseIntf.clear();
            if (useDenseMatrix())
            {
                unsigned N = getRowSize() * maxId;
                std::memset(matrix, 0, N * sizeof(int));
            }
            else
            {
                for (auto &I : sparseMatrix)
                {
                    I.clear();
                }
            }
        }

        void computeInterference();
        bool interfereBetween(unsigned v1, unsigned v2) const;
        inline unsigned int getInterferenceBlk(unsigned idx) const
        {
            assert(useDenseMatrix() && "matrix is not initialized");
            return matrix != nullptr ? matrix[idx] : 0;
        }
        inline unsigned int getRowSize() const
        {
            return maxId / BITS_DWORD + 1;
        }

        std::vector<unsigned int>& getSparseIntfForVar(unsigned int id) { return sparseIntf[id]; }

        // Only upper-half matrix is now used in intf graph.
        inline void safeSetInterference(unsigned v1, unsigned v2)
        {
            // Assume v1 < v2
            if (useDenseMatrix())
            {
                unsigned col = v2 / BITS_DWORD;
                matrix[v1 * getRowSize() + col] |= BitMask[v2 - col * BITS_DWORD];
            }
            else
            {
                sparseMatrix[v1].emplace(v2);
            }
        }

        inline void setBlockInterferencesOneWay(unsigned v1, unsigned col, unsigned block)
        {
            if (useDenseMatrix())
            {
#ifdef _DEBUG
                MUST_BE_TRUE(sparseIntf.size() == 0, "Updating intf graph matrix after populating sparse intf graph");
#endif

                matrix[v1 * getRowSize() + col] |= block;
            }
            else
            {
                auto&& intfSet = sparseMatrix[v1];
                for (int i = 0; i < BITS_DWORD; ++i)
                {
                    if (block & BitMask[i])
                    {
                        uint32_t v2 = col * BITS_DWORD + i;
                        intfSet.emplace(v2);
                    }
                }
            }
        }

        bool varSplitCheckBeforeIntf(unsigned v1, unsigned v2);

        void checkAndSetIntf(unsigned v1, unsigned v2)
        {
            if (v1 < v2)
            {
                safeSetInterference(v1, v2);
            }
            else if (v1 > v2)
            {
                safeSetInterference(v2, v1);
            }
        }

        void addCalleeSaveBias(BitSet& live);
        void buildInterferenceAtBBExit(G4_BB* bb, BitSet& live);
        void buildInterferenceWithinBB(G4_BB* bb, BitSet& live, G4_Declare* arg, G4_Declare* ret);
        void buildInterferenceForDst(G4_BB* bb, BitSet& live, G4_INST* inst, std::list<G4_INST*>::reverse_iterator i, G4_DstRegRegion* dst);
        void buildInterferenceForFcall(G4_BB* bb, BitSet& live, G4_INST* inst, std::list<G4_INST*>::reverse_iterator i, G4_VarBase* regVar);

        inline void filterSplitDclares(unsigned startIdx, unsigned endIdx, unsigned n, unsigned col, unsigned &elt, bool is_split);

        void buildInterferenceWithLive(BitSet& live, unsigned i);
        void buildInterferenceWithSubDcl(unsigned lr_id, G4_Operand *opnd, BitSet& live, bool setLive, bool setIntf);
        void buildInterferenceWithAllSubDcl(unsigned v1, unsigned v2);

        void markInterferenceForSend(G4_BB* bb, G4_INST* inst, G4_DstRegRegion* dst);

        void dumpInterference() const;
        void interferenceVerificationForSplit() const;

        void buildInterferenceWithLocalRA(G4_BB* bb);

        void buildInterferenceAmongLiveIns();

        void generateSparseIntfGraph();
        bool isStrongEdgeBetween(G4_Declare*, G4_Declare*);
    };

    class GraphColor
    {
        GlobalRA& gra;

        unsigned totalGRFRegCount; // .reg_count_total
        unsigned numVar;
        unsigned numSplitStartID;
        unsigned numSplitVar;
        unsigned *spAddrRegSig;
        Interference intf;
        PhyRegPool& regPool;
        IR_Builder& builder;
        LiveRange** lrs;
        bool isHybrid;
        LIVERANGE_LIST spilledLRs;
        bool requireCallerSaveRestoreCode;
        bool requireCalleeSaveRestoreCode;
        bool requireA0CallerSaveRestoreCode;
        bool requireFlagCallerSaveRestoreCode;
        bool forceSpill;
        vISA::Mem_Manager mem;
        const Options *m_options;

        unsigned evenTotalDegree;
        unsigned oddTotalDegree;
        unsigned evenTotalRegNum;
        unsigned oddTotalRegNum;
        unsigned evenMaxRegNum;
        unsigned oddMaxRegNum;

        G4_Kernel& kernel;
        LivenessAnalysis& liveAnalysis;

        std::vector<LiveRange*> colorOrder;
        LIVERANGE_LIST unconstrainedWorklist;
        LIVERANGE_LIST constrainedWorklist;
        unsigned int numColor = 0;

#define GRAPH_COLOR_MEM_SIZE 16*1024

        // This function returns the weight of interference edge lr1--lr2,
        // which is used for computing the degree of lr1.
        //
        // When there is no alignment restriction, we should use the normal weight,
        // which is lr1_nreg + lr2_nreg - 1.
        //
        // Otherewise, we need to take into account additional space that may be
        // required because of the alignment restriction. For example,
        // if lr1 has even alignment and lr2 has no alignment restriction,
        // we need to consider the following cases that would require the
        // maximal available GRF space for successful allocation:
        // 1) lr1's size is odd, lr2's size is odd and lr2's start position is even,
        //    the total space required would be (lr1_nreg + lr2_nreg + 1)
        // 2) lr1's size is odd, lr2's size is even and lr2's start position is even,
        //    the total space required would be (lr1_nreg + lr2_nreg)
        // 3) lr1's size is even, lr2's size is odd and lr2's start position is odd,
        //    the total space required would be (lr1_nreg + lr2_nreg)
        // 4) lr1's size is even, lr2's size is even and lr2's start position is odd,
        //    the total space required would be (lr1_nreg + lr2_nreg + 1)
        // The above logic can be simplified to the following formula:
        //    lr1_nreg + lr2_nreg + 1 - ((lr1_nreg + lr2_nreg) % 2)
        //
        // If both lr1 and lr2 have even alignment restriction,
        // we need to consider the following cases that would require the
        // maximal available GRF space for successful allocation:
        // 1) lr1's size is odd, lr2's size is odd and lr2's start position is even,
        //    the total space required would be (lr1_nreg + lr2_nreg + 1)
        // 2) lr1's size is odd, lr2's size is even and lr2's start position is even,
        //    the total space required would be (lr1_nreg + lr2_nreg)
        // 3) lr1's size is even, lr2's size is odd and lr2's start position is even,
        //    the total space required would be (lr1_nreg + lr2_nreg)
        // 4) lr1's size is even, lr2's size is even and lr2's start position is even,
        //    the total space required would be (lr1_nreg + lr2_nreg - 1)
        // The above logic can be simplified to the following formula:
        //    lr1_nreg + lr2_nreg - 1 + (lr1_nreg % 2) + (lr2_nreg % 2)
        //
        unsigned edgeWeightGRF(LiveRange* lr1, LiveRange* lr2)
        {
            G4_Align lr1_align = lr1->getVar()->getAlignment();
            G4_Align lr2_align = lr2->getVar()->getAlignment();
            unsigned lr1_nreg = lr1->getNumRegNeeded();
            unsigned lr2_nreg = lr2->getNumRegNeeded();

            MUST_BE_TRUE(lr1_align == Either ||
                lr1_align == Even ||
                lr2_align == Either ||
                lr2_align == Even, "Found unsupported GRF alignment in register allocation!");

            if (lr1_align == Either)
            {
                return  lr1_nreg + lr2_nreg - 1;
            }
            else if (lr2_align == Either)
            {
                unsigned sum = lr1_nreg + lr2_nreg;
                return sum + 1 - ((sum) % 2);
            }
            else if (lr2_align == Even)
            {
                return lr1_nreg + lr2_nreg - 1 + (lr1_nreg % 2) + (lr2_nreg % 2);
            }
            else
            {
                MUST_BE_TRUE(false, "Found unsupported alignment in register allocation!");
                return 0;
            }
        }

        unsigned edgeWeightARF(LiveRange* lr1, LiveRange* lr2)
        {
            if (lr1->getRegKind() == G4_FLAG)
            {
                G4_SubReg_Align lr1_align = lr1->getVar()->getSubRegAlignment();
                G4_SubReg_Align lr2_align = lr2->getVar()->getSubRegAlignment();
                unsigned lr1_nreg = lr1->getNumRegNeeded();
                unsigned lr2_nreg = lr2->getNumRegNeeded();

                if (lr1_align == Any)
                {
                    return  lr1_nreg + lr2_nreg - 1;
                }
                else if (lr1_align == Even_Word && lr2_align == Any)
                {
                    return lr1_nreg + lr2_nreg + 1 - ((lr1_nreg + lr2_nreg) % 2);
                }
                else if (lr1_align == Even_Word && lr2_align == Even_Word)
                {
                    return lr1_nreg + lr2_nreg - 1 + (lr1_nreg % 2) + (lr2_nreg % 2);
                }
                else
                {
                    MUST_BE_TRUE(false, "Found unsupported subRegAlignment in flag register allocation!");
                    return 0;
                }
            }
            else if (lr1->getRegKind() == G4_ADDRESS)
            {
                G4_SubReg_Align lr1_align = lr1->getVar()->getSubRegAlignment();
                G4_SubReg_Align lr2_align = lr2->getVar()->getSubRegAlignment();
                unsigned lr1_nreg = lr1->getNumRegNeeded();
                unsigned lr2_nreg = lr2->getNumRegNeeded();

                if (lr1_align == Any)
                {
                    return  lr1_nreg + lr2_nreg - 1;
                }
                else if (lr1_align == Four_Word && lr2_align == Any)
                {
                    return lr1_nreg + lr2_nreg + 3 - (lr1_nreg + lr2_nreg) % 4;
                }
                else if (lr1_align == Four_Word && lr2_align == Four_Word)
                {
                    return lr1_nreg + lr2_nreg - 1 + (4 - lr1_nreg % 4) % 4 + (4 - lr2_nreg % 4) % 4;
                }
                else if (lr1_align == Eight_Word && lr2_align == Any)
                {
                    return lr1_nreg + lr2_nreg + 7 - (lr1_nreg + lr2_nreg) % 8;
                }
                else if (lr1_align == Eight_Word && lr2_align == Four_Word)
                {
                    if (((8 - lr1_nreg % 8) % 8) >= 4)
                        return lr1_nreg + lr2_nreg - 1 + (8 - lr1_nreg % 8) % 8 - 4;
                    return lr1_nreg + lr2_nreg - 1 + (8 - lr1_nreg % 8) % 8 +
                        (4 - lr2_nreg % 4) % 4;
                }
                else if (lr1_align == Eight_Word && lr2_align == Eight_Word)
                {
                    return lr1_nreg + lr2_nreg - 1 + (8 - lr1_nreg % 8) % 8 +
                        (8 - lr2_nreg % 8) % 8;
                }
                else
                {
                    MUST_BE_TRUE(false, "Found unsupported subRegAlignment in address register allocation!");
                    return 0;
                }
            }
            MUST_BE_TRUE(false, "Found unsupported ARF reg type in register allocation!");
            return 0;
        }

        void computeDegreeForGRF();
        void computeDegreeForARF();
        void computeSpillCosts(bool useSplitLLRHeuristic);
        void determineColorOrdering();
        void removeConstrained();
        void relaxNeighborDegreeGRF(LiveRange* lr);
        void relaxNeighborDegreeARF(LiveRange* lr);
        bool assignColors(ColorHeuristic heuristicGRF, bool doBankConflict, bool highInternalConflict);

        void clearSpillAddrLocSignature()
        {
            memset(spAddrRegSig, 0, getNumAddrRegisters() * sizeof(unsigned));
        }
        void pruneActiveSpillAddrLocs(G4_DstRegRegion*, unsigned, G4_Type);
        void updateActiveSpillAddrLocs(G4_DstRegRegion*, G4_SrcRegRegion*, unsigned execSize);
        bool redundantAddrFill(G4_DstRegRegion*, G4_SrcRegRegion*, unsigned execSize);

    public:
        GraphColor(LivenessAnalysis& live, unsigned totalGRF, bool hybrid, bool forceSpill_, G4_Declare* firstOrigDcl = nullptr);

        static const char* StackCallStr;

        const Options * getOptions() { return m_options; }

        bool regAlloc(
            bool doBankConflictReduction,
            bool highInternalConflict,
            bool reserveSpillReg, unsigned& spillRegSize, unsigned& indrSpillRegSize, RPE* rpe);
        bool requireSpillCode() { return !spilledLRs.empty(); }
        Interference * getIntf() { return &intf; }
        void createLiveRanges(unsigned reserveSpillSize = 0);
        LiveRange ** getLiveRanges() const { return lrs; }
        const LIVERANGE_LIST & getSpilledLiveRanges() const { return spilledLRs; }
        void confirmRegisterAssignments();
        void resetTemporaryRegisterAssignments();
        void cleanupRedundantARFFillCode();
        void addA0SaveRestoreCode();
        void addFlagSaveRestoreCode();
        void addSaveRestoreCode(unsigned);
        void addCallerSaveRestoreCode();
        void addCalleeSaveRestoreCode();
        void addFileScopeSaveRestoreCode();
        void addGenxMainStackSetupCode();
        void addCalleeStackSetupCode();
        void saveSubRegs(
            unsigned startReg, unsigned startSubReg, unsigned size, G4_Declare* scratchRegDcl, G4_Declare* framePtr,
            unsigned frameOffset, INST_LIST& instList, INST_LIST_ITER insertIt);
        void saveRegs(
            unsigned startReg, unsigned owordSize, G4_Declare* scratchRegDcl, G4_Declare* framePtr,
            unsigned frameOffset, INST_LIST& instList, INST_LIST_ITER insertIt);
        void saveActiveRegs(
            std::vector<bool>& saveRegs, unsigned startReg,
            unsigned frameOffset, INST_LIST& instList, INST_LIST_ITER insertIt);
        void restoreSubRegs(
            unsigned startReg, unsigned startSubReg, unsigned size, G4_Declare* scratchRegDcl, G4_Declare* framePtr,
            unsigned frameOffset, INST_LIST& instList, INST_LIST_ITER insertIt);
        void restoreRegs(
            unsigned startReg, unsigned owordSize, G4_Declare* scratchRegDcl, G4_Declare* framePtr,
            unsigned frameOffset, INST_LIST& instList, INST_LIST_ITER insertIt);
        void restoreActiveRegs(
            std::vector<bool>& restoreRegs, unsigned startReg,
            unsigned frameOffset, INST_LIST& instList, INST_LIST_ITER insertIt);
        void OptimizeActiveRegsFootprint(std::vector<bool>& saveRegs);
        void OptimizeActiveRegsFootprint(std::vector<bool>& saveRegs, std::vector<bool>& retRegs);
        void saveFileScopeVar(G4_RegVar* filescopeVar, INST_LIST& instList, INST_LIST_ITER insertIt);
        void restoreFileScopeVar(G4_RegVar* filescopeVar, INST_LIST& instList, INST_LIST_ITER insertIt);
        void dumpRegisterPressure();
    };

    class RAVarInfo
    {
    public:
        unsigned numSplit = 0;
        unsigned int bb_id = UINT_MAX;      // block local variable's block id.
        G4_Declare* splittedDCL = nullptr;
        LocalLiveRange* localLR = nullptr;
        unsigned int numRefs = 0;
        BankConflict conflict = BANK_CONFLICT_NONE;      // used to indicate bank that should be assigned to dcl if possible
        G4_INST* startInterval = nullptr;
        G4_INST* endInterval = nullptr;
        unsigned char* mask = nullptr;
        AugmentationMasks maskType = AugmentationMasks::Undetermined;
        std::vector<G4_Declare*> subDclList;
        unsigned int subOff = 0;
    };

    class GlobalRA
    {
    private:
        template <class REGION_TYPE> static unsigned getRegionDisp(REGION_TYPE * region);
        unsigned getRegionByteSize(G4_DstRegRegion * region, unsigned execSize);
        static unsigned owordMask();
        static bool owordAligned(unsigned offset);
        template <class REGION_TYPE> bool isUnalignedRegion(REGION_TYPE * region, unsigned execSize);
        bool shouldPreloadDst(G4_DstRegRegion* spilledRangeRegion, uint8_t execSize, G4_INST* instContext, G4_BB* curBB);
        static unsigned sendBlockSizeCode(unsigned owordSize);
        void updateDefSet(std::set<G4_Declare*>& defs, G4_Declare* referencedDcl);
        void detectUndefinedUses(LivenessAnalysis& liveAnalysis, G4_Kernel& kernel);
        void markBlockLocalVar(G4_RegVar* var, unsigned bbId);
        void markBlockLocalVars(G4_BB* bb, Mem_Manager& mem, bool doLocalRA);
        void computePhyReg();
        void fixAlignment();

        RAVarInfo defaultValues;
        std::vector<RAVarInfo> vars;

        void resize(unsigned int id)
        {
            if (id >= vars.size())
                vars.resize(id + 1);
        }

    public:
        G4_Kernel& kernel;
        IR_Builder& builder;
        PhyRegPool& regPool;
        PointsToAnalysis& pointsToAnalysis;
        FCALL_RET_MAP fcallRetMap;

        // RA specific fields
        unsigned int getSplitVarNum(G4_Declare* dcl)
        {
            auto dclid = dcl->getDeclId();
            if (dclid >= vars.size())
            {
                return defaultValues.numSplit;
            }
            return vars[dclid].numSplit;
        }

        void setSplitVarNum(G4_Declare* dcl, unsigned int val)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            vars[dclid].numSplit = val;
        }

        unsigned int getBBId(G4_Declare* dcl)
        {
            auto dclid = dcl->getDeclId();
            if (dclid >= vars.size())
            {
                return defaultValues.bb_id;
            }
            return vars[dclid].bb_id;
        }
        void setBBId(G4_Declare* dcl, unsigned int id)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            vars[dclid].bb_id = id;
        }

        bool isBlockLocal(G4_Declare* dcl)
        {
            return getBBId(dcl) < (UINT_MAX - 1);
        }

        G4_Declare* getSplittedDeclare(G4_Declare* dcl)
        {
            auto dclid = dcl->getDeclId();
            if (dclid >= vars.size())
            {
                return defaultValues.splittedDCL;
            }
            return vars[dclid].splittedDCL;
        }

        void setSplittedDeclare(G4_Declare* dcl, G4_Declare* sd)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            vars[dclid].splittedDCL = sd;
        }

        LocalLiveRange* getLocalLR(G4_Declare* dcl)
        {
            auto dclid = dcl->getDeclId();
            if (dclid >= vars.size())
            {
                return defaultValues.localLR;
            }
            return vars[dclid].localLR;
        }

        void setLocalLR(G4_Declare* dcl, LocalLiveRange* lr)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            MUST_BE_TRUE(vars[dclid].localLR == NULL, "Local live range already allocated for declaration");
            vars[dclid].localLR = lr;
        }

        void resetLocalLR(G4_Declare* dcl)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            vars[dclid].localLR = nullptr;
        }

        void recordRef(G4_Declare* dcl)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            vars[dclid].numRefs += 1;
        }

        unsigned int getNumRefs(G4_Declare* dcl)
        {
            auto dclid = dcl->getDeclId();
            if (dclid >= vars.size())
            {
                return defaultValues.numRefs;
            }
            return vars[dclid].numRefs;
        }

        void setNumRefs(G4_Declare* dcl, unsigned int refs)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            vars[dclid].numRefs = refs;
        }

        BankConflict getBankConflict(G4_Declare* dcl)
        {
            auto dclid = dcl->getDeclId();
            if (dclid >= vars.size())
            {
                return defaultValues.conflict;
            }
            return vars[dclid].conflict;
        }

        void setBankConflict(G4_Declare* dcl, BankConflict c)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            vars[dclid].conflict = c;
        }

        G4_INST* getStartInterval(G4_Declare* dcl)
        {
            auto dclid = dcl->getDeclId();
            if (dclid >= vars.size())
            {
                return defaultValues.startInterval;
            }
            return vars[dclid].startInterval;
        }

        void setStartInterval(G4_Declare* dcl, G4_INST* inst)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            vars[dclid].startInterval = inst;
        }

        G4_INST* getEndInterval(G4_Declare* dcl)
        {
            auto dclid = dcl->getDeclId();
            if (dclid >= vars.size())
            {
                return defaultValues.endInterval;
            }
            return vars[dclid].endInterval;
        }

        void setEndInterval(G4_Declare* dcl, G4_INST* inst)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            vars[dclid].endInterval = inst;
        }

        unsigned char* getMask(G4_Declare* dcl)
        {
            auto dclid = dcl->getDeclId();
            if (dclid >= vars.size())
            {
                return defaultValues.mask;
            }
            return vars[dclid].mask;
        }

        void setMask(G4_Declare* dcl, unsigned char* m)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            vars[dclid].mask = m;
        }

        AugmentationMasks getAugmentationMask(G4_Declare* dcl) const
        {
            auto dclid = dcl->getDeclId();
            if (dclid >= vars.size())
            {
                return defaultValues.maskType;
            }
            return vars[dclid].maskType;
        }

        void setAugmentationMask(G4_Declare* dcl, AugmentationMasks m)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            vars[dclid].maskType = m;
            if (dcl->getIsSplittedDcl())
            {
                auto dclSubDclSize = getSubDclSize(dcl);
                for (unsigned i = 0; i < dclSubDclSize; i++)
                {
                    G4_Declare * subDcl = getSubDcl(dcl, i);
                    setAugmentationMask(subDcl, m);
                }
            }
        }

        bool getHasNonDefaultMaskDef(G4_Declare* dcl) const
        {
            return (getAugmentationMask(dcl) == AugmentationMasks::NonDefault);
        }

        void addSubDcl(G4_Declare *dcl, G4_Declare* subDcl)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            vars[dclid].subDclList.push_back(subDcl);
        }

        void clearSubDcl(G4_Declare* dcl)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            vars[dclid].subDclList.clear();
        }

        G4_Declare* getSubDcl(G4_Declare* dcl, unsigned i)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            return vars[dclid].subDclList[i];
        }

        unsigned getSubDclSize(G4_Declare* dcl)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            return (unsigned)(vars[dclid].subDclList.size());
        }

        unsigned int getSubOffset(G4_Declare* dcl)
        {
            auto dclid = dcl->getDeclId();
            if (dclid >= vars.size())
            {
                return defaultValues.subOff;
            }
            return vars[dclid].subOff;
        }

        void setSubOffset(G4_Declare* dcl, unsigned int offset)
        {
            auto dclid = dcl->getDeclId();
            resize(dclid);
            vars[dclid].subOff = offset;
        }

        G4_Align getBankAlign(G4_Declare*);
        bool areAllDefsNoMask(G4_Declare*);
        void removeUnreferencedDcls();
        LocalLiveRange* GetOrCreateLocalLiveRange(G4_Declare* topdcl, Mem_Manager& mem);

        GlobalRA(G4_Kernel& k, PhyRegPool& r, PointsToAnalysis& p2a) : kernel(k), builder(*k.fg.builder), regPool(r),
            pointsToAnalysis(p2a)
        {
            vars.resize(k.Declares.size());
        }

        void emitFGWithLiveness(LivenessAnalysis& liveAnalysis);
        void reportSpillInfo(LivenessAnalysis& liveness, GraphColor& coloring);
        static uint32_t getRefCount(int loopNestLevel);
        bool isReRAPass();
        void updateSubRegAlignment(unsigned char regFile, G4_SubReg_Align subAlign);
        void updateAlignment(unsigned char regFile, G4_Align align);
        void getBankAlignment(LiveRange* lr, G4_Align &align);
        void printLiveIntervals();
        void reportUndefinedUses(LivenessAnalysis& liveAnalysis, G4_BB* bb, G4_INST* inst, G4_Declare* referencedDcl, std::set<G4_Declare*>& defs, std::ofstream& optreport, Gen4_Operand_Number opndNum);
        void detectNeverDefinedUses();
        void emitVarLiveIntervals();

        void determineSpillRegSize(unsigned& spillRegSize, unsigned& indrSpillRegSize);
        G4_Imm* createMsgDesc(unsigned owordSize, bool writeType, bool isSplitSend);
        void addrRegAlloc();
        void flagRegAlloc();
        bool hybridRA(bool doBankConflictReduction, bool highInternalConflict, DECLARE_LIST_ITER firstDclIter, LocalRA& lra);
        void assignRegForAliasDcl();
        void removeSplitDecl();
        int coloringRegAlloc();
        void addCallerSavePseudoCode();
        void addCalleeSavePseudoCode();
        void addStoreRestoreForFP();
        void setABIForStackCallFunctionCalls();
        void markGraphBlockLocalVars();
        void verifyRA(LivenessAnalysis & liveAnalysis);
    };

    class VarSplit
    {
    private:
        G4_Kernel& kernel;
        GlobalRA& gra;

        VarRange* splitVarRange(VarRange *src1, VarRange *src2, std::stack<VarRange*> *toDelete);
        void rangeListSpliting(VAR_RANGE_LIST *rangeList, G4_Operand *opnd, std::stack<VarRange*> *toDelete);
        static void getHeightWidth(G4_Type type, unsigned int numberElements, unsigned short &dclWidth, unsigned short &dclHeight, int &totalByteSize);
        void createSubDcls(G4_Kernel& kernel, G4_Declare* oldDcl, std::vector<G4_Declare*> &splitDclList);
        void insertMovesToTemp(IR_Builder& builder, G4_Declare* oldDcl, G4_Operand *dstOpnd, INST_LIST &instList, INST_LIST_ITER instIter, std::vector<G4_Declare*> &splitDclList);
        void insertMovesFromTemp(G4_Kernel& kernel, G4_Declare* oldDcl, int index, G4_Operand *srcOpnd, int pos, INST_LIST &instList, INST_LIST_ITER instIter, std::vector<G4_Declare*> &splitDclList);

    public:
        bool didLocalSplit = false;
        bool didGlobalSplit = false;

        void localSplit(IR_Builder& builder, G4_BB* bb);
        void globalSplit(IR_Builder& builder, G4_Kernel &kernel);
        bool canDoGlobalSplit(IR_Builder& builder, G4_Kernel &kernel, uint32_t instNum, uint32_t spillRefCount, uint32_t sendSpillRefCount);

        VarSplit(GlobalRA& g) : kernel(g.kernel), gra(g)
        {

        }
    };

    //
    // Spill code clean up
    //
    typedef struct _CLEAN_NUM_PROFILE
    {
        unsigned spill_clean_num[10];
        unsigned fill_clean_num[10];
    } CLEAN_NUM_PROFILE;

    typedef struct _SCRATCH_RANGE
    {
        unsigned int leftOff;
        unsigned int rightOff;
    }SCRATCH_RANGE;

    typedef std::vector<SCRATCH_RANGE >   SCRATCH_RANGE_VEC;
    typedef std::vector<SCRATCH_RANGE >::iterator   SCRATCH_RANGE_VEC_ITER;

    typedef struct _RANGE
    {
        unsigned int linearizedStart;
        unsigned int linearizedEnd;
        bool         predicate;
    }REG_RANGE;

    typedef std::vector<REG_RANGE >   REG_RANGE_VEC;
    typedef std::vector<REG_RANGE >::iterator   REG_RANGE_VEC_ITER;

    typedef std::pair<vISA::G4_INST *, int > RENAME_OPND;
    typedef std::vector<RENAME_OPND>  RANAME_VEC;

    typedef struct _SCRATCH_ACCESS
    {
        //Basic info
#ifdef _DEBUG
        int            regNum;
#endif
        vISA::G4_Declare*    dcl;
        vISA::G4_Operand*    tgtOpnd;
        INST_LIST_ITER inst_it;

        unsigned int   linearizedStart; //linearized start regsiter address
        unsigned int   linearizedEnd;   //linearized end regsiter address
        unsigned int   leftOff;         //left offset in scratch space
        unsigned int   rightOff;        //right offset in the scratch space
        unsigned int   useCount;

        bool     isSpill;
        bool     isBlockLocal;
        bool     directKill;

        bool     regKilled;
        bool     regPartialKilled;
        bool     regOverKilled;
        bool     inRangePartialKilled;
        bool     regInUse;

        bool     fillInUse;
        bool     removeable;
        bool     instKilled;
        bool     evicted;

        unsigned int   maskFlag;

        RANAME_VEC              renameOperandVec;
        SCRATCH_RANGE_VEC       killedScratchRange;
        REG_RANGE_VEC           killedRegRange;
        struct _SCRATCH_ACCESS* preScratchAccess;
        struct _SCRATCH_ACCESS* prePreScratchAccess;
        struct _SCRATCH_ACCESS* preFillAccess;

    } SCRATCH_ACCESS;

    typedef std::vector< SCRATCH_ACCESS *> SCRATCH_PTR_VEC;

    typedef vISA::std_arena_based_allocator<SCRATCH_ACCESS*> SCRATCH_PTR_ALLOCATOR;
    typedef std::list<SCRATCH_ACCESS*, SCRATCH_PTR_ALLOCATOR>           SCRATCH_PTR_LIST;
    typedef std::list<SCRATCH_ACCESS*, SCRATCH_PTR_ALLOCATOR>::iterator SCRATCH_PTR_LIST_ITER;

    class FlagSpillCleanup
    {
    private:
        GlobalRA& gra;

        void FlagLineraizedStartAndEnd(G4_Declare*  topdcl, unsigned int& linearizedStart, unsigned int& linearizedEnd);
        bool replaceWithPreDcl(IR_Builder& builder, SCRATCH_ACCESS* scratchAccess, SCRATCH_ACCESS* preScratchAccess,
            G4_RegFileKind  regKind);
        bool scratchKilledByPartial(SCRATCH_ACCESS* scratchAccess, SCRATCH_ACCESS* preScratchAccess);
        bool addKilledGRFRanges(unsigned int linearizedStart, unsigned int linearizedEnd, SCRATCH_ACCESS* scratchAccess,
            G4_RegFileKind  regKind, G4_Predicate*   predicate);
        bool regFullyKilled(SCRATCH_ACCESS* scratchAccess, unsigned int linearizedStart, unsigned int linearizedEnd,
            unsigned short maskFlag, G4_RegFileKind regKind, G4_Predicate* predicate);
        bool inRangePartialKilled(SCRATCH_ACCESS* scratchAccess, unsigned int linearizedStart, unsigned int linearizedEnd,
            unsigned short maskFlag);
        bool regDefineAnalysis(SCRATCH_ACCESS* scratchAccess, unsigned int linearizedStart, unsigned int linearizedEnd,
            unsigned short maskFlag, G4_RegFileKind regKind, G4_Predicate* predicate);
        void regDefineFlag(SCRATCH_PTR_LIST* scratchTraceList, G4_INST* inst, G4_Operand* opnd, G4_RegFileKind regKind);
        bool regUseAnalysis(SCRATCH_ACCESS* scratchAccess, unsigned int linearizedStart, unsigned int linearizedEnd);
        void regUseFlag(SCRATCH_PTR_LIST* scratchTraceList, G4_INST* inst, G4_Operand* opnd, int opndIndex, G4_RegFileKind regKind);
        void initializeScratchAccess(SCRATCH_ACCESS *scratchAccess, INST_LIST_ITER inst_it);
        bool initializeFlagScratchAccess(SCRATCH_PTR_VEC* scratchAccessList, SCRATCH_ACCESS*& scratchAccess, INST_LIST_ITER inst_it);
        void freeScratchAccess(SCRATCH_PTR_VEC *scratchAccessList);
        void flagDefine(SCRATCH_PTR_LIST& scratchTraceList, G4_INST* inst);
        void flagUse(SCRATCH_PTR_LIST& scratchTraceList, G4_INST* inst);
        bool flagScratchDefineUse(G4_BB* bb, SCRATCH_PTR_LIST* scratchTraceList, SCRATCH_PTR_VEC* candidateList,
            SCRATCH_ACCESS* scratchAccess, CLEAN_NUM_PROFILE* clean_num_profile);
        void flagSpillFillClean(G4_BB* bb, INST_LIST_ITER inst_it, SCRATCH_PTR_VEC& scratchAccessList,
            SCRATCH_PTR_LIST& scratchTraceList, SCRATCH_PTR_VEC& candidateList, CLEAN_NUM_PROFILE* clean_num_profile);
        void regFillClean(IR_Builder& builder, G4_BB* bb, SCRATCH_PTR_VEC& candidateList, G4_RegFileKind regKind,
            CLEAN_NUM_PROFILE* clean_num_profile);
        void regSpillClean(IR_Builder& builder, G4_BB* bb, SCRATCH_PTR_VEC& candidateList, CLEAN_NUM_PROFILE* clean_num_profile);

    public:
        void spillFillCodeCleanFlag(IR_Builder& builder, G4_Kernel& kernel, G4_RegFileKind regKind, CLEAN_NUM_PROFILE* clean_num_profile);
        FlagSpillCleanup(GlobalRA& g) : gra(g)
        {

        }

    };
}

#endif // __GRAPHCOLOR_H__
