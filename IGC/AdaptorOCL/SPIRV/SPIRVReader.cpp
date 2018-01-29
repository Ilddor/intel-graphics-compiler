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
//===- SPIRVReader.cpp - Converts SPIR-V to LLVM -----------------*- C++ -*-===//
//
//                     The LLVM/SPIR-V Translator
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
/// This file implements conversion of SPIR-V binary to LLVM IR.
///
//===----------------------------------------------------------------------===//

#include "libSPIRV/SPIRVFunction.h"
#include "libSPIRV/SPIRVInstruction.h"
#include "SPIRVInternal.h"
#include "common/MDFrameWork.h"
#include "../../AdaptorCommon/TypesLegalizationPass.hpp"
#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/IntrinsicInst.h>
#include "libSPIRV/SPIRVDebugInfoExt.h"
#include <llvm/Support/Dwarf.h>
#include "common/LLVMWarningsPop.hpp"

#include <iostream>
#include <fstream>

using namespace llvm;


namespace spv{
// Prefix for placeholder global variable name.
const char* kPlaceholderPrefix = "placeholder.";

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
// Save the translated LLVM before validation for debugging purpose.
static bool DbgSaveTmpLLVM = true;
static const char *DbgTmpLLVMFileName = "_tmp_llvmspirv_module.ll";
#endif

static bool
isOpenCLKernel(SPIRVFunction *BF) {
   return BF->getModule()->isEntryPoint(ExecutionModelKernel, BF->getId());
}

static void
dumpLLVM(Module *M, const std::string &FName) {
  std::error_code EC;
  raw_fd_ostream FS(FName, EC, sys::fs::F_None);
  if (!FS.has_error()) {
    FS << *M;
  }
  FS.close();  
}

static MDNode*
getMDNodeStringIntVec(LLVMContext *Context, llvm::StringRef Str,
    const std::vector<SPIRVWord>& IntVals) {
  std::vector<Metadata*> ValueVec;
  ValueVec.push_back(MDString::get(*Context, Str));
  for (auto &I:IntVals)
    ValueVec.push_back(ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*Context), I)));
  return MDNode::get(*Context, ValueVec);
}

static MDNode*
getMDTwoInt(LLVMContext *Context, unsigned Int1, unsigned Int2) {
  std::vector<Metadata*> ValueVec;
  ValueVec.push_back(ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*Context), Int1)));
  ValueVec.push_back(ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*Context), Int2)));
  return MDNode::get(*Context, ValueVec);
}

static MDNode*
getMDString(LLVMContext *Context, llvm::StringRef Str) {
  std::vector<Metadata*> ValueVec;
  if (!Str.empty())
    ValueVec.push_back(MDString::get(*Context, Str));
  return MDNode::get(*Context, ValueVec);
}

static void
addOCLVersionMetadata(LLVMContext *Context, Module *M,
    llvm::StringRef MDName, unsigned Major, unsigned Minor) {
  NamedMDNode *NamedMD = M->getOrInsertNamedMetadata(MDName);
  NamedMD->addOperand(getMDTwoInt(Context, Major, Minor));
}

static void
addNamedMetadataString(LLVMContext *Context, Module *M,
    llvm::StringRef MDName, llvm::StringRef Str) {
  NamedMDNode *NamedMD = M->getOrInsertNamedMetadata(MDName);
  NamedMD->addOperand(getMDString(Context, Str));
}

static void
addOCLKernelArgumentMetadata(LLVMContext *Context,
  std::vector<llvm::Metadata*> &KernelMD, llvm::StringRef MDName,
    SPIRVFunction *BF, std::function<Metadata *(SPIRVFunctionParameter *)>Func){
  std::vector<Metadata*> ValueVec;
    ValueVec.push_back(MDString::get(*Context, MDName));
  BF->foreachArgument([&](SPIRVFunctionParameter *Arg) {
    ValueVec.push_back(Func(Arg));
  });
  KernelMD.push_back(MDNode::get(*Context, ValueVec));
}

class SPIRVToLLVM;

class SPIRVToLLVMDbgTran {
public:
  SPIRVToLLVMDbgTran(SPIRVModule *TBM, Module *TM, SPIRVToLLVM* s)
  :BM(TBM), M(TM), SpDbg(BM), Builder(*M), SPIRVTranslator(s) {
    Enable = BM->hasDebugInfo();
  }

  void addDbgInfoVersion() {
    if (!Enable)
      return;
    M->addModuleFlag(Module::Warning, "Dwarf Version",
        dwarf::DWARF_VERSION);
    M->addModuleFlag(Module::Warning, "Debug Info Version",
        DEBUG_METADATA_VERSION);
  }

  DIFile *getDIFile(const std::string &FileName){
    return getOrInsert(FileMap, FileName, [=](){
      std::string BaseName;
      std::string Path;
      splitFileName(FileName, BaseName, Path);
      if (!BaseName.empty())
        return Builder.createFile(BaseName, Path);
      else
        return Builder.createFile("", "");
    });
  }

  DIFile* getDIFile(SPIRVString* inst)
  {
      if (!inst)
          return nullptr;

      return getDIFile(inst->getStr());
  }

  DIFile* getDIFile(SPIRVExtInst* inst)
  {
      OpDebugSource src(inst);

      return getDIFile(src.getFileStr());
  }

  DICompileUnit* createCompileUnit() {
      if (!Enable || cu)
          return cu;

      OpCompilationUnit cunit(BM->getCompilationUnit());

      auto lang = cunit.getLang();
      auto file = getDIFile(BM->get<SPIRVExtInst>(cunit.getSource()));
      auto producer = "spirv";
      auto flags = "";
      auto rv = 0;

      cu = Builder.createCompileUnit(lang, file, producer, false, flags, rv);

      addDbgInfoVersion();

      return addMDNode(BM->getCompilationUnit(), cu);
  }

  DIExpression* createExpression(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DIExpression*>(inst))
          return n;

      return addMDNode(inst, Builder.createExpression());
  }

  DIType* createTypeBasic(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DIType*>(inst))
          return n;

      OpDebugTypeBasic type(inst);
      uint64_t sizeInBits = type.getSize();
      unsigned int encoding;

      auto enc = type.getEncoding();
      auto& name = type.getName()->getStr();

      switch (enc)
      {
      case OpDebugTypeBasic::Encodings::enc_boolean:
          encoding = dwarf::DW_ATE_boolean;
          break;
      case OpDebugTypeBasic::Encodings::enc_address:
          encoding = dwarf::DW_ATE_address;
          break;
      case OpDebugTypeBasic::Encodings::enc_float:
          encoding = dwarf::DW_ATE_float;
          break;
      case OpDebugTypeBasic::Encodings::enc_signed:
          encoding = dwarf::DW_ATE_signed;
          break;
      case OpDebugTypeBasic::Encodings::enc_unsigned:
          encoding = dwarf::DW_ATE_unsigned;
          break;
      case OpDebugTypeBasic::Encodings::enc_signedchar:
          encoding = dwarf::DW_ATE_signed_char;
          break;
      case OpDebugTypeBasic::enc_unsignedchar:
          encoding = dwarf::DW_ATE_unsigned_char;
          break;
      default:
          return addMDNode(inst, Builder.createUnspecifiedType(name));
      }

      return addMDNode(inst, Builder.createBasicType(name, sizeInBits, encoding));
  }

  DIType* createPtrType(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DIType*>(inst))
          return n;

      OpDebugPtrType ptrType(inst);

      auto pointeeType = createType(BM->get<SPIRVExtInst>(ptrType.getBaseType()));

      return addMDNode(inst, Builder.createPointerType(pointeeType, M->getDataLayout().getPointerSizeInBits()));
  }

  DIType* createTypeQualifier(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DIType*>(inst))
          return n;

      OpDebugTypeQualifier qualType(inst);

      auto baseType = createType(BM->get<SPIRVExtInst>(qualType.getBaseType()));
      auto qual = qualType.getQualifier();

      unsigned int qualifier = 0;
      if (qual == OpDebugTypeQualifier::TypeQualifier::qual_const)
          qualifier = dwarf::DW_TAG_const_type;
      else if (qual == OpDebugTypeQualifier::TypeQualifier::qual_restrict)
          qualifier = dwarf::DW_TAG_restrict_type;
      else if (qual == OpDebugTypeQualifier::TypeQualifier::qual_volatile)
          qualifier = dwarf::DW_TAG_volatile_type;

      return addMDNode(inst, Builder.createQualifiedType(qualifier, baseType));
  }

  DIType* createTypeArray(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DIType*>(inst))
          return n;

      OpDebugTypeArray arrayType(inst);

      auto size = BM->get<SPIRVConstant>(arrayType.getComponentCount())->getZExtIntValue();
      auto baseType = createType(BM->get<SPIRVExtInst>(arrayType.getBaseType()));

      auto sr = Builder.getOrCreateSubrange(0, size-1);
      auto mds = llvm::makeArrayRef(llvm::cast<llvm::Metadata>(sr));
      llvm::DINodeArray subscripts = llvm::MDTuple::get(M->getContext(), mds);

      return addMDNode(inst, Builder.createArrayType(size * baseType->getSizeInBits(), 0, baseType, subscripts));
  }

  DIType* createTypeVector(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DIType*>(inst))
          return n;

      OpDebugTypeVector vectorType(inst);

      auto size = vectorType.getNumComponents();
      auto type = createType(BM->get<SPIRVExtInst>(vectorType.getBaseType()));

      auto sr = Builder.getOrCreateSubrange(0, size - 1);
      auto mds = llvm::makeArrayRef(llvm::cast<llvm::Metadata>(sr));
      llvm::DINodeArray subscripts = llvm::MDTuple::get(M->getContext(), mds);

      return addMDNode(inst, Builder.createVectorType(size, 0, type, subscripts));
  }

  DIType* createTypeDef(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DIType*>(inst))
          return n;

      OpDebugTypeDef typeDef(inst);

      auto type = createType(BM->get<SPIRVExtInst>(typeDef.getBaseType()));
      auto& name = typeDef.getName()->getStr();
      auto file = getDIFile(BM->get<SPIRVExtInst>(typeDef.getSource()));
      auto line = typeDef.getLine();
      auto scopeContext = createScope(BM->get<SPIRVExtInst>(typeDef.getParent()));

      return addMDNode(inst, Builder.createTypedef(type, name, file, line, scopeContext));
  }

  DIType* createTypeEnum(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DIType*>(inst))
          return n;

      OpDebugTypeEnum typeEnum(inst);
      auto scope = createScope(BM->get<SPIRVExtInst>(typeEnum.getParent()));
      auto& name = typeEnum.getName()->getStr();
      auto file = getDIFile(BM->get<SPIRVExtInst>(typeEnum.getSource()));
      auto line = typeEnum.getLine();
      auto size = typeEnum.getSize();
      auto type = createType(BM->get<SPIRVExtInst>(typeEnum.getType()));
      
      SmallVector<Metadata*,6> elements;

      for (unsigned int i = 0; i != typeEnum.getNumItems(); i++)
      {
          auto item = typeEnum.getItem(i);
          auto enumerator = Builder.createEnumerator(item.first->getStr(), item.second);
          elements.push_back(enumerator);
      }

      auto nodeArray = Builder.getOrCreateArray(llvm::makeArrayRef(elements));

      return addMDNode(inst, Builder.createEnumerationType(scope, name, file, line, size, 0, nodeArray, type));
  }

  DIType* createMember(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DIType*>(inst))
          return n;

      OpDebugTypeMember typeMember(inst);

      // scope is not clear from SPIRV spec
      auto scope = getCompileUnit();
      auto& name = typeMember.getName()->getStr();
      auto file = getDIFile(BM->get<SPIRVExtInst>(typeMember.getSource()));
      auto line = typeMember.getLine();
      auto size = typeMember.getSize();
      auto offset = typeMember.getOffset();
      auto flagRaw = typeMember.getFlags();
      auto type = createType(BM->get<SPIRVExtInst>(typeMember.getType()));

      return addMDNode(inst, Builder.createMemberType(scope, name, file, line, size, 0, offset, (llvm::DINode::DIFlags) flagRaw, type));
  }

  DIType* createCompositeType(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DIType*>(inst))
          return n;

      OpDebugTypeComposite compositeType(inst);

      auto tag = compositeType.getTag();
      auto& name = compositeType.getName()->getStr();
      auto file = getDIFile(BM->get<SPIRVExtInst>(compositeType.getSource()));
      auto line = compositeType.getLine();
      auto size = compositeType.getSize();
      auto flagRaw = compositeType.getFlags();
      auto scope = createScope(BM->get<SPIRVExtInst>(compositeType.getParent()));

#if 0
      // SPIRV spec has single parent field whereas LLVM DIBuilder API requires Scope as well as DerivedFrom.
      // What is expected behavior?

      // parent may be OpDebugCompilationUnit/OpDebugFunction/OpDebugLexicalBlock/OpDebugTypeComposite
      DIType* from = nullptr;
      auto parentInst = BM->get<SPIRVExtInst>(parent);
      if (parentInst->getExtOp() == OCLExtOpDbgKind::CompileUnit)
          from = nullptr;
      else if (parentInst->getExtOp() == OCLExtOpDbgKind::Function)
          from = nullptr;//createFunction(parentInst);
      else if (parentInst->getExtOp() == OCLExtOpDbgKind::LexicalBlock)
          from = nullptr; //createLexicalBlock(parentInst);
      else if (parentInst->getExtOp() == OCLExtOpDbgKind::TypeComposite)
          from = createCompositeType(parentInst);
