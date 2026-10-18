// Standalone test of the movie HLE (PPSSPP sceMpeg + MediaEngine): plays a PSMF file the way a game
// does - ring buffer filled by a "guest" callback through sceMpegRingbufferPut, sceMpegGetAvcAu +
// sceMpegAvcDecode, sceMpegGetAtracAu + sceMpegAtracDecode - and compares the decoded frames with
// FFmpeg's own decode of the same stream. Also plays the built-in fake movie (mpeg_fake_pmf).
//
//   build/test_mpeg [clip.pmf] [out-dir]
//
// GPL-2.0+ (links PPSSPP code), see LICENSE.TXT.

#include <cmath>
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
#include "../audio/audio_hle.h"
#include "mpeg_hle.h"

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
u32 cur_module_gp(void) { return 0x08A80000; }

static std::vector<HleEnt> hle;
void hle_register(const HleEnt *ents, int n) { hle.insert(hle.end(), ents, ents + n); }

// "Guest" code: the ring buffer fill callback lives at a fake address and is run by host code here.
static const u32 RING_CALLBACK = 0x08900000;
static const u8 *g_streamData;
static size_t g_streamSize, g_streamPos;
static int g_callbackCalls;
u32 run_guest(Ctx *tmpl, u32 addr, u32 a0, u32 a1, u32 a2) {
	if (addr != RING_CALLBACK) fatal("unexpected guest call %08x", addr);
	g_callbackCalls++;
	// int callback(void *data, int numPackets, void *arg): read up to numPackets 2048-byte packets.
	size_t want = (size_t)a1 * 2048;
	size_t left = g_streamSize - g_streamPos;
	size_t n = want < left ? want : left;
	memcpy(PTR(a0), g_streamData + g_streamPos, n);
	g_streamPos += n;
	return (u32)(n / 2048);
}

