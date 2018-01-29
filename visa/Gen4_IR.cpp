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

#include "visa_igc_common_header.h"
#include "Common_ISA.h"
#include "Common_ISA_util.h"
#include "Common_ISA_framework.h"
#include "JitterDataStruct.h"
#include "VISAKernel.h"
#include "BuildCISAIR.h"
#include "Gen4_IR.hpp"

using namespace std;
using namespace vISA;

static const char* CondModStr[Mod_cond_undef] =
{
    "z",  // zero
    "e",  // equal
    "nz", // not zero
    "ne", // not equal
    "g",  // greater
    "ge", // greater or equal
    "l",  // less
    "le", // less or equal
    "o",  // overflow
    "r",  // round increment
    "u",  // unorder (NaN)
};

static const char* SrcModifierStr[Mod_src_undef] =
{
    "-",       // Mod_Minus
    "(abs)",   // Mod_Abs
    "-(abs)",  // Mod_Minus_Abs
    "-"        // Mod_Not (print as -)
};

G4_InstOptInfo InstOptInfo[] =
{
    {InstOpt_Align16, "Align16"}, 
    {InstOpt_M0, "M0"},
    {InstOpt_M4, "M4"},
    {InstOpt_M8, "M8"},
    {InstOpt_M12, "M12"},
    {InstOpt_M16, "M16"},
    {InstOpt_M20, "M20"},
    {InstOpt_M24, "M24"},
    {InstOpt_M28, "M28"},
    {InstOpt_Switch, "Switch"}, 
    {InstOpt_Atomic, "Atomic"}, 
    {InstOpt_NoDDChk, "NoDDChk"}, 
    {InstOpt_NoDDClr, "NoDDClr"}, 
    {InstOpt_WriteEnable, "NoMask"}, 
    {InstOpt_BreakPoint, "BreakPoint"}, 
    {InstOpt_EOT, "EOT"}, 
    {InstOpt_AccWrCtrl, "AccWrEn"},
    {InstOpt_Compacted, "Compacted"},
    {InstOpt_NoSrcDepSet, "NoSrcDepSet"},
    {InstOpt_NoPreempt, "NoPreempt"},
    {InstOpt_END, "END"}  
};

#define HANDLE_INST( op, nsrc, ndst, type, plat, attr ) \
    { G4_##op, #op, nsrc, ndst, type, plat, \
      attr },
#define HANDLE_NAME_INST( op, name, nsrc, ndst, type, plat, attr ) \
    { G4_##op, name, nsrc, ndst, type, plat, \
      attr },

G4_Inst_Info G4_Inst_Table[] = {
#include "G4Instruction.def"
};


static const char* getChannelEnableStr(ChannelEnable channel)
{
    switch (channel)
    {
    case NoChannelEnable:
        return "";
    case ChannelEnable_X:
        return "x";
    case ChannelEnable_Y:
        return "y";
    case ChannelEnable_XY:
        return "xy";
    case ChannelEnable_Z:
        return "z";
    case ChannelEnable_W:
        return "w";
    case ChannelEnable_ZW:
        return "zw";
    case ChannelEnable_XYZW:
        return "xyzw";
    default:
        MUST_BE_TRUE(false, "unsupported channel enable");
        return "";
    }
}

//global functions
uint8_t roundDownPow2( uint8_t n)
{
    uint8_t i = 1;
    while (n >= i) i <<= 1;
    return (i>>1);
}

bool isPow2( uint8_t n )
{
    uint8_t i = 1;
    while (n > i) i <<= 1;
    return ( n == i );
}

// check if type1 can be represented by type2
bool Is_Type_Included(G4_Type type1, G4_Type type2, const Options *opt)
{
    if( type1 == type2 )
    {
        return true;
    }

    // Float and Int types are never subtype of each other
    if (IS_TYPE_FLOAT_ALL(type1) ^ IS_TYPE_FLOAT_ALL(type2))
    {
        return false;
    }
    if (type1 == Type_F && type2 == Type_HF && getGenxPlatform() > GENX_BDW && opt->getOption(vISA_enableUnsafeCP_DF))
    {
        return true;
    }

    if( Operand_Type_Rank( type1 ) < Operand_Type_Rank( type2 ) )
    {
        if( ( IS_UNSIGNED_INT(type1) || type1 == Type_UV ) &&
            ( IS_UNSIGNED_INT(type2) || type2 == Type_UV ) )
        {
            return true;
        }
        else if( ( IS_SIGNED_INT(type1) || type1 == Type_V ) &&
            ( IS_SIGNED_INT(type2) || type2 == Type_V ) )
        {
            return true;
        }
        else if( ( type1 == Type_UB || type1 == Type_UW || type1 == Type_UV ) && IS_TYPE_INT( type2 ) )
        {
            return true;
        }
        else if( (type1 == Type_HF) && (type2 == Type_F) )
        {
            return true;
        }
    }
    return false;
}

/* Return the base rank for the input type ignoring the signed/unsigned
 * aspect of types.
 *    - Types of higher precision have higher ranks.
 *    - Floating types have higher precision than all integer types.
 */
short Operand_Type_Base_Rank( G4_Type type )
{
    short type_size = (short) G4_Type_Table[type].byteSize;
    short type_rank = type_size * 2;

    if( type == Type_V || type == Type_UV )
    {
        type_rank = (short) G4_Type_Table[Type_W].byteSize;
    }
    else if( type == Type_VF )
    {
        type_rank = (short) G4_Type_Table[Type_F].byteSize;
    }
    else if ( IS_TYPE_FLOAT_ALL(type) )
    {
        type_rank += 2;
    }

    return type_rank;
}

/* Return the rank for the input type.
 *    - Types of higher precision have higher ranks.
 *    - Floating types have higher precision than all integer types.
 *    - Unsigned types have a higher rank than a signed type with the same
 *      precision.
 */
short Operand_Type_Rank( G4_Type type )
{
    short type_rank = Operand_Type_Base_Rank( type );

    switch ( type ) {
        case Type_UB:
        case Type_UW:
        case Type_UD: {
            type_rank++;
            break;
        }
        default: {
            // No nothing.
            break;
        }
    }

    return type_rank;
}

const uint16_t G4_SendMsgDescriptor::FFLatency[] = {
    2,    //0: SFID_NULL
    2,    //1: Useless
    300,  //2: SFID_SAMPLER
    200,  //3: SFID_GATEWAY
    400,  //4: SFID_DP_READ, SFID_DP_DC2
    200,  //5: SFID_DP_WRITE
    50,   //6: SFID_URB
    50,   //7: SFID_SPAWNER
    50,   //8: SFID_VME
    60,   //9: SFID_DP_CC
    400,  //10: SFID_DP_DC
    50,   //11: SFID_DP_PI
    400,  //12: SFID_DP_DC1
    200,  //13: SFID_CRE
    200   //14: unknown, SFID_NUM
};

G4_SendMsgDescriptor::G4_SendMsgDescriptor( uint32_t fCtrl, uint32_t regs2rcv,
    uint32_t regs2snd, uint32_t fID, bool isEot, uint16_t extMsgLen,
    uint32_t extFCtrl, bool isRead, bool isWrite, G4_Operand *bti, G4_Operand *sti,
    IR_Builder& builder)
{
    // All unnamed bits should be passed with those control bits.
    // Otherwise, need to be set individually.
    desc.value = fCtrl;


    desc.layout.rspLength = regs2rcv;
    desc.layout.msgLength = regs2snd;

    extDesc.value = 0;
    extDesc.layout.funcID = fID;
    extDesc.layout.eot = isEot;
    extDesc.layout.extMsgLength = extMsgLen;
    extDesc.layout.extFuncCtrl = extFCtrl;

    readMsg = isRead;
    writeMsg = isWrite;

    m_bti = bti;
    m_sti = sti;
    m_extMsgDesc  =  NULL;

    if (m_bti && m_bti->isImm())
    {
        setBindingTableIdx((unsigned)m_bti->asImm()->getInt());
    }
    if (m_sti && m_sti->isImm())
    {
        setSamplerTableIdx((unsigned)m_sti->asImm()->getInt());
    }

    uint32_t totalMaxLength = builder.getMaxSendMessageLength();
    MUST_BE_TRUE(extDesc.layout.extMsgLength + desc.layout.msgLength < totalMaxLength, 
        "combined message length may not exceed the maximum");
  
    if (extDesc.layout.extMsgLength + desc.layout.msgLength >= 16)
    {
        MUST_BE_TRUE(!isEot, "cm_sends can't set eot if message length is greater than 16");
    }
}

G4_SendMsgDescriptor::G4_SendMsgDescriptor(uint32_t desc, uint32_t extDesc,
                                           bool isRead,
                                           bool isWrite,
                                           G4_Operand *bti,
                                           G4_Operand *sti)
                                           : readMsg(isRead), writeMsg(isWrite), m_sti(sti), m_bti(bti)
{
    this->desc.value = desc;
    this->extDesc.value = extDesc;

    m_extMsgDesc = NULL;
    if (bti && bti->isImm())
    {
        setBindingTableIdx((unsigned)bti->asImm()->getInt());
    }
    if (sti && sti->isImm())
    {
        setSamplerTableIdx((unsigned)sti->asImm()->getInt());
    }
}

const char* G4_SendMsgDescriptor::getDescType()
{
    // Return plain text string of type of msg, ie "oword read", "oword write",
    // "media rd", etc.
    G4_SendMsgDescriptor* msgDesc = this;
    unsigned int bits, category;

	bits = getMessageType();

    switch (msgDesc->getFuncId())
    {
    case SFID_SAMPLER: return "sampler"; break;
    case SFID_GATEWAY: return "gateway"; break;
    case SFID_DP_DC2:
        switch (bits)
        {
        case DC2_UNTYPED_SURFACE_READ:
            return "scaled untyped surface read";
        case DC2_A64_SCATTERED_READ:
            return "scaled A64 scatter read";
        case DC2_A64_UNTYPED_SURFACE_READ:
            return "scaled A64 untyped surface read";
        case DC2_BYTE_SCATTERED_READ:
            return "scaled byte scattered read";
        case DC2_UNTYPED_SURFACE_WRITE:
            return "scaled untyped surface write";
        case DC2_A64_UNTYPED_SURFACE_WRITE:
            return "scaled A64 untyped surface write";
        case DC2_A64_SCATTERED_WRITE:
            return "scaled A64 scattered write";
        case DC2_BYTE_SCATTERED_WRITE:
            return "scaled byte scattede write";
        default:
            return "unrecognized message";
        }
    case SFID_DP_WRITE:
        switch (bits)
        {
        case 0xc: return "render target write"; break;
        case 0xd: return "render target read"; break;
        default: return "reserved encoding used!";
        }
        break;
    case SFID_URB: return "urb"; break;
    case SFID_SPAWNER: return "thread spawner"; break;
    case SFID_VME: return "vme"; break;
    case SFID_DP_CC:
        switch (bits)
        {
        case 0x0: return "oword block read"; break;
        case 0x1: return "unaligned oword block read"; break;
        case 0x2: return "oword dual block read"; break;
        case 0x3: return "dword scattered read"; break;
        default: return "reserved encoding used!";
            break;
        }
    case SFID_DP_DC:
        category = (msgDesc->getFuncCtrl() >> 18) & 0x1;
        if (category == 0)
        {
            // legacy data port
            bool hword = (msgDesc->getFuncCtrl() >> 13) & 0x1;
            switch (bits)
            {
            case 0x0: return hword ? "hword block read" : "oword block read";
            case 0x1: return hword ? "hword aligned block read" : "unaligned oword block read";
            case 0x2: return "oword dual block read";
            case 0x3: return "dword scattered read";
            case 0x4: return "byte scattered read";
            case 0x7: return "memory fence";
            case 0x8: return hword ? "hword block write" : "oword block write";
            case 0x9: return "hword aligned block write";
            case 0xa: return "oword dual block write";
            case 0xb: return "dword scattered write";
            case 0xc: return "byte scattered write";
            default: return "reserved encoding used!";
            }
        }
        else
        {
            // scratch
            bits = (msgDesc->getFuncCtrl() >> 17) & 0x1;

            if (bits == 0)
                return "scratch read";
            else
                return "scratch write";
        }
        break;
    case SFID_DP_PI: return "dp_pi"; break;
    case SFID_DP_DC1:
        switch (bits)
        {
        case 0x0: return "transpose read"; break;
        case 0x1: return "untyped surface read"; break;
        case 0x2: return "untyped atomic operation"; break;
        case 0x3: return "untyped atomic operation simd4x2"; break;
        case 0x4: return "media block read"; break;
        case 0x5: return "typed surface read"; break;
        case 0x6: return "typed atomic operation"; break;
        case 0x7: return "typed atomic operation simd4x2"; break;
        case 0x8: return "untyped atomic float add"; break;
        case 0x9: return "untyped surface write"; break;
        case 0xa: return "media block write (non-iecp)"; break;
        case 0xb: return "atomic counter operation"; break;
        case 0xc: return "atomic counter operation simd4x2"; break;
        case 0xd: return "typed surface write"; break;
        case 0x10: return "a64 scattered read"; break;
        case 0x11: return "a64 untyped surface read"; break;
        case 0x12: return "a64 untyped atomic operation"; break;
        case 0x13: return "a64 untyped atomic operation simd4x2"; break;
        case 0x14: return "a64 block read"; break;
        case 0x15: return "a64 block write"; break;
        case 0x18: return "a64 untyped atomic float add"; break;
        case 0x19: return "a64 untyped surface write"; break;
        case 0x1a: return "a64 scattered write"; break;
        default: return "reserved encoding used!";
        }
        break;
    case SFID_CRE: return "cre"; break;
    default: return "--";
    }
    return NULL;
}

bool G4_SendMsgDescriptor::isSLMMessage() const 
{
    if (getFuncId() == SFID_DP_DC2)
    {
        uint32_t msgType = getMessageType();
        if ((msgType == DC2_UNTYPED_SURFACE_WRITE || msgType == DC2_BYTE_SCATTERED_WRITE) &&
            (getFuncCtrl() & 0x80))
        {
            return true;
        }
    }
    if (m_bti && m_bti->isImm() && m_bti->asImm()->getInt() == SLMIndex)
    {
        return true;
    }
   
    return false;
}

G4_INST::G4_INST(USE_DEF_ALLOCATOR& allocator,
    std::vector<G4_INST*>& instList,
    G4_Predicate* prd,
    G4_opcode o,
    G4_CondMod* m,
    bool s,
    unsigned char size,
    G4_DstRegRegion* d,
    G4_Operand* s0,
    G4_Operand* s1,
    unsigned int opt) :
    op(o), dst(d), predicate(prd), mod(m), option(opt), msgDesc(NULL),
    local_id(0),
    srcCISAoff(-1),
    sat(s), scratch(false),
    mIsIndirectJmpTarget(false), evenlySplitInst(false),
    execSize(size), bin(0), mLabel(NULL),
    global_id(-1),
    useInstList(allocator),
    defInstList(allocator),
    location(NULL)
{
    srcs[0] = s0;
    srcs[1] = s1;
    srcs[2] = nullptr;
    srcs[3] = nullptr;

    m_Transient.ra.lexicalId = 0;
    dead = false;
    implAccSrc = nullptr;
    implAccDst = nullptr;
    isSpillOrFill = false;

    resetRightBound(dst);
    resetRightBound(s0);
    resetRightBound(s1);
    computeRightBound((G4_Operand*)predicate);
    computeRightBound((G4_Operand*)mod);

    associateOpndWithInst(dst, this);
    associateOpndWithInst(s0, this);
    associateOpndWithInst(s1, this);

    id = (int)instList.size();
    instList.push_back(this);
}

G4_INST::G4_INST(
    USE_DEF_ALLOCATOR& allocator,
    std::vector<G4_INST*>& instList,
    G4_Predicate* prd,
    G4_opcode o,
    G4_CondMod* m,
    bool s,
    unsigned char size,
    G4_DstRegRegion* d,
    G4_Operand* s0,
    G4_Operand* s1,
    G4_Operand* s2,
    unsigned int opt) :
    op(o), dst(d), predicate(prd), mod(m), option(opt), msgDesc(NULL),
    local_id(0),
    global_id(-1),
    srcCISAoff(-1),
    sat(s), scratch(false),
    mIsIndirectJmpTarget(false), evenlySplitInst(false),
    execSize(size), bin(0), mLabel(NULL),
    useInstList(allocator),
    defInstList(allocator),
    location(NULL)
{
    srcs[0] = s0;
    srcs[1] = s1;
    srcs[2] = s2;
    srcs[3] = nullptr;

    m_Transient.ra.lexicalId = 0;
    dead = false;
    implAccSrc = nullptr;
    implAccDst = nullptr;
    isSpillOrFill = false;

    resetRightBound(dst);
    resetRightBound(s0);
    resetRightBound(s1);
    resetRightBound(s2);
    computeRightBound((G4_Operand*)predicate);
    computeRightBound((G4_Operand*)mod);

    associateOpndWithInst(dst, this);
    associateOpndWithInst(s0, this);
    associateOpndWithInst(s1, this);
    associateOpndWithInst(s2, this);
    associateOpndWithInst((G4_Operand*)predicate, this);
    associateOpndWithInst((G4_Operand*)mod, this);

    id = (int)instList.size();
    instList.push_back(this);
}

void G4_INST::setOpcode(G4_opcode opcd)
{
    MUST_BE_TRUE(opcd <= G4_NUM_OPCODE &&
        (G4_Inst_Table[op].instType == G4_Inst_Table[opcd].instType ||
        G4_Inst_Table[opcd].instType == InstTypeMov ||
        (
        (G4_Inst_Table[op].instType == InstTypeMov ||
        G4_Inst_Table[op].instType == InstTypeArith ||
        G4_Inst_Table[op].instType == InstTypeLogic ||
        G4_Inst_Table[op].instType == InstTypePseudoLogic ||
        G4_Inst_Table[op].instType == InstTypeVector) &&

        (G4_Inst_Table[opcd].instType == InstTypeMov ||
        G4_Inst_Table[opcd].instType == InstTypeArith ||
        G4_Inst_Table[opcd].instType == InstTypeLogic ||
        G4_Inst_Table[opcd].instType == InstTypeVector)
        ) ||
        opcd == G4_label),
        "setOpcode would change the intruction class, which is illegal.");

    bool resetBounds = false;

    if (op != opcd)
    {
        resetBounds = true;
    }

    op = opcd;

    if (resetBounds)
    {
        resetRightBound(dst);
        resetRightBound(srcs[0]);
        resetRightBound(srcs[1]);
        resetRightBound(srcs[2]);
        resetRightBound((G4_Operand*)predicate);
        resetRightBound((G4_Operand*)mod);
        resetRightBound((G4_Operand*)implAccDst);
        resetRightBound((G4_Operand*)implAccSrc);
    }
}

void G4_INST::setExecSize(unsigned char s)
{
    bool resetBounds = false;

    if (execSize != s)
    {
        resetBounds = true;
    }

    execSize = s;

    if (resetBounds)
    {
        resetRightBound(dst);
        resetRightBound(srcs[0]);
        resetRightBound(srcs[1]);
        resetRightBound(srcs[2]);
        resetRightBound((G4_Operand*)predicate);
        resetRightBound((G4_Operand*)mod);
        resetRightBound((G4_Operand*)implAccDst);
        resetRightBound((G4_Operand*)implAccSrc);
    }
}

//
// We assume no mixed int and float source type, but mixed HF and F is ok
//
G4_Type G4_INST::getExecType() const
{
    G4_Type execType = Type_W;

    // special handling for int divide, as it supports D/UD sources only, while
    // vISA DIV allows B/W types
    // FIXME: if there are more instructions like this, we may need to reorder fixDstAlignment()
    // so that it happens after all sources are fixed and we can get the correct execution type
    if (isMath() && asMathInst()->isMathIntDiv())
    {
        return Type_D;
    }

    for (unsigned i = 0; i < G4_MAX_SRCS; i++)
    {
        if (srcs[i] != NULL)
        {
            G4_Type srcType = srcs[i]->getType();
            if( G4_Type_Table[srcType].byteSize >= G4_Type_Table[execType].byteSize )
            {
                if (IS_DTYPE(srcType))
                {
                    execType = Type_D;
                }
                else if (IS_QTYPE(srcType))
                {
                    execType = Type_Q;
                }
                else if (srcType == Type_HF)
                {
                    execType = Type_HF;
                }
                else if (IS_FTYPE(srcType))
                {
                    execType = Type_F;
                }
                else if (IS_DFTYPE(srcType))
                {
                    execType = srcType;
                }
            }
        }
    }

    // int <-> HF conversion requires exec type to be dword
    // we don't consider Q<->HF since there are special checks in fixMov() for them
    if (dst)
    {
        G4_Type dstType = dst->getType();
        if (IS_HFTYPE(dstType) && (IS_TYPE_INT(execType) && !IS_QTYPE(execType)))
        {
            execType = Type_D;
        }
        else if (IS_HFTYPE(execType) && (IS_TYPE_INT(dstType) && !IS_QTYPE(dstType)))
        {
            execType = Type_F;
        }
    }

    return execType;
}

// V and VF are treated differently here from the above function
// FIXME: Why do we need two functions???
G4_Type G4_INST::getExecType2() const
{
    G4_Type execType = Type_W;

    // special handling for int divide, as it supports D/UD sources only, while
    // vISA DIV allows B/W types
    if (isMath() && asMathInst()->isMathIntDiv())
    {
        return Type_D;
    }

    for (unsigned i = 0; i < G4_MAX_SRCS; i++)
    {
        if (srcs[i] == NULL)
        {
            continue;
        }
        G4_Type srcType = srcs[i]->getType();
        if( srcType == Type_HF &&
            G4_Type_Table[srcType].byteSize >= G4_Type_Table[execType].byteSize &&
            !IS_DFTYPE(execType) && !IS_FTYPE(execType) )
        {
            execType = Type_HF;
            break;
        }
        else if (srcType == Type_V)
        {
            execType = Type_V;
            break;
        }
        else if (srcType == Type_UV)
        {
            execType = Type_UV;
            break;
        }
        else if (IS_DFTYPE(srcType) && !IS_DFTYPE(execType))
        {
            execType = srcs[i]->getType();
            break;
        }
        else if( (IS_FTYPE(srcType) || srcType == Type_VF) &&
            !IS_DFTYPE(execType) && !IS_FTYPE(execType) )
        {
            execType = Type_F;
        }
        else if( IS_DTYPE(srcType) &&
            G4_Type_Table[srcType].byteSize >= G4_Type_Table[execType].byteSize &&
            !IS_DFTYPE(execType) && !IS_FTYPE(execType) )
        {
            execType = Type_D;
        }
        else if( IS_QTYPE(srcType) &&
            G4_Type_Table[srcType].byteSize >= G4_Type_Table[execType].byteSize &&
            !IS_DFTYPE(execType) && !IS_FTYPE(execType) )
        {
            execType = Type_Q;
        }
    }

    // int <-> HF conversion requires exec type to be dword
    // we don't consider Q<->HF since there are special checks in fixMov() for them
    if (dst)
    {
        G4_Type dstType = dst->getType();
        if (IS_HFTYPE(dstType) && (IS_TYPE_INT(execType) && !IS_QTYPE(execType)))
        {
            execType = Type_D;
        }
        else if (IS_HFTYPE(execType) && (IS_TYPE_INT(dstType) && !IS_QTYPE(dstType)))
        {
            execType = Type_F;
        }
    }

    return execType;
}

uint16_t G4_INST::getMaskOffset()
{
    unsigned maskOption = (this->getOption() & InstOpt_QuarterMasks);
    switch(maskOption)
    {
    case InstOpt_NoOpt:
        return 0;
    case InstOpt_M0:
        return 0;
    case InstOpt_M4:
        return 4;
    case InstOpt_M8:
        return 8;
    case InstOpt_M12:
        return 12;
    case InstOpt_M16:
        return 16;
    case InstOpt_M20:
        return 20;
    case InstOpt_M24:
        return 24;
    case InstOpt_M28:
        return 28;
    default:
        MUST_BE_TRUE( 0, "Incorrect instruction execution mask" );
        return 0;
    }
}

void G4_INST::removeAllDefs()
{
    while (!defInstList.empty())
    {
        auto&& item = defInstList.front();
        defInstList.pop_front();

        G4_INST *def = item.first;
        for (auto I = def->use_begin(); I != def->use_end(); /*empty*/)
        {
            if (I->first == this && I->second == item.second)
            {
                I = def->useInstList.erase(I);
            }
            else
            {
                ++I;
            }
        }
    }
}

void G4_INST::removeAllUses()
{
    while (!useInstList.empty())
    {
        auto&& item = useInstList.front();
        useInstList.pop_front();

        G4_INST *user = item.first;
        for (auto I = user->def_begin(); I != user->def_end(); /*empty*/)
        {
            if (I->first == this && I->second == item.second)
            {
                I = user->defInstList.erase(I);
            }
            else
            {
                ++I;
            }
        }
    }
}

void G4_INST::removeDefUse( Gen4_Operand_Number opndNum )
{
    DEF_EDGE_LIST_ITER iter = this->defInstList.begin();
    while( iter != this->defInstList.end() )
    {
        if( (*iter).second == opndNum )
        {
            USE_EDGE_LIST_ITER iter1 = (*iter).first->useInstList.begin();
            while( iter1 != (*iter).first->useInstList.end() ){
                if( (*iter1).first == this && (*iter1).second == opndNum ){
                    USE_EDGE_LIST_ITER curr_iter1 = iter1++;
                    (*iter).first->useInstList.erase( curr_iter1 );
                }else{
                    ++iter1;
                }
            }
            DEF_EDGE_LIST_ITER curr_iter = iter++;
            this->defInstList.erase( curr_iter );
        }else{
            ++iter;
        }
    }
}

USE_EDGE_LIST_ITER G4_INST::eraseUse(USE_EDGE_LIST_ITER iter)
{
    G4_INST *useInst = iter->first;
    for (auto I = useInst->def_begin(); I != useInst->def_end(); /*empty*/)
    {
        if (I->first == this && I->second == iter->second)
            I = useInst->defInstList.erase(I);
        else
            ++I;
    }
    return useInstList.erase(iter);
}

// Transfer definitions used in this[opndNum1] to definitions used in
// inst2[opndNum2] and update definitions's def-use chain accordingly.
void G4_INST::transferDef( G4_INST *inst2, Gen4_Operand_Number opndNum1, Gen4_Operand_Number opndNum2 )
{
    DEF_EDGE_LIST_ITER iter = this->defInstList.begin();
    while( iter != this->defInstList.end() )
    {
        if( (*iter).second == opndNum1 )
        {
            inst2->defInstList.push_back( std::pair<G4_INST*, Gen4_Operand_Number>((*iter).first, opndNum2) );
            USE_EDGE_LIST_ITER iter1 = (*iter).first->useInstList.begin();
            while( iter1 != (*iter).first->useInstList.end() ){
                if( (*iter1).second == opndNum1 && (*iter1).first == this ){
                    USE_EDGE_LIST_ITER curr_iter1 = iter1++;
                    (*iter).first->useInstList.erase( curr_iter1 );
                    (*iter).first->useInstList.push_back( std::pair<G4_INST*, Gen4_Operand_Number>(inst2, opndNum2) );
                }else{
                    ++iter1;
                }
            }
            DEF_EDGE_LIST_ITER curr_iter = iter++;
            this->defInstList.erase( curr_iter );
        }else{
            ++iter;
        }
    }
}

// This copies, from this definition's source opndNum1, all of its defintions to
// inst2's source opndNum2. This is used for example by copy propagation to copy
// the def-use link of the move to the use instruction.
//
// If 'checked' is true, then this only copies those effective defs to inst2.
//
void G4_INST::copyDef(G4_INST *inst2, Gen4_Operand_Number opndNum1,
                      Gen4_Operand_Number opndNum2, bool checked)
{
    for (auto I = def_begin(); I != def_end(); ++I)
    {
        if (I->second == opndNum1)
        {
            // If checked is enabled, then compare inst2[opndNum] with this
            // definition. Skip if this is not an effective use.
            if (checked)
            {
                G4_Operand *use = inst2->getOperand(opndNum2);
                ASSERT_USER(use, "null operand unexpected");
                G4_Operand *dst = I->first->getOperand(Opnd_dst);
                G4_Operand *condMod = I->first->getOperand(Opnd_condMod);
                if ((dst && use->compareOperand(dst) != Rel_disjoint) ||
                    (condMod && use->compareOperand(condMod) != Rel_disjoint))
                {
                    // OK
                }
                else
                {
                    // Skip to the next def.
                    continue;
                }
            }
            I->first->addDefUse(inst2, opndNum2);
        }
    }
    inst2->defInstList.unique();
}

/// Copy this instruction's defs to inst2.
void G4_INST::copyDefsTo(G4_INST *inst2, bool checked)
{
    if (this == inst2)
        return;

    for (auto I = def_begin(), E = def_end(); I != E; ++I)
    {
        G4_Operand *use = inst2->getOperand(I->second);
        // Copy when the corresponding use operand is not null.
        if (!use)
           continue;

        if (checked)
        {
            G4_Operand *dst = I->first->getOperand(Opnd_dst);
            G4_Operand *condMod = I->first->getOperand(Opnd_condMod);
            G4_Operand* implicitAccDef = I->first->getImplAccDst();
            if ((dst && use->compareOperand(dst) != Rel_disjoint) ||
                (condMod && use->compareOperand(condMod) != Rel_disjoint) ||
                (implicitAccDef && use->compareOperand(implicitAccDef) != Rel_disjoint))
            {
                // OK
            }
            else
            {
                // Skip to the next def.
                continue;
            }
        }

        // inst2[I->second] is defined by I->first.
        I->first->addDefUse(inst2, I->second);
    }
}

