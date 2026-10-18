#!/usr/bin/env python3
# Test harness for the OpenGL backend in the real game, without touching runtime/ files: derives
# build/gl_frontend/frontend_gl.c from runtime/frontend.c (SDL GL window + ge_present instead of the
# SDL renderer, screenshots via ge_read_display, backend chosen by RCSM_GPU=soft|gl before
# ge_soft_init via a constructor) and links build/rcsm_gl from the runtime's objects with it.
#
#   python3 runtime/gpu/tools/gl_frontend.py          (after tools/build.sh has built build/obj)
#   RCSM_GPU=gl RCSM_GL_SCALE=2 xvfb-run -a build/rcsm_gl game
#
# This mirrors the integration steps given to the frontend's owner; it is not part of the libraries.
import os, re, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
src = open(os.path.join(ROOT, 'runtime/frontend.c')).read()

def sub(old, new, count=1):
    global src
    if src.count(old) < 1:
        sys.exit('pattern not found: ' + old[:60])
    src = src.replace(old, new, count)

sub('#include <SDL2/SDL.h>\n', '''#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <stdlib.h>

// rcsm GL harness: backend choice before ge_soft_init() (core.c calls it before fe_init()).
static int gl_backend;
static SDL_GLContext glctx;
__attribute__((constructor)) static void fe_pick_backend(void) {
	const char *e = getenv("RCSM_GPU");
	gl_backend = e && !strcmp(e, "gl");
	ge_select_backend(gl_backend);
}
''')
# Screenshots: what was presented.
sub('''static void convert_fb(void) {
	if (!fb_set) { memset(rgba, 0, sizeof rgba); return; }''', '''static void convert_fb(void) {
	if (!fb_set) { memset(rgba, 0, sizeof rgba); return; }
	if (glctx) { if (ge_read_display((uint8_t *)rgba, 480, 272) != 0) memset(rgba, 0, sizeof rgba); return; }''')
# Present every vblank through ge_present (also headless: the GL backend runs its frame there).
sub('''	if (!headless) {
		convert_fb();
		SDL_UpdateTexture(tex, NULL, rgba, 480 * 4);
		SDL_RenderClear(ren);
		SDL_RenderCopy(ren, tex, NULL, NULL);
		SDL_RenderPresent(ren);
	}''', '''	if (glctx) {
		int w, h;
		SDL_GL_GetDrawableSize(win, &w, &h);
		// Letterbox to 480:272.
		int vw = w, vh = w * 272 / 480;
		if (vh > h) { vh = h; vw = h * 480 / 272; }
		glViewport(0, 0, w, h);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		ge_present((w - vw) / 2, (h - vh) / 2, vw, vh);
		SDL_GL_SwapWindow(win);
	}''')
# RCSM_SHOT_HD=1: screenshots also at the internal resolution (frame_N_hd.png).
sub('''void fe_set_display(u32 top, u32 stride, u32 fmt) {''', '''static void fe_write_png_hd(const char *path) {
	int s = getenv("RCSM_GL_SCALE") ? atoi(getenv("RCSM_GL_SCALE")) : 2, w = 480 * s, h = 272 * s;
	u8 *px = malloc((size_t)w * h * 4);
	if (!glctx || ge_read_display(px, w, h) != 0) { free(px); return; }
	FILE *f = fopen(path, "wb");
	if (!f) { free(px); return; }
	static const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
	fwrite(sig, 1, 8, f);
	u8 ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 6, 0, 0, 0};
	png_chunk(f, "IHDR", ihdr, 13);
	size_t rawlen = (size_t)h * (1 + w * 4);
	u8 *raw = malloc(rawlen);
	for (int y = 0; y < h; y++) { raw[y * (1 + w * 4)] = 0; memcpy(raw + y * (1 + w * 4) + 1, px + (size_t)y * w * 4, w * 4); }
	uLongf zl = compressBound(rawlen);
	u8 *z = malloc(zl);
	compress2(z, &zl, raw, rawlen, 6);
	png_chunk(f, "IDAT", z, zl);
	png_chunk(f, "IEND", NULL, 0);
	fclose(f);
	free(raw); free(z); free(px);
}

void fe_set_display(u32 top, u32 stride, u32 fmt) {''')
sub('''		fe_write_png(p);
	}''', '''		fe_write_png(p);
		if (getenv("RCSM_SHOT_HD")) {
			snprintf(p, sizeof p, "%s/frame_%06llu_hd.png", shot_dir, (unsigned long long)g_vblank_count);
			fe_write_png_hd(p);
		}
	}''')
# The window: an OpenGL 3.3 core context (hidden when headless).
sub('''	headless = getenv("RCSM_HEADLESS") && atoi(getenv("RCSM_HEADLESS"));
	if (!headless) {''', '''	headless = getenv("RCSM_HEADLESS") && atoi(getenv("RCSM_HEADLESS"));
	{
		if (SDL_Init(SDL_INIT_VIDEO | (headless ? 0 : SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER)) != 0) fatal("SDL init failed (%s)", SDL_GetError());
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		int scale = getenv("RCSM_SCALE") ? atoi(getenv("RCSM_SCALE")) : 2;
		win = SDL_CreateWindow("rcsm (OpenGL harness)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		                       480 * scale, 272 * scale, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (headless ? SDL_WINDOW_HIDDEN : 0));
		if (!win) fatal("SDL_CreateWindow failed (%s)", SDL_GetError());
		glctx = SDL_GL_CreateContext(win);
		if (!glctx) fatal("SDL_GL_CreateContext failed (%s)", SDL_GetError());
		SDL_GL_SetSwapInterval(0);
		ge_gl_init(SDL_GL_GetProcAddress, getenv("RCSM_GL_SCALE") ? atoi(getenv("RCSM_GL_SCALE")) : 2);
	}
	if (!headless) {''')
sub('''		if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
			LOGE("SDL init failed (%s); running headless", SDL_GetError());
			headless = true;
			return;
		}
		int scale = getenv("RCSM_SCALE") ? atoi(getenv("RCSM_SCALE")) : 2;
		win = SDL_CreateWindow("Ratchet & Clank: Size Matters (native)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		                       480 * scale, 272 * scale, SDL_WINDOW_RESIZABLE);
		ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC * 0);
		if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
		SDL_RenderSetLogicalSize(ren, 480, 272);
		tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, 480, 272);
''', '')

out = os.path.join(ROOT, 'build/gl_frontend')
os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'frontend_gl.c'), 'w').write(src)
objdir = os.path.join(ROOT, 'build/obj')
objs = [os.path.join(objdir, f) for f in sorted(os.listdir(objdir)) if f.endswith('.o') and f != 'frontend.o']
cc = ['gcc', '-O2', '-g', '-fno-strict-aliasing', '-fwrapv', '-I' + os.path.join(ROOT, 'runtime'), '-w', '-c',
      os.path.join(out, 'frontend_gl.c'), '-o', os.path.join(out, 'frontend_gl.o')]
subprocess.check_call(cc)
ff = subprocess.check_output(['pkg-config', '--libs', 'libavformat', 'libavcodec', 'libswscale', 'libswresample', 'libavutil']).decode().split()
b = lambda n: os.path.join(ROOT, 'build', n)
link = ['g++', '-o', b('rcsm_gl')] + objs + [os.path.join(out, 'frontend_gl.o'), b('libpsputility.a'), b('libpspgpu.a'),
        b('libpspmpeg.a'), b('libpspaudio.a')] + ff + ['-lSDL2', '-lGL', '-lpng', '-lpthread', '-lz', '-lm']
subprocess.check_call(link)
print('built', b('rcsm_gl'))
