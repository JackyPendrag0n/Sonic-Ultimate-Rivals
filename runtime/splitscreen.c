// Local 2-player split screen: a second copy of the game runs as a child process, and the two race each other
// through the game's own Wireless Play (the ad hoc layer in net.c carries their traffic over local UDP), the
// same way two PSPs did. This window shows both: player 1 on the left (or top), player 2 on the right (or
// bottom), and player 2's controller is read here and handed to the child.
//
// The child has a hidden window. Every frame it draws, it copies the image into shared memory (two buffers,
// `ready` names the newer one); the parent uploads it to a texture and blits it into its half. Player 2 keeps
// their own save data (<saves>/player2, first copied from player 1's) so their unlocks and profile are their own.
// The child is muted: both copies play the same music, and two copies of it a few frames apart sound wrong.
#define _GNU_SOURCE
#include "kernel.h"
#include "config.h"
#include "gpu/ge_soft.h"
#include <SDL2/SDL.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
typedef HANDLE proc_t;
#define NO_PROC NULL
#else
#include <GL/gl.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
typedef pid_t proc_t;
#define NO_PROC 0
#endif

#define SPLIT_MAGIC 0x53523250u   // 'SR2P'
#define MAX_W (480 * 3)
#define MAX_H (272 * 3)

typedef struct {
	u32 magic;
	volatile u32 ready;          // 1 + index of the newest complete image, 0 = none yet
	volatile u32 frame;          // bumped for every image
	u32 w, h;
	volatile u32 buttons;        // player 2 input, written by the parent
	volatile u8 lx, ly;
	volatile u32 quit;           // parent asks the child to exit
	u8 px[2][MAX_W * MAX_H * 4];
} Shared;

static Shared *sh;
static proc_t child = NO_PROC;
static bool is_child;
static u32 last_frame;
static GLuint tex, fbo;
static int tex_w, tex_h;

