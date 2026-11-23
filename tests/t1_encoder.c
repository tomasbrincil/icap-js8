/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * T1: encoder tests, PC only.
 *  - the three design vectors (CRC and 58 data tones, parity block then message block)
 *  - frame builder input validation, frame text round trip, CRC check behaviour
 *  - with a file argument: vectors "text i3 crc hex22 tones79" written by tests/model.py
 *    (independent Python model following the Fortran sources) compared bit-exact
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../src/js8_constants.h"
#include "../src/js8crc.h"
#include "../src/js8enc.h"

static int fails;
#define CHECK(cond, ...) do { int ok_ = (cond); printf("%s  ", ok_ ? "PASS" : "FAIL"); \
    printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

static void hex22(const uint8_t f[11], char *out)
{
    int i;
    for (i = 0; i < 11; i++) sprintf(out + 2 * i, "%02x", f[i]);
}

static void tonestr(const uint8_t t[79], char *out)
{
    int i;
    for (i = 0; i < 79; i++) out[i] = (char)('0' + t[i]);
    out[79] = 0;
}

int main(int argc, char **argv)
{
    static const struct { const char *txt; int i3; unsigned crc; const char *data58; } vec[3] = {
        { "000000000000", 0, 0x02a, "4353055005465072162246003666200000000000000000000000000052" },
        { "0123456789AB", 0, 0x00c, "7035363413022156013132203260700010203040506071011121300014" },
        { "HELLOWORLD++", 5, 0x3fc, "2301624051552044001263101766121162525304030332515777751774" },
    };
    uint8_t f[11], tones[79];
    char ts[80], hx[23];
    int i, k, r;

    for (i = 0; i < 3; i++) {
        char d58[59];
        int costas_ok = 1;
        r = js8_frame_build(vec[i].txt, vec[i].i3, f);
        js8_encode(f, js8_costas_normal, tones);
        tonestr(tones, ts);
        memcpy(d58, ts + 7, 29); memcpy(d58 + 29, ts + 43, 29); d58[58] = 0;
        for (k = 0; k < 7; k++)
            if (tones[k] != js8_costas_normal[0][k] || tones[36 + k] != js8_costas_normal[1][k] || tones[72 + k] != js8_costas_normal[2][k]) costas_ok = 0;
        hex22(f, hx);
        printf("      %s i3=%d frame %s tones %s\n", vec[i].txt, vec[i].i3, hx, ts);
        CHECK(r == 0 && js8_crc12_extract(f) == vec[i].crc, "vector %d CRC 0x%03x (expect 0x%03x)", i + 1, js8_crc12_extract(f), vec[i].crc);
        CHECK(strcmp(d58, vec[i].data58) == 0, "vector %d 58 data tones match the design", i + 1);
        CHECK(costas_ok, "vector %d Costas at symbols 0..6, 36..42, 72..78", i + 1);
        CHECK(js8_crc12_check(f) && (f[10] & 1) == 0 && (f[9] >> 5) == vec[i].i3, "vector %d frame self-check (CRC ok, spare bit 0, i3)", i + 1);
    }

    /* validation */
    {
        uint8_t g[11];
        int bad_acc = 0, c;
        memset(g, 0xAA, sizeof g);
        CHECK(js8_frame_build("HELLO WORLD+", 0, g) == -6 && g[0] == 0xAA, "space rejected at index 5, output untouched");
        CHECK(js8_frame_build("/23456789012", 0, g) == -1, "'/' rejected at index 0");
        CHECK(js8_frame_build("00000000000?", 0, g) == -12, "'?' rejected at index 11");
        CHECK(js8_frame_build("000000000000", 8, g) == -100 && js8_frame_build("000000000000", -1, g) == -100, "i3 outside 0..7 rejected");
        for (c = 1; c < 256; c++) {
            char t[12];
            memset(t, 'A', 12); t[7] = (char)c;
            r = js8_frame_build(t, 3, g);
            if ((r == 0) != (strchr(js8_alphabet, c) != NULL)) bad_acc++;
        }
        CHECK(bad_acc == 0 && js8_frame_build("AAAAAAA\0AAAA", 3, g) == -8, "exactly the 64 alphabet characters are accepted (all 256 byte values tried)");
    }

    /* round trip + CRC sensitivity */
    {
        char t[13];
        int i3, miss = 0;
        js8_frame_build("HELLOWORLD++", 5, f);
        js8_frame_text(f, t, &i3); t[12] = 0;
        CHECK(strcmp(t, "HELLOWORLD++") == 0 && i3 == 5, "js8_frame_text inverts js8_frame_build");
        for (k = 0; k < 87; k++) {
            f[k >> 3] ^= (uint8_t)(0x80 >> (k & 7));
            if (js8_crc12_check(f)) miss++;
            f[k >> 3] ^= (uint8_t)(0x80 >> (k & 7));
        }
        CHECK(miss == 0 && js8_crc12_check(f), "each of the 87 single-bit errors fails the CRC check (%d missed)", miss);
        f[10] |= 1;
        CHECK(!js8_crc12_check(f), "spare 88th bit set -> check fails");
        f[10] &= 0xFE;
        /* js8_encode must ignore the 88th bit */
        {
            uint8_t t1[79], t2[79];
            js8_encode(f, js8_costas_normal, t1);
            f[10] |= 1; js8_encode(f, js8_costas_normal, t2); f[10] &= 0xFE;
            CHECK(memcmp(t1, t2, 79) == 0, "js8_encode ignores the 88th frame bit");
        }
    }

    /* vectors from the Python model */
    if (argc > 1) {
        FILE *fp = fopen(argv[1], "r");
        char txt[64], crcs[16], hexs[64], tns[128];
        int i3, nvec = 0, bad_frame = 0, bad_crc = 0, bad_tones = 0, bad_build = 0, hist[8] = { 0 };
        if (!fp) { perror(argv[1]); return 2; }
        while (fscanf(fp, "%63s %d %15s %63s %127s", txt, &i3, crcs, hexs, tns) == 5) {
            unsigned crc_m = 0;
            nvec++;
            if (strlen(txt) != 12 || strlen(hexs) != 22 || strlen(tns) != 79) { bad_build++; continue; }
            sscanf(crcs, "%x", &crc_m);
            if (js8_frame_build(txt, i3, f) != 0) { bad_build++; continue; }
            hex22(f, hx);
            if (strcmp(hx, hexs) != 0) bad_frame++;
            if (js8_crc12_extract(f) != crc_m) bad_crc++;
            js8_encode(f, js8_costas_normal, tones);
            tonestr(tones, ts);
            if (strcmp(ts, tns) != 0) bad_tones++;
            for (k = 0; k < 79; k++) hist[tones[k]]++;
        }
        fclose(fp);
        CHECK(nvec >= 1003, "model vectors read: %d (3 fixed + random)", nvec);
        CHECK(bad_build == 0, "model vectors accepted by js8_frame_build (%d rejected/malformed)", bad_build);
        CHECK(bad_frame == 0, "11-byte frames bit-exact vs Python model (%d differ)", bad_frame);
        CHECK(bad_crc == 0, "CRC-12 equal to Python model (%d differ)", bad_crc);
        CHECK(bad_tones == 0, "79 tones bit-exact vs Python model (%d differ)", bad_tones);
        printf("      tone histogram over all vectors:");
        for (k = 0; k < 8; k++) printf(" %d", hist[k]);
        printf("\n");
    } else {
        printf("NOTE  no model vector file given, cross-check against tests/model.py NOT run\n");
        fails++;
    }

    printf("T1 encoder: %s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
