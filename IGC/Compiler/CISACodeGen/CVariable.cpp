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

#include "Compiler/CISACodeGen/CVariable.hpp"

using namespace IGC;

void CVariable::ResolveAlias()
{
    // If a variable alias another alias, make it point to the main variable
    CVariable* aliasVar = m_alias;
    assert(aliasVar);
    uint offset = m_aliasOffset;
    while (aliasVar->m_alias != nullptr)
    {
        offset += aliasVar->m_aliasOffset;
        aliasVar = aliasVar->m_alias;
    }
    m_alias = aliasVar;
    assert((offset < (UINT16_MAX)) && "offset > higher than 64k");

    m_aliasOffset = (uint16_t) offset;
}

/// CVariable constructor, for most generic cases
///
CVariable::CVariable(uint16_t nbElement, bool uniform, VISA_Type type, 
                     e_varType varType, e_alignment align, bool vectorUniform, 
                     uint16_t numberOfInstance) :
    m_immediateValue(0),
    m_alias(nullptr),
    m_nbElement(nbElement),
    m_aliasOffset(0),
    m_numberOfInstance(int_cast<uint8_t>(numberOfInstance)),
    m_type(type),
    m_varType(varType),
    m_align(align),
    m_uniform(uniform),
    m_isImmediate(false),
    m_subspanUse(false),
    m_uniformVector(vectorUniform),
    m_undef(false),
    m_isUnpacked(false)
{
}

static unsigned
getAlignment(e_alignment align)
{
    switch (align)
    {
    case EALIGN_BYTE:   return 1;
    case EALIGN_WORD:   return 2;
    case EALIGN_DWORD:  return 4;
    case EALIGN_QWORD:  return 8;
    case EALIGN_OWORD:  return 16;
    case EALIGN_GRF:    return 32;
    case EALIGN_2GRF:   return 64;
    default:
        break;
    }
    return 1;
}

static e_alignment
getAlignment(unsigned off)
{
    switch (off)
    {
    case 1: return EALIGN_BYTE;
    case 2: return EALIGN_WORD;
    case 4: return EALIGN_DWORD;
    case 8: return EALIGN_QWORD;
    case 16: return EALIGN_OWORD;
    case 32: return EALIGN_GRF;
    case 64: return EALIGN_2GRF;
    default:
        break;
    }

    return EALIGN_BYTE;
}

static e_alignment
updateAlign(e_alignment align, unsigned offset)
{
    assert(align != EALIGN_AUTO);
    return getAlignment(int_cast<unsigned int>(llvm::MinAlign(getAlignment(align), offset)));
}

/// CVariable constructor, for alias
///
CVariable::CVariable(CVariable* var, VISA_Type type, uint16_t offset, 
                     uint16_t numElements, bool uniform) : 
    m_immediateValue(0),
    m_alias(var),
    m_aliasOffset(offset),
    m_numberOfInstance(1),
    m_type(type),
    m_uniform(uniform),
    m_isImmediate(false),
    m_subspanUse(false),
    m_uniformVector(false),
    m_undef(false),
    m_isUnpacked(false)
{
    if (numElements)
    {
        m_nbElement = numElements;
    }
    else
    {
        m_nbElement = var->m_nbElement * CEncoder::GetCISADataTypeSize(var->m_type) / CEncoder::GetCISADataTypeSize(m_type);
    }
    m_align = updateAlign(var->m_align, offset);
    m_subspanUse = var->m_subspanUse;
    m_numberOfInstance = var->m_numberOfInstance;
    assert(var->m_varType == EVARTYPE_GENERAL && "only general variable can have alias");
    m_varType = EVARTYPE_GENERAL;
}

/// CVariable constructor, for immediate
///
CVariable::CVariable(uint64_t immediate, VISA_Type type) : 
    m_immediateValue(immediate),
    m_alias(nullptr),
    m_nbElement(1),
    m_numberOfInstance(1),
    m_type(type),
    m_varType(EVARTYPE_GENERAL),
    m_uniform(true),
    m_isImmediate(true),
    m_subspanUse(false),
    m_uniformVector(false),
    m_undef(false),
    m_isUnpacked(false)
{

}

/// CVariable constructor, for undef
///
CVariable::CVariable(VISA_Type type) :
    m_immediateValue(0),
    m_alias(nullptr),
    m_nbElement(0),
    m_numberOfInstance(1),
    m_type(type),
    m_varType(EVARTYPE_GENERAL),
    m_uniform(true),
    m_isImmediate(true),
    m_subspanUse(false),
    m_uniformVector(false),
    m_undef(true),
    m_isUnpacked(false)
{
    // undef variable are represented as immediate but can considered as trash data
}
