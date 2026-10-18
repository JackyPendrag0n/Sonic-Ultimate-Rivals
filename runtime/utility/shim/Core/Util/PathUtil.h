// Shim for PPSSPP's Core/Util/PathUtil.h (rcsm utility extraction): the two path checks the savedata
// code uses (from PPSSPP's Core/Util/PathUtil.cpp). GPL-2.0+.
#pragma once
#include <string_view>
#include "Common/File/Path.h"
inline bool HasPathTraversal(std::string_view path) {
	if (path.find_first_of("/\\") != std::string_view::npos)
		return true;
	return path == "." || path == "..";
}
inline bool IsSafePathComponent(std::string_view name) {
	if (name.empty() || name.back() == ' ' || name.back() == '.')
		return false;
	for (unsigned char c : name) {
		if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':')
			return false;
	}
	return true;
}
