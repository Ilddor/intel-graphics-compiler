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

#ifndef _BUILDCISAIR_H_
#define _BUILDCISAIR_H_

namespace vISA
{
    class Mem_Manager;
}
class CisaKernel;
class CisaBinary;
class VISAKernelImpl;
class VISAFunction;

#ifndef DLL_MODE
extern FILE *CISAin;
extern FILE *CISAout;
extern int CISAdebug;
#endif

#include "VISABuilderAPIDefinition.h"
#include "visa_wa.h"

//#define TIME_vISA_LOADING
//#define TIME_IR_CONSTRUCTION
class Options;

class NativeRelocs
{
public:
    void *operator new(size_t sz, vISA::Mem_Manager& m){ return m.alloc(sz); }
    void addEntry(uint64_t offset, uint64_t info, int64_t addend, unsigned int nativeOffset);
    unsigned int getNativeOffset(unsigned int cisaOffset);
    bool isOffsetReloc(uint64_t offset, SuperRelocEntry& info);

    unsigned getNumEntries() { return (uint32_t) entries.size(); }
    SuperRelocEntry getEntry(unsigned int idx)
    {
        return entries[idx];
    }
private:
    std::vector<SuperRelocEntry> entries;
};

class CISA_IR_Builder : public VISABuilder
{
public:

	CISA_IR_Builder(CM_VISA_BUILDER_OPTION buildOption, int majorVersion, int minorVersion, PVISA_WA_TABLE pWaTable) : m_mem(4096)
    {
        mBuildOption = buildOption;
        m_executionSatarted = false;
        m_kernel_count = 0;
        m_function_count = 0;
        m_majorVersion = majorVersion;
        m_minorVersion = minorVersion;
        m_cisaBinary = new (m_mem) CisaFramework::CisaBinary(&m_options);
        m_currentKernel = NULL;
        m_pWaTable = pWaTable;
        nativeRelocs = NULL;
    }

	virtual ~CISA_IR_Builder();

    #ifndef DLL_MODE
	//
	// routines for initializing and ending CISA parser
	//
	// to make the tool quit when there is incorrect input file
	bool openCISAParsingFile(const char* fileName, char* mode)
	{
		if( (CISAin = fopen(fileName, mode)) == NULL)
		{
			fprintf(stderr,"Cannot open file %s\n", fileName);
			return false;
		}
		return true;
	}

    void closeCISAParsingFile() {fclose(CISAin);}

    #endif
    /**************START VISA BUILDER API*****************************/

    static int CreateBuilder(CISA_IR_Builder *&builder,
		vISABuilderMode mode,
		CM_VISA_BUILDER_OPTION buildOption,
		TARGET_PLATFORM platform,
		int numArgs,
		const char* flags[],
		PVISA_WA_TABLE pWaTable,
		bool initializeWA = false);
    static int DestroyBuilder(CISA_IR_Builder *builder);
    CM_BUILDER_API virtual int AddKernel(VISAKernel *& kernel, const char* kernelName);
    CM_BUILDER_API virtual int AddFunction(VISAFunction *& function, const char* functionName);
    CM_BUILDER_API virtual int Compile(const char * isaFileNameint);
    CM_BUILDER_API virtual int CreateVISAFileVar(VISA_FileVar *& decl, char *name, unsigned int numElements, VISA_Type dataType,
                                            VISA_Align varAlign);

    CM_BUILDER_API void SetOption(vISAOptions option, bool val) { m_options.setOption(option, val); }
    CM_BUILDER_API void SetOption(vISAOptions option, uint32_t val) { m_options.setOption(option, val); }

    /**************END VISA BUILDER API*************************/

    string_pool_entry** branch_targets;

    VISAKernelImpl *m_kernel;
    CisaFramework::CisaBinary *m_cisaBinary;
    VISAKernelImpl * get_kernel() { return m_kernel; }

	void CISA_IR_setVersion(unsigned char major_ver, unsigned char minor_ver)
	{
		m_majorVersion = major_ver;
		m_minorVersion = minor_ver;
	}

    Common_ISA_Input_Class get_input_class(Common_ISA_Var_Class var_class);

