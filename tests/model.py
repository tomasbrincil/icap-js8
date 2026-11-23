#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
"""Independent Python model of the JS8 encoder, used to cross-check the C code (T0/T1).

It deliberately does NOT use the generated C header: it reads the original JS8Call source
files as DATA (js8call/js8call@a7ff1be, fetched by tools/fetch_refs.sh) and follows the Fortran
logic literally:
  genjs8.f90     6-bit words, i3bit, CRC, tone mapping, Costas placement
  crc12.cpp      boost::augmented_crc<12,0xc06> over the 11 bytes, then xor 42
  encode174.f90  generator g() in FILE order + the colorder column permutation
The C code instead uses the pre-permuted generator and a plain 76-bit CRC, so agreement
between the two is a real check of the bit placement.

  model.py gen N SEED            -> N random vectors "text i3 crc hex22 tones79" on stdout
                                    (after the three fixed design vectors)
  model.py tables HEADER         -> compare the C header with second copies parsed from
                                    bpdecode174.f90, ldpc_174_87_params.f90 and JS8.cpp
Option --src DIR: directory with js8call/ and js8call-improved/ (default $JS8_REF, else third_party/).
"""
import os
import random
import re
import sys

SRC = (os.environ.get('JS8_REF') or os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + '/third_party')
ALPHABET = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+'
COSTAS = [4, 2, 5, 6, 1, 3, 0]


def rd(p):
    return open(p, encoding='utf-8', errors='replace').read()


def fortran_params(src):
    par = rd(src + '/js8call/lib/ft8/ldpc_174_87_params.f90')
    g = re.findall(r'"([0-9a-f]{22})"', par)
    assert len(g) == 87
    co = [int(x) for x in re.findall(r'\d+', par.split('data colorder/')[1])]
    assert len(co) == 174
    return g, co


def fortran_bp(src):
    bp = rd(src + '/js8call/lib/ft8/bpdecode174.f90')

    def block(name):
        s = bp.split('data ' + name + '/')[1].split('/')[0]
        return [int(x) for x in re.findall(r'\d+', s)]
    mn, nm, nrw = block('Mn'), block('Nm'), block('nrw')
    assert len(mn) == 522 and len(nm) == 609 and len(nrw) == 87
    return ([mn[i * 3:i * 3 + 3] for i in range(174)],
            [nm[i * 7:i * 7 + 7] for i in range(87)], nrw)


def augmented_crc12(data):
    """boost::augmented_crc<12, 0xc06>: shift every message bit through the register."""
    rem = 0
    for byte in data:
        for k in range(7, -1, -1):
            rem = (rem << 1) | ((byte >> k) & 1)
            if rem & 0x1000:
                rem ^= 0x1C06
    return rem


class Model:
    def __init__(self, src=SRC):
        g, self.colorder = fortran_params(src)
        # gen(i,icol): bit icol (1..87) of row i, MSB first (encode174.f90:24-36)
        self.gen = [[(int(row, 16) >> (87 - j)) & 1 for j in range(87)] for row in g]

    def frame(self, text, i3):
        words = [ALPHABET.index(c) for c in text]
        assert len(words) == 12
        bits = []
        for w in words:
            bits += [(w >> k) & 1 for k in range(5, -1, -1)]
        bits += [(i3 >> k) & 1 for k in (2, 1, 0)]                 # 75 bits
        b88 = bits + [0] * 13
        by = [int(''.join(map(str, b88[i:i + 8])), 2) for i in range(0, 88, 8)]
        crc = augmented_crc12(by) ^ 42
        msgbits = bits + [(crc >> k) & 1 for k in range(11, -1, -1)]  # 87 bits
        return msgbits, crc

    def encode174(self, msgbits):
        pchecks = [sum(a & b for a, b in zip(self.gen[i], msgbits)) & 1 for i in range(87)]
        itmp = pchecks + msgbits
        cw = [0] * 174
        for i in range(174):
            cw[self.colorder[i]] = itmp[i]       # codeword(colorder+1)=itmp(1:N)
        return cw

    def tones(self, msgbits):
        cw = self.encode174(msgbits)
        it = [0] * 79
        it[0:7] = COSTAS
        it[36:43] = COSTAS
        it[72:79] = COSTAS
        k = 6
        for j in range(58):
            k += 1
            if j == 29:
                k += 7
            it[k] = cw[3 * j] * 4 + cw[3 * j + 1] * 2 + cw[3 * j + 2]
        return it

    def vector(self, text, i3):
        msgbits, crc = self.frame(text, i3)
        b88 = msgbits + [0]
        hx = ''.join('%02x' % int(''.join(map(str, b88[i:i + 8])), 2) for i in range(0, 88, 8))
        return '%s %d %03x %s %s' % (text, i3, crc, hx, ''.join(map(str, self.tones(msgbits))))


