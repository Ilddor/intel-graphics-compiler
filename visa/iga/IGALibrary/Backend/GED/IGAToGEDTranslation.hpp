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
#ifndef _IGA_TO_GED_TRANSLATION_H_
#define _IGA_TO_GED_TRANSLATION_H_

#include "ged.h"
#include "../../IR/Types.hpp"

namespace iga
{

    enum ArchRegFile
    {                                          // (ARF Registers -- Overview):
        ARCH_REG_FILE_NULL = 0x00,  // 0000 null    Null register
        ARCH_REG_FILE_A = 0x01,  // 0001 a0.#    Address register
        ARCH_REG_FILE_ACC = 0x02,  // 0010 acc#    Accumulator register
        ARCH_REG_FILE_F = 0x03,  // 0011 f0.#    Flag register [DevSNB]
                                 // 0011 f#.#    Flag register [DevIVB+]
                                 ARCH_REG_FILE_MASK = 0x04,  // 0100 ce#     Channel Enable register [DevHSW+]
                                 ARCH_REG_FILE_STACK = 0x05,  // 0110 msg#        reserved
                                 ARCH_REG_FILE_STACK_DEPTH = 0x06,  // 0110 sp      Stack Pointer Register DevHSW+
                                 ARCH_REG_FILE_STATE_REG = 0x07,  // 0111 sr0.#   State register
                                 ARCH_REG_FILE_CNTL_REG = 0x08,  // 1000 cr0.#   Control register
                                 ARCH_REG_FILE_NCNT_REG = 0x09,  // 1001 n#      Notification count register
                                 ARCH_REG_FILE_IP = 0x0A,  // 1010 ip      Instruction pointer register
                                 ARCH_REG_FILE_TDR_REG = 0x0B,  // 1011 tdr     Thread dependency register
                                 ARCH_REG_FILE_PERF_REG = 0x0C,  // 1100 tm0     TimeStamp register [DevIVB+]
                                 ARCH_REG_FILE_FC_REG = 0x0D,  // 1101 fc#.#   Flow Control register[DevHSW+]
                                 ARCH_REG_FILE_DBG_REG = 0x0F   // 1111 dbg0    Debug only [DevIVB+]
    };

