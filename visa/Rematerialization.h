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

#ifndef __REMAT_H__
#define __REMAT_H__

#include "FlowGraph.h"
#include "GraphColor.h"
#include "RPE.h"
#include <list>
#include <map>

namespace vISA
{
// Attempt remat only if reg pressure in loop is higher than this
#define REMAT_LOOP_REG_PRESSURE 85

// Remat will be attempted only when reg pressure is higher than this macro
#define REMAT_REG_PRESSURE 120

// Remat will trigger only for vars that have less than following uses
#define MAX_USES_REMAT 6

// Minimum def-use distance for remat to trigger
#define MIN_DEF_USE_DISTANCE 20

// Distance in instructions to reuse rematted value in BB
#define MAX_LOCAL_REMAT_REUSE_DISTANCE 40

    typedef std::pair<G4_INST*, G4_BB*> Reference;
    class References
    {
    public:
        std::vector<Reference> def;
        //std::vector<Reference> uses;

        unsigned int numUses = 0;

        // lastUseLexId provides a quick and dirty way
        // to determine end of liveness for a variable.
        // This is not always accurate due to holes
        // in live-ranges but should be fine most times.
        unsigned int lastUseLexId = 0;

        // Store number of times this var has been used in
        // a remat'd operation. This forms part of heuristic
        // that decides if it is profitable to remat an
        // operation inside a loop.
        unsigned int numRemats = 0;

        // Store set of rows of this variable ever used.
        // This is useful for samplers.
        std::unordered_set<unsigned int> rowsUsed;
    };

    class Dominators
    {
    private:
        std::map<G4_BB*, std::set<G4_BB*>> dom;

        FlowGraph& fg;

    public:
        Dominators(FlowGraph& f) : fg(f)
        {
            computeDominators();
        }

        // Store dominator information in data structure above for easy querying.
        // Flow graph stores immediate doms of each BB.s
        void computeDominators();
        bool dominates(G4_BB*, G4_BB*);
        void dump();
    };

    class Rematerialization
    {
    private:
        G4_Kernel& kernel;
        LivenessAnalysis& liveness;
        GraphColor& coloring;
        Dominators doms;
        G4_Declare* samplerHeader = nullptr;
        unsigned int numRematsInLoop = 0;
        bool IRChanged = false;
        bool samplerHeaderMapPopulated = false;
        unsigned int loopInstsBeforeRemat = 0;
        unsigned int totalInstsBeforeRemat = 0;
        RPE& rpe;

        std::vector<G4_Declare*> spills;
        // For each top dcl, this map holds all defs
        std::unordered_map<G4_Declare*, References> operations;
        // This vector contains declares that could potentially save spill
        // if remat'd.
        std::vector<bool> rematCandidates;

        // Map each sampler instruction with instruction initializing
        // samplerHeader instruction. This is required when inserting
        // remat'd samplers.
        std::unordered_map<G4_INST*, G4_INST*> samplerHeaderMap;
        std::unordered_set<G4_BB*> deLVNedBBs;

        void populateRefs();
        void populateSamplerHeaderMap();
        void deLVNSamplers(G4_BB*);
        bool canRematerialize(G4_SrcRegRegion*, G4_BB*, const Reference*&, INST_LIST_ITER instIter);
        G4_SrcRegRegion* rematerialize(G4_SrcRegRegion*, G4_BB*, const Reference*, std::list<G4_INST*>&, G4_INST*&);
        G4_SrcRegRegion* createSrcRgn(G4_SrcRegRegion*, G4_DstRegRegion*, G4_Declare*);
        const Reference* findUniqueDef(References&, G4_SrcRegRegion*);
        bool areInSameLoop(G4_BB*, G4_BB*, bool&);
        bool isRangeSpilled(G4_Declare*);
        bool areAllDefsInBB(G4_Declare*, G4_BB*, unsigned int);
        unsigned int getLastUseLexId(G4_Declare*);
        bool checkLocalWAR(G4_INST*, G4_BB*, INST_LIST_ITER);

        void reduceNumUses(G4_Declare* dcl)
        {
            auto opIt = operations.find(dcl);
            if (opIt != operations.end())
            {
                auto numUses = (*opIt).second.numUses;
                if(numUses > 0)
                    (*opIt).second.numUses = numUses - 1;
                
                if(numUses == 1)
                {
                    for (auto ref : (*opIt).second.def)
                    {
                        ref.second->instList.remove(ref.first);
                    }
                    (*opIt).second.def.clear();
                }
            }

            
        }

        unsigned int getNumUses(G4_Declare* dcl)
        {
            auto opIt = operations.find(dcl);
            if (opIt != operations.end())
                return (*opIt).second.numUses;

            return 0;
        }

        void incNumRemat(G4_Declare* dcl)
        {
            auto opIt = operations.find(dcl);
            if (opIt != operations.end())
                (*opIt).second.numRemats += 1;
        }

        unsigned int getNumRemats(G4_Declare* dcl)
        {
            auto opIt = operations.find(dcl);
            if (opIt != operations.end())
                return (*opIt).second.numRemats;

            return 0;
        }

        bool isRematCandidateOp(G4_INST* inst)
        {
            if (inst->isFlowControl() || inst->isWait() || inst->isFence() ||
                inst->isLifeTimeEnd() || inst->isAccDstInst() || inst->isAccSrcInst() ||
                inst->getImplAccDst() || inst->getImplAccSrc())
            {
                return false;
            }

            return true;
        }

        void cleanRedundantSamplerHeaders();

        unsigned int getNumRematsInLoop() { return numRematsInLoop; }
        void incNumRematsInLoop() { numRematsInLoop++; }

    public:
        Rematerialization(G4_Kernel& k, LivenessAnalysis& l, GraphColor& c, RPE& r) :
            kernel(k), liveness(l), coloring(c), doms(k.fg), rpe(r)
        {
            rematCandidates.resize(l.getNumSelectedVar(), false);

            for (auto&& lr : coloring.getSpilledLiveRanges())
            {
                auto dcl = lr->getVar()->getDeclare()->getRootDeclare();
                if (!dcl->isSpilled())
                {
                    spills.push_back(dcl);
                    dcl->setSpillFlag();
                }

                // Mark all simultaneously live variables as remat candidates
                unsigned int spillId = dcl->getRegVar()->getId();
                auto& intfVec = coloring.getIntf()->getSparseIntfForVar(spillId);

                for (auto intfId : intfVec)
                {
                    rematCandidates[intfId] = true;
                }
            }

            std::set<G4_BB*> bbsInLoop;
            for (auto&& be : kernel.fg.backEdges)
            {
                auto loopIt = kernel.fg.naturalLoops.find(be);

                if (loopIt != kernel.fg.naturalLoops.end())
                {
                    bbsInLoop.insert((*loopIt).second.begin(), (*loopIt).second.end());
                }
            }

            for (auto& bb : kernel.fg.BBs)
            {
                bool bbInLoop = (bbsInLoop.find(bb) != bbsInLoop.end());
                if (bbInLoop)
                {
                    for (auto& inst : bb->instList)
                    {
                        if (!inst->isLabel() && !inst->isPseudoKill())
                        {
                            loopInstsBeforeRemat++;
                        }
                    }
                }
            }
        }

        ~Rematerialization()
        {
            for (auto&& dcl : spills)
            {
                dcl->resetSpillFlag();
            }
        }

        bool getChangesMade() { return IRChanged; }

        void run();
    };
}
#endif