/// Copy this instruction's uses to inst2.
void G4_INST::copyUsesTo(G4_INST *inst2, bool checked)
{
    if (this == inst2)
        return;

    for (auto I = use_begin(), E = use_end(); I != E; ++I)
    {
        if (checked)
        {
            G4_Operand *use = I->first->getOperand(I->second);
            ASSERT_USER(use, "null operand unexpected");

            G4_Operand *dst = inst2->getOperand(Opnd_dst);
            G4_Operand *condMod = inst2->getOperand(Opnd_condMod);
            G4_Operand *implicitAccDef = inst2->getImplAccDst();
            if ((dst && use->compareOperand(dst) != Rel_disjoint) ||
                (condMod && use->compareOperand(condMod) != Rel_disjoint) ||
                (implicitAccDef && use->compareOperand(implicitAccDef) != Rel_disjoint))
            {
                // OK
            }
            else
            {
                // Skip to the next use.
                continue;
            }
        }

        // I->first[I->second] is defined by inst2.
        inst2->addDefUse(I->first, I->second);
    }
}

// This transfers this instructions' useInstList to inst2's,
// and update each use's defInstList to point to inst2.
// this instruction's use is destroyed in the process.
// if keepExisting is true, it will preserve inst2's existing uses.
void G4_INST::transferUse( G4_INST *inst2, bool keepExisting)
{
    if (this == inst2)
    {
        return;
    }

    if (!keepExisting)
    {
        for (auto iter = inst2->useInstList.begin(), iterEnd = inst2->useInstList.end();
            iter != iterEnd;
            ++iter)
        {
            G4_INST* useInst = (*iter).first;
            auto defIter = useInst->defInstList.begin(), defIterEnd = useInst->defInstList.end();
            while (defIter != defIterEnd)
            {
                G4_INST* defInst = (*iter).first;
                if (defInst == inst2)
                {
                    defIter = useInst->defInstList.erase(defIter);
                }
                else
                {
                    ++defIter;
                }
            }
        }
        inst2->useInstList.clear();
    }

    USE_EDGE_LIST_ITER tmp_iter2 = this->useInstList.begin();
    while( tmp_iter2 != this->useInstList.end() )
    {
        inst2->useInstList.push_back(*tmp_iter2);
        DEF_EDGE_LIST_ITER tmp_iter1 = (*tmp_iter2).first->defInstList.begin();
        while( tmp_iter1 != (*tmp_iter2).first->defInstList.end() ){
            if( (*tmp_iter1).first == this ){
                (*tmp_iter2).first->defInstList.push_back( std::pair<G4_INST*, Gen4_Operand_Number>(inst2, (*tmp_iter1).second) );
                DEF_EDGE_LIST_ITER curr_iter = tmp_iter1++;
                (*tmp_iter2).first->defInstList.erase( curr_iter );

            }else{
                ++tmp_iter1;
            }
        }
        tmp_iter2++;
    }
    this->useInstList.clear();
}

void G4_INST::removeUseOfInst( )
{
    for (auto iter = defInstList.begin(); iter != defInstList.end(); ++iter)
    {
        auto iter1 = (*iter).first->useInstList.begin();
        while (iter1 != (*iter).first->useInstList.end()){
            if ((*iter1).first == this)
            {
                USE_EDGE_LIST_ITER curr_iter1 = iter1++;
                (*iter).first->useInstList.erase(curr_iter1);
            }
            else
            {
                ++iter1;
            }
        }
    }
}

// remove the faked def-instructions in def list, which is resulted from instruction spliting
void G4_INST::trimDefInstList()
{
    // trim def list
    DEF_EDGE_LIST_ITER iter = this->defInstList.begin();
    // since ACC is only exposed in ARCTAN intrinsic translation, there is no instruction split with ACC
    while( iter != this->defInstList.end() )
    {
        G4_Operand *src = this->getOperand( (*iter).second );

        if (src == nullptr)
        {
            // it's possible the source is entirely gone (e.g., predicate removed)
            iter = this->defInstList.erase(iter);
            continue;
        }
        G4_CmpRelation rel = Rel_undef;
        if( src->isFlag() )
        {
            if( (*iter).first->getCondMod() )
            {
                rel = src->compareOperand( (*iter).first->getCondMod() );
            }
            else if( (*iter).first->getDst() )
            {
                if( (*iter).first->hasNULLDst() && (*iter).first->hasNULLDst() )
                {
                    rel = Rel_disjoint;
                }
                else
                {
                    rel = src->compareOperand( (*iter).first->getDst() );
                }
            }
        }
        else
        {
            rel = src->compareOperand( (*iter).first->getDst() );
        }

        if( rel == Rel_disjoint )
        {
            // remove this def-use
            // assumption: no duplicate def-use info
            USE_EDGE_LIST_ITER useIter = (*iter).first->useInstList.begin();
            while( useIter != (*iter).first->useInstList.end() )
            {
                if( (*useIter).first == this && (*useIter).second == Opnd_src2 )
                {
                    (*iter).first->useInstList.erase( useIter );
                    break;
                }
                useIter++;
            }
            DEF_EDGE_LIST_ITER tmpIter = iter;
            iter++;
            this->defInstList.erase( tmpIter );
            continue;
        }
        iter++;
    }
}

#ifdef _DEBUG
static void printDefUseImpl(std::ostream &os, G4_INST *def, G4_INST *use,
                            Gen4_Operand_Number pos)
{
    os << "\n  def: ";
    def->emit(os);
    os << "\n user: ";
    use->emit(os);
    os << "\n opnd: ";
    use->getOperand(pos)->emit(os);
    os << "\n";
}
#endif

void G4_INST::dumpDefUse()
{
#if _DEBUG
    std::cerr << "\n------------ defs ------------\n";
    for (auto&& UD : defInstList)
    {
        printDefUseImpl(std::cerr, UD.first, this, UD.second);
    }
    std::cerr << "\n------------ uses ------------\n";
    for (auto&& DU : useInstList)
    {
        printDefUseImpl(std::cerr, this, DU.first, DU.second);
    }
#endif
}

namespace {
    // Customized def-use iterator comparison. Do not compare itself
    // but the content it is pointing to.
    struct def_less
    {
        bool operator()(DEF_EDGE_LIST_ITER a, DEF_EDGE_LIST_ITER b)
        {
            if (a->first < b->first)
            {
                return true;
            }
            else if ((a->first == b->first) && (a->second < b->second))
            {
                return true;
            }
            return false;
        }
    };
}

G4_INST *G4_INST::getSingleDef(Gen4_Operand_Number opndNum, bool MakeUnique)
{
    if (MakeUnique)
    {
        std::set<DEF_EDGE_LIST_ITER, def_less> found;
        for (auto I = def_begin(); I != def_end(); /* empty */)
        {
            if (!found.insert(I).second)
            {
                I = defInstList.erase(I);
            }
            else
            {
                ++I;
            }
        }
    }

    G4_INST *def = 0;
    unsigned def_count = 0;
    for (auto I = def_begin(), E = def_end(); I != E; ++I)
    {
        if (I->second == opndNum)
        {
            if (++def_count > 1) return 0;
            def = I->first;
        }
    }

    return def;
}

// add def-use between this instruction <--> inst[srcPos]
// Note that this function does not check for duplicates
void G4_INST::addDefUse(G4_INST* inst, Gen4_Operand_Number srcPos)
{
    MUST_BE_TRUE(srcPos == Opnd_dst || srcPos == Opnd_src0 || srcPos == Opnd_src1 ||
        srcPos == Opnd_src2 || srcPos == Opnd_src3 || srcPos == Opnd_pred ||
        srcPos == Opnd_implAccSrc, "unexpected operand number");
    this->useInstList.push_back(std::pair<G4_INST*, Gen4_Operand_Number>(inst, srcPos));
    inst->defInstList.push_back(std::pair<G4_INST*, Gen4_Operand_Number>(this, srcPos));
}

// find the ACC use inst of this instruction and trim their def chains
// this is called when some ACC def instruction is split into two instructions.
void G4_INST::trimACCDefUse()
{
    // trim def list
    USE_EDGE_LIST_ITER iter = this->useInstList.begin();
    // since ACC is only exposed in ARCTAN intrinsic translation, there is no instruction split with ACC
    while (iter != this->useInstList.end())
    {
        if ((*iter).second == Opnd_implAccSrc)
        {
            (*iter).first->trimDefInstList();
        }
        iter++;
    }
}
// exchange def/use info of src0 and src1 after they are swapped.
void G4_INST::swapDefUse()
{
    DEF_EDGE_LIST_ITER iter = this->defInstList.begin();
    // since ACC is only exposed in ARCTAN intrinsic translation, there is no instruction split with ACC
    while (iter != this->defInstList.end())
    {
        if ((*iter).second == Opnd_src1)
        {
            (*iter).second = Opnd_src0;
        }
        else if ((*iter).second == Opnd_src0)
        {
            (*iter).second = Opnd_src1;
        }
        else
        {
            iter++;
            continue;
        }
        // change uselist of def inst
        USE_EDGE_LIST_ITER useIter = (*iter).first->useInstList.begin();
        for (; useIter != (*iter).first->useInstList.end(); useIter++)
        {
            if ((*useIter).first == this)
            {
                if ((*useIter).second == Opnd_src1)
                {
                    (*useIter).second = Opnd_src0;
                }
                else if ((*useIter).second == Opnd_src0)
                {
                    (*useIter).second = Opnd_src1;
                }
            }
        }
        iter++;
    }
}

// returns true if inst is a commutable binary instruction and its two sources can be swapped
bool G4_INST::canSwapSource() const
{
    if (getNumSrc() != 2)
    {
        return false;
    }

    if (!INST_COMMUTATIVE(opcode()))
    {
        return false;
    }

    G4_Operand* src0 = getSrc(0);
    G4_Operand* src1 = getSrc(1);
    // src1 restrictions: no ARF, no VXH
    if (src0->isSrcRegRegion())
    {
        G4_SrcRegRegion* src0Region = src0->asSrcRegRegion();
        if (src0Region->isAreg() || src0Region->getRegion()->isRegionWH())
        {
            return false;
        }
    }

    // src0 restrictions: no Imm
    if (src1->isImm() || src1->isAddrExp())
    {
        return false;
    }

    // special check for mul: don't put DW on src1
    if (opcode() == G4_mul)
    {
        if (IS_DTYPE(src0->getType()) && !IS_DTYPE(src1->getType()))
        {
            return false;
        }
    }

    return true;
}
// fix src2 def/use to implicitSrc def/use
void G4_INST::fixMACSrc2DefUse()
{
    if( op != G4_mac )
    {
        return;
    }
    for( DEF_EDGE_LIST_ITER iter = this->defInstList.begin();
        iter != this->defInstList.end();
        iter++ )
    {
        if( (*iter).second == Opnd_src2 )
        {
            (*iter).second = Opnd_implAccSrc;
            G4_INST* defInst = (*iter).first;
            for( USE_EDGE_LIST_ITER useIter = defInst->useInstList.begin();
                useIter != defInst->useInstList.end();
                ++useIter )
            {
                if( ( (*useIter).first == this ) &&
                    ( (*useIter).second == Opnd_src2 ) )
                {
                    (*useIter).second = Opnd_implAccSrc;
                    break;
                }
            }
            break;
        }
    }
}

// a raw move is a move with
// -- no saturation or src modifiers
// -- same dst and src type
// -- no conditional modifier (predicate is ok)
bool G4_INST::isRawMov()
{
    return op == G4_mov && !sat && dst->getType() == srcs[0]->getType() &&
        getCondMod() == NULL &&
        (srcs[0]->isImm() ||
        (srcs[0]->isSrcRegRegion() && srcs[0]->asSrcRegRegion()->getModifier() == Mod_src_undef));
}
bool G4_INST::hasACCSrc()
{
    if( implAccSrc ||
        ( srcs[0] && srcs[0]->isSrcRegRegion() && srcs[0]->asSrcRegRegion()->isAccReg() ) )
    {
        return true;
    }
    return false;
}

// check if acc is possibly used by this instruction
bool G4_INST::hasACCOpnd()
{
    return ( isAccWrCtrlInst() ||
        implAccSrc ||
        implAccDst ||
        (op == G4_mulh &&
        IS_DTYPE(srcs[0]->getType()) && IS_DTYPE(srcs[1]->getType())) ||
        (dst && dst->isAccReg()) ||
        (srcs[0] && srcs[0]->isAccReg()) ||
        (srcs[1] && srcs[1]->isAccReg()) ||
        (srcs[2] && srcs[2]->isAccReg()) );
}

G4_Type G4_INST::getOpExecType( int& extypesize )
{
    G4_Type extype;
    if( isRawMov() )
    {
        extype = srcs[0]->getType();
    }
    else
    {
        extype = getExecType2();
    }
    if(IS_VINTTYPE(extype))
    {
        extypesize = G4_GRF_REG_NBYTES/2;
    }
    else if(IS_VFTYPE(extype))
    {
        extypesize = G4_GRF_REG_NBYTES;
    }
    else
    {
        extypesize = G4_Type_Table[extype].byteSize;
    }

    return extype;
}

G4_INST::MovType getMovType(G4_Type dstTy, G4_Type srcTy, G4_SrcModifier srcMod) {
    // COPY when dst & src types are the same.
    if (dstTy == srcTy)
        return G4_INST::Copy;

    bool dstIsFP = IS_TYPE_FLOAT_ALL(dstTy);
    bool srcIsFP = IS_TYPE_FLOAT_ALL(srcTy);

    // If dst & src are not both FPs or both Integers, that MOV must be
    // conversions from Integer to FP or vice versa.
    if (dstIsFP != srcIsFP) {
        if (dstIsFP)
            return G4_INST::IntToFP;

        ASSERT_USER(srcIsFP, "Unexpected source type!");
        return G4_INST::FPToInt;
    }

    // If they are both FPs, that MOV must be either up or down conversion.
    // Note it could not be a COPY as dst & src are different here.
    if (dstIsFP) {
        ASSERT_USER(srcIsFP, "Unexpected source type!");

        // TODO: Do we need to treat 'vf' differently?

        if (G4_Type_Table[srcTy].byteSize < G4_Type_Table[dstTy].byteSize)
            return G4_INST::FPUpConv;

        ASSERT_USER(G4_Type_Table[srcTy].byteSize >
                    G4_Type_Table[dstTy].byteSize,
                    "Unexpected FP source and destination type sizes!");
        return G4_INST::FPDownConv;
    }

    // They are both Integers. The destination signedness is ignored here to
    // detect the mov type as it really does not matter without saturation nor
    // condition modifier.

    ASSERT_USER(!IS_VINTTYPE(dstTy),
                "Unexpected immediate types are used as dst type!");

    // Always treat 'v' as SExt as they will always be extended even for
    // BYTE-sized types.
    if (srcTy == Type_V)
        return G4_INST::SExt;

    // Always treat 'uv' as SExt as they will always be extended even for
    // BYTE-sized types.
    if (srcTy == Type_UV)
        return G4_INST::ZExt;

    // Treat that mov as truncation.
    if (G4_Type_Table[srcTy].byteSize > G4_Type_Table[dstTy].byteSize)
        return G4_INST::Trunc;

    // Treat that mov as sign extend or zero extend based on the signedness of
    // the source type only.
    if (G4_Type_Table[srcTy].byteSize < G4_Type_Table[dstTy].byteSize) {
        if (IS_SIGNED_INT(srcTy)) {
            // Treat ABS as zero-extenstion.
            if (srcMod == Mod_Abs)
                return G4_INST::ZExt;
            return G4_INST::SExt;
        }
        return G4_INST::ZExt;
    }

    // Otherwise, treat it as COPY they are the same in bit size.
    // Treat ABS as zero-extenstion.
    if (IS_SIGNED_INT(srcTy) && srcMod == Mod_Abs)
        return G4_INST::ZExt;
    return G4_INST::Copy;
}

// check if this instruction can be propagated
G4_INST::MovType G4_INST::canPropagate(const IR_Builder* builder)
{
    G4_Declare* topDcl = NULL;

    if (dst == NULL)
    {
        return SuperMov;
    }

    topDcl = dst->getTopDcl();

    if (op != G4_mov 
        // Do not eliminate if either sat or condMod is present.
        || getSaturate() || getCondMod()
        // Do not eliminate if there's no use (dead or side-effect code?)
        || useInstList.size() == 0
        // Do not eliminate stack call return value passing instructions.
        // Do not eliminate vars marked with Output attribute
        || (topDcl && (topDcl->getHasFileScope() || topDcl->isOutput())))
    {
        return SuperMov;
    }

    // can't propagate stack call related variables (Arg, Retval, SP, FP)
    if (topDcl)
    {
        G4_Declare* rootDcl = topDcl->getRootDeclare();
        if (rootDcl->getIsPreDefFEStackVar() || rootDcl->getIsPreDefArg() || rootDcl->getIsPreDefRet())
        {
            return SuperMov;
        }
    }


    // Do not eliminate MOV/COPY to Acc/flag registers.
    if (dst->isAccReg() || dst->isFlag()) 
    {
        return SuperMov;
    }

    G4_Operand *src = srcs[0];

    if (src->isRelocImm())
    {
        return SuperMov;
    }

    // Do not elminate MOV/COPY from flag registers.
    if (src->isFlag()) {
        return SuperMov;
    }

    // Do not propagate through copy of `acc0` with NoMask.
    if (src->isAccReg() && isWriteEnableInst()) {
        return SuperMov;
    }

    if (builder->kernel.fg.globalOpndHT.isOpndGlobal(dst))
    {
        return SuperMov;
    }

    G4_Type dstType = dst->getType();
    G4_Type srcType = src->getType();

    G4_SrcModifier srcMod = Mod_src_undef;
    if (src->isSrcRegRegion()) {
        srcMod = src->asSrcRegRegion()->getModifier();
    }

    MovType MT = getMovType(dstType, srcType, srcMod);

    //Disabling mix mode copy propogation
    if (!builder->hasMixMode() &&
        ((IS_TYPE_F32_F64(srcType) && IS_HFTYPE(dstType)) ||
        (IS_HFTYPE(srcType) && IS_TYPE_F32_F64(dstType))))
    {
        return SuperMov;
    }

    // Selectively enable copy propagation on the detected mov type.
    switch (MT) {
    default:
        return SuperMov;
    case Copy:
    case ZExt:
    case SExt:
        // COPY and integer extending are allowed.
        break;
    case Trunc: {
        if (!src->isSrcRegRegion())
            return SuperMov;
        G4_SrcRegRegion *src0 = src->asSrcRegRegion();
        if (src0->getRegion()->isContiguous(getExecSize())) {
            unsigned newHS =
                G4_Type_Table[srcType].byteSize /
                G4_Type_Table[dstType].byteSize;
            if (newHS > 4) {
                // Rule out Q -> B. WHY?
                return SuperMov;
            }
        } else if (!src0->isScalar()) {
            return SuperMov;
        }
        break;
    }
    case FPUpConv:
        // For FPUpConv, only HF -> F is allowed.
        if (!(srcType == Type_HF    &&
            dstType == Type_F))
            return SuperMov;
        break;
    case G4_INST::FPDownConv:
    {
        if (IS_TYPE_F32_F64(srcType)                  &&
            IS_HFTYPE(dstType)                     &&
            builder->getOption(vISA_enableUnsafeCP_DF)  &&
            useInstList.size() == 1)
            return FPDownConvSafe;
        break;
    }
    // TODO: Enable IntToFP or vice versa on constant.
    }

    return MT;
}

// Check to see whether the given type is supported by this opcode + operand. Mainly focus on integer ops
// This is used by copy propagation and def-hoisting to determine if the resulting instruction is legal
bool G4_INST::isLegalType(G4_Type type, Gen4_Operand_Number opndNum) const
{
    bool isSrc = (opndNum == Opnd_src0 || opndNum == Opnd_src1 || opndNum == Opnd_src2);
    switch (op)
    {
    default:
        // ToDo: Make this function more complete by adding more opcodes
        // keep alphabetical order when adding to make it easier to maintain
        return true;
    case G4_addc:
        return type == Type_UD;
    case G4_bfe:
    case G4_bfi1:
    case G4_bfi2:
        // additionally check src and dst have same type
        return (type == Type_D || type == Type_UD) &&
         (isSrc ? type == dst->getType() : type == getSrc(0)->getType());
    case G4_bfrev:
        return type == Type_UD;
    case G4_cbit:
        return type == Type_UB || type == Type_UW || type == Type_UD;
    case G4_fbh:
        return type == Type_D || type == Type_UD;
    case G4_fbl:
        return type == Type_UD;
    case G4_lzd:
        return type == Type_D || type == Type_UD;
    case G4_sad2:
    case G4_sada2:
        return type == Type_B || type == Type_UB;
    case G4_subb:
        return type == Type_UD;
    }
}

// returns true if inst supports only F type for both src and dst
bool G4_INST::isFloatOnly() const
{
	switch (op)
	{
	default:
		return false;
	case G4_dp2:
	case G4_dp3:
	case G4_dp4:
	case G4_dph:
	case G4_frc:
	case G4_line:
	case G4_lrp:
	case G4_pln:
	case G4_rndd:
	case G4_rnde:
	case G4_rndu:
	case G4_rndz:
		return true;
	}
}

/// isSignSensitive() - Check whether this instruction is sign sensitive on the
/// specified source operand.
bool G4_INST::isSignSensitive(Gen4_Operand_Number opndNum) const
{
    G4_Operand *use = getOperand(opndNum);
    G4_Type useType = use->getType();
    G4_Type dstType = dst->getType();

    // If extending is required, most of insts are sign sensitive.
    if (G4_Type_Table[dstType].byteSize > G4_Type_Table[useType].byteSize) {
        return true;
    }

    switch (op) {
    case G4_asr:
        if (opndNum != Opnd_src0)
            break;
        // FALL THROUGH
    case G4_mach:
    case G4_fbh:
    case G4_mulh:
    case G4_sel:
    case G4_cmp:
    case G4_cmpn:
        return true;
    case G4_mov:
        // inttofp is sign sensitive
        return IS_TYPE_INT(useType) && IS_TYPE_FLOAT_ALL(dstType);
    default:
        break;
    }
    // By default, inst is regarded as sign insensitive.
    return false;
}

G4_Type G4_INST::getPropType(Gen4_Operand_Number opndNum, MovType MT, G4_INST *mov)
{
    G4_Operand *use = getOperand(opndNum);
    G4_Type useType = use->getType();
    G4_Type srcType = mov->getSrc(0)->getType();

    G4_SrcModifier srcMod = Mod_src_undef;
    if (mov->getSrc(0)->isSrcRegRegion()) {
        srcMod = mov->getSrc(0)->asSrcRegRegion()->getModifier();
    }
    G4_SrcModifier useMod = Mod_src_undef;
    if (use->isSrcRegRegion()) {
        useMod = use->asSrcRegRegion()->getModifier();
    }

    bool useIsFP = IS_TYPE_FLOAT_ALL(useType);
    bool srcIsFP = IS_TYPE_FLOAT_ALL(srcType);
    // Different numeric type.
    bool diffNumTy = useIsFP != srcIsFP;

    // TODO: Once we handle IntToFp, this condition should be checked
    // individually for each MovType.

    switch (MT) {
    case Copy:
        // Different numeric type with src mod cannot be propagated.
        if (diffNumTy && srcMod != Mod_src_undef)
            return Type_UNDEF;
        // Fp is simply to use useType.
        if (useIsFP)
            return useType;
        // Int needs to consider whether the use is sign-sensitive and the src
        // modifier.
        if (isSignSensitive(opndNum)) {
            switch (srcMod) {
            case Mod_Not:
            case Mod_Minus:
            case Mod_Minus_Abs:
                if (IS_UNSIGNED_INT(useType))
                    return Type_UNDEF;
                // Assume the combination of srcMod/srcType is valid.
                // FALL THROUGH
            case Mod_Abs:
                return srcType;
            default:
                break;
            }
        }
        else if (srcMod == Mod_Abs && IS_UNSIGNED_INT(useType) &&
                 IS_SIGNED_INT(srcType))
            return srcType;
        return useType;
    case ZExt:
        // Different numeric type with src zero-extended cannot be propagated.
        if (diffNumTy)
            return Type_UNDEF;
        // (sext (zext x)) is equal to (zext x)
        return srcType;
    case SExt:
        // Different numeric type with src sign-extended cannot be propagated.
        if (diffNumTy)
            return Type_UNDEF;
        // (zext (sext x)) is not equal to (sext x)
        if (IS_UNSIGNED_INT(useType))
            return Type_UNDEF;
        // Check if there's any modifier on the use.
        switch (useMod) {
        case Mod_Not:
        case Mod_Minus:
        case Mod_Minus_Abs:
            if (IS_QTYPE(useType) && IS_DTYPE(srcType)) {
                // (- (sext x)) is not equal to (sext (-x)) due to the corner case
                // where x is INT_MIN and -x is still INT_MIN without being
                // extended.
                return Type_UNDEF;
            }
            // FALL THROUGH
        default:
            break;
        }
        return srcType;
    case Trunc:
        if (diffNumTy)
            return Type_UNDEF;
        // Truncation always use the useType but the original source operand.
        // As a result, region needs changing to access the truncated bits
        // only.
        return useType;
    case FPUpConv:
        // Different numeric type with src up-converted cannot be propagated.
        if (diffNumTy)
            return Type_UNDEF;
        return srcType;
    case FPDownConvSafe:
        return srcType;
    default:
        break;
    }

    return Type_UNDEF;
}

// cases that we do not propagate
// 0. use inst does not support the type of the operand being propagated
// 1. use inst is align16 instruction
// 2. first source of line
// 3. indirect source to compressed instructions or math instructions
// 4. byte src to if/while instructions
// 5. src with modifier to logic inst on BDW
// 6. When useinst is lifetime.end

