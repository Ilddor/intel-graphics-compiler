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

#include <string>
#include <iostream>
#include <sstream>
#include <fstream>
#include <list>

#include "visa_igc_common_header.h"
#include "Common_ISA.h"
#include "Common_ISA_util.h"
#include "Common_ISA_framework.h"
#ifdef DLL_MODE
#include "RT_Jitter_Interface.h"
#else
#include "JitterDataStruct.h"
#endif
#include "VISAKernel.h"
#include "BuildCISAIR.h"
#include "BinaryCISAEmission.h"
#include "Timer.h"
#include "BinaryEncoding.h"

#include "Gen4_IR.hpp"
#include "FlowGraph.h"
#include "BuildIR.h"
#include "DebugInfo.h"
#include "PatchInfo.h"
#include "PatchInfoWriter.h"

using namespace std;
using namespace vISA;
extern "C" int64_t getTimerTicks(unsigned int idx);

#define IS_GEN_PATH  (mBuildOption == CM_CISA_BUILDER_GEN)
#define IS_BOTH_PATH  (mBuildOption == CM_CISA_BUILDER_BOTH)
#define IS_GEN_BOTH_PATH  (mBuildOption == CM_CISA_BUILDER_GEN || mBuildOption ==  CM_CISA_BUILDER_BOTH)
#define IS_VISA_BOTH_PATH  (mBuildOption == CM_CISA_BUILDER_CISA || mBuildOption ==  CM_CISA_BUILDER_BOTH)

CISA_IR_Builder::~CISA_IR_Builder()
{
    m_cisaBinary->~CisaBinary();

    std::list<VISAKernelImpl *>::iterator iter_start = m_kernels.begin();
    std::list<VISAKernelImpl *>::iterator iter_end = m_kernels.end();

    while (iter_start != iter_end)
    {
        VISAKernelImpl *kernel = *iter_start;
        iter_start++;
        // don't call delete since vISAKernelImpl is allocated in memory pool
        kernel->~VISAKernelImpl();
    }

    if (nativeRelocs)
    {
        nativeRelocs->~NativeRelocs();
    }
}

void CISA_IR_Builder::InitVisaWaTable(TARGET_PLATFORM platform, Stepping step)
{

	if ((platform == GENX_SKL && (step == Step_A || step == Step_B)) ||
		(platform == GENX_BXT && step == Step_A))
	{
		VISA_WA_ENABLE(m_pWaTable, WaHeaderRequiredOnSimd16Sample16bit);
	}
	else
	{
		VISA_WA_DISABLE(m_pWaTable, WaHeaderRequiredOnSimd16Sample16bit);
	}

	if ((platform == GENX_SKL) && (step == Step_A))
	{
		VISA_WA_ENABLE(m_pWaTable, WaSendsSrc1SizeLimitWhenEOT);
	}
	else
	{
		VISA_WA_DISABLE(m_pWaTable, WaSendsSrc1SizeLimitWhenEOT);
	}

	if ((platform == GENX_SKL && (step == Step_A || step == Step_B)) ||
		(platform == GENX_BXT && step == Step_A))
	{
		VISA_WA_ENABLE(m_pWaTable, WaDisallow64BitImmMov);
	}
	else
	{
		VISA_WA_DISABLE(m_pWaTable, WaDisallow64BitImmMov);
	}

	if (platform == GENX_BDW && step == Step_A)
	{
		VISA_WA_ENABLE(m_pWaTable, WaByteDstAlignRelaxedRule);
	}
	else
	{
		VISA_WA_DISABLE(m_pWaTable, WaByteDstAlignRelaxedRule);
	}

	if (platform == GENX_SKL && step == Step_A)
	{
		VISA_WA_ENABLE(m_pWaTable, WaSIMD16SIMD32CallDstAlign);
	}
	else
	{
		VISA_WA_DISABLE(m_pWaTable, WaSIMD16SIMD32CallDstAlign);
	}

	if (platform == GENX_BDW || platform == GENX_CHV ||
		platform == GENX_BXT || platform == GENX_SKL)
	{
		VISA_WA_ENABLE(m_pWaTable, WaThreadSwitchAfterCall);
	}
	else
	{
		VISA_WA_DISABLE(m_pWaTable, WaThreadSwitchAfterCall);
	}

	if ((platform == GENX_SKL && step < Step_E) ||
		(platform == GENX_BXT && step <= Step_B))
	{
		VISA_WA_ENABLE(m_pWaTable, WaSrc1ImmHfNotAllowed);
	}
	else
	{
		VISA_WA_DISABLE(m_pWaTable, WaSrc1ImmHfNotAllowed);
	}

	if (platform == GENX_SKL && step == Step_A)
	{
		VISA_WA_ENABLE(m_pWaTable, WaDstSubRegNumNotAllowedWithLowPrecPacked);
	}
	else
	{
		VISA_WA_DISABLE(m_pWaTable, WaDstSubRegNumNotAllowedWithLowPrecPacked);
	}

	if ((platform == GENX_SKL && step < Step_C))
	{
		VISA_WA_ENABLE(m_pWaTable, WaDisableMixedModeLog);
		VISA_WA_ENABLE(m_pWaTable, WaDisableMixedModeFdiv);
		VISA_WA_ENABLE(m_pWaTable, WaDisableMixedModePow);
	}
	else
	{
		VISA_WA_DISABLE(m_pWaTable, WaDisableMixedModeLog);
		VISA_WA_DISABLE(m_pWaTable, WaDisableMixedModeFdiv);
		VISA_WA_DISABLE(m_pWaTable, WaDisableMixedModePow);
	}


	if ((platform == GENX_SKL && step < Step_C) ||
		platform == GENX_CHV)
	{
		VISA_WA_ENABLE(m_pWaTable, WaFloatMixedModeSelNotAllowedWithPackedDestination);
	}
	else
	{
		VISA_WA_DISABLE(m_pWaTable, WaFloatMixedModeSelNotAllowedWithPackedDestination);
	}

	// always disable in offline mode
	VISA_WA_DISABLE(m_pWaTable, WADisableWriteCommitForPageFault);

	if ((platform == GENX_SKL && step < Step_D) ||
		(platform == GENX_BXT && step == Step_A))
	{
		VISA_WA_ENABLE(m_pWaTable, WaDisableSIMD16On3SrcInstr);
	}

    if (platform == GENX_SKL && (step == Step_C || step == Step_D))
    {
        VISA_WA_ENABLE(m_pWaTable, WaSendSEnableIndirectMsgDesc);
    }
    else
    {
        VISA_WA_DISABLE(m_pWaTable, WaSendSEnableIndirectMsgDesc);
    }

	if (platform == GENX_SKL || platform == GENX_BXT)
	{
		VISA_WA_ENABLE(m_pWaTable, WaClearArfDependenciesBeforeEot);
	}

	if (platform == GENX_SKL && step == Step_A)
	{
		VISA_WA_ENABLE(m_pWaTable, WaDisableSendsSrc0DstOverlap);
	}

    if (platform >= GENX_SKL)
    {
        VISA_WA_ENABLE(m_pWaTable, WaMixModeSelInstDstNotPacked);
    }

    if (m_options.getTarget() == VISA_CM && platform >= GENX_SKL)
    {
        VISA_WA_ENABLE(m_pWaTable, WaDisableSendSrcDstOverlap);
    }

    if (platform >= GENX_CNL)
    {
        VISA_WA_ENABLE(m_pWaTable, WaDisableSendSrcDstOverlap);
    }
    if (platform == GENX_CNL && step == Step_A)
    {
        VISA_WA_ENABLE(m_pWaTable, WaDisableSendsPreemption);
    }
    if (platform == GENX_CNL)
    {
        VISA_WA_ENABLE(m_pWaTable, WaNoSimd16TernarySrc0Imm);
    }

}

// note that this will break if we have more than one builder active,
// since we rely on the pCisaBuilder to point to the current builder
int CISA_IR_Builder::CreateBuilder(
	CISA_IR_Builder *&builder,
	vISABuilderMode mode,
	CM_VISA_BUILDER_OPTION buildOption,
	TARGET_PLATFORM platform,
	int numArgs,
	const char* flags[],
	PVISA_WA_TABLE pWaTable,
	bool initWA)
{

	initTimer();

	if (builder != NULL)
	{
		CmAssert(0);
		return CM_FAILURE;
	}

    startTimer(TIMER_TOTAL);
    startTimer(TIMER_BUILDER);  // builder time ends with we call compile (i.e., it covers the IR construction time)
    //this must be called before any other API.
    SetVisaPlatform(platform);

	// initialize stepping to none in case it's not passed in
	InitStepping();

	builder = new CISA_IR_Builder(buildOption, COMMON_ISA_MAJOR_VER, COMMON_ISA_MINOR_VER, pWaTable);
	pCisaBuilder = builder;

	if (!builder->m_options.parseOptions(numArgs, flags))
	{
		delete builder;
		CmAssert(0);
		return CM_FAILURE;
	}

    builder->m_options.setTarget((mode == vISA_3D) ? VISA_3D : VISA_CM);
    builder->m_options.setOption(vISA_isParseMode, (mode == vISA_PARSER));

	if (mode == vISA_PARSER)
	{
		builder->m_options.setOption(vISA_GeneratevISABInary, true);
		/*
			In parser mode we always want to dump out vISA
			I don't feel like modifying FE, and dealing with FE/BE missmatch issues.
		*/
		builder->m_options.setOption(vISA_DumpvISA, true);
		/*
			Dumping out .asm and .dat files for BOTH mod. Since they are used in
			simulation mode. Again can be pased by FE, but don't want to deal
			with FE/BE miss match issues.
		*/
		if (buildOption != CM_CISA_BUILDER_CISA)
		{
			builder->m_options.setOption(vISA_outputToFile, true);
			builder->m_options.setOption(vISA_GenerateBinary, true);
		}
	}

    // emit location info always for these cases
    if (mode == vISABuilderMode::vISA_MEDIA && builder->m_options.getOption(vISA_outputToFile))
    {
        builder->m_options.setOption(vISA_EmitLocation, true);
    }

	// we must wait till after the options are processed,
	// so that stepping is set and init will work properly
	if (initWA)
	{
		builder->InitVisaWaTable(platform, GetStepping());
	}

    return CM_SUCCESS;
}

int CISA_IR_Builder::DestroyBuilder(CISA_IR_Builder *builder)
{

    if(builder == NULL)
    {
        CmAssert(0);
        return CM_FAILURE;
    }

    delete builder;

    return CM_SUCCESS;
}

bool CISA_IR_Builder::CISA_IR_initialization(char *kernel_name,
                                             int line_no)
{
    m_kernel->InitializeKernel(kernel_name);
    return true;
}

int CISA_IR_Builder::AddKernel(VISAKernel *& kernel, const char* kernelName)
{

    if( kernel != NULL )
    {
        CmAssert( 0 );
        return CM_FAILURE;
    }
    m_executionSatarted = true;

    VISAKernelImpl * kerneltemp = new (m_mem) VISAKernelImpl(mBuildOption, &m_options);
    kernel = static_cast<VISAKernel *>(kerneltemp);
    m_kernel = kerneltemp;
    //m_kernel->setName(kernelName);
    m_kernel->setIsKernel(true);
    m_kernels.push_back(kerneltemp);
    m_kernel->setVersion((unsigned char)this->m_majorVersion, (unsigned char)this->m_minorVersion);
    m_kernel->setPWaTable(m_pWaTable);
    m_kernel->InitializeKernel(kernelName);
    this->m_kernel_count++;

    if(IS_GEN_BOTH_PATH)
    {
        // Append all globals in CISA_IR_Builder instance to new kernel
        unsigned int numFileScopeVars = this->m_cisaBinary->getNumFileVars();

        for( unsigned int i = 0; i < numFileScopeVars; i++ )
        {
            VISA_FileVar* fileScopeVar = this->m_cisaBinary->getFileVar(i);

            kerneltemp->addFileScopeVar(fileScopeVar, i);
        }
    }

    return CM_SUCCESS;
}

int CISA_IR_Builder::AddFunction(VISAFunction *& function, const char* functionName)
{
    if( function != NULL )
    {
        CmAssert( 0 );
        return CM_FAILURE;
    }

    this->AddKernel((VISAKernel *&)function, functionName);

    ((VISAKernelImpl*)function)->m_functionId = this->m_function_count;

    this->m_kernel_count--;
    this->m_function_count++;
    ((VISAKernelImpl *)function)->setIsKernel(false);
    m_functionsVector.push_back(function);
    return CM_SUCCESS;
}

// default size of the physical reg pool mem manager in bytes
#define PHY_REG_MEM_SIZE   (16*1024)

typedef struct fcallState
{
    G4_INST* fcallInst;
    G4_Operand* opnd0;
    G4_Operand* opnd1;
    G4_BB* retBlock;
    unsigned int execSize;
} fcallState;

typedef std::vector<std::pair<G4_Kernel*, fcallState>> savedFCallStates;
typedef savedFCallStates::iterator savedFCallStatesIter;

void saveFCallState(G4_Kernel* kernel, savedFCallStates& savedFCallState)
{
    // Iterate over all BBs in kernel.
    // For each fcall seen, store its opnd0, opnd1, retBlock.
    // so that after compiling the copy of function for 1 kernel,
    // the IR can be reused for another kernel rather than
    // recompiling.
    // kernel points to a stackcall function.
    for( BB_LIST_ITER bb_it = kernel->fg.BBs.begin();
        bb_it != kernel->fg.BBs.end();
        bb_it++ )
    {
        G4_BB* curBB = (*bb_it);

        if( curBB->instList.size() > 0 && curBB->isEndWithFCall() )
        {
            // Save state for this fcall
            G4_INST* fcallInst = curBB->instList.back();

            fcallState currFCallState;

            currFCallState.fcallInst = fcallInst;
            currFCallState.opnd0 = fcallInst->getSrc(0);
            currFCallState.opnd1 = fcallInst->getSrc(1);
            currFCallState.retBlock = curBB->Succs.front();
            currFCallState.execSize = fcallInst->getExecSize();

            savedFCallState.push_back( std::make_pair( kernel, currFCallState ) );
        }
    }
}

