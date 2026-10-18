// Standalone test of the audio HLE (PPSSPP SAS + Atrac), through the registered HLE functions with
// guest memory, like recompiled code calls them.
//   (a) streams the start of an ATRAC3 file through sceAtracSetDataAndGetID / GetStreamDataInfo /
//       AddStreamData / DecodeData and writes a WAV, checking levels and that the spectrum is tonal;
//   (b) plays a synthetic VAG sine through SAS voices (key on, pitch, ADSR, key off).
//
//   build/test_audio [file.at3] [out.wav]
//
// GPL-2.0+ (links PPSSPP code), see LICENSE.TXT.

#include <cmath>
#include <complex>
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
// The clock jumps forward on thread delays, as if the thread had slept.
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
void thread_delay_until(u64 when) {  // HLE delays (Media Engine time)
	u64 n = now_us();
	if (when > n) delayedUs += when - n;
}
void thread_resched(void) {}
u32 run_guest(Ctx *tmpl, u32 addr, u32 a0, u32 a1, u32 a2) { fatal("unexpected guest call %08x", addr); }

static std::vector<HleEnt> hle;
void hle_register(const HleEnt *ents, int n) { hle.insert(hle.end(), ents, ents + n); }

// ================================================================= helpers
static Ctx ctx;
static u32 call(const char *name, u32 a0 = 0, u32 a1 = 0, u32 a2 = 0, u32 a3 = 0, u32 a4 = 0, u32 a5 = 0, u32 a6 = 0) {
	for (const HleEnt &e : hle) {
		if (!strcmp(e.name, name)) {
			Ctx *c = &ctx;
			A(0) = a0; A(1) = a1; A(2) = a2; A(3) = a3; A(4) = a4; A(5) = a5; A(6) = a6;
			c->r[2] = 0xDEADBEEF;
			e.fn(c);
			return c->r[2];
		}
	}
	fatal("HLE %s not registered", name);
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void write_wav(const char *path, const std::vector<s16> &pcm, int channels, int rate) {
	FILE *f = fopen(path, "wb");
	if (!f) fatal("can't write %s", path);
	u32 dataBytes = (u32)pcm.size() * 2;
	auto w32 = [&](u32 v) { fwrite(&v, 4, 1, f); };
	auto w16 = [&](u16 v) { fwrite(&v, 2, 1, f); };
	fwrite("RIFF", 1, 4, f); w32(36 + dataBytes); fwrite("WAVEfmt ", 1, 8, f);
	w32(16); w16(1); w16(channels); w32(rate); w32(rate * channels * 2); w16(channels * 2); w16(16);
	fwrite("data", 1, 4, f); w32(dataBytes);
	fwrite(pcm.data(), 2, pcm.size(), f);
	fclose(f);
}

static double rms(const s16 *p, size_t n, int stride) {
	double s = 0;
	for (size_t i = 0; i < n; i++) s += (double)p[i * stride] * p[i * stride];
	return n ? sqrt(s / n) : 0;
}

static void fft(std::vector<std::complex<double>> &a) {
	size_t n = a.size();
	for (size_t i = 1, j = 0; i < n; i++) {
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) std::swap(a[i], a[j]);
	}
	for (size_t len = 2; len <= n; len <<= 1) {
		double ang = -2 * M_PI / len;
		std::complex<double> wl(cos(ang), sin(ang));
		for (size_t i = 0; i < n; i += len) {
			std::complex<double> w(1);
			for (size_t k = 0; k < len / 2; k++) {
				auto u = a[i + k], v = a[i + k + len / 2] * w;
				a[i + k] = u + v;
				a[i + k + len / 2] = u - v;
				w *= wl;
			}
		}
	}
}

