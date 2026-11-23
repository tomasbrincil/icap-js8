/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8sim - HOST simulator of the ICAP loader for the JS8 radio app: the SAME shell source
 * (shell/js8app.c, compiled into this file with -DDIGI_HOST) and the same decoder / message
 * sources as the ARM build run against a fake radio.  It models:
 *   - icap_audio_read is fed from a recording (8000 Hz mono s16 WAV, or headerless .pcm) on a
 *     VIRTUAL clock: sample 0 of the file sits on a 15 s period edge of the fake RTC (plus -e);
 *     before and after the file the receiver hears silence;
 *   - the file API is mapped to a directory (default ./build/sd) with
 *     the loader's rules: names of A-Z 0-9 _ - . up to 48 characters, at most 4 files open,
 *     no append, no seek; reads fall back to a read-only overlay directory (-J) so that test
 *     runs can share one copy of the dictionary files;
 *   - virtual files @MYCALL and @CLOCK; every other @name fails with ICAP_E_NAME;
 *   - DECODE screen, app window (panel) and popups go to stdout;
 *   - icap_tx_* and icap_net_* are present but only COUNT calls (the summary must show 0).
 *
 * Time.  Default = accelerated: icap_sleep_ms() advances the virtual clock without waiting and
 * CPU time spent in the app counts K-fold (-k K, default 1; e.g. -k 30 makes one x86 ms count
 * as 30 ms to mimic a slower processor).  -r = real time (sleeps really sleep, wall clock).
 * CPU time is noisy on a shared machine, so there is also a DETERMINISTIC slow-CPU model:
 * -k 0 -q MS charges no CPU time at all and MS virtual milliseconds for every icap_should_stop()
 * call made WHILE A DECODE RUNS (the decoder polls it through its abort callback; stop polls of the
 * start-up, of the idle loop and of the result phase cost nothing).  On x86 the decoder's polls are
 * 0.07 .. 0.10 ms apart, so -q 3 stands for a processor 30 .. 40 times slower than this PC.  It is an
 * arbitrary model: SD card time is not in it, and it says nothing about the radio's real speed.
 * -k 0 alone (no -q) is the fully deterministic fast case: a decode takes no virtual time at all.
 *
 * usage: js8sim [options] FILE.wav|FILE.pcm
 *   -d DIR     app folder (default ./build/sd)
 *   -J DIR     read-only overlay searched when a file is not in DIR
 *   -c CALL    content of @MYCALL (default empty = not set on the radio)
 *   -t HHMMSS  fake RTC time of file sample 0 (default 120000, date 2026-11-23); must be a period edge
 *   -D YYYYMMDD fake RTC date
 *   -e SEC     file sample 0 is SEC after the period edge (default 0)
 *   -l SEC     the app starts SEC before file sample 0 (default 1.5)
 *   -T SEC     keep running SEC after the end of the file (default 80: lets messages time out)
 *   -k K       CPU time factor (accelerated mode)        -r   real time
 *   -q MS      virtual ms charged per icap_should_stop() call (default 0)
 *   -R N       audio ring size in samples (default 16000; more unread audio = ICAP_E_OVERRUN)
 *   -H BYTES   size of the app data region (default 245760)
 *   -p         print every app-window line when it changes (default: only the final window)
 *   -B         popup is always "busy" (returns ICAP_E_BUSY)
 * Output (stdout):
 *   D <line>            icap_decode_out          P <text>   icap_print
 *   U <text>            popup (newlines shown as " | ")
 *   L<n> <text>         app window line (with -p; the final window always, as "W<n>")
 *   # ...               summary: virtual time, files, audio, decode statistics
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "icap.h"

static const char *g_dir = "build/sd", *g_overlay, *g_mycall = "";
static int16_t *g_s; static long g_n;                 /* the recording */
static double g_vt;                                   /* virtual ms since app start */
static double g_k = 1.0, g_pollcost; static int g_real, g_verbose, g_popbusy;
static double g_lead = 1.5, g_tail = 80.0, g_off = 0.0;
static long g_clock0;                                 /* fake RTC seconds of day at file sample 0 minus g_off */
static int g_year = 2026, g_mon = 11, g_day = 23;
static long g_ring = 16000;
static struct timespec g_last;

