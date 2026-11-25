# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
"""The decoded rows of a simulator run (tests/js8sim.c output).

Since 24 Nov 2026 a decoded row of the DECODE screen is "HHMM [m ]TEXT" (m: '=' a complete message of more than one
frame, '>' to MYCALL, '?' checksum not verified), continuation rows "     TEXT" (five blanks) - no SNR and no audio offset.
The row alone no longer tells a decoded text from a status row that starts with a time ("HHMM TX <f0> k/n ...", the BAND
view "HHMM FFFF TEXT"), so the simulator prints "Q <snr> <freq>" just before the FIRST row of every decoded text
(JS8_HOST_DECMETA in shell/js8app.c, never on the screen).  A decoded text = that row + the continuation rows right after it.
"""
import re

ROW = re.compile(r'^(\d{4}) (?:([=>?]) )?(.*)$')


def records(lines):
    """list of dict(time, snr, freq, mark, rows, lines): one per decoded text; rows = the DECODE rows as shown, lines = the
    text of each row without the prefix ("HHMM [m ]" / the five blanks).  A Q line that is not followed by a row of that form
    gives a record with time '?' (a test treats it as unexpected)."""
    out, meta, cur = [], None, None
    for l in lines:
        if l.startswith('Q '):
            q = l.split()
            meta, cur = (int(q[1]), int(q[2])), None
            continue
        if not l.startswith('D '):
            continue
        s = l[2:]
        if meta is not None:
            m = ROW.match(s)
            if m:
                cur = {'time': m.group(1), 'snr': meta[0], 'freq': meta[1], 'mark': m.group(2) or '', 'rows': [s], 'lines': [m.group(3)]}
            else:
                cur = {'time': '?', 'snr': meta[0], 'freq': meta[1], 'mark': '?', 'rows': [s], 'lines': [s]}
            out.append(cur)
            meta = None
        elif cur is not None and s.startswith(' ' * 5) and s[5:6] != ' ':
            cur['rows'].append(s); cur['lines'].append(s[5:])
        else:
            cur = None
    return out


def decoded(lines):
    """the decoded rows (first rows and their continuation rows), in order, exactly as shown"""
    return [r for x in records(lines) for r in x['rows']]


OLD = re.compile(r'^(\d{4}) ([+-]\d\d) ([ \d]{4}) (?:([=>]) )?(.*)$')


def norm_old(lines):
    """the decoded texts of a run of a build from BEFORE 24 Nov 2026 (rows "HHMM sNN FFFF [m ]TEXT" + continuations), as
    "HHMM snr freq mark text" with the blanks of the text removed (the two formats wrap at different columns)"""
    out = []
    for l in lines:
        if not l.startswith('D '):
            continue
        s = l[2:]
        m = OLD.match(s)
        if m:
            out.append([m.group(1), int(m.group(2)), int(m.group(3)), m.group(4) or '', m.group(5)])
        elif out and s.startswith(' ' * 5) and s[5:6] != ' ':
            out[-1][4] += s[5:]
    return ['%s %+03d %d %s %s' % (t, n, f, mk or '-', x.replace(' ', '')) for t, n, f, mk, x in out]


def norm_new(lines):
    """the same for a run of this tree (rows "HHMM [m ]TEXT" + the bench's Q lines); SNR / offset clamped as the old rows did"""
    return ['%s %+03d %d %s %s' % (r['time'], max(-99, min(99, r['snr'])), max(0, min(9999, r['freq'])), r['mark'] or '-', ''.join(r['lines']).replace(' ', ''))
            for r in records(lines)]


if __name__ == '__main__':                        # tests/merge_identity.sh: decrows.py old|new FILE
    import sys
    with open(sys.argv[2], errors='replace') as f_:
        ls = f_.read().splitlines()
    print('\n'.join(norm_old(ls) if sys.argv[1] == 'old' else norm_new(ls)))
