"""Static recompiler: PSP (Allegrex) relocatable ELF/PRX -> base-independent C.

Usage: recomp.py <elf> <cname> <outdir> [--extra-seeds file]

Model:
  * Every module is loaded by the runtime at a 64KiB-aligned base MB.  With such a base, LO16
    relocations are no-ops and HI16 relocations only add MB>>16, so `lui` with a HI16 reloc becomes
    `(imm<<16) + MB` and everything else is position independent.
  * Each discovered function becomes `void F_<cname>_<off>(Ctx *c)`.  jal -> direct C call,
    jr $ra -> return, branches to other functions -> tail calls.
  * Import stubs (jal into .sceStub.text) -> HLE(c, nid).
  * VFPU and other rare ops -> calls into the (PPSSPP-derived) interpreter shim.
"""
import sys, os, struct, zlib, argparse, json
from collections import defaultdict
sys.path.insert(0, os.path.dirname(__file__))
from pspelf import PspElf
from mips import decode, rs, rt, rd, sa, imm16, simm16, target26

INTERP_ENCODINGS_PREFIX = ('v', 'lv', 'sv', 'mfv', 'mtv')

BRANCH2 = {'beq': '==', 'bne': '!='}
BRANCH1 = {'blez': '<= 0', 'bgtz': '> 0', 'bltz': '< 0', 'bgez': '>= 0'}


def is_vfpu(d):
    i = d['interp']
    return i in ('Int_SV', 'Int_SVQ', 'Int_Vflush', 'Int_Mftv') or i.startswith('Int_V') and i != 'Int_VBranch' \
        or d['name'] in ('vpfxs', 'vpfxt', 'vpfxd', 'viim.s', 'vfim.s')