    class _IGAToGEDTranslation
    {
    public:
        static GED_OPCODE lowerOpcode(Op opcode, Platform pltfm)
        {
            GED_OPCODE op = GED_OPCODE_INVALID;
            switch (opcode)
            {
            case Op::ILLEGAL:
                op = GED_OPCODE_illegal;
                break;
            case Op::MOV:
                op = GED_OPCODE_mov;
                break;
            case Op::SEL:
                op = GED_OPCODE_sel;
                break;
            case Op::MOVI:
                op = GED_OPCODE_movi;
                break;
            case Op::NOT:
                op = GED_OPCODE_not;
                break;
            case Op::AND:
                op = GED_OPCODE_and;
                break;
            case Op::OR:
                op = GED_OPCODE_or;
                break;
            case Op::XOR:
                op = GED_OPCODE_xor;
                break;
            case Op::SHR:
                op = GED_OPCODE_shr;
                break;
            case Op::SHL:
                op = GED_OPCODE_shl;
                break;
            case Op::SMOV:
                op = GED_OPCODE_smov;
                break;
            case Op::ASR:
                op = GED_OPCODE_asr;
                break;
            case Op::CMP:
                op = GED_OPCODE_cmp;
                break;
            case Op::CMPN:
                op = GED_OPCODE_cmpn;
                break;
            case Op::CSEL:
                op = GED_OPCODE_csel;
                break;
            case Op::F32TO16:
                op = GED_OPCODE_f32to16;
                break;
            case Op::F16TO32:
                op = GED_OPCODE_f16to32;
                break;
            case Op::BFREV:
                op = GED_OPCODE_bfrev;
                break;
            case Op::BFE:
                op = GED_OPCODE_bfe;
                break;
            case Op::BFI1:
                op = GED_OPCODE_bfi1;
                break;
            case Op::BFI2:
                op = GED_OPCODE_bfi2;
                break;
            case Op::JMPI:
                op = GED_OPCODE_jmpi;
                break;
            case Op::BRD:
                op = GED_OPCODE_brd;
                break;
            case Op::IF:
                op = GED_OPCODE_if;
                break;
            case Op::BRC:
                op = GED_OPCODE_brc;
                break;
            case Op::ELSE:
                op = GED_OPCODE_else;
                break;
            case Op::ENDIF:
                op = GED_OPCODE_endif;
                break;
            case Op::WHILE:
                op = GED_OPCODE_while;
                break;
            case Op::BREAK:
                op = GED_OPCODE_break;
                break;
            case Op::CONT:
                op = GED_OPCODE_cont;
                break;
            case Op::HALT:
                op = GED_OPCODE_halt;
                break;
            case Op::CALL:
                op = GED_OPCODE_call;
                break;
            case Op::CALLA:
                op = GED_OPCODE_calla;
                break;
            case Op::RET:
                op = GED_OPCODE_ret;
                break;
            case Op::GOTO:
                op = GED_OPCODE_goto;
                break;
            case Op::JOIN:
                op = GED_OPCODE_join;
                break;
            case Op::WAIT:
                op = GED_OPCODE_wait;
                break;
            case Op::SEND:
                op = GED_OPCODE_send;
                break;
            case Op::SENDC:
                op = GED_OPCODE_sendc;
                break;
            case Op::SENDS:
                op = GED_OPCODE_sends;
                break;
            case Op::SENDSC:
                op = GED_OPCODE_sendsc;
                break;
            case Op::MATH:
            case Op::MATH_COS:
            case Op::MATH_EXP:
            case Op::MATH_FDIV:
            case Op::MATH_IDIV:
            case Op::MATH_INV:
            case Op::MATH_INVM:
            case Op::MATH_IQOT:
            case Op::MATH_IREM:
            case Op::MATH_LOG:
            case Op::MATH_POW:
            case Op::MATH_RSQT:
            case Op::MATH_RSQTM:
            case Op::MATH_SIN:
            case Op::MATH_SQT:
                op = GED_OPCODE_math;
                break;
            case Op::ADD:
                op = GED_OPCODE_add;
                break;
            case Op::MUL:
                op = GED_OPCODE_mul;
                break;
            case Op::AVG:
                op = GED_OPCODE_avg;
                break;
            case Op::FRC:
                op = GED_OPCODE_frc;
                break;
            case Op::RNDU:
                op = GED_OPCODE_rndu;
                break;
            case Op::RNDD:
                op = GED_OPCODE_rndd;
                break;
            case Op::RNDE:
                op = GED_OPCODE_rnde;
                break;
            case Op::RNDZ:
                op = GED_OPCODE_rndz;
                break;
            case Op::MAC:
                op = GED_OPCODE_mac;
                break;
            case Op::MACH:
                op = GED_OPCODE_mach;
                break;
            case Op::LZD:
                op = GED_OPCODE_lzd;
                break;
            case Op::FBH:
                op = GED_OPCODE_fbh;
                break;
            case Op::FBL:
                op = GED_OPCODE_fbl;
                break;
            case Op::CBIT:
                op = GED_OPCODE_cbit;
                break;
            case Op::ADDC:
                op = GED_OPCODE_addc;
                break;
            case Op::SUBB:
                op = GED_OPCODE_subb;
                break;
            case Op::SAD2:
                op = GED_OPCODE_sad2;
                break;
            case Op::SADA2:
                op = GED_OPCODE_sada2;
                break;
            case Op::DP4:
                op = GED_OPCODE_dp4;
                break;
            case Op::DPH:
                op = GED_OPCODE_dph;
                break;
            case Op::DP3:
                op = GED_OPCODE_dp3;
                break;
            case Op::DP2:
                op = GED_OPCODE_dp2;
                break;
            case Op::LINE:
                op = GED_OPCODE_line;
                break;
            case Op::PLN:
                op = GED_OPCODE_pln;
                break;
            case Op::MAD:
                op = GED_OPCODE_mad;
                break;
            case Op::LRP:
                op = GED_OPCODE_lrp;
                break;
            case Op::MADM:
                op = GED_OPCODE_madm;
                break;
            case Op::NOP:
                op = GED_OPCODE_nop;
                break;
            default:
                break;
            }

            return op;
        }

