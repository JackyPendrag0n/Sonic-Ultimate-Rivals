// PC settings overlay (Dear ImGui). Opened with F1 / Esc / controller Guide; pauses the game.
// The same settings are also in the game's own Options screen (game.c).
#include <SDL2/SDL.h>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl2.h"
#include "imgui/imgui_impl_opengl3.h"
#include "config.h"
#include "menu_font.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

extern "C" {
void fe_apply_display_settings(void);
void ge_gl_set_internal_scale(int scale);
int config_fps_next(int fps, int dir);
bool split_active(void);
bool split_start(void);
void split_stop(void);
void fe_split_layout_changed(void);
}

static bool g_open, g_ready, g_quit, g_dirty;
static ImFont *g_menu_font;   // the game's menus' look: bold italic
static int g_renderer_at_start = -1;

extern "C" void menu_init(SDL_Window *w, void *glctx) {
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
	io.IniFilename = nullptr;
	io.Fonts->AddFontDefault();
	ImFontConfig fc; fc.FontDataOwnedByAtlas = false;
	g_menu_font = io.Fonts->AddFontFromMemoryTTF((void *)menu_font_ttf, sizeof menu_font_ttf, 48, &fc);
	ImGui::StyleColorsDark();
	ImGuiStyle &st = ImGui::GetStyle();
	st.WindowRounding = 6; st.FrameRounding = 4; st.GrabRounding = 4;
	st.Colors[ImGuiCol_WindowBg].w = 0.94f;
	ImGui_ImplSDL2_InitForOpenGL(w, glctx);
	ImGui_ImplOpenGL3_Init("#version 330 core");
	g_ready = true;
	g_renderer_at_start = g_cfg.renderer;
}

extern "C" bool menu_event(SDL_Event *e) { return g_ready && ImGui_ImplSDL2_ProcessEvent(e); }
extern "C" bool menu_is_open(void) { return g_open; }
extern "C" void menu_set_open(bool open) {
	if (g_open && !open && g_dirty) { config_save(); g_dirty = false; }
	g_open = open && g_ready;
}
extern "C" bool menu_quit_requested(void) { return g_quit; }

static void changed() { g_dirty = true; }

static void help(const char *t) {
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::BeginItemTooltip()) { ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30); ImGui::TextUnformatted(t); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
}

