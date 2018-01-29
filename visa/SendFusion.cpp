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

#include "SendFusion.h"
#include "BuildIR.h"
#include "Gen4_IR.hpp"

#include <map>
#include <algorithm>

using namespace std;
using namespace vISA;

namespace vISA
{
	class SendFusion
	{
	private:
        enum {
            // Control if one pair of two sends should be considered for
            // fusion. If their distance (#instructions) is greater than
            // SEND_FUSION_MAX_SPAN, it will not be fused.
            SEND_FUSION_MAX_SPAN = 40
        };

        FlowGraph* CFG;
        IR_Builder *Builder;
        Mem_Manager* MMgr;

        // BB that is being processed now
        G4_BB* CurrBB;

        // Dispatch mask for shader entry (1 Type_UD), One per shader.
        G4_VarBase* DMaskUD;

        // For each BB, if sr0.2 is modified, save it in this map
        std::map<G4_BB*, G4_INST*> LastSR0ModInstPerBB;

        // Flag var used within each BB (1 Type_UW).
        G4_INST* FlagDefPerBB;

        // If this optimization does any change to the code, set it to true.
        bool changed;

        // Check if this instruction is a candidate, might do simplification.
        // Return true if it is an candidate for send fusion.
        bool simplifyAndCheckCandidate(INST_LIST_ITER Iter);
        bool canFusion(INST_LIST_ITER It0, INST_LIST_ITER It1);
        void doFusion(
            INST_LIST_ITER IT0, INST_LIST_ITER IT1, INST_LIST_ITER InsertBeforePos);

        // Common function for canSink()/canHoist(). It checks if
        // StartIT can sink to EndIT or EndIT can hoist to StartIT
        // based on whether isForward is true or false.
        bool canMoveOver(
            INST_LIST_ITER StartIT, INST_LIST_ITER EndIT, bool isForward);

        // Return true if IT can be sinked to sinkToIT.
        bool canSink(INST_LIST_ITER IT, INST_LIST_ITER sinkBeforeIT) {
            return canMoveOver(IT, sinkBeforeIT, true);
        }

        // Return true if IT can be hoisted to hostToIT.
        bool canHoist(INST_LIST_ITER IT, INST_LIST_ITER hoistAfterIT) {
            return canMoveOver(hoistAfterIT, IT, false);
        }

        void initDMaskModInfo();
        void createDMask(INST_LIST* InsertList, INST_LIST_ITER InsertBeforePos);
        void createFlagPerBB(INST_LIST* InsertList, INST_LIST_ITER InsertBeforePos);

        void packPayload(
            G4_INST* FusedSend, G4_INST* Send0, G4_INST* Send1,
            INST_LIST& InsertList, INST_LIST_ITER InsertBeforePos);

        void unpackPayload(
            G4_INST* FusedSend, G4_INST* Send0, G4_INST* Send1,
            INST_LIST& InsertList, INST_LIST_ITER InsertBeforePos);

        G4_VarBase* getVarBase(G4_VarBase* RegVar, G4_Type Ty);
        uint32_t getFuncCtrlWithSimd16(G4_SendMsgDescriptor* Desc);
        void simplifyMsg(INST_LIST_ITER SendIter);


    public:
        SendFusion(FlowGraph* aCFG, Mem_Manager* aMMgr)
            : CFG(aCFG),
              MMgr(aMMgr),
              Builder(aCFG->builder),
              CurrBB(nullptr),
              DMaskUD(nullptr),
              FlagDefPerBB(nullptr)
        {
            initDMaskModInfo();
        }

        bool run(G4_BB* BB);
	};
}

// Return the Function Control that is the original except SIMD Mode
// is set to SIMD16.
uint32_t SendFusion::getFuncCtrlWithSimd16(G4_SendMsgDescriptor* Desc)
{

    uint32_t FC = Desc->getFuncCtrl();
    uint32_t funcID = Desc->getFuncId();
    uint32_t msgType = Desc->getMessageType();
    bool unsupported = false;
    if (funcID == SFID_DP_DC)
    {
        switch (msgType)
        {
        default:
            unsupported = true;
            break;

        case DC_DWORD_SCATTERED_READ:
        case DC_DWORD_SCATTERED_WRITE:
        case DC_BYTE_SCATTERED_READ:
        case DC_BYTE_SCATTERED_WRITE:
            // bit 8 : SM2
            FC = ((FC & ~0x100) | (MDC_SM2_SIMD16 << 0x8));
            break;

        case DC_UNTYPED_SURFACE_READ:
        case DC_UNTYPED_SURFACE_WRITE:
            // bit13-12: SM3
            FC = ((FC & ~0x3000) | (MDC_SM3_SIMD16 << 12));
            break;
        }
    }
    else if (funcID == SFID_DP_DC1)
    {
        switch (msgType)
        {
        default:
            unsupported = true;
            break;
        case DC1_UNTYPED_SURFACE_READ:
        case DC1_UNTYPED_SURFACE_WRITE:
            // bit13-12: SM3
            FC = ((FC & ~0x3000) | (MDC_SM3_SIMD16 << 12));
            break;
        }
    }
    else if (funcID == SFID_DP_DC2)
    {
        switch (msgType)
        {
        default:
            unsupported = true;
            break;
        case DC2_BYTE_SCATTERED_READ:
        case DC2_BYTE_SCATTERED_WRITE:
            // bit 8 : SM2
            FC = ((FC & ~0x100) | (MDC_SM2_SIMD16 << 0x8));
            break;

        case DC2_UNTYPED_SURFACE_READ:
        case DC2_UNTYPED_SURFACE_WRITE:
            // bit13-12: SM3
            FC = ((FC & ~0x3000) | (MDC_SM3_SIMD16 << 12));
            break;
        }
    }

    if (unsupported)
    {
        assert(false && "Unsupported message!");
    }
    return FC;
}

