// Shim for PPSSPP's Core/HLE/sceKernelMemory.h (rcsm utility extraction): PPGe's and the dialogs'
// kernel/user memory blocks come from a pool the utility library gets from the rcsm kernel, with a
// first-fit allocator (they allocate and free text images every frame). GPL-2.0+.
#pragma once
#include "Core/MemMap.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/ErrorCodes.h"

class UtilityMemoryPool {
public:
	// Returns (u32)-1 when out of memory, like PPSSPP's BlockAllocator.
	u32 Alloc(u32 &size, bool fromTop = false, const char *tag = nullptr);
	bool Free(u32 position);
	u32 GetLargestFreeBlockSize() const;
};
#define kernelMemory utility_kernelMemory
#define userMemory utility_userMemory
extern UtilityMemoryPool kernelMemory;
extern UtilityMemoryPool userMemory;

int sceKernelGetCompiledSdkVersion();
// The dialogs lock volatile memory while they run. The rcsm kernel doesn't arbitrate it (games may use
// it themselves), so these never take it and, importantly, never "use" (clear) it.
int KernelVolatileMemLock(int type, u32 paddr, u32 psize);
int KernelVolatileMemUnlock(int type);
