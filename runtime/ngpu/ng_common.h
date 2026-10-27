// Native GE renderer: what the OpenGL (ng_gl.cpp) and Vulkan (ng_vk.cpp) sides share. The GE state a draw
// turns into (its shader key and uniforms), primitive assembly, textures as RGBA8 (decoded, or replaced from a
// texture pack), and the conversions between framebuffer memory and RGBA8.
#pragma once
#include "ng_internal.h"
#include "ng_draw.h"
#include <vector>

// Everything a draw's shaders read, std140.
struct Ubo {
	float world[16], view[16], proj[16], tgen[16];
	float vpscale[4], vpcenter[4];   // xyz; the centre has the drawing offset taken off
	float fbsize[4];                 // target w, h (PSP pixels); 1 / texture w, h (through mode)
	float uvso[4];                   // texture coordinate scale (xy) and offset (zw)
	float fbtex[4];                  // normalised texture coordinates -> sampled texture: scale, offset
	float matamb[4], matdiff[4], matspec[4], matemis[4], ambient[4];   // matspec.a: specular power
	float lpos[4][4], ldir[4][4], latt[4][4], lamb[4][4], ldiff[4][4], lspec[4][4], lspot[4][4];
	float fog[4], fogcolor[4], texenv[4];
	int atest[4];                    // ref, mask
	int depal[4];                    // shift, mask, base
};

// Shader key bits
enum {
	K_THROUGH = 1 << 0, K_CLEAR = 1 << 1, K_COLOR = 1 << 2, K_NORMAL = 1 << 3, K_LIGHTING = 1 << 4,
	K_LMODE = 1 << 5, K_FOG = 1 << 6, K_TEX = 1 << 7, K_TEXALPHA = 1 << 8, K_DOUBLE = 1 << 9, K_DEPAL = 1 << 10,
	K_FLAT = 1 << 11, K_REVNORMAL = 1 << 12, K_TEXPROJ = 1 << 13,
	K_MATUPD_SHIFT = 16,   // 3 bits
	K_TEXFUNC_SHIFT = 19,  // 3 bits
	K_ATEST_SHIFT = 22,    // 3 bits (alpha test function; 1 = always = off)
	K_TEXMAP_SHIFT = 25,   // 2 bits
	K_PROJSRC_SHIFT = 27,  // 2 bits
	K_LIGHT_SHIFT = 32,    // 4 x (enable, type 2 bits, computation 2 bits)
};

// The key of a draw's state, without the texture's part (ng_tex_key_bits)
u64 ng_draw_key(const NgLayout &L, bool through, bool clear);
// The texture's part, once a texture is bound
u64 ng_tex_key_bits(bool through);
// The uniforms (fbw x fbh: the target's size in PSP pixels); viewOff moves the camera (split screen)
void ng_fill_ubo(Ubo &u, int fbw, int fbh, bool through, const float viewOff[2]);

// Triangles, lines or points from a GE primitive, as vertices and 16-bit indices. lastFirst: each primitive's
// last vertex comes first (where flat shading takes its colour from the first vertex), with the winding kept.
enum NgMode { NG_TRIANGLES, NG_LINES, NG_POINTS };
int ng_assemble(int prim, const NgVert *v, int count, bool through, std::vector<NgVert> &verts, std::vector<u16> &idx,
                NgMode *mode, bool lastFirst);
// Whether a rectangle draw maps texels 1:1 onto PSP pixels (those sample nearest: see set_sampler)
bool ng_pixel_mapped(int prim, const NgVert *v, int count, bool through);

// ---- CLUT and textures
void ng_clut_load(u32 addr, int bytes);   // LOADCLUT
u64 ng_clut_hash(void);
void ng_palette(u32 pal[256]);            // the loaded CLUT as 256 RGBA8 entries (depal)

// The current texture (level 0) in memory: its cache key, and how many bytes it reads. False if it can't be read.
struct NgTexRef { u32 addr; int fmt, w, h, stride; u32 bytes; u64 key; };
bool ng_tex_ref(NgTexRef *t);
// Its texels as RGBA8, decoded or from the texture pack (*w x *h, which a replacement can change); the pointer
// stays valid until the next call
const u32 *ng_tex_pixels(const NgTexRef &t, int *w, int *h);
void ng_tex_forget_replacements(void);

// ---- framebuffer memory
static inline int ng_fmt_bpp(int fmt) { return fmt == FMT_8888 ? 4 : 2; }
// w x h pixels of format fmt (row pitch stride) in guest memory, as RGBA8
void ng_fb_from_memory(u32 addr, int stride, int fmt, int w, int h, u32 *out);
// and back (8888 keeps the alpha in memory: it's the stencil, which the renderers don't keep)
void ng_fb_to_memory(u32 addr, int stride, int fmt, int w, int h, const u32 *px);
