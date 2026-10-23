// Stand-ins for the PPSSPP support code the SAS/Atrac sources link against: logging, reporting,
// memory helpers on the runtime's g_mem, memory-info notifications, savestate stubs.
//
// build.sh makes every symbol defined here weak: the GPU library (libpspgpu.a) defines the same
// things with the same behaviour, and either copy may end up in the final binary.
//
// Part of the rcsm audio extraction; GPL-2.0+ like the PPSSPP sources it links with (../LICENSE.TXT).

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Common/Log.h"
#include "Common/LogReporting.h"
#include "Common/MemoryUtil.h"
#include "Common/StringUtils.h"
#include "Common/Serialize/Serializer.h"
#include "Common/Thread/ThreadUtil.h"
#include "Core/Compatibility.h"
#include "Core/ConfigValues.h"
#include "Core/MemMap.h"
#include "Core/MemMapHelpers.h"
#include "Core/Debugger/MemBlockInfo.h"

extern "C" void logf_(int lvl, const char *fmt, ...);
extern "C" int g_loglevel;

MIPSState *currentMIPS = nullptr;  // the runtime's VFPU glue has the real one

// ------------------------------------------------------------------ logging
static bool g_logEnabled = true;
bool *g_bLogEnabledSetting = &g_logEnabled;
LogChannel g_log[(size_t)Log::NUMBER_OF_LOGS];

static void LogLine(int lvl, const char *msg) {
	if (lvl <= g_loglevel)
		logf_(lvl, "%s", msg);
}

void GenericLog(Log type, LogLevel level, const char *file, int line, const char *fmt, ...) {
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	size_t n = strlen(buf);
	while (n && buf[n - 1] == '\n') buf[--n] = 0;
	int lvl = level == LogLevel::LERROR ? 0 : (level == LogLevel::LWARNING || level == LogLevel::LNOTICE) ? 1 : level == LogLevel::LVERBOSE ? 3 : 2;
	LogLine(lvl, buf);
}

bool HandleAssert(bool isDebugAssert, const char *function, const char *file, int line, const char *expression, const char *format, ...) {
	char buf[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	char msg[1400];
	snprintf(msg, sizeof(msg), "assert (%s) in %s (%s:%d): %s", expression, function, file, line, buf);
	LogLine(0, msg);
	return true;  // continue
}

namespace Reporting {
bool ShouldLogNTimes(const char *identifier, int n) { return true; }
void ReportMessage(const char *message, ...) {}
void ReportMessageFormatted(const char *message, const char *formatted) {}
}

void SetCurrentThreadName(const char *threadName) {}

std::string StringFromFormat(const char *format, ...) {
	va_list args;
	va_start(args, format);
	char *buf = nullptr;
	std::string s;
	if (vasprintf(&buf, format, args) >= 0 && buf) {
		s = buf;
		free(buf);
	}
	va_end(args);
	return s;
}

void DataToHexString(const uint8_t *data, size_t size, std::string *output, bool lineBreaks) {
	output->clear();
	char tmp[4];
	for (size_t i = 0; i < size; i++) {
		snprintf(tmp, sizeof(tmp), "%02x", data[i]);
		output->append(tmp);
		if (lineBreaks && (i & 15) == 15) output->push_back('\n');
		else if (i + 1 < size) output->push_back(' ');
	}
}

void Compatibility::Clear() { flags_ = {}; vrCompat_ = {}; ignored_.clear(); activeList_.clear(); }

// ------------------------------------------------------------------ memory (g_mem, masked)
namespace Memory {
u8 *base = nullptr;
u32 g_MemorySize = 0x02000000;
u32 g_PSPModel = 0;
u32 g_UserPartitionSize = 0;
int g_depthTranslationIndex = DepthTranslationIndex(0x400);

bool DepthMirrorsActive() { return true; }
u32 DepthMirroredSlow(u32 address) {
	const DepthLayout &layout = DEPTH_LAYOUTS[(address >> 22) & 1][g_depthTranslationIndex];
	return (address & 0x40000000) | 0x04000000 | layout.Stored(address & 0x001FFFFF);
}
u8 *GetPointerWriteOrException(const u32 address) {
	if (!IsValidAddress(address))
		ERROR_LOG(Log::MemMap, "unknown memory access %08x", address);
	return GetPointerWriteUnchecked(address);
}
const u8 *GetPointerOrException(const u32 address) {
	return GetPointerWriteOrException(address);
}
u8 *GetPointerWriteRangeOrException(const u32 address, const u32 size) {
	if (!IsValidRange(address, size))
		ERROR_LOG(Log::MemMap, "unknown memory range %08x+%x", address, size);
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

void *AllocateAlignedMemory(size_t size, size_t alignment) {
	void *p = nullptr;
	if (alignment < sizeof(void *)) alignment = sizeof(void *);
	if (posix_memalign(&p, alignment, size) != 0) return nullptr;
	return p;
}
#ifdef _WIN32
void FreeAlignedMemory(void *ptr) { _aligned_free(ptr); }
#else
void FreeAlignedMemory(void *ptr) { free(ptr); }
#endif

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