bool G4_INST::canPropagateTo(G4_INST *useInst, Gen4_Operand_Number opndNum, MovType MT, bool inSimdFlow)
{
    G4_Operand *src = srcs[0];
    bool indirectSrc = src->isSrcRegRegion() &&
                       src->asSrcRegRegion()->getRegAccess() != Direct;
    bool hasModifier = src->isSrcRegRegion() &&
                       src->asSrcRegRegion()->getModifier() != Mod_src_undef;
    G4_Type dstType = dst->getType();
    G4_Type srcType = src->getType();

    G4_Operand *use = useInst->getOperand(opndNum);
    G4_Type useType = use->getType();

    if (useInst->is2SrcAlign16())
    {
        // don't copy propagate for the legacy dp* instructions,
        // as we are missing some HW conformity checks for them
        return false;
    }

    // Skip lifetime.
    if (useInst->isLifeTimeEnd())
    {
        return false;
    }

    //disabling mixMode for MATH instruction
    if (isMixedMode() && useInst->isMath())
    {
        return false;
    }

    // special checks for message desc/extended desc, which must be either a0 or imm
    if (useInst->isSend())
    {
        auto msgDescOpnd = useInst->isSplitSend() ? Opnd_src2 : Opnd_src1;
        if (opndNum == msgDescOpnd)
        {
            if (!src->isImm() && !src->isAddress())
            {
                return false;
            }
        }
        if (opndNum == Opnd_src3)
        {    
            // there are some HW restrictions that prevent imm exdesc (e.g., on MRT write),
            // so we conservatively disable copy prop here   
            return false;
        }
    }

#if 0
    // Cannot propagate immediate src into send/sends GRF operands
    // special checks for send/sends GRF operands, which don't support
    // immediates and regions
    if ((opndNum == Opnd_src0 && useInst->isSend()) ||
        (opndNum == Opnd_src1 && useInst->isSplitSend()))
    {
        if (src->isImm() || MT != Copy)
        {
            return false;
        }
    }
#endif

    // The following are copied from local dataflow analysis.
    // TODO: re-examine..
    if ((opndNum == Opnd_src0 && useInst->isSend()) ||
        (opndNum == Opnd_src1 && useInst->isSplitSend()))
    {
        return false;
    }

    auto isFloatPseudoMAD = [](G4_INST *inst) {
        if (inst->opcode() != G4_pseudo_mad)
            return false;
        if (IS_TYPE_FLOAT_ALL(inst->getDst()->getType()))
            return true;
        return false;
    };


    //     mov (16|M0) r47.0 1:w
    // (W) add (16|M0) r49.0 r47.0 r45.0
    //
    // FIXME: remove this once DU/UD chain are computed correctly.
    //
    // Only skip when the defInst ('this') is defined in SIMD CF.
    if (useInst->isWriteEnableInst() && !this->isWriteEnableInst() && inSimdFlow)
    {
        return false;
    }

    if (src->isImm())
    {
        if (isFloatPseudoMAD(useInst) || useInst->opcode() == G4_math ||
            use->asSrcRegRegion()->getModifier() != Mod_src_undef)
        {
            return false;
        }
    } else if (src->isSrcRegRegion() &&
               src->asSrcRegRegion()->getRegAccess() != Direct &&
               (isFloatPseudoMAD(useInst) || useInst->opcode() == G4_math))
    {
        return false;
    }

    // FIXME: to add specific checks for other instructions.
    G4_opcode useInst_op = useInst->opcode();

    // Only few instructions allow hf mixed mode.
    if (this->isMixedMode() &&
        !(useInst_op == G4_mov ||
          useInst_op == G4_mul ||
          useInst_op == G4_mad ||
          useInst_op == G4_pseudo_mad   ||
          useInst_op == G4_add ||
          useInst_op == G4_sel ||
          useInst_op == G4_math))
    {
        return false;
    }
    if (this->isMixedMode()     &&
        execSize < 16           &&
        MT == FPDownConvSafe    &&
        useInst->execSize == 16 &&
        !useInst->isMixedMode())
    {
        return false;
    }

    if ((useInst_op == G4_line && opndNum == Opnd_src0) ||
        ((useInst_op == G4_if || useInst_op == G4_while) && !src->isImm() && IS_BTYPE(srcType)) ||
        (hasModifier && G4_Inst_Table[useInst_op].instType == InstTypeLogic) ||
        (indirectSrc && (useInst->opcode() == G4_math || useInst->isComprInst())))
    {
        // TODO: Revisit to simplify checking or remove invalid checking.
        return false;
    }

    bool isVxHSrc = indirectSrc && src->asSrcRegRegion()->getRegion()->isRegionWH();

    if (isVxHSrc && useInst->getExecSize() != execSize)
    {
        return false;
    }

    // In general, to check whether that MOV could be propagated:
    //
    //  dst/T1 = src/T0;
    //  op(..., use/T2, ...);
    //
    // We need firstly check whether 'dst' and 'use' are exactly the same
    // variable regardless data type.

    // Check T1 and T2 has the same bit/byte size. Otherwise, it's not legal to
    // be propagated.
    // TODO: Revisit later if exection mask is guaranteed to be NoMask.
    if (G4_Type_Table[dstType].byteSize != G4_Type_Table[useType].byteSize) {
        return false;
    }

    // Do not propagate if def type is float and use type is int, or vice
    // versa.
    // NOTE: Such usage is possible from bitcast (not through this MOV but the
    // reference in the use insn) from one type to another.
    // TODO: Revisit this later to handle the case where this MOV insn is
    // exactly a COPY. The useType should be used instead.
    if (MT != Copy && ((IS_TYPE_FLOAT_ALL(dstType) && IS_TYPE_INT(useType)) ||
                       (IS_TYPE_INT(dstType) && IS_TYPE_FLOAT_ALL(useType))))
    {
        return false;
    }

	if (MT == Copy &&
		hasModifier &&
		dstType != useType)
	{
		return false;
	}

    // Check 'dst' of MOV and 'use' are the same variable. Otherwise, it's not
    // legal to be propagated.
    G4_CmpRelation rel = dst->compareOperand(use);
    if (rel != Rel_eq)
    {
        // TODO: When dst Rel_gt use, we still have chance to copy propagate
        // the src by splitting it accordingly.
        // E.g.
        // mov (16) r3.0<1>:ud ...
        // mul (8) r5.0<1>:ud r3.0<8;8,1>:ud ...
        // mul (8) r6.0<1>:ud r4.0<8;8,1>:ud ...
        return false;
    }

    // Type to be used after propagation. Use srcType by default.
    G4_Type propType = useInst->getPropType(opndNum, MT, this);

    if (propType == Type_UNDEF)
        return false;

    // Don't propagate unsupported propType.
    if (!useInst->isLegalType(propType, opndNum))
    {
        return false;
    }

    // TODO: Revisit this later as IntToFp could be folded on specific insns,
    // such as add, cmp, and mul, when types of all source operands could be
    // consistent.
    if (!(useInst->isRawMov() && dstType == useType) &&
        ((IS_FTYPE(dstType) && (IS_TYPE_INT(propType) || IS_VINTTYPE(propType))) ||
         (IS_TYPE_INT(dstType) && (IS_FTYPE(propType) || IS_VFTYPE(propType)))))
    {
        return false;
    }

    unsigned numDef = 0;
    for (DEF_EDGE_LIST_ITER DI = useInst->defInstList.begin(),
                            DE = useInst->defInstList.end(); DI != DE; ++DI)
    {
        if (DI->second == opndNum)
        {
            numDef++;
            if (numDef > 1)
            {
                return false;
            }
        }
    }

    // Cannot generally safely propagate replicated vectors.
    unsigned dstElSize = G4_Type_Table[dstType].byteSize;
    unsigned srcElSize = G4_Type_Table[propType].byteSize;
    unsigned useElSize = G4_Type_Table[useType].byteSize;

    RegionDesc *rd = !src->isSrcRegRegion() ? NULL : src->asSrcRegRegion()->getRegion();
    unsigned char new_exec_size = useInst->getExecSize();
    if (useElSize != dstElSize &&
        (!src->isSrcRegRegion()
         || rd->isRepeatRegion(execSize)
         || !(rd->isFlatRegion() && rd->isPackedRegion())))
    {
        return false;
    }

    // Skip propagate scalar copies into the additive operand (src2) of integer
    // pseudo mad.
    if (opndNum == Opnd_src2 && useInst->opcode() == G4_pseudo_mad &&
        IS_TYPE_INT(useType) && rd && rd->isScalar())
      return false;

    // Check repeat region
    bool sameDefUseELSize = (dstElSize == useElSize);
    bool sameExecSize = (execSize == new_exec_size);
    RegionDesc *useRd = use->isSrcRegRegion() ? use->asSrcRegRegion()->getRegion() : nullptr;
    bool repeatUseRegion = useRd && useRd->isRepeatRegion(new_exec_size);
    bool scalarUse = useRd && useRd->isScalar();
    bool repeatSrcRegion = (rd && rd->isRepeatRegion(execSize));
    if (!sameExecSize &&
        !((sameDefUseELSize && scalarUse) ||
          (!repeatUseRegion && rd && rd->isFlatRegion() && rd->isPackedRegion()) ||
          (repeatUseRegion && sameDefUseELSize && (src->isImm() || !repeatSrcRegion))))
    {
        return false;
    }

    // Be conserversative, do not bother to do complicated region compositions.
    // There are three variables to compute the composition:
    // (1) the dst stride
    // (2) the source region
    // (3) the use source region

    // dStride, the dst stride
    // stride1, stride2 must be positive
    auto isComposable = [=](unsigned dStride, unsigned stride1,
                            unsigned stride2) -> bool
    {
        MUST_BE_TRUE(stride1 && stride2, "scalar region not expected");

        // composition is rd1 (or rd2).
        // If two variables are trivial, then the other variable could be
        // arbitrary. E.g.
        //
        // mov (8) V81(0,0)<1>:w V80(0,0)<1;1,0>:w
        // add (16) V82(0,0)<1>:w V81(0,0)<0;8,1>:w 0xa:w
        //
        // where rd1 has stride 1, dStride = 1, rd2 is non single strided.
        if ((stride1 == 1 && dStride == 1) || (stride2 == 1 && dStride == 1))
          return true;

        // If either stride1 or stride2 equals UndefVal, then there is no easy
        // formula to do the composition unless dStride == 1 and the other has
        // stride 1. This case is covered by the first check.
        //
        // To be composable, both regions need to be single strided (i.e. value
        // != UndefVal). This check is simplified by the value UndefVal (64).
        return stride1 * stride2 * dStride <= 32;
    };

    if (!sameExecSize && rd && useRd)
    {
        // the compoisition is also scalar.
        if (!rd->isScalar() && !useRd->isScalar())
        {
            G4_DstRegRegion *dstRegion = dst;
            uint16_t dstStride = dstRegion->getHorzStride();

            // A value to indicate this region is non-single strided.
            // Make it larger than 32 to simplify/unify the checking.
            const uint16_t UndefVal = 64;

            uint16_t stride1 = UndefVal;
            if (rd->isContiguous(execSize))
                stride1 = 1;
            else
                rd->isSingleNonUnitStride(execSize, stride1);

            uint16_t stride2 = UndefVal;
            if (useRd->isContiguous(new_exec_size))
                stride2 = 1;
            else
                useRd->isSingleNonUnitStride(new_exec_size, stride2);

            if (!isComposable(dstStride, stride1, stride2))
                return false;
        }
    }

    // check data type alignment
    if ((srcElSize < useElSize) &&
        (dstElSize == srcElSize) &&
        (execSize > 1) &&
        (!src->isImm()) &&
        ((src->getByteOffset() % useElSize ) != 0))
    {
        return false;
    }

	if (src->isImm() && use->asSrcRegRegion()->getModifier() != Mod_src_undef)
	{
		//FIXME: do we need to worry about signal bit in NaN being dropped?
		if (IS_TYPE_INT(srcType))
		{
			// we can't represent -(INT_MIN) or abs(INT_MIN)
			int64_t value = src->asImm()->getImm();
			switch (propType)
			{
			case Type_Q:
			case Type_UQ:
				return value != LLONG_MIN;
			default:
				return value != INT_MIN;
			}
		}
	}

    return true;
}

// check if this inst can be hoisted
// assume only MOV inst is checked
bool G4_INST::canHoist(bool simdBB, const Options *opt)
{
    if (dst == NULL)
    {
        return false;
    }
    if ( getDst()->getTopDcl() && getDst()->getTopDcl()->getHasFileScope() )
    {
            return false;
    }

    G4_Operand *src = srcs[0];
    // check attributes of src and number of defs
    bool archRegSrc = ( src->isFlag() || src->isAreg() || src->isAddress() );
    bool indirectSrc = ( src->getTopDcl() && src->getTopDcl()->getAddressed() ) || src->getRegAccess() != Direct;
    bool noMultiDefOpt = ( ( defInstList.size() > 1 ) &&
        ( predicate || ( dst->getRegAccess() != Direct ) || simdBB ) );
    if( src->isImm() ||
        archRegSrc ||
        indirectSrc ||
        ( src->asSrcRegRegion()->getModifier() != Mod_src_undef ) ||
        ( defInstList.size() == 0 ) ||
        noMultiDefOpt )
    {
            return false;
    }

    // check type
    G4_Type dstType, srcType;
    dstType = dst->getType();
    srcType = src->getType();

    // no dst type promotion after hoisting
    if (!Is_Type_Included(dstType, srcType, opt) ||
        // if multi def, src and dst should have the same type size
        ( ( defInstList.size() > 1 ) &&
        ( ( Operand_Type_Rank( srcType ) != Operand_Type_Rank( dstType ) ) ||
        // if multidef and used as a scalar, execution size should be one.
        ( src->isSrcRegRegion() && src->asSrcRegRegion()->isScalar() && ( execSize > 1 ) ) ) ) )
    {
        return false;
    }

    // no opt repeat region
    unsigned short src_wd = src->asSrcRegRegion()->getRegion()->width;
    if( ( src_wd != execSize &&
        ( src->asSrcRegRegion()->getRegion()->vertStride < ( src_wd * src->asSrcRegRegion()->getRegion()->horzStride ) ) ) ||
        // actually we can hoist if src is a scalar and target inst has no pred or cond mod.
        ( execSize > 1 && src->asSrcRegRegion()->isScalar() ) )
    {
        return false;
    }

    if (src->getTopDcl() && src->getTopDcl()->isOutput())
    {
        return false;
    }

    return true;
}

// check if this instruction can be hoisted to defInst
bool G4_INST::canHoistTo( G4_INST *defInst, bool simdBB, const IR_Builder* builder)
{

    bool indirect_dst = (dst->getRegAccess() != Direct);

    G4_Operand *def_dst = defInst->getDst();

    if (def_dst == NULL)
    {
        // can this actually happen?
        return false;
    }
    G4_Type defDstType = def_dst->getType();
    G4_Type dstType = dst->getType(), srcType = srcs[0]->getType();
    bool rawMovInst = isRawMov();
    bool cantHoistMAD = (defInst->opcode() == G4_pseudo_mad && !(IS_TYPE_FLOAT_ALL(dstType) && IS_TYPE_FLOAT_ALL(defDstType)));
    if (
        ( defInst->useInstList.size() != 1 ) ||
        ( defInst->opcode() == G4_sad2 ) ||
        ( defInst->opcode() == G4_sada2 ) ||
        (defInst->opcode() == G4_cbit && dstType != defDstType) ||
        (( cantHoistMAD || (defInst->opcode() == G4_math)) &&
        ( indirect_dst || ( dstType != defDstType && !rawMovInst ) ) ) )
    {
        return false;
    }

    if (!defInst->isLegalType(dstType, Opnd_dst))
    {
        return false;
    }

    if (isMixedMode())
    {
        if (defInst->isMath())
        {
            return false;
        }
        if (!builder->hasMixMode())
        {
            // normally we should disable the opt, but for the special case where 
            // defInst is a move with integer source, we can still hoist since it 
            // won't produce a mixed mode inst
            if (!(defInst->isMov() && IS_TYPE_INT(defInst->getSrc(0)->getType())))     
            {
                return false;
            }
        }
    }

    // compare boudaries and bitset
    if( ( def_dst->getLeftBound() < srcs[0]->getLeftBound() ) ||
        ( def_dst->getRightBound() > srcs[0]->getRightBound() ) )
    {
        return false;
    }

    if (getSaturate() && !defInst->canSupportSaturate())
    {
        return false;
    }

    unsigned int srcElSize = G4_Type_Table[srcType].byteSize, defDstElSize = G4_Type_Table[defDstType].byteSize,
        dstElSize = G4_Type_Table[dstType].byteSize;

    // check mixed type conversion
    // TODO: cleanup this part since mixed type check of the first half is already checked in canHoist.
    if( ( !( defInst->isRawMov() && ( defDstType == srcType ) ) &&
        ( ( IS_FTYPE(dstType) && ( IS_TYPE_INT( srcType ) || IS_VINTTYPE( srcType ) ) ) ||
        ( ( IS_FTYPE(srcType) || IS_VFTYPE(srcType) ) && IS_TYPE_INT( dstType ) ) ) ) ||
        ( !rawMovInst &&
        ( ( IS_FTYPE( defDstType ) && IS_TYPE_INT( defInst->getExecType() ) ) ||
        ( IS_FTYPE( defInst->getExecType() )  && IS_TYPE_INT( defDstType ) ) ) ) )
    {
        return false;
    }

	if( !rawMovInst && (defInst->getSrc(0) &&
        (IS_DFTYPE(defInst->getSrc(0)->getType()) || IS_FTYPE(defInst->getSrc(0)->getType()))) &&
        (IS_SIGNED_INT(defDstType) || IS_UNSIGNED_INT(defDstType)) )
	{
		// Sequence that should not be optimized:
		// mov V1:d    V2:df
		// mov V3:uw    V1:d
		//
		// This is *NOT* a candidate for:
		// mov V3:uw    V2:df
		//
        // In general, df/f->int performs saturation and unless value of
        // df/f is known, the result of mov may differ based on type
        // of dst.
		return false;
	}

    if (defInst->isSend()) {
        // 'mov' following 'send' could be hoisted if and only if
        // - that 'mov' is raw move,
        // - that 'mov' is not predicated,
        // - dst of 'mov' is aligned to GRF.
        // - execution size is 8 or 16.
        if (!rawMovInst)
            return false;
        if (getPredicate() || defInst->getPredicate())
            return false;
        if (dst->getLeftBound() % G4_GRF_REG_NBYTES != 0)
            return false;
#if 0
        return (execSize == 8 || execSize == 16 || execSize == 32);
#else
        return false;
#endif
    }

    if (defInst->opcode() == G4_mov && defInst->getSrc(0)->isFlag())
    {
        // TODO: check if use is a predicate, if not, can propagate?
        return false;
    }

    if (simdBB && (defInst->isWriteEnableInst() ^ isWriteEnableInst()))
    {
        // no opt if one isNoMask but the other is not
        return false;
    }

    if (defInst->getMaskOffset() != getMaskOffset() &&
        (simdBB || getPredicate() || getCondMod() ||
         defInst->getPredicate() || defInst->getCondMod()))
    {
        // no opt if their mask offset do not match,
        // and mov/defInst has flags
        return false;
    }

    if ((getPredicate() || getCondMod()) && (defInst->getPredicate() || defInst->getCondMod()))
    {
        // can't have both inst using flags
        return false;
    }

    bool same_type_size = ( G4_Type_Table[def_dst->getType()].byteSize == G4_Type_Table[srcType].byteSize );
    bool scalarSrc = ( srcs[0]->asSrcRegRegion()->isScalar() );
    // handle predicated MOV and float def
    if( ( getPredicate() && ( execSize > 1 ) && !same_type_size ) ||
        ( IS_FTYPE( defDstType ) && ( defDstType != srcType ) && ( dstType != srcType ) ) )
    {
        return false;
    }

    // if used as scalar and repeated region, dst should be packed
    // add(2) v2<1>:w v3 v4
    // mov(2) v5<2>:d  V2<0;1,0>:d
    if( scalarSrc && !same_type_size &&
        ( execSize > 1 ) && ( dst->getHorzStride() != 1 ) )
    {
        return false;
    }

    // if indirect source is repeat region, or defhoisting will make it a repeat region,
    // no opt
    if( srcs[0]->asSrcRegRegion()->getRegion()->isRepeatRegion( execSize ) &&
        !scalarSrc )
    {
        return false;
    }

    // check type conversion
    if( IS_SIGNED_INT(dstType) && ( defInst->opcode() == G4_mov ) &&
        ( G4_Type_Table[dstType].byteSize > srcElSize ) &&
        ( ( IS_SIGNED_INT(defDstType) && IS_UNSIGNED_INT(defInst->getSrc(0)->getType()) ) ||
        ( IS_UNSIGNED_INT(defDstType) && IS_SIGNED_INT(defInst->getSrc(0)->getType()) ) ) )
    {
        return false;
    }

    // check alignment and saturate
    if (((srcElSize > defDstElSize) || defInst->getSaturate()) && (srcType != dstType))
    {
        return false;
    }

    uint16_t dstHS = dst->getHorzStride();
    uint16_t srcHS = 0;
    RegionDesc *srcRd = srcs[0]->asSrcRegRegion()->getRegion();
    if (!srcRd->isSingleStride(execSize, srcHS))
    {
        return false;
    }
    if ((srcElSize < defDstElSize) && ((dstHS > 1) || (srcHS > 1)))
    {
        return false;
    }
    if ((dstElSize != defDstElSize) && (srcElSize == dstElSize) &&
        (indirect_dst || ((dst->getByteOffset() % defDstElSize) != 0) ||
        (dstHS != srcHS)))
    {
        return false;
    }

    // dont hoist stack calls related variables (Arg, Retval, SP, FP)
    if (defInst->getDst() && defInst->getDst()->getTopDcl())
    {
        G4_Declare* defDstDcl = defInst->getDst()->getTopDcl()->getRootDeclare();
        if (defDstDcl->getIsPreDefFEStackVar() || defDstDcl->getIsPreDefArg() ||
            defDstDcl->getIsPreDefRet())
        {
            return false;
        }
    }

    // For mov HF F, we have to check if the def Inst supports HF
    if (dstType != Type_F && defInst->isFloatOnly() && !isRawMov())
    {
        return false;
    }

    // Before:
    // or (8) V100(0,0)<1>:d ...
    // or (8) V100(1,0)<1>:d ...
    // mov (16) V101(0,0)<1>:b    V102(0,0)<16;16,1>:w <-- V102 is alias of V100
    // mov (16) V101(0,16)<1>:b   V102(1,0)<16;16,1>:w

    // After (invalid optimization):
    // or (8) V100(0,0)<1>:d ...
    // or (8) V100(0,4)<1>:d ...
    if(defDstType != srcType)
    {
        if(isRawMov() == false)
        {
            return false;
        }
    }

    return true;
}

// check if the sources of an inst is commutative
// besides the property shown in inst table, some INT MUL instructions
// are not commutative due to HW restrictions
bool G4_INST::isCommutative() const
{
    //TODO: we can invert condMod of cmp to swap sources
    if( !( G4_Inst_Table[op].attributes & ATTR_COMMUTATIVE ) || op == G4_cmp )
        return false;

    // for mul we can do D*W but not W*D
    if( op == G4_mul )
    {
        if( IS_DTYPE(srcs[0]->getType()))
        {
            return false;
        }
    }
    return true;
}

// check if this inst and its use inst can use ACC as dst/src
// check the following rules:
// 1. acc can only be src0 when explicit
// 2. if alreay has implicit acc src/dst, no acc opt
// 3. if dst is addressed somewhere, no acc opt
// 4. 3-src inst can not use ACC as dst
// 5. acc src can not use modifier in LOGIC inst

// There are many cases that use simd16 Float, split these instructions enables more opportimizations.
bool G4_INST::canUseACCOpt( bool handleComprInst, bool checkRegion, uint16_t &hs, bool allow3Src, bool allowTypeDemotion, bool insertMov )
{
    hs = 0;

    // dst checks
    if (!dst || hasNULLDst() || IS_BTYPE(dst->getType()) ||
        (dst->getBase()->isRegVar() &&
        dst->getBase()->asRegVar()->getDeclare()->getAddressed()))
    {
        return false;
    }

    // opcode related checks (why is this necessary? shouldn't inst always be pseudo_mad???)
    if (op == G4_math || op == G4_shl ||
        (!allow3Src && G4_Inst_Table[op].n_srcs > 2 ) ||
        (predicate && op != G4_sel))
    {
        return false;
    }

    // acc usage checks
    if (implAccDst ||
        implAccSrc ||
        dst->isAccReg())
    {
        return false;
    }

    // no can do if dst has >1 use, or that its use is not in src0
    if (useInstList.size() != 1  ||
        (( useInstList.front().second != Opnd_src0 ) &&
        !( ( useInstList.front().second == Opnd_src1 ) && useInstList.front().first->isCommutative() )))
    {
        return false;
    }

    // check use inst
    G4_INST *useInst = use_begin()->first;
    Gen4_Operand_Number opndNum = use_begin()->second;
    G4_SrcRegRegion *use = useInst->getOperand(opndNum)->asSrcRegRegion();
    G4_opcode useOp = useInst->opcode();

    // check number of defs
    if (0 == useInst->getSingleDef(opndNum))
    {
        return false;
    }

    // use inst opcode checks

    if (useInst->isSend() ||
        useOp == G4_math || useOp == G4_mac || useOp == G4_mach || useOp == G4_sada2 ||
        G4_Inst_Table[useOp].n_srcs > 2 ||
        (G4_Inst_Table[useOp].instType == InstTypeLogic && use->getModifier() != Mod_src_undef))
    {
        return false;
    }

    // checks on the use inst
    if (!useInst->getDst() || useInst->getExecSize() != execSize ||
        (useInst->getMaskOption() != getMaskOption() && !isWriteEnableInst()))
    {
        return false;
    }

    // checks on the use operand
    if (use->getRegAccess() != Direct ||
        IS_BTYPE(use->getType()) ||
        (checkRegion && use->compareOperand(dst) != Rel_eq ))
    {
        return false;
    }

    // more are needed to check. for example, tsc and other architecture...

    if( G4_Type_Table[dst->getType()].byteSize != G4_Type_Table[use->getType()].byteSize )
    {
        return false;
    }
    uint32_t dstStrideSize = G4_Type_Table[useInst->getDst()->getType()].byteSize * ( insertMov ? 1 : useInst->getDst()->getHorzStride() );
    uint32_t execStrideSize;
    G4_Type useExecType;
    if( allowTypeDemotion )
    {
        // recompute exec type
        useExecType = Type_W;
        for (unsigned i = 0; i < G4_Inst_Table[useInst->opcode()].n_srcs; i++)
        {
            if( useInst->getSrc(i) == use )
            {
                continue;
            }
            G4_Type srcType = useInst->getSrc(i)->getType();
            if( IS_VINTTYPE( srcType ) )
            {
                continue;
            }
            if( srcType == Type_DF
                && !IS_DFTYPE(useExecType) )
            {
                useExecType = srcType;
                break;
            }
            else if( (IS_FTYPE(srcType) || srcType == Type_VF) &&
                !IS_DFTYPE(useExecType) && !IS_FTYPE(useExecType) )
            {
                useExecType = Type_F;
            }
            else if( IS_DTYPE(srcType) && !IS_DTYPE(useExecType) &&
                !IS_DFTYPE(useExecType) && !IS_FTYPE(useExecType) )
            {
                useExecType = Type_D;
            }
        }
        execStrideSize = G4_Type_Table[useExecType].byteSize;
    }
    else
    {
        execStrideSize = G4_Type_Table[useInst->getExecType()].byteSize;
    }
    uint32_t useTypeSize = allowTypeDemotion ?
        G4_Type_Table[Type_UW].byteSize : G4_Type_Table[use->getType()].byteSize;

    hs = useInst->hasNULLDst() ?
        ( G4_Type_Table[useInst->getDst()->getType()].byteSize / useTypeSize )
        : ( !useInst->getDst() ? 1
        : ( ( ( dstStrideSize > execStrideSize ) ? dstStrideSize : execStrideSize ) / useTypeSize ) );

    bool useRepeatRegion = ( !use->isScalar() && use->getRegion()->isRepeatRegion( useInst->getExecSize() ) ) || // no repeat region -- we can improve this by extending def, any benefit in power?
        ( use->isScalar() && useInst->getExecSize() > 1 );
    bool dstAlignedToGRF = useInst->hasNULLDst() || useInst->getDst()->checkGRFAlign();
    if( checkRegion &&
        ( useRepeatRegion ||
        ( hs > 4 ) ||  // src hs cannot be larger than 4
        !dstAlignedToGRF ) )
    {
        return false;
    }

    bool crossGRF = (( !handleComprInst && IS_TYPE_INT( use->getType() ) ) ) &&
        ( ( useInst->getDst()->getRightBound() - useInst->getDst()->getLeftBound() ) > GENX_GRF_REG_SIZ );
    if( crossGRF )
    {
        return false;
    }
    return true;
}

bool G4_INST::hasNULLDst()
{
    if( dst && dst->isNullReg() )
    {
        return true;
    }

    return false;
}

bool G4_INST::goodTwoGRFDst( bool& evenSplitDst )
{
    evenSplitDst = false;
    // The following applies to all platforms
    // The problem is , the first case is really an instruction with two destination registers.
    // in which case, hardware breaks into two operations. When this happens, hardware cannot update flag registers.
    // I.e., if execution size is 8 or less and the destination register is 2, flag updates are not supported.
    // -naveen

    if( !dst || hasNULLDst() )
    {
        evenSplitDst = true;
        return true;
    }
    else
    {
        evenSplitDst = dst->evenlySplitCrossGRF(execSize);
        // check if elements evenly split between two GRFs
        if( evenSplitDst )
        {
            return true;
        }
        else
        {
            return false;
        }
    }
}

// check if there is WAW, WAR, RAW dependency between the passing-in inst and this instruction
// there is no check for the case that two instructions are both send, since these checks are
// only used in def-joisting and copy propagation
bool G4_INST::isWARdep( G4_INST *inst )
{
    G4_Operand *msg0 = NULL;
    G4_Operand *src0_0 = inst->getSrc( 0 );
    G4_Operand *src0_1 = inst->getSrc( 1 );
    G4_Operand *src0_2 = inst->getSrc( 2 );
    G4_Operand *implicitSrc0   = inst->getImplAccSrc();
    G4_Predicate *pred0   = inst->getPredicate();

    G4_Operand *dst1 = this->dst;

    if( dst1 && !this->hasNULLDst() )
    {

        if( //checkDst0 && ( dependency = DoInterfere( dst0, dst1, RET_WAR, noDDChkMode ) ) ||
            ( src0_0 && src0_0->compareOperand( dst1 ) != Rel_disjoint ) ||
            ( src0_1 && src0_1->compareOperand( dst1 ) != Rel_disjoint ) ||
            ( src0_2 && src0_2->compareOperand( dst1 ) != Rel_disjoint ) ||
            ( msg0 && ( msg0->compareOperand( dst1 ) != Rel_disjoint ) ) ||
            ( pred0 && ( pred0->compareOperand( dst1 ) != Rel_disjoint ) ) ||
            ( implicitSrc0 && ( implicitSrc0->compareOperand( dst1 ) != Rel_disjoint ) ) )
        {
            return true;
        }
    }

    if( mod )
    {
        if( ( pred0 && pred0->compareOperand( mod ) != Rel_disjoint ) ||
            ( src0_0 && src0_0->isFlag() && src0_0->compareOperand( mod ) != Rel_disjoint ) ||
            ( src0_1 && src0_1->isFlag() && src0_1->compareOperand( mod ) != Rel_disjoint ) ||
            ( src0_2 && src0_2->isFlag() && src0_2->compareOperand( mod ) != Rel_disjoint ) )
        {
            return true;
        }
    }

    if( this->implAccDst )
    {
        if( ( implicitSrc0 && implicitSrc0->compareOperand( implAccDst ) != Rel_disjoint ) ||
            ( src0_0 && src0_0->isAccReg() && src0_0->compareOperand( implAccDst ) != Rel_disjoint ) ||
            ( src0_1 && src0_1->isAccReg() && src0_1->compareOperand( implAccDst ) != Rel_disjoint ) ||
            ( src0_2 && src0_2->isAccReg() && src0_2->compareOperand( implAccDst ) != Rel_disjoint ) )
        {
            return true;
        }
    }
    return false;
}

