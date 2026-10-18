// IoFileMgrForUser / StdioForUser: maps disc0:/umd0: to the game directory, ms0: to a save directory.
#define _GNU_SOURCE
#include "kernel.h"
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/mman.h>
#include <strings.h>
#include "mpeg/mpeg_hle.h"

#define MAX_FD 64
typedef struct { int used; int hostfd; DIR *dir; char path[512]; s64 async_res; int async_pending; s64 win_off, win_len; char host[1024]; } PspFile;
static PspFile fds[MAX_FD];

// Resolve each path component case-insensitively (PSP file systems are case-insensitive).
static void resolve_ci(char *path) {
	char out[1024] = "";
	char tmp[1024];
	snprintf(tmp, sizeof tmp, "%s", path);
	char *save = NULL;
	int absolute = tmp[0] == '/';
	if (absolute) strcpy(out, "/");
	for (char *tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
		if (!strcmp(tok, ".")) continue;
		char cand[1024];
		snprintf(cand, sizeof cand, "%s%s%s", out, (out[0] && out[strlen(out) - 1] != '/') ? "/" : "", tok);
		struct stat st;
		if (stat(cand, &st) != 0) {
			DIR *d = opendir(out[0] ? out : ".");
			if (d) {
				struct dirent *e;
				while ((e = readdir(d))) {
					if (!strcasecmp(e->d_name, tok)) {
						snprintf(cand, sizeof cand, "%s%s%s", out, (out[0] && out[strlen(out) - 1] != '/') ? "/" : "", e->d_name);
						break;
					}
				}
				closedir(d);
			}
		}
		snprintf(out, sizeof out, "%s", cand);
	}
	strcpy(path, out);
}

int io_host_path(const char *psp, char *out, size_t outsz) {
	const char *colon = strchr(psp, ':');
	const char *dev = psp;
	const char *rest = colon ? colon + 1 : psp;
	const char *root;
	if (!colon) {
		root = g_game_root;
		// relative to the boot directory
		snprintf(out, outsz, "%s/PSP_GAME/SYSDIR/%s", root, psp);
		resolve_ci(out);
		return 0;
	}
	size_t dl = colon - dev;
	if ((dl == 5 && !strncasecmp(dev, "disc0", 5)) || !strncasecmp(dev, "umd", 3)) root = g_game_root;
	else if (dl == 3 && !strncasecmp(dev, "ms0", 3)) root = g_save_root;
	else if (!strncasecmp(dev, "host", 4)) return -1;
	else { LOGI("io: unsupported device in '%s'", psp); return -1; }
	while (*rest == '/') rest++;
	snprintf(out, outsz, "%s/%s", root, rest);
	resolve_ci(out);
	return 0;
}

// ---- raw sector access (disc0:/sce_lbn0x<lbn>_size0x<size>)
// Games ask for a file's start sector with sceIoIoctl(0x01020004) and then reopen the disc by sector,
// often adding offsets they computed from the original disc layout. An extracted game folder has no
// sectors, so the runtime carries the disc's sector map (tools/lbnmap.py) and maps sce_lbn opens back
// to a window into the file that holds those sectors.
typedef struct { u32 lbn, size; const char *path; } DiscFile;
static const DiscFile disc_files[] = {
#include "lbnmap_ulus10323.inc"
};
#define NDISC ((int)(sizeof disc_files / sizeof disc_files[0]))
// path relative to the game folder ("PSP_GAME/..."), compared case-insensitively
static const DiscFile *disc_by_host(const char *host) {
	size_t rl = strlen(g_game_root);
	if (strncmp(host, g_game_root, rl) || host[rl] != '/') return NULL;
	for (int i = 0; i < NDISC; i++) if (!strcasecmp(host + rl + 1, disc_files[i].path)) return &disc_files[i];
	return NULL;
}
static const DiscFile *disc_by_lbn(u32 lbn) {
	for (int i = 0; i < NDISC; i++) {
		u32 n = (disc_files[i].size + 2047) / 2048;
		if (lbn >= disc_files[i].lbn && lbn < disc_files[i].lbn + (n ? n : 1)) return &disc_files[i];
	}
	return NULL;
}

