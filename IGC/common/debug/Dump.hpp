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

#include "common/Types.hpp"

#include "AdaptorCommon/customApi.hpp"
#include "Compiler/CodeGenPublicEnums.h"

#include <iStdLib/utility.h>

#include "common/LLVMWarningsPush.hpp"
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Module.h>
#include <llvm/ADT/Optional.h>
#include "common/LLVMWarningsPop.hpp"

#include <string>
#include <vector>
#include <stdarg.h>

// Forward declarations
struct D3D10DDIARG_SIGNATURE_ENTRY;
struct D3D10DDIARG_STAGE_IO_SIGNATURES;
struct D3D11DDIARG_TESSELLATION_IO_SIGNATURES;
namespace IGC { class CodeGenContext; }

namespace USC
{
    struct ShaderD3D;
    struct SPixelShaderNOS;
    struct SVertexShaderNOS;
}

namespace IGC
{
class CShader;
namespace Debug
{
/// return the name of the file to dump
std::string GetDumpName(CShader* pProgram, const char* ext);

/*************************************************************************************************\
 *  Generic
 */

class DumpName
{
public:
    explicit DumpName(std::string const& dumpName);
    DumpName();

    DumpName ShaderName(std::string const& name) const;
    DumpName Type(ShaderType type) const;
    DumpName Extension(std::string const& extension) const;
    DumpName SIMDSize(SIMDMode width) const;
    DumpName DispatchMode(ShaderDispatchMode mode) const;
    DumpName Hash(ShaderHash hash) const;
    DumpName PostFix(std::string const& postfixStr) const;
    DumpName Pass(std::string const& name, llvm::Optional<uint32_t> index = llvm::Optional<uint32_t>()) const;
    DumpName PSPhase(PixelShaderPhaseType phase) const;
    std::string str() const;
    std::string overridePath() const;
    std::string RelativePath() const;
    std::string AbsolutePath(OutputFolderName folder) const;

private:
    class CPassDescriptor
    {
    public:
        std::string               m_name;
        llvm::Optional<unsigned int>    m_index;
    };

    llvm::Optional<std::string>         m_dumpName;
    llvm::Optional<std::string>         m_shaderName;
    llvm::Optional<ShaderType>          m_type;
    llvm::Optional<PixelShaderPhaseType> m_psPhase;
    llvm::Optional<std::string>         m_extension;
    llvm::Optional<SIMDMode>            m_simdWidth;
    llvm::Optional<ShaderDispatchMode>  m_ShaderMode;
    llvm::Optional<ShaderHash>          m_hash;
    llvm::Optional<std::string>         m_postfixStr;
    llvm::Optional<CPassDescriptor>     m_pass;
};

/// return the name of the file to dump
DumpName GetDumpNameObj(CShader* pProgram, const char* ext);

class Dump
{
public:
    Dump( DumpName const& dumpName, DumpType type );
    virtual ~Dump();

    llvm::raw_ostream& stream() const;

    template<typename T>
    llvm::raw_ostream& operator<< ( T const& val ) const
    {
        stream() << val;
    }

    static bool isBinaryDump(DumpType type);

private:
    std::string               m_string;
    DumpName                   m_name;
    llvm::raw_ostream*         m_pStream;
};

void DumpLLVMIRText(
    llvm::Module*             pModule,
    Dump                      const& dump,
    llvm::AssemblyAnnotationWriter*  optionalAnnotationWriter = nullptr);

int PrintDebugMsgV(
    const Dump* dump,
    const char* fmt,
    va_list ap);

void PrintDebugMsg(
    const Dump* dump,
    const char* fmt,
    ...);


inline FILE* OpenDumpFile(
    const char* fileNamePrefix,
    const char* fileNameExt,
    const ShaderHash& hash,
    const char* flag)
{
    std::string name =
        IGC::Debug::DumpName(fileNamePrefix)
        .Hash(hash)
        .Extension(fileNameExt)
        .str();
    FILE* fp = fopen(name.c_str(), flag);
    return fp;
}

AsmHash AsmHashOCL(const UINT* pShaderCode, size_t size);
ShaderHash ShaderHashOCL(const UINT* pShaderCode, size_t size);

AsmHash    AsmHashOGL(QWORD hash);
NosHash    NosHashOGL(QWORD hash);
ShaderHash ShaderHashOGL(QWORD glslHash, QWORD nosHash);

} // namespace Debug
} // namespace IGC
