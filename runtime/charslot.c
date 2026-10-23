// Character skins: extra icons on the character select screen that race as one of the eight characters
// (same moves, same stats, same code) but with their own name, icon, textures and racer files.
//
// A skin is a folder in a mod: <mod>/characters/<any name>/ holding
//     character.ini           name=Dark Sonic / base=sonic
//     icon.png                its icon, laid out like icon_template.png (written there on first run)
//     icon_selected.png       (optional) the highlighted icon, like icon_template_selected.png
//     textures/               (optional) replacement textures, named like the mods' textures/ folders,
//                             that apply only while the skin is racing
//     RACER_<BASE>_0.STR, _1.STR  (optional) the base character's racer files with the skin's own model,
//                             textures and voice; same size as the game's
//
// Select Character and Select Rival are each a grid widget of eight children (0x420-byte widgets, one per character), four per
// row. We give it a third row: one child per skin (up to four), each a copy of its base character's child,
// so picking it fires the base character, plus locked, hidden fillers so the row is complete (the grid's
// navigation is row/column arithmetic over the child count and skips locked children). Each skin child gets
// copies of the base's two icon textures, which the renderer replaces with the skin's icon by address.
// Picking a skin child turns the skin on for its base character; picking the base character turns it off,
// and so does starting a story.
#define _GNU_SOURCE
#include "kernel.h"
#include "config.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <png.h>

#define FOCUS_WIDGET  0x08AAC044
#define W_SIZE        0x420
#define W_SELFLAG     0x00C        // byte: the highlighted child
#define W_LOCKED      0x00D        // byte: navigation skips the child
#define W_TEX_OFF     0x280        // icon texture, not highlighted
#define W_TEX_ON      0x320        // icon texture, highlighted
#define W_CHAR        0x350        // the character's name hash, fired when picked
#define W_CHILDREN    0x39C
#define W_SELECTED    0x3A0
#define W_NCHILD      0x3A4
#define W_LIST_ROWS   0x3B4
#define W_COLS        0x3F0
#define TEX_SIZE      0x4480       // texture object, 128x128 8-bit pixels and palette included
#define MAX_SKINS     4

enum { B_CROSS = 0x4000 };
extern u32 fe_buttons;
u32 game_text_id(int i);
void game_text(int i, const char *s);
void ge_tex_set_overlay(const char *const *dirs, int ndirs);
void ge_tex_set_addr(u32 addr, const char *png);
int io_mod_count(void);
const char *io_mod_root(int i);
void io_set_racer_skin(int base, const char *dir);

static const u32 char_hash[8] = {0x39D88C71, 0x7576822D, 0xCA3B38F1, 0x2F5B185B, 0x54704F2B, 0x0EA8D5F6, 0xD3B9C425, 0x632B61EB};
static const char *const char_key[8] = {"sonic", "shadow", "knuckles", "silver", "metalsonic", "tails", "espio", "rouge"};

typedef struct {
	char name[40], dir[600], icon[2][640], tex[640];
	int base;
	bool has_tex, pack;
} Skin;
static Skin skins[MAX_SKINS];
static int nskins;
static int active[8] = {-1, -1, -1, -1, -1, -1, -1, -1};   // skin in use per character
static u32 prev_buttons;

static int base_index(const char *v) {
	char k[32]; int n = 0;
	for (; *v && n < 31; v++) if (*v != ' ' && *v != '_' && *v != '-') k[n++] = (char)tolower((unsigned char)*v);
	k[n] = 0;
	if (!strcmp(k, "metal")) return 4;
	for (int i = 0; i < 8; i++) if (!strcmp(k, char_key[i])) return i;
	return -1;
}

static bool is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }

