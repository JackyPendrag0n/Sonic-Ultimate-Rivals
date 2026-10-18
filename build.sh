#!/bin/bash
# Build the native Linux port from YOUR copy of the game.
#
#   ./build.sh /path/to/game        # folder that contains PSP_GAME (SYSDIR + USRDIR)
#
# It recompiles every code module it finds (SYSDIR/EBOOT.BIN, USRDIR/BIN/*.PRX, USRDIR/MODULES/*.PRX)
# into C under gen/, builds the runtime libraries, and links build/sr2recomp.
# Then run:   build/sr2recomp /path/to/game
#
# Options (environment): JOBS=N (default: nproc), MARCH=... (default: native; use x86-64-v2 for an
# older-CPU-compatible build), SKIP_MODULES="rtpspnetah rtpspnetin" (default; Sony network libraries
# for multiplayer use a relocation format the recompiler doesn't handle yet).
set -e
cd "$(dirname "$0")"
GAME=${1:-${GAME:-game}}
[ -d "$GAME" ] || { echo "usage: $0 /path/to/game (folder containing PSP_GAME)"; exit 2; }
GAME=$(cd "$GAME" && pwd)
case "$GAME" in */PSP_GAME|*/psp_game) GAME=$(dirname "$GAME");; esac
MARCH=${MARCH:-native}
SKIP_MODULES=${SKIP_MODULES:-"rtpspnetah rtpspnetin"}
export JOBS=${JOBS:-$(nproc)}

need() { command -v "$1" >/dev/null || { echo "missing: $1 ($2)"; exit 1; }; }
need python3 "python3"
need gcc "gcc"
need g++ "g++"
need pkg-config "pkg-config"
for p in sdl2 libavcodec libavformat libavutil libswscale libswresample libpng zlib; do
  pkg-config --exists $p || { echo "missing development package for '$p'."
    echo "Debian/Ubuntu: sudo apt install build-essential pkg-config libsdl2-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libpng-dev zlib1g-dev"
    echo "Fedora:        sudo dnf install gcc-c++ pkgconf SDL2-devel ffmpeg-free-devel libpng-devel zlib-devel"
    echo "Arch:          sudo pacman -S base-devel sdl2 ffmpeg libpng zlib"
    exit 1; }
done

# find a path case-insensitively below $GAME
ci() { find "$GAME" -ipath "$GAME/$1" -print -quit 2>/dev/null; }
is_elf() { [ "$(head -c 4 "$1" | od -A n -t x1 | tr -d ' ')" = "7f454c46" ]; }

mkdir -p data build
tmp=$(mktemp)
eboot=$(ci PSP_GAME/SYSDIR/EBOOT.BIN)
if [ -z "$eboot" ] || ! is_elf "$eboot"; then
  boot=$(ci PSP_GAME/SYSDIR/BOOT.BIN)
  if [ -n "$boot" ] && is_elf "$boot"; then eboot=$boot
  else echo "PSP_GAME/SYSDIR/EBOOT.BIN is missing or encrypted. A decrypted EBOOT.BIN (plain ELF) is required."; exit 1; fi
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
echo "modules: $(wc -l < data/modules.txt)"

# runtime libraries (PPSSPP-derived GPU, audio, video, system dialogs)
for lib in gpu audio mpeg utility; do
  if [ ! -f build/libpsp$lib.a ]; then
    echo "building libpsp$lib.a ..."
    CXXFLAGS="-O3 -march=$MARCH" runtime/$lib/build.sh lib > build/lib$lib.log 2>&1 || { tail -20 build/lib$lib.log; exit 1; }
  fi
done

GENOPT="-O1 -march=$MARCH -foptimize-sibling-calls -fno-strict-aliasing -fwrapv -Iruntime -w" \
OPT="-O2 -march=$MARCH" tools/build.sh
echo
echo "Done. Run:  build/sr2recomp \"$GAME\""
