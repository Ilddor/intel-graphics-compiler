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
#include "common/debug/Dump.hpp"

#include "common/debug/TeeOutputStream.hpp"

#include "AdaptorCommon/customApi.hpp"
#include "Compiler/CodeGenPublic.h"
#include "Compiler/CISACodeGen/ShaderCodeGen.hpp"
#include "Compiler/CISACodeGen/PixelShaderCodeGen.hpp"
#include "common/igc_regkeys.hpp"

#include <iStdLib/utility.h>

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/FileSystem.h>
#include "common/LLVMWarningsPop.hpp"

#include <stdarg.h>

#include <fstream>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <algorithm>

using namespace IGC;
using namespace IGC::Debug;
using namespace std;

namespace IGC
{
namespace Debug
{


/*************************************************************************************************\
 *  Generic
 */

DumpName::DumpName( std::string const& dumpName )
    : m_dumpName(dumpName)
{
}

DumpName::DumpName()
{
}

DumpName DumpName::ShaderName(std::string const& name) const
{
    assert( name.find(" ") == std::string::npos && "Shader name must not contain spaces");
    DumpName copy(*this);
    copy.m_shaderName = name;
    return copy;
}

DumpName DumpName::Type(ShaderType type) const
{
    DumpName copy(*this);
    copy.m_type = type;
    return copy;
}

DumpName DumpName::PSPhase(PixelShaderPhaseType phase) const
{
    DumpName copy(*this);
    copy.m_psPhase = phase;
    return copy;
}

DumpName DumpName::Extension(std::string const& extension) const
{
    assert(
        (extension.size() == 0 || extension.at(0) != '.') &&
        "Extension shouldn't start with a '.', and shouldn't be empty");
    assert( extension.find(" ") == std::string::npos && "Extension must not contain spaces" );
    DumpName copy(*this);
    copy.m_extension = extension;
    return copy;
}

DumpName DumpName::SIMDSize(SIMDMode width) const
{
    DumpName copy(*this);
    copy.m_simdWidth = width;
    return copy;
}

DumpName DumpName::DispatchMode(ShaderDispatchMode mode) const
{
    DumpName copy(*this);
    switch(mode)
    {
    case ShaderDispatchMode::SINGLE_PATCH:
    case ShaderDispatchMode::DUAL_PATCH:
    case ShaderDispatchMode::EIGHT_PATCH:
        copy.m_ShaderMode = mode;
        break;
    default:
        break;
    }
    return copy;
}

DumpName DumpName::Hash(ShaderHash hash) const
{
    DumpName copy(*this);
    copy.m_hash = hash;
    return copy;
}

DumpName DumpName::PostFix(std::string const& postfixStr) const
{
    DumpName copy(*this);
    copy.m_postfixStr = postfixStr;
    return copy;
}

DumpName DumpName::Pass(std::string const& name, llvm::Optional<unsigned int> index) const
{
    std::string newName = name;
    //remove spaces
    newName.erase(remove_if(
        newName.begin(),
        newName.end(), 
        [](char c) {
        return isspace(static_cast<unsigned char>(c));
    }), 
        newName.end());

    assert(newName.find(" ") == std::string::npos && "Pass name must not contain spaces");
    DumpName copy(*this);
    CPassDescriptor pd = { newName, index };
    copy.m_pass = pd;
    return copy;
}

std::string DumpName::AbsolutePath(OutputFolderName folder) const
{
    std::stringstream ss;
    bool underscore = false;  /// Determines whether the next segment prints a leading underscore

    ss << folder;

    //    m_dumpName = IGC::Debug::GetShaderOutputName();

    if(m_dumpName.hasValue() && m_dumpName.getValue() != "")
    {
        ss << m_dumpName.getValue();
        underscore = true;
    }
    if(m_shaderName.hasValue())
    {
        ss << (underscore ? "_" : "")
            << m_shaderName.getValue();
        underscore = true;
    }
    if(m_type.hasValue())
    {
        ss << (underscore ? "_" : "");
        switch(m_type.getValue())
        {
        case ShaderType::OPENCL_SHADER: ss << "OCL"; break;
        case ShaderType::PIXEL_SHADER: ss << "PS"; break;
        case ShaderType::DOMAIN_SHADER: ss << "DS"; break;
        case ShaderType::HULL_SHADER: ss << "HS"; break;
        case ShaderType::VERTEX_SHADER: ss << "VS"; break;
        case ShaderType::GEOMETRY_SHADER: ss << "GS"; break;
        case ShaderType::COMPUTE_SHADER: ss << "CS"; break;
        case ShaderType::UNKNOWN:
        default: assert(0 && "Unknown Shader Type"); break;
        }
        underscore = true;
    }
    if(m_psPhase.hasValue())
    {
        if(m_psPhase.getValue() != PixelShaderPhaseType::PSPHASE_LEGACY)
        {
            ss << (underscore ? "_" : "");
            switch(m_psPhase.getValue())
            {
            case PixelShaderPhaseType::PSPHASE_COARSE: ss << "CPS"; break;
            case PixelShaderPhaseType::PSPHASE_PIXEL: ss << "PS"; break;
            default: assert(0); break;
            }
        }
    }
    if(m_hash.hasValue())
    {
        ss << (underscore ? "_" : "")
            << "asm"
            << std::hex
            << std::setfill('0')
            << std::setw(sizeof(m_hash->asmHash.value) * CHAR_BIT / 4)
            << m_hash->asmHash.value
            << std::dec
            << std::setfill(' ');

        if (m_hash->nosHash.value != 0)
        {
            ss << "_"
               << "nos"
               << std::hex
               << std::setfill('0')
               << std::setw(sizeof(m_hash->nosHash.value) * CHAR_BIT / 4)
               << m_hash->nosHash.value
               << std::dec
               << std::setfill(' ');
        }

        if (m_hash->psoHash.value != 0)
        {
            ss << "_"
                << "pso"
                << std::hex
                << std::setfill('0')
                << std::setw(sizeof(m_hash->psoHash.value) * CHAR_BIT / 4)
                << m_hash->psoHash.value
                << std::dec
                << std::setfill(' ');
        }

        if(m_postfixStr.hasValue() && !m_postfixStr.getValue().empty())
        {
            ss << "_"
                << m_postfixStr.getValue();
        }

        underscore = true;
    }
    if(m_pass.hasValue())
    {
        if(m_pass->m_index.hasValue())
        {
            ss << (underscore ? "_" : "")
                << std::setfill('0')
                << std::setw(4)
                << m_pass->m_index.getValue()
                << std::setfill(' ');
            underscore = true;
        }
        ss << (underscore ? "_" : "")
            << m_pass->m_name;
        underscore = true;
    }
    if(m_simdWidth.hasValue())
    {
        ss << (underscore ? "_" : "")
            << "simd"
            << numLanes(m_simdWidth.getValue());
        underscore = true;
    }
    if(m_ShaderMode.hasValue())
    {
        if(m_ShaderMode.getValue() == ShaderDispatchMode::SINGLE_PATCH)
        {
            ss << (underscore ? "_" : "")
                << "SinglePatch";
            underscore = true;
        }
        if(m_ShaderMode.getValue() == ShaderDispatchMode::DUAL_PATCH)
        {
            ss << (underscore ? "_" : "")
                << "DualPatch";
            underscore = true;
        }
        if(m_ShaderMode.getValue() == ShaderDispatchMode::EIGHT_PATCH)
        {
            ss << (underscore ? "_" : "")
                << "EightPatch";
            underscore = true;
        }
    }
    if(m_extension.hasValue())
    {
        ss << "." << m_extension.getValue();
    }
    return ss.str();
}

std::string DumpName::str() const
{
    return AbsolutePath(IGC::Debug::GetShaderOutputFolder());
}

std::string DumpName::overridePath() const
{
    return AbsolutePath(IGC::Debug::GetShaderOverridePath());
}

std::string DumpName::RelativePath() const
{
    return AbsolutePath("");
}

namespace {
    bool isText( DumpType type )
    {
        switch ( type )
        {
        case DumpType::NOS_TEXT              : return true;
        case DumpType::COS_TEXT              : return true;
        case DumpType::ASM_TEXT              : return true;
        case DumpType::ASM_BC                : return false;
        case DumpType::TRANSLATED_IR_TEXT    : return true;
        case DumpType::TRANSLATED_IR_BC      : return false;
        case DumpType::PASS_IR_TEXT          : return true;
        case DumpType::PASS_IR_BC            : return false;
        case DumpType::OptIR_TEXT            : return true;
        case DumpType::OptIR_BC              : return false;
        case DumpType::VISA_TEXT             : return true;
        case DumpType::VISA_BC               : return false;
        case DumpType::GENX_ISA_TEXT         : return true;
        case DumpType::GENX_ISA_BC           : return false;
        case DumpType::LLVM_OPT_STAT_TEXT    : return true;
        case DumpType::TIME_STATS_TEXT       : return true;
        case DumpType::TIME_STATS_CSV        : return true;
        case DumpType::DBG_MSG_TEXT          : return true;
        default                              : assert( 0 && "unreachable" ); return true;
        }
    }

