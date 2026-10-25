// C API of the rcsm GE (libpspgpu.a: PPSSPP's GE list processor with its software renderer and its
// OpenGL hardware renderer). The ge_soft_* names are historical: they drive whichever backend is active.
// Link: build/libpspgpu.a -lpthread (no -lGL: GL functions are loaded through ge_gl_init's get_proc).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Backend, chosen once at startup before ge_soft_init(): 0 = software (default), 1 = OpenGL.
void ge_select_backend(int backend);

// Register the sceGe HLE functions with hle_register() and create the GPU of the selected backend.
// Call once, after g_mem is mapped and after any other code that registers placeholder sceGe NIDs
// (later registrations win). Software: renders into guest VRAM; PSPGPU_THREADS=n sets the number of
// drawing threads (default: number of CPUs). OpenGL: the GPU is created by whichever of
// ge_soft_init()/ge_gl_init() runs last; both must have run before guest code issues GE commands.
void ge_soft_init(void);

// After the frontend made its OpenGL (3.x+ core or compatibility) context current on this thread.
// Call it for both backends: it loads the GL functions (get_proc = SDL_GL_GetProcAddress) that the
// presenter needs. OpenGL backend: also creates PPSSPP's renderer. internal_scale 1..8 (x 480x272).
void ge_gl_init(void *(*get_proc)(const char *), int internal_scale);

// OpenGL backend: change the internal resolution (1..8); takes effect at the next ge_present().
void ge_gl_set_internal_scale(int scale);

// OpenGL backend, best effort: texture_filtering 0 auto, 1 nearest, 2 linear; anisotropy = max
// samples (0/1 off, 2, 4, 8, 16); msaa is ignored (PPSSPP's OpenGL backend has no MSAA).
// Takes effect at the next ge_present().
void ge_gl_set_options(int texture_filtering, int anisotropy, int msaa);
// rcsm enhanced lighting (OpenGL): mode 0 original / 1 enhanced, specular, shadows, quality 0-2, intensity 0-2
void ge_gl_set_lighting(int mode, int specular, int shadows, int quality, float intensity);

// Split views (OpenGL): each view draws over the whole of a framebuffer of its own at the internal resolution
// (display list command 0xFA000001 | v << 8 before its draws, 0xFA000003 back to the game's framebuffer), and
// 0xFA000002 | n << 8 scales the n views into their parts of the game's framebuffer: for view v, the PSP-pixel
// rectangle src[4 v..] of its image goes to dst[4 v..] (x, y, w, h, top-left origin). ge_views_available():
// whether the backend does this (the software one doesn't). ge_views_layout() may be called whenever; it's
// used at the next compose.
int ge_views_available(void);
void ge_views_layout(int n, const int *dst, const int *src);

// Once per displayed frame, on the GL thread: draws the current PSP display into the default
// framebuffer (framebuffer 0) stretched over the rectangle (x, y, w, h) in glViewport convention
// (origin bottom-left), bilinear. OpenGL backend: also ends the host frame and executes its GL work.
// Leaves GL in a default state (framebuffer 0 bound, no program/VAO/textures/samplers bound,
// blend/depth/stencil/scissor/cull off, all write masks on, pixel store alignment 4).
void ge_present(int fb_x, int fb_y, int fb_w, int fb_h);

// The last presented image, scaled to w x h (bilinear unless 1:1), as top-down RGBA8888 with alpha
// 255. Needs the GL context and a previous ge_present(); returns 0, or -1 if there's nothing yet.
// With OpenGL, guest VRAM does not hold rendered frames (PPSSPP only writes framebuffers back to
// memory when the game needs them), so screenshots must come from here.
int ge_read_display(uint8_t *rgba, int w, int h);

// Once per vblank, from the scheduler: the GPU's per-frame upkeep. Software: also completes queued
// drawing so VRAM holds the frame.
void ge_soft_vblank(void);

// Tell the GPU which buffer is displayed (from sceDisplaySetFrameBuf). format: 0=565 1=5551 2=4444 3=8888.
void ge_soft_set_display(uint32_t topaddr, int stride, int format);

// Software: wait until all queued drawing has landed in guest memory (e.g. before reading VRAM).
void ge_soft_flush(void);

// GE interrupts for lists enqueued from now on (PPGe draws its dialogs with them off). Default on.
void ge_soft_enable_interrupts(int enable);

// The game's compiled SDK version (sceKernelSetCompiledSdkVersion*); a few GE behaviours depend on it.
// Default 0x06060010.
void ge_soft_set_sdk_version(uint32_t ver);

// rcsm: offset added to every view matrix's translation (view space units, x right / y up) as the GE
// loads it - moves the camera without the game knowing (over-the-shoulder aiming). 0,0 = off.
void ge_set_view_offset(float x, float y);
// test aid: trace every GE command the software renderer executes into a file (pc, op u32 pairs); NULL stops
void ge_trace(const char *path);

// CPU-side memory operations the GPU should know about, like PPSSPP's sceDmacMemcpy, memcpy/memset
// HLE helpers and sceKernelDcacheWriteback* do. They keep the OpenGL backend's framebuffers and
// texture cache coherent with VRAM/RAM; harmless with the software backend. memcpy/memset return 1
// if the GPU performed the operation itself (then the caller must not), else 0.
int ge_notify_memcpy(uint32_t dst, uint32_t src, uint32_t size);
int ge_notify_memset(uint32_t dst, uint8_t value, uint32_t size);
void ge_notify_cpu_write(uint32_t addr, uint32_t size);
// sceMpeg wrote a decoded movie frame into guest memory (libpspmpeg.a calls this).
void ge_write_formatted_from_memory(uint32_t addr, int size, int width, int format);

#ifdef __cplusplus
}
#endif
