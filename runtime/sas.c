// sceSasCore: the PSP's sound mixer, which the game plays its sound effects through.
//
// 32 voices of VAG ADPCM or 16-bit PCM, each with a pitch (linear resampling), an ADSR envelope, a dry and
// an effect send level per side; the sends go through the SPU-style reverb. One __sceSasCore call mixes a
// grain (256 samples here) into the caller's buffer. The arithmetic follows PPSSPP's SasAudio.cpp and
// SasReverb.cpp (GPL-2.0+), which matches hardware recordings, so the game sounds as it did there.
// SR2_PPSSPP_AUDIO=1 keeps PPSSPP's own implementation (runtime/audio) instead.
#include <string.h>
#include "kernel.h"
#include "media.h"

enum {
	VOICES = 32, VOL_MAX = 0x1000, MAX_GRAIN = 2048, PITCH_BASE = 0x1000, PITCH_MASK = 0xFFF, PITCH_SHIFT = 12,
	PITCH_MAX = 0x4000, HEIGHT_MAX = 0x40000000,
};
enum { CURVE_LIN_INC, CURVE_LIN_DEC, CURVE_LIN_BENT, CURVE_EXP_DEC, CURVE_EXP_INC, CURVE_DIRECT };
enum { ST_KEYON_STEP = -42, ST_KEYON = -2, ST_OFF = -1, ST_ATTACK, ST_DECAY, ST_SUSTAIN, ST_RELEASE };
enum { V_OFF, V_VAG, V_NOISE, V_TRIWAVE, V_PULSEWAVE, V_PCM, V_ATRAC3 };

#define ERR_INVALID_GRAIN 0x80420001
#define ERR_INVALID_MAX_VOICES 0x80420002
#define ERR_INVALID_OUTPUT_MODE 0x80420003
#define ERR_INVALID_SAMPLE_RATE 0x80420004
#define ERR_BAD_ADDRESS 0x80420005
#define ERR_INVALID_VOICE 0x80420010
#define ERR_INVALID_NOISE_FREQ 0x80420011
#define ERR_INVALID_PITCH 0x80420012
#define ERR_INVALID_ADSR_CURVE_MODE 0x80420013
#define ERR_INVALID_PARAMETER 0x80420014
#define ERR_INVALID_LOOP_POS 0x80420015
#define ERR_VOICE_PAUSED 0x80420016
#define ERR_INVALID_VOLUME 0x80420018
#define ERR_INVALID_ADSR_RATE 0x80420019
#define ERR_INVALID_PCM_SIZE 0x8042001A
#define ERR_REV_INVALID_TYPE 0x80420020
#define ERR_REV_INVALID_FEEDBACK 0x80420021
#define ERR_REV_INVALID_DELAY 0x80420022
#define ERR_REV_INVALID_VOLUME 0x80420023
#define ERR_ATRAC3_ALREADY_SET 0x80420040
#define ERR_ATRAC3_NOT_SET 0x80420041

static inline s16 clamp16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v; }

// ---------------------------------------------------------------- VAG (PSX ADPCM) decoding
// Rows past 4 are what the hardware reads past the end of its table.
static const u8 vag_f[16][2] = {
	{0, 0}, {60, 0}, {115, 52}, {98, 55}, {122, 60}, {0, 0}, {0, 0}, {52, 0},
	{55, 2}, {60, 125}, {0, 0}, {0, 91}, {0, 0}, {2, 216}, {125, 6}, {0, 151},
};

typedef struct {
	s16 samples[28];
	int cur;
	u32 data, read;
	int block, loop_block, nblocks;
	int s1, s2;
	bool loop, loop_next, end;
} Vag;

static void vag_start(Vag *v, u32 data, u32 size, bool loop) {
	v->loop = loop; v->loop_next = false; v->loop_block = -1; v->nblocks = size / 16;
	v->end = false; v->data = v->read = data; v->cur = 28; v->block = -1; v->s1 = v->s2 = 0;
}

static void vag_block(Vag *v, const u8 **rp) {
	if (v->block == v->nblocks - 1) { v->end = true; return; }
	const u8 *p = *rp;
	int pred = *p++, shift = pred & 15;
	pred >>= 4;
	int flags = *p++;
	if (flags == 7) { v->end = true; return; }
	if (flags == 6) v->loop_block = v->block;
	else if (flags == 3 && v->loop) v->loop_next = true;
	int s1 = v->s1, s2 = v->s2, c1 = vag_f[pred][0], c2 = -vag_f[pred][1];
	for (int i = 0; i < 28; i += 2) {
		u8 d = *p++;
		int a = (s16)((d & 15) << 12) >> shift, b = (s16)((d & 0xF0) << 8) >> shift;
		s2 = clamp16(a + ((s1 * c1 + s2 * c2) >> 6));
		s1 = clamp16(b + ((s2 * c1 + s1 * c2) >> 6));
		v->samples[i] = s2;
		v->samples[i + 1] = s1;
	}
	v->s1 = s1; v->s2 = s2; v->cur = 0; v->block++;
	*rp = p;
}

