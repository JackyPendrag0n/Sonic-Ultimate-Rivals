// Stand-ins for the parts of PPSSPP that its OpenGL hardware renderer links against but that the rcsm
// runtime doesn't have or need: VR, post-processing shaders (none are used: PPSSPP's default), texture
// replacement packs and texture upscaling (off by default in PPSSPP), shader translation (only for
// post shaders), host UI/system queries, the on-screen display and file access (no shader cache).
//
// Symbols that libpsputility.a may also define are weak here.
//
// This file is part of the rcsm GPU extraction and, like the PPSSPP sources it links with, is
// licensed under the GPL 2.0 or later (see ../LICENSE.TXT).

#include <cstring>
#include <string>
#include <vector>

#include "Common/Log.h"
#include "Common/File/Path.h"
#include "Common/File/FileUtil.h"
#include "Common/File/VFS/VFS.h"
#include "Common/Data/Text/I18n.h"
#include "Common/GPU/Shader.h"
#include "Common/GPU/ShaderTranslation.h"
#include "Common/System/OSD.h"
#include "Common/System/System.h"
#include "Common/VR/PPSSPPVR.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "Core/HW/Display.h"
#include "GPU/GPU.h"
#include "GPU/Common/GPUDebugInterface.h"
#include "GPU/Common/PostShader.h"
#include "GPU/Common/ReplacedTexture.h"
#include "GPU/Common/TextureReplacer.h"
#include "GPU/Common/TextureScalerCommon.h"

// ------------------------------------------------------------------ host / system
volatile CoreState coreState = CORE_RUNNING_CPU;
GpuNoParamSFO g_paramSFO;
Path GetSysDirectory(PSPDirectories directoryType) { return Path(); }
std::string System_GetProperty(SystemProperty prop) { return std::string(); }
float System_GetPropertyFloat(SystemProperty prop) { return 0.0f; }

// Frame statistics for PPSSPP's debug overlay (not shown).
int __DisplayGetFlipCount() { return 0; }
int __DisplayGetVCount() { return 0; }
void __DisplayGetFPS(float *out_vps, float *out_fps, float *out_actual_fps) {
	if (out_vps) *out_vps = 60.0f;
	if (out_fps) *out_fps = 60.0f;
	if (out_actual_fps) *out_actual_fps = 60.0f;
}

// From GPU/GPU.cpp.
const char *RasterChannelToString(RasterChannel channel) {
	return channel == RASTER_COLOR ? "COLOR" : "DEPTH";
}

// From GPU/Common/GPUDebugInterface.cpp.
void GPUDebugBuffer::Free() {
	if (alloc_ && data_ != nullptr)
		delete[] data_;
	data_ = nullptr;
}
void GPUDebugBuffer::ZeroBytes() {
	_dbg_assert_(data_ != nullptr);
	memset(data_, 0, PixelSize() * stride_ * height_);
}

// The OSD shows PPSSPP's renderer warnings (e.g. shader compile errors); they're logged instead.
__attribute__((weak)) OnScreenDisplay g_OSD;
__attribute__((weak)) void OnScreenDisplay::Show(OSDType type, std::string_view text, std::string_view text2, std::string_view icon, float duration_s, const char *id) {
	if (type == OSDType::MESSAGE_ERROR || type == OSDType::MESSAGE_WARNING)
		WARN_LOG(Log::G3D, "%.*s %.*s", (int)text.size(), text.data(), (int)text2.size(), text2.data());
}

// English UI strings (see shim/Common/Data/Text/I18n.h).
std::string_view I18NCategory::T(std::string_view key, std::string_view def) {
	return def.empty() ? key : def;
}
const char *I18NCategory::T_cstr(const char *key, const char *def) {
	return def ? def : key;
}
std::shared_ptr<I18NCategory> GetI18NCategory(I18NCat cat) {
	static std::shared_ptr<I18NCategory> category = std::make_shared<I18NCategory>();
	return category;
}

