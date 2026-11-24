/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * txchk - PC-only: measure what the JS8 app "transmitted" into the simulator's WAV (tests/js8sim.c -w).
 * Independent of the transmit code: it knows only the JS8 Normal signal definition (79 symbols of 1280
 * samples at 8000 Hz, 8 tones 6.25 Hz apart, continuous phase) and the SDK synthesizer's published
 * envelope (raised-cosine ramp of 40 samples at both ends).
 *
 * For every transmission found (a run of non-zero samples; zeros shorter than 80 samples do not end it):
 *   start    first sample of the run, and its offset from "period edge + 0.5 s" (sample 0 of the WAV is a
 *            period edge, a period is 120000 samples).  The waveform starts at phase 0, so its very first
 *            sample is 0 and the run is found one sample late; the tool corrects for that when sample
 *            start-1 fits the model better.
 *   length   samples from the start to the last non-zero sample (nominal 101120 = 12.64 s)
 *   f0       tone-0 frequency, from a 1 Hz DFT scan of the first symbol (Costas tone 4 = f0 + 25 Hz)
 *   tones    the 79 tones, non-coherent 8-tone detection per symbol
 *   model    max |sample - ideal| in LSB, where ideal = A * env(i) * sin(phi(i)), phi the continuous
 *            phase of the detected tones starting at 0: a phase jump, a click, a missing or repeated
 *            sample or a wrong envelope shows up as a large error
 *   step     max |x[i] - x[i-1]| against the largest step an ideal signal can make (A * 2 sin(pi f_max / fs))
 *   peak     max |sample|, dBFS, samples at +-32767 (clipping)
 *   spectrum Welch PSD (8192-point Hann, 50 % overlap, the whole transmission incl. both ramps):
 *            99 % power bandwidth, power outside [f0 - 50, f0 + 93.75] Hz relative to the total,
 *            highest density further than 200 Hz from the signal relative to the in-band peak
 * Output: one "F ..." line per transmission, "# ..." summary.  Exit code 0 always (the caller judges).
 *
 * usage: txchk FILE.wav
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define PI 3.14159265358979323846
#define NSYM 79
#define SPS 1280
#define FRAME (NSYM * SPS)
#define NFFT 8192

