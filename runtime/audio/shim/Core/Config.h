// Shim for PPSSPP's Core/Config.h (rcsm audio extraction): fixed settings for SAS/Atrac. Renamed so
// it can't collide with the GPU library's own Config shim in the same binary. GPL-2.0+.
#pragma once
#include <string>
#include "ppsspp_config.h"
#include "Common/CommonTypes.h"
#include "Core/ConfigValues.h"

#define g_Config g_AudioConfig
struct AudioConfig {
	bool bSeparateSASThread = false;  // mix synchronously on the calling guest thread
	int iReverbVolume = 100;          // 100 = 1.0x
	bool bUseOldAtrac = false;        // use the newer Atrac2 context (like PPSSPP's default)
	bool bIgnoreBadMemAccess = true;
	bool bForceFfmpegForAudioDec = false;  // (sceMpeg/MP3/AAC in libpspmpeg.a) AT3/AT3+ use the in-tree decoder
};
extern AudioConfig g_Config;