    //CISA Build Functions
    bool CISA_IR_initialization(char *kernel_name, int line_no);
    bool CISA_general_variable_decl(char * var_name,
											unsigned int var_elemts_num,
											VISA_Type data_type,
											VISA_Align var_align,
											char * var_alias_name,
											int var_alias_offset,
											int line_no,
                                            vISA::G4_Declare *dcl);
    bool CISA_general_variable_decl(char * var_name,
											unsigned int var_elemts_num,
											VISA_Type data_type,
											VISA_Align var_align,
											char * var_alias_name,
											int var_alias_offset,
                                            attr_gen_struct scope,
											int line_no);
    bool CISA_file_variable_decl(char * var_name,
											unsigned int var_elemts_num,
											VISA_Type data_type,
											VISA_Align var_align,
											int line_no,
                                            vISA::G4_Declare *dcl);
    bool CISA_file_variable_decl(char * var_name,
											unsigned int var_elemts_num,
											VISA_Type data_type,
											VISA_Align var_align,
											int line_no);
    bool CISA_addr_variable_decl(char *var_name, unsigned int var_elements, VISA_Type data_type, attr_gen_struct scope, int line_no);

    bool CISA_predicate_variable_decl(char *var_name,
                                            unsigned int var_elements,
                                            attr_gen_struct reg,
										    int line_no);
    bool CISA_create_func_decl(char * name,
                                int resolved_index,
                                int line_no);

    bool CISA_sampler_variable_decl(char *var_name, int num_elts, char* name, int line_no);

    bool CISA_surface_variable_decl(char *var_name, int num_elts, char* name, attr_gen_struct attr, int line_no);

    bool CISA_vme_variable_decl(char *var_name, int num_elts, char* name, int line_no);

    bool CISA_input_directive(char* var_name, short offset, unsigned short size, int line_no);

    bool CISA_implicit_input_directive(char * argName, char * varName, short offset, unsigned short size, int line_no);

    //bool CISA_attr_directive(char* input_name, attribute_info_t* attr);
    bool CISA_attr_directive(char* input_name, char* input_var, int line_no);
    bool CISA_attr_directiveNum(char* input_name, unsigned char input_var, int line_no);

    bool CISA_create_label(char * label_name, int line_no);
    bool CISA_function_directive(char* func_name);


    bool CISA_create_arith_instruction(VISA_opnd * cisa_pred,
											   ISA_Opcode opcode,
											   bool  sat,
                                               Common_VISA_EMask_Ctrl emask,
											   unsigned exec_size,
                                               VISA_opnd * dst_cisa,
                                               VISA_opnd * src0_cisa,
                                               VISA_opnd * src1_cisa,
                                               VISA_opnd * src2_cisa,
											   int line_no);
    bool CISA_create_arith_instruction2(VISA_opnd * cisa_pred,
											   ISA_Opcode opcode,
                                               Common_VISA_EMask_Ctrl emask,
											   unsigned exec_size,
                                               VISA_opnd * dst_cisa,
                                               VISA_opnd * src0_cisa,
                                               VISA_opnd * src1_cisa,
                                               VISA_opnd * src2_cisa,
											   int line_no);

    bool CISA_create_mov_instruction(VISA_opnd *pred,
											   ISA_Opcode opcode,
											   Common_VISA_EMask_Ctrl emask,
    										   unsigned exec_size,
 											   bool  sat,
											   VISA_opnd *dst,
											   VISA_opnd *src0,
											   int line_no);

    bool CISA_create_mov_instruction(VISA_opnd *dst,
											   char *src0,
											   int line_no);

    bool CISA_create_movs_instruction(Common_VISA_EMask_Ctrl emask,
                                             ISA_Opcode opcode,
	                                         unsigned exec_size,
										     VISA_opnd *dst,
                                             VISA_opnd *src0,
                                             int line_no);

    bool CISA_create_movs_instruction(Common_VISA_EMask_Ctrl emask,
	                                         unsigned exec_size,
										     Common_ISA_State_Opnd dstType,
                                             vISA::G4_Declare* dstDcl,
                                             vISA::G4_Operand* src,
                                             unsigned char offsetDst);


    bool CISA_create_branch_instruction(VISA_opnd *pred,
												ISA_Opcode opcode,
                                                Common_VISA_EMask_Ctrl emask,
											    unsigned exec_size,
										        char *target_label,
                                                int line_no);


    bool CISA_create_cmp_instruction(Common_ISA_Cond_Mod sub_op,
                                             ISA_Opcode opcode,
                                             Common_VISA_EMask_Ctrl emask,
											 unsigned exec_size,
											 char *name,
											 VISA_opnd *src0,
											 VISA_opnd *src1,
                                             int line_no);