// We will do send fusion for a few messages. Those messages all
// have the address payload for each channel, thus address payload
// is 1 GRF for exec_size=8 (no A64 messages for now).
//
// The optimization is performed for the following cases:
//    1) [(w)] send(8) + send(8) --> (W&flag) send(16), and
//          Either noMask or not. When no NoMask, the shader
//          must be SIMD8.
//    2) (w) send(1|2|4) -> (W) send (2|4|8).
//          Must have NoMask.
//          Note that this case can be performed for shader of
//          SIMD8 or SIMD16, SIMD32.
//
bool SendFusion::simplifyAndCheckCandidate(INST_LIST_ITER Iter)
{
    G4_INST* I = *Iter;
    // For now, we will handle a few simple messages.
    // If needed, more messages can be handled later.
    G4_opcode opc = I->opcode();
    if ((opc != G4_send && opc != G4_sends) ||
        I->getExecSize() > 8 ||
        I->getPredicate() != nullptr ||
        !(I->is1QInst() || I->isWriteEnableInst()))
    {
        return false;
    }

    // If DMask is modified (for example, CPS), say in BB,
    // only handle sends that are after the modification in BB.
    //   BB
    //      send   // (1)
    //      send   // (2)
    //      sr0.2 = ...
    //      send   // (3)
    //      send   // (4)
    // only send 3 and 4 are handled. All sends before "sr0.2 = "
    // are skipped!

    if (LastSR0ModInstPerBB.count(CurrBB))
    {
        G4_INST* sr0ModInst = LastSR0ModInstPerBB[CurrBB];
        if (sr0ModInst->getLocalId() > I->getLocalId())
        {
            return false;
        }
    }

    G4_SendMsgDescriptor* msgDesc = I->getMsgDesc();
    if (!msgDesc->isHDC() || msgDesc->isHeaderPresent() || msgDesc->getSti() != nullptr)
    {
        return false;
    }

	// If exec_size=1|2|4, make sure that it is noMask and its
	// data payload/response is 1 GRF.
	uint16_t msgLen = msgDesc->MessageLength();
	uint16_t rspLen = msgDesc->ResponseLength();
	uint16_t extMsgLen = msgDesc->extMessageLength();
	if (I->getExecSize() < 8 &&
		!(I->isWriteEnableInst() &&
		  ((msgDesc->isDataPortWrite() && (msgLen + extMsgLen) == 2) ||
		   (msgDesc->isDataPortRead() && msgLen == 1 && rspLen == 1))))
	{
		return false;
	}

    // Unless we can prove there are no aliases of two sends, we will not be
    // able to do fusion (or we know for sure that the first addr's value or
    // last addr's value is taken). For now, disable it.
    if  (msgDesc->isDataPortWrite())
    {
        return false;
    }

    // Send might have a0 as its descriptor, if we know a0 is
    // a compile-time constant, replace a0 with the constant.
    simplifyMsg(Iter);

    // Make sure send's operands are direct GRF operands!
    G4_DstRegRegion* dst = I->getDst();
    G4_SrcRegRegion* src0 = I->getSrc(0)->asSrcRegRegion();
    G4_SrcRegRegion* src1 = I->isSplitSend() ? I->getSrc(1)->asSrcRegRegion() : nullptr;
    if ((dst && dst->isIndirect()) || 
        (src0 && src0->isIndirect()) ||
        (src1 && src1->isIndirect()))
    {
        return false;
    }

    // For now, handle constant descriptor (and ext Desc)
    G4_Operand* descOpnd = I->isSplitSend() ? I->getSrc(2) : I->getSrc(1);
    G4_Operand* extDescOpnd = I->isSplitSend() ? I->getSrc(3) : nullptr;
    if (!descOpnd->isImm() || (extDescOpnd && !extDescOpnd->isImm()))
    {
        return false;
    }

    uint32_t funcID = msgDesc->getFuncId();
    uint32_t msgType = msgDesc->getMessageType();
    if (funcID == SFID_DP_DC)
    {
        switch(msgType)
        {
        case DC_DWORD_SCATTERED_READ:
        case DC_DWORD_SCATTERED_WRITE:
        case DC_BYTE_SCATTERED_READ:
        case DC_BYTE_SCATTERED_WRITE:
        case DC_UNTYPED_SURFACE_READ:
        case DC_UNTYPED_SURFACE_WRITE:
            return true;
        }
    }
    else if (funcID == SFID_DP_DC1)
    {
        switch (msgType)
        {
        case DC1_UNTYPED_SURFACE_READ:
        case DC1_UNTYPED_SURFACE_WRITE:
            return true;
        }
    }
    else if (funcID == SFID_DP_DC2)
    {
        switch (msgType)
        {
        case DC2_BYTE_SCATTERED_READ:
        case DC2_BYTE_SCATTERED_WRITE:
        case DC2_UNTYPED_SURFACE_READ:
        case DC2_UNTYPED_SURFACE_WRITE:
            return true;
        }
    }
    
    return false;
}

G4_VarBase* SendFusion::getVarBase(G4_VarBase* Var, G4_Type Ty)
{
    G4_RegVar* RegVar = Var->asRegVar();
    G4_Declare* Dcl = RegVar->getDeclare();
    G4_Type DclTy = Dcl->getElemType();
    if (Dcl->getElemType() == Ty)
    {
        return Var;
    }
    int16_t sz = G4_Type_Table[DclTy].byteSize * Dcl->getNumElems();
    int16_t elts = sz / G4_Type_Table[Ty].byteSize;
    G4_Declare* newDcl = Builder->createTempVar(elts, Ty, Either, Any);
    newDcl->setAliasDeclare(Dcl->getRootDeclare(), Dcl->getAliasOffset());
    return newDcl->getRegVar();
}

