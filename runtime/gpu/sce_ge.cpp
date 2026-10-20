// sceGe HLE for the rcsm runtime, on top of PPSSPP's GPUCommon list processor and SoftGPU.
//
// Ported from PPSSPP's Core/HLE/sceGe.cpp (Copyright (c) 2012- PPSSPP Project), with the interrupt
// manager, CoreTiming and thread waits replaced by the rcsm kernel:
//   * CoreTiming events (GE sync/interrupt) fire "instantly": they're queued with intr_queue_ex() and
//     run from the scheduler the next time it gets control. Ticks are 222 MHz CPU cycles derived from
//     now_us(), and never go back past an event that has fired, so a list is complete once its
//     completion event ran.
//   * GE interrupts are serialized like on the PSP: one at a time, the next is raised after the
//     previous handler returned. Signal/finish callbacks run as interrupt handlers (intr_queue_ex with
//     the gp sceGeSetCallback was called with), and `done` does PPSSPP's handleResult()
//     (InterruptEnd + continue the queue).
//   * sceGeListSync/sceGeDrawSync in wait mode block with thread_wait(W_GE, ...) exactly when PPSSPP
//     would wait, and are woken by __GeTriggerWait.
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU
// General Public License as published by the Free Software Foundation, version 2.0 or later versions.
// See LICENSE.TXT.

#include <algorithm>
#include <cstdlib>
#include <list>
#include <map>
#include <vector>

#include "Common/Thread/ThreadManager.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/ErrorCodes.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/sceGe.h"
#include "Core/HLE/sceKernelInterrupt.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/MemMap.h"
#include "Core/System.h"
#include "GPU/GPU.h"
#include "GPU/GPUCommon.h"
#include "GPU/GPUState.h"
#include "GPU/Software/SoftGpu.h"
#include "stubs/gpu_glue.h"

#include "stubs/kernel_bridge.h"  // last: it defines short macros (A, RET, LW, ...)

#include "ge_soft.h"
#include "ge_internal.h"

void gpu_log(int lvl, const char *msg) {
	if (lvl <= g_loglevel)
		logf_(lvl, "[ge] %s", msg);
}

// ================================================================= kernel hooks GPUCommon calls
static u32 g_sdkVersion = 0x06060010;
int sceKernelGetCompiledSdkVersion() { return (int)g_sdkVersion; }
bool __KernelIsDispatchEnabled() { return true; }
bool __IsInInterrupt() { return g_in_intr; }
bool __InterruptsEnabled() { return true; }

// GE interrupts that have been raised (their event fired) but not dispatched yet, because another
// GE interrupt's handler is still running.
static int geRaised;
static bool geInFlight;
int __CancelRaisedInterrupts(PSPInterrupt intno) {
	if (intno != PSP_GE_INTR)
		return 0;
	int n = geRaised;
	geRaised = 0;
	return n;
}

void hleSplitSyscallOverGe() {
	gpu->ProcessDLQueue();
}

// ================================================================= CoreTiming
int CPU_HZ = 222000000;

namespace CoreTiming {
static std::vector<EventType> eventTypes;
struct PendingEvent {
	s64 time;
	u64 userdata;
	int type;
	u64 seq;
};
static std::vector<PendingEvent> pendingEvents;
static u64 eventSeq;
static u64 tickFloor;
static bool pumpQueued;

u64 GetTicks(MIPSState *) {
	u64 t = now_us() * (u64)(CPU_HZ / 1000000);
	return t < tickFloor ? tickFloor : t;
}

int RegisterEvent(const char *name, TimedCallback callback) {
	eventTypes.push_back(EventType{callback, name});
	return (int)eventTypes.size() - 1;
}

static void Pump(void *, u32) {
	pumpQueued = false;
	while (!pendingEvents.empty()) {
		auto it = std::min_element(pendingEvents.begin(), pendingEvents.end(), [](const PendingEvent &a, const PendingEvent &b) {
			return a.time != b.time ? a.time < b.time : a.seq < b.seq;
		});
		PendingEvent ev = *it;
		pendingEvents.erase(it);
		// GPU work is instant: the event's time has now come.
		if (ev.time > 0 && (u64)ev.time > tickFloor)
			tickFloor = (u64)ev.time;
		eventTypes[ev.type].callback(ev.userdata, 0);
	}
}

void ScheduleEvent(s64 cyclesIntoFuture, int event_type, u64 userdata) {
	pendingEvents.push_back(PendingEvent{(s64)GetTicks(nullptr) + cyclesIntoFuture, userdata, event_type, ++eventSeq});
	if (!pumpQueued) {
		pumpQueued = true;
		intr_queue_ex(0, 0, 0, 0, 0, Pump, nullptr);
	}
}

s64 UnscheduleEvent(int event_type, u64 userdata) {
	for (auto it = pendingEvents.begin(); it != pendingEvents.end(); ++it) {
		if (it->type == event_type && it->userdata == userdata) {
			s64 left = it->time - (s64)GetTicks(nullptr);
			pendingEvents.erase(it);
			return left;
		}
	}
	return 0;
}
}  // namespace CoreTiming

