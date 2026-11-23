/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8jsc.c - JSC dictionary on SD card without seek; see js8jsc.h and
 * tools/mkjsc.py for the file formats.
 */
#include "js8jsc.h"

#define RD_BUF 256

typedef struct {
    js8jsc_t *c;
    int fd;
    int pos, n;
    uint8_t buf[RD_BUF];
} rd_t;

static void mkname(char *name, char kind, unsigned no)
{
    static const char hx[] = "0123456789ABCDEF";
    name[0] = 'J'; name[1] = 'S'; name[2] = 'C'; name[3] = kind;
    name[4] = hx[(no >> 8) & 15]; name[5] = hx[(no >> 4) & 15]; name[6] = hx[no & 15];
    name[7] = '.'; name[8] = 'B'; name[9] = 'I'; name[10] = 'N'; name[11] = 0;
}

static int rd_open(rd_t *r, js8jsc_t *c, const char *name)
{
    r->c = c; r->pos = r->n = 0;
    r->fd = c->io->open(c->io->user, name);
    c->st_last_bytes = 0;
    if (r->fd < 0) return -1;
    c->st_opens++;
    return 0;
}

static void rd_close(rd_t *r)
{
    js8jsc_t *c = r->c;
    c->io->close(c->io->user, r->fd);
    if (c->st_last_bytes > c->st_max_bytes) c->st_max_bytes = c->st_last_bytes;
}

static int rd_byte(rd_t *r)
{
    if (r->pos >= r->n) {
        int n = r->c->io->read(r->c->io->user, r->fd, r->buf, RD_BUF);
        if (n <= 0) return -1;
        r->n = n; r->pos = 0;
        r->c->st_bytes += (uint32_t)n;
        r->c->st_last_bytes += (uint32_t)n;
    }
    return r->buf[r->pos++];
}

static int rd_skip(rd_t *r, int n)
{
    while (n > 0) {
        int have = r->n - r->pos;
        if (have <= 0) { if (rd_byte(r) < 0) return -1; n--; continue; }
        if (have > n) have = n;
        r->pos += have; n -= have;
    }
    return 0;
}

int js8jsc_init(js8jsc_t *c, const js8_io_t *io, uint8_t *mem, uint32_t mem_size)
{
    rd_t r;
    uint8_t h[16];
    uint32_t i, used = 0, hn;
    int k;

    c->io = io; c->head = mem; c->head_n = 0; c->head_bytes = 0; c->ok = 0; c->clock = 0;
    c->st_bytes = c->st_opens = c->st_last_bytes = c->st_max_bytes = 0;
    c->st_cache_hits = c->st_head_hits = 0;
    for (k = 0; k < JS8JSC_CACHE_N; k++) { c->cache[k].idx = 0xFFFFFFFFu; c->cache[k].age = 0; }
    if (!io || rd_open(&r, c, "JSCHEAD.BIN") < 0) return -1;
    for (k = 0; k < 16; k++) {
        int b = rd_byte(&r);
        if (b < 0) { rd_close(&r); return -2; }
        h[k] = (uint8_t)b;
    }
    if (h[0] != 'J' || h[1] != 'S' || h[2] != 'C' || h[3] != '1') { rd_close(&r); return -3; }
    c->total = (uint32_t)h[4] | (uint32_t)h[5] << 8 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 24;
    c->per_file = (uint16_t)(h[8] | h[9] << 8);
    c->enc_files = (uint16_t)(h[10] | h[11] << 8);
    hn = (uint32_t)(h[12] | h[13] << 8);
    if (c->total == 0 || c->total > JS8JSC_SIZE || c->per_file == 0 || h[14] > JS8JSC_WORD_MAX) {
        rd_close(&r); return -3;
    }
    for (i = 0; mem && i < hn; i++) {
        int len = rd_byte(&r), j;
        if (len < 0 || len > JS8JSC_WORD_MAX) break;
        if (used + 1u + (uint32_t)len > mem_size) break;
        mem[used] = (uint8_t)len;
        for (j = 0; j < len; j++) {
            int b = rd_byte(&r);
            if (b < 0) break;
            mem[used + 1 + j] = (uint8_t)b;
        }
        if (j < len) break;
        used += 1u + (uint32_t)len;
        c->head_n = i + 1;
    }
    c->head_bytes = used;
    rd_close(&r);
    c->ok = 1;
    return 0;
}