class Recomp:
    def __init__(self, path, cname, extra_seeds=()):
        self.e = e = PspElf(path)
        self.cname = cname
        self.ts = e.text.addr
        self.te = e.text.addr + e.text.size
        # code sections right next to .text (.init before it, .fini after it, as in the network PRXs)
        for nm in ('.init', '.fini'):
            sec = e.secmap.get(nm)
            if sec and sec.size and sec.flags & 4:
                if 0 <= self.ts - (sec.addr + sec.size) < 64: self.ts = sec.addr   # allow alignment padding
                elif sec.addr == self.te: self.te = sec.addr + sec.size
        self.stubs = {}
        for imp in e.imports:
            self.stubs[imp['stub']] = imp
        self.stub_lo = min(self.stubs) if self.stubs else 0
        self.stub_hi = max(self.stubs) + 8 if self.stubs else 0
        self.warnings = []
        self._pair_relocs()
        self.extra_seeds = set(extra_seeds)
        self.hook_sites = {}   # addr -> hook dict
        self.thunks = {}       # addr -> C thunk body for hooked calls to import stubs
        hp = os.path.join(os.path.dirname(__file__), '..', 'data', 'hooks.json')
        if os.path.exists(hp):
            for h in json.load(open(hp))['hooks']:
                if h.get('match') == 'dt30':
                    n = 0
                    for a in range(self.ts, self.te, 4):
                        op = self.op(a)
                        if (op >> 26) == 0x0d and (op & 0xFFFF) == 0x8889 and rs(op) == rt(op):
                            r = rt(op)
                            for b in range(a - 4, max(self.ts, a - 36) - 4, -4):
                                pb = self.op(b)
                                if (pb >> 26) == 0x0f and rt(pb) == r:
                                    if (pb & 0xFFFF) == 0x3d08 and a not in self.hook_sites:
                                        self.hook_sites[a] = h
                                        n += 1
                                    break
                    if n:
                        self.warn('hook %s x%d' % (h['name'], n))
                    continue
                pat = []
                for w in h['pattern']:
                    v, _, m = w.partition('/')
                    pat.append((int(v, 16), int(m, 16) if m else 0xFFFFFFFF))
                hits = []
                for a in range(self.ts, self.te - 4 * len(pat) + 4, 4):
                    if all((self.op(a + 4 * i) & m) == (v & m) for i, (v, m) in enumerate(pat)):
                        hits.append(a + 4 * h['index'])
                for a in hits:
                    self.hook_sites[a] = h
                if hits:
                    self.warn('hook %s at %s' % (h['name'], ','.join('%x' % x for x in hits)))

    def warn(self, s):
        self.warnings.append(s)

    def op(self, a):
        return self.e.u32(a)

    def in_text(self, a):
        return self.ts <= a < self.te

    # ---------------------------------------------------------------- relocations
    def _pair_relocs(self):
        """Compute the full value of every HI16/LO16 pair (PSP pairing: HI16 pairs with next non-HI16)."""
        e = self.e
        rel = e.relocs
        self.hi_reloc = set()
        # imported variables (e.g. the multiplayer module's RCPM variable): their HI16 relocations become
        # `(imm<<16) + <resolved address>`, read from the import's nid word, which the loader overwrites with
        # the variable's address. The LO16 halves keep their immediates (the sum is the same).
        self.var_hi = {}
        for lib, nid, slot, tab in e.import_var_slots:
            k = 0
            while True:
                w = e.u32(tab + 4 * k)
                if w == 0: break
                ty, at = w >> 26, (w & 0x3FFFFFF) << 2
                if ty == 5: self.var_hi[at] = slot
                elif ty != 6: self.warn('unsupported variable import reloc type %d at %08x' % (ty, at))
                k += 1
        self.lo_value = {}   # addr of lo instruction -> full value (image relative)
        self.hi_value = {}
        pending = []
        for a, t, ts in rel:
            if t == 5:
                pending.append(a)
                self.hi_reloc.add(a)
            elif t in (6, 1) and pending:
                lo = e.u32(a) & 0xFFFF
                lo = lo - 0x10000 if lo & 0x8000 else lo
                for h in pending:
                    full = ((e.u32(h) & 0xFFFF) << 16) + lo
                    self.hi_value[h] = full & 0xFFFFFFFF
                self.lo_value[a] = (((e.u32(pending[-1]) & 0xFFFF) << 16) + lo) & 0xFFFFFFFF
                pending = []
            elif t == 6:
                # LO16 without preceding HI16 in this run: belongs to the last HI group (PSP behaviour)
                pass
        self.r32 = {a for a, t, _ in rel if t == 2}
        self.r26 = {a for a, t, _ in rel if t == 4}

    # ---------------------------------------------------------------- analysis
    def analyze(self):
        e = self.e
        seeds = set()
        seeds.add(e.entry)
        for lib, nid, val, kind in e.exports:
            if kind == 'func' and self.in_text(val):
                seeds.add(val)
        for a in range(self.ts, self.te, 4):
            op = self.op(a)
            d = decode(op)
            if d and d['name'] in ('jal',):
                t = target26(op) | (a + 4) & 0xF0000000
                if self.in_text(t):
                    seeds.add(t)
                elif not (self.stub_lo <= t < self.stub_hi):
                    self.warn('jal to %08x outside text at %08x' % (t, a))
        # jump tables
        self.jumptables = {}   # jr addr -> list of targets
        jt_entry_addrs = set()
        for a in range(self.ts, self.te, 4):
            op = self.op(a)
            d = decode(op)
            if d and d['name'] == 'jr' and rs(op) != 31:
                tab = self.find_jumptable(a, rs(op))
                if tab:
                    base, n = tab
                    targets = []
                    for i in range(n):
                        ea = base + 4 * i
                        if ea not in self.r32:
                            break
                        tv = e.u32(ea)
                        if not self.in_text(tv):
                            break
                        targets.append(tv)
                        jt_entry_addrs.add(ea)
                    if targets:
                        self.jumptables[a] = targets
                    else:
                        self.warn('empty jumptable at %08x' % a)
                else:
                    self.warn('unresolved jr r%d at %08x' % (rs(op), a))
        # data pointers into text = function pointers (vtables, callbacks)
        for a in self.r32:
            if self.in_text(a) or a in jt_entry_addrs:
                continue
            v = e.u32(a)
            if self.in_text(v) and v % 4 == 0:
                seeds.add(v)
        # lui/addiu pairs in code that build text addresses (thread entry points, callbacks)
        for h, v in self.hi_value.items():
            if self.in_text(h) and self.in_text(v) and v % 4 == 0:
                seeds.add(v)
        for a, v in self.lo_value.items():
            if self.in_text(a) and self.in_text(v) and v % 4 == 0:
                seeds.add(v)
        seeds |= {s for s in self.extra_seeds if self.in_text(s)}
        seeds = {s for s in seeds if self.in_text(s)}
        # split until every branch target is local or a function start
        while True:
            fl = sorted(seeds)
            added = set()
            for i, s in enumerate(fl):
                end = fl[i + 1] if i + 1 < len(fl) else self.te
                for a in range(s, end, 4):
                    for t in self.local_targets(a):
                        if not (s <= t < end) and self.in_text(t) and t not in seeds:
                            added.add(t)
            if not added:
                break
            seeds |= added
        self.funcs = sorted(seeds)
        self.func_set = set(self.funcs)
        self.func_end = {}
        for i, s in enumerate(self.funcs):
            self.func_end[s] = self.funcs[i + 1] if i + 1 < len(self.funcs) else self.te

    def local_targets(self, a):
        op = self.op(a)
        d = decode(op)
        if not d:
            return []
        n = d['name']
        if d['interp'] in ('Int_RelBranch', 'Int_RelBranchRI', 'Int_FPUBranch', 'Int_VBranch'):
            if n in ('bltzal', 'bgezal', 'bltzall', 'bgezall'):
                return []
            return [(a + 4 + (simm16(op) << 2)) & 0xFFFFFFFF]
        if n == 'j':
            return [target26(op) | (a + 4) & 0xF0000000]
        if n == 'jr' and a in getattr(self, 'jumptables', {}):
            return self.jumptables[a]
        return []

    def find_jumptable(self, a, reg):
        """Look back from `jr reg` for `lw reg, lo(base)` whose LO16 reloc gives the table, and sltiu bound."""
        lw_addr = None
        for b in range(a - 4, max(self.ts, a - 64) - 4, -4):
            op = self.op(b)
            d = decode(op)
            if d and d['name'] == 'lw' and rt(op) == reg:
                lw_addr = b
                break
        if lw_addr is None:
            return None
        if lw_addr in self.lo_value:
            base = self.lo_value[lw_addr]
        else:
            # `lui/addiu base; addu p, idx, base; lw reg, 0(p)`: take the nearest preceding LO16 that
            # builds a data address
            base = None
            for b in range(lw_addr - 4, max(self.ts, lw_addr - 48) - 4, -4):
                v = self.lo_value.get(b)
                if v is not None and not self.in_text(v):
                    base = v
                    break
            if base is None:
                return None
        n = None
        for b in range(lw_addr - 4, max(self.ts, lw_addr - 80) - 4, -4):
            op = self.op(b)
            d = decode(op)
            if d and d['name'] == 'sltiu':
                n = imm16(op)
                break
        if n is None:
            n = 512
        return base, n

    # ---------------------------------------------------------------- codegen
    def fname(self, a):
        return 'F_%s_%05x' % (self.cname, a)

    def gen_all(self, outdir, per_file=200):
        os.makedirs(outdir, exist_ok=True)
        cn = self.cname
        hdr = ['#pragma once', '#include "rt.h"', 'extern u32 MB_%s;' % cn]
        for f in self.funcs:
            hdr.append('void %s(Ctx *c);' % self.fname(f))
        # thunks are only known after codegen; generate code first, then write the header
        self._hdr = hdr
        files = []
        self.interp_used = set()
        for k in range(0, len(self.funcs), per_file):
            body = ['#include "%s_funcs.h"' % cn, '#define MB MB_%s' % cn, '']
            for f in self.funcs[k:k + per_file]:
                body.append(self.gen_func(f))
            fn = '%s_%03d.c' % (cn, k // per_file)
            write_if_changed(os.path.join(outdir, fn), '\n'.join(body))
            files.append(fn)
        hdr = self._hdr + sorted(self.thunks.values())
        write_if_changed(os.path.join(outdir, '%s_funcs.h' % cn), '\n'.join(hdr) + '\n')
        # module table
        crc = zlib.crc32(self.e.d) & 0xFFFFFFFF
        tab = ['#include "%s_funcs.h"' % cn, 'u32 MB_%s;' % cn,
               'static const FuncEnt funcs_%s[] = {' % cn]
        for f in self.funcs:
            tab.append('  {0x%05x, %s},' % (f, self.fname(f)))
        tab.append('};')
        tab.append('const RecompModule mod_%s = {"%s", 0x%08x, %d, &MB_%s, funcs_%s, %d, 0x%x, 0x%x};' % (
            cn, cn, crc, len(self.e.d), cn, cn, len(self.funcs), self.ts, self.te))
        write_if_changed(os.path.join(outdir, '%s_table.c' % cn), '\n'.join(tab) + '\n')
        files.append('%s_table.c' % cn)
        return files

    def gen_func(self, start):
        end = self.func_end[start]
        labels = set()
        for a in range(start, end, 4):
            for t in self.local_targets(a):
                if start <= t < end:
                    labels.add(t)
        out = ['void %s(Ctx *c) {' % self.fname(start)]
        eh = self.hook_sites.get(start)
        if eh and eh['kind'] == 'entry':
            # entry hook: the function becomes a thunk; the runtime decides whether/how to run the body
            body = '%s_body' % self.fname(start)
            out = ['static void %s(Ctx *c);' % body,
                   'void %s(Ctx *c) { rt_hookcall(c, %d, %s); } /* hook %s */' % (self.fname(start), eh['id'], body, eh['name']),
                   'static void %s(Ctx *c) {' % body]
        a = start
        while a < end:
            if a in labels:
                out.append('L_%05x:;' % a)
            op = self.op(a)
            d = decode(op)
            if d is None:
                out.append('  rt_bad_insn(c, MB + 0x%x, 0x%08x); return;' % (a, op))
                a += 4
                continue
            if 'DELAYSLOT' in d['flags']:
                out.append(self.gen_branch(a, op, d, start, end))
                if a + 4 in labels and a + 4 < end:
                    # something branches into the delay slot: emit it again as its own block
                    out.append('  goto L_%05x_ds;' % (a + 8))
                    out.append('L_%05x:;' % (a + 4))
                    out.append(self.gen_insn(a + 4))
                    out.append('L_%05x_ds:;' % (a + 8))
                a += 8
            else:
                out.append(self.gen_insn(a))
                a += 4
        # fell off the end: continue into the next function
        last = self.op(end - 8) if end - 8 >= start else 0
        out.append('  ' + self.tail(end) + ';')
        out.append('}')
        return '\n'.join(out) + '\n'

    def tail(self, t):
        if t in self.func_set:
            return 'return %s(c)' % self.fname(t)
        if self.in_text(t):
            return 'return rt_jump(c, MB + 0x%x)' % t
        return 'return rt_jump(c, 0x%x)' % t

    def target_code(self, t, start, end):
        if start <= t < end:
            return 'goto L_%05x' % t
        return self.tail(t)

    def gen_branch(self, a, op, d, start, end):
        n = d['name']
        ds = self.gen_insn(a + 4) if a + 4 < self.te else ''
        R = self.rd_reg
        pc8 = 'MB + 0x%x' % (a + 8)
        cmt = '  /* %05x %s */' % (a, n)
        if n in ('j',):
            t = target26(op)
            return '%s\n%s\n  %s;' % (cmt, ds, self.target_code(t, start, end))
        if n == 'jal' and a in self.hook_sites and self.hook_sites[a]['kind'] == 'call':
            h = self.hook_sites[a]
            t = target26(op)
            if t in self.stubs:
                fn = 'thunk_%s_%05x' % (self.cname, a)
                self.thunks[a] = 'static inline void %s(Ctx *c) { HLE(c, 0x%08X); }' % (fn, self.stubs[t]['nid'])
            elif t in self.func_set:
                fn = self.fname(t)
            else:
                raise Exception('hook %s: call target %x not a function' % (h['name'], t))
            return '%s\n  c->r[31] = %s;\n%s\n  rt_hookcall(c, %d, %s); /* hook %s */' % (cmt, pc8, ds, h['id'], fn, h['name'])
        if n == 'jal':
            t = target26(op)
            if t in self.stubs:
                imp = self.stubs[t]
                return '%s\n  c->r[31] = %s;\n%s\n  HLE(c, 0x%08X); /* %s */' % (cmt, pc8, ds, imp['nid'], imp['name'])
            if t in self.func_set:
                return '%s\n  c->r[31] = %s;\n%s\n  %s(c);' % (cmt, pc8, ds, self.fname(t))
            return '%s\n  c->r[31] = %s;\n%s\n  rt_call(c, MB + 0x%x);' % (cmt, pc8, ds, t)
        if n == 'jr':
            r = rs(op)
            if r == 31:
                return '%s\n%s\n  return;' % (cmt, ds)
            s = '%s\n  { u32 t_ = %s;\n%s\n' % (cmt, R(r), ds)
            if a in self.jumptables:
                s += '  switch (t_ - MB) {\n'
                for t in sorted(set(self.jumptables[a])):
                    s += '    case 0x%x: %s;\n' % (t, self.target_code(t, start, end))
                s += '  }\n'
            s += '  return rt_jump(c, t_); }'
            return s
        if n == 'jalr':
            r, rdst = rs(op), rd(op)
            s = '%s\n  { u32 t_ = %s;\n' % (cmt, R(r))
            if rdst:
                s += '  c->r[%d] = %s;\n' % (rdst, pc8)
            s += '%s\n  rt_call(c, t_); }' % ds
            return s
        # conditional branches
        t = (a + 4 + (simm16(op) << 2)) & 0xFFFFFFFF
        likely = n.endswith('l') and n not in ('bltzal', 'bgezal', 'bvtl', 'bvfl', 'bc1tl', 'bc1fl') or \
            n in ('bltzall', 'bgezall', 'bvtl', 'bvfl', 'bc1tl', 'bc1fl')
        base = n[:-1] if likely else n
        link = base in ('bltzal', 'bgezal')
        if link:
            base = base[:-2]
        if base in BRANCH2:
            cond = '%s %s %s' % (R(rs(op)), BRANCH2[base], R(rt(op)))
        elif base in BRANCH1:
            cond = '(s32)%s %s' % (R(rs(op)), BRANCH1[base])
        elif base == 'bc1t':
            cond = 'c->fpcond'
        elif base == 'bc1f':
            cond = '!c->fpcond'
        elif base in ('bvt', 'bvf'):
            bit = (op >> 18) & 7
            cond = '((c->vfpuCtrl[VFPU_CTRL_CC] >> %d) & 1)' % bit
            if base == 'bvf':
                cond = '!' + cond
        else:
            raise Exception('unknown branch %s at %x' % (n, a))
        if link:
            if likely:
                return '%s\n  c->r[31] = %s; if (%s) {\n%s\n  rt_call(c, MB + 0x%x); }' % (cmt, pc8, cond, ds, t)
            return '%s\n  { int b_ = %s; c->r[31] = %s;\n%s\n  if (b_) rt_call(c, MB + 0x%x); }' % (cmt, cond, pc8, ds, t)
        tc = self.target_code(t, start, end)
        if likely:
            return '%s\n  if (%s) {\n%s\n  %s; }' % (cmt, cond, ds, tc)
        if not ds.strip():
            return '%s\n  if (%s) %s;' % (cmt, cond, tc)
        return '%s\n  { int b_ = %s;\n%s\n  if (b_) %s; }' % (cmt, cond, ds, tc)

    @staticmethod
    def rd_reg(r):
        return '0u' if r == 0 else 'c->r[%d]' % r

    def gen_insn(self, a):
        code = self.gen_insn_raw(a)
        h = self.hook_sites.get(a)
        if h and h['kind'] == 'value':
            r = rt(self.op(a))
            reg = 'fi' if (self.op(a) >> 26) == 0x31 else 'r'   # lwc1: hook the loaded FPU register
            code += '\n  c->%s[%d] = rt_hookval(c, %d, c->%s[%d]); /* hook %s */' % (reg, r, h['id'], reg, r, h['name'])
        return code

    def gen_insn_raw(self, a):
        op = self.op(a)
        d = decode(op)
        R = self.rd_reg
        if d is None:
            return '  rt_bad_insn(c, MB + 0x%x, 0x%08x);' % (a, op)
        n = d['name']
        if op == 0:
            return ''
        if 'DELAYSLOT' in d['flags']:
            self.warn('branch in delay slot at %08x' % a)
            return '  rt_bad_insn(c, MB + 0x%x, 0x%08x); /* branch in delay slot */' % (a, op)
        s_, t_, d_, sh = rs(op), rt(op), rd(op), sa(op)
        imm, simm = imm16(op), simm16(op)
        W = lambda r, expr: ('  c->r[%d] = %s;' % (r, expr)) if r else ''
        ea = '(%s + %d)' % (R(s_), simm) if simm else R(s_)
        if a in self.e.reloc_at and n not in ('lui',):
            types = [t for t, _ in self.e.reloc_at[a]]
            if any(t not in (6, 1) for t in types):
                self.warn('unexpected reloc %s on %s at %08x' % (types, n, a))
        # ---- immediate ALU
        if n in ('addiu', 'addi'):
            return W(t_, '%s + %du' % (R(s_), simm & 0xFFFFFFFF))
        if n == 'slti':
            return W(t_, '(s32)%s < %d' % (R(s_), simm))
        if n == 'sltiu':
            return W(t_, '%s < %du' % (R(s_), simm & 0xFFFFFFFF))
        if n == 'andi':
            return W(t_, '%s & 0x%x' % (R(s_), imm))
        if n == 'ori':
            return W(t_, '%s | 0x%x' % (R(s_), imm))
        if n == 'xori':
            return W(t_, '%s ^ 0x%x' % (R(s_), imm))
        if n == 'lui':
            if a in self.hi_reloc:
                return W(t_, '0x%xu + MB' % (imm << 16))
            if a in self.var_hi:
                return W(t_, '0x%xu + LW(MB + 0x%xu)' % (imm << 16, self.var_hi[a]))
            return W(t_, '0x%xu' % (imm << 16))
        # ---- loads / stores
        loads = {'lb': 'LB', 'lbu': 'LBU', 'lh': 'LH', 'lhu': 'LHU', 'lw': 'LW'}
        if n in loads:
            return W(t_, '%s(%s)' % (loads[n], ea))
        stores = {'sb': 'SB', 'sh': 'SH', 'sw': 'SW'}
        if n in stores:
            return '  %s(%s, %s);' % (stores[n], ea, R(t_))
        if n in ('lwl', 'lwr', 'swl', 'swr'):
            return '  { u32 a_ = %s; %s }' % (ea, {
                'lwl': W(t_, 'rt_lwl(a_, %s)' % R(t_)) or ';',
                'lwr': W(t_, 'rt_lwr(a_, %s)' % R(t_)) or ';',
                'swl': 'rt_swl(a_, %s);' % R(t_),
                'swr': 'rt_swr(a_, %s);' % R(t_)}[n])
        if n == 'll':
            return W(t_, 'LW(%s)' % ea) + ' c->llbit = 1;'
        if n == 'sc':
            return '  SW(%s, %s);' % (ea, R(t_)) + W(t_, '1')
        if n == 'lwc1':
            return '  c->fi[%d] = LW(%s);' % (t_, ea)
        if n == 'swc1':
            return '  SW(%s, c->fi[%d]);' % (ea, t_)
        if n == 'cache' or n == 'sync' or n == 'halt':
            return '  /* %s */' % n
        # ---- shifts
        if n == 'sll':
            return W(d_, '%s << %d' % (R(t_), sh))
        if n == 'srl':
            if s_ & 1:
                return W(d_, 'ROTR(%s, %d)' % (R(t_), sh))
            return W(d_, '%s >> %d' % (R(t_), sh))
        if n == 'sra':
            return W(d_, '(u32)((s32)%s >> %d)' % (R(t_), sh))
        if n == 'sllv':
            return W(d_, '%s << (%s & 31)' % (R(t_), R(s_)))
        if n == 'srlv':
            if sh & 1:
                return W(d_, 'ROTR(%s, %s & 31)' % (R(t_), R(s_)))
            return W(d_, '%s >> (%s & 31)' % (R(t_), R(s_)))
        if n == 'srav':
            return W(d_, '(u32)((s32)%s >> (%s & 31))' % (R(t_), R(s_)))
        # ---- 3-reg ALU
        alu3 = {'addu': '+', 'add': '+', 'subu': '-', 'sub': '-', 'and': '&', 'or': '|', 'xor': '^'}
        if n in alu3:
            if n == 'or' and t_ == 0:
                return W(d_, R(s_))
            return W(d_, '%s %s %s' % (R(s_), alu3[n], R(t_)))
        if n == 'nor':
            return W(d_, '~(%s | %s)' % (R(s_), R(t_)))
        if n == 'slt':
            return W(d_, '(s32)%s < (s32)%s' % (R(s_), R(t_)))
        if n == 'sltu':
            return W(d_, '%s < %s' % (R(s_), R(t_)))
        if n == 'max':
            return W(d_, '(u32)MAX_S32(%s, %s)' % (R(s_), R(t_)))
        if n == 'min':
            return W(d_, '(u32)MIN_S32(%s, %s)' % (R(s_), R(t_)))
        if n == 'movz':
            return ('  if (%s == 0) c->r[%d] = %s;' % (R(t_), d_, R(s_))) if d_ else ''
        if n == 'movn':
            return ('  if (%s != 0) c->r[%d] = %s;' % (R(t_), d_, R(s_))) if d_ else ''
        if n == 'clz':
            return W(d_, 'CLZ32(%s)' % R(s_))
        if n == 'clo':
            return W(d_, 'CLZ32(~%s)' % R(s_))
        # ---- hi/lo
        if n == 'mfhi':
            return W(d_, 'c->hi')
        if n == 'mflo':
            return W(d_, 'c->lo')
        if n == 'mthi':
            return '  c->hi = %s;' % R(s_)
        if n == 'mtlo':
            return '  c->lo = %s;' % R(s_)
        if n in ('mult', 'multu', 'div', 'divu', 'madd', 'maddu', 'msub', 'msubu'):
            return '  rt_%s(c, %s, %s);' % (n, R(s_), R(t_))
        # ---- allegrex
        if n == 'ext':
            size = d_ + 1
            mask = (1 << size) - 1 if size < 32 else 0xFFFFFFFF
            return W(t_, '(%s >> %d) & 0x%xu' % (R(s_), sh, mask))
        if n == 'ins':
            lsb, msb = sh, d_
            size = msb - lsb + 1
            mask = (((1 << size) - 1) << lsb) & 0xFFFFFFFF
            return W(t_, '(%s & 0x%xu) | ((%s << %d) & 0x%xu)' % (R(t_), (~mask) & 0xFFFFFFFF, R(s_), lsb, mask))
        if n == 'seb':
            return W(d_, '(u32)(s32)(s8)%s' % R(t_))
        if n == 'seh':
            return W(d_, '(u32)(s32)(s16)%s' % R(t_))
        if n == 'wsbh':
            return W(d_, 'WSBH(%s)' % R(t_))
        if n == 'wsbw':
            return W(d_, '__builtin_bswap32(%s)' % R(t_))
        if n == 'bitrev':
            return W(d_, 'rt_bitrev(%s)' % R(t_))
        if n == 'mfic':
            return W(t_, 'c->intr')
        if n == 'mtic':
            return '  c->intr = %s;' % R(t_)
        if n == 'syscall':
            return '  rt_syscall(c, MB + 0x%x, 0x%08x);' % (a, op)
        if n == 'break':
            return '  rt_break(c, MB + 0x%x, 0x%08x);' % (a, op)
        # ---- FPU
        ft, fs, fd = t_, d_, sh
        if n == 'mfc1':
            return W(t_, 'c->fi[%d]' % fs)
        if n == 'mtc1':
            return '  c->fi[%d] = %s;' % (fs, R(t_))
        if n == 'cfc1':
            return W(t_, 'rt_cfc1(c, %d)' % fs)
        if n == 'ctc1':
            return '  rt_ctc1(c, %d, %s);' % (fs, R(t_))
        fop3 = {'add.s': '+', 'sub.s': '-', 'mul.s': '*', 'div.s': '/'}
        if n in fop3:
            return '  c->f[%d] = c->f[%d] %s c->f[%d];' % (fd, fs, fop3[n], ft)
        if n == 'mov.s':
            return '  c->fi[%d] = c->fi[%d];' % (fd, fs)
        if n == 'neg.s':
            return '  c->f[%d] = -c->f[%d];' % (fd, fs)
        if n == 'abs.s':
            return '  c->f[%d] = fabsf(c->f[%d]);' % (fd, fs)
        if n == 'sqrt.s':
            return '  c->f[%d] = rt_sqrt(c->f[%d]);' % (fd, fs)
        if n == 'cvt.s.w':
            return '  c->f[%d] = (float)c->fs[%d];' % (fd, fs)
        cv = {'round.w.s': 'rt_round_w', 'trunc.w.s': 'rt_trunc_w', 'ceil.w.s': 'rt_ceil_w',
              'floor.w.s': 'rt_floor_w'}
        if n in cv:
            return '  c->fs[%d] = %s(c->f[%d]);' % (fd, cv[n], fs)
        if n == 'cvt.w.s':
            return '  c->fs[%d] = rt_cvt_w(c, c->f[%d]);' % (fd, fs)
        if d['interp'] == 'Int_FPUComp':
            return '  c->fpcond = rt_fcmp(c->f[%d], c->f[%d], %d);' % (fs, ft, op & 0xF)
        if n in ('mfc0', 'mtc0'):
            return '  /* %s ignored */' % n
        # ---- VFPU & everything else -> interpreter shim
        if is_vfpu(d) or d['interp'] not in ('0',):
            self.interp_used.add(d['interp'])
            return '  INTERP(%s, c, MB + 0x%x, 0x%08xu); /* %s */' % (d['interp'], a, op, n)
        return '  rt_bad_insn(c, MB + 0x%x, 0x%08x); /* %s */' % (a, op, n)


def write_if_changed(path, text):
    if os.path.exists(path) and open(path).read() == text:
        return
    open(path, 'w').write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('cname')
    ap.add_argument('outdir')
    ap.add_argument('--extra-seeds')
    a = ap.parse_args()
    extra = []
    if a.extra_seeds and os.path.exists(a.extra_seeds):
        extra = [int(l.split()[0], 16) for l in open(a.extra_seeds) if l.strip() and not l.startswith('#')]
    r = Recomp(a.elf, a.cname, extra)
    r.analyze()
    files = r.gen_all(a.outdir)
    print('%s: %d functions, %d jumptables, %d files, interp: %s' % (
        a.cname, len(r.funcs), len(r.jumptables), len(files), ' '.join(sorted(r.interp_used))))
    from collections import Counter
    wc = Counter(w.split(' at ')[0] for w in r.warnings)
    for k, v in wc.most_common():
        print('  warn x%d: %s' % (v, k))
    open(os.path.join(a.outdir, '%s_warnings.txt' % a.cname), 'w').write('\n'.join(r.warnings))
    open(os.path.join(a.outdir, '%s_interp.txt' % a.cname), 'w').write('\n'.join(sorted(r.interp_used)))


if __name__ == '__main__':
    main()
