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


#ifndef _BUILDIR_H_
#define _BUILDIR_H_

#include <cstdarg>
#include <list>
#include <map>
#include <set>

#include "Gen4_IR.hpp"
#include "FlowGraph.h"
#include "visa_igc_common_header.h"
#include "Common_ISA.h"
#include "Common_ISA_util.h"
#include "RT_Jitter_Interface.h"
#include "visa_wa.h"
#include "PreDefinedVars.h"


#define MAX_DWORD_VALUE  0x7fffffff
#define MIN_DWORD_VALUE  0x80000000
#define MAX_UDWORD_VALUE 0xffffffff
#define MIN_UDWORD_VALUE 0
#define MAX_WORD_VALUE 32767
#define MIN_WORD_VALUE -32768
#define MAX_UWORD_VALUE 65535
#define MIN_UWORD_VALUE 0
#define MAX_CHAR_VALUE 127
#define MIN_CHAR_VALUE -128
#define MAX_UCHAR_VALUE 255
#define MIN_UCHAR_VALUE 0

typedef struct FCCalls
{
    // callOffset is in inst number units
    unsigned int callOffset;
    const char* calleeLabelString;
} FCCalls;

enum DeclareType
{
    Regular = 0,
    Fill = 1,
    Spill = 2,
    Tmp = 3,
    AddrSpill = 4,
    CoalescedFill = 5,
    CoalescedSpill = 6
};

namespace vISA
{
// IR_Builder class has a member of type FCPatchingInfo
// as a member. This class is expected to hold all FC
// related information.
class FCPatchingInfo
{
private:
    // Flag to tell if this instance has any fast-composite
    // type calls. Callees for such call instructions are not available
    // in same compilation unit.
    bool hasFCCalls;

    // Set to true if kernel has "Callable" attribute set in VISA
    // stream.
    bool isFCCallableKernel;

    // Set to true if kernel was compiled with /mCM_composable_kernel
    // FE flag.
    bool isFCComposableKernel;

    std::vector<FCCalls*> FCCallsToPatch;
    std::vector<unsigned int> FCReturnOffsetsToPatch;

public:
    FCPatchingInfo()
    {
        hasFCCalls = false;
        isFCCallableKernel = false;
        isFCComposableKernel = false;
    }

    void setHasFCCalls(bool hasFC) { hasFCCalls = hasFC; }
    bool getHasFCCalls() { return hasFCCalls; }
    void setIsCallableKernel(bool value) { isFCCallableKernel = value; }
    bool getIsCallableKernel() { return isFCCallableKernel; }
    void setFCComposableKernel(bool value) { isFCComposableKernel = value; }
    bool getFCComposableKernel() { return isFCComposableKernel; }
    std::vector<FCCalls*>& getFCCallsToPatch() { return FCCallsToPatch; }
    std::vector<unsigned int>& getFCReturnsToPatch() { return FCReturnOffsetsToPatch; }

    enum RegAccessType : unsigned char {
      Fully_Use = 0,
      Partial_Use = 1,
      Fully_Def = 2,
      Partial_Def = 3
    };

    enum RegAccessPipe : unsigned char {
      Pipe_ALU = 0,
      Pipe_Math = 1,
      Pipe_Send = 2
    };

    struct RegAccess {
      RegAccess *Next;    // The next access on the same GRF.
      RegAccessType Type; // 'def' or 'use' of that GRF.
      unsigned RegNo;     // GRF.
      unsigned Pipe;      // Pipe.
      // where that access is issued.
      G4_INST *Inst;        // Where that GRF is accessed.
      unsigned Offset;      // Instruction offset populated finally.
      // Token associated with that access.
      unsigned Token; // Optional token allocation associated to 'def'.

      RegAccess() :
        Next(nullptr), Type(Fully_Use), RegNo(0), Pipe(0), Inst(nullptr),
        Offset(0), Token(0) {}
    };

    // FIXME: Need to consider the pipeline ID together with GRF since
    // different pipe will use/def out-of-order. Need to synchronize all of
    // them to resolve the dependency.

    std::list<RegAccess> RegFirstAccessList;
    std::list<RegAccess> RegLastAccessList;
    // Per GRF, the first access.
    std::map<unsigned, RegAccess *> RegFirstAccessMap;
    // Per GRF, the last access.
    std::map<unsigned, RegAccess *> RegLastAccessMap;
    // Note that tokens recorded are tokens allocated but not used in last
    // access list.
    std::set<unsigned> AllocatedToken;  // Allocated token.
};
}

namespace vISA
{
//
// hash table for holding reg vars and reg region
//

class OperandHashTable
{
    Mem_Manager& mem;

    struct ImmKey
    {
        int64_t val;
        G4_Type valType;
        ImmKey(int64_t imm, G4_Type type) : val(imm), valType(type) {}
        bool operator==(const ImmKey& imm) const
        {
            return val == imm.val && valType == imm.valType;
        }
    };

    struct ImmKeyHash
    {
        std::size_t operator()(const ImmKey& imm) const
        {
            return (std::size_t) (imm.val ^ imm.valType);
        }
    };

    struct stringCompare
    {
        bool operator() (const char* s1, const char* s2) const
        {
            return strcmp(s1, s2) == 0;
        }
    };

    std::unordered_map<ImmKey, G4_Imm*, ImmKeyHash> immTable;
    std::unordered_map<const char *, G4_Label*, std::hash<const char*>, stringCompare> labelTable;

public:
    OperandHashTable(Mem_Manager& m) : mem(m)
    {
    }

    // Generic methods that work on both integer and floating-point types.
    // For floating-point types, 'imm' needs to be G4_Imm(<float-value>.getImm().
    G4_Imm*          lookupImm(int64_t imm, G4_Type ty);
    G4_Imm*          createImm(int64_t imm, G4_Type ty);

    G4_Label*        lookupLabel(const char* lab);
    G4_Label*        createLabel(const char* lab);
};

//
// place for holding all region descriptions
//
class RegionPool
{
    Mem_Manager& mem;
    std::vector<RegionDesc*> rgnlist;
public:
    RegionPool(Mem_Manager& m) : mem(m) {}
    RegionDesc* createRegion(uint16_t vstride, uint16_t width, uint16_t hstride);
};

//
// place for hbolding all .declare
//
class DeclarePool
{
    Mem_Manager& mem;
    std::vector<G4_Declare*> dcllist;
    int addrSpillLocCount; //incremented in G4_RegVarAddrSpillLoc()
public:
    DeclarePool(Mem_Manager& m) : mem(m), addrSpillLocCount(0) { dcllist.reserve(2048); }
    ~DeclarePool()
    {
        for (unsigned i = 0, size = (unsigned) dcllist.size(); i < size; i++)
        {
            G4_Declare* dcl = dcllist[i];
            dcl->~G4_Declare();
        }
        dcllist.clear();
    }
    G4_Declare* createDeclare(const char*    name,
                              G4_RegFileKind regFile,
                              unsigned short nElems,
                              unsigned short nRows,
                              G4_Type        ty,
                              DeclareType kind = Regular,
                              G4_RegVar *    base = NULL,
                              G4_Operand *   repRegion = NULL,
                              unsigned       execSize = 0)
    {
        G4_Declare* dcl = new (mem) G4_Declare(name, regFile, nElems * nRows, ty, dcllist);
        G4_RegVar * regVar;
        if (kind == DeclareType::Regular)
            regVar = new (mem) G4_RegVar(dcl, G4_RegVar::RegVarType::Default);
        else if (kind == DeclareType::AddrSpill)
            regVar = new (mem) G4_RegVarAddrSpillLoc(dcl, addrSpillLocCount);
        else if (kind == DeclareType::Tmp)
            regVar = new (mem) G4_RegVarTmp(dcl, base);
        else if (kind == DeclareType::Spill)
            regVar = new (mem) G4_RegVarTransient(dcl, base, repRegion->asDstRegRegion (), execSize, G4_RegVarTransient::TransientType::Spill);
        else if (kind == DeclareType::Fill)
            regVar = new (mem)G4_RegVarTransient(dcl, base, repRegion->asSrcRegRegion(), execSize, G4_RegVarTransient::TransientType::Fill);
        else if (kind == DeclareType::CoalescedFill || kind == DeclareType::CoalescedSpill)
            regVar = new (mem)G4_RegVarCoalesced(dcl, kind == DeclareType::CoalescedFill);
        else
        {
            MUST_BE_TRUE(false, ERROR_INTERNAL_ARGUMENT);
            regVar = NULL;
        }
        dcl->setRegVar(regVar);
        dcl->setAlign( Either );

        if (regFile == G4_ADDRESS)
        {
            dcl->setSubRegAlign(Any);
        }
        else if (regFile != G4_FLAG)
        {
            if (nElems * nRows * G4_Type_Table[ty].byteSize >= G4_GRF_REG_NBYTES)
            {
                dcl->setSubRegAlign(Sixteen_Word);
            }
            else
            {
                // at a minimum subRegAlign has to be at least the type size
                dcl->setSubRegAlign(Get_G4_SubRegAlign_From_Type(ty));
            }
        }
        else
        {
            if (dcl->getNumberFlagElements() == 32)
            {
                dcl->setSubRegAlign(Even_Word);
            }
        }

        return dcl;
    }

    G4_Declare* createPreVarDeclare(
                              PreDefinedVarsInternal index,
                              unsigned short n_elems,
                              unsigned short n_rows,
                              G4_Type        ty)
    {

        G4_Declare* dcl = new (mem)G4_Declare(getPredefinedVarString(index), G4_INPUT, n_elems * n_rows, ty, dcllist);
        G4_RegVar * regVar;
        regVar = new (mem) G4_RegVar(dcl, G4_RegVar::RegVarType::Default);
        dcl->setRegVar(regVar);

        return dcl;
    }

    std::vector<G4_Declare*>& getDeclareList() {return dcllist;}
};

//
// interface for creating operands and instructions
//
class IR_Builder {

public:

    char* curFile;
    unsigned int curLine;
    int curCISAOffset;

private:
    //allocator pools
    USE_DEF_ALLOCATOR useDefAllocator;

    int                 func_id;
    G4_INST*            last_inst;
    FINALIZER_INFO*        metaData;

    bool isKernel;
    int cunit;
    reloc_symtab* varRelocTable;
    reloc_symtab* funcRelocTable;
    const std::vector <char*>* resolvedCalleeNames;

    // pre-defined declare that binds to R0 (the entire GRF)
    // when pre-emption is enabled, builtinR0 is replaced by a temp,
    // and a move is inserted at kernel entry
    // mov (8) builtinR0 realR0
    G4_Declare* builtinR0; // this is either r0 or the temp if pre-emption is enabled
    G4_Declare* realR0; // this always refers to r0

    // pre-defined declare that binds to A0.0:ud
    G4_Declare* builtinA0;
    // pre-defined declare that binds to A0.2:ud
    G4_Declare* builtinA0Dot2; //used for splitsend's ext msg descriptor
    // pre-defind declare that binds to HWTid (R0.5:ud)
    G4_Declare* builtinHWTID;
    // pre-defined bindless surface index (252, 1 UD)
    G4_Declare* builtinT252;
    // pre-defined bindless sampler index (31, 1 UD)
    G4_Declare* builtinBindlessSampler;
    // pre-defined sampler header
    G4_Declare* builtinSamplerHeader;
    //pre-defined vector of stride 4 (16 W of [0, 4, 8, 12, ..., 60])
    //only intialized if the kernel uses SLM untyped r/w for spill
    G4_Declare* builtinImmVector4;
    // pre-defined SLM scatter spill address: SLMStart(tid) + builtinImmVector4, 16 DW
    // like builinImmVector4, only initialized if kernel uses SLM untyped r/w (ok, may need it for blocked SLM message as well)
    G4_Declare* builtinSLMSpillAddr;