int js8jsc_word(js8jsc_t *c, uint32_t index, char *out, int max)
{
    rd_t r;
    char name[12];
    int k, len, j, victim = 0, b;
    unsigned skip, cnt;
    char w[JS8JSC_WORD_MAX];

    if (!c || !c->ok || index >= c->total) return -1;
    if (index < c->head_n) {
        const uint8_t *p = c->head;
        uint32_t i;
        for (i = 0; i < index; i++) p += 1 + p[0];
        len = p[0];
        for (j = 0; j < len && j < max; j++) out[j] = (char)p[1 + j];
        c->st_head_hits++;
        return j;
    }
    c->clock++;
    if (c->clock == 0) {                       /* age counter wrapped: restart LRU order */
        for (k = 0; k < JS8JSC_CACHE_N; k++) c->cache[k].age = 0;
        c->clock = 1;
    }
    for (k = 0; k < JS8JSC_CACHE_N; k++) {
        if (c->cache[k].idx == index) {
            len = c->cache[k].len;
            for (j = 0; j < len && j < max; j++) out[j] = c->cache[k].w[j];
            c->cache[k].age = c->clock;
            c->st_cache_hits++;
            return j;
        }
        if (c->cache[k].age < c->cache[victim].age) victim = k;
    }
    mkname(name, 'D', index / c->per_file);
    if (rd_open(&r, c, name) < 0) return -2;
    {
        uint8_t h[8];
        for (k = 0; k < 8; k++) {
            b = rd_byte(&r);
            if (b < 0) { rd_close(&r); return -3; }
            h[k] = (uint8_t)b;
        }
        cnt = (unsigned)(h[4] | h[5] << 8);
        if (h[0] != 'J' || h[1] != 'D') { rd_close(&r); return -3; }
    }
    skip = index % c->per_file;
    if (skip >= cnt) { rd_close(&r); return -3; }
    while (skip--) {
        len = rd_byte(&r);
        if (len < 0 || rd_skip(&r, len) < 0) { rd_close(&r); return -3; }
    }
    len = rd_byte(&r);
    if (len < 0 || len > JS8JSC_WORD_MAX) { rd_close(&r); return -3; }
    for (j = 0; j < len; j++) {
        b = rd_byte(&r);
        if (b < 0) { rd_close(&r); return -3; }
        w[j] = (char)b;
    }
    rd_close(&r);
    if (len <= JS8JSC_CACHE_W) {
        c->cache[victim].idx = index;
        c->cache[victim].len = (uint8_t)len;
        c->cache[victim].age = c->clock;
        for (j = 0; j < len; j++) c->cache[victim].w[j] = w[j];
    }
    for (j = 0; j < len && j < max; j++) out[j] = w[j];
    return j;
}

/* Scan one encode file for the first entry that is a prefix of w.
 * Returns 1 found, 0 not found, < 0 error. */
static int scan(js8jsc_t *c, const char *name, const uint8_t *w, int wlen, int grp,
                uint32_t *index, int *advance)
{
    rd_t r;
    int k, b, len, j, res = 0;

    if (rd_open(&r, c, name) < 0) return -2;
    for (k = 0; k < 8; k++) {
        b = rd_byte(&r);
        if (b < 0 || (k == 0 && b != 'J') || (k == 1 && b != 'E')) { rd_close(&r); return -3; }
    }
    for (;;) {
        int match = 1, below = 0, decided = 0;
        uint32_t v;
        len = rd_byte(&r);
        if (len < 0) break;                      /* end of file */
        if (len > wlen) match = 0;
        for (j = 0; j < len; j++) {
            b = rd_byte(&r);
            if (b < 0) { rd_close(&r); return -3; }
            if (j < wlen && b != w[j]) {
                match = 0;
                /* entries of one first character are in descending order: once the
                 * 3-character group of w is behind us nothing can match any more */
                if (!decided && j > 0 && j < grp && b < w[j]) below = 1;
                decided = 1;
            }
        }
        v = 0;
        for (j = 0; j < 3; j++) {
            b = rd_byte(&r);
            if (b < 0) { rd_close(&r); return -3; }
            v |= (uint32_t)b << (8 * j);
        }
        if (match) {
            *index = v & 0x3FFFFu;
            *advance = len - (int)(v >> 18);
            res = 1;
            break;
        }
        if (below) break;
    }
    rd_close(&r);
    return res;
}

int js8jsc_lookup(js8jsc_t *c, const char *w, int wlen, uint32_t *index, int *advance)
{
    const uint8_t *u = (const uint8_t *)w;
    char name[12];
    int r;

    if (!c || !c->ok || wlen <= 0) return -1;
    if (wlen >= 3 && c->enc_files) {
        mkname(name, 'E', (unsigned)((((uint32_t)u[0] * 31u + u[1]) * 31u + u[2]) % c->enc_files));
        r = scan(c, name, u, wlen, 3, index, advance);
        if (r != 0) return r;
    }
    /* short file: a 1-character entry may follow a failed 2-character one, no early exit */
    return scan(c, "JSCES.BIN", u, wlen, 0, index, advance);
}
