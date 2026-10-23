#!/usr/bin/env python3
"""Unpack your own Sonic Rivals 2 disc image for the build:  python3 tools/setup_iso.py <game.iso> [folder]

Writes the disc's files to <folder> (default: "game" next to this repository's build.sh), checks that the
image is the US release (ULUS10323) this port is made for, and that its program file can be recompiled.
Standard library only, so it runs the same on Linux, macOS and Windows.

Nothing from the game is part of this repository: the build reads the program code from this folder and
the port loads the music, levels and videos from it at run time.
"""
import os, struct, sys

GAME_ID = "ULUS10323"
SKIP = ("PSP_GAME/SYSDIR/UPDATE/",)   # the firmware update on the disc (20 MB) isn't needed


def read_iso(path):
    f = open(path, "rb")
    def rd(lba, n):
        f.seek(lba * 2048)
        return f.read(n)
    pvd = rd(16, 2048)
    if pvd[1:6] != b"CD001":
        sys.exit("%s is not an ISO 9660 image. Make a plain .iso of the UMD (CSO/ZSO/CHD files must be "
                 "converted back to .iso first)." % path)
    files = []
    def walk(lba, size, prefix):
        data = rd(lba, size)
        o = 0
        while o < len(data):
            l = data[o]
            if l == 0:
                o = (o // 2048 + 1) * 2048
                continue
            r = data[o:o + l]
            o += l
            ext, sz, flags, nl = struct.unpack_from("<I", r, 2)[0], struct.unpack_from("<I", r, 10)[0], r[25], r[32]
            name = r[33:33 + nl]
            if name in (b"\0", b"\1"):
                continue
            name = name.decode("latin1").split(";")[0]
            p = prefix + name
            if flags & 2:
                walk(ext, sz, p + "/")
            else:
                files.append((p, ext, sz))
    root = pvd[156:190]
    walk(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "")
    return f, files


def param_sfo_id(blob):
    # PARAM.SFO: header, key table, data table; we want DISC_ID
    if blob[:4] != b"\0PSF":
        return None
    kt, dt, n = struct.unpack_from("<III", blob, 8)
    for i in range(n):
        ko, fmt, ln, mx, do = struct.unpack_from("<HHIII", blob, 20 + i * 16)
        key = blob[kt + ko:blob.index(b"\0", kt + ko)].decode()
        if key == "DISC_ID":
            return blob[dt + do:dt + do + ln].rstrip(b"\0").decode()
    return None


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__.strip())
        sys.exit(2)
    iso = sys.argv[1]
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "game")
    f, files = read_iso(iso)
    byname = {p.upper(): (p, e, s) for p, e, s in files}

    sfo = byname.get("PSP_GAME/PARAM.SFO")
    if not sfo:
        sys.exit("%s has no PSP_GAME/PARAM.SFO: not a PSP game disc." % iso)
    f.seek(sfo[1] * 2048)
    disc_id = param_sfo_id(f.read(sfo[2]))
    if disc_id != GAME_ID:
        sys.exit("This image is %s, but the port is made for Sonic Rivals 2 USA (%s). Other releases have "
                 "different code and won't work." % (disc_id or "unknown", GAME_ID))

    boot = byname.get("PSP_GAME/SYSDIR/BOOT.BIN")
    if not boot:
        sys.exit("PSP_GAME/SYSDIR/BOOT.BIN is missing from the image: is it a complete dump?")
    f.seek(boot[1] * 2048)
    if f.read(4) != b"\x7fELF":
        sys.exit("PSP_GAME/SYSDIR/BOOT.BIN in this image is not a plain program file, so it can't be recompiled.")

    total = sum(s for p, e, s in files if not p.upper().startswith(SKIP))
    done = 0
    print("Unpacking %s (%s) to %s" % (os.path.basename(iso), disc_id, out))
    for p, ext, sz in files:
        if p.upper().startswith(SKIP):
            continue
        dst = os.path.join(out, *p.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.exists(dst) and os.path.getsize(dst) == sz:
            done += sz
            continue
        f.seek(ext * 2048)
        with open(dst, "wb") as g:
            left = sz
            while left:
                chunk = f.read(min(left, 1 << 22))
                if not chunk:
                    sys.exit("%s ends early (inside %s): the dump is truncated." % (iso, p))
                g.write(chunk)
                left -= len(chunk)
        pct = (done + sz) * 100 // max(total, 1)
        if pct // 10 != done * 100 // max(total, 1) // 10:
            print("  %3d%%" % pct)
        done += sz
    print("  %d files, %.0f MB" % (len(files), total / 1e6))
    print("Done. Next: build with  ./build.sh \"%s\"  (Windows/MSYS2 and macOS: the same command)." % out)


if __name__ == "__main__":
    main()
