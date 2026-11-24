/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8asm.c - reassembly of JS8 frames into messages; see js8asm.h.
 */
#include "js8asm.h"

static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void s_copy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static int s_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int word_at(const char *s, int i, const char *w, int wl)
{
    int k;
    for (k = 0; k < wl; k++) if (s[i + k] != w[k]) return 0;
    return 1;
}

static int is_callch(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/';
}

void js8asm_init(js8asm_t *a, const char *mycall, const char *groups,
                 js8asm_emit_t emit, void *user)
{
    int i;
    a->emit = emit; a->user = user;
    s_copy(a->mycall, mycall ? mycall : "", JS8_CALL_MAX);
    s_copy(a->groups, groups ? groups : "@ALLCALL", JS8ASM_GROUPS_MAX);
    for (i = 0; i < JS8ASM_STREAMS; i++) { a->s[i].used = 0; a->s[i].len = 0; }
    a->pool_len = 0;
}

/* Radio::base_callsign, simplified: the longest part between slashes */
static void base_call(const char *call, char *out)
{
    int i = 0, best = 0, bl = 0, st = 0;
    for (;; i++) {
        if (call[i] == '/' || call[i] == 0) {
            if (i - st > bl) { bl = i - st; best = st; }
            st = i + 1;
            if (!call[i]) break;
        }
    }
    for (i = 0; i < bl && i < JS8_CALL_MAX - 1; i++) out[i] = call[best + i];
    out[i] = 0;
}

int js8asm_to_me(const js8asm_t *a, const char *to)
{
    char b[JS8_CALL_MAX];
    int i = 0, tl = s_len(to);

    if (!to[0]) return 0;
    if (a->mycall[0]) {
        if (s_eq(to, a->mycall)) return JS8ASM_F_TO_ME;
        base_call(a->mycall, b);
        if (to[0] != '@' && s_eq(to, b)) return JS8ASM_F_TO_ME;
    }
    if (to[0] != '@') return 0;
    while (a->groups[i]) {
        int st;
        while (a->groups[i] == ' ') i++;
        st = i;
        while (a->groups[i] && a->groups[i] != ' ') i++;
        if (i - st == tl && word_at(a->groups, st, to, tl)) return JS8ASM_F_TO_GROUP;
    }
    return 0;
}

/* ---- text pool: every stream's text is one contiguous piece of a->pool ---- */

/* shorten the text of s to n characters; the pieces behind it move down */
static void cut(js8asm_t *a, js8asm_stream_t *s, int n)
{
    int d = (int)s->len - n, from, i;
    if (d <= 0) return;
    from = s->off + s->len;
    for (i = from; i < (int)a->pool_len; i++) a->pool[i - d] = a->pool[i];
    for (i = 0; i < JS8ASM_STREAMS; i++)
        if (&a->s[i] != s && a->s[i].len > 0 && (int)a->s[i].off >= from) a->s[i].off = (uint16_t)(a->s[i].off - d);
    a->pool_len = (uint16_t)(a->pool_len - d);
    s->len = (uint16_t)n;
}

static void append(js8asm_t *a, js8asm_stream_t *s, const char *t)
{
    int k = s_len(t), at, i;
    if (k > JS8ASM_TEXT_MAX - 1 - (int)s->len) { k = JS8ASM_TEXT_MAX - 1 - (int)s->len; s->trunc = 1; }
    if (k > JS8ASM_POOL - (int)a->pool_len) { k = JS8ASM_POOL - (int)a->pool_len; s->trunc = 1; }
    if (k <= 0) return;
    if (s->len == 0) s->off = a->pool_len;                   /* a new piece starts at the end */
    at = s->off + s->len;
    for (i = (int)a->pool_len - 1; i >= at; i--) a->pool[i + k] = a->pool[i];
    for (i = 0; i < JS8ASM_STREAMS; i++)
        if (&a->s[i] != s && a->s[i].len > 0 && (int)a->s[i].off >= at) a->s[i].off = (uint16_t)(a->s[i].off + k);
    for (i = 0; i < k; i++) a->pool[at + i] = t[i];
    a->pool_len = (uint16_t)(a->pool_len + k);
    s->len = (uint16_t)(s->len + k);
}

