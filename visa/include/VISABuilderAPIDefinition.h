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

#ifndef VISA_BUILDER_DEFINITION_H
#define VISA_BUILDER_DEFINITION_H

#include "VISAOptions.h"

typedef enum
{
    LIFETIME_START = 0,
    LIFETIME_END = 1
} VISAVarLifetime;

struct BasicRelocEntry;

class VISAKernel
{
public:

    /********** CREATE VARIABLE APIS START ******************/
    ///  CreateVISAGenVar - create an instance of vISA general variable and return it via decl.
    ///  varName must not be NULL, but need not be unique
    ///  numberElemetns must be [1,4096]
    ///  dataType may be one of UD, D, UW, W, UB, B, UQ, Q, DF, HF, F.
    ///  varAlign must be greater than or equal to the data type of the variable, and may be
    ///  specified only when parentDecl is NULL
    ///  parentDecl, if specified, means that this variable is an alias of another variable
    ///  starting at aliasOffset, which is in bytes.  parentDecl is permitted to be itself an
    ///  alias of another variable.  It is an error if the variable's actual offset in the root
    ///  (non-aliased) variable is not aligned to dataType
    CM_BUILDER_API virtual int CreateVISAGenVar(VISA_GenVar *& decl, const char *varName, int numberElements, VISA_Type dataType,
                                        VISA_Align varAlign, VISA_GenVar *parentDecl = NULL, int aliasOffset = 0) = 0;

    /// CreateVISAGenVar - create an instance of vISA general variable and return it via decl.
    /// This is the same as the previous function, except that it is used to declare an alias to a
    /// file-scope variable
    CM_BUILDER_API virtual int CreateVISAGenVar(VISA_GenVar *& decl, const char *varName, int numberElements, VISA_Type dataType,
                                        VISA_Align varAlign, VISA_FileVar *parentDecl, int aliasOffset) = 0;

    /// CreateVISAAddrVar - create an instance of vISA address variable and return it via decl.
    /// an address variablbe must have type UW
    /// numberelements must be [1, 16]
    CM_BUILDER_API virtual int CreateVISAAddrVar(VISA_AddrVar *& decl, const char *varName, unsigned int numberElements) = 0;

    /// AddKernelAttribute - create an attribute for the kernel.  See the vISA spec for a list
    /// of recognized attributes.
    /// name must be a ASCII string with length <=64
    CM_BUILDER_API virtual int AddKernelAttribute(const char* name, int size, const void *value) = 0;

    /// CreateVISAPredVar - create an instance of vISA predicate variable and return it via decl.
    /// a predicate variable must have type Bool
    /// numberelements must be {1,2,4,8,16,32}
    CM_BUILDER_API virtual int CreateVISAPredVar(VISA_PredVar *& decl, const char* varName, unsigned short numberElements) = 0;

    /// FIXME: we should have different names for them
    /// AddAttributeToVar - add an attribute for the variable.
    CM_BUILDER_API virtual int AddAttributeToVar(VISA_PredVar *decl, const char* varName, unsigned int size, void *val) = 0;

    CM_BUILDER_API virtual int AddAttributeToVar(VISA_SurfaceVar *decl, const char* varName, unsigned int size, void *val) = 0;

    CM_BUILDER_API virtual int AddAttributeToVar(VISA_GenVar *decl, const char* name, unsigned int size, void *val) = 0 ;

    CM_BUILDER_API virtual int AddAttributeToVar(VISA_AddrVar *decl, const char* name, unsigned int size, void *val) = 0 ;

    /// CreateVISASamplerVar - create an instance of vISA sampler variable and return it via decl.
    /// a sampler variable must have type UD
    /// numberElements must be [1,128]
    CM_BUILDER_API virtual int CreateVISASamplerVar(VISA_SamplerVar *&decl, const char* name, unsigned int numberElements) = 0;

    /// CreateVISASurfaceVar - create an instance of vISA surface variable and return it via decl.
    /// a surface variable must have type UD
    /// numberElements must be [1,128]
    CM_BUILDER_API virtual int CreateVISASurfaceVar(VISA_SurfaceVar *&decl, const char* name, unsigned int numberElements) = 0;

    /// CreateVISALabelVar - create an instance of vISA label variable and return it via decl.
    /// a label is either a block label or a subroutine label
    /// namse must be unique for all labels in a kernel/function
    /// FIXME: why is label an operand?
    CM_BUILDER_API virtual int CreateVISALabelVar(VISA_LabelOpnd *& opnd, const char* name, VISA_Label_Kind kind) = 0;

    /// CreateVISAImplicitInputVar - create an input variable from a vISA general variable
    /// offset is the zero-based byte offset of the input
    /// size is in number of bytes for this variable. It must match the size of the variable
    /// kind What type of impliict argument it is
    CM_BUILDER_API virtual int CreateVISAImplicitInputVar(VISA_GenVar *decl, unsigned short offset, unsigned short size, unsigned short kind) = 0;

    /// CreateVISAInputVar - create an input variable from a vISA general variable
    /// offset is the zero-based byte offset of the input
    /// size is in number of bytes for this variable. It must match the size of the variable
    CM_BUILDER_API virtual int CreateVISAInputVar(VISA_GenVar *decl, unsigned short offset, unsigned short size) = 0;

    /// CreateVISAInputVar - create an input variable from a vISA sampler variable
    /// offset is the zero-based byte offset of the input
    /// size is in number of bytes for this variable. It must match the size of the variable
    CM_BUILDER_API virtual int CreateVISAInputVar(VISA_SamplerVar *decl, unsigned short offset, unsigned short size) = 0;

    /// CreateVISAInputVar - create an input variable from a vISA surface variable
    /// offset is the zero-based byte offset of the input
    /// size is in number of bytes for this variable. It must match the size of the variable
    CM_BUILDER_API virtual int CreateVISAInputVar(VISA_SurfaceVar *decl, unsigned short offset, unsigned short size) = 0;

    CM_BUILDER_API virtual int CreateVISAInputVar(VISA_VMEVar *decl, unsigned short offset, unsigned short size) = 0;

    /// GetPredefinedVar - return a handle to a predefined general variable (e.g., r0)
    CM_BUILDER_API virtual int GetPredefinedVar(VISA_GenVar *&predDcl, PreDefined_Vars varName) = 0;

    /// GetPredefinedSurface - return a handle to a predefined surface (e.g., SLM surface)
    CM_BUILDER_API virtual int GetPredefinedSurface(VISA_SurfaceVar *&surfDcl, PreDefined_Surface surfaceName) = 0;

	/// GetBindlessSampler - return the pre-defined bindless sampler index
	CM_BUILDER_API virtual int GetBindlessSampler(VISA_SamplerVar *&samplerDcl) = 0;
    /********** CREATE VARIALBE APIS END ******************/

    /********** CREATE OPERAND APIS START ******************/
    /// It is not permitted to use an operand in more than one instruction; fresh operands
    /// should be created from the variable for each instruction.

