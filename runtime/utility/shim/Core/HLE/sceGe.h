// Shim for PPSSPP's Core/HLE/sceGe.h (rcsm utility extraction): PspGeListArgs (from PPSSPP) for PPGe, which
// enqueues its display list with libpspgpu.a's sceGeListEnQueue (sce_ge.cpp). GPL-2.0+.
#pragma once
#include "Common/CommonTypes.h"
#include "Common/Swap.h"
#include "Core/MemMap.h"
typedef u32_le SceSize_le;
struct PspGeListArgs
{
	SceSize_le size;
	PSPPointer<u32_le> context;
	u32_le numStacks;
	u32_le stackAddr;
};
u32 sceGeListEnQueue(u32 listAddress, u32 stallAddress, int callbackId, u32 optParamAddr);
