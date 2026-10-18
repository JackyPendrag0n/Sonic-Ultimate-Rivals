// Shim for PPSSPP's Core/HLE/HLE.h (rcsm GPU extraction). GPL-2.0+.
#pragma once
#include <cstdio>
#include <cstdarg>
#include "Common/CommonTypes.h"
#include "Common/Log.h"
#include "Core/MIPS/MIPS.h"
#include "Core/ConfigValues.h"
inline void hleEatCycles(int) {}
inline void hleReSchedule(const char *) {}
inline void hleCoreTimingForceCheck() {}
void hleSplitSyscallOverGe();
