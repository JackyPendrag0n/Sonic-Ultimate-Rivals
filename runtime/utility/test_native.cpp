// Checks the runtime's own system dialogs and save data (runtime/sysdlg.c, runtime/savedata.c) against
// PPSSPP's (this directory): the same requests and button presses go to both, and their results, statuses,
// request memory and files are compared. Then saves cross over: each side loads the other's encrypted
// saves, the encryption and PARAM.SFO hashes are checked against PPSSPP's own functions, and an
// unencrypted save from older runtime builds loads.
//
//   build/test_native [out-dir]              (built by runtime/utility/build.sh; default build/utility_native)
//
// GPL-2.0+ (links PPSSPP code), see LICENSE.TXT.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>

#include "Common/CommonTypes.h"
#include "Core/Config.h"
#include "Common/Swap.h"
#include "Core/HLE/sceChnnlsv.h"
#include "Core/Dialog/PSPMsgDialog.h"
#include "Core/HLE/sceCtrl.h"
#define private public   // the test calls SavedataParam's own encryption
#include "Core/Dialog/SavedataParam.h"
#undef private
#include "utility_hle.h"
#include "../gpu/ge_soft.h"
#include "../audio/audio_hle.h"
#include "../audio/stubs/kernel_bridge.h"  // last: short macros

extern "C" {
#include "../savedata.h"
void sysdlg_init(void);
}

// ================================================================= minimal kernel stand-ins
u8 *g_mem;
int g_loglevel = 0;
Thread *g_cur;
bool g_in_intr;
char g_save_root[512];
char g_game_root[512];
rt_u64 g_vblank_count;
extern "C" u32 fe_buttons;
u32 fe_buttons;
extern "C" u32 g_sdk_version;
u32 g_sdk_version = 0x06060010;   // what PPSSPP's library answers for sceKernelGetCompiledSdkVersion
extern "C" void net_adhoc_connected(void) {}

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
static rt_u64 vclock = 1000000;   // frames advance it by 1/60 s, HLE delays jump it forward
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
void intr_queue_ex(u32 handler, u32 a0, u32 a1, u32 a2, u32 gp, void (*done)(void *, u32), void *ud) { intrq.push_back({handler, done, ud}); }
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
void thread_delay_until(rt_u64 when) {
	if (when > vclock) vclock = when;
	run_interrupts();
}
u32 run_guest(Ctx *tmpl, u32 addr, u32 a0, u32 a1, u32 a2) { fatal("unexpected guest call %08x", addr); }

int io_host_path(const char *psp, char *out, size_t outsz) {
	if (strncmp(psp, "ms0:", 4)) return -1;
	const char *rest = psp + 4;
	while (*rest == '/') rest++;
	snprintf(out, outsz, "%s/%s", g_save_root, rest);
	return 0;
}

// ================================================================= the two implementations
static std::vector<HleEnt> tab[2];   // 0: PPSSPP, 1: native
static const char *const impl_name[2] = {"PPSSPP", "native"};
static Ctx ctx;
static u32 call(int impl, const char *name, u32 a0 = 0, u32 a1 = 0, u32 a2 = 0) {
	for (size_t i = tab[impl].size(); i-- > 0;) {
		const HleEnt &e = tab[impl][i];
		if (!strcmp(e.name, name)) {
			Ctx *c = &ctx;
			A(0) = a0; A(1) = a1; A(2) = a2;
			c->r[2] = 0xDEADBEEF;
			e.fn(c);
			return c->r[2];
		}
	}
	fatal("%s: %s not registered", impl_name[impl], name);
}

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

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
static bool exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }

// PARAM.SFO as key -> (format, bytes)
static std::map<std::string, std::pair<int, std::vector<u8>>> sfo_entries(const std::vector<u8> &sfo) {
	std::map<std::string, std::pair<int, std::vector<u8>>> m;
	if (sfo.size() < 20 || memcmp(sfo.data(), "\0PSF", 4)) return m;
	auto rd32 = [&](size_t o) { return (u32)sfo[o] | sfo[o + 1] << 8 | sfo[o + 2] << 16 | (u32)sfo[o + 3] << 24; };
	u32 kt = rd32(8), dt = rd32(12), n = rd32(16);
	for (u32 i = 0; i < n; i++) {
		size_t e = 20 + i * 16;
		u32 ko = sfo[e] | sfo[e + 1] << 8, fmt = sfo[e + 2] | sfo[e + 3] << 8, len = rd32(e + 4), max = rd32(e + 8), dof = rd32(e + 12);
		std::vector<u8> v(sfo.begin() + dt + dof, sfo.begin() + dt + dof + max);
		v.resize(std::max(len, max));
		m[(const char *)&sfo[kt + ko]] = {fmt | (int)len << 16, v};
	}
	return m;
}
static int sfo_offset(const std::vector<u8> &sfo, const char *key) {   // where a value's data starts
	auto rd32 = [&](size_t o) { return (u32)sfo[o] | sfo[o + 1] << 8 | sfo[o + 2] << 16 | (u32)sfo[o + 3] << 24; };
	u32 kt = rd32(8), dt = rd32(12), n = rd32(16);
	for (u32 i = 0; i < n; i++)
		if (!strcmp((const char *)&sfo[kt + (sfo[20 + i * 16] | sfo[21 + i * 16] << 8)], key)) return dt + rd32(20 + i * 16 + 12);
	return -1;
}

// ================================================================= the "game"
static const u32 PARAM = 0x08900000, DATA_BUF = 0x08910000, ICON_BUF = 0x08930000, LOAD_BUF = 0x08950000;
static const u32 MS_FREE = 0x08960000, MS_DATA = 0x08960100, USED_DATA = 0x08960200, NAME_LIST = 0x08960300;
static const u32 MSG_PARAM = 0x08970000, NET_PARAM = 0x08971000, SHARE_PARAM = 0x08972000, OUT = 0x08980000;
static const u32 DATA_SIZE = 5688;   // Sonic Rivals 2's GAMEDATA.BIN

static void end_frame() {
	ge_soft_set_display(0x04000000, 512, 3);
	ge_soft_vblank();
	vclock += 16667;
	g_vblank_count++;
	run_interrupts();
}

struct Api { const char *init, *status, *update, *shutdown; };
static const Api SAVEDATA = {"sceUtilitySavedataInitStart", "sceUtilitySavedataGetStatus", "sceUtilitySavedataUpdate", "sceUtilitySavedataShutdownStart"};
static const Api MSG = {"sceUtilityMsgDialogInitStart", "sceUtilityMsgDialogGetStatus", "sceUtilityMsgDialogUpdate", "sceUtilityMsgDialogShutdownStart"};
static const Api NET = {"sceUtilityNetconfInitStart", "sceUtilityNetconfGetStatus", "sceUtilityNetconfUpdate", "sceUtilityNetconfShutdownStart"};
static const Api SHARE = {"sceUtilityGameSharingInitStart", "sceUtilityGameSharingGetStatus", "sceUtilityGameSharingUpdate", "sceUtilityGameSharingShutdownStart"};

struct Run {
	int init = 0, updates = 0, frames = 0;
	std::string statuses;
};

// The game loop around a dialog; input(update) is what's held during that Update. A dialog the game
// presses through ends when it's fading out, so input stops being looked at once it returns 0 for good.
static Run finish(int impl, const Api &api, std::function<u32(int)> input = nullptr);
static Run run(int impl, const Api &api, u32 param, std::function<u32(int)> input = nullptr) {
	Run r;
	r.init = (int)call(impl, api.init, param);
	if (r.init < 0) return r;
	return finish(impl, api, input);
}
// Runs a started dialog to its end
static Run finish(int impl, const Api &api, std::function<u32(int)> input) {
	Run r;
	int last = -1;
	bool shut = false;
	for (r.frames = 0; r.frames < 4000; r.frames++) {
		int st = (int)call(impl, api.status);
		if (st != last) { r.statuses += (char)('0' + st); last = st; }
		if (st == 2) {
			fe_buttons = input ? input(r.updates) : 0;
			int u = (int)call(impl, api.update, 1);
			CHECK(u >= 0, "%s: %s returned %08x", impl_name[impl], api.update, u);
			r.updates++;
		} else if (st == 3 && !shut) {
			CHECK(call(impl, api.shutdown) == 0, "%s shutdown", impl_name[impl]);
			shut = true;
		} else if (st == 0 && shut) break;
		else if (st < 0) { fprintf(stderr, "%s: %s returned %08x\n", impl_name[impl], api.status, st); break; }
		end_frame();
	}
	fe_buttons = 0;
	return r;
}

