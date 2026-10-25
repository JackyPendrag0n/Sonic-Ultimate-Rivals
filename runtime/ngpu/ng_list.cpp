// Native GE renderer: the display list processor and the sceGe HLE (renderer 2). Lists run when they're
// enqueued (or when their stall address moves on), to their end or their stall address, on the calling thread;
// the game only ever waits for lists that have already finished.
#include "ngpu.h"
#include "../kernel.h"
#include "ng_internal.h"
#include "ng_draw.h"
#include "../gpu/ge_soft.h"

enum { LS_COMPLETED = 0, LS_QUEUED = 1, LS_DRAWING = 2, LS_STALLED = 3 };
enum { MAX_LISTS = 64, STACK = 32 };

struct GeList {
	bool used;
	int state;
	u32 pc, stall;
	int cbid;
	u32 stack[STACK][2];   // return address, offset
	int sp;
	u32 offset;
};
static GeList s_lists[MAX_LISTS];
static struct { u32 signal_func, signal_arg, finish_func, finish_arg, gp; bool used; } s_cbs[16];
static bool s_interrupts = true;
static u32 s_edramTrans = 0x400;

static void callback(int cbid, bool finish, u32 data) {
	if (!s_interrupts || cbid < 0 || cbid >= 16 || !s_cbs[cbid].used) return;
	u32 fn = finish ? s_cbs[cbid].finish_func : s_cbs[cbid].signal_func;
	u32 arg = finish ? s_cbs[cbid].finish_arg : s_cbs[cbid].signal_arg;
	if (fn) intr_queue(fn, data & 0xFFFF, arg, 0, s_cbs[cbid].gp);
}

static void run(GeList *l) {
	GeState &g = g_ge;
	g.offset = l->offset;
	l->state = LS_DRAWING;
	for (int guard = 0; guard < 8000000; guard++) {
		if (l->stall && l->pc == l->stall) { l->state = LS_STALLED; l->offset = g.offset; return; }
		if (!ng_valid(l->pc, 4)) { NG_LOGE("ngpu: display list at a bad address %08x", l->pc); break; }
		u32 pc = l->pc, op = ng_r32(pc), cmd = op >> 24, data = op & 0xFFFFFF;
		l->pc += 4;
		switch (cmd) {
		case CMD_JUMP: l->pc = g.rel(data & 0xFFFFFC); break;
		case CMD_BJUMP: break;   // (bounding boxes always pass)
		case CMD_CALL:
			if (l->sp < STACK) { l->stack[l->sp][0] = l->pc; l->stack[l->sp][1] = g.offset; l->sp++; }
			l->pc = g.rel(data & 0xFFFFFC);
			break;
		case CMD_RET:
			if (l->sp) { l->sp--; l->pc = l->stack[l->sp][0]; g.offset = l->stack[l->sp][1]; }
			break;
		case CMD_OFFSET: g.offset = data << 8; break;
		case CMD_ORIGIN: g.offset = pc; break;
		case CMD_SIGNAL: {
			// (followed by END, whose data is the second half of a jump/call target)
			u32 behav = (data >> 16) & 0xFF, end = ng_r32(l->pc);
			u32 target = ((data & 0xFFFF) << 16) | (end & 0xFFFF);
			if (behav >= 1 && behav <= 3) callback(l->cbid, false, data);
			else if (behav == 0x11) { l->pc = target; break; }
			else if (behav == 0x12) {
				if (l->sp < STACK) { l->stack[l->sp][0] = l->pc + 4; l->stack[l->sp][1] = g.offset; l->sp++; }
				l->pc = target;
				break;
			} else if (behav == 0x13) {
				if (l->sp) { l->sp--; l->pc = l->stack[l->sp][0]; g.offset = l->stack[l->sp][1]; }
				break;
			}
			l->pc += 4;   // past its END: the list goes on
			break;
		}
		case CMD_FINISH:
			ng_exec(pc, op);
			callback(l->cbid, true, data);
			break;
		case CMD_END:
			ng_exec(pc, op);
			l->state = LS_COMPLETED;
			l->offset = g.offset;
			return;
		default:
			ng_exec(pc, op);
			break;
		}
	}
	NG_LOGE("ngpu: display list ran away (pc %08x)", l->pc);
	l->state = LS_COMPLETED;
}

