// Runtime interface shared by recompiled code and the native runtime.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8; typedef int8_t s8; typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64; typedef int64_t s64;

// CPU context. Field names mirror PPSSPP's MIPSState so its VFPU interpreter can run on it.
typedef struct Ctx {
	u32 r[32];
	union { float f[32]; u32 fi[32]; s32 fs[32]; };
	union { float v[128]; u32 vi[128]; };
	u32 vfpuCtrl[16];
	u32 pc, lo, hi, fcr31, fpcond;
	u32 llbit, intr;
	u32 vcmpResult[4];
	void *thread;  // owning PSP thread (runtime private)
} Ctx;

enum { VFPU_CTRL_SPREFIX, VFPU_CTRL_TPREFIX, VFPU_CTRL_DPREFIX, VFPU_CTRL_CC, VFPU_CTRL_INF4,
       VFPU_CTRL_RSV5, VFPU_CTRL_RSV6, VFPU_CTRL_REV, VFPU_CTRL_RCX0, VFPU_CTRL_RCX1,
       VFPU_CTRL_RCX2, VFPU_CTRL_RCX3, VFPU_CTRL_RCX4, VFPU_CTRL_RCX5, VFPU_CTRL_RCX6, VFPU_CTRL_RCX7, VFPU_CTRL_MAX };

// ---------------------------------------------------------------- memory
extern u8 *g_mem;
#define MEM_MASK 0x3FFFFFFFu
typedef u32 __attribute__((may_alias, aligned(1))) u32_u;
typedef u16 __attribute__((may_alias, aligned(1))) u16_u;
static inline u8 *PTR(u32 a) { return g_mem + (a & MEM_MASK); }
#ifndef SR2_WATCH
#define LW(a)  (*(u32_u *)PTR(a))
#define LH(a)  ((u32)(s32)*(s16 *)(u16_u *)PTR(a))
#define LHU(a) ((u32)*(u16_u *)PTR(a))
#define LB(a)  ((u32)(s32)*(s8 *)PTR(a))
#define LBU(a) ((u32)*PTR(a))
#define SW(a, v) (*(u32_u *)PTR(a) = (u32)(v))
#define SH(a, v) (*(u16_u *)PTR(a) = (u16)(v))
#define SB(a, v) (*PTR(a) = (u8)(v))
#else
// Debug build of generated code: report accesses inside [g_watch_lo, g_watch_hi) with the C function name.
extern u32 g_watch_lo, g_watch_hi;
extern const char *g_trace_fn;
void rt_watch(u32 a, int size, int write, u32 val, const char *fn);
void rt_trace(u32 a, int size, int write, u32 val, const char *fn);
#define WCHK_(a, s, w, v) do { u32 x_ = (a) & MEM_MASK; if (x_ >= g_watch_lo && x_ < g_watch_hi) rt_watch(x_, s, w, v, __func__); \
	if (g_trace_fn && (w) && !strcmp(__func__, g_trace_fn)) rt_trace(x_, s, w, v, __func__); } while (0)
#define LW(a)  ({ u32 a_ = (a); u32 r_ = *(u32_u *)PTR(a_); WCHK_(a_, 4, 0, r_); r_; })
#define LH(a)  ({ u32 a_ = (a); u32 r_ = (u32)(s32)*(s16 *)(u16_u *)PTR(a_); WCHK_(a_, 2, 0, r_); r_; })
#define LHU(a) ({ u32 a_ = (a); u32 r_ = (u32)*(u16_u *)PTR(a_); WCHK_(a_, 2, 0, r_); r_; })
#define LB(a)  ({ u32 a_ = (a); u32 r_ = (u32)(s32)*(s8 *)PTR(a_); WCHK_(a_, 1, 0, r_); r_; })
#define LBU(a) ({ u32 a_ = (a); u32 r_ = (u32)*PTR(a_); WCHK_(a_, 1, 0, r_); r_; })
#define SW(a, v) do { u32 a_ = (a), v_ = (u32)(v); WCHK_(a_, 4, 1, v_); *(u32_u *)PTR(a_) = v_; } while (0)
#define SH(a, v) do { u32 a_ = (a), v_ = (u32)(v); WCHK_(a_, 2, 1, v_); *(u16_u *)PTR(a_) = (u16)v_; } while (0)
#define SB(a, v) do { u32 a_ = (a), v_ = (u32)(v); WCHK_(a_, 1, 1, v_); *PTR(a_) = (u8)v_; } while (0)
#endif

