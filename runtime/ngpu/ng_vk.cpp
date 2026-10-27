// Native GE renderer: the Vulkan side (renderer 3, or SR2_GPU=vulkan). It draws what the OpenGL side (ng_gl.cpp)
// draws, the same way: framebuffers at the internal resolution with PSP row 0 as image row 0, textures decoded on
// the CPU and cached by content, and one shader (runtime/ngpu/vk, precompiled into ng_vk_spv.h) specialized
// per draw key.
//
// The window, the settings overlay and the system dialogs stay OpenGL: the displayed framebuffer is copied into
// an image that Vulkan shares with OpenGL (GL_EXT_memory_object), and ng_gl.cpp draws that into the window.
// Where the drivers can't share images, the OpenGL renderer is used instead.
//
// Work is recorded into one of a ring of contexts, each with its command buffers, a host-visible buffer that
// vertices, indices, uniforms and texture uploads are streamed through, and its descriptor sets. A context is
// submitted at the end of every display list and every present, and is reused once the GPU is done with it.
// Every image the game draws into stays in the GENERAL layout, so it can be drawn to, sampled and copied without
// layout changes; a barrier follows each render pass and copy.
#ifdef _WIN32
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#define VK_NO_PROTOTYPES
#include "gpu/ppsspp/ext/vulkan/vulkan.h"
#include "ngpu.h"
#include "ng_internal.h"
#include "ng_draw.h"
#include "ng_tex.h"
#include "ng_common.h"
#include "ng_gl_funcs.h"
#include "ng_vk_spv.h"
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <functional>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

bool g_ng_vk;   // draws go to the Vulkan side (ng_gl.cpp forwards its entry points)

// ================================================================= loading
#define VK_GLOBAL_FUNCS(X) X(vkCreateInstance) X(vkEnumerateInstanceVersion) X(vkEnumerateInstanceExtensionProperties)
#define VK_INSTANCE_FUNCS(X) \
	X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
	X(vkGetPhysicalDeviceFeatures) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#define VK_DEVICE_FUNCS(X) \
	X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkQueueSubmit) X(vkDeviceWaitIdle) X(vkAllocateMemory) X(vkFreeMemory) \
	X(vkMapMemory) X(vkBindBufferMemory) X(vkBindImageMemory) X(vkGetBufferMemoryRequirements) \
	X(vkGetImageMemoryRequirements) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkCreateImage) X(vkDestroyImage) \
	X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkCreateShaderModule) \
	X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool) \
	X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateRenderPass) \
	X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateCommandPool) X(vkResetCommandPool) \
	X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkWaitForFences) \
	X(vkResetFences) X(vkGetFenceStatus) X(vkCreateSemaphore) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
	X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) \
	X(vkCmdDrawIndexed) X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants) \
	X(vkCmdPushConstants) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
	X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage)
#ifdef _WIN32
#define VK_EXTERNAL_FUNCS(X) X(vkGetMemoryWin32HandleKHR) X(vkGetSemaphoreWin32HandleKHR)
#else
#define VK_EXTERNAL_FUNCS(X) X(vkGetMemoryFdKHR) X(vkGetSemaphoreFdKHR)
#endif
#define VK_DECL(f) static PFN_##f f;
VK_GLOBAL_FUNCS(VK_DECL) VK_INSTANCE_FUNCS(VK_DECL) VK_DEVICE_FUNCS(VK_DECL) VK_EXTERNAL_FUNCS(VK_DECL)
#undef VK_DECL
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

// OpenGL's side of the sharing (GL_EXT_memory_object, GL_EXT_semaphore), which ng_gl_funcs.h doesn't load
#ifndef APIENTRY
#define APIENTRY
#endif
typedef const GLubyte *(APIENTRY *PFNGetStringi)(GLenum, GLuint);
typedef void (APIENTRY *PFNGetUnsignedBytei_v)(GLenum, GLuint, GLubyte *);
typedef void (APIENTRY *PFNCreateMemoryObjects)(GLsizei, GLuint *);
typedef void (APIENTRY *PFNDeleteMemoryObjects)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFNMemoryObjectParameteriv)(GLuint, GLenum, const GLint *);
typedef void (APIENTRY *PFNImportMemoryFd)(GLuint, GLuint64, GLenum, GLint);
typedef void (APIENTRY *PFNImportMemoryWin32Handle)(GLuint, GLuint64, GLenum, void *);
typedef void (APIENTRY *PFNTexStorageMem2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
typedef void (APIENTRY *PFNGenSemaphores)(GLsizei, GLuint *);
typedef void (APIENTRY *PFNImportSemaphoreFd)(GLuint, GLenum, GLint);
typedef void (APIENTRY *PFNImportSemaphoreWin32Handle)(GLuint, GLenum, void *);
typedef void (APIENTRY *PFNSemaphoreOp)(GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *);
static struct {
	PFNGetStringi GetStringi;
	PFNGetUnsignedBytei_v GetUnsignedBytei_v;
	PFNCreateMemoryObjects CreateMemoryObjects;
	PFNDeleteMemoryObjects DeleteMemoryObjects;
	PFNMemoryObjectParameteriv MemoryObjectParameteriv;
	PFNImportMemoryFd ImportMemoryFd;
	PFNImportMemoryWin32Handle ImportMemoryWin32Handle;
	PFNTexStorageMem2D TexStorageMem2D;
	PFNGenSemaphores GenSemaphores;
	PFNImportSemaphoreFd ImportSemaphoreFd;
	PFNImportSemaphoreWin32Handle ImportSemaphoreWin32Handle;
	PFNSemaphoreOp WaitSemaphore, SignalSemaphore;
} gx;
enum {
	GLX_TEXTURE_TILING = 0x9580, GLX_DEDICATED_MEMORY_OBJECT = 0x9581, GLX_OPTIMAL_TILING = 0x9584,
	GLX_HANDLE_TYPE_OPAQUE_FD = 0x9586, GLX_HANDLE_TYPE_OPAQUE_WIN32 = 0x9587, GLX_NUM_DEVICE_UUIDS = 0x9596,
	GLX_DEVICE_UUID = 0x9597, GLX_LAYOUT_SHADER_READ_ONLY = 0x9591, GLX_NUM_EXTENSIONS = 0x821D, GLX_EXTENSIONS = 0x1F03,
};

// ================================================================= device
static bool s_ready;
static int s_scale = 2, s_filter, s_aniso;
static u32 s_frame = 1, s_texEpoch = 1;
static float s_viewOff[2];
static void *s_lib;
static VkInstance s_inst;
static VkPhysicalDevice s_phys;
static VkDevice s_dev;
static VkQueue s_queue;
static u32 s_qfam;
static VkPhysicalDeviceMemoryProperties s_memProps;
static VkDeviceSize s_uboAlign = 256;
static bool s_anisoOk;
static float s_maxAniso = 1;
static VkFormat s_depthFmt;
static bool s_depthBlit;   // the depth format can be scaled with a blit
static bool s_glSems;      // OpenGL can wait for and signal Vulkan semaphores

static bool vk_ok(VkResult r, const char *what) {
	if (r == VK_SUCCESS) return true;
	NG_LOGE("Vulkan: %s failed (%d)", what, (int)r);
	return false;
}

static u32 mem_type(u32 bits, VkMemoryPropertyFlags want) {
	for (u32 i = 0; i < s_memProps.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (s_memProps.memoryTypes[i].propertyFlags & want) == want) return i;
	return ~0u;
}

static VkDeviceMemory alloc_for(const VkMemoryRequirements &req, VkMemoryPropertyFlags want, const void *next = nullptr) {
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, next, req.size, mem_type(req.memoryTypeBits, want)};
	if (ai.memoryTypeIndex == ~0u) ai.memoryTypeIndex = mem_type(req.memoryTypeBits, 0);
	VkDeviceMemory m = VK_NULL_HANDLE;
	if (vkAllocateMemory(s_dev, &ai, nullptr, &m) != VK_SUCCESS) {
		NG_LOGE("Vulkan: out of memory (%llu bytes)", (unsigned long long)req.size);
		abort();
	}
	return m;
}

