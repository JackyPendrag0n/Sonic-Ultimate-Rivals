#!/bin/bash
# Build the native binary. data/modules.txt lists "<path to ELF/PRX> <c name>" per line.
set -e
cd "$(dirname "$0")/.."
OPT=${OPT:--O2}
mkdir -p build/obj gen
names=()
while read -r path name; do
  [ -z "$path" ] || [[ "$path" == \#* ]] && continue
  names+=("$name")
  if [ ! -f gen/$name/${name}_table.c ] || [ "$path" -nt gen/$name/${name}_table.c ] || [ tools/recomp.py -nt gen/$name/${name}_table.c ] || [ data/hooks.json -nt gen/$name/${name}_table.c ] || [ -f data/seeds_$name.txt -a data/seeds_$name.txt -nt gen/$name/${name}_table.c ]; then
    # (files are rewritten only when their content changes, so unchanged code is not recompiled)
    python3 -I tools/recomp.py "$path" $name gen/$name --extra-seeds data/seeds_$name.txt && touch gen/$name/${name}_table.c
  fi
done < data/modules.txt
python3 -I tools/gen_support.py "${names[@]}"
CFLAGS="$OPT -g -fno-strict-aliasing -fwrapv -foptimize-sibling-calls -Iruntime -w"
objs=()
build() { # src obj compiler flags...
  local src=$1 obj=$2; shift 2
  local dep=runtime/kernel.h
  case $src in gen/*) dep=runtime/rt.h;; esac
  local dep2=runtime/config.h
  case $src in gen/*) dep2=runtime/rt.h;; esac
  if [ ! -f $obj ] || [ $src -nt $obj ] || [ runtime/rt.h -nt $obj ] || [ runtime/interp_list.h -nt $obj ] || [ $dep -nt $obj ] || [ $dep2 -nt $obj ]; then "$@" -c $src -o $obj || exit 1; fi
}
pids=(); fail=
for n in "${names[@]}"; do
  for f in gen/$n/*.c; do
    o=build/obj/$(basename ${f%.c}).o; objs+=($o)
    build $f $o gcc ${GENOPT:-$CFLAGS} & pids+=($!)
    if [ ${#pids[@]} -ge ${JOBS:-$(nproc)} ]; then wait ${pids[0]} || fail=1; pids=("${pids[@]:1}"); fi
  done
done
for f in gen/modules.c gen/nidnames.c runtime/*.c; do
  o=build/obj/$(basename ${f%.c}).o; objs+=($o)
  build $f $o gcc $CFLAGS -Wall -Wno-unused-function -Wno-unused-variable &
  pids+=($!)
done
for f in runtime/*.cpp runtime/imgui/*.cpp; do
  o=build/obj/cxx_$(basename ${f%.cpp}).o; objs+=($o)
  build $f $o g++ -std=c++17 -O2 -g -Iruntime $(pkg-config --cflags sdl2) -w & pids+=($!)
done
for f in runtime/vfpu/*.cpp; do
  o=build/obj/vfpu_$(basename ${f%.cpp}).o; objs+=($o)
  build $f $o g++ -std=c++17 -O2 -g -Iruntime/vfpu/inc -w & pids+=($!)
done
for p in "${pids[@]}"; do wait $p || fail=1; done
if [ -n "$fail" ]; then echo "build FAILED (compile errors above)"; exit 1; fi
[ -f build/libpspgpu.a ] || runtime/gpu/build.sh lib
[ -f build/libpspaudio.a ] || runtime/audio/build.sh lib
[ -f build/libpspmpeg.a ] || runtime/mpeg/build.sh lib
[ -f build/libpsputility.a ] || runtime/utility/build.sh lib
g++ -o build/sr2recomp "${objs[@]}" build/libpsputility.a build/libpspgpu.a build/libpspmpeg.a build/libpspaudio.a $(pkg-config --libs libavformat libavcodec libswscale libswresample libavutil) -lSDL2 -lpng -lpthread -lz -lm
echo "built build/sr2recomp"
