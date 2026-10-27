// Checks the runtime's own SAS mixer and Atrac player (runtime/sas.c, runtime/atrac.c) against PPSSPP's
// (this directory): both get the same calls on separate copies of guest memory, and every return value,
// output word and PCM sample is compared.
//   (a) each .at3 given: set data, sound sample / loop info, decode past the end of the loop (looping
//       forever), seek with ResetPlayPosition, decode to the end without looping;
//   (b) SAS: 32 voices of pseudo-random VAG at assorted pitches, volumes and envelopes, each reverb type,
//       key on/off and pause while mixing, __sceSasCoreWithMix.
//
//   build/test_native file.at3...      (built by runtime/audio/build.sh)
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

#include "../kernel.h"
#include "audio_hle.h"

extern "C" void sas_init(void);
extern "C" void atrac_init(void);

u8 *g_mem;
int g_loglevel = -1;   // PPSSPP logs the expected errors (key on while paused, ...)
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
static u64 delayedUs;
u64 now_us(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (u64)t.tv_sec * 1000000 + t.tv_nsec / 1000 + delayedUs;
}
static u32 kheap = 0x08100000;
u32 kalloc(u32 size) {
	u32 a = (kheap + 15) & ~15u;
	kheap = a + size;
	memset(PTR(a), 0, size);
	return a;
}
void thread_delay_until(u64 when) {
	u64 n = now_us();
	if (when > n) delayedUs += when - n;
}
void thread_resched(void) {}
u32 run_guest(Ctx *tmpl, u32 addr, u32 a0, u32 a1, u32 a2) { fatal("unexpected guest call %08x", addr); }

static std::vector<HleEnt> *registering;
static std::vector<HleEnt> ppsspp, native;
void hle_register(const HleEnt *ents, int n) { registering->insert(registering->end(), ents, ents + n); }

// Guest memory: PPSSPP's side works at the addresses given, the native side at the same addresses + OFS.
static const u32 OFS = 0x01000000;
static int failures, checks;

static u32 call1(std::vector<HleEnt> &tab, const char *name, const u32 *a) {
	for (const HleEnt &e : tab) {
		if (!strcmp(e.name, name)) {
			Ctx ctx{};
			Ctx *c = &ctx;
			for (int i = 0; i < 7; i++) A(i) = a[i];
			c->r[2] = 0xDEADBEEF;
			e.fn(c);
			return c->r[2];
		}
	}
	fatal("HLE %s not registered", name);
}

// Calls both; `ptrs` marks which arguments are guest addresses (moved by OFS on the native side).
static u32 both(const char *name, unsigned ptrs, u32 a0 = 0, u32 a1 = 0, u32 a2 = 0, u32 a3 = 0, u32 a4 = 0, u32 a5 = 0, u32 a6 = 0) {
	u32 a[7] = {a0, a1, a2, a3, a4, a5, a6}, b[7];
	for (int i = 0; i < 7; i++) b[i] = a[i] + ((ptrs >> i & 1) && a[i] ? OFS : 0);
	u32 rp = call1(ppsspp, name, a), rn = call1(native, name, b);
	checks++;
	if (rp != rn && failures++ < 30) fprintf(stderr, "FAIL %s: PPSSPP returned %08x, native %08x\n", name, rp, rn);
	return rp;
}

// Compares guest memory [addr, addr+bytes) between the two sides as s16 samples; returns the largest difference.
static int compare(const char *what, u32 addr, u32 bytes, bool report = true) {
	const s16 *p = (const s16 *)PTR(addr), *n = (const s16 *)PTR(addr + OFS);
	int worst = 0, at = -1;
	for (u32 i = 0; i < bytes / 2; i++) {
		int d = abs(p[i] - n[i]);
		if (d > worst) { worst = d; at = (int)i; }
	}
	checks++;
	if (worst && report && failures++ < 30) fprintf(stderr, "FAIL %s: differs by up to %d (word %d: PPSSPP %d, native %d)\n", what, worst, at, p[at], n[at]);
	return worst;
}

