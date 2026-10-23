// Forced include (-include) for Windows builds: the POSIX calls the runtime and its PPSSPP parts use that
// MinGW-w64 doesn't have. No <windows.h> here, see platform.h.
#pragma once
#ifdef _WIN32
#ifndef _POSIX_THREAD_SAFE_FUNCTIONS
#define _POSIX_THREAD_SAFE_FUNCTIONS 200112L   // localtime_r, gmtime_r
#endif
#include <time.h>
#include <io.h>
#include <direct.h>
#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>

#ifndef mkdir
#define mkdir(p, m) _mkdir(p)
#endif

#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
#endif
static inline long sr2_sysconf(int name) {
	const char *e = name == _SC_NPROCESSORS_ONLN ? getenv("NUMBER_OF_PROCESSORS") : NULL;
	return e && atol(e) > 0 ? atol(e) : 4;
}
#define sysconf sr2_sysconf

// memory from this must be freed with _aligned_free
static inline int sr2_posix_memalign(void **p, size_t align, size_t size) {
	*p = _aligned_malloc(size, align);
	return *p ? 0 : ENOMEM;
}
#define posix_memalign sr2_posix_memalign

static inline int sr2_vasprintf(char **out, const char *fmt, va_list ap) {
	va_list ap2;
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap2);
	va_end(ap2);
	*out = NULL;
	if (n < 0 || !(*out = (char *)malloc((size_t)n + 1))) return -1;
	return vsnprintf(*out, (size_t)n + 1, fmt, ap);
}
#define vasprintf sr2_vasprintf
#endif
