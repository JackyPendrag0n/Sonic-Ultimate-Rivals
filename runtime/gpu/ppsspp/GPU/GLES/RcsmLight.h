// rcsm: enhanced lighting for the OpenGL renderer (per-pixel relighting with a sun, specular highlights,
// texture-derived surface detail, and real-time shadows from a sun shadow map).
//
// The game lights almost everything with the PSP's per-vertex lighting and one main directional light per
// level (light 0). Draws with normals get two extra varyings (world position and normal) and a fragment
// stage that re-shades the vertex-lit colour per pixel against that light ("the sun"), adds a Blinn-Phong
// highlight and samples the shadow map. The shadow map is drawn at the end of each frame from that frame's
// opaque 3D draws (their vertex buffers are still in the frame's push buffers) and used by the next frame.
//
// Part of the rcsm GPU extraction; GPL 2.0 or later like the PPSSPP sources it extends.
#pragma once

#include <cstdint>

class GLRenderManager;
class GLRInputLayout;
class GLRBuffer;
class GLRFramebuffer;

struct RcsmLightState {
	// settings (ge_gl_set_lighting)
	bool enhanced = true;
	bool shadows = true;
	bool specular = true;
	int quality = 1;            // 0 low, 1 medium, 2 high
	float intensity = 1.0f;
	// this frame (main pass)
	float sun[3] = {-0.07f, 0.72f, 0.68f};   // world-space direction towards the light
	float sunColor[3] = {1.0f, 0.96f, 0.88f};
	float cam[3] = {0, 0, 0};
	float lightVP[16] = {};     // column-major: world -> shadow map clip space (the map the last frame drew)
	float shadowTexel = 1.0f / 2048;
	bool shadowValid = false;
	bool haveSun = false;       // the scene has a directional main light (else: original lighting)
	int noSunFrames = 0;
	GLRFramebuffer *shadowFB = nullptr;
	uint32_t serial = 1;        // bumped every frame: programs re-upload the uniforms once per frame
};
extern RcsmLightState g_rcsmLight;

// true if this draw (current gstate) should use the enhanced shaders, given the VS has normals and HW transform
bool RcsmLight_FragmentEligible();
// a HW-transformed draw was issued (DrawEngineGLES): note the scene light/camera and maybe record it for shadows
void RcsmLight_OnDraw(GLRenderManager *render, GLRInputLayout *layout, GLRBuffer *vb, uint32_t voff, GLRBuffer *ib,
                      uint32_t ioff, uint32_t glPrim, int count, int maxIndex, bool indexed);
// bind the shadow map for an enhanced draw
void RcsmLight_BindShadowMap(GLRenderManager *render);
// end of the host frame: draw the shadow map for the next frame
void RcsmLight_EndFrame(GLRenderManager *render);
// settings changed: shaders must be re-chosen
bool RcsmLight_TakeDirty();
