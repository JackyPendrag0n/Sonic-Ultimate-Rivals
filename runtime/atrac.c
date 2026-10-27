// sceAtrac3plus: the PSP's ATRAC3/ATRAC3+ player, which the game streams its music through.
//
// The game loads each whole track into memory and hands it over with sceAtracSetDataAndGetID, then pulls
// one 2048-sample frame at a time with sceAtracDecodeData. Frames are decoded with FFmpeg's libavcodec.
// The bookkeeping (first valid sample, frames skipped at the start and after a loop, end and loop points,
// error codes, Media Engine time) follows PPSSPP's AtracCtx2.cpp and sceAtrac.cpp (GPL-2.0+), which were
// checked against hardware tests. Only the "all data loaded" buffering the game uses is supported; the
// streaming modes fail with a logged error. SR2_PPSSPP_AUDIO=1 keeps PPSSPP's own implementation.
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include "kernel.h"
#include "media.h"

#define ERR_API_FAIL 0x80630002
#define ERR_NO_ATRACID 0x80630003
#define ERR_BAD_ATRACID 0x80630005
#define ERR_UNKNOWN_FORMAT 0x80630006
#define ERR_BAD_CODEC_PARAMS 0x80630008
#define ERR_SIZE_TOO_SMALL 0x80630011
#define ERR_BAD_ALIGNMENT 0x80630014
#define ERR_BAD_SAMPLE 0x80630015
#define ERR_BAD_FIRST_RESET_SIZE 0x80630016
#define ERR_BAD_SECOND_RESET_SIZE 0x80630017
#define ERR_NO_LOOP_INFORMATION 0x80630021
#define ERR_ALL_DATA_DECODED 0x80630024
#define ERR_ILLEGAL_ADDR 0x800200D3
#define ERR_INVALID_POINTER 0x80000103

enum { AT3PLUS = 0x1000, AT3 = 0x1001, CONTEXTS = 6, MAX_SPF = 2048 };
enum { ALLDATA_IS_ON_MEMORY = -1 };

typedef struct {
	bool used;
	int codec, chans, sample_size;
	u32 buffer, buffer_bytes;
	int first_valid, end_sample, loop_start, loop_end, loop_num, decode_pos, skip_frames;
	int data_off, cur_off, data_end;
	u32 codec_err;
	int setdata_skipped;
	AVCodecContext *cc;
	AVPacket *pkt;
	AVFrame *frame;
} Atrac;

// libatrac3plus starts out with two contexts for each codec, in this order.
static const int kTypes[CONTEXTS] = {AT3PLUS, AT3PLUS, AT3, AT3, 0, 0};
static Atrac ctx[CONTEXTS];

static int spf(const Atrac *a) { return a->codec == AT3PLUS ? 2048 : 1024; }
static int skip_samples(const Atrac *a) { return a->codec == AT3PLUS ? 0x170 : 0x45; }

// ---------------------------------------------------------------- decoding (libavcodec)
static void dec_close(Atrac *a) {
	avcodec_free_context(&a->cc);
	av_packet_free(&a->pkt);
	av_frame_free(&a->frame);
}

static bool dec_open(Atrac *a, const u8 *extra, int extra_size) {
	const AVCodec *codec = avcodec_find_decoder(a->codec == AT3PLUS ? AV_CODEC_ID_ATRAC3P : AV_CODEC_ID_ATRAC3);
	if (!codec) { LOGE("atrac: this FFmpeg has no %s decoder", a->codec == AT3PLUS ? "ATRAC3+" : "ATRAC3"); return false; }
	a->cc = avcodec_alloc_context3(codec);
	a->pkt = av_packet_alloc();
	a->frame = av_frame_alloc();
	if (!a->cc || !a->pkt || !a->frame) return false;
	a->cc->sample_rate = 44100;
	a->cc->block_align = a->sample_size;
	av_channel_layout_default(&a->cc->ch_layout, a->chans);
	if (extra_size) {
		a->cc->extradata = av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
		memcpy(a->cc->extradata, extra, extra_size);
		a->cc->extradata_size = extra_size;
	}
	if (avcodec_open2(a->cc, codec, NULL) < 0) { LOGE("atrac: decoder setup failed"); return false; }
	return true;
}

