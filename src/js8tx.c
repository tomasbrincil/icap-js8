/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8tx.c - transmit path of the JS8 radio app: outbox, safety rules, the selection a human confirms.
 * See js8tx.h for the rules.  Nothing in this file keys a transmitter by itself: every path to io->tx_begin /
 * io->tx_write starts at js8tx_send() - called only for the ENT of the radio's keyboard, which showed the owner the whole
 * text - at js8tx_request() (the unit tests stand for that), at the owner's arming of CQ (one "CQ CQ CQ <grid>" frame per
 * arming, rule 8) or at the arming of the beacon with txhb= set (rule 9).
 */
#include "js8tx.h"
#include "js8enc.h"
#include "js8_constants.h"

#define IO (t->io)

/* ---- small text helpers (no libc) */
static char *cp(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char *cpn(char *d, const char *s, int max) { while (*s && max-- > 0) *d++ = *s++; *d = 0; return d; }
static int eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
#ifdef JS8_LINK
static int pfx(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
#endif
static char *un(char *d, unsigned v)
{
    char b[10]; int n = 0;
    do { b[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) *d++ = b[--n];
    *d = 0; return d;
}
/* " key=<int>" of a loader k=v line (the key must start a token), def when absent */
static int fld(const char *b, const char *key, int def)
{
    int i, j, neg, v;
    for (i = 0; b[i]; i++) {
        if (i && b[i - 1] != ' ') continue;
        for (j = 0; key[j] && b[i + j] == key[j]; j++) ;
        if (key[j] || b[i + j] != '=') continue;
        i += j + 1; neg = b[i] == '-'; if (neg) i++;
        if (b[i] < '0' || b[i] > '9') return def;
        for (v = 0; b[i] >= '0' && b[i] <= '9'; i++) v = v * 10 + (b[i] - '0');
        return neg ? -v : v;
    }
    return def;
}
static int grid_ok(const char *g)
{
    return g[0] >= 'A' && g[0] <= 'R' && g[1] >= 'A' && g[1] <= 'R' && g[2] >= '0' && g[2] <= '9' && g[3] >= '0' && g[3] <= '9';
}
static char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static char lo(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

/* ---- configuration (keys case-insensitive, blanks before '=' allowed; the shell also normalises the file) */
static const char *kv(const char *s, const char *k)    /* after "<k> =": the value start, or 0 */
{
    while (*k) if (lo(*s++) != *k++) return 0;
    while (*s == ' ' || *s == '\t') s++;
    return *s == '=' ? s + 1 : 0;
}
static int vint(const char *s)                       /* digits (blanks skipped); -1 = none, or something else follows them */
{
    int v = 0, nd = 0;
    for (; *s == ' '; s++) ;
    for (; *s >= '0' && *s <= '9' && nd < 6; s++, nd++) v = v * 10 + (*s - '0');
    return nd && *s <= ' ' ? v : -1;
}
static int vbool(const char *s)                      /* exactly 0 / 1 / on / off / yes / no (any case, S9 24 Nov 2026); -1 = not understood */
{
    static const char w[] = "0\0001\0OFF\0ON\0NO\0YES\0";
    const char *k = w;
    int i, v = 0;
    for (; *s == ' '; s++) ;
    for (; *k; v++) {
        for (i = 0; k[i] && up(s[i]) == k[i]; i++) ;
        if (!k[i] && (unsigned char)s[i] <= ' ') return v & 1;
        while (*k) k++;
        k++;
    }
    return -1;
}
/* Grants of the loader: the tap credit ("@QTX0/1", one-frame keyboard messages, rule 1) and the CQ grant ("@QTXC", rule 8). */
static void poll_tx(js8tx_t *t, int quiet);

void js8tx_init(js8tx_t *t, const js8tx_io_t *io, const char *mycall, js8jsc_t *jsc, int16_t *buf)
{
    uint8_t *p = (uint8_t *)t; uint32_t n = sizeof *t;
    while (n--) *p++ = 0;
    t->io = io; t->mycall = mycall; t->jsc = jsc; t->buf = buf;
    t->enable = 0;                                     /* TX is OFF unless JS8.CFG says tx=1 (rule 7) */
    t->maxf = 6; t->sess_cap = 20; t->ttl_s = 120; t->use_jsc = 1; t->tap = 1;
    t->gate = -1; t->clk = -1;
    t->t_next = io->ticks(); t->t_tx = t->t_next - 1000u;
    poll_tx(t, 1);                                     /* the state of "@TX" / cq= at the very start: an arming that is already there does not count */
}

void js8tx_set_grid(js8tx_t *t, const char *loc)
{
    char g[4]; int i;
    for (i = 0; i < 4; i++) { if (!loc[i]) return; g[i] = up(loc[i]); }
    if (!t->grid[0] && grid_ok(g)) { for (i = 0; i < 4; i++) t->grid[i] = g[i]; t->grid[4] = 0; }
}

int js8tx_cfg(js8tx_t *t, const char *s)
{
    static const char keys[] = "tx\0txtest\0txstrat\0txpath\0txf\0txmaxf\0txsess\0txttl\0txjsc\0txterm\0txhb\0txtap\0loc\0";
    const char *k = keys, *v0 = 0;
    int id = 0, v;

    if (lo(s[0]) == 't' && lo(s[1]) == 'x' && s[2] >= '1' && s[2] < '1' + JS8TX_PRESETS && (v0 = kv(s + 3, "")) != 0) {
        char *d = t->preset[s[2] - '1']; int n = 0;
        for (s = v0; *s == ' '; s++) ;
        for (; *s >= 32 && *s <= 126 && n < JS8TX_TEXT; s++) d[n++] = up(*s);
        while (n && d[n - 1] == ' ') n--;
        d[n] = 0;
        return 2;
    }
    while (*k && !(v0 = kv(s, k))) { while (*k) k++; k++; id++; }
    if (!*k) {
        /* not one of ours.  A key that starts with "tx" is meant for the transmitter: a typing error (txtset=1) must
         * not pass silently - it is refused with a note */
        int i = 2;
        if (lo(s[0]) != 't' || lo(s[1]) != 'x') return 0;
        while ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_') i++;
        if (!kv(s + i, "")) return 0;
        t->cfg_err |= 16;
        return 1;
    }
    /* FAIL SAFE (rule 7): a value that is not understood never leaves TX more open than the default.  A bad tx=
     * switches TX off and a bad txtest= switches TXTEST on, both for good (a later good line does not undo it). */
    v = id < 2 || id == 3 || id == 8 || id == 9 || id == 11 ? vbool(v0) : vint(v0);
    switch (id) {
    case 0: if (v < 0) t->cfg_err |= 1; t->enable = (uint8_t)(v > 0 && !(t->cfg_err & 1)); break;
    case 1: if (v < 0) t->cfg_err |= 2; t->test = (uint8_t)(v != 0 || (t->cfg_err & 2)); break;
    case 2: { while (*v0 == ' ') v0++; char c = up(*v0); if ((c == 'A' || c == 'B') && v0[1] <= ' ') t->strat_b = c == 'B'; else t->cfg_err |= 4; break; }
    case 3: if (v) t->cfg_err |= 8; break;             /* txpath=1 (any message on tap grants) no longer exists: see txtap= */
    case 4: if (v == 0 || (v >= 300 && v <= 2900)) t->f_cfg = (uint16_t)v; else t->cfg_err |= 4; break;
    case 5: if (v >= 1 && v <= JS8TX_FMAX) t->maxf = (uint8_t)v; else t->cfg_err |= 4; break;
    case 6: if (v >= 1 && v <= 500) t->sess_cap = (uint16_t)v; else t->cfg_err |= 4; break;
    case 7: if (v >= 30 && v <= 900) t->ttl_s = (uint16_t)v; else t->cfg_err |= 4; break;
    case 8: if (v < 0) t->cfg_err |= 4; else t->use_jsc = (uint8_t)v; break;
    case 9: if (v < 0) t->cfg_err |= 4; t->term = v == 1; break;
    case 10: if (v == 0 || (v >= 10 && v <= 240)) t->hb_min = (uint8_t)v; else t->cfg_err |= 4; break;   /* HB beacon interval, minutes (rule 9) */
    case 11: if (v < 0) t->cfg_err |= 4; else t->tap = (uint8_t)v; break;   /* one-frame messages on the tap credit (rule 1) */
    default: while (*v0 == ' ') v0++; t->grid[0] = 0; js8tx_set_grid(t, v0); break;
    }
    return 1;
}

const char *js8tx_err(int code)
{
    switch (code) {
    case JS8TX_OK:     return "ok";
    case JS8TX_E_OFF:  return "TX is off";
    case JS8TX_E_CALL: return "set MY call";
    case JS8TX_E_GRID: return "set the locator (GPS / loc=)";
    case JS8TX_E_TEXT: return "bad text";
    case JS8TX_E_ENC:  return "text not encodable";
    case JS8TX_E_LONG: return "too many parts";
    case JS8TX_E_FULL: return "queue full";
    case JS8TX_E_SESS: return "session cap reached";
    case JS8TX_E_TEST: return "TXTEST: one heartbeat only";
    case JS8TX_E_NOSEL: return "nothing loaded";
    case JS8TX_E_BUSY: return "a text is waiting";
    case JS8TX_E_ABORT: return "aborted - restart the app";
    default:           return "error";
    }
}

/* how the owner switches the red frame off (the radio: tap the mode tile -> MODE menu -> hold FT8; longer = green) */
#define RED_OFF "red off = MODE > hold FT8 again"

/* what changes what goes on the air: a note (app window, log) AND a DECODE row (U1, 24 Nov 2026) */
static void tell(js8tx_t *t, const char *s) { IO->note(s); IO->say(s, 0); }

/* ---- queue */
static void drop_grant(js8tx_t *t) { if (t->grant) { t->grant = 0; IO->tx_end(); } }

static void q_drop(js8tx_t *t, int i)
{
    if (i == 0) drop_grant(t);
    for (; i + 1 < t->qn; i++) t->q[i] = t->q[i + 1];
    t->qn--;
}

void js8tx_abort(js8tx_t *t, const char *why)
{
    int had = t->qn || t->grant;
    drop_grant(t);
    t->qn = 0;
#ifdef JS8_LINK
    t->lsel[0] = 0; t->tpend[0] = 0;
#endif
    if (had) { t->n_aborts++; if (why) IO->note(why); }
}

static char *item_hb(const js8tx_t *t, char *o, int cq) { o = cp(o, cq ? "CQ CQ CQ " : "HB "); return cp(o, t->grid); }

/* Build and queue.  src JS8TX_SRC_CQ goes to the HEAD of the queue (it is sent on the CQ grant, before anything that
 * waits for the beacon - which cannot be armed at the same time). */
static int req(js8tx_t *t, const char *text, int src)
{
    js8tx_msg_t *m;
    js8_build_t b;
    char hb[16];
    int n, i, len = 0, skip = 0, queued = 0;

    if (!t->enable || t->dead) return JS8TX_E_OFF;
    if (t->owner) return JS8TX_E_ABORT;                /* S2: nothing new while the owner abort is latched */
    if (!t->mycall || !t->mycall[0] || t->mycall[0] == '@' || !js8_call_kind(t->mycall)) return JS8TX_E_CALL;
    if (!t->grid[0]) return JS8TX_E_GRID;
    while (text[len]) { if (text[len] < 32 || text[len] > 126 || len >= JS8TX_TEXT) return JS8TX_E_TEXT; len++; }
    for (i = 0; i < len && text[i] == ' '; i++) ;
    if (i == len || text[i] == '`') return JS8TX_E_TEXT;   /* (a backtick starts JS8Call's raw compound syntax: not built here, so not sent) */
    if (t->test) { item_hb(t, hb, 0); if (src == JS8TX_SRC_CQ || t->test_used || t->qn || !eq(text, hb)) return JS8TX_E_TEST; }
    if (t->qn >= JS8TX_QMAX) return JS8TX_E_FULL;
    for (i = 0; i < t->qn; i++) queued += t->q[i].n - t->q[i].k;
    if (t->sess_frames + queued >= t->sess_cap) return JS8TX_E_SESS;

    m = &t->q[t->qn];                                  /* built in place; it counts only when qn is raised */
    /* the dictionary compresses only when its files are checked (JSCSUM.BIN): one flipped bit in an unchecked
     * encode file would put other words on the air than the ones the operator confirmed (rule 6) */
    b.mycall = t->mycall; b.mygrid = t->grid; b.identify = 1; b.jsc = t->use_jsc && t->jsc && t->jsc->sums ? t->jsc : 0;
    n = js8_build_message_ex(&b, text, m->frames, t->maxf + 2, &skip);
    if (n < 0 || n > t->maxf) return JS8TX_E_LONG;
    if (n == 0 || skip) return JS8TX_E_ENC;
    if (src == JS8TX_SRC_CQ && n != 1) return JS8TX_E_LONG;
    for (i = 0; i < n; i++) if (!js8_msg_crc_ok(m->frames[i])) return JS8TX_E_ENC;
    if (t->sess_frames + queued + n > t->sess_cap) return JS8TX_E_SESS;
    m->n = (uint8_t)n; m->k = 0; m->busy = 0; m->gen = t->gen; m->f0 = 0; m->t_req = IO->ticks(); m->src = (uint8_t)src;
    m->hold = m->bh = (uint8_t)(src != JS8TX_SRC_CQ && t->strat_b && t->tx_any && t->gen_tx == t->gen);   /* strategy B: this arming has had its frame */
    for (i = 0; i < len; i++) m->text[i] = up(text[i]);
    m->text[len] = 0;
    t->qn++;
    if (src == JS8TX_SRC_CQ && t->qn > 1) {            /* to the head */
        js8tx_msg_t x = t->q[t->qn - 1];
        for (i = t->qn - 1; i > 0; i--) t->q[i] = t->q[i - 1];
        t->q[0] = x;
    }
    return JS8TX_OK;
}
int js8tx_request(js8tx_t *t, const char *text) { return req(t, text, JS8TX_SRC_MENU); }

/* ---- the keyboard path: the owner tapped a row, saw the text on the radio's keyboard and pressed ENT */
int js8tx_check(js8tx_t *t, const char *text)
{
    int r = req(t, text, JS8TX_SRC_MENU);              /* every check a real request gets ... */
    if (r) return r;
    t->qn--;                                           /* ... and the entry is taken back at once: nothing is queued */
    return t->q[t->qn].n;
}

/* ask the loader for ONE transmission on the credit of the owner's tap ("@QTX0" / "@QTX1", SDK QSO mode): the next
 * period, unless it starts in less than 1.5 s.  0 = granted (q[0] goes out in that slot), else the loader's answer. */
static int qtx_ask(js8tx_t *t)
{
    char nm[8], b[24];
    int c = IO->cycle_ms(), r, par;
    if (c < 0) return JS8TX_L_BUSY;
    par = c / 15000;
    if (c % 15000 <= 14000) par ^= 1;
    cp(nm, "@QTX0"); nm[4] = (char)('0' + par);
    r = IO->vfile(nm, b, (int)sizeof b);
    r = r > 0 ? fld(b, "r", -99) : -99;
    if (r >= 0 && r < 31000) { t->grant = 1; t->t_slot = IO->ticks() + (uint32_t)r; return 0; }
    if (r >= 0) IO->tx_end();                          /* S4: a grant the app does not use is handed back at once */
    return r < 0 ? r : -99;
}

/* nothing to send (review 24 Nov 2026): shorter than 2 characters, or one word that is a callsign (the untouched "<CALL> "
 * of a tap on a station's row) */
static int nothing(const char *s)
{
    char w[JS8_CALL_MAX];
    int n = 0, e;
    while (*s == ' ') s++;
    for (e = 0; s[e]; e++) ;
    while (e && s[e - 1] == ' ') e--;
    if (e < 2) return 1;
    if (e >= JS8_CALL_MAX) return 0;
    for (; n < e; n++) { if (s[n] == ' ') return 0; w[n] = up(s[n]); }
    w[n] = 0;
    return js8_call_kind(w) != 0;
}
int js8tx_send(js8tx_t *t, const char *text, int *granted)
{
    char l[48], *q;
    int r, i, n, s = 0;
    *granted = 0;
    if (t->owner) { t->n_refused++; IO->say("TX: aborted - restart the app to send", 0); return JS8TX_E_ABORT; }   /* S2 */
    if (nothing(text)) { t->n_refused++; IO->say("TX: nothing to send - type a message", 0); return JS8TX_E_EMPTY; }
    /* ONE waiting message (review 24 Nov 2026): this ENT replaces every message of the owner that waits - also one that has
     * sent a frame already, and also when this text is then refused.  The CQ frame of the green frame is not his ENT's. */
    for (i = 0; i < t->qn; i++) if (t->q[i].src != JS8TX_SRC_CQ) { s |= t->q[i].k ? 2 : 1; q_drop(t, i--); }
    r = req(t, text, JS8TX_SRC_MENU);
    if (r) {
        if (s) IO->say(s & 2 ? "TX: dropped the message in progress" : "TX: replaced the waiting message", 0);
        t->n_refused++; q = cp(l, "TX refused: "); cp(q, js8tx_err(r)); IO->say(l, 0); return r;
    }
    n = t->q[t->qn - 1].n;
    for (i = 0; t->prep && i < t->prep_n; i++) if (eq(t->prep[i], t->q[t->qn - 1].text)) t->prep[i][0] = 0;   /* a prepared reply is used once */
    /* (a) ONE frame, nothing else waiting, not armed: the loader's tap credit (the tap on the row) sends it - no arming */
    if (t->tap && n == 1 && t->qn == 1 && !t->armed && !t->cq && !t->grant && qtx_ask(t) == 0) {
        *granted = 1;
        IO->say(s ? "TX replaced: on air at the next slot" : "TX: on air at the next slot (tap)", 0);
        return n;
    }
    /* (b) everything else waits for the red frame.  ONE row says how many frames will go (U5): n is the whole queue now (the
     * replaced message is gone; a CQ frame is never waiting together with a beacon message).  On the screen a frame is a
     * "part" ("frame" there is the red / green frame of the radio: tap the mode tile -> MODE menu -> hold FT8 = red):
     *   "3 parts: MODE > hold FT8 (red), 2 min" (txttl) / "replaced: MODE > hold FT8 (red), 3 parts" /
     *   armed: "TX queued: sending 3 parts" / "TX replaced: sending 3 parts"          (all <= 40 characters) */
    if (t->armed) { q = cp(l, s ? "TX replaced: sending " : "TX queued: sending "); q = un(q, (unsigned)n); }
    else if (s) { q = cp(l, n < 10 ? "replaced: MODE > hold FT8 (red), " : "replaced: MODE > hold FT8 red, "); q = un(q, (unsigned)n); }
    else q = un(l, (unsigned)n);
    q = cp(q, n == 1 ? " part" : " parts");
    if (!t->armed && !s) {
        q = cp(q, ": MODE > hold FT8 (red), ");
        q = un(q, t->ttl_s % 60u ? t->ttl_s : t->ttl_s / 60u); cp(q, t->ttl_s % 60u ? " s" : " min");
    }
    IO->say(l, 0);
    return n;
}

/* ---- the transmit items the terminal can load by index ("p"): 0 HB, 1 CQ, 2..5 presets tx1..tx4, 6.. prepared replies */
#define NITEMS(t) (2 + JS8TX_PRESETS + ((t)->prep ? (t)->prep_n : 0))
int js8tx_item(const js8tx_t *t, int i, char *o)
{
    *o = 0;
    if (!t->enable || t->dead || !t->grid[0] || i < 0 || i >= NITEMS(t)) return 0;
    if (i < 2) { if (t->test && i) return 0; item_hb(t, o, i); return 1; }
    if (t->test) return 0;
    if (i < 2 + JS8TX_PRESETS) { cp(o, t->preset[i - 2]); return o[0] != 0; }
    cpn(o, t->prep[i - 2 - JS8TX_PRESETS], JS8TX_PREP_W - 1);   /* a prepared reply */
    return o[0] != 0;
}

/* ---- audio offset */
void js8tx_heard(js8tx_t *t, int f)
{
    int i, o = 0;
    if (f <= 0 || f > 4000) return;
    for (i = 0; i < JS8TX_HEARD; i++) {
        int d = (int)t->heard[i].f - f;
        if (t->heard[i].age && d > -4 && d < 4) { o = i; break; }
        if (t->heard[i].age < t->heard[o].age) o = i;
    }
    t->heard[o].f = (uint16_t)f; t->heard[o].age = 8;
}
void js8tx_period(js8tx_t *t) { for (int i = 0; i < JS8TX_HEARD; i++) if (t->heard[i].age) t->heard[i].age--; }

int js8tx_pick_offset(const js8tx_t *t)
{
    int c, i, best = JS8TX_F_DEFAULT, bs = -1;
    if (t->f_cfg) return t->f_cfg;
    for (c = JS8TX_F_MIN + 100; c <= JS8TX_F_MAX - 50; c += 50) {      /* 600 .. 2400 Hz */
        int d = 200, s;
        for (i = 0; i < JS8TX_HEARD; i++) if (t->heard[i].age) {
            int x = c - (int)t->heard[i].f; if (x < 0) x = -x;
            if (x < d) d = x;
        }
        s = d * 4096 - (c > JS8TX_F_DEFAULT ? c - JS8TX_F_DEFAULT : JS8TX_F_DEFAULT - c);   /* free first, then near the middle */
        if (s > bs) { bs = s; best = c; }
    }
    return best;
}

int js8tx_wave(txs_mfsk_t *w, uint8_t tones[79], const uint8_t frame[JS8_FRAME_BYTES], int f0_hz)
{
    js8_encode(frame, js8_costas_normal, tones);
    return txs_mfsk_init(w, tones, JS8_NN, JS8A_SPS, 1, (double)f0_hz, 6.25, JS8_NTONES, 0, 0, 0);
}

/* ---- loader state */
static void cq_armed(js8tx_t *t)                       /* a new arming of CQ (green frame): sends NOTHING (rule 8) */
{
    /* Owner decision 24 Nov 2026: no CQ.  The green frame is not used by the app; it queues nothing and asks for no
     * grant.  (The radio itself shows the green frame while a one-part reply on the tap credit is on the air - that
     * is not an arming the app sees here.)  The CQ path below (JS8TX_SRC_CQ, @QTXC) stays unused. */
    if (!t->enable || t->dead) return;
    tell(t, "green not used - hold FT8 again");
}
static void poll_tx(js8tx_t *t, int quiet)
{
    char b[112];
    int n = IO->vfile("@TX", b, (int)sizeof b), armed = t->armed, owner = t->owner, cq = t->cq, i, had = 0;
    t->have_tx = n > 0;
    /* A read that fails (a transient NOFD, ...) changes NOTHING: armed / owner / cq keep their last known values.
     * Otherwise the next good read would look like a new arming and release a held message (rule 4). */
    if (n > 0) {
        owner = fld(b, "owner", 0) > 0;
        armed = fld(b, "beacon", 0) > 0;
        if (!armed) t->seen0 = 1, t->ab0 = 0;
        else if (!t->seen0) {
            /* armed before this app ever saw the beacon disarmed: that is not the owner's setting in the MODE menu (the loader arms
             * by itself at app start when the app's .CFG - for a file named FT8.APP: FT8.CFG - has beacon=1) */
            armed = 0;
            if (!quiet && !t->said_stale) { t->said_stale = 1; tell(t, "TX: red frame at start - off, on again"); }
        }
        if (owner) { armed = 0; if (!t->owner) t->ab0 = 1; }
        else if (t->ab0) armed = 0;                    /* O4: an owner abort that clears with the beacon still armed is no new arming */
        t->gate = (int8_t)fld(b, "gate", -1); t->clk = (int8_t)fld(b, "clk", -1);
    }
    if (!t->cq_off) {                                  /* the green frame: "@QSO" cq= */
        n = IO->vfile("@QSO", b, (int)sizeof b);
        if (n == JS8TX_L_NAME) { t->cq_off = 1; cq = 0; }
        else if (n > 0) {
            cq = fld(b, "cq", 0) > 0;
            if (!cq) t->cq_seen0 = 1;
            else if (!t->cq_seen0) cq = 0;            /* armed before the app saw it disarmed: does not count */
        }
    }
    if (owner) cq = 0;
    if (owner && !t->owner) { if (t->qn || t->grant) IO->say("TX: owner abort, queue cleared", 0); js8tx_abort(t, "TX: owner abort, queue cleared"); }
    if (armed && !t->armed) {                          /* a new arming; HB only on one made with the queue empty (rule 9) and not */
        uint32_t now = IO->ticks();                    /* just after a message was lost - that arming was meant for it (U2) */
        t->gen++; t->arm_new = 1; t->hb_said = 0;
        t->hb_arm = !t->qn && !(t->t_lost && now - t->t_lost < JS8TX_LOST_MS);
        t->t_lost = 0;
    }
    if (!armed && t->armed && !t->in_run) {
        /* disarmed outside a frame: nothing that was waiting for this arming may go on at a later one (rule 4).  Every message
         * of the owner is dropped - also one held after a PERM (S1, 24 Nov 2026) and a strategy-B hold: confirm, arm, disarm =
         * nothing further. */
        for (i = 0; i < t->qn; i++) if (t->q[i].src != JS8TX_SRC_CQ) { q_drop(t, i--); had = 1; }
        if (had) { IO->note("TX: disarmed, waiting messages dropped"); IO->say("TX: red frame off - message dropped", 0); t->t_lost = IO->ticks() | 1u; }
    }
    t->armed = (uint8_t)armed; t->owner = (uint8_t)owner;
    if (!cq && t->cq && !t->in_run) {                  /* CQ disarmed: its frame is not sent */
        for (i = 0, had = 0; i < t->qn; i++) if (t->q[i].src == JS8TX_SRC_CQ) { q_drop(t, i--); had = 1; }
        if (had) tell(t, "TX: green frame off - CQ dropped");
    }
    if (cq && !t->cq) { t->cq = 1; if (!t->in_run) cq_armed(t); }   /* (an arming seen during a frame queues nothing) */
    t->cq = (uint8_t)cq;
    if (t->qn && t->q[0].hold == 1 && t->q[0].gen != t->gen) t->q[0].hold = 0;
}

static int sendable(const js8tx_t *t)                  /* the beacon path */
{
    return t->qn && t->armed && !t->owner && t->q[0].src != JS8TX_SRC_CQ && t->q[0].hold != 1 && (int32_t)(IO->ticks() - t->t_next) >= 0;
}

static void drop_head(js8tx_t *t, const char *why) { q_drop(t, 0); IO->note(why); }

/* The CQ frame at the head: ask the loader's CQ path for the next period, once per period, never less than
 * JS8TX_CQ_ASK_MS before its slot (the audio must be ready about 1 s before the period starts). */
static void cq_ask(js8tx_t *t)
{
    char b[24];
    js8tx_msg_t *m = &t->q[0];
    int c = IO->cycle_ms(), r;
    if (c < 0 || (int)(15500 - c % 15000) % 15000 < JS8TX_CQ_ASK_MS) return;   /* clock unknown / too close to the slot */
    r = IO->vfile("@QTXC", b, (int)sizeof b);
    r = r > 0 ? fld(b, "r", -99) : r;
    if (r >= 0 && r < 31000) { t->grant = 1; t->t_slot = IO->ticks() + (uint32_t)r; return; }
    if (r >= 0) IO->tx_end();                          /* S4: a grant the app does not use is handed back at once */
    t->t_next = IO->ticks() + 13000u;                  /* one try per period */
    if (r == JS8TX_L_BUSY && ++m->busy < JS8TX_BUSY_TRIES) { tell(t, t->clk == 0 ? "TX wait: set the radio clock" : "TX wait: radio not ready"); return; }
    q_drop(t, 0); tell(t, r == JS8TX_L_BUSY ? "TX gave up: radio not ready" : "TX: CQ not granted by the radio");
}

int js8tx_poll(js8tx_t *t)
{
    uint32_t now = IO->ticks();
    int c, d, i;
    char *q;
    if (t->cfg_err) {                                  /* what js8tx_cfg refused (it cannot write notes itself) */
        if (t->cfg_err & 1) IO->note("JS8.CFG: bad tx= value, TX is OFF");
        if (t->cfg_err & 2) IO->note("JS8.CFG: bad txtest= value, TXTEST on");
        if (t->cfg_err & 4) IO->note("JS8.CFG: bad tx* value, default kept");
        if (t->cfg_err & 8) IO->note("JS8.CFG: txpath is gone (see txtap)");
        if (t->cfg_err & 16) IO->note("JS8.CFG: unknown tx* key, ignored");
        t->cfg_err = 0;
    }
    if (!t->enable || t->dead) return -1;
    if (now - t->t_tx >= JS8TX_TXPOLL_MS) {
        char l[24 + JS8TX_TEXT];
        t->t_tx = now;
        poll_tx(t, 0);
        /* HB BEACON (rule 9): arming the beacon with txhb= set is the owner's request for a heartbeat now and then every
         * hb_min minutes.  Only HB, only while armed (seen during this run), only with an empty queue, never in TXTEST. */
        if (t->hb_min && t->armed && t->hb_arm && !t->owner && !t->qn && !t->test && (!t->t_hb || now - t->t_hb >= (uint32_t)t->hb_min * 60000u)) {
            char h[12]; item_hb(t, h, 0);
            t->t_hb = now ? now : 1u;
            if (req(t, h, JS8TX_SRC_HB) == JS8TX_OK) {
                IO->note("TX beacon: HB queued");
                if (!t->hb_said) { t->hb_said = 1; q = un(cp(l, "TX: HB every "), t->hb_min); cp(q, " min"); IO->say(l, 0); IO->say(RED_OFF, 0); }
            } else tell(t, "TX beacon: HB refused");
        }
        if (t->arm_new && !t->in_run) {                /* U4: an arming that sends nothing is said so */
            t->arm_new = 0;
            if (!t->qn && t->armed) {
                if (t->hb_min && t->hb_arm && !t->test && !t->owner) {   /* (a re-arming within txhb minutes of the last HB) */
                    uint32_t m = (uint32_t)t->hb_min - (now - t->t_hb) / 60000u;
                    q = un(cp(l, "TX: next HB in "), m); cp(q, " min"); IO->say(l, 0); IO->say(RED_OFF, 0);
                } else tell(t, "TX: red frame on, nothing to send");
            }
        }
        for (i = 0; i < t->qn; i++) if (now - t->q[i].t_req > (uint32_t)t->ttl_s * 1000u) {
            int own = t->q[i].src != JS8TX_SRC_CQ && t->q[i].src != JS8TX_SRC_HB;
            cpn(cp(l, "TX expired - nothing waits: "), t->q[i].text, 12);   /* (40 characters: 28 + 12) */
            q_drop(t, i--);
            tell(t, l);
            if (own) t->t_lost = now | 1u;
        }
#ifdef JS8_LINK
        if (t->tpend[0] && now - t->t_tpend > JS8TX_SEL_MS) { t->tpend[0] = 0; IO->say("TX: terminal text expired", 0); }
#endif
        if (t->grant && (int32_t)(now - t->t_slot) > 300) {    /* the granted slot went by unused: the beacon path is left */
            drop_grant(t);
            tell(t, "TX: slot missed, MODE > hold FT8 (red)");
        }
    }
    if (!t->grant && t->qn && t->q[0].src == JS8TX_SRC_CQ && t->cq && !t->owner && (int32_t)(now - t->t_next) >= 0) cq_ask(t);
    if (t->grant) { d = (int32_t)(t->t_slot - JS8TX_EARLY_MS - now); return d > 0 ? d : 0; }
    if (!sendable(t)) return -1;
    c = IO->cycle_ms();
    if (c < 0) return -1;                              /* radio clock not known: nothing is sent */
    d = 15500 - JS8TX_LEAD_MS - c % 15000;             /* tx_begin JS8TX_LEAD_MS before the slot start (period edge + 0.5 s) */
    if (d < -600) d += 15000;                          /* too late for this slot: the next one */
    return d > 0 ? d : 0;
}

static char *kofn(char *q, const js8tx_msg_t *m) { q = un(q, m->k + 1u); *q++ = '/'; return un(q, m->n); }

/* Between tx_begin and the end of the frame: is this transmission still wanted?  The app does not rely on the loader
 * failing the write after a disarm or an owner abort (rule 4).  "@TX" / "@QSO" are virtual files: no SD access. */
static int tx_lost(js8tx_t *t, int tapped, int cq)   /* (tapped: a tap grant - the loader enforces its own rules, the owner abort is ours) */
{
    poll_tx(t, 0);
    return t->owner || (cq ? !t->cq : !tapped && !t->armed);
}

int js8tx_run(js8tx_t *t)
{
    static const char hx[] = "0123456789abcdef";
    js8tx_msg_t *m = &t->q[0];
    char l[JS8TX_LOG_LINE], *q;
    int r, got, w, rc = 0, tapped = 0, started = 0, i, cq;
    uint32_t total = 0, t0, tp;

    if (!t->qn || t->dead || !t->enable) { drop_grant(t); return 0; }
    if (IO->stop()) { js8tx_abort(t, "TX: stop, queue cleared"); return 0; }
    if (t->sess_frames >= t->sess_cap) { js8tx_abort(t, "TX: session cap, queue cleared"); return 0; }
    cq = m->src == JS8TX_SRC_CQ;
    if (!t->grant) {
        if (cq) return 0;                              /* a CQ frame goes out only on its grant */
        poll_tx(t, 0);                                 /* fresh "@TX": still armed, no owner abort */
        if (!sendable(t)) return 0;
    }
    if (!m->f0) m->f0 = (uint16_t)js8tx_pick_offset(t);
    if (js8tx_wave(&t->wave, t->tones, m->frames[m->k], m->f0)) { drop_head(t, "TX: encoder error, message dropped"); return 0; }
    if (t->grant) { t0 = t->t_slot; t->grant = 0; tapped = 1; }
    else {
        t->n_begin++;
        r = IO->tx_begin();
        if (r == JS8TX_L_PERM) {                       /* not armed after all / the loader's own limit: wait for a NEW arming */
            m->hold = 1; m->bh = 0; m->gen = t->gen;
            q = kofn(cp(l, "TX refused: part "), m); cp(q, " - red frame again");   /* (<= 40 characters: 12/12) */
            tell(t, l);
            return 0;
        }
        /* radio not ready, or the loader's slot is further away than this app asked for (its slot clock and ours
         * differ): the grant is given back at once - the app never waits many seconds on a keyed grant, where a
         * disarm could be missed.  One try per slot, a few slots. */
        if (r == JS8TX_L_BUSY || r > JS8TX_BEGIN_MAX) {
            if (r > 0) IO->tx_end();
            if (++m->busy >= JS8TX_BUSY_TRIES) { q_drop(t, 0); tell(t, "TX gave up: radio not ready"); return 0; }
            t->t_next = IO->ticks() + 13000u;
            poll_tx(t, 0);
            tell(t, r > 0 ? "TX wait: slot clock differs" : t->clk == 0 ? "TX wait: set the radio clock" : t->gate == 7 ? "TX wait: USB-D + DATA MOD=WLAN" :
                     t->gate == 11 ? "TX wait: LAN session open" : "TX wait: radio not ready");
            return 0;
        }
        if (r < 0) { t->dead = 1; js8tx_abort(t, 0); IO->note("TX not available in this firmware"); return 0; }
        t0 = IO->ticks() + (uint32_t)r;
    }
    q = cp(l, "TX "); q = un(q, m->f0); *q++ = ' '; q = kofn(q, m); *q++ = ' '; cpn(q, m->text, JS8TX_TEXT);
    IO->say(l, 1);                                     /* the shell: "HHMM " + this, cut to the 40 characters of a DECODE row; the terminal gets it whole */
    /* rc: 0 the frame went out whole, 1 stop request, 2 disarmed / owner abort seen in "@TX", 3 the owner tapped a row before
     * the slot of a tap grant (S3), < 0 the loader's write error */
    t->in_run = 1;
    tp = IO->ticks();
    while (!rc && (int32_t)(t0 - JS8TX_WRITE_MS - IO->ticks()) > 0) {
        if (IO->stop()) rc = 1;
        else if (tapped && IO->cancel && IO->cancel()) rc = 3;   /* (tapped: a tap or CQ grant, not yet keyed) */
        else if (IO->ticks() - tp >= JS8TX_TXPOLL_MS) { tp = IO->ticks(); if (tx_lost(t, tapped, cq)) rc = 2; }
        else IO->wait(20);
    }
    while (!rc) {
        got = txs_mfsk_fill(&t->wave, t->buf, JS8TX_CHUNK);
        if (got <= 0) break;
        started = 1;
        if (IO->stop()) { rc = 1; break; }
        if (IO->ticks() - tp >= JS8TX_TXPOLL_MS) { tp = IO->ticks(); if (tx_lost(t, tapped, cq)) { rc = 2; break; } }
        w = IO->tx_write(t->buf, got);
        if (w > 0) total += (uint32_t)w;
        if (w != got) { rc = w < 0 ? w : -1; break; }
    }
    IO->tx_end();
    t->in_run = 0;
    if (total) { t->sess_frames++; if (t->test) t->test_used = 1; }
    /* log record: T <f0> <k/n> <samples> <rc> <hex22> <text>.  Also for a frame that was announced ("TX <f0> k/n ..."
     * row, "T S" to the terminal) and then never got audio: 0 samples.  (m still points at the message even if
     * poll_tx has cleared the queue: clearing only sets the count.) */
    q = cp(l, "T "); q = un(q, m->f0); *q++ = ' '; q = kofn(q, m); *q++ = ' '; q = un(q, total); *q++ = ' ';
    if (rc < 0) { *q++ = '-'; q = un(q, (unsigned)-rc); } else q = un(q, (unsigned)rc);
    *q++ = ' ';
    for (i = 0; i < JS8_FRAME_BYTES; i++) { *q++ = hx[m->frames[m->k][i] >> 4]; *q++ = hx[m->frames[m->k][i] & 15]; }
    *q++ = ' '; cpn(q, m->text, JS8TX_TEXT);
    IO->log(l);
    if (rc) {
        if (started) t->n_frames_cut++;
        if (rc != 2) poll_tx(t, 0);
        /* poll_tx has cleared the queue (and said so) if the loader reports an owner abort; otherwise: */
        if (rc == 2 && t->qn && !t->owner) {          /* (U1: a disarm that cut a message is shown on the DECODE screen too) */
            IO->say(cq ? "TX: green frame off - CQ dropped" : "TX: red frame off - message dropped", 0);
            if (!cq) t->t_lost = IO->ticks() | 1u;
        }
        if (rc == 3) IO->say("TX: cancelled before the slot", 0);
        js8tx_abort(t, rc == 2 ? "TX: disarmed, queue cleared" : rc == 3 ? "TX: cancelled before the slot" : rc != 1 ? "TX: write failed, queue cleared" :
                       started ? "TX: stopped, queue cleared" : "TX: stop before the slot, queue cleared");
        return total > 0;
    }
    t->n_frames_ok++;
    m->k++; m->t_req = IO->ticks(); m->busy = 0;
    if (!tapped) { t->tx_any = 1; t->gen_tx = t->gen; }
    if (m->k >= m->n) {
        t->last_src = m->src;
        /* 24 Nov 2026 on the radio: after a confirmed message the loader showed a fresh arming and HB started at once; an owner
         * message that just went out blocks the HB start like a dropped one (U2) */
        if (m->src != JS8TX_SRC_HB && m->src != JS8TX_SRC_CQ) t->t_lost = IO->ticks() | 1u;
        /* the DECODE screen (owner decision 24 Nov 2026): "SENT: <text>" (cut to the 40 characters of a row), after an HB
         * of txhb= "HB SENT, next in N min"; taps are not delivered while the beacon is armed, so then also
         * "red frame off to tap again".  The note "TX done: <text>" (app window + log) stays. */
        char d[44];
        if (m->src == JS8TX_SRC_HB) { q = un(cp(d, "HB SENT, next in "), t->hb_min); cp(q, " min"); }
        else { q = cpn(cp(d, "SENT: "), m->text, 40 - 6); while (q[-1] == ' ') *--q = 0; }   /* (the text has no trailing blank) */
        q = cp(l, "TX done: "); cpn(q, m->text, 30); drop_head(t, l);   /* (m is the next message after this) */
        IO->say(d, 0);
        if (t->armed) IO->say("red frame off to tap again", 0);
    }
    /* strategy B = one frame per arming: whatever is at the head of the queue now - the rest of this message or the
     * next message - waits; since 24 Nov 2026 a disarm drops it (confirm, arm, disarm = nothing further), so under
     * strategy B only the first frame of a message goes out */
    if (t->strat_b && !tapped && t->qn && !m->hold && m->src != JS8TX_SRC_CQ) {
        m->hold = m->bh = 1; m->gen = t->gen;
        q = cp(l, "TX B: held, red frame off drops "); kofn(q, m);
        IO->say(l, 0); IO->note(l);
    }
    return total > 0;
}

void js8tx_status(const js8tx_t *t, char *o)
{
    o = cp(o, t->test ? "TXTEST " : "TX ");
    if (!t->enable) { cp(o, "off (tx=0)"); return; }
    if (t->dead) { cp(o, "n/a"); return; }
    o = cp(o, t->owner ? "ABORTED" : t->armed ? "ARMED" : t->cq ? "GREEN unused" : t->have_tx ? "disarmed" : "no @TX");
    if (t->qn) {
        const js8tx_msg_t *m = &t->q[0];
        o = cp(o, " q"); o = un(o, t->qn); *o++ = ' '; o = kofn(o, m);
        o = cp(o, m->hold == 1 ? " NEEDARM" : "");
    }
    o = cp(o, " s"); o = un(o, t->sess_frames); *o++ = '/'; o = un(o, t->sess_cap);
    if (t->hb_min && !t->test) { o = cp(o, " HB"); un(o, t->hb_min); }   /* the HB beacon (txhb=) */
}

#ifdef JS8_LINK
/* ---- the terminal path */
int js8tx_select(js8tx_t *t, const char *text)
{
    int r;
    t->lsel[0] = 0;
    for (r = 0; text[r]; r++) if (r >= JS8TX_KBD) return JS8TX_E_TEXT;   /* over the keyboard's 43 characters: never offered */
    if (pfx(text, "NEXT ")) return JS8TX_E_TEXT;       /* (the tap-grant prompt of this module) */
    r = js8tx_check(t, text);
    if (r <= 0) return r;
    cp(t->lsel, t->q[t->qn].text); t->t_lsel = IO->ticks();
    return r;
}
/* GO: the loaded text is handed to the radio - the owner's next tap on a row opens the keyboard with it, and only ENT there
 * sends it.  Never over a text already waiting there. */
int js8tx_confirm(js8tx_t *t)
{
    int r;
    if (!t->lsel[0]) return JS8TX_E_NOSEL;
    if (IO->ticks() - t->t_lsel > JS8TX_SEL_MS) { t->lsel[0] = 0; return JS8TX_E_NOSEL; }
    if (t->tpend[0]) return JS8TX_E_BUSY;
    r = js8tx_check(t, t->lsel);
    if (r > 0) { cp(t->tpend, t->q[t->qn].text); t->t_tpend = IO->ticks(); }
    else t->n_refused++;
    t->lsel[0] = 0;
    return r;
}
#endif
