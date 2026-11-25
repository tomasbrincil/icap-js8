/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * fuzz_shell - adversarial host test of the radio shell (shell/js8app.c, included unchanged with
 * -DDIGI_HOST) against a fake loader that CHECKS the radio limits and injects faults.
 * Build with -fsanitize=address,undefined.
 *
 *   fuzz_shell frames SD_DIR [iterations] [seed] [dictmode]
 *       the result path only: random / hostile frames straight into on_frame() (DECODE lines, logs,
 *       reassembly, popups, log segments, heard table / inbox, operator commands from hostile row texts) with SD write
 *       faults.  dictmode: 0 none, 1 intact, 2 corrupt WITHOUT JSCSUM.BIN (the corrupt words reach the
 *       text path, as before the checksums existed), 3 corrupt with JSCSUM.BIN (the checksum must stop them)
 *   fuzz_shell app SD_DIR FILE.wav|- [periods] [seed] [faults]
 *       the whole app_main on a virtual clock: audio = the recording looped on period edges (or noise
 *       for "-"), the RTC starts shortly before midnight; faults (bit mask): 1 SD write errors and
 *       full card, 2 RTC steps and RTC not set, 4 audio overruns / stalls, 8 open failures,
 *       16 corrupted state files JS8STA.BIN / JS8STB.BIN, a garbage segment file and JS8.CFG at start.
 *       Environment COST = virtual ms charged
 *       per service call (default 0.002; 0.02 and more makes every decode run out of time); END_MS = stop
 *       at this virtual time; FAILLOG=from,to = writes to the frame log (JS8L*) fail in this window of virtual ms;
 *       STALL=at,ms = one sleep call that returns `ms` late at virtual time `at`; EXPECT_SKIP=n = the app must
 *       count exactly n skipped periods and write the note
 *
 *   fuzz_shell ring SD_DIR
 *       the segment ring alone, through log_add / log_flush / snap_save / snap_load of the shell: holes are never
 *       overwritten, at most keep= segments exist, after the ring is full the oldest is replaced, a restart
 *       continues where the state file says, and - state file one step old, missing or garbage - where the
 *       segment files themselves say
 *   fuzz_shell mkwav OUT.wav NSIG
 *       60 s recording for "app": NSIG stations 45 Hz apart from 480 Hz, each sending in every period a
 *       different compressed frame of three rare dictionary words (three SD bucket scans per frame)
 *
 * Checked by the fake loader: DECODE line <= 40 printable ASCII characters, app-window line <= 40
 * printable bytes and row 0..7, file names (A-Z 0-9 _ - . up to 48), at most 4 files open, handles
 * balanced at exit, no write to a read handle, popup text printable and at most one popup request per
 * period, SD calls between two icap_should_stop() polls (app mode: at most MAX_SD_GAP), every segment file
 * plain text with header, known line types and (unless a write fault cut it) a trailer with the right line
 * count and CRC, number of segment files <= keep, the spare end of the shell's data region untouched.
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