static void vag_get(Vag *v, s16 *out, int n) {
	if (v->end) { memset(out, 0, n * 2); return; }
	if (!media_valid(v->read, v->nblocks * 16)) return;
	const u8 *p = PTR(v->read), *orig = p;
	for (int i = 0; i < n; i++) {
		if (v->cur == 28) {
			if (v->loop_next) {
				v->read = v->data + 16 * v->loop_block + 16;   // (data is block -1)
				p = orig = PTR(v->read);
				v->block = v->loop_block;
				v->loop_next = false;
			}
			vag_block(v, &p);
			if (v->end) { memset(out + i, 0, (n - i) * 2); return; }
		}
		out[i] = v->samples[v->cur++];
	}
	v->read += (u32)(p - orig);
}

// ---------------------------------------------------------------- ADSR envelope
typedef struct {
	int a_rate, d_rate, s_rate, s_level, r_rate;
	int a_type, d_type, s_type, r_type;
	int state;
	s64 height;
} Env;

static int simple_rate(int n) {
	n &= 0x7F;
	if (n == 0x7F) return 0;
	int r = ((7 - (n & 3)) << 26) >> (n >> 2);
	return r ? r : 1;
}
static int exponent_rate(int n) {
	n &= 0x7F;
	if (n == 0x7F) return 0;
	int r = ((7 - (n & 3)) << 24) >> (n >> 2);
	return r ? r : 1;
}

static void env_simple(Env *e, u32 b1, u32 b2) {
	e->a_rate = simple_rate(b1 >> 8);
	e->a_type = (b1 & 0x8000) ? CURVE_LIN_BENT : CURVE_LIN_INC;
	int dn = (b1 >> 4) & 15;
	e->d_rate = dn ? (int)(0x80000000u >> dn) : 0x7FFFFFFF;
	e->d_type = CURVE_EXP_DEC;
	e->s_type = (b2 >> 14) & 3;
	e->s_rate = e->s_type == CURVE_EXP_DEC ? exponent_rate(b2 >> 6) : simple_rate(b2 >> 6);
	e->r_type = (b2 & 0x20) ? CURVE_EXP_DEC : CURVE_LIN_DEC;
	int rn = b2 & 31;
	if (rn == 31) e->r_rate = 0;
	else if (e->r_type == CURVE_LIN_DEC) e->r_rate = rn == 30 ? 0x40000000 : rn == 29 ? 1 : 0x10000000 >> rn;
	else e->r_rate = rn ? (int)(0x80000000u >> rn) : 0x7FFFFFFF;
	e->s_level = ((b1 & 15) + 1) << 26;
}

static void env_walk(Env *e, int type, int rate) {
	s64 d;
	switch (type) {
	case CURVE_LIN_INC: e->height += rate; break;
	case CURVE_LIN_DEC: e->height -= rate; break;
	case CURVE_LIN_BENT: e->height += e->height <= (s64)HEIGHT_MAX * 3 / 4 ? rate : rate / 4; break;
	case CURVE_EXP_DEC:
		d = e->height - HEIGHT_MAX;
		d += (-d * rate) >> 32;
		e->height = d + HEIGHT_MAX - (s64)(((u64)(u32)rate + 3) / 4);
		break;
	case CURVE_EXP_INC:
		d = e->height - HEIGHT_MAX;
		d += (-d * rate) >> 32;
		e->height = d + 0x4000 + HEIGHT_MAX;
		break;
	case CURVE_DIRECT: e->height = rate; break;
	}
}

static void env_state(Env *e, int s) {
	if (e->height > HEIGHT_MAX) e->height = HEIGHT_MAX;
	e->state = s;
}

static inline void env_step(Env *e) {
	switch (e->state) {
	case ST_ATTACK:
		env_walk(e, e->a_type, e->a_rate);
		if (e->height >= HEIGHT_MAX || e->height < 0) env_state(e, ST_DECAY);
		break;
	case ST_DECAY:
		env_walk(e, e->d_type, e->d_rate);
		if (e->height < e->s_level) env_state(e, ST_SUSTAIN);
		break;
	case ST_SUSTAIN:
		env_walk(e, e->s_type, e->s_rate);
		if (e->height <= 0) { e->height = 0; env_state(e, ST_RELEASE); }
		break;
	case ST_RELEASE:
		env_walk(e, e->r_type, e->r_rate);
		if (e->height <= 0) { e->height = 0; env_state(e, ST_OFF); }
		break;
	case ST_KEYON:   // the hardware holds a key-on at 0 for 32 steps before the attack
		e->height = 0;
		env_state(e, ST_KEYON_STEP);
		break;
	case ST_KEYON_STEP:
		if (++e->height >= 31) { e->height = 0; env_state(e, ST_ATTACK); }
		break;
	}
}
static int env_height(const Env *e) { return (int)(e->height > HEIGHT_MAX ? HEIGHT_MAX : e->height); }

