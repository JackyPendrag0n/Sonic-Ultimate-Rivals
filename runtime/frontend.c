// Host front end: SDL2 + OpenGL window (both renderers present through GL), input, audio, the in-game
// settings menu (menu.cpp), screenshots, and headless/scripted operation for testing.
//
// Environment (testing):
//   RCSM_HEADLESS=1          hidden window, no audio. Without a display, falls back to the software renderer.
//   RCSM_SHOT_EVERY=N        write <shot dir>/frame_NNNNNN.png every N vblanks;  RCSM_SHOT_DIR=dir
//   RCSM_INPUT="f:BTN+BTN,f:-,..."  scripted input from vblank f ('-' = release; SUP/SDOWN/SLEFT/SRIGHT = stick)
//   RCSM_DUMP_AT="f1,f2"     dump guest RAM 0x08800000-0x0A000000 at those vblanks
#define _GNU_SOURCE
#include "kernel.h"
#include "config.h"
#include "gpu/ge_soft.h"
#include <SDL2/SDL.h>
#include <GL/gl.h>
// GL entry points are fetched through SDL so the binary doesn't link libGL directly.
static void (*p_glViewport)(GLint, GLint, GLsizei, GLsizei);
static void (*p_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glClear)(GLbitfield);
static void (*p_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
#define glViewport p_glViewport
#define glClearColor p_glClearColor
#define glClear p_glClear
#include <zlib.h>
#include <sys/stat.h>
#include <math.h>

// menu.cpp
void menu_init(SDL_Window *w, void *glctx);
bool menu_event(SDL_Event *e);
bool menu_is_open(void);
void menu_set_open(bool open);
void menu_draw(int w, int h, float fps, float game_fps);
bool menu_quit_requested(void);

u32 fe_buttons;
bool g_display_dirty;
u8 fe_lx = 128, fe_ly = 128;
bool fe_fly_down, fe_fly_fast;      // debug flying: Left Ctrl = down, Left Shift / left stick click = fast
float fe_cam_x, fe_cam_y;          // camera stick (right stick, keys R/F/Q/E), -1..1, y > 0 = look up
static u32 kb_buttons, pad_buttons, script_buttons;
static int script_ax, script_ay, script_rx, script_ry;
static bool script_strafe;
bool fe_strafe;                    // strafe mode active (L2/R2 or Z/C, held or toggled)
bool fe_aim;                       // modern aiming: aim held (L2, Z/C, right mouse button)
float fe_mouse_dx, fe_mouse_dy;    // mouse look since the last game frame (pixels), consumed by mods.c
static bool mouse_aim, mouse_fire, mouse_grabbed;
static bool kb_strafe, kb_cam_up, kb_cam_down, strafe_btn_prev, strafe_toggled;
static int kb_ax, kb_ay;

static SDL_Window *win;
static SDL_GLContext glctx;
static SDL_AudioDeviceID audio_dev;
static SDL_GameController *pad;
static bool headless, have_gl;
static u32 fb_top, fb_stride, fb_fmt;
static bool fb_set;
static int shot_every; static u64 shot_from;   // RCSM_SHOT_FROM=vblank: no shots before it
static char shot_dir[256] = "shots";
static u32 rgba[480 * 272];
static u64 flips;   // display buffer changes (game frames)

typedef struct { u64 frame; u32 buttons; int ax, ay, rx, ry; bool strafe; } ScriptEv;
static ScriptEv *script;
static int nscript, script_pos;

enum { B_SELECT = 0x1, B_START = 0x8, B_UP = 0x10, B_RIGHT = 0x20, B_DOWN = 0x40, B_LEFT = 0x80, B_L = 0x100, B_R = 0x200,
       B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000, B_CROSS = 0x4000, B_SQUARE = 0x8000, B_HOME = 0x10000 };

static u32 parse_buttons(const char *s) {
	static const struct { const char *n; u32 b; } names[] = {
		{"SELECT", B_SELECT}, {"START", B_START}, {"UP", B_UP}, {"RIGHT", B_RIGHT}, {"DOWN", B_DOWN}, {"LEFT", B_LEFT},
		{"L", B_L}, {"R", B_R}, {"TRIANGLE", B_TRIANGLE}, {"CIRCLE", B_CIRCLE}, {"CROSS", B_CROSS}, {"SQUARE", B_SQUARE}};
	u32 b = 0;
	char tmp[128];
	snprintf(tmp, sizeof tmp, "%s", s);
	for (char *save, *t = strtok_r(tmp, "+", &save); t; t = strtok_r(NULL, "+", &save))
		for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
			if (!strcasecmp(t, names[i].n)) b |= names[i].b;
	return b;
}
static void parse_stick(const char *s, int *ax, int *ay) {
	*ax = *ay = 0;
	if (strcasestr(s, "SUP")) *ay = -127;
	if (strcasestr(s, "SDOWN")) *ay = 127;
	if (strcasestr(s, "SLEFT")) *ax = -127;
	if (strcasestr(s, "SRIGHT")) *ax = 127;
}
static void parse_rstick(const char *s, int *rx, int *ry) {   // RUP/RDOWN/RLEFT/RRIGHT = right stick
	*rx = *ry = 0;
	if (strcasestr(s, "RUP")) *ry = -127;
	if (strcasestr(s, "RDOWN")) *ry = 127;
	if (strcasestr(s, "RLEFT")) *rx = -127;
	if (strcasestr(s, "RRIGHT")) *rx = 127;
}

// ---------------------------------------------------------------- frames, screenshots
static void convert_fb(void) {
	if (have_gl) { if (ge_read_display((uint8_t *)rgba, 480, 272) != 0) memset(rgba, 0, sizeof rgba); return; }
	if (!fb_set) { memset(rgba, 0, sizeof rgba); return; }
	ge_soft_flush();
	for (int y = 0; y < 272; y++) {
		u32 *dst = rgba + y * 480;
		if (fb_fmt == 3) {
			const u32 *src = (const u32 *)PTR(fb_top + y * fb_stride * 4);
			for (int x = 0; x < 480; x++) dst[x] = src[x] | 0xFF000000u;
		} else {
			const u16 *src = (const u16 *)PTR(fb_top + y * fb_stride * 2);
			for (int x = 0; x < 480; x++) {
				u16 p = src[x];
				u32 r, g, b;
				if (fb_fmt == 0) { r = (p & 31) << 3; g = ((p >> 5) & 63) << 2; b = (p >> 11) << 3; r |= r >> 5; g |= g >> 6; b |= b >> 5; }
				else if (fb_fmt == 1) { r = (p & 31) << 3; g = ((p >> 5) & 31) << 3; b = ((p >> 10) & 31) << 3; r |= r >> 5; g |= g >> 5; b |= b >> 5; }
				else { r = (p & 15) * 17; g = ((p >> 4) & 15) * 17; b = ((p >> 8) & 15) * 17; }
				dst[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
			}
		}
	}
}

static void png_chunk(FILE *f, const char *type, const u8 *data, u32 len) {
	u8 hdr[8] = {len >> 24, len >> 16, len >> 8, len, type[0], type[1], type[2], type[3]};
	fwrite(hdr, 1, 8, f);
	if (len) fwrite(data, 1, len, f);
	u32 crc = crc32(0, hdr + 4, 4);
	crc = crc32(crc, data, len);
	u8 c[4] = {crc >> 24, crc >> 16, crc >> 8, crc};
	fwrite(c, 1, 4, f);
}

static void write_png(const char *path, const u32 *px, int w, int h) {
	FILE *f = fopen(path, "wb");
	if (!f) return;
	static const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
	fwrite(sig, 1, 8, f);
	u8 ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 6, 0, 0, 0};
	png_chunk(f, "IHDR", ihdr, 13);
	size_t rawlen = (size_t)h * (1 + w * 4);
	u8 *raw = malloc(rawlen);
	for (int y = 0; y < h; y++) { raw[y * (1 + w * 4)] = 0; memcpy(raw + y * (1 + w * 4) + 1, px + (size_t)y * w, w * 4); }
	uLongf zl = compressBound(rawlen);
	u8 *z = malloc(zl);
	compress2(z, &zl, raw, rawlen, 6);
	png_chunk(f, "IDAT", z, zl);
	png_chunk(f, "IEND", NULL, 0);
	fclose(f);
	free(raw); free(z);
}

void fe_write_png(const char *path) { convert_fb(); write_png(path, rgba, 480, 272); }

static void screenshot_hd(void) {
	char p[300];
	mkdir(shot_dir, 0755);
	snprintf(p, sizeof p, "%s/screenshot_%llu.png", shot_dir, (unsigned long long)g_vblank_count);
	int s = have_gl ? g_cfg.internal_scale : 1, w = 480 * s, h = 272 * s;
	u32 *px = malloc((size_t)w * h * 4);
	if (have_gl && ge_read_display((uint8_t *)px, w, h) == 0) write_png(p, px, w, h);
	else fe_write_png(p);
	free(px);
	LOGI("screenshot %s", p);
}

void fe_set_display(u32 top, u32 stride, u32 fmt) {
	if (top != fb_top) flips++;
	fb_top = top & 0x0FFFFFFF; fb_stride = stride; fb_fmt = fmt; fb_set = top != 0;
	ge_soft_set_display(top, stride, fmt);
}

// ---------------------------------------------------------------- input
static void set_fullscreen(bool on) {
	if (win) SDL_SetWindowFullscreen(win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static u32 key_to_button(SDL_Keycode k) {
	switch (k) {
	case SDLK_UP: return B_UP; case SDLK_DOWN: return B_DOWN; case SDLK_LEFT: return B_LEFT; case SDLK_RIGHT: return B_RIGHT;
	case SDLK_SPACE: case SDLK_k: return B_CROSS;
	case SDLK_l: return B_CIRCLE;
	case SDLK_j: return B_SQUARE;
	case SDLK_i: return B_TRIANGLE;
	case SDLK_q: return B_L;
	case SDLK_e: return B_R;
	case SDLK_z: return (g_cfg.strafe_mode || g_cfg.modern_aim) ? 0 : B_LEFT;    // strafe/aim (or the PSP d-pad strafe when off)
	case SDLK_c: return (g_cfg.strafe_mode || g_cfg.modern_aim) ? 0 : B_RIGHT;
	case SDLK_RETURN: return B_START;
	case SDLK_BACKSPACE: case SDLK_TAB: return B_SELECT;
	default: return 0;
	}
}

static int kb_w, kb_a, kb_s, kb_d;
static bool kb_fly_down, kb_fly_fast;

static float pad_cam_x, pad_cam_y;
static bool pad_strafe_btn;
static void poll_input(void) {
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		if (e.type == SDL_QUIT) { LOGI("window closed"); config_save(); exit(0); }
		if (e.type == SDL_KEYDOWN && !e.key.repeat) {
			SDL_Keycode k = e.key.keysym.sym;
			if (k == SDLK_F1 || k == SDLK_ESCAPE) { menu_set_open(!menu_is_open()); continue; }
			if (k == SDLK_F11) { g_cfg.fullscreen = !g_cfg.fullscreen; set_fullscreen(g_cfg.fullscreen); config_save(); continue; }
			if (k == SDLK_F12) { screenshot_hd(); continue; }
			{	// debug toggles: F5 god mode, F6 noclip, F7 free camera
				extern bool g_god, g_noclip, g_freecam;
				extern bool debug_in_level(void);
				if ((k == SDLK_F5 || k == SDLK_F6 || k == SDLK_F7) && debug_in_level()) {
					bool *t = k == SDLK_F5 ? &g_god : k == SDLK_F6 ? &g_noclip : &g_freecam;
					*t = !*t;
					LOGI("debug: %s %s", k == SDLK_F5 ? "god mode" : k == SDLK_F6 ? "noclip" : "free camera", *t ? "on" : "off");
					continue;
				}
				extern bool g_play_clank;
				extern bool g_skyboard;
				if (k == SDLK_F9 && debug_in_level()) { g_skyboard = !g_skyboard; LOGI("skyboard %s", g_skyboard ? "on" : "off"); continue; }
				if (k == SDLK_F8) { g_play_clank = !g_play_clank; LOGI("play as Clank %s", g_play_clank ? "on" : "off"); continue; }
			}
		}
		if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) { menu_set_open(!menu_is_open()); continue; }
		if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) { pad = SDL_GameControllerOpen(e.cdevice.which); continue; }
		if (e.type == SDL_CONTROLLERDEVICEREMOVED && pad && !SDL_GameControllerGetAttached(pad)) { SDL_GameControllerClose(pad); pad = NULL; continue; }
		if (menu_is_open()) { menu_event(&e); continue; }
		if (g_cfg.modern_aim && g_cfg.mouse_look) {
			if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
				bool down = e.type == SDL_MOUSEBUTTONDOWN;
				if (e.button.button == SDL_BUTTON_RIGHT) mouse_aim = down;
				if (e.button.button == SDL_BUTTON_LEFT) mouse_fire = down;
				continue;
			}
			if (e.type == SDL_MOUSEMOTION && mouse_grabbed) { fe_mouse_dx += e.motion.xrel; fe_mouse_dy += e.motion.yrel; continue; }
		}
		if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
			bool down = e.type == SDL_KEYDOWN;
			SDL_Keycode k = e.key.keysym.sym;
			if (k == SDLK_w) kb_w = down;
			if (k == SDLK_a) kb_a = down;
			if (k == SDLK_s) kb_s = down;
			if (k == SDLK_d) kb_d = down;
			if (k == SDLK_z || k == SDLK_c) kb_strafe = down;
			if (k == SDLK_r) kb_cam_up = down;
			if (k == SDLK_LCTRL) kb_fly_down = down;
			if (k == SDLK_LSHIFT) kb_fly_fast = down;
			if (k == SDLK_f) kb_cam_down = down;
			u32 b = key_to_button(k);
			if (down) kb_buttons |= b; else kb_buttons &= ~b;
		}
	}
	kb_ax = (kb_d - kb_a) * 127;
	kb_ay = (kb_s - kb_w) * 127;
	pad_buttons = 0;
	static bool pad_fly_fast;
	if (!pad) pad_fly_fast = false;
	int ax = kb_ax, ay = kb_ay;
	float cx = 0, cy = (float)(kb_cam_up - kb_cam_down);
	bool strafe_btn = kb_strafe;
	if (pad && !menu_is_open()) {
		static const struct { int sb; u32 b; } map[] = {
			{SDL_CONTROLLER_BUTTON_A, B_CROSS}, {SDL_CONTROLLER_BUTTON_B, B_CIRCLE}, {SDL_CONTROLLER_BUTTON_X, B_SQUARE},
			{SDL_CONTROLLER_BUTTON_Y, B_TRIANGLE}, {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, B_L}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, B_R},
			{SDL_CONTROLLER_BUTTON_START, B_START}, {SDL_CONTROLLER_BUTTON_BACK, B_SELECT}, {SDL_CONTROLLER_BUTTON_DPAD_UP, B_UP},
			{SDL_CONTROLLER_BUTTON_DPAD_DOWN, B_DOWN}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, B_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, B_RIGHT}};
		for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
			if (SDL_GameControllerGetButton(pad, map[i].sb)) pad_buttons |= map[i].b;
		pad_fly_fast = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK);
		int dz = g_cfg.stick_deadzone * 32767 / 100;
		int px = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), py = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
		if ((long)px * px + (long)py * py > (long)dz * dz) { ax = px / 256; ay = py / 256; }
		bool lt = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000;
		bool rt = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000;
		if (g_cfg.modern_aim) {
			// modern Ratchet: L2 aims (over the shoulder, strafing), R2 fires
			strafe_btn |= lt;
			if (rt) pad_buttons |= B_CIRCLE;
		} else if (g_cfg.strafe_mode) {
			// Ratchet 3 style: L2/R2 put Ratchet into strafe mode (see HOOK_STRAFE_MOVE)
			strafe_btn |= lt || rt;
		} else {
			if (lt) pad_buttons |= B_L;
			if (rt) pad_buttons |= B_R;
		}
		if (g_cfg.right_stick_camera) {
			// The PSP turns the camera with L/R. The right stick drives it (proportionally in levels, see
			// HOOK_CAM_BUTTONS; elsewhere as the L/R buttons).
			int rx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
			int ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
			if (rx < -16000) pad_buttons |= B_L;
			if (rx > 16000) pad_buttons |= B_R;
			float fx = rx / 32767.0f, fy = -ry / 32767.0f, m = sqrtf(fx * fx + fy * fy), d = g_cfg.stick_deadzone / 100.0f;
			if (m > d && m > 0) {
				float k = (m - d) / (1 - d) / m;   // rescale so the deadzone edge is 0
				if (fabsf(fx) > d * 0.5f) cx = fx * k;
				if (fabsf(fy) > d * 0.5f) cy += fy * k;
			}
		}
	}
	if (ax < -128) ax = -128; if (ax > 127) ax = 127;
	if (ay < -128) ay = -128; if (ay > 127) ay = 127;
	fe_lx = (u8)(ax + 128); fe_ly = (u8)(ay + 128);
	if (cx > 1) cx = 1; if (cx < -1) cx = -1; if (cy > 1) cy = 1; if (cy < -1) cy = -1;
	pad_cam_x = cx; pad_cam_y = cy; pad_strafe_btn = strafe_btn;
	fe_fly_down = kb_fly_down; fe_fly_fast = kb_fly_fast || pad_fly_fast;
	{	// mouse look (modern aiming): grab the pointer while playing
		bool want = g_cfg.modern_aim && g_cfg.mouse_look && !menu_is_open() && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS);
		if (want != mouse_grabbed) { SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE); mouse_grabbed = want; fe_mouse_dx = fe_mouse_dy = 0; }
		if (!want) mouse_aim = mouse_fire = false;
		pad_strafe_btn |= mouse_aim;
		if (mouse_fire) pad_buttons |= B_CIRCLE;
	}
}

