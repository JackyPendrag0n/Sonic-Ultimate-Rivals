// C API of the rcsm audio HLE (libpspaudio.a: PPSSPP's SAS mixer + ATRAC3/ATRAC3+ decoder).
// Link: build/libpspaudio.a -lpthread (C++: link with g++ or add -lstdc++)
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Registers every __sceSas* and sceAtrac* NID PPSSPP implements (overriding earlier registrations).
// Call after kernel_init() and g_mem setup. PSPAUDIO_NODELAY=1 in the environment disables the
// Media Engine processing delays PPSSPP applies to __sceSasCore / sceAtracDecodeData etc.
void audio_hle_init(void);

#ifdef __cplusplus
}
#endif
