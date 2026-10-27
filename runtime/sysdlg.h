// The PSP's system dialogs (sceUtility): save/load/delete, messages, and the network and game sharing
// dialogs, run by sysdlg.c and drawn by the overlay (menu.cpp) in the game's menu style.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif

// One save a dialog shows: an existing save directory, or an empty slot the game named.
typedef struct SdSlot {
	char name[21];          // the save name (the directory without the game name)
	char dir[64];           // the directory under ms0:/PSP/SAVEDATA
	int64_t size;           // all its files; 0: empty slot
	bool broken;            // no readable PARAM.SFO
	time_t mtime;           // when it was saved (PARAM.SFO's time)
	char title[128], save_title[128], detail[1024];
	uint8_t *icon;          // ICON0.PNG (or the game's icon for a new save), malloc'd, or NULL
	uint32_t icon_size;
} SdSlot;

enum { SYSDLG_NONE, SYSDLG_SAVEDATA, SYSDLG_MSG };

// What a dialog shows this frame
typedef struct SysDlgView {
	int kind;                    // SYSDLG_*: NONE draws nothing
	float alpha;                 // the fade, 0 to 1
	const char *heading;         // "Save", "Load", "Delete" (save data only)
	const SdSlot *slots;         // the saves; list: pick one of nslots, else slots[sel] alone (none if !nslots)
	int nslots, sel;
	int nall;                    // all of slots (for their icons), whatever this frame shows
	bool list;
	bool show_new;               // describe the save about to be made: new_title, new_detail, new_size
	const char *new_title, *new_detail;
	uint32_t new_size;
	const char *message;         // the message (with \n), or NULL
	int buttons;                 // under the message: 0 none, 1 OK, 2 Yes / No
	int choice;                  // Yes / No: 1 Yes
	bool ok_cross;               // the confirm button is cross (else circle)
	const char *ok_label;        // the footer's buttons, NULL to leave one out
	const char *back_label;
	unsigned serial;             // changes whenever the slots (and their icons) do
} SysDlgView;

void sysdlg_init(void);                 // registers the sceUtility functions
const SysDlgView *sysdlg_view(void);    // for drawing, from the frontend thread between game frames
bool sysdlg_active(void);               // a dialog is up (native or PPSSPP's)
const char *sysdlg_nickname(void);      // the system nickname: SR2_NICKNAME or PLAYER
uint8_t *sysdlg_icon_rgba(const SdSlot *s, int *w, int *h);   // a save's icon as RGBA (free it), or NULL

#ifdef __cplusplus
}
#endif
