// Native GE renderer: the OpenGL side. Framebuffers become FBOs at the internal resolution (rows stored top
// first, so PSP row y is GL row y and the image is presented flipped), textures are decoded on the CPU and
// cached by content, and each draw runs a shader generated for its GE state, with every uniform in one
// uniform block.
#include "ngpu.h"
#include "ng_internal.h"
#include "ng_draw.h"
#include "ng_tex.h"
#include "ng_gl_funcs.h"
#include <vector>
#include <unordered_map>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <algorithm>
#include <png.h>

static bool s_ready;
static int s_scale = 2, s_filter, s_aniso;
static u32 s_frame = 1;
static u32 s_texEpoch = 1;   // textures are checked for changes once per display list (the CPU writes between lists)
static float s_viewOff[2];

// ================================================================= GL state cache
// Only what the renderer changes; forgotten (re-set on next use) after every hand-back to other GL code.
static struct {
	int blend = -1, depth = -1, cull = -1;
	GLenum bsrc, bdst, bsrcA, bdstA, beq, beqA;
	u32 bcolor = 0xFFFFFFFF;
	GLenum dfunc; int dmask = -1;
	int cmask = -1;
	int cullFace = -1;
	int sc[4] = {-1, -1, -1, -1};
	GLuint prog = ~0u, fbo = ~0u;
	bool bound;   // the renderer's uniform block binding
	int vp[4] = {-1, -1, -1, -1};
} st;

static void st_forget() {
	st = {};
	st.blend = st.depth = st.cull = st.dmask = st.cmask = st.cullFace = -1;
	st.prog = st.fbo = ~0u;
	st.bcolor = 0xFFFFFFFF; st.bsrc = 0; st.beq = 0;
	st.bound = false;
	for (int i = 0; i < 4; i++) st.sc[i] = st.vp[i] = -1;
}

static void st_cap(GLenum cap, int on, int &cur) {
	if (cur == on) return;
	cur = on;
	if (on) glEnable(cap); else glDisable(cap);
}

static void st_viewport(int x, int y, int w, int h) {
	if (st.vp[0] == x && st.vp[1] == y && st.vp[2] == w && st.vp[3] == h) return;
	st.vp[0] = x; st.vp[1] = y; st.vp[2] = w; st.vp[3] = h;
	glViewport(x, y, w, h);
}

static void st_scissor(int x, int y, int w, int h) {
	if (st.sc[0] == x && st.sc[1] == y && st.sc[2] == w && st.sc[3] == h) return;
	st.sc[0] = x; st.sc[1] = y; st.sc[2] = w; st.sc[3] = h;
	glScissor(x, y, w, h);
}

static void st_program(GLuint p) {
	if (st.prog != p) { st.prog = p; glUseProgram(p); }
}

// Leaves GL as ge_present() promises: default framebuffer, nothing bound, tests off, masks on.
static void gl_reset_state() {
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glUseProgram(0);
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	for (int i = 1; i >= 0; i--) { glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_2D, 0); }
	glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glColorMask(1, 1, 1, 1); glDepthMask(1);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4); glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	st_forget();
}

// ================================================================= shaders
static GLuint compile(GLenum type, const std::string &src) {
	GLuint s = glCreateShader(type);
	const char *p = src.c_str();
	glShaderSource(s, 1, &p, nullptr);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[4096];
		glGetShaderInfoLog(s, sizeof log, nullptr, log);
		NG_LOGE("ngpu: shader compile failed: %s\n%s", log, src.c_str());
	}
	return s;
}

static GLuint link(const std::string &vs, const std::string &fs) {
	GLuint p = glCreateProgram();
	GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
	glAttachShader(p, v);
	glAttachShader(p, f);
	glBindAttribLocation(p, 0, "a_pos");
	glBindAttribLocation(p, 1, "a_uv");
	glBindAttribLocation(p, 2, "a_nrm");
	glBindAttribLocation(p, 3, "a_col");
	glBindFragDataLocation(p, 0, "o_col");
	glLinkProgram(p);
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[4096];
		glGetProgramInfoLog(p, sizeof log, nullptr, log);
		NG_LOGE("ngpu: program link failed: %s", log);
	}
	glDeleteShader(v);
	glDeleteShader(f);
	return p;
}

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

static const char *kUboGlsl = R"(
layout(std140) uniform U {
	mat4 u_world, u_view, u_proj, u_tgen;
	vec4 u_vpscale, u_vpcenter, u_fbsize, u_uvso, u_fbtex;
	vec4 u_matamb, u_matdiff, u_matspec, u_matemis, u_ambient;
	vec4 u_lpos[4], u_ldir[4], u_latt[4], u_lamb[4], u_ldiff[4], u_lspec[4], u_lspot[4];
	vec4 u_fog, u_fogcolor, u_texenv;
	ivec4 u_atest, u_depal;
};
)";

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

static std::string gen_vs(u64 k) {
	std::string s = "#version 330\n";
	s += kUboGlsl;
	s += "in vec3 a_pos; in vec2 a_uv; in vec3 a_nrm; in vec4 a_col;\n";
	s += (k & K_FLAT) ? "flat out vec4 v_col0; flat out vec3 v_col1;\n" : "out vec4 v_col0; out vec3 v_col1;\n";
	s += "out vec3 v_tc; out float v_fog;\n";
	s += "float pspPow(float x, float e) { return e <= 0.0 ? 1.0 : pow(max(x, 0.0), e); }\n";
	s += "vec3 nz(vec3 v, vec3 d) { float l = dot(v, v); return l == 0.0 ? d : v * inversesqrt(l); }\n";
	s += "void main() {\n";
	s += (k & K_COLOR) ? "  vec4 vc = a_col;\n" : "  vec4 vc = u_matamb;\n";
	if (k & K_THROUGH) {
		s += "  gl_Position = vec4(a_pos.x * 2.0 / u_fbsize.x - 1.0, a_pos.y * 2.0 / u_fbsize.y - 1.0, a_pos.z * (2.0 / 65535.0) - 1.0, 1.0);\n";
		s += "  v_col0 = vc; v_col1 = vec3(0.0);\n";
		s += "  v_tc = vec3(a_uv * u_fbsize.zw, 1.0);\n";
		s += "  v_fog = 1.0;\n";
		s += "}\n";
		return s;
	}
	s += "  vec4 wp = u_world * vec4(a_pos, 1.0);\n";
	s += (k & K_REVNORMAL) ? "  vec3 n0 = -a_nrm;\n" : "  vec3 n0 = a_nrm;\n";
	if (!(k & K_NORMAL)) s += (k & K_REVNORMAL) ? "  n0 = vec3(0.0, 0.0, -1.0);\n" : "  n0 = vec3(0.0, 0.0, 1.0);\n";
	s += "  vec3 wn = nz(mat3(u_world) * n0, vec3(0.0, 0.0, 1.0));\n";
	s += "  vec4 vp = u_view * wp;\n";
	s += "  vec4 clip = u_proj * vp;\n";
	// the GE viewport, linear in clip space so that clipping still works: screen = ndc * scale + centre
	s += "  vec3 sc = clip.xyz * u_vpscale.xyz + u_vpcenter.xyz * clip.w;\n";
	s += "  gl_Position = vec4(sc.x * 2.0 / u_fbsize.x - clip.w, sc.y * 2.0 / u_fbsize.y - clip.w, sc.z * (2.0 / 65535.0) - clip.w, clip.w);\n";
	if (k & K_LIGHTING) {
		int mu = (k >> K_MATUPD_SHIFT) & 7;
		s += (mu & 1) ? "  vec4 ambC = vc;\n" : "  vec4 ambC = u_matamb;\n";
		s += (mu & 2) ? "  vec3 difC = vc.rgb;\n" : "  vec3 difC = u_matdiff.rgb;\n";
		s += (mu & 4) ? "  vec3 spcC = vc.rgb;\n" : "  vec3 spcC = u_matspec.rgb;\n";
		s += "  vec4 l0 = u_ambient * ambC + vec4(u_matemis.rgb, 0.0);\n";
		s += "  vec3 l1 = vec3(0.0);\n";
		s += "  vec3 viewDir = nz(vec3(u_view[0].z, u_view[1].z, u_view[2].z), vec3(0.0, 0.0, 1.0));\n";
		s += "  vec3 L; float ldot, scale, dist, d2;\n";
		for (int i = 0; i < 4; i++) {
			int bits = (int)((k >> (K_LIGHT_SHIFT + i * 5)) & 31);
			if (!(bits & 1)) continue;
			int type = (bits >> 1) & 3, comp = (bits >> 3) & 3;
			char b[1024];
			std::string I = std::to_string(i);
			s += "  scale = 1.0;\n";
			if (type == 0) {
				s += "  L = u_lpos[" + I + "].xyz;\n";   // directional: normalised on upload
			} else {
				s += "  L = u_lpos[" + I + "].xyz - wp.xyz; d2 = dot(L, L); dist = sqrt(d2); L = d2 > 0.0 ? L / dist : L;\n";
				s += "  scale = clamp(1.0 / dot(u_latt[" + I + "].xyz, vec3(1.0, dist, d2)), 0.0, 1.0);\n";
				if (type >= 2) {
					snprintf(b, sizeof b, "  { float a = dot(u_ldir[%d].xyz, L); scale = a >= u_lspot[%d].x ? scale * pspPow(a, u_lspot[%d].y) : 0.0; }\n", i, i, i);
					s += b;
				}
			}
			s += "  ldot = dot(L, wn);\n";
			if (comp == 2) s += "  ldot = pspPow(ldot, u_matspec.a);\n";
			snprintf(b, sizeof b, "  l0.rgb += (u_lamb[%d].rgb * ambC.rgb + u_ldiff[%d].rgb * difC * max(ldot, 0.0)) * scale;\n", i, i);
			s += b;
			if (comp == 1) {
				snprintf(b, sizeof b, "  if (ldot >= 0.0) { vec3 h = nz(L + viewDir, vec3(0.0)); l1 += u_lspec[%d].rgb * spcC * pspPow(dot(h, wn), u_matspec.a) * scale; }\n", i);
				s += b;
			}
		}
		if (k & K_LMODE) s += "  v_col0 = clamp(l0, 0.0, 1.0); v_col1 = clamp(l1, 0.0, 1.0);\n";
		else s += "  v_col0 = clamp(vec4(l0.rgb + l1, l0.a), 0.0, 1.0); v_col1 = vec3(0.0);\n";
	} else {
		s += "  v_col0 = vc; v_col1 = vec3(0.0);\n";
	}
	if (k & K_TEX) {
		int map = (k >> K_TEXMAP_SHIFT) & 3, src = (k >> K_PROJSRC_SHIFT) & 3;
		if (map == 1) {
			const char *srcs[4] = {"vec4(a_pos, 1.0)", "vec4(a_uv, 0.0, 1.0)", "vec4(nz(n0, vec3(0.0)), 1.0)", "vec4(n0, 1.0)"};
			s += std::string("  v_tc = (u_tgen * ") + srcs[src] + ").xyz;\n";
			// (the texture scale and offset only apply to UV mapping, not to the texture matrix)
		} else {
			s += "  v_tc = vec3(a_uv * u_uvso.xy + u_uvso.zw, 1.0);\n";
		}
	} else {
		s += "  v_tc = vec3(0.0, 0.0, 1.0);\n";
	}
	s += (k & K_FOG) ? "  v_fog = (vp.z + u_fog.x) * u_fog.y;\n" : "  v_fog = 1.0;\n";
	s += "}\n";
	return s;
}