bool G4_INST::isWAWdep( G4_INST *inst )
{
    G4_Operand *dst0 = inst->getDst();
    G4_Operand *dst1 = this->dst;
    G4_CondMod *cMod0   = inst->getCondMod();
    G4_CondMod *cMod1   = this->mod;
    G4_Operand *implicitDst0   = inst->getImplAccDst();
    G4_Operand *implicitDst1   = this->implAccDst;

    bool NULLDst1 = !dst1 || this->hasNULLDst();
    if( dst0 && !inst->hasNULLDst() )
    {
        if( ( !NULLDst1 && dst1->compareOperand( dst0 ) != Rel_disjoint ) ||
            ( implicitDst1 && implicitDst1->compareOperand( dst0 ) != Rel_disjoint ) ||
            ( cMod1 && cMod1->getBase() && cMod1->compareOperand( dst0 ) != Rel_disjoint ) )
        {
            return true;
        }
    }

    if( implicitDst0 )
    {
        if( ( !NULLDst1 && dst1->compareOperand( implicitDst0 ) != Rel_disjoint ) ||
            ( implicitDst1 && implicitDst1->compareOperand( implicitDst0 ) != Rel_disjoint ) )
        {
            return true;
        }
    }

    if( cMod0 && cMod0->getBase() )
    {
        if( ( !NULLDst1 && dst1->compareOperand( cMod0 ) != Rel_disjoint ) ||
            ( cMod1 && cMod1->getBase() && cMod1->compareOperand( cMod0 ) != Rel_disjoint ) )
        {
            return true;
        }
    }

    return false;
}
bool G4_INST::isRAWdep( G4_INST *inst )
{
    G4_Operand *dst0   = inst->getDst();
    G4_CondMod *cMod0   = inst->getCondMod();
    G4_Operand *implicitDst0   = inst->getImplAccDst();
    G4_Operand *msg1 = NULL;
    G4_Predicate *pred1   = this->getPredicate();
    G4_Operand *src1_0 = this->getSrc(0);
    G4_Operand *src1_1 = this->getSrc(1);
    G4_Operand *src1_2 = this->getSrc(2);
    G4_Operand *implicitSrc1   = this->implAccSrc;

    bool NULLSrc1 = ( this->opcode() == G4_math && src1_1->isNullReg() );
    if( dst0 && !inst->hasNULLDst() )
    {
        if( ( src1_0 && src1_0->compareOperand( dst0 ) != Rel_disjoint ) ||
            ( src1_1 && !NULLSrc1 && src1_1->compareOperand( dst0 ) != Rel_disjoint ) ||
            ( src1_2 && src1_2->compareOperand( dst0 ) != Rel_disjoint ) ||
            ( msg1 && msg1->compareOperand( dst0 ) != Rel_disjoint ) ||
            ( pred1 && pred1->compareOperand( dst0 ) != Rel_disjoint ) ||
            ( implicitSrc1 && implicitSrc1->compareOperand( dst0 ) != Rel_disjoint ) )
        {
            return true;
        }
    }

    if( cMod0 && cMod0->getBase() )
    {
        if( ( pred1 && pred1->compareOperand( cMod0 ) != Rel_disjoint ) ||
            ( src1_0 && src1_0->isFlag() && src1_0->compareOperand( cMod0 ) != Rel_disjoint ) ||
            ( src1_2 && src1_2->isFlag() && src1_2->compareOperand( cMod0 ) != Rel_disjoint ) ||
            ( src1_1 && src1_1->isFlag() && src1_1->compareOperand( cMod0 ) != Rel_disjoint ) )
        {
            return true;
        }
    }

    if( implicitDst0 )
    {
        if( ( implicitSrc1 && implicitSrc1->compareOperand( implicitDst0 ) != Rel_disjoint ) ||
            ( src1_0 && src1_0->isAccReg() && src1_0->compareOperand( implicitDst0 ) != Rel_disjoint ) ||
            ( src1_2 && src1_2->isAccReg() && src1_2->compareOperand( implicitDst0 ) != Rel_disjoint ) ||
            ( src1_1 && src1_1->isAccReg() && src1_1->compareOperand( implicitDst0 ) != Rel_disjoint ) )
        {
            return true;
        }
    }
    return false;
}

bool G4_INST::detectComprInst() const
{
    enum class ComprInstStates : unsigned char { U, T, F };

    G4_Type execType = getExecType();
    ComprInstStates comprInst = ComprInstStates::U;

    // Compressed instructions must have a minimum execution size of
    // at least 8.
    if (execSize < 8)
    {
        comprInst = ComprInstStates::F;
    }

    // Compressed instructions must have a minimum execution size of
    // at least 16 if the execution type is less than DF.
    else if (dst &&
             dst->getHorzStride() != UNDEFINED_SHORT &&
             dst->getType() != Type_UNDEF)
    {
        if (execSize * G4_Type_Table[dst->getType()].byteSize *
             dst->getHorzStride() > G4_GRF_REG_NBYTES)
        {
            comprInst = ComprInstStates::T;
        }
        else
        {
            comprInst = ComprInstStates::F;
        }
    }

    // Uncompressed instructions can only operate on a max of 4 DFs or
    // 8 DF4/F/DWs or 16 W/Bs (the only exception being packed byte
    // moves which always have destinations).
    else if (execSize * G4_Type_Table[execType].byteSize >
             G4_GRF_REG_NBYTES)
    {
        comprInst = ComprInstStates::T;
    }

    else
    {
        comprInst = ComprInstStates::F;
    }

    return (comprInst == ComprInstStates::T);
}

/*
 * Check to see if the interpretation of the i/p src region is unaffected by
 * virtue of it making it a src of the compressed op, as opposed to (if
 * possible) it appearing within a regular uncompressed op with the same exec
 * size.
 * Register-indirect operands are NOT compression invariant. The following 4 rules
 * are used to determine compression invariant register-direct opnds:
 *    1. constants, scalars, and ARF regions/registers are always compression invariant
 *    2. if both the dst region and the i/p source region are native packed
 *       regions, and the GRF source region is additionally of type W/UW
 *    3. the src region covers (i.e. vs(region) * rows(region)) exactly two
 *       registers (strides allowed), except when the dst region is a native
 *       packed region and the GRF source has packed rows of type W/UW
 *    4. the first src of line op is always considered compression invariant
 *       (this is a special case quadruple region of <0;4,1>)
 * e.g.
 *   (Both srcs are compression invariant in the following examples)
 *      add (16) r10.0<1>:d  r12.0<0;1,0>:w  0x80:w {CC}
 *      add (16) r10.0<2>:w  r12.0<8;8,1>:d  r14.0<16;8,2>:w {CC}
 *      add (16) r10.0<1>:d  r12.0<16;8,2>:w r14.0<32;8,4>:b {CC}
 *      add (16) r10.0<1>:d  r12.0<8;8,1>:w  r14.0<8;8,1>:w {CC}
 *      add (16) r10.0<1>:d  r12.0<4;4,1>:w  r14.0<4;4,1>:d {CC}
 *      add (32) r10.0<1>:w  r12.0<8;8,1>:w  r14.0<16;8,2>:b {CC}
 *      add (8)  r10.0<1>:df r12.0<4;4,1>:df r14.0<4;4,1>:df {CC}
 *      mov (8)  r10.0<1>:df r12.0<4;4,1>:w {CC}
 *   (Only the first src is compression invariant in the following examples)
 *      add (16) r10.0<1>:d  r12.0<8;8,1>:w  r14.0<16;8,2>:b {CC}
 *      add (16) r10.0<2>:w  r14.0<32;8,1>:b r12.0<16;8,1>:w {CC}
 *      add (16) r10.0<2>:w  r12.0<4;4,1>:d  r14.0<8;8,1>:w {CC}
 *      add (32) r10.0<1>:w  r12.0<8;8,1>:w  r14.0<8;8,1>:b {CC}
 *   (Neither src is compression invariant in the following examples)
 *      add (16) r10.0<2>:w  r12.0<8;8,1>:w  r14.0<16;8,2>:b {CC}
 *      add (32) r10.0<1>:w  r12.0<8;8,1>:b  r14.0<8;8,1>:b {CC}
 *      mov (8)  r10.0<1>:df r12.0<4;4,1>:dw {CC}
 * Inputs:
 *      src - the i/p src operand region
 *      src_pos - the position that the src operand appears in the list
 *                of src operands
 * Assumptions:
 *    - this function is only valid for compressed ops and it is invalid
 *      to call it for uncompressed ops
 */
bool
G4_INST::isComprInvariantSrcRegion(G4_SrcRegRegion* src, int srcPos)
{
    if (src == NULL)
    {
        return true;
    }
    else if (src->isImm() || src->isAddrExp())
    {
        return true;
    }
    else if (src->getRegAccess() != Direct)
    {
        return false;
    }
    else if (src->getBase()->asRegVar()->getDeclare()->getRegFile() != G4_GRF &&
             src->getBase()->asRegVar()->getDeclare()->getRegFile() != G4_INPUT)
    {
         return true;
    }

    RegionDesc* region = src->getRegion();

    if (opcode() == G4_line && srcPos == 0)
    {
        return true;
    }
    else if (region->isScalar())
    {
        return true;
    }
    else
    {
        int num_rows  = getExecSize() / src->getRegion()->width;
        int type_sz   = G4_Type_Table[src->getType()].byteSize;
        int byte_size = src->getRegion()->vertStride * type_sz * num_rows;

        if (getDst() && getDst()->isNativePackedRegion() &&
            IS_WTYPE(src->getType())) {
            if (src->isNativePackedRegion()) {
                return true;
            }
            else if (src->isNativePackedRowRegion()) {
                return false;
            }
        }
        if (byte_size == 2 * G4_GRF_REG_NBYTES) {
            return true;
        }
        else {
            return false;
        }
    }
}

bool G4_INST::isAccSrcInst() const
{
    if (srcs[0] && srcs[0]->isSrcRegRegion() && srcs[0]->asSrcRegRegion()->getBase()->isAccReg())
    {
        return true;
    }
    else if (getNumSrc() == 3 && srcs[1] != nullptr)
    {
        if (srcs[1]->isSrcRegRegion() && srcs[1]->asSrcRegRegion()->getBase()->isAccReg())
        {
            return true;
        }
    }
    return false;
}

// Check if this instruction has an explicit acc destination
bool G4_INST::isAccDstInst() const
{

    if (dst != NULL && dst->getBase()->isAccReg() )
    {
        return true;
    }
    return false;
}

bool G4_INST::isArithAddr()
{
    if (srcs[1] != NULL)
        return isArithmetic() && srcs[1]->isAddrExp();
    else
        return false;
}

bool G4_INST::isMovAddr()
{
    if (srcs[0] != NULL)
        return isMov() && srcs[0]->isAddrExp();
    return false;
}

//
// Check if the operands of send instruction obey the symbolic register rule
// ToDo: this is obsolete and should be removed
//
bool G4_INST::isValidSymbolOperand(bool &dst_valid, bool *srcs_valid)
{
    MUST_BE_TRUE(srcs_valid, ERROR_INTERNAL_ARGUMENT);

    bool obeyRule = true;
    if (dst && dst->getBase()->isRegVar())
    {
        dst_valid = dst->obeySymbolRegRule();
        if (!dst_valid)
            obeyRule = false;
    }
    else
        dst_valid = false;                          // does not change obeyRule for non-register-variable

    for (unsigned i = 0; i < G4_MAX_SRCS; i++)
    {
        if (srcs[i] && srcs[i]->isSrcRegRegion() && srcs[i]->asSrcRegRegion()->getBase()->isRegVar())
        {
            srcs_valid[i] = srcs[i]->asSrcRegRegion()->obeySymbolRegRule();
            if (!srcs_valid[i])
                obeyRule = false;
        }
        else
            srcs_valid[i] = false;                  // does not change obeyRule for non-register-variable
    }

    return obeyRule;
}

bool G4_INST::isOptBarrier()
{
    if (op == G4_join)
    {
        return true;
    }

    if (isIntrinsic() && asIntrinsicInst()->getIntrinsicId() == Intrinsic::MemFence)
    {
        return true;
    }

    // any instructions that access special ARFs is considered a opt barrier
    // this includes any ARF that is not address/flag/acc
    if (dst != NULL)
    {
        if (dst->isAreg())
        {
            if (dst->isNReg() ||
                dst->isSrReg() ||
                dst->isCrReg() ||
                dst->isTmReg() ||
                dst->isTDRReg())
            {
                return true;
            }
        }
    }

    for (int i = 0; i < G4_Inst_Table[op].n_srcs; i++)
    {
        if (srcs[i] != NULL)
        {
            if (srcs[i]->isAreg())
            {
                if (srcs[i]->isNReg() ||
                    srcs[i]->isSrReg() ||
                    srcs[i]->isCrReg() ||
                    srcs[i]->isTmReg() ||
                    srcs[i]->isTDRReg())
                {
                    return true;
                }
            }
        }
    }
    return false;
}

bool G4_INST::isDirectSplittableSend()
{
    if (!isSend())
    {
        return false;
    }
    
    unsigned short elemSize = dst->getElemSize();
    CISA_SHARED_FUNCTION_ID funcID = msgDesc->getFuncId();
    unsigned subFuncID = msgDesc->getMessageType();

    switch (funcID)
    {
    case SFID_DP_DC1:
        switch (subFuncID)
        {
        case DC1_A64_SCATTERED_READ:   //emask need be vertically cut.
           return false;

        case DC1_A64_UNTYPED_SURFACE_READ:  //SVM gather 4: emask can be reused if the per-channel data is larger than 1 GRF
        case DC1_UNTYPED_SURFACE_READ:   //VISA gather 4
        case DC1_TYPED_SURFACE_READ:   //Gather 4 typed
            if (elemSize * execSize > G4_GRF_REG_NBYTES &&
            	elemSize * execSize % G4_GRF_REG_NBYTES == 0)
            {
                return true;
            }
            else
            {
                return false;
            }

        default: return false;
        }
    case SFID_DP_DC2:
        switch (subFuncID)
        {
        case DC2_UNTYPED_SURFACE_READ:   //gather 4 scaled :  emask can be reused if the per-channel data is larger than 1 GRF
        case DC2_A64_UNTYPED_SURFACE_READ: //SVM gather 4 scaled
            if (elemSize * execSize > G4_GRF_REG_NBYTES &&
            	elemSize * execSize % G4_GRF_REG_NBYTES == 0)
            {
                return true;
            }
            else
            {
                return false;
            }

        case DC2_BYTE_SCATTERED_READ:   //scaled byte scattered read: gather_scaled, handled as block read write, nomask
            return true;

        default: return false;
        }
    case SFID_DP_DC:
        switch (subFuncID)
        {
        case DC_DWORD_SCATTERED_READ:   //dword scattered read: emask need be vertically cut according to splitting
        case DC_BYTE_SCATTERED_READ:       //byte scattered read
            return false;
        case DC_UNALIGNED_OWORD_BLOCK_READ: //Nomask
        case DC_OWORD_BLOCK_READ:
            return true;
        default: return false;
        }
    case SFID_SAMPLER:
        return true;
    default: return false;
    }

    return false;
}

//
// emit send instruction with symbolic/physical register operand depending on the operand check
//
void G4_INST::emit_send(std::ostream& output, bool symbol_dst, bool *symbol_srcs)
{
    G4_INST* sendInst = this;

    if (sendInst->predicate)
    {
        sendInst->predicate->emit(output);
    }

    output << G4_Inst_Table[op].str;

    if (sendInst->mod)
    {
        sendInst->mod->emit(output);
    }
    if (sendInst->sat)
    {
        output << ".sat";
    }
    output << ' ';
    if (UNDEFINED_EXEC_SIZE != sendInst->execSize)
    {
        output << '(' << static_cast<int>(sendInst->execSize) << ") ";
    }

    sendInst->dst->emit(output, symbol_dst);
    output << ' ';
    G4_Operand* currSrc = sendInst->srcs[0];
    if (currSrc->isSrcRegRegion())
    {
        //
        // only output reg var & reg off; don't output region desc and type
        //
        ((G4_SrcRegRegion*)currSrc)->emitRegVarOff(output, false);
    }
    else
    {
        currSrc->emit(output, false); //emit CurrDst
    }
    output << ' ';

    if (isSplitSend())
    {
        // emit src1
        sendInst->srcs[1]->asSrcRegRegion()->emitRegVarOff(output, false);
        output << ' ';
    }

    // emit exDesc if srcs[3] is not null.  It should always be a0.2 unless it was constant folded
	if (isSplitSend() && srcs[3])
	{
		sendInst->srcs[3]->emit(output, false);
		output << ' ';
	}
	else
	{
        std::ios::fmtflags outFlags(output.flags());
        output.flags(std::ios_base::hex | std::ios_base::showbase);
        output << sendInst->getMsgDesc()->getExtendedDesc();
        output << ' ';
        output.flags(outFlags);
    }


    // emit msgDesc (2 for sends and 1 for send). Last operand shown in asm.
    int msgDescId = isSplitSend() ? 2 : 1;
    sendInst->srcs[msgDescId]->emit(output, false);

    sendInst->emit_options(output);
}

void G4_INST::emit_send_desc(std::ostream& output)
{
    G4_INST* sendInst = this;

    // Emit a text description of the descriptor if it is available
     G4_SendMsgDescriptor* msgDesc = sendInst->getMsgDesc();
    if( msgDesc != NULL )
    {
        output << " // ";

        if (msgDesc->getDescType() != NULL)
        {
            output << msgDesc->getDescType();
        }

        // Whether spill/fill
        if( sendInst->getSpillOrFill() == true )
        {
            if( msgDesc->isDataPortRead() || msgDesc->isScratchRead() )
                output << ", fill";
            else
                output << ", spill";

            if(msgDesc->isScratchRead() || msgDesc->isScratchWrite() )
            {
                output << ", offset = " << msgDesc->getScratchRWOffset();
            }
        }
        output << ", resLen=" << msgDesc->ResponseLength();
        output << ", msgLen=" << msgDesc->MessageLength();
        if (isSplitSend())
        {
			output << ", extMsgLen=" << msgDesc->extMessageLength();
        }

        if (msgDesc->isBarrierMsg())
        {
            output << ", barrier";
        }

    }
}

void
G4_INST::emit_send(std::ostream& output, bool dotStyle)
{

    if (pCisaBuilder->m_options.getOption(vISA_SymbolReg))
    {
        //
        // Emit as comment if there is invalid operand, then emit instruction based on the situation of operand
        //
        bool dst_valid = true;
        bool srcs_valid[G4_MAX_SRCS];
        if (!isValidSymbolOperand(dst_valid, srcs_valid))
        {
            if (!dotStyle)
            {
                bool srcs_valid1[G4_MAX_SRCS];
                for (unsigned i=0; i<G4_MAX_SRCS; i++)
                    srcs_valid1[i] = true;
                output << "//";
                emit_send(output, true, srcs_valid1);       // emit comments
                output << std::endl;
            }
        }
        emit_send(output, dst_valid, srcs_valid); 
    }
    else
        emit_send(output, false, NULL);  
}

//
// Add symbolic register support, emit instruction based on the situation of operands
//
void G4_INST::emit_inst(std::ostream& output, bool symbol_dst, bool *symbol_srcs)
{

    if ( op==G4_nop )
    {
        output << G4_Inst_Table[op].str;
        return;
    }

    if (isLabel())
    {
        srcs[0]->emit(output);
        output << ":";
        if(((G4_Label*)srcs[0])->isStartLoopLabel())
            output<<"\ndo";
    }
    else
    {
        if (predicate)
        {
            predicate->emit(output);
        }
        output << G4_Inst_Table[op].str;
        if (isIntrinsic())
        {
            output << "." << asIntrinsicInst()->getName();
        }

        if (mod)
        {
            mod->emit(output);
        }
        if (sat)
        {
            output << ".sat";
        }
        output << ' ';

        if (UNDEFINED_EXEC_SIZE != execSize && op != G4_nop && op!=G4_wait)
        {// no need to emit size for nop, wait
            output << '(' << static_cast<int>(execSize) << ") ";
        }
        if (dst)
        {
            dst->emit(output, symbol_dst);  // emit symbolic/physical register depends on the flag
            output << ' ';
        }

        for (unsigned i = 0; i < G4_Inst_Table[op].n_srcs; i++)
        {
            if (srcs[i])
            {
                if (symbol_srcs != NULL)
                {
                    srcs[i]->emit(output, symbol_srcs[i]);  // emit symbolic/physical register depends on the flag
                }
                else
                {
                    srcs[i]->emit(output, false);   // emit physical register
                }
                output << ' ';
            }
        }

        if( isMath() && asMathInst()->getMathCtrl() != MATH_RESERVED )
        {
            output << (hex) << "0x" << asMathInst()->getMathCtrl() << (dec) << " ";
        }

        if( isFlowControl() && asCFInst()->getJip() )
        {
            asCFInst()->getJip()->emit(output);
            output << ' ';
        }

        if( isFlowControl() && asCFInst()->getUip() )
        {
            asCFInst()->getUip()->emit(output);
            output << ' ';
        }

        if( op == G4_goto )
        {
            if (asCFInst()->isBackward())
            {
                output << 1;
            }
            else
            {
                output << 0;
            }
            output << ' ';
        }
        this->emit_options(output);
        //
        // emit src2 of pln as comments
        //
        if (op == G4_pln)
        {
            if (srcs[2] != NULL)
            {
                output << "\t//";
                srcs[2]->emit(output, false);
            }
        }
    }
}

//
// Here we add a parameter symbolreg instead of use global option Options::symbolReg, because we should ouput non-symbolic register when dumping dot files
//
void G4_INST::emit(std::ostream& output, bool symbolreg, bool dotStyle)
{
    bool dst_valid = true;
    bool srcs_valid[G4_MAX_SRCS];

    if (symbolreg)
    {
        if (op==G4_nop || isLabel())
        {
            emit_inst(output, false, NULL);
            return;
        }

        //
        // Emit as comment if there is invalid operand, then emit instruction based on the situation of operand
        //
        if (!isValidSymbolOperand(dst_valid, srcs_valid))
        {
            if (!dotStyle)
            {
                output << "//";
                bool srcs_valid1[G4_MAX_SRCS];
                for (unsigned i = 0; i < G4_MAX_SRCS; i++)
                    srcs_valid1[i] = true;
                emit_inst(output, true, srcs_valid1);           // emit comments
                output << std::endl;
            }
        }
        emit_inst(output, dst_valid, srcs_valid)    ;       // emit instruction
    }
    else
        emit_inst(output, false, NULL)  ;       // emit instruction with physical register
}

std::ostream& operator<<(std::ostream& os, G4_INST& inst)
{
    inst.emit(os, false, false);
    return os;
}

void
G4_INST::emit_options(std::ostream& output)
{
    unsigned int tmpOption = this->option;

    if( isEOT() )
    {
        tmpOption |= InstOpt_EOT;
    }

    // must set AccWrCtrl explicitly in GT+
    if( isAccWrCtrlInst() || (isFlowControl() && asCFInst()->isBackward()) ) {
        tmpOption |= InstOpt_AccWrCtrl;
    }

    //emit mask option
    output << "{";
    switch (getMaskOffset())
    {
    case 0:
        output << (execSize == 4 ? "N1" : (execSize == 16 ? "H1" : "Q1"));
        break;
    case 4:
        output << "N2";
        break;
    case 8:
        output << (execSize == 4 ? "N3" : "Q2");
        break;
    case 12:
        output << "N4";
        break;
    case 16:
        output << (execSize == 4 ? "N5" : (execSize == 16 ? "H2" : "Q3"));
        break;
    case 20:
        output << "N6";
        break;
    case 24:
        output << (execSize == 4 ? "N7" : "Q4");
        break;
    case 28:
        output << "N8";
        break;
    default:
        assert(false && "unexpected mask offset");
        break;
    }
    output << ", ";

    tmpOption &= ~InstOpt_QuarterMasks;
    if (0 != tmpOption)
    {
        if (isAligned1Inst())
        {
            output << "Align1, ";
        }

        unsigned short optIdx = 0;
        while (0xFFFFFFFF != InstOptInfo[optIdx].optMask)
        {
            if (tmpOption & InstOptInfo[optIdx].optMask)
            {
                output << InstOptInfo[optIdx].optStr;
                tmpOption &= ~InstOptInfo[optIdx].optMask; //clear this bit;
                if (tmpOption != 0x0)
                {
                    output << ", ";
                }
            }
            optIdx++;
        }
        output << '}';
    }
    else
    {
        //just print align1
        output << "Align1}";
    }
}


static const char* const operandString[] =
{
    OPND_NUM_ENUM(STRINGIFY)
};

void G4_INST::emitDefUse(std::ostream& output)
{
    output << "Def:\n";
    for (auto iter = defInstList.begin(), iterEnd = defInstList.end(); iter != iterEnd; ++iter)
    {
        G4_INST* inst = (*iter).first;
        inst->emit(output);
        output << "\t" << operandString[(*iter).second];
        output << "\n";
    }
    output << "Use:\n";
    for (auto iter = useInstList.begin(), iterEnd = useInstList.end(); iter != iterEnd; ++iter)
    {
        G4_INST* inst = (*iter).first;
        inst->emit(output);
        output << "\t" << operandString[(*iter).second];
        output << "\n";
    }
}

bool G4_INST::isMixedMode() const
{
    bool mixedMode = false;
	if (isSend())
	{
		return false;
	}
    for(int i = 0; i < G4_Inst_Table[opcode()].n_srcs; ++i)
    {
        G4_Operand *tOpnd = getSrc(i);
        if(!tOpnd)
        {
            continue;
        }

        G4_Type srcType = tOpnd->getType();

        if(getDst() &&
            (getDst()->getType() == Type_HF ||
            srcType == Type_HF ) &&
            getDst()->getType() != srcType)
        {
            mixedMode =  true;
            break;
        }
    }

    return mixedMode;
}

// print r#
void G4_Greg::emit(std::ostream& output, bool symbolreg)
{
    output << "r" << getRegNum();
}

void G4_Areg::emit(std::ostream& output, bool symbolreg)
{
    switch (getArchRegType())
    {
    case AREG_NULL:    output << "null";  break;
    case AREG_A0:      output << "a0";    break;
    case AREG_ACC0:    output << "acc0";  break;
    case AREG_ACC1:    output << "acc1";  break;
    case AREG_MASK0:   output << "ce0";   break;
    case AREG_MS0:     output << "ms0";   break;
    case AREG_DBG:     output << "dbg0";  break;
    case AREG_SR0:     output << "sr0";   break;
    case AREG_CR0:     output << "cr0";   break;
    case AREG_TM0:     output << "tm0";   break;
    case AREG_N0:      output << "n0";    break;
    case AREG_N1:      output << "n1";    break;
    case AREG_IP:      output << "ip";    break;
    case AREG_F0:      output << "f0";    break;
    case AREG_F1:      output << "f1";    break;
    case AREG_TDR0:    output << "tdr0";  break;
    case AREG_SP:      output << "sp";    break;
    default:
        output << "unknown architecture reg";
        MUST_BE_TRUE(false, ERROR_UNKNOWN);
    }
}

//
// initial all values idential to rgn's
//
G4_SrcRegRegion::G4_SrcRegRegion(G4_SrcRegRegion &rgn)
    : G4_Operand(G4_Operand::srcRegRegion)
{
    base = rgn.base;
    acc = rgn.acc;
    regOff = rgn.regOff;
    subRegOff = rgn.subRegOff;
    mod = rgn.mod;
    immAddrOff = rgn.immAddrOff;
    desc = rgn.desc;
    type = rgn.type;
    // copy swizzle value
    char *sw1 = swizzle, *sw2 = rgn.swizzle;
    while (*sw2) *sw1++ = *sw2++;
    *sw1 = *sw2;
    accRegSel = rgn.accRegSel;

    bitVec[0] = rgn.bitVec[0];
    bitVec[1] = rgn.bitVec[1];
    bitVec[2] = rgn.bitVec[2];
    top_dcl = rgn.top_dcl;
    left_bound = rgn.left_bound;
    right_bound = rgn.right_bound;
    byteOffset = rgn.byteOffset;
    rightBoundSet = rgn.rightBoundSet;
}
//
// Initialize all values idential to rgn's, except for the base operand.
// Caller is responsible for allocating base operand and making sure it doesn't
// mess up the operands' hash table.
//
G4_SrcRegRegion::G4_SrcRegRegion(G4_SrcRegRegion &rgn, G4_VarBase *new_base)
    : G4_Operand(G4_Operand::srcRegRegion)
{
    acc = rgn.acc;
    regOff = rgn.regOff;
    subRegOff = rgn.subRegOff;
    mod = rgn.mod;
    immAddrOff = rgn.immAddrOff;
    desc = rgn.desc;
    type = rgn.type;
    // copy swizzle value
    char *sw1 = swizzle, *sw2 = rgn.swizzle;
    while (*sw2) *sw1++ = *sw2++;
    *sw1 = *sw2;

    base = new_base;

    computeLeftBound();
    rightBoundSet = false;
}
//
// return true if rng and this have the same reg region
//
bool G4_SrcRegRegion::sameSrcRegRegion(G4_SrcRegRegion& rgn)
{
    return base == rgn.base &&
           acc == rgn.acc &&
           mod == rgn.mod &&
           strcmp(swizzle,rgn.swizzle) == 0 &&
           desc == rgn.desc &&
           regOff == rgn.regOff &&
           subRegOff == rgn.subRegOff &&
           immAddrOff == rgn.immAddrOff &&
           type == rgn.type &&
           accRegSel == rgn.accRegSel;
}