static int alloc_fd(void) {
	for (int i = 3; i < MAX_FD; i++) if (!fds[i].used) { memset(&fds[i], 0, sizeof fds[i]); fds[i].used = 1; return i; }
	return -1;
}
static PspFile *getf(u32 fd) { return (fd < MAX_FD && fds[fd].used) ? &fds[fd] : NULL; }
// the PSP path an open file descriptor was opened with (sceKernelLoadModuleByID), or NULL
const char *io_fd_path(u32 fd) { PspFile *f = getf(fd); return f && !f->dir ? f->path : NULL; }

bool io_virtual_file(const char *psp, const unsigned char **data, unsigned *size);
static s32 do_open(const char *psp, u32 flags) {
	char hp[1024];
	const char *lb = strstr(psp, "sce_lbn");
	if (lb) {
		u32 lbn = 0, size = 0;
		if (sscanf(lb, "sce_lbn%x_size%x", &lbn, &size) != 2) return SCE_ERROR_FILE_NOT_FOUND;
		const DiscFile *m = disc_by_lbn(lbn);
		if (!m) {
			if (getenv("RCSM_LBN_FATAL")) fatal("io: %s: sector %x isn't inside any file of the disc", psp, lbn);
			LOGE("io: %s: sector %x isn't inside any file of the disc", psp, lbn);
			return SCE_ERROR_FILE_NOT_FOUND;
		}
		char host[1024];
		snprintf(host, sizeof host, "%s/%s", g_game_root, m->path);
		resolve_ci(host);
		int h = open(host, O_RDONLY);
		if (h < 0) LOGE("io: %s needs %s, which can't be opened", psp, host);
		if (h < 0) return SCE_ERROR_FILE_NOT_FOUND;
		int fd = alloc_fd();
		if (fd < 0) { close(h); return 0x80020320; }
		fds[fd].hostfd = h;
		fds[fd].win_off = (s64)(lbn - m->lbn) * 2048;
		fds[fd].win_len = size;
		lseek(h, fds[fd].win_off, SEEK_SET);
		snprintf(fds[fd].path, sizeof fds[fd].path, "%s", psp);
		snprintf(fds[fd].host, sizeof fds[fd].host, "%s", host);
		LOGD("open %s -> %s +%llx (%x bytes), fd %d", psp, m->path, (long long)fds[fd].win_off, size, fd);
		return fd;
	}
	if (io_host_path(psp, hp, sizeof hp) < 0) hp[0] = 0;
	int of = 0;
	switch (flags & 3) { case 1: of = O_RDONLY; break; case 2: of = O_WRONLY; break; default: of = O_RDWR; }
	if (flags & 0x100) of |= O_APPEND;
	if (flags & 0x200) of |= O_CREAT;
	if (flags & 0x400) of |= O_TRUNC;
	if (flags & 0x800) of |= O_EXCL;
	if ((flags & 0x200) && g_save_root[0] && !strncmp(psp, "ms0:", 4)) {
		// make parent dirs for writes to the memory stick
		char d[1024]; snprintf(d, sizeof d, "%s", hp);
		for (char *p = d + strlen(g_save_root) + 1; (p = strchr(p, '/')); p++) { *p = 0; mkdir(d, 0755); *p = '/'; }
	}
	int h = open(hp, of, 0644);
	bool faked = false;
	if (h < 0) {
		// Missing FMV clips: serve a 1-second black PSMF so the game's player runs and finishes.
		size_t pl = strlen(psp);
		const char *fake = getenv("RCSM_FAKE_MISSING_VIDEOS");
		if (pl > 4 && !strcasecmp(psp + pl - 4, ".PMF") && (flags & 3) == 1 && (!fake || atoi(fake))) {
			int err = errno;
			if (err == EACCES || err == EPERM)
				LOGE("video %s: %s exists but can't be read (permission denied). Make the CLIPS files readable by "
				     "your user, e.g. chmod -R a+rX on the CLIPS folder (files extracted with sudo or from a mounted "
				     "ISO are often root-only).", psp, hp);
			else {
				static int said;
				if (!said++) {
					char dir[1024]; snprintf(dir, sizeof dir, "%s", hp);
					char *slash = strrchr(dir, '/'); if (slash) *slash = 0;
					struct stat ds;
					if (stat(dir, &ds) != 0)
						LOGE("videos: no CLIPS folder (looked for %s). Copy PSP_GAME/USRDIR/CLIPS from a full dump of the "
						     "disc to play the FMVs.", dir);
					else
						LOGE("videos: %s exists, but %s isn't in it (%s).", dir, strrchr(hp, '/') ? strrchr(hp, '/') + 1 : hp, strerror(err));
				}
			}
			unsigned sz = 0;
			const unsigned char *data = mpeg_fake_pmf(&sz);
			h = memfd_create("fakepmf", 0);
			if (h >= 0 && write(h, data, sz) == (ssize_t)sz) {
				lseek(h, 0, SEEK_SET);
				LOGI("video %s is missing: substituting a 1 s black clip", psp);
				faked = true;
			} else if (h >= 0) { close(h); h = -1; }
		}
	}
	if (h < 0 && (flags & 3) == 1) {
		// files the mods add (e.g. the Clank skin, skins.c)
		const unsigned char *vd; unsigned vs;
		if (io_virtual_file(psp, &vd, &vs)) {
			h = memfd_create("virtual", 0);
			if (h >= 0 && write(h, vd, vs) == (ssize_t)vs) { lseek(h, 0, SEEK_SET); LOGI("io: %s served by the runtime", psp); }
			else if (h >= 0) { close(h); h = -1; }
		}
	}
	if (h < 0) { LOGD("open %s (%s) -> errno %d", psp, hp, errno); return SCE_ERROR_FILE_NOT_FOUND; }
	struct stat st;
	if (fstat(h, &st) == 0 && S_ISDIR(st.st_mode)) { close(h); return SCE_ERROR_FILE_NOT_FOUND; }
	int fd = alloc_fd();
	if (fd < 0) { close(h); return 0x80020320; }
	fds[fd].hostfd = h;
	snprintf(fds[fd].path, sizeof fds[fd].path, "%s", psp);
	snprintf(fds[fd].host, sizeof fds[fd].host, "%s", hp);
	{ size_t pl = strlen(psp); if (pl > 4 && !strcasecmp(psp + pl - 4, ".PMF") && !faked) LOGI("video %s: opened %s", psp, hp); }
	LOGD("open %s -> fd %d", psp, fd);
	return fd;
}

