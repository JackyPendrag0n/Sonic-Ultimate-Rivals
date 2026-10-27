#!/usr/bin/env python3
"""secrefs.py STREAMS/R2_ZONE_n_ACT_m.STR : where a race level's course sections are pointed at from outside them.

A level file starts with a table of chunks (+4 + 8k: file offset, size): 0 the level, 1.. the course sections. Each
section is a memory image made for the address of its load slot, and sections k and k+4 share an address (four
section buffers), so to keep them all in memory sections 5.. have to move. This lists the words outside the
sections (the world header, read to WORLD, and the level image) that point into the buffers and decides which
section each one belongs to:
  slothdr   the slot headers (WORLD + 0x90 + 0x4c0 k)
  leveltab  the level's per-slot blocks (level + 0x14 + 0x40 k)
  array     arrays indexed by slot (entry k in buffer (k-1) & 3 for most k, entry 0 not in a buffer)
  sparse    the level's objects: 12 entries indexed by slot - 1, zero or in that slot's buffer
  size      only one section is big enough
and prints the rest (on Blue Coast 1 they are runtime state in the world, slot 1's, and words of mesh data that
happen to look like pointers). Buffer addresses as the race load sets them (SLOT_BASES in runtime/game.c)."""
import struct, sys, collections
WORLD, LEVEL = 0x089ABD10, 0x0922E350
BUFS, BSZ = [0x09647CF0, 0x0977B030, 0x098AE370, 0x099E16B0], 0x133340
SLOTS = WORLD + 0x90

def buf(v):
    for i, b in enumerate(BUFS):
        if b <= v < b + BSZ: return i
    return None

def owners(d):
    hdr = struct.unpack_from('<34I', d, 0)
    chunks = [(hdr[1 + 2 * i], hdr[2 + 2 * i]) for i in range(16)]
    nsec = max(k for k in range(1, 13) if chunks[k][1])
    cands = lambda v: [k for k in range(1, nsec + 1) if (k - 1) % 4 == buf(v) and v - BUFS[buf(v)] < chunks[k][1]]
    mem = {}
    for off, sz, base in [(0, 0x5800, WORLD), (chunks[0][0], chunks[0][1], LEVEL)]:
        for i, x in enumerate(struct.unpack_from('<%dI' % (sz // 4), d, off)): mem[base + i * 4] = x
    refs = [a for a, x in sorted(mem.items()) if buf(x) is not None and x % 4 == 0]
    own = {}
    for a in refs:
        if SLOTS <= a < SLOTS + 16 * 0x4c0: own[a] = ('slothdr', (a - SLOTS) // 0x4c0)
        elif LEVEL + 0x54 <= a < LEVEL + 0x14 + 0x40 * 16: own[a] = ('leveltab', (a - LEVEL - 0x14) // 0x40)
    for a in refs:
        if a in own: continue
        for k in range(1, nsec + 1):
            s = a - 4 * k
            ok = sum(1 for j in range(1, nsec + 1) if mem.get(s + 4 * j) is not None and buf(mem[s + 4 * j]) == (j - 1) % 4)
            if ok >= min(nsec, 6) and buf(mem[a]) == (k - 1) % 4 and buf(mem.get(s, 0)) is None: own[a] = ('array', k); break
    for a in refs:
        if a in own: continue
        c = cands(mem[a])
        if len(c) == 1: own[a] = ('size', c[0]); continue
        ok = [k for k in c if all(mem.get(a - 4 * (k - 1) + 4 * j, 1) == 0 or buf(mem.get(a - 4 * (k - 1) + 4 * j, 0)) == j % 4 for j in range(12))]
        if len(ok) == 1: own[a] = ('sparse', ok[0])
    return nsec, chunks, mem, refs, own, cands

if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    nsec, chunks, mem, refs, own, cands = owners(d)
    print('%d sections; %d words point into the buffers: %s' % (nsec, len(refs), dict(collections.Counter(o[0] for o in own.values()))))
    if '-v' in sys.argv:
        for a in refs:
            if a in own and own[a][1] > 4: print('  %-8s %08x -> %08x section %d' % (own[a][0], a, mem[a], own[a][1]))
    for a in refs:
        if a not in own: print('  undecided %08x -> %08x (could be section %s)' % (a, mem[a], cands(mem[a]) or 'none: too far in'))