// Button presses at given Updates (one Update each)
static std::function<u32(int)> presses(std::vector<std::pair<int, u32>> at) {
	return [at](int i) { for (auto &p : at) if (p.first == i) return p.second; return 0u; };
}

static SceUtilitySavedataParam *P() { return (SceUtilitySavedataParam *)PTR(PARAM); }

static void request(int mode, const char *saveName, u32 seed = 0) {
	memset(P(), 0, sizeof(SceUtilitySavedataParam));
	SceUtilitySavedataParam &p = *P();
	p.common.size = 1536;
	p.common.language = 1;
	p.common.buttonSwap = 1;
	p.common.graphicsThread = 0x11; p.common.accessThread = 0x13; p.common.fontThread = 0x12; p.common.soundThread = 0x10;
	p.mode = (SceUtilitySavedataType)mode;
	strcpy(p.gameName, "ULUS10323");
	strcpy(p.saveName, saveName);
	strcpy(p.fileName, "GAMEDATA.BIN");
	memcpy(p.key, "RIVALS_2Z\0\0\0\0\0\0\0", 16);   // the game's key
	if (seed) {
		for (u32 i = 0; i < DATA_SIZE; i++) PTR(DATA_BUF)[i] = (u8)(i * 7 + seed + (i >> 8));
		p.dataBuf.ptr = DATA_BUF;
		p.dataBufSize = 0x2000;
		p.dataSize = DATA_SIZE;
		strcpy(p.sfoParam.title, "Sonic Rivals 2");
		snprintf(p.sfoParam.savedataTitle, sizeof p.sfoParam.savedataTitle, "Summary %u", seed);
		strcpy(p.sfoParam.detail, "Sonic: 0% / Tails: 0%\nShadow: 0% / Metal Sonic: 0%");
		p.sfoParam.parentalLevel = 3;
		static const u8 png[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 1, 2, 3, 4, 5, 6, 7, 8, 9};
		memcpy(PTR(ICON_BUF), png, sizeof png);
		p.icon0FileData.buf.ptr = ICON_BUF;
		p.icon0FileData.bufSize = 0x1000;
		p.icon0FileData.size = sizeof png;
	} else {
		memset(PTR(LOAD_BUF), 0, 0x2000);
		p.dataBuf.ptr = LOAD_BUF;
		p.dataBufSize = 0x2000;
	}
}
static void name_list(std::initializer_list<const char *> names) {
	memset(PTR(NAME_LIST), 0, 21 * 20);
	int i = 0;
	for (const char *n : names) strcpy((char *)PTR(NAME_LIST + 20 * i++), n);
	P()->saveNameList.ptr = NAME_LIST;
}
static bool loaded(u32 seed) {
	for (u32 i = 0; i < DATA_SIZE; i++)
		if (PTR(LOAD_BUF)[i] != (u8)(i * 7 + seed + (i >> 8))) return false;
	return true;
}

static std::string root;
static void use_stick(const std::string &name) {
	snprintf(g_save_root, sizeof g_save_root, "%s/%s", root.c_str(), name.c_str());
	if (system(("mkdir -p '" + std::string(g_save_root) + "/PSP/SAVEDATA'").c_str())) fatal("mkdir");
}
static std::string save_dir(const char *name) { return std::string(g_save_root) + "/PSP/SAVEDATA/ULUS10323" + name; }