#endif

      SmallVector<Metadata*, 6> elements;
      for (unsigned int i = 0; i != compositeType.getNumItems(); i++)
      {
          auto member = static_cast<SPIRVExtInst*>(BM->getEntry(compositeType.getItem(i)));
          if (member->getExtOp() == OCLExtOpDbgKind::TypeMember)
          {
              auto md = createMember(member);
              elements.push_back(md);
          }
      }
      auto nodeArray = Builder.getOrCreateArray(llvm::makeArrayRef(elements));

      if (tag == SPIRVDebug::CompositeTypeTag::Structure)
      {
          return addMDNode(inst, Builder.createStructType(scope, name, file, line, size, 0, (llvm::DINode::DIFlags)flagRaw, nullptr, nodeArray));
      }
      else if (tag == SPIRVDebug::CompositeTypeTag::Union)
      {
          return addMDNode(inst, Builder.createUnionType(scope, name, file, line, size, 0, (llvm::DINode::DIFlags)flagRaw, nodeArray));
      }

      return nullptr;
  }

  DIType* createTypeInherit(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DIType*>(inst))
          return n;

      OpDebugTypeInheritance typeInherit(inst);

      auto type = createType(BM->get<SPIRVExtInst>(typeInherit.getChild()));
      auto base = createType(BM->get<SPIRVExtInst>(typeInherit.getParent()));
      auto offset = typeInherit.getOffset();
      auto flagRaw = typeInherit.getFlags();

      return addMDNode(inst, Builder.createInheritance(type, base, offset, (llvm::DINode::DIFlags)flagRaw));
  }

  DIType* createPtrToMember(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DIType*>(inst))
          return n;

      OpDebugPtrToMember ptrToMember(inst);

      auto pointee = createType(BM->get<SPIRVExtInst>(ptrToMember.getType()));
      auto Class = createType(BM->get<SPIRVExtInst>(ptrToMember.getParent()));
      auto size = M->getDataLayout().getPointerSizeInBits();

      return addMDNode(inst, Builder.createMemberPointerType(pointee, Class, size));
  }

  DIType* createType(SPIRVExtInst* type)
  {
      if (!type)
      {
          // return void type
          return Builder.createNullPtrType();
      }

      if (auto n = getExistingNode<DIType*>(type))
          return n;

      switch (type->getExtOp())
      {
      case OCLExtOpDbgKind::TypeBasic:
          return createTypeBasic(type);
      case OCLExtOpDbgKind::TypePtr:
          return createPtrType(type);
      case OCLExtOpDbgKind::TypeComposite:
          return createCompositeType(type);
      case OCLExtOpDbgKind::TypeQualifier:
          return createTypeQualifier(type);
      case OCLExtOpDbgKind::TypeArray:
          return createTypeArray(type);
      case OCLExtOpDbgKind::TypeVector:
          return createTypeVector(type);
      case OCLExtOpDbgKind::TypeDef:
          return createTypeDef(type);
      case OCLExtOpDbgKind::TypeEnum:
          return createTypeEnum(type);
      case OCLExtOpDbgKind::TypeInheritance:
          return createTypeInherit(type);
      case OCLExtOpDbgKind::TypePtrToMember:
          return createPtrToMember(type);
      default:
          break;
      }

      return addMDNode(type, Builder.createBasicType("int", 4, 0));
  }

  DIGlobalVariable* createGlobalVariable(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DIGlobalVariable*>(inst))
          return n;

      OpDebugGlobalVar globalVar(inst);

      auto& name = globalVar.getName()->getStr();
      auto& linkageName = globalVar.getLinkageName()->getStr();
      auto file = getDIFile(BM->get<SPIRVExtInst>(globalVar.getSource()));
      auto line = globalVar.getLine();
      auto type = createType(BM->get<SPIRVExtInst>(globalVar.getType()));

      return addMDNode(inst, Builder.createTempGlobalVariableFwdDecl(getCompileUnit(), name, linkageName, file,
          (unsigned int)line, type, true));
  }

  DISubprogram* createFunctionDecl(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DISubprogram*>(inst))
          return n;

      OpDebugFuncDecl funcDcl(inst);

      auto scope = createScope(BM->get<SPIRVExtInst>(funcDcl.getParent()));
      auto& name = funcDcl.getName()->getStr();
      auto& linkageName = funcDcl.getLinkageName()->getStr();
      auto file = getDIFile(BM->get<SPIRVExtInst>(funcDcl.getSource()));
      auto line = funcDcl.getLine();
      auto type = createSubroutineType(BM->get<SPIRVExtInst>(funcDcl.getType()));

      return addMDNode(inst, Builder.createTempFunctionFwdDecl(scope, name, linkageName, file, (unsigned int)line, type, true, true, (unsigned int)line));
  }

  DISubroutineType* createSubroutineType(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DISubroutineType*>(inst))
          return n;

      OpDebugSubroutineType spType(inst);
      std::vector<Metadata*> Args;
      auto returnType = BM->getEntry(spType.getReturnType());
      if(returnType->getOpCode() == Op::OpTypeVoid)
          Args.push_back(nullptr);
      else
        Args.push_back(createType(BM->get<SPIRVExtInst>(spType.getReturnType())));

      for (unsigned int i = 0; i != spType.getNumParms(); i++)
      {
          auto parmType = spType.getParmType(i);
          Args.push_back(createType(static_cast<SPIRVExtInst*>(BM->getValue(parmType))));
      }

      return addMDNode(inst, Builder.createSubroutineType(Builder.getOrCreateTypeArray(Args)));
  }

  DISubprogram* createFunction(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DISubprogram*>(inst))
          return n;

      OpDebugSubprogram sp(inst);

      auto scope = createScope(BM->get<SPIRVExtInst>(sp.getParent()));
      auto& name = sp.getName()->getStr();
      auto& linkageName = sp.getLinkage()->getStr();
      auto file = getDIFile(BM->get<SPIRVExtInst>(sp.getSource()));
      auto spType = createSubroutineType(BM->get<SPIRVExtInst>(sp.getType()));
      auto flags = (DINode::DIFlags)(sp.getFlags());

      return addMDNode(inst, Builder.createFunction(scope, name, linkageName, file, sp.getLine(), spType, false, true, sp.getScopeLine(), flags));
  }

  DILexicalBlock* createLexicalBlock(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DILexicalBlock*>(inst))
          return n;

      OpDebugLexicalBlock lb(inst);

      auto scope = createScope(BM->get<SPIRVExtInst>(lb.getParent()));
      auto file = getDIFile(BM->get<SPIRVExtInst>(lb.getSource()));

      return addMDNode(inst, Builder.createLexicalBlock(scope, file, lb.getLine(), lb.getColumn()));
  }

  DILexicalBlockFile* createLexicalBlockDiscriminator(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DILexicalBlockFile*>(inst))
          return n;

      OpDebugLexicalBlkDiscriminator lbDisc(inst);

      auto scope = createScope(BM->get<SPIRVExtInst>(lbDisc.getParent()));
      auto file = getDIFile(BM->get<SPIRVExtInst>(lbDisc.getSource()));
      auto disc = lbDisc.getDiscriminator();

      return addMDNode(inst, Builder.createLexicalBlockFile(scope, file, disc));
  }

  DILocation* createInlinedAt(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DILocation*>(inst))
          return n;



      OpDebugInlinedAt inlinedAt(inst);
      DILocation* iat = nullptr;

      auto line = inlinedAt.getLine();
      auto scope = createScope(BM->get<SPIRVExtInst>(inlinedAt.getScope()));
      if(inlinedAt.inlinedAtPresent())
        iat = createInlinedAt(BM->get<SPIRVExtInst>(inlinedAt.getInlinedAt()));

      return addMDNode(inst, createLocation(line, 0, scope, iat));
  }
  
  DIScope* createScope(SPIRVExtInst* inst)
  {
      if (!inst)
          return nullptr;

      if (inst->getExtOp() == OCLExtOpDbgKind::Scope)
      {
          OpDebugScope scope(inst);
          return createScope(BM->get<SPIRVExtInst>(scope.getScope()));
      }

      if (inst->getExtOp() == OCLExtOpDbgKind::Function)
      {
          return createFunction(inst);
      }
      else if (inst->getExtOp() == OCLExtOpDbgKind::LexicalBlock)
      {
          return createLexicalBlock(inst);
      }
      else if (inst->getExtOp() == OCLExtOpDbgKind::CompileUnit)
      {
          return createCompileUnit();
      }

      return nullptr;
  }

  DILocation* getInlinedAtFromScope(SPIRVExtInst* inst)
  {
      if (inst->getExtOp() == OCLExtOpDbgKind::Scope)
      {
          OpDebugScope scope(inst);

          if (!scope.hasInlinedAt())
              return nullptr;

          return createInlinedAt(BM->get<SPIRVExtInst>(scope.getInlinedAt()));
      }

      return nullptr;
  }

  DILocalVariable* createInlinedLocalVar(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DILocalVariable*>(inst))
          return n;

      OpDebugInlinedLocalVar var(inst);

      auto origVar = createLocalVar(BM->get<SPIRVExtInst>(var.getVar()));
      //auto inlinedAt = createInlinedAt(BM->get<SPIRVExtInst>(var.getInlinedAt()));

      return addMDNode(inst, origVar);
  }

  DILocalVariable* createLocalVar(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DILocalVariable*>(inst))
          return n;

      OpDebugLocalVar var(inst);
      auto scope = createScope(BM->get<SPIRVExtInst>(var.getParent()));
      auto& name = BM->get<SPIRVString>(var.getName())->getStr();
      auto file = getDIFile(BM->get<SPIRVExtInst>(var.getSource()));
      auto type = createType(BM->get<SPIRVExtInst>(var.getType()));
      auto line = var.getLine();

      if (var.isParamVar())
      {
          return addMDNode(inst, Builder.createParameterVariable(scope, name, var.getArgNo(), file, line, type));
      }
      else
      {
          return addMDNode(inst, Builder.createAutoVariable(scope, name, file, line, type));
      }
  }

  DIGlobalVariableExpression* createGlobalVar(SPIRVExtInst* inst)
  {
      if (auto n = getExistingNode<DIGlobalVariableExpression*>(inst))
          return n;

      OpDebugGlobalVar var(inst);
      auto ctxt = createScope(BM->get<SPIRVExtInst>(var.getParent()));
      auto& name = var.getName()->getStr();
      auto& linkageName = var.getLinkageName()->getStr();
      auto file = getDIFile(BM->get<SPIRVExtInst>(var.getSource()));
      auto type = createType(BM->get<SPIRVExtInst>(var.getType()));

      return addMDNode(inst, Builder.createGlobalVariableExpression(ctxt, name, linkageName, file, var.getLine(), type, true));
  }

  DILocation* createLocation(SPIRVWord line, SPIRVWord column, DIScope* scope, DILocation* inlinedAt = nullptr)
  {
      return DILocation::get(M->getContext(), (unsigned int)line, (unsigned int)column, scope, inlinedAt);
  }

  Instruction* createDbgDeclare(SPIRVExtInst* inst, Value* localVar, BasicBlock* insertAtEnd)
  {
      // Format
      // 8  12   <id>  Result Type  Result <id>  <id> Set  28  <id> Local Variable <id> Variable  <id> Expression
      OpDebugDeclare dbgDcl(inst);
      
      auto dbgDclInst = Builder.insertDeclare(localVar, 
          createLocalVar(BM->get<SPIRVExtInst>(dbgDcl.getVar())),
          createExpression(BM->get<SPIRVExtInst>(dbgDcl.getExpression())),
          createLocation(inst->getLine()->getLine(), inst->getLine()->getColumn(), createScope(inst->getDIScope())), 
          insertAtEnd);
      return dbgDclInst;
  }

  Instruction* createDbgValue(SPIRVExtInst* inst, Value* localVar, BasicBlock* insertAtEnd)
  {
      OpDebugValue dbgValue(inst);

      auto dbgValueInst = Builder.insertDbgValueIntrinsic(localVar, 0, 
          createLocalVar(BM->get<SPIRVExtInst>(dbgValue.getVar())), 
          createExpression(BM->get<SPIRVExtInst>(dbgValue.getExpression())),
          createLocation(inst->getLine()->getLine(), inst->getLine()->getColumn(), createScope(inst->getDIScope())),
          insertAtEnd);

      return dbgValueInst;
  }

  void transGlobals()
  {
      if (!Enable)
          return;

      auto globalVars = BM->getGlobalVars();

      for (auto& gvar : globalVars)
      {
          (void)createGlobalVar(gvar);
      }
  }

  void transDbgInfo(SPIRVValue *SV, Value *V);

  void finalize() {
    if (!Enable)
      return;
    Builder.finalize();
  }

  template<typename T>
  T getExistingNode(SPIRVInstruction* inst)
  {
      auto it = MDMap.find(inst);
      if (it != MDMap.end())
          return (T)it->second;
      return nullptr;
  }

  template<typename T>
  T addMDNode(SPIRVInstruction* inst, T node)
  {
      MDMap[inst] = node;
      return node;
  }

private:
  SPIRVModule *BM;
  Module *M;
  SPIRVDbgInfo SpDbg;
  DIBuilder Builder;
  bool Enable;
  DICompileUnit* cu = nullptr;
  SPIRVToLLVM* SPIRVTranslator = nullptr;
  std::unordered_map<std::string, DIFile*> FileMap;
  std::unordered_map<Function *, DISubprogram*> FuncMap;
  std::unordered_map<SPIRVInstruction*, MDNode*> MDMap;

  DICompileUnit* getCompileUnit() { return cu; }

  void splitFileName(const std::string &FileName,
      std::string &BaseName,
      std::string &Path) {
    auto Loc = FileName.find_last_of("/\\");
    if (Loc != std::string::npos) {
      BaseName = FileName.substr(Loc + 1);
      Path = FileName.substr(0, Loc);
    } else {
      BaseName = FileName;
      Path = ".";
    }
  }
};

class SPIRVToLLVM {
public:
  SPIRVToLLVM(Module *LLVMModule, SPIRVModule *TheSPIRVModule)
    :M(LLVMModule), BM(TheSPIRVModule), DbgTran(BM, M, this){
      if (M)
          Context = &M->getContext();
      else
          Context = NULL;
  }

  Type *transType(SPIRVType *BT);
  GlobalValue::LinkageTypes transLinkageType(const SPIRVValue* V);
  /// Decode SPIR-V encoding of vector type hint execution mode.
  Type *decodeVecTypeHint(LLVMContext &C, unsigned code);
  std::string transTypeToOCLTypeName(SPIRVType *BT, bool IsSigned = true);
  std::vector<Type *> transTypeVector(const std::vector<SPIRVType *>&);
  bool translate();
  bool transAddressingModel();

  enum class BoolAction
  {
      Promote,
      Truncate,
      Noop
  };

  Value *transValue(SPIRVValue *, Function *F, BasicBlock *,
      bool CreatePlaceHolder = true, BoolAction Action = BoolAction::Promote);
  Value *transValueWithoutDecoration(SPIRVValue *, Function *F, BasicBlock *,
      bool CreatePlaceHolder);
  bool transDecoration(SPIRVValue *, Value *);
  bool transAlign(SPIRVValue *, Value *);
  Instruction *transOCLBuiltinFromExtInst(SPIRVExtInst *BC, BasicBlock *BB);
  std::vector<Value *> transValue(const std::vector<SPIRVValue *>&, Function *F,
      BasicBlock *, BoolAction Action = BoolAction::Promote);
  Function *transFunction(SPIRVFunction *F);
  bool transFPContractMetadata();
  bool transKernelMetadata();
  bool transSourceLanguage();
  bool transSourceExtension();
  bool transCompilerOption();
  void addNamedBarrierArray();
  void findNamedBarrierKernel(Function* F, llvm::SmallPtrSet<Function*, 4> &kernel_set);
  Type *getNamedBarrierType();
  Value *transConvertInst(SPIRVValue* BV, Function* F, BasicBlock* BB);
  Instruction *transSPIRVBuiltinFromInst(SPIRVInstruction *BI, BasicBlock *BB);
  void transOCLVectorLoadStore(std::string& UnmangledName,
      std::vector<SPIRVWord> &BArgs);

  /// Post-process translated LLVM module for OpenCL.
  bool postProcessOCL();

  void transDebugInfo(SPIRVExtInst*, llvm::BasicBlock*);
  SPIRVToLLVMDbgTran& getDbgTran() { return DbgTran; }

  /// \brief Post-process OpenCL builtin functions returning struct type.
  ///
  /// Some OpenCL builtin functions are translated to SPIR-V instructions with
  /// struct type result, e.g. NDRange creation functions. Such functions
  /// need to be post-processed to return the struct through sret argument.
  bool postProcessOCLBuiltinReturnStruct(Function *F);

  /// \brief Post-process OpenCL builtin functions having array argument.
  ///
  /// These functions are translated to functions with array type argument
  /// first, then post-processed to have pointer arguments.
  bool postProcessOCLBuiltinWithArrayArguments(Function *F);

  typedef DenseMap<SPIRVType *, Type *> SPIRVToLLVMTypeMap;
  typedef DenseMap<SPIRVValue *, Value *> SPIRVToLLVMValueMap;
  typedef DenseMap<SPIRVFunction *, Function *> SPIRVToLLVMFunctionMap;
  typedef DenseMap<GlobalVariable *, SPIRVBuiltinVariableKind> BuiltinVarMap;

  // A SPIRV value may be translated to a load instruction of a placeholder
  // global variable. This map records load instruction of these placeholders
  // which are supposed to be replaced by the real values later.
  typedef std::map<SPIRVValue *, LoadInst*> SPIRVToLLVMPlaceholderMap;

private:
  Module *M;
  BuiltinVarMap BuiltinGVMap;
  LLVMContext *Context;
  SPIRVModule *BM;
  SPIRVToLLVMTypeMap TypeMap;
  SPIRVToLLVMValueMap ValueMap;
  SPIRVToLLVMFunctionMap FuncMap;
  SPIRVToLLVMPlaceholderMap PlaceholderMap;
  SPIRVToLLVMDbgTran DbgTran;
  GlobalVariable *m_NamedBarrierVar;
  GlobalVariable *m_named_barrier_id;
  DICompileUnit* compileUnit = nullptr;

  Type *mapType(SPIRVType *BT, Type *T) {
    TypeMap[BT] = T;
    return T;
  }