#define MAX_SD_GAP 40                            /* SD calls allowed between two stop polls: two segments + the state file */
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
    /* the start-up loads pump audio too (n = 800); the app's own "start the capture service" call has n = 1.  Stopping
     * at the first pump used to cut the dictionary load short, which the checksums now (rightly) refuse. */
    if (g_init_only) { if (n == 1) g_stop = 1; return 0; }
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
static uint64_t g_seed3 = 0x9E3779B97F4A7C15ull; static char g_badname[600][16]; static int g_nbad;   /* dictmode 3: the damaged files that were opened */
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
            else if (!strncmp(nm, "JSC", 3) && g_dictmode && !(g_dictmode == 2 && !strcmp(nm, "JSCSUM.BIN"))) {
                char p[1024]; snprintf(p, sizeof p, "%s/%s", sd_dir, nm);
                FILE *f = fopen(p, "rb");
                if (!f) { st_openfail++; return ICAP_E_IO; }
                fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
                g_h[h].d = malloc((size_t)n + 1);
                if (fread(g_h[h].d, 1, (size_t)n, f) != (size_t)n) n = 0;
                fclose(f);
                if (g_dictmode == 3) {                       /* damage that stays the same at every open, in bucket files only: the
                                                              * checksum must catch each damaged file at its first use */
                    uint64_t save = rs; long n0 = n; uint8_t *o = malloc((size_t)n + 1); memcpy(o, g_h[h].d, (size_t)n);
                    rs = g_seed3; for (const char *q = nm; *q; q++) rs = (rs ^ (uint8_t)*q) * 0x100000001B3ull; rs |= 1; rnd(); rnd();
                    if ((nm[3] == 'D' || nm[3] == 'E') && nm[4] != 'S' && n > 16) switch (rnd() % 5) {
                        case 1: n = 9 + (long)(rnd() % (uint32_t)(n - 9)); break;
                        case 2: for (int k = 1 + 2 * (int)(rnd() % 8); k--; ) g_h[h].d[rnd() % (uint32_t)n] ^= (uint8_t)(1u << (rnd() & 7)); break;
                        case 3: for (long k = 8; k < n; k++) g_h[h].d[k] = (uint8_t)rnd(); break;
                        case 4: g_h[h].d[8 + rnd() % (uint32_t)(n - 8)] ^= 0x20; break;                  /* one letter changes case */
                        default: break;
                    }
                    rs = save;
                    if (n != n0 || memcmp(o, g_h[h].d, (size_t)n)) {
                        int k; for (k = 0; k < g_nbad && strcmp(g_badname[k], nm); k++) ;
                        if (k == g_nbad && g_nbad < 600) snprintf(g_badname[g_nbad++], 16, "%s", nm);
                    }
                    free(o);
                } else
                if (g_dictmode >= 2) switch (rnd() % 6) {
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
    if (g_faillog1 > g_faillog0 && g_vt >= g_faillog0 && g_vt < g_faillog1 && g_h[h].fi >= 0 && !strncmp(g_f[g_h[h].fi].name, "JS8L", 4)) { st_wfail++; return ICAP_E_IO; }
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

static const char *fz_cmd(void) { return 0; }   /* the app's main loop gets no command here; "frames" calls ui_cmd itself */
#define JS8_HOST_CMD fz_cmd
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

/* every segment file on the fake card must be plain text lines (CR LF, printable ASCII) whatever was received:
 * the header line "H <stream> <8 digits> <EOH>", lines of the stream's kinds and - unless a write fault cut the
 * file short - the trailer "E <lines> <crc>" with the right line count and the right CRC */
static long seg_good, seg_cut;
static void check_text_files(void)
{
    for (int i = 0; i < NFILES; i++) {
        const char *nm = g_f[i].name;
        if (strncmp(nm, "JS8L", 4) && strncmp(nm, "JS8M", 4) && strncmp(nm, "JS8A", 4)) continue;
        if (nm[4] < '0' || nm[4] > '9') { FAIL("unexpected file %s", nm); continue; }
        const uint8_t *d = g_f[i].d; long n = g_f[i].n, k, ls = 0, lines = 0, last = -1;
        const char *kinds = nm[3] == 'L' ? "RNT" : nm[3] == 'M' ? "M" : "<";
        int bad = 0;
        for (k = 0; k < n; k++) { uint8_t ch = d[k]; if ((ch < 32 && ch != '\r' && ch != '\n') || ch > 126) { FAIL("%s: byte 0x%02x at offset %ld of %ld (not text)", nm, ch, k, n); bad = 1; break; } }
        if (n >= 23 && (memcmp(d, "H ", 2) || memcmp(d + 2, nm, 4) || d[6] != ' ' || memcmp(d + 15, " <EOH>\r\n", 8))) { FAIL("%s does not start with the segment header", nm); bad = 1; }
        for (k = 0; k < n && !bad; k++) {
            if (d[k] == '\r' && k + 1 < n && d[k + 1] != '\n') { FAIL("%s: bare CR at %ld", nm, k); bad = 1; }
            if (d[k] == '\n' && (k == 0 || d[k - 1] != '\r')) { FAIL("%s: bare LF at %ld", nm, k); bad = 1; }
            if (d[k] == '\n') {
                char c0 = (char)d[ls];
                if (ls == 0) { if (c0 != 'H') { FAIL("%s: first line is not the header", nm); bad = 1; } }
                else if (c0 == 'E' && k - ls == 12) last = ls;
                else { lines++; if (!strchr(kinds, c0) || (c0 != '<' && d[ls + 1] != ' ')) { FAIL("%s: line at %ld starts with \"%.12s\"", nm, ls, d + ls); bad = 1; } }
                ls = k + 1;
            }
        }
        if (bad) continue;
        if (last >= 0 && ls == n) {                          /* a complete segment: line count and CRC */
            unsigned crc = 0; int cnt = atoi((const char *)d + last + 2);
            sscanf((const char *)d + last + 7, "%4x", &crc);
            if (cnt != lines || crc != js8_crc16(0xFFFFu, d, (uint32_t)last)) FAIL("%s: trailer says %d lines crc %04x, the file has %ld lines crc %04x", nm, cnt, crc, lines, js8_crc16(0xFFFFu, d, (uint32_t)last));
            seg_good++;
        } else {
            seg_cut++;
            if (!st_wfail && !st_openfail) FAIL("%s has no trailer although no write failed (%ld B)", nm, n);
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
    for (uint32_t i = 0; i < spare_n; i++) if (spare_p[i]) { FAIL("byte %u of the spare end of the shell's data region was written", i); break; }
}
/* ring test helpers: the R lines "R <number> ..." found in the segment files (+ the lines still waiting in RAM) */
static char have_[200000];
static int ring_scan(int seq, int with_ram, int *lo, int *dups)
{
    int cnt = 0; *lo = seq; *dups = 0;
    memset(have_, 0, (size_t)seq + 1);
    for (int k = 0; k < NFILES; k++) if (!strncmp(g_f[k].name, "JS8L0", 5) || !strncmp(g_f[k].name, "JS8L1", 5))
        for (long p = 0; p + 8 < g_f[k].n; p++) if (g_f[k].d[p] == 'R' && (p == 0 || g_f[k].d[p - 1] == '\n')) { int v = atoi((const char *)g_f[k].d + p + 2); if (have_[v]++) (*dups)++; else cnt++; if (v < *lo) *lo = v; }
    if (with_ram) for (uint32_t p = 0; p + 8 < lg_frames.len; p++) if (lg_frames.buf[p] == 'R' && (p == 0 || lg_frames.buf[p - 1] == '\n')) { int v = atoi(lg_frames.buf + p + 2); if (have_[v]++) (*dups)++; else cnt++; if (v < *lo) *lo = v; }
    return cnt;
}
static void ring_reset(void)                                 /* what a restart forgets */
{
    lg_frames.len = 0; lg_frames.lines = 0; lg_frames.next = 0; lg_frames.full = 0; lg_frames.said = 0; lg_frames.dirty = 0; lg_frames.seq = 0;
    lg_frames.wrapped = 0; lg_frames.failed = 0; lg_frames.wait = 0; snap_found = 0; snap_gen = 0;
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
        char line[80]; int seq = 0, i, k, lo, dups, n;
        g_init_only = 1; g_clock_ok = 0; g_dictmode = 0;
        /* --- 1: holes.  JS8L0002 and JS8L0005 exist (the owner deleted 1, 3, 4): they are never overwritten */
        fput("JS8.CFG", "keep=8\n", 7);
        fput("JS8L0002.TXT", "KEEP ME 2\r\n", 11); fput("JS8L0005.TXT", "KEEP ME 5\r\n", 11);
        app_main(&svc);                                      /* init only */
        g_init_only = 0; g_stop = 0; quit = 0;
        /* (the start-up wrote its "no dictionary" note as segment 1 and the state files when it left: start again clean) */
        for (k = 0; k < NFILES; k++) if (!strcmp(g_f[k].name, "JS8L0001.TXT") || !strcmp(g_f[k].name, F_STA) || !strcmp(g_f[k].name, F_STB)) g_f[k].name[0] = 0;
        ring_reset(); leaving = 0;
        const int per = (int)(lg_frames.cap / 51u);          /* lines of 49 characters + CR LF per full segment */
        for (i = 0; i < 4 * per + 10; i++) { sprintf(line, "R %06d 0123456789012345678901234567890123456789", seq++); log_add(&lg_frames, line); }
        k = ffind("JS8L0002.TXT"); if (k < 0 || g_f[k].n != 11 || memcmp(g_f[k].d, "KEEP ME 2", 9)) FAIL("ring 1: JS8L0002.TXT was overwritten");
        k = ffind("JS8L0005.TXT"); if (k < 0 || g_f[k].n != 11 || memcmp(g_f[k].d, "KEEP ME 5", 9)) FAIL("ring 1: JS8L0005.TXT was overwritten");
        if (ffind("JS8L0001.TXT") < 0 || ffind("JS8L0003.TXT") < 0 || ffind("JS8L0004.TXT") < 0 || ffind("JS8L0006.TXT") < 0 || count_arch("JS8L") != 6 || lg_frames.full)
            FAIL("ring 1: expected new segments 1, 3, 4, 6 next to the old 2, 5 (have %d, full %d)", count_arch("JS8L"), lg_frames.full);
        printf("ring 1 (holes): %d segments (%d lines each), next %d, full %d\n", count_arch("JS8L"), per, lg_frames.next, lg_frames.full);
        /* --- 2: the ring fills (keep=8, two foreign files inside it), then the oldest is replaced and a note is due */
        for (i = 0; i < 12 * per; i++) {
            sprintf(line, "R %06d 0123456789012345678901234567890123456789", seq++); log_add(&lg_frames, line);
            if (rnd() % 61 == 0) log_flush(&lg_frames);      /* (segments of every size, as the age / urgent flushes make them) */
            /* at EVERY step: the segments + the lines waiting in RAM hold a gap-free run of lines up to the newest one
             * (the first overwrite must hit the oldest segment, never the newest) */
            n = ring_scan(seq, 1, &lo, &dups);
            if (n != seq - lo || dups) { FAIL("ring 2: after line %d the kept lines %d..%d have a gap (%d of %d, %d twice)", seq - 1, lo, seq - 1, n, seq - lo, dups); break; }
        }
        log_flush(&lg_frames);
        if (count_arch("JS8L") != 8 || !lg_frames.full || !lg_frames.said || !pending_note) FAIL("ring 2: %d segments, full %d, said %d", count_arch("JS8L"), lg_frames.full, lg_frames.said);
        n = ring_scan(seq, 0, &lo, &dups);
        if (n != seq - lo || dups || n < 6 * per / 2) FAIL("ring 2: %d lines kept, oldest %d, newest %d: not contiguous", n, lo, seq - 1);
        printf("ring 2 (full): %d segments, lines %d..%d kept (%d), next %d, seq %u\n", count_arch("JS8L"), lo, seq - 1, n, lg_frames.next, lg_frames.seq);
        /* --- 3: restart.  The position comes from the state file; the newest segment is not the one replaced next */
        for (int pass = 0; pass < 3; pass++) {
            int next_before = lg_frames.next, lo_before = lo; uint32_t seq_before = lg_frames.seq;
            snap_dirty = 1; snap_save(); snap_dirty = 1; snap_save();   /* (both files of the pair exist) */
            if (pass == 1) {                                 /* 3b: no state file at all - found from the sequence numbers */
                int a = ffind(F_STA), b2 = ffind(F_STB);
                if (a < 0 || b2 < 0) FAIL("ring 3b: the state file pair is not on the card"); else { g_f[a].name[0] = 0; g_f[b2].name[0] = 0; }
            }
            if (pass == 2) {                                 /* 3c: the newer state file is garbage - the older one + the segments */
                int nf = ffind((snap_gen & 1u) ? F_STB : F_STA);
                if (nf < 0) FAIL("ring 3c: newest state file missing"); else for (k = 0; k < g_f[nf].n; k += 7) g_f[nf].d[k] ^= 0x55;
            }
            ring_reset();
            snap_load(lg_frames.buf, JS8_LOG_SZ);
            if (pass == 1) { if (snap_found) FAIL("ring 3b: a state was found although both files are gone"); }
            else if (!snap_found) FAIL("ring 3%c: no valid state file found", 'a' + pass);
            sprintf(line, "R %06d 0123456789012345678901234567890123456789", seq++); log_add(&lg_frames, line); log_flush(&lg_frames);
            if (lg_frames.next != next_before % 8 + 1 || !lg_frames.full || lg_frames.seq != seq_before + 1u)
                FAIL("ring 3%c: after the restart the segment went to %d (expected %d), full %d, seq %u (expected %u)", 'a' + pass, (lg_frames.next + 6) % 8 + 1, next_before, lg_frames.full, lg_frames.seq, seq_before + 1u);
            for (i = 0; i < 2 * per; i++) { sprintf(line, "R %06d 0123456789012345678901234567890123456789", seq++); log_add(&lg_frames, line); }
            log_flush(&lg_frames);
            n = ring_scan(seq, 0, &lo, &dups);
            if (n != seq - lo || dups || lo <= lo_before) FAIL("ring 3%c: %d lines, oldest %d (before the restart %d), newest %d, %d twice", 'a' + pass, n, lo, lo_before, seq - 1, dups);
            printf("ring 3%c (restart, %s): lines %d..%d kept (%d), next %d, seq %u\n", 'a' + pass,
                   pass == 0 ? "state file" : pass == 1 ? "no state file" : "newest state file garbage", lo, seq - 1, n, lg_frames.next, lg_frames.seq);
        }
        /* --- 4: a state that is one step old (the power went after a segment was written, before its state file):
         *        the newer segment is recognised by its sequence number and not overwritten - ring full and not full */
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) { wipe_card(); ring_reset(); cfg_keep = 8; for (i = 0; i < 2 * per + 5; i++) { sprintf(line, "R %06d 0123456789012345678901234567890123456789", seq++); log_add(&lg_frames, line); } log_flush(&lg_frames); }
            snap_dirty = 1; snap_save();
            int newest = lg_frames.next; uint32_t sq = lg_frames.seq; char nm[16];
            sprintf(line, "R %06d 0123456789012345678901234567890123456789", seq++); log_add(&lg_frames, line); log_flush(&lg_frames);   /* -> file `newest`, no state written */
            seg_name(&lg_frames, newest, nm);
            k = ffind(nm); long sz = k >= 0 ? g_f[k].n : -1;
            ring_reset();
            snap_load(lg_frames.buf, JS8_LOG_SZ);
            if (!snap_found || lg_frames.next != newest % 8 + 1 || lg_frames.seq != sq + 1u) FAIL("ring 4%c: stale state not corrected: next %d (expected %d), seq %u (expected %u)", 'a' + pass, lg_frames.next, newest % 8 + 1, lg_frames.seq, sq + 1u);
            sprintf(line, "R %06d 0123456789012345678901234567890123456789", seq++); log_add(&lg_frames, line); log_flush(&lg_frames);
            k = ffind(nm); if (k < 0 || g_f[k].n != sz) FAIL("ring 4%c: %s (the newest segment) was overwritten", 'a' + pass, nm);
            n = ring_scan(seq, 0, &lo, &dups);
            if (n != seq - lo || dups) FAIL("ring 4%c: lines %d..%d: %d kept, %d twice", 'a' + pass, lo, seq - 1, n, dups);
            printf("ring 4%c (state one step old, ring %s): newest segment %s kept, next %d\n", 'a' + pass, pass ? "not full" : "full", nm, lg_frames.next);
        }
        /* --- 5: garbage / foreign state files are not taken; a position beyond keep= (keep was lowered) is probed again */
        wipe_card();
        fput(F_STA, "J8S2 this is not a state file at all, just some text of the right kind of start", 80);
        { uint8_t g[1192]; for (i = 0; i < (int)sizeof g; i++) g[i] = (uint8_t)rnd(); memcpy(g, "J8S2", 4); fput(F_STB, g, (long)(SNAP_HDR + sizeof(js8f_keep_t))); }
        ring_reset();
        snap_load(lg_frames.buf, JS8_LOG_SZ);
        if (snap_found || lg_frames.next != 0 || lg_frames.full) FAIL("ring 5: garbage state accepted: found %d next %d full %d", snap_found, lg_frames.next, lg_frames.full);
        cfg_keep = 8; lg_frames.next = 7; lg_frames.full = 1; lg_frames.seq = 99; snap_dirty = 1; snap_save();
        cfg_keep = 4; ring_reset();
        snap_load(lg_frames.buf, JS8_LOG_SZ);
        if (!snap_found || lg_frames.next != 0 || lg_frames.full) FAIL("ring 5: position 7 accepted with keep=4: next %d full %d", lg_frames.next, lg_frames.full);
        printf("ring 5 (garbage state, keep lowered): ignored / probed again\n");
        /* --- 6: a card that refuses writes: the lines stay in RAM (the older half is dropped when it overflows), nothing
         *        is lost once the card works again, and a file our own failed write left behind is taken again */
        wipe_card(); ring_reset(); cfg_keep = 8; log_err = 0;
        g_full = 1;
        for (i = 0; i < per / 2; i++) { sprintf(line, "R %06d 0123456789012345678901234567890123456789", seq++); log_add(&lg_frames, line); }
        int first_kept = seq - per / 2;
        for (i = 0; i < 20; i++) log_flush(&lg_frames);
        if (!log_err || !n_segfail || lg_frames.len == 0) FAIL("ring 6: failed writes not noticed (log_err %d, segfail %u, waiting %u B)", log_err, n_segfail, lg_frames.len);
        g_full = 0; log_flush(&lg_frames);
        n = ring_scan(seq, 0, &lo, &dups);
        if (n != per / 2 || lo != first_kept || dups || count_arch("JS8L") > 2) FAIL("ring 6: after the card works again %d lines from %d in %d files (expected %d from %d)", n, lo, count_arch("JS8L"), per / 2, first_kept);
        printf("ring 6 (card refuses writes, then works): %d lines kept in %d file(s), failed writes %u\n", n, count_arch("JS8L"), n_segfail);
        st_wfail = 0; st_openfail = 0;
        check_logs();
        printf("fuzz_shell: %s (%ld failures)\n", fails ? "FAIL" : "PASS", fails);
        return fails ? 1 : 0;
    }
    if (!strcmp(argv[1], "frames")) {
        long iters = argc > 3 ? atol(argv[3]) : 100000;
        if (argc > 4) rs ^= (uint64_t)atol(argv[4]) * 0x9E3779B97F4A7C15ull;
        if (argc > 5) g_dictmode = atoi(argv[5]);
        g_seed3 ^= rs;
        g_faults = 1;
        fput("JS8.CFG", "groups=@ALLCALL,@OK keep=12\n", 28);
        g_init_only = 1; g_clock_ok = 0;                     /* init only; no capture while frames are injected */
        app_main(&svc);
        g_init_only = 0; g_stop = 0; quit = 0; leaving = 0;
        if ((g_dictmode == 1 || g_dictmode == 3) && !jsc) FAIL("dictionary not loaded");
        /* canary behind the shell's part of the data region (the decoder work area is not used in this mode) */
        uint8_t *work = heap + JS8_RESERVE; uint32_t wl = (uint32_t)(heap + heap_len - work);
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
            if (rnd() % 40 == 0) {                           /* the owner taps a row: one of ours, a decode, or anything */
                static const char *rows[] = { "V> VIEW", "V> VIEW INBOX", "V> VIEW HEARD", "V> VIEW BAND", "V> VIEW STATUS", "V> VIEW X", "V> LOG Q1ABC", "V> LOG",
                                              "V> LOG @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@", "V> ", "V>", "", "2358 > Q1ABC: OK1BT MSG X", "2358 ? Q1ABC: OK1BT MSG X", "     Q1ABC: X",
                                              "2358 DL/Q2ABC/P: @ALLCALL CQ", "2358 WWWWWWWWWWWWWWWWWWWWWWWWWWWWW:", "2358 TX 1500 1/3 X: Y", "TX> HB", "SEND? X", "V> LOG DL/Q2ABC/P" };
                char row[64]; int rn = 0;
                if (rnd() % 4) snprintf(row, sizeof row, "%s", rows[rnd() % (sizeof rows / sizeof rows[0])]);
                else { rn = (int)(rnd() % 64); for (int j = 0; j < rn; j++) row[j] = (char)(32 + rnd() % 95); row[rn] = 0; }
                /* (since the keyboard control of 24 Nov 2026 a tap does nothing in the RX build; the views and the log are operator
                 * commands of the app window / the terminal: the same hostile texts go to the command parser) */
                ui_cmd(row[0] == 'V' && row[1] == '>' && row[2] == ' ' ? row + 3 : row);
                check_logs();
            }
            if (rnd() % 5 == 0) {                            /* end of a period */
                mono += 15u * (1u + (rnd() % 20 == 0 ? rnd() % 8 : 0));
                ss += 15; if (ss == 60) { ss = 0; if (++mm == 60) { mm = 0; hh = (hh + 1) % 24; } }
                js8asm_tick(asmb, mono); js8f_tick(feat, p_abs());
                log_tick(&lg_frames, 1); log_tick(&lg_msgs, 1); snap_save(); panel();
                check_logs();
                n_periods++; g_pop_period_open = 0;          /* what app_main does per period */
                pop_tok = POP_BURST;                         /* (the burst limit is tested in feat_e2e; here every period may raise its popup, so that
                                                              * the popup TEXT of hostile messages keeps being exercised as before the limit existed) */
                if (count_arch("JS8L") > 12 || count_arch("JS8M") > 12 || count_arch("JS8A") > 12) FAIL("more than keep=12 segment files: %d + %d + %d", count_arch("JS8L"), count_arch("JS8M"), count_arch("JS8A"));
            }
            if (rnd() % 400 == 0) g_full = !g_full && (rnd() & 1);
            if (rnd() % 3000 == 0) g_popbusy = !g_popbusy;
            if (rnd() % 9000 == 0) { g_pop_period_open = 0;   /* (the counters jump by millions of periods: the app's "one popup per period" starts anew) */
                n_frames += 99000000u; n_periods += 9900000u; n_msgs += 99000000u; gaps += 99000000u; n_skipped += 990000u; n_tome += 990000u; }
        }
        for (uint32_t i = 0; i < wl; i++) if (work[i] != 0xC3) { FAIL("byte %u behind the shell's data region was overwritten", i); break; }
        g_full = 0; log_flush(&lg_frames); log_flush(&lg_msgs); snap_dirty = 1; snap_save();
        printf("   segment files on the fake card: %d + %d + %d ADIF (keep=12), ring full %d/%d, popups skipped by the limits / bad checksum %u\n",
               count_arch("JS8L"), count_arch("JS8M"), count_arch("JS8A"), lg_frames.full, lg_msgs.full, n_popskip);
        if (iters >= 20000 && !(lg_frames.full && lg_msgs.full && count_arch("JS8L") == 12 && count_arch("JS8M") == 12)) FAIL("the ring did not fill to keep=12");
        check_text_files();
        {   /* the state file pair: both there, the newer one valid and equal to what is in RAM */
            int a = ffind(F_STA), b2 = ffind(F_STB), nf = (snap_gen & 1u) ? b2 : a;
            if (a < 0 || b2 < 0 || nf < 0 || g_f[nf].n != (long)(SNAP_HDR + sizeof(js8f_keep_t)) || memcmp(g_f[nf].d + SNAP_HDR, &feat->k, sizeof feat->k))
                FAIL("state files: A %ld B, B %ld B, newest does not hold the current state", a < 0 ? -1 : g_f[a].n, b2 < 0 ? -1 : g_f[b2].n);
            int nh = 0; for (int j = 0; j < JS8F_HEARD; j++) nh += feat->k.heard[j].call[0] != 0;
            printf("   segments complete %ld, cut by a write fault %ld; segments written %u, failed %u; state files written %u; heard %d, inbox %d (unread %d), dictionary files bad %u\n",
                   seg_good, seg_cut, n_segs, n_segfail, n_snaps, nh, feat->k.in_n, feat->k.unread, jsc ? jsc->st_bad : 0);
            if (g_dictmode == 3 && jsc) {                    /* every damaged dictionary file that was opened is marked corrupt, none passed as good */
                int passed = 0;
                for (int j = 0; j < g_nbad; j++) { unsigned no = (unsigned)strtoul(g_badname[j] + 4, 0, 16), fk = g_badname[j][3] == 'D' ? 1u + no : 2u + 256u + no; int st = (jsc->fst[fk >> 2] >> ((fk & 3u) * 2u)) & 3; if (st != 2) { passed++; FAIL("dictmode 3: damaged %s was not rejected (state %d)", g_badname[j], st); } }
                printf("   dictmode 3: %d damaged dictionary files opened, %u rejected by their checksum, %d passed; good files checked %u\n", g_nbad, jsc->st_bad, passed, jsc->st_checked);
                if (!jsc->sums || !g_nbad || jsc->st_bad != (unsigned)g_nbad) FAIL("dictmode 3: sums %d, damaged files opened %d, rejected %u", jsc->sums, g_nbad, jsc->st_bad);
            }
        }
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
            garbage_file(F_STA, rnd() & 1 ? (long)(SNAP_HDR + sizeof(js8f_keep_t)) : 1 + (long)(rnd() % 3000), (int)(rnd() % 3));
            garbage_file(F_STB, rnd() & 1 ? (long)(SNAP_HDR + sizeof(js8f_keep_t)) : 1 + (long)(rnd() % 3000), (int)(rnd() % 3));
            garbage_file("JS8L0001.TXT", 1 + (long)(rnd() % 30000), (int)(rnd() % 3));
            garbage_file("JS8M0001.TXT", 1 + (long)(rnd() % 30000), (int)(rnd() % 3));
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
            int found = 0;
            for (int i = 0; i < NFILES; i++) if (!strncmp(g_f[i].name, "JS8L", 4) && g_f[i].n && memmem(g_f[i].d, (size_t)g_f[i].n, "a period was skipped", 20)) found = 1;
            if (n_skipped != (uint32_t)atoi(getenv("EXPECT_SKIP")) || !found)
                FAIL("skipped periods %u (expected %s), or no note in the frame log", n_skipped, getenv("EXPECT_SKIP"));
        }
        if (!(g_faults & (1 | 8))) {                         /* short reads alone (fault 16) must not look like a read error */
            for (int i = 0; i < NFILES; i++)
                if (!strncmp(g_f[i].name, "JS8L", 4) && !((g_faults & 16) && !strcmp(g_f[i].name, "JS8L0001.TXT")) && g_f[i].n && memmem(g_f[i].d, (size_t)g_f[i].n, "SD read error", 13))
                    FAIL("'SD read error' note without an SD fault");
        }
        for (int i = 0; i < NFILES; i++) if (g_f[i].name[0] && strncmp(g_f[i].name, "JS8CAP", 6)) printf("   file %-14s %ld B\n", g_f[i].name, g_f[i].n);
        printf("   segments complete %ld, cut by a write fault %ld; written %u, failed %u; state files %u (found at start %d)\n", seg_good, seg_cut, n_segs, n_segfail, n_snaps, snap_found);
        for (uint32_t i = 0; i < spare_n; i++) if (spare_p[i]) { FAIL("byte %u of the spare end of the shell's data region was written", i); break; }
        if (!(g_faults & 16) && g_dictmode == 1)             /* the logs must be plain text lines */
            for (int i = 0; i < NFILES; i++) if (!strncmp(g_f[i].name, "JS8L", 4) || !strncmp(g_f[i].name, "JS8M", 4))
                for (long k = 0; k < g_f[i].n; k++) { uint8_t ch = g_f[i].d[k]; if ((ch < 32 && ch != '\r' && ch != '\n') || ch > 126) { FAIL("%s: byte 0x%02x at offset %ld of %ld (not text)", g_f[i].name, ch, k, g_f[i].n); break; } }
    }
    printf("fuzz_shell: %s (%ld failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