        static GED_MODEL lowerPlatform(Platform platform)
        {
            GED_MODEL pltf = GED_MODEL_INVALID;
            switch (platform)
            {
            case Platform::GEN7:
                pltf = GED_MODEL_GEN_7;
                break;
            case Platform::GEN7P5:
                pltf = GED_MODEL_GEN_7_5;
                break;
            case Platform::GEN8:
                pltf = GED_MODEL_GEN_8;
                break;
            case Platform::GEN8LP:
                pltf = GED_MODEL_GEN_8_1;
                break;
            case Platform::GEN9:
                pltf = GED_MODEL_GEN_9;
                break;
            case Platform::GEN9LP:
                pltf = GED_MODEL_GEN_9;
                break;
            case Platform::GEN9P5:
                pltf = GED_MODEL_GEN_9;
                break;
            case Platform::GEN10:
                pltf = GED_MODEL_GEN_10;
                break;
            default:
                break;
            }

            return pltf;
        }

        static GED_PRED_CTRL lowerPredCtrl(PredCtrl predMod)
        {
            GED_PRED_CTRL predCtrl = GED_PRED_CTRL_INVALID;

            switch (predMod)
            {
            case PredCtrl::NONE:
                predCtrl = GED_PRED_CTRL_Normal;
                break;
            case PredCtrl::SEQ:
                predCtrl = GED_PRED_CTRL_Sequential;
                break;
            case PredCtrl::ANY2H:
                predCtrl = GED_PRED_CTRL_any2h;
                break;
            case PredCtrl::ANY4H:
                predCtrl = GED_PRED_CTRL_any4h;
                break;
            case PredCtrl::ANY8H:
                predCtrl = GED_PRED_CTRL_any8h;
                break;
            case PredCtrl::ANY16H:
                predCtrl = GED_PRED_CTRL_any16h;
                break;
            case PredCtrl::ANY32H:
                predCtrl = GED_PRED_CTRL_any32h;
                break;
            case PredCtrl::ALL2H:
                predCtrl = GED_PRED_CTRL_all2h;
                break;
            case PredCtrl::ALL4H:
                predCtrl = GED_PRED_CTRL_all4h;
                break;
            case PredCtrl::ALL8H:
                predCtrl = GED_PRED_CTRL_all8h;
                break;
            case PredCtrl::ALL16H:
                predCtrl = GED_PRED_CTRL_all16h;
                break;
            case PredCtrl::ALL32H:
                predCtrl = GED_PRED_CTRL_all32h;
                break;
            case PredCtrl::ANYV:
                predCtrl = GED_PRED_CTRL_anyv;
                break;
            case PredCtrl::ALLV:
                predCtrl = GED_PRED_CTRL_allv;
                break;
            default:
                break;
            }

            return predCtrl;
        }

        static GED_SRC_MOD lowerSrcMod(SrcModifier modSource)
        {
            GED_SRC_MOD srcMod = GED_SRC_MOD_INVALID;

            switch (modSource)
            {
            case SrcModifier::NONE:
                srcMod = GED_SRC_MOD_Normal;
                break;
            case SrcModifier::ABS:
                srcMod = GED_SRC_MOD_Absolute;
                break;
            case SrcModifier::NEG:
                srcMod = GED_SRC_MOD_Negative;
                break;
            case SrcModifier::NEG_ABS:
                srcMod = GED_SRC_MOD_Negative_Absolute;
                break;
            case SrcModifier::NOT:
                srcMod = GED_SRC_MOD_Negative;
                break;
            default:
                break;
            }

            return srcMod;
        }

