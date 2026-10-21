// Host front end: SDL2 + OpenGL window (both renderers present through GL), input, audio, the in-game
// settings menu (menu.cpp), screenshots, and headless/scripted operation for testing.
//
// Environment (testing):
//   SR2_HEADLESS=1          hidden window, no audio. Without a display, falls back to the software renderer.
//   SR2_SHOT_EVERY=N        write <shot dir>/frame_NNNNNN.png every N vblanks;  SR2_SHOT_DIR=dir
//   SR2_WINSHOT_EVERY=N     the same, of the whole window as presented (GL)
//   SR2_INPUT="f:BTN+BTN,f:-,..."  scripted input from vblank f ('-' = release; SUP/SDOWN/SLEFT/SRIGHT = stick)
//   SR2_DUMP_AT="f1,f2"     dump guest RAM 0x08800000-0x0A000000 at those vblanks
//   SR2_CTL=fifo            live test control, one command per line: "press BTN+BTN [vblanks]" (stick: SUP...),
//                           "shot file.png", "dump file.bin" (RAM from 0x08000000), "poke ADDR hex|float",
//                           "track ADDR" (float logged every vblank to fifo.track); acks "ok <vblank>" to fifo.out
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
#include <fcntl.h>
#include <unistd.h>
#include <math.h>

// menu.cpp
void menu_init(SDL_Window *w, void *glctx);
bool menu_event(SDL_Event *e);
bool menu_is_open(void);
void menu_set_open(bool open);
void menu_draw(int w, int h, float fps, float game_fps);
bool menu_quit_requested(void);

// splitscreen.c
void split_child_init(void);
bool split_is_child(void);
bool split_active(void);
bool split_start(void);
void split_stop(void);
void split_poll(void);
void split_child_frame(void);
void split_child_input(u32 *buttons, u8 *lx, u8 *ly);
void split_set_input(u32 buttons, u8 lx, u8 ly);
void split_draw(int x, int y, int w, int h);

u32 fe_buttons;
// players 2-4 in the same game (extra racers), by pad port 1-3: controllers 2-4 or "press2".."press4"
u32 fe_pbuttons[4]; u8 fe_plx[4] = {128, 128, 128, 128}, fe_ply[4] = {128, 128, 128, 128};
bool g_display_dirty;
u8 fe_lx = 128, fe_ly = 128;
static u32 kb_buttons, pad_buttons, script_buttons;
static int script_ax, script_ay;
static int kb_ax, kb_ay;

static SDL_Window *win;
static SDL_GLContext glctx;
static SDL_AudioDeviceID audio_dev;
#define MAX_PADS 8
static SDL_GameController *pads[MAX_PADS];   // the connected controllers, in the order they came
static int npads;
static bool seats_set;                        // the Multiplayer screen handed out the controllers:
static int seat_pad[4] = {-1, -1, -1, -1};    // each player's (SDL instance id; -1: none), else the k-th connected
static int last_pad = -1;                     // the controller last pressed (player 1 when the Multiplayer screen opens)
static bool headless, have_gl;
static u32 fb_top, fb_stride, fb_fmt;
static bool fb_set;
static int shot_every; static u64 shot_from;   // SR2_SHOT_FROM=vblank: no shots before it
static char shot_dir[256] = "shots";
static u32 rgba[480 * 272];
static u64 flips;   // display buffer changes (game frames)

typedef struct { u64 frame; u32 buttons; int ax, ay; } ScriptEv;
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

// ---------------------------------------------------------------- frames, screenshots
static bool want_winshot;
static char winshot_path[300];   // SR2_CTL "winshot": where the next window capture goes
static int p2x, p2y;   // SR2_CTL "press2": player 2 test input (p2b)
static int pnx[2], pny[2], releasen_in[2];   // "press3", "press4" (pnb)
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

