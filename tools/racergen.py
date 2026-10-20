#!/usr/bin/env python3
"""Rebuild a character's racer file for another load slot and address.

  racergen.py <game folder> <CHAR> <slot> <base hex> <heap hex|keep> <out file>

A racer file (STREAMS/RACER_<CHAR>_<n>.STR, 0x166670 bytes) is a raw memory image that the game copies to a
fixed address and uses as is: _0 is made for load slot 14 at 0x08F61670, _1 for slot 15 at 0x090C7CF0.
The two images hold the same objects in the same order (a few blocks sit 16 bytes apart), so lining them
up shows every word that depends on the slot:
  - pointers into the image itself (also through the uncached mirror, bit 30): rebased to <base>;
  - the slot number, as a whole word or as byte 2 of 0x0?SS00nn handles: set to <slot>;
  - pointers to per-slot game objects (arrays with a 0x10, 0x40 or 0x120 stride): moved by that stride;
  - pointers into the racer's own block of the game heap (around 0x09C40000-0x09DC0000; zero-filled work
    memory whose layout differs per character): moved, keeping _0's layout, to a block at <heap>
    (the output says how big it is; "keep" leaves them as in _0).
Everything else is the same in both images and is copied from _0. With slot 14, base 08F61670 and
"keep" the output is byte for byte RACER_<CHAR>_0.STR (checked for all eight characters).

Needs numpy. See README "Racer files".
"""
import os
import sys
sys.path = [p for p in sys.path if os.path.abspath(p or '.') != os.path.dirname(os.path.abspath(__file__))]   # tools/dis.py hides the stdlib's dis
import numpy as np  # noqa: E402

SIZE = 0x166670
BASE = (0x08F61670, 0x090C7CF0)   # load addresses of _0 (slot 14) and _1 (slot 15)
HEAP = (0x09600000, 0x0A000000)   # where the racers' heap blocks are
STRIDES = (0x10, 0x40, 0x120)     # per-slot game object arrays
SLOT_OBJS = 0x0922E360            # the 0x40-byte one, slot 0


def load(game, ch, n):
    p = os.path.join(game, 'PSP_GAME', 'USRDIR', 'STREAMS', 'RACER_%s_%d.STR' % (ch, n))
    f = np.fromfile(p, dtype='<u4').astype(np.int64)
    if len(f) * 4 != SIZE:
        sys.exit('racergen: %s is not a racer file' % p)
    return f


def mask(f):
    """the image with slot-dependent words blanked, for lining the two images up"""
    g = f.copy()
    a = g & 0x3fffffff
    g[(a >= 0x08000000) & (a < 0x0a000000)] = 1
    s = (g >> 16) & 0xff
    g[((g >> 24) <= 2) & (s >= 13) & (s <= 16) & (g != 0)] = 2
    g[(g == 14) | (g == 15)] = 3
    return g