static void finish(js8asm_t *a, js8asm_stream_t *s, int timeout)
{
    js8asm_msg_t *m = &a->out;
    int n = s->len, i, ml;

    /* the text leaves the pool; the stream is free from here on */
    for (i = 0; i < n; i++) m->text[i] = a->pool[s->off + i];
    cut(a, s, 0);
    m->freq = s->m.freq; m->snr = s->m.snr; m->cmd = s->m.cmd; m->flags = s->m.flags; m->frames = s->m.frames;
    m->t_first = s->m.t_first; m->t_last = s->m.t_last;
    s_copy(m->from, s->m.from, JS8_CALL_MAX); s_copy(m->to, s->m.to, JS8_CALL_MAX);

    while (n > 0 && m->text[n - 1] == ' ') n--;              /* rstrip */
    m->text[n] = 0;
    if (timeout) m->flags |= JS8ASM_F_TIMEOUT;
    if (!s->got_first) m->flags |= JS8ASM_F_PARTIAL;
    if (s->trunc) m->flags |= JS8ASM_F_TRUNC;

    /* checksummed command with text: "<body> XXX" */
    if (s->has_hdr && js8_cmd_checksum(m->cmd) && n > s->hdr_len && !timeout && !s->trunc) {
        int b = s->hdr_len, bl;
        char ck[3];
        while (b < n && m->text[b] == ' ') b++;              /* lstrip */
        bl = n - b;
        m->flags |= JS8ASM_F_CRC_BAD;
        if (bl >= 4) {
            js8_checksum16(m->text + b, bl - 4, ck);
            if (m->text[n - 4] == ' ' && ck[0] == m->text[n - 3] && ck[1] == m->text[n - 2] &&
                ck[2] == m->text[n - 1]) {
                m->flags = (uint8_t)((m->flags & ~JS8ASM_F_CRC_BAD) | JS8ASM_F_CRC_OK);
                n -= 4;
                m->text[n] = 0;
            }
        }
    }
    /* 24 Nov 2026 on the radio: a MSG whose checksum did not verify showed its 3 checksum characters ("... SCOTLAND. ZG1").
     * The flag stays (log 'x', DECODE marker '?'); the checksum-looking tail is taken off the text. */
    if ((m->flags & JS8ASM_F_CRC_BAD) && n >= 5 && m->text[n - 4] == ' ') {
        static const char ab[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?";
        int ok = 1, j, k;
        for (j = n - 3; j < n && ok; j++) { for (k = 0; ab[k] && ab[k] != m->text[j]; k++) ; ok = ab[k] != 0; }
        if (ok) { n -= 4; m->text[n] = 0; }
    }
    /* printable from here on (the checksum above is over the text as sent) */
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)m->text[i];
        if (ch == '\n' || ch == '\r' || ch == '\t') m->text[i] = ' ';
        else if (ch < 32 || ch == 127) m->text[i] = '?';
    }
    while (n > 0 && m->text[n - 1] == ' ') n--;
    m->text[n] = 0;
    m->flags |= (uint8_t)js8asm_to_me(a, m->to);
    ml = s_len(a->mycall);
    if (ml) {
        for (i = s->has_hdr ? s->hdr_len : 0; i + ml <= n; i++) {
            if (word_at(m->text, i, a->mycall, ml) && (i == 0 || !is_callch(m->text[i - 1])) &&
                !is_callch(m->text[i + ml])) { m->flags |= JS8ASM_F_MENTION; break; }
        }
    }
    s->used = 0;
    if (n > 0 && a->emit) a->emit(a->user, m);
}

static js8asm_stream_t *find(js8asm_t *a, int freq)
{
    js8asm_stream_t *best = 0;
    int i, bd = JS8ASM_RANGE_HZ + 1;
    for (i = 0; i < JS8ASM_STREAMS; i++) {
        int d;
        if (!a->s[i].used) continue;
        d = a->s[i].m.freq - freq;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = &a->s[i]; }
    }
    return best;
}

static js8asm_stream_t *alloc(js8asm_t *a, int freq, uint32_t now)
{
    js8asm_stream_t *s = 0;
    int i;
    for (i = 0; i < JS8ASM_STREAMS; i++) if (!a->s[i].used) { s = &a->s[i]; break; }
    if (!s) {                                                /* table full: close the oldest */
        s = &a->s[0];
        for (i = 1; i < JS8ASM_STREAMS; i++)
            if ((uint32_t)(now - a->s[i].m.t_last) > (uint32_t)(now - s->m.t_last)) s = &a->s[i];
        finish(a, s, 1);
    }
    s->used = 1; s->has_hdr = 0; s->got_first = 0; s->has_cmpd = 0;
    s->hdr_len = 0; s->len = 0; s->off = 0; s->trunc = 0; s->cmpd[0] = 0;
    s->m.freq = (int16_t)freq; s->m.snr = 127; s->m.cmd = -1; s->m.flags = 0; s->m.frames = 0;
    s->m.t_first = now; s->m.t_last = now;
    s->m.from[0] = 0; s->m.to[0] = 0;
    return s;
}

