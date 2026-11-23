/* SPDX-License-Identifier: GPL-3.0-or-later AND MIT
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8dec.c - JS8 (JS8Call compatible, submode Normal) decoder for the IC-705 ICAP app (see js8dec.h).
 *
 * Modified 2026 for JS8 by this project (the changes are GPL-3.0-or-later).
 * This file is a fork of apps/ft8/ft8dec.c of the ICAP SDK (MIT, Copyright (c) 2026 Dawid SQ6EMM),
 * which carries adapted copies of the sync search and BP decoder of kgoba/ft8_lib:
 *
 *   MIT License - Copyright (c) 2018 Kārlis Goba (ft8_lib), Copyright (c) 2026 Dawid SQ6EMM (ft8dec.c)
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
 *   associated documentation files (the "Software"), to deal in the Software without restriction,
 *   including without limitation the rights to use, copy, modify, merge, publish, distribute,
 *   sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions: The above copyright notice and this
 *   permission notice shall be included in all copies or substantial portions of the Software.
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 *   NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 *   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The JS8 specifics (Costas arrays, LDPC (174,87) tables, CRC-12, the four decoding passes and their
 * acceptance limits) follow the JS8Call sources (GPLv3; lib/js8/js8dec.f90, JS8.cpp), re-implemented
 * on the SDK structure; no JS8Call code is copied. The combined file is conveyed under
 * GPL-3.0-or-later; the MIT notice above stays with the parts it covers (see NOTICE.md).
 *
 * Per call (one 15 s period, audio streamed from the SD capture through dsrc_t):
 *   1. Waterfall as in the FT8 app (Hann over 2 symbols, 2x time and 2x frequency oversampling, uint8
 *      magnitudes in 0.5 dB steps) in the caller's work area.  Per row: one FFT of 2560 real samples as 1280
 *      complex (5 radix-4 FFTs of 256 + a 5-point butterfly), the Hann window applied to the spectrum (3 taps),
 *      power -> 0.5 dB by table (rfft, spec_row).  The candidate search adds the waterfall cells of the Costas
 *      pattern row by row for all frequencies at once (find_candidates), BP is a layered integer sum-product
 *      (bp), the subtraction runs four phasors per signal (corr_run, sub_run): kernels of the speed-up of
 *      23 Nov 2026, checked by tests/js8kbench.c (make kbench).
 *   2. Costas sync search (three 7-symbol groups, js8_costas_normal) -> per candidate: hard-sync check
 *      (>= 7 of 21 like JS8Call), LLRs from the waterfall with the natural-binary tone map, BP over the
 *      (174,87) code (codeword = 87 parity bits then the 87 message bits), CRC-12. Attempts per
 *      candidate, as JS8Call: 1 = first metric, 2 = second metric, 3 = LLR[0..23] zeroed,
 *      4 = LLR[0..47] zeroed; plus (from the FT8 app) 5 = tone powers interpolated towards the true
 *      time / frequency. JS8Call's limits on the number of hard errors apply per pass.
 *   3. Decoded signals are subtracted from the audio and the search is repeated next to them (a "residual
 *      pass": one more read of the capture and one more waterfall, about 2.5 s on the radio; by default at most
 *      one, on a time grid shifted by half a row, and only if the first pass left evidence for it - js8_decode()).
 *      The reference is plain continuous-phase FSK (JS8Call does no Gaussian shaping): the frequency steps at the
 *      symbol edge.
 *   The waterfall of the first pass can be built while the period is still being captured (js8_stream_*): then
 *   only search and decoding are left for the time after the period.
 *   4. DT / frequency refined by parabolic interpolation; SNR in 2500 Hz from the tone power over a noise
 *      baseline (low percentile of the period's average spectrum).
 * All buffers are in `work` (js8_work_peak()), including the BP message arrays; the deepest call chain needs
 * about 4.5 KB of stack (arm-none-eabi-gcc -fstack-usage). should_abort is polled per waterfall row and per candidate.
 */
#include "js8dec.h"
#include "js8_constants.h"
#include "js8crc.h"
#include "js8enc.h"

#define TOSR        2          /* time subdivisions per symbol */
#define FOSR        2          /* frequency subdivisions per 6.25 Hz */
#define MAX_SIG     64         /* signals per period */
#define MAX_BLOCKS  93         /* 15 s / 0.16 s */
#define MAX_SF      24         /* strong sync failures remembered per band pass */
#define NSYM        JS8_NN     /* 79 */
#define LN          JS8_LDPC_N
#define LK          JS8_LDPC_K
#define LM          JS8_LDPC_M
#define NUM_SYNC    3
#define LEN_SYNC    7
#define SYNC_OFFSET 36

/* Defaults of 23 Nov 2026 (front-end work): the first measurements on the radio showed
 * ~2 s per pass over the capture (SD read + 184 FFTs) and ~28 ms per BP run, most of them wasted on noise candidates.
 *   min_score 10 (was 8), nsync2 / nsync34 10 (was 7): attempts 2, 3, 4 only for candidates with a clear Costas pattern
 *   swide 1, ev 9: ONE kind of residual pass (everything decoded so far subtracted, looking next to the new signals
 *     and the strong failures), made only when the pass before left evidence that it is needed; at most one on the
 *     shifted grid (shift 1) and one more on the normal grid (subpasses 2; the merge of 23 Nov 2026: with the faster
 *     kernels and the streamed first pass the second one fits the time budget and brings the 20-signal yield back
 *     to that of the first build)
 *   foot 8, nbr 0x13: fewer attempts for the sidelobes of a decoded signal and the neighbours of a failed candidate
 *   erounds 1, emask 0x11: a decoded signal is erased from the waterfall in work and the candidates it covered get
 *     another try at once (attempts 1 and 5) - what a second residual pass used to find, without the pass
 *   budget 0: see js8dec.h (the app sets it from JS8.CFG budget=)
 * The decoding scheme of the first build: -T min_score=8,nsync2=7,nsync34=7,swide=0,subpasses=3,ev=0,foot=0,nbr=0,erounds=0 */
js8_tune_t js8_tune = {
    /* llr */ 2, /* llr2 */ 0, /* llr5 */ 2, /* max_cand */ 300, /* min_score */ 10, /* iters */ 30,
    /* osd */ 0, /* osd_nt */ 60, /* nhmax */ 59, /* nh34 */ 39, /* nh4 */ 30,
    /* subpasses */ 2, /* nsync_min */ 7, /* nsync34 */ 10, /* passmask */ 0x1F,
    /* scale */ 4.9f, /* snr_off */ -26.0f, /* pct */ 10, /* skip */ 2, /* near */ 1,
    /* dup_out */ 0, /* dup_hz */ 25.0f, /* osd_min */ 0, /* osd_dmax */ 1e9f,
    /* shift */ 1, /* nstrong */ 12, /* sstrong */ 15, /* swide */ 1, /* nsync2 */ 10, /* foot */ 8, /* ev */ 9, /* nbr */ 0x13, /* erounds */ 1, /* emask */ 0x11,
    /* budget */ 0
};
js8_stats_t js8_stats;
uint32_t (*js8_clock)(void);

#ifdef JS8_DEBUG
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif

static uint32_t peak;
uint32_t js8_work_peak(void) { return peak; }

/* ---- types (after ft8_lib decode.h) */
typedef struct {
    int num_blocks;              /* blocks (symbols) stored */
    int num_bins;                /* bins of 6.25 Hz */
    uint8_t *mag;                /* [blocks][TOSR][FOSR][num_bins] */
    int block_stride;            /* TOSR * FOSR * num_bins */
} wf_t;
typedef struct {
    int16_t score, time_offset, freq_offset;
    uint8_t time_sub, freq_sub;
} cand_t;

/* ---- helpers */
static uintptr_t al8(uintptr_t p) { return (p + 7u) & ~(uintptr_t)7u; }
static void zero(void *p, uint32_t n) { uint8_t *q = p; while (n--) *q++ = 0; }
static float mx(float a, float b) { return a > b ? a : b; }

typedef struct {                 /* a decoded signal (kept for the subtraction passes) */
    float f0;                    /* tone 0, Hz */
    int32_t t0;                  /* first sample of symbol 0 (src index) */
    uint8_t tones[NSYM];
    uint8_t pad;
} sig_t;

typedef struct {                 /* per signal while streaming */
    uint32_t phc, phs;           /* reference phase at the correlation / subtraction sample */
    float cr[4], ci[4];          /* symbol correlations, ring by symbol index & 3 */
    float pr, pi;                /* sum c[k] conj(c[k-1]) -> frequency correction */
    int started_c, started_s;    /* reference phase initialised (a signal may start before the stream) */
} sst_t;
#define STAB 512                 /* reference oscillator sine table (linear interpolation: error < 2e-5) */
#define NW 6                     /* 32-bit words per codeword (OSD) */

typedef struct {
    const dsrc_t *src;
    int (*ab)(void);
    int aborted;
    int fs, block, sub, nfft, N, P, m, nblk, podd;
    int loff; float peps;        /* spec_row: offset of the 0.5 dB scale (Q12), power floor */
    uint16_t lt[66];             /* 20 log10(1 + i/64) in Q12, i = 0..64 */
    int32_t s0;                  /* sample index of the waterfall origin (may be < 0: zeros) */
    float nom, df;
    const float *qs;             /* quarter-wave sine table: qs[j] = sin(2 pi j / nfft), j <= nfft/4 */
    float lin[256], amp[256];    /* uint8 waterfall value -> linear power / amplitude */
    float sn[STAB + STAB / 4 + 1]; /* sin(2 pi i / STAB), with a quarter extra for cos */
#ifndef JS8_BP_SPA
    int16_t bL[LN + 1], br[LM * 7];   /* BP: bit totals, check-to-bit messages (in the arena, not on the 16 KiB stack) */
#else
    uint8_t mpos[LM][7];         /* BP: for check m, edge i: which of bit Nm[m][i]'s 3 checks it is */
    float tov[LN][3], toc[LM][7], sum[LN];
#endif
#ifdef JS8_WITH_OSD
    uint32_t gc[LN][3];          /* generator by columns: message bits that feed codeword bit n */
    uint32_t (*w)[NW];           /* OSD scratch [LK][NW] (after the candidates) */
    float osd_r[LN], osd_rp[NW * 32];
    uint8_t osd_perm[LN], osd_piv[LK];
#endif
    uint8_t dec[MAX_SIG][JS8_FRAME_BYTES];   /* frames already decoded */
    sig_t sig[MAX_SIG];
    int nsig;
    const float *base;           /* mean noise power per bin, current band pass */
    /* candidates with a clear Costas pattern that did not decode on the normal time grid: they get a
     * second chance on a waterfall whose rows are shifted by half a row (40 ms), see js8_decode() */
    struct { int16_t hbin, row; } sf[MAX_SF];
    int nsf, phase, evid, first;
    int stage; uint32_t tlast, t0;   /* stage clock (js8_stats.t); clock at the start of the call (js8_tune.budget) */
    int ipass, kind;             /* decoding attempt in work, 0..4; kind of pass 0..2 (statistics) */
    int own_fo;                  /* candidates at freq_offset >= own_fo belong to the next band pass */
    js8_result_t *out; int maxout, nout;
} ctx_t;

/* stage clock: charge the time since the last change to the stage that ends here */
static __attribute__((noinline)) void stg(ctx_t *c, int id)
{
    if (js8_clock) { uint32_t now = js8_clock(), d = now - c->tlast; js8_stats.t[c->stage] += d; js8_stats.tk[c->kind] += d; c->tlast = now; }
    c->stage = id;
}
static __attribute__((noinline)) int poll(ctx_t *c)
{
    if (!c->aborted && c->ab) {
        int prev = c->stage;
        stg(c, JS8_T_POLL); js8_stats.npoll++;
        if (c->ab()) c->aborted = 1;
        stg(c, prev);
    }
    return c->aborted;
}

