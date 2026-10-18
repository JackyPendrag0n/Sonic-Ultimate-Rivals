// Shim for PPSSPP's Common/Data/Text/I18n.h (rcsm GPU extraction): English only. A key's text is
// its default if given, else an English string for the few keys without one, else the key. GPL-2.0+.
#pragma once
// Renamed so they cannot meet libpsputility.a's copies.
#define I18NCategory GpuI18NCategory
#define GetI18NCategory gpu_GetI18NCategory
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
enum class I18NCat : uint8_t {
	AUDIO = 0, CONTROLS, CWCHEATS, DESKTOPUI, DEVELOPER, DIALOG, ERRORS, GAME, GRAPHICS, INSTALLZIP,
	KEYMAPPING, MAINMENU, MAINSETTINGS, MAPPABLECONTROLS, NETWORKING, PSPCREDITS, MEMSTICK, REMOTEISO,
	REPORTING, SAVEDATA, SCREEN, SEARCH, STORE, SYSINFO, SYSTEM, TEXTURESHADERS, THEMES, UI_ELEMENTS,
	UPGRADE, VR, ACHIEVEMENTS, PSPSETTINGS, CATEGORY_COUNT,
};
class I18NCategory {
public:
	std::string_view T(std::string_view key, std::string_view def = "");
	const char *T_cstr(const char *key, const char *def = nullptr);
};
std::shared_ptr<I18NCategory> GetI18NCategory(I18NCat cat);
