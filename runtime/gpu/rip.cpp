// Frame ripper: writes the 3D geometry of one game frame as a Wavefront OBJ (scene.obj + scene.mtl) with
// every texture it uses as PNG, named like the texture dumps so that edited textures can go straight into
// a mod's textures/ folder.
//
// ge_rip_request(dir) arms it. At the next display flip the texture cache is emptied, so every texture of
// the following frame is decoded (and written to dir) again; that frame's draws are recorded, and at the
// flip after it the files are written. Each draw becomes one OBJ object, in world space: bone and morph
// blending and the world matrix are applied, the camera is not. 2D (through mode) draws are left out.
// Draws without depth writes (the sky dome, which follows the camera, shadows and effects) are named
// overlay_N instead of draw_N.
//
// Like the PPSSPP sources it links with, this file is licensed under the GPL 2.0 or later.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <sys/stat.h>

#include "Core/MemMap.h"
#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"
#include "GPU/Common/TextureCacheCommon.h"

enum RipState { RIP_IDLE, RIP_ARMED, RIP_RECORDING };
static RipState rip_state;
static bool rip_clear_pending;
static std::string rip_dir;

struct RipDraw {
	u32 texaddr;   // 0: untextured
	bool nodepth;  // drawn without depth writes: sky, shadows, effects
	std::vector<float> v;     // xyz
	std::vector<float> vt;    // uv
	std::vector<float> vn;    // normal
	std::vector<u32> tri;     // indices into v, 3 per triangle
};
static std::vector<RipDraw> rip_draws;

extern "C" int ge_tex_rip_dump(const char *dir);   // tex_replace.cpp: NULL restores the normal dump folder

extern "C" void ge_rip_request(const char *dir) {
	if (rip_state != RIP_IDLE) return;
	rip_dir = dir;
	rip_state = RIP_ARMED;
}

bool rip_recording() { return rip_state == RIP_RECORDING; }
bool rip_take_clear() { bool c = rip_clear_pending; rip_clear_pending = false; return c; }

static float rd(const u8 *p, int fmt, bool sign) {
	switch (fmt) {
	case 1: return sign ? (s8)*p / 128.0f : *p / 128.0f;
	case 2: { u16 v; memcpy(&v, p, 2); return sign ? (s16)v / 32768.0f : v / 32768.0f; }
	case 3: { float f; memcpy(&f, p, 4); return f; }
	}
	return 0;
}

static void xform(const float *m, const float *in, float *out, bool point) {
	for (int r = 0; r < 3; r++) out[r] = m[r] * in[0] + m[3 + r] * in[1] + m[6 + r] * in[2] + (point ? m[9 + r] : 0);
}