    /// CreateVISAAddressSrcOperand -- create a vISA address source operand (A_N(i)) from an address variable
    /// offset is the offset in number of elements
    /// width is the number of contiguous data elements accessed for this operand
    CM_BUILDER_API virtual int CreateVISAAddressSrcOperand(VISA_VectorOpnd *&opnd, VISA_AddrVar *decl, unsigned int offset, unsigned int width) = 0;

    /// CreateVISAAddressDstOperand -- create a vISA address destination operand (A_N(i)) from an address variable
    /// offset is the offset in number of elements
    CM_BUILDER_API virtual int CreateVISAAddressDstOperand(VISA_VectorOpnd *&opnd, VISA_AddrVar *decl, unsigned int offset) = 0;

    /// CreateVISAAddressOfOperand -- create a vISA addressof operand (&V_N+offset) from a general variable
    /// offset is the offset in bytes from the variable base
    CM_BUILDER_API virtual int CreateVISAAddressOfOperand(VISA_VectorOpnd *&cisa_opnd, VISA_GenVar *decl, unsigned int offset) = 0;

    /// CreateVISAAddressOfOperand -- create a vISA addressof operand (&V_N+offset) from a surface variable
    /// offset is the offset in bytes from the variable base
    CM_BUILDER_API virtual int CreateVISAAddressOfOperand(VISA_VectorOpnd *&cisa_opnd, VISA_SurfaceVar *decl, unsigned int offset) = 0;

    /// CreateVISAIndirectSrcOperand -- create a vISA indirect source operand
    /// (r[A_N(addrOffset), immedOffset]<verticalStride;width,horizontalStride>:type)
    /// mod is the source modifier
    /// addrOffset is in number of elements
    /// immedOffset is in byte and must be [-512, 511]
    /// verticalStride, width, and horizontalStride encode the 2D source region
    /// FIXME: should this be removed?
    CM_BUILDER_API virtual int CreateVISAIndirectSrcOperand(VISA_VectorOpnd *& opnd, VISA_AddrVar *cisa_decl, VISA_Modifier mod, unsigned int addrOffset, short immediateOffset,
                                                    unsigned short verticalStride, unsigned short width, unsigned short horizontalStride, VISA_Type type) = 0;

    /// CreateVISAIndirectDstOperand -- create a vISA indirect dst operand
    /// (r[A_N(addrOffset), immedOffset]<horizontalStride>:type)
    /// addrOffset is in number of elements
    /// immedOffset is in byte and must be [-512, 511]
    /// horizontalStride encode the 1D destination region
    CM_BUILDER_API virtual int CreateVISAIndirectDstOperand(VISA_VectorOpnd *& opnd, VISA_AddrVar *decl, unsigned int addrOffset, short immediateOffset,
                                                    unsigned short horizontalStride, VISA_Type type) = 0;

    /// CreateVISAIndirectOperandVxH -- create a vISA indirect source operand with multiple addresses
    /// (r[A_N(addrOffset), immedOffset]<1,0>:type)
    /// addrOffset is in number of elements
    /// immedOffset is in byte and must be [-512, 511]
    CM_BUILDER_API virtual int CreateVISAIndirectOperandVxH(VISA_VectorOpnd *& cisa_opnd, VISA_AddrVar *decl, unsigned int addrOffset, short immediateOffset, VISA_Type type) = 0;

    /// CreateVISAPredicateOperand -- create a vISA predicate operand that can be used for predicated execution
    CM_BUILDER_API virtual int CreateVISAPredicateOperand(VISA_PredOpnd *& opnd, VISA_PredVar *decl, VISA_PREDICATE_STATE state, VISA_PREDICATE_CONTROL cntrl) = 0;

    /// CreateVISASrcOperand -- create a vISA direct source operand from a general variable
    /// (V_N(offset)<vStride;widht,hStride>)
    /// FIXME: combine rowOffset and colOffsret into a single element offset
    CM_BUILDER_API virtual int CreateVISASrcOperand(VISA_VectorOpnd *& opnd, VISA_GenVar *cisa_decl, VISA_Modifier mod, unsigned short vStride, unsigned short width, unsigned short hStride,
                                            unsigned char rowOffset, unsigned char colOffset) = 0;

    /// CreateVISADstOperand -- create a vISA direct destination operand from a general variable
    /// (V_N(offset)<hStride>)
    /// FIXME: combine rowOffset and colOffsret into a single element offset
    CM_BUILDER_API virtual int CreateVISADstOperand(VISA_VectorOpnd *&opnd, VISA_GenVar *decl, unsigned short hStride, unsigned char rowOffset, unsigned char colOffset) = 0;

    /// CreateVISAImmediate -- create a vISA immediate operand based on val and type
    /// sizeof(type) bytes will be read from val and converted to the immediate value with the specified type
    CM_BUILDER_API virtual int CreateVISAImmediate(VISA_VectorOpnd *&opnd, const void *val, VISA_Type type) = 0;

    /// CreateVISAStateOperand -- create a vISA state operand (S_N(offset)) from either a surface variable
    /// offset is in number of elements
    CM_BUILDER_API virtual int CreateVISAStateOperand(VISA_VectorOpnd *&opnd, VISA_SurfaceVar *decl, unsigned char offset, bool useAsDst) = 0;

    /// CreateVISAStateOperand -- create a vISA state operand (S_N(offset)) from either a surface variable
    /// offset is in number of elements
    CM_BUILDER_API virtual int CreateVISAStateOperand(VISA_VectorOpnd *&opnd, VISA_SurfaceVar *decl, uint8_t size, unsigned char offset, bool useAsDst) = 0;

    /// CreateVISAStateOperand -- create a vISA state operand (S_N(offset)) from either a sampler variable
    /// offset is in number of elements
    CM_BUILDER_API virtual int CreateVISAStateOperand(VISA_VectorOpnd *&opnd, VISA_SamplerVar *decl, unsigned char offset, bool useAsDst) = 0;

    CM_BUILDER_API virtual int CreateVISAStateOperand(VISA_VectorOpnd *&opnd, VISA_SamplerVar *decl, uint8_t size, unsigned char offset, bool useAsDst) = 0;

    CM_BUILDER_API virtual int CreateVISAStateOperand(VISA_VectorOpnd *&opnd, VISA_VMEVar *decl, unsigned char offset, bool useAsDst) = 0;

    CM_BUILDER_API virtual int CreateVISAStateOperandHandle(VISA_StateOpndHandle *&opnd, VISA_SurfaceVar *decl) = 0;

    CM_BUILDER_API virtual int CreateVISAStateOperandHandle(VISA_StateOpndHandle *&opnd, VISA_SamplerVar *decl) = 0;

    CM_BUILDER_API virtual int CreateVISAStateOperandHandle(VISA_StateOpndHandle *&opnd, VISA_VMEVar *decl) = 0;

    /// CreateVISARawOperand -- create a vISA raw operand (V_N + offset) from a general variable
    /// offset is in number of bytes
    /// a raw operand must be GRF (256 byte) aligned
    CM_BUILDER_API virtual int CreateVISARawOperand(VISA_RawOpnd *&opnd, VISA_GenVar *decl, unsigned short offset) = 0;

    /// CreateVISANullRawOperand -- A short hand for createing V0<0;1,0>
    CM_BUILDER_API virtual int CreateVISANullRawOperand(VISA_RawOpnd *&opnd) = 0;
    /********** CREATE OPERAND APIS END ******************/

