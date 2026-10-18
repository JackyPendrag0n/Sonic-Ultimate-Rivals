// Shim for PPSSPP's Core/CoreParameter.h (rcsm utility extraction). GPL-2.0+.
#pragma once
#include "Common/CommonTypes.h"
#include "Core/Compatibility.h"
#include "Common/File/Path.h"
#define CoreParameter UtilityCoreParameter
struct CoreParameter {
	Compatibility compat;
	bool headLess = true;   // PPGe: no host text renderer, draw text with the atlas font
	int renderScaleFactor = 1;
	int pixelWidth = 480;
	int pixelHeight = 272;
	Path fileToStart;
};
