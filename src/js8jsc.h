/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8jsc - access to the JS8Call JSC dictionary (262144 words) stored as many
 * small files on the radio's SD card (built by tools/mkjsc.py).  The ICAP file
 * API has no seek, so every lookup opens ONE bucket file and reads it
 * sequentially up to the wanted entry.  Freestanding C99, no libc.
 *
 *   decode:  index -> word      JSCHEAD.BIN (in RAM, optional) / JSCDxxx.BIN
 *   encode:  text  -> index     JSCExxx.BIN + JSCES.BIN (longest prefix; host test tools only)
 *
 * The dictionary itself is derived from JS8Call (C) 2018 Jordan Sherer KN4CRD,
 * GPL-3.0-or-later.
 */
#ifndef JS8JSC_H
#define JS8JSC_H
#include <stdint.h>

#define JS8JSC_SIZE      262144u   /* JSC::size */
#define JS8JSC_WORD_MAX  26        /* longest dictionary word */
#define JS8JSC_CACHE_N   16        /* small LRU of recently decoded words */
#define JS8JSC_CACHE_W   11        /* longest word kept in the LRU */

/* File access supplied by the application (radio: icap_open/read/close,
 * PC: stdio).  open returns fd >= 0 or < 0; read returns bytes read (0 = EOF). */
typedef struct {
    int (*open)(void *user, const char *name);
    int (*read)(void *user, int fd, void *buf, uint32_t len);
    int (*close)(void *user, int fd);
    void *user;
} js8_io_t;

typedef struct {
    const js8_io_t *io;
    const uint8_t  *head;        /* head entries [len][chars]..., caller's memory */
    uint32_t head_n;             /* entries loaded into RAM                       */
    uint32_t head_bytes;
    uint32_t total;              /* 262144                                        */
    uint16_t per_file;           /* entries per JSCDxxx.BIN                       */
    uint16_t enc_files;          /* number of JSCExxx.BIN                         */
    uint8_t  ok;
    uint8_t  clock;
    struct { uint32_t idx; uint8_t len; uint8_t age; char w[JS8JSC_CACHE_W]; } cache[JS8JSC_CACHE_N];
    /* statistics (bytes read from files, files opened, last lookup cost) */
    uint32_t st_bytes, st_opens, st_last_bytes, st_max_bytes, st_cache_hits, st_head_hits;
} js8jsc_t;

/* Read JSCHEAD.BIN; load as many head entries as fit into mem (mem may be
 * NULL / 0: every lookup then goes to the bucket files).  Returns 0 or < 0. */
int js8jsc_init(js8jsc_t *c, const js8_io_t *io, uint8_t *mem, uint32_t mem_size);

/* index -> word (NOT NUL terminated).  Returns the length, or < 0 on error
 * (dictionary not available, file missing, index out of range). */
int js8jsc_word(js8jsc_t *c, uint32_t index, char *out, int max);

/* Longest dictionary prefix of w[0..wlen) exactly as JSC::lookup finds it.
 * Returns 1 and sets *index and *advance (characters consumed), 0 if there is
 * none, < 0 on a file error. */
int js8jsc_lookup(js8jsc_t *c, const char *w, int wlen, uint32_t *index, int *advance);

#endif
