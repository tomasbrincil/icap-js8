/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8prof - HOST profiler of js8_decode(): where does a period's work go?  Uses the decoder's own stage
 * clock (js8_clock, here microseconds of CLOCK_MONOTONIC) and its deterministic operation counters
 * (js8_stats), the radio app's arena and band by default (189824 B, 450..2550 Hz), the shell's read
 * contract (s16 -> float in 512-sample pieces) and a polled abort callback.
 *
 * usage: js8prof [options] SCENARIO...
 *   file:PATH[@T0]     a recording (8 kHz mono s16); period edges at T0 + k * 15 s (default 0)
 *   multi:N[:LO,HI]    N synthetic periods of 20 signals as "js8sweep multi" (3 pairs 6.25 Hz apart, 2 pairs
 *                      50 Hz apart, 10 singles), levels uniform in LO..HI dB (default -18,0), white noise
 *   noise:N            N periods of white Gaussian noise (RMS 300), no signal
 *   empty:N            N periods of an empty band as a receiver gives it: noise shaped by an SSB filter
 *                      (300..2700 Hz, RMS 300 in the passband), no signal
 *   silence:N          N periods of digital silence (+-1 LSB dither)
 * options:
 *   -b LO,HI   tone-0 search range (default 450,2550)      -W BYTES  arena (default 189824)
 *   -T k=v,..  decoder knobs (js8_tune_t)                   -S SEED   seed of the synthetic scenarios
 *   -r REPS    decode every period REPS times, keep the fastest run (default 5; the counters are identical)
 *   -s POS     streaming front end (as the radio app): rows from capture sample POS on are made before the decode;
 *              -s auto: POS follows from the modelled decode time of the period before (first period of a run: 0)
 *   -B MS      budget: abort the decode when the MODELLED radio time (see -K) passes MS (0 = off)
 *   -K list    radio model, ms per operation: fft,score,iter,subch,ksamp,llr,poll,fixed
 *              (per FFT row, per 1000 sync scores, per BP iteration, per signal x row subtracted, per 1000
 *              samples read, per bit-metric vector, per abort poll, per period); default = SY_MODEL
 *   -v         one line per period
 * Output: a table per scenario (stage: ms per period on this PC, share; pass kinds; counters per period;
 * modelled radio ms: mean and max), and one "M" line per scenario for scripts.
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
static const int16_t *g_s; static long g_n, g_cap0;      /* file scenario */
static int from_file;
static int src_read(void *ctx, uint32_t start, float *out, uint32_t n)
{
    (void)ctx;
    if (start >= CAP) return 0;
    if (start + n > CAP) n = CAP - start;
    uint32_t got = 0;
    while (got < n) {
        uint32_t k = n - got; if (k > 512) k = 512;
        for (uint32_t i = 0; i < k; i++) {
            int16_t v;
            if (from_file) { long idx = g_cap0 + (long)start + (long)got + (long)i; v = idx >= 0 && idx < g_n ? g_s[idx] : 0; }
            else v = pcm[start + got + i];
            out[got + i] = (float)v * (1.0f / 32768.0f);
        }
        got += k;
    }
    return (int)got;
}
static uint32_t clk(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u);
}
static double prev_model;
static long stream_pos = -1;                             /* -s: streaming front end from this capture sample */
static int16_t cap_get(uint32_t i) { if (from_file) { long idx = g_cap0 + (long)i; return idx >= 0 && idx < g_n ? g_s[idx] : 0; } return pcm[i]; }
static double budget;
static int cut;
static int dec_abort(void)
{
    if (budget > 0 && sy_model_ms(&js8_stats) > budget) { cut = 1; return 1; }
    return 0;
}

static void rand_frame(uint8_t f[11])
{
    for (int i = 0; i < 11; i++) f[i] = (uint8_t)(sy_rng64() >> 24);
    f[9] &= 0xE0; f[10] = 0;
    js8_crc12_store(f, js8_crc12(f));
}
static void add(const uint8_t f[11], double f0, double dt, double amp)
{
    uint8_t tones[79];
    js8_encode(f, js8_costas_normal, tones);
    sy_add_cpfsk(fbuf, PERIOD, tones, f0, (0.5 + dt) * SY_FS, amp);
}
/* windowed-sinc band-pass 300..2700 Hz (255 taps, Hamming); gain 1 in the passband */
static void ssb_filter(double *x, long n)
{
    enum { NT = 255 };
    static double h[NT]; static int have;
    static double y[PERIOD];
    if (!have) {
        for (int i = 0; i < NT; i++) {
            double t = i - (NT - 1) / 2.0, w = 0.54 - 0.46 * cos(SY_TWO_PI * i / (NT - 1));
            double a = t == 0 ? 2.0 * 2700.0 / SY_FS : sin(SY_TWO_PI * 2700.0 * t / SY_FS) / (3.141592653589793 * t);
            double b = t == 0 ? 2.0 * 300.0 / SY_FS : sin(SY_TWO_PI * 300.0 * t / SY_FS) / (3.141592653589793 * t);
            h[i] = (a - b) * w;
        }
        have = 1;
    }
    for (long i = 0; i < n; i++) {
        double s = 0;
        for (int k = 0; k < NT; k++) { long j = i - k + (NT - 1) / 2; if (j >= 0 && j < n) s += h[k] * x[j]; }
        y[i] = s;
    }
    memcpy(x, y, (size_t)n * sizeof *x);
}