// ================================================================= sceGe state
static const int LIST_ID_MAGIC = 0x35000000;

static PspGeCallbackData ge_callback_data[16];
static bool ge_used_callbacks[16];

// Sub-interrupt handlers of PSP_GE_INTR, as sceGeSetCallback registers them (subIntrBase | SIGNAL/FINISH).
struct GeSubIntr {
	bool registered;
	u32 func, arg, gp;
};
static GeSubIntr ge_subintr[32];

static std::map<int, std::vector<Thread *>> listWaitingThreads;
static std::vector<Thread *> drawWaitingThreads;

struct GeInterruptData {
	int listid;
	u32 pc;
	u32 cmd;
};
static std::list<GeInterruptData> ge_pending_cb;
static int geSyncEvent;
static int geInterruptEvent;

static inline u32 GeWaitId(GPUSyncType type, int id) {
	return type == GPU_SYNC_DRAW ? 0x10000u : (u32)id;
}

static void GeDispatchInterrupts();

// The second half of PPSSPP's GeIntrHandler: after the guest's signal/finish handler returned.
static void GeInterruptHandlerDone(void *, u32) {
	geInFlight = false;
	if (ge_pending_cb.empty()) {
		// sceGeBreak(1) from the handler cleared it.
		GeDispatchInterrupts();
		return;
	}
	GeInterruptData intrdata = ge_pending_cb.front();
	ge_pending_cb.pop_front();

	DisplayList *dl = gpu->getList(intrdata.listid);
	if (!dl || !dl->interruptsEnabled) {
		ERROR_LOG(Log::sceGe, "Unable to finish GE interrupt: list %d has interrupts disabled, should not happen", intrdata.listid);
		GeDispatchInterrupts();
		return;
	}

	switch (intrdata.cmd == GE_CMD_SIGNAL ? dl->signal : PSP_GE_SIGNAL_NONE) {
	case PSP_GE_SIGNAL_HANDLER_SUSPEND:
		if (sceKernelGetCompiledSdkVersion() <= 0x02000010) {
			if (dl->state != PSP_GE_DL_STATE_NONE && dl->state != PSP_GE_DL_STATE_COMPLETED) {
				dl->state = PSP_GE_DL_STATE_QUEUED;
			}
		}
		break;
	default:
		break;
	}

	gpu->InterruptEnd(intrdata.listid);
	gpu->ProcessDLQueue();
	GeDispatchInterrupts();
}

