// Native GE renderer: the parts the OpenGL and Vulkan sides share (see ng_common.h).
#include "ng_common.h"
#include "ng_tex.h"
#include <unordered_map>
#include <string>
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// ================================================================= draw state
u64 ng_draw_key(const NgLayout &L, bool through, bool clear) {
	const GeState &g = g_ge;
	u64 key = 0;
	if (through) key |= K_THROUGH;
	if (L.col) key |= K_COLOR;
	if (clear) {
		key |= K_CLEAR | K_COLOR;
		if (!L.col) key &= ~K_COLOR;
		return key;
	}
	if (L.nrm) key |= K_NORMAL;
	if (!(g.cmd[CMD_SHADE] & 1)) key |= K_FLAT;
	if (!through) {
		if (g.cmd[CMD_LIGHTING_EN] & 1) {
			key |= K_LIGHTING | (u64)(g.cmd[CMD_MATUPDATE] & 7) << K_MATUPD_SHIFT;
			if (g.cmd[CMD_LIGHTMODE] & 1) key |= K_LMODE;
			for (int i = 0; i < 4; i++)
				if (g.cmd[CMD_LIGHT_EN0 + i] & 1) {
					u32 lt = g.cmd[CMD_LTYPE0 + i];
					u64 bits = 1 | ((lt >> 8) & 3) << 1 | (lt & 3) << 3;
					key |= bits << (K_LIGHT_SHIFT + i * 5);
				}
		}
		if (g.cmd[CMD_FOG_EN] & 1) key |= K_FOG;
		if (g.cmd[CMD_REVNORMAL] & 1) key |= K_REVNORMAL;
	}
	u32 at = (g.cmd[CMD_ATEST_EN] & 1) ? (g.cmd[CMD_ATEST] & 7) : 1;
	key |= (u64)at << K_ATEST_SHIFT;
	return key;
}

u64 ng_tex_key_bits(bool through) {
	const GeState &g = g_ge;
	u64 key = K_TEX;
	u32 tf = g.cmd[CMD_TEXFUNC];
	key |= (u64)(tf & 7) << K_TEXFUNC_SHIFT;
	if (tf & 0x100) key |= K_TEXALPHA;
	if (tf & 0x10000) key |= K_DOUBLE;
	if (!through) {
		int map = g.cmd[CMD_TEXMAPMODE] & 3, src = (g.cmd[CMD_TEXMAPMODE] >> 8) & 3;
		if (map == 1) key |= (u64)1 << K_TEXMAP_SHIFT | (u64)src << K_PROJSRC_SHIFT | K_TEXPROJ;
	}
	return key;
}

static void mat43(float *out, const float *m) {   // GE 4x3 (column major) -> mat4
	out[0] = m[0]; out[1] = m[1]; out[2] = m[2]; out[3] = 0;
	out[4] = m[3]; out[5] = m[4]; out[6] = m[5]; out[7] = 0;
	out[8] = m[6]; out[9] = m[7]; out[10] = m[8]; out[11] = 0;
	out[12] = m[9]; out[13] = m[10]; out[14] = m[11]; out[15] = 1;
}

static void rgb(float *out, u32 c, float a = 0) {
	out[0] = (c & 0xFF) / 255.0f; out[1] = ((c >> 8) & 0xFF) / 255.0f; out[2] = ((c >> 16) & 0xFF) / 255.0f; out[3] = a;
}