// ================================================================= Atrac
static void test_atrac(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) fatal("can't read %s", path);
	std::vector<u8> file;
	u8 buf[65536];
	size_t got;
	while ((got = fread(buf, 1, sizeof buf, f)) > 0) file.insert(file.end(), buf, buf + got);
	fclose(f);
	const u32 data = 0x08200000, out = 0x08180000, info = 0x08170000;
	memcpy(PTR(data), file.data(), file.size());
	memcpy(PTR(data + OFS), file.data(), file.size());
	const int failsBefore = failures;

	int id = (int)both("sceAtracSetDataAndGetID", 1, data, (u32)file.size());
	if (id < 0) { printf("atrac %s: SetData failed %08x\n", path, id); return; }
	both("sceAtracGetSoundSample", 14, id, info, info + 4, info + 8);
	compare("GetSoundSample", info, 12);
	const int endSample = *(s32 *)PTR(info), loopStart = *(s32 *)PTR(info + 4), loopEnd = *(s32 *)PTR(info + 8);
	both("sceAtracGetBitrate", 2, id, info);
	compare("GetBitrate", info, 4);
	both("sceAtracSetLoopNum", 0, id, (u32)-1);

	// Decode until past the loop end twice (or 400 frames).
	int frames = 0, samples = 0, worst = 0;
	auto decode = [&](int maxFrames, bool &done) {
		done = false;
		for (int i = 0; i < maxFrames; i++) {
			memset(PTR(out), 0x55, 0x2000);
			memset(PTR(out + OFS), 0x55, 0x2000);
			u32 r = both("sceAtracDecodeData", 30, id, out, info, info + 4, info + 8);
			compare("DecodeData counts", info, 12);
			int n = *(s32 *)PTR(info);
			int d = compare("DecodeData PCM", out, 0x2000, false);
			if (d > worst) worst = d;
			if (d > 2 && failures++ < 30) fprintf(stderr, "FAIL DecodeData PCM: frame %d differs by %d\n", frames, d);
			frames++;
			samples += n;
			if (r || *(s32 *)PTR(info + 4)) { done = true; return; }
		}
	};
	bool done;
	const int toLoop = loopEnd > 0 ? (2 * loopEnd - loopStart) / 2048 + 4 : 300;
	decode(toLoop < 400 ? toLoop : 400, done);

	// Seek, then play out without looping.
	const int seek = endSample > 300000 ? 200000 + 77 : endSample / 2;
	both("sceAtracGetBufferInfoForResetting", 4, id, (u32)seek, info);
	for (int i = 0; i < 2; i++)   // the buffer pointers: native's are at its own copy
		if (*(u32 *)PTR(info + OFS + i * 16)) *(u32 *)PTR(info + OFS + i * 16) -= OFS;
	compare("GetBufferInfoForResetting", info, 32);
	both("sceAtracResetPlayPosition", 0, id, (u32)seek, 0, 0);
	both("sceAtracSetLoopNum", 0, id, 0);
	decode(2000, done);
	both("sceAtracGetInternalErrorInfo", 2, id, info);
	compare("GetInternalErrorInfo", info, 4);
	both("sceAtracReleaseAtracID", 0, id);
	printf("atrac %s: end %d, loop %d..%d; %d frames, %d samples, largest PCM difference %d -> %s\n", strrchr(path, '/') ? strrchr(path, '/') + 1 : path,
		endSample, loopStart, loopEnd, frames, samples, worst, failures == failsBefore ? "same" : "DIFFERENT");
}

// ================================================================= SAS
static u32 rng = 12345;
static u32 rnd() { rng = rng * 1103515245 + 12345; return rng >> 8; }