#define ROTR(x, n) (((x) >> (n)) | ((x) << ((32 - (n)) & 31)))
#define MAX_S32(a, b) ((s32)(a) > (s32)(b) ? (s32)(a) : (s32)(b))
#define MIN_S32(a, b) ((s32)(a) < (s32)(b) ? (s32)(a) : (s32)(b))
#define CLZ32(x) ((x) ? (u32)__builtin_clz(x) : 32u)
#define WSBH(x) ((((x) & 0xFF00FF00u) >> 8) | (((x) & 0x00FF00FFu) << 8))

static inline u32 rt_lwl(u32 a, u32 r) { u32 sh = (a & 3) * 8; u32 m = LW(a & ~3u); return (r & (0x00ffffffu >> sh)) | (m << (24 - sh)); }
static inline u32 rt_lwr(u32 a, u32 r) { u32 sh = (a & 3) * 8; u32 m = LW(a & ~3u); return (r & (0xffffff00u << (24 - sh))) | (m >> sh); }
static inline void rt_swl(u32 a, u32 r) { u32 sh = (a & 3) * 8; u32 m = LW(a & ~3u); SW(a & ~3u, (r >> (24 - sh)) | (m & (0xffffff00u << sh))); }
static inline void rt_swr(u32 a, u32 r) { u32 sh = (a & 3) * 8; u32 m = LW(a & ~3u); SW(a & ~3u, (r << sh) | (m & (0x00ffffffu >> (24 - sh)))); }

static inline u32 rt_bitrev(u32 v) { u32 r = 0; for (int i = 0; i < 32; i++) if (v & (1u << i)) r |= 0x80000000u >> i; return r; }

static inline void rt_mult(Ctx *c, u32 a, u32 b) { s64 r = (s64)(s32)a * (s64)(s32)b; c->lo = (u32)r; c->hi = (u32)((u64)r >> 32); }
static inline void rt_multu(Ctx *c, u32 a, u32 b) { u64 r = (u64)a * b; c->lo = (u32)r; c->hi = (u32)(r >> 32); }
static inline void rt_madd(Ctx *c, u32 a, u32 b) { s64 o = (s64)(((u64)c->hi << 32) | c->lo); s64 r = o + (s64)(s32)a * (s64)(s32)b; c->lo = (u32)r; c->hi = (u32)((u64)r >> 32); }
static inline void rt_maddu(Ctx *c, u32 a, u32 b) { u64 o = ((u64)c->hi << 32) | c->lo; u64 r = o + (u64)a * b; c->lo = (u32)r; c->hi = (u32)(r >> 32); }
static inline void rt_msub(Ctx *c, u32 a, u32 b) { s64 o = (s64)(((u64)c->hi << 32) | c->lo); s64 r = o - (s64)(s32)a * (s64)(s32)b; c->lo = (u32)r; c->hi = (u32)((u64)r >> 32); }
static inline void rt_msubu(Ctx *c, u32 a, u32 b) { u64 o = ((u64)c->hi << 32) | c->lo; u64 r = o - (u64)a * b; c->lo = (u32)r; c->hi = (u32)(r >> 32); }
static inline void rt_div(Ctx *c, u32 ua, u32 ub) {
	s32 a = (s32)ua, b = (s32)ub;
	if (a == (s32)0x80000000 && b == -1) { c->lo = 0x80000000u; c->hi = 0; }
	else if (b != 0) { c->lo = (u32)(a / b); c->hi = (u32)(a % b); }
	else { c->lo = a < 0 ? 1 : 0xFFFFFFFFu; c->hi = (u32)a; }
}
static inline void rt_divu(Ctx *c, u32 a, u32 b) {
	if (b != 0) { c->lo = a / b; c->hi = a % b; }
	else { c->lo = a <= 0xFFFF ? 0xFFFF : 0xFFFFFFFFu; c->hi = a; }
}