// No assets (post-processing shader sources are the only thing read).
VFS g_VFS;
uint8_t *VFS::ReadFile(std::string_view filename, size_t *size) {
	*size = 0;
	return nullptr;
}
void VFS::Clear() {}
bool VFS::GetFileListing(std::string_view path, std::vector<File::FileInfo> *listing, const char *filter) { return false; }

// No files are written or read (no shader cache, no texture dumps).
namespace File {
__attribute__((weak)) FILE *OpenCFile(const Path &filename, const char *mode) { return nullptr; }
uint64_t IOFile::GetSize() { return 0; }
bool IOFile::Seek(int64_t off, int origin) { return false; }
}

// ------------------------------------------------------------------ VR (never enabled)
bool IsVREnabled() { return false; }
void PreprocessStepVR(void *step) {}
void SetVRCompat(VRCompatFlag flag, long value) {}
void *BindVRFramebuffer() { return nullptr; }
void PreVRFrameRender(int fboIndex) {}
void PostVRFrameRender() {}
int GetVRFBOIndex() { return 0; }
int GetVRPassesCount() { return 1; }
bool IsBigScreenVRMode() { return false; }
bool IsFlatVRGame() { return false; }
bool IsFlatVRScene() { return false; }
bool IsGameVRScene() { return false; }
bool IsImmersiveVRMode() { return false; }
bool Is2DVRObject(float *projMatrix, bool ortho) { return false; }
void UpdateVRParams(float *projMatrix) {}
void UpdateVRProjection(float *projMatrix, float *output) {}
void UpdateVRView(float *leftEye, float *rightEye) {}

// ------------------------------------------------------------------ post-processing (no shaders)
void ReloadAllPostShaderInfo(Draw::DrawContext *draw) {}
const ShaderInfo *GetPostShaderInfo(std::string_view name) { return nullptr; }
std::vector<const ShaderInfo *> GetFullPostShadersChain(const std::vector<std::string> &names) { return {}; }
void RemoveUnknownPostShaders(std::vector<std::string> *names) { names->clear(); }
void FixPostShaderOrder(std::vector<std::string> *names) {}
bool TranslateShader(std::string *dst, ShaderLanguage destLang, const ShaderLanguageDesc &desc, TranslatedShaderMetadata *destMetadata, std::string src, ShaderLanguage srcLang, ShaderStage stage, std::string *errorMessage) {
	if (errorMessage)
		*errorMessage = "shader translation is not available";
	return false;
}

// ------------------------------------------------------------------ texture replacement (disabled)
TextureReplacer::TextureReplacer(Draw::DrawContext *draw) {}
TextureReplacer::~TextureReplacer() {}
void TextureReplacer::NotifyConfigChanged() {}
u32 TextureReplacer::ComputeHash(u32 addr, int bufw, int w, int h, bool swizzled, GETextureFormat fmt, u16 maxSeenV) { return 0; }
ReplacedTexture *TextureReplacer::FindReplacement(ReplacementCacheKey key, int w, int h) { return nullptr; }
bool TextureReplacer::WillSave(const ReplacedTextureDecodeInfo &replacedInfo) const { return false; }
void TextureReplacer::NotifyTextureDecoded(ReplacedTexture *texture, const ReplacedTextureDecodeInfo &replacedInfo, const void *data, int pitch, int level, int origW, int origH, int scaledW, int scaledH) {}
void TextureReplacer::Decimate(ReplacerDecimateMode mode) {}
bool ReplacedTexture::Poll(double budget) { return false; }
bool ReplacedTexture::CopyLevelTo(int level, uint8_t *out, size_t outDataSize, int rowPitch) { return false; }

// ------------------------------------------------------------------ texture upscaling (off: 1x)
TextureScalerCommon::TextureScalerCommon() {}
TextureScalerCommon::~TextureScalerCommon() {}
void TextureScalerCommon::ScaleAlways(u32 *out, u32 *src, int width, int height, int *scaledWidth, int *scaledHeight, int factor) {
	memcpy(out, src, (size_t)width * height * 4);
	*scaledWidth = width;
	*scaledHeight = height;
}