static std::string gen_fs(u64 k) {
	std::string s = "#version 330\n";
	s += kUboGlsl;
	s += (k & K_FLAT) ? "flat in vec4 v_col0; flat in vec3 v_col1;\n" : "in vec4 v_col0; in vec3 v_col1;\n";
	s += "in vec3 v_tc; in float v_fog;\nuniform sampler2D u_tex; uniform sampler2D u_pal;\nout vec4 o_col;\n";
	s += "void main() {\n";
	if (k & K_CLEAR) {
		s += "  o_col = v_col0;\n}\n";
		return s;
	}
	s += "  vec4 c = v_col0;\n";
	if (k & K_TEX) {
		s += (k & K_TEXPROJ) ? "  vec2 uv = v_tc.z != 0.0 ? v_tc.xy / v_tc.z : vec2(0.0);\n" : "  vec2 uv = v_tc.xy;\n";
		s += "  uv = uv * u_fbtex.xy + u_fbtex.zw;\n";
		if (k & K_DEPAL) {
			// a framebuffer read as a 32-bit indexed texture: the index is a byte of each pixel
			s += "  ivec2 ts = textureSize(u_tex, 0);\n";
			s += "  uvec4 b = uvec4(texelFetch(u_tex, clamp(ivec2(uv * vec2(ts)), ivec2(0), ts - 1), 0) * 255.0 + 0.5);\n";
			s += "  uint px = b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);\n";
			s += "  int idx = (int(px >> uint(u_depal.x)) & u_depal.y) | u_depal.z;\n";
			s += "  vec4 t = texelFetch(u_pal, ivec2(idx & 255, 0), 0);\n";
		} else {
			s += "  vec4 t = texture(u_tex, uv);\n";
		}
		bool ta = (k & K_TEXALPHA) != 0;
		switch ((k >> K_TEXFUNC_SHIFT) & 7) {
		case 1:  // decal
			s += ta ? "  c = vec4(mix(c.rgb, t.rgb, t.a), c.a);\n" : "  c = vec4(t.rgb, c.a);\n";
			break;
		case 2:  // blend with the environment colour
			s += ta ? "  c = vec4(mix(c.rgb, u_texenv.rgb, t.rgb), c.a * t.a);\n" : "  c = vec4(mix(c.rgb, u_texenv.rgb, t.rgb), c.a);\n";
			break;
		case 3:  // replace
			s += ta ? "  c = t;\n" : "  c = vec4(t.rgb, c.a);\n";
			break;
		case 4:  // add
			s += ta ? "  c = vec4(c.rgb + t.rgb, c.a * t.a);\n" : "  c = vec4(c.rgb + t.rgb, c.a);\n";
			break;
		default:  // modulate
			s += ta ? "  c = c * t;\n" : "  c = vec4(c.rgb * t.rgb, c.a);\n";
			break;
		}
		if (k & K_DOUBLE) s += "  c.rgb *= 2.0;\n";
	}
	s += "  c.rgb = clamp(c.rgb + v_col1, 0.0, 1.0);\n";
	int at = (k >> K_ATEST_SHIFT) & 7;
	if (at != 1) {
		static const char *ops[8] = {"false", "true", "==", "!=", "<", "<=", ">", ">="};
		if (at == 0) s += "  discard;\n";
		else s += std::string("  { int a = int(c.a * 255.0 + 0.5) & u_atest.y; if (!(a ") + ops[at] + " (u_atest.x & u_atest.y))) discard; }\n";
	}
	if (k & K_FOG) s += "  c.rgb = mix(u_fogcolor.rgb, c.rgb, clamp(v_fog, 0.0, 1.0));\n";
	s += "  o_col = c;\n}\n";
	return s;
}

static std::unordered_map<u64, GLuint> s_programs;
static GLuint s_ubo;

static GLuint program_for(u64 key) {
	auto it = s_programs.find(key);
	if (it != s_programs.end()) return it->second;
	GLuint p = link(gen_vs(key), gen_fs(key));
	GLuint bi = glGetUniformBlockIndex(p, "U");
	if (bi != GL_INVALID_INDEX) glUniformBlockBinding(p, bi, 0);
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "u_tex"), 0);
	glUniform1i(glGetUniformLocation(p, "u_pal"), 1);
	st.prog = p;
	s_programs[key] = p;
	return p;
}

// ================================================================= framebuffers
struct Fb {
	u32 addr;
	int stride, fmt;
	int w, h;           // allocated, PSP pixels
	int usedH;          // rows drawn into (for texturing from it)
	GLuint fbo, color, depth;
	bool drawn;         // since its last write back to memory
	u32 lastFrame;
};
static std::vector<Fb *> s_fbs;
static Fb *s_cur;
static u32 s_dispAddr, s_dispStride = 512;
static int s_dispFmt;

static int fmt_bpp(int fmt) { return fmt == FMT_8888 ? 4 : 2; }

static void fb_create_gl(Fb *f) {
	glGenTextures(1, &f->color);
	glBindTexture(GL_TEXTURE_2D, f->color);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f->w * s_scale, f->h * s_scale, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenRenderbuffers(1, &f->depth);
	glBindRenderbuffer(GL_RENDERBUFFER, f->depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, f->w * s_scale, f->h * s_scale);
	glGenFramebuffers(1, &f->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->color, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, f->depth);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) NG_LOGE("ngpu: framebuffer %08x incomplete", f->addr);
	st.fbo = f->fbo;
}

static void fb_delete_gl(Fb *f) {
	glDeleteFramebuffers(1, &f->fbo);
	glDeleteTextures(1, &f->color);
	glDeleteRenderbuffers(1, &f->depth);
	f->fbo = f->color = f->depth = 0;
}

