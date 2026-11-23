#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
#
# mkjsc.py - build the JSC dictionary files for the radio's SD card from the
# JS8Call source file jsc_list.cpp (read as TEXT, never compiled).
#
# The dictionary data itself (word list, order, indices) is (C) 2018 Jordan
# Sherer KN4CRD, JS8Call, GPL-3.0-or-later; the generated files are derived data.
#
#   python3 -I tools/mkjsc.py [--list jsc_list.cpp] [--map jsc_map.cpp]
#                             [--out DIR] [--per-file 1024] [--enc-files 256]
#                             [--head 5740] [--stats]
#
# The ICAP file API has only open/read/write/close (no seek), so the
# dictionary is split into many small files that are read sequentially:
#
#   JSCHEAD.BIN   16-byte header + the first --head entries (frequency order)
#                 "JSC1", u32 total, u16 per_file, u16 enc_files, u16 head_n,
#                 u8 maxlen, u8 0; then head_n x [len u8][chars]
#   JSCDxxx.BIN   decode buckets, xxx = hex(index / per_file):
#                 "JD", u16 fileno, u16 count, u16 payload; count x [len][chars]
#   JSCExxx.BIN   encode buckets (text -> index; used by the host test tools only), xxx = hex(hash(first 3 chars) % enc_files),
#                 words of 3+ chars in JSC::list order (longest prefix first):
#                 "JE", u16 fileno, u32 payload; n x [len][chars][u24 v]
#                 v = index | (len - advance) << 18
#   JSCES.BIN     same entry format, all candidate words of 1..2 chars
#
# Decoding honours the `size` field of JSC::map like JS8Call-improved does
# (index 81 -> "@ALLCAL", index 262143 -> "R").  All integers little endian.
import argparse
import collections
import os
import re
import struct
import subprocess
import sys

URL = "https://raw.githubusercontent.com/js8call/js8call/a7ff1be0b389d287fdc56e2ea0d06962aa68127d/jsc_list.cpp"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEF_SRC = os.path.join(os.environ.get("JS8_REF") or ROOT + "/third_party", "js8call")
ENT = re.compile(rb'\{\s*"((?:[^"\\]|\\.)*)"\s*(?:/\*.*?\*/)?\s*,\s*(\d+)\s*,\s*(\d+)\s*\}')
HEX = b"0123456789abcdefABCDEF"


def unesc(b):
    out = bytearray()
    i = 0
    while i < len(b):
        c = b[i]
        if c != 0x5C:
            out.append(c)
            i += 1
            continue
        n = b[i + 1:i + 2]
        if n == b"x":
            j = i + 2
            while j < len(b) and b[j] in HEX:
                j += 1
            out.append(int(b[i + 2:j], 16) & 255)
            i = j
        elif n == b"n":
            out.append(10)
            i += 2
        else:
            out.append(b[i + 1])
            i += 2
    return bytes(out)


def parse(path):
    d = open(path, "rb").read()
    secs = {}
    for m in re.finditer(rb"JSC::(\w+)\[(\d+)\]\s*=\s*\{", d):
        e = d.index(b"};", m.end())
        rows = [(unesc(a), int(b), int(c)) for a, b, c in ENT.findall(d[m.end():e])]
        if len(rows) != int(m.group(2)):
            sys.exit("%s: JSC::%s declares %s entries, parsed %d"
                     % (path, m.group(1).decode(), m.group(2).decode(), len(rows)))
        secs[m.group(1).decode()] = rows
    return secs


def jsc_hash(s, n):
    return ((s[0] * 31 + s[1]) * 31 + s[2]) % n


