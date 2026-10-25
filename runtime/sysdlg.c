// sceUtility: the PSP's system dialogs and settings, as Sonic Rivals 2 uses them.
//
// A game starts a dialog with an InitStart call and a request struct, calls its Update once a frame while
// polling GetStatus (0 none, 1 starting, 2 running, 3 finished, 4 shutting down), and ends it with
// ShutdownStart. The save data dialog does the save, load and delete work (savedata.c) and shows the save
// lists and messages; the message dialog shows the game's messages; the network dialog only "connects"
// (net.c is an ad hoc network with nobody else on it) and game sharing reports cancelled. The screens are
// drawn by the overlay (menu.cpp) from sysdlg_view(), in the game's menu style rather than the PSP's.
//
// The status timing (start and shutdown delays, Update and module load times), the flow of each screen and
// the results follow PPSSPP's sceUtility.cpp and Core/Dialog (GPL-2.0+), which were checked against
// hardware. SR2_PPSSPP_DIALOGS=1 runs PPSSPP's own dialogs instead.
#include <string.h>
#include <time.h>
#include "kernel.h"
#include "media.h"
#include "savedata.h"

#define ERR_INVALID_STATUS        0x80110001
#define ERR_INVALID_ADDRESS       0x80110002
#define ERR_INVALID_PARAM_SIZE    0x80110004
#define ERR_WRONG_TYPE            0x80110005
#define ERR_STRING_TOO_LONG       0x80110102
#define ERR_INVALID_SYSTEM_PARAM  0x80110103
#define ERR_MSG_BADOPTION         0x80110501
#define ERR_MSG_ERRORCODEINVALID  0x80110502
#define ERR_AV_MODULE_BAD_ID      0x80110F01
#define ERR_MODULE_BAD_ID         0x80111101
#define ERR_BAD_ARGUMENT          0x80000004
#define RESULT_CANCEL 1

enum { B_UP = 0x10, B_RIGHT = 0x20, B_DOWN = 0x40, B_LEFT = 0x80, B_CIRCLE = 0x2000, B_CROSS = 0x4000 };
enum { T_NONE, T_SAVE, T_MSG, T_NET, T_SHARE, T_COUNT };
enum { S_NONE, S_INIT, S_RUNNING, S_FINISHED, S_SHUTDOWN };

// ================================================================= the dialogs' common life
static int status[T_COUNT];
static int cur_type;          // the dialog started last: one runs at a time, whatever its type
static bool cur_active;       // it hasn't been shut down
static struct { bool on, init; int type; u64 at; } pending;   // the firmware's start/stop work, done after a delay

// The start and shutdown work runs on a firmware thread; here it completes once its time has passed,
// checked whenever the game calls in (it polls GetStatus while it waits).
static void run_pending(void) {
	if (!pending.on || now_us() < pending.at) return;
	pending.on = false;
	if (pending.init) { if (status[pending.type] == S_INIT) status[pending.type] = S_RUNNING; }
	else if (status[pending.type] == S_SHUTDOWN) status[pending.type] = S_NONE;
}
static void start_init(int type, int us) {
	status[type] = S_INIT;
	pending.on = true; pending.init = true; pending.type = type; pending.at = now_us() + us;
}
static void start_shutdown(int type, int us) {
	status[type] = S_SHUTDOWN;
	pending.on = true; pending.init = false; pending.type = type; pending.at = now_us() + us;
}
static bool busy(void) { return cur_type != T_NONE && status[cur_type] != S_NONE; }

static void delay(int us) { if (g_cur && !g_in_intr) thread_delay_until(now_us() + us); }

// A request's size must be one the PSP accepts
static u32 check_request(u32 p, const u32 *sizes, int n) {
	if (!media_valid(p, 48)) return ERR_INVALID_ADDRESS;
	u32 size = LW(p);
	bool ok = false;
	for (int i = 0; i < n; i++) ok |= size == sizes[i];
	if (!ok) return ERR_INVALID_PARAM_SIZE;
	if (!media_valid(p, size)) return ERR_INVALID_ADDRESS;
	if (p & 3) return ERR_BAD_ARGUMENT;
	return 0;
}

// Starting one dialog while another is still up fails; otherwise it becomes the current one
static u32 init_start(int type, u32 (*init)(u32), u32 p) {
	run_pending();
	if (busy()) return ERR_INVALID_STATUS;
	bool was_busy = status[type] != S_NONE;
	u32 r = init(p);
	if ((s32)r >= 0 || (!was_busy && status[type] != S_NONE)) { cur_type = type; cur_active = true; }
	return r;
}

static u32 get_status(int type) {
	run_pending();
	if (cur_type != type) return ERR_WRONG_TYPE;
	return status[type];
}

static u32 shutdown_start(int type, int us) {
	run_pending();
	if (cur_type != type) return ERR_WRONG_TYPE;
	cur_active = false;
	if (status[type] != S_FINISHED) return ERR_INVALID_STATUS;
	start_shutdown(type, us);
	return 0;
}

