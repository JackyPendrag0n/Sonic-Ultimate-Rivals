// rcsm GPU backends: backend selection, PPSSPP's OpenGL hardware renderer (GPU_GLES on thin3d's GL
// render manager) driven single-threaded from the runtime's main thread, and the presenter that puts
// the PSP display on the frontend's default framebuffer for either backend.
//
// Frame model (OpenGL backend): every GE command between two ge_present() calls is recorded into the
// render manager's queue. ge_present() ends the host frame - PPSSPP's display copy (the displayed
// framebuffer, or RAM when the display points at memory the GE didn't draw) into an offscreen
// "backbuffer" at the internal resolution - then runs the whole queue on this thread
// (GLRenderManager::ThreadFrame), blits the result to the requested rectangle of framebuffer 0, and
// begins the next host frame. Readbacks the emulation needs mid-frame (block transfers out of
// framebuffers, etc.) run the queue on the spot (GLRenderManager::FlushSync, patched for this).
//
// This file is part of the rcsm GPU extraction and, like the PPSSPP sources it links with, is
// licensed under the GPL 2.0 or later (see LICENSE.TXT).

#include <algorithm>
#include <cstring>
#include <vector>

#include "Common/GPU/thin3d.h"
#include "Common/GPU/OpenGL/GLCommon.h"
#include "Common/GPU/OpenGL/GLFeatures.h"
#include "Common/GPU/OpenGL/GLRenderManager.h"
#include "Common/System/Display.h"
#include "Common/Log.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "Core/MemMap.h"
#include "GPU/GPUCommon.h"
#include "GPU/GPU.h"
#include "GPU/GPUState.h"
#include "GPU/GLES/GPU_GLES.h"
#include "GPU/GLES/RcsmLight.h"

#include "stubs/kernel_bridge.h"  // last: short macros
#include "ge_soft.h"
#include "ge_internal.h"

namespace Draw {
DrawContext *T3DCreateGLContext(bool canChangeSwapInterval);
}
extern GLuint g_defaultFBO;  // Common/GPU/OpenGL/GLQueueRunner.cpp: what "the backbuffer" binds
int rcsm_gl_load(void *(*get_proc)(const char *));

static int g_backend = GE_BACKEND_SOFTWARE;
static bool g_backendLocked;  // the GPU exists: the choice is made

static bool g_glLoaded;
static Draw::DrawContext *g_draw;
static GLRenderManager *g_renderManager;
static int g_scale = 1;
static int g_pendingScale;          // ge_gl_set_internal_scale() before the next frame starts
static bool g_pendingConfigChange;  // ge_gl_set_options()
static bool g_frameOpen;            // inside BeginFrame/EndFrame

// The image the presenter blits from: PPSSPP's presentation output (OpenGL) or the display buffer
// converted from VRAM (software), plus its size. Valid after the first ge_present().
static GLuint g_outFbo, g_outTex;
static int g_outW, g_outH;
static bool g_outValid;

int ge_backend(void) { return g_backend; }

extern "C" void ge_select_backend(int backend) {
	if (g_backendLocked) {
		LOGE("GE: ge_select_backend(%d) after the GPU was created - ignored", backend);
		return;
	}
	g_backend = backend == GE_BACKEND_OPENGL ? GE_BACKEND_OPENGL : GE_BACKEND_SOFTWARE;
	// The settings PPSSPP's hardware backends use (the software renderer ignores most of them).
	g_Config.bSoftwareRendering = g_backend == GE_BACKEND_SOFTWARE;
	g_Config.bHardwareTransform = g_backend == GE_BACKEND_OPENGL;
	PSP_CoreParameter().gpuCore = g_backend == GE_BACKEND_OPENGL ? GPUCORE_GLES : GPUCORE_SOFTWARE;
}

