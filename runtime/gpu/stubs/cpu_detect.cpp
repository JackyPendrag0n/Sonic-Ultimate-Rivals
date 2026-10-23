// Replacement for PPSSPP's Common/CPUDetect.cpp (rcsm GPU extraction): the original needs the
// cpu_features submodule, which isn't vendored. Only the x86 feature bits the software renderer
// dispatches on are filled in. GPL-2.0+.
#include <cstring>
#include <unistd.h>
#include "Common/CPUDetect.h"

CPUInfo cpu_info;

CPUInfo::CPUInfo() {
	Detect();
}

void CPUInfo::Detect() {
	memset(this, 0, sizeof(*this));
	vendor = VENDOR_OTHER;
	OS64bit = CPU64bit = Mode64bit = true;
#if defined(__x86_64__) || defined(__i386__)
	strcpy(cpu_string, "x86-64");
	strcpy(brand_string, "x86-64");
	bLongMode = true;
	__builtin_cpu_init();
	bSSE = bSSE2 = true;
	bSSE3 = __builtin_cpu_supports("sse3");
	bSSSE3 = __builtin_cpu_supports("ssse3");
	bSSE4_1 = __builtin_cpu_supports("sse4.1");
	bSSE4_2 = __builtin_cpu_supports("sse4.2");
	bPOPCNT = __builtin_cpu_supports("popcnt");
	bAVX = __builtin_cpu_supports("avx");
	bAVX2 = __builtin_cpu_supports("avx2");
	bFMA3 = __builtin_cpu_supports("fma");
	bBMI1 = __builtin_cpu_supports("bmi");
	bBMI2 = __builtin_cpu_supports("bmi2");
	bLZCNT = bBMI1;
#else
	// ARM64 (Apple Silicon, ARM Linux): NEON is always there
	strcpy(cpu_string, "arm64");
	strcpy(brand_string, "arm64");
	bNEON = bASIMD = true;
#endif
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	logical_cpu_count = n > 0 ? (int)n : 1;
	num_cores = logical_cpu_count;
	HTT = false;
}

std::vector<std::string> CPUInfo::Features() { return {}; }
std::string CPUInfo::Summarize() { return cpu_string; }
const char *GetCompilerABI() { return cpu_info.cpu_string; }
