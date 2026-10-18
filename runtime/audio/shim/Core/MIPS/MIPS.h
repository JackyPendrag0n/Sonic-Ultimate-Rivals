// Shim for PPSSPP's Core/MIPS/MIPS.h (rcsm audio extraction). HLE arguments come from the shim
// HLE.h's PARAM macros, not currentMIPS; the declaration only keeps PPSSPP headers compiling (the
// runtime's VFPU glue owns the real `currentMIPS`, stubs.cpp has a weak fallback). GPL-2.0+.
#pragma once
#include "ppsspp_config.h"
#include <cstddef>
#include "Common/CommonTypes.h"
#include "Core/Opcode.h"
enum MIPSGPReg { MIPS_REG_ZERO = 0, MIPS_REG_V0 = 2, MIPS_REG_V1 = 3, MIPS_REG_A0 = 4, MIPS_REG_GP = 28, MIPS_REG_SP = 29, MIPS_REG_RA = 31 };
// Only r[] is used: hle bridge points currentMIPS at a register file while a PSPAction runs.
class MIPSState {
public:
	u32 r[32];
};
extern MIPSState *currentMIPS;
typedef Memory::Opcode MIPSOpcode;
