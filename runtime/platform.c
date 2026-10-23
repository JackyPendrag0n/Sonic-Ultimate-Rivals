// Host OS layer, see platform.h.
#define _GNU_SOURCE
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "platform.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <mmsystem.h>

static void *sched_fiber;

void host_init(void) {
	timeBeginPeriod(1);   // Sleep()/nanosleep() otherwise wake in 15.6 ms steps, which breaks frame pacing
	WSADATA wd;
	WSAStartup(MAKEWORD(2, 2), &wd);
	SetConsoleOutputCP(CP_UTF8);
}

static void WINAPI fiber_entry(void *arg) { ((host_ctx *)arg)->fn(); }

void host_ctx_make(host_ctx *c, void *stack, size_t size, void (*fn)(void)) {
	(void)stack;
	// a thread can be started again after it ended; its old fiber is finished, unless it is the one running
	if (c->fiber && c->fiber != GetCurrentFiber()) DeleteFiber(c->fiber);
	c->fn = fn;
	c->fiber = CreateFiber(size, fiber_entry, c);
	if (!c->fiber) { fprintf(stderr, "CreateFiber failed (%lu)\n", GetLastError()); exit(1); }
}

void host_ctx_swap(host_ctx *from, host_ctx *to) {
	if (!sched_fiber) {
		sched_fiber = ConvertThreadToFiber(NULL);
		if (!sched_fiber) sched_fiber = GetCurrentFiber();
	}
	from->fiber = GetCurrentFiber();
	SwitchToFiber(to->fiber);
}

void *host_mem_reserve(size_t size) {
	return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

int host_exe_path(char *buf, size_t n) {
	DWORD r = GetModuleFileNameA(NULL, buf, (DWORD)n);
	if (r == 0 || r >= n) return -1;
	for (char *p = buf; *p; p++) if (*p == '\\') *p = '/';
	return 0;
}

static void env_dir(char *buf, size_t n, const char *var) {
	const char *a = getenv(var);
	if (!a) a = getenv("USERPROFILE");
	snprintf(buf, n, "%s/sr2recomp", a ? a : ".");
	for (char *p = buf; *p; p++) if (*p == '\\') *p = '/';
}
void host_data_dir(char *buf, size_t n) { env_dir(buf, n, "APPDATA"); }
void host_config_dir(char *buf, size_t n) { env_dir(buf, n, "APPDATA"); }

void host_pause_before_exit(void) {
	DWORD procs[2];
	if (GetConsoleProcessList(procs, 2) == 1) {
		fputs("\nPress Enter to close this window.\n", stderr);
		getchar();
	}
}

int setenv(const char *name, const char *value, int overwrite) {
	if (!overwrite && getenv(name)) return 0;
	return _putenv_s(name, value) ? -1 : 0;
}
int unsetenv(const char *name) { return _putenv_s(name, "") ? -1 : 0; }

char *realpath(const char *path, char *resolved) {
	char *r = _fullpath(resolved, path, PATH_MAX);
	if (r) for (char *p = r; *p; p++) if (*p == '\\') *p = '/';
	return r;
}

char *strcasestr(const char *hay, const char *needle) {
	size_t n = strlen(needle);
	for (; *hay; hay++) if (!_strnicmp(hay, needle, n)) return (char *)hay;
	return n ? NULL : (char *)hay;
}

int fnmatch(const char *p, const char *s, int flags) {
	(void)flags;
	for (; *p; p++, s++) {
		if (*p == '*') {
			while (p[1] == '*') p++;
			if (!p[1]) return 0;
			for (; *s; s++) if (!fnmatch(p + 1, s, 0)) return 0;
			return 1;
		}
		if (!*s || (*p != '?' && *p != *s)) return 1;
	}
	return *s ? 1 : 0;
}

int host_memfile(const void *data, size_t size) {
	char dir[MAX_PATH], path[MAX_PATH];
	if (!GetTempPathA(sizeof dir, dir) || !GetTempFileNameA(dir, "sr2", 0, path)) return -1;
	int fd = _open(path, _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY | _O_TEMPORARY, 0600);
	if (fd < 0) return -1;
	if (_write(fd, data, (unsigned)size) != (int)size) { _close(fd); return -1; }
	_lseek(fd, 0, SEEK_SET);
	return fd;
}

char *strtok_r(char *s, const char *delim, char **save) {
	if (!s) s = *save;
	s += strspn(s, delim);
	if (!*s) { *save = s; return NULL; }
	char *e = s + strcspn(s, delim);
	if (*e) *e++ = 0;
	*save = e;
	return s;
}

#else
#include <sys/mman.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

void host_init(void) {}
void host_pause_before_exit(void) {}

int host_memfile(const void *data, size_t size) {
#ifdef __linux__
	int fd = memfd_create("sr2mem", 0);
#else
	char path[] = "/tmp/sr2memXXXXXX";
	int fd = mkstemp(path);
	if (fd >= 0) unlink(path);
#endif
	if (fd < 0) return -1;
	if (write(fd, data, size) != (ssize_t)size) { close(fd); return -1; }
	lseek(fd, 0, SEEK_SET);
	return fd;
}

void host_ctx_make(host_ctx *c, void *stack, size_t size, void (*fn)(void)) {
	getcontext(c);
	c->uc_stack.ss_sp = stack;
	c->uc_stack.ss_size = size;
	c->uc_link = NULL;
	makecontext(c, fn, 0);
}

void host_ctx_swap(host_ctx *from, host_ctx *to) { swapcontext(from, to); }

void *host_mem_reserve(size_t size) {
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	return p == MAP_FAILED ? NULL : p;
}

int host_exe_path(char *buf, size_t n) {
#ifdef __APPLE__
	uint32_t sz = (uint32_t)n;
	if (_NSGetExecutablePath(buf, &sz)) return -1;
	char rp[PATH_MAX];
	if (realpath(buf, rp)) snprintf(buf, n, "%s", rp);
	return 0;
#else
	ssize_t r = readlink("/proc/self/exe", buf, n - 1);
	if (r <= 0) return -1;
	buf[r] = 0;
	return 0;
#endif
}

// Linux: XDG folders. macOS: the same names under ~/Library/Application Support would be more native,
// but one layout keeps the docs simple, so macOS uses the XDG ones too.
void host_data_dir(char *buf, size_t n) {
	if (getenv("XDG_DATA_HOME")) snprintf(buf, n, "%s/sr2recomp", getenv("XDG_DATA_HOME"));
	else snprintf(buf, n, "%s/.local/share/sr2recomp", getenv("HOME") ? getenv("HOME") : ".");
}
void host_config_dir(char *buf, size_t n) {
	if (getenv("XDG_CONFIG_HOME")) snprintf(buf, n, "%s/sr2recomp", getenv("XDG_CONFIG_HOME"));
	else snprintf(buf, n, "%s/.config/sr2recomp", getenv("HOME") ? getenv("HOME") : ".");
}
#endif
