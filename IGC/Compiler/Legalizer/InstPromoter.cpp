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

// vim:ts=2:sw=2:et:

#define DEBUG_TYPE "type-legalizer"
#include "TypeLegalizer.h"
#include "InstPromoter.h"
#include "common/LLVMWarningsPush.hpp"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "common/LLVMWarningsPop.hpp"
using namespace llvm;
using namespace IGC::Legalizer;

bool InstPromoter::promote(Instruction *I) {
  IRB->SetInsertPoint(&(*std::next(BasicBlock::iterator(I))));
  IRB->SetCurrentDebugLocation(I->getDebugLoc());
  Promoted = nullptr;

  if (!visit(*I))
    return false;

  if (Promoted)
  {
      TL->setLegalizedValues(I, Promoted);
      // In the case where we've only legalized the arguments to the instruction,
      // we can go ahead and replace it with the new promoted instruction since
      // the return types are the same.
      if (I->getType() == Promoted->getType())
      {
          I->replaceAllUsesWith(Promoted);
      }
  }

  return true;
}

// By default, capture all missing instructions!
bool InstPromoter::visitInstruction(Instruction &I) {
  DEBUG(dbgs() << "PROMOTE: " << I << '\n');
  llvm_unreachable("UNKNOWN INSTRUCTION IS BEING PROMOTED!");
  return false;
}

/// Terminator instructions
///

bool InstPromoter::visitTerminatorInst(TerminatorInst &I) {
  // All terminators are handled specially.
  return false;
}

bool InstPromoter::visitSelectInst (SelectInst &I) {

	ValueSeq *Ops0, *Ops1;
	std::tie(Ops0, std::ignore) = TL->getLegalizedValues(I.getOperand(1), true);
	assert(Ops0->size() == 1);
	// we should get value from Ops0 here, as next getLegalizedValues call may grow ValueMap object 
	// when inserting new pair with ValueMap.insert(e.g.when ValueMap.NumBuckets grows from 64 to 128) 
	// and previously received ValueSeq objects will become invalid.
	Value *LHS = Ops0->front();

	std::tie(Ops1, std::ignore) = TL->getLegalizedValues(I.getOperand(2), true);
	assert(Ops1->size() == 1);
	Value *RHS = Ops1->front();

	Promoted = IRB->CreateSelect(I.getOperand(0), LHS, RHS,
		Twine(I.getName(), getSuffix()));

	return true;
}

bool InstPromoter::visitICmpInst(ICmpInst &I)
{
    bool isSigned = I.isSigned();

    ValueSeq *Ops0, *Ops1;
    std::tie(Ops0, std::ignore) = TL->getLegalizedValues(I.getOperand(0), isSigned);
    assert(Ops0->size() == 1);
    // we should get value from Ops0 here, as next getLegalizedValues call may grow ValueMap object 
    // when inserting new pair with ValueMap.insert(e.g.when ValueMap.NumBuckets grows from 64 to 128) 
    // and previously received ValueSeq objects will become invalid.
    Value *LHS = Ops0->front();

    std::tie(Ops1, std::ignore) = TL->getLegalizedValues(I.getOperand(1), isSigned);
    assert(Ops1->size() == 1);
    Value *RHS = Ops1->front();

    unsigned initialWidth = I.getOperand(0)->getType()->getIntegerBitWidth();
    unsigned finalWidth   = LHS->getType()->getIntegerBitWidth();

    if (!isSigned)
    {
        LHS = IRB->CreateAnd(LHS, (1 << initialWidth) - 1);
        RHS = IRB->CreateAnd(RHS, (1 << initialWidth) - 1);
    }
    else
    {
        assert(finalWidth >= initialWidth);

        unsigned shiftAmt = finalWidth - initialWidth;

        LHS = IRB->CreateShl(LHS, shiftAmt);
        LHS = IRB->CreateAShr(LHS, shiftAmt);
        RHS = IRB->CreateShl(RHS, shiftAmt);
        RHS = IRB->CreateAShr(RHS, shiftAmt);
    }

    Promoted = IRB->CreateICmp(I.getPredicate(), LHS, RHS,
        Twine(I.getName(), getSuffix()));

    return true;
}

/// Standard binary operators
///

