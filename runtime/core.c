// Core runtime: memory, logging, HLE dispatch, guest call dispatch, module loading, main().
#define _GNU_SOURCE
#include "kernel.h"
#include <stdarg.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#ifdef _WIN32
// (the backtraces are only used by the memory-watch debug tools)
static int backtrace(void **f, int n) { (void)f; (void)n; return 0; }
static char **backtrace_symbols(void *const *f, int n) { (void)f; (void)n; return NULL; }
#else
#include <execinfo.h>
#endif
#ifndef _WIN32
#include <fnmatch.h>
#endif
#include <ctype.h>
#include <strings.h>

u8 *g_mem;
int g_loglevel = 1;
char g_game_root[512];
char g_save_root[512];

void logf_(int lvl, const char *fmt, ...) {
	if (lvl > g_loglevel) return;
	va_list a;
	va_start(a, fmt);
	fprintf(stderr, "[%9.3f %-12.12s] ", now_us() / 1e6, g_cur ? g_cur->name : "-");
	vfprintf(stderr, fmt, a);
	fputc('\n', stderr);
	va_end(a);
}

void fatal(const char *fmt, ...) {
	va_list a;
	va_start(a, fmt);
	fprintf(stderr, "\n*** FATAL [%s]: ", g_cur ? g_cur->name : "-");
	vfprintf(stderr, fmt, a);
	fputc('\n', stderr);
	va_end(a);
	if (g_cur) {
		Ctx *c = &g_cur->ctx;
		fprintf(stderr, "  ra=%08x sp=%08x v0=%08x a0=%08x a1=%08x a2=%08x a3=%08x\n", c->r[31], c->r[29], c->r[2], c->r[4], c->r[5], c->r[6], c->r[7]);
		fprintf(stderr, "  backtrace (guest return addresses on stack, best effort):");
		u32 sp = c->r[29];
		int n = 0;
		for (u32 i = 0; i < 1024 && n < 16; i++) {
			u32 v = LW(sp + i * 4);
			LoadedModule *m = module_by_addr(v);
			if (m && v > m->base + m->text_start && v < m->base + m->text_end && (v & 3) == 0) {
				u32 prev = LW(v - 8);
				if ((prev >> 26) == 3) { fprintf(stderr, " %s+%x", m->name, v - m->base); n++; }
			}
		}
		fprintf(stderr, "\n");
	}
	fflush(stderr);
	host_pause_before_exit();
	exit(1);
}

// ---------------------------------------------------------------- HLE dispatch
#define HLE_HASH 4096
static struct { u32 nid; const char *name; HleFn fn; int calls; } hle_tab[HLE_HASH];

void hle_register(const HleEnt *ents, int n) {
	for (int i = 0; i < n; i++) {
		u32 h = (ents[i].nid * 2654435761u) >> 20;
		while (hle_tab[h].nid && hle_tab[h].nid != ents[i].nid) h = (h + 1) & (HLE_HASH - 1);
		hle_tab[h].nid = ents[i].nid;
		hle_tab[h].name = ents[i].name;
		hle_tab[h].fn = ents[i].fn;
	}
}

HleFn hle_lookup(u32 nid) {
	u32 h = (nid * 2654435761u) >> 20;
	while (hle_tab[h].nid && hle_tab[h].nid != nid) h = (h + 1) & (HLE_HASH - 1);
	return hle_tab[h].nid == nid ? hle_tab[h].fn : NULL;
}

void HLE(Ctx *c, u32 nid) {
	u32 h = (nid * 2654435761u) >> 20;
	while (hle_tab[h].nid && hle_tab[h].nid != nid) h = (h + 1) & (HLE_HASH - 1);
	if (hle_tab[h].nid == nid && hle_tab[h].fn) {
		hle_tab[h].calls++;
		if (g_loglevel >= 3)
			LOGV("%s(%x, %x, %x, %x, %x)", hle_tab[h].name, A(0), A(1), A(2), A(3), A(4));
		hle_tab[h].fn(c);
		return;
	}
	if (hle_tab[h].nid != nid) {
		hle_tab[h].nid = nid;
		hle_tab[h].name = nid_name(nid);
	}
	// a function another loaded module exports (the game's own PRX libraries, e.g. RCPM for multiplayer)
	for (LoadedModule *m = g_modules; m; m = m->next)
		for (u32 i = 0; i < m->nexp; i++)
			if (m->exp_nids[i] == nid) {
				u32 a0 = A(0), a1 = A(1), a2 = A(2), a3 = A(3), ra = c->r[31];
				rt_call(c, m->exp_addrs[i]);
				if (g_loglevel >= 2 && hle_tab[h].calls++ < 40)
					LOGI("link %s:%08x(%x, %x, %x, %x) -> %x  ra=%08x", m->name, nid, a0, a1, a2, a3, c->r[2], ra);
				return;
			}
	if (hle_tab[h].calls++ < 3)
		LOGI("UNIMPLEMENTED %s (%08x) args %x %x %x %x  ra=%08x", hle_tab[h].name, nid, A(0), A(1), A(2), A(3), c->r[31]);
	RET(0);
}