def codelen(i):
    return 5 if i < 7 else 9 if i < 70 else 13 if i < 637 else 17 if i < 5740 else 21 if i < 51667 else 25


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", default=os.path.join(DEF_SRC, "jsc_list.cpp"))
    ap.add_argument("--map", default=os.path.join(DEF_SRC, "jsc_map.cpp"),
                    help="optional, only used to cross-check the list")
    ap.add_argument("--out", default=(os.environ.get("BUILD") or ROOT + "/build") + "/sd")
    ap.add_argument("--per-file", type=int, default=1024)
    ap.add_argument("--enc-files", type=int, default=256)
    ap.add_argument("--head", type=int, default=5740)
    ap.add_argument("--stats", action="store_true", help="print the bucket size comparison")
    a = ap.parse_args()

    if not os.path.exists(a.list):
        os.makedirs(os.path.dirname(a.list), exist_ok=True)
        print("fetching", URL)
        subprocess.check_call(["curl", "-fsSL", "-o", a.list, URL])
    L = parse(a.list)
    lst, pre = L["list"], L["prefix"]
    total = len(lst)

    # index -> word; the decoder emits the first `size` characters (improved tree)
    words = [None] * total
    for s, n, i in lst:
        if words[i] is not None:
            sys.exit("duplicate index %d" % i)
        words[i] = s
    adv = [len(w) for w in words]          # characters the compressor consumes
    known = {81: 7, 262143: 1}             # JSC::map size anomalies (both trees)
    for i, n in known.items():
        adv[i] = n
    if os.path.exists(a.map):
        mp = parse(a.map)["map"]
        ok = len(mp) == total and all(e[2] == k and e[0] == words[k] for k, e in enumerate(mp))
        diff = {k: e[1] for k, e in enumerate(mp) if e[1] != len(e[0])}
        print("cross-check with jsc_map.cpp: strings %s, size anomalies %s"
              % ("identical" if ok else "DIFFER", diff))
        if not ok or diff != known:
            sys.exit("jsc_map.cpp does not match the list / known anomalies")
    dec = [words[i][:adv[i]] for i in range(total)]
    maxlen = max(len(w) for w in dec)

    # encode candidates = exactly what JSC::lookup can return
    cand, seen = [], set()
    for s, n, i in pre:
        if s in seen:
            continue
        seen.add(s)
        if n == 1:
            cand.append(lst[i])
        else:
            cand += [e for e in lst[i:i + n] if e[0][:1] == s]
    grp = collections.defaultdict(list)
    for e in cand:
        grp[e[0][:1]].append(e[0][:e[1]])
    viol = sum(1 for v in grp.values() for x, y in zip(v, v[1:]) if not x > y)
    print("entries %d, characters %d, longest %d, encode candidates %d, order violations %d"
          % (total, sum(map(len, dec)), maxlen, len(cand), viol))
    if viol:
        sys.exit("list is not strictly descending inside a prefix group: "
                 "first match would not be the longest prefix")

    if a.stats:
        S = [len(w) + 1 for w in dec]
        print("decode bucket comparison (bytes per lookup; dir = FAT directory entries, 32 B each,")
        print("searched linearly on open, an ASSUMPTION about the radio's file system):")
        print("  per_file files  max_file avg_file | stream avg / worst | with dir scan avg / worst")
        for B in (128, 256, 512, 1024, 2048, 4096):
            sz = [8 + sum(S[x:x + B]) for x in range(0, total, B)]
            n = len(sz)
            avg = sum(sz) // n
            print("  %8d %5d %9d %8d | %6d / %6d    | %6d / %6d"
                  % (B, n, max(sz), avg, avg // 2, max(sz), 16 * n + avg // 2, 32 * n + max(sz)))
        best = {}
        for i, w in enumerate(dec):
            c = codelen(i)
            best[c] = max(best.get(c, 0), len(w) + 1)
        for B in (69, 71):
            dp = [0] * (B + 1)
            for b in range(1, B + 1):
                dp[b] = dp[b - 1]
                for c, v in best.items():
                    if c <= b:
                        dp[b] = max(dp[b], dp[b - c] + v)
            print("  longest possible text of one data frame with %d code bits: %d characters" % (B, dp[B]))

    os.makedirs(a.out, exist_ok=True)
    for f in os.listdir(a.out):
        if re.fullmatch(r"JSC(HEAD|ES|[DE][0-9A-F]{3})\.BIN", f):
            os.remove(os.path.join(a.out, f))

    def ent(w):
        return bytes([len(w)]) + w

    sizes = collections.defaultdict(list)
    head_n = min(a.head, total)
    body = b"".join(ent(w) for w in dec[:head_n])
    hdr = b"JSC1" + struct.pack("<IHHHBB", total, a.per_file, a.enc_files, head_n, maxlen, 0)
    open(os.path.join(a.out, "JSCHEAD.BIN"), "wb").write(hdr + body)
    sizes["HEAD"].append(len(hdr) + len(body))

    nd = (total + a.per_file - 1) // a.per_file
    if nd > 4096 or a.enc_files > 4096:
        sys.exit("too many files for a 3-digit hex name")
    for k in range(nd):
        part = dec[k * a.per_file:(k + 1) * a.per_file]
        body = b"".join(ent(w) for w in part)
        if len(body) > 65535:
            sys.exit("decode bucket too large")
        d = b"JD" + struct.pack("<HHH", k, len(part), len(body)) + body
        open(os.path.join(a.out, "JSCD%03X.BIN" % k), "wb").write(d)
        sizes["D"].append(len(d))

    def eent(e):
        s, n, i = e
        w = s[:n]
        d = len(w) - adv[i]
        if not 0 <= d < 32:
            sys.exit("advance out of range for %r" % (e,))
        return ent(w) + struct.pack("<I", i | d << 18)[:3]

    buckets = [[] for _ in range(a.enc_files)]
    short = []
    for e in cand:
        if e[1] >= 3:
            buckets[jsc_hash(e[0], a.enc_files)].append(eent(e))
        else:
            short.append(eent(e))
    for k, b in enumerate(buckets):
        body = b"".join(b)
        d = b"JE" + struct.pack("<HI", k, len(body)) + body
        open(os.path.join(a.out, "JSCE%03X.BIN" % k), "wb").write(d)
        sizes["E"].append(len(d))
    body = b"".join(short)
    d = b"JE" + struct.pack("<HI", 0xFFFF, len(body)) + body
    open(os.path.join(a.out, "JSCES.BIN"), "wb").write(d)
    sizes["ES"].append(len(d))

    print("written to", a.out)
    tot = 0
    for k, name in (("HEAD", "JSCHEAD.BIN"), ("D", "JSCDxxx.BIN"), ("E", "JSCExxx.BIN"), ("ES", "JSCES.BIN")):
        v = sizes[k]
        tot += sum(v)
        print("  %-12s files %4d  total %8d B  max %6d  avg %6d  min %6d"
              % (name, len(v), sum(v), max(v), sum(v) // len(v), min(v)))
    print("  all files: %d, %d bytes; head %d entries (%d B of RAM if fully loaded)"
          % (sum(len(v) for v in sizes.values()), tot, head_n, sizes["HEAD"][0] - 16))


if __name__ == "__main__":
    main()
