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

//===- SPIRVModule.cpp - Class to represent SPIR-V module --------*- C++ -*-===//
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
/// This file implements Module class for SPIR-V.
///
//===----------------------------------------------------------------------===//

#include "SPIRVModule.h"
#include "SPIRVDebug.h"
#include "SPIRVDecorate.h"
#include "SPIRVEntry.h"
#include "SPIRVValue.h"
#include "SPIRVFunction.h"
#include "SPIRVInstruction.h"

namespace spv{

SPIRVModule::SPIRVModule()
{}

SPIRVModule::~SPIRVModule()
{}

class SPIRVModuleImpl : public SPIRVModule {
public:
  SPIRVModuleImpl():SPIRVModule(), NextId(0),
    SPIRVVersion(SPIRVVersionSupported::fullyCompliant),
    SPIRVGenerator(SPIRVGEN_AMDOpenSourceLLVMSPIRVTranslator),
    InstSchema(SPIRVISCH_Default),
    SrcLang(SpvSourceLanguageOpenCL_C),
    SrcLangVer(12),
    MemoryModel(SPIRVMemoryModelKind::MemoryModelOpenCL){
    AddrModel = sizeof(size_t) == 32 ? AddressingModelPhysical32 : AddressingModelPhysical64;
  };
  virtual ~SPIRVModuleImpl();

  // Object query functions
  bool exist(SPIRVId) const;
  bool exist(SPIRVId, SPIRVEntry **) const;
  SPIRVId getId(SPIRVId Id = SPIRVID_INVALID, unsigned Increment = 1);
  virtual SPIRVEntry *getEntry(SPIRVId Id) const;
  virtual void addUnknownStructField(
    SPIRVTypeStruct*, unsigned idx, SPIRVId id);
  virtual void resolveUnknownStructFields();
  bool hasDebugInfo() const { return !LineVec.empty();}

  // Error handling functions
  SPIRVErrorLog &getErrorLog() { return ErrLog;}
  SPIRVErrorCode getError(std::string &ErrMsg) { return ErrLog.getError(ErrMsg);}

  // Module query functions
  SPIRVAddressingModelKind getAddressingModel() { return AddrModel;}
  SPIRVExtInstSetKind getBuiltinSet(SPIRVId SetId) const;
  const SPIRVCapSet &getCapability() const { return CapSet;}
  const std::string &getCompileFlag() const { return CompileFlag;}
  std::string &getCompileFlag() { return CompileFlag;}
  void setCompileFlag(const std::string &options) { CompileFlag = options; }
  std::set<std::string> &getExtension() { return SPIRVExt;}
  SPIRVFunction *getFunction(unsigned I) const { return FuncVec[I];}
  SPIRVVariable *getVariable(unsigned I) const { return VariableVec[I];}
  virtual SPIRVValue *getValue(SPIRVId TheId) const;
  virtual std::vector<SPIRVValue *> getValues(const std::vector<SPIRVId>&)const;
  virtual std::vector<SPIRVId> getIds(const std::vector<SPIRVEntry *>&)const;
  virtual std::vector<SPIRVId> getIds(const std::vector<SPIRVValue *>&)const;
  virtual SPIRVType *getValueType(SPIRVId TheId)const;
  virtual std::vector<SPIRVType *> getValueTypes(const std::vector<SPIRVId>&)
      const;
  SPIRVMemoryModelKind getMemoryModel() { return MemoryModel;}
  virtual SPIRVConstant* getLiteralAsConstant(unsigned Literal);
  unsigned getNumEntryPoints(SPIRVExecutionModelKind EM) const {
    auto Loc = EntryPointVec.find(EM);
    if (Loc == EntryPointVec.end())
      return 0;
    return Loc->second.size();
  }
  SPIRVFunction *getEntryPoint(SPIRVExecutionModelKind EM, unsigned I) const {
    auto Loc = EntryPointVec.find(EM);
    if (Loc == EntryPointVec.end())
      return nullptr;
    spirv_assert(I < Loc->second.size());
    return get<SPIRVFunction>(Loc->second[I]);
  }
  unsigned getNumFunctions() const { return FuncVec.size();}
  unsigned getNumVariables() const { return VariableVec.size();}
  SpvSourceLanguage getSourceLanguage(SPIRVWord * Ver = nullptr) const {
    if (Ver)
      *Ver = SrcLangVer;
    return SrcLang;
  }
  std::set<std::string> &getSourceExtension() { return SrcExtension;}
  bool isEntryPoint(SPIRVExecutionModelKind, SPIRVId EP) const;
  const std::string &getModuleProcessed() const { return ModuleProcessed; }

