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

static void players_tab() {
	ImGui::TextWrapped("Two players on one PC: a second copy of the game opens next to yours, and you race each other "
	                   "through the game's own Wireless Play, the same as two PSPs.");
	ImGui::Spacing();
	if (!split_active()) { if (ImGui::Button("Start player 2 (F9)")) { split_start(); } }
	else if (ImGui::Button("Stop player 2 (F9)")) split_stop();
	const char *layouts[] = {"Side by side", "Top and bottom"};
	if (ImGui::Combo("Layout", &g_cfg.split_layout, layouts, 2)) { changed(); fe_split_layout_changed(); }
	ImGui::Separator();
	ImGui::TextDisabled("How to start a race");
	ImGui::BulletText("Both players: Multiplayer > Wireless Play.");
	ImGui::BulletText("Player 1: Create, then pick a character, stage and wager.");
	ImGui::BulletText("Player 2: Join, pick player 1's game, then a character.");
	ImGui::TextDisabled("Controls");
	ImGui::BulletText("Two controllers: one each.");
	ImGui::BulletText("One controller: player 1 on the keyboard, player 2 on the controller.");
	ImGui::TextDisabled("Player 2 has its own save data, first copied from yours. Its sound is muted.");
}

static void split_tab() {
	ImGui::TextWrapped("Up to four players in one game, each with their own controller and part of the screen. "
	                   "Player 1 picks a character in the game as usual; pick the others here.");
	ImGui::Spacing();
	const char *counts[] = {"1 (off)", "2", "3", "4"};
	int n = g_cfg.mp_players - 1;
	if (ImGui::Combo("Players", &n, counts, 4)) { g_cfg.mp_players = n + 1; changed(); }
	int *chars[] = {&g_cfg.mp_char2, &g_cfg.mp_char3, &g_cfg.mp_char4};
	for (int p = 2; p <= g_cfg.mp_players; p++) {
		char l[32]; snprintf(l, sizeof l, "Player %d", p);
		if (ImGui::Combo(l, chars[p - 2], mp_char_names, MP_CHARS)) changed();
	}
	if (ImGui::Checkbox("Debug: player 1's controller drives everyone", &g_cfg.pad_all)) changed();
	help("For testing on your own: every player copies controller 1 (and the keyboard).");
	ImGui::Separator();
	ImGui::TextDisabled("How to play");
	ImGui::BulletText("Changes take effect at the next race load.");
	ImGui::BulletText("Start a Free Play race. Players 2-4 use controllers 2-4.");
	ImGui::BulletText("Two players split the screen top and bottom; three or four, into quarters.");
	ImGui::TextDisabled("Experimental: the HUD and placings only follow player 1.");
}

extern "C" void menu_draw(int w, int h, float fps, float game_fps) {
	if (!g_ready) return;
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();
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
			if (ImGui::BeginTabItem("Split screen", nullptr, fl(4))) { split_tab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Two windows", nullptr, fl(3))) { players_tab(); ImGui::EndTabItem(); }
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
