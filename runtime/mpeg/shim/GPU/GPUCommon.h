// Shim for PPSSPP's GPU/GPUCommon.h (rcsm mpeg extraction). sceMpeg only tells the GPU where it wrote
// decoded frames (PerformWriteFormattedFromMemory): forwarded to libpspgpu.a's C API, which the OpenGL
// backend needs to see movie frames written into framebuffers (a weak no-op in mpeg_hle.cpp stands in
// when libpspgpu.a isn't linked). Renamed so it can't collide with libpspgpu.a's `gpu`. GPL-2.0+.
#pragma once
#include <list>
#include <vector>
#include "Common/CommonTypes.h"
#include "GPU/ge_constants.h"
extern "C" void ge_write_formatted_from_memory(uint32_t addr, int size, int width, int format);
class MpegGpuHooks {
public:
	void PerformWriteFormattedFromMemory(u32 addr, int size, int width, GEBufferFormat format) {
		ge_write_formatted_from_memory(addr, size, width, (int)format);
	}
};
#define gpu mpeg_gpu_hooks
extern MpegGpuHooks *gpu;
