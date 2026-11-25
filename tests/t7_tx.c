/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * T7: unit tests of the transmit engine js8tx.c on the PC.  Nothing transmits: the "loader" is a mock in
 * this file with its own clock (slots at k * 15 s + 0.5 s; a write takes n / 8000 s).
 *   t7_tx vectors FILE   every frame of FILE ("text i3 crc hex22 tones79", written by the independent
 *                        Python model tests/model.py) -> js8tx_wave -> the AUDIO the engine would write
 *                        -> 8-tone detection per symbol -> must equal the model's 79 tones; also the
 *                        sample count (101120), peak and the first/last samples
 *   t7_tx unit           the safety rules, both consecutive-slot strategies, every loader error, the selection (SEND?
 *                        screen) and its timing, CQ by the green frame, the review findings of 23 Nov 2026
 * Exit code 1 if a check fails.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../src/js8tx.h"
#include "../src/js8_constants.h"

static int fails, checks;
#define CHECK(cond, ...) do { int ok_ = (cond); checks++; printf("%s  ", ok_ ? "PASS" : "FAIL"); \
    printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

/* ---- the mock loader */
static struct {
    uint32_t now, rem; int armed, owner, gate, clk, stopped, noqso, notx, once, spent, begin_err, clock_bad, cq, vfail, qtxc, qtxc_ok, credit, qtx, qtx_ok;
    int sess; uint32_t t_start; long this_n, fail_at, stop_at, cqoff_at;
    int begins, begins_ok, ends, writes; long samples; int ntx; uint32_t starts[64]; long lens[64];
    int tapn; char tap[64];
    char said[400][72]; int nsaid; char notes[200][72]; int nnotes; char logs[64][140]; int nlogs;
    long writes_after_stop;
} M;
static int16_t scratch[JS8TX_CHUNK];
static uint32_t m_ticks(void) { return M.now; }
static void m_wait(uint32_t ms) { M.now += ms; }
static int m_stop(void) { return M.stopped; }
static int m_begin(void)
{
    M.begins++;
    if (M.begin_err) return M.begin_err;
    if (!M.armed || M.owner || (M.once && M.spent)) return JS8TX_L_PERM;
    if (M.gate || !M.clk) return JS8TX_L_BUSY;
    M.sess = 1; M.this_n = 0; M.begins_ok++;
    M.t_start = (M.now + 14499u) / 15000u * 15000u + 500u; if (M.t_start < M.now) M.t_start += 15000u;
    if (M.t_start >= M.now + 15000u) M.t_start -= 15000u;
    return (int)(M.t_start - M.now);
}
static int m_write(const int16_t *b, int n)
{
    (void)b;
    if (M.stopped) M.writes_after_stop++;
    if (!M.sess) return JS8TX_L_PERM;
    if (M.now < M.t_start) M.now = M.t_start;
    if (!M.this_n && M.ntx < 64) M.starts[M.ntx] = M.now;
    if (M.fail_at >= 0 && M.this_n >= M.fail_at) { M.sess = 0; M.owner = 1; if (M.ntx < 64) M.lens[M.ntx++] = M.this_n; return JS8TX_L_PERM; }
    M.this_n += n; M.samples += n; M.writes++;
    if (M.stop_at >= 0 && M.this_n > M.stop_at) M.stopped = 1;
    if (M.cqoff_at >= 0 && M.this_n > M.cqoff_at) M.cq = 0;      /* the owner disarms CQ while the frame is on the air */
    M.rem += (uint32_t)n; M.now += M.rem / 8u; M.rem %= 8u;
    return n;
}
static void m_end(void) { M.ends++; if (M.sess && M.this_n) { if (M.ntx < 64) M.lens[M.ntx++] = M.this_n; M.spent = 1; } M.sess = 0; }
static int m_vfile(const char *name, char *b, int cap)
{
    if (!strcmp(name, "@TX")) { if (M.notx) return JS8TX_L_NAME; if (M.vfail > 0) { M.vfail--; return -2; }   /* -2: a transient read failure */
                                return snprintf(b, (size_t)cap, "v=1 beacon=%d owner=%d gate=%d clk=%d st=0 next=3", M.armed, M.owner, M.gate, M.clk); }
    if (!strcmp(name, "@QSO")) { if (M.noqso) return JS8TX_L_NAME; return snprintf(b, (size_t)cap, "n=%d cq=%d q=0 t=%s", M.tapn, M.cq, M.tap); }
    if (!strncmp(name, "@QTX", 4) && (name[4] == '0' || name[4] == '1')) {   /* the tap credit (SDK QSO mode): one per tap, not while armed */
        int r;
        M.qtx++;
        if (M.noqso || !M.credit || M.armed || M.owner || M.sess) r = JS8TX_L_PERM;
        else if (M.gate || !M.clk) r = JS8TX_L_BUSY;
        else {
            M.credit = 0; M.sess = 1; M.this_n = 0; M.qtx_ok++;
            M.t_start = (M.now + 14499u) / 15000u * 15000u + 500u; if (M.t_start < M.now + 1000u) M.t_start += 15000u;
            r = (int)(M.t_start - M.now);
        }
        return snprintf(b, (size_t)cap, "r=%d", r);
    }
    if (!strcmp(name, "@QTXC")) {                /* the CQ period: only while CQ is armed, never with the beacon, one at a time */
        int r;
        M.qtxc++;
        if (M.noqso || !M.cq || M.armed || M.owner || M.sess) r = JS8TX_L_PERM;
        else if (M.gate || !M.clk) r = JS8TX_L_BUSY;
        else {
            M.sess = 1; M.this_n = 0; M.qtxc_ok++;
            M.t_start = (M.now + 14499u) / 15000u * 15000u + 500u; if (M.t_start < M.now) M.t_start += 15000u;
            r = (int)(M.t_start - M.now);
        }
        return snprintf(b, (size_t)cap, "r=%d", r);
    }
    return JS8TX_L_NAME;
}
static int m_cycle(void) { return M.clock_bad ? -1 : (int)(M.now % 30000u); }
static void m_say(const char *row, int stamp) { (void)stamp; if (strlen(row) > (stamp ? 56u : 40u)) printf("FAIL  row too long (limit 40; a stamped \"TX <f0> k/n <text>\" row is cut to 40 by the shell and goes whole to the terminal: 56): %s\n", row), fails++; if (M.nsaid < 400) snprintf(M.said[M.nsaid++], 72, "%s", row); }
static void m_note(const char *l) { if (strlen(l) > 40) printf("NOTE  (longer than the 40-byte window, cut there) %s\n", l); if (M.nnotes < 200) snprintf(M.notes[M.nnotes++], 72, "%s", l); }
static void m_log(const char *l) { if (M.nlogs < 64) snprintf(M.logs[M.nlogs++], 140, "%s", l); }
static const js8tx_io_t io = { m_begin, m_write, m_end, m_ticks, m_stop, m_wait, m_vfile, m_cycle, m_say, m_note, m_log, 0 };

