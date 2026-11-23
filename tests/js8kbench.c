/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8kbench - HOST check and micro-benchmark of the decoder's inner kernels (includes js8dec.c to reach them):
 *   1. STFT row (radix-4/5 FFT from the ring + spectral Hann + table log) against a direct double-precision
 *      windowed DFT of the same samples, for 8000, 6000 and 12000 Hz: number of waterfall cells that differ
 *      and the largest difference (0.5 dB steps); exit 1 if any cell is off by more than 1 step or more than
 *      0.5 % of the cells differ.
 *   2. time per STFT row and per BP iteration (fastest of several runs), for the cost model.
 * usage: js8kbench [seed]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "../src/js8dec.c"

static double now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3; }
static uint64_t rs = 88172645463325252ull;
static double ru(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (double)(rs >> 11) / 9007199254740992.0; }
static double rg(void) { double a = ru() + 1e-12, b = ru(); return sqrt(-2 * log(a)) * cos(6.283185307179586 * b); }

static int check_rate(int fs, int kind)
{
    static ctx_t c; static float qs[1024], ring[8192], y[4096]; static uint8_t out[4096];
    memset(&c, 0, sizeof c);
    if (stft_geom(&c, fs)) { printf("fs %d: geometry refused\n", fs); return 1; }
    stft_tables(&c, qs);
    const int nfft = c.nfft, RL = nfft + 3 * c.sub;
    int b_lo = (int)(100.0f / c.df), b_max = (c.N - FOSR) / FOSR, nb = b_max - b_lo;
    long ncell = 0, ndiff = 0; int maxd = 0;
    for (int trial = 0; trial < 6; trial++) {
        int ri = (int)(ru() * RL) & ~1;
        double f1 = 300 + ru() * 2500, a1 = kind == 2 ? 0.9 : kind ? pow(10, -ru() * 3) : 0, dc = kind == 2 ? 0.3 : 0;
        static double x[8192];
        for (int i = 0; i < nfft; i++) {
            double v = dc + a1 * cos(6.283185307179586 * f1 * i / fs + 1.0) + 0.01 * rg();
            v = floor(v * 32768.0 + 0.5); if (v > 32767) v = 32767; if (v < -32768) v = -32768;
            x[i] = v / 32768.0; ring[(ri + i) % RL] = (float)x[i];
        }
        static double xw[8192], ct[8192], stb[8192];
        for (int i = 0; i < nfft; i++) { xw[i] = x[i] * (2.0 / nfft) * 0.5 * (1.0 - cos(6.283185307179586 * i / nfft)); ct[i] = cos(6.283185307179586 * i / nfft); stb[i] = sin(6.283185307179586 * i / nfft); }
        rfft(&c, ring, ri, RL, y);
        spec_row(&c, y, b_lo * FOSR, nb, out);
        for (int fsub = 0; fsub < FOSR; fsub++)
            for (int bin = 0; bin < nb; bin++) {
                int k = (b_lo + bin) * FOSR + fsub;
                double re = 0, im = 0;
                for (int i = 0, ph = 0; i < nfft; i++) { re += xw[i] * ct[ph]; im -= xw[i] * stb[ph]; ph += k; if (ph >= nfft) ph -= nfft; }
                double db = 10.0 * log10(1e-12 + re * re + im * im);
                int sc = (int)(2.0 * db + 240.0); sc = sc < 0 ? 0 : sc > 255 ? 255 : sc;
                int d = abs(sc - out[fsub * nb + bin]);
                ncell++; if (d) ndiff++; if (d > maxd) maxd = d;
            }
    }
    printf("fs %5d (N = %d x %d) %s: %ld cells, %ld differ (%.3f %%), largest difference %d step(s)\n", fs, c.P, c.m,
           kind == 2 ? "full-scale tone + DC" : kind ? "tone + noise" : "noise", ncell, ndiff, 100.0 * ndiff / ncell, maxd);
    return maxd > 1 || ndiff * 200 > ncell;
}

