// Texture dumping and replacement for the hardware renderer, in place of PPSSPP's texture pack code.
//
// Every texture the game uploads is named by a hash of its pixels (and of its palette, for indexed
// textures), so the name stays the same wherever the game happens to load it in memory:
//     <palette hash 8 hex><pixel hash 8 hex>.png
// Dumping (ge_tex_configure with a dump folder) writes each new texture there once, as RGBA PNG.
// Replacing: a PNG with the same name in any of the replacement folders is used instead, at whatever
// size it has (so 2x or 4x versions of a texture work), with the later folder winning.
//
// Like the PPSSPP sources it links with, this file is licensed under the GPL 2.0 or later.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <dirent.h>
#include <sys/stat.h>
#include <png.h>

#include "Core/MemMap.h"
#include "GPU/Common/ReplacedTexture.h"
#include "GPU/Common/TextureReplacer.h"
#include "GPU/Common/TextureDecoder.h"
#include "ext/xxhash.h"

static std::unordered_map<std::string, std::string> g_replacements;   // file name -> path
static std::string g_dump_dir;
static std::unordered_set<std::string> g_dumped;
static std::mutex g_lock;
static TextureReplacer *g_replacer;

static void scan_dir(const std::string &dir) {
	DIR *d = opendir(dir.c_str());
	if (!d) return;
	while (struct dirent *e = readdir(d)) {
		std::string n = e->d_name;
		if (n.size() == 20 && !strcasecmp(n.c_str() + 16, ".png")) {
			for (auto &ch : n) ch = (char)tolower((unsigned char)ch);
			g_replacements[n] = dir + "/" + e->d_name;
		}
	}
	closedir(d);
}

// dirs: replacement folders, lowest priority first. dump_dir: NULL or "" to not dump.
extern "C" int ge_tex_configure(const char *const *dirs, int ndirs, const char *dump_dir) {
	std::lock_guard<std::mutex> guard(g_lock);
	g_replacements.clear();
	for (int i = 0; i < ndirs; i++) scan_dir(dirs[i]);
	g_dump_dir = dump_dir ? dump_dir : "";
	g_dumped.clear();
	if (!g_dump_dir.empty()) {
		mkdir(g_dump_dir.c_str(), 0755);
		DIR *d = opendir(g_dump_dir.c_str());   // don't rewrite what an earlier run dumped
		while (d) {
			struct dirent *e = readdir(d);
			if (!e) break;
			g_dumped.insert(e->d_name);
		}
		if (d) closedir(d);
	}
	if (g_replacer) g_replacer->NotifyConfigChanged();
	return (int)g_replacements.size();
}

static std::string tex_name(ReplacementCacheKey key) {
	char buf[32];
	snprintf(buf, sizeof buf, "%08x%08x.png", key.ClutHash(), key.ContentsHash());
	return buf;
}

static bool load_png(const std::string &path, std::vector<uint8_t> &out, int *w, int *h) {
	png_image img;
	memset(&img, 0, sizeof img);
	img.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&img, path.c_str())) return false;
	img.format = PNG_FORMAT_RGBA;
	out.resize(PNG_IMAGE_SIZE(img));
	if (!png_image_finish_read(&img, nullptr, out.data(), 0, nullptr)) { png_image_free(&img); return false; }
	*w = (int)img.width;
	*h = (int)img.height;
	return true;
}

TextureReplacer::TextureReplacer(Draw::DrawContext *draw) { g_replacer = this; NotifyConfigChanged(); }
TextureReplacer::~TextureReplacer() {
	if (g_replacer == this) g_replacer = nullptr;
	for (auto &it : levelCache_) delete it.second;
}

void TextureReplacer::NotifyConfigChanged() {
	replaceEnabled_ = !g_replacements.empty();
	saveEnabled_ = !g_dump_dir.empty();
	ignoreAddress_ = true;
	cache_.clear();
	for (auto &it : levelCache_) delete it.second;
	levelCache_.clear();
}

u32 TextureReplacer::ComputeHash(u32 addr, int bufw, int w, int h, bool swizzled, GETextureFormat fmt, u16 maxSeenV) {
	if (h == 512 && maxSeenV < 512 && maxSeenV != 0) h = (int)maxSeenV;
	const u8 *p = Memory::GetPointerUnchecked(addr);
	const u32 bpp = textureBitsPerPixel[fmt];
	if (bufw <= w) {
		const u32 pixels = bufw * h + (w - bufw);
		return XXH32(p, bpp * pixels / 8, 0xBACD7814);
	}
	// rows with gaps between them: hash each row
	const u32 line = bpp * w / 8, stride = bpp * bufw / 8;
	u32 result = 0;
	for (int y = 0; y < h; y++, p += stride) result = (result * 11) ^ XXH32(p, line, 0xBACD7814);
	return result;
}