  // If a value is mapped twice, the existing mapped value is a placeholder,
  // which must be a load instruction of a global variable whose name starts
  // with kPlaceholderPrefix.
  Value *mapValue(SPIRVValue *BV, Value *V) {
    auto Loc = ValueMap.find(BV);
    if (Loc != ValueMap.end()) {
      if (Loc->second == V)
        return V;
      auto LD = dyn_cast<LoadInst>(Loc->second);
      auto Placeholder = dyn_cast<GlobalVariable>(LD->getPointerOperand());
      spirv_assert (LD && Placeholder &&
          Placeholder->getName().startswith(kPlaceholderPrefix) &&
          "A value is translated twice");
      // Replaces placeholders for PHI nodes
      LD->replaceAllUsesWith(V);
      LD->dropAllReferences();
      LD->removeFromParent();
      Placeholder->dropAllReferences();
      Placeholder->removeFromParent();
    }
    ValueMap[BV] = V;
    return V;
  }

  bool isSPIRVBuiltinVariable(GlobalVariable *GV,
      SPIRVBuiltinVariableKind *Kind = nullptr) {
    auto Loc = BuiltinGVMap.find(GV);
    if (Loc == BuiltinGVMap.end())
      return false;
    if (Kind)
      *Kind = Loc->second;
    return true;
  }
  // OpenCL function always has NoUnwound attribute.
  // Change this if it is no longer true.
  bool isFuncNoUnwind() const { return true;}
  bool isSPIRVCmpInstTransToLLVMInst(SPIRVInstruction *BI) const;
  bool transOCLBuiltinsFromVariables();
  bool transOCLBuiltinFromVariable(GlobalVariable *GV,
      SPIRVBuiltinVariableKind Kind);
  MDString *transOCLKernelArgTypeName(SPIRVFunctionParameter *);

  Value *mapFunction(SPIRVFunction *BF, Function *F) {
    FuncMap[BF] = F;
    return F;
  }

  Value *getTranslatedValue(SPIRVValue *BV);
  Type *getTranslatedType(SPIRVType *BT);

  SPIRVErrorLog &getErrorLog() {
    return BM->getErrorLog();
  }

  void setCallingConv(CallInst *Call) {
    Function *F = Call->getCalledFunction();
    Call->setCallingConv(F->getCallingConv());
  }

  void setAttrByCalledFunc(CallInst *Call);
  Type *transFPType(SPIRVType* T);
  BinaryOperator *transShiftLogicalBitwiseInst(SPIRVValue* BV, BasicBlock* BB,
      Function* F);
  Instruction *transCmpInst(SPIRVValue* BV, BasicBlock* BB, Function* F);
  Instruction *transLifetimeInst(SPIRVInstTemplateBase* BV, BasicBlock* BB, Function* F);
  uint64_t calcImageType(const SPIRVValue *ImageVal);
  std::string transOCLImageTypeName(spv::SPIRVTypeImage* ST);
  std::string transOCLSampledImageTypeName(spv::SPIRVTypeSampledImage* ST);
  std::string transOCLImageTypeAccessQualifier(spv::SPIRVTypeImage* ST);
  std::string transOCLPipeTypeAccessQualifier(spv::SPIRVTypePipe* ST);

  Value *oclTransConstantSampler(spv::SPIRVConstantSampler* BCS);

  template<class Source, class Func>
  bool foreachFuncCtlMask(Source, Func);

  Value *promoteBool(Value *pVal, BasicBlock *BB);
  Value *truncBool(Value *pVal, BasicBlock *BB);
  Type  *truncBoolType(SPIRVType *SPVType, Type *LLType);
};

void SPIRVToLLVMDbgTran::transDbgInfo(SPIRVValue *SV, Value *V) {
    if (!SV)
        return;

    if (!Enable || !SV->hasLine() || !SV->getDIScope())
        return;
    if (auto I = dyn_cast<Instruction>(V)) {
        assert(SV->isInst() && "Invalid instruction");
        auto SI = static_cast<SPIRVInstruction *>(SV);
        assert(SI->getParent() &&
            SI->getParent()->getParent() &&
            "Invalid instruction");
        auto Line = SV->getLine();
        DILocation* iat = nullptr;
        DIScope* scope = nullptr;
        if (SV->getDIScope())
        {
            scope = SPIRVTranslator->getDbgTran().createScope(SV->getDIScope());
            iat = SPIRVTranslator->getDbgTran().getInlinedAtFromScope(SV->getDIScope());
        }

        SPIRVTranslator->getDbgTran().createLocation(Line->getLine(),
            Line->getColumn(), scope, iat);

        I->setDebugLoc(DebugLoc::get(Line->getLine(), Line->getColumn(),
            scope));
    }
}

Type *
SPIRVToLLVM::getTranslatedType(SPIRVType *BV){
  auto Loc = TypeMap.find(BV);
  if (Loc != TypeMap.end())
    return Loc->second;
  return nullptr;
}

Value *
SPIRVToLLVM::getTranslatedValue(SPIRVValue *BV){
  auto Loc = ValueMap.find(BV);
  if (Loc != ValueMap.end())
    return Loc->second;
  return nullptr;
}

void
SPIRVToLLVM::setAttrByCalledFunc(CallInst *Call) {
  Function *F = Call->getCalledFunction();
  if (F->isIntrinsic()) {
    return;
  }
  Call->setCallingConv(F->getCallingConv());
  Call->setAttributes(F->getAttributes());
}

SPIRAddressSpace 
getOCLOpaqueTypeAddrSpace(Op OpCode)
{
    switch (OpCode)
    {
    case OpTypePipe:
        // these types are handled in special way at SPIRVToLLVM::transType
        return SPIRAS_Global;
    default:
        //OpTypeQueue:
        //OpTypeEvent:
        //OpTypeDeviceEvent:
        //OpTypeReserveId
        return SPIRAS_Private;
    }
}

bool
SPIRVToLLVM::transOCLBuiltinsFromVariables(){
  std::vector<GlobalVariable *> WorkList;
  for (auto I = M->global_begin(), E = M->global_end(); I != E; ++I) {
    SPIRVBuiltinVariableKind Kind = BuiltInCount;
    if (!isSPIRVBuiltinVariable(&(*I), &Kind))
      continue;
    if (!transOCLBuiltinFromVariable(&(*I), Kind))
      return false;
    WorkList.push_back(&(*I));
  }
  for (auto &I:WorkList) {
    I->eraseFromParent();
  }
  return true;
}

// For integer types shorter than 32 bit, unsigned/signedness can be inferred
// from zext/sext attribute.
MDString *
SPIRVToLLVM::transOCLKernelArgTypeName(SPIRVFunctionParameter *Arg) {
  auto Ty = Arg->isByVal() ? Arg->getType()->getPointerElementType() :
    Arg->getType();
  return MDString::get(*Context, transTypeToOCLTypeName(Ty, !Arg->isZext()));
}

// Variable like GlobalInvocationId[x] -> get_global_id(x).
// Variable like WorkDim -> get_work_dim().
bool
SPIRVToLLVM::transOCLBuiltinFromVariable(GlobalVariable *GV,
    SPIRVBuiltinVariableKind Kind)
{
  std::string FuncName;
  if (!SPIRSPIRVBuiltinVariableMap::find(Kind, &FuncName))
      return false;

  decorateSPIRVBuiltin(FuncName);
  Function *Func = M->getFunction(FuncName);
  if (!Func)
  {
      Type *ReturnTy = GV->getType()->getPointerElementType();
      FunctionType *FT = FunctionType::get(ReturnTy, false);
      Func = Function::Create(FT, GlobalValue::ExternalLinkage, FuncName, M);
      Func->setCallingConv(CallingConv::SPIR_FUNC);
      Func->addFnAttr(Attribute::NoUnwind);
      Func->addFnAttr(Attribute::ReadNone);
  }

  SmallVector<LoadInst*, 4> Deletes;
  SmallVector<LoadInst*, 4> Users;
  for (auto UI : GV->users())
  {
      spirv_assert(isa<LoadInst>(UI) && "Unsupported use");
      auto *LD = cast<LoadInst>(UI);
      Users.push_back(LD);
      Deletes.push_back(LD);
  }
  for (auto &I : Users)
  {
      auto Call = CallInst::Create(Func, "", I);
      Call->takeName(I);
      setAttrByCalledFunc(Call);
      I->replaceAllUsesWith(Call);
  }
  for (auto &I : Deletes)
  {
      assert(I->use_empty());
      if (I->use_empty())
          I->eraseFromParent();
  }

  return true;
}

Type *
SPIRVToLLVM::transFPType(SPIRVType* T) {
  switch(T->getFloatBitWidth()) {
  case 16: return Type::getHalfTy(*Context);
  case 32: return Type::getFloatTy(*Context);
  case 64: return Type::getDoubleTy(*Context);
  default:
    llvm_unreachable("Invalid type");
    return nullptr;
  }
}

std::string
SPIRVToLLVM::transOCLImageTypeName(spv::SPIRVTypeImage* ST) {
  return std::string(kSPR2TypeName::OCLPrefix)
       + rmap<std::string>(ST->getDescriptor())
       + kSPR2TypeName::Delimiter
       + rmap<std::string>(ST->getAccessQualifier());
}

std::string
SPIRVToLLVM::transOCLSampledImageTypeName(spv::SPIRVTypeSampledImage* ST) {
   return std::string(kLLVMName::builtinPrefix) + kSPIRVTypeName::SampledImage;
}

GlobalValue::LinkageTypes
SPIRVToLLVM::transLinkageType(const SPIRVValue* V) {
  if (V->getLinkageType() == LinkageTypeInternal) {
    return GlobalValue::InternalLinkage;
  }
  else if (V->getLinkageType() == LinkageTypeImport) {
    // Function declaration
    if (V->getOpCode() == OpFunction) {
      if (static_cast<const SPIRVFunction*>(V)->getNumBasicBlock() == 0)
        return GlobalValue::ExternalLinkage;
    }
    // Variable declaration
    if (V->getOpCode() == OpVariable) {
      if (static_cast<const SPIRVVariable*>(V)->getInitializer() == 0)
        return GlobalValue::ExternalLinkage;
    }
    // Definition
    return GlobalValue::AvailableExternallyLinkage;
  }
  else {// LinkageTypeExport
    if (V->getOpCode() == OpVariable) {
      if (static_cast<const SPIRVVariable*>(V)->getInitializer() == 0 )
        // Tentative definition
        return GlobalValue::CommonLinkage;
    }
    return GlobalValue::LinkOnceODRLinkage;
  }
}

Type *
SPIRVToLLVM::transType(SPIRVType *T) {
  auto Loc = TypeMap.find(T);
  if (Loc != TypeMap.end())
    return Loc->second;

  T->validate();
  switch(T->getOpCode()) {
  case OpTypeVoid:
    return mapType(T, Type::getVoidTy(*Context));
  case OpTypeBool:
    return mapType(T, Type::getInt8Ty(*Context));
  case OpTypeInt:
    return mapType(T, Type::getIntNTy(*Context, T->getIntegerBitWidth()));
  case OpTypeFloat:
    return mapType(T, transFPType(T));
  case OpTypeArray:
    return mapType(T, ArrayType::get(transType(T->getArrayElementType()),
        T->getArrayLength()));
  case OpTypePointer:
    return mapType(T, PointerType::get(transType(T->getPointerElementType()),
        SPIRSPIRVAddrSpaceMap::rmap(T->getPointerStorageClass())));
  case OpTypeVector:
    return mapType(T, VectorType::get(transType(T->getVectorComponentType()),
        T->getVectorComponentCount()));
  case OpTypeOpaque:
    return mapType(T, StructType::create(*Context, T->getName()));
  case OpTypeFunction: {
    auto FT = static_cast<SPIRVTypeFunction *>(T);
    auto RT = transType(FT->getReturnType());
    std::vector<Type *> PT;
    for (size_t I = 0, E = FT->getNumParameters(); I != E; ++I)
      PT.push_back(transType(FT->getParameterType(I)));
    return mapType(T, FunctionType::get(RT, PT, false));
    }
  case OpTypeImage: {
   return mapType(T, getOrCreateOpaquePtrType(M,
          transOCLImageTypeName(static_cast<SPIRVTypeImage *>(T))));
  }
  case OpTypeSampler:
     //ulong __builtin_spirv_OpTypeSampler
     return mapType(T, Type::getInt64Ty(*Context));
  case OpTypeSampledImage: {
     //ulong3 __builtin_spirv_OpSampledImage
     return mapType(T, VectorType::get(Type::getInt64Ty(*Context), 3));
  }
  case OpTypeStruct: {
    auto ST = static_cast<SPIRVTypeStruct *>(T);
    auto *pStructTy = StructType::create(*Context, ST->getName());
    mapType(ST, pStructTy);
    SmallVector<Type *, 4> MT;
    for (size_t I = 0, E = ST->getMemberCount(); I != E; ++I)
      MT.push_back(transType(ST->getMemberType(I)));

    pStructTy->setBody(MT, ST->isPacked());
    return pStructTy;
    } 
  case OpTypePipeStorage:
  {
      return mapType(T, Type::getInt8PtrTy(*Context, SPIRAS_Global));
  }
  case OpTypeNamedBarrier: 
  {
    return mapType(T, getNamedBarrierType());
  }
  default: {
    auto OC = T->getOpCode();
    if (isOpaqueGenericTypeOpCode(OC))
    {
        auto name = BuiltinOpaqueGenericTypeOpCodeMap::rmap(OC);
        auto *pST = M->getTypeByName(name);
        pST = pST ? pST : StructType::create(*Context, name);

        return mapType(T, PointerType::get(pST, getOCLOpaqueTypeAddrSpace(OC)));
    }
    llvm_unreachable("Not implemented");
    }
  }
  return 0;
}

std::string
SPIRVToLLVM::transTypeToOCLTypeName(SPIRVType *T, bool IsSigned) {
  switch(T->getOpCode()) {
  case OpTypeVoid:
    return "void";
  case OpTypeBool:
    return "bool";
  case OpTypeInt: {
    std::string Prefix = IsSigned ? "" : "u";
    switch(T->getIntegerBitWidth()) {
    case 8:
      return Prefix + "char";
    case 16:
      return Prefix + "short";
    case 32:
      return Prefix + "int";
    case 64:
      return Prefix + "long";
    default:
      llvm_unreachable("invalid integer size");
      return Prefix + std::string("int") + T->getIntegerBitWidth() + "_t";
    }
  }
  break;
  case OpTypeFloat:
    switch(T->getFloatBitWidth()){
    case 16:
      return "half";
    case 32:
      return "float";
    case 64:
      return "double";
    default:
      llvm_unreachable("invalid floating pointer bitwidth");
      return std::string("float") + T->getFloatBitWidth() + "_t";
    }
    break;
  case OpTypeArray:
    return "array";
  case OpTypePointer:
    return transTypeToOCLTypeName(T->getPointerElementType()) + "*";
  case OpTypeVector:
    return transTypeToOCLTypeName(T->getVectorComponentType()) +
        T->getVectorComponentCount();
  case OpTypeOpaque:
      return T->getName();
  case OpTypeFunction:
    llvm_unreachable("Unsupported");
    return "function";
  case OpTypeStruct: {
    auto Name = T->getName();
    if (Name.find("struct.") == 0)
      Name[6] = ' ';
    else if (Name.find("union.") == 0)
      Name[5] = ' ';
    return Name;
  }
  case OpTypePipe:
    return "pipe";
  case OpTypeSampler:
    return "sampler_t";
  case OpTypeImage:
    return rmap<std::string>(static_cast<SPIRVTypeImage *>(T)->getDescriptor());
  default:
      if (isOpaqueGenericTypeOpCode(T->getOpCode())) {
        auto Name = BuiltinOpaqueGenericTypeOpCodeMap::rmap(T->getOpCode());
        if (Name.find("opencl.") == 0) {
            return Name.substr(7);
        } else {
            return Name;
        }
      }
      llvm_unreachable("Not implemented");
      return "unknown";
  }
}

std::vector<Type *>
SPIRVToLLVM::transTypeVector(const std::vector<SPIRVType *> &BT) {
  std::vector<Type *> T;
  for (auto I: BT)
    T.push_back(transType(I));
  return T;
}