int main(int argc, char **argv)
{
    int bad = 0;
    if (argc > 1) rs ^= strtoull(argv[1], 0, 0) * 0x9E3779B97F4A7C15ull;
    static const int rates[] = { 8000, 6000, 12000, 6400, 11200 };
    for (unsigned i = 0; i < sizeof rates / sizeof rates[0]; i++) for (int k = 0; k < 3; k++) bad |= check_rate(rates[i], k);
    {   /* timing: STFT row at 8 kHz, app band */
        static ctx_t c; static float qs[1024], ring[8192], y[4096]; static uint8_t out[4096];
        stft_geom(&c, 8000); stft_tables(&c, qs);
        const int RL = c.nfft + 3 * c.sub;
        for (int i = 0; i < RL; i++) ring[i] = (float)(0.01 * rg());
        double best = 1e30, bestf = 1e30;
        for (int rep = 0; rep < 200; rep++) {
            double t0 = now_us();
            for (int j = 0; j < 20; j++) rfft(&c, ring, 640 * (j % 7), RL, y);
            double t1 = now_us();
            for (int j = 0; j < 20; j++) spec_row(&c, y, 72 * FOSR, 345, out);
            double t2 = now_us();
            if (t1 - t0 < bestf) bestf = t1 - t0;
            if (t2 - t1 < best) best = t2 - t1;
        }
        printf("time per STFT row (8 kHz, 345 bins): FFT %.1f us + spectrum/log %.1f us\n", bestf / 20, best / 20);
        /* BP: noise LLRs (never converge), full iteration count */
        float llr[LN]; uint8_t plain[LN];
#ifdef JS8_BP_SPA
        bp_init(&c);
#endif
        double bb = 1e30; uint32_t it = 0;
        for (int rep = 0; rep < 200; rep++) {
            for (int i = 0; i < LN; i++) llr[i] = (float)(4.9 * rg());
            memset(&js8_stats, 0, sizeof js8_stats);
            double t0 = now_us();
            for (int j = 0; j < 5; j++) bp(&c, llr, js8_tune.iters, plain);
            double t1 = now_us();
            if (js8_stats.bp_iters && (t1 - t0) / js8_stats.bp_iters < bb) { bb = (t1 - t0) / js8_stats.bp_iters; it = js8_stats.bp_iters / 5; }
        }
        printf("BP on noise LLRs (never converges): %.2f us per iteration, %u iterations = %.1f us per run (%s)\n", bb, it, bb * it,
#ifdef JS8_BP_SPA
               "reference: flooding sum-product, float"
#else
               "layered sum-product, integer"
#endif
               );
        {   /* subtraction kernels: one signal x one row (640 samples) */
            static float x[640], ring2[8192]; sig_t sg; sst_t sst; double bs = 1e30;
            memset(&sg, 0, sizeof sg); sg.f0 = 1234.5f; sg.t0 = 0; for (int i = 0; i < NSYM; i++) sg.tones[i] = (uint8_t)(i % 8);
            for (int i = 0; i <= STAB + STAB / 4; i++) c.sn[i] = (float)sin(6.283185307179586 * i / STAB);
            for (int rep = 0; rep < 200; rep++) {
                memset(&sst, 0, sizeof sst);
                double t0 = now_us();
                for (int j = 0; j < 100; j++) { sig_corr(&c, &sg, &sst, 640 * j, x, 640, 4294967296.0f / 8000.0f); sig_sub(&c, &sg, &sst, 640 * j, ring2, 0, 4480, 640, 4294967296.0f / 8000.0f); }
                double t1 = now_us();
                if (t1 - t0 < bs) bs = t1 - t0;
            }
            printf("subtraction (correlate + subtract), one signal x one row: %.2f us\n", bs / 100);
        }
    }
    printf(bad ? "FAIL\n" : "PASS\n");
    return bad;
}