    /********** APPEND INSTRUCTION APIS START ******************/

    /// AppendVISAArithmeticInst -- append an one-source vISA arithmetic instruction to this kernel
    /// [pred] op[.sat] (emask, execSize) dst src0
    CM_BUILDER_API virtual int AppendVISAArithmeticInst(ISA_Opcode opcode, VISA_PredOpnd *pred, bool satMode, Common_VISA_EMask_Ctrl emask,
                                          Common_ISA_Exec_Size executionSize, VISA_VectorOpnd *tmpDst, VISA_VectorOpnd *src0) = 0;

    /// AppendVISAArithmeticInst -- append a two-source vISA arithmetic instruction to this kernel
    /// [pred] op[.sat] (emask, execSize) dst src0 src1
    CM_BUILDER_API virtual int AppendVISAArithmeticInst(ISA_Opcode opcode, VISA_PredOpnd *pred, bool satMode, Common_VISA_EMask_Ctrl emask,
                                          Common_ISA_Exec_Size executionSize, VISA_VectorOpnd *tmpDst, VISA_VectorOpnd *src0, VISA_VectorOpnd *src1) = 0;

    /// AppendVISAArithmeticInst -- append a three-source vISA arithmetic instruction to this kernel
    /// [pred] op[.sat] (emask, execSize) dst src0 src1 src2
    CM_BUILDER_API virtual int AppendVISAArithmeticInst(ISA_Opcode opcode, VISA_PredOpnd *pred, bool satMode, Common_VISA_EMask_Ctrl emask,
                                          Common_ISA_Exec_Size executionSize, VISA_VectorOpnd *tmpDst, VISA_VectorOpnd *src0, VISA_VectorOpnd *src1, VISA_VectorOpnd *src2) = 0;

    /// AppendVISAArithmeticTwoDstInst -- append a two-dst, two-source vISA arithmetic instruction to this kernel
    /// [pred] op (emask, execSize) dst0 dst1 src0 src1
    /// This is used by addc and subb
    CM_BUILDER_API virtual int AppendVISAArithmeticInst(ISA_Opcode opcode, VISA_PredOpnd *pred, Common_VISA_EMask_Ctrl emask,
                                          Common_ISA_Exec_Size executionSize, VISA_VectorOpnd *dst1, VISA_VectorOpnd *carry_borrow, VISA_VectorOpnd *src0, VISA_VectorOpnd *src1) = 0;

    /// FIXME: we should probably have separate API for logic and shift instructions, as the arguments they expect are quite different
    /// AppendVISALogicOrShiftInst -- append a two-dst, two-source vISA arithmetic instruction to this kernel
    /// [pred] op[.sat] (emask, execSize) dst src0 src1 [src2 src3]
    /// src2 is used only for bfe and bfi
    /// src3 is used only for bfi
    CM_BUILDER_API virtual int AppendVISALogicOrShiftInst(ISA_Opcode opcode, VISA_PredOpnd *pred, bool satMode, Common_VISA_EMask_Ctrl emask,
                                            Common_ISA_Exec_Size executionSize, VISA_VectorOpnd *dst, VISA_VectorOpnd *src0, VISA_VectorOpnd *src1, VISA_VectorOpnd *src2 = NULL, VISA_VectorOpnd *src3 = NULL) = 0;

    ///AppendVISALogicOrShiftInst
    ///Used to perform logic operations on predicates.
    ///The operand is constructed internally.
    ///Enforces none matching types
    CM_BUILDER_API virtual int AppendVISALogicOrShiftInst(ISA_Opcode opcode, Common_VISA_EMask_Ctrl emask,
                                            Common_ISA_Exec_Size executionSize, VISA_PredVar *dst, VISA_PredVar *src0, VISA_PredVar *src1) = 0;
    /// FIXME: why not use the addressof_opnd if we had introduced it earlier?
    /// AppendVISAAddrAddInst -- append an address add instruction to this kernel
    /// addr_add (emask, execSize) dst src0 src1
    CM_BUILDER_API virtual int AppendVISAAddrAddInst(Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize, VISA_VectorOpnd *dst, VISA_VectorOpnd *src0, VISA_VectorOpnd *src1) = 0;

    /// AppendVISADataMovementInst -- append a one-source data movement instruction to this kernel
    /// [pred] op[.sat] (emask, execSize) dst src0
    CM_BUILDER_API virtual int AppendVISADataMovementInst(ISA_Opcode opcode, VISA_PredOpnd *pred, bool satMod, Common_VISA_EMask_Ctrl emask,
                                            Common_ISA_Exec_Size executionSize, VISA_VectorOpnd *dst, VISA_VectorOpnd *src0) = 0;

    /// AppendVISADataMovementInst -- append a two-source data movement instruction to this kernel
    /// [pred] op[.sat] (emask, execSize) dst src0 src1
    CM_BUILDER_API virtual int AppendVISADataMovementInst(ISA_Opcode opcode, VISA_PredOpnd *pred, bool satMod, Common_VISA_EMask_Ctrl emask,
                                            Common_ISA_Exec_Size executionSize, VISA_VectorOpnd *dst, VISA_VectorOpnd *src0, VISA_VectorOpnd *src1) = 0;

    ///AppendVISAPredicateMove
    ///Moves the context of the Predicate in to a Vector Operand.
    ///Predicate operand is constructed internally
    CM_BUILDER_API virtual int AppendVISAPredicateMove(VISA_VectorOpnd *dst, VISA_PredVar *src0) = 0;

    ///AppendVISASetP
    ///Moves the content from the vector operand back in to Predicate operand.
    ///Destination Predicate operand is constructed internally.
    CM_BUILDER_API virtual int AppendVISASetP(Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize, VISA_PredVar *dst, VISA_VectorOpnd *src0) = 0;

    /// AppendVISAMinMaxInst -- append a two-source data movement instruction to this kernel
    /// min/max[.sat] (emask, execSize) dst src0 src1
    CM_BUILDER_API virtual int AppendVISAMinMaxInst(CISA_MIN_MAX_SUB_OPCODE subOpcode, bool satMod, Common_VISA_EMask_Ctrl emask,
                                            Common_ISA_Exec_Size executionSize, VISA_VectorOpnd *dst, VISA_VectorOpnd *src0, VISA_VectorOpnd *src1) = 0;

    /// AppendVISAComparisonInst -- append a two-source comparison instruction to this kernel
    ///                             Destination is a flag register.
    /// cmp.sub_op.f# (emask, execSize) nullDst src0 src1
    CM_BUILDER_API virtual int AppendVISAComparisonInst(Common_ISA_Cond_Mod sub_op, Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize, VISA_PredVar *dst, VISA_VectorOpnd *src0, VISA_VectorOpnd *src1) = 0;

    /// AppendVISAComparisonInst -- append a two-source comparison instruction to this kernel.
    ///                             Destination is a GRF register.
    ///                             Flag register is updated but live range is only this instruction.
    /// cmp.sub_op.f# (emask, execSize) dst src0 src1
    CM_BUILDER_API virtual int AppendVISAComparisonInst(Common_ISA_Cond_Mod sub_op, Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize, VISA_VectorOpnd *dst, VISA_VectorOpnd *src0, VISA_VectorOpnd *src1) = 0;

