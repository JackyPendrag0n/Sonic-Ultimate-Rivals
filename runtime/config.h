// PC settings shared by the frontend, the settings overlay and the game's own Options screen.
// Saved to ~/.config/sr2recomp/settings.ini.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct GameConfig {
	// graphics
	int renderer;          // 0 software, 1 OpenGL            (restart required)
	int internal_scale;    // 1..8 (OpenGL)
	int tex_filter;        // 0 auto, 1 nearest, 2 linear
	int anisotropy;        // 0,2,4,8,16
	bool vsync;
	bool fullscreen;
	int window_scale;      // 1..6
	bool keep_aspect;      // letterbox to 480:272
	int fps;               // race frame rate: 30 (original), 60, 120, 144, 165, 240
	bool show_fps;
	// controls
	int stick_deadzone;    // 0..100 %
	// audio
	int volume;            // 0..100
} GameConfig;

extern GameConfig g_cfg;
void config_load(void);
void config_save(void);
const char *config_path(void);

#ifdef __cplusplus
}
#endif