void restoreFCallState(G4_Kernel* kernel, savedFCallStates& savedFCallState)
{
    // Iterate over all BBs in kernel and fix all fcalls converted
    // to calls by reconverting them to fcall. This is required
    // because we want to reuse IR of function for next kernel.

    // start, end iterators denote boundaries in vector that correspond
    // to current kernel. This assumes that entries for different
    // functions are not interspersed.
    savedFCallStatesIter start = savedFCallState.begin(), end = savedFCallState.end();

    for( BB_LIST_ITER bb_it = kernel->fg.BBs.begin();
        bb_it != kernel->fg.BBs.end();
        bb_it++ )
    {
        G4_BB* curBB = (*bb_it);

        if( curBB->instList.size() > 0 &&
            curBB->instList.back()->isCall() )
        {
            // Check whether this call is a convert from fcall
            for( savedFCallStatesIter state_it = start;
                state_it != end;
                state_it++ )
            {
                if( (*state_it).second.fcallInst == curBB->instList.back() )
                {
                    // Found a call to replace with fcall and ret with fret

                    // Restore corresponding ret to fret
                    G4_BB* retBlock = (*state_it).second.retBlock;

                    G4_BB* retbbToConvert = retBlock->Preds.back();

                    G4_INST* retToReplace = retbbToConvert->instList.back();

                    retToReplace->setOpcode( G4_pseudo_fret );
                    retToReplace->setDest(NULL);

                    kernel->fg.removePredSuccEdges(retbbToConvert, retBlock);

                    // Now restore call operands
                    G4_INST* instToReplace = curBB->instList.back();

                    auto& state = (*state_it).second;
                    instToReplace->setSrc(state.opnd0, 0);
                    instToReplace->setSrc(state.opnd1, 1);
                    instToReplace->setExecSize((unsigned char)state.execSize);

                    // Remove edge between call and previously joined function
                    while( curBB->Succs.size() > 0 )
                    {
                        kernel->fg.removePredSuccEdges( curBB, curBB->Succs.front() );
                    }

                    // Restore edge to retBlock
                    kernel->fg.addPredSuccEdges( curBB, (*state_it).second.retBlock );

                    instToReplace->setOpcode( G4_pseudo_fcall );
                }
            }
        }
    }

    // Remove all in-edges to stack call function. These may have been added
    // to connect earlier kernels with the function.
    while( kernel->fg.getEntryBB()->Preds.size() > 0 )
    {
        kernel->fg.removePredSuccEdges( kernel->fg.getEntryBB()->Preds.front(), kernel->fg.getEntryBB() );
    }
}


G4_Kernel* Get_Resolved_Compilation_Unit( common_isa_header header, std::list<G4_Kernel*> compilation_units, int idx )
{
    for( std::list<G4_Kernel*>::iterator k = compilation_units.begin();
        k != compilation_units.end(); k++ )
    {
        if( (*k)->fg.builder->getCUnitId() == (header.num_kernels + idx) && (*k)->fg.builder->getIsKernel() == false )
        {
            return (*k);
        }
    }

    return NULL;
}

void Enumerate_Callees( common_isa_header header, G4_Kernel* kernel, std::list<G4_Kernel*> compilation_units, std::list<int>& callees )
{
    for(std::list<int>::iterator it = kernel->fg.builder->callees.begin();
        it != kernel->fg.builder->callees.end();
        it++)
    {
        int cur = (*it);

        bool alreadyAdded = false;

        for( std::list<int>::iterator c = callees.begin();
            c != callees.end();
            c++ )
        {
            if( (*c) == cur )
            {
                alreadyAdded = true;
                break;
            }
        }

        if( alreadyAdded == false )
        {
            callees.push_back( cur );
            G4_Kernel* k = Get_Resolved_Compilation_Unit( header, compilation_units, cur );
            Enumerate_Callees( header, k, compilation_units, callees );
        }
    }
}

// propagate callee JIT info to the kernel
// add more fields as necessary
static void propagateCalleeInfo(G4_Kernel* kernel, G4_Kernel* callee)
{
    if (callee->fg.builder->getJitInfo()->usesBarrier)
    {
        kernel->fg.builder->getJitInfo()->usesBarrier = true;
    }
}

// After compiling each compilation unit this function is invoked which stitches together callers
// with their callees. It modifies pseudo_fcall/fret in to call/ret opcodes.
void Stitch_Compiled_Units( common_isa_header header, std::list<G4_Kernel*> compilation_units )
{
    list <int> callee_index;
    G4_Kernel* kernel = NULL;

    for (std::list<G4_Kernel*>::iterator it = compilation_units.begin();
        it != compilation_units.end();
        it++ )
    {
        G4_Kernel* cur = (*it);

        if( (*it)->fg.builder->getIsKernel() == true )
        {
            ASSERT_USER( kernel == NULL, "Multiple kernel objects found when stitching together");
            kernel = cur;
        }
    }

    ASSERT_USER( kernel != NULL, "Valid kernel not found when stitching compiled units");

    Enumerate_Callees( header, kernel, compilation_units, callee_index );

    callee_index.sort();
    callee_index.unique();

#ifdef _DEBUG
    for( list<int>::iterator it = callee_index.begin(); it != callee_index.end(); ++it ) {
        DEBUG_VERBOSE( *it << " (" << header.functions[*it].name << "), " );
    }
#endif

    // Append flowgraph of all callees to kernel
    for( std::list<int>::iterator it = callee_index.begin();
        it != callee_index.end();
        it++ )
    {
        int cur = (*it);
        G4_Kernel* callee = Get_Resolved_Compilation_Unit( header, compilation_units, cur );
        propagateCalleeInfo(kernel, callee);

        for( BB_LIST_ITER bb = callee->fg.BBs.begin();
            bb != callee->fg.BBs.end();
            bb++ )
        {
            kernel->fg.BBs.push_back( (*bb) );
            kernel->fg.incrementNumBBs();
        }
    }

    kernel->fg.reassignBlockIDs();

    // Change fcall/fret to call/ret and setup caller/callee edges
    for( BB_LIST_ITER it = kernel->fg.BBs.begin();
        it != kernel->fg.BBs.end();
        it++ )
    {
        G4_BB* cur = (*it);

        if( cur->instList.size() > 0 && cur->isEndWithFCall() )
        {
            // Setup successor/predecessor
            G4_INST* fcall = cur->instList.back();
            int calleeIndex = fcall->asCFInst()->getCalleeIndex();
            G4_Kernel* callee = Get_Resolved_Compilation_Unit( header, compilation_units, calleeIndex );
            G4_BB* retBlock = cur->Succs.front();
            ASSERT_USER( cur->Succs.size() == 1, "fcall basic block cannot have more than 1 successor");
            ASSERT_USER( retBlock->Preds.size() == 1, "block after fcall cannot have more than 1 predecessor");

            // Remove old edge
            retBlock->Preds.erase(retBlock->Preds.begin());
            cur->Succs.erase(cur->Succs.begin());

            // Connect new fg
            kernel->fg.addPredSuccEdges( cur, callee->fg.getEntryBB() );
            kernel->fg.addPredSuccEdges( callee->fg.getUniqueReturnBlock(), retBlock );

            G4_INST* calleeLabel = callee->fg.getEntryBB()->instList.front();
            ASSERT_USER( calleeLabel->isLabel() == true, "Entry inst is not label");

            // ret/e-mask
            fcall->setSrc( fcall->getSrc(0), 1 );

            // dst label
            fcall->setSrc( calleeLabel->getSrc(0), 0 );
            fcall->setOpcode( G4_call );
        }
    }

    // Change fret to ret
    for( BB_LIST_ITER it = kernel->fg.BBs.begin();
        it != kernel->fg.BBs.end();
        it++ )
    {
        G4_BB* cur = (*it);

        if( cur->instList.size() > 0 && cur->isEndWithFRet() )
        {
            G4_INST* fret = cur->instList.back();
            ASSERT_USER( fret->opcode() == G4_pseudo_fret, "Expecting to see pseudo_fret");
            fret->setOpcode( G4_return );
            fret->setDest( kernel->fg.builder->createNullDst(Type_UD) );
        }
    }

    // Append declarations and color attributes from all callees to kernel
    for( list<int>::iterator it = callee_index.begin(); it != callee_index.end(); ++it ) {
        G4_Kernel* callee;

        callee = Get_Resolved_Compilation_Unit( header, compilation_units, (*it) );

        for( DECLARE_LIST_ITER dcl_it = callee->Declares.begin(); dcl_it != callee->Declares.end(); ++dcl_it ) {
            G4_Declare* curDcl;
            curDcl = *dcl_it;
            kernel->Declares.push_back( curDcl );
        }
    }
}

void CISA_IR_Builder::emitFCPatchFile()
{
  for(auto K = m_kernels.begin(), E = m_kernels.end(); K != E; K++) {
    VISAKernelImpl *Kernel = *K;
    IR_Builder *Builder = Kernel->getIRBuilder();

    if (Builder) {
      std::string PInfoFPath =
        std::string(Builder->kernel.getName()) + ".fcpatch";
      std::ofstream OFS(PInfoFPath, std::ios::binary | std::ios::out);

      const char *KernelName = Builder->kernel.getName();
      std::vector<std::tuple<unsigned, const char *>> Rels;
      auto &Calls = Builder->getFCPatchInfo()->getFCCallsToPatch();
      for (auto &C : Calls) {
        unsigned Offset = C->callOffset * 16; // Change it back to byte offset.
        Rels.push_back(std::make_tuple(Offset, C->calleeLabelString));
      }


      auto getPatchInfoPlatform = []() -> unsigned {
        switch (getGenxPlatform()) {
        case GENX_BDW:    return cm::patch::PP_BDW;
        case GENX_CHV:    return cm::patch::PP_CHV;
        case GENX_SKL:    return cm::patch::PP_SKL;
        case GENX_BXT:    return cm::patch::PP_BXT;
        default:
          break;
        }
        return cm::patch::PP_NONE;
      };

      writePatchInfo(OFS, getPatchInfoPlatform(), KernelName,
                     Kernel->isFCCallerKernel(), Kernel->isFCCallableKernel(),
                     Rels
                     );
    }
  }
}

// default size of the kernel mem manager in bytes
#define KERNEL_MEM_SIZE    (4*1024*1024)
int CISA_IR_Builder::Compile( const char* nameInput)
{

    stopTimer(TIMER_BUILDER);   // TIMER_BUILDER is started when builder is created
    int status = CM_SUCCESS;

    std::string name = std::string(nameInput);

    if (IS_VISA_BOTH_PATH)
    {

        std::list< VISAKernelImpl *>::iterator iter = m_kernels.begin();
        std::list< VISAKernelImpl *>::iterator end = m_kernels.end();
        CBinaryCISAEmitter cisaBinaryEmitter;
        int kernelIndex = 0;
        if ( IS_BOTH_PATH )
        {
            m_options.setOption(vISA_NumGenBinariesWillBePatched, (uint32_t) 1);
        }
        m_cisaBinary->initCisaBinary(m_kernel_count, m_function_count);
        m_cisaBinary->setMajorVersion((unsigned char)this->m_majorVersion);
        m_cisaBinary->setMinorVersion((unsigned char)this->m_minorVersion);
        m_cisaBinary->setMagicNumber(COMMON_ISA_MAGIC_NUM);

        int status = CM_SUCCESS;
        for( ; iter != end; iter++, kernelIndex++ )
        {
            VISAKernelImpl * kTemp = *iter;
            unsigned int binarySize = 0;
            status = cisaBinaryEmitter.Emit(kTemp, binarySize);
            m_cisaBinary->initKernel(kernelIndex, kTemp);
        }
        m_cisaBinary->finalizeCisaBinary();

        if (status != CM_SUCCESS)
        {
            return status;
        }

        // We call the verifier and dumper directly.
        if (!m_options.getOption(vISA_IsaAssembly) &&
            (m_options.getOption(vISA_GenerateISAASM) ||
             !m_options.getOption(vISA_NoVerifyvISA)))
        {
            m_cisaBinary->isaDumpVerify(m_kernels, &m_options);
        }
    }

    /*
        In case there is an assert in compilation phase, at least vISA binary will be generated.
    */
    if ( IS_VISA_BOTH_PATH && m_options.getOption(vISA_DumpvISA) )
    {
        status = m_cisaBinary->dumpToFile(name);
    }

    if ( IS_GEN_BOTH_PATH )
    {
        Mem_Manager mem(4096);
        common_isa_header pseudoHeader;
        // m_kernels contains kernels and functions to compile.
        std::list< VISAKernelImpl *>::iterator iter = m_kernels.begin();
        std::list< VISAKernelImpl *>::iterator end = m_kernels.end();
        iter = m_kernels.begin();
        end = m_kernels.end();

        pseudoHeader.num_kernels = 0;
        pseudoHeader.num_functions = 0;
        for( ; iter != end; iter++ )
        {
            if( (*iter)->getIsKernel() == true )
            {
                pseudoHeader.num_kernels++;
            }
            else
            {
                pseudoHeader.num_functions++;
            }
        }

        pseudoHeader.functions = (function_info_t*)mem.alloc(sizeof(function_info_t) * pseudoHeader.num_functions);

        int i;
        unsigned int k = 0;
        std::list<G4_Kernel*> compilationUnits;
        std::list<VISAKernelImpl*> kernels;
        std::list<VISAKernelImpl*> functions;
        for( iter = m_kernels.begin(), i = 0; iter != end; iter++, i++ )
        {
            VISAKernelImpl* kernel = (*iter);

            compilationUnits.push_back(kernel->getKernel());

            kernel->setupRelocTable();
            kernel->getIRBuilder()->setIsKernel(kernel->getIsKernel());
            kernel->getIRBuilder()->setCUnitId(i);
            if( kernel->getIsKernel() == false )
            {
                if (kernel->getIRBuilder()->getArgSize() < kernel->getKernelFormat()->input_size)
                {
                    kernel->getIRBuilder()->setArgSize(kernel->getKernelFormat()->input_size);
                }
                if (kernel->getIRBuilder()->getRetVarSize() < kernel->getKernelFormat()->return_value_size)
                {
                    kernel->getIRBuilder()->setRetVarSize(kernel->getKernelFormat()->return_value_size);
                }
                kernel->getIRBuilder()->setFuncId(k);

                strcpy_s((char*)&pseudoHeader.functions[k].name, COMMON_ISA_MAX_FILENAME_LENGTH, (*iter)->getKernel()->getName());
                k++;
                functions.push_back(kernel);
            }
            else
            {
                kernels.push_back(kernel);
            }

            m_currentKernel = kernel;

            int status =  kernel->compileFastPath();
			if (status != CM_SUCCESS)
			{
                stopTimer(TIMER_TOTAL);
				return status;
            }
        }

        savedFCallStates savedFCallState;

        for(std::list<VISAKernelImpl*>::iterator kernel_it = kernels.begin();
            kernel_it != kernels.end();
            kernel_it++)
        {
            VISAKernelImpl* kernel = (*kernel_it);

            saveFCallState(kernel->getKernel(), savedFCallState);
        }

        for( std::list<VISAKernelImpl*>::iterator func_it = functions.begin();
            func_it != functions.end();
            func_it++ )
        {
            VISAKernelImpl* function = (*func_it);

            saveFCallState( function->getKernel(), savedFCallState );
        }

        bool FCPatchNeeded = false;
        for( std::list<VISAKernelImpl*>::iterator kernel_it = kernels.begin();
            kernel_it != kernels.end();
            kernel_it++ )
        {
            VISAKernelImpl* kernel = (*kernel_it);

            m_currentKernel = kernel;
            compilationUnits.clear();
            compilationUnits.push_back( kernel->getKernel() );
            for( std::list<VISAKernelImpl*>::iterator func_it = functions.begin();
                func_it != functions.end();
                func_it++ )
            {
                compilationUnits.push_back( (*func_it)->getKernel() );
                if(m_options.getOption(vISA_GenerateDebugInfo))
                {
                    (*func_it)->getKernel()->getKernelDebugInfo()->resetRelocOffset();
                    resetGenOffsets(*(*func_it)->getKernel());
                }
            }

            unsigned int genxBufferSize = 0;

            Stitch_Compiled_Units(pseudoHeader, compilationUnits);

            void* genxBuffer = kernel->compilePostOptimize(genxBufferSize);
            kernel->setGenxBinaryBuffer(genxBuffer, genxBufferSize);

            if(m_options.getOption(vISA_GenerateDebugInfo))
            {
                kernel->computeAndEmitDebugInfo(functions);
            }

#ifndef DLL_MODE
            if (m_options.getOptionCstr(vISA_RelocFilename))
            {
                // Emit gen reloc information to a file only in offline invocation.
                // In DLL mode return reloc information of the kernel being
                // compiled.
                kernel->computeAndEmitGenRelocs();
            }
#endif

            restoreFCallState( kernel->getKernel(), savedFCallState );

            if(kernel->isFCCallableKernel() ||
                kernel->isFCCallerKernel() ||
                kernel->isFCComposableKernel()) 
            {
                FCPatchNeeded = true;
            }
        }

        // Emit out FC patch file
        if (FCPatchNeeded == true) {
            emitFCPatchFile();
        }
    }

	if (IS_VISA_BOTH_PATH && m_options.getOption(vISA_DumpvISA))
	{
		unsigned int numGenBinariesWillBePatched = m_options.getuInt32Option(vISA_NumGenBinariesWillBePatched);

		if (numGenBinariesWillBePatched)
		{
			std::list< VISAKernelImpl *>::iterator iter = m_kernels.begin();
			std::list< VISAKernelImpl *>::iterator end = m_kernels.end();

			int kernelCount = 0;
			int functionCount = 0;

			//only patch for Both path; vISA path doesn't need this.
			for (int i = 0; iter != end; iter++, i++)
			{
				VISAKernelImpl * kTemp = *iter;
				void * genxBuffer = NULL;
				unsigned int genxBufferSize = 0;
				if (kTemp->getIsKernel())
				{
					genxBuffer = kTemp->getGenxBinaryBuffer();
					genxBufferSize = kTemp->getGenxBinarySize();
					m_cisaBinary->patchKernel(kernelCount, genxBufferSize, genxBuffer, getGenxPlatformEncoding());
					kernelCount++;
				}
				else
				{
					m_cisaBinary->patchFunction(functionCount);
					functionCount++;
				}
			}
		}

		status = m_cisaBinary->dumpToFile(name);
	}

    stopTimer(TIMER_TOTAL); // have to record total time before dump the timer
    if (m_options.getOption(vISA_dumpTimer))
    {
        const char *asmName = nullptr;
        m_options.getOption(VISA_AsmFileName, asmName);
        dumpAllTimers(asmName, true);
    }

    return status;
}

