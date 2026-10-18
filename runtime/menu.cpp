// In-game PC settings menu (Dear ImGui overlay). Opened with F1 / Esc / controller Guide; pauses the game.
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
// mods.c (debug menu)
bool debug_in_level(void);
int debug_current_level(void);
void debug_warp(int level);
int debug_weapon_info(int i, const char **name, int *owned, int *level, int *ammo, int *max_ammo);
void debug_unlock(int type_mask);
void debug_set_owned(int i, bool on);
void debug_set_weapon_level(int i, int level);
void debug_level_all(int level);
void debug_refill_all(void);
void debug_unlock_armour(void);
void debug_unlock_skins(void);
extern bool g_god, g_noclip, g_freecam, g_skyboard;
extern float g_fly_speed;
// armour.c (Riptide)
void armour_request(int r);
int armour_worn_mask(void);
// skins.c (multiplayer skins)
int skins_count(void);
const char *skins_name(int k);
const char *skins_colour_name(int k, int c);
int skins_colour_slot(int k, int c);
bool skins_ready(void);
// clank.c
extern bool g_play_clank;
int clank_status(void);
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

static void changed() { g_dirty = true; mods_config_changed(); }

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
	int fps_i = g_cfg.fps == 60 ? 1 : 0;
	const char *fpss[] = {"30 (original)", "60"};
	if (ImGui::Combo("Frame rate", &fps_i, fpss, 2)) { g_cfg.fps = fps_i ? 60 : 30; changed(); }
	help("60 fps runs the game's simulation in 1/60 s steps during gameplay. Menus stay at 30 fps. "
	     "Some effects tied to frame counts may run differently; switch back to 30 if something looks off.");
	if (ImGui::Checkbox("VSync", &g_cfg.vsync)) changed();
	if (ImGui::Checkbox("Fullscreen (F11)", &g_cfg.fullscreen)) { changed(); fe_apply_display_settings(); }
	ImGui::BeginDisabled(g_cfg.fullscreen);
	if (ImGui::SliderInt("Window scale", &g_cfg.window_scale, 1, 6, "%dx")) { changed(); fe_apply_display_settings(); }
	ImGui::EndDisabled();
	if (ImGui::Checkbox("Keep 480:272 aspect ratio", &g_cfg.keep_aspect)) changed();
	if (ImGui::Checkbox("Show FPS counter", &g_cfg.show_fps)) changed();
	ImGui::SeparatorText("Lighting (OpenGL)");
	const char *lmodes[] = {"Original (PSP)", "Enhanced"};
	if (ImGui::Combo("Lighting", &g_cfg.lighting, lmodes, 2)) changed();
	help("Enhanced: everything lit by the level's sun is re-shaded per pixel, with surface detail taken from the "
	     "textures, real-time shadows from the sun (Ratchet, enemies, crates, buildings...) and specular "
	     "highlights. Indoor areas without a sun keep the original lighting. Needs the OpenGL renderer.");
	ImGui::BeginDisabled(g_cfg.lighting == 0);
	if (ImGui::Checkbox("Real-time shadows", &g_cfg.shadows)) changed();
	ImGui::SameLine();
	if (ImGui::Checkbox("Specular highlights", &g_cfg.specular)) changed();
	const char *sq[] = {"Low (1024, short range)", "Medium (2048)", "High (3072, long range)"};
	if (ImGui::Combo("Shadow quality", &g_cfg.shadow_quality, sq, 3)) changed();
	if (ImGui::SliderFloat("Light intensity", &g_cfg.light_intensity, 0.0f, 2.0f, "%.1fx")) changed();
	ImGui::EndDisabled();
}

