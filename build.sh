#!/bin/bash
# Build the native port from YOUR copy of the game.
#
#   ./build.sh "/path/to/Sonic Rivals 2.iso"   # your own dump of the UMD: unpacked into ./game first
#   ./build.sh /path/to/game                   # or a folder that already contains PSP_GAME (SYSDIR + USRDIR)
#
# Works on Linux, on macOS (Homebrew) and on Windows in an MSYS2 UCRT64 or MINGW64 shell; see README.md.
# It recompiles the game's program (SYSDIR/BOOT.BIN, or a decrypted SYSDIR/EBOOT.BIN) into C under gen/,
# builds the runtime libraries, and links build/sr2recomp (build/sr2recomp.exe on Windows). Then run:
#   build/sr2recomp "/path/to/game"     (with no argument it looks in ./game and next to the program)
#
# Options (environment): JOBS=N (default: number of CPUs), MARCH=... (x86-64 default: x86-64-v2, which runs
# on any x86-64 CPU from about 2009 on; MARCH=native is faster on the build machine but the binary then only
# runs on CPUs with the same features; Apple Silicon and ARM: the compiler's default), CC/CXX (compilers),
# SKIP_MODULES="rtpspnetah rtpspnetin" (default; Sony network libraries for multiplayer use a relocation
# format the recompiler doesn't handle yet).
# Cross-building the Windows .exe on Linux: tools/cross-windows.sh (it calls this script).
set -e
cd "$(dirname "$0")"
GAME=${1:-${GAME:-game}}
case "$GAME" in
  *.iso|*.ISO)
    [ -f "$GAME" ] || { echo "no such file: $GAME"; exit 2; }
    PY=$(command -v python3 || command -v python) || { echo "missing: python3"; exit 1; }
    "$PY" -I tools/setup_iso.py "$GAME" game
    GAME=game;;
esac
[ -d "$GAME" ] || { echo "usage: $0 /path/to/game.iso   or   $0 /path/to/game (folder containing PSP_GAME)"; exit 2; }
GAME=$(cd "$GAME" && pwd -P)
case "$GAME" in */PSP_GAME|*/psp_game) GAME=$(dirname "$GAME");; esac
SKIP_MODULES=${SKIP_MODULES:-"rtpspnetah rtpspnetin"}

# ---------------------------------------------------------------- platform
case "$(uname -s)" in
  Darwin) OS=macos; NCPU=$(sysctl -n hw.ncpu)
    export CC=${CC:-clang} CXX=${CXX:-clang++};;
  MINGW*|MSYS*|CYGWIN*) OS=windows; NCPU=$(nproc)
    export CC=${CC:-gcc} CXX=${CXX:-g++};;
  *) OS=linux; NCPU=$(nproc)
    export CC=${CC:-gcc} CXX=${CXX:-g++};;
esac
# (a cross compiler decides the target: CC=x86_64-w64-mingw32-gcc builds the Windows .exe on Linux)
case "$($CC -dumpmachine)" in *mingw*|*windows*) TARGET=windows;; *apple*|*darwin*) TARGET=macos;; *) TARGET=linux;; esac
case "$($CC -dumpmachine)" in
  x86_64*|amd64*) MARCH=${MARCH:-x86-64-v2};;
  *) MARCH=${MARCH:-};;
esac
ARCHFLAG=${MARCH:+-march=$MARCH}
export JOBS=${JOBS:-$NCPU}

need() { command -v "$1" >/dev/null || { echo "missing: $1 ($2)"; exit 1; }; }
PY=$(command -v python3 || command -v python) || { echo "missing: python3"; exit 1; }
need "$CC" "C compiler"
need "$CXX" "C++ compiler"
need pkg-config "pkg-config"
for p in sdl2 libavcodec libavformat libavutil libswscale libswresample libpng zlib; do
  pkg-config --exists $p || { echo "missing development package for '$p'."
    case $OS in
      linux)
        echo "Debian/Ubuntu: sudo apt install build-essential pkg-config libsdl2-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libpng-dev zlib1g-dev"
        echo "Fedora:        sudo dnf install gcc-c++ pkgconf SDL2-devel ffmpeg-free-devel libpng-devel zlib-devel"
        echo "Arch:          sudo pacman -S base-devel sdl2 ffmpeg libpng zlib";;
      macos)
        echo "macOS: install Homebrew (https://brew.sh), then:  brew install pkg-config sdl2 ffmpeg libpng";;
      windows)
        echo "MSYS2 (UCRT64 shell): pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,pkgconf,SDL2,ffmpeg,libpng,zlib} python";;
    esac
    [ $TARGET = windows ] && [ $OS = linux ] && echo "(cross build: run tools/cross-windows.sh, it builds these for Windows)"
    exit 1; }
