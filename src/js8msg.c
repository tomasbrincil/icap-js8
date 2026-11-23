/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8msg.c - 11-byte JS8 frame -> text, as JS8Call shows it.
 * Re-implemented from JS8Call varicode.cpp, decodedtext.cpp, jsc.cpp
 * ((C) 2018 Jordan Sherer KN4CRD, GPL-3.0-or-later); no code copied, tables derived.
 *
 * Receiver order (decodedtext.h): fast data (i3bit & 4) -> data (payload
 * bit 0 set) -> heartbeat/CQ (type 0) -> compound (1, 2) -> directed (3).
 * Edge values follow JS8Call exactly (checked against an independent model
 * of DecodedText in tests/rv_protocol.py; no conforming packer sends them):
 *   - grid value 32400 (nbasegrid itself) shows "RA90", above it no grid,
 *   - a 28-bit call >= nbasecall that is not one of the 54 group calls is
 *     unpacked like any other value (first character '/', index 37 of the
 *     39-character alphabet - inside the table for every 28-bit value),
 *   - a compound-directed SNR command without the number flag shows "-31".
 * The one deliberate difference: JSC decode honours the `size` field
 * (JS8Call-improved behaviour).  Text is returned as JS8Call holds it,
 * control characters of the dictionary included (word 69 is a newline);
 * making it printable is the job of the caller (js8asm, the shell).
 */
#include "js8msg.h"