// ---------------------------------------------------------------- memory watch (debug builds)
u32 g_watch_lo = 0xFFFFFFFF, g_watch_hi = 0;
const char *g_trace_fn;
void rt_trace(u32 a, int size, int write, u32 val, const char *fn) {
	float f; memcpy(&f, &val, 4);
	LOGI("trace: %s vb %llu store%d %08x = %08x (%g)", fn, (unsigned long long)g_vblank_count, size, a, val, f);
}
void rt_watch(u32 a, int size, int write, u32 val, const char *fn) {
	// one line per distinct (function, address, kind), plus a count on every 64th repeat
	static struct { const char *fn; u32 a; int w; u32 n; } seen[4096];
	static int nseen;
	static int all = -1;
	if (all < 0) all = getenv("SR2_WATCH_ALL") != NULL;
	if (all) {
		if (write) LOGI("watchall: vb %llu %s WRITE %08x = %08x", (unsigned long long)g_vblank_count, fn, a, val);
		if (!write || !getenv("SR2_WATCH_BT")) return;
		void *frames[32];
		int n = backtrace(frames, 32);
		char **sy = backtrace_symbols(frames, n);
		char line[3000] = "";
		for (int i = 1; i < n && sy; i++) {
			char *p = strchr(sy[i], '('), *q = p ? strchr(p, '+') : NULL;
			if (p && q && q > p + 1) { strncat(line, " <- ", sizeof line - strlen(line) - 1); strncat(line, p + 1, q - p - 1 < 60 ? q - p - 1 : 60); }
		}
		LOGI("watch bt:%s", line);
		free(sy);
		return;
	}
	if (getenv("SR2_WATCH_BT") && atoi(getenv("SR2_WATCH_BT")) == 2) {   // SR2_WATCH_BT=2: one line per distinct call path, reads too
		void *frames[8];
		int n = backtrace(frames, 8);
		static void *paths[4096][4]; static int np;
		for (int i = 0; i < np; i++) if (!memcmp(paths[i], frames + 2, sizeof paths[i])) return;
		if (np < 4096) memcpy(paths[np++], frames + 2, sizeof paths[0]);
		char **sy = backtrace_symbols(frames, n);
		char line[1000] = "";
		for (int i = 1; i < n && sy; i++) {
			char *p = strchr(sy[i], '('), *q = p ? strchr(p, '+') : NULL;
			if (p && q && q > p + 1) { strncat(line, " <- ", sizeof line - strlen(line) - 1); strncat(line, p + 1, q - p - 1 < 60 ? q - p - 1 : 60); }
		}
		LOGI("watch path %s vb %llu:%s", write ? "W" : "R", (unsigned long long)g_vblank_count, line);
		free(sy);
		return;
	}
	for (int i = 0; i < nseen; i++)
		if (seen[i].fn == fn && seen[i].a == a && seen[i].w == write) {
			if ((++seen[i].n & 63) == 0) LOGI("watch: %s %s %08x (+%x) = %x  [x%u]", fn, write ? "WRITE" : "read ", a, a - g_watch_lo, val, seen[i].n);
			return;
		}
	if (nseen < 4096) seen[nseen++] = (typeof(seen[0])){fn, a, write, 1};
	static int bt = -1;
	if (bt < 0) bt = getenv("SR2_WATCH_BT") != NULL;
	if (bt && write) {
		void *frames[24];
		int n = backtrace(frames, 24);
		char **sy = backtrace_symbols(frames, n);
		char line[2048] = "";
		for (int i = 1; i < n && sy; i++) {
			char *p = strchr(sy[i], '('), *q = p ? strchr(p, '+') : NULL;
			if (p && q && q > p + 1) { strncat(line, " <- ", sizeof line - strlen(line) - 1); strncat(line, p + 1, q - p - 1 < 60 ? q - p - 1 : 60); }
		}
		LOGI("watch bt:%s", line);
		free(sy);
	}
	LOGI("watch: %s %s %08x (+%x) size %d = %x  (vblank %llu)", fn, write ? "WRITE" : "read ", a, a - g_watch_lo, size, val, (unsigned long long)g_vblank_count);
}