/* accumulated over the periods of a scenario */
static struct {
    int nper, ndec, ncut;
    double t[JS8_T_N], tk[3], total, model, model_max, host_max;
    double c_ncand, c_ntried, c_nbp, c_iters, c_nsamp, c_nscore, c_nsubch, c_nfft, c_nllr, c_npoll, c_nsig, c_nwf[3], c_nband;
    double bpn[5], bpok[5], bpit[5];
} A;
static void *arena; static uint32_t arena_len = 189824;
static float flo = 450, fhi = 2550;
static int reps = 5, verbose;
static const char *stage_name[JS8_T_N] = { "capture read", "subtract (corr+sub)", "STFT: window+FFT", "STFT: power->dB, baseline",
    "sync search", "candidates: sync check+LLR", "BP pass 1", "BP pass 2", "BP pass 3 (24 erased)", "BP pass 4 (48 erased)", "BP pass 5 (interp.)",
    "decoded: re-encode, refine", "abort poll" };

static void one_period(int idx)
{
    static js8_result_t res[64];
    dsrc_t src = { 0, src_read, CAP, SY_FS };
    js8_stats_t best; double best_tot = 1e30; int nd = 0, bcut = 0;
    memset(&best, 0, sizeof best);
    for (int r = 0; r < reps; r++) {
        cut = 0;
        /* -s auto: as on the radio - the stream starts when the decode of the period before has returned
         * (the capture starts 0.4 s after the earliest start of that decode) */
        long sp = stream_pos != -2 ? stream_pos : prev_model > 400.0 ? (long)((prev_model - 400.0) * 8.0) : 0;
        sy_stream(arena, arena_len, CAP, flo, fhi, 0.5f, sp, cap_get);
        int n = js8_decode(&src, flo, fhi, 0.5f, res, 50, arena, arena_len, dec_abort);
        double tot = 0;
        for (int i = 0; i < JS8_T_N; i++) tot += js8_stats.t[i];
        if (tot < best_tot) { best_tot = tot; best = js8_stats; }
        nd = n; bcut = cut;
    }
    if (nd < 0 && !bcut) { printf("# period %d: js8_decode returned %d\n", idx, nd); return; }
    if (nd < 0) nd = 0;
    double model = sy_model_ms(&best);
    prev_model = model;
    A.nper++; A.ndec += nd; A.ncut += bcut; A.total += best_tot / 1000.0; A.model += model;
    if (model > A.model_max) A.model_max = model;
    if (best_tot / 1000.0 > A.host_max) A.host_max = best_tot / 1000.0;
    for (int i = 0; i < JS8_T_N; i++) A.t[i] += best.t[i] / 1000.0;
    for (int i = 0; i < 3; i++) { A.tk[i] += best.tk[i] / 1000.0; A.c_nwf[i] += best.nwf[i]; }
    for (int i = 0; i < 5; i++) { A.bpn[i] += best.bpn[i]; A.bpok[i] += best.bpok[i]; A.bpit[i] += best.bpit[i]; }
    A.c_ncand += best.ncand; A.c_ntried += best.ntried; A.c_nbp += best.nbp; A.c_iters += best.bp_iters; A.c_nsamp += 640.0 * best.nrdch;
    A.c_nscore += best.nscore; A.c_nsubch += best.nsubch; A.c_nfft += best.nfft; A.c_nllr += best.nllr; A.c_npoll += best.npoll;
    A.c_nsig += best.nsig; A.c_nband += best.nband;
    if (verbose)
        printf("P %d dec=%d%s host_ms=%.1f model_ms=%.0f wf=%d/%d/%d fft=%u score=%u cand=%u tried=%u bp=%u iters=%u subch=%u sig=%u\n", idx, nd, bcut ? " CUT" : "",
               best_tot / 1000.0, model, best.nwf[0], best.nwf[1], best.nwf[2], best.nfft, best.nscore, best.ncand, best.ntried, best.nbp, best.bp_iters, best.nsubch, best.nsig);
}
static void report(const char *name)
{
    double n = A.nper ? A.nper : 1;
    printf("\n== %s: %d periods, %d frames decoded (%.2f per period)%s\n", name, A.nper, A.ndec, A.ndec / n, budget > 0 ? " [budget active]" : "");
    printf("   %-28s %9s %7s\n", "stage", "ms/period", "share");
    for (int i = 0; i < JS8_T_N; i++) printf("   %-28s %9.2f %6.1f%%\n", stage_name[i], A.t[i] / n, A.total > 0 ? 100.0 * A.t[i] / A.total : 0.0);
    printf("   %-28s %9.2f %6.1f%%   (max %.1f)\n", "TOTAL js8_decode (this PC)", A.total / n, 100.0, A.host_max);
    printf("   by pass: first %.2f ms (%.1f%%, %.2f waterfalls)  after-subtraction %.2f ms (%.1f%%, %.2f wf)  shifted grid %.2f ms (%.1f%%, %.2f wf); band passes %.2f\n",
           A.tk[0] / n, 100.0 * A.tk[0] / (A.total > 0 ? A.total : 1), A.c_nwf[0] / n, A.tk[1] / n, 100.0 * A.tk[1] / (A.total > 0 ? A.total : 1), A.c_nwf[1] / n,
           A.tk[2] / n, 100.0 * A.tk[2] / (A.total > 0 ? A.total : 1), A.c_nwf[2] / n, A.c_nband / n);
    printf("   per period: FFT rows %.0f, samples read %.0f (%.0f bytes of s16), sync scores %.0f, candidates %.1f, tried %.1f, LLR vectors %.1f,\n"
           "               BP runs %.1f, BP iterations %.0f, signal x row subtractions %.0f, signals %.2f, abort polls %.0f\n",
           A.c_nfft / n, A.c_nsamp / n, 2.0 * A.c_nsamp / n, A.c_nscore / n, A.c_ncand / n, A.c_ntried / n, A.c_nllr / n, A.c_nbp / n, A.c_iters / n, A.c_nsubch / n, A.c_nsig / n, A.c_npoll / n);
    printf("   BP by pass (runs / iterations / frames accepted per period):");
    for (int i = 0; i < 5; i++) printf("  p%d %.1f/%.0f/%.2f", i + 1, A.bpn[i] / n, A.bpit[i] / n, A.bpok[i] / n);
    printf("\n   modelled radio time: mean %.0f ms, max %.0f ms; cut by the budget: %d of %d periods\n", A.model / n, A.model_max, A.ncut, A.nper);
    printf("M name=%s periods=%d dec=%d cut=%d host_ms=%.3f host_max=%.3f model_ms=%.1f model_max=%.1f", name, A.nper, A.ndec, A.ncut, A.total / n, A.host_max, A.model / n, A.model_max);
    static const char *sk[JS8_T_N] = { "read", "sub", "fft", "mag", "search", "llr", "bp1", "bp2", "bp3", "bp4", "bp5", "fin", "poll" };
    for (int i = 0; i < JS8_T_N; i++) printf(" t_%s=%.3f", sk[i], A.t[i] / n);
    printf(" nfft=%.1f nsamp=%.0f nscore=%.0f ncand=%.1f ntried=%.1f nllr=%.1f nbp=%.1f iters=%.1f nsubch=%.1f nsig=%.2f npoll=%.1f wf=%.2f/%.2f/%.2f\n",
           A.c_nfft / n, A.c_nsamp / n, A.c_nscore / n, A.c_ncand / n, A.c_ntried / n, A.c_nllr / n, A.c_nbp / n, A.c_iters / n, A.c_nsubch / n, A.c_nsig / n, A.c_npoll / n,
           A.c_nwf[0] / n, A.c_nwf[1] / n, A.c_nwf[2] / n);
    memset(&A, 0, sizeof A); prev_model = 0;
}

