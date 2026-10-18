// Shim for <zstd.h> (rcsm utility extraction). The embedded PPGe atlas is zlib/uncompressed, so zstd
// is never needed; renamed so it can't interpose the system libzstd that FFmpeg may load. GPL-2.0+.
#pragma once
#include <cstddef>
#define ZSTD_decompress rcsm_utility_no_zstd_decompress
size_t ZSTD_decompress(void *dst, size_t dstCapacity, const void *src, size_t compressedSize);
