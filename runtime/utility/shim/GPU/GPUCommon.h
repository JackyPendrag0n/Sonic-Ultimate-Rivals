// Shim for PPSSPP's GPU/GPUCommon.h (rcsm utility extraction). PPGe only switches GE interrupts off
// around its own list and ages its text cache by flips; both go to libpspgpu.a's C API. GPL-2.0+.
#pragma once
#include "Common/CommonTypes.h"
#include "GPU/ge_constants.h"
extern "C" void ge_soft_enable_interrupts(int enable);
class UtilityGpuHooks {
public:
	void EnableInterrupts(bool enable) { ge_soft_enable_interrupts(enable ? 1 : 0); }
};
#define gpu utility_gpu_hooks
extern UtilityGpuHooks *gpu;
struct UtilityGpuStats {
	struct { int numFlips = 0; } totals;
};
#define gpuStats utility_gpuStats
extern UtilityGpuStats gpuStats;
