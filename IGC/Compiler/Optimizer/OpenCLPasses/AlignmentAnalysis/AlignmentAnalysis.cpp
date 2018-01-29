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

#include "Compiler/Optimizer/OpenCLPasses/AlignmentAnalysis/AlignmentAnalysis.hpp"
#include "Compiler/IGCPassSupport.h"
#include "Compiler/CodeGenPublic.h"

#include "common/LLVMWarningsPush.hpp"
#include <llvm/IR/InstIterator.h>
#include <llvm/Support/MathExtras.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include "common/LLVMWarningsPop.hpp"

#include <deque>
#include <set>

using namespace llvm;
using namespace IGC;

// Register pass to igc-opt
#define PASS_FLAG "igc-fix-alignment"
#define PASS_DESCRIPTION "Fix the alignment of loads and stores according to OpenCL rules"
#define PASS_CFG_ONLY false
#define PASS_ANALYSIS false
IGC_INITIALIZE_PASS_BEGIN(AlignmentAnalysis, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)
IGC_INITIALIZE_PASS_END(AlignmentAnalysis, PASS_FLAG, PASS_DESCRIPTION, PASS_CFG_ONLY, PASS_ANALYSIS)

char AlignmentAnalysis::ID = 0;

AlignmentAnalysis::AlignmentAnalysis() : FunctionPass(ID)
{
    initializeAlignmentAnalysisPass(*PassRegistry::getPassRegistry());
}

bool AlignmentAnalysis::runOnFunction(Function &F)
{
    m_DL = &F.getParent()->getDataLayout();

    // The work-list queue for the data flow algorithm
    std::deque<Instruction*> workList;

    // A helper set, to avoid inserting the same instruction
    // into the worklist several times. (this is the set of "grey" nodes).
    // We could instead perform lookups directly on the worklist, but this
    // is faster.
    std::set<Instruction*> inList;

    // First, initialize the worklist with all of the instructions in the function.
    for( llvm::inst_iterator inst = inst_begin(F), instEnd = inst_end(F); inst != instEnd; ++inst )
    {
        workList.push_back(&*inst);
        inList.insert(&*inst);
    }

    // It is more efficient to handle the earlier instructions first, 
    // so we pop from the front.
    while( !inList.empty() )
    {
        // Get the next instruction
        Instruction* inst = workList.front();
        workList.pop_front();
        inList.erase(inst);

        // Get the alignment of this instruction
        if( processInstruction(inst) )
        {
            // The alignment of inst changed, (re-)process all users, by
            // adding them to the end of the queue.
            for( auto user = inst->user_begin(), userEnd = inst->user_end(); user != userEnd; ++user )
            {
                Instruction* userInst = cast<Instruction>(*user);
                // If the user is already in the queue, no need to add it again
                if( inList.find(userInst) == inList.end() )
                {
                    workList.push_back(userInst);
                    inList.insert(userInst);
                }
            }
        }
    }

    return true;
}

bool AlignmentAnalysis::processInstruction(llvm::Instruction* I)
{
    // Get the currently known alignment of I.
    unsigned int currAlign = getAlignValue(I);
    
    // Compute the instruction's alignment 
    // using the alignment of the arguments.
    unsigned int newAlign = visit(I);

    // The new alignment may not be better than the current one,
    // since we're only allowed to go in one direction in the lattice.
    newAlign = iSTD::Min(currAlign, newAlign);

    // If the alignment changed, we want to process the users of this
    // value, so return true. Otherwise, this instruction has stabilized
    // (for now).

    if( newAlign != currAlign )
    {
        m_alignmentMap[I] = newAlign;
        return true;
    }

    return false;
}

unsigned int AlignmentAnalysis::visitInstruction(Instruction &I)
{
    // The safe thing to do for unknown instructions is to return 1.
    return MinimumAlignment;
}

unsigned int AlignmentAnalysis::visitAllocaInst(AllocaInst &I)
{
    // Return the alignment of the alloca, which ought to be correct
    unsigned int newAlign = I.getAlignment();
    
    // If the alloca uses the default alignment, pull it from the datalayout
    if( !newAlign )
    {
        Type* allocaType = I.getType();
        newAlign = m_DL->getABITypeAlignment(allocaType->getPointerElementType());
    }

    return newAlign;
}

unsigned int AlignmentAnalysis::visitSelectInst(SelectInst &I)
{
    Value* srcTrue = I.getTrueValue();
    Value* srcFalse = I.getFalseValue();

    // In general this should be the GCD, but because we assume we are always aligned on
    // powers of 2, the GCD is the minimum.
    return iSTD::Min(getAlignValue(srcTrue), getAlignValue(srcFalse));
}

unsigned int AlignmentAnalysis::visitPHINode(PHINode &I)
{
    unsigned int newAlign = Value::MaximumAlignment;

    // The alignment of a PHI is the minimal alignment of any of the
    // incoming values.
    unsigned numVals = I.getNumIncomingValues();
    for( unsigned int i = 0; i < numVals; ++i )
    {
        Value* op = I.getIncomingValue(i);
        newAlign = iSTD::Min(newAlign, getAlignValue(op));
    }

    return newAlign;
}

