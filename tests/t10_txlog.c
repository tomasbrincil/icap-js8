/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * T10 (robustness review): the log record js8tx_run() builds in its 100-byte line buffer.
 * Build with ASan (make t7).  Mock loader as in tests/t7_tx.c; nothing transmits.
 * txmaxf=12: a 70-character message (JS8TX_TEXT), automatic offset 1500 Hz, and the loader fails a
 * write (ICAP_E_PERM, -5: owner abort) in the last 1120 samples of a frame
 *   -> record "T 1500 1/5 100000 -5 <hex22> <56 chars>" = 100 characters + NUL in char l[100].
 * (A short write gives rc -1 and the same length; an error code of two digits, e.g. ICAP_E_TIMEOUT -12, overflows
 * already from 10000 samples on.)
 * Prints the longest record seen; with ASan the run stops at the first write past the buffer.
 *
 * After the fix (23 Nov 2026): the buffer is JS8TX_LOG_LINE bytes.  The test passes when the 100-character record was
 * really produced (the scenario still reaches it), a second run with the longest possible error code (11 characters)
 * gave a 109-character record, and both fit - under ASan (`make t10`) without a report.
 */
#include <stdio.h>
#include <string.h>
#include "../src/js8tx.h"

static struct { uint32_t now, rem, t_start; int armed, sess, fail_rc; long this_n, fail_at; int ntx; size_t maxlog; } M;
static int16_t scratch[JS8TX_CHUNK];
static uint32_t m_ticks(void) { return M.now; }
static void m_wait(uint32_t ms) { M.now += ms; }
static int m_stop(void) { return 0; }
static int m_begin(void)
{
    if (!M.armed) return JS8TX_L_PERM;
    M.sess = 1; M.this_n = 0;
    M.t_start = (M.now + 14499u) / 15000u * 15000u + 500u; if (M.t_start >= M.now + 15000u) M.t_start -= 15000u;
    return (int)(M.t_start - M.now);
}
static int m_write(const int16_t *b, int n)
{
    (void)b;
    if (!M.sess) return JS8TX_L_PERM;
    if (M.now < M.t_start) M.now = M.t_start;
    if (M.fail_at >= 0 && M.this_n >= M.fail_at) { M.sess = 0; return M.fail_rc; }
    M.this_n += n; M.rem += (uint32_t)n; M.now += M.rem / 8u; M.rem %= 8u;
    return n;
}
static void m_end(void) { if (M.sess && M.this_n) M.ntx++; M.sess = 0; }
static int m_vfile(const char *name, char *b, int cap)
{
    if (!strcmp(name, "@TX")) return snprintf(b, (size_t)cap, "v=1 beacon=%d owner=0 gate=0 clk=1 st=0 next=3", M.armed);
    return JS8TX_L_NAME;
}
static int m_cycle(void) { return (int)(M.now % 30000u); }
static void m_say(const char *row, int stamp) { (void)row; (void)stamp; }
static void m_note(const char *l) { (void)l; }
static void m_log(const char *l) { size_t n = strlen(l); if (n > M.maxlog) M.maxlog = n; printf("log record, %zu characters + NUL: %s\n", n, l); }
static const js8tx_io_t io = { m_begin, m_write, m_end, m_ticks, m_stop, m_wait, m_vfile, m_cycle, m_say, m_note, m_log, 0 };
static js8tx_t T;

static size_t run_case(const char *text, int fail_rc)
{
    uint32_t end;
    int r;
    memset(&M, 0, sizeof M); M.now = 1000; M.fail_at = 100000; M.fail_rc = fail_rc;
    js8tx_init(&T, &io, "OK1BT", 0, scratch);   /* the loader starts disarmed: an arming that is already there at the start does not count */
    js8tx_cfg(&T, "tx=1"); js8tx_cfg(&T, "txmaxf=12");
    M.armed = 1;
    js8tx_set_grid(&T, "JO70");
    r = js8tx_request(&T, text);
    printf("request rc=%d frames=%d text length=%zu (JS8TX_TEXT=%d), write error %d at sample %ld; line buffer in js8tx_run: %d bytes\n", r, T.qn ? T.q[0].n : 0, strlen(text),
           JS8TX_TEXT, fail_rc, M.fail_at, JS8TX_LOG_LINE);
    if (r) return 0;
    end = M.now + 200000u;
    while ((int32_t)(end - M.now) > 0) {
        int d = js8tx_poll(&T);
        if (d == 0) { uint32_t t0 = M.now; js8tx_run(&T); if (M.now == t0) M.now += 1; } else M.now += 10;
    }
    return M.maxlog;
}

int main(void)
{
    /* JS8TX_TEXT = 70 characters (since the keyer memories, 23 Nov 2026); without the dictionary every character is a Huffman code */
    static const char *textA = "Q7AB MSG THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 1234567890 ABCDEF";
    size_t a = run_case(textA, JS8TX_L_PERM), b = run_case(textA, -2147483647);
    int ok = strlen(textA) == JS8TX_TEXT && a == 114 && b == 123 && b + 1 <= JS8TX_LOG_LINE;
    printf("%s  T10 log records of %zu and %zu characters (+ NUL) in a %d-byte buffer (before the fix: 100 + NUL in char l[100])\n", ok ? "PASS" : "FAIL", a, b, JS8TX_LOG_LINE);
    return ok ? 0 : 1;
}