  // Module changing functions
  bool importBuiltinSet(const std::string &, SPIRVId *);
  bool importBuiltinSetWithId(const std::string &, SPIRVId);
  void optimizeDecorates();
  void setAddressingModel(SPIRVAddressingModelKind AM) { AddrModel = AM;}
  void setAlignment(SPIRVValue *, SPIRVWord);
  void setMemoryModel(SPIRVMemoryModelKind MM) { MemoryModel = MM;}
  void setName(SPIRVEntry *E, const std::string &Name);
  void setSourceLanguage(SpvSourceLanguage Lang, SPIRVWord Ver) {
    SrcLang = Lang;
    SrcLangVer = Ver;
  }
  void setModuleProcessed(const std::string& MP) {
    ModuleProcessed = MP;
  }

  // Object creation functions
  template<class T> void addTo(std::vector<T *> &V, SPIRVEntry *E);
  virtual SPIRVEntry *addEntry(SPIRVEntry *E);
  virtual SPIRVString *getString(const std::string &Str);
  virtual SPIRVMemberName *addMemberName(SPIRVTypeStruct *ST,
      SPIRVWord MemberNumber, const std::string &Name);
  virtual SPIRVLine *addLine(SPIRVString *FileName, SPIRVWord Line,
      SPIRVWord Column);
  virtual void addCapability(SPIRVCapabilityKind);
  virtual const SPIRVDecorateGeneric *addDecorate(const SPIRVDecorateGeneric *);
  virtual SPIRVDecorationGroup *addDecorationGroup();
  virtual SPIRVDecorationGroup *addDecorationGroup(SPIRVDecorationGroup *Group);
  virtual SPIRVGroupDecorate *addGroupDecorate(SPIRVDecorationGroup *Group,
      const std::vector<SPIRVEntry *> &Targets);
  virtual SPIRVGroupDecorateGeneric *addGroupDecorateGeneric(
      SPIRVGroupDecorateGeneric *GDec);
  virtual SPIRVGroupMemberDecorate *addGroupMemberDecorate(
      SPIRVDecorationGroup *Group, const std::vector<SPIRVEntry *> &Targets);
  virtual void addEntryPoint(SPIRVExecutionModelKind ExecModel,
      SPIRVId EntryPoint);
  virtual SPIRVForward *addForward(SPIRVType *Ty);
  virtual SPIRVForward *addForward(SPIRVId, SPIRVType *Ty);
  virtual SPIRVFunction *addFunction(SPIRVFunction *);
  virtual SPIRVFunction *addFunction(SPIRVTypeFunction *, SPIRVId);
  virtual SPIRVEntry *replaceForward(SPIRVForward *, SPIRVEntry *);

  // Type creation functions
  template<class T> T * addType(T *Ty);
  virtual SPIRVTypeInt *addIntegerType(unsigned BitWidth);

  // Constant creation functions
  virtual SPIRVValue *addConstant(SPIRVValue *);
  virtual SPIRVValue *addConstant(SPIRVType *, uint64_t);

  // Instruction creation functions
  virtual SPIRVInstruction *addInstruction(SPIRVInstruction *Inst,
      SPIRVBasicBlock *BB);

  virtual SPIRVExtInst* getCompilationUnit()
  {
      for (auto& item : IdEntryMap)
      {
          if (item.second->getOpCode() == spv::Op::OpExtInst)
          {
              auto extInst = static_cast<SPIRVExtInst*>(item.second);
              if (extInst->getExtSetKind() == SPIRVExtInstSetKind::SPIRVEIS_DebugInfo &&
                  extInst->getExtOp() == OCLExtOpDbgKind::CompileUnit)
                  return extInst;
          }
      }

      return nullptr;
  }