// strafe trigger -> strafe mode (hold or toggle), camera stick; also fed by scripted input
static void update_strafe_and_camera(void) {
	bool btn = pad_strafe_btn || script_strafe;
	if (g_cfg.modern_aim) { fe_aim = btn; fe_strafe = false; strafe_toggled = false; strafe_btn_prev = btn; goto cam; }
	fe_aim = false;
	if (g_cfg.strafe_mode == 2) { if (btn && !strafe_btn_prev) strafe_toggled = !strafe_toggled; fe_strafe = strafe_toggled; }
	else { strafe_toggled = false; fe_strafe = g_cfg.strafe_mode == 1 && btn; }
	strafe_btn_prev = btn;
cam:
	fe_cam_x = script_rx ? script_rx / 127.0f : pad_cam_x;
	fe_cam_y = script_ry ? -script_ry / 127.0f : pad_cam_y;
}

// ---------------------------------------------------------------- presentation
static float fps_shown, game_fps_shown;
static bool want_winshot;
static u64 fps_t0, fps_frames, fps_flips0;

static void present(void) {
	if (!have_gl) return;
	int w, h;
	SDL_GL_GetDrawableSize(win, &w, &h);
	int vw = w, vh = h;
	if (g_cfg.keep_aspect) { vh = w * 272 / 480; if (vh > h) { vh = h; vw = h * 480 / 272; } }
	glViewport(0, 0, w, h);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	ge_present((w - vw) / 2, (h - vh) / 2, vw, vh);
	if (!headless) menu_draw(w, h, fps_shown, game_fps_shown);
	if (want_winshot && p_glReadPixels) {
		// test aid: capture the whole window (game + overlay) as presented
		u32 *px = malloc((size_t)w * h * 4), *fl = malloc((size_t)w * h * 4);
		p_glReadPixels(0, 0, w, h, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, px);
		for (int y = 0; y < h; y++) memcpy(fl + (size_t)y * w, px + (size_t)(h - 1 - y) * w, w * 4);
		char p[300];
		mkdir(shot_dir, 0755);
		snprintf(p, sizeof p, "%s/window_%llu.png", shot_dir, (unsigned long long)g_vblank_count);
		write_png(p, fl, w, h);
		free(px); free(fl);
		want_winshot = false;
	}
	SDL_GL_SwapWindow(win);
	fps_frames++;
	u64 n = SDL_GetTicks64();
	if (n - fps_t0 >= 500) {
		fps_shown = fps_frames * 1000.0f / (n - fps_t0);
		game_fps_shown = (flips - fps_flips0) * 1000.0f / (n - fps_t0);
		fps_t0 = n; fps_frames = 0; fps_flips0 = flips;
	}
}

