// Internal glue between sce_ge.cpp (GE HLE, software backend) and ge_gl.cpp (backend selection,
// OpenGL backend, presenter). GPL-2.0+ (see LICENSE.TXT).
#pragma once
#include <cstdint>
class GPUCommon;
enum { GE_BACKEND_SOFTWARE = 0, GE_BACKEND_OPENGL = 1 };
int ge_backend(void);
// Shared setup (logging levels, Memory::base, worker threads); idempotent.
void ge_common_init(void);
// ge_soft_init() has registered the sceGe HLE (so a GPU may be created now).
bool ge_hle_ready(void);
// Creates the OpenGL GPU (needs ge_gl_init() to have made the draw context). Sets `gpu`.
GPUCommon *ge_gl_create_gpu(void);
// The display framebuffer the game set (false if none yet).
bool ge_soft_display(uint32_t *top, uint32_t *stride, int *format);
