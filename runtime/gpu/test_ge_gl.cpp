// Backend test for the rcsm GE: runs the same display lists through the selected backend (software
// or PPSSPP's OpenGL renderer) in an SDL OpenGL window, the way the runtime does (sceGe HLE calls,
// vblank upkeep, ge_present into framebuffer 0), and writes what was presented to PNGs so the
// backends can be compared (tools/compare_backends.sh). Covers: clears, gouraud triangles, a CALLed
// sub-list, depth test, stencil test, render-to-texture (a framebuffer used as a texture), block
// transfers VRAM -> RAM (CPU checks the bytes) and RAM -> VRAM, a display buffer written by the CPU,
// a RAM texture rewritten by the CPU between frames (like a movie), and signal/finish callbacks.
//
//   build/test_ge_gl soft|gl [out-prefix] [internal-scale]
//   TEST_GE_BENCH=n: also time n frames of a heavy scene.
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
#include <SDL2/SDL.h>

#include "GL/glew.h"  // the library's GL loader (rcsm_gl* pointers, filled by ge_gl_init)
#include "../kernel.h"
#include "ge_soft.h"

// ================================================================= minimal kernel stand-ins
u8 *g_mem;
int g_loglevel = getenv("TEST_LOGLEVEL") ? atoi(getenv("TEST_LOGLEVEL")) : 1;
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
struct HandlerCall { u32 handler, a0, a1; };
static std::vector<HandlerCall> handlerCalls;
static void run_interrupts() {
	for (size_t i = 0; i < intrq.size(); i++) {
		IntrReq q = intrq[i];
		if (q.handler) handlerCalls.push_back({q.handler, q.a0, q.a1});
		if (q.done) q.done(q.ud, 0);
	}
	intrq.clear();
}
s32 thread_wait(int wait_type, u32 id, u32 timeout_ptr, bool cb) {
	Thread *t = g_cur;
	t->status = TS_WAITING;
	t->wait_type = wait_type;
	t->wait_id = id;
	run_interrupts();
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
void thread_delay_until(u64 when) { run_interrupts(); }

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

static void write_png(const std::string &path, const u8 *rgba, int w, int h) {
	std::vector<u8> raw;
	raw.reserve((size_t)(w * 3 + 1) * h);
	for (int y = 0; y < h; y++) {
		raw.push_back(0);
		const u8 *row = rgba + (size_t)y * w * 4;
		for (int x = 0; x < w; x++) { raw.push_back(row[x * 4]); raw.push_back(row[x * 4 + 1]); raw.push_back(row[x * 4 + 2]); }
	}
	uLongf zlen = compressBound(raw.size());
	std::vector<u8> z(zlen);
	compress2(z.data(), &zlen, raw.data(), raw.size(), 6);
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) fatal("can't write %s", path.c_str());
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

static SDL_Window *win;
static int winW = 960, winH = 544;

// One host frame: vblank upkeep, present into the window, swap.
static void end_frame() {
	ge_soft_vblank();
	glViewport(0, 0, winW, winH);
	glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
	if (!getenv("TEST_NO_CLEAR"))
		glClear(GL_COLOR_BUFFER_BIT);
	ge_present(0, 0, winW, winH);
	SDL_GL_SwapWindow(win);
}

static std::vector<u8> read_display(int w, int h) {
	std::vector<u8> px((size_t)w * h * 4);
	CHECK(ge_read_display(px.data(), w, h) == 0, "ge_read_display failed");
	return px;
}
static u32 px_at(const std::vector<u8> &img, int w, int x, int y) {
	const u8 *p = &img[((size_t)y * w + x) * 4];
	return p[0] | p[1] << 8 | p[2] << 16;
}

// Vertex writers (through mode).
static void vtx_c(u32 a, u32 color, int x, int y, int z = 0) { SW(a, color); SH(a + 4, x); SH(a + 6, y); SH(a + 8, z); }
static void vtx_tc(u32 a, int u, int v, u32 color, int x, int y, int z = 0) { SH(a, u); SH(a + 2, v); SW(a + 4, color); SH(a + 8, x); SH(a + 10, y); SH(a + 12, z); }

static void target(ListBuilder &l, u32 fbaddr, int stride) {
	l.cmd(0x9C, fbaddr & 0xFFFFFF);       // FRAMEBUFPTR
	l.cmd(0x9D, stride);                  // FRAMEBUFWIDTH
	l.cmd(0xD2, 3);                       // 8888
	l.cmd(0x9E, 0x088000);                // ZBUFPTR (0x04088000)
	l.cmd(0x9F, 512);
	l.cmd(0x15, 0);                       // REGION1
	l.cmd(0x16, (271 << 10) | 479);       // REGION2
	l.cmd(0xD4, 0);                       // SCISSOR1
	l.cmd(0xD5, (271 << 10) | 479);       // SCISSOR2
	l.cmd(0xD6, 0);                       // MINZ
	l.cmd(0xD7, 0xFFFF);                  // MAXZ
}

// ================================================================= the test
int main(int argc, char **argv) {
	const bool useGL = argc > 1 && !strcmp(argv[1], "gl");
	const std::string prefix = argc > 2 ? argv[2] : (useGL ? "ge_gl" : "ge_soft");
	const int scale = argc > 3 ? atoi(argv[3]) : 1;

	void *m = mmap(nullptr, 0x40000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (m == MAP_FAILED) fatal("mmap failed");
	g_mem = (u8 *)m;
	static Thread mainThread;
	strcpy(mainThread.name, "test");
	mainThread.status = TS_RUNNING;
	g_cur = &mainThread;
	ctx.r[28] = 0x08A80000;

	// The frontend's part: an OpenGL 3.3 core context on this thread.
	if (SDL_Init(SDL_INIT_VIDEO) != 0) fatal("SDL_Init: %s", SDL_GetError());
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	win = SDL_CreateWindow("test_ge_gl", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW, winH, SDL_WINDOW_OPENGL);
	if (!win) fatal("SDL_CreateWindow: %s", SDL_GetError());
	SDL_GLContext glc = SDL_GL_CreateContext(win);
	if (!glc) fatal("SDL_GL_CreateContext: %s", SDL_GetError());
	SDL_GL_SetSwapInterval(0);

	ge_select_backend(useGL ? 1 : 0);
	ge_soft_init();
	ge_gl_init(SDL_GL_GetProcAddress, scale);
	ge_soft_set_display(0x04000000, 512, 3);

	const u32 cbstruct = 0x08880000;
	SW(cbstruct + 0, 0x08900100); SW(cbstruct + 4, 0x5151);
	SW(cbstruct + 8, 0x08900200); SW(cbstruct + 12, 0xF1F1);
	const u32 cbid = call("sceGeSetCallback", cbstruct);

	// --- vertices
	const u32 V = 0x08840000;
	const u32 vClear = V, vTri = V + 0x40, vRTClear = V + 0x80, vRTTri = V + 0xC0, vDepthA = V + 0x100, vDepthB = V + 0x120,
		vStencilMark = V + 0x140, vStencilFill = V + 0x160, vRTSprite = V + 0x180, vTexSprite = V + 0x1C0;
	vtx_c(vClear, 0xFF402010, 0, 0); vtx_c(vClear + 12, 0xFF402010, 480, 272);
	vtx_c(vTri, 0xFF0000FF, 20, 250); vtx_c(vTri + 12, 0xFF00FF00, 110, 20); vtx_c(vTri + 24, 0xFFFF0000, 200, 250);
	vtx_c(vRTClear, 0xFFFFFFFF, 0, 0); vtx_c(vRTClear + 12, 0xFFFFFFFF, 128, 128);
	vtx_c(vRTTri, 0xFF0000FF, 10, 118); vtx_c(vRTTri + 12, 0xFF00C000, 64, 8); vtx_c(vRTTri + 24, 0xFFFF0000, 118, 118);
	vtx_c(vDepthA, 0xFFFF8000, 220, 20, 30000); vtx_c(vDepthA + 12, 0xFFFF8000, 300, 100, 30000);       // blue-ish (ABGR)
	vtx_c(vDepthB, 0xFF00FFFF, 260, 60, 10000); vtx_c(vDepthB + 12, 0xFF00FFFF, 340, 140, 10000);       // yellow, behind
	vtx_c(vStencilMark, 0x8000FF00, 360, 20); vtx_c(vStencilMark + 12, 0x8000FF00, 420, 60);            // green, writes stencil
	vtx_c(vStencilFill, 0xFFFF00FF, 340, 10); vtx_c(vStencilFill + 12, 0xFFFF00FF, 470, 120);           // magenta, stencil-tested
	vtx_tc(vRTSprite, 0, 0, 0xFFFFFFFF, 220, 140); vtx_tc(vRTSprite + 16, 128, 128, 0xFFFFFFFF, 348, 268);

	// --- sub-list: the gouraud triangle (CALL/RET)
	ListBuilder sub(0x08810000);
	sub.cmd(0x12, 0x80011C);              // VERTEXTYPE: through, pos16, col8888
	sub.cmd(0x01, vTri & 0xFFFFFF);
	sub.cmd(0x04, (3 << 16) | 3);
	sub.cmd(0x0B);                        // RET

	// --- frame 1
	ListBuilder l(0x08800000);
	l.cmd(0x10, 0x080000);                // BASE
	l.cmd(0x50, 1);                       // SHADEMODE gouraud
	l.cmd(0x1E, 0); l.cmd(0x21, 0); l.cmd(0x23, 0); l.cmd(0x24, 0);
	l.cmd(0xE8, 0); l.cmd(0xE9, 0);
	// Render target: 128x128 at 0x04110000.
	target(l, 0x04110000, 128);
	l.cmd(0xD5, (127 << 10) | 127);
	l.cmd(0x16, (127 << 10) | 127);
	l.cmd(0xD3, 1 | (3 << 8));            // CLEARMODE color+stencil
	l.cmd(0x12, 0x80011C); l.cmd(0x01, vRTClear & 0xFFFFFF); l.cmd(0x04, (6 << 16) | 2);
	l.cmd(0xD3, 0);
	l.cmd(0x01, vRTTri & 0xFFFFFF); l.cmd(0x04, (3 << 16) | 3);
	// Display framebuffer.
	target(l, 0x04000000, 512);
	l.cmd(0xD3, 1 | (7 << 8));            // clear color, stencil, depth (to 0)
	l.cmd(0x12, 0x80011C); l.cmd(0x01, vClear & 0xFFFFFF); l.cmd(0x04, (6 << 16) | 2);
	l.cmd(0xD3, 0);
	l.cmd(0x0A, sub.start & 0xFFFFFF);    // CALL the triangle
	// Depth: A (z 30000) then B (z 10000) with GREATER: B only where A isn't.
	l.cmd(0x23, 1);                       // ZTESTENABLE
	l.cmd(0xDE, 6);                       // ZTEST GREATER
	l.cmd(0xE7, 0);                       // depth writes on
	l.cmd(0x01, vDepthA & 0xFFFFFF); l.cmd(0x04, (6 << 16) | 2);
	l.cmd(0x01, vDepthB & 0xFFFFFF); l.cmd(0x04, (6 << 16) | 2);
	l.cmd(0x23, 0);
	// Stencil: mark a rectangle with ref 0x80 (REPLACE), then fill only where stencil == 0x80.
	l.cmd(0x24, 1);                       // STENCILTESTENABLE
	l.cmd(0xDC, 1 | (0x80 << 8) | (0xFF << 16));   // ALWAYS, ref 0x80
	l.cmd(0xDD, 2 | (2 << 8) | (2 << 16));         // REPLACE on all
	l.cmd(0x01, vStencilMark & 0xFFFFFF); l.cmd(0x04, (6 << 16) | 2);
	l.cmd(0xDC, 2 | (0x80 << 8) | (0xFF << 16));   // EQUAL 0x80
	l.cmd(0xDD, 0);                                 // KEEP
	l.cmd(0x01, vStencilFill & 0xFFFFFF); l.cmd(0x04, (6 << 16) | 2);
	l.cmd(0x24, 0);
	// Render-to-texture: the 128x128 target as a texture.
	l.cmd(0x1E, 1);                       // TEXTUREMAPENABLE
	l.cmd(0xC3, 3); l.cmd(0xC2, 0);
	l.cmd(0xA0, 0x110000);                // TEXADDR0 low 24 bits
	l.cmd(0xA8, (0x04 << 16) | 128);      // TEXBUFWIDTH0: address bits 24-27, width
	l.cmd(0xB8, (7 << 8) | 7);            // 128x128
	l.cmd(0xC6, 0); l.cmd(0xC9, 3 | (1 << 8)); l.cmd(0xCB, 0);
	l.cmd(0x12, 0x80011E); l.cmd(0x01, vRTSprite & 0xFFFFFF); l.cmd(0x04, (6 << 16) | 2);
	l.cmd(0x1E, 0);
	// Block transfers: 64x32 of the display (at 250,200, inside the RTT sprite) to RAM, then back to (380,200).
	const u32 ram = 0x08C00000;
	l.cmd(0xB2, 0x000000); l.cmd(0xB3, (0x04 << 16) | 512);        // src 0x04000000, stride 512
	l.cmd(0xB4, ram & 0xFFFFFF); l.cmd(0xB5, (0x08 << 16) | 64);   // dst RAM, stride 64
	l.cmd(0xEB, 250 | (200 << 10)); l.cmd(0xEC, 0);
	l.cmd(0xEE, 63 | (31 << 10));
	const int xfer = getenv("TEST_NO_XFER") ? atoi(getenv("TEST_NO_XFER")) : 0;  // debugging: 1 skips the readback, 2 the upload
	l.cmd(xfer == 1 ? 0x00 : 0xEA, 1);    // TRANSFERSTART, 32-bit
	l.cmd(0xB2, ram & 0xFFFFFF); l.cmd(0xB3, (0x08 << 16) | 64);
	l.cmd(0xB4, 0x000000); l.cmd(0xB5, (0x04 << 16) | 512);
	l.cmd(0xEB, 0); l.cmd(0xEC, 380 | (200 << 10));
	l.cmd(0xEE, 63 | (31 << 10));
	l.cmd(xfer == 2 ? 0x00 : 0xEA, 1);
	l.cmd(0x0E, (0x02 << 16) | 0x0777);   // SIGNAL continue
	l.cmd(0x0C, 0);
	l.cmd(0x0F, 0x1234);                  // FINISH
	l.cmd(0x0C, 0);

	// Two frames, like a game: PPSSPP (and so the OpenGL backend) presents a framebuffer once it has
	// been drawn to after its creation (the first frame of a new buffer counts as not dirty).
	u32 id = 0, ramPx = 0, ramPx2 = 0;
	for (int frame = 0; frame < 2; frame++) {
		memset(PTR(ram), 0, 64 * 32 * 4);
		id = call("sceGeListEnQueue", l.start, 0, cbid, 0);
		CHECK((int)id >= 0, "EnQueue %08x", id);
		CHECK(call("sceGeListSync", id, 0) == 0, "ListSync");
		CHECK(call("sceGeDrawSync", 0) == 0, "DrawSync");
		// The CPU reads what the first transfer wrote to RAM (OpenGL: read back from the GPU).
		ramPx = LW(ram) & 0xFFFFFF;
		ramPx2 = LW(ram + (20 * 64 + 40) * 4) & 0xFFFFFF;
		end_frame();
	}
	CHECK(handlerCalls.size() == 4 && handlerCalls[0].a0 == 0x777 && handlerCalls[1].a0 == 0x1234, "callbacks (%zu)", handlerCalls.size());
	std::vector<u8> scene = read_display(480, 272);
	write_png(prefix + "_scene.png", scene.data(), 480, 272);
	if (scale > 1) {
		std::vector<u8> big = read_display(480 * scale, 272 * scale);
		write_png(prefix + "_scene_x" + std::to_string(scale) + ".png", big.data(), 480 * scale, 272 * scale);
	}
	u32 bg = px_at(scene, 480, 5, 5), tri = px_at(scene, 480, 110, 200), dA = px_at(scene, 480, 240, 40), dB = px_at(scene, 480, 330, 130),
		dAB = px_at(scene, 480, 280, 80), st = px_at(scene, 480, 390, 40), stOut = px_at(scene, 480, 450, 100),
		rtt = px_at(scene, 480, 225, 145), rttTri = px_at(scene, 480, 284, 240), copied = px_at(scene, 480, 380, 200);
	printf("%s: bg %06x tri %06x depthA %06x depthB %06x overlap %06x stencil in %06x out %06x rtt %06x/%06x ram %06x/%06x copied %06x\n",
		useGL ? "gl" : "soft", bg, tri, dA, dB, dAB, st, stOut, rtt, rttTri, ramPx, ramPx2, copied);
	auto near = [](u32 a, u32 b) {
		for (int s = 0; s < 24; s += 8)
			if (abs((int)((a >> s) & 0xFF) - (int)((b >> s) & 0xFF)) > 8) return false;
		return true;
	};
	CHECK(near(bg, 0x402010), "background %06x", bg);
	CHECK(near(dA, 0xFF8000), "depth A %06x", dA);
	CHECK(near(dB, 0x00FFFF), "depth B outside A %06x", dB);
	CHECK(near(dAB, 0xFF8000), "depth overlap must keep A %06x", dAB);
	CHECK(near(st, 0xFF00FF), "stencil-tested fill inside the mark %06x", st);
	CHECK(near(stOut, 0x402010), "stencil-tested fill outside the mark %06x", stOut);
	CHECK(near(rtt, 0xFFFFFF), "render target clear seen through the texture %06x", rtt);
	CHECK(!near(rttTri, 0xFFFFFF) && !near(rttTri, 0x402010), "render target triangle seen through the texture %06x", rttTri);
	CHECK(near(ramPx, px_at(scene, 480, 250, 200)) && near(ramPx2, px_at(scene, 480, 290, 220)), "block transfer to RAM %06x %06x", ramPx, ramPx2);
	CHECK(near(copied, px_at(scene, 480, 250, 200)), "block transfer back to VRAM %06x", copied);

	// --- live internal-resolution change (OpenGL; a no-op for software): the same scene at 3x.
	if (useGL) {
		ge_gl_set_internal_scale(3);
		ge_gl_set_options(2, 16, 0);
		for (int frame = 0; frame < 2; frame++) {  // the change applies at the end of the next present
			id = call("sceGeListEnQueue", l.start, 0, cbid, 0);
			call("sceGeListSync", id, 0);
			end_frame();
		}
		std::vector<u8> rescaled = read_display(480, 272);
		std::vector<u8> big3 = read_display(1440, 816);
		write_png(prefix + "_scene_rescaled_x3.png", big3.data(), 1440, 816);
		u32 r0 = px_at(rescaled, 480, 240, 40), r1 = px_at(rescaled, 480, 390, 40);
		CHECK(near(r0, 0xFF8000) && near(r1, 0xFF00FF), "scene after switching to 3x: %06x %06x", r0, r1);
		ge_gl_set_internal_scale(scale);
	}

	// --- frame 2: a display buffer the CPU wrote (565, in main RAM), like a movie player that blits
	const u32 cpuFb = 0x08D00000;
	for (int y = 0; y < 272; y++)
		for (int x = 0; x < 512; x++) {
			u32 r = x * 31 / 479, g = y * 63 / 271, b = ((x / 32 + y / 32) & 1) ? 31 : 0;
			SH(cpuFb + (y * 512 + x) * 2, r | (g << 5) | (b << 11));
		}
	ge_notify_cpu_write(cpuFb, 512 * 272 * 2);
	ge_soft_set_display(cpuFb, 512, 0);
	end_frame();
	std::vector<u8> cpu = read_display(480, 272);
	write_png(prefix + "_cpufb.png", cpu.data(), 480, 272);
	u32 c0 = px_at(cpu, 480, 10, 10), c1 = px_at(cpu, 480, 470, 260);
	CHECK(near(c0, 0x000000) && near(c1, 0xFFFFFF) == false && (c1 & 0xFF) > 0xE0 && ((c1 >> 8) & 0xFF) > 0xE0, "CPU display %06x %06x", c0, c1);

	// --- frames 3-4: a RAM texture drawn full screen, rewritten by the CPU between frames (movie-like)
	const u32 tex = 0x08E00000;
	auto fill_tex = [&](bool second) {
		for (int y = 0; y < 256; y++)
			for (int x = 0; x < 256; x++) {
				bool check = ((x >> 5) ^ (y >> 5)) & 1;
				u32 c = second ? (check ? 0xFF00C0FF : 0xFF203000) : (check ? 0xFFFF4000 : 0xFF00FF40);
				SW(tex + (y * 256 + x) * 4, c);
			}
	};
	vtx_tc(vTexSprite, 0, 0, 0xFFFFFFFF, 0, 0); vtx_tc(vTexSprite + 16, 256, 256, 0xFFFFFFFF, 480, 272);
	ListBuilder t(0x08820000);
	t.cmd(0x10, 0x080000);
	target(t, 0x04000000, 512);
	t.cmd(0x23, 0); t.cmd(0x24, 0);
	t.cmd(0x1E, 1); t.cmd(0xC3, 3); t.cmd(0xC2, 0);
	t.cmd(0xA0, tex & 0xFFFFFF); t.cmd(0xA8, (0x08 << 16) | 256); t.cmd(0xB8, (8 << 8) | 8);
	t.cmd(0xC6, 0); t.cmd(0xC9, 3 | (1 << 8)); t.cmd(0xCB, 0);
	t.cmd(0x12, 0x80011E); t.cmd(0x01, vTexSprite & 0xFFFFFF); t.cmd(0x04, (6 << 16) | 2);
	t.cmd(0x1E, 0);
	t.cmd(0x0F, 0); t.cmd(0x0C, 0);
	ge_soft_set_display(0x04000000, 512, 3);
	std::vector<u8> movie[2];
	for (int frame = 0; frame < 2; frame++) {
		fill_tex(frame == 1);
		ge_write_formatted_from_memory(tex, 256 * 256 * 4, 256, 3);  // what sceMpegAvcDecode reports
		id = call("sceGeListEnQueue", t.start, 0, (u32)-1, 0);
		call("sceGeListSync", id, 0);
		end_frame();
		movie[frame] = read_display(480, 272);
	}
	write_png(prefix + "_movie.png", movie[1].data(), 480, 272);
	u32 m0 = px_at(movie[0], 480, 5, 5), m1 = px_at(movie[1], 480, 5, 5);
	CHECK(near(m0, 0x00FF40) && near(m1, 0x203000), "texture rewritten by the CPU: frame 1 %06x frame 2 %06x", m0, m1);

	// --- optional benchmark: a heavy frame (many overdrawn textured, blended triangles).
	if (const char *b = getenv("TEST_GE_BENCH")) {
		const int frames = atoi(b);
		const int tris = 4000;
		const u32 bv = 0x09000000;
		for (int i = 0; i < tris; i++) {
			int cx = (i * 37) % 480, cy = (i * 53) % 272;
			u32 col = 0x80000000u | ((i * 2654435761u) & 0xFFFFFF);
			vtx_tc(bv + i * 48 + 0, 0, 0, col, cx - 60, cy - 40);
			vtx_tc(bv + i * 48 + 16, 256, 0, col, cx + 60, cy - 30);
			vtx_tc(bv + i * 48 + 32, 128, 256, col, cx, cy + 50);
		}
		ListBuilder h(0x08830000);
		h.cmd(0x10, 0x090000 >> 0);
		target(h, 0x04000000, 512);
		h.cmd(0x10, 0x080000);
		h.cmd(0xD3, 1 | (7 << 8)); h.cmd(0x12, 0x80011C); h.cmd(0x01, vClear & 0xFFFFFF); h.cmd(0x04, (6 << 16) | 2); h.cmd(0xD3, 0);
		h.cmd(0x1E, 1); h.cmd(0xC3, 3); h.cmd(0xC2, 0);
		h.cmd(0xA0, tex & 0xFFFFFF); h.cmd(0xA8, (0x08 << 16) | 256); h.cmd(0xB8, (8 << 8) | 8);
		h.cmd(0xC6, 1); h.cmd(0xC9, 0 | (1 << 8));    // linear, modulate
		h.cmd(0x21, 1); h.cmd(0xDF, 2 | (3 << 4));      // blend src alpha / inv src alpha
		h.cmd(0x10, 0x090000);
		h.cmd(0x12, 0x80011E); h.cmd(0x01, bv & 0xFFFFFF); h.cmd(0x04, (3 << 16) | (tris * 3));
		h.cmd(0x21, 0); h.cmd(0x1E, 0);
		h.cmd(0x0F, 0); h.cmd(0x0C, 0);
		u64 t0 = now_us(), tGe = 0;
		for (int i = 0; i < frames; i++) {
			u64 a = now_us();
			id = call("sceGeListEnQueue", h.start, 0, (u32)-1, 0);
			call("sceGeListSync", id, 0);
			ge_soft_flush();
			tGe += now_us() - a;
			end_frame();
		}
		glFinish();
		u64 t1 = now_us();
		std::vector<u8> bench = read_display(480, 272);
		write_png(prefix + "_bench.png", bench.data(), 480, 272);
		printf("bench (%s, x%d): %d frames of %d blended textured triangles: %.2f ms/frame (GE list processing %.2f ms, present %.2f ms)\n",
			useGL ? "OpenGL" : "software", scale, frames, tris, (t1 - t0) / 1000.0 / frames, tGe / 1000.0 / frames, (t1 - t0 - tGe) / 1000.0 / frames);
	}

	printf("%s: wrote %s_*.png\n", failures ? "FAILED" : "OK", prefix.c_str());
	SDL_GL_DeleteContext(glc);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return failures ? 1 : 0;
}
