#!/bin/bash
# Builds PPSSPP's SAS mixer and ATRAC3/ATRAC3+ decoder with their HLE layers (GPL-2.0+, see
# LICENSE.TXT) into build/libpspaudio.a, plus the standalone test build/test_audio.
#
#   runtime/audio/build.sh          build libpspaudio.a and test_audio
#   runtime/audio/build.sh lib      library only
#   runtime/audio/build.sh clean
#
# Link into the runtime with: build/libpspaudio.a -lpthread (C++, so link with g++).
# Support code also present in libpspgpu.a (logging, Memory:: helpers, CPU detection, ...) is
# weakened in this library, so both can be linked into one binary.
set -e
AUD="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$AUD/../.." && pwd)"
OUT="$ROOT/build"
OBJ="$OUT/audio_obj"
CXX="${CXX:-g++}"
JOBS="$(nproc)"

if [ "$1" = clean ]; then rm -rf "$OBJ" "$OUT/libpspaudio.a" "$OUT/test_audio"; exit 0; fi

INCLUDES="-I$AUD/shim -I$AUD/ppsspp -iquote $AUD/ppsspp/Common"
DEFINES="-DMASKED_PSP_MEMORY -DNDEBUG"
CXXFLAGS="${CXXFLAGS:--O2} -std=c++17 -g0 -fPIC -msse2 -fno-strict-aliasing -Wno-unused-result -Wno-deprecated-declarations -Wno-class-memaccess -Wno-format-truncation"

SOURCES="
ppsspp/Core/HLE/sceSas.cpp
ppsspp/Core/HW/SasAudio.cpp
ppsspp/Core/HW/SasReverb.cpp
ppsspp/Core/HLE/sceAtrac.cpp
ppsspp/Core/HLE/AtracCtx.cpp
ppsspp/Core/HLE/AtracCtx2.cpp
ppsspp/Core/Util/AtracTrack.cpp
ppsspp/Core/HW/Atrac3Standalone.cpp
ppsspp/Core/Util/AudioFormat.cpp
ppsspp/Core/HW/BufferQueue.cpp
ppsspp/ext/at3_standalone/atrac.cpp
ppsspp/ext/at3_standalone/atrac3.cpp
ppsspp/ext/at3_standalone/atrac3plus.cpp
ppsspp/ext/at3_standalone/atrac3plusdec.cpp
ppsspp/ext/at3_standalone/atrac3plusdsp.cpp
ppsspp/ext/at3_standalone/compat.cpp
ppsspp/ext/at3_standalone/fft.cpp
ppsspp/ext/at3_standalone/get_bits.cpp
ppsspp/ext/at3_standalone/mem.cpp
audio_hle.cpp
"
# Support objects: all their global symbols are made weak (see above).
SUPPORT="
ppsspp/Common/Data/Format/RIFF.cpp
stubs/stubs.cpp
stubs/cpu_detect.cpp
"

mkdir -p "$OBJ"

objname() { echo "$OBJ/$(echo "$1" | sed 's|/|_|g; s|\.cpp$|.o|')"; }

# Compile one source if it (or a header it depends on) is newer than its object.
compile() {
	local src="$1" weaken="$2" o; o="$(objname "$src")"
	local d="${o%.o}.d"
	if [ -f "$o" ] && [ -f "$d" ]; then
		local deps; deps=$(sed -e 's/^[^:]*://' -e 's/\\$//' "$d")
		local stale=0
		for f in $deps; do [ "$f" -nt "$o" ] && { stale=1; break; }; done
		[ $stale = 0 ] && return 0
	fi
	echo "  CC  $src"
	$CXX $CXXFLAGS $DEFINES $INCLUDES -MMD -MF "$d" -c "$AUD/$src" -o "$o"
	# Only defined symbols: weak *undefined* references wouldn't pull archive members in.
	if [ "$weaken" = weak ]; then
		nm --defined-only -g "$o" | awk 'NF==3 {print $3}' > "${o%.o}.weak"
		objcopy --weaken-symbols="${o%.o}.weak" "$o"
	fi
}
export -f compile objname
export OBJ AUD CXX CXXFLAGS DEFINES INCLUDES

{ echo "$SOURCES" | grep -v '^$' | sed 's/$/ strong/'; echo "$SUPPORT" | grep -v '^$' | sed 's/$/ weak/'; } |
	xargs -P "$JOBS" -L1 bash -c 'compile "$0" "$1"'

OBJS=""
for s in $SOURCES $SUPPORT; do OBJS="$OBJS $(objname "$s")"; done
rm -f "$OUT/libpspaudio.a"
ar rcs "$OUT/libpspaudio.a" $OBJS
echo "built $OUT/libpspaudio.a"

[ "$1" = lib ] && exit 0

compile test_audio.cpp strong
$CXX -o "$OUT/test_audio" "$(objname test_audio.cpp)" "$OUT/libpspaudio.a" -lpthread
echo "built $OUT/test_audio"