CISA_GEN_VAR * CISA_IR_Builder::getFileVarDeclFromName(const std::string &name)
{
    std::map<std::string, CISA_GEN_VAR *>::iterator it;
    it = m_file_var_name_to_decl_map.find(name);
    if(m_file_var_name_to_decl_map.end() == it)
    {
        return NULL;
    }else
    {
        return it->second;
    }
}

bool CISA_IR_Builder::setFileVarNameDeclMap(const std::string &name, CISA_GEN_VAR * genDecl)
{
    bool succeeded = true;

    //make sure mapping doesn't already exist
    if( getFileVarDeclFromName(name) != NULL )
    {
        return false;
    }
    m_file_var_name_to_decl_map[name] = genDecl;
    return succeeded;
}


bool CISA_IR_Builder::CISA_general_variable_decl(char * var_name,
                                                 unsigned int var_elemts_num,
                                                 VISA_Type data_type,
                                                 VISA_Align var_align,
                                                 char * var_alias_name,
                                                 int var_alias_offset,
                                                 attr_gen_struct scope,
                                                 int line_no)
{
    VISA_GenVar * genVar = NULL;

    VISA_GenVar *parentDecl = NULL;

    if( var_alias_name != NULL && strcmp(var_alias_name, "") != 0 )
    {
        parentDecl = (VISA_GenVar *)m_kernel->getDeclFromName(var_alias_name);

        if( parentDecl == NULL )
        {
            parentDecl = (VISA_GenVar *)this->getFileVarDeclFromName(var_alias_name);
        }
    }

    m_kernel->CreateVISAGenVar(genVar, var_name, var_elemts_num, data_type, var_align, parentDecl, var_alias_offset);

    if( scope.attr_set )
    {
        m_kernel->AddAttributeToVar(genVar, scope.name, 1, &scope.value);
    }

    return true;
}

int CISA_IR_Builder::CreateVISAFileVar(VISA_FileVar *& decl, char *varName, unsigned int numberElements, VISA_Type dataType,
                                       VISA_Align varAlign)
{
    decl = (VISA_FileVar*)m_mem.alloc(sizeof(VISA_FileVar));

    decl->type = FILESCOPE_VAR;
    filescope_var_info_t *file_info = &decl->fileVar;

    size_t len = strlen(varName);
    file_info->bit_properties = dataType;
    file_info->linkage = 2;
    file_info->bit_properties += varAlign << 4;
    file_info->bit_properties += STORAGE_REG << 7;
    file_info->num_elements = (unsigned short)numberElements;
    file_info->attribute_count = 0;
    file_info->attributes = NULL;
    file_info->name = (unsigned char *)m_mem.alloc(len + 1);
    file_info->name_len = (unsigned short) len;
    memcpy_s(file_info->name, len + 1, varName, file_info->name_len+1);
    file_info->scratch = NULL;

    decl->index = this->m_cisaBinary->setFileScopeVar(decl);

    if( IS_GEN_BOTH_PATH )
    {
        // Append file var to all kernel/function objects in CISA_IR_Builder
        for( std::list<VISAKernelImpl*>::iterator it = m_kernels.begin();
            it != m_kernels.end();
            it++ )
        {
            VISAKernelImpl* kernel = (*it);

            kernel->addFileScopeVar(decl, decl->index - 1);
        }
    }

    return CM_SUCCESS;
}

bool CISA_IR_Builder::CISA_file_variable_decl(char * var_name,
                                              unsigned int var_num_elements,
                                              VISA_Type data_type,
                                              VISA_Align var_align,
                                              int line_no)
{
    VISA_FileVar * decl;
    if(getFileVarDeclFromName(var_name) != NULL)
    {
        return true;
    }
    this->CreateVISAFileVar(decl, var_name, var_num_elements, data_type, var_align);
    this->setFileVarNameDeclMap(std::string(var_name), (CISA_GEN_VAR*) decl);
    return true;
}

void CISA_IR_Builder::setupNativeRelocs(unsigned int numRelocs, const BasicRelocEntry* relocs)
{
    for (unsigned int i = 0; i < numRelocs; i++)
    {
        getNativeRelocs()->addEntry(relocs[i].relocOffset, relocs[i].info, relocs[i].addend, 0);
    }
}

void NativeRelocs::addEntry(uint64_t offset, uint64_t info, int64_t addend, unsigned int nativeOffset)
{
    SuperRelocEntry entry;
    entry.input.relocOffset = offset;
    entry.input.info = info;
    entry.input.addend = addend;
    entry.nativeOffset = nativeOffset;
    entries.push_back(entry);
}

bool NativeRelocs::isOffsetReloc(uint64_t offset, SuperRelocEntry& info)
{
    for (auto it : entries)
    {
        if (it.input.relocOffset == offset)
        {
            info = it;
            return true;
        }
    }

    return false;
}

unsigned int NativeRelocs::getNativeOffset(unsigned int cisaOffset)
{
    for (auto it : entries)
    {
        if (it.input.relocOffset == cisaOffset)
        {
            return it.nativeOffset;
        }
    }

#define INVALID_GEN_OFFSET (0xffffffff)

    return INVALID_GEN_OFFSET;
}

// skip all vISA parser functions in DLL mode
#ifndef DLL_MODE
bool CISA_IR_Builder::CISA_addr_variable_decl(char *var_name, unsigned int var_elements, VISA_Type data_type, attr_gen_struct scope, int line_no)
{

    VISA_AddrVar *decl = NULL;
    this->m_kernel->CreateVISAAddrVar(decl, var_name, var_elements);
    if( scope.attr_set )
    {
        m_kernel->AddAttributeToVar(decl, scope.name, 1, &scope.value);
    }
    return true;
}

bool CISA_IR_Builder::CISA_predicate_variable_decl(char *var_name, unsigned int var_elements, attr_gen_struct reg, int line_no)
{
    int reg_id = reg.value;
    char value[2]; // AddAttributeToVar will perform a copy, so we can stack allocate value
    *value = '0'+reg_id;
    value[1] = '\0';

    VISA_PredVar *decl = NULL;
    m_kernel->CreateVISAPredVar(decl, var_name, (unsigned short)var_elements);
    if( reg.attr_set )
    {
        m_kernel->AddAttributeToVar(decl, reg.name, 2, value);
    }
    return true;
}

bool CISA_IR_Builder::CISA_sampler_variable_decl(char *var_name, int num_elts, char* name, int line_no)
{
    VISA_SamplerVar *decl = NULL;
    m_kernel->CreateVISASamplerVar(decl, var_name, num_elts);
    return true;
}

bool CISA_IR_Builder::CISA_surface_variable_decl(char *var_name, int num_elts, char* name, attr_gen_struct attr_val, int line_no)
{
    int reg_id = attr_val.value;
    char * value = (char *) m_mem.alloc(1);
    *value = (char)reg_id;

    VISA_SurfaceVar *decl = NULL;
    m_kernel->CreateVISASurfaceVar(decl, var_name, num_elts);
    if (attr_val.attr_set)
    {
        m_kernel->AddAttributeToVar(decl, attr_val.name, 1, value);
    }
    return true;
}

bool CISA_IR_Builder::CISA_implicit_input_directive(char * argName, char *varName, short offset, unsigned short size, int line_no)
{
    std::string implicitArgName = argName;
    auto pos = implicitArgName.find("UNDEFINED_");
    uint32_t numVal = 0;
    if ( pos!= std::string::npos)
    {
        pos += strlen("UNDEFINED_");
        auto numValString = implicitArgName.substr(pos, implicitArgName.length());
        numVal = std::stoi(numValString);
    }
    else
    {
        auto implicitInputName = implicitArgName.substr(strlen(".implicit_"), implicitArgName.length());
        for (; numVal < IMPLICIT_INPUT_COUNT; ++numVal)
        {
            if (!strcmp(implicitInputName.c_str(), implictKindStrings[numVal]))
            {
                break;
            }
        }
    }

    int status = CM_SUCCESS;
    CISA_GEN_VAR *temp = m_kernel->getDeclFromName(varName);
    MUST_BE_TRUE1(temp != NULL, line_no, "Var marked for input was not found!");
    status = m_kernel->CreateVISAImplicitInputVar((VISA_GenVar *)temp, offset, size, numVal);
    if (status != CM_SUCCESS)
    {
        std::cerr << "Failed to create input Var. Line: " << line_no << std::endl;
        return false;
    }
    return true;
}
bool CISA_IR_Builder::CISA_input_directive(char* var_name, short offset, unsigned short size, int line_no)
{

    int status = CM_SUCCESS;
    CISA_GEN_VAR *temp = m_kernel->getDeclFromName(var_name);
    MUST_BE_TRUE1(temp != NULL, line_no, "Var marked for input was not found!" );
    status = m_kernel->CreateVISAInputVar((VISA_GenVar *)temp,offset,size);
    if(status != CM_SUCCESS)
    {
        std::cerr<<"Failed to create input Var. Line: "<<line_no<<std::endl;
        return false;
    }
    return true;
}

bool CISA_IR_Builder::CISA_attr_directive(char* input_name, char* input_var, int line_no)
{

    if(strcmp(input_name, "AsmName" ) == 0)
    {
        char asmFileName[MAX_OPTION_STR_LENGTH];

        strncpy_s(asmFileName, MAX_OPTION_STR_LENGTH, input_var, MAX_OPTION_STR_LENGTH-1);
        char *pos = strstr(asmFileName, ".asm");
        if (pos != NULL)
        {
            *pos = '\0';
        }
        m_options.setOption(VISA_AsmFileName, asmFileName);
    }

    if(strcmp(input_name, "Target" ) == 0){
        unsigned char visa_target;
        if(strcmp(input_var, "cm" ) == 0)
        {
            visa_target = VISA_CM;
        }
        else if(strcmp(input_var, "3d" ) == 0)
        {
            visa_target = VISA_3D;
        }
        else if(strcmp(input_var, "cs" ) == 0)
        {
            visa_target = VISA_CS;
        }
        else
        {
            MUST_BE_TRUE1(false, line_no, "Invalid kernel target attribute.");
        }
        m_kernel->AddKernelAttribute(input_name, 1, &visa_target);
    }
    else
    {
        m_kernel->AddKernelAttribute(input_name, input_var == nullptr ? 0 : (int)strlen(input_var), input_var);
    }

    return true;
}

bool CISA_IR_Builder::CISA_attr_directiveNum(char* input_name, unsigned char input_var, int line_no)
{
    /*
    attribute_info_t* attr = (attribute_info_t*)m_mem.alloc(sizeof(attribute_info_t));

    attr->value.stringVal = (char *)m_mem.alloc(sizeof(char));
    *attr->value.stringVal = input_var;
    attr->size = sizeof(unsigned char);

    m_kernel->addAttribute(input_name, attr);
    */

    m_kernel->AddKernelAttribute(input_name, sizeof(unsigned char), &input_var);
    return true;
}

