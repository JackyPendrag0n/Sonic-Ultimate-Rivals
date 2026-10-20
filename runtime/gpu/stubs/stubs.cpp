// Stand-ins for the parts of PPSSPP that the GE list processor and the software renderer link against
// but that the rcsm runtime doesn't have or need: logging/reporting, the GE debugger and recorder,
// savestates, hardware-renderer framebuffer/texture managers, presentation, PPGe, memory info, and
// the Memory:: globals (mapped onto the runtime's g_mem).
//
// This file is part of the rcsm GPU extraction and, like the PPSSPP sources it links with, is
// licensed under the GPL 2.0 or later (see ../LICENSE.TXT).

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <sys/mman.h>

#include "Common/Log.h"
#include "Common/LogReporting.h"
#include "Common/MemoryUtil.h"
#include "Common/Serialize/Serializer.h"
#include "Common/Data/Text/StringWriter.h"
#include "Common/GPU/thin3d.h"
#include "Common/GPU/OpenGL/GLFeatures.h"
#include "Common/Math/math_util.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "Core/MemMap.h"
#include "Core/MemMapHelpers.h"
#include "Core/Debugger/MemBlockInfo.h"
#include "Core/Util/PPGeDraw.h"
#include "Core/MIPS/JitCommon/JitCommon.h"
#include "GPU/GPU.h"
#include "GPU/GPUCommon.h"
#include "GPU/GPUState.h"
#include "GPU/Common/GPUDebugInterface.h"
#include "GPU/Common/PresentationCommon.h"
#include "GPU/Common/PostShader.h"
#include "GPU/Common/FramebufferManagerCommon.h"
#include "GPU/Common/TextureCacheCommon.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/Debugger/Breakpoints.h"
#include "GPU/Debugger/Debugger.h"
#include "GPU/Debugger/Record.h"
#include "GPU/Debugger/Stepping.h"

#include "gpu_glue.h"

// ------------------------------------------------------------------ globals
// From GPU/GPU.cpp.
GPUStatistics gpuStats;
GPUCommon *gpu;

Config g_Config;
bool g_coreCollectDebugStats = false;
bool g_DoubleTextureCoordinates = false;
// Weak: libpsputility.a (sceUtility + PPGe) has the real ones.
__attribute__((weak)) bool currentDialogActive = false;
__attribute__((weak)) u32 g_ppgeAtlasFakeSize = 0;
// The runtime's VFPU glue defines the real (strong) one; the GPU only passes it around.
__attribute__((weak)) MIPSState *currentMIPS = nullptr;

static CoreParameter g_coreParameter;
CoreParameter &PSP_CoreParameter() { return g_coreParameter; }
GPUBackend GetGPUBackend() { return GPUBackend::OPENGL; }
void Compatibility::Clear() { flags_ = {}; vrCompat_ = {}; ignored_.clear(); activeList_.clear(); }

// ------------------------------------------------------------------ logging
static bool g_logEnabled = true;
bool *g_bLogEnabledSetting = &g_logEnabled;
LogChannel g_log[(size_t)Log::NUMBER_OF_LOGS];

void GenericLog(Log type, LogLevel level, const char *file, int line, const char *fmt, ...) {
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	size_t n = strlen(buf);
	while (n && buf[n - 1] == '\n') buf[--n] = 0;
	int lvl;
	switch (level) {
	case LogLevel::LERROR: lvl = 0; break;
	case LogLevel::LWARNING: case LogLevel::LNOTICE: lvl = 1; break;
	case LogLevel::LINFO: case LogLevel::LDEBUG: lvl = 2; break;
	default: lvl = 3; break;
	}
	gpu_log(lvl, buf);
}

