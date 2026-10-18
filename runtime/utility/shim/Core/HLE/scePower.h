// Shim for PPSSPP's Core/HLE/scePower.h (rcsm utility extraction). See sceKernelMemory.h for the
// volatile memory lock. GPL-2.0+.
#pragma once
#include "Core/HLE/sceKernelMemory.h"
inline int PowerScaleFromDefaultClock(int us) { return us; }