static inline s16 clamp16(float f) {
	if (f >= 1.0f) return 32767;
	if (f <= -1.0f) return -32767;
	return (s16)(int)(f * 32767.0f);
}

// One frame of the packet at `in` into `out` as interleaved stereo (spf samples).
static bool dec_frame(Atrac *a, const u8 *in, s16 *out) {
	av_packet_unref(a->pkt);
	if (av_new_packet(a->pkt, a->sample_size) < 0) return false;
	memcpy(a->pkt->data, in, a->sample_size);
	if (avcodec_send_packet(a->cc, a->pkt) < 0) return false;
	if (avcodec_receive_frame(a->cc, a->frame) < 0) return false;
	const AVFrame *f = a->frame;
	int n = f->nb_samples < spf(a) ? f->nb_samples : spf(a);
	const float *l = (const float *)f->extended_data[0];
	const float *r = a->chans > 1 ? (const float *)f->extended_data[1] : l;
	bool planar = f->format == AV_SAMPLE_FMT_FLTP;
	for (int i = 0; i < n; i++) {
		out[2 * i] = clamp16(planar ? l[i] : l[i * a->chans]);
		out[2 * i + 1] = clamp16(planar ? r[i] : l[i * a->chans + (a->chans > 1)]);
	}
	if (n < spf(a)) memset(out + 2 * n, 0, (spf(a) - n) * 4);
	av_frame_unref(a->frame);
	return true;
}

// ---------------------------------------------------------------- the player
static int skip_frames_at(const Atrac *a, int pos) { return (pos & (spf(a) - 1)) < skip_samples(a) ? 2 : 1; }

static int file_offset_at(const Atrac *a, int pos) {
	int off = (pos / spf(a) - 1) * a->sample_size;
	if ((pos & (spf(a) - 1)) < skip_samples(a) && off != 0) off -= a->sample_size;
	return off + a->data_off;
}

static int next_samples(const Atrac *a) {
	int mask = spf(a) - 1;
	int over = (a->decode_pos | mask) - a->end_sample;
	int adjusted = (a->decode_pos & mask) + (over > 0 ? over : 0);
	return spf(a) - adjusted > 0 ? spf(a) - adjusted : 0;
}

// Decodes the next frame. While frames are being skipped (after setting the data, a seek or a loop) the
// result is thrown away; otherwise *n samples of `out` are valid.
static u32 decode_one(Atrac *a, s16 *out, int *n, int *finish) {
	int want = next_samples(a);
	if (a->cur_off + a->sample_size > a->data_end || a->decode_pos > a->end_sample) { *finish = 1; return ERR_ALL_DATA_DECODED; }
	u32 in = a->buffer + a->cur_off;
	if (!media_valid(in, a->sample_size)) { LOGE("atrac: bad input address %08x", in); return ERR_API_FAIL; }
	static s16 scratch[MAX_SPF * 2];
	if (!dec_frame(a, PTR(in), out ? out : scratch)) {
		*finish = 0;
		a->codec_err = 0x20b;
		return ERR_API_FAIL;
	}
	a->codec_err = 0;
	a->cur_off += a->sample_size;
	if (a->skip_frames == 0) {
		if (n) *n = want;
		*finish = a->end_sample < a->decode_pos + want ? a->loop_num == 0 : 0;
		a->decode_pos += want;
		if (a->loop_end && a->loop_num && a->decode_pos > a->loop_end) {
			a->cur_off = file_offset_at(a, a->loop_start);
			a->skip_frames = skip_frames_at(a, a->loop_start);
			a->decode_pos = a->loop_start;
			if (a->loop_num > 0) a->loop_num--;
		}
	} else {
		a->skip_frames--;
	}
	return 0;
}