    /// FIXME: we should separate goto from other instructions, as it's the only one that takes a label
    /// AppendVISACFSIMDInst -- append a structured SIMD control flow instruction to this kernel
    /// [pred] op (emask, execSize)
    CM_BUILDER_API virtual int AppendVISACFSIMDInst(ISA_Opcode opcode, VISA_PredOpnd *pred, Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize, VISA_LabelOpnd *label = NULL) = 0;

    /// AppendVISACFLabelInst -- append a label instruction to this kernel
    /// label:
    CM_BUILDER_API virtual int AppendVISACFLabelInst(VISA_LabelOpnd *label) = 0;

    /// AppendVISACFJmpInst -- append a scalar jmp instruction to this kernel
    /// [pred] jmp (NoMask, 1) label
    CM_BUILDER_API virtual int AppendVISACFJmpInst(VISA_PredOpnd *pred, VISA_LabelOpnd *label) = 0;

    /// AppendVISACFCallInst -- append a subroutine call instruction to this kernel
    /// [pred] call (emask, execSize) label
    CM_BUILDER_API virtual int AppendVISACFCallInst(VISA_PredOpnd *pred, Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize, VISA_LabelOpnd *label) = 0;

    /// AppendVISACFRetInst -- append a subroutine return instruction to this kernel
    /// [pred] ret (emask, execSize)
    CM_BUILDER_API virtual int AppendVISACFRetInst(VISA_PredOpnd *pred, Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize) = 0;

    /// AppendVISACFFunctionCallInst -- append a function call instruction to this kernel
    /// [pred] fcall (emask, execSize) function
    /// argSize must be [0, sizeof(Arg)]
    /// returnSize must be [0, sizeof(RetVal)]
    CM_BUILDER_API virtual int AppendVISACFFunctionCallInst(VISA_PredOpnd *pred, Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize, unsigned short functionID, unsigned char argSize, unsigned char returnSize) = 0;

    /// AppendVISACFFunctionRetInst -- append a function return instruction to this kernel
    /// [pred] fret (emask, execSize)
    CM_BUILDER_API virtual int AppendVISACFFunctionRetInst(VISA_PredOpnd *pred, Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize) = 0;

    /// AppendVISACFSwitchJmpInst -- append a switch jump instruction to this kernel
    /// switchjmp (NoMask, 1) index (label0, label1, ... labelN-1)
    /// labelCount must be in [1,32]
    CM_BUILDER_API virtual int AppendVISACFSwitchJMPInst(VISA_VectorOpnd *index, unsigned char labelCount, VISA_LabelOpnd **labels) = 0;

    /// AppendVISASurfAccessDWordAtomicInst -- append a dword atomic write instruction to this kernel
    /// globalOffset and elementOffset are both in unit of dword
    CM_BUILDER_API virtual int AppendVISASurfAccessDwordAtomicInst(
        CMAtomicOperations subOp, bool is16Bit, Common_VISA_EMask_Ctrl emask,
        Common_ISA_Exec_Size executionSize, VISA_StateOpndHandle *surface,
        VISA_VectorOpnd *globalOffset, VISA_RawOpnd *elementOffset,
        VISA_RawOpnd *src0, VISA_RawOpnd *src1, VISA_RawOpnd *dst) = 0;

    /// AppendVISASurfAccessDWordAtomicInst -- append a dword atomic write instruction to this kernel
    /// NOTE: offsets are both in unit of BYTE!
    CM_BUILDER_API virtual int AppendVISASurfAccessDwordAtomicInst(
        VISA_PredOpnd *pred, CMAtomicOperations subOpc, bool is16Bit,
        Common_VISA_EMask_Ctrl eMask, Common_ISA_Exec_Size execSize,
        VISA_StateOpndHandle *surface, VISA_RawOpnd *offsets,
        VISA_RawOpnd *src0, VISA_RawOpnd *src1, VISA_RawOpnd *dst) = 0;

    /// FIXME: why not have separate functions for them?
    /// AppendVISASurfAccessGatherScatterInst -- append gather/scatter instruction to this kernel
    /// globalOffset and elementOffset are both in unit of element size
    CM_BUILDER_API virtual int AppendVISASurfAccessGatherScatterInst(ISA_Opcode opcode, Common_VISA_EMask_Ctrl emask, GATHER_SCATTER_ELEMENT_SIZE elementSize,
                                                      Common_ISA_Exec_Size executionSize, VISA_StateOpndHandle *surface, VISA_VectorOpnd *globalOffset, VISA_RawOpnd *elementOffset, VISA_RawOpnd *srcDst) = 0;

    /// AppendVISASurfAccessGather4Scatter4Inst -- append a dword gather4/scatter4 instruction to this kernel
    /// globalOffset and elementOffset are both in unit of dwords
    CM_BUILDER_API virtual int AppendVISASurfAccessGather4Scatter4Inst(ISA_Opcode opcode, VISAChannelMask chMask, Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize,
                                                        VISA_StateOpndHandle *surface, VISA_VectorOpnd *globalOffset, VISA_RawOpnd *elementOffset, VISA_RawOpnd *srcDst) = 0;

    /// AppendVISASurfAccessGather4Scatter4TypedInst -- append a typed dword gather4/scatter4 instruction to this kernel
    /// uOffset, vOffset, and rOffset are all in unit of pixels.
    /// vOffset should be set to V0 for 1D surfaces
    /// rOffset should be set to V0 for 1D and 2D surfaces
    CM_BUILDER_API virtual int AppendVISASurfAccessGather4Scatter4TypedInst(ISA_Opcode opcode,
                                                                            VISA_PredOpnd *pred,
                                                                            VISAChannelMask chMask,
                                                                            Common_VISA_EMask_Ctrl emask,
                                                                            Common_ISA_Exec_Size executionSize,
                                                                            VISA_StateOpndHandle *surface,
                                                                            VISA_RawOpnd *uOffset,
                                                                            VISA_RawOpnd *vOffset,
                                                                            VISA_RawOpnd *rOffset,
                                                                            VISA_RawOpnd *lod,
                                                                            VISA_RawOpnd *dst) = 0;

    /// AppendVISASurfAccessGather4Scatter4ScaledInst
    /// --append a dword gather4/scatter4 instruction to this kernel
    /// @globalOffset
    /// @offsets
    /// @dstSrc
    CM_BUILDER_API virtual int
        AppendVISASurfAccessGather4Scatter4ScaledInst(ISA_Opcode                opcode,
                                                      VISA_PredOpnd             *pred,
                                                      Common_VISA_EMask_Ctrl    eMask,
                                                      Common_ISA_Exec_Size      execSize,
                                                      VISAChannelMask           chMask,
                                                      VISA_StateOpndHandle      *surface,
                                                      VISA_VectorOpnd           *globalOffset,
                                                      VISA_RawOpnd              *offsets,
                                                      VISA_RawOpnd              *dstSrc) = 0;

