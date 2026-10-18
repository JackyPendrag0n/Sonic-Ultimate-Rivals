"""Generate gen/modules.c (recompiled module registry), gen/nidnames.c and runtime/interp_list.h."""
import json, sys, os, glob, io

def write_if_changed(path, text):
    if os.path.exists(path) and open(path).read() == text:
        return
    open(path, "w").write(text)

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
mods = sys.argv[1:]
f = io.StringIO()
if True:
    f.write('#include "rt.h"\n')
    for m in mods:
        f.write('extern const RecompModule mod_%s;\n' % m)
    f.write('const RecompModule *const g_recomp_modules[] = {%s};\n' % ', '.join('&mod_%s' % m for m in mods))
    f.write('const int g_recomp_module_count = %d;\n' % len(mods))
write_if_changed(os.path.join(root, 'gen', 'modules.c'), f.getvalue())
db = json.load(open(os.path.join(root, 'data', 'nids.json')))
f = io.StringIO()
if True:
    f.write('#include "rt.h"\n#include <stdio.h>\nstatic const struct { u32 nid; const char *name; } T[] = {\n')
    for k, v in sorted(db.items()):
        f.write('{0x%s,"%s"},\n' % (k, v['name']))
    f.write('};\nconst char *nid_name(u32 nid) { int lo = 0, hi = sizeof T / sizeof T[0] - 1;\n'
            ' while (lo <= hi) { int m = (lo + hi) / 2; if (T[m].nid == nid) return T[m].name; if (T[m].nid < nid) lo = m + 1; else hi = m - 1; }\n'
            ' static char b[8][16]; static int i; i = (i + 1) & 7; snprintf(b[i], 16, "nid_%08X", nid); return b[i]; }\n')
write_if_changed(os.path.join(root, 'gen', 'nidnames.c'), f.getvalue())
names = set()
for m in mods:
    p = os.path.join(root, 'gen', m, '%s_interp.txt' % m)
    names |= {l.strip() for l in open(p) if l.strip()}
write_if_changed(os.path.join(root, 'runtime', 'interp_list.h'), ''.join('X(%s)\n' % n for n in sorted(names)))
print('modules:', ' '.join(mods), '| interp fns:', len(names))
