"""Print instruction words with reloc flags for pattern building: words.py <elf> <start> <count>"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from pspelf import PspElf
from dis import fmt
e = PspElf(sys.argv[1]); s = int(sys.argv[2], 16); n = int(sys.argv[3])
stubs = {i['stub']: i['name'] for i in e.imports}
for a in range(s, s + 4 * n, 4):
    r = [t for t, _ in e.reloc_at.get(a, [])]
    print('%06x %08x %-4s %s' % (a, e.u32(a), ','.join(map(str, r)), fmt(e, a, stubs)))