    /// AppendVISASurfAccessStrBufLdStInst
    /// --append a strbuf load/store (or gather4/scatter4)
    /// @surface
    /// @uOffsets
    /// @vOffsets
    CM_BUILDER_API virtual int
        AppendVISASurfAccessStrBufLdStInst(ISA_Opcode                opcode,
                                           VISA_PredOpnd             *pred,
                                           Common_VISA_EMask_Ctrl    eMask,
                                           Common_ISA_Exec_Size      execSize,
                                           VISAChannelMask           chMask,
                                           VISA_StateOpndHandle      *surface,
                                           VISA_RawOpnd              *uOffsets,
                                           VISA_RawOpnd              *vOffsets,
                                           VISA_RawOpnd              *dstSrc) = 0;

    /// AppendVISASurfAccessScatterScaledInst
    /// --append a (1/2/4) byte gather/scatter instruction to this kernel
    /// @globalOffset
    /// @offsets
    /// @dstSrc
    CM_BUILDER_API virtual int
        AppendVISASurfAccessScatterScaledInst(ISA_Opcode                opcode,
                                              VISA_PredOpnd             *pred,
                                              Common_VISA_EMask_Ctrl    eMask,
                                              Common_ISA_Exec_Size      execSize,
                                              Common_ISA_SVM_Block_Num  numBlocks,
                                              VISA_StateOpndHandle      *surface,
                                              VISA_VectorOpnd           *globalOffset,
                                              VISA_RawOpnd              *offsets,
                                              VISA_RawOpnd              *dstSrc) = 0;

    /// AppendVISASurfAccessMediaLoadStoreInst -- append a media block load/store instruction to this kernel
    /// blockWdith, blockHeight, and xOffset are in unit of bytes
    /// yOffset is in number of rows
    CM_BUILDER_API virtual int AppendVISASurfAccessMediaLoadStoreInst(ISA_Opcode opcode, MEDIA_LD_mod modifier, VISA_StateOpndHandle *surface, unsigned char blockWidth,
                                                       unsigned char blockHeight, VISA_VectorOpnd *xOffset, VISA_VectorOpnd *yOffset, VISA_RawOpnd *srcDst,
                                                       CISA_PLANE_ID plane = CISA_PLANE_Y) = 0;

    /// AppendVISASurfAccessOWordLoadStoreInst -- append an oword load/store instruction to this kernel
    /// oword_load_aligned: offset is in unit of owords (16 bytes)
    /// oword_load_unaligned: offset is in unit of bytes, but must be dword aligned
    CM_BUILDER_API virtual int AppendVISASurfAccessOwordLoadStoreInst(ISA_Opcode opcode, Common_VISA_EMask_Ctrl emask, VISA_StateOpndHandle *surface, Common_ISA_Oword_Num size, VISA_VectorOpnd *offset, VISA_RawOpnd *srcDst) = 0;

    /// AppendVISASvmBlockStoreInst -- append an A64 oword store instruction to this kernel
    /// address is in unit of owords (16 bytes)
    /// unaligned must be false
    CM_BUILDER_API virtual int AppendVISASvmBlockStoreInst(Common_ISA_Oword_Num size, bool unaligned, VISA_VectorOpnd* address, VISA_RawOpnd *src) = 0;

    /// AppendVISASvmBlockLoadInst -- append an A64 oword load instruction to this kernel
    /// address is in unit of owords (16 bytes)
    CM_BUILDER_API virtual int AppendVISASvmBlockLoadInst(Common_ISA_Oword_Num size, bool unaligned, VISA_VectorOpnd* address, VISA_RawOpnd *dst) = 0;

    /// AppendVISASvmScatterInst -- append an A64 byte scattered write instruction to this kernel
    /// address is in unit of bytes
    CM_BUILDER_API virtual int AppendVISASvmScatterInst(VISA_PredOpnd *pred,
        Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize,
        Common_ISA_SVM_Block_Type blockType, Common_ISA_SVM_Block_Num numBlocks,
        VISA_RawOpnd* address, VISA_RawOpnd *src) = 0;

    /// AppendVISASvmGatherInst -- append an A64 byte scattered read instruction to this kernel
    /// address is in unit of bytes
    CM_BUILDER_API virtual int AppendVISASvmGatherInst (VISA_PredOpnd *pred,
        Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize,
        Common_ISA_SVM_Block_Type blockType, Common_ISA_SVM_Block_Num numBlocks,
        VISA_RawOpnd* address, VISA_RawOpnd *dst) = 0;

    /// AppendVISASvmAtomicInst -- append an A64 untyped atomic integer instruction to this kernel
    /// address is in unit of bytes
    CM_BUILDER_API virtual int
    AppendVISASvmAtomicInst(VISA_PredOpnd *pred, Common_VISA_EMask_Ctrl emask,
                            Common_ISA_Exec_Size executionSize,
                            CMAtomicOperations op, bool is16Bit,
                            VISA_RawOpnd *addresses, VISA_RawOpnd *src0,
                            VISA_RawOpnd *src1, VISA_RawOpnd *dst) = 0;

    /// AppendVISASvmGather4ScaledInst -- append an A64 byte scaled read instruction to this kernel
    /// address is in unit of bytes
    CM_BUILDER_API virtual int AppendVISASvmGather4ScaledInst(VISA_PredOpnd             *pred,
                                                              Common_VISA_EMask_Ctrl    eMask,
                                                              Common_ISA_Exec_Size      execSize,
                                                              VISAChannelMask           channelMask,
                                                              VISA_VectorOpnd           *address,
                                                              VISA_RawOpnd              *offsets,
                                                              VISA_RawOpnd              *dst) = 0;

    /// AppendVISASvmScatter4ScaledInst -- append an A64 byte scaled write instruction to this kernel
    /// address is in unit of bytes
    CM_BUILDER_API virtual int AppendVISASvmScatter4ScaledInst(VISA_PredOpnd            *pred,
                                                               Common_VISA_EMask_Ctrl   eMask,
                                                               Common_ISA_Exec_Size     execSize,
                                                               VISAChannelMask          channelMask,
                                                               VISA_VectorOpnd          *address,
                                                               VISA_RawOpnd             *offsets,
                                                               VISA_RawOpnd             *src) = 0;

