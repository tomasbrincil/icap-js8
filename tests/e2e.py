#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
"""End-to-end test of the JS8 radio app on the PC.

The radio's shell source (shell/js8app.c) runs unchanged inside the loader simulator tests/js8sim.c.
  (a) generated scenarios: messages -> frames (js8mk = js8pack) -> audio (js8gen = js8enc) ->
      simulator; the DECODE lines, popups, JS8LOG.TXT and JS8MSG.TXT are compared with what was sent;
  (b) every real recording in $SAMPLES/8k, compared with $SAMPLES/expected.json and with the
      decoder test bench (js8wav with the app's arena and band, run by this script: e2e/bench/*.txt).
The real-recording runs use the simulator's deterministic clock (-k 0: no x86 CPU time is charged), so the
capture start does not depend on how busy the PC is and two runs give the same frames.
Writes $BUILD/results-e2e.md and exits 1 if a check failed.  Nothing transmits.

  python3 -I -B tests/e2e.py [--skip-real] [--skip-rt] [--skip-arm]
"""
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import js8seg                          # reader / checker of the segment files (tests/js8seg.py)

HOME = os.path.expanduser('~')
SRC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CC = (os.environ.get('CC') or 'cc').split()      # host compiler, as the Makefile
BUILD = os.environ.get('BUILD') or SRC + '/build'
SDK = os.environ.get('ICAP_SDK') or os.environ.get('SDK') or SRC + '/third_party/icap-sdk'
SAMPLES = os.environ.get('SAMPLES') or SRC + '/recordings'
SD = BUILD + '/sd'                     # dictionary files (tools/mkjsc.py)
E2E = BUILD + '/e2e'
SIM, MK, GEN, WAVBIN = BUILD + '/js8sim', BUILD + '/js8mk', BUILD + '/js8gen', BUILD + '/js8wav'
ALPHABET = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+'
DATE = '20261123'

checks = []          # (name, passed, detail)
md = []              # report lines


def check(name, ok, detail=''):
    checks.append((name, bool(ok), detail))
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, (' - ' + detail) if detail else ''))
    return ok


def sh(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, **kw)


def build():
    cf = CC + ['-std=gnu99', '-O2', '-Wall', '-Wextra', '-Wshadow', '-fno-math-errno']
    core = ['src/js8dec.c', 'src/js8msg.c', 'src/js8jsc.c', 'src/js8asm.c', 'src/js8feat.c', 'src/js8_constants.c', 'src/js8crc.c', 'src/js8enc.c', 'shell/digi_util.c']
    r = sh(cf + ['-DDIGI_HOST', '-I' + SDK, '-Ishell', '-Isrc', 'tests/js8sim.c'] + core + ['-lm', '-o', SIM], cwd=SRC)
    check('build js8sim (shell/js8app.c + decoder + message layer, host)', r.returncode == 0 and not r.stdout.strip(),
          'no warnings' if not r.stdout.strip() else r.stdout[:300])
    r2 = sh(CC + ['-std=c99', '-D_POSIX_C_SOURCE=200809L', '-O2', '-Wall', '-Wextra', 'tests/js8mk.c', 'src/js8pack.c', 'src/js8msg.c',
             'src/js8jsc.c', '-o', MK], cwd=SRC)
    check('build js8mk', r2.returncode == 0 and not r2.stdout.strip(), r2.stdout[:300])
    if not os.path.exists(GEN):
        sh(['make', 'BUILD=' + BUILD, BUILD + '/js8gen'], cwd=SRC)
    r3 = sh(['make', 'BUILD=' + BUILD, BUILD + '/js8wav'], cwd=SRC)      # the decoder bench, from the same src/js8dec.c
    check('build js8wav (decoder bench)', r3.returncode == 0, r3.stdout[-300:] if r3.returncode else '')
    if not os.path.exists(SD + '/JSCHEAD.BIN'):
        sh([sys.executable, '-I', '-B', 'tools/mkjsc.py', '--out', SD], cwd=SRC)
    return r.returncode == 0 and r2.returncode == 0


def frames_of(call, text, grid=None, identify=False, dict_=True):
    cmd = [MK, '-c', call]
    if grid:
        cmd += ['-g', grid]
    if identify:
        cmd += ['-i']
    if dict_:
        cmd += ['-J', SD]
    r = sh(cmd + [text])
    if r.returncode:
        raise RuntimeError('js8mk failed: ' + r.stdout)
    out = []
    for l in r.stdout.splitlines():
        hx, ty, i3, tx = l.split(' ', 3) if l.count(' ') >= 3 else (l.split(' ') + [''])[:4]
        out.append({'hex': hx, 'type': int(ty), 'i3': int(i3), 'text': tx})
    return out


def chars12(hx):
    v = int(hx, 16) >> 16                      # the first 72 of the 88 bits
    return ''.join(ALPHABET[(v >> (66 - 6 * i)) & 63] for i in range(12))


def wrap(text, room, cont=35):
    """the wrapping rule of dec_lines() in shell/js8app.c, restated: `room` characters on the first line, `cont` on the
    continuation lines; the word HEARTBEAT takes two columns and is shown as HB; a full line is broken at a blank
    in its second half if there is one (unless a blank follows it anyway)"""
    text = text.rstrip(' ')
    out, pos, n = [], 0, len(text)
    while True:
        line, w, brk = '', 0, None
        while pos < n:
            hb = text.startswith('HEARTBEAT', pos)
            if w + (2 if hb else 1) > room:
                if text[pos] != ' ' and brk is not None:
                    line, pos = line[:brk[0]], brk[1]
                break
            if text[pos] == ' ' and w > room // 2:
                brk = (len(line), pos)
            if hb:
                line += 'HB'; pos += 9; w += 2
            else:
                line += text[pos]; pos += 1; w += 1
        out.append(line)
        while pos < n and text[pos] == ' ':
            pos += 1
        if pos >= n:
            return out
        room = cont


