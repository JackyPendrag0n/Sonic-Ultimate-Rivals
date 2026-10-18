#!/bin/bash
# Builds PPSSPP's PSP system utility dialogs (savedata, message, on-screen keyboard, placeholders),
# their sceUtility HLE and the PPGe dialog renderer (GPL-2.0+, see LICENSE.TXT) into
# build/libpsputility.a, plus the standalone test build/test_utility.
#
# Uses runtime/audio's shims and HLE bridge and runtime/gpu's sceGeListEnQueue, so link it before
# those libraries:
#
#   build/libpsputility.a build/libpspmpeg.a build/libpspaudio.a build/libpspgpu.a \
#       $(pkg-config --libs libavformat libavcodec libswscale libswresample libavutil) -lpng -lz -lpthread
#
#   runtime/utility/build.sh          build libpsputility.a and test_utility
#   runtime/utility/build.sh lib      library only
#   runtime/utility/build.sh clean
set -e
UTL="$(cd "$(dirname "$0")" && pwd)"
AUD="$(cd "$UTL/../audio" && pwd)"
ROOT="$(cd "$UTL/../.." && pwd)"
OUT="$ROOT/build"
OBJ="$OUT/utility_obj"
CXX="${CXX:-g++}"
CC="${CC:-gcc}"
JOBS="$(nproc)"

if [ "$1" = clean ]; then rm -rf "$OBJ" "$OUT/libpsputility.a" "$OUT/test_utility"; exit 0; fi

# The utility shims first, then the audio library's (HLE bridge ABI), then the PPSSPP copies.
INCLUDES="-I$UTL/shim -I$AUD/shim -I$UTL/ppsspp -iquote $UTL/ppsspp/Common $(pkg-config --cflags libpng)"
DEFINES="-DMASKED_PSP_MEMORY -DNDEBUG"
CXXFLAGS="${CXXFLAGS:--O2} -std=c++17 -g0 -fPIC -msse2 -fno-strict-aliasing -Wno-unused-result -Wno-deprecated-declarations -Wno-class-memaccess -Wno-format-truncation"
CFLAGS="${CFLAGS:--O2} -std=gnu11 -g0 -fPIC -w"

SOURCES="
ppsspp/Core/Dialog/PSPDialog.cpp
ppsspp/Core/Dialog/PSPSaveDialog.cpp
ppsspp/Core/Dialog/PSPMsgDialog.cpp
ppsspp/Core/Dialog/PSPOskDialog.cpp
ppsspp/Core/Dialog/PSPOskConstants.cpp
ppsspp/Core/Dialog/PSPPlaceholderDialog.cpp
ppsspp/Core/Dialog/SavedataParam.cpp
ppsspp/Core/Util/PPGeDraw.cpp
ppsspp/Core/ELF/ParamSFO.cpp
ppsspp/Core/HLE/sceChnnlsv.cpp
ppsspp/Common/Render/TextureAtlas.cpp
ppsspp/Common/Data/Format/ZIMLoad.cpp
ppsspp/Common/Data/Format/PNGLoad.cpp
ppsspp/Common/Data/Encoding/Utf8.cpp
ppsspp/Common/Data/Text/WrapText.cpp
ppsspp/ext/libkirk/AES.c
ppsspp/ext/libkirk/SHA1.c
ppsspp/ext/libkirk/amctrl.c
ppsspp/ext/libkirk/bn.c
ppsspp/ext/libkirk/ec.c
ppsspp/ext/libkirk/kirk_engine.c
utility_hle.cpp
utility_fs.cpp
utility_support.cpp
"

mkdir -p "$OBJ"
objname() { echo "$OBJ/$(echo "$1" | sed 's|/|_|g; s|\.c$|.o|; s|\.cpp$|.o|')"; }
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
	case "$src" in
	*.c) $CC $CFLAGS $DEFINES $INCLUDES -MMD -MF "$d" -c "$UTL/$src" -o "$o" ;;
	*) $CXX $CXXFLAGS $DEFINES $INCLUDES -MMD -MF "$d" -c "$UTL/$src" -o "$o" ;;
	esac
}
export -f compile objname
export OBJ UTL CXX CC CXXFLAGS CFLAGS DEFINES INCLUDES

echo "$SOURCES" | grep -v '^$' | xargs -P "$JOBS" -I{} bash -c 'compile {}'

OBJS=""
for s in $SOURCES; do OBJS="$OBJS $(objname "$s")"; done
rm -f "$OUT/libpsputility.a"
ar rcs "$OUT/libpsputility.a" $OBJS
echo "built $OUT/libpsputility.a"

[ "$1" = lib ] && exit 0

for l in gpu audio mpeg; do [ -f "$OUT/libpsp$l.a" ] || "$ROOT/runtime/$l/build.sh" lib; done
compile test_utility.cpp
$CXX -o "$OUT/test_utility" "$(objname test_utility.cpp)" "$OUT/libpsputility.a" "$OUT/libpspmpeg.a" \
	"$OUT/libpspaudio.a" "$OUT/libpspgpu.a" \
	$(pkg-config --libs libavformat libavcodec libswscale libswresample libavutil) -lpng -lz -lpthread
echo "built $OUT/test_utility"