// PPSSPP's GeIntrHandler::run(). Returns true if a guest handler was queued.
static bool GeRunInterrupt() {
	if (ge_pending_cb.empty()) {
		DEBUG_LOG(Log::sceGe, "Ignoring GE interrupt, nothing pending anymore");
		return false;
	}

	GeInterruptData intrdata = ge_pending_cb.front();
	DisplayList *dl = gpu->getList(intrdata.listid);
	if (dl == NULL) {
		WARN_LOG(Log::sceGe, "Unable to run GE interrupt: list doesn't exist: %d", intrdata.listid);
		ge_pending_cb.pop_front();
		return false;
	}
	if (!dl->interruptsEnabled) {
		ERROR_LOG(Log::sceGe, "Unable to run GE interrupt: list has interrupts disabled, should not happen");
		ge_pending_cb.pop_front();
		return false;
	}

	gpu->InterruptStart(intrdata.listid);

	const u32 cmd = intrdata.cmd;
	int subintr = -1;
	if (dl->subIntrBase >= 0) {
		switch (dl->signal) {
		case PSP_GE_SIGNAL_SYNC:
		case PSP_GE_SIGNAL_JUMP:
		case PSP_GE_SIGNAL_CALL:
		case PSP_GE_SIGNAL_RET:
			break;
		case PSP_GE_SIGNAL_HANDLER_PAUSE:
			if (cmd == GE_CMD_FINISH)
				subintr = dl->subIntrBase | PSP_GE_SUBINTR_SIGNAL;
			break;
		default:
			if (cmd == GE_CMD_SIGNAL)
				subintr = dl->subIntrBase | PSP_GE_SUBINTR_SIGNAL;
			else
				subintr = dl->subIntrBase | PSP_GE_SUBINTR_FINISH;
			break;
		}
	}

	// Set the list as complete once the interrupt starts.
	if (dl->signal != PSP_GE_SIGNAL_HANDLER_PAUSE && cmd == GE_CMD_FINISH && dl->state != PSP_GE_DL_STATE_NONE) {
		dl->state = PSP_GE_DL_STATE_COMPLETED;
	}
	// The pause has been delivered now. It's marked the same way as sceGeBreak does.
	if (dl->signal == PSP_GE_SIGNAL_HANDLER_PAUSE && cmd == GE_CMD_FINISH) {
		dl->signal = PSP_GE_SIGNAL_HANDLER_SUSPEND;
	}

	const GeSubIntr *handler = (subintr >= 0 && subintr < 32 && ge_subintr[subintr].registered) ? &ge_subintr[subintr] : nullptr;
	if (handler) {
		u32 a0 = dl->subIntrToken & 0xFFFF;
		u32 a2 = sceKernelGetCompiledSdkVersion() <= 0x02000010 ? 0 : intrdata.pc + 4;
		DEBUG_LOG(Log::sceGe, "GE interrupt: list %d %s handler %08x(%x, %x, %x)", intrdata.listid,
			cmd == GE_CMD_SIGNAL ? "signal" : "finish", handler->func, a0, handler->arg, a2);
		intr_queue_ex(handler->func, a0, handler->arg, a2, handler->gp, GeInterruptHandlerDone, nullptr);
		return true;
	}

	if (dl->signal == PSP_GE_SIGNAL_HANDLER_SUSPEND && cmd == GE_CMD_SIGNAL) {
		if (sceKernelGetCompiledSdkVersion() <= 0x02000010) {
			if (dl->state != PSP_GE_DL_STATE_NONE && dl->state != PSP_GE_DL_STATE_COMPLETED) {
				dl->state = PSP_GE_DL_STATE_QUEUED;
			}
		}
	}

	ge_pending_cb.pop_front();
	gpu->InterruptEnd(intrdata.listid);
	gpu->ProcessDLQueue();
	return false;
}

static void GeDispatchInterrupts() {
	while (!geInFlight && geRaised > 0) {
		geRaised--;
		if (GeRunInterrupt())
			geInFlight = true;
	}
}

static void __GeExecuteSync(u64 userdata, int cyclesLate) {
	int listid = userdata >> 32;
	GPUSyncType type = (GPUSyncType)(userdata & 0xFFFFFFFF);
	bool wokeThreads = __GeTriggerWait(type, listid);
	gpu->SyncEnd(type, listid, wokeThreads);
}

static void __GeExecuteInterrupt(u64 userdata, int cyclesLate) {
	geRaised++;
	GeDispatchInterrupts();
}

bool __GeTriggerSync(GPUSyncType type, int id, u64 atTicks) {
	u64 userdata = (u64)id << 32 | (u64)type;
	s64 future = atTicks - CoreTiming::GetTicks(currentMIPS);
	if (type == GPU_SYNC_DRAW) {
		s64 left = CoreTiming::UnscheduleEvent(geSyncEvent, userdata);
		if (left > future)
			future = left;
	}
	CoreTiming::ScheduleEvent(future, geSyncEvent, userdata);
	return true;
}

