/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * ft8ref - HOST reference bench: the UNMODIFIED FT8 decoder and encoder of the ICAP SDK
 * (apps/ft8/ft8dec.c, ft8enc.c, vendored ft8_lib, apps/digi-common/txsynth.c, msg77.c; compiled from
 * the SDK checkout, nothing copied here) next to js8dec.c:
 *
 *   ft8ref cross -t PERIODS -n N [-l LO,HI]   FT8-only audio (N real FT8 signals per period from the SDK
 *                                             encoder, GFSK, random CQ messages, + noise) through BOTH
 *                                             decoders: JS8 decodes here are false decodes; the FT8 decode
 *                                             count shows that the audio is valid FT8
 *   ft8ref load  -t PERIODS -n N [-l LO,HI]   comparable load: the same layout (frequencies, dt, levels,
 *                                             noise seed) once as N FT8 signals -> ft8_decode and once as
 *                                             N JS8 signals -> js8_decode; decode time of both
 *   ft8ref noise -t PERIODS                   the same noise-only period through both decoders (time)
 *   ft8ref gen -o FILE -p PERIODS -n N        write FT8-only audio as WAV (for js8wav)
 * Common: -S seed, -l LO,HI (levels, dB in 2500 Hz, default -18,5), -T k=v (JS8 knobs), -b LO,HI (band).
 * Both decoders get the band 200..3000 Hz, a 116800-sample capture, nominal start 0.5 s; arena 245760 B
 * for JS8 and 245760 - sizeof(ft8_state_t) for FT8 (as the shell does). Noise RMS 300.
 */
#include "ft8dec.h"
#include "ft8enc.h"
#include "../src/js8dec.h"
#include "../src/js8_constants.h"
#include "../src/js8crc.h"
#include "../src/js8enc.h"
#include "js8synth.h"

#define PERIOD 120000
#define CAP 116800
#define SIGMA 300.0
#define MAXN 64