// ---------------------------------------------------------------- reverb (the PSX SPU reverb with the PSP's presets)
typedef struct {
	s32 size;
	s16 dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL;
	s16 vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2;
	s16 dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4;
	s16 dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2;
} RevPreset;

static const RevPreset rev_presets[9] = {
	{0x26C0, 0x007D, 0x005B, 0x6D80, 0x54B8, (s16)0xBED0, 0x0000, 0x0000, (s16)0xBA80,   // Room
	 0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF,
	 0x0336, 0x01B7, 0x0335, 0x01B6, 0x0334, 0x01B5, 0x0334, 0x01B5,
	 0x0334, 0x01B5, 0x01B4, 0x0136, 0x00B8, 0x005C},
	{0x1F40, 0x0033, 0x0025, 0x70F0, 0x4FA8, (s16)0xBCE0, 0x4410, (s16)0xC0F0, (s16)0x9C00,   // Studio Small
	 0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266,
	 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7,
	 0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026},
	{0x4840, 0x00B1, 0x007F, 0x70F0, 0x4FA8, (s16)0xBCE0, 0x4510, (s16)0xBEF0, (s16)0xB4C0,   // Studio Medium
	 0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616,
	 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7,
	 0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080},
	{0x6FE0, 0x00E3, 0x00A9, 0x6F60, 0x4FA8, (s16)0xBCE0, 0x4510, (s16)0xBEF0, (s16)0xA680,   // Studio Large
	 0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973,
	 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2,
	 0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA},
	{0xADE0, 0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, (s16)0xB800, (s16)0xBC00, (s16)0xC000,   // Hall
	 0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1,
	 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD,
	 0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A},
	{0xF6C0, 0x033D, 0x0231, 0x7E00, 0x5000, (s16)0xB400, (s16)0xB000, 0x4C00, (s16)0xB000,   // Space Echo
	 0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2,
	 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D,
	 0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232},
	// Echo and Delay: rev_apply recomputes some fields from sceSasRevParam's delay and feedback
	{0x18040, 0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, (s16)0x8100,   // Echo
	 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000,
	 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE,
	 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004},
	{0x18040, 0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000,   // Delay
	 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000,
	 0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE,
	 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004},
	{0x3C00, 0x0017, 0x0013, 0x70F0, 0x4FA8, (s16)0xBCE0, 0x4510, (s16)0xBEF0, (s16)0x8500,   // Half Echo
	 0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7,
	 0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059,
	 0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014},
};

#define REV_BUF 0x20000
static s16 rev_buf[REV_BUF];
static RevPreset rev_d;
static int rev_preset = -1, rev_pos, rev_delay, rev_feedback;

static void rev_apply(void) {
	if (rev_preset < 0) return;
	rev_d = rev_presets[rev_preset];
	if (rev_preset != 6 && rev_preset != 7) return;
	int d16 = (rev_delay + 1) * 16;
	rev_d.mLSAME = (s16)(d16 * 2 - rev_d.dAPF1);
	rev_d.mRSAME = (s16)(d16 - rev_d.dAPF2);
	rev_d.mLCOMB1 = (s16)(rev_d.mRCOMB1 + d16);
	rev_d.dLSAME = (s16)(rev_d.dRSAME + d16);
	rev_d.mLAPF1 = (s16)(rev_d.mLAPF2 + d16);
	rev_d.mRAPF1 = (s16)(d16 + (rev_d.mLAPF2 >> 1));
	rev_d.vWALL = (s16)(-(rev_feedback << 8));
}
static void rev_set_preset(int p) {
	if (p >= -1 && p < 9) rev_preset = p;
	if (rev_preset != -1) { rev_pos = REV_BUF - rev_presets[rev_preset].size; memset(rev_buf, 0, sizeof rev_buf); }
	else rev_pos = 0;
	rev_apply();
}

// the work area wraps within its top `size` entries
static int rev_size;
static inline s16 *RB(int i) {
	int a = rev_pos + i;
	if (a >= REV_BUF) a -= rev_size;
	if (a < REV_BUF - rev_size) a += rev_size;
	return &rev_buf[a];
}

