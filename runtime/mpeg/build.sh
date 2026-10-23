#!/bin/bash
# Builds PPSSPP's sceMpeg HLE + MediaEngine (GPL-2.0+, see LICENSE.TXT) on the system FFmpeg into
# build/libpspmpeg.a, plus the standalone test build/test_mpeg. Uses runtime/audio's shims and HLE
# bridge, so libpspaudio.a must be built first (runtime/audio/build.sh lib) and linked after this one:
#
#   build/libpspmpeg.a build/libpspaudio.a $(pkg-config --libs libavformat libavcodec libswscale \
#       libswresample libavutil) -lpthread
#
#   runtime/mpeg/build.sh          build libpspmpeg.a and test_mpeg
#   runtime/mpeg/build.sh lib      library only
#   runtime/mpeg/build.sh clean
set -e
MPG="$(cd "$(dirname "$0")" && pwd)"
AUD="$(cd "$MPG/../audio" && pwd)"
ROOT="$(cd "$MPG/../.." && pwd)"
OUT="$ROOT/build"
OBJ="$OUT/mpeg_obj"
CXX="${CXX:-g++}"
# x86 builds use SSE2; other CPUs (Apple Silicon, ARM Linux) take PPSSPP's NEON/plain paths
case "$($CXX -dumpmachine)" in x86_64*|i?86*) SIMDFLAGS="-msse2";; *) SIMDFLAGS="";; esac
case "$($CXX -dumpmachine)" in *mingw*|*windows*) PICFLAG="";; *) PICFLAG="-fPIC";; esac
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"
FFLIBS="libavformat libavcodec libswscale libswresample libavutil"

if [ "$1" = clean ]; then rm -rf "$OBJ" "$OUT/libpspmpeg.a" "$OUT/test_mpeg"; exit 0; fi

# The movie's own shims first, then the audio library's (HLE bridge ABI), then the PPSSPP copies.
INCLUDES="-I$MPG/shim -I$AUD/shim -I$MPG/ppsspp -iquote $MPG/ppsspp/Common $(pkg-config --cflags $FFLIBS)"
DEFINES="-DMASKED_PSP_MEMORY -DNDEBUG -DUSE_FFMPEG"
# -fpermissive: PPSSPP assigns avcodec_find_decoder()'s const AVCodec* to AVCodec* (FFmpeg >= 5).
CXXFLAGS="${CXXFLAGS:--O2} -std=c++17 -g0 $PICFLAG $SIMDFLAGS -fno-strict-aliasing -fpermissive -w"

SOURCES="
ppsspp/Core/HLE/sceMpeg.cpp
ppsspp/Core/HW/MediaEngine.cpp
ppsspp/Core/HW/MpegDemux.cpp
ppsspp/Core/HW/SimpleAudioDec.cpp
mpeg_hle.cpp
"

mkdir -p "$OBJ"
objname() { echo "$OBJ/$(echo "$1" | sed 's|/|_|g; s|\.cpp$|.o|')"; }
compile() {
	local src="$1" o; o="$(objname "$src")"
	local d="${o%.o}.d"
	if [ -f "$o" ] && [ -f "$d" ]; then
		local deps; deps=$(sed -e 's/^[^:]*://' -e 's/\\$//' "$d")
		local stale=0
		for f in $deps; do [ "$f" -nt "$o" ] && { stale=1; break; }; done
		[ $stale = 0 ] && return 0
	fi
	echo "  CC  $src"
	$CXX $CXXFLAGS $DEFINES $INCLUDES -MMD -MF "$d" -c "$MPG/$src" -o "$o"
}
export -f compile objname
export OBJ MPG CXX CXXFLAGS DEFINES INCLUDES

echo "$SOURCES" | grep -v '^$' | xargs -P "$JOBS" -I{} bash -c 'compile {}'

OBJS=""
for s in $SOURCES; do OBJS="$OBJS $(objname "$s")"; done
rm -f "$OUT/libpspmpeg.a"
${AR:-ar} rcs "$OUT/libpspmpeg.a" $OBJS
echo "built $OUT/libpspmpeg.a"

[ "$1" = lib ] && exit 0

[ -f "$OUT/libpspaudio.a" ] || "$AUD/build.sh" lib
compile test_mpeg.cpp
$CXX -o "$OUT/test_mpeg" "$(objname test_mpeg.cpp)" "$OUT/libpspmpeg.a" "$OUT/libpspaudio.a" \
	$(pkg-config --libs $FFLIBS) -lz -lpthread
echo "built $OUT/test_mpeg"
# Test movie: 2 s of FFmpeg's test pattern (needs the ffmpeg CLI with libx264).
[ -f "$OUT/test_clip.pmf" ] || python3 -I "$MPG/tools/make_pmf.py" "$OUT/test_clip.pmf" --seconds 2 --source testsrc