// For Send's desc, it may have a0 as its descriptor. If so,
// simplifyMsg() tries to replace a0 with a constant if possible.
void SendFusion::simplifyMsg(INST_LIST_ITER SendIter)
{
    G4_INST* Send = *SendIter;
    Gen4_Operand_Number opn = Send->isSplitSend() ? Opnd_src2 : Opnd_src1;
    G4_Operand* descOpnd = Send->getSrc(opn - 1);
    if (descOpnd->isImm())
    {
        return;
    }

    // Need to find the bti from following pattern:
    //   (W) mov(1) T6(0, 0)<1>:d 0x2:uw
    //   (W) add(1) a0.0<1>:ud T6(0,0)<0;1,0> : ud 0x2110800:ud
    //       send(8) V84(0, 0)<1>:f V82(0,0)<1;1,0>:ud a0.0<0;1,0>:ud
    // 0x2 is the bti.
    int nDefs = 0;
    G4_INST *addI = nullptr;
    for (auto DI = Send->def_begin(), DE = Send->def_end(); DI != DE; ++DI)
    {
        if (DI->second == opn)
        {
            addI = DI->first;
            ++nDefs;
        }
    }

    if (nDefs != 1 || addI->opcode() != G4_add ||
        !addI->getSrc(1)->isImm() || !addI->isWriteEnableInst() ||
        addI->getExecSize() != 1) {
        return;
    }

    G4_INST* movI = nullptr;
    nDefs = 0;
    for (auto DI = addI->def_begin(), DE = addI->def_end(); DI != DE; ++DI)
    {
        if (DI->second == Opnd_src0)
        {
            movI = DI->first;
            ++nDefs;
        }
    }
    if (nDefs != 1 || movI->opcode() != G4_mov ||
        !movI->getSrc(0)->isImm() || !movI->isWriteEnableInst() ||
        movI->getExecSize() != 1) {
        return;
    }

    // Sanity check
    G4_SendMsgDescriptor* desc = Send->getMsgDesc();
    G4_Operand* bti = desc->getBti();
    if ((bti && bti->getTopDcl() != movI->getDst()->getTopDcl()) ||
        (addI->getSrc(1)->asImm()->getInt() != desc->getDesc()))
    {
        return;
    }

    uint32_t descImm =
        (uint32_t)(movI->getSrc(0)->asImm()->getInt() +
                   addI->getSrc(1)->asImm()->getInt());
    G4_Imm* newImm = Builder->createImm(descImm, descOpnd->getType());
    Send->setSrc(newImm, opn - 1);
    Send->removeDefUse(opn);

    // Need to re-create descriptor for this send
    G4_SendMsgDescriptor* newDesc = Builder->createSendMsgDesc(
        descImm,
        desc->getExtendedDesc(),
        desc->isDataPortRead(),
        desc->isDataPortWrite(),
        desc->getBti(),
        desc->getSti()); // should be nullptr
    Send->setMsgDesc(newDesc);

    // If addI or movI is dead, remove them.
    // Normally, addI and movI are right before Send and
    // remove them using iterator is more efficient. So
    // we check if the previous iterators refer to them,
    // if so, use iterators to remove them.
    // (As we found addI and movI in this BB, decreasing
    //  SendIter twice should be safe and no need to check
    //  if the iterator refers to the begin().)
    INST_LIST_ITER I1st = SendIter;
    --I1st;
    INST_LIST_ITER I2nd = I1st;
    --I2nd;
    if (addI->useEmpty())
    {
        addI->removeAllDefs();
        if (addI == *I1st)
        {
            CurrBB->instList.erase(I1st);
        }
        else
        {
            CurrBB->instList.remove(addI);
        }
    }
    if (movI->useEmpty())
    {
        movI->removeAllDefs();
        if (movI == *I2nd)
        {
            CurrBB->instList.erase(I2nd);
        }
        else
        {
            CurrBB->instList.remove(movI); 
        }
    }

    changed = true;
}

bool SendFusion::canFusion(INST_LIST_ITER IT0, INST_LIST_ITER IT1)
{
    G4_INST* I0 = *IT0;
    G4_INST* I1 = *IT1;
    G4_opcode opc = I0->opcode();
    assert( (opc == G4_send || opc == G4_sends) && opc == I1->opcode() &&
            "Arguments to canFusion must be the same kind of Send Messages!");

	// Only RAW (two loads) prevents fusing, WAW or WAR do not!
	if (I0->isRAWdep(I1))
	{
		return false;
	}

    // Current implementation uses split send to replace two sends. For simplicity,
    // here make sure their payload does not overlap!
    if (opc == G4_send)
    {
        G4_SrcRegRegion* s0 = I0->getSrc(0)->asSrcRegRegion();
        G4_SrcRegRegion* s1 = I1->getSrc(0)->asSrcRegRegion();
        if (s0->compareOperand(s1) != Rel_disjoint)
        { 
            return false;
        }
    }

    // Consider Send with Imm constant as descriptor, so
    // no need to check desc->getBti().
    G4_SendMsgDescriptor* desc0 = I0->getMsgDesc();
    G4_SendMsgDescriptor* desc1 = I1->getMsgDesc();
    bool fusion = I0->getOption() == I1->getOption() &&
                  (desc0->getDesc() == desc1->getDesc() &&
                   desc0->getExtendedDesc() == desc1->getExtendedDesc()) &&
                  !I0->isWAWdep(I1) && !I0->isRAWdep(I1);
    return fusion;
}

// canMoveOver() : common function used for sink and hoist.
//   Check if StartIT can sink to EndIT (right before EndIT) :  isForward == true.
//   Check if EndIT can hoist to StartIT (right after StartIT) : isForward == false.
// Return true if so, false otherwise.
// 
// Note that since StartIT and EndIT should be independent upon each other, their
// order (which one goes first) does not matter.
bool SendFusion::canMoveOver(
    INST_LIST_ITER StartIT, INST_LIST_ITER EndIT, bool isForward)
{
    G4_INST* Inst_first = *StartIT;
    G4_INST* Inst_last = *EndIT;
    if (Inst_first == Inst_last) {
        return true;
    }

    int lid_first = Inst_first->getLocalId();
    int lid_last = Inst_last->getLocalId();
    assert(lid_first <= lid_last && "Wrong inst position to sink to!");
    int span = lid_last - lid_first;
    if (span >= SEND_FUSION_MAX_SPAN) {
        return false;
    }

    bool movable = true;
    G4_INST* moveInst = (isForward ? Inst_first : Inst_last);
    INST_LIST_ITER II = (isForward ? StartIT : EndIT);
    INST_LIST_ITER IE = (isForward ? EndIT : StartIT);
    for (isForward ? ++II : --II;
         II != IE;
         isForward ? ++II : --II)
    {
       G4_INST* tmp = *II;
       if (moveInst->isWARdep(tmp) ||
           moveInst->isWAWdep(tmp) ||
           moveInst->isRAWdep(tmp))
       {
           // there is dependence, cannot move.
           movable = false;
           break;
       }
    }
    return movable;
}

