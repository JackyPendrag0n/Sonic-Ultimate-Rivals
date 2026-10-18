#!/bin/bash
# glsurvey.sh NAME "debug actions" [pulses...]: GL renderer under Xvfb, boot to level 1 (deterministic), window shots every ${EVERY:-120}
cd "$(dirname "$0")/.."
N=$1; A=$2; shift 2; D=build/exp/$N; rm -rf $D; mkdir -p $D
IN=$(python3 tools/pulses.py 400:CROSS 900-1500/20:START 1600:DOWN 1700:CROSS 1900-2300/90:CROSS 2500-3100/60:CIRCLE 4300-4900/60:CROSS "$@")
RCSM_DETERMINISTIC=1 RCSM_SAVE_DIR=$PWD/$D/save RCSM_SHOT_DIR=$D RCSM_WINSHOT_EVERY=${EVERY:-120} RCSM_INPUT="$IN" RCSM_DEBUG_AT="$A" \
  xvfb-run -a -s "-screen 0 1280x900x24" timeout ${TMO:-900} ${BIN:-build/rcsm} game --log 1 > $D/log 2>&1
