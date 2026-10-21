// Sonic Rivals 2 game layer: settings persistence and the code hooks (data/hooks.json) that the
// recompiled game calls into: the race frame rate and the PC options in the game's Options screen.
#define _GNU_SOURCE
#include "kernel.h"
#include "config.h"
#include <sys/stat.h>
#include <stdarg.h>
#include <math.h>

GameConfig g_cfg = {
	.renderer = 1, .internal_scale = 2, .tex_filter = 0, .anisotropy = 4, .vsync = true, .fullscreen = false,
	.window_scale = 2, .keep_aspect = true, .fps = 60, .show_fps = false, .stick_deadzone = 15, .volume = 100,
	.mp_players = 1, .mp_char2 = 1, .mp_char3 = 3, .mp_char4 = 4,
};
const char *const mp_char_names[MP_CHARS] = {"Sonic", "Shadow", "Silver", "Knuckles", "Rouge", "Tails", "Espio", "Metal Sonic"};
const char *const mp_char_files[MP_CHARS] = {"SONIC", "SHADOW", "SILVER", "KNUCKLES", "ROUGE", "TAILS", "ESPIO", "METALSONIC"};

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
	X(window_scale, i) X(keep_aspect, b) X(fps, i) X(show_fps, b) X(stick_deadzone, i) X(volume, i) X(split_layout, i) \
	X(mp_players, i) X(mp_char2, i) X(mp_char3, i) X(mp_char4, i) X(pad_all, b)

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
	g_cfg.mp_players = clampi(g_cfg.mp_players, 1, 4);
	g_cfg.mp_char2 = clampi(g_cfg.mp_char2, 0, MP_CHARS - 1);
	g_cfg.mp_char3 = clampi(g_cfg.mp_char3, 0, MP_CHARS - 1);
	g_cfg.mp_char4 = clampi(g_cfg.mp_char4, 0, MP_CHARS - 1);
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
enum { HOOK_SET_SWAP_INTERVAL = 1, HOOK_COPTER_LIFT_DT, HOOK_SIG_START_DT, HOOK_RACER_MOVE, HOOK_BUILD_RACER_SLOT, HOOK_TRACK_ENTITY_SECTION, HOOK_RACER_INPUT,
       HOOK_CAMERA_UPDATE, HOOK_WORLD_DRAW, HOOK_SHADOW_DRAW, HOOK_EFFECT_DRAW,
       HOOK_RACER_STEP, HOOK_RACE_STATE, HOOK_SEND_EVENT };
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
enum { S_GRAPHICS, S_ROW0, S_SKIN = S_ROW0 + 5, NSTR = S_SKIN + 4, STR_LEN = 48 };
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

// strings for charslot.c (skin names): their ids once the text table is patched, 0 before
u32 game_text_id(int i) { return str_table ? str_base + S_SKIN + i : 0; }
void game_text(int i, const char *s) { if (str_buf) set_str(S_SKIN + i, "%s", s); }

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

void charslot_vblank(void);

void game_vblank(void) {
	static int last_fps;
	if (g_cfg.fps != last_fps) { last_fps = g_cfg.fps; apply_frame_rate(); }
	text_patch();
	options_patch();
	options_input();
	charslot_vblank();
}

// ---------------------------------------------------------------- frame-rate fixes
// Races ran at 30 fps on the PSP and most of the game scales by the frame time, but a few timers are set to
// "dt * N", meaning N frames. Above 30 fps those get shorter, so they are fed at least 1/30 s.
static u32 at_least_30fps_dt(u32 v) {
	float dt; memcpy(&dt, &v, 4);
	if (!(dt >= 1.0f / 30)) dt = 1.0f / 30;
	memcpy(&v, &dt, 4);
	return v;
}