ReplacedTexture *TextureReplacer::FindReplacement(ReplacementCacheKey key, int w, int h) {
	key.ZeroAddress();
	std::string name = tex_name(key);
	auto it = levelCache_.find(name);
	if (it != levelCache_.end()) return it->second;
	std::string path;
	{
		std::lock_guard<std::mutex> guard(g_lock);
		auto f = g_replacements.find(name);
		if (f == g_replacements.end()) return nullptr;
		path = f->second;
	}
	ReplacementDesc desc{};
	desc.w = w;
	desc.h = h;
	desc.cacheKey = key;
	desc.logId = name;
	ReplacedTexture *t = new ReplacedTexture(nullptr, desc);
	std::vector<uint8_t> px;
	int iw = 0, ih = 0;
	if (load_png(path, px, &iw, &ih) && iw > 0 && ih > 0) {
		ReplacedTextureLevel lv;
		lv.w = lv.fullW = iw;
		lv.h = lv.fullH = ih;
		lv.fullDataSize = iw * ih * 4;
		t->levels_.push_back(lv);
		t->data_.push_back(std::move(px));
		t->fmt = Draw::DataFormat::R8G8B8A8_UNORM;
		t->alphaStatus_ = TextureAlpha::Any;
		t->state_ = ReplacementState::ACTIVE;
	} else {
		fprintf(stderr, "texture replacement %s couldn't be read as PNG\n", path.c_str());
		t->state_ = ReplacementState::NOT_FOUND;
	}
	levelCache_[name] = t;
	return t;
}

bool TextureReplacer::WillSave(const ReplacedTextureDecodeInfo &info) const {
	if (!saveEnabled_ || info.isVideo || !info.isFinal) return false;
	ReplacementCacheKey key(info.cachekey, info.hash);
	key.ZeroAddress();
	std::lock_guard<std::mutex> guard(g_lock);
	return !g_dumped.count(tex_name(key));
}

void TextureReplacer::NotifyTextureDecoded(ReplacedTexture *texture, const ReplacedTextureDecodeInfo &info, const void *data, int pitch, int level, int origW, int origH, int scaledW, int scaledH) {
	if (level != 0 || info.fmt != Draw::DataFormat::R8G8B8A8_UNORM || !WillSave(info)) return;
	ReplacementCacheKey key(info.cachekey, info.hash);
	key.ZeroAddress();
	std::string name = tex_name(key);
	{
		std::lock_guard<std::mutex> guard(g_lock);
		g_dumped.insert(name);
	}
	png_image img;
	memset(&img, 0, sizeof img);
	img.version = PNG_IMAGE_VERSION;
	img.width = origW;
	img.height = origH;
	img.format = PNG_FORMAT_RGBA;
	std::string path = g_dump_dir + "/" + name;
	if (!png_image_write_to_file(&img, path.c_str(), 0, data, pitch, nullptr))
		fprintf(stderr, "couldn't write %s\n", path.c_str());
}

void TextureReplacer::Decimate(ReplacerDecimateMode mode) {}

ReplacedTexture::ReplacedTexture(VFSBackend *vfs, const ReplacementDesc &desc) : desc_(desc) { logId_ = desc.logId; }
ReplacedTexture::~ReplacedTexture() {}
bool ReplacedTexture::Poll(double budget) { return true; }
bool ReplacedTexture::CopyLevelTo(int level, uint8_t *out, size_t outDataSize, int rowPitch) {
	if (state_ != ReplacementState::ACTIVE || level < 0 || (size_t)level >= levels_.size()) return false;
	const ReplacedTextureLevel &lv = levels_[level];
	const uint8_t *src = data_[level].data();
	const int row = lv.w * 4;
	if (rowPitch < row) return false;
	for (int y = 0; y < lv.h; y++) {
		if (outDataSize && (size_t)(y * rowPitch + row) > outDataSize) return false;
		memcpy(out + (size_t)y * rowPitch, src + (size_t)y * row, row);
	}
	return true;
}