static void h_IoOpen(Ctx *c) { RET(do_open(Pstr(A(0)), A(1))); }
static void h_IoOpenAsync(Ctx *c) {
	s32 r = do_open(Pstr(A(0)), A(1));
	if (r >= 0) { fds[r].async_res = r; fds[r].async_pending = 1; }
	else LOGD("OpenAsync %s failed %x", Pstr(A(0)), r);
	RET(r);
}
static void h_IoClose(Ctx *c) {
	PspFile *f = getf(A(0));
	if (!f) { RET(0x80020323); return; }
	if (f->hostfd > 0) close(f->hostfd);
	f->used = 0;
	RET(0);
}
static void h_IoCloseAsync(Ctx *c) { PspFile *f = getf(A(0)); if (f) { close(f->hostfd); f->hostfd = -1; f->async_res = 0; f->async_pending = 1; } RET(f ? 0 : 0x80020323); }
static s32 do_read(u32 fd, u32 buf, u32 size) {
	PspFile *f = getf(fd);
	if (!f) return 0x80020323;
	if (f->win_len) {   // sector window: stop at its end
		s64 pos = lseek(f->hostfd, 0, SEEK_CUR) - f->win_off;
		if (pos >= f->win_len) return 0;
		if ((s64)size > f->win_len - pos) size = (u32)(f->win_len - pos);
	}
	ssize_t n = read(f->hostfd, PTR(buf), size);
	return n < 0 ? (s32)0x80010005 : (s32)n;
}
static void h_IoRead(Ctx *c) { RET(do_read(A(0), A(1), A(2))); }
static void h_IoReadAsync(Ctx *c) {
	PspFile *f = getf(A(0));
	if (!f) { RET(0x80020323); return; }
	f->async_res = do_read(A(0), A(1), A(2));
	f->async_pending = 1;
	RET(0);
}
static void h_IoWrite(Ctx *c) {
	u32 fd = A(0);
	if (fd == 1 || fd == 2) {
		fprintf(stderr, "%.*s", (int)A(2), Pstr(A(1)));
		RET(A(2));
		return;
	}
	PspFile *f = getf(fd);
	if (!f) { RET(0x80020323); return; }
	ssize_t n = write(f->hostfd, PTR(A(1)), A(2));
	RET(n < 0 ? 0x80010005 : n);
}
static void h_IoWriteAsync(Ctx *c) {
	PspFile *f = getf(A(0));
	if (!f) { RET(0x80020323); return; }
	ssize_t n = write(f->hostfd, PTR(A(1)), A(2));
	f->async_res = n; f->async_pending = 1;
	RET(0);
}
static s64 do_lseek(u32 fd, s64 off, u32 whence) {
	PspFile *f = getf(fd);
	if (!f) return (s32)0x80020323;
	if (f->win_len) {
		s64 base = whence == 1 ? lseek(f->hostfd, 0, SEEK_CUR) : whence == 2 ? f->win_off + f->win_len : f->win_off;
		off_t r = lseek(f->hostfd, base + off, SEEK_SET);
		return r < 0 ? (s32)0x80010016 : r - f->win_off;
	}
	off_t r = lseek(f->hostfd, off, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET);
	return r < 0 ? (s32)0x80010016 : r;
}
static void h_IoLseek(Ctx *c) { s64 off = (s64)(((u64)A(3) << 32) | A(2)); RET64(do_lseek(A(0), off, A(4))); }
static void h_IoLseek32(Ctx *c) { RET((s32)do_lseek(A(0), (s32)A(1), A(2))); }
static void h_IoLseekAsync(Ctx *c) {
	PspFile *f = getf(A(0));
	if (!f) { RET(0x80020323); return; }
	s64 off = (s64)(((u64)A(3) << 32) | A(2));
	f->async_res = do_lseek(A(0), off, A(4)); f->async_pending = 1;
	RET(0);
}
static void h_IoLseek32Async(Ctx *c) {
	PspFile *f = getf(A(0));
	if (!f) { RET(0x80020323); return; }
	f->async_res = do_lseek(A(0), (s32)A(1), A(2)); f->async_pending = 1;
	RET(0);
}
static void wait_async(Ctx *c, bool poll) {
	PspFile *f = getf(A(0));
	if (!f) { RET(0x80020323); return; }
	if (!f->async_pending) { RET(poll ? 1 : 0x80020329); return; }
	if (A(1)) { SW(A(1), (u32)f->async_res); SW(A(1) + 4, (u32)((u64)f->async_res >> 32)); }
	f->async_pending = 0;
	if (f->hostfd < 0) f->used = 0;  // async close completed
	RET(0);
}
static void h_IoWaitAsync(Ctx *c) { wait_async(c, false); }
static void h_IoPollAsync(Ctx *c) { wait_async(c, true); }
static void h_IoGetAsyncStat(Ctx *c) {
	PspFile *f = getf(A(0));
	if (!f) { RET(0x80020323); return; }
	u32 res = A(2);
	if (!f->async_pending) { RET(1); return; }
	SW(res, (u32)f->async_res); SW(res + 4, (u32)((u64)f->async_res >> 32));
	f->async_pending = 0;
	RET(0);
}

