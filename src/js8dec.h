/* SPDX-License-Identifier: GPL-3.0-or-later AND MIT
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8dec.h - JS8 (JS8Call compatible, submode Normal) decoder for the IC-705 ICAP app.
 * Freestanding C, no malloc, no libc. Fork of the ICAP SDK's FT8 decoder (apps/ft8/ft8dec.c,
 * MIT, Copyright (c) 2026 Dawid SQ6EMM, which contains adapted parts of kgoba/ft8_lib,
 * Copyright (c) 2018 Kārlis Goba, MIT -
 * see the notice in js8dec.c). Output is the raw 87-bit frame; text rendering is js8msg.
 */
#ifndef JS8DEC_H
#define JS8DEC_H
#include <stdint.h>
#include "digi_util.h"   /* dsrc_t (SDK apps/digi-common) */

typedef struct {
    uint8_t frame[11];   /* 72 payload bits, 3 bits i3, CRC-12, one 0 bit (MSB first); CRC verified */
    uint8_t pass;        /* which decoding attempt succeeded: 1..4 = JS8 passes, 5 = interpolated metric,
                            +8 if OSD (only when enabled) */
    uint8_t nhard;       /* hard errors corrected (channel bits whose sign disagreed with the codeword) */
    uint8_t nsync;       /* hard Costas matches, 0..21 */
    float freq_hz;       /* audio frequency of tone 0 (JS8Call "offset") */
    float dt_s;          /* signal start minus nominal_start_s (0.5 s into the period for DT = 0) */
    float snr_db;        /* SNR in 2500 Hz reference bandwidth, clipped at JS8_SNR_FLOOR */
    int   quality;       /* sync score (ft8_lib units) */
} js8_result_t;

/* decoder knobs (fixed in the app; the host test drivers override them) */
typedef struct {
    int llr;             /* metric of pass 1/3/4: 0 = dB, 1 = amplitude, 2 = sqrt(amplitude) */
    int llr2;            /* metric of pass 2 */
    int llr5;            /* metric of pass 5 (interpolated powers) */
    int max_cand;        /* candidates kept per search */
    int min_score;       /* minimum sync score of a candidate */
    int iters;           /* BP iterations, counted as flooding iterations (JS8Call: 30): the layered decoder makes
                            (iters + 1) / 2 sweeps, each worth about two of them */
    int osd;             /* 0 = off, 1 = order 1, 2 = + pairs (only if compiled with JS8_WITH_OSD) */
    int osd_nt;          /* OSD: pairs among the nt least reliable pivots */
    int nhmax;           /* accept only nhard <= nhmax (JS8Call: < 60) */
    int nh34, nh4;       /* limits for passes 3..4 and pass 4 (JS8Call: 39, 30) */
    int subpasses;       /* 1 + number of subtraction passes */
    int nsync_min;       /* hard Costas matches needed to try a candidate (JS8Call: 7) */
    int nsync34;         /* ... needed for passes 3 and 4 */
    int passmask;        /* bit0..bit4 = passes 1..5 enabled */
    float scale;         /* LLR standard deviation fed to BP */
    float snr_off;       /* SNR calibration (dB) */
    int pct;             /* noise baseline percentile */
    int skip;            /* a candidate this close (grid units) to a decoded one is skipped */
    int near;            /* after a subtraction search only next to the removed signals */
    int dup_out;         /* 1 = also report the same frame found at another frequency (> dup_hz away) */
    float dup_hz;
    int osd_min;         /* OSD only for candidates with at least this sync score */
    float osd_dmax;      /* OSD: reject if the soft distance is larger */
    int shift;           /* extra passes on a time grid shifted by half a row (40 ms), made only when a
                            normal pass left candidates with nsync >= nstrong undecoded; 0 = none */
    int nstrong;         /* hard Costas matches (of 21) that make an undecoded candidate worth the shifted pass */
    int sstrong;         /* ... or this sync score */
    int swide;           /* shifted pass: 1 = also search next to the signals decoded since the last shifted pass */
    int nsync2;          /* hard Costas matches needed for attempt 2 (dB metric) */
    int foot;            /* a candidate within this many 6.25 Hz bins of a signal decoded in the same pass gets the
                            first attempt only (0 = off) */
    int ev;              /* swide: a further pass is made only if an undecoded candidate with at least ev Costas tones
                            sits inside a signal decoded in the pass before (0 = whenever something was decoded) */
    int nbr;             /* attempts (bit mask as passmask) allowed for a candidate next to (+- skip) a stronger one that
                            failed; 0 = all */
    int erounds;         /* > 0: a decoded signal is erased from the waterfall in work (its tone cells are lowered to the
                            noise baseline), and in up to erounds extra rounds the candidates that failed with >= ev
                            Costas tones inside a signal decoded meanwhile are tried again on that waterfall */
    int emask;           /* attempts (bit mask as passmask) made in those rounds */
    int budget;          /* > 0 and js8_clock set: a further pass over the capture is not started when the time since
                            the start of the call plus the cost of the last pass made in this call would exceed
                            budget (js8_clock units; the app: ms).  The hard cut stays with should_abort. */
} js8_tune_t;
extern js8_tune_t js8_tune;

#define JS8_SNR_FLOOR  (-30.0f)
#define JS8_WORK_MIN   (96u * 1024u)    /* below this the call fails */
#define JS8_ERR_ARGS   (-1)
#define JS8_ERR_ABORT  (-2)             /* aborted before anything was decoded */
#define JS8_ERR_IO     (-3)