/* cos/sin of 2 pi j / nfft, 0 <= j < nfft */
static void tsc(const ctx_t *c, int j, float *co, float *si)
{
    int q = c->nfft >> 2;
    if (j < q)          { *si =  c->qs[j];             *co =  c->qs[q - j]; }
    else if (j < 2 * q) { *si =  c->qs[2 * q - j];     *co = -c->qs[j - q]; }
    else if (j < 3 * q) { *si = -c->qs[j - 2 * q];     *co = -c->qs[3 * q - j]; }
    else                { *si = -c->qs[4 * q - j];     *co =  c->qs[j - 3 * q]; }
}
/* cos/sin of a 32-bit phase (2^32 = 2 pi): STAB-entry table, linear interpolation */
static inline void psc(const ctx_t *c, uint32_t ph, float *co, float *si)
{
    uint32_t j = ph >> 23;
    float fr = (float)(ph & 0x7FFFFFu) * (1.0f / 8388608.0f);
    const float *t = c->sn + j;
    *si = t[0] + (t[1] - t[0]) * fr;
    *co = t[STAB / 4] + (t[STAB / 4 + 1] - t[STAB / 4]) * fr;
}

/* ---- FFT of one STFT frame: X[k] = sum x[n] e^{-2 pi i k n / nfft} of nfft real samples, taken as N = nfft/2
 * complex numbers z[n] = x[2n] + i x[2n+1].  N = P * m (P = 2^a, m odd): the m decimated sequences get a radix-4
 * FFT of P points each, then one radix-m step in place (8 kHz: m = 5, a 5-point butterfly).  The samples are
 * NOT windowed here: the Hann window is applied to the spectrum (spec_row), so the frame is loaded straight
 * from the ring in bit-reversed order and no second buffer is needed.  y: N complex. */
static void cfft_p(const ctx_t *c, float *s, int P)          /* in place, input in bit-reversed order */
{
    int h;
    if (c->podd) {                                           /* P = 2 * 4^j: one radix-2 stage first */
        for (float *a = s; a < s + 2 * P; a += 4) {
            float tr = a[2], ti = a[3];
            a[2] = a[0] - tr; a[3] = a[1] - ti; a[0] += tr; a[1] += ti;
        }
        h = 2;
    } else {                                                 /* first radix-4 stage: all twiddles are 1 */
        for (float *a = s; a < s + 2 * P; a += 8) {
            float t0r = a[0] + a[2], t0i = a[1] + a[3], t1r = a[0] - a[2], t1i = a[1] - a[3];
            float sr = a[4] + a[6], si = a[5] + a[7], dr = a[4] - a[6], di = a[5] - a[7];
            a[0] = t0r + sr; a[1] = t0i + si; a[4] = t0r - sr; a[5] = t0i - si;
            a[2] = t1r + di; a[3] = t1i - dr; a[6] = t1r - di; a[7] = t1i + dr;
        }
        h = 4;
    }
    for (; h < P; h <<= 2) {                                 /* radix 4: groups of 4h, quarters x0..x3 */
        const int st = c->nfft / (4 * h), g = 8 * h;
        for (int k = 0; k < h; k++) {
            float w1r, w1i, w2r, w2i, w3r, w3i;              /* e^{+i..}: the products below conjugate them */
            tsc(c, k * st, &w1r, &w1i); tsc(c, 2 * k * st, &w2r, &w2i); tsc(c, 3 * k * st, &w3r, &w3i);
            for (float *x0 = s + 2 * k; x0 < s + 2 * P; x0 += g) {
                float *x1 = x0 + 2 * h, *x2 = x1 + 2 * h, *x3 = x2 + 2 * h;
                float p1r = x1[0] * w2r + x1[1] * w2i, p1i = x1[1] * w2r - x1[0] * w2i;
                float p2r = x2[0] * w1r + x2[1] * w1i, p2i = x2[1] * w1r - x2[0] * w1i;
                float p3r = x3[0] * w3r + x3[1] * w3i, p3i = x3[1] * w3r - x3[0] * w3i;
                float t0r = x0[0] + p1r, t0i = x0[1] + p1i, t1r = x0[0] - p1r, t1i = x0[1] - p1i;
                float sr = p2r + p3r, si = p2i + p3i, dr = p2r - p3r, di = p2i - p3i;
                x0[0] = t0r + sr; x0[1] = t0i + si; x2[0] = t0r - sr; x2[1] = t0i - si;
                x1[0] = t1r + di; x1[1] = t1i - dr; x3[0] = t1r - di; x3[1] = t1i + dr;
            }
        }
    }
}
static void rfft(const ctx_t *c, const float *ring, int ri, int RL, float *y)   /* frame = ring slots from ri -> Z in y */
{
    const int P = c->P, m = c->m;
    for (int q = 0, j = 0; q < P; q++) {                     /* sequence r = every m-th complex sample, bit-reversed */
        float *d = y + 2 * j;
        for (int r = 0; r < m; r++, d += 2 * P) {
            d[0] = ring[ri]; if (++ri == RL) ri = 0;
            d[1] = ring[ri]; if (++ri == RL) ri = 0;
        }
        int bit = P >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
    }
    for (int r = 0; r < m; r++) cfft_p(c, y + 2 * r * P, P);
    if (m == 1) return;
    for (int q = 0; q < P; q++) {                            /* Z[q + P s] = sum_r (Y_r[q] W^(r q)) e^{-2 pi i r s / m} */
        float tr[16], ti[16], *v = y + 2 * q;
        tr[0] = v[0]; ti[0] = v[1];
        for (int r = 1; r < m; r++) {
            float wr, wi; const float *u = v + 2 * r * P;
            tsc(c, 2 * r * q, &wr, &wi);
            tr[r] = u[0] * wr + u[1] * wi; ti[r] = u[1] * wr - u[0] * wi;
        }
        if (m == 5) {
            const float c1 = 0.309016994f, s1 = 0.951056516f, c2 = -0.809016994f, s2 = 0.587785252f;
            float ar = tr[1] + tr[4], ai = ti[1] + ti[4], br = tr[1] - tr[4], bi = ti[1] - ti[4];
            float cr = tr[2] + tr[3], ci = ti[2] + ti[3], dr = tr[2] - tr[3], di = ti[2] - ti[3];
            float e1r = tr[0] + c1 * ar + c2 * cr, e1i = ti[0] + c1 * ai + c2 * ci;
            float e2r = tr[0] + c2 * ar + c1 * cr, e2i = ti[0] + c2 * ai + c1 * ci;
            float f1r = s1 * br + s2 * dr, f1i = s1 * bi + s2 * di;      /* out1 = e1 - i f1, out4 = e1 + i f1 */
            float f2r = s2 * br - s1 * dr, f2i = s2 * bi - s1 * di;      /* out2 = e2 - i f2, out3 = e2 + i f2 */
            v[0] = tr[0] + ar + cr; v[1] = ti[0] + ai + ci;
            v += 2 * P; v[0] = e1r + f1i; v[1] = e1i - f1r;
            v += 2 * P; v[0] = e2r + f2i; v[1] = e2i - f2r;
            v += 2 * P; v[0] = e2r - f2i; v[1] = e2i + f2r;
            v += 2 * P; v[0] = e1r - f1i; v[1] = e1i + f1r;
        } else {                                             /* any odd m <= 15 (other sample rates): plain DFT */
            const int stm = c->nfft / m;
            for (int sx = 0; sx < m; sx++, v += 2 * P) {
                float sr = tr[0], si = ti[0];
                for (int r = 1; r < m; r++) {
                    float wr, wi;
                    tsc(c, (r * sx % m) * stm, &wr, &wi);
                    sr += tr[r] * wr + ti[r] * wi; si += ti[r] * wr - tr[r] * wi;
                }
                v[0] = sr; v[1] = si;
            }
        }
    }
}
/* 2 X[k] from the packed complex FFT, 0 < k < N */
static inline void xk2(const ctx_t *c, const float *Z, int k, float *xr, float *xi)
{
    const float *a = Z + 2 * k, *b = Z + 2 * (c->N - k);
    float er = a[0] + b[0], ei = a[1] - b[1], dr = a[0] - b[0], di = a[1] + b[1], wr, wi;
    tsc(c, k, &wr, &wi);                                      /* X = E + O e^{-i w}, O = (di, -dr) / 2 */
    *xr = er + di * wr - dr * wi; *xi = ei - di * wi - dr * wr;
}
/* one waterfall row: Hann window as the 3-tap filter X[k] - (X[k-1] + X[k+1]) / 2 on the spectrum, |.|^2 -> 0.5 dB
 * steps by exponent + mantissa table (no log, no division, no float -> int conversion).
 * out[fsub * nb + bin] for k = k0 + bin * FOSR + fsub. */
static void spec_row(const ctx_t *c, const float *Z, int k0, int nb, uint8_t *out)
{
    float pr, pi, cr, ci;
    xk2(c, Z, k0 - 1, &pr, &pi); xk2(c, Z, k0, &cr, &ci);
    for (int j = 0; j < nb * FOSR; j++) {
        float nr, ni;
        xk2(c, Z, k0 + j + 1, &nr, &ni);
        float hr = cr - 0.5f * (pr + nr), hi = ci - 0.5f * (pi + ni);
        union { float f; uint32_t u; } v;
        v.f = hr * hr + hi * hi + c->peps;
        const uint16_t *t = c->lt + ((v.u >> 17) & 63u);
        int sc = ((int)(v.u >> 23) * 24660 + t[0] + (((t[1] - t[0]) * (int)((v.u >> 9) & 255u)) >> 8) + c->loff) >> 12;
        out[(j % FOSR) * nb + j / FOSR] = (uint8_t)(sc < 0 ? 0 : sc > 255 ? 255 : sc);
        pr = cr; pi = ci; cr = nr; ci = ni;
    }
}

/* STFT geometry for a sample rate (0 = ok) and its tables (qs: nfft/4 + 1 floats) */
static int stft_geom(ctx_t *c, int fs)
{
    c->fs = fs; c->block = fs * 4 / 25; c->sub = c->block / TOSR; c->nfft = c->block * FOSR;
    if ((c->block & 1) || (c->nfft & 3)) return -1;
    c->N = c->nfft / 2; c->m = c->N;
    while (!(c->m & 1)) c->m >>= 1;
    c->P = c->N / c->m;                                      /* N = P * m, P = 2^a, m odd */
    c->podd = 0;
    for (int v = c->P; v > 1; v >>= 1) c->podd ^= 1;
    if (c->m > 15 || c->P < 8) return -1;
    c->df = (float)fs / (float)c->block;
    return 0;
}
static void stft_tables(ctx_t *c, float *qs)
{
    for (int j = 0; j <= c->nfft / 4; j++) { double s, co; dg_sincos(6.283185307179586 * (double)j / (double)c->nfft, &s, &co); qs[j] = (float)s; }
    c->qs = qs;
    /* spec_row: the row is 0.5/nfft * (2X[k] - X[k-1] - X[k+1]); value = 20 log10(power + 1e-12) + 240 */
    float sc = 0.5f / (float)c->nfft;
    c->peps = 1e-12f / (sc * sc);
    c->loff = (int)(4096.0f * (40.0f * dg_log10f(sc) + 240.0f)) - 127 * 24660;
    for (int i = 0; i <= 64; i++) c->lt[i] = (uint16_t)(81920.0f * dg_log10f(1.0f + (float)i * (1.0f / 64.0f)) + 0.5f);
}

/* ---- streaming: read the audio, subtract the known signals (lag), build the waterfall of the residual.
 * Reference = plain CPFSK as JS8Call's Modulator: the frequency is constant over a symbol and steps at
 * the symbol edge, the phase is continuous. */
/* phase increment per sample (2^32 / fs units) during symbol k */
static uint32_t ph_inc(const ctx_t *c, const sig_t *s, int k, float fscale)
{
    return (uint32_t)(int32_t)((s->f0 + c->df * (float)s->tones[k]) * fscale);
}
/* reference phase at sample u of the signal (phase 0 at u = 0) */
static uint32_t ph_at(const ctx_t *c, const sig_t *s, int32_t u, float fscale)
{
    uint32_t ph = 0;
    int ks = (int)(u / c->block);
    if (ks > NSYM) ks = NSYM;
    for (int k = 0; k < ks; k++) ph += ph_inc(c, s, k, fscale) * (uint32_t)c->block;
    if (ks < NSYM) ph += ph_inc(c, s, ks, fscale) * (uint32_t)(u - ks * c->block);
    return ph;
}