bool CISA_IR_Builder::CISA_create_label(char *label_name, int line_no)
{
    VISA_INST_Desc *inst_desc = NULL;
    VISA_LabelOpnd *opnd[1] = {NULL};
    inst_desc = &CISA_INST_table[ISA_LABEL];

    //when we print out ./function from isa we also print out label.
    //if we don't skip it during re-parsing then we will have duplicate labels
    if (m_kernel->getLabelOperandFromFunctionName(std::string(label_name)) == NULL)
    {
        opnd[0] = m_kernel->getLabelOpndFromLabelName(std::string(label_name));
        if (opnd[0] == NULL)
        {
            // forward jump
            m_kernel->CreateVISALabelVar(opnd[0], label_name, LABEL_BLOCK);
        }
        m_kernel->AppendVISACFLabelInst(opnd[0]);
    }

    return true;
}


bool CISA_IR_Builder::CISA_function_directive(char* func_name)
{

    VISA_INST_Desc *inst_desc = NULL;
    VISA_LabelOpnd *opnd[1] = {NULL};
    inst_desc = &CISA_INST_table[ISA_SUBROUTINE];
    opnd[0] = m_kernel->getLabelOperandFromFunctionName(std::string(func_name));
    if (opnd[0] == NULL)
    {
        m_kernel->CreateVISALabelVar(opnd[0], func_name, LABEL_SUBROUTINE);
    }

    m_kernel->AppendVISACFLabelInst(opnd[0]);
    return true;
}


bool CISA_IR_Builder::CISA_create_arith_instruction(VISA_opnd * pred,
                                                    ISA_Opcode opcode,
                                                    bool  sat,
                                                    Common_VISA_EMask_Ctrl emask,
                                                    unsigned exec_size,
                                                    VISA_opnd * dst_cisa,
                                                    VISA_opnd * src0_cisa,
                                                    VISA_opnd * src1_cisa,
                                                    VISA_opnd * src2_cisa,
                                                    int line_no
                                                    )
{
    Common_ISA_Exec_Size executionSize =  Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    int status = m_kernel->AppendVISAArithmeticInst(opcode, (VISA_PredOpnd *)pred, sat, emask, executionSize,
        (VISA_VectorOpnd *)dst_cisa, (VISA_VectorOpnd *)src0_cisa, (VISA_VectorOpnd *)src1_cisa, (VISA_VectorOpnd *)src2_cisa);
    MUST_BE_TRUE1(status == CM_SUCCESS, line_no, "Could not create CISA arithmetic instruction.");
    return true;
}

bool CISA_IR_Builder::CISA_create_arith_instruction2(VISA_opnd * pred,
                                                     ISA_Opcode opcode,
                                                     Common_VISA_EMask_Ctrl emask,
                                                     unsigned exec_size,
                                                     VISA_opnd * dst_cisa,
                                                     VISA_opnd * carry_borrow,
                                                     VISA_opnd * src1_cisa,
                                                     VISA_opnd * src2_cisa,
                                                     int line_no
                                                     )
{
    Common_ISA_Exec_Size executionSize =  Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    int status = m_kernel->AppendVISAArithmeticInst(opcode, (VISA_PredOpnd *)pred, emask, executionSize,
        (VISA_VectorOpnd *)dst_cisa, (VISA_VectorOpnd *)carry_borrow, (VISA_VectorOpnd *)src1_cisa, (VISA_VectorOpnd *)src2_cisa);
    MUST_BE_TRUE1(status == CM_SUCCESS, line_no, "Could not create CISA arithmetic instruction.");
    return true;
}

bool CISA_IR_Builder::CISA_create_mov_instruction(VISA_opnd *pred,
                                                  ISA_Opcode opcode,
                                                  Common_VISA_EMask_Ctrl emask,
                                                  unsigned exec_size,
                                                  bool  sat,
                                                  VISA_opnd *dst,
                                                  VISA_opnd *src0,
                                                  int line_no)
{
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISADataMovementInst(opcode, (VISA_PredOpnd*) pred, sat, emask, executionSize, (VISA_VectorOpnd *)dst, (VISA_VectorOpnd *)src0);
    return true;
}

bool CISA_IR_Builder::CISA_create_mov_instruction(VISA_opnd *dst,
                                                  char *src0_name,
                                                  int line_no)
{
    CISA_GEN_VAR *src0 = m_kernel->getDeclFromName(src0_name);
    MUST_BE_TRUE1(src0 != NULL, line_no, "The source operand of a move instruction was null");
    m_kernel->AppendVISAPredicateMove((VISA_VectorOpnd *)dst, (VISA_PredVar  *)src0);
    return true;
}

bool CISA_IR_Builder::CISA_create_movs_instruction(Common_VISA_EMask_Ctrl emask,
                                                   ISA_Opcode opcode,
                                                   unsigned exec_size,
                                                   VISA_opnd *dst,
                                                   VISA_opnd *src0,
                                                   int line_no)
{
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISADataMovementInst(ISA_MOVS, NULL, false, emask, executionSize, (VISA_VectorOpnd *)dst, (VISA_VectorOpnd *)src0);
    return true;
}

bool CISA_IR_Builder::CISA_create_branch_instruction(VISA_opnd *pred,
                                                     ISA_Opcode opcode,
                                                     Common_VISA_EMask_Ctrl emask,
                                                     unsigned exec_size,
                                                     char *target_label,
                                                     int line_no)
{
    VISA_INST_Desc *inst_desc = NULL;
    VISA_LabelOpnd * opnd[1];
    inst_desc = &CISA_INST_table[opcode];
    int i = 0;

    switch(opcode)
    {
    case ISA_CALL:
        {
            //need second path over instruction stream to
            //determine correct IDs since function directive might not have been
            //encountered yet

            opnd[i] = m_kernel->getLabelOperandFromFunctionName(std::string(target_label));
            if( opnd[i] == NULL )
            {
                m_kernel->CreateVISALabelVar(opnd[i], target_label, LABEL_SUBROUTINE);
                opnd[i]->tag = ISA_SUBROUTINE;
            }
            Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
            m_kernel->AppendVISACFCallInst((VISA_PredOpnd *) pred, emask, executionSize, opnd[i]);
            m_kernel->patchLastInst(opnd[i]);
            return true;
        }
    case ISA_JMP:
        {
            opnd[i] = m_kernel->getLabelOpndFromLabelName(std::string(target_label));

            //forward jump label: create the label optimistically
            if( opnd[i] == NULL )
            {
                m_kernel->CreateVISALabelVar(opnd[i], target_label, LABEL_BLOCK);
            }

            m_kernel->AppendVISACFJmpInst((VISA_PredOpnd *) pred, opnd[i]);
            m_kernel->patchLastInst(opnd[i]);
            return true;
        }
    case ISA_GOTO:
        {

            opnd[i] = m_kernel->getLabelOpndFromLabelName(std::string(target_label));

            //forward jump label: create the label optimistically
            if( opnd[i] == NULL )
            {
                m_kernel->CreateVISALabelVar(opnd[i], target_label, LABEL_BLOCK);
            }
            Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
            m_kernel->AppendVISACFSIMDInst(opcode, (VISA_PredOpnd*)pred, emask, executionSize, opnd[i]);
            m_kernel->patchLastInst(opnd[i]);
            return true;
        }
    default:
        {
            MUST_BE_TRUE(0, "UNKNOWN Branch OP not supported.");
            return false;
        }
    }

    return true;
}

bool CISA_IR_Builder::CISA_create_cmp_instruction(Common_ISA_Cond_Mod sub_op,
                                                  ISA_Opcode opcode,
                                                  Common_VISA_EMask_Ctrl emask,
                                                  unsigned exec_size,
                                                  char *name,
                                                  VISA_opnd *src0,
                                                  VISA_opnd *src1,
                                                  int line_no)
{
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    CISA_GEN_VAR * decl = m_kernel->getDeclFromName(std::string(name));
    m_kernel->AppendVISAComparisonInst(sub_op, emask, executionSize, (VISA_PredVar *)decl, (VISA_VectorOpnd *)src0, (VISA_VectorOpnd *)src1);
    return true;
}

bool CISA_IR_Builder::CISA_create_cmp_instruction(Common_ISA_Cond_Mod sub_op,
                                                  ISA_Opcode opcode,
                                                  Common_VISA_EMask_Ctrl emask,
                                                  unsigned exec_size,
                                                  VISA_opnd *dst,
                                                  VISA_opnd *src0,
                                                  VISA_opnd *src1,
                                                  int line_no)
{
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISAComparisonInst(sub_op, emask, executionSize, (VISA_VectorOpnd *)dst, (VISA_VectorOpnd *)src0, (VISA_VectorOpnd *)src1);
    return true;
}


bool CISA_IR_Builder::CISA_create_media_instruction(ISA_Opcode opcode,
                                                    MEDIA_LD_mod media_mod,
                                                    int block_width,
                                                    int block_height,
                                                    unsigned int plane_ID,
                                                    char * surface_name,
                                                    VISA_opnd *xOffset,
                                                    VISA_opnd *yOffset,
                                                    VISA_opnd *raw_dst,
                                                    int line_no)
{

    unsigned char mod;
    mod = media_mod & 0x7;
    MUST_BE_TRUE1( (mod < MEDIA_LD_Mod_NUM), line_no, "Common ISA ISA_MEDIA_LD uses illegal exec size." );

    VISA_SurfaceVar *surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surface_name);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");

    VISA_StateOpndHandle * surface = NULL;

    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);

    m_kernel->AppendVISASurfAccessMediaLoadStoreInst(opcode, media_mod, surface, (unsigned char)block_width, (unsigned char)block_height,
        (VISA_VectorOpnd *)xOffset, (VISA_VectorOpnd *)yOffset, (VISA_RawOpnd *)raw_dst, (CISA_PLANE_ID)plane_ID);

    return true;
}

/*
For both RET and FRET instructions
*/
bool CISA_IR_Builder::CISA_Create_Ret(VISA_opnd *pred_opnd,
                                      ISA_Opcode opcode,
                                      Common_VISA_EMask_Ctrl emask,
                                      unsigned int exec_size,
                                      int line_no)
{
    if (opcode == ISA_RET)
    {
        Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
        m_kernel->AppendVISACFRetInst((VISA_PredOpnd *)pred_opnd, emask, executionSize);
    }
    else
    {
        Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
        m_kernel->AppendVISACFFunctionRetInst((VISA_PredOpnd *)pred_opnd, emask, executionSize);
    }

    return true;
}

bool CISA_IR_Builder::CISA_create_oword_instruction(ISA_Opcode opcode,
                                                    bool media_mod,
                                                    unsigned int size,
                                                    char *surface_name,
                                                    VISA_opnd *offset_opnd,
                                                    VISA_opnd *raw_dst_src,
                                                    int line_no)
{
    VISA_SurfaceVar *surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surface_name);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");
    VISA_StateOpndHandle * surface = NULL;
    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);
    m_kernel->AppendVISASurfAccessOwordLoadStoreInst(opcode, vISA_EMASK_M1, surface, Get_Common_ISA_Oword_Num_From_Number(size), (VISA_VectorOpnd*)offset_opnd, (VISA_RawOpnd*)raw_dst_src);
    return true;
}

bool CISA_IR_Builder::CISA_create_svm_block_instruction(SVMSubOpcode  subopcode,
                                                        unsigned      owords,
                                                        bool          unaligned,
                                                        VISA_opnd*    address,
                                                        VISA_opnd*    srcDst,
                                                        int           line_no)
{
    switch (subopcode)
    {
    case SVM_BLOCK_LD:
        m_kernel->AppendVISASvmBlockLoadInst(Get_Common_ISA_Oword_Num_From_Number(owords), unaligned, (VISA_VectorOpnd*)address, (VISA_RawOpnd*)srcDst);
        return true;
    case SVM_BLOCK_ST:
        m_kernel->AppendVISASvmBlockStoreInst(Get_Common_ISA_Oword_Num_From_Number(owords), unaligned, (VISA_VectorOpnd*)address, (VISA_RawOpnd*)srcDst);
        return true;
    default:
        return false;
    }

    return false;
}

bool CISA_IR_Builder::CISA_create_svm_scatter_instruction(VISA_opnd*    pred,
                                                          SVMSubOpcode  subopcode,
                                                          Common_VISA_EMask_Ctrl emask,
                                                          unsigned      exec_size,
                                                          unsigned      blockSize,
                                                          unsigned      numBlocks,
                                                          VISA_opnd*    addresses,
                                                          VISA_opnd*    srcDst,
                                                          int           line_no)
{
    Common_ISA_SVM_Block_Type blockType = valueToVISASVMBlockType(blockSize);
    Common_ISA_SVM_Block_Num blockNum = valueToVISASVMBlockNum(numBlocks);
    switch (subopcode)
    {
    case SVM_SCATTER:
        m_kernel->AppendVISASvmScatterInst((VISA_PredOpnd*)pred, emask, Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size),
            blockType, blockNum, (VISA_RawOpnd*)addresses, (VISA_RawOpnd*)srcDst);
        return true;
    case SVM_GATHER:
        m_kernel->AppendVISASvmGatherInst((VISA_PredOpnd*)pred, emask, Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size),
            blockType, blockNum, (VISA_RawOpnd*)addresses, (VISA_RawOpnd*)srcDst);
        return true;
    default:
        return false;
    }


    return false;
}

bool
CISA_IR_Builder::CISA_create_svm_gather4_scaled(VISA_opnd               *pred,
                                                Common_VISA_EMask_Ctrl  eMask,
                                                unsigned                execSize,
                                                ChannelMask             chMask,
                                                VISA_opnd               *address,
                                                VISA_opnd               *offsets,
                                                VISA_opnd               *dst,
                                                int                     lineNum) {
    int ret
        = m_kernel->AppendVISASvmGather4ScaledInst(static_cast<VISA_PredOpnd *>(pred),
                                                   eMask,
                                                   Get_Common_ISA_Exec_Size_From_Raw_Size(execSize),
                                                   chMask.getAPI(),
                                                   static_cast<VISA_VectorOpnd *>(address),
                                                   static_cast<VISA_RawOpnd *>(offsets),
                                                   static_cast<VISA_RawOpnd *>(dst));

    return ret == CM_SUCCESS;
}

