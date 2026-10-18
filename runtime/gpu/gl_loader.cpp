// OpenGL function pointers for PPSSPP's GL backend and the rcsm presenter (see shim/GL/glew.h, which
// tools/gen_gl_loader.py generates). Filled from the frontend's get_proc (SDL_GL_GetProcAddress).
// GPL-2.0+ (see LICENSE.TXT).
#define RCSM_GL_LOADER_IMPL
#include "GL/glew.h"

// Returns the number of functions the context doesn't provide (left null; PPSSPP checks the
// extensions it uses before calling them).
int rcsm_gl_load(void *(*get_proc)(const char *)) {
	int missing = 0;
#define RCSM_LOAD(f) \
	rcsm_##f = (decltype(rcsm_##f))get_proc(#f); \
	if (!rcsm_##f) missing++;
	RCSM_GL_FUNCTIONS(RCSM_LOAD)
#undef RCSM_LOAD
	return missing;
}