// ================================================================= GL objects for the presenter
static void EnsureOutTarget(int w, int h) {
	if (g_outFbo && g_outW == w && g_outH == h)
		return;
	GLuint fbo = 0, tex = 0;
	glGenFramebuffers(1, &fbo);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		LOGE("GE: presentation framebuffer %dx%d is incomplete", w, h);
	if (g_outFbo) {
		// Keep showing the last image (scaled) until the next frame is presented.
		if (g_outValid) {
			glDisable(GL_SCISSOR_TEST);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, g_outFbo);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
			glBlitFramebuffer(0, 0, g_outW, g_outH, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDeleteFramebuffers(1, &g_outFbo);
		glDeleteTextures(1, &g_outTex);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	g_outFbo = fbo;
	g_outTex = tex;
	g_outW = w;
	g_outH = h;
}

// What imgui_impl_opengl3 and the frontend can rely on after ge_present().
static void ResetGLState() {
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindVertexArray(0);
	glUseProgram(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	for (int i = 7; i >= 0; i--) {
		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(GL_TEXTURE_2D, 0);
		if (glBindSampler)
			glBindSampler(i, 0);
	}
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DITHER);
	glDisable(GL_COLOR_LOGIC_OP);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_TRUE);
	glStencilMask(0xFF);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ONE, GL_ZERO);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
}

static void BlitOutTo(GLuint dstFbo, int x, int y, int w, int h, bool linear) {
	glDisable(GL_SCISSOR_TEST);  // the scissor test applies to blits
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, g_outFbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
	glBlitFramebuffer(0, 0, g_outW, g_outH, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, linear ? GL_LINEAR : GL_NEAREST);
}

// ================================================================= OpenGL backend
static void ApplyRenderSize() {
	const int w = 480 * g_scale, h = 272 * g_scale;
	g_Config.iInternalResolution = g_scale;
	// Presentation draws 1:1 into an offscreen "backbuffer" at the render size; the blit to the
	// window scales.
	PSP_CoreParameter().pixelWidth = w;
	PSP_CoreParameter().pixelHeight = h;
	g_display.Recalculate(w, h, 1.0f, 1.0f, 1.0f);
	EnsureOutTarget(w, h);
	g_defaultFBO = g_outFbo;
	g_draw->SetTargetSize(w, h);
}

static void BeginHostFrame() {
	if (g_frameOpen || !gpu)
		return;
	const DisplayLayoutConfig &config = g_Config.GetDisplayLayoutConfig(DeviceOrientation::Landscape);
	gpu->BeginHostFrame(config);
	g_draw->BeginFrame(Draw::DebugFlags::NONE);
	if (RcsmLight_TakeDirty())   // enhanced lighting switched: re-choose shaders
		gstate_c.Dirty(DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_ALL_UNIFORMS);
	g_frameOpen = true;
}

GPUCommon *ge_gl_create_gpu(void) {
	if (!g_draw)
		return nullptr;
	g_backendLocked = true;
	gpu = new GPU_GLES(nullptr, g_draw);
	BeginHostFrame();
	return gpu;
}

extern "C" void ge_gl_init(void *(*get_proc)(const char *), int internal_scale) {
	if (!g_glLoaded) {
		int missing = rcsm_gl_load(get_proc);
		if (!rcsm_glGetString || !glGetString(GL_VERSION)) {
			LOGE("GE: no current OpenGL context in ge_gl_init()");
			return;
		}
		g_glLoaded = true;
		LOGI("GE: OpenGL %s (%s, %s)%s", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER),
			(const char *)glGetString(GL_VENDOR), missing ? ", some extension functions missing" : "");
	}
	g_scale = std::max(1, std::min(internal_scale, 8));
	if (g_backend != GE_BACKEND_OPENGL || g_draw)
		return;  // the software backend only needs the presenter

	ge_common_init();
	CheckGLExtensions();
	g_draw = Draw::T3DCreateGLContext(false);
	g_renderManager = (GLRenderManager *)g_draw->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);
	if (!g_draw->CreatePresets())
		LOGE("GE: thin3d presets failed to compile");
	// No render thread: its setup runs here, and so does its queue (see ge_present).
	g_renderManager->ThreadStart(g_draw);
	ApplyRenderSize();
	if (ge_hle_ready() && !gpu)
		ge_gl_create_gpu();
}