// (Re)allocates at w x h, keeping what it held.
static void fb_resize(Fb *f, int w, int h) {
	Fb old = *f;
	f->w = w; f->h = h;
	fb_create_gl(f);
	glDisable(GL_SCISSOR_TEST); st.sc[0] = -1;
	glClearColor(0, 0, 0, 0); glClearDepth(0); glClearStencil(0);
	glColorMask(1, 1, 1, 1); glDepthMask(1); st.cmask = -1; st.dmask = -1;
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	if (old.fbo) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, old.fbo);
		int cw = std::min(old.w, w) * s_scale, ch = std::min(old.h, h) * s_scale;
		glBlitFramebuffer(0, 0, cw, ch, 0, 0, cw, ch, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, f->fbo);
		fb_delete_gl(&old);
	}
	glEnable(GL_SCISSOR_TEST);
}

static Fb *fb_find(u32 addr) {
	for (Fb *f : s_fbs)
		if (f->addr == addr) return f;
	return nullptr;
}

static Fb *fb_get(u32 addr, int stride, int fmt, int needH) {
	needH = std::max(1, std::min(needH, 512));
	if (stride <= 0) stride = 512;
	Fb *f = fb_find(addr);
	if (!f) {
		f = new Fb();
		f->addr = addr; f->stride = stride; f->fmt = fmt;
		s_fbs.push_back(f);
		fb_resize(f, stride, stride >= 480 ? std::max(needH, 272) : needH);
		NG_LOGD("ngpu: framebuffer %08x %dx%d format %d", addr, f->w, f->h, fmt);
	}
	f->fmt = fmt;
	if (f->stride != stride || needH > f->h) {
		f->stride = stride;
		fb_resize(f, stride, std::max(needH, f->h));
	}
	return f;
}

// The framebuffer a VRAM texture address falls in, with the texel offset into it.
static Fb *fb_for_texture(u32 addr, int *ox, int *oy) {
	addr = 0x04000000 | (addr & 0x1FFFFF);
	Fb *best = nullptr;
	for (Fb *f : s_fbs) {
		u32 pitch = f->stride * fmt_bpp(f->fmt);
		u32 end = f->addr + pitch * std::max(f->usedH, 1);
		if (addr >= f->addr && addr < end && (!best || f->addr > best->addr)) best = f;
	}
	if (best) {
		u32 off = addr - best->addr, pitch = best->stride * fmt_bpp(best->fmt);
		*oy = off / pitch;
		*ox = (off % pitch) / fmt_bpp(best->fmt);
	}
	return best;
}

// ================================================================= split views (0xFA commands)
// As in the PPSSPP OpenGL backend (runtime/gpu/ge_gl.cpp): each view draws into a framebuffer of its own the
// size of the game's, starting from the game's depth and the first view's colour, and the views are scaled into
// their parts of the game's framebuffer when the game composes them.
#define MAX_VIEWS 4
struct ViewFb { GLuint fbo, color, depth; int w, h; };
static ViewFb s_view[MAX_VIEWS];
static bool s_viewFresh[MAX_VIEWS];
static int s_viewDst[MAX_VIEWS][4], s_viewSrc[MAX_VIEWS][4], s_viewN;
static Fb *s_viewBase;   // the game's framebuffer while a view draws
static int s_viewCur = -1;

static GLuint target_fbo(Fb *f) {
	return f == s_viewBase && s_viewCur >= 0 ? s_view[s_viewCur].fbo : f->fbo;
}

static void bind_target(Fb *f) {
	GLuint fbo = target_fbo(f);
	if (st.fbo != fbo) { st.fbo = fbo; glBindFramebuffer(GL_FRAMEBUFFER, fbo); }
	st_viewport(0, 0, f->w * s_scale, f->h * s_scale);
}

static void view_alloc(ViewFb *v, int w, int h) {
	if (v->fbo && v->w == w && v->h == h) return;
	if (v->fbo) { glDeleteFramebuffers(1, &v->fbo); glDeleteTextures(1, &v->color); glDeleteRenderbuffers(1, &v->depth); }
	v->w = w; v->h = h;
	glGenTextures(1, &v->color);
	glBindTexture(GL_TEXTURE_2D, v->color);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glGenRenderbuffers(1, &v->depth);
	glBindRenderbuffer(GL_RENDERBUFFER, v->depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
	glGenFramebuffers(1, &v->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, v->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, v->color, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, v->depth);
	st.fbo = v->fbo;
}

static void blit(GLuint from, GLuint to, int s0, int s1, int s2, int s3, int d0, int d1, int d2, int d3, GLbitfield what, GLenum filter) {
	glBindFramebuffer(GL_READ_FRAMEBUFFER, from);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to);
	glBlitFramebuffer(s0, s1, s2, s3, d0, d1, d2, d3, what, filter);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	st.fbo = ~0u;
}

