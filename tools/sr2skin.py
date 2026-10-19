#!/usr/bin/env python3
"""Make and edit character skins (extra slots on the character select screen; see README "Character skins").

  sr2skin.py new <game folder> <base> "<Name>" <skin folder>
      Start a skin: character.ini plus copies of the base character's racer files (RACER_<BASE>_0/1.STR),
      which carry its model, textures and voice.
  sr2skin.py sounds <skin folder>
      Write the voice clips and sound effects in the skin's racer files to <skin folder>/sounds/*.wav.
  sr2skin.py voice <skin folder>
      Put the edited WAVs from <skin folder>/sounds/ back into both racer files. Any WAV works (any rate,
      mono or stereo, 16-bit); it is converted to the clip's own rate, and a clip keeps its original length:
      longer audio is cut, shorter audio ends in silence. Unchanged clips are left alone.

Only the Python standard library is needed.
"""
import os
import re
import shutil
import struct
import sys
import wave

BASES = {'sonic': 'SONIC', 'shadow': 'SHADOW', 'knuckles': 'KNUCKLES', 'silver': 'SILVER', 'metalsonic': 'METALSONIC',
         'metal': 'METALSONIC', 'tails': 'TAILS', 'espio': 'ESPIO', 'rouge': 'ROUGE'}
PACK_SIZE = 0x166670
COEF = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]
END_FRAME = bytes([0, 7]) + bytes([0x77]) * 14


def die(msg):
    sys.exit('sr2skin: ' + msg)


def read_ini(skin):
    ini = {}
    try:
        for line in open(os.path.join(skin, 'character.ini')):
            if '=' in line:
                k, v = line.split('=', 1)
                ini[k.strip().lower()] = v.strip()
    except OSError:
        die('%s has no character.ini' % skin)
    base = BASES.get(re.sub(r'[\s_-]', '', ini.get('base', '').lower()))
    if not base:
        die('character.ini needs base= (sonic, shadow, knuckles, silver, metal sonic, tails, espio or rouge)')
    return ini, base


def packs(skin):
    _, base = read_ini(skin)
    p = [os.path.join(skin, 'RACER_%s_%d.STR' % (base, k)) for k in (0, 1)]
    for f in p:
        if not os.path.isfile(f):
            die('%s is missing (sr2skin.py new copies it from the game)' % f)
    return p


def clips(data):
    """(offset of the header, data size, sample rate, name) of every VAG clip in a racer file, in order"""
    out = []
    for m in re.finditer(b'VAGp', data):
        o = m.start()
        size, rate = struct.unpack_from('>II', data, o + 12)
        name = data[o + 0x20:o + 0x30].split(b'\0')[0].decode('ascii', 'replace')
        if 0 < size < 0x100000 and 4000 <= rate <= 48000:
            out.append((o, size, rate, name))
    return out