bool InstPromoter::visitBinaryOperator(BinaryOperator &I) {
  ValueSeq *Ops0, *Ops1;
  std::tie(Ops0, std::ignore) = TL->getLegalizedValues(I.getOperand(0));
  assert(Ops0->size() == 1);
  // we should get value from Ops0 here, as next getLegalizedValues call may grow ValueMap object 
  // when inserting new pair with ValueMap.insert(e.g.when ValueMap.NumBuckets grows from 64 to 128) 
  // and previously received ValueSeq objects will become invalid.
  Value *LHS = Ops0->front();

  std::tie(Ops1, std::ignore) = TL->getLegalizedValues(I.getOperand(1));
  assert(Ops1->size() == 1);
  Value *RHS = Ops1->front();

  switch (I.getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
    break;
  case Instruction::UDiv:
  case Instruction::URem:
    std::tie(LHS, RHS) = TL->zext(LHS, RHS, I.getType());
    break;
  case Instruction::SDiv:
  case Instruction::SRem:
    std::tie(LHS, RHS) = TL->sext(LHS, RHS, I.getType());
    break;
  case Instruction::Shl:
    RHS = TL->zext(RHS, I.getType());
    break;
  case Instruction::LShr:
    std::tie(LHS, RHS) = TL->zext(LHS, RHS, I.getType());
    break;
  case Instruction::AShr:
    LHS = TL->sext(LHS, I.getType());
    RHS = TL->zext(RHS, I.getType());
    break;
  default:
    llvm_unreachable("UNKNOWN BINARY OPERATOR IS BEING PROMOTED!");
  }

  Promoted =
    TL->createBinOpAsGiven(&I, LHS, RHS, Twine(I.getName(), getSuffix()));

  return true;
}

/// Memory operators
///

bool InstPromoter::visitLoadInst(LoadInst &I) {
  Type *OrigTy = I.getType();

  TypeSeq *TySeq;
  std::tie(TySeq, std::ignore) = TL->getLegalizedTypes(OrigTy);
  assert(TySeq->size() == 1);

  unsigned AS = I.getPointerAddressSpace();

  Value *OldPtr = I.getPointerOperand();
  Type *PromotedTy = TySeq->front();

  Value *NewBasePtr =
    IRB->CreatePointerCast(OldPtr, IRB->getInt8PtrTy(AS),
                           Twine(OldPtr->getName(), ".ptrcast"));

  // Different from promotion of regular instructions, such as 'add', promotion
  // of load is required to split the original load into small ones and
  // concatenate them together, e.g. i56 needs splitting into loads of i32,
  // i16, and i8. It's because, without alignment checking, out-of-bound load
  // may be generated if the promoted type is used directly.

  Value *PromotedVal = Constant::getNullValue(PromotedTy);
  unsigned Off = 0;
  unsigned Part = 0;
  for (unsigned TotalLoadBits = TL->getTypeStoreSizeInBits(OrigTy),
                ActualLoadBits = 0; TotalLoadBits != 0;
                TotalLoadBits -= ActualLoadBits) {
    // Get the largest integer type but not bigger than total load bits.
    ActualLoadBits = TL->getLargestLegalIntTypeSize(TotalLoadBits);

    Type *NewTy = TL->getIntNTy(ActualLoadBits);
    Type *NewPtrTy = PointerType::get(NewTy, AS);

    Value *NewPtr =
      TL->getPointerToElt(NewBasePtr, Off, NewPtrTy,
          Twine(NewBasePtr->getName(), ".off") + Twine(Off));

    LoadInst *NewLd =
      IRB->CreateLoad(NewPtr, Twine(I.getName(), getSuffix()) + Twine(Part));
    TL->dupMemoryAttribute(NewLd, &I, Off);

    Value *NewVal =
      IRB->CreateZExt(NewLd, PromotedTy, Twine(NewLd->getName(), ".zext"));
    NewVal = TL->shl(NewVal, Off << 3);
    PromotedVal =
      IRB->CreateOr(PromotedVal, NewVal, Twine(NewVal->getName(), ".concat"));

    assert((ActualLoadBits & 0x7) == 0 &&
           "LEGAL INTEGER TYPE IS NOT BYTE ADDRESSABLE!");
    Off += ActualLoadBits >> 3;
    ++Part;
  }

  Promoted = PromotedVal;
  return true;
}

