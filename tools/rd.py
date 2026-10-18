#!/usr/bin/env python3
# rd.py DUMP ADDR [N] : print N words (floats/hex) from a SR2_DUMP_AT RAM dump (base 0x08800000)
import struct, sys
d = open(sys.argv[1], 'rb').read()
a0 = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 32
for a in range(a0, a0 + 4 * n, 4):
    v = struct.unpack_from('<I', d, a - 0x08800000)[0]
    print('%08x %08x %g' % (a, v, struct.unpack('<f', struct.pack('<I', v))[0]))