static void header(js8asm_t *a, js8asm_stream_t *s, const char *from, const char *to, int cmd,
                   int has_num, int num)
{
    char h[64];
    s_copy(s->m.from, from, JS8_CALL_MAX);
    s_copy(s->m.to, to, JS8_CALL_MAX);
    s->m.cmd = (int8_t)cmd;
    /* free-text command (31) is just the blank between the call and the text */
    js8_format_directed(from, to, cmd == 31 ? -1 : cmd, has_num, num, h, (int)sizeof h);
    cut(a, s, 0);
    append(a, s, h);
    append(a, s, " ");
    s->hdr_len = (uint8_t)s->len;                            /* h is at most 63 characters */
    s->has_hdr = 1;
}

void js8asm_feed(js8asm_t *a, const js8_msg_t *f, int freq, int snr, uint32_t now)
{
    js8asm_stream_t *s;
    int first = (f->i3 & JS8_I3_FIRST) != 0, last = (f->i3 & JS8_I3_LAST) != 0;
    int hdr_type = f->type <= JS8_FT_DIRECTED;               /* heartbeat, compound, directed */
    int has_cmpd = 0;
    char cmpd[JS8_CALL_MAX];
    const char *from;

    if (f->type == JS8_FT_UNKNOWN) return;
    cmpd[0] = 0;
    s = find(a, freq);
    if (s && (first || (hdr_type && s->len > 0))) {
        /* a new message begins on this frequency: close what is buffered there
         * (JS8Call clears the buffer on a First frame) */
        if (!first && s->has_cmpd) { has_cmpd = 1; s_copy(cmpd, s->cmpd, JS8_CALL_MAX); }
        if (s->len > 0) finish(a, s, 1);
        s->used = 0;
        s = 0;
    }
    if (!s) {
        s = alloc(a, freq, now);
        s->got_first = (uint8_t)first;
        s->has_cmpd = (uint8_t)has_cmpd;
        s_copy(s->cmpd, cmpd, JS8_CALL_MAX);
    }
    s->m.freq = (int16_t)freq;                               /* follow drift */
    s->m.t_last = now;
    if (snr < -128) snr = -128;
    if (snr < s->m.snr) s->m.snr = (int8_t)snr;
    if (s->m.frames < 255) s->m.frames++;

    switch (f->type) {
    case JS8_FT_HEARTBEAT:                                   /* "CALL: @HB HEARTBEAT JO70 " */
        s_copy(s->m.from, f->from, JS8_CALL_MAX);
        s_copy(s->m.to, f->to, JS8_CALL_MAX);
        append(a, s, f->text);
        {
            int n = s->len;
            while (n > 0 && a->pool[s->off + n - 1] == ' ') n--;
            cut(a, s, n);
        }
        append(a, s, " ");
        s->hdr_len = (uint8_t)(s->len > 255 ? 255 : s->len); s->has_hdr = 1;
        break;
    case JS8_FT_COMPOUND:                                    /* "CALL: " - the sender's full call */
        s_copy(s->cmpd, f->from, JS8_CALL_MAX);
        s->has_cmpd = 1;
        if (last) {                                          /* nothing follows: show it */
            s_copy(s->m.from, f->from, JS8_CALL_MAX);
            append(a, s, f->from); append(a, s, ":");
            if (f->grid[0]) { append(a, s, " "); append(a, s, f->grid); }
        }
        break;
    case JS8_FT_COMPOUND_DIRECTED:
    case JS8_FT_DIRECTED:
        from = f->from;
        if (s_eq(from, "<....>") && s->has_cmpd) from = s->cmpd;
        header(a, s, from, f->to, f->cmd, f->has_num, f->num);
        break;
    default: {                                               /* data frames */
        const char *t = f->text;
        /* a compound-call frame followed by data instead of a directed frame (the directed frame was not
         * decoded, or the sender only identified): JS8Call shows "CALL: " and then the text */
        if (s->has_cmpd && !s->has_hdr && s->len == 0) {
            s_copy(s->m.from, s->cmpd, JS8_CALL_MAX);
            append(a, s, s->cmpd); append(a, s, ": ");
            s->hdr_len = (uint8_t)s->len; s->has_hdr = 1;
        }
        /* the header already ends in one blank (JS8Call would show two after CQ/HB) */
        if (s->has_hdr && s->len == s->hdr_len) while (*t == ' ') t++;
        append(a, s, t);
        break;
    }
    }
    if (last) finish(a, s, 0);
}

void js8asm_tick(js8asm_t *a, uint32_t now)
{
    int i;
    for (i = 0; i < JS8ASM_STREAMS; i++) {
        js8asm_stream_t *s = &a->s[i];
        uint32_t age;
        if (!s->used) continue;
        age = now - s->m.t_last;
        if (s->len > 0) { if (age > JS8ASM_TIMEOUT_S) finish(a, s, 1); }
        else if (age > JS8ASM_DROP_S) s->used = 0;
    }
}