// A racer's move step F(racer, dt) does pos += vel * dt and then collides and slides. At very high frame rates
// a racer sliding into a concave corner (the start of a loop, a ramp running into a wall) moves a shorter and
// shorter distance each frame and never crosses onto the next surface, which at 30-165 fps it does by
// overshooting a little; it then sits there until the speed runs out. When a racer has covered under a quarter
// of the distance its speed asks for, three frames running, one step is taken with a 60 fps frame time.
#define RACER_BODY   0x004     // -> body; position at +0x20
#define RACER_VEL    0x130
static struct { u32 racer; float pos[3], expect; int stuck; } movers[8];

static void racer_move(Ctx *c, void (*orig)(Ctx *c)) {
	u32 racer = A(0), body = LW(racer + RACER_BODY);
	float dt = c->f[12], pos[3], vel[3];
	memcpy(pos, PTR(body + 0x20), sizeof pos);
	memcpy(vel, PTR(racer + RACER_VEL), sizeof vel);
	int i = 0;
	while (i < 8 && movers[i].racer && movers[i].racer != racer) i++;
	if (i == 8) i = racer >> 4 & 7;
	if (movers[i].racer != racer) memset(&movers[i], 0, sizeof movers[i]), movers[i].racer = racer;
	float dx = pos[0] - movers[i].pos[0], dy = pos[1] - movers[i].pos[1], dz = pos[2] - movers[i].pos[2];
	float moved = sqrtf(dx * dx + dy * dy + dz * dz);
	if (movers[i].expect > 0.004f && moved < 0.25f * movers[i].expect) movers[i].stuck++;
	else movers[i].stuck = 0;
	memcpy(movers[i].pos, pos, sizeof pos);
	movers[i].expect = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]) * dt;
	if (movers[i].stuck >= 3 && dt < 1.0f / 60) {
		movers[i].stuck = 0;
		c->f[12] = 1.0f / 60;
	}
	orig(c);
}