static u32 skip_pending(Atrac *a, int *count) {
	*count = 0;
	int finish;
	while (a->skip_frames) {
		u32 r = decode_one(a, NULL, NULL, &finish);
		if (r) {
			if (r == ERR_API_FAIL) { LOGI("atrac: frame failed to decode while skipping"); (*count)++; }
			return r;
		}
		(*count)++;
	}
	return 0;
}

static int frame_us(const Atrac *a) {
	if (a->codec == AT3PLUS) return a->chans == 1 ? 1500 + a->sample_size * 84 / 100 : 1815 + a->sample_size * 158 / 100;
	return a->chans == 1 ? 685 : 970 + a->sample_size * 44 / 100;
}
static int init_us(int codec, int chans) { return codec == AT3PLUS ? (chans == 1 ? 524 : 646) : 210; }

// ---------------------------------------------------------------- RIFF WAVE parsing (PPSSPP's ParseWaveAT3)
typedef struct {
	int codec, chans, block_align, data_off, data_size, first_sample, end_sample, loop_start, loop_end;
	const u8 *extra;
	int extra_size;
} Track;

static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u32 rd16(const u8 *p) { return p[0] | p[1] << 8; }

static const u8 kAt3PlusGuid[16] = {0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44, 0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62};

static int parse_wave(const u8 *d, u32 len, Track *t) {
	memset(t, 0, sizeof *t);
	t->codec = (int)ERR_UNKNOWN_FORMAT;
	t->loop_start = t->loop_end = -1;
	u32 off = 0;
	for (;;) {
		if (off + 12 >= len) return ERR_SIZE_TOO_SMALL;
		if (memcmp(d + off, "RIFF", 4)) return ERR_UNKNOWN_FORMAT;
		u32 size = (rd32(d + off + 4) + 1) & ~1u;
		bool wave = !memcmp(d + off + 8, "WAVE", 4);
		off += 12;
		if (wave) break;
		if (size < 4 || (u64)off + size - 4 > len) return ERR_SIZE_TOO_SMALL;
		off += size - 4;
	}
	bool extended_fact = false;
	for (;;) {
		if (off + 8 >= len) return ERR_SIZE_TOO_SMALL;
		const u8 *id = d + off;
		int size = (int)((rd32(d + off + 4) + 1) & ~1u);
		off += 8;
		u32 next = off + size;
		const u8 *p = d + off;
		bool is_data = !memcmp(id, "data", 4);
		if (!is_data && (size < 0 || (u64)off + size > len)) return ERR_SIZE_TOO_SMALL;
		if (is_data) {
			t->data_size = size;
			t->data_off = (int)off;
			if (!t->first_sample) t->first_sample = t->codec == AT3 ? 0x400 : 0x800;
			if (extended_fact && t->codec == AT3PLUS) {
				t->first_sample -= 0xb8;
				if (t->loop_end != -1) { t->loop_end -= 0xb8; t->loop_start -= 0xb8; }
			}
			return t->codec;
		} else if (!memcmp(id, "fmt ", 4)) {
			if (t->codec != (int)ERR_UNKNOWN_FORMAT || size < 0x20) return ERR_UNKNOWN_FORMAT;
			u32 tag = rd16(p);
			t->chans = rd16(p + 2);
			if (t->chans != 1 && t->chans != 2) return ERR_UNKNOWN_FORMAT;
			if (rd32(p + 4) != 44100) return ERR_UNKNOWN_FORMAT;
			t->block_align = rd16(p + 12);
			if (!t->block_align) return ERR_UNKNOWN_FORMAT;
			if (tag == 0x270) {
				// ATRAC3: the 14 bytes after cbSize are what libavcodec takes as WAV extradata.
				if (rd16(p + 0x12) != 1) return ERR_UNKNOWN_FORMAT;
				if (rd16(p + 0x18) != rd16(p + 0x1a) || rd32(p + 0x1c) != 1) return ERR_UNKNOWN_FORMAT;
				t->extra = p + 0x12;
				t->extra_size = 14;
				t->codec = AT3;
			} else if (tag == 0xFFFE) {
				if (size < 0x34 || memcmp(p + 0x18, kAt3PlusGuid, 16)) return ERR_UNKNOWN_FORMAT;
				if ((u32)(p[0x2a] << 27) >> 29 != (u32)t->chans) return ERR_UNKNOWN_FORMAT;
				t->codec = AT3PLUS;
			} else {
				return ERR_UNKNOWN_FORMAT;
			}
		} else if (!memcmp(id, "smpl", 4)) {
			if (t->loop_start < 0) {
				if (size < 0x20) return ERR_UNKNOWN_FORMAT;
				if (rd32(p + 0x1c) != 0) {
					if (size < 0x34) return ERR_SIZE_TOO_SMALL;
					t->loop_start = (int)rd32(p + 0x2c);
					t->loop_end = (int)rd32(p + 0x30);
					if (t->loop_end <= t->loop_start) return ERR_BAD_CODEC_PARAMS;
				}
			}
		} else if (!memcmp(id, "fact", 4)) {
			if (size < 4) return ERR_UNKNOWN_FORMAT;
			t->end_sample = (int)rd32(p);
			if (size - 4 == 4) {
				t->first_sample = (int)rd32(p + 4);
			} else if (size - 4 >= 8) {
				t->first_sample = (int)rd32(p + 8);
				extended_fact = true;
			}
		}
		off = next;
	}
}

