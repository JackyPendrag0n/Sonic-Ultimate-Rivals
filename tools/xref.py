#!/usr/bin/env python3
"""xref.py MODULE.prx LO [HI] : list code (HI16/LO16 pairs) and data (32-bit) references to image addresses in [LO, HI)."""
import sys, struct, os
sys.path.insert(0, os.path.dirname(__file__))
from pspelf import PspElf
e = PspElf(sys.argv[1]); lo = int(sys.argv[2], 16); hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else lo + 1
img = e.orig_image
last_hi = None
for a, t, ts in e.relocs:
    op = struct.unpack_from('<I', img, a)[0]
    if t == 5: last_hi = (a, op, ts)
    elif t == 6 and last_hi:
        imm = op & 0xFFFF; imm = imm - 0x10000 if imm & 0x8000 else imm
        v = (((last_hi[1] & 0xFFFF) << 16) + imm + last_hi[2]) & 0xFFFFFFFF
        if lo <= v < hi: print('code %06x (hi %06x) -> %06x' % (a, last_hi[0], v))
    elif t == 2:
        v = (op + ts) & 0xFFFFFFFF
        if lo <= v < hi: print('data %06x -> %06x' % (a, v))
