// Native GE renderer: GE state and the command executor.
#include "ngpu.h"
#include "ng_internal.h"
#include "ng_draw.h"
#include "../gpu/ge_soft.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <map>
#include <string>
#include <vector>

GeState g_ge;
bool g_ng_census;
static std::string census_path;

// ---------------------------------------------------------------- census
// Every distinct draw state, counted, so the renderer can be checked against what the game really uses.
struct CensusEntry { u64 draws, verts; };
static std::map<std::string, CensusEntry> census_draw;
static std::map<std::string, u64> census_misc;
static u64 census_cmds[256];

static void census_prim(int prim, int count) {
	const GeState &g = g_ge;
	char k[512];
	u32 vt = g.vtype();
	int n = snprintf(k, sizeof k, "prim %d vtype %06x%s", prim, vt & 0x7FFFFF, g.through() ? " through" : "");
	if (g.clearMode()) {
		n += snprintf(k + n, sizeof k - n, " CLEAR %03x", g.cmd[CMD_CLEARMODE] & 0xF00);
	} else {
		if (g.cmd[CMD_TEX_EN] & 1) {
			u32 ta = g.texAddr(0);
			n += snprintf(k + n, sizeof k - n, " tex fmt %d%s %dx%d lv%d %s func %05x filt %03x wrap %03x map %03x", g.texFormat(),
			              g.texSwizzle() ? "sw" : "", g.texW(0), g.texH(0), g.texMaxLevel(),
			              (ta & 0x0F000000) == 0x04000000 ? "VRAM" : "ram", g.cmd[CMD_TEXFUNC] & 0x1FFFF,
			              g.cmd[CMD_TEXFILTER] & 0x107, g.cmd[CMD_TEXWRAP] & 0x101, g.cmd[CMD_TEXMAPMODE] & 0x303);
			if (g.texFormat() >= FMT_CLUT4 && g.texFormat() <= FMT_CLUT32)
				n += snprintf(k + n, sizeof k - n, " clut %06x", g.cmd[CMD_CLUTFORMAT] & 0x1FFFFF);
			if (g.cmd[CMD_TEXLEVEL] & 0xFF0003) n += snprintf(k + n, sizeof k - n, " level %06x", g.cmd[CMD_TEXLEVEL] & 0xFF0003);
		}
		if (!g.through() && (g.cmd[CMD_LIGHTING_EN] & 1)) {
			n += snprintf(k + n, sizeof k - n, " light mode %d mat %d", g.cmd[CMD_LIGHTMODE] & 1, g.cmd[CMD_MATUPDATE] & 7);
			for (int i = 0; i < 4; i++)
				if (g.cmd[CMD_LIGHT_EN0 + i] & 1) n += snprintf(k + n, sizeof k - n, " L%d:%03x", i, g.cmd[CMD_LTYPE0 + i] & 0x303);
		}
		if (!g.through() && (g.cmd[CMD_FOG_EN] & 1)) n += snprintf(k + n, sizeof k - n, " fog");
		if (g.cmd[CMD_BLEND_EN] & 1) {
			u32 b = g.cmd[CMD_BLEND] & 0x7FF;
			n += snprintf(k + n, sizeof k - n, " blend %03x", b);
			if ((b & 0xF) == 10) n += snprintf(k + n, sizeof k - n, " fa %06x", g.cmd[CMD_FIXA] & 0xFFFFFF);
			if (((b >> 4) & 0xF) == 10) n += snprintf(k + n, sizeof k - n, " fb %06x", g.cmd[CMD_FIXB] & 0xFFFFFF);
		}
		if (g.cmd[CMD_ATEST_EN] & 1) n += snprintf(k + n, sizeof k - n, " atest %06x", g.cmd[CMD_ATEST] & 0xFFFF07);
		if (g.cmd[CMD_CTEST_EN] & 1) n += snprintf(k + n, sizeof k - n, " CTEST %d", g.cmd[CMD_CTEST] & 3);
		if (g.cmd[CMD_STEST_EN] & 1) n += snprintf(k + n, sizeof k - n, " STENCIL %06x op %06x", g.cmd[CMD_STEST] & 0xFFFF07, g.cmd[CMD_SOP] & 0x070707);
		if (g.cmd[CMD_LOGIC_EN] & 1) n += snprintf(k + n, sizeof k - n, " LOGIC %d", g.cmd[CMD_LOGICOP] & 0xF);
		if (!g.through() && (g.cmd[CMD_CULL_EN] & 1)) n += snprintf(k + n, sizeof k - n, " cull %d", g.cmd[CMD_CULL] & 1);
		if (g.cmd[CMD_SHADE] & 1) n += snprintf(k + n, sizeof k - n, " gouraud");
		if (g.cmd[CMD_DITHER_EN] & 1) n += snprintf(k + n, sizeof k - n, " dither");
	}
	if (g.cmd[CMD_ZTEST_EN] & 1) n += snprintf(k + n, sizeof k - n, " ztest %d", g.cmd[CMD_ZTEST] & 7);
	if (!(g.cmd[CMD_ZWRITE_DIS] & 1)) n += snprintf(k + n, sizeof k - n, " zwrite");
	u32 mask = (g.cmd[CMD_MASKRGB] & 0xFFFFFF) | (g.cmd[CMD_MASKA] << 24);
	if (mask) n += snprintf(k + n, sizeof k - n, " pmask %08x", mask);
	n += snprintf(k + n, sizeof k - n, " fb %08x/%d fmt %d", g.fbAddr(), g.fbStride(), g.fbFormat());
	if (!g.through()) {
		if ((g.cmd[CMD_MINZ] & 0xFFFF) != 0 || (g.cmd[CMD_MAXZ] & 0xFFFF) != 0xFFFF)
			n += snprintf(k + n, sizeof k - n, " zrange %04x-%04x", g.cmd[CMD_MINZ] & 0xFFFF, g.cmd[CMD_MAXZ] & 0xFFFF);
		if (g.cmd[CMD_DEPTHCLIP_EN] & 1) n += snprintf(k + n, sizeof k - n, " depthclip");
	}
	CensusEntry &e = census_draw[k];
	e.draws++;
	e.verts += count;
}