bool
CISA_IR_Builder::CISA_create_svm_scatter4_scaled(VISA_opnd              *pred,
                                                 Common_VISA_EMask_Ctrl eMask,
                                                 unsigned               execSize,
                                                 ChannelMask            chMask,
                                                 VISA_opnd              *address,
                                                 VISA_opnd              *offsets,
                                                 VISA_opnd              *src,
                                                 int                    lineNum) {
    int ret
        = m_kernel->AppendVISASvmScatter4ScaledInst(static_cast<VISA_PredOpnd *>(pred),
                                                    eMask,
                                                    Get_Common_ISA_Exec_Size_From_Raw_Size(execSize),
                                                    chMask.getAPI(),
                                                    static_cast<VISA_VectorOpnd *>(address),
                                                    static_cast<VISA_RawOpnd *>(offsets),
                                                    static_cast<VISA_RawOpnd *>(src));

    return ret == CM_SUCCESS;
}

bool CISA_IR_Builder::CISA_create_svm_atomic_instruction(VISA_opnd* pred,
                                                         Common_VISA_EMask_Ctrl emask,
                                                         unsigned   exec_size,
                                                         CMAtomicOperations op,
                                                         bool is16Bit,
                                                         VISA_opnd* addresses,
                                                         VISA_opnd* src0,
                                                         VISA_opnd* src1,
                                                         VISA_opnd* dst,
                                                         int line_no)
{
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISASvmAtomicInst(
        (VISA_PredOpnd *)pred, emask, executionSize, op, is16Bit,
        (VISA_RawOpnd *)addresses, (VISA_RawOpnd *)src0, (VISA_RawOpnd *)src1,
        (VISA_RawOpnd *)dst);
    return true;
}

bool CISA_IR_Builder::CISA_create_address_instruction(ISA_Opcode opcode,
                                                      Common_VISA_EMask_Ctrl emask,
                                                      unsigned exec_size,
                                                      VISA_opnd *dst,
                                                      VISA_opnd *src0,
                                                      VISA_opnd *src1,
                                                      int line_no)
{
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISAAddrAddInst(emask, executionSize, (VISA_VectorOpnd *)dst, (VISA_VectorOpnd *)src0, (VISA_VectorOpnd *)src1);
    return true;
}

bool CISA_IR_Builder::CISA_create_logic_instruction(VISA_opnd *pred,
                                                    ISA_Opcode opcode,
                                                    bool sat,
                                                    Common_VISA_EMask_Ctrl emask,
                                                    unsigned exec_size,
                                                    VISA_opnd *dst,
                                                    VISA_opnd *src0,
                                                    VISA_opnd *src1,
                                                    VISA_opnd *src2,
                                                    VISA_opnd *src3,
                                                    int line_no)
{
    if( opcode != ISA_SHR &&
        opcode != ISA_SHL &&
        opcode != ISA_ASR )
    {
        MUST_BE_TRUE1(!sat, line_no, "Saturation mode is not supported for this Logic Opcode." );
    }

    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISALogicOrShiftInst(opcode, (VISA_PredOpnd *)pred, sat, emask, executionSize, (VISA_VectorOpnd *)dst, (VISA_VectorOpnd *)src0, (VISA_VectorOpnd *)src1,
        (VISA_VectorOpnd *)src2, (VISA_VectorOpnd *)src3);
    return true;
}

bool CISA_IR_Builder::CISA_create_logic_instruction(ISA_Opcode opcode,
                                                    Common_VISA_EMask_Ctrl emask,
                                                    unsigned exec_size,
                                                    char *dst_name,
                                                    char *src0_name,
                                                    char *src1_name,
                                                    int line_no)
{
    if( opcode != ISA_AND &&
        opcode != ISA_OR  &&
        opcode != ISA_NOT &&
        opcode != ISA_XOR )
    {
        MUST_BE_TRUE1(false, line_no, "Prediate variables are not supported for this Logic Opcode." );
    }
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    CISA_GEN_VAR *dst = m_kernel->getDeclFromName(dst_name);
    MUST_BE_TRUE1(dst != NULL, line_no, "The destination operand of a logical instruction was null");
    CISA_GEN_VAR *src0 = m_kernel->getDeclFromName(src0_name);
    MUST_BE_TRUE1(src0 != NULL, line_no, "The first source operand of a logical instruction was null");
    CISA_GEN_VAR *src1 = NULL;
    if ( opcode != ISA_NOT )
    {
        src1 = m_kernel->getDeclFromName(src1_name);
        MUST_BE_TRUE1(src1 != NULL, line_no, "The second source operand of a logical instruction was null");
    }
    m_kernel->AppendVISALogicOrShiftInst(opcode, emask, executionSize, (VISA_PredVar *)dst, (VISA_PredVar *)src0, (VISA_PredVar *)src1);
    return true;
}

bool CISA_IR_Builder::CISA_create_math_instruction(VISA_opnd *pred,
                                                   ISA_Opcode opcode,
                                                   bool  sat,
                                                   Common_VISA_EMask_Ctrl emask,
                                                   unsigned exec_size,
                                                   VISA_opnd *dst,
                                                   VISA_opnd *src0,
                                                   VISA_opnd *src1,
                                                   int line_no)
{
    Common_ISA_Exec_Size executionSize =  Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISAArithmeticInst(opcode, (VISA_PredOpnd *)pred, sat, emask, executionSize, (VISA_VectorOpnd *)dst, (VISA_VectorOpnd *)src0, (VISA_VectorOpnd *)src1, NULL);
    return true;
}

bool CISA_IR_Builder::CISA_create_setp_instruction(ISA_Opcode opcode,
                                                   Common_VISA_EMask_Ctrl emask,
                                                   unsigned exec_size,
                                                   char * var_name,
                                                   VISA_opnd *src0,
                                                   int line_no)
{
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    CISA_GEN_VAR *dst = m_kernel->getDeclFromName(var_name);
    m_kernel->AppendVISASetP(emask, executionSize, (VISA_PredVar *)dst, (VISA_VectorOpnd *)src0);
    return true;
}

bool CISA_IR_Builder::CISA_create_sel_instruction(ISA_Opcode opcode,
                                                  bool sat,
                                                  VISA_opnd *pred,
                                                  Common_VISA_EMask_Ctrl emask,
                                                  unsigned exec_size,
                                                  VISA_opnd *dst,
                                                  VISA_opnd *src0,
                                                  VISA_opnd *src1,
                                                  int line_no)
{
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISADataMovementInst(opcode, (VISA_PredOpnd*)pred, sat, emask, executionSize, (VISA_VectorOpnd *)dst, (VISA_VectorOpnd *)src0, (VISA_VectorOpnd *)src1);
    return true;
}

bool CISA_IR_Builder::CISA_create_fminmax_instruction(bool minmax,
                                                      ISA_Opcode opcode,
                                                      bool sat,
                                                      VISA_opnd *pred,
                                                      Common_VISA_EMask_Ctrl emask,
                                                      unsigned exec_size,
                                                      VISA_opnd *dst,
                                                      VISA_opnd *src0,
                                                      VISA_opnd *src1,
                                                      int line_no)
{
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISAMinMaxInst((minmax ? CISA_DM_FMAX : CISA_DM_FMIN), sat, emask, executionSize, (VISA_VectorOpnd *)dst, (VISA_VectorOpnd *)src0, (VISA_VectorOpnd *)src1);
    return true;
}

bool CISA_IR_Builder::CISA_create_scatter_instruction(ISA_Opcode opcode,
                                                      int elt_size,
                                                      Common_VISA_EMask_Ctrl emask,
                                                      unsigned elemNum,
                                                      bool modifier,
                                                      char *surface_name,
                                                      VISA_opnd *global_offset, //global_offset
                                                      VISA_opnd *element_offset, //element_offset
                                                      VISA_opnd *raw_dst_src, //dst/src
                                                      int line_no)
{
    //GATHER  0x39 (GATHER)  Elt_size   Is_modified Num_elts    Surface Global_Offset   Element_Offset  Dst
    //SCATTER 0x3A (SCATTER) Elt_size               Num_elts    Surface Global_Offset   Element_Offset  Src
    VISA_SurfaceVar *surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surface_name);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");

    VISA_StateOpndHandle * surface = NULL;

    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);

    MUST_BE_TRUE1(elemNum == 16 || elemNum == 8 || elemNum == 1, line_no, "Unsupported number of elements for gather/scatter instruction.");

    Common_ISA_Exec_Size executionSize = EXEC_SIZE_16;

    if(elemNum == 16)
    {
        executionSize = EXEC_SIZE_16;
    }
    else if(elemNum == 8)
    {
        executionSize = EXEC_SIZE_8;
    }
    else if(elemNum == 1)
    {
        executionSize = EXEC_SIZE_1;
    }

    GATHER_SCATTER_ELEMENT_SIZE elementSize = GATHER_SCATTER_BYTE_UNDEF;
    if(elt_size == 1)
    {
        elementSize = GATHER_SCATTER_BYTE;
    }else if( elt_size == 2)
    {
        elementSize = GATHER_SCATTER_WORD;
    }else if(elt_size == 4)
    {
        elementSize = GATHER_SCATTER_DWORD;
    }

    m_kernel->AppendVISASurfAccessGatherScatterInst(opcode, emask, elementSize, executionSize, surface, (VISA_VectorOpnd *)global_offset, (VISA_RawOpnd *)element_offset, (VISA_RawOpnd *)raw_dst_src);
    return true;
}

bool CISA_IR_Builder::CISA_create_scatter4_instruction(ISA_Opcode opcode,
                                                       ChannelMask ch_mask,
                                                       bool mod,
                                                       Common_VISA_EMask_Ctrl emask,
                                                       int elemNum,
                                                       char *surf_name,
                                                       VISA_opnd *global_offset, //global_offset
                                                       VISA_opnd *element_offset, //element_offset
                                                       VISA_opnd *raw_dst_src, //dst/src
                                                       int line_no)
{
    VISA_SurfaceVar *surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surf_name);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");

    VISA_StateOpndHandle * surface = NULL;

    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);

    Common_ISA_Exec_Size executionSize = EXEC_SIZE_16;
    if(elemNum == 16)
    {
        executionSize = EXEC_SIZE_16;
    }
    else if(elemNum == 8)
    {
        executionSize = EXEC_SIZE_8;
    }
    m_kernel->AppendVISASurfAccessGather4Scatter4Inst(opcode, ch_mask.getAPI(), emask, executionSize, surface, (VISA_VectorOpnd *)global_offset, (VISA_RawOpnd *)element_offset, (VISA_RawOpnd *)raw_dst_src);
    return true;
}

bool CISA_IR_Builder::CISA_create_scatter4_typed_instruction(ISA_Opcode opcode,
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
                                                             int line_no)
{
    VISA_SurfaceVar *surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surfaceName);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");

    VISA_StateOpndHandle * surface = NULL;
    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(execSize);
    m_kernel->AppendVISASurfAccessGather4Scatter4TypedInst(opcode, (VISA_PredOpnd *)pred, ch_mask.getAPI(), emask, executionSize, surface, (VISA_RawOpnd *)uOffset, (VISA_RawOpnd *)vOffset, (VISA_RawOpnd *)rOffset, (VISA_RawOpnd *)lod, (VISA_RawOpnd*)dst);
    return true;
}

bool CISA_IR_Builder::CISA_create_scatter4_scaled_instruction(ISA_Opcode                opcode,
                                                              VISA_opnd                 *pred,
                                                              Common_VISA_EMask_Ctrl    eMask,
                                                              unsigned                  execSize,
                                                              ChannelMask               chMask,
                                                              char                      *surfaceName,
                                                              VISA_opnd                 *globalOffset,
                                                              VISA_opnd                 *offsets,
                                                              VISA_opnd                 *dstSrc,
                                                              int                       lineNo)
{
    VISA_SurfaceVar *surfaceVar =
        (VISA_SurfaceVar*)m_kernel->getDeclFromName(surfaceName);
    MUST_BE_TRUE1(surfaceVar != NULL, lineNo, "Surface was not found");

    VISA_StateOpndHandle *surface = NULL;
    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);

    int ret = m_kernel->AppendVISASurfAccessGather4Scatter4ScaledInst(
                opcode, static_cast<VISA_PredOpnd *>(pred),
                eMask, Get_Common_ISA_Exec_Size_From_Raw_Size(execSize),
                chMask.getAPI(),
                surface,
                static_cast<VISA_VectorOpnd *>(globalOffset),
                static_cast<VISA_RawOpnd *>(offsets),
                static_cast<VISA_RawOpnd *>(dstSrc));

    return ret == CM_SUCCESS;
}

bool CISA_IR_Builder::CISA_create_strbuf_scaled_instruction(ISA_Opcode              opcode,
                                                            VISA_opnd               *pred,
                                                            Common_VISA_EMask_Ctrl  eMask,
                                                            unsigned                execSize,
                                                            ChannelMask             chMask,
                                                            char                    *surfaceName,
                                                            VISA_opnd               *uOffsets,
                                                            VISA_opnd               *vOffsets,
                                                            VISA_opnd               *dstSrc,
                                                            int                     lineNo)
{
    VISA_SurfaceVar *surfaceVar =
        (VISA_SurfaceVar*)m_kernel->getDeclFromName(surfaceName);
    MUST_BE_TRUE1(surfaceVar != NULL, lineNo, "Surface was not found");

    VISA_StateOpndHandle *surface = NULL;
    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);

    int ret = m_kernel->AppendVISASurfAccessStrBufLdStInst(
                opcode, static_cast<VISA_PredOpnd *>(pred),
                eMask, Get_Common_ISA_Exec_Size_From_Raw_Size(execSize),
                chMask.getAPI(),
                surface,
                static_cast<VISA_RawOpnd *>(uOffsets),
                static_cast<VISA_RawOpnd *>(vOffsets),
                static_cast<VISA_RawOpnd *>(dstSrc));

    return ret == CM_SUCCESS;
}

bool CISA_IR_Builder::CISA_create_scatter_scaled_instruction(ISA_Opcode             opcode,
                                                             VISA_opnd              *pred,
                                                             Common_VISA_EMask_Ctrl eMask,
                                                             unsigned               execSize,
                                                             unsigned               numBlocks,
                                                             char                   *surfaceName,
                                                             VISA_opnd              *globalOffset,
                                                             VISA_opnd              *offsets,
                                                             VISA_opnd              *dstSrc,
                                                             int                    lineNo)
{
    VISA_SurfaceVar *surfaceVar =
        (VISA_SurfaceVar*)m_kernel->getDeclFromName(surfaceName);
    MUST_BE_TRUE1(surfaceVar != NULL, lineNo, "Surface was not found");

    VISA_StateOpndHandle *surface = NULL;
    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);

    int ret = m_kernel->AppendVISASurfAccessScatterScaledInst(
                opcode, static_cast<VISA_PredOpnd *>(pred),
                eMask, Get_Common_ISA_Exec_Size_From_Raw_Size(execSize),
                valueToVISASVMBlockNum(numBlocks),
                surface,
                static_cast<VISA_VectorOpnd *>(globalOffset),
                static_cast<VISA_RawOpnd *>(offsets),
                static_cast<VISA_RawOpnd *>(dstSrc));

    return ret == CM_SUCCESS;
}

