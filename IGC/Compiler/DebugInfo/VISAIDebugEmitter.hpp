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
#pragma once

#include "llvm/Config/llvm-config.h"

#if LLVM_VERSION_MAJOR == 4 && LLVM_VERSION_MINOR == 0
#include "common/LLVMWarningsPush.hpp"
#include "common/LLVMWarningsPop.hpp"

namespace llvm
{
class Instruction;
class Function;
}

namespace IGC
{
// Forward declaration
class CShader;

/// @brief IDebugEmitter is an interface for debug info emitter class.
///        It can be used by IGC VISA emitter pass to emit debug info.
class IDebugEmitter
{
public:
    /// @brief Creates a new concrete instance of debug emitter.
    /// @return A new instance of debug emitter.
    static IDebugEmitter* Create();

    /// @brief Releases given instance of debug emitter.
    /// @param pDebugEmitter instance of debug emitter to release.
    static void Release(IDebugEmitter* pDebugEmitter);

    IDebugEmitter() {}

    virtual ~IDebugEmitter() {}

    /// @brief Initialize debug emitter for processing the given shader.
    /// @param pShader shader to process, and emit debug info for.
    /// @param debugEnabled indicator for emitting debug info or not.
    virtual void Initialize(CShader* pShader, bool debugEnabled) = 0;

    /// @brief Emit debug info to given buffer and reset debug emitter.
    /// @param pBuffer [OUT] object buffer conatins the emitted debug info.
    /// @param size [OUT] size of debug info buffer.
    // @param finalize [IN] indicates whether this is last function in group.
    virtual void Finalize(void *&pBuffer, unsigned int &size, bool finalize) = 0;

    /// @brief Process instruction before emitting its VISA code.
    /// @param pInst instruction to process.
    virtual void BeginInstruction(llvm::Instruction *pInst) = 0;

    /// @brief Process instruction after emitting its VISA code.
    /// @param pInst instruction to process.
    virtual void EndInstruction(llvm::Instruction *pInst) = 0;

    /// @brief Mark begin of VISA code emitting section.
    virtual void BeginEncodingMark() = 0;

    /// @brief Mark end of VISA code emitting section.
    virtual void EndEncodingMark() = 0;

    /// @brief Free given buffer memory.
    /// @param pBuffer buffer allocated by debug emiiter component.
    virtual void Free(void *pBuffer) = 0;

    virtual void setFunction(llvm::Function* F, bool c) = 0;

    virtual void ResetVISAModule() = 0;
};

} // namespace IGC

#endif
