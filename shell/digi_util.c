/* SPDX-License-Identifier: GPL-3.0-or-later
 * digi_util.c - freestanding helpers (math, FFT, buffered SD reader).
 */
#include "digi_util.h"

void dg_zero(void *p, uint32_t nbytes)
{
    volatile uint8_t *d = (volatile uint8_t *)p;  /* volatile: stop gcc turning this into memset() */
    while (nbytes--) *d++ = 0;
}

void dg_copy(void *dst, const void *src, uint32_t nbytes)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) { while (nbytes--) *d++ = *s++; }
    else { d += nbytes; s += nbytes; while (nbytes--) *--d = *--s; }
}

/* gcc may still emit calls to these for struct copies; provide weak versions */
__attribute__((weak)) void *memset(void *p, int c, size_t n)
{
    volatile uint8_t *d = (volatile uint8_t *)p;
    while (n--) *d++ = (uint8_t)c;
    return p;
}
__attribute__((weak)) void *memcpy(void *dst, const void *src, size_t n)
{
    dg_copy(dst, src, (uint32_t)n);
    return dst;
}
__attribute__((weak)) void *memmove(void *dst, const void *src, size_t n)
{
    dg_copy(dst, src, (uint32_t)n);
    return dst;
}

void dg_sincos(double x, double *s, double *c)
{
    const double inv_hpi = 0.63661977236758134308;
    const double hpi_hi = 1.57079632673412561417;   /* pi/2 split for reduction */
    const double hpi_lo = 6.07710050650619224932e-11;
    double kf = x * inv_hpi;
    int k = (int)(kf >= 0 ? kf + 0.5 : kf - 0.5);   /* |x| < 3e9 rad */
    double r = (x - (double)k * hpi_hi) - (double)k * hpi_lo;
    double r2 = r * r;
    double sn = r * (1.0 + r2 * (-1.0 / 6 + r2 * (1.0 / 120 + r2 * (-1.0 / 5040 + r2 * (1.0 / 362880 +
                r2 * (-1.0 / 39916800 + r2 * (1.0 / 6227020800.0)))))));
    double cs = 1.0 + r2 * (-0.5 + r2 * (1.0 / 24 + r2 * (-1.0 / 720 + r2 * (1.0 / 40320 +
                r2 * (-1.0 / 3628800 + r2 * (1.0 / 479001600.0 + r2 * (-1.0 / 87178291200.0)))))));
    switch (k & 3) {
    case 0: *s = sn;  *c = cs;  break;
    case 1: *s = cs;  *c = -sn; break;
    case 2: *s = -sn; *c = -cs; break;
    default: *s = -cs; *c = sn; break;
    }
}

float dg_log10f(float x)
{
    union { float f; uint32_t u; } v;
    if (!(x > 0.f)) return -38.f;
    v.f = x;
    int e = (int)((v.u >> 23) & 0xff) - 127;
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;     /* m in [1,2) */
    float m = v.f;
    if (m > 1.41421356f) { m *= 0.5f; e++; }
    float t = (m - 1.f) / (m + 1.f), t2 = t * t;
    float ln = 2.f * t * (1.f + t2 * (1.f / 3 + t2 * (1.f / 5 + t2 * (1.f / 7 + t2 * (1.f / 9)))));
    return (ln + (float)e * 0.69314718056f) * 0.43429448190f;
}

void dg_fft_table(float *q, uint32_t n)
{
    uint32_t k;
    for (k = 0; k <= n / 4; k++) {
        double s, c;
        dg_sincos(6.283185307179586477 * (double)k / (double)n, &s, &c);
        q[k] = (float)s;
    }
}

/* exp(-i*2*pi*k/n) for 0<=k<n/2 from the quarter table */
static inline void tw(const float *q, uint32_t n, uint32_t k, float *c, float *s)
{
    uint32_t n4 = n >> 2;
    if (k <= n4) { *c = q[n4 - k]; *s = -q[k]; }
    else { *c = -q[k - n4]; *s = -q[(n >> 1) - k]; }
}

