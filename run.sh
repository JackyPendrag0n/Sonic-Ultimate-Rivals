#!/bin/bash
# headless test run: run.sh <seconds> <log>
S=$(cd "$(dirname "$0")/.." && pwd)
export HOME=$S/home; mkdir -p $HOME
timeout ${1:-30} xvfb-run -a -s "-screen 0 1280x720x24" env SDL_AUDIODRIVER=dummy ${EXTRA_ENV} "$(dirname "$0")"/build/rcsm $S/game > ${2:-$S/run.log} 2>&1
