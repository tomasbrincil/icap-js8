#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
"""Transmit-path tests of the JS8 radio app on the PC.  NOTHING TRANSMITS: the radio's shell
(shell/js8app.c built with -DJS8_TX) runs inside the loader simulator tests/js8sim.c, whose TX model
captures everything the app "sends" into a WAV file.

  (a) loopback   every message the app transmits is decoded back by OUR receiver - a separate run of
                 the RX-only simulator on the captured WAV - and compared with the text, and frame by
                 frame (hex) with what js8mk builds for the same text;
  (b) timing     tests/txchk.c on every captured WAV: start within +-20 ms of slot + 0.5 s, 101120
                 samples, continuous phase (ideal-signal fit), peak / clipping, spectrum;
  (c) tones      the 79 tones found in the audio of every frame against the independent Python model
                 tests/model.py (the 1000-random-frame run of the same check is `t7_tx vectors`);
  (d) safety     zero transmitted samples in every forbidden situation;
  (e) soak       2 hours of virtual time with a random owner and receive traffic, three loader variants.
  plus the two consecutive-slot strategies, the tap-grant path and TXTEST.

Every run gets its own new directory under build/tx/run-<time>/ (nothing is deleted).
Writes build/tx/checks.md + checks.json, exits 1 if a check failed.

  python3 -I -B tests/tx_e2e.py [--skip-soak] [--soak-hours H]
"""
import json
import os
import random
import re
import shutil
import subprocess
import sys
import time
import wave
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gestconv import conv, with_tx   # noqa: E402  (the gesture menu of 23 Nov 2026: owner scripts, tx=1)
import decrows as DR                 # noqa: E402  (the decoded rows of a run: tests/decrows.py)
from concurrent.futures import ThreadPoolExecutor

HOME = os.path.expanduser('~')
SRC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.environ.get('BUILD') or SRC + '/build'
SDK = os.environ.get('ICAP_SDK') or os.environ.get('SDK') or SRC + '/third_party/icap-sdk'
REF = os.environ.get('SRCREF') or (os.environ.get('JS8_REF') or SRC + '/third_party')
SD = BUILD + '/sd'
ROOT = BUILD + '/tx/run-' + time.strftime('%Y%m%d-%H%M%S')
SIM, SIMTX, MK, CHK, T7 = BUILD + '/js8sim', BUILD + '/js8simtx', BUILD + '/js8mk', BUILD + '/txchk', BUILD + '/t7_tx'
SIMASAN, T7ASAN = BUILD + '/san/js8simtx_asan', BUILD + '/san/t7_tx_asan'
# The tap-grant path (txpath=1: transmitting WITHOUT the beacon armed) is compiled only with -DJS8_QTX, which the radio
# builds do not have.  js8simtx is built as the radio is (no JS8_QTX); the runs with txpath=1 use these two instead.
SIMQTX, SIMQTXASAN = BUILD + '/js8simqtx', BUILD + '/san/js8simqtx_asan'
SAMPLES = (os.environ.get('SAMPLES') or SRC + '/recordings') + '/8k'
MIX = BUILD + '/e2e/mix.wav'           # 5 periods of receive traffic (tests/e2e.py)
FRAME = 101120

sys.path.insert(0, os.path.join(SRC, 'tests'))
import model as pymodel                # noqa: E402  (the independent Python model, read as a module)
import js8seg                          # noqa: E402  (reader of the app's segment files)

checks = []
leads = []                             # (ms between the app's first tx_write and the slot start, run) of every transmission
facts = {}                             # numbers for the report


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
    check('build js8sim (RX-only flavour of the shell, host)', r1.returncode == 0 and not r1.stdout.strip(), r1.stdout[:300] or 'no warnings')
    r2 = sh(cf + ['-ffp-contract=off', '-DDIGI_HOST', '-DJS8_TX', '-I' + SDK, '-I' + SDK + '/apps/digi-common', '-Ishell', '-Isrc', 'tests/js8sim.c'] + core +
            ['src/js8tx.c', 'src/js8pack.c', SDK + '/apps/digi-common/txsynth.c', '-lm', '-o', SIMTX], cwd=SRC)
    check('build js8simtx (RX+TX flavour: shell -DJS8_TX + src/js8tx.c + src/js8pack.c + SDK txsynth.c, host)', r2.returncode == 0 and not r2.stdout.strip(),
          r2.stdout[:300] or 'no warnings')
    r3 = sh(['cc', '-std=c99', '-D_POSIX_C_SOURCE=200809L', '-O2', '-Wall', '-Wextra', 'tests/js8mk.c', 'src/js8pack.c', 'src/js8msg.c', 'src/js8jsc.c', '-o', MK], cwd=SRC)
    r4 = sh(['cc', '-std=gnu99', '-O2', '-Wall', '-Wextra', 'tests/txchk.c', '-lm', '-o', CHK], cwd=SRC)
    r2q = sh(cf + ['-ffp-contract=off', '-DDIGI_HOST', '-DJS8_TX', '-DJS8_QTX', '-I' + SDK, '-I' + SDK + '/apps/digi-common', '-Ishell', '-Isrc', 'tests/js8sim.c'] + core +
             ['src/js8tx.c', 'src/js8pack.c', SDK + '/apps/digi-common/txsynth.c', '-lm', '-o', SIMQTX], cwd=SRC)
    check('build js8simqtx (the same with -DJS8_QTX: the tap-grant path, which the radio builds do not contain)', r2q.returncode == 0 and not r2q.stdout.strip(),
          r2q.stdout[:300] or 'no warnings')
    r5 = sh(['cc', '-std=gnu99', '-D_GNU_SOURCE', '-DJS8_QTX', '-O2', '-Wall', '-Wextra', '-ffp-contract=off', '-Isrc', '-I' + SDK + '/apps/digi-common', 'tests/t7_tx.c',
             'src/js8tx.c', 'src/js8pack.c', 'src/js8msg.c', 'src/js8jsc.c', 'src/js8enc.c', 'src/js8crc.c', 'src/js8_constants.c', SDK + '/apps/digi-common/txsynth.c', '-lm', '-o', T7], cwd=SRC)
    check('build js8mk, txchk, t7_tx', r3.returncode == 0 and r4.returncode == 0 and r5.returncode == 0 and not (r3.stdout + r4.stdout + r5.stdout).strip(),
          (r3.stdout + r4.stdout + r5.stdout)[:300] or 'no warnings')
    os.makedirs(BUILD + '/san', exist_ok=True)
    san = ['-g', '-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer']
    r6 = sh(['cc', '-std=gnu99', '-Wall', '-Wextra', '-fno-math-errno', '-ffp-contract=off'] + san + ['-DDIGI_HOST', '-DJS8_TX', '-I' + SDK, '-I' + SDK + '/apps/digi-common',
            '-Ishell', '-Isrc', 'tests/js8sim.c'] + core + ['src/js8tx.c', 'src/js8pack.c', SDK + '/apps/digi-common/txsynth.c', '-lm', '-o', SIMASAN], cwd=SRC)
    r6q = sh(['cc', '-std=gnu99', '-Wall', '-Wextra', '-fno-math-errno', '-ffp-contract=off'] + san + ['-DDIGI_HOST', '-DJS8_TX', '-DJS8_QTX', '-I' + SDK,
             '-I' + SDK + '/apps/digi-common', '-Ishell', '-Isrc', 'tests/js8sim.c'] + core + ['src/js8tx.c', 'src/js8pack.c', SDK + '/apps/digi-common/txsynth.c', '-lm', '-o', SIMQTXASAN], cwd=SRC)
    check('build js8simqtx with AddressSanitizer + UBSan', r6q.returncode == 0, r6q.stdout[:300])
    r7 = sh(['cc', '-std=gnu99', '-D_GNU_SOURCE', '-DJS8_QTX', '-Wall', '-Wextra', '-ffp-contract=off'] + san + ['-Isrc', '-I' + SDK + '/apps/digi-common', 'tests/t7_tx.c',
             'src/js8tx.c', 'src/js8pack.c', 'src/js8msg.c', 'src/js8jsc.c', 'src/js8enc.c', 'src/js8crc.c', 'src/js8_constants.c', SDK + '/apps/digi-common/txsynth.c', '-lm', '-o', T7ASAN], cwd=SRC)
    check('build js8simtx and t7_tx with AddressSanitizer + UBSan', r6.returncode == 0 and r7.returncode == 0, (r6.stdout + r7.stdout)[:300])
    return all(r.returncode == 0 for r in (r1, r2, r2q, r3, r4, r5, r6, r6q, r7))


def silence(path, seconds):
    w = wave.open(path, 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000)
    w.writeframes(b'\0\0' * (8000 * seconds))
    w.close()