static void load_skin(const char *dir) {
	char p[700];
	snprintf(p, sizeof p, "%s/character.ini", dir);
	FILE *f = fopen(p, "r");
	if (!f) return;
	Skin s = {.base = -1};
	char line[256];
	while (fgets(line, sizeof line, f)) {
		char *e = strchr(line, '=');
		if (!e) continue;
		*e++ = 0;
		e[strcspn(e, "\r\n")] = 0;
		while (*e == ' ') e++;
		char *k = line; while (*k == ' ') k++;
		k[strcspn(k, " ")] = 0;
		if (!strcasecmp(k, "name")) snprintf(s.name, sizeof s.name, "%s", e);
		else if (!strcasecmp(k, "base")) s.base = base_index(e);
	}
	fclose(f);
	if (s.base < 0) { LOGE("skin %s: character.ini needs base= (sonic, shadow, knuckles, silver, metal sonic, tails, espio or rouge)", dir); return; }
	if (nskins == MAX_SKINS) { LOGE("skin %s: only %d skins fit on the select screen", dir, MAX_SKINS); return; }
	if (!s.name[0]) { const char *b = strrchr(dir, '/'); snprintf(s.name, sizeof s.name, "%s", b ? b + 1 : dir); }
	snprintf(s.dir, sizeof s.dir, "%s", dir);
	snprintf(s.icon[0], sizeof s.icon[0], "%s/icon.png", dir);
	snprintf(s.icon[1], sizeof s.icon[1], "%s/icon_selected.png", dir);
	if (!is_file(s.icon[1])) snprintf(s.icon[1], sizeof s.icon[1], "%s", s.icon[0]);
	snprintf(s.tex, sizeof s.tex, "%s/textures", dir);
	struct stat st;
	s.has_tex = stat(s.tex, &st) == 0 && S_ISDIR(st.st_mode);
	// racer files must keep the game's size: the game addresses them by disc sector
	char up[16]; snprintf(up, sizeof up, "%s", char_key[s.base]);
	for (char *c = up; *c; c++) *c = (char)toupper((unsigned char)*c);
	int npack = 0;
	for (int k = 0; k < 2; k++) {
		snprintf(p, sizeof p, "%s/RACER_%s_%d.STR", dir, up, k);
		if (stat(p, &st)) continue;
		if (st.st_size == 0x166670) { npack++; continue; }
		LOGE("skin %s: %s is %lld bytes but the game's are %d, so the skin's racer files won't be used", s.name, p, (long long)st.st_size, 0x166670);
		npack = -9;
	}
	s.pack = npack > 0;
	skins[nskins++] = s;
	LOGE("skin %d: %s (plays as %s)%s%s", nskins, s.name, char_key[s.base], s.has_tex ? ", textures" : "", s.pack ? ", racer files" : "");
}

void charslot_init(void) {
	for (int m = 0; m < io_mod_count(); m++) {
		char d[700];
		snprintf(d, sizeof d, "%s/characters", io_mod_root(m));
		DIR *dir = opendir(d);
		if (!dir) continue;
		struct dirent *e;
		char *names[64]; int n = 0;
		while ((e = readdir(dir)) && n < 64) if (e->d_name[0] != '.' && e->d_name[0] != '_') names[n++] = strdup(e->d_name);
		closedir(dir);
		for (int i = 0; i < n; i++) {
			for (int j = i + 1; j < n; j++) if (strcasecmp(names[j], names[i]) < 0) { char *t = names[i]; names[i] = names[j]; names[j] = t; }
			char p[1400]; snprintf(p, sizeof p, "%s/%s", d, names[i]);
			load_skin(p);
			free(names[i]);
		}
	}
}

// ---------------------------------------------------------------- icons
// The icons are 128x128 8-bit textures, PSP-swizzled (16x8-pixel blocks), with a 256-colour palette.
static void write_icon_template(u32 tex, const char *path) {
	u32 px = LW(tex + 4), pal = LW(tex + 8);
	u8 *out = malloc(128 * 128 * 4);
	for (int i = 0; i < 128 * 128; i++) {
		int blk = i / 128, in = i % 128;
		int by = blk / 8, bx = blk % 8, r = in / 16, c = in % 16;
		u8 v = LB(px + i);
		memcpy(out + ((by * 8 + r) * 128 + bx * 16 + c) * 4, PTR(pal + v * 4), 4);
	}
	png_image img;
	memset(&img, 0, sizeof img);
	img.version = PNG_IMAGE_VERSION;
	img.width = img.height = 128;
	img.format = PNG_FORMAT_RGBA;
	if (png_image_write_to_file(&img, path, 0, out, 0, NULL)) LOGI("skins: wrote %s", path);
	free(out);
}

static u32 copy_tex(u32 src, u32 dst) {
	memcpy(PTR(dst), PTR(src), TEX_SIZE);
	for (u32 a = dst; a < dst + TEX_SIZE; a += 4) { u32 v = LW(a); if (v - src < TEX_SIZE) SW(a, v - src + dst); }
	SW(dst + 0x34, 0); SW(dst + 0x38, 0);   // not in the game's texture list
	return dst;
}

