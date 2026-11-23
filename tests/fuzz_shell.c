/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * fuzz_shell - adversarial host test of the radio shell (shell/js8app.c, included unchanged with
 * -DDIGI_HOST) against a fake loader that CHECKS the radio limits and injects faults.
 * Build with -fsanitize=address,undefined.
 *
 *   fuzz_shell frames SD_DIR [iterations] [seed] [dictmode]
 *       the result path only: random / hostile frames straight into on_frame() (DECODE lines, logs,
 *       reassembly, popups, log rollover) with SD write faults.  dictmode: 0 none, 1 intact, 2 corrupt
 *   fuzz_shell app SD_DIR FILE.wav|- [periods] [seed] [faults]
 *       the whole app_main on a virtual clock: audio = the recording looped on period edges (or noise
 *       for "-"), the RTC starts shortly before midnight; faults (bit mask): 1 SD write errors and
 *       full card, 2 RTC steps and RTC not set, 4 audio overruns / stalls, 8 open failures,
 *       16 corrupted JS8LOG.TXT / JS8MSG.TXT / JS8.CFG at start.  Environment COST = virtual ms charged
 *       per service call (default 0.002; 0.02 and more makes every decode run out of time); END_MS = stop
 *       at this virtual time; FAILLOG=from,to = writes to JS8LOG.TXT fail in this window of virtual ms;
 *       STALL=at,ms = one sleep call that returns `ms` late at virtual time `at`; EXPECT_SKIP=n = the app must
 *       count exactly n skipped periods and write the note
 *
 *   fuzz_shell ring SD_DIR
 *       the log ring (archive files) alone, through log_add / log_flush / log_load of the shell: holes are never
 *       overwritten, at most keep= archives exist, after the ring is full the oldest is replaced, a restart
 *       continues where the header line says
 *   fuzz_shell mkwav OUT.wav NSIG
 *       60 s recording for "app": NSIG stations 45 Hz apart from 480 Hz, each sending in every period a
 *       different compressed frame of three rare dictionary words (three SD bucket scans per frame)
 *
 * Checked by the fake loader: DECODE line <= 40 printable ASCII characters, app-window line <= 40
 * printable bytes and row 0..7, file names (A-Z 0-9 _ - . up to 48), at most 4 files open, handles
 * balanced at exit, no write to a read handle, popup text printable and at most one popup request per
 * period, SD calls between two icap_should_stop() polls (app mode: at most MAX_SD_GAP), every log file
 * plain text with known line types, number of archive files <= keep.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <math.h>
#include "icap.h"
#include "../src/js8pack.h"
#include "../src/js8dec.h"
#include "../src/js8enc.h"
#include "../src/js8_constants.h"
#include "js8synth.h"

static uint64_t rs = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }

static long fails;
#define FAIL(...) do { if (fails++ < 40) { printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- knobs */
static const char *sd_dir;
static int g_stop, g_faults, g_dictmode = 1, g_clock_ok = 1, g_full, g_popbusy;
static double g_vt;                              /* virtual ms */
static double g_cost = 0.02;                     /* virtual ms per service call */
static double g_rtc_off;                         /* RTC = g_vt + g_rtc_off (ms since day 0 00:00) */
static uint32_t g_tick0 = 0xFFFFC000u;           /* tick counter wraps 16 s after start */
static int16_t *g_wav; static long g_wn;
static double g_stop_last = -1, g_stop_gap;
static double g_faillog0, g_faillog1;            /* FAILLOG=from,to (virtual ms): writes to JS8LOG.TXT fail in this window */      /* longest virtual time without a stop poll */
static int g_pop_period_open, g_app_mode; static double g_pop_last = -1;
static long st_dec, st_dec_long, st_dec_max, st_line_long, st_line_max, st_pop, st_pop_max, st_pop_lines;
static long st_opens, st_openfail, st_maxopen, st_nopen, st_wfail, st_overrun;
static long sd_o, sd_r, sd_w, sd_omax, sd_rmax, sd_wmax;   /* SD calls since the last stop poll, and the worst */

#define MAX_SD_GAP 40                            /* SD calls allowed between two stop polls: one log rewrite + one archive */
static void step(void) { g_vt += g_cost; }

/* ---- screen */
static char g_panel[8][128];
static void s_set(int l, const char *s)
{
    size_t n = strlen(s);
    step();
    if (l < 0 || l > 7) { FAIL("set_line row %d", l); return; }
    if ((long)n > st_line_max) st_line_max = (long)n;
    if (n > 40) { st_line_long++; FAIL("app-window line %d is %zu bytes: \"%s\"", l, n, s); }
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) { FAIL("app-window line %d with byte 0x%02x", l, (unsigned char)s[i]); break; }
    if (l == 3 && strcmp(g_panel[l], s)) g_pop_period_open = 0;   /* "last HHMMSS: ..." changes once per period */
    snprintf(g_panel[l], sizeof g_panel[l], "%s", s);
}
static void s_print(const char *s) { (void)s; step(); }
static void s_clear(void) { step(); }
static void s_decode(const char *s)
{
    size_t n = strlen(s), i;
    step(); st_dec++;
    if ((long)n > st_dec_max) st_dec_max = (long)n;
    if (n > 40) { st_dec_long++; FAIL("DECODE line of %zu characters: \"%s\"", n, s); }
    for (i = 0; i < n; i++) if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) { FAIL("DECODE line with byte 0x%02x", (unsigned char)s[i]); break; }
}
static int s_popup(const char *t)
{
    long n = (long)strlen(t), lines = 1, i;
    step();
    for (i = 0; i < n; i++) if (t[i] == '\n') lines++;
    for (i = 0; i < n; i++) if (t[i] != '\n' && ((unsigned char)t[i] < 32 || (unsigned char)t[i] > 126)) { FAIL("popup text with byte 0x%02x", (unsigned char)t[i]); break; }
    if (g_pop_period_open++) FAIL("second popup request in one period");
    if (g_vt - g_pop_last < 14000.0 && g_pop_last >= 0 && g_app_mode) FAIL("two popup requests %.0f ms apart", g_vt - g_pop_last);
    g_pop_last = g_vt;
    if (n > st_pop_max) st_pop_max = n;
    if (lines > st_pop_lines) st_pop_lines = lines;
    if (g_popbusy && rnd() % 2) return ICAP_E_BUSY;
    st_pop++;
    return 0;
}