        static uint32_t combineARF(uint32_t regNum, RegName regT)
        {
            switch (regT)
            {
            case iga::RegName::ARF_NULL:
                regNum |= (ARCH_REG_FILE_NULL << 4);
                break;
            case iga::RegName::ARF_A:
                regNum |= (ARCH_REG_FILE_A << 4);
                break;
            case iga::RegName::ARF_ACC:
                regNum |= (ARCH_REG_FILE_ACC << 4);
                break;
            case iga::RegName::ARF_F:
                regNum |= (ARCH_REG_FILE_F << 4);
                break;
            case iga::RegName::ARF_CE:
                regNum |= (ARCH_REG_FILE_MASK << 4);
                break;
            case iga::RegName::ARF_MSG:
                regNum |= (ARCH_REG_FILE_STACK << 4);
                break;
            case iga::RegName::ARF_SP:
                regNum |= (ARCH_REG_FILE_STACK_DEPTH << 4);
                break;
            case iga::RegName::ARF_SR:
                regNum |= (ARCH_REG_FILE_STATE_REG << 4);
                break;
            case iga::RegName::ARF_CR:
                regNum |= (ARCH_REG_FILE_CNTL_REG << 4);
                break;
            case iga::RegName::ARF_N:
                regNum |= (ARCH_REG_FILE_NCNT_REG << 4);
                break;
            case iga::RegName::ARF_IP:
                regNum |= (ARCH_REG_FILE_IP << 4);
                break;
            case iga::RegName::ARF_TDR:
                regNum |= (ARCH_REG_FILE_TDR_REG << 4);
                break;
            case iga::RegName::ARF_TM:
                regNum |= (ARCH_REG_FILE_PERF_REG << 4);
                break;
            case iga::RegName::ARF_FC:
                regNum |= (ARCH_REG_FILE_FC_REG << 4);
                break;
            case iga::RegName::ARF_DBG:
                regNum |= (ARCH_REG_FILE_DBG_REG << 4);
                break;
            case iga::RegName::GRF_R:
                break;
            case iga::RegName::INVALID:
                IGA_ASSERT_FALSE("invalid arch register in encoding");
                break;
            default:
                break;
            }
            return regNum;
        }


        static GED_SFID lowerSendTFID(uint32_t id, Platform platform)
        {
            GED_SFID tfid = GED_SFID_INVALID;
            switch (id)
            {
            case 0:
                tfid = GED_SFID_NULL;
                break;
            case 2:
                tfid = GED_SFID_SAMPLER;
                break;
            case 3:
                tfid = GED_SFID_GATEWAY;
                break;
            case 4:
                if (platform < Platform::GEN9)
                {
                    tfid = GED_SFID_DP_SAMPLER;
                }
                else
                {
                    tfid = GED_SFID_DP_DC2;
                }
                break;
            case 5:
                tfid = GED_SFID_DP_RC;
                break;
            case 6:
                tfid = GED_SFID_URB;
                break;
            case 7:
                tfid = GED_SFID_SPAWNER;
                break;
            case 8:
                tfid = GED_SFID_VME;
                break;
            case 9:
                if (platform < Platform::GEN9)
                {
                    tfid = GED_SFID_DP_CC;
                }
                else
                {
                    tfid = GED_SFID_DP_DCRO;
                }
                break;
            case 10:
                tfid = GED_SFID_DP_DC0;
                break;
            case 11:
                tfid = GED_SFID_PI;
                break;
            case 12:
                tfid = GED_SFID_DP_DC1;
                break;
            case 13:
                tfid = GED_SFID_CRE;
                break;
            case 15:
                tfid = GED_SFID_INVALID;
                break;
            default:
                break;
            }
            return tfid;
        }

        static GED_SATURATE lowerSaturate(DstModifier dstSat)
        {
            GED_SATURATE gedSat = GED_SATURATE_INVALID;

            switch (dstSat)
            {
            case DstModifier::NONE:
                gedSat = GED_SATURATE_Normal;
                break;
            case DstModifier::SAT:
                gedSat = GED_SATURATE_sat;
                break;
            default:
                break;
            }

            return gedSat;
        }

        static GED_DATA_TYPE lowerDataType(Type opndType)
        {
            GED_DATA_TYPE dataType = GED_DATA_TYPE_INVALID;

            switch (opndType)
            {
            case Type::UD:
                dataType = GED_DATA_TYPE_ud;
                break;
            case Type::D:
                dataType = GED_DATA_TYPE_d;
                break;
            case Type::UW:
                dataType = GED_DATA_TYPE_uw;
                break;
            case Type::W:
                dataType = GED_DATA_TYPE_w;
                break;
            case Type::UB:
                dataType = GED_DATA_TYPE_ub;
                break;
            case Type::B:
                dataType = GED_DATA_TYPE_b;
                break;
            case Type::DF:
                dataType = GED_DATA_TYPE_df;
                break;
            case Type::F:
                dataType = GED_DATA_TYPE_f;
                break;
            case Type::V:
                dataType = GED_DATA_TYPE_v;
                break;
            case Type::VF:
                dataType = GED_DATA_TYPE_vf;
                break;
            case Type::UQ:
                dataType = GED_DATA_TYPE_uq;
                break;
            case Type::UV:
                dataType = GED_DATA_TYPE_uv;
                break;
            case Type::Q:
                dataType = GED_DATA_TYPE_q;
                break;
            case Type::HF:
                dataType = GED_DATA_TYPE_hf;
                break;
            default:
                break;
            }
            return dataType;
        }