    bool usesSampler;

    // function call related declares
    G4_Declare* be_sp;
    G4_Declare* be_fp;
    G4_Declare* tmpFCRet;

    unsigned short arg_size;
    unsigned short return_var_size;

    static unsigned int sampler8x8_group_id;

    // Populate this data structure so after compiling all kernels
    // in file, we can emit out patch file using this up-levelled
    // information.
    FCPatchingInfo* fcPatchInfo;

    const PVISA_WA_TABLE m_pWaTable;
    Options *m_options;

    std::map<G4_INST*, G4_FCALL*> m_fcallInfo;

    // Basic region descriptors.
    RegionDesc CanonicalRegionStride0, // <0; 1, 0>
               CanonicalRegionStride1, // <1; 1, 0>
               CanonicalRegionStride2, // <2; 1, 0>
               CanonicalRegionStride4; // <4; 1, 0>

    class PreDefinedVars
    {
    public:
        PreDefinedVars()
        {
            memset(hasPredefined,  0, sizeof(hasPredefined));
            memset(predefinedVars, 0, sizeof(predefinedVars));
        }
        void setHasPredefined(PreDefinedVarsInternal id, bool val)
        {
            hasPredefined[static_cast<int>(id)] = val;
        }

        bool isHasPredefined(PreDefinedVarsInternal id) const
        {
            return hasPredefined[static_cast<int>(id)];
        }

        void setPredefinedVar(PreDefinedVarsInternal id, G4_Declare *dcl)
        {
            predefinedVars[static_cast<int>(id)] = dcl;
        }

        G4_Declare* getPreDefinedVar(PreDefinedVarsInternal id) const
        {
            if (id >= PreDefinedVarsInternal::VAR_LAST)
            {
                return nullptr;
            }
            return predefinedVars[static_cast<int>(id)];
        }
    private:
        // records whether a vISA pre-defined var is used by the kernel
        // some predefined need to be expanded (e.g., HWTid, color)
        bool hasPredefined[static_cast<int>(PreDefinedVarsInternal::VAR_LAST)];
        G4_Declare* predefinedVars[static_cast<int>(PreDefinedVarsInternal::VAR_LAST)];
    };

    bool use64BitFEStackVars;
    bool hasNullReturnSampler = false;

public:
    PreDefinedVars preDefVars;
    Mem_Manager&        mem;        // memory for all operands and insts
    PhyRegPool&         phyregpool; // all physical regs
    OperandHashTable    hashtable;  // all created region operands
    RegionPool          rgnpool;    // all region description
    DeclarePool         dclpool;    // all created decalres
    INST_LIST instList;   // all created insts
    // list of instructions ever allocated
    // This list may only grow and is freed when IR_Builder is destroyed
    std::vector<G4_INST*> instAllocList;
    G4_Kernel&          kernel;
    // the following fileds are used for dcl name when a new dcl is created.
    // number of predefined variables are included.
    int                 num_general_dcl;
    unsigned            num_temp_dcl;
    // number of temp GRF vars created to hold spilled addr/flag  
    uint32_t            numAddrFlagSpillLoc = 0;
    std::vector<input_info_t*> m_inputVect;

    const Options* getOptions() const { return m_options; }
    bool getOption(vISAOptions opt) const {return m_options->getOption(opt); }
    void getOption(vISAOptions opt, const char *&str) const {return m_options->getOption(opt, str); }
    void addInputArg(input_info_t * inpt);
    input_info_t * getInputArg(unsigned int index);
    unsigned int getInputCount();
    input_info_t *getRetIPArg();

    std::list <int> callees;

    enum SubRegs_SP_FP
    {
        FE_SP = 0, // Can be either :ud or :uq
        FE_FP = 1, // Can be either :ud or :uq
        BE_SP = 6, // :ud
        BE_FP = 7 // :ud
    };

    // Getter/setter for be_sp and be_fp
    G4_Declare* getBESP()
    {
        if( be_sp == NULL )
        {
            be_sp = createDeclareNoLookup("be_sp", G4_GRF, 1, 1, Type_UD);
            be_sp->getRegVar()->setPhyReg(phyregpool.getGreg(kernel.getStackCallStartReg()), SubRegs_SP_FP::BE_SP);
        }

        return be_sp;
    }

    G4_Declare* getBEFP()
    {
        if( be_fp == NULL )
        {
            be_fp = createDeclareNoLookup("be_fp", G4_GRF, 1, 1, Type_UD );
            be_fp->getRegVar()->setPhyReg(phyregpool.getGreg(kernel.getStackCallStartReg()), SubRegs_SP_FP::BE_FP);
        }

        return be_fp;
    }

    G4_Declare* getStackCallArg() const
    {
        return preDefVars.getPreDefinedVar(PreDefinedVarsInternal::ARG);
    }
    G4_Declare* getStackCallRet() const
    {
        return preDefVars.getPreDefinedVar(PreDefinedVarsInternal::RET);
    }

    bool isOpndAligned( G4_Operand *opnd, unsigned short &offset, int align_byte );

    // check if opnd is or can be made "alignByte"-byte aligned. This function will change the underlying
    // variable's alignment (e.g., make a scalar variable GRF-aligned) when possible to satisfy 
    // the alignment
    bool isOpndAligned(G4_Operand* opnd, int alignByte)
    {
        uint16_t offset = 0; // ignored
        return isOpndAligned(opnd, offset, alignByte);
    }

    void setFuncId( int id ) { func_id = id; }
    int getFuncId() { return func_id; }
    void setCUnitId( int id ) { cunit = id; }
    int getCUnitId() { return cunit; }
    void setIsKernel( bool value ) { isKernel = value; }
    bool getIsKernel() { return isKernel; }
    void setVarRelocTable( reloc_symtab* tab ) { varRelocTable = tab; }
    reloc_symtab* getVarRelocTable() { return varRelocTable; }
    void setFuncRelocTable( reloc_symtab* tab ) { funcRelocTable = tab; }
    reloc_symtab* getFuncRelocTable() { return funcRelocTable; }
    void setResolvedCalleeNames(std::vector<char*>* nameList) { resolvedCalleeNames = nameList; }
    const std::vector<char*>* getResolvedCalleeNames() { return resolvedCalleeNames; }
    void predefinedVarRegAssignment(uint8_t inputSize);
    void expandPredefinedVars();
    void setArgSize( unsigned short size ) { arg_size = size; }
    unsigned short getArgSize() { return arg_size; }
    void setRetVarSize( unsigned short size ) { return_var_size = size; }
    unsigned short getRetVarSize() { return return_var_size; }
    FCPatchingInfo* getFCPatchInfo()
    {
        // Create new instance of FC patching class if one is not
        // yet created.
        if(fcPatchInfo == NULL)
        {
            FCPatchingInfo* instance;
            instance = (FCPatchingInfo*)mem.alloc(sizeof(FCPatchingInfo));
            fcPatchInfo = new (instance) FCPatchingInfo();
        }

        return fcPatchInfo;
    }

    void setFCPatchInfo(FCPatchingInfo* instance) { fcPatchInfo = instance; }

    PVISA_WA_TABLE getPWaTable() { return m_pWaTable; }

    char* getNameString(Mem_Manager& mem, size_t size, const char* format, ...)
    {
#ifdef _DEBUG
        char* name = (char*) mem.alloc(size);
        va_list args;
        va_start(args, format);
        std::vsnprintf(name, size, format, args);
        va_end(args);
        return name;
#else
        const char* name = "";
        return const_cast<char*>(name);
#endif
    }


    G4_FCALL* getFcallInfo(G4_INST* inst) {
        std::map<G4_INST *, G4_FCALL *>::iterator it;
        it = m_fcallInfo.find(inst);
        if (m_fcallInfo.end() == it)
        {
            return NULL;
        }
        else
        {
            return it->second;
        }
    }

    bool getHasNullReturnSampler() const { return hasNullReturnSampler; }

    /*
        Initializes predefined vars for all the vISA versions
    */
    void createPreDefinedVars()
    {
        for (PreDefinedVarsInternal i : allPreDefVars)
        {
            G4_Declare* dcl = nullptr;

            if (predefinedVarNeedGRF(i))
            {
                // work item id variables are handled uniformly
                G4_Type ty = Get_G4_Type_From_Common_ISA_Type(getPredefinedVarType((PreDefinedVarsInternal)i));
                dcl = createPreVar(getPredefinedVarID((PreDefinedVarsInternal)i), 1, ty, Either, Any);
            }
            else
            {
                const char* name = getPredefinedVarString(i);
                switch (i)
                {
                case PreDefinedVarsInternal::VAR_NULL:
                    dcl = createDeclareNoLookup(name, G4_GRF, 1, 1, Type_UD);
                    dcl->getRegVar()->setPhyReg(phyregpool.getNullReg(), 0);
                    break;
                case PreDefinedVarsInternal::TSC:
                {
                    G4_Declare* tscDcl = createPreVar(i, 5, Type_UD, Either, Any);
                    tscDcl->getRegVar()->setPhyReg(phyregpool.getTm0Reg(), 0);
                    dcl = tscDcl;
                    break;
                }
                case PreDefinedVarsInternal::R0:
                {
                    dcl = getBuiltinR0();
                    break;
                }
                case PreDefinedVarsInternal::SR0:
                {
                    G4_Declare* sr0Dcl = createPreVar(i, 4, Type_UD, Either, Any);
                    sr0Dcl->getRegVar()->setPhyReg(phyregpool.getSr0Reg(), 0);
                    dcl = sr0Dcl;
                    break;
                }
                case PreDefinedVarsInternal::CR0:
                {
                    G4_Declare* cr0Dcl = createPreVar(i, 3, Type_UD, Either, Any);
                    cr0Dcl->getRegVar()->setPhyReg(phyregpool.getCr0Reg(), 0);
                    dcl = cr0Dcl;
                    break;
                }
                case PreDefinedVarsInternal::CE0:
                {
                    G4_Declare* ce0Dcl = createPreVar(i, 1, Type_UD, Either, Any);
                    ce0Dcl->getRegVar()->setPhyReg(phyregpool.getMask0Reg(), 0);
                    dcl = ce0Dcl;
                    break;
                }
                case PreDefinedVarsInternal::DBG:
                {
                    G4_Declare* dbgDcl = createPreVar(i, 2, Type_UD, Either, Any);
                    dbgDcl->getRegVar()->setPhyReg(phyregpool.getDbgReg(), 0);
                    dcl = dbgDcl;
                    break;
                }
                case PreDefinedVarsInternal::ARG:
                {
                    dcl = createDeclareNoLookup(name, G4_INPUT, 8, 32, Type_UD);
                    dcl->getRegVar()->setPhyReg(phyregpool.getGreg(28), 0);
                    dcl->setIsPreDefArg();
                    break;
                }
                case PreDefinedVarsInternal::RET:
                {
                    dcl = createDeclareNoLookup(name, G4_GRF, 8, 12, Type_UD);
                    dcl->getRegVar()->setPhyReg(phyregpool.getGreg(16), 0);
                    dcl->setIsPreDefRet();
                    dcl->setLiveOut();
                    break;
                }
                case PreDefinedVarsInternal::FE_SP:
                {
                    unsigned int startReg = kernel.getStackCallStartReg();
                    dcl = createDeclareNoLookup(name, G4_GRF, 1, 1, is64BitFEStackVars() ? Type_UQ : Type_UD);
                    dcl->getRegVar()->setPhyReg(phyregpool.getGreg(startReg), SubRegs_SP_FP::FE_SP);
                    dcl->setIsPreDefFEStackVar();
                    break;
                }
                case PreDefinedVarsInternal::FE_FP:
                {
                    // PREDEFINED_FE_FP
                    unsigned int startReg = kernel.getStackCallStartReg();
                    dcl = createDeclareNoLookup(name, G4_GRF, 1, 1, is64BitFEStackVars() ? Type_UQ : Type_UD);
                    dcl->getRegVar()->setPhyReg(phyregpool.getGreg(startReg), SubRegs_SP_FP::FE_FP);
                    dcl->setIsPreDefFEStackVar();
                    break;
                }
                case PreDefinedVarsInternal::HW_TID:
                {
                    // PREDEFINED_HW_TID
                    dcl = getBuiltinHWTID();
                    break;
                }
                case PreDefinedVarsInternal::X:
                case PreDefinedVarsInternal::Y:
                case PreDefinedVarsInternal::COLOR:
                {
                    // these three are size 1 UW
                    dcl = createDeclareNoLookup(name, G4_GRF, 1, 1,
                        Get_G4_Type_From_Common_ISA_Type(getPredefinedVarType(i)));
                    break;
                }
                default:
                {
                    break;
                }
                }
            }
            preDefVars.setPredefinedVar(i, dcl);
        }
    }

