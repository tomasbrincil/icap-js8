/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8sweep - HOST test driver for js8dec.c: synthesizes JS8 Normal periods in memory (same modulator
 * as tests/js8gen.c: plain CPFSK, analytic phase, white Gaussian noise, s16 quantisation) and decodes
 * them through js8_decode() with a memory-backed dsrc_t (capture = first 116800 samples of the period,
 * nominal start 0.5 s, arena 245760 B, like the radio shell).
 *
 * SNR = signal power / noise power in 2500 Hz, noise RMS 300 (s16 units) over the 4 kHz band.
 *
 *   js8sweep clean [-f LO,HI,STEP] [-N sigma]   T2: one signal per period, f0 swept, dt cycling through
 *                                               -2.0..+2.0 s in 0.05 s steps; check frame, |df| <= 1 Hz,
 *                                               |ddt| <= 0.05 s
 *   js8sweep awgn -s LO,HI,STEP -n TRIALS       T3: one random frame per trial, random f0 (-f LO,HI) and
 *                                               dt (+-DT, -d), per SNR step: decodes, by pass, false decodes
 *   js8sweep multi -n TRIALS [-l LO,HI]         20 signals per period: 3 pairs 6.25 Hz apart, 2 pairs 50 Hz
 *                                               apart, 10 singles; levels uniform in LO..HI dB
 *   js8sweep noise -n PERIODS                   noise only: every decode is a false decode
 *                -c SYMS                        awgn: the first SYMS symbols of the frame are not transmitted
 * Common: -S seed, -T k=v,.. (decoder knobs), -b LO,HI (search band, default 200,3000), -v (per-trial lines),
 *         -W BYTES (work arena, default 245760; the radio app has less, see "make apparena"),
 *         -B MS (the app's budget= : end the decode when the MODELLED radio time, js8synth.h sy_model_ms, passes MS)
 *         -z POS (streaming front end as in the radio app: waterfall rows from capture sample POS on are built before
 *         js8_decode() is called; 0 = the whole first pass; the results must not change)
 * Output: "S ..." summary lines (one per step), "F ..." for every false decode, "X ..." for every failure
 * of the clean test, "#" comments.
 */
#include "../src/js8dec.h"
#include "../src/js8_constants.h"
#include "../src/js8crc.h"
#include "../src/js8enc.h"
#include "js8synth.h"

#define PERIOD 120000
#define CAP 116800
#define SIGMA 300.0

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
static float flo = 200, fhi = 3000;
static uint32_t arena_len = 245760;              /* -W: work arena (the radio app has less: 245760 - the shell's reserve) */
static js8_result_t res[64];
static double t_ms, budget_ms;
static long stream_pos = -1;                     /* -s: streaming front end from this capture sample */
static int16_t cap_get(uint32_t i) { return pcm[i]; }
static int over_budget(void) { return sy_model_ms(&js8_stats) > budget_ms; }
static int decode(void)
{
    dsrc_t src = { 0, src_read, CAP, SY_FS };
    sy_stream(arena, arena_len, CAP, flo, fhi, 0.5f, stream_pos, cap_get);
    double t0 = sy_now_ms();
    int nd = js8_decode(&src, flo, fhi, 0.5f, res, 64, arena, arena_len, budget_ms > 0 ? over_budget : 0);
    t_ms = sy_now_ms() - t0;
    return nd < 0 && budget_ms > 0 ? 0 : nd;     /* cut by the budget before anything was decoded */
}
static void rand_frame(uint8_t f[11])
{
    for (int i = 0; i < 11; i++) f[i] = (uint8_t)(sy_rng64() >> 24);
    f[9] &= 0xE0; f[10] = 0;
    js8_crc12_store(f, js8_crc12(f));
}
static void clear(void) { memset(fbuf, 0, sizeof fbuf); }
static void add(const uint8_t f[11], double f0, double dt, double amp)
{
    uint8_t tones[79];
    js8_encode(f, js8_costas_normal, tones);
    sy_add_cpfsk(fbuf, PERIOD, tones, f0, (0.5 + dt) * SY_FS, amp);
}
static void finish(double sigma) { sy_add_noise(fbuf, PERIOD, sigma); sy_quantize(fbuf, pcm, PERIOD); }
static void hex(const uint8_t *f) { for (int j = 0; j < 11; j++) printf("%02x", f[j]); }

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    double s_lo = -24, s_hi = -10, s_step = 2, f_lo = 500, f_hi = 2500, f_step = 1.3, dtmax = 0.8, l_lo = -18, l_hi = 5, nsig = -1;
    int trials = 100, verbose = 0;
    uint64_t seed = 1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) sscanf(argv[++i], "%lf,%lf,%lf", &s_lo, &s_hi, &s_step);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) sscanf(argv[++i], "%lf,%lf,%lf", &f_lo, &f_hi, &f_step);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) sscanf(argv[++i], "%lf,%lf", &l_lo, &l_hi);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) sscanf(argv[++i], "%f,%f", &flo, &fhi);
        else if (!strcmp(argv[i], "-W") && i + 1 < argc) arena_len = (uint32_t)atol(argv[++i]) & ~7u;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dtmax = atof(argv[++i]);
        else if (!strcmp(argv[i], "-N") && i + 1 < argc) nsig = atof(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) sy_cut_syms = atof(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) trials = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-S") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) { if (sy_tune(argv[++i])) return 2; }
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-B") && i + 1 < argc) { budget_ms = atof(argv[++i]); sy_budget(budget_ms); }
        else if (!strcmp(argv[i], "-z") && i + 1 < argc) stream_pos = atol(argv[++i]);
        else { fprintf(stderr, "js8sweep: bad option %s\n", argv[i]); return 2; }
    }
    sy_rng_s = seed;
    arena = malloc(arena_len + 8);

    if (!strcmp(mode, "clean")) {
        double sigma = nsig < 0 ? 0 : nsig, amp = 8000;
        int n = 0, ok = 0, fbad = 0, tbad = 0, miss = 0, extra = 0, k = 0;
        double maxdf = 0, maxdt = 0, ms = 0;
        int by_dt[81][2]; memset(by_dt, 0, sizeof by_dt);
        for (double f0 = f_lo; f0 <= f_hi + 1e-9; f0 += f_step, k++) {
            int di = (k * 37) % 81;                    /* all 81 dt steps, in a scrambled order */
            double dt = -2.0 + 0.05 * di;
            uint8_t fr[11];
            rand_frame(fr); clear(); add(fr, f0, dt, amp); finish(sigma);
            int nd = decode(), hit = -1;
            for (int i = 0; i < nd; i++) if (!memcmp(res[i].frame, fr, 11)) hit = i; else { extra++; printf("F clean f0=%.1f dt=%+.2f got ", f0, dt); hex(res[i].frame); printf("\n"); }
            n++; ms += t_ms; by_dt[di][0]++;
            if (hit < 0) { miss++; printf("X miss f0=%.2f dt=%+.2f\n", f0, dt); continue; }
            double df = fabs((double)res[hit].freq_hz - f0), dd = fabs((double)res[hit].dt_s - dt);
            if (df > maxdf) maxdf = df;
            if (dd > maxdt) maxdt = dd;
            int good = 1;
            if (df > 1.0) { fbad++; good = 0; printf("X freq f0=%.2f dt=%+.2f got %.2f\n", f0, dt, (double)res[hit].freq_hz); }
            if (dd > 0.05) { tbad++; good = 0; printf("X time f0=%.2f dt=%+.2f got %+.3f\n", f0, dt, (double)res[hit].dt_s); }
            if (good) { ok++; by_dt[di][1]++; }
            if (verbose) printf("t %.2f %+.2f %.2f %+.3f %.0f p%d\n", f0, dt, (double)res[hit].freq_hz, (double)res[hit].dt_s, (double)res[hit].snr_db, res[hit].pass);
        }
        for (int d = 0; d < 81; d++) printf("D dt=%+.2f n=%d ok=%d\n", -2.0 + 0.05 * d, by_dt[d][0], by_dt[d][1]);
        printf("S clean sigma=%.0f n=%d ok=%d miss=%d freq_bad=%d time_bad=%d extra=%d max_df=%.3f max_ddt=%.4f ms=%.1f\n", sigma, n, ok, miss, fbad, tbad, extra, maxdf, maxdt, ms / n);
        return ok == n && !extra ? 0 : 1;
    }
    if (!strcmp(mode, "awgn")) {
        for (double snr = s_lo; snr <= s_hi + 1e-9; snr += s_step) {
            int ok = 0, nfalse = 0, bypass[16]; double se = 0, ms = 0, sdf = 0, sdt = 0; uint32_t bp = 0;
            memset(bypass, 0, sizeof bypass);
            for (int t = 0; t < trials; t++) {
                uint8_t fr[11];
                double f0 = f_lo + (f_hi - f_lo) * sy_u(), dt = dtmax * (2 * sy_u() - 1);
                rand_frame(fr); clear(); add(fr, f0, dt, sy_amp_for_snr(snr, SIGMA)); finish(SIGMA);
                int nd = decode(), hit = -1;
                for (int i = 0; i < nd; i++) if (!memcmp(res[i].frame, fr, 11)) hit = i; else { nfalse++; printf("F awgn snr=%.0f got ", snr); hex(res[i].frame); printf(" f=%.1f dt=%+.2f p%d nh=%d ns=%d\n", (double)res[i].freq_hz, (double)res[i].dt_s, res[i].pass, res[i].nhard, res[i].nsync); }
                ms += t_ms; bp += js8_stats.nbp;
                if (hit >= 0) {
                    ok++; bypass[res[hit].pass & 15]++; se += (double)res[hit].snr_db;
                    sdf += fabs((double)res[hit].freq_hz - f0); sdt += fabs((double)res[hit].dt_s - dt);
                }
                if (verbose) printf("t %.0f %d %.1f %+.2f\n", snr, hit >= 0, f0, dt);
            }
            printf("S awgn snr=%.0f n=%d ok=%d p1=%d p2=%d p3=%d p4=%d p5=%d osd=%d false=%d snr_est=%.2f df=%.2f ddt=%.3f ms=%.1f bp=%.1f\n", snr, trials, ok,
                   bypass[1], bypass[2], bypass[3], bypass[4], bypass[5], bypass[9] + bypass[10] + bypass[11] + bypass[12] + bypass[13], nfalse,
                   ok ? se / ok : 0.0, ok ? sdf / ok : 0.0, ok ? sdt / ok : 0.0, ms / trials, (double)bp / trials);
            fflush(stdout);
        }
        return 0;
    }
    if (!strcmp(mode, "multi")) {
        int tot[3] = { 0, 0, 0 }, got[3] = { 0, 0, 0 }, nfalse = 0; double ms = 0, msmax = 0;
        for (int t = 0; t < trials; t++) {
            uint8_t fr[20][11]; double f0[20]; int cls[20];
            /* 16 slots of 125 Hz from 500 Hz: 10 singles, 3 close pairs (6.25 Hz), 2 adjacent pairs (50 Hz), 1 empty */
            int slot[16], n = 0;
            for (int i = 0; i < 16; i++) slot[i] = i;
            for (int i = 15; i > 0; i--) { int j = (int)(sy_rng64() % (uint64_t)(i + 1)), x = slot[i]; slot[i] = slot[j]; slot[j] = x; }
            clear();
            for (int i = 0; i < 15; i++) {
                double base = 500.0 + 125.0 * slot[i] + 10.0 * sy_u();
                int kind = i < 10 ? 0 : i < 13 ? 1 : 2;
                f0[n] = base; cls[n] = kind; n++;
                if (kind) { f0[n] = base + (kind == 1 ? 6.25 : 50.0); cls[n] = kind; n++; }
            }
            for (int i = 0; i < n; i++) {
                double lvl = l_lo + (l_hi - l_lo) * sy_u(), dt = 0.5 * (2 * sy_u() - 1);
                rand_frame(fr[i]); add(fr[i], f0[i], dt, sy_amp_for_snr(lvl, SIGMA));
                tot[cls[i]]++;
            }
            finish(SIGMA);
            int nd = decode();
            ms += t_ms; if (t_ms > msmax) msmax = t_ms;
            for (int i = 0; i < nd; i++) {
                int hit = -1;
                for (int j = 0; j < n; j++) if (!memcmp(res[i].frame, fr[j], 11)) hit = j;
                if (hit < 0) { nfalse++; printf("F multi got "); hex(res[i].frame); printf(" f=%.1f\n", (double)res[i].freq_hz); }
                else got[cls[hit]]++;
            }
        }
        printf("S multi levels=%.0f..%.0f periods=%d single=%d/%d pair6.25=%d/%d pair50=%d/%d all=%d/%d false=%d ms=%.1f ms_max=%.1f\n", l_lo, l_hi, trials,
               got[0], tot[0], got[1], tot[1], got[2], tot[2], got[0] + got[1] + got[2], tot[0] + tot[1] + tot[2], nfalse, ms / trials, msmax);
        return 0;
    }
    if (!strcmp(mode, "noise")) {
        int nfalse = 0; double ms = 0, msmax = 0; uint64_t cand = 0, tried = 0, bp = 0;
        for (int t = 0; t < trials; t++) {
            clear(); finish(SIGMA);
            int nd = decode();
            ms += t_ms; if (t_ms > msmax) msmax = t_ms;
            cand += js8_stats.ncand; tried += js8_stats.ntried; bp += js8_stats.nbp;
            for (int i = 0; i < nd; i++) { nfalse++; printf("F noise got "); hex(res[i].frame); printf(" f=%.1f dt=%+.2f p%d nh=%d ns=%d q=%d\n", (double)res[i].freq_hz, (double)res[i].dt_s, res[i].pass, res[i].nhard, res[i].nsync, res[i].quality); }
        }
        printf("S noise periods=%d false=%d cand=%.1f tried=%.1f bp=%.1f ms=%.1f ms_max=%.1f\n", trials, nfalse, (double)cand / trials, (double)tried / trials, (double)bp / trials, ms / trials, msmax);
        return 0;
    }
    fprintf(stderr, "usage: js8sweep clean|awgn|multi|noise [options] (see the header of tests/js8sweep.c)\n");
    return 2;
}