        static GED_SPECIAL_ACC lowerSpecialAcc(ImplAcc acc)
        {
            switch (acc)
            {
            case ImplAcc::ACC2: return GED_SPECIAL_ACC_acc2;
            case ImplAcc::ACC3: return GED_SPECIAL_ACC_acc3;
            case ImplAcc::ACC4: return GED_SPECIAL_ACC_acc4;
            case ImplAcc::ACC5: return GED_SPECIAL_ACC_acc5;
            case ImplAcc::ACC6: return GED_SPECIAL_ACC_acc6;
            case ImplAcc::ACC7: return GED_SPECIAL_ACC_acc7;
            case ImplAcc::ACC8: return GED_SPECIAL_ACC_acc8;
            case ImplAcc::ACC9: return GED_SPECIAL_ACC_acc9;
            case ImplAcc::NOACC: return GED_SPECIAL_ACC_noacc;
            default: return GED_SPECIAL_ACC_INVALID;
            }
        }




        static GED_CHANNEL_OFFSET lowerQtrCtrl(ChannelOffset ectrl)
        {
            GED_CHANNEL_OFFSET gedCtrl = GED_CHANNEL_OFFSET_INVALID;
            switch (ectrl)
            {
            case iga::ChannelOffset::M0:
                gedCtrl = GED_CHANNEL_OFFSET_M0;
                break;
            case iga::ChannelOffset::M4:
                gedCtrl = GED_CHANNEL_OFFSET_M4;
                break;
            case iga::ChannelOffset::M8:
                gedCtrl = GED_CHANNEL_OFFSET_M8;
                break;
            case iga::ChannelOffset::M12:
                gedCtrl = GED_CHANNEL_OFFSET_M12;
                break;
            case iga::ChannelOffset::M16:
                gedCtrl = GED_CHANNEL_OFFSET_M16;
                break;
            case iga::ChannelOffset::M20:
                gedCtrl = GED_CHANNEL_OFFSET_M20;
                break;
            case iga::ChannelOffset::M24:
                gedCtrl = GED_CHANNEL_OFFSET_M24;
                break;
            case iga::ChannelOffset::M28:
                gedCtrl = GED_CHANNEL_OFFSET_M28;
                break;
            default:
                break;
            }
            return gedCtrl;
        }

        static GED_EXEC_MASK_OFFSET_CTRL lowerQtrCtrl(ExecSize eSize, ChannelOffset ectrl)
        {
            GED_EXEC_MASK_OFFSET_CTRL gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_INVALID;
            switch (eSize)
            {
            case ExecSize::SIMD1:
            case ExecSize::SIMD2:
                if (ectrl == ChannelOffset::M0)
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_Q1;
                switch (ectrl)
                {
                case ChannelOffset::M0:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_Q1;
                    break;
                case ChannelOffset::M8:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_Q2;
                    break;
                case ChannelOffset::M16:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_Q3;
                    break;
                case ChannelOffset::M24:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_Q4;
                    break;
                default:
                    break;
                }
                break;
            case ExecSize::SIMD4:
                switch (ectrl)
                {
                case ChannelOffset::M0:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_N1;
                    break;
                case ChannelOffset::M4:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_N2;
                    break;
                case ChannelOffset::M8:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_N3;
                    break;
                case ChannelOffset::M12:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_N4;
                    break;
                case ChannelOffset::M16:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_N5;
                    break;
                case ChannelOffset::M20:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_N6;
                    break;
                case ChannelOffset::M24:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_N7;
                    break;
                case ChannelOffset::M28:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_N8;
                    break;
                default:
                    break;
                }
                break;
            case ExecSize::SIMD8:
                switch (ectrl)
                {
                case ChannelOffset::M0:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_Q1;
                    break;
                case ChannelOffset::M8:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_Q2;
                    break;
                case ChannelOffset::M16:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_Q3;
                    break;
                case ChannelOffset::M24:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_Q4;
                    break;
                default:
                    break;
                }
                break;
            case ExecSize::SIMD16:
                switch (ectrl)
                {
                case ChannelOffset::M0:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_H1;
                    break;
                case ChannelOffset::M16:
                    gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_H2;
                    break;
                default:
                    break;
                }
                break;
            case ExecSize::SIMD32:
                gedCtrl = GED_EXEC_MASK_OFFSET_CTRL_Q1;
                break;
            default:
                break;
            }
            return gedCtrl;
        }