    void createBuiltinDecls()
    {

        builtinR0 = createDeclareNoLookup(
            "BuiltinR0",
            G4_INPUT,
            GENX_GRF_REG_SIZ / G4_Type_Table[Type_UD].byteSize,
            1,
            Type_UD);
        builtinR0->getRegVar()->setPhyReg(phyregpool.getGreg(0), 0);
        realR0 = builtinR0;

        builtinA0 = createDeclareNoLookup(
            "BuiltinA0",
            G4_ADDRESS,
            1,
            1,
            Type_UD);
        builtinA0->getRegVar()->setPhyReg(phyregpool.getAddrReg(), 0);
        builtinA0Dot2 = createDeclareNoLookup(
            "BuiltinA0Dot2",  //a0.2
            G4_ADDRESS,
            1,
            1,
            Type_UD);
        builtinA0Dot2->getRegVar()->setPhyReg(phyregpool.getAddrReg(), 2);

        builtinHWTID = createDeclareNoLookup("hw_tid", G4_GRF, 1, 1, Type_UD);

        builtinT252 = createDeclareNoLookup(vISAPreDefSurf[PREDEFINED_SURFACE_T252].name, G4_GRF, 1, 1, Type_UD);
        builtinBindlessSampler = createDeclareNoLookup("B_S", G4_GRF, 1, 1, Type_UD);

        builtinSamplerHeader = createDeclareNoLookup("samplerHeader", G4_GRF, 8, 1, Type_UD);

        builtinSLMSpillAddr = nullptr;
        builtinImmVector4 = nullptr;
    }

    G4_Declare* getBuiltinSLMSpillAddr() const { return builtinSLMSpillAddr; }
    G4_Declare* getBuiltinImmVector4() const { return builtinImmVector4; }

    void initBuiltinSLMSpillAddr(int perThreadSLMSize);

    IR_Builder(INST_LIST_NODE_ALLOCATOR &alloc, PhyRegPool &pregs, G4_Kernel &k,
        Mem_Manager &m, Options *options, bool isFESP64Bits,
        FINALIZER_INFO *jitInfo = NULL, PVISA_WA_TABLE pWaTable = NULL)
        : curFile(NULL), curLine(0), curCISAOffset(-1), func_id(-1), last_inst(NULL), metaData(jitInfo),
        isKernel(false), cunit(0), varRelocTable(NULL), funcRelocTable(NULL), resolvedCalleeNames(NULL),
        usesSampler(false), m_pWaTable(pWaTable), m_options(options), CanonicalRegionStride0(0, 1, 0),
        CanonicalRegionStride1(1, 1, 0), CanonicalRegionStride2(2, 1, 0), CanonicalRegionStride4(4, 1, 0),
        use64BitFEStackVars(isFESP64Bits), mem(m), phyregpool(pregs), hashtable(m), rgnpool(m), dclpool(m),
        instList(alloc), kernel(k)
    {
        num_general_dcl = 0;
        num_temp_dcl = 0;
        kernel.setBuilder(this); // kernel needs pointer to the builder
        createBuiltinDecls();

        sampler8x8_group_id = 0;

        be_sp = be_fp = tmpFCRet = nullptr;

        arg_size = 0;
        return_var_size = 0;

        if (metaData != NULL)
        {
            memset(metaData, 0, sizeof(FINALIZER_INFO));
        }

        fcPatchInfo = NULL;

        if (m_options->getOption(vISA_enablePreemption))
        {
            G4_Declare *R0CopyDcl = createTempVar(8, Type_UD, Either, Sixteen_Word);
            builtinR0 = R0CopyDcl;
        }

        createPreDefinedVars();
    }

    ~IR_Builder()
    {
        // We need to invoke the destructor of every instruction ever allocated
        // so that its members will be freed.
        // Note that we don't delete the instruction itself as it's allocated from
        // the memory manager's pool
        for (unsigned i = 0, size = (unsigned)instAllocList.size(); i != size; i++)
        {
            G4_INST* inst = instAllocList[i];
            inst->~G4_INST();
        }
        instAllocList.clear();

        if (fcPatchInfo != NULL)
        {
            fcPatchInfo->~FCPatchingInfo();
        }
    }

    G4_Declare* createDeclareNoLookup(const char*    name,
        G4_RegFileKind regFile,
        unsigned short n_elems,
        unsigned short n_rows,
        G4_Type        ty,
        DeclareType kind = Regular,
        G4_RegVar *    base = NULL,
        G4_Operand *   repRegion = NULL,
        unsigned       execSize = 0,
        bool           pushFront = false)
    {
        if (regFile == G4_FLAG)
        {
            MUST_BE_TRUE(ty == Type_UW, "flag decl must have type UW");
        }

        G4_Declare* dcl = dclpool.createDeclare(name, regFile, n_elems,
            n_rows, ty, kind, base, repRegion, execSize);
        if (!pushFront)
        {
            kernel.Declares.push_back(dcl);
        }
        else
        {
            kernel.Declares.push_front(dcl);
        }
        ++num_general_dcl;
        return dcl;
    }

    G4_Declare* createPreVarDeclareNoLookup(
        PreDefinedVarsInternal index,
        unsigned short n_elems,
        unsigned short n_rows,
        G4_Type        ty)
    {
        G4_Declare* dcl = dclpool.createPreVarDeclare(index, n_elems, n_rows, ty);
        kernel.Declares.push_back(dcl);
        return dcl;
    }

    G4_Declare* getBuiltinR0()
    {
        return builtinR0;
    }

    G4_Declare* getRealR0() const
    {
        return realR0;
    }

    G4_Declare* getBuiltinA0()
    {
        return builtinA0;
    }

    G4_Declare* getBuiltinA0Dot2()
    {
        return builtinA0Dot2;
    }

    G4_Declare* getBuiltinHWTID() const
    {
        return builtinHWTID;
    }

    G4_Declare* getBuiltinT252() const
    {
        return builtinT252;
    }

    G4_Declare* getBuiltinBindlessSampler() const
    {
        return builtinBindlessSampler;
    }

    bool isBindlessSampler(G4_Operand* sampler) const
    {
        return sampler->isSrcRegRegion() && sampler->getTopDcl() == getBuiltinBindlessSampler();
    }

    bool isBindlessSurface(G4_Operand* bti) const
    {
        return bti->isSrcRegRegion() && bti->getTopDcl() == getBuiltinT252();
    }

    FINALIZER_INFO* getJitInfo()
    {
        return metaData;
    }

    // create a new temp GRF with the specified type/size and undefined regions
    G4_Declare* createTempVar(unsigned int numElements, G4_Type type, G4_Align align, G4_SubReg_Align subAlign, const char* prefix = "TV" )
    {
        char* name = getNameString(mem, 20, "%s%d", prefix, num_temp_dcl++);

        unsigned short dcl_width = 0, dcl_height = 1;
        int totalByteSize = numElements * G4_Type_Table[type].byteSize;
        if( totalByteSize <= G4_GRF_REG_NBYTES )
        {
            dcl_width = totalByteSize / G4_Type_Table[type].byteSize;
        }
        else
        {
            // here we assume that the start point of the var is the beginning of a GRF?
            // so subregister must be 0?
            dcl_width = G4_GRF_REG_NBYTES / G4_Type_Table[type].byteSize;
            dcl_height = totalByteSize / G4_GRF_REG_NBYTES;
            if( totalByteSize % G4_GRF_REG_NBYTES != 0 )
            {
                dcl_height++;
            }
        }

        G4_Declare* dcl = createDeclareNoLookup(name, G4_GRF, dcl_width, dcl_height, type);
        dcl->setAlign( align );
        dcl->setSubRegAlign( subAlign );
        return dcl;
    }

    // create a new temp GRF as the home location of a spilled addr/flag dcl
    G4_Declare* createAddrFlagSpillLoc(G4_Declare* dcl)
    {
        char* name = getNameString(mem, 16, "SP_LOC_%d", numAddrFlagSpillLoc++);
        G4_Declare* spillLoc = createDeclareNoLookup(name,
            G4_GRF,
            dcl->getNumElems(),
            1,
            dcl->getElemType(),
            DeclareType::AddrSpill);
        dcl->setSpilledDeclare(spillLoc);
        spillLoc->setSubRegAlign(dcl->getSubRegAlign()); // for simd32 flag the spill loc has to be 2-word aligned since it's accessed as dw
        return spillLoc;
    }

    // like the above, but also mark the variable as don't spill
    // this is used for temp variables in macro sequences where spilling woul not help
    // FIXME: can we somehow merge this with G4_RegVarTmp/G4_RegVarTransient?
    G4_Declare* createTempVarWithNoSpill(unsigned int numElements, G4_Type type, G4_Align align, G4_SubReg_Align subAlign, const char* prefix = "TV")
    {
        G4_Declare* dcl = createTempVar(numElements, type, align, subAlign, prefix);
        dcl->setDoNotSpill();
        return dcl;
    }

    //
    // Create a declare that is hardwired to some phyiscal GRF.
    // It is useful to implement various workarounds post RA where we want to directly 
    // address some physical GRF.
    // regOff is in unit of the declare type.
    // caller is responsible for ensuring the resulting variable does not violate any HW restrictions
    // (e.g., operand does not cross two GRF)
    //
    G4_Declare* createHardwiredDeclare(uint32_t numElements, G4_Type type, uint32_t regNum, uint32_t regOff)
    {
        G4_Declare* dcl = createTempVar(numElements, type, Either, Any);
        unsigned int linearizedStart = (regNum * G4_GRF_REG_NBYTES) + (regOff * getTypeSize(type));
        // since it's called post RA (specifically post computePReg) we have to manually set the GRF's byte offset
        dcl->setGRFBaseOffset(linearizedStart); 
        dcl->getRegVar()->setPhyReg(phyregpool.getGreg(regNum), regOff);
        return dcl;
    }

