// Shim for PPSSPP's Common/File/VFS/VFS.h (rcsm utility extraction): the only assets read are PPGe's
// atlas files, which are embedded in the library. GPL-2.0+.
#pragma once
#include <cstddef>
#include <cstdint>
class UtilityVFS {
public:
	// new[]-allocated copy, or nullptr.
	uint8_t *ReadFile(const char *filename, size_t *size);
};
#define g_VFS utility_VFS
extern UtilityVFS g_VFS;