static void graphics_tab() {
	const char *renderers[] = {"Software (CPU)", "OpenGL (GPU)"};
	if (ImGui::Combo("Renderer", &g_cfg.renderer, renderers, 2)) changed();
	if (g_cfg.renderer != g_renderer_at_start) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Restart the game to switch renderer.");
	ImGui::BeginDisabled(g_renderer_at_start != 1);
	if (ImGui::SliderInt("Internal resolution", &g_cfg.internal_scale, 1, 8, "%dx")) { changed(); fe_apply_display_settings(); }
	ImGui::SameLine(); ImGui::TextDisabled("%dx%d", 480 * g_cfg.internal_scale, 272 * g_cfg.internal_scale);
	const char *filters[] = {"Auto (game's choice)", "Nearest", "Linear"};
	if (ImGui::Combo("Texture filtering", &g_cfg.tex_filter, filters, 3)) { changed(); fe_apply_display_settings(); }
	const char *anis[] = {"Off", "2x", "4x", "8x", "16x"};
	int ai = g_cfg.anisotropy == 0 ? 0 : g_cfg.anisotropy == 2 ? 1 : g_cfg.anisotropy == 4 ? 2 : g_cfg.anisotropy == 8 ? 3 : 4;
	if (ImGui::Combo("Anisotropic filtering", &ai, anis, 5)) { static const int v[] = {0, 2, 4, 8, 16}; g_cfg.anisotropy = v[ai]; changed(); fe_apply_display_settings(); }
	ImGui::EndDisabled();
	ImGui::Separator();
	char fl[32];
	snprintf(fl, sizeof fl, g_cfg.fps == 30 ? "30 (original)" : "%d", g_cfg.fps);
	if (ImGui::BeginCombo("Race frame rate", fl)) {
		for (int f = 30, i = 0; i < 6; i++, f = config_fps_next(f, 1)) {
			snprintf(fl, sizeof fl, f == 30 ? "30 (original)" : "%d", f);
			if (ImGui::Selectable(fl, f == g_cfg.fps)) { g_cfg.fps = f; changed(); }
		}
		ImGui::EndCombo();
	}
	help("The PSP runs races at 30 fps and menus at 60. The game times its races by the clock, so they play the same "
	     "at any frame rate. Above 60, keep VSync off or use a monitor with that refresh rate.");
	if (ImGui::Checkbox("VSync", &g_cfg.vsync)) changed();
	ImGui::BeginDisabled(!g_cfg.vsync);
	if (ImGui::Checkbox("Lock game timing to the display (experimental)", &g_cfg.vsync_lock)) changed();
	ImGui::EndDisabled();
	help("With VSync on, the game's 60 Hz clock and the monitor drift against each other, and every so often a frame "
	     "waits too long for the monitor and is dropped. This lines the game's clock up with the monitor instead. "
	     "Works when the frame rate matches the monitor (or divides it, like 60 on 120 Hz). The FPS counter shows "
	     "whether it is locked.");
	if (ImGui::Checkbox("Fullscreen (F11)", &g_cfg.fullscreen)) { changed(); fe_apply_display_settings(); }
	ImGui::BeginDisabled(g_cfg.fullscreen);
	if (ImGui::SliderInt("Window scale", &g_cfg.window_scale, 1, 6, "%dx")) { changed(); fe_apply_display_settings(); }
	ImGui::EndDisabled();
	if (ImGui::Checkbox("Keep 480:272 aspect ratio", &g_cfg.keep_aspect)) changed();
	if (ImGui::Checkbox("Show FPS counter", &g_cfg.show_fps)) changed();
}

static void controls_tab() {
	if (ImGui::SliderInt("Stick deadzone", &g_cfg.stick_deadzone, 0, 50, "%d%%")) changed();
	ImGui::Separator();
	ImGui::TextDisabled("Controller (SDL / Xbox layout)");
	ImGui::BulletText("Left stick / d-pad: move     A: jump     B, X, Y: the PSP's circle, square, triangle");
	ImGui::BulletText("LB / LT: L     RB / RT: R     Start / Back: Start / Select     Guide: this menu");
	ImGui::TextDisabled("Keyboard");
	ImGui::BulletText("WASD or arrows: move     Space or K: jump     L: circle     J: square     I: triangle");
	ImGui::BulletText("Q / E: L / R     Enter: Start     Backspace: Select");
	ImGui::BulletText("F1 / Esc: this menu     F11: fullscreen     F12: screenshot");
}