// Packing payload:
//    Given the following (untyped surface write, resLen=0, msgLen=1, extMsgLen=2):
//      Send0:   sends (8|M0)  null r10 r12  0x8C   0x02026CFF
//      Send1:   sends (8|M0)  null r40 r42  0x8C   0x02026CFF
//
//    PackPayload() does:
//      mov (8) r18.0<1>:ud r10.0<8;8,1>:ud
//      mov (8) r19.0<1>:ud r40.0<8;8,1>:ud
//      mov (8) r20.0<1>:ud r12.0<8;8,1>:ud
//      mov (8) r21.0<1>:ud r42.0<8;8,1>:ud
//      mov (8) r22.0<1>:ud r13.0<8;8,1>:ud
//      mov (8) r23.0<1>:ud r43.0<8;8,1>:ud
//
//    With those mov instructions, the new send will be:
//     (untyped surface write, resLen=0, msgLen=2, extMsgLen=4)
//     (W&f0.0)) sends (16|M0)  null r18 r20  0x10C 0x04025CFF
//
// Note that f0.0 is created in doFusion(). Check it out for details.
//
void SendFusion::packPayload(
    G4_INST* FusedSend, G4_INST* Send0, G4_INST* Send1,
    INST_LIST& InsertList, INST_LIST_ITER InsertBeforePos)
{
    // Both Send0 and Send1 have the same MsgDesc.
	unsigned char ExecSize = Send0->getExecSize();
    G4_SendMsgDescriptor* origDesc = Send0->getMsgDesc();
    int option = Send0->getOption();

    int16_t msgLen = origDesc->MessageLength();
    int16_t extMsgLen = origDesc->extMessageLength();
    RegionDesc* stride1 = Builder->getRegionStride1();

    // Sanity check for exec_size =1|2|4
	if (ExecSize < 8)
	{
		assert((origDesc->isDataPortWrite() ||
			    (msgLen == 1 && origDesc->ResponseLength() == 1)) &&
			"Internal Error (SendFusion): unexpected read message!");
		assert((origDesc->isDataPortRead() || (msgLen + extMsgLen == 2)) &&
			"Internal Error (SendFusion): unexpected write message!");
	}
	
    // Using a loop of count == 2 for handing both Msg & extMsg payload.
	// 
	// Note that the following works for exec_size=1|2|4|8. It is designed
	// to handle exec_size=8. It also works for exec_size=1|2|4 with minor
	// changes.
    int16_t numMov[2] = {msgLen, extMsgLen};

    for (int i=0; i < 2; ++i)
    {
        int32_t nMov = numMov[i];
        Gen4_Operand_Number opn = (i == 0 ? Opnd_src0 : Opnd_src1);
        if (nMov <= 0) continue;
        G4_Type Ty = FusedSend->getOperand(opn)->getType();

        assert(G4_Type_Table[Ty].byteSize == 4 && "Unexpected type for send!");

        G4_SrcRegRegion* Reg0 = Send0->getOperand(opn)->asSrcRegRegion();
        G4_SrcRegRegion* Reg1 = Send1->getOperand(opn)->asSrcRegRegion();
        G4_VarBase* Var0 = getVarBase(Reg0->getBase(), Ty);
        G4_VarBase* Var1 = getVarBase(Reg1->getBase(), Ty);
        G4_VarBase* Dst = FusedSend->getOperand(opn)->getBase();
        int16_t Off0 = Reg0->getRegOff();
        int16_t Off1 = Reg1->getRegOff();
        for (int i = 0; i < nMov; ++i)
        {
            // copy Src0 to Dst
            G4_SrcRegRegion* S = Builder->createSrcRegRegion(
                Mod_src_undef, Direct, Var0, Off0 + i, 0, stride1, Ty);
            G4_DstRegRegion* D = Builder->createDstRegRegion(
                Direct, Dst, 2 * i, 0, 1, Ty);
            G4_INST* Inst0 = Builder->createInternalInst(
                NULL, G4_mov, NULL, false, ExecSize, D, S, nullptr, option);
            InsertList.insert(InsertBeforePos, Inst0);

            // copy Src1 to Dst
            S = Builder->createSrcRegRegion(
                Mod_src_undef, Direct, Var1, Off1 + i, 0, stride1, Ty);
            D = Builder->createDstRegRegion(
                Direct, Dst,
				(ExecSize == 8 ? (2 * i + 1) : 2 * i),
				(ExecSize == 8 ? 0 : ExecSize),
				1, Ty);
            G4_INST* Inst1 = Builder->createInternalInst(
                NULL, G4_mov, NULL, false, ExecSize, D, S, nullptr, option);
            InsertList.insert(InsertBeforePos, Inst1);

            // Update DefUse
            Inst0->addDefUse(FusedSend, opn);
            Send0->copyDef(Inst0, opn, Opnd_src0, true);
            Inst1->addDefUse(FusedSend, opn);
            Send1->copyDef(Inst1, opn, Opnd_src0, true);
        }
    }
}

