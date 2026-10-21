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
	// local 2 players
	int split_layout;      // 0 side by side, 1 top and bottom
	// local 2-4 players in one game (game.c, extra racers)
	int mp_players;        // 1..4
	int mp_char2, mp_char3, mp_char4;   // characters of players 2-4 (index into mp_char_names)
	bool pad_all;          // debug: player 1's controller drives all players
	bool mp_test_seats;    // debug: player 1 adds players without a controller on the Multiplayer screen
} GameConfig;

extern GameConfig g_cfg;
void config_load(void);
void config_save(void);
const char *config_path(void);
#define MP_CHARS 8
extern const char *const mp_char_names[MP_CHARS], *const mp_char_files[MP_CHARS];

#ifdef __cplusplus
}
#endif
