/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * fuzz_dec - adversarial host test of js8_decode(): hostile audio and hostile call parameters.
 * Build with -fsanitize=address,undefined: the work area is a malloc block of EXACTLY work_len
 * bytes and the result array has exactly maxout entries, so one byte too far is caught.
 *
 *   fuzz_dec [iterations] [seed]         random cases (see below)
 *   fuzz_dec poll                        abort-poll latency: longest CPU time between two polls,
 *                                        as a share of the whole decode, on the worst loads
 *
 * Random cases: audio = noise / silence / DC / full-scale square / values far outside +-1 / NaN-free
 * garbage / up to 90 strong JS8 signals with random frames (more than MAX_SIG and maxout), length
 * 0..130000 samples, sample source returning short counts or errors, nominal start -3..+3 s,
 * f_lo/f_hi anything (also reversed, negative, above Nyquist), work_len 96..260 KiB, maxout 1..64,
 * abort at a random poll.  Checked besides the sanitizers: return value range, js8_work_peak() <=
 * work_len, every reported frame has a valid CRC-12, results finite.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "../src/js8dec.h"
#include "../src/js8_constants.h"
#include "../src/js8crc.h"
#include "../src/js8enc.h"
#include "js8synth.h"

static long fails;
#define FAIL(...) do { if (fails++ < 40) { printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static float *g_x; static uint32_t g_n; static int g_rdmode;
static int rd(void *ctx, uint32_t start, float *out, uint32_t n)
{
    (void)ctx;
    if (start >= g_n) return 0;
    if (start + n > g_n) n = g_n - start;
    if (g_rdmode == 1 && n > 3 && sy_rng64() % 7 == 0) n = (uint32_t)(sy_rng64() % n);     /* short count */
    if (g_rdmode == 2 && sy_rng64() % 400 == 0) return -1;                                 /* I/O error */
    memcpy(out, g_x + start, n * sizeof(float));
    return (int)n;
}

static long g_polls, g_abort_at;
static double g_last, g_maxgap, g_t0;
static double cpu(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6; }
static int ab(void)
{
    double t = cpu();
    if (t - g_last > g_maxgap) g_maxgap = t - g_last;
    g_last = t;
    return ++g_polls == g_abort_at;
}

static void rand_frame(uint8_t f[11])
{
    for (int i = 0; i < 11; i++) f[i] = (uint8_t)sy_rng64();
    f[10] = 0; js8_crc12_store(f, js8_crc12(f));
}

static void make_audio(int kind, uint32_t n, int nsig, double amp)
{
    double *b = calloc(n + 1, sizeof *b);
    for (int s = 0; s < nsig; s++) {
        uint8_t f[11], tones[79];
        rand_frame(f); js8_encode(f, js8_costas_normal, tones);
        double f0 = 200.0 + (double)(sy_rng64() % 3600), t0 = 4000.0 + (double)((long)(sy_rng64() % 24000) - 12000);
        if (kind == 7) f0 = 600.0 + 56.25 * (double)(s % 50);                 /* a packed band */
        if (kind == 8) { f0 = 1000.0 + (double)(s % 3) * 3.0; t0 = 4000.0 + (double)(s * 37 % 900); }   /* piled up */
        sy_add_cpfsk(b, (long)n, tones, f0, t0, amp * (0.2 + sy_u()));
    }
    for (uint32_t i = 0; i < n; i++) {
        double v = b[i];
        switch (kind) {
        case 0: v = 0; break;
        case 1: v += 0.05 * sy_gauss(); break;
        case 2: v = 1.0; break;
        case 3: v = (i / 7) & 1 ? 1.0 : -1.0; break;
        case 4: v = (double)((long)(sy_rng64() % 2000001) - 1000000) * 1e30; break;       /* absurd source values */
        case 5: v = (sy_rng64() & 1) ? 32767.0 / 32768.0 : -1.0; break;
        case 6: v = v > 0.02 ? 0.02 : v < -0.02 ? -0.02 : v; break;                       /* hard clipping */
        default: v += 0.001 * sy_gauss(); break;
        }
        g_x[i] = (float)v;
    }
    free(b);
}