void ng_gl_rcsm(u32 data) {
	if (!s_ready) return;
	int kind = data & 0xFF, arg = (data >> 8) & 0xFF;
	s_viewCur = -1;
	Fb *f = s_cur;
	if (!f) return;
	glDisable(GL_SCISSOR_TEST); st.sc[0] = -1;
	int w = f->w * s_scale, h = f->h * s_scale;
	if (kind == 1 && arg < MAX_VIEWS) {
		view_alloc(&s_view[arg], w, h);
		if (!s_viewFresh[arg]) {
			GLuint from = arg && s_viewFresh[0] ? s_view[0].fbo : f->fbo;
			blit(f->fbo, s_view[arg].fbo, 0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
			blit(from, s_view[arg].fbo, 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}
		s_viewFresh[arg] = true;
		s_viewBase = f;
		s_viewCur = arg;
	} else if (kind == 2) {
		for (int v = 0; v < std::min(arg, s_viewN); v++) {
			int from = s_view[v].fbo && s_viewFresh[v] ? v : 0;
			if (!s_view[from].fbo || !s_viewFresh[from] || s_view[from].w != w || s_view[from].h != h) continue;
			const int *sr = s_viewSrc[from == v ? v : 0], *d = s_viewDst[v];
			int S = s_scale;
			blit(s_view[from].fbo, f->fbo, sr[0] * S, sr[1] * S, (sr[0] + sr[2]) * S, (sr[1] + sr[3]) * S,
			     d[0] * S, d[1] * S, (d[0] + d[2]) * S, (d[1] + d[3]) * S, GL_COLOR_BUFFER_BIT, GL_LINEAR);
			blit(s_view[from].fbo, f->fbo, sr[0] * S, sr[1] * S, (sr[0] + sr[2]) * S, (sr[1] + sr[3]) * S,
			     d[0] * S, d[1] * S, (d[0] + d[2]) * S, (d[1] + d[3]) * S, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
		}
	}
	glEnable(GL_SCISSOR_TEST);
}

static void views_new_frame() {
	s_viewCur = -1;
	for (bool &b : s_viewFresh) b = false;
}

// ================================================================= textures
struct TexEntry {
	GLuint tex;
	int w, h;
	u64 hash;
	u32 checked;   // frame of the last content check
};
static std::unordered_map<u64, TexEntry> s_texCache;
static u8 s_clutRaw[1024];
static u64 s_clutHash;
static u32 s_clutTotal, s_clutMax;   // bytes the last CLUT load brought, and the most any did
static GLuint s_palTex;
static u64 s_palTexHash = ~0ull;
static std::vector<u32> s_decodeBuf;

void ng_gl_loadclut(u32 addr, int bytes) {
	if (bytes > 1024) bytes = 1024;
	if (bytes <= 0 || !ng_valid(addr, bytes)) return;
	memcpy(s_clutRaw, ng_ptr(addr), bytes);
	s_clutHash = ng_hash(s_clutRaw, 1024);
	s_clutTotal = bytes;
	s_clutMax = std::max<u32>(s_clutMax, bytes);
}

static bool s_pixelMapped;
static void set_sampler(bool linMin, bool linMag, bool clampS, bool clampT) {
	if (s_filter == 1 || (s_filter == 0 && s_pixelMapped)) linMin = linMag = false;
	else if (s_filter == 2) linMin = linMag = true;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linMin ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linMag ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clampS ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clampT ? GL_CLAMP_TO_EDGE : GL_REPEAT);
}

// ---- texture replacement and dumping (gpu/tex_replace.cpp keeps the folders): textures are named as the
// PPSSPP renderer names them, "<palette hash><contents hash>.png", so packs work with both
extern "C" int ge_tex_replace_active(void);
extern "C" int ge_tex_find(u32 addr, const char *name, char *path, int size);
extern "C" void ge_tex_dump(const char *name, const void *rgba, int w, int h);
extern "C" int ge_tex_load_png(const char *path, unsigned char **rgba, int *w, int *h);

static const int kTexBits[16] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0};

// The texture's row pitch in texels, as the GE reads it (16-byte aligned).
static int tex_bufw(int fmt, u32 raw) {
	if (fmt >= FMT_DXT1) return raw & 0x7FF;
	int bits = kTexBits[fmt];
	int bufw = raw & 0x7FF & ~((128 / bits) - 1);
	return bufw ? bufw : 128 / bits;
}

static void replacement_name(char *name, u32 addr, int fmt, int w, int h, int bufw) {
	const GeState &g = g_ge;
	u32 bits = kTexBits[fmt];
	const u8 *p = ng_ptr(addr);
	u32 contents;
	if (bufw <= w) {
		contents = ng_xxh32(p, bits * (bufw * h + (w - bufw)) / 8, 0xBACD7814);
	} else {
		u32 line = bits * w / 8, stride = bits * bufw / 8;
		contents = 0;
		for (int y = 0; y < h; y++, p += stride) contents = (contents * 11) ^ ng_xxh32(p, line, 0xBACD7814);
	}
	u32 clut = g.cmd[CMD_TEXSIZE0] & 0xF0F;
	if (fmt >= FMT_CLUT4 && fmt <= FMT_CLUT32) {
		u32 baseBytes = g.clutBase() * (g.clutFormat() == 3 ? 4 : 2);
		u32 bytes = std::min(s_clutTotal + baseBytes, std::min<u32>(s_clutMax, 1024));
		clut ^= ng_xxh32(s_clutRaw, bytes, 0xC0108888) ^ g.cmd[CMD_CLUTFORMAT];
	}
	sprintf(name, "%08x%08x.png", clut, contents);
}

struct LoadedPng { std::vector<u32> px; int w = 0, h = 0; };
static std::unordered_map<std::string, LoadedPng> s_pngs;

// Binds the draw's texture on unit 0 and fills the texture mapping uniforms. Returns false if there's none.

// Whether a rectangle draw maps texels 1:1 onto PSP pixels. Like PPSSPP's "smart 2D texture filtering", those
// sample nearest whatever filter the game asked for, so that sprites and text stay sharp at higher resolutions.
static bool pixel_mapped(int prim, const NgVert *v, int count, bool through) {
	if (prim != PRIM_RECTANGLES || !through || count < 2) return false;
	for (int i = 0; i + 1 < count; i += 2) {
		float dx = v[i + 1].pos[0] - v[i].pos[0], dy = v[i + 1].pos[1] - v[i].pos[1];
		float du = v[i + 1].uv[0] - v[i].uv[0], dv = v[i + 1].uv[1] - v[i].uv[1];
		if (dx <= 0 || dy <= 0 || (dx != du && dy != dv)) return false;
	}
	return true;
}

static bool bind_texture(Ubo &u, u64 &key) {
	const GeState &g = g_ge;
	u32 addr = g.texAddr(0);
	int fmt = g.texFormat(), w = g.texW(0), h = g.texH(0), stride = tex_bufw(g.texFormat(), g.cmd[CMD_TEXBW0]);
	bool linMin = g.cmd[CMD_TEXFILTER] & 1, linMag = (g.cmd[CMD_TEXFILTER] >> 8) & 1;
	bool clampS = g.cmd[CMD_TEXWRAP] & 1, clampT = (g.cmd[CMD_TEXWRAP] >> 8) & 1;
	u.fbsize[2] = 1.0f / w; u.fbsize[3] = 1.0f / h;
	u.fbtex[0] = 1; u.fbtex[1] = 1; u.fbtex[2] = 0; u.fbtex[3] = 0;
	glActiveTexture(GL_TEXTURE0);

	// a framebuffer?
	if ((addr & 0x0F000000) == 0x04000000) {
		int ox = 0, oy = 0;
		Fb *f = fb_for_texture(addr, &ox, &oy);
		bool direct = f && ((fmt == f->fmt) || (fmt <= FMT_4444 && f->fmt <= FMT_4444));
		bool depal = f && fmt == FMT_CLUT32 && f->fmt == FMT_8888;
		if (direct || depal) {
			if (f == s_cur) NG_LOGD("ngpu: texture from the framebuffer being drawn (%08x)", f->addr);
			glBindTexture(GL_TEXTURE_2D, f->color);
			u.fbtex[0] = (float)w / f->w; u.fbtex[1] = (float)h / f->h;
			u.fbtex[2] = (float)ox / f->w; u.fbtex[3] = (float)oy / f->h;
			if (depal) {
				set_sampler(false, false, true, true);
				key |= K_DEPAL;
				u.depal[0] = g.clutShift(); u.depal[1] = g.clutMask(); u.depal[2] = g.clutBase() & 0xFF;
				glActiveTexture(GL_TEXTURE1);
				glBindTexture(GL_TEXTURE_2D, s_palTex);
				glActiveTexture(GL_TEXTURE0);
				if (s_palTexHash != s_clutHash) {
					s_palTexHash = s_clutHash;
					glActiveTexture(GL_TEXTURE1);
					glPixelStorei(GL_UNPACK_ALIGNMENT, 4); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
					u32 pal[256];
					for (int i = 0; i < 256; i++) {
						if (g.clutFormat() == 3) memcpy(&pal[i], s_clutRaw + i * 4, 4);
						else { u16 c; memcpy(&c, s_clutRaw + i * 2, 2); pal[i] = ng_color16(c, g.clutFormat()); }
					}
					glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, pal);
					glActiveTexture(GL_TEXTURE0);
				}
			} else {
				set_sampler(linMin, linMag, clampS, clampT);
			}
			return true;
		}
	}

	// from memory
	u32 bytes = ng_tex_bytes(fmt, std::max(stride, 1), h);
	if (g.texSwizzle()) bytes = ng_tex_bytes(fmt, std::max(stride, 1), (h + 7) & ~7);
	if (stride <= 0 || !ng_valid(addr, bytes)) return false;
	bool indexed = fmt >= FMT_CLUT4 && fmt <= FMT_CLUT32;
	u64 k = ng_hash(&addr, 4, (u64)fmt << 56 ^ (u64)w << 40 ^ (u64)h << 24 ^ (u64)stride << 8 ^ (g.texSwizzle() ? 1 : 0));
	if (indexed) k = ng_hash(&s_clutHash, 8, k ^ (g.cmd[CMD_CLUTFORMAT] & 0x1FFFFF));
	TexEntry &e = s_texCache[k];
	bool fresh = !e.tex;
	if (fresh) glGenTextures(1, &e.tex);
	glBindTexture(GL_TEXTURE_2D, e.tex);
	if (fresh || e.checked != s_texEpoch) {
		u64 hsh = ng_hash(ng_ptr(addr), bytes);
		e.checked = s_texEpoch;
		if (fresh || hsh != e.hash) {
			e.hash = hsh;
			NgClut clut;
			if (indexed) ng_clut_set(&clut, s_clutRaw, g.clutFormat(), g.clutShift(), g.clutMask(), g.clutBase());
			s_decodeBuf.resize((size_t)w * h);
			if (!ng_tex_decode(ng_ptr(addr), fmt, w, h, stride, g.texSwizzle(), &clut, s_decodeBuf.data())) {
				static bool warned[16];
				if (!warned[fmt & 15]) { warned[fmt & 15] = true; NG_LOGE("ngpu: texture format %d isn't supported", fmt); }
				std::fill(s_decodeBuf.begin(), s_decodeBuf.end(), 0xFFFF00FFu);
			}
			const u32 *px = s_decodeBuf.data();
			int pw = w, ph = h;
			if (ge_tex_replace_active()) {
				char name[32], path[1024];
				replacement_name(name, addr, fmt, w, h, stride);
				if (ge_tex_find(addr, name, path, sizeof path)) {
					LoadedPng &png = s_pngs[path];
					if (!png.w) {
						unsigned char *data = nullptr;
						if (ge_tex_load_png(path, &data, &png.w, &png.h)) {
							png.px.assign((u32 *)data, (u32 *)data + (size_t)png.w * png.h);
							free(data);
						} else {
							NG_LOGE("texture replacement %s couldn't be read as PNG", path);
							png.w = -1;
						}
					}
					if (png.w > 0) { px = png.px.data(); pw = png.w; ph = png.h; }
				} else {
					ge_tex_dump(name, px, w, h);
				}
			}
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
			if (fresh || e.w != pw || e.h != ph) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pw, ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
			else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, px);
			e.w = pw; e.h = ph;
			if (fresh && s_aniso > 1) glTexParameterf(GL_TEXTURE_2D, 0x84FE /*GL_TEXTURE_MAX_ANISOTROPY*/, (float)s_aniso);
		}
	}
	set_sampler(linMin, linMag, clampS, clampT);
	return true;
}