void __GeCancelRaisedInterrupts(bool interruptRunning) {
	int count = __CancelRaisedInterrupts(PSP_GE_INTR);
	auto it = ge_pending_cb.begin();
	if (interruptRunning && it != ge_pending_cb.end())
		++it;
	for (; count > 0 && it != ge_pending_cb.end(); --count) {
		DisplayList *dl = gpu->getList(it->listid);
		if (dl)
			dl->pendingInterrupt = false;
		it = ge_pending_cb.erase(it);
	}
}

bool __GeTriggerInterrupt(int listid, u32 pc, u64 atTicks) {
	GeInterruptData intrdata;
	intrdata.listid = listid;
	intrdata.pc = pc;
	intrdata.cmd = Memory::ReadUnchecked_U32(pc - 4) >> 24;
	ge_pending_cb.push_back(intrdata);

	u64 userdata = (u64)listid << 32 | (u64)pc;
	CoreTiming::ScheduleEvent(atTicks - CoreTiming::GetTicks(currentMIPS), geInterruptEvent, userdata);
	return true;
}

void __GeWaitCurrentThread(GPUSyncType type, SceUID waitId, const char *reason) {
	Thread *t = g_cur;
	if (!t || g_in_intr) {
		ERROR_LOG(Log::sceGe, "%s: can't wait outside a thread", reason);
		return;
	}
	if (type == GPU_SYNC_DRAW) {
		drawWaitingThreads.push_back(t);
	} else if (type == GPU_SYNC_LIST) {
		listWaitingThreads[waitId].push_back(t);
	} else {
		ERROR_LOG(Log::sceGe, "__GeWaitCurrentThread: bad wait type");
		return;
	}
	DEBUG_LOG(Log::sceGe, "%s: thread '%s' waits", reason, t->name);
	thread_wait(W_GE, GeWaitId(type, waitId), 0, false);
}

static bool __GeTriggerWait(GPUSyncType type, SceUID waitId, std::vector<Thread *> &waitingThreads) {
	bool wokeThreads = false;
	const u32 id = GeWaitId(type, waitId);
	for (Thread *t : waitingThreads) {
		// Only if it's still waiting for this (it may have been released some other way).
		if (t->status == TS_WAITING && t->wait_type == W_GE && t->wait_id == id) {
			thread_wake(t, 0);
			wokeThreads = true;
		}
	}
	waitingThreads.clear();
	gstate_c.textureSyncTimeDomain++;
	gpuStats.perFrame.numGEInterrupts++;
	return wokeThreads;
}

bool __GeTriggerWait(GPUSyncType type, SceUID waitId) {
	if (type == GPU_SYNC_DRAW)
		return __GeTriggerWait(type, waitId, drawWaitingThreads);
	else if (type == GPU_SYNC_LIST)
		return __GeTriggerWait(type, waitId, listWaitingThreads[waitId]);
	ERROR_LOG(Log::sceGe, "__GeTriggerWait: bad wait type");
	return false;
}

void __GeInit() {
	memset(&ge_used_callbacks, 0, sizeof(ge_used_callbacks));
	memset(&ge_callback_data, 0, sizeof(ge_callback_data));
	memset(&ge_subintr, 0, sizeof(ge_subintr));
	ge_pending_cb.clear();
	geRaised = 0;
	geInFlight = false;
	geSyncEvent = CoreTiming::RegisterEvent("GeSyncEvent", &__GeExecuteSync);
	geInterruptEvent = CoreTiming::RegisterEvent("GeInterruptEvent", &__GeExecuteInterrupt);
	listWaitingThreads.clear();
	drawWaitingThreads.clear();
}

void __GeShutdown() {}

// ================================================================= HLE functions
static int __GeSubIntrBase(int callbackId) {
	return callbackId * 2;
}

// After list processing that may have woken threads: let a higher priority one run (hleReSchedule).
static void GeReschedule() {
	if (!g_in_intr)
		thread_resched();
}

static void h_GeEdramGetAddr(Ctx *c) { RET(0x04000000); }
static void h_GeEdramGetSize(Ctx *c) { RET(0x00200000); }

static void h_GeEdramSetAddrTranslation(Ctx *c) {
	u32 new_size = A(0);
	bool outsideRange = new_size != 0 && (new_size < 0x200 || new_size > 0x1000);
	bool notPowerOfTwo = (new_size & (new_size - 1)) != 0;
	if (outsideRange || notPowerOfTwo) {
		RET(SCE_KERNEL_ERROR_INVALID_VALUE);
		return;
	}
	RET(gpu->SetAddrTranslation(new_size));
}