// unpackPayload does the following for read messages.
//   Given the following (untyped surface read, resLen=2, msgLen=1)
//     Send0: send (8|M0) r8:f   r7   0xC  0x02206CFF
//     Send0: send (8|M0) r21:f  r10  0xC  0x02206CFF
//
//   Assuming the fused send is as follows:
//     (untyped surface read, resLen=4, msgLen=1, extMsgLen=1)
//     (W&f0.0) sends (16|M0) r16:f  r7  r10  0x4C  0x02405CFF
//   (Note: check doFusion about f0.0)
//
//   Unpacking code is:
//     mov (8|M0) r8.0<1>:f  r16.0<8;8,1>:f
//     mov (8|M0) r9.0<1>:f  r18.0<8;8,1>:f
//     mov (8|M0) r21.0<1>:f r17.0<8;8,1>:f
//     mov (8|M0) r22.0<1>:f r19.0<8;8,1>:f
//
void SendFusion::unpackPayload(
    G4_INST* FusedSend, G4_INST* Send0, G4_INST* Send1,
    INST_LIST& InsertList, INST_LIST_ITER InsertBeforePos)
{
    G4_Type Ty = FusedSend->getDst()->getType();
    assert(G4_Type_Table[Ty].byteSize == 4 && "Unexpected Type!");

    // Use the original option for mov instructions
	unsigned char ExecSize = Send0->getExecSize();
    int option = Send0->getOption();
    int32_t nMov = Send0->getMsgDesc()->ResponseLength();

	// Make sure the response len = 1 for exec_size = 1|2|4
	//
	// Note that the code is designed for exec_size=8. It also
	// works for exec_size=1|2|4 with minor change (keep in mind
	// that nMov = 1 for exec_size=1|2|4)
	assert((ExecSize == 8 || nMov == 1) &&
		   "Internal Error(SendFusion) : unexpected message response length!");

    G4_VarBase* Payload = FusedSend->getDst()->getBase();
    G4_VarBase* Dst0 = getVarBase(Send0->getDst()->getBase(), Ty);
    G4_VarBase* Dst1 = getVarBase(Send1->getDst()->getBase(), Ty);
    int16_t Off0 = Send0->getDst()->getRegOff();
    int16_t Off1 = Send1->getDst()->getRegOff();
    RegionDesc *stride1 = Builder->getRegionStride1();

    G4_SrcRegRegion* S;
    G4_DstRegRegion* D;
    // Copy to Dst0
    for (int i = 0; i < nMov; ++i)
    {
        S = Builder->createSrcRegRegion(
            Mod_src_undef, Direct, Payload, 2*i, 0, stride1, Ty);
        D = Builder->createDstRegRegion(
            Direct, Dst0, Off0 + i, 0, 1, Ty);
        G4_INST* Inst0 = Builder->createInternalInst(
            NULL, G4_mov, NULL, false, ExecSize, D, S, nullptr, option);
        InsertList.insert(InsertBeforePos, Inst0);

        // Update DefUse
        FusedSend->addDefUse(Inst0, Opnd_src0);
        Send0->copyUsesTo(Inst0, true);
    }

    // Copy to Dst1
    for (int i = 0; i < nMov; ++i)
    { 
        S = Builder->createSrcRegRegion(
            Mod_src_undef, Direct, Payload,
			(ExecSize == 8 ? 2*i + 1 : 2*i),
			(ExecSize == 8) ? 0 : ExecSize,
			stride1, Ty);
        D = Builder->createDstRegRegion(Direct, Dst1, Off1 + i, 0, 1, Ty);
        G4_INST* Inst1 = Builder->createInternalInst(
            NULL, G4_mov, NULL, false, ExecSize, D, S, nullptr, option);
        InsertList.insert(InsertBeforePos, Inst1);

        // Update DefUse
        FusedSend->addDefUse(Inst1, Opnd_src0);
        Send1->copyUsesTo(Inst1, true);
    }
}

void SendFusion::initDMaskModInfo()
{
    for (BB_LIST_ITER BI = CFG->BBs.begin(), BE = CFG->BBs.end(); BI != BE; ++BI)
    {
        G4_BB* BB = *BI;
        for (INST_LIST_ITER II = BB->instList.begin(), IE = BB->instList.end(); II != IE; ++II)
        {
            G4_INST* inst = *II;
            G4_DstRegRegion* dst = inst->getDst();
            // Check if sr0.2 (DW) is modified.
            if (dst && dst->isAreg() && dst->isSrReg() &&
                ((dst->getLeftBound() <= 8 && dst->getRightBound() >= 8) ||
                 (dst->getLeftBound() <= 11 && dst->getRightBound() >= 11)))
            {
                LastSR0ModInstPerBB[BB] = inst;
            }
        }
    }
}

// Dispatch mask
// The real channel mask for a thread is computed as
//          sr0.2 & ce0
// This function saves sr0.2 (DMask) in a variable in the entry BB (one for
// each shader/kernel) for the later use. We have to do (sr0.2 & ce0) for each
// BB as ce0 reflects the channel enable under control flow, and each BB might
// have different value of ce0.
void SendFusion::createDMask(INST_LIST* InsertList, INST_LIST_ITER InsertBeforePos)
{
    // (W) mov (1|M0) r10.0<1>:ud sr0.2.0<0;1,0>:ud
    G4_Declare* dmaskDecl = Builder->createTempVar(1, Type_UD, Either, Any, "DMask");
    G4_VarBase* sr0 = Builder->phyregpool.getSr0Reg();
    G4_SrcRegRegion* Src = Builder->createSrcRegRegion(
        Mod_src_undef, Direct, sr0, 0, 2, Builder->getRegionScalar(), Type_UD);
    G4_DstRegRegion* Dst = Builder->createDstRegRegion(
        Direct, dmaskDecl->getRegVar(), 0, 0, 1, Type_UD);
    G4_INST* Inst = Builder->createInternalInst(
        NULL, G4_mov, NULL, false, 1, Dst, Src, NULL, InstOpt_WriteEnable);
    InsertList->insert(InsertBeforePos, Inst);

    // update DefUse info
    CFG->globalOpndHT.addGlobalOpnd(Dst);

    // Save DMaskUD for use later.
    DMaskUD = dmaskDecl->getRegVar();

    for (auto II = LastSR0ModInstPerBB.begin(), IE = LastSR0ModInstPerBB.end();
         II != IE; ++II)
    {
        G4_BB* BB = II->first;
        G4_INST* inst = II->second;
        INST_LIST_ITER InsertPos = std::find(BB->instList.begin(), BB->instList.end(), inst);
        ++InsertPos;

        G4_SrcRegRegion* S = Builder->createSrcRegRegion(
            Mod_src_undef, Direct, sr0, 0, 2, Builder->getRegionScalar(), Type_UD);
        G4_DstRegRegion* D = Builder->createDstRegRegion(
            Direct, dmaskDecl->getRegVar(), 0, 0, 1, Type_UD);
        G4_INST* Inst = Builder->createInternalInst(
            NULL, G4_mov, NULL, false, 1, D, S, NULL, InstOpt_WriteEnable);
        BB->instList.insert(InsertPos, Inst);
    }
}