// ================================================================= helpers
static Ctx ctx;
static u32 call(const char *name, u32 a0 = 0, u32 a1 = 0, u32 a2 = 0, u32 a3 = 0, u32 a4 = 0, u32 a5 = 0, u32 a6 = 0) {
	for (const HleEnt &e : hle) {
		if (!strcmp(e.name, name)) {
			Ctx *c = &ctx;
			A(0) = a0; A(1) = a1; A(2) = a2; A(3) = a3; A(4) = a4; A(5) = a5; A(6) = a6;
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

static void write_png(const char *path, const u8 *rgba, int w, int h, int stride_px) {
	std::vector<u8> raw;
	for (int y = 0; y < h; y++) {
		raw.push_back(0);
		const u8 *row = rgba + (size_t)y * stride_px * 4;
		for (int x = 0; x < w; x++) { raw.push_back(row[x * 4]); raw.push_back(row[x * 4 + 1]); raw.push_back(row[x * 4 + 2]); }
	}
	uLongf zlen = compressBound(raw.size());
	std::vector<u8> z(zlen);
	compress2(z.data(), &zlen, raw.data(), raw.size(), 9);
	FILE *f = fopen(path, "wb");
	if (!f) return;
	auto be32 = [&](u32 v) { u8 b[4] = {u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v)}; fwrite(b, 1, 4, f); };
	auto chunk = [&](const char *type, const u8 *data, u32 len) {
		be32(len); fwrite(type, 1, 4, f); if (len) fwrite(data, 1, len, f);
		u32 crc = crc32(0, (const Bytef *)type, 4); crc = crc32(crc, data, len); be32(crc);
	};
	static const u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
	fwrite(sig, 1, 8, f);
	u8 ihdr[13] = {u8(w >> 24), u8(w >> 16), u8(w >> 8), u8(w), u8(h >> 24), u8(h >> 16), u8(h >> 8), u8(h), 8, 2, 0, 0, 0};
	chunk("IHDR", ihdr, 13);
	chunk("IDAT", z.data(), (u32)zlen);
	chunk("IEND", nullptr, 0);
	fclose(f);
}

// FFmpeg's decode of the program stream after the PSMF header, as 480x272 RGBA frames.
static std::vector<std::vector<u8>> reference_frames(const std::string &pmfPath) {
	std::vector<std::vector<u8>> frames;
	std::string cmd = "ffmpeg -v error -skip_initial_bytes 2048 -f mpeg -i '" + pmfPath + "' -map 0:v -f rawvideo -pix_fmt rgba - 2>/dev/null";
	FILE *p = popen(cmd.c_str(), "r");
	if (!p) return frames;
	std::vector<u8> fr(480 * 272 * 4);
	while (fread(fr.data(), 1, fr.size(), p) == fr.size()) frames.push_back(fr);
	pclose(p);
	return frames;
}

static double psnr(const u8 *ours, int strideBytes, const std::vector<u8> &ref) {
	double se = 0;
	for (int y = 0; y < 272; y++)
		for (int x = 0; x < 480; x++)
			for (int c = 0; c < 3; c++) {
				double d = (double)ours[y * strideBytes + x * 4 + c] - ref[(y * 480 + x) * 4 + c];
				se += d * d;
			}
	double mse = se / (480.0 * 272 * 3);
	return mse <= 0 ? 99.0 : 10 * log10(255.0 * 255.0 / mse);
}

struct PlayResult { int callbacks = 0, frames = 0, audioFrames = 0, audioNonZero = 0; double minPsnr = 99, avgPsnr = 0; int compared = 0; };

static PlayResult play(const char *label, const std::vector<u8> &pmf, const std::vector<std::vector<u8>> &ref, const std::string &pngOut, int packets) {
	PlayResult res;
	g_streamData = pmf.data() + 2048;
	g_streamSize = pmf.size() - 2048;
	g_streamPos = 0;
	g_callbackCalls = 0;

	const u32 hdr = 0x08A00000, ringStruct = 0x08A10000, vars = 0x08A20000, avcAu = 0x08A20100, atracAu = 0x08A20200;
	const u32 frameBuf = 0x04088000, pcmBuf = 0x08A30000, mpegHandle = 0x08A20300;
	memcpy(PTR(hdr), pmf.data(), 2048);

	CHECK(call("sceMpegInit") == 0, "sceMpegInit");
	u32 ringSize = call("sceMpegRingbufferQueryMemSize", packets);
	CHECK(ringSize >= packets * 2048u, "ring size %x", ringSize);
	const u32 ringData = 0x08B00000;
	CHECK(call("sceMpegRingbufferConstruct", ringStruct, packets, ringData, ringSize, RING_CALLBACK, 0x1234) == 0, "ring construct");
	u32 memSize = call("sceMpegQueryMemSize", 0);
	const u32 mpegData = 0x08C00000;
	u32 r = call("sceMpegCreate", mpegHandle, mpegData, memSize, ringStruct, 512, 0, 0);
	CHECK(r == 0, "sceMpegCreate %08x (mem %x)", r, memSize);
	u32 mpeg = mpegHandle;

	CHECK(call("sceMpegQueryStreamOffset", mpeg, hdr, vars) == 0 && LW(vars) == 2048, "stream offset %u", LW(vars));
	CHECK(call("sceMpegQueryStreamSize", hdr, vars) == 0 && LW(vars) == pmf.size() - 2048, "stream size %u", LW(vars));
	u32 vs = call("sceMpegRegistStream", mpeg, 0, 0);
	u32 as = call("sceMpegRegistStream", mpeg, 1, 0);
	u32 esBuf = call("sceMpegMallocAvcEsBuf", mpeg);
	CHECK(esBuf != 0, "MallocAvcEsBuf");
	CHECK(call("sceMpegInitAu", mpeg, esBuf, avcAu) == 0, "InitAu avc");
	CHECK(call("sceMpegQueryAtracEsSize", mpeg, vars, vars + 4) == 0, "QueryAtracEsSize");
	u32 atracEsSize = LW(vars), atracOutSize = LW(vars + 4);
	const u32 atracEs = 0x08A40000;
	CHECK(call("sceMpegInitAu", mpeg, atracEs, atracAu) == 0, "InitAu atrac");

	int noData = 0, decodeErrors = 0;
	for (int iter = 0; iter < 5000 && noData < 30; iter++) {
		s32 avail = (s32)call("sceMpegRingbufferAvailableSize", ringStruct);
		if (avail > 0 && g_streamPos < g_streamSize)
			call("sceMpegRingbufferPut", ringStruct, avail, avail);

		u32 ar = call("sceMpegGetAtracAu", mpeg, as, atracAu, vars + 8);
		if (ar == 0) {
			call("sceMpegAtracDecode", mpeg, atracAu, pcmBuf, 0);
			res.audioFrames++;
			const s16 *pcm = (const s16 *)PTR(pcmBuf);
			for (int i = 0; i < 2048 * 2; i++) if (pcm[i]) { res.audioNonZero++; break; }
		}

		u32 gr = call("sceMpegGetAvcAu", mpeg, vs, avcAu, vars + 12);
		if (gr != 0) {
			noData++;
			continue;
		}
		noData = 0;
		SW(vars + 16, frameBuf);
		SW(vars + 20, 0);
		u32 dr = call("sceMpegAvcDecode", mpeg, avcAu, 512, vars + 16, vars + 20);
		if (dr != 0) { decodeErrors++; continue; }
		if (LW(vars + 20) == 1) {  // a frame was output
			if (res.frames < (int)ref.size()) {
				double p = psnr(PTR(frameBuf), 512 * 4, ref[res.frames]);
				res.minPsnr = std::min(res.minPsnr, p);
				res.avgPsnr += p;
				res.compared++;
			}
			if (res.frames == 20 && !pngOut.empty())
				write_png(pngOut.c_str(), PTR(frameBuf), 480, 272, 512);
			res.frames++;
		}
	}
	if (res.compared) res.avgPsnr /= res.compared;
	res.callbacks = g_callbackCalls;
	printf("%s: %d frames decoded (%d errors), %d atrac frames (%d non-silent), ring callback ran %d times, "
	       "fed %zu/%zu bytes, atrac es %u/%u, PSNR vs FFmpeg min %.1f avg %.1f dB over %d frames\n",
	       label, res.frames, decodeErrors, res.audioFrames, res.audioNonZero, g_callbackCalls, g_streamPos, g_streamSize,
	       atracEsSize, atracOutSize, res.minPsnr, res.avgPsnr, res.compared);

	call("sceMpegUnRegistStream", mpeg, vs);
	call("sceMpegUnRegistStream", mpeg, as);
	call("sceMpegFreeAvcEsBuf", mpeg, esBuf);
	call("sceMpegRingbufferDestruct", ringStruct);
	CHECK(call("sceMpegDelete", mpeg) == 0, "delete");
	CHECK(call("sceMpegFinish") == 0, "finish");
	return res;
}

int main(int argc, char **argv) {
	std::string clip = argc > 1 ? argv[1] : "/home/claude/rcsm/build/test_clip.pmf";
	std::string outDir = argc > 2 ? argv[2] : "/home/claude/rcsm/build";
	void *m = mmap(nullptr, 0x40000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (m == MAP_FAILED) fatal("mmap failed");
	g_mem = (u8 *)m;
	static Thread mainThread;
	strcpy(mainThread.name, "test");
	mainThread.status = TS_RUNNING;
	g_cur = &mainThread;

	audio_hle_init();
	mpeg_hle_init();

	// (1) the test clip, against FFmpeg's decode
	FILE *f = fopen(clip.c_str(), "rb");
	if (!f) fatal("can't open %s (build.sh makes it with tools/make_pmf.py)", clip.c_str());
	std::vector<u8> pmf;
	u8 tmp[65536];
	size_t n;
	while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) pmf.insert(pmf.end(), tmp, tmp + n);
	fclose(f);
	auto ref = reference_frames(clip);
	printf("clip %s: %zu bytes, FFmpeg reference: %zu frames\n", clip.c_str(), pmf.size(), ref.size());
	// A ring smaller than the clip: several fill callbacks, wrapping around.
	PlayResult a = play("clip", pmf, ref, outDir + "/test_mpeg_frame20.png", 16);
	CHECK(ref.size() > 0, "no reference frames");
	CHECK(a.frames >= (int)ref.size() - 2 && a.frames <= (int)ref.size(), "decoded %d of %zu frames", a.frames, ref.size());
	CHECK(a.minPsnr > 30, "decoded frames don't match FFmpeg's (min PSNR %.1f dB)", a.minPsnr);
	CHECK(a.audioFrames > 10, "too few audio frames (%d)", a.audioFrames);
	CHECK(a.audioNonZero == 0, "silent ATRAC3+ frames decoded to non-silence");
	CHECK(a.callbacks > 2, "ring buffer callback ran only %d times", a.callbacks);

	// (2) the built-in fake movie
	unsigned fsize = 0;
	const unsigned char *fake = mpeg_fake_pmf(&fsize);
	std::vector<u8> fakeV(fake, fake + fsize);
	std::vector<std::vector<u8>> noRef;
	PlayResult b = play("fake", fakeV, noRef, "", 64);
	CHECK(b.frames >= 28, "fake movie: %d frames", b.frames);
	const u8 *px = PTR(0x04088000 + (100 * 512 + 200) * 4);
	CHECK(px[0] < 8 && px[1] < 8 && px[2] < 8, "fake movie isn't black (%d %d %d)", px[0], px[1], px[2]);
	CHECK(b.audioFrames >= 20 && b.audioNonZero == 0, "fake movie audio: %d frames, %d non-silent", b.audioFrames, b.audioNonZero);

	printf("HLE delays: %.1f ms\n%s\n", delayedUs / 1000.0, failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
}
