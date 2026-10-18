#!/bin/bash
# exp.sh NAME PULSE... : deterministic boot into level 1 (same prefix as exp_walk.sh) then the given extra
# pulses (tools/pulses.py syntax). Screenshots every ${EVERY:-30} vblanks; RAM dumps at $DUMPS.
cd "$(dirname "$0")/.."
N=$1; shift; D=build/exp/$N; rm -rf $D; mkdir -p $D
IN=$(python3 tools/pulses.py 400:CROSS 900-1500/20:START 1600:DOWN 1700:CROSS 1900-2300/90:CROSS 2500-3100/60:CIRCLE 4300-4900/60:CROSS "$@")
PSPGPU_THREADS=1 RCSM_DETERMINISTIC=${DET:-1} RCSM_GPU=soft RCSM_SAVE_DIR=$PWD/$D/save RCSM_HEADLESS=1 RCSM_SHOT_DIR=$D RCSM_SHOT_EVERY=${EVERY:-30} RCSM_SHOT_FROM=${FROM:-0} RCSM_DUMP_AT=${DUMPS:-} RCSM_INPUT="$IN" \
  timeout ${TMO:-330} ${BIN:-build/rcsm} game --log ${LOGLVL:-1} > $D/log 2>&1
