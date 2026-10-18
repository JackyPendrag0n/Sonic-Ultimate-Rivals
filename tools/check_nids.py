import re, json, sys
fix = '--fix' in sys.argv
args = [a for a in sys.argv[1:] if a != '--fix']
db = {int(k,16): v['name'] for k, v in json.load(open(args[0])).items()}
byname = {}
for k, v in db.items(): byname.setdefault(v, []).append(k)
bad = 0
for f in args[1:]:
    src = open(f).read()
    def rep(m):
        global bad
        nid, name = int(m.group(1), 16), m.group(2)
        if db.get(nid) == name: return m.group(0)
        bad += 1
        cands = byname.get(name, [])
        print('%s: %08X %s -> db says %s; by name: %s' % (f, nid, name, db.get(nid), ['%08X' % x for x in cands]))
        if fix and len(cands) == 1: return '{0x%08X, "%s"' % (cands[0], name)
        return m.group(0)
    out = re.sub(r'\{0x([0-9A-Fa-f]{8}), "(\w+)"', rep, src)
    if fix: open(f, 'w').write(out)
print('mismatches:', bad)
