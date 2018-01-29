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

// #define DEBUG_TRACE_ENABLED
#include "../Frontend/IRToString.hpp"
#include "../asserts.hpp"

#include "../ErrorHandler.hpp"
#include "Block.hpp"
#include "Instruction.hpp"

#include <sstream>

using namespace iga;

struct BlockInference
{
    std::map<int32_t, Block *> &blockStarts;
    MemManager *allocator;

    BlockInference(std::map<int32_t, Block *> &bs, MemManager *a)
        : blockStarts(bs), allocator(a) { }

    Block *getBlock(int32_t pc) {
        auto itr = blockStarts.find(pc);
        if (itr == blockStarts.end()) {
            Block *blk      = new (allocator) Block(pc);
            blockStarts[pc] = blk;
            return blk;
        } else {
            return itr->second;
        }
    }

    void replaceNumericLabel(
        ErrorHandler &errHandler,
        size_t binaryLength,
        Op op,
        int32_t pc,
        int32_t instLen,
        Instruction *inst,
        int srcIx) {
        Operand &src = inst->getSource(srcIx);
        if (src.getKind() == Operand::Kind::LABEL) {
            int32_t absolutePc = src.getImmediateValue().s32;
            if (op != Op::CALLA)
                absolutePc += pc;
            if (absolutePc < 0 || absolutePc > (int32_t)binaryLength) {
                std::stringstream ss;
                ss << "src" << srcIx << " targets";
                if (absolutePc < 0) {
                    ss << " before kernel start";
                } else {
                    ss << " after kernel end";
                }
                ss << ": PC " << absolutePc;

                Loc loc(0, 0, (uint32_t)pc, (uint32_t)instLen);
                errHandler.reportError(loc, ss.str());
            }
            src.setLabelSource(getBlock(absolutePc), src.getType());
        }
    }

    void run(ErrorHandler &errHandler, size_t binaryLength, InstList &insts) {
        // define start block to ensure at least one block exists
        (void)getBlock(0);

        int32_t pc = 0;
        for (Instruction *inst : insts) {
            int32_t instLen = inst->hasInstOpt(InstOpt::COMPACTED) ? 8 : 16;
            const OpSpec &instSpec = inst->getOpSpec();
            if (instSpec.isBranching()) {
                // all branching instructions can redirect to next instruction
                // start a new block after this one
                (void)getBlock(pc + instLen);
                // replace src0
                replaceNumericLabel(
                    errHandler,
                    binaryLength,
                    instSpec.op,
                    pc,
                    instLen,
                    inst,
                    0);
                // replace src1
                if (inst->getSourceCount() > 1) {
                    replaceNumericLabel(
                        errHandler,
                        binaryLength,
                        instSpec.op,
                        pc,
                        instLen,
                        inst,
                        1);
                }
            } else if (inst->hasInstOpt(InstOpt::EOT)) {
                // also treat EOT as the end of a BB
                (void)getBlock(pc + instLen);
            }
            pc += instLen;
        }

        // for each block, we need to append the following instructions
        pc               = 0;
        auto bitr        = blockStarts.begin();
        Block *currBlock = bitr->second;
        bitr++;

        for (Instruction *inst : insts) {
            int32_t instLen = inst->hasInstOpt(InstOpt::COMPACTED) ? 8 : 16;
            if (bitr != blockStarts.end() && pc >= bitr->first) {
                currBlock = bitr->second;
                bitr++;
            }

            currBlock->appendInstruction(inst);

            pc += instLen;
        }
    }
}; // class BlockInference

#ifdef DEBUG_TRACE_ENABLED
static void traceOperand(const Operand &op) {
    switch (op.getKind()) {
    case Operand::Kind::DIRECT:
        DEBUG_TRACE(
            "%s%d.%d",
            ToString(op.getRegName()).c_str(),
            (int)op.getReg().regNum,
            (int)op.getReg().subRegNum);
        break;
    case Operand::Kind::INDIRECT:
        DEBUG_TRACE("%6s", "IND");
        break;
    case Operand::Kind::IMMEDIATE:
        DEBUG_TRACE("0x%llx", op.getImmediateValue().u64);
        if (op.getType() == Type::F) {
            DEBUG_TRACE("(=%f)", op.getImmediateValue().f32);
        } else if (op.getType() == Type::DF) {
            DEBUG_TRACE("(=%lf)", op.getImmediateValue().f64);
        }
        break;
    case Operand::Kind::LABEL:
        DEBUG_TRACE("@%8p:%d", op.getTargetBlock(), op.getImmediateValue().s32);
        break;
    case Operand::Kind::INVALID:
        DEBUG_TRACE("%6s", "INV");
        break;
    default:
        DEBUG_TRACE("%6s", "??");
        break;
    }
    if (op.getType() != Type::RESERVED) {
        DEBUG_TRACE("%s", ToString(op.getType()).c_str());
    }
}
#endif

std::map<int32_t, Block *> Block::inferBlocks(
    ErrorHandler &errHandler,
    size_t binaryLength,
    MemManager &mem,
    InstList &insts) {
    std::map<int32_t, Block *> blockStarts;
    BlockInference bi(blockStarts, &mem);
    bi.run(errHandler, binaryLength, insts);

#ifdef DEBUG_TRACE_ENABLED
    for (auto bitr : blockStarts) {
        auto instList = bitr.second->getInstList();
        DEBUG_TRACE(
            "BLOCK %5d (%d) => %d instrs   \n",
            (int)bitr.first,
            (int)bitr.second->getOffset(),
            (int)instList.size());
        for (auto inst : instList) {
            DEBUG_TRACE(
                "  ID%3d => PC%3d  %-10s",
                inst->getID(),
                inst->getDecodePC(),
                inst->getOpSpec()->mnemonic);
            traceOperand(inst->getDestination());

            for (int srcIx = 0; srcIx < (int)inst->getSourceCount(); srcIx++) {
                DEBUG_TRACE(",  ");
                traceOperand(inst->getSource(srcIx));
            }
            DEBUG_TRACE("\n");
        }
    }
#endif

    return blockStarts;
}

void Block::insertInstBefore(InstList::iterator iter,
    Instruction *inst) {
    m_instructions.insert(iter, inst);
}