// compute max execution size starting from the current pos.
// power of two. cross-GRF boundary is allowed if the region is evenly split.
// cross half-GRF should guaranttee evenly split
uint8_t G4_SrcRegRegion::getMaxExecSize(int pos, uint8_t maxExSize, bool allowCrossGRF, uint16_t &vs, uint16_t &wd, bool &twoGRFsrc)
{
    if (isRightBoundSet() == false)
    {
        getInst()->computeRightBound(this);
    }

    twoGRFsrc = false;
    vs = 0;
    wd = 0;
    if (isScalar())
    {
        vs = 0;
        wd = 1;
        return maxExSize;
    }
    else if (acc != Direct)
    {
        if (getGenxPlatform() >= GENX_SKL && (desc->vertStride != desc->width * desc->horzStride))
        {
            // this operand should be split as
            // op (wd) r[a0.0, 0]<hs; 1, 0>
            // op (wd) r[a0.0, vs*typeSize]<hs; 1, 0>
            vs = desc->horzStride;
            wd = 1;
            return (uint8_t)desc->width;
        }
        else
        {
            // assume this operand is kosher (i.e., does not cross GRF) as the vISA spec requires it
            vs = desc->vertStride;
            wd = desc->width;
            return roundDownPow2(maxExSize);
        }
    }

    // align16 operands
    if (desc->isRegionV())
    {
        vs = desc->vertStride;
        wd = desc->width;
        if (desc->horzStride == 0)
        {
            return roundDownPow2(maxExSize);
        }

        uint32_t elSize = G4_Type_Table[type].byteSize;
        uint8_t maxSize = 0;

        uint32_t prevPos = pos * elSize;
        uint8_t numEleInFristGRF = 0, numEleInSecondGRF = 0;
        uint32_t newLB = getLeftBound() + prevPos;
        bool crossGRF = (newLB / G4_GRF_REG_NBYTES != getRightBound() / G4_GRF_REG_NBYTES),
            inFirstGRF = true;

        for (int i = pos + 4; i < (pos + maxExSize); i += 4)
        {
            uint32_t currPos = i * elSize;

            // check cross GRF boundary
            if (crossGRF && inFirstGRF)
            {
                uint32_t newRB = getLeftBound() + currPos - 1;
                uint32_t leftGRF = newLB / G4_GRF_REG_NBYTES, rightGRF = newRB / G4_GRF_REG_NBYTES;
                if (leftGRF != rightGRF)
                {
                    inFirstGRF = false;
                    numEleInFristGRF = maxSize;
                    newLB = newRB;
                }
            }

            maxSize += 4;

            if (numEleInFristGRF)
            {
                numEleInSecondGRF += 4;
                if (numEleInSecondGRF == numEleInFristGRF)
                {
                    twoGRFsrc = true;
                    break;
                }
            }
        }
        if (numEleInSecondGRF < numEleInFristGRF)
        {
            twoGRFsrc = false;
            maxSize = numEleInFristGRF;
        }
        return maxSize;
    }

    // align1 direct
    uint32_t elSize = G4_Type_Table[type].byteSize;
    uint8_t maxSize = 1;

    bool alignToRow = pos % desc->width == 0;

    // region may not be contiguous/single stride depending on the start position
    bool contRegion = desc->isContiguous(maxExSize + (pos % desc->width));

    uint16_t vStride = 1;
    if (contRegion || desc->isSingleNonUnitStride(maxExSize + (pos % desc->width), vStride))
    {
        // apparently the old code actually allows GRF-crossing as long as it's evenly divided
        // (the function comment lied), so we have to try all exec sizes from the largest possible. sigh..
        vs = vStride;
        wd = 1;
        // we need to be careful with start byte here since maxExSize may not be same as inst exec size
        // e.g., say this is called on
        // mov (16) V44_m(2,0)<1>:f V43_in(1,19)<16;8,1>:ub
        // with pos 8 and maxExSize 8
        // the region is considered single stride in this case, but is not with the original exsize (16),
        // so we can't just multiply stride with type size to get starting offset
        uint32_t startByte = (getLeftBound() + getByteOffset(pos)) % GENX_GRF_REG_SIZ;
        int retExecSize = 1;
        int execTypeSize = vStride * getElemSize();
        int exSizes[] = { 32, 16, 8, 4, 2 };

        for (auto size : exSizes)
        {
            if (maxExSize < size)
            {
                continue;
            }
            if (startByte + (size - 1) * execTypeSize + getElemSize() <= GENX_GRF_REG_SIZ)
            {
                // no GRF crossing (we don't count the padding bytes after the last element)
                retExecSize = size;
                break;
            }
            else if (allowCrossGRF)
            {
                int numEltInFirstGRF = (GENX_GRF_REG_SIZ - startByte) / execTypeSize;
                // startByte may not be aligned to exec type size (e.g., r1.1<2;1,0>:b).  We need to increment by 1 in this case
                if ((GENX_GRF_REG_SIZ - startByte) % execTypeSize != 0)
                {
                    numEltInFirstGRF += 1;
                }
                if (numEltInFirstGRF == size - numEltInFirstGRF)
                {
                    twoGRFsrc = true;
                    retExecSize = size;
                    break;
                }
            }
        }

        return (uint8_t)retExecSize;
    }

    // conservative.
    // Here we assume that no cross width if row size is larger than width
    // mul (16) V112(0,0)<1>:f V111(0,0)<16;16,1>:f r1.0<1;4,0>:f
    if( !alignToRow && !contRegion && desc->vertStride != 0 && desc->horzStride != 0 )
    {
        wd = vs = (uint16_t)roundDownPow2( ( pos/desc->width + 1 ) * desc->width - pos );

        // Need to check whether this subregion crosses grf or not.
        // E.g. the second half does cross a grf:
        // mov (8) V41(0, 9)<1> V58(2, 8)<32;8,4>
        //
        // Given a linearized index, compute its byte offset relative to the
        // first element (index 0).
        auto computeOffset = [=](unsigned index) -> unsigned {
            unsigned typeSize = G4_Type_Table[type].byteSize;
            unsigned offset = (index % desc->width) * desc->horzStride * typeSize;
            offset += (index / desc->width) * desc->vertStride * typeSize;
            return offset;
        };

        // Since a single element cannot cross a grf, checking the first byte of the
        // first and last element is sufficient.
        // FIXME: fix other places with this logic.
        unsigned firstPos = getLeftBound() + computeOffset((unsigned)pos);
        unsigned lastPos = getLeftBound() + computeOffset((unsigned)(pos + wd - 1));
        twoGRFsrc = firstPos / G4_GRF_REG_NBYTES != lastPos / G4_GRF_REG_NBYTES;

        return (uint8_t)wd;
    }

    uint8_t posInFirstRow = pos%desc->width, eleInRow = 1, eleInFirstRow = desc->width - posInFirstRow;
    uint8_t pow2 = roundDownPow2( eleInFirstRow );

    if( eleInFirstRow != pow2 && !contRegion )
    {
        wd = pow2;
        vs = wd * desc->horzStride;
        return pow2;
    }

    uint32_t prevPos = ( pos/desc->width * desc->vertStride + posInFirstRow * desc->horzStride ) * elSize;
    uint8_t numEleInFristGRF = 0, numEleInSecondGRF = 0;
    bool crossRow = false;
    uint32_t newLB = getLeftBound() + prevPos;
    bool crossGRF = ( newLB / G4_GRF_REG_NBYTES != getRightBound() / G4_GRF_REG_NBYTES),
        inFirstGRF = true;
    bool negVS = ( desc->vertStride < desc->horzStride * desc->width );

    for( int i = pos + 1; i < ( pos + maxExSize ); i++ )
    {
        uint8_t posInRow = i % desc->width;
        uint32_t currPos = ( ( i / desc->width ) * desc->vertStride + posInRow * desc->horzStride ) * elSize;

        // check cross row boundary
        if( ( !contRegion || desc->vertStride == 0 ) && posInRow == 0 )
        {
            uint8_t pow2Val = roundDownPow2( eleInRow );
            if( pow2Val != eleInRow  ||
                ( ( desc->vertStride == 0 || negVS ) && !alignToRow ) )
            {
                // this happens in the first row
                wd = maxSize = pow2Val;
                vs = wd * desc->horzStride;
                break;
            }
            else if( wd == 0 )
            {
                // <2;4,1>
                wd = eleInRow;
                if( alignToRow )
                {
                    vs= desc->vertStride;
                }
                else
                {
                    vs = ( currPos - prevPos ) / elSize;
                }
            }
            crossRow = true;
            eleInRow = 0;
        }

        // check cross GRF boundary
        if( crossGRF && inFirstGRF )
        {
            uint32_t newRB = getLeftBound() + currPos + elSize - 1;
            uint32_t leftGRF = newLB / G4_GRF_REG_NBYTES, rightGRF = newRB / G4_GRF_REG_NBYTES;
            if( leftGRF != rightGRF )
            {
                inFirstGRF = false;
                uint8_t pow2Val = roundDownPow2( maxSize );

                // if number of element in first GRF is not power of 2, or
                // subregister offset of two GRFs are different and not contiguous( too conservative? )
                if( pow2Val != maxSize ||
                    ( !contRegion && !( alignToRow && maxSize <= desc->width ) && newLB % G4_GRF_REG_NBYTES != (getLeftBound() + currPos) % G4_GRF_REG_NBYTES ) )
                {
                    maxSize = pow2Val;
                    if( wd == 0 )
                    {
                        wd = pow2Val;
                        vs = wd * desc->horzStride;
                    }
                    break;
                }
                else if( wd == 0 )
                {
                    wd = maxSize < desc->width ? maxSize : desc->width;
                    vs = ( currPos - prevPos ) / elSize;
                }
                numEleInFristGRF = maxSize;
                newLB = newRB;
            }
        }

        maxSize++;
        eleInRow++;
        // make sure the number of elements in two rows are the same
        if( crossRow && eleInRow == eleInFirstRow && !alignToRow && !contRegion )
        {
            break;
        }

        if( numEleInFristGRF )
        {
            numEleInSecondGRF++;
            if( numEleInSecondGRF == numEleInFristGRF )
            {
                twoGRFsrc = true;
                break;
            }
        }
    }
    if( wd == 0 )
    {
        // contiguous region
        wd = pow2;
        vs = wd * desc->horzStride;
    }
    if( numEleInSecondGRF < numEleInFristGRF )
    {
        maxSize = numEleInFristGRF;
    }
    return maxSize;
}

//
// output (Var+refOff).subRegOff
//
void printRegVarOff(std::ostream&  output,
                    G4_Operand*    opnd,
                    short          regOff,  // base+regOff is the starting register
                    short          subRegOff, // sub reg offset
                    short          immAddrOff, // imm addr offset
                    G4_Type        type,
                    bool symbolreg,
                    bool printSubReg)
//
// symbolreg == false,  output physcial register operand
// symbolreg == true,   output symbolic register operand
// Here we use symbolreg instead of Options::symbolReg, because sometimes we need to emit an instruction with invalid symbolic register as comment and
// then emit a workable instruction with physical register. In this case, we do not want to re-set the global option variable Options::symbolReg to switch
// between these two states, that may have potential side effects.
//
{
    short subRegOffset = (subRegOff != (short) UNDEFINED_SHORT) ? subRegOff : 0;

    G4_RegAccess acc = opnd->getRegAccess();
    G4_VarBase* base = opnd->getBase();
    if (acc == Direct)
    {
        MUST_BE_TRUE(regOff != (short) UNDEFINED_SHORT,
            ERROR_INTERNAL_ARGUMENT);

        if (base->isRegVar())
        {
            G4_RegVar* baseVar = static_cast<G4_RegVar*>(base);
            int declOpSize(G4_Type_Table[baseVar->getDeclare()->getElemType()].byteSize);
            int thisOpSize(G4_Type_Table[type].byteSize);

            if (baseVar->isPhyRegAssigned())
            {

                if (symbolreg && !base->isFlag())
                {
                    //
                    // No matter the type of register and if the allocation successed, we output format <symbol>(RegOff, SubRegOff)
                    // Note: we have check if the register allocation  successed when emit the declare!
                    //
                    if (pCisaBuilder->m_options.getOption(vISA_UniqueLabels))
                    {
                        const char *labelStr = nullptr;
                        pCisaBuilder->m_options.getOption(vISA_LabelStr, labelStr);
                        if (labelStr != nullptr)
                        {
                        output << labelStr << "_" << base->asRegVar()->getName() << "(" << regOff << "," << subRegOff << ")";
                        }
                    } else
                    {
                        output << base->asRegVar()->getName() << "(" << regOff << "," << subRegOff << ")";
                    }
                    return;
                }

                if (baseVar->getPhyReg()->isGreg())
                {
                     uint32_t byteAddress = opnd->getLinearizedStart();
                     int regNum = 0, subRegNum = 0;
                     if (byteAddress != 0)
                     {
                         regNum = byteAddress / GENX_GRF_REG_SIZ;
                         subRegNum = ( byteAddress % GENX_GRF_REG_SIZ ) / G4_Type_Table[type].byteSize;
                     }
                     else
                     {
                         // before RA, use the value in Greg directly
                         regNum = baseVar->getPhyReg()->asGreg()->getRegNum();
                         subRegNum = baseVar->getPhyRegOff();
                     }

                     output << "r" << regNum;
                     if (printSubReg)
                     {
                         output << "." << subRegNum;
                     }
                }
                else if (baseVar->getPhyReg()->isAreg())
                {
                    MUST_BE_TRUE(regOff == 0, ERROR_INTERNAL_ARGUMENT);

                    (static_cast<G4_Areg*>(baseVar->getPhyReg()))->emit(output);
                    if (!baseVar->isNullReg())
                    {
                        unsigned ArfSubRegNum = baseVar->getPhyRegOff();

                        //ArfSubRegNum is in unit of declOpSize
                        //transform ArfSubRegNum to unit of thisOpSize
                        if (thisOpSize != declOpSize)
                        {
                            if (opnd->getInst()->opcode() != G4_pseudo_kill)
                            {
                                MUST_BE_TRUE((ArfSubRegNum * declOpSize) % thisOpSize == 0,
                                    ERROR_DATA_RANGE("ARF sub-register number"));
                            }
                            ArfSubRegNum = (ArfSubRegNum * declOpSize) / thisOpSize;
                        }

                        unsigned subreg = ArfSubRegNum + subRegOffset;
                        output << '.' << subreg;
                    }
                }
                else
                    MUST_BE_TRUE(false, ERROR_UNKNOWN);
            }
            else        // physical register not allocated
            {
                baseVar->emit(output);
                output << '(' << regOff << ',' << subRegOff << ')';
            }
        }
        else //This is not a RegVar
        {
            if (base->isAccReg() && regOff != 0)
            {
                bool valid;
                int regNum = base->ExRegNum(valid);
                output << "acc" << regNum + regOff;
            }
            else
            {
                base->emit(output);
            }
            if (!base->isNullReg() && !base->isIpReg() && !base->isNReg() && subRegOff != (short) UNDEFINED_SHORT && printSubReg)
            {
                output << '.' << subRegOff;
            }
        }
    }
    else //This is an indirect access
    {
        if (acc == IndirGRF)
        {
            output << "r[";
        }
        else //Unknown access type
        {
            MUST_BE_TRUE(false, ERROR_UNKNOWN);
        }

        if (base->isRegVar())
        {
            MUST_BE_TRUE(regOff == 0, ERROR_INTERNAL_ARGUMENT);
            G4_RegVar* baseVar = static_cast<G4_RegVar*>(base);
            if (baseVar->isPhyRegAssigned())
            {
                MUST_BE_TRUE(baseVar->getPhyReg()->isAreg(), ERROR_UNKNOWN);

                if (symbolreg)
                {
                    if (pCisaBuilder->m_options.getOption(vISA_UniqueLabels)) {
                        const char* labelStr = nullptr;
                        pCisaBuilder->m_options.getOption(vISA_LabelStr, labelStr);
                        if (labelStr != nullptr)
                        {
                        output << labelStr << "_";
                    }
                    }
                    output << baseVar->getName();
                    output << '(' << regOff << ',' << subRegOffset << ")," << immAddrOff << ']';
                }
                else
                {
                    (static_cast<G4_Areg*>(baseVar->getPhyReg()))->emit(output);
                    output << '.' << (baseVar->getPhyRegOff() + subRegOffset);
                    output << ", " << immAddrOff << ']';
                }
            }
            else //No register assigned yet
            {
                baseVar->emit(output);
                output << '(' << regOff << ',' << subRegOff << ')';
                output << ", " << immAddrOff << ']';
            }
        }
        else if (base->isAreg())
        {
            (static_cast<G4_Areg*>(base))->emit(output);
            output << '.' << subRegOffset;
            output << ", " << immAddrOff << ']';
        }
        else
        {
            MUST_BE_TRUE(false, "Unknown base variable type for indirect access");
        }
    }
}

//
// output <modifier>(Var+refOff).subRegOff<16;16,1>.xxyy
//
// symbolreg == false,  output <modifier>(Var+refOff).subRegOff<16;16,1>.xxyy
// symbolreg == true,   output <modifier><symbol>(RegOff, SubRegOff)<16;16,1> in symbolic register emit
// Here we use symbolreg instead of Options::symbolReg, because sometimes we need to emit an instruction with invalid symbolic register as comment and
// then emit a workable instruction with physical register. In this case, we do not want to re-set the global option variable Options::symbolReg to switch
// between these two states, that may have potential side effects.
//
void G4_SrcRegRegion::emit(std::ostream& output, bool symbolreg)
{
    if (mod != Mod_src_undef)
    {
        output << SrcModifierStr[mod];
    }

    //
    // output Var(refOff,subRegOff)
    //
    emitRegVarOff(output, symbolreg);
    //
    // output <vertStride;width,horzStride>
    //
    // do not emit region for null reg
    // do not emit region for macro madm
    if (desc && !base->isNullReg() && !base->isNReg() && !isAccRegValid())// rgn == NULL, the default region is used
    {
		bool align1ternary = getGenxPlatform() >= GENX_CNL && inst != NULL && inst->getNumSrc() == 3 &&
			!inst->isSend() && inst->isAligned1Inst();

        // RegionV is invalid for SRC operands
        if(desc->isRegionWH())
        {
            output << "<" << desc->width << "," << desc->horzStride << ">";
        }
        else if (desc->isRegionSW()) // support <0/4> for Src of Align16 instruction
        {
            output << "<" << desc->vertStride << ">";
        }
        else if ( desc->vertStride == UNDEFINED_SHORT && desc->width == UNDEFINED_SHORT )
        {
            output << "<" << desc->horzStride << ">";
        }
        else
        {
			if (align1ternary)
			{
				// format is <V;H> with W derived from V and H
				output << "<" << desc->vertStride << ";" << desc->horzStride << ">";
			}
            else if (!isWithSwizzle())
            {
				// do not print region for align16 sources
                output << "<" << desc->vertStride << ";" << desc->width << "," << desc->horzStride << ">";
            }
        }
    }

    if (isAccRegValid())
    {
        // no vertical stride for 3-source instruction
        if (inst->getNumSrc() != 3)
        {
            output << "<" << desc->vertStride << ">";
        }

        // output acc2~acc9
        if (getAccRegSel() == NOACC)
        {
            output << ".noacc";
        }
        else
        {
            output <<".acc"<< (getAccRegSel()+2);
        }
    }
	else if (*swizzle)
	{
		output << "." << swizzle;
	}

    if (Type_UNDEF != type)
    {
        if (!symbolreg || acc != Direct)                // can output register data type for indirect addressing in any time
            output << ':' << G4_Type_Table[type].str;
    }
}

//

// return true if this src is a scalar

// V82(1,0)<0>.xxxx:f

// V82(1,0)<0;1,0>:f  --- detect via

//

bool G4_SrcRegRegion::isScalar()

{

    if (!isWithSwizzle())
    {

        return getRegion()->isScalar(); // check <0;1,0>
    }
    else
    {
        return swizzle[0] == 'r';
    }

}


//
// This function is used to check if the src operand obey the rule of symbolic register. We need this function to check the operand before we emit an instruction
//
bool G4_SrcRegRegion::obeySymbolRegRule()
{
    if (!base->isRegVar())          // only for reg var
        return false;

    if (base->asRegVar()->getDeclare()->isSpilled())
    {
        return false;
    }

    //
    // Rule-3: No swizzle .xyzw
    //
    if (*swizzle)
    {
        return false;
    }
    //
    // Rule-4: do not support date type redefinition in direct addressing
    //
    if (Type_UNDEF != type)
    {
         if (base->isRegVar() && acc == Direct && base->asRegVar()->getDeclare()->getElemType() != type)        // check if the data type is the same as in declare
         {
             return false;
         }
    }

    return true;
}

//
// Here we use symbolreg instead of Options::symbolReg, because sometimes we need to emit an instruction with invalid symbolic register as comment and
// then emit a workable instruction with physical register. In this case, we do not want to re-set the global option variable Options::symbolReg to switch
// between these two states, that may have potential side effects.
//
void G4_SrcRegRegion::emitRegVarOff(std::ostream& output, bool symbolreg)
{
    bool printSubReg = true;
    if (inst != NULL && inst->isSend())
    {
        printSubReg = false;
    }
    printRegVarOff(output, this, regOff,subRegOff,immAddrOff,type, symbolreg, printSubReg);
}

//
// initial all values idential to rgn's
//
G4_DstRegRegion::G4_DstRegRegion(G4_DstRegRegion &rgn)
    : G4_Operand(G4_Operand::dstRegRegion)
{
    acc = rgn.acc;
    base = rgn.base;
    regOff = rgn.regOff;
    subRegOff = rgn.subRegOff;
    immAddrOff = rgn.immAddrOff;
    horzStride = rgn.horzStride;
    type = rgn.type;
    writeMask = rgn.writeMask;
    accRegSel = rgn.accRegSel;

    top_dcl = rgn.top_dcl;
    left_bound = rgn.left_bound;
    right_bound = rgn.right_bound;
    bitVec[0] = rgn.bitVec[0];
    bitVec[1] = rgn.bitVec[1];
    bitVec[2] = rgn.bitVec[2];
    byteOffset = rgn.byteOffset;
    rightBoundSet = rgn.rightBoundSet;
}

void G4_DstRegRegion::computeLeftBound()
{
    top_dcl = NULL;
    uint32_t newregoff = regOff, offset = 0;
    if( base && base->isRegVar() )
    {
        top_dcl = base->asRegVar()->getDeclare();
        if( !top_dcl && base->asRegVar()->isGreg() )
        {
            newregoff = base->asRegVar()->asGreg()->getRegNum();
        }
    }

    if( top_dcl )
    {
        while( top_dcl->getAliasDeclare() )
        {
            offset += top_dcl->getAliasOffset();
            top_dcl = top_dcl->getAliasDeclare();
        }
    }

    if (base != NULL && base->isFlag())
    {
        if( base->isRegVar() )
		{
			if (base->asRegVar()->getPhyReg())
			{
				left_bound = base->asRegVar()->getPhyRegOff() * 16;   // the bound of flag register is in unit of BIT
				left_bound += subRegOff * 16;
				if (((G4_Areg*)(base->asRegVar()->getPhyReg()))->getArchRegType() == AREG_F1)
				{
					left_bound += 32;
				}
			}
			else
			{
				left_bound = subRegOff * 16;
			}
        }
		else
		{
            left_bound = subRegOff * 16;
            if( ((G4_Areg*)(base))->getArchRegType() == AREG_F1 ){
                left_bound += 32;
            }
        }

        byteOffset = left_bound / 8;
    }
    else if ( base != NULL && base->isAccReg())
    {
        left_bound = subRegOff * G4_Type_Table[type].byteSize;
        if( ((G4_Areg*)(base))->getArchRegType() == AREG_ACC1 || regOff == 1 )
        {
            left_bound += 32;  // TODO: size of ACC is assumed to be 32 BYTEs.
        }
        byteOffset = left_bound;
    } else if( top_dcl ){
        if( acc == Direct ){
            left_bound = offset + newregoff * G4_GRF_REG_NBYTES + subRegOff * G4_Type_Table[type].byteSize;
            if( top_dcl->getTotalElems() * top_dcl->getElemSize() >= GENX_GRF_REG_SIZ ){
                byteOffset = left_bound;
            }
            else{
                unsigned alignOff = G4_Type_Table[type].byteSize > G4_Type_Table[Type_W].byteSize ?
                    G4_Type_Table[type].byteSize : G4_Type_Table[Type_W].byteSize;

                if( top_dcl->getSubRegAlign() == Even_Word || top_dcl->getSubRegAlign() >= Four_Word ){
                    alignOff = top_dcl->getSubRegAlign() * 2;
                }

                byteOffset = left_bound + alignOff;
            }
        }
        else{
            left_bound = subRegOff * G4_Type_Table[ADDR_REG_TYPE].byteSize;
            byteOffset = G4_Type_Table[type].byteSize;
        }
    }else{  //arch reg
        left_bound = 0;
        byteOffset = left_bound;
    }
}
//
// Initialize all values idential to rgn's, except for the base operand.
// Caller is responsible for allocating base operand and making sure it doesn't
// mess up the operands' hash table.
//
G4_DstRegRegion::G4_DstRegRegion(G4_DstRegRegion &rgn, G4_VarBase *new_base)
    : G4_Operand(G4_Operand::dstRegRegion)
{
    acc = rgn.acc;
    regOff = rgn.regOff;
    subRegOff = rgn.subRegOff;
    immAddrOff = rgn.immAddrOff;
    horzStride = rgn.horzStride;
    type = rgn.type;
    writeMask = rgn.writeMask;
    base = new_base;

    computeLeftBound();
    rightBoundSet = false;
}

unsigned G4_DstRegRegion::computeRightBound( uint8_t exec_size )
{

    bitVec[0] = 0;
    bitVec[1] = 0;

    unsigned short startPoint = 0;
    if( base->isFlag() ){
        unsigned int totalBits = 0;
		if (G4_Inst_Table[inst->opcode()].instType != InstTypePseudoLogic)
		{
			// mov (1) f0.1<1>:uw ...
			// subreg is 1 if it's a 32 bit flag and we want to set the upper 16 bits
			left_bound = subRegOff * 16;
			totalBits = G4_Type_Table[type].bitSize;
		}
		else
        {
            /*
                we need to set leftBound for pseudo intruction
                so that it creates use/def links correctly in the control flow graph between
                cmp instruction and pseudo instruction.
                This matters when we break up SIMD32 instruction in to two SIMD16 with H1/H2 masks.
                The bound for compare for H2 will be [15,31], and this has to match.
                Without this no use/def link was created which caused issues in logic optimization.
                Also it produce incorrect behavior in any operation that relies on compareOperand.
            */
            left_bound = inst->getMaskOffset();
            totalBits = exec_size;
            startPoint = (unsigned short)left_bound;
        }

        right_bound = left_bound + totalBits - 1;

        bitVec[0] = totalBits == 32 ? 0xFFFFFFFF : (1 << totalBits) - 1;
    }
    else
    {
        // For call, the return addr is always set as if simd2.
        if (inst->isCall())
        {
            exec_size = 2;
        }

        if (acc == Direct)
        {
            // byte level footprint computing bit vectors.
            uint64_t footprint = 0;
            unsigned short type_size = (unsigned short)G4_Type_Table[type].byteSize;
            unsigned short s_size = horzStride * type_size;

            // A common case: V<1>:d or V<1>:f
            if (horzStride == 1 && type_size == 4)
            {
                MUST_BE_TRUE(exec_size <= 16, "execeding two grfs?");
                // Some footprints for common dst regions with dw type.
                uint64_t footprints[] = {
                    ((uint64_t)1 <<  4) - 1, // size = 1
                    ((uint64_t)1 <<  8) - 1, // size = 2
                    ((uint64_t)1 << 16) - 1, // size = 4
                    ((uint64_t)1 << 32) - 1, // size = 8
                    ULLONG_MAX               // size = 16
                };

                unsigned index = (exec_size ==  1) ? 0 :
                                 (exec_size ==  8) ? 3 :
                                 (exec_size == 16) ? 4 :
                                 (exec_size ==  4) ? 2 : 1;
                footprint = footprints[index];
            }
            else
            {
                // General cases.
                unsigned short bit_seq = G4_Type_Table[type].footprint;
                for (uint8_t i = 0; i < exec_size; ++i)
                {
                    int eltOffset = i * s_size;
                    footprint |= ((uint64_t)bit_seq) << eltOffset;
                }
            }

            unsigned totalBytes = (exec_size - 1) * s_size + type_size;
            right_bound = left_bound + totalBytes - 1;

            bitVec[0] = (uint32_t)footprint;
            bitVec[1] = (uint32_t)(footprint >> 32);
        }
        else
        {
            // indirect
            bitVec[0] |= 0x3;
            right_bound = left_bound + G4_Type_Table[ADDR_REG_TYPE].byteSize - 1;
        }
    }
    rightBoundSet = true;
    return right_bound;
}