std::vector<Value *>
SPIRVToLLVM::transValue(const std::vector<SPIRVValue *> &BV, Function *F,
    BasicBlock *BB, BoolAction Action) {
  std::vector<Value *> V;
  for (auto I: BV)
    V.push_back(transValue(I, F, BB, true, Action));
  return V;
}

bool
SPIRVToLLVM::isSPIRVCmpInstTransToLLVMInst(SPIRVInstruction* BI) const {
  auto OC = BI->getOpCode();
  return isCmpOpCode(OC) &&
      !(OC >= OpLessOrGreater && OC <= OpUnordered);
}

Value *
SPIRVToLLVM::transValue(SPIRVValue *BV, Function *F, BasicBlock *BB,
    bool CreatePlaceHolder, BoolAction Action)
{
  auto procBool = [&](Value *v)
  {
      if (Action == BoolAction::Noop)
          return v;

      if (!BV->hasType())
          return v;

      if (!BV->getType()->isTypeVectorOrScalarBool())
          return v;

      return Action == BoolAction::Promote ?
          promoteBool(v, BB) :
          truncBool(v, BB);
  };

  SPIRVToLLVMValueMap::iterator Loc = ValueMap.find(BV);
  if (Loc != ValueMap.end() && (!PlaceholderMap.count(BV) || CreatePlaceHolder))
  {
      return procBool(Loc->second);
  }

  BV->validate();

  auto V = transValueWithoutDecoration(BV, F, BB, CreatePlaceHolder);
  if (!V) {
    return nullptr;
  }
  V->setName(BV->getName());
  if (!transDecoration(BV, V)) {
    spirv_assert (0 && "trans decoration fail");
    return nullptr;
  }

  return procBool(V);
}

Value *
SPIRVToLLVM::transConvertInst(SPIRVValue* BV, Function* F, BasicBlock* BB) {
  SPIRVUnary* BC = static_cast<SPIRVUnary*>(BV);
  auto Src = transValue(BC->getOperand(0), F, BB, BB ? true : false);
  auto Dst = transType(BC->getType());
  CastInst::CastOps CO = Instruction::BitCast;
  bool IsExt = Dst->getScalarSizeInBits()
      > Src->getType()->getScalarSizeInBits();
  switch (BC->getOpCode()) {
  case OpPtrCastToGeneric:
  case OpGenericCastToPtr:
    CO = Instruction::AddrSpaceCast;
    break;
  case OpSConvert:
    CO = IsExt ? Instruction::SExt : Instruction::Trunc;
    break;
  case OpUConvert:
    CO = IsExt ? Instruction::ZExt : Instruction::Trunc;
    break;
  case OpFConvert:
    CO = IsExt ? Instruction::FPExt : Instruction::FPTrunc;
    break;
  default:
    CO = static_cast<CastInst::CastOps>(OpCodeMap::rmap(BC->getOpCode()));
  }
  assert(CastInst::isCast(CO) && "Invalid cast op code");
  if (BB)
    return CastInst::Create(CO, Src, Dst, BV->getName(), BB);
  return ConstantExpr::getCast(CO, dyn_cast<Constant>(Src), Dst);
}

BinaryOperator *SPIRVToLLVM::transShiftLogicalBitwiseInst(SPIRVValue* BV,
    BasicBlock* BB,Function* F) {
  SPIRVBinary* BBN = static_cast<SPIRVBinary*>(BV);
  assert(BB && "Invalid BB");
  Instruction::BinaryOps BO;
  auto OP = BBN->getOpCode();
  if (isLogicalOpCode(OP))
    OP = IntBoolOpMap::rmap(OP);
  BO = static_cast<Instruction::BinaryOps>(OpCodeMap::rmap(OP));
  auto Inst = BinaryOperator::Create(BO,
      transValue(BBN->getOperand(0), F, BB),
      transValue(BBN->getOperand(1), F, BB), BV->getName(), BB);
  return Inst;
}

Instruction *
SPIRVToLLVM::transLifetimeInst(SPIRVInstTemplateBase* BI, BasicBlock* BB, Function* F)
{
    auto ID = (BI->getOpCode() == OpLifetimeStart) ?
        Intrinsic::lifetime_start :
        Intrinsic::lifetime_end;
    auto *pFunc = Intrinsic::getDeclaration(M, ID);
    auto *pPtr = transValue(BI->getOperand(0), F, BB);

    Value *pArgs[] =
    {
        ConstantInt::get(Type::getInt64Ty(*Context), BI->getOpWord(1)),
        CastInst::CreatePointerCast(pPtr, PointerType::getInt8PtrTy(*Context), "", BB)
    };

    auto *pCI = CallInst::Create(pFunc, pArgs, "", BB);
    return pCI;
}

Instruction *
SPIRVToLLVM::transCmpInst(SPIRVValue* BV, BasicBlock* BB, Function* F) {
  SPIRVCompare* BC = static_cast<SPIRVCompare*>(BV);
  assert(BB && "Invalid BB");
  SPIRVType* BT = BC->getOperand(0)->getType();
  Instruction* Inst = nullptr;
  if (BT->isTypeVectorOrScalarInt() 
   || BT->isTypePointer()
   || BT->isTypeBool())
    Inst = new ICmpInst(*BB, CmpMap::rmap(BC->getOpCode()),
        transValue(BC->getOperand(0), F, BB),
        transValue(BC->getOperand(1), F, BB));
  else if (BT->isTypeVectorOrScalarFloat())
    Inst = new FCmpInst(*BB, CmpMap::rmap(BC->getOpCode()),
        transValue(BC->getOperand(0), F, BB),
        transValue(BC->getOperand(1), F, BB));
  assert(Inst && "not implemented");
  return Inst;
}

void 
SPIRVToLLVM::findNamedBarrierKernel(Function* F, llvm::SmallPtrSet<Function*,4> &kernel_set)
{
    for (auto U : F->users())
    {
        if (auto CI = llvm::dyn_cast<llvm::CallInst>(U))
        {
            llvm::Function* func = CI->getParent()->getParent();
            if (func->getCallingConv() == CallingConv::SPIR_KERNEL)
            {
                kernel_set.insert(func);
            }
            else
                spirv_assert(0 && "Intialized called in User Function");
        }
    }
}


bool
SPIRVToLLVM::postProcessOCL() {
  // I think we dont need it
  std::vector <Function*> structFuncs;
  for (auto& F : M->functions())
  {
      if (isFunctionBuiltin(&F) && F.getReturnType()->isStructTy())
      {
          structFuncs.push_back(&F);
      }
      if (F.getName().startswith("__builtin_spirv_OpNamedBarrierInitialize"))
      {
          //First find entry block
          llvm::SmallPtrSet<Function*, 4> kernel_set;
          findNamedBarrierKernel(&F, kernel_set);
          for (auto element : kernel_set)
          {
              BasicBlock::iterator pInsertionPoint = element->getEntryBlock().begin();
              while (isa<AllocaInst>(pInsertionPoint)) ++pInsertionPoint;
              std::vector<Type *> ArgTys{Type::getInt32PtrTy(*Context, SPIRAS_Local)};
              auto newName = "__intel_getInitializedNamedBarrierArray";
              auto newFType = FunctionType::get(Type::getVoidTy(*Context), ArgTys, false);
              auto newF = cast<Function>(M->getOrInsertFunction(newName, newFType));
              Value* Arg_array[] = { m_named_barrier_id };
              CallInst::Create(newF, Arg_array, "", &(*pInsertionPoint));
          }
      }
  }
  for (auto structFunc : structFuncs)
    postProcessOCLBuiltinReturnStruct(structFunc);

  std::vector<Function*> arrFuncs;
  for (auto& F : M->functions())
  {
      if (isFunctionBuiltin(&F)
          && std::any_of(F.arg_begin(), F.arg_end(), [](Argument& arg) { return arg.getType()->isArrayTy(); }) )
          {
              arrFuncs.push_back(&F);
          }
  }
  for (auto arrFunc : arrFuncs)
      postProcessOCLBuiltinWithArrayArguments(arrFunc);

  //Adjust ndrange_t type
  auto ndrangeTy = M->getTypeByName("struct.ndrange_t");
  if (ndrangeTy != nullptr)
  {
      ndrangeTy->setName("struct.Ndrange_t");
  }

  return true;
}

static Value* StripAddrspaceCast(Value *pVal)
{
    while (Operator::getOpcode(pVal) == Instruction::AddrSpaceCast)
        pVal = cast<Operator>(pVal)->getOperand(0);

    return pVal;
}

bool
SPIRVToLLVM::postProcessOCLBuiltinReturnStruct(Function *F) {
  
  if (!isFunctionBuiltin(F) || !F->getReturnType()->isStructTy())
    return false;

  std::string Name = F->getName();
  F->setName(Name + ".old");

  std::vector<Type *> ArgTys;
  getFunctionTypeParameterTypes(F->getFunctionType(), ArgTys);
  ArgTys.insert(ArgTys.begin(), PointerType::get(F->getReturnType(),
      SPIRAS_Private));
  auto newFType = FunctionType::get(Type::getVoidTy(*Context), ArgTys, false);
  auto newF = cast<Function>(M->getOrInsertFunction(Name,  newFType));
  newF->setCallingConv(F->getCallingConv());

  for (auto I = F->user_begin(), E = F->user_end(); I != E;) {
    if (auto CI = dyn_cast<CallInst>(*I++)) {
      assert(CI->hasOneUse());
      auto ST = dyn_cast<StoreInst>(*(CI->user_begin()));
      auto Args = getArguments(CI);
      Args.insert(Args.begin(), StripAddrspaceCast(ST->getPointerOperand()));
      auto NewCI = CallInst::Create(newF, Args, CI->getName(), CI);
      NewCI->setCallingConv(CI->getCallingConv());
      ST->eraseFromParent();
      CI->eraseFromParent();
    }
  }
  F->dropAllReferences();
  F->removeFromParent();
  return true;
}

bool
SPIRVToLLVM::postProcessOCLBuiltinWithArrayArguments(Function* F) {
  auto Name = F->getName();
  auto Attrs = F->getAttributes();
  auto DL = M->getDataLayout();
  auto ptrSize = DL.getPointerSize();

  mutateFunction (F, [=](CallInst *CI, std::vector<Value *> &Args) {
    auto FBegin = CI->getParent()->getParent()->begin()->getFirstInsertionPt();
    for (auto &I:Args) {
      auto T = I->getType();
      if (!T->isArrayTy())
        continue;
      
      if (auto constVal = dyn_cast<Constant>(I)) {
          I = new GlobalVariable(*M, T, true, GlobalValue::InternalLinkage, constVal);
      } else if (auto loadInst = dyn_cast<LoadInst>(I)) {
          I = loadInst->getPointerOperand();
      }

      auto Alloca = new AllocaInst(T, "", &(*FBegin));
      Alloca->setAlignment(ptrSize);
      IRBuilder<> builder(CI);
      auto size = DL.getTypeAllocSize(T);
      builder.CreateMemCpy(Alloca, I, size, ptrSize);
      I = ptrSize > 4
          ? builder.CreateConstInBoundsGEP2_64(Alloca, 0, 0)
          : builder.CreateConstInBoundsGEP2_32(nullptr, Alloca, 0, 0);
    }
    return Name;
  }, false, &Attrs);
  return true;
}

std::string
SPIRVToLLVM::transOCLPipeTypeAccessQualifier(spv::SPIRVTypePipe* ST) {
  return SPIRSPIRVAccessQualifierMap::rmap(ST->getAccessQualifier());
}

Value *
SPIRVToLLVM::oclTransConstantSampler(spv::SPIRVConstantSampler* BCS) {
  auto Lit = (BCS->getAddrMode() << 1) |
      BCS->getNormalized() |
      ((BCS->getFilterMode() + 1) << 4);
  auto Ty = IntegerType::getInt64Ty(*Context);
  return ConstantInt::get(Ty, Lit);
}

Value *SPIRVToLLVM::promoteBool(Value *pVal, BasicBlock *BB)
{
    if (!pVal->getType()->getScalarType()->isIntegerTy(1))
        return pVal;

    auto *PromoType = isa<VectorType>(pVal->getType()) ?
        cast<Type>(VectorType::get(Type::getInt8Ty(pVal->getContext()),
            pVal->getType()->getVectorNumElements())) :
        Type::getInt8Ty(pVal->getContext());

    if (auto *C = dyn_cast<Constant>(pVal))
        return ConstantExpr::getZExtOrBitCast(C, PromoType);
    
    if (BB == nullptr)
        return pVal;

    if (auto *Arg = dyn_cast<Argument>(pVal))
    {
        auto &entry = BB->getParent()->getEntryBlock();
        Instruction *Cast = nullptr;
        if (entry.empty())
        {
            Cast = CastInst::CreateZExtOrBitCast(Arg, PromoType, "i1promo", BB);
        }
        else
        {
            auto IP = entry.begin();
            while (isa<AllocaInst>(IP)) ++IP;
            if (IP == BB->end())
                Cast = CastInst::CreateZExtOrBitCast(Arg, PromoType, "i1promo", BB);
            else
                Cast = CastInst::CreateZExtOrBitCast(Arg, PromoType, "i1promo", &(*IP));
        }
        return Cast;
    }

    auto *pInst = cast<Instruction>(pVal);
    auto *Cast = CastInst::CreateZExtOrBitCast(pInst, PromoType, "i1promo");
    Cast->insertAfter(pInst);
    return Cast;
}

Value *SPIRVToLLVM::truncBool(Value *pVal, BasicBlock *BB)
{
    if (!pVal->getType()->getScalarType()->isIntegerTy(8))
        return pVal;

    auto *TruncType = isa<VectorType>(pVal->getType()) ?
        cast<Type>(VectorType::get(Type::getInt1Ty(pVal->getContext()),
            pVal->getType()->getVectorNumElements())) :
        Type::getInt1Ty(pVal->getContext());

    if (auto *C = dyn_cast<Constant>(pVal))
        return ConstantExpr::getTruncOrBitCast(C, TruncType);
    
    if (BB == nullptr)
        return pVal;

    if (auto *Arg = dyn_cast<Argument>(pVal))
    {
        auto &entry = BB->getParent()->getEntryBlock();
        Instruction *Cast = nullptr;
        if (entry.empty())
        {
            Cast = CastInst::CreateTruncOrBitCast(Arg, TruncType, "i1trunc", BB);
        }
        else
        {
            auto IP = entry.begin();
            while (isa<AllocaInst>(IP)) ++IP;
            if (IP == BB->end())
                Cast = CastInst::CreateTruncOrBitCast(Arg, TruncType, "i1trunc", BB);
            else
                Cast = CastInst::CreateTruncOrBitCast(Arg, TruncType, "i1trunc", &(*IP));
        }
        return Cast;
    }

    auto *pInst = cast<Instruction>(pVal);
    auto *Cast = CastInst::CreateTruncOrBitCast(pInst, TruncType, "i1trunc");
    Cast->insertAfter(pInst);
    return Cast;
}

Type *SPIRVToLLVM::truncBoolType(SPIRVType *SPVType, Type *LLType)
{
    if (!SPVType->isTypeVectorOrScalarBool())
        return LLType;

    return isa<VectorType>(LLType) ?
        cast<Type>(VectorType::get(Type::getInt1Ty(LLType->getContext()),
                                   LLType->getVectorNumElements())) :
        Type::getInt1Ty(LLType->getContext());
}

