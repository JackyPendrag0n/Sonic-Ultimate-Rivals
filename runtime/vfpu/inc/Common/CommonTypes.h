#pragma once
#include <cstdint>
#include <cstddef>
#include "ppsspp_config.h"
typedef uint8_t u8; typedef int8_t s8; typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32; typedef uint64_t u64; typedef int64_t s64;
typedef u32 u32_le; typedef u16 u16_le; typedef s32 s32_le; typedef float float_le;
#ifndef CDECL
#define CDECL
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#endif
#define NO_INLINE __attribute__((noinline))
#define FORCE_INLINE inline __attribute__((always_inline))
