// Native GE renderer (backend 2): the runtime's own display list processor and OpenGL renderer, written for
// what Sonic Rivals 2 uses. PPSSPP's renderers stay as backends 0 and 1 (runtime/gpu).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Fills in the GE backend's hook table (gpu/ge_soft.h: g_ge_native), so that renderer 2 can be selected.
void ngpu_register(void);
// Renderer 3: the same renderer drawing with Vulkan (ng_vk.cpp; OpenGL still shows the frames). Call before
// ngpu_register(); if Vulkan can't be used, it falls back to OpenGL by itself.
void ngpu_use_vulkan(int on);

// Shadow mode (SR2_NGPU_SHADOW=1 with the PPSSPP OpenGL backend): PPSSPP runs the lists and drives the
// game, and every command it executes is also fed to the native renderer, for comparisons. offset is the
// list's current offset address (ORIGIN/OFFSETADDR, restored by RET), which the native side can't see.
extern void (*g_ngpu_shadow_op)(uint32_t pc, uint32_t op, uint32_t offset);
void ngpu_shadow_enable(void);

// Feature census (SR2_NGPU_CENSUS=file): every distinct render state the game draws with, written at exit.
void ngpu_census_write(void);

#ifdef __cplusplus
}
#endif