       bool CISA_create_cmp_instruction(Common_ISA_Cond_Mod sub_op,
                                             ISA_Opcode opcode,
                                             Common_VISA_EMask_Ctrl emask,
											 unsigned exec_size,
											 VISA_opnd *dst,
											 VISA_opnd *src0,
											 VISA_opnd *src1,
                                             int line_no);

    bool CISA_create_media_instruction(ISA_Opcode opcode,
											   MEDIA_LD_mod media_mod,
											   int row_off,
											   int elem_off,
											   unsigned int plane_ID,
											   char * surface_name,
											   VISA_opnd *src0,
											   VISA_opnd *src1,
											   VISA_opnd *raw_dst,
                                               int line_no);


    bool CISA_Create_Ret(VISA_opnd *pred_opnd,
                                 ISA_Opcode opcode,
                                 Common_VISA_EMask_Ctrl emask,
								 unsigned int exec_size,
                                 int line_no);

    bool CISA_create_oword_instruction(ISA_Opcode opcode,
											   bool media_mod,
											   unsigned int size,
											   char *surface_name,
											   VISA_opnd *src0,
											   VISA_opnd *raw_dst_src,
											   int line_no);

    bool CISA_create_svm_block_instruction(SVMSubOpcode subopcode,
                                           unsigned     owords,
                                           bool         unaligned,
                                           VISA_opnd*   address,
                                           VISA_opnd*   srcDst,
                                           int          line_no);

    bool CISA_create_svm_scatter_instruction(VISA_opnd*   pred,
                                             SVMSubOpcode subopcode,
                                             Common_VISA_EMask_Ctrl emask,
                                             unsigned     exec_size,
                                             unsigned     blockSize,
                                             unsigned     numBlocks,
                                             VISA_opnd*   addresses,
                                             VISA_opnd*   srcDst,
                                             int          line_no);

    bool CISA_create_svm_atomic_instruction(VISA_opnd* pred,
                                            Common_VISA_EMask_Ctrl emask,
                                            unsigned   exec_size,
                                            CMAtomicOperations op,
                                            bool is16Bit,
                                            VISA_opnd* addresses,
                                            VISA_opnd* src0,
                                            VISA_opnd* src1,
                                            VISA_opnd* dst,
                                            int line_no);

    bool CISA_create_svm_gather4_scaled(VISA_opnd              *pred,
                                        Common_VISA_EMask_Ctrl eMask,
                                        unsigned               execSize,
                                        ChannelMask            chMask,
                                        VISA_opnd              *address,
                                        VISA_opnd              *offsets,
                                        VISA_opnd              *src,
                                        int                    lineNum);

    bool CISA_create_svm_scatter4_scaled(VISA_opnd              *pred,
                                         Common_VISA_EMask_Ctrl eMask,
                                         unsigned               execSize,
                                         ChannelMask            chMask,
                                         VISA_opnd              *address,
                                         VISA_opnd              *offsets,
                                         VISA_opnd              *src,
                                         int                    lineNum);

    bool CISA_create_address_instruction(ISA_Opcode opcode,
                                                Common_VISA_EMask_Ctrl emask,
    										     unsigned exec_size,
											     VISA_opnd *dst,
											     VISA_opnd *src0,
											     VISA_opnd *src1,
                                                 int line_no);


    bool CISA_create_logic_instruction(VISA_opnd *pred,
											   ISA_Opcode opcode,
                                               bool sat,
                                               Common_VISA_EMask_Ctrl emask,
											   unsigned exec_size,
											   VISA_opnd *dst,
											   VISA_opnd *src0,
											   VISA_opnd *src1,
                                               VISA_opnd *src2,
                                               VISA_opnd *src3,
                                               int line_no);

    bool CISA_create_logic_instruction(ISA_Opcode opcode,
                                               Common_VISA_EMask_Ctrl emask,
											   unsigned exec_size,
											   char *dst,
											   char *src0,
											   char *src1,
                                               int line_no);

    bool CISA_create_math_instruction(VISA_opnd *pred,
											   ISA_Opcode opcode,
											   bool  sat,
                                               Common_VISA_EMask_Ctrl emask,
											   unsigned exec_size,
											   VISA_opnd *dst,
											   VISA_opnd *src0,
											   VISA_opnd *src1,
                                               int line_no);