// This function will create a flag for each BB. And this flag is used as pred for
// all fused sends in the BB.  It basically does:
//
//     (W) and (1|M0) r11.0<1>:ud ce0.0<0;1,0>:ud DMaskUD
//     (W) mov (2|M0) r12.0<1>:ub r11.0<0;1,0>:ub
//     (W) mov (1|M0) f0.0<1>:uw 0:ud r12.0<1>:uw
//
// where DMaskUD is computed in createDMask. Note that those instructions are
// right before the location of first send fusion, not in the begining of BB
// (as BB might have sr0 modifying instruction before those send instructions).
//
void SendFusion::createFlagPerBB(INST_LIST* InsertList, INST_LIST_ITER InsertBeforePos)
{
    // FlagPerBB is saved for use later.
    G4_Declare* flagDecl = Builder->createTempFlag(1, "FlagPerBB");
    G4_VarBase* FlagPerBB = flagDecl->getRegVar();
    RegionDesc* scalar = Builder->getRegionScalar();

    // (W) and (|M0) tmp<1>:ud ce0.0<0;1,0>:ud DMaskUD:ud
    G4_Declare* tmpDecl = Builder->createTempVar(1, Type_UD, Either, Any, "Flag");
    G4_SrcRegRegion *ce0Src = Builder->createSrcRegRegion(
        Mod_src_undef, Direct, Builder->phyregpool.getMask0Reg(), 0, 0, scalar, Type_UD);
    G4_SrcRegRegion *dmaskSrc = Builder->createSrcRegRegion(
        Mod_src_undef, Direct, DMaskUD, 0, 0, scalar, Type_UD);
    G4_DstRegRegion* tmpDst = Builder->createDstRegRegion(
        Direct, tmpDecl->getRegVar(), 0, 0, 1, Type_UD);
    G4_INST* Inst0 = Builder->createInternalInst(
        NULL, G4_and, NULL, false, 1, tmpDst, ce0Src, dmaskSrc, InstOpt_WriteEnable);
    InsertList->insert(InsertBeforePos, Inst0);

    //  Duplicate 8-bit mask to the next 8 bits
    //  (W) mov (2|M0) tmp:ub tmp.0<0;1,0>:ub
    G4_Declare* tmpUBDecl = Builder->createTempVar(4, Type_UB, Either, Any, "Flag");
    tmpUBDecl->setAliasDeclare(tmpDecl, 0);
    G4_SrcRegRegion* S = Builder->createSrcRegRegion(
        Mod_src_undef, Direct, tmpUBDecl->getRegVar(), 0, 0, scalar, Type_UB);
    G4_DstRegRegion* D = Builder->createDstRegRegion(
        Direct, tmpUBDecl->getRegVar(), 0, 0, 1, Type_UB);
    G4_INST* Inst1 = Builder->createInternalInst(
        NULL, G4_mov, NULL, false, 2, D, S, nullptr, InstOpt_WriteEnable);
    InsertList->insert(InsertBeforePos, Inst1);

    // update DefUse
    Inst0->addDefUse(Inst1, Opnd_src0);

    // (W) mov (1|M0) flagPerBB.0<1>:UW tmp.0<1>:UW
    G4_Declare* tmpUW = Builder->createTempVar(1, Type_UW, Either, Any);
    tmpUW->setAliasDeclare(tmpDecl, 0);
    G4_SrcRegRegion* Src = Builder->createSrcRegRegion(
        Mod_src_undef, Direct, tmpUW->getRegVar(), 0, 0, scalar, Type_UW);
    G4_DstRegRegion* flag = Builder->createDstRegRegion(
        Direct, FlagPerBB, 0, 0, 1, Type_UW);
    FlagDefPerBB = Builder->createInternalInst(
        NULL, G4_mov, NULL, false, 1, flag, Src, nullptr, InstOpt_WriteEnable);
    InsertList->insert(InsertBeforePos, FlagDefPerBB);

    // update DefUse
    Inst1->addDefUse(FlagDefPerBB, Opnd_src0);
}

