#pragma once
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
#include <immintrin.h>
#define _M_SSE 0x402
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif
