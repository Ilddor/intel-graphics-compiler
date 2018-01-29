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

#include "FlowGraph.h"
#include <string>
#include <set>
#include <iostream>
#include <sstream>
#include <fstream>
#include <functional>
#include <algorithm>
#include "BuildIR.h"
#include "Option.h"
#include "stdlib.h"
#include "queue"
#include "BitSet.h"
#include "PhyRegUsage.h"
#include "visa_wa.h"
#include "CFGStructurizer.h"
#include "DebugInfo.h"
#include <random>
#include <chrono>

#include "iga/IGALibrary/api/iga.h"
#include "iga/IGALibrary/api/iga.hpp"

#if defined(WIN32)
#define CDECLATTRIBUTE __cdecl
#elif __GNUC__
#ifdef __x86_64__
#define CDECLATTRIBUTE
#else
#define CDECLATTRIBUTE                 __attribute__((__cdecl__))
#endif
#endif


using namespace std;
using namespace vISA;

static _THREAD char* prevFilename;
static _THREAD int prevSrcLineNo;

bool G4_BB::isSuccBB(G4_BB* succ)
{
    for (std::list<G4_BB*>::iterator it = Succs.begin(); it != Succs.end(); ++it)
    {
        if ((*it) == succ)  return true;
    }
    return false;
}

void G4_BB::removePredEdge(G4_BB* pred)
{
    for (std::list<G4_BB*>::iterator it = Preds.begin(); it != Preds.end(); ++it)
    {
        if (*it != pred) continue;
        // found
        Preds.erase(it);
        return;
    }
    MUST_BE_TRUE(false, ERROR_FLOWGRAPH); // edge is not found
}

void G4_BB::removeSuccEdge(G4_BB* succ)
{
    for (std::list<G4_BB*>::iterator it = Succs.begin(); it != Succs.end(); ++it)
    {
        if (*it != succ) continue;
        // found
        Succs.erase(it);
        return;
    }
    MUST_BE_TRUE(false, ERROR_FLOWGRAPH); // edge is not found
}

//
// find the fall-through BB of the current block.
// if the last inst is a unconditional jump, then the target is not considered a fall-through BB
// NOTE: Pay attention this function is only works after the handleReturn() duo the the conditional CALL
//
G4_BB* G4_BB::fallThroughBB()
{
    G4_INST* last = (!instList.empty()) ? instList.back() : NULL;

    if (last != NULL)
    {
        if (last->opcode() == G4_goto || last->opcode() == G4_join)
        {
            return NULL;
        }
        if (last->isFlowControl())
        {
            // if No successor, return NULL;
            if (Succs.empty())
            {
                return NULL;
            }

            //
            // Instructions    Predicate-On    Predicate-Off    Num of Succ
            // Jmpi                Front                None               >=1
            // CALL             Front                None               >=2     considered the conditional call here
            // while               Front                Front              2 // G4_while considered to fall trhu even without pred, since break jumps to while
            // if, else        Front                Front               2
            // break, cont      Front                None               1,2
            // return              Front                None               >=1
            // do                   Front                Front                1
            // endif                 Front                Front                1
            if (last->isCall())
            {
                return BBAfterCall();
            }
            else if (last->getPredicate() == NULL &&
                // G4_while considered to fall trhu even without pred, since break jumps to while
                (last->opcode() == G4_jmpi || last->opcode() == G4_break || last->opcode() == G4_cont || last->isReturn()))
            {
                return NULL;
            }
            else
            {
                return Succs.front();
            }
        }
    }

    //
    // process other cases
    //
    if (Succs.size() == 0) // exit BB
        return NULL; // no fall-through BB
    else
        return Succs.front();
}

//
// to check if the last instruction in list is EOT
//
bool G4_BB::isLastInstEOT()
{
    if (instList.size() == 0)
    {
        return false;
    }

    G4_INST *i = instList.back();

    if (parent->builder->hasSendShootdown())
    {
        // due to send shootdown, a predicated send may not actually be an EOT
        return i->isEOT() && i->getPredicate() == NULL;
    }
    else
    {
        return i->isEOT();
    }
}

G4_opcode G4_BB::getLastOpcode() const
{
    G4_INST *i = instList.empty() ? nullptr : instList.back();
    if (i)
    {
        return i->opcode();
    }
    else
    {
        return G4_illegal;
    }
}

//
// return label's corresponding BB
// if label's BB is not yet created, then create one and add map label to BB
//
G4_BB* FlowGraph::getLabelBB(Label_BB_Map& map, char const* label)
{
    MUST_BE_TRUE(label != NULL, ERROR_INTERNAL_ARGUMENT);
    std::string label_string = label;
    if (map.find(label_string) != map.end())
        return map[label_string];
    else    // BB does not exist
    {
        G4_BB* bb = createNewBB();
        map[label_string] = bb;             // associate them
        return bb;
    }
}

//
// first is the first inst of a BB
//
G4_BB* FlowGraph::beginBB(Label_BB_Map& map, G4_INST* first)
{
    if (first == NULL) return NULL;
    G4_BB* bb = (first->isLabel()) ? getLabelBB(map, first->getLabelStr()) : createNewBB();
    BBs.push_back(bb); // append to BBs list
    return bb;
}

void FlowGraph::matchLoop(INST_LIST& instlist)
{
    int numLoopNest = 0;
    // global stack the do instructions (converted to a label ) as well as
    // break/cont that do not yet have a matching while
    std::stack<G4_INST*> loopInsts;
    // queue for break/continue for innermost loop that do not have their
    // JIPs assigned yet
    std::queue<G4_INST*> innerBreakCont;
    char labelName[64];

    for (INST_LIST_ITER it = instlist.begin(); it != instlist.end(); ++it)
    {
        G4_INST* inst = *it;
        switch (inst->opcode())
        {
        case G4_do:
        {
            while (!innerBreakCont.empty())
            {
                G4_INST* breakContInst = innerBreakCont.front();
                innerBreakCont.pop();
                loopInsts.push(breakContInst);
            }
            numLoopNest++;
            // replace do with a new loop label
            SNPRINTF(labelName, 64, "%s_%d", "_LOOP_START", loopLabelId++);
            G4_Label* loopLabel = builder->createLabel(labelName, LABEL_BLOCK);
            loopLabel->setStartLoopLabel();
            inst->setOpcode(G4_label);
            inst->setSrc(loopLabel, 0);
            loopInsts.push(inst);
            break;
        }
        case G4_break:
        case G4_cont:
            innerBreakCont.push(inst);
            break;
        case G4_while:
        {
            // add the unprocessed break/cont to the global stack
            while (!innerBreakCont.empty())
            {
                G4_INST* breakContInst = innerBreakCont.front();
                innerBreakCont.pop();
                loopInsts.push(breakContInst);
            }
            bool foundLoopStart = false;
            while (!loopInsts.empty())
            {
                G4_INST* loopInst = loopInsts.top();
                loopInsts.pop();
                if (loopInst->isLabel())
                {
                    // find loop start, assign while label to it
                    inst->asCFInst()->setJip(loopInst->getSrc(0));
                    foundLoopStart = true;
                    break;
                }
                else if (loopInst->opcode() == G4_break)
                {

                    G4_Label* breakLabel = NULL;
                    INST_LIST_ITER labelIter = it;
                    //break's UIP should be the while instruction itself
                    --labelIter;

                    if ((*labelIter)->isLabel())
                    {
                        // a label is already there after the while, use it
                        breakLabel = (G4_Label*)(*labelIter)->getSrc(0);
                    }
                    else
                    {
                        // insert label after while
                        SNPRINTF(labelName, 64, "%s_%d", "_LOOP_BREAK", loopLabelId++);

                        breakLabel = builder->createLabel(labelName, LABEL_BLOCK);
                        G4_INST* labelInst = createNewLabelInst(breakLabel, inst->getLineNo(), inst->getCISAOff());
                        instlist.insert(std::next(it), labelInst);
                    }
                    loopInst->asCFInst()->setUip(breakLabel);
                    if (loopInst->asCFInst()->getJip() == NULL)
                    {
                        loopInst->asCFInst()->setJip(breakLabel);
                    }
                }
                else
                {
                    MUST_BE_TRUE(loopInst->opcode() == G4_cont, "expect cont instruction");

                    // cont's UIP should be the while instruction
                    G4_Label* contLabel = NULL;
                    INST_LIST_ITER prev = it;
                    --prev;
                    if ((*prev)->isLabel())
                    {
                        // a label is already there before the while, use it
                        contLabel = (G4_Label*)(*prev)->getSrc(0);
                    }
                    else
                    {
                        // insert label before while
                        SNPRINTF(labelName, 64, "%s_%d", "_LOOP_CONT_", loopLabelId++);

                        contLabel = builder->createLabel(labelName, LABEL_BLOCK);
                        G4_INST* labelInst = createNewLabelInst(contLabel, inst->getLineNo(), inst->getCISAOff());
                        instlist.insert(it, labelInst);
                    }
                    loopInst->asCFInst()->setUip(contLabel);
                    if (loopInst->asCFInst()->getJip() == NULL)
                    {
                        loopInst->asCFInst()->setJip(contLabel);
                    }
                }
            }
            ASSERT_USER(foundLoopStart, "while without matching do");
            break;
        }
        case G4_if:
        {
            // add the unprocessed break/cont to the global stack
            while (!innerBreakCont.empty())
            {
                G4_INST* breakContInst = innerBreakCont.front();
                innerBreakCont.pop();
                loopInsts.push(breakContInst);
            }
            break;
        }
        case G4_endif:
        {
            INST_LIST_ITER prev = it;
            --prev;
            //matchBranch should've inserted a label before the endif
            MUST_BE_TRUE((*prev)->isLabel(), "Expect label before endif");
            G4_Label* endifLabel = (G4_Label*)(*prev)->getSrc(0);
            while (!innerBreakCont.empty())
            {
                // break/cont's JIP should point to this endif
                G4_INST* breakContInst = innerBreakCont.front();
                innerBreakCont.pop();
                breakContInst->asCFInst()->setJip(endifLabel);
                loopInsts.push(breakContInst);
            }
            break;
        }
        case G4_else:
        {
            G4_Label* elseLabel = NULL;
            if (!innerBreakCont.empty())
            {
                INST_LIST_ITER prev = it;
                --prev;
                if ((*prev)->isLabel())
                {
                    elseLabel = (G4_Label*)(*prev)->getSrc(0);
                }
                else
                {
                    // insert label before else
                    SNPRINTF(labelName, 64, "%s_%d", "_LOOP_ELSE_", loopLabelId++);
                    elseLabel = builder->createLabel(labelName, LABEL_BLOCK);
                    G4_INST* labelInst = createNewLabelInst(elseLabel, inst->getLineNo(), inst->getCISAOff());
                    instlist.insert(it, labelInst);
                }
            }

            while (!innerBreakCont.empty())
            {
                // break/cont's JIP should point to this else

                G4_INST* breakContInst = innerBreakCont.front();
                innerBreakCont.pop();
                breakContInst->asCFInst()->setJip(elseLabel);
                loopInsts.push(breakContInst);
            }
            break;
        }
        default:
            break; // Prevent gcc warning
        }
    }

    ASSERT_USER(loopInsts.empty() && innerBreakCont.empty(),
        "unmatched do/break/cont instruction");

}

G4_INST* FlowGraph::createNewLabelInst(G4_Label* label, int lineNo = 0, int CISAOff = 0)
{
    //srcFileName is NULL
    return builder->createInternalInst(NULL, G4_label,
        NULL, false, 1, NULL, label, NULL, 0, lineNo, CISAOff, NULL);
}

G4_BB* FlowGraph::createNewBB(bool insertInFG)
{
    G4_BB* bb = new (mem)G4_BB(instListAlloc, numBBId, this);

    // Increment counter only when new BB is inserted in FlowGraph
    if (insertInFG)
        numBBId++;

    if (builder->getOptions()->getTarget() == VISA_3D)
    {
        // all 3D bbs are considered to be in SIMD CF since the dispatch mask may
        // have some channels off.
        bb->setInSimdFlow(true);
    }

    BBAllocList.push_back(bb);
    return bb;
}

static int globalCount = 1;
int64_t FlowGraph::insertDummyUUIDMov()
{
    // Here when -addKernelId is passed
    if (builder->getIsKernel())
    {
        // Add mov inst as first instruction in kernel. This
        // has to be *first* instruction in the kernel so debugger
        // can detect the pattern.
        //
        // 32-bit random number is generated and set as src0 operand.
        // Earlier nop was being generated with 64-bit UUID overloading
        // MBZ bits and this turned out to be a problem for the
        // debugger (random clobbering of r0).

        for (auto bb : BBs)
        {
            uint32_t seed = (uint32_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
            std::mt19937 mt_rand(seed * globalCount);
            globalCount++;

            G4_DstRegRegion* nullDst = builder->createNullDst(Type_UD);
            int64_t uuID = (int64_t)mt_rand();
            G4_Operand* randImm = (G4_Operand*)builder->createImm(uuID, Type_UD);
            G4_INST* movInst = builder->createInternalInst(nullptr, G4_mov, nullptr, false, 1,
                nullDst, randImm, nullptr, 0);

            auto instItEnd = bb->instList.end();
            for (auto it = bb->instList.begin();
                it != instItEnd;
                it++)
            {
                if ((*it)->isLabel())
                {
                    bb->instList.insert(++it, movInst);
                    return uuID;
                }

                bb->instList.push_front(movInst);
                return uuID;
            }
        }
    }
    return 0;
}

//
// (1) check if-else-endif and iff-endif pairs
// (2) add label for those omitted ones
//
bool FlowGraph::matchBranch(int &sn, INST_LIST& instlist, INST_LIST_ITER &it)
{
    G4_INST* inst = *it;
    G4_INST* prev = NULL;
    //
    // process if-endif or if-else-endif
    //
    if (inst->opcode() == G4_if)
    {
        // check label / add label
        G4_Label *if_label = NULL;
        G4_Label *else_label = NULL;
        G4_Label *endif_label = NULL;   // the label immediately before the endif
        G4_INST* ifInst = inst;
        assert(inst->asCFInst()->getJip() == nullptr && "IF should not have a label at this point");

        // create if_label
        char createdLabel[64];
        if (builder->getIsKernel())
            SNPRINTF(createdLabel, 64, "k%d_%s_%d", builder->getCUnitId(), "_AUTO_GENERATED_IF_LABEL_", sn);
        else
            SNPRINTF(createdLabel, 64, "f%d_%s_%d", builder->getCUnitId(), "_AUTO_GENERATED_IF_LABEL_", sn);
        sn++;
        if_label = builder->createLabel(createdLabel, LABEL_BLOCK);
        inst->asCFInst()->setJip(if_label);

        // look for else/endif
        int elseCount = 0;
        it++;
        while (it != instlist.end())
        {
            inst = *it;
            // meet iff or if
            if (inst->opcode() == G4_if)
            {
                if (!matchBranch(sn, instlist, it))
                    return false;
            }
            else if (inst->opcode() == G4_else)
            {
                if (elseCount != 0)
                {
                    MUST_BE_TRUE(false, "ERROR: Mismatched if-else: more than one else following if!");
                    return false;
                }
                elseCount++;
                INST_LIST_ITER it1 = it;
                it1++;

                // add endif label to "else"
                char createdLabel[64];
                if (builder->getIsKernel())
                    SNPRINTF(createdLabel, 64, "k%d_%s_%d", builder->getCUnitId(), "_AUTO_GENERATED_ELSE_LABEL_", sn);
                else
                    SNPRINTF(createdLabel, 64, "f%d_%s_%d", builder->getCUnitId(), "_AUTO_GENERATED_ELSE_LABEL_", sn);

                sn++;
                else_label = builder->createLabel(createdLabel, LABEL_BLOCK);
                inst->asCFInst()->setJip(else_label);

                // insert if-else label
                G4_INST* label = createNewLabelInst(if_label, inst->getLineNo(), inst->getCISAOff());
                instlist.insert(it1, label);

                // Uip must be the same as Jip for else instructions.
                inst->asCFInst()->setUip(else_label);
            }
            else if (inst->opcode() == G4_endif)
            {
                // For GT, if/else/endif are different from Gen4:
                // (1). if - endif
                //          if endif_label
                //          ...
                //          endif_label:
                //          endif
                // (2). if - else - endif
                //          if else_label
                //          ...
                //          else endif_label
                //          else_label:
                //          ...
                //          endif_label:     // this is different from Gen4
                //          endif
                //

                if (elseCount == 0)                 // if-endif case
                {
                    // insert endif label
                    G4_INST* label = createNewLabelInst(if_label, inst->getLineNo(), inst->getCISAOff());
                    instlist.insert(it, label);
                    endif_label = if_label;
                }
                else                                    // if-else-endif case
                {
                    // insert endif label
                    G4_INST* label = createNewLabelInst(else_label, inst->getLineNo(), inst->getCISAOff());
                    instlist.insert(it, label);
                    endif_label = else_label;
                }

                //we must also set the UIP of if to point to its corresponding endif
                ifInst->asCFInst()->setUip(endif_label);
                return true;
            }   // if opcode
            if (it == instlist.end())
            {
                MUST_BE_TRUE(false, "ERROR: Can not find endif for if!");
                return false;
            }
            prev = inst;
            it++;
        }   // while
    }
    //
    // process iff-endif
    //
    else
        MUST_BE_TRUE(false, ERROR_FLOWGRAPH);

    return false;
}

//
//  HW Rules about the jip and uip for the control flow instructions
//  if:
//      <branch_ctrl> set
//          jip = first join in the if block
//          uip = endif
//      <branch_ctrl> not set
//          jip = instruction right after the else, or the endif if else doesn't exist
//          uip = endif
//  else:
//      <branch_ctrl> set
//          jip = first join in the else block
//          uip = endif
//      <branch_ctrl> not set
//          jip = uip = endif
//  endif:
//          jip = uip = next enclosing endif/while
//  while:
//          jip = loop head label
//          uip = don't care
//  break:
//          jip = end of innermost CF block (else/endif/while)
//          uip = while
//  cont:
//          jip = end of innermost CF block (else/endif/while)
//          uip = while
//

//
// Preprocess the instruction and kernel list, including:
// 1. Check if the labels and kernel names are unique
// 2. Check if all the labels used by jmp, CALL, cont, break, goto is defined, determine if goto is forward or backward
// 3. Process the non-label "if-else-endif" cases
//
void FlowGraph::preprocess(INST_LIST& instlist)
{
    std::map<std::string, G4_INST*> kernel_map;  // map label to its corresponding instruction

    //
    // First pass: (1) Set up the label map; (2) check the labels;
    //
    std::map<std::string, G4_INST*> label_map;  // map label to its corresponding instruction
    INST_LIST_ITER it1 = instlist.begin();
    while (it1 != instlist.end() && (*it1)->isLabel())                                   // remove the repeated labels at the beginning
    {
        std::string label_string = (*it1)->getLabelStr();
        if (label_map.find(label_string) != label_map.end())
        {
            it1 = instlist.erase(it1);
        }
        else
        {
            label_map[label_string] = *it1;
            it1++;
        }
    }

    while (it1 != instlist.end())
    {
        G4_INST* i = *it1;
        if (i->isDead())
        {
            INST_LIST_ITER curr_iter = it1;
            ++it1;
            instlist.erase(curr_iter);
            continue;
        }
        if (i->isLabel())
        {
            std::string label_string = i->getLabelStr();
            if (label_map.find(label_string) != label_map.end())
            {
                MUST_BE_TRUE(false, "ERROR: Redefined label \"" << label_string << "\"");
            }
            else
                label_map[label_string] = i;
        }
        it1++;
    }
    //
    // Second pass: Check the label used by jmp, call, cont, break, etc.
    //
    uint16_t numGoto = 0;
    INST_LIST_ITER II = instlist.begin();
    while (II != instlist.end())
    {
        G4_INST* i = *II;
        INST_LIST_ITER currIter = II;
        ++II;

        std::string label_string;
        if (i->opcode() == G4_goto)
        {
            label_string = ((G4_Label *)i->asCFInst()->getUip())->getLabel();
            std::map<std::string, G4_INST*>::iterator labelIter = label_map.find(label_string);
            MUST_BE_TRUE2(labelIter != label_map.end(), "ERROR: Undefined label \"" << label_string << "\"", i);
            // check if it is forward or backward goto
            if (i->getId() > (*labelIter).second->getId())
            {
                i->asCFInst()->setBackward(true);
            }

            INST_LIST_ITER tmpIter = currIter;
            ++tmpIter;

            if (tmpIter != instlist.end())
            {
                G4_INST *nextInst = *tmpIter;
                // sanity check, make sure goto is indeed needed.
                if (nextInst->isLabel() && label_string == nextInst->getLabelStr())
                {
                    instlist.erase(currIter);
                    continue;
                }
            }

            // ??? Why is this necessary ???
            // add label instruction after goto
            bool  insertLabel = false;
            if (tmpIter != instlist.end())
            {
                INST_LIST_ITER nextIter = tmpIter;
                nextIter++;
                if (!(*tmpIter)->isLabel() ||
                    (i->asCFInst()->isBackward() && nextIter != instlist.end() && G4_Inst_Table[(*nextIter)->opcode()].instType == InstTypeFlow))
                {
                    insertLabel = true;
                }
            }
            else
            {
                insertLabel = true;
                // create a dummy inst
                //srcFileName is NULL
                G4_INST* nopInst = builder->createInternalInst(NULL, G4_nop, NULL, false, 1, NULL, NULL, NULL, 0,
                    i->getLineNo(), i->getCISAOff(), NULL);
                instlist.insert(tmpIter, nopInst);
                tmpIter--;
            }

            if (insertLabel)
            {
                char name[20];
                SNPRINTF(name, 20, "_BW_GOTO_JIP_%d", numGoto++);
                G4_Label* lbl = builder->createLabel(name, LABEL_BLOCK);
                G4_INST* lInst = createNewLabelInst(lbl, i->getLineNo(), i->getCISAOff());
                instlist.insert(tmpIter, lInst);
                tmpIter--;
            }
        }
        else if ((i->opcode() == G4_jmpi || i->isCall()) &&
            i->getSrc(0) &&
            i->getSrc(0)->isLabel())
        {
            label_string = ((G4_Label *)i->getSrc(0))->getLabel();
            std::map<std::string, G4_INST*>::iterator labelIter = label_map.find(label_string);
            MUST_BE_TRUE2(labelIter != label_map.end(), "ERROR: Undefined label \"" << label_string << "\"", i);
            if (i->getId() > (*labelIter).second->getId())
            {
                i->asCFInst()->setBackward(true);
            }
        } // fi
    } // for

    //
    // 3. Process the non-label "if-else-endif" cases as following
    //
    {
        int sn = 0;
        for (INST_LIST_ITER it = instlist.begin(); it != instlist.end(); ++it)
        {
            G4_INST *inst = *it;
            if (inst->opcode() == G4_if)
            {
                if (!matchBranch(sn, instlist, it))
                {
                    MUST_BE_TRUE2(false, "ERROR: mismatched if-branch", inst);
                    break;
                }
            }   // fi
        }   // for
    }

    //
    // 4. Match up the while/break/cont instructions and generate labels for them
    //
    matchLoop(instlist);
}

void FlowGraph::NormalizeFlowGraph()
{
    // For CISA 3 flowgraph there will be pseudo_fcall instructions to invoke stack functions.
    // Save/restore code will be added around this at the time of register allocation.
    // If fcall's successor has more than 1 predecessor then it will create problems inserting
    // code. This function handles such patterns by creating new basic block and guaranteeing
    // that fcall's successor has a single predecessor.
    for (BB_LIST_ITER it = BBs.begin(); it != BBs.end(); it++)
    {
        G4_BB* bb = *it;
        if (bb->isEndWithFCall())
        {
            G4_BB* retBB = bb->Succs.front();
            G4_INST *lInst = retBB->instList.front();
            if (retBB->Preds.size() > 1)
            {

                // To insert restore code we need to guarantee that save code has
                // been executed. If retBB has multiple preds, this may not be
                // guaranteed, so insert a new node.
                G4_BB* newNode = createNewBB();

                // Remove old edge
                removePredSuccEdges(bb, retBB);

                // Add new edges
                addPredSuccEdges(bb, newNode);
                addPredSuccEdges(newNode, retBB);

                // Create and insert label inst
                char name[32];
                if (builder->getIsKernel() == true)
                {
                    // kernel
                    SNPRINTF(name, 32, "L_AUTO_k%d_%d", builder->getCUnitId(), newNode->getId());
                }
                else
                {
                    // function
                    SNPRINTF(name, 32, "L_AUTO_f%d_%d", builder->getCUnitId(), newNode->getId());
                }

                G4_Label* lbl = builder->createLabel(name, LABEL_BLOCK);
                //srcFileName is NULL
                G4_INST* inst = createNewLabelInst(lbl, lInst->getLineNo(), lInst->getCISAOff());
                newNode->instList.push_back(inst);
                BBs.insert(++it, newNode);
                it--;

                retBB = newNode;
            }
        }
    }
}

//
// build a control flow graph from the inst list
// we want to keep the original BB order (same as shown in assembly code) for linear scan
// register allocation. We use beginBB to indicate that we encounter the beginning
// a BB and add the BB to the BB list and use getLabelBB to create a block for a label but
// not to add to the BB list.For instance, when we come across a "jmp target", getLabelBB
// is called to create a BB for target but the target BB is added into the list later (
// assume forward jmp) as the target label is visited.
//
//
void FlowGraph::constructFlowGraph(INST_LIST& instlist)
{
    MUST_BE_TRUE(!instlist.empty(), ERROR_SYNTAX("empty instruction list"));

    //
    // The funcInfoHashTable maintains a map between the id of the function's INIT block
    // to its FuncInfo definition.
    //
    FuncInfoHashTable funcInfoHashTable;

    preprocess(instlist);

    //
    // map label to its corresponding BB
    //
    std::map<std::string, G4_BB*> labelMap;
    //
    // create the entry block of the flow graph
    //
    G4_BB* fstartBB = NULL;
    G4_BB* curr_BB = entryBB = fstartBB = beginBB(labelMap, instlist.front());

    kernelInfo = new (mem)FuncInfo(UINT_MAX, entryBB, NULL);

    bool hasSIMDCF = false, hasNoUniformGoto = false;
    while (!instlist.empty())
    {
        INST_LIST_ITER iter = instlist.begin();
        G4_INST* i = *iter;

        MUST_BE_TRUE(curr_BB != NULL, "Current BB must not be empty");
        //
        // inst i belongs to the current BB
        // remove inst i from instlist and relink it to curr_BB's instList
        //
        curr_BB->instList.splice(curr_BB->instList.end(), instlist, iter);
        G4_INST* next_i = (instlist.empty()) ? NULL : *instlist.begin();

        // If this block is a start of the function
        if (i->isLabel() && ((G4_Label*)i->getSrc(0))->isFuncLabel() && fstartBB->getId() != curr_BB->getId())
        {
            fstartBB = curr_BB;
        }
        else if (fstartBB->getId() != curr_BB->getId() && !fstartBB->existsInBBList(curr_BB->getId()))
        {
            fstartBB->addToBBList(curr_BB->getId(), curr_BB);
            curr_BB->setStartBlock(fstartBB);
        }

        if (i->isSend())
        {
            curr_BB->setSendInBB(true);
        }

        if (i->isLabel())
        {
            G4_Label* label = i->getLabel();
            if (label->isFuncLabel())
            {
                // exit BB is not known yet
                FuncInfo *subroutineInfo = new (mem)FuncInfo(
                    (unsigned int)subroutines.size(), curr_BB, NULL);
                subroutines.push_back(subroutineInfo);
            }
        }
        //
        // do and endif do not have predicate and jump-to label,so we treated them as non-control instruction
        // the labels around them will decides the beginning of a new BB
        //
        if (i->isFlowControl() && i->opcode() != G4_endif)
        {
            G4_BB* next_BB = beginBB(labelMap, next_i);

            {
                if (i->opcode() == G4_jmpi || i->isCall())
                {
                    //
                    // add control flow edges <curr_BB,target>
                    //
                    if (i->getSrc(0)->isLabel())
                    {
                        addPredSuccEdges(curr_BB, getLabelBB(labelMap, i->getLabelStr()));
                    }
                    else if (i->asCFInst()->isIndirectJmp())
                    {
                        // indirect jmp
                        // For each label in the switch table, we insert a jmpi
                        // to that label. We keep the jmpi in the same
                        // block as the indirect jmp instead of creaing a new block for
                        // each of them, as the offset actually determines which jmpi
                        // instruction we will hit.
                        // FIXME: We may want to delay the emission of the jmps to
                        // each individual labels, so that we still maintain the property
                        // that every basic block ends with a control flow instruction
                        const std::list<G4_Label*>& jmpTargets = i->asCFInst()->getIndirectJmpLabels();
                        for (std::list<G4_Label*>::const_iterator it = jmpTargets.begin(); it != jmpTargets.end(); ++it)
                        {
                            G4_INST* jmpInst = builder->createInst(NULL, G4_jmpi, NULL, false, 1, NULL, *it, NULL, 0);
                            jmpInst->setIndirectJmpTarget();
                            INST_LIST_ITER jmpInstIter = builder->instList.end();
                            curr_BB->instList.splice(curr_BB->instList.end(), builder->instList, --jmpInstIter);
                            addPredSuccEdges(curr_BB, getLabelBB(labelMap, (*it)->getLabel()));
                        }
                    }

                    if (i->isCall())
                    {
                        //
                        // the <CALL,return addr> link is added temporarily to facilitate linking
                        // RETURN with return addresses. The link will be removed after it
                        // serves its purpose
                        // NOTE: No matter it has predicate, we add this link. We will check the predicate in handleReturn()
                        // and only remove the link when it is not a conditional call
                        //
                        addPredSuccEdges(curr_BB, next_BB);
                    }
                    else if (i->getPredicate() != NULL) // pred means conditional branch
                    {
                        // add fall through edge
                        addPredSuccEdges(curr_BB, next_BB);
                    }
                }    // if (i->opcode()
                else if (i->opcode() == G4_if || i->opcode() == G4_while ||
                    i->opcode() == G4_else)
                {
                    hasSIMDCF = true;
                    if (i->asCFInst()->getJip()->isLabel())
                    {
                        if (i->opcode() == G4_else || i->opcode() == G4_while)
                        {
                            // for G4_while, jump no matter predicate
                            addPredSuccEdges(curr_BB, getLabelBB(labelMap, i->asCFInst()->getJipLabelStr()));
                        }
                        else if ((i->getPredicate() != NULL) ||
                            ((i->getCondMod() != NULL) &&
                            (i->getSrc(0) != NULL) &&
                                (i->getSrc(1) != NULL)))
                        {
                            addPredSuccEdges(curr_BB, getLabelBB(labelMap, i->asCFInst()->getJipLabelStr()));
                        }
                    }
                    if (i->opcode() == G4_while)
                    {
                        // always do this since break jumps to while, otherwise code after while without predicate appears unreachable.
                        // add fall through edge if while is not the last instruction
                        if (next_BB)
                        {
                            addPredSuccEdges(curr_BB, next_BB);
                        }
                    }
                    else
                    {
                        //  always fall through
                        addPredSuccEdges(curr_BB, next_BB);     // add fall through edge
                    }
                }
                else if (i->opcode() == G4_break || i->opcode() == G4_cont || i->opcode() == G4_halt)
                {
                    // JIP and UIP must have been computed at this point
                    MUST_BE_TRUE(i->asCFInst()->getJip() != NULL && i->asCFInst()->getUip() != NULL,
                        "null JIP or UIP for break/cont instruction");
                    addPredSuccEdges(curr_BB, getLabelBB(labelMap, ((G4_Label *)i->asCFInst()->getJip())->getLabel()));

                    if (strcmp(i->asCFInst()->getJipLabelStr(), i->asCFInst()->getUipLabelStr()) != 0)
                        addPredSuccEdges(curr_BB, getLabelBB(labelMap, ((G4_Label *)i->asCFInst()->getUip())->getLabel()));

                    //
                    // pred means conditional branch
                    //
                    if (i->getPredicate() != NULL) // need to add fall through edge
                    {
                        // add fall through edge
                        addPredSuccEdges(curr_BB, next_BB);
                    }
                }
                else if (i->isReturn() || i->opcode() == G4_pseudo_exit)
                {
                    // does nothing for unconditional return;
                    // later phase will link the return and the return address
                    if (i->getPredicate() != NULL) // need to add fall through edge
                    {
                        // add fall through edge
                        addPredSuccEdges(curr_BB, next_BB);
                    }
                }
                else if (i->opcode() == G4_pseudo_fcall || i->opcode() == G4_pseudo_fc_call)
                {
                    addPredSuccEdges(curr_BB, next_BB);
                }
                else if (i->opcode() == G4_pseudo_fret || i->opcode() == G4_pseudo_fc_ret)
                {
                    if (i->getPredicate() != NULL)
                    {
                        // need to add fall through edge
                        addPredSuccEdges(curr_BB, next_BB);
                    }
                }
                else if (i->opcode() == G4_goto)
                {
                    hasNoUniformGoto = true;
                    hasSIMDCF = true;
                    addPredSuccEdges(curr_BB, getLabelBB(labelMap, i->asCFInst()->getUipLabelStr()));

                    if (i->getPredicate() != NULL)
                    {
                        // fall through
                        addPredSuccEdges(curr_BB, next_BB);
                    }
                }
                else
                {
                    MUST_BE_TRUE1(false, i->getLineNo(),
                        ERROR_INVALID_G4INST); // not yet handled
                }
            } // need edge
            curr_BB = next_BB;
        }  // flow control
        else if (curr_BB->isLastInstEOT())
        {
            //this is a send instruction that marks end of thread.
            //the basic block will end here with no outgoing links
            curr_BB = beginBB(labelMap, next_i);
        }
        else
        {
            //
            // examine next inst to see if it is a label inst (beginning of a BB)
            //
            if (next_i != NULL &&  // not the last inst
                next_i->isLabel())
            {
                G4_BB* next_BB = beginBB(labelMap, next_i);
                addPredSuccEdges(curr_BB, next_BB);
                curr_BB = next_BB;
            }
        }
    }       // while

    // we can do this only after fg is constructed
    pKernel->calculateSimdSize();

    // Jmpi instruction cannot be used when EU Fusion is enabled.
    bool hasGoto = hasNoUniformGoto;
    if (builder->noScalarJmp())
    {
        // No jmpi should be inserted after this point.
        hasGoto |= convertJmpiToGoto();
    }

    if (builder->getOption(vISA_DumpDotAll))
    {
        pKernel->dumpDotFile("beforeRemoveRedundantLabels");
    }

    removeRedundantLabels();

    if (builder->getOption(vISA_DumpDotAll))
    {
        pKernel->dumpDotFile("afterRemoveRedundantLabels");
    }

    // Ensure each block other than entry starts with a label.
    for (auto bb : BBs)
    {
        if (bb != entryBB && !bb->instList.empty())
        {
            G4_INST *inst = bb->instList.front();
            if (inst->isLabel())
                continue;

            char name[32];
            SNPRINTF(name, 32, "_AUTO_LABEL_%d", autoLabelId++);
            G4_Label *label = builder->createLabel(name, LABEL_BLOCK);
            G4_INST *labelInst = builder->createInst(
                nullptr, G4_label, nullptr, false, UNDEFINED_EXEC_SIZE, nullptr,
                label, nullptr, 0);
            bb->instList.push_front(labelInst);
        }
    }

    handleExit();

    handleReturn(labelMap, funcInfoHashTable);
    mergeReturn(labelMap, funcInfoHashTable);
    normalizeSubRoutineBB(funcInfoHashTable);

    handleWait();

    if (isStackCallFunc)
    {
        mergeFReturns();
    }


    if (builder->getOption(vISA_DumpDotAll))
    {
        pKernel->dumpDotFile("beforeRemoveUnreachableBlocks");
    }
    removeUnreachableBlocks();

    if (builder->getOption(vISA_DumpDotAll))
    {
        pKernel->dumpDotFile("afterRemoveUnreachableBlocks");
    }

    //
    // build the table of function info nodes
    //
    funcInfoTable.resize(funcInfoHashTable.size());

    for (FuncInfoHashTable::iterator it = funcInfoHashTable.begin(); it != funcInfoHashTable.end(); ++it) {
        FuncInfo* funcInfo = (*it).second;
        funcInfo->getInitBB()->setFuncInfo(funcInfo);
        funcInfo->getExitBB()->setFuncInfo(funcInfo);
        funcInfoTable[funcInfo->getId()] = funcInfo;
    }

    setPhysicalPredSucc();
    if (hasGoto)
    {
        if (builder->getOption(vISA_EnableStructurizer))
        {
            if (builder->getOption(vISA_DumpDotAll))
            {
                pKernel->dumpDotFile("before_doCFGStructurizer");
            }

            doCFGStructurize(this);

            if (builder->getOption(vISA_DumpDotAll))
            {
                pKernel->dumpDotFile("after_doCFGStructurizer");
            }
        }
        else
        {
            if (builder->getOption(vISA_DumpDotAll))
            {
                pKernel->dumpDotFile("beforeProcessGoto");
            }
            processGoto(hasSIMDCF);
            if (builder->getOption(vISA_DumpDotAll))
            {
                pKernel->dumpDotFile("afterProcessGoto");
            }
        }
    }

    // This finds back edges and populates blocks into function info objects.
    // The latter will be used to mark SIMD blocks. Prior to RA, no transformation
    // shall invalidate back edges (G4_BB -> G4_BB).
    reassignBlockIDs();
    findBackEdges();

    if (hasSIMDCF && builder->getOptions()->getTarget() == VISA_CM)
    {
        markSimdBlocks(labelMap, funcInfoHashTable);
        addSIMDEdges();
    }

    normalizeRegionDescriptors();
    localDataFlowAnalysis();
}

void FlowGraph::normalizeRegionDescriptors()
{
    for (auto bb : BBs)
    {
        for (auto inst : bb->instList)
        {
            uint16_t execSize = inst->getExecSize();
            for (unsigned i = 0, e = (unsigned)inst->getNumSrc(); i < e; ++i)
            {
                G4_Operand *src = inst->getSrc(i);
                if (!src || !src->isSrcRegRegion())
                    continue;

                G4_SrcRegRegion *srcRegion = src->asSrcRegRegion();
                RegionDesc *desc = srcRegion->getRegion();
                auto normDesc = builder->getNormalizedRegion(execSize, desc);
                if (normDesc && normDesc != desc)
                {
                    srcRegion->setRegion(normDesc, /*invariant*/ true);
                }
            }
        }
    }
}

//
// attempt to sink the pseudo-wait into the immediate succeeding send instruction (in same BB)
// by changing it into a sendc.
// If sinking fails, generate a fence with sendc.
//
void FlowGraph::handleWait()
{
    for (auto bb : BBs)
    {
        auto iter = bb->instList.begin(), instEnd = bb->instList.end();
        while (iter != instEnd)
        {
            auto inst = *iter;
            if (inst->isIntrinsic() &&
                inst->asIntrinsicInst()->getIntrinsicId() == Intrinsic::Wait)
            {
                bool sunk = false;
                auto nextIter = iter;
                ++nextIter;
                while (nextIter != instEnd)
                {
                    G4_INST* nextInst = *nextIter;
                    if (nextInst->isSend())
                    {
                        nextInst->setOpcode(nextInst->isSplitSend() ? G4_sendsc : G4_sendc);
                        sunk = true;
                        break;
                    }
                    else if (nextInst->isOptBarrier())
                    {
                        break;
                    }
                    ++nextIter;
                }
                if (!sunk)
                {
                    bool commitEnable = builder->needsFenceCommitEnable();
                    G4_INST* fenceInst = builder->createFenceInstruction(0, commitEnable, false, true);
                    bb->instList.insert(iter, fenceInst);
                }
                iter = bb->instList.erase(iter);
            }
            else
            {
                ++iter;
            }
        }
    }
}

//
// Each g4_pseudo_exit instruction will be translated into one of the following:
// -- a unconditional simd1 ret: translated into a EOT send (may be optionally merged with
//    an immediately preceding send)
// -- a conditional simd1 ret: translated into a jmpi to the exit block
// -- a non-uniforom ret: translated into a halt to the exit block
// for case 2 and 3 an exit block will be inserted, and it will consist of a EOT send
// plus a join if it's targeted by another goto instruction
//
void FlowGraph::handleExit()
{

    // blocks that end with non-uniform or conditional return
    std::vector<G4_BB*> exitBlocks;
    BB_LIST_ITER iter = BBs.begin(), iterEnd = BBs.end();
    for (; iter != iterEnd; ++iter)
    {
        G4_BB* bb = *iter;
        if (bb->instList.size() == 0)
        {
            continue;
        }
        if (subroutines.size() > 1 && bb == subroutines[1]->getInitBB())
        {
            // we've reached the start of the first non-kernel subroutine
            break;
        }
        G4_INST* lastInst = bb->instList.back();
        if (lastInst->opcode() == G4_pseudo_exit)
        {
            if (lastInst->getExecSize() == 1 &&
                !(builder->getFCPatchInfo() && builder->getFCPatchInfo()->getFCComposableKernel() == true))
            {
                //uniform return
                if (lastInst->getPredicate() != NULL)
                {
                    exitBlocks.push_back(bb);
                }
                else
                {
                    //generate EOT send
                    G4_INST* lastInst = bb->instList.back();
                    bb->instList.pop_back();
                    bool needsEOTSend = true;
                    // the EOT may be folded into the BB's last instruction if it's a send
                    // that supports EOT
                    if ((builder->getOptions()->getTarget() == VISA_3D) && bb->instList.size() > 1)
                    {
                        G4_INST* secondToLastInst = bb->instList.back();
                        if (secondToLastInst->canBeEOT() &&
                            !(secondToLastInst->getMsgDesc()->extMessageLength() > 2 &&
                                VISA_WA_CHECK(builder->getPWaTable(), WaSendsSrc1SizeLimitWhenEOT)))
                        {
                            secondToLastInst->getMsgDesc()->setEOT();
                            if (secondToLastInst->isSplitSend())
                            {
                                if (secondToLastInst->getSrc(3)->isImm())
                                {
                                    secondToLastInst->setSrc(builder->createImm(secondToLastInst->getMsgDesc()->getExtendedDesc(), Type_UD), 3);
                                }
                            }
                            needsEOTSend = false;
                            if (builder->getHasNullReturnSampler())
                            {
                                // insert a sampler cache flush with null return
                                // no need for this in other paths since they can never
                                // generate sampler with null return
                                int samplerFlushOpcode = 0x1F;
                                int samplerFlushFC = (SamplerSIMDMode::SIMD32 << 17) +
                                    (samplerFlushOpcode << 12);
                                int desc = G4_SendMsgDescriptor::createDesc(samplerFlushFC, true,
                                    1, 0);
                                G4_SrcRegRegion* sendMsgOpnd = builder->Create_Src_Opnd_From_Dcl(
                                    builder->getBuiltinR0(),
                                    builder->getRegionStride1());

                                G4_INST* samplerFlushInst = builder->createSendInst(nullptr, G4_send,
                                    8, builder->createNullDst(Type_UD), sendMsgOpnd,
                                    builder->createImm(SFID_SAMPLER, Type_UD), builder->createImm(desc, Type_UD),
                                    0, true, true, NULL, 0);
                                auto iter = bb->instList.end();
                                --iter;
                                bb->instList.insert(iter, samplerFlushInst);
                            }
                        }
                    }

                    if (needsEOTSend)
                    {
                        bb->addEOTSend(lastInst);
                    }
                }
            }
            else
            {
                exitBlocks.push_back(bb);
            }
        }
    }

    // create an exit BB
    if (exitBlocks.size() > 0)
    {
        G4_BB *exitBB = createNewBB();

        if (builder->getFCPatchInfo() &&
            builder->getFCPatchInfo()->getFCComposableKernel())
        {
            // For FC composable kernels insert exitBB as
            // last BB in BBs list. This automatically does
            // jump threading.
            BBs.push_back(exitBB);
        }
        else
        {
            BBs.insert(iter, exitBB);
        }

        G4_Label *exitLabel = builder->createLabel("__EXIT_BB", LABEL_BLOCK);
        G4_INST* label = createNewLabelInst(exitLabel);
        exitBB->instList.push_back(label);

        if (!(builder->getFCPatchInfo() &&
            builder->getFCPatchInfo()->getFCComposableKernel()))
        {
            // Dont insert EOT send for FC composable kernels
            exitBB->addEOTSend();
        }

        for (int i = 0, size = (int)exitBlocks.size(); i < size; i++)
        {
            G4_BB* retBB = exitBlocks[i];
            addPredSuccEdges(retBB, exitBB, false);
            G4_INST* retInst = retBB->instList.back();
            retBB->instList.pop_back();

            // Insert jump only if newly inserted exitBB is not lexical
            // successor of current BB
            auto lastBBIt = BBs.end();
            lastBBIt--;
            if ((*lastBBIt) == exitBB)
            {
                lastBBIt--;
                if ((*lastBBIt) == retBB)
                {
                    // This condition is BB layout dependent.
                    // However, we dont change BB layout in JIT
                    // and in case we do it in future, we
                    // will need to insert correct jumps
                    // there to preserve correctness.
                    continue;
                }
            }

            if (retInst->getExecSize() == 1)
            {
                G4_INST* jmpInst = builder->createInternalInst(retInst->getPredicate(), G4_jmpi,
                    NULL, false, 1, NULL, exitLabel, NULL, NULL, InstOpt_NoOpt);
                retBB->instList.push_back(jmpInst);
            }
            else
            {
                // uip for goto will be fixed later
                G4_INST* gotoInst = builder->createInternalCFInst(retInst->getPredicate(),
                    G4_goto, retInst->getExecSize(), exitLabel, exitLabel, InstOpt_NoOpt);
                retBB->instList.push_back(gotoInst);
            }
        }
    }

#ifdef _DEBUG

    // sanity check
    for (BB_LIST_ITER iter = BBs.begin(), iterEnd = BBs.end(); iter != iterEnd; ++iter)
    {
        G4_BB* bb = *iter;
        if (bb->instList.size() == 0)
        {
            continue;
        }
        G4_INST* lastInst = bb->instList.back();
        if (lastInst->opcode() == G4_pseudo_exit)
        {
            MUST_BE_TRUE(false, "All pseudo exits should be removed at this point");
        }
    }

#endif
}

//
// This phase iterates each BB and checks if the last inst of a BB is a "call foo".  If yes,
// the algorith traverses from the block of foo to search for RETURN and link the block of
// RETURN with the block of the return address
//

void FlowGraph::handleReturn(std::map<std::string, G4_BB*>& labelMap, FuncInfoHashTable& funcInfoHashTable)
{

    for (std::list<G4_BB*>::iterator it = BBs.begin(); it != BBs.end(); ++it)
    {
        G4_BB* bb = (*it);

        if (bb->isEndWithCall())
        {
            bb->setBBType(G4_BB_CALL_TYPE);
            G4_INST* last = bb->instList.back();
            if (last->getSrc(0)->isLabel())
            {
                // make sure bb has only two successors, one subroutine and one return addr
                MUST_BE_TRUE1(bb->Succs.size() == 2, last->getLineNo(),
                    ERROR_FLOWGRAPH);

                // find the  subroutine BB and return Addr BB
                std::string subName = last->getLabelStr();
                G4_BB* subBB = labelMap[subName];
                //
                // the fall through BB must be the front
                //
                G4_BB* retAddr = bb->Succs.front();
                prepareTraversal();
                linkReturnAddr(labelMap, subBB, retAddr);

                // set callee info for CALL
                FuncInfoHashTable::iterator calleeInfoLoc = funcInfoHashTable.find(subBB->getId());

                if (calleeInfoLoc != funcInfoHashTable.end()) {
                    (*calleeInfoLoc).second->incrementCallCount();
                    bb->setCalleeInfo((*calleeInfoLoc).second);
                    doIPA = true;
                }
                else {
                    unsigned funcId = (unsigned int)funcInfoHashTable.size();
                    FuncInfo *funcInfo = new (mem)FuncInfo(
                        funcId, subBB, retAddr->Preds.front());
                    std::pair<FuncInfoHashTable::iterator, bool> loc =
                        funcInfoHashTable.insert(
                            std::make_pair(subBB->getId(), funcInfo));
                    subBB->setBBType(G4_BB_INIT_TYPE);
                    retAddr->Preds.front()->setBBType(G4_BB_EXIT_TYPE);
                    MUST_BE_TRUE(loc.second, ERROR_FLOWGRAPH);
                    bb->setCalleeInfo((*(loc.first)).second);
                }

                // set up BB after CALL link
                bb->setBBAfterCall(retAddr);
                retAddr->setBBBeforeCall(bb);
                retAddr->setBBType(G4_BB_RETURN_TYPE);
            }
        }
    }
    //
    // remove <CALL, return addr> link when it is not a conditional call
    //
    for (std::list<G4_BB*>::iterator it = BBs.begin(); it != BBs.end(); ++it)
    {
        G4_BB* bb = (*it);

        if (bb->isEndWithCall())
        {
            G4_INST* last = bb->instList.back();
            if (last->getPredicate() == NULL)
            {
                MUST_BE_TRUE(!bb->Succs.empty(), ERROR_FLOWGRAPH);
                G4_BB* retAddr = bb->Succs.front();     // return BB must be the front
                bb->removeSuccEdge(retAddr);
                retAddr->removePredEdge(bb);
            }
        }
    }
}


void FlowGraph::linkReturnAddr(std::map<std::string, G4_BB*>& map, G4_BB* bb, G4_BB* returnAddr)
{
    if (bb->isAlreadyTraversed(traversalNum))
        return;
    bb->markTraversed(traversalNum);

    //
    // check if bb contain RETURN (last inst)
    //
    G4_INST* last = bb->instList.back();
    if (!bb->instList.empty() && last->isReturn())
    {
        //
        // check the direct recursive call here!
        //
        if (bb == returnAddr && hasStackCalls == false)
        {
            MUST_BE_TRUE(false,
                "ERROR: Do not support recursive subroutine call!");
        }
        addPredSuccEdges(bb, returnAddr, false); // IMPORTANT: add to the back to allow fall through edge is in the front, which is used by fallThroughBB()

        if (last->getPredicate() != NULL)
        {
            MUST_BE_TRUE(bb->Succs.size() > 1, ERROR_FLOWGRAPH);
            linkReturnAddr(map, bb->Succs.front(), returnAddr);     // checked, the fall through BB must be the front
        }

        return;
    }
    else
    {
        // handle returns in BB that are not part of CFG.
        for (std::map<int, G4_BB*>::iterator it = bb->getBBListStart(); it != bb->getBBListEnd(); ++it)
            linkReturnAddr(map, it->second, returnAddr);
    }
}


//
// This phase iterates each BB and checks if the last inst of a BB is a "call foo".  If yes,
// the algorith traverses from the block of foo to search for RETURN. If multiple returns exist,
// the algorithm will merge returns into one
// [Reason]:to handle the case when spill codes are needed between multiple return BBs of one subroutine
//          and the afterCAll BB. It is impossible to insert spill codes of different return BBs all before
//          afterCall BB.
//
void FlowGraph::mergeReturn(Label_BB_Map& map, FuncInfoHashTable& funcInfoHashTable)
{
    BB_LIST returnBBList;
    for (BB_LIST_ITER it = BBs.begin(); it != BBs.end(); ++it)
    {
        G4_BB* bb = (*it);

        if (bb->isEndWithCall())
        {
            G4_INST* last = bb->instList.back();
            if (last->getSrc(0)->isLabel())
            {
                // find the  subroutine BB and return Addr BB
                std::string subName = last->getLabelStr();
                G4_BB* subBB = map[subName];
                G4_BB* retAddr = bb->BBAfterCall();
                prepareTraversal();
                returnBBList.clear();
                searchReturn(subBB, retAddr, returnBBList);
                G4_BB* mergedExitBB = mergeSubRoutineReturn(bb, retAddr, returnBBList);

                // update callee exit block info
                FuncInfoHashTable::iterator calleeInfoLoc = funcInfoHashTable.find(subBB->getId());
                MUST_BE_TRUE(calleeInfoLoc != funcInfoHashTable.end(), ERROR_FLOWGRAPH);
                if (mergedExitBB) {
                    (*calleeInfoLoc).second->getExitBB()->unsetBBType(G4_BB_EXIT_TYPE);
                    mergedExitBB->setBBType(G4_BB_EXIT_TYPE);
                    (*calleeInfoLoc).second->updateExitBB(mergedExitBB);
                }
            }
        }
    }

}

void FlowGraph::searchReturn(G4_BB* bb, G4_BB* returnAddr, BB_LIST & retBBList)
{
    if (bb->isAlreadyTraversed(traversalNum))
        return;
    bb->markTraversed(traversalNum);

    //
    // check if bb contain RETURN (last inst)
    //
    G4_INST* last = (bb->instList.empty()) ? NULL : bb->instList.back();
    if (last && bb->isSuccBB(returnAddr) && last->isReturn())
    {
        if (bb == returnAddr && hasStackCalls == false)
        {
            MUST_BE_TRUE(false,
                "ERROR: Do not support recursive subroutine call!");
        }
        retBBList.push_back(bb);            // if bb contained return, check in to retBBList

        if (last->getPredicate() != NULL)
        {
            MUST_BE_TRUE(bb->Succs.size() > 1, ERROR_FLOWGRAPH);
            searchReturn(bb->Succs.front(), returnAddr, retBBList);
        }
        return;
    }
    else
    {
        if (bb->isEndWithCall())
        {
            searchReturn(bb->BBAfterCall(), returnAddr, retBBList);
        }
        else if (bb->getBBType() == G4_BB_EXIT_TYPE)
        {
            // Do nothing.
        }
        else
        {
            for (std::map<int, G4_BB*>::iterator it = bb->getBBListStart(); it != bb->getBBListEnd(); ++it)
            {
                searchReturn(it->second, returnAddr, retBBList);
            }
        }
    }
}

G4_BB* FlowGraph::mergeSubRoutineReturn(G4_BB* bb, G4_BB* returnAddr, BB_LIST & retBBList)
{

    G4_BB* newBB = NULL;

    if (retBBList.size() > 1)     // For return number >1, need to merge returns
    {
        builder->instList.clear();

        //
        // insert newBB to fg.BBs list
        //
        newBB = createNewBB();
        BBs.insert(BBs.end(), newBB);
        // choose the last BB in retBBList as a candidate
        G4_BB* candidateBB = *(retBBList.rbegin());
        newBB->setStartBlock(candidateBB->getStartBlock());
        // Add <newBB, succBB> edges
        G4_INST* last = candidateBB->instList.back();
        BB_LIST_ITER succIt = (last->getPredicate() == NULL) ? candidateBB->Succs.begin() : (++candidateBB->Succs.begin());

        for (; succIt != candidateBB->Succs.end(); ++succIt) {
            addPredSuccEdges(newBB, (*succIt), false);
        }

        //
        // Create a label for the newBB and insert return to new BB
        //
        char str[64];
        SNPRINTF(str, 64, "LABEL__%d", newBB->getId());
        G4_Label* lab = builder->createLabel(str, LABEL_BLOCK);
        G4_INST* labelInst = createNewLabelInst(lab);

        // exitBB is really just a dummy one for analysis, and does not need a return
        // we will instead leave the return in each of its predecessors
        newBB->instList.push_back(labelInst);

        //
        // Deal with all return BBs
        //
        for (BB_LIST_ITER it = retBBList.begin(); it != retBBList.end(); ++it)
        {
            G4_BB * retBB = (*it);
            if (retBB->getId() == newBB->getId())
            {
                continue;
            }
            last = retBB->instList.back();
            // remove <retBB,its succBB> edges, do not remove the fall through edge if predicated
            BB_LIST retSuccsList;
            retSuccsList.assign(retBB->Succs.begin(), retBB->Succs.end());
            for (BB_LIST_ITER retSuccIt = retSuccsList.begin(); retSuccIt != retSuccsList.end(); ++retSuccIt)
            {
                G4_BB * retSuccBB = (*retSuccIt);
                if (last->getPredicate() != NULL && retSuccIt == retSuccsList.begin()) continue;
                retBB->removeSuccEdge(retSuccBB);
                retSuccBB->removePredEdge(retBB);
            }
            // Add <retBB,newBB> edges
            addPredSuccEdges(retBB, newBB, false);
        }
    }

    return newBB;
}

void FlowGraph::decoupleInitBlock(G4_BB* bb, FuncInfoHashTable& funcInfoHashTable)
{
    G4_BB* oldInitBB = bb;
    G4_BB* newInitBB = createNewBB();
    BBs.insert(BBs.end(), newInitBB);

    FuncInfoHashTable::iterator old_iter = funcInfoHashTable.find(oldInitBB->getId());
    MUST_BE_TRUE(old_iter != funcInfoHashTable.end(), " Function info is not in hashtable.");
    G4_BB* exitBB = (*old_iter).second->getExitBB();
    unsigned funcId = (*old_iter).second->getId();

    BB_LIST_ITER kt = oldInitBB->Preds.begin();
    while (kt != oldInitBB->Preds.end())
    {
        // the pred of this new INIT BB are all call BB
        if ((*kt)->getBBType() & G4_BB_CALL_TYPE) {

            newInitBB->Preds.push_back((*kt));

            BB_LIST_ITER jt = (*kt)->Succs.begin();
            while (jt != (*kt)->Succs.end()) {
                if ((*jt) == oldInitBB)
                {
                    break;
                }
                jt++;
            }
            MUST_BE_TRUE(jt != (*kt)->Succs.end(), ERROR_FLOWGRAPH);
            (*kt)->Succs.insert(jt, newInitBB);
            (*kt)->Succs.erase(jt);

            // update info in func table
            FuncInfoHashTable::iterator calleeInfoLoc = funcInfoHashTable.find(newInitBB->getId());

            if (calleeInfoLoc != funcInfoHashTable.end()) {
                (*calleeInfoLoc).second->incrementCallCount();
                (*kt)->setCalleeInfo((*calleeInfoLoc).second);
            }
            else {
                FuncInfo *funcInfo = new (mem)FuncInfo(
                    funcId, newInitBB, exitBB);
                std::pair<FuncInfoHashTable::iterator, bool> loc =
                    funcInfoHashTable.insert(
                        std::make_pair(newInitBB->getId(), funcInfo));
                MUST_BE_TRUE(loc.second, ERROR_FLOWGRAPH);
                (*kt)->setCalleeInfo((*(loc.first)).second);
            }

            BB_LIST_ITER tmp_kt = kt;
            ++kt;
            // erase this pred from old INIT BB's pred
            oldInitBB->Preds.erase(tmp_kt);
        }
        else
        {
            ++kt;
        }
    }

    // Erase item from unordered_map using
    // key rather than iterator since iterator may be
    // invalid due to insert operation since last find.
    {
        FuncInfoHashTable::iterator calleeInfoLoc = funcInfoHashTable.find(oldInitBB->getId());
        if (calleeInfoLoc != funcInfoHashTable.end()) {
            (*calleeInfoLoc).second->~FuncInfo();
        }
        funcInfoHashTable.erase(oldInitBB->getId());
    }

    oldInitBB->unsetBBType(G4_BB_INIT_TYPE);
    newInitBB->setBBType(G4_BB_INIT_TYPE);
    addPredSuccEdges(newInitBB, oldInitBB);

    char str[64];
    SNPRINTF(str, 64, "LABEL__EMPTYBB__%d", newInitBB->getId());
    G4_Label* label = builder->createLabel(str, LABEL_BLOCK);
    G4_INST* labelInst = createNewLabelInst(label);
    newInitBB->instList.push_back(labelInst);
}


void FlowGraph::decoupleExitBlock(G4_BB* bb)
{
    G4_BB* oldExitBB = bb;
    G4_BB* newExitBB = createNewBB();
    BBs.insert(BBs.end(), newExitBB);

    BB_LIST_ITER kt = oldExitBB->Succs.begin();

    while (kt != oldExitBB->Succs.end())
    {
        // the succs of this new EXIT BB are all call ret
        if ((*kt)->getBBType() & G4_BB_RETURN_TYPE) {

            newExitBB->Succs.push_back((*kt));

            BB_LIST_ITER jt = (*kt)->Preds.begin();
            while (jt != (*kt)->Preds.end()) {
                if ((*jt) == oldExitBB)
                {
                    break;
                }
                jt++;
            }
            MUST_BE_TRUE(jt != (*kt)->Preds.end(), ERROR_FLOWGRAPH);
            (*kt)->Preds.insert(jt, newExitBB);
            (*kt)->Preds.erase(jt);

            (*kt)->BBBeforeCall()->getCalleeInfo()->updateExitBB(newExitBB);

            BB_LIST_ITER tmp_kt = kt;
            ++kt;
            // erase this succ from old EXIT BB's succs
            oldExitBB->Succs.erase(tmp_kt);
        }
        else {
            ++kt;
        }
    }

    oldExitBB->unsetBBType(G4_BB_EXIT_TYPE);
    newExitBB->setBBType(G4_BB_EXIT_TYPE);
    addPredSuccEdges(oldExitBB, newExitBB);

    char str[64];
    SNPRINTF(str, 64, "LABEL__EMPTYBB__%d", newExitBB->getId());
    G4_Label* label = builder->createLabel(str, LABEL_BLOCK);
    G4_INST* labelInst = createNewLabelInst(label);
    newExitBB->instList.push_back(labelInst);
}

void FlowGraph::decoupleReturnBlock(G4_BB* bb)
{
    G4_BB* oldRetBB = bb;
    G4_BB* newRetBB = createNewBB();
    BBs.insert(BBs.end(), newRetBB);
    G4_BB* itsExitBB = oldRetBB->BBBeforeCall()->getCalleeInfo()->getExitBB();

    BB_LIST_ITER jt = itsExitBB->Succs.begin();

    for (; jt != itsExitBB->Succs.end(); ++jt)
    {
        if ((*jt) == oldRetBB)
        {
            break;
        }
    }

    MUST_BE_TRUE(jt != itsExitBB->Succs.end(), ERROR_FLOWGRAPH);

    itsExitBB->Succs.insert(jt, newRetBB);
    itsExitBB->Succs.erase(jt);
    newRetBB->Preds.push_back(itsExitBB);
    newRetBB->Succs.push_back(oldRetBB);

    BB_LIST_ITER kt = oldRetBB->Preds.begin();

    for (; kt != oldRetBB->Preds.end(); ++kt)
    {
        if ((*kt) == itsExitBB)
        {
            break;
        }
    }

    MUST_BE_TRUE(kt != oldRetBB->Preds.end(), ERROR_FLOWGRAPH);

    oldRetBB->Preds.insert(kt, newRetBB);
    oldRetBB->Preds.erase(kt);
    oldRetBB->Preds.unique();


    oldRetBB->unsetBBType(G4_BB_RETURN_TYPE);
    newRetBB->setBBType(G4_BB_RETURN_TYPE);

    newRetBB->setBBBeforeCall(oldRetBB->BBBeforeCall());
    oldRetBB->BBBeforeCall()->setBBAfterCall(newRetBB);

    bb->setBBBeforeCall(NULL);

    char str[64];
    SNPRINTF(str, 64, "LABEL__EMPTYBB__%d", newRetBB->getId());
    G4_Label* label = builder->createLabel(str, LABEL_BLOCK);
    G4_INST* labelInst = createNewLabelInst(label);
    newRetBB->instList.push_back(labelInst);
}

void FlowGraph::normalizeSubRoutineBB(FuncInfoHashTable& funcInfoTable)
{
    for (BB_LIST_ITER it = BBs.begin(); it != BBs.end(); ++it)
    {
        if (((*it)->getBBType() & G4_BB_CALL_TYPE))
        {
            G4_BB* callBB = (*it);

            if (callBB->getBBType() & G4_BB_INIT_TYPE)
            {
                decoupleInitBlock(callBB, funcInfoTable);
            }
            if (callBB->getBBType() & G4_BB_EXIT_TYPE)
            {
                decoupleExitBlock(callBB);
            }

            if (callBB->getBBType() & G4_BB_RETURN_TYPE)
            {
                decoupleReturnBlock(callBB);
            }
        }
        else if (((*it)->getBBType() & G4_BB_INIT_TYPE))
        {
            G4_BB* initBB = (*it);
            if (initBB->getBBType() != G4_BB_INIT_TYPE)
            {
                decoupleInitBlock(initBB, funcInfoTable);
            }
        }
        else if (((*it)->getBBType() & G4_BB_EXIT_TYPE))
        {
            G4_BB* exitBB = (*it);

            if (exitBB->getBBType() & G4_BB_INIT_TYPE)
            {
                decoupleInitBlock(exitBB, funcInfoTable);
            }
            if (exitBB->getBBType() & G4_BB_CALL_TYPE)
            {
                decoupleExitBlock(exitBB);
            }

            if (exitBB->getBBType() & G4_BB_RETURN_TYPE)
            {
                decoupleReturnBlock(exitBB);
            }
        }
        else if (((*it)->getBBType() & G4_BB_RETURN_TYPE))
        {
            G4_BB* retBB = (*it);

            if (retBB->getBBType() & G4_BB_INIT_TYPE)
            {
                MUST_BE_TRUE(false, ERROR_FLOWGRAPH);
            }
            if (retBB->getBBType() & G4_BB_EXIT_TYPE)
            {
                MUST_BE_TRUE(!(retBB->getBBType() & G4_BB_CALL_TYPE), ERROR_FLOWGRAPH);
                decoupleReturnBlock(retBB);
            }
            else if (retBB->getBBType() & G4_BB_CALL_TYPE)
            {
                decoupleReturnBlock(retBB);
            }
            else if (retBB->Preds.size() > 1)
            {
                decoupleReturnBlock(retBB);
            }
        }
    }

    /*
    Clearing BB list, just a pre-caution incase
    later phases add/remove BBs and this list is
    not kept consistent.
    */
    for (BB_LIST_ITER it = BBs.begin(); it != BBs.end(); ++it)
    {
        G4_BB* bb = (*it);
        bb->clearBBList();
    }
}

//
// This function does a DFS and any blocks that get visited will have their
// preId set according to the ordering. Blocks that never get visited will
// have their preId unmodified.
//
void doDFS(G4_BB* startBB, unsigned int p)
{
    unsigned int currP = p;
    std::stack<G4_BB*> traversalStack;
    traversalStack.push(startBB);

    while (!traversalStack.empty())
    {
        G4_BB* currBB = traversalStack.top();
        traversalStack.pop();
        if (currBB->getPreId() != UINT_MAX)
        {
            continue;
        }
        currBB->setPreId(currP++);

        BB_LIST_ITER IE = currBB->Succs.end();
        for (BB_LIST_ITER it = currBB->Succs.begin(); it != IE; ++it)
        {
            G4_BB* tmp = *it;
            if (tmp->getPreId() == UINT_MAX)
            {
                traversalStack.push(tmp);
            }
        }
    }
}

//
// The optimization pass will remove unreachable blocks from functions. Later compilation phases
// assume that the only unreachable code that can exist in a function is the block with
// return/pseudo_fret instruction. All other unreachable code should be removed. The only time
// blocks with return/pseudo_fret will be removed is when the header of that function itself
// is deemed unreachable.
//
void FlowGraph::removeUnreachableBlocks()
{
    unsigned preId = 0;
    std::vector<bool> canRemove(BBs.size(), false);

    //
    // initializations
    //
    for (std::list<G4_BB*>::iterator it = BBs.begin(); it != BBs.end(); ++it)
    {
        (*it)->setPreId(UINT_MAX);
    }
    //
    // assign DFS based pre/rpost ids to all blocks in the main program
    //
    doDFS(entryBB, preId);

    for (BB_LIST_ITER it = BBs.begin(), itEnd = BBs.end(); it != itEnd; ++it)
    {
        if ((*it)->getPreId() == UINT_MAX && (*it)->getStartBlock() == NULL)
        {
            // Entire function is unreachable. So it should be ok
            // to delete the return as well.
            canRemove[(*it)->getId()] = true;
        }

    }

    //
    // Basic blocks with preId/rpostId set to UINT_MAX are unreachable
    //
    BB_LIST_ITER it = BBs.begin();
    while (it != BBs.end())
    {
        G4_BB* bb = (*it);

        if (bb->getPreId() == UINT_MAX)
        {
            //leaving dangling BBs with return/EOT in for now.
            //workaround to handle unreachable return
            //for example return after infinite loop.
            if (((bb->isEndWithFRet() || (bb->instList.size() > 0 && (G4_INST*)bb->instList.back()->isReturn())) &&
                (bb->getStartBlock() && canRemove[bb->getStartBlock()->getId()] == false)) ||
                (bb->instList.size() > 0 && bb->instList.back()->isEOT()))
            {
                it++;
                continue;
            }

            while (bb->Succs.size() > 0)
            {
                removePredSuccEdges(bb, bb->Succs.front());
            }

            if (bb->getStartBlock() != NULL)
            {
                bb->getStartBlock()->removeBlockFromBBList(bb->getId());
            }
            BB_LIST_ITER prev = it;
            prev++;
            BBs.erase(it);
            it = prev;
        }
        else
        {
            it++;
        }
    }

    reassignBlockIDs();
}

void FlowGraph::AssignDFSBasedIds(G4_BB* bb, unsigned &preId, unsigned &postId, std::list<G4_BB*>& rpoBBList)
{
    bb->setPreId(preId++);
    //
    // perform a context-sensitive (actually just call-sensitive) traversal.
    // if this is CALL block then we need to resume DFS at the corresponding RETURN block
    //
    if (bb->getBBType() & G4_BB_CALL_TYPE)
    {
        G4_BB* returnBB = bb->BBAfterCall();
        MUST_BE_TRUE(returnBB->getPreId() == UINT_MAX, ERROR_FLOWGRAPH);
        MUST_BE_TRUE(bb->Succs.front()->getBBType() & G4_BB_INIT_TYPE, ERROR_FLOWGRAPH);
        MUST_BE_TRUE(bb->Succs.size() == 1, ERROR_FLOWGRAPH);
        AssignDFSBasedIds(returnBB, preId, postId, rpoBBList);
    }
    //
    // if this is the EXIT block of subroutine then just return. the CALL block
    // will ensure traversal of the RETURN block.
    //
    else if (bb->getBBType() & G4_BB_EXIT_TYPE)
    {
        // do nothing
    }
    else {
        std::list<G4_BB*> ordered_succs;
        G4_INST* last = (bb->instList.empty()) ? NULL : bb->instList.back();
        //
        // visit "else" branches before "then" branches so that "then" block get a
        // lower rpo number than "else" blocks.
        //
        if (last && last->getPredicate() &&
            (last->opcode() == G4_jmpi || last->opcode() == G4_if) &&
            bb->Succs.size() == 2)
        {
            G4_BB* true_branch = bb->Succs.front();
            G4_BB* false_branch = bb->Succs.back();
            ordered_succs.push_back(false_branch);
            ordered_succs.push_back(true_branch);
        }

        std::list<G4_BB*>& succs = (ordered_succs.empty()) ? bb->Succs : ordered_succs;
        //
        // visit all successors
        //
        for (std::list<G4_BB*>::iterator it = succs.begin(); it != succs.end(); ++it)
        {
            G4_BB* succBB = *it;
            //
            // visit unmarked successors
            //
            if (succBB->getPreId() == UINT_MAX) {
                AssignDFSBasedIds(*it, preId, postId, rpoBBList);
            }
            //
            // track back-edges
            //
            else if (succBB->getRPostId() == UINT_MAX) {
                backEdges.push_back(Edge(bb, succBB));
            }
        }
    }
    //
    // Set the post id in the rpostid field. The caller will update the field to the
    // actual postid number.
    //
    bb->setRPostId(postId++);
    //
    // Link the blocks in RPO order and also add it to the list
    //
    if (rpoBBList.size() > 0)
    {
        bb->setNextRPOBlock(rpoBBList.front());
    }
    else {
        bb->setNextRPOBlock(NULL);
    }
    rpoBBList.push_front(bb);
}

// prevent overwriting dump file and indicate compilation order with dump serial number
static _THREAD int dotDumpCount = 0;

//
// Remove the fall through edges between subroutine and its non-caller preds
// Remove basic blocks that only contain a label, funcation lebels are untouched.
//
void FlowGraph::removeRedundantLabels()
{
    for (BB_LIST_ITER it = BBs.begin(); it != BBs.end();)
    {
        G4_BB* bb = *it;
        if (bb == entryBB)
        {
            it++;
            continue;
        }
        if (bb->Succs.size() == 0 && bb->Preds.size() == 0) {
            //leaving dangling BBs with return in for now.
            //workaround to handle unreachable return
            //for example return after infinite loop.
            if (bb->isEndWithFRet() || (bb->instList.size() > 0 && ((G4_INST*)bb->instList.back())->isReturn()))
            {
                it++;
                continue;
            }

            if (bb->getStartBlock() != NULL)
            {
                bb->getStartBlock()->removeBlockFromBBList(bb->getId());
            }
            bb->instList.clear();
            BB_LIST_ITER rt = it++;
            BBs.erase(rt);

            continue;
        }
        //
        // The removal candidates will have a single successor and a single inst
        //
        if (bb->Succs.size() == 1 && bb->instList.size() == 1)
        {
            G4_INST* removedBlockInst = bb->instList.front();
            if (removedBlockInst->isLabel() == false ||
                strncmp(removedBlockInst->getLabelStr(), "LABEL__EMPTYBB", 14) == 0 ||
                strncmp(removedBlockInst->getLabelStr(), "__AUTO_GENERATED_DUMMY_LAST_BB", 30) == 0)
            {
                ++it;
                continue;
            }

            // check if the label is a function label
            unsigned int numNonCallerPreds = 0;
            BB_LIST_ITER lt = bb->Preds.begin();
            bool isFuncLabel = true;
            G4_BB* pred_bb = NULL;
            for (; lt != bb->Preds.end(); ++lt)
            {
                if (!((*lt)->isEndWithCall()))
                {
                    if (numNonCallerPreds > 0)
                    {
                        isFuncLabel = false;
                        break;
                    }
                    numNonCallerPreds++;
                    pred_bb = (*lt);
                }
                else
                {
                    G4_INST *i = (*lt)->instList.back();
                    if (i->getSrc(0)->isLabel())
                    {
                        if (i->getSrc(0) != removedBlockInst->getLabel())
                        {
                            if (numNonCallerPreds > 0)
                            {
                                isFuncLabel = false;
                                break;
                            }
                            numNonCallerPreds++;
                            pred_bb = (*lt);
                        }
                    }
                }
            }

            // keep the function label there such that we have an empty init BB for this subroutine.
            if (isFuncLabel && numNonCallerPreds < bb->Preds.size())
            {
                // remove fall through edge.
                if (pred_bb)
                {
                    removePredSuccEdges(pred_bb, bb);
                }
                removedBlockInst->getLabel()->setFuncLabel(true);
                ++it;
                continue;
            }

            G4_Label *succ_label = bb->Succs.front()->instList.front()->getLabel();

            // check if the last inst of pred is a control flow inst
            lt = bb->Preds.begin();
            for (; lt != bb->Preds.end(); ++lt)
            {
                BB_LIST_ITER jt = (*lt)->Succs.begin();

                for (; jt != (*lt)->Succs.end(); ++jt)
                {
                    if ((*jt) == bb) {
                        break;
                    }
                }
                G4_INST *i = (*lt)->instList.back();
                // replace label in instructions
                if (i->isFlowControl())
                {
                    if (i->isIndirectJmpTarget())
                    {
                        // due to the switchjmp we may have multiple jmpi
                        // at the end of a block.
                        bool foundMatchingJmp = false;
                        for (INST_LIST::iterator iter = --(*lt)->instList.end();
                            iter != (*lt)->instList.begin(); --iter)
                        {
                            i = *iter;
                            if (i->opcode() == G4_jmpi)
                            {
                                if (i->getSrc(0)->isLabel() &&
                                    i->getSrc(0) == removedBlockInst->getLabel())
                                {
                                    i->setSrc(succ_label, 0);
                                    foundMatchingJmp = true;
                                    break;
                                }
                            }
                            else
                            {
                                break;
                            }
                        }
                        MUST_BE_TRUE(foundMatchingJmp, "Can't find the matching jmpi to the given label");
                    }
                    else if (i->opcode() == G4_jmpi || i->isCall())
                    {
                        if (i->getSrc(0)->isLabel())
                        {
                            if (i->getSrc(0) == removedBlockInst->getLabel())
                            {
                                i->setSrc(succ_label, 0);
                            }
                        }
                    }
                    else if (i->opcode() == G4_if || i->opcode() == G4_while ||
                        i->opcode() == G4_else)
                    {
                        if (i->asCFInst()->getJip()->isLabel())
                        {
                            if (i->asCFInst()->getJip() == removedBlockInst->getLabel())
                            {
                                if (i->opcode() == G4_else || i->opcode() == G4_while)
                                {
                                    // for G4_while, jump no matter predicate
                                    i->asCFInst()->setJip(succ_label);
                                }
                                // for G4_if , jump only when it has predictate; if no predicate, no jump
                                // this rule changed in GT as below
                                // [(<pred>)] if (<exec_size>) null null null <JIP>
                                // if[.<cmod>] (<exec_size>) null <src0> <src1> <JIP>
                                else if ((i->getPredicate() != NULL) ||
                                    ((i->getCondMod() != NULL) &&
                                    (i->getSrc(0) != NULL) &&
                                        (i->getSrc(1) != NULL))) {
                                    i->asCFInst()->setJip(succ_label);
                                }
                            }
                        }
                    }
                    else if (i->opcode() == G4_break || i->opcode() == G4_cont || i->opcode() == G4_halt)
                    {
                        // JIP and UIP must have been computed at this point
                        MUST_BE_TRUE(i->asCFInst()->getJip() != NULL && i->asCFInst()->getUip() != NULL,
                            "null JIP or UIP for break/cont instruction");
                        if (i->asCFInst()->getJip() == removedBlockInst->getLabel())
                        {
                            i->asCFInst()->setJip(succ_label);
                        }

                        if (i->asCFInst()->getUip() == removedBlockInst->getLabel())
                        {
                            i->asCFInst()->setUip(succ_label);
                        }
                    }
                    else if (i->opcode() == G4_goto)
                    {
                        // UIP must have been computed at this point
                        MUST_BE_TRUE(i->asCFInst()->getUip() != NULL,
                            "null UIP for goto instruction");
                        if (i->asCFInst()->getUip() == removedBlockInst->getLabel())
                        {
                            i->asCFInst()->setUip(succ_label);
                        }
                        if (i->asCFInst()->getUip() == removedBlockInst->getLabel())
                        {
                            i->asCFInst()->setUip(succ_label);
                        }
                    }
                }

                (*lt)->Succs.insert(jt, bb->Succs.front());
                (*lt)->Succs.erase(jt);

                // [Bug1915]: In rare case the precessor may have more than one Succ edge pointing
                // to the same BB, due to empty block being eliminated.  For example, with
                // BB1:
                // (P) jmp BB3
                // BB2:
                // BB3:
                // BB4:
                // ...
                // After BB2 is eliminated BB1's two succ will both point to BB3.
                // When we get rid of BB3,
                // we have to make sure we update both Succ edges as we'd otherwise create an
                // edge to a non-existing BB.  Note that we don't just delete the edge because
                // elsewhere there may be assumptions that if a BB ends with a jump it must have
                // two successors
                {
                    BB_LIST_ITER succs = (*lt)->Succs.begin();
                    BB_LIST_ITER end = (*lt)->Succs.end();
                    while (succs != end)
                    {
                        BB_LIST_ITER iter = succs;
                        ++succs;
                        if ((*iter) == bb)
                        {
                            (*lt)->Succs.insert(iter, bb->Succs.front());
                            (*lt)->Succs.erase(iter);
                        }
                    }
                }
            }

            //
            // Replace the unique successor's predecessor links with the removed block's predessors.
            //
            BB_LIST_ITER kt = bb->Succs.front()->Preds.begin();

            for (; kt != bb->Succs.front()->Preds.end(); ++kt)
            {
                if ((*kt) == bb)
                {
                    break;
                }
            }

            BB_LIST_ITER mt = bb->Preds.begin();

            for (; mt != bb->Preds.end(); ++mt)
            {
                bb->Succs.front()->Preds.insert(kt, *mt);
            }

            bb->Succs.front()->Preds.erase(kt);
            bb->Succs.front()->Preds.unique();
            //
            // Propagate the removed block's type to its unique successor.
            //
            bb->Succs.front()->setBBType(bb->getBBType());
            //
            // Update the call graph if this is a RETURN node.
            //
            if (bb->BBBeforeCall())
            {
                bb->BBBeforeCall()->setBBAfterCall(bb->Succs.front());
                bb->Succs.front()->setBBBeforeCall(bb->BBBeforeCall());
            }
            //
            // A CALL node should never be empty.
            //
            else if (bb->BBBeforeCall())
            {
                MUST_BE_TRUE(false, ERROR_FLOWGRAPH);
            }
            //
            // Remove the block to be removed.
            //
            if (bb->getStartBlock() != NULL)
            {
                bb->getStartBlock()->removeBlockFromBBList(bb->getId());
            }
            bb->Succs.clear();
            bb->Preds.clear();
            bb->instList.clear();

            BB_LIST_ITER rt = it++;
            BBs.erase(rt);
        }
        else
        {
            ++it;
        }
    }

    reassignBlockIDs();
}

//
// remove any mov with the same src and dst opnds
//
void FlowGraph::removeRedundMov()
{
    for (std::list<G4_BB*>::iterator it = BBs.begin(); it != BBs.end(); ++it)
    {
        G4_BB* bb = (*it);

        INST_LIST_ITER curr_iter = bb->instList.begin();
        while (curr_iter != bb->instList.end())
        {
            G4_INST* inst = (*curr_iter);
            if (inst->opcode() == G4_mov &&
                inst->getCondMod() == NULL &&
                inst->getSaturate() == false)
            {
                G4_Operand *src = inst->getSrc(0);
                G4_DstRegRegion *dst = inst->getDst();

                if (src->isSrcRegRegion())
                {
                    G4_SrcRegRegion* srcRgn = (G4_SrcRegRegion*)src;
                    if (!dst->isIndirect() &&
                        !srcRgn->isIndirect() &&
                        dst->isGreg() &&
                        src->isGreg() &&
                        srcRgn->getModifier() == Mod_src_undef &&
                        dst->getType() == src->getType())
                    {
                        G4_RegVar* dstBase = (G4_RegVar*)dst->getBase();
                        G4_RegVar* srcBase = (G4_RegVar*)srcRgn->getBase();

                        int dstSubReg, dstReg, srcSubReg, srcReg;

                        dstSubReg = dst->getSubRegOff() + dstBase->getPhyRegOff();
                        srcSubReg = srcRgn->getSubRegOff() + srcBase->getPhyRegOff();
                        dstReg = dst->getRegOff() + dstBase->getPhyReg()->asGreg()->getRegNum();
                        srcReg = srcRgn->getRegOff() + srcBase->getPhyReg()->asGreg()->getRegNum();

                        if (dstReg == srcReg && dstSubReg == srcSubReg)
                        {
                            uint16_t stride = 0;
                            RegionDesc *rd = srcRgn->getRegion();
                            unsigned ExSize = inst->getExecSize();
                            if (ExSize == 1 ||
                                (rd->isSingleStride(ExSize, stride) &&
                                (dst->getHorzStride() == stride)))
                            {
                                curr_iter = bb->instList.erase(curr_iter);
                                continue;
                            }
                        }
                    }
                }
            }
            ++curr_iter;
        }
    }
}

//
// Remove any placeholder empty blocks that could have been inserted to aid analysis
//
void FlowGraph::removeEmptyBlocks()
{
    bool changed = true;

    while (changed)
    {
        changed = false;
        for (BB_LIST_ITER it = BBs.begin(); it != BBs.end();)
        {
            G4_BB* bb = *it;

            //
            // The removal candidates will have a unique successor and a single label
            // starting with LABEL__EMPTYBB as the only instruction besides a JMP.
            //
            if (bb->instList.size() > 0 && bb->instList.size() < 3)
            {
                INST_LIST::iterator removedBlockInst = bb->instList.begin();

                if ((*removedBlockInst)->isLabel() == false ||
                    strncmp((*removedBlockInst)->getLabelStr(),
                        "LABEL__EMPTYBB", 14) != 0)
                {
                    ++it;
                    continue;
                }

                ++removedBlockInst;

                if (removedBlockInst != bb->instList.end())
                {
                    // if the BB is not empty, it must end with a unconditional jump
                    if ((*removedBlockInst)->opcode() != G4_jmpi || bb->Succs.size() > 1)
                    {
                        ++it;
                        continue;
                    }
                }

                for (auto predBB : bb->Preds)
                {
                    //
                    // Replace the predecessors successor links to the removed block's unique successor.
                    //
                    BB_LIST_ITER jt = predBB->Succs.begin();

                    for (; jt != predBB->Succs.end(); ++jt)
                    {
                        if ((*jt) == bb)
                        {
                            break;
                        }
                    }

                    for (auto succBB : bb->Succs)
                    {
                        predBB->Succs.insert(jt, succBB);
                    }
                    predBB->Succs.erase(jt);
                    predBB->Succs.unique();
                }

                for (auto succBB : bb->Succs)
                {
                    //
                    // Replace the unique successor's predecessor links with the removed block's predessors.
                    //
                    BB_LIST_ITER kt = succBB->Preds.begin();

                    for (; kt != succBB->Preds.end(); ++kt)
                    {
                        if ((*kt) == bb)
                        {
                            break;
                        }
                    }

                    for (auto predBB : bb->Preds)
                    {
                        succBB->Preds.insert(kt, predBB);
                    }

                    succBB->Preds.erase(kt);
                    succBB->Preds.unique();

                    //
                    // Propagate the removed block's type to its unique successor.
                    //
                    succBB->setBBType(bb->getBBType());
                    //
                    // Update the call graph if this is a RETURN node.
                    //
                    if (bb->BBBeforeCall())
                    {
                        bb->BBBeforeCall()->setBBAfterCall(succBB);
                        succBB->setBBBeforeCall(bb->BBBeforeCall());
                    }
                    else if (bb->BBBeforeCall())
                    {
                        //
                        // A CALL node should never be empty.
                        //
                        MUST_BE_TRUE(false, ERROR_FLOWGRAPH);
                    }
                }
                //
                // Remove the block to be removed.
                //
                bb->Succs.clear();
                bb->Preds.clear();
                bb->instList.clear();

                BB_LIST_ITER rt = it++;
                BBs.erase(rt);
                changed = true;
            }
            else
            {
                ++it;
            }
        }
    }
}

//
// If multiple freturns exist in a flowgraph create a new basic block
// with an freturn. Replace all freturns with jumps.
//
void FlowGraph::mergeFReturns()
{
    std::list<G4_BB*> exitBBs;
    G4_BB* candidateFretBB = NULL;
    G4_Label *dumLabel = NULL;

    for (BB_LIST_ITER bb_it = BBs.begin();
        bb_it != BBs.end();
        bb_it++)
    {
        G4_BB* cur = (*bb_it);

        if (cur->instList.size() > 0 && cur->instList.back()->isFReturn())
        {
            exitBBs.push_back(cur);

            if (cur->instList.size() == 2 && cur->instList.front()->isLabel())
            {
                // An fret already exists that can be shared among all
                // so skip creating a new block with fret.
                dumLabel = cur->instList.front()->getSrc(0)->asLabel();
                candidateFretBB = cur;
            }
        }
    }

    if (exitBBs.size() > 1)
    {
        if (candidateFretBB == NULL)
        {
            G4_BB* newExit = createNewBB();
            char str[128];
            if (builder->getIsKernel())
            {
                ASSERT_USER(false, "Not expecting fret in kernel");
            }
            else
            {
                SNPRINTF(str, 128, "__MERGED_FRET_EXIT_BLOCK_f%d", builder->getCUnitId());
            }
            dumLabel = builder->createLabel(str, LABEL_BLOCK);
            G4_INST* label = createNewLabelInst(dumLabel);
            newExit->instList.push_back(label);
            G4_INST* fret = builder->createInst(NULL, G4_pseudo_fret, NULL, false, 1, NULL, NULL, NULL, 0);
            newExit->instList.push_back(fret);
            BBs.push_back(newExit);
            candidateFretBB = newExit;
        }

        for (BB_LIST_ITER it = exitBBs.begin(); it != exitBBs.end(); ++it)
        {
            G4_BB* cur = (*it);

            if (cur != candidateFretBB)
            {
                G4_INST* last = cur->instList.back();
                addPredSuccEdges(cur, candidateFretBB);

                last->setOpcode(G4_jmpi);
                last->setSrc(dumLabel, 0);
                last->setExecSize(1);
            }
        }
    }
}


//
// Add a dummy BB for multiple-exit flow graph
// The criteria of a valid multiple-exit flow graph is:
//  (1). equal or less than one BB w/o successor has non-EOT last instruction;
//  (2). Other BBs w/o successor must end with EOT
//
void FlowGraph::linkDummyBB()
{
    //
    // check the flow graph to find if it satify the criteria and record the exit BB
    //
    std::list<G4_BB*> exitBBs;
    int nonEotExitBB = 0;
    for (std::list<G4_BB*>::iterator it = BBs.begin(); it != BBs.end(); ++it)
    {
        G4_BB *bb = *it;
        if (bb->Succs.empty())
        {
            exitBBs.push_back(bb);      // record exit BBs
            G4_INST* last = bb->instList.back();
            if (last == NULL)
            {
                MUST_BE_TRUE(false, "ERROR: Invalid flow graph with empty exit BB!");
            }
            if (!bb->isLastInstEOT())
            {
                nonEotExitBB++;
                if (nonEotExitBB > 1)
                {
                    MUST_BE_TRUE(false,
                        "ERROR: Invalid flow graph with more than one exit BB not end with EOT!");
                }
            }
        }
    }

    //
    // create the dummy BB and link the exit BBs to it
    //
    if (nonEotExitBB == 1 && exitBBs.size() > 1)
    {
        G4_BB *dumBB = createNewBB();
        MUST_BE_TRUE(dumBB != NULL, ERROR_FLOWGRAPH);
        char str[128];
        SNPRINTF(str, 128, "__AUTO_GENERATED_DUMMY_LAST_BB");
        G4_Label *dumLabel = builder->createLabel(str, LABEL_BLOCK);
        G4_INST* label = createNewLabelInst(dumLabel);
        dumBB->instList.push_back(label);
        BBs.push_back(dumBB);

        for (std::list<G4_BB*>::iterator it = exitBBs.begin(); it != exitBBs.end(); ++it)
        {
            G4_BB *bb = *it;
            dumBB->Preds.push_back(bb);
            bb->Succs.push_back(dumBB);
        }
    }
}

//
// Re-assign block ID so that we can use id to determine the ordering of two blocks in the code layout
//
void FlowGraph::reassignBlockIDs()
{
    //
    // re-assign block id so that we can use id to determine the ordering of
    // two blocks in the code layout; namely which one is ahead of the other.
    // Important: since we re-assign id, there MUST NOT exist any instruction
    // that depends on BB id. Or the code will be incorrect once we reassign id.
    //
    std::list<G4_BB*> function_start_list;
    unsigned int i = 0;
    for (BB_LIST_ITER it = BBs.begin(); it != BBs.end(); ++it)
    {
        G4_BB* bb = *it;
        bb->setId(i);
        if (bb->getBBListStart() != bb->getBBListEnd())
            function_start_list.push_back(bb);
        i++;
        MUST_BE_TRUE(i <= getNumBB(), ERROR_FLOWGRAPH);
    }

    /*
    re-does a mapping of ids to basic blocks, to keep them consistent.
    This function is called when basic blocks are removed.
    Later on map is accessed by block id.
    So need to re-map with correct ids.
    */
    for (BB_LIST_ITER it = function_start_list.begin(); it != function_start_list.end(); ++it)
    {
        G4_BB* bb = *it;
        std::list<G4_BB*> temp_list;
        for (std::map<int, G4_BB*>::iterator it2 = bb->getBBListStart(); it2 != bb->getBBListEnd(); it2++)
        {
            temp_list.push_back(it2->second);
        }
        bb->clearBBList();
        for (std::list<G4_BB*>::iterator it2 = temp_list.begin(); it2 != temp_list.end(); it2++)
        {
            G4_BB* bb_temp = *it2;
            bb->addToBBList(bb_temp->getId(), bb_temp);
        }
    }

    numBBId = i;
}

//
// given a label string, find its BB and the label's offset in the BB
// label_offset is the offset of label in BB, since there may be nop insterted before the label
//
G4_BB *FlowGraph::findLabelBB(char *label, int &label_offset)
{
    MUST_BE_TRUE(label, ERROR_INTERNAL_ARGUMENT);

    for (BB_LIST_ITER it = BBs.begin(); it != BBs.end(); ++it)      // to make it simple, did not use trick to speed up the search
    {
        G4_BB* bb = *it;
        G4_INST *first = bb->instList.empty() ? NULL : bb->instList.front();

        char *label_t = NULL;

        if (first && first->isLabel())
        {
            label_t = first->getLabelStr();
            label_offset = 0;
        }
        if (label_t == NULL)
            continue;

        if (strcmp(label, label_t) == 0)
            return bb;
    }

    return NULL;
}

/*
*  Mark blocks that are nested in SIMD control flow.
*  Only structured CF is handled here, SIMD BBs due to goto/join
*  are marked in processGoto()
*
*  This function also sets the JIP of the endif to its enclosing endif/while, if it exists
*  Note: we currently do not consider goto/join when adding the JIP for endif,
*  since structure analysis should not allow goto into/out of if-endif.  This means
*  we only need to set the the JIP of the endif to its immediately enclosing endif/while
*
*  The simd control flow blocks must be well-structured
*
*/
void FlowGraph::markSimdBlocks(std::map<std::string, G4_BB*>& labelMap, FuncInfoHashTable &FuncInfoMap)
{
    std::stack<StructuredCF*> ifAndLoops;

    for (BB_LIST_ITER it = BBs.begin(); it != BBs.end(); ++it)
    {
        G4_BB* bb = *it;
        for (INST_LIST_ITER it = bb->instList.begin(); it != bb->instList.end(); ++it)
        {
            G4_INST* inst = *it;
            // check if first non-label inst is an endif
            if (inst->opcode() != G4_label && inst->opcode() != G4_join)
            {
                if (inst->opcode() == G4_endif)
                {

                    MUST_BE_TRUE(ifAndLoops.size() > 0, "endif without matching if");
                    StructuredCF* cf = ifAndLoops.top();
                    MUST_BE_TRUE(cf->mType == STRUCTURED_CF_IF, "ill-formed if");
                    cf->setEnd(bb, inst);
                    ifAndLoops.pop();
                    if (ifAndLoops.size() > 0)
                    {
                        cf->enclosingCF = ifAndLoops.top();
                    }
                }
                else
                {
                    // stop at the first non-endif instruction (there may be multiple endifs)
                    break;
                }
            }
        }

        // check if bb is SIMD loop head
        for (BB_LIST_ITER preds = bb->Preds.begin(); preds != bb->Preds.end(); ++preds)
        {
            G4_BB* predBB = *preds;
            // check if one of the pred ends with a while
            if (predBB->getId() >= bb->getId())
            {
                if (predBB->instList.size() != 0 &&
                    predBB->instList.back()->opcode() == G4_while)
                {
                    StructuredCF* cf = new (mem)StructuredCF(STRUCTURED_CF_LOOP, bb);
                    structuredSimdCF.push_back(cf);
                    ifAndLoops.push(cf);
                }
            }
        }

        if (ifAndLoops.size() > 0)
        {
            bb->setInSimdFlow(true);
        }

        if (bb->instList.size() > 0)
        {
            G4_INST* lastInst = bb->instList.back();
            if (lastInst->opcode() == G4_if)
            {
                StructuredCF* cf = new (mem)StructuredCF(STRUCTURED_CF_IF, bb);
                structuredSimdCF.push_back(cf);
                ifAndLoops.push(cf);
            }
            else if (lastInst->opcode() == G4_while)
            {
                MUST_BE_TRUE(ifAndLoops.size() > 0, "while without matching do");
                StructuredCF* cf = ifAndLoops.top();
                MUST_BE_TRUE(cf->mType == STRUCTURED_CF_LOOP, "ill-formed while loop");
                cf->setEnd(bb, lastInst);
                ifAndLoops.pop();
                if (ifAndLoops.size() > 0)
                {
                    cf->enclosingCF = ifAndLoops.top();
                }
            }
        }
    }

    MUST_BE_TRUE(ifAndLoops.size() == 0, "not well-structured SIMD CF");

    for (int i = 0, size = (int)structuredSimdCF.size(); i < size; i++)
    {
        StructuredCF* cf = structuredSimdCF[i];
        if (cf->mType == STRUCTURED_CF_IF && cf->enclosingCF != NULL)
        {
            setJIPForEndif(cf->mEndInst, cf->enclosingCF->mEndInst, cf->enclosingCF->mEndBB);
        }
    }

    // Visit the call graph, and mark simd blocks in subroutines.
    std::set<FuncInfo*> Visited;
    std::queue<FuncInfo*> Funcs;

    // Starting with kernel.
    Funcs.push(kernelInfo);

    // Now process all subroutines called in a simd-cf block.
    while (!Funcs.empty())
    {
        FuncInfo* CallerInfo = Funcs.front();
        Funcs.pop();

        // Skip if this is already visited.
        if (!Visited.insert(CallerInfo).second)
            continue;

        for (auto BB : CallerInfo->getBBList())
        {
            if (BB->isInSimdFlow() && BB->isEndWithCall())
            {
                G4_INST *CI = BB->instList.back();
                if (CI->getSrc(0)->isLabel())
                {
                    G4_Label* Callee = CI->getSrc(0)->asLabel();
                    G4_BB* CalleeEntryBB = labelMap[Callee->getLabel()];
                    FuncInfo* CalleeInfo = CalleeEntryBB->getFuncInfo();
                    Funcs.push(CalleeInfo);

                    // Mark all blocks in this subroutine.
                    for (auto BB1 : CalleeInfo->getBBList())
                    {
                        BB1->setInSimdFlow(true);
                    }
                }
            }
        }
    }
}

/*
* Insert a join at the beginning of this basic block, immediately after the label
* If a join is already present, nothing will be done
*/
void FlowGraph::insertJoinToBB(G4_BB* bb, uint8_t execSize, G4_Label* jip)
{
    MUST_BE_TRUE(bb->instList.size() > 0, "empty block");
    INST_LIST_ITER iter = bb->instList.begin();

    // Skip label if any.
    if ((*iter)->isLabel())
    {
        iter++;
    }

    if (iter == bb->instList.end())
    {
        // insert join at the end
        G4_INST* jInst = builder->createInternalCFInst(NULL, G4_join, execSize, jip, NULL, InstOpt_NoOpt);
        bb->instList.push_back(jInst);
    }
    else
    {
        G4_INST* secondInst = *iter;

        if (secondInst->opcode() == G4_join)
        {
            if (execSize > secondInst->getExecSize())
            {
                secondInst->setExecSize(execSize);
            }
        }
        else
        {
            G4_INST* jInst = builder->createInternalCFInst(NULL, G4_join, execSize, jip, NULL, InstOpt_NoOpt);
            bb->instList.insert(iter, jInst);
        }
    }
}

typedef std::pair<G4_BB*, int> BlockSizePair;

static void addBBToActiveJoinList(std::list<BlockSizePair>& activeJoinBlocks, G4_BB* bb, int execSize)
{
    // add goto target to list of active blocks that need a join
    std::list<BlockSizePair>::iterator listIter;
    for (listIter = activeJoinBlocks.begin(); listIter != activeJoinBlocks.end(); ++listIter)
    {
        G4_BB* aBB = (*listIter).first;
        if (aBB->getId() == bb->getId())
        {
            // block already in list, update exec size if necessary
            if (execSize > (*listIter).second)
            {
                (*listIter).second = execSize;
            }
            break;
        }
        else if (aBB->getId() > bb->getId())
        {
            activeJoinBlocks.insert(listIter, BlockSizePair(bb, execSize));
            break;
        }
    }

    if (listIter == activeJoinBlocks.end())
    {
        activeJoinBlocks.push_back(BlockSizePair(bb, execSize));
    }
}

void FlowGraph::setPhysicalPredSucc()
{
    BB_LIST_CITER it = BBs.cbegin();
    BB_LIST_CITER cend = BBs.cend();
    if (it != cend)
    {
        // first, set up head BB
        G4_BB* pred = *it;
        pred->setPhysicalPred(NULL);

        for (++it; it != cend; ++it)
        {
            G4_BB* bb = *it;
            bb->setPhysicalPred(pred);
            pred->setPhysicalSucc(bb);
            pred = bb;
        }

        // last, set up the last BB
        pred->setPhysicalSucc(NULL);
    }
}

G4_Label* FlowGraph::insertEndif(G4_BB* bb, unsigned char execSize, bool createLabel)
{
    // endif is placed immediately after the label
    G4_INST* endifInst = builder->createInternalCFInst(NULL, G4_endif, execSize, NULL, NULL, InstOpt_NoOpt);
    INST_LIST_ITER iter = bb->instList.begin();
    MUST_BE_TRUE(iter != bb->instList.end(), "empty BB");
    iter++;
    bb->instList.insert(iter, endifInst);

    // this block may be a target of multiple ifs, in which case we will need to insert
    // one endif for each if.  The innermost endif will use the BB label, while for the other
    // endifs a new label will be created for each of them.
    if (createLabel)
    {
        char name[32];
        SNPRINTF(name, 32, "_AUTO_LABEL_%d", autoLabelId++);
        G4_Label* label = builder->createLabel(name, LABEL_BLOCK);
        endifInst->setInstLabel(label);
        return label;
    }
    else
    {
        return bb->getLabel();
    }
}

/*
*  This function set the JIP of the endif to the target instruction (either endif or while)
*
*/
void FlowGraph::setJIPForEndif(G4_INST* endif, G4_INST* target, G4_BB* targetBB)
{
    MUST_BE_TRUE(endif->opcode() == G4_endif, "must be an endif instruction");
    G4_Label* label = target->getInstLabel();
    if (label == NULL)
    {
        // see if there's another label before the inst that we can reuse
        // FIXME: we really should associate labels with inst instead of having special label inst,
        // so we can avoid ugly code like this
        G4_INST* prevInst = NULL;
        if (target->opcode() == G4_endif)
        {
            for (INST_LIST_ITER it = targetBB->instList.begin(), itEnd = targetBB->instList.end();
                it != itEnd; ++it)
            {
                G4_INST* inst = *it;
                if (inst == target)
                {
                    if (prevInst != NULL && prevInst->isLabel())
                    {
                        label = prevInst->getLabel();
                    }
                    break;
                }
                prevInst = inst;
            }
        }
        else
        {
            MUST_BE_TRUE(target->opcode() == G4_while, "must be a while instruction");
            INST_LIST_RITER it = ++(targetBB->instList.rbegin());
            if (it != targetBB->instList.rend())
            {
                G4_INST* inst = *it;
                if (inst->isLabel())
                {
                    label = inst->getLabel();
                }
            }
        }

        if (label == NULL)
        {
            char name[32];
            SNPRINTF(name, 32, "_AUTO_LABEL_%d", autoLabelId++);
            label = builder->createLabel(name, LABEL_BLOCK);
            target->setInstLabel(label);
        }
    }
    endif->asCFInst()->setJip(label);

#ifdef DEBUG_VERBOSE_ON
    cout << "set JIP for: \n";
    endif->emit(cout);
    cout << "\n";
#endif
}

/*
*  This function generates UCF (unstructurized control flow, that is, goto/join/jmpi)
*    This function inserts the join for each goto as well as compute the JIP for the
*    goto and join instructions. It additionally converts uniform (simd1 or non-predicated)
*    gotos into scalar jumps. If it's a forward goto, it may be converted into a jmpi only
*    if it is uniform and it does not overlap with another divergent goto.  All uniform
*    backward gotos may be converted into scalar jumps.
*
*  - This function does *not* alter the CFG.
*  - This function does *not* consider SCF (structured control flow), as a well-formed
*    vISA program should not have overlapped goto and structured CF instructions.
*
*/
void FlowGraph::processGoto(bool HasSIMDCF)
{
    // list of active blocks where a join needs to be inserted, sorted in lexical order
    std::list<BlockSizePair> activeJoinBlocks;
    bool doScalarJmp = !builder->noScalarJmp();

    for (BB_LIST_ITER it = BBs.begin(), itEnd = BBs.end(); it != itEnd; ++it)
    {
        G4_BB* bb = *it;
        if (bb->instList.size() == 0)
        {
            continue;
        }

        if (activeJoinBlocks.size() > 0)
        {
            if (bb == activeJoinBlocks.front().first)
            {
                // This block is the target of one or more forward goto,
                // or the fall-thru of a backward goto, needs to insert a join
                int execSize = activeJoinBlocks.front().second;
                G4_Label* joinJIP = NULL;

                activeJoinBlocks.pop_front();
                if (activeJoinBlocks.size() > 0)
                {
                    //set join JIP to the next active join
                    G4_BB* joinBlock = activeJoinBlocks.front().first;
                    joinJIP = joinBlock->getLabel();
                }

                insertJoinToBB(bb, (uint8_t)execSize, joinJIP);
            }
        }

        // check to see if this block is the target of one (or more) backward goto
        // If so, we process the backward goto and push its fall-thru block to the
        // active join list
        for (std::list<G4_BB*>::iterator iter = bb->Preds.begin(), iterEnd = bb->Preds.end(); iter != iterEnd; ++iter)
        {
            G4_BB* predBB = *iter;
            G4_INST *lastInst = predBB->instList.back();
            if (lastInst->opcode() == G4_goto && lastInst->asCFInst()->isBackward() &&
                lastInst->asCFInst()->getUip() == bb->getLabel())
            {
                // backward goto
                bool isUniform = lastInst->getExecSize() == 1 || lastInst->getPredicate() == NULL;
                if (isUniform && doScalarJmp)
                {
                    // can always convert a uniform backward goto into a jmp
                    convertGotoToJmpi(lastInst);

                    // No need to do the following for scalar jump. If there are gotos inside loop,
                    // the join point will be updated when handling gotos.

                    // we still have to add a join point at the BB immediately after the back edge,
                    // since there may be subsequent loop breaks that are waiting there.
                    // example:
                    // L1:
                    // (P1) goto exit
                    // (P2) goto L2
                    // goto L1
                    // L2:
                    // ...
                    // exit:
                    //
                    // In this case the goto exit's JIP should be set to L2 as there may be channels
                    // waiting there due to "goto L2"
                    G4_BB* loopExitBB = predBB->getPhysicalSucc();
                    // loop exit may be null if the loop is the outer-most one
                    // (i.e., the loop has no breaks but only EOT sends)
                    if (loopExitBB != NULL)
                    {
                        addBBToActiveJoinList(activeJoinBlocks, loopExitBB, lastInst->getExecSize());
                    }

                }
                else
                {
                    uint8_t eSize = lastInst->getExecSize() > 1 ? lastInst->getExecSize() : pKernel->getSimdSize();
                    if (lastInst->getExecSize() == 1)
                    {   // For simd1 goto, convert it to a goto with the right execSize.
                        lastInst->setExecSize(eSize);
                        // This should have noMask removed if any
                        lastInst->setOptions(InstOpt_M0);
                    }
                    // add join to the fall-thru BB
                    if (G4_BB* fallThruBB = predBB->getPhysicalSucc())
                    {
                        addBBToActiveJoinList(activeJoinBlocks, fallThruBB, eSize);
                        lastInst->asCFInst()->setJip(fallThruBB->getLabel());
                    }
                }
            }
        }

        // at this point if there are active join blocks, we are in SIMD control flow
        // FIXME: This is over pessimistic for kernels with actual simd cf.
        if (HasSIMDCF && !activeJoinBlocks.empty())
        {
            bb->setInSimdFlow(true);
        }

        G4_INST* lastInst = bb->instList.back();
        if (lastInst->opcode() == G4_goto && !lastInst->asCFInst()->isBackward())
        {
            // forward goto
            // the last Succ BB is our goto target
            G4_BB* gotoTargetBB = bb->Succs.back();
            bool isUniform = lastInst->getExecSize() == 1 || lastInst->getPredicate() == NULL;

            if (isUniform && doScalarJmp &&
                (activeJoinBlocks.size() == 0 || activeJoinBlocks.front().first->getId() > gotoTargetBB->getId()))
            {
                // can convert goto into a scalar jump to UIP, if the jmp will not make us skip any joins
                // CFG itself does not need to be updated
                convertGotoToJmpi(lastInst);
            }
            else
            {
                //set goto JIP to the first active block
                uint8_t eSize = lastInst->getExecSize() > 1 ? lastInst->getExecSize() : pKernel->getSimdSize();
                addBBToActiveJoinList(activeJoinBlocks, gotoTargetBB, eSize);
                G4_BB* joinBlock = activeJoinBlocks.front().first;
                if (lastInst->getExecSize() == 1)
                {   // For simd1 goto, convert it to a goto with the right execSize.
                    lastInst->setExecSize(eSize);
                    lastInst->setOptions(InstOpt_M0);
                }
                lastInst->asCFInst()->setJip(joinBlock->getLabel());

                if (!builder->gotoJumpOnTrue())
                {
                    // For BDW/SKL goto, the false channels are the ones that actually will take the jump,
                    // and we thus have to flip the predicate
                    G4_Predicate *pred = lastInst->getPredicate();
                    if (pred != NULL)
                    {
                        pred->setState(pred->getState() == PredState_Plus ? PredState_Minus : PredState_Plus);
                    }
                    else
                    {
                        // if there is no predicate, generate a predicate with all 0s.
                        // if predicate is SIMD32, we have to use a :ud dst type for the move
                        uint8_t execSize = lastInst->getExecSize() > 16 ? 2 : 1;
                        G4_Declare* tmpFlagDcl = builder->createTempFlag(execSize);
                        G4_DstRegRegion* newPredDef = builder->createDstRegRegion(Direct, tmpFlagDcl->getRegVar(), 0, 0, 1, execSize == 2 ? Type_UD : Type_UW);
                        G4_INST *predInst = builder->createInternalInst(NULL, G4_mov, NULL, false, 1,
                            newPredDef, builder->createImm(0, Type_UW), NULL,
                            InstOpt_WriteEnable, lastInst->getLineNo(), lastInst->getCISAOff(), lastInst->getSrcFilename());
                        INST_LIST_ITER iter = bb->instList.end();
                        iter--;
                        bb->instList.insert(iter, predInst);

                        pred = builder->createPredicate(
                            PredState_Plus,
                            tmpFlagDcl->getRegVar(),
                            0);
                        lastInst->setPredicate(pred);
                    }
                }
            }
        }
    }
}

//
// Evaluate AddrExp/AddrExpList to Imm
//
void G4_Kernel::evalAddrExp()
{
    for (std::list<G4_BB*>::iterator it = fg.BBs.begin(); it != fg.BBs.end(); ++it)
    {
        G4_BB* bb = (*it);

        for (INST_LIST_ITER i = bb->instList.begin(); i != bb->instList.end(); i++)
        {
            G4_INST* inst = (*i);
            //std::ostringstream os;
            //inst->emit(os);
            //
            // process each source operand
            //
            for (unsigned j = 0; j < G4_MAX_SRCS; j++)
            {
                G4_Operand* opnd = inst->getSrc(j);

                if (opnd == NULL) continue;

                if (opnd->isAddrExp())
                {
                    int val = opnd->asAddrExp()->eval();
                    G4_Type ty = opnd->asAddrExp()->getType();

                    G4_Imm* imm = fg.builder->createImm(val, ty);
                    inst->setSrc(imm, j);
                }
            }
        }
    }
}

//
// Add declares for the stack and frame pointers.
//
void FlowGraph::addFrameSetupDeclares(IR_Builder& builder, PhyRegPool& regPool)
{
    if (framePtrDcl == NULL)
    {
        framePtrDcl = builder.getBEFP();
    }
    if (stackPtrDcl == NULL)
    {
        stackPtrDcl = builder.getBESP();
    }
    if (scratchRegDcl == NULL)
    {
        scratchRegDcl = builder.createDeclareNoLookup("SR", G4_GRF, 8, 2, Type_UD);
        scratchRegDcl->getRegVar()->setPhyReg(regPool.getGreg(builder.kernel.getStackCallStartReg() + 1), 0);
    }
}

static inline void trackVarReferenceFilescopeVars(G4_RegVar* var, DECLARE_LIST& refVars, BitSet& visited)
{
    G4_Declare* dcl = var->getDeclare();
    if (visited.isSet(dcl->getDeclId()) == false)
    {
        visited.set(dcl->getDeclId(), true);
        G4_Declare* aliasDcl = dcl->getAliasDeclare();
        if (aliasDcl && aliasDcl->getAliasDeclare() == NULL && aliasDcl->getHasFileScope())
        {
            refVars.push_back(dcl);
        }
        else if (aliasDcl)
        {
            trackVarReferenceFilescopeVars(aliasDcl->getRegVar(), refVars, visited);
        }
    }
}

static void trackOpndReferenceFilescopeVars(G4_Operand* opnd, DECLARE_LIST& refVars, BitSet& visited)
{
    if (opnd != NULL)
    {
        if (opnd->isSrcRegRegion())
        {
            G4_VarBase* base = opnd->asSrcRegRegion()->getBase();
            if (base->isRegVar() && base->asRegVar()->getDeclare())
            {
                trackVarReferenceFilescopeVars(base->asRegVar(), refVars, visited);
            }
        }
        else if (opnd->isDstRegRegion())
        {
            G4_VarBase* base = opnd->asDstRegRegion()->getBase();
            if (base->isRegVar() && base->asRegVar()->getDeclare())
            {
                trackVarReferenceFilescopeVars(base->asRegVar(), refVars, visited);
            }
        }
        else if (opnd->isAddrExp())
        {
            trackVarReferenceFilescopeVars(((G4_AddrExp*)opnd)->getRegVar(), refVars, visited);
        }
    }
}

void FlowGraph::trackCutReferenceFilescopeVars(BB_LIST& graphCutBBs, DECLARE_LIST& refVars, unsigned numDcls)
{
    BitSet visited(numDcls, false);

    for (BB_LIST_ITER bt = graphCutBBs.begin(); bt != graphCutBBs.end(); ++bt)
    {
        for (INST_LIST_ITER it = (*bt)->instList.begin(); it != (*bt)->instList.end(); ++it)
        {
            for (unsigned j = 0; j < G4_MAX_SRCS; j++)
            {
                trackOpndReferenceFilescopeVars((*it)->getSrc(j), refVars, visited);
            }
            trackOpndReferenceFilescopeVars((*it)->getDst(), refVars, visited);
        }
    }
}

//
// Perform the layout for filescoped variables in the GENX_MAIN frame.
// Also create unique versions of filescoped variabled per function.
//
void FlowGraph::doFilescopeVarLayout(IR_Builder& builder, DECLARE_LIST& declares,
    unsigned& fileScopeAreaSize)
{
    // Assign OWORD aligned frame offsets for all file scope variables.

    DECLARE_LIST fileScopeDclRoots;
    DECLARE_LIST filescopeVars;
    unsigned owordSize = 8 * sizeof(short);
    // fileScopeAreaSize is in units of oword
    fileScopeAreaSize = 0;

#define ROUND(x,y)  ((x) + ((y - x % y) % y))

    for (DECLARE_LIST_ITER di = declares.begin(); di != declares.end(); ++di)
    {
        if ((*di)->getHasFileScope())
        {
            if ((*di)->getAliasDeclare() == NULL)
            {
                unsigned int spillMemOffset = builder.getOptions()->getuInt32Option(vISA_SpillMemOffset);
                (*di)->getRegVar()->setDisp(
                    fileScopeAreaSize * owordSize + spillMemOffset);
                unsigned size = (*di)->getElemSize() * (*di)->getNumElems() * (*di)->getNumRows();
                size += (owordSize - size % owordSize) % owordSize;
                fileScopeAreaSize += size / owordSize;
                fileScopeDclRoots.push_back(*di);
            }
        }
        //(*di)->setId(numDcls++);
    }

    // Round the filescope area size to GRF size so that the spill area
    // starts on a GRF boundary.
    fileScopeAreaSize = ROUND(fileScopeAreaSize, 2);
}

//
// Insert pseudo dcls to represent the caller-save and callee-save registers.
// This is only required when there is more than one graph cut due to the presence
// of function calls using a stack calling convention.
//
void FlowGraph::addSaveRestorePseudoDeclares(IR_Builder& builder)
{
    //
    // VCA_SAVE (r1.0-r60.0) [r0 is reserved] - one required per stack call,
    // but will be reused across cuts.
    //
    INST_LIST callSites;
    for (auto bb : builder.kernel.fg.BBs)
    {
        if (bb->isEndWithFCall())
        {
            callSites.push_back(bb->instList.back());
        }
    }
    if (callSites.size() <= pseudoVCADclList.size())
    {
        std::vector<G4_Declare*>::iterator it = pseudoVCADclList.begin();
        for (auto callsite : callSites)
        {
            (*it)->getRegVar()->setPhyReg(NULL, 0);
            callsite->asCFInst()->setAssocPseudoVCA((*it)->getRegVar());
            ++it;
        }
    }
    else
    {
        INST_LIST_ITER it = callSites.begin();
        for (auto pseudoVCADcl : pseudoVCADclList)
        {
            MUST_BE_TRUE(it != callSites.end(), "incorrect call sites");
            pseudoVCADcl->getRegVar()->setPhyReg(NULL, 0);
            (*it)->asCFInst()->setAssocPseudoVCA(pseudoVCADcl->getRegVar());
            ++it;
        }
        for (unsigned id = (unsigned)pseudoVCADclList.size(); it != callSites.end(); ++it, ++id)
        {
            const char* nameBase = "VCA_SAVE";
            const int maxIdLen = 3;
            MUST_BE_TRUE(id < 1000, ERROR_FLOWGRAPH);
            char *name = builder.getNameString(mem, strlen(nameBase) + maxIdLen + 1, "%s_%d", nameBase, id);
            pseudoVCADclList.push_back(builder.createDeclareNoLookup(name, G4_GRF, 8, 59, Type_UD));
            (*it)->asCFInst()->setAssocPseudoVCA(pseudoVCADclList.back()->getRegVar());
        }
    }
    //
    // VCE_SAVE (r60.0-r125.0) [r125-127 is reserved]
    //
    if (pseudoVCEDcl == NULL)
    {
        unsigned int numRowsVCE = getKernel()->getNumCalleeSaveRegs();
        pseudoVCEDcl = builder.createDeclareNoLookup("VCE_SAVE", G4_GRF, 8, static_cast<unsigned short>(numRowsVCE), Type_UD);
    }
    else
    {
        pseudoVCEDcl->getRegVar()->setPhyReg(NULL, 0);
    }

    //
    // Insert caller save decls for A0
    //
    unsigned int i = 0;
    for (auto callSite : callSites)
    {
        char* name = builder.getNameString(mem, 50, builder.getIsKernel() ? "k%d_SA0_%d" : "f%d_SA0_%d", builder.getCUnitId(), i);
        G4_Declare* saveA0 = builder.createDeclareNoLookup(name, G4_ADDRESS, (unsigned short)getNumAddrRegisters(), 1, Type_UW);
        pseudoA0DclList.push_back(saveA0);
        callSite->asCFInst()->setAssocPseudoA0Save(saveA0->getRegVar());
        i++;
    }

    //
    // Insert caller save decls for flag
    //
    unsigned int j = 0;
    for (auto callSite : callSites)
    {
        char *name = builder.getNameString(mem, 64, builder.getIsKernel() ? "k%d_SFLAG_%d" : "f%d_SFLAG_%d", builder.getCUnitId(), j);
        G4_Declare* saveFLAG = builder.createDeclareNoLookup(name, G4_FLAG, (unsigned short)getNumFlagRegisters(), 1, Type_UW);
        pseudoFlagDclList.push_back(saveFLAG);
        callSite->asCFInst()->setAssocPseudoFlagSave(saveFLAG->getRegVar());
        j++;
    }

}

//
// Since we don't do SIMD augmentation in RA for CM, we have to add an edge
// between the then and else block of an if branch to ensure that liveness is
// computed correctly, if conservatively. This also applies to any goto BB and
// its JIP join block
//
void FlowGraph::addSIMDEdges()
{
    std::map<G4_Label*, G4_BB*> joinBBMap;
    for (auto bb : BBs)
    {
        if (bb->instList.size() > 0 && bb->instList.back()->opcode() == G4_else)
        {
            addUniquePredSuccEdges(bb, bb->getPhysicalSucc());
        }
        else
        {
            // check goto case
            auto instIter = std::find_if(bb->instList.begin(), bb->instList.end(),
                [](G4_INST* inst) { return !inst->isLabel(); });
            if (instIter != bb->instList.end() && (*instIter)->opcode() == G4_join)
            {
                G4_INST* firstInst = bb->instList.front();
                if (firstInst->isLabel())
                {
                    joinBBMap[firstInst->getLabel()] = bb;
                }
            }
        }
    }

    if (!joinBBMap.empty())
    {
        for (auto bb : BBs)
        {
            if (bb->isEndWithGoto())
            {
                G4_INST* gotoInst = bb->instList.back();
                auto iter = joinBBMap.find(gotoInst->asCFInst()->getJip()->asLabel());
                if (iter != joinBBMap.end())
                {
                    addUniquePredSuccEdges(bb, iter->second);
                }
            }
        }
    }
}

// Dump the instructions into a file
void G4_Kernel::dumpPassInternal(const char* appendix)
{
    MUST_BE_TRUE(appendix != NULL, ERROR_INTERNAL_ARGUMENT);
    if (!m_options->getOption(vISA_DumpPasses))  // skip dumping dot files
        return;

    char fileName[256];
    MUST_BE_TRUE(strlen(appendix) < 40, ERROR_INVALID_VISA_NAME(appendix));
    if (name != NULL)
    {
        MUST_BE_TRUE(strlen(name) < 206, ERROR_INVALID_VISA_NAME(name));
        SNPRINTF(fileName, sizeof(fileName), "%s.%03d.%s.dump", name, dotDumpCount++, appendix);
    }
    else
    {
        SNPRINTF(fileName, sizeof(fileName), "%s.%03d.%s.dump", "UnknownKernel", dotDumpCount++, appendix);
    }

    std::string fname(fileName);
    fname = sanitizeString(fname);

    fstream ofile(fname, ios::out);
    if (!ofile)
    {
        MUST_BE_TRUE(false, ERROR_FILE_READ(fileName));
    }

    const char* asmFileName = NULL;
    m_options->getOption(VISA_AsmFileName, asmFileName);
    if (asmFileName == NULL)
        ofile << "UnknownKernel" << std::endl << std::endl;
    else
        ofile << asmFileName << std::endl << std::endl;

    for (std::list<G4_BB*>::iterator it = fg.BBs.begin();
        it != fg.BBs.end(); ++it)
    {
        // Emit BB number
        G4_BB* bb = (*it);
        bb->writeBBId(ofile);
        ofile << "\tPreds: ";
        for (auto pred : bb->Preds)
        {
            pred->writeBBId(ofile);
            ofile << " ";
        }
        ofile << "\tSuccs: ";
        for (auto succ : bb->Succs)
        {
            succ->writeBBId(ofile);
            ofile << " ";
        }
        ofile << "\n";

        bb->emit(ofile);
        ofile << "\n\n";
    } // bbs

    ofile.close();
}

//
// This routine dumps out the dot file of the control flow graph along with instructions.
// dot is drawing graph tool from AT&T.
//
void G4_Kernel::dumpDotFileInternal(const char* appendix)
{
    MUST_BE_TRUE(appendix != NULL, ERROR_INTERNAL_ARGUMENT);
    if (!m_options->getOption(vISA_DumpDot))  // skip dumping dot files
        return;

    //
    // open the dot file
    //
    char fileName[256];
    MUST_BE_TRUE(strlen(appendix) < 40, ERROR_INVALID_VISA_NAME(appendix));
    if (name != NULL)
    {
        MUST_BE_TRUE(strlen(name) < 206, ERROR_INVALID_VISA_NAME(name));
        SNPRINTF(fileName, sizeof(fileName), "%s.%03d.%s.dot", name, dotDumpCount++, appendix);
    }
    else
    {
        SNPRINTF(fileName, sizeof(fileName), "%s.%03d.%s.dot", "UnknownKernel", dotDumpCount++, appendix);
    }

    std::string fname(fileName);
    fname = sanitizeString(fname);

    fstream ofile(fname, ios::out);
    if (!ofile)
    {
        MUST_BE_TRUE(false, ERROR_FILE_READ(fname));
    }
    //
    // write digraph KernelName {"
    //          size = "8, 10";
    //
    const char* asmFileName = NULL;
    m_options->getOption(VISA_AsmFileName, asmFileName);
    if (asmFileName == NULL)
        ofile << "digraph UnknownKernel" << " {" << std::endl;
    else
        ofile << "digraph " << asmFileName << " {" << std::endl;
    //
    // keep the graph width 8, estimate a reasonable graph height
    //
    const unsigned itemPerPage = 64;                                        // 60 instructions per Letter page
    unsigned totalItem = (unsigned)Declares.size();
    for (std::list<G4_BB*>::iterator it = fg.BBs.begin(); it != fg.BBs.end(); ++it)
        totalItem += ((unsigned int)(*it)->instList.size());
    totalItem += (unsigned)fg.BBs.size();
    float graphHeight = (float)totalItem / itemPerPage;
    graphHeight = graphHeight < 100.0f ? 100.0f : graphHeight;    // minimal size: Letter
    ofile << endl << "\t// Setup" << endl;
    ofile << "\tsize = \"80.0, " << graphHeight << "\";\n";
    ofile << "\tpage= \"80.5, 110\";\n";
    ofile << "\tpagedir=\"TL\";\n";
    //
    // dump out declare information
    //     Declare [label="
    //
    //if (name == NULL)
    //  ofile << "\tDeclares [shape=record, label=\"{kernel:UnknownKernel" << " | ";
    //else
    //  ofile << "\tDeclares [shape=record, label=\"{kernel:" << name << " | ";
    //for (std::list<G4_Declare*>::iterator it = Declares.begin(); it != Declares.end(); ++it)
    //{
    //  (*it)->emit(ofile, true, Options::symbolReg);   // Solve the DumpDot error on representing <>
    //
    //  ofile << "\\l";  // left adjusted
    //}
    //ofile << "}\"];" << std::endl;
    //
    // dump out flow graph
    //
    for (std::list<G4_BB*>::iterator it = fg.BBs.begin(); it != fg.BBs.end(); ++it)
    {
        G4_BB* bb = (*it);
        //
        // write:   BB0 [shape=plaintext, label=<
        //                      <TABLE BORDER="0" CELLBORDER="1" CELLSPACING="0">
        //                          <TR><TD ALIGN="CENTER">BB0: TestRA_Dot</TD></TR>
        //                          <TR><TD>
        //                              <TABLE BORDER="0" CELLBORDER="0" CELLSPACING="0">
        //                                  <TR><TD ALIGN="LEFT">TestRA_Dot:</TD></TR>
        //                                  <TR><TD ALIGN="LEFT"><FONT color="red">add (8) Region(0,0)[1] Region(0,0)[8;8,1] PAYLOAD(0,0)[8;8,1] [NoMask]</FONT></TD></TR>
        //                              </TABLE>
        //                          </TD></TR>
        //                      </TABLE>>];
        // print out label if the first inst is a label inst
        //
        ofile << "\t";
        bb->writeBBId(ofile);
        ofile << " [shape=plaintext, label=<" << std::endl;
        ofile << "\t\t\t    <TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\">" << std::endl;
        ofile << "\t\t\t\t<TR><TD ALIGN=\"CENTER\">";
        bb->writeBBId(ofile);
        ofile << ": ";

        if (!bb->instList.empty() && bb->instList.front()->isLabel())
        {
            bb->instList.front()->getSrc(0)->emit(ofile);
        }
        ofile << "</TD></TR>" << std::endl;
        //emit all instructions within basic block
        ofile << "\t\t\t\t<TR><TD>" << std::endl;

        if (!bb->instList.empty())
        {
            ofile << "\t\t\t\t\t    <TABLE BORDER=\"0\" CELLBORDER=\"0\" CELLSPACING=\"0\">" << std::endl;
            for (INST_LIST_ITER i = bb->instList.begin(); i != bb->instList.end(); i++)
            {
                //
                // detect if there is spill code first, set different color for it
                //
                std::string fontColor = "black";
                //
                // emit the instruction
                //
                ofile << "\t\t\t\t\t\t<TR><TD ALIGN=\"LEFT\"><FONT color=\"" << fontColor << "\">";
                std::ostringstream os;
                (*i)->emit(os, m_options->getOption(vISA_SymbolReg), true);
                std::string dotStr(os.str());
                //TODO: dot doesn't like '<', '>', '{', or '}' (and '&') this code below is a hack. need to replace with delimiters.
                std::replace_if(dotStr.begin(), dotStr.end(), bind2nd(equal_to<char>(), '<'), '[');
                std::replace_if(dotStr.begin(), dotStr.end(), bind2nd(equal_to<char>(), '>'), ']');
                std::replace_if(dotStr.begin(), dotStr.end(), bind2nd(equal_to<char>(), '{'), '[');
                std::replace_if(dotStr.begin(), dotStr.end(), bind2nd(equal_to<char>(), '}'), ']');
                std::replace_if(dotStr.begin(), dotStr.end(), bind2nd(equal_to<char>(), '&'), '$');
                ofile << dotStr;

                ofile << " %" << (*i)->getId();
                ofile << "</FONT></TD></TR>" << std::endl;
                //ofile << "\\l"; // left adjusted
            }
            ofile << "\t\t\t\t\t    </TABLE>" << std::endl;
        }

        ofile << "\t\t\t\t</TD></TR>" << std::endl;
        ofile << "\t\t\t    </TABLE>>];" << std::endl;
        //
        // dump out succ edges
        // BB12 -> BB10
        //
        for (std::list<G4_BB*>::iterator sit = bb->Succs.begin(); sit != bb->Succs.end(); ++sit)
        {
            bb->writeBBId(ofile);
            ofile << " -> ";
            (*sit)->writeBBId(ofile);
            ofile << std::endl;
        }
    }
    //
    // write "}" to end digraph
    //
    ofile << std::endl << " }" << std::endl;
    //
    // close dot file
    //
    ofile.close();
}

// Wrapper function
void G4_Kernel::dumpDotFile(const char* appendix)
{
    if (m_options->getOption(vISA_DumpDot))
        dumpDotFileInternal(appendix);
    if (m_options->getOption(vISA_DumpPasses))
        dumpPassInternal(appendix);
}

static const char* const RATypeString[] =
{
    RA_TYPE(STRINGIFY)
};

static iga_gen_t getIGAPlatform()
{
    iga_gen_t platform = IGA_GEN_INVALID;
    switch (getGenxPlatform())
    {
    case GENX_BDW:
        platform = IGA_GEN8;
        break;
    case GENX_CHV:
        platform = IGA_GEN8lp;
        break;
    case GENX_SKL:
        platform = IGA_GEN9;
        break;
    case GENX_BXT:
        platform = IGA_GEN9lp;
        break;
    case GENX_CNL:
        platform = IGA_GEN10;
        break;
    default:
        break;
    }

    return platform;
}

vector<string>
split(const string & str, const char * delimiter) {
    vector<string> v;
    string::size_type start = 0;

    for (auto pos = str.find_first_of(delimiter, start); pos != string::npos; start = pos + 1, pos = str.find_first_of(delimiter, start))
    {
        if (pos != start)
        {
            v.emplace_back(str, start, pos - start);
        }


    }

    if (start < str.length())
        v.emplace_back(str, start, str.length() - start);
    return v;
}
#ifdef DEBUG_VERBOSE_ON
static int noBankCount = 0;
#endif
void G4_Kernel::emit_asm(std::ostream& output, bool beforeRegAlloc, void * binary, uint32_t binarySize)
{
    //
    // for GTGPU lib release, don't dump out asm
    //
#ifdef NDEBUG
#ifdef GTGPU_LIB
    return;
#endif
#endif
    bool newAsm = false;
    if (m_options->getOption(vISA_dumpNewSyntax) && !(binary == NULL || binarySize == 0))
    {
        newAsm = true;
    }

    if (!m_options->getOption(vISA_StripComments))
    {
        output << "//.kernel ";
        if (name != NULL)
        {
            // some 3D kernels do not have name
            output << name;
        }

        output << "\n" << "//.platform " << platformString[getGenxPlatform()];
        output << "\n" << "//.stepping " << GetSteppingString();
        output << "\n" << "//.CISA version " << (unsigned int)major_version
            << "." << (unsigned int)minor_version;
        output << "\n" << "//.options " << m_options->getArgString().str();
        output << "\n" << "//.instCount " << asmInstCount;
        output << "\n//.RA type\t" << RATypeString[RAType];

        if (auto jitInfo = fg.builder->getJitInfo())
        {
            if (jitInfo->numGRFUsed != 0)
            {
                output << "\n" << "//.GRF count " << jitInfo->numGRFUsed;
            }
            if (jitInfo->spillMemUsed > 0)
            {
                output << "\n" << "//.spill size " << jitInfo->spillMemUsed;
            }
            if (jitInfo->numGRFSpillFill > 0)
            {
                output << "\n" << "//.spill GRF ref count " << jitInfo->numGRFSpillFill;
            }
            if (jitInfo->numFlagSpillStore > 0)
            {
                output << "\n//.spill flag store " << jitInfo->numFlagSpillStore;
                output << "\n//.spill flag load " << jitInfo->numFlagSpillLoad;
            }
        }

        output << "\n\n";

        //Step2: emit declares (as needed)
        //
        // firstly, emit RA declare as comments or code depends on Options::symbolReg
        // we check if the register allocation is successful here
        //

        for (auto dcl : Declares)
        {
            if (!dcl->getRegVar()->isPhyRegAssigned() && !dcl->isSpilled() &&
                !fg.isPseudoDcl(dcl) && !dcl->getIsScallDcl() &&
                !dcl->getIsPartialDcl())
            {
                if (beforeRegAlloc == false)
                {
                    if (!dcl->getRegVar()->isPhyRegAssigned() && !dcl->isSpilled())
                    {
                        MUST_BE_TRUE(false, "ERROR: Fail to allocate physical register for variable <"
                            << dcl->getName() << ">");
                    }
                }
            }
            dcl->emit(output, false, m_options->getOption(vISA_SymbolReg));
            output << "\n";
        }

        // emit input location and size
        output << "//.kernel_reordering_info_start" << std::endl;
        output << "//id\tbyte_offset\tbyte_size\tkind\timplicit_kind" << std::endl;

        unsigned int inputCount = fg.builder->getInputCount();
        for (unsigned int id = 0; id < inputCount; id++)
        {
            input_info_t* input_info = fg.builder->getInputArg(id);
            output << "//.arg_" << (id + 1) << "\t" << input_info->offset
                << "\t" << input_info->size << "\t"
                << (int)input_info->getInputClass() << "\t"
                << (int)input_info->getImplicitKind() << std::endl;
        }

        output << "//.kernel_reordering_info_end" << std::endl;
        fg.BCStats.clear();
    }


    // Set this to NULL to always print filename for each kernel
    prevFilename = NULL;

    if (!newAsm)
    {
        //Step3: emit code and subroutines
        output << std::endl << ".code";
    }

    uint32_t BCNum = 0;
    uint32_t simd8Num = 0;

    if (newAsm)
    {
        char stringBuffer[512];
        uint32_t pc = 0;
        output << std::endl;
        bool dissasemblyFailed = false;
#define ERROR_STRING_MAX_LENGTH 65536
        char errBuf[ERROR_STRING_MAX_LENGTH];

        KernelView kView(getIGAPlatform(), binary, binarySize, errBuf, ERROR_STRING_MAX_LENGTH);
        dissasemblyFailed = !kView.decodeSucceeded();

        std::string igaErrMsgs;
        std::vector<std::string> igaErrMsgsVector;
        std::map<int, std::string> errorToStringMap;
        if (dissasemblyFailed)
        {
            std::cerr << "Failed to decode binary for asm output. Please report the issue and try disabling IGA disassembler for now." << std::endl;
            igaErrMsgs = std::string(errBuf);
            igaErrMsgsVector = split(igaErrMsgs, "\n");
            for (auto msg : igaErrMsgsVector)
            {
                auto pos = msg.find("ERROR");
                if (pos != string::npos)
                {
                    std::cerr << msg.c_str() << std::endl;
                    std::vector<string> aString = split(msg, " ");
                    for (auto token : aString)
                    {
                        if (token.find_first_of("0123456789") != string::npos)
                        {
                            int errorPC = std::atoi(token.c_str());
                            errorToStringMap[errorPC] = msg;
                            break;
                        }
                    }
                }
            }
        }

        //
        // For label, generate a label with uniqueLabel as prefix (required by some tools).
        // We do so by using labeler callback.  If uniqueLabels is not present, use iga's
        // default label.  For example,
        //   Without option -uniqueLabels:
        //      generating default label,   L1234
        //   With option -uniqueLabels <sth>:
        //      generating label with <sth> as prefix, <sth>_L1234
        //
        const char* labelPrefix = nullptr;
        if (m_options->getOption(vISA_UniqueLabels))
        {
            m_options->getOption(vISA_LabelStr, labelPrefix);
        }
        typedef struct {
            char m_labelString[64]; // label string for uniqueLabels
            char* m_labelPrefix;    // label prefix
            char m_tmpString[64];   // tmp storage, default label
            KernelView *m_pkView;   // handle to KernelView object.
        } lambdaArg_t;
        lambdaArg_t lambdaArg;
        lambdaArg.m_labelPrefix = const_cast<char*>(labelPrefix);
        lambdaArg.m_pkView = &kView;

        // Labeler callback function.
        auto labelerLambda = [](int32_t pc, void *data) -> const char*
        {
            lambdaArg_t *pArg = (lambdaArg_t *)data;
            char* tmpString = pArg->m_tmpString;
            char* labelString = pArg->m_labelString;

            pArg->m_pkView->getDefaultLabelName(pc, tmpString, 64);
            const char *retString;
            if (pArg->m_labelPrefix != nullptr)
            {
                SNPRINTF(labelString, 60, "%s_%s", (const char*)pArg->m_labelPrefix, tmpString);
                retString = labelString;
            }
            else
            {
                retString = tmpString;
            }
            return retString;
        };

        uint32_t lastLabelPC = 0;
        for (BB_LIST_ITER itBB = fg.BBs.begin(); itBB != fg.BBs.end(); ++itBB)
        {
            G4_INST *prevInst = nullptr;
            for (INST_LIST_ITER itInst = (*itBB)->instList.begin(); itInst != (*itBB)->instList.end(); ++itInst)
            {

                bool isInstTarget = kView.isInstTarget(pc);
                if (isInstTarget)
                {
                    const char* stringLabel = labelerLambda(pc, (void *)&lambdaArg);

                    if ((*itInst)->isLabel())
                    {
                        output << "\n\n//" << (*itInst)->getLabelStr() << ":";
                        //handling the case where there is an empty block with just label.
                        //this way we don't print IGA label twice
                        if ((*itBB)->instList.size() == 1)
                        {
                            break;
                        }
                    }

                    //preventing the case where there are two labels in G4 IR so duplicate IGA labels are printed
                    //then parser asserts.
                    /*
                        //label_cf_20_particle:
                        L3152:

                        //label12_particle:
                        L3152:

                        endif (32|M0)                        L3168                            // [3152]: #218 //:$239:%332
                        */
                    if (pc != lastLabelPC || pc == 0)
                    {
                        output << "\n" << stringLabel << ":" << std::endl;
                        lastLabelPC = pc;
                    }

                    if ((*itInst)->isLabel())
                    {
                        ++itInst;
                        //G4_IR has instruction for label.
                        if (itInst == (*itBB)->instList.end())
                        {
                            break;
                        }
                    }
                }
                else if ((*itInst)->isLabel())
                {
                    output << "\n\n//" << (*itInst)->getLabelStr() << ":";
                    continue;
                }

                (*itBB)->emitInstructionInfo(output, itInst);
                output << std::endl;

                auto errString = errorToStringMap.find(pc);
                if (errString != errorToStringMap.end())
                {
                    output << "// " << errString->second.c_str() << std::endl;
                    output << "// Text representation might not be correct" << std::endl;
                }

                kView.getInstSyntax(pc, stringBuffer, 512, labelerLambda, (void*)&lambdaArg);
                pc += kView.getInstSize(pc);
                (*itBB)->emitBasicInstructionIga(stringBuffer, output, itInst, prevInst, BCNum, simd8Num);
                prevInst = (*itInst);
            }
        }
    }
    else
    {
        for (BB_LIST_ITER it = fg.BBs.begin(); it != fg.BBs.end(); ++it)
        {
            output << std::endl;
            (*it)->emit(output);

        }
    }
#ifdef DEBUG_VERBOSE_ON
    printf("noBankCount: %d\n", noBankCount);
#endif
    if (!newAsm)
    {
        //Step4: emit clean-up.
        output << std::endl;
        output << ".end_code" << std::endl;
        output << ".end_kernel" << std::endl;
        output << std::endl;
    }
    if (newAsm)
    {
        output << "\n\n//.BankConflicts: " << BCNum << "\n";
        output << "//.SIMD8s: " << simd8Num << "\n//\n";
        output << "// Bank Conflict Statistics: \n";
        output << "// -- GOOD: " << fg.BCStats.NumOfGoodInsts << "\n";
        output << "// --  BAD: " << fg.BCStats.NumOfBadInsts << "\n";
        output << "// --   OK: " << fg.BCStats.NumOfOKInsts << "\n";
    }
}

//
//  Add an EOT send to the end of this BB.
//
void G4_BB::addEOTSend(G4_INST* lastInst)
{
    // mov (8) r1.0<1>:ud r0.0<8;8,1>:ud {NoMask}
    // send (8) null r1 0x27 desc
    IR_Builder* builder = parent->builder;
    G4_Declare *dcl = builder->Create_MRF_Dcl(8, Type_UD);
    G4_DstRegRegion* movDst = builder->Create_Dst_Opnd_From_Dcl(dcl, 1);
    G4_SrcRegRegion* r0Src = builder->Create_Src_Opnd_From_Dcl(
        builder->getBuiltinR0(), builder->getRegionStride1());
    G4_INST *movInst = builder->createInternalInst(NULL, G4_mov, NULL, false, 8,
        movDst, r0Src, NULL, InstOpt_WriteEnable, 0, lastInst ? lastInst->getCISAOff() : -1, 0);
    if (lastInst)
    {
        movInst->setLocation(lastInst->getLocation());
    }
    instList.push_back(movInst);

    int exdesc = (0x1 << 5) + SFID_SPAWNER;
    // response len = 0, msg len = 1
    int desc = (0x1 << 25) + (0x1 << 4);

    G4_SrcRegRegion* sendSrc = builder->Create_Src_Opnd_From_Dcl(
        dcl, builder->getRegionStride1());

    G4_DstRegRegion *sendDst = builder->createNullDst(Type_UD);

    G4_INST* sendInst = builder->createSendInst(
        NULL,
        G4_send,
        1,
        sendDst,
        sendSrc,
        builder->createImm(exdesc, Type_UD),
        builder->createImm(desc, Type_UD),
        InstOpt_NoOpt,
        true,
        true,
        NULL,
        0);
    // need to make sure builder list is empty since later phases do a splice on the entire list
    builder->instList.pop_back();
    // createSendInst incorrectly sets its cisa offset to the last value of the counter.
    sendInst->setCISAOff(movInst->getCISAOff());
    sendInst->setLocation(movInst->getLocation());
    instList.push_back(sendInst);
}

void G4_BB::emitInstructionInfo(std::ostream& output, INST_LIST_ITER &it)
{
    bool emitFile = false, emitLineNo = false;
    char* curFilename = (*it)->getSrcFilename();
    int curSrcLineNo = (*it)->getLineNo();
    const int maxLineLen = 256;
    char curSrcLine[maxLineLen];

    if (prevFilename == NULL && curFilename != NULL) {
        // emit filename and line no
        emitFile = true;
        emitLineNo = true;
    }

    if (prevFilename != NULL && curFilename != NULL) {
        if (strcmp(prevFilename, curFilename) != 0) {
            emitFile = true;
            emitLineNo = true;
        }
    }

    if (prevSrcLineNo != curSrcLineNo) {
        emitLineNo = true;
    }

    if ((*it)->isLabel() == true) {
        emitFile = false;
        emitLineNo = false;
    }

    // don't emit lineno 0 (used for instructions without corresponding src locations)
    if (curSrcLineNo == 0) {
        emitLineNo = false;
    }

    // always emit filename with lineno if there is one
    emitFile = emitLineNo && (curFilename != NULL);

    if (emitLineNo == true)
    {
        FILE* fp;
        int current_src_line = 0;

        if (curFilename != NULL)
        {
            // Get src line to print
            fp = fopen(curFilename, "r");
            if (fp != NULL)
            {
                while (current_src_line < curSrcLineNo)
                {
                    if (fgets(curSrcLine, maxLineLen, fp) == nullptr)
                    {
                        strcpy_s(curSrcLine, maxLineLen, " Line could not be read\n");
                        break;
                    }
                    int length = (int)strlen(curSrcLine);
                    if (length && curSrcLine[length - 1] != '\n')
                    {
                        // source line exceeds 255 characters, skips rest of the line
                        char buf[maxLineLen];
                        char* s = nullptr;
                        do
                        {
                            s = fgets(buf, maxLineLen, fp);
                        } while (s != nullptr && buf[strlen(buf) - 1] != '\n');
                    }
                    current_src_line++;
                }
                fclose(fp);
            }
            else
            {
                curSrcLine[0] = 0;
                if (parent->getKernel()->getOptions()->getTarget() != VISATarget::VISA_3D)
                {
                    strcpy_s(curSrcLine, maxLineLen, " Cannot parse because src file not found\n");
                }
            }
        }
        else
        {
            curSrcLine[0] = 0;
            if (parent->getKernel()->getOptions()->getTarget() != VISATarget::VISA_3D)
            {
                strcpy_s(curSrcLine, maxLineLen, " Cannot parse because src file not found\n");
            }
        }
    }

    // remove leading whitespace from source line for comment
    char* curSrcLineTextPtr = curSrcLine;
    if (emitLineNo) {
        while (*curSrcLineTextPtr == ' ' || *curSrcLineTextPtr == '\t')
        {
            ++curSrcLineTextPtr;
        }
    }

    if (emitFile == true && emitLineNo == false) {
        output << std::endl;
        output << "// " << (const char*)curFilename << std::endl;
    }
    else if (emitLineNo == true && curSrcLine[0] != 0) {
        output << std::endl;
        if (emitFile) {
            output << "// " << (const char*)curFilename;
        }
        else {
            output << "// ??";
        }
        output << "(" << curSrcLineNo << "): " << curSrcLineTextPtr;
        int len = (int)strlen(curSrcLine);
        if (len == 0 || curSrcLine[len - 1] != '\n')
        {
            // Print line feed if not found at end of line
            output << std::endl;
        }
    }

    if (emitFile == true)
        prevFilename = curFilename;

    if (emitLineNo == true)
        prevSrcLineNo = curSrcLineNo;
}

void G4_BB::emitBankConflict(std::ostream& output, G4_INST *inst)
{
    int regNum[2][G4_MAX_SRCS];
    int execSize[G4_MAX_SRCS];
    int regSrcNum = 0;


    if (inst->getNumSrc() == 3 && !inst->isSend())
    {
        for (unsigned i = 0; i < G4_Inst_Table[inst->opcode()].n_srcs; i++)
        {
            G4_Operand * srcOpnd = inst->getSrc(i);
            regNum[1][i] = -1;
            if (srcOpnd)
            {
                if (srcOpnd->isSrcRegRegion() &&
                    srcOpnd->asSrcRegRegion()->getBase() &&
                    srcOpnd->asSrcRegRegion()->getBase()->isRegVar())
                {
                    G4_RegVar* baseVar = static_cast<G4_RegVar*>(srcOpnd->asSrcRegRegion()->getBase());
                    if (baseVar->isGreg()) {
                        uint32_t byteAddress = srcOpnd->getLinearizedStart();
                        if (byteAddress != 0) {
                            regNum[0][i] = byteAddress / GENX_GRF_REG_SIZ;
                        }
                        else {
                            // before RA, use the value in Greg directly
                            regNum[0][i] = baseVar->getPhyReg()->asGreg()->getRegNum();
                        }
                        regNum[1][i] = regNum[0][i];
                        regSrcNum++;
                    }
                    execSize[i] = srcOpnd->getLinearizedEnd() - srcOpnd->getLinearizedStart();
                }
            }
        }
    }


    if (regSrcNum == 3)
    {
        int maxGRFNum = 0;
        output << " {";
        if (parent->builder->oneGRFBankDivision())
        {//EVEN/ODD
            for (int i = 0; i < 3; i++)
            {
                output << i << "=";
                if (!(regNum[0][i] % 2) && regNum[0][i] < SECOND_HALF_BANK_START_GRF)
                {
                    output << "EL, ";
                }
                if (regNum[0][i] % 2 && regNum[0][i] < SECOND_HALF_BANK_START_GRF)
                {
                    output << "OL, ";
                }
                if (!(regNum[0][i] % 2) && regNum[0][i] >= SECOND_HALF_BANK_START_GRF)
                {
                    output << "EH, ";
                }
                if (regNum[0][i] % 2 && regNum[0][i] >= SECOND_HALF_BANK_START_GRF)
                {
                    output << "OH, ";
                }
            }
        }
        else
        { //EVEN EVEN/ODD ODD
            for (int i = 0; i < 3; i++)
            {
                output << i << "=";
                for (int j = 0; j < (execSize[i] + GENX_GRF_REG_SIZ - 1) / GENX_GRF_REG_SIZ; j++)
                {
                    int reg_num = regNum[0][i] + j;
                    if (!(reg_num & 0x02) && reg_num < SECOND_HALF_BANK_START_GRF)
                    {
                        output << "EL, ";
                    }
                    if ((reg_num & 0x02) && reg_num < SECOND_HALF_BANK_START_GRF)
                    {
                        output << "OL, ";
                    }
                    if (!(reg_num & 0x02) && reg_num >= SECOND_HALF_BANK_START_GRF)
                    {
                        output << "EH, ";
                    }
                    if ((reg_num & 0x02) && reg_num >= SECOND_HALF_BANK_START_GRF)
                    {
                        output << "OH, ";
                    }
                    if (j > 1)
                    {
                        regNum[1][i] = reg_num;
                    }
                }
                maxGRFNum = ((execSize[i] + GENX_GRF_REG_SIZ - 1) / GENX_GRF_REG_SIZ) > maxGRFNum ?
                    ((execSize[i] + GENX_GRF_REG_SIZ - 1) / GENX_GRF_REG_SIZ) : maxGRFNum;
            }

#ifdef DEBUG_VERBOSE_ON
            if (((regNum[0][1] & 0x02) == (regNum[0][2] & 0x02)) &&
                ((regNum[0][1] >= SECOND_HALF_BANK_START_GRF && regNum[0][2] < SECOND_HALF_BANK_START_GRF) ||
                (regNum[0][1] < SECOND_HALF_BANK_START_GRF && regNum[0][2] >= SECOND_HALF_BANK_START_GRF)))
            {
                noBankCount++;
            }
#endif
        }
        output << "BC=";
        if (!parent->builder->twoSourcesCollision())
        {
            if (!parent->builder->oneGRFBankDivision())
            { //EVEN EVEN/ODD ODD
                ASSERT_USER(maxGRFNum < 3, "Not supporting register size > 2");
                if (maxGRFNum == 2)
                {
                    for (int i = 0; i < maxGRFNum; i++)
                    {
                        if ((regNum[i][1] & 0x02) == (regNum[i][2] & 0x02))
                        {
                            if ((regNum[i][1] < SECOND_HALF_BANK_START_GRF &&
                                regNum[i][2] < SECOND_HALF_BANK_START_GRF) ||
                                (regNum[i][1] >= SECOND_HALF_BANK_START_GRF &&
                                    regNum[i][2] >= SECOND_HALF_BANK_START_GRF))
                            {
                                parent->BCStats.addBad();
                                output << "BAD,";
                            }
                            else
                            {
                                parent->BCStats.addOK();
                                output << "OK,";
                            }
                        }
                        else
                        {
                            parent->BCStats.addGood();
                            output << "GOOD,";
                        }
                    }
                }
                else
                {
                    for (int i = 0; i < maxGRFNum; i++)
                    {
                        if (((regNum[i][1] & 0x02) == (regNum[i][2] & 0x02)) &&
                            ((regNum[i][0] & 0x02) == (regNum[i][1] & 0x02)))
                        {
                            if ((regNum[i][0] < SECOND_HALF_BANK_START_GRF &&
                                regNum[i][1] < SECOND_HALF_BANK_START_GRF &&
                                regNum[i][2] < SECOND_HALF_BANK_START_GRF) ||
                                (regNum[i][0] >= SECOND_HALF_BANK_START_GRF &&
                                    regNum[i][1] >= SECOND_HALF_BANK_START_GRF &&
                                    regNum[i][2] >= SECOND_HALF_BANK_START_GRF))
                            {
                                parent->BCStats.addBad();
                                output << "BAD,";
                            }
                            else
                            {
                                parent->BCStats.addOK();
                                output << "OK,";
                            }
                        }
                        else
                        {
                            parent->BCStats.addGood();
                            output << "GOOD,";
                        }
                    }
                }
            }
            else
            {  //EVEN/ODD
                if ((regNum[0][1] % 2) != (regNum[0][2] % 2) ||
                    (regNum[0][0] % 2) != (regNum[0][1] % 2) ||
                    (regNum[0][1] == regNum[0][2]))
                {
                    parent->BCStats.addGood();
                    output << "GOOD";
                }
                else
                {
                    if ((regNum[0][0] < SECOND_HALF_BANK_START_GRF &&
                        regNum[0][1] < SECOND_HALF_BANK_START_GRF &&
                        regNum[0][2] < SECOND_HALF_BANK_START_GRF) ||
                        (regNum[0][0] >= SECOND_HALF_BANK_START_GRF &&
                            regNum[0][1] >= SECOND_HALF_BANK_START_GRF &&
                            regNum[0][2] >= SECOND_HALF_BANK_START_GRF))
                    {
                        parent->BCStats.addBad();
                        output << "BAD";
                    }
                    else
                    {
                        parent->BCStats.addOK();
                        output << "OK";
                    }
                }
            }
        }
        else  //Two source
        {  //   EVEN/ODD
            if ((regNum[0][1] != regNum[0][2]) &&
                ((regNum[0][1] % 2) == (regNum[0][2] % 2)))
            {
                if ((regNum[0][1] < SECOND_HALF_BANK_START_GRF &&
                    regNum[0][2] < SECOND_HALF_BANK_START_GRF) ||
                    (regNum[0][1] >= SECOND_HALF_BANK_START_GRF &&
                        regNum[0][2] >= SECOND_HALF_BANK_START_GRF))
                {
                    parent->BCStats.addBad();
                    output << "BAD";
                }
                else
                {
                    parent->BCStats.addOK();
                    output << "OK";
                }
            }
            else
            {
                parent->BCStats.addGood();
                output << "GOOD";
            }
        }
        output << "}";
    }
}


static void emitInstId(std::ostream& output, int srcLine, int vISAId, int genId)
{
    if (srcLine != 0)
    {
        output << "#" << srcLine << ":";
    }
    if (vISAId != -1)
    {
        output << "$" << vISAId << ":";
    }
    if (genId != -1)
    {
        output << "&" << genId;
    }
}

void G4_BB::emitBasicInstructionIga(char* instSyntax, std::ostream& output, INST_LIST_ITER &it, G4_INST* prevInst, uint32_t &BCNum, uint32_t &simd8Num)
{
    G4_INST* inst = *it;

    output << instSyntax;
    if (!inst->isLabel() && inst->opcode() < G4_NUM_OPCODE)
    {
        output << " //";
        emitInstId(output, inst->getLineNo(), inst->getCISAOff(), inst->getGlobalID());

         emitBankConflict(output, inst);
        if (inst->isSend())
        {
            inst->emit_send_desc(output);
        }
    }
}
void G4_BB::emitBasicInstruction(std::ostream& output, INST_LIST_ITER &it)
{
    if ((*it)->isSend())
    {
        //
        // emit send instruction
        //
        G4_INST* SendInst = *it;
        SendInst->emit_send(output);

        output << " //";
        emitInstId(output, SendInst->getLineNo(), SendInst->getCISAOff(), SendInst->getGlobalID());
        (*it)->emit_send_desc(output);
    }
    else
    {
        //
        // emit label and instruction
        //
        G4_INST *inst = *it;
        inst->emit(output, parent->builder->getOption(vISA_SymbolReg));
        if ((*it)->isLabel() == false)
        {
            output << " //";
            emitInstId(output, inst->getLineNo(), inst->getCISAOff(), inst->getGlobalID());
            emitBankConflict(output, inst);
        }
    }
}
void G4_BB::emitInstruction(std::ostream& output, INST_LIST_ITER &it)
{
    //prints out instruction line
    emitInstructionInfo(output, it);

    emitBasicInstruction(output, it);

    output << std::endl;
}
void G4_BB::emit(std::ostream& output)
{

    for (INST_LIST_ITER it = instList.begin(); it != instList.end(); ++it)
    {
        emitInstruction(output, it);
    }
}

void G4_BB::resetLocalId()
{
    int i = 0;

    for (INST_LIST_ITER iter = instList.begin(), end = instList.end();
        iter != end;
        ++iter, ++i)
    {
        (*iter)->setLocalId(i);
    }
}

void G4_BB::dump() const
{
    for (auto& x : instList)
        x->dump();
    std::cerr << "\n";
}

void G4_BB::dumpDefUse() const
{
    for (auto& x : instList)
    {
        x->dump();
        if (x->def_size() > 0 || x->use_size() > 0)
        {
            x->dumpDefUse();
            std::cerr << "\n\n\n";
        }
    }
}

// all of the operand in this table are srcRegion
void GlobalOpndHashTable::addGlobalOpnd(G4_Operand *opnd)
{
    G4_Declare *topDcl = opnd->getTopDcl();

    if (topDcl != NULL)
    {

        // global operands must have a declare
        auto entry = globalOperands.find(topDcl);
        if (entry != globalOperands.end())
        {
            entry->second->insert((uint16_t)opnd->getLeftBound(), (uint16_t)opnd->getRightBound());
        }
        else
        {
            HashNode* node = new (mem)HashNode(
                (uint16_t)opnd->getLeftBound(),
                (uint16_t)opnd->getRightBound(),
                private_arena_allocator);
            globalOperands[topDcl] = node;
        }
    }
}

// if def overlaps with any operand in this table, it is treated as global
bool GlobalOpndHashTable::isOpndGlobal(G4_Operand *opnd)
{

    G4_Declare* dcl = opnd->getTopDcl();
    if (dcl == NULL)
    {
        return false;
    }
    else if (dcl->getAddressed() == true)
    {
        // Conservatively assume that all address taken
        // virtual registers are global
        return true;
    }
    else if (dcl->getHasFileScope() == true)
    {
        return true;
    }
    else
    {
        auto entry = globalOperands.find(dcl);
        if (entry == globalOperands.end())
        {
            return false;
        }
        HashNode* node = entry->second;
        return node->isInNode((uint16_t)opnd->getLeftBound(), (uint16_t)opnd->getRightBound());
    }
}

void GlobalOpndHashTable::dump()
{
    for (auto&& entry : globalOperands)
    {
        G4_Declare* dcl = entry.first;
        dcl->emit(std::cerr, false, false);
        if ((dcl->getRegFile() & G4_FLAG) == 0)
        {
            std::vector<bool> globalElt;
            globalElt.resize(dcl->getByteSize(), false);
            auto ranges = entry.second;
            for (auto bound : ranges->bounds)
            {
                uint16_t lb = getLB(bound);
                uint16_t rb = getRB(bound);
                for (int i = lb; i <= rb; ++i)
                {
                    globalElt[i] = true;
                }
            }
            bool inRange = false;
            for (int i = 0, size = (int)globalElt.size(); i < size; ++i)
            {
                if (globalElt[i] && !inRange)
                {
                    // start of new range
                    std::cerr << "[" << i << ",";
                    inRange = true;
                }
                else if (!globalElt[i] && inRange)
                {
                    // end of range
                    std::cerr << i - 1 << "], ";
                    inRange = false;
                }
            }
            if (inRange)
            {
                // close last range
                std::cerr << globalElt.size() - 1 << "]";
            }
        }
        std::cerr << "\n";
    }
}

void G4_Kernel::calculateSimdSize()
{
    // Iterate over all instructions in kernel to check
    // whether default execution size of kernel is
    // SIMD8/16. This is required for knowing alignment
    // to use for GRF candidates.

    // only do it once per kernel, as we should not introduce inst with larger simd size than in the input
    if (simdSize != 0)
    {
        return;
    }

    simdSize = 8;

    for (auto bb : fg.BBs)
    {
        for (auto inst : bb->instList)
        {
            // do not consider send since for certain messages we have to set its execution size
            // to 16 even in simd8 shaders
            if (!inst->isLabel() && !inst->isSend())
            {
                uint32_t size = inst->getMaskOffset() + inst->getExecSize();
                if (size > 16)
                {
                    simdSize = 32;
                    return;
                }
                else if (size > 8)
                {
                    simdSize = 16;
                }
            }
        }
    }
}

void G4_Kernel::dump() const
{
    std::cerr << "G4_Kernel: " << this->name << "\n";
    for (auto& B : this->fg.BBs)
        B->dump();
}

//
// Perform DFS traversal on the flow graph (do not enter subroutine, but mark subroutine blocks
// so that they will be processed independently later)
//
void FlowGraph::DFSTraverse(G4_BB* startBB, unsigned &preId, unsigned &postId, FuncInfo* fn)
{
    MUST_BE_TRUE(fn != NULL, "Invalid func info");
    std::stack<G4_BB*> traversalStack;
    traversalStack.push(startBB);

    while (!traversalStack.empty())
    {
        G4_BB*  bb = traversalStack.top();
        if (bb->getPreId() != UINT_MAX)
        {
            // Pre-processed already and continue to the next one.
            // Before doing so, set postId if not set before.
            traversalStack.pop();
            if (bb->getRPostId() == UINT_MAX)
            {
                // All bb's succ has been visited (PreId is set) at this time.
                // if any of its succ has not been finished (RPostId not set),
                // bb->succ forms a backedge.
                //
                // Note: originally, CALL and EXIT will not check back-edges, here
                //       we skip checking for them as well. (INIT & RETURN should
                //       be checked as well ?)
                if (!(bb->getBBType() & (G4_BB_CALL_TYPE | G4_BB_EXIT_TYPE)))
                {
                    for (auto succBB : bb->Succs)
                    {
                        if (succBB->getRPostId() == UINT_MAX)
                        {
                            backEdges.push_back(Edge(bb, succBB));
                        }
                    }
                }

                // Need to keep this after backedge checking so that self-backedge
                // (single-bb loop) will not be missed.
                bb->setRPostId(postId++);
            }
            continue;
        }

        fn->addBB(bb);
        bb->setPreId(preId++);

        if (bb->getBBType() & G4_BB_CALL_TYPE)
        {
            G4_BB* returnBB = bb->BBAfterCall();
            MUST_BE_TRUE(bb->Succs.front()->getBBType() & G4_BB_INIT_TYPE, ERROR_FLOWGRAPH);
            MUST_BE_TRUE(bb->Succs.size() == 1, ERROR_FLOWGRAPH);

            {
                bool found = false;
                for (auto func : fn->getCallees())
                {
                    if (func == bb->getCalleeInfo())
                        found = true;
                }
                if (!found)
                {
                    fn->addCallee(bb->getCalleeInfo());
                }
            }

            if (returnBB->getPreId() == UINT_MAX)
            {
                traversalStack.push(returnBB);
            }
            else
            {
                MUST_BE_TRUE(false, ERROR_FLOWGRAPH);
            }
        }
        else if (bb->getBBType() & G4_BB_EXIT_TYPE)
        {
            // Skip
        }
        else
        {
            // To be consistent with previous behavior, use reverse_iter.
            BB_LIST_RITER RIE = bb->Succs.rend();
            for (BB_LIST_RITER rit = bb->Succs.rbegin(); rit != RIE; ++rit)
            {
                G4_BB* succBB = *rit;
                if (succBB->getPreId() == UINT_MAX)
                {
                    traversalStack.push(succBB);
                }
            }
        }
        // As the top of stack may be different than that at the
        // beginning of this iteration, cannot do pop here. Instead,
        // do pop and set RPostId at the beginning of each iteration.
        //
        // traversalStack.pop();
        // bb->setRPostId(postId++);
    }
}

void FlowGraph::markRPOTraversal()
{
    MUST_BE_TRUE(numBBId == BBs.size(), ERROR_FLOWGRAPH);

    unsigned postID = 0;
    backEdges.clear();

    for (auto curBB : BBs)
    {
        curBB->setRPostId(postID++);

        if (curBB->instList.size() > 0)
        {
            if (curBB->getBBType() & G4_BB_CALL_TYPE)
            {
                // skip
            }
            else if (curBB->getBBType() & G4_BB_EXIT_TYPE)
            {
                // Skip
            }
            else
            {
                for (auto succBB : curBB->Succs)
                {
                    if (curBB->getId() >= succBB->getId())
                    {
                        backEdges.push_back(Edge(curBB, succBB));
                    }
                }
            }
        }
    }
}

//
// Find back-edges in the flow graph.
//
void FlowGraph::findBackEdges()
{
    MUST_BE_TRUE(numBBId == BBs.size(), ERROR_FLOWGRAPH);

    for (auto bb : BBs)
    {
        bb->setPreId(UINT_MAX);
        bb->setRPostId(UINT_MAX);
    }

    unsigned preId = 0;
    unsigned postID = 0;
    backEdges.clear();

    DFSTraverse(entryBB, preId, postID, kernelInfo);

    for (auto fn : funcInfoTable)
    {
        DFSTraverse(fn->getInitBB(), preId, postID, fn);
    }
}

//
// Find natural loops in the flow graph.
// Assumption: the input FG is reducible.
//
void FlowGraph::findNaturalLoops()
{
    for (auto&& backEdge : backEdges)
    {
        G4_BB* head = backEdge.second;
        G4_BB* tail = backEdge.first;
        std::list<G4_BB*> loopBlocks;
        Blocks loopBody;
        loopBlocks.push_back(tail);
        loopBody.insert(tail);

        while (!loopBlocks.empty())
        {
            G4_BB* loopBlock = loopBlocks.front();
            loopBlocks.pop_front();
            loopBlock->setInNaturalLoop(true);
            loopBlock->setNestLevel();

            if ((loopBlock == head) || (loopBlock->getBBType() & G4_BB_INIT_TYPE))
            {
                // Skip
            }
            else if (loopBlock->getBBType() & G4_BB_RETURN_TYPE)
            {
                if (!loopBlock->BBBeforeCall()->isInNaturalLoop())
                {
                    loopBlocks.push_front(loopBlock->BBBeforeCall());
                    loopBody.insert(loopBlock->BBBeforeCall());
                }
            }
            else {
                for (auto predBB : loopBlock->Preds)
                {
                    if (!predBB->isInNaturalLoop())
                    {
                        if (predBB == entryBB && head != entryBB)
                        {
                            // graph is irreducible, punt natural loop detection for entire CFG
                            this->reducible = false;
                            naturalLoops.clear();
                            for (auto BB : BBs)
                            {
                                BB->setInNaturalLoop(false);
                                BB->resetNestLevel();
                            }
                            return;
                        }
                        MUST_BE_TRUE(predBB != entryBB || head == entryBB, ERROR_FLOWGRAPH);
                        loopBlocks.push_front(predBB);
                        loopBody.insert(predBB);
                    }
                }
            }
        }

        for (auto loopBB : loopBody)
        {
            loopBB->setInNaturalLoop(false);
        }

        naturalLoops.insert(pair<Edge, Blocks>(backEdge, loopBody));
    }
}

void FlowGraph::traverseFunc(FuncInfo* func, unsigned int *ptr)
{
    func->setPreID((*ptr)++);
    func->setVisited();
    for (auto callee : func->getCallees())
    {
        if (!(callee->getVisited()))
        {
            traverseFunc(callee, ptr);
        }
    }
    sortedFuncTable.push_back(func);
    func->setPostID((*ptr)++);
}

//
// Sort functions in topological order
//
void FlowGraph::sortFuncs()
{
    unsigned int visitID = 1;
    traverseFunc(kernelInfo, &visitID);
}

//
// This should be called only after pre-/post-visit ID are set
//
static bool checkVisitID(FuncInfo* func1, FuncInfo* func2)
{
    if (func1->getPreID() < func2->getPreID() &&
        func1->getPostID() > func2->getPostID())
    {
        return true;
    }
    else
    {
        return false;
    }
}

//
// Find dominators for each function
//
void FlowGraph::findDominators(std::map<FuncInfo*, std::set<FuncInfo*>>& domMap)
{
    std::map<FuncInfo*, std::set<FuncInfo*>> predMap;

    for (auto func : sortedFuncTable)
    {
        if (func == kernelInfo)
        {
            std::set<FuncInfo*> initSet;
            initSet.insert(kernelInfo);
            domMap.insert(std::make_pair(kernelInfo, initSet));
        }
        else
        {
            std::set<FuncInfo*> initSet;
            for (auto funcTmp : sortedFuncTable)
            {
                initSet.insert(funcTmp);
            }
            domMap.insert(std::make_pair(func, initSet));
        }

        for (auto callee : func->getCallees())
        {
            std::map<FuncInfo*, std::set<FuncInfo*>>::iterator predMapIter = predMap.find(callee);
            if (predMapIter == predMap.end())
            {
                std::set<FuncInfo*> initSet;
                initSet.insert(func);
                predMap.insert(std::make_pair(callee, initSet));
            }
            else
            {
                (*predMapIter).second.insert(func);
            }
        }
    }

    bool changed = false;
    do
    {
        changed = false;

        unsigned int funcTableSize = static_cast<unsigned int> (sortedFuncTable.size());
        unsigned int funcID = funcTableSize - 1;
        do
        {
            funcID--;
            FuncInfo* func = sortedFuncTable[funcID];

            std::map<FuncInfo*, std::set<FuncInfo*>>::iterator predMapIter = predMap.find(func);
            if (predMapIter != predMap.end())
            {
                std::set<FuncInfo*>& domSet = (*domMap.find(func)).second;
                std::set<FuncInfo*> oldDomSet = domSet;
                domSet.clear();
                domSet.insert(func);

                std::vector<unsigned int> funcVec(funcTableSize);
                for (unsigned int i = 0; i < funcTableSize; i++)
                {
                    funcVec[i] = 0;
                }

                std::set<FuncInfo*>& predSet = (*predMapIter).second;
                for (auto pred : predSet)
                {
                    for (auto predDom : (*domMap.find(pred)).second)
                    {
                        unsigned int domID = (predDom->getScopeID() == UINT_MAX) ? funcTableSize - 1 : predDom->getScopeID() - 1;
                        funcVec[domID]++;
                    }
                }

                unsigned int predSetSize = static_cast<unsigned int> (predSet.size());
                for (unsigned int i = 0; i < funcTableSize; i++)
                {
                    if (funcVec[i] == predSetSize)
                    {
                        FuncInfo* newFunc = sortedFuncTable[i];
                        domSet.insert(newFunc);
                        if (oldDomSet.find(newFunc) == oldDomSet.end())
                            changed = true;
                    }
                }

                if (oldDomSet.size() != domSet.size())
                {
                    changed = true;
                }
            }
        } while (funcID != 0);

    } while (changed);
}

//
// Check if func1 is a dominator of func2
//
static bool checkDominator(FuncInfo* func1, FuncInfo* func2, std::map<FuncInfo*, std::set<FuncInfo*>>& domMap)
{
    std::map<FuncInfo*, std::set<FuncInfo*>>::iterator domMapIter = domMap.find(func2);

    if (domMapIter != domMap.end())
    {
        std::set<FuncInfo*> domSet = (*domMapIter).second;
        std::set<FuncInfo*>::iterator domSetIter = domSet.find(func1);

        if (domSetIter != domSet.end())
        {
            return true;
        }
    }

    return false;
}

//
// Determine the scope of a varaible based on different contexts
//
unsigned int FlowGraph::resolveVarScope(G4_Declare* dcl, FuncInfo* func)
{
    unsigned int oldID = dcl->getScopeID();
    unsigned int newID = func->getScopeID();

    if (oldID == newID)
    {
        return oldID;
    }
    else if (oldID == 0)
    {
        return newID;
    }
    else if (oldID == UINT_MAX ||
        newID == UINT_MAX)
    {
        return UINT_MAX;
    }
    else if (builder->getOption(vISA_EnableGlobalScopeAnalysis))
    {
        // This is safe if the global variable usage is
        // self-contained under the calling function
        std::map<FuncInfo*, std::set<FuncInfo*>> domMap;

        findDominators(domMap);

        FuncInfo* oldFunc = sortedFuncTable[oldID - 1];

        if (checkVisitID(func, oldFunc) &&
            checkDominator(func, oldFunc, domMap))
        {
            return newID;
        }
        else if (checkVisitID(oldFunc, func) &&
            checkDominator(oldFunc, func, domMap))
        {
            return oldID;
        }
        else
        {
            unsigned int start = (newID > oldID) ? newID : oldID;
            unsigned int end = static_cast<unsigned int> (sortedFuncTable.size());

            for (unsigned int funcID = start; funcID != end; funcID++)
            {
                FuncInfo* currFunc = sortedFuncTable[funcID];
                if (checkVisitID(currFunc, func) &&
                    checkDominator(currFunc, func, domMap) &&
                    checkVisitID(currFunc, oldFunc) &&
                    checkDominator(currFunc, oldFunc, domMap))
                {
                    return currFunc->getScopeID();
                }
            }
        }
    }

    return UINT_MAX;
}

//
// Visit all operands referenced in a function and update the varaible scope
//
void FlowGraph::markVarScope(std::vector<G4_BB*>& BBList, FuncInfo* func)
{
    for (auto bb : BBList)
    {
        for (auto it = bb->instList.begin(); it != bb->instList.end(); it++)
        {
            G4_INST* inst = (*it);

            G4_DstRegRegion* dst = inst->getDst();

            if (dst &&
                !dst->isAreg() &&
                dst->getBase())
            {
                G4_Declare* dcl = GetTopDclFromRegRegion(dst);
                unsigned int scopeID = resolveVarScope(dcl, func);
                dcl->updateScopeID(scopeID);
            }

            for (int i = 0; i < G4_MAX_SRCS; i++)
            {
                G4_Operand* src = inst->getSrc(i);

                if (src && !src->isAreg())
                {
                    if (src->isSrcRegRegion() &&
                        src->asSrcRegRegion()->getBase())
                    {
                        G4_Declare* dcl = GetTopDclFromRegRegion(src);
                        unsigned int scopeID = resolveVarScope(dcl, func);
                        dcl->updateScopeID(scopeID);
                    }
                    else if (src->isAddrExp() &&
                        src->asAddrExp()->getRegVar())
                    {
                        G4_Declare* dcl = src->asAddrExp()->getRegVar()->getDeclare()->getRootDeclare();
                        unsigned int scopeID = resolveVarScope(dcl, func);
                        dcl->updateScopeID(scopeID);
                    }
                }
            }
        }
    }
}

//
// Traverse the call graph and mark varaible scope
//
void FlowGraph::markScope()
{
    sortFuncs();

    unsigned id = 1;
    std::vector<FuncInfo *>::iterator kernelIter = sortedFuncTable.end();
    kernelIter--;
    for (std::vector<FuncInfo *>::iterator funcIter = sortedFuncTable.begin();
        funcIter != sortedFuncTable.end();
        ++funcIter)
    {
        if (funcIter == kernelIter)
        {
            id = UINT_MAX;
        }

        FuncInfo* func = (*funcIter);
        func->setScopeID(id);

        for (auto bb : func->getBBList())
        {
            bb->setScopeID(id);
        }

        id++;
    }

    for (auto func : sortedFuncTable)
    {
        markVarScope(func->getBBList(), func);
    }
}

// Return the mask for anyH or allH predicate control in the goto to be emitted.
static uint32_t getFlagMask(G4_Predicate_Control pCtrl)
{
    switch (pCtrl)
    {
    case G4_Predicate_Control::PRED_ALL2H:
        return 0xFFFFFFFC;
    case G4_Predicate_Control::PRED_ALL4H:
        return 0xFFFFFFF0;
    case G4_Predicate_Control::PRED_ALL8H:
        return 0xFFFFFF00;
    case G4_Predicate_Control::PRED_ALL16H:
        return 0xFFFF0000;
    case G4_Predicate_Control::PRED_ALL32H:
        return 0x00000000;
    case G4_Predicate_Control::PRED_ANY2H:
        return 0x00000003;
    case G4_Predicate_Control::PRED_ANY4H:
        return 0x0000000F;
    case G4_Predicate_Control::PRED_ANY8H:
        return 0x000000FF;
    case G4_Predicate_Control::PRED_ANY16H:
        return 0x0000FFFF;
    case G4_Predicate_Control::PRED_ANY32H:
        return 0xFFFFFFFF;
    default:
        MUST_BE_TRUE(false, "only for AllH or AnyH predicate control");
        break;
    }
    return 0;
}

// Given a predicate ctrl for jmpi, return the adjusted predicate ctrl in a new
// simd size.
static G4_Predicate_Control getPredCtrl(unsigned simdSize,
    G4_Predicate_Control pCtrl)
{
    if (G4_Predicate::isAllH(pCtrl))
        return (simdSize == 8)
        ? PRED_ALL8H
        : (simdSize == 16) ? PRED_ALL16H
        : G4_Predicate_Control::PRED_ALL32H;

    // Any or default
    return (simdSize == 8)
        ? PRED_ANY8H
        : (simdSize == 16) ? PRED_ANY16H
        : G4_Predicate_Control::PRED_ANY32H;
}

// Convert jmpi to goto. E.g.
//
// Case1:
// .decl P1 v_type=P num_elts=2
//
// cmp.ne (M1, 2) P1 V33(0,0)<2;2,1> 0x0:f
// (!P1.any) jmpi (M1, 1) BB1
//
// ===>
//
// cmp.ne (M1, 2) P1 V33(0,0)<2;2,1> 0x0:f
// and (1) P1 P1 0b00000011
// (!P2.any) goto (M1, 8) BB1
//
// Case2:
// .decl P1 v_type=P num_elts=2
//
//  cmp.ne (M1, 2) P1 V33(0,0)<2;2,1> 0x0:f
// (!P1.all) jmpi (M1, 1) BB1
//
// ===>
//
// cmp.ne (M1, 2) P1 V33(0,0)<2;2,1> 0x0:f
// or (1) P1 P1 0b11111100
// (!P1.all) goto (M1, 8) BB1
//
bool FlowGraph::convertJmpiToGoto()
{
    bool Changed = false;
    for (auto bb : BBs)
    {
        for (auto I = bb->instList.begin(); I != bb->instList.end(); ++I)
        {
            G4_INST *inst = *I;
            if (inst->opcode() != G4_jmpi)
                continue;

            unsigned predSize = pKernel->getSimdSize();
            G4_Predicate *newPred = nullptr;

            if (G4_Predicate *pred = inst->getPredicate())
            {
                // The number of bool elements in vISA decl.
                unsigned nElts = pred->getTopDcl()->getNumberFlagElements();

                // Since we need to turn this into goto, set high bits properly.
                if (nElts != predSize)
                {
                    // The underlying dcl type is either uw or ud.
                    G4_Type SrcTy = pred->getTopDcl()->getElemType();
                    G4_Type DstTy = (predSize > 16) ? Type_UD : Type_UW;

                    G4_Predicate_Control pCtrl = pred->getControl();
                    MUST_BE_TRUE(nElts == 1 ||
                        G4_Predicate::isAnyH(pCtrl) ||
                        G4_Predicate::isAllH(pCtrl),
                        "predicate control not handled yet");

                    // Common dst and src0 operand for flag.
                    G4_Declare *newDcl = builder->createTempFlag(predSize > 16 ? 2 : 1);
                    auto pDst = builder->createDstRegRegion(
                        G4_RegAccess::Direct, newDcl->getRegVar(), 0, 0, 1, DstTy);
                    auto pSrc0 = builder->createSrcRegRegion(
                        G4_SrcModifier::Mod_src_undef, G4_RegAccess::Direct,
                        pred->getBase(), 0, 0, builder->getRegionScalar(), SrcTy);

                    auto truncMask = [](uint32_t mask, G4_Type Ty) -> uint64_t
                    {
                        return (Ty == Type_UW) ? uint16_t(mask) : mask;
                    };

                    if (pCtrl == G4_Predicate_Control::PRED_DEFAULT)
                    {
                        // P = P & 1
                        auto pSrc1 = builder->createImm(1, Type_UW);
                        auto pInst = builder->createInternalInst(
                            nullptr, G4_and, nullptr, false, 1, pDst, pSrc0, pSrc1,
                            InstOpt_M0 | InstOpt_WriteEnable);
                        bb->instList.insert(I, pInst);
                    }
                    else if (G4_Predicate::isAnyH(pCtrl))
                    {
                        // P = P & mask
                        uint32_t mask = getFlagMask(pCtrl);
                        auto pSrc1 = builder->createImm(truncMask(mask, DstTy), DstTy);
                        auto pInst = builder->createInternalInst(
                            nullptr, G4_and, nullptr, false, 1, pDst, pSrc0, pSrc1,
                            InstOpt_M0 | InstOpt_WriteEnable);
                        bb->instList.insert(I, pInst);
                    }
                    else
                    {
                        // AllH
                        // P = P | mask
                        uint32_t mask = getFlagMask(pCtrl);
                        auto pSrc1 = builder->createImm(truncMask(mask, DstTy), DstTy);
                        auto pInst = builder->createInternalInst(
                            nullptr, G4_or, nullptr, false, 1, pDst, pSrc0, pSrc1,
                            InstOpt_M0 | InstOpt_WriteEnable);
                        bb->instList.insert(I, pInst);
                    }

                    // Adjust pred control to the new execution size and build the
                    // new predicate.
                    pCtrl = getPredCtrl(predSize, pCtrl);
                    newPred = builder->createPredicate(
                        pred->getState(), newDcl->getRegVar(), 0, pCtrl);
                }
            }

            // (!P) jmpi L
            // becomes:
            // P = P & MASK
            // (!P.anyN) goto (N) L
            inst->setOpcode(G4_goto);
            inst->setExecSize((unsigned char)predSize);
            if (newPred)
                inst->setPredicate(newPred);
            inst->asCFInst()->setUip(inst->getSrc(0));
            inst->setSrc(nullptr, 0);
            inst->setOptions(InstOpt_M0);
            Changed = true;
        }
    }
    return Changed;
}

FlowGraph::~FlowGraph()
{
    // even though G4_BBs are allocated in a mem pool and freed in one shot,
    // we must call each BB's desstructor explicitly to free up the memory used
    // by the STL objects(list, vector, etc.) in each BB
    for (unsigned i = 0, size = (unsigned)BBAllocList.size(); i < size; i++)
    {
        G4_BB* bb = BBAllocList[i];
        bb->~G4_BB();
    }
    BBAllocList.clear();
    globalOpndHT.clearHashTable();
    for (auto funcInfo : subroutines)
    {
        funcInfo->~FuncInfo();
    }
    for (auto funcInfo : funcInfoTable)
    {
        funcInfo->~FuncInfo();
    }
    kernelInfo->~FuncInfo();
    for (auto summary : localRASummaries)
    {
        summary->~PhyRegSummary();
    }
}

KernelDebugInfo* G4_Kernel::getKernelDebugInfo()
{
    if (kernelDbgInfo == nullptr)
    {
        kernelDbgInfo = new(fg.mem)KernelDebugInfo();
    }

    return kernelDbgInfo;
}

G4_Kernel::~G4_Kernel()
{
    if (kernelDbgInfo)
    {
        kernelDbgInfo->~KernelDebugInfo();
    }

    if (gtPinInfo)
    {
        gtPinInfo->~gtPinData();
    }

    Declares.clear();
}

void G4_Kernel::use64BitFESP()
{
    for (auto dcl : Declares)
    {
        if (dcl->getIsPreDefFEStackVar())
        {
            dcl->setTypeToUQ();
        }
    }

    fg.builder->set64BitFEStackVars();
}

void gtPinData::markInsts()
{
    // Take a snapshot of instructions in kernel.
    for (auto bb : kernel.fg.BBs)
    {
        for (auto inst : bb->instList)
        {
            markedInsts.insert(inst);
        }
    }
}

bool isMarked(G4_INST* inst, std::set<G4_INST*>& insts)
{
    if (insts.find(inst) == insts.end())
    {
        return false;
    }
    return true;
}

void gtPinData::removeUnmarkedInsts()
{
    if (!kernel.fg.getIsStackCallFunc() &&
        !kernel.fg.getHasStackCalls())
    {
        // Marked instructions correspond to caller/callee save
        // and FP/SP manipulation instructions.
        return;
    }

    MUST_BE_TRUE(whichRAPass == ReRAPass, "Unexpectedly removing unmarked instructions in first RA pass");
    // Instructions not seen in "marked" snapshot will be removed by this function.
    for (auto bb : kernel.fg.BBs)
    {
        for (auto it = bb->instList.begin(), itEnd = bb->instList.end();
            it != itEnd;)
        {
            auto inst = (*it);

            if (markedInsts.find(inst) == markedInsts.end())
            {
                it = bb->instList.erase(it);
                continue;
            }
            it++;
        }
    }
}

unsigned int G4_Kernel::calleeSaveStart()
{
    return getCallerSaveLastGRF() + 1;
}

unsigned int G4_Kernel::getStackCallStartReg()
{
    // Last 3 GRFs to be used as scratch
    unsigned int totalGRFs = getOptions()->getuInt32Option(vISA_TotalGRFNum);
    unsigned int startReg = totalGRFs - getNumScratchRegs();
    return startReg;
}

unsigned int G4_Kernel::getNumCalleeSaveRegs()
{
    unsigned int totalGRFs = getOptions()->getuInt32Option(vISA_TotalGRFNum);
    return totalGRFs - calleeSaveStart() - getNumScratchRegs();
}

void SCCAnalysis::run()
{
    SCCNodes.resize(cfg.getNumBB());
    for (auto BB : cfg.BBs)
    {
        if (!SCCNodes[BB->getId()])
        {
            findSCC(createSCCNode(BB));
        }
    }
}

void SCCAnalysis::findSCC(SCCNode* node)
{
    SCCStack.push(node);
    for (auto succBB : node->bb->Succs)
    {
        if (succBB == node->bb)
        {
            // no self loop
            continue;
        }
        else if (node->bb->isEndWithCall())
        {
            // ignore call edges and replace it with physical succ instead
            succBB = node->bb->getPhysicalSucc();
            if (!succBB)
            {
                continue;
            }
        }
        else if (node->bb->getBBType() & G4_BB_RETURN_TYPE)
        {
            // stop at return BB
            // ToDo: do we generate (P) ret?
            continue;
        }
        SCCNode* succNode = SCCNodes[succBB->getId()];
        if (!succNode)
        {
            succNode = createSCCNode(succBB);
            findSCC(succNode);
            node->lowLink = std::min(node->lowLink, succNode->lowLink);
        }
        else if (succNode->isOnStack)
        {
            node->lowLink = std::min(node->lowLink, succNode->index);
        }
    }

    // root of SCC
    if (node->lowLink == node->index)
    {
        SCC newSCC(node->bb);
        SCCNode* bodyNode = nullptr;
        do
        {
            bodyNode = SCCStack.top();
            SCCStack.pop();
            bodyNode->isOnStack = false;
            newSCC.addBB(bodyNode->bb);
        } while (bodyNode != node);
        SCCs.push_back(newSCC);
    }
}