static void fill_time(u32 p, time_t t) {
	struct tm tm; localtime_r(&t, &tm);
	SH(p, tm.tm_year + 1900); SH(p + 2, tm.tm_mon + 1); SH(p + 4, tm.tm_mday);
	SH(p + 6, tm.tm_hour); SH(p + 8, tm.tm_min); SH(p + 10, tm.tm_sec); SW(p + 12, 0);
}
static void fill_stat(u32 p, struct stat *st) {
	memset(PTR(p), 0, 0x58);
	bool dir = S_ISDIR(st->st_mode);
	SW(p, dir ? 0x11FF : 0x21FF);
	SW(p + 4, dir ? 0x10 : 0x20);
	SW(p + 8, (u32)st->st_size); SW(p + 12, (u32)((u64)st->st_size >> 32));
	fill_time(p + 16, st->st_ctime); fill_time(p + 32, st->st_atime); fill_time(p + 48, st->st_mtime);
}
static void h_IoGetstat(Ctx *c) {
	char hp[1024];
	struct stat st;
	if (io_host_path(Pstr(A(0)), hp, sizeof hp) < 0 || stat(hp, &st) != 0) { LOGD("getstat %s -> not found", Pstr(A(0))); RET(SCE_ERROR_FILE_NOT_FOUND); return; }
	fill_stat(A(1), &st);
	const DiscFile *df = disc_by_host(hp);
	if (df) SW(A(1) + 0x40, df->lbn);   // st_private[0]: on UMD, the file's start sector
	RET(0);
}
static void h_IoDopen(Ctx *c) {
	char hp[1024];
	if (io_host_path(Pstr(A(0)), hp, sizeof hp) < 0) { RET(SCE_ERROR_FILE_NOT_FOUND); return; }
	DIR *d = opendir(hp);
	if (!d) { LOGD("dopen %s -> not found", Pstr(A(0))); RET(SCE_ERROR_FILE_NOT_FOUND); return; }
	int fd = alloc_fd();
	fds[fd].dir = d; fds[fd].hostfd = -1;
	snprintf(fds[fd].path, sizeof fds[fd].path, "%s", hp);
	RET(fd);
}
static void h_IoDread(Ctx *c) {
	PspFile *f = getf(A(0));
	if (!f || !f->dir) { RET(0x80020323); return; }
	struct dirent *e = readdir(f->dir);
	if (!e) { RET(0); return; }
	char full[1100]; struct stat st;
	snprintf(full, sizeof full, "%s/%s", f->path, e->d_name);
	stat(full, &st);
	fill_stat(A(1), &st);
	snprintf(Pstr(A(1) + 0x58), 256, "%s", e->d_name);
	RET(1);
}
static void h_IoDclose(Ctx *c) {
	PspFile *f = getf(A(0));
	if (!f || !f->dir) { RET(0x80020323); return; }
	closedir(f->dir); f->used = 0;
	RET(0);
}
static void h_IoMkdir(Ctx *c) {
	char hp[1024];
	if (io_host_path(Pstr(A(0)), hp, sizeof hp) < 0) { RET(SCE_ERROR_FILE_NOT_FOUND); return; }
	RET(mkdir(hp, 0755) == 0 ? 0 : 0x80010011);
}
static void h_IoRemove(Ctx *c) {
	char hp[1024];
	if (io_host_path(Pstr(A(0)), hp, sizeof hp) < 0) { RET(SCE_ERROR_FILE_NOT_FOUND); return; }
	RET(unlink(hp) == 0 ? 0 : SCE_ERROR_FILE_NOT_FOUND);
}
static void h_IoRename(Ctx *c) {
	char a[1024], b[1024];
	if (io_host_path(Pstr(A(0)), a, sizeof a) < 0 || io_host_path(Pstr(A(1)), b, sizeof b) < 0) { RET(SCE_ERROR_FILE_NOT_FOUND); return; }
	RET(rename(a, b) == 0 ? 0 : SCE_ERROR_FILE_NOT_FOUND);
}
static void h_IoDevctl(Ctx *c) {
	const char *dev = Pstr(A(0));
	u32 cmd = A(1), out = A(4), outlen = A(5);
	LOGD("sceIoDevctl(%s, %08x, in %x/%u, out %x/%u)", dev, cmd, A(2), A(3), out, outlen);
	if (out && outlen) memset(PTR(out), 0, outlen);
	bool ms = !strncmp(dev, "mscmhc0", 7) || !strncmp(dev, "ms0", 3) || !strncmp(dev, "fatms0", 6);
	if (ms) {
		switch (cmd) {
		case 0x02015804:  // register memory stick insert/eject callback (mscmhc0)
		case 0x02415821:  // same, fatms0
			if (A(2)) callback_notify(LW(A(2)), 1);  // 1 = inserted
			break;
		case 0x02025806: case 0x02425823: if (out) SW(out, 1); break;  // inserted / FAT ready
		case 0x02025801: if (out) SW(out, 4); break;  // state: assigned
		case 0x02425818:  // free space: {maxClusters, freeClusters, maxSectors, sectorSize, sectorCount}
			if (A(2)) {
				u32 p = LW(A(2));
				SW(p, 0x8000); SW(p + 4, 0x8000); SW(p + 8, 0x8000); SW(p + 12, 0x200); SW(p + 16, 0x40);
			}
			break;
		}
	}
	RET(0);
}
static void h_IoChangeAsyncPriority(Ctx *c) { RET(0); }
static void h_IoIoctl(Ctx *c) {
	PspFile *f = getf(A(0));
	u32 cmd = A(1), out = A(4), outlen = A(5);
	if (!f) { RET(0x80020323); return; }
	if (cmd == 0x01020004 && out && outlen >= 4) {   // UMD: file start sector
		const DiscFile *m = disc_by_host(f->host);
		u32 lbn = m ? m->lbn + (u32)(f->win_off / 2048) : 0;
		if (!m) LOGE("sceIoIoctl(%s): not a file of the disc, no start sector", f->path);
		SW(out, lbn);
		LOGD("sceIoIoctl(%s): start sector %x", f->path, lbn);
		RET(lbn ? 0 : 0x80010016);
		return;
	}
	LOGI("sceIoIoctl(fd %d, cmd %08x) not handled", A(0), cmd);
	RET(0);
}