/// compare regRegion to opnd
/// regRegion is either a SrcRegRegion or DstRegRegion, opnd can be any G4_operand
/// We put this in a separate function since G4_DstRegRegion and G4_SrcRegRegion 
/// should have (nearly) identical code for compareOperand
static G4_CmpRelation compareRegRegionToOperand(G4_Operand* regRegion, G4_Operand* opnd)
{
    assert(regRegion->isSrcRegRegion() || regRegion->isDstRegRegion() && "expect either src or dst regRegion");
    bool legal_opnd = opnd->isSrcRegRegion() || opnd->isDstRegRegion() || opnd->isPredicate() || opnd->isCondMod();
    G4_VarBase* myBase = regRegion->getBase();
    G4_VarBase *opndBase = opnd->getBase();
    G4_RegAccess myAcc = regRegion->getRegAccess();
    G4_RegAccess opndAcc = opnd->getRegAccess();
    G4_Declare* myDcl = regRegion->getTopDcl();
    G4_Declare* opndDcl = opnd->getTopDcl();

    if (!legal_opnd || myBase == nullptr || opndBase == nullptr)
    {
        // a null base operand can never interfere with anything
        return Rel_disjoint;
    }

    if (myDcl == opndDcl && opndDcl != nullptr)
    {
        // special checks for pseudo kills
        G4_INST* myInst = regRegion->getInst();
        G4_INST* opndInst = opnd->getInst();
        if (myInst && (myInst->isPseudoKill() || myInst->isLifeTimeEnd()))
        {
            return Rel_interfere;
        }

        if (opndInst && (opndInst->isPseudoKill() || opndInst->isLifeTimeEnd()))
        {
            return Rel_interfere;
        }
    }

    if (opndAcc == myAcc && myAcc != Direct)
    {
        // two indirect are assumed to interfere in the absence of pointer analysis
        return Rel_interfere;
    }
    else if (opndAcc != myAcc)
    {
        // direct v. indirect
        // the two may inteferce if the direct operand is either an address-taken GRF or an address operand
        // we could make the check tighter by considering the offsets of the address operand, 
        // but it won't much difference in practice
        auto mayInterfereWithIndirect = [](G4_Operand* direct, G4_Operand* indirect)
        { 
            assert((direct->getRegAccess() == Direct && indirect->getRegAccess() == IndirGRF) && 
                "first opereand should be direct and second indirect");
            return (direct->getTopDcl() && direct->getTopDcl()->getAddressed()) ||
                (direct->isAddress() && direct->getTopDcl() == indirect->getTopDcl());
        };

        if ((opndAcc != Direct && mayInterfereWithIndirect(regRegion, opnd)) ||
            (myAcc != Direct && mayInterfereWithIndirect(opnd, regRegion)))
        {
            return Rel_interfere;
        }
        return Rel_disjoint;
    }

    if (myDcl != opndDcl)
    {
        return Rel_disjoint;
    }
    if (myBase->getKind() != opndBase->getKind())
    {
        return Rel_disjoint;
    }
    if (myBase->isPhyAreg())
    {
        if (myBase->asAreg()->getArchRegType() == AREG_NULL)
        {
            //like NaN, a null ARF is disjoint to everyone including itself
            return Rel_disjoint;
        }
        return (myBase->asAreg()->getArchRegType() ==
              opndBase->asAreg()->getArchRegType()) ? Rel_eq : Rel_disjoint;
    }

    unsigned int left_bound2 = opnd->getLeftBound(), right_bound2 = opnd->getRightBound();
    uint32_t myLeftBound = regRegion->getLeftBound();
    uint32_t myRightBound = regRegion->getRightBound();

    {
        unsigned opndBitVecL = opnd->getBitVecL(), opndBitVecH = opnd->getBitVecH();
        uint32_t myBitVecL = regRegion->getBitVecL(), myBitVecH = regRegion->getBitVecH();
        if (myRightBound < left_bound2 || right_bound2 < myLeftBound)
        {
            return Rel_disjoint;
        }
        else if (myLeftBound == left_bound2 &&
            myRightBound == right_bound2 &&
            myBitVecL == opndBitVecL && myBitVecH == opndBitVecH)
        {
            return Rel_eq;
        }
        else
        {
            // First consider if any operand is from a send.
            uint32_t myBitVecS = regRegion->getBitVecS();
            uint32_t opndBitVecS = opnd->getBitVecS();
            if (opndBitVecS > 0 || myBitVecS > 0)
            {
                if (myBitVecS > 0 && opndBitVecS == 0)
                {
                    // The first convers >= 3 grfs, and the second is within 2 grfs.
                    if (left_bound2 >= myLeftBound && right_bound2 <= myRightBound)
                    {
                         return Rel_gt;
                    }
                    return Rel_interfere;
                }
                else if (myBitVecS == 0 && opndBitVecS > 0)
                {
                    // The first is within 2 grfs, and the second convers >= 3 grfs.
                    if (myLeftBound >= left_bound2 && myRightBound <= right_bound2)
                    {
                        return Rel_lt;
                    }
                    return Rel_interfere;
                }
                else
                {
                    // myBitVecS > 0 and opndBitVecS > 0
                    if (left_bound2 >= myLeftBound && right_bound2 <= myRightBound)
                    {
                         return Rel_gt;
                    }
                    if (myLeftBound >= left_bound2 && myRightBound <= right_bound2)
                    {
                        return Rel_lt;
                    }
                    return Rel_interfere;
                }
            }

            // Now both operands are within two GRFs, comparing their L/H vectors.
            int dist = left_bound2 - myLeftBound;
            unsigned new_bitVecL = myBitVecL, new_bitVecH = myBitVecH;
            if (dist > 0 && dist < (2 * GENX_GRF_REG_SIZ))
            {
                if (dist >= GENX_GRF_REG_SIZ)
                {
                    unsigned lbit = new_bitVecH >> (dist - GENX_GRF_REG_SIZ);
                    new_bitVecL = lbit;
                    new_bitVecH = 0;
                }
                else
                {
                    new_bitVecL >>= dist;
                    unsigned lbit = new_bitVecH << (GENX_GRF_REG_SIZ - dist);
                    new_bitVecL |= lbit;
                    new_bitVecH >>= dist;
                }
            }
            else if (dist < 0 && dist > (-2 * GENX_GRF_REG_SIZ))
            {
                dist = abs(dist);
                if (dist >= GENX_GRF_REG_SIZ)
                {
                    unsigned lbit = opndBitVecH >> (dist - GENX_GRF_REG_SIZ);
                    opndBitVecL = lbit;
                    opndBitVecH = 0;
                }
                else
                {
                    opndBitVecL >>= dist;
                    unsigned lbit = opndBitVecH << (GENX_GRF_REG_SIZ - dist);
                    opndBitVecL |= lbit;
                    opndBitVecH >>= dist;
                }
            }
            unsigned commonL = new_bitVecL & opndBitVecL, commonH = new_bitVecH & opndBitVecH;

            if (myLeftBound <= left_bound2 &&
                myRightBound >= right_bound2 &&
                commonL == opndBitVecL &&
                commonH == opndBitVecH)
            {
                return Rel_gt;
            }
            else if (myLeftBound >= left_bound2 &&
                myRightBound <= right_bound2 &&
                commonL == new_bitVecL &&
                commonH == new_bitVecH)
            {
                return Rel_lt;
            }
            else if (dist < (2*GENX_GRF_REG_SIZ) && dist > (-2 * GENX_GRF_REG_SIZ) && commonL == 0 && commonH == 0)
            {
                return Rel_disjoint;
            }
            else
            {
                return Rel_interfere;
            }
        }
    }
}

G4_CmpRelation G4_DstRegRegion::compareOperand( G4_Operand *opnd)
{
    return compareRegRegionToOperand(this, opnd);
}

bool G4_DstRegRegion::isNativeType()
{
    G4_Type type = getType();

    if (IS_WTYPE(type) || IS_DTYPE(type) || IS_FTYPE(type) || type == Type_DF) {
        return true;
    }
    else {
        return false;
    }
}

bool G4_DstRegRegion::isNativePackedRowRegion()
{
    if (isNativeType()) {
        return horzStride  == 1;
    }
    else {
        return false;
    }
}

bool G4_DstRegRegion::isNativePackedRegion()
{
    return isNativePackedRowRegion();
}

bool G4_DstRegRegion::coverGRF( uint16_t numGRF, uint8_t execSize )
{
    uint32_t size = G4_GRF_REG_NBYTES * numGRF;
    uint32_t range = getRightBound() - getLeftBound() + 1;
    if( acc == Direct )
    {
        if( range == size )
        {
            return true;
        }
        if( horzStride > 1 )
        {
            if( size == execSize * horzStride * G4_Type_Table[type].byteSize )
            {
                return true;
            }
        }
    }
    else
    {
        if( size == execSize * horzStride * G4_Type_Table[type].byteSize )
        {
            return true;
        }
    }
    return false;
}

bool G4_DstRegRegion::goodAlign16Dst( )
{
    bool isAlign16 = false;
    unsigned byteSubRegOff =
        subRegOff * G4_Type_Table[type].byteSize;

    if( byteSubRegOff  % 16 != 0 )
    {
        return false;
    }

    if( base )
    {
        if( base->isRegVar() )
        {
            G4_Declare *dcl = base->asRegVar()->getDeclare();

            if( dcl )
            {
                G4_Declare *aliasDcl = dcl;

                unsigned aliasOffset = 0;
                while( aliasDcl->getAliasDeclare() )
                {
                    aliasOffset += aliasDcl->getAliasOffset();
                    aliasDcl = aliasDcl->getAliasDeclare();
                }
                if( aliasOffset % 16 != 0 )
                {
                    return false;
                }

                if( aliasDcl->getSubRegAlign() >= Eight_Word ||
                    aliasDcl->getNumRows() * aliasDcl->getElemSize() * aliasDcl->getElemSize() >= 16 )
                {
                    return true;
                }
            }
            else if( base->asRegVar()->isGreg() &&
                     base->asRegVar()->isPhyRegAssigned() &&
                     base->asRegVar()->getByteAddr() % 16 == 0 )
            {
                return true;
            }
        }
    }

    return isAlign16;
}

// Check if dst satisfies the following conditions( for platforms before BDW ):
//The destination region is entirely contained in the lower OWord of a register.
//The destination region is entirely contained in the upper OWord of a register.
//The destination elements are evenly split between the two OWords of a register.

bool G4_DstRegRegion::goodOneGRFDst( uint8_t execSize )
{
    if( acc != Direct )
    {
        if( horzStride * G4_Type_Table[type].byteSize * execSize == GENX_GRF_REG_SIZ )
        {
            return true;
        }
        else
        {
            return false;
        }
    }
    uint32_t halfSize = (getRightBound() - getLeftBound() + 1 + ( horzStride - 1 ) * G4_Type_Table[type].byteSize) / 2;
    uint32_t middle = getLeftBound() + halfSize;
    if( getLeftBound()/(GENX_GRF_REG_SIZ/2) == getRightBound()/(GENX_GRF_REG_SIZ/2) ||
        ( getLeftBound()/(GENX_GRF_REG_SIZ/2) == (getRightBound()/(GENX_GRF_REG_SIZ/2) - 1) &&
        getLeftBound()%(GENX_GRF_REG_SIZ/2) == middle%(GENX_GRF_REG_SIZ/2) ) )
    {
        return true;
    }
    return false;
}

bool G4_DstRegRegion::goodtwoGRFDst( uint8_t execSize )
{
    return evenlySplitCrossGRF(execSize);
}

// this is true if dst crosses GRF and has same number of elements in both GRFs
// (i.e, the middle element has same GRF offset as the start element)
bool G4_DstRegRegion::evenlySplitCrossGRF( uint8_t execSize )
{
    // check number of elements in first GRF.
    MUST_BE_TRUE( acc == Direct, "Indirect operand can not cross GRF boundary." );

    if (execSize == 1)
    {
        return false;
    }

    int halfBytes = left_bound + horzStride * G4_Type_Table[type].byteSize * (execSize >> 1);
    int halfOffset = halfBytes % GENX_GRF_REG_SIZ;
    int startOffset = left_bound % GENX_GRF_REG_SIZ;
    return halfOffset == startOffset;
}

/*
 * check if the input opnd is align to GRF
 * if the first level dcl is not aligned to GRF or sub register offset of this opnd is not multiple GRFs, including 0,
 * return true.
 */
bool G4_DstRegRegion::checkGRFAlign()
{

    bool GRF_aligned = false;
    unsigned byte_subregoff =
        subRegOff * G4_Type_Table[type].byteSize;

    if( byte_subregoff  % G4_GRF_REG_NBYTES != 0 )
    {
        return false;
    }

    if( base )
    {
        if( base->isRegVar() )
        {
            G4_Declare *dcl = base->asRegVar()->getDeclare();

            if( dcl )
            {
                G4_Declare *aliasdcl = dcl;

                unsigned aliasOffset = 0;
                while( aliasdcl->getAliasDeclare() )
                {
                    aliasOffset += aliasdcl->getAliasOffset();
                    aliasdcl = aliasdcl->getAliasDeclare();
                }
                if( aliasOffset % G4_GRF_REG_NBYTES != 0 )
                {
                    return false;
                }

                if( aliasdcl->getSubRegAlign() >= Sixteen_Word ||
                    aliasdcl->getNumRows() * aliasdcl->getElemSize() * aliasdcl->getElemSize() >= G4_GRF_REG_NBYTES ){
                        return true;
                }
            }
            else if( base->asRegVar()->isPhyRegAssigned() &&
                base->asRegVar()->getByteAddr() % G4_GRF_REG_NBYTES == 0 )
            {
                    return true;
            }
        }
    }

    return GRF_aligned;
}

//
// returns true if this operand (must be either Src or DstRegRegion) has a fixed subreg offset.
// This is true only if
// -- operand is direct, 
// -- operand has assigned GRF (i.e., input), or
// -- base declare is a GRF variable that is GRF-aligned
// if true, the subreg offset is also returned via offset in bytes
// Note this always returns false for ARFs (flag, addr, etc.)
//
static bool regionHasFixedSubreg(G4_Operand* opnd, uint32_t& offset)
{
    assert(opnd->isSrcRegRegion() || opnd->isDstRegRegion());
    short subRegOff = 0;
    if (opnd->isSrcRegRegion())
    {
        if (opnd->asSrcRegRegion()->getRegAccess() != Direct)
        {
            return false;
        }
        subRegOff = opnd->asSrcRegRegion()->getSubRegOff();
    }
    else if (opnd->isDstRegRegion())
    {
        if (opnd->asDstRegRegion()->getRegAccess() != Direct)
        {
            return false;
        }
        subRegOff = opnd->asDstRegRegion()->getSubRegOff();
    }

    G4_VarBase* base = opnd->getBase();

    if (base == NULL || !base->isRegVar() || !base->asRegVar()->getDeclare()->useGRF())
    {
        return false;
    }

    if (base->asRegVar()->isPhyRegAssigned())
    {
        offset = base->asRegVar()->getPhyRegOff() * G4_Type_Table[opnd->getType()].byteSize;
        return true;
    }

    uint32_t subregByte = 0;
    G4_Declare *rootDcl = base->asRegVar()->getDeclare()->getRootDeclare(subregByte);
    subregByte += subRegOff * G4_Type_Table[opnd->getType()].byteSize;

    if (rootDcl->getSubRegAlign() < Sixteen_Word)
    {
        return false;
    }
    offset = subregByte % GENX_GRF_REG_SIZ;

    return true;
}


bool G4_DstRegRegion::hasFixedSubregOffset(uint32_t& offset)
{
    return regionHasFixedSubreg(this, offset);
}

// compute max execution size starting from the current pos.
// power of two. no cross GRF boundary is allowed now.
// TODO: cross GRF is allowed in BDW+.
// cross half-GRF should guaranttee evenly split
uint8_t G4_DstRegRegion::getMaxExecSize( int pos, uint8_t maxExSize, bool twoGRFsrc )
{
    if( acc != Direct )
    {
        return roundDownPow2(maxExSize);
    }

    uint8_t elSize = (uint8_t) G4_Type_Table[type].byteSize;
    uint8_t exTypeSize = horzStride * elSize;
    uint8_t maxSize = roundDownPow2( maxExSize );
    uint32_t newLB = getLeftBound() + pos * exTypeSize,
        newRB = newLB + ( maxExSize - 1 ) * exTypeSize + elSize - 1;
    uint32_t leftGRF = newLB / G4_GRF_REG_NBYTES, rightGRF = newRB / G4_GRF_REG_NBYTES;
    // pre-BDW does not allow cross GRF dst except full 2-GRF dst.
    // BDW+ allows if elements are evenly split between two GRFs
    bool crossGRF = false;
    if( isCrossGRFDst() )
    {
        // check cross GRF boundary
        uint8_t byteInFirstGRF = ( ( leftGRF + 1 ) * G4_GRF_REG_NBYTES - newLB );
        uint8_t eleInFirstGRF = byteInFirstGRF / exTypeSize +
            // v20(0,17)<2>:ub and simd size is 16
            ( ( byteInFirstGRF % exTypeSize != 0 ) && ( byteInFirstGRF % exTypeSize >= elSize ) ? 1 : 0 );

        if( leftGRF != rightGRF )
        {
            uint8_t pow2 = roundDownPow2( eleInFirstGRF );
            if (pow2 != eleInFirstGRF)
            {
                maxSize = pow2;
                newRB = newLB + ( maxSize - 1 ) * exTypeSize + elSize - 1;
            }
            else
            {
                // number of elements in first GRF is power of 2 and HS is not used to cross GRF
                // search into second GRF
                // if number of elements in second GRF >= numbr of elements in first GRF
                uint8_t byteInSecondGRF = ( newRB + 1 ) % G4_GRF_REG_NBYTES;
                uint8_t eleInSecondGRF = byteInSecondGRF / exTypeSize + ( horzStride > 1 ? 1 : 0 );
                if( eleInSecondGRF >= eleInFirstGRF )
                {
                    crossGRF = true;
                    maxSize = eleInFirstGRF * 2;
                }
            }
        }
    }
    // check if cross half-GRF boundary
    // FIXME: if we know that the new srcs are all in one GRF, we do not have to do the following check.
    if( !crossGRF && twoGRFsrc )
    {
        uint32_t halfGRFSize = G4_GRF_REG_NBYTES / 2;
        if( newLB / halfGRFSize != newRB / halfGRFSize )
        {
            uint32_t middlePoint = ( newRB + ( horzStride - 1 ) * elSize - newLB + 1 ) / 2;
            // check middle point
            if( ( middlePoint + newLB ) % halfGRFSize != 0 )
            {
                // check size before half-GRF
                uint8_t sizeBeforeMidGRF = ( ( leftGRF * G4_GRF_REG_NBYTES + halfGRFSize ) - newLB + exTypeSize - 1 ) / exTypeSize;
                uint8_t pow2Size = roundDownPow2( sizeBeforeMidGRF );
                // V36(0,1)<4>:ud is slipt into 2x2
                if( sizeBeforeMidGRF <= (maxSize >> 1) && pow2Size == sizeBeforeMidGRF )
                {
                    maxSize = 2 * pow2Size;
                }
                else
                {
                    maxSize = pow2Size;
                }
            }
        }
    }

    return maxSize;
}
//
// output (Var+refOff).subRegOff<1><WriteMask>
//
// symbolreg == false,  output <modifier>(Var+refOff).subRegOff<16;16,1>.xxyy
// symbolreg == true,   output <modifier><symbol>(RegOff, SubRegOff)<16;16,1> in symbolic register emit
// Here we use symbolreg instead of Options::symbolReg, because sometimes we need to emit an instruction with invalid symbolic register as comment and
// then emit a workable instruction with physical register. In this case, we do not want to re-set the global option variable Options::symbolReg to switch
// between these two states, that may have potential side effects.
//
void G4_DstRegRegion::emit(std::ostream& output, bool symbolreg)
{
    //
    // output Var(refOff,subRegOff)
    //
    emitRegVarOff(output, symbolreg);

    //
    // output <horzStride>
    //
    if (inst != NULL && inst->isSplitSend())
    {
        // do nothing for sends
    }
    else if (writeMask != NoChannelEnable)
    {
        // do nothing for align16 instructions
    }
    else if (isAccRegValid())
    {
        // do nothing for madm
    }
    else if (horzStride != UNDEFINED_SHORT)
    {
        output << '<' << horzStride << '>';
    }
    else if (base->isAreg())
    {
        output << "<1>";
    }
    else if (base->isNullReg())
    {
        // do not emit region for null reg
    }
    else if (base->isFlag())
    {
        output << "<1>";
    }
    else
    {
        MUST_BE_TRUE(false, "No default region specified");
    }

    if (isAccRegValid())
    {
        // output acc2~acc9
        if (getAccRegSel() == NOACC)
        {
            output << ".noacc";
        }
        else
        {
            output <<".acc"<< (getAccRegSel()+2);
        }
    }
    else if (writeMask != NoChannelEnable)
    {
        output << "." << getChannelEnableStr(writeMask);
    }

    if (Type_UNDEF != type)
    {
        if (!symbolreg || acc != Direct)                // can output register data type for indirect addressing in any time
            output << ':' << G4_Type_Table[type].str;
    }
}

//
// This function is used to check if the src operand obey the rule of symbolic register. We need this function to check the operand before we emit an instruction
//
bool G4_DstRegRegion::obeySymbolRegRule()
{
    if (!base->isRegVar())          // only for reg var
        return false;
    if (base->asRegVar()->getDeclare()->isSpilled())
    {
        return false;
    }
    //
    // For dst operand, we do not have Rule-2
    // Rule-2: must have register region or default register region
    //
    // Rule-3: No swizzle .xyzw
    //
    if (writeMask != NoChannelEnable)
    {
        return false;
    }
    //
    // Rule-4: do not support date type redefinition in direct addressing
    //
    if (Type_UNDEF != type)
    {
         if (base->isRegVar() && acc == Direct && base->asRegVar()->getDeclare()->getElemType() != type)        // check if the data type is the same as in declare
         {
             return false;
         }
    }

    return true;
}

//
// Here we use symbolreg instead of Options::symbolReg, because sometimes we need to emit an instruction with invalid symbolic register as comment and
// then emit a workable instruction with physical register. In this case, we do not want to re-set the global option variable Options::symbolReg to switch
// between these two states, that may have potential side effects.
//
void G4_DstRegRegion::emitRegVarOff(std::ostream& output, bool symbolreg)
{
    bool printSubReg = true;
    if (inst != NULL && inst->isSplitSend())
    {
        printSubReg = false;
    }
    printRegVarOff(output, this, regOff,subRegOff,immAddrOff,type, symbolreg, printSubReg);
}

//
// return true if prd and this are the same inst predicate
//
bool G4_Predicate::samePredicate(G4_Predicate& prd)
{
    return getBase() == prd.getBase() &&
           state == prd.state &&
           subRegOff == prd.subRegOff &&
           control == prd.control;
}
//
// return true if mod and this are the same condition modifier
//
bool G4_CondMod::sameCondMod(G4_CondMod& m)
{
    return getBase() == m.getBase() &&
           mod == m.mod &&
           subRegOff == m.subRegOff;
}

//
// create all physical register operands
//
PhyRegPool::PhyRegPool(Mem_Manager& m, unsigned int maxRegisterNumber) 
{
    maxGRFNum = maxRegisterNumber;

    GRF_Table = (G4_Greg**)m.alloc(sizeof(G4_Greg*) * maxGRFNum);
    // create General Registers
    for (unsigned int i = 0; i < maxGRFNum; i++)
        GRF_Table[i] = new (m) G4_Greg(i);

    for( unsigned i = 0; i < AREG_LAST; i++ ) 
    {
        ARF_Table[i] = nullptr;
    }

    // create Architecture Registers
    ARF_Table[AREG_NULL]     = new (m) G4_Areg(AREG_NULL);
    ARF_Table[AREG_A0]       = new (m) G4_Areg(AREG_A0);
    ARF_Table[AREG_ACC0]     = new (m) G4_Areg(AREG_ACC0);
    ARF_Table[AREG_ACC1]     = new (m) G4_Areg(AREG_ACC1);
    ARF_Table[AREG_MASK0]    = new (m) G4_Areg(AREG_MASK0);
    ARF_Table[AREG_MS0]      = new (m) G4_Areg(AREG_MS0);
    ARF_Table[AREG_DBG]     = new (m) G4_Areg(AREG_DBG);
    ARF_Table[AREG_SR0]      = new (m) G4_Areg(AREG_SR0);
    ARF_Table[AREG_CR0]      = new (m) G4_Areg(AREG_CR0);
    ARF_Table[AREG_TM0]      = new (m) G4_Areg(AREG_TM0);
    ARF_Table[AREG_N0]       = new (m) G4_Areg(AREG_N0);
    ARF_Table[AREG_N1]       = new (m) G4_Areg(AREG_N1);
    ARF_Table[AREG_IP]       = new (m) G4_Areg(AREG_IP);
    ARF_Table[AREG_F0]       = new (m) G4_Areg(AREG_F0);
    ARF_Table[AREG_F1]       = new (m) G4_Areg(AREG_F1);
    ARF_Table[AREG_TDR0]     = new (m) G4_Areg(AREG_TDR0);
    ARF_Table[AREG_SP]       = new (m)G4_Areg(AREG_SP);
}

bool GlobalRA::areAllDefsNoMask(G4_Declare* dcl)
{
    bool retval = true;
    auto maskUsed = getMask(dcl);
    if (maskUsed != NULL)
    {
        if (maskUsed == (unsigned char*)allDefsNoMask)
        {
            retval = true;
        }
        else if (maskUsed == (unsigned char*)allDefsNotNoMask)
        {
            retval = false;
        }
        else
        {
            auto byteSize = dcl->getByteSize();
            for (unsigned int i = 0; i < byteSize; i++)
            {
                if (maskUsed[i] != NOMASK_BYTE)
                {
                    retval = false;
                    break;
                }
            }
        }
    }
    else
    {
        retval = false;
    }
    return retval;
}

void G4_Declare::setAlign(G4_Align al)
{
    regVar->setAlignment(al);
}

G4_Align G4_Declare::getAlign() const
{
    return regVar->getAlignment();
}

G4_Align GlobalRA::getBankAlign(G4_Declare* dcl)
{
    IR_Builder* builder = kernel.fg.builder;
    switch (getBankConflict(dcl))
    {
    case BANK_CONFLICT_FIRST_HALF_EVEN:
    case BANK_CONFLICT_SECOND_HALF_EVEN:
        if (builder->oneGRFBankDivision())
        {
            return Even;
        }
        else
        {
            return Even2GRF;
        }
        break;
    case BANK_CONFLICT_FIRST_HALF_ODD:
    case BANK_CONFLICT_SECOND_HALF_ODD:
        if (builder->oneGRFBankDivision())
        {
            return Odd;
        }
        else
        {
            return Odd2GRF;
        }
        break;
    default: break;
    }

    return Either;
}


void G4_Declare::setSubRegAlign(G4_SubReg_Align subAl)
{
    regVar->setSubRegAlignment(subAl);
}

G4_SubReg_Align G4_Declare::getSubRegAlign() const
{
    return regVar->getSubRegAlignment();
}

void
G4_Declare::emit(std::ostream &output, bool isDumpDot,  bool isSymbolReg)
{

    //
    // .declare <symbol>    Base=RegFile RegBase {.SubRegBase}
    //                              ElementSize=ElementSize
    //                              {SrcRegion=DefaultSrcRegion}
    //                              {DstRegion=DefaultDstRegion}
    //                              {Type=DefaultType}
    //
    if (isSymbolReg)
    {
        //
        // check if the register allocation is success
        //
        if (isSpilled())        // FIXME: check the "spill code" mark instead here!
        {
            //
            // spill code: do not emit declare
            //
            output << "//.declare " << name;
            // Base field
            output << "\tBase=";
            if (useGRF())
            {
                output << "r (spilled)";                        // currently will not happen
            }
            else if (regFile == G4_ADDRESS)
            {
                output << "a (spilled)";
            }
            else if (regFile == G4_FLAG)
            {
                output << "f (spilled)";
            }
            else
            {
                MUST_BE_TRUE(false, ERROR_UNKNOWN); //unhandled case
            }
            return;
        }
        else if (regVar->isFlag() )
        {
            return;
        }
        else
        {
            if (pCisaBuilder->m_options.getOption(vISA_UniqueLabels))
            {
                const char * labelStr = nullptr;
                pCisaBuilder->m_options.getOption(vISA_LabelStr, labelStr);
                if (labelStr != nullptr)
                {
                output << ".declare " << labelStr << "_" << name;
            }
            }
            else
            {
                output << ".declare " << name;
            }

            // Base field
            if (useGRF())
            {
                if( regVar->isGreg() )
                {
                    output << "\tBase=r";
                    G4_Greg * GrfReg = (G4_Greg *) regVar->getPhyReg();
                    int reg = GrfReg->getRegNum();
                    int off = regVar->getPhyRegOff();
                    output<<reg<<"."<<off;
                }
            }
            else if (regFile == G4_ADDRESS)
            {
                output << "\tBase=a";
                if (regVar->isA0())
                {
                    int off = regVar->getPhyRegOff();
                    output<<"0."<<off;
                }
            }
            else
            {
                MUST_BE_TRUE(false, ERROR_UNKNOWN); //unhandled case
            }
        }

        int elemSize = G4_Type_Table[elemType].byteSize;
        output << " ElementSize="<<elemSize;
    }
    //
    // .declare Var_Name reg_file = r|a
    //          width = (# of elements)
    //          height = (# of rows)
    //          elem_type=UD|W|...
    //          srcRegion=<vertStride;width,horzStride>  --- default src region
    //          dstRegion=<horzStride>      --- default dst region
    //
    else
    {
        output << "//.declare " << name;
        output << " rf=";
        if (useGRF())
        {
            output << 'r';
        }
        else if (regFile == G4_ADDRESS)
        {
            output << 'a';
        }
        else if (regFile == G4_FLAG)
        {
            output << 'f';
        }
        else
        {
            MUST_BE_TRUE(false, ERROR_UNKNOWN); //unhandled case
        }

        output << " size=" << getByteSize();
        if (Type_UNDEF != elemType)
        {
            output << " type=" << G4_Type_Table[elemType].str;
        }
        if (AliasDCL)
        {
            output << " alias=" << AliasDCL->getName() << "+" << getAliasOffset();
        }
		output << " align=" << getSubRegAlign() << " words";
        if (regVar->isPhyRegAssigned())
        {
            G4_VarBase* phyreg = regVar->getPhyReg();
            if (phyreg->isGreg())
            {
                output << " (r" << phyreg->asGreg()->getRegNum() << "." << regVar->getPhyRegOff() << ")";
            }
            else if (phyreg->isAddress())
            {
                output << " (a0." << regVar->getPhyRegOff() << ")";
            }
            else if (phyreg->isFlag())
            {
                bool valid = false;
                output << " (f" << phyreg->asAreg()->ExRegNum(valid) << "." << regVar->getPhyRegOff() << ")";
            }
        }
        else if (isSpilled())
        {
            if (spillDCL != nullptr)
            {
                output << " (spilled -> " << spillDCL->getName() << ")";
            }
            else
            {
                output << " (spilled)";
            }
        }
    }

    //
    // emit data type in symbolic register case
    //
    if (isSymbolReg)
    {
        // Type=
        if (Type_UNDEF != elemType)
        {
            output << " Type=" << G4_Type_Table[elemType].str;
        }
    }

    if (liveIn && liveOut)
    {
        output << " Input_Output";
    }
    else if (liveIn)
    {
        output << " Input";
    }
    else if (liveOut)
    {
        output << " Output";
    }

    output << std::endl;
}