static void split_tab() {
	ImGui::TextWrapped("Up to four players in one game, each with their own controller and part of the screen.");
	ImGui::Spacing();
	ImGui::TextDisabled("How to play");
	ImGui::BulletText("Main Menu > Multiplayer opens the setup screen.");
	ImGui::BulletText("Players 2-4: press A on your controller to join, left / right to pick a character.");
	ImGui::BulletText("Computer players: player 1 presses Y / triangle to fill a seat with one (X / square takes it out).");
	ImGui::BulletText("Player 1: A or Start, then pick the stage and your character as in Free Play.");
	ImGui::BulletText("Act 1 stages are races, Act 2 stages are battles (all six modes), Act 4 are bosses.");
	ImGui::BulletText("Battles: every player keeps their own score; the first to the goal wins, or the best at time-up.");
	ImGui::BulletText("Bosses: every player has their own six hits to land; the first to land them wins.");
	ImGui::BulletText("Two players split the screen top and bottom; three or four, into quarters.");
	ImGui::BulletText("Before the start each player gets a camera shot and says a line.");
	ImGui::BulletText("A race goes on until everyone is in (or 90 s after the winner): 1st, 2nd, 3rd, 4th.");
	ImGui::Separator();
	if (ImGui::Checkbox("Smarter computer players (experimental)", &g_cfg.mp_smart_ai)) changed();
	help("Computer players race whoever is in front instead of easing off to wait for player 1, so they are "
	     "harder to beat and keep up with the fastest player. Races only, in Multiplayer, Story and Single Event: "
	     "a rival in front of you keeps racing flat out.");
	if (ImGui::Checkbox("Debug: player 1's controller drives everyone", &g_cfg.pad_all)) changed();
	help("For testing on your own: every player copies controller 1 (and the keyboard).");
	if (ImGui::Checkbox("Debug: add players without a controller", &g_cfg.mp_test_seats)) changed();
	help("On the Multiplayer screen, Y / triangle then adds a player with no controller instead of a computer player "
	     "(driven by test inputs). They stand still unless the box above is ticked.");
	if (ImGui::Checkbox("Debug: unlock all outfits", &g_cfg.unlock_outfits)) changed();
	help("Every character's outfits can be picked on the Change Outfit list (triangle at character select), in any "
	     "mode. Your save is not changed: untick it and they are locked again until you earn them.");
}

// the Multiplayer screen (game.c): who plays, on which controller, as whom
extern "C" int mp_lobby(int *pad, int *ch);
extern "C" const char *fe_pad_name(int id);

