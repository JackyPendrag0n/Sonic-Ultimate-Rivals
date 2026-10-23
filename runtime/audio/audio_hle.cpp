// rcsm glue for PPSSPP's SAS mixer (sceSasCore) and ATRAC3/ATRAC3+ (sceAtrac3plus) HLE.
//
// PPSSPP's sceSas.cpp and sceAtrac.cpp are compiled unmodified. Their HLEFunction tables (built with
// PPSSPP's FunctionWrappers) are collected by RegisterHLEModule() below and registered with the rcsm
// kernel through one trampoline per function, which points the PARAM/RETURN macros (shim HLE.h) at
// the calling thread's Ctx, runs the function, fires the CoreTiming events it scheduled, and then
// applies hleDelayResult() as a real thread delay (the time the Media Engine would take).
//
// SAS mixes synchronously on the calling guest thread (no separate SAS thread). Atrac contexts live
// in a kernel-allocated BSS, as if libatrac3plus.prx had been loaded.
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU
// General Public License as published by the Free Software Foundation, version 2.0 or later versions.
// See LICENSE.TXT. Parts are derived from PPSSPP (Copyright (c) 2012- PPSSPP Project).

#include <algorithm>
#include <cmath>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "Common/Log.h"
#include "Core/Config.h"
#include "Core/CoreTiming.h"
#include "Core/MemMap.h"
#include "Core/System.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/ErrorCodes.h"
#include "Core/HLE/sceAtrac.h"
#include "Core/HLE/sceAudiocodec.h"
#include "Core/HLE/sceKernelInterrupt.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/HLE/sceSas.h"
#include "Core/HLE/sceVideocodec.h"
#include "audio_hle.h"
#include "hle_bridge.h"

#include "stubs/kernel_bridge.h"  // last: it defines short macros (A, RET, LW, ...)

void Register_sceSasCore();
void Register_sceAtrac3plus();
void __AtracInit();
void __AtracNotifyLoadModule(int version, u32 crc, u32 bssAddr, int bssSize);

static_assert(offsetof(Ctx, r) == offsetof(AudioHleRegs, r), "Ctx layout");
static_assert(offsetof(Ctx, f) == offsetof(AudioHleRegs, f), "Ctx layout");

// ================================================================= settings / small kernel hooks
AudioConfig g_Config;
static AudioCoreParameter g_coreParameter;
AudioCoreParameter &PSP_CoreParameter() { return g_coreParameter; }
AudioKernelMemory kernelMemory;

static bool g_delays = true;  // PSPAUDIO_NODELAY=1 turns hleDelayResult into a no-op

// Shared with the GPU library (same definitions there): weak so the two link together.
__attribute__((weak)) int CPU_HZ = 222000000;
__attribute__((weak)) bool __IsInInterrupt() { return g_in_intr; }
__attribute__((weak)) bool __InterruptsEnabled() { return true; }
__attribute__((weak)) bool __KernelIsDispatchEnabled() { return true; }
__attribute__((weak)) int sceKernelGetCompiledSdkVersion() { return 0x06060010; }

SceUID __KernelGetCurThread() { return g_cur ? (SceUID)g_cur->uid : 0; }
// sceSas's own wait/resume path (sasMixFinish) is never used: see AudioCoreTiming::RegisterEvent.
SceUID __KernelGetWaitID(SceUID threadID, WaitType type, u32 &error) { error = 1; return 0; }
u32 __KernelGetWaitValue(SceUID threadID, u32 &error) { error = 1; return 0; }
void __KernelResumeThreadFromWait(SceUID threadID, u64 retval) {}
void __KernelReSchedule(const char *reason) {}
void __KernelWaitCurThread(WaitType type, SceUID waitId, u32 waitValue, u32 timeoutPtr, bool processCallbacks, const char *reason) {
	ERROR_LOG(Log::HLE, "unexpected kernel wait (%s)", reason);
}

u32 AudioKernelMemory::Alloc(u32 &size, bool fromTop, const char *tag) {
	size = (size + 15) & ~15u;
	return kalloc(size);
}
bool AudioKernelMemory::Free(u32 position) { return true; }  // kalloc memory is permanent

