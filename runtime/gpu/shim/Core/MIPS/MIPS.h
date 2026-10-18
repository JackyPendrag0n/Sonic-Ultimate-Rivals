// Shim for PPSSPP's Core/MIPS/MIPS.h (rcsm GPU extraction). The GPU code only passes currentMIPS
// to CoreTiming::GetTicks(), which ignores it. stubs.cpp defines it weakly, because the runtime's
// VFPU glue already has a (strong) `currentMIPS`. GPL-2.0+.
#pragma once
#include "ppsspp_config.h"
#include <cstddef>
#include "Common/CommonTypes.h"
#include "Core/Opcode.h"
class MIPSState;
extern MIPSState *currentMIPS;
typedef Memory::Opcode MIPSOpcode;
