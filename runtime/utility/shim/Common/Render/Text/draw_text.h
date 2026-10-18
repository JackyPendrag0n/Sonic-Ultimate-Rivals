// Shim for PPSSPP's Common/Render/Text/draw_text.h (rcsm utility extraction). PPGe runs "headless"
// here, so its host text renderer is never created (TextDrawer::Create isn't even called) and all text
// is drawn with the PPGe atlas font. This only declares what PPGeDraw.cpp's unused paths mention.
// GPL-2.0+.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "Common/GPU/thin3d_min.h"

enum class FontFamily { SansSerif = 0 };
enum class FontStyleFlags { Default = 0 };
struct FontStyle {
	FontStyle(FontFamily family, int size, FontStyleFlags flags) {}
};
enum {
	ALIGN_LEFT = 0, ALIGN_RIGHT = 16, ALIGN_TOP = 0, ALIGN_BOTTOM = 1, ALIGN_HCENTER = 4, ALIGN_VCENTER = 8,
	ALIGN_VBASELINE = 32, ALIGN_CENTER = ALIGN_HCENTER | ALIGN_VCENTER, FLAG_WRAP_TEXT = 2048, FLAG_ELLIPSIZE_TEXT = 4096,
	ALIGN_TOPLEFT = ALIGN_TOP | ALIGN_LEFT,
};
struct Bounds {
	Bounds() {}
	Bounds(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}
	float x = 0, y = 0, w = 0, h = 0;
};
struct TextStringEntry {
	TextStringEntry(int frameCount) : lastUsedFrame(frameCount) {}
	int width = 0, height = 0, bmWidth = 0, bmHeight = 0, lastUsedFrame;
};
class TextDrawer {
public:
	static TextDrawer *Create(Draw::DrawContext *draw) { return nullptr; }
	void SetFontScale(float x, float y) {}
	void SetForcedDPIScale(float s) {}
	void SetOrCreateFont(const FontStyle &style) {}
	void MeasureString(std::string_view str, float *w, float *h) { *w = *h = 0; }
	void MeasureStringRect(std::string_view str, float maxWidth, float *w, float *h, int align = 0) { *w = *h = 0; }
	bool DrawStringBitmapRect(std::vector<uint8_t> &bitmapData, TextStringEntry &entry, Draw::DataFormat texFormat, std::string_view str, const Bounds &bounds, int align, bool fullColor) { return false; }
	void OncePerFrame() {}
};