// VFPU stores (sv.s/sv.q run in the interpreter): named by the guest pc of the instruction
Ctx *g_interp_ctx;
void rt_watch_vfpu(u32 a, u32 val) {
	static char names[64][24]; static u32 pcs[64]; static int n;
	u32 pc = g_interp_ctx ? g_interp_ctx->pc : 0;
	int i = 0;
	while (i < n && pcs[i] != pc) i++;
	if (i == n) { if (n == 64) i = 63; else n++; pcs[i] = pc; snprintf(names[i], sizeof names[i], "vfpu@%08x", pc); }
	rt_watch(a, 4, 1, val, names[i]);
}

// ---------------------------------------------------------------- guest calls
LoadedModule *g_modules;

LoadedModule *module_by_addr(u32 addr) {
	for (LoadedModule *m = g_modules; m; m = m->next)
		if (addr >= m->base && addr < m->base + m->size) return m;
	return NULL;
}

static RecompFn lookup_fn(u32 addr, LoadedModule **mout) {
	LoadedModule *m = module_by_addr(addr);
	if (mout) *mout = m;
	if (!m) return NULL;
	u32 off = addr - m->base;
	if (off < m->text_start || off >= m->text_end || (off & 3)) return NULL;
	return m->fntab[(off - m->text_start) >> 2];
}

void rt_call(Ctx *c, u32 addr) {
	LoadedModule *m;
	RecompFn f = lookup_fn(addr, &m);
	if (f) { f(c); return; }
	if (m && addr - m->base >= m->stub_lo && addr - m->base < m->stub_hi) {
		u32 idx = (addr - m->base - m->stub_lo) / 8;
		HLE(c, m->stub_nids[idx]);
		return;
	}
	if (!addr) for (int i = 0; i < 32; i += 8)
		LOGI("r%d: %08x %08x %08x %08x %08x %08x %08x %08x", i, c->r[i], c->r[i+1], c->r[i+2], c->r[i+3], c->r[i+4], c->r[i+5], c->r[i+6], c->r[i+7]);
	if (!addr) for (u32 a = c->r[29]; a < c->r[29] + 0x200; a += 32)
		LOGI("sp+%03x: %08x %08x %08x %08x %08x %08x %08x %08x", a - c->r[29], LW(a), LW(a+4), LW(a+8), LW(a+12), LW(a+16), LW(a+20), LW(a+24), LW(a+28));
	fatal("call to unknown address %08x (%s+%x). Add it to the module's extra seeds and re-run the recompiler.",
	      addr, m ? m->name : "?", m ? addr - m->base : addr);
}

void rt_jump(Ctx *c, u32 addr) { rt_call(c, addr); }

void rt_bad_insn(Ctx *c, u32 pc, u32 op) { fatal("bad instruction %08x at %08x", op, pc); }
void rt_syscall(Ctx *c, u32 pc, u32 op) { fatal("raw syscall %08x at %08x", op, pc); }
void rt_break(Ctx *c, u32 pc, u32 op) {
	LoadedModule *m = module_by_addr(pc);
	fatal("guest executed 'break' (assert/abort) at %08x (%s+%x) code %x", pc, m ? m->name : "?", m ? pc - m->base : 0, op >> 6);
}

// ---------------------------------------------------------------- module loading
extern const RecompModule *const g_recomp_modules[];
extern const int g_recomp_module_count;

static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u16 rd16(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }

static void apply_relocs(const u8 *file, u32 base, const u32 *segv, int nseg) {
	u32 shoff = rd32(file + 32);
	u16 shentsize = rd16(file + 46), shnum = rd16(file + 48);
	for (int s = 0; s < shnum; s++) {
		const u8 *sh = file + shoff + s * shentsize;
		if (rd32(sh + 4) != 0x700000A0) continue;
		u32 off = rd32(sh + 16), size = rd32(sh + 20);
		int n = size / 8;
		const u8 *rel = file + off;
		u32 *ops = malloc(n * 4);
		for (int i = 0; i < n; i++) {
			u32 info = rd32(rel + i * 8 + 4);
			ops[i] = LW(base + rd32(rel + i * 8) + segv[(info >> 8) & 0xFF]);
		}
		for (int i = 0; i < n; i++) {
			u32 roff = rd32(rel + i * 8), info = rd32(rel + i * 8 + 4);
			int type = info & 0xFF, oseg = (info >> 8) & 0xFF, tseg = (info >> 16) & 0xFF;
			if (oseg >= nseg) continue;
			u32 addr = base + roff + segv[oseg];
			u32 to = base + (tseg < nseg ? segv[tseg] : 0);
			u32 op = ops[i];
			switch (type) {
			case 2: op += to; break;
			case 4: op = (op & 0xFC000000) | (((op & 0x03FFFFFF) + (to >> 2)) & 0x03FFFFFF); break;
			case 5:
				for (int j = i + 1; j < n; j++) {
					u32 jinfo = rd32(rel + j * 8 + 4);
					if ((jinfo & 0xFF) == 5) continue;
					u32 full = ((op & 0xFFFF) << 16) + (u32)(s32)(s16)(ops[j] & 0xFFFF) + to;
					u32 hi = ((full >> 16) + ((full & 0x8000) ? 1 : 0)) & 0xFFFF;
					op = (op & 0xFFFF0000) | hi;
					break;
				}
				break;
			case 6: case 1: op = (op & 0xFFFF0000) | ((op + to) & 0xFFFF); break;
			default: continue;
			}
			SW(addr, op);
		}
		free(ops);
	}
}

static u32 g_next_mod_uid = 1;

// Data patches applied right after a module is loaded and relocated.
// Spec: "glob@offset=value;..."  glob matches the lower-case file name without extension (e.g. level_*),
// offset is module-relative hex, value is an integer (dec/0x) or a float written as f1.5.
// Source: the SR2_PATCH env var (testing).
static void apply_patch_spec(LoadedModule *m, const char *key, const char *spec) {
	if (!spec) return;
	char *dup = strdup(spec), *save = NULL;
	for (char *t = strtok_r(dup, ";, \n", &save); t; t = strtok_r(NULL, ";, \n", &save)) {
		char *at = strchr(t, '@'), *eq = at ? strchr(at, '=') : NULL;
		if (!at || !eq) continue;
		*at = 0; *eq = 0;
		if (fnmatch(t, key, 0) != 0) continue;
		u32 off = strtoul(at + 1, NULL, 16), val;
		if (eq[1] == 'f') { float f = strtof(eq + 2, NULL); memcpy(&val, &f, 4); }
		else val = strtoul(eq + 1, NULL, 0);
		if (off + 4 > m->size) { LOGE("patch %s@%x outside module", key, off); continue; }
		LOGD("patch %s+%x: %08x -> %08x", key, off, LW(m->base + off), val);
		SW(m->base + off, val);
	}
	free(dup);
}
void module_apply_patches(LoadedModule *m) {
	char key[64];
	const char *b = strrchr(m->path, '/');
	snprintf(key, sizeof key, "%s", b ? b + 1 : m->path);
	for (char *p = key; *p; p++) *p = tolower(*p);
	char *dot = strrchr(key, '.');
	if (dot) *dot = 0;
	apply_patch_spec(m, key, getenv("SR2_PATCH"));
}

