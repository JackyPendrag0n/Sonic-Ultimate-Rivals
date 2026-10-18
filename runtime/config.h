// PC settings shared by the frontend, the settings menu and the mods. Saved to ~/.config/rcsm/settings.ini.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct RcsmConfig {
	// graphics
	int renderer;          // 0 software, 1 OpenGL            (restart required)
	int internal_scale;    // 1..8 (OpenGL)
	int tex_filter;        // 0 auto, 1 nearest, 2 linear
	int anisotropy;        // 0,2,4,8,16
	bool vsync;
	bool fullscreen;
	int window_scale;      // 1..6
	bool keep_aspect;      // letterbox to 480:272
	int fps;               // 30 (original) or 60
	bool show_fps;
	// controls
	int strafe_mode;           // L2/R2 (keyboard Z/C): 0 off (they act as PSP L/R), 1 hold to strafe, 2 press to toggle
	bool right_stick_camera;   // right stick (keyboard: Q/E, R/F) moves the camera
	bool free_camera;          // camera can also be tilted up/down
	bool invert_camera_y;
	float camera_speed;        // right-stick camera speed multiplier
	int stick_deadzone;        // 0..100 %
	bool modern_aim;           // modern Ratchet shooting: L2 aims over the shoulder, R2 fires, Ratchet faces the crosshair
	bool mouse_look;           // modern aiming on keyboard+mouse: mouse turns the camera, right button aims, left fires
	// camera
	float fov;                 // vertical field of view in degrees (original 31.5)
	float camera_distance;     // follow-distance multiplier (original 1.0)
	float draw_distance;       // object draw-distance multiplier (original 1.0)
	// audio
	int volume;            // 0..100
	// mods
	bool lacerator_mod;
	float lacerator_fire_mult; // >1 = faster
	float lacerator_damage_mult;
	int lacerator_ammo;        // max ammo at level 1 (original 60)
	bool mini_turret;          // the cut Mini-Turret Glove (weapon slot 14) restored, built on the Bee Mine Glove
	bool riptide;              // new armour set (Riptide), given at the start; worn in the Electroshock column
	int extra_skin;            // 0 = the game's skin, 1.. = a multiplayer character worn in single player (skins.c)
	int extra_skin_colour;     // its texture slot (0 = default / first)
	bool no_backpack_clank;    // hide the Clank on Ratchet's back (skins.c)
	int extra_skin_under;      // the save's own equipped skin under it, + 1 (0 = not known)
	// lighting (OpenGL)
	int lighting;              // 0 original (PSP vertex lighting), 1 enhanced (per-pixel, specular, shadows)
	bool specular;
	bool shadows;
	int shadow_quality;        // 0 low, 1 medium, 2 high
	float light_intensity;     // enhanced sun light strength
} RcsmConfig;

#define RCSM_ORIG_FOV 31.5f

extern RcsmConfig g_cfg;
void config_load(void);
void config_save(void);
const char *config_path(void);
void mods_config_changed(void);   // re-apply live mod patches after a settings change

#ifdef __cplusplus
}
#endif
