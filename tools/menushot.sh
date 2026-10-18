#!/bin/bash
# menushot.sh NAME TAB VBLANK [extra env...]: boot (GL, Xvfb) into level 1, open the settings menu on tab TAB at VBLANK,
# save a window screenshot (build/exp/NAME/window_*.png).
cd "$(dirname "$0")/.."
N=$1; T=$2; V=$3; D=build/exp/$N; rm -rf $D; mkdir -p $D
IN=$(python3 tools/pulses.py 400:CROSS 900-1500/20:START 1600:DOWN 1700:CROSS 1900-2300/90:CROSS 2500-3100/60:CIRCLE 4300-4900/60:CROSS)
RCSM_DETERMINISTIC=1 RCSM_SAVE_DIR=$PWD/$D/save RCSM_SHOT_DIR=$D RCSM_INPUT="$IN" RCSM_MENU_TEST=$V RCSM_MENU_TAB=$T \
  xvfb-run -a -s "-screen 0 1280x900x24" timeout ${TMO:-300} ${BIN:-build/rcsm} game --log 1 > $D/log 2>&1