    G4_INST* createPseudoKills(std::initializer_list<G4_Declare*> dcls)
    {
        G4_INST* inst = nullptr;
        for (auto dcl : dcls)
        {
            inst = createPseudoKill(dcl);
        }

        return inst;
    }

    G4_INST* createPseudoKill(G4_Declare* dcl)
    {
        auto dstRgn = createDstRegRegion(Direct, dcl->getRegVar(), 0, 0, 1, Type_UD);
        G4_INST* inst = createInst(nullptr, G4_pseudo_kill, nullptr, false, 1,
            dstRgn, nullptr, nullptr, InstOpt_WriteEnable);

        return inst;
    }

    /* numberOfFlags MEANS NUMBER OF WORDS (e.g., 1 means 16-bit), not number of bits or number of data elements in operands. */
    G4_Declare* createTempFlag(unsigned short numberOfFlags, const char* prefix = "TEMP_FLAG_" )
    {
        char* name = getNameString(mem, 20, "%s%d", prefix, num_temp_dcl++);

        G4_Declare* dcl = createDeclareNoLookup(name, G4_FLAG, numberOfFlags, 1, Type_UW);

        return dcl;
    }

    // like above, but pass numFlagElements instead. This allows us to distinguish between 1/8/16-bit flags,
    // which are all allocated as a UW. name is allocated by caller
    G4_Declare* createFlag(uint16_t numFlagElements, const char* name)
    {
        uint32_t numWords = (numFlagElements + 15) / 16;
        G4_Declare* dcl = createDeclareNoLookup(name, G4_FLAG, numWords, 1, Type_UW);
        dcl->setNumberFlagElements((uint8_t) numFlagElements);
        return dcl;
    }

    G4_Declare* createPreVar(PreDefinedVarsInternal preDefVar_index, unsigned short numElements, G4_Type type, G4_Align align, G4_SubReg_Align subAlign)
    {
        MUST_BE_TRUE(preDefVar_index < PreDefinedVarsInternal::VAR_LAST, "illegal predefined var index");
        unsigned short dcl_width = 0, dcl_height = 1;
        int totalByteSize = numElements * G4_Type_Table[type].byteSize;
        if( totalByteSize <= G4_GRF_REG_NBYTES )
        {
            dcl_width = totalByteSize / G4_Type_Table[type].byteSize;
        }
        else
        {
            // here we assume that the start point of the var is the beginning of a GRF?
            // so subregister must be 0?
            dcl_width = G4_GRF_REG_NBYTES / G4_Type_Table[type].byteSize;
            dcl_height = totalByteSize / G4_GRF_REG_NBYTES;
            if( totalByteSize % G4_GRF_REG_NBYTES != 0 )
            {
                dcl_height++;
            }
        }

        if( subAlign == Any )
        {
            // subAlign has to be type size at the minimum
            subAlign = Get_G4_SubRegAlign_From_Type( type );
        }

        G4_Declare* dcl = createPreVarDeclareNoLookup(preDefVar_index, dcl_width, dcl_height, type);
        dcl->setAlign( align );
        dcl->setSubRegAlign( subAlign );
        return dcl;
    }

    //
    // create <vstride; width, hstride>
    //
    // PLEASE use getRegion* interface to get regions if possible!
    // This function will be mostly used for external regions.
    RegionDesc* createRegionDesc(uint16_t vstride,
        uint16_t width,
        uint16_t hstride)
    {
        return rgnpool.createRegion(vstride, width, hstride);
    }

    // Given the execution size and region parameters, create a region
    // descriptor.
    //
    // PLEASE use getRegion* interface to get regions if possible!
    // This function will be mostly used for external regions.
    RegionDesc *createRegionDesc(uint16_t size, uint16_t vstride,
        uint16_t width, uint16_t hstride)
    {
        // Performs normalization for commonly used regions.
        switch (RegionDesc::getRegionDescKind(size, vstride, width, hstride)) {
        default:
            break;
        case RegionDesc::RK_Stride0:
            return getRegionScalar();
        case RegionDesc::RK_Stride1:
            return getRegionStride1();
        case RegionDesc::RK_Stride2:
            return getRegionStride2();
        case RegionDesc::RK_Stride4:
            return getRegionStride4();
        }
        return rgnpool.createRegion(vstride, width, hstride);
    }

    /// Helper to normalize an existing region descriptor.
    RegionDesc *getNormalizedRegion(uint16_t size, RegionDesc *rd)
    {
        return createRegionDesc(size, rd->vertStride, rd->width, rd->horzStride);
    }

    /// Get the predefined region descriptors.
    RegionDesc *getRegionScalar()  { return &CanonicalRegionStride0; }
    RegionDesc *getRegionStride1() { return &CanonicalRegionStride1; }
    RegionDesc *getRegionStride2() { return &CanonicalRegionStride2; }
    RegionDesc *getRegionStride4() { return &CanonicalRegionStride4; }

    G4_SendMsgDescriptor* createSendMsgDesc(uint32_t desc,
        uint32_t extDesc,
        bool isRead,
        bool isWrite,
        G4_Operand *bti,
        G4_Operand *sti);

    G4_SendMsgDescriptor* createSendMsgDesc(
        unsigned funcCtrl,
        unsigned regs2rcv,
        unsigned regs2snd,
        CISA_SHARED_FUNCTION_ID funcID,
        bool eot,
        unsigned extMsgLength,
        uint16_t extFuncCtrl,
        bool isRead,
        bool isWrite,
        G4_Operand *bti,
        G4_Operand *sti);

    G4_Operand* emitSampleIndexGE16(
        G4_Operand* sampler, G4_Declare* headerDecl);

    //
    // deprecated, please use the one below
    //
    G4_SrcRegRegion* createSrcRegRegion(G4_SrcRegRegion& src)
    {
        G4_SrcRegRegion* rgn = new (mem)G4_SrcRegRegion(src);
        return rgn;
    }

    // Create a new srcregregion allocated in mem
    G4_SrcRegRegion* createSrcRegRegion(G4_SrcModifier m,
        G4_RegAccess   a,
        G4_VarBase*    b,
        short roff,
        short sroff,
        RegionDesc*    rd,
        G4_Type        ty,
        G4_AccRegSel regSel = ACC_UNDEFINED)
    {
        G4_SrcRegRegion* rgn = new (mem)G4_SrcRegRegion(m, a, b, roff, sroff, rd, ty, regSel);
        return rgn;
    }

    G4_SrcRegRegion* createIndirectSrc(G4_SrcModifier m,
        G4_VarBase*    b,
        short roff,
        short sroff,
        RegionDesc*    rd,
        G4_Type        ty,
        short immAddrOff)
    {
        G4_SrcRegRegion* rgn = new (mem) G4_SrcRegRegion(m, IndirGRF, b, roff, sroff, rd, ty, ACC_UNDEFINED);
        rgn->setImmAddrOff(immAddrOff);
        return rgn;
    }

    //
    // deprecated, please use the version below
    //
    G4_DstRegRegion* createDstRegRegion(G4_DstRegRegion& dst)
    {
        G4_DstRegRegion* rgn = new (mem) G4_DstRegRegion(dst);
        return rgn;
    }

    // create a new dstregregion allocated in mem
    G4_DstRegRegion* createDstRegRegion(G4_RegAccess a,
        G4_VarBase* b,
        short roff,
        short sroff,
        unsigned short hstride,
        G4_Type        ty,
        G4_AccRegSel regSel = ACC_UNDEFINED)
    {
        G4_DstRegRegion* rgn = new (mem) G4_DstRegRegion(a, b, roff, sroff, hstride, ty, regSel);
        return rgn;
    }

    //
    // return the imm operand; create one if not yet created
    //
    G4_Imm* createImm(int64_t imm, G4_Type ty)
    {
        G4_Imm* i = hashtable.lookupImm(imm, ty);
        return (i != NULL)? i : hashtable.createImm(imm, ty);
    }

    //
    // return the float operand; create one if not yet created
    //
    G4_Imm* createImm(float fp)
    {
        uint32_t imm = *((uint32_t*) &fp);
        G4_Type immType = Type_F;
        if (getGenxPlatform() >= GENX_CHV && m_options->getOption(vISA_FImmToHFImm) &&
            !VISA_WA_CHECK(getPWaTable(), WaSrc1ImmHfNotAllowed))
        {
            // we may be able to lower it to HF
            // ieee32 format: 23-8-1
            // ieee16 format: 10-5-1
            // bit0-22 are fractions
            uint32_t fraction = imm & 0x7FFFFF;
            // bit23-30 are exponents
            uint32_t exponent = (imm >> 23) & 0xFF;
            uint32_t sign = (imm >> 31) & 0x1;
            int expVal = ((int) exponent) - 127;

            if (exponent == 0 && fraction == 0)
            {
                // 0 and -0
                immType = Type_HF;
                imm = sign << 15;
            }
            else if ((fraction & 0x1FFF) == 0 && (expVal <= 15 && expVal >= -16))
            {
                // immediate can be exactly represented in HF.
                // we exclude denormal, infinity, and NaN.
                immType = Type_HF;
                uint32_t newExp = (expVal + 15) & 0x1F;
                imm = (sign << 15) | (newExp << 10) | (fraction >> 13);
            }
        }
        G4_Imm* i = hashtable.lookupImm(imm, immType);
        return (i != NULL)? i : hashtable.createImm(imm, immType);
    }

    //
    // return the double operand; create one if not yet created
    //
    G4_Imm* createDFImm(double fp)
    {
        int64_t val = (int64_t)(*(uint64_t*)&fp);
        G4_Imm* i = hashtable.lookupImm(val, Type_DF);
        return (i != NULL)? i : hashtable.createImm(val, Type_DF);
    }

    // For integer immediates use a narrower type if possible
    // also change byte type to word type since HW does not support byte imm
    G4_Type getNewType( int64_t imm, G4_Type ty )
    {
        switch(ty)
        {
        case Type_Q:
        case Type_D:
            // It is legal to change a positive imm's type from signed to unsigned if it fits
            // in the unsigned type. We do prefer signed type however for readability.
            if (imm >= MIN_WORD_VALUE && imm <= MAX_WORD_VALUE) 
            {
                return Type_W;
            } 
            else if (imm >= MIN_UWORD_VALUE && imm <= MAX_UWORD_VALUE) 
            {
                return Type_UW;
            } 
            else if (imm >= int(MIN_DWORD_VALUE) && imm <= int(MAX_DWORD_VALUE)) 
            {
                return Type_D;
            } 
            else if (imm >= unsigned(MIN_UDWORD_VALUE) && imm <= unsigned(MAX_UDWORD_VALUE)) 
            {
                return Type_UD;
            }
            break;
        case Type_UQ:
        case Type_UD:
            {
                // unsigned imm must stay as unsigned
                uint64_t immU = static_cast<uint64_t>(imm);
                if (immU <= MAX_UWORD_VALUE) 
                {
                    return Type_UW;
                }
                else if (immU <= unsigned(MAX_UDWORD_VALUE)) 
                {
                    return Type_UD;
                }
                break;
            }
        case Type_UB:
            return Type_UW;
        case Type_B:
            return Type_W;
        default:
            return ty;
        }
        return ty;
    }