/// For instructions, this function assumes they are created in order
/// and appended to the given basic block. An instruction may use a
/// instruction from another BB which has not been translated. Such
/// instructions should be translated to place holders at the point
/// of first use, then replaced by real instructions when they are
/// created.
///
/// When CreatePlaceHolder is true, create a load instruction of a
/// global variable as placeholder for SPIRV instruction. Otherwise,
/// create instruction and replace placeholder if there is one.
Value *
SPIRVToLLVM::transValueWithoutDecoration(SPIRVValue *BV, Function *F,
    BasicBlock *BB, bool CreatePlaceHolder){

  auto OC = BV->getOpCode();
  IntBoolOpMap::rfind(OC, &OC);

  // Translation of non-instruction values
  switch(OC) {
  case OpConstant: {
    SPIRVConstant *BConst = static_cast<SPIRVConstant *>(BV);
    SPIRVType *BT = BV->getType();
    Type *LT = transType(BT);
    switch(BT->getOpCode()) {
    case OpTypeBool:
    case OpTypeInt:
      return mapValue(BV, ConstantInt::get(LT, BConst->getZExtIntValue(),
          static_cast<SPIRVTypeInt*>(BT)->isSigned()));
    case OpTypeFloat: {
      const llvm::fltSemantics *FS = nullptr;
      switch (BT->getFloatBitWidth()) {
      case 16:
        FS = &APFloat::IEEEhalf();
        break;
      case 32:
        FS = &APFloat::IEEEsingle();
        break;
      case 64:
        FS = &APFloat::IEEEdouble();
        break;
      default:
        spirv_assert (0 && "invalid float type");
      }
      return mapValue(BV, ConstantFP::get(*Context, APFloat(*FS,
          APInt(BT->getFloatBitWidth(), BConst->getZExtIntValue()))));
    }
    default:
      llvm_unreachable("Not implemented");
      return NULL;
    }
  }
  break;

  case OpConstantTrue:
    return mapValue(BV, ConstantInt::getTrue(*Context));

  case OpConstantFalse:
    return mapValue(BV, ConstantInt::getFalse(*Context));

  case OpConstantNull: {
    auto LT = transType(BV->getType());
    if (auto PT = dyn_cast<PointerType>(LT))
      return mapValue(BV, ConstantPointerNull::get(PT));
    return mapValue(BV, ConstantAggregateZero::get(LT));
  }

  case OpConstantComposite: {
    auto BCC = static_cast<SPIRVConstantComposite*>(BV);
    std::vector<Constant *> CV;
    for (auto &I:BCC->getElements())
      CV.push_back(dyn_cast<Constant>(transValue(I, F, BB)));
    switch(BV->getType()->getOpCode()) {
    case OpTypeVector:
      return mapValue(BV, ConstantVector::get(CV));
    case OpTypeArray:
      return mapValue(BV, ConstantArray::get(
          dyn_cast<ArrayType>(transType(BCC->getType())), CV));
    case OpTypeStruct:
      return mapValue(BV, ConstantStruct::get(
          dyn_cast<StructType>(transType(BCC->getType())), CV));
    default:
      llvm_unreachable("not implemented");
      return nullptr;
    }
  }
  break;

  case OpCompositeConstruct: {
    auto BCC = static_cast<SPIRVCompositeConstruct*>(BV);
    std::vector<Value *> CV;
    for(auto &I : BCC->getElements())
    {
      CV.push_back( transValue( I,F,BB ) );
    }
    switch(BV->getType()->getOpCode()) {
    case OpTypeVector:
    {
      Type  *T = transType( BCC->getType() );

      Value *undef = llvm::UndefValue::get( T );
      Value *elm1  = undef;
      uint32_t pos = 0;

      auto  CreateCompositeConstruct = [&]( Value* Vec,Value* ValueToBeInserted,uint32_t Pos )
      {
        Value *elm = InsertElementInst::Create(
          Vec,
          ValueToBeInserted,
          ConstantInt::get( *Context,APInt( 32,Pos ) ),
          BCC->getName(),BB );
        return elm;
      };

      for(unsigned i = 0; i < CV.size(); i++)
      {
        if(CV[i]->getType()->isVectorTy())
        {
          for(uint32_t j = 0; j < CV[i]->getType()->getVectorNumElements(); j++)
          {
            Value *v = ExtractElementInst::Create( CV[i],ConstantInt::get( *Context,APInt( 32,j ) ),BCC->getName(),BB );
            elm1 = CreateCompositeConstruct( elm1,v,pos++ );
          }
        }
        else
        {
          elm1 = CreateCompositeConstruct( elm1,CV[i],pos++ );
        }
      }
      return mapValue( BV,elm1 );
    }
    break;
    case OpTypeArray:
    case OpTypeStruct:
    {
      Type *T = transType( BV->getType() );
      Value *undef = llvm::UndefValue::get( T );
      Value *elm1  = undef;

      for(unsigned i = 0; i < CV.size(); i++)
      {
        elm1 = InsertValueInst::Create(
          elm1,
          CV[i],
          i,
          BCC->getName(),BB );
      }
      return mapValue( BV,elm1 );
    }
    break;
    default:
      llvm_unreachable( "not implemented" );
      return nullptr;
    }
  }
  break;
  case OpConstantSampler: {
    auto BCS = static_cast<SPIRVConstantSampler*>(BV);
    return mapValue(BV, oclTransConstantSampler(BCS));
  }

  case OpSpecConstantOp: {
    auto BI = createInstFromSpecConstantOp(
        static_cast<SPIRVSpecConstantOp*>(BV));
    return mapValue(BV, transValue(BI, nullptr, nullptr, false));
  }

  case OpConstantPipeStorage:
  {
      auto CPS = static_cast<SPIRVConstantPipeStorage*>(BV);
      const uint32_t packetSize    = CPS->GetPacketSize();
      const uint32_t packetAlign   = CPS->GetPacketAlignment();
      const uint32_t maxNumPackets = CPS->GetCapacity();
      // This value matches the definition from the runtime and that from pipe.cl.
      const uint32_t INTEL_PIPE_HEADER_RESERVED_SPACE = 128;

      const uint32_t numPacketsAlloc = maxNumPackets + 1;
      const uint32_t bufSize = packetSize * numPacketsAlloc + INTEL_PIPE_HEADER_RESERVED_SPACE;

      SmallVector<uint8_t, 256> buf(bufSize, 0);
      // Initialize the pipe_max_packets field in the control structure.
      for (unsigned i = 0; i < 4; i++)
          buf[i] = (uint8_t)((numPacketsAlloc >> (8 * i)) & 0xff);

      auto *pInit = ConstantDataArray::get(*Context, buf);

      GlobalVariable *pGV = new GlobalVariable(
          *M,
          pInit->getType(),
          false,
          GlobalVariable::InternalLinkage,
          pInit,
          Twine("pipebuf"),
          nullptr,
          GlobalVariable::ThreadLocalMode::NotThreadLocal,
          SPIRAS_Global);

      pGV->setAlignment(std::max(4U, packetAlign));

      auto *pStorageTy = transType(CPS->getType());
      return mapValue(CPS, ConstantExpr::getBitCast(pGV, pStorageTy));
  }

  case OpUndef:
    return mapValue(BV, UndefValue::get(transType(BV->getType())));

  case OpVariable: {
    auto BVar = static_cast<SPIRVVariable *>(BV);
    auto Ty = transType(BVar->getType()->getPointerElementType());
    bool IsConst = BVar->isConstant();
    llvm::GlobalValue::LinkageTypes LinkageTy = transLinkageType(BVar);
    Constant *Initializer = nullptr;
    SPIRVStorageClassKind BS = BVar->getStorageClass();
    SPIRVValue *Init = BVar->getInitializer();
    if (Init)
        Initializer = dyn_cast<Constant>(transValue(Init, F, BB, false));
    else if (LinkageTy == GlobalValue::CommonLinkage)
        // In LLVM variables with common linkage type must be initilized by 0
        Initializer = Constant::getNullValue(Ty);
    else if (BS == StorageClassWorkgroupLocal)
        Initializer = UndefValue::get(Ty);

    if (BS == StorageClassFunction && !Init) {
        assert (BB && "Invalid BB");
        return mapValue(BV, new AllocaInst(Ty, BV->getName(), BB));
    }
    auto AddrSpace = SPIRSPIRVAddrSpaceMap::rmap(BS);
    auto LVar = new GlobalVariable(*M, Ty, IsConst, LinkageTy, Initializer,
        BV->getName(), 0, GlobalVariable::NotThreadLocal, AddrSpace);
	GlobalVariable::UnnamedAddr addrType = (IsConst && Ty->isArrayTy() &&
		Ty->getArrayElementType()->isIntegerTy(8)) ? GlobalVariable::UnnamedAddr::Global : 
		GlobalVariable::UnnamedAddr::None;
    LVar->setUnnamedAddr(addrType);
    SPIRVBuiltinVariableKind BVKind = BuiltInCount;
    if (BVar->isBuiltin(&BVKind))
      BuiltinGVMap[LVar] = BVKind;
    return mapValue(BV, LVar);
  }
  break;

  case OpFunctionParameter: {
    auto BA = static_cast<SPIRVFunctionParameter*>(BV);
    assert (F && "Invalid function");
    unsigned ArgNo = 0;
    for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end(); I != E;
        ++I, ++ArgNo) {
      if (ArgNo == BA->getArgNo())
        return mapValue(BV, &(*I));
    }
    spirv_assert (0 && "Invalid argument");
    return NULL;
  }
  break;

  case OpFunction:
    return mapValue(BV, transFunction(static_cast<SPIRVFunction *>(BV)));

  case OpLabel:
    return mapValue(BV, BasicBlock::Create(*Context, BV->getName(), F));
    break;
  default:
    // do nothing
    break;
  }

  // Creation of place holder
  if (CreatePlaceHolder) {
    auto GV = new GlobalVariable(*M,
        transType(BV->getType()),
        false,
        GlobalValue::PrivateLinkage,
        nullptr,
        std::string(kPlaceholderPrefix) + BV->getName(),
        0, GlobalVariable::NotThreadLocal, 0);
    auto LD = new LoadInst(GV, BV->getName(), BB);
    PlaceholderMap[BV] = LD;
    return mapValue(BV, LD);
  }

  // Translation of instructions
  switch (BV->getOpCode()) {
  case OpBranch: {
    auto BR = static_cast<SPIRVBranch *>(BV);
    assert(BB && "Invalid BB");
    return mapValue(BV, BranchInst::Create(
      dyn_cast<BasicBlock>(transValue(BR->getTargetLabel(), F, BB)), BB));
    }
    break;

  case OpBranchConditional: {
    auto BR = static_cast<SPIRVBranchConditional *>(BV);
    assert(BB && "Invalid BB");
    return mapValue(BV, BranchInst::Create(
      dyn_cast<BasicBlock>(transValue(BR->getTrueLabel(), F, BB)),
      dyn_cast<BasicBlock>(transValue(BR->getFalseLabel(), F, BB)),
      // cond must be an i1, truncate bool to i1 if it was an i8.
      transValue(BR->getCondition(), F, BB, true, BoolAction::Truncate),
      BB));
    }
    break;

  case OpPhi: {
    auto Phi = static_cast<SPIRVPhi *>(BV);
    assert(BB && "Invalid BB");
    auto LPhi = dyn_cast<PHINode>(mapValue(BV, PHINode::Create(
      transType(Phi->getType()),
      Phi->getPairs().size() / 2,
      Phi->getName(),
      BB)));
    Phi->foreachPair([&](SPIRVValue *IncomingV, SPIRVBasicBlock *IncomingBB,
      size_t Index){
      auto Translated = transValue(IncomingV, F, BB);
      LPhi->addIncoming(Translated,
        dyn_cast<BasicBlock>(transValue(IncomingBB, F, BB)));
    });
    return LPhi;
    }
    break;

  case OpReturn:
    assert(BB && "Invalid BB");
    return mapValue(BV, ReturnInst::Create(*Context, BB));
    break;

  case OpReturnValue: {
    auto RV = static_cast<SPIRVReturnValue *>(BV);
    return mapValue(BV, ReturnInst::Create(*Context,
      transValue(RV->getReturnValue(), F, BB), BB));
    }
    break;

  case OpStore: {
    SPIRVStore *BS = static_cast<SPIRVStore*>(BV);
    assert(BB && "Invalid BB");
    auto *pValue    = transValue(BS->getSrc(), F, BB);
    auto *pPointer  = transValue(BS->getDst(), F, BB);
    bool isVolatile =
        BS->hasDecorate(DecorationVolatile) || BS->SPIRVMemoryAccess::getVolatile() != 0;
    unsigned alignment = BS->SPIRVMemoryAccess::getAlignment();

    if (auto *CS = dyn_cast<ConstantStruct>(pValue))
    {
        // Break up a store with a literal struct as the value as we don't have any
        // legalization infrastructure to do it:
        // Ex.
        // store %0 { <2 x i32> <i32 -2100480000, i32 2100480000>, %1 { i32 -2100483600, i8 -128 } }, %0 addrspace(1)* %a
        // =>
        // %CS.tmpstore = alloca %0
        // %0 = getelementptr inbounds %0* %CS.tmpstore, i32 0, i32 0
        // store <2 x i32> <i32 -2100480000, i32 2100480000>, <2 x i32>* %0
        // %1 = getelementptr inbounds %0* %CS.tmpstore, i32 0, i32 1
        // %2 = getelementptr inbounds %1* %1, i32 0, i32 0
        // store i32 -2100483600, i32* %2
        // %3 = getelementptr inbounds %1* %1, i32 0, i32 1
        // store i8 -128, i8* %3
        // %4 = bitcast %0 addrspace(1)* %a to i8 addrspace(1)*
        // %5 = bitcast %0* %CS.tmpstore to i8*
        // call void @llvm.memcpy.p1i8.p0i8.i64(i8 addrspace(1)* %4, i8* %5, i64 16, i32 0, i1 false)
        // So we emit this store in a similar fashion as clang would.
        IRBuilder<> IRB(&F->getEntryBlock(), F->getEntryBlock().begin());
        auto DL = M->getDataLayout();
        std::function<void(ConstantStruct*, Value*)>
        LowerConstantStructStore = [&](ConstantStruct *CS, Value *pointer)
        {
            for (unsigned I = 0, E = CS->getNumOperands(); I != E; I++)
            {
                auto *op = CS->getOperand(I);
                auto *pGEP = IRB.CreateConstInBoundsGEP2_32(nullptr, pointer, 0, I);
                if (auto *InnerCS = dyn_cast<ConstantStruct>(op))
                    LowerConstantStructStore(InnerCS, pGEP);
                else
                    IRB.CreateStore(op, pGEP);
            }
        };
        auto *pAlloca = IRB.CreateAlloca(pValue->getType(), nullptr, "CS.tmpstore");
        IRB.SetInsertPoint(BB);
        LowerConstantStructStore(CS, pAlloca);
        auto *pDst = IRB.CreateBitCast(pPointer,
            Type::getInt8PtrTy(*Context, pPointer->getType()->getPointerAddressSpace()));
        auto *pSrc = IRB.CreateBitCast(pAlloca, Type::getInt8PtrTy(*Context));
        auto *pMemCpy = IRB.CreateMemCpy(pDst, pSrc,
            DL.getTypeAllocSize(pAlloca->getAllocatedType()),
            alignment, isVolatile);
        return mapValue(BV, pMemCpy);
    }

    return mapValue(BV, new StoreInst(
      pValue,
      pPointer,
      isVolatile,
      alignment,
      BB));
  }
  break;

  case OpLoad: {
    SPIRVLoad *BL = static_cast<SPIRVLoad*>(BV);
    assert(BB && "Invalid BB");
    return mapValue(BV, new LoadInst(
      transValue(BL->getSrc(), F, BB),
      BV->getName(),
      BL->hasDecorate(DecorationVolatile) || BL->SPIRVMemoryAccess::getVolatile() != 0,
      BL->SPIRVMemoryAccess::getAlignment(),
      BB));
    }
    break;

  case OpCopyMemorySized: {
    SPIRVCopyMemorySized *BC = static_cast<SPIRVCopyMemorySized *>(BV);
    assert(BB && "Invalid BB");
    
    SPIRVValue* BS = BC->getSource();
    SPIRVValue* BT = BC->getTarget();
    Value* SrcValue = transValue(BS, F, BB);
    Value* TrgValue = transValue(BT, F, BB);
    
    //Create Bitcast of the Target and source Arguments
    //This is needed in the case the either the source or target arguments are of
    //<2 x i32>* type. LLVM only supports i8* type.
    IRBuilder<> IRB(BB);
    Value* pDst = IRB.CreateBitCast(
                        TrgValue,
                        Type::getInt8PtrTy(*Context, TrgValue->getType()->getPointerAddressSpace()));
    Value* pSrc = IRB.CreateBitCast(
                        SrcValue,
                        Type::getInt8PtrTy(*Context, SrcValue->getType()->getPointerAddressSpace()));

    //Get all types needed to create llvm::memcpy
    Type *Int1Ty = Type::getInt1Ty(*Context);
    Type* Int32Ty = Type::getInt32Ty(*Context);
    Type* VoidTy = Type::getVoidTy(*Context);
    Type* DstTy = pDst->getType();
    Type* SrcTy = pSrc->getType();
    Type* SizeTy = transType(BC->getSize()->getType());
    Type* ArgTy[] = { DstTy, SrcTy, SizeTy, Int32Ty, Int1Ty };

    std::string FuncName;
    raw_string_ostream TempName(FuncName);
    TempName << "llvm.memcpy";
    TempName << ".p" << SPIRSPIRVAddrSpaceMap::rmap(BT->getType()->getPointerStorageClass()) << "i8";
    TempName << ".p" << SPIRSPIRVAddrSpaceMap::rmap(BS->getType()->getPointerStorageClass()) << "i8";
    if (BC->getSize()->getType()->getBitWidth() == 32)
       TempName << ".i32";
    else
       TempName << ".i64";
    TempName.flush();

    FunctionType *FT = FunctionType::get(VoidTy, ArgTy, false);
    Function *Func = dyn_cast<Function>(M->getOrInsertFunction(FuncName, FT));
    assert(Func && Func->getFunctionType() == FT && "Function type mismatch");
    Func->setLinkage(GlobalValue::ExternalLinkage);

    if (isFuncNoUnwind())
      Func->addFnAttr(Attribute::NoUnwind);

    Value *Arg[] = { pDst,
                     pSrc,
                     dyn_cast<llvm::ConstantInt>(transValue(BC->getSize(),Func, BB)),
                     ConstantInt::get(Int32Ty,BC->SPIRVMemoryAccess::getAlignment()),
                     ConstantInt::get(Int1Ty,BC->hasDecorate(DecorationVolatile) || BC->SPIRVMemoryAccess::getVolatile() != 0 ) };

    return mapValue( BV, CallInst::Create(Func, Arg, "", BB));
  }
  break;
  case OpCopyObject: {
    auto BI = static_cast<SPIRVInstTemplateBase*>(BV);
    auto source = transValue( BI->getOperand( 0 ),F,BB );
    return mapValue( BV,source );
  }
  case OpSelect: {
    SPIRVSelect *BS = static_cast<SPIRVSelect*>(BV);
    assert(BB && "Invalid BB");
    return mapValue(BV, SelectInst::Create(
      // cond must be an i1, truncate bool to i1 if it was an i8.
      transValue(BS->getCondition(), F, BB, true, BoolAction::Truncate),
      transValue(BS->getTrueValue(), F, BB),
      transValue(BS->getFalseValue(), F, BB),
      BV->getName(), BB));
    }
    break;

  case OpSwitch: {
    auto BS = static_cast<SPIRVSwitch *>(BV);
    assert(BB && "Invalid BB");
    auto Select = transValue(BS->getSelect(), F, BB);
    auto LS = SwitchInst::Create(Select,
      dyn_cast<BasicBlock>(transValue(BS->getDefault(), F, BB)),
      BS->getNumPairs(), BB);
    BS->foreachPair(
        [&](SPIRVSwitch::LiteralTy Literals, SPIRVBasicBlock *Label) {
        assert(!Literals.empty() && "Literals should not be empty");
        assert(Literals.size() <= 2 && "Number of literals should not be more then two");
        uint64_t Literal = uint64_t(Literals.at(0));
        if (Literals.size() == 2) {
          Literal += uint64_t(Literals.at(1)) << 32;
        }
          LS->addCase(ConstantInt::get(dyn_cast<IntegerType>(Select->getType()), Literal),
                      dyn_cast<BasicBlock>(transValue(Label, F, BB)));
        });
    return mapValue(BV, LS);
    }
    break;

  case OpAccessChain:
  case OpInBoundsAccessChain:
  case OpPtrAccessChain:
  case OpInBoundsPtrAccessChain: {
    auto AC = static_cast<SPIRVAccessChainBase *>(BV);
    auto Base = transValue(AC->getBase(), F, BB);
    auto Index = transValue(AC->getIndices(), F, BB);
    if (!AC->hasPtrIndex())
      Index.insert(Index.begin(), getInt32(M, 0));
    auto IsInbound = AC->isInBounds();
    Value *V = nullptr;
    if (BB) {
      auto GEP = GetElementPtrInst::Create(nullptr, Base, Index, BV->getName(), BB);
      GEP->setIsInBounds(IsInbound);
      V = GEP;
    } else {
      V = ConstantExpr::getGetElementPtr(nullptr, dyn_cast<Constant>(Base), Index, IsInbound);
    }
    return mapValue(BV, V);
    }
    break;

  case OpCompositeExtract: {
    SPIRVCompositeExtract *CE = static_cast<SPIRVCompositeExtract *>(BV);
    assert(BB && "Invalid BB");
    assert(CE->getComposite()->getType()->isTypeVector() && "Invalid type");
    assert(CE->getIndices().size() == 1 && "Invalid index");
    return mapValue(BV, ExtractElementInst::Create(
      transValue(CE->getComposite(), F, BB),
      ConstantInt::get(*Context, APInt(32, CE->getIndices()[0])),
      BV->getName(), BB));
    }
    break;

  case OpVectorExtractDynamic: {
    auto CE = static_cast<SPIRVVectorExtractDynamic *>(BV);
    assert(BB && "Invalid BB");
    return mapValue(BV, ExtractElementInst::Create(
      transValue(CE->getVector(), F, BB),
      transValue(CE->getIndex(), F, BB),
      BV->getName(), BB));
    }
    break;

  case OpCompositeInsert: {
    auto CI = static_cast<SPIRVCompositeInsert *>(BV);
    assert(BB && "Invalid BB");
    assert(CI->getComposite()->getType()->isTypeVector() && "Invalid type");
    assert(CI->getIndices().size() == 1 && "Invalid index");
    return mapValue(BV, InsertElementInst::Create(
      transValue(CI->getComposite(), F, BB),
      transValue(CI->getObject(), F, BB),
      ConstantInt::get(*Context, APInt(32, CI->getIndices()[0])),
      BV->getName(), BB));
    }
    break;

  case OpVectorInsertDynamic: {
    auto CI = static_cast<SPIRVVectorInsertDynamic *>(BV);
    assert(BB && "Invalid BB");
    return mapValue(BV, InsertElementInst::Create(
      transValue(CI->getVector(), F, BB),
      transValue(CI->getComponent(), F, BB),
      transValue(CI->getIndex(), F, BB),
      BV->getName(), BB));
    }
    break;

  case OpVectorShuffle: {
    auto VS = static_cast<SPIRVVectorShuffle *>(BV);
    assert(BB && "Invalid BB");
    std::vector<Constant *> Components;
    IntegerType *Int32Ty = IntegerType::get(*Context, 32);
    for (auto I : VS->getComponents()) {
      if (I == static_cast<SPIRVWord>(-1))
        Components.push_back(UndefValue::get(Int32Ty));
      else
        Components.push_back(ConstantInt::get(Int32Ty, I));
    }
    return mapValue(BV, new ShuffleVectorInst(
      transValue(VS->getVector1(), F, BB),
      transValue(VS->getVector2(), F, BB),
      ConstantVector::get(Components),
      BV->getName(), BB));
    }
    break;

  case OpFunctionCall: {
    SPIRVFunctionCall *BC = static_cast<SPIRVFunctionCall *>(BV);
    assert(BB && "Invalid BB");
    auto Call = CallInst::Create(
      transFunction(BC->getFunction()),
      transValue(BC->getArgumentValues(), F, BB),
      BC->getName(),
      BB);
    setCallingConv(Call);
    setAttrByCalledFunc(Call);
    return mapValue(BV, Call);
    }
    break;

  case OpExtInst:
    return mapValue(BV, transOCLBuiltinFromExtInst(
      static_cast<SPIRVExtInst *>(BV), BB));
    break;

  case OpSNegate: {
    SPIRVUnary *BC = static_cast<SPIRVUnary*>(BV);
    return mapValue(BV, BinaryOperator::CreateNSWNeg(
      transValue(BC->getOperand(0), F, BB),
      BV->getName(), BB));
    }

  case OpFNegate: {
    SPIRVUnary *BC = static_cast<SPIRVUnary*>(BV);
    return mapValue(BV, BinaryOperator::CreateFNeg(
      transValue(BC->getOperand(0), F, BB),
      BV->getName(), BB));
    }
    break;

  case OpNot: {
    SPIRVUnary *BC = static_cast<SPIRVUnary*>(BV);
    return mapValue(BV, BinaryOperator::CreateNot(
      transValue(BC->getOperand(0), F, BB),
      BV->getName(), BB));
    }
    break;

  case OpBitCount:{
    auto BI = static_cast<SPIRVInstruction *>(BV);
    Type* RetTy = transType(BI->getType());
    auto NewCI = CallInst::Create(F, "__builtin_spirv_OpBitCount_i8", BB);
    return mapValue(BV, ZExtInst::CreateIntegerCast(NewCI, RetTy, 0, "", BB));
  }
  case OpSizeOf:
  {
      auto BI = static_cast<SPIRVSizeOf*>(BV);
      assert(BI->getOpWords().size() == 1 && "OpSizeOf takes one argument!");
      // getOperands() returns SPIRVValue(s) but this argument is a SPIRVType so
      // we have to just grab it by its entry id.
      auto pArg = BI->get<SPIRVTypePointer>(BI->getOpWord(0));
      auto pointee = pArg->getPointerElementType();
      auto DL = M->getDataLayout();
      uint64_t size = DL.getTypeAllocSize(transType(pointee));
      return mapValue(BV, ConstantInt::get(Type::getInt32Ty(*Context), size));
  }
  case OpCreatePipeFromPipeStorage:
  {
      auto BI = static_cast<SPIRVCreatePipeFromPipeStorage*>(BV);
      assert(BI->getOpWords().size() == 1 &&
          "OpCreatePipeFromPipeStorage takes one argument!");
      return mapValue(BI, CastInst::CreateTruncOrBitCast(
          transValue(BI->getOperand(0), F, BB),
          transType(BI->getType()),
          "", BB));
  }
  case OpUnreachable:
  {
      return mapValue(BV, new UnreachableInst(*Context, BB));
  }
  case OpLifetimeStart:
  case OpLifetimeStop:
  {
      return mapValue(BV,
          transLifetimeInst(static_cast<SPIRVInstTemplateBase*>(BV), BB, F));
  }
  case OpVectorTimesScalar:
  {
      auto BI = static_cast<SPIRVInstTemplateBase*>(BV);

      auto Vector = transValue(BI->getOperand(0), F, BB);
      auto Scalar = transValue(BI->getOperand(1), F, BB);

      auto VecType = cast<VectorType>(Vector->getType());
      auto Undef   = UndefValue::get(VecType);

      auto ScalarVec = InsertElementInst::Create(Undef, Scalar,
          ConstantInt::getNullValue(Type::getInt32Ty(*Context)), "", BB);

      for (unsigned i = 1; i < VecType->getNumElements(); i++)
      {
          ScalarVec = InsertElementInst::Create(ScalarVec, Scalar,
              ConstantInt::get(Type::getInt32Ty(*Context), i), "", BB);
      }

      return mapValue(BV, BinaryOperator::CreateFMul(Vector, ScalarVec, "", BB));
  }
  case OpSMod:
  {
      auto BI = static_cast<SPIRVSRem*>(BV);
      auto *a = transValue(BI->getOperand(0), F, BB);
      auto *b = transValue(BI->getOperand(1), F, BB);
      auto *zero = ConstantInt::getNullValue(a->getType());
      auto *ShiftAmt = ConstantInt::get(
          a->getType()->getScalarType(),
          a->getType()->getScalarSizeInBits() - 1);
      auto *ShiftOp = isa<VectorType>(a->getType()) ?
          ConstantVector::getSplat(a->getType()->getVectorNumElements(), ShiftAmt) :
          ShiftAmt;

      // OCL C:
      //
      // int mod(int a, int b)
      // {
      //     int out = a % b;
      //     if (((a >> 31) != (b >> 31)) & out != 0)
      //     {
      //         // only add b to out if sign(a) != sign(b) and out != 0.
      //         out += b;
      //     }
      //
      //     return out;
      // }

      // %out = srem %a, %b
      auto *out = BinaryOperator::CreateSRem(a, b, "", BB);
      // %sha = ashr %a, 31
      auto *sha = BinaryOperator::CreateAShr(a, ShiftOp, "", BB); 
      // %shb = ashr %b, 31
      auto *shb = BinaryOperator::CreateAShr(b, ShiftOp, "", BB); 
      // %cmp1 = icmp ne %sha, %shb
      auto *cmp1 = CmpInst::Create(Instruction::ICmp, llvm::CmpInst::ICMP_NE,
          sha, shb, "", BB);
      // %cmp2 = icmp ne %out, 0
      auto *cmp2 = CmpInst::Create(Instruction::ICmp, llvm::CmpInst::ICMP_NE,
          out, zero, "", BB);
      // %and  = and %cmp1, %cmp2
	  auto *and1 = BinaryOperator::CreateAnd(cmp1, cmp2, "", BB);
      // %add  = add %out, %b
      auto *add = BinaryOperator::CreateAdd(out, b, "", BB); 
      // %sel  = select %and, %add, %out
      auto *sel = SelectInst::Create(and1, add, out, "", BB);

      return mapValue(BV, sel);
  }
  default: {
    auto OC = BV->getOpCode();
    if (isSPIRVCmpInstTransToLLVMInst(static_cast<SPIRVInstruction*>(BV))) {
      return mapValue(BV, transCmpInst(BV, BB, F));
    } else if (OCLSPIRVBuiltinMap::find(OC)) {
       return mapValue(BV, transSPIRVBuiltinFromInst(
          static_cast<SPIRVInstruction *>(BV), BB));
    } else if (isBinaryShiftLogicalBitwiseOpCode(OC) ||
                isLogicalOpCode(OC)) {
          return mapValue(BV, transShiftLogicalBitwiseInst(BV, BB, F));
    } else if (isCvtOpCode(OC)) {
        auto BI = static_cast<SPIRVInstruction *>(BV);
        Value *Inst = nullptr;
        if (BI->hasFPRoundingMode() || BI->isSaturatedConversion())
           Inst = transSPIRVBuiltinFromInst(BI, BB);
        else
          Inst = transConvertInst(BV, F, BB);
        return mapValue(BV, Inst);
    }
    return mapValue(BV, transSPIRVBuiltinFromInst(
      static_cast<SPIRVInstruction *>(BV), BB));
  }

  spirv_assert(0 && "Cannot translate");
  llvm_unreachable("Translation of SPIRV instruction not implemented");
  return NULL;
  }
}