// `in` is the 22 kHz send mix (n stereo frames); writes 2n stereo frames at 44 kHz
static void rev_process(s16 *out, const s16 *in, int n, int vl, int vr) {
	if (rev_preset == -1) {   // off: the send, scaled, at both rates
		for (int i = 0; i < n; i++) {
			out[i * 4 + 0] = out[i * 4 + 2] = clamp16(in[i * 2] * vl >> 15);
			out[i * 4 + 1] = out[i * 4 + 3] = clamp16(in[i * 2 + 1] * vr >> 15);
		}
		return;
	}
	vl <<= 1; vr <<= 1;   // the ME's return is (evol * out) >> 11
	const RevPreset *d = &rev_d;
	rev_size = d->size;
	for (int i = 0; i < n; i++) {
		s16 L = in[i * 2] >> 2, R = in[i * 2 + 1] >> 2;   // the ME feeds the reverb a quarter of the send
		*RB(d->mLSAME) = clamp16(L + (*RB(d->dLSAME) * d->vWALL >> 15) - (*RB(d->mLSAME - 1) * d->vIIR >> 15) + *RB(d->mLSAME - 1));
		*RB(d->mRSAME) = clamp16(R + (*RB(d->dRSAME) * d->vWALL >> 15) - (*RB(d->mRSAME - 1) * d->vIIR >> 15) + *RB(d->mRSAME - 1));
		*RB(d->mLDIFF) = clamp16(L + (*RB(d->dRDIFF) * d->vWALL >> 15) - (*RB(d->mLDIFF - 1) * d->vIIR >> 15) + *RB(d->mLDIFF - 1));
		*RB(d->mRDIFF) = clamp16(R + (*RB(d->dLDIFF) * d->vWALL >> 15) - (*RB(d->mRDIFF - 1) * d->vIIR >> 15) + *RB(d->mRDIFF - 1));
		s32 Lo = (d->vCOMB1 * *RB(d->mLCOMB1) + d->vCOMB2 * *RB(d->mLCOMB2) + d->vCOMB3 * *RB(d->mLCOMB3) + d->vCOMB4 * *RB(d->mLCOMB4)) >> 15;
		s32 Ro = (d->vCOMB1 * *RB(d->mRCOMB1) + d->vCOMB2 * *RB(d->mRCOMB2) + d->vCOMB3 * *RB(d->mRCOMB3) + d->vCOMB4 * *RB(d->mRCOMB4)) >> 15;
		*RB(d->mLAPF1) = clamp16(Lo - (d->vAPF1 * *RB(d->mLAPF1 - d->dAPF1) >> 15));
		Lo = *RB(d->mLAPF1 - d->dAPF1) + (*RB(d->mLAPF1) * d->vAPF1 >> 15);
		*RB(d->mRAPF1) = clamp16(Ro - (d->vAPF1 * *RB(d->mRAPF1 - d->dAPF1) >> 15));
		Ro = *RB(d->mRAPF1 - d->dAPF1) + (*RB(d->mRAPF1) * d->vAPF1 >> 15);
		*RB(d->mLAPF2) = clamp16(Lo - (d->vAPF2 * *RB(d->mLAPF2 - d->dAPF2) >> 15));
		Lo = *RB(d->mLAPF2 - d->dAPF2) + (*RB(d->mLAPF2) * d->vAPF2 >> 15);
		*RB(d->mRAPF2) = clamp16(Ro - (d->vAPF2 * *RB(d->mRAPF2 - d->dAPF2) >> 15));
		Ro = *RB(d->mRAPF2 - d->dAPF2) + (*RB(d->mRAPF2) * d->vAPF2 >> 15);
		out[i * 4 + 0] = clamp16((Lo * vl) >> 15);
		out[i * 4 + 1] = clamp16((Ro * vr) >> 15);
		out[i * 4 + 2] = out[i * 4 + 3] = 0;
		if (++rev_pos >= REV_BUF) rev_pos -= rev_size;
	}
}

// ---------------------------------------------------------------- voices and mixing
typedef struct {
	bool playing, paused, on;
	int type;
	u32 vag_addr, vag_size, pcm_addr;
	int pcm_size, pcm_index, pcm_loop;
	u32 frac;
	int pitch;
	bool loop;
	int noise_freq;
	int vol_l, vol_r, fx_l, fx_r;
	s16 hist[2];
	Env env;
	Vag vag;
} Voice;

static struct {
	bool inited;
	int grain, output_mode;
	int fx_type, fx_delay, fx_feedback, fx_l, fx_r;
	bool dry, wet;
	Voice v[VOICES];
	s32 mix[MAX_GRAIN * 2], send[MAX_GRAIN * 2];
	s16 send_down[MAX_GRAIN], send_out[MAX_GRAIN * 2];
	s16 temp[MAX_GRAIN * 4 + 2 + 16];
} S;

static void voice_read(Voice *v, s16 *out, int n) {
	switch (v->type) {
	case V_VAG: vag_get(&v->vag, out, n); break;
	case V_PCM: {
		while (n > 0) {
			if (!v->on) { v->pcm_index = 0; break; }
			int k = v->pcm_size - v->pcm_index < n ? v->pcm_size - v->pcm_index : n;
			if (media_valid(v->pcm_addr + v->pcm_index * 2, k * 2)) memcpy(out, PTR(v->pcm_addr + v->pcm_index * 2), k * 2);
			else memset(out, 0, k * 2);
			v->pcm_index += k; n -= k; out += k;
			if (v->pcm_index >= v->pcm_size) {
				if (!v->loop) break;
				v->pcm_index = v->pcm_loop;
			}
		}
		if (n > 0) memset(out, 0, n * 2);
		break;
	}
	default: memset(out, 0, n * 2); break;   // (noise and the waves aren't generated, as in PPSSPP)
	}
}