        static GED_MASK_CTRL lowerEmask(MaskCtrl ectrl)
        {
            GED_MASK_CTRL eMask = GED_MASK_CTRL_INVALID;

            switch (ectrl)
            {
            case MaskCtrl::NORMAL:
                eMask = GED_MASK_CTRL_Normal;
                break;
            case MaskCtrl::NOMASK:
                eMask = GED_MASK_CTRL_NoMask;
                break;
            default:
                break;
            }

            return eMask;
        }


        static uint32_t lowerExecSize(ExecSize eSize)
        {
            return (uint32_t)ExecSizeToInt(eSize);
        }


        static GED_COND_MODIFIER lowerCondModifier(FlagModifier condMod)
        {
            GED_COND_MODIFIER mod = GED_COND_MODIFIER_INVALID;

            switch (condMod)
            {
            case FlagModifier::NONE:
                mod = GED_COND_MODIFIER_Normal;
                break;
            case FlagModifier::EQ:
                mod = GED_COND_MODIFIER_z;
                break;
            case FlagModifier::NE:
                mod = GED_COND_MODIFIER_nz;
                break;
            case FlagModifier::GT:
                mod = GED_COND_MODIFIER_g;
                break;
            case FlagModifier::GE:
                mod = GED_COND_MODIFIER_ge;
                break;
            case FlagModifier::LT:
                mod = GED_COND_MODIFIER_l;
                break;
            case FlagModifier::LE:
                mod = GED_COND_MODIFIER_le;
                break;
            case FlagModifier::OV:
                mod = GED_COND_MODIFIER_o;
                break;
            case FlagModifier::UN:
                mod = GED_COND_MODIFIER_u;
                break;
            default:
                break;
            }

            return mod;
        }




        static GED_MATH_FC lowerMathFC(Op op)
        {
            switch (op) {
            case Op::MATH_INV:   return GED_MATH_FC_INV; break;
            case Op::MATH_LOG:   return GED_MATH_FC_LOG; break;
            case Op::MATH_EXP:   return GED_MATH_FC_EXP; break;
            case Op::MATH_SQT:   return GED_MATH_FC_SQRT; break;
            case Op::MATH_RSQT:  return GED_MATH_FC_RSQ; break;
            case Op::MATH_POW:   return GED_MATH_FC_POW; break;
            case Op::MATH_SIN:   return GED_MATH_FC_SIN; break;
            case Op::MATH_COS:   return GED_MATH_FC_COS; break;
            case Op::MATH_IDIV:  return GED_MATH_FC_INT_DIV_BOTH; break;
            case Op::MATH_IQOT:  return GED_MATH_FC_INT_DIV_QUOTIENT; break;
            case Op::MATH_IREM:  return GED_MATH_FC_INT_DIV_REMAINDER; break;
            case Op::MATH_FDIV:  return GED_MATH_FC_FDIV; break;
            case Op::MATH_INVM:  return GED_MATH_FC_INVM; break;
            case Op::MATH_RSQTM: return GED_MATH_FC_RSQRTM; break;
            default:             return GED_MATH_FC_INV;
            }
        }

        static GED_BRANCH_CTRL lowerBranchCntrl(BranchCntrl brnch)
        {
            GED_BRANCH_CTRL gedBrnch = GED_BRANCH_CTRL_INVALID;

            switch (brnch)
            {
            case BranchCntrl::OFF:
                gedBrnch = GED_BRANCH_CTRL_Normal;
                break;
            case BranchCntrl::ON:
                gedBrnch = GED_BRANCH_CTRL_Branch;
                break;
            default:
                break;
            }

            return gedBrnch;
        }