static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2 * PI / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; k++) {
                double wr = cos(ang * k), wi = sin(ang * k);
                double ur = re[i + k], ui = im[i + k];
                double vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi, vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k] = ur + vr; im[i + k] = ui + vi; re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
            }
    }
}
static double tone_pow(const int16_t *x, long n, double f)
{
    double c = 0, s = 0;
    for (long i = 0; i < n; i++) { c += x[i] * cos(2 * PI * f * i / 8000.0); s += x[i] * sin(2 * PI * f * i / 8000.0); }
    return c * c + s * s;
}
static double env(long i, long len)             /* the SDK synthesizer's ramp: sin^2(pi/2 (m + 1/2) / 40) */
{
    long m = i < 40 ? i : i >= len - 40 ? len - 1 - i : -1;
    double s;
    if (m < 0) return 1.0;
    s = sin(PI / 2 * ((double)m + 0.5) / 40.0);
    return s * s;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: txchk FILE.wav\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 44, SEEK_SET);
    long n = (sz - 44) / 2; int16_t *x = malloc((size_t)n * 2 + 2);
    if (fread(x, 2, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    int nf = 0; long total_nz = 0;
    for (long i = 0; i < n; i++) if (x[i]) total_nz++;
    for (long i = 0; i < n; ) {
        if (!x[i]) { i++; continue; }
        long a = i, last = i, z = 0;
        for (; i < n && z < 80; i++) if (x[i]) { last = i; z = 0; } else z++;
        long len = last - a + 1;
        if (len < 4 * SPS) { printf("S %ld %ld short burst\n", a, len); nf++; continue; }
        /* tone 0 frequency from the first symbol */
        double bp = 0; int bf = 0;
        for (int fq = 100; fq < 3900; fq++) { double p = tone_pow(x + a, SPS, fq); if (p > bp) { bp = p; bf = fq; } }
        int f0 = bf - 25;
        int tones[NSYM]; long nsym = len / SPS; if (nsym > NSYM) nsym = NSYM;
        for (int k = 0; k < NSYM; k++) tones[k] = 0;
        for (long k = 0; k < nsym; k++) {
            double best = -1;
            for (int t = 0; t < 8; t++) { double p = tone_pow(x + a + k * SPS, SPS, f0 + 6.25 * t); if (p > best) { best = p; tones[k] = t; } }
        }
        /* model fit: amplitude from the flat part; try the start at a and at a - 1 */
        long pk = 0, clip = 0; double stepmax = 0;
        for (long j = a; j <= last; j++) { long v = labs((long)x[j]); if (v > pk) pk = v; if (v >= 32767) clip++; if (j > a) { double d = fabs((double)x[j] - x[j - 1]); if (d > stepmax) stepmax = d; } }
        double errbest = 1e30; long abest = a;
        for (long s0 = a - 1; s0 <= a; s0++) {
            if (s0 < 0) continue;
            double ph = 0, e = 0;
            for (long j = 0; j < FRAME && s0 + j < n; j++) {
                int t = tones[j / SPS];
                double id = (double)pk * env(j, FRAME) * sin(ph), d = fabs(id - x[s0 + j]);
                if (d > e) e = d;
                ph += 2 * PI * (f0 + 6.25 * t) / 8000.0;
            }
            if (e < errbest) { errbest = e; abest = s0; }
        }
        long length = last - abest + 1;
        double fmax = f0 + 43.75, step_ideal = (double)pk * 2 * sin(PI * fmax / 8000.0);
        /* Welch PSD over the transmission */
        static double psd[NFFT / 2 + 1], re[NFFT], im[NFFT];
        memset(psd, 0, sizeof psd);
        int segs = 0;
        for (long s0 = abest - NFFT / 2; s0 + NFFT / 2 <= abest + FRAME + NFFT / 2; s0 += NFFT / 2) {
            for (int j = 0; j < NFFT; j++) {
                long k = s0 + j; double v = (k >= abest && k < abest + FRAME && k >= 0 && k < n) ? x[k] : 0.0;
                re[j] = v * (0.5 - 0.5 * cos(2 * PI * j / NFFT)); im[j] = 0;
            }
            fft(re, im, NFFT);
            for (int j = 0; j <= NFFT / 2; j++) psd[j] += re[j] * re[j] + im[j] * im[j];
            segs++;
        }
        double tot = 0, inb = 0, pin = 0, pout = 0, df = 8000.0 / NFFT;
        for (int j = 0; j <= NFFT / 2; j++) {
            double fr = j * df; tot += psd[j];
            if (fr >= f0 - 50 && fr <= f0 + 93.75) inb += psd[j];
            if (fr >= f0 && fr <= f0 + 43.75) { if (psd[j] > pin) pin = psd[j]; }
            if (fr < f0 - 200 || fr > f0 + 243.75) { if (psd[j] > pout) pout = psd[j]; }
        }
        double acc = 0, lo = 0, hi = 0; int got = 0;
        for (int j = 0; j <= NFFT / 2; j++) { acc += psd[j]; if (!got && acc >= 0.005 * tot) { lo = j * df; got = 1; } if (acc >= 0.995 * tot) { hi = j * df; break; } }
        char ts[NSYM + 1]; for (int k = 0; k < NSYM; k++) ts[k] = (char)('0' + tones[k]); ts[NSYM] = 0;
        long per = abest / 120000; double off_ms = (double)(abest - per * 120000 - 4000) / 8.0;
        printf("F %d start=%ld period=%ld off_ms=%.3f len=%ld f0=%d peak=%ld dbfs=%.2f clip=%ld model_err_lsb=%.1f step=%.0f step_ideal=%.0f "
               "obw99_hz=%.1f obw_lo=%.1f obw_hi=%.1f oob_dbc=%.1f far_dbc=%.1f tones=%s\n", nf, abest, per, off_ms, length, f0, pk,
               20 * log10((double)pk / 32768.0), clip, errbest, stepmax, step_ideal, hi - lo, lo, hi, 10 * log10((tot - inb) / tot + 1e-30),
               10 * log10(pout / pin + 1e-30), ts);
        nf++;
    }
    printf("# txchk file=%s samples=%ld nonzero=%ld transmissions=%d\n", argv[1], n, total_nz, nf);
    return 0;
}