static bool voice_ended(const Voice *v) {
	return v->type == V_VAG ? v->vag.end : v->type == V_PCM ? v->pcm_index >= v->pcm_size : false;
}

static void mix_voice(Voice *v) {
	if ((v->type == V_VAG && !v->vag_addr) || (v->type == V_PCM && !v->pcm_addr)) return;
	int grain = S.grain, delay = 0;
	bool keyon = v->env.state == ST_KEYON;
	if (keyon) {   // the first 32 samples (in source samples) after a key-on are silent
		delay = v->type == V_PCM && v->pitch > PITCH_BASE ? 32 : (int)((32 * (u32)v->pitch) >> PITCH_SHIFT);
		if (v->type == V_VAG) delay++;
	}
	S.temp[0] = v->hist[0];
	S.temp[1] = v->hist[1];
	u32 frac = v->frac;
	int pitch = v->pitch;
	int nread = (int)((frac + (u32)pitch * (u32)(grain - delay > 0 ? grain - delay : 0)) >> PITCH_SHIFT);
	if (nread > (int)(sizeof S.temp / 2) - 2) nread = (int)(sizeof S.temp / 2) - 2;
	int pos = 2;
	if (keyon) { pos = 0; nread += 2; }
	voice_read(v, &S.temp[pos], nread);
	int end = pos + nread;
	for (int i = 0; i < delay; i++) env_step(&v->env);
	bool interp = pitch != PITCH_BASE || (frac & PITCH_MASK);
	for (int i = delay; i < grain; i++) {
		const s16 *s = S.temp + (frac >> PITCH_SHIFT);
		int smp = s[0];
		if (interp) { int f = frac & PITCH_MASK; smp = s[0] - (((s[0] - s[1]) * f) >> PITCH_SHIFT); }
		frac += pitch;
		int e = env_height(&v->env);
		env_step(&v->env);
		e = (e + (1 << 14)) >> 15;   // 30-bit height to 15 bits, rounded
		smp = (smp * e + (1 << 14)) >> 15;
		S.mix[i * 2] += (smp * v->vol_l) >> 12;
		S.mix[i * 2 + 1] += (smp * v->vol_r) >> 12;
		S.send[i * 2] += smp * v->fx_l >> 12;
		S.send[i * 2 + 1] += smp * v->fx_r >> 12;
	}
	v->hist[0] = S.temp[end - 2];
	v->hist[1] = S.temp[end - 1];
	v->frac = frac - (u32)(end - 2) * PITCH_BASE;
	if (voice_ended(v)) { v->env.state = ST_OFF; v->env.height = 0; }
	if (v->env.state == ST_OFF) { v->playing = false; v->on = false; }
}

static void sas_mix(u32 out_addr, u32 in_addr, int lvol, int rvol) {
	int g = S.grain;
	for (int i = 0; i < VOICES; i++)
		if (S.v[i].playing && !S.v[i].paused) mix_voice(&S.v[i]);
	if (!media_valid(out_addr, 4 * g)) {
		LOGE("sas: bad output address %08x", out_addr);
	} else if (S.output_mode == 0) {
		if (S.wet) {   // the send, at 22 kHz (every other frame), through the reverb
			for (int i = 0; i < g / 2; i++) {
				S.send_down[i * 2] = clamp16(S.send[i * 4]);
				S.send_down[i * 2 + 1] = clamp16(S.send[i * 4 + 1]);
			}
			rev_process(S.send_out, S.send_down, g / 2, (u16)(S.fx_l << 3), (u16)(S.fx_r << 3));
		}
		s16 *o = (s16 *)PTR(out_addr);
		const s16 *in = in_addr && media_valid(in_addr, 4 * g) ? (const s16 *)PTR(in_addr) : NULL;
		for (int i = 0; i < g * 2; i += 2) {
			int l = 0, r = 0;
			if (in) { l = in[i] * lvol >> 12; r = in[i + 1] * rvol >> 12; }
			if (S.dry) { l += S.mix[i]; r += S.mix[i + 1]; }
			if (S.wet) { l += S.send_out[i]; r += S.send_out[i + 1]; }
			o[i] = clamp16(l);
			o[i + 1] = clamp16(r);
		}
	} else {   // raw: dry L, dry R, send L, send R, each a grain long
		s16 *o = (s16 *)PTR(out_addr);
		for (int i = 0; i < g; i++) {
			o[i] = clamp16(S.mix[i * 2]);
			o[g + i] = clamp16(S.mix[i * 2 + 1]);
			o[2 * g + i] = clamp16(S.send[i * 2]);
			o[3 * g + i] = clamp16(S.send[i * 2 + 1]);
		}
	}
	memset(S.mix, 0, sizeof(s32) * g * 2);
	memset(S.send, 0, sizeof(s32) * g * 2);
}

