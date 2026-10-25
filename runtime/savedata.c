// The PSP's save data storage: what the sceUtilitySavedata modes do to the memory stick, without their screens
// (sysdlg.c has those). A save is a directory ms0:/PSP/SAVEDATA/<game name><save name> holding the game's data
// file, a PARAM.SFO describing it, and optional icons, picture and sound.
//
// Like a PSP, saves are encrypted with the sceChnnlsv cipher (AES-128 with the PSP's KIRK keys, through
// libavutil) and the PARAM.SFO carries the matching hashes, so saves from a PSP or PPSSPP load here and saves
// made here load there. Unencrypted saves (from older builds of this runtime) load too.
//
// What each mode does, its error codes and the sizes it reports follow PPSSPP's SavedataParam.cpp and
// sceChnnlsv.cpp (GPL-2.0+), which were checked against hardware.
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <libavutil/aes.h>
#include <libavutil/mem.h>
#include <libavutil/random_seed.h>
#include "kernel.h"
#include "media.h"
#include "savedata.h"

#define CLUSTER 32768u                 // PSP memory sticks use 32 KB clusters
#define STICK_BYTES (1ull << 30)       // the reported stick size: SR2 computes free space in 32 bits
#define FILE_LIST_ENTRIES 99           // PARAM.SFO's SAVEDATA_FILE_LIST: {char name[13]; u8 hash[16]; u8 pad[3]}
#define FILE_LIST_BYTES (FILE_LIST_ENTRIES * 32)

static const char *const subfile_names[4] = {"ICON0.PNG", "ICON1.PMF", "PIC1.PNG", "SND0.AT3"};
static const int subfile_offs[4] = {SD_ICON0, SD_ICON1, SD_PIC1, SD_SND0};

// ================================================================= small helpers
void sd_field(u32 p, int off, int len, char *out) {
	const char *s = (const char *)PTR(p + off);
	int n = 0;
	while (n < len && s[n]) n++;
	memcpy(out, s, n);
	out[n] = 0;
}

static bool name_ok(const char *s) {
	return !strchr(s, '/') && !strchr(s, '\\') && strcmp(s, ".") && strcmp(s, "..");
}

bool sd_names_ok(u32 p) {
	char g[14], s[21], f[14];
	sd_field(p, SD_GAME_NAME, 13, g);
	sd_field(p, SD_SAVE_NAME, 20, s);
	sd_field(p, SD_FILE_NAME, 13, f);
	return name_ok(g) && name_ok(s) && name_ok(f);
}

static void game_name(u32 p, char *out) { sd_field(p, SD_GAME_NAME, 13, out); }
static void save_name(u32 p, char *out) {   // "<>" (any) counts as none
	sd_field(p, SD_SAVE_NAME, 20, out);
	if (!strcmp(out, "<>")) out[0] = 0;
}
static void file_name(u32 p, char *out) { sd_field(p, SD_FILE_NAME, 13, out); }

static bool has_key(u32 p) {
	for (int i = 0; i < 16; i++)
		if (*PTR(p + SD_KEY + i)) return true;
	return false;
}

// The host path of ms0:/PSP/SAVEDATA/<dir>[/<file>]
static void host_path(const char *dir, const char *file, char *out, size_t n) {
	char psp[256];
	snprintf(psp, sizeof psp, "ms0:/PSP/SAVEDATA/%s%s%s", dir, file && *file ? "/" : "", file ? file : "");
	if (io_host_path(psp, out, n) < 0) out[0] = 0;
}

