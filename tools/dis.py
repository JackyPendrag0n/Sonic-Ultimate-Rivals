"""Quick MIPS disassembler for a module (pre-relocated at base 0).  dis.py <elf> <start hex> [<end hex> | +count]"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from pspelf import PspElf
from mips import decode, rs, rt, rd, sa, simm16, imm16, target26

R = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
     's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


def fmt(e, a, stubs):
    op = e.u32(a)
    d = decode(op)
    if not d:
        return '.word 0x%08x' % op
    n, it = d['name'], d['interp']
    if op == 0:
        return 'nop'
    if it in ('Int_IType',):
        if n == 'lui':
            return '%s %s, 0x%x' % (n, R[rt(op)], imm16(op))
        if n in ('andi', 'ori', 'xori'):
            return '%s %s, %s, 0x%x' % (n, R[rt(op)], R[rs(op)], imm16(op))
        return '%s %s, %s, %d' % (n, R[rt(op)], R[rs(op)], simm16(op))
    if it in ('Int_ITypeMem', 'Int_StoreSync'):
        return '%s %s, %d(%s)' % (n, R[rt(op)], simm16(op), R[rs(op)])
    if it == 'Int_FPULS':
        return '%s f%d, %d(%s)' % (n, rt(op), simm16(op), R[rs(op)])
    if it in ('Int_RelBranch', 'Int_RelBranchRI', 'Int_FPUBranch', 'Int_VBranch'):
        t = a + 4 + (simm16(op) << 2)
        if n in ('beq', 'bne', 'beql', 'bnel'):
            return '%s %s, %s, %x' % (n, R[rs(op)], R[rt(op)], t)
        if it in ('Int_FPUBranch', 'Int_VBranch'):
            return '%s %x' % (n, t)
        return '%s %s, %x' % (n, R[rs(op)], t)
    if n in ('j', 'jal'):
        t = target26(op)
        if t in stubs:
            return '%s %s' % (n, stubs[t])
        return '%s %x' % (n, t)
    if n in ('jr',):
        return 'jr %s' % R[rs(op)]
    if n == 'jalr':
        return 'jalr %s, %s' % (R[rd(op)], R[rs(op)])
    if it == 'Int_ShiftType':
        if n in ('sll', 'srl', 'sra'):
            return '%s %s, %s, %d' % (n, R[rd(op)], R[rt(op)], sa(op))
        return '%s %s, %s, %s' % (n, R[rd(op)], R[rt(op)], R[rs(op)])
    if it in ('Int_RType3',):
        if n == 'or' and rt(op) == 0:
            return 'move %s, %s' % (R[rd(op)], R[rs(op)])
        return '%s %s, %s, %s' % (n, R[rd(op)], R[rs(op)], R[rt(op)])
    if it == 'Int_MulDivType':
        if n in ('mfhi', 'mflo'):
            return '%s %s' % (n, R[rd(op)])
        return '%s %s, %s' % (n, R[rs(op)], R[rt(op)])
    if it == 'Int_mxc1':
        return '%s %s, f%d' % (n, R[rt(op)], rd(op))
    if it in ('Int_FPU3op',):
        return '%s f%d, f%d, f%d' % (n, sa(op), rd(op), rt(op))
    if it in ('Int_FPU2op',):
        return '%s f%d, f%d' % (n, sa(op), rd(op))
    if it == 'Int_FPUComp':
        return '%s f%d, f%d' % (n, rd(op), rt(op))
    return '%s (%08x)' % (n, op)


def main():
    e = PspElf(sys.argv[1])
    s = int(sys.argv[2], 16)
    if len(sys.argv) > 3:
        x = sys.argv[3]
        end = s + 4 * int(x[1:]) if x.startswith('+') else int(x, 16)
    else:
        end = s + 4 * 40
    stubs = {i['stub']: i['name'] for i in e.imports}
    for a in range(s, end, 4):
        rel = e.reloc_at.get(a)
        print('%06x: %08x  %-40s%s' % (a, e.u32(a), fmt(e, a, stubs), '  ; reloc' if rel else ''))


if __name__ == '__main__':
    main()