static void controls_tab() {
	const char *styles[] = {"Classic (PSP) + Ratchet 3 strafe", "Modern (PS4 / Rift Apart style)"};
	int style = g_cfg.modern_aim ? 1 : 0;
	if (ImGui::Combo("Shooting style", &style, styles, 2)) { g_cfg.modern_aim = style == 1; changed(); }
	help("Modern: hold L2 to aim - the camera moves over Ratchet's shoulder and zooms in, he draws his gun, strafes "
	     "and turns to face the crosshair; R2 fires toward the crosshair (firing without aiming also turns him to it). "
	     "Keyboard: hold Z to aim, L to fire, or use the mouse (below). "
	     "Classic: the PSP controls, with optional Ratchet 3 style strafing on L2/R2.");
	if (g_cfg.modern_aim) {
		if (ImGui::Checkbox("Mouse look", &g_cfg.mouse_look)) changed();
		help("Modern style on keyboard: the mouse turns the camera, right button aims, left button fires. "
		     "The pointer is captured while playing (F1 / Esc frees it).");
	} else {
		const char *strafe_modes[] = {"Off (L2/R2 = PSP L/R)", "Hold L2 / R2", "Toggle with L2 / R2"};
		if (ImGui::Combo("Strafe (Ratchet 3 style)", &g_cfg.strafe_mode, strafe_modes, 3)) changed();
		help("While strafing, Ratchet keeps facing the same way and the left stick moves him in any direction at analog "
		     "speed (like Up Your Arsenal); pulling back walks him backwards. As in the PSP game this works with a gun "
		     "out, not with the wrench. Hold: strafe while L2 or R2 is held. Toggle: press L2 or R2 to switch strafing "
		     "on/off. Keyboard: Z or C. The d-pad keeps the PSP's own d-pad strafing.");
	}
	if (ImGui::Checkbox("Right stick moves the camera", &g_cfg.right_stick_camera)) changed();
	help("Left/right turns the camera (the PSP used L/R for this). Up/down tilts it if 'Free camera' is on "
	     "(Camera tab). Keyboard: Q/E turn, R/F tilt.");
	if (ImGui::SliderInt("Stick deadzone", &g_cfg.stick_deadzone, 0, 50, "%d%%")) changed();
	ImGui::Separator();
	ImGui::TextDisabled("Controller (SDL / Xbox layout)");
	ImGui::BulletText("Left stick: move     A: jump     X: wrench     B: fire     Y: quick select");
	if (g_cfg.modern_aim) ImGui::BulletText("Right stick: camera     L2: aim     R2: fire     LB / RB: turn camera");
	else ImGui::BulletText("Right stick: camera     L2 / R2: strafe     LB / RB: turn camera");
	ImGui::BulletText("Start / Back: Start / Select     Guide: this menu");
	ImGui::TextDisabled("Keyboard");
	ImGui::BulletText("WASD: move     Space or K: jump     J: wrench     L: fire     I: quick select");
	if (g_cfg.modern_aim) ImGui::BulletText("Z / C or right mouse: aim     left mouse: fire     mouse: camera     Q / E, R / F: camera");
	else ImGui::BulletText("Z / C: strafe     Q / E: turn camera     R / F: tilt camera     Arrows: d-pad");
	ImGui::BulletText("Enter: Start     Backspace: Select     F1 / Esc: this menu     F11: fullscreen     F12: screenshot");
}

static float hfov_deg(float vfov_deg) { return 2 * atanf(tanf(vfov_deg * 3.14159265f / 360) * 480.0f / 272.0f) * 180 / 3.14159265f; }