void ng_fill_ubo(Ubo &u, int fbw, int fbh, bool through, const float viewOff[2]) {
	const GeState &g = g_ge;
	u.fbsize[0] = (float)fbw; u.fbsize[1] = (float)fbh;
	if (!through) {
		mat43(u.world, g.world);
		float view[12];
		memcpy(view, g.view, sizeof view);
		view[9] += viewOff[0]; view[10] += viewOff[1];
		mat43(u.view, view);
		memcpy(u.proj, g.proj, sizeof u.proj);
		mat43(u.tgen, g.tgen);
		u.vpscale[0] = g.f(CMD_VP_XS); u.vpscale[1] = g.f(CMD_VP_YS); u.vpscale[2] = g.f(CMD_VP_ZS); u.vpscale[3] = 0;
		u.vpcenter[0] = g.f(CMD_VP_XC) - g.offsetX16() / 16.0f;
		u.vpcenter[1] = g.f(CMD_VP_YC) - g.offsetY16() / 16.0f;
		u.vpcenter[2] = g.f(CMD_VP_ZC); u.vpcenter[3] = 0;
		u.uvso[0] = g.f(CMD_TEX_SU); u.uvso[1] = g.f(CMD_TEX_SV); u.uvso[2] = g.f(CMD_TEX_OU); u.uvso[3] = g.f(CMD_TEX_OV);
		u32 ma = (g.cmd[CMD_MAT_AMBIENT] & 0xFFFFFF) | (g.cmd[CMD_MAT_ALPHA] & 0xFF) << 24;
		rgb(u.matamb, ma, (ma >> 24) / 255.0f);
		rgb(u.matdiff, g.cmd[CMD_MAT_DIFFUSE]);
		rgb(u.matspec, g.cmd[CMD_MAT_SPECULAR], g.f(CMD_MAT_SPECCOEF));
		rgb(u.matemis, g.cmd[CMD_MAT_EMISSIVE]);
		rgb(u.ambient, g.cmd[CMD_AMBIENT], (g.cmd[CMD_AMBIENT_A] & 0xFF) / 255.0f);
		for (int i = 0; i < 4; i++) {
			float p[3] = {g.f(CMD_LX0 + i * 3), g.f(CMD_LX0 + i * 3 + 1), g.f(CMD_LX0 + i * 3 + 2)};
			if (((g.cmd[CMD_LTYPE0 + i] >> 8) & 3) == 0) {
				float l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
				if (l > 0) for (float &c : p) c /= l;
			}
			memcpy(u.lpos[i], p, 12);
			float d[3] = {g.f(CMD_LDX0 + i * 3), g.f(CMD_LDX0 + i * 3 + 1), g.f(CMD_LDX0 + i * 3 + 2)};
			float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			if (l > 0) for (float &c : d) c /= l;
			memcpy(u.ldir[i], d, 12);
			u.latt[i][0] = g.f(CMD_LKA0 + i * 3); u.latt[i][1] = g.f(CMD_LKA0 + i * 3 + 1); u.latt[i][2] = g.f(CMD_LKA0 + i * 3 + 2);
			u.lspot[i][0] = g.f(CMD_LKO0 + i); u.lspot[i][1] = std::min(g.f(CMD_LKS0 + i), 496.0f);
			rgb(u.lamb[i], g.cmd[CMD_LAC0 + i * 3]);
			rgb(u.ldiff[i], g.cmd[CMD_LDC0 + i * 3]);
			rgb(u.lspec[i], g.cmd[CMD_LSC0 + i * 3]);
		}
		u.fog[0] = g.f(CMD_FOG1); u.fog[1] = g.f(CMD_FOG2);
		if (!std::isfinite(u.fog[1])) u.fog[1] = u.fog[1] < 0 ? -1e30f : 1e30f;
		rgb(u.fogcolor, g.cmd[CMD_FOGCOLOR]);
	} else {
		u32 ma = (g.cmd[CMD_MAT_AMBIENT] & 0xFFFFFF) | (g.cmd[CMD_MAT_ALPHA] & 0xFF) << 24;
		rgb(u.matamb, ma, (ma >> 24) / 255.0f);
	}
	rgb(u.texenv, g.cmd[CMD_TEXENV]);
	u.atest[0] = (g.cmd[CMD_ATEST] >> 8) & 0xFF; u.atest[1] = (g.cmd[CMD_ATEST] >> 16) & 0xFF;
}

