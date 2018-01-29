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

///===========================================================================
/// This file contains types, enumerations, classes and other declarations 
/// used by IGC link optimization.
#include "Compiler/CodeGenPublic.h"
#include "Compiler/CISACodeGen/helper.h"
#include "Compiler/CISACodeGen/ShaderCodeGen.hpp"
#include "Compiler/MetaDataApi/MetaDataApi.h"
#include "Compiler/IGCPassSupport.h"

#include "Compiler/LinkOpt.hpp"

#include "common/debug/Debug.hpp"
#include "common/igc_regkeys.hpp"
#include "common/debug/Dump.hpp"
#include "common/LLVMUtils.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/ADT/SmallBitVector.h>
#include "common/LLVMWarningsPop.hpp"

#include <sstream>
#include <atomic>

#include <stdarg.h>

using namespace llvm;
using namespace std;

//
// Input/output intrinsics between shader stages
// VS:
//  - OUTPUT(x, y, z, w, usage, attrIdx)
//
// HS:
//  - HSinputVec(vertexIdx, attrIdx)
//  - OutputTessControlPoint(x, y, z, w, attrIdx, cpid, mask)
//  - PatchConstantOutput(x, y, z, w, attrIdx, mask)
//
// DS:
//  - DSCntrlPtInputVec(vertexIdx, attrIdx)
//  - DSPatchConstInputVec(inputIdx)
//
// GS:
//  - GSinputVec(vertexIdx, attrIdx)
//  - OUTPUTGS(x, y, z, w, usage, attrIdx, emitCount)
//
// PS:
//  - inputVec(attrIdx, interpolationMode)