static u32 GeEnqueue(u32 listAddress, u32 stallAddress, int callbackId, u32 optParamAddr, bool head) {
	auto optParam = PSPPointer<PspGeListArgs>::Create(optParamAddr);
	bool runList;
	u32 listID = gpu->EnqueueList(listAddress, stallAddress, __GeSubIntrBase(callbackId), optParam, head, &runList);
	if ((int)listID >= 0)
		listID = LIST_ID_MAGIC ^ listID;
	if (runList)
		gpu->ProcessDLQueue();
	DEBUG_LOG(Log::sceGe, "%08x=sceGeListEnQueue%s(addr=%08x, stall=%08x, cbid=%08x, param=%08x)", listID,
		head ? "Head" : "", listAddress, stallAddress, callbackId, optParamAddr);
	return listID;
}

u32 sceGeListEnQueue(u32 listAddress, u32 stallAddress, int callbackId, u32 optParamAddr) {
	return GeEnqueue(listAddress, stallAddress, callbackId, optParamAddr, false);
}
u32 sceGeListEnQueueHead(u32 listAddress, u32 stallAddress, int callbackId, u32 optParamAddr) {
	return GeEnqueue(listAddress, stallAddress, callbackId, optParamAddr, true);
}

static void h_GeListEnQueue(Ctx *c) { RET(sceGeListEnQueue(A(0), A(1), (int)A(2), A(3))); }
static void h_GeListEnQueueHead(Ctx *c) { RET(sceGeListEnQueueHead(A(0), A(1), (int)A(2), A(3))); }

static void h_GeListDeQueue(Ctx *c) {
	int result = gpu->DequeueList(LIST_ID_MAGIC ^ A(0));
	RET(result);
	GeReschedule();
}

static void h_GeListUpdateStallAddr(Ctx *c) {
	bool runList;
	int retval = gpu->UpdateStall(LIST_ID_MAGIC ^ A(0), A(1), &runList);
	if (runList)
		gpu->ProcessDLQueue();
	RET(retval);
}

// Whether GE events or interrupts are queued for the scheduler.
static bool GeWorkPending() {
	return !CoreTiming::pendingEvents.empty() || geInFlight || geRaised > 0;
}

// A status poll that isn't "complete" while GE events/interrupts are queued: let the scheduler run
// them now (a zero-length delay), like the hardware interrupt would interrupt a polling loop. Without
// this, a thread spinning on sceGeListSync(id, 1) would never see its list complete, since our events
// only fire when the scheduler gets control.
static void GeYieldIfPending(u32 status) {
	if (status != PSP_GE_LIST_COMPLETED && GeWorkPending() && g_cur && !g_in_intr)
		thread_delay_until(now_us());
}

// mode 0: wait for completion, 1: check and return
static void h_GeListSync(Ctx *c) {
	gstate_c.textureSyncTimeDomain++;
	u32 mode = A(1);
	u32 r = gpu->ListSync(LIST_ID_MAGIC ^ A(0), mode);
	RET(r);
	if (mode == 1)
		GeYieldIfPending(r);
}

static void h_GeDrawSync(Ctx *c) {
	gstate_c.textureSyncTimeDomain++;
	u32 mode = A(0);
	u32 r = gpu->DrawSync(mode);
	RET(r);
	if (mode == 1)
		GeYieldIfPending(r);
}

static void h_GeContinue(Ctx *c) {
	bool runList;
	int ret = gpu->Continue(&runList);
	if (runList)
		gpu->ProcessDLQueue();
	RET(ret);
	GeReschedule();
}

static void h_GeBreak(Ctx *c) {
	u32 mode = A(0), unknownPtr = A(1);
	if (mode > 1) {
		RET(SCE_KERNEL_ERROR_INVALID_MODE);
		return;
	}
	if ((int)unknownPtr < 0 || (int)(unknownPtr + 16) < 0) {
		RET(SCE_KERNEL_ERROR_PRIV_REQUIRED);
		return;
	}
	int result = gpu->Break(mode);
	if (result >= 0 && mode == 0)
		result = LIST_ID_MAGIC ^ result;
	RET(result);
}

