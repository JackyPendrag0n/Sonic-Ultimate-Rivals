// Shim for PPSSPP's Core/HLE/sceKernelThread.h (rcsm audio extraction): the few thread calls sceSas
// makes. Its own wait/resume path is never taken here (see audio_hle.cpp), so these are stubs. GPL-2.0+.
#pragma once
#include "Common/CommonTypes.h"
typedef int SceUID;
enum WaitType : int {
	WAITTYPE_NONE = 0,
	WAITTYPE_HLEDELAY = 14,
};
bool __KernelIsDispatchEnabled();
SceUID __KernelGetCurThread();
SceUID __KernelGetWaitID(SceUID threadID, WaitType type, u32 &error);
u32 __KernelGetWaitValue(SceUID threadID, u32 &error);
void __KernelResumeThreadFromWait(SceUID threadID, u64 retval);
void __KernelReSchedule(const char *reason = "no reason");
void __KernelWaitCurThread(WaitType type, SceUID waitId, u32 waitValue, u32 timeoutPtr, bool processCallbacks, const char *reason);

// Guest calls made on behalf of an HLE function (hleEnqueueCall, in HLE.h), and what runs after them.
class PointerWrap;
class PSPAction;
struct MipsCall {
	u32 entryPoint = 0;
	PSPAction *doAfter = nullptr;
	void setReturnValue(u32 value);
	void setReturnValue(u64 value);
	inline void setReturnValue(int value) { setReturnValue((u32)value); }
	inline void setReturnValue(s64 value) { setReturnValue((u64)value); }
};
class PSPAction {
public:
	virtual ~PSPAction() {}
	virtual void run(MipsCall &call) = 0;
	virtual void DoState(PointerWrap &p) = 0;
	int actionTypeID = 0;
};
int __KernelRegisterActionType(PSPAction *(*creator)());
void __KernelRestoreActionType(int actionType, PSPAction *(*creator)());
PSPAction *__KernelCreateAction(int actionType);
