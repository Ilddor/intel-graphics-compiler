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
#ifdef _MSC_VER
#pragma warning(disable: 4005)
#endif
#pragma once

#include <vector>
#include <stdint.h>
#include "usc.h"
#include "SystemThreadRegs.h"
#include "common/LLVMWarningsPush.hpp"
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include "common/LLVMWarningsPop.hpp"
#include "../AdaptorOCL/OCL/LoadBuffer.h"
#include "Compiler/CISACodeGen/Platform.hpp"
#include "common/debug/Debug.hpp"

#include "common/SIPKernels/Gen10SIPCSR.h"
#include "common/SIPKernels/Gen10SIPCSRDebug.h"
#include "common/SIPKernels/Gen10SIPDebug.h"
#include "common/SIPKernels/Gen10SIPDebugBindless.h"
#include "common/SIPKernels/Gen9BXTSIPCSR.h"
#include "common/SIPKernels/Gen9SIPCSR.h"
#include "common/SIPKernels/Gen9SIPCSRDebug.h"
#include "common/SIPKernels/Gen9SIPCSRDebugLocal.h"
#include "common/SIPKernels/Gen9SIPDebug.h"
#include "common/SIPKernels/Gen9SIPDebugBindless.h"


namespace SIP 
{
    enum SIP_ID {
        GEN9_SIP_DEBUG = 0,
        GEN9_SIP_CSR,
        GEN9_SIP_CSR_DEBUG,

        GEN10_SIP_DEBUG,
        GEN10_SIP_CSR,
        GEN10_SIP_CSR_DEBUG,

        GEN9_SIP_DEBUG_BINDLESS,
        GEN10_SIP_DEBUG_BINDLESS,

        GEN9_BXT_SIP_CSR,
        GEN9_SIP_CSR_DEBUG_LOCAL,
        GEN_SIP_MAX_INDEX
    };

    class CSystemThread
    {
    public:
        static bool   CreateSystemThreadKernel(
            const IGC::CPlatform &platform,
            const USC::SYSTEM_THREAD_MODE mode,
            USC::SSystemThreadKernelOutput* &pSystemThread,
            bool bindlessmode = false); 

        static void   DeleteSystemThreadKernel(
            USC::SSystemThreadKernelOutput* &pSystemThread );

    private:
        CSystemThread( void );
    };


    class CGenSystemInstructionKernelProgram 
    {
    public:
        CGenSystemInstructionKernelProgram* Create(
            const IGC::CPlatform &platform,
            const USC::SYSTEM_THREAD_MODE mode,
            const bool bindlessMode);

            void Delete(CGenSystemInstructionKernelProgram* &pKernelProgram );

            CGenSystemInstructionKernelProgram(const USC::SYSTEM_THREAD_MODE mode);
            unsigned int  GetProgramSize(){ ASSERT(m_ProgramSize) ; return m_ProgramSize;}
            unsigned int  GetCacheFlushDataSize(){ return m_FlushDataSizeInBytes;}
            unsigned int  GetPerThreadDebugDataSize(){ return m_PerThreadDebugDataSizeInBytes; }
            void * GetLinearAddress(){ ASSERT(m_LinearAddress); return m_LinearAddress;}

    protected:

         const SArfRegData* m_pShaderDebugArfRegData;
         const SArfRegData* m_pGpgpuArfSaveRegData;
         const SArfRegData* m_pGpgpuArfRestoreRegData;
         unsigned int m_cGpgpuArfRegNumber;
         unsigned int m_cShaderDebugArfRegNumber;
         unsigned int m_PerThreadDebugDataSizeInBytes; // Number of bytes that will be dumped in the scratch space per thread
         unsigned int m_FlushDataSizeInBytes;
         unsigned int m_CacheFlushCount;// Number of times we write to cache to flush it
         unsigned int m_ProgramSize;
         void* m_LinearAddress;

    };

} // namespace IGC