static uint32_t stream_bytes(const ctx_t *c, int nsub)
{
    return (uint32_t)(sizeof(float) * (uint32_t)(2 * c->nfft + 4 * c->sub) + sizeof(sst_t) * (uint32_t)(nsub + 1) + 48);
}

/* reference oscillator of a run of samples at one frequency: four phasors e^{i(ph + k inc)}, k = 0..3, turned four
 * samples at a time (table lookups only at the start of a run; four independent chains instead of one that every
 * sample waits for - and a form the compiler can turn into NEON) */
typedef struct { float c[4], s[4], wc, ws; } osc_t;
static inline void osc_set(const ctx_t *c, osc_t *o, uint32_t ph, uint32_t inc)
{
    for (int k = 0; k < 4; k++) psc(c, ph + (uint32_t)k * inc, &o->c[k], &o->s[k]);
    psc(c, 4u * inc, &o->wc, &o->ws);
}
#define OSC_TURN(o) do { for (int k_ = 0; k_ < 4; k_++) { float t_ = (o).c[k_] * (o).wc - (o).s[k_] * (o).ws; \
        (o).s[k_] = (o).s[k_] * (o).wc + (o).c[k_] * (o).ws; (o).c[k_] = t_; } } while (0)
#define KERNEL __attribute__((noinline, optimize("tree-vectorize", "vect-cost-model=dynamic", "unsafe-math-optimizations")))
/* sum of x[i] e^{-i(ph + i inc)}, i < len */
KERNEL static void corr_run(const ctx_t *c, const float *restrict x, int len, uint32_t ph, uint32_t inc, float *sr, float *si)
{
    osc_t o;
    float ar[4] = { 0, 0, 0, 0 }, ai[4] = { 0, 0, 0, 0 };
    int i = 0;
    osc_set(c, &o, ph, inc);
    for (; i + 4 <= len; i += 4) {
        for (int k = 0; k < 4; k++) { ar[k] += x[i + k] * o.c[k]; ai[k] += x[i + k] * o.s[k]; }
        OSC_TURN(o);
    }
    for (int k = 0; i < len; i++, k++) { ar[k] += x[i] * o.c[k]; ai[k] += x[i] * o.s[k]; }
    *sr += (ar[0] + ar[1]) + (ar[2] + ar[3]); *si -= (ai[0] + ai[1]) + (ai[2] + ai[3]);
}
/* x[i] -= Re((a + i da) e^{i(ph + i inc)}), i < len; a = (ar, ai) */
KERNEL static void sub_run(const ctx_t *c, float *restrict x, int len, uint32_t ph, uint32_t inc, float ar, float ai, float dar, float dai)
{
    osc_t o;
    float pr[4], pi[4];
    const float d4r = 4.0f * dar, d4i = 4.0f * dai;
    int i = 0;
    osc_set(c, &o, ph, inc);
    for (int k = 0; k < 4; k++) { pr[k] = ar + (float)k * dar; pi[k] = ai + (float)k * dai; }
    for (; i + 4 <= len; i += 4) {
        for (int k = 0; k < 4; k++) { x[i + k] -= pr[k] * o.c[k] - pi[k] * o.s[k]; pr[k] += d4r; pi[k] += d4i; }
        OSC_TURN(o);
    }
    for (int k = 0; i < len; i++, k++) x[i] -= pr[k] * o.c[k] - pi[k] * o.s[k];
}

/* correlate one signal with the input chunk [a, a+n) of the stream */
static void sig_corr(const ctx_t *c, const sig_t *s, sst_t *t, int32_t a, const float *x, int n, float fscale)
{
    const int block = c->block;
    int32_t u = a - s->t0;
    int j = 0;
    if (u < 0) { j = -u; u = 0; }
    if (u >= NSYM * block || j >= n) return;
    int ks = u / block, wi = u - ks * block;
    if (!t->started_c) { t->started_c = 1; t->phc = ph_at(c, s, u, fscale); }
    uint32_t ph = t->phc;
    while (j < n) {                                          /* runs: to the end of the symbol or of the chunk */
        const uint32_t flat = ph_inc(c, s, ks, fscale);
        const int q = ks & 3;
        int len = block - wi;
        if (len > n - j) len = n - j;
        if (wi == 0) t->cr[q] = t->ci[q] = 0.0f;
        corr_run(c, x + j, len, ph, flat, &t->cr[q], &t->ci[q]);
        ph += flat * (uint32_t)len; j += len; wi += len;
        if (wi == block) {
            if (ks > 0) {                                    /* symbol ks complete: phase step vs ks-1 */
                int qb = (ks - 1) & 3;
                t->pr += t->cr[q] * t->cr[qb] + t->ci[q] * t->ci[qb];
                t->pi += t->ci[q] * t->cr[qb] - t->cr[q] * t->ci[qb];
            }
            wi = 0;
            if (++ks == NSYM) break;
        }
    }
    t->phc = ph;
}
/* subtract one signal from the output chunk (lagged): ring slots from ri0, stream index a */
static void sig_sub(const ctx_t *c, const sig_t *s, sst_t *t, int32_t a, float *ring, int ri0, int RL, int n, float fscale)
{
    const int block = c->block, half = block / 2;
    const float ab = 2.0f / (float)block, inv = 1.0f / (float)block;
    int32_t u = a - s->t0;
    int j = 0;
    if (u < 0) { j = -u; u = 0; }
    if (u >= NSYM * block || j >= n) return;
    int ks = u / block, wi = u - ks * block, ri = ri0 + j;
    if (ri >= RL) ri -= RL;
    if (!t->started_s) { t->started_s = 1; t->phs = ph_at(c, s, u, fscale); }
    uint32_t ph = t->phs;
    while (j < n) {              /* runs: to the middle or the end of the symbol, the end of the chunk or of the ring */
        const uint32_t flat = ph_inc(c, s, ks, fscale);
        int len = (wi < half ? half : block) - wi;
        if (len > n - j) len = n - j;
        if (len > RL - ri) len = RL - ri;
        int k0, k1; float w;                                 /* amplitude: linear between symbol centres */
        if (wi < half) { k0 = ks > 0 ? ks - 1 : 0; k1 = ks; w = ks > 0 ? (float)(wi + half) * inv : 1.0f; }
        else { k0 = ks; k1 = ks < NSYM - 1 ? ks + 1 : ks; w = ks < NSYM - 1 ? (float)(wi - half) * inv : 0.0f; }
        float dr = t->cr[k1 & 3] - t->cr[k0 & 3], di = t->ci[k1 & 3] - t->ci[k0 & 3];
        int edge = (wi < half && ks == 0) || (wi >= half && ks == NSYM - 1);
        sub_run(c, ring + ri, len, ph, flat, ab * (t->cr[k0 & 3] + dr * w), ab * (t->ci[k0 & 3] + di * w),
                edge ? 0.0f : ab * dr * inv, edge ? 0.0f : ab * di * inv);
        ph += flat * (uint32_t)len; j += len; wi += len; ri += len;
        if (ri == RL) ri = 0;
        if (wi == block) { wi = 0; if (++ks == NSYM) break; }
    }
    t->phs = ph;
}

/* one waterfall row from the nfft samples in ring[] from slot ri (ring length RL): FFT, windowed bin powers ->
 * 0.5 dB steps.  timed = 0: called from the streaming front end, outside js8_decode() (no statistics) */
static void wf_row(ctx_t *c, const float *ring, int ri, int RL, float *y, int b_lo, int nb, uint8_t *mag, int timed)
{
    rfft(c, ring, ri, RL, y);
    if (timed) { stg(c, JS8_T_MAG); js8_stats.nfft++; }
    spec_row(c, y, b_lo * FOSR, nb, mag);
}

/* rows 0..steps-1 are built (the streaming front end may have made the rest already) */
static int build_wf(ctx_t *c, int b_lo, int nb, uint8_t *mag, uint8_t *scr, sig_t *sg, int nsub, int steps)
{
    const int nfft = c->nfft, sub = c->sub, LAG = 3 * sub, RL = nfft + LAG;
    uintptr_t p = (uintptr_t)scr;
    float *y = (float *)p;          p = al8(p + sizeof(float) * (uint32_t)nfft);
    float *ring = (float *)p;       p = al8(p + sizeof(float) * (uint32_t)RL);
    float *rd = (float *)p;         p = al8(p + sizeof(float) * (uint32_t)sub);
    sst_t *ss = (sst_t *)p;
    const float fscale = 4294967296.0f / (float)c->fs;
    zero(ring, sizeof(float) * (uint32_t)RL);
    zero(ss, sizeof(sst_t) * (uint32_t)(nsub + 1));
    int32_t n = c->s0;                                       /* next input sample */
    int32_t m = c->s0 - LAG;                                 /* next output (residual) sample */
    int rin = LAG, rout = 0;                                 /* their ring slots (the ring starts at stream index m) */
    int off = 0;
    for (int step = -LAG / sub; step < steps; step++) {
        /* input chunk [n, n+sub) -> ring + correlations */
        int k = 0;
        stg(c, JS8_T_READ);
        while (k < sub && n + k < 0) rd[k++] = 0.0f;
        if (k < sub && (uint32_t)(n + k) < c->src->len) {
            js8_stats.nrdch++;
            int r = c->src->read(c->src->ctx, (uint32_t)(n + k), rd + k, (uint32_t)(sub - k));
            if (r < 0) return JS8_ERR_IO;
            k += r;
        }
        while (k < sub) rd[k++] = 0.0f;
        stg(c, nsub ? JS8_T_SUB : JS8_T_FFT);
        js8_stats.nsubch += (uint32_t)nsub;
        for (int j = 0; j < sub; j++) { ring[rin] = rd[j]; if (++rin == RL) rin = 0; }
        for (int i = 0; i < nsub; i++) sig_corr(c, &sg[i], &ss[i], n, rd, sub, fscale);
        n += sub;
        /* output chunk [m, m+sub): subtract */
        for (int i = 0; i < nsub; i++) sig_sub(c, &sg[i], &ss[i], m, ring, rout, RL, sub, fscale);
        m += sub; rout += sub; if (rout >= RL) rout -= RL;
        if (step < 0) { if (poll(c)) return JS8_ERR_ABORT; continue; }
        /* STFT frame = residual [m - nfft, m) = ring slots from rout - nfft */
        int ri = rout - nfft; if (ri < 0) ri += RL;
        stg(c, JS8_T_FFT);
        wf_row(c, ring, ri, RL, y, b_lo, nb, mag + off, 1); off += nb * FOSR;
        if (poll(c)) return JS8_ERR_ABORT;
    }
    for (int i = 0; i < nsub; i++) {                         /* frequency correction from the symbol phase steps */
        float pr = ss[i].pr, pi = ss[i].pi;
        if (pr > 0 && (pi < 0 ? -pi : pi) < pr) sg[i].f0 += (pi / pr) * (float)c->fs / (6.2831853f * (float)c->block);
    }
    return 0;
}

/* ---- candidate decoding: LLRs from the waterfall, BP, CRC-12 */

/* BP: layered (check by check) sum-product in 16-bit integers.
 * L[] = minus the bit totals in 1/8 LLR units (L < 0 = bit 1; index 1..174 as in kJS8_LDPC_Nm), r[] = check-to-bit
 * messages per edge in check order.  The check rule 2 atanh(prod tanh(x/2)) is evaluated pairwise as
 * a [+] b = sign * (min(|a|,|b|) + f(|a|+|b|) - f(||a|-|b||)), f(x) = ln(1 + e^-x) from a small table (exact
 * up to the 1/8 rounding), with forward / backward partial results per check: no tanh / atanh, no division, no
 * float in the loop.  A sweep updates every check once and the totals at once, so it converges in about half
 * the iterations of a flooding decoder: max_iters counts flooding iterations (js8_tune.iters, JS8Call: 30) and
 * (max_iters + 1) / 2 sweeps are made.  The early stop of WSJT-X / JS8Call (no progress in 5 checks after 10) is
 * kept.  The flooding float decoder this replaces (ft8_lib's bp_decode) is kept below under JS8_BP_SPA for the
 * host benchmark.  (A normalised min-sum, 7/8 of the smallest input, is about a third cheaper and equal on white
 * noise, but lost 2 of 547 frames on the real recordings - the overlapping pair of A_2_6 - so the exact rule is used.)
 * Returns the number of unsatisfied checks (0 = codeword in plain). */