    //
    // return the imm operand with its lowest type( W or above ); create one if not yet created
    //
    G4_Imm* createImmWithLowerType(int64_t imm, G4_Type ty)
    {
        G4_Type new_type = getNewType( imm, ty );
        G4_Imm* i = hashtable.lookupImm(imm, new_type);
        return (i != NULL)? i : hashtable.createImm(imm, new_type);
    }

    //
    // Create immediate operand without looking up hash table. This operand
    // is a relocatable immediate type.
    //
    G4_Reloc_Imm* createRelocImm(SuperRelocEntry& reloc, G4_Type ty)
    {
        G4_Reloc_Imm* newImm;
        newImm = new (mem)G4_Reloc_Imm(reloc, ty);
        return newImm;
    }

    //
    // return the label operand; create one if not found
    //
    G4_Label* lookupLabel(char* lab)
    {
        G4_Label* l = hashtable.lookupLabel(lab);
        return l;
    }

    //
    // return the label operand; create one if not found.
    // a new copy of "lab" is created for the new label, so
    // caller does not have to allocate memory for lab
    //
    G4_Label* createLabel(const char* lab, VISA_Label_Kind kind)
    {
        // Create new lab string with name of compilation unit
        // to ensure unique label names across compilation units.
        std::string newLab = lab;
        if (kind != LABEL_FC) {
            newLab += std::string("_") + kernel.getName();
#if _DEBUG
            newLab = sanitizeString(newLab);
#endif
        }
        lab = newLab.c_str();

        G4_Label* l = hashtable.lookupLabel(lab);
        return (l != NULL)? l : hashtable.createLabel(lab);
    }

    G4_Predicate* createPredicate(G4_PredState s, G4_VarBase* flag, unsigned short srOff, G4_Predicate_Control ctrl = PRED_DEFAULT)
    {
        G4_Predicate* pred = new (mem)G4_Predicate(s, flag, srOff, ctrl);
        return pred;
    }

    G4_Predicate* createPredicate(G4_Predicate& prd)
    {
        G4_Predicate* p = new (mem) G4_Predicate( prd );
        return p;
    }

    G4_CondMod* createCondMod(G4_CondModifier m, G4_VarBase* flag, unsigned short off)
    {
        G4_CondMod* p = new (mem)G4_CondMod(m, flag, off);
        return p;
    }

    //
    // return the condition modifier; create one if not yet created
    //
    G4_CondMod* createCondMod(G4_CondMod& mod)
    {
        G4_CondMod* p = new (mem) G4_CondMod( mod );
        return p;
    }

    //
    // create register address expression normalized to (&reg +/- exp)
    //
    G4_AddrExp* createAddrExp(G4_RegVar* reg, int offset, G4_Type ty)
    {
        return new (mem) G4_AddrExp(reg, offset, ty);
    }

    //
    // create instructions
    //
    G4_INST* createInst(G4_Predicate* prd, G4_opcode op,
                        G4_CondMod* mod, bool sat,
                        unsigned char size, G4_DstRegRegion* dst,
                        G4_Operand* src0, G4_Operand* src1,
                        unsigned int option);
    G4_INST* createInst(G4_Predicate* prd, G4_opcode op,
                        G4_CondMod* mod, bool sat,
                        unsigned char size, G4_DstRegRegion* dst,
                        G4_Operand* src0, G4_Operand* src1,
                        unsigned int option, int lineno);
    template <typename T>
    G4_INST* createInst(G4_Predicate* prd, G4_opcode op,
                        G4_CondMod* mod, bool sat,
                        T size, G4_DstRegRegion* dst,
                        G4_Operand* src0, G4_Operand* src1,
                        unsigned int option, int lineno)
    {
        unsigned char sz = static_cast<unsigned char>(size);
        return createInst(prd, op, mod, sat, sz, dst, src0, src1, option, lineno);
    }
    template <typename T>
    G4_INST* createInst(G4_Predicate* prd, G4_opcode op,
                        G4_CondMod* mod, bool sat,
                        T size, G4_DstRegRegion* dst,
                        G4_Operand* src0, G4_Operand* src1,
                        unsigned int option)
    {
        unsigned char sz = static_cast<unsigned char>(size);
        return createInst(prd, op, mod, sat, sz, dst, src0, src1, option);
    }

    G4_INST* createInternalInst(G4_Predicate* prd, G4_opcode op,
        G4_CondMod* mod, bool sat,
        unsigned char size, G4_DstRegRegion* dst,
        G4_Operand* src0, G4_Operand* src1,
        unsigned int option);
    G4_INST* createInternalInst(G4_Predicate* prd, G4_opcode op,
        G4_CondMod* mod, bool sat,
        unsigned char size, G4_DstRegRegion* dst,
        G4_Operand* src0, G4_Operand* src1,
        unsigned int option, int lineno, int CISAoff,
        char* srcFilename);

    G4_INST* createInst(G4_Predicate* prd, G4_opcode op,
        G4_CondMod* mod, bool sat,
        unsigned char size, G4_DstRegRegion* dst,
        G4_Operand* src0, G4_Operand* src1, G4_Operand* src2,
        unsigned int option);
    G4_INST* createInst(G4_Predicate* prd, G4_opcode op,
        G4_CondMod* mod, bool sat,
        unsigned char size, G4_DstRegRegion* dst,
        G4_Operand* src0, G4_Operand* src1, G4_Operand* src2,
        unsigned int option, int lineno);
    template <typename T>
    G4_INST* createInst(G4_Predicate* prd, G4_opcode op,
        G4_CondMod* mod, bool sat,
        T size, G4_DstRegRegion* dst,
        G4_Operand* src0, G4_Operand* src1, G4_Operand* src2,
        unsigned int option)
    {
        unsigned char sz = static_cast<unsigned char>(size);
        return createInst(prd, op, mod, sat, sz, dst, src0, src1, src2, option);
    }
    template <typename T>
    G4_INST* createInst(G4_Predicate* prd, G4_opcode op,
        G4_CondMod* mod, bool sat,
        T size, G4_DstRegRegion* dst,
        G4_Operand* src0, G4_Operand* src1, G4_Operand* src2,
        unsigned int option, int lineno)
    {
        unsigned char sz = static_cast<unsigned char>(size);
        return createInst(prd, op, mod, sat, sz, dst, src0, src1, src2, option, lineno);
    }

    G4_INST* createInternalInst(G4_Predicate* prd, G4_opcode op,
        G4_CondMod* mod, bool sat,
        unsigned char size, G4_DstRegRegion* dst,
        G4_Operand* src0, G4_Operand* src1, G4_Operand* src2,
        unsigned int option);
    G4_INST* createInternalInst(G4_Predicate* prd, G4_opcode op,
        G4_CondMod* mod, bool sat,
        unsigned char size, G4_DstRegRegion* dst,
        G4_Operand* src0, G4_Operand* src1, G4_Operand* src2,
        unsigned int option, int lineno, int CISAoff,
        char* srcFilename);

    G4_INST* createInternalCFInst(G4_Predicate* prd, G4_opcode op,
        unsigned char size, G4_Label* jip, G4_Label* uip,
        unsigned int option, int lineno = 0, int CISAoff = -1,
        char* srcFilename = NULL);


    G4_INST* createSendInst(G4_Predicate* prd, G4_opcode op,
                            unsigned char size, G4_DstRegRegion* postDst,
                            G4_SrcRegRegion* payload, G4_Operand* src,
                            G4_Operand* msg, unsigned int option,
                            bool isRead, bool isWrite,
                            G4_SendMsgDescriptor *msgDesc,
                            int lineno = 0);

    G4_INST* createSplitSendInst(G4_Predicate* prd, G4_opcode op,
                                 unsigned char size, G4_DstRegRegion* dst,
                                 G4_SrcRegRegion* src1, G4_SrcRegRegion* src2,
                                 G4_Operand* msg, unsigned int option,
                                 G4_SendMsgDescriptor *msgDesc,
                                 G4_Operand* src3,
                                 int lineno = 0);

    G4_INST* createMathInst(G4_Predicate* prd, bool sat,
                            unsigned char size, G4_DstRegRegion* dst,
                            G4_Operand* src0, G4_Operand* src1, G4_MathOp mathOp,
                            unsigned int option, int lineno = 0);

    G4_INST* createIntrinsicInst(G4_Predicate* prd, Intrinsic intrinId,
        unsigned char size, G4_DstRegRegion* dst,
        G4_Operand* src0, G4_Operand* src1, G4_Operand* src2,
        unsigned int option, int lineno = 0);

    G4_INST* createInternalIntrinsicInst(G4_Predicate* prd, Intrinsic intrinId,
        unsigned char size, G4_DstRegRegion* dst,
        G4_Operand* src0, G4_Operand* src1, G4_Operand* src2,
        unsigned int option, int lineno = 0);

    G4_MathOp Get_MathFuncCtrl(ISA_Opcode op, G4_Type type);
    void resizePredefinedStackVars();

    G4_Operand* duplicateOpndImpl( G4_Operand* opnd );
    template <typename T> T* duplicateOperand(T* opnd)
    {
        return static_cast<T *>(duplicateOpndImpl(opnd));
    }

    G4_INST *Create_Send_Inst_For_CISA(G4_Predicate *pred,
                                       G4_DstRegRegion *postDst,
                                       G4_SrcRegRegion *payload,
                                       unsigned execSize,
                                       G4_SendMsgDescriptor *msgDesc,
                                       unsigned option,
                                       bool is_sendc);

    G4_INST *Create_SplitSend_Inst_For_CISA(G4_Predicate *pred,
                                            G4_DstRegRegion *dst,
                                            G4_SrcRegRegion *src1,
                                            G4_SrcRegRegion *src2,
                                            unsigned execsize,
                                            G4_SendMsgDescriptor *msgDesc,
                                            unsigned option,
                                            bool is_sendc);

    G4_INST* Create_Send_Inst_For_CISA(
                        G4_Predicate* pred,
                        G4_DstRegRegion *postDst,
                        G4_SrcRegRegion *payload,
                        unsigned regs2snd,
                        unsigned regs2rcv,
                        unsigned execsize,
                        unsigned fc,
                        CISA_SHARED_FUNCTION_ID tf_id,
                        bool eot,
                        bool head_present,
                        bool isRead,
                        bool isWrite,
                        G4_Operand *bti,
                        G4_Operand *sti,
                        unsigned int option,
                        bool is_sendc);

    G4_INST* Create_SplitSend_Inst_For_CISA(G4_Predicate* pred, G4_DstRegRegion *dst,
                                            G4_SrcRegRegion *src1, unsigned regs2snd1,
                                            G4_SrcRegRegion *src2, unsigned regs2snd2,
                                            unsigned regs2rcv,
                                            unsigned execsize,
                                            unsigned fc, unsigned exFuncCtrl,
                                            CISA_SHARED_FUNCTION_ID tf_id, bool eot,
                                            bool head_present,
                                            bool isRead,
                                            bool isWrite,
                                            G4_Operand *bti, G4_Operand *sti,
                                            unsigned option,
                                            bool is_sendc);