/* ---- time */
static double g_stall_at = -1, g_stall_ms;       /* STALL=at,ms: one service call that blocks for `ms` at virtual time `at` */
static void s_sleep(uint32_t m)
{
    step(); g_vt += m;
    if (g_stall_at >= 0 && g_vt >= g_stall_at) { g_vt += g_stall_ms; g_stall_at = -1; }
}
static void s_yield(void) { step(); }
static uint32_t s_tick(void) { step(); return (uint32_t)(int64_t)g_vt + g_tick0; }
static double g_end = 1e18, g_next_evt = 20000.0;
static long st_rtc_steps, st_rtc_bad;
static int s_stop(void)
{
    step();
    if (g_stop_last >= 0 && g_vt - g_stop_last > g_stop_gap) g_stop_gap = g_vt - g_stop_last;
    g_stop_last = g_vt;
    if (sd_o + sd_r + sd_w > sd_omax + sd_rmax + sd_wmax) { sd_omax = sd_o; sd_rmax = sd_r; sd_wmax = sd_w; }
    sd_o = sd_r = sd_w = 0;
    if (g_faults && g_vt >= g_next_evt) {                    /* fault scheduler (app mode) */
        g_next_evt = g_vt + 3000.0 + (double)(rnd() % 40000);
        if (g_faults & 2) switch (rnd() % 6) {
            case 0: g_rtc_off += 1000.0 * (double)(1 + rnd() % 20); st_rtc_steps++; break;       /* owner sets the clock forward */
            case 1: g_rtc_off -= 1000.0 * (double)(1 + rnd() % 200); st_rtc_steps++; break;      /* ... backward */
            case 2: g_rtc_off += (double)(rnd() % 2000) - 1000.0; st_rtc_steps++; break;         /* sub-second step */
            case 3: g_clock_ok = !g_clock_ok; st_rtc_bad++; break;
            default: g_clock_ok = 1; break;
        }
        if (g_faults & 1) g_full = rnd() % 4 == 0;
    }
    return g_stop || g_vt >= g_end;
}
static int s_clock(struct icap_time *t)
{
    static const int mdays[2] = { 30, 31 };              /* November, December 2026 */
    step();
    if (!g_clock_ok) { if (rnd() & 1) return ICAP_E_IO; memset(t, 0, sizeof *t); t->year = 2000; t->mon = 1; t->day = 1; return 0; }
    double ms = g_vt + g_rtc_off;
    long sec = (long)(ms / 1000.0), day;
    if (ms < 0) sec = 0;
    day = sec / 86400; sec %= 86400;
    t->year = 2026; t->mon = 11; t->day = (uint8_t)(29 + day);
    if (t->day > mdays[0]) { t->day = (uint8_t)(t->day - mdays[0]); t->mon = 12; }
    t->hour = (uint8_t)(sec / 3600); t->min = (uint8_t)(sec / 60 % 60); t->sec = (uint8_t)(sec % 60); t->wday = 0;
    return 0;
}

/* ---- audio: ring that fills in virtual time; the recording is looped on 15 s period edges of the RTC */
static long g_apos = -1; static int g_stall;
static int s_arate(void) { return 8000; }
static int g_init_only;                          /* stop as soon as the app starts the audio service (= start-up done) */
static int s_aread(int16_t *buf, int n)
{
    step();
    if (g_init_only) { g_stop = 1; return 0; }
    long have = (long)(g_vt * 8.0);
    if (g_apos < 0) g_apos = have;
    if ((g_faults & 4) && rnd() % 20000 == 0) g_stall = 1 + (int)(rnd() % 3000);
    if (g_stall) { g_stall--; return 0; }
    long avail = have - g_apos;
    if (avail > 16000) { g_apos = have; st_overrun++; return ICAP_E_OVERRUN; }
    if (avail <= 0) return 0;
    if (avail > n) avail = n;
    for (long i = 0; i < avail; i++) {
        if (g_wav) {
            double ms = (double)(g_apos + i) / 8.0 + g_rtc_off;   /* RTC ms of this sample */
            long k = (long)(ms * 8.0); long per = 15L * 8000L, loop = (g_wn / per) * per;
            if (loop <= 0) loop = per;
            k %= loop; if (k < 0) k += loop;
            buf[i] = k < g_wn ? g_wav[k] : 0;
        } else buf[i] = (int16_t)((int)(rnd() % 2001) - 1000);
    }
    g_apos += avail;
    return (int)avail;
}