static int16_t pcm[PERIOD];
static double fbuf[PERIOD];
static int src_read(void *ctx, uint32_t start, float *out, uint32_t n)
{
    (void)ctx;
    if (start >= CAP) return 0;
    if (start + n > CAP) n = CAP - start;
    for (uint32_t i = 0; i < n; i++) out[i] = (float)pcm[start + i] * (1.0f / 32768.0f);
    return (int)n;
}
static void *arena;
static ft8_state_t *hash;
static float flo = 200, fhi = 3000;
static js8_result_t jres[64];
static ft8_result_t fres[64];
static double t_ms;
static int dec_js8(void)
{
    dsrc_t src = { 0, src_read, CAP, SY_FS };
    double t0 = sy_now_ms();
    int nd = js8_decode(&src, flo, fhi, 0.5f, jres, 50, arena, 245760, 0);
    t_ms = sy_now_ms() - t0;
    return nd;
}
static int dec_ft8(void)
{
    dsrc_t src = { 0, src_read, CAP, SY_FS };
    uint32_t sb = ((uint32_t)sizeof(ft8_state_t) + 7u) & ~7u;
    double t0 = sy_now_ms();
    int nd = ft8_decode(&src, flo, fhi, 0.5f, fres, 50, arena, 245760 - sb, 0, hash);
    t_ms = sy_now_ms() - t0;
    return nd;
}
static void rand_msg(char *m)
{
    static const char L[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char *P[] = { "OK", "DL", "SP", "OM", "OE", "HA", "PA", "ON" };     /* always a valid standard call */
    sprintf(m, "CQ %s%d%c%c%c %c%c%d%d", P[sy_rng64() % 8], (int)(sy_rng64() % 10), L[sy_rng64() % 26], L[sy_rng64() % 26], L[sy_rng64() % 26],
            L[sy_rng64() % 18], L[sy_rng64() % 18], (int)(sy_rng64() % 10), (int)(sy_rng64() % 10));
}
static int add_ft8(const char *msg, double f0, double dt, double amp)
{
    static ft8_tx_t tx; static int16_t tmp[FT8_TX_SAMPLES];
    if (ft8_tx_init(&tx, msg, f0, 20000) != 0) return -1;
    int n = 0, k;
    while (n < (int)FT8_TX_SAMPLES && (k = ft8_tx_fill(&tx, tmp + n, (int)FT8_TX_SAMPLES - n)) > 0) n += k;
    long t0 = (long)floor((0.5 + dt) * SY_FS + 0.5);
    for (int i = 0; i < n; i++) { long j = t0 + i; if (j >= 0 && j < PERIOD) fbuf[j] += (double)tmp[i] * amp / 20000.0; }
    return 0;
}
static void add_js8(double f0, double dt, double amp)
{
    uint8_t f[11], tones[79];
    for (int i = 0; i < 11; i++) f[i] = (uint8_t)(sy_rng64() >> 24);
    f[9] &= 0xE0; f[10] = 0;
    js8_crc12_store(f, js8_crc12(f));
    js8_encode(f, js8_costas_normal, tones);
    sy_add_cpfsk(fbuf, PERIOD, tones, f0, (0.5 + dt) * SY_FS, amp);
}
typedef struct { double f0, dt, lvl; char msg[24]; } lay_t;
static void layout(lay_t *l, int n, double l_lo, double l_hi)
{
    for (int i = 0; i < n; i++) {
        l[i].f0 = 300.0 + 2500.0 * sy_u(); l[i].dt = 0.5 * (2 * sy_u() - 1); l[i].lvl = l_lo + (l_hi - l_lo) * sy_u();
        rand_msg(l[i].msg);
    }
}
static void synth_ft8(const lay_t *l, int n)
{
    memset(fbuf, 0, sizeof fbuf);
    for (int i = 0; i < n; i++) if (add_ft8(l[i].msg, l[i].f0, l[i].dt, sy_amp_for_snr(l[i].lvl, SIGMA))) { fprintf(stderr, "ft8ref: cannot encode %s\n", l[i].msg); exit(1); }
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "", *out = NULL;
    int trials = 20, n = 20, periods = 4;
    double l_lo = -18, l_hi = 5;
    uint64_t seed = 1;
    lay_t lay[MAXN];
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) trials = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) periods = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) sscanf(argv[++i], "%lf,%lf", &l_lo, &l_hi);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) sscanf(argv[++i], "%f,%f", &flo, &fhi);
        else if (!strcmp(argv[i], "-S") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) { if (sy_tune(argv[++i])) return 2; }
        else { fprintf(stderr, "ft8ref: bad option %s\n", argv[i]); return 2; }
    }
    if (n < 0 || n > MAXN) return 2;
    sy_rng_s = seed;
    arena = malloc(245760);
    hash = (ft8_state_t *)calloc(1, sizeof *hash);

    if (!strcmp(mode, "gen")) {
        if (!out) return 2;
        int16_t *all = (int16_t *)malloc((size_t)periods * PERIOD * 2);
        for (int p = 0; p < periods; p++) {
            layout(lay, n, l_lo, l_hi); synth_ft8(lay, n);
            sy_add_noise(fbuf, PERIOD, SIGMA); sy_quantize(fbuf, all + (long)p * PERIOD, PERIOD);
        }
        printf("# %s: %d periods, %d FT8 signals each, levels %.0f..%.0f dB, seed %llu\n", out, periods, n, l_lo, l_hi, (unsigned long long)seed);
        return sy_save_wav(out, all, (long)periods * PERIOD) ? 1 : 0;
    }
    if (!strcmp(mode, "cross")) {
        int nf = 0, nj = 0; double msf = 0, msj = 0;
        for (int t = 0; t < trials; t++) {
            layout(lay, n, l_lo, l_hi); synth_ft8(lay, n);
            sy_add_noise(fbuf, PERIOD, SIGMA); sy_quantize(fbuf, pcm, PERIOD);
            int a = dec_ft8(); msf += t_ms; if (a > 0) nf += a;
            int b = dec_js8(); msj += t_ms;
            for (int i = 0; i < b; i++) {
                nj++; printf("F ft8audio got ");
                for (int j = 0; j < 11; j++) printf("%02x", jres[i].frame[j]);
                printf(" f=%.1f dt=%+.2f p%d nh=%d ns=%d\n", (double)jres[i].freq_hz, (double)jres[i].dt_s, jres[i].pass, jres[i].nhard, jres[i].nsync);
            }
        }
        printf("S cross periods=%d ft8_sent=%d ft8_decoded=%d js8_false=%d ft8_ms=%.1f js8_ms=%.1f\n", trials, trials * n, nf, nj, msf / trials, msj / trials);
        return 0;
    }
    if (!strcmp(mode, "load") || !strcmp(mode, "noise")) {
        int nf = 0, nj = 0, isnoise = !strcmp(mode, "noise"); double msf = 0, msj = 0, mxf = 0, mxj = 0;
        if (isnoise) n = 0;
        for (int t = 0; t < trials; t++) {
            uint64_t ns;
            layout(lay, n, l_lo, l_hi);
            ns = sy_rng64();
            synth_ft8(lay, n);
            sy_rng_s = ns; sy_add_noise(fbuf, PERIOD, SIGMA); sy_quantize(fbuf, pcm, PERIOD);
            int a = dec_ft8(); msf += t_ms; if (t_ms > mxf) mxf = t_ms; if (a > 0) nf += a;
            memset(fbuf, 0, sizeof fbuf);
            sy_rng_s = ns ^ 0x5555;
            for (int i = 0; i < n; i++) add_js8(lay[i].f0, lay[i].dt, sy_amp_for_snr(lay[i].lvl, SIGMA));
            sy_rng_s = ns; sy_add_noise(fbuf, PERIOD, SIGMA); sy_quantize(fbuf, pcm, PERIOD);
            int b = dec_js8(); msj += t_ms; if (t_ms > mxj) mxj = t_ms; if (b > 0) nj += b;
            sy_rng_s = ns + 1;
        }
        printf("S %s periods=%d signals=%d ft8_decoded=%d js8_decoded=%d ft8_ms=%.1f js8_ms=%.1f ratio=%.2f ft8_ms_max=%.1f js8_ms_max=%.1f\n", mode, trials, trials * n, nf, nj,
               msf / trials, msj / trials, msj / msf, mxf, mxj);
        return 0;
    }
    fprintf(stderr, "usage: ft8ref cross|load|noise|gen [options] (see the header of tests/ft8ref.c)\n");
    return 2;
}