// F10: write the next frame's 3D scene as OBJ + PNG textures to <saves>/rips/rip_<vblank>/ (gpu/rip.cpp)
extern void ge_rip_request(const char *dir);
extern void ge_rip_flip(void);
static void rip_frame(void) {
	char d[700];
	snprintf(d, sizeof d, "%s/rips", g_save_root);
	mkdir(d, 0755);
	snprintf(d, sizeof d, "%s/rips/rip_%llu", g_save_root, (unsigned long long)g_vblank_count);
	ge_rip_request(d);
	LOGE("ripping the next frame to %s", d);
}

void fe_set_display(u32 top, u32 stride, u32 fmt) {
	if (top != fb_top) { flips++; ge_rip_flip(); }
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
	case SDLK_RETURN: return B_START;
	case SDLK_BACKSPACE: case SDLK_TAB: return B_SELECT;
	default: return 0;
	}
}

static int kb_w, kb_a, kb_s, kb_d;

static u32 read_pad(SDL_GameController *g, int *ax, int *ay) {
	if (!g) return 0;
	static const struct { int sb; u32 b; } map[] = {
		{SDL_CONTROLLER_BUTTON_A, B_CROSS}, {SDL_CONTROLLER_BUTTON_B, B_CIRCLE}, {SDL_CONTROLLER_BUTTON_X, B_SQUARE},
		{SDL_CONTROLLER_BUTTON_Y, B_TRIANGLE}, {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, B_L}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, B_R},
		{SDL_CONTROLLER_BUTTON_START, B_START}, {SDL_CONTROLLER_BUTTON_BACK, B_SELECT}, {SDL_CONTROLLER_BUTTON_DPAD_UP, B_UP},
		{SDL_CONTROLLER_BUTTON_DPAD_DOWN, B_DOWN}, {SDL_CONTROLLER_BUTTON_DPAD_LEFT, B_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, B_RIGHT}};
	u32 b = 0;
	for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
		if (SDL_GameControllerGetButton(g, map[i].sb)) b |= map[i].b;
	int dz = g_cfg.stick_deadzone * 32767 / 100;
	int px = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTX), py = SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_LEFTY);
	if ((long)px * px + (long)py * py > (long)dz * dz) { *ax = px / 256; *ay = py / 256; }
	if (SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000) b |= B_L;
	if (SDL_GameControllerGetAxis(g, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000) b |= B_R;
	return b;
}

static int pad_id(SDL_GameController *g) { return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g)); }
static SDL_GameController *pad_by_id(int id) {
	for (int i = 0; i < npads; i++) if (pad_id(pads[i]) == id) return pads[i];
	return NULL;
}
static void pad_add(SDL_GameController *g) {
	if (!g) return;
	for (int i = 0; i < npads; i++) if (pads[i] == g) return;
	if (npads < MAX_PADS) pads[npads++] = g; else SDL_GameControllerClose(g);
}
static void pads_prune(void) {
	int n = 0;
	for (int i = 0; i < npads; i++)
		if (SDL_GameControllerGetAttached(pads[i])) pads[n++] = pads[i]; else SDL_GameControllerClose(pads[i]);
	npads = n;
}
// player k's controller (0-3): the one the Multiplayer screen gave them, else the k-th connected
static SDL_GameController *player_pad(int k) {
	if (seats_set && seat_pad[k] >= 0) return pad_by_id(seat_pad[k]);
	if (seats_set && k) return NULL;
	return k < npads ? pads[k] : NULL;
}

// for the Multiplayer screen (game.c): every controller's id, name and buttons, test inputs "press2".."press4" as
// ids -2..-4; the controller last pressed; and handing them out (ids per player, NULL: by connection order)
static u32 p2b, pnb[2];
int fe_pads(int *ids, u32 *buttons, const char **names, int max) {
	int n = 0;
	for (int i = 0; i < npads && n < max; i++) {
		int x = 0, y = 0;
		ids[n] = pad_id(pads[i]); names[n] = SDL_GameControllerName(pads[i]);
		buttons[n++] = read_pad(pads[i], &x, &y) | (x < -80 ? B_LEFT : x > 80 ? B_RIGHT : 0);
	}
	for (int k = 0; k < 3 && n < max; k++) {
		ids[n] = -2 - k; names[n] = "test input"; buttons[n++] = k ? pnb[k - 1] : p2b;
	}
	return n;
}
int fe_last_pad(void) { return last_pad; }
const char *fe_pad_name(int id) {
	SDL_GameController *g = pad_by_id(id);
	return g ? SDL_GameControllerName(g) : NULL;
}
void fe_set_seats(const int *ids) {
	seats_set = ids != NULL;
	for (int k = 0; k < 4; k++) seat_pad[k] = ids ? ids[k] : -1;
}

