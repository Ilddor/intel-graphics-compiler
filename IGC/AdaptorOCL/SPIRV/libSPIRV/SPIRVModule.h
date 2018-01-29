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

//===- SPIRVModule.h - Class to represent a SPIR-V module --------*- C++ -*-===//
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
/// This file defines Module class for SPIR-V.
///
//===----------------------------------------------------------------------===//

#ifndef SPIRVMODULE_HPP_
#define SPIRVMODULE_HPP_

#include "SPIRVEntry.h"

#include <iostream>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spv{

class SPIRVBasicBlock;
class SPIRVConstant;
class SPIRVEntry;
class SPIRVFunction;
class SPIRVInstruction;
class SPIRVType;
class SPIRVTypeArray;
class SPIRVTypeBool;
class SPIRVTypeFloat;
class SPIRVTypeFunction;
class SPIRVTypeInt;
class SPIRVTypeOpaque;
class SPIRVTypePointer;
class SPIRVTypeImage;
class SPIRVTypeSampler;
class SPIRVTypeSampledImage;
class SPIRVTypeStruct;
class SPIRVTypeVector;
class SPIRVTypeVoid;
class SPIRVTypePipe;
class SPIRVTypeNamedBarrier;
class SPIRVValue;
class SPIRVVariable;
class SPIRVDecorateGeneric;
class SPIRVDecorationGroup;
class SPIRVGroupDecorate;
class SPIRVGroupMemberDecorate;
class SPIRVGroupDecorateGeneric;
class SPIRVInstTemplateBase;

typedef SPIRVBasicBlock SPIRVLabel;
struct SPIRVTypeImageDescriptor;

class SPIRVModule
{
public:
  typedef std::set<SPIRVCapabilityKind> SPIRVCapSet;
  static SPIRVModule* createSPIRVModule();
  SPIRVModule();
  virtual ~SPIRVModule();

  // Object query functions
  virtual bool exist(SPIRVId) const = 0;
  virtual bool exist(SPIRVId, SPIRVEntry **)const = 0;
  template<class T> T* get(SPIRVId Id) const
  {
    return static_cast<T*>(getEntry(Id));
  }
  virtual SPIRVEntry *getEntry(SPIRVId) const = 0;
  virtual void addUnknownStructField(
      SPIRVTypeStruct*, unsigned idx, SPIRVId id) = 0;
  virtual void resolveUnknownStructFields() = 0;
  virtual bool hasDebugInfo() const = 0;

  // Error handling functions
  virtual SPIRVErrorLog &getErrorLog() = 0;
  virtual SPIRVErrorCode getError(std::string&) = 0;

  // Module query functions
  virtual SPIRVAddressingModelKind getAddressingModel() = 0;
  virtual const SPIRVCapSet &getCapability() const = 0;
  virtual SPIRVExtInstSetKind getBuiltinSet(SPIRVId) const = 0;
  virtual std::string &getCompileFlag() = 0;
  virtual void setCompileFlag(const std::string &options) = 0;
  virtual const std::string &getCompileFlag() const = 0;
  virtual SPIRVFunction *getEntryPoint(SPIRVExecutionModelKind, unsigned) const = 0;
  virtual std::set<std::string> &getExtension() = 0;
  virtual SPIRVFunction *getFunction(unsigned) const = 0;
  virtual SPIRVVariable *getVariable(unsigned) const = 0;
  virtual SPIRVMemoryModelKind getMemoryModel() = 0;
  virtual unsigned getNumFunctions() const = 0;
  virtual unsigned getNumEntryPoints(SPIRVExecutionModelKind) const = 0;
  virtual unsigned getNumVariables() const = 0;
  virtual SpvSourceLanguage getSourceLanguage(SPIRVWord *) const = 0;
  virtual std::set<std::string> &getSourceExtension() = 0;
  virtual SPIRVValue *getValue(SPIRVId TheId)const = 0;
  virtual std::vector<SPIRVValue *> getValues(const std::vector<SPIRVId>&)const = 0;
  virtual std::vector<SPIRVId> getIds(const std::vector<SPIRVEntry *>&)const = 0;
  virtual std::vector<SPIRVId> getIds(const std::vector<SPIRVValue *>&)const = 0;
  virtual SPIRVType *getValueType(SPIRVId TheId)const = 0;
  virtual std::vector<SPIRVType *> getValueTypes(const std::vector<SPIRVId>&) const = 0;
  virtual SPIRVConstant* getLiteralAsConstant(unsigned Literal) = 0;
  virtual bool isEntryPoint(SPIRVExecutionModelKind, SPIRVId) const = 0;