/* statistics */
static long st_opens, st_open_fail, st_nofd, st_badname, st_maxopen, st_nopen, st_wbytes, st_rbytes;
static long st_overruns, st_audio_calls, st_tx, st_net, st_popups, st_dec, st_dec_long, st_yield;
static double st_max_step;
static long st_win_long;                              /* app-window lines longer than 40 bytes (the panel's width) */

static void tick(void)
{
    struct timespec t;
    clock_gettime(g_real ? CLOCK_MONOTONIC : CLOCK_THREAD_CPUTIME_ID, &t);
    double d = (double)(t.tv_sec - g_last.tv_sec) * 1000.0 + (double)(t.tv_nsec - g_last.tv_nsec) / 1e6;
    g_last = t;
    d *= g_real ? 1.0 : g_k;
    if (d > st_max_step) st_max_step = d;
    g_vt += d;
}
static double file_t0(void) { return g_lead * 1000.0; }   /* virtual ms of file sample 0 */

/* ---- screen */
static char g_panel[8][41];
static void s_set(int l, const char *s)
{
    if (l < 0 || l > 7) return;
    if (strlen(s) > 40) st_win_long++;
    char b[41]; int k = 0; while (s[k] && k < 40) { b[k] = s[k]; k++; } b[k] = 0;
    if (strcmp(b, g_panel[l])) { strcpy(g_panel[l], b); if (g_verbose) printf("L%d %s\n", l, b); }
}
static void s_print(const char *s) { printf("P %s\n", s); }
static void s_clear(void) { memset(g_panel, 0, sizeof g_panel); }
static void s_decode(const char *s)
{
    st_dec++;
    if (strlen(s) > 40) st_dec_long++;
    printf("D %s\n", s); fflush(stdout);
}
static int s_popup(const char *t)
{
    if (g_popbusy) return ICAP_E_BUSY;
    st_popups++;
    printf("U ");
    for (; *t; t++) if (*t == '\n') printf(" | "); else putchar(*t);
    printf("\n");
    return 0;
}

/* ---- time */
static void s_sleep(uint32_t m) { tick(); if (g_real) usleep(m * 1000); else g_vt += m; }
static void s_yield(void) { st_yield++; tick(); }
static uint32_t s_tick(void) { tick(); return (uint32_t)g_vt + 100000u; }
static int decoding;                                  /* the shell's own variable (defined below with the app): slot being decoded, -1 none */
static int s_stop(void)
{
    tick();
    if (decoding >= 0) g_vt += g_pollcost;            /* the slow-CPU model charges the decoder's abort polls only */
    return g_vt >= file_t0() + (double)g_n / 8.0 + g_tail * 1000.0;
}
static int s_clock(struct icap_time *t)
{
    tick();
    double ms = (double)g_clock0 * 1000.0 + (g_vt - file_t0());
    long sec = (long)(ms / 1000.0); if (ms < 0) sec = (long)((ms - 999.0) / 1000.0);
    int day = g_day;
    while (sec < 0) { sec += 86400; day--; }
    while (sec >= 86400) { sec -= 86400; day++; }
    t->year = (uint16_t)g_year; t->mon = (uint8_t)g_mon; t->day = (uint8_t)day;
    t->hour = (uint8_t)(sec / 3600); t->min = (uint8_t)(sec / 60 % 60); t->sec = (uint8_t)(sec % 60); t->wday = 1;
    return 0;
}

/* ---- audio: a ring that fills in virtual time */
static long g_apos = -1;                              /* next sample to hand out (index from app start) */
static int s_arate(void) { return 8000; }
static int s_aread(int16_t *buf, int n)
{
    tick(); st_audio_calls++;
    long have = (long)(g_vt * 8.0);
    if (g_apos < 0) g_apos = have;                    /* the first call starts the service */
    long avail = have - g_apos;
    if (avail > g_ring) { g_apos = have; st_overruns++; return ICAP_E_OVERRUN; }
    if (avail <= 0) return 0;
    if (avail > n) avail = n;
    long f0 = (long)(file_t0() * 8.0 + g_off * 8000.0);
    for (long i = 0; i < avail; i++) { long k = g_apos + i - f0; buf[i] = k >= 0 && k < g_n ? g_s[k] : 0; }
    g_apos += avail;
    return (int)avail;
}

