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
 *   - icap_tx_* and icap_net_* are present but only COUNT calls (the summary must show 0), unless the
 *     TX MODEL (-X) / the NET MODEL (-N) is switched on (see below).  Nothing here ever reaches a radio: "transmitted" audio
 *     is captured into a WAV file (-w).
 *
 * TX model (-X, for the JS8_TX flavour of the shell; built from what the SDK README, beacon.h and qso.h
 * say and from the measurements of 23 Nov 2026 - where the real loader is not known the model is a guess
 * and the option list says so):
 *   - slot clock: slots start at period edge + 0.5 s (@CLOCK P=15000 O=500);
 *   - icap_tx_begin(): ICAP_E_PERM unless the owner has armed the beacon (and no owner abort is latched,
 *     the loader's own limits are not used up), ICAP_E_BUSY while the gate is closed or a transmission is
 *     already armed, else the ms to the next slot start;
 *   - icap_tx_write(): refused outside a granted transmission; the first write blocks until the slot
 *     start (a first write more than 300 ms after it: the loader gives up, ICAP_E_BUSY); then the audio
 *     is played at 8000 samples/s through a 250 ms queue (a write blocks while the queue is full; an
 *     empty queue is an UNDERRUN and leaves a hole); more than 15 s of audio is cut (ICAP_E_PERM);
 *   - icap_tx_end(): waits for the queue to drain; without any audio it cancels the armed transmission;
 *   - @TX "beacon= owner= gate= clk= st= next=", @QSO "n= cq= q= t=", @QTX0 / @QTX1 "r=" (one grant per
 *     tap made while nothing was armed; not while the beacon is armed; icap_tx_begin after a grant is
 *     a protocol error and counted), @MYLOC;
 *   - the OWNER is a script (-O "T cmd [arg];..." or -o FILE with one event per line; T = seconds after the
 *     period edge at file sample 0):
 *       arm | disarm | abort (owner abort: ends a transmission, latches owner=1 until the next arm) |
 *       stop (EXIT: icap_should_stop() is 1 from now on, a transmission ends) |
 *       tap TEXT (taps the newest DECODE row that contains TEXT) | taprow N (the N-th newest row) |
 *       tapraw TEXT (a tap that reports TEXT itself - a row the app never printed) |
 *       gate N (TX gate code, 0 = open) | clk 0|1 (clock lock)
 *   - every action of the owner and of the TX model is printed as an "X <ms> ..." line (ms after the period
 *     edge at file sample 0), so that a test can check the app against the owner independently.
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
 *   -K DIR     the SD card ROOT (icap_open with ICAP_O_ROOT, read only); without -K nothing is found there
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
 *   -X FLAGS   switch the TX model on; FLAGS = comma list, "on" alone for the defaults:
 *                noqso      @QSO / @QTX do not exist (ICAP_E_NAME): no taps
 *                tapsarmed  taps are delivered while the beacon is armed (README: they are not - unknown)
 *                once       one transmission per arming: icap_tx_begin answers ICAP_E_PERM after a
 *                           transmission until the owner arms again (the unknown this models: consecutive slots)
 *                cap=N      the loader's own session limit: ICAP_E_PERM after N transmissions
 *                rxgap      no receive audio is delivered for the time of a transmission (default: it keeps flowing)
 *                armed      the beacon is ALREADY armed when the app starts (the loader arms by itself at app start when
 *                           the app's .CFG has beacon=1 - for a file named FT8.APP: a leftover FT8.CFG)
 *                skew=MS    the radio clock (icap_clock_now) is MS ahead of the loader's slot clock
 *                notx       @TX does not exist (old loader)
 *   -O SCRIPT  owner events      -o FILE  owner events, one per line
 *   -g GRID    content of @MYLOC (default empty)
 *   -w FILE    write everything "transmitted" as 8000 Hz mono s16 WAV; sample 0 = the period edge at file sample 0
 *   -L N       play the input file N times in a row (its length should be a multiple of 15 s)
 *   -P N[,M]   POWER CUT at the N-th write call to a file that is not a capture file (JS8CAP*): the process ends
 *              there with exit code 77, no app code runs any more.  What the card then holds of the file that was
 *              being written is NOT KNOWN for the real radio, so four outcomes are modelled (M, default N mod 4):
 *              0 a random part of this write is on the card, 1 the file is empty, 2 the file does not exist,
 *              3 the file still has the content it had before it was opened for writing
 *   -Q BYTES   the card is FULL at BYTES in the app folder (overlay not counted): a write beyond it is cut short;
 *              the owner event "quota BYTES" changes it while running (0 = no limit)
 *   -S SEC     print a status line every SEC virtual seconds ("S ..." : counters, table fills, files and bytes in
 *              the app folder) - for long runs
 * More owner events (with -X; they need no TX flavour):
 *       cmd TEXT (an operator command as the terminal will send it: VIEW [name] | LOG call) | quota BYTES
 *   -X flags:  qlog (the loader's ADIF log exists: "@QLOG <record>" appends to ICAPQSO.ADI), qlogerr (it answers r=-3)
 * NET MODEL (-N, for the JS8_LINK flavours of the shell; without -N every icap_net_* call answers ICAP_E_NONET
 * and is counted - the flavours without the link must show net_calls=0):
 *   -N FLAGS   comma list:
 *                map=HOST:PORT=LPORT  icap_net_open(HOST, PORT) connects a REAL TCP socket to 127.0.0.1:LPORT (several
 *                           allowed).  A host that is not mapped is a DEAD ADDRESS: the call takes its whole timeout
 *                           and answers ICAP_E_CONN.  A mapped port nobody listens on is refused at once.
 *                lat=MS     artificial latency: every icap_net_* call (but net_status) takes MS virtual ms more
 *                pace=F     the accelerated clock is slowed down: one virtual ms that passes in a sleep or a modelled
 *                           wait costs 1/F real ms, so that a peer on a real socket (tests/fake_term.py) answers within
 *                           a few virtual ms (F = 20: a 15 s period takes 0.75 s)
 *                sndbuf=N   SO_SNDBUF of the socket (a small one makes a peer that does not read stall the sender soon)
 *                nonet      icap_net_status() says "no network" (owner event "net 1" brings it up)
 *                gen=SEED[:M] no socket at all: the peer is a HOSTILE-TERMINAL GENERATOR inside the simulator.  Every
 *                           connect succeeds; what the app receives is an endless seeded stream of well-formed and
 *                           malformed protocol lines (all commands with random arguments, random bytes, overlong
 *                           lines, wrong versions, sudden closes).  M = 0 (default): a GO line never carries the right
 *                           nonce (it carries random ones and the nonces of earlier connections) - so not one
 *                           transmission may be requested.  M = 1: the generator reads the hello like a real terminal
 *                           and sometimes sends a correct GO.  Runs at full accelerated speed (megabytes of input).
 *              icap_net_recv with nothing to read takes its timeout and answers ICAP_E_TIMEOUT; icap_net_send into a
 *              full socket takes its timeout and answers ICAP_E_TIMEOUT; a closed peer gives 0 / ICAP_E_CONN; at most
 *              2 sockets.  What the real WLAN module does in these cases is NOT measured - this is the SDK README's
 *              description.  Owner event "net 0|1": the network goes away (all sockets die) / comes back.
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include "icap.h"

static const char *g_dir = "build/sd", *g_overlay, *g_mycall = "", *g_root;   /* g_root: -K, the SD card root */
static int16_t *g_s; static long g_n;                 /* the recording */
static double g_vt;                                   /* virtual ms since app start */
static double g_k = 1.0, g_pollcost; static int g_real, g_verbose, g_popbusy;
static double g_lead = 1.5, g_tail = 80.0, g_off = 0.0;
static long g_clock0;                                 /* fake RTC seconds of day at file sample 0 minus g_off */
static int g_year = 2026, g_mon = 11, g_day = 23;
static long g_ring = 16000;
static struct timespec g_last;
static int g_loops = 1;
static const char *g_myloc = "", *g_txwav;
static int g_stopnow;                                 /* the owner pressed EXIT */
static long g_pcut, g_wcount, g_quota, g_used; static int g_pmode = -1;
static double g_status_s, g_status_next;
static char g_cmdq[16][64]; static int g_cmdn, g_cmdi;   /* operator commands waiting for the app's main loop */
static void sim_status(void);
/* ---- net model state (-N) */
static struct {
    int on, nonet, sndbuf, nmap, fd[2];
    double lat, pace;
    struct { char host[64]; int port, lport; } map[8];
    long n_open, n_open_ok, n_dead, n_send, n_recv, n_close, b_out, b_in, n_over, n_stall;
    double t_max, t_sum;
} N = { .fd = { -1, -1 } };
static struct { int on, mode, pn, pp, fresh; uint32_t s; char nonce[9], old[9]; long bytes, lines, go_right, closes; char pend[700]; } GEN;
static double st_aread_gap, g_last_aread = -1.0;      /* longest time between two icap_audio_read calls */
static const char *sim_cmd(void) { return g_cmdi < g_cmdn ? g_cmdq[g_cmdi++ % 16] : 0; }
#define JS8_HOST_CMD sim_cmd
/* the radio's keyboard (shell/js8app_kbd.h): the app hands its buffer and callback; the owner types with "kbd TEXT" (ENT;
 * "kbd =" keeps the text the keyboard opened with, "kbd" alone sends it empty) or leaves with "kbdexit" (EXIT: no callback),
 * or with "kbdstuck" (EXIT, but the menu id stays 349 - a firmware where the halfword does not follow the EXIT).  "menu N" sets the
 * menu id of the screen under the keyboard (the owner pressed MENU); -X kbdnoid: the menu id never reads 349 (S8, 24 Nov 2026).
 * The menu id the app polls reads 349 while it is open.  The app may close it itself (JS8_HOST_KBDCLOSE: 0x200cd828 on the radio).  A finger stays on the screen for 150 ms after each tap (the app must
 * wait for it to leave before it opens the keyboard: an open under the finger is what crashed the radio in the emulator). */
static char *g_kbd_buf; static unsigned (*g_kbd_cb)(const char *, uint8_t *); static long g_kbd_opens, g_kbd_ent, g_kbd_under_finger, g_kbd_closes;
static int g_kbd_stuck;                               /* "kbdstuck": the keyboard is gone, the menu id still reads 349 */
static long g_mem_in_dec;                             /* JS8MEM.TXT opened while a decode runs (the app must defer it to its main loop) */
static int decoding;                                  /* (the shell's variable, defined with the app below) */
static double g_vt_touch = -1e9;
static double rel(void);
static __attribute__((unused)) void sim_kbd(char *buf, int len, unsigned (*cb)(const char *, uint8_t *))
{
    char t[48]; int n = len < 47 ? len : 47;
    memcpy(t, buf, (size_t)n); while (n && t[n - 1] == ' ') n--; t[n] = 0;
    g_kbd_buf = buf; g_kbd_cb = cb; g_kbd_opens++;
    if (g_vt - g_vt_touch < 150.0) g_kbd_under_finger++;
    printf("X %.0f kbd open \"%s\"\n", rel(), t);
}
static __attribute__((unused)) int sim_touched(void) { return g_vt - g_vt_touch < 150.0; }
static unsigned g_menu;                               /* owner event "menu N": the menu id of the screen under the keyboard (S7) */
static int g_kbdnoid;                                 /* -X kbdnoid: the menu id never shows the keyboard (S8) */
static __attribute__((unused)) unsigned sim_kbdid(void) { return (g_kbd_cb || g_kbd_stuck) && !g_kbdnoid ? 349u : g_menu; }
static __attribute__((unused)) void sim_kbdclose(void)
{
    g_kbd_closes++;
    printf("X %.0f kbd closed by the app (was %s)\n", rel(), g_kbd_cb ? "open" : g_kbd_stuck ? "stuck at 349" : "not open");
    g_kbd_cb = 0; g_kbd_stuck = 0;
}
#define JS8_HOST_KBD sim_kbd
#define JS8_HOST_KBDCLOSE sim_kbdclose
#define JS8_HOST_TOUCHED sim_touched
#define JS8_HOST_KBDID sim_kbdid

/* ---- TX model state (-X) */
static struct {
    int on, noqso, tapsarmed, once, cap, rxgap, notx, qlog; double skew;
    int beacon, owner, gate, clk, cq;
    int ntap, credit; char tap[64];
    int sess, qgrant, spent, count;                   /* sess: 0 none, 1 granted (no audio yet), 2 on the air */
    double t_start, t_first; long nw, pos0;           /* slot start, first sample (virtual ms), samples, capture index of sample 0 */
    int16_t *wav; long wavn;                          /* capture */
    long n_begin, n_begin_ok, n_qtx, n_qtx_ok, n_tx, n_samples, n_wr_rej, n_under, n_late, n_cut, n_empty, n_cancel,
         n_proto, n_after_stop, n_taps, n_taps_lost, n_tap_miss;
} X;
#define NEV 20000
static struct { double t; char cmd[8]; char arg[64]; } g_ev[NEV];
static int g_nev, g_iev;
#define NROW 256
static char g_rows[NROW][72]; static long g_nrow;
static double st_stop_gap, g_last_stop = -1.0;
/* the owner's gestures: taps 310 ms apart (measured on the radio: 300..315), and the two human-like events */
static struct { double t[16], end; int n, i; } TQ;   /* end: the app has ended the gesture (tapgap after its last tap) and painted */
static struct { int on, exit_; double until; char text[64]; } KB;   /* a "kbd" / "kbdexit" waiting for the keyboard to open */
static long n_gest[4];
static double g_tapgap = 100.0;                      /* -G: how long after a tap the owner waits before the next event (ms) */

/* statistics */
static long st_opens, st_open_fail, st_nofd, st_badname, st_maxopen, st_nopen, st_wbytes, st_rbytes;
static long st_overruns, st_audio_calls, st_tx, st_net, st_popups, st_dec, st_dec_long, st_yield;
static double st_max_step;
static long st_win_long;                              /* app-window lines longer than 40 bytes (the panel's width) */
static long st_wfull;                                 /* writes cut short by the full card (-Q) */

static double file_t0(void) { return g_lead * 1000.0; }   /* virtual ms of file sample 0 */
static double rel(void) { return g_vt - file_t0(); }
static void tx_stop_session(const char *why)
{
    if (!X.sess) return;
    if (X.sess == 2 && strcmp(why, "end")) {          /* ended by the owner: what was still in the queue is not played */
        long played = (long)((g_vt - X.t_first) * 8.0);
        if (played < 0) played = 0;
        for (long k = played; k < X.nw; k++) if (X.pos0 + k >= 0 && X.pos0 + k < X.wavn) X.wav[X.pos0 + k] = 0;
        if (played < X.nw) { X.n_samples -= X.nw - played; X.nw = played; }
    }
    if (X.sess == 2) { X.n_tx++; printf("X %.0f txend samples=%ld start=%.1f why=%s\n", rel(), X.nw, X.t_first - file_t0(), why); if (X.once) X.spent = 1; X.count++; }
    else { X.n_cancel++; printf("X %.0f txcancel why=%s\n", rel(), why); }
    X.sess = 0; X.qgrant = 0;
    if (X.rxgap) { extern long g_apos; g_apos = (long)(g_vt * 8.0); }
}
static void deliver(const char *row)                  /* one tap of the owner on a DECODE row */
{
    g_vt_touch = g_vt;                                /* (the finger is on the screen, delivered or not) */
    if (X.noqso || (X.beacon && !X.tapsarmed)) { X.n_taps_lost++; printf("X %.0f owner tap-not-delivered %s\n", rel(), row); return; }
    int n = (int)strlen(row); while (n && row[n - 1] == ' ') n--; if (n > 63) n = 63;
    memmove(X.tap, row, (size_t)n); X.tap[n] = 0;
    X.ntap = (X.ntap + 1) & 0xffff; X.n_taps++;
    if (!X.sess) X.credit = 1;                        /* a tap made while nothing was armed or on the air */
    printf("X %.0f owner tap %s\n", rel(), X.tap);
}
static void gesture(int k)                            /* k taps, 310 ms apart, on the newest row */
{
    k = k < 1 ? 1 : k > 9 ? 9 : k;
    n_gest[k > 3 ? 3 : k - 1]++;
    printf("X %.0f owner gesture %d\n", rel(), k);
    TQ.n = TQ.i = 0;
    for (int j = 0; j < k; j++) TQ.t[TQ.n++] = g_vt + 310.0 * j;
    TQ.end = TQ.t[TQ.n - 1] + g_tapgap + 300.0;
}
static void owner_run(void)                           /* apply every owner event that is due */
{
    while (TQ.i < TQ.n && TQ.t[TQ.i] <= g_vt) { TQ.i++; deliver(g_nrow ? g_rows[(g_nrow - 1) % NROW] : "-"); }
    if (TQ.i >= TQ.n) TQ.n = TQ.i = 0;
    if (KB.on && !TQ.n) {                             /* the owner types on the keyboard once it is open */
        if (g_kbd_cb) {
            unsigned (*cb)(const char *, uint8_t *) = g_kbd_cb;
            KB.on = 0; g_kbd_cb = 0;
            if (KB.exit_) { g_kbd_stuck = KB.exit_ == 2; printf("X %.0f owner kbd EXIT%s\n", rel(), g_kbd_stuck ? " (menu id stays 349)" : ""); }
            else {
                char t[48]; uint8_t act = 0;
                if (!strcmp(KB.text, "=") && g_kbd_buf) { memcpy(t, g_kbd_buf, 43); t[43] = 0; } else snprintf(t, sizeof t, "%-43.43s", KB.text);
                cb(t, &act); g_kbd_ent++;
                printf("X %.0f owner kbd ENT \"%.43s\" action=%d%s\n", rel(), t, act, decoding >= 0 ? " (during a decode)" : "");
            }
            TQ.end = g_vt + 300.0;
        } else if (g_vt > KB.until) { KB.on = 0; X.n_tap_miss++; printf("X %.0f owner kbdmiss (no keyboard open)\n", rel()); }
    }
    if (TQ.n || KB.on || g_vt < TQ.end) return;       /* the owner does one thing after the other: a gesture is over when the app has taken it */
    if (X.sess == 1 && g_vt > X.t_start + 300.0) {    /* armed, but no audio came: the loader gives up */
        X.n_empty++; printf("X %.0f txempty slot=%.1f\n", rel(), X.t_start - file_t0()); X.sess = 0; X.qgrant = 0;
    }
    while (g_iev < g_nev && file_t0() + g_ev[g_iev].t * 1000.0 <= g_vt) {
        const char *c = g_ev[g_iev].cmd, *a = g_ev[g_iev].arg;
        const char *row = 0;
        g_iev++;
        if (!strcmp(c, "arm")) { X.beacon = 1; X.cq = 0; X.owner = 0; X.spent = 0; printf("X %.0f owner arm\n", rel()); }
        else if (!strcmp(c, "cq")) {                  /* the green frame: CQ armed / disarmed (never together with the beacon) */
            int was = X.cq;
            X.cq = atoi(a) != 0;
            if (X.cq) { if (X.beacon) tx_stop_session("disarm"); X.beacon = 0; X.owner = 0; }
            else if (was && X.qgrant == 2) tx_stop_session("cq-off");   /* (qgrant 2: a CQ grant) */
            printf("X %.0f owner cq %d\n", rel(), X.cq);
        }
        else if (!strcmp(c, "g")) { gesture(atoi(a)); return; }
        else if (!strcmp(c, "kbd") || !strcmp(c, "kbdexit") || !strcmp(c, "kbdstuck")) {   /* the radio's keyboard: ENT with a text / EXIT, once it is open (up to 10 s) */
            KB.on = 1; KB.exit_ = c[3] == 'e' ? 1 : c[3] == 's' ? 2 : 0; KB.until = g_vt + 10000.0; snprintf(KB.text, sizeof KB.text, "%s", a);
            owner_run(); return;
        }
        else if (!strcmp(c, "disarm")) { X.beacon = 0; tx_stop_session("disarm"); printf("X %.0f owner disarm\n", rel()); }
        else if (!strcmp(c, "abort")) { X.owner = 1; X.credit = 0; tx_stop_session("abort"); printf("X %.0f owner abort\n", rel()); }
        else if (!strcmp(c, "stop")) { g_stopnow = 1; tx_stop_session("stop"); printf("X %.0f owner stop\n", rel()); }
        else if (!strcmp(c, "menu")) { g_menu = (unsigned)atoi(a); printf("X %.0f owner menu %u\n", rel(), g_menu); }
        else if (!strcmp(c, "gate")) { X.gate = atoi(a); printf("X %.0f owner gate %d\n", rel(), X.gate); }
        else if (!strcmp(c, "clk")) { X.clk = atoi(a); printf("X %.0f owner clk %d\n", rel(), X.clk); }
        else if (!strcmp(c, "cmd")) { snprintf(g_cmdq[g_cmdn++ % 16], 64, "%s", a); printf("X %.0f owner cmd %s\n", rel(), a); TQ.end = g_vt + 300.0; return; }   /* (one at a time: the app takes it in its main loop) */
        else if (!strcmp(c, "quota")) { g_quota = atol(a); printf("X %.0f owner quota %ld (used %ld)\n", rel(), g_quota, g_used); }
        else if (!strcmp(c, "net")) {
            N.nonet = !atoi(a);
            if (N.nonet) for (int i = 0; i < 2; i++) if (N.fd[i] >= 0) { close(N.fd[i]); N.fd[i] = -1; }
            printf("X %.0f owner net %d\n", rel(), !N.nonet);
        }
        else if (!strcmp(c, "tapraw")) row = a;
        else if (!strcmp(c, "taprow")) { long k = g_nrow - 1 - atol(a); if (k >= 0 && k > g_nrow - 1 - NROW) row = g_rows[k % NROW]; else X.n_tap_miss++; }
        else if (!strcmp(c, "tap")) {
            for (long k = g_nrow - 1; k >= 0 && k > g_nrow - 1 - NROW; k--) if (strstr(g_rows[k % NROW], a)) { row = g_rows[k % NROW]; break; }
            if (!row) { X.n_tap_miss++; printf("X %.0f owner tapmiss %s\n", rel(), a); }
        }
        if (row) { deliver(row); TQ.end = g_vt + g_tapgap + 300.0; return; }   /* a single tap is a gesture of one: the next event waits for its end */
    }
}
static void tick(void)
{
    struct timespec t;
    clock_gettime(g_real ? CLOCK_MONOTONIC : CLOCK_THREAD_CPUTIME_ID, &t);
    double d = (double)(t.tv_sec - g_last.tv_sec) * 1000.0 + (double)(t.tv_nsec - g_last.tv_nsec) / 1e6;
    g_last = t;
    d *= g_real ? 1.0 : g_k;
    if (d > st_max_step) st_max_step = d;
    g_vt += d;
    if (X.on) owner_run();
    if (g_status_s > 0 && g_vt >= g_status_next) { g_status_next += g_status_s * 1000.0; sim_status(); }
}

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
    snprintf(g_rows[g_nrow % NROW], sizeof g_rows[0], "%s", s); g_nrow++;
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
static void pace(double ms) { if (N.pace > 0 && !g_real && ms > 0) usleep((useconds_t)(ms * 1000.0 / N.pace)); }
static void s_sleep(uint32_t m) { tick(); if (g_real) usleep(m * 1000); else { g_vt += m; pace(m); } if (X.on) owner_run(); }
static void s_yield(void) { st_yield++; tick(); }
static uint32_t s_tick(void) { tick(); return (uint32_t)g_vt + 100000u; }
static int decoding;                                  /* the shell's own variable (defined below with the app): slot being decoded, -1 none */
static int s_stop(void)
{
    tick();
    if (decoding >= 0) g_vt += g_pollcost;            /* the slow-CPU model charges the decoder's abort polls only */
    if (g_last_stop >= 0 && g_vt - g_last_stop > st_stop_gap) st_stop_gap = g_vt - g_last_stop;
    g_last_stop = g_vt;
    return g_stopnow || g_vt >= file_t0() + (double)g_n * g_loops / 8.0 + g_tail * 1000.0;
}
static int s_clock(struct icap_time *t)
{
    tick();
    double ms = (double)g_clock0 * 1000.0 + (g_vt - file_t0()) + X.skew;
    long sec = (long)(ms / 1000.0); if (ms < 0) sec = (long)((ms - 999.0) / 1000.0);
    int day = g_day;
    while (sec < 0) { sec += 86400; day--; }
    while (sec >= 86400) { sec -= 86400; day++; }
    t->year = (uint16_t)g_year; t->mon = (uint8_t)g_mon; t->day = (uint8_t)day;
    t->hour = (uint8_t)(sec / 3600); t->min = (uint8_t)(sec / 60 % 60); t->sec = (uint8_t)(sec % 60); t->wday = 1;
    return 0;
}

/* ---- audio: a ring that fills in virtual time */
long g_apos = -1;                                     /* next sample to hand out (index from app start) */
static int s_arate(void) { return 8000; }
static int s_aread(int16_t *buf, int n)
{
    tick(); st_audio_calls++;
    if (g_last_aread >= 0 && X.sess != 2 && g_vt - g_last_aread > st_aread_gap) st_aread_gap = g_vt - g_last_aread;
    g_last_aread = g_vt;
    long have = (long)(g_vt * 8.0);
    if (g_apos < 0) g_apos = have;                    /* the first call starts the service */
    long avail = have - g_apos;
    if (avail > g_ring) { g_apos = have; st_overruns++; return ICAP_E_OVERRUN; }
    if (avail <= 0) return 0;
    if (avail > n) avail = n;
    long f0 = (long)(file_t0() * 8.0 + g_off * 8000.0);
    for (long i = 0; i < avail; i++) { long k = g_apos + i - f0; buf[i] = k >= 0 && k < g_n * g_loops ? g_s[k % g_n] : 0; }
    g_apos += avail;
    return (int)avail;
}

/* ---- files */
#define MAXFD 4
static struct { int used, fd, virt, vpos, vlen, wr; char vbuf[160]; char path[1024]; char name[64]; char *old; long oldn; } g_fd[MAXFD];
/* the slot (index k, start = period edge k + 500 ms) that starts next; par >= 0: only slots of that parity */
static double next_slot(int par)
{
    long k = (long)((g_vt - file_t0() - 500.0) / 15000.0);
    for (;; k++) {
        double ts = file_t0() + (double)k * 15000.0 + 500.0;
        if (ts < g_vt) continue;
        if (par >= 0 && (((g_clock0 / 15) + k) & 1) != par) continue;
        return ts;
    }
}
static int name_ok(const char *nm)
{
    int n = 0;
    for (; nm[n]; n++) {
        char c = nm[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return n > 0 && n <= ICAP_NAME_MAX;
}
static long g_root_opens;
static int s_fopen(const char *nm, int mode)
{
    int h = -1;
    tick(); st_opens++;
    if (decoding >= 0 && strstr(nm, "MEM.TXT")) g_mem_in_dec++;
    for (int i = 0; i < MAXFD; i++) if (!g_fd[i].used) { h = i; break; }
    if (nm[0] == '@') {
        char v[160];
        if (!strcmp(nm, "@MYCALL")) snprintf(v, sizeof v, "%s", g_mycall);
        else if (!strcmp(nm, "@CLOCK") && !X.on) snprintf(v, sizeof v, "K=1 S=0 L=1 P=60000 O=1000 N=0");
        else if (!strcmp(nm, "@CLOCK")) snprintf(v, sizeof v, "K=1 S=0 L=%d P=15000 O=500 N=0", X.clk);
        else if (X.on && !strcmp(nm, "@MYLOC")) snprintf(v, sizeof v, "%s", g_myloc);
        else if (X.on && !X.notx && !strcmp(nm, "@TX"))
            snprintf(v, sizeof v, "v=1 beacon=%d owner=%d gate=%d clk=%d st=%d next=%d", X.beacon, X.owner, X.gate, X.clk, X.sess,
                     (int)((next_slot(-1) - g_vt) / 1000.0));
        else if (X.on && !X.noqso && !strcmp(nm, "@QSO")) snprintf(v, sizeof v, "n=%d cq=%d q=%d t=%s", X.ntap, X.cq, X.sess && X.qgrant, X.tap);
        else if (X.on && !X.noqso && !strcmp(nm, "@QTXC")) {   /* the CQ period: granted only while CQ is armed (SDK README) */
            int r;
            X.n_qtx++;
            if (!X.cq || X.beacon || X.owner || X.sess || g_stopnow) r = ICAP_E_PERM;
            else if (X.gate || !X.clk) r = ICAP_E_BUSY;
            else {
                X.n_qtx_ok++;
                X.t_start = next_slot(-1); X.sess = 1; X.qgrant = 2; X.nw = 0;
                r = (int)(X.t_start - g_vt);
                printf("X %.0f qtxc grant slot=%.1f\n", rel(), X.t_start - file_t0());
            }
            snprintf(v, sizeof v, "r=%d", r);
        }
        else if (X.on && !X.noqso && (!strcmp(nm, "@QTX0") || !strcmp(nm, "@QTX1"))) {
            int r;
            X.n_qtx++;
            if (X.beacon || !X.credit || X.owner || X.sess || g_stopnow) r = ICAP_E_PERM;
            else if (X.gate || !X.clk) r = ICAP_E_BUSY;
            else {
                X.credit = 0; X.n_qtx_ok++;
                X.t_start = next_slot(nm[4] - '0'); X.sess = 1; X.qgrant = 1; X.nw = 0;
                r = (int)(X.t_start - g_vt);
                printf("X %.0f qtx grant par=%c slot=%.1f\n", rel(), nm[4], X.t_start - file_t0());
            }
            snprintf(v, sizeof v, "r=%d", r);
        }
        else if (X.on && X.qlog && !strncmp(nm, "@QLOG ", 6)) {       /* the loader's ADIF log: real append, done by the loader */
            int r = 0;
            if (X.qlog == 2 || strlen(nm + 6) < 1 || strlen(nm + 6) > 240) r = -3;
            else {
                char p[1024]; struct stat sb; FILE *f;
                snprintf(p, sizeof p, "%s/ICAPQSO.ADI", g_dir);
                int fresh = stat(p, &sb) != 0 || sb.st_size == 0;
                f = fopen(p, "ab");
                if (!f) r = -3;
                else { if (fresh) fprintf(f, "ICAP QSO log\r\n<ADIF_VER:5>3.1.4 <EOH>\r\n"); fprintf(f, "%s\r\n", nm + 6); fclose(f); }
            }
            snprintf(v, sizeof v, "r=%d", r);
        }
        else { st_open_fail++; return ICAP_E_NAME; }
        if (mode & ICAP_O_WRITE) { st_open_fail++; return ICAP_E_PERM; }
        if (h < 0) { st_nofd++; return ICAP_E_NOFD; }
        g_fd[h].used = 1; g_fd[h].virt = 1; g_fd[h].vpos = 0; g_fd[h].vlen = (int)strlen(v); strcpy(g_fd[h].vbuf, v);
    } else {
        char p[1024];
        if (!name_ok(nm)) { st_badname++; st_open_fail++; return ICAP_E_NAME; }
        if (h < 0) { st_nofd++; return ICAP_E_NOFD; }
        if (mode & ICAP_O_ROOT) {                     /* the SD card root (-K): read-only here, no overlay; without -K nothing is there */
            int fd;
            if ((mode & ICAP_O_WRITE) || !g_root) { st_open_fail++; return ICAP_E_IO; }
            snprintf(p, sizeof p, "%s/%s", g_root, nm);
            if ((fd = open(p, O_RDONLY)) < 0) { st_open_fail++; return ICAP_E_IO; }
            g_fd[h].used = 1; g_fd[h].virt = 0; g_fd[h].fd = fd; g_fd[h].wr = 0; g_root_opens++;
            snprintf(g_fd[h].path, sizeof g_fd[h].path, "%s", p); snprintf(g_fd[h].name, sizeof g_fd[h].name, "%s", nm);
            if (++st_nopen > st_maxopen) st_maxopen = st_nopen;
            return h;
        }
        snprintf(p, sizeof p, "%s/%s", g_dir, nm);
        free(g_fd[h].old); g_fd[h].old = 0; g_fd[h].oldn = -1;
        if (mode & ICAP_O_WRITE) {                    /* what the file held before: for the quota and for power-cut outcome 3 */
            struct stat sb;
            if (stat(p, &sb) == 0) {
                g_used -= sb.st_size;
                if (g_pcut && strncmp(nm, "JS8CAP", 6)) {
                    FILE *f = fopen(p, "rb");
                    g_fd[h].old = malloc((size_t)sb.st_size + 1); g_fd[h].oldn = f ? (long)fread(g_fd[h].old, 1, (size_t)sb.st_size, f) : 0;
                    if (f) fclose(f);
                }
            }
        }
        int fd = open(p, (mode & ICAP_O_WRITE) ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY, 0644);
        if (fd < 0 && !(mode & ICAP_O_WRITE) && g_overlay) { snprintf(p, sizeof p, "%s/%s", g_overlay, nm); fd = open(p, O_RDONLY); }
        if (fd < 0) { st_open_fail++; return ICAP_E_IO; }
        g_fd[h].used = 1; g_fd[h].virt = 0; g_fd[h].fd = fd; g_fd[h].wr = (mode & ICAP_O_WRITE) != 0;
        snprintf(g_fd[h].path, sizeof g_fd[h].path, "%s", p); snprintf(g_fd[h].name, sizeof g_fd[h].name, "%s", nm);
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
    if (strncmp(g_fd[h].name, "JS8CAP", 6) && ++g_wcount == g_pcut) {                /* the power goes during this write */
        int m = g_pmode >= 0 ? g_pmode : (int)(g_pcut % 4);
        long part = (long)((unsigned long)g_pcut * 7919ul % (n + 1u));
        if (m == 0) { if (write(g_fd[h].fd, b, (size_t)part) < 0) part = -1; }
        else if (m == 1) { if (ftruncate(g_fd[h].fd, 0)) part = -1; }
        else if (m == 2) unlink(g_fd[h].path);
        else if (g_fd[h].oldn < 0) unlink(g_fd[h].path);                             /* 3: as it was before - it did not exist */
        else { if (ftruncate(g_fd[h].fd, 0) || pwrite(g_fd[h].fd, g_fd[h].old, (size_t)g_fd[h].oldn, 0) < 0) part = -1; }
        printf("# POWERCUT write=%ld file=%s mode=%d part=%ld of %u vt=%.0f rel=%.0f\n", g_wcount, g_fd[h].name, m, part, n, g_vt, rel());
        fflush(stdout);
        _exit(77);
    }
    if (g_quota > 0 && g_used + (long)n > g_quota) {                                 /* card full: what still fits is written */
        long fit = g_quota - g_used; if (fit < 0) fit = 0;
        if (fit && write(g_fd[h].fd, b, (size_t)fit) == fit) { g_used += fit; st_wbytes += fit; }
        st_wfull++;
        return fit > 0 ? (int)fit : ICAP_E_IO;
    }
    int r = (int)write(g_fd[h].fd, b, n);
    if (r > 0) { st_wbytes += r; g_used += r; }
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

/* ---- transmit: counted; refused unless the TX model is on (the RX-only flavour must never call these) */
static int s_txb(void)
{
    st_tx++; tick();
    if (!X.on) return ICAP_E_PERM;
    X.n_begin++;
    if (g_stopnow) { X.n_after_stop++; return ICAP_E_PERM; }
    if (X.sess) { if (X.qgrant) X.n_proto++; return ICAP_E_BUSY; }
    if (!X.beacon || X.owner || (X.once && X.spent) || (X.cap && X.count >= X.cap)) { printf("X %.0f begin PERM\n", rel()); return ICAP_E_PERM; }
    if (X.gate || !X.clk) { printf("X %.0f begin BUSY\n", rel()); return ICAP_E_BUSY; }
    X.t_start = next_slot(-1); X.sess = 1; X.qgrant = 0; X.nw = 0; X.n_begin_ok++;
    printf("X %.0f begin ok slot=%.1f\n", rel(), X.t_start - file_t0());
    return (int)(X.t_start - g_vt);
}
static int s_txw(const int16_t *b, int n)
{
    st_tx++; tick();
    if (!X.on) return ICAP_E_PERM;
    if (g_stopnow) X.n_after_stop++;
    if (!X.sess || n <= 0) { X.n_wr_rej++; return ICAP_E_PERM; }
    if (X.sess == 1) {                                /* the first audio: wait for the slot */
        double lead = X.t_start - g_vt;               /* how long before the slot start the app handed over its first audio */
        if (g_vt < X.t_start) { pace(X.t_start - g_vt); g_vt = X.t_start; owner_run(); if (!X.sess) { X.n_wr_rej++; return ICAP_E_PERM; } }
        if (g_vt > X.t_start + 300.0) { X.n_late++; printf("X %.0f txlate\n", rel()); X.sess = 0; X.qgrant = 0; return ICAP_E_BUSY; }
        X.t_first = g_vt; X.sess = 2; X.nw = 0; X.pos0 = (long)((g_vt - file_t0()) * 8.0 + 0.5);
        printf("X %.0f txstart slot=%.1f first=%.1f lead=%.1f\n", rel(), X.t_start - file_t0(), X.t_first - file_t0(), lead);
    }
    long played = (long)((g_vt - X.t_first) * 8.0);
    if (played > X.nw + 8) { X.n_under++; X.pos0 += played - X.nw; X.t_first += (double)(played - X.nw) / 8.0; }   /* a hole in the audio */
    if (X.nw + n > 120000) { X.n_cut++; tx_stop_session("cap"); return ICAP_E_PERM; }
    for (int i = 0; i < n; i++) { long k = X.pos0 + X.nw + i; if (k >= 0 && k < X.wavn) X.wav[k] = b[i]; }
    X.nw += n; X.n_samples += n;
    if (X.nw - (long)((g_vt - X.t_first) * 8.0) > 2000) { double to = X.t_first + (double)(X.nw - 2000) / 8.0; pace(to - g_vt); g_vt = to; owner_run(); }
    return n;
}
static void s_txe(void)
{
    st_tx++; tick();
    if (!X.on || !X.sess) return;
    if (X.sess == 2) { double te = X.t_first + (double)X.nw / 8.0; if (g_vt < te) { pace(te - g_vt); g_vt = te; } }
    tx_stop_session("end");
}
/* ---- hostile terminal generator (-N gen=SEED[:M]) */
static uint32_t gr(void) { GEN.s ^= GEN.s << 13; GEN.s ^= GEN.s >> 17; GEN.s ^= GEN.s << 5; return GEN.s; }
static void gen_line(void)
{
    static const char *const tm[] = { "m %q %d %t", "m %q %d %t", "p %q %n", "g %q %N", "g %q %N", "x %q", "v %q %V %n", "v %q %V", "c %q %K", "c %q %K=%v", "l %q %C", "k",
                                      "h 2 gen", "h %n", "g %q", "m %q", "%t", "S f=%n tx=A q=%n", "H JS8 2 %N X", "R %q 0 SEL %n %n %t", "T Q %n %n %t", "D %t" };
    static const char *const words[] = { "Q7AB", "MSG", "HELLO", "SNR?", "SNR -12", "@ALLCALL", "CQ CQ CQ", "HB", "JO70", "73", "NEXT 2/3", "TX> HB", "SEND?", "QRV ON 40M",
                                         "INFO?", "GRID?", "HEARTBEAT", "ACK", "tx=1", "term=1.2.3.4", "-", ":", "9A/OK1BT", "OK1BT", "..", "%s%n%x" };
    static const char *const views[] = { "BAND", "INBOX", "HEARD", "PRESETS", "STATUS", "B", "I", "H", "P", "S", "X", "band", "" };
    static const char *const keys[] = { "popup", "flush", "txf", "info", "status", "call", "loc", "keep", "flo", "fhi", "tx", "txmaxf", "txsess", "txttl", "txstrat", "term", "term2",
                                        "txpath", "txtest", "groups", "", "=", "popup=", "txf=" };
    char *o = GEN.pend; int cap = (int)sizeof GEN.pend - 2, n = 0;
    uint32_t w = gr() % 100;
    GEN.pn = GEN.pp = 0;
    if (GEN.fresh) { GEN.fresh = 0; if (gr() % 10) { GEN.pn = sprintf(o, "h 2 gen\n"); GEN.lines++; return; } }
    if (w < 2) { n = (int)(gr() % 300); for (int i = 0; i < n; i++) o[i] = (char)gr(); }                    /* random bytes */
    else if (w < 4) { n = (int)(gr() % 690); memset(o, ' ' + (int)(gr() % 90), (size_t)n); }                  /* an overlong line (maybe without its LF) */
    else {
        const char *t = tm[gr() % (sizeof tm / sizeof tm[0])];
        /* GO lines and lines that are not commands end a connection after a few (3 wrong nonces, 8 bad lines, a wrong version): keep
         * them rarer, so that most of the stream reaches the command handlers */
        while ((t[0] == 'g' || t[0] < 'a' || (t[0] == 'h' && t[2] == '%')) && gr() % 8) t = tm[gr() % (sizeof tm / sizeof tm[0])];
        for (; *t && n < cap - 130; t++) {
            if (*t != '%') { o[n++] = *t; continue; }
            switch (*++t) {
            case 'q': { int k = 1 + (int)(gr() % 4); if (gr() % 30 == 0) k = (int)(gr() % 9); while (k--) o[n++] = "0123456789abcXYZ._"[gr() % (gr() % 20 ? 16 : 18)]; break; }
            case 'd': n += sprintf(o + n, "%s", gr() % 3 ? words[gr() % (sizeof words / sizeof words[0])] : "-"); break;
            case 't': { int k = (int)(gr() % 9); if (gr() % 12 == 0) k = 30;
                        while (k-- && n < cap - 140) { if (gr() % 6) n += sprintf(o + n, "%s ", words[gr() % (sizeof words / sizeof words[0])]); else o[n++] = (char)(32 + gr() % 95); } break; }
            case 'n': if (gr() % 4) n += sprintf(o + n, "%d", (int)(gr() % 20)); else n += sprintf(o + n, "%d", (int)gr()); break;
            case 'V': n += sprintf(o + n, "%s", views[gr() % (sizeof views / sizeof views[0])]); break;
            case 'K': n += sprintf(o + n, "%s", keys[gr() % (sizeof keys / sizeof keys[0])]); break;
            case 'v': if (gr() % 3) n += sprintf(o + n, "%d", (int)(gr() % 3000)); else n += sprintf(o + n, "%s %d", words[gr() % (sizeof words / sizeof words[0])], (int)gr()); break;
            case 'C': n += sprintf(o + n, "%s", gr() % 2 ? words[gr() % (sizeof words / sizeof words[0])] : "OK1ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"); break;
            case 'N':
                if (GEN.mode == 1 && gr() % 3 == 0) { n += sprintf(o + n, "%s", GEN.nonce); GEN.go_right++; }
                else if (GEN.old[0] && gr() % 3 == 0) n += sprintf(o + n, "%s", GEN.old);                   /* the nonce of the previous connection */
                else { uint32_t v = gr(); char g[9]; sprintf(g, "%08X", v); if (GEN.mode == 0 && !strcmp(g, GEN.nonce)) g[0] ^= 1; n += sprintf(o + n, "%s", g); }
                break;
            default: o[n++] = '%'; break;
            }
        }
        if (gr() % 15 == 0 && n > 0) o[gr() % (uint32_t)n] = (char)gr();                                     /* one byte hit */
    }
    if (gr() % 40) o[n++] = '\n';
    if (gr() % 25 == 0) { o[n - 1] = '\r'; o[n++] = '\n'; }
    GEN.pn = n; GEN.lines++;
}

/* ---- network: counted; "no network" unless the net model is on (-N).  Real sockets to 127.0.0.1 only. */
static void net_cost(double t0, double ms)            /* a net call took ms virtual ms (latency / a timeout) */
{
    if (ms > 0) { if (g_real) usleep((useconds_t)(ms * 1000.0)); else { g_vt += ms; pace(ms); } }
    tick();
    if (g_vt - t0 > N.t_max) N.t_max = g_vt - t0;
    N.t_sum += g_vt - t0;
    if (X.on) owner_run();
}
static int s_nst(void) { st_net++; tick(); return N.on && !N.nonet ? 0 : ICAP_E_NONET; }
static int s_nopen(const char *h, uint32_t p, uint32_t f, uint32_t t)
{
    struct sockaddr_in sa;
    int k, slot = -1, fd, one = 1;
    double t0;
    (void)f; st_net++; tick(); t0 = g_vt;
    if (!N.on || N.nonet) return ICAP_E_NONET;
    N.n_open++;
    if (GEN.on) {
        if (N.fd[0] >= 0) { N.n_over++; return ICAP_E_NOFD; }
        N.fd[0] = 1000; N.n_open_ok++; GEN.fresh = 1; GEN.pn = GEN.pp = 0;
        net_cost(t0, N.lat);
        return 1;
    }
    for (k = 0; k < N.nmap; k++) if (!strcmp(N.map[k].host, h) && N.map[k].port == (int)p) break;
    if (k == N.nmap) { N.n_dead++; printf("X %.0f net open %s:%u dead-address %u ms\n", rel(), h, p, t); net_cost(t0, t + N.lat); return ICAP_E_CONN; }
    for (int i = 0; i < 2; i++) if (N.fd[i] < 0) { slot = i; break; }
    if (slot < 0) { N.n_over++; return ICAP_E_NOFD; }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (N.sndbuf) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &N.sndbuf, sizeof N.sndbuf);
    memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)N.map[k].lport); sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        if (fd >= 0) close(fd);
        printf("X %.0f net open %s:%u refused\n", rel(), h, p); net_cost(t0, N.lat); return ICAP_E_CONN;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    N.fd[slot] = fd; N.n_open_ok++;
    printf("X %.0f net open %s:%u ok\n", rel(), h, p); fflush(stdout);
    net_cost(t0, N.lat);
    return slot + 1;
}
static int s_nio(int h, const void *b, uint32_t n, uint32_t t)
{
    double t0; long r;
    st_net++; tick(); t0 = g_vt; N.n_send++;
    if (!N.on || h < 1 || h > 2 || N.fd[h - 1] < 0) return N.on ? ICAP_E_CONN : ICAP_E_NONET;
    if (GEN.on) {                                     /* the generator "reads" the hello, as a real terminal would */
        const char *p = b;
        for (uint32_t i = 0; i + 16 <= n; i++) if (!memcmp(p + i, "H JS8 2 ", 8)) { memcpy(GEN.old, GEN.nonce, 9); memcpy(GEN.nonce, p + i + 8, 8); GEN.nonce[8] = 0; }
        for (uint32_t i = 0; i < n; i++) if (p[i] != '\n' && (p[i] < 32 || p[i] > 126)) N.n_over += 1000;   /* the app must only ever send printable lines */
        N.b_out += n; net_cost(t0, N.lat);
        return (int)n;
    }
    r = send(N.fd[h - 1], b, n, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { N.n_stall++; net_cost(t0, t + N.lat); return ICAP_E_TIMEOUT; }
    net_cost(t0, N.lat);
    if (r < 0) return ICAP_E_CONN;
    N.b_out += r;
    return (int)r;
}
static int s_nrecv(int h, void *b, uint32_t n, uint32_t t)
{
    double t0; long r;
    st_net++; tick(); t0 = g_vt; N.n_recv++;
    if (!N.on || h < 1 || h > 2 || N.fd[h - 1] < 0) return N.on ? ICAP_E_CONN : ICAP_E_NONET;
    if (GEN.on) {
        uint32_t k, w = gr() % 2000;
        if (w < 500) { net_cost(t0, t + N.lat); return ICAP_E_TIMEOUT; }
        if (w == 500) { GEN.closes++; return 0; }                       /* the peer closes */
        if (w == 501) return ICAP_E_CONN;
        if (GEN.pp >= GEN.pn) gen_line();
        k = (uint32_t)(GEN.pn - GEN.pp); if (k > n) k = n;
        if (gr() % 4 == 0 && k > 1) k = 1 + gr() % k;                   /* any chunking */
        memcpy(b, GEN.pend + GEN.pp, k); GEN.pp += (int)k; GEN.bytes += k; N.b_in += k;
        net_cost(t0, N.lat);
        return (int)k;
    }
    if (g_real) { struct pollfd pf = { N.fd[h - 1], POLLIN, 0 }; poll(&pf, 1, (int)t); }
    r = recv(N.fd[h - 1], b, n, MSG_DONTWAIT);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { net_cost(t0, (g_real ? 0 : t) + N.lat); return ICAP_E_TIMEOUT; }
    net_cost(t0, N.lat);
    if (r < 0) return ICAP_E_CONN;
    N.b_in += r;
    return (int)r;
}
static int s_nclose(int h)
{
    st_net++; N.n_close++;
    if (!N.on) return ICAP_E_NONET;
    if (h >= 1 && h <= 2 && N.fd[h - 1] >= 0) { if (!GEN.on) close(N.fd[h - 1]); N.fd[h - 1] = -1; if (!GEN.on) printf("X %.0f net close\n", rel()); }
    return 0;
}
static int s_nerr(void) { st_net++; return 0; }

static int s_rs(struct icap_radio *r) { memset(r, 0, sizeof *r); r->freq_hz = 7078000; return 0; }
static int s_mem(struct icap_meminfo *m) { memset(m, 0, sizeof *m); return 0; }
static uint32_t s_task(void) { return 1; }
#include <dirent.h>
#include <sys/resource.h>
static void dir_stat(long *files, long *bytes, long cnt[3])   /* the app folder: files, bytes, segment files per stream */
{
    DIR *d = opendir(g_dir); struct dirent *e; struct stat sb; char p[1024];
    *files = *bytes = 0; cnt[0] = cnt[1] = cnt[2] = 0;
    if (!d) return;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
        if (stat(p, &sb)) continue;
        (*files)++; *bytes += sb.st_size;
        if (!strncmp(e->d_name, "JS8L", 4) && e->d_name[4] <= '9') cnt[0]++;
        if (!strncmp(e->d_name, "JS8M", 4) && e->d_name[4] <= '9') cnt[1]++;
        if (!strncmp(e->d_name, "JS8A", 4) && e->d_name[4] <= '9') cnt[2]++;
    }
    closedir(d);
}
static int s_mode(char *b, int max) { if (max >= 4) { strcpy(b, "JS8"); return 3; } return 0; }

/* ---- streaming front end and TX.  Every sample the shell hands to js8_stream_put() (the only way the decoder's FFT can
 * run outside js8_decode()) and every js8_stream_begin() goes through these two wrappers.  What arrives while a
 * transmission is in progress is counted apart and MUST BE 0 (tests/tx_e2e.py, link_e2e.py, sys_e2e.py, fix_e2e.py):
 * "in progress" = the shell is inside tx_slot() (tx_busy), or the loader model holds a session opened by an accepted
 * icap_tx_begin(), or audio is being written for a transmission (X.sess == 2). */
#include "js8dec.h"
static long st_strm, st_strm_tx, st_strm_begin, st_strm_begin_tx;
static int sim_in_tx(void);
static void sim_stream_put(const int16_t *x, uint32_t n) { st_strm += n; if (sim_in_tx()) st_strm_tx += n; js8_stream_put(x, n); }
static int sim_stream_begin(void *work, uint32_t work_len, uint32_t fs, uint32_t max_len, float f_lo, float f_hi, float nom, uint32_t pos)
{
    st_strm_begin++; if (sim_in_tx()) st_strm_begin_tx++;
    return js8_stream_begin(work, work_len, fs, max_len, f_lo, f_hi, nom, pos);
}
#define js8_stream_put sim_stream_put
#define js8_stream_begin sim_stream_begin

/* ---- the app itself: the radio's shell source, unchanged */
#include "../shell/js8app.c"
#undef js8_stream_put
#undef js8_stream_begin
static int sim_in_tx(void)
{
#ifdef JS8_TX
    if (tx_busy) return 1;
#endif
    return X.sess == 2 || (X.sess == 1 && !X.qgrant);
}

static void sim_status(void)
{
    long f, b, c[3]; struct rusage ru; int nh = 0;
    dir_stat(&f, &b, c); getrusage(RUSAGE_SELF, &ru);
    if (feat) for (int i = 0; i < JS8F_HEARD; i++) nh += feat->k.heard[i].call[0] != 0;
    printf("S t=%.0f periods=%u frames=%u msgs=%u to_me=%u heard=%d inbox=%d unread=%d arena_peak=%u files=%ld bytes=%ld L=%ld M=%ld A=%ld segs=%u segfail=%u "
           "snaps=%u log_err=%d skipped=%u popups=%ld tx=%ld rss_kb=%ld\n", g_vt / 1000.0, n_periods, n_frames, n_msgs, n_tome, nh, feat ? feat->k.in_n : 0,
           feat ? feat->k.unread : 0, arena_peak, f, b, c[0], c[1], c[2], n_segs, n_segfail, n_snaps, log_err, n_skipped, st_popups, X.n_tx, ru.ru_maxrss);
    fflush(stdout);
}

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
    while ((o = getopt(argc, argv, "K:d:J:c:t:D:e:l:T:k:q:rR:H:pBX:O:o:g:G:w:L:P:Q:S:N:")) != -1) {
        switch (o) {
        case 'P': g_pcut = atol(optarg); if (strchr(optarg, ',')) g_pmode = atoi(strchr(optarg, ',') + 1); break;
        case 'Q': g_quota = atol(optarg); break;
        case 'S': g_status_s = atof(optarg); break;
        case 'd': g_dir = optarg; break;
        case 'J': g_overlay = optarg; break;
        case 'K': g_root = optarg; break;
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
        case 'g': g_myloc = optarg; break;
        case 'G': g_tapgap = atof(optarg); break;
        case 'w': g_txwav = optarg; break;
        case 'L': g_loops = atoi(optarg) > 0 ? atoi(optarg) : 1; break;
        case 'X': {
            char *sv = 0, *tk, *cp2 = strdup(optarg);
            X.on = 1; X.clk = 1;
            for (tk = strtok_r(cp2, ",", &sv); tk; tk = strtok_r(0, ",", &sv)) {
                if (!strcmp(tk, "noqso")) X.noqso = 1; else if (!strcmp(tk, "tapsarmed")) X.tapsarmed = 1;
                else if (!strcmp(tk, "once")) X.once = 1; else if (!strncmp(tk, "cap=", 4)) X.cap = atoi(tk + 4);
                else if (!strcmp(tk, "rxgap")) X.rxgap = 1; else if (!strncmp(tk, "skew=", 5)) X.skew = atof(tk + 5);
                else if (!strcmp(tk, "notx")) X.notx = 1; else if (!strcmp(tk, "armed")) X.beacon = 1;
                else if (!strcmp(tk, "kbdnoid")) g_kbdnoid = 1;
                else if (!strcmp(tk, "qlog")) X.qlog = 1; else if (!strcmp(tk, "qlogerr")) X.qlog = 2;
                else if (strcmp(tk, "on")) { fprintf(stderr, "unknown -X flag %s\n", tk); return 2; }
            }
            free(cp2); break;
        }
        case 'N': {
            char *sv = 0, *tk, *cp2 = strdup(optarg);
            N.on = 1;
            for (tk = strtok_r(cp2, ",", &sv); tk; tk = strtok_r(0, ",", &sv)) {
                if (!strncmp(tk, "lat=", 4)) N.lat = atof(tk + 4); else if (!strncmp(tk, "pace=", 5)) N.pace = atof(tk + 5);
                else if (!strncmp(tk, "gen=", 4)) { GEN.on = 1; GEN.s = (uint32_t)strtoul(tk + 4, 0, 10) * 2654435761u + 1u; if (strchr(tk, ':')) GEN.mode = atoi(strchr(tk, ':') + 1); }
                else if (!strncmp(tk, "sndbuf=", 7)) N.sndbuf = atoi(tk + 7); else if (!strcmp(tk, "nonet")) N.nonet = 1;
                else if (!strncmp(tk, "map=", 4) && N.nmap < 8) {
                    char *c1 = strchr(tk + 4, ':'), *e1 = strchr(tk + 4, '=');
                    if (!c1 || !e1 || c1 > e1 || c1 - (tk + 4) > 63) { fprintf(stderr, "bad -N map %s\n", tk); return 2; }
                    memcpy(N.map[N.nmap].host, tk + 4, (size_t)(c1 - (tk + 4))); N.map[N.nmap].port = atoi(c1 + 1); N.map[N.nmap].lport = atoi(e1 + 1); N.nmap++;
                }
                else if (strcmp(tk, "on")) { fprintf(stderr, "unknown -N flag %s\n", tk); return 2; }
            }
            free(cp2); break;
        }
        case 'O': case 'o': {
            char *txt = 0, *sv = 0, *ln;
            if (o == 'O') txt = strdup(optarg);
            else { FILE *f = fopen(optarg, "rb"); long z; if (!f) { perror(optarg); return 2; } fseek(f, 0, SEEK_END); z = ftell(f); fseek(f, 0, SEEK_SET);
                   txt = calloc(1, (size_t)z + 1); if (fread(txt, 1, (size_t)z, f) != (size_t)z) return 2; fclose(f); }
            for (ln = strtok_r(txt, ";\n", &sv); ln && g_nev < NEV; ln = strtok_r(0, ";\n", &sv)) {
                char cmd[8] = "", arg[64] = ""; double tt; int used = 0;
                if (sscanf(ln, " %lf %7s %n", &tt, cmd, &used) < 2) continue;
                if (!used) used = (int)strlen(ln);
                snprintf(arg, sizeof arg, "%s", ln + used);
                g_ev[g_nev].t = tt; strcpy(g_ev[g_nev].cmd, cmd); strcpy(g_ev[g_nev].arg, arg); g_nev++;
            }
            free(txt); break;
        }
        default: fprintf(stderr, "see the header of tests/js8sim.c\n"); return 2;
        }
    }
    if (optind >= argc || load(argv[optind])) { fprintf(stderr, "usage: js8sim [options] FILE.wav|.pcm\n"); return 2; }
    { long f, c[3]; dir_stat(&f, &g_used, c); }
    g_status_next = g_status_s * 1000.0;
    if (ss % 15) { fprintf(stderr, "-t must be a period edge (seconds 00/15/30/45)\n"); return 2; }
    g_clock0 = hh * 3600L + mi * 60L + ss;
    for (int i = 1; i < g_nev; i++) if (g_ev[i].t < g_ev[i - 1].t) { fprintf(stderr, "owner events must be in time order\n"); return 2; }
    if (X.on) { X.wavn = (long)(((double)g_n * g_loops / 8000.0 + g_tail + 20.0) * 8000.0); X.wav = calloc((size_t)X.wavn, 2); }
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

    if (X.on) owner_run();
    if (X.sess) { X.n_proto++; tx_stop_session("app-exit-with-tx-open"); }
    if (X.on && g_txwav) {
        FILE *f = fopen(g_txwav, "wb");
        uint32_t dl = (uint32_t)X.wavn * 2u, h[11] = { 0x46464952u, 36u + dl, 0x45564157u, 0x20746d66u, 16u, 0x00010001u, 8000u, 16000u, 0x00100002u, 0x61746164u, dl };
        if (!f || fwrite(h, 4, 11, f) != 11 || fwrite(X.wav, 2, (size_t)X.wavn, f) != (size_t)X.wavn) { perror(g_txwav); return 2; }
        fclose(f);
    }
    for (int i = 0; i < 8; i++) printf("W%d %s\n", i, g_panel[i]);
    printf("# sim rc=%d file=%s samples=%ld (%.1f s) virtual=%.1f s wall=%.2f s k=%g q=%g%s\n", rc, argv[optind], g_n, (double)g_n / 8000.0,
           g_vt / 1000.0, (double)(w1.tv_sec - w0.tv_sec) + (double)(w1.tv_nsec - w0.tv_nsec) / 1e9, g_k, g_pollcost, g_real ? " realtime" : "");
    printf("# files opens=%ld failed=%ld max_open=%ld still_open=%ld nofd=%ld badname=%ld read=%ld B written=%ld B root_opens=%ld\n",
           st_opens, st_open_fail, st_maxopen, st_nopen, st_nofd, st_badname, st_rbytes, st_wbytes, g_root_opens);
    printf("# audio calls=%ld overruns=%ld app_gaps=%u yields=%ld max_step_ms=%.2f\n", st_audio_calls, st_overruns, gaps, st_yield, st_max_step);
    printf("# decode_out lines=%ld over40=%ld popups=%ld tx_calls=%ld net_calls=%ld win_over40=%ld\n", st_dec, st_dec_long, st_popups, st_tx, st_net, st_win_long);
    printf("# app periods=%u frames=%u msgs=%u to_me=%u skipped=%u notes=%d raw=%u popup_busy=%u log_err=%d\n",
           n_periods, n_frames, n_msgs, n_tome, n_skipped, n_notes, n_raw, n_popbusy, log_err);
    printf("# decode ms last=%u avg=%u max=%u (virtual) arena peak=%u of %u (heap %u, shell %u)\n", dec_ms_last,
           n_periods ? dec_ms_sum / n_periods : 0, dec_ms_max, arena_peak, arena_len, heap, heap - arena_len);
    printf("# stop poll_gap_max_ms=%.0f\n", st_stop_gap);
    printf("# stream samples=%ld begins=%ld in_tx_samples=%ld in_tx_begins=%ld\n", st_strm, st_strm_begin, st_strm_tx, st_strm_begin_tx);
    printf("# writes other_than_capture=%ld cut_short_by_full_card=%ld\n", g_wcount, st_wfull);
    printf("# audioread max_gap_ms=%.0f\n", st_aread_gap);
    if (N.on) printf("# net opens=%ld open_ok=%ld dead_address=%ld sends=%ld recvs=%ld closes=%ld bytes_out=%ld bytes_in=%ld send_stalls=%ld over_two_sockets=%ld "
                     "max_call_ms=%.1f total_ms=%.0f lat=%g pace=%g\n", N.n_open, N.n_open_ok, N.n_dead, N.n_send, N.n_recv, N.n_close, N.b_out, N.b_in, N.n_stall, N.n_over,
                     N.t_max, N.t_sum, N.lat, N.pace);
    if (GEN.on) printf("# gen mode=%d bytes=%ld lines=%ld go_with_right_nonce=%ld closes=%ld\n", GEN.mode, GEN.bytes, GEN.lines, GEN.go_right, GEN.closes);
#ifdef JS8_LINK
    if (lnk) printf("# link conn=%u open_fail=%u lost=%u bad=%u nonce_bad=%u cmds=%u dropped=%u lines=%u state=%d bytes=%u\n", lnk->n_conn, lnk->n_open_fail, lnk->n_lost, lnk->n_bad,
                    lnk->n_nonce, lnk->n_cmd, lnk->n_drop, lnk->n_lines, lnk->state, (unsigned)sizeof(js8link_t));
#endif
    if (X.on) {
        printf("# txmodel begin=%ld begin_ok=%ld qtx=%ld qtx_ok=%ld transmissions=%ld samples=%ld cancelled=%ld write_rejected=%ld underruns=%ld late=%ld cut=%ld "
               "empty_keyed=%ld protocol_errors=%ld calls_after_stop=%ld\n", X.n_begin, X.n_begin_ok, X.n_qtx, X.n_qtx_ok, X.n_tx, X.n_samples, X.n_cancel,
               X.n_wr_rej, X.n_under, X.n_late, X.n_cut, X.n_empty, X.n_proto, X.n_after_stop);
        printf("# kbd opens=%ld ent=%ld open_at_end=%d closed_by_app=%ld mem_in_decode=%ld\n", g_kbd_opens, g_kbd_ent, g_kbd_cb != 0 || g_kbd_stuck, g_kbd_closes, g_mem_in_dec);
        printf("# owner events=%d done=%d taps=%ld taps_not_delivered=%ld tap_miss=%ld gestures=%ld/%ld/%ld/%ld kbd_under_finger=%ld kbd_opens=%ld\n", g_nev, g_iev, X.n_taps,
               X.n_taps_lost, X.n_tap_miss, n_gest[0], n_gest[1], n_gest[2], n_gest[3], g_kbd_under_finger, g_kbd_opens);
    }
#ifdef JS8_TX
    printf("# txapp slots=%u begin=%u frames_ok=%u frames_cut=%u refused=%u aborts=%u session=%u queued=%u dead=%u\n", n_txslots, txq.n_begin, txq.n_frames_ok,
           txq.n_frames_cut, txq.n_refused, txq.n_aborts, (unsigned)txq.sess_frames, (unsigned)txq.qn, (unsigned)txq.dead);
#endif
    if (jsc) printf("# dict head_words=%u head_bytes=%u opens=%u bytes=%u max_lookup=%u cache_hits=%u head_hits=%u\n", jsc->head_n, jsc->head_bytes,
                    jsc->st_opens, jsc->st_bytes, jsc->st_max_bytes, jsc->st_cache_hits, jsc->st_head_hits);
    else printf("# dict none\n");
    if (jsc) printf("# dictsum sums=%d checked=%u bad=%u\n", jsc->sums, jsc->st_checked, jsc->st_bad);
    if (feat) {
        int nh = 0, np = 0; uint32_t dirty = 0; long f, b, c[3];
        for (int i = 0; i < JS8F_HEARD; i++) nh += feat->k.heard[i].call[0] != 0;
        for (int i = 0; i < JS8F_PREP; i++) np += feat->prep[i][0] != 0;
        for (uint32_t i = 0; i < spare_n; i++) dirty += spare_p[i] != 0;
        dir_stat(&f, &b, c);
        printf("# feat heard=%d inbox=%d unread=%d view=%d prepared=%d popskip=%u spare=%u spare_dirty=%u feat_bytes=%u\n", nh, feat->k.in_n, feat->k.unread, feat->k.view, np,
               n_popskip, spare_n, dirty, (unsigned)sizeof(js8feat_t));
        printf("# store segs=%u segfail=%u snaps=%u snap_found=%d gen=%u wfull=%ld lnext=%d lfull=%d lseq=%u mnext=%d mfull=%d mseq=%u anext=%d aseq=%u dirfiles=%ld dirbytes=%ld lfiles=%ld mfiles=%ld afiles=%ld\n",
               n_segs, n_segfail, n_snaps, snap_found, snap_gen, st_wfull, lg_frames.next, lg_frames.full, lg_frames.seq, lg_msgs.next, lg_msgs.full, lg_msgs.seq,
               lg_adif.next, lg_adif.seq, f, b, c[0], c[1], c[2]);
    }
    return rc;
}