// ================================================================= drawing
static GLuint s_vao, s_vbo, s_ibo;
static const int kVboVerts = 1 << 17, kIboIdx = 1 << 19;
static int s_vboPos, s_iboPos;
static std::vector<NgVert> s_verts;
static std::vector<u16> s_idx;
static Ubo s_ubo_data, s_ubo_last;
static bool s_ubo_valid;

static void mat43(float *out, const float *m) {   // GE 4x3 (column major) -> mat4
	out[0] = m[0]; out[1] = m[1]; out[2] = m[2]; out[3] = 0;
	out[4] = m[3]; out[5] = m[4]; out[6] = m[5]; out[7] = 0;
	out[8] = m[6]; out[9] = m[7]; out[10] = m[8]; out[11] = 0;
	out[12] = m[9]; out[13] = m[10]; out[14] = m[11]; out[15] = 1;
}

static void rgb(float *out, u32 c, float a = 0) {
	out[0] = (c & 0xFF) / 255.0f; out[1] = ((c >> 8) & 0xFF) / 255.0f; out[2] = ((c >> 16) & 0xFF) / 255.0f; out[3] = a;
}

static GLenum blend_src(int f, u32 fix, bool &useConst) {
	switch (f) {
	case 0: return GL_DST_COLOR;
	case 1: return GL_ONE_MINUS_DST_COLOR;
	case 2: return GL_SRC_ALPHA;
	case 3: return GL_ONE_MINUS_SRC_ALPHA;
	case 4: return GL_DST_ALPHA;
	case 5: return GL_ONE_MINUS_DST_ALPHA;
	case 6: return GL_SRC_ALPHA;            // (doubled: approximated)
	case 7: return GL_ONE_MINUS_SRC_ALPHA;
	case 8: return GL_DST_ALPHA;
	case 9: return GL_ONE_MINUS_DST_ALPHA;
	default:
		if ((fix & 0xFFFFFF) == 0xFFFFFF) return GL_ONE;
		if ((fix & 0xFFFFFF) == 0) return GL_ZERO;
		useConst = true;
		return GL_CONSTANT_COLOR;
	}
}
static GLenum blend_dst(int f, u32 fix, u32 fixA, bool &useConst) {
	switch (f) {
	case 0: return GL_SRC_COLOR;
	case 1: return GL_ONE_MINUS_SRC_COLOR;
	case 2: return GL_SRC_ALPHA;
	case 3: return GL_ONE_MINUS_SRC_ALPHA;
	case 4: return GL_DST_ALPHA;
	case 5: return GL_ONE_MINUS_DST_ALPHA;
	case 6: return GL_SRC_ALPHA;
	case 7: return GL_ONE_MINUS_SRC_ALPHA;
	case 8: return GL_DST_ALPHA;
	case 9: return GL_ONE_MINUS_DST_ALPHA;
	default:
		if ((fix & 0xFFFFFF) == 0xFFFFFF) return GL_ONE;
		if ((fix & 0xFFFFFF) == 0) return GL_ZERO;
		if (useConst && ((fix ^ fixA) & 0xFFFFFF) == 0xFFFFFF) return GL_ONE_MINUS_CONSTANT_COLOR;
		useConst = true;
		return GL_CONSTANT_COLOR;
	}
}

