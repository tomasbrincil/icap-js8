/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8wav - HOST harness: feeds a recording (8000 Hz mono s16 WAV, or headerless .pcm as the radio's
 * capture files) through js8_decode() the way the radio shell (SDK apps/digi-common/digiapp.c) does:
 *   - one call per 15 s period, the capture being the first CAP samples of the period (shell: 116800
 *     = 14.6 s), nominal start 0.5 s;
 *   - audio only through a dsrc_t read callback with the shell's contract (start >= len -> 0, short
 *     counts at the end, s16 -> float / 32768, data fetched in 512-sample pieces, backward seeks
 *     counted because the radio has to reopen the file for them);
 *   - one fixed work arena (default 240 KiB = the radio's data region; -r takes off what the shell
 *     keeps for itself), 8-byte aligned; the peak use is reported;
 *   - an abort callback that is polled (count and longest gap between polls are reported).
 *
 * Recordings are usually NOT aligned to UTC, so the position of the period edge in the file is a
 * parameter:
 *   -t SEC     the period edges are at SEC + k * 15 s in the file (default 0). A first partial period
 *              (before SEC) is decoded too, the missing audio reads as silence.
 *   -a         alignment search: decode the whole file for edge offsets 0, 0.5, ... 14.5 s, print one
 *              "A" line per offset, take the offset with the most decodes (ties: smallest mean |dt|),
 *              shift it by the median dt so that dt ~ 0, and print the R lines for that alignment.
 *   -u         with -a: also print the union over all offsets, one "U" line per distinct frame
 *              (same 22 hex digits within 2 s and 10 Hz count once; the strongest instance is shown).
 * Other options:
 *   -b LO,HI   tone-0 search range in Hz (default 200,3000)
 *   -c N       capture length in samples (default 116800)
 *   -n SEC     nominal start inside the capture (default 0.5)
 *   -W BYTES   arena size (default 245760)       -r BYTES   bytes reserved for the shell (default 0)
 *   -T k=v,..  decoder knobs (see js8_tune_t), e.g. -T passmask=1,iters=30
 *   -m N       max results per period (default 50, the shell's MAXOUT)
 *   -q         no per-period "E" lines
 *   -A N       abort test: the abort callback returns 1 from its N-th poll on (per period)
 *   -s POS     streaming front end as in the radio app: the waterfall rows from capture sample POS on are built
 *              before js8_decode() is called (0 = the whole first pass; the results must not change)
 *   -B MS      the app's budget= : the abort callback returns 1 when the MODELLED radio time (js8synth.h
 *              sy_model_ms, from the decoder's operation counters) passes MS
 * Output (stdout):
 *   R <period> <snr> <dt> <freq> <hex22>          one decoded frame (period = index from the first edge)
 *   E <period> <ndec> <ncand> <ms> <peak> ...     end of period (statistics)
 *   A <offset> <ndec> <mean|dt|>                  alignment search
 *   U <t_file> <snr> <freq> <hex22> <pass>        union line; t_file = frame start in the file (s)
 *   # ...                                         summary
 */
#include "../src/js8dec.h"
#include "js8synth.h"

#define PERIOD 120000

static const int16_t *g_s; static long g_n;          /* the whole recording */
static long g_cap0;                                  /* file index of capture sample 0 */
static uint32_t g_caplen;
static long n_reads, n_back, last_end;
static long n_polls, abort_at; static double last_poll, max_gap, budget_ms;
static long stream_pos = -1;                         /* -s: streaming front end from this capture sample */
static int16_t cap_get(uint32_t i) { long idx = g_cap0 + (long)i; return idx >= 0 && idx < g_n ? g_s[idx] : 0; }

static int src_read(void *ctx, uint32_t start, float *out, uint32_t n)
{
    (void)ctx;
    if (start >= g_caplen) return 0;
    if (start + n > g_caplen) n = g_caplen - start;
    if ((long)start < last_end) n_back++;            /* the radio reopens the file and skips forward */
    uint32_t got = 0;
    while (got < n) {                                /* 512-sample pieces like the shell's rbuf */
        uint32_t k = n - got; if (k > 512) k = 512;
        for (uint32_t i = 0; i < k; i++) {
            long idx = g_cap0 + (long)start + (long)got + (long)i;
            int16_t v = idx >= 0 && idx < g_n ? g_s[idx] : 0;
            out[got + i] = (float)v * (1.0f / 32768.0f);
        }
        got += k; n_reads++;
    }
    last_end = (long)start + (long)got;
    return (int)got;
}
static int dec_abort(void)
{
    double t = sy_now_ms();
    if (n_polls && t - last_poll > max_gap) max_gap = t - last_poll;
    last_poll = t; n_polls++;
    if (budget_ms > 0 && sy_model_ms(&js8_stats) > budget_ms) return 1;
    return abort_at > 0 && n_polls >= abort_at;
}

typedef struct { int period; double off; js8_result_t r; } hit_t;
static hit_t *hits; static int nhits, caphits;
static void add_hit(int period, double off, const js8_result_t *r)
{
    if (nhits == caphits) { caphits = caphits ? 2 * caphits : 256; hits = (hit_t *)realloc(hits, (size_t)caphits * sizeof *hits); }
    hits[nhits].period = period; hits[nhits].off = off; hits[nhits].r = *r; nhits++;
}

static void *arena; static uint32_t arena_len;
static float flo = 200, fhi = 3000, nom = 0.5f;
static int maxout = 50, quiet;
static uint32_t peak_all; static double ms_all, ms_max; static int nper_all, rc_bad;

/* decode the whole file with period edges at off + k * 15 s; print = 1: R/E lines */
static int run(double off, int print)
{
    long e0 = (long)floor(off * SY_FS + 0.5);
    int first = nhits, pidx = 0;
    static js8_result_t res[256];
    while (e0 > 0) e0 -= PERIOD;                     /* first (possibly partial) period */
    if (e0 + (long)g_caplen <= 0) e0 += PERIOD;
    for (; e0 < g_n; e0 += PERIOD, pidx++) {
        if (g_n - e0 < PERIOD / 4 && pidx > 0) break;    /* a tail shorter than 3.75 s cannot hold a frame */
        g_cap0 = e0; last_end = 0;
        dsrc_t src = { 0, src_read, g_caplen, SY_FS };
        long nb0 = n_back;
        double t0 = sy_now_ms();
        n_polls = 0; max_gap = 0;
        sy_stream(arena, arena_len, g_caplen, flo, fhi, nom, stream_pos, cap_get);
        int nd = js8_decode(&src, flo, fhi, nom, res, maxout, arena, arena_len, dec_abort);
        double ms = sy_now_ms() - t0;
        if (nd == JS8_ERR_ABORT && budget_ms > 0) nd = 0;    /* cut by the budget before anything was decoded */
        if (nd < 0) { rc_bad++; if (print) printf("E %d rc=%d\n", pidx, nd); continue; }
        if (js8_work_peak() > peak_all) peak_all = js8_work_peak();
        ms_all += ms; if (ms > ms_max) ms_max = ms; nper_all++;
        for (int i = 0; i < nd; i++) {
            add_hit(pidx, (double)e0 / SY_FS, &res[i]);
            if (print) {
                printf("R %d %+.0f %+.2f %.1f ", pidx, (double)res[i].snr_db, (double)res[i].dt_s, (double)res[i].freq_hz);
                for (int j = 0; j < 11; j++) printf("%02x", res[i].frame[j]);
                printf("\n");
            }
        }
        if (print && !quiet)
            printf("E %d %d %u %.1f %u tried=%u bp=%u iters=%u polls=%ld gap_ms=%.2f rewinds=%ld t=%.2f model_ms=%.0f\n", pidx, nd, js8_stats.ncand, ms,
                   js8_work_peak(), js8_stats.ntried, js8_stats.nbp, js8_stats.bp_iters, n_polls, max_gap, n_back - nb0, (double)e0 / SY_FS, sy_model_ms(&js8_stats));
    }
    return nhits - first;
}
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static int cmph(const void *a, const void *b)
{
    const hit_t *x = (const hit_t *)a, *y = (const hit_t *)b;
    double tx = x->off + 0.5 + (double)x->r.dt_s, ty = y->off + 0.5 + (double)y->r.dt_s;
    return tx < ty ? -1 : tx > ty ? 1 : x->r.freq_hz < y->r.freq_hz ? -1 : x->r.freq_hz > y->r.freq_hz;
}

int main(int argc, char **argv)
{
    const char *file = NULL;
    double off = 0;
    int align = 0, uni = 0;
    uint32_t wlen = 245760, reserve = 0;
    g_caplen = 116800;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) off = atof(argv[++i]);
        else if (!strcmp(argv[i], "-a")) align = 1;
        else if (!strcmp(argv[i], "-u")) uni = 1;
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (!strcmp(argv[i], "-A") && i + 1 < argc) abort_at = atol(argv[++i]);
        else if (!strcmp(argv[i], "-B") && i + 1 < argc) { budget_ms = atof(argv[++i]); sy_budget(budget_ms); }
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) stream_pos = atol(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) { if (sscanf(argv[++i], "%f,%f", &flo, &fhi) != 2) { fprintf(stderr, "-b LO,HI\n"); return 2; } }
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) g_caplen = (uint32_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) nom = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "-W") && i + 1 < argc) wlen = (uint32_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) reserve = (uint32_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) maxout = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) { if (sy_tune(argv[++i])) return 2; }
        else if (argv[i][0] != '-' && !file) file = argv[i];
        else { fprintf(stderr, "js8wav: bad option %s (see the header of tests/js8wav.c)\n", argv[i]); return 2; }
    }
    if (!file) { fprintf(stderr, "usage: js8wav FILE [-t SEC | -a [-u]] [-b LO,HI] [-T k=v,...] ...\n"); return 2; }
    if (maxout < 1 || maxout > 256 || reserve >= wlen) { fprintf(stderr, "js8wav: bad -m / -r\n"); return 2; }
    g_s = sy_load(file, &g_n);
    if (!g_s) return 1;
    arena_len = (wlen - reserve) & ~7u;
    arena = malloc(arena_len);                      /* malloc memory is at least 8-byte aligned */
    printf("# file %s: %ld samples = %.2f s; band %.0f..%.0f Hz; capture %u samples; arena %u B\n", file, g_n, (double)g_n / SY_FS, (double)flo, (double)fhi, g_caplen, arena_len);

    if (align) {
        double best_off = 0, best_coarse = 0, best_mad = 1e9; int best_n = -1, best_first = 0, best_cnt = 0;
        for (int k = 0; k < 30; k++) {
            double o = 0.5 * k, mad = 0;
            int first = nhits, n = run(o, 0);
            for (int i = first; i < nhits; i++) mad += fabs((double)hits[i].r.dt_s);
            mad = n ? mad / n : 0;
            printf("A %.1f %d %.2f\n", o, n, mad);
            if (n > best_n || (n == best_n && mad < best_mad)) { best_n = n; best_mad = mad; best_off = o; best_first = first; best_cnt = n; }
        }
        best_coarse = best_off;
        if (best_cnt > 0) {                          /* shift by the median dt */
            double *d = (double *)malloc((size_t)best_cnt * sizeof *d);
            for (int i = 0; i < best_cnt; i++) d[i] = (double)hits[best_first + i].r.dt_s;
            qsort(d, (size_t)best_cnt, sizeof *d, cmpd);
            best_off += d[best_cnt / 2];
            free(d);
        }
        best_off = fmod(best_off + 150.0, 15.0);
        if (best_off > 14.995) best_off = 0;
        int nsearch = nhits;                         /* hits of the 30 search runs (for the union) */
        if (best_cnt > 0 && run(best_off, 0) < best_n) {   /* the shifted grid lost frames: keep the searched one */
            best_off = best_coarse;
            printf("# the dt-centred alignment decodes fewer frames; keeping the searched offset\n");
        }
        nhits = nsearch;
        printf("# best alignment: period edges at %.2f s + k*15 s (%d decodes in the 0.5 s search)\n", best_off, best_n);
        int nun = nsearch;
        if (uni) {                                   /* union over the search offsets */
            qsort(hits, (size_t)nun, sizeof *hits, cmph);
            int nu = 0;
            for (int i = 0; i < nun; i++) {
                int dupe = 0;
                double ti = hits[i].off + 0.5 + (double)hits[i].r.dt_s;
                for (int j = 0; j < nun && !dupe; j++) {
                    if (j == i || memcmp(hits[j].r.frame, hits[i].r.frame, 11)) continue;
                    double tj = hits[j].off + 0.5 + (double)hits[j].r.dt_s;
                    if (fabs(tj - ti) > 2.0 || fabs((double)(hits[j].r.freq_hz - hits[i].r.freq_hz)) > 10.0) continue;
                    if (hits[j].r.snr_db > hits[i].r.snr_db || (hits[j].r.snr_db == hits[i].r.snr_db && j < i)) dupe = 1;
                }
                if (dupe) continue;
                printf("U %.2f %+.0f %.1f ", ti, (double)hits[i].r.snr_db, (double)hits[i].r.freq_hz);
                for (int j = 0; j < 11; j++) printf("%02x", hits[i].r.frame[j]);
                printf(" %d\n", hits[i].r.pass);
                nu++;
            }
            printf("# union over 30 offsets: %d distinct frames\n", nu);
        }
        double ms_search = ms_all; int np_search = nper_all;
        ms_all = 0; ms_max = 0; nper_all = 0;
        int n = run(best_off, 1);
        printf("# %d frames at the best alignment; %d periods, %.1f ms mean, %.1f ms max per period; arena peak %u of %u B; (search: %d decodes, %.1f ms mean)\n",
               n, nper_all, nper_all ? ms_all / nper_all : 0.0, ms_max, peak_all, arena_len, np_search, np_search ? ms_search / np_search : 0.0);
    } else {
        int n = run(off, 1);
        printf("# %d frames; %d periods, %.1f ms mean, %.1f ms max per period; arena peak %u of %u B\n",
               n, nper_all, nper_all ? ms_all / nper_all : 0.0, ms_max, peak_all, arena_len);
    }
    if (rc_bad) printf("# %d periods returned an error\n", rc_bad);
    return 0;
}
