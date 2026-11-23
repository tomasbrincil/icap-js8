/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * arena_guard - review test (PC only): js8_work_peak() is layout arithmetic, not a measurement.
 * This runs js8_decode() on a recording with the work arena placed between two PROT_NONE pages
 * (the arena END touches the upper guard page), filled with a pattern beforehand, and reports the
 * highest byte really written. A write outside the arena kills the process (SIGSEGV).
 *   arena_guard FILE.wav ARENA_BYTES FLO FHI       (every 15 s period of the file, edge = sample 0)
 */
#define _GNU_SOURCE
#include "../src/js8dec.h"
#include "js8synth.h"
#include <sys/mman.h>
#include <unistd.h>
#define CAP 116800
static int16_t *pcm; static long npcm, off;
static int src_read(void *ctx, uint32_t start, float *out, uint32_t n)
{
    (void)ctx;
    if (start >= CAP) return 0;
    if (start + n > CAP) n = CAP - start;
    for (uint32_t i = 0; i < n; i++) { long k = off + start + i; out[i] = k < npcm ? (float)pcm[k] * (1.0f / 32768.0f) : 0.0f; }
    return (int)n;
}
int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: arena_guard FILE.wav ARENA_BYTES FLO FHI\n"); return 2; }
    uint32_t len = (uint32_t)atol(argv[2]);
    long pg = sysconf(_SC_PAGESIZE), span = ((long)len + pg - 1) / pg * pg;
    uint8_t *m = mmap(0, (size_t)(span + 2 * pg), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED || mprotect(m + pg, (size_t)span, PROT_READ | PROT_WRITE)) { perror("mmap"); return 2; }
    uint8_t *arena = m + pg + span - len;                 /* arena end == start of the upper guard page */
    if (len & 7u) { fprintf(stderr, "ARENA_BYTES must be a multiple of 8\n"); return 2; }
    pcm = sy_load(argv[1], &npcm);
    if (!pcm) return 2;
    static js8_result_t res[64];
    uint32_t top = 0, plan = 0; int total = 0, periods = 0;
    for (off = 0; off + CAP / 2 < npcm; off += 120000, periods++) {
        memset(arena, 0xA5, len);
        dsrc_t src = { 0, src_read, CAP, SY_FS };
        int nd = js8_decode(&src, (float)atof(argv[3]), (float)atof(argv[4]), 0.5f, res, 64, arena, len, 0);
        if (nd < 0) { printf("period %d: js8_decode error %d\n", periods, nd); return 1; }
        total += nd;
        uint32_t hi = len;
        while (hi > 0 && arena[hi - 1] == 0xA5) hi--;
        if (hi > top) top = hi;
        if (js8_work_peak() > plan) plan = js8_work_peak();
    }
    printf("%s arena %u band %s..%s: %d periods, %d frames, js8_work_peak %u, highest byte written %u, no guard-page fault\n",
           argv[1], len, argv[3], argv[4], periods, total, plan, top);
    return plan <= len && top <= len ? 0 : 1;
}
