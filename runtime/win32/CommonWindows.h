// Stand-in for PPSSPP's Common/CommonWindows.h (not vendored): the Windows headers without min/max macros.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef DrawText
#undef CreateEvent
#undef CreateDirectory
#undef DeleteFile
#undef GetObject