struct Buf { VkBuffer buf; VkDeviceMemory mem; u8 *ptr; VkDeviceSize size; };
static Buf buf_create(VkDeviceSize size, VkBufferUsageFlags usage) {
	Buf b = {};
	b.size = size;
	VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	ci.size = size; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	vkCreateBuffer(s_dev, &ci, nullptr, &b.buf);
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(s_dev, b.buf, &req);
	b.mem = alloc_for(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	vkBindBufferMemory(s_dev, b.buf, b.mem, 0);
	vkMapMemory(s_dev, b.mem, 0, VK_WHOLE_SIZE, 0, (void **)&b.ptr);
	return b;
}

struct Img { VkImage img; VkImageView view; VkDeviceMemory mem; int w, h; };
static Img img_create(int w, int h, VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
	Img im = {};
	im.w = w; im.h = h;
	VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent = {(u32)w, (u32)h, 1};
	ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = usage; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	vk_ok(vkCreateImage(s_dev, &ci, nullptr, &im.img), "vkCreateImage");
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(s_dev, im.img, &req);
	im.mem = alloc_for(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	vkBindImageMemory(s_dev, im.img, im.mem, 0);
	if (!(usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) return im;
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = im.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
	// (a depth/stencil view is only an attachment: both aspects)
	vi.subresourceRange = {aspect, 0, 1, 0, 1};
	vkCreateImageView(s_dev, &vi, nullptr, &im.view);
	return im;
}

// ================================================================= contexts
// What a context's work may still be using when it's replaced: destroyed once the context is done
struct Garbage { VkImage img; VkImageView view; VkDeviceMemory mem; VkFramebuffer fb; VkBuffer buf; };
struct TexSet { VkImageView view; VkImageLayout layout; VkSampler sampler; VkImageView pal;
	bool operator==(const TexSet &o) const { return view == o.view && layout == o.layout && sampler == o.sampler && pal == o.pal; } };
struct TexSetHash { size_t operator()(const TexSet &t) const {
	u64 k[4] = {(u64)(uintptr_t)t.view, (u64)t.layout, (u64)(uintptr_t)t.sampler, (u64)(uintptr_t)t.pal};
	return ng_hash(k, sizeof k); } };
struct Ctx {
	VkCommandPool pool;
	VkCommandBuffer init, main;
	bool initUsed;
	VkFence fence;
	bool pending;          // submitted, not yet known to be done
	Buf stream;
	VkDeviceSize pos;
	VkDescriptorPool dpool;
	VkDescriptorSet uboSet;
	int setsLeft;
	std::unordered_map<TexSet, VkDescriptorSet, TexSetHash> sets;
	std::vector<Garbage> garbage;
	std::vector<Buf> bigBufs;   // uploads too big for the stream buffer
};
static const int kCtxs = 6, kSets = 4096;
static const VkDeviceSize kStream = 16 << 20;
static Ctx s_ctx[kCtxs];
static int s_ci;
static Ctx &C() { return s_ctx[s_ci]; }

static VkRenderPass s_rp;
static VkDescriptorSetLayout s_dslUbo, s_dslTex;
static VkPipelineLayout s_layout;
static VkShaderModule s_vs, s_fs, s_blitVs, s_blitFs;
static VkSampler s_nearestClamp, s_linearClamp;
static Img s_dummy;   // 1x1, for draws without a texture

// bound state of the current command buffer
static struct {
	bool rp;
	VkFramebuffer fb;
	VkPipeline pipe;
	VkDescriptorSet tex;
	int64_t uboOff;
	int sc[4];
	u32 bcolor;
	bool bcolorSet;
} bs;

static void bs_reset() { bs = {}; bs.uboOff = -1; bs.sc[0] = -1; }

static void barrier(VkCommandBuffer cb) {
	VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                        u32 srcQ = VK_QUEUE_FAMILY_IGNORED, u32 dstQ = VK_QUEUE_FAMILY_IGNORED) {
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	b.oldLayout = from; b.newLayout = to;
	b.srcQueueFamilyIndex = srcQ; b.dstQueueFamilyIndex = dstQ;
	b.image = img;
	b.subresourceRange = {aspect, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static void end_rp() {
	if (!bs.rp) return;
	vkCmdEndRenderPass(C().main);
	barrier(C().main);
	bs.rp = false;
	bs.fb = VK_NULL_HANDLE;
}

static void throw_away(const Img &im, VkFramebuffer fb = VK_NULL_HANDLE) {
	C().garbage.push_back({im.img, im.view, im.mem, fb, VK_NULL_HANDLE});
}

static void wb_ctx_done(int ci);
static void ctx_begin() {
	Ctx &c = C();
	if (c.pending) {
		vkWaitForFences(s_dev, 1, &c.fence, VK_TRUE, UINT64_MAX);
		c.pending = false;
	}
	wb_ctx_done(s_ci);
	for (Garbage &g : c.garbage) {
		if (g.fb) vkDestroyFramebuffer(s_dev, g.fb, nullptr);
		if (g.view) vkDestroyImageView(s_dev, g.view, nullptr);
		if (g.img) vkDestroyImage(s_dev, g.img, nullptr);
		if (g.buf) vkDestroyBuffer(s_dev, g.buf, nullptr);
		if (g.mem) vkFreeMemory(s_dev, g.mem, nullptr);
	}
	c.garbage.clear();
	for (Buf &b : c.bigBufs) { vkDestroyBuffer(s_dev, b.buf, nullptr); vkFreeMemory(s_dev, b.mem, nullptr); }
	c.bigBufs.clear();
	vkResetFences(s_dev, 1, &c.fence);
	vkResetCommandPool(s_dev, c.pool, 0);
	vkResetDescriptorPool(s_dev, c.dpool, 0);
	c.sets.clear();
	c.setsLeft = kSets;
	VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, c.dpool, 1, &s_dslUbo};
	vkAllocateDescriptorSets(s_dev, &ai, &c.uboSet);
	VkDescriptorBufferInfo bi = {c.stream.buf, 0, sizeof(Ubo)};
	VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = c.uboSet; w.dstBinding = 0; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	w.pBufferInfo = &bi;
	vkUpdateDescriptorSets(s_dev, 1, &w, 0, nullptr);
	c.pos = 0;
	c.initUsed = false;
	VkCommandBufferBeginInfo bi2 = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	vkBeginCommandBuffer(c.init, &bi2);
	vkBeginCommandBuffer(c.main, &bi2);
	bs_reset();
}

// Submits the current context's work and starts the next context. wait/signal: semaphores shared with OpenGL.
static void ctx_submit(VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE) {
	end_rp();
	Ctx &c = C();
	barrier(c.init);
	vkEndCommandBuffer(c.init);
	vkEndCommandBuffer(c.main);
	VkCommandBuffer cbs[2] = {c.init, c.main};
	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = c.initUsed ? 2 : 1;
	si.pCommandBuffers = c.initUsed ? cbs : cbs + 1;
	if (wait) { si.waitSemaphoreCount = 1; si.pWaitSemaphores = &wait; si.pWaitDstStageMask = &stage; }
	if (signal) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &signal; }
	vk_ok(vkQueueSubmit(s_queue, 1, &si, c.fence), "vkQueueSubmit");
	c.pending = true;
	s_ci = (s_ci + 1) % kCtxs;
	ctx_begin();
}

// Room in the current context's stream buffer, or a buffer of its own for a big upload
static void stream_alloc(VkDeviceSize size, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off, u8 **ptr) {
	if (size > kStream / 4) {
		Buf b = buf_create(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
		C().bigBufs.push_back(b);
		*buf = b.buf; *off = 0; *ptr = b.ptr;
		return;
	}
	VkDeviceSize p = (C().pos + align - 1) / align * align;
	if (p + size > kStream) { ctx_submit(); p = 0; }
	C().pos = p + size;
	*buf = C().stream.buf; *off = p; *ptr = C().stream.ptr + p;
}

// ================================================================= framebuffers
struct VFb {
	u32 addr;
	int stride, fmt;
	int w, h;           // allocated, PSP pixels
	int usedH;          // rows drawn into (for texturing from it)
	Img color, depth;
	VkFramebuffer fbo;
	bool drawn;         // since its last write back to memory
	u32 lastFrame;
};
static std::vector<VFb *> s_fbs;
static VFb *s_cur;
static u32 s_dispAddr, s_dispStride = 512;
static int s_dispFmt;

static void target_create(Img *color, Img *depth, VkFramebuffer *fbo, int w, int h) {
	*color = img_create(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
	*depth = img_create(w, h, s_depthFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                    VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | (s_depthFmt == VK_FORMAT_D32_SFLOAT ? 0 : VK_IMAGE_ASPECT_STENCIL_BIT));
	VkImageView att[2] = {color->view, depth->view};
	VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
	fi.renderPass = s_rp; fi.attachmentCount = 2; fi.pAttachments = att; fi.width = w; fi.height = h; fi.layers = 1;
	vkCreateFramebuffer(s_dev, &fi, nullptr, fbo);
	// into GENERAL, cleared (colour and depth 0, as on the OpenGL side)
	end_rp();
	VkCommandBuffer cb = C().main;
	VkImageAspectFlags da = s_depthFmt == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	img_barrier(cb, color->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	img_barrier(cb, depth->img, da, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	VkClearColorValue cc = {};
	VkClearDepthStencilValue dc = {0, 0};
	VkImageSubresourceRange cr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, dr = {da, 0, 1, 0, 1};
	vkCmdClearColorImage(cb, color->img, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &cr);
	vkCmdClearDepthStencilImage(cb, depth->img, VK_IMAGE_LAYOUT_GENERAL, &dc, 1, &dr);
	barrier(cb);
}

static VkImageAspectFlags depth_aspects() {
	return s_depthFmt == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
}

static void copy_image(VkImage from, VkImage to, VkImageAspectFlags aspect, int w, int h) {
	VkImageCopy r = {};
	r.srcSubresource = r.dstSubresource = {aspect, 0, 0, 1};
	r.extent = {(u32)w, (u32)h, 1};
	vkCmdCopyImage(C().main, from, VK_IMAGE_LAYOUT_GENERAL, to, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
}

static void blit_image(VkImage from, VkImage to, VkImageAspectFlags aspect, const int s[4], const int d[4], VkFilter filter,
                       VkImageLayout toLayout = VK_IMAGE_LAYOUT_GENERAL) {
	VkImageBlit r = {};
	r.srcSubresource = r.dstSubresource = {aspect, 0, 0, 1};
	r.srcOffsets[0] = {s[0], s[1], 0}; r.srcOffsets[1] = {s[2], s[3], 1};
	r.dstOffsets[0] = {d[0], d[1], 0}; r.dstOffsets[1] = {d[2], d[3], 1};
	vkCmdBlitImage(C().main, from, VK_IMAGE_LAYOUT_GENERAL, to, toLayout, 1, &r, filter);
}

// (Re)allocates at w x h, keeping what it held.
static void fb_resize(VFb *f, int w, int h) {
	VFb old = *f;
	f->w = w; f->h = h;
	target_create(&f->color, &f->depth, &f->fbo, w * s_scale, h * s_scale);
	if (old.fbo) {
		int cw = std::min(old.w, w) * s_scale, ch = std::min(old.h, h) * s_scale;
		copy_image(old.color.img, f->color.img, VK_IMAGE_ASPECT_COLOR_BIT, cw, ch);
		copy_image(old.depth.img, f->depth.img, depth_aspects(), cw, ch);
		barrier(C().main);
		throw_away(old.color, old.fbo);
		throw_away(old.depth);
	}
}

static VFb *fb_find(u32 addr) {
	for (VFb *f : s_fbs)
		if (f->addr == addr) return f;
	return nullptr;
}

static VFb *fb_get(u32 addr, int stride, int fmt, int needH) {
	needH = std::max(1, std::min(needH, 512));
	if (stride <= 0) stride = 512;
	VFb *f = fb_find(addr);
	if (!f) {
		f = new VFb();
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
static VFb *fb_for_texture(u32 addr, int *ox, int *oy) {
	addr = 0x04000000 | (addr & 0x1FFFFF);
	VFb *best = nullptr;
	for (VFb *f : s_fbs) {
		u32 pitch = f->stride * ng_fmt_bpp(f->fmt);
		u32 end = f->addr + pitch * std::max(f->usedH, 1);
		if (addr >= f->addr && addr < end && (!best || f->addr > best->addr)) best = f;
	}
	if (best) {
		u32 off = addr - best->addr, pitch = best->stride * ng_fmt_bpp(best->fmt);
		*oy = off / pitch;
		*ox = (off % pitch) / ng_fmt_bpp(best->fmt);
	}
	return best;
}

// ================================================================= split views (0xFA commands)
// As on the OpenGL side: each view draws into a framebuffer of its own the size of the game's, starting from the
// game's depth and the first view's colour, and the views are scaled into their parts of the game's framebuffer
// when the game composes them.
#define MAX_VIEWS 4
struct VView { Img color, depth; VkFramebuffer fbo; int w, h; };
static VView s_view[MAX_VIEWS];
static bool s_viewFresh[MAX_VIEWS];
static int s_viewDst[MAX_VIEWS][4], s_viewSrc[MAX_VIEWS][4], s_viewN;
static VFb *s_viewBase;
static int s_viewCur = -1;

static void view_alloc(VView *v, int w, int h) {
	if (v->fbo && v->w == w && v->h == h) return;
	if (v->fbo) { throw_away(v->color, v->fbo); throw_away(v->depth); }
	v->w = w; v->h = h;
	target_create(&v->color, &v->depth, &v->fbo, w, h);
}

void ng_vk_sync(void);
void ng_vk_rcsm(u32 data) {
	if (!s_ready) return;
	ng_vk_sync();
	int kind = data & 0xFF, arg = (data >> 8) & 0xFF;
	s_viewCur = -1;
	VFb *f = s_cur;
	if (!f) return;
	end_rp();
	int w = f->w * s_scale, h = f->h * s_scale;
	if (kind == 1 && arg < MAX_VIEWS) {
		view_alloc(&s_view[arg], w, h);
		if (!s_viewFresh[arg]) {
			VkImage from = arg && s_viewFresh[0] ? s_view[0].color.img : f->color.img;
			copy_image(f->depth.img, s_view[arg].depth.img, depth_aspects(), w, h);
			copy_image(from, s_view[arg].color.img, VK_IMAGE_ASPECT_COLOR_BIT, w, h);
			barrier(C().main);
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
			int s4[4] = {sr[0] * S, sr[1] * S, (sr[0] + sr[2]) * S, (sr[1] + sr[3]) * S};
			int d4[4] = {d[0] * S, d[1] * S, (d[0] + d[2]) * S, (d[1] + d[3]) * S};
			blit_image(s_view[from].color.img, f->color.img, VK_IMAGE_ASPECT_COLOR_BIT, s4, d4, VK_FILTER_LINEAR);
			if (s_depthBlit) blit_image(s_view[from].depth.img, f->depth.img, depth_aspects(), s4, d4, VK_FILTER_NEAREST);
			else if (sr[2] == d[2] && sr[3] == d[3]) {
				VkImageCopy r = {};
				r.srcSubresource = r.dstSubresource = {depth_aspects(), 0, 0, 1};
				r.srcOffset = {s4[0], s4[1], 0}; r.dstOffset = {d4[0], d4[1], 0};
				r.extent = {(u32)(s4[2] - s4[0]), (u32)(s4[3] - s4[1]), 1};
				vkCmdCopyImage(C().main, s_view[from].depth.img, VK_IMAGE_LAYOUT_GENERAL, f->depth.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
			}
		}
		barrier(C().main);
	}
}

static void views_new_frame() {
	s_viewCur = -1;
	for (bool &b : s_viewFresh) b = false;
}

// ================================================================= textures
struct VTex { Img im; u64 hash; u32 checked; };
static std::unordered_map<u64, VTex> s_texCache;
static Img s_palImg;
static u64 s_palHash = ~0ull;

// A new sampled image with the given RGBA8 texels, uploaded by the context's init command buffer (which runs
// before its main one).
static Img upload_image(const u32 *px, int w, int h) {
	Img im = img_create(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
	VkBuffer buf; VkDeviceSize off; u8 *p;
	stream_alloc((VkDeviceSize)w * h * 4, 16, &buf, &off, &p);
	memcpy(p, px, (size_t)w * h * 4);
	VkCommandBuffer cb = C().init;
	C().initUsed = true;
	img_barrier(cb, im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkBufferImageCopy r = {};
	r.bufferOffset = off;
	r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	r.imageExtent = {(u32)w, (u32)h, 1};
	vkCmdCopyBufferToImage(cb, buf, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	img_barrier(cb, im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	return im;
}

static std::unordered_map<u32, VkSampler> s_samplers;
static VkSampler sampler_for(bool linMin, bool linMag, bool clampS, bool clampT, bool aniso) {
	u32 k = linMin | linMag << 1 | clampS << 2 | clampT << 3 | (aniso ? s_aniso : 0) << 4;
	auto it = s_samplers.find(k);
	if (it != s_samplers.end()) return it->second;
	VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	ci.magFilter = linMag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	ci.minFilter = linMin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	ci.addressModeU = clampS ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
	ci.addressModeV = clampT ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
	ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (aniso && s_anisoOk && s_aniso > 1) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = std::min((float)s_aniso, s_maxAniso); }
	ci.maxLod = 0.25f;
	VkSampler s;
	vkCreateSampler(s_dev, &ci, nullptr, &s);
	s_samplers[k] = s;
	return s;
}

static bool s_pixelMapped;
static VkSampler pick_sampler(bool linMin, bool linMag, bool clampS, bool clampT) {
	if (s_filter == 1 || (s_filter == 0 && s_pixelMapped)) linMin = linMag = false;
	else if (s_filter == 2) linMin = linMag = true;
	// anisotropic filtering, as on the OpenGL side, only where the game asks for mipmaps
	return sampler_for(linMin, linMag, clampS, clampT, linMin && ((g_ge.cmd[CMD_TEXMODE] >> 16) & 7));
}

// The draw's texture, and the texture mapping uniforms. Returns false if there's none.
static bool bind_texture(Ubo &u, u64 &key, TexSet &ts) {
	const GeState &g = g_ge;
	u32 addr = g.texAddr(0);
	int fmt = g.texFormat(), w = g.texW(0), h = g.texH(0);
	bool linMin = g.cmd[CMD_TEXFILTER] & 1, linMag = (g.cmd[CMD_TEXFILTER] >> 8) & 1;
	bool clampS = g.cmd[CMD_TEXWRAP] & 1, clampT = (g.cmd[CMD_TEXWRAP] >> 8) & 1;
	u.fbsize[2] = 1.0f / w; u.fbsize[3] = 1.0f / h;
	u.fbtex[0] = 1; u.fbtex[1] = 1; u.fbtex[2] = 0; u.fbtex[3] = 0;

	// a framebuffer?
	if ((addr & 0x0F000000) == 0x04000000) {
		int ox = 0, oy = 0;
		VFb *f = fb_for_texture(addr, &ox, &oy);
		bool direct = f && ((fmt == f->fmt) || (fmt <= FMT_4444 && f->fmt <= FMT_4444));
		bool depal = f && fmt == FMT_CLUT32 && f->fmt == FMT_8888;
		if (direct || depal) {
			if (f == s_cur) NG_LOGD("ngpu: texture from the framebuffer being drawn (%08x)", f->addr);
			ts.view = f->color.view; ts.layout = VK_IMAGE_LAYOUT_GENERAL;
			u.fbtex[0] = (float)w / f->w; u.fbtex[1] = (float)h / f->h;
			u.fbtex[2] = (float)ox / f->w; u.fbtex[3] = (float)oy / f->h;
			if (depal) {
				ts.sampler = s_nearestClamp;
				key |= K_DEPAL;
				u.depal[0] = g.clutShift(); u.depal[1] = g.clutMask(); u.depal[2] = g.clutBase() & 0xFF;
				if (s_palHash != ng_clut_hash()) {
					s_palHash = ng_clut_hash();
					u32 pal[256];
					ng_palette(pal);
					if (s_palImg.img) throw_away(s_palImg);
					s_palImg = upload_image(pal, 256, 1);
				}
				ts.pal = s_palImg.view;
			} else {
				ts.sampler = pick_sampler(linMin, linMag, clampS, clampT);
			}
			return true;
		}
	}

	// from memory
	NgTexRef t;
	if (!ng_tex_ref(&t)) return false;
	VTex &e = s_texCache[t.key];
	bool fresh = !e.im.img;
	if (fresh || e.checked != s_texEpoch) {
		u64 hsh = ng_hash(ng_ptr(t.addr), t.bytes);
		e.checked = s_texEpoch;
		if (fresh || hsh != e.hash) {
			e.hash = hsh;
			int pw, ph;
			const u32 *px = ng_tex_pixels(t, &pw, &ph);
			// (a new image: draws already recorded keep sampling the old one)
			if (!fresh) throw_away(e.im);
			e.im = upload_image(px, pw, ph);
		}
	}
	ts.view = e.im.view; ts.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	ts.sampler = pick_sampler(linMin, linMag, clampS, clampT);
	return true;
}

static VkDescriptorSet tex_set(const TexSet &t) {
	Ctx &c = C();
	auto it = c.sets.find(t);
	if (it != c.sets.end()) return it->second;
	VkDescriptorSet s;
	VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, c.dpool, 1, &s_dslTex};
	vkAllocateDescriptorSets(s_dev, &ai, &s);
	c.setsLeft--;
	VkDescriptorImageInfo ii[2] = {{t.sampler, t.view, t.layout},
	                               {s_nearestClamp, t.pal ? t.pal : s_dummy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
	VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	w.dstSet = s; w.dstBinding = 0; w.descriptorCount = 2; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	w.pImageInfo = ii;
	vkUpdateDescriptorSets(s_dev, 1, &w, 0, nullptr);
	c.sets[t] = s;
	return s;
}

// ================================================================= pipelines
// Render state bits (with the shader key, a pipeline's identity)
union Raster {
	struct {
		u32 blend : 1, src : 4, dst : 4, eq : 3, depth : 1, dfunc : 3, dwrite : 1, cmask : 4, cull : 1, front : 1, topo : 2, blit : 1;
	};
	u32 bits;
};
struct PipeKey { u64 key; u32 raster; bool operator==(const PipeKey &o) const { return key == o.key && raster == o.raster; } };
struct PipeKeyHash { size_t operator()(const PipeKey &k) const { return std::hash<u64>()(k.key * 31 + k.raster); } };
static std::unordered_map<PipeKey, VkPipeline, PipeKeyHash> s_pipes;

enum { BF_ZERO, BF_ONE, BF_SRC_COLOR, BF_ONE_MINUS_SRC_COLOR, BF_DST_COLOR, BF_ONE_MINUS_DST_COLOR, BF_SRC_ALPHA,
       BF_ONE_MINUS_SRC_ALPHA, BF_DST_ALPHA, BF_ONE_MINUS_DST_ALPHA, BF_CONST, BF_ONE_MINUS_CONST };
static const VkBlendFactor kFactors[12] = {
	VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
	VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
	VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
	VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR};

// Runs fn on a thread with a big stack, and waits for it. The game's threads run on 1 MB stacks, which a driver
// compiling a pipeline (with LLVM, say) can run out of.
static void on_big_stack(const std::function<void()> &fn) {
	static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
	static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
	static const std::function<void()> *job;
	static bool started, done;
	pthread_mutex_lock(&mx);
	if (!started) {
		started = true;
		pthread_attr_t a;
		pthread_attr_init(&a);
		pthread_attr_setstacksize(&a, 64 << 20);
		pthread_t t;
		pthread_create(&t, &a, [](void *) -> void * {
			pthread_mutex_lock(&mx);
			for (;;) {
				while (!job) pthread_cond_wait(&cv, &mx);
				pthread_mutex_unlock(&mx);
				(*job)();
				pthread_mutex_lock(&mx);
				job = nullptr;
				done = true;
				pthread_cond_broadcast(&cv);
			}
			return nullptr;
		}, nullptr);
		pthread_attr_destroy(&a);
	}
	job = &fn;
	done = false;
	pthread_cond_broadcast(&cv);
	while (!done) pthread_cond_wait(&cv, &mx);
	pthread_mutex_unlock(&mx);
}

static VkPipeline pipeline_for(u64 key, Raster r) {
	PipeKey pk = {key, r.bits};
	auto it = s_pipes.find(pk);
	if (it != s_pipes.end()) return it->second;
	u32 spec[2] = {(u32)key, (u32)(key >> 32)};
	VkSpecializationMapEntry me[2] = {{0, 0, 4}, {1, 4, 4}};
	VkSpecializationInfo si = {2, me, sizeof spec, spec};
	VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = r.blit ? s_blitVs : s_vs; st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = r.blit ? s_blitFs : s_fs; st[1].pName = "main";
	if (!r.blit) { st[0].pSpecializationInfo = &si; st[1].pSpecializationInfo = &si; }
	VkVertexInputBindingDescription vb = {0, sizeof(NgVert), VK_VERTEX_INPUT_RATE_VERTEX};
	VkVertexInputAttributeDescription va[4] = {
		{0, 0, VK_FORMAT_R32G32B32_SFLOAT, (u32)offsetof(NgVert, pos)}, {1, 0, VK_FORMAT_R32G32_SFLOAT, (u32)offsetof(NgVert, uv)},
		{2, 0, VK_FORMAT_R32G32B32_SFLOAT, (u32)offsetof(NgVert, nrm)}, {3, 0, VK_FORMAT_R8G8B8A8_UNORM, (u32)offsetof(NgVert, color)}};
	VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	if (!r.blit) { vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb; vi.vertexAttributeDescriptionCount = 4; vi.pVertexAttributeDescriptions = va; }
	static const VkPrimitiveTopology topos[4] = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
	                                             VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
	VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = topos[r.topo];
	VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1; vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1;
	rs.cullMode = r.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
	rs.frontFace = r.front ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
	VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	static const VkCompareOp ops[8] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_NOT_EQUAL,
	                                   VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL};
	ds.depthTestEnable = r.depth; ds.depthWriteEnable = r.depth && r.dwrite; ds.depthCompareOp = ops[r.dfunc];
	VkPipelineColorBlendAttachmentState ba = {};
	ba.blendEnable = r.blend;
	ba.srcColorBlendFactor = kFactors[r.src]; ba.dstColorBlendFactor = kFactors[r.dst];
	static const VkBlendOp eqs[5] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
	ba.colorBlendOp = eqs[r.eq];
	ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; ba.alphaBlendOp = VK_BLEND_OP_ADD;
	ba.colorWriteMask = r.cmask;
	VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1; cb.pAttachments = &ba;
	VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
	VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 3; dy.pDynamicStates = dyn;
	VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	ci.stageCount = 2; ci.pStages = st; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp;
	ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pDepthStencilState = &ds; ci.pColorBlendState = &cb;
	ci.pDynamicState = &dy; ci.layout = s_layout; ci.renderPass = s_rp;
	VkPipeline p = VK_NULL_HANDLE;
	VkResult res;
	on_big_stack([&] { res = vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p); });
	vk_ok(res, "vkCreateGraphicsPipelines");
	s_pipes[pk] = p;
	return p;
}

static int blend_src(int f, u32 fix, bool &useConst) {
	switch (f) {
	case 0: return BF_DST_COLOR;
	case 1: return BF_ONE_MINUS_DST_COLOR;
	case 2: case 6: return BF_SRC_ALPHA;            // (6, doubled: approximated)
	case 3: case 7: return BF_ONE_MINUS_SRC_ALPHA;
	case 4: case 8: return BF_DST_ALPHA;
	case 5: case 9: return BF_ONE_MINUS_DST_ALPHA;
	default:
		if ((fix & 0xFFFFFF) == 0xFFFFFF) return BF_ONE;
		if ((fix & 0xFFFFFF) == 0) return BF_ZERO;
		useConst = true;
		return BF_CONST;
	}
}
static int blend_dst(int f, u32 fix, u32 fixA, bool &useConst) {
	switch (f) {
	case 0: return BF_SRC_COLOR;
	case 1: return BF_ONE_MINUS_SRC_COLOR;
	case 2: case 6: return BF_SRC_ALPHA;
	case 3: case 7: return BF_ONE_MINUS_SRC_ALPHA;
	case 4: case 8: return BF_DST_ALPHA;
	case 5: case 9: return BF_ONE_MINUS_DST_ALPHA;
	default:
		if ((fix & 0xFFFFFF) == 0xFFFFFF) return BF_ONE;
		if ((fix & 0xFFFFFF) == 0) return BF_ZERO;
		if (useConst && ((fix ^ fixA) & 0xFFFFFF) == 0xFFFFFF) return BF_ONE_MINUS_CONST;
		useConst = true;
		return BF_CONST;
	}
}

// The draw's render state (as ng_gl.cpp's set_raster_state); *bcolor: the blend constant, if it uses one
static Raster raster_state(int prim, bool through, NgMode mode, u32 *bcolor) {
	const GeState &g = g_ge;
	Raster r;
	r.bits = 0;
	r.topo = mode;
	*bcolor = 0xFFFFFFFF;
	if (g.clearMode()) {
		u32 cm = g.cmd[CMD_CLEARMODE];
		bool dw = (cm & 0x400) != 0;
		r.depth = dw; r.dfunc = 1; r.dwrite = 1;
		r.cmask = ((cm & 0x100) ? 7 : 0) | ((cm & 0x200) ? 8 : 0);
		return r;
	}
	if (g.cmd[CMD_BLEND_EN] & 1) {
		r.blend = 1;
		u32 b = g.cmd[CMD_BLEND];
		bool useConst = false;
		u32 fa = g.cmd[CMD_FIXA] & 0xFFFFFF, fb = g.cmd[CMD_FIXB] & 0xFFFFFF;
		int s = blend_src(b & 0xF, fa, useConst);
		int d = blend_dst((b >> 4) & 0xF, fb, fa, useConst);
		r.src = s; r.dst = d;
		int e = (b >> 8) & 7;
		r.eq = e >= 1 && e <= 4 ? e : 0;   // (5, absolute difference: approximated)
		if (useConst) *bcolor = s == BF_CONST ? fa : fb;
	}
	if (g.cmd[CMD_ZTEST_EN] & 1) {
		r.depth = 1;
		r.dfunc = g.cmd[CMD_ZTEST] & 7;
		r.dwrite = !(g.cmd[CMD_ZWRITE_DIS] & 1);
	}
	// colour mask: the GE's per-bit mask, per channel here; alpha (the stencil) only by clears
	u32 m = g.cmd[CMD_MASKRGB] & 0xFFFFFF;
	r.cmask = ((m & 0xFF) != 0xFF ? 1 : 0) | ((m & 0xFF00) != 0xFF00 ? 2 : 0) | ((m & 0xFF0000) != 0xFF0000 ? 4 : 0);
	if (!through && (g.cmd[CMD_CULL_EN] & 1) && prim >= PRIM_TRIANGLES && prim != PRIM_RECTANGLES) {
		r.cull = 1;
		// (OpenGL's counter-clockwise is Vulkan's clockwise, with the same coordinates: Vulkan's y runs down)
		r.front = g.cmd[CMD_CULL] & 1;
	}
	return r;
}

// ================================================================= drawing
static void begin_rp(VkFramebuffer fbo, int w, int h) {
	if (bs.rp && bs.fb == fbo) return;
	end_rp();
	VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	bi.renderPass = s_rp; bi.framebuffer = fbo; bi.renderArea = {{0, 0}, {(u32)w, (u32)h}};
	vkCmdBeginRenderPass(C().main, &bi, VK_SUBPASS_CONTENTS_INLINE);
	VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
	vkCmdSetViewport(C().main, 0, 1, &vp);
	bs.rp = true; bs.fb = fbo;
	bs.sc[0] = -1;
}

extern bool g_ng_dirty;
static struct {
	bool open;
	int prim;
	NgMode mode;
	bool pixelMapped;
	VkFramebuffer fbo;
	int fbw, fbh;
	VkPipeline pipe;
	TexSet tex;
	Ubo ubo;
	int sc[4];
	u32 bcolor;
	std::vector<NgVert> verts;
	std::vector<u16> idx;
} s_batch;
static std::vector<NgVert> s_verts;
static std::vector<u16> s_idx;
static Ubo s_lastUbo;
static int64_t s_lastUboOff = -1;
static int s_lastUboCtx = -1;

static void flush_batch() {
	if (!s_batch.open) return;
	s_batch.open = false;
	int nv = (int)s_batch.verts.size(), n = (int)s_batch.idx.size();
	if (!n) return;
	// room for everything first, so that all of it lands in one context
	VkDeviceSize need = (VkDeviceSize)nv * sizeof(NgVert) + n * 2 + 16 + sizeof(Ubo) + s_uboAlign * 2;
	if (C().pos + need > kStream || C().setsLeft < 1) ctx_submit();
	Ctx &c = C();
	begin_rp(s_batch.fbo, s_batch.fbw, s_batch.fbh);
	VkCommandBuffer cb = c.main;
	if (s_lastUboCtx != s_ci || s_lastUboOff < 0 || memcmp(&s_lastUbo, &s_batch.ubo, sizeof(Ubo))) {
		VkBuffer b; VkDeviceSize off; u8 *p;
		stream_alloc(sizeof(Ubo), s_uboAlign, &b, &off, &p);
		memcpy(p, &s_batch.ubo, sizeof(Ubo));
		s_lastUbo = s_batch.ubo; s_lastUboOff = off; s_lastUboCtx = s_ci;
	}
	if (bs.pipe != s_batch.pipe) { bs.pipe = s_batch.pipe; vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_batch.pipe); }
	if (bs.uboOff != s_lastUboOff) {
		bs.uboOff = s_lastUboOff;
		u32 off = (u32)s_lastUboOff;
		vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 1, &c.uboSet, 1, &off);
	}
	VkDescriptorSet ts = tex_set(s_batch.tex);
	if (bs.tex != ts) { bs.tex = ts; vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 1, 1, &ts, 0, nullptr); }
	if (memcmp(bs.sc, s_batch.sc, sizeof bs.sc)) {
		memcpy(bs.sc, s_batch.sc, sizeof bs.sc);
		VkRect2D r = {{s_batch.sc[0], s_batch.sc[1]}, {(u32)s_batch.sc[2], (u32)s_batch.sc[3]}};
		vkCmdSetScissor(cb, 0, 1, &r);
	}
	if (!bs.bcolorSet || bs.bcolor != s_batch.bcolor) {
		bs.bcolorSet = true; bs.bcolor = s_batch.bcolor;
		u32 cc = s_batch.bcolor;
		float bc[4] = {(cc & 0xFF) / 255.0f, ((cc >> 8) & 0xFF) / 255.0f, ((cc >> 16) & 0xFF) / 255.0f, 1.0f};
		vkCmdSetBlendConstants(cb, bc);
	}
	VkBuffer vb, ib; VkDeviceSize voff, ioff; u8 *p;
	stream_alloc((VkDeviceSize)nv * sizeof(NgVert), 16, &vb, &voff, &p);
	memcpy(p, s_batch.verts.data(), (size_t)nv * sizeof(NgVert));
	stream_alloc((VkDeviceSize)n * 2, 4, &ib, &ioff, &p);
	memcpy(p, s_batch.idx.data(), (size_t)n * 2);
	vkCmdBindVertexBuffers(cb, 0, 1, &vb, &voff);
	vkCmdBindIndexBuffer(cb, ib, ioff, VK_INDEX_TYPE_UINT16);
	vkCmdDrawIndexed(cb, n, 1, 0, 0, 0);
	extern u32 g_ng_gl_draws;
	g_ng_gl_draws++;
	s_batch.verts.clear();
	s_batch.idx.clear();
}

void ng_vk_sync(void) { flush_batch(); g_ng_dirty = true; }

void ng_vk_prim(int prim, const NgVert *v, int count, const NgLayout &L) {
	if (!s_ready || count <= 0) return;
	const GeState &g = g_ge;
	bool through = L.through, clear = g.clearMode();
	extern u32 g_ng_draws;
	g_ng_draws++;
	static int nobatch = -1;   // SR2_NGPU_NOBATCH=1: every draw on its own, for comparisons
	if (nobatch < 0) nobatch = getenv("SR2_NGPU_NOBATCH") && atoi(getenv("SR2_NGPU_NOBATCH"));
	if (s_batch.open && !g_ng_dirty && prim == s_batch.prim && !nobatch) {
		bool pm = !clear && (g.cmd[CMD_TEX_EN] & 1) && (g.cmd[CMD_TEXFILTER] & 0x101) && ng_pixel_mapped(prim, v, count, through);
		NgMode mode;
		int n = pm == s_batch.pixelMapped ? ng_assemble(prim, v, count, through, s_verts, s_idx, &mode, true) : 0;
		int base = (int)s_batch.verts.size();
		if (n && mode == s_batch.mode && base + (int)s_verts.size() <= 65536 && (int)s_batch.idx.size() + n <= (1 << 19)) {
			s_batch.verts.insert(s_batch.verts.end(), s_verts.begin(), s_verts.end());
			for (u16 i : s_idx) s_batch.idx.push_back((u16)(base + i));
			return;
		}
	}
	flush_batch();
	int sx1 = g.scissorX1(), sy1 = g.scissorY1(), sx2 = g.scissorX2(), sy2 = g.scissorY2();
	if (sx2 < sx1 || sy2 < sy1) return;
	VFb *f = fb_get(g.fbAddr(), g.fbStride(), g.fbFormat(), sy2 + 1);
	s_cur = f;
	f->usedH = std::max(f->usedH, std::min(sy2 + 1, f->h));
	f->drawn = true;
	f->lastFrame = s_frame;

	NgMode mode;
	// (Vulkan's flat shading takes the first vertex's colour, the GE's the last)
	int n = ng_assemble(prim, v, count, through, s_verts, s_idx, &mode, true);
	if (!n || (int)s_verts.size() > 65536) return;

	Ubo &u = s_batch.ubo;
	u64 key = ng_draw_key(L, through, clear);
	ng_fill_ubo(u, f->w, f->h, through, s_viewOff);
	TexSet ts = {s_dummy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, s_nearestClamp, VK_NULL_HANDLE};
	bool tex = !clear && (g.cmd[CMD_TEX_EN] & 1);
	if (tex) {
		s_pixelMapped = (g.cmd[CMD_TEXFILTER] & 0x101) && ng_pixel_mapped(prim, v, count, through);
		if (bind_texture(u, key, ts)) key |= ng_tex_key_bits(through);
	}
	u32 bcolor;
	Raster r = raster_state(prim, through, mode, &bcolor);
	bool view = f == s_viewBase && s_viewCur >= 0;
	s_batch.fbo = view ? s_view[s_viewCur].fbo : f->fbo;
	s_batch.fbw = f->w * s_scale; s_batch.fbh = f->h * s_scale;
	s_batch.pipe = pipeline_for(key, r);
	s_batch.tex = ts;
	s_batch.bcolor = bcolor;
	int S = s_scale;
	s_batch.sc[0] = sx1 * S; s_batch.sc[1] = sy1 * S; s_batch.sc[2] = (sx2 - sx1 + 1) * S; s_batch.sc[3] = (sy2 - sy1 + 1) * S;
	s_batch.open = true;
	s_batch.prim = prim;
	s_batch.mode = mode;
	s_batch.pixelMapped = tex && s_pixelMapped;
	s_batch.verts.swap(s_verts);
	s_batch.idx.swap(s_idx);
	g_ng_dirty = false;
}

// ================================================================= memory <-> framebuffers
// Guest memory drawn over the top rows of a framebuffer
static void draw_memory(VFb *f, int rows) {
	std::vector<u32> px((size_t)f->stride * rows);
	ng_fb_from_memory(f->addr, f->stride, f->fmt, f->stride, rows, px.data());
	Img im = upload_image(px.data(), f->stride, rows);
	TexSet ts = {im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, s_linearClamp, VK_NULL_HANDLE};
	if (C().setsLeft < 1) ctx_submit();
	begin_rp(f->fbo, f->w * s_scale, f->h * s_scale);
	VkCommandBuffer cb = C().main;
	Raster r;
	r.bits = 0; r.blit = 1; r.cmask = 15; r.topo = 3;
	VkPipeline p = pipeline_for(0, r);
	bs.pipe = p;
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
	VkDescriptorSet s = tex_set(ts);
	bs.tex = s;
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 1, 1, &s, 0, nullptr);
	VkRect2D sc = {{0, 0}, {(u32)(f->w * s_scale), (u32)(f->h * s_scale)}};
	vkCmdSetScissor(cb, 0, 1, &sc);
	bs.sc[0] = -1;
	float pc[8] = {-1, -1, 1, -1 + 2.0f * rows / f->h, 0, 0, 1, 1};
	vkCmdPushConstants(cb, s_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
	vkCmdDraw(cb, 4, 1, 0, 0);
	end_rp();
	throw_away(im);   // (into the context that used it)
}

static void memory_written(u32 addr, u32 size) {
	if (!s_ready) return;
	addr = 0x04000000 | (addr & 0x1FFFFF);
	for (VFb *f : s_fbs) {
		u32 pitch = f->stride * ng_fmt_bpp(f->fmt), end = f->addr + pitch * f->h;
		if (addr >= end || addr + size <= f->addr) continue;
		int rows = std::min<int>(f->h, (std::min(end, addr + size) - f->addr + pitch - 1) / pitch);
		draw_memory(f, rows);
		f->usedH = std::max(f->usedH, rows);
	}
}

// Framebuffers the game reads back with the CPU (the exposure buffer, see ng_gl.cpp), written back to guest
// memory after they've been drawn: scaled down into a small image, copied into the context's stream buffer, and
// into memory once the context is done (normally by the next list).
static bool wants_writeback(const VFb *f) { return f->stride <= 64; }
struct VWb { bool active; int ctx; const u8 *px; u32 addr; int stride, fmt, w, h; };
static std::vector<VWb> s_wb;
static Img s_wbImg;

static void wb_finish(VWb &j) {
	ng_fb_to_memory(j.addr, j.stride, j.fmt, j.w, j.h, (const u32 *)j.px);
	j.active = false;
}
static void wb_ctx_done(int ci) {
	for (VWb &j : s_wb)
		if (j.active && j.ctx == ci) wb_finish(j);
}
static void wb_collect() {
	for (VWb &j : s_wb)
		if (j.active && s_ctx[j.ctx].pending && vkGetFenceStatus(s_dev, s_ctx[j.ctx].fence) == VK_SUCCESS) wb_finish(j);
}

static void writeback(VFb *f) {
	if (!f->drawn) return;
	f->drawn = false;
	int w = std::min(f->stride, f->w), h = f->usedH;
	if (w <= 0 || h <= 0) return;
	end_rp();
	if (!s_wbImg.img || s_wbImg.w < w || s_wbImg.h < h) {
		if (s_wbImg.img) throw_away(s_wbImg);
		s_wbImg = img_create(std::max(w, s_wbImg.w), std::max(h, s_wbImg.h), VK_FORMAT_R8G8B8A8_UNORM,
		                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
		img_barrier(C().main, s_wbImg.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	}
	VkBuffer buf; VkDeviceSize off; u8 *p;
	stream_alloc((VkDeviceSize)w * h * 4, 16, &buf, &off, &p);
	int s4[4] = {0, 0, w * s_scale, h * s_scale}, d4[4] = {0, 0, w, h};
	blit_image(f->color.img, s_wbImg.img, VK_IMAGE_ASPECT_COLOR_BIT, s4, d4, VK_FILTER_LINEAR);
	barrier(C().main);
	VkBufferImageCopy r = {};
	r.bufferOffset = off;
	r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	r.imageExtent = {(u32)w, (u32)h, 1};
	vkCmdCopyImageToBuffer(C().main, s_wbImg.img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r);
	barrier(C().main);
	VWb j = {true, s_ci, p, f->addr, f->stride, f->fmt, w, h};
	for (VWb &o : s_wb) if (!o.active) { o = j; return; }
	s_wb.push_back(j);
}

void ng_vk_list_done(void) {
	if (!s_ready) return;
	ng_vk_sync();
	wb_collect();
	s_texEpoch++;
	for (VFb *f : s_fbs)
		if (f->drawn && wants_writeback(f)) writeback(f);
	ctx_submit();
}

// ================================================================= presenting (through OpenGL)
// The displayed frame goes into an image shared with OpenGL, which ng_gl.cpp draws into the window.
static struct {
	VkImage img;
	VkDeviceMemory mem;
	VkDeviceSize size;
	int w, h;
	GLuint glMem, glTex;
	VkSemaphore vkDone, glDone;   // Vulkan wrote it / OpenGL read it (with s_glSems)
	GLuint glVkDone, glGlDone;
	bool glDonePending;
	bool shared;                  // held by OpenGL (between a present and the next copy)
	GLsync glFence;               // without semaphores: OpenGL's last read of it
} s_disp;

void ng_gl_external_display(unsigned tex, float u1, float v1);
extern "C" void ngpu_present(int x, int y, int w, int h);
extern "C" int ngpu_read_display(uint8_t *rgba, int w, int h);
extern "C" void ngpu_set_display(uint32_t addr, int stride, int fmt);

#ifdef _WIN32
typedef HANDLE ExtHandle;
static const VkExternalMemoryHandleTypeFlagBits kMemHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
static const VkExternalSemaphoreHandleTypeFlagBits kSemHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
typedef int ExtHandle;
static const VkExternalMemoryHandleTypeFlagBits kMemHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
static const VkExternalSemaphoreHandleTypeFlagBits kSemHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

static bool export_memory(VkDeviceMemory mem, ExtHandle *h) {
#ifdef _WIN32
	VkMemoryGetWin32HandleInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, mem, kMemHandle};
	return vk_ok(vkGetMemoryWin32HandleKHR(s_dev, &gi, h), "vkGetMemoryWin32HandleKHR");
#else
	VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr, mem, kMemHandle};
	return vk_ok(vkGetMemoryFdKHR(s_dev, &gi, h), "vkGetMemoryFdKHR");
#endif
}

static VkSemaphore shared_semaphore(GLuint *glSem) {
	VkExportSemaphoreCreateInfo ei = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, nullptr, (VkExternalSemaphoreHandleTypeFlags)kSemHandle};
	VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ei};
	VkSemaphore s = VK_NULL_HANDLE;
	if (!vk_ok(vkCreateSemaphore(s_dev, &ci, nullptr, &s), "vkCreateSemaphore")) return VK_NULL_HANDLE;
	gx.GenSemaphores(1, glSem);
#ifdef _WIN32
	VkSemaphoreGetWin32HandleInfoKHR gi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR, nullptr, s, kSemHandle};
	HANDLE h;
	if (!vk_ok(vkGetSemaphoreWin32HandleKHR(s_dev, &gi, &h), "vkGetSemaphoreWin32HandleKHR")) return VK_NULL_HANDLE;
	gx.ImportSemaphoreWin32Handle(*glSem, GLX_HANDLE_TYPE_OPAQUE_WIN32, h);
#else
	VkSemaphoreGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, nullptr, s, kSemHandle};
	int fd;
	if (!vk_ok(vkGetSemaphoreFdKHR(s_dev, &gi, &fd), "vkGetSemaphoreFdKHR")) return VK_NULL_HANDLE;
	gx.ImportSemaphoreFd(*glSem, GLX_HANDLE_TYPE_OPAQUE_FD, fd);   // (OpenGL owns the fd now)
#endif
	return s;
}

static void disp_destroy() {
	if (!s_disp.img) return;
	vkDeviceWaitIdle(s_dev);
	glDeleteTextures(1, &s_disp.glTex);
	gx.DeleteMemoryObjects(1, &s_disp.glMem);
	vkDestroyImage(s_dev, s_disp.img, nullptr);
	vkFreeMemory(s_dev, s_disp.mem, nullptr);
	s_disp.img = VK_NULL_HANDLE;
	s_disp.shared = false;
}

// (Re)creates the shared image at the display's size at the internal resolution. False if OpenGL can't use it.
static bool disp_create() {
	disp_destroy();
	int w = 480 * s_scale, h = 272 * s_scale;
	VkExternalMemoryImageCreateInfo ei = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, (VkExternalMemoryHandleTypeFlags)kMemHandle};
	VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ei};
	ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R8G8B8A8_UNORM; ci.extent = {(u32)w, (u32)h, 1};
	ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (!vk_ok(vkCreateImage(s_dev, &ci, nullptr, &s_disp.img), "vkCreateImage (shared)")) return false;
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(s_dev, s_disp.img, &req);
	VkMemoryDedicatedAllocateInfo di = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, s_disp.img, VK_NULL_HANDLE};
	VkExportMemoryAllocateInfo xi = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &di, (VkExternalMemoryHandleTypeFlags)kMemHandle};
	s_disp.mem = alloc_for(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &xi);
	s_disp.size = req.size;
	vkBindImageMemory(s_dev, s_disp.img, s_disp.mem, 0);
	s_disp.w = w; s_disp.h = h;
	ExtHandle hnd;
	if (!export_memory(s_disp.mem, &hnd)) return false;
	while (glGetError()) {}
	gx.CreateMemoryObjects(1, &s_disp.glMem);
	GLint ded = GL_TRUE;
	gx.MemoryObjectParameteriv(s_disp.glMem, GLX_DEDICATED_MEMORY_OBJECT, &ded);
#ifdef _WIN32
	gx.ImportMemoryWin32Handle(s_disp.glMem, s_disp.size, GLX_HANDLE_TYPE_OPAQUE_WIN32, hnd);
#else
	gx.ImportMemoryFd(s_disp.glMem, s_disp.size, GLX_HANDLE_TYPE_OPAQUE_FD, hnd);   // (OpenGL owns the fd now)
#endif
	glGenTextures(1, &s_disp.glTex);
	glBindTexture(GL_TEXTURE_2D, s_disp.glTex);
	glTexParameteri(GL_TEXTURE_2D, GLX_TEXTURE_TILING, GLX_OPTIMAL_TILING);
	gx.TexStorageMem2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h, s_disp.glMem, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	GLenum err = glGetError();
	if (err) { NG_LOGE("Vulkan: OpenGL couldn't take the shared image (error %04x)", err); return false; }
	end_rp();
	img_barrier(C().main, s_disp.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	return true;
}

// Copies the displayed framebuffer into the shared image and has OpenGL wait for it. False if there's none yet
// (ng_gl.cpp then shows the display memory).
static bool publish() {
	ng_vk_sync();
	VFb *f = s_dispAddr ? fb_find(s_dispAddr) : nullptr;
	if (!f || !s_disp.img) { ng_gl_external_display(0, 0, 0); return false; }
	if (!s_glSems && s_disp.glFence) {   // OpenGL's previous read has to be over before the image is written again
		glClientWaitSync(s_disp.glFence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
		glDeleteSync(s_disp.glFence);
		s_disp.glFence = nullptr;
	}
	end_rp();
	VkCommandBuffer cb = C().main;
	u32 ext = s_disp.shared ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED, me = s_disp.shared ? s_qfam : VK_QUEUE_FAMILY_IGNORED;
	img_barrier(cb, s_disp.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ext, me);
	int cw = std::min(s_disp.w, f->w * s_scale), ch = std::min(s_disp.h, f->h * s_scale);
	VkImageCopy r = {};
	r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	r.extent = {(u32)cw, (u32)ch, 1};
	vkCmdCopyImage(cb, f->color.img, VK_IMAGE_LAYOUT_GENERAL, s_disp.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
	img_barrier(cb, s_disp.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, s_qfam, VK_QUEUE_FAMILY_EXTERNAL);
	s_disp.shared = true;
	int ci = s_ci;
	if (s_glSems) {
		ctx_submit(s_disp.glDonePending ? s_disp.glDone : VK_NULL_HANDLE, s_disp.vkDone);
		s_disp.glDonePending = false;
		GLenum layout = GLX_LAYOUT_SHADER_READ_ONLY;
		gx.WaitSemaphore(s_disp.glVkDone, 0, nullptr, 1, &s_disp.glTex, &layout);
	} else {
		ctx_submit();
		vkWaitForFences(s_dev, 1, &s_ctx[ci].fence, VK_TRUE, UINT64_MAX);
	}
	ng_gl_external_display(s_disp.glTex, (float)cw / s_disp.w, (float)ch / s_disp.h);
	return true;
}

// After OpenGL drew from the shared image
static void published() {
	if (s_glSems) {
		GLenum layout = GLX_LAYOUT_SHADER_READ_ONLY;
		gx.SignalSemaphore(s_disp.glGlDone, 0, nullptr, 1, &s_disp.glTex, &layout);
		glFlush();
		s_disp.glDonePending = true;
	} else {
		s_disp.glFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	}
}

extern "C" void ngvk_present(int x, int y, int w, int h) {
	if (!s_ready) { ngpu_present(x, y, w, h); return; }
	views_new_frame();
	s_frame++;
	bool shown = publish();
	ngpu_present(x, y, w, h);
	if (shown) published();
	wb_collect();
}

extern "C" int ngvk_read_display(uint8_t *rgba, int w, int h) {
	if (!s_ready) return ngpu_read_display(rgba, w, h);
	bool shown = publish();
	int r = ngpu_read_display(rgba, w, h);
	if (shown) published();
	return r;
}

// ================================================================= setup
static VkShaderModule shader(const u32 *code, size_t size) {
	VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, size, code};
	VkShaderModule m = VK_NULL_HANDLE;
	vkCreateShaderModule(s_dev, &ci, nullptr, &m);
	return m;
}

static bool has_ext(const std::vector<VkExtensionProperties> &l, const char *name) {
	for (auto &e : l) if (!strcmp(e.extensionName, name)) return true;
	return false;
}

static bool gl_has(const char *name) {
	GLint n = 0;
	glGetIntegerv(GLX_NUM_EXTENSIONS, &n);
	for (GLint i = 0; i < n; i++) {
		const char *e = (const char *)gx.GetStringi(GLX_EXTENSIONS, i);
		if (e && !strcmp(e, name)) return true;
	}
	return false;
}

static bool vk_init(void *(*get_proc)(const char *)) {
	// OpenGL: can it take Vulkan's images?
#define GX(f, n) gx.f = (decltype(gx.f))get_proc(n)
	GX(GetStringi, "glGetStringi"); GX(GetUnsignedBytei_v, "glGetUnsignedBytei_vEXT");
	GX(CreateMemoryObjects, "glCreateMemoryObjectsEXT"); GX(DeleteMemoryObjects, "glDeleteMemoryObjectsEXT");
	GX(MemoryObjectParameteriv, "glMemoryObjectParameterivEXT"); GX(ImportMemoryFd, "glImportMemoryFdEXT");
	GX(ImportMemoryWin32Handle, "glImportMemoryWin32HandleEXT"); GX(TexStorageMem2D, "glTexStorageMem2DEXT");
	GX(GenSemaphores, "glGenSemaphoresEXT"); GX(ImportSemaphoreFd, "glImportSemaphoreFdEXT");
	GX(ImportSemaphoreWin32Handle, "glImportSemaphoreWin32HandleEXT"); GX(WaitSemaphore, "glWaitSemaphoreEXT");
	GX(SignalSemaphore, "glSignalSemaphoreEXT");
#undef GX
#ifdef _WIN32
	const char *memExt = "GL_EXT_memory_object_win32", *semExt = "GL_EXT_semaphore_win32";
	bool glMem = gx.ImportMemoryWin32Handle != nullptr;
	bool glSem = gx.ImportSemaphoreWin32Handle != nullptr;
#else
	const char *memExt = "GL_EXT_memory_object_fd", *semExt = "GL_EXT_semaphore_fd";
	bool glMem = gx.ImportMemoryFd != nullptr;
	bool glSem = gx.ImportSemaphoreFd != nullptr;
#endif
	if (!gx.GetStringi || !gl_has("GL_EXT_memory_object") || !gl_has(memExt) || !glMem || !gx.CreateMemoryObjects || !gx.TexStorageMem2D) {
		NG_LOGE("Vulkan: the OpenGL driver can't show Vulkan's images (no %s)", memExt);
		return false;
	}
	s_glSems = gl_has("GL_EXT_semaphore") && gl_has(semExt) && glSem && gx.GenSemaphores && gx.WaitSemaphore && gx.SignalSemaphore;

	// the loader
#ifdef _WIN32
	s_lib = (void *)LoadLibraryA("vulkan-1.dll");
	if (s_lib) vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress((HMODULE)s_lib, "vkGetInstanceProcAddr");
#else
	const char *libs[] = {"libvulkan.so.1", "libvulkan.so", "libvulkan.1.dylib", "libMoltenVK.dylib"};
	for (const char *l : libs) if ((s_lib = dlopen(l, RTLD_NOW | RTLD_LOCAL))) break;
	if (s_lib) vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(s_lib, "vkGetInstanceProcAddr");
#endif
	if (!vkGetInstanceProcAddr) { NG_LOGE("Vulkan: no Vulkan driver found"); return false; }
#define VK_GET_GLOBAL(f) f = (PFN_##f)vkGetInstanceProcAddr(VK_NULL_HANDLE, #f);
	VK_GLOBAL_FUNCS(VK_GET_GLOBAL)
#undef VK_GET_GLOBAL
	u32 ver = VK_API_VERSION_1_0;
	if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&ver);
	if (!vkCreateInstance || ver < VK_API_VERSION_1_1) { NG_LOGE("Vulkan: needs Vulkan 1.1"); return false; }
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "Sonic Rivals 2", 1, "sr2recomp", 1, VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app};
	const char *layers[1] = {"VK_LAYER_KHRONOS_validation"};
	if (getenv("SR2_VK_VALIDATE") && atoi(getenv("SR2_VK_VALIDATE"))) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers; }
	if (!vk_ok(vkCreateInstance(&ici, nullptr, &s_inst), "vkCreateInstance")) return false;
#define VK_GET_INST(f) f = (PFN_##f)vkGetInstanceProcAddr(s_inst, #f);
	VK_INSTANCE_FUNCS(VK_GET_INST)
#undef VK_GET_INST

	// the device OpenGL runs on (images can only be shared there)
	u8 glUuid[16] = {};
	bool haveUuid = false;
	if (gx.GetUnsignedBytei_v) {
		GLint n = 0;
		glGetIntegerv(GLX_NUM_DEVICE_UUIDS, &n);
		if (n > 0) { gx.GetUnsignedBytei_v(GLX_DEVICE_UUID, 0, glUuid); haveUuid = true; }
	}
	u32 nd = 0;
	vkEnumeratePhysicalDevices(s_inst, &nd, nullptr);
	std::vector<VkPhysicalDevice> devs(nd);
	vkEnumeratePhysicalDevices(s_inst, &nd, devs.data());
	VkPhysicalDeviceProperties props = {};
	for (VkPhysicalDevice d : devs) {
		VkPhysicalDeviceIDProperties idp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
		VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp};
		vkGetPhysicalDeviceProperties2(d, &p2);
		if (p2.properties.apiVersion < VK_API_VERSION_1_1) continue;
		if (haveUuid ? !memcmp(idp.deviceUUID, glUuid, 16) : !s_phys) { s_phys = d; props = p2.properties; }
	}
	if (!s_phys) { NG_LOGE("Vulkan: no Vulkan device is the one OpenGL runs on"); return false; }
	vkGetPhysicalDeviceMemoryProperties(s_phys, &s_memProps);
	s_uboAlign = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16);
	u32 nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(s_phys, &nq, nullptr);
	std::vector<VkQueueFamilyProperties> qf(nq);
	vkGetPhysicalDeviceQueueFamilyProperties(s_phys, &nq, qf.data());
	s_qfam = ~0u;
	for (u32 i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { s_qfam = i; break; }
	if (s_qfam == ~0u) { NG_LOGE("Vulkan: no graphics queue"); return false; }
	u32 ne = 0;
	vkEnumerateDeviceExtensionProperties(s_phys, nullptr, &ne, nullptr);
	std::vector<VkExtensionProperties> exts(ne);
	vkEnumerateDeviceExtensionProperties(s_phys, nullptr, &ne, exts.data());
#ifdef _WIN32
	const char *want[2] = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME};
