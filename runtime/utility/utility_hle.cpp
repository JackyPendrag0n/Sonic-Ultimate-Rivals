// sceUtility HLE for the rcsm runtime: system dialogs (savedata, message, on-screen keyboard), system
// parameters and utility module loading.
//
// Adapted from PPSSPP's Core/HLE/sceUtility.cpp (Copyright (c) 2012- PPSSPP Project). The dialogs
// themselves (Core/Dialog/*, Core/Util/PPGeDraw.cpp) are PPSSPP's, compiled unmodified. Changes here:
//   * PPSSPP runs each dialog's init/shutdown work on a guest helper thread made of syscalls
//     (HLEHelperThread). That needs generated MIPS code, which a recompiled runtime can't run, so the
//     work completes after the same delay instead, checked whenever the game calls into sceUtility
//     (it polls GetStatus while it waits). Thread priorities are not modelled.
//   * Utility modules (sceUtilityLoad*Module) are tracked like PPSSPP but always succeed for valid
//     ids (no memory is reserved; already-loaded/missing-dependency answers are only logged).
//   * Netconf, screenshot, gamedata install, NP sign-in and the HTML viewer run PPSSPP's placeholder
//     dialog (normal status lifecycle, result CANCEL). No network configurations exist.
//   * No savestates.
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU
// General Public License as published by the Free Software Foundation, version 2.0 or later versions.
// See LICENSE.TXT.

#include <algorithm>
#include <cstring>
#include <map>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/Log.h"
#include "Common/Data/Text/I18n.h"
#include "Core/Config.h"
#include "Core/CoreTiming.h"
#include "Core/MemMapHelpers.h"
#include "Core/System.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/ErrorCodes.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/HLE/sceUtility.h"
#include "Core/Dialog/PSPDialog.h"
#include "Core/Dialog/PSPSaveDialog.h"
#include "Core/Dialog/PSPMsgDialog.h"
#include "Core/Dialog/PSPOskDialog.h"
#include "Core/Dialog/PSPPlaceholderDialog.h"
#include "Core/Dialog/SavedataParam.h"
#include "Core/Util/PPGeDraw.h"
#include "GPU/GPUCommon.h"
#include "../audio/hle_bridge.h"
#include "utility_hle.h"

#include "../audio/stubs/kernel_bridge.h"  // last: it defines short macros (A, RET, LW, ...)

void __PPGeInit();

static UtilityDialogType currentDialogType;
bool currentDialogActive;
static PSPSaveDialog *saveDialog;
static PSPMsgDialog *msgDialog;
static PSPOskDialog *oskDialog;
static PSPPlaceholderDialog *netDialog;
static PSPPlaceholderDialog *screenshotDialog;
static PSPPlaceholderDialog *gamedataInstallDialog;
static PSPPlaceholderDialog *npSigninDialog;
static PSPPlaceholderDialog *gameSharingDialog;
static PSPPlaceholderDialog *htmlViewerDialog;
static bool htmlViewerStarted;

static int oldStatus = -1;
static std::map<int, u32> currentlyLoadedModules;

// ---------------------------------------------------------------- the dialog "helper thread"
// What PPSSPP's helper thread does at its end (FinishInit / FinishShutdown), once its time is up.
struct PendingDialogWork {
	bool active = false;
	bool init = false;
	UtilityDialogType type = UtilityDialogType::NONE;
	u64 readyAtUs = 0;
};
static PendingDialogWork pendingWork;

static PSPDialog *CurrentDialog(UtilityDialogType type);

static int UtilityInitDialog(int type) {
	PSPDialog *dialog = CurrentDialog((UtilityDialogType)type);
	if (dialog)
		return dialog->FinishInit();
	return 0;
}

static int UtilityFinishDialog(int type) {
	PSPDialog *dialog = CurrentDialog((UtilityDialogType)type);
	if (dialog)
		return dialog->FinishShutdown();
	return 0;
}

static void RunPendingDialogWork(bool force = false) {
	if (!pendingWork.active)
		return;
	if (!force && now_us() < pendingWork.readyAtUs)
		return;
	PendingDialogWork work = pendingWork;
	pendingWork.active = false;
	if (work.init)
		UtilityInitDialog((int)work.type);
	else
		UtilityFinishDialog((int)work.type);
}

// Called by PPSSPP's PSPDialog::ChangeStatusInit / ChangeStatusShutdown.
void UtilityDialogInitialize(UtilityDialogType type, int delayUs, int accessPriority, int graphicsPriority) {
	// A previous helper still running is cut short, like PPSSPP's CleanupDialogThreads(true).
	pendingWork.active = false;
	pendingWork = PendingDialogWork{true, true, type, now_us() + (u64)std::max(delayUs, 0)};
}

void UtilityDialogShutdown(UtilityDialogType type, int delayUs, int accessPriority, int graphicsPriority) {
	pendingWork.active = false;
	pendingWork = PendingDialogWork{true, false, type, now_us() + (u64)std::max(delayUs, 0)};
}

static void CleanupDialogThreads(bool force = false) {
	RunPendingDialogWork(force);
}

static void ActivateDialog(UtilityDialogType type) {
	if (!currentDialogActive) {
		currentDialogType = type;
		currentDialogActive = true;
		oldStatus = -1;
	}
}

static void DeactivateDialog() {
	if (currentDialogActive)
		currentDialogActive = false;
}