// ---- the pad, the fade
extern u32 fe_buttons;
static u32 btn_last, btn_pressed;
static int held_frames;
static void read_buttons(void) { u32 b = fe_buttons; btn_pressed = b & ~btn_last; btn_last = b; }

static struct { bool fading, in; float t, value; } fade;
static void fade_start(bool in) { fade.fading = true; fade.in = in; fade.t = 0; }
static bool fading_out(void) { return fade.fading && !fade.in; }
static bool pressed(u32 b) { return !fading_out() && (btn_pressed & b); }
// Up/down: a press, or held for half a second and then every sixth of one (PPSSPP's IsButtonHeld)
static bool pressed_or_held(u32 b) {
	if (pressed(b)) { held_frames = 0; return true; }
	if (fading_out() || !(btn_last & b)) return false;
	return ++held_frames >= 30 && held_frames % 10 == 0;
}
// Returns true when a fade out has just ended
static bool fade_update(int anim) {
	if (!fade.fading) return false;
	fade.t += (anim > 1 ? anim : 1) / 60.0f;
	if (fade.t < 0.2f) { fade.value = fade.in ? fade.t / 0.2f : 1 - fade.t / 0.2f; return false; }
	fade.value = fade.in ? 1 : 0;
	fade.fading = false;
	return !fade.in;
}

static bool ok_is_cross(u32 p) { return LW(p + 8) == 1; }   // the request's buttonSwap
static u32 ok_button(u32 p) { return ok_is_cross(p) ? B_CROSS : B_CIRCLE; }
static u32 back_button(u32 p) { return ok_is_cross(p) ? B_CIRCLE : B_CROSS; }

// ================================================================= save data
enum {
	DS_NONE, DS_SAVE_LIST, DS_SAVE_CONFIRM, DS_SAVE_OVERWRITE, DS_SAVE_SAVING, DS_SAVE_FAILED, DS_SAVE_DONE,
	DS_LOAD_LIST, DS_LOAD_CONFIRM, DS_LOAD_LOADING, DS_LOAD_FAILED, DS_LOAD_DONE, DS_LOAD_NODATA,
	DS_DELETE_LIST, DS_DELETE_CONFIRM, DS_DELETE_DELETING, DS_DELETE_FAILED, DS_DELETE_DONE, DS_DELETE_NODATA,
};
static const char *const mode_names[] = {
	"AUTOLOAD", "AUTOSAVE", "LOAD", "SAVE", "LISTLOAD", "LISTSAVE", "LISTDELETE", "LISTALLDELETE", "SIZES",
	"AUTODELETE", "DELETE", "LIST", "FILES", "MAKEDATASECURE", "MAKEDATA", "READDATASECURE", "READDATA",
	"WRITEDATASECURE", "WRITEDATA", "ERASESECURE", "ERASE", "DELETEDATA", "GETSIZE"};

static struct {
	u32 p;
	int mode, display;
	SdSlot *slots;
	int nslots;           // the saves listed (an empty single save is shown but not counted)
	int sel, choice;      // the save picked; Yes (1) or No
	int io_frames;        // frames left showing "Saving..." and the like
	int io_next;          // what shows after them
	bool io_started;      // DS_NONE: the work was done on the previous Update
	unsigned serial;
} sd;

static void sd_relist(void) {
	if (sd.slots) sd_free_slots(sd.slots, sd.nslots > 1 ? sd.nslots : 1);
	sd.nslots = sd_list_slots(sd.p, &sd.slots);
	sd.serial++;
}

static int focus_slot(u32 p) {
	int n = sd.nslots, best = 0;
	switch (LW(p + SD_FOCUS)) {
	case 0: {   // the request's save name
		char name[21];
		sd_field(p, SD_SAVE_NAME, 20, name);
		for (int i = 0; i < n; i++) if (!strcmp(sd.slots[i].name, name)) return i;
		return 0;
	}
	case 1: return 0;
	case 2: return n > 0 ? n - 1 : 0;
	case 3: case 4: {   // the latest / oldest save
		time_t t = 0;
		for (int i = 0; i < n; i++) {
			if (!sd.slots[i].size) continue;
			if (!t || (LW(p + SD_FOCUS) == 3 ? sd.slots[i].mtime > t : sd.slots[i].mtime < t)) { t = sd.slots[i].mtime; best = i; }
		}
		return best;
	}
	case 5: for (int i = 0; i < n; i++) if (sd.slots[i].size) return i; return 0;
	case 6: for (int i = n - 1; i >= 0; i--) if (sd.slots[i].size) return i; return 0;
	case 7: for (int i = 0; i < n; i++) if (!sd.slots[i].size) return i; return 0;
	case 8: for (int i = n - 1; i >= 0; i--) if (!sd.slots[i].size) return i; return 0;
	default: return 0;
	}
}