static int menu_test_frames;
static void run_menu_pause(void) {
	// Pause the game while the settings menu is open: keep presenting the last frame + menu.
	u64 t0 = now_us();
	if (audio_dev) SDL_PauseAudioDevice(audio_dev, 1);
	while (menu_is_open()) {
		poll_input();
		if (menu_test_frames && --menu_test_frames == 1) want_winshot = true;
		if (menu_test_frames == 0 && getenv("RCSM_MENU_TEST")) { present(); menu_set_open(false); break; }
		present();
		if (menu_quit_requested()) { config_save(); exit(0); }
		if (!g_cfg.vsync) SDL_Delay(8);
	}
	if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
	SDL_GL_SetSwapInterval(g_cfg.vsync ? 1 : 0);
	kernel_add_pause_time(now_us() - t0);
	kb_buttons = 0; kb_w = kb_a = kb_s = kb_d = 0; kb_strafe = kb_cam_up = kb_cam_down = false;
	kb_fly_down = kb_fly_fast = false;
}

void fe_vblank(void) {
	ge_soft_vblank();
	{	// test aid: RCSM_LOG_AT="vblank:level" raises the log level from that vblank on
		static long at = -2; static int lvl;
		if (at == -2) { const char *e = getenv("RCSM_LOG_AT"); at = e ? atol(e) : -1; if (e && strchr(e, ':')) lvl = atoi(strchr(e, ':') + 1); }
		if (at >= 0 && g_vblank_count == (u64)at) g_loglevel = lvl;
	}
	while (script && script_pos < nscript && g_vblank_count >= script[script_pos].frame) {
		script_buttons = script[script_pos].buttons;
		script_ax = script[script_pos].ax; script_ay = script[script_pos].ay;
		script_rx = script[script_pos].rx; script_ry = script[script_pos].ry;
		script_strafe = script[script_pos].strafe;
		script_pos++;
	}
	if (!headless) {
		poll_input();
		static long menu_test_at = -2;
		if (menu_test_at == -2) menu_test_at = getenv("RCSM_MENU_TEST") ? atol(getenv("RCSM_MENU_TEST")) : -1;
		if (menu_test_at >= 0 && g_vblank_count == (u64)menu_test_at) { menu_set_open(true); menu_test_frames = 8; }
		if (menu_is_open()) run_menu_pause();
	}
	fe_buttons = kb_buttons | pad_buttons | script_buttons;
	{ extern void mods_vblank(void); mods_vblank(); }
	if (g_display_dirty) { g_display_dirty = false; fe_apply_display_settings(); }   // (native menu changes)
	ge_gl_set_lighting(g_cfg.lighting, g_cfg.specular, g_cfg.shadows, g_cfg.shadow_quality, g_cfg.light_intensity);
	{	// test aid: RCSM_GE_TRACE=vblank traces one frame of GE commands into <shot dir>/getrace_<vb>.bin
		static long tv = -2;
		if (tv == -2) { const char *e = getenv("RCSM_GE_TRACE"); tv = e ? atol(e) : -1; }
		if (tv >= 0 && g_vblank_count == (u64)tv) { char p[300]; snprintf(p, sizeof p, "%s/getrace_%ld.bin", shot_dir, tv); ge_trace(p); }
		if (tv >= 0 && g_vblank_count == (u64)tv + 2) ge_trace(NULL);
	}
	{	// test aid: RCSM_MOUSE_AT="from-to:dx,dy" adds mouse motion every vblank in the window
		static const char *m = (const char *)1;
		if (m == (const char *)1) m = getenv("RCSM_MOUSE_AT");
		unsigned long a, b; float dx, dy;
		if (m && sscanf(m, "%lu-%lu:%f,%f", &a, &b, &dx, &dy) == 4 && g_vblank_count >= a && g_vblank_count < b) { fe_mouse_dx += dx; fe_mouse_dy += dy; }
	}
	update_strafe_and_camera();
	if (script_ax || script_ay) { fe_lx = (u8)(script_ax + 128); fe_ly = (u8)(script_ay + 128); }
	else if (headless) { fe_lx = 128; fe_ly = 128; }
	{
		static const char *dump_at = (const char *)-1;
		if (dump_at == (const char *)-1) dump_at = getenv("RCSM_DUMP_AT");
		if (dump_at) {
			char key[32], list[4096];
			snprintf(key, sizeof key, ",%llu,", (unsigned long long)g_vblank_count);
			snprintf(list, sizeof list, ",%s,", dump_at);
			if (strstr(list, key)) {
				char p[300];
				mkdir(shot_dir, 0755);
				snprintf(p, sizeof p, "%s/ram_%06llu.bin", shot_dir, (unsigned long long)g_vblank_count);
				FILE *f = fopen(p, "wb");
				if (f) { fwrite(PTR(0x08800000), 1, 0x01800000, f); fclose(f); }
				fe_write_png(strcat(p, ".png"));
			}
		}
	}
	{	// RCSM_WINSHOT_EVERY=N: window (GL) screenshots every N vblanks
		static int we = -1;
		if (we < 0) { const char *e = getenv("RCSM_WINSHOT_EVERY"); we = e ? atoi(e) : 0; }
		if (we > 0 && g_vblank_count % we == 0) want_winshot = true;
	}
	if (shot_every && g_vblank_count >= shot_from && g_vblank_count % shot_every == 0) {
		char p[300];
		snprintf(p, sizeof p, "%s/frame_%06llu.png", shot_dir, (unsigned long long)g_vblank_count);
		fe_write_png(p);
	}
	present();
}