// SR2_NGPU_STATS=1: every 5 s, how long the lists took to run (decoding and GL calls) and how many draws they made
u32 g_ng_draws, g_ng_gl_draws;
static void run_timed(GeList *l) {
	static int on = -1;
	if (on < 0) on = getenv("SR2_NGPU_STATS") && atoi(getenv("SR2_NGPU_STATS"));
	if (!on) { run(l); return; }
	static u64 t0, busy, lists;
	u64 a = now_us();
	if (!t0) t0 = a;
	run(l);
	u64 b = now_us();
	busy += b - a; lists++;
	if (b - t0 >= 5000000) {
		NG_LOGE("ngpu stats: lists took %.1f%% of the time, %llu lists, %u draws (%.0f a second) in %u GL draws", busy * 100.0 / (b - t0),
		        (unsigned long long)lists, g_ng_draws, g_ng_draws * 1e6 / (b - t0), g_ng_gl_draws);
		t0 = b; busy = lists = 0; g_ng_draws = g_ng_gl_draws = 0;
	}
}

static u32 enqueue(u32 list, u32 stall, int cbid, u32 opt, bool head) {
	(void)opt; (void)head;
	int i;
	for (i = 0; i < MAX_LISTS; i++) if (!s_lists[i].used || s_lists[i].state == LS_COMPLETED) break;
	if (i == MAX_LISTS) return 0x80000021;
	GeList *l = &s_lists[i];
	*l = GeList();
	l->used = true;
	l->pc = list & 0x0FFFFFFF;
	l->stall = stall & 0x0FFFFFFF;
	l->cbid = cbid >= 0 && cbid < 16 ? cbid : -1;
	run_timed(l);
	return (u32)i;
}

static GeList *list_by_id(u32 id) { return id < MAX_LISTS && s_lists[id].used ? &s_lists[id] : nullptr; }

static void h_EdramGetAddr(Ctx *c) { RET(0x04000000); }
static void h_EdramGetSize(Ctx *c) { RET(0x00200000); }
static void h_EdramSetAddrTranslation(Ctx *c) { u32 o = s_edramTrans; s_edramTrans = A(0); RET(o); }
static void h_ListEnQueue(Ctx *c) { RET(enqueue(A(0), A(1), (int)A(2), A(3), false)); }
static void h_ListEnQueueHead(Ctx *c) { RET(enqueue(A(0), A(1), (int)A(2), A(3), true)); }
static void h_ListDeQueue(Ctx *c) {
	GeList *l = list_by_id(A(0));
	if (!l) { RET(0x80000100); return; }
	l->used = false;
	RET(0);
}
static void h_ListUpdateStallAddr(Ctx *c) {
	GeList *l = list_by_id(A(0));
	if (!l) { RET(0x80000100); return; }
	l->stall = A(1) & 0x0FFFFFFF;
	if (l->state == LS_STALLED) run_timed(l);
	RET(0);
}
static void h_ListSync(Ctx *c) {
	GeList *l = list_by_id(A(0));
	if (!l) { RET(0x80000100); return; }
	RET(A(1) == 1 ? (u32)l->state : 0);
}
static void h_DrawSync(Ctx *c) {
	if (A(0) == 1) {
		for (auto &l : s_lists) if (l.used && l.state == LS_STALLED) { RET(LS_STALLED); return; }
		RET(LS_COMPLETED);
		return;
	}
	RET(0);
}
static void h_Break(Ctx *c) { RET(0); }
static void h_Continue(Ctx *c) { RET(0); }
static void h_SetCallback(Ctx *c) {
	int i;
	for (i = 0; i < 16; i++) if (!s_cbs[i].used) break;
	if (i == 16) { RET(0x80000022); return; }
	u32 p = A(0);
	s_cbs[i].signal_func = ng_r32(p); s_cbs[i].signal_arg = ng_r32(p + 4);
	s_cbs[i].finish_func = ng_r32(p + 8); s_cbs[i].finish_arg = ng_r32(p + 12);
	s_cbs[i].gp = c->r[28];
	s_cbs[i].used = true;
	RET(i);
}
static void h_UnsetCallback(Ctx *c) { if (A(0) < 16) s_cbs[A(0)].used = false; RET(0); }

