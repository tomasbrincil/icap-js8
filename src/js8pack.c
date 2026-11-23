/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8pack.c - text -> JS8 frames; see js8pack.h.  Re-implemented from
 * JS8Call varicode.cpp / jsc.cpp ((C) 2018 Jordan Sherer KN4CRD, GPL-3.0-or-later).
 *
 * Deliberate differences from JS8Call:
 *   - input that JS8Call could not encode at all (it would loop forever) is
 *     skipped one character at a time;
 *   - without the JSC files (jsc == NULL) Huffman is used for as many leading
 *     characters as are encodable, instead of all-or-nothing;
 *   - the raw "`CALL ..." compound syntax is not parsed; compound calls are
 *     handled through the directed path as JS8Call does.
 */
#include "js8pack.h"

static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int is_up(int c)    { return c >= 'A' && c <= 'Z'; }
static int is_dig(int c)   { return c >= '0' && c <= '9'; }
static int is_alnum(int c) { return is_up(c) || is_dig(c); }
static int is_word(int c)  { return is_alnum(c) || (c >= 'a' && c <= 'z') || c == '_'; }
static int is_sp(int c)    { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static int idx(int c)
{
    int i;
    for (i = 0; i < 39; i++) if (js8_alnum[i] == c) return i;
    return -1;
}

static int starts(const char *s, const char *p)
{
    int n = 0;
    while (p[n]) { if (s[n] != p[n]) return 0; n++; }
    return n ? n : -1;
}

static void put(uint8_t *f, int pos, int n, uint32_t v)
{
    while (n-- > 0) {
        if ((v >> n) & 1u) f[pos >> 3] |= (uint8_t)(0x80u >> (pos & 7));
        pos++;
    }
}

static void clr(uint8_t *f) { int i; for (i = 0; i < JS8_FRAME_BYTES; i++) f[i] = 0; }

static void finish(uint8_t *f, int i3)
{
    f[9] = (uint8_t)((i3 & 7) << 5);
    f[10] = 0;
    js8_msg_crc_set(f);
}

/* ---- callsigns and grid ---------------------------------------------------- */

static int std_pattern(const char *s, int n)
{
    /* ^[0-9A-Z]?[0-9A-Z][0-9][A-Z]{0,3}$ */
    int lead, i;
    for (lead = 1; lead >= 0; lead--) {
        if (n < lead + 2 || n > lead + 5) continue;
        if (lead && !is_alnum(s[0])) continue;
        if (!is_alnum(s[lead]) || !is_dig(s[lead + 1])) continue;
        for (i = lead + 2; i < n; i++) if (!is_up(s[i])) break;
        if (i == n) return 1;
    }
    return 0;
}

static int adjacency(const char *s, int n)
{
    int i;
    for (i = 0; i + 1 < n; i++)
        if ((is_dig(s[i]) && is_up(s[i + 1])) || (is_up(s[i]) && is_dig(s[i + 1]))) return 1;
    return 0;
}

int js8_call_kind(const char *call)
{
    int n = s_len(call), b = n, i, slashes = 0, first_slash = -1;

    if (js8_group_find(call)) return 1;
    if (n >= 2 && call[n - 2] == '/' && call[n - 1] == 'P') b = n - 2;
    if (std_pattern(call, b) || std_pattern(call, n))
        return (n > 2 && adjacency(call, n)) ? 1 : 0;
    /* compound: optional '@', then 1..11 of [A-Z0-9/], ending in a word character */
    i = call[0] == '@' ? 1 : 0;
    if (n - i < 1 || n - i > 11 || !is_alnum(call[n - 1])) return 0;
    for (; i < n; i++) {
        if (call[i] == '/') { slashes++; if (first_slash < 0) first_slash = i; }
        else if (!is_alnum(call[i])) return 0;
    }
    if (n - slashes > 9) return 0;
    if (first_slash >= 0) {
        char t[JS8_CALL_MAX];
        for (i = 0; i < first_slash && i < JS8_CALL_MAX - 1; i++) t[i] = call[i];
        t[i] = 0;
        return js8_group_find(t) ? 0 : 2;
    }
    if (call[0] == '@') return 2;
    return (n > 2 && adjacency(call, n)) ? 2 : 0;
}

int js8_call28_pack(const char *call, uint32_t *v, int *portable)
{
    char c[10], m[6];
    int n = s_len(call), g, i, lead;

    *portable = 0;
    g = js8_group_find(call);
    if (g) { *v = JS8_NBASECALL + (uint32_t)g; return 1; }
    if (n >= 2 && call[n - 2] == '/' && call[n - 1] == 'P') { n -= 2; *portable = 1; }
    if (n > 8) return 0;
    for (i = 0; i < n; i++) c[i] = call[i];
    if (n >= 4 && c[0] == '3' && c[1] == 'D' && c[2] == 'A' && c[3] == '0') {
        for (i = 3; i + 1 < n; i++) c[i] = c[i + 1];           /* 3DA0x -> 3D0x */
        c[2] = '0'; n--;
    }
    if (n >= 3 && c[0] == '3' && c[1] == 'X' && is_up(c[2])) {
        for (i = 1; i + 1 < n; i++) c[i] = c[i + 1];           /* 3Xa.. -> Qa.. */
        c[0] = 'Q'; n--;
    }
    if (n < 2 || n > 6) return 0;
    for (lead = 1; lead >= 0; lead--) {
        uint32_t p;
        if (n + lead > 6) continue;
        for (i = 0; i < 6; i++) m[i] = ' ';
        for (i = 0; i < n; i++) m[lead + i] = c[i];
        if (!(is_alnum(m[0]) || m[0] == ' ') || !is_alnum(m[1]) || !is_dig(m[2])) continue;
        if (!(is_up(m[3]) || m[3] == ' ') || !(is_up(m[4]) || m[4] == ' ') ||
            !(is_up(m[5]) || m[5] == ' ')) continue;
        p = (uint32_t)idx(m[0]);
        p = 36u * p + (uint32_t)idx(m[1]);
        p = 10u * p + (uint32_t)idx(m[2]);
        p = 27u * p + (uint32_t)idx(m[3]) - 10u;
        p = 27u * p + (uint32_t)idx(m[4]) - 10u;
        p = 27u * p + (uint32_t)idx(m[5]) - 10u;
        if (p == 0) return 0;
        *v = p;
        return 1;
    }
    return 0;
}

static void limb_muladd(uint16_t l[4], unsigned mul, unsigned add)
{
    uint32_t carry = add;
    int i;
    for (i = 0; i < 4; i++) {
        uint32_t cur = (uint32_t)l[i] * mul + carry;
        l[i] = (uint16_t)cur;
        carry = cur >> 16;
    }
}

int js8_call50_pack(const char *call, uint16_t l[4])
{
    static const uint8_t radix[11] = { 39, 38, 38, 2, 38, 38, 38, 2, 38, 38, 38 };
    char w[16];
    int n = 0, i;

    for (i = 0; call[i] && n < 13; i++)
        if (idx(call[i]) >= 0) w[n++] = call[i];             /* [A-Z0-9 /@] only */
    if (n > 3 && w[3] != '/') { for (i = n; i > 3; i--) w[i] = w[i - 1]; w[3] = ' '; n++; }
    if (n > 7 && w[7] != '/') { for (i = n; i > 7; i--) w[i] = w[i - 1]; w[7] = ' '; n++; }
    while (n < 11) w[n++] = ' ';
    l[0] = l[1] = l[2] = l[3] = 0;
    for (i = 0; i < 11; i++) {
        unsigned d = (i == 3 || i == 7) ? (unsigned)(w[i] == '/') : (unsigned)idx(w[i]);
        limb_muladd(l, radix[i], d);
    }
    return (l[0] | l[1] | l[2] | l[3]) != 0;
}

unsigned js8_grid_pack(const char *g)
{
    unsigned k, m;
    if (s_len(g) < 4 || g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' ||
        !is_dig(g[2]) || !is_dig(g[3])) return JS8_NMAXGRID;
    k = (unsigned)(g[0] - 'A') * 10u + (unsigned)(g[2] - '0');
    m = (unsigned)(g[1] - 'A') * 10u + (unsigned)(g[3] - '0');
    return (179u - k) * 180u + m;
}

/* ---- single frames --------------------------------------------------------- */

static int compound_frame(const char *call, unsigned type, unsigned num, unsigned bits3,
                          int i3, uint8_t *f)
{
    uint16_t l[4];
    if (!js8_call50_pack(call, l)) return 0;
    clr(f);
    put(f, 0, 3, type);
    put(f, 3, 2, l[3]); put(f, 5, 16, l[2]); put(f, 21, 16, l[1]); put(f, 37, 16, l[0]);
    put(f, 53, 16, num);
    put(f, 69, 3, bits3);
    finish(f, i3);
    return 1;
}

int js8_pack_heartbeat(const char *mycall, const char *grid, int cq, int bits3,
                       int i3, uint8_t out[JS8_FRAME_BYTES])
{
    unsigned num = grid ? js8_grid_pack(grid) : JS8_NMAXGRID;
    if (!mycall || !mycall[0]) return 0;
    if (cq) num |= 0x8000u;
    return compound_frame(mycall, JS8_FT_HEARTBEAT, num, (unsigned)bits3 & 7u, i3, out);
}

int js8_pack_compound(const char *call, const char *grid, int i3, uint8_t out[JS8_FRAME_BYTES])
{
    return compound_frame(call, JS8_FT_COMPOUND, grid ? js8_grid_pack(grid) : JS8_NMAXGRID, 0, i3, out);
}

static unsigned pack_num(int has_num, int num)
{
    if (!has_num) return 0;
    if (num < -30) num = -30;
    if (num > 31) num = 31;
    return (unsigned)(num + 31);
}

int js8_pack_compound_directed(const char *call, int cmd, int has_num, int num,
                               int i3, uint8_t out[JS8_FRAME_BYTES])
{
    unsigned v;
    if (cmd < 0 || cmd > 31) return 0;
    if (js8_cmd_is_snr(cmd)) v = ((2u | (unsigned)(cmd == 29)) << 6) + (pack_num(has_num, num) & 63u);
    else v = (unsigned)cmd & 127u;
    return compound_frame(call, JS8_FT_COMPOUND_DIRECTED, JS8_NUSERGRID + v, 0, i3, out);
}

int js8_pack_directed(const char *from, const char *to, int cmd, int has_num, int num,
                      int i3, uint8_t out[JS8_FRAME_BYTES])
{
    uint32_t pf, pt;
    int porf, port;
    if (cmd < 0 || cmd > 31) return 0;
    if (!js8_call28_pack(from, &pf, &porf) || !js8_call28_pack(to, &pt, &port)) return 0;
    clr(out);
    put(out, 0, 3, JS8_FT_DIRECTED);
    put(out, 3, 28, pf);
    put(out, 31, 28, pt);
    put(out, 59, 5, (uint32_t)cmd);
    put(out, 64, 1, (uint32_t)porf);
    put(out, 65, 1, (uint32_t)port);
    put(out, 66, 6, pack_num(has_num, num));
    finish(out, i3);
    return 1;
}

/* ---- data frames ----------------------------------------------------------- */

static const js8_huff_t *huff_find(int c)
{
    int k;
    for (k = 0; k < 44; k++) if (js8_huff[k].c == c) return &js8_huff[k];
    return 0;
}

static void pad(uint8_t *f, int nbits)
{
    int i;
    for (i = nbits + 1; i < 72; i++) put(f, i, 1, 1);        /* one 0, then 1s */
}

static int pack_huff(const char *t, int len, int strict, uint8_t *f)
{
    int nb = 2, i;
    clr(f);
    put(f, 0, 2, 2);                                         /* {1,0} */
    if (strict) for (i = 0; i < len; i++) if (!huff_find(t[i])) return 0;
    for (i = 0; i < len; i++) {
        const js8_huff_t *h = huff_find(t[i]);
        if (!h || nb + h->len >= 72) break;
        put(f, nb, h->len, h->code);
        nb += h->len;
    }
    pad(f, nb);
    return i;
}

static int pack_jsc(const char *t, int len, js8jsc_t *jsc, uint8_t *f)
{
    int nb = 2, p = 0;
    clr(f);
    put(f, 0, 2, 3);                                         /* {1,1} */
    while (p < len) {
        uint32_t index, x;
        int adv, sep = 0, wl, cl, r, k;
        uint8_t dig[8];
        if (t[p] == ' ') {
            /* empty word that is not the last one: the blank itself */
            r = js8jsc_lookup(jsc, " ", 1, &index, &adv);
        } else {
            for (wl = 0; p + wl < len && t[p + wl] != ' '; wl++) ;
            r = js8jsc_lookup(jsc, t + p, wl, &index, &adv);
            if (r == 1 && adv == wl && p + wl < len) sep = 1;   /* swallow the blank */
        }
        if (r != 1 || adv <= 0) break;
        k = 0;
        dig[k++] = (uint8_t)(index % 7u);
        x = index / 7u;
        while (x > 0) { x -= 1; dig[k++] = (uint8_t)(x % 9u + 7u); x /= 9u; }
        cl = 4 * k + 1;
        if (nb + cl >= 72) break;
        while (k-- > 1) { put(f, nb, 4, dig[k]); nb += 4; }
        put(f, nb, 5, ((uint32_t)dig[0] << 1) | (uint32_t)sep); nb += 5;
        p += adv + sep;
    }
    pad(f, nb);
    return p;
}

int js8_pack_data(const char *text, int len, js8jsc_t *jsc, int i3, uint8_t out[JS8_FRAME_BYTES])
{
    uint8_t hf[JS8_FRAME_BYTES], cf[JS8_FRAME_BYTES];
    int hn, cn = 0, i;

    hn = pack_huff(text, len, jsc != 0, hf);
    if (jsc) cn = pack_jsc(text, len, jsc, cf);
    if (hn > cn) { for (i = 0; i < 9; i++) out[i] = hf[i]; cn = hn; }
    else         { for (i = 0; i < 9; i++) out[i] = cf[i]; }
    if (cn == 0) return 0;
    finish(out, i3);
    return cn;
}

/* ---- message builder ------------------------------------------------------- */

/* heartbeat_re; returns matched length, fills *cq, *b3, grid[5] */
static int parse_hb(const char *s, int *cq, int *b3, char *grid)
{
    static const char types[] =
        "CQ CQ CQ\0CQ DX\0CQ QRP\0CQ CONTEST\0CQ FIELD\0CQ FD\0CQ CQ\0CQ\0HB\0HEARTBEAT\0";
    const char *t = types;
    int p = 0, k, n;

    while (is_sp(s[p])) p++;
    n = starts(s + p, "@ALLCALL");
    if (n < 0) n = starts(s + p, "@HB");
    if (n > 0) {
        if (!is_sp(s[p + n])) return 0;
        p += n;
        while (is_sp(s[p])) p++;
    }
    for (k = 0; k < 10; k++, t += s_len(t) + 1) {
        int e;
        n = starts(s + p, t);
        if (n <= 0) continue;
        e = p + n;
        if (k == 9) {                                        /* HEARTBEAT(?!\s+SNR) */
            int q = e;
            while (is_sp(s[q])) q++;
            if (q > e && starts(s + q, "SNR") > 0) continue;
        }
        *cq = k < 8; *b3 = k < 8 ? k : 0; grid[0] = 0;
        if (is_sp(s[e]) && s[e + 1] >= 'A' && s[e + 1] <= 'R' && s[e + 2] >= 'A' && s[e + 2] <= 'R' &&
            is_dig(s[e + 3]) && is_dig(s[e + 4]) && !is_word(s[e + 5])) {
            for (n = 0; n < 4; n++) grid[n] = s[e + 1 + n];
            grid[4] = 0;
            return e + 5;
        }
        if (!is_word(s[e])) return e;                        /* \b */
    }
    return 0;
}

/* optional_cmd_pattern at s; returns matched length (0 = none), *code;
 * *bare = 1 for the one-character forms "?" and ">" (key without a blank) */
static int parse_cmd(const char *s, int *code, int *bare)
{
    /* regex alternatives in order; '$' = needs (?=[ ]|$) */
    static const char alt[] =
        "AGN?\0QSL?\0HW CPY?\0MSG TO:\0SNR?\0INFO?\0GRID?\0STATUS?\0QUERY MSGS?\0HEARING?\0"
        "$STATUS\0$HEARING\0$QUERY CALL\0$QUERY MSGS\0$QUERY\0$CMD\0$MSG\0$NACK\0$ACK\0$73\0$YES\0"
        "$NO\0$HEARTBEAT SNR\0$SNR\0$QSL\0$RR\0$SK\0$FB\0$INFO\0$GRID\0$DIT DIT\0";
    static const int8_t codes[31] = {
        30, 22, 19, 10, 0, 16, 4, 6, 12, 3,
        7, 8, 13, 12, 11, 24, 9, 2, 14, 28, 27,
        26, 29, 25, 23, 21, 20, 18, 17, 15, 1 };
    int sp, k;

    *bare = 0;
    for (sp = is_sp(s[0]) ? 1 : 0; sp >= 0; sp--) {
        const char *a = alt;
        for (k = 0; k < 31; k++, a += s_len(a) + 1) {
            int la = a[0] == '$', n = starts(s + sp, a + la);
            if (n <= 0) continue;
            if (la && s[sp + n] != ' ' && s[sp + n] != 0) continue;
            *code = codes[k];
            return sp + n;
        }
        if (s[sp] == '?') { *code = 0; *bare = 1; return sp + 1; }
        if (s[sp] == '>') { *code = 5; *bare = 1; return sp + 1; }
        if (s[sp] == ' ') { *code = 31; return sp + 1; }
    }
    return 0;
}

/* optional_num_pattern (only called after a command ending in SNR) */
static int parse_num(const char *s, int *num)
{
    int p = 0, neg = 0, v;
    if (is_sp(s[p])) p++;
    if (s[p] == '-' || s[p] == '+') { neg = s[p] == '-'; p++; }
    if (!is_dig(s[p])) return 0;
    if (s[p] == '3' && (s[p + 1] == '0' || s[p + 1] == '1')) { v = 30 + (s[p + 1] - '0'); p += 2; }
    else if (s[p] <= '2' && is_dig(s[p + 1])) { v = (s[p] - '0') * 10 + (s[p + 1] - '0'); p += 2; }
    else { v = s[p] - '0'; p += 1; }
    *num = neg ? -v : v;
    return p;
}

static int s_same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int contains(const char *s, const char *sub)
{
    int i;
    if (!sub[0]) return 1;
    for (i = 0; s[i]; i++) if (starts(s + i, sub) > 0) return 1;
    return 0;
}

int js8_build_message(const js8_build_t *cfg, const char *text,
                      uint8_t frames[][JS8_FRAME_BYTES], int max_frames)
{
    char line[JS8_LINE_MAX + 24];
    const char *mycall = cfg->mycall ? cfg->mycall : "";
    int len = 0, pos = 0, nf = 0, has_dir = 0, has_data = 0, i;
    int my_kind = js8_call_kind(mycall), mylen = s_len(mycall);

    for (i = 0; text[i] && len < JS8_LINE_MAX; i++) {
        char c = text[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        line[len++] = c;
    }
    line[len] = 0;
    /* drop our own call from the start of the line */
    if (mylen && starts(line, mycall) > 0 && (line[mylen] == ':' || line[mylen] == ' ')) {
        pos = mylen + 1;
        while (is_sp(line[pos])) pos++;
    }

    while (line[pos]) {
        char grid[5], to[JS8_CALL_MAX + 1];
        int cq = 0, b3 = 0, l, n = 0, m, cmd = -1, has_num = 0, num = 0, to_kind = 0, bare = 0;
        uint8_t df[JS8_FRAME_BYTES];
        const char *s = line + pos;

        if (nf + 2 > max_frames) return -1;
        l = parse_hb(s, &cq, &b3, grid);
        if (l > 0 && !mylen) l = 0;

        /* directed_re: callsign, command, number */
        {
            int c = 0, k;
            if (s[c] == '@') c++;
            while (is_alnum(s[c]) || s[c] == '/') c++;
            if (c > (s[0] == '@') && c <= JS8_CALL_MAX) {
                for (k = 0; k < c; k++) to[k] = s[k];
                to[c] = 0;
                k = parse_cmd(s + c, &cmd, &bare);
                if (k > 0 && !s_same(to, mycall) && (to_kind = js8_call_kind(to)) != 0) {
                    n = c + k;
                    if (js8_cmd_is_snr(cmd)) {
                        k = parse_num(s + n, &num);
                        if (k > 0) { has_num = 1; n += k; }
                    }
                }
            }
        }
        if (n > 0) {
            /* check that the frame really packs, as packDirectedMessage would */
            uint32_t v; int por;
            if (my_kind == 0 || (my_kind == 1 && !js8_call28_pack(mycall, &v, &por)) ||
                (to_kind == 1 && !js8_call28_pack(to, &v, &por))) n = 0;
        }
        if (n == 0) to_kind = 0;

        /* identify plain free text */
        if (cfg->identify && nf == 0 && to_kind == 0 && l == 0 && mylen && !contains(s, mycall) &&
            len + mylen + 2 <= JS8_LINE_MAX && pos >= 0) {
            int rest = len - pos, sh = mylen + 2;
            for (i = rest; i >= 0; i--) line[pos + sh + i] = line[pos + i];
            for (i = 0; i < mylen; i++) line[pos + i] = mycall[i];
            line[pos + mylen] = ':'; line[pos + mylen + 1] = ' ';
            len += sh;
        }
        m = js8_pack_data(s, len - pos, cfg->jsc, 0, df);

        if (!has_dir && !has_data && l > 0) {
            if (!js8_pack_heartbeat(mycall, grid[0] ? grid : 0, cq, b3, 0, frames[nf])) l = 0;
            else { nf++; pos += l; continue; }
        }
        if (!has_dir && !has_data && n > 0) {
            if (my_kind == 2 || to_kind == 2) {
                if (js8_pack_compound(mycall, cfg->mygrid, 0, frames[nf])) nf++;
                if (js8_pack_compound_directed(to, cmd, has_num, num, 0, frames[nf])) nf++;
            } else {
                js8_pack_directed(mycall, to, cmd, has_num, num, 0, frames[nf]);
                nf++;
            }
            has_dir = 1;
            pos += n;
            /* buffered command with text: strip blanks, append the checksum */
            if (!(bare && cmd == 0) && line[pos]) {          /* isCommandBuffered */
                while (is_sp(line[pos])) pos++;
                if (js8_cmd_checksum(cmd) == 16) {
                    line[len] = ' ';
                    js8_checksum16(line + pos, len - pos, line + len + 1);
                    len += 4;
                    line[len] = 0;
                }
            }
            continue;
        }
        if (m > 0) {
            for (i = 0; i < JS8_FRAME_BYTES; i++) frames[nf][i] = df[i];
            nf++; has_data = 1; pos += m;
            continue;
        }
        pos++;                                               /* not encodable: skip */
    }
    if (nf > 0) {
        frames[0][9] |= (uint8_t)(JS8_I3_FIRST << 5);
        js8_msg_crc_set(frames[0]);
        frames[nf - 1][9] |= (uint8_t)(JS8_I3_LAST << 5);
        js8_msg_crc_set(frames[nf - 1]);
    }
    return nf;
}
