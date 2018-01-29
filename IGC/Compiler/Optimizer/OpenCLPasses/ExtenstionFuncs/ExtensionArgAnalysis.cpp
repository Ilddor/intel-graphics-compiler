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

#include "Compiler/Optimizer/OpenCLPasses/ExtenstionFuncs/ExtensionArgAnalysis.hpp"
#include "Compiler/IGCPassSupport.h"
#include "Compiler/Optimizer/OCLBIUtils.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/Function.h>
#include "common/LLVMWarningsPop.hpp"

using namespace llvm;
using namespace IGC;

// Register pass to igc-opt
#define PASS_FLAG "igc-extension-arg-analysis"
#define PASS_DESCRIPTION "Analyzes extenstion functions arguments"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS true
IGC_INITIALIZE_PASS_BEGIN(ExtensionArgAnalysis, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_DEPENDENCY(MetaDataUtilsWrapper)
IGC_INITIALIZE_PASS_END(ExtensionArgAnalysis, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

namespace IGC
{

    char ExtensionArgAnalysis::ID = 0;

    // VME Functions:

    enum VME_FUNCTIONS
    {
        VME_FUNCTION,
        ADVANCED_VME_FUNCTION,
        ADVANCED_VME_BIDIR_FUNCTION,
        NUM_VME_FUNCTIONS
    };

    static const llvm::StringRef VME_FUNCTION_STRINGS[] =
    {
        "block_motion_estimate_intel",
        "block_advanced_motion_estimate_check_intel",
        "block_advanced_motion_estimate_bidirectional_check_intel"
    };

    static_assert(
        sizeof(VME_FUNCTION_STRINGS) / sizeof(*VME_FUNCTION_STRINGS) == NUM_VME_FUNCTIONS,
        "VME_FUNCTION_STRINGS array needs to be in sync with VME_FUNCTIONS enum, fix me!");

    // VA functions:

    enum VA_FUNCTIONS
    {
        VA_FUNCTION_ERODE,
        VA_FUNCTION_DILATE,
        VA_FUNCTION_MIN_FILTER,
        VA_FUNCTION_MAX_FILTER,
        VA_FUNCTION_CONVOLVE,
        VA_FUNCTION_MINMAX,
        VA_FUNCTION_CENTROID,
        VA_FUNCTION_BOOL_CENTROID,
        VA_FUNCTION_BOOL_SUM,
        VA_FUNCTION_CONVOLVE_1D,
        VA_FUNCTION_CONVOLVE_PIXEL,
        VA_FUNCTION_LBP_IMAGE,
        VA_FUNCTION_LBP_CORRELATION,
        VA_FUNCTION_FLOODFILL,
        VA_FUNCTION_CORRELATION_SEARCH,
        NUM_VA_FUNCTIONS
    };

    static const llvm::StringRef VA_FUNCTION_STRINGS[] =
    {
        "erode_2d_intel",
        "dilate_2d_intel",
        "min_filter_2d_intel",
        "max_filter_2d_intel",
        "convolve_2d_intel",
        "minmax_2d_intel",
        "centroid_2d_intel",
        "bool_centroid_2d_intel",
        "bool_sum_2d_intel",
        "convolve_1d_intel",
        "convolve_pixel_intel",
        "lbp_image_intel",
        "lbp_correlation_intel",
        "floodfill_intel",
        "correlation_search_intel",
    };
    static_assert(
        sizeof(VA_FUNCTION_STRINGS) / sizeof(*VA_FUNCTION_STRINGS) == NUM_VA_FUNCTIONS,
        "VA_FUNCTION_STRINGS array needs to be in sync with VA_FUNCTIONS enum, fix me!");

    const IGCMD::ResourceExtensionTypeEnum VA_FUNCTION_SAMPLER_TYPES[] =
    {
        IGCMD::ResourceExtensionTypeEnum::MediaSamplerTypeErode,
        IGCMD::ResourceExtensionTypeEnum::MediaSamplerTypeDilate,
        IGCMD::ResourceExtensionTypeEnum::MediaSamplerTypeMinMaxFilter,
        IGCMD::ResourceExtensionTypeEnum::MediaSamplerTypeMinMaxFilter,
        IGCMD::ResourceExtensionTypeEnum::MediaSamplerTypeConvolve,
        IGCMD::ResourceExtensionTypeEnum::MediaSamplerTypeMinMax,
        IGCMD::ResourceExtensionTypeEnum::MediaSamplerTypeCentroid,
        IGCMD::ResourceExtensionTypeEnum::MediaSamplerTypeBoolCentroid,
        IGCMD::ResourceExtensionTypeEnum::MediaSamplerTypeBoolSum,
        // SKL+ functions:
        IGCMD::ResourceExtensionTypeEnum::MediaSamplerTypeConvolve,
        IGCMD::ResourceExtensionTypeEnum::MediaSamplerTypeConvolve,
        IGCMD::ResourceExtensionTypeEnum::MediaSamplerTypeLbp,
        IGCMD::ResourceExtensionTypeEnum::MediaSamplerTypeLbp,
        IGCMD::ResourceExtensionTypeEnum::MediaSamplerTypeFloodFill,
        IGCMD::ResourceExtensionTypeEnum::MediaSamplerTypeCorrelation,
    };
    static_assert(
        sizeof(VA_FUNCTION_SAMPLER_TYPES) / sizeof(*VA_FUNCTION_SAMPLER_TYPES) == NUM_VA_FUNCTIONS,
        "Sampler mapping array needs to be in sync with VA_FUNCTIONS enum, fix me!");

    ExtensionArgAnalysis::ExtensionArgAnalysis() : FunctionPass(ID)
    {
        m_extensionType = IGCMD::ResourceExtensionTypeEnum::NonExtensionType;

        initializeExtensionArgAnalysisPass(*PassRegistry::getPassRegistry());
    }

    void ExtensionArgAnalysis::visitCallInst(llvm::CallInst &CI)
    {
        auto *F = CI.getCalledFunction();

        if (!F)
        {
            return;
        }

        auto SetExtension = [&](int argIndex, IGCMD::ResourceExtensionTypeEnum expected, SmallPtrSet<Argument*, 3> &Args)
        {
            if (auto *pArg = dyn_cast<Argument>(CImagesBI::CImagesUtils::traceImageOrSamplerArgument(&CI, argIndex))) {
                if (m_ExtensionMap.count(pArg) != 0)
                {
                    if (m_ExtensionMap[pArg] != expected)
                    {
                        getAnalysis<CodeGenContextWrapper>().getCodeGenContext()->EmitError("Inconsistent use of image!");
                        return;
                    }
                }
                else
                {
                    m_ExtensionMap[pArg] = expected;
                    Args.insert(pArg);
                }
            }
        };

        //
        // If kernel has device-side VME built-ins, its simd size has to be 16 given that
        // those built-ins only work for SIMD16 kernels.
        //
        auto CheckandSetSIMD16 = [&]()
        {
            if (IGC::IGCMD::MetaDataUtils* pMdUtils = getAnalysis<MetaDataUtilsWrapper>().getMetaDataUtils())
            {
                auto funcIter = pMdUtils->findFunctionsInfoItem(CI.getParent()->getParent());
                if (funcIter != pMdUtils->end_FunctionsInfo())
                {
                    IGC::IGCMD::SubGroupSizeMetaDataHandle subGroupSize = funcIter->second->getSubGroupSize();
                    if (subGroupSize->hasValue())
                    {
                        if (subGroupSize->getSIMD_size() != 16)
                            getAnalysis<CodeGenContextWrapper>().getCodeGenContext()->EmitError("SIMD16 is expected");
                    }
                    else
                        subGroupSize->setSIMD_size(16);
                }
            }
        };


        StringRef name = F->getName();

        if (name.startswith("__builtin_IB_media_block_") || name == "__builtin_IB_media_block_rectangle_read")
        {
            SetExtension(0, IGCMD::ResourceExtensionTypeEnum::MediaResourceBlockType, m_MediaBlockArgs);
        }
        else if (name == "__builtin_IB_vme_send_fbr" ||
            name == "__builtin_IB_vme_send_ime")
        {
            SetExtension(3, IGCMD::ResourceExtensionTypeEnum::MediaResourceType, m_MediaArgs);
            SetExtension(4, IGCMD::ResourceExtensionTypeEnum::MediaResourceType, m_MediaArgs);
            CheckandSetSIMD16();
        }
        else if (name == "__builtin_IB_vme_send_sic")
        {
            SetExtension(3, IGCMD::ResourceExtensionTypeEnum::MediaResourceType, m_MediaArgs);
            SetExtension(4, IGCMD::ResourceExtensionTypeEnum::MediaResourceType, m_MediaArgs);
            SetExtension(5, IGCMD::ResourceExtensionTypeEnum::MediaResourceType, m_MediaArgs);
            CheckandSetSIMD16();
        }
        else if (name.startswith("__builtin_IB_vme_send_ime_new") ||
            name == "__builtin_IB_vme_send_sic_new" ||
            name == "__builtin_IB_vme_send_fbr_new")
        {
            // Handle image args.
            SetExtension(1, IGCMD::ResourceExtensionTypeEnum::MediaResourceType, m_MediaArgs);
            SetExtension(2, IGCMD::ResourceExtensionTypeEnum::MediaResourceType, m_MediaArgs);
            SetExtension(3, IGCMD::ResourceExtensionTypeEnum::MediaResourceType, m_MediaArgs);
            // Handle sampler arg.
            SetExtension(4, IGCMD::ResourceExtensionTypeEnum::MediaSamplerType, m_MediaSamplerArgs);
            CheckandSetSIMD16();
        }

    }

    bool ExtensionArgAnalysis::runOnFunction(Function &F)
    {
        m_ExtensionMap.clear();
        m_MediaArgs.clear();
        m_MediaBlockArgs.clear();
        m_extensionType = IGCMD::ResourceExtensionTypeEnum::NonExtensionType;
        m_vaArgs.clear();
        m_MediaSamplerArgs.clear();
        visit(F);

        StringRef funcName = F.getName();

        if (funcName == VME_FUNCTION_STRINGS[VME_FUNCTION] ||
            funcName == VME_FUNCTION_STRINGS[ADVANCED_VME_FUNCTION] ||
            funcName == VME_FUNCTION_STRINGS[ADVANCED_VME_BIDIR_FUNCTION])
        {
            // First function arg is the sampler
            auto arg = F.arg_begin();
            m_MediaSamplerArgs.insert(&(*arg));
        }

        m_extensionType = IGCMD::ResourceExtensionTypeEnum::NonExtensionType;

        for (int func = VA_FUNCTION_ERODE; func < NUM_VA_FUNCTIONS; ++func)
        {
            if (funcName.equals(VA_FUNCTION_STRINGS[func]))
            {
                // First function arg is the src image, second is the sampler,
                // and third arg is output buffer (ignored by this analysis).
                auto arg = F.arg_begin();
                for (int i = 0; i < 2; ++i)
                {
                    m_vaArgs.insert(&(*arg));
                    arg++;
                }

                m_extensionType = VA_FUNCTION_SAMPLER_TYPES[func];
            }
        }

        return false;
    }

} // namespace IGC
