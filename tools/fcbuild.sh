#!/bin/bash
# fcbuild.sh : link build/sr2recomp_fc, the game with a call counter at every function's entry (test aid;
# the control fifo's "fcount PATH" writes the counts since the last one). Run tools/build.sh first.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/fcgen build/fcobj
python3 - <<'PY'
import re, glob, os
pat = re.compile(r'^((?:static )?void F_sr2_([0-9a-f]+)(?:_body)?\(Ctx \*c\) \{)$', re.M)
for f in sorted(glob.glob('gen/sr2/sr2_[0-9]*.c')):
    out = 'build/fcgen/' + os.path.basename(f)
    if os.path.exists(out) and os.path.getmtime(out) > os.path.getmtime(f): continue
    s = open(f).read()
    s = pat.sub(lambda m: m.group(1) + '\n  { extern unsigned *g_fcount; if (g_fcount) g_fcount[0x%s >> 2]++; }' % m.group(2), s)
    open(out, 'w').write(s)
PY
CF="-O1 -fno-strict-aliasing -fwrapv -foptimize-sibling-calls -Iruntime -Igen/sr2 -w"
objs=(); pids=()
for o in build/obj/*.o; do
  b=$(basename $o .o); use=$o
  case $b in sr2_[0-9]*)
    src=build/fcgen/$b.c; use=build/fcobj/$b.o
    if [ ! -f $use ] || [ $src -nt $use ]; then
      gcc $CF -c $src -o $use & pids+=($!)
      if [ ${#pids[@]} -ge ${JOBS:-2} ]; then wait ${pids[0]}; pids=("${pids[@]:1}"); fi
    fi;;
  esac
  objs+=($use)
done
wait
g++ -rdynamic -o build/sr2recomp_fc "${objs[@]}" build/libpsputility.a build/libpspgpu.a build/libpspmpeg.a build/libpspaudio.a $(pkg-config --libs libavformat libavcodec libswscale libswresample libavutil) -lSDL2 -lpng -lpthread -lz -lm
echo built build/sr2recomp_fc