static u32 sd_init(u32 p) {
	static const u32 sizes[] = {1480, 1500, 1536};
	if (status[T_SAVE] != S_NONE) return ERR_INVALID_STATUS;
	u32 r = check_request(p, sizes, 3);
	if (r) { LOGE("sceUtilitySavedataInitStart: bad request at %08x: %08x", p, r); return r; }
	if (!sd_names_ok(p)) return ERR_INVALID_PARAM_SIZE;
	sd.p = p;
	sd.mode = LW(p + SD_MODE);
	sd.io_started = false;
	sd.io_frames = 0;
	sd.choice = 1;
	sd_relist();
	char game[14], save[21], file[14];
	sd_field(p, SD_GAME_NAME, 13, game);
	sd_field(p, SD_SAVE_NAME, 20, save);
	sd_field(p, SD_FILE_NAME, 13, file);
	LOGI("savedata: %s (%d) game '%s' save '%s' file '%s', %d listed", sd.mode >= 0 && sd.mode <= SD_GETSIZE ? mode_names[sd.mode] : "?",
	     sd.mode, game, save, file, sd.nslots);
	sd.sel = sd_multi_name(sd.mode) ? focus_slot(p) : 0;
	bool have = sd.nslots > 0 && sd.slots[0].size;
	switch (sd.mode) {
	case SD_LOAD: sd.display = have ? (sd.slots[0].broken ? DS_LOAD_FAILED : DS_LOAD_CONFIRM) : DS_LOAD_NODATA;
		if (have && sd.slots[0].broken) SW(p + SD_RESULT, SD_ERR_LOAD_DATA_BROKEN);
		break;
	case SD_LISTLOAD: sd.display = sd.nslots ? DS_LOAD_LIST : DS_LOAD_NODATA; break;
	case SD_SAVE: sd.display = have ? DS_SAVE_OVERWRITE : DS_SAVE_CONFIRM; if (have) sd.choice = 0; break;
	case SD_LISTSAVE: sd.display = DS_SAVE_LIST; break;
	case SD_LISTDELETE: case SD_LISTALLDELETE: sd.display = sd.nslots ? DS_DELETE_LIST : DS_DELETE_NODATA; break;
	case SD_DELETE: sd.display = have ? DS_DELETE_CONFIRM : DS_DELETE_NODATA; if (have) sd.choice = 0; break;
	default: sd.display = DS_NONE; break;
	}
	start_init(T_SAVE, 200000);
	fade_start(true);
	fade.value = 0;
	held_frames = 0;
	btn_last = fe_buttons;
	return 0;
}

static const char *sd_dir_name(void) {   // the save name the work is for: the picked one, or the request's
	static char name[21];
	if (sd_multi_name(sd.mode) && sd.nslots > 0 && sd.sel < sd.nslots) return sd.slots[sd.sel].name;
	sd_field(sd.p, SD_SAVE_NAME, 20, name);
	if (!strcmp(name, "<>")) name[0] = 0;
	return name;
}

// The work of the modes that show nothing
static void sd_quiet_io(void) {
	u32 p = sd.p, r = 0;
	const char *name = sd_dir_name();
	switch (sd.mode) {
	case SD_LOAD: case SD_AUTOLOAD: r = sd_load(p, name, true); break;
	case SD_SAVE: case SD_AUTOSAVE: r = sd_save(p, name, true); break;
	case SD_SIZES: r = sd_sizes(p); break;
	case SD_LIST: sd_id_list(p); break;
	case SD_DELETEDATA: case SD_AUTODELETE: case SD_DELETE:
		r = sd.nslots && sd_delete_dir(p, sd.slots[0].dir) ? 0 : sd.mode == SD_DELETEDATA ? SD_ERR_RW_NO_DATA : SD_ERR_DELETE_NO_DATA;
		break;
	case SD_MAKEDATA: case SD_MAKEDATASECURE:
		r = sd_save(p, name, sd.mode == SD_MAKEDATASECURE);
		if (r == SD_ERR_SAVE_MS_NOSPACE) r = SD_ERR_RW_MEMSTICK_FULL;
		break;
	case SD_WRITEDATA: case SD_WRITEDATASECURE: r = sd_save(p, name, sd.mode == SD_WRITEDATASECURE); break;
	case SD_READDATA: case SD_READDATASECURE:
		r = sd_load(p, name, sd.mode == SD_READDATASECURE);
		if (r == SD_ERR_LOAD_DATA_BROKEN) r = SD_ERR_RW_DATA_BROKEN;
		if (r == SD_ERR_LOAD_NO_DATA) r = SD_ERR_RW_NO_DATA;
		break;
	case SD_ERASE: case SD_ERASESECURE: r = sd_erase(p); break;
	default: LOGE("savedata: mode %d is not supported", sd.mode); break;
	}
	SW(p + SD_RESULT, r);
	if (r) LOGI("savedata: %s -> %08x", mode_names[sd.mode], r);
}

