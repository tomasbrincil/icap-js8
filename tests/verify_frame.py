#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
"""verify_frame.py - is a decoded JS8 frame really in the audio?  Independent of the C decoder.

The frame (22 hex digits as in JS8LOG.TXT) is re-encoded with the Python model of tests/rv_protocol.py
(written from genjs8.f90 / encode174.f90 / crc12.cpp; shares no code with src/js8enc.c or src/js8dec.c), its CRC-12 is
checked, and the energy of the 79 symbols is measured in the recording at the expected tones:
  - time and frequency are refined on the 21 Costas symbols only (t +-60 ms, f +-1.5 Hz);
  - the verdict uses the 58 DATA symbols only, so the search does not bias it:
      ratio = mean energy at the expected tone / mean energy of the other seven tones
      hits  = data symbols whose strongest of the 8 tones is the expected one
A real signal at the decoder threshold (-19 dB) gives a ratio of about 6; a codeword that BP "found" in
noise gives about 1.5 (it agrees with the noise in roughly a third of the symbols); wrong time or
frequency gives 1.0.  Verdict "ok" needs ratio >= 3.0.  A frame under a stronger co-channel signal can fall
below that although it is real - then a person has to look.

usage: verify_frame.py FILE.wav HEX22 F0_HZ T_START_S [--min-ratio 3.0]
prints one line "ratio R hits H/58 crc ok|BAD f F t T verdict ok|NOT CONFIRMED" and exits 0 / 1.
"""
import cmath
import math
import os
import struct
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rv_protocol as rv   # noqa: E402  (the model; importing it runs nothing but table parsing)

FS, SPS, DF = 8000, 1280, 6.25
A64 = rv.ALPH[:64]
SYNC = set(range(7)) | set(range(36, 43)) | set(range(72, 79))


def frame_tones(hx):
    v = int(hx, 16)
    bits72, i3, crc = v >> 16, (v >> 13) & 7, (v >> 1) & 0xFFF
    msg = ''.join(A64[(bits72 >> (66 - 6 * i)) & 63] for i in range(12))
    _, crc_model, tone = rv.genjs8(msg, i3)
    return tone, crc_model == crc


def read_wav(path):
    w = wave.open(path, 'rb')
    assert w.getframerate() == FS and w.getnchannels() == 1 and w.getsampwidth() == 2, 'need 8000 Hz mono s16'
    d = w.readframes(w.getnframes())
    return struct.unpack('<%dh' % (len(d) // 2), d)


def energy(x, start, f):
    """|sum x[n] exp(-j 2 pi f n / FS)|^2 over one symbol starting at sample `start` (zeros outside the file)"""
    a, b = max(start, 0), min(start + SPS, len(x))
    if b <= a:
        return 0.0
    w = cmath.exp(-2j * math.pi * f / FS)
    ph, acc = cmath.exp(-2j * math.pi * f * (a - start) / FS), 0j
    for n in range(a, b):
        acc += x[n] * ph
        ph *= w
    return abs(acc) ** 2


def verify(x, hx, f0, t0, min_ratio=3.0):
    tone, crc_ok = frame_tones(hx)
    best = (-1.0, f0, t0)
    for dt in (-0.06, -0.04, -0.02, 0.0, 0.02, 0.04, 0.06):
        for df in (-1.5, -1.0, -0.5, 0.0, 0.5, 1.0, 1.5):
            s0 = int(round((t0 + dt) * FS))
            e = sum(energy(x, s0 + k * SPS, f0 + df + tone[k] * DF) for k in SYNC)
            if e > best[0]:
                best = (e, f0 + df, t0 + dt)
    _, f, t = best
    s0 = int(round(t * FS))
    exp_e = oth_e = 0.0
    hits = nd = 0
    for k in range(79):
        if k in SYNC:
            continue
        if s0 + k * SPS < 0 or s0 + (k + 1) * SPS > len(x):
            continue                              # symbol not (fully) in the recording
        e = [energy(x, s0 + k * SPS, f + j * DF) for j in range(8)]
        exp_e += e[tone[k]]
        oth_e += (sum(e) - e[tone[k]]) / 7.0
        hits += e[tone[k]] == max(e)
        nd += 1
    ratio = exp_e / oth_e if oth_e > 0 else 0.0
    ok = crc_ok and nd >= 29 and ratio >= min_ratio
    return {'ratio': ratio, 'hits': hits, 'nd': nd, 'crc_ok': crc_ok, 'f': f, 't': t, 'ok': ok}


def main():
    a = sys.argv[1:]
    mr = 3.0
    if '--min-ratio' in a:
        i = a.index('--min-ratio')
        mr = float(a[i + 1])
        del a[i:i + 2]
    if len(a) != 4:
        print(__doc__)
        return 2
    r = verify(read_wav(a[0]), a[1], float(a[2]), float(a[3]), mr)
    print('ratio %.2f hits %d/%d crc %s f %.1f t %.2f verdict %s' % (r['ratio'], r['hits'], r['nd'], 'ok' if r['crc_ok'] else 'BAD', r['f'], r['t'],
                                                                   'ok' if r['ok'] else 'NOT CONFIRMED'))
    return 0 if r['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
