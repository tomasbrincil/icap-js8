#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
"""Regression tests of the review fixes of 23 Nov 2026, shell level.  NOTHING TRANSMITS: the RX+TX app (built as the
radio build is: no -DJS8_QTX) runs in the loader simulator js8simtx; what it "sends" is a WAV file, which a separate
run of the RX-only simulator decodes back.  No socket is opened.

  python3 -I -B tests/fix_e2e.py        (make fixe2e; about half a minute)

Every run gets a new directory under build/rfix/run-<time>/.  Writes build/rfix/checks.md, exits 1 if a check
failed.  The engine-level, terminal-link and storage cases of the same review are suites of the development tree that
are not part of this release.

  F1  a disarm BETWEEN two frames of a message drops it: arming again sends nothing
  F2  the beacon is already armed when the app starts: that arming does not count
  F3  a preset that starts with "NEXT " is sent as confirmed (it was swallowed)
  F4  "@HB HEARTBEAT JO70" is one heartbeat frame
  F5  a dictionary without JSCSUM.BIN is not used for transmitting: a flipped bit cannot change the words on the air
  F6  JS8.CFG words: tx=off, tx=maybe, txtest=on
  F7  a text that starts with a backtick is refused
  F8  txpath=1 in the radio build: a note, and no transmission without arming
  F9  a directed message to a compound call the packing cannot hold whole is not sent to a truncated call
  K1..K10  the keyboard control (24 Nov 2026); K11..K20 the operator-flow review of 24 Nov 2026 (open timer after Mn/Pn, nothing to
           send / X / CANCEL, ONE waiting message, HB only on a fresh arming, the app closes the keyboard (stop, 120 s), taps while
           granted, retarget, a refused text reopens, strategy B and the disarm, JS8MEM.TXT outside the decoder's poll)
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import wave
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gestconv import conv, with_tx   # noqa: E402  (the gesture menu of 23 Nov 2026: owner scripts, tx=1)

HOME = os.path.expanduser('~')
SRC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.environ.get('BUILD') or SRC + '/build'
SDK = os.environ.get('ICAP_SDK') or os.environ.get('SDK') or SRC + '/third_party/icap-sdk'
SD = BUILD + '/sd'
MIX = BUILD + '/e2e/mix.wav'                 # receive traffic (tests/e2e.py)
SIM, SIMTX = BUILD + '/js8sim', BUILD + '/js8simtx'
ROOT = BUILD + '/rfix/run-' + time.strftime('%Y%m%d-%H%M%S')
L3 = 'Q7AB MSG HELLO THERE HOW ARE YOU'
FRAME = 101120
checks = []


def check(name, ok, detail=''):
    checks.append((name, bool(ok), str(detail)))
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, (' - ' + str(detail)) if detail else ''), flush=True)
    return ok


def sh(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, errors='replace', **kw)


def build():
    core = ['src/js8dec.c', 'src/js8msg.c', 'src/js8jsc.c', 'src/js8asm.c', 'src/js8feat.c', 'src/js8_constants.c', 'src/js8crc.c', 'src/js8enc.c', 'shell/digi_util.c']
    cf = ['cc', '-std=gnu99', '-O2', '-Wall', '-Wextra', '-Wshadow', '-fno-math-errno']
    r1 = sh(cf + ['-DDIGI_HOST', '-I' + SDK, '-Ishell', '-Isrc', 'tests/js8sim.c'] + core + ['-lm', '-o', SIM], cwd=SRC)
    r2 = sh(cf + ['-ffp-contract=off', '-DDIGI_HOST', '-DJS8_TX', '-DJS8_KBD', '-I' + SDK, '-I' + SDK + '/apps/digi-common', '-Ishell', '-Isrc', 'tests/js8sim.c'] + core +
            ['src/js8tx.c', 'src/js8pack.c', SDK + '/apps/digi-common/txsynth.c', '-lm', '-o', SIMTX], cwd=SRC)
    return check('build js8sim and js8simtx (RX+TX as the radio build: no -DJS8_QTX), no warnings', r1.returncode == 0 and r2.returncode == 0 and not (r1.stdout + r2.stdout).strip(),
                 (r1.stdout + r2.stdout)[:300])


class Run:
    def __init__(self, name, owner, cfg='', flags='on', tail=20, sd=SD, rx=False, wav=None, rawcfg=None, mem=None, extra=(), loops=1):
        self.dir = os.path.join(ROOT, name)
        os.makedirs(self.dir)
        cfg, owner = with_tx(cfg), conv(owner, cfg, 'JO70')
        if rawcfg is not None:                   # JS8.CFG byte for byte (no tx=1 added)
            open(os.path.join(self.dir, 'JS8.CFG'), 'wb').write(rawcfg)
        elif cfg:
            open(os.path.join(self.dir, 'JS8.CFG'), 'w').write(cfg.replace('\n', '\r\n') + '\r\n')
        if mem is not None:                      # JS8MEM.TXT (the CW keyer memories of MEM.APP), byte for byte
            open(os.path.join(self.dir, 'JS8MEM.TXT'), 'wb').write(mem)
        self.wav = os.path.join(ROOT, name + '.tx.wav')
        cmd = [SIM if rx else SIMTX, '-d', self.dir, '-J', sd, '-k', '0', '-T', str(tail), '-L', str(loops)] + list(extra)
        if not rx:
            cmd += ['-X', flags, '-w', self.wav, '-c', 'OK1BT', '-g', 'JO70aa']
            if owner:
                cmd += ['-O', owner]
        else:
            cmd += ['-c', 'OK9RX']
        cmd.append(wav or QUIET)
        r = sh(cmd)
        self.rc, self.out = r.returncode, r.stdout
        open(os.path.join(ROOT, name + '.out.txt'), 'w').write(' '.join(cmd) + '\n' + self.out)
        self.d = [l[2:] for l in self.out.splitlines() if l.startswith('D ')]
        self.x = [l for l in self.out.splitlines() if l.startswith('X ')]
        self.w = dict((int(l[1]), l[3:]) for l in self.out.splitlines() if re.match(r'^W\d ', l))

    def s(self, key):
        m = re.search(r'\b%s=(-?\d+)' % key, self.out)
        return int(m.group(1)) if m else None

    def tx(self, key):
        m = re.search(r'^# txmodel .*\b%s=(-?\d+)' % key, self.out, re.M)
        return int(m.group(1)) if m else None

    def said(self, sub):
        return any(sub in l for l in self.d)

    def said_n(self, sub):
        return sum(1 for l in self.d if sub in l)

    def log(self):
        out = []
        for fn in sorted(os.listdir(self.dir)):
            if fn.startswith('JS8L'):
                out += open(os.path.join(self.dir, fn), errors='replace').read().splitlines()
        return out

    def notes(self):
        return [l[9:] for l in self.log() if l.startswith('N ')]

    def bursts(self):
        if not os.path.exists(self.wav):
            return []
        w = wave.open(self.wav, 'rb'); n = w.getnframes(); x = struct.unpack('<%dh' % n, w.readframes(n)); w.close()
        out, s, last = [], None, None
        for i, v in enumerate(x):
            if v:
                if s is None:
                    s = i
                elif i - last > 80:
                    out.append((s, last + 1)); s = i
                last = i
        if s is not None:
            out.append((s, last + 1))
        return out

    def zero(self):
        return self.rc == 0 and self.tx('samples') == 0 and self.tx('transmissions') == 0 and self.tx('begin') == 0 and not self.bursts()


def rx_texts(name, wavfile):
    rx = Run(name, '', rx=True, wav=wavfile, tail=20)
    out = []
    for fn in sorted(os.listdir(rx.dir)):
        if fn.startswith('JS8M'):
            for l in open(os.path.join(rx.dir, fn), errors='replace').read().splitlines():
                m = re.match(r'^M \S+ \S+ \d+ (\d+) \S+ (.*)$', l)
                if m:
                    out.append((int(m.group(1)), m.group(2).rstrip()))
    return out


TAPS = '3 tap JS8 TX;5 tap TX> %s;8 tap SEND? %s'


def st(r):                                       # the transmit status (line 1 of the app window)
    return r.w.get(1, '')


def item(r, text):
    """the menu block listed an item that starts with text (active "> " or not "  ")"""
    return any(l.startswith(('> ' + text, '  ' + text)) for l in r.d)


def main():
    global QUIET
    os.makedirs(ROOT)
    QUIET = ROOT + '/quiet.wav'
    w = wave.open(QUIET, 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000); w.writeframes(b'\0\0' * (8000 * 150)); w.close()
    if not build():
        return 1

    # F1 ------------------------------------------------------------------------------------------------------------
    r = Run('f1_disarm_gap', TAPS % ('Q7AB', 'Q7AB') + ';10 arm;28.3 disarm;100 arm', cfg='tx1=' + L3, tail=80)
    b = r.bursts()
    check('F1 3-frame message, disarm in the gap after frame 1 (28.3 s), arm again at 100 s with no new human action: exactly ONE frame went out, one tx_begin, '
          'note "TX: disarmed, waiting messages dropped"',
          r.rc == 0 and len(b) == 1 and FRAME - 8 <= b[0][1] - b[0][0] <= FRAME and r.tx('begin') == 1 and r.tx('transmissions') == 1 and
          any('disarmed, waiting messages dropped' in n for n in r.notes()),
          'bursts at %s s, tx_begin %s, notes %s' % ([round(s / 8000.0, 2) for s, e in b], r.tx('begin'), [n for n in r.notes() if 'TX' in n]))
    # (safety review 23 Nov 2026, finding 2) a disarm before the first frame clears what waited for that arming: confirm, arm, disarm = nothing
    r = Run('f1_disarm_before_first', TAPS % ('Q7AB', 'Q7AB') + ';10 arm;12 disarm;40 arm', cfg='tx1=' + L3, tail=80)
    check('F1 confirmed, armed, disarmed before the first frame, armed again at 40 s: ZERO samples, note "TX: disarmed, waiting messages dropped"',
          r.zero() and any('disarmed, waiting messages dropped' in n for n in r.notes()), 'samples %s, notes %s' % (r.tx('samples'), [n for n in r.notes() if 'TX' in n]))
    r = Run('f1_queue_then_arm', TAPS % ('Q7AB', 'Q7AB') + ';40 arm', cfg='tx1=' + L3, tail=80)
    b = r.bursts()
    check('F1 control: confirm first (disarmed), arm second - the README procedure: 3 frames after the arming',
          r.rc == 0 and len(b) == 3 and all(FRAME - 8 <= e - s <= FRAME for s, e in b) and b[0][0] > 40 * 8000, 'bursts at %s s' % [round(s / 8000.0, 2) for s, e in b])

    # F2 ------------------------------------------------------------------------------------------------------------
    r = Run('f2_armed_at_start', TAPS % ('HB', 'HB'), flags='on,tapsarmed,armed', tail=60)
    check('F2 the beacon is armed BEFORE the app starts (a leftover FT8.CFG with beacon=1), heartbeat confirmed by two taps, 2 minutes: ZERO samples, zero tx_begin, '
          'note "TX: red frame at start - off, on again"',
          r.zero() and r.said('1 part: MODE > hold FT8 (red), 2 min') and any('red frame at start' in n for n in r.notes()),
          'samples %s, tx_begin %s; notes %s' % (r.tx('samples'), r.tx('begin'), [n for n in r.notes() if 'TX' in n]))
    r = Run('f2_then_rearmed', TAPS % ('HB', 'HB') + ';30 disarm;33 arm', flags='on,tapsarmed,armed', tail=60)
    b = r.bursts()
    check('F2 then the owner disarms and arms (a real long press): the confirmed heartbeat goes out, one frame',
          r.rc == 0 and len(b) == 1 and FRAME - 8 <= b[0][1] - b[0][0] <= FRAME and b[0][0] > 33 * 8000, 'bursts at %s s' % [round(s / 8000.0, 2) for s, e in b])

    # F3 ------------------------------------------------------------------------------------------------------------
    r = Run('f3_next_preset', TAPS % ('NEXT WEEK', 'NEXT WEEK') + ';10 arm', cfg='tx1=NEXT WEEK SAME TIME', tail=80)
    rt = rx_texts('f3_rx', r.wav) if r.bursts() else []
    check('F3 preset "NEXT WEEK SAME TIME" confirmed and armed: it is transmitted, and a separate receiver reads "OK1BT: NEXT WEEK SAME TIME"',
          r.rc == 0 and r.said('MODE > hold FT8 (red), 2 min') and len(rt) == 1 and rt[0][1] == 'OK1BT: NEXT WEEK SAME TIME', 'received %s; rows %s' % (rt, [l for l in r.d if 'TX' in l][-3:]))

    # F4 ------------------------------------------------------------------------------------------------------------
    r = Run('f4_at_hb', TAPS % ('@HB', '@HB') + ';10 arm', cfg='tx1=@HB HEARTBEAT JO70', tail=60)
    b = r.bursts()
    rt = rx_texts('f4_rx', r.wav) if b else []
    check('F4 preset "@HB HEARTBEAT JO70": ONE frame (it was a directed frame + a data frame), received as the heartbeat "OK1BT: @HB HEARTBEAT JO70"',
          r.rc == 0 and len(b) == 1 and r.said('1 part: MODE > hold FT8 (red), 2 min') and len(rt) == 1 and rt[0] == (1, 'OK1BT: @HB HEARTBEAT JO70'), 'frames %d, received %s' % (len(b), rt))

    # F5 ------------------------------------------------------------------------------------------------------------
    sd2 = os.path.join(ROOT, 'sd_nosum')
    os.makedirs(sd2)
    for fn in os.listdir(SD):
        if fn.startswith('JSC') and fn != 'JSCSUM.BIN':
            shutil.copy(os.path.join(SD, fn), sd2)
    bb = bytearray(open(sd2 + '/JSCE0EF.BIN', 'rb').read())
    p, hit = 8, False
    while p < len(bb):
        n = bb[p]
        if bytes(bb[p + 1:p + 1 + n]) == b'HELLO':
            bb[p + 1 + n] ^= 0x40; hit = True
            break
        p += 4 + n
    open(sd2 + '/JSCE0EF.BIN', 'wb').write(bb)
    r = Run('f5_unchecked_dict', TAPS % ('HELLO', 'HELLO') + ';10 arm;40 disarm;42 tap JS8 TX;44 tap TX> HB;47 tap SEND? HB;50 arm', cfg='tx1=HELLO HELLO', sd=sd2, tail=60)
    b = r.bursts()
    rt = rx_texts('f5_rx', r.wav) if b else []
    check('F5 dictionary WITHOUT JSCSUM.BIN and one bit flipped in the entry of HELLO (JSCE0EF.BIN): "HELLO HELLO" is refused ("text not encodable") - the unchecked '
          'dictionary is not used for transmitting, so nothing but the confirmed words can go out; the heartbeat (no dictionary needed) still works',
          hit and r.rc == 0 and r.said('TX refused: text not encodable') and len(b) == 1 and rt == [(1, 'OK1BT: @HB HEARTBEAT JO70')] and
          any('dictionary not checked' in n for n in r.notes()), 'frames %d, received %s (before the fix: "OK1BT: BUQ BUQ")' % (len(b), rt))
    r = Run('f5_checked_dict', TAPS % ('HELLO', 'HELLO') + ';10 arm', cfg='tx1=HELLO HELLO', tail=60)
    rt = rx_texts('f5c_rx', r.wav) if r.bursts() else []
    check('F5 control: with the checked dictionary "HELLO HELLO" is sent compressed and received as "OK1BT: HELLO HELLO"', len(rt) == 1 and rt[0][1] == 'OK1BT: HELLO HELLO', rt)

    # F6 ------------------------------------------------------------------------------------------------------------
    for val, note in (('off', None), ('no', None), ('maybe', 'bad tx= value, TX is OFF'), ('-1', 'bad tx= value, TX is OFF'), (' 0', None)):
        r = Run('f6_tx_%s' % val.strip().replace('-', 'm'), '1 arm;' + TAPS % ('HB', 'HB') + ';20 tap JS8;22 tap TX> HB;25 tap SEND? HB', cfg='tx=' + val, flags='on,tapsarmed', tail=40)
        check('F6 JS8.CFG "tx=%s", armed, taps on the menu rows: TX is off - no menu, ZERO samples%s' % (val, ', note "%s"' % note if note else ''),
              r.zero() and ' kbd open ' not in ''.join(r.x) and (note is None or any(note in n for n in r.notes())) and
              (note is not None or not any('JS8.CFG' in n for n in r.notes())), 'samples %s, notes %s' % (r.tx('samples'), [n for n in r.notes() if 'CFG' in n]))
    r = Run('f6_txtest_on', TAPS % ('CQ', 'CQ') + ';10 arm', cfg='txtest=on\ntx1=' + L3, tail=40)
    check('F6 JS8.CFG "txtest=on": TXTEST is on (status line), ZERO samples for the CQ request',
          r.zero() and st(r).startswith('TXTEST'), st(r))
    r = Run('f6_txtest_bad', TAPS % ('CQ', 'CQ') + ';10 arm', cfg='txtest=2x\ntx1=' + L3, tail=40)
    check('F6 JS8.CFG "txtest=2x" (not understood): TXTEST is ON (fail safe), note "JS8.CFG: bad txtest= value, TXTEST on"',
          r.zero() and st(r).startswith('TXTEST') and any('bad txtest= value, TXTEST on' in n for n in r.notes()), [n for n in r.notes() if 'CFG' in n])
    r = Run('f6_caps_bad', '', cfg='txsess=0\ntxttl=5', tail=20)
    check('F6 JS8.CFG "txsess=0", "txttl=5" (out of range): defaults kept, ONE note "JS8.CFG: bad tx* value, default kept"',
          sum(1 for n in r.notes() if 'bad tx* value, default kept' in n) == 1, [n for n in r.notes() if 'CFG' in n])

    # F7 ------------------------------------------------------------------------------------------------------------
    r = Run('f7_backtick', TAPS % ('`Q7AB', '`Q7AB') + ';10 arm', cfg='tx1=`Q7AB JO70', tail=40)
    check('F7 preset "`Q7AB JO70" (JS8Call\'s raw compound syntax, not built here): "TX refused: bad text", ZERO samples', r.zero() and r.said('TX refused: bad text'),
          [l for l in r.d if 'TX' in l][-2:])

    # F8 ------------------------------------------------------------------------------------------------------------
    r = Run('f8_txpath', TAPS % ('HB', 'HB') + ';30 tap JS8 TX;32 tap TX> HB;35 tap SEND? HB', cfg='txpath=1', tail=60)
    check('F8 JS8.CFG "txpath=1" (any message on tap grants: gone since 24 Nov 2026), heartbeat sent with ENT and txtap=0, beacon NEVER armed: ZERO samples, "@QTX" never asked, '
          'note "JS8.CFG: txpath is gone (see txtap)"',
          r.zero() and r.tx('qtx') == 0 and r.said('MODE > hold FT8 (red), 2 min') and any('txpath is gone' in n for n in r.notes()),
          'samples %s, @QTX asked %s, notes %s' % (r.tx('samples'), r.tx('qtx'), [n for n in r.notes() if 'CFG' in n]))

    # F9 ------------------------------------------------------------------------------------------------------------
    r = Run('f9_long_compound', TAPS % ('DL/Q7AB/P', 'DL/Q7AB/P') + ';10 arm', cfg='tx1=DL/Q7AB/P SNR?', tail=80)
    b = r.bursts()
    rt = rx_texts('f9_rx', r.wav) if b else []
    check('F9 "DL/Q7AB/P SNR?" (a call the 50-bit packing cannot hold whole): sent as JS8Call sends it, as free text with every character - never as a query to the '
          'truncated call "DL/Q7AB/"', r.rc == 0 and len(rt) == 1 and rt[0][1] == 'OK1BT: DL/Q7AB/P SNR?', 'frames %d, received %s' % (len(b), rt))

    # F10 (safety review 23 Nov 2026, finding 5) JS8.CFG as people write it ------------------------------------------------
    r = Run('f10_bom', '', rawcfg=b'\xef\xbb\xbftx=1\r\ntxtest=1\r\n', tail=20)
    check('F10 JS8.CFG with a UTF-8 BOM before "tx=1": the first key counts (TX on, TXTEST on)', st(r).startswith('TXTEST disarmed'), st(r))
    r = Run('f10_comments', '', rawcfg=b'tx=1\r\n# txtest=0\r\n  ; tx=0\r\ntxtest=1 # the first runs\r\ninfo=QRP #1 txtest=0\r\n', tail=20)
    check('F10 "#" / ";" comment lines and an inline "# ..." are ignored; a text key (info=) keeps "#" and does not hide a key in its text: TX on, TXTEST on',
          st(r).startswith('TXTEST disarmed') and not any('JS8.CFG' in n for n in r.notes()), '%s; notes %s' % (st(r), r.notes()))
    r = Run('f10_case_blanks', '', rawcfg=b'TX = 1\r\nTxTest =  1\r\nTXSESS= 7\r\n', tail=20)
    check('F10 key names in any case, blanks around "=": "TX = 1", "TxTest =  1", "TXSESS= 7" are taken', st(r).startswith('TXTEST disarmed') and st(r).endswith('s0/7'), st(r))
    r = Run('f10_default_off', '20 taprow 0;23 kbd HB JO70;25 arm', rawcfg=b'txsess=7\r\n', tail=40)
    check('F10 no tx= line: TX is OFF - a tap opens no keyboard, ZERO samples', r.zero() and st(r).startswith('TX off') and ' kbd open ' not in ''.join(r.x), st(r))
    r = Run('f10_unknown', '', rawcfg=b'tx=1\r\ntxtset=1\r\n', tail=20)
    check('F10 a misspelt tx* key ("txtset=1"): refused with the note "JS8.CFG: unknown tx* key, ignored"', any('unknown tx* key' in n for n in r.notes()), r.notes())
    r = Run('f10_too_long', '', rawcfg=b'tx=1\r\n' + b'flo=450\r\n' * 110 + b'txtest=0\r\n', tail=20)
    check('F10 JS8.CFG longer than the app reads (%d B): said ("JS8.CFG: too long, rest ignored, TXTEST on"), and fail safe - TXTEST on' % (6 + 9 * 110 + 10),
          any('too long' in n for n in r.notes()) and st(r).startswith('TXTEST'), '%s; notes %s' % (st(r), [n for n in r.notes() if 'CFG' in n]))

    # K: the keyboard control (owner decision 24 Nov 2026): a tap on a row opens the radio's keyboard, ENT sends --------------
    def kopen(r):
        return [x.split(' kbd open ', 1)[1].strip('"') for x in r.x if ' kbd open ' in x]
    r = Run('k1_open', '20 tap Q7AB: @HB;23 kbdexit;25 taprow 0;27 kbdexit', wav=MIX, tail=30)
    ko = kopen(r)
    check('K1 a tap on a station\'s decoded row opens the radio\'s keyboard with "<CALL> "; a tap on another row with an empty text; never while the finger is '
          'on the screen (the app waits for the release + 300 ms); EXIT sends nothing ("TX: keyboard closed, nothing sent")',
          ko[:1] == ['Q7AB'] and len(ko) == 2 and r.s('kbd_under_finger') == 0 and r.said('TX: keyboard closed, nothing sent') and r.zero(), 'opened with %s' % ko)
    r = Run('k2_tap_credit', '3 taprow 0;4 kbd HB JO70', cfg='txtap=1', tail=40)
    rt = rx_texts('k2_rx', r.wav) if r.bursts() else []
    check('K2 ENT on ONE frame ("HB JO70"), beacon never armed: the loader\'s tap credit (@QTX) sends it in the next slot - no tx_begin, no arming; received as the heartbeat',
          len(r.bursts()) == 1 and r.tx('begin') == 0 and r.tx('qtx_ok') == 1 and r.said('TX: on air at the next slot (tap)') and rt == [(1, 'OK1BT: @HB HEARTBEAT JO70')],
          'tx_begin %s, grants %s, received %s' % (r.tx('begin'), r.tx('qtx_ok'), rt))
    r = Run('k3_frames', '3 taprow 0;4 kbd ' + L3 + ';30 arm', cfg='txtap=1', tail=80)
    check('K3 ENT on a 3-frame message: queued, "3 parts: MODE > hold FT8 (red), 2 min", the tap credit is not asked; the red frame sends the 3 frames; '
          'then the rows "SENT: <text>" and "red frame off to tap again" (taps are not delivered while armed)',
          r.said('3 parts: MODE > hold FT8 (red), 2 min') and r.tx('qtx') == 0 and len(r.bursts()) == 3 and r.tx('begin') == 3
          and any(r.d[i] == 'SENT: ' + L3[:34].rstrip() and r.d[i + 1:i + 2] == ['red frame off to tap again'] for i in range(len(r.d))) and not r.said('TX done'),
          'bursts %d, qtx %s' % (len(r.bursts()), r.tx('qtx')))
    r = Run('k3_refused', '3 taprow 0;4 kbd HB JO70', cfg='txtap=1', flags='on,once', tail=40, extra=('-l', '1.5'))
    r = Run('k3_nocredit', '2 arm;3 disarm;3.5 taprow 0;4 kbd HB JO70', cfg='txtap=1', flags='on,tapsarmed', tail=40)
    r = Run('k4_cancel', '3 taprow 0;4 kbd ' + L3 + ';10 taprow 0;11 kbd;20 arm', cfg='txtap=1', tail=60)
    check('K4 the cancel: ENT on an EMPTY keyboard clears the queue ("TX: cancelled, queue cleared"); arming afterwards sends nothing', r.zero() and r.said('TX: cancelled, queue cleared'), r.notes()[-3:])
    mem = (b'm1=q7ab de ok1bt\r\nm2=bad \x01 byte\r\nm3=~~~ ^^^\r\nm4=' + b'Q7AB HELLO THERE THIS IS A LONG KEYER MEMORY TEXT ABOUT SEVENTY CHARS' + b'\r\n'
           b'garbage line\r\nm9=nine\r\n\xff\xfe\x00\r\n')
    r = Run('k5_memory', '3 taprow 0;4 kbd M1;6 kbd =;8 taprow 0;9 kbd M4;11 kbdexit;13 taprow 0;14 kbd M7;16 arm', cfg='txjsc=0', mem=mem, tail=60)
    ko = kopen(r)
    rt = rx_texts('k5_rx', r.wav) if r.bursts() else []
    check('K5 keyer memories (JS8MEM.TXT of MEM.APP): "M1" + ENT opens the keyboard again WITH memory 1 - the owner sees it whole and sends it with ENT; '
          'a 70-character memory is REFUSED ("TX: M4 has 69 chars, max 43", review 24 Nov 2026: never cut - the keyboard does not open with it); a missing memory says so',
          'q7ab de ok1bt' in ko and not any(x.startswith('Q7AB HELLO') for x in ko) and r.said('TX: M4 has 69 chars, max 43') and r.said('no such keyer memory')
          and [t for n, t in rt] == ['OK1BT: Q7AB DE OK1BT'], 'opened with %s; received %s' % (ko, rt))
    r = Run('k6_preset', '3 taprow 0;4 kbd P1;6 kbd =;8 arm', cfg='tx1=' + L3, tail=80)
    check('K6 "P1" + ENT opens the keyboard again with preset tx1 of JS8.CFG; ENT sends it', L3.upper() in kopen(r) and len(r.bursts()) == 3, kopen(r))
    # (K7, a tap on a query opens the keyboard with the prepared reply: tests/feat_e2e.py F1c, which has the traffic for it)
    r = Run('k8_txtest', '3 taprow 0;4 kbd =', cfg='txtest=1\ntxtap=1', tail=40)
    check('K8 TXTEST: the keyboard opens with "HB JO70" (the only text TXTEST sends); ENT: one frame on the tap credit', kopen(r)[:1] == ['HB JO70'] and len(r.bursts()) == 1, kopen(r))
    r = Run('k9_stop_open', '3 taprow 0;6 stop;12 kbdexit', tail=30)
    check('K9 (review 24 Nov 2026) a stop request while the keyboard is open: the app CLOSES the keyboard itself (0x200cd828) and leaves only when the menu id is no '
          'longer 349 - note "KBD open at exit: closing it", the keyboard is not open at the end, nothing sent',
          any('KBD open at exit: closing it' in n for n in r.notes()) and r.rc == 0 and r.s('closed_by_app') == 1 and r.s('open_at_end') == 0 and r.zero(),
          '%s; closed_by_app %s open_at_end %s' % (r.notes()[-2:], r.s('closed_by_app'), r.s('open_at_end')))
    r = Run('k9_stop_stuck', '3 taprow 0;5 kbdstuck;8 stop', tail=30)
    check('K9 an EXIT that leaves the menu id at 349, then a stop request: the app closes the keyboard before it leaves (open at the end: 0)',
          r.rc == 0 and r.s('closed_by_app') == 1 and r.s('open_at_end') == 0 and r.zero(), 'closed_by_app %s open_at_end %s' % (r.s('closed_by_app'), r.s('open_at_end')))
    r = Run('k10_armed', '2 arm;5 taprow 0;6 kbd HB JO70', tail=30)
    check('K10 beacon armed: taps are not delivered (SDK), so no keyboard opens and nothing is sent by a tap', not kopen(r) and r.s('taps_not_delivered') >= 1, kopen(r))

    # K11..K21: the operator-flow review of 24 Nov 2026 -------------------------------------------------------------------
    r = Run('k11_mem_late', '3 taprow 0;15 kbd M1;17 kbd =;30 arm', cfg='txjsc=0', mem=b'm1=q7ab de ok1bt\r\n', tail=60)
    check('K11 "M1" + ENT more than 10 s after the keyboard opened: the keyboard opens again with the memory (the 10 s open timer restarts; it said '
          '"keyboard not opened (screen held)" before), ENT sends it', 'q7ab de ok1bt' in kopen(r) and not r.said('keyboard not opened') and len(r.bursts()) >= 1,
          'opened with %s; rows %s' % (kopen(r), [l for l in r.d if 'TX' in l][-3:]))
    r = Run('k11_preset_late', '3 taprow 0;15 kbd P1;17 kbd =;30 arm', cfg='tx1=' + L3, tail=80)
    check('K11 "P1" + ENT more than 10 s after the keyboard opened: the keyboard opens again with the preset, ENT sends it (3 frames)',
          L3.upper() in kopen(r) and not r.said('keyboard not opened') and len(r.bursts()) == 3, kopen(r))
    r = Run('k12_call_only', '20 tap Q7AB: @HB;21 kbd =;23 taprow 0;24 kbd Q7AB;26 taprow 0;27 kbd A;30 arm', wav=MIX, cfg='txtap=1', tail=30)
    check('K12 the untouched "<CALL> " of a tap on a station\'s row, a callsign alone, one character: "TX: nothing to send - type a message" each time, nothing queued, ZERO samples',
          kopen(r)[:1] == ['Q7AB'] and sum(1 for l in r.d if l == 'TX: nothing to send - type a message') == 3 and r.zero() and r.tx('qtx') == 0,
          [l for l in r.d if l.startswith('TX')][-4:])
    r = Run('k12_x_cancel', '3 taprow 0;4 kbd ' + L3 + ';8 taprow 0;9 kbd X;12 taprow 0;13 kbd ' + L3 + ';16 taprow 0;17 kbd cancel;20 arm', tail=60)
    check('K12 "X" and "CANCEL" (any case) on the keyboard are the cancel: the queue is cleared twice ("TX: cancelled, queue cleared"), arming sends nothing',
          r.zero() and sum(1 for l in r.d if l == 'TX: cancelled, queue cleared') == 2, [l for l in r.d if l.startswith('TX')][-4:])
    r = Run('k13_replace', '3 taprow 0;4 kbd ' + L3 + ';8 taprow 0;9 kbd HB JO70;20 arm', tail=60)
    b = r.bursts()
    rt = rx_texts('k13_rx', r.wav) if b else []
    check('K13 ONE waiting message: a 3-frame message waits, a new ENT "HB JO70" replaces it (ONE row "replaced: MODE > hold FT8 (red), 1 part"); '
          'that ONE frame only goes out', r.said_n('replaced: ') == 1 and r.said('replaced: MODE > hold FT8 (red), 1 part') and len(b) == 1
          and rt == [(1, 'OK1BT: @HB HEARTBEAT JO70')], 'bursts %d, received %s' % (len(b), rt))
    r = Run('k13_in_progress', '3 taprow 0;4 kbd ' + L3 + ';10 arm;40 disarm;45 taprow 0;46 kbd HB JO70;50 arm', flags='on,once', tail=80)
    b = r.bursts()
    check('K13 a message that sent frame 1 (the loader refused the next slot: held for a new arming): the disarm drops it (S1 24 Nov 2026, "TX: red frame off - message dropped"); '
          'a new ENT, the next arming sends only the new frame (2 frames in all)', r.said('TX: red frame off - message dropped') and len(b) == 2, 'bursts %d, rows %s' % (len(b), [l for l in r.d if l.startswith('TX')][-4:]))
    r = Run('k14_hb_after_queued', '3 taprow 0;4 kbd ' + L3 + ';10 arm;700 disarm;703 arm', cfg='txhb=10', tail=60, loops=5)
    b = r.bursts()
    check('K14 txhb=10: an arming that sends a queued message does not start the HB (3 frames, no HB in 11 min of that arming); the next arming, made with the '
          'queue empty, sends HB at once', len(b) == 4 and b[3][0] > 703 * 8000 and sum(1 for n in r.notes() if 'HB queued' in n) == 1,
          'bursts at %s s' % [round(s0 / 8000.0) for s0, e in b])
    r = Run('k15_idle', '3 taprow 0', tail=200)
    check('K15 the keyboard left open with no ENT: after 120 s the app closes it ("KBD: keyboard closed by the app"), nothing sent',
          r.said('KBD: keyboard closed by the app') and r.s('closed_by_app') == 1 and r.s('open_at_end') == 0 and r.zero(), 'closed_by_app %s' % r.s('closed_by_app'))
    r = Run('k15_stuck', '3 taprow 0;5 kbdstuck', tail=200)
    x = [l for l in r.x if 'kbd closed by the app' in l]
    check('K15 EXIT, but the menu id stays 349: the app closes the keyboard after 120 s (and not before)', r.s('closed_by_app') == 1 and r.s('open_at_end') == 0 and x
          and float(x[0].split()[1]) >= 120000, x)
    r = Run('k16_tap_granted', '3 taprow 0;4 kbd HB JO70;6 taprow 0;7 kbd ' + L3, cfg='txtap=1', tail=40)
    check('K16 (S3 24 Nov 2026) a tap while the tap-credit frame is granted for the next slot gives the grant back ("TX: cancelled before the slot"), no keyboard, nothing on the air',
          len(kopen(r)) == 1 and len(r.bursts()) == 0 and r.tx('qtx_ok') == 1 and r.said('TX: cancelled before the slot'), 'opened %s, bursts %d' % (kopen(r), len(r.bursts())))
    r = Run('k17_retarget', '20 tap Q7AB: @HB;20.1 tapraw RETARGET ROW;24 kbdexit', wav=MIX, extra=('-G', '0'), tail=30)
    check('K17 a quick second tap before the keyboard opened retargets it: it opens ONCE, with the text of the second row (empty), not "Q7AB "',
          kopen(r) == [''] and r.s('kbd_under_finger') == 0, kopen(r))
    r = Run('k18_keep_refused', '3 taprow 0;4 kbd `Q7AB HELLO;8 taprow 0;9 kbdexit;12 taprow 0;13 kbdexit', tail=30)
    check('K18 a text refused for its encoding ("TX refused: bad text") reopens the keyboard with that text on the next tap (to be edited); the tap after that opens empty',
          r.said('TX refused: bad text') and kopen(r)[1:] == ['`Q7AB HELLO', ''], kopen(r))
    r = Run('k19_strat_b', '3 taprow 0;4 kbd ' + L3 + ';10 arm;40 disarm;45 arm;80 disarm;85 arm', cfg='txstrat=B', tail=80)
    check('K19 txstrat=B: frame 1 under the arming, the held rest is dropped by the disarm (confirm, arm, disarm = nothing further): ONE frame in two more armings',
          len(r.bursts()) == 1 and any('disarmed, waiting messages dropped' in n for n in r.notes()), 'bursts %d' % len(r.bursts()))
    r = Run('k20_mem_decode', '28 taprow 0;30.05 kbd M1;40 kbd =', wav=MIX, cfg='txjsc=0', mem=b'm1=q7ab de ok1bt\r\n', extra=('-q', '3'), tail=30)
    ent = [l for l in r.x if 'kbd ENT "M1' in l]
    check('K20 "M1" + ENT arriving while a decode runs (slow-CPU model -q 3): JS8MEM.TXT is not read inside the decoder\'s abort poll (0 opens during a decode), '
          'the main loop reads it after the decode and the keyboard opens with the memory', ent and 'during a decode' in ent[0] and r.s('mem_in_decode') == 0
          and 'q7ab de ok1bt' in kopen(r), 'ENT %s; opens during a decode %s; opened %s' % (ent, r.s('mem_in_decode'), kopen(r)))

    # G: the green frame and the HB beacon
    r = Run('g3_cq_green', '2 cq 1;70 cq 0', tail=60)
    check('G3 NO CQ (owner decision 24 Nov 2026): the green frame on for 68 s sends NOTHING, asks no CQ grant (@QTXC), no tx_begin; row "green not used - hold FT8 again"',
          r.zero() and r.tx('qtx') == 0 and r.tx('begin') == 0 and r.said('green not used - hold FT8 again'), r.notes())
    r = Run('g3_cq_txtest', '2 cq 1', cfg='txtest=1', tail=40)
    check('G3 the green frame in TXTEST: nothing', r.zero() and r.tx('qtx') == 0, r.notes())
    r = Run('g7_hb_beacon', '2 arm', cfg='txhb=10', tail=60, loops=10)
    b = r.bursts()
    check('G7 HB beacon (txhb=10, beacon armed at 2 s, 25 min): "HB JO70" at once and every 10 min - 3 frames; the status line ends "HB10"',
          len(b) == 3 and 590 <= (b[1][0] - b[0][0]) / 8000.0 <= 616 and r.w.get(1, '').endswith('HB10'), 'frames at %s s, status %s' % ([round(s / 8000.0) for s, e in b], r.w.get(1)))

    # D: the dictionary on the SD card ROOT (loader limit seen on the radio 23 Nov 2026: apps listed after ~257 dictionary files are not shown)
    empty = os.path.join(ROOT, 'empty_apps'); os.makedirs(empty)
    r = Run('d1_dict_root', '3 taprow 0;4 kbd HB JO70', wav=MIX, sd=empty, extra=('-K', SD), tail=30)
    raw = [l for l in r.d if re.match(r'^\d{4} (?:[=>?] )?#', l)]
    check('D1 dictionary only on the card root (ICAP_O_ROOT), none in the app folder: it is used (note "JS8: dictionary on the card root", no raw "#" rows, '
          'the dictionary files are read from the root)', any('dictionary on the card root' in n for n in r.notes()) and not any('no dictionary' in n for n in r.notes())
          and not raw and r.s('root_opens') > 10 and (r.said('MODE > hold FT8 (red), 2 min') or r.said('on air at the next slot')), 'root opens %s, raw rows %d, notes %s' % (r.s('root_opens'), len(raw), r.notes()[:3]))
    r = Run('d2_dict_apps', '', wav=MIX, tail=30)
    check('D2 dictionary in the app folder only (the old layout): still used, nothing read from the root but the one probe',
          not any('dictionary on the card root' in n for n in r.notes()) and not any('no dictionary' in n for n in r.notes()) and r.s('root_opens') == 0, r.notes()[:3])

    os.makedirs(BUILD + '/rfix', exist_ok=True)
    bad = [c for c in checks if not c[1]]
    with open(BUILD + '/rfix/checks.md', 'w') as f:
        f.write('# Review fixes of 23 Nov 2026 - shell-level regression checks (tests/fix_e2e.py)\n\nRun %s. %d checks, %d passed, %d failed. Nothing transmits (simulator only).\n\n'
                % (ROOT, len(checks), len(checks) - len(bad), len(bad)))
        f.write('| # | check | result | detail |\n|---|---|---|---|\n')
        for i, (n, ok, d) in enumerate(checks, 1):
            f.write('| %d | %s | %s | %s |\n' % (i, n.replace('|', '/'), 'PASS' if ok else '**FAIL**', d.replace('|', '/').replace('\n', ' ')[:300]))
    print('fix_e2e: %d checks, %d failed -> %s/rfix/checks.md' % (len(checks), len(bad), BUILD))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