static void sd_finish(u32 result) { SW(sd.p + SD_RESULT, result); fade_start(false); }

static bool list_mode(void) { return sd.mode != SD_SAVE && sd.mode != SD_LOAD && sd.mode != SD_DELETE; }

// Starts the visible work: it's done now, and its screen ("Saving...") shows for a moment
static void sd_visible_io(int now_showing) {
	u32 p = sd.p;
	sd.display = now_showing;
	sd.io_frames = 40;
	const char *name = sd_dir_name();
	switch (now_showing) {
	case DS_SAVE_SAVING: sd.io_next = sd_save(p, name, true) == 0 ? DS_SAVE_DONE : DS_SAVE_FAILED; break;
	case DS_LOAD_LOADING: {
		u32 r = sd_load(p, name, true);
		SW(p + SD_RESULT, r);
		sd.io_next = r == 0 ? DS_LOAD_DONE : DS_LOAD_FAILED;
		break;
	}
	case DS_DELETE_DELETING:
		if (sd.sel < sd.nslots && sd_delete_dir(p, sd.slots[sd.sel].dir)) { SW(p + SD_RESULT, 0); sd.io_next = DS_DELETE_DONE; }
		else sd.io_next = DS_DELETE_FAILED;
		break;
	}
}

static void sd_update_screens(void) {
	u32 ok = ok_button(sd.p), back = back_button(sd.p);
	bool yes = sd.choice == 1;
	switch (sd.display) {
	case DS_SAVE_LIST: case DS_LOAD_LIST: case DS_DELETE_LIST:
		if (pressed_or_held(B_UP) && sd.sel > 0) sd.sel--;
		else if (pressed_or_held(B_DOWN) && sd.sel < sd.nslots - 1) sd.sel++;
		if (pressed(back)) sd_finish(RESULT_CANCEL);
		else if (pressed(ok) && sd.nslots) {
			if (sd.display == DS_SAVE_LIST) {
				if (sd.slots[sd.sel].size) { sd.choice = 0; sd.display = DS_SAVE_OVERWRITE; }
				else sd_visible_io(DS_SAVE_SAVING);
			} else if (sd.display == DS_LOAD_LIST) sd_visible_io(DS_LOAD_LOADING);
			else { sd.choice = 0; sd.display = DS_DELETE_CONFIRM; }
		}
		return;
	case DS_SAVE_CONFIRM: case DS_SAVE_OVERWRITE: case DS_LOAD_CONFIRM: case DS_DELETE_CONFIRM:
		if (pressed(B_LEFT) && !yes) sd.choice = 1;
		else if (pressed(B_RIGHT) && yes) sd.choice = 0;
		if (pressed(back) || (pressed(ok) && !yes)) {
			if (sd.display == DS_SAVE_OVERWRITE && sd.mode != SD_SAVE) sd.display = DS_SAVE_LIST;
			else if (sd.display == DS_DELETE_CONFIRM && list_mode()) sd.display = DS_DELETE_LIST;
			else sd_finish(RESULT_CANCEL);
		} else if (pressed(ok))
			sd_visible_io(sd.display == DS_LOAD_CONFIRM ? DS_LOAD_LOADING : sd.display == DS_DELETE_CONFIRM ? DS_DELETE_DELETING : DS_SAVE_SAVING);
		return;
	case DS_SAVE_SAVING: case DS_LOAD_LOADING: case DS_DELETE_DELETING:
		if (--sd.io_frames <= 0) {
			sd.display = sd.io_next;
			if (sd.display == DS_SAVE_DONE || sd.display == DS_DELETE_DONE) {   // show what changed
				char name[21];
				snprintf(name, sizeof name, "%s", sd.sel < sd.nslots ? sd.slots[sd.sel].name : "");
				sd_relist();
				for (int i = 0; i < sd.nslots; i++) if (!strcmp(sd.slots[i].name, name)) sd.sel = i;
			}
		}
		return;
	case DS_SAVE_FAILED: case DS_LOAD_FAILED: case DS_DELETE_FAILED:
		if (pressed(back) || pressed(ok)) {   // back to the list to try another, or out
			if (sd.display == DS_SAVE_FAILED && sd.mode != SD_SAVE) sd.display = DS_SAVE_LIST;
			else if (sd.display == DS_LOAD_FAILED && sd.mode != SD_LOAD) sd.display = DS_LOAD_LIST;
			else if (sd.display == DS_DELETE_FAILED && list_mode()) sd.display = DS_DELETE_LIST;
			else if (sd.display == DS_SAVE_FAILED) sd_finish(RESULT_CANCEL);
			else fade_start(false);
		}
		return;
	case DS_SAVE_DONE: case DS_LOAD_DONE:   // (the PSP only takes Back here; either is friendlier)
		if (pressed(back) || pressed(ok)) sd_finish(0);
		return;
	case DS_DELETE_DONE:
		if (pressed(back) || pressed(ok)) {
			if (!sd.nslots) sd.display = DS_DELETE_NODATA;
			else if (list_mode()) { if (sd.sel > sd.nslots - 1) sd.sel = sd.nslots - 1; sd.display = DS_DELETE_LIST; }
			else sd_finish(0);
		}
		return;
	case DS_LOAD_NODATA: case DS_DELETE_NODATA:
		if (pressed(back) || pressed(ok)) sd_finish(sd.display == DS_LOAD_NODATA ? SD_ERR_LOAD_NO_DATA : SD_ERR_DELETE_NO_DATA);
		return;
	}
}

