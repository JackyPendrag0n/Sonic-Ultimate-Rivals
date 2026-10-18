#!/bin/bash
# exp_hit.sh NAME "PATCH": walk to the first crab robots and fire at them; screenshots every 20 vblanks.
cd "$(dirname "$0")/.."
N=$1; P=$2; D=build/exp/$N; rm -rf $D; mkdir -p $D
IN=$(python3 tools/pulses.py 400:CROSS 900-1500/20:START 1600:DOWN 1700:CROSS 1900-2300/90:CROSS 2500-3100/60:CIRCLE 4300-4900/60:CROSS 5000-5700:SUP 5700-6000:SRIGHT 6000-7200:CIRCLE)
PSPGPU_THREADS=1 RCSM_SAVE_DIR=$PWD/$D/save RCSM_HEADLESS=1 RCSM_PATCH="$P" RCSM_SHOT_DIR=$D RCSM_SHOT_EVERY=${SHOT:-20} RCSM_INPUT="$IN" \
  timeout ${TMO:-330} ${BIN:-build/rcsm} game --log 1 > $D/log 2>&1
echo done $D