static void test_sas() {
	const u32 core = 0x08A00000, out = 0x08A40000, vag = 0x08A80000;
	const int grain = 256;
	const int failsBefore = failures;
	both("__sceSasInit", 1, core, grain, 32, 0, 44100);
	// 32 pseudo-random VAG sounds, 40-200 blocks, some looping.
	u32 at = vag;
	u32 addr[32], size[32];
	for (int v = 0; v < 32; v++) {
		int blocks = 40 + rnd() % 160;
		bool loop = v % 3 == 0;
		addr[v] = at;
		for (int b = 0; b < blocks; b++) {
			u8 *p = PTR(at + b * 16);
			p[0] = (u8)((rnd() % 5) << 4 | (rnd() % 13));
			p[1] = loop ? (b == 0 ? 6 : b == blocks - 1 ? 3 : 0) : (b == blocks - 1 ? 1 : 0);
			for (int i = 2; i < 16; i++) p[i] = (u8)rnd();
		}
		size[v] = blocks * 16;
		memcpy(PTR(at + OFS), PTR(at), size[v]);
		at += size[v] + 64;
	}
	int worst = 0;
	auto mix = [&](int grains, bool withMix) {
		for (int g = 0; g < grains; g++) {
			for (int i = 0; i < grain * 2; i++) ((s16 *)PTR(out))[i] = ((s16 *)PTR(out + OFS))[i] = (s16)(i * 37);
			if (withMix)
				both("__sceSasCoreWithMix", 3, core, out, 0x800, 0x1000);
			else
				both("__sceSasCore", 3, core, out);
			int d = compare("SAS output", out, grain * 4, false);
			if (d > worst) worst = d;
			if (d && failures++ < 30) fprintf(stderr, "FAIL SAS output: grain differs by %d\n", d);
			both("__sceSasGetEndFlag", 1, core);
			for (int v = 0; v < 32; v += 5) both("__sceSasGetEnvelopeHeight", 1, core, v);
		}
	};
	for (int rev = 0; rev < 8; rev++) {
		both("__sceSasRevType", 1, core, rev);
		both("__sceSasRevParam", 1, core, rev * 16, rev * 9);
		both("__sceSasRevEVOL", 1, core, 0x1000 - rev * 0x100, 0x800 + rev * 0x80);
		both("__sceSasRevVON", 1, core, rev & 1, 1);
		for (int v = 0; v < 32; v++) {
			both("__sceSasSetVoice", 5, core, v, addr[v], size[v], v % 3 == 0);
			both("__sceSasSetPitch", 1, core, v, 0x200 + rnd() % 0x3E00);
			int l = rnd() % 0x1001, r = rnd() % 0x1001;
			both("__sceSasSetVolume", 1, core, v, l, r, rnd() % 0x1001, rnd() % 0x1001);
			if (v % 2)
				both("__sceSasSetSimpleADSR", 1, core, v, rnd() & 0xFFFF, rnd() & 0xFFFF);
			else {
				both("__sceSasSetADSRmode", 1, core, v, 0xF, rnd() % 6, rnd() % 6, rnd() % 6, rnd() % 6);
				both("__sceSasSetADSR", 1, core, v, 0xF, rnd() % 0x80000000, rnd() % 0x80000000, rnd() % 0x80000000, rnd() % 0x80000000);
				both("__sceSasSetSL", 1, core, v, rnd() % 0x40000000);
			}
			if (rnd() % 4) both("__sceSasSetKeyOn", 1, core, v);
		}
		mix(30, rev & 1);
		both("__sceSasSetPause", 1, core, 0x0000F0F0, 1);
		mix(5, false);
		both("__sceSasGetPauseFlag", 1, core);
		both("__sceSasSetPause", 1, core, 0x0000F0F0, 0);
		for (int v = 0; v < 32; v += 2) both("__sceSasSetKeyOff", 1, core, v);
		mix(40, false);
		for (int v = 1; v < 32; v += 4) both("__sceSasSetKeyOn", 1, core, v);
		mix(20, true);
	}
	printf("sas: %d grains, largest difference %d -> %s\n", 8 * 95, worst, failures == failsBefore ? "same" : "DIFFERENT");
}

int main(int argc, char **argv) {
	void *m = mmap(nullptr, 0x40000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (m == MAP_FAILED) fatal("mmap failed");
	g_mem = (u8 *)m;
	static Thread mainThread;
	strcpy(mainThread.name, "test");
	mainThread.status = TS_RUNNING;
	g_cur = &mainThread;

	registering = &ppsspp;
	audio_hle_init();
	registering = &native;
	sas_init();
	atrac_init();
	for (int i = 1; i < argc; i++) test_atrac(argv[i]);
	test_sas();
	printf("%d checks, %d failed: %s\n", checks, failures, failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
}
