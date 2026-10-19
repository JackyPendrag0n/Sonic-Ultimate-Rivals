// Sonic Rivals 2 game layer: settings persistence and the code hooks (data/hooks.json) that the
// recompiled game calls into: the race frame rate and the PC options in the game's Options screen.
#define _GNU_SOURCE
#include "kernel.h"
#include "config.h"
#include <sys/stat.h>
#include <stdarg.h>

GameConfig g_cfg = {
	.renderer = 1, .internal_scale = 2, .tex_filter = 0, .anisotropy = 4, .vsync = true, .fullscreen = false,
	.window_scale = 2, .keep_aspect = true, .fps = 60, .show_fps = false, .stick_deadzone = 15, .volume = 100,
};

// ---------------------------------------------------------------- config file
static char cfg_path[600];
const char *config_path(void) {
	if (!cfg_path[0]) {
		const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
		char dir[512];
		if (x) snprintf(dir, sizeof dir, "%s/sr2recomp", x);
		else snprintf(dir, sizeof dir, "%s/.config/sr2recomp", h ? h : ".");
		char tmp[512]; snprintf(tmp, sizeof tmp, "%s", dir);
		for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
		mkdir(tmp, 0755);
		snprintf(cfg_path, sizeof cfg_path, "%s/settings.ini", dir);
	}
	return cfg_path;
}

#define CFG_FIELDS(X) \
	X(renderer, i) X(internal_scale, i) X(tex_filter, i) X(anisotropy, i) X(vsync, b) X(fullscreen, b) \
	X(window_scale, i) X(keep_aspect, b) X(fps, i) X(show_fps, b) X(stick_deadzone, i) X(volume, i) X(split_layout, i)

static const int fps_steps[] = {30, 60, 120, 144, 165, 240};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void config_set(char *k, const char *v) {
	while (*k == ' ') k++;
	for (char *e = k + strlen(k); e > k && (e[-1] == ' '); ) *--e = 0;
#define X(name, t) if (!strcmp(k, #name)) g_cfg.name = atoi(v);
	CFG_FIELDS(X)
#undef X
}