struct Spectrum { double flatness, centroidHz, peakHz; };
// Averaged power spectrum of the left channel (Hann windows of 4096), 20 Hz - 16 kHz.
static Spectrum analyze(const s16 *stereo, size_t frames, int stride, int rate) {
	const size_t N = 4096;
	std::vector<double> power(N / 2, 0.0);
	int windows = 0;
	for (size_t start = 0; start + N <= frames; start += N / 2, windows++) {
		std::vector<std::complex<double>> a(N);
		for (size_t i = 0; i < N; i++)
			a[i] = stereo[(start + i) * stride] * (0.5 - 0.5 * cos(2 * M_PI * i / (N - 1)));
		fft(a);
		for (size_t k = 0; k < N / 2; k++) power[k] += std::norm(a[k]);
	}
	size_t k0 = 20 * N / rate, k1 = 16000 * N / rate;
	double logSum = 0, sum = 0, wsum = 0, peak = 0;
	size_t peakK = 0;
	for (size_t k = k0; k < k1; k++) {
		double p = power[k] + 1e-9;
		logSum += log(p);
		sum += p;
		wsum += p * k;
		if (p > peak) { peak = p; peakK = k; }
	}
	size_t n = k1 - k0;
	Spectrum s;
	s.flatness = exp(logSum / n) / (sum / n);
	s.centroidHz = wsum / sum * rate / N;
	s.peakHz = (double)peakK * rate / N;
	return s;
}

// ================================================================= (a) ATRAC3 streaming
static void test_atrac(const char *path, const char *wavOut) {
	FILE *f = fopen(path, "rb");
	if (!f) { CHECK(false, "can't open %s", path); return; }
	std::vector<u8> file;
	u8 tmp[65536];
	size_t n;
	while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) file.insert(file.end(), tmp, tmp + n);
	fclose(f);

	// Like the game: a streaming buffer much smaller than the file.
	const u32 buf = 0x08900000, bufSize = 0x10000;
	const u32 out = 0x08980000, vars = 0x08990000;
	memcpy(PTR(buf), file.data(), bufSize);
	int id = (int)call("sceAtracSetDataAndGetID", buf, bufSize);
	CHECK(id >= 0, "sceAtracSetDataAndGetID returned %08x", id);
	if (id < 0) return;
	call("sceAtracSetLoopNum", id, 0);
	CHECK(call("sceAtracGetOutputChannel", id, vars) == 0 && LW(vars) == 2, "output channels %u", LW(vars));
	u32 r = call("sceAtracGetRemainFrame", id, vars);
	printf("atrac: id %d, file %zu bytes, remain frames after SetData: %d (ret %08x)\n", id, file.size(), (int)LW(vars), r);

	std::vector<s16> pcm;
	const int wantSamples = 44100 * 6;
	int frames = 0, adds = 0, finish = 0;
	u32 addedBytes = 0;
	while ((int)pcm.size() / 2 < wantSamples && !finish) {
		u32 ret = call("sceAtracDecodeData", id, out, vars, vars + 4, vars + 8);
		if (ret != 0) { CHECK(false, "sceAtracDecodeData returned %08x after %d frames", ret, frames); break; }
		int samples = (int)LW(vars);
		finish = (int)LW(vars + 4);
		pcm.insert(pcm.end(), (s16 *)PTR(out), (s16 *)PTR(out) + samples * 2);
		frames++;

		// Refill the streaming buffer the way games do.
		call("sceAtracGetStreamDataInfo", id, vars + 16, vars + 20, vars + 24);
		u32 writePtr = LW(vars + 16), writable = LW(vars + 20), readOffset = LW(vars + 24);
		if (writable > 0 && readOffset < file.size()) {
			u32 bytes = std::min<u32>(writable, (u32)file.size() - readOffset);
			memcpy(PTR(writePtr), file.data() + readOffset, bytes);
			u32 ar = call("sceAtracAddStreamData", id, bytes);
			CHECK(ar == 0, "sceAtracAddStreamData returned %08x", ar);
			adds++;
			addedBytes += bytes;
		}
	}
	call("sceAtracGetRemainFrame", id, vars);
	printf("atrac: %d frames decoded (%zu samples, %.2f s), %d stream refills (%u bytes), remain %d\n", frames, pcm.size() / 2,
		pcm.size() / 2 / 44100.0, adds, addedBytes, (int)LW(vars));
	CHECK(adds > 0, "streaming never asked for more data");
	CHECK(call("sceAtracReleaseAtracID", id) == 0, "release");

	write_wav(wavOut, pcm, 2, 44100);
	size_t nf = pcm.size() / 2;
	int peak = 0;
	for (s16 s : pcm) peak = std::max(peak, std::abs((int)s));
	double rl = rms(pcm.data(), nf, 2), rr = rms(pcm.data() + 1, nf, 2);
	double dbfs = 20 * log10((rl + rr) / 2 / 32768.0 + 1e-12);
	double corr = 0, el = 0, er = 0;
	for (size_t i = 0; i < nf; i++) { corr += (double)pcm[2 * i] * pcm[2 * i + 1]; el += (double)pcm[2 * i] * pcm[2 * i]; er += (double)pcm[2 * i + 1] * pcm[2 * i + 1]; }
	corr /= sqrt(el * er) + 1e-9;
	// Skip the first second (intros can be quiet).
	Spectrum sp = analyze(pcm.data() + 2 * 44100, nf > 44100 ? nf - 44100 : 0, 2, 44100);
	printf("atrac: RMS L %.0f R %.0f (%.1f dBFS), peak %d, L/R correlation %.2f, spectral flatness %.3f, centroid %.0f Hz\n",
		rl, rr, dbfs, peak, corr, sp.flatness, sp.centroidHz);
	// RMS per second
	size_t clipped = 0;
	for (s16 s : pcm) clipped += s == 32767 || s == -32768 || s == -32767;
	printf("atrac: %zu of %zu samples at full scale\n", clipped, pcm.size());
	CHECK(clipped < pcm.size() / 1000, "too much clipping");
	printf("atrac: RMS per second:");
	for (size_t s = 0; s + 44100 <= nf; s += 44100) printf(" %.0f", rms(pcm.data() + 2 * s, 44100, 2));
	printf("\n");
	CHECK(nf >= 44100 * 5, "too few samples");
	CHECK(rl > 300 && rr > 300, "music is (nearly) silent");
	CHECK(peak < 32767 || dbfs < -3, "clipping everywhere");
	CHECK(sp.flatness < 0.2, "spectrum looks like noise (flatness %.3f)", sp.flatness);
	printf("atrac: wrote %s\n", wavOut);
}