void fe_audio_push(const s16 *samples, int frames) {
	if (!audio_dev) return;
	if (SDL_GetQueuedAudioSize(audio_dev) > 44100 * 4 / 4) return;  // >250 ms queued: drop rather than lag
	if (g_cfg.volume >= 100) { SDL_QueueAudio(audio_dev, samples, frames * 4); return; }
	static s16 buf[8192];
	int n = frames * 2 > 8192 ? 8192 : frames * 2;
	for (int i = 0; i < n; i++) buf[i] = samples[i] * g_cfg.volume / 100;
	SDL_QueueAudio(audio_dev, buf, n * 2);
}

// The backend must be chosen before ge_soft_init (called from main before fe_init).
void fe_preinit(void) {
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");  // let Ctrl+C / SIGTERM end the process normally
	config_load();
	headless = getenv("RCSM_HEADLESS") && atoi(getenv("RCSM_HEADLESS"));
	if (g_cfg.renderer == 1) {
		// probe for a display: OpenGL needs a (possibly hidden) window
		if (SDL_Init(SDL_INIT_VIDEO) != 0) {
			LOGE("no display available (%s): using the software renderer", SDL_GetError());
			g_cfg.renderer = 0;
		}
	}
	ge_select_backend(g_cfg.renderer);
}

