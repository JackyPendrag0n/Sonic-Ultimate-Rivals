// rcsm: enhanced lighting for the OpenGL renderer. See RcsmLight.h.
// Part of the rcsm GPU extraction; GPL 2.0 or later like the PPSSPP sources it extends.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Common/GPU/OpenGL/GLRenderManager.h"
#include "GPU/GPUState.h"
#include "GPU/Math3D.h"
#include "GPU/Common/ShaderCommon.h"
#include "GPU/GLES/RcsmLight.h"

RcsmLightState g_rcsmLight;

namespace {

struct ShadowDraw {
	GLRInputLayout *layout;
	GLRBuffer *vb, *ib;
	uint32_t voff, ioff;
	uint32_t prim;
	int count, maxIndex;
	bool indexed;
	float world[16];
};

std::vector<ShadowDraw> g_draws;
struct SunVote { float d[3]; int n; };
SunVote g_votes[8];
int g_nvotes;
bool g_haveView;
float g_view[12];
bool g_dirty = true;
GLRProgram *g_prog;
GLint g_locWorld = -1, g_locVP = -1;
int g_fbSize;
int g_consumerStep = -1, g_consumerFlush;

const char *kShadowVS =
	"#version 330\n"
	"in vec3 position;\n"
	"uniform mat4 u_world;\n"
	"uniform mat4 u_lvp;\n"
	"void main() {\n"
	"  vec3 wp = (vec4(position, 1.0) * u_world).xyz;\n"
	"  gl_Position = u_lvp * vec4(wp, 1.0);\n"
	"}\n";
const char *kShadowFS =
	"#version 330\n"
	"out vec4 fragColor0;\n"
	"void main() { fragColor0 = vec4(1.0); }\n";

void normalize3(float *v) {
	float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}
float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void cross3(float *o, const float *a, const float *b) {
	o[0] = a[1] * b[2] - a[2] * b[1];
	o[1] = a[2] * b[0] - a[0] * b[2];
	o[2] = a[0] * b[1] - a[1] * b[0];
}

}  // namespace

bool RcsmLight_TakeDirty() {
	bool d = g_dirty;
	g_dirty = false;
	return d;
}

extern "C" void ge_gl_set_lighting(int mode, int specular, int shadows, int quality, float intensity) {
	bool en = mode != 0;
	if (en != g_rcsmLight.enhanced) g_dirty = true;
	g_rcsmLight.enhanced = en;
	g_rcsmLight.specular = specular != 0;
	g_rcsmLight.shadows = shadows != 0;
	g_rcsmLight.quality = quality < 0 ? 0 : quality > 2 ? 2 : quality;
	g_rcsmLight.intensity = intensity < 0 ? 0 : intensity > 2 ? 2 : intensity;
}

// sky domes and other backdrops follow the camera: their world origin is (near) the camera position
static bool FollowsCamera() {
	const float *m = gstate.viewMatrix, *w = gstate.worldMatrix;
	float d2 = 0;
	for (int j = 0; j < 3; j++) {
		float c = -(m[9] * m[j * 3 + 0] + m[10] * m[j * 3 + 1] + m[11] * m[j * 3 + 2]);
		d2 += (w[9 + j] - c) * (w[9 + j] - c);
	}
	return d2 < 4.0f;
}

bool RcsmLight_FragmentEligible() {
	if (!g_rcsmLight.enhanced || gstate.isModeThrough() || gstate.isModeClear())
		return false;
	if (FollowsCamera())
		return false;
	if (!gstate.isDepthWriteEnabled())   // the sky and other backdrops
		return false;
	// the lit world and characters (unlit geometry is mostly sky, clouds, glows and backdrops)
	if (!gstate.isLightingEnabled() && !(gstate.vertType & GE_VTYPE_WEIGHT_MASK))
		return false;
	if (!gstate.isAlphaBlendEnabled())
		return gstate.isDepthWriteEnabled();   // (not the sky and other backdrops)
	// lit (standard) alpha blending yes; additive / other glows no
	return gstate.getBlendFuncA() == GE_SRCBLEND_SRCALPHA && gstate.getBlendFuncB() == GE_DSTBLEND_INVSRCALPHA &&
		gstate.getBlendEq() == GE_BLENDMODE_MUL_AND_ADD;
}

