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
       HOOK_RACER_STEP, HOOK_RACE_STATE, HOOK_SEND_EVENT, HOOK_HUD_DRAW, HOOK_RING_PICKUP,
       HOOK_HUD_ELEM, HOOK_HUD_PART_A, HOOK_HUD_PART_B, HOOK_HUD_PART_C, HOOK_HUD_PART_D,
       HOOK_PLAYER_CONTACTS, HOOK_FOCUS_RACER_STEP, HOOK_RACER_UPDATE, HOOK_RACER_FINISH, HOOK_SLOT_ACTIVE,
       HOOK_ENTITY_HANDLE, HOOK_VOICE_PICK, HOOK_MENU_GOTO };
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

static void mp_trace(void);
static void mp_menu_vblank(void);
void game_vblank(void) {
	static int last_fps;
	if (g_cfg.fps != last_fps) { last_fps = g_cfg.fps; apply_frame_rate(); }
	text_patch();
	options_patch();
	options_input();
	charslot_vblank();
	mp_menu_vblank();
	mp_trace();
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
// it), and is set up as a human racer reading player 2's pad.
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
static int extra_char[MAX_EXTRA] = {-1, -1, -1};   // index into mp_char_names (-1: a racer file given by SR2_EXTRA_RACER)
bool mp_active(void);   // players 2-4 join this race (the Multiplayer screen, see below)
static bool race_ending;   // with extra racers: everyone is in, the game's finish runs (see placings)
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
		if (!mp_active() || k + 1 >= g_cfg.mp_players) return false;
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
		extra_char[k] = ci;
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
// put racer r at t (x, y, z, w), moving its body and itself as the game's moves do, facing like racer `like`
static void racer_place_at(Ctx *c, u32 r, const float *t, u32 like) {
	float d[4], cur[4];
	memcpy(PTR(r + 0xb0), t, 16);
	memcpy(PTR(r + 0xc0), PTR(like + 0xc0), 16);
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
	if (like != r) memcpy(PTR(eb + 0x30), PTR(LW(like + 4) + 0x30), 16);
	SW(eb + 8, LW(eb + 8) | 0xc000);
	c->r[4] = r; rt_call(c, 0x08800000 + 0xa2430);
	*c = save;
}

static void extra_racer_place(Ctx *c, u32 r, u32 pr, int k) {
	float t[4];
	// beside the player (x is across the track, y along it, z up): in a row ahead, the ones still standing
	// block the others; behind, there may be a wall
	float d0[3] = { -0.6f * (k + 1), 0, 0 };
	const char *e = getenv("SR2_EXTRA_DXYZ");
	if (e && sscanf(e, "%f,%f,%f", &d0[0], &d0[1], &d0[2]) == 3) for (int j = 0; j < 3; j++) d0[j] *= k + 1;
	memcpy(t, PTR(pr + 0xb0), 16);
	for (int j = 0; j < 3; j++) t[j] += d0[j];
	racer_place_at(c, r, t, pr);
}

// test aid ("warp K DIST" on the control fifo): put player K+1 on the course's path at that distance (or,
// with DIST < 0, beside player 1)
// (the path: each section slot's node list, slot header +0x444, a node's position at +0, its distance at +0x34)
static volatile int warp_k = -1; static volatile float warp_dist;
void game_request_warp(int k, float dist) { warp_dist = dist; warp_k = k; }
static void warp_pending(Ctx *c) {
	int k = warp_k; if (k < 0) return;
	warp_k = -1;
	u32 r = k ? (k <= n_extra ? extra_racer[k - 1] : 0) : LW(WORLD + 0x5024);
	if (!r) return;
	if (warp_dist < 0 && k) { extra_racer_place(c, r, LW(WORLD + 0x5024), k - 1); LOGI("warp: player %d beside player 1", k + 1); return; }
	for (int s = 0; s < 16; s++) {
		u32 arr = LW(WORLD_SLOT(s) + 0x444);
		if (arr < 0x08800000 || arr >= 0x0A000000) continue;
		for (int i = 0; i < 4096; i++) {
			u32 n = LW(arr + i * 4);
			if (n < 0x08800000 || n >= 0x0A000000) break;
			float d; memcpy(&d, PTR(n + 0x34), 4);
			if (d >= warp_dist) {
				float t[4]; memcpy(t, PTR(n), 16); t[2] += 1; t[3] = 1;
				racer_place_at(c, r, t, r);
				LOGI("warp: player %d to %.1f (slot %d node %d) at %.2f %.2f %.2f", k + 1, d, s, i, t[0], t[1], t[2]);
				return;
			}
		}
	}
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
	return split_view && n_extra && !extra_pending[n_extra - 1] && LW(CAM_MAIN) && !race_ending;   // the results: whole screen
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

// view k, or the whole screen (k < 0): viewport centre (XPOS/YPOS, float24), scale and scissor. Two views split the
// screen in halves, three or four in quarters. The first view is not clipped: it fills the screen (centred
// on its own part), and the other views are drawn over the rest. Drawing the first view into only part of
// the screen leaves the colours washed out, as if something later in the frame weighs in the parts nothing
// was drawn to.
static void ge_region_scaled(int k, bool whole) {
	int x0 = 0, y0 = 0, w = 480, h = 272;
	if (k >= 0 && n_extra >= 2) { w = 240; h = 136; x0 = (k & 1) * 240; y0 = (k >> 1) * 136; }
	else if (k >= 0) { if (split_view == 2) { w = 240; x0 = k * 240; } else { h = 136; y0 = k * 136; } }
	float px = 2048.0f - 240 + x0 + w / 2, py = 2048.0f - 136 + y0 + h / 2;   // the screen offset stays (2048-240, 2048-136)
	u32 bx, by; memcpy(&bx, &px, 4); memcpy(&by, &py, 4);
	ge_put(0x45000000 | bx >> 8);
	ge_put(0x46000000 | by >> 8);
	// quarters show the whole view at half size (viewport scale, XSCALE/YSCALE); halves show its middle
	float sx = 240, sy = -136;
	if (k >= 0 && (n_extra >= 2 || whole)) { sx = 120; sy = -68; }
	memcpy(&bx, &sx, 4); memcpy(&by, &sy, 4);
	ge_put(0x42000000 | bx >> 8);
	ge_put(0x43000000 | by >> 8);
	if (k <= 0) { x0 = 0; y0 = 0; w = 480; h = 272; }
	ge_put(0xD4000000 | x0 | y0 << 10);
	ge_put(0xD5000000 | (x0 + w - 1) | (y0 + h - 1) << 10);
}

static void ge_region(int k) { ge_region_scaled(k, false); }

// split screen: the shared race meter and timer, smaller than the game draws them so the bottom views keep
// more room, kept at the bottom of the screen; the timer moves from the top to just above the meter
#define HUD_SHARED_SCALE 0.65f
#define HUD_TIMER_Y  20.0f    // the timer's middle, from the top of the screen
#define HUD_TIMER_TO 232.0f   // where it goes (full size), just above the meter
static void ge_place(float scale, float dy) {   // the whole screen at that scale, about its middle, moved down by dy
	float px = 2048.0f, py = 2048.0f + dy, sx = 240 * scale, sy = -136 * scale;
	u32 b;
	memcpy(&b, &px, 4); ge_put(0x45000000 | b >> 8);
	memcpy(&b, &py, 4); ge_put(0x46000000 | b >> 8);
	memcpy(&b, &sx, 4); ge_put(0x42000000 | b >> 8);
	memcpy(&b, &sy, 4); ge_put(0x43000000 | b >> 8);
}
// the meter keeps its bottom edge where it is; the timer goes to HUD_TIMER_TO measured up from the bottom, scaled
#define HUD_METER_DY (136 * (1 - HUD_SHARED_SCALE))
#define HUD_TIMER_DY (136 - (272 - HUD_TIMER_TO) * HUD_SHARED_SCALE + (136 - HUD_TIMER_Y) * HUD_SHARED_SCALE)

static float race_progress(Ctx *c, u32 racer);
static int finish_place[1 + MAX_EXTRA], n_finished;
static int split_lead;   // the player the fourth quarter shows (three players), for the overlay
static int split_leader(Ctx *c, u32 p1) {   // the player furthest along who has not finished (0: player 1)
	int lead = 0; float best = -1;
	for (int k = 0; k <= n_extra; k++) {
		if (finish_place[k] && n_finished <= n_extra) continue;
		float p = race_progress(c, k ? extra_racer[k - 1] : p1);
		if (p > best) { best = p; lead = k; }
	}
	return split_lead = lead;
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
	if (n_extra == 2) {   // three players: the fourth quarter follows whoever leads the race
		int lead = split_leader(c, p1);
		if (lead) { cam_swap(lead - 1); SW(WORLD + 0x5024, extra_racer[lead - 1]); }
		view_refresh(c);
		ge_region(3); orig(c);
		*c = save;
		if (lead) { SW(WORLD + 0x5024, p1); cam_swap(lead - 1); }
	}
	view_refresh(c);
	ge_region(-1);
	*c = out;
}

#define HUD_RINGS    0x30    // HUD (F_12ad44's argument): the ring count shown, a float
#define HUD_BASE        0x08A7DF30   // the HUD; its elements (F_12f280) are at these offsets:
#define HUD_TIMER       0x2c0        // the timer (box and digits)
#define HUD_METER       0x1550       // the race meter's bar, at the bottom
#define HUD_METER_ICON  0x1600       // the local player's icon on it
#define HUD_RIVAL_ICON  0x16b0       // the rival's
#define HUD_DRAW_TEXT   0x12d868     // F_12d868 (part A): draws the text the elements queued (rings, timer digits)
// split screen draws the HUD in passes: each view's own (rings, boost), then once for the whole screen the
// race meter (again per player for its icon) and the timer
enum { HUD_PASS_ALL, HUD_PASS_VIEW, HUD_PASS_METER, HUD_PASS_ICON, HUD_PASS_TIMER };
static int hud_pass, hud_in_elem;
#define HUD_ICON_X      0x60   // a meter icon: its x (-1..1), and its image (a HUD texture)
#define HUD_ICON_IMAGE  0x74
#define HUD_METER_LENGTH (285.0f / 272)   // the icon's x per unit of race progress (F_129380)
// the HUD's textures are in a list (node at +0x34: next, previous, ..., the name at +0x10); the heads by character
static const char *const hud_heads[MP_CHARS] = {"head sonic", "head shadow", "head silver", "head knuckles",
	"rouge_head", "Tails_head", "espio_head", "head metalsonic"};
static u32 hud_texture(u32 from, const char *name) {
	u32 node = from + 0x34;
	for (int i = 0; i < 512 && node >= 0x08800000 && node < 0x0A000000; i++, node = LW(node)) {
		if (!strncmp((const char *)PTR(node + 0x10), name, 32)) return node - 0x34;
		if (i && node == from + 0x34) break;
	}
	return 0;
}
static float race_progress(Ctx *c, u32 racer) {   // F_b070c: how far along the race a racer is, 0..1
	Ctx save = *c;
	c->r[4] = racer; rt_call(c, 0x08800000 + 0xb070c);
	float p = c->f[0];
	*c = save;
	return p;
}
#define RACER_RINGS  0x72c   // racer: rings held
static int race_state_depth;   // inside F_592cc
static int extra_index(u32 r) { for (int k = 0; k < n_extra; k++) if (extra_racer[k] == r) return k; return -1; }
static int input_extra(u32 in) {   // which extra racer an input block belongs to
	for (int k = 0; k < n_extra; k++) { u32 r = extra_racer[k]; if (LW(r + 0x8c) && LW(LW(r + 0x8c) + 4) == in) return k; }
	return -1;
}

// Placings, with players 2-4 in the race: each player's first crossing of the finish line (F_b2530) takes the
// next place, and the race goes on; it ends (the game's own finish, for player 1) once everyone is in, or
// MP_FINISH_GRACE seconds after the first. Without extra racers the finish is the game's own.
#define GOAL_CONTACT_RA  0x105310   // F_105238, the goal line's contact with a racer, calls F_b2530 here
#define GOAL_TRIGGERED   0x1bc      // goal line: crossed already (no one else counts after that)
#define MP_FINISH_GRACE  90
static u64 first_finish_vb;
static u32 p1_racer;   // player 1's racer (the local player is swapped while extra racers are stepped)
static int player_index(u32 r) { if (r && r == p1_racer) return 0; int k = extra_index(r); return k >= 0 ? k + 1 : -1; }
static int p1_char = -1;   // player 1's character, from their meter icon's head (at the start of the race)
static int head_char(void);
#define RACER_TIME  0x750   // a racer's finish time (s), set by F_b2530 from the race timer; the results show it
static float finish_time[1 + MAX_EXTRA];
static void placings_reset(u32 p1) { memset(finish_place, 0, sizeof finish_place); n_finished = 0; race_ending = false; p1_racer = p1; p1_char = head_char(); }
static float race_time(Ctx *c) {   // F_12e7ac(the HUD's timer): the race time now
	Ctx save = *c;
	c->r[4] = HUD_BASE + HUD_TIMER; rt_call(c, 0x08800000 + 0x12e7ac);
	float t = c->f[0];
	*c = save;
	return t;
}
static void p1_finish(Ctx *c, void (*orig)(Ctx *c)) {   // the game's finish for player 1 (the results), with their own time
	race_ending = true;
	c->r[4] = p1_racer; orig(c);
	if (finish_place[0]) memcpy(PTR(p1_racer + RACER_TIME), &finish_time[0], 4);
}
static void finish_call(Ctx *c) { rt_call(c, 0x08800000 + 0xb2530); }   // F_b2530 (the hook lets it through now)
static void race_end(Ctx *c) {
	Ctx save = *c;
	p1_finish(c, finish_call);
	*c = save;
}

// for the overlay (menu.cpp): each player's place (0: still racing), the players' count (0: no placings shown),
// each player's view in the game image (x, y, w, h as fractions), and whether the race is over
int mp_placings(int *place, float *time, float (*view)[4], int *over) {
	if (!n_extra || !p1_racer || !in_race) return 0;
	int n = n_extra + 1;
	for (int k = 0; k < n; k++) {
		place[k] = finish_place[k]; time[k] = finish_time[k];
		float x = 0, y = 0, w = 1, h = 1;
		if (n_extra >= 2) { w = h = 0.5f; x = (k & 1) * 0.5f; y = (k >> 1) * 0.5f; }
		else if (split_view == 2) { w = 0.5f; x = k * 0.5f; }
		else { h = 0.5f; y = k * 0.5f; }
		view[k][0] = x; view[k][1] = y; view[k][2] = w; view[k][3] = h;
	}
	*over = race_ending;
	return n;
}
static int head_char(void) {
	u32 img = LW(HUD_BASE + HUD_METER_ICON + HUD_ICON_IMAGE);
	if (img < 0x08800000 || img >= 0x0A000000) return -1;
	for (int i = 0; i < MP_CHARS; i++) if (!strncmp((const char *)PTR(img + 0x34 + 0x10), hud_heads[i], 32)) return i;
	return -1;
}
int mp_split_lead(void) { return n_extra == 2 && p1_racer && !race_ending ? split_lead : -1; }   // see split_leader
int mp_player_char(int k) { return k ? (k <= n_extra ? extra_char[k - 1] : -1) : p1_char; }

u32 rt_hookval(Ctx *c, u32 id, u32 v) {
	switch (id) {
	case HOOK_COPTER_LIFT_DT:                                // Tails' Tail Copter: lift time per flap
	case HOOK_SIG_START_DT: return at_least_30fps_dt(v);     // signature move start: lift time and kick
	}
	return v;
}
// ---------------------------------------------------------------- the Multiplayer screen
// Main Menu > Multiplayer (the PSP's Wireless Play and Game Sharing) is replaced by a local race setup: the game
// goes to the Single Player menu instead, and this screen (drawn by menu.cpp) covers it. Controllers join as
// players 2-4 with A / cross and pick a character with left / right; player 1 is the controller (and keyboard)
// that opened it. When player 1 starts, the Single Player menu is driven to Free Play by pressing its buttons, so
// the game sets that mode up as usual, and player 1 goes on through Free Play's stage and character select. Extra
// racers are added only in that Free Play (mp_mode), which ends back in the Single Player or Main Menu.
#define MENUS       0x08A842BC   // the menus: +0x9f4 the current screen (its hash at +0, the chosen button at +0x20)
#define SCR_MAIN    0x4050d926
#define SCR_SINGLE  0xa69d1022
#define SCR_MULTI   0x8d082f86
#define SCR_STAGES  0x8898eb5a
#define BTN_Y       0x14         // a menu button's height on screen (larger is higher)
enum { B_START = 0x8, LOBBY_EMPTY = -100 };
int fe_pads(int *ids, u32 *buttons, const char **names, int max);
int fe_last_pad(void);
void fe_set_seats(const int *ids);

static bool mp_mode;
static struct { bool open; int pad[4], ch[4]; } lobby;   // pad: controller id (player 1: -1 keyboard only), or empty
static struct { int ids[16]; u32 prev[16]; int n; } lobby_pads;
static int lobby_last[4] = {LOBBY_EMPTY, LOBBY_EMPTY, LOBBY_EMPTY, LOBBY_EMPTY};   // the last race's players, seated again
const char *fe_pad_name(int id);
static enum { MAC_IDLE, MAC_SETTLE, MAC_UP, MAC_UP_WAIT, MAC_CROSS, MAC_WAIT_STAGES, MAC_BACK, MAC_WAIT_MAIN } mac;
static int mac_t, mac_tries, mac_hold; static u32 mac_button, mac_sel; static float mac_y;

static u32 menu_screen(void) { return LW(MENUS + 0x9f4); }
static u32 menu_hash(void) { u32 s = menu_screen(); return s ? LW(s) : 0; }
bool mp_active(void) { return mp_mode || getenv("SR2_MP_TEST"); }

static u32 pad_pressed(int id, u32 b) {   // buttons newly down on that controller
	int i = 0;
	while (i < lobby_pads.n && lobby_pads.ids[i] != id) i++;
	if (i == lobby_pads.n) { if (i == 16) return 0; lobby_pads.n++; lobby_pads.ids[i] = id; lobby_pads.prev[i] = b; return 0; }
	u32 e = b & ~lobby_pads.prev[i];
	lobby_pads.prev[i] = b;
	return e;
}

static void lobby_begin(void) {
	lobby.open = true;
	lobby.pad[0] = fe_last_pad();
	const int *def[] = {&g_cfg.mp_char2, &g_cfg.mp_char3, &g_cfg.mp_char4};
	for (int k = 1; k < 4; k++) {
		int id = lobby_last[k];
		bool here = id != LOBBY_EMPTY && id != lobby.pad[0] && (id < -1 || fe_pad_name(id));
		lobby.pad[k] = here ? id : LOBBY_EMPTY; lobby.ch[k] = *def[k - 1];
	}
	int ids[4] = {lobby.pad[0], -1, -1, -1};
	fe_set_seats(ids);   // only player 1's controller (and the keyboard) drives the menus meanwhile
	lobby_pads.n = 0;
	int id[16]; u32 b[16]; const char *nm[16];
	int n = fe_pads(id, b, nm, 16);
	for (int i = 0; i < n; i++) pad_pressed(id[i], b[i]);   // what is held now (the A that opened it) is not a press
	LOGI("multiplayer: setup opened, player 1 on controller %d", lobby.pad[0]);
}

static void lobby_end(bool start) {
	lobby.open = false;
	if (!start) { mac = MAC_BACK; mac_t = 0; return; }
	int ids[4] = {lobby.pad[0], -1, -1, -1}, *chs[] = {&g_cfg.mp_char2, &g_cfg.mp_char3, &g_cfg.mp_char4}, n = 1;
	for (int k = 1; k < 4; k++)   // players in the order of their seats, gaps closed
		if (lobby.pad[k] != LOBBY_EMPTY) { ids[n] = lobby.pad[k]; *chs[n - 1] = lobby.ch[k]; n++; }
	for (int k = 0; k < 4; k++) lobby_last[k] = k < n ? ids[k] : LOBBY_EMPTY;
	g_cfg.mp_players = n;
	config_save();
	fe_set_seats(ids);
	mac = MAC_SETTLE; mac_t = 0; mac_tries = 0;
	LOGI("multiplayer: %d players, to Free Play", n);
}

static int lobby_players(void) { int n = 1; for (int k = 1; k < 4; k++) n += lobby.pad[k] != LOBBY_EMPTY; return n; }

static void lobby_input(u32 p1) {
	static u32 p1_prev = ~0u;
	u32 p1e = p1 & ~p1_prev;
	p1_prev = p1;
	int id[16]; u32 b[16]; const char *nm[16];
	int n = fe_pads(id, b, nm, 16);
	for (int i = 0; i < n; i++) {
		u32 e = pad_pressed(id[i], b[i]);
		if (id[i] == lobby.pad[0]) continue;   // player 1's, read with the keyboard
		int k = 1;
		while (k < 4 && lobby.pad[k] != id[i]) k++;
		if (k == 4) {   // not playing yet: A / cross or Start takes the first free seat
			if (!(e & (B_CROSS | B_START))) continue;
			for (k = 1; k < 4 && lobby.pad[k] != LOBBY_EMPTY; k++) {}
			if (k < 4) { lobby.pad[k] = id[i]; LOGI("multiplayer: controller %d joins as player %d", id[i], k + 1); }
			continue;
		}
		if (e & B_CIRCLE) { lobby.pad[k] = LOBBY_EMPTY; continue; }
		if (e & B_LEFT) lobby.ch[k] = (lobby.ch[k] + MP_CHARS - 1) % MP_CHARS;
		if (e & B_RIGHT) lobby.ch[k] = (lobby.ch[k] + 1) % MP_CHARS;
	}
	if (p1e & B_CIRCLE) lobby_end(false);
	else if ((p1e & (B_CROSS | B_START)) && lobby_players() > 1) lobby_end(true);
}

// for menu.cpp: the setup screen while it is open (per seat: controller id or LOBBY_EMPTY, character)
int mp_lobby(int *pad, int *ch) {
	if (!lobby.open && mac != MAC_SETTLE && mac != MAC_UP && mac != MAC_UP_WAIT && mac != MAC_CROSS && mac != MAC_WAIT_STAGES) return 0;
	for (int k = 0; k < 4; k++) { pad[k] = lobby.pad[k]; ch[k] = lobby.ch[k]; }
	return lobby.open ? 1 : 2;   // 2: starting
}

static void mac_press(u32 b) { mac_button = b; mac_hold = 4; }
static void mac_abort(const char *why) { LOGE("multiplayer: could not reach Free Play (%s)", why); mac = MAC_IDLE; mp_mode = false; }

// Free Play is the Single Player menu's last button: up from the top wraps to it, so up is pressed until the
// chosen button moves down the screen
static void macro_vblank(void) {
	if (mac_hold) { fe_buttons = mac_button; mac_hold--; return; }
	fe_buttons = 0;
	mac_t++;
	u32 s = menu_screen(), h = menu_hash();
	switch (mac) {
	case MAC_SETTLE:   // the Single Player menu, still for a moment
		if (h != SCR_SINGLE || !LW(s + 0x20)) { mac_tries = 0; if (mac_t > 600) mac_abort("no Single Player menu"); break; }
		if (++mac_tries >= 20) { mac = MAC_UP; mac_tries = 0; }
		break;
	case MAC_UP:
		mac_sel = LW(s + 0x20); memcpy(&mac_y, PTR(mac_sel + BTN_Y), 4);
		mac_press(B_UP); mac = MAC_UP_WAIT; mac_t = 0;
		break;
	case MAC_UP_WAIT: {
		if (h != SCR_SINGLE) { mac_abort("left the Single Player menu"); break; }
		u32 sel = LW(s + 0x20);
		if (sel == mac_sel) { if (mac_t > 30) mac = MAC_UP; break; }
		float y; memcpy(&y, PTR(sel + BTN_Y), 4);
		if (y < mac_y) { mac = MAC_CROSS; mac_t = 0; }
		else if (++mac_tries > 6) mac_abort("no Free Play button");
		else mac = MAC_UP;
		break;
	}
	case MAC_CROSS:
		if (mac_t < 10) break;
		mp_mode = true;
		mac_press(B_CROSS); mac = MAC_WAIT_STAGES; mac_t = 0;
		break;
	case MAC_WAIT_STAGES:
		if (h == SCR_STAGES) { mac = MAC_IDLE; LOGI("multiplayer: in Free Play"); }
		else if (mac_t > 600) mac_abort("no stage select");
		break;
	case MAC_BACK:   // the setup was closed: back to the Main Menu
		if (h != SCR_SINGLE) { if (mac_t > 600) mac = MAC_IDLE; break; }
		if (mac_t < 10) break;
		mac_press(B_CIRCLE); mac = MAC_WAIT_MAIN; mac_t = 0;
		break;
	case MAC_WAIT_MAIN:
		if (h == SCR_MAIN || mac_t > 600) mac = MAC_IDLE;
		break;
	default: break;
	}
}

static void mp_menu_vblank(void) {
	if (lobby.open) { lobby_input(fe_buttons); fe_buttons = 0; }
	else if (mac != MAC_IDLE) macro_vblank();
	else if (mp_mode) {   // back in the Single Player or Main Menu: the multiplayer Free Play is over
		u32 h = menu_hash();
		if (h == SCR_SINGLE || h == SCR_MAIN) { mp_mode = false; fe_set_seats(NULL); LOGI("multiplayer: ended"); }
	}
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
		if (getenv("SR2_EV_TRACE")) LOGI("race %08x state %u (was %u) from %08x vb %llu", A(0), A(1), LW(A(0) + 0x24), c->r[31], (unsigned long long)g_vblank_count);
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
	case HOOK_RING_PICKUP: {   // F_16b90(rings, racer)
		static int tr = -1; if (tr < 0) tr = getenv("SR2_EV_TRACE") != NULL;
		if (!tr) break;
		u32 rl = A(0), r = A(1);
		orig(c);
		static u32 last[8]; int k = extra_index(r) + 1;
		if (k >= 0 && k < 8 && (last[k] != rl || c->r[2])) { last[k] = rl; LOGI("rings %08x (n %u) racer %08x (p%d): took %u", rl, LW(rl + 0x10), r, k + 1, c->r[2]); }
		return;
	}
	case HOOK_HUD_ELEM: case HOOK_HUD_PART_A: case HOOK_HUD_PART_B: case HOOK_HUD_PART_C: case HOOK_HUD_PART_D: {
		if (hud_pass) {   // split screen: which pieces this pass of the HUD draws
			u32 e = id == HOOK_HUD_ELEM ? A(0) - HUD_BASE : ~0u;
			bool shared = e == HUD_TIMER || e == HUD_METER || e == HUD_METER_ICON || e == HUD_RIVAL_ICON;
			if (id == HOOK_HUD_PART_A) break;   // the text queued by the elements this pass draws
			if (hud_pass == HUD_PASS_VIEW) { if (shared) return; }
			else if (id == HOOK_HUD_ELEM) {
				bool want = hud_pass == HUD_PASS_TIMER ? e == HUD_TIMER
				          : hud_pass == HUD_PASS_METER ? e == HUD_METER || e == HUD_RIVAL_ICON : e == HUD_METER_ICON;
				if (!want) return;
				hud_in_elem++; orig(c); hud_in_elem--;
				return;
			} else if (!hud_in_elem) return;   // parts outside the wanted elements
		}
		// test aid: SR2_HUD_HIDE="a".."d" (parts) or element offsets (hex), or SR2_HUD_HIDE_FILE, read again every frame
		static char hide[256]; static int got;
		static int list_on = -1; if (list_on < 0) list_on = getenv("SR2_HUD_LIST") != NULL;
		if (list_on) {   // test aid: each element or part drawn, once per call site
			static u32 seen[256][2]; static int ns; u32 e = id == HOOK_HUD_ELEM ? A(0) - 0x08A7DF30 : 0xa00000 + id, ra = c->r[31]; int i = 0;
			while (i < ns && !(seen[i][0] == (id == HOOK_HUD_ELEM ? e : A(0)) && seen[i][1] == ra)) i++;
			if (i == ns && ns < 256) { seen[ns][0] = id == HOOK_HUD_ELEM ? e : A(0); seen[ns][1] = ra; ns++; LOGI("hud %s %x a0 %08x ra %06x", id == HOOK_HUD_ELEM ? "elem" : "part", e, A(0), ra - 0x08800000); }
		}
		static const char *hf;
		if (!got) { const char *e = getenv("SR2_HUD_HIDE"); snprintf(hide, sizeof hide, "%s", e ? e : ""); hf = getenv("SR2_HUD_HIDE_FILE"); got = 1; }
		static u64 read_at;
		if (hf && read_at != g_vblank_count) {
			read_at = g_vblank_count; hide[0] = 0;
			FILE *f = fopen(hf, "r"); if (f) { if (!fgets(hide, sizeof hide, f)) hide[0] = 0; fclose(f); }
			hide[strcspn(hide, "\n")] = 0;
		}
		if (!hide[0]) break;
		char key[16];
		if (id == HOOK_HUD_ELEM) snprintf(key, sizeof key, ",%x,", A(0) - 0x08A7DF30); else snprintf(key, sizeof key, ",%c,", 'a' + id - HOOK_HUD_PART_A);
		char list[256]; snprintf(list, sizeof list, ",%s,", hide);
		if (strstr(list, key)) return;
		snprintf(key, sizeof key, ",r%x,", c->r[31] - 0x08800000);   // or by call site
		if (strstr(list, key)) return;
		snprintf(key, sizeof key, ",p%x,", A(0));   // or by argument
		if (strstr(list, key)) return;
		break;
	}
	case HOOK_PLAYER_CONTACTS:   // F_15c2d0(world): only for the local player; again for each extra racer
		if (n_extra && !getenv("SR2_NO_CONTACTS")) {
			Ctx save = *c;
			orig(c);
			Ctx out = *c;
			u32 p1 = LW(WORLD + 0x5024);
			for (int k = 0; k < n_extra; k++) {
				if (extra_pending[k]) continue;
				*c = save; SW(WORLD + 0x5024, extra_racer[k]); orig(c);
			}
			SW(WORLD + 0x5024, p1);
			*c = out;
			return;
		}
		break;
	case HOOK_FOCUS_RACER_STEP:   // F_15e4a8(world, racer)
		if (n_extra && getenv("SR2_FOCUS_STEP")) {
			Ctx save = *c;
			orig(c);
			Ctx out = *c;
			for (int k = 0; k < n_extra; k++) {
				if (extra_pending[k]) continue;
				*c = save; c->r[5] = extra_racer[k]; orig(c);
			}
			*c = out;
			return;
		}
		break;
	case HOOK_SLOT_ACTIVE: {   // F_160724(world, slot): whether a load slot is live (14, 15 and 63 always). Sounds
		// start and keep playing only for what is in a live slot; for the sound code an extra racer's slot counts as
		// live, so players 2-4 are heard, voices too. Only there: other callers take a live slot for a loaded course
		// section (and stop drawing the racer).
		u32 ra = c->r[31];
		if (ra != 0x0883d15c && ra != 0x0883d330 && ra != 0x0883d564   // the play calls (F_3d0xx, F_3d294, F_3d48c)
		    && ra != 0x0896252c) break;                                  // F_162514, from the sound update
		for (int k = 0; k < n_extra; k++)
			if (!extra_pending[k] && A(1) == LW(LW(extra_racer[k] + 4) + 0x10)) { c->r[2] = 1; return; }
		break;
	}
	case HOOK_VOICE_PICK:   // F_126efc(voice, kind, ...): at the start (kind 7, from F_e1xxx) player 1 says a line;
		// players 2-4 say one of theirs too, one after another (voice+0x204: seconds until it is said)
		if (n_extra && !race_ending && A(1) == 7 && LW(c->r[29]) == 0x088e2194 && LW(A(0)) == p1_racer) {
			orig(c);
			Ctx save = *c;
			for (int k = 0; k < n_extra; k++) {
				u32 v = extra_pending[k] ? 0 : LW(extra_racer[k] + 0x4ec);
				if (!v) continue;
				c->r[4] = v; c->r[5] = 7; c->r[6] = save.r[6]; c->r[7] = save.r[7];
				orig(c);
				*c = save;
				float d = 1.2f * (k + 1);
				if (LW(v + 0x208)) memcpy(PTR(v + 0x204), &d, 4);
			}
			return;
		}
		break;
	case HOOK_MENU_GOTO:   // F_14cb34(menus, hash, transition): Main Menu > Multiplayer goes to the local race setup
		if (getenv("SR2_EV_TRACE")) LOGI("menus: %08x -> %08x", menu_hash(), A(1));
		if (A(1) == SCR_MULTI && menu_hash() == SCR_MAIN && !lobby.open) { c->r[5] = SCR_SINGLE; lobby_begin(); }
		break;
	case HOOK_ENTITY_HANDLE:   // F_15f84(entity): see hooks.json; the handle of an extra racer as for slot 15
		if (A(0) && extra_index(A(0)) >= 0) {
			u32 b = LW(A(0) + 4), s = LW(b + 0x10);
			SW(b + 0x10, 15); orig(c); SW(b + 0x10, s);
			return;
		}
		break;
	case HOOK_RACER_FINISH: {   // F_b2530(racer)
		if (!n_extra || race_ending) break;
		int k = player_index(A(0));
		if (k < 0) break;
		if (!finish_place[k]) {
			finish_place[k] = ++n_finished;
			finish_time[k] = race_time(c);
			if (n_finished == 1) first_finish_vb = g_vblank_count;
			LOGI("finish: player %d is %d of %d", k + 1, n_finished, n_extra + 1);
		}
		if (c->r[31] == 0x08800000 + GOAL_CONTACT_RA) SB(c->r[16] + GOAL_TRIGGERED, 0);   // for the others still to come
		if (n_finished == n_extra + 1) { p1_finish(c, orig); return; }
		c->r[2] = 0;
		return;
	}
	case HOOK_RACER_UPDATE:   // F_af53c(racer, ...)
		if (getenv("SR2_UPDATE_AS_P1") && extra_index(A(0)) >= 0) {
			u32 p1 = LW(WORLD + 0x5024), f = LW(WORLD + 0x5028);
			SW(WORLD + 0x5024, A(0)); SW(WORLD + 0x5028, A(0));
			orig(c);
			SW(WORLD + 0x5024, p1); SW(WORLD + 0x5028, f);
			return;
		}
		break;
	case HOOK_HUD_DRAW:   // F_12ad44(hud)
		if (split_view_on()) {   // one HUD per view, scaled into it like the view
			Ctx save = *c;
			u32 p1 = LW(WORLD + 0x5024), hud = A(0), rings = LW(hud + HUD_RINGS);
			for (int k = 0; k <= n_extra; k++) {
				if (k) {   // the boost bar reads the local player as it draws; the ring count is kept by the HUD
					u32 r = extra_racer[k - 1];
					float n = (float)(s32)LW(r + RACER_RINGS);
					SW(WORLD + 0x5024, r); memcpy(PTR(hud + HUD_RINGS), &n, 4);
				}
				hud_pass = HUD_PASS_VIEW;
				ge_region_scaled(k, true); orig(c);   // halves too get the whole HUD, at half size
				rt_call(c, 0x08800000 + HUD_DRAW_TEXT);   // its text now, in its view (the HUD draws it next time)
				*c = save;
			}
			SW(WORLD + 0x5024, p1); SW(hud + HUD_RINGS, rings);
			ge_region(-1);
			ge_place(HUD_SHARED_SCALE, HUD_METER_DY);
			hud_pass = HUD_PASS_METER; orig(c);   // the race meter
			rt_call(c, 0x08800000 + HUD_DRAW_TEXT);
			*c = save;
			// each player's icon on it, player 1's on top: the icon moved along by that player's progress
			// (as F_129380 places it), with their character's head
			u32 icon = HUD_BASE + HUD_METER_ICON, x0 = LW(icon + HUD_ICON_X), img0 = LW(icon + HUD_ICON_IMAGE);
			float p0 = race_progress(c, p1), xf; memcpy(&xf, &x0, 4);
			for (int k = n_extra; k >= 0; k--) {
				if (k) {
					float x = xf + (race_progress(c, extra_racer[k - 1]) - p0) * HUD_METER_LENGTH;
					u32 xb; memcpy(&xb, &x, 4); SW(icon + HUD_ICON_X, xb);
					u32 head = extra_char[k - 1] >= 0 ? hud_texture(img0, hud_heads[extra_char[k - 1]]) : 0;
					SW(icon + HUD_ICON_IMAGE, head ? head : img0);
				} else { SW(icon + HUD_ICON_X, x0); SW(icon + HUD_ICON_IMAGE, img0); }
				hud_pass = HUD_PASS_ICON; orig(c);
				rt_call(c, 0x08800000 + HUD_DRAW_TEXT);
				*c = save;
			}
			ge_place(HUD_SHARED_SCALE, HUD_TIMER_DY);   // the timer, just above the meter
			hud_pass = HUD_PASS_TIMER; orig(c);
			rt_call(c, 0x08800000 + HUD_DRAW_TEXT);
			*c = save;
			hud_pass = HUD_PASS_ALL;
			ge_region(-1);
			return;
		}
		break;
	case HOOK_SEND_EVENT:   // F_16c388(manager, target, source, hash, ...)
		if (getenv("SR2_EV_TRACE") && A(1) && A(1) == LW(WORLD + 0x5024)) {   // test aid: events the local player gets
			static u32 seen[256][2]; static int ns;
			int i = 0; while (i < ns && !(seen[i][0] == A(3) && seen[i][1] == c->r[31])) i++;
			if (i == ns && ns < 256) { seen[ns][0] = A(3); seen[ns][1] = c->r[31]; ns++;
				LOGI("event to player: hash %08x source %08x (vt %08x) from %08x vb %llu", A(3), A(2), A(2) ? LW(A(2)) : 0, c->r[31], (unsigned long long)g_vblank_count); }
		}
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
			if (sec > 0 && sec <= (s32)LW(WORLD + 0x4d10)) { SW(body + 0xc, sec); if (getenv("SR2_OLD_SEC")) SW(body + 0x10, sec); }   // +0x10 stays its slot, as a racer's
			c->r[2] = 1;
			return;
		}
		break;
	case HOOK_RACER_MOVE:
		warp_pending(c);
		static int grace = -1;
		if (grace < 0) grace = getenv("SR2_MP_GRACE") ? atoi(getenv("SR2_MP_GRACE")) : MP_FINISH_GRACE;   // test aid
		if (n_finished && !race_ending && g_vblank_count - first_finish_vb > (u64)grace * 60) {
			LOGI("finish: %d s since the first, ending the race", grace);
			race_end(c);
		}
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
			if (k == 0) placings_reset(pr);
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
			// None keeps a seat: with player 2 in Free Play's empty rival seat, player 1 could not move. All are
			// stepped after the seated racers instead (HOOK_RACER_STEP).
			SW(WORLD + 0x502c, seat0); SW(WORLD + 0x4f84, info0);
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

static void mp_trace(void) {
	static int trace = -1;
	if (trace < 0) trace = getenv("SR2_MP_TRACE") != NULL;
	if (trace && n_extra && g_vblank_count % 30 == 0) {   // test aid: where each player and camera is
		char line[512]; int o = 0;
		u32 rs[4] = {LW(WORLD + 0x5024)};
		for (int k = 0; k < n_extra; k++) rs[k + 1] = extra_racer[k];
		for (int k = 0; k <= n_extra; k++) {
			u32 r = rs[k]; float *pos = r && LW(r + 4) ? (float *)PTR(LW(r + 4) + 0x20) : NULL;
			o += snprintf(line + o, sizeof line - o, " p%d %08x (%.1f %.1f %.1f)", k + 1, r, pos ? pos[0] : 0, pos ? pos[1] : 0, pos ? pos[2] : 0);
		}
		u32 cam = LW(CAM_MAIN);
		LOGI("mp:%s cam target %08x seats %08x %08x", line, cam ? LW(cam + CAM_TARGET) : 0, LW(WORLD + 0x502c), LW(WORLD + 0x5030));
	}
}