// GL 3.x entry points, fetched through SDL like the rest of the frontend
static void (*p_GenTextures)(GLsizei, GLuint *);
static void (*p_BindTexture)(GLenum, GLuint);
static void (*p_TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (*p_TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
static void (*p_TexParameteri)(GLenum, GLenum, GLint);
static void (*p_GenFramebuffers)(GLsizei, GLuint *);
static void (*p_BindFramebuffer)(GLenum, GLuint);
static void (*p_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static void (*p_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
static void (*p_PixelStorei)(GLenum, GLint);

bool split_is_child(void) { return is_child; }
bool split_active(void) { return child != NO_PROC; }

// ---------------------------------------------------------------- host process and shared memory
// The shared block is passed to the child as an inherited handle (Windows) or file descriptor, named in SR2_SPLIT_FD.
#ifdef _WIN32
static HANDLE job;

static void map_shared(const char *handle) {
	sh = MapViewOfFile((HANDLE)(uintptr_t)strtoull(handle, NULL, 10), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
	if (!sh) fatal("split screen: MapViewOfFile failed (%lu)", GetLastError());
}
static bool create_shared(char *name, size_t n) {
	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
	HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(Shared), NULL);
	if (!h) return false;
	snprintf(name, n, "%llu", (unsigned long long)(uintptr_t)h);
	map_shared(name);
	return true;
}
static void parent_death_signal(void) {}   // the parent's job object closes this process with it
static bool proc_running(proc_t p) { return WaitForSingleObject(p, 0) == WAIT_TIMEOUT; }
static void proc_end(proc_t p) {
	for (int i = 0; i < 50 && proc_running(p); i++) Sleep(10);
	if (proc_running(p)) TerminateProcess(p, 1);
	WaitForSingleObject(p, 2000);
	CloseHandle(p);
}
// start "exe game_root" with these variables changed for the child only (value NULL: removed)
static proc_t proc_spawn(const char *exe, const char *const *vars) {
	char *saved[16] = {0}; int nv = 0;
	for (int i = 0; vars[i]; i += 2, nv++) {
		const char *o = getenv(vars[i]);
		saved[nv] = o ? strdup(o) : NULL;
		if (vars[i + 1]) setenv(vars[i], vars[i + 1], 1); else unsetenv(vars[i]);
	}
	char cmd[2200];
	snprintf(cmd, sizeof cmd, "\"%s\" \"%s\"", exe, g_game_root);
	STARTUPINFOA si = {sizeof si};
	PROCESS_INFORMATION pi;
	BOOL ok = CreateProcessA(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
	for (int i = 0, k = 0; vars[i]; i += 2, k++) {
		if (saved[k]) { setenv(vars[i], saved[k], 1); free(saved[k]); } else unsetenv(vars[i]);
	}
	if (!ok) { LOGE("split screen: CreateProcess failed (%lu)", GetLastError()); return NO_PROC; }
	if (!job) {
		job = CreateJobObjectA(NULL, NULL);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
		li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
	}
	if (job) AssignProcessToJobObject(job, pi.hProcess);
	CloseHandle(pi.hThread);
	return pi.hProcess;
}
#define proc_id(p) ((int)GetProcessId(p))
#else
static void map_shared(const char *fd) {
	sh = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, atoi(fd), 0);
	if (sh == MAP_FAILED) fatal("split screen: mmap failed");
}
static bool create_shared(char *name, size_t n) {
#ifdef __linux__
	int fd = memfd_create("sr2-split", 0);
#else
	char nm[64]; snprintf(nm, sizeof nm, "/sr2-split-%d", (int)getpid());
	int fd = shm_open(nm, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd >= 0) shm_unlink(nm);
#endif
	if (fd < 0 || ftruncate(fd, sizeof(Shared))) return false;
	snprintf(name, n, "%d", fd);
	map_shared(name);
	return true;
}
static void parent_death_signal(void) {
#ifdef __linux__
	prctl(PR_SET_PDEATHSIG, SIGTERM);   // close with player 1's window
#endif
	if (getppid() == 1) exit(0);
}
static bool proc_running(proc_t p) { return waitpid(p, NULL, WNOHANG) == 0; }
static void proc_end(proc_t p) {
	for (int i = 0; i < 50 && waitpid(p, NULL, WNOHANG) == 0; i++) usleep(10000);
	if (waitpid(p, NULL, WNOHANG) == 0) { kill(p, SIGTERM); waitpid(p, NULL, 0); }
}
static proc_t proc_spawn(const char *exe, const char *const *vars) {
	pid_t pid = fork();
	if (pid < 0) return NO_PROC;
	if (pid == 0) {
		for (int i = 0; vars[i]; i += 2) { if (vars[i + 1]) setenv(vars[i], vars[i + 1], 1); else unsetenv(vars[i]); }
		execl(exe, exe, g_game_root, (char *)NULL);
		_exit(127);
	}
	return pid;
}
#define proc_id(p) ((int)(p))
#endif

// ---------------------------------------------------------------- child side
void split_child_init(void) {
	const char *e = getenv("SR2_SPLIT_FD");
	if (!e) return;
	is_child = true;
	map_shared(e);
	parent_death_signal();
	LOGI("split screen: running as player 2");
}

// called for every frame the child presents (GL context current)
void split_child_frame(void) {
	if (!sh) return;
	if (sh->quit) exit(0);
	int s = g_cfg.internal_scale < 1 ? 1 : g_cfg.internal_scale > 3 ? 3 : g_cfg.internal_scale;
	u32 w = 480 * s, h = 272 * s;
	u32 slot = sh->ready == 1 ? 1 : 0;   // write the buffer the parent isn't showing
	if (ge_read_display(sh->px[slot], w, h) != 0) return;
	sh->w = w; sh->h = h;
	__sync_synchronize();
	sh->ready = slot + 1;
	sh->frame++;
}

void split_child_input(u32 *buttons, u8 *lx, u8 *ly) {
	if (!sh) return;
	*buttons = sh->buttons; *lx = sh->lx; *ly = sh->ly;
}

// ---------------------------------------------------------------- parent side
static void copy_dir(const char *from, const char *to) {
	DIR *d = opendir(from);
	if (!d) return;
	mkdir(to, 0755);
	struct dirent *de;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.' || !strcmp(de->d_name, "player2") || !strcmp(de->d_name, "texdump") || !strcmp(de->d_name, "rips")
		    || !strcmp(de->d_name, "mods")) continue;
		char a[1200], b[1200];
		snprintf(a, sizeof a, "%s/%s", from, de->d_name);
		snprintf(b, sizeof b, "%s/%s", to, de->d_name);
		struct stat st;
		if (stat(a, &st)) continue;
		if (S_ISDIR(st.st_mode)) { copy_dir(a, b); continue; }
		FILE *in = fopen(a, "rb"), *out = in ? fopen(b, "wb") : NULL;
		char buf[65536]; size_t n;
		while (out && (n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
		if (in) fclose(in);
		if (out) fclose(out);
	}
	closedir(d);
}

static void kill_child(void) {
	if (child == NO_PROC) return;
	sh->quit = 1;
	proc_end(child);
	child = NO_PROC;
}

void fe_split_layout_changed(void);

bool split_start(void) {
	if (child != NO_PROC || is_child) return child != NO_PROC;
	if (!sh) {
		char v[32];
		if (!create_shared(v, sizeof v)) { LOGE("split screen: no shared memory"); return false; }
		sh->magic = SPLIT_MAGIC;
		setenv("SR2_SPLIT_FD", v, 1);
	}
	sh->ready = 0; sh->quit = 0; sh->buttons = 0; sh->lx = sh->ly = 128;
	last_frame = sh->frame;
	char p2[600];
	snprintf(p2, sizeof p2, "%s/player2", g_save_root);
	struct stat st;
	if (stat(p2, &st)) { copy_dir(g_save_root, p2); LOGI("split screen: player 2's saves start as a copy of yours (%s)", p2); }
	char exe[1024];
	if (host_exe_path(exe, sizeof exe)) return false;
	const char *const vars[] = {
		"SR2_SAVE_DIR", p2,
		"SR2_DATA_ROOT", g_save_root,   // mods still come from player 1's folder
		"SR2_NICKNAME", "PLAYER 2",
		"SR2_HEADLESS", "1",
		"SR2_CTL", NULL,
		NULL};
	proc_t pid = proc_spawn(exe, vars);
	if (pid == NO_PROC) return false;
	child = pid;
	static bool registered;
	if (!registered) { registered = true; atexit(kill_child); }
	LOGI("split screen: started player 2 (pid %d)", proc_id(pid));
	fe_split_layout_changed();
	return true;
}

void split_stop(void) {
	kill_child();
	fe_split_layout_changed();
}

// once per frame in the parent: notice a child that ended by itself
void split_poll(void) {
	if (child != NO_PROC && !proc_running(child)) {
		LOGI("split screen: player 2's game ended");
#ifdef _WIN32
		CloseHandle(child);
#endif
		child = NO_PROC;
		fe_split_layout_changed();
	}
}

void split_set_input(u32 buttons, u8 lx, u8 ly) {
	if (!sh || child == NO_PROC) return;
	sh->buttons = buttons; sh->lx = lx; sh->ly = ly;
}

static void gl_load(void) {
	if (p_BlitFramebuffer) return;
	p_GenTextures = SDL_GL_GetProcAddress("glGenTextures");
	p_BindTexture = SDL_GL_GetProcAddress("glBindTexture");
	p_TexImage2D = SDL_GL_GetProcAddress("glTexImage2D");
	p_TexSubImage2D = SDL_GL_GetProcAddress("glTexSubImage2D");
	p_TexParameteri = SDL_GL_GetProcAddress("glTexParameteri");
	p_GenFramebuffers = SDL_GL_GetProcAddress("glGenFramebuffers");
	p_BindFramebuffer = SDL_GL_GetProcAddress("glBindFramebuffer");
	p_FramebufferTexture2D = SDL_GL_GetProcAddress("glFramebufferTexture2D");
	p_BlitFramebuffer = SDL_GL_GetProcAddress("glBlitFramebuffer");
	p_PixelStorei = SDL_GL_GetProcAddress("glPixelStorei");
}

// draw player 2's latest image into (x, y, w, h) of the default framebuffer (glViewport convention)
void split_draw(int x, int y, int w, int h) {
	if (!sh || child == NO_PROC || !sh->ready) return;
	gl_load();
	if (!tex) {
		p_GenTextures(1, &tex);
		p_GenFramebuffers(1, &fbo);
	}
	p_BindTexture(GL_TEXTURE_2D, tex);
	if (sh->w != (u32)tex_w || sh->h != (u32)tex_h) {
		tex_w = sh->w; tex_h = sh->h;
		p_TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex_w, tex_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		p_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		p_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		last_frame = sh->frame - 1;
	}
	if (sh->frame != last_frame) {
		last_frame = sh->frame;
		p_PixelStorei(GL_UNPACK_ALIGNMENT, 4);
		p_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_w, tex_h, GL_RGBA, GL_UNSIGNED_BYTE, sh->px[sh->ready - 1]);
	}
	p_BindTexture(GL_TEXTURE_2D, 0);
	p_BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	p_FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	p_BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	// the image is top-down: flip it while blitting
	p_BlitFramebuffer(0, tex_h, tex_w, 0, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	p_BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}