static bool mp_test_seats_on() { return g_cfg.mp_test_seats; }
// The game's own menu look: a light blue backdrop of tiles and arrows, white bands with a saw edge top and bottom,
// a navy title, and items as silver arrow banners with a star (red when lit), white type outlined in navy
namespace native {
static const ImU32 NAVY = IM_COL32(22, 32, 92, 255), WHITE = IM_COL32(255, 255, 255, 255);
static void text(ImDrawList *dl, ImFont *f, float x, float y, float size, ImU32 fill, ImU32 edge, const char *t, float align = 0) {
	ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, t);
	x -= ts.x * align;
	float o = fmaxf(1.0f, size * 0.07f);
	for (int d = 0; d < 8; d++) {
		static const float dx[] = {-1, 0, 1, -1, 1, -1, 0, 1}, dy[] = {-1, -1, -1, 0, 0, 1, 1, 1};
		dl->AddText(f, size, ImVec2(x + dx[d] * o, y + dy[d] * o), edge, t);
	}
	dl->AddText(f, size, ImVec2(x, y), fill, t);
}
static void chevron(ImDrawList *dl, float x, float cy, float hh, float wd, bool right, ImU32 fill, ImU32 edge) {
	float s = right ? 1 : -1;
	ImVec2 p[6] = {{x, cy - hh}, {x + wd * 0.45f * s, cy - hh}, {x + wd * s, cy}, {x + wd * 0.45f * s, cy + hh}, {x, cy + hh}, {x + wd * 0.55f * s, cy}};
	dl->AddConcavePolyFilled(p, 6, fill);
	dl->AddPolyline(p, 6, edge, ImDrawFlags_Closed, fmaxf(1, hh * 0.08f));
}
static void star(ImDrawList *dl, ImVec2 c, float r, ImU32 fill, ImU32 edge) {
	ImVec2 p[10];
	for (int i = 0; i < 10; i++) {
		float a = -1.5708f + i * 0.6283f, rr = i & 1 ? r * 0.45f : r;
		p[i] = ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr);
	}
	dl->AddConcavePolyFilled(p, 10, fill);
	dl->AddPolyline(p, 10, edge, ImDrawFlags_Closed, fmaxf(1, r * 0.1f));
}
static void backdrop(ImDrawList *dl, float w, float h, float u, float t) {
	dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h), IM_COL32(120, 205, 250, 255), IM_COL32(120, 205, 250, 255),
	                            IM_COL32(60, 150, 228, 255), IM_COL32(60, 150, 228, 255));
	float sp = u * 2.9f, sz = u * 2.5f;
	for (int row = 0; row * sp < h + sp; row++) {
		float dir = row & 1 ? -1 : 1, off = fmodf(t * u * 0.8f, sp) * dir;
		for (float x = -sp * 2 + off; x < w + sp; x += sp) {
			ImVec2 a(x, row * sp - u * 0.6f), b(x + sz, row * sp - u * 0.6f + sz);
			dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 26), u * 0.35f);
			dl->AddRect(a, b, IM_COL32(255, 255, 255, 46), u * 0.35f, 0, fmaxf(1, u * 0.08f));
			if (((int)floorf((x - off) / sp) + row) % 3 == 0) {   // the arrows in some of the tiles
				float cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2;
				ImVec2 q[3] = {{cx - sz * 0.15f * dir, cy - sz * 0.25f}, {cx + sz * 0.15f * dir, cy}, {cx - sz * 0.15f * dir, cy + sz * 0.25f}};
				dl->AddPolyline(q, 3, IM_COL32(20, 60, 150, 70), 0, u * 0.22f);
			}
		}
	}
}
static void saw_band(ImDrawList *dl, float w, float y0, float y1, bool top, float u) {   // a white band with a saw edge
	float tooth = u * 0.7f, th = u * 0.35f;
	dl->AddRectFilledMultiColor(ImVec2(0, y0), ImVec2(w, y1), IM_COL32(255, 255, 255, 255), IM_COL32(255, 255, 255, 255),
	                            IM_COL32(214, 224, 238, 255), IM_COL32(214, 224, 238, 255));
	float ey = top ? y1 : y0;
	for (float x = 0; x < w; x += tooth) {
		ImVec2 a(x, ey), b(x + tooth, ey), c(x + tooth / 2, ey + (top ? th : -th));
		dl->AddTriangleFilled(a, b, c, top ? IM_COL32(214, 224, 238, 255) : IM_COL32(255, 255, 255, 255));
	}
}
static void banner(ImDrawList *dl, float x0, float x1, float y, float bh, bool lit) {   // a menu item: an arrow
	float tip = bh * 0.5f;
	ImVec2 p[5] = {{x0, y}, {x1, y}, {x1 + tip, y + bh / 2}, {x1, y + bh}, {x0, y + bh}};
	ImU32 t = lit ? IM_COL32(255, 120, 105, 255) : IM_COL32(250, 251, 255, 255), b = lit ? IM_COL32(196, 18, 22, 255) : IM_COL32(168, 176, 196, 255);
	dl->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(x1, y + bh), t, t, b, b);
	dl->AddTriangleFilled(p[1], p[2], p[3], lit ? IM_COL32(226, 60, 56, 255) : IM_COL32(208, 214, 228, 255));
	dl->AddRectFilled(ImVec2(x0, y + bh * 0.08f), ImVec2(x1, y + bh * 0.2f), IM_COL32(255, 255, 255, lit ? 70 : 120));
	dl->AddPolyline(p, 5, NAVY, ImDrawFlags_Closed, fmaxf(1.5f, bh * 0.06f));
}
static void button(ImDrawList *dl, ImFont *f, float x, float cy, float r, int shape, const char *label) {   // a pad button and its label
	dl->AddCircleFilled(ImVec2(x, cy), r, IM_COL32(30, 30, 40, 255));
	dl->AddCircle(ImVec2(x, cy), r, IM_COL32(220, 220, 230, 255), 0, fmaxf(1, r * 0.1f));
	float k = r * 0.45f, lw = fmaxf(1.5f, r * 0.17f);
	switch (shape) {
	case 0: dl->AddLine(ImVec2(x - k, cy - k), ImVec2(x + k, cy + k), IM_COL32(140, 190, 255, 255), lw);   // cross
	        dl->AddLine(ImVec2(x - k, cy + k), ImVec2(x + k, cy - k), IM_COL32(140, 190, 255, 255), lw); break;
	case 1: dl->AddCircle(ImVec2(x, cy), k, IM_COL32(255, 120, 130, 255), 0, lw); break;   // circle
	case 2: dl->AddTriangle(ImVec2(x, cy - k), ImVec2(x + k, cy + k * 0.7f), ImVec2(x - k, cy + k * 0.7f), IM_COL32(110, 230, 190, 255), lw); break;
	default: dl->AddRect(ImVec2(x - k * 0.8f, cy - k * 0.8f), ImVec2(x + k * 0.8f, cy + k * 0.8f), IM_COL32(240, 150, 220, 255), 0, 0, lw);   // square
	}
	text(dl, f, x + r * 1.4f, cy - r * 0.95f, r * 1.9f, WHITE, NAVY, label);
}
}

