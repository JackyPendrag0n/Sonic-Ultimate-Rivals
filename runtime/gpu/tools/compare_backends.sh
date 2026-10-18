#!/bin/bash
# Runs build/test_ge_gl on the software and the OpenGL backend (under xvfb-run if there's no
# display) and compares what they presented: per-image mean/max difference and a side-by-side PNG
# (software | OpenGL | difference x4) per scene in build/gltest/.
#   runtime/gpu/tools/compare_backends.sh [internal-scale]
set -e
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="$ROOT/build/gltest"
SCALE="${1:-1}"
mkdir -p "$OUT"
run() { if [ -z "$DISPLAY" ]; then xvfb-run -a -s "-screen 0 1280x720x24" "$@"; else "$@"; fi; }
run "$ROOT/build/test_ge_gl" soft "$OUT/soft" 1 | grep -v '^\['
run "$ROOT/build/test_ge_gl" gl "$OUT/gl" "$SCALE" | grep -v '^\['
python3 -I - "$OUT" <<'PY'
import sys, numpy as np
from PIL import Image
out = sys.argv[1]
for scene in ('scene', 'cpufb', 'movie'):
    a = np.asarray(Image.open(f'{out}/soft_{scene}.png').convert('RGB')).astype(int)
    b = np.asarray(Image.open(f'{out}/gl_{scene}.png').convert('RGB')).astype(int)
    d = np.abs(a - b)
    big = (d.max(axis=2) > 24).mean() * 100
    print(f'{scene:6s}: mean abs diff {d.mean():.2f}/255, max {d.max()}, pixels off by >24: {big:.2f}%')
    side = np.concatenate([a, b, np.clip(d * 4, 0, 255)], axis=1).astype(np.uint8)
    Image.fromarray(side).save(f'{out}/compare_{scene}.png')
PY
