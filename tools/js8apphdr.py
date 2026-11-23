#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
"""Read the header of an ICAP *.APP file and check it against the IC-705 limits.

  js8apphdr.py JS8.APP [--su DIR] [--md OUT.md]

Prints load_size, bss, their sum against the 80 KiB app window, the stack request, the registered
modes and the flags.  With --su DIR (gcc -fstack-usage files of the build) it lists the largest
stack frames.  Exit 1 if image + bss do not fit the window.
"""
import glob
import struct
import sys

WINDOW = 80 * 1024


def main():
    a = sys.argv[1:]
    if not a:
        sys.stderr.write(__doc__)
        return 2
    su = a[a.index('--su') + 1] if '--su' in a else None
    out = a[a.index('--md') + 1] if '--md' in a else None
    d = open(a[0], 'rb').read()
    magic, abi, hdr, models, fw, load, bss, entry, nrel, stack, crc = struct.unpack('<IHHIIIIIIII', d[:40])
    name = d[40:56].split(b'\0')[0].decode()
    flags, seg2 = struct.unpack('<II', d[56:64])
    modes = []
    if hdr >= 96:
        for i in range(4):
            m = d[64 + 8 * i:72 + 8 * i]
            nm = m[:6].split(b'\0')[0].decode()
            if nm:
                modes.append('%s (radio mode %s)' % (nm, 'USB-D + DECODE' if m[6] == 1 else 'keep'))
    L = []
    L.append('file %s: %d bytes, magic %s, ABI %d, header %d B, name "%s"' % (a[0], len(d), 'ICAP' if magic == 0x50414349 else hex(magic), abi, hdr, name))
    L.append('models 0x%x (%s), fw_build %d, flags 0x%x (%s), seg2 %d, relocations %d' % (
        models, 'IC-705 only' if models == 1 else 'see icap.h', fw, flags,
        '+'.join(n for b, n in ((1, 'SPLIT'), (2, 'AUTOSTART'), (4, 'BACKGROUND')) if flags & b) or 'none', seg2, nrel))
    L.append('registered modes: %s' % (', '.join(modes) or 'none'))
    L.append('load_size %d B + bss %d B = %d B of the %d B app window (%.1f %%), %d B free' % (
        load, bss, load + bss, WINDOW, 100.0 * (load + bss) / WINDOW, WINDOW - load - bss))
    L.append('stack requested %d B' % stack)
    if su:
        fr = []
        for f in glob.glob(su + '/*.su'):
            for l in open(f):
                p = l.rstrip('\n').split('\t')
                if len(p) >= 2:
                    fr.append((int(p[1]), p[0]))
        fr.sort(reverse=True)
        L.append('largest stack frames (gcc -fstack-usage): ' + ', '.join('%s %d' % (n.split(':')[-1], b) for b, n in fr[:8]))
    txt = '\n'.join(L)
    print(txt)
    if out:
        open(out, 'w').write(txt + '\n')
    return 0 if load + bss <= WINDOW and not (flags & 1) else 1


if __name__ == '__main__':
    sys.exit(main())