static void lobby_draw(int w, int h) {
	int pad[4], ch[4];
	int st = mp_lobby(pad, ch);
	if (!st) return;
	using namespace native;
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	ImFont *f = g_menu_font ? g_menu_font : ImGui::GetFont();
	float W = (float)w, H = (float)h, u = H / 24.0f, t = (float)ImGui::GetTime();
	backdrop(dl, W, H, u, t);
	// the dark strip down the left, behind the items
	for (float y = u * 3.4f; y < H - u * 2.6f; y += u * 0.5f)
		for (float x = 0; x < u * 2.4f; x += u * 0.5f)
			dl->AddRectFilled(ImVec2(x, y), ImVec2(x + u * 0.5f, y + u * 0.5f), ((int)(x / (u * 0.5f) + y / (u * 0.5f)) & 1) ? IM_COL32(44, 46, 58, 255) : IM_COL32(30, 32, 42, 255));
	// the title band
	saw_band(dl, W, 0, u * 3.0f, true, u);
	chevron(dl, u * 0.5f, u * 1.45f, u * 0.75f, u * 1.0f, true, IM_COL32(190, 196, 210, 255), IM_COL32(110, 116, 136, 255));
	chevron(dl, u * 1.4f, u * 1.45f, u * 0.75f, u * 1.0f, true, IM_COL32(230, 30, 30, 255), IM_COL32(120, 0, 0, 255));
	const char *title = "Multiplayer";
	float ts = u * 2.0f;
	text(dl, f, u * 2.8f, u * 0.35f, ts, NAVY, WHITE, title);
	float tx = u * 2.8f + f->CalcTextSizeA(ts, FLT_MAX, 0, title).x + u * 1.6f;
	chevron(dl, tx, u * 1.45f, u * 0.75f, u * 1.0f, false, IM_COL32(230, 30, 30, 255), IM_COL32(120, 0, 0, 255));
	for (int i = 1; i <= 4; i++)
		chevron(dl, tx + i * u * 0.9f, u * 1.45f, u * 0.75f, u * 1.0f, false, IM_COL32(220, 226, 236, 255 - i * 40), IM_COL32(150, 156, 176, 255 - i * 40));
	// the players: one banner each
	static const char *const who[] = {"Keyboard", "Computer", "Test input", "Controller"};
	float bh = u * 2.3f, gap = u * 0.95f, y0 = u * 4.6f, x0 = u * 3.6f, x1 = W * 0.56f;
	float pulse = 0.6f + 0.4f * sinf(t * 4);
	int players = 0;
	for (int k = 0; k < 4; k++) players += k == 0 || pad[k] != -100;
	for (int k = 0; k < 4; k++) {
		float y = y0 + k * (bh + gap), cy = y + bh / 2;
		bool in = k == 0 || pad[k] != -100, bot = pad[k] == -50, test = pad[k] < -1 && !bot;
		banner(dl, x0, x1, y, bh, in);
		dl->AddCircleFilled(ImVec2(x0, cy), bh * 0.62f, IM_COL32(236, 240, 248, 255));
		dl->AddCircle(ImVec2(x0, cy), bh * 0.62f, NAVY, 0, fmaxf(1.5f, bh * 0.06f));
		star(dl, ImVec2(x0, cy), bh * 0.45f, in ? IM_COL32(255, 214, 40, 255) : IM_COL32(250, 252, 255, 255), in ? IM_COL32(170, 110, 0, 255) : IM_COL32(150, 160, 184, 255));
		char l[64];
		snprintf(l, sizeof l, "P%d", k + 1);
		float sz = bh * 0.62f, lx = x0 + bh * 0.85f, ly = cy - sz * 0.58f;
		text(dl, f, lx, ly, sz, WHITE, NAVY, l);
		lx += f->CalcTextSizeA(sz, FLT_MAX, 0, "P4").x + bh * 0.45f;
		const char *pn = in && pad[k] >= 0 ? fe_pad_name(pad[k]) : NULL;
		if (k == 0) text(dl, f, lx, ly, sz, WHITE, NAVY, "Picks after the stage");
		else if (in) text(dl, f, lx, ly, sz, WHITE, NAVY, mp_char_names[ch[k]]);
		else text(dl, f, lx, ly, sz, IM_COL32(255, 255, 255, (int)(255 * pulse)), IM_COL32(22, 32, 92, (int)(255 * pulse)), "Press A to join");
		// beside it: how they play, and what they can do
		float rx = x1 + bh * 0.9f, rs = bh * 0.42f;
		if (in) {
			text(dl, f, rx, cy - rs * 1.15f, rs, WHITE, NAVY, k == 0 ? (pn ? pn : who[0]) : bot ? who[1] : test ? who[2] : (pn ? pn : who[3]));
			text(dl, f, rx, cy + rs * 0.05f, rs * 0.85f, IM_COL32(230, 240, 255, 255), NAVY,
			     k == 0 ? "Player 1 runs the menus" : bot || test ? "P1: Left / Right to change" : "Left / Right to change, B to leave");
		} else text(dl, f, rx, cy - rs * 0.55f, rs * 0.9f, IM_COL32(230, 240, 255, 255), NAVY, "or P1: Y for a computer");
	}
	// what happens next
	float sy = y0 + 4 * (bh + gap) - gap * 0.2f;
	const char *msg = st == 2 ? "Starting..." : players > 1 ? "Ready! Player 1: A or Start to pick the stage" : "Waiting for player 2";
	text(dl, f, W / 2, sy, u * 0.95f, players > 1 || st == 2 ? WHITE : IM_COL32(225, 235, 255, 255), NAVY, msg, 0.5f);
	// the footer band with the buttons
	float fy = H - u * 2.4f;
	saw_band(dl, W, fy, H, false, u);
	ImVec2 rp[4] = {{W * 0.36f, fy + u * 0.45f}, {W, fy + u * 0.45f}, {W, H - u * 0.25f}, {W * 0.36f + u * 1.2f, H - u * 0.25f}};
	dl->AddConvexPolyFilled(rp, 4, IM_COL32(204, 22, 26, 255));
	dl->AddPolyline(rp, 4, IM_COL32(110, 0, 0, 255), ImDrawFlags_Closed, fmaxf(1, u * 0.08f));
	float by = (fy + u * 0.45f + H - u * 0.25f) / 2, br = u * 0.55f, bx = W * 0.36f + u * 2.0f;
	const char *labels[] = {"Start", "Back", mp_test_seats_on() ? "Test player" : "Computer", "Remove"};
	for (int i = 0; i < 4; i++) {
		button(dl, f, bx, by, br, i, labels[i]);
		bx += br * 2.6f + f->CalcTextSizeA(br * 1.9f, FLT_MAX, 0, labels[i]).x + u * 0.6f;
	}
}

