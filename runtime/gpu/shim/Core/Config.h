// Shim for PPSSPP's Core/Config.h (rcsm GPU extraction): the settings the GE, the software renderer
// and the OpenGL backend read, with PPSSPP's defaults (software rendering until ge_select_backend()
// picks OpenGL, which switches the hardware-only ones on; no JIT, no frameskip). GPL-2.0+.
#pragma once
#include <string>
#include "ppsspp_config.h"
#include "Common/CommonTypes.h"
#include <map>
#include <vector>
#include "Core/ConfigValues.h"
#include "Common/System/Display.h"

// Presentation settings (PPSSPP defaults). The rcsm presenter stretches the PSP image over the
// rectangle the frontend asks for.
struct DisplayLayoutConfig {
	int iDisplayFilter = SCALE_LINEAR;
	bool bDisplayStretch = false;
	float fDisplayOffsetX = 0.5f;
	float fDisplayOffsetY = 0.5f;
	float fDisplayScale = 1.0f;
	bool bDisplayIntegerScale = false;
	float fDisplayAspectRatio = 1.0f;
	int iInternalScreenRotation = ROTATION_LOCKED_HORIZONTAL;
	bool bRotateControlsWithScreen = true;
	bool bIgnoreScreenInsets = true;
	bool bEnableCardboardVR = false;
	int iCardboardScreenSize = 50;
	int iCardboardXShift = 0;
	int iCardboardYShift = 0;
	bool bImmersiveMode = true;
	bool InternalRotationIsPortrait() const { return false; }
};

struct Config {
	bool bHardwareTransform = false;
	bool bSoftwareRendering = true;
	bool bIgnoreBadMemAccess = true;
	bool bSoftwareDisableDithering = false;
	bool bVertexDecoderJit = false;
	int iDepthRasterMode = 0;
	int iFrameSkip = 0;
	int iSplineBezierQuality = 2;
	int iTexFiltering = 1;  // TEX_FILTER_AUTO
	int iInternalResolution = 1;
	int iGPUBackend = 1;
	bool bSoftwareRenderingJit = false;
	bool bFastMemory = true;
	bool bTextureBackoffCache = false;
	bool bDisplayStretch = false;
	int iTexScalingLevel = 1;
	int iTexScalingType = 0;
	bool bTexDeposterize = false;
	bool bSmart2DTexFiltering = false;

	// Hardware backend settings (PPSSPP defaults unless noted).
	bool bVendorBugChecksEnabled = true;
	std::vector<std::string> vPostShaderNames;  // no post-processing shaders
	bool bSkipBufferEffects = false;
	int iAnisotropyLevel = 4;
	std::string sStereoToMonoShader = "RedBlue";
	std::map<std::string, float> mPostShaderSetting;
	int iReplacementTextureLoadSpeed = 0;
	float fHeadUpDisplayScale = 0.3f;
	bool bShaderCache = false;  // PPSSPP: true; there is no disc ID / cache directory here
	int iSkipGPUReadbackMode = 0;  // SKIP_GPU_READBACK_MODE: off
	int iMultiSampleLevel = 0;
	int iDebugOverlay = 0;
	int iBloomHack = 0;
	bool bUberShaderVertex = true;
	bool bSaveNewTextures = false;
	bool bReplaceTextures = false;
	bool bRescaleHUD = true;
	bool bHideSlowWarnings = false;
	bool bForceVR = false;
	bool bDisplayCropTo16x9 = false;  // PPSSPP: true (crops 2 lines at exactly 16:9); the frontend owns the layout

	DisplayLayoutConfig displayLayoutLandscape;
	const DisplayLayoutConfig &GetDisplayLayoutConfig(DeviceOrientation orientation) const { return displayLayoutLandscape; }
	DisplayLayoutConfig &GetDisplayLayoutConfig(DeviceOrientation orientation) { return displayLayoutLandscape; }
	bool IsPortrait() const { return false; }
};
extern Config g_Config;