void
G4_Predicate::emit(std::ostream& output, bool symbolreg)
{
    static const char* align16ControlNames[] =
    {
        "",
        "xyzw",
        "x",
        "y",
        "z",
        "w"
        "any4h",
        "all4h"
    };

    output << "(";
    if (state == PredState_Minus)
    {
        output << '-';
    }
    else // state == PredState_Plus || state == PredState_undef
    {
        output << '+';
    }

    if (getBase()->asRegVar()->isPhyRegAssigned())
    {
        getBase()->asRegVar()->getPhyReg()->emit(output);
        output << "." << getBase()->asRegVar()->getPhyRegOff();
    }
    else
    {
        getBase()->emit(output);
        if (subRegOff != UNDEFINED_SHORT)
        {
            output << '.' << subRegOff;
        }
    }

    if (align16Control != PRED_ALIGN16_DEFAULT)
    {
        output << "." << align16ControlNames[align16Control];
    }
    else
    {
        if( control != PRED_DEFAULT )
        {
            output << '.';
            switch( control )
            {
            case PRED_ANY2H:
                output << "any2h";
                break;
            case PRED_ANY4H:
                output << "any4h";
                break;
            case PRED_ANY8H:
                output << "any8h";
                break;
            case PRED_ANY16H:
                output << "any16h";
                break;
            case PRED_ANY32H:
                output << "any32h";
                break;
            case PRED_ALL2H:
                output << "all2h";
                break;
            case PRED_ALL4H:
                output << "all4h";
                break;
            case PRED_ALL8H:
                output << "all8h";
                break;
            case PRED_ALL16H:
                output << "all16h";
                break;
            case PRED_ALL32H:
                output << "all32h";
                break;
            case PRED_ANYV:
                output << "anyv";
                break;
            case PRED_ALLV:
                output << "allv";
                break;
            default:
                // do nothing
                break;
            }
        }
    }

    output << ") ";
}

G4_Predicate::G4_Predicate(G4_Predicate &prd)
    : G4_Operand(G4_Operand::predicate, prd.getBase())
{
    state = prd.state;
    subRegOff = prd.subRegOff;
    control = prd.control;
    align16Control = prd.align16Control;

    top_dcl = prd.top_dcl;
    left_bound = prd.left_bound;
    right_bound = prd.right_bound;
    bitVec[0] = prd.bitVec[0];
    bitVec[1] = prd.bitVec[1];
    byteOffset = prd.byteOffset;
    rightBoundSet = prd.rightBoundSet;
}

unsigned G4_Predicate::computeRightBound( uint8_t exec_size )
{
    rightBoundSet = true;
    bitVec[0] = 0;
    bitVec[1] = 0;

	uint16_t group_size = (uint16_t)getPredCtrlGroupSize();
	uint16_t totalBits = (exec_size > group_size) ? exec_size : group_size;

	if (inst)
		left_bound = inst->getMaskOffset();

    right_bound = left_bound + totalBits - 1;

    bitVec[0] = exec_size == 32 ? 0xFFFFFFFF : (1 << exec_size) - 1;

    return right_bound;
}

static G4_CmpRelation compareBound(uint32_t myLB, uint32_t myRB, uint32_t otherLB, uint32_t otherRB)
{
    if (myLB == otherLB && myRB == otherRB)
    {
        return Rel_eq;
    }
    else if (myRB < otherLB || otherRB < myLB)
    {
        return Rel_disjoint;
    }
    else if (myLB <= otherLB && myRB >= otherRB)
    {
        return Rel_gt;
    }
    else if (myLB >= otherLB && myRB <= otherRB)
    {
        return Rel_lt;
    }
    else
    {
        return Rel_interfere;
    }
}

/// compare flag to opnd
/// flag is either a G4_Predicate or G4_CondMod, opnd can be any G4_operand
/// We put this in a separate function since G4_Predicate and G4_CondMod 
/// should have identical code for compareOperand
static G4_CmpRelation compareFlagToOperand(G4_Operand* flag, G4_Operand* opnd)
{
    assert((flag->isPredicate() || flag->isCondMod()) && "expect either predicate or conditional modifier");

    bool legalOpnd = opnd->isSrcRegRegion() || opnd->isDstRegRegion() || opnd->isPredicate() || opnd->isCondMod();
    G4_VarBase* myBase = flag->getBase();
    G4_VarBase *opndBase = opnd->getBase();

    if (!legalOpnd || myBase == nullptr || opndBase == nullptr || !opndBase->isFlag())
    {
        return Rel_disjoint;
    }

    // flags with different base declare definitely do not interfere (we do not consider physical flags here)
    if (flag->getTopDcl() != opnd->getTopDcl())
    {
        return Rel_disjoint;
    }

    // Do we generate pseudo kill on flags?
    G4_INST* opndInst = opnd->getInst();
    if (opndInst && (opndInst->isPseudoKill() || opndInst->isLifeTimeEnd()))
    {
        return Rel_interfere;
    }

    return compareBound(flag->getLeftBound(), flag->getRightBound(), opnd->getLeftBound(), opnd->getRightBound());
}

G4_CmpRelation G4_Predicate::compareOperand(G4_Operand *opnd)
{
    return compareFlagToOperand(this, opnd);
}

// remove half of the bitvector and change right bound
void G4_Predicate::splitPred( )
{
    uint16_t range = getRightBound() - getLeftBound() + 1;
    uint16_t shiftLen = range >> 2;
    right_bound = getLeftBound() + shiftLen - 1;

    bitVec[0] = getBitVecL() >> shiftLen;
}
void
G4_CondMod::emit(std::ostream& output, bool symbolreg)
{
    output << '.' << CondModStr[mod];
    output << '.';

    if (getBase() == nullptr)
    {
        output << "f0.0";
    }
    else if (getBase()->asRegVar()->isPhyRegAssigned())
    {
        getBase()->asRegVar()->getPhyReg()->emit(output);
        output << "." << getBase()->asRegVar()->getPhyRegOff();
    } else {
        getBase()->emit(output);
        if (subRegOff != UNDEFINED_SHORT)
        {
            output << '.' << subRegOff;
        }
    }
}
G4_CondMod::G4_CondMod(G4_CondMod &cMod)
    : G4_Operand(G4_Operand::condMod, cMod.getBase())
{
    mod = cMod.mod;
    subRegOff = cMod.subRegOff;

    top_dcl = cMod.top_dcl;
    left_bound = cMod.left_bound;
    right_bound = cMod.right_bound;
    bitVec[0] = cMod.bitVec[0];
    bitVec[1] = cMod.bitVec[1];
    byteOffset = cMod.byteOffset;
    rightBoundSet = cMod.rightBoundSet;
}

unsigned G4_CondMod::computeRightBound( uint8_t exec_size )
{
    bitVec[0] = 0;
    bitVec[1] = 0;
    rightBoundSet = true;

    if(inst)
        left_bound = inst->getMaskOffset();

    right_bound = left_bound + exec_size - 1;

    bitVec[0] = exec_size == 32 ? 0xFFFFFFFF : (1 << exec_size) - 1;

    return right_bound;
}

/// same as G4_Predicate::compareOperand
G4_CmpRelation G4_CondMod::compareOperand( G4_Operand *opnd)
{
    return compareFlagToOperand(this, opnd);
}


// remove half of the bitvector and change right bound
void G4_CondMod::splitCondMod( )
{
    uint16_t range = getRightBound() - getLeftBound() + 1;
    uint16_t shiftLen = range >> 2;
    right_bound = getLeftBound() + shiftLen - 1;

    bitVec[0] = getBitVecL() >> shiftLen;
}
bool G4_Imm::isEqualTo(G4_Imm& imm1) const
{
    return (imm1.getType() == type) && (imm1.getImm() == imm.num);
}

// check if an immedate is in the range of type
bool G4_Imm::isInTypeRange(int64_t imm, G4_Type ty)
{
    switch (ty)
    {
    case Type_D:
        return imm >= (int)MIN_DWORD_VALUE && imm <= (int)MAX_DWORD_VALUE;
    case Type_Q:
        return true;
    case Type_UQ:
        return imm >= 0;
    case Type_UD:
        return (imm >= (unsigned)MIN_UDWORD_VALUE && imm <= (unsigned)MAX_UDWORD_VALUE);
    case Type_W:
        return (imm >= (int)MIN_WORD_VALUE && imm <= (int)MAX_WORD_VALUE);
    case Type_UW:
        return (imm >= (int)MIN_UWORD_VALUE && imm <= (int)MAX_UWORD_VALUE);
    case Type_B:
        return (imm >= (int)MIN_CHAR_VALUE && imm <= (int)MAX_CHAR_VALUE);
    case Type_UB:
        return (imm >= (int)MIN_UCHAR_VALUE && imm <= (int)MAX_UCHAR_VALUE);
    default:
        break;
    }

    return false;
}

bool G4_Imm::isZero() const
{
    if (IS_TYPE_F32_F64(type))
    {
        if (type == Type_F)
        {
            return (imm.fp32 == 0.0f);
        }
        return (imm.fp == 0.0);
    }
    return (imm.num == 0);
}

G4_CmpRelation G4_Imm::compareOperand(G4_Operand *opnd)
{
    G4_CmpRelation rel = Rel_disjoint;
    if (opnd->isImm() && this->isEqualTo(opnd->asImm()))
    {
        return Rel_eq;
    }
    return rel;
}

void
G4_Imm::emit(std::ostream& output, bool symbolreg )
{

    //
    // we only emit hex in this function
    //
    std::ios::fmtflags outFlags(output.flags());
    output.flags(std::ios_base::hex | std::ios_base::showbase);

    short word;
    if (type == Type_DF)
    {
        output << (uint64_t)imm.num;
    }
    else if (type == Type_F)
    {
        output << imm.num32;
    }
    else if (type == Type_W || type == Type_UW || type == Type_B || type == Type_UB)
    {
        word = (short)imm.num;
        output << word;
    }
    else if (type == Type_D || type == Type_UD)
    {
        // 32-bit int
        output << (int) imm.num;
    }
    else
    {
        // 64-bit int
        output << imm.num;
    }

    output.flags(outFlags);

    if (Type_UNDEF != type)
    {
        output << ':' << G4_Type_Table[type].str;
    }
}

void
// emit number, automatically select the format according to its original format
G4_Imm::emitAutoFmt(std::ostream& output)
{
	if (Type_F == type)
	{
		output << imm.fp32;
	}
	else if (Type_DF == type)
    {
       output << imm.fp;
    }
	else if (Type_W == type || Type_B == type)
    {
		output << (short)imm.num;
    }
	else if (Type_D == type)
    {
		output << imm.num;
    }
	else //unsigned value
    {
		output << (unsigned int)imm.num;
    }

    if (Type_UNDEF != type)
    {
        output << ':' << G4_Type_Table[type].str;
    }
}

G4_RegVar *
G4_RegVarTransient::getNonTransientBaseRegVar ()
{
    G4_RegVar * base;
    for (base = getBaseRegVar (); base->isRegVarTransient (); base = base->getBaseRegVar ());
    return base;
}

G4_RegVar *
G4_RegVarTransient::getAbsBaseRegVar ()
{
    G4_RegVar * base;
    for (base = getBaseRegVar (); base->getBaseRegVar () != base; base = base->getBaseRegVar ());
    return base;
}

G4_RegVar *
G4_RegVarTmp::getAbsBaseRegVar ()
{
    G4_RegVar * base;
    for (base = getBaseRegVar (); base->getBaseRegVar () != base; base = base->getBaseRegVar ());
    return base;
}

void
G4_RegVar::emit(std::ostream& output, bool symbolreg)
{

    output << decl->getName();
    if (reg.phyReg != NULL)
    {
        output << "(";
        reg.phyReg->emit(output);
        output << '.' << reg.subRegOff << ':' <<
            G4_Type_Table[getDeclare()->getElemType()].str << ")";
    }
}
int G4_AddrExp::eval()
{
    int byteAddr = 0;

    if( m_addressedReg->getPhyReg() == NULL )
    {
        // address taken range is spilled
        G4_Declare* addrTakenSpillFillDcl = m_addressedReg->getDeclare()->getAddrTakenSpillFill();
        MUST_BE_TRUE(addrTakenSpillFillDcl != NULL, "No addr taken spill fill register found!");
        byteAddr = addrTakenSpillFillDcl->getRegVar()->getPhyReg()->asGreg()->getRegNum() * 32;
    }
    else
    {
        byteAddr = m_addressedReg->getByteAddr(); //let's assume the unsigned=>int won't overflow for now.
    }

    //byteAddr += offsetInEle * G4_Type_Table[addressedReg->getDeclare()->getElemType()].byteSize;
    byteAddr += m_offset;

    return byteAddr;
}
void
G4_AddrExp::emit(std::ostream& output, bool symbolreg)
{
    output << '&';
    m_addressedReg->emit(output);
    output << '+' << m_offset;
}

void G4_SrcRegRegion::computeLeftBound()
{
    top_dcl = NULL;
    unsigned newregoff = regOff, offset = 0;

    if( base )
    {
        if( base->isRegVar() )
        {
            top_dcl = base->asRegVar()->getDeclare();
            if( !top_dcl && base->asRegVar()->isGreg() )
            {
                newregoff = base->asRegVar()->asGreg()->getRegNum();
            }
        }
    }

    if( top_dcl )
    {
        while( top_dcl->getAliasDeclare() )
        {
            offset += top_dcl->getAliasOffset();
            top_dcl = top_dcl->getAliasDeclare();
        }
    }

    if( base != NULL && base->isFlag() )
    {
        if( base->isRegVar() )
        {
            if(base->asRegVar()->getPhyReg())
            {
                left_bound = base->asRegVar()->getPhyRegOff() * 16;   // the bound of flag register is in unit of BIT
				left_bound += subRegOff * 16;
                if( ((G4_Areg*)(base->asRegVar()->getPhyReg()))->getArchRegType() == AREG_F1 )
                {
                    left_bound += 32;
                }
            }
            else
            {
                left_bound = subRegOff * 16;
            }
        }
        else
        {
            left_bound = subRegOff * 16;
            if( ((G4_Areg*)(base))->getArchRegType() == AREG_F1 )
            {
                left_bound += 32;
            }
        }

        right_bound = 0;
    }
    else if (base != NULL && base->isAccReg())
    {
        left_bound = subRegOff * G4_Type_Table[type].byteSize;
        if( ((G4_Areg*)(base))->getArchRegType() == AREG_ACC1 )
        {
            left_bound += 32;  // TODO: size of ACC is assumed to be 32 BYTEs.
        }
        byteOffset = left_bound;
    }
    else if ( top_dcl )
    {
        if( acc == Direct )
        {
            left_bound = offset + newregoff * G4_GRF_REG_NBYTES + subRegOff * G4_Type_Table[type].byteSize;
            if( top_dcl->getTotalElems() * top_dcl->getElemSize() >= GENX_GRF_REG_SIZ )
            {
                byteOffset = left_bound;
            }
            else
            {
                unsigned alignOff = G4_Type_Table[type].byteSize > G4_Type_Table[Type_W].byteSize ?
                    G4_Type_Table[type].byteSize : G4_Type_Table[Type_W].byteSize;
                if( top_dcl->getSubRegAlign() == Even_Word || top_dcl->getSubRegAlign() >= Four_Word )
                {
                    alignOff = top_dcl->getSubRegAlign() * 2;
                }
                byteOffset = left_bound + alignOff;
            }
        }
        else
        {
            left_bound = subRegOff * G4_Type_Table[ADDR_REG_TYPE].byteSize;
            byteOffset = G4_Type_Table[type].byteSize;
        }

        if( desc && desc->isScalar() )
        {
            right_bound = left_bound + G4_Type_Table[type].byteSize - 1;
        }
        else
        {
            right_bound = 0;
            // for other cases, we need execution size and instruction compression attr, so we just set
            // partial value here, which will be patched later
            // right_bound = desc->horzStride * G4_Type_Table[type].byteSize;
            // patch it with *exec_size + left_bound
            // if vertical stride == 0 and width < exec_size, divide it by 2
        }
    }
    else
    {  //arch reg
        left_bound = 0;
        byteOffset = left_bound;
    }
}

unsigned G4_SrcRegRegion::computeRightBound( uint8_t exec_size )
{
    bitVec[0] = 0;
    bitVec[1] = 0;
    unsigned short hs = desc->isScalar() ? 1 : desc->horzStride;
    unsigned short vs = desc->isScalar() ? 0 : desc->vertStride;
    rightBoundSet = true;
    unsigned short typeSize = (unsigned short) G4_Type_Table[type].byteSize;

    unsigned short startPoint = 0;
    if (base->isFlag())
    {
        unsigned int totalBits = 0;
        if( G4_Inst_Table[inst->opcode()].instType != InstTypePseudoLogic )
        {
			// mov (1) ... fx.1<0;1,0>:uw
            left_bound = subRegOff * 16;
            totalBits = base->asRegVar()->getDeclare()->getNumberFlagElements() < G4_Type_Table[type].bitSize ?
                        base->asRegVar()->getDeclare()->getNumberFlagElements() : G4_Type_Table[type].bitSize;
        }
        else
        {
            /*
                we need to set leftBound for pseudo intruction
                so that it creates use/def links correctly in the control flow graph between
                cmp instruction and pseudo instruction.
                This matters when we break up SIMD32 instruction in to two SIMD16 with H1/H2 masks.
                The bound for compare for H2 will be [15,31], and this has to match.
                Without this no use/def link was created which caused issues in logic optimization.
                Also it produce incorrect behavior in any operation that relies on compareOperand.
            */
            left_bound = inst->getMaskOffset();
            totalBits = exec_size;
            /*
                Compare operand uses not only LB/RB, but also bitVec to determine equivalency.
                If bit vetors don't match it might conclude that is not entirely killed in the BB
                and marks it as global.
                Which then can prevent logic optimizations from kicking in.
            */
            startPoint = (unsigned short)left_bound;
        }

        right_bound = left_bound + totalBits - 1;

        bitVec[0] = totalBits == 32 ? 0xFFFFFFFF : (1 << totalBits) - 1;
    }
	else
	{
        if (acc == Direct)
		{
            unsigned short bit_seq = G4_Type_Table[type].footprint;

            uint64_t footPrint = 0;

            MUST_BE_TRUE(exec_size >= desc->width, "exec size must be >= width");
            if (desc->isScalar())
            {
                footPrint = bit_seq;
            }
            else if (desc->isContiguous(exec_size))
            {
                int totalBits = exec_size * typeSize;
                MUST_BE_TRUE(totalBits <= 64, "total bits exceeds 2 GRFs");
                if (totalBits == 64)
                {
                    footPrint = ULLONG_MAX;
                }
                else
                {
                    footPrint = (1ULL << totalBits) - 1;
                }
            }
            else
            {
                for (int i = 0, numRows = exec_size / desc->width; i < numRows; ++i)
                {
                    for (int j = 0; j < desc->width; ++j)
                    {
                        int eltOffset = i * desc->vertStride * typeSize + j * desc->horzStride * typeSize;
                        footPrint |= ((uint64_t) bit_seq) << eltOffset;
                    }
                }
            }

            bitVec[0] = (uint32_t) footPrint;
            bitVec[1] = (uint32_t) (footPrint >> 32);

			if (desc->isScalar())
			{
				right_bound = left_bound + typeSize - 1;
			}
			else
			{
				int num_rows = exec_size / desc->width;
				if (num_rows > 0)
				{
					right_bound =
						left_bound +
						(num_rows - 1) * vs * typeSize +
						hs * (desc->width - 1) * typeSize +
						typeSize - 1;
				}
				else
				{
					// this fix applies to inplicit acc src
					// usually when we compute new rb after inst splitting,
					// the region is still the old one.
					// exec_size may be smaller than width
					right_bound =
						left_bound +
						hs * (exec_size - 1) * typeSize +
						typeSize - 1;
				}
			}
        }
        else
        {
            unsigned short numAddrSubReg = 1;
            if( desc->isRegionWH() )
            {
                numAddrSubReg = exec_size/desc->width;
            }
            for( uint16_t i = 0; i < numAddrSubReg; i++ )
            {
                bitVec[0] |= 0x3 << ( i * 2 );
            }
            right_bound = left_bound + G4_Type_Table[ADDR_REG_TYPE].byteSize * numAddrSubReg - 1;
        }
    }
    return right_bound;
}

G4_CmpRelation G4_SrcRegRegion::compareOperand( G4_Operand *opnd)
{
    return compareRegRegionToOperand(this, opnd);
}

bool G4_SrcRegRegion::isNativeType()
{
    G4_Type type = getType();

    if (IS_WTYPE(type) || IS_DTYPE(type) || IS_FTYPE(type) || type == Type_DF) {
        return true;
    }
    else {
        return false;
    }
}

bool G4_SrcRegRegion::isNativePackedRowRegion()
{
    if (isNativeType())
    {
        // A single element row is always packed.
        return (desc->horzStride == 1) ||
               (desc->width == 1 && desc->horzStride == 0);
    }

    return false;
}

bool G4_SrcRegRegion::isNativePackedRegion()
{
    return isNativePackedRowRegion() && desc->vertStride == desc->width;
}

bool G4_SrcRegRegion::coverTwoGRF()
{
    uint16_t range = getRightBound() - getLeftBound() + 1;
    if( range < GENX_GRF_REG_SIZ )
        return false;
    if( desc->horzStride > 1 )
    {
        range += ( desc->horzStride - 1 ) * G4_Type_Table[type].byteSize;
    }
    if( range == GENX_GRF_REG_SIZ * 2 &&
        (desc->vertStride == desc->horzStride * desc->width ||
         desc->isContiguous(getInst()->getExecSize())))
    {
        return true;
    }
    return false;
}
// Assumption:
// operand crosses GRF boundary
bool G4_SrcRegRegion::evenlySplitCrossGRF( uint8_t execSize, bool &sameSubRegOff,
    bool &vertCrossGRF, bool &contRegion, uint8_t &eleInFirstGRF )
{
    // always return true since all align16 instructions are generated by JIT
    // later on when we have other execution types for align16 instructions,
    // fix the following if to check src element distribution.
    // FIXME: do we need to check HS here?
    if( desc->isRegionV() )
    {
        sameSubRegOff = true;
        vertCrossGRF = true;
        contRegion = true;
        return true;
    }
    vertCrossGRF = true;
    contRegion = desc->isSingleStride(getInst()->getExecSize());
    MUST_BE_TRUE( acc == Direct, "Indirect operand can not cross GRF boundary." );
    uint8_t firstSubRegOff = getLeftBound() % GENX_GRF_REG_SIZ;
    uint8_t left = firstSubRegOff;
    uint8_t typeSize = (uint8_t) G4_Type_Table[type].byteSize;
    uint8_t execTySize = ( desc->horzStride == 0 ? 1 : desc->horzStride ) * typeSize;
    uint8_t lastEltEndByte = desc->horzStride * (desc->width - 1) * typeSize + typeSize;
    uint8_t realRowSize = lastEltEndByte;
    // check number of elements in first GRF.
    eleInFirstGRF = 0;
    while( left < GENX_GRF_REG_SIZ )
    {
        if( left + realRowSize  <= GENX_GRF_REG_SIZ )
        {
            // realRowSize is used to handle V12(0,17)<32;8,2>:b
            eleInFirstGRF += desc->width;
            left += desc->vertStride * G4_Type_Table[type].byteSize;
        }
        else
        {
            vertCrossGRF = false;
            // V12(0,17)<32;8,2>:b is a good two GRF source
            eleInFirstGRF++;
            uint8_t newLeft = left + typeSize;
            newLeft += execTySize;
            while( newLeft < GENX_GRF_REG_SIZ )
            {
                eleInFirstGRF++;
                newLeft += execTySize;
            }
            if( newLeft == GENX_GRF_REG_SIZ )
            {
                eleInFirstGRF++;
                if( eleInFirstGRF % desc->width == 0 )
                {
                    left += desc->vertStride * G4_Type_Table[type].byteSize;
                }
                else
                {
                    left = newLeft + ( execTySize - typeSize );
                }
            }
            else if( eleInFirstGRF % desc->width == 0 )
            {
                left += desc->vertStride * G4_Type_Table[type].byteSize;
            }
            else if( typeSize == execTySize )
            {
                left = newLeft;
            }
            else
            {
                left = newLeft - typeSize;
            }
        }
    }
    uint8_t secondSubRegOff = left % GENX_GRF_REG_SIZ;

    sameSubRegOff = ( firstSubRegOff == secondSubRegOff );
    // TODO: this guaranttees that there are equal number fo elements in each GRF, but not the distribution of elements in each of them.
    if( eleInFirstGRF * 2 == execSize )
    {
        return true;
    }
    return false;
}

bool G4_SrcRegRegion::evenlySplitCrossGRF( uint8_t execSize )
{
    // check number of elements in first GRF.
    MUST_BE_TRUE( acc == Direct, "Indirect operand can not cross GRF boundary." );
    uint16_t sizeInFirstGRF = GENX_GRF_REG_SIZ - getLeftBound() % GENX_GRF_REG_SIZ;
    uint16_t vertSize = desc->vertStride * getElemSize();
    uint16_t execTypeSize = desc->horzStride == 0 ? getElemSize() : desc->horzStride * getElemSize();
    uint16_t numEle = ( sizeInFirstGRF + execTypeSize - 1 )/ execTypeSize;
    uint16_t rowSize = desc->horzStride == 0 ? execTypeSize : desc->width * execTypeSize,
        numRows = desc->vertStride == 0 ? 1 : execSize/desc->width,
        numElePerRow = rowSize / execTypeSize,
        numExecEmePerRow = desc->horzStride == 0 ? 1 : desc->width;

    if( sizeInFirstGRF <= vertSize )
    {
        if( numEle >= desc->width )
        {
            numEle = desc->width;
        }
    }
    else if( desc->vertStride > desc->width )
    {
        numEle = sizeInFirstGRF/vertSize * numExecEmePerRow +
            (( sizeInFirstGRF%vertSize > rowSize ) ? numExecEmePerRow : ( sizeInFirstGRF%vertSize + execTypeSize - 1 ) / execTypeSize );
    }

    uint16_t totalNumEle = ( desc->vertStride >= numElePerRow ) ? ( numRows * numExecEmePerRow ) :
        ( getRightBound() - getLeftBound() + 1 ) / execTypeSize;

    // TODO: this guarantees that there are equal number of elements in each GRF, but not the distribution of elements in each of them.
    if( numEle * 2 == totalNumEle )
    {
        return true;
    }
    return false;
}

/*
 * check if the input opnd is align to GRF
 * if the first level dcl is not aligned to GRF or sub register offset of this opnd is not multiple GRFs, including 0,
 * return true.
 */
bool G4_SrcRegRegion::checkGRFAlign(){

    bool GRF_aligned = false;
    uint32_t byte_subregoff = subRegOff * G4_Type_Table[type].byteSize;

    if( byte_subregoff  % G4_GRF_REG_NBYTES != 0 ){
        return false;
    }

    if( base ){
        if( base->isRegVar() ){
            G4_Declare *dcl = base->asRegVar()->getDeclare();

            if( dcl ){
                G4_Declare *aliasdcl = dcl;

                unsigned aliasOffset = 0;
                while( aliasdcl->getAliasDeclare() )
                {
                    aliasOffset += aliasdcl->getAliasOffset();
                    aliasdcl = aliasdcl->getAliasDeclare();
                }
                if( aliasOffset % G4_GRF_REG_NBYTES != 0 )
                {
                    return false;
                }

                if( aliasdcl->getSubRegAlign() >= Sixteen_Word ||
                    aliasdcl->getNumRows() * aliasdcl->getElemSize() * aliasdcl->getElemSize() >= G4_GRF_REG_NBYTES ){
                        return true;
                }
            }else if( base->asRegVar()->isPhyRegAssigned() &&
                base->asRegVar()->getByteAddr() % G4_GRF_REG_NBYTES == 0 ){
                    return true;
            }
        }
    }

    return GRF_aligned;
}

//
// returns true if this SrcRegRegion has a fixed subreg offset (in bytes).
// This is true only if
// -- src is direct
// -- base declare is a GRF variable that is GRF-aligned
// if true, the subreg offset is also returned via offset
// Note this always returns false for ARFs (flag, addr, etc.)
//
bool G4_SrcRegRegion::hasFixedSubregOffset(uint32_t& offset)
{
    return regionHasFixedSubreg(this, offset);
}

/*
 * Return true if the src operand has a native type and has a packed (stride
 * of 1) region.
 */
bool G4_SrcRegRegion::isNativePackedSrcRegion()
{
    return isNativePackedRowRegion() &&
            ( desc->vertStride == desc->width );
}

void RegionDesc::emit(std::ostream& output)
{
    if (isRegionV())
    {
        output << '<' << horzStride << '>';
    }
    else if (isRegionWH())
    {
        output << '<' << width << ',' << horzStride << '>';
    }
    else
    {
        output << '<' << vertStride << ';' << width << ',' << horzStride << '>';
    }
}

void G4_Label::emit(std::ostream& output, bool symbolreg)
{
    if (pCisaBuilder->m_options.getOption(vISA_UniqueLabels))
    {
        const char* labelStr = nullptr;
        pCisaBuilder->m_options.getOption(vISA_LabelStr, labelStr);
        if (labelStr != nullptr)
        {
        output << (pCisaBuilder != NULL ? pCisaBuilder->getCurrentKernel()->getName() : "") << "_" <<
            labelStr << "_" << label;
    }
    }
    else
    {
        output << label;
    }
}

unsigned G4_RegVar::getByteAddr()
{
    MUST_BE_TRUE(reg.phyReg != NULL, ERROR_UNKNOWN);
    if (reg.phyReg->isGreg())
    {
        return reg.phyReg->asGreg()->getRegNum() * 32 +
            reg.subRegOff * (G4_Type_Table[decl->getElemType()].byteSize);
    }

    MUST_BE_TRUE(false, ERROR_UNKNOWN);
    return 0;
}

void G4_RegVar::setSubRegAlignment(G4_SubReg_Align subAlg)
{
    // sub reg alignment can only be more restricted than prior setting
    MUST_BE_TRUE(subAlign == Any || subAlign == subAlg || subAlign%2 == 0,
                 ERROR_UNKNOWN);
    if (subAlign > subAlg)
    {
        MUST_BE_TRUE(subAlign % subAlg == 0, "Sub reg alignment conflict");
        // do nothing; keep the original alignment (more restricted)
    }
    else
    {
        MUST_BE_TRUE(subAlg % subAlign == 0, "Sub reg alignment conflict");
        subAlign = subAlg;
    }
}

