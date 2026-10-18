// Sonic Rivals 2 game layer: settings persistence, plus no-op versions of the hooks the shared
// runtime (frontend, menu, HLE) calls into. Game-specific features get added here.
#define _GNU_SOURCE
#include "kernel.h"
#include "config.h"
#include <sys/stat.h>
#include <math.h>

RcsmConfig g_cfg = {
	.renderer = 1, .internal_scale = 2, .tex_filter = 0, .anisotropy = 4, .vsync = true, .fullscreen = false,
	.window_scale = 2, .keep_aspect = true, .fps = 60, .show_fps = false,
	.strafe_mode = 0, .right_stick_camera = true, .free_camera = false, .invert_camera_y = false, .camera_speed = 1.0f,
	.stick_deadzone = 15, .modern_aim = false, .mouse_look = false, .fov = RCSM_ORIG_FOV, .camera_distance = 1.0f, .draw_distance = 1.0f,
	.volume = 100, .lighting = 0, .shadow_quality = 1, .light_intensity = 1.0f,
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
	X(window_scale, i) X(keep_aspect, b) X(fps, i) X(show_fps, b) X(strafe_mode, i) X(right_stick_camera, b) \
	X(free_camera, b) X(invert_camera_y, b) X(camera_speed, f) X(stick_deadzone, i) X(modern_aim, b) X(mouse_look, b) X(fov, f) X(camera_distance, f) X(draw_distance, f) X(volume, i) X(lacerator_mod, b) X(lacerator_fire_mult, f) X(lacerator_damage_mult, f) \
	X(lacerator_ammo, i) X(mini_turret, b) X(riptide, b) X(extra_skin, i) X(extra_skin_colour, i) X(extra_skin_under, i) X(no_backpack_clank, b) \
	X(lighting, i) X(specular, b) X(shadows, b) X(shadow_quality, i) X(light_intensity, f)

static void config_set(char *k, const char *v) {
	while (*k == ' ') k++;
	for (char *e = k + strlen(k); e > k && (e[-1] == ' '); ) *--e = 0;
#define X(name, t) if (!strcmp(k, #name)) { if (#t[0] == 'f') g_cfg.name = strtof(v, NULL); else g_cfg.name = atoi(v); }
	CFG_FIELDS(X)
#undef X
	if (!strcmp(k, "strafe_on_triggers")) g_cfg.strafe_mode = atoi(v) ? 1 : 0;   // older settings files
}

void config_load(void) {
	FILE *f = fopen(config_path(), "r");
	bool existed = f != NULL;
	char line[256];
	bool has_strafe_mode = false;
	while (f && fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (!eq || line[0] == '#' || line[0] == '[') continue;
		*eq = 0;
		if (strstr(line, "strafe_mode")) has_strafe_mode = true;
		if (has_strafe_mode && strstr(line, "strafe_on_triggers")) continue;
		config_set(line, eq + 1);
	}
	if (f) fclose(f);
	if (!existed) config_save();
	if (g_cfg.internal_scale < 1) g_cfg.internal_scale = 1;
	if (g_cfg.internal_scale > 8) g_cfg.internal_scale = 8;
	if (g_cfg.fps != 30) g_cfg.fps = 60;
	if (g_cfg.lacerator_fire_mult < 0.25f) g_cfg.lacerator_fire_mult = 0.25f;
	if (g_cfg.lacerator_fire_mult > 4) g_cfg.lacerator_fire_mult = 4;
	if (g_cfg.lacerator_ammo < 1) g_cfg.lacerator_ammo = 60;
	if (g_cfg.strafe_mode < 0 || g_cfg.strafe_mode > 2) g_cfg.strafe_mode = 1;
	if (!(g_cfg.fov >= 20 && g_cfg.fov <= 90)) g_cfg.fov = RCSM_ORIG_FOV;
	if (!(g_cfg.camera_distance >= 0.5f && g_cfg.camera_distance <= 3)) g_cfg.camera_distance = 1;
	if (!(g_cfg.camera_speed >= 0.1f && g_cfg.camera_speed <= 5)) g_cfg.camera_speed = 1;
	// environment overrides for testing
	const char *e;
	if ((e = getenv("RCSM_FPS"))) g_cfg.fps = atoi(e) == 30 ? 30 : 60;
	if ((e = getenv("RCSM_GPU"))) g_cfg.renderer = !strcmp(e, "soft") ? 0 : 1;
	if ((e = getenv("RCSM_GL_SCALE"))) g_cfg.internal_scale = atoi(e);
	if ((e = getenv("RCSM_LACERATOR"))) g_cfg.lacerator_mod = atoi(e) != 0;
	if ((e = getenv("RCSM_CFG"))) {   // "key=value,key=value"
		char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", e);
		for (char *sv, *t = strtok_r(tmp, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) {
			char *eq = strchr(t, '=');
			if (eq) { *eq = 0; config_set(t, eq + 1); }
		}
	}
}

void config_save(void) {
	FILE *f = fopen(config_path(), "w");
	if (!f) return;
	fprintf(f, "# sr2recomp settings (also editable in-game: F1 / Esc / controller Guide button)\n");
#define X(name, t) if (#t[0] == 'f') fprintf(f, "%s = %g\n", #name, (double)g_cfg.name); else fprintf(f, "%s = %d\n", #name, (int)g_cfg.name);
	CFG_FIELDS(X)
#undef X
	fclose(f);
}

// ---------------------------------------------------------------- hooks used by the shared runtime
void mods_config_changed(void) {}
void mods_vblank(void) {}
float mods_crosshair(void) { return 0; }
float mods_aim_t(void) { return 0; }
u32 rt_hookval(Ctx *c, u32 id, u32 v) { return v; }
void rt_hookcall(Ctx *c, u32 id, void (*orig)(Ctx *c)) { orig(c); }
void fly_filter_pad(u32 *buttons, u8 *lx, u8 *ly) {}
bool io_virtual_file(const char *psp, const unsigned char **data, unsigned *size) { return false; }

// debug menu entries from the Size Matters port, not wired up for this game yet
bool g_god, g_noclip, g_freecam, g_skyboard, g_play_clank;
float g_fly_speed = 1;
bool debug_in_level(void) { return false; }
int debug_current_level(void) { return -1; }
void debug_warp(int level) {}
int debug_weapon_info(int i, const char **name, int *owned, int *level, int *ammo, int *max_ammo) { return 0; }
void debug_unlock(int type_mask) {}
void debug_set_owned(int i, bool on) {}
void debug_set_weapon_level(int i, int level) {}
void debug_level_all(int level) {}
void debug_refill_all(void) {}
void debug_unlock_armour(void) {}
void debug_unlock_skins(void) {}
void armour_request(int r) {}
int armour_worn_mask(void) { return 0; }
int skins_count(void) { return 0; }
const char *skins_name(int k) { return ""; }
const char *skins_colour_name(int k, int c) { return ""; }
int skins_colour_slot(int k, int c) { return 0; }
bool skins_ready(void) { return false; }
int clank_status(void) { return 0; }
