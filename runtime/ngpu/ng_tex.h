// Native GE renderer: texture decoding (GE texture formats -> RGBA8) and hashing.
#pragma once
#include "ng_internal.h"
#include <vector>

// Bytes one texture level occupies in memory (stride in texels, h rows).
u32 ng_tex_bytes(int fmt, int stride, int h);
// The CLUT format's index transform and the palette (what the last LOADCLUT loaded, as RGBA8).
struct NgClut {
	int fmt, shift, mask, base;
	u32 pal[512];   // 512 16-bit or 256 32-bit entries
	u32 index(u32 v) const { return (((v >> shift) & mask) | (base & (fmt == 3 ? 0xFF : 0x1FF))) & (fmt == 3 ? 0xFF : 0x1FF); }
};
void ng_clut_set(NgClut *c, const u8 *raw, int fmt, int shift, int mask, int base);
// Decodes w x h texels (row pitch = stride texels) into out as RGBA8 (R in the low byte). Returns false for
// formats it can't decode.
bool ng_tex_decode(const u8 *src, int fmt, int w, int h, int stride, bool swizzled, const NgClut *clut, u32 *out);
// RGBA8 from a 16-bit GE colour.
u32 ng_color16(u16 v, int fmt);

u64 ng_hash(const void *p, size_t n, u64 seed = 0);
u32 ng_xxh32(const void *p, size_t n, u32 seed);
