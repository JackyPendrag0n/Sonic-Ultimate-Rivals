// Shim for PPSSPP's Core/CoreTiming.h (rcsm audio extraction). The audio code's events (Atrac's
// delayed output write) are run by audio_hle.cpp right after the HLE call that scheduled them. The
// namespace is renamed so it can't collide with the GPU library's CoreTiming. GPL-2.0+.
#pragma once
#include "Common/CommonTypes.h"
class PointerWrap;
class MIPSState;
extern int CPU_HZ;
inline s64 usToCycles(int us) { return (CPU_HZ / 1000000 * (s64)us); }
inline s64 usToCycles(s64 us) { return (CPU_HZ / 1000000 * us); }
inline s64 usToCycles(u64 us) { return (s64)(CPU_HZ / 1000000 * us); }
inline s64 cyclesToUs(s64 cycles) { return (cycles * 1000000) / CPU_HZ; }
#define CoreTiming AudioCoreTiming
namespace CoreTiming {
	typedef void (*TimedCallback)(u64 userdata, int cyclesLate);
	u64 GetTicks(MIPSState *mips);
	u64 GetGlobalTimeUs();
	int RegisterEvent(const char *name, TimedCallback callback);
	void RestoreRegisterEvent(int &event_type, const char *name, TimedCallback callback);
	void ScheduleEvent(s64 cyclesIntoFuture, int event_type, u64 userdata = 0);
	s64 UnscheduleEvent(int event_type, u64 userdata);
}