#else
	const char *want[2] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
#endif
	if (!has_ext(exts, want[0])) { NG_LOGE("Vulkan: the device can't share images (no %s)", want[0]); return false; }
	if (!has_ext(exts, want[1])) s_glSems = false;
	VkPhysicalDeviceFeatures have, feats = {};
	vkGetPhysicalDeviceFeatures(s_phys, &have);
	s_anisoOk = have.samplerAnisotropy;
	feats.samplerAnisotropy = have.samplerAnisotropy;
	s_maxAniso = props.limits.maxSamplerAnisotropy;
	float prio = 1;
	VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, s_qfam, 1, &prio};
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi;
	dci.enabledExtensionCount = s_glSems ? 2 : 1; dci.ppEnabledExtensionNames = want;
	dci.pEnabledFeatures = &feats;
	if (!vk_ok(vkCreateDevice(s_phys, &dci, nullptr, &s_dev), "vkCreateDevice")) return false;
#define VK_GET_DEV(f) f = (PFN_##f)vkGetDeviceProcAddr(s_dev, #f);
	VK_DEVICE_FUNCS(VK_GET_DEV) VK_EXTERNAL_FUNCS(VK_GET_DEV)
#undef VK_GET_DEV
	vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);

	// depth: 24 bits with stencil as on the OpenGL side, or what there is
	const VkFormat depths[3] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT};
	s_depthFmt = VK_FORMAT_UNDEFINED;
	for (VkFormat df : depths) {
		VkFormatProperties fp;
		vkGetPhysicalDeviceFormatProperties(s_phys, df, &fp);
		if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
			s_depthFmt = df;
			s_depthBlit = (fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) ==
			              (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT);
			break;
		}
	}

	// the render pass every framebuffer uses: kept contents, GENERAL throughout
	VkAttachmentDescription att[2] = {};
	att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
	att[1].format = s_depthFmt;
	for (VkAttachmentDescription &a : att) {
		a.samples = VK_SAMPLE_COUNT_1_BIT;
		a.loadOp = a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		a.storeOp = a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
		a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
	}
	VkAttachmentReference cref = {0, VK_IMAGE_LAYOUT_GENERAL}, dref = {1, VK_IMAGE_LAYOUT_GENERAL};
	VkSubpassDescription sub = {};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref; sub.pDepthStencilAttachment = &dref;
	VkRenderPassCreateInfo rci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	rci.attachmentCount = 2; rci.pAttachments = att; rci.subpassCount = 1; rci.pSubpasses = &sub;
	if (!vk_ok(vkCreateRenderPass(s_dev, &rci, nullptr, &s_rp), "vkCreateRenderPass")) return false;

	VkDescriptorSetLayoutBinding ub = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};
	VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 1, &ub};
	vkCreateDescriptorSetLayout(s_dev, &dl, nullptr, &s_dslUbo);
	VkDescriptorSetLayoutBinding tb[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
	                                      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT}};
	dl.bindingCount = 2; dl.pBindings = tb;
	vkCreateDescriptorSetLayout(s_dev, &dl, nullptr, &s_dslTex);
	VkDescriptorSetLayout sets[2] = {s_dslUbo, s_dslTex};
	VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
	VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 2, sets, 1, &pc};
	vkCreatePipelineLayout(s_dev, &lci, nullptr, &s_layout);
	s_vs = shader(spv_ng_vert, sizeof spv_ng_vert);
	s_fs = shader(spv_ng_frag, sizeof spv_ng_frag);
	s_blitVs = shader(spv_blit_vert, sizeof spv_blit_vert);
	s_blitFs = shader(spv_blit_frag, sizeof spv_blit_frag);
	s_nearestClamp = sampler_for(false, false, true, true, false);
	s_linearClamp = sampler_for(true, true, true, true, false);

	for (Ctx &c : s_ctx) {
		VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, s_qfam};
		vkCreateCommandPool(s_dev, &pci, nullptr, &c.pool);
		VkCommandBuffer cbs[2];
		VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, c.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
		vkAllocateCommandBuffers(s_dev, &ai, cbs);
		c.init = cbs[0]; c.main = cbs[1];
		VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		vkCreateFence(s_dev, &fci, nullptr, &c.fence);
		c.stream = buf_create(kStream, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
		                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
		VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets * 2}};
		VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, kSets + 1, 2, ps};
		vkCreateDescriptorPool(s_dev, &dpi, nullptr, &c.dpool);
	}
	s_ci = 0;
	ctx_begin();
	u32 white = 0xFFFFFFFF;
	s_dummy = upload_image(&white, 1, 1);
	if (s_glSems) {
		s_disp.vkDone = shared_semaphore(&s_disp.glVkDone);
		s_disp.glDone = shared_semaphore(&s_disp.glGlDone);
		if (!s_disp.vkDone || !s_disp.glDone) s_glSems = false;
	}
	if (!disp_create()) return false;
	ctx_submit();
	NG_LOGI("GE: native Vulkan renderer (%s, Vulkan %u.%u), internal resolution x%d%s", props.deviceName,
	        VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), s_scale,
	        s_glSems ? "" : " (OpenGL waits for each frame: no shared semaphores)");
	return true;
}