// ---------------------------------------------------------------- extra racer (experimental)
// A racer file is a 1.4 MB memory image made for one load slot at one address (RACER_<C>_0: slot 14 at
// 0x08F61670, _1: slot 15 at 0x090C7CF0). tools/racergen.py rebuilds one for slot 13 (a level slot that
// races leave empty) at 0x0A000000. SR2_EXTRA_RACER=<that file> loads it during the race load, copies its
// slot header into world slot 13 like the game does for slots 14/15, and builds its scene nodes. On the
// first frame of the race it is switched on, put next to the local player (SR2_EXTRA_DXYZ=x,y,z offsets
// it), takes the empty rival seat (Free Play), and is set up as a human racer reading player 2's pad.
// Up to three files, separated by ':' (each made for its own slot and address, e.g. slots 13, 12, 11 at
// 0x0A000000, 0x0A400000, 0x0A800000), give players 2-4; the ones without a seat are stepped right after
// the seated racers (HOOK_RACER_STEP). Without SR2_EXTRA_RACER the settings (mp_players, mp_char2-4) pick the
// characters, and their files are built during the load (racergen.c) for the level slots left empty.
#define WORLD        0x089ABD10
#define WORLD_SLOT(k) (WORLD + 0x90 + (k) * 0x4c0)
#define FILE_SLOT_HDR 0x5430   // the slot header saved inside a racer file
#define MAX_EXTRA 3
static int n_extra;                                         // extra racers built for this race
static u32 extra_base, extra_racer[MAX_EXTRA], extra_pending[MAX_EXTRA];   // and whether each waits to be switched on
u32 racergen(const char *ch, int slot, u32 base, u32 heap, u8 *out, int *unresolved);
static bool slot_free(int k) { return !LW(WORLD_SLOT(k) + 0xd0) && !LW(WORLD_SLOT(k) + 0x438); }
static bool extra_racer_prepare(int k) {
	static u8 buf[0x200000];
	size_t n;
	const char *list = getenv("SR2_EXTRA_RACER");
	if (list) {
		char path[1024];
		for (int i = 0; ; i++) {   // the k-th of the ':'-separated files
			const char *e = strchr(list, ':');
			size_t len = e ? (size_t)(e - list) : strlen(list);
			if (i == k) { if (!len || len >= sizeof path) return false; memcpy(path, list, len); path[len] = 0; break; }
			if (!e) return false;
			list = e + 1;
		}
		FILE *f = fopen(path, "rb");
		if (!f) { LOGI("extra racer: cannot open %s", path); return false; }
		n = fread(buf, 1, sizeof buf, f); fclose(f);
		LOGI("extra racer: %s (%zu bytes)", path, n);
	} else {
		// Players 2-4 from the settings: build the character's racer file for the highest level slot
		// this race leaves empty (below the extras already built), at 0x0A000000 + k * 4 MB, its heap block 2 MB in.
		if (k + 1 >= g_cfg.mp_players) return false;
		static int used_slot[MAX_EXTRA];
		int slot = k ? used_slot[k - 1] - 1 : 13;
		while (slot > 0 && !slot_free(slot)) slot--;
		if (slot <= 0) { LOGE("extra racer %d: no free load slot in this level", k + 1); return false; }
		used_slot[k] = slot;
		int ci = k == 0 ? g_cfg.mp_char2 : k == 1 ? g_cfg.mp_char3 : g_cfg.mp_char4;
		u32 base = 0x0A000000 + k * 0x400000, heap = base + 0x200000;
		u32 hs = racergen(mp_char_files[ci], slot, base, heap, buf, NULL);
		if (!hs) return false;
		memset(PTR(heap), 0, 0x200000);
		n = 0x166670;
		LOGI("extra racer %d: %s, slot %d", k + 1, mp_char_names[ci], slot);
	}
	u32 start; memcpy(&start, buf + FILE_SLOT_HDR + 0x410, 4);   // the file's own address (+0x10)
	extra_base = start - 0x10;
	memcpy(PTR(extra_base), buf, n);
	return true;
}
static void extra_racer_build(Ctx *c, u32 loader) {
	u32 hdr = extra_base + FILE_SLOT_HDR, slot = WORLD_SLOT(LW(hdr + 0x20));   // the file's own slot number
	static const u16 fields[] = { 0xd0, 0x410, 0x414, 0x438, 0x47c, 0x4b0 };   // root node, file range, node count, ..
	for (unsigned i = 0; i < sizeof fields / sizeof *fields; i++) SW(slot + fields[i], LW(hdr + fields[i]));
	Ctx save = *c;
	c->r[4] = loader; c->r[5] = slot;
	rt_call(c, 0x08800000 + 0x16208c);   // build the slot's scene nodes (the racer among them)
	*c = save;
	// What F_159954 then does for its own racers i = 0, 1: the racer becomes entity i of world slot 0, and
	// its 0x70-byte body is copied to entry i of that slot's body table. Add ours after them.
	u32 racer = LW(slot + 0xd0), s0 = WORLD_SLOT(0), i = LW(s0 + 0x438);
	if (i < 64) {
		u32 body = LW(s0 + 0x410) + i * 0x70;
		memcpy(PTR(body), PTR(LW(racer + 4)), 0x70);
		SW(racer + 4, body);
		SW(s0 + 0xd0 + 4 * i, racer);
		SW(s0 + 0x438, i + 1);
	}
	LOGI("extra racer %d: built, racer %08x is entity %u of slot 0", n_extra, racer, i);
	extra_racer[n_extra] = extra_pending[n_extra] = racer;
	n_extra++;
}