static void h_SavedataInitStart(Ctx *c) { RET(init_start(T_SAVE, sd_init, A(0))); }
static void h_SavedataGetStatus(Ctx *c) { RET(get_status(T_SAVE)); }
static void h_SavedataShutdownStart(Ctx *c) {
	u32 r = shutdown_start(T_SAVE, 2000);
	if (!r && sd.slots) { sd_free_slots(sd.slots, sd.nslots > 1 ? sd.nslots : 1); sd.slots = NULL; sd.nslots = 0; }
	RET(r);
}
static void h_SavedataUpdate(Ctx *c) {
	run_pending();
	if (cur_type != T_SAVE) { RET(ERR_WRONG_TYPE); return; }
	if (status[T_SAVE] != S_RUNNING) { RET(ERR_INVALID_STATUS); return; }
	read_buttons();
	if (sd.display == DS_NONE) {   // work on the first Update, finished on the next
		if (!sd.io_started) { sd_quiet_io(); sd.io_started = true; }
		else status[T_SAVE] = S_FINISHED;
	} else {
		if (fade_update(A(0))) status[T_SAVE] = S_FINISHED;
		else sd_update_screens();
	}
	delay(300);
	RET(0);
}

// ================================================================= messages
enum { MSG_V1 = 572, MSG_V2 = 580, MSG_V3 = 708 };
enum { O_YESNO = 0x10, O_OK = 0x20, O_NOCANCEL = 0x80, O_DEFAULT_NO = 0x100, O_SUPPORTED = 0x1B3 };
static struct {
	u32 p, size;
	bool error, yesno, ok, valid_button, cancel_button, abort;
	u32 result;
	int frames, choice;
	char text[512], ok_label[65], back_label[65];
} msg;

static void error_text(u32 code, char *out, size_t n) {
	switch (code) {
	case 0x80110306: snprintf(out, n, "Save data was corrupt. (%08x)", code); break;
	case 0x80110301: case 0x80110321: case 0x80110381: case 0x80110341: case 0x801103C1:
		snprintf(out, n, "Memory stick not inserted. (%08x)", code); break;
	case 0x80110307: case 0x80110327: case 0x80110347: case 0x801103C7:
		snprintf(out, n, "Warning: no save data was found. (%08x)", code); break;
	case 0x80110323: case 0x80110383: snprintf(out, n, "Memory stick full. Check your storage space. (%08x)", code); break;
	default: snprintf(out, n, "Error code: %08x", code); break;
	}
}

static u32 msg_init(u32 p) {
	static const u32 sizes[] = {MSG_V1, MSG_V2, MSG_V3};
	if (status[T_MSG] != S_NONE) return ERR_INVALID_STATUS;
	u32 r = check_request(p, sizes, 3);
	if (r) return r;
	memset(&msg, 0, sizeof msg);
	msg.p = p;
	msg.size = LW(p);
	u32 type = LW(p + 52), err = LW(p + 56), opt = msg.size >= MSG_V2 ? LW(p + 572) : 0;
	if (type == 0 && !(err & 0x80000000)) { msg.error = true; msg.result = ERR_MSG_ERRORCODEINVALID; }
	else if (msg.size == MSG_V2 && type == 1 && (opt & ~(u32)(0x01 | O_YESNO | O_DEFAULT_NO))) { msg.error = true; msg.result = ERR_MSG_BADOPTION; }
	else if (msg.size == MSG_V3 && (((opt & O_DEFAULT_NO) && !(opt & O_YESNO)) || (opt & ~(u32)O_SUPPORTED))) { msg.error = true; msg.result = ERR_MSG_BADOPTION; }
	msg.choice = 1;
	if (!msg.error) {
		msg.yesno = (opt & O_YESNO) && (msg.size == MSG_V3 || (msg.size == MSG_V2 && type == 1));
		if (opt & O_DEFAULT_NO) msg.choice = 0;
		if ((opt & O_OK) && msg.size == MSG_V3) { msg.choice = 1; msg.ok = true; }
		msg.valid_button = msg.yesno || msg.ok;
		msg.cancel_button = !((opt & O_NOCANCEL) && msg.size == MSG_V3);
	}
	if (type == 0) error_text(err, msg.text, sizeof msg.text);
	else sd_field(p, 60, 512, msg.text);
	if (msg.size == MSG_V3) { sd_field(p, 580, 64, msg.ok_label); sd_field(p, 644, 64, msg.back_label); }
	LOGI("message dialog: \"%s\"%s%s", msg.text, msg.yesno ? " (yes/no)" : "", msg.error ? " (bad request)" : "");
	start_init(T_MSG, 300000);
	fade_start(true);
	fade.value = 0;
	btn_last = fe_buttons;
	return 0;
}