namespace IGC
{

static const ShaderIOAnalysis::AnalysisType s_doIn = ShaderIOAnalysis::INPUT;
static const ShaderIOAnalysis::AnalysisType s_doOut = ShaderIOAnalysis::OUTPUT;

static const ShaderType s_vsType = ShaderType::VERTEX_SHADER;
static const ShaderType s_hsType = ShaderType::HULL_SHADER;
static const ShaderType s_dsType = ShaderType::DOMAIN_SHADER;
static const ShaderType s_gsType = ShaderType::GEOMETRY_SHADER;
static const ShaderType s_psType = ShaderType::PIXEL_SHADER;

static void LTODumpLLVMIR(CodeGenContext* ctx, const char* name)
{
    if (IGC_IS_FLAG_ENABLED(EnableLTODebug))
    {
        DumpLLVMIR(ctx, name);
    }
}

//////////////////////////////////////////////////////////////////////////////
// LinkOptContext
//////////////////////////////////////////////////////////////////////////////

static std::atomic<unsigned> NumLinkProgs;

void LinkOptContext::initDebugDump()
{
    if (IGC_IS_FLAG_ENABLED(EnableLTODebug))
    {
        std::ostringstream oss;

        oss << "link_" << NumLinkProgs;
        NumLinkProgs++;

        m_dump = new Debug::Dump(
            Debug::DumpName(oss.str())
            .Extension("log"),
            Debug::DumpType::DBG_MSG_TEXT);
    }
}

void LinkOptContext::closeDebugDump()
{
    if (m_dump)
    {
        delete m_dump;
        m_dump = nullptr;
    }
}

void LinkOptContext::debugPrint(const char* fmt, ...)
{
    if (IGC_IS_FLAG_ENABLED(EnableLTODebug) && this->m_dump)
    {
        va_list ap;
        va_start(ap, fmt);
        PrintDebugMsgV(this->m_dump, fmt, ap);
        va_end(ap);
    }
}

//////////////////////////////////////////////////////////////////////////////
// ShaderIOAnalysis
//////////////////////////////////////////////////////////////////////////////
char ShaderIOAnalysis::ID = 0;

void ShaderIOAnalysis::addInputDecl(llvm::GenIntrinsicInst* inst)
{
    Value* val = inst->getOperand(INPUT_ATTR_ARG);

    if (m_shaderType == ShaderType::PIXEL_SHADER)
    {
        uint imm = getImmValueU32(val);
        getContext()->addPSInput(inst, imm);
    }
}

void ShaderIOAnalysis::addDSCtrlPtInputDecl(llvm::GenIntrinsicInst* inst)
{
    assert(m_shaderType == ShaderType::DOMAIN_SHADER);
    if (isa<ConstantInt>(inst->getOperand(DSCTRLPTINPUT_CPID_ARG)))
    {
        uint ctrlIdx = getImmValueU32(inst->getOperand(DSCTRLPTINPUT_CPID_ARG));
        uint elemIdx = getImmValueU32(inst->getOperand(DSCTRLPTINPUT_ATTR_ARG));
        getContext()->addDSCtrlPtInput(inst, elemIdx, ctrlIdx);
    }
    else
    {
        getContext()->m_abortLTO = true;
    }
}

void ShaderIOAnalysis::addDSPatchConstInputDecl(llvm::GenIntrinsicInst* inst)
{
    if (isa<ConstantInt>(inst->getOperand(DSPATCHCONSTINPUT_ATTR_ARG)))
    {
        uint idx = getImmValueU32(inst->getOperand(DSPATCHCONSTINPUT_ATTR_ARG));
        getContext()->addDSPatchConstInput(inst, idx);
    }
    else
    {
        getContext()->m_abortLTO = true;
    }
}

void ShaderIOAnalysis::addPatchConstOutput(GenIntrinsicInst* inst)
{
    Value* index = inst->getOperand(PATCHCONSTOUTPUT_ATTR_ARG);
    uint imm = getImmValueU32(index);

    getContext()->addHSPatchConstOutput(inst, imm);
}

void ShaderIOAnalysis::addHSPatchConstInputDecl(llvm::GenIntrinsicInst* inst)
{
    if (isa<ConstantInt>(inst->getOperand(HSPATCHCONSTINPUT_ATTR_ARG)))
    {
        uint idx = getImmValueU32(inst->getOperand(HSPATCHCONSTINPUT_ATTR_ARG));
        getContext()->addHSPatchConstInput(inst, idx);
    }
    else
    {
        getContext()->m_abortLTO = true;
    }
}

void ShaderIOAnalysis::addOutputDecl(GenIntrinsicInst* inst)
{
    Value* usage = inst->getOperand(OUTPUT_USAGE_ARG);
    Value* index = inst->getOperand(OUTPUT_ATTR_ARG);
    ShaderOutputType otype = (ShaderOutputType)getImmValueU32(usage);

    if (!isa<ConstantInt>(index))
    {
        // GS may have variable index
        getContext()->m_abortLTO = true;
        return;
    }

    uint imm = getImmValueU32(index);

    if (otype == SHADER_OUTPUT_TYPE_DEFAULT)
    {
        switch (m_shaderType)
        {
        case ShaderType::VERTEX_SHADER:
            getContext()->addVSOutput(inst, imm);
            break;

        case ShaderType::DOMAIN_SHADER:
            getContext()->addDSOutput(inst, imm);
            break;

        case ShaderType::PIXEL_SHADER:
            break;

        case ShaderType::HULL_SHADER:
            getContext()->addHSPatchConstOutput(inst, imm);
            break;

        case ShaderType::GEOMETRY_SHADER:
            getContext()->addGSOutput(inst, imm);
            break;

        default:
            assert(false && "Unknow shader type for OUTPUT intrinsic");
        }
    }
}

void ShaderIOAnalysis::addVSInputDecl(GenIntrinsicInst* inst)
{
    assert(m_shaderType == ShaderType::VERTEX_SHADER);

    uint elemIdx = getImmValueU32(inst->getOperand(INPUT_ATTR_ARG));
    getContext()->addVSInput(inst, elemIdx);
}

void ShaderIOAnalysis::addHSInputDecl(GenIntrinsicInst* inst)
{
    assert(m_shaderType == ShaderType::HULL_SHADER);

    uint elemIdx = getImmValueU32(inst->getOperand(HSGSINPUT_ATTR_ARG));
    getContext()->addHSInput(inst, elemIdx);
}

void ShaderIOAnalysis::addGSInputDecl(GenIntrinsicInst* inst)
{
    assert(m_shaderType == ShaderType::GEOMETRY_SHADER);
    if (isa<ConstantInt>(inst->getOperand(HSGSINPUT_ATTR_ARG)))
    {
        uint elemIdx = getImmValueU32(inst->getOperand(HSGSINPUT_ATTR_ARG));
        getContext()->addGSInput(inst, elemIdx);
    }
    else
    {
        getContext()->m_abortLTO = true;
    }
}

// OutputTessControlPoint(x, y, z, w, i32 idx, i32 cpid)
void ShaderIOAnalysis::addHSCtrlPtOutputDecl(GenIntrinsicInst* inst)
{
    assert(m_shaderType == ShaderType::HULL_SHADER);

    uint attrIdx = getImmValueU32(inst->getOperand(OUTPUTTESSCTRLPT_ATTR_ARG));
    uint cpIdx = 0;
    getContext()->addHSCtrlPtOutput(inst, attrIdx, cpIdx);
}

// <4 x float> = HSOutputCntrlPtInputVec(i32 vertex, i32 attr)
void ShaderIOAnalysis::addHSOutputInputDecl(GenIntrinsicInst* inst)
{
    assert(m_shaderType == ShaderType::HULL_SHADER);
    uint attrIdx = getImmValueU32(inst->getOperand(1));
    getContext()->addHSOutputInput(inst, attrIdx);
}

void ShaderIOAnalysis::onGenIntrinsic(GenIntrinsicInst* inst)
{
    assert(inst != nullptr);
    switch (inst->getIntrinsicID())
    {
    case GenISAIntrinsic::GenISA_OUTPUT:
    case GenISAIntrinsic::GenISA_OUTPUTGS:
        // float x, float y, float z, float w, i32 usage, i32 index_mask
        if (doOutput())
            addOutputDecl(inst);
        break;

    case GenISAIntrinsic::GenISA_PatchConstantOutput:
        if (doOutput())
            addPatchConstOutput(inst);
        break;

    case GenISAIntrinsic::GenISA_DCL_inputVec:
        // i32 input_index, i32 interpolation_mode
        if (doInput())
            addInputDecl(inst);
        break;

    case GenISAIntrinsic::GenISA_Interpolate:
        // i32 input_index, v2f32 bary
        if (doInput())
            addInputDecl(inst);
        break;

    case GenISAIntrinsic::GenISA_DCL_DSCntrlPtInputVec:
        if (doInput())
            addDSCtrlPtInputDecl(inst);
        break;

    case GenISAIntrinsic::GenISA_DCL_ShaderInputVec:
        assert(m_shaderType == ShaderType::VERTEX_SHADER);
        if (doInput())
            addVSInputDecl(inst);
        break;

    case GenISAIntrinsic::GenISA_DCL_HSinputVec:
        if (doInput())
            addHSInputDecl(inst);
        break;

    case GenISAIntrinsic::GenISA_DCL_HSPatchConstInputVec:
        if (doInput())
            addHSPatchConstInputDecl(inst);
        break;

    case GenISAIntrinsic::GenISA_DCL_HSOutputCntrlPtInputVec:
        if (doOutput())
            addHSOutputInputDecl(inst);
        break;

    case GenISAIntrinsic::GenISA_OutputTessControlPoint:
        if (doOutput())
            addHSCtrlPtOutputDecl(inst);
        break;

    case GenISAIntrinsic::GenISA_DCL_DSPatchConstInputVec:
        if (doInput())
            addDSPatchConstInputDecl(inst);
        break;


    case GenISAIntrinsic::GenISA_DCL_GSinputVec:
        if (doInput())
            addGSInputDecl(inst);
        break;

    case GenISAIntrinsic::GenISA_DCL_input:
    case GenISAIntrinsic::GenISA_PHASE_INPUT:
        //Debug::PrintDebugMsg(m_dump, "%s\n", intrin_name);
        assert(false);
        break;

    case GenISAIntrinsic::GenISA_ScalarTessFactors:
    case GenISAIntrinsic::GenISA_InnerScalarTessFactors:
    case GenISAIntrinsic::GenISA_OuterScalarTessFactors:
    case GenISAIntrinsic::GenISA_DCL_DSInputTessFactor:
        // todo, handle LTO on tess factors.
    default:
        break;
    }
}

bool ShaderIOAnalysis::runOnFunction(Function& F)
{
    for (auto BI = F.begin(), BE = F.end(); BI != BE; BI++)
    {
        for (auto II = BI->begin(), IE = BI->end(); II != IE; II++)
        {
            if (isa<GenIntrinsicInst>(II))
            {
                onGenIntrinsic(cast<GenIntrinsicInst>(II));
            }
        }
    }
    return false;
}

// check whether shader input is using constant interpolation
static bool isConstInterpolationInput(GenIntrinsicInst* inst)
{
    if (inst->getIntrinsicID() != GenISAIntrinsic::GenISA_DCL_inputVec)
    {
        return false;
    }

    ConstantInt* modeVal = dyn_cast<ConstantInt>(
        inst->getOperand(ShaderIOAnalysis::INPUT_INTERPMODE_ARG));
    bool isConstInterpolation = false;

    if (modeVal != nullptr)
    {
        e_interpolation mode =
            static_cast<e_interpolation>(modeVal->getZExtValue());
        if (mode == EINTERPOLATION_CONSTANT)
        {
            isConstInterpolation = true;
        }
    }
    else
    {
        isConstInterpolation = true;
    }

    return isConstInterpolation;
}

static bool isConstInterpolationOutput(const vector<Value*>& outVals)
{
    bool constInterp = true;
    for (unsigned j = 1; j < outVals.size(); j++)
    {
        if (outVals[j] != outVals[0])
        {
            constInterp = false;
            break;
        }
    }
    return constInterp;
}

static ConstantFP* isConstFP(const vector<Value*>& vals)
{
    ConstantFP* cfp = nullptr;

    if (vals.size() > 0)
    {
        cfp = dyn_cast<ConstantFP>(vals[0]);
        if (cfp != nullptr)
        {
            for (unsigned i = 1; i < vals.size(); i++)
            {
                if (cfp != vals[i])
                {
                    return nullptr;
                }
            }
            return cfp;
        }
    }
    return cfp;
}

// Compact PS input attributes. Since "Constant Interpolation Enable" field
// in 3DSTATE_SBE will use 1 bit for 4 VS output attr components, so they
// will be grouped together in the front, then align index by 4 and followed
// by other input attrs.
//
// psIdxMap is the outputed mapping between old PS input index & new index
//   newPSAttrIndex = psIdxMap[oldPSAttrIndex]
static void compactPsInputs(
    CodeGenContext* psCtx,
    vector<int>& psIdxMap,
    ShaderType inputShaderType,
    VecOfIntrinVec& psIn,
    const VecOfVec<Value*>& outVals)
{
    unsigned nPsIn = 0;

    // cleanup all constants, gather const interpolation inputs
    for (unsigned i = 0; i < psIn.size(); i++)
    {
        IntrinVec& iv = psIn[i];
        if (iv.size() > 0)
        {
            bool clean = false;
            ConstantFP* cfp;

            if ((cfp = isConstFP(outVals[i])) != nullptr)
            {
                // if vs output is const, then just propogate it
                APFloat immf = cfp->getValueAPF();
                Value* psInConst = ConstantFP::get(toLLVMContext(*psCtx), immf);

                for (auto inst : iv)
                {
                    if (inst->getType()->isHalfTy())
                    {
                        // PS input is in low precision, lower the output const value
                        bool isExact = false;
                        APFloat immh = immf;
                        immh.convert(llvm::APFloat::IEEEhalf(), llvm::APFloat::rmTowardZero, &isExact);
                        psInConst = ConstantFP::get(toLLVMContext(*psCtx), immh);
                    }
                    inst->replaceAllUsesWith(psInConst);
                    inst->eraseFromParent();
                }
                clean = true;
            }
            else
            {
                // check and handle constant interpolate attrs
                bool isConstInterpInput = false;
                
                for (auto inst : iv)
                {
                    if (isConstInterpolationInput(inst))
                    {
                        isConstInterpInput = true;
                    }
                }

                // check for GS output can be promoted to const interpolation
                Value* constInterpV = nullptr;
                if (inputShaderType == ShaderType::GEOMETRY_SHADER &&
                    !psCtx->m_DriverInfo.WADisableConstInterpolationPromotion() &&
                    isConstInterpolationOutput(outVals[i]))
                {
                    isConstInterpInput = true;
                    constInterpV = ConstantInt::get(
                        Type::getInt32Ty(toLLVMContext(*psCtx)),
                        EINTERPOLATION_CONSTANT);
                }
                
                if (isConstInterpInput)
                {
                    Value* newIdx = ConstantInt::get(
                        Type::getInt32Ty(toLLVMContext(*psCtx)), nPsIn);
                    psIdxMap[i] = nPsIn++;

                    for (auto inst : iv)
                    {
                        inst->setOperand(ShaderIOAnalysis::INPUT_ATTR_ARG, newIdx);
                        if (constInterpV)
                        {
                            inst->setOperand(ShaderIOAnalysis::INPUT_INTERPMODE_ARG,
                                constInterpV);
                        }
                    }
                    clean = true;
                }
            }

            if (clean)
            {
                iv.clear();
            }
        }
    }
    nPsIn = iSTD::Align(nPsIn, 4);

    for (unsigned i = 0; i < psIn.size(); i++)
    {
        IntrinVec& iv = psIn[i];

        if (iv.size() > 0)
        {
            for (auto inst : iv)
            {
                if (psIdxMap[i] == -1)
                {
                    psIdxMap[i] = nPsIn++;
                }
                inst->setOperand(ShaderIOAnalysis::INPUT_ATTR_ARG,
                    ConstantInt::get(Type::getInt32Ty(toLLVMContext(*psCtx)), psIdxMap[i]));
            }
        }
    }
}

// Compact VS/DS output attributes based on PS attr index map, -1 in index
// map means it's not used by PS.
// Returns true if any output instruction was removed.
static bool compactVsDsOutput(
    CodeGenContext* vsdsCtx,
    const vector<int>& psIdxMap,
    VecOfIntrinVec& outInsts,
    VecOfVec<Value*>& outVals)
{
    unsigned numOut = outInsts.size() * 4;
    VecOfIntrinVec newOut(outInsts.size());
    unsigned nNewOut = 0;
    bool outputRemoved = false;

    // init all output attrs to undef
    // move output intrinsics to close to each other, for f64, we may see
    // non-contiguous output index 
    Value* undef = UndefValue::get(Type::getFloatTy(toLLVMContext(*vsdsCtx)));
    for (unsigned i = 0; i < outInsts.size(); i++)
    {
        if (outInsts[i].size() > 0)
        {
            for (auto intrin : outInsts[i])
            {
                intrin->setOperand(0, undef);
                intrin->setOperand(1, undef);
                intrin->setOperand(2, undef);
                intrin->setOperand(3, undef);
                intrin->setOperand(ShaderIOAnalysis::OUTPUT_ATTR_ARG,
                    ConstantInt::get(Type::getInt32Ty(toLLVMContext(*vsdsCtx)), nNewOut));
                newOut[nNewOut].push_back(intrin);
            }
            nNewOut++;
        }
    }

    // fill-in live attrs to new location based on psIdxMap
    SmallBitVector liveOutInst(nNewOut);
    for (unsigned i = 0; i < numOut; i++)
    {
        if (psIdxMap[i] >= 0)
        {
            unsigned newIdx = psIdxMap[i];

            if (newIdx / 4 >= nNewOut)
            {
                // output attr is promoted to const interpolation, this may 
                // case the increasing of PS input attrs, so we need to create
                // new output intrinsics in GS
                newOut.resize(newIdx / 4 + 1);
                liveOutInst.resize(newIdx / 4 + 1);
                for (auto intrin : newOut[nNewOut - 1])
                {
                    GenIntrinsicInst* newIntrin = cast<GenIntrinsicInst>(
                        intrin->clone());
                    newIntrin->setOperand(0, undef);
                    newIntrin->setOperand(1, undef);
                    newIntrin->setOperand(2, undef);
                    newIntrin->setOperand(3, undef);
                    newIntrin->setOperand(ShaderIOAnalysis::OUTPUT_ATTR_ARG,
                        ConstantInt::get(Type::getInt32Ty(toLLVMContext(*vsdsCtx)), newIdx / 4));
                    newIntrin->insertAfter(intrin);
                    newOut[newIdx / 4].push_back(newIntrin);
                }
                nNewOut = newIdx / 4 + 1;
            }
            
            for (unsigned j = 0; j < newOut[newIdx / 4].size(); j++)
            {
                newOut[newIdx / 4][j]->setOperand(newIdx % 4, outVals[i][j]);
            }
            liveOutInst.set(newIdx / 4);
        }
    }

    // cleanup unused output intrinsics
    for (unsigned i = 0; i < nNewOut; i++)
    {
        if (!liveOutInst.test(i))
        {
            for (auto intrin : newOut[i])
            {
                intrin->eraseFromParent();
                outputRemoved = true;
            }
        }
    }

    return outputRemoved;
}

// on enter: ps optimized, ds unified
// after: dead attr removed, vs/ds optimized
// Returns true if vs/ds output was removed.
static bool linkOptVsDsGsToPs(
    LinkOptContext* linkCtx,
    ShaderType outShaderType,
    CodeGenContext* outCtx,
    VecOfIntrinVec& outInsts)
{
    bool preStageOutputRemoved = false;

    VecOfIntrinVec &psIn = linkCtx->ps.inInsts;
    CodeGenContext* psCtx;

    psCtx = linkCtx->getPS();

    if (outInsts.size() == 0)
    {
        return preStageOutputRemoved;
    }

    unsigned numOut = outInsts.size() * 4;
    VecOfVec<Value*> outVals(numOut);
    vector<int> psIdxMap(numOut, -1);   // -1 marks unused VS output

    // gather vs output operands
    for (unsigned i = 0; i < outInsts.size(); i++)
    {
        if (outInsts[i].size() != 0)
        {
            for (auto intrin : outInsts[i])
            {
                for (unsigned j = 0; j < 4; j++)
                {
                    Value* val = intrin->getOperand(j);
                    outVals[i * 4 + j].push_back(val);
                }

            }
        }
    }

    // there are cases where PS has more inputs than VS outputs
    // e.g. gl_PointCoord
    if (psIn.size() > numOut)
    {
        psIn.resize(numOut);
    }
    // make sure all PS input will has a VS output
    Value* f0 = ConstantFP::get(Type::getFloatTy(toLLVMContext(*outCtx)), 0);
    for (unsigned i = 0; i < psIn.size(); i++)
    {
        if (psIn[i].size() > 0)
        {
            if (outInsts[i / 4].size() != 0 && outVals[i].size() != 0)
            {
                // for input with the output is undef, reset the output to 0.0f
                for (unsigned j = 0; j < outVals[i].size(); j++)
                {
                    if (isa<UndefValue>(outVals[i][j]))
                    {
                        outVals[i][j] = f0;
                    }
                }
            }
            else
            {
                // also for input with missing output, reset the output to 0.0f
                outVals[i].resize(psIn[i].size(), f0);
            }
        }
    }

    // compact PS input attrs (moving towards index 0)
    compactPsInputs(psCtx, psIdxMap, outShaderType, psIn, outVals);

    // whether we have output values removed, so that we can do DCR again
    // to remove operations producing them
    preStageOutputRemoved = compactVsDsOutput(outCtx, psIdxMap, outInsts, outVals);

    return preStageOutputRemoved;
}

static bool linkOptHsToDs(LinkOptContext* linkCtx)
{
    bool hsOutRemoved = false;
    vector<vector<list<GenIntrinsicInst*> > > &dsIn = linkCtx->ds.inInsts;
    VecOfIntrinVec &hsOut = linkCtx->hs.outInsts;
    map<unsigned, IntrinVec >& hsOutIn = linkCtx->hs.outIn;

    CodeGenContext* dsCtx = linkCtx->getDS();
    CodeGenContext* hsCtx = linkCtx->getHS();

    // optimize control points
    assert(hsOut.size() >= dsIn.size());
    unsigned nDead = 0;
    for (unsigned i = 0; i < dsIn.size(); i++)
    {
        if (dsIn[i].size() == 0 && hsOutIn.find(i) == hsOutIn.end())
        {
            nDead++;
            for (auto inst : hsOut[i])
            {
                inst->eraseFromParent();
                hsOutRemoved = true;
            }
        }
        else
        {
            if (nDead)
            {
                for (unsigned c = 0; c < dsIn[i].size(); c++)
                {
                    for (auto inst : dsIn[i][c])
                    {
                        inst->setOperand(ShaderIOAnalysis::DSCTRLPTINPUT_ATTR_ARG,
                            ConstantInt::get(Type::getInt32Ty(toLLVMContext(*dsCtx)), i - nDead));
                    }
                }
                for (auto inst : hsOut[i])
                {
                    inst->setOperand(ShaderIOAnalysis::OUTPUTTESSCTRLPT_ATTR_ARG,
                        ConstantInt::get(Type::getInt32Ty(toLLVMContext(*hsCtx)), i - nDead));
                }
                for (auto inst : hsOutIn[i])
                {
                    inst->setOperand(1,
                        ConstantInt::get(Type::getInt32Ty(toLLVMContext(*hsCtx)), i - nDead));
                }
            }
        }
    }

    nDead = 0;
    for (unsigned i = dsIn.size(); i < hsOut.size(); i++)
    {
        if (hsOutIn.find(i) == hsOutIn.end())
        {
            nDead++;
            for (auto inst : hsOut[i])
            {
                inst->eraseFromParent();
                hsOutRemoved = true;
            }
        }
        else
        {
            if (nDead)
            {
                for (auto inst : hsOut[i])
                {
                    inst->setOperand(ShaderIOAnalysis::OUTPUTTESSCTRLPT_ATTR_ARG,
                        ConstantInt::get(Type::getInt32Ty(toLLVMContext(*hsCtx)), i - nDead));
                }
                for (auto inst : hsOutIn[i])
                {
                    inst->setOperand(1,
                        ConstantInt::get(Type::getInt32Ty(toLLVMContext(*hsCtx)), i - nDead));
                }
            }
        }
    }

    // optimize patch constants
    VecOfIntrinVec& pcOut = linkCtx->hs.pcOut;
    VecOfIntrinVec& hspcIn = linkCtx->hs.pcIn;
    VecOfIntrinVec& dspcIn = linkCtx->ds.pcIn;

    assert(pcOut.size() >= hspcIn.size() && pcOut.size() >= dspcIn.size());
    unsigned sidx = 0;
    for (unsigned i = 0; i < pcOut.size(); i++)
    {
        if ( (i < hspcIn.size() && hspcIn[i].size()) ||
             (i < dspcIn.size() && dspcIn[i].size()) )
        {
            assert(pcOut[i].size());
            Value* dsv_sidx = ConstantInt::get(
                Type::getInt32Ty(toLLVMContext(*dsCtx)), sidx);
            Value* hsv_sidx = ConstantInt::get(
                Type::getInt32Ty(toLLVMContext(*hsCtx)), sidx);

            if (i < hspcIn.size())
            {
                for (auto inst : hspcIn[i])
                {
                    inst->setOperand(ShaderIOAnalysis::HSPATCHCONSTINPUT_ATTR_ARG, hsv_sidx);
                }
            }

            if (i < dspcIn.size())
            {
                for (auto inst : dspcIn[i])
                {
                    inst->setOperand(ShaderIOAnalysis::DSPATCHCONSTINPUT_ATTR_ARG, dsv_sidx);
                }
            }

            unsigned attrIdx;
            if (pcOut[i][0]->getIntrinsicID() == GenISAIntrinsic::GenISA_OUTPUT)
            {
                attrIdx = ShaderIOAnalysis::OUTPUT_ATTR_ARG;
            }
            else
            {
                assert(pcOut[i][0]->getIntrinsicID() ==
                    GenISAIntrinsic::GenISA_PatchConstantOutput);
                attrIdx = ShaderIOAnalysis::PATCHCONSTOUTPUT_ATTR_ARG;
            }

            for (auto inst : pcOut[i])
            {
                inst->setOperand(attrIdx, hsv_sidx);
            }

            pcOut[i].clear();

            sidx++;
        }
    }

    for (unsigned i = 0; i < pcOut.size(); i++)
    {
        if (pcOut[i].size())
        {
            for (auto inst : pcOut[i])
            {
                inst->eraseFromParent();
            }
        }
    }

    return hsOutRemoved;
}

// link opt between VecOfIntrinVec outputs & VecOfIntrinVec inputs
// VS -> HS or GS
// DS -> GS
static void linkOptVovToVov(LinkOptContext* linkCtx,
    CodeGenContext* outCtx,
    VecOfIntrinVec& outInsts,
    CodeGenContext* inCtx,
    VecOfIntrinVec& inInsts)
{
    unsigned nDead = 0;
    for (unsigned i = 0; i < inInsts.size(); i++)
    {
        if (inInsts[i].size() == 0)
        {
            if (outInsts[i].size() != 0)
            {
                assert(outInsts[i].size() == 1);
                nDead++;
                outInsts[i][0]->eraseFromParent();
            }
        }
        else
        {
            if (nDead)
            {
                GenIntrinsicInst* inst;
                for (unsigned j = 0; j < inInsts[i].size(); j++)
                {
                    inst = inInsts[i][j];
                    inst->setOperand(ShaderIOAnalysis::HSGSINPUT_ATTR_ARG,
                        ConstantInt::get(Type::getInt32Ty(toLLVMContext(*inCtx)), i - nDead));
                }
                assert(outInsts[i].size() == 1);
                inst = outInsts[i][0];
                inst->setOperand(ShaderIOAnalysis::OUTPUT_ATTR_ARG,
                    ConstantInt::get(Type::getInt32Ty(toLLVMContext(*outCtx)), i - nDead));
            }
        }
    }
    for (unsigned i = inInsts.size(); i < outInsts.size(); i++)
    {
        if (outInsts[i].size() != 0)
        {
            assert(outInsts[i].size() == 1);
            outInsts[i][0]->eraseFromParent();
        }
    }
}

bool runPasses( CodeGenContext* ctx, ...)
{   
    llvm::legacy::PassManager mpm;
    va_list ap;
    Pass* p;
    va_start(ap, ctx);
    while ((p = va_arg(ap, Pass*)))
    {
        mpm.add(p);
    }
    va_end(ap);

    return mpm.run(*ctx->getModule());
}

static void ltoPrepare(LinkOptContext* linkContext)
{
    linkContext->initDebugDump();

    linkContext->debugPrint("Link program:\n");
    for (int i = 0; i < LTO_NUM_SHADER_STAGES; i++)
    {
        CodeGenContext* pctx = linkContext->getContext((ShaderType)i);
        ShaderType stype = (ShaderType)i;
        if (pctx != nullptr)
        {
#if defined(_DEBUG)
            llvm::verifyModule(*pctx->getModule());
#endif
            linkContext->debugPrint(
                "  %s %016llx\n",
                ShaderTypeString[int(stype)],
                pctx->hash.getAsmHash());
        }
    }
}

static void ltoDestroy(LinkOptContext* linkContext)
{
    linkContext->closeDebugDump();
}

static ShaderType ltoToPS(LinkOptContext* ltoCtx)
{
    CodeGenContext* psCtx = ltoCtx->getPS();
    CodeGenContext* gsCtx = ltoCtx->getGS();
    CodeGenContext* dsCtx = ltoCtx->getDS();
    CodeGenContext* vsCtx = ltoCtx->getVS();
    CodeGenContext* prePsCtx;
    VecOfIntrinVec* prePsOuts;
    ShaderType prevType;

    if (gsCtx != nullptr)
    {
        prePsCtx = gsCtx;
        prevType = s_gsType;
        prePsOuts = &ltoCtx->gs.outInsts;
    }
    else
    if (dsCtx != nullptr)
    {
        prePsCtx = dsCtx;
        prevType = s_dsType;
        prePsOuts = &ltoCtx->ds.outInsts;
    }
    else
    {
        prePsCtx = vsCtx;
        prevType = s_vsType;
        prePsOuts = &ltoCtx->vs.outInsts;
    }

    ltoCtx->m_abortLTO = false;
    runPasses(prePsCtx,
        new ShaderIOAnalysis(ltoCtx, prevType, s_doOut),
        nullptr);
    runPasses(psCtx,
        new ShaderIOAnalysis(ltoCtx, s_psType, s_doIn),
        nullptr);

    if (!ltoCtx->m_abortLTO)
    {
        LTODumpLLVMIR(psCtx, "beLTOI"); LTODumpLLVMIR(prePsCtx, "beLTOO");
        if (linkOptVsDsGsToPs(ltoCtx, prevType, prePsCtx, *prePsOuts))
        {
            // Outputs were removed from preStage, so DCR make sense in such case.
            runPasses(prePsCtx,
                createDeadCodeEliminationPass(),
                nullptr);
        }
        LTODumpLLVMIR(psCtx, "afLTOI"); LTODumpLLVMIR(prePsCtx, "afLTOO");
    }

    return prevType;
}

static ShaderType ltoToGs(LinkOptContext* ltoCtx)
{
    CodeGenContext* gsCtx = ltoCtx->getGS();
    CodeGenContext* dsCtx = ltoCtx->getDS();
    CodeGenContext* vsCtx = ltoCtx->getVS();
    CodeGenContext* preGsCtx;
    VecOfIntrinVec* preGsOuts;
    ShaderType prevType;

    if (dsCtx != nullptr)
    {
        // ds -> gs
        prevType = s_dsType;
        preGsCtx = dsCtx;
        preGsOuts = &ltoCtx->ds.outInsts;
    }
    else
    {
        // vs -> gs
        prevType = s_vsType;
        preGsCtx = vsCtx;
        preGsOuts = &ltoCtx->vs.outInsts;
    }

    ltoCtx->m_abortLTO = false;
    runPasses(preGsCtx,
        new ShaderIOAnalysis(ltoCtx, prevType, s_doOut),
        nullptr);
    runPasses(gsCtx,
        llvm::createDeadCodeEliminationPass(),
        new ShaderIOAnalysis(ltoCtx, s_gsType, s_doIn),
        nullptr);

    if (!ltoCtx->m_abortLTO)
    {
        LTODumpLLVMIR(gsCtx, "beLTOI");    LTODumpLLVMIR(preGsCtx, "beLTOO");
        linkOptVovToVov(ltoCtx,
            preGsCtx, *preGsOuts,
            gsCtx, ltoCtx->gs.inInsts);
        LTODumpLLVMIR(gsCtx, "afLTOI");   LTODumpLLVMIR(preGsCtx, "afLTOO");
    }

    return prevType;
}

static ShaderType ltoToDs(LinkOptContext* ltoCtx)
{
    CodeGenContext* dsCtx = ltoCtx->getDS();
    CodeGenContext* hsCtx = ltoCtx->getHS();
    ShaderType prevType;

    assert(dsCtx != nullptr && hsCtx != nullptr);
    prevType = s_hsType;

    ltoCtx->m_abortLTO = false;

    runPasses(hsCtx,
        new ShaderIOAnalysis(ltoCtx, s_hsType, s_doOut),
        nullptr);
    runPasses(dsCtx,
        llvm::createDeadCodeEliminationPass(),
        new ShaderIOAnalysis(ltoCtx, s_dsType, s_doIn),
        nullptr);

    if (!ltoCtx->m_abortLTO)
    {
        LTODumpLLVMIR(dsCtx, "beLTOI");    LTODumpLLVMIR(hsCtx, "beLTOO");
        linkOptHsToDs(ltoCtx);
        LTODumpLLVMIR(dsCtx, "afLTOI");   LTODumpLLVMIR(hsCtx, "afLTOO");
    }

    return prevType;
}

static ShaderType ltoToHs(LinkOptContext* ltoCtx)
{
    CodeGenContext* hsCtx = ltoCtx->getHS();
    CodeGenContext* vsCtx = ltoCtx->getVS();
    ShaderType prevType;

    assert(vsCtx != nullptr);
    prevType = s_vsType;

    ltoCtx->m_abortLTO = false;
    runPasses(vsCtx,
        new ShaderIOAnalysis(ltoCtx, s_vsType, s_doOut),
        nullptr);
    runPasses(hsCtx,
        llvm::createDeadCodeEliminationPass(),
        new ShaderIOAnalysis(ltoCtx, s_hsType, s_doIn),
        nullptr);

    if (!ltoCtx->m_abortLTO)
    {
        LTODumpLLVMIR(hsCtx, "beLTOI");    LTODumpLLVMIR(vsCtx, "beLTOO");
        linkOptVovToVov(ltoCtx,
            vsCtx, ltoCtx->vs.outInsts,
            hsCtx, ltoCtx->hs.inInsts);
        LTODumpLLVMIR(hsCtx, "afLTOI");   LTODumpLLVMIR(vsCtx, "afLTOO");
    }

    return prevType;
}

void LinkOptIR(CodeGenContext* ctxs[])
{
    if (!IGC_IS_FLAG_ENABLED(EnableLTO))
    {
        return;
    }

    LinkOptContext linkContextObj;
    for (unsigned i = 0; i < LTO_NUM_SHADER_STAGES; i++)
    {
        linkContextObj.setContext((ShaderType)i, ctxs[i]);
    }

    LinkOptContext* ltoCtx = &linkContextObj;
    ltoPrepare(ltoCtx);

    CodeGenContext* psCtx = ltoCtx->getPS();
    CodeGenContext* gsCtx = ltoCtx->getGS();
    CodeGenContext* dsCtx = ltoCtx->getDS();
    CodeGenContext* hsCtx = ltoCtx->getHS();
    CodeGenContext* vsCtx = ltoCtx->getVS();

    if ((hsCtx != nullptr && dsCtx == nullptr) ||
        (hsCtx == nullptr && dsCtx != nullptr))
    {
        // skip some weired cases only having HS or DS
        ltoDestroy(ltoCtx);
        return;
    }

    ShaderType prevType;

    prevType = ltoToPS(ltoCtx);

    while (prevType != ShaderType::VERTEX_SHADER)
    {
        switch (prevType)
        {
        case ShaderType::GEOMETRY_SHADER:
            prevType = ltoToGs(ltoCtx);
            break;

        case ShaderType::DOMAIN_SHADER:
            prevType = ltoToDs(ltoCtx);
            prevType = ltoToHs(ltoCtx);
            break;

        case ShaderType::VERTEX_SHADER:
            break;

        default:
            assert(false && "Internal error in link opt!");
        }
    }
    if (psCtx)  DumpLLVMIR(psCtx, "lto");
    if (gsCtx)  DumpLLVMIR(gsCtx, "lto");
    if (dsCtx)  DumpLLVMIR(dsCtx, "lto");
    if (hsCtx)  DumpLLVMIR(hsCtx, "lto");
    if (vsCtx)  DumpLLVMIR(vsCtx, "lto");

    ltoDestroy(ltoCtx);
}

} // namespace IGC

