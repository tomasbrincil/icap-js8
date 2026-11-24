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
    uint16_t crc;                               /* of every byte handed out by the file so far */
    uint8_t err;                                /* a read failed (not: end of file) */
    uint8_t buf[RD_BUF];
} rd_t;

uint16_t js8_crc16(uint16_t crc, const void *p, uint32_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--) {
        int i;
        crc ^= (uint16_t)(*b++ << 8);
        for (i = 0; i < 8; i++) crc = (uint16_t)((crc & 0x8000u) ? ((unsigned)crc << 1) ^ 0x1021u : (unsigned)crc << 1);
    }
    return crc;
}

static void mkname(char *name, char kind, unsigned no)
{
    static const char hx[] = "0123456789ABCDEF";
    name[0] = 'J'; name[1] = 'S'; name[2] = 'C'; name[3] = kind;
    name[4] = hx[(no >> 8) & 15]; name[5] = hx[(no >> 4) & 15]; name[6] = hx[no & 15];
    name[7] = '.'; name[8] = 'B'; name[9] = 'I'; name[10] = 'N'; name[11] = 0;
}

static int rd_open(rd_t *r, js8jsc_t *c, const char *name)
{
    r->c = c; r->pos = r->n = 0; r->crc = 0xFFFFu; r->err = 0;
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
        if (n < 0) r->err = 1;
        if (n <= 0) return -1;
        r->n = n; r->pos = 0;
        r->crc = js8_crc16(r->crc, r->buf, (uint32_t)n);
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

/* ---- checksums: 2 bits of state per file (0 not checked, 1 good, 2 corrupt) */
static int fget(const js8jsc_t *c, unsigned k) { return k < JS8JSC_FILES_MAX ? (c->fst[k >> 2] >> ((k & 3u) * 2u)) & 3 : 0; }

/* The lookup has what it wanted from file k (broken: its content made no sense).  Closes the reader.  On the
 * first use of the file, when JSCSUM.BIN is there: read on to the end and compare the CRC of the whole file.
 * Returns 0 (good, or nothing to check against), -3 (I/O error, broken without a checksum), -4 (corrupt). */
static int rd_finish(rd_t *r, unsigned k, int broken)
{
    js8jsc_t *c = r->c;
    uint16_t crc;
    int b0 = -1, b1 = -1;

    if (r->err || !c->sums || k >= JS8JSC_FILES_MAX || fget(c, k)) { rd_close(r); return (broken || r->err) ? -3 : 0; }
    while (rd_byte(r) >= 0) ;
    crc = r->crc;
    rd_close(r);
    if (r->err) return -3;
    if (rd_open(r, c, "JSCSUM.BIN") < 0) return -3;
    c->st_opens--;                                       /* (the statistics count dictionary lookups) */
    if (rd_skip(r, 6 + 2 * (int)k) == 0) { b0 = rd_byte(r); b1 = rd_byte(r); }
    rd_close(r);
    if (b1 < 0) return -3;
    if (crc == (uint16_t)(b0 | b1 << 8)) { c->fst[k >> 2] |= (uint8_t)(1u << ((k & 3u) * 2u)); c->st_checked++; return broken ? -3 : 0; }
    c->fst[k >> 2] |= (uint8_t)(2u << ((k & 3u) * 2u)); c->st_bad++;
    return -4;
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
    c->st_bad = c->st_checked = 0; c->sums = 0;
    for (k = 0; k < (int)sizeof c->fst; k++) c->fst[k] = 0;
    for (k = 0; k < JS8JSC_CACHE_N; k++) { c->cache[k].idx = 0xFFFFFFFFu; c->cache[k].age = 0; }
    if (!io) return -1;
    if (rd_open(&r, c, "JSCSUM.BIN") == 0) {             /* optional; when it is there it must be intact */
        uint32_t n = 0;
        int ok = 1;
        for (;;) {
            int b = rd_byte(&r);
            if (b < 0) break;
            if (n < 4 && b != "JSCS"[n]) ok = 0;
            n++;
        }
        rd_close(&r);
        c->st_opens = 0;
        if (r.err) return -1;
        if (!ok || n < 8 || r.crc != 0) return -5;
        c->sums = 1;
    }
    if (rd_open(&r, c, "JSCHEAD.BIN") < 0) return -1;
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
    if (rd_finish(&r, 0, 0)) return -5;                  /* (without JSCSUM.BIN this only closes the file) */
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
    {
        unsigned fk = 1u + index / c->per_file;          /* its number in JSCSUM.BIN */
        if (fget(c, fk) == 2) return -4;                 /* this file failed its checksum before */
        mkname(name, 'D', index / c->per_file);
        if (rd_open(&r, c, name) < 0) return -2;
        len = -1;
        do {
            uint8_t h[8];
            for (k = 0; k < 8; k++) {
                b = rd_byte(&r);
                if (b < 0) break;
                h[k] = (uint8_t)b;
            }
            if (k < 8 || h[0] != 'J' || h[1] != 'D') break;
            cnt = (unsigned)(h[4] | h[5] << 8);
            skip = index % c->per_file;
            if (skip >= cnt) break;
            while (skip) {
                j = rd_byte(&r);
                if (j < 0 || rd_skip(&r, j) < 0) break;
                skip--;
            }
            if (skip) break;
            j = rd_byte(&r);
            if (j < 0 || j > JS8JSC_WORD_MAX) break;
            for (k = 0; k < j; k++) {
                b = rd_byte(&r);
                if (b < 0) break;
                w[k] = (char)b;
            }
            if (k == j) len = j;
        } while (0);
        k = rd_finish(&r, fk, len < 0);                  /* first use of the file: its checksum */
        if (k < 0) return k;
    }
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
static int scan(js8jsc_t *c, const char *name, unsigned fk, const uint8_t *w, int wlen, int grp,
                uint32_t *index, int *advance)
{
    rd_t r;
    int k, b = 0, len, j, res = 0;

    if (fget(c, fk) == 2) return -4;                     /* this file failed its checksum before */
    if (rd_open(&r, c, name) < 0) return -2;
    for (k = 0; k < 8; k++) {
        b = rd_byte(&r);
        if (b < 0 || (k == 0 && b != 'J') || (k == 1 && b != 'E')) { res = -3; break; }
    }
    while (res == 0) {
        int match = 1, below = 0, decided = 0;
        uint32_t v;
        len = rd_byte(&r);
        if (len < 0) break;                      /* end of file */
        if (len > wlen) match = 0;
        for (j = 0; j < len; j++) {
            b = rd_byte(&r);
            if (b < 0) break;
            if (j < wlen && b != w[j]) {
                match = 0;
                /* entries of one first character are in descending order: once the
                 * 3-character group of w is behind us nothing can match any more */
                if (!decided && j > 0 && j < grp && b < w[j]) below = 1;
                decided = 1;
            }
        }
        v = 0;
        for (j = 0; b >= 0 && j < 3; j++) {
            b = rd_byte(&r);
            if (b >= 0) v |= (uint32_t)b << (8 * j);
        }
        if (b < 0) { res = -3; break; }
        if (match) {
            *index = v & 0x3FFFFu;
            *advance = len - (int)(v >> 18);
            res = 1;
            break;
        }
        if (below) break;
    }
    k = rd_finish(&r, fk, res < 0);                      /* first use of the file: its checksum */
    return k < 0 ? k : res;
}

int js8jsc_lookup(js8jsc_t *c, const char *w, int wlen, uint32_t *index, int *advance)
{
    const uint8_t *u = (const uint8_t *)w;
    char name[12];
    unsigned nd, no;
    int r;

    if (!c || !c->ok || wlen <= 0) return -1;
    nd = (c->total + c->per_file - 1u) / c->per_file;    /* JSCSUM.BIN order: head, nd decode files, JSCES, encode files */
    if (wlen >= 3 && c->enc_files) {
        no = (unsigned)((((uint32_t)u[0] * 31u + u[1]) * 31u + u[2]) % c->enc_files);
        mkname(name, 'E', no);
        r = scan(c, name, 2u + nd + no, u, wlen, 3, index, advance);
        if (r != 0) return r;
    }
    /* short file: a 1-character entry may follow a failed 2-character one, no early exit */
    return scan(c, "JSCES.BIN", 1u + nd, u, wlen, 0, index, advance);
}