static js8tx_t T;
static void fresh(const char *call, const char *grid)
{
    memset(&M, 0, sizeof M); M.clk = 1; M.fail_at = -1; M.stop_at = -1; M.cqoff_at = -1; M.now = 1000;
    js8tx_init(&T, &io, call, 0, scratch);
    js8tx_cfg(&T, "tx=1");                       /* TX is off by default (rule 7) */
    if (grid) js8tx_set_grid(&T, grid);
}
static void run_ms(uint32_t ms)                  /* the app's main loop for ms of mock time */
{
    uint32_t end = M.now + ms;
    while ((int32_t)(end - M.now) > 0) {
        int d = js8tx_poll(&T);
        if (d == 0) { uint32_t b = M.now; js8tx_run(&T); if (M.now == b) M.now += 1; }
        else M.now += 10;
    }
}
static int said(const char *sub) { for (int i = M.nsaid - 1; i >= 0; i--) if (strstr(M.said[i], sub)) return 1; return 0; }
static int noted(const char *sub) { for (int i = M.nnotes - 1; i >= 0; i--) if (strstr(M.notes[i], sub)) return 1; return 0; }
static int slot_ok(int i) { return M.starts[i] % 15000u == 500u; }

static char l80[80];
static int unit(void)
{
    static const char *L3 = "Q7AB MSG HELLO THERE HOW ARE YOU";     /* directed + Huffman data frames (no dictionary) */
    int n3, r;

    /* U1 armed, empty queue */
    fresh("OK1BT", "JO70"); M.armed = 1; run_ms(600000);
    CHECK(M.begins == 0 && M.samples == 0, "U1 armed for 10 min with an empty queue: tx_begin calls %d, samples %ld", M.begins, M.samples);
    /* U2 request, not armed; then armed */
    fresh("OK1BT", "JO70"); r = js8tx_request(&T, "HB JO70"); run_ms(60000);
    CHECK(r == 0 && M.begins == 0 && M.samples == 0, "U2 human request but NOT armed, 60 s: request rc %d, tx_begin calls %d, samples %ld", r, M.begins, M.samples);
    M.armed = 1; run_ms(40000);
    CHECK(M.ntx == 1 && M.lens[0] == 101120 && slot_ok(0) && M.begins == 1 && T.qn == 0, "U2 then armed: 1 frame of %ld samples at slot+%u ms, tx_begin calls %d", M.lens[0], M.starts[0] % 15000u, M.begins);
    CHECK(said("SENT: HB JO70") && said("red frame off to tap again") && !said("TX done"),
          "U2 the message went out whole: rows \"SENT: HB JO70\" and (still armed, taps are not delivered) \"red frame off to tap again\"");
    run_ms(600000);
    CHECK(M.ntx == 1 && M.begins == 1, "U2 nothing is repeated afterwards (10 min armed): transmissions %d, tx_begin calls %d", M.ntx, M.begins);
    /* U3 strategy A */
    fresh("OK1BT", "JO70"); M.armed = 1; r = js8tx_request(&T, L3); n3 = T.qn ? T.q[0].n : 0; run_ms(120000);
    CHECK(r == 0 && n3 >= 3 && M.ntx == n3 && M.begins == n3, "U3 strategy A: %d-frame message -> %d transmissions, %d tx_begin calls", n3, M.ntx, M.begins);
    { int ns = 0, nr = 0, last = -1; for (int i = 0; i < M.nsaid; i++) { if (!strncmp(M.said[i], "SENT: ", 6)) { ns++; last = i; } if (!strcmp(M.said[i], "red frame off to tap again")) nr++; }
      CHECK(ns == 1 && nr == 1 && last >= 0 && !strcmp(M.said[last], "SENT: Q7AB MSG HELLO THERE HOW ARE YOU") && last + 1 < M.nsaid && !strcmp(M.said[last + 1], "red frame off to tap again"),
            "U3 ONE row \"SENT: <text>\" (the 40 characters of a row: \"%s\") after the LAST part only, then \"red frame off to tap again\"", last >= 0 ? M.said[last] : "-"); }
    fresh("OK1BT", "JO70"); M.armed = 1; r = js8tx_request(&T, "Q7AB MSG HELLO THERE HOW ARE YOUR FRIEND 73"); run_ms(120000);
    CHECK(r == 0 && M.ntx >= 3 && !strcmp(M.said[M.nsaid - 2], "SENT: Q7AB MSG HELLO THERE HOW ARE YOUR"), "U3 a text of 43 characters: \"SENT: \" + the text cut to the 40 characters of a row, no trailing blank (\"%s\")", M.nsaid > 1 ? M.said[M.nsaid - 2] : "-");
    { int ok = 1; for (int i = 0; i < M.ntx; i++) if (!slot_ok(i) || M.lens[i] != 101120 || (i && M.starts[i] - M.starts[i - 1] != 15000u)) ok = 0;
      CHECK(ok, "U3 strategy A: consecutive slots, each 101120 samples (starts %u %u %u ...)", M.starts[0], M.starts[1], M.starts[2]); }
    /* U4 strategy B (since the review of 24 Nov 2026: a disarm drops the held rest - confirm, arm, disarm = nothing further) */
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txstrat=B"); M.armed = 1; js8tx_request(&T, L3); run_ms(120000);
    CHECK(M.ntx == 1 && M.begins == 1 && T.qn == 1 && T.q[0].hold == 1 && noted("TX B: held, red frame off drops 2/"), "U4 strategy B: one frame per arming, then held (\"%s\"), tx_begin calls %d in 2 min", M.notes[M.nnotes - 1], M.begins);
    M.armed = 0; run_ms(3000);
    CHECK(T.qn == 0 && noted("disarmed, waiting messages dropped"), "U4 strategy B: the disarm drops the held frames 2..%d (queued %d)", n3, T.qn);
    M.armed = 1; run_ms(60000);
    for (int i = 2; i < n3; i++) { M.armed = 0; run_ms(3000); M.armed = 1; run_ms(40000); }
    CHECK(M.ntx == 1 && M.begins == 1, "U4 strategy B: %d more armings -> nothing more (transmissions %d, tx_begin calls %d)", n3 - 1, M.ntx, M.begins);
    /* U5 strategy A against a loader that refuses consecutive slots */
    fresh("OK1BT", "JO70"); M.armed = 1; M.once = 1; js8tx_request(&T, L3); run_ms(110000);
    CHECK(M.ntx == 1 && M.begins == 2 && T.q[0].hold == 1 && noted("TX refused: part 2/") && noted(" - red frame again") && said("TX refused: part 2/"), "U5 strategy A, loader refuses the next slot (PERM): held, tx_begin calls %d (1 ok + 1 refused, no retry storm)", M.begins);
    M.armed = 0; run_ms(3000); M.spent = 0; M.armed = 1; run_ms(20000);
    CHECK(M.ntx == 1 && T.qn == 0, "U5 (S1 24 Nov 2026) the disarm drops the held message: nothing more at a new arming (transmissions %d)", M.ntx);
    /* U6 BUSY */
    fresh("OK1BT", "JO70"); M.armed = 1; M.gate = 7; js8tx_request(&T, "HB JO70"); run_ms(300000);
    CHECK(M.begins == JS8TX_BUSY_TRIES && M.samples == 0 && T.qn == 0 && noted("gave up") && noted("DATA MOD"), "U6 loader BUSY (gate 7): %d tx_begin calls, one per slot, then dropped; samples %ld", M.begins, M.samples);
    fresh("OK1BT", "JO70"); M.armed = 1; M.clk = 0; js8tx_request(&T, "HB JO70"); run_ms(20000); M.clk = 1; run_ms(40000);
    CHECK(M.ntx == 1 && noted("set the radio clock"), "U6 BUSY (clock not locked) once, then ready: sent on a later slot (tx_begin calls %d)", M.begins);
    /* U7 write failure / owner abort mid-frame */
    fresh("OK1BT", "JO70"); M.armed = 1; M.fail_at = 40000; js8tx_request(&T, L3); js8tx_request(&T, "HB JO70"); run_ms(60000);
    CHECK(M.ntx == 1 && M.lens[0] == 40000 && T.qn == 0 && noted("owner abort"), "U7 owner abort in frame 1 at sample 40000: %ld samples out, queue cleared (2 messages)", M.lens[0]);
    M.fail_at = -1; M.owner = 0; M.armed = 0; run_ms(3000); M.armed = 1; run_ms(120000);
    CHECK(M.ntx == 1 && M.begins == 1, "U7 re-armed afterwards: nothing more is sent (transmissions %d, tx_begin calls %d)", M.ntx, M.begins);
    /* U8 stop */
    fresh("OK1BT", "JO70"); M.armed = 1; js8tx_request(&T, L3); run_ms(12000);
    M.stopped = 1; run_ms(60000);
    CHECK(M.begins == 0 && M.samples == 0 && T.qn == 0, "U8 stop request before the slot: tx_begin calls %d, samples %ld, queue cleared", M.begins, M.samples);
    fresh("OK1BT", "JO70"); M.armed = 1; M.stop_at = 20000; js8tx_request(&T, L3); js8tx_request(&T, "HB JO70"); run_ms(60000);
    CHECK(M.samples == 20000 + JS8TX_CHUNK && M.writes_after_stop == 0 && M.ends == 1 && T.qn == 0 && M.begins == 1,
          "U8 stop request inside frame 1 (after sample 20000): %ld samples out, writes after the stop %ld, tx_end called, queue cleared, no further tx_begin", M.samples, M.writes_after_stop);
    /* U9 request expires */
    fresh("OK1BT", "JO70"); js8tx_request(&T, "HB JO70"); run_ms(125000); M.armed = 1; run_ms(60000);
    CHECK(M.begins == 0 && T.qn == 0 && noted("TX expired"), "U9 armed 125 s after the request (txttl 120 s): expired, tx_begin calls %d", M.begins);
    /* U10 caps */
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txmaxf=2");
    CHECK(js8tx_request(&T, L3) == JS8TX_E_LONG && T.qn == 0, "U10 message of %d frames with txmaxf=2: refused (too many frames)", n3);
    fresh("OK1BT", "JO70");
    { int a = 0, d;
      for (int i = 0; i < JS8TX_QMAX; i++) a |= js8tx_request(&T, "HB JO70");
      d = js8tx_request(&T, "HB JO70");
      CHECK(!a && d == JS8TX_E_FULL && T.qn == JS8TX_QMAX, "U10 queue cap: message %d refused (queue full), %d waiting", JS8TX_QMAX + 1, T.qn);
      M.armed = 1; run_ms(120000);
      CHECK(M.ntx == JS8TX_QMAX, "U10 queue cap: exactly %d frames go out", M.ntx); }
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txsess=2"); M.armed = 1;
    js8tx_request(&T, "HB JO70"); run_ms(30000); js8tx_request(&T, "HB JO70");
    r = js8tx_request(&T, "HB JO70"); run_ms(60000);
    { int r2 = js8tx_request(&T, "HB JO70"); run_ms(60000);
      CHECK(r == JS8TX_E_SESS && r2 == JS8TX_E_SESS && M.ntx == 2 && M.begins == 2, "U10 session cap 2: third and later requests refused, transmissions %d, tx_begin calls %d", M.ntx, M.begins); }
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txsess=2"); M.armed = 1;
    CHECK(js8tx_request(&T, L3) == JS8TX_E_SESS, "U10 session cap 2: a %d-frame message is refused as a whole", n3);
    /* U11 malformed */
    fresh("OK1BT", "JO70"); M.armed = 1;
    { char longt[80]; memset(longt, 'A', JS8TX_TEXT + 1); longt[JS8TX_TEXT + 1] = 0;
      int a = js8tx_request(&T, ""), b = js8tx_request(&T, "   "), c = js8tx_request(&T, "HELLO\nWORLD"), d = js8tx_request(&T, longt),
          e = js8tx_request(&T, "HELLO: WORLD"), g = js8tx_request(&T, "Q7AB MSG \x7f"), h = js8tx_request(&T, "~~~~");
      run_ms(60000);
      CHECK(a == JS8TX_E_TEXT && b == JS8TX_E_TEXT && c == JS8TX_E_TEXT && d == JS8TX_E_TEXT && e == JS8TX_E_ENC && g == JS8TX_E_TEXT && h == JS8TX_E_ENC &&
            M.begins == 0 && T.qn == 0, "U11 malformed (empty, blanks, newline, 71 chars, ':' without dictionary, DEL, '~~~~'): %d %d %d %d %d %d %d, tx_begin calls %d", a, b, c, d, e, g, h, M.begins); }
    { static const char *bad[] = { "", "@ALLCALL", "123", "A", "OK1BT!" }; int ok = 1;
      for (int i = 0; i < 5; i++) { fresh(bad[i], "JO70"); M.armed = 1; if (js8tx_request(&T, "HB JO70") != JS8TX_E_CALL) ok = 0; run_ms(40000); if (M.begins) ok = 0; }
      CHECK(ok, "U11 own call missing / invalid (\"\", @ALLCALL, 123, A, OK1BT!): refused, no tx_begin"); }
    { static const char *bg[] = { 0, "", "ZZ99", "J070", "JO7" }; int ok = 1;
      for (int i = 0; i < 5; i++) { fresh("OK1BT", bg[i]); M.armed = 1; if (js8tx_request(&T, "HB JO70") != JS8TX_E_GRID) ok = 0; run_ms(40000); if (M.begins) ok = 0; }
      CHECK(ok, "U11 locator missing / invalid (none, \"\", ZZ99, J070, JO7): refused, no tx_begin"); }
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "tx=0"); M.armed = 1; r = js8tx_request(&T, "HB JO70"); run_ms(40000);
    CHECK(r == JS8TX_E_OFF && M.begins == 0, "U11 tx=0 in JS8.CFG: refused, no tx_begin");
    /* U12 TXTEST */
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txtest=1"); M.armed = 1;
    { int a = js8tx_request(&T, "CQ CQ CQ JO70"), b = js8tx_request(&T, "HB JO70"), c = js8tx_request(&T, "HB JO70"); run_ms(60000);
      int d = js8tx_request(&T, "HB JO70"); run_ms(120000);
      CHECK(a == JS8TX_E_TEST && b == 0 && c == JS8TX_E_TEST && d == JS8TX_E_TEST && M.ntx == 1 && M.begins == 1, "U12 TXTEST: only the heartbeat, once: rc %d %d %d %d, transmissions %d", a, b, c, d, M.ntx); }
    /* U13 offset */
    fresh("OK1BT", "JO70");
    { int f = js8tx_pick_offset(&T), g, h;
      js8tx_heard(&T, 1500); js8tx_heard(&T, 1450); js8tx_heard(&T, 1580); g = js8tx_pick_offset(&T);
      for (int q = 500; q <= 2500; q += 100) js8tx_heard(&T, q);
      h = js8tx_pick_offset(&T);
      CHECK(f == 1500 && abs(g - 1500) >= 100 && abs(g - 1450) >= 100 && abs(g - 1580) >= 100 && g >= JS8TX_F_MIN && g <= JS8TX_F_MAX,
            "U13 offset: nothing heard -> %d Hz; stations at 1450/1500/1580 -> %d Hz; crowded band -> %d Hz", f, g, h);
      for (int i = 0; i < 9; i++) js8tx_period(&T);
      CHECK(js8tx_pick_offset(&T) == 1500, "U13 offset: the heard list ages out after 8 periods -> %d Hz", js8tx_pick_offset(&T));
      js8tx_cfg(&T, "txf=2000"); CHECK(js8tx_pick_offset(&T) == 2000, "U13 offset: txf=2000 is taken as it is"); }
    /* U14 the keyboard path (owner decision 24 Nov 2026): ENT = js8tx_send.  One frame and nothing waiting -> the loader's tap credit
     * ("@QTX"), no arming; else the beacon */
    fresh("OK1BT", "JO70"); M.credit = 1; run_ms(1000);
    { int g = 0;
      r = js8tx_send(&T, "HB JO70", &g); run_ms(40000);
      CHECK(r == 1 && g == 1 && M.qtx_ok == 1 && M.ntx == 1 && M.begins == 0 && said("on air at the next slot (tap)") && slot_ok(0) && M.lens[0] == 101120,
            "U14 one frame, the loader grants the tap credit: sent in the next slot WITHOUT arming (tx_begin %d, grants %d, frames %d)", M.begins, M.qtx_ok, M.ntx);
      CHECK(said("SENT: HB JO70") && !said("red frame off"), "U14 ... row \"SENT: HB JO70\", no red-frame row (nothing is armed: the next tap is delivered)");
      run_ms(120000);
      CHECK(M.ntx == 1 && M.qtx == 1, "U14 ... and nothing more (the credit is asked once, frames %d)", M.ntx); }
    fresh("OK1BT", "JO70"); M.credit = 0; run_ms(1000);
    { int g = 1;
      r = js8tx_send(&T, "HB JO70", &g); run_ms(60000);
      CHECK(r == 1 && g == 0 && M.ntx == 0 && T.qn == 1 && said("1 part: MODE > hold FT8 (red), 2 min"), "U14 the loader refuses the tap credit (PERM): queued, \"1 part: MODE > hold FT8 (red), 2 min\", nothing sent");
      M.armed = 1; run_ms(40000);
      CHECK(M.ntx == 1 && M.begins == 1, "U14 ... the red frame sends it (tx_begin %d)", M.begins); }
    fresh("OK1BT", "JO70"); M.credit = 1; js8tx_cfg(&T, "tx1=q7ab snr?"); run_ms(1000);
    { int g = 1; static const char *L3b = "Q7AB MSG HELLO THERE HOW ARE YOU";
      r = js8tx_send(&T, L3b, &g); run_ms(60000);
      CHECK(r >= 3 && g == 0 && M.qtx == 0 && M.ntx == 0 && said("3 parts: MODE > hold FT8 (red), 2 min"), "U14 a message of %d frames: never on the tap credit (asked %d times), waits for the beacon", r, M.qtx); }
    fresh("OK1BT", "JO70"); M.credit = 1; js8tx_cfg(&T, "txtap=0"); run_ms(1000);
    { int g = 1; r = js8tx_send(&T, "HB JO70", &g); run_ms(60000);
      CHECK(r == 1 && g == 0 && M.qtx == 0 && M.ntx == 0, "U14 txtap=0: one frame waits for the beacon too"); }
    fresh("OK1BT", "JO70"); M.credit = 1; run_ms(1000);
    { int g = 0; js8tx_request(&T, "CQ CQ CQ JO70"); r = js8tx_send(&T, "HB JO70", &g);
      CHECK(r == 1 && g == 1 && M.qtx == 1 && T.qn == 1 && !strcmp(T.q[0].text, "HB JO70") && said("TX replaced: on air at the next slot"),
            "U14 one frame while another message of the owner waits: the ENT replaces it (ONE waiting message), then the tap credit"); }
    fresh("OK1BT", "JO70"); M.credit = 1; M.armed = 1; run_ms(1000);
    { int g = 1; r = js8tx_send(&T, "HB JO70", &g); run_ms(40000);
      CHECK(r == 1 && g == 0 && M.qtx == 0 && M.ntx == 1 && M.begins == 1, "U14 beacon armed: the red frame sends it, the tap credit is not asked"); }
    fresh("OK1BT", "JO70"); M.credit = 1; M.fail_at = 30000; run_ms(1000);
    { int g = 0; js8tx_send(&T, "HB JO70", &g); run_ms(40000);
      CHECK(g == 1 && M.ntx == 1 && M.lens[0] == 30000 && T.qn == 0 && noted("owner abort"), "U14 owner abort (PTT / EXIT) during a tap-credit frame: it ends there, the queue is cleared"); }
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "tx=0");
    { int g = 0; CHECK(js8tx_send(&T, "HB JO70", &g) == JS8TX_E_OFF && said("TX refused: TX is off") && !js8tx_item(&T, 0, l80), "U14 tx=0: refused, said, no items"); }
    fresh("OK1BT", "JO70");
    { int g = 0; CHECK(js8tx_send(&T, "HELLO: WORLD", &g) == JS8TX_E_ENC && T.qn == 0 && said("TX refused: text not encodable"), "U14 a text that cannot be built: refused, said, nothing queued"); }
    { char it[JS8TX_TEXT + 1]; int k, n = 0;
      fresh("OK1BT", "JO70"); js8tx_cfg(&T, "tx1=q7ab snr?");
      for (k = 0; k < JS8TX_ITEMS; k++) n += js8tx_item(&T, k, it);
      js8tx_item(&T, 2, it);
      CHECK(n == 3 && !strcmp(it, "Q7AB SNR?"), "U14 the terminal's items: HB, CQ, the preset (%d)", n); }
    /* U15 unknown tx_begin error, no @TX */
    fresh("OK1BT", "JO70"); M.armed = 1; M.begin_err = -6; js8tx_request(&T, "HB JO70"); run_ms(120000);
    CHECK(M.begins == 1 && T.dead && T.qn == 0 && js8tx_request(&T, "HB JO70") == JS8TX_E_OFF, "U15 tx_begin answers an unknown error (-6): TX off for this run, tx_begin calls %d", M.begins);
    fresh("OK1BT", "JO70"); M.armed = 1; M.notx = 1; js8tx_request(&T, "HB JO70"); run_ms(60000);
    CHECK(M.begins == 0, "U15 loader without @TX: the app cannot see the arming -> never calls tx_begin");
    fresh("OK1BT", "JO70"); M.armed = 1; M.clock_bad = 1; js8tx_request(&T, "HB JO70"); run_ms(60000);
    CHECK(M.begins == 0, "U15 radio clock not known: never calls tx_begin");
    /* U16 status lines fit the 40-byte window */
    { char st[80]; int mx = 0; fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txtest=1"); js8tx_cfg(&T, "txsess=500"); js8tx_status(&T, st); mx = (int)strlen(st);
      fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txstrat=B"); js8tx_cfg(&T, "txsess=500"); js8tx_cfg(&T, "txmaxf=12"); M.armed = 1; js8tx_request(&T, L3); js8tx_request(&T, L3); run_ms(30000);
      js8tx_status(&T, st); if ((int)strlen(st) > mx) mx = (int)strlen(st);
      CHECK(mx <= 40, "U16 status line: longest seen %d characters (\"%s\")", mx, st); }
    /* U17 configuration keys */
    fresh("OK1BT", 0);
    { static const char *kk[] = { "tx=1", "txtest=0", "txstrat=b", "txpath=1", "txf=1725", "txmaxf=3", "txsess=7", "txttl=45", "txjsc=0", "loc=jn89xx", "tx2=  hello World  \r\nflo=1" };
      static const char *bad[] = { "txmaxf=99", "txsess=0", "txttl=5", "txf=5000", "txstrat=Q", "txmaxf=", "txsess=x" };
      static const char *other[] = { "flo=500", "hello", "t", "tx", "txx", "tx5abc", "" };
      int all = 1, none = 1;
      for (unsigned i = 0; i < sizeof kk / sizeof kk[0]; i++) if (!js8tx_cfg(&T, kk[i])) all = 0;
      for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) if (!js8tx_cfg(&T, bad[i])) all = 0;
      for (unsigned i = 0; i < sizeof other / sizeof other[0]; i++) {       /* exact-size heap copies: an over-read is an ASan error */
          char *c = malloc(strlen(other[i]) + 1); strcpy(c, other[i]); if (js8tx_cfg(&T, c)) none = 0; free(c); }
      CHECK(all && none && T.enable && !T.test && T.strat_b && (T.cfg_err & 8) && T.f_cfg == 1725 && T.maxf == 3 && T.sess_cap == 7 && T.ttl_s == 45 && !T.use_jsc &&
            !strcmp(T.grid, "JN89") && !strcmp(T.preset[1], "HELLO WORLD") && !T.preset[0][0],
            "U17 JS8.CFG keys: values taken, out-of-range values ignored, foreign tokens left alone (txf %d maxf %d sess %d ttl %d grid %s preset \"%s\")",
            T.f_cfg, T.maxf, T.sess_cap, T.ttl_s, T.grid, T.preset[1]);
      js8tx_cfg(&T, "loc=zz99");
      CHECK(!T.grid[0] && js8tx_request(&T, "HB JN89") == JS8TX_E_GRID, "U17 an invalid loc= leaves NO locator (TX refused) rather than the radio's");
      CHECK(js8tx_cfg(&T, "tx1=abc tx=0") == 2 && js8tx_cfg(&T, "tx=1") == 1 && js8tx_cfg(&T, "flo=1") == 0, "U17 a preset key answers 2 (its value is the rest of the line), another key 1, a foreign one 0"); }
    /* U18 prepared replies (js8feat fills them when a query to MYCALL arrives): only OFFERED as menu items; sent only after the
     * confirm gesture like any other text; offered once */
    { static char prep[3][JS8TX_PREP_W];
      char it[JS8TX_TEXT + 1];
      memset(prep, 0, sizeof prep);
      fresh("OK1BT", "JO70"); T.prep = prep; T.prep_n = 3; strcpy(prep[1], "Q1ABC SNR -05"); strcpy(prep[2], "Q2XYZ GRID JO70");
      M.armed = 1; run_ms(600000);
      CHECK(M.begins == 0 && M.samples == 0 && T.qn == 0, "U18 two prepared replies, beacon armed for 10 min, nobody taps: tx_begin calls %d, samples %ld, queued %d", M.begins, M.samples, T.qn);
      M.armed = 0; run_ms(1000);
      CHECK(js8tx_item(&T, 7, it) && !strcmp(it, "Q1ABC SNR -05") && js8tx_item(&T, 8, it) && !js8tx_item(&T, 6, it), "U18 the terminal's items list the prepared replies (6.. = the table)");
      { int g = 0; r = js8tx_send(&T, "Q1ABC SNR -05", &g); }
      CHECK(r == 1 && T.qn == 1 && !prep[1][0] && prep[2][0] && M.begins == 0, "U18 ENT on the reply queues it; that reply is no longer offered, the other one still is; not armed: tx_begin calls %d", M.begins);
      M.armed = 1; run_ms(40000);
      CHECK(M.ntx == 1 && M.lens[0] == 101120 && T.qn == 0, "U18 armed: exactly one frame (transmissions %d)", M.ntx);
      fresh("OK1BT", "JO70"); T.prep = prep; T.prep_n = 3; js8tx_cfg(&T, "txtest=1");
      CHECK(!js8tx_item(&T, 8, it) && !js8tx_item(&T, 1, it) && js8tx_item(&T, 0, it), "U18 TXTEST: only the heartbeat is an item (no CQ, no replies)"); }
    /* U19 the longest text: JS8TX_TEXT characters (a CW keyer memory of MEM.APP has up to 70); the SEND? screen shows it whole */
    { char t70[JS8TX_TEXT + 2];
      memset(t70, 'E', JS8TX_TEXT); memcpy(t70, "Q7AB ", 5); t70[JS8TX_TEXT] = 0;
      fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txmaxf=12");
      r = js8tx_check(&T, t70);
      CHECK(r > 0 && T.qn == 0, "U19 a %d-character text (the terminal's limit): accepted (%d frames), nothing queued by the check", JS8TX_TEXT, r);
      t70[JS8TX_TEXT] = 'E'; t70[JS8TX_TEXT + 1] = 0;
      CHECK(js8tx_check(&T, t70) == JS8TX_E_TEXT, "U19 %d characters: refused (bad text)", JS8TX_TEXT + 1); }
    /* U20 review 23 Nov 2026 finding 2: a disarm seen before the first frame's tx_begin clears what was waiting for that arming */
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txtest=1"); r = js8tx_request(&T, "HB JO70"); run_ms(2000);
    M.armed = 1; run_ms(1000); M.armed = 0; run_ms(90000); M.armed = 1; run_ms(60000);
    CHECK(r == 0 && M.ntx == 0 && M.begins == 0 && T.qn == 0 && noted("disarmed, waiting messages dropped"),
          "U20 confirmed, armed, disarmed BEFORE the slot, armed again 90 s later: nothing goes out (transmissions %d, tx_begin calls %d)", M.ntx, M.begins);
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txstrat=B"); M.armed = 1; js8tx_request(&T, L3); run_ms(40000);
    M.armed = 0; run_ms(2000);
    CHECK(T.qn == 0 && M.ntx == 1, "U20 (review 24 Nov 2026) strategy B: frame 1 out, the held rest does NOT survive the disarm (queued %d)", T.qn);
    M.armed = 1; run_ms(30000);
    CHECK(M.ntx == 1, "U20 ... and nothing goes out at the new arming (transmissions %d)", M.ntx);
    fresh("OK1BT", "JO70"); M.once = 1; M.armed = 1; js8tx_request(&T, L3); run_ms(40000);
    M.armed = 0; run_ms(2000);
    CHECK(T.qn == 0 && M.ntx == 1 && said("red frame off - message dropped"), "U20 (S1 24 Nov 2026) held after a PERM (the loader refused the next slot): dropped by the disarm too");
    M.spent = 0; M.armed = 1; run_ms(30000);
    CHECK(M.ntx == 1, "U20 ... and nothing goes out at the new arming (transmissions %d)", M.ntx);
    /* U21 review finding 4: a failed "@TX" read changes nothing - not a disarm, not a new arming */
    fresh("OK1BT", "JO70"); M.once = 1; M.armed = 1; js8tx_request(&T, "HB JO70"); js8tx_request(&T, "CQ CQ CQ JO70"); run_ms(40000);
    { int b0 = M.begins, h = T.qn ? T.q[0].hold : -1;
      M.vfail = 1; run_ms(40000);
      CHECK(h == 1 && M.begins == b0 && T.qn == 1 && T.q[0].hold == 1, "U21 held after PERM (one TX per arming), one failed @TX read: still held, tx_begin not asked again (%d -> %d)", b0, M.begins); }
    fresh("OK1BT", "JO70"); js8tx_request(&T, "HB JO70"); M.armed = 1; run_ms(600); M.vfail = 3; run_ms(1600);
    CHECK(T.qn == 1 || M.ntx == 1, "U21 armed, three failed reads in a row: not taken as a disarm (message kept or sent)");
    run_ms(40000);
    CHECK(M.ntx == 1, "U21 ... the frame goes out (transmissions %d)", M.ntx);
    /* U22 NO CQ (rule 8, owner decision 24 Nov 2026): an arming of the green frame sends nothing, asks no @QTXC grant */
    fresh("OK1BT", "JO70"); run_ms(1000);
    M.cq = 1; run_ms(120000);
    CHECK(M.ntx == 0 && M.begins == 0 && M.qtxc == 0 && M.samples == 0 && T.qn == 0, "U22 green frame armed for 2 min: nothing sent, no @QTXC (transmissions %d, @QTXC %d)", M.ntx, M.qtxc);
    CHECK(said("green not used - hold FT8 again"), "U22 ... the screen says \"green not used - hold FT8 again\"");
    M.cq = 0; run_ms(2000); M.cq = 1; run_ms(40000);
    CHECK(M.ntx == 0 && M.qtxc == 0, "U22 green off and on again: still nothing (transmissions %d)", M.ntx);
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txtest=1"); run_ms(1000); M.cq = 1; run_ms(60000);
    CHECK(M.ntx == 0 && M.qtxc == 0, "U22 TXTEST: the green frame sends nothing");
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "tx=0"); run_ms(1000); M.cq = 1; run_ms(60000);
    CHECK(M.ntx == 0 && M.qtxc == 0, "U22 tx=0: the green frame sends nothing");
    fresh("OK1BT", "JO70"); run_ms(1000); { int g; js8tx_send(&T, "HB JO70", &g); } M.cq = 1; run_ms(60000);
    CHECK(M.qtxc == 0 && (M.nlogs == 0 || !strstr(M.logs[0], "CQ CQ CQ")), "U22 a waiting one-part text and the green frame: no CQ ever (@QTXC %d)", M.qtxc);
    /* U23 JS8.CFG: TX off by default, case-insensitive keys, unknown tx* keys refused (review finding 5) */
    memset(&M, 0, sizeof M); M.clk = 1; M.fail_at = -1; M.stop_at = -1; M.now = 1000;
    js8tx_init(&T, &io, "OK1BT", 0, scratch); js8tx_set_grid(&T, "JO70");
    CHECK(!T.enable && js8tx_request(&T, "HB JO70") == JS8TX_E_OFF, "U23 no tx= key: TX is OFF");
    CHECK(js8tx_cfg(&T, "TX=1") == 1 && T.enable, "U23 \"TX=1\": keys are case-insensitive");
    CHECK(js8tx_cfg(&T, "txtest =1") == 1 && T.test, "U23 \"txtest =1\": a blank before '=' is allowed");
    CHECK(js8tx_cfg(&T, "txtset=1") == 1 && (T.cfg_err & 16), "U23 \"txtset=1\" (typing error): refused");
    run_ms(1000);
    CHECK(noted("unknown tx* key"), "U23 ... with the note \"JS8.CFG: unknown tx* key, ignored\"");
    CHECK(js8tx_cfg(&T, "txfoo") == 0 && js8tx_cfg(&T, "flo=500") == 0, "U23 a token without '=' and a foreign key are left alone");
    /* U25 the HB beacon (txhb=, rule 9): arming the beacon is the standing request for "HB <grid>" every txhb minutes */
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txhb=10"); run_ms(60000);
    CHECK(T.hb_min == 10 && M.ntx == 0 && T.qn == 0, "U25 txhb=10, beacon not armed for 1 min: nothing queued, nothing sent");
    M.armed = 1; run_ms(25 * 60000);
    CHECK(M.ntx == 3 && M.nlogs == 3 && strstr(M.logs[2], "HB JO70") && M.starts[1] - M.starts[0] >= 600000u && M.starts[1] - M.starts[0] <= 615000u,
          "U25 armed for 25 min: HB at once and every 10 min (3 frames, %u s apart)", (M.starts[1] - M.starts[0]) / 1000u);
    { int nh = 0, nr = 0; for (int i = 0; i < M.nsaid; i++) { if (!strcmp(M.said[i], "HB SENT, next in 10 min")) nh++; if (!strcmp(M.said[i], "red frame off to tap again")) nr++; }
      CHECK(nh == 3 && nr == 3 && !said("SENT: HB") && said("TX: HB every 10 min") && said("red off = MODE > hold FT8 again"),
            "U25 after each HB \"HB SENT, next in 10 min\" + \"red frame off to tap again\" (%d / %d), at the start \"TX: HB every 10 min\" + \"red off = MODE > hold FT8 again\"", nh, nr); }
    M.armed = 0; run_ms(60000); M.armed = 1; run_ms(60000);
    CHECK(M.ntx == 3, "U25 disarm + arm again 7 min after the last HB: no burst (frames %d)", M.ntx);
    run_ms(4 * 60000);
    CHECK(M.ntx == 4, "U25 ... the next HB comes 10 min after the last queued one (frames %d)", M.ntx);
    M.armed = 0; run_ms(30 * 60000);
    CHECK(M.ntx == 4 && T.qn == 0, "U25 disarmed for 30 min: no HB (frames %d)", M.ntx);
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txhb=10"); js8tx_cfg(&T, "txtest=1"); M.armed = 1; run_ms(25 * 60000);
    CHECK(M.ntx == 0 && T.qn == 0, "U25 TXTEST: the beacon sends nothing (frames %d)", M.ntx);
    fresh("OK1BT", 0); js8tx_cfg(&T, "txhb=10"); M.armed = 1; run_ms(3 * 60000);
    CHECK(M.ntx == 0 && noted("TX beacon: HB refused"), "U25 no locator: the HB is refused (note), nothing sent");
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txhb=5"); M.armed = 1; run_ms(3 * 60000);
    CHECK(T.hb_min == 0 && M.ntx == 0 && noted("bad tx* value"), "U25 txhb=5 (below 10): refused with a note, no beacon");
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txhb=10"); M.armed = 1; js8tx_request(&T, "CQ CQ CQ JO70"); run_ms(60000);
    CHECK(M.ntx >= 1 && M.nlogs >= 1 && strstr(M.logs[0], "CQ CQ CQ"), "U25 a message confirmed before the arming goes first (the HB waits for an empty queue)");
    /* U26 review 24 Nov 2026: nothing to send - shorter than 2 characters or only a callsign (the untouched "<CALL> " of a tap) */
    fresh("OK1BT", "JO70"); M.credit = 1; run_ms(1000);
    { static const char *no[] = { "Q7AB", " Q7AB ", "q7ab", "Q7AB/P", "@ALLCALL", "A", " B ", "Q1AW" };
      static const char *yes[] = { "Q7AB HELLO", "Q7AB SNR?", "HB JO70" };
      int ok = 1, g;
      for (unsigned i = 0; i < sizeof no / sizeof no[0]; i++) { M.nsaid = 0; if (js8tx_send(&T, no[i], &g) != JS8TX_E_EMPTY || g || T.qn || !said("TX: nothing to send - type a message")) ok = 0; }
      CHECK(ok && M.qtx == 0, "U26 a callsign alone or under 2 characters (Q7AB, \" Q7AB \", q7ab, Q7AB/P, @ALLCALL, A, \" B \", Q1AW): \"TX: nothing to send - type a message\", nothing queued, no credit asked");
      js8tx_cfg(&T, "txtap=0"); js8tx_request(&T, L3);
      CHECK(js8tx_send(&T, "Q7AB", &g) == JS8TX_E_EMPTY && T.qn == 1 && T.q[0].n >= 3, "U26 ... and the message that waits stays (nothing replaced it)");
      ok = 1;
      for (unsigned i = 0; i < sizeof yes / sizeof yes[0]; i++) { fresh("OK1BT", "JO70"); if (js8tx_send(&T, yes[i], &g) < 1) ok = 0; }
      fresh("OK1BT", "JO70"); r = js8tx_send(&T, "HELLO", &g);
      CHECK(ok && r != JS8TX_E_EMPTY, "U26 a directed text, a query, HB: accepted; a one-word text that is no callsign (HELLO) is not \"nothing to send\" (rc %d)", r); }
    /* U27 review 24 Nov 2026: ONE waiting message - a new ENT replaces the waiting one (also one that has sent a frame); the row
     * "arm beacon" counts what will really go */
    fresh("OK1BT", "JO70"); run_ms(1000);
    { int g;
      js8tx_send(&T, L3, &g); M.nsaid = 0; r = js8tx_send(&T, "HB JO70", &g);
      CHECK(r == 1 && T.qn == 1 && !strcmp(T.q[0].text, "HB JO70") && M.nsaid == 1 && said("replaced: MODE > hold FT8 (red), 1 part"),
            "U27 3-frame message waiting, ENT on \"HB JO70\": replaced, ONE row says 1 frame (U5)");
      M.armed = 1; run_ms(120000);
      CHECK(M.ntx == 1 && M.nlogs == 1 && strstr(M.logs[0], "HB JO70"), "U27 armed: exactly that one frame goes (transmissions %d)", M.ntx); }
    fresh("OK1BT", "JO70"); M.once = 1; M.armed = 1; js8tx_request(&T, L3); run_ms(40000);
    { int g, k = T.qn ? T.q[0].k : -1;
      M.nsaid = 0; r = js8tx_send(&T, "HB JO70", &g);
      CHECK(k == 1 && r == 1 && T.qn == 1 && M.nsaid == 1 && said("TX replaced: sending 1 part"),
            "U27 a message that has sent frame 1 (held after a PERM, still armed) and a new ENT: the rest is dropped, ONE row says so");
      M.spent = 0; run_ms(120000);
      CHECK(M.ntx == 2 && strstr(M.logs[1], "HB JO70"), "U27 ... the next arming sends only the new frame (transmissions %d)", M.ntx); }
    fresh("OK1BT", "JO70"); run_ms(1000);
    { int g; js8tx_send(&T, L3, &g); M.nsaid = 0; r = js8tx_send(&T, "HELLO: WORLD", &g);
      CHECK(r == JS8TX_E_ENC && T.qn == 0 && said("TX: replaced the waiting message") && said("TX refused: text not encodable"),
            "U27 the ENT replaces the waiting message even when its own text is then refused (nothing waits)"); }
    fresh("OK1BT", "JO70"); run_ms(1000); M.cq = 1; M.gate = 7; run_ms(600);
    { int g; r = js8tx_send(&T, "HB JO70", &g);
      CHECK(T.qn <= 1 && (T.qn == 0 || T.q[0].src != JS8TX_SRC_CQ), "U27 with the green frame on, nothing but the owner's own text waits (no CQ frame)"); }
    /* U28 review 24 Nov 2026: the HB beacon starts only on an arming made with the queue empty */
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txhb=10"); run_ms(1000);
    { int g; js8tx_send(&T, L3, &g); n3 = T.q[0].n;
      M.armed = 1; run_ms(25 * 60000);
      CHECK(M.ntx == n3 && !noted("HB queued"), "U28 txhb=10, a %d-frame message queued, then armed 25 min: the message goes, NO HB on that arming (frames %d)", n3, M.ntx);
      M.armed = 0; run_ms(3000); M.armed = 1; run_ms(40000);
      CHECK(M.ntx == n3 + 1 && strstr(M.logs[n3], "HB JO70"), "U28 ... the next arming (queue empty) starts the HB at once (frames %d)", M.ntx); }