static void set_raster_state(int prim, bool through) {
	const GeState &g = g_ge;
	if (g.clearMode()) {
		u32 cm = g.cmd[CMD_CLEARMODE];
		st_cap(GL_BLEND, 0, st.blend);
		st_cap(GL_CULL_FACE, 0, st.cull);
		bool dw = (cm & 0x400) != 0;
		st_cap(GL_DEPTH_TEST, dw, st.depth);
		if (dw) {
			if (st.dfunc != GL_ALWAYS) { st.dfunc = GL_ALWAYS; glDepthFunc(GL_ALWAYS); }
			if (st.dmask != 1) { st.dmask = 1; glDepthMask(1); }
		}
		int c = (cm & 0x100) ? 7 : 0, a = (cm & 0x200) ? 8 : 0;
		if (st.cmask != (c | a)) { st.cmask = c | a; glColorMask(c != 0, c != 0, c != 0, a != 0); }
		return;
	}
	// blending
	if (g.cmd[CMD_BLEND_EN] & 1) {
		st_cap(GL_BLEND, 1, st.blend);
		u32 b = g.cmd[CMD_BLEND];
		bool useConst = false;
		u32 fa = g.cmd[CMD_FIXA] & 0xFFFFFF, fb = g.cmd[CMD_FIXB] & 0xFFFFFF;
		GLenum s = blend_src(b & 0xF, fa, useConst);
		GLenum d = blend_dst((b >> 4) & 0xF, fb, fa, useConst);
		GLenum eq;
		switch ((b >> 8) & 7) {
		case 1: eq = GL_FUNC_SUBTRACT; break;
		case 2: eq = GL_FUNC_REVERSE_SUBTRACT; break;
		case 3: eq = GL_MIN; break;
		case 4: eq = GL_MAX; break;
		default: eq = GL_FUNC_ADD; break;   // (5, absolute difference: approximated)
		}
		if (st.bsrc != s || st.bdst != d) { st.bsrc = s; st.bdst = d; glBlendFuncSeparate(s, d, GL_ONE, GL_ZERO); }
		if (st.beq != eq) { st.beq = eq; glBlendEquationSeparate(eq, GL_FUNC_ADD); }
		if (useConst) {
			u32 cc = (s == GL_CONSTANT_COLOR) ? fa : fb;
			if (st.bcolor != cc) { st.bcolor = cc; glBlendColor((cc & 0xFF) / 255.0f, ((cc >> 8) & 0xFF) / 255.0f, ((cc >> 16) & 0xFF) / 255.0f, 1.0f); }
		}
	} else {
		st_cap(GL_BLEND, 0, st.blend);
	}
	// depth
	bool zt = g.cmd[CMD_ZTEST_EN] & 1;
	st_cap(GL_DEPTH_TEST, zt, st.depth);
	if (zt) {
		static const GLenum funcs[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
		GLenum fn = funcs[g.cmd[CMD_ZTEST] & 7];
		if (st.dfunc != fn) { st.dfunc = fn; glDepthFunc(fn); }
		int dm = !(g.cmd[CMD_ZWRITE_DIS] & 1);
		if (st.dmask != dm) { st.dmask = dm; glDepthMask(dm); }
	}
	// colour mask: the GE's per-bit mask, per channel here; the alpha channel holds the stencil, which only
	// stencil operations (not used by the game) and clears change
	u32 m = g.cmd[CMD_MASKRGB] & 0xFFFFFF;
	int cm = ((m & 0xFF) != 0xFF ? 1 : 0) | ((m & 0xFF00) != 0xFF00 ? 2 : 0) | ((m & 0xFF0000) != 0xFF0000 ? 4 : 0);
	if (st.cmask != cm) { st.cmask = cm; glColorMask(cm & 1, (cm >> 1) & 1, (cm >> 2) & 1, 0); }
	// culling
	bool cull = !through && (g.cmd[CMD_CULL_EN] & 1) && prim >= PRIM_TRIANGLES && prim != PRIM_RECTANGLES;
	st_cap(GL_CULL_FACE, cull, st.cull);
	if (cull) {
		int face = g.cmd[CMD_CULL] & 1;
		// (rows are stored top first, which mirrors the PSP's winding)
		if (st.cullFace != face) { st.cullFace = face; glCullFace(GL_BACK); glFrontFace(face ? GL_CCW : GL_CW); }
	}
}

static void fill_ubo(Ubo &u, Fb *f, bool through) {
	const GeState &g = g_ge;
	u.fbsize[0] = (float)f->w; u.fbsize[1] = (float)f->h;
	if (!through) {
		mat43(u.world, g.world);
		float view[12];
		memcpy(view, g.view, sizeof view);
		view[9] += s_viewOff[0]; view[10] += s_viewOff[1];
		mat43(u.view, view);
		memcpy(u.proj, g.proj, sizeof u.proj);
		mat43(u.tgen, g.tgen);
		u.vpscale[0] = g.f(CMD_VP_XS); u.vpscale[1] = g.f(CMD_VP_YS); u.vpscale[2] = g.f(CMD_VP_ZS); u.vpscale[3] = 0;
		u.vpcenter[0] = g.f(CMD_VP_XC) - g.offsetX16() / 16.0f;
		u.vpcenter[1] = g.f(CMD_VP_YC) - g.offsetY16() / 16.0f;
		u.vpcenter[2] = g.f(CMD_VP_ZC); u.vpcenter[3] = 0;
		u.uvso[0] = g.f(CMD_TEX_SU); u.uvso[1] = g.f(CMD_TEX_SV); u.uvso[2] = g.f(CMD_TEX_OU); u.uvso[3] = g.f(CMD_TEX_OV);
		u32 ma = (g.cmd[CMD_MAT_AMBIENT] & 0xFFFFFF) | (g.cmd[CMD_MAT_ALPHA] & 0xFF) << 24;
		rgb(u.matamb, ma, (ma >> 24) / 255.0f);
		rgb(u.matdiff, g.cmd[CMD_MAT_DIFFUSE]);
		rgb(u.matspec, g.cmd[CMD_MAT_SPECULAR], g.f(CMD_MAT_SPECCOEF));
		rgb(u.matemis, g.cmd[CMD_MAT_EMISSIVE]);
		rgb(u.ambient, g.cmd[CMD_AMBIENT], (g.cmd[CMD_AMBIENT_A] & 0xFF) / 255.0f);
		for (int i = 0; i < 4; i++) {
			float p[3] = {g.f(CMD_LX0 + i * 3), g.f(CMD_LX0 + i * 3 + 1), g.f(CMD_LX0 + i * 3 + 2)};
			if (((g.cmd[CMD_LTYPE0 + i] >> 8) & 3) == 0) {
				float l = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
				if (l > 0) for (float &c : p) c /= l;
			}
			memcpy(u.lpos[i], p, 12);
			float d[3] = {g.f(CMD_LDX0 + i * 3), g.f(CMD_LDX0 + i * 3 + 1), g.f(CMD_LDX0 + i * 3 + 2)};
			float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			if (l > 0) for (float &c : d) c /= l;
			memcpy(u.ldir[i], d, 12);
			u.latt[i][0] = g.f(CMD_LKA0 + i * 3); u.latt[i][1] = g.f(CMD_LKA0 + i * 3 + 1); u.latt[i][2] = g.f(CMD_LKA0 + i * 3 + 2);
			u.lspot[i][0] = g.f(CMD_LKO0 + i); u.lspot[i][1] = std::min(g.f(CMD_LKS0 + i), 496.0f);
			rgb(u.lamb[i], g.cmd[CMD_LAC0 + i * 3]);
			rgb(u.ldiff[i], g.cmd[CMD_LDC0 + i * 3]);
			rgb(u.lspec[i], g.cmd[CMD_LSC0 + i * 3]);
		}
		u.fog[0] = g.f(CMD_FOG1); u.fog[1] = g.f(CMD_FOG2);
		if (!std::isfinite(u.fog[1])) u.fog[1] = u.fog[1] < 0 ? -1e30f : 1e30f;
		rgb(u.fogcolor, g.cmd[CMD_FOGCOLOR]);
	} else {
		u32 ma = (g.cmd[CMD_MAT_AMBIENT] & 0xFFFFFF) | (g.cmd[CMD_MAT_ALPHA] & 0xFF) << 24;
		rgb(u.matamb, ma, (ma >> 24) / 255.0f);
	}
	rgb(u.texenv, g.cmd[CMD_TEXENV]);
	u.atest[0] = (g.cmd[CMD_ATEST] >> 8) & 0xFF; u.atest[1] = (g.cmd[CMD_ATEST] >> 16) & 0xFF;
}

static int assemble(int prim, const NgVert *v, int count, bool through, GLenum *mode) {
	s_idx.clear();
	s_verts.clear();
	*mode = GL_TRIANGLES;
	switch (prim) {
	case PRIM_POINTS: *mode = GL_POINTS; s_verts.assign(v, v + count); for (int i = 0; i < count; i++) s_idx.push_back(i); break;
	case PRIM_LINES: *mode = GL_LINES; s_verts.assign(v, v + count); for (int i = 0; i + 1 < count; i += 2) { s_idx.push_back(i); s_idx.push_back(i + 1); } break;
	case PRIM_LINE_STRIP: *mode = GL_LINES; s_verts.assign(v, v + count); for (int i = 0; i + 1 < count; i++) { s_idx.push_back(i); s_idx.push_back(i + 1); } break;
	case PRIM_TRIANGLES: s_verts.assign(v, v + count); for (int i = 0; i + 2 < count; i += 3) { s_idx.push_back(i); s_idx.push_back(i + 1); s_idx.push_back(i + 2); } break;
	case PRIM_TRI_STRIP:
		s_verts.assign(v, v + count);
		for (int i = 0; i + 2 < count; i++) {
			if (i & 1) { s_idx.push_back(i + 1); s_idx.push_back(i); }
			else { s_idx.push_back(i); s_idx.push_back(i + 1); }
			s_idx.push_back(i + 2);
		}
		break;
	case PRIM_TRI_FAN:
		s_verts.assign(v, v + count);
		for (int i = 1; i + 1 < count; i++) { s_idx.push_back(0); s_idx.push_back(i); s_idx.push_back(i + 1); }
		break;
	case PRIM_RECTANGLES:
		if (!through) {
			static bool warned;
			if (!warned) { warned = true; NG_LOGE("ngpu: rectangles outside through mode aren't supported"); }
			return 0;
		}
		for (int i = 0; i + 1 < count; i += 2) {
			const NgVert &a = v[i], &b = v[i + 1];
			int base = (int)s_verts.size();
			NgVert c[4] = {b, b, b, b};   // colour, z (and the rest) from the second vertex
			c[0].pos[0] = a.pos[0]; c[0].pos[1] = a.pos[1]; c[0].uv[0] = a.uv[0]; c[0].uv[1] = a.uv[1];
			c[1].pos[0] = b.pos[0]; c[1].pos[1] = a.pos[1]; c[1].uv[0] = b.uv[0]; c[1].uv[1] = a.uv[1];
			c[2].pos[0] = a.pos[0]; c[2].pos[1] = b.pos[1]; c[2].uv[0] = a.uv[0]; c[2].uv[1] = b.uv[1];
			c[3].pos[0] = b.pos[0]; c[3].pos[1] = b.pos[1];
			s_verts.insert(s_verts.end(), c, c + 4);
			const int t[6] = {0, 1, 2, 1, 3, 2};
			for (int j : t) s_idx.push_back(base + j);
		}
		break;
	default: return 0;
	}
	return (int)s_idx.size();
}

void ng_gl_prim(int prim, const NgVert *v, int count, const NgLayout &L) {
	if (!s_ready || count <= 0) return;
	const GeState &g = g_ge;
	bool through = L.through, clear = g.clearMode();
	int sx1 = g.scissorX1(), sy1 = g.scissorY1(), sx2 = g.scissorX2(), sy2 = g.scissorY2();
	if (sx2 < sx1 || sy2 < sy1) return;
	Fb *f = fb_get(g.fbAddr(), g.fbStride(), g.fbFormat(), sy2 + 1);
	s_cur = f;
	bind_target(f);
	f->usedH = std::max(f->usedH, std::min(sy2 + 1, f->h));
	f->drawn = true;
	f->lastFrame = s_frame;

	GLenum mode;
	int n = assemble(prim, v, count, through, &mode);
	if (!n) return;

	Ubo &u = s_ubo_data;
	u64 key = 0;
	if (through) key |= K_THROUGH;
	if (L.col) key |= K_COLOR;
	if (clear) {
		key |= K_CLEAR | K_COLOR;
		if (!L.col) key &= ~K_COLOR;
	} else {
		if (L.nrm) key |= K_NORMAL;
		if (!(g.cmd[CMD_SHADE] & 1)) key |= K_FLAT;
		if (!through) {
			if (g.cmd[CMD_LIGHTING_EN] & 1) {
				key |= K_LIGHTING | (u64)(g.cmd[CMD_MATUPDATE] & 7) << K_MATUPD_SHIFT;
				if (g.cmd[CMD_LIGHTMODE] & 1) key |= K_LMODE;
				for (int i = 0; i < 4; i++)
					if (g.cmd[CMD_LIGHT_EN0 + i] & 1) {
						u32 lt = g.cmd[CMD_LTYPE0 + i];
						u64 bits = 1 | ((lt >> 8) & 3) << 1 | (lt & 3) << 3;
						key |= bits << (K_LIGHT_SHIFT + i * 5);
					}
			}
			if (g.cmd[CMD_FOG_EN] & 1) key |= K_FOG;
			if (g.cmd[CMD_REVNORMAL] & 1) key |= K_REVNORMAL;
		}
		u32 at = (g.cmd[CMD_ATEST_EN] & 1) ? (g.cmd[CMD_ATEST] & 7) : 1;
		key |= (u64)at << K_ATEST_SHIFT;
	}
	fill_ubo(u, f, through);
	bool tex = !clear && (g.cmd[CMD_TEX_EN] & 1);
	if (tex) {
		s_pixelMapped = (g.cmd[CMD_TEXFILTER] & 0x101) && pixel_mapped(prim, v, count, through);
		if (bind_texture(u, key)) {
			key |= K_TEX;
			u32 tf = g.cmd[CMD_TEXFUNC];
			key |= (u64)(tf & 7) << K_TEXFUNC_SHIFT;
			if (tf & 0x100) key |= K_TEXALPHA;
			if (tf & 0x10000) key |= K_DOUBLE;
			if (!through) {
				int map = g.cmd[CMD_TEXMAPMODE] & 3, src = (g.cmd[CMD_TEXMAPMODE] >> 8) & 3;
				if (map == 1) key |= (u64)1 << K_TEXMAP_SHIFT | (u64)src << K_PROJSRC_SHIFT | K_TEXPROJ;
			}
		}
	}

	GLuint prog = program_for(key);
	st_program(prog);
	if (!st.bound) {   // (other GL code may have used binding 0)
		st.bound = true;
		glBindBufferBase(GL_UNIFORM_BUFFER, 0, s_ubo);
		s_ubo_valid = false;
	}
	if (!s_ubo_valid || memcmp(&s_ubo_last, &u, sizeof u)) {
		glBindBuffer(GL_UNIFORM_BUFFER, s_ubo);
		glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof u, &u);
		s_ubo_last = u;
		s_ubo_valid = true;
	}
	set_raster_state(prim, through);
	int S = s_scale;
	st_scissor(sx1 * S, sy1 * S, (sx2 - sx1 + 1) * S, (sy2 - sy1 + 1) * S);

	// upload and draw
	int nv = (int)s_verts.size();
	if (nv > kVboVerts || n > kIboIdx) return;
	glBindVertexArray(s_vao);
	glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
	if (s_vboPos + nv > kVboVerts) { glBufferData(GL_ARRAY_BUFFER, kVboVerts * sizeof(NgVert), nullptr, GL_STREAM_DRAW); s_vboPos = 0; }
	glBufferSubData(GL_ARRAY_BUFFER, s_vboPos * sizeof(NgVert), nv * sizeof(NgVert), s_verts.data());
	if (s_iboPos + n > kIboIdx) { glBufferData(GL_ELEMENT_ARRAY_BUFFER, kIboIdx * 2, nullptr, GL_STREAM_DRAW); s_iboPos = 0; }
	glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, s_iboPos * 2, n * 2, s_idx.data());
	glDrawElementsBaseVertex(mode, n, GL_UNSIGNED_SHORT, (void *)(uintptr_t)(s_iboPos * 2), s_vboPos);
	s_vboPos += nv;
	s_iboPos += n;
}