    // helper functions
    G4_Declare *Create_MRF_Dcl( unsigned num_elt, G4_Type type );
    void Create_MOVR0_Inst(
                        G4_Declare* dcl,
                        short refOff,
                        short subregOff,
                        bool use_nomask = false );
    void Create_MOV_Inst(
                        G4_Declare* dcl,
                        short refOff,
                        short subregOff,
                        unsigned execsize,
                        G4_Predicate* pred,
                        G4_CondMod* condMod,
                        G4_Operand* src_opnd,
                        bool use_nomask = false );
    void Create_ADD_Inst(
                        G4_Declare* dcl,
                        short         regOff,
                        short         subregOff,
                        uint8_t      execsize,
                        G4_Predicate* pred,
                        G4_CondMod*   condMod,
                        G4_Operand*   src0_opnd,
                        G4_Operand*   src1_opnd,
                        G4_InstOption options);
    void Create_MOV_Send_Src_Inst(
        G4_Declare* dcl,
        short refOff,
        short subregOff,
        unsigned num_dword,
        G4_Operand* src_opnd,
        unsigned int option );

    G4_INST* createFenceInstruction( uint8_t flushParam, bool commitEnable, bool globalMemFence, bool isSendc);

    // short hand for creating a dstRegRegion
    G4_DstRegRegion* Create_Dst_Opnd_From_Dcl( G4_Declare* dcl, unsigned short hstride );
    G4_SrcRegRegion* Create_Src_Opnd_From_Dcl( G4_Declare* dcl, RegionDesc* rd );

    // Create a null dst with scalar region and the given type
    G4_DstRegRegion* createNullDst( G4_Type dstType );

    // Create a null src with scalar region and the given type
    G4_SrcRegRegion* createNullSrc( G4_Type dstType );

    G4_DstRegRegion* Check_Send_Dst( G4_DstRegRegion *dst_opnd );

    unsigned int getByteOffsetSrcRegion(
        G4_SrcRegRegion* srcRegion );

    bool checkIfRegionsAreConsecutive(
        G4_SrcRegRegion* first,
        G4_SrcRegRegion* second,
        unsigned int exec_size );

    bool checkIfRegionsAreConsecutive(
        G4_SrcRegRegion* first,
        G4_SrcRegRegion* second,
        unsigned int exec_size,
        G4_Type type);

    // emask is InstOption
    void Copy_SrcRegRegion_To_Payload(
        G4_Declare* payload,
        unsigned int& regOff,
        G4_SrcRegRegion* src,
        unsigned int exec_size,
        uint32_t emask );

    // functions translating vISA inst to G4_INST
    int translateVISAGather3dInst(
        VISASampler3DSubOpCode actualop,
        bool pixelNullMask,
        G4_Predicate* pred,
        Common_ISA_Exec_Size exeuctionSize,
        Common_VISA_EMask_Ctrl em,
        ChannelMask channelMask,
        G4_Operand* aoffimmi,
        G4_Operand* sampler,
        G4_Operand* surface,
        G4_DstRegRegion* dst,
        unsigned int numOpnds,
        G4_SrcRegRegion ** opndArray );

    int translateVISALoad3DInst(
        VISASampler3DSubOpCode actualop,
        bool pixelNullMask,
        G4_Predicate *pred,
        Common_ISA_Exec_Size exeuctionSize,
        Common_VISA_EMask_Ctrl em,
        ChannelMask channelMask,
        G4_Operand* aoffimmi,
        G4_Operand* surface,
        G4_DstRegRegion* dst,
        uint8_t numOpnds,
        G4_SrcRegRegion ** opndArray);

    int translateVISAAddrInst(ISA_Opcode opcode,
                        Common_ISA_Exec_Size execSize,
                        Common_VISA_EMask_Ctrl emask,
                        G4_DstRegRegion *dst_opnd,
                        G4_Operand *src0_opnd,
                        G4_Operand *src1_opnd);

    int translateVISAArithmeticInst(ISA_Opcode opcode,
                        Common_ISA_Exec_Size execSize,
                        Common_VISA_EMask_Ctrl emask,
                        G4_Predicate *predOpnd,
                        bool saturate,
                        G4_CondMod* condMod,
                        G4_DstRegRegion *dstOpnd,
                        G4_Operand *src0Opnd,
                        G4_Operand *src1Opnd,
                        G4_Operand *src2Opnd,
                        G4_DstRegRegion *carryBorrow);

    int translateVISAArithmeticDoubleInst(ISA_Opcode opcode,
                        Common_ISA_Exec_Size execSize,
                        Common_VISA_EMask_Ctrl emask,
                        G4_Predicate *predOpnd,
                        bool saturate,
                        G4_CondMod* condMod,
                        G4_DstRegRegion *dstOpnd,
                        G4_Operand *src0Opnd,
                        G4_Operand *src1Opnd);

    int translateVISAArithmeticSingleDivideIEEEInst(ISA_Opcode opcode,
                        Common_ISA_Exec_Size execSize,
                        Common_VISA_EMask_Ctrl emask,
                        G4_Predicate *predOpnd,
                        bool saturate,
                        G4_CondMod* condMod,
                        G4_DstRegRegion *dstOpnd,
                        G4_Operand *src0Opnd,
                        G4_Operand *src1Opnd);

    int translateVISAArithmeticSingleSQRTIEEEInst(ISA_Opcode opcode,
                        Common_ISA_Exec_Size execSize,
                        Common_VISA_EMask_Ctrl emask,
                        G4_Predicate *predOpnd,
                        bool saturate,
                        G4_CondMod* condMod,
                        G4_DstRegRegion *dstOpnd,
                        G4_Operand *src0Opnd );

    int translateVISAArithmeticDoubleSQRTInst(ISA_Opcode opcode,
                        Common_ISA_Exec_Size execSize,
                        Common_VISA_EMask_Ctrl emask,
                        G4_Predicate *predOpnd,
                        bool saturate,
                        G4_CondMod* condMod,
                        G4_DstRegRegion *dstOpnd,
                        G4_Operand *src0Opnd);

    int translateVISASyncInst(ISA_Opcode opcode, unsigned int mask);

    int translateVISAWaitInst(G4_Operand* mask);

    int translateVISAPredBarrierInst(G4_Operand *mask, G4_DstRegRegion *dst);


    int translateVISACompareInst(ISA_Opcode opcode,
                        Common_ISA_Exec_Size execSize,
                        Common_VISA_EMask_Ctrl emask,
                        Common_ISA_Cond_Mod relOp,
                        G4_Predicate *dst_opnd,
                        G4_Operand *src0_opnd,
                        G4_Operand *src1_opnd);

    int translateVISACompareInst(ISA_Opcode opcode,
                        Common_ISA_Exec_Size execSize,
                        Common_VISA_EMask_Ctrl emask,
                        Common_ISA_Cond_Mod relOp,
                        G4_DstRegRegion *dstOpnd,
                        G4_Operand *src0Opnd,
                        G4_Operand *src1Opnd);

    int translateVISACFSwitchInst(
                        G4_Operand *indexOpnd,
                        uint8_t numLabels,
                        G4_Label** lab );

    int translateVISACFLabelInst(G4_Label* lab);

    int translateVISACFCallInst(Common_ISA_Exec_Size execsize,
                        Common_VISA_EMask_Ctrl emask,
                        G4_Predicate *predOpnd,
                        G4_Label* lab);

    int translateVISACFJumpInst(G4_Predicate *predOpnd,
                        G4_Label* lab);

    int translateVISACFFCallInst(Common_ISA_Exec_Size execsize,
                        Common_VISA_EMask_Ctrl emask,
                        G4_Predicate *predOpnd,
                        uint16_t functionID,
                        uint8_t argSize,
                        uint8_t returnSize);

    int translateVISACFFretInst(Common_ISA_Exec_Size execsize,
                        Common_VISA_EMask_Ctrl emask,
                        G4_Predicate *predOpnd);

    int translateVISACFRetInst(Common_ISA_Exec_Size execsize,
                        Common_VISA_EMask_Ctrl emask,
                        G4_Predicate *predOpnd);

    int translateVISAOwordLoadInst(
                        ISA_Opcode opcode,
                        bool modified,
                        G4_Operand* surface,
                        Common_ISA_Oword_Num size,
                        G4_Operand* offOpnd,
                        G4_DstRegRegion* dstOpnd );

    int translateVISAOwordStoreInst(
                        G4_Operand* surface,
                        Common_ISA_Oword_Num size,
                        G4_Operand* offOpnd,
                        G4_SrcRegRegion* srcOpnd );

    int translateVISAMediaLoadInst(
                        MEDIA_LD_mod mod,
                        G4_Operand* surface,
                        unsigned planeID,
                        unsigned blockWidth,
                        unsigned blockHeight,
                        G4_SrcRegRegion* xOffOpnd,
                        G4_SrcRegRegion* yOffOpnd,
                        G4_DstRegRegion* dst_opnd );

    int translateVISAMediaStoreInst(
                        MEDIA_ST_mod mod,
                        G4_Operand* surface,
                        unsigned planeID,
                        unsigned blockWidth,
                        unsigned blockHeight,
                        G4_SrcRegRegion* xOffOpnd,
                        G4_SrcRegRegion* yOffOpnd,
                        G4_SrcRegRegion* srcOpnd );

    int translateVISAGatherInst(
                        Common_VISA_EMask_Ctrl emask,
                        bool modified,
                        GATHER_SCATTER_ELEMENT_SIZE eltSize,
                        Common_ISA_Exec_Size executionSize,
                        G4_Operand* surface,
                        G4_Operand* gOffOpnd,
                        G4_SrcRegRegion* eltOFfOpnd,
                        G4_DstRegRegion* dstOpnd
                        );

    int translateVISAScatterInst(
                        Common_VISA_EMask_Ctrl emask,
                        GATHER_SCATTER_ELEMENT_SIZE eltSize,
                        Common_ISA_Exec_Size executionSize,
                        G4_Operand* surface,
                        G4_Operand* gOffOpnd,
                        G4_SrcRegRegion* eltOffOpnd,
                        G4_SrcRegRegion* srcOpnd );

    int translateVISAGather4Inst(
                        Common_VISA_EMask_Ctrl emask,
                        bool modified,
                        ChannelMask chMask,
                        Common_ISA_Exec_Size executionSize,
                        G4_Operand* surface,
                        G4_Operand* gOffOpnd,
                        G4_SrcRegRegion* eltOffOpnd,
                        G4_DstRegRegion* dstOpnd );

    int translateVISAScatter4Inst(
                        Common_VISA_EMask_Ctrl emask,
                        ChannelMask chMask,
                        Common_ISA_Exec_Size executionSize,
                        G4_Operand* surface,
                        G4_Operand* gOffOpnd,
                        G4_SrcRegRegion* eltOffOpnd,
                        G4_SrcRegRegion* srcOpnd );

    int translateVISADwordAtomicInst(
                        CMAtomicOperations op,
                        bool is16Bit,
                        Common_VISA_EMask_Ctrl emask,
                        Common_ISA_Exec_Size executionSize,
                        G4_Operand* surface,
                        G4_Operand* gOffOpnd,
                        G4_SrcRegRegion* eltOffOpnd,
                        G4_SrcRegRegion* src0Opnd,
                        G4_SrcRegRegion* src1Opnd,
                        G4_DstRegRegion* dstOpnd);

    int translateVISADwordAtomicInst(CMAtomicOperations subOpc,
                                     bool is16Bit,
                                     G4_Predicate *pred,
                                     Common_ISA_Exec_Size execSize,
                                     Common_VISA_EMask_Ctrl eMask,
                                     G4_Operand* surface,
                                     G4_SrcRegRegion* offsets,
                                     G4_SrcRegRegion* src0,
                                     G4_SrcRegRegion* src1,
                                     G4_DstRegRegion* dst);