static LoadedModule *module_load_from(const char *psp_path, const char *hp, s32 *err);
LoadedModule *module_load(const char *psp_path, s32 *err) {
	char hp[1024], own[1024];
	if (io_host_path(psp_path, hp, sizeof hp) < 0) { *err = SCE_ERROR_FILE_NOT_FOUND; return NULL; }
	LoadedModule *m = module_load_from(psp_path, hp, err);
	// a mod's copy of a program (made for PPSSPP, where code can be patched) can't run here: the code is the
	// recompiled one. Use the game's own copy
	if (!m && io_game_path(psp_path, own, sizeof own) == 0 && strcmp(own, hp)) {
		LOGE("module_load: ignoring a mod's %s (%s): this build can only run the game's own code", psp_path, hp);
		m = module_load_from(psp_path, own, err);
	}
	return m;
}
static LoadedModule *module_load_from(const char *psp_path, const char *hp, s32 *err) {
	FILE *f = fopen(hp, "rb");
	if (!f) { LOGE("module_load: cannot open %s (%s)", psp_path, hp); *err = SCE_ERROR_FILE_NOT_FOUND; return NULL; }
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8 *file = malloc(sz);
	if (fread(file, 1, sz, f) != (size_t)sz) { fclose(f); free(file); *err = SCE_ERROR_FILE_NOT_FOUND; return NULL; }
	fclose(f);
	if (rd32(file) != 0x464C457F) {
		LOGE("module_load: %s (%s) is not a plain ELF (encrypted ~PSP module?) magic %08x", psp_path, hp, rd32(file));
		free(file); *err = 0x80020148; return NULL;
	}
	u32 crc = crc32(0, file, sz);
	const RecompModule *rm = NULL;
	for (int i = 0; i < g_recomp_module_count; i++)
		if (g_recomp_modules[i]->crc == crc && g_recomp_modules[i]->file_size == (u32)sz) rm = g_recomp_modules[i];
	if (!rm) {
		// another dump of the same program (decrypters differ in the section headers and padding they keep):
		// what gets loaded is all the recompiled code depends on
		u32 lcrc = 0;
		u32 phoff = rd32(file + 28);
		u16 phentsize = rd16(file + 42), phnum = rd16(file + 44);
		bool ok = phoff + (u32)phnum * phentsize <= (u32)sz;
		for (int i = 0; ok && i < phnum; i++) {
			const u8 *ph = file + phoff + i * phentsize;
			u32 off = rd32(ph + 4), fsz = rd32(ph + 16);
			if (rd32(ph) != 1) continue;
			if (off > (u32)sz || fsz > (u32)sz - off) ok = false;
			else lcrc = crc32(lcrc, file + off, fsz);
		}
		for (int i = 0; ok && i < g_recomp_module_count; i++)
			if (g_recomp_modules[i]->load_crc && g_recomp_modules[i]->load_crc == lcrc) rm = g_recomp_modules[i];
		if (rm) LOGI("module_load: %s is another dump of %s (same loaded segments)", psp_path, rm->name);
	}
	if (!rm) {
		LOGE("module_load: %s (%s, crc %08x size %ld) has no recompiled code linked in. Recompile it and rebuild.", psp_path, hp, crc, sz);
		free(file); *err = 0x80020148; return NULL;
	}
	// segments
	u32 phoff = rd32(file + 28), e_entry = rd32(file + 24);
	u16 phentsize = rd16(file + 42), phnum = rd16(file + 44);
	u32 segv[8] = {0}, img = 0;
	for (int i = 0; i < phnum && i < 8; i++) {
		const u8 *ph = file + phoff + i * phentsize;
		segv[i] = rd32(ph + 8);
		u32 e = rd32(ph + 8) + rd32(ph + 20);
		if (e > img) img = e;
	}
	LoadedModule *m = calloc(1, sizeof *m);
	m->rm = rm;
	m->size = img;
	m->base = mem_alloc(2, psp_path, 0, img, 0, 0x10000, &m->mem_uid);
	if (!m->base) fatal("out of memory loading %s (%u bytes)", psp_path, img);
	memset(PTR(m->base), 0, img);
	for (int i = 0; i < phnum && i < 8; i++) {
		const u8 *ph = file + phoff + i * phentsize;
		if (rd32(ph) != 1) continue;   // PT_LOAD only (some PRXs also have a relocation segment at vaddr 0)
		memcpy(PTR(m->base + rd32(ph + 8)), file + rd32(ph + 4), rd32(ph + 16));
	}
	apply_relocs(file, m->base, segv, phnum);
	*rm->mb = m->base;
	// module info via section header
	u32 shoff = rd32(file + 32);
	u16 shentsize = rd16(file + 46), shnum = rd16(file + 48), shstr = rd16(file + 50);
	const u8 *strsh = file + shoff + shstr * shentsize;
	u32 modinfo = 0;
	for (int s = 0; s < shnum; s++) {
		const u8 *sh = file + shoff + s * shentsize;
		const char *nm = (const char *)file + rd32(strsh + 16) + rd32(sh);
		if (!strcmp(nm, ".rodata.sceModuleInfo")) modinfo = m->base + rd32(sh + 12);
	}
	free(file);
	if (!modinfo) fatal("no module info in %s", psp_path);
	snprintf(m->name, sizeof m->name, "%s", Pstr(modinfo + 4));
	snprintf(m->path, sizeof m->path, "%s", psp_path);
	m->gp = LW(modinfo + 32);
	u32 ent_top = LW(modinfo + 36), ent_end = LW(modinfo + 40), stub_top = LW(modinfo + 44), stub_end = LW(modinfo + 48);
	m->start_prio = 0x20; m->start_stack = 0x40000; m->start_attr = 0;
	m->entry = 0; m->stop = 0;
	m->exp_nids = m->exp_addrs = NULL; m->nexp = 0;
	m->expv_nids = m->expv_addrs = NULL; m->nexpv = 0;
	for (u32 a = ent_top; a < ent_end;) {
		u32 size = LBU(a + 8), nvars = LBU(a + 9), nfuncs = LHU(a + 10), tbl = LW(a + 12);
		u32 nt = nvars + nfuncs;
		if (LW(a) && nfuncs) {   // a named library: its functions can be imported by modules loaded later
			m->exp_nids = realloc(m->exp_nids, (m->nexp + nfuncs) * 4);
			m->exp_addrs = realloc(m->exp_addrs, (m->nexp + nfuncs) * 4);
			for (u32 i = 0; i < nfuncs; i++) {
				m->exp_nids[m->nexp] = LW(tbl + 4 * i);
				m->exp_addrs[m->nexp++] = LW(tbl + 4 * (nt + i));
			}
		}
		if (LW(a) && nvars) {
			m->expv_nids = realloc(m->expv_nids, (m->nexpv + nvars) * 4);
			m->expv_addrs = realloc(m->expv_addrs, (m->nexpv + nvars) * 4);
			for (u32 i = 0; i < nvars; i++) {
				m->expv_nids[m->nexpv] = LW(tbl + 4 * (nfuncs + i));
				m->expv_addrs[m->nexpv++] = LW(tbl + 4 * (nt + nfuncs + i));
			}
		}
		for (u32 i = 0; i < nt; i++) {
			u32 nid = LW(tbl + 4 * i), val = LW(tbl + 4 * (nt + i));
			if (nid == 0xD632ACDB) m->entry = val;
			else if (nid == 0xCEE8593C) m->stop = val;
			else if (nid == 0x0F7C276C && LW(val) >= 3) { m->start_prio = LW(val + 4); m->start_stack = LW(val + 8); m->start_attr = LW(val + 12); }
		}
		a += (size < 4 ? 4 : size) * 4;
	}
	if (!m->entry) m->entry = m->base + e_entry;
	// imports: map stubs to nids (for indirect calls through stubs)
	u32 lo = 0xFFFFFFFF, hi = 0, total = 0;
	for (u32 a = stub_top; a < stub_end; a += LBU(a + 8) * 4) {
		u32 nf = LHU(a + 10), sym = LW(a + 16);
		if (sym - m->base < lo) lo = sym - m->base;
		if (sym - m->base + nf * 8 > hi) hi = sym - m->base + nf * 8;
		total += nf;
	}
	if (total) {
		m->stub_lo = lo; m->stub_hi = hi;
		m->stub_nids = calloc((hi - lo) / 8 + 1, 4);
		for (u32 a = stub_top; a < stub_end; a += LBU(a + 8) * 4) {
			u32 nf = LHU(a + 10), nidp = LW(a + 12), sym = LW(a + 16);
			for (u32 i = 0; i < nf; i++) {
				m->stub_nids[(sym - m->base - lo) / 8 + i] = LW(nidp + 4 * i);
				SW(sym + 8 * i, 0x03E00008);   // jr ra
				SW(sym + 8 * i + 4, 0x0000000C | ((0xFFF) << 6)); // syscall (never executed natively)
			}
		}
	}
	// imported variables: the recompiled code reads each one's address from its import entry's nid word
	for (u32 a = stub_top; a < stub_end; a += LBU(a + 8) * 4) {
		u32 nv = LBU(a + 9), varp = LBU(a + 8) >= 6 ? LW(a + 20) : 0;
		for (u32 i = 0; varp && i < nv; i++) {
			u32 nid = LW(varp + 8 * i + 4), addr = 0;
			for (LoadedModule *o = g_modules; o && !addr; o = o->next)
				for (u32 k = 0; k < o->nexpv; k++) if (o->expv_nids[k] == nid) { addr = o->expv_addrs[k]; break; }
			if (!addr) LOGI("module %s: imported variable %08x (%s) isn't exported by any loaded module", m->name, nid, Pstr(LW(a)));
			else LOGD("module %s: imported variable %08x -> %08x", m->name, nid, addr);
			SW(varp + 8 * i + 4, addr);
		}
	}
	m->text_start = rm->text_start;
	m->text_end = rm->text_end;
	m->fntab = calloc((m->text_end - m->text_start) / 4, sizeof(RecompFn));
	for (u32 i = 0; i < rm->nfuncs; i++) m->fntab[(rm->funcs[i].off - m->text_start) / 4] = rm->funcs[i].fn;
	m->uid = obj_new(OBJ_MODULE, m);
	m->next = g_modules;
	g_modules = m;
	module_apply_patches(m);
	LOGI("loaded module '%s' from %s at %08x-%08x (gp %08x, entry %08x, %u recompiled functions)",
	     m->name, psp_path, m->base, m->base + m->size, m->gp, m->entry, rm->nfuncs);
	return m;
}