// Put the extra racer at the start next to racer pr, the way F_159194 puts racers 0/1 at their start
// (racer+0xb0): move the body (+0x20) and the previous position (+0xe0), telling the racer's collision
// object (F_15e3c/F_15e64 take the offset), face the same way (+0xc0, body +0x30), then F_a2430.
static void extra_racer_place(Ctx *c, u32 r, u32 pr, int k) {
	float t[4], cur[4], d[4];
	float d0[3] = { 0, 1.2f * (k + 1), 0 };   // just ahead, in a row (y is along the track here; racers block each other)
	const char *e = getenv("SR2_EXTRA_DXYZ");
	if (e && sscanf(e, "%f,%f,%f", &d0[0], &d0[1], &d0[2]) == 3) for (int j = 0; j < 3; j++) d0[j] *= k + 1;
	memcpy(t, PTR(pr + 0xb0), 16);
	for (int j = 0; j < 3; j++) t[j] += d0[j];
	memcpy(PTR(r + 0xb0), t, 16);
	memcpy(PTR(r + 0xc0), PTR(pr + 0xc0), 16);
	u32 eb = LW(r + 4), tmp = c->r[29] - 0x200;   // scratch below the guest stack
	Ctx save = *c;
	static const u16 at[2] = { 0x20, 0xe0 };   // body +0x20 (base eb), racer +0xe0 (base r)
	static const u32 fn[2] = { 0x15e3c, 0x15e64 };
	for (int k = 0; k < 2; k++) {
		u32 a = (k ? r : eb) + at[k];
		memcpy(cur, PTR(a), 16);
		for (int j = 0; j < 4; j++) d[j] = t[j] - cur[j];
		memcpy(PTR(tmp), d, 16);
		c->r[29] = tmp - 0x40; c->r[4] = r; c->r[5] = tmp;
		rt_call(c, 0x08800000 + fn[k]);
		*c = save;
		memcpy(PTR(a), t, 16);
	}
	memcpy(PTR(eb + 0x30), PTR(LW(pr + 4) + 0x30), 16);
	SW(eb + 8, LW(eb + 8) | 0xc000);
	c->r[4] = r; rt_call(c, 0x08800000 + 0xa2430);
	*c = save;
}

// Players 2-4 steer the extra racers. Every racer has an input block (source at +0x1c) that F_17d58 fills
// each frame; sources 0..3 all read the one pad device the main loop updates (F_06774, 0x089A4020). Extra
// racer k gets source k+1, and for it F_17d58 runs against a device of its own kept here, updated from
// controller k+2 (g_ctrl_port makes the sceCtrl calls return that pad).
#define PAD_DEV  0x089A4020
#define PAD_DEVN(k) (0x0A1F0000 + (k) * 0x100)   // above the PSP's 32 MB, clear of the first extra racer file
#define PAD_DEV_SIZE 0x60
extern int g_ctrl_port;
// Split view: one screen, one view per human racer. The race camera (CAM_MAIN) follows the racer at +0x294;
// a second camera is the same object with its own state, kept here and swapped in while it steps
// (HOOK_CAMERA_UPDATE) and while the world is drawn for it (HOOK_WORLD_DRAW). Each view is drawn into its
// part of the screen by moving the GE's viewport centre and clipping with the scissor, so it keeps the
// game's aspect and sees the middle of what a full-screen view would. SR2_SPLIT_VIEW=lr (side by side),
// tb (top and bottom, the default) or 0 (off).
#define CAM_MAIN     0x08A855D4   // the race camera
#define CAM_SIZE     0x66c        // its allocation
#define CAM_NODE_SZ  0x7c         // the transform node it owns (camera +4)
#define CAM_TARGET   0x294
#define GE_LIST_PTR  0x089A0E1C   // the display list being written
static int split_view = -1;       // -1 not read yet, 0 off, 1 top and bottom, 2 side by side
static bool cam_ready[MAX_EXTRA];
static u32 cam_of[MAX_EXTRA];
static u8 cam_obj[MAX_EXTRA][CAM_SIZE], cam_node[MAX_EXTRA][CAM_NODE_SZ];   // the cameras of extra racers 0..2

static bool split_view_on(void) {
	if (split_view < 0) {
		const char *e = getenv("SR2_SPLIT_VIEW");
		split_view = !e ? 1 : !strcmp(e, "0") ? 0 : !strcmp(e, "lr") ? 2 : 1;
	}
	return split_view && n_extra && !extra_pending[n_extra - 1] && LW(CAM_MAIN);
}

static void swap_bytes(u8 *a, u8 *b, u32 n) { for (u32 i = 0; i < n; i++) { u8 t = a[i]; a[i] = b[i]; b[i] = t; } }

