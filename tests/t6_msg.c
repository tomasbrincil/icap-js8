/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * t6_msg.c - host tests of the JS8 message layer (js8msg, js8jsc, js8pack,
 * js8asm).  The JS8Call sources are read as TEXT and used as the reference
 * for every table; nothing of JS8Call is compiled.
 *
 *   t6_msg [SRC_DIR [SD_DIR]]
 *     SRC_DIR  directory with varicode.cpp, jsc_list.cpp   (JS8Call, as data)
 *     SD_DIR   directory with the files made by tools/mkjsc.py
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../src/js8msg.h"
#include "../src/js8jsc.h"
#include "../src/js8pack.h"
#include "../src/js8asm.h"

static int n_ok, n_fail;
static const char *cur = "";

static void section(const char *s) { cur = s; printf("== %s\n", s); }
#define CHECK(cond, ...) do { if (cond) n_ok++; else { n_fail++; \
    printf("  FAIL [%s] %s:%d: ", cur, __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define CHECK_STR(got, exp) CHECK(strcmp((got), (exp)) == 0, "got \"%s\" expected \"%s\"", (got), (exp))

/* ---- deterministic random -------------------------------------------------- */
static uint32_t rs = 0x12345678u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static uint32_t rn(uint32_t n) { return rnd() % n; }

/* ---- reading the JS8Call sources as text ----------------------------------- */
static char *slurp(const char *dir, const char *name, long *len)
{
    char path[1024];
    FILE *f;
    char *b;
    long n;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { printf("read error %s\n", path); exit(2); }
    b[n] = 0; fclose(f);
    if (len) *len = n;
    return b;
}

/* one "{ ... }" initialiser item: up to 2 string literals and 2 integers */
typedef struct { char s[2][64]; int slen[2]; int ns; long v[2]; int nv; } item_t;

static const char *parse_str(const char *p, char *out, int *len)
{
    int n = 0;
    p++;                                             /* opening quote */
    while (*p && *p != '"') {
        int c = (unsigned char)*p++;
        if (c == '\\') {
            c = (unsigned char)*p++;
            if (c == 'n') c = '\n';
            else if (c == 'x') {
                int v = 0;
                while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                    int d = *p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10;
                    v = v * 16 + d; p++;
                }
                c = v & 255;
            }
        }
        if (n < 63) out[n++] = (char)c;
    }
    out[n] = 0; *len = n;
    return *p ? p + 1 : p;
}

/* next item between p and end; returns pointer after it or NULL */
static const char *next_item(const char *p, const char *end, item_t *it)
{
    it->ns = it->nv = 0;
    while (p < end && *p != '{') {
        if (p[0] == '/' && p[1] == '/') { while (p < end && *p != '\n') p++; continue; }
        p++;
    }
    if (p >= end) return 0;
    p++;
    while (p < end && *p != '}') {
        if (*p == '"') {
            if (it->ns < 2) p = parse_str(p, it->s[it->ns], &it->slen[it->ns]), it->ns++;
            else { char t[64]; int l; p = parse_str(p, t, &l); }
        } else if (p[0] == '/' && p[1] == '*') {
            while (p < end && !(p[0] == '*' && p[1] == '/')) p++;
            p += 2;
        } else if ((*p >= '0' && *p <= '9') || (*p == '-' && p[1] >= '0' && p[1] <= '9')) {
            char *e;
            long v = strtol(p, &e, 10);
            if (it->nv < 2) it->v[it->nv++] = v; else it->v[1] = v;
            p = e;
        } else p++;
    }
    return p < end ? p + 1 : 0;
}

static const char *block(const char *src, const char *start, const char **end)
{
    const char *p = strstr(src, start);
    if (!p) { printf("source block '%s' not found\n", start); exit(2); }
    p += strlen(start);
    *end = strstr(p, "};");
    return p;
}

/* ---- file io for the dictionary (stdio) ------------------------------------ */
static const char *sd_dir;
static FILE *fds[4];
static int io_open(void *u, const char *name)
{
    char path[1024];
    int i;
    (void)u;
    for (i = 0; i < 4 && fds[i]; i++) ;
    if (i == 4) return -2;
    snprintf(path, sizeof path, "%s/%s", sd_dir, name);
    fds[i] = fopen(path, "rb");
    return fds[i] ? i : -1;
}
static int io_read(void *u, int fd, void *buf, uint32_t len) { (void)u; return (int)fread(buf, 1, len, fds[fd]); }
static int io_close(void *u, int fd) { (void)u; fclose(fds[fd]); fds[fd] = 0; return 0; }
static const js8_io_t io = { io_open, io_read, io_close, 0 };

/* ---- the dictionary parsed from jsc_list.cpp ------------------------------- */
#define NJSC 262144
typedef struct { char s[28]; int len; int index; } tup_t;
static tup_t *jlist;            /* JSC::list, in file order */
static tup_t jprefix[128];
static int njprefix;
static int *by_index;           /* index -> position in jlist */

static void load_jsc(const char *srcdir)
{
    long len;
    char *src = slurp(srcdir, "jsc_list.cpp", &len);
    const char *end, *p = block(src, "JSC::list[262144] = {", &end);
    item_t it;
    int n = 0;
    jlist = calloc(NJSC, sizeof *jlist);
    by_index = malloc(NJSC * sizeof *by_index);
    memset(by_index, 0xff, NJSC * sizeof *by_index);
    while ((p = next_item(p, end, &it)) != 0) {
        if (it.ns != 1 || it.nv != 2 || n >= NJSC) { printf("bad list item %d\n", n); exit(2); }
        memcpy(jlist[n].s, it.s[0], 27); jlist[n].len = (int)it.v[0]; jlist[n].index = (int)it.v[1];
        if (it.v[1] >= 0 && it.v[1] < NJSC) by_index[it.v[1]] = n;
        n++;
    }
    CHECK(n == NJSC, "list entries %d", n);
    p = block(src, "JSC::prefix[103] = {", &end);
    while ((p = next_item(p, end, &it)) != 0) {
        memcpy(jprefix[njprefix].s, it.s[0], 27);
        jprefix[njprefix].len = (int)it.v[0]; jprefix[njprefix].index = (int)it.v[1];
        njprefix++;
    }
    CHECK(njprefix == 103, "prefix entries %d", njprefix);
    free(src);
}

/* JSC::lookup(char const*) re-stated from jsc.cpp, on the parsed tables */
static int ref_lookup(const char *b, int *index)
{
    int i, first = 0, count = 0, found = 0;
    for (i = 0; i < njprefix; i++) {
        if (b[0] != jprefix[i].s[0]) continue;
        if (jprefix[i].len == 1) { *index = jlist[jprefix[i].index].index; return 1; }
        first = jprefix[i].index; count = jprefix[i].len; found = 1;
        break;
    }
    if (!found) return 0;
    for (i = first; i < first + count; i++)
        if (strncmp(b, jlist[i].s, (size_t)jlist[i].len) == 0) { *index = jlist[i].index; return 1; }
    return 0;
}

/* length the decoder shows / the compressor consumes: JSC::map[].size.  The
 * list has the same strings; its size differs from the map only for index
 * 262143 ("ROSIDS", map size 1) - see the design, open question 13. */
static int map_size(int index)
{
    if (index == 262143) return 1;
    return jlist[by_index[index]].len;
}