bool CISA_IR_Builder::CISA_create_sync_instruction(ISA_Opcode opcode)
{
    VISA_INST_Desc *inst_desc = NULL;
    inst_desc = &CISA_INST_table[opcode];

    CisaFramework::CisaInst * inst = new(m_mem)CisaFramework::CisaInst(m_mem);

    inst->createCisaInstruction(opcode, EXEC_SIZE_1, 0 , 0 ,NULL, 0, inst_desc);
    m_kernel->addInstructionToEnd(inst);
    return true;
}

bool CISA_IR_Builder::CISA_create_pbarrier_instruction(VISA_opnd *mask, VISA_opnd *dst) {
    int ret = m_kernel
        ->AppendVISAPredBarrierInst(static_cast<VISA_VectorOpnd *>(mask),
                                    static_cast<VISA_RawOpnd *>(dst));
    return ret == CM_SUCCESS;
}

bool CISA_IR_Builder::CISA_create_FILE_instruction(ISA_Opcode opcode, char * file_name)
{
    m_kernel->AppendVISAMiscFileInst(file_name);
    return true;
}

bool CISA_IR_Builder::CISA_create_LOC_instruction(ISA_Opcode opcode, unsigned int loc)
{
    m_kernel->AppendVISAMiscLOC(loc);
    return true;
}

bool CISA_IR_Builder::CISA_create_invtri_inst(VISA_opnd *pred,
                                              ISA_Opcode opcode,
                                              bool  sat,
                                              Common_VISA_EMask_Ctrl emask,
                                              unsigned exec_size,
                                              VISA_opnd *dst,
                                              VISA_opnd *src0,
                                              int line_no)
{
    int num_operands = 0;
    VISA_INST_Desc *inst_desc = NULL;
    VISA_opnd *opnd[4];
    inst_desc = &CISA_INST_table[opcode];
    VISA_Modifier mod = MODIFIER_NONE;

    if(sat)
        mod = MODIFIER_SAT;

    if(dst != NULL)
    {
        dst->_opnd.v_opnd.tag += mod<<3;
        opnd[num_operands] = dst;
        num_operands ++;
    }

    if(src0 != NULL)
    {
        opnd[num_operands] = src0;
        num_operands ++;
    }

    //pred id
    unsigned short pred_id = 0;
    if (pred != NULL)
        pred_id = pred->_opnd.v_opnd.opnd_val.pred_opnd.index;

    CisaFramework::CisaInst * inst = new(m_mem)CisaFramework::CisaInst(m_mem);

    unsigned char size = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    size += emask << 4;
    inst->createCisaInstruction(opcode, size, 0 , pred_id,opnd, num_operands, inst_desc);
    m_kernel->addInstructionToEnd(inst);

    return true;
}

bool CISA_IR_Builder::CISA_create_atomic_instruction (ISA_Opcode opcode,
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
                                                      int line_no)
{

    VISA_SurfaceVar *surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surface_name);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");

    VISA_StateOpndHandle * surface = NULL;

    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);

    Common_ISA_Exec_Size executionSize = EXEC_SIZE_8;

    MUST_BE_TRUE1(execSize == 8 || execSize == 16, line_no, "Unsupported number of elements for atomic instruction.");
    if (execSize == 8)
    {
        executionSize = EXEC_SIZE_8;
    }
    else if (execSize == 16)
    {
        executionSize = EXEC_SIZE_16;
    }

    m_kernel->AppendVISASurfAccessDwordAtomicInst(
        sub_op, is16Bit, emask, executionSize, surface,
        (VISA_VectorOpnd *)g_off, (VISA_RawOpnd *)elem_opnd,
        (VISA_RawOpnd *)src0, (VISA_RawOpnd *)src1, (VISA_RawOpnd *)dst);

    return true;
}

bool CISA_IR_Builder::CISA_create_dword_atomic_instruction(VISA_opnd *pred,
                                                           CMAtomicOperations subOpc,
                                                           bool is16Bit,
                                                           Common_VISA_EMask_Ctrl eMask,
                                                           unsigned execSize,
                                                           char *surfaceName,
                                                           VISA_opnd *offsets,
                                                           VISA_opnd *src0,
                                                           VISA_opnd *src1,
                                                           VISA_opnd *dst,
                                                           int lineNo) {
    VISA_SurfaceVar *surfaceVar =
        (VISA_SurfaceVar*)m_kernel->getDeclFromName(surfaceName);
    MUST_BE_TRUE1(surfaceVar != NULL, lineNo, "Surface was not found");

    VISA_StateOpndHandle *surface = NULL;
    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);

    int ret =
        m_kernel->AppendVISASurfAccessDwordAtomicInst(
                static_cast<VISA_PredOpnd *>(pred),
                subOpc,
                is16Bit,
                eMask, Get_Common_ISA_Exec_Size_From_Raw_Size(execSize),
                surface,
                static_cast<VISA_RawOpnd *>(offsets),
                static_cast<VISA_RawOpnd *>(src0),
                static_cast<VISA_RawOpnd *>(src1),
                static_cast<VISA_RawOpnd *>(dst));

    return ret == CM_SUCCESS;
}

bool CISA_IR_Builder::CISA_create_typed_atomic_instruction(VISA_opnd *pred,
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
    int lineNo)
{
    VISA_SurfaceVar *surfaceVar =
        (VISA_SurfaceVar*)m_kernel->getDeclFromName(surfaceName);
    MUST_BE_TRUE1(surfaceVar != nullptr, lineNo, "Surface was not found");

    VISA_StateOpndHandle *surface = nullptr;
    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);

    int ret =
        m_kernel->AppendVISA3dTypedAtomic(
        subOpc,
        is16Bit,
        static_cast<VISA_PredOpnd *>(pred),
        eMask, Get_Common_ISA_Exec_Size_From_Raw_Size(execSize),
        surface,
        static_cast<VISA_RawOpnd *>(u),
        static_cast<VISA_RawOpnd *>(v),
        static_cast<VISA_RawOpnd *>(r),
        static_cast<VISA_RawOpnd *>(lod),
        static_cast<VISA_RawOpnd *>(src0),
        static_cast<VISA_RawOpnd *>(src1),
        static_cast<VISA_RawOpnd *>(dst));

    return ret == CM_SUCCESS;
}


bool CISA_IR_Builder::CISA_create_SIMD_CF_instruction(VISA_opnd *pred,
                                                      ISA_Opcode opcode,
                                                      Common_VISA_EMask_Ctrl emask,
                                                      unsigned exec_size,
                                                      int line_no)
{
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISACFSIMDInst(opcode, (VISA_PredOpnd*) pred, emask, executionSize);
    return true;
}

bool CISA_IR_Builder::CISA_create_avs_instruction(ChannelMask channel,
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
                                                  int line_no)
{
    VISA_SurfaceVar *surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surface_name);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");

    VISA_StateOpndHandle * surface = NULL;
    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);

    VISA_VMEVar *samplerVar = (VISA_VMEVar *)m_kernel->getDeclFromName(sampler_name);
    MUST_BE_TRUE1(samplerVar != NULL, line_no, "Sampler was not found");

    VISA_StateOpndHandle *sampler = NULL;
    m_kernel->CreateVISAStateOperandHandle(sampler, samplerVar);
    m_kernel->AppendVISAMEAVS(surface, sampler, channel.getAPI(), (VISA_VectorOpnd *)u_offset, (VISA_VectorOpnd *)v_offset, (VISA_VectorOpnd *)deltaU,
                (VISA_VectorOpnd *)deltaV, (VISA_VectorOpnd *)u2d, (VISA_VectorOpnd *)v2d, (VISA_VectorOpnd *)groupID, (VISA_VectorOpnd *)verticalBlockNumber, cntrl,
                execMode, (VISA_VectorOpnd *)iefbypass, (VISA_RawOpnd *)dst);
    return true;
}

bool CISA_IR_Builder::CISA_create_urb_write_3d_instruction(VISA_opnd* pred,
                                                           Common_VISA_EMask_Ctrl emask,
                                                           unsigned exec_size,
                                                           unsigned int num_out,
                                                           unsigned int global_offset,
                                                           VISA_opnd* channel_mask,
                                                           VISA_opnd* urb_handle,
                                                           VISA_opnd* per_slot_offset,
                                                           VISA_opnd* vertex_data,
                                                           int line_no)
{
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISA3dURBWrite( (VISA_PredOpnd*)pred, emask, executionSize, (unsigned char)num_out, (VISA_RawOpnd*) channel_mask, (unsigned short)global_offset, (VISA_RawOpnd*)urb_handle, (VISA_RawOpnd*)per_slot_offset, (VISA_RawOpnd*)vertex_data );
    return true;
}

bool CISA_IR_Builder::CISA_create_rtwrite_3d_instruction(VISA_opnd* pred,
                                                         char* mode,
                                                         Common_VISA_EMask_Ctrl emask,
                                                         unsigned exec_size,
                                                         char* surface_name,
                                                         const std::vector<VISA_opnd*> &operands,
                                                         int line_no)
{
    vISA_RT_CONTROLS cntrls;

    memset(&cntrls, 0, sizeof(vISA_RT_CONTROLS));

    VISA_opnd* s0a              = NULL;
    VISA_opnd* oM               = NULL;
    VISA_opnd* R                = NULL;
    VISA_opnd* G                = NULL;
    VISA_opnd* B                = NULL;
    VISA_opnd* A                = NULL;
    VISA_opnd* Z                = NULL;
    VISA_opnd* Stencil          = NULL;
    VISA_opnd *CPSCounter =  NULL;
    VISA_opnd *SamplerIndex = NULL;
    VISA_opnd *r1Header = NULL;
    VISA_opnd *rti = NULL;
    uint8_t counter = 0;

    r1Header = operands[counter++];

    if( mode != NULL )
    {
        if( strstr( mode, "<SI>" ) )
        {
            SamplerIndex = operands[counter++];
        }

        if( strstr( mode, "<CPS>" ) )
        {
            CPSCounter = operands[counter++];
        }

        if(strstr(mode, "<RTI>"))
        {
            cntrls.RTIndexPresent = true;
            rti = operands[counter++];
        }

        if( strstr( mode, "<A>" ) )
        {
            cntrls.s0aPresent = true;
            s0a = operands[counter++];
        }

        if( strstr( mode, "<O>" ) )
        {
            cntrls.oMPresent = true;
            oM = operands[counter++];
        }
        R = operands[counter++];
        G = operands[counter++];
        B = operands[counter++];
        A = operands[counter++];

        if( strstr( mode, "<Z>" ) )
        {
            cntrls.zPresent = true;
            Z = operands[counter++];
        }

        if( strstr( mode, "<ST>" ) )
        {
            Stencil = operands[counter++];
        }

        if( strstr( mode, "<LRTW>" ) )
        {
            cntrls.isLastWrite = true;

        }

        if( strstr( mode, "<PS>" ) )
        {
            cntrls.isPerSample = true;
        }

        if( strstr( mode, "CM" ) )
        {
            cntrls.isCoarseMode = true;
        }
    }
    else
    {
        R = operands[counter++];
        G = operands[counter++];
        B = operands[counter++];
        A = operands[counter++];
    }


    VISA_SurfaceVar *surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surface_name);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");

    VISA_StateOpndHandle * surface = NULL;
    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);

    uint8_t numMsgSpecificOpnd = 0;
    VISA_RawOpnd* rawOpnds[20];

#define APPEND_NON_NULL_RAW_OPND( opnd ) \
    if( opnd != NULL )  \
    { \
    rawOpnds[numMsgSpecificOpnd++] = (VISA_RawOpnd*)opnd; \
    }

    APPEND_NON_NULL_RAW_OPND( s0a );
    APPEND_NON_NULL_RAW_OPND( oM );
    APPEND_NON_NULL_RAW_OPND( R );
    APPEND_NON_NULL_RAW_OPND( G );
    APPEND_NON_NULL_RAW_OPND( B );
    APPEND_NON_NULL_RAW_OPND( A );
    APPEND_NON_NULL_RAW_OPND( Z );
    APPEND_NON_NULL_RAW_OPND( Stencil );
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISA3dRTWriteCPS((VISA_PredOpnd*)pred, emask, executionSize, (VISA_VectorOpnd*)rti, 
        cntrls, surface, (VISA_RawOpnd*)r1Header, (VISA_VectorOpnd*)SamplerIndex, (VISA_VectorOpnd*)CPSCounter, numMsgSpecificOpnd, rawOpnds);

    return true;
}


bool CISA_IR_Builder::CISA_create_info_3d_instruction(VISASampler3DSubOpCode subOpcode,
                                                      Common_VISA_EMask_Ctrl emask,
                                                      unsigned exec_size,
                                                      ChannelMask channel,
                                                      char* surface_name,
                                                      VISA_opnd* lod,
                                                      VISA_opnd* dst,
                                                      int line_no)
{
    VISA_SurfaceVar* surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surface_name);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");

    VISA_StateOpndHandle* surface = NULL;
    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISA3dInfo(subOpcode, emask, executionSize, channel.getAPI(), surface, (VISA_RawOpnd*)lod, (VISA_RawOpnd*)dst);
    return true;
}

bool CISA_IR_Builder::createSample4Instruction(VISA_opnd* pred,
        VISASampler3DSubOpCode subOpcode,
        bool pixelNullMask,
        ChannelMask channel,
        Common_VISA_EMask_Ctrl emask,
        unsigned exec_size,
        VISA_opnd* aoffimmi,
        char* sampler_name,
        char* surface_name,
        VISA_opnd* dst,
        unsigned int numParameters,
        VISA_RawOpnd** params,
        int line_no)
{
    VISA_SurfaceVar *surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surface_name);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");

    VISA_StateOpndHandle *surface = NULL;

    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);

    VISA_SamplerVar *samplerVar = (VISA_SamplerVar*)m_kernel->getDeclFromName(sampler_name);
    MUST_BE_TRUE1(samplerVar != NULL, line_no, "Sampler was not found");

    VISA_StateOpndHandle * sampler = NULL;
    m_kernel->CreateVISAStateOperandHandle(sampler, samplerVar);

    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);

    MUST_BE_TRUE(channel.getNumEnabledChannels() == 1, "Only one of R,G,B,A may be specified for sample4 instruction");
    m_kernel->AppendVISA3dGather4(subOpcode, pixelNullMask, (VISA_PredOpnd*)pred, emask,
                                  executionSize, channel.getSingleChannel(), (VISA_VectorOpnd*) aoffimmi,
                                  sampler, surface, (VISA_RawOpnd*) dst, numParameters, params);
    return true;
}


