#pragma once

#include <string>
#include <map>
#include <vector>

namespace llvm
{
    class Module;
    class Function;
    class Value;
    class GlobalVariable;
}

const unsigned int INPUT_RESOURCE_SLOT_COUNT = 128;
const unsigned int NUM_SHADER_RESOURCE_VIEW_SIZE = (INPUT_RESOURCE_SLOT_COUNT + 1) / 64;

const unsigned int g_c_maxNumberOfBufferPushed = 4;


namespace IGC
{
    enum FunctionTypeMD
    {
        UnknownFunction,
        VertexShaderFunction,
        HullShaderFunction,
        DomainShaderFunction,
        GeometryShaderFunction,
        PixelShaderFunction,
        ComputeShaderFunction,
        OpenCLFunction,
        UserFunction,
        NumberOfFunctionType,
    };

	struct ArgDependencyInfoMD
	{
		int argDependency = 0;
	};

    struct ComputeShaderSecondCompileInputInfoMD
    {
        int runtimeVal_ResWidthHeight = 0;
        int runtimeVal_LoopCount = 0;
        int runtimeVal_ConstantBufferSize = 0;
        bool isSecondCompile = false;
        int isRowMajor = 0;
        int numChannelsUsed = 0;
    };

    //to hold metadata of every function
    struct FunctionMetaData
    {
        FunctionTypeMD functionType = UnknownFunction;
        std::vector<unsigned> maxByteOffsets;
        bool IsInitializer = false;
        bool IsFinalizer = false;
        unsigned CompiledSubGroupsNumber = 0;
        bool isCloned = false;
        bool hasInlineVmeSamplers = false;
    };

    // isCloned member is added to mark whether a function is clone
    // of another one. If two kernels from a compilation unit invoke
    // the same callee, IGC ends up creating clone of the callee
    // to separate call graphs. But it doesnt create metadata nodes
    // so debug info for cloned function will be empty. Marking
    // function as clone and later in debug info iterating over
    // original function instead of clone helps emit out correct debug
    // info.

    //new structure to replace old Metatdata framework's CompilerOptions
    struct CompOptions
    {
        bool DenormsAreZero                             = false;
        bool CorrectlyRoundedDivSqrt                    = false;
        bool OptDisable                                 = false;
        bool MadEnable                                  = false;
        bool NoSignedZeros                              = false;
        bool UnsafeMathOptimizations                    = false;
        bool FiniteMathOnly                             = false;
        bool FastRelaxedMath                            = false;
        bool DashGSpecified                             = false;
        bool FastCompilation                            = false;
        bool UseScratchSpacePrivateMemory               = true;
        bool RelaxedBuiltins                            = false;
        bool SubgroupIndependentForwardProgressRequired = true;
        bool GreaterThan2GBBufferRequired               = true;
        bool GreaterThan4GBBufferRequired               = true;
        bool PushConstantsEnable                        = true;
        bool HasBufferOffsetArg                         = false;
    };

    struct ComputeShaderInfo
    {
        unsigned int waveSize = 0; // force a wave size
        std::vector<ComputeShaderSecondCompileInputInfoMD> ComputeShaderSecondCompile;
    };

    struct VertexShaderInfo
    {
        int DrawIndirectBufferIndex = -1;
    };

    struct PixelShaderInfo
    {
        unsigned char BlendStateDisabledMask = 0;
        bool SkipSrc0Alpha                   = false;
        bool DualSourceBlendingDisabled      = false;
        bool ForceEnableSimd32               = false; // forces compilation of simd32; bypass heuristics
        bool outputDepth                     = false;
        bool outputStencil                   = false;
        bool outputMask                      = false;
        bool blendToFillEnabled              = false;
        bool forceEarlyZ                     = false;   // force earlyz test
        bool hasVersionedLoop                = false;   // if versioned by customloopversioning
        std::vector<int> blendOptimizationMode;
        std::vector<int> colorOutputMask;
    };

	struct SInputDesc
	{
		unsigned int index = 0;
		int argIndex = 0;
		int interpolationMode = 0;
	};

	// SimplePushInfo holding the argument number in map so that we can retrieve relavent Argument as a value pointer from Function
	struct SimplePushInfo
	{
		unsigned int cbIdx = 0;
		unsigned int offset = 0;
		unsigned int size = 0;

		std::map<unsigned int, int> simplePushLoads;
	};

	struct ConstantAddress
	{
		unsigned int bufId = 0;
		unsigned int eltId = 0;
        int size = 0;
	};

	bool operator < (const ConstantAddress &a, const ConstantAddress &b);

    struct StatelessPushInfo
    {
        unsigned int addressOffset = 0;
        bool isStatic = false;
    };

	// simplePushInfoArr needs to be initialized to a vector of size g_c_maxNumberOfBufferPushed, which we are doing in module MD initialization done in code gen context
	// All the pushinfo below is mapping to an argument number (int) so that we can retrieve relevant Argument as a value pointer from Function
    struct PushInfo
    {
        std::vector<StatelessPushInfo> pushableAddresses;
        unsigned int MaxNumberOfPushedBuffers = 0; ///> specifies the maximum number of buffers available for the simple push mechanism for current shader.

        unsigned int inlineConstantBufferSlot = 0xFFFFFFFF; // slot of the inlined constant buffer

		std::map<ConstantAddress, int> constants;
		std::map<unsigned int, SInputDesc> inputs;
		std::map<unsigned int, int> constantReg;
		std::map<uint64_t, int> statelessLoads;
		std::vector<SimplePushInfo> simplePushInfoArr;
		unsigned int simplePushBufferUsed = 0;

		std::vector<ArgDependencyInfoMD> pushAnalysisWIInfos;
    };

    struct InlineProgramScopeBuffer
	{
		int alignment;
		std::vector<unsigned char> Buffer;
	};
	
	struct ImmConstantInfo
	{
		std::vector<char> data;
	};

	struct PointerProgramBinaryInfo
	{
		int PointerBufferIndex;
		int PointerOffset;
		int PointeeAddressSpace;
		int PointeeBufferIndex;
	};

    struct ShaderData
    {
        unsigned int numReplicas = 0;
    };

    //metadata for the entire module
    struct ModuleMetaData
    {
        ModuleMetaData();
		bool isPrecise = false;
        CompOptions compOpt;
        std::map<llvm::Function*, IGC::FunctionMetaData>   FuncMD;
        PushInfo pushInfo;
        VertexShaderInfo vsInfo;
        PixelShaderInfo psInfo;
        ComputeShaderInfo csInfo;
        std::map<ConstantAddress, std::vector<char>>    inlineDynConstants;
		ImmConstantInfo immConstant;
		std::vector<InlineProgramScopeBuffer> inlineConstantBuffers;
		std::vector<InlineProgramScopeBuffer> inlineGlobalBuffers;
		std::vector<PointerProgramBinaryInfo> GlobalPointerProgramBinaryInfos;
		std::vector<PointerProgramBinaryInfo> ConstantPointerProgramBinaryInfos;
        unsigned int MinNOSPushConstantSize = 0;
        std::map<llvm::GlobalVariable*, int> inlineProgramScopeOffsets;
        ShaderData shaderData;
        bool UseBindlessImage = false;
        unsigned int privateMemoryPerWI = 0;
        std::vector<uint64_t> m_ShaderResourceViewMcsMask;
    };
    void serialize(const IGC::ModuleMetaData &moduleMD, llvm::Module* module);
    void deserialize(IGC::ModuleMetaData &deserializedMD, const llvm::Module* module);
}
