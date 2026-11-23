#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
"""js8agg.py - sum the "S ..." summary lines of several js8sweep runs (different seeds, run in parallel).

usage: js8agg.py [--thr] FILE...   (or stdin)
Lines with the same mode and step key (snr= / levels= / sigma=) are merged: integer fields and a/b
fractions are added, float fields are averaged weighted by n (or periods; snr_est, df, ddt by the
number of decodes), *_max fields take the maximum. With --thr the 50 % and 90 % points of the awgn decode rate are interpolated linearly
between the SNR steps. "F" (false decode) and "X" lines are passed through.
"""
import sys, re

OKW = ("snr_est", "df", "ddt")   # per-run means over decoded frames: weight by ok, not by n

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    thr = "--thr" in sys.argv
    lines = []
    for name in args or ["-"]:
        f = sys.stdin if name == "-" else open(name)
        lines += f.read().splitlines()
    acc, order = {}, []
    for ln in lines:
        if ln.startswith(("F ", "X ")):
            print(ln)
            continue
        if not ln.startswith("S "):
            continue
        p = ln.split()
        mode, kv = p[1], [x.split("=", 1) for x in p[2:]]
        keyf = [k for k, v in kv if k in ("snr", "levels", "sigma")]
        key = (mode,) + tuple(v for k, v in kv if k in keyf)
        d = dict(kv)
        w = float(d.get("n", d.get("periods", 1)))
        if key not in acc:
            acc[key] = {"_w": 0.0, "_keys": [k for k, v in kv]}
            order.append(key)
        a = acc[key]
        a["_w"] += w
        for k, v in kv:
            if k in keyf:
                a[k] = v
            elif "/" in v:
                x, y = v.split("/")
                o = a.get(k, (0, 0))
                a[k] = (o[0] + int(x), o[1] + int(y))
            elif k.endswith("_max"):
                a[k] = max(a.get(k, 0.0), float(v))
            elif re.fullmatch(r"-?\d+", v):
                a[k] = a.get(k, 0) + int(v)
            elif k in OKW:                      # means over the decoded frames only
                wo = float(d.get("ok", 0))
                a[k] = a.get(k, 0.0) + float(v) * wo
                a["_wo"] = a.get("_wo", 0.0) + (wo if k == OKW[0] else 0.0)
            else:
                a[k] = a.get(k, 0.0) + float(v) * w
    for key in order:
        a = acc[key]
        out = ["S", key[0]]
        for k in a["_keys"]:
            v = a[k]
            if isinstance(v, tuple):
                out.append("%s=%d/%d(%.1f%%)" % (k, v[0], v[1], 100.0 * v[0] / max(v[1], 1)))
            elif k in OKW:
                out.append("%s=%.2f" % (k, v / a["_wo"]) if a.get("_wo") else "%s=-" % k)
            elif isinstance(v, float) and not k.endswith("_max"):
                out.append("%s=%.2f" % (k, v / a["_w"]))
            elif isinstance(v, float):
                out.append("%s=%.1f" % (k, v))
            else:
                out.append("%s=%s" % (k, v))
        print(" ".join(out))
    if thr:
        pts = sorted((float(acc[k]["snr"]), acc[k]["ok"] / acc[k]["n"]) for k in order if k[0] == "awgn")
        for level in (0.5, 0.9):
            x = None
            for (s0, r0), (s1, r1) in zip(pts, pts[1:]):
                if r0 < level <= r1:
                    x = s0 + (level - r0) * (s1 - s0) / (r1 - r0)
                    break
            print("# %d %% decode point: %s" % (round(level * 100), ("%.1f dB" % x) if x is not None else "not reached in the sweep range"))

if __name__ == "__main__":
    main()