        static GED_REG_FILE lowerRegFile(RegName type)
        {
            return (type == RegName::GRF_R) ? GED_REG_FILE_GRF : GED_REG_FILE_ARF;
        }

        static GED_SPECIAL_ACC lowerImplAcc(ImplAcc implAcc)
        {
            switch (implAcc) {
            case ImplAcc::ACC2: return GED_SPECIAL_ACC_acc2;
            case ImplAcc::ACC3: return GED_SPECIAL_ACC_acc3;
            case ImplAcc::ACC4: return GED_SPECIAL_ACC_acc4;
            case ImplAcc::ACC5: return GED_SPECIAL_ACC_acc5;
            case ImplAcc::ACC6: return GED_SPECIAL_ACC_acc6;
            case ImplAcc::ACC7: return GED_SPECIAL_ACC_acc7;
            case ImplAcc::ACC8: return GED_SPECIAL_ACC_acc8;
            case ImplAcc::ACC9: return GED_SPECIAL_ACC_acc9;
            case ImplAcc::NOACC: return GED_SPECIAL_ACC_noacc;
            default: return GED_SPECIAL_ACC_INVALID;
            }
            return GED_SPECIAL_ACC_INVALID;
        }

        static GED_ARCH_REG lowerArchReg(RegName type)
        {
            GED_ARCH_REG archReg;

            switch (type)
            {
            case RegName::ARF_NULL:
                archReg = GED_ARCH_REG_null;
                break;
            case RegName::ARF_A:
                archReg = GED_ARCH_REG_a0;
                break;
            case RegName::ARF_ACC:
                archReg = GED_ARCH_REG_acc;
                break;
            case RegName::ARF_F:
                archReg = GED_ARCH_REG_f;
                break;
            case RegName::ARF_CE:
                archReg = GED_ARCH_REG_ce;
                break;
            case RegName::ARF_MSG:
                archReg = GED_ARCH_REG_msg;
                break;
            case RegName::ARF_SP:
                archReg = GED_ARCH_REG_sp;
                break;
            case RegName::ARF_SR:
                archReg = GED_ARCH_REG_sr0;
                break;
            case RegName::ARF_CR:
                archReg = GED_ARCH_REG_cr0;
                break;
            case RegName::ARF_N:
                archReg = GED_ARCH_REG_n0;
                break;
            case RegName::ARF_IP:
                archReg = GED_ARCH_REG_ip;
                break;
            case RegName::ARF_TDR:
                archReg = GED_ARCH_REG_tdr;
                break;
            case RegName::ARF_TM:
                archReg = GED_ARCH_REG_tm0;
                break;
            case RegName::ARF_FC:
                archReg = GED_ARCH_REG_fc;
                break;
            case RegName::ARF_DBG:
                archReg = GED_ARCH_REG_dbg0;
                break;
            case RegName::GRF_R:
                archReg = GED_ARCH_REG_INVALID;
                break;
            default:
                archReg = GED_ARCH_REG_INVALID;
                break;
            }
            return archReg;
        }


        static uint32_t lowerRegionVert(
            const Region::Vert vt)
        {
            return vt == Region::Vert::VT_VxH ? 3 : static_cast<uint32_t>(vt);
        }


        static uint32_t lowerRegionWidth(
            const Region::Width wi)
        {
            return static_cast<uint32_t>(wi);
        }


        static uint32_t lowerRegionHorz(
            const Region::Horz hz)
        {
            return static_cast<uint32_t>(hz);
        }


        static uint32_t createChanSel(GED_SWIZZLE swizzleX, GED_SWIZZLE swizzleY, GED_SWIZZLE swizzleZ, GED_SWIZZLE swizzleW)
        {
            uint32_t chanSel = swizzleX;
            chanSel |= swizzleY << 2;
            chanSel |= swizzleZ << 4;
            chanSel |= swizzleW << 6;
            return chanSel;
        }
        static uint32_t lowerTernaryRegionVert(
            const Region::Vert vt,
            Platform pltfm)
        {
          return _IGAToGEDTranslation::lowerRegionVert(vt);
        }

    }; //end: class

} //end: namespace

namespace iga
{
    typedef _IGAToGEDTranslation IGAToGEDTranslation;
}


#endif