    const char* commentPrefix( DumpType type )
    {
        assert( isText( type ) && "Only text types supported" );
        switch ( type )
        {
        case DumpType::NOS_TEXT              : return "// ";
        case DumpType::COS_TEXT              : return "// ";
        case DumpType::ASM_TEXT              : return "// ";
        case DumpType::TRANSLATED_IR_TEXT    : return "; ";
        case DumpType::PASS_IR_TEXT          : return "; ";
        case DumpType::OptIR_TEXT            : return "; ";
        case DumpType::VISA_TEXT             : return "/// ";
        case DumpType::GENX_ISA_TEXT         : return "/// ";
        case DumpType::LLVM_OPT_STAT_TEXT    : return "";
        case DumpType::TIME_STATS_TEXT       : return "";
        case DumpType::DBG_MSG_TEXT          : return "";
        case DumpType::TIME_STATS_CSV        : assert( 0 && "CSV doesn't have comments" ); return "";
        default                              : assert( 0 && "unreachable" ); return "#";
        }
    }

    bool isConsolePrintable( DumpType type )
    {
        return isText( type ) && type != DumpType::TIME_STATS_CSV;
    }
}

namespace
{
    /// Writes a prefix to the underlying stream upon first write. Forwards to the underlying
    /// stream on every subsequent write.
    class PrefixStream
        : public llvm::raw_ostream
    {
    public:
        PrefixStream( std::string const& prefix, Colors color, llvm::raw_ostream* pUnder, bool deleteStream )
            : llvm::raw_ostream( true /* unbuffered */ )
            , m_prefix(prefix)
            , m_prefixColor(color)
            , m_pUnder(pUnder)
            , m_deleteStream(deleteStream)
            , m_isFirstWrite(true)
        {
        }

        virtual ~PrefixStream()
        {
            flush();
            if (m_deleteStream)
            {
                m_pUnder->flush();
                delete m_pUnder;
            }
        }

        virtual uint64_t current_pos() const override
        {
            return 0;
        }

        virtual raw_ostream& changeColor(enum Colors Color, bool Bold=false, bool BG=false) override
        {
            m_pUnder->changeColor(Color,Bold,BG);
            return *this;
        }

        virtual raw_ostream& resetColor() override
        {
            m_pUnder->resetColor();
            return *this;
        }

        virtual raw_ostream& reverseColor() override
        {
            m_pUnder->reverseColor();
            return *this;
        }

        virtual bool is_displayed() const override
        {
            return m_pUnder->is_displayed();
        }

        virtual bool has_colors() const override
        {
            return m_pUnder->has_colors();
        }

    protected:
        virtual size_t preferred_buffer_size() const override
        {
            return 0;
        }

    private:
        void write_impl(const char* Ptr, size_t Size) override
        {
            if (m_isFirstWrite)
            {
                ods().changeColor(m_prefixColor, true, false);
                *m_pUnder << m_prefix;
                ods().resetColor();
                m_isFirstWrite = false;
            }
            m_pUnder->write(Ptr,Size);
        }

    private:
        std::string           m_prefix;
        Colors                m_prefixColor;
        llvm::raw_ostream*    m_pUnder;
        bool                  m_deleteStream;
        bool                  m_isFirstWrite;
    };
} // anonymous namespace

bool Dump::isBinaryDump(DumpType type)
{
    switch (type)
    {
    case DumpType::ASM_BC:
    case DumpType::TRANSLATED_IR_BC:
    case DumpType::PASS_IR_BC:
    case DumpType::OptIR_BC:
    case DumpType::VISA_BC:
    case DumpType::GENX_ISA_BC:
        return true;

    default:
        return false;
    }
}

// Note: This constructor is not allowed to hold on to the dumpName reference after it finishes
// (In the case where Dump is on the heap and DumpName is on the stack, the reference would go bad
// when the calling function returns)
Dump::Dump( DumpName const& dumpName, DumpType type )
    : m_name( dumpName )
    , m_pStream( nullptr )
{
    m_pStream = new llvm::raw_string_ostream(m_string); // buffered stream!
    if(IGC_IS_FLAG_ENABLED(PrintToConsole) && isConsolePrintable(type))
    {
        m_pStream->SetUnbuffered();
        m_pStream = new TeeOutputStream(&ods(), false, m_pStream, true);
    }
    if ( isText( type ) && type != DumpType::TIME_STATS_CSV )
    {
        std::stringstream ss;
        ss << commentPrefix(type) << "------------------------------------------------\n";
        ss << commentPrefix(type) << dumpName.RelativePath() << "\n";
        ss << commentPrefix(type) << "------------------------------------------------\n";
        m_pStream =
            new PrefixStream(
            ss.str(),
            llvm::raw_ostream::Colors::YELLOW,
            m_pStream,
            true );
    }
}

Dump::~Dump()
{
    std::ofstream asmFile(m_name.str());

    // Delete the stream first to flush all data to the underlying m_string.
    delete m_pStream;
    m_pStream = nullptr;

    asmFile << m_string;
    asmFile.close();
}

llvm::raw_ostream& Dump::stream() const
{
    return *m_pStream;
}

void DumpLLVMIRText(
    llvm::Module*             pModule,
    Dump                      const& dump,
    llvm::AssemblyAnnotationWriter*  optionalAnnotationWriter /* = nullptr */)
{
#if defined(IGC_DEBUG_VARIABLES)
        IGC::Debug::DumpLock();
        pModule->print(dump.stream(), optionalAnnotationWriter);
        IGC::Debug::DumpUnlock();
#endif
}

int PrintDebugMsgV(
    const Dump* dump,
    const char* fmt,
    va_list ap)
{
#if defined( _DEBUG ) || defined( _INTERNAL )
    if (dump)
    {
        char buf[512];
        int r;
        r = vsnprintf(buf, sizeof(buf), fmt, ap);
        buf[sizeof(buf) - 1] = '\0';
        dump->stream() << buf;
        return r;
    }
#endif
    return 0;
}

void PrintDebugMsg(
    const Dump* dump,
    const char* fmt,
    ...)
{
#if defined( _DEBUG ) || defined( _INTERNAL )
    va_list ap;
    va_start(ap, fmt);
    PrintDebugMsgV(dump, fmt, ap);
    va_end(ap);
#endif
}


AsmHash AsmHashOCL(const UINT* pShaderCode, size_t size)
{
    AsmHash hash;
    hash.value = iSTD::Hash(reinterpret_cast<const DWORD*>(pShaderCode), int_cast<DWORD>(size));
    return hash;
}

ShaderHash ShaderHashOCL(const UINT* pShaderCode, size_t size)
{
    ShaderHash hash;
    hash.asmHash = AsmHashOCL(pShaderCode, size);
    hash.nosHash = NosHash();
    return hash;
}

AsmHash AsmHashOGL(QWORD hash)
{
    AsmHash asmHash;
    asmHash.value = hash;
    return asmHash;
}

NosHash NosHashOGL(QWORD hash)
{
    NosHash nosHash;
    nosHash.value = hash;
    return nosHash;
}

ShaderHash ShaderHashOGL(QWORD glslHash, QWORD nosHash)
{
    ShaderHash shaderHash;
    shaderHash.asmHash = AsmHashOGL(glslHash);
    shaderHash.nosHash = NosHashOGL(nosHash);
    return shaderHash;
}

std::string GetDumpName(IGC::CShader* pProgram, const char* ext)
{
    return GetDumpNameObj(pProgram, ext).str();
}

DumpName GetDumpNameObj(IGC::CShader* pProgram, const char* ext)
{
    IGC::CodeGenContext* context = pProgram->GetContext();
    DumpName dumpName =
        IGC::Debug::DumpName(IGC::Debug::GetShaderOutputName())
        .Type(context->type)
        .Hash(context->hash);

    if (pProgram->GetShaderType() == ShaderType::OPENCL_SHADER)
    {
        IGC::OpenCLProgramContext* pOCLCtx = static_cast<IGC::OpenCLProgramContext*>(context);
        dumpName = dumpName.PostFix(pOCLCtx->GetStr(pProgram->entry));
    }
    else if (pProgram->GetShaderType() == ShaderType::PIXEL_SHADER)
    {
        IGC::CPixelShader* psProgram = static_cast<IGC::CPixelShader*>(pProgram);
        dumpName = dumpName.PSPhase(psProgram->GetPhase());
    }
    dumpName = dumpName.DispatchMode(pProgram->m_ShaderDispatchMode);
    dumpName = dumpName.SIMDSize(pProgram->m_dispatchSize).Extension(ext);
    return dumpName;
}

} // namespace Debug
} // namespace IGC
