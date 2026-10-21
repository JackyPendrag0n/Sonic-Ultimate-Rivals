// PC settings overlay (Dear ImGui). Opened with F1 / Esc / controller Guide; pauses the game.
// The same settings are also in the game's own Options screen (game.c).
#include <SDL2/SDL.h>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl2.h"
#include "imgui/imgui_impl_opengl3.h"
#include "config.h"
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
static int g_renderer_at_start = -1;

extern "C" void menu_init(SDL_Window *w, void *glctx) {
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
	io.IniFilename = nullptr;
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
	ImGui::BulletText("Player 1: A or Start, then pick the stage and your character as in Free Play.");
	ImGui::BulletText("Act 1 stages are races, Act 2 stages are battles (all six modes), Act 4 are bosses.");
	ImGui::BulletText("Battles: every player keeps their own score; the first to the goal wins, or the best at time-up.");
	ImGui::BulletText("Bosses: every player has their own six hits to land; the first to land them wins.");
	ImGui::BulletText("Two players split the screen top and bottom; three or four, into quarters.");
	ImGui::BulletText("Before the start each player gets a camera shot and says a line.");
	ImGui::BulletText("A race goes on until everyone is in (or 90 s after the winner): 1st, 2nd, 3rd, 4th.");
	ImGui::Separator();
	if (ImGui::Checkbox("Debug: player 1's controller drives everyone", &g_cfg.pad_all)) changed();
	help("For testing on your own: every player copies controller 1 (and the keyboard).");
	if (ImGui::Checkbox("Debug: add players without a controller", &g_cfg.mp_test_seats)) changed();
	help("On the Multiplayer screen, player 1 presses Y / triangle to add a player and X / square to remove one; "
	     "left / right picks the last added player's character. They stand still unless the box above is ticked.");
}

// the Multiplayer screen (game.c): who plays, on which controller, as whom
extern "C" int mp_lobby(int *pad, int *ch);
extern "C" const char *fe_pad_name(int id);

static bool mp_test_seats_on() { return g_cfg.mp_test_seats; }
static void lobby_draw(int w, int h) {
	int pad[4], ch[4];
	int st = mp_lobby(pad, ch);
	if (!st) return;
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	ImFont *font = ImGui::GetFont();
	auto text = [&](float cx, float y, float size, ImU32 c, const char *t) {   // centred on cx
		ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, t);
		dl->AddText(font, size, ImVec2(cx - ts.x / 2, y), c, t);
	};
	static const ImU32 pcol[] = {IM_COL32(70, 140, 255, 255), IM_COL32(240, 80, 80, 255), IM_COL32(80, 205, 100, 255), IM_COL32(245, 205, 50, 255)};
	const ImU32 white = IM_COL32(255, 255, 255, 255), grey = IM_COL32(165, 175, 200, 255);
	dl->AddRectFilled(ImVec2(0, 0), ImVec2((float)w, (float)h), IM_COL32(10, 16, 44, 240));
	float u = h / 24.0f;
	text(w / 2.0f, u * 1.4f, u * 1.8f, white, "MULTIPLAYER");
	text(w / 2.0f, u * 3.4f, u * 0.8f, grey, "Races, battles and bosses for up to four players");
	bool wide = w > h * 1.3f;
	int cols = wide ? 4 : 2;
	float gap = u * 0.8f, top = u * 5.2f, cw = (w - gap * (cols + 1)) / cols, chh = wide ? u * 12 : u * 6.6f;
	int players = 0;
	for (int k = 0; k < 4; k++) players += k == 0 || pad[k] != -100;
	float pulse = 0.55f + 0.45f * sinf((float)ImGui::GetTime() * 4);
	for (int k = 0; k < 4; k++) {
		float x = gap + (k % cols) * (cw + gap), y = top + (k / cols) * (chh + gap), cx = x + cw / 2;
		bool in = k == 0 || pad[k] != -100;
		dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cw, y + chh), in ? IM_COL32(28, 40, 88, 255) : IM_COL32(20, 26, 56, 255), u * 0.4f);
		dl->AddRect(ImVec2(x, y), ImVec2(x + cw, y + chh), in ? pcol[k] : IM_COL32(60, 70, 110, 255), u * 0.4f, 0, in ? u * 0.15f : u * 0.06f);
		char l[64];
		snprintf(l, sizeof l, "P%d", k + 1);
		text(cx, y + u * 0.4f, u * 1.3f, in ? pcol[k] : IM_COL32(90, 100, 140, 255), l);
		float ty = y + chh * (wide ? 0.32f : 0.36f);
		const char *pn = in && pad[k] >= 0 ? fe_pad_name(pad[k]) : NULL;
		if (k == 0) {
			text(cx, ty, u * 0.95f, white, "Player 1");
			text(cx, ty + u * 1.5f, u * 0.65f, grey, "picks a character");
			text(cx, ty + u * 2.3f, u * 0.65f, grey, "after the stage");
			text(cx, y + chh - u * 1.3f, u * 0.6f, grey, pn ? pn : "keyboard");
		} else if (in) {
			text(cx, ty, u * 1.05f, white, mp_char_names[ch[k]]);
			bool test = pad[k] < -1 && mp_test_seats_on();
			text(cx, ty + u * 1.6f, u * 0.65f, grey, test ? "added by player 1" : "< left / right >");
			text(cx, ty + u * 2.4f, u * 0.65f, grey, test ? "(no controller)" : "B: leave");
			text(cx, y + chh - u * 1.3f, u * 0.6f, grey, pn ? pn : pad[k] < -1 ? "test input" : "controller");
		} else {
			ImU32 c = IM_COL32(200, 210, 240, (int)(255 * pulse));
			text(cx, ty, u * 0.8f, c, "Press A");
			text(cx, ty + u * 1.2f, u * 0.8f, c, "to join");
		}
	}
	float fy = top + (wide ? 1 : 2) * (chh + gap) + u * 0.4f;
	if (st == 2) text(w / 2.0f, fy, u * 0.9f, white, "Starting...");
	else {
		text(w / 2.0f, fy, u * 0.85f, players > 1 ? white : grey,
		     players > 1 ? "Player 1: A or Start to pick the stage" : "Waiting for player 2: press A on another controller");
		text(w / 2.0f, fy + u * 1.3f, u * 0.65f, grey, mp_test_seats_on()
		     ? "Player 1: B to go back     Debug: Y adds a player, X removes, left / right their character"
		     : "Player 1: B to go back");
	}
}

