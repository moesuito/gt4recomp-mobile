// SPDX-FileCopyrightText: 2026 GT4Recomp contributors
// SPDX-License-Identifier: GPL-3.0+
//
// GT4GS_NO_JIT (private Android build): the ARM64 recompiler sources (EE/IOP/microVU/VIF/VU-AOT) are not compiled
// into this library, so vixl is not linked. The opcode tables and the savestate class still name their entry points,
// which keeps the link honest; these empty definitions close it. Nothing calls them: the library maps no executable
// code memory, VMManager initializes no recompiler provider, and every CPU provider is an interpreter (the GS-only VM
// uses the stream CPU provider). Generated from R5900OpcodeTables.h by gen_GT4GS_nojit_stubs.py, not hand-edited.

#include "R5900OpcodeTables.h"
#include "SaveState.h"
#include "VU.h"
#include "arm64/FPU-divunit-arm64.h"

namespace R5900::Dynarec::OpcodeImpl
{
	void recNULL() {}
	void recUnknown() {}
	void recMMI_Unknown() {}
	void recCOP0_Unknown() {}
	void recCOP1_Unknown() {}
	void recCOP2() {}
	void recCACHE() {}
	void recPREF() {}
	void recSYSCALL() {}
	void recBREAK() {}
	void recSYNC() {}
	void recMFSA() {}
	void recMTSA() {}
	void recMTSAB() {}
	void recMTSAH() {}
	void recTGE() {}
	void recTGEU() {}
	void recTLT() {}
	void recTLTU() {}
	void recTEQ() {}
	void recTNE() {}
	void recTGEI() {}
	void recTGEIU() {}
	void recTLTI() {}
	void recTLTIU() {}
	void recTEQI() {}
	void recTNEI() {}
	void recADDI() {}
	void recADDIU() {}
	void recDADDI() {}
	void recDADDIU() {}
	void recANDI() {}
	void recORI() {}
	void recXORI() {}
	void recSLTI() {}
	void recSLTIU() {}
	void recADD() {}
	void recADDU() {}
	void recDADD() {}
	void recDADDU() {}
	void recSUB() {}
	void recSUBU() {}
	void recDSUB() {}
	void recDSUBU() {}
	void recAND() {}
	void recOR() {}
	void recXOR() {}
	void recNOR() {}
	void recSLT() {}
	void recSLTU() {}
	void recMULT() {}
	void recMULTU() {}
	void recDIV() {}
	void recDIVU() {}
	void recSLL() {}
	void recSRL() {}
	void recSRA() {}
	void recDSLL() {}
	void recDSRL() {}
	void recDSRA() {}
	void recDSLL32() {}
	void recDSRL32() {}
	void recDSRA32() {}
	void recSLLV() {}
	void recSRLV() {}
	void recSRAV() {}
	void recDSLLV() {}
	void recDSRLV() {}
	void recDSRAV() {}
	void recBEQ() {}
	void recBEQL() {}
	void recBNE() {}
	void recBNEL() {}
	void recBLTZ() {}
	void recBLTZL() {}
	void recBLTZAL() {}
	void recBLTZALL() {}
	void recBGTZ() {}
	void recBGTZL() {}
	void recBLEZ() {}
	void recBLEZL() {}
	void recBGEZ() {}
	void recBGEZL() {}
	void recBGEZAL() {}
	void recBGEZALL() {}
	void recJ() {}
	void recJAL() {}
	void recJR() {}
	void recJALR() {}
	void recLB() {}
	void recLBU() {}
	void recLH() {}
	void recLHU() {}
	void recLW() {}
	void recLWU() {}
	void recLWL() {}
	void recLWR() {}
	void recLD() {}
	void recLDR() {}
	void recLDL() {}
	void recLQ() {}
	void recSB() {}
	void recSH() {}
	void recSW() {}
	void recSWL() {}
	void recSWR() {}
	void recSD() {}
	void recSDL() {}
	void recSDR() {}
	void recSQ() {}
	void recLWC1() {}
	void recSWC1() {}
	void recLQC2() {}
	void recSQC2() {}
	void recLUI() {}
	void recMFLO() {}
	void recMFHI() {}
	void recMTLO() {}
	void recMTHI() {}
	void recMOVN() {}
	void recMOVZ() {}
	void recMADD1() {}
	void recMADDU1() {}
	void recMADD() {}
	void recMADDU() {}
	void recMTHI1() {}
	void recMTLO1() {}
	void recMFHI1() {}
	void recMFLO1() {}
	void recMULT1() {}
	void recMULTU1() {}
	void recDIV1() {}
	void recDIVU1() {}
} // namespace R5900::Dynarec::OpcodeImpl