/* Decode one 15 s JS8 Normal period.
 *   src            : audio (fs 6000..12000 Hz with fs*0.16 an integer, 8000 expected), read only via
 *                    src->read, front to back once per pass (1 + up to 2 subtraction passes; times the
 *                    band passes when the range does not fit the work area)
 *   f_lo, f_hi     : search range for the tone-0 frequency (Hz); clipped so tone 7 stays below fs/2
 *   nominal_start_s: position in the buffer where a DT = 0 signal starts (period start + 0.5 s);
 *                    DT is searched about -2.2 .. +2.4 s around it
 *   out/maxout     : results (see js8_tune.dup_out for duplicates)
 *   work/work_len  : scratch, 8-byte aligned (>= JS8_WORK_MIN)
 *   should_abort   : polled every few ms of CPU (may be 0)
 * returns number of decodes (>=0; if aborted, what was decoded so far) or JS8_ERR_*.
 */
int js8_decode(const dsrc_t *src, float f_lo, float f_hi, float nominal_start_s,
               js8_result_t *out, int maxout, void *work, uint32_t work_len, int (*should_abort)(void));

/* Streaming front end (optional): build the waterfall of the first pass WHILE the period is captured, so
 * that after the period only the search and the decoding remain (on the radio one pass over the capture
 * costs about 2 s: SD read + 184 FFTs).
 *   js8_stream_begin  work/work_len/fs/f_lo/f_hi/nominal_start_s exactly as the later js8_decode() call will
 *                     get them; max_len = the longest capture (samples); pos = index of the first sample that
 *                     will be fed (0 = from the start of the capture; > 0: the rows before it are built by
 *                     js8_decode() from src).  Returns 1 if the stream runs, 0 if not (bad arguments, or the
 *                     band needs more than one band pass in this arena, or nominal_start_s > 0.5) - then everything works as without it.
 *   js8_stream_put    the next n samples of the capture, in order, the same values src->read will return
 *                     (x = 0: n zero samples).  Work: one waterfall row (one FFT) per 80 ms of audio.
 *   js8_stream_stop   forget the stream.
 * `work` belongs to the stream from begin until the js8_decode() call, which takes the rows over if work,
 * work_len, band, fs, nominal start match and the number of samples fed == src->len; otherwise it ignores
 * them.  Any js8_decode() call ends the stream.  The decode results are identical with and without it. */
int js8_stream_begin(void *work, uint32_t work_len, uint32_t fs, uint32_t max_len, float f_lo, float f_hi,
                     float nominal_start_s, uint32_t pos);
void js8_stream_put(const int16_t *x, uint32_t n);
void js8_stream_stop(void);

/* bytes of `work` used by the last js8_decode call (high-water mark) */
uint32_t js8_work_peak(void);

/* statistics of the last call (test bench "E" line, the app's panel and JS8PERF.TXT).
 * Stage clock: when js8_clock is set (the app: icap_ticks, ms; the host profiler: microseconds) the decoder
 * reads it at every change of stage and adds the difference to t[stage].  The stages partition the whole call:
 * the sum of t[] is the time spent in js8_decode().  Without a clock t[] stays 0; the counters always count. */
enum {
    JS8_T_READ,          /* src->read (the app: SD read of the capture + the capture pump it runs in between) */
    JS8_T_SUB,           /* correlate + subtract the decoded signals while streaming (subtraction passes) */
    JS8_T_FFT,           /* window + FFT per waterfall row */
    JS8_T_MAG,           /* bin powers -> 0.5 dB uint8 (log10), noise baseline */
    JS8_T_SEARCH,        /* Costas sync search over the waterfall (find_candidates) */
    JS8_T_LLR,           /* per candidate: hard sync check, bit metrics, bookkeeping of the candidate loop */
    JS8_T_BP1, JS8_T_BP2, JS8_T_BP3, JS8_T_BP4, JS8_T_BP5,   /* BP + CRC, by decoding attempt 1..5 */
    JS8_T_FIN,           /* a decoded signal: re-encode, time / frequency refinement, SNR */
    JS8_T_POLL,          /* inside should_abort (the app: capture pump of the next period, yield) */
    JS8_T_N
};
typedef struct {                                 /* JS8_NSTAT uint32_t words (the app sums them as an array) */
    uint32_t ncand, ntried, nbp, bp_iters;       /* candidates found / tried (hard sync ok), BP runs, BP iterations */
    uint32_t t[JS8_T_N];                         /* clock units per stage */
    uint32_t tk[3];                              /* clock units per kind of pass: 0 = first pass of a band, 1 = later
                                                    normal passes (after a subtraction), 2 = shifted-grid passes */
    uint32_t nrdch;                              /* capture chunks requested from src->read (half a symbol each:
                                                    640 samples = 1280 bytes of s16 at 8 kHz) */
    uint32_t nscore;                             /* sync scores computed (grid points searched) */
    uint32_t nsubch;                             /* signal x row chunks correlated and subtracted */
    uint32_t bpit[5];                            /* BP iterations by attempt 1..5 */
    uint32_t bpn[5], bpok[5];                    /* BP runs / accepted frames by attempt 1..5 */
    uint32_t nfft;                               /* FFTs = waterfall rows built */
    uint32_t nllr;                               /* bit-metric vectors computed */
    uint32_t npoll;                              /* should_abort calls */
    uint32_t nsig;                               /* signals decoded (incl. repeats of a frame at another frequency) */
    uint32_t nwf[3];                             /* waterfalls built per kind of pass (see tk) */
    uint32_t nband;                              /* band passes */
} js8_stats_t;
#define JS8_NSTAT (4 + JS8_T_N + 3 + 3 + 15 + 4 + 4)
extern js8_stats_t js8_stats;
extern uint32_t (*js8_clock)(void);              /* 0 = no stage timing */

#endif
