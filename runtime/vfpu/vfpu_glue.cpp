// Glue between recompiled code (Ctx) and PPSSPP's VFPU interpreter (GPL-2.0+, see LICENSE.TXT).
#include <cstdio>
#include <cstdarg>
#include <limits>
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/InterpreterVFPU.h"
u8 voffset[128];
u8 fromvoffset[128];
MIPSState *currentMIPS;
void vfpu_log(const char *fmt, ...) { va_list a; va_start(a, fmt); fprintf(stderr, "[vfpu] "); vfprintf(stderr, fmt, a); fputc('\n', stderr); va_end(a); }
#ifndef M_LOG2E
#define M_E        2.71828182845904523536f
#define M_LOG2E    1.44269504088896340736f
#define M_LOG10E   0.434294481903251827651f
#define M_LN2      0.693147180559945309417f
#define M_LN10     2.30258509299404568402f
#undef M_PI
#define M_PI       3.14159265358979323846f

#ifndef M_PI_2
#define M_PI_2     1.57079632679489661923f
#endif
#define M_PI_4     0.785398163397448309616f
#define M_1_PI     0.318309886183790671538f
#define M_2_PI     0.636619772367581343076f
#define M_2_SQRTPI 1.12837916709551257390f
#define M_SQRT2    1.41421356237309504880f
#define M_SQRT1_2  0.707106781186547524401f
#endif

const float cst_constants[32] = {
	0,
	std::numeric_limits<float>::max(),  // all these are verified on real PSP
	sqrtf(2.0f),
	sqrtf(0.5f),
	2.0f/sqrtf((float)M_PI),
	2.0f/(float)M_PI,
	1.0f/(float)M_PI,
	(float)M_PI/4,
	(float)M_PI/2,
	(float)M_PI,
	(float)M_E,
	(float)M_LOG2E,
	(float)M_LOG10E,
	(float)M_LN2,
	(float)M_LN10,
	2*(float)M_PI,
	(float)M_PI/6,
	log10f(2.0f),
	logf(10.0f)/logf(2.0f),
	sqrtf(3.0f)/2.0f,
};
extern "C" void vfpu_init(void) {
	int i = 0;
	for (int m = 0; m < 8; m++)
		for (int y = 0; y < 4; y++)
			for (int x = 0; x < 4; x++)
				voffset[m * 4 + x * 32 + y] = i++;
	for (int k = 0; k < 128; k++) fromvoffset[voffset[k]] = k;
}
extern "C" void vfpu_reset_ctx(Ctx *c) {
	for (int k = 0; k < 128; k++) c->vi[k] = 0x7F800001;  // NaN like PPSSPP
	c->vfpuCtrl[VFPU_CTRL_SPREFIX] = 0xe4;
	c->vfpuCtrl[VFPU_CTRL_TPREFIX] = 0xe4;
	c->vfpuCtrl[VFPU_CTRL_DPREFIX] = 0;
	c->vfpuCtrl[VFPU_CTRL_CC] = 0x3f;
	c->vfpuCtrl[VFPU_CTRL_INF4] = 0;
	c->vfpuCtrl[VFPU_CTRL_REV] = 0x7772ceab;
	c->vfpuCtrl[VFPU_CTRL_RCX0] = 0x3f800001;
	c->vfpuCtrl[VFPU_CTRL_RCX1] = 0x3f800002;
	c->vfpuCtrl[VFPU_CTRL_RCX2] = 0x3f800004;
	c->vfpuCtrl[VFPU_CTRL_RCX3] = 0x3f800008;
	c->vfpuCtrl[VFPU_CTRL_RCX4] = 0x3f800000;
	c->vfpuCtrl[VFPU_CTRL_RCX5] = 0x3f800000;
	c->vfpuCtrl[VFPU_CTRL_RCX6] = 0x3f800000;
	c->vfpuCtrl[VFPU_CTRL_RCX7] = 0x3f800000;
}
#ifdef SR2_WATCH
extern "C" Ctx *g_interp_ctx;
#define WATCH_CTX(c) g_interp_ctx = (c);
#else
#define WATCH_CTX(c)
#endif
#define X(n) extern "C" void interp_##n(Ctx *c, u32 op) { currentMIPS = (MIPSState *)c; WATCH_CTX(c) MIPSInt::n((MIPSState *)c, MIPSOpcode(op)); }
#include "../interp_list.h"