#ifndef JS8_BP_SPA
static const uint8_t bxt[24] = { 6, 5, 5, 4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0 };   /* round(8 ln(1 + e^(-i/8))), 0 from i = 22 */
static inline int bxp(int a, int b)              /* a [+] b, written without branches (conditional moves) */
{
    int ma = a < 0 ? -a : a, mb = b < 0 ? -b : b;
    int mn = ma < mb ? ma : mb, s = ma + mb, d = s - 2 * mn, sg = (a ^ b) >> 31;
    mn += bxt[s < 23 ? s : 23] - bxt[d < 23 ? d : 23];
    mn = mn < 0 ? 0 : mn;
    return (mn ^ sg) - sg;
}
static int bp(ctx_t *c, const float *llr, int max_iters, uint8_t *plain)
{
    int16_t *L = c->bL, *r = c->br;
    int nclast = LM, ncnt = 0;
    max_iters = (max_iters + 1) >> 1;
    js8_stats.nbp++; js8_stats.bpn[c->ipass]++;
    for (int n = 0; n < LN; n++) {
        float v = llr[n] * -8.0f;
        L[n + 1] = (int16_t)(v > 2040.0f ? 2040 : v < -2040.0f ? -2040 : (int)v);
    }
    L[0] = 0;                                                /* rows of 5 or 6 are padded with 0 in kJS8_LDPC_Nm */
    for (int e = 0; e < LM * 7; e++) r[e] = 0;
    for (int iter = 0; ; iter++) {
        int ncheck = 0;
        js8_stats.bp_iters++; js8_stats.bpit[c->ipass]++;
        for (int m = 0; m < LM; m++) {                       /* syndrome of the hard decisions (sign bits) */
            const uint8_t *nm = kJS8_LDPC_Nm[m];
            ncheck += ((L[nm[0]] ^ L[nm[1]] ^ L[nm[2]] ^ L[nm[3]] ^ L[nm[4]] ^ L[nm[5]] ^ L[nm[6]]) >> 15) & 1;   /* unused: nm = 0 */
        }
        if (ncheck == 0) {
            int any = 0;
            for (int n = 0; n < LN; n++) { plain[n] = L[n + 1] < 0; any |= plain[n]; }
            return any ? 0 : LM;                 /* all-zero: not a valid frame (its CRC would be 0x02A) */
        }
        if (iter > 0) {
            if (ncheck < nclast) ncnt = 0; else ncnt++;
            if (ncnt >= 5 && iter >= 10 && ncheck > 15) return ncheck;
        }
        nclast = ncheck;
        if (iter >= max_iters) break;
        for (int m = 0; m < LM; m++) {
            const uint8_t *nm = kJS8_LDPC_Nm[m];
            const int nr = kJS8_LDPC_Num_rows[m];            /* 5, 6 or 7 */
            int16_t *rm = r + 7 * m;
            int q[7], f[7], b, v;
            for (int i = 0; i < nr; i++) q[i] = L[nm[i]] - rm[i];
            f[0] = q[0];
            for (int i = 1; i < nr - 1; i++) f[i] = bxp(f[i - 1], q[i]);     /* f[i] = q[0] [+] .. [+] q[i] */
            b = q[nr - 1];                                                   /* b = q[i+1] [+] .. [+] q[nr-1] */
            for (int i = nr - 1; i >= 0; i--) {
                v = i == nr - 1 ? f[nr - 2] : i == 0 ? b : bxp(f[i - 1], b);
                if (i < nr - 1 && i > 0) b = bxp(b, q[i]);
                rm[i] = (int16_t)v;
                v += q[i];
                if (v > 8000) v = 8000; else if (v < -8000) v = -8000;
                L[nm[i]] = (int16_t)v;
            }
        }
    }
    return nclast;
}
#else
/* reference (host benchmark only): BP (sum-product) as ft8_lib's bp_decode, plus the WSJT-X / JS8Call early stop (no progress in 5 iterations
 * after 10).  th() clamps to +-1 beyond |x| = 4.97 and ath() is a rational function without poles on
 * [-1, 1] (denominator >= 120), so no inf / NaN can appear (JS8Call's C++ uses an unclamped atanh). */
static float th(float x)                         /* ft8_lib fast_tanh */
{
    if (x < -4.97f) return -1.0f;
    if (x > 4.97f) return 1.0f;
    float x2 = x * x;
    return x * (945.0f + x2 * (105.0f + x2)) / (945.0f + x2 * (420.0f + x2 * 15.0f));
}
static float ath(float x)                        /* ft8_lib fast_atanh */
{
    float x2 = x * x;
    return x * (945.0f + x2 * (-735.0f + x2 * 64.0f)) / (945.0f + x2 * (-1050.0f + x2 * 225.0f));
}
static void bp_init(ctx_t *c)
{
    for (int m = 0; m < LM; m++)
        for (int i = 0; i < kJS8_LDPC_Num_rows[m]; i++) {
            int n = kJS8_LDPC_Nm[m][i] - 1;
            for (int j = 0; j < 3; j++) if (kJS8_LDPC_Mn[n][j] - 1 == m) c->mpos[m][i] = (uint8_t)j;
        }
}
/* returns the number of unsatisfied checks (0 = codeword in plain) */
static int bp(ctx_t *c, const float *llr, int max_iters, uint8_t *plain)
{
    float (*tov)[3] = c->tov; float (*toc)[7] = c->toc; float *sum = c->sum;
    int nclast = LM, ncnt = 0;
    js8_stats.nbp++; js8_stats.bpn[c->ipass]++;
    for (int n = 0; n < LN; n++) tov[n][0] = tov[n][1] = tov[n][2] = 0;
    for (int iter = 0; iter <= max_iters; iter++) {          /* inclusive, as JS8Call (0..max_iterations) */
        int any = 0;
        js8_stats.bp_iters++; js8_stats.bpit[c->ipass]++;
        for (int n = 0; n < LN; n++) {
            sum[n] = llr[n] + tov[n][0] + tov[n][1] + tov[n][2];
            plain[n] = sum[n] > 0; any |= plain[n];
        }
        if (!any) return LM;                     /* all-zero: not a valid frame (its CRC would be 0x02A) */
        int ncheck = 0;
        for (int m = 0; m < LM; m++) {
            int x = 0;
            for (int i = 0; i < kJS8_LDPC_Num_rows[m]; i++) x ^= plain[kJS8_LDPC_Nm[m][i] - 1];
            ncheck += x;
        }
        if (ncheck == 0) return 0;
        if (iter > 0) {
            if (ncheck < nclast) ncnt = 0; else ncnt++;
            if (ncnt >= 5 && iter >= 10 && ncheck > 15) return ncheck;
        }
        nclast = ncheck;
        for (int m = 0; m < LM; m++)
            for (int i = 0; i < kJS8_LDPC_Num_rows[m]; i++) {
                int n = kJS8_LDPC_Nm[m][i] - 1;
                toc[m][i] = th(-(sum[n] - tov[n][c->mpos[m][i]]) / 2);
            }
        for (int m = 0; m < LM; m++) {
            int nr = kJS8_LDPC_Num_rows[m], small = 0;
            float all = 1.0f;
            for (int k = 0; k < nr; k++) { all *= toc[m][k]; if (toc[m][k] > -1e-6f && toc[m][k] < 1e-6f) small = 1; }
            for (int i = 0; i < nr; i++) {
                float t;
                if (!small) t = all / toc[m][i];             /* product of the others */
                else { t = 1.0f; for (int k = 0; k < nr; k++) if (k != i) t *= toc[m][k]; }
                int n = kJS8_LDPC_Nm[m][i] - 1;
                tov[n][c->mpos[m][i]] = -2 * ath(t);
            }
        }
    }
    return nclast;
}
#endif

/* codeword bits (0..86 parity, 87..173 message) -> 11-byte frame; 1 if the CRC-12 matches */
static int crc_ok(const uint8_t *bits174, uint8_t frame[JS8_FRAME_BYTES])
{
    int z = 1;
    for (int i = 0; i < JS8_FRAME_BYTES; i++) frame[i] = 0;
    for (int i = 0; i < LK; i++) if (bits174[LK + i]) frame[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
    if (!js8_crc12_check(frame)) return 0;
    for (int i = 0; i < 9; i++) if (frame[i]) z = 0;
    if (frame[9] & 0xE0) z = 0;
    return !z;                                   /* the all-zero message is not accepted */
}

#ifdef JS8_WITH_OSD
/* generator by columns: codeword bit n >= 87 is message bit n - 87; bit n < 87 is parity row n */
static void gen_init(ctx_t *c)
{
    for (int n = 0; n < LN; n++) {
        c->gc[n][0] = c->gc[n][1] = c->gc[n][2] = 0;
        for (int j = 0; j < LK; j++)
            if (n >= LM ? n - LM == j : (kJS8_LDPC_generator[n][j >> 3] & (0x80u >> (j & 7))) != 0)
                c->gc[n][j >> 5] |= 1u << (j & 31);
    }
}
/* OSD: order 1 + pairs among the nt least reliable pivots; best codeword in cw and its soft distance.
 * (Works on any information set: the generator is reduced on the most reliable independent columns.) */
static int osd(ctx_t *c, const float *llr, uint8_t *cw, float *dmin)
{
    uint8_t *perm = c->osd_perm, *piv = c->osd_piv; float *r = c->osd_r, *rp = c->osd_rp;
    uint32_t (*M)[NW] = c->w, h[NW], m0[NW], cc[NW], mm[NW];
    for (int i = 0; i < LN; i++) { r[i] = llr[i] < 0 ? -llr[i] : llr[i]; perm[i] = (uint8_t)i; }
    for (int i = 1; i < LN; i++) {               /* by reliability, descending (insertion sort, stable) */
        uint8_t v = perm[i]; float rv = r[v]; int j = i - 1;
        while (j >= 0 && r[perm[j]] < rv) { perm[j + 1] = perm[j]; j--; }
        perm[j + 1] = v;
    }
    for (int j = 0; j < LK; j++) for (int w = 0; w < NW; w++) M[j][w] = 0;
    for (int i = 0; i < LN; i++) {               /* column perm[i] of the generator -> column i */
        const uint32_t *col = c->gc[perm[i]];
        for (int q = 0; q < 3; q++)
            for (uint32_t x = col[q]; x; x &= x - 1) { int j = q * 32 + __builtin_ctz(x); M[j][i >> 5] |= 1u << (i & 31); }
    }
    for (int w = 0; w < NW; w++) h[w] = 0;
    for (int i = 0; i < LN; i++) if (llr[perm[i]] > 0) h[i >> 5] |= 1u << (i & 31);
    int k = 0;
    for (int col = 0; col < LN && k < LK; col++) {           /* reduced row echelon form */
        int w = col >> 5, pr = -1; uint32_t b = 1u << (col & 31);
        for (int row = k; row < LK; row++) if (M[row][w] & b) { pr = row; break; }
        if (pr < 0) continue;
        if (pr != k) for (int q = 0; q < NW; q++) { uint32_t t = M[k][q]; M[k][q] = M[pr][q]; M[pr][q] = t; }
        for (int row = 0; row < LK; row++)
            if (row != k && (M[row][w] & b)) for (int q = 0; q < NW; q++) M[row][q] ^= M[k][q];
        piv[k++] = (uint8_t)col;
    }
    if (k < LK) return 0;
    for (int w = 0; w < NW; w++) cc[w] = 0;      /* order 0: info bits = hard decisions at the pivots */
    for (int j = 0; j < LK; j++) if (h[piv[j] >> 5] >> (piv[j] & 31) & 1u) for (int q = 0; q < NW; q++) cc[q] ^= M[j][q];
    for (int q = 0; q < NW; q++) m0[q] = cc[q] ^ h[q];
#define DIST(v, out) do { float d_ = 0; for (int q_ = 0; q_ < NW && d_ < best; q_++) { uint32_t x_ = (v)[q_]; \
        while (x_) { int b_ = __builtin_ctz(x_); x_ &= x_ - 1; d_ += rp[q_ * 32 + b_]; } } out = d_; } while (0)
    for (int i = 0; i < NW * 32; i++) rp[i] = i < LN ? r[perm[i]] : 0.0f;
    float best = 3.0e38f; DIST(m0, best);
    int b1 = -1, b2 = -1, nt = js8_tune.osd_nt;
    if (nt > LK) nt = LK;
    for (int i = 0; i < LK; i++) {
        float d;
        for (int q = 0; q < NW; q++) mm[q] = m0[q] ^ M[i][q];
        DIST(mm, d);
        if (d < best) { best = d; b1 = i; b2 = -1; }
    }
    if (js8_tune.osd >= 2)
        for (int i = LK - nt; i < LK; i++)
            for (int j = i + 1; j < LK; j++) {
                float d;
                for (int q = 0; q < NW; q++) mm[q] = m0[q] ^ M[i][q] ^ M[j][q];
                DIST(mm, d);
                if (d < best) { best = d; b1 = i; b2 = j; }
            }