// The context: the command words and the matrices, in a layout of our own (only RestoreContext reads it).
struct SavedContext { u32 cmd[256]; float m[12 + 12 + 16 + 12 + 96]; };
static_assert(sizeof(SavedContext) <= 512 * 4, "PspGeContext is 512 words");
static void h_SaveContext(Ctx *c) {
	if (!ng_valid(A(0), sizeof(SavedContext))) { RET(0x80000103); return; }
	SavedContext *s = (SavedContext *)ng_ptr(A(0));
	memcpy(s->cmd, g_ge.cmd, sizeof s->cmd);
	float *m = s->m;
	memcpy(m, g_ge.world, 48); memcpy(m + 12, g_ge.view, 48); memcpy(m + 24, g_ge.proj, 64);
	memcpy(m + 40, g_ge.tgen, 48); memcpy(m + 52, g_ge.bone, 384);
	RET(0);
}
static void h_RestoreContext(Ctx *c) {
	if (!ng_valid(A(0), sizeof(SavedContext))) { RET(0x80000103); return; }
	const SavedContext *s = (const SavedContext *)ng_ptr(A(0));
	memcpy(g_ge.cmd, s->cmd, sizeof s->cmd);
	const float *m = s->m;
	memcpy(g_ge.world, m, 48); memcpy(g_ge.view, m + 12, 48); memcpy(g_ge.proj, m + 24, 64);
	memcpy(g_ge.tgen, m + 40, 48); memcpy(g_ge.bone, m + 52, 384);
	RET(0);
}
static void h_GetMtx(Ctx *c) {
	u32 type = A(0), p = A(1);
	const float *m; int n = 12;
	if (type < 8) m = g_ge.bone + type * 12;
	else if (type == 8) m = g_ge.world;
	else if (type == 9) m = g_ge.view;
	else if (type == 10) { m = g_ge.proj; n = 16; }
	else if (type == 11) m = g_ge.tgen;
	else { RET(0x80000102); return; }
	if (!ng_valid(p, n * 4)) { RET(0x80000103); return; }
	for (int i = 0; i < n; i++) { u32 b; memcpy(&b, &m[i], 4); b >>= 8; memcpy(ng_ptr(p + i * 4), &b, 4); }
	RET(0);
}
static void h_GetCmd(Ctx *c) { RET(A(0) < 256 ? g_ge.cmd[A(0)] & 0xFFFFFF : 0x80000102); }
static void h_GetStack(Ctx *c) { RET(0); }

static const HleEnt kEnts[] = {
	{0xE47E40E4, "sceGeEdramGetAddr", h_EdramGetAddr},
	{0xAB49E76A, "sceGeListEnQueue", h_ListEnQueue},
	{0x1C0D95A6, "sceGeListEnQueueHead", h_ListEnQueueHead},
	{0xE0D68148, "sceGeListUpdateStallAddr", h_ListUpdateStallAddr},
	{0x03444EB4, "sceGeListSync", h_ListSync},
	{0xB287BD61, "sceGeDrawSync", h_DrawSync},
	{0xB448EC0D, "sceGeBreak", h_Break},
	{0x4C06E472, "sceGeContinue", h_Continue},
	{0xA4FC06A4, "sceGeSetCallback", h_SetCallback},
	{0x05DB22CE, "sceGeUnsetCallback", h_UnsetCallback},
	{0x1F6752AD, "sceGeEdramGetSize", h_EdramGetSize},
	{0xB77905EA, "sceGeEdramSetAddrTranslation", h_EdramSetAddrTranslation},
	{0xDC93CFEF, "sceGeGetCmd", h_GetCmd},
	{0x57C8945B, "sceGeGetMtx", h_GetMtx},
	{0x438A385A, "sceGeSaveContext", h_SaveContext},
	{0x0BF608FB, "sceGeRestoreContext", h_RestoreContext},
	{0x5FB86AB0, "sceGeListDeQueue", h_ListDeQueue},
	{0xE66CB92E, "sceGeGetStack", h_GetStack},
};

