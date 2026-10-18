#pragma once
#include <string>
#include <cstdio>
#include "Common/CommonTypes.h"
#include <cstdarg>
inline std::string StringFromFormat(const char *fmt, ...) { char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); return b; }