static void camera_tab() {
	if (ImGui::SliderFloat("Field of view", &g_cfg.fov, 25.0f, 80.0f, "%.1f\xc2\xb0 vertical")) changed();
	ImGui::SameLine(); ImGui::TextDisabled("(%.0f\xc2\xb0 horizontal)", hfov_deg(g_cfg.fov));
	help("Original: 31.5\xc2\xb0 vertical (53\xc2\xb0 horizontal). Higher values show more of the world around the edges.");
	if (ImGui::SliderFloat("Camera distance", &g_cfg.camera_distance, 0.6f, 2.5f, "%.2fx")) changed();
	help("How far behind Ratchet the camera follows (original 1.0x). The camera still pulls in near walls.");
	if (ImGui::SliderFloat("Draw distance", &g_cfg.draw_distance, 1.0f, 4.0f, "%.2fx")) changed();
	help("How far away objects (scenery, crates, enemies, props) are still drawn, as a multiple of each object's "
	     "original draw distance. 1.0x is the PSP original; things pop in further away the higher it is. "
	     "The level's own fog and the terrain aren't affected. Changes apply immediately.");
	if (ImGui::Button("Original camera")) { g_cfg.fov = RCSM_ORIG_FOV; g_cfg.camera_distance = 1.0f; changed(); }
	ImGui::SameLine();
	if (ImGui::Button("Recommended")) { g_cfg.fov = 40.0f; g_cfg.camera_distance = 1.3f; changed(); }
	ImGui::Separator();
	ImGui::BeginDisabled(!g_cfg.right_stick_camera);
	if (ImGui::Checkbox("Free camera (tilt up/down)", &g_cfg.free_camera)) changed();
	help("Right stick up/down (keyboard R/F) tilts the camera, in addition to turning it left/right. The tilt stays "
	     "where you leave it.");
	ImGui::BeginDisabled(!g_cfg.free_camera);
	if (ImGui::Checkbox("Invert up/down", &g_cfg.invert_camera_y)) changed();
	ImGui::EndDisabled();
	if (ImGui::SliderFloat("Camera speed", &g_cfg.camera_speed, 0.25f, 3.0f, "%.2fx")) changed();
	ImGui::EndDisabled();
	if (!g_cfg.right_stick_camera) ImGui::TextDisabled("Turn on 'Right stick moves the camera' in Controls.");
}

static const char *weapon_title(const char *n) {
	static const struct { const char *k, *v; } names[] = {
		{"blaster", "Lacerator"}, {"blitzgun", "Concussion Gun"}, {"bombglove", "Acid Bomb Glove"},
		{"agentsglove", "Agents of Doom"}, {"beemineglove", "Bee Mine Glove"}, {"shieldcharger", "Static Barrier"},
		{"shockrocket", "Shock Rocket"}, {"crossbowgun", "Sniper Mine"}, {"flamethrower", "Scorcher"},
		{"lasertracer", "Laser Tracer"}, {"suckcannon", "Suck Cannon"}, {"mootator", "Mootator"},
		{"miniturretglove", "Mini-Turret Glove"}, {"ryno", "R.Y.N.O. IV"}, {"hypershot", "Hypershot"},
		{"sproutomatic", "Sprout-O-Matic"}, {"polarizer", "Polariser"}, {"pda", "PDA"}, {"shrinkray", "Shrink Ray"},
		{"boltgrabber", "Bolt Grabber"}, {"grindboots", "Grind Boots"}, {"mapomatic", "Map-O-Matic"},
		{"boxbreaker", "Box Breaker"}, {"miniturretglove", "Mini-Turret Glove"}};
	for (auto &e : names) if (!strcmp(n, e.k)) return e.v;
	return n;
}

