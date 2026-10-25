#!/bin/bash
# Build the native binary. data/modules.txt lists "<path to ELF/PRX> <c name>" per line.
set -e
cd "$(dirname "$0")/.."
OPT=${OPT:--O2}
CC=${CC:-gcc}
CXX=${CXX:-g++}
# Target: Linux, macOS or Windows (MinGW-w64, native under MSYS2 or cross from Linux)
case "$($CC -dumpmachine)" in
  *mingw*|*windows*) TARGET=windows;;
  *apple*|*darwin*) TARGET=macos;;
  *) TARGET=linux;;
esac
EXE=; PLAT_CFLAGS=; PLAT_LIBS="-lpthread -ldl"
case $TARGET in
  windows)
    EXE=.exe
    # (compat.h fills in the POSIX calls MinGW lacks; SDL_MAIN_HANDLED: our own main(), console subsystem so the log shows)
    PLAT_CFLAGS="-Iruntime/win32 -include runtime/win32/compat.h -D_FILE_OFFSET_BITS=64 -DSDL_MAIN_HANDLED"
    PLAT_LIBS="-static-libgcc -static-libstdc++ -lws2_32 -lwinmm -lopengl32 -lbcrypt -lole32 -luser32 -lsecur32 -lpthread -Wl,--stack,16777216"
    # WIN_STATIC=1 (tools/cross-windows.sh, whose libraries are static only): one .exe with no DLLs.
    # Otherwise (MSYS2) the DLLs it needs are copied next to it after linking.
    [ "$WIN_STATIC" = 1 ] && PLAT_LIBS="-static $PLAT_LIBS";;
  macos)
    # (runtime/macos: the standard GL/gl.h + glext.h, which macOS doesn't ship)
    PLAT_CFLAGS="-DGL_SILENCE_DEPRECATION -Wno-deprecated-declarations -Iruntime/macos"
    PLAT_LIBS="-lpthread -framework OpenGL";;
esac
# (the code includes <SDL2/SDL.h>: also the folder above SDL2/, which isn't a default one on Homebrew/MSYS2)
SDL_CFLAGS="$(pkg-config --cflags sdl2 | sed 's/-Dmain=SDL_main//') -I$(pkg-config --variable=includedir sdl2)"
mkdir -p build/obj gen
names=()
# --check (CI, no game files): compile the runtime only, with an empty interpreter list, and don't link
if [ "$1" = --check ]; then
  [ -f runtime/interp_list.h ] || : > runtime/interp_list.h
