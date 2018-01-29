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
#ifndef _IGA_INSTRUCTION_HPP_
#define _IGA_INSTRUCTION_HPP_

// WARNING: the IR is subject to change without any notice.  External tools
// should use the official interfaces in the external API.  Those interfaces
// are tested between releases and maintained even with changes to the IR.

#include "../MemManager/MemManager.hpp"
#include "../MemManager/StdArenaAllocator.hpp"

#include "../Models/Models.hpp"
#include "Operand.hpp"
#include "Loc.hpp"
#include "Types.hpp"
#include "../EnumBitset.hpp"
#include "../asserts.hpp"

#include <string>

namespace iga
{
    enum SourceIndex
    {
        SRC0,
        SRC1,
        SRC2,
    };

    // represents a GEN Instruction of some sort
    class Instruction
    {
    public:
        Instruction(
            const OpSpec &opSpec,
            ExecSize execSize,
            ChannelOffset chOff,
            MaskCtrl ectrl)
            : m_opSpec(opSpec)
            , m_maskCtrl(ectrl)
            , m_execSize(execSize)
            , m_chOff(chOff)
            , m_flagModifier(FlagModifier::NONE)
            , m_instID(0xFFFFFFFF)
            , m_pc(-1)
            , m_instLoc(Loc::INVALID)
            , m_comment(nullptr)
        {
            m_pred.function = PredCtrl::NONE;
            m_pred.inverse = false;
            m_flagReg = REGREF_ZERO_ZERO;
        }

        // for placement allocation
        void operator delete(void *foo, MemManager* m) { }
        void *operator new(size_t sz, MemManager* m) { return m->alloc(sz); }

        ///////////////////////////////////////////////////////////////////////
        // operations that set instruction state
        //
        void setLoc(const Loc &loc) { m_instLoc = loc; }
        void setExecSize(ExecSize es) { m_execSize = es; }
        void setBranchCntrl(BranchCntrl bc) { m_brnch = bc; }
        void setFlagModifier(FlagModifier flagModifier) { m_flagModifier = flagModifier; }
        void setFlagReg(const RegRef &reg) { m_flagReg = reg; }
        void setPredication(const Predication &predOpnd) { m_pred = predOpnd; }
        void setPC(int32_t pc){ m_pc = pc; }

        void setDirectDestination(
            DstModifier dstMod,
            RegName rType,
            const RegRef& reg,
            const Region::Horz& rgn,
            Type type);
        void setMacroDestination(
            DstModifier dstMod,
            RegName r,
            const RegRef &reg,
            ImplAcc acc,
            Type type);
        void setInidirectDestination(
            DstModifier dstMod,
            const RegRef &addrReg,
            int16_t immOffset,
            const Region::Horz& rgnH,
            Type type);
        void setDirectSource(
            SourceIndex srcIx,
            SrcModifier srcMod,
            RegName rType,
            const RegRef& reg,
            const Region& rgn,
            Type type);
        void setInidirectSource(
            SourceIndex srcIx,
            SrcModifier srcMod,
            const RegRef& reg,
            int16_t m_immOffset,
            const Region& rgn,
            Type type);
        void setMacroSource(
            SourceIndex srcIx,
            SrcModifier srcMod,
            RegName r,
            const RegRef &reg,
            ImplAcc acc,
            Type type);
        void setImmediateSource(
            SourceIndex srcIx,
            const ImmVal &val,
            Type type);
        void setLabelSource(
            SourceIndex srcIx,
            Block *jip,
            Type type = Type::INVALID);
        void setLabelSource(
            SourceIndex srcIx,
            int32_t pc,
            Type type = Type::INVALID);
        void setSource(
            SourceIndex srcIx,
            const Operand &op); // e.g. pass Operand::NULL_UD_SRC

        void setMsgDesc(const SendDescArg &msg) { m_desc = msg; }
        void setExtMsgDesc(const SendDescArg &msg) { m_exDesc = msg; }
        void setDecodePC(int32_t pc) { m_pc = pc; }
        void setID(int id) { m_instID = id; }
        void setComment(const char *comment) { m_comment = comment; }
        void addInstOpt(const InstOpt &opt) { m_instOpts.add(opt); }
        void addInstOpts(const InstOptSet &opts) { m_instOpts.add(opts); }

