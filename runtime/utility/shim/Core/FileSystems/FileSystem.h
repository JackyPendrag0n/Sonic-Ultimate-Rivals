// Shim for PPSSPP's Core/FileSystems/FileSystem.h (rcsm utility extraction): the types the savedata
// code uses with pspFileSystem. GPL-2.0+.
#pragma once
#include <ctime>
#include <string>
#include <vector>
#include "Common/CommonTypes.h"

enum FileAccess {
	FILEACCESS_NONE = 0, FILEACCESS_READ = 1, FILEACCESS_WRITE = 2, FILEACCESS_APPEND = 4,
	FILEACCESS_CREATE = 8, FILEACCESS_TRUNCATE = 16, FILEACCESS_EXCL = 32, FILEACCESS_PSP_FLAGS = 63,
	FILEACCESS_PPSSPP_QUIET = 128,
};
enum FileMove { FILEMOVE_BEGIN = 0, FILEMOVE_CURRENT = 1, FILEMOVE_END = 2 };
enum FileType { FILETYPE_NORMAL = 1, FILETYPE_DIRECTORY = 2 };

struct PSPFileInfo {
	std::string name;
	s64 size = 0;
	u32 access = 0;
	bool exists = false;
	FileType type = FILETYPE_NORMAL;
	tm atime{}; int atimeUs = 0;
	tm ctime{}; int ctimeUs = 0;
	tm mtime{}; int mtimeUs = 0;
	bool isOnSectorSystem = false;
	u32 startSector = 0;
	u32 numSectors = 0;
	u32 sectorSize = 0;
};
