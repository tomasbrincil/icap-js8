# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
"""The owner scripts of the suites written for the old row-text menu, in the keyboard control of 24 Nov 2026.

The old menu was operated by tapping rows by their TEXT ("tap TX> HB", "tap SEND?").  Now a tap on any DECODE row opens the
radio's keyboard and ENT sends what it shows (shell/js8app_kbd.h); the simulator's owner (tests/js8sim.c) has "kbd TEXT"
(ENT with TEXT once the keyboard is open; "kbd =" keeps its text, "kbd" alone sends it empty = the cancel) and "kbdexit".
conv() maps the old events:
    tap JS8 TX / tap JS8          dropped
    tap TX> X / tap TX? X         taprow 0 (a tap on the newest row opens the keyboard); X is remembered
    tap SEND? ...                 kbd <the full text of X>: HB -> "HB <grid>", CQ -> "CQ CQ CQ <grid>", a preset tx1..tx4 of the
                                  JS8.CFG that starts with X -> that preset, else X itself
    tap TX> CANCEL                taprow 0; kbd (ENT on an empty keyboard: the queue is cleared)
    tap V> VIEW X / tap V> LOG X  cmd VIEW X / cmd LOG X (views and the log are the app window's and the terminal's now)
    everything else               unchanged (a tap on a decoded row opens the keyboard with "<CALL> ")
with_tx() puts tx=1 (TX is off by default since the review of 23 Nov 2026) and txtap=0 (the old tests expect the beacon path,
not the tap credit) in front of a JS8.CFG that does not set them."""
import re


def conv(owner, cfg='', grid='JO70'):
    pre = {}
    for m in re.finditer(r'(?im)^\s*tx([1-4])\s*=\s*(.*?)\s*$', cfg or ''):
        pre[m.group(1)] = m.group(2).upper()
    g4 = (grid or 'JO70')[:4].upper()
    loc = re.search(r'(?im)^\s*loc\s*=\s*([a-r]{2}\d\d)', cfg or '')
    if loc and not grid:
        g4 = loc.group(1).upper()

    def full(x):
        x = x.strip().upper()
        if x in ('HB', 'HB ' + g4) or x.startswith('HB '):
            return 'HB ' + g4
        if x == 'CQ' or x.startswith('CQ CQ CQ'):
            return 'CQ CQ CQ ' + g4
        for k in sorted(pre):
            if pre[k].startswith(x):
                return pre[k]
        return x
    out, pend = [], None
    for ev in re.split(r'[;\n]', owner or ''):
        m = re.match(r'\s*([\d.]+)\s+(\S+)\s*(.*?)\s*$', ev)
        if not m:
            continue
        t, c, a = m.groups()
        if c == 'tap':
            if a in ('JS8', 'JS8 TX') or a.startswith('JS8 TX'):
                continue
            if a.startswith('TX> CANCEL'):
                out += ['%s taprow 0' % t, '%s kbd' % t]; continue
            if a.startswith(('TX> ', 'TX? ')):
                pend = full(a[4:]); out.append('%s taprow 0' % t); continue
            if a.startswith('V> '):
                out.append('%s cmd %s' % (t, a[3:])); continue
            if a.startswith('SEND?'):
                out.append('%s kbd %s' % (t, pend if pend else full(a[6:]))); continue
        out.append(('%s %s %s' % (t, c, a)).strip())
    return ';'.join(out)


def with_tx(cfg):
    cfg = cfg or ''
    if not re.search(r'(?im)^\s*txtap\s*=', cfg):
        cfg = 'txtap=0\n' + cfg
    if not re.search(r'(?im)^\s*tx\s*=', cfg):
        cfg = 'tx=1\n' + cfg
    return cfg