static PSPDialog *CurrentDialog(UtilityDialogType type) {
	switch (type) {
	case UtilityDialogType::NONE: break;
	case UtilityDialogType::SAVEDATA: return saveDialog;
	case UtilityDialogType::MSG: return msgDialog;
	case UtilityDialogType::OSK: return oskDialog;
	case UtilityDialogType::NET: return netDialog;
	case UtilityDialogType::SCREENSHOT: return screenshotDialog;
	case UtilityDialogType::GAMESHARING: return gameSharingDialog;
	case UtilityDialogType::GAMEDATAINSTALL: return gamedataInstallDialog;
	case UtilityDialogType::NPSIGNIN: return npSigninDialog;
	case UtilityDialogType::HTMLVIEWER: return htmlViewerDialog;
	}
	return nullptr;
}

// PPGe ages cached images/text by GPU flips; a dialog Update is called once per game frame.
static void CountDialogFrame() {
	gpuStats.totals.numFlips++;
}

static bool UtilityDialogBusy() {
	PSPDialog *current = CurrentDialog(currentDialogType);
	return current && current->IsBusy();
}

// A PSP runs one dialog at a time whatever its type (see PPSSPP).
template <typename T>
static int UtilityInitStart(UtilityDialogType type, T *dialog, u32 paramAddr) {
	RunPendingDialogWork();
	if (UtilityDialogBusy())
		return SCE_ERROR_UTILITY_INVALID_STATUS;
	PSPDialog *current = CurrentDialog(currentDialogType);
	if (current)
		current->FinishVolatile();
	dialog->FinishAutoShutdown();
	const bool wasBusy = dialog->IsBusy();
	int result = dialog->Init(paramAddr);
	if (result >= 0 || (!wasBusy && dialog->IsBusy())) {
		if (current && current != dialog)
			current->FinishAutoShutdown();
		currentDialogActive = false;
		ActivateDialog(type);
	}
	return result;
}

template <typename T>
static int UtilityShutdownStart(UtilityDialogType type, T *dialog) {
	RunPendingDialogWork();
	if (currentDialogType != type)
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	DeactivateDialog();
	return hleLogDebug(Log::sceUtility, dialog->Shutdown());
}