// ================================================================= (b) SAS
// PSP ADPCM (VAG) encoder: per 28-sample block, the predictor/shift with the least error.
static const int vagCoef[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
static std::vector<u8> encode_vag(const std::vector<int> &x) {
	std::vector<u8> out;
	int h1 = 0, h2 = 0;
	size_t blocks = (x.size() + 27) / 28;
	for (size_t b = 0; b < blocks; b++) {
		double bestErr = 1e300;
		int bestP = 0, bestS = 0;
		u8 bestNib[28];
		int bestH1 = 0, bestH2 = 0;
		for (int p = 0; p < 5; p++) {
			for (int s = 0; s <= 12; s++) {
				int y1 = h1, y2 = h2;
				double err = 0;
				u8 nib[28];
				for (int i = 0; i < 28; i++) {
					size_t idx = b * 28 + i;
					int target = idx < x.size() ? x[idx] : 0;
					int pred = (y1 * vagCoef[p][0] + y2 * vagCoef[p][1]) >> 6;
					double q = (double)(target - pred) * (1 << s) / 4096.0;
					int v = (int)lround(q);
					v = std::max(-8, std::min(7, v));
					int dec = ((int)(s16)(v << 12) >> s) + pred;
					dec = std::max(-32768, std::min(32767, dec));
					err += (double)(target - dec) * (target - dec);
					nib[i] = (u8)(v & 0xF);
					y2 = y1;
					y1 = dec;
				}
				if (err < bestErr) { bestErr = err; bestP = p; bestS = s; memcpy(bestNib, nib, 28); bestH1 = y1; bestH2 = y2; }
			}
		}
		h1 = bestH1; h2 = bestH2;
		u8 flags = b == 0 ? 6 : (b == blocks - 1 ? 3 : 0);  // loop start / loop end
		out.push_back((u8)((bestP << 4) | bestS));
		out.push_back(flags);
		for (int i = 0; i < 28; i += 2) out.push_back((u8)(bestNib[i] | (bestNib[i + 1] << 4)));
	}
	// A final block: the decoder treats the last block as the end.
	out.push_back(0); out.push_back(7);
	for (int i = 0; i < 14; i++) out.push_back(0);
	return out;
}

static double goertzel(const s16 *p, size_t n, int stride, double freq, int rate) {
	double k = 2 * cos(2 * M_PI * freq / rate), s1 = 0, s2 = 0;
	for (size_t i = 0; i < n; i++) {
		double s0 = p[i * stride] + k * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return sqrt(s1 * s1 + s2 * s2 - k * s1 * s2) / n;
}

static void test_sas(const char *wavOut) {
	const u32 core = 0x08A00000, out = 0x08A40000, vag = 0x08A80000, vars = 0x08AF0000;
	const int grain = 256;
	u32 r = call("__sceSasInit", core, grain, 32, 0, 44100);
	CHECK(r == 0, "__sceSasInit returned %08x", r);
	CHECK(call("__sceSasGetGrain", core) == (u32)grain, "grain");

	// 441 Hz sine, exactly 100 samples per period: 28 periods = 2800 samples = 100 VAG blocks, loops cleanly.
	std::vector<int> sine(2800);
	for (size_t i = 0; i < sine.size(); i++) sine[i] = (int)lround(12000 * sin(2 * M_PI * i / 100.0));
	std::vector<u8> v = encode_vag(sine);
	memcpy(PTR(vag), v.data(), v.size());

	r = call("__sceSasSetVoice", core, 0, vag, (u32)v.size(), 1);
	CHECK(r == 0, "__sceSasSetVoice returned %08x", r);
	CHECK(call("__sceSasSetPitch", core, 0, 0x1000) == 0, "pitch");
	CHECK(call("__sceSasSetVolume", core, 0, 0x1000, 0x1000, 0, 0) == 0, "volume");
	// ADSR: linear attack over ~4 grains, (almost) no decay, flat sustain, exponential release.
	CHECK(call("__sceSasSetADSRmode", core, 0, 0xF, 0, 1, 1, 3) == 0, "adsr mode");
	CHECK(call("__sceSasSetADSR", core, 0, 0xF, 0x00100000, 1, 0, 0x00400000) == 0, "adsr");
	CHECK(call("__sceSasSetSL", core, 0, 0x38000000) == 0, "sustain level");
	CHECK((call("__sceSasGetEndFlag", core) & 1) == 1, "voice 0 should be idle before key on");
	CHECK(call("__sceSasSetKeyOn", core, 0) == 0, "key on");

	std::vector<s16> pcm;
	std::vector<double> grainRms, envHeights;
	auto mix = [&](int grains) {
		for (int g = 0; g < grains; g++) {
			u32 cr = call("__sceSasCore", core, out);
			CHECK(cr == 0, "__sceSasCore returned %08x", cr);
			const s16 *p = (const s16 *)PTR(out);
			pcm.insert(pcm.end(), p, p + grain * 2);
			grainRms.push_back(rms(p, grain, 2));
			envHeights.push_back((double)(s32)call("__sceSasGetEnvelopeHeight", core, 0));
		}
	};
	mix(80);  // ~0.46 s at the native pitch
	CHECK((call("__sceSasGetEndFlag", core) & 1) == 0, "voice 0 should be playing");

	// Same voice an octave up.
	call("__sceSasSetPitch", core, 0, 0x2000);
	size_t octaveStart = pcm.size() / 2;
	mix(40);
	// Key off: release, then the voice ends.
	call("__sceSasSetKeyOff", core, 0);
	size_t releaseStart = grainRms.size();
	mix(120);

	write_wav(wavOut, pcm, 2, 44100);

	double attack0 = grainRms[0], steady = grainRms[60];
	double at441 = goertzel(pcm.data() + 2 * 20 * grain, 40 * grain, 2, 441, 44100);
	double at882a = goertzel(pcm.data() + 2 * 20 * grain, 40 * grain, 2, 882, 44100);
	double oct882 = goertzel(pcm.data() + 2 * (octaveStart + 8 * grain), 24 * grain, 2, 882, 44100);
	double oct441 = goertzel(pcm.data() + 2 * (octaveStart + 8 * grain), 24 * grain, 2, 441, 44100);
	double lastRms = grainRms.back();
	printf("sas: grain RMS: first %.0f, steady %.0f (env %.0f), after release %.0f; end flag %08x\n", attack0, steady, envHeights[60], lastRms,
		call("__sceSasGetEndFlag", core));
	printf("sas: 441 Hz %.0f vs 882 Hz %.0f at pitch 0x1000; 882 Hz %.0f vs 441 Hz %.0f at pitch 0x2000\n", at441, at882a, oct882, oct441);
	printf("sas: grain RMS during attack:");
	for (int i = 0; i < 8; i++) printf(" %.0f", grainRms[i]);
	printf("\nsas: grain RMS during release:");
	for (size_t i = releaseStart; i < releaseStart + 12 && i < grainRms.size(); i++) printf(" %.0f", grainRms[i]);
	printf("\n");
	CHECK(steady > 1000, "SAS output is (nearly) silent");
	CHECK(attack0 < steady * 0.8, "no attack ramp (first grain %.0f, steady %.0f)", attack0, steady);
	CHECK(at441 > 10 * at882a, "native pitch should be 441 Hz");
	CHECK(oct882 > 10 * oct441, "pitch 0x2000 should be an octave up");
	CHECK(lastRms < 1.0, "release didn't end the voice");
	CHECK((call("__sceSasGetEndFlag", core) & 1) == 1, "voice 0 should have ended");

	// __sceSasCoreWithMix adds onto what's in the buffer.
	for (int i = 0; i < grain * 2; i++) ((s16 *)PTR(out))[i] = 1000;
	call("__sceSasSetKeyOn", core, 0);
	call("__sceSasSetPitch", core, 0, 0x1000);
	u32 wr = call("__sceSasCoreWithMix", core, out, 0x1000, 0x1000);
	CHECK(wr == 0, "__sceSasCoreWithMix returned %08x", wr);
	printf("sas: wrote %s\n", wavOut);
}

int main(int argc, char **argv) {
	const char *at3 = argc > 1 ? argv[1] : "/home/claude/rcsm/game/PSP_GAME/USRDIR/streaming/01a.at3";
	std::string outDir = argc > 2 ? argv[2] : "/home/claude/rcsm/build";
	void *m = mmap(nullptr, 0x40000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (m == MAP_FAILED) fatal("mmap failed");
	g_mem = (u8 *)m;
	static Thread mainThread;
	strcpy(mainThread.name, "test");
	mainThread.status = TS_RUNNING;
	g_cur = &mainThread;

	audio_hle_init();
	test_atrac(at3, (outDir + "/test_atrac.wav").c_str());
	test_sas((outDir + "/test_sas.wav").c_str());
	printf("HLE delays (Media Engine time) for 6 s of ATRAC3 + 240 SAS grains: %.1f ms\n", delayedUs / 1000.0);
	printf("%s\n", failures ? "FAILED" : "OK");
	return failures ? 1 : 0;
}