static void one(long it)
{
    static const uint32_t rates[] = { 6000, 6400, 9600, 12000, 7200, 11025, 8001, 48000, 0 };
    uint32_t fs = sy_rng64() % 6 ? 8000 : rates[sy_rng64() % 9];
    uint32_t n = (uint32_t)(sy_rng64() % 4 ? 100000 + sy_rng64() % 30000 : sy_rng64() % 130000);
    uint32_t wl = (uint32_t)(sy_rng64() % 12 ? 96 * 1024 + sy_rng64() % (164 * 1024) : sy_rng64() % (120 * 1024)) & ~7u;
    int maxout = 1 + (int)(sy_rng64() % 64);
    int kind = getenv("KIND") ? atoi(getenv("KIND")) : (int)(sy_rng64() % 10), nsig = kind >= 6 ? (int)(sy_rng64() % 90) : (int)(sy_rng64() % 4);
    float flo = (float)(sy_rng64() % 4000), fhi = (float)(sy_rng64() % 4200), nom = 0.5f - (float)(sy_rng64() % 200) * 0.01f;
    switch (sy_rng64() % 12) {
    case 0: flo = 0; fhi = 3900; break;
    case 1: flo = 450; fhi = 2550; break;
    case 2: flo = -1e9f; fhi = 1e9f; break;
    case 3: flo = 3990; fhi = 4000; break;
    case 4: nom = (float)((long)(sy_rng64() % 600) - 300) * 0.01f; break;
    case 5: nom = getenv("WILDNOM") ? (float)((long)(sy_rng64() % 2000000) - 1000000) * 1e4f : (float)((long)(sy_rng64() % 60000) - 30000) * 0.001f; break;   /* WILDNOM: beyond int32 samples */
    default: if (flo > fhi && sy_rng64() % 4) { float t = flo; flo = fhi; fhi = t; } break;
    }
    g_n = n; g_rdmode = (int)(sy_rng64() % 6);
    make_audio(kind, n, nsig, 0.01 + 0.5 * sy_u());
    void *work = wl ? malloc(wl) : 0;
    js8_result_t *res = malloc(sizeof *res * (size_t)maxout);
    dsrc_t src = { 0, rd, n, fs };
    g_polls = 0; g_abort_at = sy_rng64() % 3 ? -1 : (long)(sy_rng64() % 3000);
    g_last = cpu();
    int nd = js8_decode(&src, flo, fhi, nom, res, maxout, work, wl, sy_rng64() % 9 ? ab : 0);
    if (nd > maxout || nd < -3) FAIL("it %ld: return value %d (maxout %d)", it, nd, maxout);
    if (!(nom >= -30.0f && nom <= 30.0f) && nd != JS8_ERR_ARGS) FAIL("it %ld: nominal start %g s accepted (returned %d)", it, (double)nom, nd);
    if (js8_work_peak() > wl) FAIL("it %ld: work peak %u > work_len %u", it, js8_work_peak(), wl);
    for (int i = 0; i < nd; i++) {
        if (!js8_crc12_check(res[i].frame)) FAIL("it %ld: reported frame fails CRC-12", it);
        if (!isfinite(res[i].freq_hz) || !isfinite(res[i].dt_s) || !isfinite(res[i].snr_db)) FAIL("it %ld: non-finite result", it);
        if (res[i].freq_hz < 0 || res[i].freq_hz > (float)fs / 2) FAIL("it %ld: frequency %g", it, (double)res[i].freq_hz);
    }
    if (it < 12 || it % 50 == 0)
        printf("it %ld: fs %u n %u kind %d nsig %d f %.0f..%.0f nom %.2f work %u maxout %d rd %d -> %d, peak %u, polls %ld\n",
               it, fs, n, kind, nsig, (double)flo, (double)fhi, (double)nom, wl, maxout, g_rdmode, nd, js8_work_peak(), g_polls);
    free(work); free(res);
}

/* abort-poll latency on heavy loads (CPU time on this machine; the share of the decode is what carries over) */
static void poll_test(void)
{
    static const struct { int kind, nsig; float flo, fhi; const char *name; } L[] = {
        { 1, 0, 450, 2550, "noise only, 450..2550" },
        { 9, 20, 450, 2550, "20 signals, 450..2550" },
        { 7, 50, 450, 2550, "50 signals packed, 450..2550" },
        { 9, 64, 0, 3900, "64 signals, 0..3900 (band passes)" },
        { 8, 60, 450, 2550, "60 signals piled on 1000 Hz" },
    };
    void *work = malloc(195440);
    js8_result_t res[50];
    g_n = 116800;
    for (unsigned k = 0; k < sizeof L / sizeof L[0]; k++) {
        double worst = 0, tot = 0; long polls = 0; int nd = 0;
        for (int rep = 0; rep < 3; rep++) {
            sy_rng_s = 1000u + k;
            make_audio(L[k].kind, g_n, L[k].nsig, 0.05);
            dsrc_t src = { 0, rd, g_n, 8000 };
            g_polls = 0; g_abort_at = -1; g_maxgap = 0; g_rdmode = 0;
            g_t0 = g_last = cpu();
            nd = js8_decode(&src, L[k].flo, L[k].fhi, 0.5f, res, 50, work, 195440, ab);
            tot = cpu() - g_t0; polls = g_polls;
            if (rep == 0 || g_maxgap < worst) worst = g_maxgap;          /* min over repeats: drop scheduler noise */
        }
        printf("%-36s decodes %2d  decode %7.1f ms  polls %6ld  longest gap %6.3f ms = %.2f %% of the decode (a 15 s decode -> %.0f ms)\n",
               L[k].name, nd, tot, polls, worst, 100.0 * worst / tot, 15000.0 * worst / tot);
    }
    free(work);
}

int main(int argc, char **argv)
{
    g_x = malloc(sizeof(float) * 130001);
    if (argc > 1 && !strcmp(argv[1], "poll")) { poll_test(); return 0; }
    long iters = argc > 1 ? atol(argv[1]) : 300;
    if (argc > 2) sy_rng_s = (uint64_t)atol(argv[2]) * 7919u + 1u;
    for (long i = 0; i < iters; i++) one(i);
    free(g_x);
    printf("fuzz_dec: %s (%ld failures in %ld cases)\n", fails ? "FAIL" : "PASS", fails, iters);
    return fails ? 1 : 0;
}
