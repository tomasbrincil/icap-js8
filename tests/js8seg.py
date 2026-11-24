# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
"""Reader / checker of the JS8 radio app's segment files (shell/js8app_store.h), independent of the C code.

A log stream is a row of files <PFX>nnnn.<EXT> (JS8L / JS8M: .TXT, JS8A: .ADI), each written once:
    H <PFX> <8-digit sequence number> <EOH>
    ...lines...
    E <4-digit line count> <4 hex digits CRC-16/CCITT-FALSE of every byte before this line>
A file without a valid E line was cut (power failure, full card): its complete lines still count.  A file that was
cut before "H <PFX> <sequence>" was complete is not a segment for the app either (it uses the number again).
Used by tests/e2e.py, tests/tx_e2e.py and tests/feat_e2e.py.
"""
import os
import re
import struct

EXT = {'JS8L': 'TXT', 'JS8M': 'TXT', 'JS8A': 'ADI'}


def crc16(b, crc=0xFFFF):
    for c in b:
        crc ^= c << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1) & 0xFFFF
    return crc


class Seg:
    """one segment file, parsed: .seq (None = no header), .lines (complete lines without header / trailer),
    .ok (header + trailer with the right count and CRC), .why (what is wrong)"""

    def __init__(self, path, pfx):
        self.path, self.name = path, os.path.basename(path)
        self.raw = open(path, 'rb').read()
        self.seq, self.lines, self.ok, self.why = None, [], False, ''
        d = self.raw
        m = re.match(rb'^H ' + pfx.encode() + rb' (\d{8})', d)
        if not m:
            self.why = 'no header'
            return
        self.seq = int(m.group(1))                   # the app takes the file as a segment from here on
        if not d.startswith(m.group(0) + b' <EOH>\r\n'):
            self.why = 'no trailer (cut inside the header line)'
            return
        parts = d.split(b'\r\n')
        body, tail = parts[1:-1], parts[-1]
        t = re.match(rb'^E (\d{4}) ([0-9a-f]{4})$', body[-1]) if body else None
        if t and tail == b'':
            self.lines = [x.decode('latin1') for x in body[:-1]]
            pos = len(d) - len(body[-1]) - 2
            if int(t.group(1)) != len(self.lines):
                self.why = 'line count %s, has %d' % (t.group(1).decode(), len(self.lines))
            elif int(t.group(2), 16) != crc16(d[:pos]):
                self.why = 'CRC'
            else:
                self.ok = True
        else:
            self.lines = [x.decode('latin1') for x in body]      # a cut segment: its complete lines
            self.why = 'no trailer (cut)'


def stream(d, pfx):
    """all segments of one stream in the folder, in sequence order (files without a header last)"""
    ext = EXT[pfx]
    segs = [Seg(os.path.join(d, f), pfx) for f in sorted(os.listdir(d)) if re.match('^' + pfx + r'\d{4}\.' + ext + '$', f)]
    return sorted(segs, key=lambda s: (s.seq is None, s.seq or 0, s.name))


def lines(d, pfx):
    out = []
    for s in stream(d, pfx):
        out += s.lines
    return out


def problems(d, cut_ok=False):
    """what is wrong with the segment files of a folder: list of strings (empty = all good).
    Checks every stream: header, trailer, CRC, printable ASCII, strictly increasing sequence numbers without a
    repeat; cut_ok: segments without a trailer are tolerated (after a power cut / a full card)."""
    bad = []
    for pfx in EXT:
        seen = {}
        for s in stream(d, pfx):
            if s.seq is None:
                bad.append('%s: %s' % (s.name, s.why))
                continue
            if s.seq in seen:
                bad.append('%s and %s both have sequence number %d' % (seen[s.seq], s.name, s.seq))
            seen[s.seq] = s.name
            if not s.ok and not (cut_ok and s.why.startswith('no trailer')):
                bad.append('%s: %s' % (s.name, s.why))
            if any(c < 32 and c not in (13, 10) or c > 126 for c in s.raw):
                bad.append('%s: not printable ASCII' % s.name)
            if re.search(rb'\r(?!\n)|(?<!\r)\n', s.raw):
                bad.append('%s: bare CR / LF' % s.name)
    return bad


def state(d):
    """the state file pair: list of dict(name, ok, gen, rings=[(seq, next, full) x 3], size) for A and B"""
    out = []
    for nm in ('JS8STA.BIN', 'JS8STB.BIN'):
        p = os.path.join(d, nm)
        if not os.path.exists(p):
            out.append({'name': nm, 'ok': False, 'why': 'missing'})
            continue
        b = open(p, 'rb').read()
        e = {'name': nm, 'ok': False, 'size': len(b)}
        if len(b) > 36 and b[:4] == b'J8S2':
            gen, size, crc = struct.unpack('<IHH', b[4:12])
            z = b[:10] + b'\0\0' + b[12:]
            e['gen'] = gen
            e['rings'] = [struct.unpack('<IHBB', b[12 + 8 * i:20 + 8 * i])[:3] for i in range(3)]
            e['ok'] = len(b) == 36 + size and crc16(z) == crc
            if not e['ok']:
                e['why'] = 'CRC / size'
        else:
            e['why'] = 'not a state file'
        out.append(e)
    return out
