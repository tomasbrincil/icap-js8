#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
"""snr_measure.py - review tool: measure the SNR (2500 Hz reference) of a generated JS8 WAV file
without using the generator's amplitude formula. Pure stdlib (own FFT).

usage: snr_measure.py FILE.wav F0 [PERIODS]
Assumes 8000 Hz mono s16, one frame per 15 s period starting 0.5 s into the period (dt = 0), tone 0
at F0. Only the 12.64 s where the frame is on are used. Welch periodogram (Hann, 8192 points, 50 %
overlap):  N0 = mean noise density over 200..3800 Hz outside F0-150..F0+200;
           Ps = power in F0-25..F0+70 minus N0 * 95 Hz;   SNR = Ps / (N0 * 2500).
Also prints the noise density per 600 Hz sub-band relative to the mean (whiteness) and the total
noise RMS implied by N0 * 4000 Hz.
"""
import sys, struct, math, cmath, wave

def fft(a):
    n = len(a)
    if n == 1:
        return a
    e, o = fft(a[0::2]), fft(a[1::2])
    out = [0j] * n
    h = n // 2
    for k in range(h):
        t = cmath.exp(-2j * math.pi * k / n) * o[k]
        out[k], out[k + h] = e[k] + t, e[k] - t
    return out

def main():
    name, f0 = sys.argv[1], float(sys.argv[2])
    w = wave.open(name, 'rb')
    assert (w.getframerate(), w.getnchannels(), w.getsampwidth()) == (8000, 1, 2)
    x = struct.unpack('<%dh' % w.getnframes(), w.readframes(w.getnframes()))
    nper = int(sys.argv[3]) if len(sys.argv) > 3 else len(x) // 120000
    N, fs = 8192, 8000.0
    win = [0.5 - 0.5 * math.cos(2 * math.pi * i / N) for i in range(N)]
    sw2 = sum(v * v for v in win)
    psd = [0.0] * (N // 2)
    nseg = 0
    for p in range(nper):
        a, b = p * 120000 + 4000, p * 120000 + 4000 + 101120
        s = a
        while s + N <= b:
            X = fft([complex(x[s + i] * win[i]) for i in range(N)])
            for k in range(N // 2):
                psd[k] += 2.0 * abs(X[k]) ** 2 / (fs * sw2)      # one-sided density, units^2 / Hz
            nseg += 1
            s += N // 2
    psd = [v / nseg for v in psd]
    df = fs / N
    def band(lo, hi):
        ks = [k for k in range(N // 2) if lo <= k * df < hi]
        return sum(psd[k] for k in ks) * df, len(ks) * df
    nz = [psd[k] for k in range(N // 2) if 200 <= k * df < 3800 and not (f0 - 150 <= k * df < f0 + 200)]
    n0 = sum(nz) / len(nz)
    pb, bw = band(f0 - 25, f0 + 70)
    ps = pb - n0 * bw
    print('%s: %d periods, %d segments' % (name, nper, nseg))
    print('  noise density N0 = %.4f units^2/Hz -> RMS over 4 kHz = %.1f (s16 units)' % (n0, math.sqrt(n0 * 4000)))
    out = []
    for lo in range(200, 3800, 600):
        ks = [psd[k] for k in range(N // 2) if lo <= k * df < lo + 600 and not (f0 - 150 <= k * df < f0 + 200)]
        out.append('%d..%d: %+.2f dB' % (lo, lo + 600, 10 * math.log10(sum(ks) / len(ks) / n0)))
    print('  whiteness: ' + ', '.join(out))
    print('  signal power %.1f units^2 (amplitude %.1f), noise power in 2500 Hz %.1f' % (ps, math.sqrt(2 * max(ps, 0)), n0 * 2500))
    print('  MEASURED SNR (2500 Hz) = %+.2f dB' % (10 * math.log10(ps / (n0 * 2500)) if ps > 0 else float('nan')))

if __name__ == '__main__':
    main()