// ---------------------------------------------------------------- the grids
// Select Character and Select Rival each have their own grid widget.
typedef struct {
	u32 addr, hash;
	u32 items, texs;          // our 12 children, the skins' icon textures
	u32 mine[64];             // values grid_layout wrote (see remap)
	int nmine;
} Grid;
static Grid grids[2] = {{0x095D5A50, 0xD15098D3}, {0x09644190, 0x3F3CA7DE}};

static void relocate(u32 from, u32 size, u32 to, u32 lo, u32 hi) {
	for (u32 a = lo; a < hi; a += 4) { u32 v = LW(a); if (v - from < size) SW(a, v - from + to); }
}

static bool ours(Grid *g) { return g->items && LW(g->addr + 4) == g->hash && LW(g->addr + W_CHILDREN) == g->items && LW(g->addr + W_NCHILD) == 12; }

static void grid_patch(Grid *g) {
	if (!nskins || !game_text_id(0) || LW(g->addr + 4) != g->hash) return;
	u32 old = LW(g->addr + W_CHILDREN);
	if (LW(g->addr + W_NCHILD) != 8 || old == g->items || old - 0x09000000u > 0x01000000u || LW(g->addr + W_COLS) != 4) return;
	if (!g->items) { g->items = kalloc(12 * W_SIZE); g->texs = kalloc(MAX_SKINS * 2 * TEX_SIZE); }
	u32 items = g->items;
	memcpy(PTR(items), PTR(old), 8 * W_SIZE);
	g->nmine = 0;
	relocate(old, 8 * W_SIZE, items, 0x08800000, 0x0A000000);
	relocate(old, 8 * W_SIZE, items, items, items + 8 * W_SIZE);
	for (int i = 0; i < 4; i++) {
		u32 c = items + (8 + i) * W_SIZE, b = items + (i < nskins ? skins[i].base : 7) * W_SIZE;
		memcpy(PTR(c), PTR(b), W_SIZE);
		relocate(b, W_SIZE, c, c, c + W_SIZE);
		SB(c + W_SELFLAG, 0);
		if (i >= nskins) { SB(c + W_LOCKED, 1); continue; }
		Skin *s = &skins[i];
		for (int k = 0; k < 2; k++) {
			u32 src = LW(b + (k ? W_TEX_ON : W_TEX_OFF));
			if (src - 0x08800000u >= 0x01800000u) continue;
			u32 t = copy_tex(src, g->texs + (i * 2 + k) * TEX_SIZE);
			SW(c + (k ? W_TEX_ON : W_TEX_OFF), t);
			char tp[700];
			snprintf(tp, sizeof tp, "%s/icon_template%s.png", s->dir, k ? "_selected" : "");
			if (!is_file(tp)) write_icon_template(t, tp);
			ge_tex_set_addr(LW(t + 4), is_file(s->icon[k]) ? s->icon[k] : NULL);
		}
		game_text(i, s->name);
	}
	SW(g->addr + W_NCHILD, 12);
	if ((s32)LW(g->addr + W_LIST_ROWS) < 12) SW(g->addr + W_LIST_ROWS, 12);
	LOGI("skins: %d on the character grid at %08x", nskins, g->addr);
}

// Three rows don't fit where the game puts two, so every icon is drawn a little smaller and the rows
// closer together, a bit higher up. The menu data animates the original children (positions as they slide
// in, sizes as they're highlighted) and copies values between their fields, so a value is remapped unless
// it is one we wrote.
#define ROW_SCALE 0.825f                         // row spacing and icon size, relative to the game's
#define ROW_SHIFT 0.103f                         // y' = y * ROW_SCALE + ROW_SHIFT keeps the rows clear of the title
static void remap(Grid *g, u32 a, float scale, float shift) {
	u32 v = LW(a);
	for (int i = 0; i < g->nmine; i++) if (g->mine[i] == v) return;
	float f; memcpy(&f, &v, 4);
	f = f * scale + shift;
	memcpy(&v, &f, 4);
	SW(a, v);
	if (g->nmine < 64) g->mine[g->nmine++] = v;
	else { memmove(g->mine, g->mine + 1, 63 * 4); g->mine[63] = v; }
}