    bool CISA_create_setp_instruction(ISA_Opcode opcode,
                                              Common_VISA_EMask_Ctrl emask,
    										  unsigned exec_size,
											  char *dst,
											  VISA_opnd *src0,
                                              int line_no);

    bool CISA_create_sel_instruction(ISA_Opcode opcode,
                                              bool sat,
                                              VISA_opnd *pred,
                                              Common_VISA_EMask_Ctrl emask,
    										  unsigned exec_size,
											  VISA_opnd *dst,
											  VISA_opnd *src0,
                                              VISA_opnd *src1,
                                              int line_no);

    bool CISA_create_fminmax_instruction(bool minmax,
                                              ISA_Opcode opcode,
                                              bool sat,
                                              VISA_opnd *pred,
                                              Common_VISA_EMask_Ctrl emask,
    										  unsigned exec_size,
											  VISA_opnd *dst,
											  VISA_opnd *src0,
                                              VISA_opnd *src1,
                                              int line_no);

    bool CISA_create_scatter_instruction(ISA_Opcode opcode,
											     int elemNum,
                                                 Common_VISA_EMask_Ctrl emask,
    											 unsigned elt_size,
                                                 bool modifier,
											     char *surface_name,
											     VISA_opnd *global_offset, //global_offset
											     VISA_opnd *element_offset, //element_offset
											     VISA_opnd *raw_dst_src, //dst/src
											     int line_no);

    bool CISA_create_scatter4_instruction(ISA_Opcode opcode,
                                                 ChannelMask ch_mask,
   										         bool mod,
                                                 Common_VISA_EMask_Ctrl emask,
											     int elemNum,
											     char *surf_name,
											     VISA_opnd *g_off_opnd, //global_offset
											     VISA_opnd *offset_raw, //element_offset
											     VISA_opnd *raw_dst_src, //dst/src
											     int line_no);

    bool CISA_create_scatter4_typed_instruction(ISA_Opcode opcode,
                                                        VISA_opnd *pred,
                                                        ChannelMask ch_mask,
                                                        Common_VISA_EMask_Ctrl emask,
    										            unsigned execSize,
                                                        char* surfaceName,
                                                        VISA_opnd *uOffset,
                                                        VISA_opnd *vOffset,
                                                        VISA_opnd *rOffset,
                                                        VISA_opnd *lod,
                                                        VISA_opnd *dst,
                                                        int line_no);

    bool CISA_create_scatter4_scaled_instruction(ISA_Opcode opcode,
                                                 VISA_opnd *pred,
                                                 Common_VISA_EMask_Ctrl eMask,
                                                 unsigned execSize,
                                                 ChannelMask chMask,
                                                 char* surfaceName,
                                                 VISA_opnd *globalOffset,
                                                 VISA_opnd *offsets,
                                                 VISA_opnd *dstSrc,
                                                 int line_no);

    bool CISA_create_strbuf_scaled_instruction(ISA_Opcode opcode,
                                               VISA_opnd *pred,
                                               Common_VISA_EMask_Ctrl eMask,
                                               unsigned execSize,
                                               ChannelMask chMask,
                                               char* surfaceName,
                                               VISA_opnd *uOffsets,
                                               VISA_opnd *vOffsets,
                                               VISA_opnd *dstSrc,
                                               int line_no);

    bool CISA_create_scatter_scaled_instruction(ISA_Opcode opcode,
                                                VISA_opnd *pred,
                                                Common_VISA_EMask_Ctrl eMask,
                                                unsigned execSize,
                                                unsigned numBlocks,
                                                char* surfaceName,
                                                VISA_opnd *globalOffset,
                                                VISA_opnd *offsets,
                                                VISA_opnd *dstSrc,
                                                int lineNo);

    bool CISA_create_sync_instruction(ISA_Opcode opcode);

    bool CISA_create_pbarrier_instruction(VISA_opnd *mask, VISA_opnd *dst);

    bool CISA_create_invtri_inst(VISA_opnd *pred,
											   ISA_Opcode opcode,
											   bool  sat,
                                               Common_VISA_EMask_Ctrl emask,
											   unsigned exec_size,
											   VISA_opnd *dst,
											   VISA_opnd *src0,
                                               int line_no);