static void h_MsgDialogInitStart(Ctx *c) { RET(init_start(T_MSG, msg_init, A(0))); }
static void h_MsgDialogGetStatus(Ctx *c) { RET(get_status(T_MSG)); }
static void h_MsgDialogShutdownStart(Ctx *c) { RET(shutdown_start(T_MSG, 26000)); }
static void h_MsgDialogUpdate(Ctx *c) {
	run_pending();
	if (cur_type != T_MSG) { RET(ERR_WRONG_TYPE); return; }
	if (status[T_MSG] != S_RUNNING) { RET(ERR_INVALID_STATUS); return; }
	u32 p = msg.p;
	// a PSP takes an Abort from the 8th Update on
	if (msg.error || (msg.abort && ++msg.frames >= 8)) status[T_MSG] = S_FINISHED;
	else {
		if (!msg.abort) msg.frames++;
		read_buttons();
		if (fade_update(A(0))) status[T_MSG] = S_FINISHED;
		else {
			if (msg.yesno) {
				if (pressed(B_LEFT) && msg.choice == 0) msg.choice = 1;
				else if (pressed(B_RIGHT) && msg.choice == 1) msg.choice = 0;
			}
			u32 pressed_code = ~0u;
			if (pressed(back_button(p)) && msg.cancel_button)
				pressed_code = msg.size == MSG_V3 || (msg.size == MSG_V2 && msg.yesno) ? 3 : 0;
			else if (pressed(ok_button(p)) && msg.valid_button)
				pressed_code = msg.choice == 0 ? 2 : 1;
			if (pressed_code != ~0u) {
				if (msg.size >= MSG_V2) SW(p + 576, pressed_code);
				fade_start(false);
			}
		}
		msg.result = 0;
	}
	SW(p + 48, msg.result);
	delay(800);
	RET(0);
}
static void h_MsgDialogAbort(Ctx *c) {
	run_pending();
	if (cur_type != T_MSG) { RET(ERR_WRONG_TYPE); return; }
	if (status[T_MSG] != S_RUNNING) { RET(ERR_INVALID_STATUS); return; }
	msg.abort = true;
	RET(0);
}

// ================================================================= network and game sharing
// The network dialog connects at once to the ad hoc network (net.c); game sharing (sending the game to
// another PSP) reports cancelled.
static u32 holder_p[T_COUNT];
static u32 net_init_(u32 p) {
	if (status[T_NET] != S_NONE) return ERR_INVALID_STATUS;
	if (!media_valid(p, 48)) return ERR_BAD_ARGUMENT;
	holder_p[T_NET] = p;
	start_init(T_NET, 200000);
	return 0;
}
static u32 share_init(u32 p) {
	static const u32 sizes[] = {0x50, 0x54, 0x64};
	if (status[T_SHARE] != S_NONE) return ERR_INVALID_STATUS;
	u32 r = check_request(p, sizes, 3);
	if (r) return r;
	holder_p[T_SHARE] = p;
	start_init(T_SHARE, 200000);
	return 0;
}
static u32 holder_update(int type) {
	run_pending();
	if (cur_type != type) return ERR_WRONG_TYPE;
	if (status[type] != S_RUNNING) return ERR_INVALID_STATUS;
	if (type == T_NET) {
		extern void net_adhoc_connected(void);
		net_adhoc_connected();
		SW(holder_p[type] + 28, 0);
	} else SW(holder_p[type] + 28, RESULT_CANCEL);
	status[type] = S_FINISHED;
	return 0;
}
static void h_NetconfInitStart(Ctx *c) { RET(init_start(T_NET, net_init_, A(0))); }
static void h_NetconfGetStatus(Ctx *c) { RET(get_status(T_NET)); }
static void h_NetconfUpdate(Ctx *c) { RET(holder_update(T_NET)); }
static void h_NetconfShutdownStart(Ctx *c) { RET(shutdown_start(T_NET, 2000)); }
static void h_GameSharingInitStart(Ctx *c) { RET(init_start(T_SHARE, share_init, A(0))); }
static void h_GameSharingGetStatus(Ctx *c) { RET(get_status(T_SHARE)); }
static void h_GameSharingUpdate(Ctx *c) { RET(holder_update(T_SHARE)); }
static void h_GameSharingShutdownStart(Ctx *c) { RET(shutdown_start(T_SHARE, 2000)); }

