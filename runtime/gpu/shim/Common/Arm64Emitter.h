// Shim for PPSSPP's Common/Arm64Emitter.h (ARM64 builds: Apple Silicon, ARM Linux), like x64Emitter.h here:
// the vertex decoder JIT is disabled, so its code block never emits code. GPL-2.0+.
#pragma once
#include <string>
#include <vector>
#include "Common/CommonTypes.h"
#include "Common/FakeEmitter.h"
namespace Arm64Gen {
typedef FakeGen::FakeXCodeBlock ARM64CodeBlock;
struct ARM64FloatEmitter {
	template <class T> explicit ARM64FloatEmitter(T *) {}
};
}
std::vector<std::string> DisassembleArm64(const u8 *data, int size);
