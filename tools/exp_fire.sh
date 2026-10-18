#!/bin/bash
# exp_fire.sh NAME "PATCHSPEC" [extra input]: boot to level 1, hold fire 5900-6500, report Lacerator ammo used.
cd "$(dirname "$0")/.."
N=$1; P=$2
D=build/exp/$N; rm -rf $D; mkdir -p $D
IN=$(python3 tools/pulses.py 400:CROSS 900-1500/20:START 1600:DOWN 1700:CROSS 1900-2300/90:CROSS 2500-3100/60:CIRCLE 4300-4900/60:CROSS 5800:CIRCLE ${HOLD:-5900-6500:CIRCLE} $3)
PSPGPU_THREADS=1 RCSM_SAVE_DIR=$PWD/$D/save RCSM_HEADLESS=1 RCSM_PATCH="$P" RCSM_SHOT_DIR=$D RCSM_DUMP_AT=5895,6505 RCSM_INPUT="$IN" \
  timeout ${TMO:-330} ${BIN:-build/rcsm} game --log 1 > $D/log 2>&1
python3 - $D <<'PY'
import sys, struct
d = sys.argv[1]
def rd(f, a):
    try: return struct.unpack('<I', open(f, 'rb').read()[a - 0x08800000:a - 0x08800000 + 4])[0]
    except Exception as e: return None
a0, a1 = rd(d + '/ram_005895.bin', 0x093dea4c), rd(d + '/ram_006505.bin', 0x093dea4c)
print(d, 'ammo', a0, '->', a1, 'used', (a0 - a1) if a0 is not None and a1 is not None else '?')
PY