// ================================================================= system settings, modules
const char *sysdlg_nickname(void) {
	const char *n = getenv("SR2_NICKNAME");   // local split screen names the second copy "PLAYER 2"
	return n && *n ? n : "PLAYER";
}

static void h_GetSystemParamInt(Ctx *c) {
	u32 v;
	switch (A(0)) {
	case 2: v = 0; break;    // ad hoc channel: automatic
	case 3: v = 0; break;    // WLAN power saving off
	case 4: v = 0; break;    // date format YYYY/MM/DD
	case 5: v = 0; break;    // 24 hour clock
	case 6: v = 0; break;    // time zone
	case 7: v = 0; break;    // no daylight saving
	case 8: v = 1; break;    // language: English
	case 9: v = 1; break;    // cross confirms
	case 10: v = 9; break;   // parental lock level
	default: RET(ERR_INVALID_SYSTEM_PARAM); return;
	}
	if (media_valid(A(1), 4)) SW(A(1), v);
	RET(0);
}

static void h_GetSystemParamString(Ctx *c) {
	u32 id = A(0), dest = A(1);
	int size = (int)A(2);
	if (size > 0 && !media_valid(dest, size)) { RET(-1); return; }
	if (id != 1) { RET(ERR_INVALID_SYSTEM_PARAM); return; }   // only the nickname
	const char *n = sysdlg_nickname();
	if (size <= (int)strlen(n)) { RET(ERR_STRING_TOO_LONG); return; }
	strncpy((char *)PTR(dest), n, size);
	RET(0);
}

// The firmware modules a game can load (sceUtilityLoadModule ids): loading just succeeds, after the time
// a PSP takes
static const u16 module_ids[] = {
	0x100, 0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x107, 0x108, 0x200, 0x201, 0x202, 0x203, 0x2ff,
	0x300, 0x301, 0x302, 0x303, 0x304, 0x305, 0x306, 0x307, 0x308, 0x3fe, 0x3ff, 0x400, 0x401, 0x402,
	0x403, 0x500, 0x600, 0x601};
static bool module_known(u32 id) {
	for (size_t i = 0; i < sizeof module_ids / sizeof module_ids[0]; i++) if (module_ids[i] == id) return true;
	return false;
}
static void h_LoadModule(Ctx *c) {
	u32 id = A(0);
	if (!module_known(id)) { RET(ERR_MODULE_BAD_ID); return; }
	delay(id == 0x3FF ? 130 : 25000);
	RET(0);
}
static void h_UnloadModule(Ctx *c) {
	u32 id = A(0);
	if (!module_known(id)) { RET(ERR_MODULE_BAD_ID); return; }
	delay(id == 0x3FF ? 110 : 400);
	RET(0);
}

// ================================================================= what to draw
static SysDlgView view;

bool sysdlg_active(void) {
	extern bool currentDialogActive;   // PPSSPP's dialogs (SR2_PPSSPP_DIALOGS)
	return (cur_active && cur_type != T_NONE && status[cur_type] != S_NONE) || currentDialogActive;
}

