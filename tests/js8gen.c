/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8gen - PC-only test signal generator for JS8 submode Normal. Nothing transmits; it only
 * writes a file: 8000 Hz, 16-bit signed, mono, WAV (or headerless PCM if the name ends in
 * .pcm / .PCM, the format of the radio's capture files).
 *
 * Modulation as in JS8Call's Modulator: plain continuous-phase 8-FSK, no shaping, no ramp,
 * 6.25 Hz spacing, 1280 samples/symbol, 79 symbols; the frame starts 0.5 s + dt into a 15 s
 * period. The phase is computed analytically, so dt may be any fraction of a sample.
 *
 * usage: js8gen -o OUT [options] -s SPEC [-s SPEC ...] [-R SPEC ...]
 *   -o FILE       output (.wav, or .pcm = raw); a text file FILE.txt lists what was generated
 *   -p N          number of consecutive 15 s periods (default 1)
 *   -n SIGMA      AWGN RMS in s16 units over the full 4 kHz band (default 0 = no noise;
 *                 300 if a signal level is given as SNR)
 *   -S SEED       PRNG seed (default 1); same seed -> identical file
 *   -s F0,DT,LEVEL,MSG[,I3[,PERIOD]]
 *                 F0     frequency of tone 0 in Hz
 *                 DT     time offset in s relative to the nominal start (period + 0.5 s)
 *                 LEVEL  "A<peak amplitude>" in s16 units (e.g. A8000), or a number = SNR in
 *                        dB referred to 2500 Hz noise bandwidth (needs noise)
 *                 MSG    12 alphabet characters, or "x" + 22 hex digits = raw 11-byte frame
 *                 I3     i3bit 0..7 (default 0; ignored with a raw frame)
 *                 PERIOD period index 0..N-1, or "*" = every period (default *)
 *   -R N,FMIN,FMAX,LEVEL[,DTMAX[,PERIOD]]
 *                 N random frames at random F0 in FMIN..FMAX, dt uniform in +-DTMAX
 *                 (default 0), fresh random frames in every selected period
 * SNR: P_signal / (noise power in 2500 Hz) = (A^2/2) / (SIGMA^2 * 2500/4000).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "../src/js8_constants.h"
#include "../src/js8crc.h"
#include "../src/js8enc.h"

#define TWO_PI 6.283185307179586476925286766559

typedef struct {
    double f0, dt, amp, snr;
    int has_snr, period;
    uint8_t frame[11], tones[79];
} sig_t;

#define MAXSIG 4096
static sig_t sigs[MAXSIG];
static int nsig;

static uint64_t rng_s;
static uint64_t rng64(void)
{
    uint64_t z = (rng_s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static double rng_u(void) { return (double)((rng64() >> 11) + 1) / 9007199254740993.0; } /* (0,1) */
static double rng_gauss(void)
{
    static int have; static double spare;
    double u, v, r;
    if (have) { have = 0; return spare; }
    u = rng_u(); v = rng_u();
    r = sqrt(-2.0 * log(u));
    spare = r * sin(TWO_PI * v); have = 1;
    return r * cos(TWO_PI * v);
}

static void die(const char *m, const char *a)
{
    fprintf(stderr, "js8gen: %s%s%s\n", m, a ? ": " : "", a ? a : "");
    exit(2);
}

/* split s at commas into at most max fields (modifies s) */
static int split(char *s, char **f, int max)
{
    int n = 0;
    while (n < max) {
        f[n++] = s;
        s = strchr(s, ',');
        if (!s) break;
        *s++ = 0;
    }
    return n;
}

static void parse_level(const char *s, sig_t *g)
{
    if (s[0] == 'A' || s[0] == 'a') { g->amp = atof(s + 1); g->has_snr = 0; }
    else { g->snr = atof(s); g->has_snr = 1; }
}

static void add_sig(const sig_t *g, int period, int nper)
{
    int p;
    for (p = 0; p < nper; p++) {
        if (period >= 0 && p != period) continue;
        if (nsig >= MAXSIG) die("too many signals", NULL);
        sigs[nsig] = *g; sigs[nsig].period = p; nsig++;
    }
}

static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE *f, unsigned long v) { put16(f, (unsigned)(v & 0xFFFF)); put16(f, (unsigned)((v >> 16) & 0xFFFF)); }

int main(int argc, char **argv)
{
    const char *out = NULL;
    char *sspec[512], *rspec[64];
    int ns = 0, nr = 0, nper = 1, i, k, raw = 0;
    double sigma = 0;
    int sigma_set = 0, any_snr = 0;
    uint64_t seed = 1;
    long total, clips = 0;
    double *buf;
    FILE *fp;
    char tname[1024];

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) nper = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) { sigma = atof(argv[++i]); sigma_set = 1; }
        else if (!strcmp(argv[i], "-S") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) { if (ns >= 512) die("too many -s", NULL); sspec[ns++] = argv[++i]; }
        else if (!strcmp(argv[i], "-R") && i + 1 < argc) { if (nr >= 64) die("too many -R", NULL); rspec[nr++] = argv[++i]; }
        else die("bad option (see the header of tests/js8gen.c)", argv[i]);
    }
    if (!out) die("no -o FILE", NULL);
    if (nper < 1 || nper > 240) die("periods must be 1..240", NULL);
    rng_s = seed;
    k = (int)strlen(out);
    raw = k > 4 && (!strcmp(out + k - 4, ".pcm") || !strcmp(out + k - 4, ".PCM"));

    for (i = 0; i < ns; i++) {
        char tmp[256], *f[6];
        sig_t g;
        int n, i3 = 0, period = -1;
        memset(&g, 0, sizeof g);
        if (strlen(sspec[i]) >= sizeof tmp) die("spec too long", sspec[i]);
        strcpy(tmp, sspec[i]);
        n = split(tmp, f, 6);
        if (n < 4) die("-s needs F0,DT,LEVEL,MSG", sspec[i]);
        g.f0 = atof(f[0]); g.dt = atof(f[1]); parse_level(f[2], &g);
        if (n > 4) i3 = atoi(f[4]);
        if (n > 5 && strcmp(f[5], "*")) period = atoi(f[5]);
        if (period >= nper) die("period index out of range", sspec[i]);
        if (f[3][0] == 'x' && strlen(f[3]) == 23) {
            for (k = 0; k < 11; k++) {
                unsigned v;
                if (sscanf(f[3] + 1 + 2 * k, "%2x", &v) != 1) die("bad hex frame", sspec[i]);
                g.frame[k] = (uint8_t)v;
            }
            if (!js8_crc12_check(g.frame)) fprintf(stderr, "js8gen: warning: raw frame %s has a bad CRC (generated anyway)\n", f[3] + 1);
        } else {
            if (strlen(f[3]) != 12) die("MSG must be 12 characters or x+22 hex", sspec[i]);
            if (js8_frame_build(f[3], i3, g.frame) != 0) die("MSG has a character outside the JS8 alphabet or bad I3", sspec[i]);
        }
        add_sig(&g, period, nper);
    }
    for (i = 0; i < nr; i++) {
        char tmp[256], *f[6];
        int n, cnt, period = -1, p, c;
        double fmin, fmax, dtmax = 0;
        sig_t lv;
        if (strlen(rspec[i]) >= sizeof tmp) die("spec too long", rspec[i]);
        strcpy(tmp, rspec[i]);
        n = split(tmp, f, 6);
        if (n < 4) die("-R needs N,FMIN,FMAX,LEVEL", rspec[i]);
        cnt = atoi(f[0]); fmin = atof(f[1]); fmax = atof(f[2]);
        memset(&lv, 0, sizeof lv);
        parse_level(f[3], &lv);
        if (n > 4) dtmax = atof(f[4]);
        if (n > 5 && strcmp(f[5], "*")) period = atoi(f[5]);
        for (p = 0; p < nper; p++) {
            if (period >= 0 && p != period) continue;
            for (c = 0; c < cnt; c++) {
                sig_t g = lv;
                char txt[12];
                for (k = 0; k < 12; k++) txt[k] = js8_alphabet[rng64() & 63];
                js8_frame_build(txt, (int)(rng64() & 7), g.frame);
                g.f0 = fmin + (fmax - fmin) * rng_u();
                g.dt = dtmax * (2.0 * rng_u() - 1.0);
                add_sig(&g, p, nper);
            }
        }
    }
    if (!nsig && !sigma_set) die("nothing to generate (no -s / -R and no -n)", NULL);
    for (i = 0; i < nsig; i++) if (sigs[i].has_snr) any_snr = 1;
    if (any_snr && !sigma_set) sigma = 300.0;
    if (any_snr && sigma <= 0) die("a level given as SNR needs noise (-n SIGMA > 0)", NULL);

    total = (long)nper * JS8A_PERIOD_SAMPLES;
    buf = (double *)calloc((size_t)total, sizeof *buf);
    if (!buf) die("out of memory", NULL);

    for (i = 0; i < nsig; i++) {
        sig_t *g = &sigs[i];
        const double T = (double)JS8A_SPS / JS8_FS;          /* 0.16 s */
        const double df = (double)JS8_FS / JS8A_SPS;         /* 6.25 Hz */
        double t0 = (double)g->period * JS8A_PERIOD_SAMPLES / JS8_FS + (double)JS8A_START_SAMPLES / JS8_FS + g->dt;
        double cum[80]; /* sum of tone numbers before symbol k */
        long n0, n1, n;
        if (g->has_snr) g->amp = sqrt(2.0 * pow(10.0, g->snr / 10.0) * sigma * sigma * 2500.0 / (JS8_FS / 2.0));
        else g->snr = sigma > 0 ? 10.0 * log10((g->amp * g->amp / 2.0) / (sigma * sigma * 2500.0 / (JS8_FS / 2.0))) : 999.0;
        js8_encode(g->frame, js8_costas_normal, g->tones);
        cum[0] = 0;
        for (k = 0; k < 79; k++) cum[k + 1] = cum[k] + g->tones[k];
        n0 = (long)ceil(t0 * JS8_FS - 1e-9);
        n1 = (long)ceil((t0 + 79 * T) * JS8_FS - 1e-9); /* first sample after the frame */
        for (n = n0; n < n1; n++) {
            double t = (double)n / JS8_FS - t0, ph;
            int sym;
            if (n < 0 || n >= total) continue; /* part outside the file is dropped */
            if (t < 0) t = 0;
            sym = (int)(t / T);
            if (sym > 78) sym = 78;
            /* phase = 2 pi (f0 t + df * integral of the tone number) */
            ph = g->f0 * t + df * (cum[sym] * T + g->tones[sym] * (t - sym * T));
            buf[n] += g->amp * sin(TWO_PI * (ph - floor(ph)));
        }
    }
    if (sigma > 0)
        for (i = 0; i < total; i++) buf[i] += sigma * rng_gauss();

    fp = fopen(out, "wb");
    if (!fp) { perror(out); return 1; }
    if (!raw) {
        unsigned long bytes = (unsigned long)total * 2;
        fwrite("RIFF", 1, 4, fp); put32(fp, 36 + bytes); fwrite("WAVEfmt ", 1, 8, fp);
        put32(fp, 16); put16(fp, 1); put16(fp, 1); put32(fp, JS8_FS); put32(fp, JS8_FS * 2); put16(fp, 2); put16(fp, 16);
        fwrite("data", 1, 4, fp); put32(fp, bytes);
    }
    for (i = 0; i < total; i++) {
        double v = floor(buf[i] + 0.5);
        if (v > 32767) { v = 32767; clips++; }
        if (v < -32768) { v = -32768; clips++; }
        put16(fp, (unsigned)((int)v & 0xFFFF));
    }
    if (fclose(fp)) { perror(out); return 1; }

    snprintf(tname, sizeof tname, "%s.txt", out);
    fp = fopen(tname, "w");
    if (!fp) { perror(tname); return 1; }
    fprintf(fp, "# js8gen fs=%d periods=%d samples=%ld noise_rms=%.3f seed=%llu clipped=%ld\n", JS8_FS, nper, total, sigma, (unsigned long long)seed, clips);
    fprintf(fp, "# S period f0_hz dt_s amp snr_db_2500 i3 crc hex22 text12 tones79\n");
    for (i = 0; i < nsig; i++) {
        sig_t *g = &sigs[i];
        char txt[13];
        int i3;
        js8_frame_text(g->frame, txt, &i3); txt[12] = 0;
        fprintf(fp, "S %d %.3f %+.4f %.2f %.2f %d %03x ", g->period, g->f0, g->dt, g->amp, g->snr, i3, js8_crc12_extract(g->frame));
        for (k = 0; k < 11; k++) fprintf(fp, "%02x", g->frame[k]);
        fprintf(fp, " %s ", txt);
        for (k = 0; k < 79; k++) fputc('0' + g->tones[k], fp);
        fputc('\n', fp);
    }
    fclose(fp);
    printf("js8gen: %s  %d period(s), %ld samples, %d signal(s), noise rms %.1f, clipped %ld\n", out, nper, total, nsig, sigma, clips);
    if (clips) fprintf(stderr, "js8gen: warning: %ld samples clipped\n", clips);
    free(buf);
    return 0;
}