// The Media Engine's time for one grain (measured by PPSSPP on a PSP); the caller waits for it.
static int mix_us(void) {
	float us = 110.0f + 0.49f * S.grain;
	for (int i = 0; i < VOICES; i++) {
		const Voice *v = &S.v[i];
		if (!v->playing || v->paused) continue;
		float r = v->pitch / (float)PITCH_BASE;
		us += S.grain * (v->type == V_PCM ? 0.405f + 0.0675f * r : v->type == V_NOISE || v->type == V_TRIWAVE || v->type == V_PULSEWAVE ? 0.41f : 0.445f + 0.0675f * r);
	}
	if (S.fx_type >= 0 && S.wet) us += 0.64f * S.grain;
	return (int)us;
}

// ---------------------------------------------------------------- HLE
#define VOICE_OR_ERR(n) if ((u32)(n) >= VOICES) { RET(ERR_INVALID_VOICE); return; } Voice *v = &S.v[n]

static void voice_keyon(Voice *v) {
	v->env.state = ST_KEYON;
	if (v->type == V_VAG) {
		if (!media_valid(v->vag_addr, 16)) { LOGE("sas: bad VAG address %08x", v->vag_addr); return; }
		vag_start(&v->vag, v->vag_addr, v->vag_size, v->loop);
	}
	v->playing = v->on = true;
	v->paused = false;
	v->frac = 0;
}

