// Shim for PPSSPP's Core/CoreParameter.h (rcsm GPU extraction): only the fields the GE / software
// renderer read. Licensed GPL-2.0+ like the PPSSPP sources it stands in for.
#pragma once
#include <string>
#include "Common/CommonTypes.h"
#include "Core/Compatibility.h"

enum GPUCore : int {
	GPUCORE_GLES = 0,
	GPUCORE_SOFTWARE = 1,
	GPUCORE_DIRECTX11 = 3,
	GPUCORE_VULKAN = 4,
};

class GraphicsContext;
namespace Draw { class DrawContext; }

struct CoreParameter {
	GPUCore gpuCore = GPUCORE_SOFTWARE;
	bool bUseVertexDecoderJit = false;
	GraphicsContext *graphicsContext = nullptr;
	bool headLess = true;
	int renderScaleFactor = 1;
	int renderWidth = 480;
	int renderHeight = 272;
	int pixelWidth = 480;
	int pixelHeight = 272;
	Compatibility compat;
	u32 disabledGPUFeatures = 0;
	bool frozen = false;
};