/* ---- files: app files in memory, dictionary files read from SD_DIR (optionally corrupted) */
#define NFILES 12000
static struct { char name[52]; uint8_t *d; long n; } g_f[NFILES];
static struct { int used, wr, fi; uint8_t *d; long n, pos, cap; int shortrd; } g_h[4];
static int name_ok(const char *nm)
{
    int n = 0;
    for (; nm[n]; n++) {
        char c = nm[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return n > 0 && n <= ICAP_NAME_MAX;
}
static int ffind(const char *nm) { for (int i = 0; i < NFILES; i++) if (g_f[i].name[0] && !strcmp(g_f[i].name, nm)) return i; return -1; }
static void fput(const char *nm, const void *d, long n)
{
    int i = ffind(nm);
    if (i < 0) for (i = 0; i < NFILES && g_f[i].name[0]; i++) ;
    if (i >= NFILES) { FAIL("fake card: more than %d files", NFILES); return; }
    free(g_f[i].d); if (nm != g_f[i].name) snprintf(g_f[i].name, sizeof g_f[i].name, "%s", nm);
    g_f[i].d = malloc((size_t)n + 1); memcpy(g_f[i].d, d, (size_t)n); g_f[i].n = n;
}
static int s_fopen(const char *nm, int mode)
{
    int h = -1;
    step(); st_opens++; sd_o++;
    for (int i = 0; i < 4; i++) if (!g_h[i].used) { h = i; break; }
    if (nm[0] == '@') {
        if (strcmp(nm, "@MYCALL") || (mode & ICAP_O_WRITE)) { st_openfail++; return ICAP_E_NAME; }
        if (h < 0) { FAIL("5th file opened (%s)", nm); return ICAP_E_NOFD; }
        const char *v = "ok1bt";
        g_h[h].d = malloc(8); memcpy(g_h[h].d, v, 5); g_h[h].n = 5;
    } else {
        if (!name_ok(nm)) { FAIL("bad file name \"%s\"", nm); return ICAP_E_NAME; }
        if (h < 0) { FAIL("5th file opened (%s)", nm); return ICAP_E_NOFD; }
        if ((g_faults & 8) && rnd() % 40 == 0) { st_openfail++; return ICAP_E_IO; }
        if (mode & ICAP_O_WRITE) {
            if (g_full && rnd() % 3 == 0) { st_openfail++; return ICAP_E_IO; }
            fput(nm, "", 0);                                 /* create or truncate */
            g_h[h].fi = ffind(nm); g_h[h].d = 0; g_h[h].n = 0; g_h[h].cap = 0;
        } else {
            int fi = ffind(nm);
            if (fi >= 0) { g_h[h].d = malloc((size_t)g_f[fi].n + 1); memcpy(g_h[h].d, g_f[fi].d, (size_t)g_f[fi].n); g_h[h].n = g_f[fi].n; }
            else if (!strncmp(nm, "JSC", 3) && g_dictmode) {
                char p[1024]; snprintf(p, sizeof p, "%s/%s", sd_dir, nm);
                FILE *f = fopen(p, "rb");
                if (!f) { st_openfail++; return ICAP_E_IO; }
                fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
                g_h[h].d = malloc((size_t)n + 1);
                if (fread(g_h[h].d, 1, (size_t)n, f) != (size_t)n) n = 0;
                fclose(f);
                if (g_dictmode == 2) switch (rnd() % 6) {
                    case 1: n = n ? (long)(rnd() % (uint32_t)n) : 0; break;
                    case 2: for (int k = 1 + (int)(rnd() % 16); k-- && n; ) g_h[h].d[rnd() % (uint32_t)n] ^= (uint8_t)(1u << (rnd() & 7)); break;
                    case 3: for (long k = 8; k < n; k++) g_h[h].d[k] = (uint8_t)rnd(); break;
                    case 4: for (long k = 8; k < n; k++) if (g_h[h].d[k] >= 'A') g_h[h].d[k] = (uint8_t)(rnd() % 3 ? '\n' : 0x80 + rnd() % 128); break;
                    default: break;
                }
                g_h[h].n = n;
            } else { st_openfail++; return ICAP_E_IO; }
        }
    }
    g_h[h].used = 1; g_h[h].wr = (mode & ICAP_O_WRITE) != 0; g_h[h].pos = 0; g_h[h].shortrd = (g_faults & 16) && rnd() % 4 == 0;
    if (++st_nopen > st_maxopen) st_maxopen = st_nopen;
    return h;
}
static int s_fread(int h, void *b, uint32_t n)
{
    step(); sd_r++;
    if (h < 0 || h > 3 || !g_h[h].used) { FAIL("read on bad handle %d", h); return ICAP_E_ARG; }
    if (g_h[h].wr) { FAIL("read on a write handle"); return ICAP_E_ARG; }
    long k = g_h[h].n - g_h[h].pos; if (k > (long)n) k = (long)n;
    if (g_h[h].shortrd && k > 7) k = 1 + (long)(rnd() % 7);
    memcpy(b, g_h[h].d + g_h[h].pos, (size_t)k); g_h[h].pos += k;
    return (int)k;
}
static int s_fwrite(int h, const void *b, uint32_t n)
{
    step(); sd_w++;
    if (h < 0 || h > 3 || !g_h[h].used) { FAIL("write on bad handle %d", h); return ICAP_E_ARG; }
    if (!g_h[h].wr) { FAIL("write on a read handle"); return ICAP_E_ARG; }
    if (g_full || ((g_faults & 1) && rnd() % 3000 == 0)) { st_wfail++; return rnd() & 1 ? ICAP_E_IO : (int)(n / 2); }
    if (g_faillog1 > g_faillog0 && g_vt >= g_faillog0 && g_vt < g_faillog1 && g_h[h].fi >= 0 && !strncmp(g_f[g_h[h].fi].name, "JS8LOG", 6)) { st_wfail++; return ICAP_E_IO; }
    if (g_h[h].n + (long)n > g_h[h].cap) { g_h[h].cap = (g_h[h].n + (long)n) * 2 + 4096; g_h[h].d = realloc(g_h[h].d, (size_t)g_h[h].cap); }
    memcpy(g_h[h].d + g_h[h].n, b, n); g_h[h].n += (long)n;
    return (int)n;
}
static int s_fclose(int h)
{
    step();
    if (h < 0 || h > 3 || !g_h[h].used) { FAIL("close of bad handle %d", h); return ICAP_E_ARG; }
    if (g_h[h].wr && g_h[h].fi >= 0) fput(g_f[g_h[h].fi].name, g_h[h].d ? (void *)g_h[h].d : (void *)"", g_h[h].n);
    free(g_h[h].d); g_h[h].d = 0; g_h[h].used = 0; st_nopen--;
    return 0;
}
static int s_remove(const char *nm, int flags) { (void)nm; (void)flags; FAIL("icap_remove called"); return ICAP_E_PERM; }
static int s_txb(void) { FAIL("TX call"); return ICAP_E_PERM; }
static int s_txw(const int16_t *b, int n) { (void)b; (void)n; FAIL("TX call"); return ICAP_E_PERM; }
static void s_txe(void) { FAIL("TX call"); }
static int s_nst(void) { FAIL("net call"); return ICAP_E_NONET; }
static int s_rs(struct icap_radio *r) { memset(r, 0, sizeof *r); return 0; }
static int s_mem(struct icap_meminfo *m) { memset(m, 0, sizeof *m); return 0; }
static uint32_t s_task(void) { return 1; }
static int s_mode(char *b, int max) { if (max >= 4) { strcpy(b, "JS8"); return 3; } return 0; }

#include "../shell/js8app.c"

/* ---- hostile frames */
static void put_bits(uint8_t *f, int pos, int n, uint32_t v)
{
    while (n-- > 0) { if ((v >> n) & 1u) f[pos >> 3] |= (uint8_t)(0x80u >> (pos & 7)); else f[pos >> 3] &= (uint8_t)~(0x80u >> (pos & 7)); pos++; }
}
static void rand_frame(uint8_t *f)
{
    int i, kind = (int)(rnd() % 10);
    for (i = 0; i < 11; i++) f[i] = (uint8_t)rnd();
    switch (kind) {
    case 0: put_bits(f, 0, 3, 0); break;
    case 1: put_bits(f, 0, 3, 1 + rnd() % 2); break;
    case 2: put_bits(f, 0, 3, 3); break;
    case 3: put_bits(f, 0, 2, 3); break;
    case 4: put_bits(f, 0, 2, 2); break;
    case 5: { int k; put_bits(f, 0, 2, 3); for (k = 2; k < 72; k += 5) put_bits(f, k, 5, (rnd() % 7) << 1 | (rnd() & 1)); break; }
    case 6: { int k; put_bits(f, 0, 2, 3); for (k = 2; k + 21 <= 72; k += 21) { put_bits(f, k, 16, 0x7000u | (rnd() & 0xFFFu) | 0x8888u); put_bits(f, k + 16, 5, rnd()); } break; }
    default: break;
    }
    if (kind <= 2) f[9] &= 0x7F;
    if (rnd() % 3 == 0) f[9] = (uint8_t)((f[9] & 0x9F) | (rnd() % 4) << 5);
    js8_msg_crc_set(f);
}

/* JSC frame "HELLO" (if the dictionary is there) / newline (word 69) / 0x1A (word 72) / word 5 ...: the text of
 * this frame must never start a new line in a log or reach the popup raw */
static void ctl_frame(uint8_t *f, int i3)
{
    static const uint8_t nib[] = { 15, 6, 0xFF, 7, 7, 2, 0xFE, 15, 6, 0xFF, 5, 0xFE, 15, 6, 0xFF, 7, 7, 2, 0xFF };   /* 0xFF = separator bit 0, 0xFE = 1 */
    int pos = 2;
    memset(f, 0xFF, 11);
    put_bits(f, 0, 2, 3);
    for (unsigned k = 0; k < sizeof nib && pos + 5 < 72; k++) {
        if (nib[k] >= 0xFE) { put_bits(f, pos, 1, nib[k] == 0xFE); pos++; }
        else { put_bits(f, pos, 4, nib[k]); pos += 4; }
    }
    put_bits(f, pos, 1, 0);                              /* pad: one 0, then 1s */
    f[9] = (uint8_t)((f[9] & 0x1F) | (i3 << 5));
    js8_msg_crc_set(f);
}

static uint8_t q_fr[80][JS8_FRAME_BYTES]; static int q_n, q_i, q_freq;
static void build_hostile(js8jsc_t *pj)
{
    static const char *to[] = { "OK1BT", "OK1BT", "@ALLCALL", "OK1BT/P", "Q1ABC", "@OK", "0K1BT" };
    static const char *cmd[] = { " MSG ", " ", ">", " MSG TO:Q2XYZ ", " QUERY CALL OK1BT? ", " SNR -12", " ACK ", " CMD ", " HEARTBEAT SNR +05 " };
    static const char *wd[] = { "OK1BT", "THE", "QUICK", "BROWN", "FOX", "73", "E", "?", "OK1BT/P", "XOK1BT", "5NN", "...", "A", "TEST", "MEET", "1900Z", "WWWWWWWWWWWW", "@@@", "++", "Q" };
    char t[400]; int n = 0, len = (int)(rnd() % 236);
    js8_build_t cfg = { rnd() % 5 ? "Q1ABC" : "DL/Q2ABC/P", "JO70", (uint8_t)(rnd() & 1), rnd() & 1 ? pj : 0 };
    n += snprintf(t + n, sizeof t - (size_t)n, "%s%s", to[rnd() % 7], cmd[rnd() % 9]);
    while (n < len) {
        if (rnd() % 4 == 0) { int k = 1 + (int)(rnd() % 50); char ch = "W 9/?.E"[rnd() % 7]; while (k-- && n < 300) t[n++] = ch; t[n] = 0; }
        else n += snprintf(t + n, sizeof t - (size_t)n, "%s ", wd[rnd() % 20]);
    }
    if (n > JS8_LINE_MAX - 1) n = JS8_LINE_MAX - 1;
    t[n] = 0;
    q_n = js8_build_message(&cfg, t, q_fr, 80); q_i = 0;
    if (q_n < 0) q_n = 0;
    q_freq = 400 + 50 * (int)(rnd() % 30);
}

/* every log file on the fake card must be plain text lines (CR LF, printable ASCII) whatever was received;
 * JS8LOG.TXT / JS8MSG.TXT start with the ring header and every other line starts with a known letter */
static void check_text_files(void)
{
    for (int i = 0; i < NFILES; i++) {
        if (strncmp(g_f[i].name, "JS8L", 4) && strncmp(g_f[i].name, "JS8M", 4)) continue;
        const uint8_t *d = g_f[i].d; long n = g_f[i].n, k, ls = 0;
        int isframes = !strncmp(g_f[i].name, "JS8L", 4), main_ = !strcmp(g_f[i].name, "JS8LOG.TXT") || !strcmp(g_f[i].name, "JS8MSG.TXT");
        for (k = 0; k < n; k++) { uint8_t ch = d[k]; if ((ch < 32 && ch != '\r' && ch != '\n') || ch > 126) { FAIL("%s: byte 0x%02x at offset %ld of %ld (not text)", g_f[i].name, ch, k, n); break; } }
        if (main_ && n && (n < 15 || d[0] != 'H' || d[1] != ' ' || memcmp(d + 2, g_f[i].name, 3) || d[13] != '\r' || d[14] != '\n')) FAIL("%s does not start with the ring header", g_f[i].name);
        for (k = 0; k < n; k++) {
            if (d[k] == '\r' && (k + 1 >= n || d[k + 1] != '\n')) { FAIL("%s: bare CR at %ld", g_f[i].name, k); break; }
            if (d[k] == '\n' && (k == 0 || d[k - 1] != '\r')) { FAIL("%s: bare LF at %ld", g_f[i].name, k); break; }
            if (d[k] == '\n') {
                char c0 = (char)d[ls];
                int ok = isframes ? (c0 == 'R' || c0 == 'N' || (c0 == 'H' && ls == 0)) : (c0 == 'M' || (c0 == 'H' && ls == 0));
                if (!ok || (k - ls >= 2 && d[ls + 1] != ' ')) { FAIL("%s: line at %ld starts with \"%.12s\"", g_f[i].name, ls, d + ls); break; }
                ls = k + 1;
            }
        }
    }
}
static int count_arch(const char *pfx)
{
    int n = 0;
    for (int i = 0; i < NFILES; i++) if (!strncmp(g_f[i].name, pfx, 4) && g_f[i].name[4] >= '0' && g_f[i].name[4] <= '9') n++;
    return n;
}
static void wipe_card(void) { for (int i = 0; i < NFILES; i++) { free(g_f[i].d); g_f[i].d = 0; g_f[i].n = 0; g_f[i].name[0] = 0; } }

static void check_logs(void)
{
    if (lg_frames.len > lg_frames.cap || lg_msgs.len > lg_msgs.cap) FAIL("log tail longer than its buffer (%u/%u, %u/%u)", lg_frames.len, lg_frames.cap, lg_msgs.len, lg_msgs.cap);
}

static struct icap_svc svc;
static uint8_t *heap; static uint32_t heap_len = 245760;
static void setup(void)
{
    /* data region at a 32-bit address with an inaccessible page right behind it */
    uint8_t *p = mmap(0, heap_len + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    mprotect(p + heap_len, 4096, PROT_NONE);
    heap = p;
    svc.abi_version = 3; svc.size = sizeof svc; svc.radio_id = ICAP_RADIO_IC705; svc.fw_build = 20261119;
    svc.print = s_print; svc.set_line = s_set; svc.clear = s_clear;
    svc.f_open = s_fopen; svc.f_read = s_fread; svc.f_write = s_fwrite; svc.f_close = s_fclose;
    svc.sleep_ms = s_sleep; svc.yield = s_yield; svc.tick_ms = s_tick;
    svc.mem_info = s_mem; svc.radio_state = s_rs; svc.should_stop = s_stop; svc.task_id = s_task;
    svc.heap_base = (uint32_t)(uintptr_t)p; svc.heap_size = heap_len;
    svc.clock_now = s_clock; svc.audio_read = s_aread; svc.audio_rate = s_arate; svc.decode_out = s_decode;
    svc.tx_begin = s_txb; svc.tx_audio_write = s_txw; svc.tx_end = s_txe; svc.net_status = s_nst;
    svc.popup = s_popup; svc.remove = s_remove; svc.mode = s_mode;
}
static void garbage_file(const char *nm, long n, int kind)
{
    uint8_t *d = malloc((size_t)n + 1);
    for (long i = 0; i < n; i++) d[i] = kind == 0 ? (uint8_t)rnd() : kind == 1 ? 'A' : (uint8_t)(rnd() % 40 ? 'a' + rnd() % 26 : '\n');
    fput(nm, d, n); free(d);
}
static void summary(const char *what)
{
    printf("%s: DECODE lines %ld (longest %ld, over 40: %ld), app-window longest %ld (lines over 40: %ld), popups %ld (longest %ld B, %ld lines)\n",
           what, st_dec, st_dec_max, st_dec_long, st_line_max, st_line_long, st_pop, st_pop_max, st_pop_lines);
    printf("   files: opens %ld failed %ld max_open %ld still_open %ld write_faults %ld | overruns %ld | longest stop-poll gap %.0f ms (virtual)\n",
           st_opens, st_openfail, st_maxopen, st_nopen, st_wfail, st_overrun, g_stop_gap);
    printf("   most SD calls between two stop polls: %ld opens + %ld reads + %ld writes\n", sd_omax, sd_rmax, sd_wmax);
    printf("   rtc steps %ld, rtc-not-set toggles %ld\n", st_rtc_steps, st_rtc_bad);
    printf("   app: periods %u frames %u msgs %u to_me %u popup_busy %u skipped %u notes %d raw %u log_err %d gaps %u\n",
           n_periods, n_frames, n_msgs, n_tome, n_popbusy, n_skipped, n_notes, n_raw, log_err, gaps);
    for (int i = 0; i < 8; i++) printf("   W%d %s\n", i, g_panel[i]);
    if (st_nopen) FAIL("%ld file handle(s) still open at exit", st_nopen);
    if (st_maxopen > 4) FAIL("more than 4 files open");
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: fuzz_shell frames SD_DIR [iters] [seed] [dictmode] | app SD_DIR FILE.wav|- [periods] [seed] [faults] | ring SD_DIR\n"); return 2; }
    if (!strcmp(argv[1], "mkwav") && argc > 3) {
        long n = 60L * 8000L; int ns = atoi(argv[3]);
        double *b = calloc((size_t)n, sizeof *b); int16_t *o = malloc((size_t)n * 2);
        for (int per = 0; per < 4; per++)
            for (int k = 0; k < ns; k++) {
                uint8_t f[11], tones[79]; memset(f, 0, sizeof f);
                put_bits(f, 0, 2, 3);
                for (int w = 0; w < 3; w++) {                /* 4 continuation nibbles + 1 final + separator bit: index 5740..51666 */
                    for (int q = 0; q < 4; q++) put_bits(f, 2 + 21 * w + 4 * q, 4, 7 + rnd() % 9);
                    put_bits(f, 2 + 21 * w + 16, 4, rnd() % 7); put_bits(f, 2 + 21 * w + 20, 1, 1);
                }
                put_bits(f, 65, 7, 0x3F);                    /* pad: one 0 then 1s */
                f[9] = (uint8_t)((per == 0 ? JS8_I3_FIRST : per == 3 ? JS8_I3_LAST : 0) << 5);
                js8_msg_crc_set(f);
                js8_encode(f, js8_costas_normal, tones);
                sy_add_cpfsk(b, n, tones, 480.0 + 45.0 * k, per * 120000.0 + 4000.0, 600.0);
            }
        sy_add_noise(b, n, 100.0);
        sy_quantize(b, o, n);
        int rc = sy_save_wav(argv[2], o, n) ? 2 : 0; free(b); free(o); return rc;
    }
    sd_dir = argv[2];
    setup();
    if (!strcmp(argv[1], "ring")) {
        char line[80]; int seq = 0, i, k;
        g_init_only = 1; g_clock_ok = 0; g_dictmode = 0;
        /* --- 1: holes.  JS8L0002 and JS8L0005 exist (the owner deleted 1, 3, 4): they are never overwritten */
        fput("JS8.CFG", "keep=8\n", 7);
        fput("JS8L0002.TXT", "KEEP ME 2\r\n", 11); fput("JS8L0005.TXT", "KEEP ME 5\r\n", 11);
        app_main(&svc);                                      /* init only */
        g_init_only = 0; g_stop = 0; quit = 0;
        lg_frames.len = 0; lg_frames.dirty = 0;              /* (the "no dictionary" note of the start-up) */
        for (i = 0; i < 4 * 120 + 10; i++) { sprintf(line, "R %06d 0123456789012345678901234567890123456789", seq++); log_add(&lg_frames, line); if (i % 9 == 8) log_flush(&lg_frames); }
        k = ffind("JS8L0002.TXT"); if (k < 0 || g_f[k].n != 11 || memcmp(g_f[k].d, "KEEP ME 2", 9)) FAIL("ring 1: JS8L0002.TXT was overwritten");
        k = ffind("JS8L0005.TXT"); if (k < 0 || g_f[k].n != 11 || memcmp(g_f[k].d, "KEEP ME 5", 9)) FAIL("ring 1: JS8L0005.TXT was overwritten");
        if (ffind("JS8L0001.TXT") < 0 || ffind("JS8L0003.TXT") < 0 || ffind("JS8L0004.TXT") < 0 || ffind("JS8L0006.TXT") < 0 || count_arch("JS8L") != 6 || lg_frames.full)
            FAIL("ring 1: expected new archives 1, 3, 4, 6 next to the old 2, 5 (have %d, full %d)", count_arch("JS8L"), lg_frames.full);
        printf("ring 1 (holes): %d archives, next %d, full %d\n", count_arch("JS8L"), lg_frames.next, lg_frames.full);
        /* --- 2: the ring fills (keep=8, two foreign files inside it), then the oldest is replaced and a note is due */
        for (i = 0; i < 12 * 120; i++) {
            sprintf(line, "R %06d 0123456789012345678901234567890123456789", seq++); log_add(&lg_frames, line); if (i % 9 == 8) log_flush(&lg_frames);
            /* at EVERY step: the archives + the RAM tail hold a gap-free run of lines up to the newest one (the first
             * overwrite must hit the oldest archive, never the newest) */
            static char have[100000]; int lo2 = seq, cnt = 0;
            memset(have, 0, (size_t)seq + 1);
            for (k = 0; k < NFILES; k++) if (!strncmp(g_f[k].name, "JS8L0", 5))
                for (long p = 0; p + 8 < g_f[k].n; p++) if (g_f[k].d[p] == 'R' && (p == 0 || g_f[k].d[p - 1] == '\n')) { int v = atoi((const char *)g_f[k].d + p + 2); if (!have[v]++) cnt++; if (v < lo2) lo2 = v; }
            for (uint32_t p = 0; p + 8 < lg_frames.len; p++) if (lg_frames.buf[p] == 'R' && (p == 0 || lg_frames.buf[p - 1] == '\n')) { int v = atoi(lg_frames.buf + p + 2); if (!have[v]++) cnt++; if (v < lo2) lo2 = v; }
            if (cnt != seq - lo2) { FAIL("ring 2: after line %d the kept lines %d..%d have a gap (%d of %d)", seq - 1, lo2, seq - 1, cnt, seq - lo2); break; }
        }
        log_flush(&lg_frames);
        if (count_arch("JS8L") != 8 || !lg_frames.full || !lg_frames.said || !pending_note) FAIL("ring 2: %d archives, full %d, said %d", count_arch("JS8L"), lg_frames.full, lg_frames.said);
        {   /* the 8 archives + JS8LOG.TXT hold the newest lines, each once, without a gap up to the last one */
            static char seen[100000]; int lo = seq, n = 0;
            for (i = 0; i < NFILES; i++) if (!strncmp(g_f[i].name, "JS8L", 4))
                for (long p = 0; p + 8 < g_f[i].n; p++) if (g_f[i].d[p] == 'R' && (p == 0 || g_f[i].d[p - 1] == '\n')) {
                    int v = atoi((const char *)g_f[i].d + p + 2);
                    if (seen[v]++) FAIL("ring 2: line %d twice", v);
                    if (v < lo) lo = v;
                    n++;
                }
            if (n != seq - lo || n < 8 * 100) FAIL("ring 2: %d lines kept, oldest %d, newest %d: not contiguous", n, lo, seq - 1);
            printf("ring 2 (full): %d archives, lines %d..%d kept (%d), next %d\n", count_arch("JS8L"), lo, seq - 1, n, lg_frames.next);
            /* --- 3: restart.  The position comes from the header line; the newest archive is not the one replaced next */
            int next_before = lg_frames.next, lo_before = lo;
            lg_frames.len = 0; lg_frames.next = 0; lg_frames.full = 0; lg_frames.said = 0; lg_frames.dirty = 0;
            log_load(&lg_frames);
            if (lg_frames.next != next_before || !lg_frames.full) FAIL("ring 3: after reload next %d (was %d), full %d", lg_frames.next, next_before, lg_frames.full);
            for (i = 0; i < 2 * 120; i++) { sprintf(line, "R %06d 0123456789012345678901234567890123456789", seq++); log_add(&lg_frames, line); }
            log_flush(&lg_frames);
            memset(seen, 0, sizeof seen); lo = seq; n = 0;
            for (i = 0; i < NFILES; i++) if (!strncmp(g_f[i].name, "JS8L", 4))
                for (long p = 0; p + 8 < g_f[i].n; p++) if (g_f[i].d[p] == 'R' && (p == 0 || g_f[i].d[p - 1] == '\n')) {
                    int v = atoi((const char *)g_f[i].d + p + 2);
                    if (seen[v]++) FAIL("ring 3: line %d twice", v);
                    if (v < lo) lo = v;
                    n++;
                }
            if (n != seq - lo || lo <= lo_before) FAIL("ring 3: %d lines, oldest %d (before the restart %d), newest %d", n, lo, lo_before, seq - 1);
            printf("ring 3 (restart): lines %d..%d kept (%d), next %d\n", lo, seq - 1, n, lg_frames.next);
        }
        /* --- 4: a stale header (the app was killed after writing an archive, before the log file was rewritten):
         *        while the ring is not full the number that is already taken is skipped, not overwritten */
        wipe_card();
        fput("JS8LOG.TXT", "H JS8L 0003 -\r\nR old line\r\n", 27);
        fput("JS8L0001.TXT", "A1\r\n", 4); fput("JS8L0002.TXT", "A2\r\n", 4); fput("JS8L0003.TXT", "A3 newest\r\n", 11);
        lg_frames.len = 0; lg_frames.next = 0; lg_frames.full = 0; lg_frames.said = 0; lg_frames.dirty = 0;
        log_load(&lg_frames);
        if (lg_frames.next != 3 || lg_frames.len != 12) FAIL("ring 4: header not read (next %d, tail %u B)", lg_frames.next, lg_frames.len);
        for (i = 0; i < 130; i++) { sprintf(line, "R %06d 0123456789012345678901234567890123456789", seq++); log_add(&lg_frames, line); }
        k = ffind("JS8L0003.TXT"); if (k < 0 || g_f[k].n != 11) FAIL("ring 4: JS8L0003.TXT (newest archive) was overwritten");
        if (ffind("JS8L0004.TXT") < 0) FAIL("ring 4: the tail did not go to JS8L0004.TXT");
        /* --- 5: garbage / foreign content in the header position is not taken as a header */
        wipe_card();
        fput("JS8LOG.TXT", "H JS8L 9999 F\r\nH JS8M 0002 -\r\nHello\r\n", 37);
        lg_frames.len = 0; lg_frames.next = 0; lg_frames.full = 0; lg_frames.dirty = 0;
        log_load(&lg_frames);
        if (lg_frames.next != 0 || lg_frames.full) FAIL("ring 5: number 9999 (> keep) accepted: next %d full %d", lg_frames.next, lg_frames.full);
        check_logs();
        printf("fuzz_shell: %s (%ld failures)\n", fails ? "FAIL" : "PASS", fails);
        return fails ? 1 : 0;
    }
    if (!strcmp(argv[1], "frames")) {
        long iters = argc > 3 ? atol(argv[3]) : 100000;
        if (argc > 4) rs ^= (uint64_t)atol(argv[4]) * 0x9E3779B97F4A7C15ull;
        if (argc > 5) g_dictmode = atoi(argv[5]);
        g_faults = 1;
        fput("JS8.CFG", "groups=@ALLCALL,@OK keep=12\n", 28);
        g_init_only = 1; g_clock_ok = 0;                     /* init only; no capture while frames are injected */
        app_main(&svc);
        g_init_only = 0; g_stop = 0; quit = 0;
        if (g_dictmode == 1 && !jsc) FAIL("dictionary not loaded");
        /* canary behind the log tails (the decoder work area is not used in this mode) */
        uint8_t *work = (uint8_t *)lg_msgs.buf + lg_msgs.cap; uint32_t wl = (uint32_t)(heap + heap_len - work);
        memset(work, 0xC3, wl);
        uint32_t mono = 0; int hh = 23, mm = 58, ss = 0;
        for (long i = 0; i < iters; i++) {
            js8_result_t r; memset(&r, 0, sizeof r);
            int freq;
            if (i % 97 == 5 || i % 97 == 6) {                 /* "Q1ABC: OK1BT " + a frame of control characters, to MYCALL */
                if (i % 97 == 5) js8_pack_directed("Q1ABC", "OK1BT", 31, 0, 0, JS8_I3_FIRST, r.frame);
                else ctl_frame(r.frame, JS8_I3_LAST);
                freq = 3333;
            } else if (q_i < q_n && rnd() % 8) {
                memcpy(r.frame, q_fr[q_i++], 11);
                if (rnd() % 30 == 0) r.frame[9] &= 0x1F, js8_msg_crc_set(r.frame);      /* lost flags */
                if (rnd() % 25 == 0) q_i++;                                             /* lost frame */
                freq = q_freq + (int)(rnd() % 5) - 2;
            } else {
                if (q_i >= q_n && rnd() % 3) build_hostile(jsc);
                rand_frame(r.frame); freq = rnd() % 6 ? 400 + 50 * (int)(rnd() % 30) : (int)(rnd() % 12000) - 1000;
            }
            r.freq_hz = (float)freq + (float)(rnd() % 10) * 0.1f;
            r.snr_db = rnd() % 50 ? (float)((int)(rnd() % 60) - 30) : (float)((int)(rnd() % 4000) - 2000);
            r.dt_s = rnd() % 50 ? (float)((int)(rnd() % 500) - 250) * 0.01f : (float)((int)(rnd() % 200000) - 100000);
            now_p.hh = hh; now_p.mm = mm; now_p.ss = ss; now_p.ymd = 20261129u + (hh < 12);
            on_frame(&r, mono);
            check_logs();
            if (rnd() % 5 == 0) {                            /* end of a period */
                mono += 15u * (1u + (rnd() % 20 == 0 ? rnd() % 8 : 0));
                ss += 15; if (ss == 60) { ss = 0; if (++mm == 60) { mm = 0; hh = (hh + 1) % 24; } }
                js8asm_tick(asmb, mono);
                log_flush(&lg_frames); log_flush(&lg_msgs); panel();
                check_logs();
                n_periods++; g_pop_period_open = 0;          /* what app_main does per period */
                if (count_arch("JS8L") > 12 || count_arch("JS8M") > 12) FAIL("more than keep=12 archive files: %d + %d", count_arch("JS8L"), count_arch("JS8M"));
            }
            if (rnd() % 400 == 0) g_full = !g_full && (rnd() & 1);
            if (rnd() % 3000 == 0) g_popbusy = !g_popbusy;
            if (rnd() % 9000 == 0) { n_frames += 99000000u; n_periods += 9900000u; n_msgs += 99000000u; gaps += 99000000u; n_skipped += 990000u; n_tome += 990000u; }
        }
        for (uint32_t i = 0; i < wl; i++) if (work[i] != 0xC3) { FAIL("byte %u behind the log tails was overwritten", i); break; }
        g_full = 0; lg_frames.dirty = lg_msgs.dirty = 1; log_flush(&lg_frames); log_flush(&lg_msgs);
        printf("   archive files on the fake card: %d + %d (keep=12), ring full %d/%d, popups skipped by the rate limit / bad checksum %u\n",
               count_arch("JS8L"), count_arch("JS8M"), lg_frames.full, lg_msgs.full, n_popskip);
        if (iters >= 20000 && !(lg_frames.full && lg_msgs.full && count_arch("JS8L") == 12 && count_arch("JS8M") == 12)) FAIL("the ring did not fill to keep=12");
        check_text_files();
        summary("frames");
    } else {
        long periods = argc > 4 ? atol(argv[4]) : 40;
        if (argc > 5) rs ^= (uint64_t)atol(argv[5]) * 0x9E3779B97F4A7C15ull;
        g_faults = argc > 6 ? atoi(argv[6]) : 0;
        if (strcmp(argv[3], "-")) {
            FILE *f = fopen(argv[3], "rb"); if (!f) { perror(argv[3]); return 2; }
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 44, SEEK_SET);
            g_wn = (sz - 44) / 2; g_wav = malloc((size_t)g_wn * 2 + 2);
            if (fread(g_wav, 2, (size_t)g_wn, f) != (size_t)g_wn) { fprintf(stderr, "short read\n"); return 2; }
            fclose(f);
        }
        if (g_faults & 16) {
            garbage_file("JS8LOG.TXT", 1 + (long)(rnd() % 30000), (int)(rnd() % 3));
            garbage_file("JS8MSG.TXT", 1 + (long)(rnd() % 30000), (int)(rnd() % 3));
            garbage_file("JS8.CFG", 1 + (long)(rnd() % 600), (int)(rnd() % 3));
            if (rnd() & 1) fput("JS8.CFG", "flo=0 fhi=99999999999 call=\x80\x81""abcdefghijklmnopqrstuvwxyz groups=,,,,@a,@b,@ccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc popup=7", 137);
        } else fput("JS8.CFG", "flo=300 fhi=3000\n", 17);
        g_cost = getenv("COST") ? atof(getenv("COST")) : 0.002;   /* virtual ms per service call (the CPU model) */
        g_rtc_off = (86400.0 - 47.0) * 1000.0;               /* 23:59:13: midnight and the tick wrap fall into the run */
        g_end = (double)periods * 15000.0;
        if (getenv("END_MS")) g_end = atof(getenv("END_MS"));
        if (getenv("FAILLOG")) sscanf(getenv("FAILLOG"), "%lf,%lf", &g_faillog0, &g_faillog1);
        if (getenv("STALL")) sscanf(getenv("STALL"), "%lf,%lf", &g_stall_at, &g_stall_ms);
        g_app_mode = 1;
        app_main(&svc);
        summary("app");
        if (sd_omax + sd_rmax + sd_wmax > MAX_SD_GAP) FAIL("%ld SD calls between two stop polls (limit %d)", sd_omax + sd_rmax + sd_wmax, MAX_SD_GAP);
        if (!(g_faults & 16)) check_text_files();
        if (getenv("EXPECT_SKIP")) {                         /* a period whose edge second was never seen must be counted and noted */
            int fi = ffind("JS8LOG.TXT");
            if (n_skipped != (uint32_t)atoi(getenv("EXPECT_SKIP")) || fi < 0 || !memmem(g_f[fi].d, (size_t)g_f[fi].n, "a period was skipped", 20))
                FAIL("skipped periods %u (expected %s), or no note in JS8LOG.TXT", n_skipped, getenv("EXPECT_SKIP"));
        }
        if (!(g_faults & (1 | 8))) {                         /* short reads alone (fault 16) must not look like a read error */
            int fi = ffind("JS8LOG.TXT");
            if (fi >= 0 && g_f[fi].n && memmem(g_f[fi].d, (size_t)g_f[fi].n, "SD read error", 13)) FAIL("'SD read error' note without an SD fault");
        }
        for (int i = 0; i < NFILES; i++) if (g_f[i].name[0] && strncmp(g_f[i].name, "JS8CAP", 6)) printf("   file %-14s %ld B\n", g_f[i].name, g_f[i].n);
        if (!(g_faults & 16) && g_dictmode == 1)             /* the logs must be plain text lines */
            for (int i = 0; i < NFILES; i++) if (!strncmp(g_f[i].name, "JS8L", 4) || !strncmp(g_f[i].name, "JS8M", 4))
                for (long k = 0; k < g_f[i].n; k++) { uint8_t ch = g_f[i].d[k]; if ((ch < 32 && ch != '\r' && ch != '\n') || ch > 126) { FAIL("%s: byte 0x%02x at offset %ld of %ld (not text)", g_f[i].name, ch, k, g_f[i].n); break; } }
    }
    printf("fuzz_shell: %s (%ld failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