        ///////////////////////////////////////////////////////////////////////
        // operations that get instruction state
        //

        // the source or binary location
        const Loc         &getLoc()            const { return m_instLoc; }

        // returns the instruction op specification
        const OpSpec      &getOpSpec()         const { return m_opSpec; }

        // WrEn (NoMask)
        MaskCtrl           getMaskCtrl()       const { return m_maskCtrl; }
        // predication
        bool               hasPredication() const { return m_pred.function != PredCtrl::NONE; }
        const Predication& getPredication()    const { return m_pred; }

        // opcode, math function control, and branch control
        Op                 getOp()             const { return m_opSpec.op; }
        BranchCntrl        getBranchCtrl()     const { return m_brnch; }
        // true for madm or math.invm and math.rsqrtm
        bool               isMacro()           const{ return getOpSpec().isMacro(); }
        // execution width info
        ExecSize           getExecSize()       const { return m_execSize; }
        ChannelOffset      getChannelOffset() const { return m_chOff; }

        // returns if the instruction has a flag (condition) modifier
        bool               hasFlagModifier()   const { return m_flagModifier != FlagModifier::NONE; }
        FlagModifier       getFlagModifier()   const { return m_flagModifier; }

        // flag register is shared by predication and flag modfier
        const RegRef&      getFlagReg()        const { return m_flagReg; }

        const Operand&     getDestination()    const { return m_dst; }
        Operand&           getDestination()          { return m_dst; }
        const Operand&     getSource(size_t srcNum) const { return m_srcs[srcNum]; }
        Operand&           getSource(size_t srcNum)       { return m_srcs[srcNum]; }
        const Operand&     getSource(SourceIndex srcNum) const { return m_srcs[(int)srcNum]; }
        Operand&           getSource(SourceIndex srcNum)       { return m_srcs[(int)srcNum]; }

        unsigned           getSourceCount() const{ // BRC is a weird duck, everyone else is cool
            return getOp() != Op::BRC ? getOpSpec().getSourceCount() : getSourceCountBrc();
        }

        SendDescArg        getExtMsgDescriptor() const { return m_exDesc; }
        SendDescArg        getMsgDescriptor() const { return m_desc; }

        const InstOptSet&  getInstOpts() const { return m_instOpts; }
        bool               hasInstOpt(InstOpt opt) const { return m_instOpts.contains(opt); }
        void               removeInstOpt(InstOpt opt) { m_instOpts.remove(opt); }

        const Block     *getJIP() const { return m_srcs[0].getTargetBlock(); }
        const Block     *getUIP() const { return m_srcs[1].getTargetBlock(); }
        int              getID() const { return m_instID; }
        int32_t          getPC() const { return m_pc; }
        const char      *getComment() const { return m_comment; }
        bool             isBranching() const { return getOpSpec().isBranching(); }

        void             validate() const; // asserts on malformed IR
        std::string      str(Platform pltfm) const; // returns syntax of this instruction
    private:
        unsigned         getSourceCountBrc() const;

        const OpSpec&    m_opSpec; // information about the specific inst op
        MaskCtrl         m_maskCtrl; // (W) WrEn (write enable / NoMask)
        Predication      m_pred; // predication function (and logical sign)
        RegRef           m_flagReg; // shared by m_pred and m_flagModifier

        ExecSize         m_execSize;
        ChannelOffset    m_chOff;
        Operand          m_dst;
        Operand          m_srcs[3];
        union
        {
            struct
            {
                FlagModifier        m_flagModifier; // conditional-modifier function
            };
            BranchCntrl             m_brnch; // for certain branching instructions
            struct
            {
                struct SendDescArg  m_exDesc;
                struct SendDescArg  m_desc;
            };
        };
        InstOptSet       m_instOpts; // miscellaneous instruction attributes

        int              m_instID; // unique id for this instruction (unique in the kernel)

        int32_t          m_pc; // the encode/decode PC for this instruction
        Loc              m_instLoc; // source location info we keep this
                                    // separate from the PC since we
                                    // use both during assembly

        // e.g. for illegal instructions (decode errors)
        // memory is allocated by the Kernel's memory manager containing this
        // instruction (i.e. setter maintains this memory)
        const char      *m_comment;
    }; // class Instruction
} // namespace iga
#endif //_IGA_INSTRUCTION_HPP_
