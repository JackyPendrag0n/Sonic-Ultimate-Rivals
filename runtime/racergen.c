// Racer files for other load slots, built in memory: the C version of tools/racergen.py (see its notes).
// A racer file (STREAMS/RACER_<CHAR>_<n>.STR) is a raw memory image made for one slot at one address; lining
// up _0 (slot 14) and _1 (slot 15) shows every word that depends on the slot, and those are rewritten for
// the slot, address and heap block asked for.
#include "kernel.h"

#define RG_SIZE   0x166670
#define RG_WORDS  (RG_SIZE / 4)
#define RG_LINES  (RG_WORDS / 4)
static const u32 BASE[2] = {0x08F61670, 0x090C7CF0};   // load addresses of _0 (slot 14) and _1 (slot 15)
#define HEAP_LO   0x09600000                           // where the racers' heap blocks are
#define HEAP_HI   0x0A000000
#define SLOT_OBJS 0x0922E360                           // the 0x40-byte per-slot objects, slot 0

static bool load(const char *ch, int n, u32 *w) {
	char psp[128], host[1024];
	snprintf(psp, sizeof psp, "disc0:/PSP_GAME/USRDIR/STREAMS/RACER_%s_%d.STR", ch, n);
	if (io_host_path(psp, host, sizeof host)) return false;
	FILE *f = fopen(host, "rb");
	if (!f) { LOGE("racergen: cannot open %s", host); return false; }
	size_t got = fread(w, 1, RG_SIZE + 1, f);
	fclose(f);
	if (got != RG_SIZE) { LOGE("racergen: %s is not a racer file", host); return false; }
	return true;
}

// the image with slot-dependent words blanked, for lining the two images up
static void mask(const u32 *f, u32 *g) {
	for (int i = 0; i < RG_WORDS; i++) {
		u32 v = f[i], a = v & 0x3fffffff, s = v >> 16 & 0xff;
		if (a >= 0x08000000 && a < 0x0a000000) v = 0xfffffff1;
		else if ((v >> 24) <= 2 && s >= 13 && s <= 16 && v) v = 0xfffffff2;
		else if (v == 14 || v == 15) v = 0xfffffff3;
		g[i] = v;
	}
}

// 64-byte windows of _1's masked image starting at each 16-byte line, sorted by hash
typedef struct { u64 h; s32 j; } Win;
static u64 win_hash(const u32 *m) {
	u64 h = 1469598103934665603ull;
	for (int k = 0; k < 16; k++) h = (h ^ m[k]) * 1099511628211ull;
	return h;
}
static int win_cmp(const void *a, const void *b) {
	const Win *x = a, *y = b;
	return x->h < y->h ? -1 : x->h > y->h ? 1 : x->j - y->j;
}

static u32 *M0, *M1;
static bool line_eq(int i, int j) { return j >= 0 && j < RG_LINES && !memcmp(M0 + i * 4, M1 + j * 4, 16); }

// for each 16-byte line of _0, the matching line of _1 (-1: none)
static void align(s32 *mp) {
	int nw = RG_LINES - 4;
	Win *w = malloc(nw * sizeof *w);
	for (int j = 0; j < nw; j++) w[j].h = win_hash(M1 + j * 4), w[j].j = j;
	qsort(w, nw, sizeof *w, win_cmp);
	for (int i = 0; i < RG_LINES; i++) mp[i] = -1;
	for (int i = 0; i < RG_LINES - 4; i++) {
		u64 h = win_hash(M0 + i * 4);
		int lo = 0, hi = nw;
		while (lo < hi) { int m = (lo + hi) / 2; if (w[m].h < h) lo = m + 1; else hi = m; }
		int n = 0, only = -1; bool next = false;
		for (int k = lo; k < nw && w[k].h == h; k++) {
			if (memcmp(M0 + i * 4, M1 + w[k].j * 4, 64)) continue;
			n++; only = w[k].j;
			if (i && mp[i - 1] >= 0 && w[k].j == mp[i - 1] + 1) next = true;
		}
		if (n == 1) mp[i] = only;
		else if (n > 1 && next) mp[i] = mp[i - 1] + 1;
	}
	free(w);
	for (int i = 1; i < RG_LINES; i++)   // lines whose 64-byte window crossed a change
		if (mp[i] < 0 && mp[i - 1] >= 0 && line_eq(i, mp[i - 1] + 1)) mp[i] = mp[i - 1] + 1;
	// keep only lines that really match; else try the neighbours' alignment
	for (int i = 0; i < RG_LINES; i++) {
		if (mp[i] < 0 || line_eq(i, mp[i])) continue;
		mp[i] = -1;
		if (i && mp[i - 1] >= 0 && line_eq(i, mp[i - 1] + 1)) mp[i] = mp[i - 1] + 1;
		else if (i + 1 < RG_LINES && mp[i + 1] >= 0 && line_eq(i, mp[i + 1] - 1)) mp[i] = mp[i + 1] - 1;
	}
}