static void grid_layout(Grid *g) {
	static const u16 xo[] = {0x10, 0x20, 0x30, 0x250, 0x2F0}, yo[] = {0x14, 0x24, 0x34, 0x254, 0x2F4};
	static const u16 so[] = {0x40, 0x44};
	u32 items = g->items;
	for (int i = 0; i < 8; i++) {
		u32 c = items + i * W_SIZE;
		for (int k = 0; k < 5; k++) remap(g, c + yo[k], ROW_SCALE, ROW_SHIFT);
		for (int k = 0; k < 2; k++) remap(g, c + so[k], ROW_SCALE, 0);
	}
	for (int i = 0; i < 4; i++) {
		u32 a = items + i * W_SIZE, b = items + (4 + i) * W_SIZE, c = items + (8 + i) * W_SIZE;
		for (int k = 0; k < 5; k++) {
			float x, y0, y1, y;
			memcpy(&x, PTR(b + xo[k]), 4); memcpy(&y0, PTR(a + yo[k]), 4); memcpy(&y1, PTR(b + yo[k]), 4);
			y = y1 + (y1 - y0);
			if (i >= nskins) x += 10.0f;
			memcpy(PTR(c + xo[k]), &x, 4); memcpy(PTR(c + yo[k]), &y, 4);
		}
		for (int k = 0; k < 2; k++) SW(c + so[k], LW(b + so[k]));
		// a skin is locked exactly when its base character is (not unlocked yet, or taken by the other racer)
		if (i < nskins) SB(c + W_LOCKED, LB(items + skins[i].base * W_SIZE + W_LOCKED));
	}
}

// Character names: string ids by character index, read by the name under the grid (every frame) and
// wherever else the game names a racer. A skin's name stands in for its base character's while the skin is
// highlighted, and, away from the select screens, while it is picked.
#define NAME_IDS 0x08991628
static u32 name_ids[8];
static void set_names(int highlighted) {
	if (!name_ids[0]) for (int c = 0; c < 8; c++) name_ids[c] = LW(NAME_IDS + c * 4);
	for (int c = 0; c < 8; c++) {
		int k = highlighted >= -1 ? (highlighted >= 0 && skins[highlighted].base == c ? highlighted : -1) : active[c];
		SW(NAME_IDS + c * 4, k >= 0 ? game_text_id(k) : name_ids[c]);
	}
}

// The skins whose textures are in use: those picked, and the one highlighted on a select screen (so the big
// picture of the character there can be the skin's too).
static int highlighted = -1;
static u32 tex_set = ~0u;
static void apply_textures(void) {
	const char *dirs[MAX_SKINS]; int n = 0;
	u32 set = 0;
	for (int c = 0; c < 8; c++) if (active[c] >= 0) set |= 1u << active[c];
	if (highlighted >= 0) set |= 1u << highlighted;
	if (set == tex_set) return;
	tex_set = set;
	for (int i = 0; i < nskins; i++) if ((set >> i & 1) && skins[i].has_tex) dirs[n++] = skins[i].tex;
	ge_tex_set_overlay(dirs, n);
}

static void apply(void) {
	for (int c = 0; c < 8; c++) {
		Skin *s = active[c] >= 0 ? &skins[active[c]] : NULL;
		io_set_racer_skin(c, s && s->pack ? s->dir : NULL);
	}
	apply_textures();
}

static void pick(int c, int skin) {
	if (active[c] == skin) return;
	active[c] = skin;
	if (skin >= 0) LOGI("skins: %s races as %s", skins[skin].name, char_key[c]);
	else LOGI("skins: %s races as themselves", char_key[c]);
	apply();
}

// a story is starting: its racers are the game's own
void charslot_reset(void) {
	for (int c = 0; c < 8; c++) pick(c, -1);
}

void charslot_vblank(void) {
	u32 cur = fe_buttons, pressed = cur & ~prev_buttons;
	prev_buttons = cur;
	if (!nskins || !game_text_id(0)) return;
	int names = -2;
	for (int n = 0; n < 2; n++) {
		Grid *g = &grids[n];
		grid_patch(g);
		if (!ours(g)) continue;
		grid_layout(g);
		u32 sel = LW(g->addr + W_SELECTED), focus = LW(FOCUS_WIDGET);
		int idx = sel - g->items < 12 * W_SIZE ? (int)((sel - g->items) / W_SIZE) : -1;
		if (focus != g->addr && focus - g->items >= 12 * W_SIZE) continue;
		names = idx >= 8 && idx - 8 < nskins ? idx - 8 : -1;
		if ((pressed & B_CROSS) && idx >= 0 && !LB(sel + W_LOCKED)) {
			if (idx >= 8) pick(skins[idx - 8].base, idx - 8);
			else for (int c = 0; c < 8; c++) if (LW(sel + W_CHAR) == char_hash[c]) pick(c, -1);
		}
	}
	set_names(names);
	highlighted = names;
	apply_textures();
}