#ifdef JS8_LINK                                  /* (the build "t7_tx_link" of make t7) */
    /* U24 review finding 1: the terminal only HANDS a text to the radio - its GO keeps it for the owner's next tap (the keyboard
     * opens with it), and only ENT there sends it */
    fresh("OK1BT", "JO70"); js8tx_cfg(&T, "txterm=1"); M.armed = 1; run_ms(1000);
    r = js8tx_select(&T, "Q7AB SNR?");
    CHECK(r == 1 && T.qn == 0 && !T.tpend[0], "U24 the terminal loads a text: frames %d, nothing queued, nothing handed over yet", r);
    r = js8tx_confirm(&T); run_ms(61000);
    CHECK(r == 1 && T.qn == 0 && M.begins == 0 && M.samples == 0 && !T.tpend[0] && said("terminal text expired"),
          "U24 GO with the right nonce, beacon ARMED, nobody taps on the radio for 60 s: NOTHING goes out (tx_begin %d, samples %ld), the text expires", M.begins, M.samples);
    js8tx_select(&T, "Q7AB SNR?"); r = js8tx_confirm(&T);
    CHECK(r == 1 && !strcmp(T.tpend, "Q7AB SNR?"), "U24 GO again: the text waits for the owner's tap (tpend \"%s\")", T.tpend);
    js8tx_select(&T, "HB JO70");
    CHECK(js8tx_confirm(&T) == JS8TX_E_BUSY && !strcmp(T.tpend, "Q7AB SNR?"), "U24 a second GO while a text waits: refused (BUSY), the first stays");
    { int g = 0; T.tpend[0] = 0; r = js8tx_send(&T, "Q7AB SNR?", &g); run_ms(40000);   /* (the shell: tap, keyboard with the text, ENT) */
      CHECK(r == 1 && M.ntx == 1 && M.nlogs == 1 && strstr(M.logs[0], "Q7AB SNR?"), "U24 the owner's ENT on the radio: sent (transmissions %d)", M.ntx); }
    js8tx_select(&T, "Q7AB SNR?"); run_ms(61000);
    CHECK(js8tx_confirm(&T) == JS8TX_E_NOSEL, "U24 GO more than 60 s after the text was loaded: nothing to hand over");
    { char t44[48]; memset(t44, 'E', 44); memcpy(t44, "Q7AB ", 5); t44[44] = 0;
      r = js8tx_select(&T, t44); t44[43] = 0;
      CHECK(r == JS8TX_E_TEXT && !T.lsel[0] && js8tx_select(&T, t44) > 0, "U24 (review 24 Nov 2026) the terminal loads 44 characters: refused - over the keyboard's 43, never offered; 43: taken"); }