int main(int argc, char **argv)
{
    uint64_t seed = 20261123;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) { if (sscanf(argv[++i], "%f,%f", &flo, &fhi) != 2) return 2; }
        else if (!strcmp(argv[i], "-W") && i + 1 < argc) arena_len = (uint32_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) { if (sy_tune(argv[++i])) return 2; }
        else if (!strcmp(argv[i], "-S") && i + 1 < argc) seed = strtoull(argv[++i], 0, 10);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-B") && i + 1 < argc) budget = atof(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) { i++; stream_pos = !strcmp(argv[i], "auto") ? -2 : atol(argv[i]); }
        else if (!strcmp(argv[i], "-K") && i + 1 < argc) { if (sy_model_set(argv[++i])) return 2; }
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else { fprintf(stderr, "see the header of tests/js8prof.c\n"); return 2; }
    }
    if (i >= argc) { fprintf(stderr, "usage: js8prof [options] file:PATH[@T0] | multi:N[:LO,HI] | noise:N | empty:N | silence:N ...\n"); return 2; }
    if (reps < 1) reps = 1;
    if (posix_memalign(&arena, 64, arena_len)) return 2;
    js8_clock = clk;
    if (budget > 0) sy_budget(budget);          /* js8_tune.budget on the modelled clock: the stage "times" are then modelled ms */
    printf("# js8prof: band %.0f..%.0f Hz, arena %u B, %d runs per period (fastest kept), tune: max_cand=%d min_score=%d passmask=%d iters=%d subpasses=%d shift=%d nsync_min=%d\n",
           (double)flo, (double)fhi, arena_len, reps, js8_tune.max_cand, js8_tune.min_score, js8_tune.passmask, js8_tune.iters, js8_tune.subpasses, js8_tune.shift, js8_tune.nsync_min);
    printf("# radio model (ms): %s\n", sy_model_str());
    for (; i < argc; i++) {
        const char *a = argv[i];
        sy_rng_s = seed;
        if (!strncmp(a, "file:", 5)) {
            char path[1024]; double t0 = 0; const char *at = strrchr(a, '@');
            size_t pl = at ? (size_t)(at - (a + 5)) : strlen(a + 5);
            if (pl >= sizeof path) return 2;
            memcpy(path, a + 5, pl); path[pl] = 0;
            if (at) t0 = atof(at + 1);
            g_s = sy_load(path, &g_n); if (!g_s) return 2;
            from_file = 1;
            long e0 = (long)floor(t0 * SY_FS + 0.5); int idx = 0;
            while (e0 > 0) e0 -= PERIOD;
            if (e0 + CAP <= 0) e0 += PERIOD;
            for (; e0 < g_n; e0 += PERIOD, idx++) {
                if (g_n - e0 < PERIOD / 4 && idx > 0) break;
                g_cap0 = e0; one_period(idx);
            }
            from_file = 0;
            const char *bn = strrchr(path, '/');
            report(bn ? bn + 1 : path);
            continue;
        }
        char kind[16]; int n = 0; double l_lo = -18, l_hi = 0;
        const char *col = strchr(a, ':');
        size_t kl = col ? (size_t)(col - a) : strlen(a);
        if (kl >= sizeof kind) return 2;
        memcpy(kind, a, kl); kind[kl] = 0;
        if (col) { n = atoi(col + 1); const char *c2 = strchr(col + 1, ':'); if (c2 && sscanf(c2 + 1, "%lf,%lf", &l_lo, &l_hi) != 2) return 2; }
        if (n < 1) n = 1;
        for (int t = 0; t < n; t++) {
            memset(fbuf, 0, sizeof fbuf);
            if (!strcmp(kind, "multi")) {
                uint8_t fr[20][11]; double f0[20]; int ns = 0, slot[16];
                for (int k = 0; k < 16; k++) slot[k] = k;
                for (int k = 15; k > 0; k--) { int j = (int)(sy_rng64() % (uint64_t)(k + 1)), x = slot[k]; slot[k] = slot[j]; slot[j] = x; }
                for (int k = 0; k < 15; k++) {
                    double base = 500.0 + 125.0 * slot[k] + 10.0 * sy_u();
                    int kd = k < 10 ? 0 : k < 13 ? 1 : 2;
                    f0[ns++] = base;
                    if (kd) f0[ns++] = base + (kd == 1 ? 6.25 : 50.0);
                }
                for (int k = 0; k < ns; k++) {
                    double lvl = l_lo + (l_hi - l_lo) * sy_u(), dt = 0.5 * (2 * sy_u() - 1);
                    rand_frame(fr[k]); add(fr[k], f0[k], dt, sy_amp_for_snr(lvl, SIGMA));
                }
                sy_add_noise(fbuf, PERIOD, SIGMA);
            } else if (!strcmp(kind, "noise")) sy_add_noise(fbuf, PERIOD, SIGMA);
            else if (!strcmp(kind, "empty")) { sy_add_noise(fbuf, PERIOD, SIGMA * sqrt(4000.0 / 2400.0)); ssb_filter(fbuf, PERIOD); }
            else if (!strcmp(kind, "silence")) { for (long k = 0; k < PERIOD; k++) fbuf[k] = sy_u() - 0.5; }
            else { fprintf(stderr, "unknown scenario %s\n", a); return 2; }
            sy_quantize(fbuf, pcm, PERIOD);
            one_period(t);
        }
        report(a);
    }
    return 0;
}