// ================================================================= primitives
int ng_assemble(int prim, const NgVert *v, int count, bool through, std::vector<NgVert> &verts, std::vector<u16> &idx,
                NgMode *mode, bool lastFirst) {
	idx.clear();
	verts.clear();
	*mode = NG_TRIANGLES;
	auto tri = [&](int a, int b, int c) {
		if (lastFirst) { idx.push_back(c); idx.push_back(a); idx.push_back(b); }
		else { idx.push_back(a); idx.push_back(b); idx.push_back(c); }
	};
	auto line = [&](int a, int b) {
		if (lastFirst) std::swap(a, b);
		idx.push_back(a); idx.push_back(b);
	};
	switch (prim) {
	case PRIM_POINTS: *mode = NG_POINTS; verts.assign(v, v + count); for (int i = 0; i < count; i++) idx.push_back(i); break;
	case PRIM_LINES: *mode = NG_LINES; verts.assign(v, v + count); for (int i = 0; i + 1 < count; i += 2) line(i, i + 1); break;
	case PRIM_LINE_STRIP: *mode = NG_LINES; verts.assign(v, v + count); for (int i = 0; i + 1 < count; i++) line(i, i + 1); break;
	case PRIM_TRIANGLES: verts.assign(v, v + count); for (int i = 0; i + 2 < count; i += 3) tri(i, i + 1, i + 2); break;
	case PRIM_TRI_STRIP:
		verts.assign(v, v + count);
		for (int i = 0; i + 2 < count; i++) {
			if (i & 1) tri(i + 1, i, i + 2);
			else tri(i, i + 1, i + 2);
		}
		break;
	case PRIM_TRI_FAN:
		verts.assign(v, v + count);
		for (int i = 1; i + 1 < count; i++) tri(0, i, i + 1);
		break;
	case PRIM_RECTANGLES:
		if (!through) {
			static bool warned;
			if (!warned) { warned = true; NG_LOGE("ngpu: rectangles outside through mode aren't supported"); }
			return 0;
		}
		for (int i = 0; i + 1 < count; i += 2) {
			const NgVert &a = v[i], &b = v[i + 1];
			int base = (int)verts.size();
			NgVert c[4] = {b, b, b, b};   // colour, z (and the rest) from the second vertex
			c[0].pos[0] = a.pos[0]; c[0].pos[1] = a.pos[1]; c[0].uv[0] = a.uv[0]; c[0].uv[1] = a.uv[1];
			c[1].pos[0] = b.pos[0]; c[1].pos[1] = a.pos[1]; c[1].uv[0] = b.uv[0]; c[1].uv[1] = a.uv[1];
			c[2].pos[0] = a.pos[0]; c[2].pos[1] = b.pos[1]; c[2].uv[0] = a.uv[0]; c[2].uv[1] = b.uv[1];
			c[3].pos[0] = b.pos[0]; c[3].pos[1] = b.pos[1];
			verts.insert(verts.end(), c, c + 4);
			const int t[6] = {0, 1, 2, 1, 3, 2};
			for (int j : t) idx.push_back(base + j);   // (all four corners have the same colour)
		}
		break;
	default: return 0;
	}
	return (int)idx.size();
}

bool ng_pixel_mapped(int prim, const NgVert *v, int count, bool through) {
	if (prim != PRIM_RECTANGLES || !through || count < 2) return false;
	for (int i = 0; i + 1 < count; i += 2) {
		float dx = v[i + 1].pos[0] - v[i].pos[0], dy = v[i + 1].pos[1] - v[i].pos[1];
		float du = v[i + 1].uv[0] - v[i].uv[0], dv = v[i + 1].uv[1] - v[i].uv[1];
		if (dx <= 0 || dy <= 0 || (dx != du && dy != dv)) return false;
	}
	return true;
}

// ================================================================= CLUT and textures
static u8 s_clutRaw[1024];
static u64 s_clutHash;
static u32 s_clutTotal, s_clutMax;   // bytes the last CLUT load brought, and the most any did
static std::vector<u32> s_decodeBuf;