def align(f0, f1):
    """for each 16-byte line of _0, the matching line of _1 (-1: none)"""
    m0, m1 = mask(f0), mask(f1)
    W, L = 16, len(f0) // 4
    idx = {}
    for j in range(len(f1) // 4 - 4):
        idx.setdefault(m1[j * 4:j * 4 + W].tobytes(), []).append(j)
    mp = np.full(L, -1, dtype=np.int64)
    for i in range(L - 4):
        c = idx.get(m0[i * 4:i * 4 + W].tobytes())
        if c is None:
            continue
        if len(c) == 1:
            mp[i] = c[0]
        elif i and mp[i - 1] >= 0 and mp[i - 1] + 1 in c:
            mp[i] = mp[i - 1] + 1
    for i in range(1, L):   # lines whose 64-byte window crossed a change
        j = mp[i - 1] + 1
        if mp[i] < 0 and mp[i - 1] >= 0 and (j + 1) * 4 <= len(m1) and (m0[i * 4:i * 4 + 4] == m1[j * 4:j * 4 + 4]).all():
            mp[i] = j
    # keep only lines that really match; else try the neighbours' alignment
    ok = lambda i, j: 0 <= j and (j + 1) * 4 <= len(m1) and (m0[i * 4:i * 4 + 4] == m1[j * 4:j * 4 + 4]).all()
    for i in range(L):
        if mp[i] >= 0 and not ok(i, mp[i]):
            mp[i] = -1
            for j in ([mp[i - 1] + 1] if i and mp[i - 1] >= 0 else []) + ([mp[i + 1] - 1] if i + 1 < L and mp[i + 1] >= 0 else []):
                if ok(i, j):
                    mp[i] = j
                    break
    return mp


def build(f0, f1, slot, base, heap):
    own = lambda v, b: b <= (v & 0x3fffffff) < b + SIZE
    mp = align(f0, f1)
    g = f0.copy()
    count = {'pointers': 0, 'slot numbers': 0, 'per-slot objects': 0, 'heap pointers': 0, 'unresolved': 0}
    unresolved = []
    heap_ptrs = []
    for i in range(len(mp)):
        j = mp[i]
        for k in range(4):
            o = i * 4 + k
            a = int(f0[o])
            if j < 0:
                if own(a, BASE[0]):
                    g[o] = (a & 0xc0000000) | ((a & 0x3fffffff) - BASE[0] + base); count['pointers'] += 1
                continue
            b = int(f1[j * 4 + k])
            if a == b:
                continue
            x = a ^ b
            if own(a, BASE[0]) and own(b, BASE[1]) and (a & 0xc0000000) == (b & 0xc0000000):
                g[o] = (a & 0xc0000000) | ((a & 0x3fffffff) - BASE[0] + base); count['pointers'] += 1
            elif x in (1, 0x100, 0x10000, 0x1000000) and (a // x) & 0xff == 14:
                sh = x.bit_length() - 1
                g[o] = a & ~(0xff << sh) | (slot << sh); count['slot numbers'] += 1
            elif 0x08800000 <= a < 0x0a000000 and 0x08800000 <= b < 0x0a000000:
                d = b - a
                if d in STRIDES:
                    g[o] = a + d * (slot - 14); count['per-slot objects'] += 1
                elif HEAP[0] <= a < HEAP[1] and HEAP[0] <= b < HEAP[1]:
                    heap_ptrs.append(o); count['heap pointers'] += 1
                else:
                    unresolved.append((o, a, b))
            else:
                unresolved.append((o, a, b))
    # a line that matched by content but holds a word neither rule explains (a stray match, or data that
    # really differs): treat it like an unmatched line
    for o, a, b in unresolved:
        if own(a, BASE[0]):
            g[o] = (a & 0xc0000000) | ((a & 0x3fffffff) - BASE[0] + base); count['pointers'] += 1
        elif a & 0xfdff0000 == 0x000e0000:   # 0x000E0000 / 0x020E00nn slot handles
            g[o] = a & ~0xff0000 | slot << 16; count['slot numbers'] += 1
        elif SLOT_OBJS <= a < SLOT_OBJS + 16 * 0x40:   # the 16 per-slot 0x40-byte objects
            g[o] = a + 0x40 * (slot - 14); count['per-slot objects'] += 1
        else:
            count['unresolved'] += 1
            print('  kept %06x: %08x (in _1: %08x)' % (o * 4, a, b))
    lo = min(int(f0[o]) for o in heap_ptrs) & ~0xf if heap_ptrs else 0
    hi = max(int(f0[o]) for o in heap_ptrs) if heap_ptrs else 0
    if heap is not None:
        for o in heap_ptrs:
            g[o] = int(f0[o]) - lo + heap
    return g, count, (lo, hi)


def main():
    if len(sys.argv) != 7:
        sys.exit(__doc__)
    game, ch, slot, base, out = sys.argv[1], sys.argv[2].upper(), int(sys.argv[3]), int(sys.argv[4], 16), sys.argv[6]
    heap = None if sys.argv[5] == 'keep' else int(sys.argv[5], 16)
    g, count, (lo, hi) = build(load(game, ch, 0), load(game, ch, 1), slot, base, heap)
    g.astype('<u4').tofile(out)
    print('%s: slot %d at %08x; changed %s' % (out, slot, base, ', '.join('%d %s' % (v, k) for k, v in count.items())))
    if hi:
        print('heap block: _0 uses %08x-%08x (pointers up to %08x); in the output it starts at %08x' % (lo, hi, hi, heap if heap is not None else lo))


if __name__ == '__main__':
    main()