// From PPSSPP's Core/HLE/sceAudiocodec.cpp (libatrac3plus's ATRAC3 layouts).
struct At3Param {
	u8 param;
	u16 bytes;
	u8 channels;
	u8 jointStereo;
};
static const At3Param at3Params[] = {
	{ 0x04, 0x0180, 2, 0 },
	{ 0x06, 0x0130, 2, 0 },
	{ 0x0B, 0x00C0, 2, 1 },
	{ 0x0E, 0x00C0, 1, 0 },
	{ 0x0F, 0x0098, 1, 0 },
};
bool Atrac3DecoderChannels(int bytesPerFrame, bool jointStereo, int *channels) {
	for (int i = ARRAY_SIZE(at3Params) - 1; i >= 0; i--) {
		if (at3Params[i].bytes == bytesPerFrame && (at3Params[i].jointStereo != 0) == jointStereo) {
			*channels = at3Params[i].channels;
			return true;
		}
	}
	return false;
}
bool IsAtrac3StreamJointStereo(int codecType, int bytesPerFrame, int channels) {
	if (codecType != PSP_CODEC_AT3)
		return false;
	for (const At3Param &p : at3Params) {
		if (p.bytes == bytesPerFrame && p.channels == channels)
			return p.jointStereo != 0;
	}
	return false;
}

// From PPSSPP's Core/HLE/sceAudiocodec.cpp: Media Engine time for codec init / one frame.
int AudioCodecInitUs(int codec, bool monoAt3Plus) {
	switch (codec) {
	case PSP_CODEC_AT3PLUS: return monoAt3Plus ? 524 : 646;
	case PSP_CODEC_AT3: return 210;
	case PSP_CODEC_MP3: return 517;
	default: return 230;
	}
}
int AudioCodecDecodeUs(int codec, int channels, int frameBytes) {
	switch (codec) {
	case PSP_CODEC_AT3PLUS:
		return channels == 1 ? 1500 + frameBytes * 84 / 100 : 1815 + frameBytes * 158 / 100;
	case PSP_CODEC_AT3:
		return channels == 1 ? 685 : 970 + frameBytes * 44 / 100;
	default:
		return 0;
	}
}

// From PPSSPP's Core/HLE/sceVideocodec.cpp: the Media Engine runs one job after another.
static s64 g_meBusyUntilUs;
int MEScheduleJob(int us) {
	const s64 now = (s64)now_us();
	const s64 start = std::max(now, g_meBusyUntilUs);
	g_meBusyUntilUs = start + us;
	return (int)(g_meBusyUntilUs - now);
}

// ================================================================= CoreTiming (audio's own)
namespace CoreTiming {
struct EventType {
	TimedCallback callback;
	std::string name;
};
static std::vector<EventType> eventTypes;
struct PendingEvent {
	s64 time;
	u64 userdata;
	int type;
	u64 seq;
};
static std::vector<PendingEvent> pending;
static u64 seq;

u64 GetTicks(MIPSState *) { return now_us() * (u64)(CPU_HZ / 1000000); }
u64 GetGlobalTimeUs() { return now_us(); }

int RegisterEvent(const char *name, TimedCallback callback) {
	// sceSas would block the thread on its own event until the mix is "done"; without the event it
	// uses hleDelayResult instead, which the trampoline turns into a thread delay. Same effect.
	if (!strcmp(name, "SasMix"))
		return -1;
	eventTypes.push_back(EventType{callback, name});
	return (int)eventTypes.size() - 1;
}
void RestoreRegisterEvent(int &event_type, const char *name, TimedCallback callback) {}

void ScheduleEvent(s64 cyclesIntoFuture, int event_type, u64 userdata) {
	if (event_type < 0 || event_type >= (int)eventTypes.size())
		return;
	pending.push_back(PendingEvent{(s64)GetTicks(nullptr) + cyclesIntoFuture, userdata, event_type, ++seq});
}

s64 UnscheduleEvent(int event_type, u64 userdata) {
	for (auto it = pending.begin(); it != pending.end(); ++it) {
		if (it->type == event_type && it->userdata == userdata) {
			s64 left = it->time - (s64)GetTicks(nullptr);
			pending.erase(it);
			return left;
		}
	}
	return 0;
}

// Events are only ever scheduled to land before the calling thread resumes (e.g. Atrac's output
// write "just ahead of the thread waking up"), so they run when the HLE call returns.
static void RunPending() {
	while (!pending.empty()) {
		auto it = std::min_element(pending.begin(), pending.end(), [](const PendingEvent &a, const PendingEvent &b) {
			return a.time != b.time ? a.time < b.time : a.seq < b.seq;
		});
		PendingEvent ev = *it;
		pending.erase(it);
		eventTypes[ev.type].callback(ev.userdata, 0);
	}
}
}  // namespace CoreTiming

// ================================================================= HLE plumbing
AudioHleRegs *g_audioHleRegs;
static const HLEFunction *g_curFunc;
static int g_pendingDelayUs;