static void h_Init(Ctx *c) {
	u32 core = A(0), grain = A(1), maxv = A(2), mode = A(3), rate = A(4);
	if (!media_valid(core, 4) || (core & 0x3F)) { RET(ERR_BAD_ADDRESS); return; }
	if (maxv == 0 || maxv > VOICES) { RET(ERR_INVALID_MAX_VOICES); return; }
	if (grain < 0x40 || grain > 0x800 || (grain & 0x1F)) { RET(ERR_INVALID_GRAIN); return; }
	if (mode > 1) { RET(ERR_INVALID_OUTPUT_MODE); return; }
	if (rate != 44100) { RET(ERR_INVALID_SAMPLE_RATE); return; }
	S.grain = grain;
	S.output_mode = mode;
	for (int i = 0; i < VOICES; i++) { S.v[i].playing = false; S.v[i].loop = false; }
	LOGI("sas: grain %u, %s output", grain, mode ? "raw" : "mixed");
	RET(0);
}
static void core_done(Ctx *c, u32 r) {
	RET(r);
	if (!g_in_intr) media_wait(mix_us());
}
static void h_Core(Ctx *c) {
	if (!media_valid(A(1), 4)) { RET(ERR_INVALID_PARAMETER); return; }
	sas_mix(A(1), 0, 0, 0);
	core_done(c, 0);
}
static void h_CoreWithMix(Ctx *c) {
	if (!media_valid(A(1), 4)) { RET(ERR_INVALID_PARAMETER); return; }
	if (S.output_mode == 1) { RET(0x80020065); return; }   // SCE_KERNEL_ERROR_BAD_ARGUMENT
	sas_mix(A(1), A(1), (int)A(2), (int)A(3));
	core_done(c, 0);
}
static void h_GetEndFlag(Ctx *c) {
	u32 f = 0;
	for (int i = 0; i < VOICES; i++) if (!S.v[i].playing) f |= 1u << i;
	RET(f);
}
static void h_SetVolume(Ctx *c) {
	VOICE_OR_ERR(A(1));
	int l = (int)A(2), r = (int)A(3), el = (int)A(4), er = (int)A(5);
	if (abs(l) > VOL_MAX || abs(r) > VOL_MAX || abs(el) > VOL_MAX || abs(er) > VOL_MAX) { RET(ERR_INVALID_VOLUME); return; }
	v->vol_l = l; v->vol_r = r; v->fx_l = el; v->fx_r = er;
	RET(0);
}
static void h_SetPitch(Ctx *c) {
	VOICE_OR_ERR(A(1));
	int p = (int)A(2);
	if (p < 0 || p > PITCH_MAX) { RET(ERR_INVALID_PITCH); return; }
	v->pitch = p;
	RET(0);
}
static void h_SetVoice(Ctx *c) {
	VOICE_OR_ERR(A(1));
	u32 addr = A(2);
	int size = (int)A(3), loop = (int)A(4);
	if (size == 0 || (size & 0xF)) { RET(ERR_INVALID_PARAMETER); return; }
	if (loop != 0 && loop != 1) { RET(ERR_INVALID_LOOP_POS); return; }
	if (!media_valid(addr, 1)) { RET(0); return; }
	if (v->type == V_ATRAC3) { RET(ERR_ATRAC3_ALREADY_SET); return; }
	if (size < 0) size = 0;
	bool reset = v->type != V_VAG || v->vag_addr != addr || v->vag_size != (u32)size || v->loop != (loop != 0);
	v->type = V_VAG;
	v->vag_addr = addr; v->vag_size = size; v->loop = loop != 0;
	if (v->on) v->playing = true;
	if (reset) vag_start(&v->vag, addr, size, loop != 0);
	RET(0);
}
static void h_SetVoicePCM(Ctx *c) {
	VOICE_OR_ERR(A(1));
	u32 addr = A(2);
	int size = (int)A(3), loop = (int)A(4);
	if (size <= 0 || size > 0x10000) { RET(ERR_INVALID_PCM_SIZE); return; }
	if (loop >= size) { RET(ERR_INVALID_LOOP_POS); return; }
	if (!media_valid(addr, 1)) { RET(0); return; }
	if (v->type == V_ATRAC3) { RET(ERR_ATRAC3_ALREADY_SET); return; }
	v->type = V_PCM; v->pcm_addr = addr; v->pcm_size = size; v->pcm_index = 0;
	v->pcm_loop = loop >= 0 ? loop : 0; v->loop = loop >= 0; v->playing = true;
	RET(0);
}
static void h_SetNoise(Ctx *c) {
	VOICE_OR_ERR(A(1));
	int f = (int)A(2);
	if (f < 0 || f >= 64) { RET(ERR_INVALID_NOISE_FREQ); return; }
	v->type = V_NOISE; v->noise_freq = f;
	RET(0);
}
static void h_SetADSR(Ctx *c) {
	VOICE_OR_ERR(A(1));
	int flag = (int)A(2), a = (int)A(3), d = (int)A(4), s = (int)A(5), r = (int)A(6);
	int bad = (a < 0 ? 1 : 0) | (d < 0 ? 2 : 0) | (s < 0 ? 4 : 0) | (r < 0 ? 8 : 0);
	if (bad & flag) { RET(ERR_INVALID_ADSR_RATE); return; }
	if (flag & 1) v->env.a_rate = a;
	if (flag & 2) v->env.d_rate = d;
	if (flag & 4) v->env.s_rate = s;
	if (flag & 8) v->env.r_rate = r;
	RET(0);
}
static void h_SetADSRMode(Ctx *c) {
	VOICE_OR_ERR(A(1));
	int flag = (int)A(2), a = (int)(A(3) & 0x7FFFFFFF), d = (int)(A(4) & 0x7FFFFFFF), s = (int)(A(5) & 0x7FFFFFFF), r = (int)(A(6) & 0x7FFFFFFF);
	int bad = 0;
	if (a > 5 || (a & 1)) bad |= 1;
	if (d > 5 || !(d & 1)) bad |= 2;
	if (s > 5) bad |= 4;
	if (r > 5 || !(r & 1)) bad |= 8;
	if (bad & flag) { RET(ERR_INVALID_ADSR_CURVE_MODE); return; }
	if (flag & 1) v->env.a_type = a;
	if (flag & 2) v->env.d_type = d;
	if (flag & 4) v->env.s_type = s;
	if (flag & 8) v->env.r_type = r;
	RET(0);
}
static void h_SetSL(Ctx *c) { VOICE_OR_ERR(A(1)); v->env.s_level = (int)A(2); RET(0); }
static void h_SetSimpleADSR(Ctx *c) {
	VOICE_OR_ERR(A(1));
	if ((A(3) >> 13) & 1) { RET(ERR_INVALID_ADSR_CURVE_MODE); return; }
	env_simple(&v->env, A(2) & 0xFFFF, A(3) & 0xFFFF);
	RET(0);
}
static void h_GetEnvelopeHeight(Ctx *c) { VOICE_OR_ERR(A(1)); RET(env_height(&v->env)); }
static void h_GetAllEnvelopeHeights(Ctx *c) {
	if (!media_valid(A(1), VOICES * 4)) { RET(ERR_INVALID_PARAMETER); return; }
	for (int i = 0; i < VOICES; i++) SW(A(1) + i * 4, env_height(&S.v[i].env));
	RET(0);
}
static void h_SetKeyOn(Ctx *c) {
	VOICE_OR_ERR(A(1));
	if (v->paused || v->on) { RET(ERR_VOICE_PAUSED); return; }
	voice_keyon(v);
	RET(0);
}
static void h_SetKeyOff(Ctx *c) {
	VOICE_OR_ERR(A(1));
	if (v->paused || !v->on) { RET(ERR_VOICE_PAUSED); return; }
	v->on = false;
	env_state(&v->env, ST_RELEASE);
	RET(0);
}
static void h_SetPause(Ctx *c) {
	u32 bits = A(1);
	for (int i = 0; bits && i < VOICES; i++, bits >>= 1)
		if (bits & 1) S.v[i].paused = A(2) != 0;
	RET(0);
}
static void h_GetPauseFlag(Ctx *c) {
	u32 f = 0;
	for (int i = 0; i < VOICES; i++) if (S.v[i].paused) f |= 1u << i;
	RET(f);
}
static void h_RevType(Ctx *c) {
	int t = (int)A(1);
	if (t < -1 || t > 8) { RET(ERR_REV_INVALID_TYPE); return; }
	if (t != S.fx_type) { S.fx_type = t; rev_set_preset(t); }
	RET(0);
}
static void h_RevParam(Ctx *c) {
	int d = (int)A(1), f = (int)A(2);
	if (d < 0 || d >= 128) { RET(ERR_REV_INVALID_DELAY); return; }
	if (f < 0 || f >= 128) { RET(ERR_REV_INVALID_FEEDBACK); return; }
	S.fx_delay = rev_delay = d; S.fx_feedback = rev_feedback = f;
	rev_apply();
	RET(0);
}
static void h_RevEVOL(Ctx *c) {
	if (A(1) > 0x1000 || A(2) > 0x1000) { RET(ERR_REV_INVALID_VOLUME); return; }
	S.fx_l = A(1); S.fx_r = A(2);
	RET(0);
}
static void h_RevVON(Ctx *c) { S.dry = A(1) != 0; S.wet = A(2) != 0; RET(0); }
static void h_GetGrain(Ctx *c) { RET(S.grain); }
static void h_SetGrain(Ctx *c) {
	u32 g = A(1);
	if (g < 0x40 || g > 0x800 || (g & 0x1F)) { RET(ERR_INVALID_GRAIN); return; }
	S.grain = g;
	memset(S.mix, 0, sizeof S.mix); memset(S.send, 0, sizeof S.send);
	RET(0);
}
static void h_GetOutputMode(Ctx *c) { RET(S.output_mode); }
static void h_SetOutputMode(Ctx *c) {
	if (A(1) > 1) { RET(ERR_INVALID_OUTPUT_MODE); return; }
	S.output_mode = A(1);
	RET(0);
}
static void h_Unsupported(Ctx *c) { LOGE("sas: unsupported call (wave or ATRAC3 voice)"); RET(0); }