void config_load(void) {
	FILE *f = fopen(config_path(), "r");
	bool existed = f != NULL;
	char line[256];
	while (f && fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (!eq || line[0] == '#' || line[0] == '[') continue;
		*eq = 0;
		config_set(line, eq + 1);
	}
	if (f) fclose(f);
	// environment overrides for testing
	const char *e;
	if ((e = getenv("SR2_FPS"))) g_cfg.fps = atoi(e);
	if ((e = getenv("SR2_GPU"))) g_cfg.renderer = !strcmp(e, "soft") ? 0 : 1;
	if ((e = getenv("SR2_GL_SCALE"))) g_cfg.internal_scale = atoi(e);
	if ((e = getenv("SR2_CFG"))) {   // "key=value,key=value"
		char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", e);
		for (char *sv, *t = strtok_r(tmp, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) {
			char *eq = strchr(t, '=');
			if (eq) { *eq = 0; config_set(t, eq + 1); }
		}
	}
	g_cfg.internal_scale = clampi(g_cfg.internal_scale, 1, 8);
	g_cfg.window_scale = clampi(g_cfg.window_scale, 1, 6);
	g_cfg.stick_deadzone = clampi(g_cfg.stick_deadzone, 0, 50);
	g_cfg.volume = clampi(g_cfg.volume, 0, 100);
	int best = 60;
	for (unsigned i = 0; i < sizeof fps_steps / sizeof fps_steps[0]; i++) if (g_cfg.fps >= fps_steps[i]) best = fps_steps[i];
	g_cfg.fps = best;
	if (!existed) config_save();
}

void config_save(void) {
	FILE *f = fopen(config_path(), "w");
	if (!f) return;
	fprintf(f, "# sr2recomp settings (also in the game's Options screen, and the F1 overlay)\n");
#define X(name, t) fprintf(f, "%s = %d\n", #name, (int)g_cfg.name);
	CFG_FIELDS(X)
#undef X
	fclose(f);
}

int config_fps_next(int fps, int dir) {
	int n = sizeof fps_steps / sizeof fps_steps[0], i = 0;
	while (i < n - 1 && fps_steps[i] < fps) i++;
	return fps_steps[(i + dir + n) % n];
}

// ---------------------------------------------------------------- frame rate
// The game's display manager (0x089A0BE4) flips at most every <+24> vblanks. Races ask for 2 (30 fps),
// the menus for 1. Gameplay is timed by the system clock, so the races run correctly at any rate:
// above 30 fps we ask for 1 instead, and above 60 we also speed up the emulated vblank during races.
enum { HOOK_SET_SWAP_INTERVAL = 1 };
#define DISP_MGR 0x089A0BE4
extern u32 g_vblank_us;
static bool in_race;

static void apply_frame_rate(void) {
	bool fast = in_race && g_cfg.fps > 60;
	g_vblank_us = fast ? 1000000 / g_cfg.fps : 16683;
	if (in_race) SW(DISP_MGR + 24, g_cfg.fps > 30 ? 1 : 2);
}

// ---------------------------------------------------------------- PC options in the game's Options screen
// The frontend is data-driven: screens are trees of 0x420-byte widgets baked into the menu data, which
// the game always loads at the same addresses. We give the Options list a fifth item, "Graphics", and
// when it is chosen the same list turns into the PC graphics settings (left/right or CROSS change a
// row, CIRCLE goes back), with the labels served from strings appended to the game's text table.
#define OPT_LIST       0x09594DD0   // Options list widget
#define OPT_LIST_HASH  0x69283AED   // its name hash (+4)
#define OPT_TITLE_STR  0x09597494   // string id of the Options title
#define FOCUS_WIDGET   0x08AAC044   // widget that has the input focus
#define W_SIZE         0x420
#define W_STRID        0x0C0
#define W_TARGET       0x120        // hash of the screen a list item opens
#define W_ELEM         0x390        // the widget's draw element (0x140 bytes)
#define W_CHILDREN     0x39C
#define W_SELECTED     0x3A0
#define W_NCHILD       0x3A4
#define W_LIST_ROWS    0x3B4        // rows a list shows before it scrolls
#define ELEM_SIZE      0x140
#define TEXT_TABLE     0x089ABCC0   // const char *strings[]
#define TEXT_COUNT     0x089ABCCC

enum { B_UP = 0x10, B_RIGHT = 0x20, B_DOWN = 0x40, B_LEFT = 0x80, B_CIRCLE = 0x2000, B_CROSS = 0x4000 };
enum { S_GRAPHICS, S_ROW0, NSTR = S_ROW0 + 5, STR_LEN = 48 };
extern u32 fe_buttons;
extern bool g_display_dirty;

static u32 str_buf, str_table, str_base;     // our strings, the enlarged text table, id of our first string
static u32 opt_items, opt_elem;              // the Options items (5) and the new item's draw element
static u32 opt_ids[5], opt_title_id;
static bool gfx_mode;
static u32 swallow, prev_buttons;
static int nav_presses, nav_phase;           // synthetic DOWN presses that move the highlight for us

static void set_str(int i, const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	vsnprintf((char *)PTR(str_buf + i * STR_LEN), STR_LEN, fmt, ap);
	va_end(ap);
}

// Append our strings to the game's text table (again whenever the game loads a new one).
static void text_patch(void) {
	u32 t = LW(TEXT_TABLE), n = LW(TEXT_COUNT);
	if (!t || t == str_table || n < 100 || n > 10000) return;
	if (!str_buf) str_buf = kalloc(NSTR * STR_LEN);
	str_table = kalloc((n + NSTR) * 4);
	memcpy(PTR(str_table), PTR(t), n * 4);
	for (int i = 0; i < NSTR; i++) SW(str_table + (n + i) * 4, str_buf + i * STR_LEN);
	str_base = n;
	set_str(S_GRAPHICS, "Graphics");
	SW(TEXT_TABLE, str_table);
	SW(TEXT_COUNT, n + NSTR);
	LOGI("text: %u strings, PC strings from id %u", n, n);
}

// Point every reference into [from, from+size) at the same offset in [to, ...).
static void relocate_refs(u32 from, u32 size, u32 to, u32 lo, u32 hi) {
	for (u32 a = lo; a < hi; a += 4) {
		u32 v = LW(a);
		if (v - from < size) SW(a, v - from + to);
	}
}

static void options_patch(void) {
	if (!str_table || LW(OPT_LIST + 4) != OPT_LIST_HASH) return;
	u32 old = LW(OPT_LIST + W_CHILDREN);
	if (LW(OPT_LIST + W_NCHILD) != 4 || old == opt_items || old - 0x09000000u > 0x01000000u) return;
	if (!opt_items) { opt_items = kalloc(5 * W_SIZE); opt_elem = kalloc(ELEM_SIZE); }
	memcpy(PTR(opt_items), PTR(old), 4 * W_SIZE);
	relocate_refs(old, 4 * W_SIZE, opt_items, 0x08800000, 0x0A000000);
	relocate_refs(old, 4 * W_SIZE, opt_items, opt_items, opt_items + 4 * W_SIZE);
	// the new item: a copy of the last one, one row further down
	u32 last = opt_items + 3 * W_SIZE, item = opt_items + 4 * W_SIZE;
	memcpy(PTR(item), PTR(last), W_SIZE);
	relocate_refs(last, W_SIZE, item, item, item + W_SIZE);
	SW(item + W_STRID, str_base + S_GRAPHICS);
	SW(item + W_TARGET, 0);
	u32 src = LW(last + W_ELEM);
	memcpy(PTR(opt_elem), PTR(src), ELEM_SIZE);
	relocate_refs(src, ELEM_SIZE, opt_elem, opt_elem, opt_elem + ELEM_SIZE);
	relocate_refs(last, W_SIZE, item, opt_elem, opt_elem + ELEM_SIZE);
	SW(item + W_ELEM, opt_elem);
	SW(OPT_LIST + W_NCHILD, 5);
	if ((s32)LW(OPT_LIST + W_LIST_ROWS) < 5) SW(OPT_LIST + W_LIST_ROWS, 5);
	for (int i = 0; i < 5; i++) opt_ids[i] = LW(opt_items + i * W_SIZE + W_STRID);
	gfx_mode = false;
	LOGI("options: added the Graphics item");
}

// The menu data animates each item's position (bound to the four original items), so the new row
// follows the last one, one row spacing further down.
static void options_layout(void) {
	static const u16 yoffs[] = {0x14, 0x24, 0x34, 0x254, 0x2F4};
	u32 a = opt_items + 2 * W_SIZE, b = a + W_SIZE, c = b + W_SIZE;
	for (unsigned i = 0; i < sizeof yoffs / sizeof yoffs[0]; i++) {
		float y2, y3, y;
		memcpy(&y2, PTR(a + yoffs[i]), 4); memcpy(&y3, PTR(b + yoffs[i]), 4);
		y = y3 + (y3 - y2);
		memcpy(PTR(c + yoffs[i]), &y, 4);
	}
	static const u16 xoffs[] = {0x20, 0x30, 0x250, 0x2F0};   // resting x (the selected row's +0x10 slides out)
	for (unsigned i = 0; i < sizeof xoffs / sizeof xoffs[0]; i++) SW(c + xoffs[i], LW(b + xoffs[i]));
}

static const char *on_off(bool b) { return b ? "On" : "Off"; }

static void gfx_labels(void) {
	static const char *filters[] = {"Auto", "Sharp", "Smooth"};
	if (g_cfg.renderer) set_str(S_ROW0, "Resolution: %dx", g_cfg.internal_scale);
	else set_str(S_ROW0, "Resolution: PSP");
	if (g_cfg.fps == 30) set_str(S_ROW0 + 1, "Frame Rate: 30 (PSP)");
	else set_str(S_ROW0 + 1, "Frame Rate: %d", g_cfg.fps);
	set_str(S_ROW0 + 2, "Fullscreen: %s", on_off(g_cfg.fullscreen));
	set_str(S_ROW0 + 3, "V-Sync: %s", on_off(g_cfg.vsync));
	set_str(S_ROW0 + 4, "Filtering: %s", filters[g_cfg.tex_filter % 3]);
}

static void gfx_change(int row, int dir) {
	switch (row) {
	case 0: g_cfg.internal_scale = (g_cfg.internal_scale - 1 + dir + 8) % 8 + 1; break;
	case 1: g_cfg.fps = config_fps_next(g_cfg.fps, dir); break;
	case 2: g_cfg.fullscreen = !g_cfg.fullscreen; break;
	case 3: g_cfg.vsync = !g_cfg.vsync; break;
	case 4: g_cfg.tex_filter = (g_cfg.tex_filter + dir + 3) % 3; break;
	}
	g_display_dirty = true;
	gfx_labels();
}

static void gfx_enter(void) {
	gfx_mode = true;
	nav_presses = 1; nav_phase = 0;              // from Graphics (last row) down to Resolution (first)
	gfx_labels();
	for (int i = 0; i < 5; i++) SW(opt_items + i * W_SIZE + W_STRID, str_base + S_ROW0 + i);
	opt_title_id = LW(OPT_TITLE_STR);
	SW(OPT_TITLE_STR, str_base + S_GRAPHICS);
}

static void gfx_leave(int row) {
	gfx_mode = false;
	nav_presses = row >= 0 ? 4 - row : 0; nav_phase = 0;   // back onto Graphics
	for (int i = 0; i < 5; i++) SW(opt_items + i * W_SIZE + W_STRID, opt_ids[i]);
	SW(OPT_TITLE_STR, opt_title_id);
	config_save();
}

static void options_input(void) {
	u32 cur = fe_buttons, pressed = cur & ~prev_buttons;
	prev_buttons = cur;
	swallow &= cur;                                 // a swallowed button stays hidden until released
	u32 focus = LW(FOCUS_WIDGET);
	bool on_options = opt_items && LW(OPT_LIST + W_CHILDREN) == opt_items && LW(OPT_LIST + W_NCHILD) == 5 &&
	                  (focus == OPT_LIST || focus - opt_items < 5 * W_SIZE);
	if (opt_items && LW(OPT_LIST + W_CHILDREN) == opt_items) options_layout();
	if (!on_options) { if (gfx_mode) gfx_leave(-1); nav_presses = 0; fe_buttons &= ~swallow; return; }
	u32 sel = LW(OPT_LIST + W_SELECTED);
	int row = sel - opt_items < 5 * W_SIZE ? (int)((sel - opt_items) / W_SIZE) : -1;
	if (!gfx_mode) {
		if (row == 4 && (pressed & B_CROSS)) { gfx_enter(); swallow |= B_CROSS; }
	} else {
		if (pressed & B_CIRCLE) { gfx_leave(row); swallow |= B_CIRCLE; }
		else if (row >= 0 && (pressed & (B_LEFT | B_RIGHT | B_CROSS))) gfx_change(row, (pressed & B_LEFT) ? -1 : 1);
		swallow |= cur & (B_LEFT | B_RIGHT | B_CROSS);
	}
	fe_buttons &= ~swallow;
	if (nav_presses > 0) {
		fe_buttons &= ~(B_UP | B_DOWN);
		if (nav_phase < 2) fe_buttons |= B_DOWN;
		if (++nav_phase == 4) { nav_phase = 0; nav_presses--; }
	}
}

void game_vblank(void) {
	static int last_fps;
	if (g_cfg.fps != last_fps) { last_fps = g_cfg.fps; apply_frame_rate(); }
	text_patch();
	options_patch();
	options_input();
}

u32 rt_hookval(Ctx *c, u32 id, u32 v) { return v; }

void rt_hookcall(Ctx *c, u32 id, void (*orig)(Ctx *c)) {
	switch (id) {
	case HOOK_SET_SWAP_INTERVAL:   // F_073c4(disp, interval)
		if ((s32)A(1) > 0) {
			in_race = A(1) >= 2;
			if (in_race && g_cfg.fps > 30) c->r[5] = 1;
			orig(c);
			apply_frame_rate();
			return;
		}
		break;
	}
	orig(c);
}
