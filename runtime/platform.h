// Host OS differences: Linux/macOS (POSIX) and Windows (MinGW-w64). Included by kernel.h, so every runtime
// file sees it. Windows headers stay out of here (they define macros like `small`, `near` and `DELETE` that
// clash with the game code's names); the Windows side lives in platform_win.c.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __cplusplus
extern "C" {
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifdef _WIN32
#include <io.h>
#include <direct.h>
#include <time.h>
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
// POSIX mkdir takes a mode; Windows has no modes
#ifndef mkdir
#define mkdir(p, m) _mkdir(p)
#endif
// guest threads run on fibers instead of ucontext
typedef struct { void *fiber; void (*fn)(void); } host_ctx;
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
char *realpath(const char *path, char *resolved);
char *strtok_r(char *s, const char *delim, char **save);
char *strcasestr(const char *hay, const char *needle);
int fnmatch(const char *pattern, const char *s, int flags);   // * and ? only, flags ignored
#define HOST_EXE_SUFFIX ".exe"
#else
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
// macOS only declares the (deprecated, still working) ucontext calls with _XOPEN_SOURCE set
#define _XOPEN_SOURCE 700
#include <ucontext.h>
#undef _XOPEN_SOURCE
#else
#include <ucontext.h>
#endif
typedef ucontext_t host_ctx;
#define HOST_EXE_SUFFIX ""
#endif

// run fn on its own stack in c (stack/size are used on POSIX; Windows fibers allocate their own)
void host_ctx_make(host_ctx *c, void *stack, size_t size, void (*fn)(void));
// save the current context in from and resume to
void host_ctx_swap(host_ctx *from, host_ctx *to);
// once at startup (Windows: 1 ms timer resolution, Winsock)
void host_init(void);
// 1 GiB of zeroed, read/write guest memory
void *host_mem_reserve(size_t size);
// a read/write file descriptor holding a copy of data (memfd on Linux, a deleted temporary file elsewhere)
int host_memfile(const void *data, size_t size);
// full path of the running executable; 0 on success
int host_exe_path(char *buf, size_t n);
// per-user data folder (saves, mods, texture dumps) and settings folder, without a trailing slash
void host_data_dir(char *buf, size_t n);
void host_config_dir(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