// ---------------------------------------------------------------- hook table (gpu/ge_soft.h)
extern "C" {
void ngpu_present(int x, int y, int w, int h);
int ngpu_read_display(uint8_t *rgba, int w, int h);
int ngpu_gl_init(void *(*get_proc)(const char *), int scale);
void ngpu_set_internal_scale(int scale);
void ngpu_set_options(int filter, int aniso);
void ngpu_set_view_offset(float x, float y);
void ngpu_set_display(uint32_t addr, int stride, int fmt);
int ngpu_views_available(void);
void ngpu_views_layout(int n, const int *dst, const int *src);
void ngpu_memory_written(uint32_t addr, uint32_t size);
void ngpu_tex_cache_clear(void);
void ngpu_shadow_present(void);
int ngvk_gl_init(void *(*get_proc)(const char *), int scale);
void ngvk_present(int x, int y, int w, int h);
int ngvk_read_display(uint8_t *rgba, int w, int h);
void ngvk_set_internal_scale(int scale);
void ngvk_set_options(int filter, int aniso);
void ngvk_set_view_offset(float x, float y);
void ngvk_set_display(uint32_t addr, int stride, int fmt);
void ngvk_views_layout(int n, const int *dst, const int *src);
void ngvk_memory_written(uint32_t addr, uint32_t size);
void ngvk_tex_cache_clear(void);
}

static bool s_vulkan;
// Vulkan (ng_vk.cpp) draws instead of OpenGL; takes effect at the next ngpu_register()
extern "C" void ngpu_use_vulkan(int on) { s_vulkan = on != 0; }

static void hle_init(void) {
	hle_register(kEnts, (int)(sizeof kEnts / sizeof kEnts[0]));
}
static uint32_t api_enqueue(uint32_t list, uint32_t stall, int cbid, uint32_t opt, int head) { return enqueue(list, stall, cbid, opt, head != 0); }
static void api_interrupts(int on) { s_interrupts = on != 0; }

extern "C" void ngpu_register(void) {
	GeNativeHooks &h = g_ge_native;
	h.init = hle_init;
	h.gl_init = ngpu_gl_init;
	h.set_internal_scale = ngpu_set_internal_scale;
	h.set_options = ngpu_set_options;
	h.present = ngpu_present;
	h.read_display = ngpu_read_display;
	h.set_display = ngpu_set_display;
	h.set_view_offset = ngpu_set_view_offset;
	h.views_available = ngpu_views_available;
	h.views_layout = ngpu_views_layout;
	h.memory_written = ngpu_memory_written;
	h.enqueue = api_enqueue;
	h.enable_interrupts = api_interrupts;
	h.tex_cache_clear = ngpu_tex_cache_clear;
	h.shadow_present = ngpu_shadow_present;
	if (s_vulkan) {
		h.gl_init = ngvk_gl_init;
		h.set_internal_scale = ngvk_set_internal_scale;
		h.set_options = ngvk_set_options;
		h.present = ngvk_present;
		h.read_display = ngvk_read_display;
		h.set_display = ngvk_set_display;
		h.set_view_offset = ngvk_set_view_offset;
		h.views_layout = ngvk_views_layout;
		h.memory_written = ngvk_memory_written;
		h.tex_cache_clear = ngvk_tex_cache_clear;
	}
}