bool HandleAssert(bool isDebugAssert, const char *function, const char *file, int line, const char *expression, const char *format, ...) {
	char buf[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	char msg[1400];
	snprintf(msg, sizeof(msg), "GE assert (%s) in %s (%s:%d): %s", expression, function, file, line, buf);
	gpu_log(0, msg);
	return true;  // continue
}

namespace Reporting {
// Like PPSSPP's: true for the first n calls per identifier (WARN_LOG_ONCE etc.).
bool ShouldLogNTimes(const char *identifier, int n) {
	static std::mutex lock;
	static std::map<std::string, int> counts;
	std::lock_guard<std::mutex> guard(lock);
	return counts[identifier]++ < n;
}
void ReportMessage(const char *message, ...) {}
void ReportMessageFormatted(const char *message, const char *formatted) {}
}

// ------------------------------------------------------------------ memory
namespace Memory {
u8 *base = nullptr;
u32 g_MemorySize = 0x04000000;   // 0x0A000000+ is used for extra racers
u32 g_PSPModel = 0;
u32 g_UserPartitionSize = 0;
int g_depthTranslationIndex = DepthTranslationIndex(0x400);

bool DepthMirrorsActive() { return true; }
u32 DepthMirroredSlow(u32 address) {
	const DepthLayout &layout = DEPTH_LAYOUTS[(address >> 22) & 1][g_depthTranslationIndex];
	return (address & 0x40000000) | 0x04000000 | layout.Stored(address & 0x001FFFFF);
}

// The host reservation covers the whole masked address space, so these never fault; invalid
// addresses are still reported because they usually mean a broken display list.
u8 *GetPointerWriteOrException(const u32 address) {
	if (!IsValidAddress(address) && !IsPPGEAtlasFakeAddress(address, nullptr))
		ERROR_LOG(Log::MemMap, "GPU: unknown memory access %08x", address);
	return GetPointerWriteUnchecked(address);
}
const u8 *GetPointerOrException(const u32 address) {
	return GetPointerWriteOrException(address);
}
u8 *GetPointerWriteRangeOrException(const u32 address, const u32 size) {
	if (!IsValidRange(address, size))
		ERROR_LOG(Log::MemMap, "GPU: unknown memory range %08x+%x", address, size);
	return GetPointerWriteUnchecked(address);
}
const u8 *GetPointerRangeOrException(const u32 address, const u32 size) {
	return GetPointerWriteRangeOrException(address, size);
}
void Memset(const u32 address, const u8 value, const u32 length, const char *tag) {
	memset(GetPointerWriteUnchecked(address), value, length);
}
}  // namespace Memory

void PSPPointerNotifyRW(int rw, uint32_t ptr, uint32_t bytes, const char *tag, size_t tagLen) {}

void NotifyMemInfo(MemBlockFlags flags, uint32_t start, uint32_t size, const char *tag, size_t tagLength) {}
void NotifyMemInfoPC(MemBlockFlags flags, uint32_t start, uint32_t size, uint32_t pc, const char *tag, size_t tagLength) {}
void NotifyMemInfoCopy(uint32_t destPtr, uint32_t srcPtr, uint32_t size, const char *prefix, size_t prefixLen) {}
size_t FormatMemWriteTagAt(char *buf, size_t sz, const char *prefix, size_t prefixLen, uint32_t start, uint32_t size) {
	if (sz) buf[0] = 0;
	return 0;
}
bool MemBlockInfoDetailed() { return false; }

void *AllocateMemoryPages(size_t size, uint32_t memProtFlags) {
	int prot = 0;
	if (memProtFlags & MEM_PROT_READ) prot |= PROT_READ;
	if (memProtFlags & MEM_PROT_WRITE) prot |= PROT_WRITE;
	if (memProtFlags & MEM_PROT_EXEC) prot |= PROT_EXEC;
	void *p = mmap(nullptr, size, prot, MAP_ANON | MAP_PRIVATE, -1, 0);
	return p == MAP_FAILED ? nullptr : p;
}
void FreeMemoryPages(void *ptr, size_t size) {
	if (ptr) munmap(ptr, size);
}
bool ProtectMemoryPages(const void *ptr, size_t size, uint32_t memProtFlags) { return true; }
void *AllocateExecutableMemory(size_t size) {
	gpu_log(0, "GE: executable memory requested, but the vertex decoder JIT is disabled");
	return nullptr;
}
void FreeExecutableMemory(void *ptr, size_t size) {}
bool PlatformIsWXExclusive() { return false; }
void *AllocateAlignedMemory(size_t size, size_t alignment) {
	void *p = nullptr;
	if (alignment < sizeof(void *)) alignment = sizeof(void *);
	if (posix_memalign(&p, alignment, size) != 0) return nullptr;
	return p;
}
void FreeAlignedMemory(void *ptr) { free(ptr); }

// ------------------------------------------------------------------ JIT (disabled)
JittedVertexDecoder VertexDecoderJitCache::Compile(const VertexDecoder &dec, int32_t *jittedSize) {
	*jittedSize = 0;
	return nullptr;
}
std::vector<std::string> DisassembleX86(const u8 *data, int size) { return {}; }

// ------------------------------------------------------------------ savestates (unused)
PointerWrapSection PointerWrap::Section(const char *title, int minVer, int ver) {
	SetError(ERROR_FAILURE);
	return PointerWrapSection(*this, 0, title);
}
PointerWrapSection::~PointerWrapSection() {}
void PointerWrap::SetError(Error error_) {
	if (error < error_) error = error_;
	if (error > ERROR_WARNING) mode = PointerWrap::MODE_NOOP;
}
void PointerWrap::DoVoid(void *data, int size) {
	SetError(ERROR_FAILURE);
}

// ------------------------------------------------------------------ GE debugger / recorder (inactive)
GPUBreakpoints::GPUBreakpoints() {}
bool GPUBreakpoints::IsBreakpoint(u32 pc, u32 op) { return false; }
void GPUBreakpoints::AddCmdBreakpoint(u8 cmd, bool temp) {}
void GPUBreakpoints::ClearTempBreakpoints() {}
void GPUBreakpoints::AddTextureChangeTempBreakpoint() {}

namespace GPUStepping {
bool IsStepping() { return false; }
void ResumeFromStepping() {}
}

namespace GPUDebug {
bool ParsePrimRanges(std::string_view rule, std::vector<std::pair<int, int>> *output) { return false; }
}

namespace GPURecord {
void Recorder::NotifyCommand(u32 pc) {}
void Recorder::NotifyMemcpy(u32 dest, u32 src, u32 sz) {}
void Recorder::NotifyMemset(u32 dest, int v, u32 sz) {}
void Recorder::NotifyUpload(u32 dest, u32 sz) {}
void Recorder::NotifyDisplay(u32 addr, int stride, int fmt) {}
void Recorder::NotifyBeginFrame() {}
void Recorder::NotifyCPU() {}
}

void GPUDebugBuffer::Allocate(u32 stride, u32 height, GEBufferFormat fmt, bool flipped, bool reversed) {
	GPUDebugBufferFormat actualFmt = GPUDebugBufferFormat(fmt);
	if (reversed && actualFmt < GPU_DBG_FORMAT_8888)
		actualFmt |= GPU_DBG_FORMAT_REVERSE_FLAG;
	Allocate(stride, height, actualFmt, flipped);
}
void GPUDebugBuffer::Allocate(u32 stride, u32 height, GPUDebugBufferFormat fmt, bool flipped) {
	if (alloc_ && stride_ == stride && height_ == height && fmt_ == fmt) {
		flipped_ = flipped;
		return;
	}
	if (alloc_ && data_) delete[] data_;
	alloc_ = true;
	height_ = height;
	stride_ = stride;
	fmt_ = fmt;
	flipped_ = flipped;
	data_ = new u8[PixelSize() * stride * height];
}


u32 GPUDebugBuffer::PixelSize() const {  // from GPU/Common/GPUDebugInterface.cpp
	switch (fmt_) {
	case GPU_DBG_FORMAT_8888:
	case GPU_DBG_FORMAT_8888_BGRA:
	case GPU_DBG_FORMAT_FLOAT:
	case GPU_DBG_FORMAT_24BIT_8X:
	case GPU_DBG_FORMAT_24X_8BIT:
		return 4;
	case GPU_DBG_FORMAT_888_RGB:
		return 3;
	case GPU_DBG_FORMAT_8BIT:
		return 1;
	default:
		return 2;
	}
}

// ------------------------------------------------------------------ PPGe (libpsputility.a provides it)
__attribute__((weak)) void PPGeSetDrawContext(Draw::DrawContext *draw) {}
__attribute__((weak)) const u8 *PPGeAtlasGetData() { return nullptr; }