static bool is_dir(const char *path) {
	struct stat st;
	return path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
static bool is_file(const char *path) {
	struct stat st;
	return path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static u8 *read_file(const char *path, size_t *size) {
	FILE *f = path[0] ? fopen(path, "rb") : NULL;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8 *d = malloc(n > 0 ? n + 16 : 16);
	*size = n > 0 ? fread(d, 1, n, f) : 0;
	fclose(f);
	return d;
}

static bool write_file(const char *path, const void *d, size_t n) {
	FILE *f = path[0] ? fopen(path, "wb") : NULL;
	if (!f) return false;
	bool ok = fwrite(d, 1, n, f) == n;
	return fclose(f) == 0 && ok;
}

static void make_dirs(const char *path) {   // the directory and any missing parents
	char d[1024];
	snprintf(d, sizeof d, "%s", path);
	for (char *s = d + 1; *s; s++)
		if (*s == '/') { *s = 0; mkdir(d, 0777); *s = '/'; }
	mkdir(d, 0777);
}

static bool guest_ok(u32 addr, u32 size) { return addr && media_valid(addr, size ? size : 1); }

// ================================================================= PARAM.SFO
typedef struct { char key[32]; u16 fmt; u32 len, max; u8 *data; } SfoEnt;
typedef struct { int n; SfoEnt e[48]; } Sfo;
enum { SFO_BIN = 0x0004, SFO_STR = 0x0204, SFO_INT = 0x0404 };

static void sfo_free(Sfo *s) {
	for (int i = 0; i < s->n; i++) free(s->e[i].data);
	s->n = 0;
}

static bool sfo_read(Sfo *s, const u8 *d, size_t size) {
	s->n = 0;
	if (size < 20 || memcmp(d, "\0PSF", 4)) return false;
	u32 kt, dt, n;
	memcpy(&kt, d + 8, 4); memcpy(&dt, d + 12, 4); memcpy(&n, d + 16, 4);
	if (20 + (size_t)n * 16 > size || kt > size || dt > size) return false;
	for (u32 i = 0; i < n && s->n < 48; i++) {
		const u8 *ix = d + 20 + i * 16;
		u16 ko, fmt; u32 len, max, dof;
		memcpy(&ko, ix, 2); memcpy(&fmt, ix + 2, 2); memcpy(&len, ix + 4, 4); memcpy(&max, ix + 8, 4); memcpy(&dof, ix + 12, 4);
		size_t ka = kt + ko, da = dt + dof;
		if (ka >= size || da > size) return false;
		SfoEnt *e = &s->e[s->n];
		size_t kl = strnlen((const char *)d + ka, size - ka);
		if (!kl || kl >= sizeof e->key) continue;
		memcpy(e->key, d + ka, kl); e->key[kl] = 0;
		if (len > max) max = len;
		if (da + len > size) len = size - da;
		e->fmt = fmt; e->len = len; e->max = max;
		e->data = calloc(1, max + 1);
		memcpy(e->data, d + da, len);
		s->n++;
	}
	return true;
}

static SfoEnt *sfo_get(Sfo *s, const char *key) {
	for (int i = 0; i < s->n; i++)
		if (!strcmp(s->e[i].key, key)) return &s->e[i];
	return NULL;
}

static SfoEnt *sfo_put(Sfo *s, const char *key, u16 fmt, u32 max) {
	SfoEnt *e = sfo_get(s, key);
	if (!e) {
		if (s->n >= 48) return NULL;
		e = &s->e[s->n++];
		memset(e, 0, sizeof *e);
		snprintf(e->key, sizeof e->key, "%s", key);
	}
	free(e->data);
	e->fmt = fmt; e->max = max;
	e->data = calloc(1, max + 1);
	return e;
}

static void sfo_set_str(Sfo *s, const char *key, const char *v, u32 max) {
	SfoEnt *e = sfo_put(s, key, SFO_STR, max);
	if (!e) return;
	u32 n = strlen(v);
	if (n > max - 1) n = max - 1;
	memcpy(e->data, v, n);
	e->len = n + 1;   // with the NUL
}
static void sfo_set_int(Sfo *s, const char *key, u32 v) {
	SfoEnt *e = sfo_put(s, key, SFO_INT, 4);
	if (e) { memcpy(e->data, &v, 4); e->len = 4; }
}
static void sfo_set_bin(Sfo *s, const char *key, const void *v, u32 n, u32 max) {
	SfoEnt *e = sfo_put(s, key, SFO_BIN, max);
	if (e) { memcpy(e->data, v, n < max ? n : max); e->len = n < max ? n : max; }
}
static void sfo_str(Sfo *s, const char *key, char *out, size_t n) {
	SfoEnt *e = sfo_get(s, key);
	out[0] = 0;
	if (e && e->fmt != SFO_INT) snprintf(out, n, "%.*s", (int)e->len, (const char *)e->data);
}

static int key_cmp(const void *a, const void *b) { return strcmp(((const SfoEnt *)a)->key, ((const SfoEnt *)b)->key); }

// The file, keys in order like a PSP writes them; padded with zeroes to 16 bytes (the hashes need that).
static u8 *sfo_write(Sfo *s, size_t *size) {
	qsort(s->e, s->n, sizeof s->e[0], key_cmp);
	u32 keys = 0, data = 0;
	for (int i = 0; i < s->n; i++) { keys += strlen(s->e[i].key) + 1; data += s->e[i].max; }
	keys = (keys + 3) & ~3u;
	u32 kt = 20 + 16 * s->n, dt = kt + keys, total = dt + data;
	u8 *d = calloc(1, (total + 15) & ~15u);
	u32 h[5] = {0x46535000, 0x101, kt, dt, (u32)s->n};
	memcpy(d, h, 20);
	u32 ko = 0, dof = 0;
	for (int i = 0; i < s->n; i++) {
		SfoEnt *e = &s->e[i];
		u8 *ix = d + 20 + i * 16;
		u16 k16 = (u16)ko;
		memcpy(ix, &k16, 2); memcpy(ix + 2, &e->fmt, 2); memcpy(ix + 4, &e->len, 4); memcpy(ix + 8, &e->max, 4); memcpy(ix + 12, &dof, 4);
		strcpy((char *)d + kt + ko, e->key);
		memcpy(d + dt + dof, e->data, e->len);
		ko += strlen(e->key) + 1;
		dof += e->max;
	}
	*size = total;
	return d;
}

static bool sfo_load(const char *dir, Sfo *s) {
	char path[1024];
	host_path(dir, "PARAM.SFO", path, sizeof path);
	size_t n;
	u8 *d = read_file(path, &n);
	s->n = 0;
	if (!d) return false;
	bool ok = sfo_read(s, d, n);
	free(d);
	return ok;
}

// ================================================================= sceChnnlsv
// The KIRK engine's AES keys these modes use (from its key vault)
static const struct { int id; u8 key[16]; } vault[] = {
	{3, {0x98, 0x02, 0xC4, 0xE6, 0xEC, 0x9E, 0x9E, 0x2F, 0xFC, 0x63, 0x4C, 0xE4, 0x2F, 0xBB, 0x46, 0x68}},
	{4, {0x99, 0x24, 0x4C, 0xD2, 0x58, 0xF5, 0x1B, 0xCB, 0xB0, 0x61, 0x9C, 0xA7, 0x38, 0x30, 0x07, 0x5F}},
	{5, {0x02, 0x25, 0xD7, 0xBA, 0x63, 0xEC, 0xB9, 0x4A, 0x9D, 0x23, 0x76, 0x01, 0xB3, 0xF6, 0xAC, 0x17}},
	{12, {0x84, 0x85, 0xC8, 0x48, 0x75, 0x08, 0x43, 0xBC, 0x9B, 0x9A, 0xEC, 0xA7, 0x9C, 0x7F, 0x60, 0x18}},
	{13, {0xB5, 0xB1, 0x6E, 0xDE, 0x23, 0xA9, 0x7B, 0x0E, 0xA1, 0x7C, 0xDB, 0xA2, 0xDC, 0xDE, 0xC4, 0x6E}},
	{14, {0xC8, 0x71, 0xFD, 0xB3, 0xBC, 0xC5, 0xD2, 0xF2, 0xE2, 0xD7, 0x72, 0x9D, 0xDF, 0x82, 0x68, 0x82}},
	{16, {0x32, 0x29, 0x5B, 0xD5, 0xEA, 0xF7, 0xA3, 0x42, 0x16, 0xC8, 0x8E, 0x48, 0xFF, 0x50, 0xD3, 0x71}},
	{17, {0x46, 0xF2, 0x5E, 0x8E, 0x4D, 0x2A, 0xA5, 0x40, 0x73, 0x0B, 0xC4, 0x6E, 0x47, 0xEE, 0x6F, 0x0A}},
	{18, {0x5D, 0xC7, 0x11, 0x39, 0xD0, 0x19, 0x38, 0xBC, 0x02, 0x7F, 0xDD, 0xDC, 0xB0, 0x83, 0x7D, 0x9D}},
	{83, {0xAF, 0xFE, 0x8E, 0xB1, 0x3D, 0xD1, 0x7E, 0xD8, 0x0A, 0x61, 0x24, 0x1C, 0x95, 0x92, 0x56, 0xB6}},
	{87, {0x1C, 0x9B, 0xC4, 0x90, 0xE3, 0x06, 0x64, 0x81, 0xFA, 0x59, 0xFD, 0xB6, 0x00, 0xBB, 0x28, 0x70}},
	{100, {0x03, 0xB3, 0x02, 0xE8, 0x5F, 0xF3, 0x81, 0xB1, 0x3B, 0x8D, 0xAA, 0x2A, 0x90, 0xFF, 0x5E, 0x61}},
};

static const u8 hash198C[16] = {0xFA, 0xAA, 0x50, 0xEC, 0x2F, 0xDE, 0x54, 0x93, 0xAD, 0x14, 0xB2, 0xCE, 0xA5, 0x30, 0x05, 0xDF};
static const u8 hash19BC[16] = {0xCB, 0x15, 0xF4, 0x07, 0xF9, 0x6A, 0x52, 0x3C, 0x04, 0xB9, 0xB2, 0xEE, 0x5C, 0x53, 0xFA, 0x86};
static const u8 key19CC[16] = {0x70, 0x44, 0xA3, 0xAE, 0xEF, 0x5D, 0xA5, 0xF2, 0x85, 0x7F, 0xF2, 0xD6, 0x94, 0xF5, 0x36, 0x3B};
static const u8 key19DC[16] = {0xEC, 0x6D, 0x29, 0x59, 0x26, 0x35, 0xA5, 0x7F, 0x97, 0x2A, 0x0D, 0xBC, 0xA3, 0x26, 0x33, 0x00};
static const u8 key199C[16] = {0x36, 0xA5, 0x3E, 0xAC, 0xC5, 0x26, 0x9E, 0xA3, 0x83, 0xD9, 0xEC, 0x25, 0x6C, 0x48, 0x48, 0x72};
static const u8 key19AC[16] = {0xD8, 0xC0, 0xB0, 0xF3, 0x3E, 0x6B, 0x76, 0x85, 0xFD, 0xFB, 0x4D, 0x7D, 0x45, 0x1E, 0x92, 0x03};

typedef struct { int mode; u8 result[16]; u8 key[16]; int key_len; } MacCtx;   // sceSdMac*: the hash
typedef struct { int mode; int counter; u8 key[16]; } CipherCtx;             // sceSdCipher*: the cipher

static u8 kbuf[20 + 2048];   // a KIRK request: 20-byte header, then the data
#define KDATA (kbuf + 20)

static void xor16(u8 *d, const u8 *s, int n) { for (int i = 0; i < n; i++) d[i] ^= s[i]; }

// KIRK commands 4 and 7: AES-128-CBC with a zero IV and a key from the vault, on buf's data (after its
// 20-byte header). Encrypting leaves the result in place; decrypting writes it over the header, at buf.
static int kirk_cbc(u8 *buf, int len, int key_id, bool enc) {
	const u8 *key = NULL;
	for (size_t i = 0; i < sizeof vault / sizeof vault[0]; i++)
		if (vault[i].id == key_id) key = vault[i].key;
	if (!key || len <= 0 || (len & 15)) return -257;
	struct AVAES *a = av_aes_alloc();
	av_aes_init(a, key, 128, !enc);
	u8 iv[16] = {0};
	if (enc) av_aes_crypt(a, buf + 20, buf + 20, len / 16, iv, 0);
	else {
		u8 *in = malloc(len);
		memcpy(in, buf + 20, len);
		av_aes_crypt(a, buf, in, len / 16, iv, 1);
		free(in);
	}
	av_free(a);
	return 0;
}
// Commands 5 and 8 use the console's own key, which no emulator has: they fail like PPSSPP's.
static int kirk_fuse(void) { return -258; }

static int num_from_mode(int mode) {
	switch (mode) { case 1: return 3; case 2: return 5; case 3: return 12; case 4: return 13; case 6: return 17; default: return 16; }
}
static int num_from_mode2(int mode) { return mode == 1 ? 4 : mode == 3 ? 14 : 18; }
static int type_from_mode(int mode) { return mode == 1 || mode == 2 ? 83 : mode == 3 || mode == 4 ? 87 : 100; }

// One block of the cipher: the key stream for len bytes (data_out: a KIRK buffer), XORed into data
static int cipher_block(u8 *out, u8 *data, int len, const u8 *key, int *counter, int mode) {
	memcpy(out + 20, key, 16);
	int type = type_from_mode(mode), res;
	if (type == 87) xor16(out + 20, key19AC, 16);
	else if (type == 100) xor16(out + 20, key19DC, 16);
	res = mode == 2 || mode == 4 || mode == 6 ? kirk_fuse() : kirk_cbc(out, 16, num_from_mode2(mode), false);
	if (type == 87) xor16(out, key199C, 16);
	else if (type == 100) xor16(out, key19CC, 16);
	if (res) return res;
	u8 iv[16], seed[16];
	memcpy(seed, out, 16);
	if (*counter == 1) memset(iv, 0, 16);
	else { memcpy(iv, seed, 12); u32 c = *counter - 1; memcpy(iv + 12, &c, 4); }
	for (int i = 20; i < len + 20; i += 16) {
		memcpy(out + i, seed, 12);
		u32 c = *counter;
		memcpy(out + 12 + i, &c, 4);
		(*counter)++;
	}
	u8 last[16];
	memcpy(last, out + len + 4, 16);
	res = kirk_cbc(out, len, type, false);
	if (res) return res;
	xor16(out, iv, 16);
	memcpy(iv, last, 16);
	xor16(data, out, len);
	return 0;
}

static int mac_block(u8 *buf, int size, u8 *result, int num) {
	xor16(buf + 20, result, 16);
	int res = kirk_cbc(buf, size, num, true);
	if (res) return res;
	memcpy(result, buf + size + 4, 16);
	return 0;
}

static int mac_init(MacCtx *c, int mode) {
	c->mode = mode;
	memset(c->result, 0, 16);
	memset(c->key, 0, 16);
	c->key_len = 0;
	return 0;
}

static int mac_update(MacCtx *c, const u8 *data, int len) {
	if (c->key_len >= 17) return -1026;
	if (c->key_len + len < 17) {
		memcpy(c->key + c->key_len, data, len);
		c->key_len += len;
		return 0;
	}
	int num = num_from_mode(c->mode);
	memset(KDATA, 0, 2048);
	memcpy(KDATA, c->key, c->key_len);
	int keep = (c->key_len + len) & 15;
	if (!keep) keep = 16;
	int fill = c->key_len;
	c->key_len = keep;
	int diff = len - keep;
	memcpy(c->key, data + diff, keep);
	for (int i = 0; i < diff; i++) {
		if (fill == 2048) {
			int res = mac_block(kbuf, 2048, c->result, num);
			if (res) return res;
			fill = 0;
		}
		KDATA[fill++] = data[i];
	}
	if (fill) mac_block(kbuf, fill, c->result, num);
	return 0;
}

static void shift_left(u8 *d) {   // the CMAC subkey step: d << 1, reduced
	int carry = d[0] & 0x80 ? 135 : 0;
	for (int i = 0; i < 15; i++) d[i] = (u8)(d[i] << 1 | d[i + 1] >> 7);
	d[15] = (u8)((d[15] << 1) ^ carry);
}

static int mac_final(MacCtx *c, u8 *hash, const u8 *key) {
	if (c->key_len >= 17) return -1026;
	int num = num_from_mode(c->mode), res;
	memset(KDATA, 0, 16);
	if ((res = kirk_cbc(kbuf, 16, num, true))) return res;
	u8 k1[16], h[16];
	memcpy(k1, KDATA, 16);
	shift_left(k1);
	if (c->key_len < 16) {
		shift_left(k1);
		c->key[c->key_len] = 0x80;
		if (c->key_len + 1 < 16) memset(c->key + c->key_len + 1, 0, 16 - c->key_len - 1);
	}
	xor16(c->key, k1, 16);
	memcpy(KDATA, c->key, 16);
	memcpy(h, c->result, 16);
	if ((res = mac_block(kbuf, 16, h, num))) return res;
	if (c->mode == 3 || c->mode == 4) xor16(h, hash198C, 16);
	else if (c->mode == 5 || c->mode == 6) xor16(h, hash19BC, 16);
	if (c->mode == 2 || c->mode == 4 || c->mode == 6) {
		memcpy(KDATA, h, 16);
		if ((res = kirk_fuse())) return res;
	}
	if (key) {
		xor16(h, key, 16);
		memcpy(KDATA, h, 16);
		if ((res = kirk_cbc(kbuf, 16, num, true))) return res;
		memcpy(h, KDATA, 16);
	}
	memcpy(hash, h, 16);
	mac_init(c, 0);
	return 0;
}

static int cipher_init(CipherCtx *c, int mode, int how, u8 *data, const u8 *key) {
	c->mode = mode;
	c->counter = 1;
	if (how == 2) {   // decrypting: the file's IV
		memcpy(c->key, data, 16);
		if (key) xor16(c->key, key, 16);
		return 0;
	}
	if (how == 1) {   // encrypting: a new random IV
		u8 hdr[37];
		for (int i = 0; i < 20; i += 4) { u32 r = av_get_random_seed(); memcpy(hdr + i, &r, 4); }
		memcpy(hdr + 20, hdr, 16);
		memset(hdr + 32, 0, 4);
		int type = type_from_mode(mode), res;
		if (type == 87) xor16(hdr + 20, key199C, 16);
		else if (type == 100) xor16(hdr + 20, key19CC, 16);
		res = mode == 2 || mode == 4 || mode == 6 ? kirk_fuse() : kirk_cbc(hdr, 16, num_from_mode2(mode), true);
		if (type == 87) xor16(hdr + 20, key19AC, 16);
		else if (type == 100) xor16(hdr + 20, key19DC, 16);
		if (res) return res;
		memcpy(c->key, hdr + 20, 16);
		memcpy(data, hdr + 20, 16);
		if (key) xor16(c->key, key, 16);
	}
	return 0;
}

static int cipher_update(CipherCtx *c, u8 *data, int len) {
	static u8 out[20 + 2048];
	if (!len) return 0;
	if (len & 15) return -1025;
	int i = 0;
	for (; len >= 2048; i += 2048, len -= 2048) {
		int res = cipher_block(out, data + i, 2048, c->key, &c->counter, c->mode);
		if (res) return res;
	}
	return len ? cipher_block(out, data + i, len, c->key, &c->counter, c->mode) : 0;
}

// In place: data gets the 16-byte IV in front (room for it after aligned_len bytes is the caller's)
int sd_encrypt(int mode, u8 *data, int *len, int *aligned_len, u8 hash[16], const u8 *key) {
	MacCtx m; CipherCtx c;
	memmove(data + 16, data, *aligned_len);
	memset(hash, 0, 16);
	memset(data, 0, 16);
	if (cipher_init(&c, mode, 1, data, key) < 0) return -1;
	mac_init(&m, mode);
	if (mac_update(&m, data, 16) < 0) return -3;
	if (cipher_update(&c, data + 16, *aligned_len) < 0) return -4;
	memset(data + 16 + *len, 0, *aligned_len - *len);
	if (mac_update(&m, data + 16, *aligned_len) < 0) return -5;
	if (mac_final(&m, hash, key) < 0) return -7;
	*aligned_len += 16;
	*len += 16;
	return 0;
}

int sd_decrypt(int mode, u8 *data, int *len, int *aligned_len, const u8 *key, const u8 *expected_hash) {
	MacCtx m; CipherCtx c;
	if (*aligned_len <= 16) return -1;
	*len -= 16;
	*aligned_len -= 16;
	mac_init(&m, mode);
	if (cipher_init(&c, mode, 2, data, key) < 0) return -3;
	if (mac_update(&m, data, 16) < 0) return -4;
	if (mac_update(&m, data + 16, *aligned_len) < 0) return -5;
	if (cipher_update(&c, data + 16, *aligned_len) < 0) return -6;
	if (expected_hash) {
		u8 h[16];
		if (mac_final(&m, h, key) < 0) return -7;
		if (memcmp(h, expected_hash, 16)) return -8;
	}
	memmove(data, data + 16, *len);
	return 0;
}

static int build_hash(u8 *out, const u8 *data, int aligned_len, int mode) {
	MacCtx m;
	memset(out, 0, 16);
	mac_init(&m, mode & 0xFF);
	if (mac_update(&m, data, aligned_len) < 0) return -2;
	if (mac_final(&m, out, NULL) < 0) memset(out, 1, 16);   // the console-key hashes: PPSSPP's stand-in
	return 0;
}

// The PARAM.SFO's own hashes, in its SAVEDATA_PARAMS (sfo is padded with zeroes to 16 bytes)
int sd_update_hash(u8 *sfo, int sfo_size, int params_offset, int mode) {
	int aligned = (sfo_size + 15) & ~15, ret;
	u8 h[16];
	memset(sfo + params_offset, 0, 128);
	int first = mode & 2 ? 4 : 2, second = mode & 2 ? 3 : 0;
	if (mode & 4) { first = 6; second = 5; }
	if ((ret = build_hash(h, sfo, aligned, first)) < 0) return ret - 400;
	memcpy(sfo + params_offset + 0x20, h, 16);
	sfo[params_offset] |= 1;
	if (mode & 6) {
		sfo[params_offset] |= (mode & 6) << 4;
		if ((ret = build_hash(h, sfo, aligned, second)) < 0) return ret - 500;
		memcpy(sfo + params_offset + 0x70, h, 16);
	}
	if ((ret = build_hash(h, sfo, aligned, 1)) < 0) return ret - 600;
	memcpy(sfo + params_offset + 0x10, h, 16);
	return 0;
}

// Which cipher mode the request wants: newer firmware (games built with SDK 2.07 on) uses mode 5 for
// secure versions 0 and 3; requests too short to carry a key use mode 1.
static int crypt_mode(u32 p) {
	if (LW(p + SD_SIZE) < 1536) return 1;
	extern u32 g_sdk_version;
	bool new_hash = g_sdk_version > 0x0206FFFF;
	switch (LW(p + SD_SECURE_VERSION)) {
	case 0: return new_hash ? 5 : 3;
	case 2: return 3;
	case 3: return new_hash ? 5 : 1;
	default: return 1;
	}
}
static bool missing_key(u32 p) { return LW(p + SD_SIZE) >= 1536 && LW(p + SD_SECURE_VERSION) != 1 && !has_key(p); }

// How a save on the stick was encrypted, from its PARAM.SFO: 0 none, else the cipher mode
static int save_crypt_mode(const char *dir) {
	Sfo s;
	if (!sfo_load(dir, &s)) return 0;
	SfoEnt *e = sfo_get(&s, "SAVEDATA_PARAMS");
	int m = 0;
	if (e && e->len) {
		switch (e->data[0]) {
		case 0: m = 0; break;
		case 0x01: m = 1; break;
		case 0x21: m = 3; break;
		case 0x41: m = 5; break;
		default: LOGI("savedata: unexpected SAVEDATA_PARAMS flags %02x in %s", e->data[0], dir); m = 1; break;
		}
	}
	sfo_free(&s);
	return m;
}

static bool expected_hash(const char *dir, const char *file, u8 hash[16]) {
	Sfo s;
	bool found = false;
	if (!sfo_load(dir, &s)) return false;
	SfoEnt *e = sfo_get(&s, "SAVEDATA_FILE_LIST");
	for (u32 i = 0; e && i < FILE_LIST_ENTRIES && (i + 1) * 32 <= e->len; i++) {
		const char *n = (const char *)e->data + i * 32;
		if (n[0] && !strncmp(n, file, 13)) { memcpy(hash, e->data + i * 32 + 13, 16); found = true; break; }
	}
	sfo_free(&s);
	return found;
}

// ================================================================= the save directories
static s64 dir_info(const char *dir, time_t *mtime) {   // total size of its files; PARAM.SFO's time
	char path[1024];
	host_path(dir, NULL, path, sizeof path);
	DIR *d = opendir(path);
	if (!d) return -1;
	s64 total = 0;
	bool first = true;
	struct dirent *de;
	while ((de = readdir(d))) {
		char fp[1100];
		struct stat st;
		snprintf(fp, sizeof fp, "%s/%s", path, de->d_name);
		if (de->d_name[0] == '.' || stat(fp, &st) || !S_ISREG(st.st_mode)) continue;
		if (mtime && (first || !strcasecmp(de->d_name, "PARAM.SFO"))) *mtime = st.st_mtime;
		first = false;
		total += st.st_size;
	}
	closedir(d);
	return total;
}

static u8 *guest_copy(u32 off_buf, u32 size, u32 *out_size) {   // a copy of a guest buffer, or NULL
	if (!guest_ok(off_buf, size) || !size) return NULL;
	u8 *d = malloc(size);
	memcpy(d, PTR(off_buf), size);
	*out_size = size;
	return d;
}

static void slot_fill(SdSlot *s, const char *dir, const char *name) {
	memset(s, 0, sizeof *s);
	snprintf(s->name, sizeof s->name, "%s", name);
	snprintf(s->dir, sizeof s->dir, "%s", dir);
	s->size = dir_info(dir, &s->mtime);
	if (s->size < 0) { s->size = 0; return; }
	Sfo sfo;
	if (sfo_load(dir, &sfo)) {
		sfo_str(&sfo, "TITLE", s->title, sizeof s->title);
		sfo_str(&sfo, "SAVEDATA_TITLE", s->save_title, sizeof s->save_title);
		sfo_str(&sfo, "SAVEDATA_DETAIL", s->detail, sizeof s->detail);
		sfo_free(&sfo);
	} else {
		s->broken = true;
		snprintf(s->title, sizeof s->title, "%s", dir);
	}
	char path[1024];
	size_t n;
	host_path(dir, "ICON0.PNG", path, sizeof path);
	s->icon = read_file(path, &n);
	s->icon_size = (u32)n;
}

// An empty slot shows the picture the game gave for new saves, or for SAVE its icon
static void slot_empty(u32 p, SdSlot *s, const char *dir, const char *name) {
	memset(s, 0, sizeof *s);
	snprintf(s->name, sizeof s->name, "%s", name);
	snprintf(s->dir, sizeof s->dir, "%s", dir);
	u32 nd = LW(p + SD_NEW_DATA);
	if (guest_ok(nd, 16) && LW(nd)) s->icon = guest_copy(LW(nd), LW(nd + 8), &s->icon_size);
	else if (LW(p + SD_MODE) == SD_SAVE) s->icon = guest_copy(LW(p + SD_ICON0), LW(p + SD_ICON0 + 8), &s->icon_size);
}

bool sd_multi_name(int mode) {
	switch (mode) {
	case SD_LOAD: case SD_AUTOLOAD: case SD_SAVE: case SD_AUTOSAVE: case SD_MAKEDATASECURE: case SD_MAKEDATA:
	case SD_READDATASECURE: case SD_READDATA: case SD_WRITEDATASECURE: case SD_WRITEDATA: case SD_AUTODELETE:
	case SD_DELETE: case SD_ERASESECURE: case SD_ERASE: case SD_DELETEDATA:
		return false;
	default:
		return true;
	}
}

static int dir_names(char (**out)[64]) {   // the directories in ms0:/PSP/SAVEDATA, sorted
	char path[1024];
	host_path("", NULL, path, sizeof path);
	DIR *d = opendir(path);
	int n = 0, cap = 16;
	*out = malloc(cap * 64);
	if (!d) return 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		char fp[1100];
		snprintf(fp, sizeof fp, "%s/%s", path, de->d_name);
		if (de->d_name[0] == '.' || !is_dir(fp) || strlen(de->d_name) >= 64) continue;
		if (n == cap) *out = realloc(*out, (cap *= 2) * 64);
		snprintf((*out)[n++], 64, "%s", de->d_name);
	}
	closedir(d);
	qsort(*out, n, 64, (int (*)(const void *, const void *))strcmp);
	return n;
}

void sd_free_slots(SdSlot *s, int n) {
	for (int i = 0; i < n; i++) free(s[i].icon);
	free(s);
}

int sd_list_slots(u32 p, SdSlot **out) {
	int mode = LW(p + SD_MODE), n = 0;
	char game[14], dir[64];
	game_name(p, game);
	*out = NULL;
	if (mode == SD_LISTALLDELETE) {   // everything on the stick
		char (*names)[64];
		int k = dir_names(&names);
		*out = calloc(k ? k : 1, sizeof(SdSlot));
		for (int i = 0; i < k; i++) slot_fill(&(*out)[n++], names[i], "");
		free(names);
		return n;
	}
	bool list_empty = mode != SD_LISTLOAD && mode != SD_LISTDELETE;
	u32 list = LW(p + SD_SAVE_NAME_LIST);
	int count = 0;
	if (sd_multi_name(mode) && guest_ok(list, 20))
		while (count < 1000 && guest_ok(list + count * 20, 20) && *PTR(list + count * 20)) count++;
	if (count) {
		*out = calloc(count, sizeof(SdSlot));
		char (*names)[64] = NULL;
		int nn = -1;
		for (int i = 0; i < count; i++) {
			char name[21];
			sd_field(list + i * 20, 0, 20, name);
			if (!name_ok(name)) continue;
			if (!strcmp(name, "<>")) {   // any save of the game not listed yet
				if (nn < 0) nn = dir_names(&names);
				size_t gl = strlen(game);
				for (int k = 0; k < nn; k++) {
					if (strncmp(names[k], game, gl)) continue;
					bool dup = false;
					for (int j = 0; j < n; j++) dup |= !strcmp((*out)[j].name, names[k] + gl);
					if (dup) continue;
					slot_fill(&(*out)[n++], names[k], names[k] + gl);
					break;
				}
				continue;
			}
			snprintf(dir, sizeof dir, "%s%s", game, name);
			char path[1024];
			host_path(dir, NULL, path, sizeof path);
			if (is_dir(path)) slot_fill(&(*out)[n++], dir, name);
			else if (list_empty) slot_empty(p, &(*out)[n++], dir, name);
		}
		free(names);
		return n;
	}
	// a single save, the request's own
	char name[21], path[1024];
	save_name(p, name);
	snprintf(dir, sizeof dir, "%s%s", game, name);
	host_path(dir, NULL, path, sizeof path);
	*out = calloc(1, sizeof(SdSlot));
	if (is_dir(path)) { slot_fill(*out, dir, name); return 1; }
	if (list_empty) slot_empty(p, *out, dir, name);   // shown, but not counted as a save
	return 0;
}

// ================================================================= the modes' work
static bool rw_mode(int mode) { return mode == SD_READDATA || mode == SD_READDATASECURE; }

static void load_subfile(const char *dir, const char *name, u32 fd) {
	u32 buf = LW(fd), cap = LW(fd + 4);
	if (!guest_ok(buf, cap)) return;
	char path[1024];
	host_path(dir, name, path, sizeof path);
	FILE *f = path[0] ? fopen(path, "rb") : NULL;
	if (!f) return;
	size_t n = fread(PTR(buf), 1, cap, f);
	fclose(f);
	if (n) SW(fd + 8, (u32)n);
}

int sd_load(u32 p, const char *save, bool secure) {
	int mode = LW(p + SD_MODE);
	bool rw = rw_mode(mode);
	char game[14], file[14], dir[64], path[1024];
	game_name(p, game);
	file_name(p, file);
	snprintf(dir, sizeof dir, "%s%s", game, save);
	host_path(dir, NULL, path, sizeof path);
	if (!is_dir(path)) return rw ? SD_ERR_RW_NO_DATA : SD_ERR_LOAD_NO_DATA;
	host_path(dir, file, path, sizeof path);
	if (file[0] && !is_file(path)) return rw ? SD_ERR_RW_FILE_NOT_FOUND : SD_ERR_LOAD_FILE_NOT_FOUND;
	SW(p + SD_DATA_SIZE, 0);
	if (LW(p + SD_SECURE_VERSION) > 3 || (secure && missing_key(p))) return SD_ERR_LOAD_PARAM;
	if (file[0]) {
		size_t n;
		u8 *raw = read_file(path, &n);
		if (!raw || !n) { free(raw); return SD_ERR_LOAD_NO_DATA; }
		memset(PTR(p + SD_SAVE_NAME), 0, 20);
		strncpy((char *)PTR(p + SD_SAVE_NAME), save, 20);
		int size = (int)n, prev = save_crypt_mode(dir);
		u8 *plain = NULL;
		if (prev && secure) {
			int want = crypt_mode(p);
			if (want > 1 && !has_key(p)) { free(raw); return SD_ERR_LOAD_PARAM; }
			u8 hash[16];
			bool have_hash = expected_hash(dir, file, hash);
			// the file's own mode first (saves from older firmware), then the request's; a bad hash is
			// only reported, as a damaged but decryptable save is better loaded than lost
			int tries[3][2] = {{prev, have_hash}, {want, have_hash}, {prev, 0}};
			for (int t = 0; t < 3 && !plain; t++) {
				if (t == 1 && want == prev) continue;
				if (t == 2 && !have_hash) break;
				int m = tries[t][0], len = size, al = (size + 15) & ~15;
				u8 *buf = calloc(1, al + 16);
				memcpy(buf, raw, size);
				if (sd_decrypt(m, buf, &len, &al, m > 1 ? (const u8 *)PTR(p + SD_KEY) : NULL, tries[t][1] ? hash : NULL) == 0) {
					plain = buf;
					size = len;
					if (t) LOGI("savedata: %s decrypted with mode %d%s", dir, m, t == 2 ? " (hash mismatch, likely damaged)" : "");
				} else free(buf);
			}
		}
		if (!plain) plain = raw, raw = NULL;
		u32 db = LW(p + SD_DATA_BUF), cap = LW(p + SD_DATA_BUF_SIZE);
		if (db) {
			if ((u32)size > cap || !guest_ok(db, size)) { free(plain); free(raw); return rw ? SD_ERR_RW_DATA_BROKEN : SD_ERR_LOAD_DATA_BROKEN; }
			memcpy(PTR(db), plain, size);
		}
		free(plain);
		free(raw);
		SW(p + SD_DATA_SIZE, size);
	}
	Sfo s;
	if (!sfo_load(dir, &s)) return rw ? SD_ERR_RW_DATA_BROKEN : SD_ERR_LOAD_DATA_BROKEN;
	char t[1025];
	static const struct { const char *key; int off, len; } strs[] = {
		{"TITLE", SD_TITLE, 128}, {"SAVEDATA_TITLE", SD_SAVE_TITLE, 128}, {"SAVEDATA_DETAIL", SD_DETAIL, 1024}};
	for (int i = 0; i < 3; i++) {
		sfo_str(&s, strs[i].key, t, sizeof t);
		strncpy((char *)PTR(p + strs[i].off), t, strs[i].len);
	}
	SfoEnt *pl = sfo_get(&s, "PARENTAL_LEVEL");
	*PTR(p + SD_PARENTAL) = pl && pl->len >= 1 ? pl->data[0] : 0;
	sfo_free(&s);
	SW(p + SD_BIND, 1021);   // what a PSP answers
	for (int i = 0; i < 4; i++) load_subfile(dir, subfile_names[i], p + subfile_offs[i]);
	return 0;
}

int sd_save(u32 p, const char *save, bool secure) {
	int mode = LW(p + SD_MODE);
	u32 db = LW(p + SD_DATA_BUF), dsize = LW(p + SD_DATA_SIZE), cap = LW(p + SD_DATA_BUF_SIZE);
	if (dsize > cap) return SD_ERR_RW_BAD_PARAMS;
	for (int i = 0; i < 4; i++) {
		u32 fd = p + subfile_offs[i];
		if (guest_ok(LW(fd), 1) && LW(fd + 4) < LW(fd + 8)) return SD_ERR_RW_BAD_PARAMS;
	}
	if (LW(p + SD_SECURE_VERSION) > 3 || (secure && missing_key(p))) return SD_ERR_SAVE_PARAM;
	char game[14], file[14], dir[64], path[1024];
	game_name(p, game);
	file_name(p, file);
	snprintf(dir, sizeof dir, "%s%s", game, save);
	host_path(dir, NULL, path, sizeof path);
	if (!path[0]) return SD_ERR_SAVE_MS_NOSPACE;
	make_dirs(path);

	// the data, encrypted like a PSP would
	bool has_data = guest_ok(db, 1);
	u8 *crypted = NULL, hash[16] = {0};
	int csize = 0;
	if (has_data && secure) {
		csize = dsize;
		if (!csize || (u32)csize > cap) csize = cap;
		int al = (csize + 15) & ~15, m = crypt_mode(p);
		if (m > 1 && !has_key(p)) return SD_ERR_SAVE_PARAM;
		crypted = calloc(1, al + 16);
		if (guest_ok(db, csize)) memcpy(crypted, PTR(db), csize);
		if (sd_encrypt(m, crypted, &csize, &al, hash, m > 1 ? (const u8 *)PTR(p + SD_KEY) : NULL)) {
			LOGE("savedata: encrypting %s failed, saving it unencrypted", dir);
			free(crypted);
			crypted = NULL;
		}
	}

	// PARAM.SFO
	Sfo s;
	bool was_crypted = save_crypt_mode(dir) != 0;
	if (!sfo_load(dir, &s)) s.n = 0;
	char t[1025];
	if (*PTR(p + SD_TITLE)) {   // the rewrite modes may leave these out
		sd_field(p, SD_TITLE, 128, t);       sfo_set_str(&s, "TITLE", t, 128);
		sd_field(p, SD_SAVE_TITLE, 128, t);  sfo_set_str(&s, "SAVEDATA_TITLE", t, 128);
		sd_field(p, SD_DETAIL, 1024, t);     sfo_set_str(&s, "SAVEDATA_DETAIL", t, 1024);
		sfo_set_int(&s, "PARENTAL_LEVEL", *PTR(p + SD_PARENTAL));
		sfo_set_str(&s, "CATEGORY", "MS", 4);
		sfo_set_str(&s, "SAVEDATA_DIRECTORY", dir, 64);
	}
	u8 list[FILE_LIST_BYTES] = {0};
	SfoEnt *old = sfo_get(&s, "SAVEDATA_FILE_LIST");
	if (old) memcpy(list, old->data, old->len < FILE_LIST_BYTES ? old->len : FILE_LIST_BYTES);
	if (secure && has_data) {   // this file's entry and hash (a new entry if it has none)
		for (int i = 0; i < FILE_LIST_ENTRIES; i++) {
			char *n = (char *)list + i * 32;
			if (n[0] && strncmp(n, file, 13)) continue;
			memset(n, 0, 13);
			snprintf(n, 13, "%s", file);
			memcpy(n + 13, hash, 16);
			break;
		}
	}
	sfo_set_bin(&s, "SAVEDATA_FILE_LIST", list, FILE_LIST_BYTES, FILE_LIST_BYTES);
	u8 zero[128] = {0};
	sfo_set_bin(&s, "SAVEDATA_PARAMS", zero, 128, 128);
	size_t sfo_size;
	u8 *sfo = sfo_write(&s, &sfo_size);
	bool sub_write = mode == SD_WRITEDATA || mode == SD_WRITEDATASECURE;
	if (crypted || (sub_write && was_crypted)) {
		for (int i = 0; i < s.n; i++)   // (sfo_write sorted the entries)
			if (!strcmp(s.e[i].key, "SAVEDATA_PARAMS")) {
				u32 dt, off;
				memcpy(&dt, sfo + 12, 4);
				memcpy(&off, sfo + 20 + i * 16 + 12, 4);
				sd_update_hash(sfo, (int)sfo_size, dt + off, crypt_mode(p));
			}
	}
	sfo_free(&s);
	host_path(dir, "PARAM.SFO", path, sizeof path);
	bool ok = write_file(path, sfo, sfo_size);
	free(sfo);

	if (has_data) {
		u8 *d = crypted;
		u32 n = csize;
		if (!crypted) {
			n = dsize && dsize <= cap ? dsize : cap;
			d = guest_ok(db, n) ? PTR(db) : NULL;
		}
		memset(PTR(p + SD_SAVE_NAME), 0, 20);
		strncpy((char *)PTR(p + SD_SAVE_NAME), save, 20);
		if (file[0] && d) {
			host_path(dir, file, path, sizeof path);
			ok &= write_file(path, d, n);
		}
		free(crypted);
	}
	for (int i = 0; i < 4; i++) {   // the icons, picture and sound (ICON1 and SND0 only when not empty)
		u32 fd = p + subfile_offs[i], buf = LW(fd), n = LW(fd + 8);
		if (!guest_ok(buf, n) || ((i == 1 || i == 3) && !n)) continue;
		host_path(dir, subfile_names[i], path, sizeof path);
		write_file(path, PTR(buf), n);
	}
	if (!ok) LOGE("savedata: writing %s failed", dir);
	LOGI("savedata: saved %s (%u bytes%s)", dir, dsize, crypted ? ", encrypted" : "");
	return ok ? 0 : SD_ERR_SAVE_MS_NOSPACE;
}

static void remove_tree(const char *path) {
	DIR *d = opendir(path);
	if (d) {
		struct dirent *de;
		while ((de = readdir(d))) {
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
			char fp[1100];
			snprintf(fp, sizeof fp, "%s/%s", path, de->d_name);
			if (is_dir(fp)) remove_tree(fp);
			else remove(fp);
		}
		closedir(d);
	}
	rmdir(path);
}

bool sd_delete_dir(u32 p, const char *dir) {
	char game[14], path[1024];
	game_name(p, game);
	if (!game[0] && LW(p + SD_MODE) != SD_LISTALLDELETE) return false;   // never the whole SAVEDATA
	if (!dir[0] || !name_ok(dir)) return false;
	host_path(dir, NULL, path, sizeof path);
	if (!is_dir(path)) return false;
	remove_tree(path);
	LOGI("savedata: deleted %s", dir);
	return true;
}

int sd_erase(u32 p) {
	char game[14], save[21], file[14], dir[64], path[1024];
	game_name(p, game);
	save_name(p, save);
	file_name(p, file);
	snprintf(dir, sizeof dir, "%s%s", game, save);
	host_path(dir, NULL, path, sizeof path);
	if (!is_dir(path)) return SD_ERR_RW_NO_DATA;
	host_path(dir, "PARAM.SFO", path, sizeof path);
	if (!is_file(path)) return SD_ERR_RW_DATA_BROKEN;
	host_path(dir, file, path, sizeof path);
	if (file[0] && !is_file(path)) return SD_ERR_RW_FILE_NOT_FOUND;
	if (!file[0] || !save[0]) return 0;
	remove(path);
	Sfo s;   // and its entry in the file list
	if (sfo_load(dir, &s)) {
		SfoEnt *e = sfo_get(&s, "SAVEDATA_FILE_LIST");
		bool changed = false;
		for (u32 i = 0; e && (i + 1) * 32 <= e->len; i++)
			if (!strncmp((char *)e->data + i * 32, file, 13)) { memset(e->data + i * 32, 0, 29); changed = true; break; }
		if (changed) {
			size_t n;
			u8 *d = sfo_write(&s, &n);
			host_path(dir, "PARAM.SFO", path, sizeof path);
			write_file(path, d, n);
			free(d);
		}
		sfo_free(&s);
	}
	return 0;
}

static void space_text(u64 size, bool round_up, char *out, size_t n) {
	static const char *const suffix[] = {"B", "KB", "MB", "GB"};
	for (int i = 0; i < 4; i++) {
		if (size < 1024) { snprintf(out, n, "%llu %s", (unsigned long long)size, suffix[i]); return; }
		size = round_up ? (size + 1023) / 1024 : size / 1024;
	}
	snprintf(out, n, "%llu TB", (unsigned long long)size);
}

static u64 free_bytes(void) {
	return STICK_BYTES;   // the host's free space is far more than a 1 GB stick; games mishandle more
}

static u32 clusters(u64 size) { return (u32)((size + CLUSTER - 1) / CLUSTER); }

int sd_sizes(u32 p) {
	int ret = 0;
	char txt[16];
	u32 a = LW(p + SD_MS_FREE);
	if (guest_ok(a, 20)) {
		u64 fb = free_bytes();
		SW(a, CLUSTER); SW(a + 4, (u32)(fb / CLUSTER)); SW(a + 8, (u32)(fb / 1024));
		space_text(fb, false, txt, sizeof txt);
		memset(PTR(a + 12), 0, 8);
		strncpy((char *)PTR(a + 12), txt, 8);
	}
	a = LW(p + SD_MS_DATA);
	if (guest_ok(a, 64)) {   // {char game[13]; pad[3]; char save[20]; {used clusters, KB, str[8], 32KB, str32[8]}}
		char game[14], save[21], dir[64], path[1024];
		sd_field(a, 0, 13, game);
		sd_field(a, 16, 20, save);
		snprintf(dir, sizeof dir, "%s%s", game, strcmp(save, "<>") ? save : "");
		host_path(dir, NULL, path, sizeof path);
		u32 info = a + 36;
		DIR *d = name_ok(game) && name_ok(save) && path[0] ? opendir(path) : NULL;
		if (d) {
			u32 used = 0;
			struct dirent *de;
			while ((de = readdir(d))) {
				char fp[1100];
				struct stat st;
				snprintf(fp, sizeof fp, "%s/%s", path, de->d_name);
				if (de->d_name[0] == '.' || stat(fp, &st)) continue;
				used += clusters(S_ISREG(st.st_mode) ? st.st_size : 0);
			}
			closedir(d);
			u32 total = used * CLUSTER;
			space_text(total, true, txt, sizeof txt);
			SW(info, used); SW(info + 4, total / 1024);
			strncpy((char *)PTR(info + 8), txt, 8);
			SW(info + 16, total / 1024);
			strncpy((char *)PTR(info + 20), txt, 8);
		} else {
			SW(info, 0); SW(info + 4, 0); memset(PTR(info + 8), 0, 8);
			SW(info + 16, 0); memset(PTR(info + 20), 0, 8);
			ret = SD_ERR_SIZES_NO_DATA;
		}
	}
	a = LW(p + SD_UTILITY_DATA);
	if (guest_ok(a, 28)) {   // what saving this request would take
		u64 total = 2 * CLUSTER;   // the directory and the PARAM.SFO
		if (*PTR(p + SD_FILE_NAME)) total += clusters((u64)LW(p + SD_DATA_SIZE) + 16) * (u64)CLUSTER;
		for (int i = 0; i < 4; i++) total += clusters(LW(p + subfile_offs[i] + 8)) * (u64)CLUSTER;
		space_text(total, true, txt, sizeof txt);
		SW(a, (u32)(total / CLUSTER)); SW(a + 4, (u32)(total / 1024));
		memset(PTR(a + 8), 0, 8); strncpy((char *)PTR(a + 8), txt, 8);
		SW(a + 16, (u32)(total / 1024));
		memset(PTR(a + 20), 0, 8); strncpy((char *)PTR(a + 20), txt, 8);
	}
	return ret;
}

static bool wild_match(const char *t, const char *re) {   // the PSP's save name patterns: * and ?
	if (!*re) return !*t;
	if (*re == '*') return wild_match(t, re + 1) || (*t && wild_match(t + 1, re));
	if (!*t) return false;
	return (*re == '?' || *re == *t) && wild_match(t + 1, re + 1);
}

static void psp_time(u32 a, time_t t) {   // ScePspDateTime
	struct tm tm;
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	u16 v[6] = {(u16)(tm.tm_year + 1900), (u16)(tm.tm_mon + 1), (u16)tm.tm_mday, (u16)tm.tm_hour, (u16)tm.tm_min, (u16)tm.tm_sec};
	memcpy(PTR(a), v, 12);
	SW(a + 12, 0);
}

void sd_id_list(u32 p) {   // LIST: the game's saves matching the save name
	u32 il = LW(p + SD_ID_LIST);
	if (!guest_ok(il, 12)) return;
	u32 max = LW(il), entries = LW(il + 8), n = 0;
	char game[14], save[21], pattern[40], (*names)[64];
	game_name(p, game);
	save_name(p, save);
	snprintf(pattern, sizeof pattern, "%s%s", game, save);
	int k = dir_names(&names);
	for (int i = 0; i < k && n < max; i++) {
		if (!wild_match(names[i], pattern)) continue;
		u32 e = entries + n * 72;   // {mode; ctime, atime, mtime (16 each); char name[20]}
		if (!guest_ok(e, 72)) break;
		time_t mt = 0;
		dir_info(names[i], &mt);
		SW(e, 0x11FF);
		psp_time(e + 4, mt); psp_time(e + 20, mt); psp_time(e + 36, mt);
		memset(PTR(e + 52), 0, 20);
		strncpy((char *)PTR(e + 52), names[i] + strlen(game), 20);
		n++;
	}
	free(names);
	SW(il + 4, n);
}
