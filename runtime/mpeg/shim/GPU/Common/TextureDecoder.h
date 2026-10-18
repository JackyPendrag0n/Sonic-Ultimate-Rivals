// Shim for PPSSPP's GPU/Common/TextureDecoder.h (rcsm mpeg extraction): MediaEngine swizzles decoded
// frames when the game asks for it. Implemented (weakly) in mpeg_hle.cpp; libpspgpu.a has the same
// function. GPL-2.0+.
#pragma once
#include "Common/CommonTypes.h"
void DoSwizzleTex16(const u32 *ysrcp, u8 *texptr, int bxc, int byc, u32 pitch);
