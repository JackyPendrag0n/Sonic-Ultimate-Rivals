// Native GE renderer: OpenGL function loading (see ng_gl_funcs.h).
#define NG_GL_LOADER_IMPL
#include "ng_gl_funcs.h"

#define NG_GL_DEF(f) decltype(&::f) ng_##f;
NG_GL_FUNCS(NG_GL_DEF)
#undef NG_GL_DEF

int ng_gl_load(void *(*get_proc)(const char *)) {
	int missing = 0;
#define NG_GL_GET(f) ng_##f = (decltype(ng_##f))get_proc(#f); if (!ng_##f) missing++;
	NG_GL_FUNCS(NG_GL_GET)
#undef NG_GL_GET
	return missing;
}
