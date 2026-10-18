// sceAudio channel output (sceAudioChReserve / sceAudioOutput*Blocking): up to 8 hardware channels,
// mixed on a shared 44.1 kHz timeline and handed to the host audio queue.
//
// Each channel writes ahead of the playback position by a couple of blocks and is paced against the
// wall clock, the way the PSP blocks a channel until its previous block has played. Everything behind
// the wall-clock position is final and is flushed to the host.
#define _GNU_SOURCE
#include "kernel.h"
#include <pthread.h>

#define NCH 8
#define RING 32768                   // frames, power of two
#define RATE 44100
#define LEAD 2048                    // frames a channel may run ahead of playback

typedef struct { bool used; u32 samples; u32 fmt; s32 vl, vr; u64 pos; } AudioCh;
static AudioCh ch[NCH];
static s32 ring[RING][2];
static u64 flushed;                  // frames already handed to the host
static u64 t0;                       // wall-clock time of frame 0
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;

static u64 wall_pos(void) {
	if (!t0) t0 = now_us();
	return (now_us() - t0) * RATE / 1000000;
}

static void flush_to(u64 upto) {
	extern void fe_audio_push(const s16 *samples, int frames);
	static s16 out[1024 * 2];
	if (upto > flushed + RING) flushed = upto - RING;   // fell far behind: skip
	while (flushed < upto) {
		int n = upto - flushed > 1024 ? 1024 : (int)(upto - flushed);
		for (int i = 0; i < n; i++) {
			s32 *f = ring[(flushed + i) & (RING - 1)];
			for (int k = 0; k < 2; k++) out[i * 2 + k] = f[k] > 32767 ? 32767 : f[k] < -32768 ? -32768 : f[k];
			f[0] = f[1] = 0;
		}
		fe_audio_push(out, n);
		static FILE *dump; static int dump_init;
		if (!dump_init) { dump_init = 1; const char *e = getenv("SR2_AUDIO_DUMP"); if (e) dump = fopen(e, "wb"); }   // raw s16 stereo 44.1 kHz
		if (dump) fwrite(out, 4, n, dump);
		flushed += n;
	}
}

// mix one block into the timeline; returns the frame the channel's block ends at
static u64 mix_block(AudioCh *c, u32 buf, s32 vl, s32 vr) {
	u64 now = wall_pos();
	flush_to(now);
	if (c->pos < now) c->pos = now;
	if (c->pos + c->samples > flushed + RING) c->pos = flushed + RING - c->samples;
	const s16 *s = (const s16 *)PTR(buf);
	bool mono = c->fmt == 0x10;
	for (u32 i = 0; i < c->samples; i++) {
		s32 l = mono ? s[i] : s[i * 2], r = mono ? s[i] : s[i * 2 + 1];
		s32 *f = ring[(c->pos + i) & (RING - 1)];
		f[0] += l * vl >> 15;
		f[1] += r * vr >> 15;
	}
	c->pos += c->samples;
	return c->pos;
}

static AudioCh *getch(u32 n) { return n < NCH && ch[n].used ? &ch[n] : NULL; }

static void output(Ctx *c, u32 n, s32 vl, s32 vr, u32 buf, bool blocking) {
	pthread_mutex_lock(&mx);
	AudioCh *a = getch(n);
	if (!a) { pthread_mutex_unlock(&mx); RET(0x80260002); return; }
	if (vl >= 0) a->vl = vl & 0xFFFF;
	if (vr >= 0) a->vr = vr & 0xFFFF;
	u64 end = mix_block(a, buf, a->vl, a->vr);
	u32 samples = a->samples;
	pthread_mutex_unlock(&mx);
	if (blocking && end > LEAD) thread_delay_until(t0 + (end - LEAD) * 1000000 / RATE);
	RET(samples);
}

static void h_ChReserve(Ctx *c) {
	s32 n = (s32)A(0);
	u32 samples = A(1), fmt = A(2);
	pthread_mutex_lock(&mx);
	if (n < 0) for (n = NCH - 1; n >= 0 && ch[n].used; n--) ;
	if (n < 0 || n >= NCH || ch[n].used || !samples || samples > 0xFFC0 || (samples & 63)) {
		pthread_mutex_unlock(&mx);
		LOGI("sceAudioChReserve(%d, %u, %x) failed", (s32)A(0), samples, fmt);
		RET(0x80260002);
		return;
	}
	ch[n] = (AudioCh){true, samples, fmt, 0x8000, 0x8000, 0};
	pthread_mutex_unlock(&mx);
	LOGD("sceAudioChReserve -> channel %d, %u samples, %s", n, samples, fmt == 0x10 ? "mono" : "stereo");
	RET(n);
}
static void h_ChRelease(Ctx *c) { AudioCh *a = getch(A(0)); if (a) a->used = false; RET(a ? 0 : 0x80260002); }
static void h_OutputBlocking(Ctx *c) { output(c, A(0), A(1), A(1), A(2), true); }
static void h_OutputPannedBlocking(Ctx *c) { output(c, A(0), A(1), A(2), A(3), true); }
static void h_Output(Ctx *c) { output(c, A(0), A(1), A(1), A(2), false); }
static void h_OutputPanned(Ctx *c) { output(c, A(0), A(1), A(2), A(3), false); }
static void h_GetChannelRestLength(Ctx *c) {
	AudioCh *a = getch(A(0));
	if (!a) { RET(0x80260002); return; }
	u64 now = wall_pos();
	RET(a->pos > now ? (u32)(a->pos - now > a->samples ? a->samples : a->pos - now) : 0);
}
static void h_SetChannelDataLen(Ctx *c) { AudioCh *a = getch(A(0)); if (a) a->samples = A(1); RET(a ? 0 : 0x80260002); }
static void h_ChangeChannelConfig(Ctx *c) { AudioCh *a = getch(A(0)); if (a) a->fmt = A(1); RET(a ? 0 : 0x80260002); }
static void h_ChangeChannelVolume(Ctx *c) {
	AudioCh *a = getch(A(0));
	if (!a) { RET(0x80260002); return; }
	a->vl = A(1) & 0xFFFF; a->vr = A(2) & 0xFFFF;
	RET(0);
}

void audio_ch_init(void) {
	static const HleEnt ents[] = {
		{0x5EC81C55, "sceAudioChReserve", h_ChReserve},
		{0x6FC46853, "sceAudioChRelease", h_ChRelease},
		{0x136CAF51, "sceAudioOutputBlocking", h_OutputBlocking},
		{0x13F592BC, "sceAudioOutputPannedBlocking", h_OutputPannedBlocking},
		{0x8C1009B2, "sceAudioOutput", h_Output},
		{0xE2D56B2D, "sceAudioOutputPanned", h_OutputPanned},
		{0xB011922F, "sceAudioGetChannelRestLength", h_GetChannelRestLength},
		{0xE9D97901, "sceAudioGetChannelRestLen", h_GetChannelRestLength},
		{0xCB2E439E, "sceAudioSetChannelDataLen", h_SetChannelDataLen},
		{0x95FD0C2D, "sceAudioChangeChannelConfig", h_ChangeChannelConfig},
		{0xB7E1D8E7, "sceAudioChangeChannelVolume", h_ChangeChannelVolume},
	};
	hle_register(ents, sizeof ents / sizeof ents[0]);
}