extern "C" void ge_gl_set_internal_scale(int scale) {
	scale = std::max(1, std::min(scale, 8));
	if (g_backend != GE_BACKEND_OPENGL || !g_draw) {
		g_scale = scale;
		return;
	}
	if (scale != g_scale)
		g_pendingScale = scale;  // applied between frames, in ge_present()
}

extern "C" void ge_gl_set_options(int texture_filtering, int anisotropy, int msaa) {
	// PPSSPP: 1 = auto, 2 = nearest, 3 = linear.
	g_Config.iTexFiltering = texture_filtering == 1 ? TEX_FILTER_FORCE_NEAREST : texture_filtering == 2 ? TEX_FILTER_FORCE_LINEAR : TEX_FILTER_AUTO;
	// PPSSPP stores the anisotropy level as log2(samples).
	int level = 0;
	while (level < 4 && (2 << level) <= anisotropy)
		level++;
	g_Config.iAnisotropyLevel = level;
	// MSAA: PPSSPP's OpenGL backend renders without it (only Vulkan supports it); ignored.
	(void)msaa;
	g_pendingConfigChange = true;
}

static void PresentGL() {
	if (!gpu || !g_frameOpen)
		return;
	const DisplayLayoutConfig &config = g_Config.GetDisplayLayoutConfig(DeviceOrientation::Landscape);
	// What PPSSPP's sceDisplay does at a flip: the display is "really dirty" if the game drew into
	// it since it was last shown (else the last shown buffer is presented again).
	const bool fbDirty = gpu->FramebufferDirty();
	const bool fbReallyDirty = gpu->FramebufferReallyDirty();
	gpu->SetCurFramebufferDirty(fbReallyDirty);
	if (fbDirty)
		gpuStats.totals.numFlips++;
	// Then the order PPSSPP's EmuScreen uses: post-processing passes, end of the emulated frame, the
	// UI binds (and clears) the backbuffer and the display is copied into it.
	gpu->PrepareCopyDisplayToOutput(config);
	gpu->EndHostFrame();
	// rcsm enhanced lighting: the sun shadow map for the next frame, from this frame's opaque 3D draws
	RcsmLight_EndFrame(g_renderManager);
	g_draw->BindFramebufferAsRenderTarget(nullptr, {Draw::RPAction::CLEAR, Draw::RPAction::CLEAR, Draw::RPAction::CLEAR}, "rcsm_backbuffer");
	Draw::Viewport viewport{0.0f, 0.0f, (float)g_outW, (float)g_outH, 0.0f, 1.0f};
	g_draw->SetViewport(viewport);
	gpu->CopyDisplayToOutput(config);
	g_draw->EndFrame();
	g_draw->Present(Draw::PresentMode::FIFO);
	g_frameOpen = false;
	// Run everything recorded this frame, ending with the present task.
	const u64 tRun = now_us();
	g_renderManager->ThreadFrame();
	g_outValid = true;
	// RCSM_GE_STATS=n: every n frames, log how the frame time splits between the emulation
	// (guest code, GE command processing, recording) and executing the GL work.
	static int statsEvery = getenv("RCSM_GE_STATS") ? atoi(getenv("RCSM_GE_STATS")) : 0;
	if (statsEvery > 0) {
		static u64 lastEnd, sumGL, sumAll;
		static int frames;
		const u64 now = now_us();
		if (lastEnd) {
			sumGL += now - tRun;
			sumAll += now - lastEnd;
			if (++frames == statsEvery) {
				LOGI("GE stats: %.2f ms/frame, of which GL execution %.2f ms (x%d)", sumAll / 1000.0 / frames, sumGL / 1000.0 / frames, g_scale);
				frames = 0;
				sumGL = sumAll = 0;
			}
		}
		lastEnd = now;
	}

	// Between frames: settings changes.
	if (g_pendingScale) {
		g_scale = g_pendingScale;
		g_pendingScale = 0;
		ApplyRenderSize();
		gpu->NotifyDisplayResized();
		gpu->NotifyRenderResized(config);
	}
	if (g_pendingConfigChange) {
		g_pendingConfigChange = false;
		gpu->NotifyConfigChanged();
	}
}