unsigned int AlignmentAnalysis::visitLoadInst(LoadInst &I)
{
    // Set the align attribute of the load according to the detected
    // alignment of its operand.
    I.setAlignment(iSTD::Max(I.getAlignment(), getAlignValue(I.getPointerOperand())));

    // We don't know anything about loaded values.
    return MinimumAlignment;
}

unsigned int AlignmentAnalysis::visitStoreInst(StoreInst &I)
{
    // Set the align attribute of the store according to the detected
    // alignment of its operand.
    I.setAlignment(iSTD::Max(I.getAlignment(), getAlignValue(I.getPointerOperand())));

    // Stores don't have any users, so it doesn't matter what we return.
    return MinimumAlignment;
}

unsigned int AlignmentAnalysis::visitAdd(BinaryOperator &I)
{
    // Addition can not improve the alignment.
    // In a more precise analysis, it could (e.g. 3 + 1 = 4)
    // but here, we only keep track of the highest power of 2
    // factor.
    // So, the best case scenario is that the alignment stays
    // the same if you add two values with the same alignment.
    // Note that it can not grow even in this case, because
    // we keep an underapproximation. That is:
    // If we know x and y were divisible by 4 but *not* 8,
    // then x + y would be divisible by 8. However, if x is divisible by 4
    // and y is divisible by 8, then x + y is only divisible by 4.
    Value* op0 = I.getOperand(0);
    Value* op1 = I.getOperand(1);

    return iSTD::Min(getAlignValue(op0), getAlignValue(op1));
}

unsigned int AlignmentAnalysis::visitMul(BinaryOperator &I)
{
    // Because we are dealing with powers of 2, 
    // align(x * y) = align(x) * align(y)
    Value* op0 = I.getOperand(0);
    Value* op1 = I.getOperand(1);

    return iSTD::Min(Value::MaximumAlignment, 
                     getAlignValue(op0) * getAlignValue(op1));
}

unsigned int AlignmentAnalysis::visitShl(BinaryOperator &I)
{
    // If we are shifting left by a constant, we know the
    // alignment improves according to that value.
    // In any case, it can not drop.
    Value* op0 = I.getOperand(0);
    Value* op1 = I.getOperand(1);

    if (ConstantInt* constOp1 = dyn_cast<ConstantInt>(op1))
    {
        return iSTD::Min(Value::MaximumAlignment,
            getAlignValue(op0) << constOp1->getZExtValue());
    }
    else
    {
        return getAlignValue(op0);
    }
}

unsigned int AlignmentAnalysis::visitAnd(BinaryOperator &I)
{
    Value* op0 = I.getOperand(0);
    Value* op1 = I.getOperand(1);

    // If one of the operands has trailing zeroes up to some point,
    // then so will the result. So, the alignment is at least the maximum
    // of the operands.
    return iSTD::Max(getAlignValue(op0),
        getAlignValue(op1));
}

unsigned int AlignmentAnalysis::visitGetElementPtrInst(GetElementPtrInst &I)
{
    // The alignment can never be better than the alignment of the base pointer
    unsigned int newAlign = getAlignValue(I.getPointerOperand());

    Type *Ty = I.getPointerOperand()->getType()->getScalarType();
    gep_type_iterator GTI = gep_type_begin(&I);
    for (auto op = I.op_begin() + 1, opE = I.op_end(); op != opE; ++op, ++GTI)
    {
        unsigned int offset = 0;
        if (StructType *StTy = GTI.getStructTypeOrNull())
        {
            unsigned int Field = int_cast<unsigned int>((cast<Constant>(*op))->getUniqueInteger().getZExtValue());
            offset = int_cast<unsigned int>(m_DL->getStructLayout(StTy)->getElementOffset(Field));
            Ty = StTy->getElementType(Field);
        }
        else
        {
            Ty = GTI.getIndexedType();
            unsigned int multiplier = int_cast<unsigned int>(m_DL->getTypeAllocSize(Ty));
            offset = multiplier * getAlignValue(*op);
        }

        // It's possible offset is not a power of 2, because struct fields
        // may be aligned on all sorts of weird values. So we can not just
        // take the minimum between newAlign and offset, we need the 
        // highest power of 2 that divides both.

        // x | y has trailing 0s exactly where both x and y have trailing 0s.
        newAlign = getConstantAlignment(newAlign | offset);
    }

    return newAlign;
}

// Casts don't change the alignment.
// Technically we could do better (a trunc or an extend may improve alignment)
// but this doesn't seem important enough.
unsigned int AlignmentAnalysis::visitBitCastInst(BitCastInst &I)
{
    return getAlignValue(I.getOperand(0));
}

unsigned int AlignmentAnalysis::visitPtrToIntInst(PtrToIntInst &I)
{
    return getAlignValue(I.getOperand(0));
}