done

# find a path case-insensitively below $GAME
ci() { find "$GAME" -ipath "$GAME/$1" -print -quit 2>/dev/null; }
is_elf() { [ "$(head -c 4 "$1" | od -A n -t x1 | tr -d ' ')" = "7f454c46" ]; }

mkdir -p data build
tmp=$(mktemp)
eboot=$(ci PSP_GAME/SYSDIR/EBOOT.BIN)
if [ -z "$eboot" ] || ! is_elf "$eboot"; then
  # The disc's EBOOT.BIN is encrypted, but its BOOT.BIN is the same program unencrypted.
  boot=$(ci PSP_GAME/SYSDIR/BOOT.BIN)
  if [ -n "$boot" ] && is_elf "$boot"; then eboot=$boot
  else echo "PSP_GAME/SYSDIR/BOOT.BIN is missing (and EBOOT.BIN is encrypted). Use a complete dump of the disc."; exit 1; fi
fi
echo "$eboot sr2" > "$tmp"
while IFS= read -r f; do
  n=$(basename "$f"); n=${n%.*}; n=$(echo "$n" | tr 'A-Z' 'a-z' | tr -c 'a-z0-9_\n' '_')
  skip=0; for s in $SKIP_MODULES; do [ "$n" = "$s" ] && skip=1; done
  [ $skip = 1 ] && { echo "skipping $f (network module)"; continue; }
  is_elf "$f" || { echo "skipping $f (not a plain ELF - encrypted?)"; continue; }
  echo "$f $n" >> "$tmp"
done < <(find "$GAME" \( -ipath "*/PSP_GAME/USRDIR/BIN/*.prx" -o -ipath "*/PSP_GAME/USRDIR/MODULES/*.prx" \) | sort)
mv "$tmp" data/modules.txt
echo "modules: $(wc -l < data/modules.txt)   target: $TARGET ${MARCH:-(default CPU)}"

# a different target or MARCH needs the libraries rebuilt
[ "$(cat build/march 2>/dev/null)" = "$TARGET $MARCH" ] || rm -rf build/libpsp*.a build/*_obj build/obj
echo "$TARGET $MARCH" > build/march

# recompile the game's code first: the libraries include the generated runtime/interp_list.h
tools/build.sh --gen

# runtime libraries (PPSSPP-derived GPU, audio, video, system dialogs)
LIBFLAGS="-O3 $ARCHFLAG"
case $TARGET in
  windows) LIBFLAGS="$LIBFLAGS -D_FILE_OFFSET_BITS=64 -DUNICODE -D_UNICODE -I$PWD/runtime/win32 -include $PWD/runtime/win32/compat.h";;
  macos) LIBFLAGS="$LIBFLAGS -I$PWD/runtime/macos -DGL_SILENCE_DEPRECATION -Wno-deprecated-declarations";;
esac
for lib in gpu audio mpeg utility; do
  if [ ! -f build/libpsp$lib.a ]; then
    echo "building libpsp$lib.a ..."
    CXXFLAGS="$LIBFLAGS" runtime/$lib/build.sh lib > build/lib$lib.log 2>&1 || { tail -20 build/lib$lib.log; exit 1; }
  fi
done

GENOPT="-O1 $ARCHFLAG -foptimize-sibling-calls -fno-strict-aliasing -fwrapv -Iruntime -w" \
OPT="-O2 $ARCHFLAG" tools/build.sh
EXE=; [ $TARGET = windows ] && EXE=.exe
echo
echo "Done. Run:  build/sr2recomp$EXE \"$GAME\""