static u32 set_data(Atrac *a, const Track *t, u32 buffer, u32 read_size, u32 buffer_size) {
	a->codec = t->codec;
	a->chans = t->chans;
	a->sample_size = t->block_align;
	int shift = a->codec == AT3PLUS ? 11 : 10;
	a->first_valid = t->first_sample + skip_samples(a);
	int samples = t->end_sample ? t->end_sample + a->first_valid : t->data_size / a->sample_size << shift;
	a->decode_pos = a->first_valid;
	a->loop_num = 0;
	a->end_sample = samples - 1;
	a->skip_frames = (s8)(a->first_valid >> shift);
	a->loop_start = t->loop_start >= 0 ? t->loop_start + skip_samples(a) : 0;
	a->loop_end = t->loop_start >= 0 ? t->loop_end + skip_samples(a) : 0;
	a->buffer = buffer;
	a->buffer_bytes = buffer_size;
	a->data_off = a->cur_off = t->data_off;
	a->data_end = t->data_off + t->data_size;
	a->codec_err = 0;
	if ((u32)a->sample_size > buffer_size || a->loop_end > a->end_sample) return ERR_BAD_CODEC_PARAMS;
	if ((u32)(a->end_sample >> shift) * (u32)a->sample_size >= (u32)t->data_size) return ERR_BAD_CODEC_PARAMS;
	if (buffer_size < (u32)a->data_end || read_size < (u32)a->data_end) {
		LOGE("atrac: streamed playback (buffer %u of %d bytes) isn't supported", buffer_size, a->data_end);
		return ERR_SIZE_TOO_SMALL;
	}
	if (!dec_open(a, t->extra, t->extra_size)) return ERR_API_FAIL;
	LOGD("atrac: %s %dch, frame %d bytes, samples %d..%d, loop %d..%d, data %d+%d", a->codec == AT3PLUS ? "ATRAC3+" : "ATRAC3",
	     a->chans, a->sample_size, a->first_valid, a->end_sample, a->loop_start, a->loop_end, a->data_off, t->data_size);
	int skipped;
	u32 r = skip_pending(a, &skipped);
	a->setdata_skipped = skipped;
	if (r) LOGE("atrac: skipping the first frames failed (%08x)", r);
	return r;
}

static void release(Atrac *a) {
	dec_close(a);
	memset(a, 0, sizeof *a);
}

static Atrac *get(int id) { return id >= 0 && id < CONTEXTS && ctx[id].used ? &ctx[id] : NULL; }

