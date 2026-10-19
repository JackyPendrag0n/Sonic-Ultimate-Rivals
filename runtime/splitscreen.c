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
#include <GL/gl.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>

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
static pid_t child;
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
bool split_active(void) { return child > 0; }

static void map_shared(int fd) {
	sh = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (sh == MAP_FAILED) fatal("split screen: mmap failed");
}

// ---------------------------------------------------------------- child side
void split_child_init(void) {
	const char *e = getenv("SR2_SPLIT_FD");
	if (!e) return;
	is_child = true;
	map_shared(atoi(e));
	prctl(PR_SET_PDEATHSIG, SIGTERM);   // close with player 1's window
	if (getppid() == 1) exit(0);
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
	if (child <= 0) return;
	sh->quit = 1;
	for (int i = 0; i < 50 && waitpid(child, NULL, WNOHANG) == 0; i++) usleep(10000);
	if (waitpid(child, NULL, WNOHANG) == 0) { kill(child, SIGTERM); waitpid(child, NULL, 0); }
	child = 0;
}

void fe_split_layout_changed(void);

bool split_start(void) {
	if (child > 0 || is_child) return child > 0;
	if (!sh) {
		int fd = memfd_create("sr2-split", 0);
		if (fd < 0 || ftruncate(fd, sizeof(Shared))) { LOGE("split screen: no shared memory"); return false; }
		map_shared(fd);
		sh->magic = SPLIT_MAGIC;
		char v[16]; snprintf(v, sizeof v, "%d", fd);
		setenv("SR2_SPLIT_FD", v, 1);
	}
	sh->ready = 0; sh->quit = 0; sh->buttons = 0; sh->lx = sh->ly = 128;
	last_frame = sh->frame;
	char p2[600];
	snprintf(p2, sizeof p2, "%s/player2", g_save_root);
	struct stat st;
	if (stat(p2, &st)) { copy_dir(g_save_root, p2); LOGI("split screen: player 2's saves start as a copy of yours (%s)", p2); }
	char exe[1024];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (n <= 0) return false;
	exe[n] = 0;
	pid_t pid = fork();
	if (pid < 0) return false;
	if (pid == 0) {
		setenv("SR2_SAVE_DIR", p2, 1);
		setenv("SR2_DATA_ROOT", g_save_root, 1);   // mods still come from player 1's folder
		setenv("SR2_NICKNAME", "PLAYER 2", 1);
		setenv("SR2_HEADLESS", "1", 1);
		unsetenv("SR2_CTL");
		execl(exe, exe, g_game_root, (char *)NULL);
		_exit(127);
	}
	child = pid;
	atexit(kill_child);
	LOGI("split screen: started player 2 (pid %d)", pid);
	fe_split_layout_changed();
	return true;
}

void split_stop(void) {
	kill_child();
	fe_split_layout_changed();
}

// once per frame in the parent: notice a child that ended by itself
void split_poll(void) {
	if (child > 0 && waitpid(child, NULL, WNOHANG) == child) {
		LOGI("split screen: player 2's game ended");
		child = 0;
		fe_split_layout_changed();
	}
}

void split_set_input(u32 buttons, u8 lx, u8 ly) {
	if (!sh || child <= 0) return;
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
	if (!sh || child <= 0 || !sh->ready) return;
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