template<class SourceTy, class FuncTy>
bool
SPIRVToLLVM::foreachFuncCtlMask(SourceTy Source, FuncTy Func) {
  SPIRVWord FCM = Source->getFuncCtlMask();
  SPIRSPIRVFuncCtlMaskMap::foreach([&](Attribute::AttrKind Attr,
      SPIRVFunctionControlMaskKind Mask){
    if (FCM & Mask)
      Func(Attr);
  });
  return true;
}

Function *
SPIRVToLLVM::transFunction(SPIRVFunction *BF) {
  auto Loc = FuncMap.find(BF);
  if (Loc != FuncMap.end())
    return Loc->second;

  auto IsKernel = BM->isEntryPoint(ExecutionModelKernel, BF->getId());
  auto Linkage = IsKernel ? GlobalValue::ExternalLinkage :
      transLinkageType(BF);
  FunctionType *FT = dyn_cast<FunctionType>(transType(BF->getFunctionType()));
  Function *F = dyn_cast<Function>(mapValue(BF, Function::Create(FT, Linkage,
      BF->getName(), M)));
  mapFunction(BF, F);
  if (!F->isIntrinsic()) {
    F->setCallingConv(IsKernel ? CallingConv::SPIR_KERNEL :
        CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      F->addFnAttr(Attribute::NoUnwind);
    foreachFuncCtlMask(BF, [&](Attribute::AttrKind Attr){
      F->addFnAttr(Attr);
    });
  }

  for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end(); I != E;
      ++I) {
    auto BA = BF->getArgument(I->getArgNo());
    mapValue(BA, &(*I));
    const std::string &ArgName = BA->getName();
    if (ArgName.empty())
      continue;
    I->setName(ArgName);
    BA->foreachAttr([&](SPIRVFuncParamAttrKind Kind){
     if (Kind == FunctionParameterAttributeCount)
        return;
      F->addAttribute(I->getArgNo() + 1, SPIRSPIRVFuncParamAttrMap::rmap(Kind));
    });
  }
  BF->foreachReturnValueAttr([&](SPIRVFuncParamAttrKind Kind){
    if (Kind == FunctionParameterAttributeCount)
      return;
    F->addAttribute(AttributeSet::ReturnIndex,
        SPIRSPIRVFuncParamAttrMap::rmap(Kind));
  });

  // Creating all basic blocks before creating instructions.
  for (size_t I = 0, E = BF->getNumBasicBlock(); I != E; ++I) {
    transValue(BF->getBasicBlock(I), F, nullptr, true, BoolAction::Noop);
  }

  for (size_t I = 0, E = BF->getNumBasicBlock(); I != E; ++I) {
    SPIRVBasicBlock *BBB = BF->getBasicBlock(I);
    BasicBlock *BB = dyn_cast<BasicBlock>(transValue(BBB, F, nullptr, true, BoolAction::Noop));
    for (size_t BI = 0, BE = BBB->getNumInst(); BI != BE; ++BI) {
      SPIRVInstruction *BInst = BBB->getInst(BI);
      transValue(BInst, F, BB, false, BoolAction::Noop);
    }
  }
  return F;
}