u32 hleDelayResult(u32 result, const char *reason, int usec) {
	g_pendingDelayUs += usec;
	return result;
}
u64 hleDelayResult(u64 result, const char *reason, int usec) {
	g_pendingDelayUs += usec;
	return result;
}
void hleReSchedule(const char *reason) {
	if (!g_in_intr)
		thread_resched();
}

void hleDoLogInternal(Log t, LogLevel level, u64 res, const char *file, int line, const char *reportTag, const char *reason, const char *formatted_reason) {
	int lvl = level <= LogLevel::LERROR ? 0 : level <= LogLevel::LWARNING ? 1 : level <= LogLevel::LDEBUG ? 2 : 3;
	if (lvl > g_loglevel)
		return;
	char args[256] = "";
	if (g_curFunc && g_audioHleRegs) {
		int n = 0;
		for (int i = 0; g_curFunc->argmask && g_curFunc->argmask[i] && i < 8; i++)
			n += snprintf(args + n, sizeof(args) - n, "%s%x", i ? ", " : "", g_audioHleRegs->r[4 + i]);
	}
	logf_(lvl, "%s(%s) = %08x%s", g_curFunc ? g_curFunc->name : "?", args, (u32)res, formatted_reason ? formatted_reason : "");
}

static std::vector<const HLEFunction *> g_funcs;

void RegisterHLEModule(std::string_view name, int numFunctions, const HLEFunction *funcTable) {
	for (int i = 0; i < numFunctions; i++) {
		bool dup = false;
		for (const HLEFunction *f : g_funcs)
			dup |= f->ID == funcTable[i].ID;
		if (!dup)
			g_funcs.push_back(&funcTable[i]);  // entries without a function: "unimplemented", return 0
	}
}

// ---------------------------------------------------------------- guest calls from HLE (hleEnqueueCall)
// PPSSPP runs these after the syscall returns, then the PSPAction; we do the same, synchronously on the
// calling thread with run_guest(). Used by sceMpegRingbufferPut's fill callback.
struct PendingCall {
	u32 func;
	u32 args[6];
	PSPAction *after;
};
static std::vector<PendingCall> g_enqueuedCalls;
static bool g_haveCallRet;
static u32 g_callRet;

void hleEnqueueCall(u32 func, int argc, const u32 *argv, PSPAction *afterAction) {
	PendingCall pc{func, {0, 0, 0, 0, 0, 0}, afterAction};
	for (int i = 0; i < argc && i < 6; i++)
		pc.args[i] = argv[i];
	g_enqueuedCalls.push_back(pc);
}
void MipsCall::setReturnValue(u32 value) {
	g_haveCallRet = true;
	g_callRet = value;
}
void MipsCall::setReturnValue(u64 value) {
	setReturnValue((u32)value);
}

static std::vector<PSPAction *(*)()> g_actionTypes;
int __KernelRegisterActionType(PSPAction *(*creator)()) {
	g_actionTypes.push_back(creator);
	return (int)g_actionTypes.size() - 1;
}
void __KernelRestoreActionType(int actionType, PSPAction *(*creator)()) {}
PSPAction *__KernelCreateAction(int actionType) {
	PSPAction *a = g_actionTypes[actionType]();
	a->actionTypeID = actionType;
	return a;
}

static void RunEnqueuedCalls(Ctx *c) {
	while (!g_enqueuedCalls.empty()) {
		// Take this round's calls: guest code may make (nested) HLE calls that enqueue their own.
		std::vector<PendingCall> calls;
		calls.swap(g_enqueuedCalls);
		for (size_t i = 0; i < calls.size(); i++) {
			const PendingCall &pc = calls[i];
			u32 ret = run_guest(c, pc.func, pc.args[0], pc.args[1], pc.args[2]);
			if (pc.after) {
				// The action reads the guest function's v0 through currentMIPS.
				AudioHleRegs regs{};
				regs.r[2] = ret;
				MIPSState *savedMips = currentMIPS;
				currentMIPS = (MIPSState *)&regs;
				MipsCall call;
				call.entryPoint = pc.func;
				call.doAfter = pc.after;
				pc.after->run(call);
				currentMIPS = savedMips;
				delete pc.after;
			}
		}
	}
}

static u32 g_atracBss;