else
while read -r path name; do
  [ -z "$path" ] || [[ "$path" == \#* ]] && continue
  names+=("$name")
  if [ ! -f gen/$name/${name}_table.c ] || [ "$path" -nt gen/$name/${name}_table.c ] || [ tools/recomp.py -nt gen/$name/${name}_table.c ] || [ data/hooks.json -nt gen/$name/${name}_table.c ] || [ -f data/seeds_$name.txt -a data/seeds_$name.txt -nt gen/$name/${name}_table.c ]; then
    # (files are rewritten only when their content changes, so unchanged code is not recompiled)
    python3 -I tools/recomp.py "$path" $name gen/$name --extra-seeds data/seeds_$name.txt && touch gen/$name/${name}_table.c
  fi
done < data/modules.txt
python3 -I tools/gen_support.py "${names[@]}"
fi
# --gen: only the recompiled C and the generated headers (runtime/interp_list.h, which the libraries include)
[ "$1" = --gen ] && exit 0
CFLAGS="$OPT -g -fno-strict-aliasing -fwrapv -foptimize-sibling-calls -Iruntime $PLAT_CFLAGS -w"
objs=()
build() { # src obj compiler flags...
  local src=$1 obj=$2; shift 2
  local dep=runtime/kernel.h
  case $src in gen/*) dep=runtime/rt.h;; esac
  case $src in runtime/ngpu/*) dep=$(ls -t runtime/ngpu/*.h | head -1);; esac
  local dep2=runtime/config.h
  case $src in gen/*) dep2=runtime/rt.h;; esac
  if [ ! -f $obj ] || [ $src -nt $obj ] || [ runtime/rt.h -nt $obj ] || [ runtime/interp_list.h -nt $obj ] || [ $dep -nt $obj ] || [ $dep2 -nt $obj ]; then "$@" -c $src -o $obj || exit 1; fi
}
pids=(); fail=
for n in "${names[@]}"; do
  for f in gen/$n/*.c; do
    o=build/obj/$(basename ${f%.c}).o; objs+=($o)
    build $f $o $CC ${GENOPT:-$CFLAGS} $PLAT_CFLAGS & pids+=($!)
    if [ ${#pids[@]} -ge ${JOBS:-$(nproc)} ]; then wait ${pids[0]} || fail=1; pids=("${pids[@]:1}"); fi
  done
done
support=(gen/modules.c gen/nidnames.c); [ "$1" = --check ] && support=()
for f in "${support[@]}" runtime/*.c; do
  o=build/obj/$(basename ${f%.c}).o; objs+=($o)
  build $f $o $CC $CFLAGS $SDL_CFLAGS $(pkg-config --cflags libpng zlib libavcodec libavutil) -Wall -Wno-unused-function -Wno-unused-variable &
  pids+=($!)
done
for f in runtime/*.cpp runtime/imgui/*.cpp; do
  o=build/obj/cxx_$(basename ${f%.cpp}).o; objs+=($o)
  build $f $o $CXX -std=c++17 -O2 -g -Iruntime $PLAT_CFLAGS $SDL_CFLAGS -w & pids+=($!)
done
for f in runtime/ngpu/*.cpp; do
  o=build/obj/ngpu_$(basename ${f%.cpp}).o; objs+=($o)
  build $f $o $CXX -std=c++17 -O2 -g -Iruntime $PLAT_CFLAGS $(pkg-config --cflags libpng) -Wall -Wno-unused-function & pids+=($!)
done
for f in runtime/vfpu/*.cpp; do
  o=build/obj/vfpu_$(basename ${f%.cpp}).o; objs+=($o)
  build $f $o $CXX -std=c++17 -O2 -g -Iruntime/vfpu/inc $PLAT_CFLAGS -w & pids+=($!)
done
for p in "${pids[@]}"; do wait $p || fail=1; done
if [ -n "$fail" ]; then echo "build FAILED (compile errors above)"; exit 1; fi
[ "$1" = --check ] && { echo "compile check passed ($TARGET)"; exit 0; }
[ -f build/libpspgpu.a ] || runtime/gpu/build.sh lib
[ -f build/libpspaudio.a ] || runtime/audio/build.sh lib
[ -f build/libpsputility.a ] || runtime/utility/build.sh lib
SDL_LIBS=$(pkg-config --libs sdl2 | sed 's/-lSDL2main//g; s/-mwindows//g; s/-lmingw32//g')
# Windows: SDL2 linked in statically, so the .exe needs no DLLs
[ "$WIN_STATIC" = 1 ] && SDL_LIBS=$(pkg-config --static --libs sdl2 | sed 's/-lSDL2main//g; s/-mwindows//g; s/-lmingw32//g')
$CXX -o build/sr2recomp$EXE "${objs[@]}" build/libpsputility.a build/libpspgpu.a build/libpspaudio.a \
  $(pkg-config --libs libavcodec libavutil) $SDL_LIBS $(pkg-config --libs libpng zlib) $PLAT_LIBS -lm
if [ $TARGET = windows ] && [ "$WIN_STATIC" != 1 ] && command -v ldd >/dev/null; then
  # MSYS2: copy the MinGW DLLs (SDL2, FFmpeg, ...) next to the .exe, so it also starts outside the MSYS2 shell
  ldd build/sr2recomp$EXE | awk '$3 ~ /\/(ucrt64|mingw64|clang64)\/bin\// {print $3}' | sort -u | while read -r d; do cp -u "$d" build/; done
  echo "copied $(ls build/*.dll 2>/dev/null | wc -l) DLLs into build/"
fi
echo "built build/sr2recomp$EXE"