// Decodes vertex `i` of a GE vertex buffer (vertType vt) into world space.
static void decode_vertex(const u8 *base, u32 vt, int i, float *pos, float *uv, float *nrm) {
	const int tc = vt & 3, col = (vt >> 2) & 7, nf = (vt >> 5) & 3, pf = (vt >> 7) & 3, wf = (vt >> 9) & 3;
	const int nw = ((vt >> 14) & 7) + 1, nm = ((vt >> 18) & 7) + 1;
	static const int csz[4] = {0, 1, 2, 4}, colsz[8] = {0, 0, 0, 0, 2, 2, 2, 4};
	// component offsets within one morph target
	int off = 0, al = 1, o_w = 0, o_t = 0, o_c = 0, o_n = 0, o_p = 0;
	auto place = [&](int size, int count, int *o) {
		if (!size) return;
		off = (off + size - 1) / size * size;
		*o = off;
		off += size * count;
		if (size > al) al = size;
	};
	place(wf ? csz[wf] : 0, nw, &o_w);
	place(tc ? csz[tc] : 0, 2, &o_t);
	place(col ? colsz[col] : 0, 1, &o_c);
	place(nf ? csz[nf] : 0, 3, &o_n);
	place(pf ? csz[pf] : 0, 3, &o_p);
	const int stride1 = (off + al - 1) / al * al, stride = stride1 * nm;
	const u8 *v = base + (size_t)i * stride;

	float p[3] = {0, 0, 0}, n[3] = {0, 0, 0}, t[2] = {0, 0}, w[8] = {0};
	for (int m = 0; m < nm; m++) {
		const u8 *mv = v + m * stride1;
		const float mw = nm > 1 ? gstate_c.morphWeights[m] : 1.0f;
		for (int k = 0; k < 3; k++) {
			if (pf) p[k] += mw * rd(mv + o_p + k * csz[pf], pf, true);
			if (nf) n[k] += mw * rd(mv + o_n + k * csz[nf], nf, true);
		}
		for (int k = 0; k < 2; k++) if (tc) t[k] += mw * rd(mv + o_t + k * csz[tc], tc, false);
		if (wf && m == 0) for (int k = 0; k < nw; k++) w[k] = wf == 1 ? mv[o_w + k] / 128.0f : rd(mv + o_w + k * csz[wf], wf, false);
	}
	float sp[3] = {0, 0, 0}, sn[3] = {0, 0, 0};
	if (wf) {
		for (int k = 0; k < nw; k++) {
			if (w[k] == 0) continue;
			float tp[3], tn[3];
			xform(&gstate.boneMatrix[k * 12], p, tp, true);
			xform(&gstate.boneMatrix[k * 12], n, tn, false);
			for (int c = 0; c < 3; c++) { sp[c] += w[k] * tp[c]; sn[c] += w[k] * tn[c]; }
		}
	} else {
		memcpy(sp, p, sizeof sp);
		memcpy(sn, n, sizeof sn);
	}
	xform(gstate.worldMatrix, sp, pos, true);
	xform(gstate.worldMatrix, sn, nrm, false);
	const float su = getFloat24(gstate.texscaleu), sv = getFloat24(gstate.texscalev);
	const float ou = getFloat24(gstate.texoffsetu), ov = getFloat24(gstate.texoffsetv);
	if (gstate.getUVGenMode() == GE_TEXMAP_TEXTURE_COORDS) { uv[0] = t[0] * su + ou; uv[1] = t[1] * sv + ov; }
	else { uv[0] = t[0]; uv[1] = t[1]; }
}

void rip_prim(u32 op) {
	const u32 vt = gstate.vertType & 0xFFFFFF;
	if (vt & GE_VTYPE_THROUGH) return;
	const int prim = (op >> 16) & 7, count = op & 0xFFFF;
	if (prim < GE_PRIM_TRIANGLES || prim > GE_PRIM_TRIANGLE_FAN || count < 3) return;
	const int ifmt = (vt >> 11) & 3;
	std::vector<u32> idx(count);
	if (ifmt) {
		const u8 *ip = Memory::GetPointerUnchecked(gstate_c.indexAddr);
		for (int i = 0; i < count; i++) {
			if (ifmt == 1) idx[i] = ip[i];
			else if (ifmt == 2) { u16 x; memcpy(&x, ip + 2 * i, 2); idx[i] = x; }
			else { u32 x; memcpy(&x, ip + 4 * i, 4); idx[i] = x; }
		}
	} else {
		for (int i = 0; i < count; i++) idx[i] = i;
	}
	RipDraw d;
	d.texaddr = gstate.isTextureMapEnabled() ? gstate.getTextureAddress(0) : 0;
	d.nodepth = !gstate.isDepthTestEnabled() || !gstate.isDepthWriteEnabled();
	const u8 *base = Memory::GetPointerUnchecked(gstate_c.vertexAddr);
	std::map<u32, u32> remap;
	auto vert = [&](u32 i) -> u32 {
		auto it = remap.find(i);
		if (it != remap.end()) return it->second;
		float p[3], t[2], n[3];
		decode_vertex(base, vt, i, p, t, n);
		u32 k = (u32)d.v.size() / 3;
		d.v.insert(d.v.end(), p, p + 3);
		d.vt.push_back(t[0]); d.vt.push_back(1.0f - t[1]);
		d.vn.insert(d.vn.end(), n, n + 3);
		remap[i] = k;
		return k;
	};
	auto tri = [&](u32 a, u32 b, u32 c) {
		if (a == b || b == c || a == c) return;   // degenerate strip joins
		d.tri.push_back(vert(a)); d.tri.push_back(vert(b)); d.tri.push_back(vert(c));
	};
	if (prim == GE_PRIM_TRIANGLES) for (int i = 0; i + 2 < count; i += 3) tri(idx[i], idx[i + 1], idx[i + 2]);
	else if (prim == GE_PRIM_TRIANGLE_STRIP) for (int i = 0; i + 2 < count; i++) {
		if (i & 1) tri(idx[i + 1], idx[i], idx[i + 2]); else tri(idx[i], idx[i + 1], idx[i + 2]);
	}
	else for (int i = 1; i + 1 < count; i++) tri(idx[0], idx[i], idx[i + 1]);
	if (!d.tri.empty()) rip_draws.push_back(std::move(d));
}

