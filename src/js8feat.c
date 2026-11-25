/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8feat.c - heard table, inbox, band activity, window views, prepared replies, ADIF record.
 * See js8feat.h.  Nothing in this file transmits or touches a file.
 */
#include "js8feat.h"

static char *cp(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char *cpn(char *d, const char *s, int max) { while (*s && max-- > 0) *d++ = *s++; *d = 0; return d; }
static int eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static char *un(char *d, unsigned v, int w)              /* decimal, at least w digits ('0' padded) */
{
    char b[10]; int n = 0;
    do { b[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n < w) b[n++] = '0';
    while (n) *d++ = b[--n];
    *d = 0; return d;
}
static char *sn(char *d, int v)                          /* "+05", "-11" */
{
    if (v < -30) v = -30; else if (v > 31) v = 31;
    *d++ = v < 0 ? '-' : '+';
    return un(d, (unsigned)(v < 0 ? -v : v), 2);
}
static char *pad(char *d, const char *start, int col) { while (d - start < col) *d++ = ' '; *d = 0; return d; }
static int call_ok(const char *c)                        /* a station, not a group / hashed / empty call */
{
    int n = 0, dig = 0;
    for (; c[n]; n++) {
        if (c[n] >= '0' && c[n] <= '9') dig = 1;
        else if (!((c[n] >= 'A' && c[n] <= 'Z') || c[n] == '/')) return 0;
    }
    return n >= 3 && n < JS8_CALL_MAX && dig;
}

static void printable(char *s, int size)                 /* ends within size, printable ASCII only */
{
    int i;
    s[size - 1] = 0;
    for (i = 0; s[i]; i++) if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) s[i] = '?';
}

void js8f_init(js8feat_t *f, const char *mycall)
{
    int i;
    f->mycall = mycall;
    /* the kept part may come from a state file: make sure every string ends and every index is in range */
    if (f->k.view >= JS8F_NVIEWS) f->k.view = 0;
    if (f->k.in_n > JS8F_INBOX) f->k.in_n = 0;
    for (i = 0; i < JS8F_HEARD; i++) { printable(f->k.heard[i].call, JS8_CALL_MAX); printable(f->k.heard[i].grid, 5); }
    for (i = 0, f->k.unread = 0; i < JS8F_INBOX; i++) {
        printable(f->k.in[i].text, JS8F_ITEXT);
        if (i < f->k.in_n && (f->k.in[i].fl & JS8F_I_NEW)) f->k.unread++;
    }
}

uint32_t js8f_time(uint32_t ymd, int sod)
{
    uint32_t y = ymd / 10000u, m = ymd / 100u % 100u, d = ymd % 100u;
    if (m < 3) { y--; m += 12; }                         /* days since 2000-03-01 (valid 2000 .. 2099) */
    return ((y - 2000u) * 365u + (y - 2000u) / 4u + (153u * (m - 3u) + 2u) / 5u + d - 1u) * 86400u + (uint32_t)sod;
}

js8f_heard_t *js8f_find(js8feat_t *f, const char *call)
{
    int i;
    for (i = 0; i < JS8F_HEARD; i++) if (f->k.heard[i].call[0] && eq(f->k.heard[i].call, call)) return &f->k.heard[i];
    return 0;
}

void js8f_frame(js8feat_t *f, const js8_msg_t *m, int snr, int freq, uint32_t now)
{
    js8f_heard_t *h;
    int i;
    if (!call_ok(m->from) || eq(m->from, f->mycall)) return;
    h = js8f_find(f, m->from);
    if (!h) {                                            /* a free entry, else the one heard longest ago */
        h = &f->k.heard[0];
        for (i = 0; i < JS8F_HEARD; i++) {
            if (!f->k.heard[i].call[0]) { h = &f->k.heard[i]; break; }
            if ((int32_t)(f->k.heard[i].t - h->t) < 0) h = &f->k.heard[i];
        }
        cp(h->call, m->from); h->grid[0] = 0; h->rcvd = h->sent = 127;
    }
    if (m->grid[0]) cpn(h->grid, m->grid, 4);
    h->snr = (int8_t)(snr < -99 ? -99 : snr > 99 ? 99 : snr);
    h->freq = (uint16_t)(freq < 0 ? 0 : freq > 9999 ? 9999 : freq);
    h->t = now;
    if (m->has_num && js8_cmd_is_snr(m->cmd) && eq(m->to, f->mycall)) h->rcvd = m->num;
}

void js8f_sent(js8feat_t *f, const char *text)
{
    char c[JS8_CALL_MAX];
    js8f_heard_t *h;
    int i;
    for (i = 0; i < JS8_CALL_MAX - 1 && text[i] && text[i] != ' '; i++) c[i] = text[i];
    c[i] = 0;
    h = js8f_find(f, c);
    for (; h && text[i]; i++)
        if (text[i] == ' ' && text[i + 1] == 'S' && text[i + 2] == 'N' && text[i + 3] == 'R' && text[i + 4] == ' '
            && (text[i + 5] == '-' || text[i + 5] == '+') && text[i + 6] >= '0' && text[i + 6] <= '9' && text[i + 7] >= '0' && text[i + 7] <= '9') {
            int v = (text[i + 6] - '0') * 10 + (text[i + 7] - '0');
            h->sent = (int8_t)(text[i + 5] == '-' ? -v : v);
            return;
        }
}

/* the k-th most recently heard station (k = 0: the newest), or 0 */
static const js8f_heard_t *heard_rank(const js8feat_t *f, int k)
{
    int i, j;
    for (i = 0; i < JS8F_HEARD; i++) {
        const js8f_heard_t *e = &f->k.heard[i];
        int r = 0;
        if (!e->call[0]) continue;
        for (j = 0; j < JS8F_HEARD; j++) {
            const js8f_heard_t *o = &f->k.heard[j];
            if (o->call[0] && j != i && ((int32_t)(o->t - e->t) > 0 || (o->t == e->t && j < i))) r++;
        }
        if (r == k) return e;
    }
    return 0;
}

/* the answer to a query, as text for the operator to confirm; 1 = prepared */
static int prepare(js8feat_t *f, const js8asm_msg_t *m, const char *mygrid, uint32_t now)
{
    char t[JS8F_REPLY + 1 + JS8_CALL_MAX + JS8F_INFO], *q;
    int i, o = -1, fr = -1, old = 0, k;

    q = cp(t, m->from);
    switch (m->cmd) {
    case 0:  q = cp(q, " SNR "); sn(q, m->snr); break;
    case 4:  if (!mygrid[0]) return 0; q = cp(q, " GRID "); cpn(q, mygrid, 4); break;
    case 16: if (!f->info[0]) return 0; q = cp(q, " INFO "); cp(q, f->info); break;
    case 6:  q = cp(q, " STATUS "); cp(q, f->status[0] ? f->status : "QRV"); break;
    case 3:
        q = cp(q, " HEARING");
        for (i = 0, k = 0; k < 4; i++) {
            const js8f_heard_t *h = heard_rank(f, i);
            if (!h) break;
            if (eq(h->call, m->from)) continue;
            if ((int)(q - t) + 1 + len(h->call) > JS8F_REPLY_ROW) break;   /* whole calls only: the answer is one DECODE row */
            *q++ = ' '; q = cp(q, h->call); k++;
        }
        if (!k) return 0;
        break;
    default: return 0;
    }
    t[JS8F_REPLY_ROW] = 0;                               /* "SEND? <answer>" must fit one 40-character row (info= / status= are cut) */
    for (i = len(t); i && t[i - 1] == ' '; i--) t[i - 1] = 0;
    for (i = 0; i < JS8F_PREP; i++) {                    /* the same answer is offered once; else a free / the oldest entry */
        if (!f->prep[i][0]) { if (fr < 0) fr = i; }
        else if (eq(f->prep[i], t)) o = i;
        else if ((int32_t)(f->prep_t[i] - f->prep_t[old]) < 0) old = i;
    }
    if (o < 0) o = fr >= 0 ? fr : old;
    cp(f->prep[o], t); f->prep_t[o] = now;
    return 1;
}

int js8f_message(js8feat_t *f, const js8asm_msg_t *m, const char *mygrid, uint32_t now)
{
    js8f_in_t *e;
    int i, r = 1, bad = (m->flags & (JS8ASM_F_CRC_BAD | JS8ASM_F_TIMEOUT | JS8ASM_F_PARTIAL | JS8ASM_F_TRUNC)) != 0;

    if (!(m->flags & (JS8ASM_F_TO_ME | JS8ASM_F_TO_GROUP))) return 0;
    if (!(m->flags & JS8ASM_F_TO_ME)) {                  /* to a group: not the calls and heartbeats everybody sends to @ALLCALL / @HB */
        const char *p = m->text;
        while (*p && *p != ' ') p++;                     /* "FROM:" */
        if (*p) p++;
        while (*p && *p != ' ') p++;                     /* "@GROUP" */
        if (!p[0] || !p[1] || (p[1] == 'C' && p[2] == 'Q') || (p[1] == 'H' && (p[2] == 'B' || p[2] == 'E'))) return 0;
        r = 3;
    }
    for (i = JS8F_INBOX - 1; i > 0; i--) f->k.in[i] = f->k.in[i - 1];
    if (f->k.in_n < JS8F_INBOX) f->k.in_n++;
    e = &f->k.in[0];
    e->t = now; e->freq = m->freq; e->snr = m->snr;
    e->fl = (uint8_t)(JS8F_I_NEW | ((m->flags & JS8ASM_F_TO_ME) ? 0 : JS8F_I_GROUP) | (bad ? JS8F_I_BAD : 0));
    for (i = 0; i < JS8F_ITEXT - 1 && m->text[i]; i++) { unsigned char u = (unsigned char)m->text[i]; e->text[i] = (u < 32 || u > 126) ? '?' : (char)u; }
    e->text[i] = 0;
    for (i = 0, f->k.unread = 0; i < f->k.in_n; i++) if (f->k.in[i].fl & JS8F_I_NEW) f->k.unread++;
    if ((m->flags & JS8ASM_F_TO_ME) && !bad && call_ok(m->from) && prepare(f, m, mygrid, now)) r = 2;
    return r;
}

const char *js8f_last_reply(const js8feat_t *f)
{
    int i, o = -1;
    for (i = 0; i < JS8F_PREP; i++) if (f->prep[i][0] && (o < 0 || (int32_t)(f->prep_t[i] - f->prep_t[o]) >= 0)) o = i;
    return o < 0 ? 0 : f->prep[o];
}

void js8f_tick(js8feat_t *f, uint32_t now)
{
    int i;
    for (i = 0; i < JS8F_PREP; i++) if (f->prep[i][0] && now - f->prep_t[i] > JS8F_PREP_TTL) f->prep[i][0] = 0;
}

void js8f_band(js8feat_t *f, const char *l, int freq)
{
    char *d = f->band[f->band_pos], *q = d;
    int i;
    /* "HHMM TEXT" (the first row of a decoded text on the DECODE screen, <= 40 characters) + its audio offset
     * -> "HHMM FFFF TEXT", cut to JS8F_W */
    for (i = 0; i < 4 && l[i]; i++) *q++ = l[i];
    if (len(l) > 5) {
        unsigned v = freq < 0 ? 0u : freq > 9999 ? 9999u : (unsigned)freq;
        *q++ = ' ';
        for (i = 1000; i > 1 && v < (unsigned)i; i /= 10) *q++ = ' ';
        q = un(q, v, 1);
        for (i = 4; l[i] && q - d < JS8F_W; i++) *q++ = l[i];
    }
    *q = 0;
    f->band_pos = (uint8_t)((f->band_pos + 1) % JS8F_BAND);
    if (f->band_n < JS8F_BAND) f->band_n++;
}

void js8f_read(js8feat_t *f)
{
    int i;
    for (i = 0; i < f->k.in_n; i++) f->k.in[i].fl &= (uint8_t)~JS8F_I_NEW;
    f->k.unread = 0;
}

const char *js8f_view_name(int v)
{
    static const char n[JS8F_NVIEWS][8] = { "STATUS", "BAND", "INBOX", "HEARD" };
    return n[v >= 0 && v < JS8F_NVIEWS ? v : 0];
}

static char *hhmm(char *q, uint32_t t) { q = un(q, t % 86400u / 3600u, 2); return un(q, t % 3600u / 60u, 2); }

int js8f_row(const js8feat_t *f, int view, int row, uint32_t now, char *out)
{
    char *q = out;
    int n;

    *out = 0;
    if (row < 0) return 0;
    if (row == 0) {
        q = cp(q, "JS8 "); q = cp(q, js8f_view_name(view));
        if (view == JS8F_V_INBOX) { *q++ = ' '; q = un(q, f->k.in_n, 1); }
        if (view == JS8F_V_HEARD) { for (n = 0; heard_rank(f, n); n++) ; *q++ = ' '; q = un(q, (unsigned)n, 1); }
        if (f->k.unread) { q = cp(q, " | new "); q = un(q, f->k.unread, 1); }
        if (f->mycall && f->mycall[0]) { q = cp(q, " | "); cpn(q, f->mycall, JS8F_W - (int)(q - out)); }
        return 1;
    }
    row--;
    if (view == JS8F_V_BAND) {
        if (row >= f->band_n) return 0;
        cp(out, f->band[(f->band_pos + JS8F_BAND - f->band_n + row) % JS8F_BAND]);
        return 1;
    }
    if (view == JS8F_V_INBOX) {                          /* one row per message, two when its text is longer than 34 characters */
        const js8f_in_t *e;
        int i;
        for (i = 0; i < f->k.in_n; i++) {
            n = len(f->k.in[i].text) > 34 ? 2 : 1;
            if (row < n) break;
            row -= n;
        }
        if (i >= f->k.in_n) return 0;
        e = &f->k.in[i];
        if (row) { q = pad(q, out, 6); cpn(q, e->text + 34, 34); return 1; }
        *q++ = (e->fl & JS8F_I_NEW) ? '*' : (e->fl & JS8F_I_BAD) ? '!' : (e->fl & JS8F_I_GROUP) ? 'g' : ' ';
        q = hhmm(q, e->t); *q++ = ' '; cpn(q, e->text, 34);
        return 1;
    }
    if (view == JS8F_V_HEARD) {
        const js8f_heard_t *h = heard_rank(f, row);
        uint32_t age;
        if (!h) return 0;
        q = cp(q, h->call); q = pad(q, out, 12);
        q = cp(q, h->grid); q = pad(q, out, 17);
        q = sn(q, h->snr); *q++ = ' ';
        n = h->freq; if (n < 1000) *q++ = ' '; if (n < 100) *q++ = ' '; if (n < 10) *q++ = ' ';
        q = un(q, (unsigned)n, 1); *q++ = ' ';
        age = now - h->t; if ((int32_t)age < 0) age = 0;
        if (age < 6000u) { q = un(q, age / 60u, 1); *q++ = 'm'; }
        else if (age < 360000u) { q = un(q, age / 3600u, 1); *q++ = 'h'; }
        else { q = un(q, age / 86400u, 1); *q++ = 'd'; }
        if (h->rcvd != 127) { q = cp(q, " r"); q = sn(q, h->rcvd); }
        *q = 0;
        return 1;
    }
    return 0;
}

static char *fld(char *q, const char *name, const char *v)   /* "<NAME:n>value " */
{
    *q++ = '<'; q = cp(q, name); *q++ = ':'; q = un(q, (unsigned)len(v), 1); *q++ = '>'; q = cp(q, v); *q++ = ' ';
    *q = 0; return q;
}

int js8f_adif(js8feat_t *f, const char *call, uint32_t ymd, int sod, uint32_t hz, const char *mygrid, char *out)
{
    char v[16], *q = out;
    const js8f_heard_t *h = js8f_find(f, call);
    if (!call_ok(call)) { *out = 0; return 0; }
    q = fld(q, "CALL", call);
    un(v, ymd, 8); q = fld(q, "QSO_DATE", v);
    un(un(un(v, (unsigned)sod / 3600u, 2), (unsigned)sod / 60u % 60u, 2), (unsigned)sod % 60u, 2); q = fld(q, "TIME_ON", v);
    q = fld(q, "MODE", "MFSK"); q = fld(q, "SUBMODE", "JS8");
    if (hz) { char *w = un(v, hz / 1000000u, 1); *w++ = '.'; un(w, hz % 1000000u, 6); q = fld(q, "FREQ", v); }
    if (h && h->sent != 127) { sn(v, h->sent); q = fld(q, "RST_SENT", v); }
    if (h && h->rcvd != 127) { sn(v, h->rcvd); q = fld(q, "RST_RCVD", v); }
    if (h && h->grid[0]) q = fld(q, "GRIDSQUARE", h->grid);
    if (f->mycall && f->mycall[0]) q = fld(q, "STATION_CALLSIGN", f->mycall);
    if (mygrid && mygrid[0]) q = fld(q, "MY_GRIDSQUARE", mygrid);
    q = cp(q, "<EOR>");
    return (int)(q - out);
}