void ng_clut_load(u32 addr, int bytes) {
	if (bytes > 1024) bytes = 1024;
	if (bytes <= 0 || !ng_valid(addr, bytes)) return;
	memcpy(s_clutRaw, ng_ptr(addr), bytes);
	s_clutHash = ng_hash(s_clutRaw, 1024);
	s_clutTotal = bytes;
	s_clutMax = std::max<u32>(s_clutMax, bytes);
}

u64 ng_clut_hash(void) { return s_clutHash; }

void ng_palette(u32 pal[256]) {
	const GeState &g = g_ge;
	for (int i = 0; i < 256; i++) {
		if (g.clutFormat() == 3) memcpy(&pal[i], s_clutRaw + i * 4, 4);
		else { u16 c; memcpy(&c, s_clutRaw + i * 2, 2); pal[i] = ng_color16(c, g.clutFormat()); }
	}
}

// ---- texture replacement and dumping (gpu/tex_replace.cpp keeps the folders): textures are named as the
// PPSSPP renderer names them, "<palette hash><contents hash>.png", so packs work with both
extern "C" int ge_tex_replace_active(void);
extern "C" int ge_tex_find(u32 addr, const char *name, char *path, int size);
extern "C" void ge_tex_dump(const char *name, const void *rgba, int w, int h);
extern "C" int ge_tex_load_png(const char *path, unsigned char **rgba, int *w, int *h);

static const int kTexBits[16] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0};

// The texture's row pitch in texels, as the GE reads it (16-byte aligned).
static int tex_bufw(int fmt, u32 raw) {
	if (fmt >= FMT_DXT1) return raw & 0x7FF;
	int bits = kTexBits[fmt];
	int bufw = raw & 0x7FF & ~((128 / bits) - 1);
	return bufw ? bufw : 128 / bits;
}

static void replacement_name(char *name, u32 addr, int fmt, int w, int h, int bufw) {
	const GeState &g = g_ge;
	u32 bits = kTexBits[fmt];
	const u8 *p = ng_ptr(addr);
	u32 contents;
	if (bufw <= w) {
		contents = ng_xxh32(p, bits * (bufw * h + (w - bufw)) / 8, 0xBACD7814);
	} else {
		u32 line = bits * w / 8, stride = bits * bufw / 8;
		contents = 0;
		for (int y = 0; y < h; y++, p += stride) contents = (contents * 11) ^ ng_xxh32(p, line, 0xBACD7814);
	}
	u32 clut = g.cmd[CMD_TEXSIZE0] & 0xF0F;
	if (fmt >= FMT_CLUT4 && fmt <= FMT_CLUT32) {
		u32 baseBytes = g.clutBase() * (g.clutFormat() == 3 ? 4 : 2);
		u32 bytes = std::min(s_clutTotal + baseBytes, std::min<u32>(s_clutMax, 1024));
		clut ^= ng_xxh32(s_clutRaw, bytes, 0xC0108888) ^ g.cmd[CMD_CLUTFORMAT];
	}
	sprintf(name, "%08x%08x.png", clut, contents);
}

struct LoadedPng { std::vector<u32> px; int w = 0, h = 0; };
static std::unordered_map<std::string, LoadedPng> s_pngs;

void ng_tex_forget_replacements(void) { s_pngs.clear(); }

bool ng_tex_ref(NgTexRef *t) {
	const GeState &g = g_ge;
	t->addr = g.texAddr(0);
	t->fmt = g.texFormat(); t->w = g.texW(0); t->h = g.texH(0);
	t->stride = tex_bufw(t->fmt, g.cmd[CMD_TEXBW0]);
	t->bytes = ng_tex_bytes(t->fmt, std::max(t->stride, 1), g.texSwizzle() ? (t->h + 7) & ~7 : t->h);
	if (t->stride <= 0 || !ng_valid(t->addr, t->bytes)) return false;
	t->key = ng_hash(&t->addr, 4, (u64)t->fmt << 56 ^ (u64)t->w << 40 ^ (u64)t->h << 24 ^ (u64)t->stride << 8 ^ (g.texSwizzle() ? 1 : 0));
	if (t->fmt >= FMT_CLUT4 && t->fmt <= FMT_CLUT32) t->key = ng_hash(&s_clutHash, 8, t->key ^ (g.cmd[CMD_CLUTFORMAT] & 0x1FFFFF));
	return true;
}