// ================================================================= software backend presenter
static std::vector<u32> g_softPixels;

static void PresentSoftware() {
	u32 top, stride;
	int fmt;
	if (!ge_soft_display(&top, &stride, &fmt)) {
		g_outValid = false;
		return;
	}
	if (gpu)
		gpu->FlushPendingDrawing();
	EnsureOutTarget(480, 272);
	g_softPixels.resize(480 * 272);
	for (int y = 0; y < 272; y++) {
		u32 *dst = &g_softPixels[(271 - y) * 480];  // GL rows go bottom-up
		if (fmt == GE_FORMAT_8888) {
			const u32 *src = (const u32 *)Memory::GetPointerUnchecked(top + y * stride * 4);
			for (int x = 0; x < 480; x++)
				dst[x] = src[x] | 0xFF000000u;
		} else {
			const u16 *src = (const u16 *)Memory::GetPointerUnchecked(top + y * stride * 2);
			for (int x = 0; x < 480; x++) {
				u16 p = src[x];
				u32 r, g, b;
				if (fmt == GE_FORMAT_565) {
					r = (p & 31) << 3; g = ((p >> 5) & 63) << 2; b = (p >> 11) << 3;
					r |= r >> 5; g |= g >> 6; b |= b >> 5;
				} else if (fmt == GE_FORMAT_5551) {
					r = (p & 31) << 3; g = ((p >> 5) & 31) << 3; b = ((p >> 10) & 31) << 3;
					r |= r >> 5; g |= g >> 5; b |= b >> 5;
				} else {
					r = (p & 15) * 17; g = ((p >> 4) & 15) * 17; b = ((p >> 8) & 15) * 17;
				}
				dst[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
			}
		}
	}
	glBindTexture(GL_TEXTURE_2D, g_outTex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 480, 272, GL_RGBA, GL_UNSIGNED_BYTE, g_softPixels.data());
	glBindTexture(GL_TEXTURE_2D, 0);
	g_outValid = true;
}

// ================================================================= frontend API
extern "C" void ge_present(int fb_x, int fb_y, int fb_w, int fb_h) {
	if (!g_glLoaded)
		return;
	if (g_backend == GE_BACKEND_OPENGL)
		PresentGL();
	else
		PresentSoftware();
	if (g_outValid && fb_w > 0 && fb_h > 0)
		BlitOutTo(0, fb_x, fb_y, fb_w, fb_h, g_Config.GetDisplayLayoutConfig(DeviceOrientation::Landscape).iDisplayFilter == SCALE_LINEAR);
	ResetGLState();
	if (g_renderManager)
		g_renderManager->ForgetBoundFramebuffers();
	if (g_backend == GE_BACKEND_OPENGL)
		BeginHostFrame();
}

extern "C" int ge_read_display(uint8_t *rgba, int w, int h) {
	if (!g_glLoaded || !g_outValid || w <= 0 || h <= 0)
		return -1;
	GLuint fbo = 0, tex = 0;
	glGenFramebuffers(1, &fbo);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	BlitOutTo(fbo, 0, 0, w, h, w != g_outW || h != g_outH);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	std::vector<uint8_t> tmp((size_t)w * h * 4);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
	for (int y = 0; y < h; y++)
		memcpy(rgba + (size_t)y * w * 4, &tmp[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
	for (int i = 3; i < w * h * 4; i += 4)
		rgba[i] = 0xFF;
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	ResetGLState();
	if (g_renderManager)
		g_renderManager->ForgetBoundFramebuffers();
	return 0;
}

// sr2recomp frame ripper (rip.cpp): called at every display flip
void rip_flip(TextureCacheCommon *tc);
extern "C" void ge_rip_flip(void) {
	if (gpu) rip_flip(gpu->GetTextureCacheCommon());
}
