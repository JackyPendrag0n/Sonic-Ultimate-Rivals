#!/bin/bash
# watchbuild.sh MOD... : link build/rcsm_watch with the given modules compiled with -DRCSM_WATCH
# (memory accesses report through rt_watch; use with RCSM_WATCH=lo-hi). Run tools/build.sh first.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/watchobj
CF="-O1 -g -fno-strict-aliasing -fwrapv -foptimize-sibling-calls -Iruntime -w -DRCSM_WATCH"
objs=(); pids=()
for o in build/obj/*.o; do
  b=$(basename $o .o); use=$o
  for m in "$@"; do
    case $b in ${m}_[0-9]*|${m}_table)
      src=gen/$m/$b.c; use=build/watchobj/$b.o
      if [ ! -f $use ] || [ $src -nt $use ] || [ runtime/rt.h -nt $use ]; then
        gcc $CF -c $src -o $use & pids+=($!)
        if [ ${#pids[@]} -ge ${JOBS:-2} ]; then wait ${pids[0]}; pids=("${pids[@]:1}"); fi
      fi;;
    esac
  done
  objs+=($use)
done
wait
g++ -rdynamic -o build/rcsm_watch "${objs[@]}" build/libpsputility.a build/libpspgpu.a build/libpspmpeg.a build/libpspaudio.a $(pkg-config --libs libavformat libavcodec libswscale libswresample libavutil) -lSDL2 -lpng -lpthread -lz -lm
echo built build/rcsm_watch