#undef DIST
    for (int q = 0; q < NW; q++) {
        mm[q] = m0[q]; if (b1 >= 0) mm[q] ^= M[b1][q]; if (b2 >= 0) mm[q] ^= M[b2][q];
        cc[q] = mm[q] ^ h[q];
    }
    for (int i = 0; i < LN; i++) cw[perm[i]] = (uint8_t)(cc[i >> 5] >> (i & 31) & 1u);
    *dmin = best;
    return 1;
}
#endif

static void normalize(float *v, float target_sd)
{
    float s = 0, s2 = 0;
    for (int i = 0; i < LN; i++) { s += v[i]; s2 += v[i] * v[i]; }
    float var = (s2 - s * s / LN) / LN;
    if (var <= 0) var = s2 / LN;
    if (var <= 0) return;
    float f = target_sd / dg_sqrtf(var);
    for (int i = 0; i < LN; i++) v[i] *= f;
}

static const uint8_t *cand_mag(const wf_t *wf, const cand_t *cd)
{
    int o = ((cd->time_offset * TOSR + cd->time_sub) * FOSR + cd->freq_sub) * wf->num_bins + cd->freq_offset;
    return wf->mag + o;
}

static int hard_sync(const wf_t *wf, const cand_t *cd)
{
    const uint8_t *m = cand_mag(wf, cd);
    int n = 0;
    for (int g = 0; g < NUM_SYNC; g++)
        for (int k = 0; k < LEN_SYNC; k++) {
            int s = g * SYNC_OFFSET + k, blk = cd->time_offset + s;
            if (blk < 0 || blk >= wf->num_blocks) continue;
            const uint8_t *p = m + s * wf->block_stride;
            int bi = 0; for (int j = 1; j < 8; j++) if (p[j] > p[bi]) bi = j;
            if (bi == js8_costas_normal[g][k]) n++;
        }
    return n;
}

/* power of tone j of symbol `sym` of candidate cd, waterfall shifted by dt rows / dfh half bins (0 outside) */
static float cellp(const ctx_t *c, const wf_t *wf, const cand_t *cd, int sym, int j, int dt, int dfh)
{
    int fs = cd->freq_sub + dfh, fo = cd->freq_offset;
    if (fs < 0) { fs += FOSR; fo--; } else if (fs >= FOSR) { fs -= FOSR; fo++; }
    int row = (cd->time_offset + sym) * TOSR + cd->time_sub + dt, bin = fo + j;
    if (row < 0 || row >= wf->num_blocks * TOSR || bin < 0 || bin >= wf->num_bins) return 0.0f;
    return c->lin[wf->mag[(row * FOSR + fs) * wf->num_bins + bin]];
}
static float parab(float em, float e0, float ep);

/* bit metrics of one symbol: tone = 4*b0 + 2*b1 + b2 (natural binary, no Gray map); positive = bit 1 */
static void bitmet(const float *s, float *l)
{
    l[0] = mx(mx(s[4], s[5]), mx(s[6], s[7])) - mx(mx(s[0], s[1]), mx(s[2], s[3]));
    l[1] = mx(mx(s[2], s[3]), mx(s[6], s[7])) - mx(mx(s[0], s[1]), mx(s[4], s[5]));
    l[2] = mx(mx(s[1], s[3]), mx(s[5], s[7])) - mx(mx(s[0], s[2]), mx(s[4], s[6]));
}
/* LLRs of a candidate from the waterfall cells; metric 0 = dB (JS8Call's "log" metric), 1 = amplitude
 * (JS8Call's first metric), 2 = sqrt(amplitude) (the FT8 app's choice for the 0.5 dB waterfall) */
static void llr_grid(const ctx_t *c, const wf_t *wf, const cand_t *cd, int metric, float *llr)
{
    const uint8_t *m = cand_mag(wf, cd);
    for (int k = 0; k < JS8_ND; k++) {
        int si = JS8_DATA_SYM_POS(k), blk = cd->time_offset + si, bi = 3 * k;
        if (blk < 0 || blk >= wf->num_blocks) { llr[bi] = llr[bi + 1] = llr[bi + 2] = 0; continue; }
        const uint8_t *p = m + si * wf->block_stride;
        float s[8];
        if (metric == 0) for (int j = 0; j < 8; j++) s[j] = (float)p[j] * 0.5f;
        else if (metric == 1) for (int j = 0; j < 8; j++) s[j] = c->amp[p[j]];
        else for (int j = 0; j < 8; j++) s[j] = dg_sqrtf(c->amp[p[j]]);
        bitmet(s, llr + bi);
    }
    normalize(llr, js8_tune.scale);
    js8_stats.nllr++;
}
/* LLRs with the tone powers interpolated towards the true time / frequency (parabola on the Costas tones);
 * 0 if the candidate sits on the grid already */
static int llr_interp(const ctx_t *c, const wf_t *wf, const cand_t *cd, float *llr)
{
    float e[3][3];
    for (int a = -1; a <= 1; a++)
        for (int b = -1; b <= 1; b++) {
            float sum = 0;
            for (int g = 0; g < NUM_SYNC; g++)
                for (int k = 0; k < LEN_SYNC; k++) sum += cellp(c, wf, cd, g * SYNC_OFFSET + k, js8_costas_normal[g][k], a, b);
            e[a + 1][b + 1] = sum;
        }
    float ta = parab(e[0][1], e[1][1], e[2][1]), fb = parab(e[1][0], e[1][1], e[1][2]);
    int dt = ta < 0 ? -1 : 1, df = fb < 0 ? -1 : 1;
    float wa = ta < 0 ? -ta : ta, wb = fb < 0 ? -fb : fb;
    if (wa < 0.05f && wb < 0.05f) return 0;
    float w00 = (1 - wa) * (1 - wb), w10 = wa * (1 - wb), w01 = (1 - wa) * wb, w11 = wa * wb;
    for (int k = 0; k < JS8_ND; k++) {
        int si = JS8_DATA_SYM_POS(k), blk = cd->time_offset + si, bi = 3 * k;
        if (blk < 0 || blk >= wf->num_blocks) { llr[bi] = llr[bi + 1] = llr[bi + 2] = 0; continue; }
        float s[8];
        for (int j = 0; j < 8; j++) {
            float pw = w00 * cellp(c, wf, cd, si, j, 0, 0) + w10 * cellp(c, wf, cd, si, j, dt, 0)
                     + w01 * cellp(c, wf, cd, si, j, 0, df) + w11 * cellp(c, wf, cd, si, j, dt, df);
            s[j] = js8_tune.llr5 == 0 ? 10.0f * dg_log10f(pw + 1e-30f) : js8_tune.llr5 == 1 ? dg_sqrtf(pw) : dg_sqrtf(dg_sqrtf(pw));
        }
        bitmet(s, llr + bi);
    }
    normalize(llr, js8_tune.scale);
    js8_stats.nllr++;
    return 1;
}

/* one decoding attempt: BP (then OSD if enabled), JS8Call's hard-error limits for the pass, CRC-12.
 * llr is used as given; `cut` leading LLRs are treated as erased (passes 3 and 4). */
static int attempt_(ctx_t *c, const cand_t *cd, const float *llr_in, int cut, int ipass, uint8_t *frame, int *nhard, int *how)
{
    float llr[LN];
    uint8_t plain[LN];
    (void)cd;
    c->ipass = ipass - 1; stg(c, JS8_T_BP1 + ipass - 1);
    for (int i = 0; i < LN; i++) llr[i] = i < cut ? 0.0f : llr_in[i];
    int ok = bp(c, llr, js8_tune.iters, plain) == 0, osd_used = 0;
#ifdef JS8_WITH_OSD
    if (!ok && js8_tune.osd && cd->score >= js8_tune.osd_min) {
        float dm;
        if (osd(c, llr, plain, &dm) && dm <= js8_tune.osd_dmax) { ok = 1; osd_used = 1; }
    }
#endif
    if (!ok) return 0;
    int nh = 0;                                  /* channel bits that disagree with the codeword */
    for (int i = 0; i < LN; i++) if (plain[i] ? llr[i] < 0 : llr[i] > 0) nh++;
    if (nh > js8_tune.nhmax) return 0;
    if (ipass > 2 && nh > js8_tune.nh34) return 0;
    if (ipass == 4 && nh > js8_tune.nh4) return 0;
    if (!crc_ok(plain, frame)) return 0;
    *nhard = nh; *how = ipass + (osd_used ? 8 : 0);
    return 1;
}
static int attempt(ctx_t *c, const cand_t *cd, const float *llr_in, int cut, int ipass, uint8_t *frame, int *nhard, int *how)
{
    int ok = attempt_(c, cd, llr_in, cut, ipass, frame, nhard, how);
    if (ok) js8_stats.bpok[ipass - 1]++;
    stg(c, JS8_T_LLR);
    return ok;
}

static int dbg_ns;
static int decode_cand(ctx_t *c, const wf_t *wf, const cand_t *cd, uint8_t *frame, int *nhard, int *how, int lim)
{
    float la[LN];
    const int pm = js8_tune.passmask & lim;                  /* lim: attempts allowed for this candidate */
    dbg_ns = hard_sync(wf, cd);
    if (dbg_ns < js8_tune.nsync_min) return 0;
    js8_stats.ntried++;
    /* up to five BP runs per candidate: poll between them (one run is 31 iterations at most) */
    if (pm & 0x0D) {
        llr_grid(c, wf, cd, js8_tune.llr, la);
        if ((pm & 1) && attempt(c, cd, la, 0, 1, frame, nhard, how)) return 1;
        if (poll(c)) return 0;
    }
    if ((pm & 2) && dbg_ns >= js8_tune.nsync2) {
        float lb[LN];
        llr_grid(c, wf, cd, js8_tune.llr2, lb);
        if (attempt(c, cd, lb, 0, 2, frame, nhard, how)) return 1;
        if (poll(c)) return 0;
    }
    if (pm & 16) {
        float lb[LN];
        if (llr_interp(c, wf, cd, lb) && attempt(c, cd, lb, 0, 5, frame, nhard, how)) return 1;
        if (poll(c)) return 0;
    }
    if (dbg_ns >= js8_tune.nsync34) {
        if ((pm & 4) && attempt(c, cd, la, 24, 3, frame, nhard, how)) return 1;
        if (poll(c)) return 0;
        if ((pm & 8) && attempt(c, cd, la, 48, 4, frame, nhard, how)) return 1;
    }
    return 0;
}

/* the same 75 bits (payload + i3; the CRC follows from them) decoded before?  near_only: only signals
 * within dup_hz count.  Returns 1 + index, or 0. */
static int seen(const ctx_t *c, const uint8_t *f, float f0, int near_only)
{
    for (int i = 0; i < c->nsig; i++) {
        int j = 0;
        while (j < 9 && c->dec[i][j] == f[j]) j++;
        if (j < 9 || ((c->dec[i][9] ^ f[9]) & 0xE0)) continue;
        if (near_only) { float d = c->sig[i].f0 - f0; if (d < -js8_tune.dup_hz || d > js8_tune.dup_hz) continue; }
        return i + 1;
    }
    return 0;
}