void SendFusion::doFusion(
    INST_LIST_ITER IT0, INST_LIST_ITER IT1, INST_LIST_ITER InsertBeforePos)
{
    // This function does the following:
    //    Given the following two sends:
    //      send (8|M0)  r40  r39  0xA  0x02110802 // byte scattered read, resLen=1, msgLen=1
    //      send (8|M0)  r42  r41  0xA  0x02110802 // byte scattered read, resLen=1, msgLen=1
    //    It generates the following:
    //      (1) Setting flag register
    //            (Need to do Sr0.2 & ce0.0 to get real channel mask)
    //         (1.1) Dispatch mask 
    //              (W) mov (1|M0) r10.0<1>:ud sr0.2.0<0;1,0>:ud
    //         (1.2)
    //              (W) and (1|M0) r11.0<1>:ud ce0.0<0;1,0>:ud r10.0<0;1,0>:ud
    //              (W) mov (2|M0) r12.0<1>:ub r11.0<0;1,0>:ub
    //              (W) mov (1|M0) f0.0<1>:uw 0:ud r12.0<1>:uw
    //
    //         Note : (1.1) is saved in entry BB (one for each shader). (1.2) is saved in each BB
    //                (one for each BB as each BB might have different ce0).
    //                Also, if the original sends are WriteEnabled, no Pred is needed.
    //      (2) Generating send (16)
    //                   mov (8|M0) r50.0<1>:ud r39.0<8;8,1>:ud
    //                   mov (8|M0) r51.0<1>:ud r41.0<8;8,1>:ud
    //          (W&f0.0) send (16|M0) r52:f r50 0xA 0x 0x4210802
    //                   mov (8|M0) r40.0<1>:ud r52.0<8;8,1>:ud
    //                   mov (8|M0) r42.0<1>:ud r53.0<8;8,1>:ud
    //
    //          Note this is to explain the idea. For this case, this function
    //          actually generate split send, which avoid packing payloads before
    //          the split send.

    G4_INST* I0 = *IT0;
    G4_INST* I1 = *IT1;

    // Use I0 as both I0 and I1 have the same properties
    G4_SendMsgDescriptor* desc = I0->getMsgDesc();
	unsigned char ExecSize = I0->getExecSize();
    bool isWrtEnable = I0->isWriteEnableInst();
    bool isSplitSend = I0->isSplitSend();

    if (!isWrtEnable)
    {
        if (DMaskUD == nullptr)
        {
            // First time, let's save dispatch mask in the entry BB and
            // use it for all Channel enable calcuation in the entire shader.
            // Note that if dispatch mask is modified in the shader, the DMaskUD
            // will need to be saved right after each modification.
            G4_BB* entryBB = CFG->getEntryBB();
            INST_LIST_ITER insertBeforePos = entryBB->instList.begin();

            // Skip the label if present (only first inst can be label).
            if (insertBeforePos != entryBB->instList.end() &&
                (*insertBeforePos)->isLabel())
            {
                ++insertBeforePos;
            }
            createDMask(&(entryBB->instList), insertBeforePos);
        }

        if (FlagDefPerBB == nullptr)
        {
            createFlagPerBB(&(CurrBB->instList), InsertBeforePos);
        }
    }

    G4_VarBase* FlagPerBB = nullptr;
    uint32_t rspLen = desc->ResponseLength();
    uint32_t msgLen = desc->MessageLength();
    uint32_t extMsgLen = desc->extMessageLength();

    // Message header : all candidates have no header (for now).
	//
	// Also for the message type that we handle, if execSize is 1|2|4,
	//     write:  msgLen+extMsgLen = 2; and
	//     read:   msgLen = 1 && rspLen = 1
    uint32_t newMsgLen = (ExecSize < 8 ? msgLen : 2*msgLen);
    uint32_t newRspLen = (ExecSize < 8 ? rspLen : 2*rspLen);
    uint32_t newExtMsgLen = (ExecSize < 8 ? extMsgLen : 2*extMsgLen);

    G4_Predicate* Pred = nullptr;
    if (!isWrtEnable)
    {
        FlagPerBB = FlagDefPerBB->getDst()->asDstRegRegion()->getBase();
        Pred = Builder->createPredicate(PredState_Plus, FlagPerBB, 0);
    }

    G4_Declare* DstD = nullptr;
    G4_DstRegRegion* Dst;
    if (rspLen > 0)
    {
        G4_Type DstTy = I0->getDst()->getType();
        if (G4_Type_Table[DstTy].byteSize != 4)
        {
            DstTy = Type_UD;
        }
        DstD = Builder->createTempVar(newRspLen * 8, DstTy, Either, Any, "dst");
        Dst = Builder->Create_Dst_Opnd_From_Dcl(DstD, 1);
    }
    else
    {
        Dst = Builder->createNullDst(Type_UD);
    }

    
    // No need to set bti here as we handle the case in which bti is imm only.
    // For that imm bti, the descriptor has already contained bti. Thus, we can
    // safely set bti to nullptr here.
    //G4_Operand* bti = (desc->getBti() ? Builder->duplicateOperand(desc->getBti()) : nullptr);
    G4_Operand* bti = nullptr;
    uint32_t newFC = (ExecSize < 8 ? desc->getFuncCtrl()
		                           : getFuncCtrlWithSimd16(desc));

    // Special case of two reads whose payloads can be concatenated using split send.
    if (!isSplitSend && ExecSize == 8 && rspLen > 0 && (msgLen == 1))
    {
        G4_SendMsgDescriptor* newDesc = Builder->createSendMsgDesc(
            newFC, newRspLen, msgLen, 
            desc->getFuncId(),
            false,
            msgLen,
            desc->getExtFuncCtrl(),
            desc->isDataPortRead(),
            desc->isDataPortWrite(),
            bti, nullptr);

        G4_SrcRegRegion* s0 = I0->getOperand(Opnd_src0)->asSrcRegRegion();
        G4_SrcRegRegion* s1 = I1->getOperand(Opnd_src0)->asSrcRegRegion();
        G4_SrcRegRegion* Src0 = Builder->createSrcRegRegion(*s0);
        G4_SrcRegRegion* Src1 = Builder->createSrcRegRegion(*s1);
        G4_INST* sendInst = Builder->createSplitSendInst(
            Pred, G4_sends, 16, Dst, Src0, Src1,
            Builder->createImm(newDesc->getDesc(), Type_UD),
            InstOpt_WriteEnable, newDesc, nullptr, 0);
        CurrBB->instList.insert(InsertBeforePos, sendInst);

        // Update DefUse
        if (Pred)
        {
            FlagDefPerBB->addDefUse(sendInst, Opnd_pred);
        }
        I0->transferDef(sendInst, Opnd_src0, Opnd_src0);
        I1->transferDef(sendInst, Opnd_src0, Opnd_src1);

        // Unpack the result
        unpackPayload(sendInst, I0, I1, CurrBB->instList, InsertBeforePos);

        // Delete I0 and I1 and updating defuse info
        CurrBB->instList.erase(IT0);
        CurrBB->instList.erase(IT1);
        return;
    }

    G4_SendMsgDescriptor* newDesc = Builder->createSendMsgDesc(
        newFC, newRspLen, newMsgLen,
        desc->getFuncId(),
        false,
        newExtMsgLen,
        desc->getExtFuncCtrl(),
        desc->isDataPortRead(),
        desc->isDataPortWrite(),
        bti, nullptr);

    // First, create fused send.
    RegionDesc* region = Builder->getRegionStride1();
    G4_Type P0Ty = I0->getOperand(Opnd_src0)->getType();
    if (G4_Type_Table[P0Ty].byteSize != 4)
    {
        P0Ty = Type_UD;
    }
    G4_Declare* P0 = Builder->createTempVar(newMsgLen * 8, P0Ty, Either, Any, "payload0");
    G4_SrcRegRegion* Src0 = Builder->Create_Src_Opnd_From_Dcl(P0, region);

    G4_Declare* P1 = nullptr;
    G4_INST* sendInst = nullptr;
    if (isSplitSend)
    {
        G4_Type P1Ty = I0->getOperand(Opnd_src1)->getType();
        if (G4_Type_Table[P1Ty].byteSize != 4)
        {
            P1Ty = Type_UD;
        }
        P1 = Builder->createTempVar(newExtMsgLen * 8, P1Ty, Either, Any, "payload1");
        G4_SrcRegRegion* Src1 = Builder->Create_Src_Opnd_From_Dcl(P1, region);
        sendInst = Builder->createSplitSendInst(
            Pred, G4_sends, ExecSize*2, Dst, Src0, Src1,
            Builder->createImm(newDesc->getDesc(), Type_UD),
            InstOpt_WriteEnable, newDesc, nullptr, 0);
    }
    else
    {
        sendInst = Builder->createSendInst(
            Pred, G4_send, ExecSize*2, Dst, Src0,
            Builder->createImm(newDesc->getExtendedDesc(), Type_UD),
            Builder->createImm(newDesc->getDesc(), Type_UD),
            InstOpt_WriteEnable,
            newDesc->isDataPortRead(),
            newDesc->isDataPortWrite(),
            newDesc);
    }

    // For messages we handle here, payloads are packing/unpacking
	// in an interleaving way.
    packPayload(sendInst, I0, I1, CurrBB->instList, InsertBeforePos);

    // Update DefUse
    if (Pred) {
        FlagDefPerBB->addDefUse(sendInst, Opnd_pred);
    }

    CurrBB->instList.insert(InsertBeforePos, sendInst);

    if (rspLen > 0)
    {
        // Unpack the result
        unpackPayload(sendInst, I0, I1, CurrBB->instList, InsertBeforePos);
    }

    // Delete I0 and I1 and updating defUse info
    I0->removeAllUses();
    I0->removeAllDefs();
    I1->removeAllUses();
    I1->removeAllDefs();
    CurrBB->instList.erase(IT0);
    CurrBB->instList.erase(IT1);
}