uint64_t SPIRVToLLVM::calcImageType(const SPIRVValue *ImageVal)
{

    const SPIRVTypeImage* TI = ImageVal->getType()->isTypeSampledImage() ?
        static_cast<SPIRVTypeSampledImage*>(ImageVal->getType())->getImageType() :
        static_cast<SPIRVTypeImage*>(ImageVal->getType());

    const auto &Desc = TI->getDescriptor();
    uint64_t ImageType = 0;

    ImageType |= ((uint64_t)Desc.Dim                 & 0x7) << 59;
    ImageType |= ((uint64_t)Desc.Depth               & 0x1) << 58;
    ImageType |= ((uint64_t)Desc.Arrayed             & 0x1) << 57;
    ImageType |= ((uint64_t)Desc.MS                  & 0x1) << 56;
    ImageType |= ((uint64_t)Desc.Sampled             & 0x3) << 62;
    ImageType |= ((uint64_t)TI->getAccessQualifier() & 0x3) << 54;

    return ImageType;
}

Instruction *
SPIRVToLLVM::transSPIRVBuiltinFromInst(SPIRVInstruction *BI, BasicBlock *BB) {
  assert(BB && "Invalid BB");
  const auto OC = BI->getOpCode();
  auto Ops = BI->getOperands();
  // builtins use bool for scalar and ucharx for vector bools.  Truncate
  // or promote as necessary.
  std::vector<Value *> operands;
  for (auto I : Ops)
  {
      BoolAction Action = I->getType()->isTypeBool() ?
          BoolAction::Truncate :
          BoolAction::Promote;
      operands.push_back(transValue(I, BB->getParent(), BB, true, Action));
  }

  {
      // Update image ops to add 'image type' to operands:
      switch (OC)
      {
      case OpSampledImage:
      case OpImageRead:
      case OpImageWrite:
      case OpImageQuerySize:
      case OpImageQuerySizeLod:
      {
          // resolving argument imageType for
          // __builtin_spirv_OpSampledImage(%opencl.image2d_t.read_only addrspace(1)* %srcimg0,
          //  i64 imageType, i32 20)
          Type *pType = Type::getInt64Ty(*Context);
          uint64_t ImageType = calcImageType(BI->getOperands()[0]);
          operands.insert(operands.begin() + 1, ConstantInt::get(pType, ImageType));
          break;
      }
      default:
          break;
      }

      // WA for image inlining:
      switch (OC)
      {
      case OpImageSampleExplicitLod:
      case OpImageRead:
      case OpImageWrite:
      case OpImageQueryFormat:
      case OpImageQueryOrder:
      case OpImageQuerySizeLod:
      case OpImageQuerySize:
      case OpImageQueryLevels:
      case OpImageQuerySamples:
      {
          auto type = getOrCreateOpaquePtrType(M, "struct.ImageDummy");
          auto val = Constant::getNullValue(type);
          operands.push_back(val);
          break;
      }
      case OpNamedBarrierInitialize:
      {
          auto ReuseGlobalIdValue = llvm::BitCastInst::CreatePointerCast(m_NamedBarrierVar,
              getNamedBarrierType(), "", BB);
          operands.push_back(ReuseGlobalIdValue);
          operands.push_back(m_named_barrier_id);
          break;
      }
      default:
          break;
      }
  }

  bool hasReturnTypeInTypeList = false;

  std::string suffix;
  if (isCvtOpCode(OC))
  {
      hasReturnTypeInTypeList = true;

      if (BI->isSaturatedConversion() &&
          !(BI->getOpCode() == OpSatConvertSToU || BI->getOpCode() == OpSatConvertUToS))
      {
          suffix += "_Sat";
      }

      SPIRVFPRoundingModeKind kind;
      std::string rounding_string;
      if (BI->hasFPRoundingMode(&kind))
      {
          switch (kind)
          {
              case FPRoundingModeRTE:
                  rounding_string = "_RTE";
                  break;
              case FPRoundingModeRTZ:
                  rounding_string = "_RTZ";
                  break;
              case FPRoundingModeRTP:
                  rounding_string = "_RTP";
                  break;
              case FPRoundingModeRTN:
                  rounding_string = "_RTN";
                  break;
              default:
                  break;
          } 
      }
      suffix += rounding_string;  
  }

  std::vector<Type*> ArgTys;
  for (auto &v : operands)
  {
      // replace function by function pointer
      auto *ArgTy = v->getType()->isFunctionTy() ?
          v->getType()->getPointerTo() :
          v->getType();

      ArgTys.push_back(ArgTy);
  }

  // OpImageSampleExplicitLod: SImage | Coordinate | ImageOperands | LOD
  // OpImageWrite:             Image  | Image Type | Coordinate | Texel
  // OpImageRead:              Image  | Image Type | Coordinate

  // Look for opaque image pointer operands and convert it with an i64 type
  for (auto i = 0U; i < BI->getOperands().size(); i++) {

      SPIRVValue* imagePtr = BI->getOperands()[i];

      if (imagePtr->getType()->isTypeImage())
      {
          assert(isa<PointerType>(transType(imagePtr->getType())));

          Value *ImageArgVal = llvm::PtrToIntInst::Create(
              Instruction::PtrToInt,
              transValue(imagePtr, BB->getParent(), BB),
              Type::getInt64Ty(*Context),
              "ImageArgVal",
              BB);

          // replace opaque pointer type with i64 type
          assert(ArgTys[i] == transType(imagePtr->getType()));
          ArgTys[i] = ImageArgVal->getType();
          operands[i] = ImageArgVal;
      }

  }

  if (isImageOpCode(OC))
  {
      // Writes have a void return type that is not part of the mangle.
      if (OC != OpImageWrite)
      {
          hasReturnTypeInTypeList = true;
      }

      // need to widen coordinate type
      SPIRVValue* coordinate = BI->getOperands()[1];
      Type* coordType = transType(coordinate->getType());

      Value *imageCoordinateWiden = nullptr;
      if (!isa<VectorType>(coordType))
      {
          Value *undef = UndefValue::get(VectorType::get(coordType, 4));

          imageCoordinateWiden = InsertElementInst::Create(
              undef,
              transValue(coordinate, BB->getParent(), BB),
              ConstantInt::get(Type::getInt32Ty(*Context), 0),
              "",
              BB);
      }
      else if (coordType->getVectorNumElements() != 4)
      {
          Value *undef = UndefValue::get(coordType);

          SmallVector<Constant*, 4> shuffleIdx;
          for (unsigned i = 0; i < coordType->getVectorNumElements(); i++)
              shuffleIdx.push_back(ConstantInt::get(Type::getInt32Ty(*Context), i));

          for (unsigned i = coordType->getVectorNumElements(); i < 4; i++)
              shuffleIdx.push_back(ConstantInt::get(Type::getInt32Ty(*Context), 0));

          imageCoordinateWiden = new ShuffleVectorInst(
              transValue(coordinate, BB->getParent(), BB),
              undef,
              ConstantVector::get(shuffleIdx),
              "",
              BB);
      }

      if (imageCoordinateWiden != nullptr)
      {
          const  uint32_t  CoordArgIdx = (OC == OpImageSampleExplicitLod) ? 1 : 2;
          ArgTys[CoordArgIdx] = imageCoordinateWiden->getType();
          operands[CoordArgIdx] = imageCoordinateWiden;
      }
  }

  if ((OC == OpImageQuerySizeLod) ||
      (OC == OpImageQuerySize))
  {
      hasReturnTypeInTypeList = true;
  }

  Type *RetTy = Type::getVoidTy(*Context);
  if (BI->hasType())
  {
      auto *pTrans = transType(BI->getType());
      RetTy = BI->getType()->isTypeBool() ?
          truncBoolType(BI->getType(), pTrans) :
          pTrans;
  }

  if (hasReturnTypeInTypeList)
  {
      ArgTys.insert(ArgTys.begin(), RetTy);
  }

  std::string builtinName(getSPIRVBuiltinName(OC, ArgTys, suffix));

  if (hasReturnTypeInTypeList)
  {
      ArgTys.erase(ArgTys.begin());
  }

  Function* Func = M->getFunction(builtinName);
  FunctionType* FT = FunctionType::get(RetTy, ArgTys, false);
  if (!Func || Func->getFunctionType() != FT)
  {
     Func = Function::Create(FT, GlobalValue::ExternalLinkage, builtinName, M);
     Func->setCallingConv(CallingConv::SPIR_FUNC);
     if (isFuncNoUnwind())
        Func->addFnAttr(Attribute::NoUnwind);
  }

  auto Call = CallInst::Create(Func, operands, "", BB);
     
  Call->setName(BI->getName());
  setAttrByCalledFunc(Call);
  return Call;
}


void SPIRVToLLVM::addNamedBarrierArray()
{
  llvm::SmallVector<Type*, 3> NamedBarrierArray(3, Type::getInt32Ty(*Context));
  Type *bufType = ArrayType::get(StructType::create(*Context, NamedBarrierArray, "struct.__namedBarrier"), uint64_t(8));
  Twine globalName = Twine("NamedBarrierArray");
  Twine globalName2 = Twine("NamedBarrierID");
  m_NamedBarrierVar = new GlobalVariable(*M, bufType, false,
      GlobalVariable::InternalLinkage, ConstantAggregateZero::get(bufType),
      globalName, nullptr,
      GlobalVariable::ThreadLocalMode::NotThreadLocal,
      SPIRAS_Local);
  m_named_barrier_id = new GlobalVariable(*M, Type::getInt32Ty(*Context), false,
      GlobalVariable::InternalLinkage, 
      ConstantInt::get(Type::getInt32Ty(*Context), 0),
      globalName2, nullptr,
      GlobalVariable::ThreadLocalMode::NotThreadLocal,
      SPIRAS_Local);
}

Type* SPIRVToLLVM::getNamedBarrierType()
{
    auto newType = m_NamedBarrierVar->getType()->getPointerElementType()->getArrayElementType()->getPointerTo(SPIRAS_Local);
    return newType;
}

bool
SPIRVToLLVM::translate() {
  if (!transAddressingModel())
    return false;

  compileUnit = DbgTran.createCompileUnit();
  addNamedBarrierArray(); 

  for (unsigned I = 0, E = BM->getNumVariables(); I != E; ++I) {
    auto BV = BM->getVariable(I);
    if (BV->getStorageClass() != StorageClassFunction)
      transValue(BV, nullptr, nullptr, true, BoolAction::Noop);
  }

  for (unsigned I = 0, E = BM->getNumFunctions(); I != E; ++I) {
    transFunction(BM->getFunction(I));
  }
  if (!transKernelMetadata())
    return false;
  if (!transFPContractMetadata())
    return false;
  if (!transSourceLanguage())
    return false;
  if (!transSourceExtension())
    return false;
  if (!transCompilerOption())
    return false;
  if (!transOCLBuiltinsFromVariables())
    return false;
  if (!postProcessOCL())
    return false;

  DbgTran.transGlobals();

  DbgTran.finalize();
  return true;
}

bool
SPIRVToLLVM::transAddressingModel() {
  switch (BM->getAddressingModel()) {
  case AddressingModelPhysical64:
    M->setTargetTriple(SPIR_TARGETTRIPLE64);
    M->setDataLayout(SPIR_DATALAYOUT64);
    break;
  case AddressingModelPhysical32:
    M->setTargetTriple(SPIR_TARGETTRIPLE32);
    M->setDataLayout(SPIR_DATALAYOUT32);
    break;
  case AddressingModelLogical:
    // Do not set target triple and data layout
    break;
  default:
    SPIRVCKRT(0, InvalidAddressingModel, "Actual addressing mode is " +
        (unsigned)BM->getAddressingModel());
  }
  return true;
}

bool
SPIRVToLLVM::transDecoration(SPIRVValue *BV, Value *V) {
  if (!transAlign(BV, V))
    return false;
  DbgTran.transDbgInfo(BV, V);
  return true;
}

bool
SPIRVToLLVM::transFPContractMetadata() {
  bool ContractOff = false;
  for (unsigned I = 0, E = BM->getNumFunctions(); I != E; ++I) {
    SPIRVFunction *BF = BM->getFunction(I);
    if (!isOpenCLKernel(BF))
      continue;
    if (BF->getExecutionMode(ExecutionModeContractionOff)) {
      ContractOff = true;
      break;
    }
  }
  if (!ContractOff)
    M->getOrInsertNamedMetadata(spv::kSPIR2MD::FPContract);
  return true;
}

std::string SPIRVToLLVM::transOCLImageTypeAccessQualifier(
    spv::SPIRVTypeImage* ST) {
  return SPIRSPIRVAccessQualifierMap::rmap(ST->getAccessQualifier());
}

Type *
SPIRVToLLVM::decodeVecTypeHint(LLVMContext &C, unsigned code) {
    unsigned VecWidth = code >> 16;
    unsigned Scalar = code & 0xFFFF;
    Type *ST = nullptr;
    switch (Scalar) {
    case 0:
    case 1:
    case 2:
    case 3:
        ST = IntegerType::get(C, 1 << (3 + Scalar));
        break;
    case 4:
        ST = Type::getHalfTy(C);
        break;
    case 5:
        ST = Type::getFloatTy(C);
        break;
    case 6:
        ST = Type::getDoubleTy(C);
        break;
    default:
        llvm_unreachable("Invalid vec type hint");
    }
    if (VecWidth < 1)
        return ST;
    return VectorType::get(ST, VecWidth);
}