class Run:
    """one simulator run of the RX+TX app (or of the RX-only app with rx=True)"""

    def __init__(self, name, owner='', cfg='', flags='on', wav=None, call='OK1BT', grid='JO70aa', tail=20, k='0', loops=1, rxonly=False, capture=True,
                 ownerfile=None, extra=(), binary=None, qtx_off=False):
        self.name = name
        self.dir = os.path.join(ROOT, name)
        os.makedirs(self.dir)
        cfg, owner = with_tx(cfg), conv(owner, cfg, grid)
        if cfg:
            open(os.path.join(self.dir, 'JS8.CFG'), 'w').write(cfg.replace('\n', '\r\n') + '\r\n')
        self.wav = os.path.join(ROOT, name + '.tx.wav')
        std_binary = binary is None
        if 'txpath=1' in cfg and not rxonly and not qtx_off:      # the tap-grant path needs the -DJS8_QTX build
            binary = {None: SIMQTX, SIMASAN: SIMQTXASAN}.get(binary, binary)
        cmd = ['nice', binary or (SIM if rxonly else SIMTX), '-d', self.dir, '-J', SD, '-k', k, '-T', str(tail), '-L', str(loops)]
        if flags:
            cmd += ['-X', flags]
            if capture:
                cmd += ['-w', self.wav]
        if ownerfile:
            cmd += ['-o', ownerfile]
        elif owner:
            cmd += ['-O', owner]
        if call is not None:
            cmd += ['-c', call]
        if grid is not None:
            cmd += ['-g', grid]
        cmd += list(extra) + [wav or QUIET]
        self.cmd = ' '.join("'%s'" % c if ' ' in c or ';' in c else c for c in cmd)
        t0 = time.time()
        r = sh(cmd)
        self.wall = time.time() - t0
        self.rc, self.out = r.returncode, r.stdout
        open(os.path.join(ROOT, name + '.out.txt'), 'w').write(self.cmd + '\n' + self.out)
        self.d, self.x, self.panel, self.stat = [], [], {}, {}
        for l in self.out.splitlines():
            if l.startswith('D '):
                self.d.append(l[2:])
            elif l.startswith('X '):
                m = re.match(r'^X (-?\d+) (.*)$', l)
                if m:
                    self.x.append((int(m.group(1)), m.group(2)))
                    ml = re.search(r'txstart .* lead=(-?[\d.]+)', l)
                    if ml and std_binary:
                        leads.append((float(ml.group(1)), name))
            elif re.match(r'^W\d', l):
                self.panel[int(l[1])] = l[3:]
            elif l.startswith('# '):
                for kk, v in re.findall(r'([a-z_0-9]+)=(-?[0-9.]+)', l):
                    self.stat[l.split()[1] + '.' + kk] = float(v) if '.' in v else int(v)

    def s(self, key, default=None):
        return self.stat.get(key, default)

    def log(self, name='JS8LOG.TXT'):
        """the lines of a log stream ('JS8LOG.TXT': the JS8Lnnnn.TXT segments, 'JS8MSG.TXT': JS8Mnnnn.TXT), oldest first;
        since the storage rework the logs are segment files written once (shell/js8app_store.h, tests/js8seg.py)"""
        return js8seg.lines(self.dir, name[:4])

    def tlines(self):
        """T records: frames the app says it sent: dict(utc, f0, k, n, samples, rc, hex, text)"""
        out = []
        for l in self.log():
            m = re.match(r'^T (\d{8}T\d{6}) (\d+) (\d+)/(\d+) (\d+) (-?\d+) ([0-9a-f]{22}) (.*)$', l)
            if m:
                out.append({'utc': m.group(1), 'f0': int(m.group(2)), 'k': int(m.group(3)), 'n': int(m.group(4)), 'samples': int(m.group(5)),
                            'rc': int(m.group(6)), 'hex': m.group(7), 'text': m.group(8)})
        return out

    def notes(self):
        return [l[9:] for l in self.log() if l.startswith('N ')]

    def rlines(self):
        out = []
        for l in self.log():
            m = re.match(r'^R (\d{8}T\d{6}) ([+-]\d\d) ([+-]\d+\.\d\d) (\d+\.\d) ([0-9a-f]{22}) (.*)$', l)
            if m:
                out.append({'utc': m.group(1), 'snr': int(m.group(2)), 'dt': float(m.group(3)), 'freq': float(m.group(4)), 'hex': m.group(5), 'text': m.group(6)})
        return out

    def mlines(self):
        out = []
        for l in self.log('JS8MSG.TXT'):
            m = re.match(r'^M (\d{8}T\d{6}) ([+-]\d\d) (\d+) (\d+) (\S+) (.*)$', l)
            if m:
                out.append({'utc': m.group(1), 'frames': int(m.group(4)), 'flags': m.group(5), 'text': m.group(6)})
        return out

    def xs(self, word):
        return [(t, e) for t, e in self.x if e.startswith(word)]

    def said(self, sub):
        return any(sub in l for l in self.d)

    def clean(self):
        """the run itself was healthy: exit 0, file rules, screen limits, no protocol error of the app against the TX model"""
        s = self.s
        ok = (self.rc == 0 and s('files.still_open') == 0 and s('files.nofd') == 0 and s('files.badname') == 0 and s('files.max_open', 99) <= 4 and
              s('decode_out.over40') == 0 and s('decode_out.win_over40') == 0 and s('decode_out.net_calls') == 0 and s('app.log_err') == 0 and
              s('txmodel.protocol_errors', 0) == 0 and s('txmodel.underruns', 0) == 0 and s('txmodel.late', 0) == 0 and s('txmodel.empty_keyed', 0) == 0 and
              s('txmodel.cut', 0) == 0 and s('txmodel.calls_after_stop', 0) == 0 and
              s('stream.in_tx_samples') == 0 and s('stream.in_tx_begins') == 0)
        return ok, ('rc=%d still_open=%s max_open=%s nofd=%s over40=%s win_over40=%s net=%s proto=%s underruns=%s late=%s empty_keyed=%s after_stop=%s' % (
            self.rc, s('files.still_open'), s('files.max_open'), s('files.nofd'), s('decode_out.over40'), s('decode_out.win_over40'), s('decode_out.net_calls'),
            s('txmodel.protocol_errors'), s('txmodel.underruns'), s('txmodel.late'), s('txmodel.empty_keyed'), s('txmodel.calls_after_stop')))


def receive(name, wavfile, call='Q7AB'):
    """a separate instance of OUR receiver (RX-only flavour) on the captured TX audio"""
    r = Run(name, flags='', wav=wavfile, call=call, grid=None, tail=30, rxonly=True)
    return r


def mk(call, grid, text, dict_=True):
    cmd = [MK, '-c', call, '-g', grid, '-i'] + (['-J', SD] if dict_ else []) + [text]
    r = sh(cmd)
    if r.returncode:
        return []
    return [l.split(' ')[0] for l in r.stdout.splitlines()]


def txchk(wavfile):
    r = sh([CHK, wavfile])
    out = []
    for l in r.stdout.splitlines():
        if l.startswith('F '):
            d = dict(re.findall(r'([a-z_0-9]+)=(\S+)', l))
            for kk in d:
                if kk != 'tones':
                    d[kk] = float(d[kk])
            out.append(d)
        elif l.startswith('S '):
            out.append({'short': l})
    return out


PM = None


def model_tones(hx):
    global PM
    if PM is None:
        PM = pymodel.Model(REF)
    v = int(hx, 16)
    bits = [(v >> (87 - i)) & 1 for i in range(87)]
    return ''.join(map(str, PM.tones(bits)))


quality = []        # every frame measured by txchk: dict


def audio_ok(name, run, nframes, consecutive=True, first_period=None):
    """(b) + (c) on one captured WAV"""
    fr = txchk(run.wav)
    t = run.tlines()
    ok_n = len(fr) == nframes and all('short' not in f for f in fr)
    if not check('%s: %d transmission(s) in the captured audio' % (name, nframes), ok_n, 'found %d' % len(fr)):
        return fr
    for f in fr:
        quality.append(f)
    worst_off = max(abs(f['off_ms']) for f in fr)
    check('%s: every frame starts within +-20 ms of slot + 0.5 s and is 101120 samples (12.64 s) long' % name,
          worst_off <= 20.0 and all(f['len'] == FRAME for f in fr), 'worst offset %.3f ms, lengths %s' % (worst_off, sorted(set(int(f['len']) for f in fr))))
    if consecutive and nframes > 1:
        per = [int(f['period']) for f in fr]
        check('%s: the frames are in consecutive 15 s periods' % name, per == list(range(per[0], per[0] + nframes)), 'periods %s' % per)
    if first_period is not None:
        check('%s: first frame in period %d' % (name, first_period), int(fr[0]['period']) == first_period, 'period %d' % int(fr[0]['period']))
    worst = max(f['model_err_lsb'] for f in fr)
    check('%s: continuous phase, no click - the audio equals the ideal CPFSK signal with the SDK ramp' % name, worst <= 4.0 and all(f['clip'] == 0 for f in fr),
          'max |audio - ideal| = %.1f LSB of peak %d, clipped samples %d' % (worst, int(fr[0]['peak']), int(sum(f['clip'] for f in fr))))
    tones_ok = len(t) == nframes and all(model_tones(t[i]['hex']) == fr[i]['tones'] for i in range(nframes))
    check('%s: the 79 tones in the audio of every frame equal the Python model for the logged frame' % name, tones_ok,
          '%d frames' % nframes if tones_ok else 'MISMATCH')
    return fr


OWNER_STD = '3 tap JS8 TX;5 tap %s;8 tap SEND?;9.5 arm'


