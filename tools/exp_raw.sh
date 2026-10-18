#!/bin/bash
# exp_raw.sh NAME PULSE... : like exp.sh but with no boot prefix (drive the menus yourself).
cd "$(dirname "$0")/.."
N=$1; shift; D=build/exp/$N; rm -rf $D; mkdir -p $D
[ -n "$SEED" ] && cp -r "$SEED" $D/save   # SEED=dir: start from a copy of this save folder
IN=$(python3 tools/pulses.py "$@")
PSPGPU_THREADS=1 RCSM_DETERMINISTIC=${DET:-1} RCSM_GPU=soft RCSM_SAVE_DIR=${SAVE:-$PWD/$D/save} RCSM_HEADLESS=1 RCSM_SHOT_DIR=$D RCSM_SHOT_EVERY=${EVERY:-30} RCSM_SHOT_FROM=${FROM:-0} RCSM_DUMP_AT=${DUMPS:-} RCSM_INPUT="$IN" \
  timeout ${TMO:-330} ${BIN:-build/rcsm} game --log ${LOGLVL:-1} > $D/log 2>&1
