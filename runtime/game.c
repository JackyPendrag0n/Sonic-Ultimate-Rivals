// Sonic Rivals 2 game layer: settings persistence and the code hooks (data/hooks.json) that the
// recompiled game calls into: the race frame rate and the PC options in the game's Options screen.
#define _GNU_SOURCE
#include "kernel.h"
#include "config.h"
#include "gpu/ge_soft.h"
#include <sys/stat.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#define ENV_ON(name) ({ static int on_ = -1; if (on_ < 0) on_ = getenv(name) != NULL; on_; })   // a test switch, looked up once

GameConfig g_cfg = {
	.renderer = 2, .internal_scale = 2, .tex_filter = 0, .anisotropy = 4, .vsync = true, .fullscreen = false,
	.window_scale = 2, .keep_aspect = true, .fps = 60, .show_fps = false, .stick_deadzone = 15, .volume = 100,
	.mp_players = 1, .mp_char2 = 1, .mp_char3 = 3, .mp_char4 = 4,
};
const char *const mp_char_names[MP_CHARS] = {"Sonic", "Shadow", "Silver", "Knuckles", "Rouge", "Tails", "Espio", "Metal Sonic"};
const char *const mp_char_files[MP_CHARS] = {"SONIC", "SHADOW", "SILVER", "KNUCKLES", "ROUGE", "TAILS", "ESPIO", "METALSONIC"};

// ---------------------------------------------------------------- config file
static char cfg_path[600];
const char *config_path(void) {
	if (!cfg_path[0]) {
		char dir[512];
		host_config_dir(dir, sizeof dir);
		char tmp[512]; snprintf(tmp, sizeof tmp, "%s", dir);
		for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
		mkdir(tmp, 0755);
		snprintf(cfg_path, sizeof cfg_path, "%s/settings.ini", dir);
	}
	return cfg_path;
}

#define CFG_FIELDS(X) \
	X(renderer, i) X(internal_scale, i) X(tex_filter, i) X(anisotropy, i) X(vsync, b) X(vsync_lock, b) X(fullscreen, b) \
	X(window_scale, i) X(keep_aspect, b) X(fps, i) X(show_fps, b) X(stick_deadzone, i) X(volume, i) X(split_layout, i) \
	X(mp_players, i) X(mp_char2, i) X(mp_char3, i) X(mp_char4, i) X(pad_all, b) X(mp_test_seats, b) X(mp_bots, i) X(mp_smart_ai, b) X(unlock_outfits, b) X(settings_version, i)

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
	if (g_cfg.settings_version < 1) {   // PPSSPP's renderer was the default before the runtime's own
		if (g_cfg.renderer == 1) g_cfg.renderer = 2;
		g_cfg.settings_version = 1;
		if (existed) config_save();
	}
	// environment overrides for testing
	const char *e;
	if ((e = getenv("SR2_FPS"))) g_cfg.fps = atoi(e);
	if ((e = getenv("SR2_GPU"))) g_cfg.renderer = !strcmp(e, "soft") ? 0 : !strcmp(e, "native") ? 2 : !strcmp(e, "vulkan") || !strcmp(e, "vk") ? 3 : !strcmp(e, "gl") || !strcmp(e, "ppsspp") ? 1 : 2;
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
       HOOK_ENTITY_HANDLE, HOOK_VOICE_PICK, HOOK_MENU_GOTO,
       HOOK_BANTER_CHECK, HOOK_WORLD_RACER, HOOK_STORY_PROGRESS,
       HOOK_BOSS_INIT, HOOK_BOSS_HIT, HOOK_BOSS_MOST_HITS,
       HOOK_BT_START, HOOK_BT_RESTART, HOOK_BT_UPDATE, HOOK_BT_SCORE, HOOK_BT_PARAM, HOOK_BT_EVENT13, HOOK_BT_EVENT14,
       HOOK_BT_EVENT12, HOOK_BT_EVENT, HOOK_HUD_UPDATE, HOOK_CTC_BASE, HOOK_STREAM_SECTION, HOOK_STREAM_BUSY, HOOK_SOUND_PLAY_AT, HOOK_RACER_OPPONENT, HOOK_RACER_PACE, HOOK_STREAM_WANT_CLEAR, HOOK_STREAM_WANT_PICK, HOOK_OUTFIT_OWNED, HOOK_ALPHA_DRAW, HOOK_ENTITY_DT, HOOK_SIG_START, HOOK_SIG_END, HOOK_BAT_TARGET,
       HOOK_SEC_INIT, HOOK_SEC_RELEASE, HOOK_SEC_READ_DONE, HOOK_RACER_CORE, HOOK_RACER_PHYS,
       HOOK_RACER_STATE_STEP, HOOK_DEBUG_DL };
#define DISP_MGR 0x089A0BE4
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
enum { S_GRAPHICS, S_ROW0, S_SKIN = S_ROW0 + 5, S_WINS = S_SKIN + 4, S_BONUS, NSTR, STR_LEN = 48 };
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
static void bonus_menu(void);

