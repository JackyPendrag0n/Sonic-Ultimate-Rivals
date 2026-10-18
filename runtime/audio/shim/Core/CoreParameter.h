// Shim for PPSSPP's Core/CoreParameter.h (rcsm audio extraction): only the compat flags. GPL-2.0+.
#pragma once
#include "Common/CommonTypes.h"
#include "Core/Compatibility.h"
#define CoreParameter AudioCoreParameter
struct CoreParameter {
	Compatibility compat;
};