def cmd_gen(n, seed, src):
    m = Model(src)
    for text, i3 in (('000000000000', 0), ('0123456789AB', 0), ('HELLOWORLD++', 5)):
        print(m.vector(text, i3))
    rng = random.Random(seed)
    for _ in range(n):
        text = ''.join(rng.choice(ALPHABET) for _ in range(12))
        print(m.vector(text, rng.randrange(8)))


def parse_header(path):
    t = rd(path)

    def arr(name):
        s = t.split(name + '[')[1].split('= {', 1)[1].split('};')[0]
        return [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', s)]
    return arr('kJS8_LDPC_generator'), arr('kJS8_LDPC_Nm'), arr('kJS8_LDPC_Mn'), arr('kJS8_LDPC_Num_rows')


def parse_cpp(path):
    t = rd(path)
    mn = [int(x) for x in re.findall(r'-?\d+', t.split('> Mn =')[1].split('}};')[0])]
    rows = re.findall(r'\{\s*(\d+)\s*,\s*\{([^}]*)\}\}', t.split('> Nm =')[1].split('}};')[0])
    nm = [(int(a), [int(x) for x in b.split(',')]) for a, b in rows]
    d = re.findall(r'"([0-9a-fA-F]{22})"', t.split('Rows> Data')[1].split('};')[0])
    return mn, nm, d


def cmd_tables(header, src):
    fails = 0

    def check(name, ok):
        nonlocal fails
        print('%-66s %s' % (name, 'PASS' if ok else 'FAIL'))
        fails += 0 if ok else 1

    hg, hnm, hmn, hnr = parse_header(header)
    check('header sizes (gen 957, Nm 609, Mn 522, Num_rows 87)',
          (len(hg), len(hnm), len(hmn), len(hnr)) == (957, 609, 522, 87))
    g, co = fortran_params(src)
    mn, nm, nrw = fortran_bp(src)
    check('header Mn == bpdecode174.f90 Mn', hmn == [x for r in mn for x in r])
    check('header Nm == bpdecode174.f90 Nm', hnm == [x for r in nm for x in r])
    check('header Num_rows == bpdecode174.f90 nrw', hnr == nrw)
    inv = [0] * 174
    for i, c in enumerate(co):
        inv[c] = i
    perm = [g[inv[p]] for p in range(87)]          # row p = parity bit at on-air position p
    hrows = [''.join('%02x' % b for b in hg[i * 11:i * 11 + 11]) for i in range(87)]
    check('header generator == params.f90 g() permuted by colorder', hrows == perm)
    check('colorder: permutation of 0..86, identity on 87..173',
          sorted(co[:87]) == list(range(87)) and co[87:] == list(range(87, 174)))
    for name, rel in (('orig', 'js8call/JS8.cpp'), ('impr', 'js8call-improved/JS8_Mode/JS8.cpp')):
        try:
            cmn, cnm, cd = parse_cpp('%s/%s' % (src, rel))
        except (OSError, IndexError) as e:
            check('%s JS8.cpp readable (%s)' % (name, e), False)
            continue
        check('header Mn == %s JS8.cpp Mn (+1)' % name, hmn == [x + 1 for x in cmn])
        ok = len(cnm) == 87 and all(
            cnm[i][0] == hnr[i]
            and [x + 1 for x in cnm[i][1][:hnr[i]]] == hnm[i * 7:i * 7 + hnr[i]]
            and all(v == 0 for v in hnm[i * 7 + hnr[i]:i * 7 + 7]) for i in range(87))
        check('header Nm == %s JS8.cpp Nm (+1, valid neighbours)' % name, ok)
        check('header generator == %s JS8.cpp Data' % name, hrows == [x.lower() for x in cd])
    print('model.py tables: %s' % ('ALL PASS' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


def main(argv):
    src = SRC
    if '--src' in argv:
        i = argv.index('--src')
        src = argv[i + 1]
        del argv[i:i + 2]
    if len(argv) == 4 and argv[1] == 'gen':
        cmd_gen(int(argv[2]), int(argv[3]), src)
        return 0
    if len(argv) == 3 and argv[1] == 'tables':
        return cmd_tables(argv[2], src)
    sys.stderr.write(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
