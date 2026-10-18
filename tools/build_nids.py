import re, sys, glob, json
root = sys.argv[1]
db = {}
pat = re.compile(r'\{\s*0[xX]([0-9A-Fa-f]{8})\s*,\s*([^,]*),\s*"(\w+)"\s*(?:,\s*\'(.)\'\s*,\s*"([^"]*)")?')
for f in glob.glob(root + '/Core/HLE/*.cpp'):
    for m in pat.finditer(open(f, errors='ignore').read()):
        nid = int(m.group(1), 16)
        db.setdefault(nid, {'name': m.group(3), 'ret': m.group(4) or '', 'args': m.group(5) or '', 'impl': 'nullptr' not in m.group(2)})
json.dump({'%08X' % k: v for k, v in db.items()}, open(sys.argv[2], 'w'), indent=0)
print(len(db), 'NIDs')
