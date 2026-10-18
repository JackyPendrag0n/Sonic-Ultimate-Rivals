#!/bin/bash
# exp_walk.sh NAME : deterministic boot into level 1, walk around; screenshots every 30 vblanks from 3300.
cd "$(dirname "$0")/.."
N=$1; D=build/exp/$N; rm -rf $D; mkdir -p $D
IN=$(python3 tools/pulses.py 400:CROSS 900-1500/20:START 1600:DOWN 1700:CROSS 1900-2300/90:CROSS 2500-3100/60:CIRCLE 4300-4900/60:CROSS 5000-5300:SUP 5300-5500:SLEFT 5500-5600:CROSS 5650-5800:SUP 5800-6100:CIRCLE)
PSPGPU_THREADS=1 RCSM_DETERMINISTIC=1 RCSM_GPU=soft RCSM_SAVE_DIR=$PWD/$D/save RCSM_HEADLESS=1 RCSM_SHOT_DIR=$D RCSM_SHOT_EVERY=30 RCSM_DUMP_AT=${DUMPS:-} RCSM_INPUT="$IN" \
  timeout ${TMO:-330} ${BIN:-build/rcsm} game --log 1 > $D/log 2>&1