extern "C" int mp_placings(int *place, float *time, float (*view)[4], int *over);
extern "C" int mp_player_char(int k);
extern "C" int mp_split_lead(void);
extern "C" int mp_live_place(int k);
extern "C" int mp_boss_hits(int k);
extern "C" int mp_score_text(int k, char *out, int n);

// split screen races: each player's place in their view once they finish, and all of them when the race is over
static void placings_draw(const int *game) {   // game: the game image in the window (x, y, w, h)
	int place[4], over = 0; float view[4][4], time[4];
	int n = mp_placings(place, time, view, &over);
	extern bool currentDialogActive;   // a PSP dialog (saving) is up
	if (!n || currentDialogActive) return;
	static const char *const ord[] = {"", "1st", "2nd", "3rd", "4th"};
	static const ImU32 col[] = {0, IM_COL32(255, 215, 40, 255), IM_COL32(220, 225, 235, 255), IM_COL32(225, 140, 70, 255), IM_COL32(150, 190, 255, 255)};
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	ImFont *font = g_menu_font ? g_menu_font : ImGui::GetFont();
	auto text = [&](float cx, float cy, float size, ImU32 c, const char *t) {   // centred, outlined
		ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, t);
		ImVec2 p(cx - ts.x / 2, cy - ts.y / 2);
		for (int d = 0; d < 8; d++) {
			static const int dx[] = {-1, 0, 1, -1, 1, -1, 0, 1}, dy[] = {-1, -1, -1, 0, 0, 1, 1, 1};
			float o = size / 16;
			dl->AddText(font, size, ImVec2(p.x + dx[d] * o, p.y + dy[d] * o), native::NAVY, t);
		}
		dl->AddText(font, size, p, c, t);
	};
	if (!over) {
		int lead = mp_split_lead();
		if (lead >= 0) {   // three players: the fourth quarter follows the leader
			static const ImU32 pc[] = {IM_COL32(70, 140, 255, 255), IM_COL32(240, 80, 80, 255), IM_COL32(80, 205, 100, 255)};
			char l[48]; snprintf(l, sizeof l, "P%d leads", lead + 1);
			float size = game[3] * 0.05f;
			ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, l);
			float x = game[0] + game[2] * 0.5f + size * 0.5f, y = game[1] + game[3] * 0.5f + size * 0.4f;
			dl->AddRectFilled(ImVec2(x - size * 0.3f, y - size * 0.15f), ImVec2(x + ts.x + size * 0.3f, y + ts.y + size * 0.15f), IM_COL32(0, 0, 0, 150), size * 0.2f);
			dl->AddText(font, size, ImVec2(x, y), pc[lead], l);
		}
		for (int k = 0; k < n; k++) {
			if (view[k][2] <= 0) continue;   // (computer players have no view)
			float vx = game[0] + view[k][0] * game[2], vy = game[1] + view[k][1] * game[3];
			float vw = view[k][2] * game[2], vh = view[k][3] * game[3];
			if (place[k] >= 1 && place[k] <= 4) text(vx + vw / 2, vy + vh * 0.38f, vh * 0.3f, col[place[k]], ord[place[k]]);
			int lp = mp_live_place(k);   // the place now, where the game shows its own (1st or 2nd of two)
			if (lp >= 1 && lp <= 4) {
				float sz = vh * 0.1f, x = vx + vw * 0.885f, y = vy + vh * 0.905f - sz * 0.55f;
				char num[2] = {(char)('0' + lp), 0};
				native::text(dl, font, x, y, sz, col[lp], native::NAVY, num, 1);
				native::text(dl, font, x + sz * 0.05f, y + sz * 0.1f, sz * 0.55f, col[lp], native::NAVY, ord[lp] + 1);
			}
		}
		return;
	}
	// the results: a banner per place, right of the game's own results box
	float size = game[3] * 0.042f, rh = size * 1.45f, bw = game[2] * 0.44f;
	float x = game[0] + game[2] * 0.75f - bw / 2, y = game[1] + game[3] * 0.64f - n * rh * 0.6f;
	auto row = [&](int p, int k, const char *score) {
		int ch = mp_player_char(k);
		native::banner(dl, x, x + bw, y, rh, p == 1);
		char who[48]; snprintf(who, sizeof who, "P%d  %s", k + 1, ch >= 0 ? mp_char_names[ch] : "");
		float ty = y + (rh - size) / 2 - size * 0.08f;
		native::text(dl, font, x + rh * 0.3f, ty, size, p ? col[p] : IM_COL32(200, 200, 200, 255), native::NAVY, p ? ord[p] : "--");
		native::text(dl, font, x + rh * 1.9f, ty, size, native::WHITE, native::NAVY, who);
		native::text(dl, font, x + bw - rh * 0.1f, ty, size, native::WHITE, native::NAVY, score, 1);
		y += rh * 1.2f;
	};
	for (int p = 1; p <= 4; p++)
		for (int k = 0; k < n; k++) {
			if (place[k] != p) continue;
			int cs = (int)(time[k] * 100 + 0.5f);
			char score[32];
			if (mp_boss_hits(k) >= 0) snprintf(score, sizeof score, "%d hits", mp_boss_hits(k));   // the bosses: hits, not times
			else if (!mp_score_text(k, score, sizeof score))   // the battles: their scores; races: times
				snprintf(score, sizeof score, "%d:%02d.%02d", cs / 6000, cs / 100 % 60, cs % 100);
			row(p, k, score);
		}
	for (int k = 0; k < n; k++) if (!place[k]) row(0, k, "-:--.--");   // did not finish in time
}