static int UtilityGetStatus(UtilityDialogType type, PSPDialog *dialog) {
	RunPendingDialogWork();
	if (currentDialogType != type)
		return hleLogDebug(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	const PSPDialog::DialogStatus status = dialog->GetStatus();
	CleanupDialogThreads();
	if (oldStatus != status) {
		oldStatus = status;
		return hleLogDebug(Log::sceUtility, status, "status changed: %s", UtilityDialogStatusToString(status));
	}
	return hleLogVerbose(Log::sceUtility, status, "status: %s", UtilityDialogStatusToString(status));
}

// ---------------------------------------------------------------- savedata
static int sceUtilitySavedataInitStart(u32 paramAddr) {
	return hleLogDebug(Log::sceUtility, UtilityInitStart(UtilityDialogType::SAVEDATA, saveDialog, paramAddr));
}

static int sceUtilitySavedataShutdownStart() {
	RunPendingDialogWork();
	if (currentDialogType != UtilityDialogType::SAVEDATA)
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	DeactivateDialog();
	return hleLogDebug(Log::sceUtility, saveDialog->Shutdown());
}

static int sceUtilitySavedataGetStatus() {
	return UtilityGetStatus(UtilityDialogType::SAVEDATA, saveDialog);
}

static int sceUtilitySavedataUpdate(int animSpeed) {
	RunPendingDialogWork();
	CountDialogFrame();
	if (currentDialogType != UtilityDialogType::SAVEDATA || !saveDialog)
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	int result = hleLogDebug(Log::sceUtility, saveDialog->Update(animSpeed));
	if (result >= 0)
		return hleDelayResult(result, "savedata update", 300);
	return result;
}

// ---------------------------------------------------------------- message dialog
static int sceUtilityMsgDialogInitStart(u32 paramAddr) {
	return hleLogInfo(Log::sceUtility, UtilityInitStart(UtilityDialogType::MSG, msgDialog, paramAddr));
}
static int sceUtilityMsgDialogShutdownStart() {
	return UtilityShutdownStart(UtilityDialogType::MSG, msgDialog);
}
static int sceUtilityMsgDialogUpdate(int animSpeed) {
	RunPendingDialogWork();
	CountDialogFrame();
	if (currentDialogType != UtilityDialogType::MSG)
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	int ret = msgDialog->Update(animSpeed);
	if (ret >= 0)
		return hleDelayResult(hleLogDebug(Log::sceUtility, ret), "msgdialog update", 800);
	return hleLogDebug(Log::sceUtility, ret);
}
static int sceUtilityMsgDialogGetStatus() {
	return UtilityGetStatus(UtilityDialogType::MSG, msgDialog);
}
static int sceUtilityMsgDialogAbort() {
	RunPendingDialogWork();
	if (currentDialogType != UtilityDialogType::MSG)
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	return hleLogDebug(Log::sceUtility, msgDialog->Abort());
}

// ---------------------------------------------------------------- on-screen keyboard
static int sceUtilityOskInitStart(u32 oskPtr) {
	return hleLogInfo(Log::sceUtility, UtilityInitStart(UtilityDialogType::OSK, oskDialog, oskPtr));
}
static int sceUtilityOskShutdownStart() {
	return UtilityShutdownStart(UtilityDialogType::OSK, oskDialog);
}
static int sceUtilityOskUpdate(int animSpeed) {
	RunPendingDialogWork();
	CountDialogFrame();
	if (currentDialogType != UtilityDialogType::OSK)
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	return hleLogDebug(Log::sceUtility, oskDialog->Update(animSpeed));
}
static int sceUtilityOskGetStatus() {
	return UtilityGetStatus(UtilityDialogType::OSK, oskDialog);
}

// ---------------------------------------------------------------- placeholder dialogs
#define PLACEHOLDER_DIALOG(Name, TYPE, dialog) \
	static int sceUtility##Name##InitStart(u32 paramAddr) { \
		return hleLogWarning(Log::sceUtility, UtilityInitStart(UtilityDialogType::TYPE, dialog, paramAddr), "not implemented, will report cancelled"); \
	} \
	static int sceUtility##Name##ShutdownStart() { return UtilityShutdownStart(UtilityDialogType::TYPE, dialog); } \
	static int sceUtility##Name##Update(int animSpeed) { \
		RunPendingDialogWork(); \
		CountDialogFrame(); \
		if (currentDialogType != UtilityDialogType::TYPE) \
			return hleLogDebug(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type"); \
		return hleLogDebug(Log::sceUtility, dialog->Update(animSpeed)); \
	} \
	static int sceUtility##Name##GetStatus() { return UtilityGetStatus(UtilityDialogType::TYPE, dialog); }

PLACEHOLDER_DIALOG(Netconf, NET, netDialog)
PLACEHOLDER_DIALOG(Screenshot, SCREENSHOT, screenshotDialog)
PLACEHOLDER_DIALOG(GamedataInstall, GAMEDATAINSTALL, gamedataInstallDialog)
PLACEHOLDER_DIALOG(NpSignin, NPSIGNIN, npSigninDialog)
PLACEHOLDER_DIALOG(GameSharing, GAMESHARING, gameSharingDialog)

static int sceUtilityScreenshotContStart(u32 paramAddr) {
	if (currentDialogType != UtilityDialogType::SCREENSHOT)
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	return hleLogWarning(Log::sceUtility, 0, "not implemented");
}
static int sceUtilityGamedataInstallAbort() {
	if (currentDialogType != UtilityDialogType::GAMEDATAINSTALL)
		return hleLogWarning(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	return hleLogDebug(Log::sceUtility, 0);
}

// The HTML viewer keeps its own state on a PSP: other dialogs don't make it busy or change its type,
// and until one has been started its calls return WRONG_TYPE (PPSSPP, utility/dialog/htmlviewer).
static int sceUtilityHtmlViewerInitStart(u32 paramsPtr) {
	RunPendingDialogWork();
	int ret = htmlViewerDialog->Init(paramsPtr);
	if (ret >= 0)
		htmlViewerStarted = true;
	return hleLogWarning(Log::sceUtility, ret, "not implemented, will report cancelled");
}
static int sceUtilityHtmlViewerShutdownStart() {
	RunPendingDialogWork();
	if (!htmlViewerStarted)
		return hleLogDebug(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	return hleLogDebug(Log::sceUtility, htmlViewerDialog->Shutdown());
}
static int sceUtilityHtmlViewerUpdate(int animSpeed) {
	RunPendingDialogWork();
	if (!htmlViewerStarted)
		return hleLogDebug(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	return hleLogDebug(Log::sceUtility, htmlViewerDialog->Update(animSpeed));
}
static int sceUtilityHtmlViewerGetStatus() {
	RunPendingDialogWork();
	if (!htmlViewerStarted)
		return hleLogDebug(Log::sceUtility, SCE_ERROR_UTILITY_WRONG_TYPE, "wrong dialog type");
	return hleLogVerbose(Log::sceUtility, htmlViewerDialog->GetStatus());
}

static void sceUtilityInstallInitStart(u32 unknown) {
	WARN_LOG(Log::sceUtility, "UNIMPL sceUtilityInstallInitStart()");
}
static int sceUtilityStoreCheckoutShutdownStart() { return hleLogError(Log::sceUtility, 0, "UNIMPL"); }
static int sceUtilityStoreCheckoutInitStart(u32 paramsPtr) { return hleLogError(Log::sceUtility, 0, "UNIMPL"); }
static int sceUtilityStoreCheckoutUpdate(int drawSpeed) { return hleLogError(Log::sceUtility, 0, "UNIMPL"); }
static int sceUtilityStoreCheckoutGetStatus() { return hleLogError(Log::sceUtility, 0, "UNIMPL"); }

// ---------------------------------------------------------------- network parameters
// No network configurations.
static int sceUtilityCheckNetParam(int id) {
	return hleLogDebug(Log::sceUtility, SCE_ERROR_NETPARAM_BAD_NETCONF, "no network configurations");
}
static int sceUtilityGetNetParam(int id, int param, u32 dataAddr) {
	return hleLogDebug(Log::sceUtility, SCE_ERROR_NETPARAM_BAD_NETCONF, "no network configurations");
}
static int sceUtilityGetNetParamLatestID(u32 idAddr) {
	if (Memory::IsValid4AlignedAddress(idAddr))
		Memory::WriteUnchecked_U32(0, idAddr);
	return hleLogDebug(Log::sceUtility, 0);
}

// ---------------------------------------------------------------- system parameters (from PPSSPP)
static const char *SystemParamToString(int param) {
	switch (param) {
	case PSP_SYSTEMPARAM_ID_STRING_NICKNAME: return "STRING_NICKNAME";
	case PSP_SYSTEMPARAM_ID_INT_ADHOC_CHANNEL: return "INT_ADHOC_CHANNEL";
	case PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE: return "INT_WLAN_POWERSAVE";
	case PSP_SYSTEMPARAM_ID_INT_DATE_FORMAT: return "INT_DATE_FORMAT";
	case PSP_SYSTEMPARAM_ID_INT_TIME_FORMAT: return "INT_TIME_FORMAT";
	case PSP_SYSTEMPARAM_ID_INT_TIMEZONE: return "INT_TIMEZONE";
	case PSP_SYSTEMPARAM_ID_INT_DAYLIGHTSAVINGS: return "INT_DAYLIGHTSAVINGS";
	case PSP_SYSTEMPARAM_ID_INT_LANGUAGE: return "INT_LANGUAGE";
	case PSP_SYSTEMPARAM_ID_INT_BUTTON_PREFERENCE: return "INT_BUTTON_PREFERENCE";
	case PSP_SYSTEMPARAM_ID_INT_LOCK_PARENTAL_LEVEL: return "INT_LOCK_PARENTAL_LEVEL";
	default: return "N/A";
	}
}

static u32 sceUtilitySetSystemParamString(u32 id, u32 strPtr) {
	return hleLogWarning(Log::sceUtility, 0, "(%s) ignored", SystemParamToString(id));
}

static u32 sceUtilityGetSystemParamString(u32 id, u32 destAddr, int destSize) {
	if (destSize > 0 && !Memory::IsValidRange(destAddr, destSize))
		return hleLogError(Log::sceUtility, -1);
	switch (id) {
	case PSP_SYSTEMPARAM_ID_STRING_NICKNAME: {
		if (destSize <= (int)g_Config.sNickName.length())
			return SCE_ERROR_UTILITY_STRING_TOO_LONG;
		char *buf = (char *)Memory::GetPointerWriteUnchecked(destAddr);
		strncpy(buf, g_Config.sNickName.c_str(), destSize);
		break;
	}
	default:
		return hleLogError(Log::sceUtility, SCE_ERROR_UTILITY_INVALID_SYSTEM_PARAM_ID);
	}
	return hleLogDebug(Log::sceUtility, 0, "(%s)", SystemParamToString(id));
}

static u32 sceUtilitySetSystemParamInt(u32 id, u32 value) {
	switch (id) {
	case PSP_SYSTEMPARAM_ID_INT_ADHOC_CHANNEL:
		if (value != 0 && value != 1 && value != 6 && value != 11)
			return hleLogError(Log::sceUtility, SCE_ERROR_UTILITY_INVALID_ADHOC_CHANNEL);
		break;
	case PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE:
		break;
	default:
		return hleLogError(Log::sceUtility, SCE_ERROR_UTILITY_INVALID_SYSTEM_PARAM_ID);
	}
	return hleLogDebug(Log::sceUtility, 0, "(%s)", SystemParamToString(id));
}

int GetPSPLanguage() {
	return g_Config.iLanguage;
}

static u32 sceUtilityGetSystemParamInt(u32 id, u32 destaddr) {
	u32 param = 0;
	switch (id) {
	case PSP_SYSTEMPARAM_ID_INT_ADHOC_CHANNEL: param = g_Config.iWlanAdhocChannel; break;
	case PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE: param = g_Config.bWlanPowerSave ? PSP_SYSTEMPARAM_WLAN_POWERSAVE_ON : PSP_SYSTEMPARAM_WLAN_POWERSAVE_OFF; break;
	case PSP_SYSTEMPARAM_ID_INT_DATE_FORMAT: param = g_Config.iDateFormat; break;
	case PSP_SYSTEMPARAM_ID_INT_TIME_FORMAT: param = g_Config.iTimeFormat == PSP_SYSTEMPARAM_TIME_FORMAT_12HR ? PSP_SYSTEMPARAM_TIME_FORMAT_12HR : PSP_SYSTEMPARAM_TIME_FORMAT_24HR; break;
	case PSP_SYSTEMPARAM_ID_INT_TIMEZONE: param = g_Config.iTimeZone; break;
	case PSP_SYSTEMPARAM_ID_INT_DAYLIGHTSAVINGS: param = g_Config.bDayLightSavings ? PSP_SYSTEMPARAM_DAYLIGHTSAVINGS_SAVING : PSP_SYSTEMPARAM_DAYLIGHTSAVINGS_STD; break;
	case PSP_SYSTEMPARAM_ID_INT_LANGUAGE: param = GetPSPLanguage(); break;
	case PSP_SYSTEMPARAM_ID_INT_BUTTON_PREFERENCE: param = g_Config.iButtonPreference; break;
	case PSP_SYSTEMPARAM_ID_INT_LOCK_PARENTAL_LEVEL: param = g_Config.iLockParentalLevel; break;
	default:
		return hleLogError(Log::sceUtility, SCE_ERROR_UTILITY_INVALID_SYSTEM_PARAM_ID);
	}
	Memory::WriteOrException_U32(param, destaddr);
	return hleLogInfo(Log::sceUtility, 0, "(%s): %08x", SystemParamToString(id), param);
}

// ---------------------------------------------------------------- utility modules
static const int noDeps[] = {0};
static const int httpModuleDeps[] = {0x0102, 0x0103, 0x0104, 0};
static const int sslModuleDeps[] = {0x0102, 0};
static const int httpStorageModuleDeps[] = {0x00100, 0x0102, 0x0103, 0x0104, 0x0105, 0};
static const int atrac3PlusModuleDeps[] = {0x0300, 0};
static const int mpegBaseModuleDeps[] = {0x0300, 0};
static const int mp4ModuleDeps[] = {0x0300, 0};

ModuleLoadInfo::ModuleLoadInfo(int m, u32 s, const char *_name, ModuleLoadCallback n)
	: name(_name), mod(m), size(s), dependencies(noDeps), notify(n) {}
ModuleLoadInfo::ModuleLoadInfo(int m, u32 s, const char *_name, const int *d, ModuleLoadCallback n)
	: name(_name), mod(m), size(s), dependencies(d), notify(n) {}

static const ModuleLoadInfo moduleLoadInfo[] = {
	ModuleLoadInfo(0x100, 0x00014000, "net_common"),
	ModuleLoadInfo(0x101, 0x00020000, "net_adhoc"),
	ModuleLoadInfo(0x102, 0x00058000, "net_inet"),
	ModuleLoadInfo(0x103, 0x00006000, "net_parse_uri"),
	ModuleLoadInfo(0x104, 0x00002000, "net_parse_http"),
	ModuleLoadInfo(0x105, 0x00028000, "net_http", httpModuleDeps),
	ModuleLoadInfo(0x106, 0x00044000, "net_ssl", sslModuleDeps),
	ModuleLoadInfo(0x107, 0x00010000, "unk_0x107"),
	ModuleLoadInfo(0x108, 0x00008000, "usb_pspcm", httpStorageModuleDeps),
	ModuleLoadInfo(0x200, 0x00000000, "usb_mic"),
	ModuleLoadInfo(0x201, 0x00000000, "usb_cam"),
	ModuleLoadInfo(0x202, 0x00000000, "usb_gps"),
	ModuleLoadInfo(0x203, 0x00000000, "usb_unk_0x203"),
	ModuleLoadInfo(0x2ff, 0x00000000, "unk_0x2ff"),
	ModuleLoadInfo(0x300, 0x00000000, "av_avcodec"),
	ModuleLoadInfo(0x301, 0x00000000, "av_sascore"),
	ModuleLoadInfo(0x302, 0x00008000, "av_atrac3plus", atrac3PlusModuleDeps),
	ModuleLoadInfo(0x303, 0x0000c000, "av_mpegbase", mpegBaseModuleDeps),
	ModuleLoadInfo(0x304, 0x00004000, "av_mp3"),
	ModuleLoadInfo(0x305, 0x0000a300, "av_vaudio"),
	ModuleLoadInfo(0x306, 0x00004000, "av_aac"),
	ModuleLoadInfo(0x307, 0x00000000, "av_g729"),
	ModuleLoadInfo(0x308, 0x0003c000, "av_mp4", mp4ModuleDeps),
	ModuleLoadInfo(0x3fe, 0x00000000, "me_stuff"),
	ModuleLoadInfo(0x3ff, 0x00000000, "me_core"),
	ModuleLoadInfo(0x400, 0x0000c000, "np_common"),
	ModuleLoadInfo(0x401, 0x00018000, "np_service"),
	ModuleLoadInfo(0x402, 0x00048000, "np_matching2"),
	ModuleLoadInfo(0x403, 0x0000e000, "np_unk_0x403"),
	ModuleLoadInfo(0x500, 0x00000000, "np_drm"),
	ModuleLoadInfo(0x600, 0x00000000, "irda"),
	ModuleLoadInfo(0x601, 0x00000000, "unk_0x601"),
};

const ModuleLoadInfo *__UtilityModuleInfo(int module) {
	for (size_t i = 0; i < ARRAY_SIZE(moduleLoadInfo); ++i) {
		if (moduleLoadInfo[i].mod == module)
			return &moduleLoadInfo[i];
	}
	return nullptr;
}

const std::map<int, u32> &__UtilityGetLoadedModules() {
	return currentlyLoadedModules;
}

bool __UtilityModuleGetMemoryRange(int moduleID, u32 *startPtr, u32 *sizePtr) {
	return false;
}

static int LoadModuleInternal(u32 module, bool av) {
	const ModuleLoadInfo *info = __UtilityModuleInfo(module);
	if (!info)
		return av ? SCE_ERROR_AV_MODULE_BAD_ID : SCE_ERROR_MODULE_BAD_ID;
	if (currentlyLoadedModules.find(module) != currentlyLoadedModules.end()) {
		DEBUG_LOG(Log::sceUtility, "utility module %03x (%s) is already loaded", module, info->name);
		return 0;
	}
	for (const int *dep = info->dependencies; *dep != 0; ++dep) {
		if (currentlyLoadedModules.find(*dep) == currentlyLoadedModules.end())
			DEBUG_LOG(Log::sceUtility, "utility module %03x loaded without its dependency %03x", module, *dep);
	}
	currentlyLoadedModules[module] = 0;
	return 0;
}

static int UnloadModuleInternal(u32 module, bool av) {
	const ModuleLoadInfo *info = __UtilityModuleInfo(module);
	if (!info)
		return av ? SCE_ERROR_AV_MODULE_BAD_ID : SCE_ERROR_MODULE_BAD_ID;
	currentlyLoadedModules.erase(module);
	return 0;
}

static u32 sceUtilityLoadAvModule(u32 module) {
	if (module > 7)
		return hleLogError(Log::sceUtility, SCE_ERROR_AV_MODULE_BAD_ID, "invalid module id");
	return hleDelayResult(hleLogDebug(Log::sceUtility, LoadModuleInternal(0x300 | module, true)), "utility av module loaded", 25000);
}
static u32 sceUtilityUnloadAvModule(u32 module) {
	if (module > 7)
		return hleLogError(Log::sceUtility, SCE_ERROR_AV_MODULE_BAD_ID, "invalid module id");
	return hleDelayResult(hleLogDebug(Log::sceUtility, UnloadModuleInternal(0x300 | module, true)), "utility av module unloaded", 800);
}
static u32 sceUtilityLoadModule(u32 module) {
	int result = LoadModuleInternal(module, false);
	return hleDelayResult(hleLogDebug(Log::sceUtility, result), "utility module loaded", module == 0x3FF ? 130 : 25000);
}
static u32 sceUtilityUnloadModule(u32 module) {
	int result = UnloadModuleInternal(module, false);
	return hleDelayResult(hleLogDebug(Log::sceUtility, result), "utility module unloaded", module == 0x3FF ? 110 : 400);
}
static u32 sceUtilityLoadNetModule(u32 module) { return hleLogInfo(Log::sceUtility, 0, "FAKE"); }
static u32 sceUtilityUnloadNetModule(u32 module) { return hleLogInfo(Log::sceUtility, 0, "FAKE"); }
static u32 sceUtilityLoadUsbModule(u32 module) {
	if (module < 1 || module > 5)
		return hleLogError(Log::sceUtility, 0, "invalid module id");
	return hleLogWarning(Log::sceUtility, 0, "UNIMPL");
}
static u32 sceUtilityUnloadUsbModule(u32 module) {
	if (module < 1 || module > 5)
		return hleLogError(Log::sceUtility, 0, "invalid module id");
	return hleLogWarning(Log::sceUtility, 0, "UNIMPL");
}

// ---------------------------------------------------------------- NID table (PPSSPP's)
static const HLEFunction sceUtility[] = {
	{0X1579A159, &WrapU_U<sceUtilityLoadNetModule>,                "sceUtilityLoadNetModule",                'x', "x"  },
	{0X64D50C56, &WrapU_U<sceUtilityUnloadNetModule>,              "sceUtilityUnloadNetModule",              'x', "x"  },

	{0XF88155F6, &WrapI_V<sceUtilityNetconfShutdownStart>,         "sceUtilityNetconfShutdownStart",         'i', ""   },
	{0X4DB1E739, &WrapI_U<sceUtilityNetconfInitStart>,             "sceUtilityNetconfInitStart",             'i', "x"  },
	{0X91E70E35, &WrapI_I<sceUtilityNetconfUpdate>,                "sceUtilityNetconfUpdate",                'i', "i"  },
	{0X6332AA39, &WrapI_V<sceUtilityNetconfGetStatus>,             "sceUtilityNetconfGetStatus",             'i', ""   },
	{0X5EEE6548, &WrapI_I<sceUtilityCheckNetParam>,                "sceUtilityCheckNetParam",                'i', "i"  },
	{0X434D4B3A, &WrapI_IIU<sceUtilityGetNetParam>,                "sceUtilityGetNetParam",                  'i', "iix"},
	{0X4FED24D8, &WrapI_U<sceUtilityGetNetParamLatestID>,          "sceUtilityGetNetParamLatestID",          'i', "x"  },

	{0X67AF3428, &WrapI_V<sceUtilityMsgDialogShutdownStart>,       "sceUtilityMsgDialogShutdownStart",       'i', ""   },
	{0X2AD8E239, &WrapI_U<sceUtilityMsgDialogInitStart>,           "sceUtilityMsgDialogInitStart",           'i', "x"  },
	{0X95FC253B, &WrapI_I<sceUtilityMsgDialogUpdate>,              "sceUtilityMsgDialogUpdate",              'i', "i"  },
	{0X9A1C91D7, &WrapI_V<sceUtilityMsgDialogGetStatus>,           "sceUtilityMsgDialogGetStatus",           'i', ""   },
	{0X4928BD96, &WrapI_V<sceUtilityMsgDialogAbort>,               "sceUtilityMsgDialogAbort",               'i', ""   },

	{0X9790B33C, &WrapI_V<sceUtilitySavedataShutdownStart>,        "sceUtilitySavedataShutdownStart",        'i', ""   },
	{0X50C4CD57, &WrapI_U<sceUtilitySavedataInitStart>,            "sceUtilitySavedataInitStart",            'i', "x"  },
	{0XD4B95FFB, &WrapI_I<sceUtilitySavedataUpdate>,               "sceUtilitySavedataUpdate",               'i', "i"  },
	{0X8874DBE0, &WrapI_V<sceUtilitySavedataGetStatus>,            "sceUtilitySavedataGetStatus",            'i', ""   },

	{0X3DFAEBA9, &WrapI_V<sceUtilityOskShutdownStart>,             "sceUtilityOskShutdownStart",             'i', ""   },
	{0XF6269B82, &WrapI_U<sceUtilityOskInitStart>,                 "sceUtilityOskInitStart",                 'i', "x"  },
	{0X4B85C861, &WrapI_I<sceUtilityOskUpdate>,                    "sceUtilityOskUpdate",                    'i', "i"  },
	{0XF3F76017, &WrapI_V<sceUtilityOskGetStatus>,                 "sceUtilityOskGetStatus",                 'i', ""   },

	{0X41E30674, &WrapU_UU<sceUtilitySetSystemParamString>,        "sceUtilitySetSystemParamString",         'x', "xx" },
	{0X45C18506, &WrapU_UU<sceUtilitySetSystemParamInt>,           "sceUtilitySetSystemParamInt",            'x', "xx" },
	{0X34B78343, &WrapU_UUI<sceUtilityGetSystemParamString>,       "sceUtilityGetSystemParamString",         'x', "xxi"},
	{0XA5DA2406, &WrapU_UU<sceUtilityGetSystemParamInt>,           "sceUtilityGetSystemParamInt",            'x', "xx" },

	{0XC492F751, &WrapI_U<sceUtilityGameSharingInitStart>,         "sceUtilityGameSharingInitStart",         'i', "x"  },
	{0XEFC6F80F, &WrapI_V<sceUtilityGameSharingShutdownStart>,     "sceUtilityGameSharingShutdownStart",     'i', ""   },
	{0X7853182D, &WrapI_I<sceUtilityGameSharingUpdate>,            "sceUtilityGameSharingUpdate",            'i', "i"  },
	{0X946963F3, &WrapI_V<sceUtilityGameSharingGetStatus>,         "sceUtilityGameSharingGetStatus",         'i', ""   },

	{0X2995D020, nullptr,                                          "sceUtilitySavedataErrInitStart",         '?', ""   },
	{0XB62A4061, nullptr,                                          "sceUtilitySavedataErrShutdownStart",     '?', ""   },
	{0XED0FAD38, nullptr,                                          "sceUtilitySavedataErrUpdate",            '?', ""   },
	{0X88BC7406, nullptr,                                          "sceUtilitySavedataErrGetStatus",         '?', ""   },

	{0XBDA7D894, &WrapI_V<sceUtilityHtmlViewerGetStatus>,          "sceUtilityHtmlViewerGetStatus",          'i', ""   },
	{0XCDC3AA41, &WrapI_U<sceUtilityHtmlViewerInitStart>,          "sceUtilityHtmlViewerInitStart",          'i', "x"  },
	{0XF5CE1134, &WrapI_V<sceUtilityHtmlViewerShutdownStart>,      "sceUtilityHtmlViewerShutdownStart",      'i', ""   },
	{0X05AFB9E4, &WrapI_I<sceUtilityHtmlViewerUpdate>,             "sceUtilityHtmlViewerUpdate",             'i', "i"  },

	{0X16A1A8D8, nullptr,                                          "sceUtilityAuthDialogGetStatus",          '?', ""   },
	{0X943CBA46, nullptr,                                          "sceUtilityAuthDialogInitStart",          '?', ""   },
	{0X0F3EEAAC, nullptr,                                          "sceUtilityAuthDialogShutdownStart",      '?', ""   },
	{0X147F7C85, nullptr,                                          "sceUtilityAuthDialogUpdate",             '?', ""   },

	{0XC629AF26, &WrapU_U<sceUtilityLoadAvModule>,                 "sceUtilityLoadAvModule",                 'x', "x"  },
	{0XF7D8D092, &WrapU_U<sceUtilityUnloadAvModule>,               "sceUtilityUnloadAvModule",               'x', "x"  },

	{0X2A2B3DE0, &WrapU_U<sceUtilityLoadModule>,                   "sceUtilityLoadModule",                   'x', "x"  },
	{0XE49BFE92, &WrapU_U<sceUtilityUnloadModule>,                 "sceUtilityUnloadModule",                 'x', "x"  },

	{0X0251B134, &WrapI_U<sceUtilityScreenshotInitStart>,          "sceUtilityScreenshotInitStart",          'i', "x"  },
	{0XF9E0008C, &WrapI_V<sceUtilityScreenshotShutdownStart>,      "sceUtilityScreenshotShutdownStart",      'i', ""   },
	{0XAB083EA9, &WrapI_I<sceUtilityScreenshotUpdate>,             "sceUtilityScreenshotUpdate",             'i', "i"  },
	{0XD81957B7, &WrapI_V<sceUtilityScreenshotGetStatus>,          "sceUtilityScreenshotGetStatus",          'i', ""   },
	{0X86A03A27, &WrapI_U<sceUtilityScreenshotContStart>,          "sceUtilityScreenshotContStart",          'i', "x"  },

	{0X0D5BC6D2, &WrapU_U<sceUtilityLoadUsbModule>,                "sceUtilityLoadUsbModule",                'x', "x"  },
	{0XF64910F0, &WrapU_U<sceUtilityUnloadUsbModule>,              "sceUtilityUnloadUsbModule",              'x', "x"  },

	{0X24AC31EB, &WrapI_U<sceUtilityGamedataInstallInitStart>,     "sceUtilityGamedataInstallInitStart",     'i', "x"  },
	{0X32E32DCB, &WrapI_V<sceUtilityGamedataInstallShutdownStart>, "sceUtilityGamedataInstallShutdownStart", 'i', ""   },
	{0X4AECD179, &WrapI_I<sceUtilityGamedataInstallUpdate>,        "sceUtilityGamedataInstallUpdate",        'i', "i"  },
	{0XB57E95D9, &WrapI_V<sceUtilityGamedataInstallGetStatus>,     "sceUtilityGamedataInstallGetStatus",     'i', ""   },
	{0X180F7B62, &WrapI_V<sceUtilityGamedataInstallAbort>,         "sceUtilityGamedataInstallAbort",         'i', ""   },

	{0X16D02AF0, &WrapI_U<sceUtilityNpSigninInitStart>,            "sceUtilityNpSigninInitStart",            'i', "x"  },
	{0XE19C97D6, &WrapI_V<sceUtilityNpSigninShutdownStart>,        "sceUtilityNpSigninShutdownStart",        'i', ""   },
	{0XF3FBC572, &WrapI_I<sceUtilityNpSigninUpdate>,               "sceUtilityNpSigninUpdate",               'i', "i"  },
	{0X86ABDB1B, &WrapI_V<sceUtilityNpSigninGetStatus>,            "sceUtilityNpSigninGetStatus",            'i', ""   },

	{0X1281DA8E, &WrapV_U<sceUtilityInstallInitStart>,             "sceUtilityInstallInitStart",             'v', "x"  },
	{0X5EF1C24A, nullptr,                                          "sceUtilityInstallShutdownStart",         '?', ""   },
	{0XA03D29BA, nullptr,                                          "sceUtilityInstallUpdate",                '?', ""   },
	{0XC4700FA3, nullptr,                                          "sceUtilityInstallGetStatus",             '?', ""   },

	{0X54A5C62F, &WrapI_V<sceUtilityStoreCheckoutShutdownStart>,   "sceUtilityStoreCheckoutShutdownStart",   'i', ""   },
	{0XDA97F1AA, &WrapI_U<sceUtilityStoreCheckoutInitStart>,       "sceUtilityStoreCheckoutInitStart",       'i', "x"  },
	{0XB8592D5F, &WrapI_I<sceUtilityStoreCheckoutUpdate>,          "sceUtilityStoreCheckoutUpdate",          'i', "i"  },
	{0X3AAD51DC, &WrapI_V<sceUtilityStoreCheckoutGetStatus>,       "sceUtilityStoreCheckoutGetStatus",       'i', ""   },

	{0XD17A0573, nullptr,                                          "sceUtilityPS3ScanShutdownStart",         '?', ""   },
	{0X42071A83, nullptr,                                          "sceUtilityPS3ScanInitStart",             '?', ""   },
	{0XD852CDCE, nullptr,                                          "sceUtilityPS3ScanUpdate",                '?', ""   },
	{0X89317C8F, nullptr,                                          "sceUtilityPS3ScanGetStatus",             '?', ""   },

	{0XE1BC175E, nullptr,                                          "sceUtility_E1BC175E",                    '?', ""   },
	{0X43E521B7, nullptr,                                          "sceUtility_43E521B7",                    '?', ""   },
	{0XDB4149EE, nullptr,                                          "sceUtility_DB4149EE",                    '?', ""   },
	{0XCFE7C460, nullptr,                                          "sceUtility_CFE7C460",                    '?', ""   },

	{0XC130D441, nullptr,                                          "sceUtilityPsnShutdownStart",             '?', ""   },
	{0XA7BB7C67, nullptr,                                          "sceUtilityPsnInitStart",                 '?', ""   },
	{0X0940A1B9, nullptr,                                          "sceUtilityPsnUpdate",                    '?', ""   },
	{0X094198B8, nullptr,                                          "sceUtilityPsnGetStatus",                 '?', ""   },

	{0X9F313D14, nullptr,                                          "sceUtilityAutoConnectShutdownStart",     '?', ""   },
	{0X3A15CD0A, nullptr,                                          "sceUtilityAutoConnectInitStart",         '?', ""   },
	{0XD23665F4, nullptr,                                          "sceUtilityAutoConnectUpdate",            '?', ""   },
	{0XD4C2BD73, nullptr,                                          "sceUtilityAutoConnectGetStatus",         '?', ""   },
	{0X0E0C27AF, nullptr,                                          "sceUtilityAutoConnectAbort",             '?', ""   },

	{0X06A48659, nullptr,                                          "sceUtilityRssSubscriberShutdownStart",   '?', ""   },
	{0X4B0A8FE5, nullptr,                                          "sceUtilityRssSubscriberInitStart",       '?', ""   },
	{0XA084E056, nullptr,                                          "sceUtilityRssSubscriberUpdate",          '?', ""   },
	{0X2B96173B, nullptr,                                          "sceUtilityRssSubscriberGetStatus",       '?', ""   },

	{0X149A7895, nullptr,                                          "sceUtilityDNASShutdownStart",            '?', ""   },
	{0XDDE5389D, nullptr,                                          "sceUtilityDNASInitStart",                '?', ""   },
	{0X4A833BA4, nullptr,                                          "sceUtilityDNASUpdate",                   '?', ""   },
	{0XA50E5B30, nullptr,                                          "sceUtilityDNASGetStatus",                '?', ""   },

	{0XE7B778D8, nullptr,                                          "sceUtilityRssReaderShutdownStart",       '?', ""   },
	{0X81C44706, nullptr,                                          "sceUtilityRssReaderInitStart",           '?', ""   },
	{0X6F56F9CF, nullptr,                                          "sceUtilityRssReaderUpdate",              '?', ""   },
	{0X8326AB05, nullptr,                                          "sceUtilityRssReaderGetStatus",           '?', ""   },
	{0XB0FB7FF5, nullptr,                                          "sceUtilityRssReaderContStart",           '?', ""   },

	{0XBC6B6296, nullptr,                                          "sceNetplayDialogShutdownStart",          '?', ""   },
	{0X3AD50AE7, nullptr,                                          "sceNetplayDialogInitStart",              '?', ""   },
	{0X417BED54, nullptr,                                          "sceNetplayDialogUpdate",                 '?', ""   },
	{0XB6CEE597, nullptr,                                          "sceNetplayDialogGetStatus",              '?', ""   },

	{0X28D35634, nullptr,                                          "sceUtility_28D35634",                    '?', ""   },
	{0X70267ADF, nullptr,                                          "sceUtility_70267ADF",                    '?', ""   },
	{0XECE1D3E5, nullptr,                                          "sceUtility_ECE1D3E5",                    '?', ""   },
	{0XEF3582B2, nullptr,                                          "sceUtility_EF3582B2",                    '?', ""   },

	{0x05e242a1, nullptr,                                          "sceUtility_05e242a1",                    '?', ""   },
	{0x644b513b, nullptr,                                          "sceUtility_644b513b",                    '?', ""   },
	{0x043ebe3e, nullptr,                                          "sceUtility_043ebe3e",                    '?', ""   },
};

// ---------------------------------------------------------------- init
void utility_hle_init(void) {
	static bool inited;
	if (inited)
		return;
	inited = true;
	hle_bridge_init();

	saveDialog = new PSPSaveDialog(UtilityDialogType::SAVEDATA);
	msgDialog = new PSPMsgDialog(UtilityDialogType::MSG);
	oskDialog = new PSPOskDialog(UtilityDialogType::OSK);
	netDialog = new PSPPlaceholderDialog(UtilityDialogType::NET);
	screenshotDialog = new PSPPlaceholderDialog(UtilityDialogType::SCREENSHOT);
	gamedataInstallDialog = new PSPPlaceholderDialog(UtilityDialogType::GAMEDATAINSTALL);
	npSigninDialog = new PSPPlaceholderDialog(UtilityDialogType::NPSIGNIN);
	gameSharingDialog = new PSPPlaceholderDialog(UtilityDialogType::GAMESHARING);
	htmlViewerDialog = new PSPPlaceholderDialog(UtilityDialogType::HTMLVIEWER);
	currentDialogType = UtilityDialogType::NONE;
	DeactivateDialog();
	SavedataParam::Init();
	currentlyLoadedModules.clear();
	ResetSecondsSinceLastGameSave();

	__PPGeInit();

	RegisterHLEModule("sceUtility", ARRAY_SIZE(sceUtility), sceUtility);
	int n = hle_bridge_register_pending();
	LOGI("utility: PPSSPP sceUtility dialogs + PPGe, %d functions", n);
}