void fe_init(void) {
	const char *e;
	if ((e = getenv("RCSM_SHOT_EVERY"))) shot_every = atoi(e);
	if ((e = getenv("RCSM_SHOT_FROM"))) shot_from = strtoull(e, NULL, 10);
	if ((e = getenv("RCSM_SHOT_DIR"))) snprintf(shot_dir, sizeof shot_dir, "%s", e);
	if (shot_every) mkdir(shot_dir, 0755);
	if ((e = getenv("RCSM_INPUT"))) {
		char *tmp = strdup(e);
		int cap = 1;
		for (const char *p = e; *p; p++) if (*p == ',') cap++;
		script = calloc(cap, sizeof *script);
		for (char *save, *t = strtok_r(tmp, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
			char *colon = strchr(t, ':');
			if (!colon) continue;
			*colon = 0;
			script[nscript].frame = strtoull(t, NULL, 10);
			script[nscript].buttons = colon[1] == '-' ? 0 : parse_buttons(colon + 1);
			parse_stick(colon + 1, &script[nscript].ax, &script[nscript].ay);
			parse_rstick(colon + 1, &script[nscript].rx, &script[nscript].ry);
			script[nscript].strafe = strcasestr(colon + 1, "STRAFE") != NULL;
			nscript++;
		}
		free(tmp);
	}
	if (SDL_Init(SDL_INIT_VIDEO | (headless ? 0 : SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER)) != 0) {
		if (g_cfg.renderer == 1) fatal("SDL init failed (%s)", SDL_GetError());
		LOGE("SDL init failed (%s); running without a window", SDL_GetError());
		headless = true;
		return;
	}
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	int scale = g_cfg.window_scale < 1 ? 2 : g_cfg.window_scale;
	win = SDL_CreateWindow("Sonic Rivals 2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                       480 * scale, 272 * scale, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
	                       (headless ? SDL_WINDOW_HIDDEN : 0));
	if (!win) {
		if (g_cfg.renderer == 1) fatal("could not create an OpenGL window (%s)", SDL_GetError());
		LOGE("no window (%s)", SDL_GetError());
		headless = true;
		return;
	}
	glctx = SDL_GL_CreateContext(win);
	if (!glctx) fatal("could not create an OpenGL 3.3 context (%s). Try renderer = 0 in %s", SDL_GetError(), config_path());
	have_gl = true;
	p_glViewport = SDL_GL_GetProcAddress("glViewport");
	p_glClearColor = SDL_GL_GetProcAddress("glClearColor");
	p_glClear = SDL_GL_GetProcAddress("glClear");
	p_glReadPixels = SDL_GL_GetProcAddress("glReadPixels");
	SDL_GL_SetSwapInterval(headless ? 0 : (g_cfg.vsync ? 1 : 0));
	ge_gl_init(SDL_GL_GetProcAddress, g_cfg.internal_scale);
	ge_gl_set_options(g_cfg.tex_filter, g_cfg.anisotropy, 0);
	if (!headless) {
		if (g_cfg.fullscreen) set_fullscreen(true);
		menu_init(win, glctx);
		SDL_AudioSpec want = {0}, have;
		want.freq = 44100; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 1024;
		audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
		if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
		else LOGI("no audio device (%s)", SDL_GetError());
		for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); break; }
		LOGI("%s renderer, %d fps mode. F1/Esc = settings menu. Keys: WASD move, Space/K jump, L fire, J wrench, I quick-select, "
		     "Q/E/R/F camera, Z/C strafe mode, arrows d-pad, Enter Start, F11 fullscreen, F12 screenshot",
		     g_cfg.renderer ? "OpenGL" : "software", g_cfg.fps);
	}
}

// settings menu callbacks
void fe_apply_display_settings(void) {
	set_fullscreen(g_cfg.fullscreen);
	if (!g_cfg.fullscreen && win) SDL_SetWindowSize(win, 480 * g_cfg.window_scale, 272 * g_cfg.window_scale);
	ge_gl_set_internal_scale(g_cfg.internal_scale);
	ge_gl_set_options(g_cfg.tex_filter, g_cfg.anisotropy, 0);
}