bool InstPromoter::visitStoreInst(StoreInst &I) {
  Value *OrigVal = I.getValueOperand();
  Type *OrigTy = OrigVal->getType();

  ValueSeq *ValSeq;
  std::tie(ValSeq, std::ignore) = TL->getLegalizedValues(OrigVal);
  assert(ValSeq->size() == 1);

  unsigned AS = I.getPointerAddressSpace();

  Value *OldPtr = I.getPointerOperand();
  Value *PromotedVal = ValSeq->front();

  Value *NewBasePtr =
    IRB->CreatePointerCast(OldPtr, IRB->getInt8PtrTy(AS),
                           Twine(OldPtr->getName(), ".ptrcast"));

  unsigned Off = 0;
  for (unsigned TotalStoreBits = TL->getTypeStoreSizeInBits(OrigTy),
                ActualStoreBits = 0; TotalStoreBits != 0;
                TotalStoreBits -= ActualStoreBits) {

    // Get the largest integer type but not bigger than total store bits.
    ActualStoreBits = TL->getLargestLegalIntTypeSize(TotalStoreBits);

    Type *NewTy = TL->getIntNTy(ActualStoreBits);
    Type *NewPtrTy = PointerType::get(NewTy, AS);

    Value *NewPtr =
      TL->getPointerToElt(NewBasePtr, Off, NewPtrTy,
          Twine(NewBasePtr->getName(), ".off") + Twine(Off));

    Value *NewVal = TL->lshr(PromotedVal, Off << 3);
    NewVal =
      IRB->CreateTrunc(NewVal, NewTy, Twine(NewVal->getName(), ".trunc"));

    StoreInst *NewSt = IRB->CreateStore(NewVal, NewPtr);
    TL->dupMemoryAttribute(NewSt, &I, Off);

    assert((ActualStoreBits & 0x7) == 0 &&
           "LEGAL INTEGER TYPE IS NOT BYTE ADDRESSABLE!");
    Off += ActualStoreBits >> 3;
  }

  return true;
}

/// Cast operators

bool InstPromoter::visitTruncInst(TruncInst &I) {
  ValueSeq *ValSeq; LegalizeAction ValAct;
  std::tie(ValSeq, ValAct) = TL->getLegalizedValues(I.getOperand(0));

  Value *Val = I.getOperand(0);
  if (ValAct != Legal)
    Val = ValSeq->front();

  TypeSeq *TySeq; LegalizeAction Act;
  std::tie(TySeq, Act) = TL->getLegalizedTypes(I.getDestTy());
  assert(Act == Legal || Act == Promote);

  if (Act == Legal) {
    if (Val->getType() == I.getType())
      I.replaceAllUsesWith(Val);
    else
      I.setOperand(0, Val);

    return true;
  }

  assert(TySeq->size() == 1);

  Type *PromotedTy = TySeq->front();

  assert(cast<IntegerType>(PromotedTy)->getBitWidth() <=
         cast<IntegerType>(Val->getType())->getBitWidth());

  Promoted =
    IRB->CreateTrunc(Val, PromotedTy, Twine(I.getName(), getSuffix()));

  return true;
}

bool InstPromoter::visitZExtInst(ZExtInst &I) {
  ValueSeq *ValSeq; LegalizeAction ValAct;
  std::tie(ValSeq, ValAct) = TL->getLegalizedValues(I.getOperand(0));

  Value *Val = I.getOperand(0);
  if (ValAct != Legal)
    Val = ValSeq->front();

  TypeSeq *TySeq; LegalizeAction Act;
  std::tie(TySeq, Act) = TL->getLegalizedTypes(I.getDestTy());
  assert(Act == Legal || Act == Promote);

  if (Act == Legal) {
    // Reset insert position as we don't create a new instruction from the
    // original one due to the legal return type.
    IRB->SetInsertPoint(&I);

    if (ValAct != Legal)
      Val = TL->zext(Val, I.getSrcTy());

    if (Val->getType() == I.getType())
      I.replaceAllUsesWith(Val);
    else
      I.setOperand(0, Val);

    return true;
  }

  assert(TySeq->size() == 1);

  Type *PromotedTy = TySeq->front();

  assert(cast<IntegerType>(PromotedTy)->getBitWidth() >=
         cast<IntegerType>(Val->getType())->getBitWidth());

  if (ValAct != Legal)
    Val = TL->zext(Val, I.getSrcTy());

  Promoted =
    IRB->CreateZExt(Val, PromotedTy, Twine(I.getName(), getSuffix()));

  return true;
}

/// Other operators

bool InstPromoter::visitGenIntrinsicInst(GenIntrinsicInst &I) {
  llvm_unreachable("UNKNOWN GEN INSTRINSIC INSTRUCTION IS BEING PROMOTED!");
  return false;
}

bool InstPromoter::visitCallInst(CallInst &I) {
  if (isa<GenIntrinsicInst>(&I))
    return visitGenIntrinsicInst(static_cast<GenIntrinsicInst&>(I));
  llvm_unreachable("NOT IMPLEMENTED YET!");
  return false;
}