// ---- Stdio
static void h_Stdin(Ctx *c) { RET(0); }
static void h_Stdout(Ctx *c) { RET(1); }
static void h_Stderr(Ctx *c) { RET(2); }
static void h_Printf(Ctx *c) {
	const char *fmt = Pstr(A(0));
	char out[2048]; size_t o = 0;
	int ai = 1;
	for (const char *p = fmt; *p && o < sizeof out - 64; p++) {
		if (*p != '%') { out[o++] = *p; continue; }
		char spec[16] = "%"; int sl = 1;
		p++;
		while (*p && strchr("-+ #0123456789.lh", *p) && sl < 14) spec[sl++] = *p++;
		if (!*p) break;
		spec[sl++] = *p == 'p' ? 'x' : *p; spec[sl] = 0;
		u32 v = ai < 8 ? A(ai) : 0; ai++;
		for (char *q = spec; *q; q++) if (*q == 'l' || *q == 'h') memmove(q, q + 1, strlen(q)), q--;
		switch (*p) {
		case 's': o += snprintf(out + o, sizeof out - o, spec, v ? Pstr(v) : "(null)"); break;
		case '%': out[o++] = '%'; ai--; break;
		case 'f': case 'g': case 'e': o += snprintf(out + o, sizeof out - o, "<float>"); break;
		default: o += snprintf(out + o, sizeof out - o, spec, v); break;
		}
	}
	out[o] = 0;
	fprintf(stderr, "[guest printf] %s", out);
	RET(0);
}