static void h_GeSetCallback(Ctx *c) {
	int cbID = -1;
	for (size_t i = 0; i < ARRAY_SIZE(ge_used_callbacks); ++i) {
		if (!ge_used_callbacks[i]) {
			cbID = (int)i;
			break;
		}
	}
	if (cbID == -1) {
		RET(SCE_KERNEL_ERROR_OUT_OF_MEMORY);
		return;
	}

	ge_used_callbacks[cbID] = true;
	auto callbackData = PSPPointer<PspGeCallbackData>::Create(A(0));
	ge_callback_data[cbID] = *callbackData;

	int subIntrBase = __GeSubIntrBase(cbID);
	const u32 gp = c->r[28];
	if (ge_callback_data[cbID].finish_func != 0)
		ge_subintr[subIntrBase | PSP_GE_SUBINTR_FINISH] = GeSubIntr{true, ge_callback_data[cbID].finish_func, ge_callback_data[cbID].finish_arg, gp};
	if (ge_callback_data[cbID].signal_func != 0)
		ge_subintr[subIntrBase | PSP_GE_SUBINTR_SIGNAL] = GeSubIntr{true, ge_callback_data[cbID].signal_func, ge_callback_data[cbID].signal_arg, gp};
	DEBUG_LOG(Log::sceGe, "sceGeSetCallback: signal %08x(%08x) finish %08x(%08x) -> %d", (u32)ge_callback_data[cbID].signal_func,
		(u32)ge_callback_data[cbID].signal_arg, (u32)ge_callback_data[cbID].finish_func, (u32)ge_callback_data[cbID].finish_arg, cbID);
	RET(cbID);
}

static void h_GeUnsetCallback(Ctx *c) {
	u32 cbID = A(0);
	if (cbID >= ARRAY_SIZE(ge_used_callbacks)) {
		RET(SCE_KERNEL_ERROR_INVALID_ID);
		return;
	}
	if (ge_used_callbacks[cbID]) {
		int subIntrBase = __GeSubIntrBase(cbID);
		ge_subintr[subIntrBase | PSP_GE_SUBINTR_FINISH].registered = false;
		ge_subintr[subIntrBase | PSP_GE_SUBINTR_SIGNAL].registered = false;
	}
	ge_used_callbacks[cbID] = false;
	RET(0);
}

static void h_GeSaveContext(Ctx *c) {
	u32 ctxAddr = A(0);
	if (gpu->BusyDrawing()) {
		RET(-1);
		return;
	}
	if (Memory::IsValidAddress(ctxAddr))
		gstate.Save((u32_le *)Memory::GetPointerWriteUnchecked(ctxAddr));
	RET(0);
}

static void h_GeRestoreContext(Ctx *c) {
	u32 ctxAddr = A(0);
	if (gpu->BusyDrawing()) {
		RET(SCE_KERNEL_ERROR_BUSY);
		return;
	}
	if (Memory::IsValidAddress(ctxAddr))
		gstate.Restore((u32_le *)Memory::GetPointerWriteUnchecked(ctxAddr));
	gpu->ReapplyGfxState();
	RET(0);
}

static void h_GeGetMtx(Ctx *c) {
	int type = (int)A(0);
	u32 matrixPtr = A(1);
	int size = type == GE_MTX_PROJECTION ? 16 : 12;
	if (!Memory::IsValidRange(matrixPtr, size * sizeof(float))) {
		RET(-1);
		return;
	}
	u32_le *dest = (u32_le *)Memory::GetPointerWriteUnchecked(matrixPtr);
	if (!gpu->GetMatrix24(GEMatrixType(type), dest, 0)) {
		RET(SCE_KERNEL_ERROR_INVALID_INDEX);
		return;
	}
	RET(0);
}