def vag_decode(adpcm):
    s1 = s2 = 0
    pcm = []
    for f in range(0, len(adpcm), 16):
        pred, shift, flags = adpcm[f] >> 4, adpcm[f] & 15, adpcm[f + 1]
        if flags == 7:
            break
        c0, c1 = COEF[pred] if pred < 5 else (0, 0)
        for i in range(28):
            n = (adpcm[f + 2 + i // 2] >> (4 * (i & 1))) & 15
            if n >= 8:
                n -= 16
            s = (n << 12) >> shift if shift <= 12 else 0
            s += (s1 * c0 + s2 * c1) >> 6
            s = max(-32768, min(32767, s))
            pcm.append(s)
            s2, s1 = s1, s
    return pcm


def vag_encode(pcm, nframes):
    """PSP ADPCM for `pcm`, exactly nframes 16-byte frames: the audio (cut or padded with silence), then the
    end marker"""
    out = bytearray()
    s1 = s2 = 0
    audio = nframes - 1
    pcm = (pcm + [0] * (audio * 28))[:audio * 28]
    for f in range(audio):
        blk = pcm[f * 28:(f + 1) * 28]
        best = None
        for pred, (c0, c1) in enumerate(COEF):
            # the residual this predictor leaves (from the ideal history), to pick the shift
            p1, p2, peak = s1, s2, 0
            for x in blk:
                r = x - ((p1 * c0 + p2 * c1) >> 6)
                peak = max(peak, abs(r))
                p2, p1 = p1, x
            shift = 12
            while shift > 0 and (peak >> (12 - shift)) > 7:
                shift -= 1
            # quantize for real, tracking the decoder's history
            q1, q2, err, nib = s1, s2, 0, []
            for x in blk:
                p = (q1 * c0 + q2 * c1) >> 6
                n = ((x - p) * (1 << shift) + (1 << 11)) >> 12
                n = max(-8, min(7, n))
                y = max(-32768, min(32767, ((n << 12) >> shift) + p))
                err += (x - y) * (x - y)
                nib.append(n & 15)
                q2, q1 = q1, y
            if best is None or err < best[0]:
                best = (err, pred, shift, nib, q1, q2)
        _, pred, shift, nib, s1, s2 = best
        out += bytes([(pred << 4) | shift, 0]) + bytes(nib[i] | (nib[i + 1] << 4) for i in range(0, 28, 2))
    return bytes(out + END_FRAME)


def wav_name(i, name):
    return '%02d_%s.wav' % (i, re.sub(r'[^A-Za-z0-9_]', '', name))


def read_wav(path, rate):
    with wave.open(path) as w:
        if w.getsampwidth() != 2:
            die('%s: only 16-bit WAVs' % path)
        ch, r, raw = w.getnchannels(), w.getframerate(), w.readframes(w.getnframes())
    s = struct.unpack('<%dh' % (len(raw) // 2), raw)
    mono = [sum(s[i:i + ch]) // ch for i in range(0, len(s), ch)]
    if r == rate or not mono:
        return mono
    n = int(len(mono) * rate / r)
    out = []
    for i in range(n):   # linear resampling
        t = i * r / rate
        a = int(t)
        b = min(a + 1, len(mono) - 1)
        out.append(int(mono[a] + (mono[b] - mono[a]) * (t - a)))
    return out


def write_wav(path, pcm, rate):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack('<%dh' % len(pcm), *pcm))


def cmd_new(game, base, name, skin):
    b = BASES.get(re.sub(r'[\s_-]', '', base.lower()))
    if not b:
        die('unknown base character %r' % base)
    os.makedirs(skin, exist_ok=True)
    with open(os.path.join(skin, 'character.ini'), 'w') as f:
        f.write('name=%s\nbase=%s\n' % (name, base.lower()))
    for k in (0, 1):
        src = None
        for root, _, files in os.walk(game):
            for fn in files:
                if fn.upper() == 'RACER_%s_%d.STR' % (b, k):
                    src = os.path.join(root, fn)
        if not src:
            die('RACER_%s_%d.STR not found in %s' % (b, k, game))
        shutil.copyfile(src, os.path.join(skin, 'RACER_%s_%d.STR' % (b, k)))
    print('made %s; put it in a mod as <mod>/characters/%s/' % (skin, os.path.basename(os.path.normpath(skin))))


def cmd_sounds(skin):
    data = open(packs(skin)[0], 'rb').read()
    d = os.path.join(skin, 'sounds')
    os.makedirs(d, exist_ok=True)
    cl = clips(data)
    for i, (o, size, rate, name) in enumerate(cl):
        write_wav(os.path.join(d, wav_name(i, name)), vag_decode(data[o + 0x30:o + 0x30 + size]), rate)
    print('wrote %d clips to %s' % (len(cl), d))


def cmd_voice(skin):
    d = os.path.join(skin, 'sounds')
    files = packs(skin)
    datas = [bytearray(open(f, 'rb').read()) for f in files]
    lists = [clips(x) for x in datas]
    if len(lists[0]) != len(lists[1]):
        die('the two racer files have different clips')
    changed = 0
    for i, (o, size, rate, name) in enumerate(lists[0]):
        p = os.path.join(d, wav_name(i, name))
        if not os.path.isfile(p):
            continue
        pcm = read_wav(p, rate)
        old = vag_decode(bytes(datas[0][o + 0x30:o + 0x30 + size]))
        if pcm[:len(old)] == old and all(x == 0 for x in pcm[len(old):]):
            continue
        if len(pcm) > (size // 16 - 1) * 28:
            print('%s: %.2fs long, the clip holds %.2fs; the end is cut' % (os.path.basename(p), len(pcm) / rate, (size // 16 - 1) * 28 / rate))
        enc = vag_encode(pcm, size // 16)
        for x, lst in zip(datas, lists):
            xo = lst[i][0]
            x[xo + 0x30:xo + 0x30 + size] = enc
        changed += 1
    for f, x in zip(files, datas):
        assert len(x) == PACK_SIZE
        open(f, 'wb').write(x)
    print('%d clips replaced' % changed)


def main(a):
    if len(a) == 5 and a[0] == 'new':
        cmd_new(*a[1:])
    elif len(a) == 2 and a[0] == 'sounds':
        cmd_sounds(a[1])
    elif len(a) == 2 and a[0] == 'voice':
        cmd_voice(a[1])
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == '__main__':
    main(sys.argv[1:])