// For implicit Acc operands, left bound depends on
//    a) Inst execution type
//    b) Qtr control
//
// This function handles relevant cases, including hw intricacies
// and updates left bound only.
//
void G4_INST::computeLeftBoundForImplAcc(G4_Operand* opnd)
{
    if( opnd != NULL )
    {
        G4_Type extype;
        int extypesize;
        extype = getOpExecType( extypesize );

        if (( IS_WTYPE(extype) || IS_DTYPE(extype) ) )
        {
            // This condition is a result of HW Conformity requirement
            // that for exec type = D/DW, only acc0 is used even when
            // qtr control is set to Q2/H2
            opnd->setLeftBound( 0 );
        }
        else
        {
            if( opnd->isSrcRegRegion() )
            {
                opnd->asSrcRegRegion()->computeLeftBound();
            }
            else if( opnd->isDstRegRegion() )
            {
                opnd->asDstRegRegion()->computeLeftBound();
            }
        }
    }
}

// Left and right bound for every operand is based off
// top most dcl.
// For flag register as dst/src/pred/cond mod, each bit of
// bitset represents corresponding bit of flag.
// For indirect access, right bound is set to sum of
// left bound and 15. The constant 15 is derived by the
// fact that address register is accessed as Type_UW which
// means 16 bits. right bound represents closed interval
// so 1 is subtracted.
// For direct access of GRF, each bit of bitset represents
// correcponding byte of operand.
void G4_INST::computeRightBound(G4_Operand* opnd)
{
    associateOpndWithInst(opnd, this);

	if( opnd != NULL &&
        opnd->isImm() == false &&
        opnd->isNullReg() == false )
    {
		bool done = false;

        if(isSend())
		{
            if( getMsgDesc() == NULL &&
                ( opnd == srcs[0] || opnd == dst ) )
            {
                // This call is from createSendInst where msg desc is
                // not available.
                opnd->unsetRightBound();
                done = true;
            }

            auto computeSendOperandBound = [](G4_Operand* opnd, int numReg)
            {
                if (numReg == 0)
                {
                    return;
                }

                // Sends read/write in units of GRF. With a narrower simd width,
                // the variable may have size smaller than one GRF, or smaller
                // the reponse or message length. In this case, limit the right
                // bound up to the variable size.
                unsigned LB = opnd->left_bound;
                unsigned RB = std::min(opnd->getTopDcl()->getByteSize(),
                                       LB + numReg * G4_GRF_REG_NBYTES) - 1;

                unsigned NBytes = RB - LB + 1;
                if (NBytes <= 32)
                {
                    uint32_t Mask = uint32_t(-1) >> (32 - NBytes);
                    opnd->setBitVecL(Mask);
                }
                else if (NBytes <= 64)
                {
                    opnd->setBitVecL(0xFFFFFFFF);
                    uint32_t Mask = uint32_t(-1) >> (64 - NBytes);
                    opnd->setBitVecH(Mask);
                }
                else
                {
                    // NBytes > 64
                    opnd->setBitVecL(0xFFFFFFFF);
                    opnd->setBitVecH(0xFFFFFFFF);

                    // GRF level tracking, up to 32 + 2 GRFs.
                    unsigned NGrfs = (NBytes - 64 + 31) / 32;
                    uint32_t Mask = uint32_t(-1) >> (32 - NGrfs);
                    opnd->setBitVecS(Mask);
                }
                opnd->setRightBound(RB);
            };

			if (done == false &&
                (srcs[0] == opnd ||
                 (isSplitSend() && srcs[1] == opnd )))
			{
			    // For send instruction's msg operand rightbound depends
				// on msg descriptor
				uint16_t numReg = (srcs[0] == opnd) ? getMsgDesc()->MessageLength() : getMsgDesc()->extMessageLength();
                computeSendOperandBound(opnd, numReg);
					done = true;
			    }
			else if( done == false && dst == opnd )
			{
			    // Compute right bound for dst operand
				uint16_t numReg = getMsgDesc()->ResponseLength();

                if (msgDesc != NULL &&
                	msgDesc->isScratchRW() == false &&
                    msgDesc->isOwordLoad() &&
                    (msgDesc->getFuncCtrl() & 0x700) == 0)
                {
                    //1 oword read
                    opnd->setBitVecL(0xFFFF);
                    opnd->setRightBound( opnd->left_bound + 15 );
                }
                else
                {
                    computeSendOperandBound(opnd, numReg);
                }

				done = true;
			}
		}
		else if( done == false && op == G4_pln && opnd == srcs[1] )
		{
			opnd->computeRightBound( execSize > 8 ? execSize : execSize * 2 );
            if( execSize > 8 )
            {
				opnd->setRightBound( opnd->right_bound * 2 - opnd->getLeftBound() + 1 );
            }

			done = true;
		}
		else if( done == false && (op == G4_pseudo_kill || isPseudoUse()) )
		{
			// pseudo kills/use write/read entire variable
			G4_Declare* topdcl = opnd->getBase()->asRegVar()->getDeclare()->getRootDeclare();
			opnd->setRightBound(topdcl->getByteSize() - 1);

			done = true;
		}
		else if( done == false && op == G4_pseudo_caller_save_flag )
		{
			opnd->setRightBound(opnd->getBase()->asRegVar()->getDeclare()->getNumberFlagElements() - 1);

			done = true;
        }

		if( done == false )
		{
			opnd->computeRightBound( execSize );

            if( getMaskOffset() > 0 &&
                ((opnd == getImplAccSrc()) ||
                (opnd == getImplAccDst()) ) )
            {
                // for ARF (flag, acc) we have to adjust its bound based on the emask
                // We have to reset LB since the original instruction may have a non default emask
                opnd->setLeftBound(0);
                opnd->computeRightBound(execSize);
                unsigned int multiplicationFactor = 1;
                bool exceptionBoundsComputation = false;
                if( opnd->isAccReg() )
                {
                    // Right bound granularity is in terms of
                    // bytes for Acc registers
                    multiplicationFactor = 4;
                }

                if( opnd == getImplAccDst() || opnd == getImplAccSrc() )
                {
                    G4_Type extype;
                    int extypesize;
                    extype = getOpExecType( extypesize );

                    if (( IS_WTYPE(extype) || IS_DTYPE(extype) ) )
                    {
                        // This condition is a result of HW Conformity requirement
                        // that for exec type = D/DW, only acc0 is used even when
                        // qtr control is set to Q2/H2
                        opnd->setLeftBound( 0 );
                        opnd->setRightBound( 31 );
                        exceptionBoundsComputation = true;
                    }
                }

                if( exceptionBoundsComputation == false )
                {
                    // Update left/right bound as per inst mask offset
                    opnd->setLeftBound( opnd->left_bound + (getMaskOffset() * multiplicationFactor) );
                    opnd->setRightBound( opnd->right_bound + (getMaskOffset () * multiplicationFactor) );
                }
            }

			done = true;
		}
	}
}

// This function should only be invoked after computePReg() function
// has been invoked. The function computePReg() is invoked by computePhyReg()
// just before scheduling and post-RA.
// For GRF type variables this function returns linearized byte offset into
// GRF file. So if a variable is assigned r1 and its left bound is 0, this
// function will return (1 * 32) + 0 = 32.
// For non-GRF variables, GRF base offset value is 0 so value returned will
// be left bound.
// This function works for both, G4_SrcRegRegion as well as G4_DstRegRegion.
unsigned int G4_Operand::getLinearizedStart()
{
    unsigned linearizedStart = getLeftBound();
    G4_VarBase* base = getBase();

    if (base && base->isRegVar())
    {
        // LB is computed based on the root variable, so we have to go all the way up
        G4_Declare* dcl = base->asRegVar()->getDeclare();
        linearizedStart += dcl->getGRFBaseOffset();
        linearizedStart -= dcl->getOffsetFromBase();
    }

    return linearizedStart;
}

// Just like getLinearizedStart(), this function returns linearized byte
// offset of end of variable. For eg, if a variable is assigned r1 and
// region is type dword with inst exec size = 16, linearized end will be
// (63 - 0 + 32) = 95.
// Here, right bound is 63 since the region accesses 64 bytes,
// left bound is 0 since region access begins at byte 0,
// linearizedStart() will return 32 since r1 is allocated to the region.
// This function works for both, G4_SrcRegRegion as well as G4_DstRegRegion.
unsigned int G4_Operand::getLinearizedEnd()
{
    return (getRightBound() - getLeftBound() + getLinearizedStart());
}

void G4_Operand::dump() const
{
#if _DEBUG
    const_cast<G4_Operand *>(this)->emit(std::cerr, false);
#endif
}

void G4_INST::setPredicate(G4_Predicate* p)
{
    if( predicate != NULL && predicate->getInst() == this )
    {
        predicate->setInst(NULL);
    }

	predicate = p;

    associateOpndWithInst((G4_Operand*)p, this);
    computeRightBound((G4_Operand*)p);
}

void G4_INST::setSrc(G4_Operand* opnd, unsigned i)
{
    MUST_BE_TRUE(i < G4_MAX_SRCS, ERROR_INTERNAL_ARGUMENT);

    if (srcs[i] != NULL)
    {
        if ((srcs[0] == srcs[i] && i != 0) ||
            (srcs[1] == srcs[i] && i != 1) ||
            (srcs[2] == srcs[i] && i != 2) ||
            (srcs[3] == srcs[i] && i != 3))
        {
            // opnd is present in some other
            // index of srcs so dont set its
            // inst to NULL
        }
        else
        {
            if (srcs[i]->getInst() == this)
            {
                srcs[i]->setInst(NULL);
            }
        }
    }

    srcs[i] = opnd;

    associateOpndWithInst(opnd, this);
    resetRightBound(opnd);
}

void G4_INST::setDest(G4_DstRegRegion* opnd)
{
    if (dst != NULL && dst->getInst() == this)
    {
        dst->setInst(NULL);
    }

    dst = opnd;

    associateOpndWithInst(opnd, this);
    resetRightBound(opnd);
}

void G4_INST::setCondMod(G4_CondMod* m)
{
    if (mod != NULL && mod->getInst() == this)
    {
        mod->setInst(NULL);
    }

    mod = m;

    associateOpndWithInst((G4_Operand*)m, this);
    computeRightBound((G4_Operand*)m);
}

void G4_INST::setImplAccSrc(G4_Operand* opnd)
{
    if (implAccSrc != NULL && implAccSrc->getInst() == this)
    {
        implAccSrc->setInst(NULL);
    }

    implAccSrc = opnd;

    associateOpndWithInst(opnd, this);
    computeRightBound(opnd);
}

void G4_INST::setImplAccDst(G4_DstRegRegion* opnd)
{
    if (implAccDst != NULL && implAccDst->getInst() == this)
    {
        implAccDst->setInst(NULL);
    }

    implAccDst = opnd;

    associateOpndWithInst(opnd, this);
    computeRightBound(opnd);
}

void G4_INST::dump() const
{
#if _DEBUG
    G4_INST& inst = const_cast<G4_INST&>(*this);
    if (!inst.isLabel())
        std::cerr << "\t";
    inst.emit(std::cerr, false, false);
    std::cerr << "\n";
#endif
}

bool G4_INST::canSupportSaturate() const
{
    switch (op)
    {
    case G4_mul:
    {
        for (int i = 0; i < getNumSrc(); ++i)
        {
            if (IS_DTYPE(getSrc(i)->getType()))
            {
                return false;
            }
        }
        return true;
    }
    case G4_addc:
    case G4_bfe:
    case G4_bfi1:
    case G4_bfi2:
    case G4_bfrev:
    case G4_cmp:
    case G4_cbit:
    case G4_fbh:
    case G4_fbl:
    case G4_frc:
    case G4_and:
    case G4_not:
    case G4_or:
    case G4_xor:
    case G4_smov:
        return false;
    default:
        return true;
    }

    return true;
}

bool G4_INST::canSupportCondMod(const IR_Builder& builder) const
{
    if (!builder.hasCondModForTernary() && getNumSrc() == 3)
    {
        return false;
    }

	if (op == G4_mul)
	{
		// can't support conditional modifiers if source is DW and dst is not QW
		bool dwordSrc = false;
		for (int i = 0; i < getNumSrc(); ++i)
		{
			if (IS_DTYPE(getSrc(i)->getType()))
			{
				dwordSrc = true;
				break;
			}
		}
		if (dwordSrc && !IS_QTYPE(getDst()->getType()))
		{
			return false;
		}
		return true;
	}
    else if (op == G4_pseudo_mad)
    {
        // no cond mod for D * W
        G4_Operand* src0 = getSrc(0);
        G4_Operand* src1 = getSrc(1);
        if (IS_DTYPE(src0->getType()) || IS_DTYPE(src1->getType()))
        {
            return false;
        }
        return true;
    }

	return ((op == G4_add) ||
		(op == G4_addc) ||
		(op == G4_asr) ||
		(op == G4_avg) ||
		(op == G4_dp2) ||
		(op == G4_dp3) ||
		(op == G4_dp4) ||
		(op == G4_dph) ||
		(op == G4_line) ||
		(op == G4_lrp) ||
		(op == G4_lzd) ||
		(op == G4_mac) ||
		(op == G4_mach) ||
		(op == G4_mad) ||
		(op == G4_mov) ||
		(op == G4_mul) ||
		(op == G4_pln) ||
        (op == G4_rndd) ||
        (op == G4_rnde) ||
        (op == G4_rndu) ||
        (op == G4_rndz) ||
		(op == G4_sad2) ||
		(op == G4_sada2) ||
		(op == G4_shl) ||
		(op == G4_shr) ||
		(op == G4_subb));
}

// convert (execsize, offset) into emask option
// if no such mask option exists, return InstOpt_NoOpt
G4_InstOption G4_INST::offsetToMask(int execSize, int offset, bool nibOk)
{
    switch (execSize)
    {
    case 32:
        return InstOpt_M0; 
    case 16:
        switch (offset)
        {
        case 0:
            return InstOpt_M0;
        case 16:
            return InstOpt_M16;
        default:
            return InstOpt_NoOpt;
        }
    case 8:
        switch (offset)
        {
        case 0:
            return InstOpt_M0;
        case 8:
            return InstOpt_M8;
        case 16:
            return InstOpt_M16;
        case 24:
            return InstOpt_M24;
        default:
            return InstOpt_NoOpt;
        }
    case 4:
        if (nibOk)
        {
            switch (offset)
            {
            case 0:
                return InstOpt_M0;
            case 4:
                return InstOpt_M4;
            case 8:
                return InstOpt_M8;
            case 12:
                return InstOpt_M12;
            case 16:
                return InstOpt_M16;
            case 20:
                return InstOpt_M20;
            case 24:
                return InstOpt_M24;
            case 28:
                return InstOpt_M28;
            default:
                return InstOpt_NoOpt;
            }
        }
        else
        {
            return InstOpt_NoOpt;
        }
    default:
        return InstOpt_NoOpt;
    }
}

void G4_DstRegRegion::setWriteMask(ChannelEnable wm)
{
    writeMask = wm;
}

void G4_SrcRegRegion::setSwizzle( const char* sw )
{
    MUST_BE_TRUE((int)strlen(sw) <  max_swizzle, ERROR_INTERNAL_ARGUMENT);
    strcpy_s(swizzle, max_swizzle, sw);
}

// convert contiguous regions to <N;N,1> form subject to the requirment
// that width is not used to cross GRF
// This is done because <1;1,0>/<2;2,1> require crossbar muxes and thus incur a performance penalty
// This should only be called after RA when we know the actual subreg offset
void G4_SrcRegRegion::rewriteContiguousRegion(IR_Builder& builder, uint16_t opNum)
{
    int execSize = inst->getExecSize();
    if (execSize == 1 || !desc->isContiguous(execSize))
    {
        return;
    }
    uint32_t eltSize = G4_Type_Table[getType()].byteSize;
    uint32_t subRegOffset = getLinearizedStart() % GENX_GRF_REG_SIZ;
    uint32_t endOffset = subRegOffset + inst->getExecSize() * eltSize;

    bool isAlign1Ternary = builder.hasAlign1Ternary() && inst->getNumSrc() == 3;

    if (inst->getNumSrc() < 3)
    {
        // do <16;16,1> for HF/W if possible
        if (subRegOff == 0 && execSize == 16 && eltSize == 2)
        {
            setRegion(builder.createRegionDesc(16, 16, 1), true);
            return;
        }
    }

    // Find a width that does not cross GRF from <8;8,1>, <4;4,1>, to <2;2,1>
    auto getWidth = [=](unsigned offset, unsigned eltSize) -> unsigned
    {
        unsigned Widths[] = { 8, 4, 2 };
        for (auto w : Widths)
        {
            if (w > inst->getExecSize())
                continue;

            if (w * eltSize > GENX_GRF_REG_SIZ)
            {
                // <8;8,1> is not allowed for 64-bit type
                continue;
            }

            if (endOffset <= GENX_GRF_REG_SIZ ||
                subRegOffset % (w * eltSize) == 0)
            {
                return w;
            }
        }

        // width >= 2 crosses GRF
        return 0;
    };

    unsigned short w = (unsigned short)getWidth(subRegOffset, eltSize);

    if (builder.doNotRewriteRegion() && isAlign1Ternary && (w == 2 || w == 0) && opNum != 2)
    {
        setRegion(builder.getRegionStride1(), true);
        return;
    }

    if (w)
    {
        setRegion(builder.createRegionDesc(w, w, 1), true);
    }
    else if (isAlign1Ternary)
    {
        // binary encoding asserts on <1;1,0> region for 3-src inst, so force change it to <2;2,1>
        setRegion(builder.createRegionDesc(2, 2, 1), true);
    }
}


void resetRightBound( G4_Operand* opnd )
{
    if( opnd != NULL )
    {
        opnd->unsetRightBound();
    }
}

void associateOpndWithInst(G4_Operand* opnd, G4_INST* inst)
{
    if( opnd != NULL )
    {
        opnd->setInst(inst);
    }
}

void LiveIntervalInfo:: getLiveIntervals(std::vector<std::pair<uint32_t, uint32_t>>& intervals)
{
    for (auto&& it : liveIntervals)
    {
        intervals.push_back(it);
    }
}

void LiveIntervalInfo::addLiveInterval(uint32_t start, uint32_t end)
{
    if (liveIntervals.size() == 0)
    {
        liveIntervals.push_back(std::make_pair(start, end));
    }
    else
    {
        bool done = false, firstcheck = false;
        std::pair<uint32_t, uint32_t>* prev = nullptr;
        // Check if a new live-interval can be pushed independently of others
        for (auto lr_it = liveIntervals.begin();
            lr_it != liveIntervals.end();
            lr_it++)
        {
            auto& lr = (*lr_it);

            if (firstcheck)
            {
                if (lr.first > end)
                {
                    done = true;
                    liveIntervals.insert(lr_it, std::make_pair(start, end));
                    break;
                }
                firstcheck = false;
                break;
            }

            if (lr.second < start)
            {
                firstcheck = true;
                prev = &lr;
            }

            if (lr.first <= start && lr.second >= end)
            {
                return;
            }
        }

        if (firstcheck && !done)
        {
            if (start - liveIntervals.back().second <= 1)
            {
                liveIntervals.back().second = end;
                done = true;
            }
        }

        if (!done)
        {
            for (unsigned int i = start; i <= end; i++)
            {
                liveAt(i);
            }
        }
    }
}

void LiveIntervalInfo::liveAt(uint32_t cisaOff)
{
    if (cisaOff == UNMAPPABLE_VISA_INDEX)
        return;

    // Now iterate over all intervals and check which one should
    // be extended. If none, start a new one.
    bool added = false;
    auto prev = liveIntervals.begin();

    for (auto it = liveIntervals.begin(), itEnd = liveIntervals.end();
        it != itEnd;
        prev = it++)
    {
        auto& item = (*it);

        if (added)
        {
            // Check whether prev and current one can be merged
            if (((*prev).second == item.first) ||
                ((*prev).second == item.first - 1))
            {
                prev->second = item.second;
                it = liveIntervals.erase(it);
                break;
            }
            else
            {
                break;
            }
        }

        if (item.first == cisaOff + 1)
        {
            item.first = cisaOff;
            added = true;
            break;
        }

        if (item.second == cisaOff - 1)
        {
            item.second = cisaOff;
            added = true;
            continue;
        }

        if (!added &&
            item.first <= cisaOff &&
            item.second >= cisaOff)
        {
            added = true;
            break;
        }

        if (item.first > cisaOff)
        {
            liveIntervals.insert(it, std::make_pair(cisaOff, cisaOff));
            added = true;
            break;
        }
    }

    if (!added)
    {
        liveIntervals.push_back(std::make_pair(cisaOff, cisaOff));
    }
}

bool G4_INST::supportsNullDst(const IR_Builder& builder) const
{
    if (isSend())
    {
        return true;
    }
    return getNumSrc() != 3 && !(op == G4_pln && !builder.doPlane());
}

bool G4_INST::isAlign1Ternary(IR_Builder& builder) const
{
    return builder.hasAlign1Ternary() && getNumSrc() == 3 && !isSend();
}

// Detect packed low-precision instruction. This is used by the scheduler.
// - all src and dst are GRF and of :hf type and "packed".
// (src is also packed when it is replicated scalar).
// Two cases are possible:
// 1)   add (16)    r1.0<1>:hf   r2.0<8;8,1>:hf   r3.0<8;8,1>:hf    { Align1, H1 }
// 2)   add (16)    r1.0<1>:hf   r2.0<8;8,1>:hf   r3.0<0;1,0>:hf    { Align1, H1 } 
bool G4_INST::isFastHFInstruction(void) const {
    if (getExecSize() < 16) {
        return false;
    }
    bool isHF = false;
    for (int op_i = 0, op_e = getNumSrc(); op_i < op_e; ++op_i) {
        G4_Operand *opnd = getSrc(op_i);
        if (! opnd) {
            continue;
        }
        if (!IS_HFTYPE(opnd->getType())) {
            return false;
        }
        if (opnd->isSrcRegRegion()) {
            G4_SrcRegRegion *srcRgn = opnd->asSrcRegRegion();
            if (! srcRgn->getRegion()->isContiguous(getExecSize())) {
                return false;
            }
        }
        isHF = true;
    }
    return isHF;
}


void G4_Declare::prepareForRealloc(G4_Kernel* kernel)
{
    // Reset allocated register if this dcl is not an input
    // or a pre-defined variable.
    auto& builder = kernel->fg.builder;

    setGRFBaseOffset(0);

    if (getRegFile() != G4_RegFileKind::G4_INPUT &&
        getRegVar()->isPhyRegAssigned() &&
        !getRegVar()->isAreg() &&
        this != builder->getBuiltinA0() &&
        this != builder->getBuiltinR0() &&
        this != builder->getBuiltinA0Dot2() &&
        this != builder->getBuiltinBindlessSampler() &&
        this != builder->getBuiltinHWTID() &&
        this != builder->getBuiltinT252() &&
        this != builder->getStackCallRet() &&
        this != builder->getStackCallArg() &&
        this != builder->getBEFP() &&
        this != builder->getBESP() &&
        this != kernel->fg.getScratchRegDcl() &&
        this != kernel->fg.getStackPtrDcl() &&
        this != kernel->fg.getFramePtrDcl()
        )
    {
        getRegVar()->resetPhyReg();
        getRegVar()->setDisp(UINT_MAX);
    }
}

bool G4_INST::mayExpandToAccMacro(const IR_Builder& builder) const
{
    bool isDMul = opcode() == G4_mul &&
        (IS_QTYPE(getDst()->getType()) || (IS_DTYPE(getSrc(0)->getType()) && IS_DTYPE(getSrc(1)->getType())));
    return opcode() == G4_mach || opcode() == G4_mulh || isDMul || (opcode() == G4_pln && !builder.doPlane());
}

// returns true if dst may be replaced by an explicit acc
// in addition to opcode-specific checks, we require
// -- dst must be GRF
// -- contiguous regions
// -- simd8 for D/UD, simd8/16 for F, simd16 for HF/W, other types not allowed
bool G4_INST::canDstBeAcc(const IR_Builder& builder) const
{
    if (mayExpandToAccMacro(builder))
    {
        // while this should not prevent dst from becoming acc (mul/plane macros use
        // acc as temp so should not affect final dst), later HW conformity is not equipped
        // to deal with such code so we disable the substitution
        return false;
    }

    if (dst == nullptr || dst->getTopDcl() == nullptr || dst->asDstRegRegion()->getHorzStride() != 1)
    {
        return false;
    }

    if (dst->getTopDcl()->getRegFile() != G4_GRF)
    {
        return false;
    }

    // src0 may not have Vx1 or VxH regioning
    if (getSrc(0) && getSrc(0)->isSrcRegRegion())
    {
        auto src0Region = getSrc(0)->asSrcRegRegion();
        if (src0Region->getRegAccess() == IndirGRF && src0Region->getRegion()->isRegionWH())
        {
            return false;
        }
    }

    switch (dst->getType())
    {
    case Type_W:
    case Type_UW:
    case Type_HF:
        if (getExecSize() != 16)
        {
            return false;
        }
        break;
    case Type_F:
        if (getExecSize() != 16 && getExecSize() != 8)
        {
            return false;
        }
        break;
    case Type_D:
    case Type_UD:
        if (getExecSize() != 8)
        {
            return false;
        }
        break;
    default:
        return false;
    }

    if (!builder.relaxedACCRestrictions())
    {
        if (dst->getType() == Type_HF && isMixedMode())
        {
            // acc can't be used as packed f16 for mix mode instruction as it doesn't support regioning
            return false;
        }
    }
    switch (opcode())
    {
    case G4_add:
    case G4_and:
    case G4_asr:
    case G4_avg:
    case G4_frc:
    case G4_lzd:
    case G4_mul:
    case G4_not:
    case G4_or:
    case G4_rndd:
    case G4_rnde:
    case G4_rndu:
    case G4_rndz:
    case G4_sel:
    case G4_shr:
    case G4_smov:
    case G4_xor:
        return true;
    case G4_cmp:
    case G4_cmpn:
        // disable for now since it's causing some SKL tests to fail
        return false;
    case G4_mov:
        return builder.relaxedACCRestrictions() || !getSrc(0)->isAccReg();
    case G4_pln:
        // we can't use acc if plane will be expanded
        return builder.doPlane();
    case G4_mad:
    case G4_csel:
        return builder.canMadHaveAcc();
    default:
        return false;
    }
}

// returns true if src0 may be replaced by an explicit acc
// in addition to opcode-specific checks, we require
// -- contiguous regions
// -- simd8 for D/UD, simd8/16 for F, simd16 for HF/W, other types not allowed
bool G4_INST::canSrcBeAcc(int srcId, const IR_Builder& builder) const
{
    assert((srcId == 0 || srcId == 1) && "must be either src0 or src1");
    if (getSrc(srcId) == nullptr || !getSrc(srcId)->isSrcRegRegion())
    {
        return false;
    }

    if (mayExpandToAccMacro(builder))
    {
        return false;
    }

    G4_SrcRegRegion* src = getSrc(srcId)->asSrcRegRegion();
    if (srcId == 1 && src->getModifier() != Mod_src_undef)
    {
        // src1 acc does not support modifiers
        return false;
    }
    if (!src->getRegion()->isContiguous(getExecSize()))
    {
        return false;
    }

    switch (src->getType())
    {
    case Type_W:
    case Type_UW:
    case Type_HF:
        if (getExecSize() != 16)
        {
            return false;
        }
        break;
    case Type_F:
        if (getExecSize() != 16 && getExecSize() != 8)
        {
            return false;
        }
        break;
    case Type_D:
    case Type_UD:
        if (getExecSize() != 8)
        {
            return false;
        }
        if (isSignSensitive(srcId == 0 ? Opnd_src0 : Opnd_src1))
        {
            return false;
        }
        break;
    default:
        return false;
    }

    // dst must be GRF-aligned
    if ((getDst()->getLinearizedStart() % GENX_GRF_REG_SIZ) != 0)
    {
        return false;
    }

    // check that src0 and dst have the same type/alignment
    // we allow dst to be narrower than src because it is assumed that HW conformity
    // will promote the dst later 
    auto dstEltSize = getDst()->getHorzStride() * G4_Type_Table[getDst()->getType()].byteSize;
    if (dstEltSize > G4_Type_Table[src->getType()].byteSize)
    {
        return false;
    }
    else if (getDst()->getType() == Type_HF && src->getType() == Type_F &&
        dstEltSize == 2)
    {
        // mix mode inst with packed HF dst
        // this is ok if the dst has .0 offset
        uint32_t subreg = getDst()->getLinearizedStart() % GENX_GRF_REG_SIZ;
        if (subreg != 0)
        {
            return false;
        }
    }

    if (IS_TYPE_FLOAT_ALL(src->getType()) ^ IS_TYPE_FLOAT_ALL(getDst()->getType()))
    {
        // no float <-> int conversion for acc source
        return false;
    }

    switch (opcode())
    {
    case G4_add:
    case G4_asr:
    case G4_avg:
    case G4_cmp:
    case G4_cmpn:
    case G4_frc:
    case G4_lzd:
    case G4_rndd:
    case G4_rnde:
    case G4_rndu:
    case G4_rndz:
    case G4_sel:
    case G4_shl:
    case G4_shr:
    case G4_smov:
        return true;
    case G4_mov:
        return builder.relaxedACCRestrictions() || !getDst()->isAccReg();
    case G4_mad:
        // no int acc if it's used as mul operand
        return builder.canMadHaveAcc() &&
            ((srcId == 1 && IS_FTYPE(src->getType())) || (srcId == 0 && src->getModifier() == Mod_src_undef));
    case G4_csel:
        return builder.canMadHaveAcc();
    case G4_mul:
        return IS_FTYPE(src->getType());
    case G4_and:
    case G4_not:
    case G4_or:
    case G4_xor:
        return src->getModifier() == Mod_src_undef;
    case G4_pln:
        return builder.doPlane() && src->getModifier() == Mod_src_undef;
    default:
        return false;
    }
}