/* ---- files */
#define MAXFD 4
static struct { int used, fd, virt, vpos, vlen; char vbuf[96]; } g_fd[MAXFD];
static int name_ok(const char *nm)
{
    int n = 0;
    for (; nm[n]; n++) {
        char c = nm[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return n > 0 && n <= ICAP_NAME_MAX;
}
static int s_fopen(const char *nm, int mode)
{
    int h = -1;
    tick(); st_opens++;
    for (int i = 0; i < MAXFD; i++) if (!g_fd[i].used) { h = i; break; }
    if (nm[0] == '@') {
        char v[96];
        if (!strcmp(nm, "@MYCALL")) snprintf(v, sizeof v, "%s", g_mycall);
        else if (!strcmp(nm, "@CLOCK")) snprintf(v, sizeof v, "K=1 S=0 L=1 P=60000 O=1000 N=0");
        else { st_open_fail++; return ICAP_E_NAME; }
        if (mode & ICAP_O_WRITE) { st_open_fail++; return ICAP_E_PERM; }
        if (h < 0) { st_nofd++; return ICAP_E_NOFD; }
        g_fd[h].used = 1; g_fd[h].virt = 1; g_fd[h].vpos = 0; g_fd[h].vlen = (int)strlen(v); strcpy(g_fd[h].vbuf, v);
    } else {
        char p[1024];
        if (!name_ok(nm)) { st_badname++; st_open_fail++; return ICAP_E_NAME; }
        if (h < 0) { st_nofd++; return ICAP_E_NOFD; }
        snprintf(p, sizeof p, "%s/%s", g_dir, nm);
        int fd = open(p, (mode & ICAP_O_WRITE) ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY, 0644);
        if (fd < 0 && !(mode & ICAP_O_WRITE) && g_overlay) { snprintf(p, sizeof p, "%s/%s", g_overlay, nm); fd = open(p, O_RDONLY); }
        if (fd < 0) { st_open_fail++; return ICAP_E_IO; }
        g_fd[h].used = 1; g_fd[h].virt = 0; g_fd[h].fd = fd;
    }
    if (++st_nopen > st_maxopen) st_maxopen = st_nopen;
    return h;
}
static int s_fread(int h, void *b, uint32_t n)
{
    tick();
    if (h < 0 || h >= MAXFD || !g_fd[h].used) return ICAP_E_ARG;
    if (g_fd[h].virt) {
        int k = g_fd[h].vlen - g_fd[h].vpos; if (k > (int)n) k = (int)n;
        memcpy(b, g_fd[h].vbuf + g_fd[h].vpos, (size_t)k); g_fd[h].vpos += k; return k;
    }
    int r = (int)read(g_fd[h].fd, b, n);
    if (r > 0) st_rbytes += r;
    return r < 0 ? ICAP_E_IO : r;
}
static int s_fwrite(int h, const void *b, uint32_t n)
{
    tick();
    if (h < 0 || h >= MAXFD || !g_fd[h].used || g_fd[h].virt) return ICAP_E_ARG;
    int r = (int)write(g_fd[h].fd, b, n);
    if (r > 0) st_wbytes += r;
    return r < 0 ? ICAP_E_IO : r;
}
static int s_fclose(int h)
{
    tick();
    if (h < 0 || h >= MAXFD || !g_fd[h].used) return ICAP_E_ARG;
    if (!g_fd[h].virt) close(g_fd[h].fd);
    g_fd[h].used = 0; st_nopen--;
    return 0;
}
static int s_remove(const char *nm, int flags)
{
    char p[1024]; (void)flags;
    if (!name_ok(nm)) return ICAP_E_NAME;
    snprintf(p, sizeof p, "%s/%s", g_dir, nm);
    return unlink(p) ? ICAP_E_IO : 0;
}

/* ---- things this app must never use: counted, refused */
static int s_txb(void) { st_tx++; return ICAP_E_PERM; }
static int s_txw(const int16_t *b, int n) { (void)b; (void)n; st_tx++; return ICAP_E_PERM; }
static void s_txe(void) { st_tx++; }
static int s_nst(void) { st_net++; return ICAP_E_NONET; }
static int s_nopen(const char *h, uint32_t p, uint32_t f, uint32_t t) { (void)h; (void)p; (void)f; (void)t; st_net++; return ICAP_E_NONET; }
static int s_nio(int h, const void *b, uint32_t n, uint32_t t) { (void)h; (void)b; (void)n; (void)t; st_net++; return ICAP_E_NONET; }
static int s_nrecv(int h, void *b, uint32_t n, uint32_t t) { (void)h; (void)b; (void)n; (void)t; st_net++; return ICAP_E_NONET; }
static int s_nclose(int h) { (void)h; st_net++; return ICAP_E_NONET; }
static int s_nerr(void) { st_net++; return 0; }

static int s_rs(struct icap_radio *r) { memset(r, 0, sizeof *r); r->freq_hz = 7078000; return 0; }
static int s_mem(struct icap_meminfo *m) { memset(m, 0, sizeof *m); return 0; }
static uint32_t s_task(void) { return 1; }
static int s_mode(char *b, int max) { if (max >= 4) { strcpy(b, "JS8"); return 3; } return 0; }

/* ---- the app itself: the radio's shell source, unchanged */
#include "../shell/js8app.c"

static int load(const char *fn)
{
    FILE *f = fopen(fn, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz + 1);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return -1; }
    fclose(f);
    long off = 0, len = sz;
    if (sz >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) {
        long p = 12; int fmt_ok = 0; len = -1;
        while (p + 8 <= sz) {
            uint32_t cl = (uint32_t)b[p + 4] | (uint32_t)b[p + 5] << 8 | (uint32_t)b[p + 6] << 16 | (uint32_t)b[p + 7] << 24;
            if (!memcmp(b + p, "fmt ", 4)) {
                int ch = b[p + 10] | b[p + 11] << 8, bits = b[p + 22] | b[p + 23] << 8;
                uint32_t rate = (uint32_t)b[p + 12] | (uint32_t)b[p + 13] << 8 | (uint32_t)b[p + 14] << 16;
                if (ch != 1 || bits != 16 || rate != 8000) { fprintf(stderr, "%s: need 8000 Hz mono s16 (got %u Hz, %d ch, %d bit)\n", fn, rate, ch, bits); return -1; }
                fmt_ok = 1;
            } else if (!memcmp(b + p, "data", 4)) { off = p + 8; len = cl; if (off + len > sz) len = sz - off; break; }
            p += 8 + cl + (cl & 1);
        }
        if (!fmt_ok || len < 0) { fprintf(stderr, "%s: bad WAV\n", fn); return -1; }
    }
    g_n = len / 2; g_s = malloc((size_t)g_n * 2 + 2);
    memcpy(g_s, b + off, (size_t)g_n * 2);
    free(b);
    return 0;
}

int main(int argc, char **argv)
{
    static struct icap_svc s;
    uint32_t heap = 245760;
    int hh = 12, mi = 0, ss = 0, o;
    while ((o = getopt(argc, argv, "d:J:c:t:D:e:l:T:k:q:rR:H:pB")) != -1) {
        switch (o) {
        case 'd': g_dir = optarg; break;
        case 'J': g_overlay = optarg; break;
        case 'c': g_mycall = optarg; break;
        case 't': { long v = atol(optarg); hh = (int)(v / 10000); mi = (int)(v / 100 % 100); ss = (int)(v % 100); break; }
        case 'D': { long v = atol(optarg); g_year = (int)(v / 10000); g_mon = (int)(v / 100 % 100); g_day = (int)(v % 100); break; }
        case 'e': g_off = atof(optarg); break;
        case 'l': g_lead = atof(optarg); break;
        case 'T': g_tail = atof(optarg); break;
        case 'k': g_k = atof(optarg); break;
        case 'q': g_pollcost = atof(optarg); break;
        case 'r': g_real = 1; break;
        case 'R': g_ring = atol(optarg); break;
        case 'H': heap = (uint32_t)atol(optarg); break;
        case 'p': g_verbose = 1; break;
        case 'B': g_popbusy = 1; break;
        default: fprintf(stderr, "see the header of tests/js8sim.c\n"); return 2;
        }
    }
    if (optind >= argc || load(argv[optind])) { fprintf(stderr, "usage: js8sim [options] FILE.wav|.pcm\n"); return 2; }
    if (ss % 15) { fprintf(stderr, "-t must be a period edge (seconds 00/15/30/45)\n"); return 2; }
    g_clock0 = hh * 3600L + mi * 60L + ss;
    /* the data region must have a 32-bit address (svc.heap_base is uint32_t) */
    void *hp = mmap(0, heap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (hp == MAP_FAILED) { perror("mmap"); return 2; }

    s.abi_version = 3; s.size = sizeof s; s.radio_id = ICAP_RADIO_IC705; s.fw_build = 20261119;
    s.print = s_print; s.set_line = s_set; s.clear = s_clear;
    s.f_open = s_fopen; s.f_read = s_fread; s.f_write = s_fwrite; s.f_close = s_fclose;
    s.sleep_ms = s_sleep; s.yield = s_yield; s.tick_ms = s_tick;
    s.mem_info = s_mem; s.radio_state = s_rs; s.should_stop = s_stop; s.task_id = s_task;
    s.heap_base = (uint32_t)(uintptr_t)hp; s.heap_size = heap;
    s.clock_now = s_clock; s.audio_read = s_aread; s.audio_rate = s_arate; s.decode_out = s_decode;
    s.tx_begin = s_txb; s.tx_audio_write = s_txw; s.tx_end = s_txe;
    s.net_status = s_nst; s.net_open = s_nopen; s.net_send = s_nio; s.net_recv = s_nrecv; s.net_close = s_nclose; s.net_error = s_nerr;
    s.popup = s_popup; s.remove = s_remove; s.mode = s_mode;

    struct timespec w0, w1;
    clock_gettime(CLOCK_MONOTONIC, &w0);
    clock_gettime(g_real ? CLOCK_MONOTONIC : CLOCK_THREAD_CPUTIME_ID, &g_last);
    int rc = app_main(&s);
    clock_gettime(CLOCK_MONOTONIC, &w1);

    for (int i = 0; i < 8; i++) printf("W%d %s\n", i, g_panel[i]);
    printf("# sim rc=%d file=%s samples=%ld (%.1f s) virtual=%.1f s wall=%.2f s k=%g q=%g%s\n", rc, argv[optind], g_n, (double)g_n / 8000.0,
           g_vt / 1000.0, (double)(w1.tv_sec - w0.tv_sec) + (double)(w1.tv_nsec - w0.tv_nsec) / 1e9, g_k, g_pollcost, g_real ? " realtime" : "");
    printf("# files opens=%ld failed=%ld max_open=%ld still_open=%ld nofd=%ld badname=%ld read=%ld B written=%ld B\n",
           st_opens, st_open_fail, st_maxopen, st_nopen, st_nofd, st_badname, st_rbytes, st_wbytes);
    printf("# audio calls=%ld overruns=%ld app_gaps=%u yields=%ld max_step_ms=%.2f\n", st_audio_calls, st_overruns, gaps, st_yield, st_max_step);
    printf("# decode_out lines=%ld over40=%ld popups=%ld tx_calls=%ld net_calls=%ld win_over40=%ld\n", st_dec, st_dec_long, st_popups, st_tx, st_net, st_win_long);
    printf("# app periods=%u frames=%u msgs=%u to_me=%u skipped=%u notes=%d raw=%u popup_busy=%u log_err=%d\n",
           n_periods, n_frames, n_msgs, n_tome, n_skipped, n_notes, n_raw, n_popbusy, log_err);
    printf("# decode ms last=%u avg=%u max=%u (virtual) arena peak=%u of %u (heap %u, shell %u)\n", dec_ms_last,
           n_periods ? dec_ms_sum / n_periods : 0, dec_ms_max, arena_peak, arena_len, heap, heap - arena_len);
    if (jsc) printf("# dict head_words=%u head_bytes=%u opens=%u bytes=%u max_lookup=%u cache_hits=%u head_hits=%u\n", jsc->head_n, jsc->head_bytes,
                    jsc->st_opens, jsc->st_bytes, jsc->st_max_bytes, jsc->st_cache_hits, jsc->st_head_hits);
    else printf("# dict none\n");
    return rc;
}