const SysDlgView *sysdlg_view(void) {
	memset(&view, 0, sizeof view);
	int s = cur_type ? status[cur_type] : S_NONE;
	if (!cur_active || (s != S_RUNNING && s != S_FINISHED)) return &view;
	view.alpha = fade.value;
	if (cur_type == T_MSG && !msg.error) {
		view.kind = SYSDLG_MSG;
		view.message = msg.text;
		view.buttons = msg.yesno ? 2 : msg.ok ? 1 : 0;
		view.choice = msg.choice;
		view.ok_cross = ok_is_cross(msg.p);
		if (msg.valid_button) view.ok_label = msg.ok_label[0] ? msg.ok_label : "Select";
		if (msg.cancel_button) view.back_label = msg.back_label[0] ? msg.back_label : "Back";
		return &view;
	}
	if (cur_type != T_SAVE || sd.display == DS_NONE) return &view;
	int d = sd.display;
	view.kind = SYSDLG_SAVEDATA;
	view.ok_cross = ok_is_cross(sd.p);
	view.serial = sd.serial;
	view.heading = d <= DS_SAVE_DONE ? "Save" : d <= DS_LOAD_NODATA ? "Load" : "Delete";
	view.slots = sd.slots;
	view.nslots = sd.nslots;
	view.sel = sd.sel;
	view.choice = sd.choice;
	view.list = d == DS_SAVE_LIST || d == DS_LOAD_LIST || d == DS_DELETE_LIST;
	if (!view.list && !sd_multi_name(sd.mode) && sd.slots && !sd.nslots) view.nslots = 1;   // the save about to be made
	view.new_title = (const char *)PTR(sd.p + SD_SAVE_TITLE);
	view.new_size = LW(sd.p + SD_DATA_SIZE);
	view.show_new = d == DS_SAVE_CONFIRM || d == DS_SAVE_SAVING || d == DS_SAVE_FAILED ||
	                (d == DS_SAVE_LIST && sd.sel < sd.nslots && !sd.slots[sd.sel].size);
	static const char *const text[] = {
		[DS_SAVE_CONFIRM] = "Do you want to save this data?", [DS_SAVE_OVERWRITE] = "Do you want to overwrite the data?",
		[DS_SAVE_SAVING] = "Saving...\nPlease wait.", [DS_SAVE_FAILED] = "Unable to save data.", [DS_SAVE_DONE] = "Save completed.",
		[DS_LOAD_CONFIRM] = "Load this data?", [DS_LOAD_LOADING] = "Loading...\nPlease wait.",
		[DS_LOAD_FAILED] = "Load failed.\nThe data is corrupted.", [DS_LOAD_DONE] = "Load completed.", [DS_LOAD_NODATA] = "There is no data.",
		[DS_DELETE_CONFIRM] = "This save data will be deleted.\nAre you sure you want to continue?",
		[DS_DELETE_DELETING] = "Deleting...\nPlease wait.", [DS_DELETE_FAILED] = "Unable to delete data.",
		[DS_DELETE_DONE] = "Delete completed.", [DS_DELETE_NODATA] = "There is no data.",
	};
	view.message = text[d];
	bool question = d == DS_SAVE_CONFIRM || d == DS_SAVE_OVERWRITE || d == DS_LOAD_CONFIRM || d == DS_DELETE_CONFIRM;
	bool working = d == DS_SAVE_SAVING || d == DS_LOAD_LOADING || d == DS_DELETE_DELETING;
	view.buttons = question ? 2 : 0;
	if (d == DS_DELETE_DONE || d == DS_DELETE_NODATA || d == DS_LOAD_NODATA || d == DS_DELETE_DELETING || d == DS_DELETE_FAILED)
		view.nslots = 0;   // nothing to show beside these
	if (!working) {
		view.ok_label = view.list || question ? "Select" : NULL;
		view.back_label = "Back";
	}
	return &view;
}

// ================================================================= registration
static const HleEnt sysdlg_ents[] = {
	{0x50C4CD57, "sceUtilitySavedataInitStart", h_SavedataInitStart},
	{0x8874DBE0, "sceUtilitySavedataGetStatus", h_SavedataGetStatus},
	{0xD4B95FFB, "sceUtilitySavedataUpdate", h_SavedataUpdate},
	{0x9790B33C, "sceUtilitySavedataShutdownStart", h_SavedataShutdownStart},
	{0x2AD8E239, "sceUtilityMsgDialogInitStart", h_MsgDialogInitStart},
	{0x9A1C91D7, "sceUtilityMsgDialogGetStatus", h_MsgDialogGetStatus},
	{0x95FC253B, "sceUtilityMsgDialogUpdate", h_MsgDialogUpdate},
	{0x67AF3428, "sceUtilityMsgDialogShutdownStart", h_MsgDialogShutdownStart},
	{0x4928BD96, "sceUtilityMsgDialogAbort", h_MsgDialogAbort},
	{0x4DB1E739, "sceUtilityNetconfInitStart", h_NetconfInitStart},
	{0x6332AA39, "sceUtilityNetconfGetStatus", h_NetconfGetStatus},
	{0x91E70E35, "sceUtilityNetconfUpdate", h_NetconfUpdate},
	{0xF88155F6, "sceUtilityNetconfShutdownStart", h_NetconfShutdownStart},
	{0xC492F751, "sceUtilityGameSharingInitStart", h_GameSharingInitStart},
	{0x946963F3, "sceUtilityGameSharingGetStatus", h_GameSharingGetStatus},
	{0x7853182D, "sceUtilityGameSharingUpdate", h_GameSharingUpdate},
	{0xEFC6F80F, "sceUtilityGameSharingShutdownStart", h_GameSharingShutdownStart},
	{0xA5DA2406, "sceUtilityGetSystemParamInt", h_GetSystemParamInt},
	{0x34B78343, "sceUtilityGetSystemParamString", h_GetSystemParamString},
	{0x2A2B3DE0, "sceUtilityLoadModule", h_LoadModule},
	{0xE49BFE92, "sceUtilityUnloadModule", h_UnloadModule},
};

void sysdlg_init(void) {
	hle_register(sysdlg_ents, sizeof sysdlg_ents / sizeof sysdlg_ents[0]);
	char path[1024];   // a fresh memory stick has the folder
	if (io_host_path("ms0:/PSP/SAVEDATA", path, sizeof path) == 0) {
		for (char *s = path + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(path, 0777); *s = '/'; }
		mkdir(path, 0777);
	}
	LOGI("utility: native system dialogs (save data, messages)");
}
