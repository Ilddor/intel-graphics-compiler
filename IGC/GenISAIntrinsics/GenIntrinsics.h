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

#pragma once

#include "common/LLVMWarningsPush.hpp"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Function.h"
#include "common/LLVMWarningsPop.hpp"

namespace llvm {

namespace GenISAIntrinsic {
  enum ID : unsigned {
    no_intrinsic = Intrinsic::num_intrinsics,
#define GET_INTRINSIC_ENUM_VALUES
#include "IntrinsicGenISA.gen"
#undef GET_INTRINSIC_ENUM_VALUES
    num_genisa_intrinsics
  };

  /// Intrinsic::getName(ID) - Return the LLVM name for an intrinsic, such as
  /// "llvm.ppc.altivec.lvx".
  std::string getName(ID id, ArrayRef<Type*> Tys = None);

  
  /// Intrinsic::getDeclaration(M, ID) - Create or insert an LLVM Function
  /// declaration for an intrinsic, and return it.
  ///
  /// The Tys parameter is for intrinsics with overloaded types (e.g., those
  /// using iAny, fAny, vAny, or iPTRAny).  For a declaration of an overloaded
  /// intrinsic, Tys must provide exactly one type for each overloaded type in
  /// the intrinsic.
#if defined(ANDROID) || defined(__linux__)
  __attribute__ ((visibility ("default"))) Function *getDeclaration(Module *M, ID id, ArrayRef<Type*> Tys = None);
#else
  Function *getDeclaration(Module *M, ID id, ArrayRef<Type*> Tys = None);
#endif
  AttributeSet getGenIntrinsicAttributes(LLVMContext& C, GenISAIntrinsic::ID id);
 
  //Override of isIntrinsic method defined in Function.h
  inline bool isIntrinsic(const Function *CF)
  {
      return (CF->getName().startswith("genx"));
  }
  
  ID getIntrinsicID(const Function *F);
  
} // namespace GenISAIntrinsic

} // namespace llvm