/* sum of the tone-bin powers of a decoded signal, waterfall rows shifted by dt (half symbols) / dfh (half bins) */
static float tone_energy(const ctx_t *c, const wf_t *wf, const cand_t *cd, const uint8_t *tones, int dt, int dfh)
{
    int fs = cd->freq_sub + dfh, fo = cd->freq_offset;
    while (fs < 0) { fs += FOSR; fo--; }
    while (fs >= FOSR) { fs -= FOSR; fo++; }
    float e = 0;
    for (int k = 0; k < NSYM; k++) {
        /* The five energies (centre, +-1 row, +-1 half bin) are compared with each other, so they must be sums
         * over the SAME symbols: a symbol counts only if it is inside the waterfall for every one of the shifts.
         * (Summing whatever was in range made a signal at the edge of a band pass look weaker on the outer side:
         * 2 Hz frequency error at the split between two band passes, found by "make apparena".) */
        int row0 = (cd->time_offset + k) * TOSR + cd->time_sub, bin0 = cd->freq_offset + tones[k];
        if (row0 - 1 < 0 || row0 + 1 >= wf->num_blocks * TOSR || bin0 - 1 < 0 || bin0 + 1 >= wf->num_bins) continue;
        e += c->lin[wf->mag[((row0 + dt) * FOSR + fs) * wf->num_bins + fo + tones[k]]];
    }
    return e;
}
/* the time parabola is biased (the energy-vs-offset curve of a Hann-2T window is not a parabola): measured map
 * from its vertex (|d|, grid units of half a symbol) to the true offset (table of the FT8 app) */
static const float tmap_d[9] = { 0.0f, 0.04f, 0.08f, 0.12f, 0.165f, 0.22f, 0.29f, 0.375f, 0.49f };
static float tcal(float d)
{
    float a = d < 0 ? -d : d, u = 0.5f;
    for (int i = 1; i < 9; i++)
        if (a <= tmap_d[i]) { u = 0.0625f * ((float)(i - 1) + (a - tmap_d[i - 1]) / (tmap_d[i] - tmap_d[i - 1])); break; }
    if (u > 0.5f) u = 0.5f;
    return d < 0 ? -u : u;
}
static float parab(float em, float e0, float ep)              /* vertex offset of a parabola through -1, 0, +1 */
{
    float den = em - 2.0f * e0 + ep;
    if (!(e0 >= em && e0 >= ep)) return em > ep ? -0.5f : 0.5f;   /* the neighbour row is better: half way */
    if (den >= 0) return 0.0f;
    float d = 0.5f * (em - ep) / den;
    return d < -0.5f ? -0.5f : d > 0.5f ? 0.5f : d;
}

/* noise baseline per bin: the time-averaged power of each bin (frequency subdivision 0), then its pct-th
 * percentile over +-BW_HALF bins. */
#define BW_HALF 30
static void baseline(const ctx_t *c, const wf_t *wf, float *b, float *tmp)
{
    int rows = wf->num_blocks * TOSR, nb = wf->num_bins;
    for (int bin = 0; bin < nb; bin++) {
        float sum = 0;
        for (int row = 0; row < rows; row++) sum += c->lin[wf->mag[(row * FOSR) * nb + bin]];
        tmp[bin] = sum / (float)rows;
    }
    float w[2 * BW_HALF + 1];
    for (int bin = 0; bin < nb; bin++) {
        int lo = bin - BW_HALF, hi = bin + BW_HALF, n = 0;
        if (lo < 0) { hi -= lo; lo = 0; }
        if (hi > nb - 1) { lo -= hi - (nb - 1); hi = nb - 1; if (lo < 0) lo = 0; }
        for (int i = lo; i <= hi; i++) {                     /* insertion into sorted w */
            float v = tmp[i]; int j = n++;
            while (j > 0 && w[j - 1] > v) { w[j] = w[j - 1]; j--; }
            w[j] = v;
        }
        b[bin] = w[n * js8_tune.pct / 100];
    }
}

/* ---- candidate search: ft8_lib's ftx_find_candidates / ft8_sync_score (decode.c, MIT) with the three JS8
 * Costas groups and an abort poll; the score itself is computed row-wise, see find_candidates */
static void heap_down(cand_t *h, int n)
{
    int cur = 0;
    for (;;) {
        int l = 2 * cur + 1, r = l + 1, sm = cur;
        if (l < n && h[l].score < h[sm].score) sm = l;
        if (r < n && h[r].score < h[sm].score) sm = r;
        if (sm == cur) break;
        cand_t t = h[sm]; h[sm] = h[cur]; h[cur] = t; cur = sm;
    }
}
static void heap_up(cand_t *h, int n)
{
    int cur = n - 1;
    while (cur > 0) {
        int par = (cur - 1) / 2;
        if (!(h[cur].score < h[par].score)) break;
        cand_t t = h[par]; h[par] = h[cur]; h[cur] = t; cur = par;
    }
}
/* acc[i] += w * p[i] for n cells, n a multiple of 8: the one hot loop of the search, in a form the compiler turns
 * into a few NEON instructions per 8 cells (no loop versions, no remainder loop) */
__attribute__((noinline, noclone, optimize("tree-vectorize", "vect-cost-model=dynamic")))
static void acc_row(int16_t *restrict acc, const uint8_t *restrict p, int n, int w)
{
    for (int i = 0; i < n; i += 8)
        for (int j = 0; j < 8; j++) acc[i + j] = (int16_t)(acc[i + j] + w * p[i + j]);
}
#define FC_CH 256                /* frequency offsets scored at a time (512 B of stack) */
/* The score of ft8_sync_score is a sum of waterfall cells with small integer weights, and which cells take part
 * depends on the time offset only.  So for one time offset the cells are listed once and added row by row for
 * all frequency offsets together (same integers as the cell-by-cell form, identical scores and candidates). */
static int find_candidates(ctx_t *c, const wf_t *wf, int ncand, cand_t *heap, int min_score, const uint8_t *binok)
{
    int n = 0;
    cand_t cd;
    const int nfo = wf->num_bins - 7, bs = wf->block_stride;
    for (cd.time_sub = 0; cd.time_sub < TOSR; ++cd.time_sub)
        for (cd.freq_sub = 0; cd.freq_sub < FOSR; ++cd.freq_sub)
            for (cd.time_offset = -10; cd.time_offset < 20; ++cd.time_offset) {
                int32_t off[4 * NUM_SYNC * LEN_SYNC + NUM_SYNC * LEN_SYNC]; int8_t wt[NUM_SYNC * LEN_SYNC];
                int nc = 0, nn = NUM_SYNC * LEN_SYNC, num = 0;       /* off[0..nc): Costas cells, off[21..nn): neighbours */
                if (poll(c)) return -1;
                cd.freq_offset = 0;
                const int32_t o0 = (int32_t)(cand_mag(wf, &cd) - wf->mag);
                for (int m = 0; m < NUM_SYNC; ++m)
                    for (int k = 0; k < LEN_SYNC; ++k) {
                        int block = SYNC_OFFSET * m + k, abs_ = cd.time_offset + block, w = 0;
                        if (abs_ < 0) continue;
                        if (abs_ >= wf->num_blocks) break;
                        int sm = js8_costas_normal[m][k];
                        int32_t o = o0 + block * bs + sm;
                        if (sm > 0) { off[nn++] = o - 1; w++; }
                        if (sm < 7) { off[nn++] = o + 1; w++; }
                        if (k > 0 && abs_ > 0) { off[nn++] = o - bs; w++; }
                        if (k + 1 < LEN_SYNC && abs_ + 1 < wf->num_blocks) { off[nn++] = o + bs; w++; }
                        off[nc] = o; wt[nc++] = (int8_t)w; num += w;
                    }
                for (int f0 = 0; f0 < nfo; f0 += FC_CH) {
                    int16_t acc[FC_CH];
                    const int nf = nfo - f0 < FC_CH ? nfo - f0 : FC_CH, n8 = (nf + 7) & ~7;
                    /* n8: up to 7 cells past the last one are added too (never used; they are waterfall cells of the
                     * next row or, after the last row, bytes of the scratch area that follows the waterfall) */
                    for (int i = 0; i < n8; i++) acc[i] = 0;
                    for (int i = 0; i < nc; i++) acc_row(acc, wf->mag + off[i] + f0, n8, wt[i]);
                    for (int i = NUM_SYNC * LEN_SYNC; i < nn; i++) acc_row(acc, wf->mag + off[i] + f0, n8, -1);
                    for (int i = 0; i < nf; i++) {
                        if (binok && !binok[f0 + i]) continue;
                        js8_stats.nscore++;
                        int score = acc[i];
                        if (min_score > 0 && score < min_score * num) continue;   /* = score / num < min_score */
                        if (num > 0) score /= num;
                        if (score < min_score) continue;
                        cd.freq_offset = (int16_t)(f0 + i); cd.score = (int16_t)score;
                        if (n == ncand && cd.score > heap[0].score) { --n; heap[0] = heap[n]; heap_down(heap, n); }
                        if (n < ncand) { heap[n++] = cd; heap_up(heap, n); }
                    }
                }
            }
    for (int len = n; len > 1; ) {                           /* descending by score */
        cand_t t = heap[len - 1]; heap[len - 1] = heap[0]; heap[0] = t;
        len--; heap_down(heap, len);
    }
    return n;
}

/* A decoded signal is taken out of the waterfall in work: its tone cells (+- 1 row, +- 2 half bins) are lowered to
 * the noise baseline.  A weaker candidate that overlaps it then sees those cells as erasures instead of as a wrong
 * strong tone (no second read of the capture, no FFT). */
static void wf_erase(const ctx_t *c, wf_t *wf, const cand_t *cd, const uint8_t *tones)
{
    const int nb = wf->num_bins, rows = wf->num_blocks * TOSR;
    for (int k = 0; k < NSYM; k++) {
        int bin = cd->freq_offset + tones[k], h0 = bin * FOSR + cd->freq_sub, r0 = (cd->time_offset + k) * TOSR + cd->time_sub;
        int v = (int)(20.0f * dg_log10f(c->base[bin] + 1e-12f) + 240.0f);
        if (v < 0) v = 0;
        for (int r = r0 - 1; r <= r0 + 1; r++) {
            if (r < 0 || r >= rows) continue;
            for (int h = h0 - 2; h <= h0 + 2; h++) {
                if (h < 0 || h >= nb * FOSR) continue;
                uint8_t *q = wf->mag + (r * FOSR + (h & (FOSR - 1))) * nb + h / FOSR;
                if (*q > v) *q = (uint8_t)v;
            }
        }
    }
}
/* is the candidate inside the 8 tones (+- 9 bins) of one of the decoded signals j0.. , but not one of that
 * signal's own neighbouring grid points? */
static int near_sig(const ctx_t *c, const cand_t *cd, int b_lo, int j0)
{
    int ch = (b_lo + cd->freq_offset) * FOSR + cd->freq_sub, cr = cd->time_offset * TOSR + cd->time_sub;
    for (int j = j0; j < c->nsig; j++) {
        int dfq = ch - (int)(c->sig[j].f0 / c->df * FOSR + 0.5f);
        int dt = cr - ((c->sig[j].t0 - c->s0) / c->sub + TOSR);
        if (dfq >= -9 * FOSR && dfq <= 9 * FOSR && !(dfq >= -3 && dfq <= 3 && dt >= -3 && dt <= 3)) return 1;
    }
    return 0;
}

/* one search+decode round over a waterfall; returns 0, or JS8_ERR_ABORT */
/* restricted != 0 (every pass but the first): search only next to the signals new0..new1-1 (subtracted for the
 * first time) and, on the shifted time grid, next to the remembered strong sync failures */