static void h_GeGetCmd(Ctx *c) {
	int cmd = (int)A(0);
	if (cmd >= 0 && cmd < (int)ARRAY_SIZE(gstate.cmdmem)) {
		// Does not mask away the high bits.  But matrix regs don't read back.
		u32 val = gstate.cmdmem[cmd];
		switch (cmd) {
		case GE_CMD_BONEMATRIXDATA:
		case GE_CMD_WORLDMATRIXDATA:
		case GE_CMD_VIEWMATRIXDATA:
		case GE_CMD_PROJMATRIXDATA:
		case GE_CMD_TGENMATRIXDATA:
			val &= 0xFF000000;
			break;
		case GE_CMD_BONEMATRIXNUMBER:
			val &= 0xFF00007F;
			break;
		case GE_CMD_WORLDMATRIXNUMBER:
		case GE_CMD_VIEWMATRIXNUMBER:
		case GE_CMD_PROJMATRIXNUMBER:
		case GE_CMD_TGENMATRIXNUMBER:
			val &= 0xFF00000F;
			break;
		default:
			break;
		}
		RET(val);
		return;
	}
	RET(SCE_KERNEL_ERROR_INVALID_INDEX);
}

static void h_GeGetStack(Ctx *c) {
	RET(gpu->GetStack((int)A(0), A(1)));
}

static const HleEnt ge_ents[] = {
	{0xE47E40E4, "sceGeEdramGetAddr", h_GeEdramGetAddr},
	{0xAB49E76A, "sceGeListEnQueue", h_GeListEnQueue},
	{0x1C0D95A6, "sceGeListEnQueueHead", h_GeListEnQueueHead},
	{0xE0D68148, "sceGeListUpdateStallAddr", h_GeListUpdateStallAddr},
	{0x03444EB4, "sceGeListSync", h_GeListSync},
	{0xB287BD61, "sceGeDrawSync", h_GeDrawSync},
	{0xB448EC0D, "sceGeBreak", h_GeBreak},
	{0x4C06E472, "sceGeContinue", h_GeContinue},
	{0xA4FC06A4, "sceGeSetCallback", h_GeSetCallback},
	{0x05DB22CE, "sceGeUnsetCallback", h_GeUnsetCallback},
	{0x1F6752AD, "sceGeEdramGetSize", h_GeEdramGetSize},
	{0xB77905EA, "sceGeEdramSetAddrTranslation", h_GeEdramSetAddrTranslation},
	{0xDC93CFEF, "sceGeGetCmd", h_GeGetCmd},
	{0x57C8945B, "sceGeGetMtx", h_GeGetMtx},
	{0x438A385A, "sceGeSaveContext", h_GeSaveContext},
	{0x0BF608FB, "sceGeRestoreContext", h_GeRestoreContext},
	{0x5FB86AB0, "sceGeListDeQueue", h_GeListDeQueue},
	{0xE66CB92E, "sceGeGetStack", h_GeGetStack},
};

// ================================================================= exported C API
static bool g_hleReady;
static int g_drawThreads = 1;
static u32 g_dispTop, g_dispStride;
static int g_dispFormat;

bool ge_hle_ready(void) { return g_hleReady; }

bool ge_soft_display(uint32_t *top, uint32_t *stride, int *format) {
	if (!g_dispTop)
		return false;
	*top = g_dispTop;
	*stride = g_dispStride;
	*format = g_dispFormat;
	return true;
}

void ge_common_init(void) {
	static bool done;
	if (done)
		return;
	done = true;
	Memory::base = g_mem;

	// PPSSPP formats every enabled log line, so only enable what the runtime will print.
	LogLevel level = g_loglevel >= 3 ? LogLevel::LVERBOSE : g_loglevel >= 2 ? LogLevel::LDEBUG : g_loglevel >= 1 ? LogLevel::LWARNING : LogLevel::LERROR;
	for (LogChannel &ch : g_log)
		ch.level = level;

	// Drawing threads: PSPGPU_THREADS=n (1 = draw on the calling thread only).
	int threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (const char *e = getenv("PSPGPU_THREADS"))
		threads = atoi(e);
	threads = std::max(1, std::min(threads, 16));
	g_threadManager.Init(threads, 1);
	g_drawThreads = threads;
}

