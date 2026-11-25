/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8app_tx.h - the loader binding of the transmit path, included by shell/js8app.c ONLY in the RX+TX
 * flavour (-DJS8_TX).  It is a separate file so that the RX-only flavour (JS8.APP) contains no call of
 * icap_tx_* at all, not even in dead source text: tests/e2e.py greps the sources linked into JS8.APP.
 * Not a stand-alone header: it uses the statics of js8app.c defined above its #include.
 */
/* ---- transmit glue (js8tx.c does the work and holds every safety rule; this is only the loader binding) */
static uint32_t n_txslots;                       /* periods in which audio was written to the transmitter */
static int tx_cut;                               /* the running decode was left for a transmission */
static int tx_due = -1;                          /* the last js8tx_poll: ms until a transmission (-1 none), polled at tx_due_t */
static uint32_t tx_due_t;
static int txpoll(void) { tx_due = js8tx_poll(&txq); tx_due_t = icap_ticks(); return tx_due; }
static int txio_vfile(const char *name, char *b, int max)
{
    int fd = icap_open(name, ICAP_O_READ), n;
    if (fd < 0) return fd;
    n = icap_read(fd, b, (uint32_t)max - 1); icap_close(fd);
    if (n < 0) return n;
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) n--;
    b[n] = 0;
    return n;
}
static int txio_cycle(void)                      /* ms since the last :00 / :30 of the radio clock, -1 = not known */
{
    uint32_t e = icap_ticks() - txc.tick;
    if (cap.clock_bad || !txc.edge || e > 1500u) return -1;
    return (txc.sec % 30) * 1000 + (int)(e > 999u ? 999u : e);
}
static void txio_say(const char *row, int stamp)
{
    char l[5 + 96], *q = l;                      /* a stamped row is longer than the screen: shown cut, the terminal gets it whole */
    char sv;
    if (stamp) {                                 /* the period that starts next (in less than 3 s) */
        int sod = txc.sod + 3; sod -= sod % PERIOD; sod %= 86400;
        q = num(q, sod / 3600, 2, '0'); q = num(q, sod / 60 % 60, 2, '0'); *q++ = ' ';   /* "HHMM " as on a decoded row */
    }
    while (*row && q < l + sizeof l - 1) *q++ = clean(*row++);
    *q = 0;
    sv = l[DEC_W]; l[DEC_W] = 0;                 /* the DECODE screen: 40 characters; the status row "HHMM TX <f0> k/n <text>" is cut there */
    icap_decode_out(l);
    l[DEC_W] = sv;
#ifdef JS8_LINK
    if (stamp) link_tx_start(l + 5);             /* "TX <f0> <k>/<n> <text>": the frame is keyed for the next slot */
#endif
}
static void txio_log(const char *line)           /* "T <rest>" -> "T <yyyymmddThhmmss> <rest>" in the frame log */
{
    char b[JS8TX_LOG_LINE + 20], *q = b;         /* the record + "yyyymmddThhmmss " */
    struct icap_time t;
    int sod = txc.sod, stopped = 0;
    if (icap_clock_now(&t) == 0 && t.year >= 2020) { sod = (t.hour * 60 + t.min) * 60 + t.sec; cur_ymd = (uint32_t)t.year * 10000u + (uint32_t)t.mon * 100u + t.day; }
    {   /* the period that was transmitted in (the record is written right after it).  A frame that never got audio
         * (0 samples: stopped / disarmed between tx_begin and the slot) is recorded up to 1 s BEFORE its period begins:
         * it gets the period it was keyed for, like its "TX <f0> k/n" row - not the period before, which may hold the
         * record of the frame that was really sent there */
        const char *p = line; int sp = 0;
        for (; *p && sp < 3; p++) if (*p == ' ') sp++;
        if (p[0] == '0' && p[1] == ' ') { sod = (sod + 3) % 86400; stopped = (p[2] == '1' || p[2] == '3') && p[3] == ' '; }   /* (3: a tap gave the grant back) */
    }
    sod -= sod % PERIOD;
    *q++ = 'T'; *q++ = ' '; q = num(q, (int)cur_ymd, 8, '0'); *q++ = 'T'; q = hhmmss(q, sod / 3600, sod / 60 % 60, sod % 60);
    cat_clean(q, line + 1);
    /* The frame log's T records are the frames that were handed to the transmitter.  A frame that a stop request
     * (EXIT / STOP / cancel from the terminal) took back before its slot - 0 samples, rc 1 - is not one: the log has
     * the note "TX: stop before the slot, queue cleared" for it.  The terminal still gets its "T E" (PROTOCOL.md: every
     * "T S" is followed by a "T E"). */
    if (!stopped) { log_add(&lg_frames, b); lg_frames.urgent = 1; }
#ifdef JS8_LINK
    link_tx_end(line);
#endif
    {   /* "T <f0> <k/n> <samples> <rc> <hex22> <text>": a frame that went out whole -> the report we sent (heard table, ADIF) */
        const char *p = line; int sp = 0, rc0 = 0;
        for (; *p && sp < 6; p++) if (*p == ' ') { sp++; if (sp == 4) rc0 = p[1] == '0' && p[2] == ' '; }
        if (rc0) js8f_sent(feat, p);
    }
}
static void txio_wait(uint32_t ms) { icap_sleep_ms(ms); }
static uint32_t txio_ticks(void) { return icap_ticks(); }
static int txio_begin(void) { return icap_tx_begin(); }
static int txio_write(const int16_t *b, int n) { return icap_tx_write(b, n); }
static void txio_end(void) { icap_tx_end(); }
static void note(const char *s);
static int kbd_tapped(void);                     /* shell/js8app_kbd.h: a new tap while js8tx_run waits for a tap grant's slot (S3) */
static int txio_cancel(void) { return kbd_tapped(); }   /* (named txio_*: tools/js8stack.py knows the interface by its names) */
static int tx_available(void) { return icap_tx_available(); }
#ifdef JS8_LINK
#define TXIO_STOP link_stop                      /* EXIT / STOP, or a cancel from the terminal while a frame is on the air */
#else
#define TXIO_STOP stop
#endif
static const js8tx_io_t txio = { txio_begin, txio_write, txio_end, txio_ticks, TXIO_STOP, txio_wait, txio_vfile, txio_cycle, txio_say, note, txio_log, txio_cancel };