static void mp_trace(void);
static void mp_menu_vblank(void);
// The same runtime also runs Sonic Rivals (1). Everything in this file is for Rivals 2's code.
bool game_is_sr2(void) {
	static int v = -1;
	if (v < 0) v = !memcmp(PTR(0x08800000 + 0x187774), "SG_MEMORY_ZONE", 14);
	return v;
}
void game_vblank(void) {
	static int last_fps;
	if (!game_is_sr2()) return;
	if (g_cfg.fps != last_fps) { last_fps = g_cfg.fps; apply_frame_rate(); }
	text_patch();
	options_patch();
	options_input();
	bonus_menu();
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

// Stages converted from Sonic Rivals 1 (tools: the world header carries 'R1ST' at +0x57f0). Rivals 1 puts a
// racer on whichever route it lands on; Rivals 2 keeps it on its chain of path records, so after a spring up to
// a route laid right above another the racer stays on the lower one and stops at its end. When a racer has stood
// well above or below its current path segment for a moment, look up the nearest segment again (F_9b2a8, as at
// spawn). The locator is racer +0x114: +0x8 next record, +0xa current record, +0xc slot.
#define R1_STAGE_MAGIC 0x54535231
static int resnap_wait[8];
static void racer_place_at(Ctx *c, u32 r, const float *t, u32 like);
// A racer falling well below its route on a Rivals 1 stage has dropped through a gap in the converted collision
// (or into a pit, where the game would put it back too): put it back on the route, on the record of a loaded
// section that is nearest across the ground and above it.
static void path_catch(Ctx *c, u32 racer, const float *pos) {
	float best = 1e9f, at[4] = {0, 0, 0, 1};
	for (int s = 0; s < 16; s++) {
		Ctx save = *c;
		c->r[4] = WORLD; c->r[5] = s; rt_call(c, 0x08800000 + 0x160724);
		u32 on = c->r[2]; *c = save;
		if (!on) continue;
		u32 arr = LW(WORLD_SLOT(s) + 0x444), n = LW(WORLD_SLOT(s) + 0x448) / 2;
		if (arr < 0x08800000 || arr >= 0x0A000000) continue;
		for (u32 k = 0; k < n; k++) {
			u32 rec = LW(arr + 4 * k);
			if (rec < 0x08800000 || rec >= 0x0A000000) break;
			float p0[3], p1[3];
			memcpy(p0, PTR(rec), sizeof p0); memcpy(p1, PTR(rec + 0x10), sizeof p1);
			float dx = p1[0] - p0[0], dy = p1[1] - p0[1], l2 = dx * dx + dy * dy;
			if (l2 < 0.01f) continue;
			float t = ((pos[0] - p0[0]) * dx + (pos[1] - p0[1]) * dy) / l2;
			t = t < 0 ? 0 : t > 1 ? 1 : t;
			float q[3] = { p0[0] + t * dx, p0[1] + t * dy, p0[2] + t * (p1[2] - p0[2]) };
			if (q[2] < pos[2]) continue;
			float h = hypotf(q[0] - pos[0], q[1] - pos[1]) * 4 + (q[2] - pos[2]) * 0.1f;
			if (h < best) { best = h; at[0] = q[0]; at[1] = q[1]; at[2] = q[2] + 1.5f; }
		}
	}
	if (best >= 1e9f) return;
	racer_place_at(c, racer, at, racer);
	float zero[3] = {0, 0, 0};
	memcpy(PTR(racer + RACER_VEL), zero, sizeof zero);
	LOGI("catch: racer %08x fell to %.1f, back on the route at %.1f %.1f %.1f", racer, pos[2], at[0], at[1], at[2]);
}
// Rivals 2 stages carry the race-end screens in their own section 0: five objects (world +0x4fc4, the results
// panel first) that the goal opens. Rivals 1 stages have none, so a Rivals 1 stage borrows them from a Rivals 2
// section 0 image (Blue Coast 1 as initialised in a race, r2sec0.bin in a mod folder): it is
// loaded above the extra racers, its pointers into itself moved along with it. Without the file the stage gets
// do-nothing objects (zeroed, every virtual function a bare return, F_064f8) and the race ends without a results screen.
#define R2_SEC0      0x0922E350
#define R2_SEC0_SIZE 0x390000
#define R2_UI_AT     0x0AC00000   // above the third extra racer's block (0x0A000000 + 3 * 4 MB)
#define R1_EVENT_STUBS 0x0A1F8000   // free: between PAD_DEVN and the first extra racer's heap
int io_mod_count(void);
const char *io_mod_root(int i);
static bool r2_ui_load(void) {
	char path[1024];
	FILE *f = NULL;
	for (int i = io_mod_count() - 1; i >= 0 && !f; i--) {   // the highest-priority mod that has one
		snprintf(path, sizeof path, "%s/r2sec0.bin", io_mod_root(i));
		f = fopen(path, "rb");
	}
	if (!f) return false;
	size_t n = fread(PTR(R2_UI_AT), 1, R2_SEC0_SIZE, f);
	fclose(f);
	if (n != R2_SEC0_SIZE) return false;
	u32 delta = R2_UI_AT - R2_SEC0, moved = 0;
	for (u32 a = R2_UI_AT; a < R2_UI_AT + R2_SEC0_SIZE; a += 4) {
		u32 v = LW(a);
		if (v >= R2_SEC0 && v < R2_SEC0 + R2_SEC0_SIZE) { SW(a, v + delta); moved++; }
	}
	SW(WORLD + 0x4fc4, 0x094e20c0 + delta);
	LOGI("r1 stage: race-end screens from %s (%u pointers moved)", path, moved);
	return true;
}
static void r1_event_stubs(void) {
	if (LW(WORLD + 0x57f0) != R1_STAGE_MAGIC || LW(WORLD + 0x4fc4)) return;
	if (r2_ui_load()) return;
	u32 vt = R1_EVENT_STUBS + 0x20, obj = R1_EVENT_STUBS + 0x100;
	memset(PTR(R1_EVENT_STUBS), 0, 0x700);
	for (int i = 0; i < 32; i++) SW(vt + 4 * i, 0x08800000 + 0x064f8);
	for (int i = 0; i < 5; i++) { SW(R1_EVENT_STUBS + 4 * i, obj + 0x100 * i); SW(obj + 0x100 * i, vt); }
	SW(WORLD + 0x4fc4, R1_EVENT_STUBS);
	LOGI("r1 stage: event object stubs at %08x", R1_EVENT_STUBS);
}
// Rivals 1 boost logs: Rivals 2 starts a boost from two trigger boxes on the boost object (shapes named a9370d8c
// and 137e039f in its collision group bc8617b0), which Rivals 1's logs don't have. A racer inside where those boxes
// would be gets the calls the collision step makes for them: the object's touch (vt[45], F_e09e8) and its trigger
// (F_aef40 -> vt[46], F_e0814) with a hit record naming the box.
#define BOOST_VT   0x089988a0
#define MAX_LOGS   64
static u32 logs[MAX_LOGS], log_hit;
static int nlogs = -1;
static void boost_logs_find(void) {
	nlogs = 0;
	for (u32 a = 0x09000000; a < 0x0A000000 && nlogs < MAX_LOGS; a += 16) {
		if (LW(a) != BOOST_VT) continue;
		u32 res = LW(a + 0x84);
		if (res - 0x08800000u < 0x01800000u && LW(res + 4) == 0x52e5d5e2) logs[nlogs++] = a;
	}
	if (!log_hit) log_hit = kalloc(0x100);
	static int said = -1;
	if (nlogs != said) LOGI("r1 stage: %d boost logs", said = nlogs);
}
static void boost_logs(Ctx *c, u32 racer) {
	if (LW(WORLD + 0x57f0) != R1_STAGE_MAGIC) { nlogs = -1; return; }
	static u64 found_at;   // sections stream in and out during the race: look again every second
	if (nlogs < 0 || g_vblank_count - found_at >= 60 || (nlogs && LW(logs[0]) != BOOST_VT)) { boost_logs_find(); found_at = g_vblank_count; }
	u32 body = LW(racer + RACER_BODY);
	float pos[3];
	memcpy(pos, PTR(body + 0x20), sizeof pos);
	for (int i = 0; i < nlogs; i++) {
		u32 o = logs[i];
		float p[3], a;
		memcpy(p, PTR(o + 0xb0), sizeof p); memcpy(&a, PTR(o + 0xc8), 4);
		float dx = pos[0] - p[0], dy = pos[1] - p[1], dz = pos[2] - p[2];
		float lx = dx * cosf(a) + dy * sinf(a), ly = -dx * sinf(a) + dy * cosf(a);   // its local frame: +y along the log
		if (fabsf(lx) > 1.0f || dz < -1.0f || dz > 2.5f || ly < -2.5f || ly > 3.0f) continue;
		Ctx save = *c;
		c->r[4] = o; c->r[5] = racer; c->r[6] = log_hit; rt_call(c, LW(LW(o) + 0xb4));
		*c = save;
		// the boxes: a9370d8c from 2 before to 1 past the object, 137e039f from 1.5 to 2.5 past it
		u32 shape = log_hit + 0x88;
		memset(PTR(log_hit), 0, 0x100);
		SW(log_hit + 0x78, o); SW(log_hit + 0x84, shape);
		SW(shape + 0x40, ly < 1.5f ? 0xa9370d8c : 0x137e039f);
		c->r[4] = racer; c->r[5] = log_hit; rt_call(c, 0x08800000 + 0xaef40);
		*c = save;
		static int dbg = -1;
		if (dbg < 0) dbg = getenv("SR2_LOG_DEBUG") != NULL;
		if (dbg) LOGI("log %08x racer %08x l %.2f %.2f %.2f: 65c %d 654 %d 8e4 %08x 230 %d", o, racer, lx, ly, dz, *PTR(racer + 0x65c), LW(racer + 0x654), LW(racer + 0x8e4), LW(racer + 0x230));
	}
}
static void path_resnap(Ctx *c, u32 racer, int i) {
	if (LW(WORLD + 0x57f0) != R1_STAGE_MAGIC) return;
	u32 loc = racer + 0x114, body = LW(racer + RACER_BODY);
	int slot = (s8)*PTR(loc + 0xd), cur = (s16)LH(loc + 0xa);   // the current record (+0xa) and its slot (+0xd); +8/+0xc are the next one
	if (slot < 0 || slot > 15 || cur < 0) return;
	u32 arr = LW(WORLD_SLOT(slot) + 0x444), n = LW(WORLD_SLOT(slot) + 0x448);
	if (arr < 0x08800000 || arr >= 0x0A000000 || (u32)cur >= n) return;
	u32 rec = LW(arr + 4 * cur);
	if (rec < 0x08800000 || rec >= 0x0A000000) return;
	float p0[3], p1[3], pos[3], vel[3];
	memcpy(p0, PTR(rec), sizeof p0); memcpy(p1, PTR(rec + 0x10), sizeof p1);
	memcpy(pos, PTR(body + 0x20), sizeof pos); memcpy(vel, PTR(racer + RACER_VEL), sizeof vel);
	float dx = p1[0] - p0[0], dy = p1[1] - p0[1], l2 = dx * dx + dy * dy;
	float t = l2 > 1e-6f ? ((pos[0] - p0[0]) * dx + (pos[1] - p0[1]) * dy) / l2 : 0;
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	float dz = pos[2] - (p0[2] + t * (p1[2] - p0[2]));
	if (dz < -8.0f && vel[2] < 0) { path_catch(c, racer, pos); resnap_wait[i] = 0; return; }
	// standing well off its route (a spring or a drop put it on another one), or falling past the end of a
	// dead-end route onto the one below it
	bool standing = fabsf(dz) >= 2.5f && fabsf(vel[2]) <= 0.05f, dropping = dz < -2.5f && vel[2] < -0.05f;
	if (!standing && !dropping) { resnap_wait[i] = 0; return; }
	if (++resnap_wait[i] < (standing ? 15 : 6)) return;
	resnap_wait[i] = 0;
	Ctx save = *c;
	c->r[4] = loc; c->r[5] = body + 0x20; c->r[6] = 0; c->r[7] = 5;
	rt_call(c, 0x08800000 + 0x9b2a8);
	*c = save;
	LOGI("resnap: racer %08x %.1f off its path, now slot %d record %d", racer, dz, (s8)*PTR(loc + 0xd), (s16)LH(loc + 0xa));
}
#define FILE_SLOT_HDR 0x5430   // the slot header saved inside a racer file
#define MAX_EXTRA 3
static int n_extra;                                         // extra racers built for this race
static u32 extra_base, extra_racer[MAX_EXTRA], extra_pending[MAX_EXTRA];   // and whether each waits to be switched on
static int extra_char[MAX_EXTRA] = {-1, -1, -1};   // index into mp_char_names (-1: a racer file given by SR2_EXTRA_RACER)
static bool extra_seated[MAX_EXTRA];   // player 2 drives the race's own rival (Single Event): seated, not one of ours
static int voice_trace_on = -1;   // test aid (SR2_VOICE_TRACE): log voice lines and the sound slot checks for players 2-4
#define voice_trace (voice_trace_on < 0 ? (voice_trace_on = getenv("SR2_VOICE_TRACE") != NULL) : voice_trace_on)
static bool extra_cpu[MAX_EXTRA];   // a computer player (a bot seat on the Multiplayer screen): the game's own driver steers it
bool mp_active(void);   // players 2-4 join this race (the Multiplayer screen, see below)
static bool race_ending;   // with extra racers: everyone is in, the game's finish runs (see placings)
u32 racergen(const char *ch, int slot, u32 base, u32 heap, u8 *out, int *unresolved);
static bool slot_free(int k) { return !LW(WORLD_SLOT(k) + 0xd0) && !LW(WORLD_SLOT(k) + 0x438); }
static bool extra_racer_prepare(int k, int player) {   // the k-th racer file we add, for player (1..3) + 1
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
		if (!mp_active() || player + 1 > g_cfg.mp_players || player > MAX_EXTRA) return false;
		static int used_slot[MAX_EXTRA];
		int slot = k ? used_slot[k - 1] - 1 : 13;
		while (slot > 0 && !slot_free(slot)) slot--;
		if (slot <= 0) { LOGE("extra racer %d: no free load slot in this level", k + 1); return false; }
		used_slot[k] = slot;
		int ci = player == 1 ? g_cfg.mp_char2 : player == 2 ? g_cfg.mp_char3 : g_cfg.mp_char4;
		u32 base = 0x0A000000 + k * 0x400000, heap = base + 0x200000;
		u32 hs = racergen(mp_char_files[ci], slot, base, heap, buf, NULL);
		if (!hs) return false;
		memset(PTR(heap), 0, 0x200000);
		n = 0x166670;
		extra_char[player - 1] = ci;
		LOGI("extra racer %d: %s, slot %d (player %d)", k + 1, mp_char_names[ci], slot, player + 1);
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
static volatile int bosshit_k = -1;   // test aid ("bosshit K"): player K+1 hits the boss
void game_request_bosshit(int k) { bosshit_k = k; }
#define BTCALL_STATE 0x0A1FF000   // free, beside PAD_DEVN
static volatile int btcall_k = -1, btcall_n; static volatile u32 btcall_fn, btcall_a2, btcall_a3;   // test aid ("btcall K FN A2 A3 N"):
void game_request_btcall(int k, u32 fn, u32 a2, u32 a3, int n) { btcall_fn = fn; btcall_a2 = a2; btcall_a3 = a3; btcall_n = n; btcall_k = k; }   // a battle dispatcher for player K+1, N frames
static void warp_pending(Ctx *c) {
	extern u32 boss_obj_get(void);
	if (btcall_k >= 0) {
		int k = btcall_k; if (--btcall_n <= 0) btcall_k = -1;
		u32 r = k ? (k <= n_extra ? extra_racer[k - 1] : 0) : LW(WORLD + 0x5024);
		Ctx save = *c;
		if (r && btcall_fn == 0x16c388) {   // "btcall K 16c388 HASH": send the event HASH to the racer, from itself
			c->r[4] = LW(0x089A39EC); c->r[5] = r; c->r[6] = r; c->r[7] = btcall_a2; c->r[8] = c->r[9] = 0; c->r[10] = 1;
			rt_call(c, 0x08800000 + btcall_fn);
		} else if (r && btcall_fn == 0x93fb8) {   // "btcall K 93fb8": the racer's pit death starts (its state: +0x10 the racer)
			SW(BTCALL_STATE + 0x10, r); c->r[4] = BTCALL_STATE;
			rt_call(c, 0x08800000 + btcall_fn);
		} else if (r) { c->r[4] = 0x08A8561C; c->r[5] = r; c->r[6] = btcall_a2; c->r[7] = btcall_a3; rt_call(c, 0x08800000 + btcall_fn); }
		*c = save;
	}
	if (bosshit_k >= 0 && boss_obj_get()) {
		int k = bosshit_k; bosshit_k = -1;
		u32 r = k ? (k <= n_extra ? extra_racer[k - 1] : 0) : LW(WORLD + 0x5024);
		Ctx save = *c;
		if (r) { c->r[4] = boss_obj_get(); c->r[5] = r; rt_call(c, 0x08800000 + 0xc0164); }
		*c = save;
	}
	int k = warp_k; if (k < 0) return;
	warp_k = -1;
	u32 r = k ? (k <= n_extra ? extra_racer[k - 1] : 0) : LW(WORLD + 0x5024);
	if (!r) return;
	if (warp_dist < 0 && k) { extra_racer_place(c, r, LW(WORLD + 0x5024), k - 1); LOGI("warp: player %d beside player 1", k + 1); return; }
	for (int s = 0; s < 16; s++) {
		u32 arr = LW(WORLD_SLOT(s) + 0x444);
		if (arr < 0x08800000 || arr >= 0x0C000000) continue;
		for (int i = 0; i < 4096; i++) {
			u32 n = LW(arr + i * 4);
			if (n < 0x08800000 || n >= 0x0C000000) break;
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

static bool banter_on;   // the racers' shots before the start (whole screen)
static bool boss_mp(void);   // a boss with players 2-4 (see the bosses below)
static int boss_player_hits(int k);
static bool bt_mp(void);   // a battle room with players 2-4 (see the battles below)
static float bt_score(int p);
// The views: player 1 and every player 2-4 who is not a computer player (bots get no part of the screen)
static int view_player[1 + MAX_EXTRA];
static int n_views(void) {
	int n = 0;
	view_player[n++] = 0;
	for (int k = 0; k < n_extra; k++) if (!extra_cpu[k] || ENV_ON("SR2_BOT_VIEWS")) view_player[n++] = k + 1;   // (test aid: theirs too)
	return n;
}
static bool split_view_on(void) {
	if (split_view < 0) {
		const char *e = getenv("SR2_SPLIT_VIEW");
		split_view = !e ? 1 : !strcmp(e, "0") ? 0 : !strcmp(e, "lr") ? 2 : 1;
	}
	return split_view && n_extra && n_views() > 1 && !extra_pending[n_extra - 1] && LW(CAM_MAIN) && !race_ending   // the results: whole screen
	       && !banter_on;   // so is each racer's shot before the start
}

static void swap_bytes(u8 *a, u8 *b, u32 n) { for (u32 i = 0; i < n; i++) { u8 t = a[i]; a[i] = b[i]; b[i] = t; } }

// The camera zones of the live course sections (a list at slot +0x45c, count at +0x460) remember whether the
// camera is inside them (zone +0x60), for the blend into a zone's framing; each camera keeps its own flags, or
// every camera keeps "entering" the zones afresh and none settles into the side view.
#define ZONE_IN    0x60
#define SLOT_COUNT (WORLD + 0x4d10)
#define MAX_ZONES  128
static bool section_live(int k);
static struct { u32 zone; u8 in; } cam_zone[MAX_EXTRA][MAX_ZONES];
static int cam_nzone[MAX_EXTRA];
static bool cam_in[MAX_EXTRA];
static void zones_swap(int k) {
	int n = 0, count = LW(SLOT_COUNT);
	static u32 zl[MAX_ZONES]; static u8 old[MAX_ZONES];
	for (int s = 0; s < count && s < 16; s++) {
		if (!(s == 0 || s >= 14 || section_live(s) || (s == 1 && LW(0x08A80000 + 0x5860) == 1))) continue;
		u32 list = LW(WORLD_SLOT(s) + 0x45c), nz = LB(WORLD_SLOT(s) + 0x460);
		if (list < 0x08800000 || list >= 0x0C000000 || nz > 32) continue;
		for (u32 i = 0; i < nz && n < MAX_ZONES; i++) {
			u32 z = LW(list + i * 4);
			if (z >= 0x08800000 && z < 0x0C000000) zl[n++] = z;
		}
	}
	if (!cam_in[k]) {   // in: the flags this camera left (out of a zone it has not seen yet)
		for (int i = 0; i < n; i++) {
			old[i] = 0;
			for (int j = 0; j < cam_nzone[k]; j++) if (cam_zone[k][j].zone == zl[i]) { old[i] = cam_zone[k][j].in; break; }
		}
		for (int i = 0; i < n; i++) { cam_zone[k][i].zone = zl[i]; cam_zone[k][i].in = LB(zl[i] + ZONE_IN); SB(zl[i] + ZONE_IN, old[i]); }
		cam_nzone[k] = n;
	} else {            // out: keep this camera's flags, put back the first camera's
		for (int i = 0; i < cam_nzone[k]; i++) {
			u32 z = cam_zone[k][i].zone; u8 mine = LB(z + ZONE_IN);
			SB(z + ZONE_IN, cam_zone[k][i].in); cam_zone[k][i].in = mine;
		}
	}
	cam_in[k] = !cam_in[k];
}

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
	zones_swap(k);
}

// A view context: what the game keeps one of for "the" player's view, kept per player and swapped in as one unit
// while the game steps or draws for that player: the race camera and its transform node, the camera-zone flags
// (cam_swap) and the local player (world +0x5024), whose position, path and section the camera and the scenery
// go by. Player 1's context lives in the game; extra racer k's is kept here and is in between view_enter(k) and
// view_leave(k). SR2_VIEW_DIFF=1 (test aid) lists what else a camera step writes (see view_diff).
static int view_cur = -1;      // the extra racer whose context is in (-1: player 1's)
static u32 view_p1_local;      // player 1's local player meanwhile
static int sctx_use(int k);    // (and their course sections, see the section contexts)
static int view_sctx;
static void view_enter(int k) {
	if (k < 0) return;
	view_sctx = sctx_use(k + 1);
	cam_swap(k);
	view_p1_local = LW(WORLD + 0x5024);
	SW(WORLD + 0x5024, extra_racer[k]);
	view_cur = k;
}
static void view_leave(int k) {
	if (k < 0) return;
	SW(WORLD + 0x5024, view_p1_local);
	cam_swap(k);
	sctx_use(view_sctx);
	view_cur = -1;
}

// test aid: the words a camera step writes outside the view context (camera, node, zone flags, the stack), which
// would carry over from one player's view to the next. Logged once, a few seconds into a race with players 2-4.
#define DIFF_LO 0x08800000u
#define DIFF_HI 0x0A800000u
static void view_diff(Ctx *c, void (*orig)(Ctx *c), int k) {
	static u8 *snap; static int done;
	if (done >= 2) { if (k >= 0) view_enter(k); orig(c); if (k >= 0) view_leave(k); return; }
	if (!snap) snap = malloc(DIFF_HI - DIFF_LO);
	if (k >= 0) view_enter(k);
	memcpy(snap, PTR(DIFF_LO), DIFF_HI - DIFF_LO);
	u32 sp = c->r[29];
	orig(c);
	u32 cam = LW(CAM_MAIN), node = LW(cam + 4);
	u32 run_lo = 0, run_hi = 0; int runs = 0;
	for (u32 a = DIFF_LO; a < DIFF_HI; a += 4) {
		if (!memcmp(snap + (a - DIFF_LO), PTR(a), 4)) continue;
		if ((a >= cam && a < cam + CAM_SIZE) || (a >= node && a < node + CAM_NODE_SZ) || (a >= sp - 0x4000 && a < sp + 0x100)) continue;
		bool zone = false;
		for (int i = 0; i < cam_nzone[0] && !zone; i++) zone = (a & ~3u) == ((cam_zone[0][i].zone + ZONE_IN) & ~3u);
		if (zone) continue;
		if (run_hi && a <= run_hi + 16) { run_hi = a + 4; continue; }
		if (run_hi && runs++ < 200) LOGI("viewdiff: %s camera step writes %08x..%08x", k < 0 ? "player 1's" : "an extra", run_lo, run_hi);
		run_lo = a; run_hi = a + 4;
	}
	if (run_hi && runs++ < 200) LOGI("viewdiff: %s camera step writes %08x..%08x", k < 0 ? "player 1's" : "an extra", run_lo, run_hi);
	LOGI("viewdiff: %d runs (cam %08x node %08x)", runs, cam, node);
	if (k >= 0) view_leave(k);
	done++;
}

// test aid: the same for one of the race render's draw calls in an extra racer's view (the display list it writes
// shows up too, as one long run from the list pointer on)
static void draw_diff(Ctx *c, void (*orig)(Ctx *c)) {
	static u8 *snap;
	if (!snap) snap = malloc(DIFF_HI - DIFF_LO);
	memcpy(snap, PTR(DIFF_LO), DIFF_HI - DIFF_LO);
	u32 sp = c->r[29], list = LW(GE_LIST_PTR);
	orig(c);
	u32 run_lo = 0, run_hi = 0; int runs = 0;
	for (u32 a = DIFF_LO; a < DIFF_HI; a += 4) {
		if (!memcmp(snap + (a - DIFF_LO), PTR(a), 4)) continue;
		if (a >= sp - 0x8000 && a < sp + 0x100) continue;
		if (run_hi && a <= run_hi + 16) { run_hi = a + 4; continue; }
		if (run_hi && runs++ < 300) LOGI("drawdiff: %08x..%08x", run_lo, run_hi);
		run_lo = a; run_hi = a + 4;
	}
	if (run_hi && runs++ < 300) LOGI("drawdiff: %08x..%08x", run_lo, run_hi);
	LOGI("drawdiff: %d runs (list from %08x to %08x)", runs, list, LW(GE_LIST_PTR));
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
// on its own part), and the other views are drawn over the rest: the race's first frame sets the exposure
// from the screen's brightness, and a part left dark washes the colours out for the whole race (see
// docs/TECHNICAL.md).
static void ge_region_scaled(int k, bool whole) {
	int x0 = 0, y0 = 0, w = 480, h = 272;
	int nv = n_views();
	if (k >= 0 && nv >= 3) { w = 240; h = 136; x0 = (k & 1) * 240; y0 = (k >> 1) * 136; }
	else if (k >= 0) { if (split_view == 2) { w = 240; x0 = k * 240; } else { h = 136; y0 = k * 136; } }
	float px = 2048.0f - 240 + x0 + w / 2, py = 2048.0f - 136 + y0 + h / 2;   // the screen offset stays (2048-240, 2048-136)
	u32 bx, by; memcpy(&bx, &px, 4); memcpy(&by, &py, 4);
	ge_put(0x45000000 | bx >> 8);
	ge_put(0x46000000 | by >> 8);
	// quarters show the whole view at half size (viewport scale, XSCALE/YSCALE); halves show its middle
	float sx = 240, sy = -136;
	if (k >= 0 && (nv >= 3 || whole)) { sx = 120; sy = -68; }
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
		float p = boss_mp() ? boss_player_hits(k) : bt_mp() ? bt_score(k) : race_progress(c, k ? extra_racer[k - 1] : p1);
		if (p > best) { best = p; lead = k; }
	}
	return split_lead = lead;
}

// one draw call of the race render, once per view: the first camera's view, then each extra racer's (with
// that racer as the local player, whose section picks the scenery), then the first camera's state back
static int live_place[1 + MAX_EXTRA];   // races: each player's place now (the finished keep theirs)
static void live_places(Ctx *c, u32 p1) {
	float pr[1 + MAX_EXTRA];
	for (int k = 0; k <= n_extra; k++) pr[k] = race_progress(c, k ? extra_racer[k - 1] : p1);
	for (int k = 0; k <= n_extra; k++) {
		if (finish_place[k]) { live_place[k] = finish_place[k]; continue; }
		int n = n_finished + 1;
		for (int j = 0; j <= n_extra; j++) if (j != k && !finish_place[j] && (pr[j] > pr[k] || (pr[j] == pr[k] && j < k))) n++;
		live_place[k] = n;
	}
}
// With a framebuffer per view (OpenGL, ge_views_available): each view is drawn over the whole screen into its
// own framebuffer at the internal resolution (each draw pass: GE_VIEW_DRAW before the view's draws, GE_VIEW_END
// after the last), and after the frame's last pass all of them are scaled into their parts of the screen
// (GE_VIEW_COMPOSE); quarters show the whole view, halves its middle (as ge_region). The runtime's commands go
// in the display list (ge_gl.cpp's ViewOp), as the GE draws the list after the game has built it. Every part
// gets drawn, so the first view needn't fill the screen any more, and a quarter is the whole view at full
// resolution scaled down rather than drawn small.
#define GE_VIEW_DRAW(v)    (0xFA000001 | (v) << 8)
#define GE_VIEW_COMPOSE(n) (0xFA000002 | (n) << 8)
#define GE_VIEW_END        0xFA000003   // back to the game's framebuffer
static void views_layout(int nv) {
	int dst[16], src[16];
	for (int v = 0; v < nv; v++) {
		int *d = dst + 4 * v, *r = src + 4 * v;
		r[0] = 0; r[1] = 0; r[2] = 480; r[3] = 272;
		if (nv >= 3) { d[0] = (v & 1) * 240; d[1] = (v >> 1) * 136; d[2] = 240; d[3] = 136; }
		else if (split_view == 2) { d[0] = v * 240; d[1] = 0; d[2] = 240; d[3] = 272; r[0] = 120; r[2] = 240; }
		else { d[0] = 0; d[1] = v * 136; d[2] = 480; d[3] = 136; r[1] = 68; r[3] = 136; }
	}
	ge_views_layout(nv, dst, src);
}
static int views_pending;   // views drawn since the last compose (their number of parts)
static void views_compose(void) {
	if (!views_pending) return;
	views_layout(views_pending);
	ge_put(GE_VIEW_COMPOSE(views_pending));
	views_pending = 0;
}
// last: the frame's last draw pass (the see-through objects), after which the views go on the screen
static void split_draw_views(Ctx *c, void (*orig)(Ctx *c), bool last) {
	Ctx save = *c;
	bool own = ge_views_available();   // a framebuffer per view
	if (own) { ge_region(-1); ge_put(GE_VIEW_DRAW(0)); } else ge_region(0);
	orig(c);
	Ctx out = *c;
	*c = save;
	u32 p1 = LW(WORLD + 0x5024);
	int nv = n_views();
	if (!bt_mp() && !boss_mp()) live_places(c, p1);
	for (int v = 1; v < nv; v++) {
		int k = view_player[v] - 1;
		view_enter(k); view_refresh(c);
		ge_region(own ? -1 : v);
		if (own) ge_put(GE_VIEW_DRAW(v));
		static int ddiff = -1, dsteps;
		if (ddiff < 0) ddiff = ENV_ON("SR2_VIEW_DIFF");
		if (ddiff && ++dsteps > 2400 && dsteps < 2410) draw_diff(c, orig);
		else orig(c);
		*c = save;
		view_leave(k);
	}
	int parts = nv;
	if (nv == 3) {   // three views: the fourth quarter follows whoever leads the race
		int lead = split_leader(c, p1);
		view_enter(lead - 1); view_refresh(c);
		ge_region(own ? -1 : 3);
		if (own) ge_put(GE_VIEW_DRAW(3));
		orig(c);
		*c = save;
		view_leave(lead - 1);
		parts = 4;
	}
	if (own) {
		views_pending = parts;
		if (last) views_compose(); else ge_put(GE_VIEW_END);
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
#define TIME_MODE       0x08A86DCC   // the world's time mode (F_169828): 4 while Chaos Control slows everything but its user
#define TIME_CHAOS      4
#define RACER_CHAOS     0x74c        // racer: byte set on Chaos Control's user, whose time is not slowed
#define OUTFIT_MENU_RA  0x089455c8   // F_145124's call of F_1393f0: the Change Outfit list greys the outfits not unlocked
#define DEATH_FADE_RA   0x088971d0   // F_970a8 (a racer's pit death, each frame): the screen fades out and in when it is
                                     // the local player's (F_3731c(world, 1)); with players 2-4 nobody's does
#define HUD_PLACE       0xb00        // the place, bottom right
#define HUD_TAG_CLOCK0  0x4d0        // Tag: the other racer's (index 0) time as it, bottom right, and its head
#define HUD_TAG_HEAD0   0x630
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
static void title_restore(void);
int mp_player_char(int k);
static int boss_winner = -1;   // the bosses: who got the hits first (see below)
static bool hudv_ok;   // the battles: the views' HUDs are set up (see hud_merge)
static void placings_reset(u32 p1) { title_restore(); memset(finish_place, 0, sizeof finish_place); n_finished = 0; race_ending = false; p1_racer = p1; p1_char = head_char(); }
static float race_time(Ctx *c) {   // F_12e7ac(the HUD's timer): the race time now
	Ctx save = *c;
	c->r[4] = HUD_BASE + HUD_TIMER; rt_call(c, 0x08800000 + 0x12e7ac);
	float t = c->f[0];
	*c = save;
	return t;
}
// The results title ("Sonic Wins", "Sonic Loses") is player 1's: with players 2-4 it names whoever came first
// (with their player number when another player has the same character). Player 1's entries in the tables of
// those titles name the winner from the end of the race until it is left.
#define WINS_TABLE     (0x08800000 + 0x19c7d4)   // "<character> Wins" string ids, by the game's character order
#define LOSES_TABLE    (0x08800000 + 0x19c7f4)   // "<character> Loses"
static const int game_char_of[MP_CHARS] = {0, 1, 3, 2, 7, 5, 6, 4};   // mp_char_names order -> the game's
static int title_char = -1;   // player 1's (the game's order) while the tables are changed
static u32 title_was[2];
static int race_winner(void) {   // the player in first place, or -1
	if (getenv("SR2_TEST_WINNER")) return atoi(getenv("SR2_TEST_WINNER"));   // test aid
	if (boss_mp() && boss_winner >= 0) return boss_winner;
	for (int k = 0; k <= n_extra; k++) if (finish_place[k] == 1) return k;
	return -1;
}
static void title_restore(void) {
	if (title_char < 0) return;
	SW(WINS_TABLE + title_char * 4, title_was[0]); SW(LOSES_TABLE + title_char * 4, title_was[1]);
	title_char = -1;
}
static void title_set(void) {
	title_restore();
	int w = race_winner(), ch = w >= 0 ? mp_player_char(w) : -1, p1 = mp_player_char(0);
	if (ch < 0 || ch >= MP_CHARS || p1 < 0 || p1 >= MP_CHARS) return;
	int same = 0;
	for (int k = 0; k <= n_extra; k++) same += mp_player_char(k) == ch;
	u32 id = LW(WINS_TABLE + game_char_of[ch] * 4);
	if (same > 1 && str_table) { set_str(S_WINS, "P%d %s Wins", w + 1, mp_char_names[ch]); id = str_base + S_WINS; }
	title_char = game_char_of[p1];
	title_was[0] = LW(WINS_TABLE + title_char * 4); title_was[1] = LW(LOSES_TABLE + title_char * 4);
	SW(WINS_TABLE + title_char * 4, id); SW(LOSES_TABLE + title_char * 4, id);
	LOGI("results: P%d %s wins", w + 1, mp_char_names[ch]);
}

static void p1_finish(Ctx *c, void (*orig)(Ctx *c)) {   // the game's finish for player 1 (the results), with their own time
	title_set();
	race_ending = true;
	// the game's own finish is player 1's win, or (Single Event, someone else won) the rival's, so player 1 is not
	// given the win's unlocks
	c->r[4] = race_winner() > 0 && extra_seated[0] ? extra_racer[0] : p1_racer;
	orig(c);
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
int mp_live_place(int k) { return n_extra && p1_racer && in_race && !bt_mp() && !boss_mp() && k >= 0 && k <= n_extra ? live_place[k] : 0; }
int mp_placings(int *place, float *time, float (*view)[4], int *over) {
	if (!n_extra || !p1_racer || !in_race) return 0;
	int n = n_extra + 1;
	int nv = n_views();
	for (int k = 0; k < n; k++) {
		place[k] = finish_place[k]; time[k] = finish_time[k];
		int v = 0;
		while (v < nv && view_player[v] != k) v++;
		float x = 0, y = 0, w = 1, h = 1;
		if (v == nv) w = h = 0;   // a computer player: no view
		else if (nv >= 3) { w = h = 0.5f; x = (v & 1) * 0.5f; y = (v >> 1) * 0.5f; }
		else if (nv == 1) {}
		else if (split_view == 2) { w = 0.5f; x = v * 0.5f; }
		else { h = 0.5f; y = v * 0.5f; }
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
// The bosses (Act 4): the first to land the boss's number of hits (6) wins. The boss counts the local player's
// hits (+0x6d4) and the other racer's (+0x6e0, the rival: player 2); players 3 and 4 have counts of their own here.
// A hit of theirs is counted with them in the local player's place, and every hit is weighed against the best of
// everyone else, so the boss gets harder (and someone wins) as it would with two.
#define BOSS_P1       0x6d0   // the local player, its hits (byte), and a second count (word)
#define BOSS_P1_HITS  0x6d4
#define BOSS_P1_HITS2 0x6d8
#define BOSS_RIVAL    0x6dc
#define BOSS_RIVAL_HITS 0x6e0
#define BOSS_WON      0x7a4   // someone has the hits
static u32 boss_obj;
static bool stream4;   // a section can list three neighbours (stream_links): the loads want up to four
static int boss_hits[1 + MAX_EXTRA];
static int boss_player_hits(int k) {   // player k's hits now
	if (!boss_obj) return 0;
	if (k == 0) return PTR(boss_obj + BOSS_P1_HITS)[0];
	if (k <= n_extra && extra_seated[k - 1]) return PTR(boss_obj + BOSS_RIVAL_HITS)[0];
	return boss_hits[k];
}
static int boss_best_but(int skip) {   // the most hits of anyone but player skip, and who (into *who)
	int best = 0;
	for (int k = 0; k <= n_extra; k++) if (k != skip && boss_player_hits(k) > best) best = boss_player_hits(k);
	return best;
}
static bool boss_mp(void) { return boss_obj && n_extra && p1_racer; }
static void boss_hit(Ctx *c, void (*orig)(Ctx *c)) {   // F_c0164(boss, racer)
	u32 b = A(0), r = A(1);
	int k = player_index(r);
	if (b != boss_obj || !n_extra || k < 0) { orig(c); return; }
	u8 *p1h = PTR(b + BOSS_P1_HITS), *rh = PTR(b + BOSS_RIVAL_HITS);
	u8 p1h0 = *p1h, rh0 = *rh;
	u32 p10 = LW(b + BOSS_P1), p1w0 = LW(b + BOSS_P1_HITS2);
	bool rival = k > 0 && extra_seated[k - 1];
	int best = boss_best_but(k);
	if (k == 0) { *rh = best; orig(c); *rh = rh0; }
	else if (rival) { *p1h = best; orig(c); *p1h = p1h0; }
	else {
		SW(b + BOSS_P1, r); *p1h = boss_hits[k]; *rh = best;
		orig(c);
		boss_hits[k] = *p1h;
		SW(b + BOSS_P1, p10); *p1h = p1h0; SW(b + BOSS_P1_HITS2, p1w0); *rh = rh0;
	}
	LOGI("boss: P%d hits (%d)", k + 1, boss_player_hits(k));
	if (boss_winner < 0 && PTR(b + BOSS_WON)[0]) { boss_winner = k; LOGI("boss: P%d wins", k + 1); }
}

// each view's boss counters: its player's hits and head on the left, the best of the others' on the right
// (the HUD's update, F_129380, prints the two counters into these strings, "<hits> / <needed>")
#define BOSS_TEXT_P1    0x08AA9994
#define BOSS_TEXT_RIVAL 0x08AA99A0
#define BOSS_TEXT_LEN   12
static void boss_text(u32 at, int hits) {
	char *t = (char *)PTR(at), buf[BOSS_TEXT_LEN];
	const char *rest = memchr(t, '/', BOSS_TEXT_LEN) ? strchr(t, ' ') : NULL;
	if (!rest) return;
	snprintf(buf, sizeof buf, "%d%s", hits, rest);
	memcpy(t, buf, BOSS_TEXT_LEN);
}
static void boss_view(int k, bool on) {
	static u32 img[2]; static char txt[2][BOSS_TEXT_LEN];
	u32 i1 = HUD_BASE + HUD_METER_ICON + HUD_ICON_IMAGE, i2 = HUD_BASE + HUD_RIVAL_ICON + HUD_ICON_IMAGE;
	if (!on) { SW(i1, img[0]); SW(i2, img[1]); memcpy(PTR(BOSS_TEXT_P1), txt[0], BOSS_TEXT_LEN); memcpy(PTR(BOSS_TEXT_RIVAL), txt[1], BOSS_TEXT_LEN); return; }
	img[0] = LW(i1); img[1] = LW(i2);
	memcpy(txt[0], PTR(BOSS_TEXT_P1), BOSS_TEXT_LEN); memcpy(txt[1], PTR(BOSS_TEXT_RIVAL), BOSS_TEXT_LEN);
	boss_text(BOSS_TEXT_P1, boss_player_hits(k)); memset(PTR(BOSS_TEXT_RIVAL), 0, BOSS_TEXT_LEN);   // only its own count
	int ck = mp_player_char(k);
	u32 hk = ck >= 0 ? hud_texture(img[0], hud_heads[ck]) : 0;
	if (hk) SW(i1, hk);
}

// the end of a boss with players 2-4: the winner first, then the others by their hits
static void boss_placings(Ctx *c) {
	float t = race_time(c);
	int w = boss_winner, n = 0;
	for (int k = 0; k <= n_extra; k++) finish_place[k] = 0;
	if (w >= 0) { finish_place[w] = ++n; finish_time[w] = t; }
	for (int h = 6 * 4; h >= 0; h--)
		for (int k = 0; k <= n_extra; k++)
			if (!finish_place[k] && boss_player_hits(k) == h) { finish_place[k] = ++n; finish_time[k] = t; }
	n_finished = n;
}
u32 boss_obj_get(void) { return boss_obj; }
int mp_boss_hits(int k) { return boss_mp() ? boss_player_hits(k) : -1; }   // for the overlay: -1 not a boss

// The battle rooms (Act 2). The battle (BATTLE) has a rules object per mode, which keeps the score of two racers
// by index: the local player 1 and the other racer 0 (F_3731c), and is asked about a racer through the battle's
// dispatchers (F_812a0 ... F_81858: that racer's index, then the mode's method; anyone else gets -1). Each
// player's part of the rules object is kept here while they are not in an index ("seated"): the rival (player 2)
// is in index 0 and player 1 in index 1, and players 3 and 4 are seated in index 1 for the time the rules object
// handles them (their knock-outs, laps, Chao, time on the hill, tags). Whoever reaches the score first stays
// seated, so the game's own end of the battle is theirs. Each view has a HUD of its own, updated with its player
// in index 1 and the leader of the others in index 0 (HOOK_HUD_UPDATE), and the results rank everyone.
#define BATTLE      0x08A8561C   // +0 on, +4 the mode (1-6), +8 the rules object of each mode
#define BT_TIME     0x8          // rules: the time limit (float; Tag: the time as it that loses)
#define BT_TARGET   0xc          // the score that wins (int)
#define BT_RACERS   0x14         // the racers of index 0 and 1
#define BT_TIMED    0x5          // King of the Hill: to the time limit (not to a score)
#define BT_RULES_SIZE 0x70
#define TAG_IT      0x44         // Tag: the index that is it (-1: no one, or a player who is not seated)
#define TAG_CLOCK   0x4d0        // Tag: each index's time as it, in the HUD's clocks (elements, F_12e7ac)
#define KOTH_RATE   0x08992908   // King of the Hill: the hill time per frame's weight and second (F_821b8)
#define RACER_RING_COUNT 0x72c   // a racer's rings (Ring Battle's score; Knock Out's lives)
#define RACER_KO_OUT 0x74e
#define HUD_SIZE    0x3560
#define CTC_BASE_OWNER 0x1a8   // Capture the Chao: a base's racer (HOOK_CTC_BASE)
enum { BT_RING = 1, BT_KO, BT_KOTH, BT_CTC, BT_TAG, BT_LAPS };
typedef struct { u8 o0, o1, n; } BtField;   // a part of the rules object: its offset for index 0 and 1, its size
static const struct { BtField f[3]; int nf; bool fscore; const char *unit; } bt_modes[7] = {   // the first is the score
	[BT_RING] = {{{0}}, 0, false, "rings"},
	[BT_KO]   = {{{0x44, 0x48, 4}, {0x52, 0x53, 1}}, 2, false, "KOs"},
	[BT_KOTH] = {{{0x4c, 0x50, 4}, {0x48, 0x49, 1}, {0x54, 0x58, 4}}, 3, true, "pts"},   // points on the hill, on it, since the last update
	[BT_CTC]  = {{{0x4c, 0x50, 4}, {0x48, 0x49, 1}}, 2, false, "Chao"},   // Chao brought home, carrying one
	[BT_TAG]  = {{{0x48, 0x4c, 4}}, 1, true, "s it"},   // (the time as it, copied from the HUD's clocks)
	[BT_LAPS] = {{{0x48, 0x4c, 4}}, 1, false, "laps"},
};
static u8 bt_st[1 + MAX_EXTRA][16];   // each player's part, while not seated
static float bt_tag[1 + MAX_EXTRA];   // Tag: each player's time as it, while not seated
static int bt_s[2] = {1, 0};          // the players seated in index 0 and 1
static int bt_home = 0;               // the player seated in index 1 between events (player 1, or who has won)
static int bt_tag_x = -1;             // Tag: who is it when the index says no one
static u32 bt_rl;                     // the rules object these are for
static int bt_mode(void) { u32 m = LBU(BATTLE) ? LW(BATTLE + 4) : 0; return m >= 1 && m <= 6 && LW(BATTLE + 8 + 4 * m) ? m : 0; }
static u32 bt_rules(void) { int m = bt_mode(); return m ? LW(BATTLE + 8 + 4 * m) : 0; }
static bool bt_mp(void) { return n_extra && extra_seated[0] && p1_racer && in_race && bt_rl && bt_rules() == bt_rl; }
static u32 bt_racer(int p) { return p ? extra_racer[p - 1] : p1_racer; }
static void bt_slot_io(int idx, u8 *buf, bool put) {
	int m = bt_mode(), at = 0;
	for (int i = 0; i < bt_modes[m].nf; i++) {
		const BtField *f = &bt_modes[m].f[i];
		u8 *g = PTR(bt_rl + (idx ? f->o1 : f->o0));
		if (put) memcpy(g, buf + at, f->n); else memcpy(buf + at, g, f->n);
		at += f->n;
	}
}
static float bt_clock(int idx) {   // F_12e7ac: the clock's start minus what is left
	u32 e = HUD_BASE + TAG_CLOCK + idx * 0xb0; float a, b;
	memcpy(&a, PTR(e + 0x2c), 4); memcpy(&b, PTR(e + 0x30), 4);
	return a != 0 ? a - b : b;
}
static void bt_clock_set(int idx, float t) {
	u32 e = HUD_BASE + TAG_CLOCK + idx * 0xb0; float a, b;
	memcpy(&a, PTR(e + 0x2c), 4); b = a != 0 ? a - t : t;
	memcpy(PTR(e + 0x30), &b, 4);
}
static int tag_it(void) { s32 i = LW(bt_rl + TAG_IT); return i == 0 || i == 1 ? bt_s[i] : bt_tag_x; }
static void tag_it_set(int who) { bt_tag_x = who; SW(bt_rl + TAG_IT, who < 0 ? -1 : who == bt_s[1] ? 1 : who == bt_s[0] ? 0 : -1); }
// seat players n0 and n1 (clocks: and their Tag clocks)
static void bt_seat(int n0, int n1, bool clocks) {
	if (n0 == bt_s[0] && n1 == bt_s[1]) return;
	int it = bt_mode() == BT_TAG ? tag_it() : -1;
	for (int i = 0; i < 2; i++) { bt_slot_io(i, bt_st[bt_s[i]], false); if (clocks) bt_tag[bt_s[i]] = bt_clock(i); }
	bt_s[0] = n0; bt_s[1] = n1;
	for (int i = 0; i < 2; i++) {
		bt_slot_io(i, bt_st[bt_s[i]], true);
		if (clocks) bt_clock_set(i, bt_tag[bt_s[i]]);
		SW(bt_rl + BT_RACERS + 4 * i, bt_racer(bt_s[i]));
	}
	if (bt_mode() == BT_TAG) tag_it_set(it);
}
static void bt_get(int p, u8 *s) {
	for (int i = 0; i < 2; i++) if (bt_s[i] == p) { bt_slot_io(i, s, false); return; }
	memcpy(s, bt_st[p], 16);
}
static float bt_tag_time(int p) {
	for (int i = 0; i < 2; i++) if (bt_s[i] == p) return bt_clock(i);
	return bt_tag[p];
}
static float bt_score(int p) {   // player p's score now (Tag: minus their time as it)
	int m = bt_mode();
	if (m == BT_RING) return (s32)LW(bt_racer(p) + RACER_RING_COUNT);
	if (m == BT_TAG) return -bt_tag_time(p);
	u8 s[16]; bt_get(p, s);
	if (bt_modes[m].fscore) { float f; memcpy(&f, s, 4); return f; }
	s32 v; memcpy(&v, s, 4); return v;
}
static int bt_other(int k) {   // who is shown beside player k: the leader of the others (Tag: who is it, else the closest to losing)
	if (bt_mode() == BT_TAG) { int it = tag_it(); if (it >= 0 && it != k) return it; }
	int o = -1; float best = 0;
	for (int p = 0; p <= n_extra; p++) {
		if (p == k) continue;
		float s = bt_mode() == BT_TAG ? -bt_score(p) : bt_score(p);
		if (o < 0 || s > best) { best = s; o = p; }
	}
	return o;
}
static void bt_reset(void) {
	bt_rl = bt_rules();
	bt_s[0] = 1; bt_s[1] = 0; bt_home = 0; bt_tag_x = -1;
	memset(bt_st, 0, sizeof bt_st); memset(bt_tag, 0, sizeof bt_tag);
	hudv_ok = false;
	if (!bt_rl || !n_extra) return;
	u32 r1 = LW(bt_rl + BT_RACERS + 4);
	if (bt_mode() == BT_KO && r1)   // everyone's lives, as the rules object's init gives the two
		for (int k = 1; k < n_extra; k++) { SW(extra_racer[k] + RACER_RING_COUNT, LW(r1 + RACER_RING_COUNT)); SB(extra_racer[k] + RACER_KO_OUT, 0); }
	LOGI("battle: mode %d, %d players", bt_mode(), n_extra + 1);
}
static void bt_win(int p) {   // player p has the score: they stay seated, and the game ends the battle (theirs)
	if (p == bt_home || p == 1) return;
	LOGI("battle: P%d decides it", p + 1);
	bt_home = p; bt_seat(1, p, true);
}
static bool bt_reached(int p) {   // player p has the score to win (the modes played to a score)
	int m = bt_mode(); float s = bt_score(p), t = (s32)LW(bt_rl + BT_TARGET);
	if (m == BT_KO || m == BT_CTC || m == BT_LAPS) return s >= t;
	if (m == BT_KOTH) return !LBU(bt_rl + BT_TIMED) && s >= t;
	if (m == BT_TAG) { float lim; memcpy(&lim, PTR(bt_rl + BT_TIME), 4); return -s >= lim; }   // (to lose)
	return false;
}
static void bt_world_seat(u32 *save) {   // the local player and the other racer as F_3731c tells them: the seated two
	save[0] = LW(WORLD + 0x5024); save[1] = LW(WORLD + 0x502c); save[2] = LW(WORLD + 0x5030);
	SW(WORLD + 0x5024, bt_racer(bt_s[1])); SW(WORLD + 0x502c, bt_racer(bt_s[0])); SW(WORLD + 0x5030, bt_racer(bt_s[0]));
}
static void bt_world_back(const u32 *save) { SW(WORLD + 0x5024, save[0]); SW(WORLD + 0x502c, save[1]); SW(WORLD + 0x5030, save[2]); }
// a dispatcher, for racer A(1): a player who is not seated is, for the call (in index 1, or the rival's partner),
// and in Tag whoever is it is seated too
static void bt_dispatch(Ctx *c, void (*orig)(Ctx *c)) {
	int p = player_index(A(1));
	if (!bt_mp() || p < 0 || race_ending) { orig(c); return; }
	int it = bt_mode() == BT_TAG ? tag_it() : -1, n0 = bt_s[0], n1 = bt_s[1];
	if (p != n0 && p != n1) n1 = p;
	if (it >= 0 && it != n0 && it != n1) { if (p == n1) n0 = it; else n1 = it; }
	if (n0 == n1) { orig(c); return; }
	if (n0 == bt_s[0] && n1 == bt_s[1]) { orig(c); return; }
	int s0 = bt_s[0], s1 = bt_s[1]; u32 w[3];
	bt_seat(n0, n1, false); bt_world_seat(w);
	orig(c);
	bt_world_back(w); bt_seat(s0, s1, false);
	if (p >= 2 && bt_reached(p)) bt_win(p);
}
// the battle's frame: players 3 and 4's time on the hill (King of the Hill: only one on it gets the time) and
// time as it (Tag), before the rules object's update
static void bt_frame(Ctx *c) {
	int m = bt_mode(); float dt = c->f[12];
	if (m == BT_TAG) {
		int it = tag_it();
		if (it >= 2 && it != bt_s[0] && it != bt_s[1]) { bt_tag[it] += dt; if (bt_reached(it)) bt_win(it); }
	}
	if (m != BT_KOTH) return;
	float w[1 + MAX_EXTRA] = {0}; int n_in = 0, in = -1;
	for (int p = 0; p <= n_extra; p++) {
		u8 s[16]; bt_get(p, s); memcpy(&w[p], s + 5, 4);
		if (w[p] > 0) { n_in++; in = p; }
	}
	bool seated_in = (w[bt_s[0]] > 0) + (w[bt_s[1]] > 0) == 1;
	if (n_in == 1 && in != bt_s[0] && in != bt_s[1]) {
		u8 *s = bt_st[in]; float t, k; memcpy(&t, s, 4); memcpy(&k, PTR(KOTH_RATE), 4);
		t += w[in] * dt * k; memcpy(s, &t, 4);
		if (bt_reached(in)) bt_win(in);
	} else if (n_in > 1 && seated_in) {   // someone else is on the hill too: the seated one gets nothing either
		int i = w[bt_s[0]] > 0 ? 1 : 0; float tiny = 1e-6f;
		const BtField *on = &bt_modes[m].f[1], *wt = &bt_modes[m].f[2];
		SB(bt_rl + (i ? on->o1 : on->o0), 1); memcpy(PTR(bt_rl + (i ? wt->o1 : wt->o0)), &tiny, 4);
	}
	for (int p = 2; p <= n_extra; p++) if (p != bt_s[0] && p != bt_s[1]) {   // as the update does for the two
		u8 *s = bt_st[p]; if (w[p] <= 0) s[4] = 0;
		float z = 0; memcpy(s + 5, &z, 4);
	}
}
// the end: everyone by their score, the game's winner first of those level with them
static void bt_placings(Ctx *c, int w0) {
	float t = race_time(c), sc[1 + MAX_EXTRA];
	int n = 0;
	for (int k = 0; k <= n_extra; k++) { sc[k] = bt_score(k); finish_place[k] = 0; }
	while (n <= n_extra) {
		int b = -1;
		for (int k = 0; k <= n_extra; k++)
			if (!finish_place[k] && (b < 0 || sc[k] > sc[b] || (sc[k] == sc[b] && k == w0))) b = k;
		finish_place[b] = ++n; finish_time[b] = t;
	}
	n_finished = n;
	for (int k = 0; k <= n_extra; k++) LOGI("battle: P%d %g %s, place %d", k + 1, bt_mode() == BT_TAG ? -sc[k] : sc[k], bt_modes[bt_mode()].unit, finish_place[k]);
}
// for the overlay: player k's score as text (0: not a battle)
int mp_score_text(int k, char *out, int n) {
	if (!bt_mp() || k < 0 || k > n_extra) return 0;
	float s = bt_score(k); int m = bt_mode();
	if (m == BT_TAG) snprintf(out, n, "%.1f %s", -s, bt_modes[m].unit);
	else if (m == BT_KOTH) snprintf(out, n, "%.0f %s", s, bt_modes[m].unit);
	else snprintf(out, n, "%d %s", (int)s, bt_modes[m].unit);
	return 1;
}

// A HUD per view in the battles: each view's own copy (hudv), which its update runs on, as its player with the
// leader of the others beside them. What the game writes to the HUD itself (outside its update) goes to every
// copy, a word at a time (hud_merge).
static u8 hudv[1 + MAX_EXTRA][HUD_SIZE], hud_ref[HUD_SIZE];
static void hud_merge(void) {
	u32 *cur = (u32 *)PTR(HUD_BASE), *ref = (u32 *)hud_ref;
	if (!hudv_ok) {
		for (int k = 0; k <= MAX_EXTRA; k++) memcpy(hudv[k], cur, HUD_SIZE);
		memcpy(hud_ref, cur, HUD_SIZE); hudv_ok = true;
		return;
	}
	for (int i = 0; i < HUD_SIZE / 4; i++)
		if (cur[i] != ref[i]) { for (int k = 0; k <= MAX_EXTRA; k++) ((u32 *)hudv[k])[i] = cur[i]; ref[i] = cur[i]; }
}
// the heads beside the scores: index 1's and index 0's (the HUD sets them up for the two racers; its own
// tables of the heads, by the game's character order, normal and hit)
#define HUD_HEAD1      (0x6e0 + HUD_ICON_IMAGE)
#define HUD_HEAD0      (0x630 + HUD_ICON_IMAGE)
#define HUD_HEADS      0x32ac
#define HUD_HEADS_HIT  0x3330   // rows of 8
#define HUD_HEADS_HIT_ROWS 8
static void bt_head(u32 at, int p) {
	int ch = mp_player_char(p);
	if (ch < 0 || ch >= MP_CHARS) return;
	int want = game_char_of[ch];
	u32 img = LW(HUD_BASE + at);
	for (int i = 0; i < 8; i++) {
		if (img == LW(HUD_BASE + HUD_HEADS + 4 * i)) { SW(HUD_BASE + at, LW(HUD_BASE + HUD_HEADS + 4 * want)); return; }
		for (int r = 0; r < HUD_HEADS_HIT_ROWS; r++)
			if (img == LW(HUD_BASE + HUD_HEADS_HIT + r * 0x20 + 4 * i)) { SW(HUD_BASE + at, LW(HUD_BASE + HUD_HEADS_HIT + r * 0x20 + 4 * want)); return; }
	}
}
static void bt_heads(int k, int o) {
	bt_head(HUD_HEAD1, k); bt_head(HUD_HEAD0, o);
	bt_head(HUD_METER_ICON + HUD_ICON_IMAGE, k); bt_head(HUD_RIVAL_ICON + HUD_ICON_IMAGE, o);   // (Laps Race: on its meter)
}
// the rules object and the world as view k's HUD sees them (its update and its draw both read them): player k
// seated in index 1, the one beside them in index 0; returns that one (-1: nothing changed)
static u8 bt_view_rules[BT_RULES_SIZE]; static int bt_view_s[3]; static u32 bt_view_w[3]; static bool bt_view_on;
static int bt_view_begin(int k) {
	int o = bt_other(k);
	if (o < 0) return -1;
	memcpy(bt_view_rules, PTR(bt_rl), BT_RULES_SIZE);
	bt_view_s[0] = bt_s[0]; bt_view_s[1] = bt_s[1]; bt_view_s[2] = bt_tag_x;
	bt_seat(o, k, false);
	bt_world_seat(bt_view_w);
	bt_view_on = true;
	return o;
}
static void bt_view_end(void) {
	if (!bt_view_on) return;
	bt_world_back(bt_view_w);
	memcpy(PTR(bt_rl), bt_view_rules, BT_RULES_SIZE); bt_s[0] = bt_view_s[0]; bt_s[1] = bt_view_s[1]; bt_tag_x = bt_view_s[2];
	bt_view_on = false;
}
static void bt_hud_update(Ctx *c, void (*orig)(Ctx *c)) {
	Ctx save = *c;
	hud_merge();
	orig(c);
	memcpy(hud_ref, PTR(HUD_BASE), HUD_SIZE);
	float tt[1 + MAX_EXTRA];
	for (int p = 0; p <= n_extra; p++) tt[p] = bt_tag_time(p);
	for (int k = 0; k <= n_extra; k++) {
		memcpy(PTR(HUD_BASE), hudv[k], HUD_SIZE);
		int o = bt_view_begin(k);
		if (o >= 0 && bt_mode() == BT_TAG) { bt_clock_set(0, tt[o]); bt_clock_set(1, tt[k]); }
		*c = save; orig(c);
		if (o >= 0) bt_heads(k, o);
		bt_view_end();
		memcpy(hudv[k], PTR(HUD_BASE), HUD_SIZE);
	}
	memcpy(PTR(HUD_BASE), hud_ref, HUD_SIZE);
	*c = save;
}

int mp_split_lead(void) { return n_views() == 3 && p1_racer && in_race && split_view_on() ? split_lead : -1; }   // see split_leader
int mp_player_char(int k) { return k ? (k <= n_extra ? extra_char[k - 1] : -1) : p1_char; }

static bool sig_mp(void);
u32 rt_hookval(Ctx *c, u32 id, u32 v) {
	if (!game_is_sr2()) return v;
	switch (id) {
	case HOOK_BAT_TARGET: {   // a bat (s0) of another player than the local one goes for the local player: keep its own
		u32 bat = c->r[16], t = LW(bat + 0x2d0);
		return sig_mp() && player_index(t) >= 0 && t != LW(bat + 0x200) ? t : v;
	}
	case HOOK_STREAM_WANT_CLEAR: case HOOK_STREAM_WANT_PICK:   // slti, a0 < 3: 4 with the sections' third neighbour
		return stream4 ? (s32)c->r[4] < 4 : v;
	case HOOK_COPTER_LIFT_DT:                                // Tails' Tail Copter: lift time per flap
	case HOOK_SIG_START_DT: return at_least_30fps_dt(v);     // signature move start: lift time and kick
	}
	return v;
}
// ---------------------------------------------------------------- bonus stage on the stage select
// A mod with a bonus/ folder (io.c) gets its own row on Blue Coast's act list in Free Play: the list's Act 2 item,
// hidden there (+0xc flag 0x100, +0x240 clear), is shown with the bonus stage's name. Choosing it switches the bonus
// files on and chooses Act 1 (whose files they replace) instead; choosing another act, or the main or single player menu,
// switches them off.
#define ACT_LIST       0x096476F0   // the act list widget on Select Stage
#define ACT_LIST_HASH  0x56B567E2
#define ACT_ZONE_STR   0x096E3504   // string id of the zone name shown beside it
#define SCR_MAIN_MENU  0x4050d926
#define SCR_SINGLE_PL  0xa69d1022
#define SCR_SELECT     0x8898eb5a
const char *io_bonus_name(void);
void io_set_bonus(bool on);
static u32 menu_hash(void);
static void bonus_menu(void) {
	static u32 item, old_flags, old_str, old_shown, prev, held, seq;
	const char *name = io_bonus_name();
	if (!name || !str_table) return;
	u32 h = menu_hash();
	if (h == SCR_MAIN_MENU || h == SCR_SINGLE_PL) io_set_bonus(false);
	u32 zs = LW(ACT_ZONE_STR), t = LW(TEXT_TABLE);
	bool here = h == SCR_SELECT && LW(ACT_LIST + 4) == ACT_LIST_HASH && LW(ACT_LIST + W_NCHILD) == 4 &&
	            zs < LW(TEXT_COUNT) && !strcmp((const char *)PTR(LW(t + 4 * zs)), "Blue Coast Zone");
	u32 first = LW(ACT_LIST + W_CHILDREN), it = first + W_SIZE;
	if (here && !item && (LW(it + 0xc) & 0x100)) {   // the hidden Act 2 row becomes ours
		item = it; old_flags = LW(it + 0xc); old_str = LW(it + W_STRID); old_shown = LW(it + 0x240);
		set_str(S_BONUS, "%s", name);
		SW(it + W_STRID, str_base + S_BONUS);
		LOGI("stage select: %s row added", name);
	}
	if (item && !here) {
		SW(item + 0xc, old_flags); SW(item + W_STRID, old_str); SW(item + 0x240, old_shown); item = 0;
	}
	u32 cur = fe_buttons, pressed = cur & ~prev;
	prev = cur;
	held &= cur;
	if (item) {
		SW(item + 0xc, LW(item + 0xc) & ~0x100u); SW(item + 0x240, 0x00010100);   // shown
		if ((pressed & B_CROSS) && !seq) {
			u32 sel = LW(ACT_LIST + W_SELECTED);
			if (sel == item) { io_set_bonus(true); held |= B_CROSS; seq = 1; }
			else if (sel - first < 4 * W_SIZE) io_set_bonus(false);
		}
	}
	fe_buttons &= ~held;
	// the game starts the act by the row's place in the list: go up to Act 1 and choose it. Menus skip input
	// while they animate, so each press is checked against the list and repeated until it takes
	if (seq) {
		fe_buttons &= ~(B_UP | B_DOWN | B_CROSS);
		static u32 next, btn, at;   // a press is held 2 frames; the next comes when the frame count reaches next
		bool on_list = h == SCR_SELECT && LW(ACT_LIST + 4) == ACT_LIST_HASH;
		if (!on_list || ++seq > 300) seq = next = 0;   // the race is loading (or give up after five seconds)
		else if (seq < next) { if (seq - at < 2) fe_buttons |= btn; }
		else {
			btn = LW(ACT_LIST + W_SELECTED) != first ? B_UP : B_CROSS;   // CROSS again only if the screen stays
			fe_buttons |= btn; at = seq; next = seq + (btn == B_UP ? 8 : 30);
		}
	}
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
#define SCR_CHARS   0x17ed40a4   // Single Event: Select Character
#define SCR_RIVAL   0xc76ff34b   // Single Event: Select Rival
#define RIVAL_CHOICE 0x08AA9F9C  // the rival picked there (the game's character order)
#define BTN_Y       0x14         // a menu button's height on screen (larger is higher)
enum { B_START = 0x8, B_TRIANGLE = 0x1000, B_SQUARE = 0x8000, LOBBY_EMPTY = -100, LOBBY_BOT = -50 };   // seat ids besides controllers
int fe_pads(int *ids, u32 *buttons, const char **names, int max);
int fe_last_pad(void);
void fe_set_seats(const int *ids);

static bool mp_mode;
static struct { bool open; int pad[4], ch[4]; } lobby;   // pad: controller id (player 1: -1 keyboard only), or empty
static struct { int ids[16]; u32 prev[16]; int n; } lobby_pads;
static int lobby_last[4] = {LOBBY_EMPTY, LOBBY_EMPTY, LOBBY_EMPTY, LOBBY_EMPTY};   // the last race's players, seated again
const char *fe_pad_name(int id);
static enum { MAC_IDLE, MAC_SETTLE, MAC_UP, MAC_UP_WAIT, MAC_DOWN, MAC_DOWN_WAIT, MAC_CROSS, MAC_WAIT_STAGES, MAC_BACK, MAC_WAIT_MAIN,
              MAC_RIVAL } mac;
static u32 rival_from;   // the screen Select Rival was entered from
static int mac_t, mac_tries, mac_hold; static u32 mac_button, mac_sel; static float mac_y;

static u32 menu_screen(void) { return LW(MENUS + 0x9f4); }
static u32 menu_hash(void) { u32 s = menu_screen(); return s ? LW(s) : 0; }
bool mp_active(void) { return mp_mode || ENV_ON("SR2_MP_TEST"); }   // (looked up once: this runs in per-frame hooks)

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
		bool here = id != LOBBY_EMPTY && id != lobby.pad[0] && (id < -1 || fe_pad_name(id));   // bots and test inputs stay too
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
	g_cfg.mp_bots = 0;
	for (int k = 1; k < 4; k++)   // players in the order of their seats, gaps closed
		if (lobby.pad[k] != LOBBY_EMPTY) {
			ids[n] = lobby.pad[k]; *chs[n - 1] = lobby.ch[k];
			if (lobby.pad[k] == LOBBY_BOT) g_cfg.mp_bots |= 1 << (n + 1);   // bit = player number
			n++;
		}
	for (int k = 0; k < 4; k++) lobby_last[k] = k < n ? ids[k] : LOBBY_EMPTY;
	for (int k = 1; k < n; k++) if (ids[k] == LOBBY_BOT) ids[k] = -1;   // no controller
	g_cfg.mp_players = n;
	config_save();
	fe_set_seats(ids);
	mac = MAC_SETTLE; mac_t = 0; mac_tries = 0;
	LOGI("multiplayer: %d players, to Single Event", n);
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
	{   // player 1 seats computer players (Y / triangle; with the debug switch, test inputs -2..-4 instead) and
		// picks their characters (left / right: the last one added), X / square takes the last one out
		int last = 0;
		for (int k = 1; k < 4; k++) if (lobby.pad[k] < -1 && lobby.pad[k] != LOBBY_EMPTY) last = k;
		if (p1e & B_TRIANGLE) {
			int k = 1, t = -2;
			while (k < 4 && lobby.pad[k] != LOBBY_EMPTY) k++;
			if (!g_cfg.mp_test_seats) t = LOBBY_BOT;
			else for (int i = 1; i < 4; i++) if (lobby.pad[i] == t) { t--; i = 0; }   // the first test input not seated
			if (k < 4 && (t == LOBBY_BOT || t >= -4)) {
				lobby.pad[k] = t;
				if (t == LOBBY_BOT) {   // a character no one has yet
					for (int tries = 0; tries < MP_CHARS; tries++) {
						bool used = false;
						for (int i = 1; i < 4; i++) used |= i != k && lobby.pad[i] != LOBBY_EMPTY && lobby.ch[i] == lobby.ch[k];
						if (!used && lobby.ch[k] != 0) break;   // (player 1 is often Sonic)
						lobby.ch[k] = (lobby.ch[k] + 1) % MP_CHARS;
					}
				}
				LOGI("multiplayer: %s player %d added", t == LOBBY_BOT ? "computer" : "test", k + 1);
			}
		}
		if ((p1e & B_SQUARE) && last) lobby.pad[last] = LOBBY_EMPTY;
		if (last && (p1e & B_LEFT)) lobby.ch[last] = (lobby.ch[last] + MP_CHARS - 1) % MP_CHARS;
		if (last && (p1e & B_RIGHT)) lobby.ch[last] = (lobby.ch[last] + 1) % MP_CHARS;
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

// Single Event is the Single Player menu's second button: up from the top wraps to the last one (Free Play), so up
// is pressed until the chosen button moves down the screen, then down twice (wrapping to the top, then the second)
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
		if (y < mac_y) { mac = MAC_DOWN; mac_t = 0; mac_tries = 0; }
		else if (++mac_tries > 6) mac_abort("no Free Play button");
		else mac = MAC_UP;
		break;
	}
	case MAC_DOWN:
		if (mac_t < 6) break;
		mac_sel = LW(s + 0x20);
		mac_press(B_DOWN); mac = MAC_DOWN_WAIT; mac_t = 0;
		break;
	case MAC_DOWN_WAIT:
		if (h != SCR_SINGLE) { mac_abort("left the Single Player menu"); break; }
		if (LW(s + 0x20) == mac_sel) { if (mac_t > 30) mac = MAC_DOWN; break; }
		mac = ++mac_tries == 2 ? MAC_CROSS : MAC_DOWN; mac_t = 0;
		break;
	case MAC_CROSS:
		if (mac_t < 10) break;
		mp_mode = true;
		mac_press(B_CROSS); mac = MAC_WAIT_STAGES; mac_t = 0;
		break;
	case MAC_WAIT_STAGES:   // Single Event opens on Select Character
		if (h == SCR_CHARS) { mac = MAC_IDLE; LOGI("multiplayer: in Single Event"); }
		else if (mac_t > 600) mac_abort("no character select");
		break;
	case MAC_RIVAL:   // Select Rival is skipped: player 2 is the rival. Coming back from the stages goes on back.
		if (h != SCR_RIVAL) { if (mac_t > 600 || (mac_tries && h != SCR_RIVAL)) mac = MAC_IDLE; break; }
		if (mac_t < 20 || mac_t % 40) break;
		if (rival_from == SCR_STAGES) { mac_press(B_CIRCLE); mac_tries++; break; }
		if (mac_tries < 12) {   // the cursor (RIVAL_CHOICE) to player 2's character: the grid's rows hold 0-3 and 4-7
			int want = game_char_of[g_cfg.mp_char2], cur = LW(RIVAL_CHOICE);
			if (cur != want) { mac_press((cur >> 2) != (want >> 2) ? ((want >> 2) ? B_DOWN : B_UP) : B_RIGHT); mac_tries++; mac_t = 30; break; }
			mac_tries = 12;
		}
		if (mac_tries++ & 1) { mac_press(B_RIGHT); mac_t = 10; break; }   // still here: a greyed one (player 1's), try the next
		mac_press(B_CROSS);
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
	static u32 shown; static int tr = -1;
	if (tr < 0) tr = getenv("SR2_MENU_TRACE") != NULL;
	if (tr && menu_hash() != shown) { shown = menu_hash(); LOGI("menu screen %08x", shown); }   // test aid
	if (lobby.open) { lobby_input(fe_buttons); fe_buttons = 0; }
	else if (mac != MAC_IDLE) macro_vblank();
	if (!lobby.open && mac == MAC_IDLE && mp_mode) {   // back in the Single Player or Main Menu: the multiplayer Single Event is over
		u32 h = menu_hash();
		if (h == SCR_SINGLE || h == SCR_MAIN) { mp_mode = false; fe_set_seats(NULL); title_restore(); LOGI("multiplayer: ended"); }
	}
}


// Level streaming with players 2-4. A course is cut into sections (load slots 1..n, world +0x4d10) and only four
// are in memory at once: section k always goes into buffer (k - 1) & 3, so the game keeps the one behind the
// section it follows (world +0x5da0) and the two ahead of it. It follows player 1; a player more than a section
// behind or two ahead of them has no ground (their view goes empty and they fall). Here the window follows the
// leader (the section streaming, F_15f97c, and the loads, F_15e4a8, are given the leader instead of player 1), and
// whoever is still outside it for a moment is put back on the course at the rear of the window (or ahead of it, at
// the front), facing the way the leader runs. Races only: battles and bosses fit in their window.
#define STREAM_BUF_N   0x0899FF4C   // the number of section buffers (4)
#define STREAM_BUFS    (WORLD + 0x5d18)   // per buffer, 16 bytes: +4 the slot loaded (its number at +0x20), +8 state (0x20: live)
#define STREAM_CUR     (WORLD + 0x5da0)
#define STREAM_CUR_SEC (WORLD + 0x5d9c)   // its section (F_15e0e8's 4th argument), whose neighbours the loads (F_15e4a8) keep
#define STREAM_OUT_US  700000   // outside the window this long: put back on the course
static u32 stream_leader;   // the racer the streaming follows (0: player 1, the game's own)
static int stream_sec[1 + MAX_EXTRA];
static u64 stream_out_since[1 + MAX_EXTRA];
static bool sctx_on;   // a section context per player (below): no window to share
static int sctx_in;    // the player whose context is in the world (0: player 1)
static bool stream_mp(void) {
	return !sctx_on && n_extra && p1_racer && in_race && !race_ending && !banter_on && !bt_mp() && !boss_mp() && !extra_pending[n_extra - 1]
	       && !ENV_ON("SR2_STREAM_P1");   // test aid: the game's own streaming
}
static bool section_live(int k) {
	u32 nb = LW(STREAM_BUF_N);
	if (k < 1 || !nb || nb > 16) return false;
	u32 e = STREAM_BUFS + ((k - 1) & (nb - 1)) * 16, sp = LW(e + 4);
	return LW(e + 8) == 0x20 && sp >= 0x08800000 && sp < 0x0C000000 && (s32)LW(sp + 0x20) == k;
}
static u32 player_racer(int k) { return k ? (k <= n_extra ? extra_racer[k - 1] : 0) : p1_racer; }
static int racer_section(Ctx *c, u32 r) {   // F_15e0e8(world, racer, 0): the section a racer stands in, or -1
	Ctx save = *c;
	c->r[4] = WORLD; c->r[5] = r; c->r[6] = 0; c->r[7] = 0;
	rt_call(c, 0x08800000 + 0x15e0e8);
	s32 sec = c->r[2];
	*c = save;
	if (sec < 1 || sec > (s32)LW(WORLD + 0x4d10)) sec = r && LW(r + 4) ? (s32)LW(LW(r + 4) + 0xc) : -1;
	return sec;
}
static u64 mono_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (u64)ts.tv_sec * 1000000 + ts.tv_nsec / 1000; }
static u32 stream_pick(Ctx *c) {   // the leader: the player in the furthest section (ties keep the one before)
	int best = -1; u32 lead = 0;
	for (int k = 0; k <= n_extra; k++) {
		u32 r = player_racer(k);
		stream_sec[k] = r ? racer_section(c, r) : -1;
		if (!r || finish_place[k] || stream_sec[k] < 1) continue;
		if (stream_sec[k] > best || (stream_sec[k] == best && r == stream_leader)) { best = stream_sec[k]; lead = r; }
	}
	if (!lead) lead = p1_racer;
	if (lead != stream_leader && ENV_ON("SR2_MP_TRACE")) LOGI("stream: following player %d (section %d)", player_index(lead) + 1, best);
	return stream_leader = lead;
}
// The loads (F_15e4a8) keep the current section and those in its list of neighbours (section +0x24, up to three,
// <= 0 for none), and load the nearest missing one of the current section and its first two neighbours. A course
// lists two, the one behind and the one ahead, so only three of the four buffers are used. With players 2-4 the
// list becomes the one ahead, the one after it, then the one behind (kept, never loaded: it was current before), so
// all four hold a section. (Only the first three of the wanted list can be loaded: the fourth has no place in the
// list of active sections, WORLD +0x5d50, three entries.)
static void stream_links(bool on) {   // (off: back as the course has them, for the results and anything after)
	int n = (s32)LW(WORLD + 0x4d10);
	for (int k = 1; k <= n && k < 14; k++) {
		u32 o = WORLD_SLOT(k) + 0x24;
		s32 a = LW(o), b = LW(o + 4), x = LW(o + 8);
		if (on && a == k - 1 && b == k + 1 && x <= 0 && k + 2 <= n) {   // (as the course has it: behind, ahead)
			SW(o, k + 1); SW(o + 4, k + 2); SW(o + 8, k - 1 > 0 ? k - 1 : (u32)-1);
		} else if (!on && a == k + 1 && b == k + 2 && (x == k - 1 || (k == 1 && x == -1))) {
			SW(o, k - 1); SW(o + 4, k + 1); SW(o + 8, (u32)-1);
		}
	}
	if (on) stream4 = true;   // (stays: with two neighbours the fourth place is never filled)
}
// Course sections at addresses of their own. A level file (STREAMS/R2_ZONE_*.STR) starts with a table of its chunks
// (+4 + 8k: file offset, size; read into the world at WORLD): 0 the level, then the course sections. Each section k
// is a memory image made for the address the race load gives its load slot (F_158184 fills SLOT_BASES: slot 0 the
// level, then the four section buffers in turn), so sections k and k+4 are made for the same address and the game
// keeps at most four. Here sections 5 and up get addresses of their own above the PSP's memory (SEC_HOME), and every
// word that points into one is moved with it:
//   - inside the section, its pointers to its own data (plain words in [base, base + SEC_BUF_SIZE): 1,324 in Blue
//     Coast 1's first section), moved as it is read;
//   - outside it, in the world header (its slot header, and arrays indexed by slot further on) and the level image
//     (a block per slot at level +0x14 + 0x40 k, arrays indexed by slot, and the level's objects, each with 12
//     entries indexed by slot - 1), moved once when the level is read. A word into a buffer there belongs to
//     section k or k+4; which one is told by where it sits (sec_owner, the same rules as tools/secrefs.py).
// Words that only look like pointers (mesh data, which has no rule) stay as they are.
// This is done in multiplayer races; SR2_SECTIONS=1 does it in every race, SR2_SECTIONS=0 in none.
#define SLOT_BASES   0x08AAC6C0   // per load slot: where its data goes
#define SEC_BUF_SIZE 0x133340     // a section buffer (the distance between two)
#define LEVEL_CHUNKS (WORLD + 4)  // the level file's chunk table, as read into the world
#define LEVEL_BASE   0x0922E350   // where the level image goes (SLOT_BASES slot 0)
#define SEC_HOME(k)  (0x0B000000u + ((k) - 5) * 0x200000u)   // section k >= 5 (+ its buffer's low 16 bits)
#define MAX_SECTIONS 12
static int sec_n;                     // the level's sections (0: not a level, or moving them is off)
static u32 sec_buf[4];                // the four buffers, as the race load set them
static u32 sec_size[MAX_SECTIONS + 1];
static bool sections_own(void) {   // multiplayer races (SR2_SECTIONS=1 every race, 0 never)
	const char *e = getenv("SR2_SECTIONS");
	return e ? strcmp(e, "0") != 0 : mp_active();
}
static int sec_bufof(u32 v) {   // which buffer v points into (-1: none)
	for (int b = 0; b < 4; b++) if (v >= sec_buf[b] && v < sec_buf[b] + SEC_BUF_SIZE) return b;
	return -1;
}
u32 sec_addr(int k) { return k >= 5 && k <= sec_n ? SEC_HOME(k) + (sec_buf[(k - 1) & 3] & 0xFFFF) : 0; }
static int sec_chunk(const char *host, s64 at) {   // which chunk of the level file a read at `at` is (-1: none)
	if (!strstr(host, "R2_ZONE_") && !strstr(host, "r2_zone_")) return -1;
	for (int k = 0; k <= MAX_SECTIONS; k++) if (LW(LEVEL_CHUNKS + 8 * k) == (u32)at && LW(LEVEL_CHUNKS + 8 * k + 4)) return k;
	return -1;
}
static bool sec_in_window(u32 a) {   // a word of the world header or of the level image
	return (a >= WORLD && a < WORLD + 0x5800) || (a >= LEVEL_BASE && a < LEVEL_BASE + sec_size[0]);
}
// which section the word at a (outside the sections, pointing into a buffer) belongs to: 0 none we can tell
static int sec_owner(u32 a) {
	u32 v = LW(a); int b = sec_bufof(v);
	if (b < 0 || (v & 3)) return 0;
	if (a >= WORLD_SLOT(0) && a < WORLD_SLOT(16)) return (a - WORLD_SLOT(0)) / 0x4c0;   // a slot header
	if (a >= LEVEL_BASE + 0x54 && a < LEVEL_BASE + 0x14 + 0x40 * 16) return (a - LEVEL_BASE - 0x14) / 0x40;   // the level's per-slot blocks
	for (int k = b + 1; k <= sec_n; k += 4) {   // an array indexed by slot: entry 0 not in a buffer, most entries in theirs
		u32 s = a - 4 * k; int ok = 0;
		if (!sec_in_window(s) || sec_bufof(LW(s)) >= 0) continue;
		for (int j = 1; j <= sec_n; j++) if (sec_in_window(s + 4 * j) && sec_bufof(LW(s + 4 * j)) == ((j - 1) & 3)) ok++;
		if (ok >= (sec_n < 6 ? sec_n : 6)) return k;
	}
	int cand[3], nc = 0;   // only one section of that buffer is big enough
	for (int k = b + 1; k <= sec_n; k += 4) if (v - sec_buf[b] < sec_size[k] && nc < 3) cand[nc++] = k;
	if (nc == 1) return cand[0];
	int ok_k = 0, n_ok = 0;   // a level object's 12 entries, indexed by slot - 1: each zero or in its slot's buffer
	for (int i = 0; i < nc; i++) {
		u32 s = a - 4 * (cand[i] - 1); bool good = true;
		for (int j = 0; j < 12 && good; j++) {
			if (!sec_in_window(s + 4 * j)) { good = false; break; }
			u32 w = LW(s + 4 * j);
			if (w && sec_bufof(w) != (j & 3)) good = false;
		}
		if (good) { ok_k = cand[i]; n_ok++; }
	}
	return n_ok == 1 ? ok_k : 0;
}
static void sec_lights_reset(void);
static void sec_level_read(void) {   // the level image is in: the world header and it, pointing at sections 5 and up
	sec_n = 0;
	sctx_on = false; sctx_in = 0;
	sec_lights_reset();
	if (!sections_own()) return;
	for (int b = 0; b < 4; b++) sec_buf[b] = LW(SLOT_BASES + 4 * (b + 1));
	for (int k = 0; k <= MAX_SECTIONS; k++) sec_size[k] = LW(LEVEL_CHUNKS + 8 * k + 4);
	int n = 0;
	while (n < MAX_SECTIONS && sec_size[n + 1]) n++;
	if (n < 5 || LW(SLOT_BASES) != LEVEL_BASE) { LOGI("sections: %d, all in the game's buffers", n); return; }
	for (int b = 0; b < 4; b++) if (sec_buf[b] != LW(SLOT_BASES + 4 * (b + 5)) && b + 5 <= n) { LOGI("sections: unexpected slot addresses, left alone"); return; }
	sec_n = n;
	static u32 moves[4096][2]; int nm = 0, kinds[MAX_SECTIONS + 1] = {0};
	for (int part = 0; part < 2; part++) {   // decide every word first: moving one changes what its neighbours look like
		u32 lo = part ? LEVEL_BASE : WORLD, hi = part ? LEVEL_BASE + sec_size[0] : WORLD + 0x5800;
		for (u32 a = lo; a < hi; a += 4) {
			int k = sec_owner(a);
			if (k >= 5 && k <= sec_n && sec_bufof(LW(a)) == ((k - 1) & 3) && nm < 4096) { moves[nm][0] = a; moves[nm][1] = k; nm++; kinds[k]++; }
		}
	}
	for (int i = 0; i < nm; i++) { u32 a = moves[i][0]; int k = moves[i][1]; SW(a, LW(a) - sec_buf[(k - 1) & 3] + sec_addr(k)); }
	for (int k = 5; k <= sec_n; k++) SW(SLOT_BASES + 4 * k, sec_addr(k));
	LOGI("sections: %d; 5..%d moved to their own addresses (%d words outside them point at them)", sec_n, sec_n, nm);
}
static bool sctx_held(int k, int except);
#define SEC_SCRATCH 0x0C800000u   // where a section read goes that another player's context already has in
u32 game_read_dest(const char *host, s64 at, u32 buf, u32 size) {
	if (!sec_n) return buf;
	int k = sec_chunk(host, at);
	if (k < 1 || k > sec_n) return buf;
	if (sctx_held(k, -1) && size <= SEC_BUF_SIZE) {   // live in some player's context: its data (and the state its
		if (ENV_ON("SR2_STREAM_TRACE")) LOGI("sections: %d is live, its read is dropped", k);   // objects keep in it) stays
		return SEC_SCRATCH;
	}
	return k >= 5 ? sec_addr(k) : buf;
}
void game_read_done(const char *host, s64 at, u32 buf, u32 n) {
	int k = sec_chunk(host, at);
	if (k == 0 && buf == LEVEL_BASE) { sec_level_read(); return; }
	if (k < 5 || k > sec_n || buf != sec_addr(k)) return;
	u32 from = sec_buf[(k - 1) & 3], cnt = 0;
	for (u32 p = buf; p + 4 <= buf + n; p += 4) {   // (and the uncached ones, 0x40000000 up: its display lists')
		u32 v = LW(p), u = (v >> 28) == 4 && !ENV_ON("SR2_NO_UNC") ? 0x40000000u : 0, a = v - u;
		if (a >= from && a < from + SEC_BUF_SIZE) { SW(p, a - from + buf + u); cnt++; }
	}
	if (ENV_ON("SR2_STREAM_TRACE")) LOGI("sections: %d read to %08x, %u of its words point into it", k, buf, cnt);
}

// Section contexts. The game streams the course around one racer: four buffers (WORLD +0x5d1c, 16 bytes each:
// the slot loaded, its state, flags), the three active sections (+0x5d58, 12 bytes each: slot, buffer, a flag;
// with the level as a fourth, everything that looks for course objects, ground and walls walks these), per-buffer
// pointers into the section's data (+0x5d7c, +0x8c), the current section (+0x5d9c slot, +0x5da0 number). With the
// sections at addresses of their own (above), each player gets a copy of all of this, swapped in while the game
// loads, steps, collides, films or draws for them, so every player has the course around them wherever they are.
// A buffer goes 4 (claimed) -> 8 (read issued) -> 16 (read done, F_159128) -> 32 (live: F_15ea70 made its
// objects) -> 2 (objects off) -> 1 (released, F_15f274). A section live in several contexts is made once and
// released once: F_15ea70 and F_15f274 only do the bookkeeping while another context holds it (state 32 or 2),
// its objects stay on while another has it live (F_160724, whether a slot is live, answers for all contexts),
// and a read of it is dropped (its data, and its objects' state in it, stay as they are).
#define SCTX_LO   (WORLD + 0x5d1c)
#define SCTX_N    (0x5da4 - 0x5d1c)
#define SCTX_BUF(b)  (16 * (b))   // in a context: buffer b's slot (+0), state (+4)
#define SCTX_CUR  (0x5da0 - 0x5d1c)
static u8 sctx_mem[1 + MAX_EXTRA][SCTX_N];
static bool sctx_settling[1 + MAX_EXTRA];   // held until the course around them is in (sctx_settle)
static int sctx_rd[MAX_SECTIONS + 1][8], sctx_nrd[MAX_SECTIONS + 1];   // per section: the contexts whose reads of it are queued
static u8 *sctx_at(int k) { return k == sctx_in ? (u8 *)PTR(SCTX_LO) : sctx_mem[k]; }
static u32 sctx_w(int k, int off) { u32 v; memcpy(&v, sctx_at(k) + off, 4); return v; }
static void sctx_sw(int k, int off, u32 v) { memcpy(sctx_at(k) + off, &v, 4); }
// Each buffer has a pair of lights (F_16aea4 hangs them off the models of the section made in it, F_15a340 puts the
// view's matrix in them), and a light hangs off one model: sections 4 apart, live in two contexts, took them from
// each other, and the one left without drew nothing (F_5547c finds a model's lights among its children). Sections 5
// and up get a pair of their own, after their data; the game's arrays point at the pair of the section in each
// buffer of the context in.
#define SEC_LIGHTS   0x089A3AC8u   // two arrays of 6 node pointers (+0x20 the second); 0 is the level's, b + 1 buffer b's
#define SEC_LIGHT_SZ 0x110
static u32 sec_light0[2][4];   // the game's own, which sections 1-4 keep
static void sec_lights_reset(void) { memset(sec_light0, 0, sizeof sec_light0); }
static void sec_lights_for(int b, int k) {   // buffer b's pointers to section k's pair
	if (!sec_n || b < 0 || b > 3) return;
	for (int i = 0; i < 2; i++) {
		u32 at = SEC_LIGHTS + 0x20 * i + 4 * (b + 1), l = sec_light0[i][b];
		if (!l) { l = LW(at); if (l < 0x08800000 || l >= 0x0A000000) continue; sec_light0[i][b] = l; }
		if (k >= 5 && k <= sec_n && ((k - 1) & 3) == b) {
			u32 n = SEC_HOME(k) + 0x1F0000 + 0x200 * i;
			if (LW(n + 36) != n + 0x70) {   // first use: a copy of the game's, hanging from nothing
				memcpy(PTR(n), PTR(l), SEC_LIGHT_SZ);
				SW(n + 36, n + 0x70);
				memset(PTR(n + 16), 0, 12); memset(PTR(n + 76), 0, 12);
			}
			l = n;
		}
		SW(at, l);
	}
}
static void sec_lights(void) {   // the pairs for the sections in the buffers now
	for (int b = 0; b < 4; b++) {
		u32 sp = LW(WORLD + 0x5d1c + 16 * b);
		sec_lights_for(b, sp ? (s32)LW(sp + 0x20) : 0);
	}
}
static int sctx_use(int k) {   // put player k's context in the world; returns whose was in
	int was = sctx_in;
	if (!sctx_on || k == sctx_in || k < 0 || k > n_extra) return was;
	memcpy(sctx_mem[sctx_in], PTR(SCTX_LO), SCTX_N);
	memcpy(PTR(SCTX_LO), sctx_mem[k], SCTX_N);
	sctx_in = k;
	sec_lights();
	return was;
}
static int sctx_state(int j, int k) {   // the state of section k in context j (0: not in its buffer)
	int b = (k - 1) & 3;
	u32 sp = sctx_w(j, SCTX_BUF(b));
	return sp == WORLD_SLOT(k) ? (int)sctx_w(j, SCTX_BUF(b) + 4) : 0;
}
static bool sctx_held(int k, int except) {   // section k made and not released in a context other than `except`
	if (!sctx_on || k < 1 || k > sec_n) return false;
	for (int j = 0; j <= n_extra; j++) { int st = sctx_state(j, k); if (j != except && (st == 0x20 || st == 2)) return true; }
	return false;
}
static bool sctx_live(int k) {   // live in any context
	for (int j = 0; j <= n_extra; j++) if (sctx_state(j, k) == 0x20) return true;
	return false;
}
static bool sctx_wanted(void) {
	return sec_n && n_extra && p1_racer && in_race && !race_ending && !banter_on && !bt_mp() && !boss_mp()
	       && !extra_pending[n_extra - 1] && !ENV_ON("SR2_ONE_CONTEXT");
}
static void sctx_start(void) {   // everyone starts with player 1's
	for (int k = 0; k <= MAX_EXTRA; k++) { memcpy(sctx_mem[k], PTR(SCTX_LO), SCTX_N); }
	memset(sctx_nrd, 0, sizeof sctx_nrd);
	memset(sctx_settling, 0, sizeof sctx_settling);
	sctx_in = 0; sctx_on = true;
	LOGI("sections: a context per player");
}
static void sctx_stop(Ctx *c) {   // back to player 1's; what only the others had is released
	if (!sctx_on) return;
	sctx_use(0);
	for (int j = 1; j <= n_extra; j++)
		for (int b = 0; b < 4; b++) {
			u32 sp = sctx_w(j, SCTX_BUF(b)), st = sctx_w(j, SCTX_BUF(b) + 4);
			if (!sp || (st != 0x20 && st != 2)) continue;
			int k = (s32)LW(sp + 0x20);
			sctx_sw(j, SCTX_BUF(b), 0); sctx_sw(j, SCTX_BUF(b) + 4, 1);
			if (k < 1 || k > sec_n || sctx_held(k, j)) continue;
			sctx_sw(j, SCTX_BUF(b), sp); sctx_sw(j, SCTX_BUF(b) + 4, st);   // the last one: really release it
			sctx_use(j);
			Ctx save = *c;
			c->r[4] = WORLD; c->r[5] = b; c->r[6] = 0; rt_call(c, 0x08800000 + 0x15f274);
			*c = save;
			sctx_use(0);
		}
	sctx_on = false;
	LOGI("sections: back to one context");
}
static int sctx_racer(u32 r) { return sctx_on ? player_index(r) : -1; }
static void sctx_note_reads(int k, const u32 *st) {   // reads issued since the states st: note whose they are
	for (int b = 0; b < 4; b++) {
		u32 sp = LW(SCTX_LO + SCTX_BUF(b));
		if (st[b] != 8 && LW(SCTX_LO + SCTX_BUF(b) + 4) == 8 && sp) {
			int s = (s32)LW(sp + 0x20);
			if (s >= 1 && s <= MAX_SECTIONS && sctx_nrd[s] < 8) sctx_rd[s][sctx_nrd[s]++] = k;
		}
	}
}
// the streaming step and the loads for players 2-4, each in their context, after the world update's for player 1
static void sctx_stream(Ctx *c) {
	u32 local = LW(WORLD + 0x5024), focus = LW(WORLD + 0x5028);
	for (int k = 1; k <= n_extra; k++) {
		u32 r = extra_racer[k - 1];
		if (!r || extra_pending[k - 1]) continue;
		int was = sctx_use(k);
		SW(WORLD + 0x5024, r); SW(WORLD + 0x5028, r);
		Ctx save = *c;
		c->r[4] = WORLD; rt_call(c, 0x08800000 + 0x15f97c);
		*c = save;
		u32 st[4]; for (int b = 0; b < 4; b++) st[b] = LW(SCTX_LO + SCTX_BUF(b) + 4);
		c->r[4] = WORLD; c->r[5] = r; rt_call(c, 0x08800000 + 0x15e4a8);
		*c = save;
		sctx_note_reads(k, st);
		SW(WORLD + 0x5024, local); SW(WORLD + 0x5028, focus);
		sctx_use(was);
	}
}
static void sctx_trace(void) {
	if (!ENV_ON("SR2_STREAM_TRACE")) return;
	static char last[256];
	char line[256]; int o = 0;
	for (int j = 0; j <= n_extra; j++) {
		o += snprintf(line + o, sizeof line - o, " p%d %d:", j + 1, (int)sctx_w(j, SCTX_CUR));
		for (int b = 0; b < 4; b++) {
			u32 sp = sctx_w(j, SCTX_BUF(b)), st = sctx_w(j, SCTX_BUF(b) + 4);
			o += snprintf(line + o, sizeof line - o, " %d/%x", sp >= 0x08800000 && sp < 0x0C000000 ? (int)LW(sp + 0x20) : 0, st);
		}
	}
	if (strcmp(line, last)) { strcpy(last, line); LOGI("contexts:%s", line); }
}

// put racer r back on the course: a third of the way into section s's path, facing like racer `like`
static bool racer_to_section(Ctx *c, u32 r, int s, u32 like) {
	u32 arr = LW(WORLD_SLOT(s) + 0x444), nodes[4096]; int n = 0;
	if (arr < 0x08800000 || arr >= 0x0C000000) return false;
	for (; n < 4096; n++) { u32 nd = LW(arr + n * 4); if (nd < 0x08800000 || nd >= 0x0C000000) break; nodes[n] = nd; }
	if (!n) return false;
	float t[4]; memcpy(t, PTR(nodes[n / 3]), 16); t[2] += 1; t[3] = 1;
	racer_place_at(c, r, t, like);
	float zero[3] = {0, 0, 0};
	memcpy(PTR(r + RACER_VEL), zero, sizeof zero);
	u32 body = LW(r + 4);
	Ctx save = *c;   // find its place on the course's path again (F_9b2a8, as at the start)
	c->r[4] = r + 0x114; c->r[5] = body + 0x20; c->r[6] = 0; c->r[7] = 5;
	rt_call(c, 0x08800000 + 0x9b2a8);
	*c = save;
	return true;
}
static bool below_section(u32 r) {   // fallen through (its floor went away under it): falling, well below every live path
	u32 body = LW(r + 4);
	if (!body) return false;
	float lo = 1e9f, z, v[3];
	for (int s = 0; s < 16; s++) {
		u32 arr = LW(WORLD_SLOT(s) + 0x444);
		if (!section_live(s) || arr < 0x08800000 || arr >= 0x0C000000) continue;
		for (int i = 0; i < 4096; i++) {
			u32 nd = LW(arr + i * 4);
			if (nd < 0x08800000 || nd >= 0x0C000000) break;
			memcpy(&z, PTR(nd + 8), 4); if (z < lo) lo = z;
		}
	}
	memcpy(&z, PTR(body + 0x28), 4); memcpy(v, PTR(r + RACER_VEL), sizeof v);
	return lo < 1e9f && z < lo - 40 && v[2] < 0;
}
// A computer player can get stuck where the course wants speed (the bottom of a loop, a steep slope), rolling to
// and fro without getting anywhere: put it a little further along the path. One that stands still is waiting on
// purpose (the game's driver eases off when it is far ahead) and is left alone.
#define UNSTICK_VB    240     // no headway for this long (vblanks)
#define UNSTICK_GAIN  0.004f  // of the race
#define UNSTICK_NET   12.0f   // and no further than this from where it was (straight line, world units)
static void bot_unstick(Ctx *c, u32 r, int k) {
	static float prog[1 + MAX_EXTRA]; static u64 since[1 + MAX_EXTRA]; static float moved[1 + MAX_EXTRA]; static int tries[1 + MAX_EXTRA]; static u64 trap[1 + MAX_EXTRA]; static bool going[1 + MAX_EXTRA];
	static float pos0[1 + MAX_EXTRA][3];
	float v[3]; memcpy(v, PTR(r + RACER_VEL), sizeof v);
	moved[k] += sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (g_vblank_count < since[k] + UNSTICK_VB && g_vblank_count >= since[k]) return;
	float p = race_progress(c, r), m = moved[k] / UNSTICK_VB;
	// The game's progress (F_b070c) stands still for seconds at a time on some stretches (springs, rails, the long
	// runs near the end of Blue Coast) while the racer flies along, so it alone made bots that were going fine
	// "stuck" and moved them on every few seconds. A stuck one also has got nowhere: it stands, or rolls to and
	// fro, so it ends up close to where it was.
	float pos[3], net2; memcpy(pos, PTR(LW(r + 4) + 0x20), sizeof pos);
	net2 = (pos[0] - pos0[k][0]) * (pos[0] - pos0[k][0]) + (pos[1] - pos0[k][1]) * (pos[1] - pos0[k][1]) + (pos[2] - pos0[k][2]) * (pos[2] - pos0[k][2]);
	bool stuck = since[k] && g_vblank_count - since[k] < UNSTICK_VB * 2 && p - prog[k] < UNSTICK_GAIN && going[k] && !finish_place[k]   // (standing still counts too, once it got going)
	             && net2 < UNSTICK_NET * UNSTICK_NET;
	if (ENV_ON("SR2_UNSTICK_TRACE")) LOGI("unstick: p%d progress %.4f -> %.4f, moving %.3f, got %.1f, section %d (cur %d, active %x %x %x)%s", k + 1, prog[k], p, m, sqrtf(net2), stream_sec[k], (int)LW(STREAM_CUR), LW(WORLD + 0x5d50), LW(WORLD + 0x5d54), LW(WORLD + 0x5d58), stuck ? ": stuck" : "");
	if (p < 0.01f) going[k] = false; else if (m > 2.0f) going[k] = true;
	prog[k] = p; since[k] = g_vblank_count; moved[k] = 0; memcpy(pos0[k], pos, sizeof pos);
	if (!stuck) return;
	tries[k] = trap[k] && g_vblank_count - trap[k] < UNSTICK_VB * 8 ? tries[k] + 1 : 1;   // (stuck again soon after: further each time)
	trap[k] = g_vblank_count;
	u32 body = LW(r + 4); float at[3]; memcpy(at, PTR(body + 0x20), sizeof at);
	u32 best = 0; float bd = 1e9f, from = 0;
	for (int s = 0; s < 16; s++) {   // its nearest node on the path, of a live section
		if (!section_live(s)) continue;
		u32 arr = LW(WORLD_SLOT(s) + 0x444);
		if (arr < 0x08800000 || arr >= 0x0C000000) continue;
		for (int i = 0; i < 4096; i++) {
			u32 nd = LW(arr + i * 4);
			if (nd < 0x08800000 || nd >= 0x0C000000) break;
			float q[3]; memcpy(q, PTR(nd), sizeof q);
			float d = (q[0] - at[0]) * (q[0] - at[0]) + (q[1] - at[1]) * (q[1] - at[1]) + (q[2] - at[2]) * (q[2] - at[2]);
			if (d < bd) { bd = d; best = nd; memcpy(&from, PTR(nd + 0x34), 4); }
		}
	}
	if (!best) return;
	// and the first one 25 or so further on. Further each time can reach past the loaded part of the course, and
	// then nothing qualified and the bot was left stuck for good (seen at Blue Coast's wall springs): the furthest
	// loaded node ahead of it does then.
	u32 to = 0, last = 0; float to_d = 1e9f, last_d = from + 10;
	float want = from + 25 * (tries[k] < 8 ? tries[k] : 8) + 7 * k;   // (spread out: bots that bunch up block each other)
	for (int s = 0; s < 16; s++) {
		if (!section_live(s)) continue;
		u32 arr = LW(WORLD_SLOT(s) + 0x444);
		if (arr < 0x08800000 || arr >= 0x0C000000) continue;
		for (int i = 0; i < 4096; i++) {
			u32 nd = LW(arr + i * 4);
			if (nd < 0x08800000 || nd >= 0x0C000000) break;
			float d; memcpy(&d, PTR(nd + 0x34), 4);
			if (d >= want && d < to_d) to = nd, to_d = d;
			if (d > last_d) last = nd, last_d = d;
		}
	}
	if (!to) to = last, to_d = last_d;
	if (!to) return;
	float t[4]; memcpy(t, PTR(to), 16); t[2] += 1; t[3] = 1;
	racer_place_at(c, r, t, r);
	float zero[3] = {0, 0, 0};
	memcpy(PTR(r + RACER_VEL), zero, sizeof zero);
	Ctx save = *c;
	c->r[4] = r + 0x114; c->r[5] = body + 0x20; c->r[6] = 0; c->r[7] = 5;
	rt_call(c, 0x08800000 + 0x9b2a8);
	*c = save;
	LOGI("unstick: player %d had no headway, on to %.0f along the course", k + 1, to_d);
}
// The world update (F_15aafc) runs the objects of the sections in its active list, player 1's, in a row of passes.
// Those live only in other players' contexts get the same passes here, once a frame, in the first such context
// with its racer as the one the world follows: their ground pieces, springs and rings come to life for them.
static u32 sec_obj(int sec, u32 j, u32 *info);
static u32 sec_obj(int sec, u32 j, u32 *info) {   // object j of a section: the first set, then the second
	u32 base = WORLD + sec * 1216, n8 = LW(base + 0x4c8), o;
	if (j < n8) { *info = LW(base + 0x4a0) + 112 * j; o = LW(base + 0x160 + 4 * j); }
	else { *info = LW(base + 0x4a4) + 112 * (j - n8); o = LW(base + 0x260 + 4 * (j - n8)); }
	if (o && (o < 0x08800000 || o >= 0x0C000000 || LW(o) < 0x08980000 || LW(o) >= 0x089b0000)) {
		static int said; if (said++ < 4) LOGI("sections: section %d object %u at %08x is not one (table %08x, context %d)", sec, j, o, LW(o), sctx_in + 1);
		return 0;
	}
	return o;
}
static u32 obj_call(Ctx *c, u32 fn, u32 a0, u32 a1, u32 a2) {
	if (fn < 0x08804000 || fn >= 0x08980000) { static int said; if (said++ < 8) LOGI("sections: object %08x has no function there (%08x)", a0, fn); return 0; }
	Ctx t = *c; t.r[4] = a0; t.r[5] = a1; t.r[6] = a2; rt_call(&t, fn); return t.r[2];
}
static void sctx_section_passes(Ctx *c, int sec) {
	u32 base = WORLD + sec * 1216, n8 = LW(base + 0x4c8), n = n8 + LW(base + 0x4cc), info, o;
	if (ENV_ON("SR2_CTX_DEBUG")) {
		static u64 last; u64 t = mono_us();
		if (t - last > 3000000) {
			last = t; int act = 0; for (u32 j = 0; j < n; j++) if (sec_obj(sec, j, &info) && (LW(info + 8) & 1)) act++;
			LOGI("ctx debug: passes for section %d in %d: %u objects (%u first), %d on", sec, sctx_in + 1, n, n8, act);
		}
	}
	#define BIT(x, b) (((x) >> (b)) & 1)
	#define FLAGS(o) LW(LW((o) + 4) + 8)
	for (u32 j = 0; j < n; j++)   // waking (F_1699d0)
		if ((o = sec_obj(sec, j, &info)) && (LW(info + 8) & 0x20900) && !obj_call(c, 0x08800000 + 0x1699d0, o, sec, 0)) break;
	for (u32 j = 0; j < n; j++) {   // F_16c0b0, or a reset for those so marked
		if (!(o = sec_obj(sec, j, &info)) || !(LW(info + 4) & 0xf02) || !BIT(LW(info + 8), 10)) continue;
		u32 f = LW(o + 4);
		if (BIT(LW(f + 8), 7)) {
			SW(f + 8, LW(f + 8) & 0xfffdfcffu);
			if (obj_call(c, LW(LW(o) + 16), o, 0, 0)) { u32 v = obj_call(c, LW(LW(o) + 16), o, 0, 0); SW(v + 8, LW(v + 8) & ~2u); }
			f = LW(o + 4); SW(f + 8, LW(f + 8) & ~3u);
		} else if (!BIT(LW(f + 4), 3)) obj_call(c, 0x08800000 + 0x16c0b0, o, 1, 2);
	}
	u32 sp = c->r[29] + 0x40;   // as in F_15aafc's frame: a flag byte, the time step (the caller keeps room above sp)
	SW(sp, 0); memcpy(PTR(sp + 4), PTR(WORLD + 0x5d14), 4);
	for (u32 j = 0; j < n8; j++)
		if ((o = sec_obj(sec, j, &info)) && (LW(info + 8) & 1) && !obj_call(c, 0x08800000 + 0x1691b4, o, sec, sp)) {
			if (LW(sp) & 0xff) LOGI("sections: an object of section %d ends the race for another player (ignored)", sec);
			break;
		}
	for (u32 j = 0; j < n; j++)
		if ((o = sec_obj(sec, j, &info)) && (LW(info + 8) & 1) && !obj_call(c, 0x08800000 + 0x169828, o, sec, sp + 4)) break;
	for (u32 j = 0; j < n; j++)
		if ((o = sec_obj(sec, j, &info)) && (LW(info + 8) & 1) && BIT(FLAGS(o), 15)) obj_call(c, 0x08800000 + 0x14888, o, 0, 0);
	for (u32 j = 0; j < n8; j++) {   // those with physics of their own
		if (!(o = sec_obj(sec, j, &info)) || !BIT(LW(info + 4), 3) || !(LW(info + 8) & 1)) continue;
		if (BIT(LW(LW(o + 4) + 4), 3) && (LW(o + 744) & 0x1000000)) continue;
		obj_call(c, 0x08800000 + 0x15c0a4, o, 0, 0);
		obj_call(c, 0x08800000 + 0xa5828, o, 0, 0);
		obj_call(c, 0x08800000 + 0x15c0a4, o, 0, 0);
		memcpy(PTR(o + 0x20), PTR(LW(o + 4) + 0x20), 16);
	}
	for (int q = 0; q < 2; q++) {   // the section's own two (F_15aafc asks F_160724 whether the section is live)
		Ctx t = *c; t.r[4] = LW(base + 0x4a8 + 4 * q); memcpy(&t.f[12], PTR(WORLD + 0x5d14), 4);
		u32 fn = t.r[4] ? LW(LW(t.r[4]) + 16) : 0;
		if (fn >= 0x08804000 && fn < 0x08980000) rt_call(&t, fn);
	}
	for (u32 j = 0; j < n8; j++) {   // the step and the after-step of each
		if (!(o = sec_obj(sec, j, &info)) || !BIT(LW(info + 4), 2) || !(LW(info + 8) & 1)) continue;
		if (BIT(FLAGS(o), 15)) obj_call(c, 0x08800000 + 0x14888, o, 0, 0);
		Ctx t = *c; t.r[4] = o; memcpy(&t.f[12], PTR(o + 48), 4);
		u32 fn = LW(LW(o) + 108);
		if (fn >= 0x08804000 && fn < 0x08980000) rt_call(&t, fn);
		if (!(FLAGS(o) & 0x4000000)) obj_call(c, LW(LW(o) + 68), o, 0, 0);
	}
	for (u32 j = n8; j < n; j++) {
		if (!(o = sec_obj(sec, j, &info)) || !BIT(LW(info + 4), 6) || !(LW(info + 8) & 1)) continue;
		if (!(FLAGS(o) & 0x4000000)) obj_call(c, LW(LW(o) + 68), o, 0, 0);
	}
	#undef BIT
	#undef FLAGS
}
static void sctx_passes(Ctx *c0) {
	Ctx cc = *c0, *c = &cc; c->r[29] = (c0->r[29] - 0x100) & ~15u;
	u32 f24 = LW(WORLD + 0x5024), f28 = LW(WORLD + 0x5028);
	for (int k = 1; k <= sec_n; k++) {
		if (sctx_state(0, k) == 32) continue;
		for (int j = 1; j <= n_extra; j++) {
			if (sctx_state(j, k) != 32 || extra_pending[j - 1] || !extra_racer[j - 1]) continue;
			bool view = LW(CAM_MAIN) && view_cur < 0 && !ENV_ON("SR2_PASS_NOVIEW");   // with their camera: objects wake and act by it
			int was = view ? -1 : sctx_use(j);
			if (view) view_enter(j - 1);
			SW(WORLD + 0x5024, extra_racer[j - 1]); SW(WORLD + 0x5028, extra_racer[j - 1]);
			sctx_section_passes(c, k);
			SW(WORLD + 0x5028, f28);
			if (view) view_leave(j - 1); else { SW(WORLD + 0x5024, f24); sctx_use(was); }
			break;
		}
	}
}
static void sctx_dump(Ctx *c, u32 r, int k, const char *what) {   // SR2_CTX_DEBUG: the context in the world, with what collision reads
	char line[700]; int o = 0;
	for (int b = 0; b < 4; b++) {
		u32 m = LW(WORLD + 0x5d8c + 4 * b), m84 = m ? LW(m + 84) : 0;
		o += snprintf(line + o, sizeof line - o, " [%08x %x %08x %08x %08x %08x]", LW(SCTX_LO + 16 * b), LW(SCTX_LO + 16 * b + 4), LW(WORLD + 0x5d7c + 4 * b), m, m84, m84 ? LW(m84 + 28) : 0);
	}
	u32 cs = LW(WORLD + 0x5d9c);
	o += snprintf(line + o, sizeof line - o, " cur %08x (%d)", cs, cs ? (int)LW(cs + 0x20) : -1);
	if (cs) for (int i = 0; i < 6; i++) o += snprintf(line + o, sizeof line - o, " %x", LW(cs + 0x24 + 4 * i));
	u32 body = LW(r + 4);
	if (body) {   // a ray straight down through the racer against the live sections (F_1636d0)
		Ctx t = *c; u32 sp = (c->r[29] - 0x800) & ~15u;
		float a[4], b[4]; memcpy(a, PTR(body + 0x20), 12); a[3] = 1; a[2] += 10; b[0] = 0; b[1] = 0; b[2] = -50; b[3] = 0;
		memcpy(PTR(sp + 0x100), a, 16); memcpy(PTR(sp + 0x110), b, 16); memset(PTR(sp + 0x200), 0, 0x100);
		t.r[29] = sp; t.r[4] = WORLD; t.r[5] = sp + 0x100; t.r[6] = sp + 0x110; t.r[7] = sp + 0x200; t.r[8] = 0; t.r[9] = 0;
		rt_call(&t, 0x08800000 + 0x1636d0);
		float h[4]; memcpy(h, PTR(sp + 0x200), 16);
		o += snprintf(line + o, sizeof line - o, " ray z %.1f: %u at %.1f %.1f %.1f", a[2] - 10, t.r[2], h[0], h[1], h[2]);
	}
	LOGI("ctx debug: p%d %s in %d:%s", k + 1, what, sctx_in + 1, line);
}
static void sctx_keep_up(Ctx *c, u32 r, int k) {   // with a context each: only one who fell through the course
	if (finish_place[k]) { stream_out_since[k] = 0; return; }
	int was = sctx_use(k);
	if (ENV_ON("SR2_CTX_DEBUG")) { static u64 last[MAX_EXTRA + 1]; u64 t = mono_us(); if (t - last[k] > 5000000) { last[k] = t; sctx_dump(c, r, k, "now"); } }
	if (!below_section(r)) { stream_out_since[k] = 0; sctx_use(was); return; }
	u64 now = mono_us();
	if (!stream_out_since[k]) stream_out_since[k] = now;
	else if (now - stream_out_since[k] >= STREAM_OUT_US) {
		stream_out_since[k] = 0;
		int sec = racer_section(c, r), to = sec;
		for (int d = 1; d < 4 && !section_live(to); d++) to = section_live(sec - d) ? sec - d : sec + d;
		if (ENV_ON("SR2_CTX_DEBUG")) sctx_dump(c, r, k, "falls");
		if (section_live(to) && racer_to_section(c, r, to, r)) LOGI("sections: player %d fell through section %d: back on the course in %d", k + 1, sec, to);
	}
	sctx_use(was);
}
static void stream_keep_up(Ctx *c, u32 r, int k) {   // from the move step of player k's racer
	if (sctx_on && k >= 0 && k <= n_extra) { sctx_keep_up(c, r, k); return; }
	int f = (s32)LW(STREAM_CUR);
	if (k < 0 || k > n_extra || !stream_mp() || !stream_leader || f < 1 || finish_place[k]) { if (k >= 0 && k <= MAX_EXTRA) stream_out_since[k] = 0; return; }
	int sec = stream_sec[k];
	bool out = sec < f - 1 || sec > f + 2 || !section_live(sec) || below_section(r);
	if (!out) { stream_out_since[k] = 0; return; }
	u64 now = mono_us();
	if (!stream_out_since[k]) { stream_out_since[k] = now; return; }
	if (now - stream_out_since[k] < STREAM_OUT_US) return;
	stream_out_since[k] = 0;
	int to = sec > f + 2 ? f + 2 : sec >= f - 1 && section_live(sec) ? sec : f - 1;   // (under its own section: back onto it)
	while (to < f + 2 && !section_live(to)) to++;
	while (to > f - 1 && !section_live(to)) to--;
	if (!section_live(to)) return;
	if (racer_to_section(c, r, to, stream_leader)) LOGI("stream: player %d was in section %d (window %d..%d) or under it: back on the course in %d", k + 1, sec, f - 1, f + 2, to);
}

// The banter before the start: story races open with a camera shot of each of the two racers saying a line
// (race states 3 and 4, F_597d8). With players 2-4 every player gets one, in turn: the check for it (F_81944) says
// yes, the racer the shot asks for (F_3731c) is the next player, and the end of the last shot (state 5 or 6) goes
// back to state 3 for the next one.
#define BANTER_CHECK_RA  0x088598a4   // F_597d8 state 2: F_81944(mode) decides on the banter
#define BANTER_FIRST_RA  0x08859934   // state 2 -> 3: the first shot's racer, F_3731c(world, 0)
#define BANTER_NEXT_RA   0x08859a14   // state 3: the next shot's racer, F_3731c(world, 1)
#define BANTER_OTHER1_RA 0x0885997c   // ... and the racer each shot looks past (F_59770)
#define BANTER_OTHER2_RA 0x08859a48
#define BANTER_END_RA    0x08859a98   // state 3 -> 4, and state 4 -> 5 / 6 when the shot is over
static int banter_next, banter_n;
static u32 banter_racer[1 + MAX_EXTRA];
static bool banter_done;   // this race opened with it (players 2-4 then say nothing more at the start)
#define BANTER_SHOT  (WORLD + 0x4fc8)   // the shots' camera: distance from the racer at the start and end, ...
#define BANTER_ZOOM  0.55f
static float banter_dist[2];
static void banter_stop(void) { if (banter_on) memcpy(PTR(BANTER_SHOT), banter_dist, 8); banter_on = false; }

// The smarter computer players (experimental, Settings > Multiplayer): the game's driver keeps an eye on the
// local player (world +0x5024), easing off when it is far ahead of them and pushing when behind. While a
// computer player steps, the one it watches is instead whoever is furthest along of the others, so it races
// the leader rather than waiting for player 1.
// Signature moves with players 2-4. A move that reaches a rival asks F_b1924 for "the" rival, which only knows the
// two seats: it aims at the nearest other player instead, picked when the move starts (F_123374) and kept until it
// ends (F_123cf4), Rouge's bats and Metal Sonic's copy among them. The one hit asks who hit them the same way, and
// gets the player whose move is aimed at them. Chaos Control already slows everyone but its user (time mode 4,
// HOOK_ENTITY_DT), and Silver's ESP, which scrambles its target's controls (F_174e8(controls, 3), 2 puts them back),
// is given to everyone else too.
#define RACER_CHAR      0x1d0   // the game's character id (F_b502c): Metal Sonic (5) takes the rival's
#define RACER_SIG       0x4f4   // the signature move: +0 its racer, +8 set while it runs
#define CHAR_SILVER     4
#define CHAR_METAL      5
#define SIG_START_RA    0x089233c0   // F_123374, the move's start
#define SIG_END_RA      0x08923d3c   // F_123cf4, its end
#define SET_CONTROLS    (0x08800000 + 0x174e8)
static u32 sig_target[1 + MAX_EXTRA];
static u32 sig_player(int k) { u32 r = player_racer(k); return r && (!k || !extra_pending[k - 1]) ? r : 0; }
static float racer_dist2(u32 a, u32 b) {
	float *p = (float *)PTR(LW(a + 4) + 0x20), *q = (float *)PTR(LW(b + 4) + 0x20);
	float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
	return dx * dx + dy * dy + dz * dz;
}
static u32 nearest_rival(u32 r) {
	u32 best = 0; float bd = 0;
	for (int k = 0; k <= n_extra; k++) {
		u32 o = sig_player(k);
		if (!o || o == r || !LW(o + 4) || !LW(r + 4)) continue;
		float d = racer_dist2(r, o);
		if (!best || d < bd) best = o, bd = d;
	}
	return best;
}
static bool guest_ptr(u32 p) { return p >= 0x08800000 && p < 0x0B000000; }   // (extra racers live above the 32 MB)
static bool sig_running(u32 r) { u32 s = LW(r + RACER_SIG); return guest_ptr(s) && LW(s + 8); }
static bool sig_mp(void) { return n_extra && mp_active() && p1_racer && !bt_mp() && !boss_mp(); }
static u32 sig_opponent(u32 r, u32 ra) {
	int i = player_index(r);
	if (i < 0) return 0;
	switch (ra) {
	case SIG_START_RA:
		return sig_target[i] = nearest_rival(r);
	case 0x08923b80: case 0x08923bf8: case 0x08923c58:   // Rouge's bats (the start)
	case 0x08924728: case 0x08924e80:                     // the move's effects following the target (F_1246fc, ESP)
	case SIG_END_RA:
	case 0x088b505c: case 0x088b506c: case 0x088b5080:   // F_b502c: whose move Metal Sonic copies
		if (!sig_target[i] || !sig_running(r) || player_index(sig_target[i]) < 0) sig_target[i] = nearest_rival(r);
		return sig_target[i];
	case 0x08912cbc:                                      // hit by a move (F_112xxx), and the sig events (F_b3ef8)
	case 0x088b4e9c: case 0x088b4eb4: case 0x088b4ed0: {
		u32 best = 0; float bd = 0;   // the one whose move is aimed at them, else the nearest with a move running
		for (int k = 0; k <= n_extra; k++) {
			u32 o = sig_player(k);
			if (!o || o == r || !sig_running(o)) continue;
			if (sig_target[k] == r) {
				if (ENV_ON("SR2_OPP_TRACE")) LOGI("signature: player %d hit by player %d's move (%08x)", i + 1, k + 1, ra);
				return o;
			}
			float d = racer_dist2(r, o);
			if (!best || d < bd) best = o, bd = d;
		}
		return best;
	}
	}
	return 0;
}
static void esp_others(Ctx *c, u32 r, int mode) {   // ESP on (3) or off (2) for everyone but its user and its target
	int i = player_index(r);
	for (int k = 0; k <= n_extra; k++) {
		u32 o = sig_player(k);
		if (!o || o == r || o == sig_target[i]) continue;
		u32 in = LW(o + 0x8c);
		if (!guest_ptr(in)) continue;
		Ctx save = *c;
		c->r[4] = LW(in + 4); c->r[5] = mode; rt_call(c, SET_CONTROLS);
		*c = save;
		if (ENV_ON("SR2_OPP_TRACE")) LOGI("ESP %d on player %d (controls %08x: %08x)", mode, k + 1, LW(in + 4), LW(LW(in + 4) + 0x50));
	}
}
static u32 smart_target(Ctx *c, u32 r) {
	if (!g_cfg.mp_smart_ai || !mp_active() || !p1_racer || race_ending || bt_mp() || boss_mp()) return 0;
	int k = extra_index(r);
	if (k < 0 || !extra_cpu[k]) return 0;
	static u64 at[MAX_EXTRA]; static u32 got[MAX_EXTRA];   // once a frame
	if (at[k] == g_vblank_count + 1) return got[k];
	at[k] = g_vblank_count + 1;
	u32 best = 0; float bp = -1, mine = race_progress(c, r);   // (in front of everyone, the next one: RACER_PACE keeps it going)
	for (int i = 0; i <= n_extra; i++) {
		u32 o = i ? extra_racer[i - 1] : p1_racer;
		if (o == r || (i && extra_pending[i - 1])) continue;
		float p = race_progress(c, o);
		if (p > bp) bp = p, best = o;
	}
	if (ENV_ON("SR2_SMART_TRACE") && g_vblank_count % 120 == 0) LOGI("smart AI: player %d watches player %d (%.3f vs %.3f)", k + 2, player_index(best) + 1, mine, bp);
	return got[k] = best;
}
static bool smart_pace(u32 r) {   // a computer driver in a race that should never ease off for the one behind
	if (!g_cfg.mp_smart_ai || !in_race || bt_mode() || boss_obj || (mp_active() && (bt_mp() || boss_mp()))) return false;
	u32 ip = LW(r + 0x8c), in = ip >= 0x08800000 && ip < 0x0A000000 ? LW(ip + 4) : 0;
	if (in < 0x08800000 || in >= 0x0A000000) return false;
	s32 src = (s32)LW(in + 0x1c);
	return src >= -3 && src <= -1;
}
static void racer_step(Ctx *c, void (*orig)(Ctx *c), u32 r) {
	u32 t = smart_target(c, r), lp = LW(WORLD + 0x5024);
	if (t == r) t = 0;
	if (t) SW(WORLD + 0x5024, t);
	c->r[4] = r; orig(c);
	if (t) SW(WORLD + 0x5024, lp);
}

// A player put down in a section their context has not got in yet (a warp, or the streaming behind) would drop
// through it, and on through the course below: they are held still until it is live, and its neighbours too
// (the ground a step on may be the next one's), as the game holds player 1 while it loads.
static bool sctx_settle(int k, u32 s) {   // whether player k waits; s: the section their racer is in
	if (!sctx_on || (s32)s < 1 || (s32)s > sec_n) return false;
	if (sctx_state(k, s) != 0x20) sctx_settling[k] = true;
	else if (sctx_settling[k]) {
		u32 sl = WORLD_SLOT(s);
		bool all = true;
		for (int i = 0; i < 3; i++) {
			s32 n = (s32)LW(sl + 0x24 + 4 * i);
			if (n >= 1 && n <= sec_n && LW(sl + 0x30 + 4 * i) && sctx_state(k, n) != 0x20) all = false;
		}
		if (all) sctx_settling[k] = false;
	}
	if (sctx_settling[k]) { static u32 said; if (g_vblank_count - said > 120) { said = g_vblank_count; u32 sl = WORLD_SLOT(s);
		LOGI("sections: player %d waits for section %d (%x) and around it: %d %x %x, %d %x %x, %d %x %x", k + 1, s, sctx_state(k, s),
		     (s32)LW(sl + 0x24), LW(sl + 0x30), sctx_state(k, (s32)LW(sl + 0x24)), (s32)LW(sl + 0x28), LW(sl + 0x34), sctx_state(k, (s32)LW(sl + 0x28)),
		     (s32)LW(sl + 0x2c), LW(sl + 0x38), sctx_state(k, (s32)LW(sl + 0x2c))); } }
	return sctx_settling[k];
}
#define RACER_RIVAL  0x230   // racer: 1 for the second seat's (the CPU rival in a single player race)
#define RACER_MAX_DT (1.0f / 20)   // the longest step of a racer's physics and move (slower than the race on a slow host)
void rt_hookcall(Ctx *c, u32 id, void (*orig)(Ctx *c)) {
	if (!game_is_sr2()) { orig(c); return; }
	if (sctx_on && (id == HOOK_RACER_MOVE || id == HOOK_RACER_UPDATE || id == HOOK_RACER_STEP || id == HOOK_ENTITY_DT
	                || id == HOOK_RACER_CORE || id == HOOK_RACER_PHYS)) {   // a player's racer:
		int k = player_index(A(0));                                                                   // in their context
		if (ENV_ON("SR2_CTX_DEBUG")) { static u8 seen[128][5]; int kk = k < 0 ? 4 : k; if (id < 128 && !seen[id][kk]) { seen[id][kk] = 1; LOGI("ctx debug: hook %u racer %08x player %d, context in %d", id, A(0), k + 1, sctx_in + 1); } }
		if (id == HOOK_RACER_PHYS && k >= 1 && LW(A(0) + RACER_RIVAL) == 1 && !ENV_ON("SR2_RIVAL_PHYS")) {
			// the second seat's racer is the CPU rival's, whose landing (F_9f0ac: F_9d118 with t1 = 1, F_9d63c) only
			// works near the local player: it went through floors in sections only that player had. Not for a player
			SW(A(0) + RACER_RIVAL, 0); rt_hookcall(c, id, orig); SW(A(0) + RACER_RIVAL, 1); return;
		}
		if ((id == HOOK_RACER_PHYS || id == HOOK_RACER_MOVE) && c->f[12] > RACER_MAX_DT && !ENV_ON("SR2_NO_DT_CAP"))
			c->f[12] = RACER_MAX_DT;   // a long frame (the game's time step goes up to 1/8 s): a racer falling fast went through a floor between two checks
		if (id == HOOK_RACER_PHYS && k >= 1 && sctx_settle(k, LW(LW(A(0) + 4) + 12))) return;   // held still meanwhile
		if ((id == HOOK_RACER_PHYS || (id == HOOK_RACER_MOVE && ENV_ON("SR2_MOVE_FOCUS"))) && k >= 1 && LW(WORLD + 0x5024) != A(0) && !ENV_ON("SR2_NO_PHYS_FOCUS")) {   // and as the one the world follows: the
			u32 f = LW(WORLD + 0x5024), f28 = LW(WORLD + 0x5028);   // ground checks take only that racer anywhere, the others only inside a live
			SW(WORLD + 0x5024, A(0));     // section's bounds (F_1605f4), which leave out some of the course
			if (ENV_ON("SR2_F28")) SW(WORLD + 0x5028, A(0));
			rt_hookcall(c, id, orig);
			SW(WORLD + 0x5024, f); SW(WORLD + 0x5028, f28);
			return;
		}
		if (k >= 0 && k != sctx_in) { int was = sctx_use(k); rt_hookcall(c, id, orig); sctx_use(was); return; }
	}
	switch (id) {
	case HOOK_BUILD_RACER_SLOT: {   // F_16208c(loader, slot)
		u32 loader = A(0), slot = A(1);
		orig(c);
		if (slot == WORLD_SLOT(15)) {
			n_extra = 0; boss_obj = 0;
			memset(extra_seated, 0, sizeof extra_seated);
			for (int k = 0; k < MAX_EXTRA; k++)   // bots: mp_bots has a bit per player number (2-4)
				extra_cpu[k] = mp_active() && (ENV_ON("SR2_MP_CPU") || (g_cfg.mp_bots >> (k + 2) & 1));
			// Single Event has the race's own rival (slot 14): with players 2-4 it is player 2's, and the
			// racers we add are players 3 and 4
			u32 rival = LW(WORLD_SLOT(14) + 0xd0);
			if (mp_active() && !ENV_ON("SR2_EXTRA_RACER") && g_cfg.mp_players > 1 && rival) {
				extra_racer[0] = extra_pending[0] = rival; extra_seated[0] = true; extra_char[0] = g_cfg.mp_char2;
				n_extra = 1;
				LOGI("player 2: the rival seat, racer %08x (%s)", rival, mp_char_names[g_cfg.mp_char2]);
			}
			for (int k = 0, first = n_extra; k < MAX_EXTRA - first && extra_racer_prepare(k, k + 1 + first); k++) extra_racer_build(c, loader);
		}
		return;
	}
	case HOOK_RACER_INPUT:   // F_17d58(input)
		if ((s32)LW(A(0) + 0x1c) == 1 && input_extra(A(0)) >= 0 && !extra_cpu[input_extra(A(0))]) {
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
			bool lead_view = n_views() == 3;   // (the fourth quarter may follow a computer player)
			static int diff = -1;
			if (diff < 0) diff = ENV_ON("SR2_VIEW_DIFF");
			static int steps;   // (a few seconds into the race)
			bool diff_now = diff && ++steps > 600;
			for (int k = 0; k < n_extra; k++) {   // the players' cameras (computer players need none)
				if (extra_cpu[k] && !lead_view && !ENV_ON("SR2_BOT_VIEWS")) continue;
				if (diff_now) view_diff(c, orig, k);
				else { view_enter(k); orig(c); view_leave(k); }   // (the camera reads the local player: where it stands, its path)
				*c = save;
			}
			if (diff_now) view_diff(c, orig, -1), *c = save;   // and player 1's step again, for comparison
			return;
		}
		break;
	case HOOK_WORLD_DRAW:     // F_15a460(world)
	case HOOK_SHADOW_DRAW:    // F_31300
	case HOOK_EFFECT_DRAW:    // F_30414
	case HOOK_ALPHA_DRAW:     // F_24064
		if (split_view_on()) { split_draw_views(c, orig, id == HOOK_ALPHA_DRAW); return; }
		break;
	case HOOK_RACE_STATE: {   // F_592cc(race, state)
		if (ENV_ON("SR2_EV_TRACE")) LOGI("race %08x state %u (was %u) from %08x vb %llu", A(0), A(1), LW(A(0) + 0x24), c->r[31], (unsigned long long)g_vblank_count);
		if (banter_on && (A(1) == 5 || A(1) == 6) && c->r[31] == BANTER_END_RA) {
			if (banter_next < banter_n) c->r[5] = 3;   // the next player's shot
			else banter_stop();
		}
		if (A(1) < 3 || (banter_on && A(1) >= 5 && c->r[31] != BANTER_END_RA)) banter_stop();   // (the bosses go on without the shots)
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
		if (id == HOOK_HUD_ELEM && A(0) - HUD_BASE == HUD_PLACE && n_extra && p1_racer && in_race && mp_active() && !bt_mp() && !boss_mp())
			return;   // the place (1 or 2, of the two seats): the overlay shows each player's of everyone
		if (hud_pass) {   // split screen: which pieces this pass of the HUD draws
			u32 e = id == HOOK_HUD_ELEM ? A(0) - HUD_BASE : ~0u;
			bool shared = e == HUD_TIMER || (!boss_mp() && !bt_mp() && (e == HUD_METER || e == HUD_METER_ICON || e == HUD_RIVAL_ICON));
			if (id == HOOK_HUD_PART_A) break;   // the text queued by the elements this pass draws
			bool theirs = boss_mp() ? e == HUD_RIVAL_ICON : bt_mp() && bt_mode() == BT_TAG && (e == HUD_TAG_CLOCK0 || e == HUD_TAG_HEAD0);
			if (hud_pass == HUD_PASS_VIEW) { if (shared || theirs) return; }   // (a boss, Tag: each view only its own count)
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
		if (n_extra && !ENV_ON("SR2_NO_CONTACTS")) {
			Ctx save = *c;
			orig(c);
			Ctx out = *c;
			u32 p1 = LW(WORLD + 0x5024);
			for (int k = 0; k < n_extra; k++) {
				if (extra_pending[k]) continue;
				int was = sctx_use(k + 1);
				*c = save; SW(WORLD + 0x5024, extra_racer[k]); orig(c);
				sctx_use(was);
			}
			SW(WORLD + 0x5024, p1);
			if (sctx_on && !ENV_ON("SR2_NO_PASSES")) sctx_passes(c);
			*c = out;
			return;
		}
		break;
	case HOOK_FOCUS_RACER_STEP:   // F_15e4a8(world, racer): loads the sections around the current one, nearest to it first
		if (sctx_wanted() != sctx_on && sections_own()) { if (sctx_on) sctx_stop(c); else sctx_start(); }
		if (sctx_on) {   // player 1's context, then everyone else's
			int was = sctx_use(0);
			u32 st[4]; for (int b = 0; b < 4; b++) st[b] = LW(SCTX_LO + SCTX_BUF(b) + 4);
			orig(c);
			sctx_note_reads(0, st);
			Ctx out = *c;
			sctx_stream(c);
			*c = out;
			sctx_trace();
			sctx_use(was);
			return;
		}
		if (stream_mp() && stream_leader) c->r[5] = stream_leader;
		if (n_extra && ENV_ON("SR2_FOCUS_STEP")) {
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
	case HOOK_RACER_STATE_STEP: {   // F_b9b2c(racer, state): 0 hands over to the next state, then F_b8720 steps again
		static u32 who, n;
		u32 r = A(0);
		orig(c);
		if (c->r[2]) { n = 0; return; }
		if (r != who) { who = r; n = 0; }
		if (++n >= 64) {   // round and round: done for this frame
			static int said; if (said++ < 8) LOGI("racer %08x: no state settles (now %08x), on to the next frame", r, LW(r + 0x4b4));
			c->r[2] = 1; n = 0;
		}
		return;
	}
	case HOOK_SLOT_ACTIVE: {   // F_160724(world, slot): whether a load slot is live (14, 15 and 63 always). Sounds
		// start and keep playing only for what is in a live slot; for the sound code an extra racer's slot counts as
		// live, so players 2-4 are heard, voices too. Only there: other callers take a live slot for a loaded course
		// section (and stop drawing the racer).
		u32 ra = c->r[31];
		if (sctx_on && ra == 0x0895b040 && (s32)A(1) >= 1 && sctx_state(0, A(1)) != 32) { c->r[2] = 0; return; }   // (sctx_passes)
		if (sctx_on && (s32)A(1) >= 1 && (s32)A(1) <= sec_n && sctx_live(A(1))) { c->r[2] = 1; return; }   // live for someone
		if (voice_trace) for (int k = 0; k < n_extra; k++)
			if (!extra_pending[k] && A(1) == LW(LW(extra_racer[k] + 4) + 0x10)) { static u32 seen[64]; int i = 0; while (i < 64 && seen[i] && seen[i] != ra) i++; if (i < 64 && !seen[i]) { seen[i] = ra; LOGI("slot check: slot %u (p%d) from %08x", A(1), k + 2, ra); } }
		if (ra != 0x0883d15c && ra != 0x0883d330 && ra != 0x0883d564   // the play calls (F_3d0xx, F_3d294, F_3d48c)
		    && ra != 0x0896252c) break;                                  // F_162514, from the sound update
		for (int k = 0; k < n_extra; k++)
			if (!extra_pending[k] && A(1) == LW(LW(extra_racer[k] + 4) + 0x10)) { c->r[2] = 1; return; }
		break;
	}
	case HOOK_RACER_PACE: {   // F_b1a84(racer)
		u32 r = A(0);
		orig(c);
		if (ENV_ON("SR2_SMART_TRACE") && g_vblank_count % 120 == 0)
			LOGI("smart AI: racer %08x pace %d, progress %.3f", r, (int)LW(r + 0x8dc), race_progress(c, r));
		if (LW(r + 0x8dc) == 1 && smart_pace(r)) {
			SW(r + 0x8dc, 0);
		}
		return;
	}
	case HOOK_ENTITY_DT:   // F_169828(entity, ?, &dt): mode 4 spares only the seated racers, so players 3-4 using
		// Chaos Control would slow themselves too. Mode 0 (with the slow-motion clock at 1) leaves the time as it is
		if (n_extra && LW(TIME_MODE) == TIME_CHAOS && extra_index(A(0)) >= 0 && LB(A(0) + RACER_CHAOS)) {
			SW(TIME_MODE, 0); orig(c); SW(TIME_MODE, TIME_CHAOS);
			return;
		}
		break;
	case HOOK_SIG_START: {   // F_123374(sig): a signature move starts
		u32 sig = A(0), r = LW(sig);
		bool was = LW(sig + 8);
		orig(c);
		if (ENV_ON("SR2_OPP_TRACE")) LOGI("signature: racer %08x (char %u) starts, was %d now %u, target %08x, mp %d", r, LW(r + RACER_CHAR), was, LW(sig + 8), player_index(r) >= 0 ? sig_target[player_index(r)] : 0, sig_mp());
		if (sig_mp() && !was && LW(sig + 8) && LW(r + RACER_CHAR) == CHAR_SILVER) esp_others(c, r, 3);
		return;
	}
	case HOOK_SIG_END: {   // F_123cf4(sig): it ends
		u32 sig = A(0), r = LW(sig);
		if (sig_mp() && LW(sig + 8) && LW(r + RACER_CHAR) == CHAR_SILVER) esp_others(c, r, 2);
		break;
	}
	case HOOK_RACER_OPPONENT: {   // F_b1924(racer)
		if (ENV_ON("SR2_OPP_TRACE")) { static u32 seen[256]; int i = 0; while (i < 256 && seen[i] && seen[i] != c->r[31]) i++;
			if (i < 256 && !seen[i]) { seen[i] = c->r[31]; LOGI("opponent asked from %08x (F %06x) for racer %08x vb %llu", c->r[31], c->r[31] - 0x08800000, A(0), (unsigned long long)g_vblank_count); } }
		if (sig_mp()) { u32 t = sig_opponent(A(0), c->r[31]); if (t) { c->r[2] = t; return; } }
		u32 t = smart_target(c, A(0));
		if (t) { c->r[2] = t; return; }
		break;
	}
	case HOOK_SOUND_PLAY_AT:   // F_d120(sound, owner, position, ...): with a position the sound fades with the distance
		if (c->r[31] != 0x089270fc) break;   // to the listener (player 1's camera), without one it plays as it is.
		{                                     // A voice line (from F_127080) of players 2-4 is for everyone to hear
			int k = player_index(A(1)) - 1;   // (their deaths too, wherever they are on the course)
			if (k >= 0 && k < n_extra && !extra_cpu[k] && mp_active()) c->r[6] = 0;
		}
		if (voice_trace) {
			u32 snd = A(0), who = A(1), at = A(2);
			orig(c);
			LOGI("voice play: sound %08x owner %08x (p%d)%s -> %d", snd, who, player_index(who) + 1, at ? "" : " for everyone", (int)c->r[2]);
			return;
		}
		break;
	case HOOK_VOICE_PICK:   // F_126efc(voice, kind, ...): at the start (kind 7, from F_e1xxx) player 1 says a line;
		// players 2-4 say one of theirs too, one after another (voice+0x204: seconds until it is said)
		if (voice_trace) LOGI("voice: racer %08x (p%d) kind %u from %08x", LW(A(0)), player_index(LW(A(0))) + 1, A(1), c->r[31]);
		if (n_extra && !race_ending && !banter_done && A(1) == 7 && LW(c->r[29]) == 0x088e2194 && LW(A(0)) == p1_racer) {
			orig(c);
			Ctx save = *c;
			for (int k = 0; k < n_extra; k++) {
				u32 v = extra_pending[k] || extra_seated[k] ? 0 : LW(extra_racer[k] + 0x4ec);
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
	case HOOK_BANTER_CHECK:   // F_81944(mode)
		if (c->r[31] == BANTER_CHECK_RA) {
			banter_stop(); banter_done = false;
			if (!n_extra || !mp_active() || ENV_ON("SR2_NO_BANTER")) break;
			banter_n = banter_next = 0;
			banter_racer[banter_n++] = LW(WORLD + 0x5024);
			for (int k = 0; k < n_extra; k++) if (!extra_pending[k]) banter_racer[banter_n++] = extra_racer[k];
			if (!banter_racer[0] || banter_n < 2) break;
			banter_on = banter_done = true;
			memcpy(banter_dist, PTR(BANTER_SHOT), 8);   // closer than the story's two-shot: the others stand 0.6 apart
			float d[2] = { banter_dist[0] * BANTER_ZOOM, banter_dist[1] * BANTER_ZOOM };
			memcpy(PTR(BANTER_SHOT), d, 8);
			LOGI("banter: %d racers", banter_n);
			c->r[2] = 1;
			return;
		}
		break;
	case HOOK_WORLD_RACER: {   // F_3731c(world, i)
		u32 ra = c->r[31];
		if (ra == DEATH_FADE_RA && mp_active() && n_extra) { c->r[2] = 0; return; }   // no fade for player 1 either
		if (!banter_on) break;
		if (ra == BANTER_FIRST_RA || ra == BANTER_NEXT_RA) {
			c->r[2] = banter_racer[banter_next < banter_n ? banter_next : banter_n - 1];
			banter_next++;
			return;
		}
		if (ra == BANTER_OTHER1_RA || ra == BANTER_OTHER2_RA) {   // the shot looks past the player before (or after)
			int k = banter_next - 2;
			c->r[2] = banter_racer[k >= 0 ? k : 1];
			return;
		}
		break;
	}
	case HOOK_MENU_GOTO:   // F_14cb34(menus, hash, transition): Main Menu > Multiplayer goes to the local race setup
		if (ENV_ON("SR2_EV_TRACE")) LOGI("menus: %08x -> %08x", menu_hash(), A(1));
		if (A(1) == SCR_MULTI && menu_hash() == SCR_MAIN && !lobby.open) { c->r[5] = SCR_SINGLE; lobby_begin(); }
		if (A(1) == SCR_RIVAL && mp_mode && g_cfg.mp_players > 1 && mac == MAC_IDLE) { rival_from = menu_hash(); mac = MAC_RIVAL; mac_t = 0; mac_tries = 0; }
		break;
	case HOOK_ENTITY_HANDLE:   // F_15f84(entity): see hooks.json; the handle of an extra racer as for slot 15
		if (A(0) && extra_index(A(0)) >= 0 && !extra_seated[extra_index(A(0))]) {
			u32 b = LW(A(0) + 4), s = LW(b + 0x10);
			SW(b + 0x10, 15); orig(c); SW(b + 0x10, s);
			return;
		}
		break;
	case HOOK_RACER_FINISH: {   // F_b2530(racer)
		if (!n_extra || race_ending) break;
		if (bt_mp()) {   // the battle is over for everyone
			bt_placings(c, player_index(A(0)));
			p1_finish(c, orig);
			return;
		}
		if (boss_mp()) {   // someone has the hits: the fight is over for everyone
			if (boss_winner < 0) boss_winner = player_index(A(0)) >= 0 ? player_index(A(0)) : 0;
			boss_placings(c);
			LOGI("boss over: P%d wins", boss_winner + 1);
			p1_finish(c, orig);
			return;
		}
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
		if (ENV_ON("SR2_UPDATE_AS_P1") && extra_index(A(0)) >= 0) {
			u32 p1 = LW(WORLD + 0x5024), f = LW(WORLD + 0x5028);
			SW(WORLD + 0x5024, A(0)); SW(WORLD + 0x5028, A(0));
			orig(c);
			SW(WORLD + 0x5024, p1); SW(WORLD + 0x5028, f);
			return;
		}
		break;
	case HOOK_HUD_DRAW:   // F_12ad44(hud)
		if (split_view_on()) {   // one HUD per view, scaled into it like the view
			views_compose();   // (in case the frame had no see-through pass)
			Ctx save = *c;
			bool own = bt_mp() && hudv_ok;   // the battles: each view's own HUD
			static u8 canon[HUD_SIZE];
			if (own) { hud_merge(); memcpy(canon, PTR(HUD_BASE), HUD_SIZE); }
			u32 p1 = LW(WORLD + 0x5024), hud = A(0), rings = LW(hud + HUD_RINGS);
			int nv = n_views();
			for (int v = 0; v < nv; v++) {
				int k = view_player[v];
				if (own) memcpy(PTR(HUD_BASE), hudv[k], HUD_SIZE);
				bool seated = own && bt_view_begin(k) >= 0;
				if (k) {   // the boost bar reads the local player as it draws; the ring count is kept by the HUD
					u32 r = extra_racer[k - 1];
					float n = (float)(s32)LW(r + RACER_RINGS);
					SW(WORLD + 0x5024, r); memcpy(PTR(hud + HUD_RINGS), &n, 4);
				}
				hud_pass = HUD_PASS_VIEW;
				if (boss_mp()) boss_view(k, true);
				ge_region_scaled(v, true); orig(c);   // halves too get the whole HUD, at half size
				rt_call(c, 0x08800000 + HUD_DRAW_TEXT);   // its text now, in its view (the HUD draws it next time)
				if (boss_mp()) boss_view(k, false);
				if (seated) bt_view_end();
				if (own) memcpy(hudv[k], PTR(HUD_BASE), HUD_SIZE);
				*c = save;
			}
			if (own) memcpy(PTR(HUD_BASE), canon, HUD_SIZE);
			SW(WORLD + 0x5024, p1); SW(hud + HUD_RINGS, rings);
			ge_region(-1);
			if (boss_mp() || bt_mp()) goto hud_timer;   // the bosses and battles: each view has its own counters, no race meter
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
		hud_timer:
			ge_place(HUD_SHARED_SCALE, HUD_TIMER_DY);   // the timer, just above the meter
			hud_pass = HUD_PASS_TIMER; orig(c);
			rt_call(c, 0x08800000 + HUD_DRAW_TEXT);
			*c = save;
			hud_pass = HUD_PASS_ALL;
			ge_region(-1);
			if (own) memcpy(hud_ref, PTR(HUD_BASE), HUD_SIZE);
			return;
		}
		break;
	case HOOK_SEND_EVENT:   // F_16c388(manager, target, source, hash, ...)
		if (ENV_ON("SR2_EV_TRACE") && A(1) && A(1) == LW(WORLD + 0x5024)) {   // test aid: events the local player gets
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
		if (!n_extra) break;
		int was = sctx_use(sctx_racer(r));
		if (r != (s1 ? s1 : s0)) { racer_step(c, orig, r); sctx_use(was); return; }
		Ctx save = *c;
		racer_step(c, orig, r);
		sctx_use(was);
		Ctx out = *c;
		for (int k = 0; k < n_extra; k++) {   // then the extra racers that have no seat
			u32 e = extra_racer[k];
			if (e == s0 || e == s1 || extra_pending[k]) continue;
			u32 i0 = LW(WORLD + 0x4f84);
			SW(WORLD + 0x502c, e); SW(WORLD + 0x4f84, e);   // it borrows seat 0 meanwhile
			was = sctx_use(k + 1);
			*c = save; racer_step(c, orig, e);
			sctx_use(was);
			SW(WORLD + 0x502c, s0); SW(WORLD + 0x4f84, i0);
		}
		*c = out;
		return;
	}
	case HOOK_TRACK_ENTITY_SECTION:   // F_1691b4(entity): racers are exempt, so is the extra one
		// keep its section index current (the start of F_1691b4). Player 1 too while the streaming follows someone
		// else: F_15f97c sets the followed section into the local player's index, which their camera goes by
		if ((extra_index(A(0)) >= 0 && !extra_seated[extra_index(A(0))]) || (stream_mp() && A(0) == p1_racer)) {
			u32 e = A(0), body = LW(e + 4);
			Ctx save = *c;
			int was = sctx_use(player_index(e));   // among the sections of their own context
			c->r[4] = WORLD; c->r[5] = e; c->r[6] = 0; c->r[7] = 0;
			rt_call(c, 0x08800000 + 0x15e0e8);
			s32 sec = c->r[2];
			sctx_use(was);
			*c = save;
			if (sec > 0 && sec <= (s32)LW(WORLD + 0x4d10)) { SW(body + 0xc, sec); if (ENV_ON("SR2_OLD_SEC")) SW(body + 0x10, sec); }   // +0x10 stays its slot, as a racer's
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
			cam_ready[k] = false; cam_nzone[k] = 0;
			u32 pr = LW(WORLD + 0x5024), eb = LW(r + 4);   // the local player (F_3731c(world, 0))
			if (!pr) pr = A(0);
			u32 pb = LW(pr + 4);
			Ctx save = *c;
			bool cpu = extra_cpu[k] || ENV_ON("SR2_MP_CPU");   // a bot (or, test aid, every player 2-4)
			if (extra_seated[k]) {   // the rival: already in the race, only its driver changes to player 2
				if (k == 0) placings_reset(pr);
				if (k == 0 && ENV_ON("SR2_P1_CPU") && LW(pr + 0x8c)) {   // test aid: the computer drives player 1 too
					c->r[4] = LW(LW(pr + 0x8c) + 4); c->r[5] = 2; c->r[6] = -2; rt_call(c, 0x08800000 + 0x17138); *c = save;
					c->r[4] = pr; c->r[5] = 1; rt_call(c, 0x08800000 + 0xade6c); *c = save;
				}
				if (cpu) continue;
				memcpy(PTR(PAD_DEVN(k)), PTR(PAD_DEV), PAD_DEV_SIZE);
				u32 in = LW(r + 0x8c) ? LW(LW(r + 0x8c) + 4) : 0;
				if (in) { c->r[4] = in; c->r[5] = 2; c->r[6] = 1; rt_call(c, 0x08800000 + 0x17138); *c = save; }
				c->r[4] = r; c->r[5] = 0; rt_call(c, 0x08800000 + 0xade6c);
				*c = save;
				LOGI("player 2 drives the rival %08x, input %08x", r, in);
				continue;
			}
			u32 seat0 = LW(WORLD + 0x502c), info0 = LW(WORLD + 0x4f84);
			SW(WORLD + 0x502c, r); SW(WORLD + 0x4f84, r);   // seated while it is switched on (kept by the first one in Free Play)
			c->r[4] = r; c->r[5] = 1; c->r[6] = 2;
			rt_call(c, 0x08800000 + 0x16c0b0);
			*c = save;
			extra_racer_place(c, r, pr, k);
			if (k == 0) placings_reset(pr);
			if (k == 0 && ENV_ON("SR2_P1_CPU") && LW(pr + 0x8c)) {   // test aid: the computer drives player 1 too
				c->r[4] = LW(LW(pr + 0x8c) + 4); c->r[5] = 2; c->r[6] = -2; rt_call(c, 0x08800000 + 0x17138); *c = save;
				c->r[4] = pr; c->r[5] = 1; rt_call(c, 0x08800000 + 0xade6c); *c = save;
			}
			u32 ctl = LW(r + 0x480);   // its controller: reset it, as the racer's start step (F_aff70) does first
			c->r[4] = ctl; rt_call(c, LW(LW(ctl) + 8));
			*c = save;
			memcpy(PTR(PAD_DEVN(k)), PTR(PAD_DEV), PAD_DEV_SIZE);   // its player's pad device starts as a copy of the game's
			u32 in = LW(r + 0x8c) ? LW(LW(r + 0x8c) + 4) : 0;   // its input block: set up like F_159194 does for a
			if (in) {                                            // human racer (F_17138: button map, source), source 1;
				c->r[4] = in; c->r[5] = 2; c->r[6] = cpu ? -2 : 1;   // a bot's, like the computer rival's: -2, no pad
				rt_call(c, 0x08800000 + 0x17138);
				*c = save;
			}
			c->r[4] = r; c->r[5] = cpu; rt_call(c, 0x08800000 + 0xade6c);   // not computer-driven (F_ade6c, as F_159194)
			*c = save;
			c->r[4] = r; c->r[5] = 0; c->r[6] = 0; rt_call(c, LW(LW(r) + 0x38));   // its race-start step (F_aff70)
			*c = save;
			// None keeps a seat: with player 2 in Free Play's empty rival seat, player 1 could not move. All are
			// stepped after the seated racers instead (HOOK_RACER_STEP).
			SW(WORLD + 0x502c, seat0); SW(WORLD + 0x4f84, info0);
			LOGI("extra racer %d: switched on %08x (body %08x), player body %08x, input %08x, racers in race: %u", k, r, eb, pb, in, PTR(0x08A7C69C)[0]);
		}
		if (in_race) {   // human racers only: this move step also runs for other moving objects
			int ri = A(0) == (p1_racer ? p1_racer : LW(WORLD + 0x5024)) ? 0 : -1;   // (not +0x5024 alone: an extra
			for (int k = 0; k < n_extra && ri < 0; k++) if (A(0) == extra_racer[k]) ri = k + 1;   // racer is it in its physics)
			if (ri >= 0) path_resnap(c, A(0), ri);
			if (ri >= 0) stream_keep_up(c, A(0), ri);
			if (ri >= 0 && (ri ? extra_cpu[ri - 1] : ENV_ON("SR2_P1_CPU"))) bot_unstick(c, A(0), ri);
			u32 p1 = LW(WORLD + 0x5024);
			if (p1 && LW(A(0)) == LW(p1)) boost_logs(c, A(0));   // every racer, the computer's too
			if (ri == 0) r1_event_stubs();
			// Rivals 1 stages turn sharp corners (a wall onto a ledge) in path records under a unit long; a
			// racer crossing two of them in one step has its speed projected straight onto the last one and
			// loses it, so racers there move in four shorter steps
			if (ri >= 0 && g_cfg.fps <= 60 && LW(WORLD + 0x57f0) == R1_STAGE_MAGIC) {
				float dt = c->f[12];
				Ctx save = *c;
				for (int s = 0; s < 4; s++) { *c = save; c->f[12] = dt / 4; orig(c); }
				return;
			}
		}
		if (in_race && g_cfg.fps > 60) { racer_move(c, orig); return; }
		break;
	case HOOK_SEC_INIT: {   // F_15ea70(world, slot): made already for another context: only this one's bookkeeping
		int k = (s32)LW(A(1) + 0x20), b = (k - 1) & 3;
		if (!sctx_held(k, sctx_in)) { sec_lights_for(b, k); break; }
		SW(WORLD + 0x5d20 + 16 * b, 0x20); SB(WORLD + 0x5d24 + 16 * b, 1);
		SW(WORLD + 0x5d7c + 4 * b, LW(WORLD + 0x4c90 + 4 * k)); SW(WORLD + 0x5d8c + 4 * b, LW(WORLD + 0x4cd0 + 4 * k));
		return;
	}
	case HOOK_SEC_RELEASE: {   // F_15f274(world, buffer, flag): another context still has it: only this one's bookkeeping
		u32 e = WORLD + 0x5d1c + 16 * A(1), sp = LW(e), st = LW(e + 4);
		if (A(1) <= 3 && sp) sec_lights_for(A(1), (s32)LW(sp + 0x20));
		if (A(1) > 3 || !sp || (st != 0x20 && st != 2) || !sctx_held((s32)LW(sp + 0x20), sctx_in)) break;
		SW(e + 4, 1); SB(e + 8, 0); SW(e, 0);
		return;
	}
	case HOOK_SEC_READ_DONE: {   // F_159128(?, ?, section): into the context that asked for the read
		int k = (s32)A(2), j = -1;
		if (!sctx_on || k < 1 || k > MAX_SECTIONS) break;
		while (sctx_nrd[k] && j < 0) {
			int q = sctx_rd[k][0];
			memmove(sctx_rd[k], sctx_rd[k] + 1, --sctx_nrd[k] * sizeof sctx_rd[k][0]);
			if (q <= n_extra && (sctx_state(q, k) == 8 || sctx_state(q, k) == 0x100)) j = q;
		}
		for (int q = 0; q <= n_extra && j < 0; q++) if (sctx_state(q, k) == 8 || sctx_state(q, k) == 0x100) j = q;
		if (j < 0) break;
		int was = sctx_use(j); orig(c); sctx_use(was);
		return;
	}
	case HOOK_STREAM_SECTION:   // F_15e0e8(world, focus racer, 0), from F_15f97c: the section to stream around
		if (!stream_mp()) { stream_leader = 0; if (stream4) stream_links(false); break; }
		stream_links(true);
		c->r[5] = stream_pick(c);
		orig(c);
		{   // only four sections fit (one behind, two ahead): with someone behind, the leader keeps one ahead and
			int s = (s32)c->r[2];   // the window reaches two back. The course objects (springs among them) only work
			bool behind = false;    // in the active sections, the current one and its first two neighbours, so with
			                        // the window back a player one section behind the leader still has them working
			for (int k = 0; k <= n_extra; k++) if (!finish_place[k] && stream_sec[k] >= 1 && stream_sec[k] < s) behind = true;
			if (s > 1 && behind) { c->r[2] = s - 1; SW(STREAM_CUR_SEC, WORLD_SLOT(s - 1)); }   // (and the section the loads go around)
			static int last = -9;
			if (ENV_ON("SR2_STREAM_TRACE") && (int)c->r[2] != last) { last = c->r[2]; LOGI("stream: leader in %d, window around %d", s, last); }
		}
		if (ENV_ON("SR2_STREAM_TRACE")) {
			static u32 seen[4][2];
			for (int b = 0; b < 4; b++) {
				u32 e = STREAM_BUFS + b * 16, sp = LW(e + 4), st = LW(e + 8), sn = sp >= 0x08800000 && sp < 0x0C000000 ? LW(sp + 0x20) : 0;
				if (seen[b][0] != sn || seen[b][1] != st) { seen[b][0] = sn; seen[b][1] = st; LOGI("stream: buffer %d section %d state %x (cur %d)", b, sn, st, (int)LW(STREAM_CUR)); }
			}
		}
		return;
	case HOOK_STREAM_BUSY:   // F_1604ec(world, focus racer), from F_15f97c
		if (stream_mp() && stream_leader) c->r[5] = stream_leader;
		break;
	case HOOK_BOSS_INIT:   // F_c0044(boss)
		boss_obj = A(0); boss_winner = -1;
		orig(c); memset(boss_hits, 0, sizeof boss_hits);
		LOGI("boss %08x: %08x and %08x", boss_obj, LW(boss_obj + BOSS_P1), LW(boss_obj + BOSS_RIVAL));
		return;
	case HOOK_BOSS_HIT: boss_hit(c, orig); return;
	case HOOK_BT_START: case HOOK_BT_RESTART:   // F_80ec4, F_80fa8 (battle)
		orig(c); bt_reset();
		return;
	case HOOK_BT_UPDATE:   // F_81080(battle, f12 = dt)
		if (bt_mp()) bt_frame(c);
		break;
	case HOOK_BT_SCORE: case HOOK_BT_PARAM: case HOOK_BT_EVENT13: case HOOK_BT_EVENT14: case HOOK_BT_EVENT12: case HOOK_BT_EVENT:
		bt_dispatch(c, orig);
		return;
	case HOOK_CTC_BASE:   // F_103fc0(base, racer)
		if (bt_mp() && player_index(A(1)) >= 2) {
			u32 b = A(0), own = LW(b + CTC_BASE_OWNER);
			SW(b + CTC_BASE_OWNER, A(1)); orig(c); SW(b + CTC_BASE_OWNER, own);
			return;
		}
		break;
	case HOOK_HUD_UPDATE:   // F_129380(hud, f12, f13 = dt)
		if (bt_mp() && split_view_on()) { bt_hud_update(c, orig); return; }
		break;
	case HOOK_BOSS_MOST_HITS:   // F_c0d30(boss)
		if (A(0) == boss_obj && boss_obj && n_extra) { orig(c); int m = boss_best_but(-1); if ((int)c->r[2] < m) c->r[2] = m; return; }
		break;
	case HOOK_STORY_PROGRESS:   // F_13a63c(profile): test aid, every zone and act open in Single Event
		if (ENV_ON("SR2_UNLOCK_ALL")) { c->r[2] = 255; return; }
		break;
	case HOOK_OUTFIT_OWNED:   // F_1393f0(profile, i)
		// the Change Outfit list asks for outfit k (1-4) of character ch as i = (k-1)*8 + ch
		if (g_cfg.unlock_outfits && c->r[31] == OUTFIT_MENU_RA && A(1) < 32) { c->r[2] = 1; return; }
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
	if (trace && !n_extra && in_race && g_vblank_count % 30 == 0)   // test aid: single player, both seats
		for (int k = 0; k < 2; k++) {
			u32 r = LW(WORLD + 0x502c + k * 4); float *pos = r && LW(r + 4) ? (float *)PTR(LW(r + 4) + 0x20) : NULL;
			if (pos) LOGI("sp: seat %d %08x (%.1f %.1f %.1f)", k, r, pos[0], pos[1], pos[2]);
		}
	if (trace && n_extra && g_vblank_count % 30 == 0) {   // test aid: where each player and camera is
		char line[512]; int o = 0;
		u32 rs[4] = {LW(WORLD + 0x5024)};
		for (int k = 0; k < n_extra; k++) rs[k + 1] = extra_racer[k];
		for (int k = 0; k <= n_extra; k++) {
			u32 r = rs[k]; float *pos = r && LW(r + 4) ? (float *)PTR(LW(r + 4) + 0x20) : NULL;
			u32 st = r && LW(r + 0x88) ? LW(LW(r + 0x88) + 0xac) : 0;
			u32 bd = r ? LW(r + 4) : 0, lc = r ? r + 0x114 : 0;
			o += snprintf(line + o, sizeof line - o, " p%d %08x (%.1f %.1f %.1f) %08x s%d/%d loc %d:%d>%d e%08x", k + 1, r, pos ? pos[0] : 0, pos ? pos[1] : 0, pos ? pos[2] : 0, st,
			              bd ? (int)LW(bd + 0xc) : -9, bd ? (int)LW(bd + 0x10) : -9, lc ? (s8)LB(lc + 0xc) : -9, lc ? (s16)LH(lc + 0xa) : -9, lc ? (s16)LH(lc + 8) : -9, r ? LW(r + 0x8e4) : 0);
		}
		u32 cam = LW(CAM_MAIN);
		LOGI("mp:%s spring %08x cam target %08x seats %08x %08x", line, LW(0x08AB4B90), cam ? LW(cam + CAM_TARGET) : 0, LW(WORLD + 0x502c), LW(WORLD + 0x5030));
		o = 0;
		for (int k = 0; k <= n_extra; k++) { u32 r = rs[k]; o += snprintf(line + o, sizeof line - o, " p%d sec %d", k + 1, r && LW(r + 4) ? (int)LW(LW(r + 4) + 0xc) : -9); }
		for (u32 i = 0; i < LW(0x0899FF4C) && i < 8; i++) { u32 e = WORLD + 0x5d18 + i * 16, sp = LW(e + 4); o += snprintf(line + o, sizeof line - o, " buf%u st %x slot %d", i, LW(e + 8), sp >= 0x08800000 && sp < 0x0C000000 ? (int)LW(sp + 0x20) : -1); }
		LOGI("mp sections: cur %d n %d%s", LW(WORLD + 0x5da0), LW(WORLD + 0x4d10), line);
	}
}