    int translateVISATypedAtomicInst(
        CMAtomicOperations atomicOp,
        bool is16Bit,
        G4_Predicate           *pred,
        Common_VISA_EMask_Ctrl emask,
        Common_ISA_Exec_Size execSize,
        G4_Operand *surface,
        G4_SrcRegRegion *uOffsetOpnd,
        G4_SrcRegRegion *vOffsetOpnd,
        G4_SrcRegRegion *rOffsetOpnd,
        G4_SrcRegRegion *lodOpnd,
        G4_SrcRegRegion *src0,
        G4_SrcRegRegion *src1,
        G4_DstRegRegion *dst);

    int translateTransposeVISALoadInst(
                        G4_Operand* surface,
                        unsigned blockWidth,
                        unsigned blockHeight,
                        G4_Operand* xOffOpnd,
                        G4_Operand* yOffOpnd,
                        G4_DstRegRegion* dstOpnd );

    int translateVISAGather4TypedInst(G4_Predicate           *pred,
                                      Common_VISA_EMask_Ctrl emask,
                                      ChannelMask chMask,
                                      G4_Operand *surfaceOpnd,
                                      Common_ISA_Exec_Size executionSize,
                                      G4_SrcRegRegion *uOffsetOpnd,
                                      G4_SrcRegRegion *vOffsetOpnd,
                                      G4_SrcRegRegion *rOffsetOpnd,
                                      G4_SrcRegRegion *lodOpnd,
                                      G4_DstRegRegion *dstOpnd);

    int translateVISAScatter4TypedInst(G4_Predicate           *pred,
                                       Common_VISA_EMask_Ctrl emask,
                                       ChannelMask chMask,
                                       G4_Operand *surfaceOpnd,
                                       Common_ISA_Exec_Size executionSize,
                                       G4_SrcRegRegion *uOffsetOpnd,
                                       G4_SrcRegRegion *vOffsetOpnd,
                                       G4_SrcRegRegion *rOffsetOpnd,
                                       G4_SrcRegRegion *lodOpnd,
                                       G4_SrcRegRegion *srcOpnd);

    int translateVISAGather4ScaledInst(G4_Predicate *pred,
                                       Common_ISA_Exec_Size execSize,
                                       Common_VISA_EMask_Ctrl eMask,
                                       ChannelMask chMask,
                                       G4_Operand *surface,
                                       G4_Operand *globalOffset,
                                       G4_SrcRegRegion *offsets,
                                       G4_DstRegRegion *dst);

    int translateVISAScatter4ScaledInst(G4_Predicate *pred,
                                        Common_ISA_Exec_Size execSize,
                                        Common_VISA_EMask_Ctrl eMask,
                                        ChannelMask chMask,
                                        G4_Operand *surface,
                                        G4_Operand *globalOffset,
                                        G4_SrcRegRegion *offsets,
                                        G4_SrcRegRegion *src);

    int translateVISAStrBufLdScaledInst(G4_Predicate *pred,
                                        Common_ISA_Exec_Size execSize,
                                        Common_VISA_EMask_Ctrl eMask,
                                        ChannelMask chMask,
                                        G4_Operand *surface,
                                        G4_SrcRegRegion *uOffsets,
                                        G4_SrcRegRegion *vOffsets,
                                        G4_DstRegRegion *dst);
    int translateVISAStrBufStScaledInst(G4_Predicate *pred,
                                        Common_ISA_Exec_Size execSize,
                                        Common_VISA_EMask_Ctrl eMask,
                                        ChannelMask chMask,
                                        G4_Operand *surface,
                                        G4_SrcRegRegion *uOffsets,
                                        G4_SrcRegRegion *vOffsets,
                                        G4_SrcRegRegion *src);

    int translateVISAGatherScaledInst(G4_Predicate *pred,
                                      Common_ISA_Exec_Size execSize,
                                      Common_VISA_EMask_Ctrl eMask,
                                      Common_ISA_SVM_Block_Num numBlocks,
                                      G4_Operand *surface,
                                      G4_Operand *globalOffset,
                                      G4_SrcRegRegion *offsets,
                                      G4_DstRegRegion *dst);

    int translateVISAScatterScaledInst(G4_Predicate *pred,
                                       Common_ISA_Exec_Size execSize,
                                       Common_VISA_EMask_Ctrl eMask,
                                       Common_ISA_SVM_Block_Num numBlocks,
                                       G4_Operand *surface,
                                       G4_Operand *globalOffset,
                                       G4_SrcRegRegion *offsets,
                                       G4_SrcRegRegion *src);

    int translateByteGatherInst(G4_Predicate *pred,
                                Common_ISA_Exec_Size execSize,
                                Common_VISA_EMask_Ctrl eMask,
                                Common_ISA_SVM_Block_Num numBlocks,
                                G4_Operand *surface,
                                G4_Operand *globalOffset,
                                G4_SrcRegRegion *offsets,
                                G4_DstRegRegion *dst);
    int translateByteScatterInst(G4_Predicate *pred,
                                 Common_ISA_Exec_Size execSize,
                                 Common_VISA_EMask_Ctrl eMask,
                                 Common_ISA_SVM_Block_Num numBlocks,
                                 G4_Operand *surface,
                                 G4_Operand *globalOffset,
                                 G4_SrcRegRegion *offsets,
                                 G4_SrcRegRegion *src);

    int translateGather4Inst(G4_Predicate *pred,
                             Common_ISA_Exec_Size execSize,
                             Common_VISA_EMask_Ctrl eMask,
                             ChannelMask chMask,
                             G4_Operand *surface,
                             G4_Operand *globalOffset,
                             G4_SrcRegRegion *offsets,
                             G4_DstRegRegion *dst);
    int translateScatter4Inst(G4_Predicate *pred,
                              Common_ISA_Exec_Size execSize,
                              Common_VISA_EMask_Ctrl eMask,
                              ChannelMask chMask,
                              G4_Operand *surface,
                              G4_Operand *globalOffset,
                              G4_SrcRegRegion *offsets,
                              G4_SrcRegRegion *src);

    int translateVISALogicInst(ISA_Opcode opcode,
                        G4_Predicate *pred_opnd,
                        bool saturate,
                        Common_ISA_Exec_Size executionSize,
                        Common_VISA_EMask_Ctrl emask,
                        G4_DstRegRegion* dst,
                        G4_Operand* src0,
                        G4_Operand* src1,
                        G4_Operand* src2,
                        G4_Operand* src3);

    int translateVISAVmeImeInst(
                        uint8_t stream_mode,
                        uint8_t search_ctrl,
                        G4_Operand* surfaceOpnd,
                        G4_Operand* uniInputOpnd,
                        G4_Operand* imeInputOpnd,
                        G4_Operand* ref0Opnd,
                        G4_Operand* ref1Opnd,
                        G4_Operand* costCenterOpnd,
                        G4_DstRegRegion* outputOpnd);

    int translateVISAVmeSicInst(
                        G4_Operand* surfaceOpnd,
                        G4_Operand* uniInputOpnd,
                        G4_Operand* sicInputOpnd,
                        G4_DstRegRegion* outputOpnd);

    int translateVISAVmeFbrInst(
                        G4_Operand* surfaceOpnd,
                        G4_Operand* unitInputOpnd,
                        G4_Operand* fbrInputOpnd,
                        G4_Operand* fbrMbModOpnd,
                        G4_Operand* fbrSubMbShapeOpnd,
                        G4_Operand* fbrSubPredModeOpnd,
                        G4_DstRegRegion* outputOpnd);

    int translateVISAVmeIdmInst(
                        G4_Operand* surfaceOpnd,
                        G4_Operand* unitInputOpnd,
                        G4_Operand* idmInputOpnd,
                        G4_DstRegRegion* outputOpnd);

    int translateVISARawSendInst(
                        G4_Predicate *predOpnd,
                        Common_ISA_Exec_Size executionSize,
                        Common_VISA_EMask_Ctrl emask,
                        uint8_t modifiers,
                        unsigned int exDesc,
                        uint8_t numSrc,
                        uint8_t numDst,
                        G4_Operand* msgDescOpnd,
                        G4_SrcRegRegion* msgOpnd,
                        G4_DstRegRegion* dstOpnd);

    int translateVISARawSendsInst(
                        G4_Predicate *predOpnd,
                        Common_ISA_Exec_Size executionSize,
                        Common_VISA_EMask_Ctrl emask,
                        uint8_t modifiers,
                        G4_Operand* exDesc,
                        uint8_t numSrc0,
                        uint8_t numSrc1,
                        uint8_t numDst,
                        G4_Operand* msgDescOpnd,
                        G4_Operand* msgOpnd0,
                        G4_Operand* msgOpnd1,
                        G4_DstRegRegion* dstOpnd,
                        unsigned ffid);

    int translateVISASamplerVAGenericInst(
                        G4_Operand*   surface,
                        G4_Operand*   sampler,
                        G4_Operand*   uOffOpnd,
                        G4_Operand*   vOffOpnd,
                        G4_Operand*   vSizeOpnd,
                        G4_Operand*   hSizeOpnd,
                        G4_Operand*   mmfMode,
                        unsigned char cntrl,
                        unsigned char msgSeq,
                        VA_fopcode    fopcode,
                        G4_DstRegRegion*   dstOpnd,
                        G4_Type       dstType,
                        unsigned      dstSize,
                        bool isBigKernel = false);

    int translateVISAAvsInst(
                        G4_Operand* surface,
                        G4_Operand* sampler,
                        ChannelMask channel,
                        unsigned numEnabledChannels,
                        G4_Operand* deltaUOpnd,
                        G4_Operand* uOffOpnd,
                        G4_Operand* deltaVOpnd,
                        G4_Operand* vOffOpnd,
                        G4_Operand* u2dOpnd,
                        G4_Operand* groupIDOpnd,
                        G4_Operand* verticalBlockNumberOpnd,
                        unsigned char cntrl,
                        G4_Operand* v2dOpnd,
                        unsigned char execMode,
                        G4_Operand* eifbypass,
                        G4_DstRegRegion* dstOpnd );

    int translateVISADataMovementInst(
                        ISA_Opcode opcode,
                        CISA_MIN_MAX_SUB_OPCODE subOpcode,
                        G4_Predicate *pred_opnd,
                        Common_ISA_Exec_Size executionSize,
                        Common_VISA_EMask_Ctrl emask,
                        bool saturate,
                        G4_DstRegRegion *dst,
                        G4_Operand *src0,
                        G4_Operand *src1);

    int translateVISASamplerInst(
                        unsigned simdMode,
                        G4_Operand* surface,
                        G4_Operand* sampler,
                        ChannelMask channel,
                        unsigned numEnabledChannels,
                        G4_Operand* uOffOpnd,
                        G4_Operand* vOffOpnd,
                        G4_Operand* rOffOpnd,
                        G4_DstRegRegion* dstOpnd );