// Runs the same scenario on both, each on its own memory stick and request memory, and compares
static std::vector<u8> mem_after[2];
static void both(const char *what, std::function<Run(int)> scenario, bool same_updates = true, size_t compare = 1536) {
	Run r[2];
	for (int k = 0; k < 2; k++) {
		use_stick(impl_name[k]);
		r[k] = scenario(k);
		mem_after[k].assign(PTR(PARAM), PTR(PARAM) + compare);
	}
	printf("%-34s PPSSPP: init %08x statuses %s, %3d updates | native: init %08x statuses %s, %3d updates\n", what,
	       r[0].init, r[0].statuses.c_str(), r[0].updates, r[1].init, r[1].statuses.c_str(), r[1].updates);
	CHECK(r[0].init == r[1].init && r[0].statuses == r[1].statuses, "%s: init/statuses differ", what);
	if (same_updates) CHECK(r[0].updates == r[1].updates, "%s: %d vs %d updates", what, r[0].updates, r[1].updates);
	for (size_t i = 0; i < compare; i++)
		if (mem_after[0][i] != mem_after[1][i]) {
			CHECK(false, "%s: request differs at +%zu: %02x vs %02x", what, i, mem_after[0][i], mem_after[1][i]);
			break;
		}
}

// Both saves on their sticks: same files, same PARAM.SFO apart from the hashes (the IV is random)
static void same_save(const char *what, const char *name) {
	std::string d[2], f[2];
	for (int k = 0; k < 2; k++) d[k] = root + "/" + impl_name[k] + "/PSP/SAVEDATA/ULUS10323" + name;
	for (const char *file : {"GAMEDATA.BIN", "ICON0.PNG", "PIC1.PNG"}) {
		auto a = read_file(d[0] + "/" + file), b = read_file(d[1] + "/" + file);
		CHECK(exists(d[0] + "/" + file) == exists(d[1] + "/" + file) && a.size() == b.size(), "%s: %s %zu vs %zu bytes", what, file, a.size(), b.size());
	}
	auto s0 = sfo_entries(read_file(d[0] + "/PARAM.SFO")), s1 = sfo_entries(read_file(d[1] + "/PARAM.SFO"));
	CHECK(!s0.empty() && s0.size() == s1.size(), "%s: PARAM.SFO has %zu vs %zu entries", what, s0.size(), s1.size());
	for (auto &e : s0) {
		if (e.first == "SAVEDATA_FILE_LIST" || e.first == "SAVEDATA_PARAMS") continue;
		CHECK(s1.count(e.first) && s1[e.first] == e.second, "%s: PARAM.SFO %s differs", what, e.first.c_str());
	}
	CHECK(s1.count("SAVEDATA_PARAMS") && s0["SAVEDATA_PARAMS"].second[0] == s1["SAVEDATA_PARAMS"].second[0],
	      "%s: SAVEDATA_PARAMS flags %02x vs %02x", what, s0["SAVEDATA_PARAMS"].second[0], s1["SAVEDATA_PARAMS"].second[0]);
}