def loopback(name, select, text, expect, call='OK1BT', cfg='', dict_=True, grid='JO70aa', g4='JO70', wav=None, owner=None, rxcall='Q7AB', flags='on', k='0',
             rxflags=None, tail=None):
    exp_hex = mk(call, g4, text, dict_)
    n = len(exp_hex)
    r = Run(name, owner or OWNER_STD % select, cfg=cfg, flags=flags, wav=wav, call=call, grid=grid, tail=tail or (25 + 15 * n), k=k)
    ok, det = r.clean()
    check('%s: simulator run clean (exit 0, file and screen limits, no protocol error, no underrun / late start)' % name, ok, det)
    t = r.tlines()
    check('%s: the app logged %d frame(s), identical to what js8mk builds for "%s"' % (name, n, text), n > 0 and [x['hex'] for x in t] == exp_hex and all(x['rc'] == 0 and x['samples'] == FRAME for x in t),
          'frames %s' % ' '.join(x['hex'] for x in t))
    audio_ok(name, r, n)
    rx = receive(name + '_rx', r.wav, rxcall)
    rh = [x['hex'] for x in rx.rlines()]
    check('%s: OUR receiver (separate instance) decodes every frame back, bit-identical' % name, rh == exp_hex and rx.s('decode_out.tx_calls') == 0,
          '%d of %d frames, dt %s s, snr %s dB' % (len([h for h in rh if h in exp_hex]), n, ' '.join('%+.2f' % x['dt'] for x in rx.rlines()),
                                                 ' '.join('%+d' % x['snr'] for x in rx.rlines())))
    ml = rx.mlines()
    check('%s: received message text is "%s"' % (name, expect), len(ml) == 1 and ml[0]['text'].rstrip() == expect and ml[0]['frames'] == n and 'x' not in ml[0]['flags'],
          ' | '.join('%s [%d fr, flags %s]' % (m['text'], m['frames'], m['flags']) for m in ml) or 'no message')
    facts.setdefault('loop', []).append({'name': name, 'text': text, 'frames': n, 'rx': ml[0]['text'] if ml else '', 'flags': ml[0]['flags'] if ml else '',
                                         'f0': t[0]['f0'] if t else 0, 'dt': [x['dt'] for x in rx.rlines()], 'snr': [x['snr'] for x in rx.rlines()]})
    return r, rx


def zero(name, r, begins=0, extra_ok=True, detail=''):
    """SAFETY: nothing was keyed"""
    ok, det = r.clean()
    fr = txchk(r.wav) if os.path.exists(r.wav) else []
    nz = sum(1 for _ in fr)
    good = (ok and r.s('txmodel.samples') == 0 and r.s('txmodel.transmissions') == 0 and nz == 0 and r.s('txmodel.begin') == begins and r.s('txmodel.qtx_ok', 0) == 0 and
            extra_ok)
    check('SAFETY %s: ZERO samples transmitted, tx_begin calls %d' % (name, begins), good,
          'samples=%s transmissions=%s tx_begin=%s grants=%s non-zero bursts in the WAV=%d %s | %s' % (r.s('txmodel.samples'), r.s('txmodel.transmissions'),
                                                                                                 r.s('txmodel.begin'), r.s('txmodel.begin_ok'), nz, detail, det))
    facts.setdefault('safety', []).append({'name': name, 'samples': r.s('txmodel.samples'), 'begin': r.s('txmodel.begin'), 'ok': bool(good), 'detail': detail})
    return good


L3 = 'q7ab msg meet me on 40m at 1900'            # directed + data frames + checksum
FREE = 'hello from the radio, test 1 2 3'              # 32 characters: "SEND? " + it is one 40-character DECODE row
FREE_LONG = 'hello from the radio, this is a test 1 2 73'   # 43: the most the radio's keyboard holds


def part_loopback():
    loopback('hb', 'TX> HB', 'HB JO70', 'OK1BT: @HB HEARTBEAT JO70')
    loopback('cq', 'TX> CQ', 'CQ CQ CQ JO70', 'OK1BT: @ALLCALL CQ CQ CQ JO70')
    loopback('hb_grid_cfg', 'TX> HB', 'HB JN89', 'OK1BT: @HB HEARTBEAT JN89', cfg='loc=jn89ab', grid=None, g4='JN89')
    loopback('freetext_jsc', 'TX> HELLO', FREE, 'OK1BT: ' + FREE.upper(), cfg='tx1=' + FREE)
    # the keyboard holds 43 characters: a text that fills it goes out whole (what the owner saw is what is sent)
    r = loopback('freetext_long', 'TX> HELLO', FREE_LONG, 'OK1BT: ' + FREE_LONG.upper(), cfg='tx1=' + FREE_LONG, tail=75)[0]
    r = Run('freetext_cut', '3 taprow 0;4 kbd ' + FREE_LONG + ' 73 MORE;9.5 arm', tail=75)
    check('a text typed longer than the keyboard holds: the keyboard keeps 43 characters, and exactly those 43 go out (logged text "%s")' % (r.tlines()[0]['text'] if r.tlines() else '?'),
          r.tlines() and r.tlines()[0]['text'] == FREE_LONG.upper() and all(len(l) <= 40 for l in r.d), 'rows %d, longest %d' % (len(r.d), max(len(l) for l in r.d)))
    loopback('directed_cmd', 'TX> Q7AB', 'q7ab snr?', 'OK1BT: Q7AB SNR?', cfg='tx1=q7ab snr?')
    loopback('directed_msg_huffman', 'TX> Q7AB', L3, 'OK1BT: ' + L3.upper(), cfg='txjsc=0\ntx1=' + L3, dict_=False)
    loopback('directed_msg_jsc', 'TX> Q7AB', L3, 'OK1BT: ' + L3.upper(), cfg='tx1=' + L3)
    loopback('group_text', 'TX> @ALLCALL', '@allcall qrv on 40m tonight', 'OK1BT: @ALLCALL QRV ON 40M TONIGHT', cfg='tx1=@allcall qrv on 40m tonight')
    loopback('compound_hb', 'TX> HB', 'HB JO70', '9A/OK1BT: @HB HEARTBEAT JO70', call='9A/OK1BT')
    loopback('compound_directed', 'TX> Q7AB', 'q7ab snr -05', '9A/OK1BT: Q7AB SNR -05', call='9A/OK1BT', cfg='tx1=q7ab snr -05')
    loopback('compound_to', 'TX> Q7AB/QRP', 'q7ab/qrp ack', 'OK1BT: Q7AB/QRP ACK', cfg='tx1=q7ab/qrp ack', rxcall='Q7AB/QRP')
    # reply shortcut on a tapped decoded row (receive traffic: mix.wav has Q7AB's heartbeat at 1100 Hz, -11 dB)
    r, rx = loopback('reply_snr', '', 'Q7AB SNR -11', 'OK1BT: Q7AB SNR -11', wav=MIX, owner='20 tap Q7AB: @HB;25 kbd Q7AB SNR -11;27 arm', tail=30)
    check('reply_snr: the tap on the decoded row of Q7AB opened the radio\'s keyboard with "Q7AB " (the owner wrote the rest)',
          any(e.startswith('kbd open "Q7AB"') for t, e in r.x), [e for t, e in r.x if 'kbd open' in e])
    check('reply_snr: the addressed station (receiver run as Q7AB) gets it as a message to itself', any('>' in m['flags'] for m in rx.mlines()),
          ' '.join(m['flags'] for m in rx.mlines()))
    t = r.tlines()
    heard = sorted(set(int(round(x['freq'])) for x in r.rlines() if x['utc'].endswith('T120000')))
    later = sorted(set(int(round(x['freq'])) for x in r.rlines() if not x['utc'].endswith('T120000')))
    check('reply_snr: automatic TX offset is in 500..2500 Hz and at least 100 Hz away from every station decoded before the transmission was started',
          bool(t) and 500 <= t[0]['f0'] <= 2450 and all(abs(t[0]['f0'] - h) >= 100 for h in heard),
          'TX at %s Hz, stations decoded before: %s Hz (decoded only later: %s Hz)' % (t[0]['f0'] if t else '?', heard, later))
    facts['offset'] = {'tx': t[0]['f0'] if t else 0, 'before': heard, 'later': later}
    check('reply_snr: RX/TX coexistence - the period before the transmission was decoded, the transmitted period was skipped without an error note, RX resumed',
          any(x['utc'].endswith('T120015') for x in r.rlines()) and not any(x['utc'].endswith('T120030') for x in r.rlines()) and
          any(x['utc'].endswith('T120045') for x in r.rlines()) and r.s('app.skipped') == 0 and r.s('audio.app_gaps') == 0 and not any('gap' in n or 'skipped' in n for n in r.notes()),
          'frames per period: %s; notes: %s' % (' '.join('%s:%d' % (p, sum(1 for x in r.rlines() if x['utc'].endswith(p))) for p in ('T120000', 'T120015', 'T120030', 'T120045', 'T120100')),
                                                '; '.join(r.notes())))
    r2 = loopback('reply_hb_ack', '', 'Q7AB HEARTBEAT SNR -11', 'OK1BT: Q7AB HEARTBEAT SNR -11', wav=MIX,
                  owner='20 tap Q7AB: @HB;25 kbd Q7AB HEARTBEAT SNR -11;27 arm', tail=30)[0]
    # fixed offset from the config
    r3 = loopback('offset_cfg', 'TX> HB', 'HB JO70', 'OK1BT: @HB HEARTBEAT JO70', cfg='txf=2125')[0]
    check('offset_cfg: txf=2125 -> the frame is at 2125 Hz', [x['f0'] for x in r3.tlines()] == [2125] and all(f['f0'] == 2125 for f in txchk(r3.wav)))