static void cam_swap(int k) {   // swap extra racer k's camera state in or out
	u32 cam = LW(CAM_MAIN);
	if (!cam_ready[k] || cam_of[k] != cam) {   // it starts as a copy of the first camera, following its racer
		memcpy(cam_obj[k], PTR(cam), CAM_SIZE);
		memcpy(cam_node[k], PTR(LW(cam + 4)), CAM_NODE_SZ);
		memcpy(cam_obj[k] + CAM_TARGET, &extra_racer[k], 4);
		cam_of[k] = cam; cam_ready[k] = true;
	}
	swap_bytes(PTR(cam), cam_obj[k], CAM_SIZE);
	swap_bytes(PTR(LW(cam + 4)), cam_node[k], CAM_NODE_SZ);
}

// rebuild the view from the current camera state, as F_581b0 does at the start of the race render
static void view_refresh(Ctx *c) {
	Ctx save = *c;
	u32 cam = LW(CAM_MAIN), buf = c->r[29] - 0x100;
	c->r[29] = buf - 0x40;
	c->r[4] = cam; c->r[5] = buf; rt_call(c, LW(LW(cam) + 0x40));
	c->r[29] = buf - 0x40;
	c->r[4] = 0x08800000 + 0x1a3f20; c->r[5] = buf; rt_call(c, 0x08800000 + 0x2ac50);
	*c = save;
}

static void ge_put(u32 w) { u32 p = LW(GE_LIST_PTR); SW(p, w); SW(GE_LIST_PTR, p + 4); }

// view k, or the whole screen (k < 0): viewport centre (XPOS/YPOS, float24) and scissor. Two views split the
// screen in halves, three or four in quarters. The first view is not clipped: it fills the screen (centred
// on its own part), and the other views are drawn over the rest. Drawing the first view into only part of
// the screen leaves the colours washed out, as if something later in the frame weighs in the parts nothing
// was drawn to.
static void ge_region(int k) {
	int x0 = 0, y0 = 0, w = 480, h = 272;
	if (k >= 0 && n_extra >= 2) { w = 240; h = 136; x0 = (k & 1) * 240; y0 = (k >> 1) * 136; }
	else if (k >= 0) { if (split_view == 2) { w = 240; x0 = k * 240; } else { h = 136; y0 = k * 136; } }
	float px = 2048.0f - 240 + x0 + w / 2, py = 2048.0f - 136 + y0 + h / 2;   // the screen offset stays (2048-240, 2048-136)
	u32 bx, by; memcpy(&bx, &px, 4); memcpy(&by, &py, 4);
	ge_put(0x45000000 | bx >> 8);
	ge_put(0x46000000 | by >> 8);
	if (k <= 0) { x0 = 0; y0 = 0; w = 480; h = 272; }
	ge_put(0xD4000000 | x0 | y0 << 10);
	ge_put(0xD5000000 | (x0 + w - 1) | (y0 + h - 1) << 10);
}

// one draw call of the race render, once per view: the first camera's view, then each extra racer's (with
// that racer as the local player, whose section picks the scenery), then the first camera's state back
static void split_draw_views(Ctx *c, void (*orig)(Ctx *c)) {
	Ctx save = *c;
	ge_region(0); orig(c);
	Ctx out = *c;
	*c = save;
	u32 p1 = LW(WORLD + 0x5024);
	for (int k = 0; k < n_extra; k++) {
		cam_swap(k); SW(WORLD + 0x5024, extra_racer[k]); view_refresh(c);
		ge_region(k + 1); orig(c);
		*c = save;
		SW(WORLD + 0x5024, p1); cam_swap(k);
	}
	view_refresh(c);
	ge_region(-1);
	*c = out;
}

static int race_state_depth;   // inside F_592cc
static int extra_index(u32 r) { for (int k = 0; k < n_extra; k++) if (extra_racer[k] == r) return k; return -1; }
static int input_extra(u32 in) {   // which extra racer an input block belongs to
	for (int k = 0; k < n_extra; k++) { u32 r = extra_racer[k]; if (LW(r + 0x8c) && LW(LW(r + 0x8c) + 4) == in) return k; }
	return -1;
}