  virtual std::vector<SPIRVExtInst*> getGlobalVars()
  {
      std::vector<SPIRVExtInst*> globalVars;

      for (auto& item : IdEntryMap)
      {
          if (item.second->getOpCode() == spv::Op::OpExtInst)
          {
              auto extInst = static_cast<SPIRVExtInst*>(item.second);
              if (extInst->getExtSetKind() == SPIRVExtInstSetKind::SPIRVEIS_DebugInfo &&
                  extInst->getExtOp() == OCLExtOpDbgKind::GlobalVariable)
                  globalVars.push_back(extInst);
          }
      }

      return globalVars;
  }

  // I/O functions
  friend std::istream & operator>>(std::istream &I, SPIRVModule& M);

private:
  SPIRVErrorLog ErrLog;
  SPIRVId NextId;
  SPIRVWord SPIRVVersion;
  SPIRVGeneratorKind SPIRVGenerator;
  SPIRVInstructionSchemaKind InstSchema;
  SpvSourceLanguage SrcLang;
  SPIRVWord SrcLangVer;
  std::set<std::string> SrcExtension;
  std::set<std::string> SPIRVExt;
  std::string CompileFlag;
  SPIRVAddressingModelKind AddrModel;
  SPIRVMemoryModelKind MemoryModel;
  std::string ModuleProcessed;

  typedef std::map<SPIRVId, SPIRVEntry *> SPIRVIdToEntryMap;
  typedef std::map<SPIRVTypeStruct*,
      std::vector<std::pair<unsigned, SPIRVId> > > SPIRVUnknownStructFieldMap;
  typedef std::unordered_set<SPIRVEntry *> SPIRVEntrySet;
  typedef std::set<SPIRVId> SPIRVIdSet;
  typedef std::vector<SPIRVId> SPIRVIdVec;
  typedef std::vector<SPIRVFunction *> SPIRVFunctionVector;
  typedef std::vector<SPIRVVariable *> SPIRVVariableVec;
  typedef std::vector<SPIRVLine *> SPIRVLineVec;
  typedef std::vector<SPIRVDecorationGroup *> SPIRVDecGroupVec;
  typedef std::vector<SPIRVGroupDecorateGeneric *> SPIRVGroupDecVec;
  typedef std::map<SPIRVId, SPIRVExtInstSetKind> SPIRVIdToBuiltinSetMap;
  typedef std::map<SPIRVExecutionModelKind, SPIRVIdSet> SPIRVExecModelIdSetMap;
  typedef std::map<SPIRVExecutionModelKind, SPIRVIdVec> SPIRVExecModelIdVecMap;
  typedef std::unordered_map<std::string, SPIRVString*> SPIRVStringMap;

  SPIRVIdToEntryMap IdEntryMap;
  SPIRVUnknownStructFieldMap UnknownStructFieldMap;
  SPIRVFunctionVector FuncVec;
  SPIRVVariableVec VariableVec;
  SPIRVEntrySet EntryNoId;         // Entries without id
  SPIRVIdToBuiltinSetMap IdBuiltinMap;
  SPIRVIdSet NamedId;
  SPIRVLineVec LineVec;
  SPIRVDecorateSet DecorateSet;
  SPIRVDecGroupVec DecGroupVec;
  SPIRVGroupDecVec GroupDecVec;
  SPIRVExecModelIdSetMap EntryPointSet;
  SPIRVExecModelIdVecMap EntryPointVec;
  SPIRVStringMap StrMap;
  SPIRVCapSet CapSet;
  std::map<unsigned, SPIRVTypeInt*> IntTypeMap;
  std::map<unsigned, SPIRVConstant*> LiteralMap;