def part_timing():
    for nm, kw in (('timing_cpu30', dict(k='30')), ('timing_skew_plus400', dict(flags='on,skew=400')), ('timing_skew_minus400', dict(flags='on,skew=-400')),
                   ('timing_rxgap', dict(flags='on,rxgap')), ('timing_traffic', dict(wav=MIX, tail=70))):
        loopback(nm, 'TX> HELLO', FREE, 'OK1BT: ' + FREE.upper(), cfg='tx1=' + FREE, **kw)
    # a slow processor (deterministic model: 20 ms per abort poll of the decoder): the decode of the period before is still
    # running when the transmission is due - it must be left at once, its frames kept, the frame sent on time
    # (since the fast receiver: the first waterfall is built during the capture and budget= ends a long decode, so a decode
    # no longer lasts into the next slot by itself.  Here: stream=0 and budget=0 - the decode works as before the speed-up -
    # and 40 ms per abort poll; the variant below keeps the stream and makes the processor slower still.)
    # (the menu is worked before the decode of 12:00:00 begins: building a text reads the dictionary, which costs this model 40 ms per read)
    r = Run('decode_cut', '3 taprow 0;4 kbd HB JO70;23 arm', wav=MIX, tail=40, extra=('-q', '40'), cfg='budget=0\nstream=0')
    ok, det = r.clean()
    fr = audio_ok('decode cut for TX', r, 1, first_period=2)
    check('decode cut for TX: a decode still running when the slot comes is left ("decode cut for TX"), the frames it had found are kept, the receiver goes on afterwards',
          ok and any('decode cut for TX' in n for n in r.notes()) and sum(1 for x in r.rlines() if x['utc'].endswith('T120000')) == 3 and
          any(x['utc'].endswith('T120045') for x in r.rlines()), 'notes: %s; decode ms max %s | %s' % ('; '.join(r.notes()), r.s('decode.max'), det))
    # armed first, taps afterwards (possible only if the loader delivers taps while the beacon is armed)
    loopback('armed_first', 'TX> HB', 'HB JO70', 'OK1BT: @HB HEARTBEAT JO70', flags='on,tapsarmed', owner='2 arm;3 tap JS8 TX;5 tap TX> HB;8 tap SEND?')


def part_strategies():
    n3 = len(mk('OK1BT', 'JO70', L3))
    # B: one frame per arming
    own = OWNER_STD % 'TX> Q7AB' + ';32 disarm;35 arm;62 disarm;65 arm;92 disarm;95 arm'
    r = Run('strategy_b', own, cfg='txstrat=B\ntx1=' + L3, tail=120)
    ok, det = r.clean()
    check('strategy B: run clean', ok, det)
    # (review 24 Nov 2026: a disarm drops the held rest - confirm, arm, disarm = nothing further; under B only frame 1 goes)
    fr = audio_ok('strategy B', r, 1, consecutive=False)
    per = [int(f.get('period', -1)) for f in fr]
    check('strategy B: ONE frame under the first arming, the held rest is dropped by the disarm - %d armings -> 1 frame, tx_begin calls 1' % n3,
          per == [1] and r.s('txmodel.begin') == 1 and any('disarmed, waiting messages dropped' in n for n in r.notes()), 'periods %s, tx_begin calls %s' % (per, r.s('txmodel.begin')))
    check('strategy B: after the frame the app shows "TX B: held, red frame off drops 2/%d"' % n3, r.said('TX B: held, red frame off drops 2/%d' % n3),
          '; '.join(l for l in r.d if 'TX B' in l))
    rx = receive('strategy_b_rx', r.wav)
    exp = mk('OK1BT', 'JO70', L3)
    facts['strategy_b_rx'] = ' | '.join('%s [%d fr, %s]' % (m['text'], m['frames'], m['flags']) for m in rx.mlines())
    check('strategy B: the one frame decodes back bit-identical (frame 1 of the message)', [x['hex'] for x in rx.rlines()] == exp[:1], '%d frames' % len(rx.rlines()))
    # B without a new arming: only one frame
    r = Run('strategy_b_one_arming', OWNER_STD % 'TX> Q7AB', cfg='txstrat=B\ntx1=' + L3, tail=150)
    fr = txchk(r.wav)
    check('strategy B, armed once and left armed: ONE frame only, the rest expires (txttl) - tx_begin calls 1', len(fr) == 1 and r.s('txmodel.begin') == 1 and
          any('TX expired' in n for n in r.notes()), 'frames %d, tx_begin %s, notes: %s' % (len(fr), r.s('txmodel.begin'), '; '.join(r.notes())))
    # A against a loader that refuses consecutive slots
    own = OWNER_STD % 'TX> Q7AB' + ';32 disarm;35 arm;62 disarm;65 arm'
    r = Run('strategy_a_loader_once', own, cfg='tx1=' + L3, flags='on,once', tail=120)
    ok, det = r.clean()
    fr = txchk(r.wav)
    perm = len(r.xs('begin PERM'))
    check('strategy A on a loader that refuses the next slot (PERM): no retry storm - 1 frame, tx_begin calls = 1 ok + 1 refused; the disarm drops the held rest (S1 24 Nov 2026)',
          ok and len(fr) == 1 and r.s('txmodel.begin') == 2 and perm == 1 and any('disarmed, waiting messages dropped' in n for n in r.notes()),
          'frames %d, tx_begin %s, PERM %d | %s' % (len(fr), r.s('txmodel.begin'), perm, det))
    check('strategy A on such a loader: the message waits ("TX refused: part k/n - red frame again", also a DECODE row) and does NOT continue after a disarm + arming',
          any('TX refused: part 2/' in n for n in r.notes()) and r.said('TX refused: part 2/') and r.said(' - red frame again') and r.said('TX: red frame off - message dropped'), '; '.join(r.notes()))
    # A with the loader's own session limit
    r = Run('strategy_a_loader_cap2', OWNER_STD % 'TX> Q7AB', cfg='tx1=' + L3, flags='on,cap=2', tail=150)
    fr = txchk(r.wav)
    check('strategy A, loader session limit of 2 transmissions: 2 frames, one refused tx_begin, then nothing (no storm)', len(fr) == 2 and r.s('txmodel.begin') == 3,
          'frames %d, tx_begin %s' % (len(fr), r.s('txmodel.begin')))
    # the tap credit (@QTX, SDK QSO mode): ENT on ONE frame goes out on the credit of the tap that opened the keyboard - no beacon
    r = Run('tap_grant', '3 taprow 0;4 kbd HB JO70', cfg='txtap=1', tail=40)
    ok, det = r.clean()
    fr = audio_ok('tap grant', r, 1)
    check('tap grant (txtap=1): one frame, the beacon never armed: sent on the loader\'s grant - tx_begin 0, @QTX asked once and granted',
          ok and r.s('txmodel.begin') == 0 and r.s('txmodel.qtx') == 1 and r.s('txmodel.qtx_ok') == 1 and len(r.xs('owner arm')) == 0 and r.said('on air at the next slot (tap)'),
          'tx_begin %s, @QTX asked %s, granted %s | %s' % (r.s('txmodel.begin'), r.s('txmodel.qtx'), r.s('txmodel.qtx_ok'), det))
    rx = receive('tap_grant_rx', r.wav)
    check('tap grant: the frame decodes back as "OK1BT: @HB HEARTBEAT JO70"', [m['text'].rstrip() for m in rx.mlines()] == ['OK1BT: @HB HEARTBEAT JO70'])
    r = Run('tap_grant_multi', '3 taprow 0;4 kbd ' + L3, cfg='txtap=1', tail=60)
    zero('tap credit: a message of %d frames is never sent on it - queued, "%d parts: MODE > hold FT8 (red), 2 min", nobody arms' % (n3, n3), r, 0,
         r.s('txmodel.qtx') == 0 and r.said('%d parts: MODE > hold FT8 (red), 2 min' % n3), '@QTX asked %s' % r.s('txmodel.qtx'))
    r = Run('tap_grant_refused', '2 abort;3 taprow 0;4 kbd HB JO70', cfg='txtap=1', tail=40)
    zero('owner abort latched (S2 24 Nov 2026): the tap opens no keyboard ("TX: aborted - restart the app to send"), the tap credit is never asked', r, 0,
         r.s('txmodel.qtx') == 0 and r.said('TX: aborted - restart the app to send') and not r.xs('kbd open'), '@QTX asked %s' % r.s('txmodel.qtx'))
    r = Run('tap_grant_default_off', '3 tap JS8 TX;5 tap TX> HB;8 tap SEND?', tail=40)
    zero('txtap=0: a confirmed message without arming is not sent through @QTX', r, 0, r.s('txmodel.qtx') == 0, '@QTX asked %s times' % r.s('txmodel.qtx'))
    # TXTEST
    own = '3 tap JS8 TX;5 tap TX> HB;8 tap SEND?;9.5 arm;32 disarm;34 tap TX 1500;36 tap TX> HB;39 tap SEND?;41 arm'
    r = Run('txtest', own, cfg='txtest=1\ntx1=' + L3, tail=90)
    ok, det = r.clean()
    fr = audio_ok('TXTEST', r, 1)
    check('TXTEST: exactly ONE heartbeat frame for the whole run; the second request is refused', ok and len(fr) == 1 and r.s('txmodel.begin') == 1 and
          r.said('TX refused: TXTEST') and [x['hex'] for x in r.tlines()] == mk('OK1BT', 'JO70', 'HB JO70'),
          'frames %d, tx_begin %s, panel: %s' % (len(fr), r.s('txmodel.begin'), r.panel.get(1)))
    rx = receive('txtest_rx', r.wav)
    check('TXTEST: the frame decodes back as "OK1BT: @HB HEARTBEAT JO70"', [m['text'].rstrip() for m in rx.mlines()] == ['OK1BT: @HB HEARTBEAT JO70'])
    r = Run('txtest_no_tap', '2 arm', cfg='txtest=1', tail=120, flags='on,tapsarmed')
    zero('TXTEST armed but nobody tapped', r)