static int round_(ctx_t *c, wf_t *wf, int b_lo, cand_t *cand, int restricted, int new0, int new1)
{
    uint8_t binok[1024], *bo = 0;
    const int nsf = c->phase ? c->nsf : 0;                    /* the list is used by the shifted pass, filled by the normal one */
    restricted = restricted && js8_tune.near;
    if (c->phase && !js8_tune.swide) new1 = new0;
    if (restricted && wf->num_bins <= 1024) {
        for (int b = 0; b < wf->num_bins; b++) binok[b] = 0;
        for (int j = new0; j < new1; j++) {
            int b0 = (int)(c->sig[j].f0 / c->df) - b_lo;
            for (int b = b0 - 9; b <= b0 + 9; b++) if (b >= 0 && b < wf->num_bins) binok[b] = 1;
        }
        for (int j = 0; j < nsf; j++) {
            int b0 = c->sf[j].hbin / FOSR - b_lo;
            for (int b = b0 - 2; b <= b0 + 2; b++) if (b >= 0 && b < wf->num_bins) binok[b] = 1;
        }
        bo = binok;
    }
    stg(c, JS8_T_SEARCH);
    int nc = find_candidates(c, wf, js8_tune.max_cand, cand, js8_tune.min_score, bo);
    stg(c, JS8_T_LLR);
    if (nc < 0 || poll(c)) return JS8_ERR_ABORT;
    js8_stats.ncand += (uint32_t)nc;
    /* 0x0800: a clear Costas pattern (>= ev tones) before anything is erased from the waterfall (the evidence below) */
    if (js8_tune.ev)
        for (int i = 0; i < nc; i++) if (hard_sync(wf, &cand[i]) >= js8_tune.ev) cand[i].score |= 0x0800;
    /* again > 0: every signal decoded in this round has been erased from the waterfall (wf_erase); the candidates
     * that failed with a clear pattern inside one of the latest signals get another try on the spot */
    for (int again = 0, n0 = c->nsig; again <= js8_tune.erounds; again++) {
    if (again && c->nsig == n0) break;
    const int n1 = n0; n0 = c->nsig;
    for (int i = 0; i < nc; i++) {
        stg(c, JS8_T_LLR);
        if (poll(c)) return JS8_ERR_ABORT;
        const cand_t *cd = &cand[i];
        if (again) {
            if ((cd->score & 0x6000) != 0x2000 || !near_sig(c, cd, b_lo, n1)) continue;
            cand[i].score &= ~0x2000;
        }
        uint8_t frame[JS8_FRAME_BYTES];
        int how = 0, nhard = 0, dup = 0, lim = again ? js8_tune.emask : 0x1F;
        for (int j = 0; j < i && !dup; j++) {                /* a stronger candidate next to it decoded already */
            const cand_t *e = &cand[j];
            if (!(e->score & 0x5000)) continue;              /* 0x4000 marks a decoded candidate, 0x1000 a failed one (scores are < 0x0800) */
            int dt = (cd->time_offset * TOSR + cd->time_sub) - (e->time_offset * TOSR + e->time_sub);
            int dfq = (cd->freq_offset * FOSR + cd->freq_sub) - (e->freq_offset * FOSR + e->freq_sub);
            int nb = dt >= -js8_tune.skip && dt <= js8_tune.skip && dfq >= -js8_tune.skip && dfq <= js8_tune.skip;
            /* next to a stronger candidate that failed every attempt: the same signal, half a step off.  Near the
             * threshold such a neighbour still decodes (15 of 88 decodes at -19 dB), by attempt 1 or 5 - the others
             * are left out for it (a signal that syncs but does not decode costs 15 BP runs instead of 30). */
            if (!(e->score & 0x4000)) { if (nb && js8_tune.nbr) lim &= js8_tune.nbr; continue; }
            if (nb) dup = 1;
            /* inside the 8 tones (+- foot bins) of a signal decoded in this pass: mostly that signal's own sidelobes
             * (a third of the failed candidates of the off-air recordings).  One attempt now; if it is a second
             * station, the next pass sees it with the first one subtracted and gives it all attempts. */
            if (js8_tune.foot && !again && dfq >= -js8_tune.foot * FOSR && dfq <= js8_tune.foot * FOSR) lim = 1;
        }
        if (dup) continue;
        /* The top tone-0 bin of a band pass that is not the last one belongs to the NEXT pass (which starts one bin
         * below it): there the candidate has neighbours on both sides.  Decoding it here, or decoding a weaker
         * neighbour instead, gave a signal just above the split a frequency that was 2 Hz off (its true grid point
         * was outside this pass).  It is marked like a decoded one so that its neighbours are skipped too. */
        if (cd->freq_offset >= c->own_fo) { cand[i].score |= 0x4000; continue; }
        float fc = ((float)(b_lo + cd->freq_offset) + (float)cd->freq_sub / FOSR) * c->df;
        if (restricted) {
            int near = 0;
            for (int j = new0; j < new1 && !near; j++) { float d = fc - c->sig[j].f0; if (d > -9.0f * c->df && d < 9.0f * c->df) near = 1; }
            for (int j = 0; j < nsf && !near; j++) { int d = (b_lo + cd->freq_offset) * FOSR + cd->freq_sub - c->sf[j].hbin; if (d >= -2 * FOSR && d <= 2 * FOSR) near = 1; }
            if (!near) continue;
        }
        int okd = decode_cand(c, wf, cd, frame, &nhard, &how, lim);
        if (!okd && !c->phase && (dbg_ns >= js8_tune.nstrong || (cd->score & 0x07FF) >= js8_tune.sstrong) && c->nsf < MAX_SF && !c->aborted && !again) {
            c->sf[c->nsf].hbin = (int16_t)((b_lo + cd->freq_offset) * FOSR + cd->freq_sub);
            c->sf[c->nsf].row = (int16_t)(cd->time_offset * TOSR + cd->time_sub);
            c->nsf++;
        }

        DBG("CAND %.2f %.3f %d %d %d %d\n", (double)fc,
            (double)((float)c->s0 / (float)c->fs + ((float)cd->time_offset - 1.0f + (float)cd->time_sub / TOSR) * (float)c->block / (float)c->fs - c->nom),
            cd->score, dbg_ns, okd ? nhard : -1, okd ? how : 0);
        if (okd) cand[i].score |= 0x4000;
        else if (dbg_ns >= js8_tune.nsync_min) cand[i].score |= (int16_t)(js8_tune.ev && dbg_ns >= js8_tune.ev ? 0x3000 : 0x1000);   /* tried in vain; 0x2000: with a clear pattern */
        if (!okd || seen(c, frame, fc, 1) || c->nsig >= MAX_SIG) continue;
        stg(c, JS8_T_FIN); js8_stats.nsig++;
        int first_time = !seen(c, frame, fc, 0);             /* the same frame elsewhere in the band: subtract, report once */
        for (int j = 0; j < JS8_FRAME_BYTES; j++) c->dec[c->nsig][j] = frame[j];
        sig_t *s = &c->sig[c->nsig++];
        js8_encode(frame, js8_costas_normal, s->tones);
        /* refine time / frequency on the waterfall grid */
        float e0 = tone_energy(c, wf, cd, s->tones, 0, 0);
        float dtr = parab(tone_energy(c, wf, cd, s->tones, -1, 0), e0, tone_energy(c, wf, cd, s->tones, 1, 0));
        float dfr = parab(tone_energy(c, wf, cd, s->tones, 0, -1), e0, tone_energy(c, wf, cd, s->tones, 0, 1));
        float tsym = (float)cd->time_offset - 1.0f + ((float)cd->time_sub + tcal(dtr)) / TOSR;
        float start = (float)c->s0 + tsym * (float)c->block;          /* samples */
        s->t0 = (int32_t)(start + (start >= 0 ? 0.5f : -0.5f));
        s->f0 = ((float)(b_lo + cd->freq_offset) + ((float)cd->freq_sub + dfr) / FOSR) * c->df;
        /* SNR: mean tone-bin power over the noise baseline */
        float xs = 0, xb = 0;
        const uint8_t *mg = cand_mag(wf, cd);
        for (int k = 0; k < NSYM; k++) {
            int blk = cd->time_offset + k;
            if (blk < 0 || blk >= wf->num_blocks) continue;
            xs += c->lin[mg[k * wf->block_stride + s->tones[k]]];
            xb += c->base[cd->freq_offset + s->tones[k]];
        }
        float snr = JS8_SNR_FLOOR;
        if (xb > 0) {
            float a = xs / xb - 1.0f;
            if (a < 0.001f) a = 0.001f;
            snr = 10.0f * dg_log10f(a) + js8_tune.snr_off;
            if (snr < JS8_SNR_FLOOR) snr = JS8_SNR_FLOOR;
        }
        if (js8_tune.erounds) wf_erase(c, wf, cd, s->tones);
        if (!first_time && !js8_tune.dup_out) continue;
        if (c->nout >= c->maxout) continue;
        js8_result_t *r = &c->out[c->nout++];
        for (int j = 0; j < JS8_FRAME_BYTES; j++) r->frame[j] = frame[j];
        r->pass = (uint8_t)how; r->nhard = (uint8_t)nhard; r->nsync = (uint8_t)dbg_ns;
        r->freq_hz = s->f0;
        r->dt_s = start / (float)c->fs - c->nom;
        r->snr_db = snr;
        r->quality = cd->score & 0x07FF;
        DBG("  dec sc=%d to=%d ts=%d fo=%d fs=%d how=%d nh=%d dt%+.2f df%+.2f\n", cd->score, cd->time_offset, cd->time_sub, cd->freq_offset, cd->freq_sub, how, nhard, (double)dtr, (double)dfr);
    }
    }
    /* evidence of a station under a decoded one: an undecoded candidate with >= ev Costas tones inside the 8 tones
     * (+- 9 bins) of a signal decoded in this pass, but not one of that signal's own neighbouring grid points */
    c->evid = 0;
    for (int i = 0; i < nc; i++)
        if ((cand[i].score & 0x4800) == 0x0800 && near_sig(c, &cand[i], b_lo, c->first)) c->evid = 1;
    stg(c, JS8_T_LLR);
    return 0;
}

/* ---- set-up shared by js8_decode() and the streaming front end: context + tables at the start of `work`,
 * band and arena layout.  Returns 0, 1 (less than half a signal: nothing to do) or JS8_ERR_ARGS. */
typedef struct { ctx_t *c; int b_lo, b_top, nb_max; uintptr_t fixed_end; uint32_t scratch, row; } lay_t;
static __attribute__((cold, noinline)) int setup(lay_t *L, uint32_t fs, uint32_t len, float f_lo, float f_hi, float nominal_start_s, void *work, uint32_t work_len)
{
    if (!work || ((uintptr_t)work & 7u) || work_len < JS8_WORK_MIN) return JS8_ERR_ARGS;
    if (fs < 6000 || fs > 12000 || (fs * 4) % 25) return JS8_ERR_ARGS;   /* fs*0.16 integer */
    if (!(nominal_start_s >= -30.0f && nominal_start_s <= 30.0f)) return JS8_ERR_ARGS;   /* also NaN; keeps the sample index in int32 */
    if (len > 0x7FFFFFFFu / 2u) return JS8_ERR_ARGS;
    uintptr_t base = (uintptr_t)work, p = base, end = base + work_len;
    ctx_t *c = (ctx_t *)p;                     p = al8(p + sizeof(ctx_t));
    zero(c, sizeof *c);
    L->c = c; c->nom = nominal_start_s;
    c->stage = JS8_T_MAG; if (js8_clock) c->tlast = c->t0 = js8_clock();      /* set-up (tables) counts as waterfall work */
    if (stft_geom(c, (int)fs)) return JS8_ERR_ARGS;
    c->s0 = (int32_t)((nominal_start_s - 0.5f) * (float)c->fs + (nominal_start_s >= 0.5f ? 0.5f : -0.5f));
    int32_t avail = (int32_t)len - c->s0;
    c->nblk = avail <= 0 ? 0 : (int)((avail + c->block - 1) / c->block);
    if (c->nblk > MAX_BLOCKS) c->nblk = MAX_BLOCKS;
    if (c->nblk < NSYM / 2) return 1;                        /* less than half a signal */

    /* band in 6.25 Hz bins: tone 0 in [f_lo, f_hi], tones 0..7 below fs/2 */
    int b_lo = (int)(f_lo / c->df), b_top = (int)(f_hi / c->df + 0.999f) + 8;
    int b_max = (c->N - FOSR) / FOSR;
    if (b_lo < 1) b_lo = 1;
    if (b_top > b_max) b_top = b_max;
    if (b_top - b_lo < 16) return JS8_ERR_ARGS;
    L->b_lo = b_lo; L->b_top = b_top;

    float *qs = (float *)p;                    p = al8(p + sizeof(float) * (uint32_t)(c->nfft / 4 + 1));
    stft_tables(c, qs);
    {   /* 0.5 dB per step from -120 dB */
        float v = 1e-12f; const float r = 1.12201845f;       /* 10^(0.5/10) */
        for (int i = 0; i < 256; i++) { c->lin[i] = v; c->amp[i] = dg_sqrtf(v); v *= r; }
    }
#ifdef JS8_WITH_OSD
    gen_init(c);
#endif
#ifdef JS8_BP_SPA
    bp_init(c);
#endif
    for (int i = 0; i <= STAB + STAB / 4; i++) { double sv, cv; dg_sincos(6.283185307179586 * (double)i / STAB, &sv, &cv); c->sn[i] = (float)sv; }
    L->fixed_end = p;
    /* after the waterfall: max(streaming STFT, candidate heap) */
    uint32_t stream_len = stream_bytes(c, MAX_SIG);
    uint32_t dec_len = (uint32_t)al8((uint32_t)js8_tune.max_cand * sizeof(cand_t));
#ifdef JS8_WITH_OSD
    dec_len += (uint32_t)(LK * NW * sizeof(uint32_t));
#endif
    L->scratch = stream_len > dec_len ? stream_len : dec_len;
    L->row = (uint32_t)(c->nblk * TOSR * FOSR);
    if (p + L->scratch + L->row * 24 + 512 > end) return JS8_ERR_ARGS;
    L->nb_max = (int)((end - p - L->scratch - 16) / (L->row + sizeof(float)));
    return 0;
}