static void rip_write(TextureCacheCommon *tc) {
	std::string obj = rip_dir + "/scene.obj", mtl = rip_dir + "/scene.mtl";
	FILE *f = fopen(obj.c_str(), "w"), *m = fopen(mtl.c_str(), "w");
	if (!f || !m) { if (f) fclose(f); if (m) fclose(m); fprintf(stderr, "rip: can't write %s\n", obj.c_str()); return; }
	fprintf(f, "# Sonic Rivals 2 frame rip: %d draws, world space\nmtllib scene.mtl\n", (int)rip_draws.size());
	std::map<std::string, bool> mats;
	u32 vbase = 1;
	int n = 0, nobj = 0;
	std::string prev_mat;
	bool prev_nodepth = false;
	for (auto &d : rip_draws) {
		std::string mat = "untextured";
		u32 ck, fh;
		if (d.texaddr && tc && tc->FindEntryNames(d.texaddr, &ck, &fh)) {
			char b[40];
			snprintf(b, sizeof b, "%08x%08x", ck, fh);
			mat = b;
		}
		if (!mats.count(mat)) {
			mats[mat] = true;
			fprintf(m, "newmtl %s\nKd 1 1 1\n", mat.c_str());
			if (mat != "untextured") fprintf(m, "map_Kd %s.png\nmap_d %s.png\n", mat.c_str(), mat.c_str());
			fprintf(m, "\n");
		}
		// consecutive draws with the same texture and depth mode make one object
		if (mat != prev_mat || d.nodepth != prev_nodepth || n == 0)
			fprintf(f, "o %s_%04d\nusemtl %s\n", d.nodepth ? "overlay" : "draw", nobj++, mat.c_str());
		prev_mat = mat;
		prev_nodepth = d.nodepth;
		n++;
		for (size_t i = 0; i < d.v.size(); i += 3) fprintf(f, "v %g %g %g\n", d.v[i], d.v[i + 1], d.v[i + 2]);
		for (size_t i = 0; i < d.vt.size(); i += 2) fprintf(f, "vt %g %g\n", d.vt[i], d.vt[i + 1]);
		for (size_t i = 0; i < d.vn.size(); i += 3) fprintf(f, "vn %g %g %g\n", d.vn[i], d.vn[i + 1], d.vn[i + 2]);
		for (size_t i = 0; i < d.tri.size(); i += 3) {
			u32 a = d.tri[i] + vbase, b = d.tri[i + 1] + vbase, c = d.tri[i + 2] + vbase;
			fprintf(f, "f %u/%u/%u %u/%u/%u %u/%u/%u\n", a, a, a, b, b, b, c, c, c);
		}
		vbase += (u32)d.v.size() / 3;
	}
	fclose(f);
	fclose(m);
	fprintf(stderr, "rip: wrote %s (%d draws, %d objects)\n", obj.c_str(), n, nobj);
}

// Called at every display flip.
void rip_flip(TextureCacheCommon *tc) {
	if (rip_state == RIP_ARMED) {
		mkdir(rip_dir.c_str(), 0755);
		ge_tex_rip_dump(rip_dir.c_str());
		rip_draws.clear();
		rip_clear_pending = true;
		rip_state = RIP_RECORDING;
	} else if (rip_state == RIP_RECORDING) {
		rip_write(tc);
		rip_draws.clear();
		ge_tex_rip_dump(nullptr);
		rip_state = RIP_IDLE;
	}
}