def part_safety():
    sel = '3 tap JS8 TX;5 tap TX> HB;8 tap SEND?'
    r = Run('s_not_armed', sel, tail=150)
    zero('not armed (request confirmed, beacon never armed, 150 s)', r, 0, (r.said('TX queued') or r.said('MODE > hold FT8 (red), 2 min')) and any('TX expired' in n for n in r.notes()), 'notes: ' + '; '.join(r.notes()))
    # every gesture but the confirm: 1x, 3x (cancel with nothing queued), items selected and left with 3x, a confirm gesture begun too early
    # (0.3 s after the SEND? screen appeared), taps on decoded rows and on rows that read like the old menu (only their number counts)
    # every way to open and leave the keyboard without an ENT on a text: EXIT, an empty ENT (the cancel), taps while it is open,
    # rows that read like the old menu, several taps in a row
    own = ('2 arm;18 tap Q7AB: @HB;20 kbdexit;22 taprow 0;24 kbd;26 tapraw SEND? HB JO70;27 tapraw TX> EVIL TEXT;29 kbdexit;31 g 3;33 kbdexit;'
           '35 tap Q2XYZ:;37 g 2;39 kbdexit;41 tap TESTING 1 2 3;43 kbd')
    r = Run('s_no_request', own, wav=MIX, flags='on,tapsarmed', tail=120, cfg='tx1=' + L3)
    opens = [e for t, e in r.x if e.startswith('kbd open')]
    zero('armed, the keyboard opened 6 times and left without an ENT on a text (EXIT, empty ENT, forged rows, taps while open) - never a request; 195 s armed', r, 0,
         r.s('owner.tap_miss') == 0 and not r.said('TX queued') and not r.said('MODE > hold FT8') and not r.said('replaced') and not r.said('SENT') and len(opens) == 6 and r.s('owner.taps', 0) >= 10,
         'taps delivered %s, keyboard opened %d times' % (r.s('owner.taps'), len(opens)))
    r = Run('s_armed_idle', '1 arm', wav=MIX, loops=4, tail=30)
    zero('armed with an empty queue for 330 s of receive traffic', r, 0, r.s('app.frames', 0) > 20, 'frames received meanwhile: %s' % r.s('app.frames'))
    # owner abort
    own = OWNER_STD % 'TX> Q7AB' + ';20.5 abort;40 arm'
    r = Run('s_abort_midframe', own, cfg='tx1=' + L3, tail=150)
    ok, det = r.clean()
    st = r.xs('txstart')
    after = [t for t, e in r.x if e.startswith('txstart') and t > 20500]
    check('SAFETY owner abort in the middle of frame 1 of 3 (5 s after its start): the audio ends at the abort, the queue is cleared, NOTHING is sent after re-arming',
          ok and len(st) == 1 and not after and r.s('txmodel.samples') <= 5 * 8000 + 8 and r.s('txmodel.begin') == 1 and r.s('txmodel.write_rejected') <= 1 and
          any('owner abort' in n for n in r.notes()),
          'samples %s (abort 5.000 s after the start), transmissions started %d, after the abort %d, tx_begin %s, rejected writes %s, notes: %s | %s' % (
              r.s('txmodel.samples'), len(st), len(after), r.s('txmodel.begin'), r.s('txmodel.write_rejected'), '; '.join(r.notes()), det))
    facts['abort_samples'] = r.s('txmodel.samples')
    r = Run('s_abort_before_slot', OWNER_STD % 'TX> Q7AB' + ';14.8 abort;40 arm', cfg='tx1=' + L3, tail=150)
    zero('after owner abort (between tx_begin and the slot start; re-armed later, 3-frame message)', r, 1, r.s('txmodel.cancelled') == 1, 'cancelled grants %s' % r.s('txmodel.cancelled'))
    r = Run('s_abort_before_begin', OWNER_STD % 'TX> Q7AB' + ';11 abort;40 arm', cfg='tx1=' + L3, tail=150)
    zero('after owner abort (message queued and armed, abort before tx_begin; re-armed later)', r, 0, any('owner abort' in n for n in r.notes()))
    # EXIT
    r = Run('s_exit_queued', OWNER_STD % 'TX> Q7AB' + ';12 stop', cfg='tx1=' + L3, tail=60)
    zero('after EXIT (message queued and armed, EXIT before tx_begin)', r)
    r = Run('s_exit_before_slot', OWNER_STD % 'TX> Q7AB' + ';14.6 stop', cfg='tx1=' + L3, tail=60)
    zero('after EXIT (EXIT between tx_begin and the slot start)', r, 1)
    r = Run('s_exit_midframe', OWNER_STD % 'TX> Q7AB' + ';20 stop', cfg='tx1=' + L3, tail=60)
    ok, det = r.clean()
    leave = r.s('sim.virtual') * 1000.0 - 1500.0 - 20000.0
    check('SAFETY EXIT in the middle of a frame: no tx call at all after the stop request, the audio ends there, the app leaves within the loader\'s 3 s',
          ok and r.s('txmodel.calls_after_stop') == 0 and r.s('txmodel.samples') <= int(4.5 * 8000) + 8 and r.s('txmodel.transmissions') == 1 and 0 <= leave <= 3000,
          'samples %s (stop 4.5 s after the start), tx calls after stop %s, app returned %.0f ms after the stop | %s' % (
              r.s('txmodel.samples'), r.s('txmodel.calls_after_stop'), leave, det))
    facts['exit_ms'] = leave
    # caps
    own = '3 tap JS8 TX;5 tap TX> HB;8 tap SEND?;10 tap TX> HB;13 tap SEND?;15 tap TX> HB;18 tap SEND?;20 tap TX> HB;23 tap SEND?;26 arm'
    r = Run('s_queue_cap', own, tail=120)
    ok, det = r.clean()
    fr = txchk(r.wav)
    check('SAFETY ONE waiting message (review 24 Nov 2026): 4 confirmed messages, each ENT replaces the waiting one ("replaced: MODE > hold FT8 (red), 1 part" x3) - exactly 1 frame goes out',
          ok and len(fr) == 1 and r.d.count('replaced: MODE > hold FT8 (red), 1 part') == 3 and r.s('txmodel.begin') == 1 and r.s('txmodel.samples') == FRAME,
          'frames %d, samples %s, tx_begin %s' % (len(fr), r.s('txmodel.samples'), r.s('txmodel.begin')))
    r = Run('s_queue_cap_zero', own.replace(';26 arm', ''), tail=30)
    zero('4 messages confirmed (each replacing the last), not armed', r, 0, r.d.count('replaced: MODE > hold FT8 (red), 1 part') == 3)
    own = ('3 tap JS8 TX;5 tap TX> HB;8 tap SEND?;9 arm;30 disarm;33 tap TX> HB;35 tap SEND?;37 arm;60 disarm;62 tap TX 1500;64 tap TX> HB;67 tap SEND?;'
           '69 tap TX> CQ;72 tap SEND?;74 tap TX> HB;76 tap SEND?;78 arm')
    r = Run('s_session_cap', own, cfg='txsess=2', tail=120)
    ok, det = r.clean()
    fr = txchk(r.wav)
    check('SAFETY session cap (txsess=2): 2 frames, every later request is refused ("session cap reached"), zero samples after the cap', ok and len(fr) == 2 and
          r.s('txmodel.begin') == 2 and r.d.count('TX refused: session cap reached') == 3 and not [t for t, e in r.x if e.startswith('txstart') and t > 60000],
          'frames %d, tx_begin %s, refusals %d' % (len(fr), r.s('txmodel.begin'), r.d.count('TX refused: session cap reached')))
    r = Run('s_session_cap_multi', OWNER_STD % 'TX> Q7AB', cfg='txsess=2\ntx1=' + L3, tail=60)
    zero('session cap exceeded (3-frame message with txsess=2 is refused as a whole)', r, 0, r.said('TX refused: session cap'))
    r = Run('s_frame_cap', OWNER_STD % 'TX> Q7AB', cfg='txmaxf=2\ntx1=' + L3, tail=60)
    zero('frames-per-action cap exceeded (3-frame message with txmaxf=2)', r, 0, r.said('TX refused: too many parts'))
    # malformed
    r = Run('s_malformed_char', OWNER_STD % 'TX> HELLO', cfg='txjsc=0\ntx1=hello: world', tail=60)
    zero('malformed message (":" cannot be encoded without the dictionary)', r, 0, r.said('TX refused: text not encodable'))
    r = Run('s_malformed_tilde', OWNER_STD % 'TX> ~', cfg='txjsc=0\ntx1=~~~ ^^^', tail=60)
    zero('malformed message (characters no frame can carry: "~~~ ^^^")', r, 0, r.said('TX refused: text not encodable'))
    # callsign / locator
    r = Run('s_no_call', OWNER_STD % 'TX> HB', call=None, tail=60)
    zero('missing callsign (no @MYCALL, no call=)', r, 0, r.said('TX refused: set MY call'))
    r = Run('s_bad_call', OWNER_STD % 'TX> HB', call='NOCALL', tail=60)
    zero('invalid callsign ("NOCALL")', r, 0, r.said('TX refused: set MY call'))
    r = Run('s_group_call', OWNER_STD % 'TX> HB', call='@ALLCALL', tail=60)
    zero('own call is a group name ("@ALLCALL")', r, 0, r.said('TX refused: set MY call'))
    r = Run('s_no_grid', OWNER_STD % 'TX> HB' + ';12 tapraw TX> HB JO70;15 tapraw SEND? HB JO70', grid=None, tail=60)
    zero('missing locator (no @MYLOC, no loc=): ENT is refused and the row says why', r, 0, r.said('set the locator'))
    r = Run('s_bad_grid', OWNER_STD % 'TX> HB', grid='XX99zz', tail=60)
    zero('invalid locator ("XX99zz")', r, 0, r.said('set the locator'))
    # loader states
    r = Run('s_gate_busy', '1 gate 7;' + OWNER_STD % 'TX> HB', tail=120)
    zero('loader refuses with BUSY (gate 7 = not USB-D / DATA MOD): one try per slot, 4 tries, then the message is dropped', r, 4,
         any('gave up' in n for n in r.notes()) and any('DATA MOD' in n for n in r.notes()), 'notes: ' + '; '.join(r.notes()))
    b = [t for t, e in r.xs('begin BUSY')]
    check('SAFETY BUSY: the 4 tries are one slot apart (no retry storm)', len(b) == 4 and all(14000 <= b[i + 1] - b[i] <= 16000 for i in range(3)), 'at %s ms' % b)
    r = Run('s_clock_unlocked', '1 clk 0;' + OWNER_STD % 'TX> HB', tail=120)
    zero('loader clock not locked (BUSY)', r, 4, any('set the radio clock' in n for n in r.notes()))
    r = Run('s_no_tx_file', OWNER_STD % 'TX> HB', flags='on,notx', tail=60)
    zero('loader without @TX (the arming cannot be seen)', r, 0, 'no @TX' in r.panel.get(1, ''), 'panel: %s' % r.panel.get(1))
    r = Run('s_ttl', '3 tap JS8 TX;5 tap TX> HB;8 tap SEND?;140 arm', tail=180)
    zero('request older than txttl (armed 132 s after the confirmation)', r, 0, any('TX expired' in n for n in r.notes()))
    r = Run('s_tx_off', OWNER_STD % 'TX> HB', cfg='tx=0', tail=60)
    zero('tx=0 in JS8.CFG', r, 0, 'off' in r.panel.get(1, ''), 'panel: %s' % r.panel.get(1))
    r = Run('s_noqso', OWNER_STD % 'TX> HB', flags='on,noqso', tail=60)
    zero('loader without @QSO (ICAP_E_NAME): no menu at all', r, 0, not any(l.startswith(('MENU', 'TX', 'SEND')) and l != 'TX: red frame on, nothing to send' for l in r.d) and r.s('owner.taps') == 0)
    r = Run('s_taps_while_armed', '2 arm;3 tap JS8 TX;5 tap TX> HB;8 tap SEND?', tail=60)
    zero('taps are not delivered while the beacon is armed (loader default per SDK README)', r, 0, r.s('owner.taps') == 0 and r.s('owner.taps_not_delivered') >= 1, 'taps delivered %s' % r.s('owner.taps'))
    # the RX-only flavour never calls TX, whatever the owner does
    r = Run('s_rx_flavour', '1 arm;3 tap Q7AB: @HB;5 tap TX> HB;8 tap SEND?', wav=MIX, flags='on,tapsarmed', rxonly=True, tail=30)
    check('SAFETY RX-only flavour (JS8.APP) with the beacon armed and taps: no tx call of any kind, no keyboard', r.rc == 0 and r.s('decode_out.tx_calls') == 0 and
          r.s('txmodel.begin') == 0 and not any(l.startswith(('TX', 'JS8 TX')) for l in r.d) and not r.xs('kbd open'), 'tx_calls=%s' % r.s('decode_out.tx_calls'))