static void poll_input(void) {
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		if (e.type == SDL_QUIT) { LOGI("window closed"); config_save(); exit(0); }
		if (e.type == SDL_KEYDOWN && !e.key.repeat) {
			SDL_Keycode k = e.key.keysym.sym;
			if (k == SDLK_F1 || k == SDLK_ESCAPE) { menu_set_open(!menu_is_open()); continue; }
			if (k == SDLK_F11) { g_cfg.fullscreen = !g_cfg.fullscreen; set_fullscreen(g_cfg.fullscreen); config_save(); continue; }
			if (k == SDLK_F12) { screenshot_hd(); continue; }
			if (k == SDLK_F10) { rip_frame(); continue; }
			if (k == SDLK_F9) { if (split_active()) split_stop(); else split_start(); continue; }
		}
		if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) { menu_set_open(!menu_is_open()); continue; }
		if (e.type == SDL_CONTROLLERDEVICEADDED) { pad_add(SDL_GameControllerOpen(e.cdevice.which)); continue; }
		if (e.type == SDL_CONTROLLERDEVICEREMOVED) { pads_prune(); continue; }
		if (e.type == SDL_CONTROLLERBUTTONDOWN) last_pad = e.cbutton.which;
		if (menu_is_open()) { menu_event(&e); continue; }
		if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
			bool down = e.type == SDL_KEYDOWN;
			SDL_Keycode k = e.key.keysym.sym;
			if (k == SDLK_w) kb_w = down;
			if (k == SDLK_a) kb_a = down;
			if (k == SDLK_s) kb_s = down;
			if (k == SDLK_d) kb_d = down;
			u32 b = key_to_button(k);
			if (down) kb_buttons |= b; else kb_buttons &= ~b;
		}
	}
	kb_ax = (kb_d - kb_a) * 127;
	kb_ay = (kb_s - kb_w) * 127;
	// Split screen: player 2 plays on the second controller, or on the only one while player 1 uses the keyboard.
	SDL_GameController *p1pad = player_pad(0), *p2pad = NULL;
	if (split_active()) { if (player_pad(1)) p2pad = player_pad(1); else { p2pad = p1pad; p1pad = NULL; } }
	int ax = kb_ax, ay = kb_ay;
	pad_buttons = menu_is_open() ? 0 : read_pad(p1pad, &ax, &ay);
	if (ax < -128) ax = -128; if (ax > 127) ax = 127;
	if (ay < -128) ay = -128; if (ay > 127) ay = 127;
	fe_lx = (u8)(ax + 128); fe_ly = (u8)(ay + 128);
	if (split_active()) {
		int bx = 0, by = 0;
		u32 b = read_pad(p2pad, &bx, &by) | p2b;
		if (p2x || p2y) { bx = p2x; by = p2y; }
		if (bx < -128) bx = -128; if (bx > 127) bx = 127;
		if (by < -128) by = -128; if (by > 127) by = 127;
		split_set_input(b, (u8)(bx + 128), (u8)(by + 128));
	} else {   // no split screen: controllers 2-4 (or "press2".."press4") are players 2-4 inside this game
		for (int k = 1; k < 4; k++) {
			int bx = 0, by = 0;
			SDL_GameController *g = player_pad(k);
			u32 sb = k == 1 ? p2b : pnb[k - 2]; int sx = k == 1 ? p2x : pnx[k - 2], sy = k == 1 ? p2y : pny[k - 2];
			fe_pbuttons[k] = (menu_is_open() ? 0 : read_pad(g, &bx, &by)) | sb;
			if (sx || sy) { bx = sx; by = sy; }
			if (bx < -128) bx = -128; if (bx > 127) bx = 127;
			if (by < -128) by = -128; if (by > 127) by = 127;
			fe_plx[k] = (u8)(bx + 128); fe_ply[k] = (u8)(by + 128);
		}
	}
}