#endif
    return 0;
}

static double tone_pow(const int16_t *x, int n, double f)
{
    double c = 0, s = 0;
    for (int i = 0; i < n; i++) { c += x[i] * cos(2 * M_PI * f * i / 8000.0); s += x[i] * sin(2 * M_PI * f * i / 8000.0); }
    return c * c + s * s;
}
static int vectors(const char *fn)
{
    FILE *f = fopen(fn, "r"); if (!f) { perror(fn); return 2; }
    static int16_t au[JS8A_FRAME_SAMPLES + 800];
    char txt[32], hx[32], tn[96]; int i3; unsigned crc;
    long nvec = 0, bad_tones = 0, bad_audio = 0, bad_len = 0, bad_eng = 0; int peak = 0;
    unsigned seed = 12345;
    while (fscanf(f, "%31s %d %x %31s %95s", txt, &i3, &crc, hx, tn) == 5) {
        uint8_t fr[11], tones[79]; txs_mfsk_t w; long n = 0; int got, f0;
        for (int i = 0; i < 11; i++) { unsigned v; sscanf(hx + 2 * i, "%2x", &v); fr[i] = (uint8_t)v; }
        seed = seed * 1103515245u + 12345u; f0 = 500 + (int)((seed >> 8) % 1951u);       /* 500 .. 2450 Hz */
        if (js8tx_wave(&w, tones, fr, f0)) { bad_eng++; continue; }
        while ((got = txs_mfsk_fill(&w, au + n, JS8TX_CHUNK)) > 0) n += got;              /* in the app's chunks */
        if (n != JS8A_FRAME_SAMPLES) bad_len++;
        int bt = 0, ba = 0;
        for (int k = 0; k < 79; k++) {
            int best = 0; double bp = -1;
            if (tones[k] != tn[k] - '0') bt = 1;
            for (int t = 0; t < 8; t++) { double p = tone_pow(au + k * 1280, 1280, f0 + 6.25 * t); if (p > bp) { bp = p; best = t; } }
            if (best != tn[k] - '0') ba = 1;
        }
        for (long i = 0; i < n; i++) if (abs(au[i]) > peak) peak = abs(au[i]);
        bad_tones += bt; bad_audio += ba; nvec++;
    }
    fclose(f);
    CHECK(nvec >= 1000 && bad_tones == 0 && bad_eng == 0, "V1 %ld frames of the Python model: tones of the transmit path equal the model's 79 tones (mismatching frames %ld)", nvec, bad_tones);
    CHECK(bad_audio == 0, "V2 the same %ld frames demodulated from the generated AUDIO (random f0 500..2450 Hz): mismatching frames %ld", nvec, bad_audio);
    CHECK(bad_len == 0 && peak == 26029, "V3 every frame is 101120 samples (12.64 s), peak %d (-2.0 dBFS) (wrong length: %ld)", peak, bad_len);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "vectors")) vectors(argv[2]);
    else if (argc == 2 && !strcmp(argv[1], "unit")) unit();
    else { fprintf(stderr, "usage: t7_tx vectors FILE | unit\n"); return 2; }
    printf("t7_tx: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
