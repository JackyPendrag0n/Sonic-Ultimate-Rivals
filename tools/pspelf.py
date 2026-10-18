"""Minimal PSP ELF/PRX parser: sections, segments, PSP relocations, module info, imports, exports."""
import struct, json, os

SHT_PSP_REL = 0x700000A0
R_NAMES = {0: 'NONE', 1: '16', 2: '32', 4: '26', 5: 'HI16', 6: 'LO16', 7: 'GPREL16'}

_NIDS = None
def nid_db():
    global _NIDS
    if _NIDS is None:
        p = os.path.join(os.path.dirname(__file__), '..', 'data', 'nids.json')
        _NIDS = {int(k, 16): v for k, v in json.load(open(p)).items()}
    return _NIDS


class Section:
    pass


class PspElf:
    def __init__(self, path):
        self.path = path
        self.d = d = open(path, 'rb').read()
        (self.e_type, self.e_machine, _, self.entry, phoff, shoff, self.flags, _, phentsize,
         phnum, shentsize, shnum, shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', d, 16)
        self.segs = []
        for i in range(phnum):
            t, off, va, pa, fsz, msz, fl, al = struct.unpack_from('<8I', d, phoff + i * phentsize)
            self.segs.append(dict(type=t, off=off, vaddr=va, paddr=pa, filesz=fsz, memsz=msz))
        self.sections = []
        for i in range(shnum):
            s = Section()
            (s.name_off, s.type, s.flags, s.addr, s.off, s.size, s.link, s.info, s.align,
             s.entsize) = struct.unpack_from('<10I', d, shoff + i * shentsize)
            self.sections.append(s)
        strtab = self.sections[shstrndx]
        for s in self.sections:
            e = d.index(b'\0', strtab.off + s.name_off)
            s.name = d[strtab.off + s.name_off:e].decode()
        self.secmap = {s.name: s for s in self.sections}
        # image: the loaded memory image at base 0 (segments are laid out from vaddr 0)
        size = max(sg['vaddr'] + sg['memsz'] for sg in self.segs)
        img = bytearray(size)
        for sg in self.segs:
            if sg['type'] != 1: continue   # PT_LOAD only (some PRXs also have a relocation segment at vaddr 0)
            img[sg['vaddr']:sg['vaddr'] + sg['filesz']] = d[sg['off']:sg['off'] + sg['filesz']]
        self.image = img
        self.img_size = size
        self.relocs = []  # (addr, type, target_seg) addresses are image-relative
        for s in self.sections:
            if s.type == SHT_PSP_REL:
                for k in range(s.size // 8):
                    off, info = struct.unpack_from('<II', d, s.off + k * 8)
                    typ = info & 0xFF
                    oseg = (info >> 8) & 0xFF
                    tseg = (info >> 16) & 0xFF
                    addr = off + self.segs[oseg]['vaddr']
                    self.relocs.append((addr, typ, self.segs[tseg]['vaddr'] if tseg < len(self.segs) else 0))
        self.reloc_at = {}
        for a, t, ts in self.relocs:
            self.reloc_at.setdefault(a, []).append((t, ts))
        # Pre-relocate to base 0 (so second-segment targets get their segment offset added). The
        # runtime then relocates the original file to a 64KiB-aligned base B; that equals this image
        # with HI16 += B>>16, 32 += B, 26 += B>>2 and LO16 unchanged.
        self.orig_image = bytes(img)
        self.image = self.relocate(0)
        self.text = self.secmap['.text']
        self._parse_modinfo()
        self._parse_imports()
        self._parse_exports()

    def relocate(self, base):
        img = bytearray(self.orig_image)
        ops = [struct.unpack_from('<I', self.orig_image, a)[0] for a, _, _ in self.relocs]
        rel = self.relocs
        for i, (a, t, ts) in enumerate(rel):
            op = ops[i]
            to = base + ts
            if t == 2:
                op = (op + to) & 0xFFFFFFFF
            elif t == 4:
                op = (op & 0xFC000000) | (((op & 0x03FFFFFF) + (to >> 2)) & 0x03FFFFFF)
            elif t == 5:
                for j in range(i + 1, len(rel)):
                    if rel[j][1] == 5:
                        continue
                    lo = ops[j] & 0xFFFF
                    lo = lo - 0x10000 if lo & 0x8000 else lo
                    full = (((op & 0xFFFF) << 16) + lo + to) & 0xFFFFFFFF
                    hi = ((full >> 16) + (1 if full & 0x8000 else 0)) & 0xFFFF
                    op = (op & 0xFFFF0000) | hi
                    break
            elif t == 6:
                op = (op & 0xFFFF0000) | (((op & 0xFFFF) + to) & 0xFFFF)
            elif t == 1:
                op = (op & 0xFFFF0000) | (((op & 0xFFFF) + to) & 0xFFFF)
            else:
                continue
            struct.pack_into('<I', img, a, op)
        return img

    def u32(self, a):
        return struct.unpack_from('<I', self.image, a)[0]

    def cstr(self, a):
        e = self.image.index(b'\0', a)
        return self.image[a:e].decode('latin1')

    def _parse_modinfo(self):
        s = self.secmap['.rodata.sceModuleInfo']
        a = s.addr
        attr, ver = struct.unpack_from('<HH', self.image, a)
        name = self.image[a + 4:a + 32].split(b'\0')[0].decode()
        gp, ent_top, ent_end, stub_top, stub_end = struct.unpack_from('<5I', self.image, a + 32)
        self.modinfo = dict(addr=a, attr=attr, ver=ver, name=name, gp=gp, ent_top=ent_top,
                            ent_end=ent_end, stub_top=stub_top, stub_end=stub_end)

    def _parse_imports(self):
        db = nid_db()
        self.imports = []  # dict(lib, nid, name, stub_addr)
        self.import_vars = []
        self.import_var_slots = []  # (lib, nid, slot address (the nid word), reloc table address)
        a = self.modinfo['stub_top']
        while a < self.modinfo['stub_end']:
            name_p, ver, flags = struct.unpack_from('<IHH', self.image, a)
            size, nvars, nfuncs = struct.unpack_from('<BBH', self.image, a + 8)
            nidp, symp = struct.unpack_from('<II', self.image, a + 12)
            varp = self.u32(a + 20) if size >= 6 else 0
            lib = self.cstr(name_p) if name_p else '?'
            for i in range(nfuncs):
                nid = self.u32(nidp + 4 * i)
                info = db.get(nid)
                self.imports.append(dict(lib=lib, nid=nid, name=info['name'] if info else 'unk_%08X' % nid,
                                         stub=symp + 8 * i, known=bool(info)))
            for i in range(nvars):
                self.import_vars.append((lib, self.u32(varp + 8 * i), self.u32(varp + 8 * i + 4)))
                self.import_var_slots.append((lib, self.u32(varp + 8 * i + 4), varp + 8 * i + 4, self.u32(varp + 8 * i)))
            a += size * 4

    def _parse_exports(self):
        self.exports = []  # (lib, nid, addr, kind)
        a = self.modinfo['ent_top']
        while a < self.modinfo['ent_end']:
            name_p, ver, attr = struct.unpack_from('<IHH', self.image, a)
            size, nvars, nfuncs = struct.unpack_from('<BBH', self.image, a + 8)
            tbl = self.u32(a + 12)
            lib = self.cstr(name_p) if name_p else 'syslib'
            n = nfuncs + nvars
            for i in range(n):
                nid = self.u32(tbl + 4 * i)
                val = self.u32(tbl + 4 * (n + i))
                self.exports.append((lib, nid, val, 'func' if i < nfuncs else 'var'))
            a += max(size, 4) * 4


if __name__ == '__main__':
    import sys
    e = PspElf(sys.argv[1])
    print('module', e.modinfo)
    print('entry %08x  image %x  relocs %d' % (e.entry, e.img_size, len(e.relocs)))
    from collections import Counter
    print('reloc types', Counter(R_NAMES.get(t, t) for _, t, _ in e.relocs))
    print('exports:')
    for x in e.exports:
        print('   %s %08X -> %08x %s' % (x[0], x[1], x[2], x[3]))
    print('imports (%d, unknown %d):' % (len(e.imports), sum(not i['known'] for i in e.imports)))
    for i in e.imports:
        print('   %-20s %08X %-40s stub %08x' % (i['lib'], i['nid'], i['name'], i['stub']))
    print('import vars', e.import_vars)