// ---------------------------------------------------------------- HLE
static void h_SetDataAndGetID(Ctx *c) {
	u32 buffer = A(0);
	s32 size = (s32)A(1);
	if (size < 0) size = 0x10000000;
	if (size < 72) { RET(ERR_SIZE_TOO_SMALL); return; }
	if (!buffer) { RET(ERR_INVALID_POINTER); return; }
	u32 avail = (u32)size;
	if (!media_valid(buffer, avail)) avail = media_valid(buffer, 1) ? 0x0C000000 - (buffer & MEM_MASK) : 0;
	Track t;
	int codec = avail ? parse_wave(PTR(buffer), avail, &t) : (int)ERR_SIZE_TOO_SMALL;
	if (codec < 0) { LOGE("atrac: unsupported file at %08x (%08x)", buffer, (u32)codec); RET(codec); return; }
	int id = -1;
	for (int i = 0; i < CONTEXTS && id < 0; i++)
		if (kTypes[i] == codec && !ctx[i].used) id = i;
	if (id < 0) { RET(ERR_NO_ATRACID); return; }
	Atrac *a = &ctx[id];
	a->used = true;
	u32 r = set_data(a, &t, buffer, avail, (u32)size);
	if (r) {
		release(a);
		RET(r);
		if (r == ERR_API_FAIL) media_wait(media_schedule(init_us(t.codec, t.chans) + (t.codec == AT3PLUS ? 214 : 169)));
		return;
	}
	RET(id);
	media_wait(media_schedule(init_us(a->codec, a->chans) + a->setdata_skipped * frame_us(a)));
}

static void h_ReleaseAtracID(Ctx *c) {
	Atrac *a = get((int)A(0));
	if (!a) { RET(ERR_BAD_ATRACID); return; }
	release(a);
	RET(0);
}

static void h_DecodeData(Ctx *c) {
	Atrac *a = get((int)A(0));
	u32 out = A(1), nsamp = A(2), fin = A(3), remain = A(4);
	if (!a) { RET(ERR_BAD_ATRACID); return; }
	if (out & 1) { RET(ERR_BAD_ALIGNMENT); return; }
	if (out && !media_valid(out, 1)) { RET(ERR_SIZE_TOO_SMALL); return; }
	static s16 pcm[MAX_SPF * 2];
	int n = 0, finish = 0;
	u32 r = 0;
	for (int tries = a->skip_frames + 1; tries > 0 && !r; tries--)
		r = decode_one(a, pcm, &n, &finish);
	if (r) n = 0;
	if (media_valid(nsamp, 4)) SW(nsamp, n);
	if (media_valid(fin, 4)) SW(fin, finish);
	if (!r && media_valid(remain, 4)) SW(remain, ALLDATA_IS_ON_MEMORY);
	RET(r);
	if (r && r != ERR_API_FAIL) return;
	// The Media Engine writes the samples as it finishes, just before the thread wakes up.
	media_wait(media_schedule(frame_us(a)));
	u32 bytes = (u32)n * 4;
	if (out && bytes) {
		if (!media_valid(out, bytes)) bytes = 0x0C000000 - (out & MEM_MASK);
		memcpy(PTR(out), pcm, bytes);
	}
}

static void h_GetBitrate(Ctx *c) {
	Atrac *a = get((int)A(0));
	if (!a) { RET(ERR_BAD_ATRACID); return; }
	int rate = a->sample_size * 352800 / 1000;
	rate = a->codec == AT3PLUS ? ((rate >> 11) + 8) & ~0xF : (rate + 511) >> 10;
	if (media_valid(A(1), 4)) SW(A(1), rate);
	RET(0);
}

static void h_SetLoopNum(Ctx *c) {
	Atrac *a = get((int)A(0));
	if (!a) { RET(ERR_BAD_ATRACID); return; }
	if (a->loop_end <= 0) { RET(ERR_NO_LOOP_INFORMATION); return; }
	a->loop_num = (int)A(1);
	RET(0);
}

