// Native GE renderer: texture decoding and hashing.
#include "ng_tex.h"

u32 ng_color16(u16 v, int fmt) {
	switch (fmt) {
	case FMT_565: {
		u32 r = v & 31, g = (v >> 5) & 63, b = v >> 11;
		return (r << 3 | r >> 2) | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2) << 16 | 0xFF000000u;
	}
	case FMT_5551: {
		u32 r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
		return (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16 | (v & 0x8000 ? 0xFF000000u : 0);
	}
	default: {
		u32 r = v & 15, g = (v >> 4) & 15, b = (v >> 8) & 15, a = v >> 12;
		return r * 0x11 | (g * 0x11) << 8 | (b * 0x11) << 16 | (a * 0x11) << 24;
	}
	}
}

static const int kBits[16] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0};

u32 ng_tex_bytes(int fmt, int stride, int h) {
	if (fmt >= FMT_DXT1 && fmt <= FMT_DXT5) return (u32)((stride + 3) / 4) * ((h + 3) / 4) * (fmt == FMT_DXT1 ? 8 : 16);
	return (u32)stride * h * kBits[fmt & 15] / 8;
}

void ng_clut_set(NgClut *c, const u8 *raw, int fmt, int shift, int mask, int base) {
	// the CLUT holds 1 KB: 512 16-bit or 256 32-bit colours
	c->fmt = fmt; c->shift = shift; c->mask = mask; c->base = base;
	if (fmt == 3) memcpy(c->pal, raw, 1024);
	else for (int i = 0; i < 512; i++) { u16 v; memcpy(&v, raw + i * 2, 2); c->pal[i] = ng_color16(v, fmt); }
}

// Unswizzles the GE's 16 byte x 8 row blocks into linear rows of `pitch` bytes.
static void unswizzle(const u8 *src, u8 *dst, int pitch, int rows) {
	int bxs = pitch / 16;
	for (int by = 0; by < (rows + 7) / 8; by++)
		for (int bx = 0; bx < bxs; bx++) {
			const u8 *b = src + (by * bxs + bx) * 128;
			for (int r = 0; r < 8; r++) {
				int y = by * 8 + r;
				if (y < rows) memcpy(dst + y * pitch + bx * 16, b + r * 16, 16);
			}
		}
}

bool ng_tex_decode(const u8 *src, int fmt, int w, int h, int stride, bool swizzled, const NgClut *c, u32 *out) {
	if (fmt >= FMT_DXT1) return false;
	u32 lut[256];
	if (fmt == FMT_CLUT4 || fmt == FMT_CLUT8)
		for (int i = 0; i < (fmt == FMT_CLUT4 ? 16 : 256); i++) lut[i] = c->pal[c->index(i)];
	int bits = kBits[fmt];
	int pitch = stride * bits / 8;
	std::vector<u8> lin;
	if (swizzled && pitch >= 16) {
		lin.resize((size_t)pitch * ((h + 7) & ~7));
		unswizzle(src, lin.data(), pitch, h);
		src = lin.data();
	}
	for (int y = 0; y < h; y++) {
		const u8 *row = src + (size_t)y * pitch;
		u32 *o = out + (size_t)y * w;
		switch (fmt) {
		case FMT_565: case FMT_5551: case FMT_4444:
			for (int x = 0; x < w; x++) { u16 v; memcpy(&v, row + x * 2, 2); o[x] = ng_color16(v, fmt); }
			break;
		case FMT_8888: memcpy(o, row, (size_t)w * 4); break;
		case FMT_CLUT4: for (int x = 0; x < w; x++) o[x] = lut[(row[x >> 1] >> ((x & 1) * 4)) & 15]; break;
		case FMT_CLUT8: for (int x = 0; x < w; x++) o[x] = lut[row[x]]; break;
		case FMT_CLUT16: for (int x = 0; x < w; x++) { u16 v; memcpy(&v, row + x * 2, 2); o[x] = c->pal[c->index(v)]; } break;
		case FMT_CLUT32: for (int x = 0; x < w; x++) { u32 v; memcpy(&v, row + x * 4, 4); o[x] = c->pal[c->index(v)]; } break;
		}
	}
	return true;
}

// ---------------------------------------------------------------- hashing
u64 ng_hash(const void *p, size_t n, u64 seed) {
	// 64-bit multiply-xorshift over 8-byte words; fast and good enough to tell texture contents apart
	const u8 *b = (const u8 *)p;
	u64 h = seed ^ (n * 0x9E3779B97F4A7C15ull);
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		u64 v; memcpy(&v, b + i, 8);
		h ^= v * 0xBF58476D1CE4E5B9ull;
		h = (h << 27 | h >> 37) * 0x94D049BB133111EBull;
	}
	for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
	h ^= h >> 31;
	return h;
}

// XXH32 (the hash texture packs are named by).
static inline u32 rotl32(u32 x, int r) { return x << r | x >> (32 - r); }
u32 ng_xxh32(const void *input, size_t len, u32 seed) {
	const u32 P1 = 2654435761u, P2 = 2246822519u, P3 = 3266489917u, P4 = 668265263u, P5 = 374761393u;
	const u8 *p = (const u8 *)input, *end = p + len;
	u32 h;
	if (len >= 16) {
		u32 v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
		const u8 *limit = end - 16;
		do {
			u32 a, b, c, d;
			memcpy(&a, p, 4); memcpy(&b, p + 4, 4); memcpy(&c, p + 8, 4); memcpy(&d, p + 12, 4);
			v1 = rotl32(v1 + a * P2, 13) * P1; v2 = rotl32(v2 + b * P2, 13) * P1;
			v3 = rotl32(v3 + c * P2, 13) * P1; v4 = rotl32(v4 + d * P2, 13) * P1;
			p += 16;
		} while (p <= limit);
		h = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18);
	} else {
		h = seed + P5;
	}
	h += (u32)len;
	while (p + 4 <= end) { u32 k; memcpy(&k, p, 4); h = rotl32(h + k * P3, 17) * P4; p += 4; }
	while (p < end) { h = rotl32(h + *p * P5, 11) * P1; p++; }
	h ^= h >> 15; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
	return h;
}
