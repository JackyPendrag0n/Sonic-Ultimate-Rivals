// Includes the rcsm kernel API (../kernel.h) into a translation unit that also uses PPSSPP headers.
// rt.h's u64/s64 are uint64_t/int64_t (unsigned long on LP64) while PPSSPP's are (unsigned) long long,
// so the kernel's typedefs are renamed here; the types are layout-identical and all kernel functions
// have C linkage. Include this after the PPSSPP headers: kernel.h defines short macros (A, RET, LW...).
// GPL-2.0+.
#pragma once
#define u64 rt_u64
#define s64 rt_s64
#include "../../kernel.h"
#undef u64
#undef s64
