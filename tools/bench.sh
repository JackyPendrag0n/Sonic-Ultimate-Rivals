#!/bin/bash
# Boot to level 1 with scripted input and report display flips per wall-clock second while in the level.
cd "$(dirname "$0")/.."
export RCSM_SAVE_DIR=$PWD/build/bench_save; rm -rf "$RCSM_SAVE_DIR"
IN=$(python3 tools/pulses.py 400:CROSS 900-1500/20:START 1600:DOWN 1700:CROSS 1900-2300/90:CROSS 2500-3100/60:CIRCLE 4300-4900/60:CROSS 5000-9000:SUP)
RCSM_HEADLESS=1 RCSM_INPUT="$IN" timeout ${1:-130} build/rcsm game --log 1 2>&1 | grep status > build/bench.log
python3 - <<'PY'
import re
rows=[tuple(map(float,re.search(r'\[\s*([\d.]+).*vblank (\d+), display flips (\d+)',l).groups())) for l in open('build/bench.log')]
lvl=[r for r in rows if r[0] > 75]
if len(lvl) >= 2:
    (t0,v0,f0),(t1,v1,f1)=lvl[0],lvl[-1]
    print('level: %.1f flips/s, %.1f vblanks/s over %.0fs' % ((f1-f0)/(t1-t0), (v1-v0)/(t1-t0), t1-t0))
PY