    CM_BUILDER_API virtual int AppendVISASurfAccessTransposeLoadInst(VISA_StateOpndHandle *surface, unsigned char blockWidth, unsigned char blockHeight,
                                                      VISA_VectorOpnd *xOffset, VISA_VectorOpnd *yOffset, VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISASILoad(VISA_StateOpndHandle *surface, VISAChannelMask channel, bool isSIMD16,
        VISA_RawOpnd *uOffset, VISA_RawOpnd *vOffset, VISA_RawOpnd *rOffset, VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISASISample(Common_VISA_EMask_Ctrl emask, VISA_StateOpndHandle *surface, VISA_StateOpndHandle *sampler, VISAChannelMask channel, bool isSIMD16,
        VISA_RawOpnd *uOffset, VISA_RawOpnd *vOffset, VISA_RawOpnd *rOffset, VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISASISampleUnorm(VISA_StateOpndHandle *surface, VISA_StateOpndHandle *sampler, VISAChannelMask channel,
        VISA_VectorOpnd *uOffset, VISA_VectorOpnd *vOffset, VISA_VectorOpnd *deltaU, VISA_VectorOpnd *deltaV, VISA_RawOpnd *dst, CHANNEL_OUTPUT_FORMAT out) = 0;

    /// AppendVISASyncInst -- appends one of barrier, fence, yield
    /// The optional argument is applicable only to the fence instruction
    CM_BUILDER_API virtual int AppendVISASyncInst(ISA_Opcode opcode, unsigned char mask = 0) = 0;

    /// AppendVISASyncInst -- create a sendc fence instruction to enforce thread dependencies
    /// The mask, if not NULL, can be used to selectively disable waiting on some of the parent threads
    CM_BUILDER_API virtual int AppendVISAWaitInst(VISA_VectorOpnd *mask) = 0;

    /// AppendVISAPredBarrierInst -- create a predicated barrier
    /// @mask predicate mask
    /// @dst DW0[15:0] contains predicated barrier mask sum
    CM_BUILDER_API virtual int AppendVISAPredBarrierInst(VISA_VectorOpnd *mask, VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISAMiscFileInst(char *fileName) = 0;

    CM_BUILDER_API virtual int AppendVISAMiscLOC(unsigned int lineNumber) = 0;

    /// AppendVISAMiscRawSend -- create a GEN send instruction
    /// [pred] send/sendc (esize) <dst> <src> <exMsgDesc> <desc> {emask}
    /// bit 0 of modifiers controls whether it's send (0) or sendc (1)
    CM_BUILDER_API virtual int AppendVISAMiscRawSend(VISA_PredOpnd *pred, Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize, unsigned char modifiers,
                                      unsigned int exMsgDesc, unsigned char srcSize, unsigned char dstSize, VISA_VectorOpnd *desc,
                                      VISA_RawOpnd *src, VISA_RawOpnd *dst) = 0;

    /// AppendVISAMiscRawSend -- create a GEN split send instruction
    /// [pred] sends/sendsc (esize) <dst> <src0> <src1> <exMsgDesc> <desc> {emask}
    /// bit 0 of modifiers controls whether it's sends (0) or sendsc (1)
    CM_BUILDER_API virtual int AppendVISAMiscRawSends(VISA_PredOpnd *pred, Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize, unsigned char modifiers,
                                       unsigned ffid, VISA_VectorOpnd *exMsgDesc, unsigned char src0Size, unsigned char src1Size, unsigned char dstSize, VISA_VectorOpnd *desc,
                                      VISA_RawOpnd *src0, VISA_RawOpnd *src1, VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISALifetime(VISAVarLifetime startOrEnd, VISA_VectorOpnd *varId) = 0;

    /********** APPEND MEDIA Instructions START ******************/

    CM_BUILDER_API virtual int AppendVISAMiscVME_FBR(VISA_StateOpndHandle *surface, VISA_RawOpnd *UNIInput, VISA_RawOpnd *FBRInput, VISA_VectorOpnd* FBRMbMode, VISA_VectorOpnd *FBRSubMbShape,
                                      VISA_VectorOpnd *FBRSubPredMode, VISA_RawOpnd *output) = 0;

    CM_BUILDER_API virtual int AppendVISAMiscVME_IME(VISA_StateOpndHandle *surface, unsigned char streamMode, unsigned char searchControlMode, VISA_RawOpnd *UNIInput,
                                      VISA_RawOpnd *IMEInput, VISA_RawOpnd *ref0, VISA_RawOpnd *ref1, VISA_RawOpnd *costCenter, VISA_RawOpnd *output) = 0;

    CM_BUILDER_API virtual int AppendVISAMiscVME_SIC(VISA_StateOpndHandle *surface, VISA_RawOpnd *UNIInput, VISA_RawOpnd *SICInput, VISA_RawOpnd *output) = 0;

    CM_BUILDER_API virtual int AppendVISAMiscVME_IDM(VISA_StateOpndHandle *surface, VISA_RawOpnd *UNIInput, VISA_RawOpnd *IDMInput, VISA_RawOpnd *output) = 0;

    CM_BUILDER_API virtual int AppendVISAMEAVS( VISA_StateOpndHandle *surface,  VISA_StateOpndHandle *sampler,          VISAChannelMask channel,                VISA_VectorOpnd *uOffset,
                                                VISA_VectorOpnd *vOffset,       VISA_VectorOpnd *deltaU,                VISA_VectorOpnd *deltaV,                VISA_VectorOpnd *u2d,
                                                VISA_VectorOpnd *v2d,           VISA_VectorOpnd *groupID,               VISA_VectorOpnd *verticalBlockNumber,   OutputFormatControl cntrl,
                                                AVSExecMode execMode,           VISA_VectorOpnd *iefBypass,             VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISAVABooleanCentroid(VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, VISA_VectorOpnd *vSize, VISA_VectorOpnd *hSize, VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISAVACentroid(VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, VISA_VectorOpnd *vSize, VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISAVAConvolve(VISA_StateOpndHandle *sampler, VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, CONVExecMode execMode, bool isBigKernel, VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISAVAErodeDilate(EDMode subOp, VISA_StateOpndHandle *sampler, VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, EDExecMode execMode, VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISAVAMinMax(VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, VISA_VectorOpnd *mmMode, VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISAVAMinMaxFilter(VISA_StateOpndHandle *sampler, VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, OutputFormatControl cntrl, MMFExecMode execMode, VISA_VectorOpnd *mmfMode, VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISAVACorrelationSearch(VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, VISA_VectorOpnd *vOrigin, VISA_VectorOpnd *hOrigin,
                                                            VISA_VectorOpnd *xDirectionSize, VISA_VectorOpnd *yDirectionSize,
                                                            VISA_VectorOpnd *xDirectionSearchSize, VISA_VectorOpnd *yDirectionSearchSize,
                                                            VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISAVAFloodFill(bool is8Connect, VISA_RawOpnd *pixelMaskHDirection,
                                                            VISA_VectorOpnd *pixelMaskVDirectionLeft, VISA_VectorOpnd *pixelMaskVDirectionRight,
                                                            VISA_VectorOpnd *loopCount, VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISAVALBPCorrelation(VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, VISA_VectorOpnd *disparity,
                                                            VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISAVALBPCreation(VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, LBPCreationMode mode,
                                                            VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISAVAConvolve1D(VISA_StateOpndHandle *sampler, VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, CONVExecMode mode, Convovle1DDirection direction, VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISAVAConvolve1Pixel(VISA_StateOpndHandle *sampler, VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, CONV1PixelExecMode mode, VISA_RawOpnd *offsets, VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISAVAHDCConvolve(VISA_StateOpndHandle *sampler, VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, HDCReturnFormat returnFormat, CONVHDCRegionSize regionSize,
                                                            VISA_StateOpndHandle *dstSurface, VISA_VectorOpnd *xOffset, VISA_VectorOpnd *yOffset) = 0;

    CM_BUILDER_API virtual int AppendVISAVAHDCErodeDilate(EDMode subOp, VISA_StateOpndHandle *sampler, VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, VISA_StateOpndHandle *dstSurface, VISA_VectorOpnd *xOffset,
                                                            VISA_VectorOpnd *yOffset) = 0;

    CM_BUILDER_API virtual int AppendVISAVAHDCMinMaxFilter(VISA_StateOpndHandle *sampler, VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, HDCReturnFormat returnFormat, MMFEnableMode mmfMode,
                                                            VISA_StateOpndHandle *dstSurface, VISA_VectorOpnd *xOffset, VISA_VectorOpnd *yOffset) = 0;

    CM_BUILDER_API virtual int AppendVISAVAHDCLBPCorrelation(VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, VISA_VectorOpnd *disparity,
                                                            VISA_StateOpndHandle *dstSurface, VISA_VectorOpnd *xOffset, VISA_VectorOpnd *yOffset) = 0;

    CM_BUILDER_API virtual int AppendVISAVAHDCLBPCreation(VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, LBPCreationMode mode,
                                                            VISA_StateOpndHandle *dstSurface, VISA_VectorOpnd *xOffset, VISA_VectorOpnd *yOffset) = 0;

    CM_BUILDER_API virtual int AppendVISAVAHDCConvolve1D(VISA_StateOpndHandle *sampler, VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, HDCReturnFormat returnFormat, Convovle1DDirection direction,
                                                            VISA_StateOpndHandle *dstSurface, VISA_VectorOpnd *xOffset, VISA_VectorOpnd *yOffset) = 0;

    CM_BUILDER_API virtual int AppendVISAVAHDCConvolve1Pixel(VISA_StateOpndHandle *sampler, VISA_StateOpndHandle *surface, VISA_VectorOpnd * uOffset,
                                                            VISA_VectorOpnd *vOffset, HDCReturnFormat returnFormat, VISA_RawOpnd *offsets,
                                                            VISA_StateOpndHandle *dstSurface, VISA_VectorOpnd *xOffset, VISA_VectorOpnd *yOffset) = 0;

    /********** APPEND MEDIA Instructions END ******************/

    /********** APPEND 3D Instructions START ******************/

    /// AppendVISA3dSampler -- create a vISA sampler message
    /// @subOpcode the sampler op (sample, sample_c, sample_d, etc.)
    /// @pixelNullMask whether an extra GRF is returned for the null mask bit
    /// @cpsEnable whether CPS LOD compensation is enabled
    /// @uniformSampler whether the sampler state is uniform across all work items
    /// @pred predicate variable
    /// @emask execution mask
    /// @executionSize (8, 16)
    /// @channelMask the RGBA channel mask for the return value
    /// @aoffimmi Dx10 _aoffimmi modifier. [0:3] - R offset, [4:7] - V offset, [8:11] - U offset
    /// @sampler sampler index
    /// @surface surface index
    /// @dst return value.  Type may be either 32-bit or 16-bit
    /// @numMsgSepcificOpnds.  Number of payload arguments
    /// @opndArray.  payload arguments.  All operands must have the same type, which is either F or HF
    CM_BUILDER_API virtual int AppendVISA3dSampler(VISASampler3DSubOpCode subOpcode,
                                                   bool pixelNullMask,
                                                   bool cpsEnable,
                                                   bool uniformSampler,
                                                   VISA_PredOpnd *pred,
                                                   Common_VISA_EMask_Ctrl emask,
                                                   Common_ISA_Exec_Size executionSize,
                                                   VISAChannelMask channelMask,
                                                   VISA_VectorOpnd* aoffimmi,
                                                   VISA_StateOpndHandle *sampler,
                                                   VISA_StateOpndHandle *surface,
                                                   VISA_RawOpnd *dst,
                                                   int numMsgSpecificOpnds,
                                                   VISA_RawOpnd **opndArray) = 0;

    /// AppendVISA3dLoad -- create a vISA sampler load message
    /// @subOpcode the sampler op (ld, ld_lz, etc.)
    /// @pixelNullMask whether an extra GRF is returned for the null mask bit
    /// @pred predicate variable
    /// @emask execution mask
    /// @executionSize (8, 16)
    /// @channelMask the RGBA channel mask for the return value
    /// @aoffimmi Dx10 _aoffimmi modifier. [0:3] - R offset, [4:7] - V offset, [8:11] - U offset
    /// @surface surface index
    /// @dst return value.  Type may be either 32-bit or 16-bit
    /// @numMsgSepcificOpnds.  Number of payload arguments
    /// @opndArray.  payload arguments.  All operands must have the type UD
    CM_BUILDER_API virtual int AppendVISA3dLoad(VISASampler3DSubOpCode subOpcode,
                                                bool pixelNullMask,
                                                VISA_PredOpnd *pred,
                                                Common_VISA_EMask_Ctrl emask,
                                                Common_ISA_Exec_Size executionSize,
                                                VISAChannelMask channelMask,
                                                VISA_VectorOpnd* aoffimmi,
                                                VISA_StateOpndHandle *surface,
                                                VISA_RawOpnd *dst,
                                                int numMsgSpecificOpnds,
                                                VISA_RawOpnd **opndArray) = 0;

    /// AppendVISA3dGather4 -- create a vISA sampler gather4 message
    /// @subOpcode the gather4 op (gather4, gather4_c, etc.)
    /// @pixelNullMask whether an extra GRF is returned for the null mask bit
    /// @pred predicate variable
    /// @emask execution mask
    /// @executionSize (8, 16)
    /// @srcChannel one of {R,G,B,A} channel to be returned
    /// @aoffimmi Dx10 _aoffimmi modifier. [0:3] - R offset, [4:7] - V offset, [8:11] - U offset
    /// @sampler sampler index
    /// @surface surface index
    /// @dst return value.  Type may be either 32-bit or 16-bit
    /// @numMsgSepcificOpnds.  Number of payload arguments
    /// @opndArray.  payload arguments.  All operands must have the same type, which is either F or HF
    CM_BUILDER_API virtual int AppendVISA3dGather4(VISASampler3DSubOpCode subOpcode,
                                                   bool pixelNullMask,
                                                   VISA_PredOpnd *pred,
                                                   Common_VISA_EMask_Ctrl emask,
                                                   Common_ISA_Exec_Size executionSize,
                                                   VISASourceSingleChannel srcChannel,
                                                   VISA_VectorOpnd* aoffimmi,
                                                   VISA_StateOpndHandle *sampler,
                                                   VISA_StateOpndHandle *surface,
                                                   VISA_RawOpnd *dst,
                                                   int numMsgSpecificOpnds,
                                                   VISA_RawOpnd **opndArray) = 0;

    CM_BUILDER_API virtual int AppendVISA3dInfo(VISASampler3DSubOpCode subOpcode, Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize, VISAChannelMask srcChannel, VISA_StateOpndHandle *surface, VISA_RawOpnd *lod, VISA_RawOpnd *dst) = 0;

    CM_BUILDER_API virtual int AppendVISA3dRTWrite(VISA_PredOpnd *pred, Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize, VISA_VectorOpnd* renderTargetIndex, vISA_RT_CONTROLS cntrls,
                                                    VISA_StateOpndHandle *surface, VISA_RawOpnd *r1HeaderOpnd, VISA_VectorOpnd *sampleIndex,
                                                    uint8_t numMsgSpecificOpnds, VISA_RawOpnd **opndArray) = 0;

    CM_BUILDER_API virtual int AppendVISA3dRTWriteCPS(VISA_PredOpnd *pred, Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize, VISA_VectorOpnd* renderTargetIndex, vISA_RT_CONTROLS cntrls,
                                                    VISA_StateOpndHandle *surface, VISA_RawOpnd *r1HeaderOpnd, VISA_VectorOpnd *sampleIndex,
                                                    VISA_VectorOpnd *cPSCounter, uint8_t numMsgSpecificOpnds, VISA_RawOpnd **opndArray) = 0;

    CM_BUILDER_API virtual int AppendVISA3dURBWrite(VISA_PredOpnd *pred, Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize, unsigned char numberOutputParams, VISA_RawOpnd *channelMask, unsigned short globalOffset,
                                                    VISA_RawOpnd *URBHandle, VISA_RawOpnd *perSLotOffset, VISA_RawOpnd *vertexData) = 0;

    CM_BUILDER_API virtual int AppendVISA3dTypedAtomic(
        CMAtomicOperations subOp, bool is16Bit, VISA_PredOpnd *pred,
        Common_VISA_EMask_Ctrl emask, Common_ISA_Exec_Size executionSize,
        VISA_StateOpndHandle *surface, VISA_RawOpnd *u, VISA_RawOpnd *v,
        VISA_RawOpnd *r, VISA_RawOpnd *lod, VISA_RawOpnd *src0,
        VISA_RawOpnd *src1, VISA_RawOpnd *dst) = 0;

    /********** APPEND 3D Instructions END ******************/

    /********** APPEND INSTRUCTION APIS END   ******************/

    /// GetGenxBinary -- returns the GEN binary in <buffer> and update its size in <size>
    /// This function may only be called after Compile() is called
    /// If finalization fails, buffer will be set to NULL and size will be set to 0
    /// buffer must be de-allocated using freeBLock API.
    CM_BUILDER_API virtual int GetGenxBinary(void *&buffer, int &size) = 0;

    /// GetJitInfo -- returns auxiliary information collected during finalization
    /// This function may only be called after Compile() is called
    /// vISA Builder is responsible for managing this memory.
    /// it will be freed when vISA builder is destroyed.
    CM_BUILDER_API virtual int GetJitInfo(FINALIZER_INFO *&jitInfo) = 0;

    /// GetErrorMessage -- returns the error message during finalization
    CM_BUILDER_API virtual int GetErrorMessage(const char *&errorMsg) = 0;

    /// GetGenxDebugInfo -- returns the GEN debug info binary in <buffer>
    /// and its size in <size>.
    /// This function may only be called after Compile() is called
    /// If finalization fails, buffer will be set to NULL and size will be set to 0
    /// buffer must be de-allocated using freeBLock API.
    /// VISAMap contains VISA->GenISA mapping which also needs to be freed using
    /// freeBlock API. numEntries determines entries populated in VISAMap.
    CM_BUILDER_API virtual int GetGenxDebugInfo(void *&buffer, unsigned int &size, void*& VISAMap, unsigned int& numEntries) = 0;

    /// GetGenRelocInfo -- returns relocation information entries so caller
    /// can patch required immediate offsets at indicated offsets.
    CM_BUILDER_API virtual int GetGenReloc(BasicRelocEntry *&buffer, unsigned int &size) = 0;

    /// GetFreeGRFInfo -- returns free GRF information for gtpin
    /// This requires reRA pass to be executed, otherwise it returs nullptr
    CM_BUILDER_API virtual int GetFreeGRFInfo(void *& buffer, unsigned int& size) = 0;

    ///Gets declaration id GenVar
    CM_BUILDER_API virtual int getDeclarationID(VISA_GenVar *decl) = 0;

    ///Gets declaration id VISA_AddrVar
    CM_BUILDER_API virtual int getDeclarationID(VISA_AddrVar *decl) = 0;

    ///Gets declaration id VISA_PredVar
    CM_BUILDER_API virtual int getDeclarationID(VISA_PredVar *decl) = 0;

    ///Gets declaration id VISA_SamplerVar
    CM_BUILDER_API virtual int getDeclarationID(VISA_SamplerVar *decl) = 0;

    ///Gets declaration id VISA_SurfaceVar
    CM_BUILDER_API virtual int getDeclarationID(VISA_SurfaceVar *decl) = 0;

    ///Gets declaration id VISA_VMEVar
    CM_BUILDER_API virtual int getDeclarationID(VISA_VMEVar *decl) = 0;

    ///Gets declaration id VISA_LabelVar
    CM_BUILDER_API virtual int getDeclarationID(VISA_LabelVar *decl) = 0;

    ///Gets declaration id VISA_FileVar
    CM_BUILDER_API virtual int getDeclarationID(VISA_FileVar *decl) = 0;

    ///Gets visa instruction counter value
    CM_BUILDER_API virtual unsigned getvIsaInstCount() = 0;
};

class VISAFunction : public VISAKernel
{
public:
    /// SetFunctionInputSize -- Set only for VISAFunction objects.
    /// This value should be set to size of arg pre-defined register.
    CM_BUILDER_API virtual int SetFunctionInputSize(unsigned int size) = 0;

    /// SetFunctionReturnSize -- Set only for VISAFunction objects.
    /// This value should be set to size of ret pre-defined register.
    CM_BUILDER_API virtual int SetFunctionReturnSize(unsigned int size) = 0;

    /// GetFunctionId -- Get function id for a stack call function VISAFunction instance.
    /// This id is used by API client for invoking correct stack function using fcall.
    CM_BUILDER_API virtual int GetFunctionId(unsigned int& id) = 0;
};

typedef enum
{
	CM_CISA_BUILDER_CISA,
	CM_CISA_BUILDER_GEN,
	CM_CISA_BUILDER_BOTH
}CM_VISA_BUILDER_OPTION;

class VISABuilder
{
public:
	CM_BUILDER_API virtual int AddKernel(VISAKernel *& kernel, const char* kernelName) = 0;
    CM_BUILDER_API virtual int AddFunction(VISAFunction *& function, const char* functionName) = 0;
	CM_BUILDER_API virtual int Compile(const char * isaFileNameint) = 0;
    /// This function enables creation of internal low level IR bypassing creation of vISA IR.
    /// It MUST be enabled before AddKernel or AddFunction.
    /// Through this PATH no persistant vISA is created. All operands live ranges end at the Append*Inst API call.
    /// When Compile function is invoked only valid option is: CM_CISA_BUILDER_GEN
    CM_BUILDER_API virtual int CreateVISAFileVar(VISA_FileVar *& decl, char *name, unsigned int numElements, VISA_Type dataType,
                                            VISA_Align varAlign) = 0;

    CM_BUILDER_API virtual void SetOption(vISAOptions option, bool val) = 0;
    CM_BUILDER_API virtual void SetOption(vISAOptions option, uint32_t val) = 0;
};
#endif