extern "C" int ngpu_gl_init(void *(*get_proc)(const char *), int scale);
extern "C" void ngpu_register(void);
extern "C" void ngpu_use_vulkan(int on);
extern "C" int ngvk_gl_init(void *(*get_proc)(const char *), int scale) {
	// OpenGL first: it shows the frames (and is the fallback)
	int r = ngpu_gl_init(get_proc, scale);
	if (r) return r;
	s_scale = std::max(1, std::min(scale, 8));
	if (!vk_init(get_proc)) {
		NG_LOGE("Vulkan isn't available: using the OpenGL renderer");
		ngpu_use_vulkan(0);
		ngpu_register();
		return 0;
	}
	g_ng_vk = true;
	s_ready = true;
	return 0;
}

extern "C" void ngvk_set_internal_scale(int scale) {
	ng_vk_sync();
	scale = std::max(1, std::min(scale, 8));
	if (scale == s_scale || !s_ready) { s_scale = scale; return; }
	// framebuffers keep their contents (rescaled) at the new size
	for (VFb *f : s_fbs) {
		VFb old = *f;
		target_create(&f->color, &f->depth, &f->fbo, f->w * scale, f->h * scale);
		int s4[4] = {0, 0, old.w * s_scale, old.h * s_scale}, d4[4] = {0, 0, f->w * scale, f->h * scale};
		blit_image(old.color.img, f->color.img, VK_IMAGE_ASPECT_COLOR_BIT, s4, d4, VK_FILTER_LINEAR);
		throw_away(old.color, old.fbo);
		throw_away(old.depth);
	}
	barrier(C().main);
	for (VView &v : s_view) {
		if (v.fbo) { throw_away(v.color, v.fbo); throw_away(v.depth); }
		v = {};
	}
	s_scale = scale;
	ctx_submit();
	if (!disp_create()) NG_LOGE("Vulkan: the shared image couldn't be made again");
}