// ---------------------------------------------------------------- FPU
static inline s32 rt_sat(double d) {
	if (d >= 2147483648.0) return 0x7FFFFFFF;
	if (d <= -2147483648.0) return (s32)0x80000000;
	if (d != d) return 0x7FFFFFFF;
	return (s32)d;
}
static inline double rt_round_even(double d) {
	float i = (float)floor(d); d -= i;
	if (d < 0.5f) return i;
	if (d > 0.5f) return i + 1.0f;
	return (((s64)i) & 1) ? i + 1.0f : i;
}
static inline s32 rt_round_w(float f) { return rt_sat(rt_round_even(f)); }
static inline s32 rt_trunc_w(float f) { return rt_sat(truncf(f)); }
static inline s32 rt_ceil_w(float f) { return rt_sat(ceilf(f)); }
static inline s32 rt_floor_w(float f) { return rt_sat(floorf(f)); }
static inline s32 rt_cvt_w(Ctx *c, float f) {
	switch (c->fcr31 & 3) { case 0: return rt_round_w(f); case 1: return rt_trunc_w(f); case 2: return rt_ceil_w(f); default: return rt_floor_w(f); }
}
static inline float rt_sqrt(float f) { if (f < 0.0f) { union { u32 u; float f; } n = {0x7FC00000}; return n.f; } return sqrtf(f); }
static inline u32 rt_fcmp(float a, float b, int cond) {
	int un = (a != a) || (b != b);
	switch (cond & 7) {
	case 0: return 0;
	case 1: return un;
	case 2: return !un && a == b;
	case 3: return un || a == b;
	case 4: return !un && a < b;
	case 5: return un || a < b;
	case 6: return !un && a <= b;
	default: return un || a <= b;
	}
}
static inline u32 rt_cfc1(Ctx *c, int r) { if (r == 31) return (c->fcr31 & ~(1u << 23)) | (c->fpcond << 23); if (r == 0) return 0x3351; return 0; }
static inline void rt_ctc1(Ctx *c, int r, u32 v) { if (r == 31) { c->fcr31 = v & 0x0181FFFF; c->fpcond = (v >> 23) & 1; } }

// ---------------------------------------------------------------- calls into the runtime
void HLE(Ctx *c, u32 nid);
u32 rt_hookval(Ctx *c, u32 id, u32 v);
void rt_hookcall(Ctx *c, u32 id, void (*orig)(Ctx *c));
void rt_call(Ctx *c, u32 addr);
void rt_jump(Ctx *c, u32 addr);
void rt_bad_insn(Ctx *c, u32 pc, u32 op);
void rt_break(Ctx *c, u32 pc, u32 op);
void rt_syscall(Ctx *c, u32 pc, u32 op);

typedef void (*InterpFn)(Ctx *c, u32 op);
#define INTERP(name, c, pc_, op) do { (c)->pc = (pc_); interp_##name((c), (op)); } while (0)
#define X(n) void interp_##n(Ctx *c, u32 op);
#include "interp_list.h"
#undef X

typedef void (*RecompFn)(Ctx *c);
typedef struct FuncEnt { u32 off; RecompFn fn; } FuncEnt;
typedef struct RecompModule {
	const char *name;
	u32 crc, file_size;
	u32 *mb;
	const FuncEnt *funcs;
	u32 nfuncs;
	u32 text_start, text_end;
} RecompModule;

#ifdef __cplusplus
}
#endif
