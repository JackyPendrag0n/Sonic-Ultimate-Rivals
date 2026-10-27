// Native GE renderer: decoded vertices and the interface between the GE side and the OpenGL side.
#pragma once
#include "ng_internal.h"

struct NgVert {
	float pos[3];
	float uv[2];
	float nrm[3];
	u32 color;   // RGBA8, R in the low byte
};

struct NgLayout {
	int tc, col, nrm, pos, wgt;      // component formats (0 = absent)
	int wcount, morphs;
	int toff, coff, noff, poff, woff, size;
	bool through;
};

void ng_vertex_layout(u32 vtype, NgLayout *L);
int ng_decode(u32 vaddr, u32 iaddr, int count, const NgLayout &L, NgVert *verts);

// ng_gl.cpp
void ng_gl_prim(int prim, const NgVert *v, int count, const NgLayout &L);
void ng_gl_loadclut(u32 addr, int bytes);
void ng_gl_rcsm(u32 data);                 // split view commands (0xFA)
void ng_gl_list_done(void);                // a display list finished (flush work, write back small buffers)