def hms(period, t0=120000):
    s = (t0 // 10000) * 3600 + (t0 // 100 % 100) * 60 + t0 % 100 + 15 * period
    return '%02d%02d%02d' % (s // 3600 % 24, s // 60 % 60, s % 60)


class Sim:
    """one simulator run, parsed"""

    def __init__(self, wav, sddir, args=(), overlay=SD, mycall='OK1BT'):
        cmd = ['nice', SIM, '-d', sddir]
        if overlay:
            cmd += ['-J', overlay]
        if mycall is not None:
            cmd += ['-c', mycall]
        cmd += list(args) + [wav]
        self.cmd = ' '.join(cmd)
        t = time.time()
        r = sh(cmd)
        self.wall = time.time() - t
        self.rc, self.out = r.returncode, r.stdout
        self.dir = sddir
        self.d, self.popups, self.panel, self.stat, self.over40 = [], [], {}, {}, 0
        self.recs = []                           # DECODE records: dict(time, snr, freq, mark, lines)
        for l in self.out.splitlines():
            if l.startswith('D '):
                s = l[2:]
                self.d.append(s)
                if len(s) > 40:
                    self.over40 += 1
                m = re.match(r'^(\d{4}) ([+-]\d\d) ([ \d]{4}) (.*)$', s)
                if m:
                    rest, mark = m.group(4), ''
                    if rest[:2] in ('= ', '> '):
                        mark, rest = rest[0], rest[2:]
                    self.recs.append({'time': m.group(1), 'snr': int(m.group(2)), 'freq': int(m.group(3)), 'mark': mark, 'lines': [rest]})
                elif s.startswith(' ' * 5) and self.recs:
                    self.recs[-1]['lines'].append(s[5:])
                else:
                    self.recs.append({'time': '?', 'snr': 0, 'freq': 0, 'mark': '?', 'lines': [s]})
            elif l.startswith('U '):
                self.popups.append(l[2:])
            elif re.match(r'^W\d ', l) or re.match(r'^W\d$', l):
                self.panel[int(l[1])] = l[3:]
            elif l.startswith('# '):
                for k, v in re.findall(r'([a-z_0-9]+)=(-?[0-9.]+)', l):
                    key = l.split()[1] + '.' + k
                    self.stat[key] = float(v) if '.' in v else int(v)
                m = re.search(r'arena peak=(\d+) of (\d+) \(heap (\d+), shell (\d+)\)', l)
                if m:
                    self.stat['arena.peak'], self.stat['arena.len'], self.stat['arena.heap'], self.stat['arena.shell'] = map(int, m.groups())
                m = re.search(r'decode ms last=(\d+) avg=(\d+) max=(\d+)', l)
                if m:
                    self.stat['dec.last'], self.stat['dec.avg'], self.stat['dec.max'] = map(int, m.groups())

    # Since the storage rework (shell/js8app_store.h) there is no JS8LOG.TXT / JS8MSG.TXT any more: each log is a row
    # of segment files JS8Lnnnn.TXT / JS8Mnnnn.TXT written once.  'JS8LOG.TXT' / 'JS8MSG.TXT' below name the STREAM:
    # its lines are those of all its segments in sequence order, without the header / trailer line of each segment.
    def lines(self, name):
        return js8seg.lines(self.dir, {'JS8LOG.TXT': 'JS8L', 'JS8MSG.TXT': 'JS8M'}[name])

    def logfiles(self, pfx, main):
        return [x.name for x in js8seg.stream(self.dir, pfx)], js8seg.lines(self.dir, pfx)

    def rlines(self):
        """R lines of the frame log (archives first): list of dict(utc, snr, dt, freq, hex, text)"""
        out = []
        for l in self.logfiles('JS8L', 'JS8LOG.TXT')[1]:
            m = re.match(r'^R (\d{8}T\d{6}) ([+-]\d\d) ([+-]\d+\.\d\d) (\d+\.\d) ([0-9a-f]{22}) (.*)$', l)
            if m:
                out.append({'utc': m.group(1), 'snr': int(m.group(2)), 'dt': float(m.group(3)), 'freq': float(m.group(4)),
                            'hex': m.group(5), 'text': m.group(6)})
        return out

    def mlines(self):
        out = []
        for l in self.logfiles('JS8M', 'JS8MSG.TXT')[1]:
            m = re.match(r'^M (\d{8}T\d{6}) ([+-]\d\d) (\d+) (\d+) (\S+) (.*)$', l)
            if m:
                out.append({'utc': m.group(1), 'snr': int(m.group(2)), 'freq': int(m.group(3)), 'frames': int(m.group(4)),
                            'flags': m.group(5), 'text': m.group(6)})
        return out

    def sane(self, name, max_open=4):
        s = self.stat
        segbad = js8seg.problems(self.dir)
        st = [x for x in js8seg.state(self.dir) if x['ok']]
        ok = (self.rc == 0 and s.get('decode_out.tx_calls') == 0 and s.get('decode_out.net_calls') == 0 and self.over40 == 0
              and s.get('decode_out.over40') == 0 and s.get('files.nofd') == 0 and s.get('files.badname') == 0
              and s.get('files.max_open', 99) <= max_open and s.get('files.still_open') == 0 and s.get('app.log_err') == 0
              and s.get('decode_out.win_over40') == 0 and not segbad and st and s.get('feat.spare_dirty') == 0)
        return check(name + ': exit 0, no TX / network call, all DECODE lines <= 40, app-window lines <= 40, <= %d files open, no bad file name, no log error, '
                     'every segment file complete with a valid CRC, a valid state file' % max_open, ok,
                     'rc=%d tx=%s net=%s over40=%s max_open=%s nofd=%s badname=%s%s' % (self.rc, s.get('decode_out.tx_calls'), s.get('decode_out.net_calls'),
                                                                                    s.get('decode_out.over40'), s.get('files.max_open'), s.get('files.nofd'), s.get('files.badname'),
                                                                                    (' segments: ' + '; '.join(segbad[:3])) if segbad else ''))


def fresh(name):
    d = os.path.join(E2E, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    return d


def block(title, text):
    md.append('')
    md.append(title)
    md.append('')
    md.append('```')
    md.extend(text.rstrip('\n').split('\n'))
    md.append('```')


# --------------------------------------------------------------------------- scenarios

def gen_wav(path, periods, sigs, noise=300, seed=7):
    """sigs: (f0, dt, level, hex, period)"""
    cmd = [GEN, '-o', path, '-p', str(periods), '-S', str(seed), '-n', str(noise)]
    for f0, dt, lev, hx, per in sigs:
        cmd += ['-s', '%s,%s,%s,x%s,0,%s' % (f0, dt, lev, hx, per)]
    r = sh(cmd)
    if r.returncode:
        raise RuntimeError('js8gen: ' + r.stdout)
    return ' '.join(cmd)


def scenario_mix():
    """multi-frame compressed text, heartbeat + CQ, directed MSG to MYCALL (checksum), SNR report to MYCALL, Huffman text"""
    st = [  # (f0, dt, snr, call, text, identify, dict, first period, expected message, to-me, flags)
        (700, 0.10, -8, 'Q2XYZ', 'HELLO WORLD, THIS IS A LONGER TEST MESSAGE FROM THE SIMULATOR. 73', True, True, 0,
         'Q2XYZ: HELLO WORLD, THIS IS A LONGER TEST MESSAGE FROM THE SIMULATOR. 73', False, '-'),
        (1100, -0.20, -12, 'Q7AB', 'HB JO70', False, True, 0, 'Q7AB: @HB HEARTBEAT JO70', False, '-'),
        (1100, -0.20, -12, 'Q7AB', 'CQ CQ CQ JO70', False, True, 2, 'Q7AB: @ALLCALL CQ CQ CQ JO70', False, 'g'),
        (1500, 0.30, -5, 'Q1ABC', 'OK1BT MSG MEET ME ON 40M AT 1900', False, True, 1, 'Q1ABC: OK1BT MSG MEET ME ON 40M AT 1900', True, '>c'),
        (1900, 0.00, -10, 'Q4XYZ', 'OK1BT SNR -12', False, True, 0, 'Q4XYZ: OK1BT SNR -12', True, '>'),
        (2300, 0.05, -6, 'N0CALL', 'TESTING 1 2 3 DE N0CALL K', False, False, 1, 'TESTING 1 2 3 DE N0CALL K', False, '-'),
    ]
    sigs, exp_frames, exp_msgs = [], [], []
    desc = []
    for f0, dt, snr, call, text, ident, dic, p0, msg, tome, flags in st:
        fr = frames_of(call, text, grid='JO70', identify=ident, dict_=dic)
        desc.append('%4d Hz dt %+.2f s %+d dB period %d..%d  %s sends "%s"  -> %d frame(s), types %s' % (
            f0, dt, snr, p0, p0 + len(fr) - 1, call, text, len(fr), ','.join(str(x['type']) for x in fr)))
        for i, x in enumerate(fr):
            sigs.append((f0, dt, snr, x['hex'], p0 + i))
            exp_frames.append({'period': p0 + i, 'f0': f0, 'dt': dt, 'snr': snr, 'hex': x['hex'], 'text': x['text'], 'type': x['type'],
                               'single': len(fr) == 1, 'tome': tome})
        exp_msgs.append({'period': p0 + len(fr) - 1, 'f0': f0, 'snr': snr, 'frames': len(fr), 'text': msg, 'tome': tome, 'flags': flags})
    return sigs, exp_frames, exp_msgs, desc


def verify_run(tag, sim, exp_frames, exp_msgs, mycall='OK1BT', nodict=False, t0=120000, check_files=True):
    """compare one run with what was sent"""
    recs = list(sim.recs)
    bad = []
    used = [False] * len(recs)

    def take(time_, f0, snr, mark, lines, snr_tol=3):
        for k, r in enumerate(recs):
            if not used[k] and r['time'] == time_ and r['mark'] == mark and abs(r['freq'] - f0) <= 1 and r['lines'] == lines:
                used[k] = True
                if abs(r['snr'] - snr) > snr_tol:
                    bad.append('SNR of "%s" is %+d, sent %+d' % (lines[0], r['snr'], snr))
                return k
        bad.append('missing %s %4d %s%s' % (time_, f0, mark + ' ' if mark else '', ' / '.join(lines)))
        return -1

    order_bad = []
    for m in exp_msgs:
        last = None
        for f in [x for x in exp_frames if x['f0'] == m['f0'] and m['period'] - m['frames'] < x['period'] <= m['period']]:
            raw = nodict and f['type'] in (6, 7)
            text = '#' + chars12(f['hex']) if raw else f['text']
            if f['single'] and f['tome'] and mycall:
                continue                          # shown only as the '>' message line
            last = take(hms(f['period'], t0)[:4], f['f0'], f['snr'], '', wrap(text, 26))
        if m['frames'] > 1 or (m['tome'] and mycall):
            if nodict and any(x['type'] in (6, 7) for x in exp_frames if x['f0'] == m['f0'] and m['period'] - m['frames'] < x['period'] <= m['period']):
                continue                          # text of the message is not recoverable without the dictionary
            k = take(hms(m['period'], t0)[:4], m['f0'], m['snr'], '>' if (m['tome'] and mycall) else '=', wrap(m['text'], 24))
            if last is not None and k >= 0 and k != last + 1 and m['frames'] > 1:
                order_bad.append(m['text'])
    extra = [recs[k] for k in range(len(recs)) if not used[k]]
    if nodict:                                    # without the dictionary only message lines with "{?}" words may be extra
        extra = [r for r in extra if not (r['mark'] in ('=', '>') and any('{?}' in l for l in r['lines']))]
    if True:
        for r in extra:
            bad.append('unexpected DECODE line: %s %+03d %4d %s%s' % (r['time'], r['snr'], r['freq'], r['mark'] + ' ' if r['mark'] else '', ' / '.join(r['lines'])))
    check(tag + ': DECODE screen shows exactly the sent frames and messages (text exact with HEARTBEAT shown as HB, time HHMM, freq +-1 Hz, SNR +-3 dB; the seconds are checked in JS8LOG.TXT)', not bad,
          '%d DECODE records' % len(recs) if not bad else '; '.join(bad[:6]))
    check(tag + ': a complete message line directly follows its last frame line', not order_bad, '; '.join(order_bad))
    if not check_files:
        return
    # JS8LOG.TXT
    rl = sim.rlines()
    want = sorted((DATE + 'T' + hms(f['period'], t0), f['hex']) for f in exp_frames)
    got = sorted((r['utc'], r['hex']) for r in rl)
    ok = want == got
    det = '%d R lines' % len(rl)
    if ok:
        worst_dt = worst_f = 0.0
        for f in exp_frames:
            r = next(r for r in rl if r['hex'] == f['hex'] and r['utc'] == DATE + 'T' + hms(f['period'], t0))
            worst_dt = max(worst_dt, abs(r['dt'] - f['dt']))
            worst_f = max(worst_f, abs(r['freq'] - f['f0']))
            if not nodict and r['text'].rstrip() != f['text'].rstrip():
                ok = False
                det = 'text "%s" != "%s"' % (r['text'], f['text'])
        det += ', worst |dt error| %.2f s, worst |freq error| %.1f Hz' % (worst_dt, worst_f)
        if worst_dt > 0.08 or worst_f > 1.0:
            ok = False
    else:
        det = 'want %s got %s' % (want[:3], got[:3])
    check(tag + ': JS8LOG.TXT has one R line per sent frame (period stamp, raw hex, text; dt within 0.08 s, freq within 1 Hz)', ok, det)
    # JS8MSG.TXT
    ml = sim.mlines()
    if not nodict:
        want = sorted((DATE + 'T' + hms(m['period'], t0), m['frames'], m['flags'] if mycall else m['flags'].replace('>', '') or '-', m['text']) for m in exp_msgs)
        got = sorted((m['utc'], m['frames'], m['flags'], m['text']) for m in ml)
        check(tag + ': JS8MSG.TXT has exactly the sent messages (stamp, frame count, flags, text)', want == got,
              '%d M lines' % len(ml) if want == got else 'want %s\n got %s' % (want, got))


def run_scenarios(skip_rt):
    md.append('')
    md.append('## (a) Generated scenarios')
    sigs, exp_frames, exp_msgs, desc = scenario_mix()
    os.makedirs(E2E, exist_ok=True)
    wav = E2E + '/mix.wav'
    gcmd = gen_wav(wav, 5, sigs)
    md.append('')
    md.append('### Scenario "mix": 6 stations, 5 periods, noise RMS 300, MYCALL = OK1BT')
    block('What was sent (js8mk = js8pack -> hex frames, js8gen = js8enc -> `e2e/mix.wav`; file sample 0 = 12:00:00):', '\n'.join(desc))
    block('Frames (hex, type, text of the frame):', '\n'.join('p%d %4d Hz %s type %d "%s"' % (f['period'], f['f0'], f['hex'], f['type'], f['text']) for f in exp_frames))
    check('scenario mix contains JSC-compressed frames (type 6) and Huffman frames (type 4)',
          any(f['type'] == 6 for f in exp_frames) and any(f['type'] == 4 for f in exp_frames),
          'types sent: %s' % sorted(set(f['type'] for f in exp_frames)))

    # A1: dictionary on the card, @MYCALL set
    s = Sim(wav, fresh('mix'))
    s.sane('mix')
    verify_run('mix', s, exp_frames, exp_msgs)
    pops = ['JS8 message for OK1BT | Q4XYZ: OK1BT SNR -12', 'JS8 message for OK1BT | Q1ABC: OK1BT MSG MEET ME ON 40M | AT 1900']
    check('mix: each message to OK1BT raised exactly one popup, nothing else', sorted(s.popups) == sorted(pops), ' ; '.join(s.popups))
    check('mix: status view counts %d frames, 6 messages, 2 to me, dictionary loaded' % len(exp_frames),
          s.stat.get('app.frames') == len(exp_frames) and s.stat.get('app.msgs') == 6 and s.stat.get('app.to_me') == 2 and s.panel.get(6, '').startswith('dict '),
          ' | '.join(s.panel.get(i, '') for i in range(8)))
    block('`%s`' % s.cmd, s.out)
    block('JS8LOG.TXT', '\n'.join(s.lines('JS8LOG.TXT')))
    block('JS8MSG.TXT', '\n'.join(s.lines('JS8MSG.TXT')))

    # A2: the same audio with a slow "CPU", deterministic model (decode overlaps the next capture)
    s = Sim(wav, fresh('mix_slow'), args=['-k', '0', '-q', '3'])
    s.sane('mix, slow CPU')
    verify_run('mix, slow CPU', s, exp_frames, exp_msgs)
    check('mix, slow CPU model (3 virtual ms per abort poll): capture and decode really overlapped (2 files open at once), no period skipped, no overrun',
          s.stat.get('files.max_open') == 2 and s.stat.get('app.skipped') == 0 and s.stat.get('audio.overruns') == 0 and s.stat.get('app.notes') == 0,
          'max_open=%s skipped=%s overruns=%s decode ms avg=%s max=%s (virtual)' % (s.stat.get('files.max_open'), s.stat.get('app.skipped'),
                                                                                 s.stat.get('audio.overruns'), s.stat.get('dec.avg'), s.stat.get('dec.max')))
    md.append('')
    md.append('**Slow CPU** (`-k 0 -q 3`): summary `%s`' % ' / '.join(l for l in s.out.splitlines() if l.startswith('# '))[:900])

    # A3: far too slow CPU: decodes are cut off, the app must survive and say so
    s = Sim(wav, fresh('mix_tooslow'), args=['-k', '0', '-q', '40'])
    s.sane('mix, hopeless CPU')
    check('mix, hopeless CPU (40 ms per abort poll, decoder cannot keep up): the app reports it (note / skipped periods) and still exits cleanly',
          s.rc == 0 and (s.stat.get('app.notes', 0) > 0 or s.stat.get('app.skipped', 0) > 0),
          'frames=%s of %d, skipped=%s, notes=%s, panel line 2 "%s"' % (s.stat.get('app.frames'), len(exp_frames), s.stat.get('app.skipped'), s.stat.get('app.notes'), s.panel.get(2)))
    block('Hopeless CPU (`-k 0 -q 40`), full output:', s.out)
    block('Hopeless CPU JS8LOG.TXT', '\n'.join(s.lines('JS8LOG.TXT')))

    # A3b: so slow that the first pass over the band does not finish: frames ARE lost, and the app must say so
    s = Sim(wav, fresh('mix_cutoff'), args=['-k', '0', '-q', '60'])
    s.sane('mix, decoder cut off')
    rl = s.rlines()
    sent = set((DATE + 'T' + hms(f['period']), f['hex']) for f in exp_frames)
    wrong = [r for r in rl if (r['utc'], r['hex']) not in sent]
    notes = [l for l in s.logfiles('JS8L', 'JS8LOG.TXT')[1] if l.startswith('N ') and ('out of time' in l or 'skipped' in l)]
    check('mix, decoder cut off (60 ms per abort poll: the first pass over the band does not finish in 15 s): frames ARE lost (%d of %d logged), '
          'every period has its "out of time" / "skipped" note, nothing wrong is logged, clean exit' % (len(rl), len(exp_frames)),
          s.rc == 0 and len(rl) < len(exp_frames) and not wrong and len(notes) >= max(4, s.stat.get('app.periods', 99)) and s.stat.get('app.frames') == len(rl),
          'periods %s, R lines %d, notes %d: %s' % (s.stat.get('app.periods'), len(rl), len(notes), '; '.join(notes[:3])))
    block('Decoder cut off (`-k 0 -q 60`), full output:', s.out)
    block('Decoder cut off JS8LOG.TXT', '\n'.join(s.lines('JS8LOG.TXT')))

    # A4: no dictionary files on the card
    s = Sim(wav, fresh('mix_nodict'), overlay=None)
    s.sane('mix, no dictionary')
    verify_run('mix, no dictionary: compressed frames shown raw as #<12 chars>, the rest as text', s, exp_frames, exp_msgs, nodict=True)
    nraw = sum(1 for f in exp_frames if f['type'] in (6, 7))
    check('mix, no dictionary: app runs, %d raw frames counted, panel says so, Huffman message still assembled' % nraw,
          s.stat.get('app.raw') == nraw and s.panel.get(6, '').startswith('no dictionary') and any(m['text'] == 'TESTING 1 2 3 DE N0CALL K' for m in s.mlines()),
          'raw=%s panel6="%s" panel2="%s"' % (s.stat.get('app.raw'), s.panel.get(6), s.panel.get(2)))
    block('`%s`' % s.cmd, s.out)

    # A5: @MYCALL empty, call= from JS8.CFG; band from the CFG
    d = fresh('mix_cfgcall')
    open(d + '/JS8.CFG', 'w').write('call=ok1bt\r\nflo=400 fhi=2600\r\n')
    s = Sim(wav, d, mycall='')
    s.sane('mix, call from JS8.CFG')
    verify_run('mix, @MYCALL empty + JS8.CFG call=ok1bt', s, exp_frames, exp_msgs)
    check('mix, JS8.CFG: panel line 0 shows the call and the band from the file', s.panel.get(0) == 'JS8 RX OK1BT 400-2600 Hz', s.panel.get(0, ''))
    check('mix, JS8.CFG: popups as with @MYCALL', sorted(s.popups) == sorted(pops), ' ; '.join(s.popups))

    # A6: no call anywhere: nothing is "to me"
    s = Sim(wav, fresh('mix_nocall'), mycall='')
    s.sane('mix, no call')
    verify_run('mix, no call at all: no ">" lines', s, exp_frames, exp_msgs, mycall='')
    check('mix, no call: no popup, note on the panel', not s.popups and 'no call' in s.panel.get(2, ''), 'panel2="%s"' % s.panel.get(2))

    # A7: popup busy
    s = Sim(wav, fresh('mix_busy'), args=['-B'])
    s.sane('mix, popup refused (busy)')
    verify_run('mix, popup refused (busy)', s, exp_frames, exp_msgs)
    check('mix, popup refused (busy): both refusals counted, no popup shown', s.stat.get('app.popup_busy') == 2 and not s.popups, 'popup_busy=%s' % s.stat.get('app.popup_busy'))

    # A8: radio clock 0.3 s late / early against the signals (-e): dt in the log moves, text does not
    for off in (0.3, -0.3):
        s = Sim(wav, fresh('mix_e'), args=['-e', str(off)])
        ef = [dict(f, dt=f['dt'] + off) for f in exp_frames]
        verify_run('mix, audio %+.1f s against the radio clock' % off, s, ef, exp_msgs)

    # A9: real time (optional, ~110 s)
    if not skip_rt:
        s = Sim(wav, fresh('mix_rt'), args=['-r', '-T', '35'])
        s.sane('mix, REAL TIME')
        ms = [m for m in exp_msgs]
        verify_run('mix, REAL TIME (-r, wall clock %.0f s)' % s.wall, s, exp_frames, ms)
        md.append('')
        md.append('**Real time** (`-r -T 35`, wall %.0f s): summary `%s`' % (s.wall, ' / '.join(l for l in s.out.splitlines() if l.startswith('# '))[:900]))

    # A10: JS8.CFG with a band the decoder would refuse: the app must fall back to the default band and say so once
    d = fresh('mix_narrow')
    open(d + '/JS8.CFG', 'w').write('flo=1000 fhi=1040\r\n')
    s = Sim(wav, d)
    s.sane('mix, JS8.CFG flo=1000 fhi=1040')
    verify_run('mix, JS8.CFG band of 40 Hz (refused, default band used)', s, exp_frames, exp_msgs)
    nn = [l for l in s.logfiles('JS8L', 'JS8LOG.TXT')[1] if l.startswith('N ')]
    check('mix, JS8.CFG band of 40 Hz: one note, panel shows the band really used', len(nn) == 1 and 'band' in nn[0] and s.panel.get(0) == 'JS8 RX OK1BT 450-2550 Hz',
          'notes: %s; panel0 "%s"' % (' ; '.join(nn), s.panel.get(0)))

    # A11: weak signals through the whole app: 12 stations x 15 periods at -18 dB (the decoder's 90 % point is about -18.6 dB)
    md.append('')
    md.append('### Scenario "weak": 12 stations x 15 periods, every frame at -18 dB, default band')
    wk, wkhex = [], {}
    for i in range(12):
        fr = frames_of('OK%dW%s' % (1 + i % 8, 'ABCDEFGHIJKL'[i] * 2), 'HB JO%d%d' % (6 + i % 3, i % 10))
        f0 = 520 + 165 * i
        wk.append((f0, 0.1 * (i % 5) - 0.2, -18, fr[0]['hex'], '*'))
        wkhex[fr[0]['hex']] = f0
    wavw = E2E + '/weak.wav'
    gen_wav(wavw, 15, wk, noise=300, seed=20261123)
    s = Sim(wavw, fresh('weak'), args=['-k', '0', '-T', '5'])
    s.sane('weak')
    rl = s.rlines()
    okf = [r for r in rl if r['hex'] in wkhex and abs(r['freq'] - wkhex[r['hex']]) <= 1.5]
    bench = sh(['nice', WAVBIN, wavw, '-q', '-b', '450,2550', '-r', str(s.stat.get('arena.shell', 0))]).stdout
    nb = sum(1 for l in bench.splitlines() if l.startswith('R '))
    check('weak: at -18 dB the app decodes at least 160 of the 180 frames, reports nothing that was not sent, and is within 4 frames of the decoder bench on the same audio',
          len(okf) >= 160 and len(okf) == len(rl) and abs(len(rl) - nb) <= 4, 'app %d (all sent: %s), bench %d of 180; reported SNR %s..%s dB' % (
              len(rl), len(okf) == len(rl), nb, min([r['snr'] for r in rl] or [0]), max([r['snr'] for r in rl] or [0])))

    # A12: noise only through the whole app, both bands: nothing may be decoded (false decodes of the complete chain)
    md.append('')
    md.append('### Scenario "noise only": 4 x 50 periods of noise per band')

    for k in range(4):
        sh([GEN, '-o', E2E + '/noise%d.wav' % k, '-p', '50', '-S', str(4242 + k), '-n', '300'])

    def noise_run(a):
        k, cfg = a
        p = E2E + '/noise%d.wav' % k
        dd = fresh('noise%d_%s' % (k, 'wide' if cfg else 'default'))
        if cfg:
            open(dd + '/JS8.CFG', 'w').write(cfg)
        return Sim(p, dd, args=['-k', '0', '-T', '5'])
    with ThreadPoolExecutor(max_workers=6) as ex:
        nres = list(ex.map(noise_run, [(k, c) for k in range(4) for c in (None, 'flo=200 fhi=3000\r\n')]))
    nper = sum(x.stat.get('app.periods', 0) for x in nres)
    nfr = sum(x.stat.get('app.frames', 0) for x in nres) + sum(len(x.d) for x in nres) + sum(len(x.rlines()) for x in nres)
    check('noise only through the app: %d periods (half default band, half 200..3000 Hz in two passes): no DECODE line, no R line, no popup' % nper,
          nper >= 390 and nfr == 0 and all(x.rc == 0 and not x.popups for x in nres), 'periods %d, frames + lines %d' % (nper, nfr))

    # A13: control characters of the JSC dictionary (word 69 = newline, word 72 = 0x1A) sent over the air: a received
    # frame must not be able to start a new line in a log ("N 120 FAKE NOTE" would look like a note record) nor reach
    # the popup raw.  Frames built with the independent packer of tests/rv_protocol.py.
    md.append('')
    md.append('### Scenario "control characters": JSC words 69 (newline) and 72 (0x1A) on the air, addressed to MYCALL')
    sys.path.insert(0, os.path.join(SRC, 'tests'))
    import rv_protocol as rv
    w_ = lambda t, sep=0: (rv.widx(t), sep)
    f1 = rv.directed('Q1ABC', 'OK1BT', 31, None, 1)                                    # "Q1ABC: OK1BT " (first)
    f2 = rv.jsc([w_(b'HELLO'), (69, 0), w_(b'N', 1)], 0)                                # "HELLO\nN "
    f3 = rv.jsc([w_(b'1'), w_(b'2'), w_(b'0', 1), w_(b'FAKE', 1), w_(b'NOTE')], 0)      # "120 FAKE NOTE"
    f4 = rv.jsc([(72, 0), w_(b'END')], 2)                                               # "\x1aEND" (last)
    wavc = E2E + '/ctl.wav'
    gen_wav(wavc, 4, [(1000, 0.0, -5, f1, 0), (1000, 0.0, -5, f2, 1), (1000, 0.0, -5, f3, 2), (1000, 0.0, -5, f4, 3)])
    s = Sim(wavc, fresh('ctl'), args=['-k', '0', '-T', '5'])
    s.sane('control characters')
    segs_l, segs_m = js8seg.stream(s.dir, 'JS8L'), js8seg.stream(s.dir, 'JS8M')
    raw_l, raw_m = b''.join(x.raw for x in segs_l), b''.join(x.raw for x in segs_m)

    def textfile(b, kinds):
        ls = b.split(b'\r\n')
        return (ls[-1] == b'' and all(l and chr(l[0]) in kinds and all(32 <= c <= 126 for c in l) for l in ls[:-1]) and chr(ls[0][0]) == 'H')
    ml = s.mlines()
    check('control characters: the frame log segments hold 4 R lines, the message log segments 1 M line (each segment: header line, lines, trailer line), '
          'CR LF only at line ends, printable ASCII only; the message and the popup show the newline as a blank and 0x1A as "?"',
          all(textfile(x.raw, 'HRNE') for x in segs_l) and all(textfile(x.raw, 'HME') for x in segs_m)
          and raw_l.count(b'\r\n') == 4 + 2 * len(segs_l) and raw_m.count(b'\r\n') == 1 + 2 * len(segs_m) and len(s.rlines()) == 4
          and len(ml) == 1 and ml[0]['text'] == 'Q1ABC: OK1BT HELLO N 120 FAKE NOTE?END' and ml[0]['flags'] == '>'
          and s.popups == ['JS8 message for OK1BT | Q1ABC: OK1BT HELLO N 120 FAKE | NOTE?END'] and all(all(32 <= ord(c) <= 126 for c in l) for l in s.d),
          'M: %s ; popup: %s' % (' / '.join(m['text'] for m in ml), ' ; '.join(s.popups)))
    block('`cat -A` of the JS8L* and JS8M* segment files:', (raw_l + raw_m).decode('latin1').replace('\r', '^M').replace('\n', '$\n'))

    # B: log "append" without an append mode: rollover into archive files, restart
    md.append('')
    md.append('### Scenario "rollover": 8 heartbeat stations x 14 periods, run twice in the same folder')
    calls = ['Q1AAA', 'Q2BBB', 'Q3CCC', 'Q4DDD', 'Q5EEE', 'Q6FFF', 'Q7GGG', 'Q8HHH']
    sg, hexes = [], []
    for i, c in enumerate(calls):
        fr = frames_of(c, 'HB JO70')
        hexes.append(fr[0]['hex'])
        sg.append((600 + 250 * i, 0.0, 'A2500', fr[0]['hex'], '*'))
    wav2 = E2E + '/roll.wav'
    gen_wav(wav2, 14, sg, noise=100)
    d = fresh('roll')
    s1 = Sim(wav2, d, args=['-T', '5'])
    s1.sane('rollover run 1')
    names1 = s1.logfiles('JS8L', 'JS8LOG.TXT')[0]
    s2 = Sim(wav2, d, args=['-T', '5', '-t', '130000'])
    s2.sane('rollover run 2')
    names, allr = s2.logfiles('JS8L', 'JS8LOG.TXT')
    mnames, allm = s2.logfiles('JS8M', 'JS8MSG.TXT')
    want = []
    for t0 in (120000, 130000):
        for p in range(14):
            want += sorted((DATE + 'T' + hms(p, t0), h) for h in hexes)
    got_r = [(r['utc'], r['hex']) for r in s2.rlines()]
    got_sorted = []
    for i in range(0, len(got_r), 8):
        got_sorted += sorted(got_r[i:i + 8])
    check('rollover: the JS8L segments hold every frame of both runs once, in time order (%d expected)' % len(want), got_sorted == want,
          '%d R lines in %s; run 1 alone: %s' % (len(got_r), ' + '.join('%s (%d B)' % (n, os.path.getsize(d + '/' + n)) for n in names), ' + '.join(names1)))
    gm = [(m['utc'], m['text']) for m in s2.mlines()]
    check('rollover: the JS8M segments hold all %d single-frame messages' % len(want), len(gm) == len(want) and len(set(gm)) == len(want),
          '%d M lines in %s' % (len(gm), ' + '.join('%s (%d B)' % (n, os.path.getsize(d + '/' + n)) for n in mnames)))
    sizes_ok = all(os.path.getsize(d + '/' + n) <= 4096 for n in names) and all(os.path.getsize(d + '/' + n) <= 1536 for n in mnames)
    seqs = [x.seq for x in js8seg.stream(d, 'JS8L')]
    check('rollover: no segment larger than its RAM buffer (4096 / 1536 B), several segments written, run 2 continued the sequence numbers of run 1 (1..n without a gap)',
          sizes_ok and len(names) >= 3 and len(mnames) >= 3 and seqs == list(range(1, len(seqs) + 1)) and len(names1) < len(names),
          'frame log files %d, message log files %d' % (len(names), len(mnames)))
    md.append('')
    md.append('Files after both runs: ' + ', '.join('`%s` %d B' % (n, os.path.getsize(d + '/' + n)) for n in names + mnames))
    md.append('')
    md.append('Run 2 summary: `%s`' % ' / '.join(l for l in s2.out.splitlines() if l.startswith('# '))[:900])


# --------------------------------------------------------------------------- real recordings

def tstart(g):
    """start of the frame in the file (s) from an R line: period stamp + 0.5 s + dt"""
    return int(g['utc'][9:11]) * 3600 + int(g['utc'][11:13]) * 60 + int(g['utc'][13:15]) - 12 * 3600 + 0.5 + g['dt']


def norm(t):
    return ' '.join(t.split())


WIDE_CFG = 'flo=200 fhi=3000\r\n'
BANDS = {'wide': '200,3000', 'default': '450,2550'}
MATCH_DT = 0.1                                   # s: a decoded frame is the reference frame if it starts within this (EXPECTED.md)


def bench_frames(wav, band, reserve, t=0.0, save=None):
    """the decoder bench (js8wav = src/js8dec.c with the shell's read contract) with the APP's arena: set of (period, hex)"""
    o = sh(['nice', WAVBIN, wav, '-q', '-b', BANDS[band], '-r', str(reserve), '-t', '%.3f' % t]).stdout
    if save:
        open(save, 'w').write(o)
    return sorted((int(m.group(1)), m.group(2)) for m in re.finditer(r'(?m)^R (\d+) \S+ \S+ \S+ ([0-9a-f]{22})', o))


def app_frames(sim):
    return sorted(((int(g['utc'][9:11]) * 3600 + int(g['utc'][11:13]) * 60 + int(g['utc'][13:15]) - 12 * 3600) // 15, g['hex']) for g in sim.rlines())


def real_one(key):
    """three deterministic runs per file (-k 0: the capture start does not depend on x86 CPU time) + the bench"""
    wav = '%s/8k/%s.8k.wav' % (SAMPLES, key)
    out = {}
    for tag, cfg, args in (('wide', WIDE_CFG, ['-k', '0']), ('default', None, ['-k', '0']), ('wide_k30', WIDE_CFG, ['-k', '0', '-q', '3'])):
        d = fresh('real/%s/%s' % (key, tag))
        if cfg:
            open(d + '/JS8.CFG', 'w').write(cfg)
        out[tag] = Sim(wav, d, args=args)
    os.makedirs(E2E + '/bench', exist_ok=True)
    res = out['wide'].stat.get('arena.shell', 0)
    for band in ('wide', 'default'):
        out['bench_' + band] = bench_frames(wav, band, res, save='%s/bench/%s.%s.txt' % (E2E, key, band))
    return key, out


def sweep_one(a):
    """decode_air_test_1min with the capture starting `off` ms into the file: app (both bands) and bench"""
    off, band, reserve = a
    wav = (sorted(glob.glob('%s/8k/*__decode_air_test_1min.8k.wav' % SAMPLES)) or ['%s/8k/decode_air_test_1min.8k.wav' % SAMPLES])[0]
    d = fresh('sweep/%s_%02d' % (band, off))
    if band == 'wide':
        open(d + '/JS8.CFG', 'w').write(WIDE_CFG)
    sim = Sim(wav, d, args=['-k', '0', '-e', '%.3f' % (-off / 1000.0)])
    return off, band, app_frames(sim), bench_frames(wav, band, reserve, t=off / 1000.0), sim


def verify_extra(a):
    key, g = a
    r = sh([sys.executable, '-I', '-B', os.path.join(SRC, 'tests', 'verify_frame.py'), '%s/8k/%s.8k.wav' % (SAMPLES, key), g['hex'], '%.1f' % g['freq'], '%.2f' % tstart(g)])
    return key, g, r.returncode == 0, r.stdout.strip()


def run_real():
    exp = json.load(open(SAMPLES + '/expected.json'))
    keys = [e['file'].replace('8k/', '').replace('.8k.wav', '') for e in exp['files']]
    ent = {k: e for k, e in zip(keys, exp['files'])}
    with ThreadPoolExecutor(max_workers=6) as ex:
        res = dict(ex.map(real_one, keys))
    md.append('')
    md.append('## (b) Real recordings')
    md.append('')
    md.append('Every file in `$SAMPLES/8k` through the simulator, file sample 0 = period edge 12:00:00, MYCALL OK1BT, dictionary present. '
              'All runs use the deterministic clock `-k 0` (no x86 CPU time is charged, so the capture starts exactly on the period edge and a re-run gives the same frames). '
              'Three runs per file: **wide** = `JS8.CFG flo=200 fhi=3000` (the band the reference used; the app\'s arena is smaller than the full data region, '
              'so the decoder splits this band into two passes), **default** = no CFG (450..2550 Hz, one pass), **wide slow** = wide with the slow-CPU model '
              '`-q 3` (3 virtual ms per decoder abort poll = a processor 30..40 times slower than this PC; a decode that is not finished when the next capture is complete is cut off, as on the radio). '
              '**bench** = `js8wav -r <shell reserve> -b <band> -t 0`: the same `src/js8dec.c` called directly on the file with the same arena size. '
              'A decoded frame counts as a reference frame if hex, frequency (3.2 Hz) and start time (%.1f s) agree.' % MATCH_DT)
    md.append('')
    md.append('| File | s | ref frames (must) | wide: frames | must found | bonus found | not in ref | bench wide | app = bench (wide) | default band: frames | must in band found | app = bench (default) | wide slow | text = ref | slow model: decode s avg/max | arena peak wide / default |')
    md.append('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    tot = {'must': 0, 'must_found': 0, 'bonus': 0, 'bonus_found': 0, 'extra': 0, 'frames': 0, 'text_eq': 0, 'text_cmp': 0, 'copies': 0, 'must_copies': 0}
    k30_notes, unknown = [], []
    detail = []
    all_ok_sane = True
    missing_must, text_bad, bench_diff = [], [], []
    missing_must_d, bench_diff_d, extras_all = [], [], []
    totd = {'must': 0, 'found': 0, 'copies': 0}
    for key in keys:
        r = res[key]
        w, dflt, k30 = r['wide'], r['default'], r['wide_k30']
        for s in (w, dflt, k30):
            st = s.stat
            if not (s.rc == 0 and st.get('decode_out.tx_calls') == 0 and st.get('decode_out.net_calls') == 0 and s.over40 == 0 and st.get('files.nofd') == 0
                    and st.get('decode_out.win_over40') == 0
                    and st.get('files.max_open', 9) <= 4 and st.get('app.log_err') == 0 and st.get('audio.overruns') == 0):
                all_ok_sane = False
                detail.append('SANITY FAIL %s: %s' % (key, s.cmd))
        rl = w.rlines()
        ref = ent[key]['reference'].get('A', {'frames': []})['frames']
        used = [False] * len(rl)
        found = {'must': 0, 'bonus': 0}
        total = {'must': 0, 'bonus': 0}
        copies = {'must': 0, 'bonus': 0}
        teq = tcmp = 0
        for fr in ref:
            total[fr['tier']] += 1
            hit = None
            for k, g in enumerate(rl):
                if not used[k] and g['hex'] == fr['hex'] and abs(g['freq'] - fr['f0_hz']) <= 3.2 and abs(tstart(g) - fr['t_start_s']) <= MATCH_DT:
                    hit = k
                    break
            if hit is None and any(g['hex'] == fr['hex'] and abs(g['freq'] - fr['f0_hz']) > 3.2 and abs(tstart(g) - fr['t_start_s']) <= MATCH_DT for g in rl):
                copies[fr['tier']] += 1           # the same frame is reported once per period (other frequency)
            elif hit is None:
                if fr['tier'] == 'must':
                    missing_must.append('%s slot %d %.1f Hz snr~%s %s "%s"' % (key, fr['slot'], fr['f0_hz'], fr['snr_est_db'], fr['hex'], fr['text']))
            else:
                used[hit] = True
                found[fr['tier']] += 1
                tcmp += 1
                if rl[hit]['text'].rstrip() == fr['text'].rstrip():
                    teq += 1
                else:
                    text_bad.append('%s %s: app "%s" / reference "%s"' % (key, fr['hex'], rl[hit]['text'], fr['text']))
        extra = [g for k, g in enumerate(rl) if not used[k]]
        # decoder bench with the app's arena, file start = period edge (generated by this run: e2e/bench/)
        bench = r['bench_wide']
        mine = app_frames(w)
        same = bench == mine
        if not same:
            bench_diff.append('%s: bench only %s; app only %s' % (key, sorted(set(bench) - set(mine)), sorted(set(mine) - set(bench))))
        # default band (what the radio uses without a JS8.CFG): the reference "must" frames inside 450..2550 Hz
        rd_ = dflt.rlines()
        dm = df_ = 0
        for fr in ref:
            if fr['tier'] != 'must' or not (450.0 <= fr['f0_hz'] <= 2550.0):
                continue
            dm += 1
            if any(g['hex'] == fr['hex'] and abs(g['freq'] - fr['f0_hz']) <= 3.2 and abs(tstart(g) - fr['t_start_s']) <= MATCH_DT for g in rd_):
                df_ += 1
            elif any(g['hex'] == fr['hex'] and abs(tstart(g) - fr['t_start_s']) <= MATCH_DT for g in rd_):
                totd['copies'] += 1
            else:
                missing_must_d.append('%s slot %d %.1f Hz snr~%s %s "%s"' % (key, fr['slot'], fr['f0_hz'], fr['snr_est_db'], fr['hex'], fr['text']))
        totd['must'] += dm
        totd['found'] += df_
        same_d = r['bench_default'] == app_frames(dflt)
        if not same_d:
            bench_diff_d.append('%s: bench only %s; app only %s' % (key, sorted(set(r['bench_default']) - set(app_frames(dflt))), sorted(set(app_frames(dflt)) - set(r['bench_default']))))
        nd = len(rd_)
        nk = len(k30.rlines())
        md.append('| %s | %.0f | %d (%d) | %d | %d/%d | %d/%d | %d | %d | %s | %d | %d/%d | %s | %d | %d/%d | %.1f/%.1f | %s / %s |' % (
            key, ent[key]['duration_s'], len(ref), total['must'], len(rl), found['must'], total['must'], found['bonus'], total['bonus'], len(extra),
            len(bench), 'yes' if same else 'NO', nd, df_, dm, 'yes' if same_d else 'NO', nk, teq, tcmp,
            k30.stat.get('dec.avg', 0) / 1000.0, k30.stat.get('dec.max', 0) / 1000.0,
            w.stat.get('arena.peak'), dflt.stat.get('arena.peak')))
        tot['must'] += total['must']
        tot['must_found'] += found['must']
        tot['copies'] += copies['must'] + copies['bonus']
        tot['must_copies'] += copies['must']
        outnotes = [l for l in k30.logfiles('JS8L', 'JS8LOG.TXT')[1] if l.startswith('N ') and 'out of time' in l]
        if outnotes:
            k30_notes.append('%s: %s' % (key, '; '.join(outnotes)))
        for g in extra:
            extras_all.append((key, g))
        tot['bonus'] += total['bonus']
        tot['bonus_found'] += found['bonus']
        tot['extra'] += len(extra)
        tot['frames'] += len(rl)
        tot['text_eq'] += teq
        tot['text_cmp'] += tcmp
        res[key]['extra'] = extra
    md.append('')
    md.append('Totals (wide): %d frames decoded; reference "must" %d/%d, "bonus" %d/%d (plus %d reference entries that are same-period copies of a decoded frame on another frequency); %d frames not in the reference; frame text equal to the reference text for %d of %d matched frames.' % (
        tot['frames'], tot['must_found'], tot['must'], tot['bonus_found'], tot['bonus'], tot['copies'], tot['extra'], tot['text_eq'], tot['text_cmp']))
    check('real: all 57 runs exit 0, no TX / network call, lines <= 40, <= 4 files, no audio overrun, no log error', all_ok_sane, '; '.join(detail[:3]))
    # frames the reference does not list: confirmed in the AUDIO by an independent test (not by the C decoder itself)
    with ThreadPoolExecutor(max_workers=6) as ex:
        vr = list(ex.map(verify_extra, extras_all))
    unknown = ['%s %s %s "%s": %s' % (k_, g_['utc'][9:], g_['hex'], g_['text'], o_) for k_, g_, ok_, o_ in vr if not ok_]
    vlines = ['%s %s %+d dB %.1f Hz %s "%s" -> %s' % (k_, g_['utc'][9:], g_['snr'], g_['freq'], g_['hex'], g_['text'], o_) for k_, g_, ok_, o_ in vr]
    check('real: every reference "must" frame (Normal) decoded by the app (wide band); a frame sent on several frequencies in one period counts once '
          '(the decoder reports it once, like JS8Call)', tot['must_found'] + tot['must_copies'] == tot['must'],
          '%d decoded + %d same-period copies of %d' % (tot['must_found'], tot['must_copies'], tot['must']) + ('; missing: ' + ' | '.join(missing_must[:8]) if missing_must else ''))
    check('real: every frame the reference does not list is confirmed in the audio by the independent energy test tests/verify_frame.py '
          '(frame re-encoded by the Python model, data-symbol energy at the expected tones >= 3x the other tones)', not unknown,
          '%d such frame(s): %s' % (len(vr), ' | '.join(o_ for _, _, _, o_ in vr)) if not unknown else ' | '.join(unknown))
    check('real: frame text of the app equals the reference decoder\'s text for every matched frame', tot['text_eq'] == tot['text_cmp'],
          '%d/%d' % (tot['text_eq'], tot['text_cmp']) + ('; ' + ' | '.join(text_bad[:5]) if text_bad else ''))
    check('real: app (capture files, pump, 2 band passes) finds exactly the frames of the decoder bench with the same arena, wide band', not bench_diff,
          ' | '.join(bench_diff[:6]) if bench_diff else '19 files identical')
    check('real, DEFAULT band 450..2550 Hz: every reference "must" frame inside the band decoded by the app', totd['found'] + totd['copies'] == totd['must'],
          '%d decoded + %d same-period copies of %d' % (totd['found'], totd['copies'], totd['must']) + ('; missing: ' + ' | '.join(missing_must_d[:8]) if missing_must_d else ''))
    check('real, DEFAULT band: app finds exactly the frames of the decoder bench with the same arena', not bench_diff_d,
          ' | '.join(bench_diff_d[:6]) if bench_diff_d else '19 files identical')
    # capture alignment: the crowded off-air file with the capture starting 0..75 ms into the file
    reserve = res[keys[0]]['wide'].stat.get('arena.shell', 0)
    with ThreadPoolExecutor(max_workers=6) as ex:
        sw = list(ex.map(sweep_one, [(off, band, reserve) for band in ('wide', 'default') for off in range(0, 80, 5)]))
    tab, sw_bad, sw_min = [], [], 99
    for band in ('wide', 'default'):
        row = []
        for off, b_, af, bf, sim in sorted(x for x in sw if x[1] == band):
            row.append('%d' % len(af) if af == bf else '%d (bench %d)' % (len(af), len(bf)))
            sw_min = min(sw_min, len(af), len(bf))
            if af != bf or sim.rc != 0:
                sw_bad.append('%s +%d ms: app %d, bench %d' % (band, off, len(af), len(bf)))
        tab.append('%-8s %s' % (band, '  '.join(row)))
    check('capture alignment, decode_air_test_1min (18 Normal frames, crowded): with the capture starting 0, 5, ... 75 ms into the file '
          'the app decodes at least 17 frames at EVERY start, in both bands, and exactly the frames of the bench', sw_min >= 17 and not sw_bad,
          'fewest %d; %s' % (sw_min, ' | '.join(sw_bad[:6]) if sw_bad else 'app = bench at all 32 points'))
    block('Frames decoded on decode_air_test_1min, capture start 0 / 5 / ... / 75 ms after file sample 0 (app; "bench" only where it differs):', '\n'.join(tab))
    if vlines:
        block('Frames the reference does not list, checked against the audio (tests/verify_frame.py):', '\n'.join(vlines))
    if missing_must:
        block('Reference "must" frames the app did not decode:', '\n'.join(missing_must))
    if text_bad:
        block('Frame text differences app / reference:', '\n'.join(text_bad))
    if bench_diff or bench_diff_d:
        block('Differences against the decoder bench (period index, hex):', '\n'.join(bench_diff + ['default band: ' + x for x in bench_diff_d]))
    if missing_must_d:
        block('Reference "must" frames inside 450..2550 Hz that the app did not decode in the default band:', '\n'.join(missing_must_d))
    ex_lines = []
    for key in keys:
        for g in res[key]['extra']:
            ex_lines.append('%s %s %+d dB %.1f Hz %s "%s"' % (key, g['utc'][9:], g['snr'], g['freq'], g['hex'], g['text']))
    if k30_notes:
        block('Wide slow runs in which a decode was cut off by the next period:', '\n'.join(k30_notes))
    if ex_lines:
        block('Frames decoded by the app that the reference does not list (CRC-12 + LDPC valid; look at the text):', '\n'.join(ex_lines))

    # published messages
    md.append('')
    md.append('### Messages against what the sources publish')
    x = res['x6100__js8_test']['wide']
    pub = [m['text'] for m in ent['x6100__js8_test']['published']['messages']]
    got = [m['text'] for m in x.mlines()]
    check('real x6100: the 6 published messages are in JS8MSG.TXT character for character', sorted(pub) == sorted(got), ' | '.join(got))
    sg = res['sigidwiki__JS8-10-27-2020']['wide']
    sm = [m['text'] for m in sg.mlines()]
    body = 'FOR GOD SO LOVED THE WORLD IN THIS WAY; HE GAVE HIS ONE AND ONLY SON, SO THAT EVERYONE WHO BELIEVES IN HIM WILL NOT PERISH BUT HAVE ETERNAL LIFE.'
    check('real sigidwiki: the published John 3:16 sentence is in one assembled message (9 compressed frames, tape drift 1385 -> 1395 Hz)',
          any(body in m for m in sm), ' | '.join(sm))
    # reference messages, all files
    nref = nfound = 0
    miss = []
    for key in keys:
        refm = ent[key]['reference'].get('A', {}).get('messages', [])
        mine = [norm(m['text']) for m in res[key]['wide'].mlines()]
        for m in refm:
            nref += 1
            t = norm(m['text'])
            if t in mine or any(t.startswith(a) and len(t) - len(a) == 4 for a in mine):
                nfound += 1
            else:
                miss.append('%s: reference "%s"' % (key, m['text']))
    md.append('')
    md.append('Reference-decoder messages (assembled by its own Python rules) found in JS8MSG.TXT after collapsing blanks, a stripped valid checksum counted as equal: **%d of %d**.' % (nfound, nref))
    if miss:
        block('Reference messages without an equal line in JS8MSG.TXT (not a pass/fail criterion: the reference joins by frequency only, see EXPECTED.md):', '\n'.join(miss))

    md.append('')
    md.append('### Decoded text per file (wide band run)')
    for key in keys:
        s = res[key]['wide']
        block('**%s** - DECODE screen:' % key, '\n'.join(s.d) if s.d else '(nothing decoded)')
        ml = s.lines('JS8MSG.TXT')
        arch = [n for n in s.logfiles('JS8M', 'JS8MSG.TXT')[0]]
        if ml:
            block('JS8MSG.TXT%s:' % (' (last file of %d)' % len(arch) if len(arch) > 1 else ''), '\n'.join(ml))
        md.append('')
        md.append('`%s`' % ' / '.join(l for l in s.out.splitlines() if l.startswith('# ') and ('app ' in l or 'decode ms' in l or 'dict' in l)))
    return res


APP_FILES = ['shell/js8app.c', 'shell/js8app_store.h', 'shell/js8app_tx.h', 'shell/js8app_kbd.h', 'src/js8dec.c', 'src/js8msg.c', 'src/js8jsc.c',
             'src/js8asm.c', 'src/js8feat.c', 'src/js8tx.c', 'src/js8pack.c', 'src/js8_constants.c', 'src/js8crc.c', 'src/js8enc.c', 'shell/digi_util.c']


def check_rx_only():
    """Loader services, a source check (the binaries are checked by tests/tx_e2e.py: no transmit symbol in JS8.APP):
    a transmit service of the loader is called only in shell/js8app_tx.h, which js8app.c includes only with -DJS8_TX;
    no source of the release calls a network service or reaches into the service table directly."""
    miss = [f for f in APP_FILES if not os.path.isfile(os.path.join(SRC, f))]
    txc = sh(['grep', '-lE', r'icap_tx_[a-z_]+ *\(', '--'] + APP_FILES, cwd=SRC).stdout.split()
    net = sh(['grep', '-nE', r'icap_net_[a-z_]+ *\(|icap__svc|svc *->', '--'] + APP_FILES, cwd=SRC).stdout.strip()
    app = open(os.path.join(SRC, 'shell/js8app.c'), encoding='utf-8').read()
    inc = re.findall(r'#ifdef JS8_TX\n#include "js8app_tx\.h"', app)
    check('loader services: icap_tx_* is called only in shell/js8app_tx.h (included only with -DJS8_TX), no network call and no '
          'direct service-table access in any source of the radio apps', txc == ['shell/js8app_tx.h'] and not net and not miss and len(inc) == 1 and app.count('js8app_tx.h"') == 1,
          (net or ' '.join(txc) or ', '.join(miss))[:200])


def run_arm():
    """the radio build: same sources, ARM, packed app; header against the limits"""
    tc = (os.environ.get('CROSS') or ((os.environ['TC'] + '/bin/arm-none-eabi-') if os.environ.get('TC') else 'arm-none-eabi-'))
    md.append('')
    md.append('## Radio build (ARM)')
    if not shutil.which(tc + 'gcc'):
        check('ARM build', False, 'cross compiler not found: ' + tc + 'gcc')
        return
    r = sh(['sh', 'shell/build.sh'], cwd=SRC, env=dict(os.environ, OUT=BUILD + '/arm'))
    warn = [l for l in r.stdout.splitlines() if 'warning' in l or 'error' in l]
    check('ARM build: shell/build.sh compiles, links (SDK app.ld + icap_start.S) and packs JS8.APP without warnings', r.returncode == 0 and not warn,
          '; '.join(warn[:3]) if warn else 'exit %d' % r.returncode)
    block('`sh shell/build.sh` (flags of the SDK apps, -O2, the message layer -Os; mkapp.py --name JS8 --models 1 --stack 16384 --abi 3 --background --mode JS8):', r.stdout)
    h = sh([sys.executable, '-I', '-B', 'tools/js8apphdr.py', BUILD + '/arm/JS8.APP', '--su', BUILD + '/arm/obj'], cwd=SRC)
    m = re.search(r'load_size (\d+) B \+ bss (\d+) B = (\d+) B', h.stdout)
    tot = int(m.group(3)) if m else 10 ** 9
    check('ARM: load_size + bss fits the 80 KiB app window (design target: <= 78 KiB), single image, no size reduction needed',
          h.returncode == 0 and tot <= 78 * 1024, m.group(0) + ' of 81920' if m else h.stdout[:200])
    block('`tools/js8apphdr.py arm/JS8.APP`:', h.stdout)
    und = sh([tc + 'nm', '-u', BUILD + '/arm/obj/js8.elf']).stdout.strip()
    nm = sh([tc + 'nm', BUILD + '/arm/obj/js8.elf']).stdout
    helpers = sorted(set(re.findall(r' T (__aeabi_\w+|__\w*div\w*)', nm)))
    check('ARM: no undefined symbol, no 64-bit division helper', not und and not any('ldiv' in x or 'di3' in x for x in helpers), 'libgcc helpers linked: ' + ', '.join(helpers))
    txc = sh(['grep', '-n', '-E', r'icap_(tx|net)_[a-z_]+ *\(', 'shell/js8app.c', 'shell/js8app_store.h', 'src/js8dec.c', 'src/js8msg.c', 'src/js8jsc.c', 'src/js8asm.c', 'src/js8feat.c', 'src/js8_constants.c', 'src/js8crc.c', 'src/js8enc.c',
              'shell/digi_util.c'], cwd=SRC).stdout.strip()
    check('ARM: no icap_tx_* / icap_net_* call in any source linked into JS8.APP (RX only)', not txc, txc[:200])
    # struct sizes ON THE RADIO (the simulator's numbers are x86-64 sizes): a probe object compiled with the ARM
    # compiler, sizes read with nm -S.  Nothing is linked or run.
    cf = ('-mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard -O2 -ffreestanding -nostdlib -fno-builtin -fno-pic -mword-relocations '
          '-fno-common -fno-tree-loop-distribute-patterns -fno-section-anchors -Wall').split()
    po = BUILD + '/arm/obj/arm_sizes.o'
    pr = sh([tc + 'gcc'] + cf + ['-I' + SDK, '-Ishell', '-Isrc', '-c', 'tests/arm_sizes.c', '-o', po], cwd=SRC)
    sz = dict((m_.group(2), int(m_.group(1), 16)) for m_ in re.finditer(r'(?m)^[0-9a-f]+ ([0-9a-f]+) \w (js8sz_\w+)$', sh([tc + 'nm', '-S', po]).stdout))
    ho = BUILD + '/obj/host_sizes.o'
    sh(CC + ['-O2', '-I' + SDK + '/apps/digi-common', '-Ishell', '-Isrc', '-c', 'tests/arm_sizes.c', '-o', ho], cwd=SRC)
    hz = dict((m_.group(2), int(m_.group(1), 16)) for m_ in re.finditer(r'(?m)^[0-9a-f]+ ([0-9a-f]+) \w (js8sz_\w+)$', sh(['nm', '-S', ho]).stdout))
    if pr.returncode == 0 and 'js8sz_asm' in sz and 'js8sz_jsc' in sz and 'js8sz_asm' in hz:
        al = lambda v: (v + 7) & ~7
        reserve = al(sz['js8sz_asm']) + al(sz['js8sz_jsc']) + 35840 + 4096 + 1536 + al(sz['js8sz_feat']) + 4096
        hres = 55936                             # JS8_RESERVE of shell/js8app.c: fixed, the same on the radio and in the simulator
        hneed = al(hz['js8sz_asm']) + al(hz['js8sz_jsc']) + 35840 + 4096 + 1536 + al(hz['js8sz_feat']) + 4096
        check('ARM: the shell pieces (ARM struct sizes, and the x86-64 sizes of the simulator) fit the fixed shell reserve, so the decoder has the same arena on the radio as in the simulator',
              reserve <= hres and hneed <= hres, 'ARM: js8asm_t %d B, js8jsc_t %d B, js8feat_t %d B -> the shell needs %d B of its fixed %d B; decoder arena on the radio and in the simulator: %d B' % (
                  sz['js8sz_asm'], sz['js8sz_jsc'], sz['js8sz_feat'], reserve, hres, 245760 - hres))
        block('Struct sizes in the ARM build (`tests/arm_sizes.c`, `arm-none-eabi-nm -S`):', '\n'.join('%-18s %6d B' % (k_[6:], v_) for k_, v_ in sorted(sz.items())) +
              '\nshell pieces = align8(asm) + align8(jsc) + 35840 (dictionary head) + 4096 + 1536 (segment buffers) + align8(features) + 4096 (capture read block) = %d B of the fixed 55936 B (JS8_RESERVE)\ndecoder arena  = 245760 - 55936 = %d B' % (reserve, 245760 - 55936))
    else:
        check('ARM: size probe tests/arm_sizes.c compiles', False, pr.stdout[:300])


def main():
    skip_real = '--skip-real' in sys.argv
    if not skip_real and not os.path.isfile(SAMPLES + '/expected.json'):
        print('SKIP part (b), real recordings: no %s/expected.json (no recordings are shipped; see docs/BUILDING.md)' % SAMPLES)
        skip_real = True
    skip_rt = '--skip-rt' in sys.argv
    t0 = time.time()
    md.append('# JS8 radio app - end-to-end results on the PC')
    md.append('')
    md.append('Generated by `tests/e2e.py` on %s. Every block below is real program output of that run.' % time.strftime('%Y-%m-%d %H:%M'))
    md.append('The radio shell `shell/js8app.c` is compiled unchanged into the loader simulator `tests/js8sim.c` (virtual clock, audio from the WAV, '
              'file API with the loader\'s limits, DECODE screen / panel / popups to stdout). Nothing transmits; the simulator counts TX and network calls (must be 0).')
    if not build():
        print('build failed')
        return 1
    check_rx_only()
    if '--skip-arm' not in sys.argv:
        run_arm()
    run_scenarios(skip_rt)
    if not skip_real:
        run_real()
    npass = sum(1 for c in checks if c[1])
    head = ['', '## Checks: %d passed, %d FAILED (run time %.0f s)' % (npass, len(checks) - npass, time.time() - t0), '', '| | Check | Detail |', '|---|---|---|']
    for n, ok, det in checks:
        head.append('| %s | %s | %s |' % ('PASS' if ok else '**FAIL**', n.replace('|', '/'), det.replace('|', '/').replace('\n', ' ')[:600]))
    notes = SRC + '/tests/e2e_notes.md'
    extra = open(notes).read() if os.path.exists(notes) else ''
    open(BUILD + '/results-e2e.md', 'w').write('\n'.join(md[:4] + head + ['', extra] + md[4:]) + '\n')
    print('%d passed, %d failed -> %s/results-e2e.md' % (npass, len(checks) - npass, BUILD))
    return 0 if npass == len(checks) else 1


if __name__ == '__main__':
    sys.exit(main())
