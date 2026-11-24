/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8app_store.h - the storage layer of the JS8 radio app, included by shell/js8app.c.
 * Not a stand-alone header: it uses the statics of js8app.c defined above its #include.
 *
 * The loader's file API has open / read / write / close / remove only: no append, no seek, opening a
 * file for writing empties it.  What happens to a file that is open for writing when the power goes
 * is not known, so the rule here is:  A FILE THAT HOLDS DATA IS NEVER OPENED FOR WRITING AGAIN.
 *
 * SEGMENTS.  Each log stream is a row of small numbered files, every one written exactly once:
 *     JS8L0001.TXT ...   frames and notes (R / N / T lines)
 *     JS8M0001.TXT ...   complete messages (M lines)
 *     JS8A0001.ADI ...   ADIF records of exchanges the operator marked (only when the loader's own
 *                        ADIF log "@QLOG" -> ICAPQSO.ADI is not available; one record per file)
 *   Lines wait in RAM (JS8_LOG_SZ / JS8_MSG_SZ bytes) and go out as ONE segment when the buffer is
 *   full, when the oldest waiting line is `flush=` periods old (JS8.CFG, default 4 = one minute), at
 *   once for something that matters (a message for MYCALL, a transmitted frame, an error note) and
 *   when the app leaves.  A power cut therefore loses at most the lines that were still waiting (the
 *   current, unwritten segment) and can damage at most the one file that was being written; every
 *   earlier segment stays as it was.
 *   A segment is text:
 *     H JS8L 00000042 <EOH>     first line: stream, sequence number (counts up for the life of the
 *                               card; sorting by it gives the time order whatever the file numbers are)
 *     ...                       the lines
 *     E 0017 3fa9               last line: number of lines, CRC-16 (js8_crc16) of every byte before
 *                               this line.  A segment without a valid last line was cut by a power
 *                               failure / a full card: its complete lines are still good.  (A file
 *                               that was cut before its header line was complete holds nothing: its
 *                               number is simply used again.)
 *   ("<EOH>" makes the header an ADIF header, so the JS8A files are valid ADIF as they are.)
 *
 * ROTATION / BOUNDED CARD USE.  Each stream is a ring of `keep=` files (JS8.CFG, 2..9999, default 600):
 * numbers 1..keep are used in turn; a number that is already taken is never overwritten before every
 * number is in use (the owner deleted files, or the position was lost); once the ring is full the
 * OLDEST segment is replaced (a note says so once per run).  The card therefore never holds more than
 *     keep x (JS8_LOG_SZ + JS8_MSG_SZ + one ADIF record) + 2 state files + 2 capture files.
 *
 * INDEX.  Where the rings stand (next number, sequence number, full) is kept, together with the heard
 * table and the inbox, in a small state file that exists TWICE: JS8STA.BIN and JS8STB.BIN are written
 * in turn, each with a generation counter and a CRC-16.  At start the newer valid one is used; a
 * power cut can damage only the one being written, the other is then one step old.  A state that is
 * one step old (or lost altogether) is corrected from the segment files themselves: the header line of
 * the file at the ring position says whether a newer segment is already there, and without any state
 * the position is found by probing (the lowest free number; when all are taken, the highest sequence
 * number).  The state files are not portable between builds (raw structures); a file of another
 * layout is ignored.
 * The probing is bounded between two stop polls: looking for a free number goes over at most SEG_PROBE
 * files per call (the segment stays in RAM and the search goes on at the next call), and the scan for
 * the highest sequence number of a full ring ends at a stop request.  A scan that was cut short leaves
 * the position UNKNOWN, and with an unknown position nothing is written: when the app leaves in that
 * state (no state file, ring full, EXIT before the first segment of the run) the lines still in RAM
 * are lost - the alternative would be to replace a segment that is not the oldest.
 *
 * Write errors (card full, card error): the segment stays in RAM and is tried again every
 * SEG_RETRY periods; when the buffer is full meanwhile the older half of the waiting lines is dropped.
 * A state file that could not be written is tried again after SNAP_RETRY_MS, not on every pass of the
 * idle loop (a missing card would otherwise get 50 failed opens per second).
 * The receiver keeps running; "LOGERR" on the status line.
 * At most 2 of the app's files are open at any time here (the capture file + one of these).
 */
