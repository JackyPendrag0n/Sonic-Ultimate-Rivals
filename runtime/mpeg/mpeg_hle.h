// C API of the rcsm movie HLE (libpspmpeg.a: PPSSPP's sceMpeg + MediaEngine on FFmpeg).
// Link: build/libpspmpeg.a build/libpspaudio.a $(pkg-config --libs libavformat libavcodec libswscale
//       libswresample libavutil) -lpthread   (C++: link with g++)
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Registers every sceMpeg* NID PPSSPP implements. Call after audio_hle_init().
void mpeg_hle_init(void);

// A ~1 s black 480x272 PSMF movie (H.264 + silent ATRAC3+), to stand in for missing .PMF files.
const unsigned char *mpeg_fake_pmf(unsigned *size);

#ifdef __cplusplus
}
#endif
