// Shim for PPSSPP's Core/System.h (rcsm GPU extraction). GPL-2.0+.
#pragma once
#include <string_view>
#include "Common/CommonTypes.h"
#include "Core/CoreParameter.h"
#include "Core/ConfigValues.h"

extern bool g_coreCollectDebugStats;
CoreParameter &PSP_CoreParameter();
inline bool PSP_IsInited() { return true; }
GPUBackend GetGPUBackend();

// For the OpenGL backend.
#include <string>
#include "Common/File/Path.h"
enum GlobalUIState { UISTATE_MENU, UISTATE_PAUSEMENU, UISTATE_INGAME, UISTATE_EXIT, UISTATE_EXCEPTION };
inline GlobalUIState GetUIState() { return UISTATE_INGAME; }
inline bool Core_IsStepping() { return false; }
enum PSPDirectories { DIRECTORY_APP_CACHE = 18 };
Path GetSysDirectory(PSPDirectories directoryType);
// PPSSPP keys its GL shader cache by the disc ID; there is none here (so no cache file is used).
// Renamed so it can't meet libpsputility.a's real ParamSFOData.
struct GpuNoParamSFO {
	std::string GetDiscID() const { return std::string(); }
};
#define g_paramSFO rcsm_gpu_paramSFO
extern GpuNoParamSFO g_paramSFO;
