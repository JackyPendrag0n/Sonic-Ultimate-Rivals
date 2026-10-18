"""Build an SR2_INPUT string.
   400:CROSS            tap (held 4 vblanks)
   1000-2000/40:START   tap every 40 vblanks
   3000-3600:SUP        hold for the whole range (stick: SUP/SDOWN/SLEFT/SRIGHT, buttons combine with +)"""
import sys
ev = []
for a in sys.argv[1:]:
    t, b = a.split(':')
    if '/' in t:
        rng, step = t.split('/')
        s, e = map(int, rng.split('-'))
        for f in range(s, e, int(step)):
            ev += [(f, b), (f + 4, '-')]
    elif '-' in t:
        s, e = map(int, t.split('-'))
        ev += [(s, b), (e, '-')]
    else:
        ev += [(int(t), b), (int(t) + 4, '-')]
print(','.join('%d:%s' % x for x in sorted(ev)))