/* One slot.  Nothing here touches the SD card before the audio is out: the capture in progress is left as it
 * is (frozen about 1 s before its end) and closed AFTER the transmission; the period that was transmitted is
 * not captured at all, and whatever the receiver heard meanwhile is thrown away. */
static void tx_slot(void)
{
    struct icap_time t;
    int sent;
    tx_busy = 1;                                 /* notes written by js8tx_run (after the audio) must not move the capture */
    sent = js8tx_run(&txq);
    tx_busy = 0;
#ifdef JS8_LINK
    if (sent && lnk) { link_tx_start(0); }       /* what the frame left for the terminal (T E, notes, status): one bounded send */
#endif
    if (!sent) return;                           /* nothing was keyed: the receiver goes on as if nothing happened */
    n_txslots++;
    kbd_resync();                                /* taps made while the frame was keyed are no request */
    /* (a message that went out whole: js8tx_run said "SENT: <text>" / "HB SENT, next in N min", and while the beacon is
     * armed - taps are not delivered then - "red frame off to tap again") */
    if (cap.fd >= 0) cap_finish();               /* the period before the transmission: decode what was captured */
    for (int g = 0; g < 64 && icap_audio_read(abuf, (int)(sizeof abuf / sizeof abuf[0])) != 0; g++) ;
    if (icap_clock_now(&t) == 0 && t.year >= 2020)   /* the transmitted period is not a "skipped" one */
        cap.last_key = (t.hour * (3600 / PERIOD) + t.min * (60 / PERIOD) + t.sec / PERIOD) % NKEYS;
    cap.last_sec = -1;
    log_flush(&lg_frames);                       /* the record of the transmitted frame goes to the card at once */
}
/* main-loop contexts only (never inside a decode or a dictionary read) */
static void tx_service(void) { if (txpoll() == 0) tx_slot(); }