static void RunHle(int index, Ctx *c) {
	const HLEFunction *f = g_funcs[index];
	if ((f->flags & HLE_NOT_IN_INTERRUPT) && g_in_intr) {
		RET(SCE_KERNEL_ERROR_ILLEGAL_CONTEXT);
		return;
	}
	if (!g_atracBss && !strncmp(f->name, "sceAtrac", 8)) {
		// As if libatrac3plus.prx was loaded: 6 contexts of 256 bytes.
		g_atracBss = kalloc(6 * 256);
		__AtracNotifyLoadModule(0x0105, 0, g_atracBss, 6 * 256);
	}
	// Everything per-call is saved: guest callbacks run from here may make nested HLE calls.
	AudioHleRegs *savedRegs = g_audioHleRegs;
	const HLEFunction *savedFunc = g_curFunc;
	const int savedDelay = g_pendingDelayUs;
	const bool savedHaveRet = g_haveCallRet;
	const u32 savedRet = g_callRet;
	std::vector<PendingCall> savedCalls;
	savedCalls.swap(g_enqueuedCalls);

	g_audioHleRegs = (AudioHleRegs *)c;
	g_curFunc = f;
	g_pendingDelayUs = 0;
	g_haveCallRet = false;
	if (f->func) {
		f->func();
	} else {
		static std::vector<u32> warned;
		if (std::find(warned.begin(), warned.end(), f->ID) == warned.end()) {
			warned.push_back(f->ID);
			LOGI("unimplemented HLE function %s (%08x), returning 0", f->name, f->ID);
		}
		RET(0);
	}
	if ((f->flags & HLE_CLEAR_STACK_BYTES) && f->stackBytesToClear) {
		// Like PPSSPP: the firmware function would have used (and left zeros in) this much stack.
		const u32 sp = c->r[29];
		memset(PTR(sp - f->stackBytesToClear), 0, f->stackBytesToClear);
	}
	const int delay = g_pendingDelayUs;
	RunEnqueuedCalls(c);
	if (g_haveCallRet)
		c->r[2] = g_callRet;
	CoreTiming::RunPending();

	g_audioHleRegs = savedRegs;
	g_curFunc = savedFunc;
	g_pendingDelayUs = savedDelay;
	g_haveCallRet = savedHaveRet;
	g_callRet = savedRet;
	g_enqueuedCalls.swap(savedCalls);
	if (delay > 0 && g_delays && g_cur && !g_in_intr)
		thread_delay_until(now_us() + delay);
}

// One HleFn per registered function: HleFn has no user data.
static constexpr int MAX_FUNCS = 512;
template <int I>
static void Tramp(Ctx *c) { RunHle(I, c); }
template <int... I>
static constexpr std::array<HleFn, sizeof...(I)> MakeTramps(std::integer_sequence<int, I...>) {
	return {{&Tramp<I>...}};
}

static size_t g_registered;

// Registers the HLE functions collected by RegisterHLEModule() since the last call (hle_bridge.h).
int hle_bridge_register_pending(void) {
	static const auto tramps = MakeTramps(std::make_integer_sequence<int, MAX_FUNCS>());
	if ((int)g_funcs.size() > MAX_FUNCS)
		fatal("hle bridge: too many HLE functions (%d)", (int)g_funcs.size());
	std::vector<HleEnt> ents;
	for (size_t i = g_registered; i < g_funcs.size(); i++)
		ents.push_back(HleEnt{g_funcs[i]->ID, g_funcs[i]->name, tramps[i]});
	g_registered = g_funcs.size();
	hle_register(ents.data(), (int)ents.size());
	return (int)ents.size();
}

// Shared setup for every library using the bridge.
void hle_bridge_init(void) {
	static bool inited;
	if (inited)
		return;
	inited = true;
	Memory::base = g_mem;
	if (const char *e = getenv("PSPAUDIO_NODELAY"))
		g_delays = atoi(e) == 0;
	LogLevel level = g_loglevel >= 3 ? LogLevel::LVERBOSE : g_loglevel >= 2 ? LogLevel::LDEBUG : g_loglevel >= 1 ? LogLevel::LWARNING : LogLevel::LERROR;
	for (LogChannel &ch : g_log)
		ch.level = level;
}

void audio_hle_init(void) {
	static bool inited;
	if (inited)
		return;
	inited = true;
	hle_bridge_init();
	__SasInit();
	__AtracInit();
	Register_sceSasCore();
	Register_sceAtrac3plus();
	int n = hle_bridge_register_pending();
	LOGI("audio: PPSSPP SAS + Atrac HLE, %d functions", n);
}

// Support functions only this library needs (the rest of its support code is in stubs/, shared with libpspgpu.a).
// From PPSSPP's Core/Config.cpp.
float Volume100ToMultiplier(int volume) {
	if (volume > 100)
		return volume / 100.0f;
	return powf(volume * 0.01f, 1.75f);
}

namespace Memory {
void WriteOrException_U32(const u32 data, const u32 address) {
	if (!IsValid4AlignedAddress(address))
		ERROR_LOG(Log::MemMap, "unknown memory write %08x", address);
	WriteUnchecked_U32(data, address);
}
}  // namespace Memory