extern "C" void ngvk_set_options(int filter, int aniso) { ng_vk_sync(); s_filter = filter; s_aniso = aniso; }
extern "C" void ngvk_set_view_offset(float x, float y) { ng_vk_sync(); s_viewOff[0] = x; s_viewOff[1] = y; }
extern "C" void ngvk_set_display(uint32_t addr, int stride, int fmt) {
	ng_vk_sync();
	s_dispAddr = 0x04000000 | (addr & 0x1FFFFF);
	s_dispStride = stride > 0 ? stride : 512;
	s_dispFmt = fmt & 3;
	ngpu_set_display(addr, stride, fmt);   // (OpenGL shows the display memory until Vulkan has drawn it)
}
extern "C" void ngvk_views_layout(int n, const int *dst, const int *src) {
	ng_vk_sync();
	s_viewN = std::max(0, std::min(n, MAX_VIEWS));
	memcpy(s_viewDst, dst, sizeof(int) * 4 * s_viewN);
	memcpy(s_viewSrc, src, sizeof(int) * 4 * s_viewN);
}
extern "C" void ngvk_memory_written(uint32_t addr, uint32_t size) {
	ng_vk_sync();
	if ((addr & 0x0F000000) == 0x04000000) memory_written(addr, size);
}
extern "C" void ngvk_tex_cache_clear(void) {
	ng_vk_sync();
	if (s_ready)
		for (auto &e : s_texCache) throw_away(e.second.im);
	s_texCache.clear();
	ng_tex_forget_replacements();
}
