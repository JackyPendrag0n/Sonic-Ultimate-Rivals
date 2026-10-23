// First-run setup from a disc image: `sr2recomp game.iso` (or dropping the .iso on the program) unpacks the
// disc's files into a "game" folder next to the program and runs from there. Same checks as
// tools/setup_iso.py: an ISO 9660 image of Sonic Rivals 2 USA (ULUS10323).
#include "kernel.h"
#include <errno.h>
#include <strings.h>

static FILE *iso;

static bool rd(u32 lba, void *buf, u32 n) {
	return fseeko(iso, (off_t)lba * 2048, SEEK_SET) == 0 && fread(buf, 1, n, iso) == n;
}

static void mkdirs(const char *path) {
	char d[1024];
	snprintf(d, sizeof d, "%s", path);
	for (char *p = d + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(d, 0755); *p = '/'; }
}

typedef struct { char path[256]; u32 lba, size; } IsoFile;
static IsoFile *files; static int nfiles, capfiles;

static void walk(u32 lba, u32 size, const char *prefix, int depth) {
	if (depth > 16 || size > (16u << 20)) return;
	u8 *data = malloc(size);
	if (!data || !rd(lba, data, size)) { free(data); return; }
	for (u32 o = 0; o < size;) {
		u8 l = data[o];
		if (l == 0) { o = (o / 2048 + 1) * 2048; continue; }
		if (o + l > size || l < 34) break;
		const u8 *r = data + o;
		o += l;
		u32 ext, sz; memcpy(&ext, r + 2, 4); memcpy(&sz, r + 10, 4);
		u8 flags = r[25], nl = r[32];
		if (nl == 1 && (r[33] == 0 || r[33] == 1)) continue;
		char name[256]; int n = 0;
		for (int i = 0; i < nl && r[33 + i] != ';' && n < 200; i++) name[n++] = r[33 + i];
		name[n] = 0;
		char p[256]; snprintf(p, sizeof p, "%s%s", prefix, name);
		if (flags & 2) { char sub[256]; snprintf(sub, sizeof sub, "%s/", p); walk(ext, sz, sub, depth + 1); continue; }
		if (nfiles == capfiles) { capfiles = capfiles ? capfiles * 2 : 256; files = realloc(files, capfiles * sizeof *files); }
		snprintf(files[nfiles].path, sizeof files[nfiles].path, "%s", p);
		files[nfiles].lba = ext; files[nfiles].size = sz; nfiles++;
	}
	free(data);
}

static IsoFile *find(const char *path) {
	for (int i = 0; i < nfiles; i++) if (!strcasecmp(files[i].path, path)) return &files[i];
	return NULL;
}

// DISC_ID from PARAM.SFO
static void disc_id(const u8 *sfo, u32 len, char *out, size_t n) {
	out[0] = 0;
	if (len < 20 || memcmp(sfo, "\0PSF", 4)) return;
	u32 kt, dt, cnt; memcpy(&kt, sfo + 8, 4); memcpy(&dt, sfo + 12, 4); memcpy(&cnt, sfo + 16, 4);
	for (u32 i = 0; i < cnt && 20 + i * 16 + 16 <= len; i++) {
		u16 ko; u32 ln, dof; const u8 *e = sfo + 20 + i * 16;
		memcpy(&ko, e, 2); memcpy(&ln, e + 4, 4); memcpy(&dof, e + 12, 4);
		if (kt + ko < len && !strcmp((const char *)sfo + kt + ko, "DISC_ID") && dt + dof + ln <= len) {
			snprintf(out, n, "%.*s", (int)ln, (const char *)sfo + dt + dof);
			return;
		}
	}
}

// Unpack iso_path into out_dir. Returns false (after logging why) if the image can't be used.
bool iso_unpack(const char *iso_path, const char *out_dir) {
	iso = fopen(iso_path, "rb");
	if (!iso) { LOGE("can't open %s: %s", iso_path, strerror(errno)); return false; }
	u8 pvd[2048];
	if (!rd(16, pvd, sizeof pvd) || memcmp(pvd + 1, "CD001", 5)) {
		LOGE("%s is not an ISO 9660 image (CSO/ZSO/CHD files must be converted back to .iso first)", iso_path);
		fclose(iso); return false;
	}
	u32 root_lba, root_size; memcpy(&root_lba, pvd + 156 + 2, 4); memcpy(&root_size, pvd + 156 + 10, 4);
	nfiles = 0;
	walk(root_lba, root_size, "", 0);
	IsoFile *sfo = find("PSP_GAME/PARAM.SFO"), *boot = find("PSP_GAME/SYSDIR/BOOT.BIN");
	char id[32] = "";
	if (sfo && sfo->size < 65536) {
		u8 *b = malloc(sfo->size);
		if (b && rd(sfo->lba, b, sfo->size)) disc_id(b, sfo->size, id, sizeof id);
		free(b);
	}
	if (strcmp(id, "ULUS10323")) {
		LOGE("%s is %s, but this port is made for Sonic Rivals 2 USA (ULUS10323)", iso_path, id[0] ? id : "not a PSP game disc");
		fclose(iso); return false;
	}
	u8 magic[4];
	if (!boot || !rd(boot->lba, magic, 4) || memcmp(magic, "\x7f" "ELF", 4)) {
		LOGE("%s: PSP_GAME/SYSDIR/BOOT.BIN is missing or not a plain program file", iso_path);
		fclose(iso); return false;
	}
	LOGI("unpacking %s into %s (one time only) ...", iso_path, out_dir);
	u64 total = 0, done = 0;
	for (int i = 0; i < nfiles; i++) total += files[i].size;
	char *buf = malloc(1 << 22);
	int last = -1;
	for (int i = 0; i < nfiles; i++) {
		if (!strncasecmp(files[i].path, "PSP_GAME/SYSDIR/UPDATE/", 23)) { done += files[i].size; continue; }   // firmware update, unused
		char dst[1024]; snprintf(dst, sizeof dst, "%s/%s", out_dir, files[i].path);
		mkdirs(dst);
		FILE *o = fopen(dst, "wb");
		if (!o) { LOGE("can't write %s: %s", dst, strerror(errno)); free(buf); fclose(iso); return false; }
		fseeko(iso, (off_t)files[i].lba * 2048, SEEK_SET);
		for (u32 left = files[i].size; left;) {
			u32 n = left < (1u << 22) ? left : (1u << 22);
			if (fread(buf, 1, n, iso) != n) { LOGE("%s ends early: the dump is truncated", iso_path); fclose(o); free(buf); fclose(iso); return false; }
			fwrite(buf, 1, n, o);
			left -= n; done += n;
			int pct = (int)(done * 100 / (total ? total : 1));
			if (pct / 10 != last) { last = pct / 10; LOGI("  %d%%", pct); }
		}
		fclose(o);
	}
	free(buf);
	fclose(iso);
	LOGI("done: %d files", nfiles);
	return true;
}
