// Shim for PPSSPP's Core/HLE/sceKernelMemory.h (rcsm audio extraction). Only the legacy Atrac
// context (used when no Atrac BSS is set up, which audio_hle_init always does) allocates from it.
// GPL-2.0+.
#pragma once
#include "Core/MemMap.h"
#define kernelMemory audio_kernelMemory
struct AudioKernelMemory {
	u32 Alloc(u32 &size, bool fromTop = false, const char *tag = nullptr);
	bool Free(u32 position);
};
extern AudioKernelMemory kernelMemory;
int sceKernelGetCompiledSdkVersion();