void RcsmLight_OnDraw(GLRenderManager *render, GLRInputLayout *layout, GLRBuffer *vb, uint32_t voff, GLRBuffer *ib,
                      uint32_t ioff, uint32_t glPrim, int count, int maxIndex, bool indexed) {
	(void)render;
	if (!g_rcsmLight.enhanced || gstate.isModeThrough() || gstate.isModeClear() || count <= 0)
		return;
	const bool skinned = (gstate.vertType & GE_VTYPE_WEIGHT_MASK) != 0;
	// the scene's sun: light 0 of the lit static geometry
	if (!skinned && gstate.isLightingEnabled() && gstate.isLightChanEnabled(0) && gstate.isDirectionalLight(0)) {
		float d[3] = {getFloat24(gstate.lpos[0]), getFloat24(gstate.lpos[1]), getFloat24(gstate.lpos[2])};
		normalize3(d);
		int i;
		for (i = 0; i < g_nvotes; i++)
			if (dot3(g_votes[i].d, d) > 0.995f) { g_votes[i].n += count; break; }
		if (i == g_nvotes && g_nvotes < 8) { memcpy(g_votes[g_nvotes].d, d, sizeof d); g_votes[g_nvotes++].n = count; }
		if (!g_haveView) { memcpy(g_view, gstate.viewMatrix, sizeof g_view); g_haveView = true; }
	}
	// shadow casters: opaque depth-writing 3D geometry
	if (!g_rcsmLight.shadows || g_draws.size() >= 8000)
		return;
	{
		static int dbg = getenv("RCSM_LIGHT_LOG") ? 1 : 0;
		static int c[5], n;
		if (dbg) {
			c[0] += gstate.isAlphaBlendEnabled(); c[1] += gstate.isAlphaTestEnabled(); c[2] += !gstate.isDepthTestEnabled(); c[3] += !gstate.isDepthWriteEnabled(); c[4]++;
			if (++n % 20000 == 0) fprintf(stderr, "rcsm light: draws %d blend %d atest %d noztest %d nozwrite %d\n", c[4], c[0], c[1], c[2], c[3]);
		}
	}
	// (the game leaves alpha blending and the alpha test on for nearly everything: standard blending is fine;
	// cut-out textures would need their alpha in the shadow pass, so those don't cast)
	if (!gstate.isDepthTestEnabled() || !gstate.isDepthWriteEnabled())
		return;
	if (gstate.isAlphaBlendEnabled() && !(gstate.getBlendFuncA() == GE_SRCBLEND_SRCALPHA && gstate.getBlendFuncB() == GE_DSTBLEND_INVSRCALPHA))
		return;
	if ((gstate.isAlphaTestEnabled() || gstate.isAlphaBlendEnabled()) && gstate.isTextureMapEnabled() && !gstate_c.textureSolidAlpha)
		return;
	ShadowDraw s;
	s.layout = layout; s.vb = vb; s.voff = voff; s.ib = ib; s.ioff = ioff; s.prim = glPrim;
	s.count = count; s.maxIndex = maxIndex; s.indexed = indexed;
	ConvertMatrix4x3To4x4Transposed(s.world, gstate.worldMatrix);
	g_draws.push_back(s);
}

void RcsmLight_BindShadowMap(GLRenderManager *render) {
	if (g_rcsmLight.shadowValid && g_rcsmLight.shadowFB) {
		if (g_consumerStep < 0) {   // the frame's first reader: the shadow pass goes in front of its step
			g_consumerStep = render->GetNumSteps() - 1;
			g_consumerFlush = render->rcsmFlushCount;
		}
		render->BindFramebufferAsTexture(g_rcsmLight.shadowFB, 5, GL_DEPTH_BUFFER_BIT);
	}
}