// Registers the sceGe HLE and creates the GPU of the selected backend. With OpenGL, the GPU needs the
// draw context from ge_gl_init(); whichever of the two runs last creates it.
void ge_soft_init(void) {
	if (g_hleReady)
		return;
	ge_common_init();
	if (ge_backend() == GE_BACKEND_SOFTWARE) {
		PSP_CoreParameter().gpuCore = GPUCORE_SOFTWARE;
		gpu = new SoftGPU(nullptr, nullptr);
		LOGI("GE: PPSSPP software renderer, %d drawing thread%s", g_drawThreads, g_drawThreads == 1 ? "" : "s");
	}
	__GeInit();
	hle_register(ge_ents, (int)(sizeof(ge_ents) / sizeof(ge_ents[0])));
	g_hleReady = true;
	if (ge_backend() == GE_BACKEND_OPENGL) {
		if (ge_gl_create_gpu())
			LOGI("GE: PPSSPP OpenGL renderer");
		else
			LOGI("GE: OpenGL renderer selected; waiting for ge_gl_init()");
	}
}

// Per frame (vblank): finish queued drawing so VRAM holds the frame, and do the GPU's frame upkeep.
extern bool g_ge_skip_draw;
void ge_soft_vblank(void) {
	if (!gpu)
		return;
	{	// test aid: SR2_DRAW_EVERY=N rasterizes only every Nth vblank's drawing (software renderer), for fast headless runs
		static int every = -1, n;
		if (every < 0) { const char *e = getenv("SR2_DRAW_EVERY"); every = e ? atoi(e) : 0; }
		if (every > 1) g_ge_skip_draw = ++n % every != 0;
	}
	gpu->FlushPendingDrawing();
	gpu->PSPFrame();
}

void ge_soft_set_display(u32 topaddr, int stride, int format) {
	g_dispTop = topaddr & 0x3FFFFFFF;
	g_dispStride = stride;
	g_dispFormat = format;
	if (!gpu)
		return;
	gpu->SetDisplayFramebuffer(topaddr, stride, (GEBufferFormat)format);
}

// Waits for any queued (possibly threaded) drawing to land in guest memory.
void ge_soft_flush(void) {
	if (gpu)
		gpu->FlushPendingDrawing();
}

// PPGe (libpsputility.a) turns GE interrupts off around its own display lists, like PPSSPP.
void ge_soft_enable_interrupts(int enable) {
	if (gpu)
		gpu->EnableInterrupts(enable != 0);
}

void ge_soft_set_sdk_version(u32 ver) {
	g_sdkVersion = ver;
}

// ================================================================= CPU-side memory notifications
// Like PPSSPP's sceDmacMemcpy / memcpy replacements (Core/HLE/sceDmac.cpp, ReplaceTables.cpp).
int ge_notify_memcpy(uint32_t dst, uint32_t src, uint32_t size) {
	if (!gpu || !size)
		return 0;
	bool skip = false;
	if (Memory::IsVRAMAddress(src) || Memory::IsVRAMAddress(dst))
		skip = gpu->PerformMemoryCopy(dst, src, (int)size);
	gpu->NotifyVideoCopy(dst, src, (int)size);
	return skip ? 1 : 0;
}

int ge_notify_memset(uint32_t dst, uint8_t value, uint32_t size) {
	if (!gpu || !size || !Memory::IsVRAMAddress(dst))
		return 0;
	return gpu->PerformMemorySet(dst, value, (int)size) ? 1 : 0;
}

// Like PPSSPP's sceKernelDcacheWriteback*Range.
void ge_notify_cpu_write(uint32_t addr, uint32_t size) {
	if (gpu && addr && size)
		gpu->InvalidateCache(addr, (int)size, GPU_INVALIDATE_HINT);
}

void ge_write_formatted_from_memory(uint32_t addr, int size, int width, int format) {
	if (gpu)
		gpu->PerformWriteFormattedFromMemory(addr, size, width, (GEBufferFormat)format);
}

// ---- rcsm: camera offset applied to view matrices (see ge_set_view_offset)
float g_rcsm_view_off[2];
extern "C" void ge_set_view_offset(float x, float y) { g_rcsm_view_off[0] = x; g_rcsm_view_off[1] = y; }

// ---- rcsm: GE command trace (test aid): every executed (pc, op) of the software renderer, binary
FILE *g_rcsm_ge_trace;
extern "C" void ge_trace(const char *path) {
	if (g_rcsm_ge_trace) { fclose(g_rcsm_ge_trace); g_rcsm_ge_trace = nullptr; }
	if (path) g_rcsm_ge_trace = fopen(path, "wb");
}