  void layoutEntry(SPIRVEntry* Entry);
};

SPIRVModuleImpl::~SPIRVModuleImpl() {
    for (auto I : IdEntryMap)
        delete I.second;

    for (auto I : EntryNoId)
        delete I;
}

SPIRVLine*
SPIRVModuleImpl::addLine(SPIRVString* FileName,
    SPIRVWord Line, SPIRVWord Column) {
  auto L = add(new SPIRVLine(this, FileName->getId(), Line, Column));
  return L;
}

// Creates decoration group and group decorates from decorates shared by
// multiple targets.
void
SPIRVModuleImpl::optimizeDecorates() {
  for (auto I = DecorateSet.begin(), E = DecorateSet.end(); I != E;) {
    auto D = *I;
    if (D->getOpCode() == OpMemberDecorate) {
      ++I;
      continue;
    }
    auto ER = DecorateSet.equal_range(D);
    if (std::distance(ER.first, ER.second) < 2) {
      I = ER.second;
      continue;
    }
    auto G = new SPIRVDecorationGroup(this, getId());
    std::vector<SPIRVId> Targets;
    Targets.push_back(D->getTargetId());
    const_cast<SPIRVDecorateGeneric*>(D)->setTargetId(G->getId());
    G->getDecorations().insert(D);
    for (I = ER.first; I != ER.second; ++I) {
      auto E = *I;
      if (*E == *D)
        continue;
      Targets.push_back(E->getTargetId());
    }
    DecorateSet.erase(ER.first, ER.second);
    auto GD = new SPIRVGroupDecorate(G, Targets);
    DecGroupVec.push_back(G);
    GroupDecVec.push_back(GD);
  }
}

void
SPIRVModuleImpl::addCapability(SPIRVCapabilityKind Cap) {
  CapSet.insert(Cap);
}

SPIRVConstant*
SPIRVModuleImpl::getLiteralAsConstant(unsigned Literal) {
  auto Loc = LiteralMap.find(Literal);
  if (Loc != LiteralMap.end())
    return Loc->second;
  auto Ty = addIntegerType(32);
  auto V = new SPIRVConstant(this, Ty, getId(), static_cast<uint64_t>(Literal));
  LiteralMap[Literal] = V;
  addConstant(V);
  return V;
}

void
SPIRVModuleImpl::layoutEntry(SPIRVEntry* E) {
  auto OC = E->getOpCode();
  switch (OC) {
  case OpLine:
    addTo(LineVec, E);
    break;
  case OpVariable: {
    auto BV = static_cast<SPIRVVariable*>(E);
    if (!BV->getParent())
      addTo(VariableVec, E);
    }
    break;
  default:
    break;
  }
}

// Add an entry to the id to entry map.
// Assert if the id is mapped to a different entry.
// Certain entries need to be add to specific collectors to maintain
// logic layout of SPIRV.
SPIRVEntry *
SPIRVModuleImpl::addEntry(SPIRVEntry *Entry) {
    assert(Entry && "Invalid entry");
    if (Entry->hasId())
    {
        SPIRVId Id = Entry->getId();
        assert(Entry->getId() != SPIRVID_INVALID && "Invalid id");
        SPIRVEntry *Mapped = nullptr;
        if (exist(Id, &Mapped))
        {
            if (Mapped->getOpCode() == OpForward)
            {
                replaceForward(static_cast<SPIRVForward *>(Mapped), Entry);
            }
            else
            {
                assert(Mapped == Entry && "Id used twice");
            }
        }
        else
        {
            IdEntryMap[Id] = Entry;
        }
    }
    else
    {
        EntryNoId.insert(Entry);
    }

    Entry->setModule(this);

    layoutEntry(Entry);
    return Entry;
}

bool
SPIRVModuleImpl::exist(SPIRVId Id) const {
  return exist(Id, nullptr);
}

bool
SPIRVModuleImpl::exist(SPIRVId Id, SPIRVEntry **Entry) const {
  assert (Id != SPIRVID_INVALID && "Invalid Id");
  SPIRVIdToEntryMap::const_iterator Loc = IdEntryMap.find(Id);
  if (Loc == IdEntryMap.end())
    return false;
  if (Entry)
    *Entry = Loc->second;
  return true;
}

// If Id is invalid, returns the next available id.
// Otherwise returns the given id and adjust the next available id by increment.
SPIRVId
SPIRVModuleImpl::getId(SPIRVId Id, unsigned increment) {
  if (!isValid(Id))
    Id = NextId;
  else
    NextId = std::max(Id, NextId);
  NextId += increment;
  return Id;
}

SPIRVEntry *
SPIRVModuleImpl::getEntry(SPIRVId Id) const {
  assert (Id != SPIRVID_INVALID && "Invalid Id");
  SPIRVIdToEntryMap::const_iterator Loc = IdEntryMap.find(Id);
  spirv_assert (Loc != IdEntryMap.end() && "Id is not in map");
  return Loc->second;
}

void
SPIRVModuleImpl::addUnknownStructField(
    SPIRVTypeStruct* pStruct, unsigned idx, SPIRVId id) 
{
    UnknownStructFieldMap[pStruct].push_back(std::make_pair(idx, id));
}

void SPIRVModuleImpl::resolveUnknownStructFields()
{
    for (auto &kv : UnknownStructFieldMap)
    {
        auto *pStruct = kv.first;
        for (auto &indices : kv.second)
        {
            unsigned idx = indices.first;
            SPIRVId id   = indices.second;

            auto *pTy = static_cast<SPIRVType*>(getEntry(id));
            pStruct->setMemberType(idx, pTy);
        }
    }

    UnknownStructFieldMap.clear();
}

SPIRVExtInstSetKind
SPIRVModuleImpl::getBuiltinSet(SPIRVId SetId) const {
  auto Loc = IdBuiltinMap.find(SetId);
  spirv_assert(Loc != IdBuiltinMap.end() && "Invalid builtin set id");
  return Loc->second;
}

bool
SPIRVModuleImpl::isEntryPoint(SPIRVExecutionModelKind ExecModel, SPIRVId EP)
  const {
  assert(isValid(ExecModel) && "Invalid execution model");
  assert(EP != SPIRVID_INVALID && "Invalid function id");
  auto Loc = EntryPointSet.find(ExecModel);
  if (Loc == EntryPointSet.end())
    return false;
  return (Loc->second.count(EP) > 0);
}

// Module change functions
bool
SPIRVModuleImpl::importBuiltinSet(const std::string& BuiltinSetName,
    SPIRVId *BuiltinSetId) {
  SPIRVId TmpBuiltinSetId = getId();
  if (!importBuiltinSetWithId(BuiltinSetName, TmpBuiltinSetId))
    return false;
  if (BuiltinSetId)
    *BuiltinSetId = TmpBuiltinSetId;
  return true;
}

bool
SPIRVModuleImpl::importBuiltinSetWithId(const std::string& BuiltinSetName,
    SPIRVId BuiltinSetId) {
  SPIRVExtInstSetKind BuiltinSet = SPIRVEIS_Count;
  SPIRVCKRT(SPIRVBuiltinSetNameMap::rfind(BuiltinSetName, &BuiltinSet),
      InvalidBuiltinSetName, "Actual is " + BuiltinSetName);
  IdBuiltinMap[BuiltinSetId] = BuiltinSet;
  return true;
}

void
SPIRVModuleImpl::setAlignment(SPIRVValue *V, SPIRVWord A) {
  V->setAlignment(A);
}

void
SPIRVModuleImpl::setName(SPIRVEntry *E, const std::string &Name) {
  E->setName(Name);
  if (!E->hasId())
    return;
  if (!Name.empty())
    NamedId.insert(E->getId());
  else
    NamedId.erase(E->getId());
}

// Type creation functions
template<class T>
T *
SPIRVModuleImpl::addType(T *Ty) {
  add(Ty);
  if (!Ty->getName().empty())
    setName(Ty, Ty->getName());
  return Ty;
}

SPIRVTypeInt *
SPIRVModuleImpl::addIntegerType(unsigned BitWidth) {
  auto Loc = IntTypeMap.find(BitWidth);
  if (Loc != IntTypeMap.end())
    return Loc->second;
  auto Ty = new SPIRVTypeInt(this, getId(), BitWidth, false);
  IntTypeMap[BitWidth] = Ty;
  return addType(Ty);
}

SPIRVFunction *
SPIRVModuleImpl::addFunction(SPIRVFunction *Func) {
  FuncVec.push_back(add(Func));
  return Func;
}

SPIRVFunction *
SPIRVModuleImpl::addFunction(SPIRVTypeFunction *FuncType, SPIRVId Id) {
  return addFunction(new SPIRVFunction(this, FuncType,
      getId(Id, FuncType->getNumParameters() + 1)));
}

const SPIRVDecorateGeneric *
SPIRVModuleImpl::addDecorate(const SPIRVDecorateGeneric *Dec) {
  SPIRVId Id = Dec->getTargetId();
  SPIRVEntry *Target = nullptr;
  bool Found = exist(Id, &Target);
  assert (Found && "Decorate target does not exist");
  if (!Dec->getOwner())
    DecorateSet.insert(Dec);
  return Dec;
}

void
SPIRVModuleImpl::addEntryPoint(SPIRVExecutionModelKind ExecModel,
    SPIRVId EntryPoint){
  assert(isValid(ExecModel) && "Invalid execution model");
  assert(EntryPoint != SPIRVID_INVALID && "Invalid entry point");
  EntryPointSet[ExecModel].insert(EntryPoint);
  EntryPointVec[ExecModel].push_back(EntryPoint);
}

SPIRVForward *
SPIRVModuleImpl::addForward(SPIRVType *Ty) {
  return add(new SPIRVForward(this, Ty, getId()));
}

SPIRVForward *
SPIRVModuleImpl::addForward(SPIRVId Id, SPIRVType *Ty) {
  return add(new SPIRVForward(this, Ty, Id));
}

SPIRVEntry *
SPIRVModuleImpl::replaceForward(SPIRVForward *Forward, SPIRVEntry *Entry) {
  SPIRVId Id = Entry->getId();
  SPIRVId ForwardId = Forward->getId();
  if (ForwardId == Id)
    IdEntryMap[Id] = Entry;
  else {
    auto Loc = IdEntryMap.find(Id);
    spirv_assert(Loc != IdEntryMap.end());
    IdEntryMap.erase(Loc);
    Entry->setId(ForwardId);
    IdEntryMap[ForwardId] = Entry;
  }
  // Annotations include name, decorations, execution modes
  Entry->takeAnnotations(Forward);
  delete Forward;
  return Entry;
}

SPIRVValue *
SPIRVModuleImpl::addConstant(SPIRVValue *C) {
  return add(C);
}

SPIRVValue *
SPIRVModuleImpl::addConstant(SPIRVType *Ty, uint64_t V) {
  if (Ty->isTypeBool()) {
    if (V)
      return new SPIRVConstantTrue(this, Ty, getId());
    else
      return new SPIRVConstantFalse(this, Ty, getId());
  }
  return addConstant(new SPIRVConstant(this, Ty, getId(), V));
}

// Instruction creation functions

SPIRVInstruction *
SPIRVModuleImpl::addInstruction(SPIRVInstruction *Inst, SPIRVBasicBlock *BB) {
  if (BB)
    return BB->addInstruction(Inst);
  if (Inst->getOpCode() != OpSpecConstantOp)
    Inst = createSpecConstantOpInst(Inst);
  return static_cast<SPIRVInstruction *>(addConstant(Inst));
}

template<class T>
void SPIRVModuleImpl::addTo(std::vector<T*>& V, SPIRVEntry* E) {
  V.push_back(static_cast<T *>(E));
}

// The first decoration group includes all the previously defined decorates.
// The second decoration group includes all the decorates defined between the
// first and second decoration group. So long so forth.
SPIRVDecorationGroup*
SPIRVModuleImpl::addDecorationGroup() {
  return addDecorationGroup(new SPIRVDecorationGroup(this, getId()));
}

SPIRVDecorationGroup*
SPIRVModuleImpl::addDecorationGroup(SPIRVDecorationGroup* Group) {
  add(Group);
  Group->takeDecorates(DecorateSet);
  DecGroupVec.push_back(Group);
  assert(DecorateSet.empty());
  return Group;
}

SPIRVGroupDecorateGeneric*
SPIRVModuleImpl::addGroupDecorateGeneric(SPIRVGroupDecorateGeneric *GDec) {
  add(GDec);
  GDec->decorateTargets();
  GroupDecVec.push_back(GDec);
  return GDec;
}
SPIRVGroupDecorate*
SPIRVModuleImpl::addGroupDecorate(
    SPIRVDecorationGroup* Group, const std::vector<SPIRVEntry*>& Targets) {
  auto GD = new SPIRVGroupDecorate(Group, getIds(Targets));
  addGroupDecorateGeneric(GD);
  return GD;
}

SPIRVGroupMemberDecorate*
SPIRVModuleImpl::addGroupMemberDecorate(
    SPIRVDecorationGroup* Group, const std::vector<SPIRVEntry*>& Targets) {
  auto GMD = new SPIRVGroupMemberDecorate(Group, getIds(Targets));
  addGroupDecorateGeneric(GMD);
  return GMD;
}

SPIRVString*
SPIRVModuleImpl::getString(const std::string& Str) {
  auto Loc = StrMap.find(Str);
  if (Loc != StrMap.end())
    return Loc->second;
  auto S = add(new SPIRVString(this, getId(), Str));
  StrMap[Str] = S;
  return S;
}

SPIRVMemberName*
SPIRVModuleImpl::addMemberName(SPIRVTypeStruct* ST,
    SPIRVWord MemberNumber, const std::string& Name) {
  return add(new SPIRVMemberName(ST, MemberNumber, Name));
}

std::istream &
operator>> (std::istream &I, SPIRVModule &M) {
  SPIRVDecoder Decoder(I, M);
  SPIRVModuleImpl &MI = *static_cast<SPIRVModuleImpl*>(&M);

  SPIRVWord Magic;
  Decoder >> Magic;

  if (Magic != MagicNumber)
  {
      spirv_fatal_error("Invalid magic number");
  }

  Decoder >> MI.SPIRVVersion;

  bool supportVersion =
      MI.SPIRVVersion <= SPIRVVersionSupported::fullyCompliant;

  if (!supportVersion)
  {
      spirv_fatal_error("Unsupported SPIRV version number");
  }

  Decoder >> MI.SPIRVGenerator;

  // Bound for Id
  Decoder >> MI.NextId;

  Decoder >> MI.InstSchema;
  assert(MI.InstSchema == SPIRVISCH_Default && "Unsupported instruction schema");

  while(Decoder.getWordCountAndOpCode())
    Decoder.getEntry();

  MI.optimizeDecorates();
  return I;
}

SPIRVModule *
SPIRVModule::createSPIRVModule() {
  return new SPIRVModuleImpl;
}

SPIRVValue *
SPIRVModuleImpl::getValue(SPIRVId TheId)const {
  return get<SPIRVValue>(TheId);
}

SPIRVType *
SPIRVModuleImpl::getValueType(SPIRVId TheId)const {
  return get<SPIRVValue>(TheId)->getType();
}

std::vector<SPIRVValue *>
SPIRVModuleImpl::getValues(const std::vector<SPIRVId>& IdVec)const {
  std::vector<SPIRVValue *> ValueVec;
  for (auto i:IdVec)
    ValueVec.push_back(getValue(i));
  return ValueVec;
}

std::vector<SPIRVType *>
SPIRVModuleImpl::getValueTypes(const std::vector<SPIRVId>& IdVec)const {
  std::vector<SPIRVType *> TypeVec;
  for (auto i:IdVec)
    TypeVec.push_back(getValue(i)->getType());
  return TypeVec;
}

std::vector<SPIRVId>
SPIRVModuleImpl::getIds(const std::vector<SPIRVEntry *> &ValueVec)const {
  std::vector<SPIRVId> IdVec;
  for (auto i:ValueVec)
    IdVec.push_back(i->getId());
  return IdVec;
}

std::vector<SPIRVId>
SPIRVModuleImpl::getIds(const std::vector<SPIRVValue *> &ValueVec)const {
  std::vector<SPIRVId> IdVec;
  for (auto i:ValueVec)
    IdVec.push_back(i->getId());
  return IdVec;
}

SPIRVDbgInfo::SPIRVDbgInfo(SPIRVModule *TM)
:M(TM){
}

std::string
SPIRVDbgInfo::getEntryPointFileStr(SPIRVExecutionModelKind EM, unsigned I) {
  if (M->getNumEntryPoints(EM) == 0)
    return "";
  return getFunctionFileStr(M->getEntryPoint(EM, I));
}

std::string
SPIRVDbgInfo::getFunctionFileStr(SPIRVFunction *F) {
  if (F->hasLine())
    return F->getLine()->getFileNameStr();
  return "";
}

unsigned
SPIRVDbgInfo::getFunctionLineNo(SPIRVFunction *F) {
  if (F->hasLine())
    return F->getLine()->getLine();
  return 0;
}

bool IsSPIRVBinary(const std::string &Img) {
  if (Img.size() < sizeof(unsigned))
    return false;
  auto Magic = reinterpret_cast<const unsigned*>(Img.data());
  return *Magic == MagicNumber;
}

}