bool CISA_IR_Builder::create3DLoadInstruction(VISA_opnd* pred,
                                              VISASampler3DSubOpCode subOpcode,
                                              bool pixelNullMask,
                                              ChannelMask channels,
                                              Common_VISA_EMask_Ctrl emask,
                                              unsigned exec_size,
                                              VISA_opnd *aoffimmi,
                                              char* surface_name,
                                              VISA_opnd* dst,
                                              unsigned int numParameters,
                                              VISA_RawOpnd** params,
                                              int line_no)
{

    VISA_SurfaceVar *surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surface_name);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");

    VISA_StateOpndHandle * surface = NULL;
    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);

    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISA3dLoad(subOpcode, pixelNullMask, (VISA_PredOpnd*)pred, emask,
                               executionSize, channels.getAPI(), (VISA_VectorOpnd*) aoffimmi,
                               surface, (VISA_RawOpnd*)dst, numParameters, params);
    return true;
}

bool CISA_IR_Builder::create3DSampleInstruction(VISA_opnd* pred,
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
                                                int line_no)
{
    VISA_SurfaceVar *surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surface_name);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");

    VISA_StateOpndHandle * surface = NULL;
    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);

    VISA_SamplerVar *samplerVar = (VISA_SamplerVar*)m_kernel->getDeclFromName(sampler_name);
    MUST_BE_TRUE1(samplerVar != NULL, line_no, "Sampler was not found");

    VISA_StateOpndHandle * sampler = NULL;
    m_kernel->CreateVISAStateOperandHandle(sampler, samplerVar);

    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISA3dSampler(subOpcode, pixelNullMask, cpsEnable, uniformSampler, (VISA_PredOpnd*)pred, emask,
                                  executionSize, channels.getAPI(), (VISA_VectorOpnd*) aoffimmi,
                                  sampler, surface, (VISA_RawOpnd*)dst, numParameters, params);
    return true;

}

bool CISA_IR_Builder::CISA_create_sample_instruction (ISA_Opcode opcode,
                                                      ChannelMask channel,
                                                      int simd_mode,
                                                      char* sampler_name,
                                                      char* surface_name,
                                                      VISA_opnd *u_opnd,
                                                      VISA_opnd *v_opnd,
                                                      VISA_opnd *r_opnd,
                                                      VISA_opnd *dst,
                                                      int line_no)
{
    VISA_SurfaceVar* surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surface_name);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");

    VISA_StateOpndHandle* surface = NULL;

    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);

    int status = CM_SUCCESS;

    if (opcode == ISA_SAMPLE) {
        VISA_VMEVar* samplerVar = (VISA_VMEVar*)m_kernel->getDeclFromName(sampler_name);
        MUST_BE_TRUE1(samplerVar != NULL, line_no, "Sampler was not found");

        VISA_StateOpndHandle* sampler = NULL;
        m_kernel->CreateVISAStateOperandHandle(sampler, samplerVar);

        bool isSimd16 = ((simd_mode == 16) ? true : false);

        status = m_kernel->AppendVISASISample(vISA_EMASK_M1, surface, sampler, channel.getAPI(), isSimd16, (VISA_RawOpnd*)u_opnd, (VISA_RawOpnd*)v_opnd, (VISA_RawOpnd*)r_opnd, (VISA_RawOpnd*)dst);

    } else if (opcode == ISA_LOAD) {
        bool isSimd16 = ((simd_mode == 16) ? true : false);
        status = m_kernel->AppendVISASILoad(surface, channel.getAPI(), isSimd16, (VISA_RawOpnd*)u_opnd, (VISA_RawOpnd*)v_opnd, (VISA_RawOpnd*)r_opnd, (VISA_RawOpnd*)dst);
    } else {
        MUST_BE_TRUE1(false, line_no, "Sampler Opcode not supported.");
        return false;
    }

    MUST_BE_TRUE1(status == CM_SUCCESS, line_no, "Failed to create SAMPLE or LOAD instruction.");
    return true;
}

bool CISA_IR_Builder::CISA_create_sampleunorm_instruction (ISA_Opcode opcode,
                                                           ChannelMask channel,
                                                           CHANNEL_OUTPUT_FORMAT out,
                                                           char* sampler_name,
                                                           char* surface_name,
                                                           VISA_opnd *src0,
                                                           VISA_opnd *src1,
                                                           VISA_opnd *src2,
                                                           VISA_opnd *src3,
                                                           VISA_opnd *dst,
                                                           int line_no)
{
    VISA_SurfaceVar *surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surface_name);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");

    VISA_StateOpndHandle * surface = NULL;
    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);

    VISA_VMEVar *samplerVar = (VISA_VMEVar *)m_kernel->getDeclFromName(sampler_name);
    MUST_BE_TRUE1(samplerVar != NULL, line_no, "Sampler was not found");

    VISA_StateOpndHandle *sampler = NULL;
    m_kernel->CreateVISAStateOperandHandle(sampler, samplerVar);
    m_kernel->AppendVISASISampleUnorm(surface, sampler, channel.getAPI(),
        (VISA_VectorOpnd *)src0, (VISA_VectorOpnd *)src1, (VISA_VectorOpnd *)src2, (VISA_VectorOpnd *)src3, (VISA_RawOpnd *)dst, out);
    return true;
}

bool CISA_IR_Builder::CISA_create_vme_ime_instruction (ISA_Opcode opcode,
                                                       unsigned char stream_mode,
                                                       unsigned char searchCtrl,
                                                       VISA_opnd *input_opnd,
                                                       VISA_opnd *ime_input_opnd,
                                                       char* surface_name,
                                                       VISA_opnd *ref0_opnd,
                                                       VISA_opnd *ref1_opnd,
                                                       VISA_opnd *costCenter_opnd,
                                                       VISA_opnd *dst_opnd,
                                                       int line_no)
{
    VISA_SurfaceVar *surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surface_name);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");

    VISA_StateOpndHandle * surface = NULL;
    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);
    m_kernel->AppendVISAMiscVME_IME(surface, stream_mode, searchCtrl, (VISA_RawOpnd *)input_opnd, (VISA_RawOpnd *)ime_input_opnd,
        (VISA_RawOpnd *)ref0_opnd, (VISA_RawOpnd *)ref1_opnd, (VISA_RawOpnd *)costCenter_opnd, (VISA_RawOpnd *)dst_opnd);

    return true;
}

bool CISA_IR_Builder::CISA_create_vme_sic_instruction (ISA_Opcode opcode,
                                                       VISA_opnd *input_opnd,
                                                       VISA_opnd *sic_input_opnd,
                                                       char* surface_name,
                                                       VISA_opnd *dst,
                                                       int line_no)
{
    VISA_SurfaceVar *surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surface_name);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");

    VISA_StateOpndHandle * surface = NULL;
    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);
    m_kernel->AppendVISAMiscVME_SIC(surface, (VISA_RawOpnd *)input_opnd, (VISA_RawOpnd *)sic_input_opnd, (VISA_RawOpnd *)dst);
    return true;
}

bool CISA_IR_Builder::CISA_create_vme_fbr_instruction (ISA_Opcode opcode,
                                                       VISA_opnd *input_opnd,
                                                       VISA_opnd *fbr_input_opnd,
                                                       char* surface_name,
                                                       VISA_opnd* fbrMbMode,
                                                       VISA_opnd* fbrSubMbShape,
                                                       VISA_opnd* fbrSubPredMode,
                                                       VISA_opnd *dst,
                                                       int line_no)
{
    VISA_SurfaceVar *surfaceVar = (VISA_SurfaceVar*)m_kernel->getDeclFromName(surface_name);
    MUST_BE_TRUE1(surfaceVar != NULL, line_no, "Surface was not found");

    VISA_StateOpndHandle * surface = NULL;
    m_kernel->CreateVISAStateOperandHandle(surface, surfaceVar);
    m_kernel->AppendVISAMiscVME_FBR(surface, (VISA_RawOpnd *)input_opnd, (VISA_RawOpnd *)fbr_input_opnd,
        (VISA_VectorOpnd *)fbrMbMode, (VISA_VectorOpnd *)fbrSubMbShape, (VISA_VectorOpnd *)fbrSubPredMode, (VISA_RawOpnd *)dst);
    return true;
}

bool CISA_IR_Builder::CISA_create_NO_OPND_instruction(ISA_Opcode opcode)
{
    m_kernel->AppendVISASyncInst(opcode);
    return true;
}

bool CISA_IR_Builder::CISA_create_switch_instruction(ISA_Opcode opcode,
                                                     unsigned exec_size,
                                                     VISA_opnd *indexOpnd,
                                                     int numLabels,
                                                     char ** labels,
                                                     int line_no)
{
    VISA_INST_Desc *inst_desc = &CISA_INST_table[opcode];
    VISA_opnd *opnd[35];
    int num_pred_desc_operands = 1;
    int num_operands = 0;

    opnd[num_operands] = (VISA_opnd * )m_mem.alloc(sizeof(VISA_opnd));
    opnd[num_operands]->_opnd.other_opnd = numLabels; //real ID will be set during kernel finalization
    opnd[num_operands]->opnd_type = CISA_OPND_OTHER;
    opnd[num_operands]->size = (unsigned short) Get_Common_ISA_Type_Size((VISA_Type)inst_desc->opnd_desc[num_pred_desc_operands].data_type);
    opnd[num_operands]->tag = (unsigned char) inst_desc->opnd_desc[num_pred_desc_operands].opnd_type;
    num_operands++;

    //global offset
    if(indexOpnd != NULL)
    {
        opnd[num_operands] = indexOpnd;
        num_operands ++;
    }

    for(int i = numLabels - 1; i >= 0; i--)
    {
        //TODO: FIX
        //m_kernel->string_pool_lookup_and_insert_branch_targets(labels[i], LABEL_VAR, ISA_TYPE_UW); //Will be checked after whole analysis of the text
        opnd[num_operands] = m_kernel->CreateOtherOpndHelper(num_pred_desc_operands, 2, inst_desc, m_kernel->getIndexFromLabelName(std::string(labels[i])));
        num_operands++;
    }

    m_kernel->AppendVISACFSwitchJMPInst((VISA_VectorOpnd *)indexOpnd, (unsigned char)numLabels, (VISA_LabelOpnd **)opnd);

    for(int i = 0; i< numLabels; i++)
    {
        VISA_opnd * temp_opnd = opnd[i];
        m_kernel->addPendingLabels(temp_opnd);
    }
    for(int i = numLabels - 1; i >= 0; i--)
    {
        m_kernel->addPendingLabelNames(std::string(labels[i]));
    }

    return true;
}

bool CISA_IR_Builder::CISA_create_fcall_instruction(VISA_opnd *pred_opnd,
                                                    ISA_Opcode opcode,
                                                    Common_VISA_EMask_Ctrl emask,
                                                    unsigned exec_size,
                                                    unsigned func_id,
                                                    unsigned arg_size,
                                                    unsigned return_size,
                                                    int line_no) //last index
{
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISACFFunctionCallInst((VISA_PredOpnd *)pred_opnd,emask, executionSize, (unsigned short)func_id, (unsigned char)arg_size, (unsigned char)return_size);
    return true;
}

bool CISA_IR_Builder::CISA_create_raw_send_instruction(ISA_Opcode opcode,
                                                       unsigned char modifier,
                                                       Common_VISA_EMask_Ctrl emask,
                                                       unsigned exec_size,
                                                       VISA_opnd *pred_opnd,
                                                       unsigned int exMsgDesc,
                                                       unsigned char srcSize,
                                                       unsigned char dstSize,
                                                       VISA_opnd *Desc,
                                                       VISA_opnd *Src,
                                                       VISA_opnd *Dst,
                                                       int line_no)
{
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISAMiscRawSend((VISA_PredOpnd *) pred_opnd, emask, executionSize, modifier, exMsgDesc, srcSize, dstSize,
        (VISA_VectorOpnd *)Desc, (VISA_RawOpnd *)Src, (VISA_RawOpnd *)Dst);
    return true;
}

bool CISA_IR_Builder::CISA_create_lifetime_inst(unsigned char startOrEnd,
                                                char *src,
                                                int line_no)
{
    // src is a string representation of variable.
    // Scan entire symbol table to find variable whose name
    // corresponds to src.
    CISA_GEN_VAR *cisaVar = m_kernel->getDeclFromName(src);
    MUST_BE_TRUE1(cisaVar != NULL, line_no, "variable for lifetime not found");

    VISA_opnd *var = NULL;
    if(cisaVar->type == GENERAL_VAR)
    {
        var = CISA_create_gen_src_operand(src, 0, 1, 0, 0, 0, MODIFIER_NONE, line_no);
    }
    else if(cisaVar->type == ADDRESS_VAR)
    {
        var = CISA_set_address_operand(cisaVar, 0, 1, (startOrEnd == 0));
    }
    else if(cisaVar->type == PREDICATE_VAR)
    {
        char cntrl[4] = {0, 0, 0, 0};
        var = CISA_create_predicate_operand(src, MODIFIER_NONE, PredState_NO_INVERSE, cntrl, line_no);
    }
    m_kernel->AppendVISALifetime((VISAVarLifetime)startOrEnd, (VISA_VectorOpnd*)var);

    return true;
}

bool CISA_IR_Builder::CISA_create_raw_sends_instruction(ISA_Opcode opcode,
                                                       unsigned char modifier,
                                                       Common_VISA_EMask_Ctrl emask,
                                                       unsigned exec_size,
                                                       VISA_opnd *pred_opnd,
                                                       VISA_opnd *exMsgDesc,
                                                       unsigned char ffid,
                                                       unsigned char src0Size,
                                                       unsigned char src1Size,
                                                       unsigned char dstSize,
                                                       VISA_opnd *Desc,
                                                       VISA_opnd *Src0,
                                                       VISA_opnd *Src1,
                                                       VISA_opnd *Dst,
                                                       int line_no)
{
    Common_ISA_Exec_Size executionSize = Get_Common_ISA_Exec_Size_From_Raw_Size(exec_size);
    m_kernel->AppendVISAMiscRawSends((VISA_PredOpnd *) pred_opnd, emask, executionSize, modifier, ffid, (VISA_VectorOpnd *)exMsgDesc, src0Size, src1Size, dstSize,
        (VISA_VectorOpnd *)Desc, (VISA_RawOpnd *)Src0, (VISA_RawOpnd *)Src1, (VISA_RawOpnd *)Dst);
    return true;
}
/*
Should be only called from CISA 2.4+
*/
bool CISA_IR_Builder::CISA_create_fence_instruction(ISA_Opcode opcode, unsigned char mode)
{
     m_kernel->AppendVISASyncInst(opcode, mode);
    return true;
}