// ---------------------------------------------------------------- presentation
static float fps_shown, game_fps_shown;
static u64 fps_t0, fps_frames, fps_flips0;

unsigned *g_fcount;   // test aid: calls per function (build/sr2recomp_fc), see "fcount"
int fe_game_rect[4];   // where the game image is in the window (x, y, w, h; y from the top), for the overlay
static bool overlay_headless;

static void present(void) {
	if (!have_gl) return;
	int w, h;
	SDL_GL_GetDrawableSize(win, &w, &h);
	glViewport(0, 0, w, h);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	if (split_active()) {
		// player 1 left (or top), player 2 right (or bottom); GL's y runs bottom-up
		bool tb = g_cfg.split_layout == 1;
		int cw = tb ? w : w / 2, ch = tb ? h / 2 : h;
		int vw = cw, vh = ch;
		if (g_cfg.keep_aspect) { vh = cw * 272 / 480; if (vh > ch) { vh = ch; vw = ch * 480 / 272; } }
		int ox = (cw - vw) / 2, oy = (ch - vh) / 2;
		ge_present(ox, (tb ? ch : 0) + oy, vw, vh);
		split_draw((tb ? 0 : cw) + ox, oy, vw, vh);
	} else {
		int vw = w, vh = h;
		if (g_cfg.keep_aspect) { vh = w * 272 / 480; if (vh > h) { vh = h; vw = h * 480 / 272; } }
		ge_present((w - vw) / 2, (h - vh) / 2, vw, vh);
		fe_game_rect[0] = (w - vw) / 2; fe_game_rect[1] = (h - vh) / 2; fe_game_rect[2] = vw; fe_game_rect[3] = vh;
		if (split_is_child()) split_child_frame();
	}
	if (!headless || overlay_headless) menu_draw(w, h, fps_shown, game_fps_shown);
	if (want_winshot && p_glReadPixels) {
		// test aid: capture the whole window (game + overlay) as presented
		u32 *px = malloc((size_t)w * h * 4), *fl = malloc((size_t)w * h * 4);
		p_glReadPixels(0, 0, w, h, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, px);
		for (int y = 0; y < h; y++) memcpy(fl + (size_t)y * w, px + (size_t)(h - 1 - y) * w, w * 4);
		char p[300];
		mkdir(shot_dir, 0755);
		snprintf(p, sizeof p, "%s/window_%llu.png", shot_dir, (unsigned long long)g_vblank_count);
		if (winshot_path[0]) { snprintf(p, sizeof p, "%s", winshot_path); winshot_path[0] = 0; }
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
		if (menu_test_frames == 0 && getenv("SR2_MENU_TEST")) { present(); menu_set_open(false); break; }
		present();
		if (menu_quit_requested()) { config_save(); exit(0); }
		if (!g_cfg.vsync) SDL_Delay(8);
	}
	if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
	SDL_GL_SetSwapInterval(g_cfg.vsync ? 1 : 0);
	kernel_add_pause_time(now_us() - t0);
	kb_buttons = 0; kb_w = kb_a = kb_s = kb_d = 0;
}

