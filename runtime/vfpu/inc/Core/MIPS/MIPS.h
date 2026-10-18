#pragma once
#include "Common/CommonTypes.h"
#include "Common/Common.h"
#include "Core/Opcode.h"
#include "../../../../rt.h"
typedef Memory::Opcode MIPSOpcode;
extern u8 voffset[128];
extern u8 fromvoffset[128];
extern const float cst_constants[32];
struct MIPSState : public Ctx {
	u8 VfpuWriteMask() const { return (vfpuCtrl[VFPU_CTRL_DPREFIX] >> 8) & 0xF; }
	bool VfpuWriteMask(int i) const { return (vfpuCtrl[VFPU_CTRL_DPREFIX] >> (8 + i)) & 1; }
};
extern MIPSState *currentMIPS;
enum VCondition { VC_FL, VC_EQ, VC_LT, VC_LE, VC_TR, VC_NE, VC_GE, VC_GT, VC_EZ, VC_EN, VC_EI, VC_ES, VC_NZ, VC_NN, VC_NI, VC_NS };
enum MIPSGPReg { MIPS_REG_ZERO = 0, MIPS_REG_RA = 31 };
#include <string>
inline std::string MIPSDisasmAt(u32) { return "?"; }
