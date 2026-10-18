// Shim for PPSSPP's Core/HLE/sceKernelModule.h (rcsm mpeg extraction). GPL-2.0+.
#pragma once
#include "Common/CommonTypes.h"
typedef int SceUID;
u32 __KernelGetModuleGP(SceUID module);
SceUID __KernelGetCurThreadModuleId();