const u32 *ng_tex_pixels(const NgTexRef &t, int *pw, int *ph) {
	const GeState &g = g_ge;
	int fmt = t.fmt, w = t.w, h = t.h;
	NgClut clut;
	if (fmt >= FMT_CLUT4 && fmt <= FMT_CLUT32) ng_clut_set(&clut, s_clutRaw, g.clutFormat(), g.clutShift(), g.clutMask(), g.clutBase());
	s_decodeBuf.resize((size_t)w * h);
	if (!ng_tex_decode(ng_ptr(t.addr), fmt, w, h, t.stride, g.texSwizzle(), &clut, s_decodeBuf.data())) {
		static bool warned[16];
		if (!warned[fmt & 15]) { warned[fmt & 15] = true; NG_LOGE("ngpu: texture format %d isn't supported", fmt); }
		std::fill(s_decodeBuf.begin(), s_decodeBuf.end(), 0xFFFF00FFu);
	}
	*pw = w; *ph = h;
	if (ge_tex_replace_active()) {
		char name[32], path[1024];
		replacement_name(name, t.addr, fmt, w, h, t.stride);
		if (ge_tex_find(t.addr, name, path, sizeof path)) {
			LoadedPng &png = s_pngs[path];
			if (!png.w) {
				unsigned char *data = nullptr;
				if (ge_tex_load_png(path, &data, &png.w, &png.h)) {
					png.px.assign((u32 *)data, (u32 *)data + (size_t)png.w * png.h);
					free(data);
				} else {
					NG_LOGE("texture replacement %s couldn't be read as PNG", path);
					png.w = -1;
				}
			}
			if (png.w > 0) { *pw = png.w; *ph = png.h; return png.px.data(); }
		} else {
			ge_tex_dump(name, s_decodeBuf.data(), w, h);
		}
	}
	return s_decodeBuf.data();
}

// ================================================================= framebuffer memory
void ng_fb_from_memory(u32 addr, int stride, int fmt, int w, int h, u32 *out) {
	int bpp = ng_fmt_bpp(fmt);
	for (int y = 0; y < h; y++) {
		const u8 *row = ng_ptr(addr + y * stride * bpp);
		u32 *o = &out[(size_t)y * w];
		if (fmt == FMT_8888) memcpy(o, row, (size_t)w * 4);
		else for (int x = 0; x < w; x++) { u16 v; memcpy(&v, row + x * 2, 2); o[x] = ng_color16(v, fmt); }
	}
}

void ng_fb_to_memory(u32 addr, int stride, int fmt, int w, int h, const u32 *px) {
	int bpp = ng_fmt_bpp(fmt);
	for (int y = 0; y < h; y++) {
		u8 *row = ng_ptr(addr + y * stride * bpp);
		const u32 *s = &px[(size_t)y * w];
		for (int x = 0; x < w; x++) {
			u32 c = s[x];
			if (fmt == FMT_8888) {
				u32 old; memcpy(&old, row + x * 4, 4);
				c = (c & 0x00FFFFFF) | (old & 0xFF000000);
				memcpy(row + x * 4, &c, 4);
			} else {
				u32 r = c & 0xFF, gg = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
				u16 v = fmt == FMT_565 ? (u16)(r >> 3 | (gg >> 2) << 5 | (b >> 3) << 11)
				      : fmt == FMT_5551 ? (u16)(r >> 3 | (gg >> 3) << 5 | (b >> 3) << 10 | 0x8000)
				      : (u16)(r >> 4 | (gg >> 4) << 4 | (b >> 4) << 8 | 0xF000);
				memcpy(row + x * 2, &v, 2);
			}
		}
	}
}