static void usage(void) {
	fprintf(stderr, "usage: sr2recomp [folder containing PSP_GAME | your game.iso] [--2p] [--log N]\n"
	                "  (no folder: the current directory, \"game\" or the folder sr2recomp is in;\n"
	                "   an .iso is unpacked once into \"game\" next to sr2recomp)\n"
	                "  env: SR2_HEADLESS=1, SR2_SCALE=N, SR2_SAVE_DIR=dir, PSPGPU_THREADS=N, SR2_LOG=N\n");
	host_pause_before_exit();
	exit(2);
}

void vfpu_init(void);

// Does `root` (with PSP_GAME/SYSDIR in any case) hold the game? Sets g_game_root to it when it does.
// Also accepts the PSP_GAME folder itself.
static bool try_game_root(const char *root) {
	snprintf(g_game_root, sizeof g_game_root, "%s", root);
	size_t gl = strlen(g_game_root);
	while (gl > 1 && g_game_root[gl - 1] == '/') g_game_root[--gl] = 0;
	struct stat st;
	char hp[1024];
	if (io_host_path("disc0:/PSP_GAME/SYSDIR", hp, sizeof hp) == 0 && stat(hp, &st) == 0 && S_ISDIR(st.st_mode)) return true;
	if (gl > 8 && !strcasecmp(g_game_root + gl - 8, "PSP_GAME") && (gl == 8 || g_game_root[gl - 9] == '/')) {
		g_game_root[gl > 9 ? gl - 9 : 1] = 0;
		if (!g_game_root[0]) strcpy(g_game_root, "/");
		if (io_host_path("disc0:/PSP_GAME/SYSDIR", hp, sizeof hp) == 0 && stat(hp, &st) == 0 && S_ISDIR(st.st_mode)) return true;
	}
	return false;
}