static void census_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void census_note(const char *fmt, ...) {
	char k[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(k, sizeof k, fmt, ap);
	va_end(ap);
	census_misc[k]++;
}

extern "C" void ngpu_census_write(void) {
	if (!g_ng_census) return;
	FILE *f = fopen(census_path.c_str(), "w");
	if (!f) return;
	fprintf(f, "== commands (count)\n");
	for (int i = 0; i < 256; i++)
		if (census_cmds[i]) fprintf(f, "%02x %llu\n", i, (unsigned long long)census_cmds[i]);
	fprintf(f, "== other\n");
	for (auto &m : census_misc) fprintf(f, "%10llu  %s\n", (unsigned long long)m.second, m.first.c_str());
	fprintf(f, "== draws (draws, vertices, state)\n");
	for (auto &d : census_draw) fprintf(f, "%8llu %9llu  %s\n", (unsigned long long)d.second.draws, (unsigned long long)d.second.verts, d.first.c_str());
	fclose(f);
}

// ---------------------------------------------------------------- executor
void ng_exec(u32 pc, u32 op) {
	GeState &g = g_ge;
	u32 cmd = op >> 24, data = op & 0xFFFFFF;
	if (g_ng_census) census_cmds[cmd]++;
	// anything but the vertex and index addresses may change what the next draw looks like (ng_gl.cpp merges
	// draws until then); a register written with the value it has changes nothing, except the data ports
	extern bool g_ng_dirty;
	// (bones and morph weights are applied to the vertices here, so they don't count)
	if (cmd != CMD_PRIM && cmd != CMD_VADDR && cmd != CMD_IADDR && cmd != CMD_BASE && cmd != CMD_NOP &&
	    cmd != CMD_BONE_NUM && cmd != CMD_BONE_DATA && !(cmd >= CMD_MORPH0 && cmd < CMD_MORPH0 + 8) &&
	    (g.cmd[cmd] != op || (cmd >= CMD_WORLD_NUM && cmd <= CMD_TGEN_DATA) ||
	     cmd == CMD_LOADCLUT || cmd == CMD_TRANS_START || cmd == CMD_RCSM || cmd == CMD_END || cmd == CMD_FINISH || cmd == CMD_SIGNAL))
		g_ng_dirty = true;
	switch (cmd) {
	case CMD_VADDR: g.vaddr = g.rel(data); break;
	case CMD_IADDR: g.iaddr = g.rel(data); break;
	case CMD_PRIM: {
		g.cmd[cmd] = op;
		int prim = (data >> 16) & 7, count = data & 0xFFFF;
		if (g_ng_census) census_prim(prim, count);
		u32 vt = g.vtype();
		NgLayout L;
		ng_vertex_layout(vt, &L);
		static std::vector<NgVert> verts;
		if ((int)verts.size() < count) verts.resize(count);
		int n = ng_decode(g.vaddr, g.iaddr, count, L, verts.data());
		if (n) ng_gl_prim(prim, verts.data(), n, L);
		int it = (vt >> VT_IDX_SHIFT) & 3;
		if (it) g.iaddr += count * (it == 1 ? 1 : it == 2 ? 2 : 4);
		else g.vaddr += count * L.size * L.morphs;
		return;
	}
	case CMD_BEZIER: case CMD_SPLINE:
		if (g_ng_census) census_note("patch %02x vtype %06x", cmd, g.vtype());
		break;
	case CMD_BBOX: if (g_ng_census) census_note("bounding box"); break;
	case CMD_BJUMP: if (g_ng_census) census_note("bjump"); break;
	case CMD_SIGNAL: if (g_ng_census) census_note("signal %02x", (data >> 16) & 0xFF); break;
	case CMD_OFFSET: case CMD_ORIGIN: break;   // the list processor keeps the offset
	case CMD_BONE_NUM: g.cmd[cmd] = data & 0x7F; return;
	case CMD_BONE_DATA: { u32 i = g.cmd[CMD_BONE_NUM]; if (i < 96) g.bone[i] = ge_float24(data); g.cmd[CMD_BONE_NUM] = (i + 1) & 0x7F; return; }
	case CMD_WORLD_NUM: g.cmd[cmd] = data & 0xF; return;
	case CMD_WORLD_DATA: { u32 i = g.cmd[CMD_WORLD_NUM]; if (i < 12) g.world[i] = ge_float24(data); g.cmd[CMD_WORLD_NUM] = (i + 1) & 0xF; return; }
	case CMD_VIEW_NUM: g.cmd[cmd] = data & 0xF; return;
	case CMD_VIEW_DATA: { u32 i = g.cmd[CMD_VIEW_NUM]; if (i < 12) g.view[i] = ge_float24(data); g.cmd[CMD_VIEW_NUM] = (i + 1) & 0xF; return; }
	case CMD_PROJ_NUM: g.cmd[cmd] = data & 0x1F; return;
	case CMD_PROJ_DATA: { u32 i = g.cmd[CMD_PROJ_NUM]; if (i < 16) g.proj[i] = ge_float24(data); g.cmd[CMD_PROJ_NUM] = (i + 1) & 0x1F; return; }
	case CMD_TGEN_NUM: g.cmd[cmd] = data & 0xF; return;
	case CMD_TGEN_DATA: { u32 i = g.cmd[CMD_TGEN_NUM]; if (i < 12) g.tgen[i] = ge_float24(data); g.cmd[CMD_TGEN_NUM] = (i + 1) & 0xF; return; }
	case CMD_MORPH0 + 0: case CMD_MORPH0 + 1: case CMD_MORPH0 + 2: case CMD_MORPH0 + 3:
	case CMD_MORPH0 + 4: case CMD_MORPH0 + 5: case CMD_MORPH0 + 6: case CMD_MORPH0 + 7:
		g.morph[cmd - CMD_MORPH0] = ge_float24(data);
		break;
	case CMD_LOADCLUT:
		ng_gl_loadclut(g.clutAddr(), ((data & 0x7F) == 0x40 ? 0x40 : (data & 0x3F)) * 32);
		if (g_ng_census) census_note("loadclut %s fmt %d blocks %d", (g.clutAddr() & 0x0F000000) == 0x04000000 ? "VRAM" : "ram", g.clutFormat(), data & 0x3F);
		break;
	case CMD_TRANS_START:
		if (g_ng_census) {
			u32 src = (g.cmd[CMD_TRANS_SRC] & 0xFFFFF0) | ((g.cmd[CMD_TRANS_SRCW] << 8) & 0xFF000000);
			u32 dst = (g.cmd[CMD_TRANS_DST] & 0xFFFFF0) | ((g.cmd[CMD_TRANS_DSTW] << 8) & 0xFF000000);
			census_note("transfer %08x/%d -> %08x/%d size %dx%d bpp %d", src, g.cmd[CMD_TRANS_SRCW] & 0x7FF, dst, g.cmd[CMD_TRANS_DSTW] & 0x7FF,
			            (g.cmd[CMD_TRANS_SIZE] & 0x3FF) + 1, ((g.cmd[CMD_TRANS_SIZE] >> 10) & 0x3FF) + 1, data & 1 ? 4 : 2);
		}
		break;
	case CMD_RCSM: if (g_ng_census) census_note("rcsm %06x", data); ng_gl_rcsm(data); break;
	case CMD_END: case CMD_FINISH: g.cmd[cmd] = op; ng_gl_list_done(); return;
	default: break;
	}
	g.cmd[cmd] = op;
}

// ---------------------------------------------------------------- shadow mode
static void shadow_op(uint32_t pc, uint32_t op, uint32_t offset) {
	g_ge.offset = offset;
	ng_exec(pc, op);
	static u32 finishes;
	if (g_ng_census && (op >> 24) == CMD_FINISH && ++finishes % 2000 == 0) ngpu_census_write();   // the game is usually killed, not exited
}

extern "C" void ngpu_shadow_enable(void) {
	ngpu_register();
	g_ge_native.shadow = 1;
	g_ge.shadow = true;
	if (const char *e = getenv("SR2_NGPU_CENSUS")) { g_ng_census = true; census_path = e; atexit(ngpu_census_write); }
	g_ngpu_shadow_op = shadow_op;
}