static void debug_tab() {
	static const struct { int idx; const char *name; } levels[] = {
		{1, "Pokitaru"}, {2, "Ryllus"}, {3, "Kalidon"}, {4, "Metalis"}, {5, "Dreamtime"},
		{6, "Medical Outpost Omega"}, {7, "Challax"}, {8, "Dayni Moon"}, {9, "Inside Clank"}, {10, "Quodrona"},
		{15, "Giant Clank battle 1"}, {21, "Giant Clank battle 2"}, {22, "Skyboard race (Kalidon)"},
		{23, "Skyboard race (Medical Outpost)"}, {0, "Main menu"},
		{16, "Multiplayer map 1"}, {17, "Multiplayer map 2"}, {18, "Multiplayer map 3"}, {19, "Multiplayer map 4"},
		{20, "Multiplayer lobby"}, {24, "HIG Treehouse (developer level)"}};
	bool in = debug_in_level();
	if (!in) { ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Available while playing a level."); return; }
	ImGui::TextDisabled("Cheats change your current game; saving keeps them.");
	if (ImGui::CollapsingHeader("Level select", ImGuiTreeNodeFlags_DefaultOpen)) {
		static int sel = -1;
		int cur = debug_current_level();
		if (sel < 0) { sel = 0; for (int i = 0; i < (int)(sizeof levels / sizeof levels[0]); i++) if (levels[i].idx == cur) sel = i; }
		const char *preview = levels[sel].name;
		if (ImGui::BeginCombo("Level", preview)) {
			for (int i = 0; i < (int)(sizeof levels / sizeof levels[0]); i++) {
				if (i == 15) ImGui::SeparatorText("multiplayer: start these from Multiplayer Game instead");
				char lbl[96]; snprintf(lbl, sizeof lbl, "%s%s", levels[i].name, levels[i].idx == cur ? "  (current)" : "");
				if (ImGui::Selectable(lbl, sel == i)) sel = i;
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::Button("Go")) { debug_warp(levels[sel].idx); menu_set_open(false); }
		help("Leaves the current level and loads the chosen one at its start (like travelling there). Story progress "
		     "isn't changed, so skipping ahead can leave things in an odd state. Warping straight into a multiplayer "
		     "map skips the game setup and leaves you without a character: to run around in them, use the main "
		     "menu's Multiplayer Game > Ad-hoc > Create Game > Start instead.");
	}
	if (ImGui::CollapsingHeader("Movement & camera", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("God mode (F5)", &g_god);
		help("Your health stays full, whatever hits you. Bottomless pits and other instant deaths still respawn you.");
		ImGui::Checkbox("Noclip (F6)", &g_noclip);
		help("Ratchet floats and passes through anything. Left stick / WASD moves him where the camera looks, "
		     "jump (Space / A) rises, wrench (J / X) or Left Ctrl sinks. Hold Left Shift or click the left stick to go faster. "
		     "Switching it off drops him where he is.");
		ImGui::Checkbox("Free camera (F7)", &g_freecam);
		help("A camera you fly on its own, away from Ratchet (he stands still meanwhile). Left stick / WASD moves, "
		     "right stick, mouse, Q/E and R/F look around, jump rises, wrench or Left Ctrl sinks, Left Shift / left stick click "
		     "goes faster. Start still pauses. Switch it off to go back to the normal camera.");
		ImGui::Checkbox("Skyboard anywhere (F9, experimental)", &g_skyboard);
		help("Ratchet rides the skyboard from the skyboard races in any level (the board itself is only in the race "
		     "levels, so he surfs on thin air). Steer with the left stick, jump with CROSS. A chase camera follows him. "
		     "Falling off the world, or flying for more than 12 seconds, puts you back at the start of the level. "
		     "The race HUD shows while riding. Switch it off to step off.");
		ImGui::SetNextItemWidth(200);
		ImGui::SliderFloat("Flying speed", &g_fly_speed, 0.25f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
		help("How fast noclip and the free camera move.");
	}
	if (ImGui::CollapsingHeader("Armour & skins", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Button("Unlock all armour")) debug_unlock_armour();
		help("All 7 armour sets, every piece (helmet, body, gloves, boots). Equip them in the pause menu's Armour screen.");
		ImGui::SameLine();
		if (ImGui::Button("Unlock all skins")) debug_unlock_skins();
		help("Plundering Pirate Captain, Ratchetzilla, Super Incognito and Tropical Vacation. Equip them in Special > Skins.");
	}
	if (ImGui::CollapsingHeader("Weapons", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Button("Unlock all weapons")) debug_unlock(1 << 2);
		ImGui::SameLine();
		if (ImGui::Button("Unlock gadgets & items")) debug_unlock((1 << 1) | (1 << 3));
		help("Gadgets and items include story items such as the Shrink Ray; getting them early can skip parts of the story.");
		ImGui::SameLine();
		if (ImGui::Button("Refill ammo")) debug_refill_all();
		static int all_lvl = 3;
		ImGui::SetNextItemWidth(160);
		char afmt[16]; snprintf(afmt, sizeof afmt, "V%d", all_lvl + 1);
		ImGui::SliderInt("##alllvl", &all_lvl, 0, 7, afmt, ImGuiSliderFlags_AlwaysClamp);
		ImGui::SameLine();
		char b[64]; snprintf(b, sizeof b, "Set all owned weapons to V%d", all_lvl + 1);
		if (ImGui::Button(b)) debug_level_all(all_lvl);
		help("V4 is the highest level in a first playthrough. V5-V8 normally need Challenge Mode upgrades.");
		if (ImGui::BeginTable("weapons", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0, 220))) {
			ImGui::TableSetupColumn("Owned", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Name");
			ImGui::TableSetupColumn("Level");
			ImGui::TableSetupColumn("Ammo");
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();
			for (int i = 0; i < 25; i++) {
				const char *n; int owned, lvl, ammo, mx;
				int t = debug_weapon_info(i, &n, &owned, &lvl, &ammo, &mx);
				if (!t) continue;
				ImGui::PushID(i);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				bool o = owned;
				if (ImGui::Checkbox("##own", &o)) debug_set_owned(i, o);
				ImGui::TableNextColumn();
				ImGui::Text("%s%s", weapon_title(n), t == 1 ? "  (gadget)" : t == 3 ? "  (item)" : "");
				ImGui::TableNextColumn();
				if (t == 2) {
					int v = lvl;
					ImGui::SetNextItemWidth(-FLT_MIN);
					char fmt[16]; snprintf(fmt, sizeof fmt, "V%d", lvl + 1);
					if (ImGui::SliderInt("##lvl", &v, 0, 7, fmt, ImGuiSliderFlags_AlwaysClamp)) debug_set_weapon_level(i, v);
				}
				ImGui::TableNextColumn();
				if (t == 2) ImGui::Text("%d / %d", ammo, mx);
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}
}

static void mods_tab() {
	if (ImGui::Checkbox("Lacerator rework", &g_cfg.lacerator_mod)) changed();
	ImGui::BeginDisabled(!g_cfg.lacerator_mod);
	if (ImGui::SliderFloat("Fire rate", &g_cfg.lacerator_fire_mult, 1.0f, 3.0f, "%.2fx")) changed();
	if (ImGui::SliderFloat("Damage", &g_cfg.lacerator_damage_mult, 0.2f, 1.5f, "%.2fx")) changed();
	if (ImGui::SliderInt("Max ammo (level 1)", &g_cfg.lacerator_ammo, 60, 300)) changed();
	help("Upgraded levels scale by the same factor (original 60/70/80/120). Ammo pickups scale too. "
	     "Current ammo isn't topped up instantly; pick up ammo or start a new game.");
	ImGui::EndDisabled();
	ImGui::Separator();
	if (ImGui::Checkbox("Mini-Turret Glove (restored weapon)", &g_cfg.mini_turret)) changed();
	help("The Mini-Turret Glove, cut from the game, restored as a full weapon: sold by vendors from Kalidon on for "
	     "30,000 bolts, 8 levels (V.8 is the Mega-Turret Glove), on the Weapons and Quick Select screens. Its real "
	     "turret code and models only exist in the developers' test level, so it's built on the Bee Mine Glove, "
	     "which the developers say it played like. Takes effect on the next level load.");
	ImGui::TextDisabled("Lacerator changes apply immediately.");
	ImGui::Separator();
	if (ImGui::Checkbox("Riptide armour (new armour set)", &g_cfg.riptide)) changed();
	help("Given to you at the start of every game and worn on any empty armour slots. Same defence as Electroshock. "
	     "In the pause menu's Armour screen it lives in the Electroshock column: CROSS cycles Riptide, Electroshock "
	     "(once you own it) and nothing. Turning this off shows Electroshock instead (the stats don't change).");
	if (g_cfg.riptide && ImGui::TreeNodeEx("About the Riptide set", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
		ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.9f, 1), "RIPTIDE");
		ImGui::TextWrapped("Gadgetron's deep-water division built the Riptide set for Pokitaru's lifeguard corps after one "
		                   "too many \"incidents\" at the Jowai Resort. Pressure-forged hydro-alloy plates ride on a "
		                   "lining of living tide-gel, and the fins aren't for show: they bleed off impact energy the way "
		                   "a breakwater turns a wave. Captain Qwark returned his set after it \"made him look damp\".");
		ImGui::Spacing();
		ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1), "Defence");
		ImGui::TextWrapped("  The same as Electroshock (full set: damage reduction 74).");
		ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1), "Breaker (full set)");
		ImGui::TextWrapped("  Jump and slam your wrench to bring down a ring of crashing surf. The "
		                                      "wrench trails sea spray instead of sparks.");
		ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1), "Tidal Mend (full set)");
		ImGui::TextWrapped("  After 4 seconds out of harm the tide-gel slowly restores your health "
		                                      "(3%% of your maximum per second).");
		ImGui::PopTextWrapPos();
		ImGui::TreePop();
	}
	if (g_cfg.riptide) {
		int m = debug_in_level() ? armour_worn_mask() : -1;
		if (m < 0) ImGui::TextDisabled("Wear / take off: available while playing a level.");
		else {
			static const char *pn[4] = {"helmet", "body", "gloves", "boots"};
			static const int pm[4] = {0x02, 0x01, 0x0c, 0x30};
			char b[96] = ""; int n = 0;
			for (int i = 0; i < 4; i++) if ((m & pm[i]) == pm[i]) { strcat(b, n ? ", " : ""); strcat(b, pn[i]); n++; }
			ImGui::Text("Wearing: %s", m == 0x3f ? "the full set" : n ? b : "none");
			if (m != 0x3f) { if (ImGui::Button("Wear the full Riptide set")) armour_request(1); }
			if (m) { if (m != 0x3f) ImGui::SameLine(); if (ImGui::Button("Take off Riptide")) armour_request(2); }
			help("Takes effect when the game resumes. Wearing the full set replaces any other armour you have on.");
		}
	}

	ImGui::SeparatorText("Extra skins");
	{
		int n = skins_count(), k = g_cfg.extra_skin;
		if (k < 0 || k > n) k = 0;
		if (ImGui::BeginCombo("Skin", k ? skins_name(k - 1) : "Off (the game's own skin)")) {
			for (int i = 0; i <= n; i++)
				if (ImGui::Selectable(i ? skins_name(i - 1) : "Off (the game's own skin)", i == k)) { g_cfg.extra_skin = i; changed(); }
			ImGui::EndCombo();
		}
		if (k && skins_ready()) {
			int cur = skins_colour_slot(k - 1, g_cfg.extra_skin_colour);
			if (ImGui::BeginCombo("Colour", skins_colour_name(k - 1, cur))) {
				for (int c = 0; c < 8; c++) {
					const char *nm = skins_colour_name(k - 1, c);
					if (nm[0] == '-') continue;
					ImGui::PushID(c);
					if (ImGui::Selectable(nm, c == cur)) { g_cfg.extra_skin_colour = c; changed(); }
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}
		}
		help("The eight multiplayer characters (they share Ratchet's skeleton and animations), in any of their team "
		     "colours, and Clank (his model fitted to Ratchet's skeleton), worn in the story like the skins in "
		     "Special > Skins. Takes effect when the game resumes. Picking a skin in Special > Skins takes this one "
		     "off. Your save keeps its own skin.");
	}

	ImGui::SeparatorText("Play as Clank");
	if (ImGui::Checkbox("No backpack Clank", &g_cfg.no_backpack_clank)) changed();
	help("Ratchet goes without Clank on his back, as on Inside Clank. Saved with the settings.");
	ImGui::Checkbox("Play as Clank (F8)", &g_play_clank);
	{
		int st = clank_status();
		if (st == 1) ImGui::TextDisabled("This level has Clank: you play as the real Clank.");
		else if (st == 2) ImGui::TextDisabled("No Clank in this level: you wear Clank's look on Ratchet's moves.");
		else if (st == 3) ImGui::TextDisabled("You're already playing as Clank here.");
	}
	help("Ryllus has Clank's own controls, model and animations (his sections are there), so there you swap to the "
	     "real Clank, using the game's own switch. Every other level only has Ratchet's code, so there you get Clank's "
	     "model on Ratchet's skeleton: Clank's look with Ratchet's moves, weapons and gadgets. Takes effect when the "
	     "game resumes. Switch it off to get Ratchet back.");
}

