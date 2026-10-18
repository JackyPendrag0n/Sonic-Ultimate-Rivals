"""Allegrex instruction decoder driven by tables extracted from PPSSPP."""
import json, os

_T = json.load(open(os.path.join(os.path.dirname(__file__), '..', 'data', 'mipstables.json')))['encodings']
_cache = {}


def decode(op):
    """Return dict(name, interp, flags) or None for invalid."""
    r = _cache.get(op)
    if r is not None or op in _cache:
        return r
    enc = 'Imme'
    ent = _T['Imme']['table'][op >> 26]
    while ent is not None and 'enc' in ent:
        enc = ent['enc']
        e = _T[enc]
        if e['table'] is None:
            ent = None
            break
        sub = (op >> e['shift']) & ((1 << e['bits']) - 1)
        ent = e['table'][sub]
    _cache[op] = ent
    return ent


def rs(op): return (op >> 21) & 31
def rt(op): return (op >> 16) & 31
def rd(op): return (op >> 11) & 31
def sa(op): return (op >> 6) & 31
def imm16(op): return op & 0xFFFF
def simm16(op): return (op & 0xFFFF) - ((op & 0x8000) << 1)
def target26(op): return (op & 0x03FFFFFF) << 2