bool SendFusion::run(G4_BB* BB)
{
    // Prepare for processing this BB
    CurrBB = BB;
    FlagDefPerBB = nullptr;
    CurrBB->resetLocalId();

    // Found two candidate sends:
    //    1. next to each (no other sends in between), and
    //    2. both have the same message descriptor.
    INST_LIST_ITER II0 = CurrBB->instList.begin();
    INST_LIST_ITER IE = CurrBB->instList.end();
    while (II0 != IE)
    {
        // Find out two send instructions (inst0 and inst1) that are next
        // to each other, and check to see if they can be fused into a
        // single one.
        G4_INST* inst0 = *II0;
        if (!simplifyAndCheckCandidate(II0)) {
            ++II0;
            continue;
        }
 
        G4_INST* inst1 = nullptr;
        INST_LIST_ITER II1 = II0;
        ++II1;
        while(II1 != IE)
        {
            G4_INST* tmp = *II1;
            if (tmp->opcode() == inst0->opcode() &&
				tmp->getExecSize() == inst0->getExecSize() &&
                simplifyAndCheckCandidate(II1))
            {
                if (canFusion(II0, II1))
                {
                    // Found
                    inst1 = tmp;
                }

                // Don't advance II1 as II1 might be the first send
                // for the next pair of candidates.
                break;
            }

            ++II1;
            G4_DstRegRegion* Dst = tmp->getDst();
            if (tmp->isSend() || tmp->isFence() ||
                (tmp->isOptBarrier() && Dst && Dst->isAreg() && Dst->isSrReg()))
            {
                // Don't try to fusion two sends that are separated
                // by other memory instructions.             
                break;
            }
        }

        if (inst1 == nullptr) {
            // No inst1 found b/w II0 and II1.
            // Start finding the next candidate from II1
            II0 = II1;
            continue;
        }
  
        // At this point, inst0 and inst1 are the pair that can be fused.
        // Now, check if they can be moved to the same position.
        bool sinkable = canSink(II0, II1);
        if (!sinkable && !canHoist(II1, II0)) {
            II0 = II1;
            continue;
        }
        INST_LIST_ITER insertPos = sinkable ? II1 : II0;

        // Perform fusion (either sink or hoist). It also delete
        // II0 and II1 after fusion. Thus, need to save ++II1
        // before invoking doFusion() for the next iteration.
        INST_LIST_ITER next_II = II1;
        ++next_II;
        doFusion(II0, II1, insertPos);

        changed = true;
        II0 = next_II;
    }

    return changed;
}

//
// The main goal is to do the following for SIMD8 shader:
//
//    [(w)] send(8) + send(8) --> (W&flag) send(16)
//
//      Either noMask or not. When no NoMask
//
// Note that (w) send(1|2|4) is also supported.
//
bool vISA::doSendFusion(FlowGraph* aCFG, Mem_Manager* aMMgr)
{
	// If split send isn't supported, simply skip send fusion
	if (!aCFG->builder->useSends())
	{
		return false;
	}

	SendFusion sendFusion(aCFG, aMMgr);

    bool change = false;
    for (BB_LIST_ITER BI = aCFG->BBs.begin(), BE = aCFG->BBs.end(); BI != BE; ++BI)
    {
        G4_BB* BB = *BI;
	    if (sendFusion.run(BB)) {
            change = true;
        }
    }
	return change;
}