#define SEG_HDR    23u                  /* "H JS8L 00000042 <EOH>\r\n" */
#define SEG_TRL    13u                  /* "E 0017 3fa9\r\n" */
#define SEG_RETRY  4                    /* periods between two tries after a failed write */
#define SEG_PROBE  10                   /* files looked at per seg_write call while searching a free number (3 SD calls each) */
#define SNAP_RETRY_MS 5000u             /* a failed state file write is tried again after this */
#define F_STA      MODE_NAME "STA.BIN"
#define F_STB      MODE_NAME "STB.BIN"

typedef struct {
    const char *pfx, *ext;
    char *buf; uint32_t cap, len;                /* lines waiting for their segment */
    int dirty;                                   /* something waits (len != 0) */
    int next, full, said;                        /* ring: the number the next segment gets (0 = not known), every number in use, the note was given */
    uint32_t seq;                                /* sequence number of the newest segment */
    int age, urgent, lines, failed, wait, wrapped;
    int tries;                                   /* taken numbers passed so far in the search for a free one */
} alog_t;
static alog_t lg_frames = { "JS8L", ".TXT", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
              lg_msgs   = { "JS8M", ".TXT", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
              lg_adif   = { "JS8A", ".ADI", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static int log_err;                              /* a log write failed (shown on the panel) */
static uint32_t n_segs, n_segfail, n_snaps, snap_gen;
static int snap_dirty, snap_found;               /* the state changed / a valid state file was found at start */
static int snap_fail; static uint32_t snap_t;    /* the last state file write failed, at this tick */
static const char *pending_note;                 /* a note that could not be written from inside log_add */

static void seg_name(const alog_t *l, int i, char *nm) { char *q = cat(nm, l->pfx); q = num(q, i, 4, '0'); cat(q, l->ext); }
/* the segment file number i: -1 = free (no such file, or one of ours that a power cut / a full card left empty or
 * with less than its header line: nothing in it, it is taken again), 0 = a file without our header line (foreign:
 * never overwritten before the ring is full), else its sequence number */
static int32_t seg_seq(const alog_t *l, int i)
{
    char nm[16], h[16];
    int fd, n, k;
    int32_t v = 0;
    seg_name(l, i, nm);
    fd = icap_open(nm, ICAP_O_READ);
    pump();
    if (fd < 0) return -1;
    n = icap_read(fd, h, sizeof h); icap_close(fd);
    if (n < 0) n = 0;
    for (k = 0; k < n && k < 15; k++) {          /* "H JS8L 00000042" */
        char want = k == 0 ? 'H' : (k == 1 || k == 6) ? ' ' : k < 6 ? l->pfx[k - 2] : 0;
        if (want ? h[k] != want : (h[k] < '0' || h[k] > '9')) return 0;
        if (k >= 7) v = v * 10 + (h[k] - '0');
    }
    return n < 15 ? -1 : v;
}
static void seq_seen(alog_t *l, int32_t s) { if (s > 0 && (uint32_t)s > l->seq) l->seq = (uint32_t)s; }
/* The ring position is not known (no state file): the lowest free number - segments are normally contiguous -
 * and, when every number is taken, the one after the highest sequence number. */
static void ring_find(alog_t *l)
{
    int lo = 0, hi = 1, i;
    l->full = 0;
    while (hi <= cfg_keep && seg_seq(l, hi) >= 0) { lo = hi; hi *= 2; }
    if (hi > cfg_keep + 1) hi = cfg_keep + 1;
    while (hi - lo > 1) { int mid = (lo + hi) / 2; if (seg_seq(l, mid) >= 0) lo = mid; else hi = mid; }
    l->next = hi;
    if (hi <= cfg_keep) { if (lo) seq_seen(l, seg_seq(l, lo)); return; }
    l->full = 1; l->next = 1;
    for (i = 1; i <= cfg_keep; i++) {
        int32_t s;
        /* a stop request ends the scan, and a scan that did not see every file does not know the oldest one: the
         * position stays unknown (seg_write then writes nothing) rather than pointing at file 1 */
        if (stop()) { l->next = 0; l->full = 0; return; }
        s = seg_seq(l, i);
        if (s > 0 && (uint32_t)s > l->seq) { l->seq = (uint32_t)s; l->next = i % cfg_keep + 1; }
    }
}
/* At start, with a position from the state file: the state may be one step old (power cut between a segment and
 * its state file).  A file at the position that carries a NEWER sequence number is such a segment: go past it. */
static void ring_verify(alog_t *l)
{
    int tries = 0;
    if (l->next < 1 || l->next > cfg_keep) { l->next = 0; l->full = 0; return; }   /* keep= was lowered: probe at the first write */
    while (tries++ < cfg_keep && !stop()) {
        int32_t s = seg_seq(l, l->next);
        if (s <= 0 || (uint32_t)s <= l->seq) break;
        l->seq = (uint32_t)s;
        if (++l->next > cfg_keep) { l->next = 1; l->full = 1; }
    }
}
/* One segment: header line, the lines, trailer line.  Not interruptible (a segment that stops half way is a cut
 * file); audio keeps being pumped.  1 = written, 0 = not written (card error, or the ring position is not known
 * because a stop request cut the scan short), -1 = still looking for a free number: call again. */
static int seg_write(alog_t *l, const char *p, uint32_t n, int lines)
{
    char nm[16], h[32], *q;
    uint16_t crc;
    int fd, ok = 1, hl;

    if (l->next < 1 || l->next > cfg_keep) { ring_find(l); l->tries = 0; if (l->next < 1) return 0; }
    if (!l->full) {                              /* never overwrite before every number is in use */
        int budget = SEG_PROBE;
        while (l->next != l->failed) {           /* (a file our own failed write left behind is taken again) */
            int32_t s = seg_seq(l, l->next);
            if (s < 0) break;
            /* after a wrap: our own, older segment at the position - the ring is ours and it is full */
            if (l->wrapped && s > 0 && (uint32_t)s < l->seq) { l->full = 1; break; }
            seq_seen(l, s);
            if (++l->next > cfg_keep) { l->next = 1; l->wrapped = 1; }
            /* every number is taken: the ring is full.  The position is back at the number the search began at */
            if (++l->tries >= cfg_keep) { l->full = 1; break; }
            /* a folder full of other files: not all in one go (the stop request is polled between the calls) */
            if (--budget == 0) return -1;
        }
    }
    if (l->full && !l->said) {                   /* say it once per run; the note itself is written by the main loop */
        l->said = 1;
        pending_note = l == &lg_frames ? MODE_NAME ": log ring full, oldest JS8L* replaced" : l == &lg_msgs ? MODE_NAME ": msg ring full, oldest JS8M* replaced"
                                       : MODE_NAME ": ADIF ring full, oldest JS8A* replaced";
    }
    seg_name(l, l->next, nm);
    q = cat(h, "H "); q = cat(q, l->pfx); *q++ = ' '; q = num(q, (int)((l->seq + 1u) % 100000000u), 8, '0'); q = cat(q, " <EOH>\r\n");
    hl = (int)(q - h);
    fd = icap_open(nm, ICAP_O_WRITE);
    if (fd < 0) { n_segfail++; log_err = 1; return 0; }
    if (icap_write(fd, h, (uint32_t)hl) != hl) ok = 0;
    crc = js8_crc16(js8_crc16(0xFFFFu, h, (uint32_t)hl), p, n);
    while (n && ok) {
        uint32_t m = n > 1024u ? 1024u : n;
        if (icap_write(fd, p, m) != (int)m) ok = 0;
        p += m; n -= m;
        pump();
    }
    q = cat(h, "E "); q = num(q, lines > 9999 ? 9999 : lines, 4, '0'); *q++ = ' ';
    for (int s = 12; s >= 0; s -= 4) *q++ = "0123456789abcdef"[(crc >> s) & 15];
    *q++ = '\r'; *q++ = '\n';
    if (ok && icap_write(fd, h, SEG_TRL) != (int)SEG_TRL) ok = 0;
    icap_close(fd);
    if (!ok) { l->failed = l->next; n_segfail++; log_err = 1; return 0; }
    l->failed = 0; l->tries = 0; l->seq++; n_segs++; snap_dirty = 1;
    if (++l->next > cfg_keep) { l->next = 1; l->wrapped = 1; }
    return 1;
}
/* write the waiting lines as a segment now; on an error they stay and are tried again later */
static void log_flush(alog_t *l)
{
    int r;
    if (!l->len) { l->dirty = 0; return; }
    r = seg_write(l, l->buf, l->len, l->lines);
    if (r > 0) { l->len = 0; l->lines = 0; l->dirty = 0; l->age = 0; l->urgent = 0; l->wait = 0; }
    else l->wait = r < 0 ? 0 : SEG_RETRY;       /* still searching a free number: the next call goes on; an error: later */
}
/* the same when the lines cannot wait (buffer full, the app leaves): the search for a free number may take a few calls */
static void log_flush_now(alog_t *l)
{
    int k = 0;
    do log_flush(l); while (l->len && !l->wait && ++k < 6);
}
static void log_add(alog_t *l, const char *s)
{
    int n = slen(s);
    if (!l->buf) return;
    if ((uint32_t)n + 2 > l->cap) n = (int)l->cap - 2;
    if (l->len + (uint32_t)n + 2 > l->cap) {
        log_flush_now(l);
        if (l->len) {                            /* the segment could not be written (card full?): drop the older half */
            uint32_t cut = l->len / 2; int gone = 0;
            while (cut < l->len && l->buf[cut] != '\n') cut++;
            if (cut < l->len) cut++;
            for (uint32_t i = 0; i < cut; i++) if (l->buf[i] == '\n') gone++;
            mvdown(l->buf, l->buf + cut, l->len - cut); l->len -= cut; l->lines -= gone;
            if (l->len + (uint32_t)n + 2 > l->cap) { l->len = 0; l->lines = 0; }
        }
    }
    memcpy(l->buf + l->len, s, (unsigned)n); l->len += (uint32_t)n; l->buf[l->len++] = '\r'; l->buf[l->len++] = '\n';
    l->lines++; l->dirty = 1;
}
/* once per period (all = 1), and from the idle loop (all = 0: only what is urgent) */
static void log_tick(alog_t *l, int all)
{
    if (!l->len) return;
    if (all) { l->age++; if (l->wait) l->wait--; }
    if (!l->wait && (l->urgent || l->age >= cfg_flush)) log_flush(l);
}

/* ---- the state file pair: ring positions + the part of the features that is kept (js8f_keep_t) */
#define SNAP_HDR 36u                             /* "J8S2", gen u32, size u16, crc u16, 3 x (seq u32, next u16, full u8, 0) */
static void put32(char *p, uint32_t v) { p[0] = (char)v; p[1] = (char)(v >> 8); p[2] = (char)(v >> 16); p[3] = (char)(v >> 24); }
static uint32_t get32(const char *p) { const uint8_t *u = (const uint8_t *)p; return (uint32_t)u[0] | (uint32_t)u[1] << 8 | (uint32_t)u[2] << 16 | (uint32_t)u[3] << 24; }
static alog_t *const snap_ring[3] = { &lg_frames, &lg_msgs, &lg_adif };
static void snap_head(char *h, uint32_t gen)
{
    memset(h, 0, SNAP_HDR);
    h[0] = 'J'; h[1] = '8'; h[2] = 'S'; h[3] = '2'; put32(h + 4, gen); put32(h + 8, (uint32_t)sizeof(js8f_keep_t));   /* crc (h[10..11]) = 0 for now */
    for (int i = 0; i < 3; i++) {
        const alog_t *l = snap_ring[i];
        put32(h + 12 + 8 * i, l->seq);
        h[16 + 8 * i] = (char)l->next; h[17 + 8 * i] = (char)(l->next >> 8); h[18 + 8 * i] = (char)(l->full != 0);
    }
}
static void snap_save(void)
{
    char h[SNAP_HDR];
    uint16_t crc;
    int fd, ok;
    if (!snap_dirty || !feat) return;
    snap_fail = 1; snap_t = icap_ticks();        /* (until the write below has worked) */
    snap_head(h, snap_gen + 1u);
    crc = js8_crc16(js8_crc16(0xFFFFu, h, SNAP_HDR), &feat->k, (uint32_t)sizeof feat->k);
    h[10] = (char)crc; h[11] = (char)(crc >> 8);
    fd = icap_open(((snap_gen + 1u) & 1u) ? F_STB : F_STA, ICAP_O_WRITE);
    if (fd < 0) { log_err = 1; return; }
    ok = icap_write(fd, h, SNAP_HDR) == (int)SNAP_HDR && icap_write(fd, &feat->k, (uint32_t)sizeof feat->k) == (int)sizeof feat->k;
    icap_close(fd);
    pump();
    if (!ok) { log_err = 1; return; }
    snap_gen++; n_snaps++; snap_dirty = 0; snap_fail = 0;
}
/* the main loop's call (every 20 ms when idle, once per period): after a failed write not before SNAP_RETRY_MS have passed */
static void snap_tick(void)
{
    if (snap_fail && icap_ticks() - snap_t < SNAP_RETRY_MS) return;
    snap_save();
}
/* at start: the newer valid state file -> ring positions, heard table, inbox.  scratch: >= SNAP_HDR + sizeof(js8f_keep_t) bytes */
static void snap_load(char *scratch, uint32_t cap)
{
    const uint32_t want = SNAP_HDR + (uint32_t)sizeof(js8f_keep_t);
    if (cap < want) return;
    for (int k = 0; k < 2 && !stop(); k++) {
        int fd = icap_open(k ? F_STB : F_STA, ICAP_O_READ), r;
        uint32_t n = 0, gen;
        uint16_t crc;
        char one;
        if (fd < 0) continue;
        while (n < want && !stop() && (r = icap_read(fd, scratch + n, want - n)) > 0) { n += (uint32_t)r; pump(); }   /* (a card that answers in
                                                  * small pieces: the stop request is polled between them; a stop leaves the file unused) */
        r = n == want ? icap_read(fd, &one, 1) : 0;   /* a longer file is not ours */
        icap_close(fd);
        if (n != want || r > 0 || scratch[0] != 'J' || scratch[1] != '8' || scratch[2] != 'S' || scratch[3] != '2') continue;
        if ((get32(scratch + 8) & 0xFFFFu) != (uint32_t)sizeof(js8f_keep_t)) continue;
        crc = (uint16_t)((uint8_t)scratch[10] | (uint8_t)scratch[11] << 8);
        scratch[10] = scratch[11] = 0;
        if (js8_crc16(0xFFFFu, scratch, want) != crc) continue;
        gen = get32(scratch + 4);
        if (snap_found && (int32_t)(gen - snap_gen) <= 0) continue;
        snap_found = 1; snap_gen = gen;
        for (int i = 0; i < 3; i++) {
            alog_t *l = snap_ring[i];
            l->seq = get32(scratch + 12 + 8 * i);
            l->next = (uint8_t)scratch[16 + 8 * i] | (uint8_t)scratch[17 + 8 * i] << 8; l->full = scratch[18 + 8 * i] == 1;
        }
        memcpy(&feat->k, scratch + SNAP_HDR, sizeof feat->k);
    }
    if (snap_found) for (int i = 0; i < 3 && !stop(); i++) ring_verify(snap_ring[i]);
}