unsigned int AlignmentAnalysis::visitIntToPtrInst(IntToPtrInst &I)
{
    return getAlignValue(I.getOperand(0));
}

unsigned int AlignmentAnalysis::visitTruncInst(TruncInst &I)
{
    return getAlignValue(I.getOperand(0));
}

unsigned int AlignmentAnalysis::visitZExtInst(ZExtInst &I)
{
    return getAlignValue(I.getOperand(0));
}

unsigned int AlignmentAnalysis::visitSExtInst(SExtInst &I)
{
    return getAlignValue(I.getOperand(0));
}

unsigned int AlignmentAnalysis::visitCallInst(CallInst &I)
{
    // Handle alignment for memcpy and memset
    Function* callee = I.getCalledFunction();
    // Skip indirect call!
    if (!callee)
        // return value does not matter
        return MinimumAlignment;

    StringRef calleeName = callee->getName();
    IntrinsicInst *Intri = dyn_cast<IntrinsicInst>(&I);
    llvm::Intrinsic::ID ID = llvm::Intrinsic::ID::not_intrinsic;

    if (Intri)
        ID = Intri->getIntrinsicID();

    if ((Intri && ID == Intrinsic::memcpy) ||
        (!Intri && calleeName.startswith("__builtin_IB_memcpy_")))
    {
        unsigned int origAlign = MinimumAlignment;
        if (ConstantInt *CI = dyn_cast<ConstantInt>(I.getArgOperand(3)))
        {
            origAlign = (unsigned int)CI->getZExtValue();
        }
        unsigned int dstAlign   = getAlignValue(I.getArgOperand(0));
        unsigned int srcAlign   = getAlignValue(I.getArgOperand(1));

        // Need to get the Min of dest alignment and src alignment
        unsigned int minAlign = iSTD::Min(dstAlign, srcAlign);

        // As the original intrinsic's alignemnt is the minimum one, use
        // max to get a better alignment.
        if (minAlign > origAlign)
        {
            I.setArgOperand(3, ConstantInt::get(I.getArgOperand(3)->getType(), minAlign));
        }

        // return value does not matter
        return MinimumAlignment;
    }

    if (Intri && ID == Intrinsic::memset)
    {
        unsigned int origAlign = MinimumAlignment;
        if (ConstantInt *CI = dyn_cast<ConstantInt>(I.getArgOperand(3)))
        {
            origAlign = (unsigned int)CI->getZExtValue();
        }
        unsigned int dstAlign = getAlignValue(I.getArgOperand(0));
 
        // As the original intrinsic's alignemnt is the minimum one, use
        // max to get a better alignment.
        if (dstAlign > origAlign)
        {
            I.setArgOperand(3, ConstantInt::get(I.getArgOperand(3)->getType(), dstAlign));
        }

        // return value does not matter
        return MinimumAlignment; 
    }

    return MinimumAlignment;
}

unsigned int AlignmentAnalysis::getAlignValue(Value *V) const
{
    if( Instruction* inst = dyn_cast<Instruction>(V) )
    {
        auto iter = m_alignmentMap.find(V);
        if( iter == m_alignmentMap.end() )
        {
            // Instructions are initialize to maximum alignment 
            // (this is the "top" value)
            return Value::MaximumAlignment;
        }

        return iter->second;
    }
    else if( Constant* con = dyn_cast<Constant>(V) )
    {
        if( ConstantInt* constInt = dyn_cast<ConstantInt>(V) )
        {
            return getConstantAlignment(constInt->getZExtValue());
        }
        else if( GlobalVariable* GV = dyn_cast<GlobalVariable>(V) )
        {
            unsigned int align = GV->getAlignment();
    
            // If the globalvariable uses the default alignment, pull it from the datalayout
            if( !align )
            {
                Type* gvType = GV->getType();
                return m_DL->getABITypeAlignment(gvType->getPointerElementType());
            }
            else
            {
                return align;
            }
        }

        // Not an int or a globalvariable, be pessimistic.
        return MinimumAlignment;
    }
    else if( Argument* arg = dyn_cast<Argument>(V) )
    {
        if( arg->getType()->isPointerTy() )
        {
            // Pointer arguments are guaranteed to be aligned on the ABI alignment
            Type* pointedTo = arg->getType()->getPointerElementType();
            if( pointedTo->isSized() )
            {
                return m_DL->getABITypeAlignment(pointedTo);
            }
            else
            {
                // We have some pointer-to-opaque-types which are not real pointers -
                // this is used to pass things like images around.
                // Apparently, DataLayout being asked about the ABI alignment of opaque types.
                // So, we don't.
                return MinimumAlignment;
            }
        }
        else
        {
            // We don't know anything about integer arguments.
            return MinimumAlignment;
        }
    }

    // Be pessimistic
    return MinimumAlignment;
}

unsigned int AlignmentAnalysis::getConstantAlignment(uint64_t C) const
{
    if (!C)
    {
        return Value::MaximumAlignment;
    }

    return iSTD::Min(Value::MaximumAlignment, 1U << llvm::countTrailingZeros(C));
}