u32 rt_hookval(Ctx *c, u32 id, u32 v) {
	switch (id) {
	case HOOK_COPTER_LIFT_DT:                                // Tails' Tail Copter: lift time per flap
	case HOOK_SIG_START_DT: return at_least_30fps_dt(v);     // signature move start: lift time and kick
	}
	return v;
}

void rt_hookcall(Ctx *c, u32 id, void (*orig)(Ctx *c)) {
	switch (id) {
	case HOOK_BUILD_RACER_SLOT: {   // F_16208c(loader, slot)
		u32 loader = A(0), slot = A(1);
		orig(c);
		if (slot == WORLD_SLOT(15)) {
			n_extra = 0;
			for (int k = 0; k < MAX_EXTRA && extra_racer_prepare(k); k++) extra_racer_build(c, loader);
		}
		return;
	}
	case HOOK_RACER_INPUT:   // F_17d58(input)
		if ((s32)LW(A(0) + 0x1c) == 1 && input_extra(A(0)) >= 0) {
			u32 in = A(0); int k = input_extra(in);
			Ctx save = *c;
			g_ctrl_port = k + 1;
			c->r[4] = PAD_DEVN(k); rt_call(c, 0x08800000 + 0x6774);   // update that player's device
			g_ctrl_port = 0;
			*c = save;
			static u8 p1[PAD_DEV_SIZE];
			memcpy(p1, PTR(PAD_DEV), PAD_DEV_SIZE);
			memcpy(PTR(PAD_DEV), PTR(PAD_DEVN(k)), PAD_DEV_SIZE);
			SW(in + 0x1c, 0);
			orig(c);
			SW(in + 0x1c, 1);
			memcpy(PTR(PAD_DEV), p1, PAD_DEV_SIZE);
			return;
		}
		break;
	case HOOK_CAMERA_UPDATE:   // F_10a250(camera, dt)
		if (split_view_on() && A(0) == LW(CAM_MAIN)) {
			Ctx save = *c;
			orig(c);
			*c = save;
			for (int k = 0; k < n_extra; k++) { cam_swap(k); orig(c); cam_swap(k); *c = save; }
			return;
		}
		break;
	case HOOK_WORLD_DRAW:     // F_15a460(world)
	case HOOK_SHADOW_DRAW:    // F_31300
	case HOOK_EFFECT_DRAW:    // F_30414
		if (split_view_on()) { split_draw_views(c, orig); return; }
		break;
	case HOOK_RACE_STATE: {   // F_592cc(race, state)
		if (!n_extra) break;
		static int depth;
		depth++; race_state_depth = depth;
		orig(c);
		depth--; race_state_depth = depth;
		u32 p1 = LW(WORLD + 0x5024);
		for (int k = 0; k < n_extra; k++)
			if (!extra_pending[k] && p1 && extra_racer[k] != LW(WORLD + 0x502c) && extra_racer[k] != LW(WORLD + 0x5030))
				SW(extra_racer[k] + 0x548, LW(p1 + 0x548));
		return;
	}
	case HOOK_SEND_EVENT:   // F_16c388(manager, target, source, hash, ...)
		if (race_state_depth && A(1) && A(1) == LW(WORLD + 0x5024)) {   // the race tells the local player: tell the
			Ctx save = *c;                                                // extra racers without a seat too
			orig(c);
			Ctx out = *c;
			for (int k = 0; k < n_extra; k++) {
				u32 e = extra_racer[k];
				if (extra_pending[k] || e == LW(WORLD + 0x502c) || e == LW(WORLD + 0x5030)) continue;
				*c = save; c->r[5] = e; if (A(2) == LW(WORLD + 0x5024)) c->r[6] = e;
				orig(c);
			}
			*c = out;
			return;
		}
		break;
	case HOOK_RACER_STEP: {   // F_b2e48(racer), from the seat loop: seat 0, then seat 1
		u32 r = A(0), s0 = LW(WORLD + 0x502c), s1 = LW(WORLD + 0x5030);
		if (!n_extra || r != (s1 ? s1 : s0)) break;
		Ctx save = *c;
		orig(c);
		Ctx out = *c;
		for (int k = 0; k < n_extra; k++) {   // then the extra racers that have no seat
			u32 e = extra_racer[k];
			if (e == s0 || e == s1 || extra_pending[k]) continue;
			u32 i0 = LW(WORLD + 0x4f84);
			SW(WORLD + 0x502c, e); SW(WORLD + 0x4f84, e);   // it borrows seat 0 meanwhile
			*c = save; c->r[4] = e; orig(c);
			SW(WORLD + 0x502c, s0); SW(WORLD + 0x4f84, i0);
		}
		*c = out;
		return;
	}
	case HOOK_TRACK_ENTITY_SECTION:   // F_1691b4(entity): racers are exempt, so is the extra one
		if (extra_index(A(0)) >= 0) {   // keep its section index current (the start of F_1691b4)
			u32 e = A(0), body = LW(e + 4);
			Ctx save = *c;
			c->r[4] = WORLD; c->r[5] = e; c->r[6] = 0; c->r[7] = 0;
			rt_call(c, 0x08800000 + 0x15e0e8);
			s32 sec = c->r[2];
			*c = save;
			if (sec > 0 && sec <= (s32)LW(WORLD + 0x4d10)) { SW(body + 0xc, sec); SW(body + 0x10, sec); }
			c->r[2] = 1;
			return;
		}
		break;
	case HOOK_RACER_MOVE:
		for (int k = 0; k < n_extra; k++) if (extra_pending[k]) {   // the race is running: switch the extra racer on
			u32 r = extra_pending[k]; extra_pending[k] = 0;            // like the world update does (F_15aafc)
			cam_ready[k] = false;
			u32 pr = LW(WORLD + 0x5024), eb = LW(r + 4);   // the local player (F_3731c(world, 0))
			if (!pr) pr = A(0);
			u32 pb = LW(pr + 4);
			Ctx save = *c;
			u32 seat0 = LW(WORLD + 0x502c), info0 = LW(WORLD + 0x4f84);
			SW(WORLD + 0x502c, r); SW(WORLD + 0x4f84, r);   // seated while it is switched on (kept by the first one in Free Play)
			c->r[4] = r; c->r[5] = 1; c->r[6] = 2;
			rt_call(c, 0x08800000 + 0x16c0b0);
			*c = save;
			extra_racer_place(c, r, pr, k);
			u32 ctl = LW(r + 0x480);   // its controller: reset it, as the racer's start step (F_aff70) does first
			c->r[4] = ctl; rt_call(c, LW(LW(ctl) + 8));
			*c = save;
			memcpy(PTR(PAD_DEVN(k)), PTR(PAD_DEV), PAD_DEV_SIZE);   // its player's pad device starts as a copy of the game's
			u32 in = LW(r + 0x8c) ? LW(LW(r + 0x8c) + 4) : 0;   // its input block: set up like F_159194 does for a
			if (in) {                                            // human racer (F_17138: button map, source), source 1
				c->r[4] = in; c->r[5] = 2; c->r[6] = 1;
				rt_call(c, 0x08800000 + 0x17138);
				*c = save;
			}
			c->r[4] = r; c->r[5] = 0; rt_call(c, 0x08800000 + 0xade6c);   // not computer-driven (F_ade6c, as F_159194)
			*c = save;
			c->r[4] = r; c->r[5] = 0; c->r[6] = 0; rt_call(c, LW(LW(r) + 0x38));   // its race-start step (F_aff70)
			*c = save;
			if (seat0 || k) {   // the first one keeps the empty rival seat
				SW(WORLD + 0x502c, seat0); SW(WORLD + 0x4f84, info0);                              // (Free Play); the others borrow it
			}
			LOGI("extra racer %d: switched on %08x (body %08x), player body %08x, input %08x, racers in race: %u", k, r, eb, pb, in, PTR(0x08A7C69C)[0]);
		}
		if (in_race && g_cfg.fps > 60) { racer_move(c, orig); return; }
		break;
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