extern "C" int fe_game_rect[4];

extern "C" char fe_perf_text[];   // frontend.c
extern "C" void menu_draw(int w, int h, float fps, float game_fps) {
	if (!g_ready) return;
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();
	placings_draw(fe_game_rect);
	lobby_draw(w, h);
	if (g_cfg.show_fps) {
		ImGui::SetNextWindowPos(ImVec2(8, 8));
		ImGui::SetNextWindowBgAlpha(0.45f);
		ImGui::Begin("##fps", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav);
		ImGui::Text("game %.0f fps  |  display %.0f Hz", game_fps, fps);
		if (fe_perf_text[0]) ImGui::TextUnformatted(fe_perf_text);
		ImGui::End();
	}
	if (g_open) {
		ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(w < 760 ? w * 0.95f : 720, h < 560 ? h * 0.92f : 500), ImGuiCond_Always);
		ImGui::Begin("PC settings  (game paused)", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
		static int force_tab = getenv("SR2_MENU_TAB") ? atoi(getenv("SR2_MENU_TAB")) : -1;  // test aid
		auto fl = [&](int i) { return force_tab == i ? ImGuiTabItemFlags_SetSelected : 0; };
		if (ImGui::BeginTabBar("tabs")) {
			if (ImGui::BeginTabItem("Graphics", nullptr, fl(0))) { graphics_tab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Controls", nullptr, fl(1))) { controls_tab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Multiplayer", nullptr, fl(4))) { split_tab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Audio", nullptr, fl(2))) { if (ImGui::SliderInt("Volume", &g_cfg.volume, 0, 100, "%d%%")) changed(); ImGui::EndTabItem(); }
			ImGui::EndTabBar();
		}
		ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() - 8);
		ImGui::Separator();
		if (ImGui::Button("Resume")) menu_set_open(false);
		ImGui::SameLine();
		ImGui::TextDisabled("settings: %s", config_path());
		ImGui::SameLine(ImGui::GetWindowWidth() - 110);
		if (ImGui::Button("Quit game")) g_quit = true;
		ImGui::End();
	}
	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