# ---- soak -----------------------------------------------------------------------------------------------------------
def soak_events(seed, seconds, variant):
    """the owner of the soak with the keyboard control (24 Nov 2026): taps open the keyboard, ENT with a text / empty, EXIT; the simulator
    runs the events one after the other, so the times are "not before"."""
    rng = random.Random(seed)
    texts = ['HB JO70', 'CQ CQ CQ JO70', L3, FREE, 'Q7AB SNR -11', 'HELLO']
    ev, t = [], 2.0
    while t < seconds - 40:
        t += rng.choice((0.4, 0.8, 1.5, 2.0, 3.0, 5.0, 9.0, 14.0, 22.0))
        x = rng.random()
        if rng.random() < 0.22:                      # a purposeful owner: tap, type, ENT, arm
            if variant == 'qtx' or rng.random() < 0.3:
                ev.append((t, 'disarm'))
            ev.append((t + 0.5, 'taprow %d' % rng.randrange(0, 6)))
            ev.append((t + 1.0, rng.choice(('kbd ' + rng.choice(texts),) * 4 + ('kbdexit', 'kbd'))))
            t += 7.0
            if variant != 'qtx' and rng.random() < 0.8:
                ev.append((t, 'arm'))
            continue
        if variant == 'qtx' and 0.66 <= x < 0.78 and rng.random() < 0.8:
            x = 0.80                                 # the tap credit works without the beacon: arm it rarely
        if x < 0.15:
            ev.append((t, 'taprow %d' % rng.randrange(0, 12)))
        elif x < 0.40:
            ev.append((t, 'g %d' % rng.choice((1, 1, 2, 3))))
        elif x < 0.60:
            ev.append((t, rng.choice(('kbd ' + rng.choice(texts), 'kbdexit', 'kbd'))))
        elif x < 0.66:
            ev.append((t, 'cq %d' % rng.choice((0, 1))) if variant != 'qtx' else (t, 'kbdexit'))
        elif x < 0.78:
            ev.append((t, 'arm'))
        elif x < 0.86:
            ev.append((t, 'disarm'))
        elif x < 0.89:
            ev.append((t, 'abort'))
        elif x < 0.91:
            ev.append((t, 'gate %d' % rng.choice((7, 11, 0, 0))))
        elif x < 0.92:
            ev.append((t, 'clk %d' % rng.choice((0, 1, 1))))
        elif x < 0.95:
            ev.append((t, 'tapraw ' + rng.choice(('SEND? HB JO70', 'TX> EVIL', 'TX? Q7AB SNR -11', '                SEND? CQ CQ CQ JO70'))))
        else:
            ev.append((t, 'gate 0'))
            ev.append((t + 0.1, 'clk 1'))
    return ev


