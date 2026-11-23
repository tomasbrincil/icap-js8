/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8synth.h - PC-only helpers shared by the decoder test drivers (js8wav, js8sweep, ft8ref):
 * PRNG, AWGN, the JS8 modulator (plain CPFSK, analytic phase, as tests/js8gen.c), WAV/PCM I/O,
 * decoder knob parsing. Nothing here is built for the radio.
 */
#ifndef JS8SYNTH_H
#define JS8SYNTH_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#define SY_TWO_PI 6.283185307179586476925286766559
#define SY_FS 8000
#define SY_SPS 1280
#define SY_U __attribute__((unused))

static uint64_t sy_rng_s = 1;
SY_U static uint64_t sy_rng64(void)
{
    uint64_t z = (sy_rng_s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
SY_U static double sy_u(void) { return (double)((sy_rng64() >> 11) + 1) / 9007199254740993.0; } /* (0,1) */
SY_U static double sy_gauss(void)
{
    static int have; static double spare;
    double u, v, r;
    if (have) { have = 0; return spare; }
    u = sy_u(); v = sy_u();
    r = sqrt(-2.0 * log(u));
    spare = r * sin(SY_TWO_PI * v); have = 1;
    return r * cos(SY_TWO_PI * v);
}

/* peak amplitude for an SNR (dB, 2500 Hz reference) over white noise of RMS sigma in the 4 kHz band */
SY_U static double sy_amp_for_snr(double snr_db, double sigma)
{
    return sigma * sqrt(2.0 * pow(10.0, snr_db / 10.0) * 2500.0 / 4000.0);
}

/* add a 79-symbol CPFSK frame: symbol 0 starts at sample position t0 (may be fractional / negative);
 * the first sy_cut_syms symbols are left out (a transmitter that keyed late) */
static double sy_cut_syms = 0;
SY_U static void sy_add_cpfsk(double *buf, long n, const uint8_t tones[79], double f0, double t0, double amp)
{
    long a = (long)ceil(t0), b = (long)ceil(t0 + 79.0 * SY_SPS), i;
    double ph_sym[80];
    int k;
    ph_sym[0] = 0;
    for (k = 0; k < 79; k++) ph_sym[k + 1] = ph_sym[k] + SY_TWO_PI * (f0 + 6.25 * tones[k]) * SY_SPS / SY_FS;
    if (a < 0) a = 0;
    if (b > n) b = n;
    for (i = a; i < b; i++) {
        double u = (double)i - t0;
        if (u < sy_cut_syms * SY_SPS) continue;
        k = (int)(u / SY_SPS); if (k > 78) k = 78;
        buf[i] += amp * sin(ph_sym[k] + SY_TWO_PI * (f0 + 6.25 * tones[k]) * (u - (double)k * SY_SPS) / SY_FS);
    }
}
SY_U static void sy_add_noise(double *buf, long n, double sigma)
{
    long i;
    if (sigma > 0) for (i = 0; i < n; i++) buf[i] += sigma * sy_gauss();
}
SY_U static long sy_quantize(const double *buf, int16_t *out, long n)
{
    long i, clips = 0;
    for (i = 0; i < n; i++) {
        double v = floor(buf[i] + 0.5);
        if (v > 32767) { v = 32767; clips++; } else if (v < -32768) { v = -32768; clips++; }
        out[i] = (int16_t)v;
    }
    return clips;
}

/* ---- WAV / PCM: 8000 Hz mono s16 only */
SY_U static int16_t *sy_load(const char *name, long *n_out)
{
    FILE *f = fopen(name, "rb");
    long len, pos = 0, dlen;
    uint8_t *d;
    int16_t *s;
    size_t k = strlen(name);
    int raw = k > 4 && (!strcmp(name + k - 4, ".pcm") || !strcmp(name + k - 4, ".PCM"));
    if (!f) { perror(name); return NULL; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    d = (uint8_t *)malloc((size_t)len + 16);
    if (!d || fread(d, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "%s: read error\n", name); fclose(f); return NULL; }
    fclose(f);
    dlen = len;
    if (!raw) {
        int fmt_ok = 0;
        if (len < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) { fprintf(stderr, "%s: not a WAV file\n", name); return NULL; }
        pos = 12; dlen = -1;
        while (pos + 8 <= len) {
            unsigned long cl = (unsigned long)d[pos + 4] | ((unsigned long)d[pos + 5] << 8) | ((unsigned long)d[pos + 6] << 16) | ((unsigned long)d[pos + 7] << 24);
            if (!memcmp(d + pos, "fmt ", 4)) {
                unsigned fmt = d[pos + 8] | (d[pos + 9] << 8), ch = d[pos + 10] | (d[pos + 11] << 8);
                unsigned long rate = (unsigned long)d[pos + 12] | ((unsigned long)d[pos + 13] << 8) | ((unsigned long)d[pos + 14] << 16);
                unsigned bits = d[pos + 22] | (d[pos + 23] << 8);
                if (fmt != 1 || ch != 1 || rate != SY_FS || bits != 16) {
                    fprintf(stderr, "%s: need PCM 8000 Hz mono s16 (got fmt %u, %u ch, %lu Hz, %u bit)\n", name, fmt, ch, rate, bits);
                    return NULL;
                }
                fmt_ok = 1;
            } else if (!memcmp(d + pos, "data", 4)) {
                pos += 8; dlen = (long)cl; if (pos + dlen > len) dlen = len - pos;
                break;
            }
            pos += 8 + (long)cl + (long)(cl & 1);
        }
        if (!fmt_ok || dlen < 0) { fprintf(stderr, "%s: no fmt/data chunk\n", name); return NULL; }
    }
    *n_out = dlen / 2;
    s = (int16_t *)malloc((size_t)(*n_out + 1) * 2);
    for (long i = 0; i < *n_out; i++) s[i] = (int16_t)(d[pos + 2 * i] | (d[pos + 2 * i + 1] << 8));
    free(d);
    return s;
}
SY_U static void sy_put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
SY_U static void sy_put32(FILE *f, unsigned long v) { sy_put16(f, (unsigned)(v & 0xFFFF)); sy_put16(f, (unsigned)((v >> 16) & 0xFFFF)); }
SY_U static int sy_save_wav(const char *name, const int16_t *s, long n)
{
    FILE *f = fopen(name, "wb");
    if (!f) { perror(name); return -1; }
    fwrite("RIFF", 1, 4, f); sy_put32(f, (unsigned long)(36 + 2 * n)); fwrite("WAVEfmt ", 1, 8, f);
    sy_put32(f, 16); sy_put16(f, 1); sy_put16(f, 1); sy_put32(f, SY_FS); sy_put32(f, SY_FS * 2); sy_put16(f, 2); sy_put16(f, 16);
    fwrite("data", 1, 4, f); sy_put32(f, (unsigned long)(2 * n));
    for (long i = 0; i < n; i++) sy_put16(f, (unsigned)(uint16_t)s[i]);
    return fclose(f);
}

SY_U static double sy_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

#ifdef JS8DEC_H
/* "k=v,k=v": decoder knobs by name; returns 0 or -1 (unknown key, printed) */
SY_U static int sy_tune(const char *spec)
{
    char tmp[512], *p, *e;
    if (strlen(spec) >= sizeof tmp) return -1;
    strcpy(tmp, spec);
    for (p = strtok(tmp, ","); p; p = strtok(NULL, ",")) {
        double v;
        e = strchr(p, '=');
        if (!e) { fprintf(stderr, "tune: need key=value: %s\n", p); return -1; }
        *e++ = 0; v = atof(e);
#define K(name, field) if (!strcmp(p, name)) { js8_tune.field = (__typeof__(js8_tune.field))v; continue; }
        K("llr", llr) K("llr2", llr2) K("llr5", llr5) K("max_cand", max_cand) K("min_score", min_score) K("iters", iters)
        K("osd", osd) K("osd_nt", osd_nt) K("nhmax", nhmax) K("nh34", nh34) K("nh4", nh4)
        K("subpasses", subpasses) K("nsync_min", nsync_min) K("nsync34", nsync34) K("passmask", passmask)
        K("scale", scale) K("snr_off", snr_off) K("pct", pct) K("skip", skip) K("near", near)
        K("dup_out", dup_out) K("dup_hz", dup_hz) K("osd_min", osd_min) K("osd_dmax", osd_dmax) K("shift", shift) K("nstrong", nstrong) K("sstrong", sstrong) K("swide", swide) K("nsync2", nsync2) K("foot", foot) K("ev", ev) K("nbr", nbr) K("erounds", erounds) K("emask", emask) K("budget", budget)
#undef K
        fprintf(stderr, "tune: unknown key %s\n", p);
        return -1;
    }
    return 0;
}

/* ---- model of the radio's decode time from the decoder's deterministic counters (js8_stats).
 * ms per: FFT row (window + FFT + power -> dB of the default 344-bin band), 1000 sync scores, BP iteration,
 * signal x row subtracted, 1000 samples read from the capture (640 per chunk), bit-metric vector, abort poll, and a fixed part
 * per period.  The default (SY_MODEL) is calibrated on the RADIO's own stage times (instrumented app, 23 Nov 2026,
 * live 40 m band):
 *   empty period (FAST preset): SD read 667 ms for one pass over the capture (116800 samples: 5.71 ms / 1000),
 *     STFT 1313 ms for 184 rows (7.14 ms / row), search 316 ms for 40440 scores (7.81 ms / 1000), total 2642 ms
 *     - the 345 ms on top of the stages are the abort polls = the capture pump of the next period, 13 % of the
 *     decode time; every constant below carries that factor 1.13;
 *   busy period (FULL preset): 4538 ms for 164 BP runs of 35 candidates = 27.7 ms per run, at about 24 iterations
 *     per run (PC counters, same kind of load) 1.15 ms per iteration incl. the bit metrics;
 *   subtraction: 442 ms in that period; per signal x row chunk it is the PC cost scaled like the FFT, 0.36 ms.
 * That is SY_MODEL_OLD: the cost per operation of the first build's kernels (float flooding BP, radix-2 FFT, log10).
 * SY_MODEL (default) is for the kernels of this build, which have NOT run on the radio yet: SY_MODEL_OLD times the
 * ratio new / old measured per stage on the PC on the same inputs (merge of 23 Nov 2026: STFT row 0.37, sync search
 * 0.36, one layered BP sweep against one flooding iteration 0.76, subtraction 0.27; the SD read is left at 1.0
 * although it is now 58 block reads per pass instead of about 370 calls).  x86 and the Cortex-A9 differ, so +-40 %; the
 * first JS8PERF.TXT of this build replaces these numbers (-K).
 * Rows made by the streaming front end during the capture are not in js8_stats and so cost nothing here. */
#define SY_MODEL_OLD "8.07,8.83,1.30,0.41,6.45,0,0,0"   /* radio stage times of 23 Nov 2026, x 1.13 for the capture pump */
#ifndef SY_MODEL
#define SY_MODEL "2.99,3.18,0.99,0.11,6.45,0,0,0"
#endif
static double sy_model_k[8]; static int sy_model_have;
SY_U static int sy_model_set(const char *spec)
{
    if (sscanf(spec, "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf", &sy_model_k[0], &sy_model_k[1], &sy_model_k[2], &sy_model_k[3],
               &sy_model_k[4], &sy_model_k[5], &sy_model_k[6], &sy_model_k[7]) != 8) { fprintf(stderr, "model: need 8 numbers\n"); return -1; }
    sy_model_have = 1;
    return 0;
}
SY_U static double sy_model_ms(const js8_stats_t *st)
{
    if (!sy_model_have) sy_model_set(SY_MODEL);
    return sy_model_k[0] * st->nfft + sy_model_k[1] * 1e-3 * st->nscore + sy_model_k[2] * st->bp_iters + sy_model_k[3] * st->nsubch
         + sy_model_k[4] * 0.64 * st->nrdch + sy_model_k[5] * st->nllr + sy_model_k[6] * st->npoll + sy_model_k[7];
}
/* js8_tune.budget (a further pass is not started when it cannot end in time) on the host: the decoder's clock is
 * then the MODELLED radio time in ms */
SY_U static uint32_t sy_model_clock(void) { return (uint32_t)sy_model_ms(&js8_stats); }
SY_U static void sy_budget(double ms) { js8_tune.budget = (int)ms; js8_clock = sy_model_clock; }
SY_U static const char *sy_model_str(void)
{
    static char b[256];
    if (!sy_model_have) sy_model_set(SY_MODEL);
    snprintf(b, sizeof b, "fft row %.4g, 1000 scores %.4g, BP iteration %.4g, subtraction chunk %.4g, 1000 samples %.4g, LLR %.4g, poll %.4g, fixed %.4g",
             sy_model_k[0], sy_model_k[1], sy_model_k[2], sy_model_k[3], sy_model_k[4], sy_model_k[5], sy_model_k[6], sy_model_k[7]);
    return b;
}
#endif

/* the streaming front end as the radio shell drives it: begin at sample `pos` of the capture, feed the rest in
 * 800-sample pieces (get(i) = capture sample i).  pos < 0: no streaming. */
SY_U static void sy_stream(void *arena, uint32_t arena_len, uint32_t caplen, float flo, float fhi, float nom, long pos, int16_t (*get)(uint32_t))
{
    int16_t b[800];
    if (pos < 0) return;
    if ((uint32_t)pos > caplen) pos = (long)caplen;
    if (!js8_stream_begin(arena, arena_len, SY_FS, caplen, flo, fhi, nom, (uint32_t)pos)) return;
    for (uint32_t i = (uint32_t)pos; i < caplen; ) {
        uint32_t k = caplen - i; if (k > 800) k = 800;
        for (uint32_t j = 0; j < k; j++) b[j] = get(i + j);
        js8_stream_put(b, k); i += k;
    }
}

#endif /* JS8SYNTH_H */