// ================================================================= memory <-> framebuffers
static GLuint s_blitProg, s_memTex;
static GLint s_blitRect, s_blitUv;

static void draw_textured_quad(GLuint tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
	st_program(s_blitProg);
	glUniform4f(s_blitRect, x0, y0, x1, y1);
	glUniform4f(s_blitUv, u0, v0, u1, v1);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);
	glBindVertexArray(s_vao);
	st_cap(GL_BLEND, 0, st.blend); st_cap(GL_DEPTH_TEST, 0, st.depth); st_cap(GL_CULL_FACE, 0, st.cull);
	if (st.cmask != 15) { st.cmask = 15; glColorMask(1, 1, 1, 1); }
	glDisable(GL_SCISSOR_TEST); st.sc[0] = -1;
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glEnable(GL_SCISSOR_TEST);
}

// Guest memory (w x h pixels of format fmt, row pitch stride) as RGBA8 into s_memTex.
static void upload_memory(u32 addr, int stride, int fmt, int w, int h) {
	s_decodeBuf.resize((size_t)w * h);
	int bpp = fmt_bpp(fmt);
	for (int y = 0; y < h; y++) {
		const u8 *row = ng_ptr(addr + y * stride * bpp);
		u32 *o = &s_decodeBuf[(size_t)y * w];
		if (fmt == FMT_8888) memcpy(o, row, (size_t)w * 4);
		else for (int x = 0; x < w; x++) { u16 v; memcpy(&v, row + x * 2, 2); o[x] = ng_color16(v, fmt); }
	}
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, s_memTex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, s_decodeBuf.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// The CPU wrote [addr, addr + size): framebuffers there get the memory's contents.
static void memory_written(u32 addr, u32 size) {
	if (!s_ready) return;
	addr = 0x04000000 | (addr & 0x1FFFFF);
	for (Fb *f : s_fbs) {
		u32 pitch = f->stride * fmt_bpp(f->fmt), end = f->addr + pitch * f->h;
		if (addr >= end || addr + size <= f->addr) continue;
		int rows = std::min<int>(f->h, (std::min(end, addr + size) - f->addr + pitch - 1) / pitch);
		upload_memory(f->addr, f->stride, f->fmt, f->stride, rows);
		glBindFramebuffer(GL_FRAMEBUFFER, f->fbo); st.fbo = f->fbo;
		st_viewport(0, 0, f->w * s_scale, f->h * s_scale);
		draw_textured_quad(s_memTex, -1, -1, 1, -1 + 2.0f * rows / f->h, 0, 0, 1, 1);
		f->usedH = std::max(f->usedH, rows);
	}
}

// Framebuffers the game reads back with the CPU: the exposure buffer (60x34 at 0x04180000, which F_4b750
// averages every frame). Those are written back to guest memory after they've been drawn.
static bool wants_writeback(const Fb *f) { return f->stride <= 64; }

static GLuint s_wbFbo, s_wbTex;
static int s_wbW, s_wbH;
static void writeback(Fb *f) {
	if (g_ge.shadow || !f->drawn) return;
	f->drawn = false;
	int w = std::min(f->stride, f->w), h = f->usedH;
	if (w <= 0 || h <= 0) return;
	if (!s_wbFbo || s_wbW < w || s_wbH < h) {
		if (s_wbFbo) { glDeleteFramebuffers(1, &s_wbFbo); glDeleteTextures(1, &s_wbTex); }
		s_wbW = std::max(w, s_wbW); s_wbH = std::max(h, s_wbH);
		glGenTextures(1, &s_wbTex);
		glBindTexture(GL_TEXTURE_2D, s_wbTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_wbW, s_wbH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glGenFramebuffers(1, &s_wbFbo);
		glBindFramebuffer(GL_FRAMEBUFFER, s_wbFbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_wbTex, 0);
	}
	glDisable(GL_SCISSOR_TEST); st.sc[0] = -1;
	blit(f->fbo, s_wbFbo, 0, 0, w * s_scale, h * s_scale, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, s_wbFbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 4); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	std::vector<u32> px((size_t)w * h);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	glBindFramebuffer(GL_FRAMEBUFFER, 0); st.fbo = ~0u;
	glEnable(GL_SCISSOR_TEST);
	int bpp = fmt_bpp(f->fmt);
	for (int y = 0; y < h; y++) {
		u8 *row = ng_ptr(f->addr + y * f->stride * bpp);
		const u32 *s = &px[(size_t)y * w];
		for (int x = 0; x < w; x++) {
			u32 c = s[x];
			if (f->fmt == FMT_8888) {
				u32 old; memcpy(&old, row + x * 4, 4);
				c = (c & 0x00FFFFFF) | (old & 0xFF000000);   // (alpha is the stencil, which the GL side doesn't keep)
				memcpy(row + x * 4, &c, 4);
			} else {
				u32 r = c & 0xFF, gg = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
				u16 v = f->fmt == FMT_565 ? (u16)(r >> 3 | (gg >> 2) << 5 | (b >> 3) << 11)
				      : f->fmt == FMT_5551 ? (u16)(r >> 3 | (gg >> 3) << 5 | (b >> 3) << 10 | 0x8000)
				      : (u16)(r >> 4 | (gg >> 4) << 4 | (b >> 4) << 8 | 0xF000);
				memcpy(row + x * 2, &v, 2);
			}
		}
	}
}

void ng_gl_list_done(void) {
	if (!s_ready) return;
	s_texEpoch++;
	for (Fb *f : s_fbs)
		if (f->drawn && wants_writeback(f)) writeback(f);
	if (g_ge.shadow) gl_reset_state();   // PPSSPP's renderer runs its GL work later, from a default state
}

// ================================================================= presenting
static const char *kBlitVs = R"(#version 330
uniform vec4 u_rect, u_uv;
out vec2 v_uv;
void main() {
	vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1);
	gl_Position = vec4(mix(u_rect.xy, u_rect.zw, c), 0.0, 1.0);
	v_uv = mix(u_uv.xy, u_uv.zw, c);
}
)";
static const char *kBlitFs = R"(#version 330
uniform sampler2D u_tex;
in vec2 v_uv;
out vec4 o_col;
void main() { o_col = vec4(texture(u_tex, v_uv).rgb, 1.0); }
)";