static const HleEnt io_ents[] = {
	{0x109F50BC, "sceIoOpen", h_IoOpen},
	{0x89AA9906, "sceIoOpenAsync", h_IoOpenAsync},
	{0x810C4BC3, "sceIoClose", h_IoClose},
	{0xFF5940B6, "sceIoCloseAsync", h_IoCloseAsync},
	{0x6A638D83, "sceIoRead", h_IoRead},
	{0xA0B5A7C2, "sceIoReadAsync", h_IoReadAsync},
	{0x42EC03AC, "sceIoWrite", h_IoWrite},
	{0x0FACAB19, "sceIoWriteAsync", h_IoWriteAsync},
	{0x27EB27B8, "sceIoLseek", h_IoLseek},
	{0x68963324, "sceIoLseek32", h_IoLseek32},
	{0x71B19E77, "sceIoLseekAsync", h_IoLseekAsync},
	{0x1B385D8F, "sceIoLseek32Async", h_IoLseek32Async},
	{0xE23EEC33, "sceIoWaitAsync", h_IoWaitAsync},
	{0x35DBD746, "sceIoWaitAsyncCB", h_IoWaitAsync},
	{0x3251EA56, "sceIoPollAsync", h_IoPollAsync},
	{0xCB05F8D6, "sceIoGetAsyncStat", h_IoGetAsyncStat},
	{0xACE946E8, "sceIoGetstat", h_IoGetstat},
	{0xB29DDF9C, "sceIoDopen", h_IoDopen},
	{0xE3EB004C, "sceIoDread", h_IoDread},
	{0xEB092469, "sceIoDclose", h_IoDclose},
	{0x06A70004, "sceIoMkdir", h_IoMkdir},
	{0xF27A9C51, "sceIoRemove", h_IoRemove},
	{0x779103A0, "sceIoRename", h_IoRename},
	{0x54F5FB11, "sceIoDevctl", h_IoDevctl},
	{0x63632449, "sceIoIoctl", h_IoIoctl},
	{0xB293727F, "sceIoChangeAsyncPriority", h_IoChangeAsyncPriority},
	{0x13A5ABEF, "sceKernelPrintf", h_Printf},
	{0x172D316E, "sceKernelStdin", h_Stdin},
	{0xA6BAB2E9, "sceKernelStdout", h_Stdout},
	{0xF78BA90A, "sceKernelStderr", h_Stderr},
	{0x0CBB0571, "sceKernelStdioLseek", h_IoLseek},
	{0xA3B931DB, "sceKernelStdioWrite", h_IoWrite},
	{0x3054D478, "sceKernelStdioRead", h_IoRead},
	{0x9D061C19, "sceKernelStdioClose", h_IoClose},
};

void io_init(void) {
	hle_register(io_ents, sizeof io_ents / sizeof io_ents[0]);
}
