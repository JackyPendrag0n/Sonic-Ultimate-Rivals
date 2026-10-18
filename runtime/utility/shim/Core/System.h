// Shim for PPSSPP's Core/System.h (rcsm utility extraction). GPL-2.0+.
#pragma once
#include <string>
#include <string_view>
#include "Common/CommonTypes.h"
#include "Common/File/Path.h"
#include "Core/CoreParameter.h"
#include "Core/ConfigValues.h"
#define PSP_CoreParameter utility_PSP_CoreParameter
CoreParameter &PSP_CoreParameter();
inline void DumpFileIfEnabled(const u8 *, const u32, std::string_view, DumpFileType) {}
// The game ID (from the savedata params' gameName) PPSSPP uses for a few compat decisions.
std::string GetGameID();