static bool own(u32 v, u32 b) { v &= 0x3fffffff; return v >= b && v < b + RG_SIZE; }
static u32 rebase(u32 a, u32 base) { return (a & 0xc0000000) | ((a & 0x3fffffff) - BASE[0] + base); }

// Build RACER_<ch> for `slot` at `base`, its heap block moved to `heap`, into out (RG_SIZE bytes).
// Returns the heap block's size (0 on failure); *unresolved gets the words kept as in _0.
u32 racergen(const char *ch, int slot, u32 base, u32 heap, u8 *out, int *unresolved) {
	u32 *f0 = malloc(RG_SIZE + 1), *f1 = malloc(RG_SIZE + 1), *g = (u32 *)out;
	M0 = malloc(RG_SIZE); M1 = malloc(RG_SIZE);
	s32 *mp = malloc(RG_LINES * sizeof *mp);
	u32 *heap_ptrs = malloc(RG_SIZE), nheap = 0, size = 0, *moved = malloc(RG_SIZE), nmoved = 0;
	int bad = 0;
	if (!load(ch, 0, f0) || !load(ch, 1, f1)) goto done;
	mask(f0, M0); mask(f1, M1);
	align(mp);
	memcpy(g, f0, RG_SIZE);
	for (int i = 0; i < RG_LINES; i++) {
		int j = mp[i];
		for (int k = 0; k < 4; k++) {
			int o = i * 4 + k;
			u32 a = f0[o];
			// an array indexed by slot (an animation's clips per slot): _0's entry 14 is _1's entry 15. Such a
			// line differs between the two even masked, so it may have no match: its neighbours' then.
			int jj = j >= 0 ? j : i && mp[i - 1] >= 0 ? mp[i - 1] + 1 : i + 1 < RG_LINES && mp[i + 1] >= 0 ? mp[i + 1] - 1 : -1;
			int p1 = jj * 4 + k;
			if (jj >= 0 && p1 + 1 < RG_WORDS && o + 1 < RG_WORDS && own(a, BASE[0]) && !f1[p1] && !f0[o + 1] && own(f1[p1 + 1], BASE[1])
			    && abs((s32)((f1[p1 + 1] & 0x3fffffff) - BASE[1]) - (s32)((a & 0x3fffffff) - BASE[0])) < 0x1000) {   // the same thing
				moved[nmoved++] = o; continue;
			}
			if (j < 0) { if (own(a, BASE[0])) g[o] = rebase(a, base); continue; }
			u32 b = f1[j * 4 + k], x = a ^ b;
			if (a == b) continue;
			if (own(a, BASE[0]) && own(b, BASE[1]) && (a & 0xc0000000) == (b & 0xc0000000)) { g[o] = rebase(a, base); continue; }
			if ((x == 1 || x == 0x100 || x == 0x10000 || x == 0x1000000) && (a / x & 0xff) == 14) {
				int sh = __builtin_ctz(x);
				g[o] = (a & ~(0xffu << sh)) | (u32)slot << sh;
				continue;
			}
			if (a >= 0x08800000 && a < 0x0a000000 && b >= 0x08800000 && b < 0x0a000000) {
				s32 d = (s32)(b - a);
				if (d == 0x10 || d == 0x40 || d == 0x120) { g[o] = a + d * (slot - 14); continue; }
				if (a >= HEAP_LO && a < HEAP_HI && b >= HEAP_LO && b < HEAP_HI) { heap_ptrs[nheap++] = o; continue; }
			}
			// a word neither rule explains (a stray match, or data that really differs)
			if (own(a, BASE[0])) g[o] = rebase(a, base);
			else if ((a & 0xfdff0000) == 0x000e0000) g[o] = (a & ~0xff0000u) | (u32)slot << 16;   // slot handles
			else if (a >= SLOT_OBJS && a < SLOT_OBJS + 16 * 0x40) g[o] = a + 0x40 * (slot - 14);
			else { bad++; LOGI("racergen: kept %06x: %08x (in _1: %08x)", o * 4, a, b); }
		}
	}
	for (u32 n = 0; n < nmoved; n++) {   // after the loop: the entry's new place was handled already
		u32 o = moved[n];
		g[o] = 0; g[o + slot - 14] = rebase(f0[o], base);
	}
	u32 lo = ~0u, hi = 0;
	for (u32 n = 0; n < nheap; n++) { u32 v = f0[heap_ptrs[n]]; if (v < lo) lo = v; if (v > hi) hi = v; }
	lo &= ~0xfu;
	for (u32 n = 0; n < nheap; n++) g[heap_ptrs[n]] = f0[heap_ptrs[n]] - lo + heap;
	size = nheap ? hi - lo + 0x10 : 0x10;
	LOGI("racergen: %s for slot %d at %08x, heap %08x (+%x), %u slot entries moved, %d words kept", ch, slot, base, heap, size, nmoved, bad);
done:
	if (unresolved) *unresolved = bad;
	free(f0); free(f1); free(M0); free(M1); free(mp); free(heap_ptrs); free(moved);
	return size;
}
