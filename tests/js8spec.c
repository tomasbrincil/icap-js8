/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8spec - small PC-only spectrum analysis tool for "make c2". It knows nothing about
 * the encoder; it only looks at the samples of a CLEAN (noise-free, single signal) file:
 *   1. envelope: first/last sample above 25 % of the peak -> start time and duration
 *   2. per 1280-sample symbol: DFT peak search (1 Hz grid 100..3900 Hz, then 0.01 Hz) ->
 *      frequency of each of the 79 symbols
 *   3. tone grid: distinct frequencies, least-squares spacing, deviation from the grid
 *   4. long DFT: for each tone the power at the tone frequency (-0.5/0/+0.5 Hz) over the whole frame
 * usage: js8spec FILE(.wav|.pcm) [-e TONES79] [-f F0] [-d DT]
 *   -e  expected 79 tones (e.g. from js8gen's FILE.txt), -f expected f0, -d expected dt
 * exit 0 only if: duration 12.64 s (+-2 ms), spacing 6.25 Hz (+-0.02), every symbol within
 * 0.15 Hz of the grid, 8 distinct tones, and the expected values (if given) match.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define FS 8000.0
#define SPS 1280
#define NSYM 79
#define TWO_PI 6.283185307179586476925286766559

static short *x;
static long nx;

static int load(const char *name)
{
    FILE *f = fopen(name, "rb");
    long size, off = 0, len;
    unsigned char *b;
    if (!f) { perror(name); return -1; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    b = (unsigned char *)malloc((size_t)size + 1);
    if (!b || fread(b, 1, (size_t)size, f) != (size_t)size) { fprintf(stderr, "read error\n"); return -1; }
    fclose(f);
    len = size;
    if (size > 44 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) {
        long p = 12;
        int fmt_ok = 0;
        len = -1;
        while (p + 8 <= size) {
            unsigned long cl = b[p + 4] | (b[p + 5] << 8) | (b[p + 6] << 16) | ((unsigned long)b[p + 7] << 24);
            if (!memcmp(b + p, "fmt ", 4)) {
                unsigned fmt = b[p + 8] | (b[p + 9] << 8), ch = b[p + 10] | (b[p + 11] << 8), bits = b[p + 22] | (b[p + 23] << 8);
                unsigned long sr = b[p + 12] | (b[p + 13] << 8) | (b[p + 14] << 16) | ((unsigned long)b[p + 15] << 24);
                printf("wav: format %u, %u channel(s), %lu Hz, %u bit\n", fmt, ch, sr, bits);
                fmt_ok = (fmt == 1 && ch == 1 && sr == 8000 && bits == 16);
            } else if (!memcmp(b + p, "data", 4)) { off = p + 8; len = (long)cl; if (off + len > size) len = size - off; break; }
            p += 8 + (long)cl + (long)(cl & 1);
        }
        if (len < 0 || !fmt_ok) { fprintf(stderr, "not an 8000 Hz 16-bit mono PCM WAV\n"); return -1; }
    } else printf("raw: headerless s16le assumed\n");
    nx = len / 2;
    x = (short *)malloc((size_t)nx * sizeof *x);
    for (long i = 0; i < nx; i++) x[i] = (short)(b[off + 2 * i] | (b[off + 2 * i + 1] << 8));
    free(b);
    return 0;
}

/* |DFT|^2 of n samples starting at s, at frequency f */
static double power_at(long s, long n, double f)
{
    double w = TWO_PI * f / FS, cr = cos(w), ci = sin(w), pr = 1, pi_ = 0, re = 0, im = 0;
    for (long i = 0; i < n; i++) {
        double v = x[s + i], t;
        re += v * pr; im -= v * pi_;
        t = pr * cr - pi_ * ci; pi_ = pr * ci + pi_ * cr; pr = t;
        if ((i & 1023) == 1023) { pr = cos(w * (double)(i + 1)); pi_ = sin(w * (double)(i + 1)); }
    }
    return re * re + im * im;
}

int main(int argc, char **argv)
{
    const char *expect = NULL;
    double exp_f0 = -1, exp_dt = -1e9;
    int have_dt = 0, i, k, fails = 0;
    long first = -1, last = -1, peak = 0, dur;
    double fsym[NSYM], fmin, sx = 0, sy = 0, sxx = 0, sxy = 0, spacing, f0fit, maxdev = 0;
    int tone[NSYM], used[64] = { 0 }, ndist = 0;
    char ts[NSYM + 1];

    if (argc < 2) { fprintf(stderr, "usage: js8spec FILE [-e TONES79] [-f F0] [-d DT]\n"); return 2; }
    for (i = 2; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-e")) expect = argv[i + 1];
        else if (!strcmp(argv[i], "-f")) exp_f0 = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "-d")) { exp_dt = atof(argv[i + 1]); have_dt = 1; }
        else { fprintf(stderr, "bad option %s\n", argv[i]); return 2; }
    }
    if (load(argv[1])) return 2;
    printf("samples: %ld (%.3f s)\n", nx, (double)nx / FS);

    for (long n = 0; n < nx; n++) { long a = labs((long)x[n]); if (a > peak) peak = a; }
    for (long n = 0; n < nx; n++) if (labs((long)x[n]) * 4 > peak) { if (first < 0) first = n; last = n; }
    if (first < 0 || peak == 0) { printf("FAIL  no signal\n"); return 1; }
    dur = last - first + 1;
    printf("envelope: peak %ld, first sample %ld (%.4f s), last %ld, duration %ld samples = %.4f s\n", peak, first, (double)first / FS, last, dur, (double)dur / FS);
    printf("%s  duration within 16 samples of 101120 (79 x 1280 = 12.64 s)\n", labs(dur - 101120) <= 16 ? "PASS" : (fails++, "FAIL"));
    if (have_dt) {
        double dt = (double)first / FS - 0.5;
        printf("%s  start = 0.5 s %+.4f s (expected dt %+.4f, tolerance 1 ms)\n", fabs(dt - exp_dt) <= 0.001 ? "PASS" : (fails++, "FAIL"), dt, exp_dt);
    }
    if (first + (long)NSYM * SPS > nx) { printf("FAIL  frame runs past the end of the file\n"); return 1; }

    /* per-symbol frequency; the envelope start is at most a fraction of a cycle late,
     * which is negligible against 1280 samples */
    for (k = 0; k < NSYM; k++) {
        long s = first + (long)k * SPS;
        double best = -1, bf = 0, f, lo, hi;
        for (f = 100; f <= 3900; f += 1.0) { double p = power_at(s, SPS, f); if (p > best) { best = p; bf = f; } }
        lo = bf - 1.5; hi = bf + 1.5; best = -1;
        for (f = lo; f <= hi; f += 0.01) { double p = power_at(s, SPS, f); if (p > best) { best = p; bf = f; } }
        fsym[k] = bf;
    }
    fmin = fsym[0];
    for (k = 1; k < NSYM; k++) if (fsym[k] < fmin) fmin = fsym[k];
    for (k = 0; k < NSYM; k++) {
        tone[k] = (int)floor((fsym[k] - fmin) / 6.25 + 0.5);
        if (tone[k] < 0 || tone[k] > 63) tone[k] = 63;
        if (!used[tone[k]]) { used[tone[k]] = 1; ndist++; }
        ts[k] = (char)(tone[k] < 10 ? '0' + tone[k] : '?');
        sx += tone[k]; sy += fsym[k]; sxx += (double)tone[k] * tone[k]; sxy += tone[k] * fsym[k];
    }
    ts[NSYM] = 0;
    spacing = (NSYM * sxy - sx * sy) / (NSYM * sxx - sx * sx);
    f0fit = (sy - spacing * sx) / NSYM;
    for (k = 0; k < NSYM; k++) { double d = fabs(fsym[k] - (f0fit + spacing * tone[k])); if (d > maxdev) maxdev = d; }

    printf("per-symbol peak frequencies (Hz), 79 symbols of 1280 samples:\n");
    for (k = 0; k < NSYM; k++) printf("%8.2f%s", fsym[k], (k % 10 == 9 || k == NSYM - 1) ? "\n" : "");
    printf("tone table (measured over the symbols using each tone):\n");
    for (i = 0; i < 64; i++) {
        double s = 0, mn = 1e9, mx = -1e9; int c = 0;
        if (!used[i]) continue;
        for (k = 0; k < NSYM; k++) if (tone[k] == i) { s += fsym[k]; c++; if (fsym[k] < mn) mn = fsym[k]; if (fsym[k] > mx) mx = fsym[k]; }
        printf("  tone %d: %3d symbols, mean %9.3f Hz (min %.2f max %.2f), offset from tone 0 fit %+8.3f Hz, whole-frame power at tone -0.5/0/+0.5 Hz: %.3e\n",
               i, c, s / c, mn, mx, s / c - f0fit, power_at(first, (long)NSYM * SPS, f0fit + spacing * i - 0.5) + power_at(first, (long)NSYM * SPS, f0fit + spacing * i)
               + power_at(first, (long)NSYM * SPS, f0fit + spacing * i + 0.5));
    }
    printf("least-squares fit: f0 = %.3f Hz, spacing = %.4f Hz, max deviation of a symbol from the grid %.3f Hz\n", f0fit, spacing, maxdev);
    printf("%s  %d distinct tones (expect 8)\n", ndist == 8 ? "PASS" : (fails++, "FAIL"), ndist);
    printf("%s  tone spacing %.4f Hz (expect 6.25 +-0.02)\n", fabs(spacing - 6.25) <= 0.02 ? "PASS" : (fails++, "FAIL"), spacing);
    printf("%s  all 79 symbols within 0.15 Hz of the grid (max %.3f)\n", maxdev <= 0.15 ? "PASS" : (fails++, "FAIL"), maxdev);
    if (exp_f0 >= 0) printf("%s  f0 %.3f Hz (expected %.3f +-0.1)\n", fabs(f0fit - exp_f0) <= 0.1 ? "PASS" : (fails++, "FAIL"), f0fit, exp_f0);
    printf("tones: %s\n", ts);
    {
        static const int c[7] = { 4, 2, 5, 6, 1, 3, 0 };
        int ok = 1;
        for (k = 0; k < 7; k++) if (tone[k] != c[k] || tone[36 + k] != c[k] || tone[72 + k] != c[k]) ok = 0;
        printf("%s  Costas 4256130 seen at symbols 0, 36 and 72\n", ok ? "PASS" : (fails++, "FAIL"));
    }
    if (expect) printf("%s  measured tones equal the expected 79 tones\n", (strlen(expect) == NSYM && !strcmp(expect, ts)) ? "PASS" : (fails++, "FAIL"));
    printf("js8spec: %s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