    bool CISA_create_atomic_instruction (ISA_Opcode opcode,
        CMAtomicOperations sub_op,
        bool is16Bit,
        Common_VISA_EMask_Ctrl emask,
        unsigned execSize,
        char *surface_name,
        VISA_opnd *g_off,
        VISA_opnd *elem_opnd,
        VISA_opnd *dst,
        VISA_opnd *src0,
        VISA_opnd *src1,
        int line_no);

    bool CISA_create_dword_atomic_instruction(VISA_opnd *pred,
                                              CMAtomicOperations subOpc,
                                              bool is16Bit,
                                              Common_VISA_EMask_Ctrl eMask,
                                              unsigned execSize,
                                              char *surfaceName,
                                              VISA_opnd *offsets,
                                              VISA_opnd *src0,
                                              VISA_opnd *src1,
                                              VISA_opnd *dst,
                                              int lineNo);

    bool CISA_create_typed_atomic_instruction(VISA_opnd *pred,
        CMAtomicOperations subOpc,
        bool is16Bit,
        Common_VISA_EMask_Ctrl eMask,
        unsigned execSize,
        char *surfaceName,
        VISA_opnd *u,
        VISA_opnd *v,
        VISA_opnd *r,
        VISA_opnd *lod,
        VISA_opnd *src0,
        VISA_opnd *src1,
        VISA_opnd *dst,
        int lineNo);

    bool CISA_create_SIMD_CF_instruction(VISA_opnd *pred,
												   ISA_Opcode opcode,
                                                   Common_VISA_EMask_Ctrl emask,
											       unsigned exec_size,
                                                   int line_no);

    bool CISA_create_urb_write_3d_instruction(VISA_opnd* pred,
        Common_VISA_EMask_Ctrl emask,
        unsigned exec_size,
        unsigned int num_out,
        unsigned int global_offset,
        VISA_opnd* channel_mask,
        VISA_opnd* urb_handle,
        VISA_opnd* per_slot_offset,
        VISA_opnd* vertex_data,
        int line_no);

    bool CISA_create_rtwrite_3d_instruction(VISA_opnd* pred,
                                                        char* mode,
                                                        Common_VISA_EMask_Ctrl emask,
                                                        unsigned exec_size,
                                                        char* surface_name,
                                                        const std::vector<VISA_opnd*>& operands,
                                                        int line_no);

    bool CISA_create_info_3d_instruction(VISASampler3DSubOpCode subOpcode,
                                                    Common_VISA_EMask_Ctrl emask,
                                                    unsigned exec_size,
                                                    ChannelMask channel,
                                                    char* surface_name,
                                                    VISA_opnd* lod,
                                                    VISA_opnd* dst,
                                                    int line_no);

    bool createSample4Instruction(VISA_opnd* pred,
        VISASampler3DSubOpCode subOpcode,
        bool pixelNullMask,
        ChannelMask channels,
        Common_VISA_EMask_Ctrl emask,
        unsigned exec_size,
        VISA_opnd* aoffimmi,
        char* sampler_name,
        char* surface_name,
        VISA_opnd* dst,
        unsigned int numParameters,
        VISA_RawOpnd** params,
        int line_no);

    bool create3DLoadInstruction(VISA_opnd* pred,
        VISASampler3DSubOpCode subOpcode,
        bool pixelNullMask,
        ChannelMask channels,
        Common_VISA_EMask_Ctrl emask,
        unsigned exec_size,
        VISA_opnd* aoffimmi,
        char* surface_name,
        VISA_opnd* dst,
        unsigned int numParameters,
        VISA_RawOpnd** params,
        int line_no);

    bool create3DSampleInstruction(VISA_opnd* pred,
        VISASampler3DSubOpCode subOpcode,
        bool pixelNullMask,
        bool cpsEnable,
        bool uniformSampler,
        ChannelMask channels,
        Common_VISA_EMask_Ctrl emask,
        unsigned exec_size,
        VISA_opnd* aoffimmi,
        char* sampler_name,
        char* surface_name,
        VISA_opnd* dst,
        unsigned int numParameters,
        VISA_RawOpnd** params,
        int line_no);

    bool CISA_create_sample_instruction (ISA_Opcode opcode,
	                                             ChannelMask channel,
												 int simd_mode,
												 char* sampler_name,
												 char* surface_name,
											     VISA_opnd *u_opnd,
											     VISA_opnd *v_opnd,
											     VISA_opnd *r_opnd,
											     VISA_opnd *dst,
											     int line_no);