void RcsmLight_EndFrame(GLRenderManager *render) {
	RcsmLightState &L = g_rcsmLight;
	L.serial++;
	// the sun: the direction most of the lit scene used this frame (eased, so it doesn't pop)
	int best = -1;
	for (int i = 0; i < g_nvotes; i++) if (best < 0 || g_votes[i].n > g_votes[best].n) best = i;
	// Hysteresis: a different direction has to win for a second before the sun moves there, so scenes whose
	// objects use two different main lights don't flip the shadows back and forth.
	static float pending[3];
	static int pendingFrames;
	const bool hadSun = L.haveSun;
	if (best >= 0) { L.haveSun = true; L.noSunFrames = 0; }
	else if (++L.noSunFrames > 30) L.haveSun = false;
	if (best >= 0) {
		const float *d = g_votes[best].d;
		if (!hadSun) { memcpy(L.sun, d, sizeof L.sun); pendingFrames = 0; }
		else if (dot3(d, L.sun) >= 0.995f) {
			for (int k = 0; k < 3; k++) L.sun[k] = L.sun[k] * 0.9f + d[k] * 0.1f;
			normalize3(L.sun);
			pendingFrames = 0;
		} else {
			if (pendingFrames && dot3(d, pending) >= 0.995f) pendingFrames++;
			else { memcpy(pending, d, sizeof pending); pendingFrames = 1; }
			if (pendingFrames > 60) { memcpy(L.sun, pending, sizeof L.sun); pendingFrames = 0; }
		}
	}
	g_nvotes = 0;
	// the camera (world position and forward) from the scene's view matrix (4x3, row vectors: v = p R + t)
	float fwd[3] = {0, 0, -1};
	if (g_haveView) {
		const float *m = g_view;
		for (int j = 0; j < 3; j++) {
			L.cam[j] = -(m[9] * m[j * 3 + 0] + m[10] * m[j * 3 + 1] + m[11] * m[j * 3 + 2]);
			fwd[j] = -m[j * 3 + 2];
		}
		normalize3(fwd);
	}
	g_haveView = false;
	{
		static int dbg = getenv("RCSM_LIGHT_LOG") ? 1 : 0;
		static int n;
		if (dbg && (n++ % 60) == 0)
			fprintf(stderr, "rcsm light: %d casters, sun %.2f %.2f %.2f cam %.1f %.1f %.1f fwd %.2f %.2f %.2f shadows %d\n", (int)g_draws.size(),
			        L.sun[0], L.sun[1], L.sun[2], L.cam[0], L.cam[1], L.cam[2], fwd[0], fwd[1], fwd[2], (int)L.shadows);
	}

	static const int sizes[3] = {1024, 2048, 3072};
	static const float radii[3] = {16.0f, 24.0f, 32.0f};

	// 1. This frame's shadow map, with the matrix this frame's shaders were given (L.lightVP), from this
	//    frame's casters. The step is moved in front of the frame's first draw that reads the map, so
	//    shadows don't trail behind moving things.
	if (L.shadowValid && L.shadowFB && g_prog) {
		const int size = g_fbSize;
		render->BindFramebufferAsRenderTarget(L.shadowFB, GLRRenderPassAction::CLEAR, GLRRenderPassAction::CLEAR,
		                                      GLRRenderPassAction::CLEAR, 0, 1.0f, 0, "rcsm_shadow");
		render->SetViewport(GLRViewport{0.0f, 0.0f, (float)size, (float)size, 0.0f, 1.0f});
		render->SetScissor(GLRect2D{0, 0, size, size});
		render->SetDepth(true, true, GL_LESS);
		render->SetNoBlendAndMask(0);
		render->SetStencilDisabled();
		render->SetRaster(GL_FALSE, GL_CCW, GL_BACK, GL_FALSE, GL_FALSE);
		render->BindProgram(g_prog);
		render->SetUniformM4x4(&g_locVP, L.lightVP);
		const float *lastWorld = nullptr;
		for (const ShadowDraw &s : g_draws) {
			if (!lastWorld || memcmp(lastWorld, s.world, sizeof s.world) != 0) {
				render->SetUniformM4x4(&g_locWorld, s.world);
				lastWorld = s.world;
			}
			if (s.indexed)
				render->DrawIndexed(s.layout, s.vb, s.voff, s.ib, s.ioff, s.prim, s.count, GL_UNSIGNED_SHORT, 1, s.maxIndex);
			else
				render->Draw(s.layout, s.vb, s.voff, s.prim, 0, s.count);
		}
		// (if a mid-frame flush already ran the first readers, they used the previous frame's map)
		if (g_consumerStep >= 0 && g_consumerFlush == render->rcsmFlushCount)
			render->RcsmMoveLastStep(g_consumerStep);
	}
	g_draws.clear();
	g_consumerStep = -1;

	// 2. The next frame's light box: an orthographic box around the area in front of the camera, looking
	//    along -sun, snapped to whole shadow-map texels so the shadows don't shimmer as the camera moves.
	if (!L.enhanced || !L.shadows || !L.haveSun) {
		L.shadowValid = false;
		return;
	}
	const int size = sizes[L.quality];
	const float R = radii[L.quality];
	if (!L.shadowFB || g_fbSize != size) {
		if (L.shadowFB) render->DeleteFramebuffer(L.shadowFB);
		L.shadowFB = render->CreateFramebuffer(size, size, true, "rcsm_shadow");
		g_fbSize = size;
	}
	if (!g_prog) {
		std::vector<GLRShader *> shaders;
		shaders.push_back(render->CreateShader(GL_VERTEX_SHADER, kShadowVS, "rcsm_shadow_vs"));
		shaders.push_back(render->CreateShader(GL_FRAGMENT_SHADER, kShadowFS, "rcsm_shadow_fs"));
		std::vector<GLRProgram::Semantic> sem = {{ATTR_POSITION, "position"}};
		std::vector<GLRProgram::UniformLocQuery> q = {{&g_locWorld, "u_world"}, {&g_locVP, "u_lvp"}};
		std::vector<GLRProgram::Initializer> init;
		g_prog = render->CreateProgram(shaders, sem, q, init, nullptr, GLRProgramFlags{});
	}
	float f[3] = {-L.sun[0], -L.sun[1], -L.sun[2]};
	float up[3] = {0, 1, 0};
	if (fabsf(f[1]) > 0.95f) { up[0] = 1; up[1] = 0; }
	float r[3], u[3];
	cross3(r, f, up); normalize3(r);
	cross3(u, r, f);
	float center[3];
	for (int k = 0; k < 3; k++) center[k] = L.cam[k] + fwd[k] * R * 0.6f;
	const float texel = 2.0f * R / size;
	float lx = dot3(center, r), ly = dot3(center, u);
	float sx = floorf(lx / texel) * texel - lx, sy = floorf(ly / texel) * texel - ly;
	for (int k = 0; k < 3; k++) center[k] += r[k] * sx + u[k] * sy;
	const float depthHalf = 120.0f;
	float eye[3];
	for (int k = 0; k < 3; k++) eye[k] = center[k] - f[k] * depthHalf;
	float *M = L.lightVP;
	memset(M, 0, sizeof L.lightVP);
	for (int k = 0; k < 3; k++) {
		M[k * 4 + 0] = r[k] / R;
		M[k * 4 + 1] = u[k] / R;
		M[k * 4 + 2] = f[k] / depthHalf;
	}
	M[12] = -dot3(eye, r) / R;
	M[13] = -dot3(eye, u) / R;
	M[14] = -dot3(eye, f) / depthHalf - 1.0f;
	M[15] = 1.0f;
	L.shadowTexel = 1.0f / size;
	L.shadowValid = true;
}