// ================================================================= the tests
static void test_crypto() {
	SavedataParam pp;
	const u8 *key = (const u8 *)"RIVALS_2Z\0\0\0\0\0\0\0";
	int sizes[] = {1, 15, 16, 17, 2032, 2047, 2048, 2049, 4096, 5688, 9000};
	int ok = 0, total = 0;
	for (int mode : {1, 3, 5})
		for (int size : sizes) {
			std::vector<u8> plain(size);
			for (int i = 0; i < size; i++) plain[i] = (u8)(i * 13 + mode + size);
			// native encrypts, PPSSPP decrypts with the hash
			std::vector<u8> buf(size + 64);
			memcpy(buf.data(), plain.data(), size);
			int len = size, al = (size + 15) & ~15;
			u8 hash[16];
			const u8 *k = mode > 1 ? key : nullptr;
			bool good = sd_encrypt(mode, buf.data(), &len, &al, hash, k) == 0;
			int len2 = len, al2 = (len + 15) & ~15;
			std::vector<u8> b2(buf.begin(), buf.begin() + len);
			b2.resize(al2 + 32);
			good &= pp.DecryptData(mode, b2.data(), &len2, &al2, k, hash) == 0 && len2 == size && !memcmp(b2.data(), plain.data(), size);
			// PPSSPP encrypts, native decrypts with the hash
			std::vector<u8> b3(size + 64);
			memcpy(b3.data(), plain.data(), size);
			len = size; al = (size + 15) & ~15;
			good &= pp.EncryptData(mode, b3.data(), &len, &al, hash, k) == 0;
			len2 = len; al2 = (len + 15) & ~15;
			std::vector<u8> b4(b3.begin(), b3.begin() + len);
			b4.resize(al2 + 32);
			good &= sd_decrypt(mode, b4.data(), &len2, &al2, k, hash) == 0 && len2 == size && !memcmp(b4.data(), plain.data(), size);
			// a wrong key fails the hash
			u8 bad[16] = {1};
			len2 = len; al2 = (len + 15) & ~15;
			std::vector<u8> b5(b3.begin(), b3.begin() + len);
			b5.resize(al2 + 32);
			if (mode > 1) good &= sd_decrypt(mode, b5.data(), &len2, &al2, bad, hash) != 0;
			CHECK(good, "encryption mode %d, %d bytes", mode, size);
			ok += good;
			total++;
		}
	// the PARAM.SFO hashes: the same bytes from both
	int hash_ok = 0;
	for (int mode : {1, 3, 5}) {
		std::vector<u8> sfo(4912);
		for (size_t i = 0; i < sfo.size(); i++) sfo[i] = (u8)(i * 31 + mode);
		std::vector<u8> a = sfo, b = sfo;
		a.resize(4928); b.resize(4928);
		int r1 = sd_update_hash(a.data(), 4912, 4264, mode), r2 = pp.UpdateHash(b.data(), 4912, 4264, mode);
		CHECK(r1 == r2 && a == b, "PARAM.SFO hash mode %d", mode);
		hash_ok += r1 == r2 && a == b;
	}
	printf("encryption: %d/%d sizes and modes both ways, PARAM.SFO hashes %d/3 identical\n", ok, total, hash_ok);
}