    bool CISA_create_avs_instruction(ChannelMask channel,
                                     char* surface_name,
                                     char* sampler_name,
                                     VISA_opnd *u_offset,
                                     VISA_opnd *v_offset,
                                     VISA_opnd *deltaU,
                                     VISA_opnd *deltaV,
                                     VISA_opnd *u2d,
                                     VISA_opnd *groupID,
                                     VISA_opnd *verticalBlockNumber,
                                     OutputFormatControl cntrl,
                                     VISA_opnd *v2d,
                                     AVSExecMode execMode,
                                     VISA_opnd *iefbypass,
                                     VISA_opnd *dst,
                                     int line_no);

    bool CISA_create_sampleunorm_instruction (ISA_Opcode opcode,
                                                      ChannelMask channel,
                                                      CHANNEL_OUTPUT_FORMAT out,
												      char* sampler_dcl,
													  char* surface_dcl,
											          VISA_opnd *src0,
											          VISA_opnd *src1,
											          VISA_opnd *src2,
											          VISA_opnd *src3,
											          VISA_opnd *dst,
											          int line_no);

    bool CISA_create_vme_ime_instruction (ISA_Opcode opcode,
                                              unsigned char stream_mode,
                                              unsigned char searchCtrl,
                                              VISA_opnd *input_opnd,
                                              VISA_opnd *ime_input_opnd,
											  char* surface_name,
											  VISA_opnd *ref0_opnd,
											  VISA_opnd *ref1_opnd,
											  VISA_opnd *costCenter_opnd,
											  VISA_opnd *dst_opnd,
                                              int line_no);

    bool CISA_create_vme_sic_instruction (ISA_Opcode opcode,
                                              VISA_opnd *input_opnd,
                                              VISA_opnd *sic_input_opnd,
											  char* surface_name,
											  VISA_opnd *dst,
                                              int line_no);

    bool CISA_create_vme_fbr_instruction (ISA_Opcode opcode,
                                              VISA_opnd *input_opnd,
                                              VISA_opnd *fbr_input_opnd,
                                              char* surface_name,
                                              VISA_opnd* fbrMbMode,
                                              VISA_opnd* fbrSubMbShape,
                                              VISA_opnd* fbrSubPredMode,
											  VISA_opnd *dst,
                                              int line_no);

    bool CISA_create_switch_instruction(ISA_Opcode opcode,
                                                unsigned exec_size,
                                                VISA_opnd *indexOpnd,
                                                int numLabels,
                                                char ** labels,
                                                int line_no);


    bool CISA_create_fcall_instruction(VISA_opnd *pred_opnd,
												ISA_Opcode opcode,
                                                Common_VISA_EMask_Ctrl emask,
											    unsigned exec_size,
                                                unsigned func_id,
                                                unsigned arg_size,
										        unsigned return_size,
                                                int line_no);

    bool CISA_create_raw_send_instruction(ISA_Opcode opcode,
                                            unsigned char modifier,
                                            Common_VISA_EMask_Ctrl emask,
										    unsigned exec_size,
                                            VISA_opnd *pred,
                                            unsigned int exMsgDesc,
                                            unsigned char srcSize,
                                            unsigned char dstSize,
                                            VISA_opnd *Desc,
                                            VISA_opnd *src,
                                            VISA_opnd *dst,
                                            int line_no);
    bool CISA_create_raw_sends_instruction(ISA_Opcode opcode,
                                            unsigned char modifier,
                                            Common_VISA_EMask_Ctrl emask,
										    unsigned exec_size,
                                            VISA_opnd *pred,
                                            VISA_opnd *exMsgDesc,
                                            unsigned char ffid,
                                            unsigned char src0Size,
                                            unsigned char src1Size,
                                            unsigned char dstSize,
                                            VISA_opnd *Desc,
                                            VISA_opnd *src0,
                                            VISA_opnd *src1,
                                            VISA_opnd *dst,
                                            int line_no);
    bool CISA_create_fence_instruction(ISA_Opcode opcode, unsigned char mode);
    bool CISA_create_wait_instruction(VISA_opnd* mask);
    bool CISA_create_yield_instruction(ISA_Opcode opcode);

    bool CISA_create_lifetime_inst(unsigned char startOrEnd,
                                   //VISA_opnd *src,
                                   char* src,
                                   int line_no);