void dg_rfft_power(float *x, uint32_t n, const float *q)
{
    uint32_t m = n >> 1, i, j, len;
    /* complex FFT of z[i] = x[2i] + j x[2i+1], length m (interleaved re,im) */
    for (i = 1, j = 0; i < m; i++) {               /* bit reversal */
        uint32_t bit = m >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = x[2 * i], ti = x[2 * i + 1];
            x[2 * i] = x[2 * j]; x[2 * i + 1] = x[2 * j + 1];
            x[2 * j] = tr; x[2 * j + 1] = ti;
        }
    }
    for (len = 2; len <= m; len <<= 1) {
        uint32_t half = len >> 1, step = n / len;  /* twiddle index step in units of 1/n */
        uint32_t s0;
        for (s0 = 0; s0 < half; s0++) {
            float wr, wi;
            tw(q, n, s0 * step, &wr, &wi);
            for (i = s0; i < m; i += len) {
                uint32_t a = 2 * i, b = 2 * (i + half);
                float br = x[b] * wr - x[b + 1] * wi;
                float bi = x[b] * wi + x[b + 1] * wr;
                x[b] = x[a] - br; x[b + 1] = x[a + 1] - bi;
                x[a] += br; x[a + 1] += bi;
            }
        }
    }
    /* untangle: X[k] = (Z[k]+conj(Z[m-k]))/2 - j/2 * W^k (Z[k]-conj(Z[m-k])) */
    {
        float z0r = x[0], z0i = x[1];
        float pm = (z0r - z0i) * (z0r - z0i);
        float p0 = (z0r + z0i) * (z0r + z0i);
        uint32_t k;
        for (k = 1; k <= m / 2; k++) {
            uint32_t k2 = m - k;
            float ar = x[2 * k], ai = x[2 * k + 1], br = x[2 * k2], bi = x[2 * k2 + 1];
            float er = 0.5f * (ar + br), ei = 0.5f * (ai - bi);   /* (Z[k]+conj Z[m-k])/2 */
            float dr = 0.5f * (ar - br), di = 0.5f * (ai + bi);   /* (Z[k]-conj Z[m-k])/2 */
            float wr, wi, xr, xi, yr, yi;
            tw(q, n, k, &wr, &wi);
            /* -j*W*d */
            float tr = wr * dr - wi * di, ti = wr * di + wi * dr;
            xr = er + ti; xi = ei - tr;                           /* X[k]   */
            /* X[m-k] = conj(E) - j conj(W^k) conj(D) */
            {
                float er2 = er, ei2 = -ei, dr2 = dr, di2 = -di;  /* conj E, conj D */
                float w2r = wr, w2i = -wi;
                float t2r = w2r * dr2 - w2i * di2, t2i = w2r * di2 + w2i * dr2;
                yr = er2 + t2i; yi = ei2 - t2r;
            }
            x[2 * k] = xr * xr + xi * xi;       /* temporarily store powers in re slots */
            x[2 * k2] = yr * yr + yi * yi;
        }
        /* compact: power[k] currently at x[2k] for 1..m-1; spread to x[k] */
        for (k = 1; k < m; k++) x[k] = x[2 * k];
        x[0] = p0;
        x[m] = pm;
    }
}

void dg_rdr_init(dg_rdr_t *r, const dsrc_t *src, float *buf, uint32_t cap)
{
    r->src = src; r->buf = buf; r->cap = cap; r->start = 0; r->n = 0; r->err = 0;
}

static int rdr_fill(dg_rdr_t *r, int32_t a, float *out, uint32_t cnt)
{
    uint32_t i = 0;
    while (i < cnt) {
        int64_t pos = (int64_t)a + i;
        if (pos < 0 || pos >= (int64_t)r->src->len) { out[i++] = 0.f; continue; }
        {
            uint32_t want = cnt - i;
            if ((int64_t)want > (int64_t)r->src->len - pos) want = (uint32_t)((int64_t)r->src->len - pos);
            int got = r->src->read(r->src->ctx, (uint32_t)pos, out + i, want);
            if (got < 0) { r->err = 1; return -1; }
            if (got == 0) { /* short source: zero rest */
                while (i < cnt) out[i++] = 0.f;
                return 0;
            }
            i += (uint32_t)got;
        }
    }
    return 0;
}

const float *dg_rdr_span(dg_rdr_t *r, int32_t a, uint32_t m)
{
    int32_t end = r->start + (int32_t)r->n;
    if (r->err || m > r->cap) return 0;
    if (r->n && a >= r->start && a + (int32_t)m <= end) return r->buf + (a - r->start);
    if (r->n && a >= r->start && a < end) {
        uint32_t keep = (uint32_t)(end - a), i;
        for (i = 0; i < keep; i++) r->buf[i] = r->buf[(a - r->start) + i];
        if (rdr_fill(r, end, r->buf + keep, r->cap - keep)) return 0;
    } else {
        if (rdr_fill(r, a, r->buf, r->cap)) return 0;
    }
    r->start = a; r->n = r->cap;
    return r->buf;
}