const char js8_alnum[40] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@";
static const char alpha72[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+";

const js8_huff_t js8_huff[44] = {
    {' ',2,0x01}, {'E',3,0x04}, {'T',4,0x0D}, {'A',4,0x03}, {'O',5,0x1F}, {'I',5,0x1C},
    {'N',5,0x17}, {'S',5,0x14}, {'H',5,0x03}, {'R',5,0x00}, {'D',6,0x3B}, {'L',6,0x33},
    {'C',6,0x31}, {'U',6,0x2D}, {'M',6,0x2B}, {'W',6,0x0B}, {'F',6,0x09}, {'G',6,0x05},
    {'Y',6,0x03}, {'P',7,0x7B}, {'B',7,0x79}, {'.',7,0x74}, {'V',7,0x65}, {'K',7,0x64},
    {'-',7,0x61}, {'+',7,0x60}, {'?',7,0x59}, {'!',7,0x58}, {'\"',7,0x55}, {'X',7,0x54},
    {'0',7,0x15}, {'J',7,0x14}, {'1',7,0x11}, {'Q',7,0x10}, {'2',7,0x09}, {'Z',7,0x08},
    {'3',7,0x05}, {'5',7,0x04}, {'4',8,0xF5}, {'9',8,0xF4}, {'8',8,0xF1}, {'6',8,0xF0},
    {'7',8,0xEB}, {'/',8,0xEA},
};

/* group / special calls, value = nbasecall + 1 + position (NUL separated) */
static const char groups[] =
    "<....>\0@ALLCALL\0@JS8NET\0"
    "@DX/NA\0@DX/SA\0@DX/EU\0@DX/AS\0@DX/AF\0@DX/OC\0@DX/AN\0"
    "@REGION/1\0@REGION/2\0@REGION/3\0"
    "@GROUP/0\0@GROUP/1\0@GROUP/2\0@GROUP/3\0@GROUP/4\0"
    "@GROUP/5\0@GROUP/6\0@GROUP/7\0@GROUP/8\0@GROUP/9\0"
    "@COMMAND\0@CONTROL\0@NET\0@NTS\0"
    "@RESERVE/0\0@RESERVE/1\0@RESERVE/2\0@RESERVE/3\0@RESERVE/4\0"
    "@APRSIS\0@RAGCHEW\0@JS8\0@EMCOMM\0@ARES\0@MARS\0@AMRRON\0@RACES\0@RAYNET\0"
    "@RADAR\0@SKYWARN\0@CQ\0@HB\0@QSO\0@QSOPARTY\0@CONTEST\0@FIELDDAY\0"
    "@SOTA\0@IOTA\0@POTA\0@QRP\0@QRO\0";

/* directed commands 0..31: what directed_cmds.key(code) returns (first key in
 * QMap order: 0 -> " SNR?", 12 -> " QUERY MSGS", 31 -> " ") */
static const char cmds[] =
    " SNR?\0 DIT DIT\0 NACK\0 HEARING?\0 GRID?\0>\0 STATUS?\0 STATUS\0"
    " HEARING\0 MSG\0 MSG TO:\0 QUERY\0 QUERY MSGS\0 QUERY CALL\0 ACK\0 GRID\0"
    " INFO?\0 INFO\0 FB\0 HW CPY?\0 SK\0 RR\0 QSL?\0 QSL\0"
    " CMD\0 SNR\0 NO\0 YES\0 73\0 HEARTBEAT SNR\0 AGN?\0 \0";

static const char cqs[] = "CQ CQ CQ\0CQ DX\0CQ QRP\0CQ CONTEST\0CQ FIELD\0CQ FD\0CQ CQ\0CQ\0";

static const char *nth(const char *p, int n)
{
    while (n-- > 0) { while (*p) p++; p++; }
    return p;
}

static int s_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

const char *js8_group_name(int n) { return (n >= 1 && n <= JS8_NGROUPS) ? nth(groups, n - 1) : ""; }
const char *js8_cmd_str(int code) { return (code >= 0 && code <= 31) ? nth(cmds, code) : ""; }
const char *js8_cq_str(int b3)    { return (b3 >= 0 && b3 <= 7) ? nth(cqs, b3) : ""; }
int js8_cmd_is_snr(int code)      { return code == 25 || code == 29; }

int js8_cmd_checksum(int code)
{
    return (code == 5 || (code >= 9 && code <= 13) || code == 24) ? 16 : 0;
}

int js8_group_find(const char *name)
{
    const char *p = groups;
    int n;
    for (n = 1; n <= JS8_NGROUPS; n++) {
        if (s_eq(p, name)) return n;
        while (*p) p++;
        p++;
    }
    return 0;
}

/* ---- bits ------------------------------------------------------------------ */

static uint32_t bits(const uint8_t *f, int pos, int n)
{
    uint32_t v = 0;
    while (n-- > 0) { v = (v << 1) | ((f[pos >> 3] >> (7 - (pos & 7))) & 1u); pos++; }
    return v;
}

unsigned js8_msg_crc12(const uint8_t f[JS8_FRAME_BYTES])
{
    unsigned reg = 0;
    int i;
    for (i = 0; i < 88; i++) {
        unsigned b = i < 75 ? (unsigned)((f[i >> 3] >> (7 - (i & 7))) & 1) : 0u;
        unsigned top = reg & 0x800u;
        reg = ((reg << 1) | b) & 0xFFFu;
        if (top) reg ^= 0xC06u;
    }
    return reg ^ 42u;
}

int js8_msg_crc_ok(const uint8_t f[JS8_FRAME_BYTES])
{
    return js8_msg_crc12(f) == ((((unsigned)f[9] & 0x1Fu) << 7) | ((unsigned)f[10] >> 1));
}

void js8_msg_crc_set(uint8_t f[JS8_FRAME_BYTES])
{
    unsigned c = js8_msg_crc12(f);
    f[9] = (uint8_t)((f[9] & 0xE0u) | ((c >> 7) & 0x1Fu));
    f[10] = (uint8_t)((c & 0x7Fu) << 1);
}

void js8_frame_chars(const uint8_t f[JS8_FRAME_BYTES], char out[13])
{
    int i;
    for (i = 0; i < 12; i++) out[i] = alpha72[bits(f, 6 * i, 6)];
    out[12] = 0;
}

/* ---- bounded text writer --------------------------------------------------- */

typedef struct { char *p; int n, max; uint8_t trunc; } wr_t;

static void wc(wr_t *w, char c)
{
    if (w->n < w->max - 1) w->p[w->n++] = c; else w->trunc = 1;
}
static void ws(wr_t *w, const char *s) { while (*s) wc(w, *s++); }
static void wend(wr_t *w) { w->p[w->n] = 0; }

static void s_copy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

/* ---- calls, grid, numbers -------------------------------------------------- */

int js8_call28_unpack(uint32_t v, int portable, char out[JS8_CALL_MAX])
{
    char w[8];
    int a, b, i, n = 0;

    if (v > JS8_NBASECALL && v <= JS8_NBASECALL + JS8_NGROUPS) {
        s_copy(out, js8_group_name((int)(v - JS8_NBASECALL)), JS8_CALL_MAX);
        return 1;
    }
    if (v >> 28) { out[0] = 0; return 0; }                     /* not a 28-bit value (API guard only) */
    w[5] = js8_alnum[v % 27 + 10]; v /= 27;
    w[4] = js8_alnum[v % 27 + 10]; v /= 27;
    w[3] = js8_alnum[v % 27 + 10]; v /= 27;
    w[2] = js8_alnum[v % 10]; v /= 10;
    w[1] = js8_alnum[v % 36]; v /= 36;
    w[0] = js8_alnum[v];
    /* 3D0xxx -> 3DA0xxx (Swaziland), Q[A-Z].. -> 3X[A-Z].. (Guinea) */
    if (w[0] == '3' && w[1] == 'D' && w[2] == '0') {
        out[n++] = '3'; out[n++] = 'D'; out[n++] = 'A'; out[n++] = '0';
        for (i = 3; i < 6; i++) out[n++] = w[i];
    } else {
        for (i = 0; i < 6; i++) out[n++] = w[i];
    }
    if (out[0] == 'Q' && out[1] >= 'A' && out[1] <= 'Z') {
        for (i = n; i > 1; i--) out[i] = out[i - 1];
        out[0] = '3'; out[1] = 'X'; n++;
    }
    /* trimmed() */
    a = 0; b = n;
    while (a < b && out[a] == ' ') a++;
    while (b > a && out[b - 1] == ' ') b--;
    for (i = 0; i < b - a; i++) out[i] = out[a + i];
    n = b - a;
    if (portable) { out[n++] = '/'; out[n++] = 'P'; }
    out[n] = 0;
    return 1;
}

static unsigned limb_divmod(uint16_t l[4], unsigned d)
{
    uint32_t rem = 0;
    int i;
    for (i = 3; i >= 0; i--) {
        uint32_t cur = (rem << 16) | l[i];
        l[i] = (uint16_t)(cur / d);
        rem = cur % d;
    }
    return (unsigned)rem;
}

void js8_call50_unpack(const uint16_t limb[4], char out[JS8_CALL_MAX])
{
    static const uint8_t radix[11] = { 39, 38, 38, 2, 38, 38, 38, 2, 38, 38, 38 };
    uint16_t l[4];
    char w[11];
    int i, n = 0;

    for (i = 0; i < 4; i++) l[i] = limb[i];
    for (i = 10; i >= 0; i--) {
        unsigned r = limb_divmod(l, radix[i]);
        w[i] = (i == 3 || i == 7) ? (r ? '/' : ' ') : js8_alnum[r];
    }
    for (i = 0; i < 11; i++) if (w[i] != ' ') out[n++] = w[i];
    out[n] = 0;
}

int js8_grid_unpack(unsigned v, char out[5])
{
    unsigned k, m;
    out[0] = 0;
    if (v > JS8_NBASEGRID) return 0;
    if (v == JS8_NBASEGRID) {                    /* JS8Call's deg2grid wraps the longitude: "RA90" */
        out[0] = 'R'; out[1] = 'A'; out[2] = '9'; out[3] = '0'; out[4] = 0;
        return 1;
    }
    k = 179u - v / 180u;
    m = v % 180u;
    out[0] = (char)('A' + k / 10); out[1] = (char)('A' + m / 10);
    out[2] = (char)('0' + k % 10); out[3] = (char)('0' + m % 10);
    out[4] = 0;
    return 1;
}

static int fmt_int(int v, char *out)
{
    int n = 0;
    unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
    if (v < 0) out[n++] = '-';
    if (u >= 100) out[n++] = (char)('0' + u / 100 % 10);
    if (u >= 10) out[n++] = (char)('0' + u / 10 % 10);
    out[n++] = (char)('0' + u % 10);
    out[n] = 0;
    return n;
}

int js8_snr_format(int snr, char *out)
{
    unsigned u = snr < 0 ? (unsigned)-snr : (unsigned)snr;
    out[0] = 0;
    if (snr < -60 || snr > 60) return 0;
    out[0] = snr >= 0 ? '+' : '-';
    out[1] = (char)('0' + u / 10);
    out[2] = (char)('0' + u % 10);
    out[3] = 0;
    return 3;
}

static void w_num(wr_t *w, int cmd, int num)
{
    char t[8];
    if (js8_cmd_is_snr(cmd)) js8_snr_format(num, t); else fmt_int(num, t);
    ws(w, t);
}

int js8_format_directed(const char *from, const char *to, int cmd, int has_num, int num,
                        char *out, int max)
{
    wr_t w;
    w.p = out; w.n = 0; w.max = max; w.trunc = 0;
    ws(&w, from); ws(&w, ": "); ws(&w, to); ws(&w, js8_cmd_str(cmd));
    if (has_num) { wc(&w, ' '); w_num(&w, cmd, num); }
    wend(&w);
    return w.n;
}

void js8_checksum16(const char *s, int len, char out[3])
{
    static const char a41[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?";
    unsigned crc = 0;
    int i, k;
    for (i = 0; i < len; i++) {                  /* CRC-16/KERMIT: reflected 0x1021 */
        crc ^= (uint8_t)s[i];
        for (k = 0; k < 8; k++) crc = (crc & 1u) ? (crc >> 1) ^ 0x8408u : crc >> 1;
    }
    out[0] = a41[crc / 1681u]; out[1] = a41[crc % 1681u / 41u]; out[2] = a41[crc % 41u];
}

/* ---- data frames ----------------------------------------------------------- */

static void huff_decode(const uint8_t *b, int n, wr_t *w)
{
    int i = 0;
    while (i < n) {
        unsigned code = 0;
        int len, k, hit = 0;
        for (len = 1; len <= 8 && i + len <= n && !hit; len++) {
            code = (code << 1) | b[i + len - 1];
            if (len < 2) continue;
            for (k = 0; k < 44; k++) {
                if (js8_huff[k].len == len && js8_huff[k].code == code) {
                    wc(w, js8_huff[k].c);
                    i += len; hit = 1;
                    break;
                }
            }
        }
        if (!hit) break;                         /* trailing partial code is dropped */
    }
}

/* (s,c)-dense code, s = 7, c = 9, 4-bit digits; JSC::decompress */
static void jsc_decode(const uint8_t *b, int n, js8jsc_t *jsc, wr_t *w, uint8_t *flags)
{
    static const uint32_t base[6] = { 0, 7, 70, 637, 5740, 51667 };
    uint8_t nib[18], sep[18];
    int cnt = 0, i = 0, start = 0;

    while (i < n && n - i >= 4) {
        uint8_t v = (uint8_t)(b[i] << 3 | b[i + 1] << 2 | b[i + 2] << 1 | b[i + 3]);
        i += 4;
        nib[cnt] = v; sep[cnt] = 0;
        if (v < 7) {
            if (n - i > 0 && b[i]) sep[cnt] = 1;
            i += 1;
        }
        cnt++;
    }
    while (start < cnt) {
        uint32_t j = 0;
        int k = 0, len;
        char word[JS8JSC_WORD_MAX];
        while (start + k < cnt && nib[start + k] >= 7) {
            j = j * 9u + (uint32_t)(nib[start + k] - 7);
            k++;
            if (j >= JS8JSC_SIZE) break;         /* cannot become valid again */
        }
        if (j >= JS8JSC_SIZE || start + k >= cnt || k > 5) break;
        j = j * 7u + nib[start + k] + base[k];
        if (j >= JS8JSC_SIZE) break;
        len = jsc ? js8jsc_word(jsc, j, word, JS8JSC_WORD_MAX) : -1;
        if (len < 0) { ws(w, "{?}"); *flags |= JS8_MF_NODICT; }
        else for (i = 0; i < len; i++) wc(w, word[i]);
        if (sep[start + k]) wc(w, ' ');
        start += k + 1;
    }
}

static int data_decode(const uint8_t *f, int fast, js8jsc_t *jsc, js8_msg_t *m, wr_t *w)
{
    uint8_t b[72];
    int i, n, off, len, compressed;

    for (i = 0; i < 72; i++) b[i] = (uint8_t)bits(f, i, 1);
    if (fast) {
        /* all 72 bits: JSC data, pad = one 0 then 1s; payload = bits[0 .. last 0) */
        n = -1;
        for (i = 0; i < 72; i++) if (!b[i]) n = i;
        off = 0; len = n < 0 ? 72 : n;           /* QList::mid(0, -1) = everything */
        compressed = 1;
    } else {
        /* bit 0 = data flag, bit 1 = compressed, then payload up to the last 0 */
        if (!b[0]) return 0;
        compressed = b[1];
        n = -1;
        for (i = 1; i < 72; i++) if (!b[i]) n = i - 1;      /* index inside the 71 bits */
        off = 2; len = n - 1;
        if (len < 0) len = 70;                   /* QList::mid(1, negative) = rest */
    }
    if (compressed) jsc_decode(b + off, len, jsc, w, &m->flags);
    else huff_decode(b + off, len, w);
    if (w->n == 0) return 0;
    m->type = fast ? JS8_FT_DATA_FAST : compressed ? JS8_FT_DATA_COMPRESSED : JS8_FT_DATA;
    return 1;
}

/* ---- frame dispatch -------------------------------------------------------- */

static void compound_call(const uint8_t *f, char out[JS8_CALL_MAX])
{
    uint16_t l[4];
    uint32_t hi = bits(f, 3, 18), lo = bits(f, 21, 32);
    l[3] = (uint16_t)(hi >> 16); l[2] = (uint16_t)hi;
    l[1] = (uint16_t)(lo >> 16); l[0] = (uint16_t)lo;
    js8_call50_unpack(l, out);
}

int js8_msg_decode(const uint8_t f[JS8_FRAME_BYTES], js8jsc_t *jsc, js8_msg_t *m)
{
    wr_t w;
    unsigned ft, num;

    m->type = JS8_FT_UNKNOWN; m->i3 = (uint8_t)(f[9] >> 5);
    m->is_cq = 0; m->bits3 = 0; m->cmd = -1; m->has_num = 0; m->num = 0; m->flags = 0;
    m->from[0] = 0; m->to[0] = 0; m->grid[0] = 0; m->text[0] = 0;
    w.p = m->text; w.n = 0; w.max = JS8_TEXT_MAX; w.trunc = 0;

    if (m->i3 & JS8_I3_DATA) {
        if (data_decode(f, 1, jsc, m, &w)) goto done;
        goto raw;                                /* other strategies refuse the data flag */
    }
    if (data_decode(f, 0, jsc, m, &w)) goto done;

    ft = bits(f, 0, 3);
    num = bits(f, 53, 16);
    if (ft == JS8_FT_HEARTBEAT) {
        m->type = JS8_FT_HEARTBEAT;
        compound_call(f, m->from);
        m->is_cq = (uint8_t)(num >> 15);
        m->bits3 = (uint8_t)bits(f, 69, 3);
        js8_grid_unpack(num & 0x7FFFu, m->grid);
        s_copy(m->to, m->is_cq ? "@ALLCALL" : "@HB", JS8_CALL_MAX);
        ws(&w, m->from); ws(&w, ": ");
        if (m->is_cq) { ws(&w, "@ALLCALL "); ws(&w, js8_cq_str(m->bits3)); }
        else ws(&w, "@HB HEARTBEAT");
        wc(&w, ' '); ws(&w, m->grid); wc(&w, ' ');
    } else if (ft == JS8_FT_COMPOUND || ft == JS8_FT_COMPOUND_DIRECTED) {
        char call[JS8_CALL_MAX];
        int have_extra = 0;
        m->type = (uint8_t)ft;
        m->bits3 = (uint8_t)bits(f, 69, 3);
        compound_call(f, call);
        if (num <= JS8_NBASEGRID) {
            js8_grid_unpack(num, m->grid);
            have_extra = 1;
        } else if (num >= JS8_NUSERGRID && num < JS8_NMAXGRID) {
            unsigned v = (num - JS8_NUSERGRID) & 0xFFu;      /* quint8 argument */
            if (v & 0x80u) {
                m->cmd = (v & 0x40u) ? 29 : 25;
                m->has_num = 1;
                m->num = (int8_t)((int)(v & 0x3Fu) - 31);
            } else {
                m->cmd = (int8_t)(v & 0x7Fu);
                if (js8_cmd_is_snr(m->cmd)) {    /* JS8Call formats (0 - 31) for an SNR command without the flag */
                    m->has_num = 1;
                    m->num = -31;
                }
            }
            have_extra = 2;
        }
        if (ft == JS8_FT_COMPOUND) {
            s_copy(m->from, call, JS8_CALL_MAX);
            ws(&w, call); ws(&w, ": ");
        } else {
            s_copy(m->from, "<....>", JS8_CALL_MAX);
            s_copy(m->to, call, JS8_CALL_MAX);
            ws(&w, call);
            if (have_extra == 1) { wc(&w, ' '); ws(&w, m->grid); }
            else if (have_extra == 2) {
                ws(&w, js8_cmd_str(m->cmd));
                if (m->has_num) { wc(&w, ' '); w_num(&w, m->cmd, m->num); }
            }
            wc(&w, ' ');
        }
    } else if (ft == JS8_FT_DIRECTED) {
        unsigned extra = bits(f, 64, 8);
        if (!js8_call28_unpack(bits(f, 3, 28), (int)((extra >> 7) & 1u), m->from) ||
            !js8_call28_unpack(bits(f, 31, 28), (int)((extra >> 6) & 1u), m->to)) {
            m->from[0] = 0; m->to[0] = 0;
            goto raw;
        }
        m->type = JS8_FT_DIRECTED;
        m->cmd = (int8_t)bits(f, 59, 5);
        extra &= 63u;
        if (extra) { m->has_num = 1; m->num = (int8_t)((int)extra - 31); }
        w.n = js8_format_directed(m->from, m->to, m->cmd, m->has_num, m->num, m->text, JS8_TEXT_MAX);
        wc(&w, ' ');
    } else {
        goto raw;
    }
done:
    wend(&w);
    if (w.trunc) m->flags |= JS8_MF_TRUNC;
    return m->type;
raw:
    m->type = JS8_FT_UNKNOWN;
    js8_frame_chars(f, m->text);
    return m->type;
}
