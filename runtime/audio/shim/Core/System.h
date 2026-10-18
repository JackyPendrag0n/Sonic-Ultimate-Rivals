// Shim for PPSSPP's Core/System.h (rcsm audio extraction). GPL-2.0+.
#pragma once
#include <string_view>
#include "Common/CommonTypes.h"
#include "Core/CoreParameter.h"
#include "Core/ConfigValues.h"
#define PSP_CoreParameter audio_PSP_CoreParameter
CoreParameter &PSP_CoreParameter();
inline void DumpFileIfEnabled(const u8 *dataPtr, const u32 length, std::string_view name, DumpFileType type) {}
