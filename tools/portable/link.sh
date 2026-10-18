#!/bin/bash
# Link dist/rcsm: static FFmpeg (minimal), static SDL2 (backends dlopen'd), static libpng/zlib/libstdc++.
# Needs FFMPEG_MIN and SDL2_MIN install prefixes. Result depends only on glibc >= 2.35.
set -e
cd "$(dirname "$0")/../.."
F=$FFMPEG_MIN/lib
mkdir -p dist
gcc -std=c99 -O2 -c tools/portable/glibc_compat.c -o build/glibc_compat.o
g++ -o dist/rcsm build/obj/*.o build/glibc_compat.o build/libpsputility.a build/libpspgpu.a build/libpspmpeg.a build/libpspaudio.a \
  $F/libavformat.a $F/libavcodec.a $F/libswscale.a $F/libswresample.a $F/libavutil.a "$SDL2_MIN/lib/libSDL2.a" \
  /usr/lib/x86_64-linux-gnu/libpng16.a /usr/lib/x86_64-linux-gnu/libz.a \
  -static-libstdc++ -static-libgcc -Wl,--wrap=fmod,--wrap=fmodf,--wrap=hypot,--wrap=arc4random -lm -ldl -lpthread -lrt
strip dist/rcsm
echo "glibc needed: $(objdump -T dist/rcsm | grep -oE 'GLIBC_[0-9.]+' | sort -t. -k2,2n -k3,3n -u | tail -1)"