bool
SPIRVToLLVM::transKernelMetadata()
{
    IGC::ModuleMetaData MD;
    NamedMDNode *KernelMDs = M->getOrInsertNamedMetadata(SPIR_MD_KERNELS);
    for (unsigned I = 0, E = BM->getNumFunctions(); I != E; ++I)
    {
        SPIRVFunction *BF = BM->getFunction(I);
        Function *F = static_cast<Function *>(getTranslatedValue(BF));
        assert(F && "Invalid translated function");
        if (F->getCallingConv() != CallingConv::SPIR_KERNEL)
            continue;
        std::vector<llvm::Metadata*> KernelMD;
        KernelMD.push_back(ValueAsMetadata::get(F));

        // Generate metadata for kernel_arg_address_spaces
        addOCLKernelArgumentMetadata(Context, KernelMD,
            SPIR_MD_KERNEL_ARG_ADDR_SPACE, BF,
            [=](SPIRVFunctionParameter *Arg){
            SPIRVType *ArgTy = Arg->getType();
            SPIRAddressSpace AS = SPIRAS_Private;
            if (ArgTy->isTypePointer())
                AS = SPIRSPIRVAddrSpaceMap::rmap(ArgTy->getPointerStorageClass());
            else if (ArgTy->isTypeOCLImage() || ArgTy->isTypePipe())
                AS = SPIRAS_Global;
            return ConstantAsMetadata::get(
                ConstantInt::get(Type::getInt32Ty(*Context), AS));
        });
        // Generate metadata for kernel_arg_access_qual
        addOCLKernelArgumentMetadata(Context, KernelMD,
            SPIR_MD_KERNEL_ARG_ACCESS_QUAL, BF,
            [=](SPIRVFunctionParameter *Arg){
            std::string Qual;
            auto T = Arg->getType();
            if (T->isTypeOCLImage()) {
                auto ST = static_cast<SPIRVTypeImage *>(T);
                Qual = transOCLImageTypeAccessQualifier(ST);
            }
            else if (T->isTypePipe()){
                auto PT = static_cast<SPIRVTypePipe *>(T);
                Qual = transOCLPipeTypeAccessQualifier(PT);
            }
            else
                Qual = "none";
            return MDString::get(*Context, Qual);
        });
        // Generate metadata for kernel_arg_type
        addOCLKernelArgumentMetadata(Context, KernelMD,
            SPIR_MD_KERNEL_ARG_TYPE, BF,
            [=](SPIRVFunctionParameter *Arg){
            return transOCLKernelArgTypeName(Arg);
        });
        // Generate metadata for kernel_arg_type_qual
        addOCLKernelArgumentMetadata(Context, KernelMD,
            SPIR_MD_KERNEL_ARG_TYPE_QUAL, BF,
            [=](SPIRVFunctionParameter *Arg){
            std::string Qual;
            if (Arg->hasDecorate(DecorationVolatile))
                Qual = kOCLTypeQualifierName::Volatile;
            Arg->foreachAttr([&](SPIRVFuncParamAttrKind Kind){
                Qual += Qual.empty() ? "" : " ";
                switch (Kind){
                case FunctionParameterAttributeNoAlias:
                    Qual += kOCLTypeQualifierName::Restrict;
                    break;
                case FunctionParameterAttributeNoWrite:
                    Qual += kOCLTypeQualifierName::Const;
                    break;
                default:
                    // do nothing.
                    break;
                }
            });
            if (Arg->getType()->isTypePipe()) {
                Qual += Qual.empty() ? "" : " ";
                Qual += kOCLTypeQualifierName::Pipe;
            }
            return MDString::get(*Context, Qual);
        });
        // Generate metadata for kernel_arg_base_type
        addOCLKernelArgumentMetadata(Context, KernelMD,
            SPIR_MD_KERNEL_ARG_BASE_TYPE, BF,
            [=](SPIRVFunctionParameter *Arg){
            return transOCLKernelArgTypeName(Arg);
        });
        // Generate metadata for kernel_arg_name
        if (BM->getCompileFlag().find("-cl-kernel-arg-info") !=
            std::string::npos) {
            bool ArgHasName = true;
            BF->foreachArgument([&](SPIRVFunctionParameter *Arg){
                ArgHasName &= !Arg->getName().empty();
            });
            if (ArgHasName)
                addOCLKernelArgumentMetadata(Context, KernelMD,
                SPIR_MD_KERNEL_ARG_NAME, BF,
                [=](SPIRVFunctionParameter *Arg){
                return MDString::get(*Context, Arg->getName());
            });
        }
        // Generate metadata for reqd_work_group_size
        if (auto EM = BF->getExecutionMode(ExecutionModeLocalSize)) {
            KernelMD.push_back(getMDNodeStringIntVec(Context,
                spv::kSPIR2MD::WGSize, EM->getLiterals()));
        }
        // Generate metadata for work_group_size_hint
        if (auto EM = BF->getExecutionMode(ExecutionModeLocalSizeHint)) {
            KernelMD.push_back(getMDNodeStringIntVec(Context,
                spv::kSPIR2MD::WGSizeHint, EM->getLiterals()));
        }
        // Generate metadata for vec_type_hint
        if (auto EM = BF->getExecutionMode(ExecutionModeVecTypeHint)) {
            std::vector<Metadata*> MetadataVec;
            MetadataVec.push_back(MDString::get(*Context, spv::kSPIR2MD::VecTyHint));
            Type *VecHintTy = decodeVecTypeHint(*Context, EM->getLiterals()[0]);
            MetadataVec.push_back(ValueAsMetadata::get(UndefValue::get(VecHintTy)));
            MetadataVec.push_back(
                ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*Context),
                0)));
            KernelMD.push_back(MDNode::get(*Context, MetadataVec));
        }

        auto &funcInfo = MD.FuncMD[F];

        // Generate metadata for initializer
        if (auto EM = BF->getExecutionMode(ExecutionModeInitializer)) {
            funcInfo.IsInitializer = true;
        }

        // Generate metadata for finalizer
        if (auto EM = BF->getExecutionMode(ExecutionModeFinalizer)) {
            funcInfo.IsFinalizer = true;
        }

        // Generate metadata for SubgroupSize
        if (auto EM = BF->getExecutionMode(ExecutionModeSubgroupSize))
        {
            unsigned subgroupSize = EM->getLiterals()[0];
            std::vector<Metadata*> MetadataVec;
            MetadataVec.push_back(MDString::get(*Context, spv::kSPIR2MD::ReqdSubgroupSize));
            MetadataVec.push_back(
                ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(*Context),
                subgroupSize)));
            KernelMD.push_back(MDNode::get(*Context, MetadataVec));
        }

        // Generate metadata for SubgroupsPerWorkgroup
        if (auto EM = BF->getExecutionMode(ExecutionModeSubgroupsPerWorkgroup))
        {
            funcInfo.CompiledSubGroupsNumber = EM->getLiterals()[0];
        }

        // Generate metadata for MaxByteOffset decorations
        {
            bool ArgHasMaxByteOffset = false;
            BF->foreachArgument([&](SPIRVFunctionParameter *Arg)
            {
                SPIRVWord offset;
                ArgHasMaxByteOffset |= Arg->hasMaxByteOffset(offset);
            });

            if (ArgHasMaxByteOffset)
            {
                BF->foreachArgument([&](SPIRVFunctionParameter *Arg)
                {
                    SPIRVWord offset;
                    bool ok = Arg->hasMaxByteOffset(offset);
                    // If the decoration is not present on an argument of the function,
                    // encode that as a zero in the metadata.  That currently seems
                    // like a degenerate case wouldn't be worth optimizing.
                    unsigned val = ok ? offset : 0;
                    funcInfo.maxByteOffsets.push_back(val);
                });
            }
        }

        llvm::MDNode *Node = MDNode::get(*Context, KernelMD);
        KernelMDs->addOperand(Node);
    }

    IGC::serialize(MD, M);

    return true;
}

bool
SPIRVToLLVM::transAlign(SPIRVValue *BV, Value *V) {
  if (auto AL = dyn_cast<AllocaInst>(V)) {
    SPIRVWord Align = 0;
    if (BV->hasAlignment(&Align))
      AL->setAlignment(Align);
    return true;
  }
  if (auto GV = dyn_cast<GlobalVariable>(V)) {
    SPIRVWord Align = 0;
    if (BV->hasAlignment(&Align))
      GV->setAlignment(Align);
    return true;
  }
  return true;
}

void
SPIRVToLLVM::transOCLVectorLoadStore(std::string& UnmangledName,
    std::vector<SPIRVWord> &BArgs) {
  if (UnmangledName.find("vload") == 0 &&
      UnmangledName.find("n") != std::string::npos) {
    if (BArgs.back() > 1) {
      std::stringstream SS;
      SS << BArgs.back();
      UnmangledName.replace(UnmangledName.find("n"), 1, SS.str());
    } else {
      UnmangledName.erase(UnmangledName.find("n"), 1);
    }
    BArgs.pop_back();
  } else if (UnmangledName.find("vstore") == 0) {
    if (UnmangledName.find("n") != std::string::npos) {
      auto T = BM->getValueType(BArgs[0]);
      if (T->isTypeVector()) {
        auto W = T->getVectorComponentCount();
        std::stringstream SS;
        SS << W;
        UnmangledName.replace(UnmangledName.find("n"), 1, SS.str());
      } else {
        UnmangledName.erase(UnmangledName.find("n"), 1);
      }
    }
    if (UnmangledName.find("_r") != std::string::npos) {
      UnmangledName.replace(UnmangledName.find("_r"), 2, std::string("_") +
          SPIRSPIRVFPRoundingModeMap::rmap(static_cast<SPIRVFPRoundingModeKind>(
              BArgs.back())));
      BArgs.pop_back();
    }
   }
}

void SPIRVToLLVM::transDebugInfo(SPIRVExtInst* BC, BasicBlock* BB)
{
    if (!BC)
        return;

    auto extOp = (OCLExtOpDbgKind)BC->getExtOp();

    switch (extOp)
    {
    case OCLExtOpDbgKind::DbgDcl:
    {
        OpDebugDeclare dbgDcl(BC);
        auto lvar = dbgDcl.getLocalVar();
        SPIRVValue* spirvVal = static_cast<SPIRVValue*>(BM->getEntry(lvar));
        SPIRVToLLVMValueMap::iterator Loc = ValueMap.find(spirvVal);
        if (Loc != ValueMap.end())
        {
            DbgTran.createDbgDeclare(BC, Loc->second, BB);
        }
        break;
    }

    case OCLExtOpDbgKind::DbgVal:
    {
        OpDebugValue dbgValue(BC);
        auto lvar = dbgValue.getVar();
        SPIRVValue* spirvVal = static_cast<SPIRVValue*>(BM->getEntry(lvar));
        SPIRVToLLVMValueMap::iterator Loc = ValueMap.find(spirvVal);
        if (Loc != ValueMap.end())
        {
            DbgTran.createDbgValue(BC, Loc->second, BB);
        }
        break;
    }

    default:
        break;
    }
}

Instruction *
SPIRVToLLVM::transOCLBuiltinFromExtInst(SPIRVExtInst *BC, BasicBlock *BB) {
  assert(BB && "Invalid BB");
  SPIRVWord EntryPoint = BC->getExtOp();
  SPIRVExtInstSetKind Set = BM->getBuiltinSet(BC->getExtSetId());
  bool IsPrintf = false;
  std::string FuncName;

  if (Set == SPIRVEIS_DebugInfo)
  {
      transDebugInfo(BC, BB);
      return nullptr;
  }

  assert (Set == SPIRVEIS_OpenCL && "Not OpenCL extended instruction");
  if (EntryPoint == OpenCLLIB::printf)
    IsPrintf = true;
  else {
      FuncName = OCLExtOpMap::map(static_cast<OCLExtOpKind>(
        EntryPoint));
  }

  auto BArgs = BC->getArguments();
  transOCLVectorLoadStore(FuncName, BArgs);

  // keep builtin functions written with bool as i1, truncate down if necessary.
  auto Args = transValue(BC->getValues(BArgs), BB->getParent(), BB, BoolAction::Truncate);
  std::vector<Type*> ArgTypes;
  for (auto &v : Args)
  {
      ArgTypes.push_back(v->getType());
  }

  bool IsVarArg = false;
  if (IsPrintf)
  {
      FuncName = "printf";
      IsVarArg = true;
      ArgTypes.resize(1);
  }
  else
  {
      decorateSPIRVExtInst(FuncName, ArgTypes);
  }

  FunctionType *FT = FunctionType::get(
      truncBoolType(BC->getType(), transType(BC->getType())),
      ArgTypes,
      IsVarArg);
  Function *F = M->getFunction(FuncName);
  if (!F) {
    F = Function::Create(FT,
      GlobalValue::ExternalLinkage,
      FuncName,
      M);
    F->setCallingConv(CallingConv::SPIR_FUNC);
    if (isFuncNoUnwind())
      F->addFnAttr(Attribute::NoUnwind);
  }
  CallInst *Call = CallInst::Create(F,
      Args,
      BC->getName(),
      BB);
  setCallingConv(Call);
  Call->addAttribute(AttributeSet::FunctionIndex, Attribute::NoUnwind);
  return Call;
}

// SPIR-V only contains language version. Use OpenCL language version as
// SPIR version.
bool
SPIRVToLLVM::transSourceLanguage() {
  SPIRVWord Ver = 0;
  SpvSourceLanguage Lang = BM->getSourceLanguage(&Ver);
  assert((Lang == SpvSourceLanguageOpenCL_C || Lang == SpvSourceLanguageOpenCL_CPP) && "Unsupported source language");
  unsigned Major = Ver/10;
  unsigned Minor = (Ver%10);
  addOCLVersionMetadata(Context, M, kSPIR2MD::SPIRVer, Major, Minor);
  addOCLVersionMetadata(Context, M, kSPIR2MD::OCLVer, Major, Minor);
  return true;
}

bool
SPIRVToLLVM::transSourceExtension() {
  auto ExtSet = rmap<OclExt::Kind>(BM->getExtension());
  auto CapSet = rmap<OclExt::Kind>(BM->getCapability());
  for (auto &I:CapSet)
    ExtSet.insert(I);
  auto OCLExtensions = getStr(map<std::string>(ExtSet));
  std::string OCLOptionalCoreFeatures;
  bool First = true;
  static const char *OCLOptCoreFeatureNames[] = {
      "cl_images",
      "cl_doubles",
  };
  for (auto &I:OCLOptCoreFeatureNames) {
    size_t Loc = OCLExtensions.find(I);
    if (Loc != std::string::npos) {
      OCLExtensions.erase(Loc, strlen(I));
      if (First)
        First = false;
      else
        OCLOptionalCoreFeatures += ' ';
      OCLOptionalCoreFeatures += I;
    }
  }
  addNamedMetadataString(Context, M, kSPIR2MD::Extensions, OCLExtensions);
  addNamedMetadataString(Context, M, kSPIR2MD::OptFeatures,
      OCLOptionalCoreFeatures);
  return true;
}

bool
SPIRVToLLVM::transCompilerOption() {
  llvm::StringRef flagString = BM->getCompileFlag();
  SmallVector<StringRef, 8> flags;
  StringRef sep(" ");
  flagString.split(flags, sep);

  std::vector<Metadata *> ValueVec;
  for (auto flag : flags) {
    flag = flag.trim();
    if (!flag.empty())
      ValueVec.push_back(MDString::get(*Context, flag));
  }
  NamedMDNode *NamedMD = M->getOrInsertNamedMetadata(SPIR_MD_COMPILER_OPTIONS);
  NamedMD->addOperand(MDNode::get(*Context, ValueVec));
  return true;
}

static void dumpSPIRVBC(const char* fname, const char* data, unsigned int size)
{
    FILE* fp;
    fp = fopen(fname, "wb");
    fwrite(data, 1, size, fp);
    fclose(fp);
}

bool ReadSPIRV(LLVMContext &C, std::istream &IS, Module *&M,
    StringRef options,
    std::string &ErrMsg) {

  std::unique_ptr<SPIRVModule> BM( SPIRVModule::createSPIRVModule() );
  BM->setCompileFlag( options );
  IS >> *BM;
  BM->resolveUnknownStructFields();
  M = new Module( "",C );
  SPIRVToLLVM BTL( M,BM.get() );
  bool Succeed = true;
  if(!BTL.translate()) {
    BM->getError( ErrMsg );
    Succeed = false;
  }

  llvm::legacy::PassManager PM;
  PM.add( new TypesLegalizationPass() );
  PM.run( *M );

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  if (DbgSaveTmpLLVM)
    dumpLLVM(M, DbgTmpLLVMFileName);
#endif	
  if (!Succeed) {
    delete M;
    M = nullptr;
  }
  return Succeed;
}

}