extern "C" int mp_placings(int *place, float *time, float (*view)[4], int *over);
extern "C" int mp_player_char(int k);
extern "C" int mp_split_lead(void);
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
	ImFont *font = ImGui::GetFont();
	auto text = [&](float cx, float cy, float size, ImU32 c, const char *t) {   // centred, outlined
		ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, t);
		ImVec2 p(cx - ts.x / 2, cy - ts.y / 2);
		for (int d = 0; d < 8; d++) {
			static const int dx[] = {-1, 0, 1, -1, 1, -1, 0, 1}, dy[] = {-1, -1, -1, 0, 0, 1, 1, 1};
			float o = size / 16;
			dl->AddText(font, size, ImVec2(p.x + dx[d] * o, p.y + dy[d] * o), IM_COL32(0, 0, 0, 220), t);
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
			if (place[k] < 1 || place[k] > 4) continue;
			float vx = game[0] + view[k][0] * game[2], vy = game[1] + view[k][1] * game[3];
			float vw = view[k][2] * game[2], vh = view[k][3] * game[3];
			text(vx + vw / 2, vy + vh * 0.38f, vh * 0.3f, col[place[k]], ord[place[k]]);
		}
		return;
	}
	// the results: one line per place, right of the game's own results box
	float size = game[3] * 0.05f, y = game[1] + game[3] * 0.64f - size * (n - 1) * 0.6f, cx = game[0] + game[2] * 0.75f;
	for (int p = 1; p <= 4; p++)
		for (int k = 0; k < n; k++) {
			if (place[k] != p) continue;
			int ch = mp_player_char(k), cs = (int)(time[k] * 100 + 0.5f);
			char line[96];
			char score[32];
			if (mp_boss_hits(k) >= 0)   // the bosses: hits, not times
				snprintf(line, sizeof line, "%s  P%d %-11s %d hits", ord[p], k + 1, ch >= 0 ? mp_char_names[ch] : "", mp_boss_hits(k));
			else if (mp_score_text(k, score, sizeof score))   // the battles: their scores
				snprintf(line, sizeof line, "%s  P%d %-11s %s", ord[p], k + 1, ch >= 0 ? mp_char_names[ch] : "", score);
			else
				snprintf(line, sizeof line, "%s  P%d %-11s %d:%02d.%02d", ord[p], k + 1, ch >= 0 ? mp_char_names[ch] : "", cs / 6000, cs / 100 % 60, cs % 100);
			text(cx, y, size, col[p], line);
			y += size * 1.2f;
		}
	for (int k = 0; k < n; k++) if (!place[k]) {   // did not finish in time
		int ch = mp_player_char(k);
		char line[96]; snprintf(line, sizeof line, "--   P%d %-11s  -:--.--", k + 1, ch >= 0 ? mp_char_names[ch] : "");
		text(cx, y, size, IM_COL32(170, 170, 170, 255), line);
		y += size * 1.2f;
	}
}

extern "C" int fe_game_rect[4];

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
