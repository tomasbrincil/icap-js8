/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * fuzz_core - adversarial host test of the message layer: random 87-bit frames (every field is
 * attacker controlled) and corrupted dictionary files through js8msg / js8jsc / js8asm.
 * Build with -fsanitize=address,undefined; every output object sits in its own malloc block so
 * that a write one byte past it is caught.
 *
 *   fuzz_core SD_DIR [iterations] [seed]
 *
 * Phases: 1 no dictionary, 2 intact dictionary, 3 dictionary with corrupted files (bit flips,
 * truncation, random bytes, failing opens, short reads), 4 js8jsc_init with a tiny / odd head area.
 * Checks besides the sanitizers: text NUL terminated inside its buffer, js8jsc_word() <= max,
 * file handles balanced (open == close) and never more than one open.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/js8msg.h"
#include "../src/js8asm.h"

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }

/* ---- file layer: whole files in memory, optional corruption per open */
static const char *sd_dir;
static int corrupt;                 /* 0 = intact; 1 = mutate */
static struct { int used; uint8_t *d; long n, pos; int shortrd; } fh[4];
static long n_open, n_close, n_openfail, max_open, cur_open;
static long fails;

static int io_open(void *u, const char *name)
{
    char path[1024]; int i; FILE *f; long n;
    (void)u;
    if (corrupt && rnd() % 23 == 0) { n_openfail++; return -1; }
    for (i = 0; i < 4 && fh[i].used; i++) ;
    if (i == 4) { fails++; printf("FAIL more than 4 files open\n"); return -2; }
    snprintf(path, sizeof path, "%s/%s", sd_dir, name);
    f = fopen(path, "rb");
    if (!f) { n_openfail++; return -1; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    fh[i].d = malloc((size_t)n + 1);
    if (fread(fh[i].d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(fh[i].d); return -1; }
    fclose(f);
    fh[i].shortrd = 0;
    if (corrupt) {
        switch (rnd() % 8) {
        case 0: break;                                               /* intact */
        case 1: n = n ? (long)(rnd() % (uint32_t)n) : 0; break;      /* truncated */
        case 2: { int k = 1 + (int)(rnd() % 16); while (k-- && n) fh[i].d[rnd() % (uint32_t)n] ^= (uint8_t)(1u << (rnd() & 7)); break; }
        case 3: { long k; for (k = 0; k < n; k++) fh[i].d[k] = (uint8_t)rnd(); break; }   /* garbage */
        case 4: { long k; for (k = 8; k < n; k++) fh[i].d[k] = 0xFF; break; }            /* all lengths 255 */
        case 5: { long k; for (k = 16; k < n; k++) fh[i].d[k] = 0; break; }              /* all lengths 0 */
        case 6: { long k; for (k = 4; k < 16 && k < n; k++) fh[i].d[k] = (uint8_t)rnd(); break; }  /* header fields */
        case 7: fh[i].shortrd = 1; break;                            /* reads return 1..3 bytes */
        }
    }
    fh[i].used = 1; fh[i].n = n; fh[i].pos = 0;
    n_open++; if (++cur_open > max_open) max_open = cur_open;
    return i;
}
static int io_read(void *u, int fd, void *buf, uint32_t len)
{
    long k;
    (void)u;
    if (fd < 0 || fd > 3 || !fh[fd].used) { fails++; printf("FAIL read on a closed handle\n"); return -1; }
    k = fh[fd].n - fh[fd].pos;
    if (k > (long)len) k = (long)len;
    if (fh[fd].shortrd && k > 3) k = 1 + (long)(rnd() % 3);
    if (corrupt && rnd() % 4001 == 0) return -5;                     /* I/O error */
    memcpy(buf, fh[fd].d + fh[fd].pos, (size_t)k); fh[fd].pos += k;
    return (int)k;
}
static int io_close(void *u, int fd)
{
    (void)u;
    if (fd < 0 || fd > 3 || !fh[fd].used) { fails++; printf("FAIL close of a closed handle %d\n", fd); return -1; }
    free(fh[fd].d); fh[fd].used = 0; n_close++; cur_open--;
    return 0;
}
static const js8_io_t io = { io_open, io_read, io_close, 0 };

/* ---- frames */
static void put_bits(uint8_t *f, int pos, int n, uint32_t v)
{
    while (n-- > 0) { if ((v >> n) & 1u) f[pos >> 3] |= (uint8_t)(0x80u >> (pos & 7)); else f[pos >> 3] &= (uint8_t)~(0x80u >> (pos & 7)); pos++; }
}
static void rand_frame(uint8_t *f)
{
    int i, kind = (int)(rnd() % 12);
    for (i = 0; i < 11; i++) f[i] = (uint8_t)rnd();
    switch (kind) {
    case 0: memset(f, 0, 9); break;
    case 1: memset(f, 0xFF, 9); break;
    case 2: put_bits(f, 0, 3, 0); break;                             /* heartbeat */
    case 3: put_bits(f, 0, 3, 1 + rnd() % 2); break;                 /* compound */
    case 4: put_bits(f, 0, 3, 3); break;                             /* directed */
    case 5: put_bits(f, 0, 3, 3); put_bits(f, 3, 28, JS8_NBASECALL - 2 + rnd() % 60); put_bits(f, 31, 28, JS8_NBASECALL - 2 + rnd() % 60); break;
    case 6: put_bits(f, 0, 2, 3); break;                             /* JSC data */
    case 7: put_bits(f, 0, 2, 2); break;                             /* Huffman data */
    case 8: { int k; put_bits(f, 0, 2, 3); for (k = 2; k < 72; k += 4) put_bits(f, k, 4, 7 + rnd() % 9); break; }   /* only continuation nibbles */
    case 9: { int k; put_bits(f, 0, 2, 3); for (k = 2; k < 72; k += 5) put_bits(f, k, 5, (rnd() % 7) << 1 | (rnd() & 1)); break; } /* 14 one-nibble words */
    case 10: { int k; for (k = 0; k < 72; k += 21) { put_bits(f, k, 16, 0xFFFF); put_bits(f, k + 16, 5, rnd()); } break; }
    default: break;
    }
    if (rnd() % 3 == 0) f[9] = (uint8_t)((f[9] & 0x1F) | (rnd() % 8) << 5);
    if (kind == 2 || kind == 3 || kind == 4 || kind == 5) f[9] &= (uint8_t)~(JS8_I3_DATA << 5);
    js8_msg_crc_set(f);
}

static long n_emit, max_text, n_tome;
static void on_msg(void *user, const js8asm_msg_t *m)
{
    size_t n = strnlen(m->text, JS8ASM_TEXT_MAX);
    (void)user;
    n_emit++;
    if (n >= JS8ASM_TEXT_MAX) { fails++; printf("FAIL message text not terminated\n"); }
    if ((long)n > max_text) max_text = (long)n;
    if (strnlen(m->from, JS8_CALL_MAX) >= JS8_CALL_MAX || strnlen(m->to, JS8_CALL_MAX) >= JS8_CALL_MAX) { fails++; printf("FAIL call not terminated\n"); }
    if (m->flags & JS8ASM_F_TO_ME) n_tome++;
}

static void run(const char *what, js8jsc_t *jsc, long iters)
{
    js8asm_t *a = malloc(sizeof *a);
    long i, types[256] = { 0 }, trunc = 0, nodict = 0, maxlen = 0;
    uint32_t now = 0;
    static const char *calls[] = { "OK1BT", "OK1BT/P", "", "@ALLCALL", "K", "VERYLONGCALL1", "<....>" };
    js8asm_init(a, calls[rnd() % 7], rnd() & 1 ? "@ALLCALL @OK  @JS8NET" : 0, on_msg, 0);
    for (i = 0; i < iters; i++) {
        uint8_t *f = malloc(11);
        js8_msg_t *m = malloc(sizeof *m);
        char *raw = malloc(13);
        size_t n;
        rand_frame(f);
        memset(m, 0xA5, sizeof *m);
        js8_msg_decode(f, jsc, m);
        n = strnlen(m->text, JS8_TEXT_MAX);
        if (n >= JS8_TEXT_MAX) { fails++; printf("FAIL frame text not terminated\n"); }
        if ((long)n > maxlen) maxlen = (long)n;
        if (strnlen(m->from, JS8_CALL_MAX) >= JS8_CALL_MAX || strnlen(m->to, JS8_CALL_MAX) >= JS8_CALL_MAX || strnlen(m->grid, 5) >= 5) {
            fails++; printf("FAIL call / grid not terminated (type %d)\n", m->type);
        }
        types[m->type]++;
        if (m->flags & JS8_MF_TRUNC) trunc++;
        if (m->flags & JS8_MF_NODICT) nodict++;
        js8_frame_chars(f, raw);
        /* few frequencies so that streams collide, sometimes far away / odd values */
        {
            int freq = rnd() % 8 ? 500 + 40 * (int)(rnd() % 24) + (int)(rnd() % 9) - 4 : (int)(rnd() % 70000) - 35000;
            int snr = rnd() % 16 ? (int)(rnd() % 60) - 30 : (int)rnd();
            js8asm_feed(a, m, freq, snr, now);
        }
        if (rnd() % 6 == 0) { now += 15u * (rnd() % 4); js8asm_tick(a, now); }
        if (rnd() % 5000 == 0) now += 0xFFFFFF00u;                   /* counter wrap */
        if (rnd() % 7000 == 0) now -= 3600u;                         /* clock stepped back */
        free(f); free(m); free(raw);
    }
    now += 1000; js8asm_tick(a, now);
    for (i = 0; i < JS8ASM_STREAMS; i++) if (a->s[i].used && a->s[i].len) { fails++; printf("FAIL stream %ld survives the timeout\n", i); }
    printf("%-22s %ld frames: hb %ld cmp %ld cmpd %ld dir %ld huff %ld jsc %ld fast %ld raw %ld | trunc %ld nodict %ld maxlen %ld | msgs %ld (to me %ld) max %ld | opens %ld close %ld fail %ld max_open %ld\n",
           what, iters, types[0], types[1], types[2], types[3], types[4], types[6], types[7], types[255], trunc, nodict, maxlen,
           n_emit, n_tome, max_text, n_open, n_close, n_openfail, max_open);
    if (n_open != n_close) { fails++; printf("FAIL file handles leaked: %ld opened, %ld closed\n", n_open, n_close); }
    if (max_open > 1) { fails++; printf("FAIL more than one dictionary file open at a time\n"); }
    free(a);
}

int main(int argc, char **argv)
{
    long iters = argc > 2 ? atol(argv[2]) : 200000, i;
    js8jsc_t *jsc = malloc(sizeof *jsc);
    uint8_t *head;
    if (argc < 2) { fprintf(stderr, "usage: fuzz_core SD_DIR [iterations] [seed]\n"); return 2; }
    sd_dir = argv[1];
    if (argc > 3) rs ^= (uint64_t)atol(argv[3]) * 0x9E3779B97F4A7C15ull;

    run("1 no dictionary", 0, iters);

    head = malloc(35840);
    if (js8jsc_init(jsc, &io, head, 35840)) { printf("FAIL intact dictionary does not load from %s\n", sd_dir); return 1; }
    run("2 intact dictionary", jsc, iters / 4);

    corrupt = 1;
    run("3 corrupt bucket files", jsc, iters / 4);
    free(head);

    /* 4: init from corrupted / missing JSCHEAD.BIN into head areas of odd sizes, then use it */
    for (i = 0; i < 300; i++) {
        uint32_t hs = rnd() % 5 == 0 ? 0 : rnd() % 40000;
        uint8_t *h = hs ? malloc(hs) : 0;
        js8jsc_t *c2 = malloc(sizeof *c2);
        int rc;
        memset(c2, 0x5A, sizeof *c2);
        rc = js8jsc_init(c2, &io, h, hs);
        if (rc == 0) {
            int k;
            if (c2->head_bytes > hs) { fails++; printf("FAIL head table overflows its area\n"); }
            for (k = 0; k < 200; k++) {
                char *w = malloc(JS8JSC_WORD_MAX);
                int max = (int)(rnd() % (JS8JSC_WORD_MAX + 1));
                int len = js8jsc_word(c2, rnd() % 4 ? rnd() % 300000 : rnd() % 6000, w, max);
                if (len > max) { fails++; printf("FAIL js8jsc_word returned %d > max %d\n", len, max); }
                free(w);
            }
            run("4 corrupt head", c2, 300);
        }
        free(c2); free(h);
    }
    free(jsc);
    if (n_open != n_close) { fails++; printf("FAIL file handles leaked at the end: %ld opened, %ld closed\n", n_open, n_close); }
    printf("fuzz_core: %s (%ld failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
