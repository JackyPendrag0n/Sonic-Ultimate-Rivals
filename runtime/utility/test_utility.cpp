// Standalone test of the utility library (PPSSPP sceUtility dialogs + PPGe on the rcsm kernel API):
// drives the registered sceUtility* HLE functions the way a game does - a param struct in guest
// memory, InitStart, GetStatus/Update once per frame, ShutdownStart - for savedata SIZES, LIST,
// AUTOSAVE, a dialog SAVE (confirmed with simulated button presses), AUTOLOAD and a dialog LOAD,
// checks the PSP-style save directory on the host, runs a message dialog and the OSK, and renders
// dialog frames through the software GE (libpspgpu) to PNGs.
//
//   build/test_utility [out-dir]      (default: build/utility_test; the "memory stick" is out-dir/ms0)
//
// GPL-2.0+ (links PPSSPP code), see LICENSE.TXT.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <zlib.h>

// The PPSSPP structure definitions, exactly as the dialogs read them.
#include "Core/Dialog/SavedataParam.h"
#include "Core/Dialog/PSPMsgDialog.h"
#include "Core/Dialog/PSPOskDialog.h"
#include "Core/HLE/sceUtility.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/HLE/ErrorCodes.h"
#include "utility_hle.h"
#include "../gpu/ge_soft.h"
#include "../audio/audio_hle.h"
#include "../mpeg/mpeg_hle.h"
#include "../audio/stubs/kernel_bridge.h"  // last: short macros

// ================================================================= minimal kernel stand-ins
u8 *g_mem;
int g_loglevel = 1;
Thread *g_cur;
bool g_in_intr;
char g_save_root[512];
char g_game_root[512];
rt_u64 g_vblank_count;
extern "C" u32 fe_buttons;
u32 fe_buttons;

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
// Virtual time: frames advance it by 1/60 s, HLE delays jump it forward.
static rt_u64 vclock = 1000000;
rt_u64 now_us(void) { return vclock; }
static u32 kheap = 0x08100000;
u32 kalloc(u32 size) {
	u32 a = (kheap + 15) & ~15u;
	kheap = a + size;
	memset(PTR(a), 0, size);
	return a;
}
u32 cur_module_gp(void) { return 0x08A80000; }

static std::vector<HleEnt> hle;
void hle_register(const HleEnt *ents, int n) { hle.insert(hle.end(), ents, ents + n); }

