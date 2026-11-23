/* SPDX-License-Identifier: GPL-3.0-or-later
 * digi_util.h - freestanding helpers shared by the on-radio digital-mode decoders
 * (no libc, no malloc). Part of the IC-705 ICAP "digi" apps.
 */
#ifndef DIGI_UTIL_H
#define DIGI_UTIL_H
#include <stdint.h>
#include <stddef.h>

#ifndef DSRC_T_DEFINED
#define DSRC_T_DEFINED
/* sample source: PCM lives on SD card and is read back in bounded chunks.
 * read() returns the number of samples written to out (>=0, short counts are
 * zero-filled by the caller) or <0 on I/O error. */
typedef struct {
    void *ctx;
    int (*read)(void *ctx, uint32_t start, float *out, uint32_t n);
    uint32_t len;   /* total samples */
    uint32_t fs;    /* sample rate (Hz) */
} dsrc_t;
#endif

void dg_zero(void *p, uint32_t nbytes);
void dg_copy(void *d, const void *s, uint32_t nbytes);

/* math (single precision unless noted) */
static inline float dg_sqrtf(float x) { return __builtin_sqrtf(x); } /* vsqrt.f32 with -fno-math-errno */
static inline float dg_fabsf(float x) { return __builtin_fabsf(x); }
void  dg_sincos(double x, double *s, double *c);   /* radians, ~1e-15 abs error */
float dg_log10f(float x);                          /* x>0, ~1e-6 rel */
static inline int dg_iround(float x) { return (int)(x >= 0.f ? x + 0.5f : x - 0.5f); }

/* quarter-wave sine table for a size-n real FFT: q[k]=sin(2*pi*k/n), k=0..n/4 */
void dg_fft_table(float *q, uint32_t n);
/* power spectrum of n real samples (n power of two, 8..65536) computed in place:
 * on return x[k] = |X[k]|^2 for k=0..n/2 (x must hold n+2 floats). */
void dg_rfft_power(float *x, uint32_t n, const float *q);

/* buffered sequential reader over a dsrc_t (out-of-range samples read as 0) */
typedef struct {
    const dsrc_t *src;
    float *buf;
    uint32_t cap;
    int32_t start;  /* first sample held in buf */
    uint32_t n;     /* samples held */
    int err;
} dg_rdr_t;
void dg_rdr_init(dg_rdr_t *r, const dsrc_t *src, float *buf, uint32_t cap);
/* returns pointer to samples [a, a+m) (m<=cap) or 0 on I/O error */
const float *dg_rdr_span(dg_rdr_t *r, int32_t a, uint32_t m);

#endif
