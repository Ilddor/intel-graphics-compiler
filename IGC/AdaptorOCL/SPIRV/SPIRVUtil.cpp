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
//===- SPIRVUtil.cpp -  SPIR-V Utilities -------------------------*- C++ -*-===//
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
/// This file defines utility classes and functions shared by SPIR-V
/// reader/writer.
///
//===----------------------------------------------------------------------===//

#include "SPIRVInternal.h"
#include "Mangler/ParameterType.h"

#include "common/LLVMWarningsPush.hpp"
#include "llvm/ADT/StringExtras.h"
#include "common/LLVMWarningsPop.hpp"

namespace spv{

void
saveLLVMModule(Module *M, const std::string &OutputFile) {
  std::error_code EC;
  tool_output_file Out(OutputFile.c_str(), EC, sys::fs::F_None);
  if (EC) {
    spirv_assert(0 && "Failed to open file");
    return;
  }

  WriteBitcodeToFile(M, Out.os());
  Out.keep();
}

PointerType*
getOrCreateOpaquePtrType(Module *M, const std::string &Name,
    unsigned AddrSpace) {
  auto OpaqueType = M->getTypeByName(Name);
  if (!OpaqueType)
    OpaqueType = StructType::create(M->getContext(), Name);
  return PointerType::get(OpaqueType, AddrSpace);
}

void
getFunctionTypeParameterTypes(llvm::FunctionType* FT,
    std::vector<Type*>& ArgTys) {
  for (auto I = FT->param_begin(), E = FT->param_end(); I != E; ++I) {
    ArgTys.push_back(*I);
  }
}

Function *
getOrCreateFunction(Module *M, Type *RetTy, ArrayRef<Type *> ArgTypes,
    StringRef Name, bool builtin, AttributeSet *Attrs, bool takeName) {
  std::string FuncName(Name);
  if (builtin)
     decorateSPIRVBuiltin(FuncName, ArgTypes);

  FunctionType *FT = FunctionType::get(
      RetTy,
      ArgTypes,
      false);
  Function *F = M->getFunction(FuncName);
  if (!F || F->getFunctionType() != FT) {
    auto NewF = Function::Create(FT,
      GlobalValue::ExternalLinkage,
      FuncName,
      M);
    if (F && takeName)
      NewF->takeName(F);
    F = NewF;
    F->setCallingConv(CallingConv::SPIR_FUNC);
    if (Attrs)
      F->setAttributes(*Attrs);
  }
  return F;
}

std::vector<Value *>
getArguments(CallInst* CI) {
  std::vector<Value*> Args;
  for (unsigned I = 0, E = CI->getNumArgOperands(); I != E; ++I) {
    Args.push_back(CI->getArgOperand(I));
  }
  return Args;
}

std::string recursive_mangle(const Type* pType)
{
    Type::TypeID ID = pType->getTypeID();

    switch (ID)
    {
        case Type::FloatTyID:
            return "f32";
        case Type::DoubleTyID:
            return "f64";
        case Type::HalfTyID:
            return "f16";
        case Type::IntegerTyID:
            return "i" + utostr(pType->getIntegerBitWidth());
        case Type::VectorTyID:
        {
            unsigned int vecLen = pType->getVectorNumElements();
            Type* pEltType = pType->getVectorElementType();
            return "v" + utostr(vecLen) + recursive_mangle(pEltType);
        }
        case Type::PointerTyID:
        {
            unsigned int AS = pType->getPointerAddressSpace();
            Type* pPointeeType = pType->getPointerElementType();

            if (isa<StructType>(pPointeeType) && cast<StructType>(pPointeeType)->isOpaque())
            {
                return "i64";
            }

            return "p" + utostr(AS) + recursive_mangle(pPointeeType);
        }
        case Type::StructTyID:
        {
            auto structName = cast<StructType>(pType)->getName();
            auto pointPos = structName.rfind('.');
            return pointPos != structName.npos ? structName.substr(pointPos + 1) : structName;
        }
        case Type::FunctionTyID:
        {
            return "func";
        }
        case Type::ArrayTyID:
        {
            auto elemType = pType->getArrayElementType();
            auto numElems = pType->getArrayNumElements();
            return "a" + utostr(numElems) + recursive_mangle(elemType);
        }
        default:
            spirv_assert(0 && "unhandled type to mangle!");
            return "";
    }
}


std::string Mangler(const std::string &FuncName, const std::vector<Type*> &ArgTypes)
{
    std::string str_type;
    for (auto U : ArgTypes)
    {
        str_type += "_" + recursive_mangle(U);
    }
    return FuncName + str_type;
}

void
decorateSPIRVBuiltin(std::string &S)
{
    S = std::string(kLLVMName::builtinPrefix) + S;
}

void
decorateSPIRVBuiltin(std::string &S, std::vector<Type*> ArgTypes) {
   S.assign(std::string(kLLVMName::builtinPrefix) + Mangler(S,ArgTypes));
}

void
decorateSPIRVExtInst(std::string &S, std::vector<Type*> ArgTypes) {
   S.assign(std::string(kLLVMName::builtinExtInstPrefixOpenCL) + Mangler(S,ArgTypes));
}

bool
isFunctionBuiltin(llvm::Function* F) {
  return F && F->isDeclaration() && F->getName().startswith(kLLVMName::builtinPrefix);
}

std::string
getSPIRVBuiltinName(Op OC, std::vector<Type*> ArgTypes, std::string suffix) {
   std::string name;
   if (OCLSPIRVBuiltinMap::find(OC, &name)){
       name = name + suffix;
       decorateSPIRVBuiltin(name, ArgTypes);
   }
   else
   {
       spirv_assert(0 && "Couldn't find opcode in map!");
   }
   return name;
}

CallInst *
mutateCallInst(Module *M, CallInst *CI,
    std::function<std::string (CallInst *, std::vector<Value *> &)>ArgMutate,
    bool Mangle, AttributeSet *Attrs, bool TakeFuncName) {

  auto Args = getArguments(CI);
  auto NewName = ArgMutate(CI, Args);
  StringRef InstName;
  if (!CI->getType()->isVoidTy() && CI->hasName()) {
    InstName = CI->getName();
    CI->setName(InstName + ".old");
  }
  auto NewCI = addCallInst(M, NewName, CI->getType(), Args, Attrs, CI, Mangle,
      InstName, TakeFuncName);
  CI->replaceAllUsesWith(NewCI);
  CI->dropAllReferences();
  CI->removeFromParent();
  return NewCI;
}

void
mutateFunction(Function *F,
    std::function<std::string (CallInst *, std::vector<Value *> &)>ArgMutate,
    bool Mangle, AttributeSet *Attrs, bool TakeFuncName) {
  auto M = F->getParent();
  for (auto I = F->user_begin(), E = F->user_end(); I != E;) {
    if (auto CI = dyn_cast<CallInst>(*I++))
      mutateCallInst(M, CI, ArgMutate, Mangle, Attrs, TakeFuncName);
  }
  if (F->use_empty()) {
    F->dropAllReferences();
    F->removeFromParent();
  }
}

CallInst *
addCallInst(Module *M, StringRef FuncName, Type *RetTy, ArrayRef<Value *> Args,
    AttributeSet *Attrs, Instruction *Pos, bool Mangle, StringRef InstName,
    bool TakeFuncName) {
  auto F = getOrCreateFunction(M, RetTy, getTypes(Args),
      FuncName, Mangle, Attrs, TakeFuncName);
  auto CI = CallInst::Create(F, Args, InstName, Pos);
  CI->setCallingConv(F->getCallingConv());
  return CI;
}

ConstantInt *
getInt64(Module *M, int64_t value) {
  return ConstantInt::get(Type::getInt64Ty(M->getContext()), value, true);
}

ConstantInt *
getInt32(Module *M, int value) {
  return ConstantInt::get(Type::getInt32Ty(M->getContext()), value, true);
}

}