def soak(variant, seconds, seed, binary=None, tag=''):
    flags = {'A': 'on,tapsarmed,rxgap', 'B': 'on,tapsarmed,once', 'qtx': 'on'}[variant]
    cfg = {'A': '', 'B': 'txstrat=B\n', 'qtx': 'txtap=1\n'}[variant] + 'txsess=500\ntx1=%s\ntx2=%s\ntx3=@allcall qrv on 40m tonight' % (L3, FREE)
    ev = soak_events(seed, seconds, variant)
    os.makedirs(ROOT, exist_ok=True)
    of = os.path.join(ROOT, 'soak_%s%s.owner.txt' % (variant, tag))
    open(of, 'w').write(''.join('%.2f %s\n' % e for e in ev))
    loops = int(seconds // 75)
    t0 = time.time()
    r = Run('soak_' + variant + tag, cfg=cfg, flags=flags, wav=MIX, loops=loops, tail=30, ownerfile=of, capture=(variant == 'A' and not tag), binary=binary)
    return variant, r, len(ev), time.time() - t0


def soak_check(variant, r, nev, wall, seconds):
    nm = 'SOAK %s' % {'A': 'strategy A (taps while armed, no RX audio during TX)', 'B': 'strategy B (loader refuses consecutive slots)',
                      'qtx': 'tap credit (txtap=1, loader default: no taps while armed)'}[variant]
    ok, det = r.clean()
    check('%s: %.1f h of virtual time, %d owner events - the app ran to the end (no deadlock), exit 0, no file handle left open (max %s open), no protocol error' % (
        nm, r.s('sim.virtual', 0) / 3600.0, nev, r.s('files.max_open')), ok and r.s('sim.virtual', 0) >= seconds and r.s('owner.done') == nev, det + ' wall %.0f s' % wall)
    check('%s: stop request polled at least every 500 ms throughout (longest gap %s ms)' % (nm, r.s('stop.poll_gap_max_ms')), r.s('stop.poll_gap_max_ms', 9999) <= 500)
    # invariants from the simulator's own record of the owner and of the TX model (not from what the app says)
    # a confirmation = ENT on the keyboard with a text ("owner kbd ENT \"<text>\"", counted whether or not the text was accepted -
    # counting them is the conservative side); a CQ arming (cq 0 -> 1) stands for its one CQ frame (src/js8tx.h rule 8)
    armed, cq, cqarm, confirms, bad, starts, full, cut, last_confirm, need_confirm, grant, cqgrant = False, False, 0, 0, [], 0, 0, 0, None, True, False, False
    worst = 0.0
    for t, e in r.x:
        if e == 'owner arm':
            armed, cq = True, False
        elif e in ('owner disarm',):
            armed = False
        elif e == 'owner cq 1':
            if not cq:
                cqarm += 1
            cq, armed = True, False
        elif e == 'owner cq 0':
            cq = False
        elif e == 'owner abort':
            armed = cq = False
            need_confirm = True
        elif e.startswith('owner kbd ENT "') and e[len('owner kbd ENT "'):].strip()[:1] not in ('"', ''):   # ENT on a text
            confirms += 1
            need_confirm = False
        elif e.startswith('qtx grant'):
            grant = True
        elif e.startswith('qtxc grant'):
            cqgrant = True
        elif e.startswith('txstart'):
            starts += 1
            m = re.search(r'slot=([\d.]+) first=([\d.]+)', e)
            slot, first = float(m.group(1)), float(m.group(2))
            worst = max(worst, abs(first - slot))
            if abs(slot % 15000.0 - 500.0) > 0.01:
                bad.append('%d: slot %.1f is not on the slot grid' % (t, slot))
            if abs(first - slot) > 20.0:
                bad.append('%d: starts %.1f ms off the slot' % (t, first - slot))
            if not (armed or grant or (cqgrant and cq)):
                bad.append('%d: transmission while the owner had nothing armed' % t)
            if need_confirm and not cqgrant:
                bad.append('%d: transmission without a confirmation gesture since the start / the last owner abort' % t)
            grant = cqgrant = False
        elif e.startswith('txend'):
            m = re.search(r'samples=(\d+)', e)
            n = int(m.group(1))
            if n == FRAME and e.endswith('why=end'):
                full += 1
            else:
                cut += 1
            if n > FRAME:
                bad.append('%d: %d samples in one transmission' % (t, n))
    check('%s: %d transmissions (%d complete frames, %d cut by the owner) - every one on the slot grid within +-20 ms (worst %.3f ms), only while armed / granted, '
          'only after a confirmation gesture (or, for a CQ frame, a CQ arming), never again after an owner abort without a new confirmation' % (nm, starts, full, cut, worst),
          not bad and starts >= 10 * seconds / 7200.0, '; '.join(bad[:5]) or '%d confirmation gestures on SEND?, %d CQ armings' % (confirms, cqarm))
    check('%s: frames transmitted <= confirmation gestures x txmaxf + CQ armings, and the app\'s own counters agree with the simulator' % nm,
          starts <= confirms * 6 + cqarm and r.s('txapp.slots') == starts and r.s('txapp.frames_ok') == full,
          'frames %d, confirmations %d, CQ armings %d, app: slots %s ok %s cut %s' % (starts, confirms, cqarm, r.s('txapp.slots'), r.s('txapp.frames_ok'), r.s('txapp.frames_cut')))
    check('%s: receiver kept working (periods decoded %s, frames %s, skipped by the decoder %s)' % (nm, r.s('app.periods'), r.s('app.frames'), r.s('app.skipped')),
          r.s('app.frames', 0) > 500 * seconds / 7200.0 and r.s('app.log_err') == 0)
    facts.setdefault('soak', []).append({'variant': variant, 'hours': r.s('sim.virtual', 0) / 3600.0, 'events': nev, 'tx': starts, 'full': full, 'cut': cut, 'confirms': confirms,
                                         'worst_ms': worst, 'max_open': r.s('files.max_open'), 'gap': r.s('stop.poll_gap_max_ms'), 'rx_frames': r.s('app.frames'),
                                         'rx_periods': r.s('app.periods'), 'begin': r.s('txmodel.begin'), 'begin_ok': r.s('txmodel.begin_ok'), 'wall': wall,
                                         'taps': r.s('owner.taps'), 'opens': r.s('files.opens'), 'rejected': r.s('txmodel.write_rejected'), 'cancelled': r.s('txmodel.cancelled')})
    if variant == 'A':
        rx = receive('soak_A_rx', r.wav)
        tl = dict((x['utc'], x) for x in r.tlines())
        rl = dict((x['utc'], x['hex']) for x in rx.rlines())
        missing = [u for u, x in tl.items() if x['rc'] == 0 and x['samples'] == FRAME and rl.get(u) != x['hex']]
        foreign = [u for u, h in rl.items() if u not in tl or tl[u]['hex'] != h]
        cutdec = [tl[u]['samples'] for u in rl if tl.get(u) and tl[u]['samples'] != FRAME]
        check('%s: the whole captured TX audio through OUR receiver: all %d complete frames decode back bit-identical in their periods, nothing else is decoded '
              'except %d frames the owner cut short' % (nm, full, len(cutdec)), not missing and not foreign and len(rx.rlines()) == full + len(cutdec) and
              rx.s('decode_out.tx_calls') == 0, 'receiver frames %s; complete frames missing %d; decoded but never sent %d; cut frames that still decoded had %s samples' % (
                  rx.s('app.frames'), len(missing), len(foreign), sorted(cutdec)))
        facts['soak_cut_decoded'] = sorted(cutdec)
        facts['soak_rx'] = rx.s('app.frames')


def results(r):
    """the results the menu showed, one per gesture: row 2 of each menu block, consecutive repeats (repaints) counted once"""
    out = []
    for i, l in enumerate(r.d):
        if l.startswith('MENU ') and not l.startswith('MENU > SEND?') and i + 2 < len(r.d):
            if not out or out[-1] != r.d[i + 2]:
                out.append(r.d[i + 2])
    return out


def decrows(r):
    """the decoded rows of a run's DECODE screen ("HHMM [m ]TEXT" and continuations, found by the Q lines: tests/decrows.py),
    without status rows and views"""
    return DR.decoded(r.out.splitlines())


def part_rx_same():
    """the receive side of the RX+TX flavour must behave exactly like the RX-only flavour"""
    files = sorted(f for f in os.listdir(SAMPLES) if f.endswith('.wav')) if os.path.isdir(SAMPLES) else []
    jobs = [(f[:-7] if f.endswith('.8k.wav') else f[:-4], os.path.join(SAMPLES, f)) for f in files] + [('e2e_mix', MIX)]

    def one(j):
        a = Run('rxsame_rx_' + j[0], flags='', wav=j[1], rxonly=True, tail=80)
        b = Run('rxsame_tx_' + j[0], flags='', wav=j[1], tail=80)
        return j[0], a, b
    diff, frames = [], 0
    with ThreadPoolExecutor(max_workers=6) as ex:
        for nm, a, b in ex.map(one, jobs):
            frames += a.s('app.frames', 0)
            if decrows(a) != decrows(b) or not decrows(a) and a.s('app.frames') or a.s('app.frames') != b.s('app.frames') or a.s('app.msgs') != b.s('app.msgs') or b.s('decode_out.tx_calls') != 0 or a.rc or b.rc or \
               a.log('JS8MSG.TXT') != b.log('JS8MSG.TXT') or a.log() != b.log():
                diff.append(nm)
    # STREAMING FRONT END AND TX.  The decoder's FFT runs outside js8_decode() only inside js8_stream_put(), which only the
    # capture pump calls; the pump does nothing while the shell is inside tx_slot().  The simulator counts every sample handed
    # to the stream and every js8_stream_begin() while a transmission is in progress (tx_busy, or a session opened by an
    # accepted icap_tx_begin(), or TX audio being written): both counters must be 0 in EVERY run of this suite (clean()); here
    # the numbers of one run with receive traffic and a multi-frame message are shown.
    r = loopback('stream_idle', 'TX> Q7AB', L3, 'OK1BT: ' + L3.upper(), cfg='tx1=' + L3, wav=MIX, tail=90)[0]
    check('STREAM IDLE DURING TX: %s frames transmitted in consecutive slots while receive traffic runs - samples given to the streaming front end during the '
          'transmissions: 0, js8_stream_begin calls during the transmissions: 0 (outside them the stream ran)' % r.s('txmodel.transmissions'),
          r.s('txmodel.transmissions', 0) >= 2 and r.s('stream.samples', 0) > 100000 and r.s('stream.begins', 0) >= 1 and r.s('stream.in_tx_samples') == 0 and r.s('stream.in_tx_begins') == 0
          and r.s('app.frames', 0) >= 3,
          'transmitted samples %s; stream: %s samples in %s starts, during TX %s samples / %s starts; frames received in the run %s' % (
              r.s('txmodel.samples'), r.s('stream.samples'), r.s('stream.begins'), r.s('stream.in_tx_samples'), r.s('stream.in_tx_begins'), r.s('app.frames')))
    r = Run('decode_cut_stream', '3 taprow 0;4 kbd HB JO70;23 arm', wav=MIX, tail=40, extra=('-q', '80'), cfg='budget=0')
    ok, det = r.clean()
    audio_ok('decode cut for TX, streaming front end on', r, 1, first_period=2)
    check('decode cut for TX, streaming front end on (80 ms per poll): the decode is left for the slot, its 3 frames are kept, the frame goes out on time, and not one sample '
          'reaches the stream while the transmission is in progress (the stream did run: samples > 0)',
          ok and any('decode cut for TX' in n for n in r.notes()) and sum(1 for x in r.rlines() if x['utc'].endswith('T120000')) == 3 and r.s('txmodel.transmissions') == 1 and
          r.s('stream.samples', 0) > 0 and r.s('stream.begins', 0) > 0 and r.s('stream.in_tx_samples') == 0 and r.s('stream.in_tx_begins') == 0,
          'stream samples %s, begins %s, during TX: samples %s, begins %s; notes: %s | %s' % (r.s('stream.samples'), r.s('stream.begins'), r.s('stream.in_tx_samples'), r.s('stream.in_tx_begins'),
                                                                                         '; '.join(r.notes()[:3]), det))
    check('RX regression: the RX+TX flavour receives exactly like the RX-only flavour on the %d recordings + the e2e mix (same DECODE lines, same JS8LOG.TXT / JS8MSG.TXT), '
          'and without an armed loader it makes no tx call' % len(files), len(jobs) >= 1 and not diff, 'different: %s' % diff if diff else '%d files, %d frames' % (len(jobs), frames))
    facts['rxsame'] = {'files': len(jobs), 'frames': frames}


def part_asan(pool):
    r = sh([T7ASAN, 'unit'])
    check('ASan/UBSan: t7_tx unit', r.returncode == 0 and 'runtime error' not in r.stdout and 'AddressSanitizer' not in r.stdout, r.stdout.strip().splitlines()[-1])
    futs = [pool.submit(soak, v, 1800, 777 + i, SIMASAN, '_asan') for i, v in enumerate(('A', 'B', 'qtx'))]
    own = OWNER_STD % 'TX> Q7AB' + ';20.5 abort;38 disarm;40 arm;45 tap TX 1500;47 tap TX> HELLO;50 tap SEND?;52 arm;70 stop'   # (disarm: S2/O4 24 Nov 2026)
    r = Run('asan_abort_stop', own, cfg='tx1=%s\ntx2=%s' % (L3, FREE), tail=100, binary=SIMASAN, flags='on,tapsarmed')
    check('ASan/UBSan: abort in a frame, new message, EXIT in a frame', r.rc == 0 and 'runtime error' not in r.out and 'AddressSanitizer' not in r.out and r.s('txmodel.transmissions') == 2,
          'rc=%d transmissions=%s' % (r.rc, r.s('txmodel.transmissions')))
    return futs


def asan_check(futs):
    for f in futs:
        v, r, nev, wall = f.result()
        check('ASan/UBSan: 0.5 h soak, variant %s (%d owner events, %s transmissions)' % (v, nev, r.s('txmodel.transmissions')),
              r.rc == 0 and 'runtime error' not in r.out and 'AddressSanitizer' not in r.out and r.s('files.still_open') == 0 and r.s('txmodel.protocol_errors') == 0,
              'rc=%d wall %.0f s' % (r.rc, wall))


# Until the features stage this was the sha256 of JS8.APP as built BEFORE the transmit path existed, and the check was
# "byte-identical".  The features stage changed the receiver app on purpose (storage, heard table, inbox, views), so the
# hash cannot hold any more.  What the check was for - the transmit path must not leak into the RX-only app - is now
# tested directly: no transmit object and no transmit symbol in its link, and the build is reproducible.
RX_FORBIDDEN = r'js8tx|txs_|tx_begin|tx_write|tx_end|js8_build_message|js8_pack_'


def part_arm():
    """both flavours for the radio: sizes against the 80 KiB window; the RX-only app must not have changed by one byte"""
    tc = (os.environ.get('CROSS') or ((os.environ['TC'] + '/bin/arm-none-eabi-') if os.environ.get('TC') else 'arm-none-eabi-'))
    if '--skip-arm' in sys.argv or not shutil.which(tc + 'gcc'):
        print('SKIP ARM builds: cross compiler not found (' + tc + 'gcc)')
        return
    out = BUILD + '/arm'
    res = {}
    for fl, app, obj in (('rx', 'JS8.APP', 'obj'), ('tx', 'FT8.APP', 'obj-tx')):
        r = sh(['sh', 'shell/build.sh'], cwd=SRC, env=dict(os.environ, OUT=out, FLAVOUR=fl))
        warn = [l for l in r.stdout.splitlines() if 'warning' in l or 'error' in l]
        h = sh(['python3', '-I', '-B', 'tools/js8apphdr.py', out + '/' + app, '--su', out + '/' + obj], cwd=SRC).stdout
        m = re.search(r'load_size (\d+) B \+ bss (\d+) B = (\d+) B', h)
        und = sh([tc + 'nm', '-u', out + '/' + obj + '/js8.elf']).stdout.strip()
        sha = sh(['sha256sum', out + '/' + app]).stdout.split()[0]
        res[fl] = {'app': app, 'file': os.path.getsize(out + '/' + app), 'load': int(m.group(1)), 'bss': int(m.group(2)), 'total': int(m.group(3)), 'sha': sha, 'hdr': h,
                   'sizes': '\n'.join(l for l in r.stdout.splitlines() if re.match(r'^\s*\d+\s+\d+\s+\d+', l))}
        check('ARM %s flavour: builds and packs %s without warnings, no undefined symbol, image + bss %d B of 81920 (%.1f %%)' % (
            fl.upper(), app, int(m.group(3)), 100.0 * int(m.group(3)) / 81920), r.returncode == 0 and not warn and not und and int(m.group(3)) <= 81920,
            '; '.join(warn[:2]) or 'load %s + bss %s, %d B free' % (m.group(1), m.group(2), 81920 - int(m.group(3))))
    out2 = BUILD + '/arm-repro'
    sh(['sh', 'shell/build.sh'], cwd=SRC, env=dict(os.environ, OUT=out2, FLAVOUR='rx'))
    sha2 = sh(['sha256sum', out2 + '/JS8.APP']).stdout.split()[0] if os.path.exists(out2 + '/JS8.APP') else ''
    rxsyms = sh([tc + 'nm', out + '/obj/js8.elf']).stdout
    leak = sorted(set(re.findall(r' \w (\w*(?:%s)\w*)' % RX_FORBIDDEN, rxsyms)))
    rxobjs = [f for f in os.listdir(out + '/obj') if f in ('js8tx.o', 'js8pack.o', 'txsynth.o')]
    sh(['rm', '-rf', out2])
    check('ARM RX-only JS8.APP holds nothing of the transmit path (no js8tx / js8pack / txsynth object, no transmit symbol) and its build is reproducible '
          '(a second build is byte-identical)', res['rx']['sha'] == sha2 and not leak and not rxobjs, 'sha256 ' + res['rx']['sha'] + (' leaked: ' + ' '.join(leak) if leak else ''))
    check('ARM TX flavour is packed as file FT8.APP, header name "FT8", decoder mode FT8, ABI 3, background, 16 KiB stack',
          'name "FT8"' in res['tx']['hdr'] and 'registered modes: FT8' in res['tx']['hdr'] and 'BACKGROUND' in res['tx']['hdr'] and 'stack requested 16384' in res['tx']['hdr'],
          res['tx']['hdr'].splitlines()[0])
    stray = [os.path.join(dp, f) for dp, dn, fn in os.walk(BUILD) for f in fn if f == 'FT8.APP' and dp != out and not dp.startswith(BUILD + '/dist')]
    check('FT8.APP exists only in %s' % out, not stray, ' '.join(stray))
    # struct size of the transmit state on the radio
    nm = sh([tc + 'nm', '-S', out + '/obj-tx/js8.elf']).stdout
    m = re.search(r'^[0-9a-f]+ ([0-9a-f]+) b txq$', nm, re.M)
    res['txq'] = int(m.group(1), 16) if m else 0
    su = {}
    for f in os.listdir(out + '/obj-tx'):
        if f.endswith('.su'):
            for l in open(out + '/obj-tx/' + f):
                p = l.split('\t')
                su[p[0].split(':')[-1]] = int(p[1])
    res['su'] = su
    facts['arm'] = res


def main():
    os.makedirs(ROOT)
    global QUIET
    QUIET = os.path.join(ROOT, 'quiet.wav')
    silence(QUIET, 15)
    hours = 2.0
    if '--soak-hours' in sys.argv:
        hours = float(sys.argv[sys.argv.index('--soak-hours') + 1])
    if not build():
        return finish()
    r = sh([T7, 'unit'])
    n = len(re.findall(r'^PASS', r.stdout, re.M))
    check('t7_tx unit: engine unit tests against a mock loader (both strategies, every loader error, caps, menu)', r.returncode == 0, r.stdout.strip().splitlines()[-1])
    facts['t7_unit'] = r.stdout
    vec = os.path.join(ROOT, 'tx_model_vectors.txt')
    g = sh([sys.executable, os.path.join(SRC, 'tests', 'model.py'), '--src', REF, 'gen', '1000', str(int(time.time()) % 100000000)])
    open(vec, 'w').write(g.stdout)
    r = sh([T7, 'vectors', vec])
    for l in r.stdout.splitlines():
        if l.startswith(('PASS', 'FAIL')):
            check('t7_tx vectors: ' + l[6:], l.startswith('PASS'))
    facts['t7_vectors'] = r.stdout
    pool = ThreadPoolExecutor(max_workers=3)
    futs = []
    if '--skip-soak' not in sys.argv:
        secs = int(hours * 3600)
        futs = [pool.submit(soak, v, secs, 20261123 + i) for i, v in enumerate(('A', 'B', 'qtx'))]
    part_loopback()
    part_timing()
    part_strategies()
    part_safety()
    part_rx_same()
    part_arm()
    afuts = part_asan(pool) if '--skip-soak' not in sys.argv else []
    asan_check(afuts)
    for f in futs:
        v, r, nev, wall = f.result()
        soak_check(v, r, nev, wall, int(hours * 3600))
    return finish()


def finish():
    if leads:
        lo, hi = min(leads), max(leads)
        check('all %d transmissions of all runs: the app handed its first audio to the loader at least 300 ms before the slot start' % len(leads), lo[0] >= 300.0,
              'earliest %.0f ms (%s), latest %.0f ms (%s) before the slot' % (hi[0], hi[1], lo[0], lo[1]))
        facts['lead'] = {'n': len(leads), 'min': lo[0], 'min_run': lo[1], 'max': hi[0], 'max_run': hi[1]}
    q = [f for f in quality if 'off_ms' in f]
    if q:
        facts['quality'] = {
            'frames': len(q), 'off_ms_worst': max(abs(f['off_ms']) for f in q), 'len': sorted(set(int(f['len']) for f in q)),
            'model_err_lsb_max': max(f['model_err_lsb'] for f in q), 'peak': sorted(set(int(f['peak']) for f in q)), 'dbfs': sorted(set(f['dbfs'] for f in q)),
            'clip': int(sum(f['clip'] for f in q)), 'obw99_min': min(f['obw99_hz'] for f in q), 'obw99_max': max(f['obw99_hz'] for f in q),
            'oob_dbc_worst': max(f['oob_dbc'] for f in q), 'far_dbc_worst': max(f['far_dbc'] for f in q), 'f0': sorted(set(int(f['f0']) for f in q))}
    npass = sum(1 for c in checks if c[1])
    out = ['| | check | detail |', '|---|---|---|']
    for name, ok, det in checks:
        out.append('| %s | %s | %s |' % ('PASS' if ok else '**FAIL**', name.replace('|', '\\|'), det.replace('|', '\\|').replace('\n', ' ')[:400]))
    os.makedirs(BUILD + '/tx', exist_ok=True)
    open(BUILD + '/tx/checks.md', 'w').write('Run directory: `%s`\n\n%d checks, %d passed, %d failed.\n\n' % (ROOT, len(checks), npass, len(checks) - npass) + '\n'.join(out) + '\n')
    json.dump({'root': ROOT, 'checks': checks, 'facts': facts}, open(BUILD + '/tx/checks.json', 'w'), indent=1)
    print('tx_e2e: %d checks, %d passed, %d FAILED  (run directory %s)' % (len(checks), npass, len(checks) - npass, ROOT))
    return 0 if npass == len(checks) else 1


if __name__ == '__main__':
    sys.exit(main())