// SR2_CTL: drive a running game from a shell (see the top of this file)
static u32 track[8]; static int ntrack;
static void ctl_poll(void) {
	static int fd = -2, release_in, release2_in;

	static char buf[1024]; static int len;
	if (fd == -2) {
		const char *e = getenv("SR2_CTL");
		fd = -1;
		if (e) { mkfifo(e, 0644); fd = open(e, O_RDWR | O_NONBLOCK); }
	}
	if (fd < 0) return;
	if (ntrack) {
		char tp[300]; snprintf(tp, sizeof tp, "%s.track", getenv("SR2_CTL"));
		FILE *f = fopen(tp, "a");
		if (f) {
			fprintf(f, "%llu", (unsigned long long)g_vblank_count);
			for (int i = 0; i < ntrack; i++) { float v; memcpy(&v, PTR(track[i]), 4); fprintf(f, " %g", v); }
			fputc('\n', f); fclose(f);
		}
	}
	if (release_in && --release_in == 0) { script_buttons = 0; script_ax = script_ay = 0; }
	if (release2_in && --release2_in == 0) { p2b = 0; p2x = p2y = 0; }
	for (int i = 0; i < 2; i++) if (releasen_in[i] && --releasen_in[i] == 0) { pnb[i] = 0; pnx[i] = pny[i] = 0; }

	int n = read(fd, buf + len, sizeof buf - 1 - len);
	if (n > 0) len += n;
	char *nl;
	while ((nl = memchr(buf, '\n', len))) {
		*nl = 0;
		char cmd[16] = "", arg[256] = ""; int cnt = 6;
		sscanf(buf, "%15s %255s %d", cmd, arg, &cnt);
		if (!strcmp(cmd, "press")) {
			script_buttons = parse_buttons(arg); parse_stick(arg, &script_ax, &script_ay); release_in = cnt;
		} else if (!strcmp(cmd, "press2")) {   // split screen: player 2's input
			p2b = parse_buttons(arg); parse_stick(arg, &p2x, &p2y); release2_in = cnt;
		} else if (!strcmp(cmd, "press3") || !strcmp(cmd, "press4")) {   // players 3 and 4 (extra racers)
			int i = cmd[5] - '3';
			pnb[i] = parse_buttons(arg); parse_stick(arg, &pnx[i], &pny[i]); releasen_in[i] = cnt;
		} else if (!strcmp(cmd, "shot")) fe_write_png(arg);
		else if (!strcmp(cmd, "winshot")) { snprintf(winshot_path, sizeof winshot_path, "%s", arg); want_winshot = true; }
		else if (!strcmp(cmd, "poke")) {      // "poke ADDR VALUE": 32-bit word, VALUE hex or a float with a '.'
			u32 a = strtoul(arg, NULL, 16); const char *v = strchr(buf + strlen(cmd) + 1, ' ');
			if (v) { if (strchr(v, '.')) { float fv = strtof(v, NULL); memcpy(PTR(a), &fv, 4); } else SW(a, strtoul(v, NULL, 16)); }
		} else if (!strcmp(cmd, "track")) {   // "track ADDR": log that float every vblank to <fifo>.track ("track 0" clears)
			u32 a = strtoul(arg, NULL, 16);
			if (!a) ntrack = 0; else if (ntrack < 8) track[ntrack++] = a;
		} else if (!strcmp(cmd, "warp")) {   // "warp K DIST": player K+1 to that distance along the course
			extern void game_request_warp(int k, float dist);
			float d = 0; const char *v = strchr(buf + strlen(cmd) + 1, ' ');
			if (v) d = strtof(v, NULL);
			game_request_warp(atoi(arg), d);
		} else if (!strcmp(cmd, "fcount")) {   // "fcount PATH" (build/sr2recomp_fc): function calls since the last one
			extern unsigned *g_fcount;
			if (!g_fcount) g_fcount = calloc(0x400000 / 4, sizeof *g_fcount);
			FILE *f = fopen(arg, "w");
			for (u32 i = 0; f && i < 0x400000 / 4; i++) if (g_fcount[i]) fprintf(f, "%06x %u\n", i * 4, g_fcount[i]);
			if (f) fclose(f);
			memset(g_fcount, 0, 0x400000 / 4 * sizeof *g_fcount);
		} else if (!strcmp(cmd, "dump")) {
			FILE *f = fopen(arg, "wb");
			if (f) { fwrite(PTR(0x08000000), 1, 0x04000000, f); fclose(f); }
		}
		char ack[300]; snprintf(ack, sizeof ack, "%s.out", getenv("SR2_CTL"));
		FILE *f = fopen(ack, "a");
		if (f) { fprintf(f, "ok %llu %s\n", (unsigned long long)g_vblank_count, cmd); fclose(f); }
		len -= nl + 1 - buf;
		memmove(buf, nl + 1, len);
	}
}