bool g_start_2p;   // --2p: open player 2 (split screen) at start

int main(int argc, char **argv) {
	// The game folder: the arguments before the first option. Several of them are joined with spaces, so an
	// unquoted path with spaces in it still works. No folder given: the current directory, then the folder
	// the executable is in.
	char given[1024] = "";
	int npath = 0;
	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--", 2)) {
			if (!strcmp(argv[i], "--log") && i + 1 < argc) g_loglevel = atoi(argv[++i]);
			else if (!strcmp(argv[i], "--help")) usage();
			else if (!strcmp(argv[i], "--2p")) g_start_2p = true;
			continue;
		}
		if (npath++) strncat(given, " ", sizeof given - strlen(given) - 1);
		strncat(given, argv[i], sizeof given - strlen(given) - 1);
	}
#ifdef _WIN32
	for (char *p = given; *p; p++) if (*p == '\\') *p = '/';
#endif
	host_init();
	const char *w = getenv("SR2_WATCH");  // "lo-hi" hex guest addresses (needs a watch build of the module)
	if (w) { g_watch_lo = strtoul(w, NULL, 16) & MEM_MASK; const char *d = strchr(w, '-'); g_watch_hi = d ? strtoul(d + 1, NULL, 16) & MEM_MASK : g_watch_lo + 4; }
	g_trace_fn = getenv("SR2_TRACE_FN");
	const char *e = getenv("SR2_LOG");
	if (e) g_loglevel = atoi(e);
	// A disc image instead of a folder: unpack it once into "game" next to the program, then run from there.
	size_t gl0 = strlen(given);
	if (gl0 > 4 && !strcasecmp(given + gl0 - 4, ".iso")) {
		extern bool iso_unpack(const char *iso_path, const char *out_dir);
		char out[1024], sysdir[1100];
		if (host_exe_path(out, sizeof out) == 0 && strrchr(out, '/')) strcpy(strrchr(out, '/'), "/game");
		else snprintf(out, sizeof out, "game");
		snprintf(sysdir, sizeof sysdir, "%s/PSP_GAME/SYSDIR/BOOT.BIN", out);
		struct stat st;
		if (stat(sysdir, &st) != 0 && !iso_unpack(given, out)) { host_pause_before_exit(); return 1; }
		snprintf(given, sizeof given, "%s", out);
	}
	{
		bool ok = false;
		if (npath) {
			ok = try_game_root(given);
			// (several words, and the joined path didn't work: maybe the first one alone is the folder)
			if (!ok && npath > 1) for (int i = 1; i < argc && !ok; i++) if (strncmp(argv[i], "--", 2)) { ok = try_game_root(argv[i]); break; }
		} else {
			ok = try_game_root(".");
			if (!ok) ok = try_game_root("game");
			if (!ok) {
				char exe[1024];
				if (host_exe_path(exe, sizeof exe) == 0) { char *sl = strrchr(exe, '/'); if (sl) { *sl = 0; ok = try_game_root(exe[0] ? exe : "/"); } }
				// the setup script extracts the disc into "game" next to the program
				if (!ok && host_exe_path(exe, sizeof exe) == 0) { char *sl = strrchr(exe, '/'); if (sl) { strcpy(sl, "/game"); ok = try_game_root(exe); } }
				if (!ok && host_exe_path(exe, sizeof exe) == 0) { char *sl = strrchr(exe, '/'); if (sl) { *sl = 0; sl = strrchr(exe, '/'); if (sl) { strcpy(sl, "/game"); ok = try_game_root(exe); } } }
			}
			if (!ok) usage();
		}
		if (!ok)
			fatal("'%s' doesn't contain PSP_GAME/SYSDIR. Pass the folder that holds PSP_GAME (put the path in quotes "
			      "if it has spaces), or run sr2recomp from that folder with no arguments.", given);
		{ char rp[PATH_MAX]; if (realpath(g_game_root, rp)) snprintf(g_game_root, sizeof g_game_root, "%s", rp); }
		LOGI("game folder: %s", g_game_root);
	}
	// Saves: $SR2_SAVE_DIR, else $XDG_DATA_HOME/sr2recomp, else ~/.local/share/sr2recomp  (memory stick root)
	const char *sd = getenv("SR2_SAVE_DIR");
	if (sd) snprintf(g_save_root, sizeof g_save_root, "%s", sd);
	else host_data_dir(g_save_root, sizeof g_save_root);
	{
		char d[600];
		snprintf(d, sizeof d, "%s", g_save_root);
		for (char *p = d + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(d, 0755); *p = '/'; }
		mkdir(d, 0755);
	}
	g_mem = host_mem_reserve(0x40000000);
	if (!g_mem) { perror("guest memory"); return 1; }
	vfpu_init();
	kernel_init();
	io_init();
	{ void charslot_init(void); charslot_init(); }
	display_init();
	ge_init();
	hle_misc_init();
	extern void fe_init(void);
	extern void fe_preinit(void);
	extern void ge_soft_init(void);
	fe_preinit();   // loads settings, picks the GPU backend
	ge_soft_init();
	extern void audio_hle_init(void), sas_init(void), atrac_init(void);
	if (getenv("SR2_PPSSPP_AUDIO") && atoi(getenv("SR2_PPSSPP_AUDIO"))) {
		audio_hle_init();
	} else {
		sas_init();
		atrac_init();
	}
	extern void utility_hle_init(void);
	utility_hle_init();
	extern void audio_ch_init(void);
	audio_ch_init();
	extern void net_init(void);
	net_init();
	fe_init();

	s32 err = 0;
	LoadedModule *m = module_load("disc0:/PSP_GAME/SYSDIR/EBOOT.BIN", &err);
	if (!m) {
		LOGI("EBOOT.BIN unusable (%08x); trying SYSDIR/BOOT.BIN", err);
		m = module_load("disc0:/PSP_GAME/SYSDIR/BOOT.BIN", &err);
	}
	if (!m) fatal("could not load the game executable (error %08x). SYSDIR/EBOOT.BIN must be the decrypted ELF this build was generated from.", err);
	LOGI("game: %s   saves: %s", g_game_root, g_save_root);
	// The kernel passes the boot path as the argument block.
	const char *bootpath = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
	u32 arg = kalloc(64);
	strcpy(Pstr(arg), bootpath);
	Thread *t = thread_create("user_main", m->entry, m->start_prio, m->start_stack, m->start_attr | 0x80000000, m->gp);
	thread_start(t, strlen(bootpath) + 1, arg);
	sched_run();
}