    int translateVISAVaSklPlusGeneralInst(
                        ISA_VA_Sub_Opcode sub_opcode,
                        G4_Operand* surface,
                        G4_Operand* sampler,
                        unsigned char mode,
                        unsigned char functionality,
                        G4_Operand* uOffOpnd,
                        G4_Operand* vOffOpnd,
                        //1pixel convolve
                        G4_Operand * offsetsOpnd,

                        //FloodFill
                        G4_Operand* loopCountOpnd,
                        G4_Operand* pixelHMaskOpnd,
                        G4_Operand* pixelVMaskLeftOpnd,
                        G4_Operand* pixelVMaskRightOpnd,

                        //LBP Correlation
                        G4_Operand* disparityOpnd,

                        //Correlation Search
                        G4_Operand* verticalOriginOpnd,
                        G4_Operand* horizontalOriginOpnd,
                        G4_Operand* xDirectionSizeOpnd,
                        G4_Operand* yDirectionSizeOpnd,
                        G4_Operand* xDirectionSearchSizeOpnd,
                        G4_Operand* yDirectionSearchSizeOpnd,

                        G4_DstRegRegion* dstOpnd,
                        G4_Type dstType,
                        unsigned dstSize,

                        //HDC
                        unsigned char pixelSize,
                        G4_Operand* dstSurfaceOpnd,
                        G4_Operand *dstXOpnd,
                        G4_Operand* dstYOpnd,
                        bool hdcMode);

    int translateVISASamplerNormInst(
                        G4_Operand* surface,
                        G4_Operand* sampler,
                        ChannelMask channel,
                        unsigned numEnabledChannels,
                        G4_Operand* deltaUOpnd,
                        G4_Operand* uOffOpnd,
                        G4_Operand* deltaVOpnd,
                        G4_Operand* vOffOpnd,
                        G4_DstRegRegion* dst_opnd );

    int translateVISASimdInst(
                        ISA_Opcode opcode,
                        G4_Predicate *predOpnd,
                        Common_ISA_Exec_Size executionSize,
                        Common_VISA_EMask_Ctrl emask,
                        G4_Label *label);

    int translateVISASampler3DInst(
                        VISASampler3DSubOpCode actualop,
                        bool pixelNullMask,
                        bool cpsEnable,
                        bool uniformSampler,           
                        G4_Predicate* pred,
                        Common_ISA_Exec_Size executionSize,
                        Common_VISA_EMask_Ctrl emask,
                        ChannelMask srcChannel,
                        G4_Operand* aoffimmi,
                        G4_Operand *sampler,
                        G4_Operand *surface,
                        G4_DstRegRegion* dst,
                        unsigned int numParms,
                        G4_SrcRegRegion ** params);

    int translateVISASampleInfoInst(
        Common_ISA_Exec_Size executionSize,
        Common_VISA_EMask_Ctrl emask,
        ChannelMask chMask,
        G4_Operand* surface,
        G4_DstRegRegion* dst);

    int translateVISAResInfoInst(
        Common_ISA_Exec_Size executionSize,
        Common_VISA_EMask_Ctrl emask,
        ChannelMask chMask,
        G4_Operand* surface,
        G4_SrcRegRegion* lod,
        G4_DstRegRegion* dst );


    int translateVISAURBWrite3DInst(
                        G4_Predicate* pred,
                        Common_ISA_Exec_Size executionSize,
                        Common_VISA_EMask_Ctrl emask,
                        uint8_t numOut,
                        uint16_t globalOffset,
                        G4_SrcRegRegion* channelMask,
                        G4_SrcRegRegion* urbHandle,
                        G4_SrcRegRegion* perSlotOffset,
                        G4_SrcRegRegion* vertexData );

    int translateVISARTWrite3DInst(
                        G4_Predicate* pred,
                        Common_ISA_Exec_Size executionSize,
                        Common_VISA_EMask_Ctrl emask,
                        G4_Operand *surface,
                        G4_SrcRegRegion *r1HeaderOpnd,
                        G4_Operand *rtIndex,
                        vISA_RT_CONTROLS cntrls,
                        G4_SrcRegRegion *sampleIndexOpnd,
                        G4_Operand *cpsCounter,
                        unsigned int numParms,
                        G4_SrcRegRegion ** msgOpnds);

    int translateVISASVMBlockReadInst(
        Common_ISA_Oword_Num numOword,
        bool unaligned,
        G4_Operand* address,
        G4_DstRegRegion* dst);

    int translateVISASVMBlockWriteInst(
        Common_ISA_Oword_Num numOword,
        G4_Operand* address,
        G4_SrcRegRegion* src);

    int translateVISASVMScatterReadInst(
        Common_ISA_Exec_Size executionSize,
        Common_VISA_EMask_Ctrl emask,
        G4_Predicate* pred,
        Common_ISA_SVM_Block_Type blockSize,
        Common_ISA_SVM_Block_Num numBlocks,
        G4_SrcRegRegion* addresses,
        G4_DstRegRegion* dst);

    int translateVISASVMScatterWriteInst(
        Common_ISA_Exec_Size executionSize,
        Common_VISA_EMask_Ctrl emask,
        G4_Predicate* pred,
        Common_ISA_SVM_Block_Type blockSize,
        Common_ISA_SVM_Block_Num numBlocks,
        G4_SrcRegRegion* addresses,
        G4_SrcRegRegion* src);

    int translateVISASVMAtomicInst(
        CMAtomicOperations op,
        bool is16Bit,
        Common_ISA_Exec_Size executionSize,
        Common_VISA_EMask_Ctrl emask,
        G4_Predicate* pred,
        G4_SrcRegRegion* addresses,
        G4_SrcRegRegion* src0,
        G4_SrcRegRegion* src1,
        G4_DstRegRegion* dst);

    int translateSVMGather4Inst(Common_ISA_Exec_Size      execSize,
                                Common_VISA_EMask_Ctrl    eMask,
                                ChannelMask               chMask,
                                G4_Predicate              *pred,
                                G4_Operand                *address,
                                G4_SrcRegRegion           *offsets,
                                G4_DstRegRegion           *dst);

    int translateSVMScatter4Inst(Common_ISA_Exec_Size     execSize,
                                 Common_VISA_EMask_Ctrl   eMask,
                                 ChannelMask              chMask,
                                 G4_Predicate             *pred,
                                 G4_Operand               *address,
                                 G4_SrcRegRegion          *offsets,
                                 G4_SrcRegRegion          *src);

    int translateVISASVMGather4ScaledInst(Common_ISA_Exec_Size      execSize,
                                          Common_VISA_EMask_Ctrl    eMask,
                                          ChannelMask               chMask,
                                          G4_Predicate              *pred,
                                          G4_Operand                *address,
                                          G4_SrcRegRegion           *offsets,
                                          G4_DstRegRegion           *dst);

    int translateVISASVMScatter4ScaledInst(Common_ISA_Exec_Size     execSize,
                                           Common_VISA_EMask_Ctrl   eMask,
                                           ChannelMask              chMask,
                                           G4_Predicate             *pred,
                                           G4_Operand               *address,
                                           G4_SrcRegRegion          *offsets,
                                           G4_SrcRegRegion          *src);
    int translateVISALifetimeInst(
        unsigned char properties,
        G4_Operand* var);

    unsigned int getObjWidth(unsigned blockWidth, unsigned blockHeight, G4_Declare * dcl)
    {
        //makes sure io_width is divisible by 4
        unsigned ioWidth = (blockWidth + G4_Type_Table[Type_D].byteSize - 1) & (~(G4_Type_Table[Type_D].byteSize - 1));
        //gets next power of 2 size
        return Round_Up_Pow2(ioWidth / dcl->getElemSize()) * dcl->getElemSize();
    }
    void fixSendDstType(G4_DstRegRegion* dst, uint8_t execSize );

    struct payloadSource {
        G4_SrcRegRegion *opnd;
        unsigned        execSize;
        unsigned        instOpt;
    };

    /// preparePayload - This method prepares payload from the specified header
    /// and sources.
    ///
    /// \param msgs             Message(s) prepared. That 2-element array must
    ///                         be cleared before calling preparePayload().
    /// \param sizes            Size(s) (in GRF) of each message prepared. That
    ///                         2-element array must be cleared before calling
    ///                         preparePayload().
    /// \param batchExSize      When it's required to copy sources, batchExSize
    ///                         specifies the SIMD width of copy.
    /// \param splitSendEnabled Whether feature split-send is available. When
    ///                         feature split-send is available, this function
    ///                         will check whether two consecutive regions
    ///                         could be prepared instead of one to take
    ///                         advantage of split-send.
    /// \param srcs             The array of sources (including header if
    ///                         present).
    /// \param len              The length of the array of sources.
    ///
    void preparePayload(G4_SrcRegRegion *msgs[2], unsigned sizes[2],
                        unsigned batchExSize, bool splitSendEnabled,
                        payloadSource sources[], unsigned len);

#define FIX_OWORD_SEND_EXEC_SIZE(BLOCK_SIZE)(((BLOCK_SIZE) > 2)? 16: (BLOCK_SIZE*4))

    char * createNameSpace(uint8_t size){ return (char*)mem.alloc(size); }

    // return either 253 or 255 for A64 messages, depending on whether we want I/A coherency or not
    uint8_t getA64BTI() const { return m_options->getOption(vISA_noncoherentStateless) ? 0xFD : 0xFF; }

    bool useSends() const
    {
        return getGenxPlatform() >= GENX_SKL && m_options->getOption(vISA_UseSends) &&
            !(VISA_WA_CHECK(m_pWaTable, WaDisableSendsSrc0DstOverlap));
    }

    void doSamplerHeaderMove(G4_Declare* header, G4_Operand* sampler);

    void set64BitFEStackVars() { use64BitFEStackVars = true; }
    void set32BitFEStackVars() { use64BitFEStackVars = false; }
    bool is64BitFEStackVars() { return use64BitFEStackVars; }

    void expandFdiv(uint8_t exsize, G4_Predicate *predOpnd, bool saturate,
        G4_DstRegRegion *dstOpnd, G4_Operand *src0Opnd, G4_Operand *src1Opnd, uint32_t instOpt);

    void expandPow(uint8_t exsize, G4_Predicate *predOpnd, bool saturate,
        G4_DstRegRegion *dstOpnd, G4_Operand *src0Opnd, G4_Operand *src1Opnd, uint32_t instOpt);

#include "HWCapsOpen.inc"

private:
    G4_SrcRegRegion* createBindlessExDesc(uint32_t exdesc);

    int translateVISASLMByteScaledInst(
        bool isRead,
        G4_Predicate *pred,
        Common_ISA_Exec_Size execSize,
        Common_VISA_EMask_Ctrl eMask,
        Common_ISA_SVM_Block_Type blockSize,
        Common_ISA_SVM_Block_Num numBlocks,
        uint8_t scale,
        G4_Operand *sideBand,
        G4_SrcRegRegion *offsets,
        G4_Operand *srcOrDst);

    int translateVISASLMUntypedScaledInst(
        bool isRead,
        G4_Predicate *pred,
        Common_ISA_Exec_Size   execSize,
        Common_VISA_EMask_Ctrl eMask,
        ChannelMask            chMask,
        uint16_t               scale,
        G4_Operand             *globalOffset,
        G4_SrcRegRegion        *offsets,
        G4_Operand             *srcOrDst);

    void applySideBandOffset(G4_Operand* sideBand, G4_SendMsgDescriptor* sendMsgDesc);

    G4_Declare* getSamplerHeader(bool isBindlessSampler);

    void buildTypedSurfaceAddressPayload(G4_SrcRegRegion* u, G4_SrcRegRegion* v, G4_SrcRegRegion* r, G4_SrcRegRegion* lod,
        uint32_t exSize, uint32_t instOpt, payloadSource sources[], uint32_t& len);

    uint32_t getSamplerResponseLength(int numChannels, bool isFP16, int execSize,
        bool pixelNullMask, bool nullDst);

};
}

#endif
