#!/usr/bin/env python3
"""Builds a PSP movie file (PSMF/.PMF): a 2048-byte PSMF header followed by an MPEG-2 program stream in
2048-byte packs, with H.264 video (stream 0xE0, encoded by ffmpeg/libx264) and silent ATRAC3+ audio
(private stream 1, PSP framing: 8-byte 0F D0 28 5C headers, 2048 samples per frame).

Part of the rcsm mpeg extraction (GPL-2.0+).

  make_pmf.py out.pmf [--seconds 1] [--source black|testsrc] [--no-audio] [--c-array NAME]

--c-array writes a C++ source fragment defining `static const unsigned char NAME[]` instead.

The silent ATRAC3+ frames are hand-made: one stereo channel unit with a single quantization unit,
the mute flag set and everything else zero, then the terminator id (bits 0 01 00000 1 0... 11 at bit
27: 20 80 00 18 00...). Both PPSSPP's in-tree decoder and FFmpeg's atrac3p decode it to exact zeros.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

PACK = 2048
VIDEO_RATE_NUM, VIDEO_RATE_DEN = 30000, 1001
TICKS_PER_FRAME = 3003           # 90 kHz at 29.97 fps
FIRST_PTS = 90000                # PSMF streams start at 1 s
AT3P_FRAME = 0x2E8               # payload bytes, 128 kbps stereo ("0F D0 28 5C")
AT3P_TICKS = 4180                # 2048 samples at 44.1 kHz in 90 kHz ticks (PSP rounding)


def encode_h264(seconds, source):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "v.264")
        src = {"black": "color=c=black:s=480x272:r=%d/%d" % (VIDEO_RATE_NUM, VIDEO_RATE_DEN),
               "testsrc": "testsrc=s=480x272:r=%d/%d" % (VIDEO_RATE_NUM, VIDEO_RATE_DEN)}[source]
        cmd = ["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", src, "-t", str(seconds),
               "-c:v", "libx264", "-profile:v", "main", "-level", "2.1", "-pix_fmt", "yuv420p",
               "-bf", "0", "-g", "15", "-x264-params", "aud=1:keyint=15:min-keyint=15:scenecut=0",
               "-f", "h264", out]
        subprocess.run(cmd, check=True)
        data = open(out, "rb").read()
    # Split into access units at the access unit delimiters (aud=1).
    starts = []
    i = 0
    while True:
        j = data.find(b"\x00\x00\x01\x09", i)
        if j < 0:
            break
        s = j - 1 if j > 0 and data[j - 1] == 0 else j
        starts.append(s)
        i = j + 4
    starts.append(len(data))
    aus = [data[starts[k]:starts[k + 1]] for k in range(len(starts) - 1)]
    keys = []
    for au in aus:
        # IDR if it has a NAL of type 5.
        k = 0
        idr = False
        while True:
            k = au.find(b"\x00\x00\x01", k)
            if k < 0 or k + 3 >= len(au):
                break
            if au[k + 3] & 0x1F == 5:
                idr = True
            k += 3
        keys.append(idr)
    return aus, keys


def pts_bytes(marker, ts):
    return bytes([(marker << 4) | (((ts >> 30) & 7) << 1) | 1,
                  (ts >> 22) & 0xFF, (((ts >> 15) & 0x7F) << 1) | 1,
                  (ts >> 7) & 0xFF, ((ts & 0x7F) << 1) | 1])


def pack_header(scr):
    # MPEG-2 pack header: SCR (base only), mux rate 25200 * 50 bytes/s, no stuffing.
    b = bytearray(b"\x00\x00\x01\xBA")
    b.append(0x44 | (((scr >> 30) & 7) << 3) | ((scr >> 28) & 3))
    b.append((scr >> 20) & 0xFF)
    b.append((((scr >> 15) & 0x1F) << 3) | 0x04 | ((scr >> 13) & 3))
    b.append((scr >> 5) & 0xFF)
    b.append(((scr & 0x1F) << 3) | 0x04)
    b.append(0x01)  # SCR extension 0 + marker
    mux = 25200
    b += bytes([(mux >> 14) & 0xFF, (mux >> 6) & 0xFF, ((mux & 0x3F) << 2) | 3])
    b.append(0xF8)  # reserved + 0 stuffing
    return bytes(b)


def system_header():
    body = bytearray()
    rate = 25200
    body += bytes([0x80 | ((rate >> 15) & 0x7F), (rate >> 7) & 0xFF, ((rate & 0x7F) << 1) | 1])
    body += bytes([0x04, 0xE1, 0xFF])  # 1 audio bound, fixed=0, CSPS=0, locks, 1 video bound
    body += bytes([0xE0, 0xE0 | 0x20, 0xE8])  # video stream: buffer 232*1024
    body += bytes([0xBD, 0xC0 | 0x20, 0x20])  # private stream 1: 32*128... (informational)
    return b"\x00\x00\x01\xBB" + struct.pack(">H", len(body)) + bytes(body)


class Muxer:
    def __init__(self):
        self.packs = []
        self.cur = None

    def _new_pack(self, scr, first=False):
        self._flush()
        self.cur = bytearray(pack_header(scr))
        if first:
            self.cur += system_header()

    def _flush(self):
        if self.cur is None:
            return
        room = PACK - len(self.cur)
        if room > 0:
            if room < 6:
                self.cur += b"\xFF" * room  # can't happen with the sizes below, but keep packs aligned
            else:
                self.cur += b"\x00\x00\x01\xBE" + struct.pack(">H", room - 6) + b"\xFF" * (room - 6)
        assert len(self.cur) == PACK
        self.packs.append(bytes(self.cur))
        self.cur = None

    def add(self, stream_id, payload, pts, dts, scr, prefix=b""):
        """Adds one access unit, split over as many PES packets/packs as needed."""
        first = True
        pos = 0
        while pos < len(payload) or first:
            if self.cur is None or PACK - len(self.cur) < 64:
                self._new_pack(scr, first=not self.packs and self.cur is None)
            room = PACK - len(self.cur)
            hdr = bytearray()
            if first:
                if dts is not None and dts != pts:
                    hdr += bytes([0x81, 0xC0, 10]) + pts_bytes(3, pts) + pts_bytes(1, dts)
                else:
                    hdr += bytes([0x81, 0x80, 5]) + pts_bytes(2, pts)
            else:
                hdr += bytes([0x81, 0x00, 0])
            pre = prefix if stream_id == 0xBD else b""
            maxdata = room - 6 - len(hdr) - len(pre)
            chunk = payload[pos:pos + maxdata]
            body = bytes(hdr) + pre + chunk
            self.cur += b"\x00\x00\x01" + bytes([stream_id]) + struct.pack(">H", len(body)) + body
            pos += len(chunk)
            first = False
            if PACK - len(self.cur) < 64:
                self._flush()

    def finish(self):
        self._flush()
        return b"".join(self.packs)


def silent_at3p_frame():
    payload = bytearray(AT3P_FRAME)
    payload[0:4] = bytes([0x20, 0x80, 0x00, 0x18])  # muted stereo unit + terminator (see above)
    return bytes([0x0F, 0xD0, 0x28, 0x5C, 0, 0, 0, 0]) + bytes(payload)


def build(seconds, source, audio):
    aus, keys = encode_h264(seconds, source)
    events = []
    for i, au in enumerate(aus):
        events.append((FIRST_PTS + i * TICKS_PER_FRAME, 0, i, au))
    n_audio = 0
    if audio:
        total = len(aus) * TICKS_PER_FRAME
        n_audio = (total + AT3P_TICKS - 1) // AT3P_TICKS + 1
        frame = silent_at3p_frame()
        for k in range(n_audio):
            events.append((FIRST_PTS + k * AT3P_TICKS, 1, k, frame))
    events.sort(key=lambda e: (e[0], e[1]))

    mux = Muxer()
    ep = []
    for ts, kind, idx, data in events:
        scr = max(0, ts - 9000)
        if kind == 0:
            if keys[idx]:
                # Each IDR starts a new pack, so the EP map can point at it.
                mux._flush()
                ep.append((ts, len(mux.packs)))
            mux.add(0xE0, data, ts, ts, scr)
        else:
            # PSP audio PES: channel byte 0, then 3 more bytes PPSSPP/the PSP skip.
            mux.add(0xBD, data, ts, None, scr, prefix=b"\x00\xFF\xFF\x80")
    stream = mux.finish()
    last_pts = FIRST_PTS + len(aus) * TICKS_PER_FRAME

    hdr = bytearray(PACK)
    hdr[0:4] = b"PSMF"
    hdr[4:8] = b"0015"
    struct.pack_into(">I", hdr, 0x08, PACK)
    struct.pack_into(">I", hdr, 0x0C, len(stream))
    nstreams = 2 if audio else 1
    ep_off = 0x82 + nstreams * 16
    struct.pack_into(">I", hdr, 0x50, 0x4E)
    hdr[0x54:0x5A] = b"\x00\x00" + struct.pack(">I", FIRST_PTS)
    hdr[0x5A:0x60] = b"\x00\x00" + struct.pack(">I", last_pts)
    struct.pack_into(">I", hdr, 0x60, 25200)                # mux rate
    struct.pack_into(">I", hdr, 0x6A, 0x22 + nstreams * 16)  # following block size
    hdr[0x6E:0x74] = hdr[0x54:0x5A]
    hdr[0x74:0x7A] = hdr[0x5A:0x60]
    struct.pack_into(">I", hdr, 0x7C, 2 + nstreams * 16)
    struct.pack_into(">H", hdr, 0x80, nstreams)
    v = 0x82
    hdr[v:v + 4] = bytes([0xE0, 0x00, 0x20, 0xFB])
    struct.pack_into(">I", hdr, v + 4, ep_off)
    struct.pack_into(">I", hdr, v + 8, len(ep))
    hdr[v + 12] = 480 // 16
    hdr[v + 13] = 272 // 16
    if audio:
        a = v + 16
        hdr[a:a + 4] = bytes([0xBD, 0x00, 0x20, 0x04])
        hdr[a + 14] = 2  # channels
        hdr[a + 15] = 2  # "frequency" (44.1 kHz)
    for k, (ts, packno) in enumerate(ep):
        o = ep_off + k * 10
        hdr[o] = k & 0xFF
        hdr[o + 1] = 0
        struct.pack_into(">I", hdr, o + 2, ts)
        struct.pack_into(">I", hdr, o + 6, packno)
    return bytes(hdr) + stream, len(aus), n_audio


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--seconds", type=float, default=1.0)
    ap.add_argument("--source", default="black", choices=["black", "testsrc"])
    ap.add_argument("--no-audio", action="store_true")
    ap.add_argument("--c-array")
    a = ap.parse_args()
    data, nv, na = build(a.seconds, a.source, not a.no_audio)
    if a.c_array:
        with open(a.out, "w") as f:
            f.write("// Generated by runtime/mpeg/tools/make_pmf.py --seconds %g --source %s%s: %d video frames, "
                    "%d silent ATRAC3+ frames, %d bytes.\n" % (a.seconds, a.source, " --no-audio" if a.no_audio else "",
                                                               nv, na, len(data)))
            f.write("static const unsigned char %s[%d] = {\n" % (a.c_array, len(data)))
            for i in range(0, len(data), 24):
                f.write("\t" + ",".join("0x%02x" % b for b in data[i:i + 24]) + ",\n")
            f.write("};\n")
    else:
        with open(a.out, "wb") as f:
            f.write(data)
    print("%s: %d bytes, %d video frames, %d audio frames" % (a.out, len(data), nv, na), file=sys.stderr)


if __name__ == "__main__":
    main()