static void h_GetSoundSample(Ctx *c) {
	Atrac *a = get((int)A(0));
	if (!a) { RET(ERR_BAD_ATRACID); return; }
	int end = a->end_sample - a->first_valid;
	int ls = a->loop_end ? a->loop_start - a->first_valid : -1;
	int le = a->loop_end ? a->loop_end - a->first_valid : -1;
	if (media_valid(A(1), 4)) SW(A(1), end);
	if (media_valid(A(2), 4)) SW(A(2), ls);
	if (media_valid(A(3), 4)) SW(A(3), le);
	RET(0);
}

static void h_GetInternalErrorInfo(Ctx *c) {
	Atrac *a = get((int)A(0));
	if (!a) { RET(ERR_BAD_ATRACID); return; }
	if (media_valid(A(1), 4)) SW(A(1), a->codec_err);
	RET(0);
}

static void h_GetBufferInfoForResetting(Ctx *c) {
	Atrac *a = get((int)A(0));
	int pos = (int)A(1);
	u32 info = A(2);
	if (!a) { RET(ERR_BAD_ATRACID); return; }
	if (!media_valid(info, 32)) { RET(ERR_ILLEGAL_ADDR); return; }
	pos += a->first_valid;
	if ((u32)pos > (u32)a->end_sample) { RET(ERR_BAD_SAMPLE); return; }
	// Everything is in memory: nothing needs to be read in for any position.
	for (int i = 0; i < 2; i++) {
		SW(info + i * 16, a->buffer);
		SW(info + i * 16 + 4, 0);
		SW(info + i * 16 + 8, 0);
		SW(info + i * 16 + 12, 0);
	}
	int skipped;
	RET(skip_pending(a, &skipped));
	if (skipped) media_wait(300);
}

static void h_ResetPlayPosition(Ctx *c) {
	Atrac *a = get((int)A(0));
	int pos = (int)A(1);
	if (!a) { RET(ERR_BAD_ATRACID); return; }
	pos += a->first_valid;
	if ((u32)pos > (u32)a->end_sample) { RET(ERR_BAD_SAMPLE); return; }
	if (A(2) != 0) { RET(ERR_BAD_FIRST_RESET_SIZE); return; }
	if (A(3) != 0) { RET(ERR_BAD_SECOND_RESET_SIZE); return; }
	a->decode_pos = pos;
	a->skip_frames = skip_frames_at(a, pos);
	a->loop_num = 0;
	a->cur_off = file_offset_at(a, pos);
	a->codec_err = 0x20b;   // what hardware reports here
	int skipped;
	u32 r = skip_pending(a, &skipped);
	RET(r);
	if ((s32)r < 0) { if (skipped) media_wait(200); return; }
	media_wait(3000);
}

static const HleEnt kAtrac[] = {
	{0x7A20E7AF, "sceAtracSetDataAndGetID", h_SetDataAndGetID},
	{0x61EB33F5, "sceAtracReleaseAtracID", h_ReleaseAtracID},
	{0x6A8C3CD5, "sceAtracDecodeData", h_DecodeData},
	{0xA554A158, "sceAtracGetBitrate", h_GetBitrate},
	{0x868120B5, "sceAtracSetLoopNum", h_SetLoopNum},
	{0xA2BBA8BE, "sceAtracGetSoundSample", h_GetSoundSample},
	{0xE88F759B, "sceAtracGetInternalErrorInfo", h_GetInternalErrorInfo},
	{0x2DD3E298, "sceAtracGetBufferInfoForResetting", h_GetBufferInfoForResetting},
	{0xCA3CA3D2, "sceAtracGetBufferInfoForReseting", h_GetBufferInfoForResetting},
	{0x644E5607, "sceAtracResetPlayPosition", h_ResetPlayPosition},
};

void atrac_init(void) {
	hle_register(kAtrac, (int)(sizeof kAtrac / sizeof kAtrac[0]));
}