extern "C" float mods_crosshair(void);
extern "C" float mods_aim_t(void);
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
	{	// modern aiming crosshair (screen centre = the game image centre)
		float a = mods_crosshair();
		if (a > 0.02f && !g_open) {
			ImDrawList *dl = ImGui::GetForegroundDrawList();
			float cx = w * 0.5f, cy = h * 0.5f, u = h / 272.0f, ai = mods_aim_t();
			float r = (7.5f - 2.5f * ai) * u;
			ImU32 col = IM_COL32(255, 255, 255, (int)(230 * a)), sh = IM_COL32(0, 0, 0, (int)(140 * a));
			dl->AddCircle(ImVec2(cx, cy), r, sh, 32, 2.6f * u * 0.5f + 1.5f);
			dl->AddCircle(ImVec2(cx, cy), r, col, 32, 1.2f * u * 0.5f + 0.8f);
			for (int i = 0; i < 4; i++) {   // four short ticks outside the ring
				float dx = (i == 0) - (i == 1), dy = (i == 2) - (i == 3);
				ImVec2 p0(cx + dx * (r + 2 * u), cy + dy * (r + 2 * u)), p1(cx + dx * (r + 5 * u), cy + dy * (r + 5 * u));
				dl->AddLine(p0, p1, sh, 2.6f * u * 0.5f + 1.5f);
				dl->AddLine(p0, p1, col, 1.2f * u * 0.5f + 0.8f);
			}
			dl->AddCircleFilled(ImVec2(cx, cy), 1.1f * u, col, 12);
		}
	}
	if (g_open) {
		ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(w < 760 ? w * 0.95f : 720, h < 660 ? h * 0.92f : 600), ImGuiCond_Always);
		ImGui::Begin("Settings  (game paused)", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
		static int force_tab = getenv("RCSM_MENU_TAB") ? atoi(getenv("RCSM_MENU_TAB")) : -1;  // test aid
		auto fl = [&](int i) { return force_tab == i ? ImGuiTabItemFlags_SetSelected : 0; };
		if (ImGui::BeginTabBar("tabs")) {
			if (ImGui::BeginTabItem("Graphics", nullptr, fl(0))) { graphics_tab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Controls", nullptr, fl(1))) { controls_tab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Camera", nullptr, fl(4))) { camera_tab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Audio", nullptr, fl(2))) { if (ImGui::SliderInt("Volume", &g_cfg.volume, 0, 100, "%d%%")) changed(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Mods", nullptr, fl(3))) { mods_tab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Debug", nullptr, fl(5))) { debug_tab(); ImGui::EndTabItem(); }
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