// Draws the displayed framebuffer (or the display memory, before the GE drew it) into the bound framebuffer at
// GL viewport (x, y, w, h), upright.
static void draw_display(int x, int y, int w, int h) {
	if (!s_dispAddr) return;
	Fb *f = fb_find(s_dispAddr);
	st_viewport(x, y, w, h);
	if (f) {
		glBindTexture(GL_TEXTURE_2D, f->color);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		draw_textured_quad(f->color, -1, 1, 1, -1, 0, 0, 480.0f / f->w, 272.0f / f->h);
	} else {
		upload_memory(s_dispAddr, s_dispStride, s_dispFmt, 480, 272);
		draw_textured_quad(s_memTex, -1, 1, 1, -1, 0, 0, 1, 1);
	}
}

extern "C" void ngpu_present(int x, int y, int w, int h) {
	if (!s_ready) return;
	views_new_frame();
	glBindFramebuffer(GL_FRAMEBUFFER, 0); st.fbo = 0;
	if (w > 0 && h > 0) draw_display(x, y, w, h);
	gl_reset_state();
	s_frame++;
}

extern "C" int ngpu_read_display(uint8_t *rgba, int w, int h) {
	if (!s_ready || !s_dispAddr || w <= 0 || h <= 0) return -1;
	GLuint fbo, tex;
	glGenFramebuffers(1, &fbo);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	st.fbo = fbo;
	draw_display(0, 0, w, h);
	glPixelStorei(GL_PACK_ALIGNMENT, 4); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	std::vector<u8> tmp((size_t)w * h * 4);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
	for (int yy = 0; yy < h; yy++) memcpy(rgba + (size_t)yy * w * 4, &tmp[(size_t)(h - 1 - yy) * w * 4], (size_t)w * 4);
	for (int i = 3; i < w * h * 4; i += 4) rgba[i] = 0xFF;
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	gl_reset_state();
	return 0;
}

// ================================================================= setup and options
extern "C" int ngpu_gl_init(void *(*get_proc)(const char *), int scale) {
	int missing = ng_gl_load(get_proc);
	if (!ng_glGenVertexArrays || !ng_glBindBufferBase || !ng_glDrawElementsBaseVertex) {
		NG_LOGE("ngpu: the OpenGL context lacks functions the native renderer needs (%d missing)", missing);
		return -1;
	}
	s_scale = std::max(1, std::min(scale, 8));
	glGenVertexArrays(1, &s_vao);
	glBindVertexArray(s_vao);
	glGenBuffers(1, &s_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
	glBufferData(GL_ARRAY_BUFFER, kVboVerts * sizeof(NgVert), nullptr, GL_STREAM_DRAW);
	glGenBuffers(1, &s_ibo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, kIboIdx * 2, nullptr, GL_STREAM_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(NgVert), (void *)offsetof(NgVert, pos));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(NgVert), (void *)offsetof(NgVert, uv));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(NgVert), (void *)offsetof(NgVert, nrm));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(NgVert), (void *)offsetof(NgVert, color));
	glBindVertexArray(0);
	glGenBuffers(1, &s_ubo);
	glBindBuffer(GL_UNIFORM_BUFFER, s_ubo);
	glBufferData(GL_UNIFORM_BUFFER, sizeof(Ubo), nullptr, GL_DYNAMIC_DRAW);
	glBindBufferBase(GL_UNIFORM_BUFFER, 0, s_ubo);
	glGenTextures(1, &s_palTex);
	glBindTexture(GL_TEXTURE_2D, s_palTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenTextures(1, &s_memTex);
	s_blitProg = link(kBlitVs, kBlitFs);
	s_blitRect = glGetUniformLocation(s_blitProg, "u_rect");
	s_blitUv = glGetUniformLocation(s_blitProg, "u_uv");
	glUseProgram(s_blitProg);
	glUniform1i(glGetUniformLocation(s_blitProg, "u_tex"), 0);
	glProvokingVertex(GL_LAST_VERTEX_CONVENTION);
	gl_reset_state();
	s_ready = true;
	NG_LOGI("GE: native OpenGL renderer (%s), internal resolution x%d", (const char *)glGetString(GL_RENDERER), s_scale);
	return 0;
}

extern "C" void ngpu_set_internal_scale(int scale) {
	scale = std::max(1, std::min(scale, 8));
	if (scale == s_scale || !s_ready) { s_scale = scale; return; }
	// framebuffers keep their contents (rescaled) at the new size
	for (Fb *f : s_fbs) {
		Fb old = *f;
		int oldScale = s_scale;
		s_scale = scale;
		fb_create_gl(f);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, old.fbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f->fbo);
		glDisable(GL_SCISSOR_TEST);
		glBlitFramebuffer(0, 0, old.w * oldScale, old.h * oldScale, 0, 0, f->w * scale, f->h * scale, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		fb_delete_gl(&old);
		s_scale = oldScale;
	}
	s_scale = scale;
	gl_reset_state();
}

extern "C" void ngpu_set_options(int filter, int aniso) { s_filter = filter; s_aniso = aniso; }
extern "C" void ngpu_set_view_offset(float x, float y) { s_viewOff[0] = x; s_viewOff[1] = y; }
extern "C" void ngpu_set_display(uint32_t addr, int stride, int fmt) {
	s_dispAddr = 0x04000000 | (addr & 0x1FFFFF);
	s_dispStride = stride > 0 ? stride : 512;
	s_dispFmt = fmt & 3;
}
extern "C" int ngpu_views_available(void) { return 1; }
extern "C" void ngpu_views_layout(int n, const int *dst, const int *src) {
	s_viewN = std::max(0, std::min(n, MAX_VIEWS));
	memcpy(s_viewDst, dst, sizeof(int) * 4 * s_viewN);
	memcpy(s_viewSrc, src, sizeof(int) * 4 * s_viewN);
}
extern "C" void ngpu_memory_written(uint32_t addr, uint32_t size) {
	if ((addr & 0x0F000000) == 0x04000000) memory_written(addr, size);
}
extern "C" void ngpu_tex_cache_clear(void) {
	if (s_ready)
		for (auto &e : s_texCache) glDeleteTextures(1, &e.second.tex);
	s_texCache.clear();
	s_pngs.clear();
}

// ================================================================= shadow mode comparisons
// SR2_NGPU_COMPARE=N: every Nth frame, the native renderer's image and PPSSPP's side by side (native | PPSSPP |
// difference x4) in SR2_NGPU_COMPARE_DIR (default build/ngcmp), and their difference in the log.
#include <sys/stat.h>
extern "C" int ge_read_display(uint8_t *rgba, int w, int h);

extern "C" void ngpu_shadow_present(void) {
	static int every = -1;
	static const char *dir;
	static u32 n;
	if (every < 0) {
		const char *e = getenv("SR2_NGPU_COMPARE");
		every = e ? atoi(e) : 0;
		dir = getenv("SR2_NGPU_COMPARE_DIR");
		if (!dir) dir = "build/ngcmp";
		if (every) mkdir(dir, 0755);
	}
	n++;
	if (every <= 0 || n % every) {
		views_new_frame();
		s_frame++;
		return;
	}
	const int w = 480 * s_scale, h = 272 * s_scale;
	std::vector<u8> a((size_t)w * h * 4), b((size_t)w * h * 4), out((size_t)w * 3 * h * 4);
	int ra = ngpu_read_display(a.data(), w, h);
	int rb = ge_read_display(b.data(), w, h);
	views_new_frame();
	s_frame++;
	if (ra || rb) return;
	double sum = 0;
	int big = 0, mx = 0;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			size_t i = ((size_t)y * w + x) * 4, o = ((size_t)y * w * 3 + x) * 4;
			int dm = 0;
			for (int c = 0; c < 4; c++) {
				int d = c < 3 ? abs(a[i + c] - b[i + c]) : 0;
				out[o + c] = a[i + c];
				out[o + (size_t)w * 4 + c] = b[i + c];
				out[o + (size_t)w * 8 + c] = c < 3 ? (u8)std::min(255, d * 4) : 255;
				sum += d;
				dm = std::max(dm, d);
			}
			mx = std::max(mx, dm);
			if (dm > 24) big++;
		}
	NG_LOGI("ngpu compare frame %u: mean abs diff %.2f/255, max %d, pixels off by >24: %.2f%%", n, sum / ((double)w * h * 3), mx,
	        100.0 * big / ((double)w * h));
	char path[512];
	snprintf(path, sizeof path, "%s/cmp_%06u.png", dir, n);
	png_image img;
	memset(&img, 0, sizeof img);
	img.version = PNG_IMAGE_VERSION;
	img.width = w * 3;
	img.height = h;
	img.format = PNG_FORMAT_RGBA;
	png_image_write_to_file(&img, path, 0, out.data(), 0, nullptr);
}
