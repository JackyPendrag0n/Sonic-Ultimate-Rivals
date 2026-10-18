// Shim for PPSSPP's Core/HLE/HLE.h (rcsm audio extraction). Keeps PPSSPP's HLEFunction tables,
// argument/return macros and hleLog* helpers, but the registers are those of the rcsm runtime's Ctx
// for the call in progress (set by the trampolines in audio_hle.cpp), and hleDelayResult() asks the
// trampoline to delay the calling thread after the call returns.
//
// Derived from PPSSPP's Core/HLE/HLE.h, Copyright (c) 2012- PPSSPP Project. GPL-2.0+.
#pragma once

#include <cstdio>
#include <cstdarg>
#include <type_traits>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Common/Log.h"
#include "Core/MIPS/MIPS.h"
#include "Core/ConfigValues.h"

class PointerWrap;

typedef void (*HLEFunc)();

enum {
	HLE_NOT_IN_INTERRUPT = 1 << 8,
	HLE_NOT_DISPATCH_SUSPENDED = 1 << 9,
	HLE_CLEAR_STACK_BYTES = 1 << 10,
	HLE_KERNEL_SYSCALL = 1 << 11,
};

struct HLEFunction {
	u32 ID;
	HLEFunc func;
	const char *name;
	char retmask;
	const char *argmask;
	u32 flags;
	u32 stackBytesToClear;
};

// Register file of the guest call in progress: a prefix of the runtime's Ctx (u32 r[32], float f[32]).
struct AudioHleRegs {
	u32 r[32];
	float f[32];
};
extern AudioHleRegs *g_audioHleRegs;

#define PARAM(n) g_audioHleRegs->r[4 + (n)]
#define PARAM64(n) (g_audioHleRegs->r[4 + (n)] | ((u64)g_audioHleRegs->r[4 + (n) + 1] << 32))
#define PARAMF(n) g_audioHleRegs->f[12 + (n)]
#define RETURN(n) g_audioHleRegs->r[2] = (n);
#define RETURN64(n) {u64 RETURN64_tmp = n; g_audioHleRegs->r[2] = RETURN64_tmp & 0xFFFFFFFF; g_audioHleRegs->r[3] = RETURN64_tmp >> 32;}
#define RETURNF(fl) g_audioHleRegs->f[0] = fl

void RegisterHLEModule(std::string_view name, int numFunctions, const HLEFunction *funcTable);

class PSPAction;
// Calls guest code `func` on the calling thread once the HLE function returns, then afterAction.
void hleEnqueueCall(u32 func, int argc, const u32 *argv, PSPAction *afterAction = nullptr);

u32 hleDelayResult(u32 result, const char *reason, int usec);
u64 hleDelayResult(u64 result, const char *reason, int usec);
inline int hleDelayResult(int result, const char *reason, int usec) {
	return hleDelayResult((u32)result, reason, usec);
}
inline s64 hleDelayResult(s64 result, const char *reason, int usec) {
	return hleDelayResult((u64)result, reason, usec);
}
inline void hleEatCycles(int cycles) {}
inline void hleEatMicro(int usec) {}
inline void hleLeave() {}
void hleReSchedule(const char *reason);
inline void hleCoreTimingForceCheck() {}

void hleDoLogInternal(Log t, LogLevel level, u64 res, const char *file, int line, const char *reportTag, const char *reason, const char *formatted_reason);

template <bool leave, bool convert_code, typename T>
[[nodiscard]]
NO_INLINE
ATTR_FORMAT_PRINTF(7, 8) T hleDoLog(Log t, LogLevel level, T res, const char *file, int line, const char *reportTag, const char *reasonFmt, ...) {
	if (!GenericLogEnabled(t, level))
		return res;
	if (convert_code && (int)res >= 0)
		level = LogLevel::LDEBUG;
	char formatted_reason[4096] = {0};
	if (reasonFmt != nullptr) {
		va_list args;
		va_start(args, reasonFmt);
		formatted_reason[0] = ':';
		formatted_reason[1] = ' ';
		vsnprintf(formatted_reason + 2, sizeof(formatted_reason) - 3, reasonFmt, args);
		formatted_reason[sizeof(formatted_reason) - 1] = '\0';
		va_end(args);
	}
	u64 fmtRes = res;
	if (std::is_floating_point<T>::value)
		fmtRes = *(const u32 *)&res;
	else if (std::is_signed<T>::value)
		fmtRes = (s64)res;
	hleDoLogInternal(t, level, fmtRes, file, line, reportTag, reasonFmt, formatted_reason);
	return res;
}

template <bool leave, bool convert_code, typename T>
[[nodiscard]]
NO_INLINE
T hleDoLog(Log t, LogLevel level, T res, const char *file, int line, const char *reportTag) {
	if (convert_code && (int)res >= 0)
		level = LogLevel::LDEBUG;
	if (((int)level > MAX_LOGLEVEL || !GenericLogEnabled(t, level)) && !reportTag)
		return res;
	u64 fmtRes = res;
	if (std::is_floating_point<T>::value)
		fmtRes = *(const u32 *)&res;
	else if (std::is_signed<T>::value)
		fmtRes = (s64)res;
	hleDoLogInternal(t, level, fmtRes, file, line, reportTag, nullptr, "");
	return res;
}

template <typename T>
[[nodiscard]]
inline T hleNoLog(T t) {
	return t;
}
inline void hleNoLogVoid() {}

// HLE-to-HLE call (PPSSPP pushes a description for its logs; here it's a plain call).
#define hleCall(module, retType, funcName, ...) ((retType)funcName(__VA_ARGS__))

#define HLE_LOG_LDEBUG LogLevel::LDEBUG
#define HLE_LOG_LVERBOSE LogLevel::LVERBOSE

#define hleLogReturnHelper(convert, t, level, res, ...) \
	(((int)level <= MAX_LOGLEVEL) ? hleDoLog<true, convert>(t, level, (res), __FILE__, __LINE__, nullptr, ##__VA_ARGS__) : hleNoLog(res))

#define hleLogError(t, res, ...) hleLogReturnHelper(false, t, LogLevel::LERROR, res, ##__VA_ARGS__)
#define hleLogWarning(t, res, ...) hleLogReturnHelper(false, t, LogLevel::LWARNING, res, ##__VA_ARGS__)
#define hleLogDebug(t, res, ...) hleLogReturnHelper(false, t, HLE_LOG_LDEBUG, res, ##__VA_ARGS__)
#define hleLogInfo(t, res, ...) hleLogReturnHelper(false, t, LogLevel::LINFO, res, ##__VA_ARGS__)
#define hleLogVerbose(t, res, ...) hleLogReturnHelper(false, t, HLE_LOG_LVERBOSE, res, ##__VA_ARGS__)
#define hleLogDebugOrWarn(t, res, ...) hleLogReturnHelper(true, t, LogLevel::LWARNING, res, ##__VA_ARGS__)
#define hleLogDebugOrError(t, res, ...) hleLogReturnHelper(true, t, LogLevel::LERROR, res, ##__VA_ARGS__)
#define hleReportError(t, res, ...) hleDoLog<true, false>(t, LogLevel::LERROR, res, __FILE__, __LINE__, "", ##__VA_ARGS__)
#define hleReportWarning(t, res, ...) hleDoLog<true, false>(t, LogLevel::LWARNING, res, __FILE__, __LINE__, "", ##__VA_ARGS__)