bool CISA_IR_Builder::CISA_create_wait_instruction(VISA_opnd* mask)
{
    m_kernel->AppendVISAWaitInst((VISA_VectorOpnd*) mask);
    return true;
}


/*** CISA 3.0 and later ***/
bool CISA_IR_Builder::CISA_create_yield_instruction(ISA_Opcode opcode)
{
    m_kernel->AppendVISASyncInst(opcode);
    return true;
}

VISA_opnd * CISA_IR_Builder::CISA_create_gen_src_operand(char* var_name, short v_stride, short width, short h_stride,
                                                         unsigned char row_offset, unsigned char col_offset, VISA_Modifier mod, int line_no)
{
    VISA_VectorOpnd *cisa_opnd = NULL;
    int status = CM_SUCCESS;
    status = m_kernel->CreateVISASrcOperand(cisa_opnd, (VISA_GenVar*)m_kernel->getDeclFromName(std::string(var_name)), mod, v_stride, width, h_stride, row_offset, col_offset);
    MUST_BE_TRUE1(status == CM_SUCCESS, line_no, "Failed to create cisa src operand." );
    return (VISA_opnd *)cisa_opnd;
}

VISA_opnd * CISA_IR_Builder::CISA_dst_general_operand(char * var_name,
                                                      unsigned char roff,
                                                      unsigned char sroff,
                                                      unsigned short hstride, int line_no)
{

    VISA_VectorOpnd *cisa_opnd = NULL;
    int status = CM_SUCCESS;
    status = m_kernel->CreateVISADstOperand(cisa_opnd, (VISA_GenVar *)m_kernel->getDeclFromName(std::string(var_name)), hstride, roff, sroff);
    MUST_BE_TRUE1(status == CM_SUCCESS, line_no, "Failed to create cisa dst operand.");
    return (VISA_opnd *)cisa_opnd;
}
VISA_opnd * CISA_IR_Builder::CISA_create_immed(uint64_t value, VISA_Type type, int line_no)
{
    VISA_VectorOpnd *cisa_opnd = NULL;
    int status = CM_SUCCESS;
    status =  m_kernel->CreateVISAImmediate(cisa_opnd, &value, type);
    MUST_BE_TRUE1(status == CM_SUCCESS,line_no,"Could not create immediate.");
    if (type == ISA_TYPE_Q || type == ISA_TYPE_UQ)
    {
        cisa_opnd->_opnd.v_opnd.opnd_val.const_opnd._val.lval = value;
    }
    else
    {
        cisa_opnd->_opnd.v_opnd.opnd_val.const_opnd._val.ival = (uint32_t)value;
    }
    return (VISA_opnd *)cisa_opnd;
}

VISA_opnd * CISA_IR_Builder::CISA_create_float_immed(double value, VISA_Type type, int line_no)
{
    VISA_VectorOpnd *cisa_opnd = NULL;
    if (type == ISA_TYPE_F)
    {
        float temp = (float)value;
        m_kernel->CreateVISAImmediate(cisa_opnd, &temp, type);
    }
    else
    {
        m_kernel->CreateVISAImmediate(cisa_opnd, &value, type);
    }

    return (VISA_opnd *)cisa_opnd;
}

CISA_GEN_VAR * CISA_IR_Builder::CISA_find_decl(char * var_name)
{
    return m_kernel->getDeclFromName(var_name);
}

VISA_opnd * CISA_IR_Builder::CISA_set_address_operand(CISA_GEN_VAR * cisa_decl, unsigned char offset, short width, bool isDst)
{
    /*
    cisa_opnd->opnd_type = CISA_OPND_VECTOR;
    cisa_opnd->tag = OPERAND_ADDRESS;
    cisa_opnd->_opnd.v_opnd.tag = OPERAND_ADDRESS;
    cisa_opnd->_opnd.v_opnd.opnd_val.addr_opnd.index= cisa_opnd->index;
    cisa_opnd->_opnd.v_opnd.opnd_val.addr_opnd.offset= offset;
    cisa_opnd->_opnd.v_opnd.opnd_val.addr_opnd.width= Get_Common_ISA_Exec_Size_From_Raw_Size(width & 0xF);
    cisa_opnd->size = Get_Size_Vector_Operand(&cisa_opnd->_opnd.v_opnd);
    */
    VISA_VectorOpnd *cisa_opnd = NULL;
    int status = CM_SUCCESS;
    status = m_kernel->CreateVISAAddressOperand(cisa_opnd, (VISA_AddrVar *)cisa_decl, offset, width, isDst);
    if( status != CM_SUCCESS )
    {
        CmAssert( 0 );
        return NULL;
    }

    return (VISA_opnd *)cisa_opnd;
}

VISA_opnd * CISA_IR_Builder::CISA_set_address_expression(CISA_GEN_VAR *cisa_decl, short offset)
{
    VISA_VectorOpnd *cisa_opnd = NULL;
    m_kernel->CreateVISAAddressOfOperand(cisa_opnd, (VISA_GenVar *)cisa_decl, offset);
    return (VISA_opnd *)cisa_opnd;
}

VISA_opnd * CISA_IR_Builder::CISA_create_indirect(CISA_GEN_VAR * cisa_decl,VISA_Modifier mod, unsigned short row_offset,
                                                  unsigned char col_offset, unsigned short immedOffset,
                                                  unsigned short vertical_stride, unsigned short width,
                                                  unsigned short horizontal_stride, VISA_Type type)
{
    /*
    cisa_opnd->opnd_type = CISA_OPND_VECTOR;
    cisa_opnd->tag = OPERAND_INDIRECT;
    cisa_opnd->_opnd.v_opnd.tag = OPERAND_INDIRECT;
    cisa_opnd->_opnd.v_opnd.opnd_val.indirect_opnd.index = cisa_decl->index;
    cisa_opnd->_opnd.v_opnd.opnd_val.indirect_opnd.addr_offset = col_offset;
    cisa_opnd->_opnd.v_opnd.opnd_val.indirect_opnd.indirect_offset = immedOffset;
    cisa_opnd->_opnd.v_opnd.opnd_val.indirect_opnd.bit_property = type;
    cisa_opnd->_opnd.v_opnd.opnd_val.indirect_opnd.region = Create_CISA_Region(vertical_stride,width,horizontal_stride);//Get_CISA_Region_Val(horizontal_stride) <<8;

    cisa_opnd->_opnd.v_opnd.tag += mod<<3;

    cisa_opnd->size = Get_Size_Vector_Operand(&cisa_opnd->_opnd.v_opnd);
    */
    VISA_VectorOpnd *cisa_opnd = NULL;
    m_kernel->CreateVISAIndirectSrcOperand(cisa_opnd, (VISA_AddrVar*)cisa_decl, mod, col_offset, immedOffset, vertical_stride, width, horizontal_stride, type);
    return cisa_opnd;
}

VISA_opnd * CISA_IR_Builder::CISA_create_indirect_dst(CISA_GEN_VAR * cisa_decl,VISA_Modifier mod, unsigned short row_offset,
                                                      unsigned char col_offset, unsigned short immedOffset,
                                                      unsigned short horizontal_stride, VISA_Type type)
{
    VISA_VectorOpnd *cisa_opnd = NULL;
    m_kernel->CreateVISAIndirectDstOperand(cisa_opnd, (VISA_AddrVar*)cisa_decl, col_offset, immedOffset, horizontal_stride, type);
    return (VISA_opnd *)cisa_opnd;
}

VISA_opnd * CISA_IR_Builder::CISA_create_state_operand(char * var_name, unsigned char offset, int line_no, bool isDst)
{

    CISA_GEN_VAR *decl = m_kernel->getDeclFromName(std::string(var_name));

    MUST_BE_TRUE1(decl != NULL, line_no, "Could not find the Declaration.");

    VISA_VectorOpnd * cisa_opnd = NULL;

    int status = CM_SUCCESS;
    switch(decl->type)
    {
    case SURFACE_VAR:
        {
            status = m_kernel->CreateVISAStateOperand(cisa_opnd, (VISA_SurfaceVar *)decl, offset, isDst);
            break;
        }
    case SAMPLER_VAR:
        {
            status = m_kernel->CreateVISAStateOperand(cisa_opnd, (VISA_SamplerVar *)decl, offset, isDst);
            break;
        }
    default:
        {
            MUST_BE_TRUE1(false, line_no, "Incorrect declaration type.");
        }
    }

    MUST_BE_TRUE1(status == CM_SUCCESS, line_no, "Was not able to create State Operand.");
    return (VISA_opnd *)cisa_opnd;
}

VISA_opnd * CISA_IR_Builder::CISA_create_predicate_operand(char * var_name, VISA_Modifier mod, VISA_PREDICATE_STATE state, char * cntrl, int line_no)
{

    VISA_PREDICATE_CONTROL control = PRED_CTRL_NON;
    if(cntrl[0] == 'a' && cntrl[1] == 'n' && cntrl[2] == 'y')
    {
        control = PRED_CTRL_ANY;
    }
    else if(cntrl[0] == 'a' && cntrl[1] == 'l' && cntrl[2] == 'l')
    {
        control = PRED_CTRL_ALL;
    }
    VISA_PredOpnd *cisa_opnd = NULL;
    CISA_GEN_VAR * decl = m_kernel->getDeclFromName(std::string(var_name));
    int status = CM_SUCCESS;
    m_kernel->CreateVISAPredicateOperand(cisa_opnd, (VISA_PredVar *)decl, state, control);
    MUST_BE_TRUE1((status == CM_SUCCESS), line_no, "Failed to create predicate operand.");
    return (VISA_opnd *)cisa_opnd;
}

VISA_opnd * CISA_IR_Builder::CISA_create_RAW_NULL_operand(int line_no)
{
    /*
    VISA_opnd * cisa_opnd = (VISA_opnd *) m_mem.alloc(sizeof(VISA_opnd));
    cisa_opnd->opnd_type = CISA_OPND_RAW;
    cisa_opnd->tag = NUM_OPERAND_CLASS;
    cisa_opnd->_opnd.r_opnd.index = 0;
    cisa_opnd->index = 0;
    cisa_opnd->_opnd.r_opnd.offset = 0;
    cisa_opnd->size = sizeof(cisa_opnd->_opnd.r_opnd.index) + sizeof(cisa_opnd->_opnd.r_opnd.offset);
    */

    VISA_RawOpnd *cisa_opnd = NULL;
    int status = CM_SUCCESS;
    status = m_kernel->CreateVISANullRawOperand(cisa_opnd);
    MUST_BE_TRUE1(status == CM_SUCCESS, line_no, "Was not able to create NULL RAW operand.");
    return (VISA_opnd *)cisa_opnd;

}

VISA_opnd * CISA_IR_Builder::CISA_create_RAW_operand(char * var_name, unsigned short offset, int line_no)
{
    VISA_RawOpnd *cisa_opnd = NULL;
    int status = m_kernel->CreateVISARawOperand(cisa_opnd, (VISA_GenVar *)m_kernel->getDeclFromName(var_name), offset);
    MUST_BE_TRUE1(status == CM_SUCCESS, line_no, "Was not able to create RAW operand.");
    return (VISA_opnd *)cisa_opnd; //delay the decision of src or dst until translate stage
}

unsigned short CISA_IR_Builder::get_hash_key(const char* str)
{
    const char *str_pt = str;
    unsigned short key=0;
    unsigned char c;
    while ((c = *str_pt++) != '\0') key = (key+c)<<1;

    return key % HASH_TABLE_SIZE;
}
string_pool_entry** CISA_IR_Builder::new_string_pool()
{
    string_pool_entry ** sp = (string_pool_entry**)m_mem.alloc(sizeof(string_pool_entry *) * HASH_TABLE_SIZE);
    memset(sp, 0, sizeof(string_pool_entry *) * HASH_TABLE_SIZE);

    return sp;
}

string_pool_entry * CISA_IR_Builder::string_pool_lookup(string_pool_entry **spool, const char *str)
{
    unsigned short key = 0;
    string_pool_entry* entry;
    char *s;

    key = get_hash_key(str);

    for( entry = spool[key]; entry != NULL; entry = entry->next ){
        s = (char *)entry->value;
        if(!strcmp(s, str))
            return entry;
    }

    return NULL;
}

bool CISA_IR_Builder::string_pool_lookup_and_insert(string_pool_entry **spool,
                                                    char *str,
                                                    Common_ISA_Var_Class type,
                                                    VISA_Type data_type)
{
    unsigned short key = 0;
    string_pool_entry* entry;
    char *s;
    int len = (int) strlen(str);

    key = get_hash_key(str);

    for( entry = spool[key]; entry != NULL; entry = entry->next ){
        s = (char *)entry->value;
        if(!strcmp(s, str))
            return false;
    }

    s = (char*)m_mem.alloc(len + 1);
    memcpy_s(s, len + 1, str, len+1);
    s[len] = '\0';

    entry = (string_pool_entry*)m_mem.alloc(sizeof(string_pool_entry));
    memset(entry, 0, sizeof(*entry));
    entry->value = s;
    entry->type = type;
    entry->data_type = data_type;

    entry->next = spool[key];
    spool[key] = entry;

    return true;
}

Common_ISA_Input_Class CISA_IR_Builder::get_input_class(Common_ISA_Var_Class var_class)
{
    if (var_class == GENERAL_VAR)
        return INPUT_GENERAL;

    if (var_class == SAMPLER_VAR)
        return INPUT_SAMPLER;

    if (var_class == SURFACE_VAR)
        return INPUT_SURFACE;

    return INPUT_UNKNOWN;
}
void CISA_IR_Builder::CISA_post_file_parse()
{
    //Checking if target labels have been declared
    /*
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
    if (branch_targets[i]) {
    string_pool_entry * l = branch_targets[i];
    while(l != NULL) {
    string_pool_entry * se = string_pool_lookup((const char*)l->value);
    MUST_BE_TRUE(se, "Not defined target label");
    MUST_BE_TRUE(se->type == LABEL_VAR, "Not defined target label");
    l = l->next;
    }
    }
    }
    */
    return;
}

bool CISA_IR_Builder::CISA_create_func_decl(char * name,
                                            int resolved_index,
                                            int line_no)
{
    /*
    Need to create name to resolved index,and resolved index to name mapping for later use.
    For example in fcall.
    */
    return true;
}


#endif // !defined(DLL_MODE)