namespace R5900::Dynarec::OpcodeImpl::MMI
{
	void recPLZCW() {}
	void recMMI0() {}
	void recMMI1() {}
	void recMMI2() {}
	void recMMI3() {}
	void recPMFHL() {}
	void recPMTHL() {}
	void recPMAXW() {}
	void recPMINW() {}
	void recPPACW() {}
	void recPEXTLH() {}
	void recPPACH() {}
	void recPEXTLB() {}
	void recPPACB() {}
	void recPEXT5() {}
	void recPPAC5() {}
	void recPABSW() {}
	void recPADSBH() {}
	void recPABSH() {}
	void recPADDUW() {}
	void recPSUBUW() {}
	void recPSUBUH() {}
	void recPEXTUH() {}
	void recPSUBUB() {}
	void recPEXTUB() {}
	void recQFSRV() {}
	void recPMADDW() {}
	void recPSLLVW() {}
	void recPSRLVW() {}
	void recPMSUBW() {}
	void recPINTH() {}
	void recPMULTW() {}
	void recPDIVW() {}
	void recPMADDH() {}
	void recPHMADH() {}
	void recPMSUBH() {}
	void recPHMSBH() {}
	void recPEXEH() {}
	void recPREVH() {}
	void recPMULTH() {}
	void recPDIVBW() {}
	void recPEXEW() {}
	void recPROT3W() {}
	void recPMADDUW() {}
	void recPSRAVW() {}
	void recPINTEH() {}
	void recPMULTUW() {}
	void recPDIVUW() {}
	void recPEXCH() {}
	void recPEXCW() {}
	void recPSRLH() {}
	void recPSRLW() {}
	void recPSRAH() {}
	void recPSRAW() {}
	void recPSLLH() {}
	void recPSLLW() {}
	void recPMAXH() {}
	void recPCGTB() {}
	void recPCGTH() {}
	void recPCGTW() {}
	void recPADDSB() {}
	void recPADDSH() {}
	void recPADDSW() {}
	void recPSUBSB() {}
	void recPSUBSH() {}
	void recPSUBSW() {}
	void recPADDB() {}
	void recPADDH() {}
	void recPADDW() {}
	void recPSUBB() {}
	void recPSUBH() {}
	void recPSUBW() {}
	void recPEXTLW() {}
	void recPEXTUW() {}
	void recPMINH() {}
	void recPCEQB() {}
	void recPCEQH() {}
	void recPCEQW() {}
	void recPADDUB() {}
	void recPADDUH() {}
	void recPMFHI() {}
	void recPMFLO() {}
	void recPAND() {}
	void recPXOR() {}
	void recPCPYLD() {}
	void recPNOR() {}
	void recPMTHI() {}
	void recPMTLO() {}
	void recPCPYUD() {}
	void recPOR() {}
	void recPCPYH() {}
} // namespace R5900::Dynarec::OpcodeImpl::MMI

namespace R5900::Dynarec::OpcodeImpl::COP0
{
	void recMFC0() {}
	void recMTC0() {}
	void recBC0F() {}
	void recBC0T() {}
	void recBC0FL() {}
	void recBC0TL() {}
	void recTLBR() {}
	void recTLBWI() {}
	void recTLBWR() {}
	void recTLBP() {}
	void recERET() {}
	void recDI() {}
	void recEI() {}
} // namespace R5900::Dynarec::OpcodeImpl::COP0

namespace R5900::Dynarec::OpcodeImpl::COP1
{
	void recMFC1() {}
	void recCFC1() {}
	void recMTC1() {}
	void recCTC1() {}
	void recCOP1_BC1() {}
	void recCOP1_S() {}
	void recCOP1_W() {}
	void recC_EQ() {}
	void recC_F() {}
	void recC_LT() {}
	void recC_LE() {}
	void recADD_S() {}
	void recSUB_S() {}
	void recMUL_S() {}
	void recDIV_S() {}
	void recSQRT_S() {}
	void recABS_S() {}
	void recMOV_S() {}
	void recNEG_S() {}
	void recRSQRT_S() {}
	void recADDA_S() {}
	void recSUBA_S() {}
	void recMULA_S() {}
	void recMADD_S() {}
	void recMSUB_S() {}
	void recMADDA_S() {}
	void recMSUBA_S() {}
	void recCVT_S() {}
	void recCVT_W() {}
	void recMAX_S() {}
	void recMIN_S() {}
	void recBC1F() {}
	void recBC1T() {}
	void recBC1FL() {}
	void recBC1TL() {}
} // namespace R5900::Dynarec::OpcodeImpl::COP1

u32 R5900::Interpreter::OpcodeImpl::COP1::eeDivideSignificandArm64(u32, u32, u64, u32)
{
	return 0;
}

bool SaveStateBase::vuJITFreeze()
{
	return true;
}

template <int idx> void dVifUnpack(const u8*, bool) {}
template void dVifUnpack<0>(const u8*, bool);
template void dVifUnpack<1>(const u8*, bool);
void dVifReset(int) {}
void vuAotNoteGlobalSetting(bool) {}
void vuAotNoVmSettingsApplied(bool, bool) {}
void vuAotSettingChanged(bool, bool, bool) {}
void vuAotNoteOutsideStatusRead() {}
void vuAotResetStickyTripwire() {}
void vuAotSetTitle(const std::string&, u32) {}
