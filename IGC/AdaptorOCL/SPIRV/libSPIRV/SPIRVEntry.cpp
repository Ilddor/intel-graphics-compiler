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

//===- SPIRVEntry.cpp - Base Class for SPIR-V Entities -----------*- C++ -*-===//
//
//                     The LLVM/SPIRV Translator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright (c) 2014 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimers.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimers in the documentation
// and/or other materials provided with the distribution.
// Neither the names of Advanced Micro Devices, Inc., nor the names of its
// contributors may be used to endorse or promote products derived from this
// Software without specific prior written permission.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH
// THE SOFTWARE.
//
//===----------------------------------------------------------------------===//
/// \file
///
/// This file implements base class for SPIR-V entities.
///
//===----------------------------------------------------------------------===//

#include "SPIRVEntry.h"
#include "SPIRVFunction.h"
#include "SPIRVInstruction.h"
#include "../SPIRVException.h"

namespace spv{

template<typename T>
SPIRVEntry* create() {
  return new T();
}

SPIRVEntry *
SPIRVEntry::create(Op OpCode) {
  switch (OpCode) {
#define _SPIRV_OP(x,...) case Op##x: return spv::create<SPIRV##x>();
#include "SPIRVOpCodeEnum.h"
#undef _SPIRV_OP
  default:
    spirv_assert(0 && "No factory the OpCode ");
  }
  return 0;
}

SPIRVErrorLog &
SPIRVEntry::getErrorLog()const {
  return Module->getErrorLog();
}

bool
SPIRVEntry::exist(SPIRVId TheId)const {
  return Module->exist(TheId);
}

SPIRVEntry *
SPIRVEntry::getOrCreate(SPIRVId TheId)const {
  SPIRVEntry *Entry = nullptr;
  bool Found = Module->exist(TheId, &Entry);
  if (!Found)
    return Module->addForward(TheId, nullptr);
  return Entry;
}

SPIRVValue *
SPIRVEntry::getValue(SPIRVId TheId)const {
  return get<SPIRVValue>(TheId);
}

SPIRVType *
SPIRVEntry::getValueType(SPIRVId TheId)const {
  return get<SPIRVValue>(TheId)->getType();
}

SPIRVDecoder
SPIRVEntry::getDecoder(std::istream& I){
  return SPIRVDecoder(I, *Module);
}

void
SPIRVEntry::setWordCount(SPIRVWord TheWordCount){
  WordCount = TheWordCount;
}

void
SPIRVEntry::setName(const std::string& TheName) {
  Name = TheName;
}

void
SPIRVEntry::setModule(SPIRVModule *TheModule) {
  assert(TheModule && "Invalid module");
  if (TheModule == Module)
    return;
  assert(Module == NULL && "Cannot change owner of entry");
  Module = TheModule;
}

// Read words from SPIRV binary and create members for SPIRVEntry.
// The word count and op code has already been read before calling this
// function for creating the SPIRVEntry. Therefore the input stream only
// contains the remaining part of the words for the SPIRVEntry.
void
SPIRVEntry::decode(std::istream &I) {
  spirv_assert (0 && "Not implemented");
}

std::vector<SPIRVValue *>
SPIRVEntry::getValues(const std::vector<SPIRVId>& IdVec)const {
  std::vector<SPIRVValue *> ValueVec;
  for (auto i:IdVec)
    ValueVec.push_back(getValue(i));
  return ValueVec;
}

std::vector<SPIRVType *>
SPIRVEntry::getValueTypes(const std::vector<SPIRVId>& IdVec)const {
  std::vector<SPIRVType *> TypeVec;
  for (auto i:IdVec)
    TypeVec.push_back(getValue(i)->getType());
  return TypeVec;
}

std::vector<SPIRVId>
SPIRVEntry::getIds(const std::vector<SPIRVValue *> ValueVec)const {
  std::vector<SPIRVId> IdVec;
  for (auto i:ValueVec)
    IdVec.push_back(i->getId());
  return IdVec;
}

SPIRVEntry *
SPIRVEntry::getEntry(SPIRVId TheId) const {
  return Module->getEntry(TheId);
}

void
SPIRVEntry::validateFunctionControlMask(SPIRVWord TheFCtlMask)
  const {
   SPIRVCK(TheFCtlMask <= (unsigned)SPIRVFunctionControlMaskKind::FunctionControlMaskMax,
      InvalidFunctionControlMask, "");
}

void
SPIRVEntry::validateValues(const std::vector<SPIRVId> &Ids)const {
  for (auto I:Ids)
    getValue(I)->validate();
}

void
SPIRVEntry::validateBuiltin(SPIRVWord TheSet, SPIRVWord Index)const {
  assert(TheSet != SPIRVWORD_MAX && Index != SPIRVWORD_MAX &&
      "Invalid builtin");
}

void
SPIRVEntry::addDecorate(const SPIRVDecorate *Dec){
  Decorates.insert(std::make_pair(Dec->getDecorateKind(), Dec));
  Module->addDecorate(Dec);
}

void
SPIRVEntry::addDecorate(Decoration Kind) {
  addDecorate(new SPIRVDecorate(Kind, this));
}

void
SPIRVEntry::addDecorate(Decoration Kind, SPIRVWord Literal) {
  addDecorate(new SPIRVDecorate(Kind, this, Literal));
}

void
SPIRVEntry::eraseDecorate(Decoration Dec){
  Decorates.erase(Dec);
}

void
SPIRVEntry::takeDecorates(SPIRVEntry *E){
  Decorates = std::move(E->Decorates);
}

void
SPIRVEntry::setLine(SPIRVLine *L){
  Line = L;
}

void SPIRVEntry::setDIScope(SPIRVExtInst* I) {
    diScope = I;
}

SPIRVExtInst* SPIRVEntry::getDIScope() {
    return diScope;
}

void
SPIRVEntry::takeLine(SPIRVEntry *E){
  Line = E->Line;
  if (Line == nullptr)
    return;
  E->Line = nullptr;
}

void
SPIRVEntry::addMemberDecorate(const SPIRVMemberDecorate *Dec){
  assert(canHaveMemberDecorates() && MemberDecorates.find(Dec->getPair()) ==
      MemberDecorates.end());
  MemberDecorates[Dec->getPair()] = Dec;
  Module->addDecorate(Dec);
}

void
SPIRVEntry::addMemberDecorate(SPIRVWord MemberNumber, Decoration Kind) {
  addMemberDecorate(new SPIRVMemberDecorate(Kind, MemberNumber, this));
}

void
SPIRVEntry::addMemberDecorate(SPIRVWord MemberNumber, Decoration Kind,
    SPIRVWord Literal) {
  addMemberDecorate(new SPIRVMemberDecorate(Kind, MemberNumber, this, Literal));
}

void
SPIRVEntry::eraseMemberDecorate(SPIRVWord MemberNumber, Decoration Dec){
  MemberDecorates.erase(std::make_pair(MemberNumber, Dec));
}

void
SPIRVEntry::takeMemberDecorates(SPIRVEntry *E){
  MemberDecorates = std::move(E->MemberDecorates);
}

void
SPIRVEntry::takeAnnotations(SPIRVForward *E){
  Module->setName(this, E->getName());
  takeDecorates(E);
  takeMemberDecorates(E);
  takeLine(E);
  if (OpCode == OpFunction)
    static_cast<SPIRVFunction *>(this)->takeExecutionModes(E);
}

// Check if an entry has Kind of decoration and get the literal of the
// first decoration of such kind at Index.
bool
SPIRVEntry::hasDecorate(Decoration Kind, size_t Index, SPIRVWord *Result)const {
  DecorateMapType::const_iterator Loc = Decorates.find(Kind);
  if (Loc == Decorates.end())
    return false;

  if (Result)
  {
    *Result = Loc->second->getLiteral(Index);
  }

  return true;
}

// Get literals of all decorations of Kind at Index.
std::set<SPIRVWord>
SPIRVEntry::getDecorate(Decoration Kind, size_t Index) const {
  auto Range = Decorates.equal_range(Kind);
  std::set<SPIRVWord> Value;
  for (auto I = Range.first, E = Range.second; I != E; ++I) {
    assert(Index < I->second->getLiteralCount() && "Invalid index");
    Value.insert(I->second->getLiteral(Index));
  }
  return Value;
}

bool
SPIRVEntry::hasLinkageType() const {
  return OpCode == OpFunction || OpCode == OpVariable;
}

SPIRVLinkageTypeKind
SPIRVEntry::getLinkageType() const {
  auto hasLinkageAttr = [&](SPIRVWord *Result)
  {
      auto Loc = Decorates.find(DecorationLinkageAttributes);
      if (Loc == Decorates.end())
          return false;

      if (Result)
      {
          auto *Dec = Loc->second;
          // Linkage Attributes has an arbitrary width string to start.  The
          // last Word is the linkage type.
          *Result = Dec->getLiteral(Dec->getLiteralCount() - 1);
      }

      return true;
  };

  assert(hasLinkageType());
  SPIRVWord LT = SPIRVLinkageTypeKind::LinkageTypeCount;
  if (!hasLinkageAttr(&LT))
     return SPIRVLinkageTypeKind::LinkageTypeInternal;
  return static_cast<SPIRVLinkageTypeKind>(LT);
}

void
SPIRVEntry::setLinkageType(SPIRVLinkageTypeKind LT) {
  assert(isValid(LT));
  assert(hasLinkageType());
  addDecorate(new SPIRVDecorate(DecorationLinkageAttributes, this, LT));
}

std::istream &
operator>>(std::istream &I, SPIRVEntry &E) {
  E.decode(I);
  return I;
}

SPIRVEntryPoint::SPIRVEntryPoint(SPIRVModule *TheModule,
  SPIRVExecutionModelKind TheExecModel, SPIRVId TheId,
  const std::string &TheName)
  :SPIRVAnnotation(TheModule->get<SPIRVFunction>(TheId),
   getSizeInWords(TheName) + 3), ExecModel(TheExecModel), Name(TheName){
}

void
SPIRVEntryPoint::decode(std::istream &I) {
  getDecoder(I) >> ExecModel >> Target >> Name;
  Module->setName(getOrCreateTarget(), Name);
  Module->addEntryPoint(ExecModel, Target);
}

void
SPIRVExecutionMode::decode(std::istream &I) {
  getDecoder(I) >> Target >> ExecMode;
  switch(ExecMode) {
  case SPIRVExecutionModeKind::ExecutionModeLocalSize:
  case SPIRVExecutionModeKind::ExecutionModeLocalSizeHint:
    WordLiterals.resize(3);
    break;
  case SPIRVExecutionModeKind::ExecutionModeInvocations:
  case SPIRVExecutionModeKind::ExecutionModeOutputVertices:
  case SPIRVExecutionModeKind::ExecutionModeVecTypeHint:
  case SPIRVExecutionModeKind::ExecutionModeSubgroupSize:
  case SPIRVExecutionModeKind::ExecutionModeSubgroupsPerWorkgroup:
    WordLiterals.resize(1);
    break;
  default:
    // Do nothing. Keep this to avoid VS2013 warning.
    break;
  }
  getDecoder(I) >> WordLiterals;
  getOrCreateTarget()->addExecutionMode(this);
}

SPIRVForward *
SPIRVAnnotationGeneric::getOrCreateTarget()const {
  SPIRVEntry *Entry = nullptr;
  bool Found = Module->exist(Target, &Entry);
  assert((!Found || Entry->getOpCode() == OpForward) &&
      "Annotations only allowed on forward");
  if (!Found)
    Entry = Module->addForward(Target, nullptr);
  return static_cast<SPIRVForward *>(Entry);
}

SPIRVName::SPIRVName(const SPIRVEntry *TheTarget, const std::string& TheStr)
  :SPIRVAnnotation(TheTarget, getSizeInWords(TheStr) + 2), Str(TheStr){
}

void
SPIRVName::decode(std::istream &I) {
  getDecoder(I) >> Target >> Str;
  Module->setName(getOrCreateTarget(), Str);
}

void
SPIRVName::validate() const {
  assert(WordCount == getSizeInWords(Str) + 2 && "Incorrect word count");
}

_SPIRV_IMP_DEC2(SPIRVString, Id, Str)
_SPIRV_IMP_DEC3(SPIRVMemberName, Target, MemberNumber, Str)

void
SPIRVLine::decode(std::istream &I) {
  getDecoder(I) >> FileName >> Line >> Column;
}

void
SPIRVLine::validate() const {
  assert(OpCode == OpLine);
  assert(WordCount == 5);
  assert(get<SPIRVEntry>(FileName)->getOpCode() == OpString);
  assert(Line != SPIRVWORD_MAX);
  assert(Column != SPIRVWORD_MAX);
}

void
SPIRVNoLine::decode(std::istream &I) {
}

void
SPIRVNoLine::validate() const {
    assert(OpCode == OpNoLine);
    assert(WordCount == 1);
}

void
SPIRVMemberName::validate() const {
  assert(OpCode == OpMemberName);
  assert(WordCount == getSizeInWords(Str) + FixedWC);
  assert(get<SPIRVEntry>(Target)->getOpCode() == OpTypeStruct);
  assert(MemberNumber < get<SPIRVTypeStruct>(Target)->getStructMemberCount());
}

SPIRVExtInstImport::SPIRVExtInstImport(SPIRVModule *TheModule, SPIRVId TheId,
    const std::string &TheStr):
  SPIRVEntry(TheModule, 2 + getSizeInWords(TheStr), OC, TheId), Str(TheStr){
  validate();
}

void
SPIRVExtInstImport::decode(std::istream &I) {
  getDecoder(I) >> Id >> Str;
  Module->importBuiltinSetWithId(Str, Id);
}

void
SPIRVExtInstImport::validate() const {
  SPIRVEntry::validate();
  assert(!Str.empty() && "Invalid builtin set");
}

void
SPIRVMemoryModel::decode(std::istream &I) {
  SPIRVAddressingModelKind AddrModel;
  SPIRVMemoryModelKind MemModel;
  getDecoder(I) >> AddrModel >> MemModel;
  Module->setAddressingModel(AddrModel);
  Module->setMemoryModel(MemModel);
}

void
SPIRVMemoryModel::validate() const {
  unsigned AM = Module->getAddressingModel();
  unsigned MM = Module->getMemoryModel();
  SPIRVCK(AM < SPIRVAddressingModelKind::AddressingModelCount, InvalidAddressingModel, "Actual is " + AM);
  SPIRVCK(MM < SPIRVMemoryModelKind::MemoryModelCount, InvalidMemoryModel, "Actual is " + MM);
}

void
SPIRVSource::decode(std::istream &I) {
  SpvSourceLanguage Lang = SpvSourceLanguageUnknown;
  SPIRVWord Ver = SPIRVWORD_MAX;
  getDecoder(I) >> Lang >> Ver;
  Module->setSourceLanguage(Lang, Ver);
}

SPIRVSourceExtension::SPIRVSourceExtension(SPIRVModule *M,
    const std::string &SS) : SPIRVEntryNoId(M, 1 + getSizeInWords(SS)), S(SS){}

void
SPIRVSourceExtension::decode(std::istream &I) {
  getDecoder(I) >> S;
  Module->getSourceExtension().insert(S);
}

SPIRVExtension::SPIRVExtension(SPIRVModule *M, const std::string &SS)
  :SPIRVEntryNoId(M, 1 + getSizeInWords(SS)), S(SS){}

void
SPIRVExtension::decode(std::istream &I) {
  getDecoder(I) >> S;
  Module->getExtension().insert(S);
}

SPIRVCapability::SPIRVCapability(SPIRVModule *M, SPIRVCapabilityKind K)
  :SPIRVEntryNoId(M, 2), Kind(K){
}

void
SPIRVCapability::decode(std::istream &I) {
  getDecoder(I) >> Kind;
  Module->addCapability(Kind);
}

void
SPIRVModuleProcessed::decode(std::istream &I) {
    getDecoder(I) >> S;
    Module->setModuleProcessed(S);
}

} // namespace spv

