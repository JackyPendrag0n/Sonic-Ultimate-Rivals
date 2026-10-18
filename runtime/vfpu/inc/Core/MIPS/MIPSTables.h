#pragma once
#include "Core/MIPS/MIPS.h"
struct MIPSInstruction;
const MIPSInstruction *MIPSGetInstruction(MIPSOpcode op);
