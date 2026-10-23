#!/bin/bash
# Build the Windows .exe on Linux with MinGW-w64:   tools/cross-windows.sh "/path/to/Sonic Rivals 2.iso"
#
# Needs: the MinGW-w64 cross compilers (Debian/Ubuntu: sudo apt install mingw-w64 nasm curl make), python3.
# The first run downloads and builds the Windows libraries (SDL2, a small FFmpeg with only the decoders the
# game needs, libpng, zlib) into build/win-deps; later runs reuse them. Everything is linked statically,
# so build/sr2recomp.exe runs on its own (copy the game folder along, see README.md).
# The ISO/folder argument is the same as for build.sh. Building natively on Windows (MSYS2) needs none of this.
set -e
cd "$(dirname "$0")/.."
HOST=x86_64-w64-mingw32
DEPS=$PWD/build/win-deps
SRC=$DEPS/src
JOBS=${JOBS:-$(nproc)}
SDL_VER=2.30.9 ZLIB_VER=1.3.1 PNG_VER=1.6.43 FFMPEG_VER=6.1.2

for t in $HOST-gcc-posix $HOST-g++-posix $HOST-ar nasm curl make; do
  command -v $t >/dev/null || { echo "missing: $t (Debian/Ubuntu: sudo apt install mingw-w64 nasm curl make)"; exit 1; }
done
mkdir -p "$SRC" "$DEPS/lib/pkgconfig" "$DEPS/include"
get() { [ -f "$SRC/$2" ] || curl -fL --retry 3 -o "$SRC/$2" "$1"; }

if [ ! -f "$DEPS/lib/libSDL2.a" ]; then
  get https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-devel-$SDL_VER-mingw.tar.gz sdl2.tar.gz
  tar -C "$SRC" -xf "$SRC/sdl2.tar.gz"
  cp -r "$SRC/SDL2-$SDL_VER/$HOST/"* "$DEPS/"
  sed -i "s|^prefix=.*|prefix=$DEPS|" "$DEPS/lib/pkgconfig/sdl2.pc"
fi
if [ ! -f "$DEPS/lib/libz.a" ]; then
  get https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.gz zlib.tar.gz
  tar -C "$SRC" -xf "$SRC/zlib.tar.gz"
  make -C "$SRC/zlib-$ZLIB_VER" -f win32/Makefile.gcc PREFIX=$HOST- -j"$JOBS" libz.a
  make -C "$SRC/zlib-$ZLIB_VER" -f win32/Makefile.gcc install BINARY_PATH="$DEPS/bin" INCLUDE_PATH="$DEPS/include" \
    LIBRARY_PATH="$DEPS/lib" SHARED_MODE=0
fi
if [ ! -f "$DEPS/lib/libpng.a" ]; then
  get https://download.sourceforge.net/libpng/libpng-$PNG_VER.tar.xz libpng.tar.xz
  tar -C "$SRC" -xf "$SRC/libpng.tar.xz"
  (cd "$SRC/libpng-$PNG_VER" && ./configure --host=$HOST --prefix="$DEPS" --disable-shared --enable-static \
     CPPFLAGS="-I$DEPS/include" LDFLAGS="-L$DEPS/lib" && make -j"$JOBS" && make install)
fi
if [ ! -f "$DEPS/lib/libavcodec.a" ]; then
  get https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz ffmpeg.tar.xz
  tar -C "$SRC" -xf "$SRC/ffmpeg.tar.xz"
  # only what the game uses: H.264 video (PMF/MPEG-PS), ATRAC3/ATRAC3+ and MP3/AAC audio
  (cd "$SRC/ffmpeg-$FFMPEG_VER" && ./configure --prefix="$DEPS" --enable-cross-compile --target-os=mingw32 --arch=x86_64 \
     --cross-prefix=$HOST- --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect \
     --enable-static --disable-shared --enable-w32threads \
     --enable-decoder=h264,atrac3,atrac3p,atrac3al,atrac3pal,mp3,mp3float,aac,pcm_s16le \
     --enable-parser=h264,mpegaudio,aac --enable-demuxer=mpegps,mpegvideo,h264,mp3,aac,oma,wav,mov \
     --enable-protocol=file --enable-swscale --enable-swresample && make -j"$JOBS" && make install)
fi

export CC=$HOST-gcc-posix CXX=$HOST-g++-posix AR=$HOST-ar PKG_CONFIG_LIBDIR=$DEPS/lib/pkgconfig PKG_CONFIG_PATH= WIN_STATIC=1
exec ./build.sh "$@"
