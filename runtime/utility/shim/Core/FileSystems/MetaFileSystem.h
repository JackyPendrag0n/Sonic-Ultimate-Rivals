// Shim for PPSSPP's Core/FileSystems/MetaFileSystem.h (rcsm utility extraction): pspFileSystem on top
// of the runtime's PSP->host path mapping (io_host_path: ms0:/ is the save directory). Implemented in
// utility_fs.cpp with plain host files. GPL-2.0+.
#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "Core/FileSystems/FileSystem.h"

class UtilityFileSystem {
public:
	std::vector<PSPFileInfo> GetDirListing(std::string_view path, bool *exists = nullptr);
	int OpenFile(std::string filename, FileAccess access, const char *devicename = nullptr);
	void CloseFile(u32 handle);
	size_t ReadFile(u32 handle, u8 *pointer, s64 size);
	size_t ReadFile(u32 handle, u8 *pointer, s64 size, int &usec) { usec = 0; return ReadFile(handle, pointer, size); }
	size_t WriteFile(u32 handle, const u8 *pointer, s64 size);
	size_t WriteFile(u32 handle, const u8 *pointer, s64 size, int &usec) { usec = 0; return WriteFile(handle, pointer, size); }
	size_t SeekFile(u32 handle, s32 position, FileMove type);
	size_t GetSeekPos(u32 handle) { return SeekFile(handle, 0, FILEMOVE_CURRENT); }
	PSPFileInfo GetFileInfo(std::string filename);
	bool MkDir(const std::string &dirname);
	bool RmDir(const std::string &dirname);
	bool RemoveFile(const std::string &filename);
	int ReadEntireFile(const std::string &filename, std::vector<u8> &data, bool quiet = false);
};
#define pspFileSystem utility_pspFileSystem
extern UtilityFileSystem pspFileSystem;
