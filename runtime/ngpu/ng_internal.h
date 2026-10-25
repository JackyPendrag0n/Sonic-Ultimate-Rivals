// Native GE renderer internals shared by its parts: GE state (ng_core.cpp), vertex decoding (ng_vertex.cpp),
// textures (ng_tex.cpp) and the OpenGL side (ng_gl.cpp).
#pragma once
#include <stdint.h>
#include <string.h>
#include "ge_regs.h"

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32;

extern "C" u8 *g_mem;
static inline u8 *ng_ptr(u32 a) { return g_mem + (a & 0x3FFFFFFF); }
static inline u32 ng_r32(u32 a) { u32 v; memcpy(&v, ng_ptr(a), 4); return v; }
static inline bool ng_valid(u32 a, u32 size) {
	a &= 0x3FFFFFFF;
	u32 e = a + size;
	if (a >= 0x08000000 && e <= 0x0C000000) return true;   // RAM, with the 32 MB above it the extra racers use
	if (a >= 0x04000000 && e <= 0x04800000) return true;   // VRAM and its mirrors
	if (a >= 0x00010000 && e <= 0x00014000) return true;   // scratchpad
	return false;
}

struct GeState {
	u32 cmd[256];
	float world[12], view[12], proj[16], tgen[12], bone[8 * 12];
	float morph[8];
	u32 base_unused;
	u32 offset;           // list offset address (ORIGIN / OFFSETADDR)
	u32 vaddr, iaddr;     // current vertex / index address
	u32 transferStart;
	// split views (0xFA commands)
	bool shadow;          // fed by PPSSPP: draw, but never write guest memory

	u32 rel(u32 data) const { return (offset + ((cmd[CMD_BASE] << 8) & 0x0F000000) + (data & 0x00FFFFFF)) & 0x0FFFFFFF; }
	u32 vtype() const { return cmd[CMD_VTYPE] & 0xFFFFFF; }
	bool through() const { return (cmd[CMD_VTYPE] & VT_THROUGH) != 0; }
	bool clearMode() const { return cmd[CMD_CLEARMODE] & 1; }
	u32 fbAddr() const { return 0x04000000 | (cmd[CMD_FBPTR] & 0x1FFFF0); }
	u32 fbStride() const { return cmd[CMD_FBWIDTH] & 0x7FC; }
	int fbFormat() const { return cmd[CMD_FBFORMAT] & 3; }
	u32 zbAddr() const { return 0x04000000 | (cmd[CMD_ZBPTR] & 0x1FFFF0); }
	u32 zbStride() const { return cmd[CMD_ZBWIDTH] & 0x7FC; }
	u32 texAddr(int l) const { return ((cmd[CMD_TEXADDR0 + l] & 0xFFFFF0) | ((cmd[CMD_TEXBW0 + l] << 8) & 0x0F000000)); }
	u32 texStride(int l) const { return cmd[CMD_TEXBW0 + l] & 0x7FF; }
	int texW(int l) const { return 1 << (cmd[CMD_TEXSIZE0 + l] & 0xF); }
	int texH(int l) const { return 1 << ((cmd[CMD_TEXSIZE0 + l] >> 8) & 0xF); }
	int texFormat() const { return cmd[CMD_TEXFORMAT] & 0xF; }
	bool texSwizzle() const { return cmd[CMD_TEXMODE] & 1; }
	int texMaxLevel() const { return (cmd[CMD_TEXMODE] >> 16) & 7; }
	u32 clutAddr() const { return (cmd[CMD_CLUTADDR] & 0xFFFFF0) | ((cmd[CMD_CLUTADDRUP] << 8) & 0x0F000000); }
	int clutFormat() const { return cmd[CMD_CLUTFORMAT] & 3; }
	int clutShift() const { return (cmd[CMD_CLUTFORMAT] >> 2) & 0x1F; }
	int clutMask() const { return (cmd[CMD_CLUTFORMAT] >> 8) & 0xFF; }
	int clutBase() const { return ((cmd[CMD_CLUTFORMAT] >> 16) & 0x1F) * 16; }
	int scissorX1() const { return cmd[CMD_SCISSOR1] & 0x3FF; }
	int scissorY1() const { return (cmd[CMD_SCISSOR1] >> 10) & 0x3FF; }
	int scissorX2() const { return cmd[CMD_SCISSOR2] & 0x3FF; }
	int scissorY2() const { return (cmd[CMD_SCISSOR2] >> 10) & 0x3FF; }
	int regionX2() const { return cmd[CMD_REGION2] & 0x3FF; }
	int regionY2() const { return (cmd[CMD_REGION2] >> 10) & 0x3FF; }
	int offsetX16() const { return cmd[CMD_OFFSETX] & 0xFFFF; }
	int offsetY16() const { return cmd[CMD_OFFSETY] & 0xFFFF; }
	float f(int c) const { return ge_float24(cmd[c]); }
};
extern GeState g_ge;

// ng_core.cpp
void ng_exec(u32 pc, u32 op);

// census (ng_core.cpp)
extern bool g_ng_census;

extern "C" void logf_(int lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define NG_LOGE(...) logf_(0, __VA_ARGS__)
#define NG_LOGI(...) logf_(1, __VA_ARGS__)
#define NG_LOGD(...) logf_(2, __VA_ARGS__)