/* ---- streaming front end (see js8dec.h): the waterfall of the first pass is built row by row while the
 * period is still being captured.  The rows are the ones build_wf() would make from the same samples (same
 * code, same arithmetic), so a decode with or without it gives identical results. */
static struct {
    ctx_t *c;                    /* 0 = no stream */
    uint32_t wlen, fs, cnt;      /* arena size; sample rate; samples received (= index of the next one) */
    int b_lo, b_top;
    int ra, r, left, wi;         /* rows ra.. are made here; next row to complete; samples until then; ring slot */
    int maxrows;
    uint8_t *mag; float *y;      /* y, ring: nfft floats each, after the waterfall */
} st;

void js8_stream_stop(void) { st.c = 0; }

static void st_put(const int16_t *x, uint32_t n)
{
    ctx_t *c = st.c;
    const int nfft = c->nfft, nb = st.b_top - st.b_lo;
    float *ring = st.y + nfft;
    while (n--) {
        ring[st.wi] = x ? (float)*x++ * (1.0f / 32768.0f) : 0.0f;
        if (++st.wi == nfft) st.wi = 0;
        st.cnt++;
        if (--st.left) continue;
        st.left = c->sub;
        if (st.r >= st.ra && st.r < st.maxrows)
            wf_row(c, ring, st.wi, nfft, st.y, st.b_lo, nb, st.mag + st.r * FOSR * nb, 0);
        st.r++;
    }
}
void js8_stream_put(const int16_t *x, uint32_t n) { if (st.c) st_put(x, n); }

__attribute__((cold)) int js8_stream_begin(void *work, uint32_t work_len, uint32_t fs, uint32_t max_len, float f_lo, float f_hi,
                     float nominal_start_s, uint32_t pos)
{
    lay_t L;
    st.c = 0;
    if (setup(&L, fs, max_len, f_lo, f_hi, nominal_start_s, work, work_len)) return 0;
    ctx_t *c = L.c;
    int nb = L.b_top - L.b_lo, sub = c->sub;
    /* no streaming if the band needs two band passes, or if the waterfall starts inside the capture (nominal start
     * > 0.5 s: build_wf() takes the samples before the origin as zeros, the stream would have the real ones) */
    if (nb > L.nb_max || c->s0 > 0) return 0;
    st.wlen = work_len; st.fs = fs; st.cnt = pos; st.b_lo = L.b_lo; st.b_top = L.b_top;
    st.mag = (uint8_t *)al8(L.fixed_end + sizeof(float) * (uintptr_t)nb);
    st.y = (float *)al8((uintptr_t)st.mag + (uintptr_t)nb * L.row);
    st.maxrows = c->nblk * TOSR;
    zero(st.y + c->nfft, sizeof(float) * (uint32_t)c->nfft);
    /* row r is the window [s0 + (r - 3) sub, s0 + (r + 1) sub); samples before the capture are zeros */
    st.r = (int)(((int32_t)pos - c->s0) / sub);
    st.left = (int)(c->s0 + (st.r + 1) * sub - (int32_t)pos);
    st.wi = 0;
    if (pos == 0) { st.ra = 0; if (st.r > st.maxrows) st.r = st.maxrows; zero(st.mag, (uint32_t)(st.r * FOSR * nb)); }
    else st.ra = st.r + 4;                                   /* first row whose window starts at or after pos */
    st.c = c;
    return 1;
}

int js8_decode(const dsrc_t *src, float f_lo, float f_hi, float nominal_start_s,
               js8_result_t *out, int maxout, void *work, uint32_t work_len, int (*should_abort)(void))
{
    ctx_t *sc = st.c;
    st.c = 0;                                                /* a decode always ends the stream */
    peak = 0;
    zero(&js8_stats, sizeof js8_stats);
    if (!src || !src->read || !out || maxout <= 0) return JS8_ERR_ARGS;
    lay_t L;
    int rc = setup(&L, src->fs, src->len, f_lo, f_hi, nominal_start_s, work, work_len);
    if (rc) return rc < 0 ? rc : 0;
    ctx_t *c = L.c;
    uintptr_t base = (uintptr_t)work, fixed_end = L.fixed_end;
    c->src = src; c->ab = should_abort; c->out = out; c->maxout = maxout;
    const int b_lo = L.b_lo, b_top = L.b_top, nb_max = L.nb_max;
    const uint32_t row = L.row, scratch = L.scratch, cand_len = (uint32_t)al8((uint32_t)js8_tune.max_cand * sizeof(cand_t));
    (void)cand_len;
    /* rows pre.. of the first waterfall were made by the streaming front end: same arena, band, origin, and
     * every sample of the capture was fed.  The rows that end after the capture are completed with zeros here. */
    int pre = c->nblk * TOSR;
    if (sc == c && st.wlen == work_len && st.b_lo == b_lo && st.b_top == b_top && st.cnt == src->len
        && pre <= st.maxrows && st.fs == src->fs && (int32_t)st.cnt + st.left == c->s0 + (st.r + 1) * c->sub) {
        stg(c, JS8_T_FFT);
        st.c = c;
        while (st.r < pre) { js8_stats.nfft++; st_put(0, (uint32_t)st.left); }
        st.c = 0;
        pre = st.ra;
    }

    for (int pb = b_lo; pb + 8 < b_top && !rc; ) {           /* band passes (one if it fits) */
        int nb = b_top - pb;
        if (nb > nb_max) nb = nb_max;
        float *bl = (float *)fixed_end;
        uint8_t *mag = (uint8_t *)al8(fixed_end + sizeof(float) * (uintptr_t)nb);
        uint8_t *scr = (uint8_t *)al8((uintptr_t)mag + (uintptr_t)nb * row);
        uint32_t used = (uint32_t)((uintptr_t)scr + scratch - base);
        if (used > peak) peak = used;
        c->base = bl; js8_stats.nband++;
        int first = c->first = c->nsig;                      /* signals of an earlier band pass are not subtracted */
        /* Passes over this band.  N = normal time grid, S = waterfall rows moved by half a row (40 ms).
         *   N0 looks everywhere; every later pass works on the audio minus everything decoded so far and looks
         *   only where something changed.
         *   An S pass is made only when an N pass left candidates with a clear Costas pattern undecoded
         *   (nsync >= nstrong or sync score >= sstrong; noise alone did that in 1 of 200 periods).
         *   Such a signal typically sits between two waterfall rows with a neighbour on top of it: on the
         *   off-air recording decode_air_test_1min the frame count used to depend on the capture start
         *   (15..18 of 18 when it moved by 5 ms steps); with one S pass it is 17..18 at every start.
         *   The S pass looks next to those failures only (swide = 1: also next to the new signals).
         * At most `subpasses` N passes and `shift` S passes; with shift = 0 this is the FT8 app's scheme.
         * Cost: one more waterfall (and one more read of the capture) per S pass; on x86 the decode time against
         * the FT8 decoder went from 0.93..1.20 to 1.06..1.30 (make ft8time). */
        int nN = 0, nS = 0, lastN = first, lastS = first, phase = 0;
        uint32_t pcost = 0;
        c->own_fo = pb + nb >= b_top ? nb : nb - 8;          /* not the last band pass: its top bin is left to the next */
        const int32_t s0n = c->s0;
        c->nsf = 0;
        for (;;) {
            int nsub = c->nsig - first;
            c->phase = phase; c->s0 = s0n + (phase ? c->sub / 2 : 0);
            DBG("band %d..%d (%d bins) pass %c%d: subtract %d, nblk %d, wf %u B, peak %u\n", pb, pb + nb, nb, phase ? 'S' : 'N', phase ? nS : nN, nsub, c->nblk, nb * row, used);
            stg(c, JS8_T_MAG);
            c->kind = phase ? 2 : nN > 0; js8_stats.nwf[c->kind]++;
            uint32_t tb = c->tlast;
            rc = build_wf(c, pb, nb, mag, scr, c->sig + first, nsub, pre);
            pre = c->nblk * TOSR;
            stg(c, JS8_T_MAG);
            pcost = c->tlast - tb;                           /* cost of the last pass over the capture (the first one may be partly or wholly streamed) */
            if (rc) break;
            wf_t wf;
            wf.num_blocks = c->nblk; wf.num_bins = nb; wf.mag = mag; wf.block_stride = TOSR * FOSR * nb;
            if (nN == 0) baseline(c, &wf, bl, (float *)scr);
            int before = c->nsig, from = phase ? lastS : lastN;
#ifdef JS8_WITH_OSD
            c->w = (uint32_t (*)[NW])(scr + cand_len);
#endif
            rc = round_(c, &wf, pb, (cand_t *)scr, nN > 0, from, before);
            if (phase) { nS++; lastS = before; c->nsf = 0; } else { nN++; lastN = before; }
            if (rc) break;
            /* a failure next to a signal that did decode is that signal's own neighbouring grid point */
            for (int i = 0; i < c->nsf; ) {
                int own = 0;
                for (int j = first; j < c->nsig && !own; j++) {
                    int dh = (int)(c->sig[j].f0 / c->df * FOSR + 0.5f) - c->sf[i].hbin;
                    int dr = (c->sig[j].t0 - s0n) / c->sub + TOSR - c->sf[i].row;
                    if (dh >= -3 && dh <= 3 && dr >= -2 && dr <= 3) own = 1;
                }
                if (own) c->sf[i] = c->sf[--c->nsf]; else i++;
            }
            /* swide (default): ONE kind of residual pass - every later pass subtracts everything decoded so far and
             * looks next to the signals that are new since the last pass of its kind, the S pass also next to the
             * strong failures.  A pass is made only if the one before decoded something (or left such failures), and
             * the grids alternate, so "N0 S" already gives a station under a stronger one its chance (before: N0 S N1
             * N2, where S looked at the failures only; each pass costs ~2 s on the radio). */
            int more = c->nsig > before && (!js8_tune.ev || c->evid);
            int canS = nS < js8_tune.shift && (c->nsf > 0 || (js8_tune.swide && more));
            int canN = nN < js8_tune.subpasses && (js8_tune.swide ? more : c->nsig > lastN);   /* something new to subtract */
            if (!phase && canS) phase = 1;
            else if (canN) phase = 0;
            else break;
            /* js8_tune.budget: a pass that cannot end in time is not started (a cut waterfall is lost work).
             * Estimate: the last pass made here + 1/4 for its candidates */
            if (js8_tune.budget > 0 && js8_clock && c->tlast - c->t0 + pcost + (pcost >> 2) > (uint32_t)js8_tune.budget) break;
        }
        c->s0 = s0n; c->phase = 0;
        if (rc || pb + nb >= b_top) break;
        pb += nb - 9;                                        /* next band pass: from one bin below the top bin left out above */
    }
    stg(c, JS8_T_MAG);                                       /* close the stage in work */
    if (rc == JS8_ERR_ABORT) return c->nout > 0 ? c->nout : JS8_ERR_ABORT;
    if (rc == JS8_ERR_IO) return c->nout > 0 ? c->nout : JS8_ERR_IO;
    if (rc) return rc;
    return c->nout;
}