void fe_vblank(void) {
	ge_soft_vblank();
	ctl_poll();
	{	// test aid: SR2_LOG_AT="vblank:level" raises the log level from that vblank on
		static long at = -2; static int lvl;
		if (at == -2) { const char *e = getenv("SR2_LOG_AT"); at = e ? atol(e) : -1; if (e && strchr(e, ':')) lvl = atoi(strchr(e, ':') + 1); }
		if (at >= 0 && g_vblank_count == (u64)at) g_loglevel = lvl;
	}
	while (script && script_pos < nscript && g_vblank_count >= script[script_pos].frame) {
		script_buttons = script[script_pos].buttons;
		script_ax = script[script_pos].ax; script_ay = script[script_pos].ay;
		script_pos++;
	}
	if (!headless) {
		poll_input();
		static long menu_test_at = -2;
		if (menu_test_at == -2) menu_test_at = getenv("SR2_MENU_TEST") ? atol(getenv("SR2_MENU_TEST")) : -1;
		if (menu_test_at >= 0 && g_vblank_count == (u64)menu_test_at) { menu_set_open(true); menu_test_frames = 8; }
		if (menu_is_open()) run_menu_pause();
	}
	fe_buttons = kb_buttons | pad_buttons | script_buttons;
	{ extern void game_vblank(void); game_vblank(); }
	if (g_display_dirty) { g_display_dirty = false; fe_apply_display_settings(); }   // changed in the game's Options screen
	{	// test aid: SR2_RIP_AT=vblank rips that frame like F10
		static long ra = -2;
		if (ra == -2) { const char *e = getenv("SR2_RIP_AT"); ra = e ? atol(e) : -1; }
		if (ra >= 0 && g_vblank_count == (u64)ra) rip_frame();
	}
	{	// test aid: SR2_GE_TRACE=vblank traces one frame of GE commands into <shot dir>/getrace_<vb>.bin
		static long tv = -2;
		if (tv == -2) { const char *e = getenv("SR2_GE_TRACE"); tv = e ? atol(e) : -1; }
		if (tv >= 0 && g_vblank_count == (u64)tv) { char p[300]; mkdir(shot_dir, 0755); snprintf(p, sizeof p, "%s/getrace_%ld.bin", shot_dir, tv); ge_trace(p); }
		if (tv >= 0 && g_vblank_count == (u64)tv + 2) ge_trace(NULL);
	}
	if (script_ax || script_ay) { fe_lx = (u8)(script_ax + 128); fe_ly = (u8)(script_ay + 128); }
	else if (headless) { fe_lx = 128; fe_ly = 128; }
	if (headless && !split_active()) {
		fe_pbuttons[1] = p2b; fe_plx[1] = (u8)(p2x + 128); fe_ply[1] = (u8)(p2y + 128);
		for (int i = 0; i < 2; i++) { fe_pbuttons[i + 2] = pnb[i]; fe_plx[i + 2] = (u8)(pnx[i] + 128); fe_ply[i + 2] = (u8)(pny[i] + 128); }
	}
	if (split_is_child()) { u32 b; split_child_input(&b, &fe_lx, &fe_ly); fe_buttons |= b; }
	split_poll();
	{
		static const char *dump_at = (const char *)-1;
		if (dump_at == (const char *)-1) dump_at = getenv("SR2_DUMP_AT");
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
	{	// SR2_WINSHOT_EVERY=N: window (GL) screenshots every N vblanks
		static int we = -1;
		if (we < 0) { const char *e = getenv("SR2_WINSHOT_EVERY"); we = e ? atoi(e) : 0; }
		if (we > 0 && g_vblank_count % we == 0) want_winshot = true;
	}
	if (shot_every && g_vblank_count >= shot_from && g_vblank_count % shot_every == 0) {
		char p[300];
		snprintf(p, sizeof p, "%s/frame_%06llu.png", shot_dir, (unsigned long long)g_vblank_count);
		fe_write_png(p);
	}
	// Present only when the game flipped (or the window may need redrawing): vblanks can tick far faster
	// than the game draws, and every present costs a full blit plus a swap.
	static u64 presented_flips, presented_at;
	u64 t = now_us();
	if (flips != presented_flips || want_winshot || g_cfg.show_fps || t - presented_at > 100000) {
		presented_flips = flips; presented_at = t;
		present();
	}
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
	split_child_init();
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");  // let Ctrl+C / SIGTERM end the process normally
	config_load();
	headless = getenv("SR2_HEADLESS") && atoi(getenv("SR2_HEADLESS"));
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
	if ((e = getenv("SR2_SHOT_EVERY"))) shot_every = atoi(e);
	if ((e = getenv("SR2_SHOT_FROM"))) shot_from = strtoull(e, NULL, 10);
	if ((e = getenv("SR2_SHOT_DIR"))) snprintf(shot_dir, sizeof shot_dir, "%s", e);
	if (shot_every) mkdir(shot_dir, 0755);
	if ((e = getenv("SR2_INPUT"))) {
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
	overlay_headless = headless && getenv("SR2_OVERLAY");   // test aid: the overlay in winshots
	if (overlay_headless) menu_init(win, glctx);
	if (!headless) {
		if (g_cfg.fullscreen) set_fullscreen(true);
		menu_init(win, glctx);
		SDL_AudioSpec want = {0}, have;
		want.freq = 44100; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 1024;
		audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
		if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
		else LOGI("no audio device (%s)", SDL_GetError());
		for (int i = 0; i < SDL_NumJoysticks(); i++)
			if (SDL_IsGameController(i)) pad_add(SDL_GameControllerOpen(i));
		LOGI("%s renderer, %d fps races. F1/Esc = PC settings. Keys: WASD/arrows move, Space/K jump, J/L/I actions, "
		     "Q/E L/R, Enter Start, F11 fullscreen, F12 screenshot", g_cfg.renderer ? "OpenGL" : "software", g_cfg.fps);
	}
	if (!split_is_child() && (g_start_2p || (getenv("SR2_2P") && atoi(getenv("SR2_2P"))))) split_start();
}

// split screen started/stopped or its layout changed: a window that isn't fullscreen grows to fit two games
void fe_split_layout_changed(void) {
	if (!win || headless || g_cfg.fullscreen) return;
	int s = g_cfg.window_scale < 1 ? 2 : g_cfg.window_scale;
	int k = split_active() ? 2 : 1;
	if (split_active() && g_cfg.split_layout == 1) SDL_SetWindowSize(win, 480 * s, 272 * s * k);
	else SDL_SetWindowSize(win, 480 * s * k, 272 * s);
}

// settings menu callbacks
void fe_apply_display_settings(void) {
	static int last_full = -1, last_scale = -1;
	if (g_cfg.fullscreen != last_full) set_fullscreen(g_cfg.fullscreen);
	if (!g_cfg.fullscreen && win && (g_cfg.fullscreen != last_full || g_cfg.window_scale != last_scale))
		fe_split_layout_changed();
	last_full = g_cfg.fullscreen; last_scale = g_cfg.window_scale;
	if (have_gl && !headless) SDL_GL_SetSwapInterval(g_cfg.vsync ? 1 : 0);
	ge_gl_set_internal_scale(g_cfg.internal_scale);
	ge_gl_set_options(g_cfg.tex_filter, g_cfg.anisotropy, 0);
}