int main(int argc, char **argv) {
	root = argc > 1 ? argv[1] : "build/utility_native";
	void *m = mmap(nullptr, 0x40000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (m == MAP_FAILED) fatal("mmap failed");
	g_mem = (u8 *)m;
	if (system(("rm -rf '" + root + "' && mkdir -p '" + root + "'").c_str()) != 0) fatal("can't create %s", root.c_str());
	static Thread mainThread;
	strcpy(mainThread.name, "MEMCARD");
	mainThread.status = TS_RUNNING;
	mainThread.prio = 0x30;
	g_cur = &mainThread;
	kirk_init(__ChnnlsvKirkState());
	g_UtilityConfig.bEncryptSave = true;   // like PPSSPP's default and a PSP

	ge_soft_init();
	audio_hle_init();
	size_t before = hle.size();
	utility_hle_init();
	tab[0].assign(hle.begin() + before, hle.end());
	before = hle.size();
	sysdlg_init();
	tab[1].assign(hle.begin() + before, hle.end());

	test_crypto();

	// --- settings and modules
	for (int k = 0; k < 2; k++) {
		static u32 res[2][32];
		int n = 0;
		for (u32 id = 1; id <= 11; id++) {
			SW(OUT, 0xAAAAAAAA);
			res[k][n++] = id == 1 ? call(k, "sceUtilityGetSystemParamString", id, OUT, 32) : call(k, "sceUtilityGetSystemParamInt", id, OUT);
			res[k][n++] = id == 1 ? (u32)strcmp((const char *)PTR(OUT), "PLAYER") : LW(OUT);
		}
		res[k][n++] = call(k, "sceUtilityGetSystemParamString", 1, OUT, 4);
		res[k][n++] = call(k, "sceUtilityLoadModule", 0x300) | call(k, "sceUtilityLoadModule", 0x301) << 1 | call(k, "sceUtilityUnloadModule", 0x300) << 2;
		res[k][n++] = call(k, "sceUtilityLoadModule", 0x999);
		if (k) CHECK(!memcmp(res[0], res[1], sizeof res[0]), "system settings and modules differ");
	}

	// --- SIZES before any save, then the first save (AUTOSAVE) and SIZES after it
	auto sizes = [](int k) {
		request(8, "", 0);
		memset(PTR(MS_FREE), 0xAA, 0x300);
		P()->msFree.ptr = MS_FREE;
		P()->msData.ptr = MS_DATA;
		P()->utilityData.ptr = USED_DATA;
		memset(PTR(MS_DATA), 0, 36);
		strcpy((char *)PTR(MS_DATA), "ULUS10323");
		strcpy((char *)PTR(MS_DATA + 16), "RVSAVEDATA0");
		P()->dataSize = DATA_SIZE;
		Run r = run(k, SAVEDATA, PARAM);
		memcpy(PTR(PARAM + 1536), PTR(MS_FREE), 0x300);   // compared with the request
		return r;
	};
	both("SIZES (no saves)", sizes, true, 1536 + 0x300);
	CHECK(LW(PARAM + 28) == 0x801103C7, "SIZES result %08x", LW(PARAM + 28));
	both("AUTOSAVE RVSAVEDATA0", [](int k) { request(1, "RVSAVEDATA0", 1); return run(k, SAVEDATA, PARAM); });
	CHECK(LW(PARAM + 28) == 0, "AUTOSAVE result %08x", LW(PARAM + 28));
	same_save("AUTOSAVE", "RVSAVEDATA0");
	both("SIZES (one save)", sizes, true, 1536 + 0x300);
	both("AUTOLOAD RVSAVEDATA0", [](int k) { request(0, "RVSAVEDATA0"); Run r = run(k, SAVEDATA, PARAM); CHECK(loaded(1), "%s AUTOLOAD data", impl_name[k]); return r; });
	both("AUTOLOAD missing", [](int k) { request(0, "RVSAVEDATA7"); return run(k, SAVEDATA, PARAM); });

	// --- the game's profile screens: LISTSAVE to the second slot, LISTLOAD it, LISTDELETE it
	auto list3 = [] { name_list({"RVSAVEDATA0", "RVSAVEDATA1", "RVSAVEDATA2"}); };
	both("LISTSAVE: down, save", [&](int k) {
		request(5, "", 2); list3();
		return run(k, SAVEDATA, PARAM, presses({{20, CTRL_DOWN}, {30, CTRL_CROSS}, {90, CTRL_CIRCLE}}));
	}, false);
	CHECK(LW(PARAM + 28) == 0 && !strcmp((const char *)PTR(PARAM + 76), "RVSAVEDATA1"), "LISTSAVE result %08x save '%s'", LW(PARAM + 28), (const char *)PTR(PARAM + 76));
	same_save("LISTSAVE", "RVSAVEDATA1");
	both("LISTSAVE: overwrite slot 1, yes", [&](int k) {
		request(5, "", 3); list3();
		return run(k, SAVEDATA, PARAM, presses({{20, CTRL_DOWN}, {30, CTRL_CROSS}, {40, CTRL_LEFT}, {50, CTRL_CROSS}, {120, CTRL_CIRCLE}}));
	}, false);
	both("LISTSAVE: back out", [&](int k) { request(5, "", 4); list3(); return run(k, SAVEDATA, PARAM, presses({{20, CTRL_CIRCLE}})); });
	CHECK(LW(PARAM + 28) == 1, "cancelled LISTSAVE result %08x", LW(PARAM + 28));
	both("LISTLOAD: down, load", [&](int k) {
		request(4, ""); list3();
		Run r = run(k, SAVEDATA, PARAM, presses({{20, CTRL_DOWN}, {30, CTRL_CROSS}, {90, CTRL_CIRCLE}}));
		CHECK(loaded(3), "%s LISTLOAD data", impl_name[k]);
		return r;
	}, false);
	both("SAVE (overwrite, yes)", [](int k) { request(3, "RVSAVEDATA0", 5); return run(k, SAVEDATA, PARAM, presses({{20, CTRL_LEFT}, {30, CTRL_CROSS}, {90, CTRL_CIRCLE}})); }, false);
	both("LOAD (yes)", [](int k) { request(2, "RVSAVEDATA0"); Run r = run(k, SAVEDATA, PARAM, presses({{30, CTRL_CROSS}, {90, CTRL_CIRCLE}})); CHECK(loaded(5), "%s LOAD data", impl_name[k]); return r; }, false);
	both("LOAD missing", [](int k) { request(2, "RVSAVEDATA2"); return run(k, SAVEDATA, PARAM, presses({{30, CTRL_CIRCLE}})); });
	both("LISTDELETE: down, yes", [&](int k) {
		request(6, ""); list3();
		Run r = run(k, SAVEDATA, PARAM, presses({{20, CTRL_DOWN}, {30, CTRL_CROSS}, {40, CTRL_LEFT}, {50, CTRL_CROSS}, {120, CTRL_CIRCLE}, {140, CTRL_CIRCLE}}));
		CHECK(!exists(save_dir("RVSAVEDATA1")) && exists(save_dir("RVSAVEDATA0")), "%s LISTDELETE", impl_name[k]);
		return r;
	}, false);
	both("LISTALLDELETE: back out", [&](int k) { request(7, ""); return run(k, SAVEDATA, PARAM, presses({{20, CTRL_CIRCLE}})); });

	// --- messages
	auto msg = [](u32 size, int type, u32 options, u32 err, const char *text) {
		auto *m = (pspMessageDialog *)PTR(MSG_PARAM);
		memset(m, 0, sizeof *m);
		m->common.size = size;
		m->common.buttonSwap = 1;
		m->type = type;
		m->errorNum = err;
		m->options = options;
		strcpy(m->string, text);
	};
	both("MSG yes/no: right, cross (No)", [&](int k) { msg(708, 1, 0x10, 0, "Create a new profile?"); return run(k, MSG, MSG_PARAM, presses({{20, CTRL_RIGHT}, {30, CTRL_CROSS}})); }, true, 708);
	CHECK(LW(MSG_PARAM + 576) == 2, "button pressed %u", LW(MSG_PARAM + 576));
	both("MSG yes/no: cross (Yes)", [&](int k) { msg(708, 1, 0x10, 0, "Overwrite?"); return run(k, MSG, MSG_PARAM, presses({{20, CTRL_CROSS}})); }, true, 708);
	both("MSG OK, circle backs out", [&](int k) { msg(708, 1, 0x20, 0, "Saved."); return run(k, MSG, MSG_PARAM, presses({{20, CTRL_CIRCLE}})); }, true, 708);
	both("MSG V2 text", [&](int k) { msg(580, 1, 0, 0, "No memory stick."); return run(k, MSG, MSG_PARAM, presses({{25, CTRL_CIRCLE}})); }, true, 580);
	both("MSG error code", [&](int k) { msg(708, 0, 0, 0x80110307, ""); return run(k, MSG, MSG_PARAM, presses({{25, CTRL_CIRCLE}})); }, true, 708);
	both("MSG bad error code", [&](int k) { msg(708, 0, 0, 0x110307, ""); return run(k, MSG, MSG_PARAM); }, true, 708);
	both("MSG bad option", [&](int k) { msg(708, 1, 0x100, 0, "x"); return run(k, MSG, MSG_PARAM); }, true, 708);

	// --- network and game sharing
	both("Netconf (ad hoc)", [](int k) { memset(PTR(NET_PARAM), 0, 0x44); SW(NET_PARAM, 0x44); SW(NET_PARAM + 48, 2); return run(k, NET, NET_PARAM); }, true, 0);
	CHECK(LW(NET_PARAM + 28) == 0, "netconf result %08x", LW(NET_PARAM + 28));
	both("GameSharing", [](int k) { memset(PTR(SHARE_PARAM), 0, 0x64); SW(SHARE_PARAM, 0x64); return run(k, SHARE, SHARE_PARAM); }, true, 0);
	CHECK(LW(SHARE_PARAM + 28) == 1, "game sharing result %08x", LW(SHARE_PARAM + 28));
	for (int k = 0; k < 2; k++) {   // a second dialog while one is up; Update of the wrong type
		request(8, "");
		u32 a = call(k, SAVEDATA.init, PARAM);
		msg(708, 1, 0, 0, "x");
		u32 b = call(k, MSG.init, MSG_PARAM), c = call(k, MSG.update, 1), d = call(k, MSG.status);
		finish(k, SAVEDATA);
		if ((int)b >= 0) finish(k, MSG);
		static u32 res[2][4];
		res[k][0] = a; res[k][1] = b; res[k][2] = c; res[k][3] = d;
		if (k) CHECK(!memcmp(res[0], res[1], sizeof res[0]), "busy: %08x %08x %08x %08x vs %08x %08x %08x %08x",
		             res[0][0], res[0][1], res[0][2], res[0][3], res[1][0], res[1][1], res[1][2], res[1][3]);
	}

	// --- saves cross over: each side loads the other's, on one stick
	use_stick("shared");
	auto xrun = [](int k) { Run r = run(k, SAVEDATA, PARAM); if (getenv("TN_DEBUG")) fprintf(stderr, "  %s: init %08x statuses %s %d updates result %08x\n", impl_name[k], r.init, r.statuses.c_str(), r.updates, LW(PARAM + 28)); };
	request(1, "RVSAVEDATA0", 11); xrun(0);     // PPSSPP saves...
	request(0, "RVSAVEDATA0"); xrun(1);         // ...the runtime loads
	CHECK(LW(PARAM + 28) == 0 && loaded(11) && P()->dataSize == DATA_SIZE, "native load of PPSSPP's save: %08x", LW(PARAM + 28));
	request(1, "RVSAVEDATA1", 12); xrun(1);     // the runtime saves...
	request(0, "RVSAVEDATA1"); xrun(0);         // ...PPSSPP loads
	CHECK(LW(PARAM + 28) == 0 && loaded(12), "PPSSPP load of the runtime's save: %08x", LW(PARAM + 28));
	{   // and the runtime's save passes PPSSPP's own checks: the file's hash, and the PARAM.SFO's hashes
		SavedataParam pp;
		auto sfo = read_file(save_dir("RVSAVEDATA1") + "/PARAM.SFO"), data = read_file(save_dir("RVSAVEDATA1") + "/GAMEDATA.BIN");
		auto ents = sfo_entries(sfo);
		const u8 *hash = ents["SAVEDATA_FILE_LIST"].second.data() + 13;
		int len = (int)data.size(), al = (len + 15) & ~15;
		data.resize(al + 32);
		CHECK(!strcmp((const char *)ents["SAVEDATA_FILE_LIST"].second.data(), "GAMEDATA.BIN") &&
		      pp.DecryptData(5, data.data(), &len, &al, (const u8 *)"RIVALS_2Z\0\0\0\0\0\0\0", hash) == 0 && len == (int)DATA_SIZE, "file hash");
		std::vector<u8> again = sfo;
		again.resize((sfo.size() + 15) & ~15);
		int off = sfo_offset(sfo, "SAVEDATA_PARAMS");
		pp.UpdateHash(again.data(), (int)sfo.size(), off, 5);
		CHECK(off > 0 && !memcmp(again.data(), sfo.data(), sfo.size()) && sfo[off] == 0x41, "PARAM.SFO hashes (flags %02x)", off > 0 ? sfo[off] : 0);
	}
	// an unencrypted save, as older builds of the runtime wrote them
	g_UtilityConfig.bEncryptSave = false;
	request(1, "RVSAVEDATA2", 13); xrun(0);
	g_UtilityConfig.bEncryptSave = true;
	request(0, "RVSAVEDATA2"); xrun(1);
	CHECK(LW(PARAM + 28) == 0 && loaded(13), "native load of an unencrypted save: %08x", LW(PARAM + 28));
	// and the runtime saving over it encrypts it, readable by both
	request(1, "RVSAVEDATA2", 14); xrun(1);
	for (int k = 0; k < 2; k++) { request(0, "RVSAVEDATA2"); xrun(k); CHECK(LW(PARAM + 28) == 0 && loaded(14), "%s load after upgrade", impl_name[k]); }
	CHECK(read_file(save_dir("RVSAVEDATA2") + "/GAMEDATA.BIN").size() == DATA_SIZE + 16, "upgraded save not encrypted");

	printf("%s: %d checks, %d failed\n", failures ? "FAILED" : "OK", checks, failures);
	return failures ? 1 : 0;
}
