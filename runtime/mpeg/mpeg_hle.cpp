// rcsm glue for PPSSPP's sceMpeg HLE (Core/HLE/sceMpeg.cpp + Core/HW/MediaEngine.cpp, FFmpeg-based).
//
// PPSSPP's sources are compiled unmodified against runtime/audio's shims and registered through the
// HLE bridge in libpspaudio.a (hle_bridge.h), which also runs sceMpegRingbufferPut's fill callback on
// the calling guest thread (hleEnqueueCall -> run_guest) and applies PPSSPP's decode delays.
// ATRAC3+ audio inside movies is decoded by libpspaudio.a's in-tree decoder.
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU
// General Public License as published by the Free Software Foundation, version 2.0 or later versions.
// See LICENSE.TXT. Parts are derived from PPSSPP (Copyright (c) 2012- PPSSPP Project).

#include <cstring>

#include <map>
#include <mutex>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}
#include "Common/CommonTypes.h"
#include "Core/HLE/sceMpeg.h"
#include "Core/HLE/sceKernelModule.h"
#include "GPU/GPUCommon.h"
#include "GPU/Common/TextureDecoder.h"
#include "../audio/hle_bridge.h"
#include "mpeg_hle.h"

#include "../audio/stubs/kernel_bridge.h"  // last: it defines short macros (A, RET, LW, ...)

void Register_sceMpeg();

static MpegGpuHooks g_gpuHooks;
MpegGpuHooks *gpu = &g_gpuHooks;
// libpspgpu.a's real one replaces this when it's linked (the runtime always links it).
extern "C" __attribute__((weak)) void ge_write_formatted_from_memory(uint32_t addr, int size, int width, int format) {}

// The ring buffer's gp (mpeg library 0x0105+): the caller's module gp.
u32 __KernelGetModuleGP(SceUID module) { return cur_module_gp(); }
SceUID __KernelGetCurThreadModuleId() { return 0; }

// From PPSSPP's GPU/Common/TextureDecoder.cpp (generic path). libpspgpu.a has the same function.
__attribute__((weak)) void DoSwizzleTex16(const u32 *ysrcp, u8 *texptr, int bxc, int byc, u32 pitch) {
	const u32 pitchBy32 = pitch >> 2;
	u32 *dest = (u32 *)texptr;
	for (int by = 0; by < byc; by++) {
		const u32 *xsrc = ysrcp;
		for (int bx = 0; bx < bxc; bx++) {
			const u32 *src = xsrc;
			for (int n = 0; n < 8; n++) {
				memcpy(dest, src, 16);
				src += pitchBy32;
				dest += 4;
			}
			xsrc += 4;
		}
		ysrcp += pitchBy32 * 8;
	}
}

// See shim/libavformat/avformat.h: FFmpeg >= 5 needs AVERROR_EOF where MediaEngine returns 0.
#undef avio_alloc_context
typedef int (*ReadPacketFn)(void *opaque, uint8_t *buf, int buf_size);
static std::map<void *, ReadPacketFn> g_readers;  // per MediaEngine (the opaque), so it can't grow
static std::mutex g_readersLock;

static int ReadPacketAdapter(void *opaque, uint8_t *buf, int size) {
	ReadPacketFn fn;
	{
		std::lock_guard<std::mutex> guard(g_readersLock);
		fn = g_readers[opaque];
	}
	int n = fn(opaque, buf, size);
	return n == 0 ? AVERROR_EOF : n;
}

extern "C" decltype(avio_alloc_context) rcsm_avio_alloc_context;
extern "C" AVIOContext *rcsm_avio_alloc_context(unsigned char *buffer, int buffer_size, int write_flag, void *opaque,
	ReadPacketFn read_packet, decltype(AVIOContext::write_packet) write_packet, int64_t (*seek)(void *opaque, int64_t offset, int whence)) {
	if (read_packet) {
		std::lock_guard<std::mutex> guard(g_readersLock);
		g_readers[opaque] = read_packet;
		read_packet = ReadPacketAdapter;
	}
	return avio_alloc_context(buffer, buffer_size, write_flag, opaque, read_packet, write_packet, seek);
}

void mpeg_hle_init(void) {
	static bool inited;
	if (inited)
		return;
	inited = true;
	hle_bridge_init();
	__MpegInit();
	__VideoPmpInit();
	Register_sceMpeg();
	int n = hle_bridge_register_pending();
	LOGI("mpeg: PPSSPP sceMpeg HLE (FFmpeg %s), %d functions", LIBAVCODEC_IDENT, n);
}

#include "fake_pmf.inc"

const unsigned char *mpeg_fake_pmf(unsigned *size) {
	if (size)
		*size = sizeof(g_fakePmf);
	return g_fakePmf;
}