struct IntrReq { u32 handler; void (*done)(void *, u32); void *ud; };
static std::vector<IntrReq> intrq;
void intr_queue_ex(u32 handler, u32 a0, u32 a1, u32 a2, u32 gp, void (*done)(void *, u32), void *ud) {
	intrq.push_back({handler, done, ud});
}
static void run_interrupts() {
	for (size_t i = 0; i < intrq.size(); i++)
		if (intrq[i].done) intrq[i].done(intrq[i].ud, 0);
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
static int delays;
void thread_delay_until(rt_u64 when) {
	delays++;
	if (when > vclock) vclock = when;
	run_interrupts();
}
u32 run_guest(Ctx *tmpl, u32 addr, u32 a0, u32 a1, u32 a2) { fatal("unexpected guest call %08x", addr); }

// ms0:/ -> the test's memory stick directory (the runtime's io.c does the same with g_save_root).
int io_host_path(const char *psp, char *out, size_t outsz) {
	if (strncmp(psp, "ms0:", 4)) return -1;
	const char *rest = psp + 4;
	while (*rest == '/') rest++;
	snprintf(out, outsz, "%s/%s", g_save_root, rest);
	return 0;
}

// ================================================================= helpers
static Ctx ctx;
static u32 call(const char *name, u32 a0 = 0, u32 a1 = 0, u32 a2 = 0, u32 a3 = 0) {
	for (size_t i = hle.size(); i-- > 0;) {  // later registrations win, like the kernel's table
		const HleEnt &e = hle[i];
		if (!strcmp(e.name, name)) {
			Ctx *c = &ctx;
			A(0) = a0; A(1) = a1; A(2) = a2; A(3) = a3;
			c->r[29] = 0x09F00000;
			c->r[2] = 0xDEADBEEF;
			e.fn(c);
			return c->r[2];
		}
	}
	fatal("HLE %s not registered", name);
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static std::vector<u8> png_encode(const u8 *rgba, int w, int h, int stride_px, bool alpha) {
	std::vector<u8> raw, out;
	const int bpp = alpha ? 4 : 3;
	for (int y = 0; y < h; y++) {
		raw.push_back(0);
		const u8 *row = rgba + (size_t)y * stride_px * 4;
		for (int x = 0; x < w; x++)
			for (int k = 0; k < bpp; k++) raw.push_back(row[x * 4 + k]);
	}
	uLongf zlen = compressBound(raw.size());
	std::vector<u8> z(zlen);
	compress2(z.data(), &zlen, raw.data(), raw.size(), 9);
	auto be32 = [&](u32 v) { out.push_back(u8(v >> 24)); out.push_back(u8(v >> 16)); out.push_back(u8(v >> 8)); out.push_back(u8(v)); };
	auto chunk = [&](const char *type, const u8 *data, u32 len) {
		be32(len); out.insert(out.end(), type, type + 4); if (len) out.insert(out.end(), data, data + len);
		u32 crc = crc32(0, (const Bytef *)type, 4); crc = crc32(crc, data, len); be32(crc);
	};
	static const u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	out.insert(out.end(), sig, sig + 8);
	u8 ihdr[13] = {u8(w >> 24), u8(w >> 16), u8(w >> 8), u8(w), u8(h >> 24), u8(h >> 16), u8(h >> 8), u8(h), 8, u8(alpha ? 6 : 2), 0, 0, 0};
	chunk("IHDR", ihdr, 13);
	chunk("IDAT", z.data(), (u32)zlen);
	chunk("IEND", nullptr, 0);
	return out;
}
static bool write_file(const std::string &path, const std::vector<u8> &d) {
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return false;
	fwrite(d.data(), 1, d.size(), f);
	fclose(f);
	return true;
}
static std::vector<u8> read_file(const std::string &path) {
	std::vector<u8> d;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return d;
	fseek(f, 0, SEEK_END);
	d.resize(ftell(f));
	fseek(f, 0, SEEK_SET);
	if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
	fclose(f);
	return d;
}

// PARAM.SFO reader (for checking the files the save wrote).
static std::string sfo_string(const std::vector<u8> &sfo, const char *key) {
	if (sfo.size() < 20 || memcmp(sfo.data(), "\0PSF", 4)) return "<bad sfo>";
	auto rd32 = [&](size_t o) { return (u32)sfo[o] | sfo[o + 1] << 8 | sfo[o + 2] << 16 | (u32)sfo[o + 3] << 24; };
	u32 keyTable = rd32(8), dataTable = rd32(12), n = rd32(16);
	for (u32 i = 0; i < n; i++) {
		size_t e = 20 + i * 16;
		u32 keyOff = sfo[e] | sfo[e + 1] << 8, len = rd32(e + 4), dataOff = rd32(e + 12);
		if (!strcmp((const char *)&sfo[keyTable + keyOff], key)) {
			std::string v((const char *)&sfo[dataTable + dataOff], len);
			while (!v.empty() && v.back() == 0) v.pop_back();
			return v;
		}
	}
	return "<missing>";
}

// ================================================================= the "game"
static const u32 GAME_LIST = 0x08800000, GAME_VERTS = 0x08810000;
static const u32 PARAM = 0x08900000, DATA_BUF = 0x08910000, ICON_BUF = 0x08930000, LOAD_BUF = 0x08950000;
static const u32 MS_FREE = 0x08960000, MS_DATA = 0x08960100, USED_DATA = 0x08960200, ID_LIST = 0x08960300, ID_ENTRIES = 0x08960400;
static const u32 MSG_PARAM = 0x08970000, OSK_PARAM = 0x08971000, OSK_FIELD = 0x08972000, OSK_DESC = 0x08972100, OSK_IN = 0x08972200, OSK_OUT = 0x08972300;
static const u32 DATA_SIZE = 0x3000;

// A frame of the game's own drawing: framebuffer setup, a clear and two colored bars, like the
// menu behind a dialog. PPGe draws on top of whatever the game's lists left in the framebuffer.
static void build_game_list() {
	u32 pc = GAME_LIST;
	auto cmd = [&](u32 c, u32 d = 0) { SW(pc, (c << 24) | (d & 0xFFFFFF)); pc += 4; };
	auto vtx = [](u32 a, u32 color, int x, int y) { SW(a, color); SH(a + 4, x); SH(a + 6, y); SH(a + 8, 0); };
	vtx(GAME_VERTS + 0, 0xFF603818, 0, 0);     vtx(GAME_VERTS + 12, 0xFF603818, 480, 272);
	vtx(GAME_VERTS + 24, 0xFF2080E0, 0, 40);   vtx(GAME_VERTS + 36, 0xFF2080E0, 480, 70);
	vtx(GAME_VERTS + 48, 0xFF30C060, 0, 200);  vtx(GAME_VERTS + 60, 0xFF30C060, 480, 230);
	cmd(0x10, 0x080000);              // BASE
	cmd(0x9C, 0); cmd(0x9D, 512);     // FRAMEBUFPTR 0x04000000, width 512
	cmd(0xD2, 3);                     // FRAMEBUFPIXFORMAT 8888
	cmd(0x9E, 0x088000); cmd(0x9F, 512);
	cmd(0x15, 0); cmd(0x16, (271 << 10) | 479);
	cmd(0xD4, 0); cmd(0xD5, (271 << 10) | 479);
	cmd(0x1E, 0); cmd(0x21, 0); cmd(0x23, 0); cmd(0xE8, 0); cmd(0xE9, 0);
	cmd(0xD3, 1 | (7 << 8));          // CLEARMODE
	cmd(0x12, 0x80011C); cmd(0x01, GAME_VERTS & 0xFFFFFF); cmd(0x04, (6 << 16) | 2);
	cmd(0xD3, 0);
	cmd(0x01, (GAME_VERTS + 24) & 0xFFFFFF); cmd(0x04, (6 << 16) | 4);
	cmd(0x0F, 0); cmd(0x0C, 0);       // FINISH, END
}

static void game_frame() {
	u32 id = call("sceGeListEnQueue", GAME_LIST, 0, (u32)-1, 0);
	CHECK((int)id >= 0, "game list enqueue %08x", id);
	call("sceGeListSync", id, 0);
}

static void end_frame() {
	ge_soft_set_display(0x04000000, 512, 3);
	ge_soft_vblank();
	vclock += 16667;
	g_vblank_count++;
	run_interrupts();
}

static void save_frame_png(const std::string &path) {
	ge_soft_flush();
	write_file(path, png_encode(g_mem + 0x04000000, 480, 272, 512, false));
}

struct DialogApi { const char *init, *status, *update, *shutdown; };
static const DialogApi SAVEDATA = {"sceUtilitySavedataInitStart", "sceUtilitySavedataGetStatus", "sceUtilitySavedataUpdate", "sceUtilitySavedataShutdownStart"};
static const DialogApi MSG = {"sceUtilityMsgDialogInitStart", "sceUtilityMsgDialogGetStatus", "sceUtilityMsgDialogUpdate", "sceUtilityMsgDialogShutdownStart"};
static const DialogApi OSK = {"sceUtilityOskInitStart", "sceUtilityOskGetStatus", "sceUtilityOskUpdate", "sceUtilityOskShutdownStart"};

struct DialogRun {
	int initResult = 0;
	int frames = 0;          // frames until the status went back to NONE
	int updates = 0;         // Update calls
	std::string statuses;    // sequence of distinct statuses seen, e.g. "12340"
};

// The usual game loop around a utility dialog. input(updateIndex) gives the buttons held during that
// Update; onUpdate runs after each Update (e.g. to capture the frame).
static DialogRun run_dialog(const DialogApi &api, u32 param, std::function<u32(int)> input = nullptr,
		std::function<void(int)> onUpdate = nullptr, int maxFrames = 3000) {
	DialogRun r;
	r.initResult = (int)call(api.init, param);
	if (r.initResult < 0) return r;
	int last = -1;
	bool shutdownSent = false;
	for (r.frames = 0; r.frames < maxFrames; r.frames++) {
		game_frame();
		int st = (int)call(api.status);
		if (st != last) { r.statuses += (char)('0' + st); last = st; }
		if (st == 2) {
			fe_buttons = input ? input(r.updates) : 0;
			int u = (int)call(api.update, 1);
			CHECK(u >= 0, "%s returned %08x", api.update, u);
			if (onUpdate) onUpdate(r.updates);
			r.updates++;
		} else if (st == 3 && !shutdownSent) {
			call(api.shutdown);
			shutdownSent = true;
		} else if (st == 0 && shutdownSent) {
			break;
		} else if (st < 0) {
			fprintf(stderr, "%s returned %08x\n", api.status, st);
			break;
		}
		end_frame();
	}
	fe_buttons = 0;
	return r;
}

// Confirm (cross) at Update 45, then "Back" (circle) every 45 Updates to close the result screen.
static u32 confirm_then_back(int i) {
	return i == 45 ? (u32)CTRL_CROSS : (i > 45 && i % 45 == 0) ? (u32)CTRL_CIRCLE : 0u;
}

// Pulses `button` for one Update every `period` Updates, starting at `first`.
static std::function<u32(int)> press_every(u32 button, int first, int period) {
	return [=](int i) { return (i >= first && (i - first) % period == 0) ? button : 0u; };
}

static SceUtilitySavedataParam *P() { return (SceUtilitySavedataParam *)PTR(PARAM); }

static void init_savedata_param(int mode, const char *saveName) {
	memset(P(), 0, sizeof(SceUtilitySavedataParam));
	SceUtilitySavedataParam &p = *P();
	p.common.size = sizeof(SceUtilitySavedataParam);
	p.common.language = 1;
	p.common.buttonSwap = 1;           // cross confirms
	p.common.graphicsThread = 0x11;
	p.common.accessThread = 0x13;
	p.common.fontThread = 0x12;
	p.common.soundThread = 0x10;
	p.mode = (SceUtilitySavedataType)mode;
	p.overwriteMode = 1;
	strcpy(p.gameName, "UCES00420");
	strcpy(p.saveName, saveName);
	strcpy(p.fileName, "DATA.BIN");
	p.focus = (SceUtilitySavedataFocus)0;
	// Like most games: a game-specific key (a full-size request without one is refused on hardware).
	for (int i = 0; i < 16; i++) p.key[i] = (u8)(0x40 + i * 3);
}

static void set_save_data(u32 seed) {
	SceUtilitySavedataParam &p = *P();
	for (u32 i = 0; i < DATA_SIZE; i++) PTR(DATA_BUF)[i] = (u8)(i * 7 + seed + (i >> 8));
	p.dataBuf.ptr = DATA_BUF;
	p.dataBufSize = DATA_SIZE;
	p.dataSize = DATA_SIZE;
	strcpy(p.sfoParam.title, "Rock Star Saga");
	snprintf(p.sfoParam.savedataTitle, sizeof(p.sfoParam.savedataTitle), "Story Save %u", seed);
	strcpy(p.sfoParam.detail, "Chapter 3 - The Tour\nPlay time 12:34");
	p.sfoParam.parentalLevel = 1;
}

static std::vector<u8> g_iconPng;
static void set_icon() {
	// 144x80 ICON0: a diagonal gradient with a white frame.
	std::vector<u8> img(144 * 80 * 4);
	for (int y = 0; y < 80; y++)
		for (int x = 0; x < 144; x++) {
			u8 *px = &img[(y * 144 + x) * 4];
			bool frame = x < 4 || y < 4 || x >= 140 || y >= 76;
			px[0] = frame ? 255 : (u8)(x * 255 / 143);
			px[1] = frame ? 255 : (u8)(64 + y * 2);
			px[2] = frame ? 255 : (u8)(255 - x * 255 / 143);
			px[3] = 255;
		}
	g_iconPng = png_encode(img.data(), 144, 80, 144, true);
	memcpy(PTR(ICON_BUF), g_iconPng.data(), g_iconPng.size());
	P()->icon0FileData.buf.ptr = ICON_BUF;
	P()->icon0FileData.bufSize = (u32)g_iconPng.size();
	P()->icon0FileData.size = (u32)g_iconPng.size();
}

static bool check_load(const char *what, u32 seed) {
	bool ok = true;
	for (u32 i = 0; i < DATA_SIZE; i++)
		if (PTR(LOAD_BUF)[i] != (u8)(i * 7 + seed + (i >> 8))) { ok = false; break; }
	CHECK(ok, "%s: loaded data differs", what);
	return ok;
}

// ================================================================= the test
int main(int argc, char **argv) {
	std::string out = argc > 1 ? argv[1] : "utility_test";
	void *m = mmap(nullptr, 0x40000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (m == MAP_FAILED) fatal("mmap failed");
	g_mem = (u8 *)m;
	if (system(("rm -rf '" + out + "/ms0' && mkdir -p '" + out + "/ms0'").c_str()) != 0) fatal("can't create %s", out.c_str());
	snprintf(g_save_root, sizeof(g_save_root), "%s/ms0", out.c_str());

	static Thread mainThread;
	strcpy(mainThread.name, "MEMCARD");
	mainThread.status = TS_RUNNING;
	mainThread.prio = 0x30;
	g_cur = &mainThread;

	// The runtime's order: GE, audio, movies, then the utility (all four libraries in one binary).
	ge_soft_init();
	audio_hle_init();
	mpeg_hle_init();
	const size_t before = hle.size();
	utility_hle_init();
	CHECK(hle.size() - before >= 100, "utility registered %zu functions", hle.size() - before);
	printf("HLE functions registered: %zu (utility %zu)\n", hle.size(), hle.size() - before);
	build_game_list();
	const std::string saveDir0 = std::string(g_save_root) + "/PSP/SAVEDATA/UCES004200000";
	const std::string saveDir1 = std::string(g_save_root) + "/PSP/SAVEDATA/UCES004200001";

	// --- system params and modules
	call("sceUtilityGetSystemParamInt", PSP_SYSTEMPARAM_ID_INT_LANGUAGE, 0x08980000);
	CHECK(LW(0x08980000) == 1, "language %u", LW(0x08980000));
	call("sceUtilityGetSystemParamInt", PSP_SYSTEMPARAM_ID_INT_BUTTON_PREFERENCE, 0x08980000);
	CHECK(LW(0x08980000) == 1, "button preference %u", LW(0x08980000));
	CHECK(call("sceUtilityGetSystemParamString", PSP_SYSTEMPARAM_ID_STRING_NICKNAME, 0x08980010, 128) == 0 &&
		!strcmp((const char *)PTR(0x08980010), "PLAYER"), "nickname '%s'", (const char *)PTR(0x08980010));
	CHECK(call("sceUtilityLoadAvModule", 0) == 0, "LoadAvModule");
	CHECK(call("sceUtilityLoadModule", 0x300) == 0, "LoadModule(0x300)");
	CHECK(call("sceUtilityLoadNetModule", 1) == 0, "LoadNetModule");
	CHECK(call("sceUtilityUnloadModule", 0x300) == 0, "UnloadModule");
	CHECK(call("sceUtilityUnloadAvModule", 0) == 0, "UnloadAvModule");

	// --- SIZES (8) at boot, before any save exists
	init_savedata_param(SCE_UTILITY_SAVEDATA_TYPE_SIZES, "0000");
	set_save_data(1);
	P()->msFree.ptr = MS_FREE;
	P()->msData.ptr = MS_DATA;
	P()->utilityData.ptr = USED_DATA;
	auto *msData = (SceUtilitySavedataMsDataInfo *)PTR(MS_DATA);
	strcpy(msData->gameName, "UCES00420");
	strcpy(msData->saveName, "0000");
	DialogRun r = run_dialog(SAVEDATA, PARAM);
	auto *msFree = (SceUtilitySavedataMsFreeInfo *)PTR(MS_FREE);
	auto *used = (SceUtilitySavedataUsedDataInfo *)PTR(USED_DATA);
	printf("SIZES: init %d, statuses %s, %d frames, result %08x; free %d clusters of %d (%s), needs %d KB\n",
		r.initResult, r.statuses.c_str(), r.frames, (u32)P()->common.result, (int)msFree->freeClusters,
		(int)msFree->clusterSize, msFree->freeSpaceStr, (int)used->usedSpaceKB);
	CHECK(r.initResult == 0 && r.statuses.find('0', 1) != std::string::npos, "SIZES lifecycle %d %s", r.initResult, r.statuses.c_str());
	CHECK((u32)P()->common.result == SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_DATA, "SIZES result %08x (expected NO_DATA before saving)", (u32)P()->common.result);
	CHECK(msFree->clusterSize == 32768 && msFree->freeClusters > 0, "msFree");
	CHECK(used->usedSpaceKB >= 32 * 3, "utilityData usedSpaceKB %d", (int)used->usedSpaceKB);

	// --- LIST (11) with nothing saved
	auto listRun = [&](const char *pattern) {
		init_savedata_param(SCE_UTILITY_SAVEDATA_TYPE_LIST, pattern);
		P()->idList.ptr = ID_LIST;
		SW(ID_LIST, 8); SW(ID_LIST + 4, 0xFFFF); SW(ID_LIST + 8, ID_ENTRIES);
		DialogRun lr = run_dialog(SAVEDATA, PARAM);
		CHECK(lr.initResult == 0 && P()->common.result == 0, "LIST init %d result %08x", lr.initResult, (u32)P()->common.result);
		return (int)LW(ID_LIST + 4);
	};
	int n = listRun("*");
	printf("LIST (empty): %d entries\n", n);
	CHECK(n == 0, "LIST should find nothing yet (%d)", n);

	// --- AUTOSAVE (1): no UI
	init_savedata_param(SCE_UTILITY_SAVEDATA_TYPE_AUTOSAVE, "0000");
	set_save_data(1);
	set_icon();
	r = run_dialog(SAVEDATA, PARAM);
	printf("AUTOSAVE: init %d, statuses %s, %d frames, %d updates, result %08x\n", r.initResult, r.statuses.c_str(), r.frames, r.updates, (u32)P()->common.result);
	CHECK(r.initResult == 0 && P()->common.result == 0, "AUTOSAVE result %08x", (u32)P()->common.result);
	std::vector<u8> data = read_file(saveDir0 + "/DATA.BIN");
	std::vector<u8> sfo = read_file(saveDir0 + "/PARAM.SFO");
	std::vector<u8> icon = read_file(saveDir0 + "/ICON0.PNG");
	CHECK(data.size() == DATA_SIZE && !memcmp(data.data(), PTR(DATA_BUF), DATA_SIZE), "DATA.BIN on host is not the plain save data (%zu bytes)", data.size());
	CHECK(icon == g_iconPng, "ICON0.PNG differs (%zu bytes)", icon.size());
	printf("  host %s: DATA.BIN %zu bytes, ICON0.PNG %zu bytes, PARAM.SFO %zu bytes: TITLE='%s' SAVEDATA_TITLE='%s' SAVEDATA_DIRECTORY='%s'\n",
		saveDir0.c_str(), data.size(), icon.size(), sfo.size(), sfo_string(sfo, "TITLE").c_str(), sfo_string(sfo, "SAVEDATA_TITLE").c_str(),
		sfo_string(sfo, "SAVEDATA_DIRECTORY").c_str());
	CHECK(sfo_string(sfo, "TITLE") == "Rock Star Saga" && sfo_string(sfo, "SAVEDATA_TITLE") == "Story Save 1" &&
		sfo_string(sfo, "SAVEDATA_DIRECTORY") == "UCES004200000" && sfo_string(sfo, "CATEGORY") == "MS", "PARAM.SFO contents");

	// --- SAVE (3): the confirmation dialog, drawn by PPGe; "press" cross to confirm and to close.
	init_savedata_param(SCE_UTILITY_SAVEDATA_TYPE_SAVE, "0001");
	set_save_data(2);
	set_icon();
	const std::string savePng = out + "/utility_save_dialog.png";
	r = run_dialog(SAVEDATA, PARAM, confirm_then_back, [&](int i) {
		if (i == 40) save_frame_png(savePng);
		if (i == 50) save_frame_png(out + "/utility_save_progress.png");
	});
	printf("SAVE dialog: init %d, statuses %s, %d frames, %d updates, result %08x\n", r.initResult, r.statuses.c_str(), r.frames, r.updates, (u32)P()->common.result);
	CHECK(r.initResult == 0 && P()->common.result == 0 && r.updates > 45, "SAVE dialog result %08x after %d updates", (u32)P()->common.result, r.updates);
	data = read_file(saveDir1 + "/DATA.BIN");
	CHECK(data.size() == DATA_SIZE && !memcmp(data.data(), PTR(DATA_BUF), DATA_SIZE), "dialog SAVE DATA.BIN (%zu bytes)", data.size());
	sfo = read_file(saveDir1 + "/PARAM.SFO");
	CHECK(sfo_string(sfo, "SAVEDATA_TITLE") == "Story Save 2", "dialog SAVE PARAM.SFO '%s'", sfo_string(sfo, "SAVEDATA_TITLE").c_str());

	// --- SIZES and LIST again
	init_savedata_param(SCE_UTILITY_SAVEDATA_TYPE_SIZES, "0000");
	P()->msData.ptr = MS_DATA;
	strcpy(msData->gameName, "UCES00420");
	strcpy(msData->saveName, "0000");
	run_dialog(SAVEDATA, PARAM);
	printf("SIZES (after save): result %08x, save 0000 uses %d KB (%s)\n", (u32)P()->common.result, (int)msData->info.usedSpaceKB, msData->info.usedSpaceStr);
	CHECK(P()->common.result == 0 && msData->info.usedSpaceKB > 0, "SIZES after save");
	n = listRun("*");
	printf("LIST: %d entries:", n);
	for (int i = 0; i < n; i++) printf(" %s", (const char *)PTR(ID_ENTRIES + i * sizeof(SceUtilitySavedataIdListEntry) + offsetof(SceUtilitySavedataIdListEntry, name)));
	printf("\n");
	CHECK(n == 2, "LIST should find both saves (%d)", n);

	// --- AUTOLOAD (0) of the autosave
	init_savedata_param(SCE_UTILITY_SAVEDATA_TYPE_AUTOLOAD, "0000");
	memset(PTR(LOAD_BUF), 0, DATA_SIZE);
	P()->dataBuf.ptr = LOAD_BUF;
	P()->dataBufSize = DATA_SIZE;
	r = run_dialog(SAVEDATA, PARAM);
	printf("AUTOLOAD: result %08x, %u bytes, title '%s'\n", (u32)P()->common.result, (u32)P()->dataSize, P()->sfoParam.savedataTitle);
	CHECK(P()->common.result == 0 && P()->dataSize == DATA_SIZE, "AUTOLOAD result %08x size %u", (u32)P()->common.result, (u32)P()->dataSize);
	check_load("AUTOLOAD", 1);

	// --- LOAD (2) through the dialog, of the dialog save
	init_savedata_param(SCE_UTILITY_SAVEDATA_TYPE_LOAD, "0001");
	memset(PTR(LOAD_BUF), 0, DATA_SIZE);
	P()->dataBuf.ptr = LOAD_BUF;
	P()->dataBufSize = DATA_SIZE;
	r = run_dialog(SAVEDATA, PARAM, confirm_then_back, [&](int i) {
		if (i == 40) save_frame_png(out + "/utility_load_dialog.png");
	});
	printf("LOAD dialog: statuses %s, %d updates, result %08x, %u bytes\n", r.statuses.c_str(), r.updates, (u32)P()->common.result, (u32)P()->dataSize);
	CHECK(P()->common.result == 0 && P()->dataSize == DATA_SIZE, "LOAD dialog result %08x", (u32)P()->common.result);
	check_load("LOAD dialog", 2);

	// --- AUTOLOAD of a missing save
	init_savedata_param(SCE_UTILITY_SAVEDATA_TYPE_AUTOLOAD, "0009");
	P()->dataBuf.ptr = LOAD_BUF;
	P()->dataBufSize = DATA_SIZE;
	run_dialog(SAVEDATA, PARAM);
	printf("AUTOLOAD (missing): result %08x\n", (u32)P()->common.result);
	CHECK((u32)P()->common.result == SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA, "missing save result %08x", (u32)P()->common.result);

	// --- message dialog (yes/no), "press" cross on Yes
	auto *msg = (pspMessageDialog *)PTR(MSG_PARAM);
	memset(msg, 0, sizeof(*msg));
	msg->common.size = SCE_UTILITY_MSGDIALOG_SIZE_V3;
	msg->common.language = 1;
	msg->common.buttonSwap = 1;
	msg->common.graphicsThread = 0x11; msg->common.accessThread = 0x13; msg->common.fontThread = 0x12; msg->common.soundThread = 0x10;
	msg->type = 1;  // text
	msg->options = SCE_UTILITY_MSGDIALOG_OPTION_YESNO | SCE_UTILITY_MSGDIALOG_OPTION_TEXTSOUND;
	strcpy(msg->string, "Overwrite the tour schedule?");
	r = run_dialog(MSG, MSG_PARAM, press_every(CTRL_CROSS, 45, 45), [&](int i) {
		if (i == 40) save_frame_png(out + "/utility_msg_dialog.png");
	});
	printf("MSG dialog: statuses %s, %d updates, result %d, button %u\n", r.statuses.c_str(), r.updates, (int)msg->common.result, (u32)msg->buttonPressed);
	CHECK(r.initResult == 0 && msg->common.result == 0 && msg->buttonPressed == 1, "msg dialog result %d button %u", (int)msg->common.result, (u32)msg->buttonPressed);

	// --- OSK: initial text "HERO", cross adds the selected character, start accepts.
	auto *osk = (SceUtilityOskParams *)PTR(OSK_PARAM);
	auto *field = (SceUtilityOskData *)PTR(OSK_FIELD);
	memset(osk, 0, sizeof(*osk));
	memset(field, 0, sizeof(*field));
	osk->base.size = sizeof(SceUtilityOskParams);
	osk->base.language = 1;
	osk->base.buttonSwap = 1;
	osk->base.graphicsThread = 0x11; osk->base.accessThread = 0x13; osk->base.fontThread = 0x12; osk->base.soundThread = 0x10;
	osk->fieldCount = 1;
	osk->fields.ptr = OSK_FIELD;
	const char *desc = "Band name", *in = "HERO";
	for (int i = 0; i <= (int)strlen(desc); i++) SH(OSK_DESC + i * 2, desc[i]);
	for (int i = 0; i <= (int)strlen(in); i++) SH(OSK_IN + i * 2, in[i]);
	field->language = (SceUtilityOskInputLanguage)0;
	field->lines = 1;
	field->desc.ptr = OSK_DESC;
	field->intext.ptr = OSK_IN;
	field->outtextlength = 16;
	field->outtextlimit = 12;
	field->outtext.ptr = OSK_OUT;
	r = run_dialog(OSK, OSK_PARAM, [](int i) { return i == 30 ? (u32)CTRL_CROSS : i == 60 ? (u32)CTRL_START : 0u; }, [&](int i) {
		if (i == 40) save_frame_png(out + "/utility_osk.png");
	});
	std::string oskOut;
	for (int i = 0; i < 16 && LH(OSK_OUT + i * 2); i++) oskOut += (char)LH(OSK_OUT + i * 2);
	printf("OSK: statuses %s, %d updates, result %d, text '%s'\n", r.statuses.c_str(), r.updates, (int)field->result, oskOut.c_str());
	CHECK(r.initResult == 0 && oskOut.size() == 5 && oskOut.compare(0, 4, "HERO") == 0, "OSK output '%s'", oskOut.c_str());

	struct stat st;
	CHECK(stat(savePng.c_str(), &st) == 0 && st.st_size > 1000, "no dialog PNG");
	printf("%s: dialog frame %s (also utility_save_progress/load_dialog/msg_dialog/osk.png), %d HLE delays\n",
		failures ? "FAILED" : "OK", savePng.c_str(), delays);
	return failures ? 1 : 0;
}