  // Module changing functions
  virtual bool importBuiltinSet(const std::string &, SPIRVId *) = 0;
  virtual bool importBuiltinSetWithId(const std::string &, SPIRVId) = 0;
  virtual void setAddressingModel(SPIRVAddressingModelKind) = 0;
  virtual void setAlignment(SPIRVValue *, SPIRVWord) = 0;
  virtual void setMemoryModel(SPIRVMemoryModelKind) = 0;
  virtual void setName(SPIRVEntry *, const std::string&) = 0;
  virtual void setSourceLanguage(SpvSourceLanguage, SPIRVWord) = 0;
  virtual void optimizeDecorates() = 0;
  virtual void setModuleProcessed(const std::string& MP) = 0;

  // Object creation functions
  template<class T> T *add(T *Entry) { addEntry(Entry); return Entry; }
  virtual SPIRVEntry *addEntry(SPIRVEntry *) = 0;
  virtual SPIRVString *getString(const std::string &Str) = 0;
  virtual SPIRVMemberName *addMemberName(SPIRVTypeStruct *ST,
      SPIRVWord MemberNumber, const std::string &Name) = 0;
  virtual SPIRVLine *addLine(SPIRVString *FileName, SPIRVWord Line,
      SPIRVWord Column) = 0;
  virtual const SPIRVDecorateGeneric *addDecorate(const SPIRVDecorateGeneric*) = 0;
  virtual SPIRVDecorationGroup *addDecorationGroup() = 0;
  virtual SPIRVDecorationGroup *addDecorationGroup(SPIRVDecorationGroup *Group) = 0;
  virtual SPIRVGroupDecorate *addGroupDecorate(SPIRVDecorationGroup *Group,
      const std::vector<SPIRVEntry *> &Targets) = 0;
  virtual SPIRVGroupMemberDecorate *addGroupMemberDecorate(
      SPIRVDecorationGroup *Group, const std::vector<SPIRVEntry *> &Targets) = 0;
  virtual SPIRVGroupDecorateGeneric *addGroupDecorateGeneric(
      SPIRVGroupDecorateGeneric *GDec) = 0;
  virtual void addEntryPoint(SPIRVExecutionModelKind, SPIRVId) = 0;
  virtual SPIRVForward *addForward(SPIRVType *Ty) = 0;
  virtual SPIRVForward *addForward(SPIRVId, SPIRVType *Ty) = 0;
  virtual SPIRVFunction *addFunction(SPIRVFunction *) = 0;
  virtual SPIRVFunction *addFunction(SPIRVTypeFunction *,
      SPIRVId Id = SPIRVID_INVALID) = 0;
  virtual SPIRVEntry *replaceForward(SPIRVForward *, SPIRVEntry *) = 0;

  // Type creation functions
  virtual SPIRVTypeInt *addIntegerType(unsigned) = 0;

  // Constants creation functions
  virtual SPIRVValue *addConstant(SPIRVValue *) = 0;
  virtual SPIRVValue *addConstant(SPIRVType *, uint64_t) = 0;

  virtual void addCapability(SPIRVCapabilityKind) = 0;
  
  virtual SPIRVExtInst* getCompilationUnit() = 0;
  virtual std::vector<SPIRVExtInst*> getGlobalVars() = 0;

  // I/O functions
  friend std::istream & operator>>(std::istream &I, SPIRVModule& M);
};

class SPIRVDbgInfo {
public:
  SPIRVDbgInfo(SPIRVModule *TM);
  std::string getEntryPointFileStr(SPIRVExecutionModelKind, unsigned);
  std::string getFunctionFileStr(SPIRVFunction *);
  unsigned getFunctionLineNo(SPIRVFunction *);
private:
  std::unordered_map<SPIRVFunction *, SPIRVLine *> FuncMap;
  const std::string ModuleFileStr;
  SPIRVModule *M;
};

}



#endif /* SPIRVMODULE_HPP_ */
