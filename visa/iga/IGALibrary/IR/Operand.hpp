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

#ifndef _IGA_OPERAND_HPP_
#define _IGA_OPERAND_HPP_

#include "ImmVal.hpp"
#include "Types.hpp"

namespace iga {
class Block;

class Operand {
  public:
    enum class Kind {
        INVALID,   // an invalid or uninitialized operand
        DIRECT,    // direct register reference
        MACRO,     // madm or math.invm or math.rsqrtm
        INDIRECT,  // register-indriect access
        IMMEDIATE, // immediate value
        LABEL,     // block target (can be numeric label/m_immVal)
    };

    Operand()
        : m_kind(Kind::INVALID), m_lblBlock(nullptr), m_type(Type::INVALID) {}

    // direct destination constructor (for constants etc)
    Operand(
        DstModifier dstMod,
        RegName rType,
        const RegRef &reg,
        const Region::Horz &rgnHz,
        Type type)
        : m_lblBlock(nullptr) {
        setDirectDestination(dstMod, rType, reg, rgnHz, type);
    }

    // direct source constructor (for constants etc)
    Operand(
        SrcModifier srcMod,
        RegName rType,
        const RegRef &reg,
        const Region &rgn,
        Type type)
        : m_lblBlock(nullptr) {
        setDirectSource(srcMod, rType, reg, rgn, type);
    }

    // describes if the operand is direct (Operand::Kind::DIRECT),
    // indirect (Operand::Kind::INDIRECT) or immediate
    // (Operand::Kind::IMMEDIATE)
    Kind getKind() const { return m_kind; }

    DstModifier getDstModifier() const { return m_regOpDstMod; }
    SrcModifier getSrcModifier() const { return m_regOpSrcMod; }

    // Applies to Operand::Kind::DIRECT
    RegName getDirRegName() const { return m_regOpName; }
    const RegRef &getDirRegRef() const { return m_regOpReg; }

    // Applies to Operand::Kind::INDIRECT only
    const RegRef &getIndAddrReg() const { return m_regOpIndReg; }
    int16_t getIndImmAddr() const { return m_regOpIndOff; }

    // Applies to Operand::Kind::DIRECT and Operand::Kind::INDIRECT
    Region getRegion() const { return m_regOpRgn; }
    ImplAcc getImplAcc() const { return m_regImplAcc; }
    // Defined if the value is immediate
    const ImmVal getImmediateValue() const { return m_immValue; }
    // if this operand corresponds to a label, this is the target block
    // nullptr if we are using numeric labels
    const Block *getTargetBlock() const { return m_lblBlock; }

    // the operand type (as in :f, :d, ...)
    Type getType() const { return m_type; }

  public:
    // re-initializes this operand as an direct destination register operand
    void setDirectDestination(
        DstModifier dstMod,
        RegName rName,
        const RegRef &reg,
        const Region::Horz &rgnHz,
        Type type);
    // re-initializes this operand as a destination register operand using
    // an implicit accumulator access
    void setMacroDestination(
        DstModifier dstMod,
        RegName rName,
        const RegRef &reg,
        ImplAcc acc,
        Type type);
    // re-initializes this operand as an indirect destination register operand
    void setInidirectDestination(
        DstModifier dstMod,
        const RegRef &reg,
        int16_t immediateOffset,
        const Region::Horz &rgnHz,
        Type type);

    // re-initializes this operand as an immeidate value with a given type
    void setImmediateSource(const ImmVal &val, Type type);
    // re-initializes this operand as a direct source register
    void setDirectSource(
        SrcModifier srcMod,
        RegName rName,
        const RegRef &reg,
        const Region &rgn,
        Type type);
    // re-initializes this operand as a source register operand using
    // an implicit accumulator access
    void setMacroSource(
        SrcModifier srcMod,
        RegName rName,
        const RegRef &reg,
        ImplAcc acc,
        Type type);

    // re-initializes this operand as an indirect register operand
    void setInidirectSource(
        SrcModifier srcMod,
        const RegRef &reg,
        int16_t immediateOffset,
        const Region &rgn,
        Type type);
    // re-initializes this operand as an immediate branch target
    void setLabelSource(Block *blk, Type type);
    void setLabelSource(int32_t jipOrUip, Type type);
    // set sthe operand region
    void setRegion(const Region &rgn) { m_regOpRgn = rgn; }
    // sets the operand type
    void setType(Type type) { m_type = type; }

  private:
    Operand::Kind m_kind;

    union { // operand register/immediate value info
        struct { // a register (direct or indirect)
            union { // optional modifier (e.g. -r12, ~r12, (abs) (sat))
                SrcModifier m_regOpSrcMod;
                DstModifier m_regOpDstMod;
            };
            struct { // the actual register (GRF for Operand::Kind::INDIRECT)
                RegName m_regOpName; // r#, a#, null, ...
                union {
                    Region m_regOpRgn;    // <1> <8;8,1>
                    ImplAcc m_regImplAcc; // implicit accumulator (r13.acc2)
                };
            };
            union { // direct, macro, and indirect
                RegRef m_regOpReg; // direct operands
                struct {           // indirect operands
                    RegRef m_regOpIndReg;  // e.g. a0.4
                    int16_t m_regOpIndOff; // e.g. "16" in "r[a0.4 + 16]"
                };
            };
        };

        struct { // immediate operand (or label)
            // the literal value (for immediate data)
            // also set before labels are resolved (label is .s32)
            // for numeric labels the value is normalized as bytes from
            // the pre-increment PC of the instruction.  Hence,
            //  * Pre-BDW branches which use units of QWORDS in the
            //    encoding will be converted to bytes.
            //  * JMPI uses the pos-increment PC; we normalize that here
            //    as well to a pre-increment value.
            ImmVal m_immValue;
            // for resolved labels
            Block *m_lblBlock;
        };
    };

    union {
        // the operand type (e.g. :d, :f, etc...)
        Type m_type;

    };

  public:
    // useful constants (reusable operands to streamline codegen)
    static const Operand DST_REG_IP_D; // brd/brc use this
    static const Operand SRC_REG_IP_D;
    static const Operand DST_REG_IP_UD; // jmpi Dst and Src0
    static const Operand SRC_REG_IP_UD;
    static const Operand DST_REG_NULL_UD; // e.g. while.Dst
    static const Operand SRC_REG_NULL_UD;
}; // class Operand
} // namespace iga
#endif // _IGA_OPERAND_HPP_
