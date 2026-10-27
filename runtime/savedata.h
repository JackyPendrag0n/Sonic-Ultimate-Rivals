// The PSP's save data storage (the sceUtilitySavedata work without its screens): PSP-style save directories
// under ms0:/PSP/SAVEDATA, their PARAM.SFO, and the sceChnnlsv encryption real PSPs and PPSSPP use, so that
// saves move freely between them and this runtime. sysdlg.c drives it and shows the screens.
#pragma once
#include "kernel.h"
#include "sysdlg.h"

// SceUtilitySavedataParam (1480, 1500 or 1536 bytes): field offsets
enum {
	SD_SIZE = 0, SD_BUTTON_SWAP = 8, SD_RESULT = 28,
	SD_MODE = 48, SD_BIND = 52, SD_OVERWRITE = 56,
	SD_GAME_NAME = 60,      // char[13]
	SD_SAVE_NAME = 76,      // char[20]
	SD_SAVE_NAME_LIST = 96, // -> char[20] entries, ending with an empty one
	SD_FILE_NAME = 100,     // char[13]
	SD_DATA_BUF = 116, SD_DATA_BUF_SIZE = 120, SD_DATA_SIZE = 124,
	SD_TITLE = 128, SD_SAVE_TITLE = 256, SD_DETAIL = 384, SD_PARENTAL = 1408,   // the SFO parameters
	SD_ICON0 = 1412, SD_ICON1 = 1428, SD_PIC1 = 1444, SD_SND0 = 1460,          // {buf, bufSize, size, unknown}
	SD_NEW_DATA = 1476, SD_FOCUS = 1480, SD_ABORT = 1484,
	SD_MS_FREE = 1488, SD_MS_DATA = 1492, SD_UTILITY_DATA = 1496,
	SD_KEY = 1500,          // u8[16]
	SD_SECURE_VERSION = 1516, SD_MULTI_STATUS = 1520, SD_ID_LIST = 1524, SD_FILE_LIST = 1528, SD_SIZE_INFO = 1532,
};

enum {
	SD_AUTOLOAD, SD_AUTOSAVE, SD_LOAD, SD_SAVE, SD_LISTLOAD, SD_LISTSAVE, SD_LISTDELETE, SD_LISTALLDELETE,
	SD_SIZES, SD_AUTODELETE, SD_DELETE, SD_LIST, SD_FILES, SD_MAKEDATASECURE, SD_MAKEDATA, SD_READDATASECURE,
	SD_READDATA, SD_WRITEDATASECURE, SD_WRITEDATA, SD_ERASESECURE, SD_ERASE, SD_DELETEDATA, SD_GETSIZE,
};

#define SD_ERR_LOAD_NO_MS          0x80110301
#define SD_ERR_LOAD_DATA_BROKEN    0x80110306
#define SD_ERR_LOAD_NO_DATA        0x80110307
#define SD_ERR_LOAD_PARAM          0x80110308
#define SD_ERR_LOAD_FILE_NOT_FOUND 0x80110309
#define SD_ERR_RW_MEMSTICK_FULL    0x80110323
#define SD_ERR_RW_DATA_BROKEN      0x80110326
#define SD_ERR_RW_NO_DATA          0x80110327
#define SD_ERR_RW_BAD_PARAMS       0x80110328
#define SD_ERR_RW_FILE_NOT_FOUND   0x80110329
#define SD_ERR_SAVE_MS_NOSPACE     0x80110383
#define SD_ERR_SAVE_PARAM          0x80110388
#define SD_ERR_DELETE_NO_DATA      0x80110347
#define SD_ERR_SIZES_NO_DATA       0x801103C7


// The saves a dialog for request p lists (PPSSPP's SavedataParam::SetPspParam): the game's saveNameList,
// one per name ("<>": any of its saves), or every save on the stick for LISTALLDELETE. Empty slots are
// listed for saving but not for loading or deleting. Returns the count; free with sd_free_slots.
int sd_list_slots(u32 p, SdSlot **out);
void sd_free_slots(SdSlot *s, int n);
bool sd_multi_name(int mode);   // whether the mode picks from the saveNameList

// The work itself. save_name: the save directory's name after the game name. Results are the PSP's codes.
int sd_load(u32 p, const char *save_name, bool secure);
int sd_save(u32 p, const char *save_name, bool secure);
bool sd_delete_dir(u32 p, const char *dir);   // a whole save directory
int sd_erase(u32 p);                          // the request's data file only
int sd_sizes(u32 p);
void sd_id_list(u32 p);
// The file name fields, for logs and checks
void sd_field(u32 p, int off, int len, char *out);   // a fixed-size string field, NUL-terminated
bool sd_names_ok(u32 p);   // no path separators or dot directories in the names

// The encryption (sceChnnlsv as the savedata utility uses it), for the tests: in place, data has room for
// 16 more bytes. Encrypt returns the file hash the PARAM.SFO's file list keeps.
int sd_encrypt(int mode, u8 *data, int *len, int *aligned_len, u8 hash[16], const u8 *key);
int sd_decrypt(int mode, u8 *data, int *len, int *aligned_len, const u8 *key, const u8 *expected_hash);
int sd_update_hash(u8 *sfo, int sfo_size, int params_offset, int mode);