static const HleEnt kSas[] = {
	{0x42778A9F, "__sceSasInit", h_Init},
	{0xA3589D81, "__sceSasCore", h_Core},
	{0x50A14DFC, "__sceSasCoreWithMix", h_CoreWithMix},
	{0x68A46B95, "__sceSasGetEndFlag", h_GetEndFlag},
	{0x440CA7D8, "__sceSasSetVolume", h_SetVolume},
	{0xAD84D37F, "__sceSasSetPitch", h_SetPitch},
	{0x99944089, "__sceSasSetVoice", h_SetVoice},
	{0xB7660A23, "__sceSasSetNoise", h_SetNoise},
	{0x019B25EB, "__sceSasSetADSR", h_SetADSR},
	{0x9EC3676A, "__sceSasSetADSRmode", h_SetADSRMode},
	{0x5F9529F6, "__sceSasSetSL", h_SetSL},
	{0x74AE582A, "__sceSasGetEnvelopeHeight", h_GetEnvelopeHeight},
	{0xCBCD4F79, "__sceSasSetSimpleADSR", h_SetSimpleADSR},
	{0xA0CF2FA4, "__sceSasSetKeyOff", h_SetKeyOff},
	{0x76F01ACA, "__sceSasSetKeyOn", h_SetKeyOn},
	{0xF983B186, "__sceSasRevVON", h_RevVON},
	{0xD5A229C9, "__sceSasRevEVOL", h_RevEVOL},
	{0x33D4AB37, "__sceSasRevType", h_RevType},
	{0x267A6DD2, "__sceSasRevParam", h_RevParam},
	{0x2C8E6AB3, "__sceSasGetPauseFlag", h_GetPauseFlag},
	{0x787D04D5, "__sceSasSetPause", h_SetPause},
	{0xA232CBE6, "__sceSasSetTrianglarWave", h_Unsupported},
	{0xD5EBBBCD, "__sceSasSetSteepWave", h_Unsupported},
	{0xBD11B7C2, "__sceSasGetGrain", h_GetGrain},
	{0xD1E0A01E, "__sceSasSetGrain", h_SetGrain},
	{0xE175EF66, "__sceSasGetOutputmode", h_GetOutputMode},
	{0xE855BF76, "__sceSasSetOutputmode", h_SetOutputMode},
	{0x07F58C24, "__sceSasGetAllEnvelopeHeights", h_GetAllEnvelopeHeights},
	{0xE1CD9561, "__sceSasSetVoicePCM", h_SetVoicePCM},
	{0x4AA9EAD6, "__sceSasSetVoiceATRAC3", h_Unsupported},
	{0x7497EA85, "__sceSasConcatenateATRAC3", h_Unsupported},
	{0xF6107F00, "__sceSasUnsetATRAC3", h_Unsupported},
};

void sas_init(void) {
	for (int i = 0; i < VOICES; i++) {
		Voice *v = &S.v[i];
		v->pitch = PITCH_BASE;
		v->vol_l = v->vol_r = v->fx_l = v->fx_r = VOL_MAX;
		v->env.state = ST_OFF;
		v->env.d_type = v->env.s_type = v->env.r_type = CURVE_LIN_DEC;
		v->vag.cur = 28;
		v->vag.end = true;
	}
	S.fx_type = -1;
	S.dry = true;
	hle_register(kSas, (int)(sizeof kSas / sizeof kSas[0]));
}
