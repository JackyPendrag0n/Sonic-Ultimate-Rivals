// Wrapper around the system <libavformat/avformat.h> (rcsm mpeg extraction).
//
// PPSSPP's MediaEngine feeds FFmpeg through an AVIOContext whose read callback returns 0 when the
// ring buffer has no more data. FFmpeg up to 4.x took 0 as end of file; FFmpeg 6.x requires
// AVERROR_EOF and otherwise spins forever in the MPEG-PS demuxer. avio_alloc_context is redirected
// to mpeg_hle.cpp, which wraps the callback to restore the old meaning (0 -> AVERROR_EOF).
// GPL-2.0+.
#pragma once
#include_next <libavformat/avformat.h>
#include <libavformat/avio.h>

#ifdef __cplusplus
extern "C" {
#endif
extern decltype(avio_alloc_context) rcsm_avio_alloc_context;
#ifdef __cplusplus
}
#endif
#define avio_alloc_context rcsm_avio_alloc_context
