// Shim for PPSSPP's Common/x64Emitter.h (rcsm GPU extraction). The vertex decoder JIT is disabled:
// VertexDecoderJitCache derives from a code block that never emits code, and is never constructed
// because g_Config.bVertexDecoderJit is false. VertexDecoderJitCache::Compile() is stubbed in
// stubs/stubs.cpp to return nullptr (= use the C++ step functions). GPL-2.0+.
#pragma once
#include "Common/FakeEmitter.h"
namespace Gen {
typedef FakeGen::FakeXCodeBlock XCodeBlock;
}
