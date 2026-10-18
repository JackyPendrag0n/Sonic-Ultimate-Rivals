"""Extract PPSSPP's MIPS/Allegrex decode tables (MIPSTables.cpp) into a JSON decode tree."""
import re, sys, json

src = open(sys.argv[1]).read()
src = re.sub(r'//[^\n]*', '', src)
src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)

enc_names = re.search(r'enum MipsEncoding \{(.*?)\};', src, re.S).group(1)
enc_names = [x.strip().split('=')[0].strip() for x in enc_names.split(',') if x.strip()]
enc_names = [x for x in enc_names if x not in ('NumEncodings', 'Instruc', 'Inval')]

bits = re.search(r'encodingBits\[NumEncodings\] = \{(.*?)\};', src, re.S).group(1)
bits = re.findall(r'EncodingBitsInfo\((\d+),\s*(\d+)\)', bits)
tables_order = re.search(r'mipsTables\[NumEncodings\] = \{(.*?)\};', src, re.S).group(1)
tables_order = [x.strip() for x in tables_order.split(',') if x.strip()]


def split_top(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip()); cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


tables = {}
for m in re.finditer(r'static const MIPSInstruction (\w+)\[(\d+)\]\s*=\s*\{(.*?)\n\};', src, re.S):
    name, n, body = m.group(1), int(m.group(2)), m.group(3)
    ents = []
    for e in split_top(body):
        if e == 'INVALID':
            ents.append(None)
        elif e == 'INVALID_X_8':
            ents += [None] * 8
        elif e.startswith('ENCODING('):
            ents.append({'enc': e[9:-1].strip()})
        elif e.startswith('INSTR('):
            a = split_top(e[6:-1])
            ents.append({'name': a[0].strip('"'), 'interp': a[3], 'flags': a[4]})
        else:
            raise Exception('bad entry ' + e)
    ents += [None] * (n - len(ents))
    tables[name] = ents

out = {'encodings': {}}
for i, en in enumerate(enc_names):
    t = tables_order[i]
    out['encodings'][en] = {'shift': int(bits[i][0]), 'bits': int(bits[i][1]),
                            'table': tables.get(t) if t != '0' else None}
json.dump(out, open(sys.argv[2], 'w'))
print('encodings', len(enc_names), 'tables', len(tables))