/* ---- helpers --------------------------------------------------------------- */
static void frame_from_chars(const char *c12, int i3, uint8_t f[11])
{
    static const char a[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+";
    int i, k;
    memset(f, 0, 11);
    for (i = 0; i < 12; i++) {
        int v = (int)(strchr(a, c12[i]) - a);
        for (k = 0; k < 6; k++) if (v >> (5 - k) & 1) f[(6 * i + k) >> 3] |= (uint8_t)(0x80 >> ((6 * i + k) & 7));
    }
    f[9] = (uint8_t)(i3 << 5);
    js8_msg_crc_set(f);
}

static void setbits(uint8_t *f, int pos, int n, uint64_t v)
{
    while (n-- > 0) { if ((v >> n) & 1) f[pos >> 3] |= (uint8_t)(0x80 >> (pos & 7)); pos++; }
}

/* ---- tests: tables against varicode.cpp ------------------------------------ */
static void t_tables(const char *srcdir)
{
    char *src = slurp(srcdir, "varicode.cpp", 0);
    const char *end, *p;
    item_t it;
    int n, code;
    static char best[32][32];
    static int have[32];

    section("tables vs varicode.cpp");
    /* directed_cmds: reverse lookup = first key in QMap (sorted) order */
    p = block(src, "QMap<QString, int> directed_cmds = {", &end);
    n = 0;
    while ((p = next_item(p, end, &it)) != 0) {
        n++;
        if (it.ns != 1 || it.nv != 1) { CHECK(0, "bad cmd item"); continue; }
        code = (int)it.v[0];
        if (code < 0) continue;
        CHECK(code <= 31, "command code %d", code);
        if (!have[code] || strcmp(it.s[0], best[code]) < 0) strcpy(best[code], it.s[0]);
        have[code] = 1;
    }
    CHECK(n == 38, "directed_cmds entries %d", n);   /* 3 faux (-1) + 35 */
    for (code = 0; code < 32; code++) {
        CHECK(have[code], "code %d missing in source", code);
        CHECK_STR(js8_cmd_str(code), best[code]);
    }
    CHECK_STR(js8_cmd_str(0), " SNR?");
    CHECK_STR(js8_cmd_str(12), " QUERY MSGS");
    CHECK_STR(js8_cmd_str(31), " ");
    CHECK_STR(js8_cmd_str(32), "");
    CHECK_STR(js8_cmd_str(-1), "");

    /* basecalls */
    p = block(src, "QMap<QString, quint32> basecalls = {", &end);
    n = 0;
    while ((p = next_item(p, end, &it)) != 0) {
        n++;
        CHECK(it.ns == 1 && it.nv == 1, "bad basecall item");
        CHECK_STR(js8_group_name((int)it.v[0]), it.s[0]);
        CHECK(js8_group_find(it.s[0]) == (int)it.v[0], "group_find %s", it.s[0]);
    }
    CHECK(n == 54 && n == JS8_NGROUPS, "basecalls %d", n);
    CHECK(strstr(src, "quint32 nbasecall = 37 * 36 * 10 * 27 * 27 * 27;") != 0, "nbasecall formula");
    CHECK(JS8_NBASECALL == 37u * 36 * 10 * 27 * 27 * 27, "nbasecall value");
    CHECK(strstr(src, "quint16 nbasegrid = 180 * 180;") && JS8_NBASEGRID == 180 * 180, "nbasegrid");
    CHECK(strstr(src, "quint16 nusergrid = nbasegrid + 10;") && JS8_NUSERGRID == 32410, "nusergrid");
    CHECK(strstr(src, "quint16 nmaxgrid  = (1<<15)-1;") && JS8_NMAXGRID == 32767, "nmaxgrid");

    /* cqs */
    p = block(src, "QMap<quint32, QString> cqs = {", &end);
    n = 0;
    while ((p = next_item(p, end, &it)) != 0) {
        n++;
        CHECK_STR(js8_cq_str((int)it.v[0]), it.s[0]);
    }
    CHECK(n == 8, "cqs %d", n);
    /* hbs: all "HB" */
    p = block(src, "QMap<quint32, QString> hbs = {", &end);
    while ((p = next_item(p, end, &it)) != 0) CHECK_STR(it.s[0], "HB");

    /* huffman table */
    p = block(src, "QMap<QString, QString> hufftable = {", &end);
    n = 0;
    while ((p = next_item(p, end, &it)) != 0) {
        int k, hit = 0;
        unsigned c = 0;
        n++;
        for (k = 0; it.s[1][k]; k++) c = c << 1 | (unsigned)(it.s[1][k] == '1');
        for (k = 0; k < 44; k++)
            if (js8_huff[k].c == it.s[0][0]) {
                hit++;
                CHECK(js8_huff[k].len == (int)strlen(it.s[1]) && js8_huff[k].code == c,
                      "huff '%s' %s", it.s[0], it.s[1]);
            }
        CHECK(hit == 1, "huff char '%s' found %d times", it.s[0], hit);
    }
    CHECK(n == 44, "hufftable %d", n);

    /* checksum_cmds, snr_cmds, alphabets */
    p = block(src, "QMap<int, int> checksum_cmds = {", &end);
    {
        int ck[32];
        memset(ck, 0, sizeof ck);
        while ((p = next_item(p, end, &it)) != 0) ck[it.v[0]] = (int)it.v[1];
        for (code = 0; code < 32; code++)
            CHECK(js8_cmd_checksum(code) == ck[code], "checksum_cmds %d", code);
    }
    CHECK(strstr(src, "QSet<int> snr_cmds = {25, 29};") != 0, "snr_cmds in source");
    for (code = 0; code < 32; code++) CHECK(js8_cmd_is_snr(code) == (code == 25 || code == 29), "snr cmd");
    CHECK(strstr(src, "QString alphanumeric = {\"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@\"};") != 0, "alphanumeric src");
    CHECK_STR(js8_alnum, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@");
    {
        const char *a = strstr(src, "QString alphabet72 = {\"");
        uint8_t f[11];
        char c[13], ref[65];
        int i;
        CHECK(a != 0, "alphabet72 src");
        memcpy(ref, a + strlen("QString alphabet72 = {\""), 64); ref[64] = 0;
        for (i = 0; i < 64; i += 12) {
            int k;
            memset(f, 0, 11);
            for (k = 0; k < 12; k++) setbits(f, 6 * k, 6, (uint64_t)((i + k) & 63));
            js8_frame_chars(f, c);
            for (k = 0; k < 12; k++) CHECK(c[k] == ref[(i + k) & 63], "alphabet72 %d", i + k);
        }
    }
    free(src);
}

/* ---- CRC ------------------------------------------------------------------- */
static void t_crc(void)
{
    uint8_t f[11];
    section("CRC-12 vectors (design section 2)");
    frame_from_chars("000000000000", 0, f);
    CHECK(js8_msg_crc12(f) == 0x02a, "crc %03x", js8_msg_crc12(f));
    frame_from_chars("0123456789AB", 0, f);
    CHECK(js8_msg_crc12(f) == 0x00c, "crc %03x", js8_msg_crc12(f));
    frame_from_chars("HELLOWORLD++", 5, f);
    CHECK(js8_msg_crc12(f) == 0x3fc, "crc %03x", js8_msg_crc12(f));
    CHECK(js8_msg_crc_ok(f), "crc_ok");
    f[3] ^= 0x10;
    CHECK(!js8_msg_crc_ok(f), "crc must fail after a bit flip");
}

/* ---- grid ------------------------------------------------------------------ */
/* Varicode::grid2deg / packGrid / unpackGrid / deg2grid restated with the
 * same float arithmetic as the source (reference for our integer forms) */
static int ref_pack_grid(const char *g4)
{
    char g[7];
    int nlong, n20d, nlat, ilong, ilat;
    float xminlong, dlong, xminlat, dlat;
    memcpy(g, g4, 4); g[4] = 'm'; g[5] = 'm'; g[6] = 0;
    nlong = 180 - 20 * (g[0] - 'A');
    n20d = 2 * (g[2] - '0');
    xminlong = 5 * (g[4] - 'a' + 0.5);
    dlong = nlong - n20d - xminlong / 60.0;
    nlat = -90 + 10 * (g[1] - 'A') + g[3] - '0';
    xminlat = 2.5 * (g[5] - 'a' + 0.5);
    dlat = nlat + xminlat / 60.0;
    ilong = dlong; ilat = dlat + 90;
    return ((ilong + 180) / 2) * 180 + ilat;
}
static void ref_unpack_grid(int value, char *out)
{
    float dlat = value % 180 - 90, dlong = value / 180 * 2 - 180 + 2;
    int nlong, n1, n2, nlat;
    if (dlong < -180) dlong += 360;
    if (dlong > 180) dlong -= 360;
    nlong = (int)(60.0 * (180.0 - dlong) / 5);
    n1 = nlong / 240; n2 = (nlong - 240 * n1) / 24;
    out[0] = (char)('A' + n1); out[2] = (char)('0' + n2);
    nlat = (int)(60.0 * (dlat + 90) / 2.5);
    n1 = nlat / 240; n2 = (nlat - 240 * n1) / 24;
    out[1] = (char)('A' + n1); out[3] = (char)('0' + n2);
    out[4] = 0;
}

static void t_grid(void)
{
    char g[5], r[5];
    int v, bad = 0, a, b, c, d;
    section("grid");
    CHECK(js8_grid_pack("JO70") == 14900, "JO70 -> %u", js8_grid_pack("JO70"));
    CHECK(js8_grid_unpack(14900, g) == 1, "unpack 14900");
    CHECK_STR(g, "JO70");
    for (v = 0; v < 32400; v++) {
        js8_grid_unpack((unsigned)v, g);
        ref_unpack_grid(v, r);
        if (strcmp(g, r) || (int)js8_grid_pack(g) != v) bad++;
    }
    CHECK(bad == 0, "%d of 32400 grid values differ from the source arithmetic / round trip", bad);
    bad = 0;
    for (a = 0; a < 18; a++) for (b = 0; b < 18; b++) for (c = 0; c < 10; c++) for (d = 0; d < 10; d++) {
        g[0] = (char)('A' + a); g[1] = (char)('A' + b); g[2] = (char)('0' + c); g[3] = (char)('0' + d); g[4] = 0;
        if ((int)js8_grid_pack(g) != ref_pack_grid(g)) bad++;
    }
    CHECK(bad == 0, "%d of 32400 grids pack differently from the source arithmetic", bad);
    /* nbasegrid itself: JS8Call's unpackGrid only refuses values ABOVE it and deg2grid wraps to "RA90" */
    CHECK(js8_grid_unpack(32400, g) == 1 && !strcmp(g, "RA90"), "32400 -> RA90 (JS8Call parity), got \"%s\"", g);
    CHECK(js8_grid_unpack(32401, g) == 0 && g[0] == 0, "no grid 32401");
    CHECK(js8_grid_unpack(32767, g) == 0 && g[0] == 0, "no grid 32767");
    CHECK(js8_grid_pack("") == 32767 && js8_grid_pack("JO7") == 32767 && js8_grid_pack("ZZ00") == 32767, "bad grid");
}

/* ---- callsigns ------------------------------------------------------------- */
static uint64_t ref_pack50(const char *call)
{
    /* Varicode::packAlphaNumeric50 restated with 64-bit arithmetic */
    static const char an[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@";
    char w[32];
    int n = 0, i;
    uint64_t m[11], v = 0;
    for (i = 0; call[i]; i++) if (strchr(an, call[i])) w[n++] = call[i];
    if (n > 3 && w[3] != '/') { memmove(w + 4, w + 3, (size_t)(n - 3)); w[3] = ' '; n++; }
    if (n > 7 && w[7] != '/') { memmove(w + 8, w + 7, (size_t)(n - 7)); w[7] = ' '; n++; }
    while (n < 11) w[n++] = ' ';
    m[10] = 1; m[9] = 38; m[8] = 38 * 38; m[7] = 38 * 38 * 38;
    m[6] = m[7] * 2; m[5] = m[6] * 38; m[4] = m[5] * 38; m[3] = m[4] * 38;
    m[2] = m[3] * 2; m[1] = m[2] * 38; m[0] = m[1] * 38;
    for (i = 0; i < 11; i++)
        v += m[i] * (uint64_t)((i == 3 || i == 7) ? (w[i] == '/') : (int)(strchr(an, w[i]) - an));
    return v;
}

static void t_calls(void)
{
    static const char *std[] = { "OK1BT", "Q2XYZ", "Q3ABC", "Q0AA", "Q1AW", "Q4ABC", "0A1AA", "OK1BT/P",
        "3X1A", "Q1Y", "Q0A", "0B3ABC", "0E0ABC", "Q1A/P", "0B4AAA", "Q1AA", 0 };
    /* Test calls: OK1BT is the author's own.  The others cannot be issued: no ITU series starts with Q or with 0
     * (Q + digit: the packer reads Q + letter as 3X + letter, see below).  3X1A is kept for its shape: "3X" followed
     * by a digit must NOT take the Guinea rule.  3DA0XY / 3XA1B exercise the two prefix rules of JS8Call
     * varicode.cpp (packCallsign / unpackCallsign: "3DA0" <-> "3D0", "3X[A-Z]" <-> "Q[A-Z]"), which fire for these
     * two prefixes only; the suffixes are arbitrary.
     * Limit of this list: no call that cannot be issued has the commonest real shape, two letters + digit + three
     * letters; the positions are covered separately (letter first: OK1BT; digit first and a 3-letter suffix: 0B3ABC). */
    /* JS8Call's base_callsign_pattern does not match these (no digit in 2nd/3rd place), so it
     * treats them as compound calls; the 28-bit packer still has the 3DA0/3X workarounds */
    static const char *odd[] = { "3DA0XY", "3XA1B", "QL100A", 0 };
    static const char *cmp[] = { "VE3/OK1BT", "OK1BT/QRP", "OK1BT/MM", "@OKGROUP", "@TEST",
        "Q0A/BCD/QRP", "OK1BT/7", "HB9/OK1BT", "Q1AW/KH6", "9A/OK1BT", "QL100A", "3DA0XY", 0 };
    /* the 50-bit layout has slash flags only at positions 3 and 7: these calls lose their
     * tail in JS8Call too (same 50-bit value as the 64-bit restatement of the source) */
    static const char *trunc[] = { "9A/OK1BT/P", "EA8/Q4ABC/P", 0 };
    static const char *bad[] = { "", "A", "HELLO", "WORLD", "12345", "A1", "CQ", "TOOLONGCALL1", "OK1BT.",
        "0A4CRD/QRPP", 0 };
    char c[JS8_CALL_MAX];
    uint32_t v;
    uint16_t l[4];
    int i, por, n, fail;

    section("callsigns");
    for (i = 0; std[i]; i++) {
        CHECK(js8_call_kind(std[i]) == 1, "kind %s = %d", std[i], js8_call_kind(std[i]));
        CHECK(js8_call28_pack(std[i], &v, &por) == 1 && v < JS8_NBASECALL, "pack %s", std[i]);
        CHECK(js8_call28_unpack(v, por, c) == 1, "unpack %s", std[i]);
        CHECK_STR(c, std[i]);
    }
    for (i = 0; odd[i]; i++) CHECK(js8_call_kind(odd[i]) == 2, "kind %s = %d", odd[i], js8_call_kind(odd[i]));
    CHECK(js8_call28_pack("3DA0XY", &v, &por) && js8_call28_unpack(v, por, c) && !strcmp(c, "3DA0XY"), "3DA0XY 28-bit");
    CHECK(js8_call28_pack("3XA1B", &v, &por) && js8_call28_unpack(v, por, c) && !strcmp(c, "3XA1B"), "3XA1B 28-bit");
    CHECK(js8_call28_pack("QL100A", &v, &por) == 0, "QL100A has no 28-bit form");
    /* hand-computed value: "OK1BT " = ((((24*36+20)*10+1)*27+1)*27+19)*27+26 */
    js8_call28_pack("OK1BT", &v, &por);
    CHECK(v == ((((24u * 36 + 20) * 10 + 1) * 27 + 1) * 27 + 19) * 27 + 26, "OK1BT value %u", v);
    for (i = 1; i <= JS8_NGROUPS; i++) {
        const char *g = js8_group_name(i);
        CHECK(js8_call28_pack(g, &v, &por) && v == JS8_NBASECALL + (uint32_t)i, "group %s", g);
        CHECK(js8_call28_unpack(v, 1, c), "group unpack");
        CHECK_STR(c, g);
        if (i > 1) CHECK(js8_call_kind(g) == 1, "group kind %s", g);
    }
    /* values >= nbasecall outside the group table: unpacked as JS8Call does (first character = alphabet
     * index 37 = '/'); expected strings from the independent model in tests/rv_protocol.py */
    CHECK(js8_call28_unpack(JS8_NBASECALL, 0, c) == 1, "nbasecall itself"); CHECK_STR(c, "/00AAA");
    CHECK(js8_call28_unpack(JS8_NBASECALL + 55, 0, c) == 1, "nbasecall+55"); CHECK_STR(c, "/00ACB");
    CHECK(js8_call28_unpack(0x0FFFFFFFu, 0, c) == 1, "all ones"); CHECK_STR(c, "/V7ZFY");
    CHECK(js8_call28_unpack(0x0FFFFFFFu, 1, c) == 1, "all ones /P"); CHECK_STR(c, "/V7ZFY/P");
    CHECK(js8_call28_unpack(0x10000000u, 0, c) == 0 && c[0] == 0, "not a 28-bit value");
    /* random standard calls, generated from the pattern */
    fail = 0;
    for (n = 0; n < 20000; n++) {
        char s[12], t[JS8_CALL_MAX];
        int k = 0, sfx = (int)rn(4), j;
        static const char an[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        if (rn(2)) s[k++] = an[rn(36)];
        s[k++] = an[rn(36)];
        s[k++] = an[rn(10)];
        for (j = 0; j < sfx; j++) s[k++] = (char)('A' + rn(26));
        s[k] = 0;
        if (k < 2) continue;
        if (!js8_call28_pack(s, &v, &por)) { if (strcmp(s, "000") == 0 || k < 2) continue; if (k == 2 || 1) { /* "00" etc. pack to 0 */
            int allz = 1; for (j = 0; j < k; j++) if (s[j] != '0') allz = 0; if (allz) continue; } fail++; continue; }
        js8_call28_unpack(v, 0, t);
        /* 3D0x and Q[A-Z]x are aliases of 3DA0x / 3X[A-Z]x by design of the format */
        if (strncmp(s, "3D0", 3) == 0 || (s[0] == 'Q' && s[1] >= 'A' && k > 1 && s[1] <= 'Z')) continue;
        if (strcmp(s, t)) { if (fail < 5) printf("  call %s -> %u -> %s\n", s, v, t); fail++; }
    }
    CHECK(fail == 0, "%d random standard calls failed the round trip", fail);

    for (i = 0; cmp[i]; i++) {
        uint64_t ref = ref_pack50(cmp[i]), got;
        CHECK(js8_call_kind(cmp[i]) == 2, "kind %s = %d", cmp[i], js8_call_kind(cmp[i]));
        CHECK(js8_call50_pack(cmp[i], l) == 1, "pack50 %s", cmp[i]);
        got = (uint64_t)l[3] << 48 | (uint64_t)l[2] << 32 | (uint64_t)l[1] << 16 | l[0];
        CHECK(got == ref && got < (1ull << 50), "pack50 %s: %llx vs %llx", cmp[i],
              (unsigned long long)got, (unsigned long long)ref);
        js8_call50_unpack(l, c);
        CHECK_STR(c, cmp[i]);
    }
    for (i = 0; trunc[i]; i++) {
        uint64_t ref = ref_pack50(trunc[i]), g2;
        js8_call50_pack(trunc[i], l);
        g2 = (uint64_t)l[3] << 48 | (uint64_t)l[2] << 32 | (uint64_t)l[1] << 16 | l[0];
        CHECK(g2 == ref, "pack50 %s", trunc[i]);
        js8_call50_unpack(l, c);
        CHECK(strlen(c) == strlen(trunc[i]) - 1 && !strncmp(c, trunc[i], strlen(c)), "truncated %s -> %s", trunc[i], c);
    }
    for (i = 0; std[i]; i++) {          /* standard calls travel in compound frames too */
        CHECK(js8_call50_pack(std[i], l) == 1, "pack50 %s", std[i]);
        js8_call50_unpack(l, c);
        CHECK_STR(c, std[i]);
    }
    fail = 0;
    for (n = 0; n < 20000; n++) {       /* random 50-bit strings vs 64-bit reference */
        static const char an[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/";
        char s[14];
        int k = (int)rn(11) + 1, j;
        uint64_t ref, got;
        for (j = 0; j < k; j++) s[j] = an[rn(37)];
        if (rn(8) == 0) s[0] = '@';
        s[k] = 0;
        ref = ref_pack50(s);
        js8_call50_pack(s, l);
        got = (uint64_t)l[3] << 48 | (uint64_t)l[2] << 32 | (uint64_t)l[1] << 16 | l[0];
        if (got != ref) fail++;
    }
    CHECK(fail == 0, "%d random strings: 16-bit-limb pack50 differs from 64-bit reference", fail);
    for (i = 0; bad[i]; i++) CHECK(js8_call_kind(bad[i]) == 0, "kind '%s' = %d", bad[i], js8_call_kind(bad[i]));
}

/* ---- single frames: text exactly as DecodedText::message() ------------------ */
static void t_frames(void)
{
    uint8_t f[11];
    js8_msg_t m;

    section("frame text");
    CHECK(js8_pack_heartbeat("OK1BT", "JO70", 0, 0, 3, f), "pack hb");
    CHECK(js8_msg_crc_ok(f), "crc");
    CHECK(js8_msg_decode(f, 0, &m) == JS8_FT_HEARTBEAT && !m.is_cq && m.i3 == 3, "hb type");
    CHECK_STR(m.text, "OK1BT: @HB HEARTBEAT JO70 ");
    CHECK_STR(m.from, "OK1BT"); CHECK_STR(m.to, "@HB"); CHECK_STR(m.grid, "JO70");
    js8_pack_heartbeat("OK1BT", 0, 0, 0, 3, f);
    js8_msg_decode(f, 0, &m);
    CHECK_STR(m.text, "OK1BT: @HB HEARTBEAT  ");
    js8_pack_heartbeat("OK1BT", "JO70", 1, 0, 3, f);
    js8_msg_decode(f, 0, &m);
    CHECK(m.is_cq == 1, "cq");
    CHECK_STR(m.text, "OK1BT: @ALLCALL CQ CQ CQ JO70 ");
    CHECK_STR(m.to, "@ALLCALL");
    js8_pack_heartbeat("OK1BT/QRP", "JN75", 1, 3, 3, f);
    js8_msg_decode(f, 0, &m);
    CHECK_STR(m.text, "OK1BT/QRP: @ALLCALL CQ CONTEST JN75 ");
    /* grid field 32400 -> "RA90" as JS8Call; 32401 -> no grid */
    memset(f, 0, 11);
    { uint16_t l[4]; js8_call50_pack("OK1BT", l);
      setbits(f, 3, 2, l[3]); setbits(f, 5, 16, l[2]); setbits(f, 21, 16, l[1]); setbits(f, 37, 16, l[0]);
      setbits(f, 53, 16, 32400); js8_msg_crc_set(f); }
    js8_msg_decode(f, 0, &m);
    CHECK_STR(m.text, "OK1BT: @HB HEARTBEAT RA90 ");
    setbits(f, 53, 16, 32401); js8_msg_crc_set(f);
    js8_msg_decode(f, 0, &m);
    CHECK_STR(m.text, "OK1BT: @HB HEARTBEAT  ");

    js8_pack_compound("OK1BT/QRP", "JO70", 1, f);
    CHECK(js8_msg_decode(f, 0, &m) == JS8_FT_COMPOUND, "compound type");
    CHECK_STR(m.text, "OK1BT/QRP: ");
    CHECK_STR(m.from, "OK1BT/QRP"); CHECK_STR(m.grid, "JO70");

    js8_pack_compound_directed("VE3/Q2XYZ", 14, 0, 0, 2, f);
    CHECK(js8_msg_decode(f, 0, &m) == JS8_FT_COMPOUND_DIRECTED && m.cmd == 14, "cd type");
    CHECK_STR(m.text, "VE3/Q2XYZ ACK ");
    CHECK_STR(m.from, "<....>"); CHECK_STR(m.to, "VE3/Q2XYZ");
    js8_pack_compound_directed("VE3/Q2XYZ", 25, 1, -12, 2, f);
    js8_msg_decode(f, 0, &m);
    CHECK_STR(m.text, "VE3/Q2XYZ SNR -12 ");
    CHECK(m.cmd == 25 && m.has_num && m.num == -12, "cd snr");
    js8_pack_compound_directed("VE3/Q2XYZ", 29, 1, 5, 2, f);
    js8_msg_decode(f, 0, &m);
    CHECK_STR(m.text, "VE3/Q2XYZ HEARTBEAT SNR +05 ");

    CHECK(js8_pack_directed("OK1BT", "Q2XYZ", 9, 0, 0, 1, f), "pack directed");
    CHECK(js8_msg_decode(f, 0, &m) == JS8_FT_DIRECTED, "dir type");
    CHECK_STR(m.text, "OK1BT: Q2XYZ MSG ");
    js8_pack_directed("OK1BT", "Q2XYZ", 25, 1, -12, 3, f);
    js8_msg_decode(f, 0, &m);
    CHECK_STR(m.text, "OK1BT: Q2XYZ SNR -12 ");
    js8_pack_directed("OK1BT/P", "Q2XYZ/P", 29, 1, 7, 3, f);
    js8_msg_decode(f, 0, &m);
    CHECK_STR(m.text, "OK1BT/P: Q2XYZ/P HEARTBEAT SNR +07 ");
    js8_pack_directed("OK1BT", "@ALLCALL", 0, 0, 0, 3, f);
    js8_msg_decode(f, 0, &m);
    CHECK_STR(m.text, "OK1BT: @ALLCALL SNR? ");
    js8_pack_directed("OK1BT", "Q2XYZ", 5, 0, 0, 3, f);
    js8_msg_decode(f, 0, &m);
    CHECK_STR(m.text, "OK1BT: Q2XYZ> ");
    js8_pack_directed("OK1BT", "Q2XYZ", 31, 0, 0, 1, f);
    js8_msg_decode(f, 0, &m);
    CHECK_STR(m.text, "OK1BT: Q2XYZ  ");
    js8_pack_directed("<....>", "Q2XYZ", 14, 0, 0, 1, f);
    js8_msg_decode(f, 0, &m);
    CHECK_STR(m.text, "<....>: Q2XYZ ACK ");
    /* non-SNR command with a number field: plain integer */
    js8_pack_directed("OK1BT", "Q2XYZ", 14, 1, 5, 3, f);
    js8_msg_decode(f, 0, &m);
    CHECK_STR(m.text, "OK1BT: Q2XYZ ACK 5 ");
    /* every command code in a directed frame */
    { int code; char exp[64];
      for (code = 0; code < 32; code++) {
          js8_pack_directed("OK1BT", "Q2XYZ", code, 0, 0, 3, f);
          js8_msg_decode(f, 0, &m);
          snprintf(exp, sizeof exp, "OK1BT: Q2XYZ%s ", js8_cmd_str(code));
          CHECK(m.cmd == code, "cmd %d", code);
          CHECK_STR(m.text, exp);
      } }
    /* directed frame with a 28-bit call above nbasecall: shown as JS8Call shows it */
    memset(f, 0, 11);
    setbits(f, 0, 3, 3); setbits(f, 3, 28, JS8_NBASECALL + 100); setbits(f, 31, 28, 12345);
    js8_msg_crc_set(f);
    CHECK(js8_msg_decode(f, 0, &m) == JS8_FT_DIRECTED, "call above nbasecall");
    CHECK_STR(m.text, "/00ADT: 000QZG SNR? ");
    /* compound-directed SNR command without the number flag: JS8Call appends formatSNR(0 - 31) */
    memset(f, 0, 11);
    { uint16_t l[4]; js8_call50_pack("OK1BT", l);
      setbits(f, 0, 3, 2); setbits(f, 3, 2, l[3]); setbits(f, 5, 16, l[2]); setbits(f, 21, 16, l[1]); setbits(f, 37, 16, l[0]);
      setbits(f, 53, 16, 32410 + 25); js8_msg_crc_set(f); }
    CHECK(js8_msg_decode(f, 0, &m) == JS8_FT_COMPOUND_DIRECTED, "compound directed");
    CHECK_STR(m.text, "OK1BT SNR -31 ");
    /* data flag set but nothing decodable: raw frame, never a heartbeat */
    memset(f, 0xff, 9); f[0] = 0x7F; f[9] = 4 << 5; f[10] = 0; js8_msg_crc_set(f);
    CHECK(js8_msg_decode(f, 0, &m) == JS8_FT_UNKNOWN, "fast data empty");
    CHECK_STR(m.text, "V+++++++++++");
    /* fast data, all bits zero: fourteen times word 0 (5-bit codes, no blank flag) */
    memset(f, 0, 11); f[9] = 4 << 5; js8_msg_crc_set(f);
    CHECK(js8_msg_decode(f, 0, &m) == JS8_FT_DATA_FAST && (m.flags & JS8_MF_NODICT), "fast zeros");
    CHECK_STR(m.text, "{?}{?}{?}{?}{?}{?}{?}{?}{?}{?}{?}{?}{?}{?}");
    /* Huffman data: 1,0 + "HELLO" + pad */
    memset(f, 0, 11);
    setbits(f, 0, 2, 2);
    { int nb = 2, i, k; const char *t = "HELLO WORLD";
      for (i = 0; t[i]; i++) for (k = 0; k < 44; k++) if (js8_huff[k].c == t[i]) {
          setbits(f, nb, js8_huff[k].len, js8_huff[k].code); nb += js8_huff[k].len; }
      for (i = nb + 1; i < 72; i++) setbits(f, i, 1, 1);
      f[9] = 3 << 5; js8_msg_crc_set(f); }
    CHECK(js8_msg_decode(f, 0, &m) == JS8_FT_DATA, "huff type");
    CHECK_STR(m.text, "HELLO WORLD");
    /* all payload ones: Huffman "11111" = O, fourteen times (70 bits) */
    memset(f, 0xff, 9); f[0] = 0xBF; f[9] = 0; f[10] = 0; js8_msg_crc_set(f);
    js8_msg_decode(f, 0, &m);
    CHECK_STR(m.text, "OOOOOOOOOOOOOO");
}

/* ---- Huffman round trip ----------------------------------------------------- */
static void t_huff(void)
{
    int n, fail = 0, frames_total = 0;
    section("Huffman round trip (no dictionary)");
    for (n = 0; n < 2000; n++) {
        char t[80], out[200];
        uint8_t f[11];
        js8_msg_t m;
        int len = (int)rn(60) + 1, i, p = 0;
        for (i = 0; i < len; i++) t[i] = js8_huff[rn(44)].c;
        t[len] = 0;
        out[0] = 0;
        while (p < len) {
            int k = js8_pack_data(t + p, len - p, 0, 0, f);
            if (k <= 0) break;
            if (!js8_msg_crc_ok(f)) fail++;
            js8_msg_decode(f, 0, &m);
            if (m.type != JS8_FT_DATA) fail++;
            strcat(out, m.text);
            p += k; frames_total++;
        }
        if (p != len || strcmp(out, t)) { if (fail < 5) printf("  huff '%s' -> '%s'\n", t, out); fail++; }
    }
    CHECK(fail == 0, "%d failures", fail);
    printf("  2000 random strings, %d frames\n", frames_total);
}

/* ---- JSC dictionary --------------------------------------------------------- */
static js8jsc_t jsc;
static uint8_t *headmem;

static void codeword(uint8_t *f, int *nb, uint32_t index, int sep)
{
    /* JSC::codeword restated */
    uint8_t d[8];
    int k = 0;
    uint32_t x = index / 7;
    while (x > 0) { x -= 1; d[k++] = (uint8_t)(x % 9 + 7); x /= 9; }
    while (k-- > 0) { setbits(f, *nb, 4, d[k]); *nb += 4; }
    setbits(f, *nb, 5, (index % 7) << 1 | (unsigned)sep); *nb += 5;
}

static void t_jsc(void)
{
    static const uint32_t sizes[3] = { 0, 4096, 40000 };
    int v, i, fail;
    char w[32];

    section("JSC dictionary files vs jsc_list.cpp");
    headmem = malloc(40000);
    for (v = 0; v < 3; v++) {
        uint32_t maxb = 0, step = v == 2 ? 1 : 37;
        uint64_t sum = 0;
        uint32_t cnt = 0, opens;
        int r = js8jsc_init(&jsc, &io, sizes[v] ? headmem : 0, sizes[v]);
        CHECK(r == 0, "init (head mem %u) = %d", sizes[v], r);
        if (r) return;
        CHECK(jsc.total == NJSC, "total %u", jsc.total);
        fail = 0;
        opens = jsc.st_opens;
        for (i = 0; i < NJSC; i += (int)step) {
            int len, exp = map_size(i);
            uint32_t before = jsc.st_bytes;
            /* defeat the word LRU so every lookup is a real file read */
            int k; for (k = 0; k < JS8JSC_CACHE_N; k++) jsc.cache[k].idx = 0xFFFFFFFFu;
            len = js8jsc_word(&jsc, (uint32_t)i, w, sizeof w);
            if (len != exp || memcmp(w, jlist[by_index[i]].s, (size_t)exp)) {
                if (fail < 5) printf("  index %d: got len %d, expected %.*s\n", i, len, exp, jlist[by_index[i]].s);
                fail++;
            }
            if ((uint32_t)i >= jsc.head_n) {
                uint32_t b = jsc.st_bytes - before;
                if (b > maxb) maxb = b;
                sum += b; cnt++;
            }
        }
        CHECK(fail == 0, "%d wrong words", fail);
        printf("  head mem %5u B: %u entries in RAM (%u B); %u indices checked (step %u), %u file lookups: "
               "avg %llu B, worst %u B read per lookup, %u opens\n",
               sizes[v], jsc.head_n, jsc.head_bytes, (unsigned)((NJSC + step - 1) / step), step, cnt,
               (unsigned long long)(cnt ? sum / cnt : 0), maxb, jsc.st_opens - opens);
    }
    /* the two size anomalies follow JS8Call-improved (size honoured) */
    i = js8jsc_word(&jsc, 81, w, sizeof w); w[i > 0 ? i : 0] = 0;
    CHECK_STR(w, "@ALLCAL");
    i = js8jsc_word(&jsc, 262143, w, sizeof w); w[i > 0 ? i : 0] = 0;
    CHECK_STR(w, "R");
    CHECK(js8jsc_word(&jsc, 262144, w, sizeof w) < 0, "index out of range");
    /* LRU cache: second read of the same word costs no file access */
    js8jsc_init(&jsc, &io, 0, 0);
    js8jsc_word(&jsc, 123456, w, sizeof w);
    { uint32_t b = jsc.st_bytes; js8jsc_word(&jsc, 123456, w, sizeof w);
      CHECK(jsc.st_bytes == b && jsc.st_cache_hits == 1, "LRU hit"); }

    section("JSC encode lookup vs JSC::lookup restated");
    js8jsc_init(&jsc, &io, headmem, 40000);
    {
        uint32_t maxb = 0, cnt = 0, miss = 0;
        uint64_t sum = 0;
        fail = 0;
        for (i = 0; i < 60000; i++) {
            char s[40];
            int len, ri = -1, rr, r, adv = 0, kind = (int)rn(4);
            uint32_t gi = 0, before;
            const tup_t *t = &jlist[rn(NJSC)];
            len = t->len;
            memcpy(s, t->s, (size_t)len);
            if (kind == 1) { int e = (int)rn(4) + 1; while (e--) s[len++] = (char)('A' + rn(26)); }
            else if (kind == 2 && len > 1) len = (int)rn((uint32_t)len) + 1;
            else if (kind == 3) { len = (int)rn(8) + 1; for (r = 0; r < len; r++) s[r] = (char)(33 + rn(60)); }
            s[len] = 0;
            if (strchr(s, ' ')) continue;                 /* words never contain a blank */
            rr = ref_lookup(s, &ri);
            before = jsc.st_bytes;
            r = js8jsc_lookup(&jsc, s, len, &gi, &adv);
            { uint32_t b = jsc.st_bytes - before; if (b > maxb) maxb = b; sum += b; cnt++; }
            if (!rr) miss++;
            if (r != rr || (rr && ((int)gi != ri || adv != map_size(ri)))) {
                if (fail < 5) printf("  lookup '%s': got %d idx %u adv %d, ref %d idx %d\n", s, r, gi, adv, rr, ri);
                fail++;
            }
        }
        CHECK(fail == 0, "%d of %u lookups differ", fail, cnt);
        printf("  %u lookups (%u without any match): avg %llu B, worst %u B read per lookup\n",
               cnt, miss, (unsigned long long)(sum / cnt), maxb);
        /* every single-character prefix entry */
        fail = 0;
        for (i = 0; i < njprefix; i++) {
            int ri = -1, adv; uint32_t gi;
            if (!ref_lookup(jprefix[i].s, &ri)) continue;
            if (js8jsc_lookup(&jsc, jprefix[i].s, 1, &gi, &adv) != 1 || (int)gi != ri) fail++;
        }
        CHECK(fail == 0, "%d single-character lookups differ", fail);
    }

    section("JSC data frames");
    {
        uint8_t f[11];
        js8_msg_t m;
        int nb, k;
        /* normal data frame: 1,1 + codewords + pad */
        memset(f, 0, 11); setbits(f, 0, 2, 3); nb = 2;
        codeword(f, &nb, 100, 1); codeword(f, &nb, 5, 0); codeword(f, &nb, 70000, 1); codeword(f, &nb, 3, 0);
        for (k = nb + 1; k < 72; k++) setbits(f, k, 1, 1);
        f[9] = 1 << 5; js8_msg_crc_set(f);
        CHECK(js8_msg_decode(f, &jsc, &m) == JS8_FT_DATA_COMPRESSED, "jsc type %d", m.type);
        { char exp[128]; snprintf(exp, sizeof exp, "%.*s %.*s%.*s %.*s",
              map_size(100), jlist[by_index[100]].s, map_size(5), jlist[by_index[5]].s,
              map_size(70000), jlist[by_index[70000]].s, map_size(3), jlist[by_index[3]].s);
          CHECK_STR(m.text, exp); printf("  sample: \"%s\"\n", m.text); }
        /* fast data frame (i3bit data flag): all 72 bits */
        memset(f, 0, 11); nb = 0;
        codeword(f, &nb, 262143, 1); codeword(f, &nb, 81, 0); codeword(f, &nb, 0, 1);
        for (k = nb + 1; k < 72; k++) setbits(f, k, 1, 1);
        f[9] = 7 << 5; js8_msg_crc_set(f);
        CHECK(js8_msg_decode(f, &jsc, &m) == JS8_FT_DATA_FAST, "fast type %d", m.type);
        { char exp[64]; snprintf(exp, sizeof exp, "R @ALLCAL%.*s ", map_size(0), jlist[by_index[0]].s);
          CHECK_STR(m.text, exp); }
        /* without the dictionary: placeholder and flag */
        js8_msg_decode(f, 0, &m);
        CHECK((m.flags & JS8_MF_NODICT) && strcmp(m.text, "{?} {?}{?} ") == 0, "nodict '%s'", m.text);
        /* longest possible text of one frame (mkjsc --stats: 82 characters) */
        {
            int best13 = -1, best25 = -1, bl13 = 0, bl25 = 0, idx;
            for (idx = 70; idx < 637; idx++) if (map_size(idx) > bl13) { bl13 = map_size(idx); best13 = idx; }
            for (idx = 51667; idx < NJSC; idx++) if (map_size(idx) > bl25) { bl25 = map_size(idx); best25 = idx; }
            memset(f, 0, 11); nb = 0;
            codeword(f, &nb, (uint32_t)best25, 1); codeword(f, &nb, (uint32_t)best25, 1);
            codeword(f, &nb, (uint32_t)best13, 1);
            for (k = nb + 1; k < 72; k++) setbits(f, k, 1, 1);
            f[9] = 7 << 5; js8_msg_crc_set(f);
            js8_msg_decode(f, &jsc, &m);
            CHECK((int)strlen(m.text) == 2 * (bl25 + 1) + bl13 + 1 && !(m.flags & JS8_MF_TRUNC),
                  "long frame text %d chars", (int)strlen(m.text));
            printf("  longest words: %d + %d chars; frame text %d chars (buffer %d)\n",
                   bl25, bl13, (int)strlen(m.text), JS8_TEXT_MAX);
        }
    }
    section("random frames (robustness)");
    {
        int n, maxlen = 0, types[256];
        memset(types, 0, sizeof types);
        for (n = 0; n < 100000; n++) {
            uint8_t f[11];
            js8_msg_t m;
            int k, l;
            for (k = 0; k < 9; k++) f[k] = (uint8_t)rnd();
            f[9] = (uint8_t)(rn(8) << 5); f[10] = 0;
            js8_msg_crc_set(f);
            js8_msg_decode(f, &jsc, &m);
            l = (int)strlen(m.text);
            if (l > maxlen) maxlen = l;
            types[m.type]++;
            if (l >= JS8_TEXT_MAX || strlen(m.from) >= JS8_CALL_MAX || strlen(m.to) >= JS8_CALL_MAX) { CHECK(0, "overflow"); break; }
        }
        CHECK(maxlen < JS8_TEXT_MAX, "max text %d", maxlen);
        printf("  100000 random frames: hb/cq %d, compound %d, compound-directed %d, directed %d, huffman %d, "
               "jsc %d, fast %d, raw %d; longest text %d\n", types[0], types[1], types[2], types[3], types[4],
               types[6], types[7], types[255], maxlen);
    }
}

/* ---- reassembly -------------------------------------------------------------- */
static js8asm_msg_t got[64];
static int ngot;
static void emit(void *u, const js8asm_msg_t *m) { (void)u; if (ngot < 64) got[ngot] = *m; ngot++; }

static js8asm_t *asm_;
static void feed_frames(uint8_t fr[][11], int n, int freq, int snr, uint32_t *t)
{
    int i;
    for (i = 0; i < n; i++) {
        js8_msg_t m;
        js8_msg_decode(fr[i], &jsc, &m);
        js8asm_feed(asm_, &m, freq, snr, *t);
        *t += 15;
        js8asm_tick(asm_, *t);
    }
}

static void t_asm_review(void);
static void t_asm(void)
{
    static js8asm_t a;
    uint8_t fr[40][11], fr2[40][11];
    js8_build_t cfg = { "Q2XYZ", "JN89", 0, &jsc };
    js8_msg_t m;
    uint32_t t = 1000;
    int n, n2, i;

    section("reassembly");
    asm_ = &a;
    js8asm_init(&a, "OK1BT", "@ALLCALL @OK", emit, 0);
    CHECK(js8asm_to_me(&a, "OK1BT") == JS8ASM_F_TO_ME && js8asm_to_me(&a, "@OK") == JS8ASM_F_TO_GROUP &&
          js8asm_to_me(&a, "@ALLCALL") == JS8ASM_F_TO_GROUP && js8asm_to_me(&a, "@OKX") == 0 &&
          js8asm_to_me(&a, "OK1BT0") == 0 && js8asm_to_me(&a, "@HB") == 0, "to_me");

    /* 1: directed MSG with text and checksum, to me */
    n = js8_build_message(&cfg, "OK1BT MSG HELLO TOM, MEETING AT 1800 UTC ON 7078", fr, 40);
    CHECK(n >= 3, "frames %d", n);
    ngot = 0; feed_frames(fr, n, 1500, -7, &t);
    CHECK(ngot == 1, "messages %d", ngot);
    CHECK_STR(got[0].text, "Q2XYZ: OK1BT MSG HELLO TOM, MEETING AT 1800 UTC ON 7078");
    CHECK(got[0].flags == (JS8ASM_F_TO_ME | JS8ASM_F_CRC_OK), "flags %02x", got[0].flags);
    CHECK(got[0].frames == n && got[0].cmd == 9 && got[0].snr == -7 && got[0].freq == 1500, "meta");
    CHECK_STR(got[0].from, "Q2XYZ"); CHECK_STR(got[0].to, "OK1BT");

    /* 2: same message with one data frame corrupted -> checksum fails */
    ngot = 0;
    for (i = 0; i < n; i++) {
        js8_msg_decode(fr[i], &jsc, &m);
        if (i == 1) m.text[0] = m.text[0] == 'X' ? 'Y' : 'X';
        js8asm_feed(&a, &m, 1500, -7, t); t += 15;
    }
    CHECK(ngot == 1 && (got[0].flags & JS8ASM_F_CRC_BAD) && !(got[0].flags & JS8ASM_F_CRC_OK), "bad crc flags %02x", got[0].flags);

    /* 3: two interleaved transmissions on different frequencies, with drift */
    n = js8_build_message(&cfg, "OK1BT MSG FIRST STREAM TEXT HERE", fr, 40);
    cfg.mycall = "Q1ABC";
    n2 = js8_build_message(&cfg, "HELLO WORLD THIS IS FREE TEXT FOR EVERYONE OK1BT PSE K", fr2, 40);
    cfg.mycall = "Q2XYZ";
    ngot = 0;
    for (i = 0; i < n || i < n2; i++) {
        if (i < n)  { js8_msg_decode(fr[i], &jsc, &m);  js8asm_feed(&a, &m, 1500 + 3 * i, -5, t); }
        if (i < n2) { js8_msg_decode(fr2[i], &jsc, &m); js8asm_feed(&a, &m, 1535 - 2 * i, -15 - i, t); }
        t += 15; js8asm_tick(&a, t);
    }
    CHECK(ngot == 2, "messages %d", ngot);
    for (i = 0; i < ngot && i < 2; i++) {
        if (got[i].cmd == 9) {
            CHECK_STR(got[i].text, "Q2XYZ: OK1BT MSG FIRST STREAM TEXT HERE");
            CHECK(got[i].flags == (JS8ASM_F_TO_ME | JS8ASM_F_CRC_OK), "flags %02x", got[i].flags);
        } else {
            CHECK_STR(got[i].text, "HELLO WORLD THIS IS FREE TEXT FOR EVERYONE OK1BT PSE K");
            CHECK(got[i].flags == JS8ASM_F_MENTION, "flags %02x", got[i].flags);
            CHECK(got[i].snr == -15 - (n2 - 1), "min snr %d", got[i].snr);
        }
    }

    /* 4: last frame lost -> closed by timeout after 60 s */
    n = js8_build_message(&cfg, "OK1BT MSG THE END OF THIS MESSAGE WILL BE LOST", fr, 40);
    ngot = 0;
    feed_frames(fr, n - 1, 1200, -3, &t);
    CHECK(ngot == 0, "nothing yet");
    js8asm_tick(&a, t + 30); CHECK(ngot == 0, "not after 45 s");
    js8asm_tick(&a, t + 50);
    CHECK(ngot == 1 && (got[0].flags & JS8ASM_F_TIMEOUT) && (got[0].flags & JS8ASM_F_TO_ME) &&
          !(got[0].flags & (JS8ASM_F_CRC_OK | JS8ASM_F_CRC_BAD)), "timeout flags %02x n %d", got[0].flags, ngot);
    CHECK(strncmp(got[0].text, "Q2XYZ: OK1BT MSG THE END", 24) == 0, "text '%s'", got[0].text);
    t += 100;

    /* 5: a First frame clears what was buffered on the frequency */
    n = js8_build_message(&cfg, "OK1BT MSG INTERRUPTED TRANSMISSION NUMBER ONE", fr, 40);
    n2 = js8_build_message(&cfg, "@OK MSG SECOND", fr2, 40);
    ngot = 0;
    feed_frames(fr, 2, 900, -3, &t);
    feed_frames(fr2, n2, 904, -3, &t);
    CHECK(ngot == 2, "messages %d", ngot);
    CHECK((got[0].flags & JS8ASM_F_TIMEOUT) != 0, "first is incomplete");
    CHECK_STR(got[1].text, "Q2XYZ: @OK MSG SECOND");
    CHECK(got[1].flags == (JS8ASM_F_TO_GROUP | JS8ASM_F_CRC_OK), "flags %02x", got[1].flags);

    /* 6: beginning missed (no First) -> partial */
    n = js8_build_message(&cfg, "HELLO THIS IS A LONGER FREE TEXT MESSAGE", fr, 40);
    ngot = 0;
    feed_frames(fr + 1, n - 1, 2000, -3, &t);
    CHECK(ngot == 1 && (got[0].flags & JS8ASM_F_PARTIAL), "partial flags %02x", got[0].flags);

    /* 7: frequencies 25 Hz apart are different streams; 8 Hz apart the same */
    n = js8_build_message(&cfg, "ABCD EFGH IJKL MNOP QRST UVWX YZ", fr, 40);
    ngot = 0;
    js8_msg_decode(fr[0], &jsc, &m); js8asm_feed(&a, &m, 1000, 0, t);
    js8_msg_decode(fr[1], &jsc, &m); js8asm_feed(&a, &m, 1025, 0, t + 15);
    for (i = 1; i < n; i++) { js8_msg_decode(fr[i], &jsc, &m); js8asm_feed(&a, &m, 1000 + 8 * i, 0, t + 15 * i); }
    CHECK(ngot == 1, "messages %d", ngot);
    CHECK_STR(got[0].text, "ABCD EFGH IJKL MNOP QRST UVWX YZ");
    CHECK(got[0].flags == 0, "flags %02x", got[0].flags);
    t += 15 * n;
    ngot = 0; js8asm_tick(&a, t + 100);
    CHECK(ngot == 1 && (got[0].flags & (JS8ASM_F_PARTIAL | JS8ASM_F_TIMEOUT)) == (JS8ASM_F_PARTIAL | JS8ASM_F_TIMEOUT),
          "stray frame at +25 Hz: n %d flags %02x", ngot, got[0].flags);
    t += 200;

    /* 8: heartbeat and CQ are complete at once */
    cfg.mycall = "Q2XYZ";
    n = js8_build_message(&cfg, "HB JN89", fr, 40);
    CHECK(n == 1, "hb frames %d", n);
    ngot = 0; feed_frames(fr, n, 700, -20, &t);
    CHECK(ngot == 1, "hb");
    CHECK_STR(got[0].text, "Q2XYZ: @HB HEARTBEAT JN89");
    CHECK(got[0].flags == 0, "hb flags %02x", got[0].flags);
    n = js8_build_message(&cfg, "CQ CQ CQ JN89", fr, 40);
    ngot = 0; feed_frames(fr, n, 700, -20, &t);
    CHECK_STR(got[0].text, "Q2XYZ: @ALLCALL CQ CQ CQ JN89");
    CHECK(got[0].flags == JS8ASM_F_TO_GROUP, "cq flags %02x", got[0].flags);
    /* CQ followed by text */
    n = js8_build_message(&cfg, "CQ CQ CQ JN89 PSE K", fr, 40);
    CHECK(n == 2, "cq+text frames %d", n);
    ngot = 0; feed_frames(fr, n, 700, -20, &t);
    CHECK_STR(got[0].text, "Q2XYZ: @ALLCALL CQ CQ CQ JN89 PSE K");

    /* 9: compound calls: compound frame + compound directed frame + data */
    cfg.mycall = "VE3/Q2XYZ";
    n = js8_build_message(&cfg, "OK1BT MSG VIA COMPOUND CALL", fr, 40);
    js8_msg_decode(fr[0], &jsc, &m); CHECK(m.type == JS8_FT_COMPOUND && m.i3 == 1, "frame 0 compound");
    js8_msg_decode(fr[1], &jsc, &m); CHECK(m.type == JS8_FT_COMPOUND_DIRECTED, "frame 1 compound directed");
    ngot = 0; feed_frames(fr, n, 1800, -9, &t);
    CHECK(ngot == 1, "messages %d", ngot);
    CHECK_STR(got[0].text, "VE3/Q2XYZ: OK1BT MSG VIA COMPOUND CALL");
    CHECK(got[0].flags == (JS8ASM_F_TO_ME | JS8ASM_F_CRC_OK), "flags %02x", got[0].flags);
    cfg.mycall = "Q2XYZ";
    n = js8_build_message(&cfg, "OK1BT/QRP SNR -11", fr, 40);
    CHECK(n == 2, "compound to: frames %d", n);
    ngot = 0; feed_frames(fr, n, 1800, -9, &t);
    CHECK_STR(got[0].text, "Q2XYZ: OK1BT/QRP SNR -11");
    CHECK(got[0].cmd == 25, "cmd %d", got[0].cmd);

    /* 10: single-frame directed commands */
    n = js8_build_message(&cfg, "OK1BT SNR?", fr, 40);
    CHECK(n == 1, "frames %d", n);
    ngot = 0; feed_frames(fr, n, 600, 1, &t);
    CHECK_STR(got[0].text, "Q2XYZ: OK1BT SNR?");
    CHECK(got[0].flags == JS8ASM_F_TO_ME, "flags %02x", got[0].flags);
    n = js8_build_message(&cfg, "OK1BT?", fr, 40);
    ngot = 0; feed_frames(fr, n, 600, 1, &t);
    CHECK_STR(got[0].text, "Q2XYZ: OK1BT SNR?");
    n = js8_build_message(&cfg, "OK1BT HEARTBEAT SNR +03", fr, 40);
    ngot = 0; feed_frames(fr, n, 600, 1, &t);
    CHECK_STR(got[0].text, "Q2XYZ: OK1BT HEARTBEAT SNR +03");
    n = js8_build_message(&cfg, "OK1BT GOOD MORNING", fr, 40);
    ngot = 0; feed_frames(fr, n, 600, 1, &t);
    CHECK_STR(got[0].text, "Q2XYZ: OK1BT GOOD MORNING");
    CHECK(got[0].cmd == 31 && (got[0].flags & JS8ASM_F_TO_ME), "free text directed");

    /* 11: more concurrent streams than table entries: the oldest is closed */
    js8asm_init(&a, "OK1BT", 0, emit, 0);
    n = js8_build_message(&cfg, "LONG FREE TEXT WITH MANY WORDS IN IT TO MAKE FRAMES", fr, 40);
    ngot = 0;
    for (i = 0; i < JS8ASM_STREAMS + 1; i++) {
        js8_msg_decode(fr[0], &jsc, &m); js8asm_feed(&a, &m, 300 + 30 * i, 0, t + (uint32_t)i);
    }
    CHECK(ngot == 1 && (got[0].flags & JS8ASM_F_TIMEOUT) && got[0].freq == 300, "eviction n %d", ngot);
    ngot = 0; js8asm_tick(&a, t + 200);
    CHECK(ngot == JS8ASM_STREAMS, "all closed by timeout: %d", ngot);
    /* 12: very long message is truncated, not overflowed */
    ngot = 0;
    js8_msg_decode(fr[0], &jsc, &m);
    js8asm_feed(&a, &m, 500, 0, t + 300);
    js8_msg_decode(fr[1], &jsc, &m);
    for (i = 0; i < 400; i++) js8asm_feed(&a, &m, 500, 0, t + 300 + (uint32_t)i);   /* far more than JS8ASM_TEXT_MAX */
    js8asm_tick(&a, t + 1000);
    CHECK(ngot == 1 && (got[0].flags & JS8ASM_F_TRUNC) && strlen(got[0].text) < JS8ASM_TEXT_MAX, "trunc");
    t_asm_review();
}

/* ---- regression tests for the review of 23 Nov 2026 (shared text pool, long messages, compound + data,
 * printable message text).  Frames are given to js8asm as js8_msg_t directly. */
static void feed_text(js8asm_t *a, int type, int i3, const char *text, int freq, uint32_t t)
{
    js8_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = (uint8_t)type; m.i3 = (uint8_t)i3; m.cmd = -1;
    snprintf(m.text, sizeof m.text, "%s", text);
    js8asm_feed(a, &m, freq, -5, t);
}
static void feed_directed(js8asm_t *a, const char *from, const char *to, int cmd, int i3, int freq, uint32_t t)
{
    js8_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = JS8_FT_DIRECTED; m.i3 = (uint8_t)i3; m.cmd = (int8_t)cmd;
    snprintf(m.from, sizeof m.from, "%s", from); snprintf(m.to, sizeof m.to, "%s", to);
    js8asm_feed(a, &m, freq, -5, t);
}
static void t_asm_review(void)
{
    static js8asm_t a;
    static char want[8][JS8ASM_TEXT_MAX + 64];
    char piece[40], ck[4] = { 0 };
    int i, k, n;
    uint32_t t = 5000;

    /* R1: a 600-character MSG to me with a valid checksum arrives whole, verified, checksum removed
     * (the old per-stream buffer cut every message at 231 characters and then skipped the checksum) */
    js8asm_init(&a, "OK1BT", 0, emit, 0);
    ngot = 0; n = 0;
    feed_directed(&a, "Q1ABC", "OK1BT", 9, JS8_I3_FIRST, 1200, t);
    for (i = 0; i < 40; i++) n += sprintf(want[0] + n, "WORD%02d OF A LONG %s", i, i < 39 ? "" : "END");
    js8_checksum16(want[0], n, ck);
    for (i = 0; i < n; i += 15) {
        snprintf(piece, sizeof piece, "%.15s", want[0] + i);
        feed_text(&a, JS8_FT_DATA, 0, piece, 1200, t += 15);
    }
    snprintf(piece, sizeof piece, " %s", ck);
    feed_text(&a, JS8_FT_DATA, JS8_I3_LAST, piece, 1200, t += 15);
    CHECK(n > 600 && ngot == 1, "R1 long message: %d chars, %d messages", n, ngot);
    CHECK(got[0].flags == (JS8ASM_F_TO_ME | JS8ASM_F_CRC_OK), "R1 flags %02x", got[0].flags);
    CHECK(strlen(got[0].text) == (size_t)n + strlen("Q1ABC: OK1BT MSG ") && !strcmp(got[0].text + 17, want[0]), "R1 text whole (%d chars)", (int)strlen(got[0].text));
    CHECK(a.pool_len == 0, "R1 pool empty after the message: %d", a.pool_len);

    /* R2: eight messages built up interleaved, every one comes out exactly as fed (pieces move in the pool) */
    js8asm_init(&a, "OK1BT", 0, emit, 0);
    ngot = 0;
    for (k = 0; k < 8; k++) want[k][0] = 0;
    for (i = 0; i < 20; i++)
        for (k = 0; k < 8; k++) {
            int j = (k * 5 + i * 3) % 8;                      /* a different order every round */
            if (i >= 6 + 2 * j) continue;                     /* different lengths: 6, 8, ... 20 pieces */
            sprintf(piece, "<%d.%02d%.*s>", j, i, j, "xxxxxxxx");
            strcat(want[j], piece);
            feed_text(&a, JS8_FT_DATA, (i == 0 ? JS8_I3_FIRST : 0) | (i == 5 + 2 * j ? JS8_I3_LAST : 0), piece, 500 + 100 * j, t += 15);
        }
    CHECK(ngot == 8, "R2 messages %d", ngot);
    for (k = 0; k < ngot && k < 8; k++) {
        int j = (got[k].freq - 500) / 100;
        CHECK(j >= 0 && j < 8 && !strcmp(got[k].text, want[j]) && got[k].flags == 0, "R2 stream %d text intact, flags %02x", j, got[k].flags);
    }
    CHECK(a.pool_len == 0, "R2 pool empty: %d", a.pool_len);

    /* R3: the pool is shared: six endless messages cannot take more than JS8ASM_POOL together, each is cut
     * and flagged, none is damaged, and a short message still gets through afterwards */
    js8asm_init(&a, "OK1BT", 0, emit, 0);
    ngot = 0;
    for (i = 0; i < 120; i++)
        for (k = 0; k < 6; k++) {
            sprintf(piece, "%c%03d......", 'A' + k, i);
            feed_text(&a, JS8_FT_DATA, i == 0 ? JS8_I3_FIRST : 0, piece, 400 + 100 * k, t);
        }
    CHECK(a.pool_len <= JS8ASM_POOL, "R3 pool_len %d", a.pool_len);
    for (k = 0; k < 6; k++) feed_text(&a, JS8_FT_DATA, JS8_I3_LAST, "!", 400 + 100 * k, t + 15);
    CHECK(ngot == 6, "R3 messages %d", ngot);
    n = 0;
    for (k = 0; k < ngot && k < 6; k++) {                    /* every character is the one that was fed at that place; */
        size_t l = strlen(got[k].text), j; int ok = 1;       /* the closing "!" fits only where the pool had room again */
        if (l && got[k].text[l - 1] == '!') l--;
        for (j = 0; ok && j < l; j++) {
            sprintf(piece, "%c%03d......", 'A' + (got[k].freq - 400) / 100, (int)(j / 10));
            if (got[k].text[j] != piece[j % 10]) ok = 0;
        }
        CHECK(ok && (got[k].flags & JS8ASM_F_TRUNC) && l > 600 && l < JS8ASM_TEXT_MAX, "R3 message %d: %d chars, flags %02x", k, (int)l, got[k].flags);
        n += (int)l;
    }
    CHECK(n == JS8ASM_POOL && a.pool_len == 0, "R3 total %d characters of %d", n, JS8ASM_POOL);
    ngot = 0; feed_text(&a, JS8_FT_DATA, JS8_I3_FIRST | JS8_I3_LAST, "SHORT ONE", 2000, t + 30);
    CHECK(ngot == 1 && !strcmp(got[0].text, "SHORT ONE") && got[0].flags == 0, "R3 short message after the flood");

    /* R4: compound-call frame followed by data frames (the compound-directed frame was lost) keeps the call */
    js8asm_init(&a, "OK1BT", 0, emit, 0);
    ngot = 0;
    {
        js8_msg_t m;
        memset(&m, 0, sizeof m);
        m.type = JS8_FT_COMPOUND; m.i3 = JS8_I3_FIRST; m.cmd = -1; strcpy(m.from, "Q1ABC/P"); strcpy(m.text, "Q1ABC/P: ");
        js8asm_feed(&a, &m, 1500, -5, t += 15);
    }
    feed_text(&a, JS8_FT_DATA, 0, " HELLO ", 1500, t += 15);
    feed_text(&a, JS8_FT_DATA, JS8_I3_LAST, "OK1BT 73", 1500, t += 15);
    CHECK(ngot == 1, "R4 messages %d", ngot);
    CHECK_STR(got[0].text, "Q1ABC/P: HELLO OK1BT 73");
    CHECK_STR(got[0].from, "Q1ABC/P");
    CHECK(got[0].flags == JS8ASM_F_MENTION && got[0].frames == 3, "R4 flags %02x frames %d", got[0].flags, got[0].frames);

    /* R5: the message text is printable (dictionary word 69 is a newline, 72 is 0x1A); the checksum is still
     * taken over the text as sent */
    js8asm_init(&a, "OK1BT", 0, emit, 0);
    ngot = 0;
    feed_text(&a, JS8_FT_DATA_COMPRESSED, JS8_I3_FIRST | JS8_I3_LAST, "HELLO\nN 120000 FAKE\rNOTE\x1a\x7f\tEND \xe9", 1000, t += 15);
    CHECK(ngot == 1, "R5 messages %d", ngot);
    CHECK_STR(got[0].text, "HELLO N 120000 FAKE NOTE?? END \xe9");
    ngot = 0;
    feed_directed(&a, "Q1ABC", "OK1BT", 9, JS8_I3_FIRST, 1000, t += 15);
    js8_checksum16("LINE1\nLINE2", 11, ck);
    sprintf(piece, "LINE1\nLINE2 %s", ck);
    feed_text(&a, JS8_FT_DATA, JS8_I3_LAST, piece, 1000, t += 15);
    CHECK(ngot == 1 && got[0].flags == (JS8ASM_F_TO_ME | JS8ASM_F_CRC_OK), "R5 checksum over the raw text: flags %02x", got[0].flags);
    CHECK_STR(got[0].text, "Q1ABC: OK1BT MSG LINE1 LINE2");
}

/* ---- pack -> unpack round trips for random messages -------------------------- */
static const char *rand_word(char *buf)
{
    static const char *extra[] = { "73", "5X9", "QTH:", "(TEST)", "RST", "599", "TNX", "FB", "GM", "PWR 5W",
        "ANT: DIPOLE", "WX SUNNY +21C", "JO70", "OK1BT", "DE", "K", "U+00E1", "A,B;C", "100%", "#1", "=", "2+2=4",
        /* "KN4CRD" is a word of the JS8Call dictionary (jsc_map.cpp); kept as a dictionary hit followed by "/P" */
        "KN4CRD/P", "IC-705", "\"QUOTE\"", "'S", "!", "?", "...", "@HOME", "E-MAIL", "[X]", "{Y}", "A_B", "~", "^", "|", "<>", "&", "$5", "*" };
    uint32_t k = rn(10);
    if (k < 6) {
        const tup_t *t = &jlist[rn(NJSC)];
        int i, ok = t->len >= 1;
        for (i = 0; i < t->len; i++) if ((unsigned char)t->s[i] < 33 || (unsigned char)t->s[i] > 126) ok = 0;
        if (ok) { memcpy(buf, t->s, (size_t)t->len); buf[t->len] = 0; return buf; }
    }
    if (k < 8) {                                         /* frequent words: low indices */
        int idx = (int)rn(5740), len = map_size(idx), i, ok = 1;
        const tup_t *t = &jlist[by_index[idx]];
        for (i = 0; i < len; i++) if ((unsigned char)t->s[i] < 33 || (unsigned char)t->s[i] > 126) ok = 0;
        if (ok && len > 0) { memcpy(buf, t->s, (size_t)len); buf[len] = 0; return buf; }
    }
    return extra[rn(sizeof extra / sizeof extra[0])];
}

static int letters_only(const char *w)
{
    int i;
    for (i = 0; w[i]; i++) if (w[i] < 'A' || w[i] > 'Z') return 0;
    return i > 0;
}

static void rand_text(char *out, int maxlen, int first_plain)
{
    int n = 0, words = (int)rn(12) + 1, i;
    char buf[40];
    out[0] = 0;
    for (i = 0; i < words; i++) {
        const char *w = rand_word(buf);
        int wl = (int)strlen(w);
        if (i == 0 && first_plain) {
            /* first word: letters only and not a keyword, so the text cannot be
             * taken for a callsign, a command continuation or CQ/HB */
            while (!letters_only(w) || !strcmp(w, "CQ") || !strcmp(w, "HB") || !strcmp(w, "HEARTBEAT") ||
                   !strcmp(w, "MSGS") || !strcmp(w, "CALL") || !strcmp(w, "TO") || !strcmp(w, "SNR") ||
                   !strcmp(w, "DIT") || !strcmp(w, "CPY") || !strcmp(w, "MSG") || !strcmp(w, "HW")) w = rand_word(buf);
            wl = (int)strlen(w);
        }
        if (n + wl + 2 >= maxlen) break;
        if (n) { out[n++] = ' '; if (rn(12) == 0) out[n++] = ' '; }
        memcpy(out + n, w, (size_t)wl); n += wl;
        out[n] = 0;
    }
}

static void t_roundtrip(void)
{
    static js8asm_t a;
    static const char *tos[] = { "OK1BT", "OK1BT/P", "@ALLCALL", "Q3ABC", "@OK", "9A/OK1BT", "Q1ABC/QRP", "@JS8NET", "3DA0XY" };
    static const char *mys[] = { "Q2XYZ", "Q2XYZ/P", "VE3/Q2XYZ", "Q1AW", "Q2XYZ/MM" };
    static const int cmds[] = { 0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 26, 27, 28, 30, 31, 25, 29 };
    int n, fail = 0, total_frames = 0, kinds[4] = { 0, 0, 0, 0 }, huff = 0, jscf = 0, crcok = 0;
    uint32_t bytes0 = jsc.st_bytes;
    uint32_t t = 5000;

    section("pack -> frames -> unpack -> reassembly, 500 random messages");
    asm_ = &a;
    for (n = 0; n < 500; n++) {
        char text[200], body[160], exp[260], num[8];
        uint8_t fr[48][11];
        js8_build_t cfg;
        int nf, i, kind = (int)rn(10), expflags_crc = 0;
        const char *my = mys[rn(5)];

        cfg.mycall = my; cfg.mygrid = "JN89"; cfg.identify = 0; cfg.jsc = &jsc;
        num[0] = 0;
        if (kind == 0) {                                 /* heartbeat / CQ */
            static const char *hb[] = { "HB", "HEARTBEAT", "CQ CQ CQ", "CQ DX", "CQ QRP", "CQ CONTEST", "CQ FIELD", "CQ FD", "CQ CQ", "CQ" };
            int h = (int)rn(10), g = (int)rn(2);
            snprintf(text, sizeof text, "%s%s", hb[h], g ? " JO70" : "");
            if (h < 2) snprintf(exp, sizeof exp, "%s: @HB HEARTBEAT%s", my, g ? " JO70" : "");
            else snprintf(exp, sizeof exp, "%s: @ALLCALL %s%s", my, hb[h], g ? " JO70" : "");
            kinds[0]++;
        } else if (kind < 5) {                           /* directed command, maybe with text */
            const char *to = tos[rn(9)];
            int cmd = cmds[rn(31)], with_text = (int)rn(3) != 0;
            const char *cs = js8_cmd_str(cmd);
            body[0] = 0;
            if (with_text || cmd == 31) rand_text(body, 120, 1);
            if (cmd == 25 || cmd == 29) snprintf(num, sizeof num, " %+03d", (int)rn(61) - 30);
            if (cmd == 31) {
                snprintf(text, sizeof text, "%s %s", to, body);
                snprintf(exp, sizeof exp, "%s: %s %s", my, to, body);
            } else {
                snprintf(text, sizeof text, "%s%s%s%s%s", to, cs, num, body[0] ? " " : "", body);
                snprintf(exp, sizeof exp, "%s: %s%s%s%s%s", my, to, cs, num, body[0] ? " " : "", body);
            }
            if (body[0] && js8_cmd_checksum(cmd)) expflags_crc = JS8ASM_F_CRC_OK;
            kinds[1]++;
        } else if (kind < 8) {                           /* free text, identified */
            cfg.identify = 1;
            rand_text(body, 150, 1);
            snprintf(text, sizeof text, "%s", body);
            snprintf(exp, sizeof exp, "%s: %s", my, body);
            kinds[2]++;
        } else {                                         /* free text, Huffman alphabet only, no dictionary */
            int len = (int)rn(70) + 3;
            cfg.jsc = 0;
            for (i = 0; i < len; i++) body[i] = js8_huff[1 + rn(43)].c;
            if (len < 6) len = 6;
            body[0] = 'X'; body[1] = 'Q'; body[2] = 'Z'; body[3] = ' ';   /* first word is not a callsign */
            if (body[4] == ' ') body[4] = 'E';
            for (i = 5; i < len; i++) if (rn(6) == 0 && body[i - 1] != ' ' && i + 1 < len) body[i] = ' ';
            if (body[len - 1] == ' ') body[len - 1] = 'K';
            body[len] = 0;
            snprintf(text, sizeof text, "%s", body);
            snprintf(exp, sizeof exp, "%s", body);
            kinds[3]++;
        }
        nf = js8_build_message(&cfg, text, fr, 48);
        if (nf <= 0) { if (fail < 8) printf("  build failed (%d): '%s'\n", nf, text); fail++; continue; }
        total_frames += nf;
        js8asm_init(&a, "OK1BT", "@ALLCALL @OK", emit, 0);
        ngot = 0;
        for (i = 0; i < nf; i++) {
            js8_msg_t m;
            if (!js8_msg_crc_ok(fr[i])) { fail++; break; }
            js8_msg_decode(fr[i], &jsc, &m);
            if (m.type == JS8_FT_DATA) huff++;
            if (m.type == JS8_FT_DATA_COMPRESSED) jscf++;
            if (m.type == JS8_FT_UNKNOWN || (m.flags & (JS8_MF_NODICT | JS8_MF_TRUNC))) { fail++; break; }
            if (((m.i3 & 1) != 0) != (i == 0) || ((m.i3 & 2) != 0) != (i == nf - 1)) { fail++; break; }
            js8asm_feed(&a, &m, 1500, -10, t); t += 15;
        }
        /* expected text: what the builder normalises (trailing blanks) */
        { int l = (int)strlen(exp); while (l > 0 && exp[l - 1] == ' ') exp[--l] = 0; }
        if (ngot != 1 || strcmp(got[0].text, exp) || (got[0].flags & (JS8ASM_F_CRC_BAD | JS8ASM_F_TIMEOUT | JS8ASM_F_PARTIAL | JS8ASM_F_TRUNC)) ||
            (got[0].flags & JS8ASM_F_CRC_OK) != expflags_crc) {
            if (fail < 8) printf("  MISMATCH (%d msgs, flags %02x, %d frames)\n    in : '%s'\n    exp: '%s'\n    got: '%s'\n",
                                 ngot, ngot ? got[0].flags : 0, nf, text, exp, ngot ? got[0].text : "");
            fail++;
        }
        if (ngot == 1 && (got[0].flags & JS8ASM_F_CRC_OK)) crcok++;
    }
    CHECK(fail == 0, "%d of 500 messages failed", fail);
    printf("  500 messages: %d heartbeat/CQ, %d directed, %d free text (JSC files), %d free text (Huffman only)\n",
           kinds[0], kinds[1], kinds[2], kinds[3]);
    printf("  %d frames (%d Huffman data, %d JSC data), %d checksums verified, dictionary bytes read %u\n",
           total_frames, huff, jscf, crcok, jsc.st_bytes - bytes0);
}

/* ---- builder details --------------------------------------------------------- */
static void t_builder(void)
{
    uint8_t fr[40][11];
    js8_build_t cfg = { "OK1BT", "JO70", 0, &jsc };
    js8_msg_t m;
    int n;
    char ck[4] = { 0, 0, 0, 0 };

    section("message builder details");
    /* CRC-16/KERMIT check value: "123456789" -> 0x2189 = 8585 -> base 41: 5,4,16 = "54G" */
    js8_checksum16("123456789", 9, ck);
    CHECK_STR(ck, "54G");
    js8_checksum16("", 0, ck);
    CHECK_STR(ck, "000");
    /* own call at the start of the line is removed */
    n = js8_build_message(&cfg, "OK1BT: Q2XYZ ACK", fr, 40);
    CHECK(n == 1, "frames %d", n);
    js8_msg_decode(fr[0], &jsc, &m);
    CHECK_STR(m.text, "OK1BT: Q2XYZ ACK ");
    CHECK(m.i3 == 3, "i3 %d", m.i3);
    /* lower case input is upper-cased */
    n = js8_build_message(&cfg, "q2xyz snr -05", fr, 40);
    js8_msg_decode(fr[0], &jsc, &m);
    CHECK_STR(m.text, "OK1BT: Q2XYZ SNR -05 ");
    /* number is clamped like packNum */
    n = js8_build_message(&cfg, "Q2XYZ SNR -31", fr, 40);
    js8_msg_decode(fr[0], &jsc, &m);
    CHECK_STR(m.text, "OK1BT: Q2XYZ SNR -30 ");
    /* text that is not a callsign stays free text */
    n = js8_build_message(&cfg, "HELLO WORLD", fr, 40);
    js8_msg_decode(fr[0], &jsc, &m);
    CHECK(n == 1 && (m.type == JS8_FT_DATA || m.type == JS8_FT_DATA_COMPRESSED), "free text type %d", m.type);
    CHECK_STR(m.text, "HELLO WORLD");
    /* MSG TO: and QUERY MSGS are found before their shorter forms */
    n = js8_build_message(&cfg, "Q2XYZ MSG TO:Q3ABC HI", fr, 40);
    js8_msg_decode(fr[0], &jsc, &m);
    CHECK(m.cmd == 10, "MSG TO: cmd %d", m.cmd);
    n = js8_build_message(&cfg, "Q2XYZ QUERY MSGS", fr, 40);
    js8_msg_decode(fr[0], &jsc, &m);
    CHECK(m.cmd == 12 && n == 1, "QUERY MSGS cmd %d", m.cmd);
    n = js8_build_message(&cfg, "Q2XYZ QUERY MSGS?", fr, 40);
    js8_msg_decode(fr[0], &jsc, &m);
    CHECK(m.cmd == 12 && n == 1, "QUERY MSGS? cmd %d n %d", m.cmd, n);
    n = js8_build_message(&cfg, "Q2XYZ>HELLO", fr, 40);
    js8_msg_decode(fr[0], &jsc, &m);
    CHECK(m.cmd == 5 && n >= 2, "relay cmd %d n %d", m.cmd, n);
    /* too many frames for the caller's array */
    CHECK(js8_build_message(&cfg, "Q2XYZ MSG THIS TEXT NEEDS MORE THAN TWO FRAMES FOR SURE", fr, 2) == -1, "max_frames");
    /* message to oneself is not a directed message (JS8Call: to != mycall) */
    { js8_build_t c2 = { "Q2XYZ", "JN89", 0, &jsc };
      n = js8_build_message(&c2, "Q2XYZ ACK", fr, 40);      /* own call stripped, "ACK" is free text */
      js8_msg_decode(fr[0], &jsc, &m);
      CHECK(n == 1 && m.type != JS8_FT_DIRECTED, "self: n %d type %d", n, m.type);
      CHECK_STR(m.text, "ACK"); }
    /* sender chooses Huffman only when it carries more characters */
    {
        uint8_t f[11];
        int k = js8_pack_data("EEEEEEEEEEEEEEEEEEEEEE", 22, &jsc, 0, f);
        js8_msg_decode(f, &jsc, &m);
        printf("  \"EEEE...\": %d chars in one frame, type %d (4 = Huffman)\n", k, m.type);
        CHECK(m.type == JS8_FT_DATA && k == 22, "huffman chosen");
        k = js8_pack_data("THE QUICK BROWN FOX", 19, &jsc, 0, f);
        js8_msg_decode(f, &jsc, &m);
        printf("  \"THE QUICK BROWN FOX\": %d chars in one frame, type %d (6 = JSC): \"%s\"\n", k, m.type, m.text);
        CHECK(m.type == JS8_FT_DATA_COMPRESSED, "jsc chosen");
    }
}

int main(int argc, char **argv)
{
    const char *srcdir = argc > 1 ? argv[1] : "third_party/js8call";
    sd_dir = argc > 2 ? argv[2] : "build/sd";

    printf("sizeof: js8_msg_t %zu, js8jsc_t %zu, js8asm_t %zu (stream %zu), js8asm_msg_t %zu\n",
           sizeof(js8_msg_t), sizeof(js8jsc_t), sizeof(js8asm_t), sizeof(js8asm_stream_t), sizeof(js8asm_msg_t));
    t_tables(srcdir);
    t_crc();
    t_grid();
    t_calls();
    t_frames();
    t_huff();
    section("loading jsc_list.cpp as text");
    load_jsc(srcdir);
    t_jsc();
    t_builder();
    t_asm();
    t_roundtrip();
    printf("\nRESULT: %d checks passed, %d failed\n", n_ok, n_fail);
    return n_fail ? 1 : 0;
}
