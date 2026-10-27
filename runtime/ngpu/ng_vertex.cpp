// Native GE renderer: vertex decoding. Every GE vertex format becomes NgVert (floats + RGBA8 colour), with
// bone skinning and morphing done here; the world/view/projection transform, lighting, fog and texture
// coordinate generation are left to the vertex shader.
#include "ng_internal.h"
#include "ng_draw.h"
#include <math.h>

static const int kTcSize[4] = {0, 1, 2, 4}, kColSize[8] = {0, 0, 0, 0, 2, 2, 2, 4}, kNrmSize[4] = {0, 1, 2, 4},
                 kPosSize[4] = {0, 1, 2, 4}, kWgtSize[4] = {0, 1, 2, 4};

void ng_vertex_layout(u32 vt, NgLayout *L) {
	int t = (vt >> VT_TC_SHIFT) & 3, c = (vt >> VT_COL_SHIFT) & 7, n = (vt >> VT_NRM_SHIFT) & 3, p = (vt >> VT_POS_SHIFT) & 3;
	int w = (vt >> VT_WGT_SHIFT) & 3;
	L->tc = t; L->col = c >= 4 ? c : 0; L->nrm = n; L->pos = p; L->wgt = w;
	L->wcount = w ? ((vt >> VT_WCOUNT_SHIFT) & 7) + 1 : 0;
	L->morphs = ((vt >> VT_MCOUNT_SHIFT) & 7) + 1;
	L->through = (vt & VT_THROUGH) != 0;
	int size = 0, align = 1;
	auto put = [&](int csize, int n_, int *off) {
		size = (size + csize - 1) & ~(csize - 1);
		*off = size;
		size += csize * n_;
		if (csize > align) align = csize;
	};
	L->woff = L->toff = L->coff = L->noff = L->poff = 0;
	if (w) put(kWgtSize[w], L->wcount, &L->woff);
	if (t) put(kTcSize[t], 2, &L->toff);
	if (L->col) { int cs = kColSize[c]; size = (size + cs - 1) & ~(cs - 1); L->coff = size; size += cs; if (cs > align) align = cs; }
	if (n) put(kNrmSize[n], 3, &L->noff);
	put(kPosSize[p], 3, &L->poff);
	size = (size + align - 1) & ~(align - 1);
	L->size = size;
}

static inline float rd_s(const u8 *p, int fmt) {   // signed normalised (positions, normals)
	switch (fmt) {
	case 1: return (s8)*p * (1.0f / 128.0f);
	case 2: { s16 v; memcpy(&v, p, 2); return v * (1.0f / 32768.0f); }
	case 3: { float f; memcpy(&f, p, 4); return f; }
	}
	return 0;
}
static inline float rd_u(const u8 *p, int fmt) {   // unsigned normalised (texture coordinates, weights)
	switch (fmt) {
	case 1: return *p * (1.0f / 128.0f);
	case 2: { u16 v; memcpy(&v, p, 2); return v * (1.0f / 32768.0f); }
	case 3: { float f; memcpy(&f, p, 4); return f; }
	}
	return 0;
}

static inline u32 rd_color(const u8 *p, int fmt) {   // -> RGBA8 (R in the low byte)
	u16 v;
	switch (fmt) {
	case 4: {  // 565
		memcpy(&v, p, 2);
		u32 r = v & 31, g = (v >> 5) & 63, b = v >> 11;
		return (r << 3 | r >> 2) | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2) << 16 | 0xFF000000u;
	}
	case 5: {  // 5551
		memcpy(&v, p, 2);
		u32 r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
		return (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16 | (v & 0x8000 ? 0xFF000000u : 0);
	}
	case 6: {  // 4444
		memcpy(&v, p, 2);
		u32 r = v & 15, g = (v >> 4) & 15, b = (v >> 8) & 15, a = v >> 12;
		return r * 0x11 | (g * 0x11) << 8 | (b * 0x11) << 16 | (a * 0x11) << 24;
	}
	case 7: { u32 c; memcpy(&c, p, 4); return c; }
	}
	return 0xFFFFFFFF;
}