    bool CISA_create_FILE_instruction(ISA_Opcode opcode, char * file_name);
    bool CISA_create_LOC_instruction(ISA_Opcode opcode, unsigned int loc);
    bool CISA_create_NO_OPND_instruction(ISA_Opcode opcode);

    void CISA_post_file_parse();

    unsigned short CISA_create_pred_id(vISA::G4_Predicate *pred);

    VISA_opnd * CISA_create_gen_src_operand(char* var_name, short v_stride, short width, short h_stride,
        unsigned char row_offset, unsigned char col_offset, VISA_Modifier mod, int line_no);
    VISA_opnd * CISA_dst_general_operand(char * var_name,
				    unsigned char roff,
				    unsigned char sroff,
					unsigned short hstride,
					int line_no);
    VISA_opnd * CISA_create_immed(uint64_t value, VISA_Type type, int line_no);
    VISA_opnd * CISA_create_float_immed(double value, VISA_Type type, int line_no);
    CISA_GEN_VAR * CISA_find_decl(char * var_name);
    VISA_opnd * CISA_set_address_operand(CISA_GEN_VAR * cisa_decl, unsigned char offset, short width, bool isDst);
    VISA_opnd * CISA_set_address_expression(CISA_GEN_VAR *cisa_decl, short offset);
    VISA_opnd * CISA_create_indirect(CISA_GEN_VAR * cisa_decl,VISA_Modifier mod, unsigned short row_offset,
                                                      unsigned char col_offset, unsigned short immedOffset,
                                                      unsigned short vertical_stride, unsigned short width,
                                                      unsigned short horizontal_stride, VISA_Type type);
	VISA_opnd * CISA_create_indirect_dst(CISA_GEN_VAR * cisa_decl,VISA_Modifier mod, unsigned short row_offset,
                                                      unsigned char col_offset, unsigned short immedOffset,
                                                      unsigned short horizontal_stride, VISA_Type type);
    VISA_opnd * CISA_create_state_operand(char * var_name, unsigned char offset, int line_no, bool isDst);
    VISA_opnd * CISA_create_predicate_operand(char * var_name, VISA_Modifier mod, VISA_PREDICATE_STATE state, char * pred_cntrl, int line_no);
    VISA_opnd * CISA_create_RAW_NULL_operand(int line_no);
    VISA_opnd * CISA_create_RAW_operand(char * var_name, unsigned short offset, int line_no);

    unsigned short get_hash_key(const char* str);
    string_pool_entry** new_string_pool();
    string_pool_entry * string_pool_lookup(string_pool_entry **spool, const char *str);
    bool string_pool_lookup_and_insert(string_pool_entry **spool,
											   char *str,
											   Common_ISA_Var_Class type,
											   VISA_Type data_type);

    VISAKernelImpl* getCurrentKernel() const { return m_currentKernel; }
    std::list<VISAKernelImpl*>& getKernels() { return m_kernels; }

    void InitVisaWaTable(TARGET_PLATFORM platform, Stepping step);

    void setTestName(std::string name) { testName = name; }

    Options m_options;

    void setupNativeRelocs(unsigned int, const BasicRelocEntry*);
    NativeRelocs* getNativeRelocs(bool createIfNULL = true)
    {
        if (!nativeRelocs &&
            createIfNULL)
        {
            nativeRelocs = new (m_mem)NativeRelocs();
        }
        return nativeRelocs;
    }


private:

    vISA::Mem_Manager m_mem;
    CM_VISA_BUILDER_OPTION mBuildOption;
    bool m_executionSatarted;

	unsigned int m_kernel_count;
    unsigned int m_function_count;
    int m_majorVersion;
    int m_minorVersion;

    std::list<VISAKernelImpl *> m_kernels;
    //keeps track of functions for stitching purposes, after compilation.
    std::vector<VISAFunction *> m_functionsVector;

    std::map<std::string, CISA_GEN_VAR *> m_file_var_name_to_decl_map;
    CISA_GEN_VAR * getFileVarDeclFromName(const std::string &name);
    bool setFileVarNameDeclMap(const std::string &name, CISA_GEN_VAR * genDecl);

    // the current kernel being compiled.  It is updated in the ::compile() function
    VISAKernelImpl* m_currentKernel;

    void emitFCPatchFile();

    std::string testName;

    PVISA_WA_TABLE m_pWaTable;

    NativeRelocs* nativeRelocs;
};
extern _THREAD CISA_IR_Builder * pCisaBuilder;
#endif
