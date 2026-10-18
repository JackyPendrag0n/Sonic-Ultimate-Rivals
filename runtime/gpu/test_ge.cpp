// Standalone test of the software GE: builds display lists in guest RAM, runs them through the
// registered sceGe HLE functions (the same path recompiled code takes), simulates the rcsm kernel's
// scheduler/interrupt queue, checks list states and callbacks, and writes VRAM to a PNG.
//
//   build/test_ge [out.png]
//
// GPL-2.0+ (links PPSSPP code), see LICENSE.TXT.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <time.h>
#include <zlib.h>

#include "../kernel.h"

extern "C" {
void ge_soft_init(void);
void ge_soft_vblank(void);
void ge_soft_set_display(u32 topaddr, int stride, int format);
void ge_soft_flush(void);
}

// ================================================================= minimal kernel stand-ins
u8 *g_mem;
int g_loglevel = 1;
Thread *g_cur;
bool g_in_intr;

void logf_(int lvl, const char *fmt, ...) {
	if (lvl > g_loglevel) return;
	va_list a;
	va_start(a, fmt);
	vfprintf(stderr, fmt, a);
	fputc('\n', stderr);
	va_end(a);
}
void fatal(const char *fmt, ...) {
	va_list a;
	va_start(a, fmt);
	vfprintf(stderr, fmt, a);
	fputc('\n', stderr);
	va_end(a);
	exit(1);
}
u64 now_us(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (u64)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static std::vector<HleEnt> hle;
void hle_register(const HleEnt *ents, int n) { hle.insert(hle.end(), ents, ents + n); }

struct IntrReq { u32 handler, a0, a1, a2, gp; void (*done)(void *, u32); void *ud; };
static std::vector<IntrReq> intrq;
void intr_queue_ex(u32 handler, u32 a0, u32 a1, u32 a2, u32 gp, void (*done)(void *, u32), void *ud) {
	intrq.push_back({handler, a0, a1, a2, gp, done, ud});
}

struct HandlerCall { u32 handler, a0, a1, a2, gp; };
static std::vector<HandlerCall> handlerCalls;

// What the scheduler does between thread switches: run queued interrupts (guest handlers are only
// recorded here) and their `done` hooks.
static void run_interrupts() {
	for (size_t i = 0; i < intrq.size(); i++) {
		IntrReq q = intrq[i];
		if (q.handler) {
			g_in_intr = true;
			handlerCalls.push_back({q.handler, q.a0, q.a1, q.a2, q.gp});
			g_in_intr = false;
		}
		if (q.done) q.done(q.ud, 0);
	}
	intrq.clear();
}

static int waits;
s32 thread_wait(int wait_type, u32 id, u32 timeout_ptr, bool cb) {
	Thread *t = g_cur;
	t->status = TS_WAITING;
	t->wait_type = wait_type;
	t->wait_id = id;
	waits++;
	run_interrupts();  // the scheduler runs while we're blocked
	if (t->status == TS_WAITING) fatal("deadlock: thread still waiting (type %d id %x)", wait_type, id);
	t->status = TS_RUNNING;
	return t->wait_result;
}
void thread_wake(Thread *t, s32 result) {
	if (t->status != TS_WAITING) return;
	t->wait_result = result;
	t->wait_type = W_NONE;
	t->status = TS_READY;
}
void thread_resched(void) {}
static int yields;
void thread_delay_until(u64 when) {  // zero-length delays are how the GE yields to the scheduler
	yields++;
	run_interrupts();
}

// ================================================================= helpers
static Ctx ctx;
static u32 call(const char *name, u32 a0 = 0, u32 a1 = 0, u32 a2 = 0, u32 a3 = 0) {
	for (const HleEnt &e : hle) {
		if (!strcmp(e.name, name)) {
			Ctx *c = &ctx;
			A(0) = a0; A(1) = a1; A(2) = a2; A(3) = a3;
			e.fn(c);
			return c->r[2];
		}
	}
	fatal("HLE %s not registered", name);
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

struct ListBuilder {
	u32 start, pc;
	explicit ListBuilder(u32 addr) : start(addr), pc(addr) {}
	void cmd(u32 c, u32 data = 0) { SW(pc, (c << 24) | (data & 0xFFFFFF)); pc += 4; }
};

static void write_png(const char *path, const u8 *rgba, int w, int h, int stride_px) {
	std::vector<u8> raw;
	raw.reserve((size_t)(w * 3 + 1) * h);
	for (int y = 0; y < h; y++) {
		raw.push_back(0);
		const u8 *row = rgba + (size_t)y * stride_px * 4;
		for (int x = 0; x < w; x++) { raw.push_back(row[x * 4]); raw.push_back(row[x * 4 + 1]); raw.push_back(row[x * 4 + 2]); }
	}
	uLongf zlen = compressBound(raw.size());
	std::vector<u8> z(zlen);
	compress2(z.data(), &zlen, raw.data(), raw.size(), 9);
	FILE *f = fopen(path, "wb");
	if (!f) fatal("can't write %s", path);
	auto be32 = [&](u32 v) { u8 b[4] = {u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v)}; fwrite(b, 1, 4, f); };
	auto chunk = [&](const char *type, const u8 *data, u32 len) {
		be32(len);
		fwrite(type, 1, 4, f);
		if (len) fwrite(data, 1, len, f);
		u32 crc = crc32(0, (const Bytef *)type, 4);
		crc = crc32(crc, data, len);
		be32(crc);
	};
	static const u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	fwrite(sig, 1, 8, f);
	u8 ihdr[13] = {u8(w >> 24), u8(w >> 16), u8(w >> 8), u8(w), u8(h >> 24), u8(h >> 16), u8(h >> 8), u8(h), 8, 2, 0, 0, 0};
	chunk("IHDR", ihdr, 13);
	chunk("IDAT", z.data(), (u32)zlen);
	chunk("IEND", nullptr, 0);
	fclose(f);
}

static u32 pixel(int x, int y) { return LW(0x04000000 + (y * 512 + x) * 4); }

// ================================================================= the test
int main(int argc, char **argv) {
	const char *out = argc > 1 ? argv[1] : "ge_test.png";
	void *m = mmap(nullptr, 0x40000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (m == MAP_FAILED) fatal("mmap failed");
	g_mem = (u8 *)m;

	static Thread mainThread;
	strcpy(mainThread.name, "test");
	mainThread.status = TS_RUNNING;
	g_cur = &mainThread;
	ctx.r[28] = 0x08A80000;  // "gp" handed to callbacks

	ge_soft_init();

	CHECK(call("sceGeEdramGetAddr") == 0x04000000, "EdramGetAddr");
	CHECK(call("sceGeEdramGetSize") == 0x00200000, "EdramGetSize");

	// Callback struct: signal handler at 0x08900100, finish handler at 0x08900200.
	const u32 cbstruct = 0x08880000;
	SW(cbstruct + 0, 0x08900100); SW(cbstruct + 4, 0x5151);
	SW(cbstruct + 8, 0x08900200); SW(cbstruct + 12, 0xF1F1);
	const u32 cbid = call("sceGeSetCallback", cbstruct);
	CHECK(cbid == 0, "SetCallback returned %d", cbid);

	// --- texture: 16x16 8888 at 0x08A00000 (checkerboard of red/yellow with a blue-ish gradient)
	const u32 tex = 0x08A00000;
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++) {
			bool check = ((x >> 2) ^ (y >> 2)) & 1;
			u32 r = check ? 0xFF : 0xF0, g = check ? 0x20 : 0xE0, b = (u32)(x * 16);
			SW(tex + (y * 16 + x) * 4, 0xFF000000 | (b << 16) | (g << 8) | r);
		}

	// --- vertices at 0x08840000
	const u32 vClear = 0x08840000, vTri = 0x08840100, vSprite = 0x08840200;
	auto vtx12 = [](u32 a, u32 color, int x, int y) { SW(a, color); SH(a + 4, x); SH(a + 6, y); SH(a + 8, 0); };
	vtx12(vClear, 0xFF402010, 0, 0);
	vtx12(vClear + 12, 0xFF402010, 480, 272);
	vtx12(vTri, 0xFF0000FF, 60, 250);      // red
	vtx12(vTri + 12, 0xFF00FF00, 150, 20); // green
	vtx12(vTri + 24, 0xFFFF0000, 270, 250);// blue
	auto vtx16 = [](u32 a, int u, int v, u32 color, int x, int y) { SH(a, u); SH(a + 2, v); SW(a + 4, color); SH(a + 8, x); SH(a + 10, y); SH(a + 12, 0); };
	vtx16(vSprite, 0, 0, 0xFFFFFFFF, 300, 70);
	vtx16(vSprite + 16, 16, 16, 0xFFFFFFFF, 428, 198);

	// --- sub-list (CALLed): the triangle
	ListBuilder sub(0x08810000);
	sub.cmd(0x12, 0x80011C);              // VERTEXTYPE: through, pos16, col8888
	sub.cmd(0x01, vTri & 0xFFFFFF);       // VADDR
	sub.cmd(0x04, (3 << 16) | 3);         // PRIM triangles x3
	sub.cmd(0x0B);                        // RET

	// --- main list at 0x08800000
	ListBuilder l(0x08800000);
	l.cmd(0x10, 0x080000);                // BASE: 0x08xxxxxx
	l.cmd(0x9C, 0x000000);                // FRAMEBUFPTR 0x04000000
	l.cmd(0x9D, 512);                     // FRAMEBUFWIDTH
	l.cmd(0xD2, 3);                       // FRAMEBUFPIXFORMAT 8888
	l.cmd(0x9E, 0x088000);                // ZBUFPTR
	l.cmd(0x9F, 512);                     // ZBUFWIDTH
	l.cmd(0x15, 0);                       // REGION1
	l.cmd(0x16, (271 << 10) | 479);       // REGION2
	l.cmd(0xD4, 0);                       // SCISSOR1
	l.cmd(0xD5, (271 << 10) | 479);       // SCISSOR2
	l.cmd(0x50, 1);                       // SHADEMODE gouraud
	l.cmd(0x1E, 0);                       // TEXTUREMAPENABLE off
	l.cmd(0x21, 0);                       // ALPHABLENDENABLE off
	l.cmd(0x23, 0);                       // ZTESTENABLE off
	l.cmd(0xE8, 0);                       // MASKRGB: write all
	l.cmd(0xE9, 0);                       // MASKALPHA
	// clear
	l.cmd(0xD3, 1 | (7 << 8));            // CLEARMODE on: color, stencil, depth
	l.cmd(0x12, 0x80011C);
	l.cmd(0x01, vClear & 0xFFFFFF);
	l.cmd(0x04, (6 << 16) | 2);           // PRIM sprites x2
	l.cmd(0xD3, 0);                       // CLEARMODE off
	// triangle via CALL/RET
	l.cmd(0x0A, sub.start & 0xFFFFFF);    // CALL
	const u32 stallAt = l.pc;             // the list is first enqueued stalled here
	// textured sprite
	l.cmd(0x1E, 1);                       // TEXTUREMAPENABLE
	l.cmd(0xC3, 3);                       // TEXFORMAT 8888
	l.cmd(0xC2, 0);                       // TEXMODE: not swizzled, 1 level
	l.cmd(0xA0, tex & 0xFFFFF0);          // TEXADDR0
	l.cmd(0xA8, ((tex >> 24) & 0xF) << 16 | 16);  // TEXBUFWIDTH0
	l.cmd(0xB8, (4 << 8) | 4);            // TEXSIZE0 16x16
	l.cmd(0xC6, 0);                       // TEXFILTER nearest
	l.cmd(0xC9, 3 | (1 << 8));            // TEXFUNC replace, RGBA
	l.cmd(0xCB, 0);                       // TEXFLUSH
	l.cmd(0x12, 0x80011E);                // VERTEXTYPE: through, tc16, col8888, pos16
	l.cmd(0x01, vSprite & 0xFFFFFF);
	l.cmd(0x04, (6 << 16) | 2);           // PRIM sprites
	l.cmd(0x1E, 0);
	// signal (handler, continue) then finish
	l.cmd(0x0E, (0x02 << 16) | 0x0777);   // SIGNAL HANDLER_CONTINUE, value 0x777
	l.cmd(0x0C, 0);                       // END
	l.cmd(0x0F, 0x1234);                  // FINISH
	l.cmd(0x0C, 0);                       // END
	const u32 listEnd = l.pc;

	// Enqueue stalled partway (like a game building its list as it goes).
	const u32 id = call("sceGeListEnQueue", l.start, stallAt, cbid, 0);
	CHECK((int)id >= 0, "EnQueue returned %08x", id);
	CHECK(call("sceGeListSync", id, 1) == 3, "list should be stalling");
	CHECK(call("sceGeDrawSync", 1) == 3, "draw should be stalling");

	call("sceGeListUpdateStallAddr", id, listEnd);
	// The list ran to its SIGNAL; the signal interrupt is pending until the scheduler runs.
	CHECK(handlerCalls.empty(), "no handler should have run yet");

	// sceGeListSync(wait) has to block until the finish callback has been delivered.
	int waitsBefore = waits;
	u32 r = call("sceGeListSync", id, 0);
	CHECK(r == 0, "ListSync wait returned %x", r);
	CHECK(waits == waitsBefore + 1, "ListSync should have blocked once (waits %d)", waits - waitsBefore);
	CHECK(handlerCalls.size() == 2, "expected signal+finish handler calls, got %zu", handlerCalls.size());
	if (handlerCalls.size() == 2) {
		CHECK(handlerCalls[0].handler == 0x08900100 && handlerCalls[0].a0 == 0x777 && handlerCalls[0].a1 == 0x5151 && handlerCalls[0].gp == 0x08A80000,
			"signal handler call %08x(%x, %x) gp %08x", handlerCalls[0].handler, handlerCalls[0].a0, handlerCalls[0].a1, handlerCalls[0].gp);
		CHECK(handlerCalls[1].handler == 0x08900200 && handlerCalls[1].a0 == 0x1234 && handlerCalls[1].a1 == 0xF1F1,
			"finish handler call %08x(%x, %x)", handlerCalls[1].handler, handlerCalls[1].a0, handlerCalls[1].a1);
	}
	CHECK(call("sceGeListSync", id, 1) == 0, "list should be complete");
	waitsBefore = waits;
	CHECK(call("sceGeDrawSync", 0) == 0, "DrawSync");
	CHECK(waits == waitsBefore, "DrawSync shouldn't block once everything is done");
	CHECK(call("sceGeDrawSync", 1) == 0, "draw should be complete");

	// A second list without a callback. Its FINISH interrupt is still to be delivered, so
	// sceGeListSync(wait) blocks until the scheduler has run it; sceGeDrawSync doesn't wait for that
	// (PPSSPP: the drawing itself is done), and nothing is pending afterwards.
	ListBuilder l2(0x08820000);
	l2.cmd(0x0F, 0);
	l2.cmd(0x0C, 0);
	const u32 id2 = call("sceGeListEnQueue", l2.start, 0, (u32)-1, 0);
	CHECK((int)id2 >= 0, "EnQueue 2 returned %08x", id2);
	waitsBefore = waits;
	CHECK(call("sceGeListSync", id2, 0) == 0, "ListSync 2");
	CHECK(waits == waitsBefore + 1, "ListSync should block for the pending finish interrupt");
	CHECK(call("sceGeListSync", id2, 1) == 0, "list 2 should be complete");
	waitsBefore = waits;
	CHECK(call("sceGeDrawSync", 0) == 0, "DrawSync 2");
	CHECK(waits == waitsBefore, "DrawSync shouldn't block");
	CHECK(handlerCalls.size() == 2, "no callback for a list without one");
	// A polling loop sees the list complete: the poll yields to the scheduler while GE work is queued.
	ListBuilder l3(0x08830000);
	l3.cmd(0x0F, 0);
	l3.cmd(0x0C, 0);
	const u32 id3 = call("sceGeListEnQueue", l3.start, 0, cbid, 0);
	int polls = 0, yieldsBefore = yields;
	u32 first = call("sceGeListSync", id3, 1);
	CHECK(first == 2, "list 3 should read as drawing until its interrupt (%d)", first);
	while (call("sceGeListSync", id3, 1) != 0 && polls < 10) polls++;
	CHECK(polls == 0 && yields == yieldsBefore + 1, "polling: %d more polls, %d yields", polls, yields - yieldsBefore);
	CHECK(handlerCalls.size() == 3 && handlerCalls[2].handler == 0x08900200, "finish callback for list 3");

	CHECK(call("sceGeGetCmd", 0xD2) == 0xD2000003, "GetCmd FRAMEBUFPIXFORMAT = %08x", call("sceGeGetCmd", 0xD2));

	// TEST_GE_BENCH=n: run the main list n more times and report the time per list.
	if (const char *b = getenv("TEST_GE_BENCH")) {
		int n = atoi(b);
		u64 t0 = now_us();
		for (int i = 0; i < n; i++) {
			u32 bid = call("sceGeListEnQueue", l.start, 0, (u32)-1, 0);
			call("sceGeListSync", bid, 0);
		}
		call("sceGeDrawSync", 0);
		u64 t1 = now_us();
		printf("bench: %d lists (clear + triangle + 128x128 sprite), %.1f us/list\n", n, (double)(t1 - t0) / (n ? n : 1));
	}

	ge_soft_set_display(0x04000000, 512, 3);
	ge_soft_vblank();

	// Pixel checks: clear color, triangle interior, sprite texels.
	u32 bg = pixel(10, 10), tri = pixel(160, 200), spr0 = pixel(302, 72), spr1 = pixel(302 + 32, 72);
	CHECK((bg & 0xFFFFFF) == 0x402010, "background %08x", bg);
	CHECK((tri & 0xFFFFFF) != 0x402010, "triangle not drawn (%08x)", tri);
	CHECK((spr0 & 0xFFFFFF) == 0x00E0F0, "sprite texel 0,0 %08x", spr0);
	CHECK((spr1 & 0xFFFF) == 0x20FF, "sprite texel 4,0 %08x", spr1);

	write_png(out, g_mem + 0x04000000, 480, 272, 512);
	printf("bg %08x tri %08x sprite %08x %08x; waits %d, handler calls %zu\n", bg, tri, spr0, spr1, waits, handlerCalls.size());
	printf("%s: wrote %s\n", failures ? "FAILED" : "OK", out);
	return failures ? 1 : 0;
}
