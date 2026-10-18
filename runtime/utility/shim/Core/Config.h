// Shim for PPSSPP's Core/Config.h (rcsm utility extraction): fixed system settings for the utility
// dialogs (English, cross confirms, nickname PLAYER, unencrypted saves). Renamed so it can't collide
// with the other libraries' Config shims. GPL-2.0+.
#pragma once
#include <string>
#include "ppsspp_config.h"
#include "Common/CommonTypes.h"
#include "Core/ConfigValues.h"

#define g_Config g_UtilityConfig
struct UtilityConfig {
	int iLanguage = 1;                 // PSP_SYSTEMPARAM_LANGUAGE_ENGLISH
	std::string sLanguageIni = "en_US";
	int iButtonPreference = 1;         // PSP_SYSTEMPARAM_BUTTON_CROSS
	std::string sNickName = "PLAYER";
	int iDateFormat = 0;               // YYYYMMDD
	int iTimeFormat = 0;               // 24h
	int iTimeZone = 0;
	bool bDayLightSavings = false;
	int iLockParentalLevel = 9;
	int iWlanAdhocChannel = 0;
	bool bWlanPowerSave = false;
	bool bEncryptSave = false;         // saves are stored unencrypted
	bool bSavedataUpgrade = true;
	bool bSoftwareRendering = true;
	bool bShowSaveLoadIndicator = false;
	bool bFullScreen = false;
	bool bBypassOSKWithKeyboard = false;
	int iIOTimingMethod = 0;           // IOTIMING_FAST
	bool bIgnoreBadMemAccess = true;
	int iMemStickSizeGB = 1;   // Sonic Rivals 2 multiplies free clusters by cluster size in 32 bits: 16 GB wraps to 0
};
extern UtilityConfig g_Config;