// Decodes vertex i (of a vertex stream at src with layout L) into out, skinning and morphing it.
static void decode_one(const u8 *src, const NgLayout &L, int morphStride, NgVert *out) {
	const GeState &g = g_ge;
	float pos[3] = {0, 0, 0}, nrm[3] = {0, 0, 0}, uv[2] = {0, 0}, col[4] = {0, 0, 0, 0};
	u32 col8 = 0;
	for (int m = 0; m < L.morphs; m++) {
		const u8 *v = src + m * morphStride;
		float mw = L.morphs > 1 ? g.morph[m] : 1.0f;
		if (L.through) {
			// through mode: integer pixel positions (16-bit signed x/y, unsigned z) and texel coordinates
			if (L.pos == 2) {
				s16 x, y; u16 z;
				memcpy(&x, v + L.poff, 2); memcpy(&y, v + L.poff + 2, 2); memcpy(&z, v + L.poff + 4, 2);
				pos[0] += x * mw; pos[1] += y * mw; pos[2] += z * mw;
			} else if (L.pos == 3) {
				float f[3]; memcpy(f, v + L.poff, 12);
				pos[0] += f[0] * mw; pos[1] += f[1] * mw; pos[2] += f[2] * mw;
			} else {
				pos[0] += (s8)v[L.poff] * mw; pos[1] += (s8)v[L.poff + 1] * mw; pos[2] += v[L.poff + 2] * mw;
			}
			if (L.tc == 1) { uv[0] += v[L.toff] * mw; uv[1] += v[L.toff + 1] * mw; }
			else if (L.tc == 2) { u16 a, b; memcpy(&a, v + L.toff, 2); memcpy(&b, v + L.toff + 2, 2); uv[0] += a * mw; uv[1] += b * mw; }
			else if (L.tc == 3) { float f[2]; memcpy(f, v + L.toff, 8); uv[0] += f[0] * mw; uv[1] += f[1] * mw; }
		} else {
			int ps = kPosSize[L.pos], ns = kNrmSize[L.nrm], ts = kTcSize[L.tc];
			float p3[3] = {rd_s(v + L.poff, L.pos), rd_s(v + L.poff + ps, L.pos), rd_s(v + L.poff + 2 * ps, L.pos)};
			float n3[3] = {0, 0, 0};
			if (L.nrm) { n3[0] = rd_s(v + L.noff, L.nrm); n3[1] = rd_s(v + L.noff + ns, L.nrm); n3[2] = rd_s(v + L.noff + 2 * ns, L.nrm); }
			if (L.wcount) {
				// skinning: sum of the weighted bone transforms (4x3, column major as the GE loads them)
				float sp[3] = {0, 0, 0}, sn[3] = {0, 0, 0};
				int ws = kWgtSize[L.wgt];
				for (int b = 0; b < L.wcount; b++) {
					float w = rd_u(v + L.woff + b * ws, L.wgt);
					if (w == 0.0f) continue;
					const float *M = g.bone + b * 12;
					for (int k = 0; k < 3; k++) {
						sp[k] += w * (M[k] * p3[0] + M[3 + k] * p3[1] + M[6 + k] * p3[2] + M[9 + k]);
						sn[k] += w * (M[k] * n3[0] + M[3 + k] * n3[1] + M[6 + k] * n3[2]);
					}
				}
				memcpy(p3, sp, sizeof sp);
				memcpy(n3, sn, sizeof sn);
			}
			for (int k = 0; k < 3; k++) { pos[k] += p3[k] * mw; nrm[k] += n3[k] * mw; }
			if (L.tc) { uv[0] += rd_u(v + L.toff, L.tc) * mw; uv[1] += rd_u(v + L.toff + ts, L.tc) * mw; }
		}
		if (L.col) {
			u32 c = rd_color(v + L.coff, L.col);
			if (L.morphs == 1) col8 = c;
			else for (int k = 0; k < 4; k++) col[k] += ((c >> (8 * k)) & 0xFF) * mw;
		}
	}
	if (L.col && L.morphs > 1) {
		col8 = 0;
		for (int k = 0; k < 4; k++) { int x = (int)(col[k] + 0.5f); col8 |= (u32)(x < 0 ? 0 : x > 255 ? 255 : x) << (8 * k); }
	}
	memcpy(out->pos, pos, sizeof pos);
	memcpy(out->nrm, nrm, sizeof nrm);
	out->uv[0] = uv[0]; out->uv[1] = uv[1];
	out->color = col8;
}

// Decodes the vertices a PRIM uses into verts (indices 0..n-1 in the order the primitive gives them,
// after resolving any index buffer). Returns the vertex count.
int ng_decode(u32 vaddr, u32 iaddr, int count, const NgLayout &L, NgVert *verts) {
	int it = (g_ge.vtype() >> VT_IDX_SHIFT) & 3;
	int stride = L.size * L.morphs;   // morph targets of one vertex are consecutive
	int morphStride = L.size;
	if (!it) {
		if (!ng_valid(vaddr, count * stride)) return 0;
		const u8 *src = ng_ptr(vaddr);
		for (int i = 0; i < count; i++) decode_one(src + i * stride, L, morphStride, &verts[i]);
		return count;
	}
	if (!ng_valid(iaddr, count * (it == 1 ? 1 : it == 2 ? 2 : 4))) return 0;
	const u8 *ip = ng_ptr(iaddr);
	for (int i = 0; i < count; i++) {
		u32 idx = it == 1 ? ip[i] : it == 2 ? (u32)((const u16 *)ip)[i] : ((const u32 *)ip)[i];
		if (!ng_valid(vaddr + idx * stride, stride)) { verts[i] = verts[i ? i - 1 : 0]; continue; }
		decode_one(ng_ptr(vaddr + idx * stride), L, morphStride, &verts[i]);
	}
	return count;
}
