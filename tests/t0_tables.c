/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * T0: table tests, PC only.
 *  - Nm/Mn/Num_rows mutually consistent, expected weights
 *  - every unit-vector message encodes to a codeword with zero syndrome against Nm
 *  - the generator re-derived from Nm by GF(2) elimination equals the stored one
 *  - CRC-12 of the all-zero frame is 0x02a; plain form == boost-style augmented form
 * (The comparison of the header against second copies parsed from the JS8Call sources is
 *  done by tests/model.py tables.)
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

static int cwbit(const uint8_t *cw, int n) { return (cw[n >> 3] >> (7 - (n & 7))) & 1; }

/* number of unsatisfied checks, straight from Nm */
static int syndrome_weight(const uint8_t cw[22])
{
    int m, k, bad = 0;
    for (m = 0; m < JS8_LDPC_M; m++) {
        int s = 0;
        for (k = 0; k < 7; k++)
            if (kJS8_LDPC_Nm[m][k]) s ^= cwbit(cw, kJS8_LDPC_Nm[m][k] - 1);
        bad += s;
    }
    return bad;
}

/* second CRC implementation: boost::augmented_crc<12,0xc06> over 11 bytes, CRC field zeroed */
static unsigned crc_augmented(const uint8_t frame[11])
{
    uint8_t b[11];
    unsigned rem = 0;
    int i, k;
    memcpy(b, frame, 11);
    b[9] &= 0xE0; b[10] = 0;
    for (i = 0; i < 11; i++)
        for (k = 7; k >= 0; k--) {
            rem = (rem << 1) | ((b[i] >> k) & 1u);
            if (rem & 0x1000u) rem ^= 0x1C06u;
        }
    return rem ^ 42u;
}

static uint64_t rng_s = 0x243F6A8885A308D3ull;
static uint64_t rng(void)
{
    uint64_t z = (rng_s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int main(void)
{
    int i, j, k, n, bad;

    /* --- structure of Nm / Mn / Num_rows --- */
    {
        int hist[8] = { 0 }, edges = 0, ok_nr = 1, ok_range = 1, ok_fwd = 1, ok_back = 1, ok_w3 = 1;
        int colw[JS8_LDPC_N] = { 0 };
        for (i = 0; i < JS8_LDPC_M; i++) {
            int w = 0;
            for (k = 0; k < 7; k++) {
                int v = kJS8_LDPC_Nm[i][k];
                if (v > JS8_LDPC_N) ok_range = 0;
                if (v) {
                    int found = 0;
                    w++;
                    if (k > 0 && !kJS8_LDPC_Nm[i][k - 1]) ok_range = 0; /* padding only at the end */
                    if (v <= JS8_LDPC_N) {
                        colw[v - 1]++;
                        for (j = 0; j < 3; j++) if (kJS8_LDPC_Mn[v - 1][j] == i + 1) found = 1;
                    }
                    if (!found) ok_fwd = 0;
                }
            }
            if (w != kJS8_LDPC_Num_rows[i]) ok_nr = 0;
            hist[w]++;
            edges += w;
        }
        for (n = 0; n < JS8_LDPC_N; n++) {
            if (colw[n] != 3) ok_w3 = 0;
            for (j = 0; j < 3; j++) {
                int c = kJS8_LDPC_Mn[n][j], found = 0;
                if (c < 1 || c > JS8_LDPC_M) { ok_back = 0; continue; }
                for (k = 0; k < 7; k++) if (kJS8_LDPC_Nm[c - 1][k] == n + 1) found = 1;
                if (!found) ok_back = 0;
            }
        }
        CHECK(ok_range, "Nm entries in 0..174, zero padding only at row end");
        CHECK(ok_nr, "Num_rows equals the number of non-zero Nm entries in each row");
        CHECK(hist[6] == 79 && hist[7] == 4 && hist[5] == 4, "row weights 79x6, 4x7, 4x5 (got %dx6 %dx7 %dx5)", hist[6], hist[7], hist[5]);
        CHECK(edges == 522 && ok_w3, "522 edges, every column weight 3 (edges %d)", edges);
        CHECK(ok_fwd && ok_back, "Nm and Mn describe the same graph");
    }

    /* --- unit vectors -> zero syndrome --- */
    {
        uint8_t f[11], cw[22];
        int minw = 999, maxw = 0, sys_ok = 1;
        bad = 0;
        for (i = 0; i < JS8_LDPC_K; i++) {
            int w = 0;
            memset(f, 0, sizeof f);
            f[i >> 3] = (uint8_t)(0x80 >> (i & 7));
            js8_encode174(f, cw);
            if (syndrome_weight(cw)) bad++;
            for (n = 0; n < JS8_LDPC_N; n++) w += cwbit(cw, n);
            if (w < minw) minw = w;
            if (w > maxw) maxw = w;
            for (n = 0; n < JS8_LDPC_K; n++) if (cwbit(cw, 87 + n) != (n == i)) sys_ok = 0;
            if (cw[21] & 3) sys_ok = 0;
        }
        CHECK(bad == 0, "87 unit-vector messages: %d codewords with non-zero syndrome", bad);
        CHECK(sys_ok, "message bits appear unpermuted at codeword bits 87..173, spare bits 0");
        printf("      (unit-vector codeword weights %d..%d)\n", minw, maxw);
        memset(f, 0, sizeof f);
        js8_encode174(f, cw);
        for (n = 0, k = 0; n < 22; n++) k |= cw[n];
        CHECK(k == 0, "all-zero message -> all-zero codeword");
        /* negative control: the syndrome test must be able to fail */
        memset(f, 0, sizeof f); f[0] = 0x80;
        js8_encode174(f, cw); cw[0] ^= 0x80;
        CHECK(syndrome_weight(cw) == 3, "negative control: one flipped bit gives 3 failed checks (got %d)", syndrome_weight(cw));
        bad = 0;
        for (i = 0; i < 2000; i++) {
            for (k = 0; k < 11; k++) f[k] = (uint8_t)rng();
            js8_encode174(f, cw);
            if (syndrome_weight(cw)) bad++;
        }
        CHECK(bad == 0, "2000 random messages: %d codewords with non-zero syndrome", bad);
    }

    /* --- generator re-derived from Nm by GF(2) elimination ---
     * H = [A | B] (87 x 174, columns 0..86 parity, 87..173 message). Row-reduce so that
     * A becomes I; then the right half is P with parity = P * message, row p = parity bit p. */
    {
        static uint8_t H[JS8_LDPC_M][JS8_LDPC_N];
        int rank = 0, diff = 0, pad = 0;
        memset(H, 0, sizeof H);
        for (i = 0; i < JS8_LDPC_M; i++)
            for (k = 0; k < 7; k++)
                if (kJS8_LDPC_Nm[i][k]) H[i][kJS8_LDPC_Nm[i][k] - 1] ^= 1;
        for (j = 0; j < JS8_LDPC_M; j++) { /* pivot on parity column j, put it in row j */
            int piv = -1;
            for (i = j; i < JS8_LDPC_M; i++) if (H[i][j]) { piv = i; break; }
            if (piv < 0) break;
            if (piv != j) {
                uint8_t t[JS8_LDPC_N];
                memcpy(t, H[piv], sizeof t); memcpy(H[piv], H[j], sizeof t); memcpy(H[j], t, sizeof t);
            }
            for (i = 0; i < JS8_LDPC_M; i++)
                if (i != j && H[i][j])
                    for (n = 0; n < JS8_LDPC_N; n++) H[i][n] ^= H[j][n];
            rank++;
        }
        CHECK(rank == 87, "parity part of H (columns 0..86) is invertible, rank %d", rank);
        if (rank == 87) {
            for (i = 0; i < JS8_LDPC_M; i++) {
                for (n = 0; n < JS8_LDPC_K; n++) {
                    int gbit = (kJS8_LDPC_generator[i][n >> 3] >> (7 - (n & 7))) & 1;
                    if (gbit != H[i][87 + n]) diff++;
                }
                if (kJS8_LDPC_generator[i][10] & 1) pad++;
            }
            CHECK(diff == 0, "generator derived from Nm == stored kJS8_LDPC_generator (%d of 7569 bits differ)", diff);
            CHECK(pad == 0, "88th (padding) bit of every generator row is 0");
        } else {
            CHECK(0, "generator comparison skipped (H not reducible)");
        }
    }

    /* --- CRC-12 --- */
    {
        uint8_t f[11];
        unsigned c;
        memset(f, 0, sizeof f);
        c = js8_crc12(f);
        CHECK(c == 0x02a, "CRC-12 of the all-zero frame = 0x%03x (expect 0x02a)", c);
        bad = 0;
        for (i = 0; i < 100000; i++) {
            for (k = 0; k < 11; k++) f[k] = (uint8_t)rng();
            if (js8_crc12(f) != crc_augmented(f)) bad++;
        }
        CHECK(bad == 0, "plain 76-bit CRC == augmented CRC over 11 bytes on 100000 random frames (%d differ)", bad);
        /* the CRC must ignore the CRC field and the spare bit */
        memset(f, 0, sizeof f); f[9] = 0x1F; f[10] = 0xFF;
        CHECK(js8_crc12(f) == 0x02a, "CRC ignores frame bits 75..87");
    }

    /* --- Costas / alphabet --- */
    {
        static const uint8_t c[7] = { 4, 2, 5, 6, 1, 3, 0 };
        int ok = 1;
        for (i = 0; i < 3; i++) for (j = 0; j < 7; j++) if (js8_costas_normal[i][j] != c[j]) ok = 0;
        CHECK(ok, "Costas Normal = {4,2,5,6,1,3,0} x3");
        ok = (strlen(js8_alphabet) == 64);
        for (i = 0; i < 256; i++) {
            const char *p = i ? strchr(js8_alphabet, i) : NULL;
            if (js8_alphabet_index(i) != (p ? (int)(p - js8_alphabet) : -1)) ok = 0;
        }
        CHECK(ok, "alphabet has 64 chars and js8_alphabet_index() inverts it for all 256 byte values");
        CHECK(JS8_DATA_SYM_POS(0) == 7 && JS8_DATA_SYM_POS(28) == 35 && JS8_DATA_SYM_POS(29) == 43 && JS8_DATA_SYM_POS(57) == 71,
              "data symbol positions 7..35 and 43..71");
    }

    printf("T0 tables: %s\n", fails ? "FAILED" : "ALL PASS");
    return fails ? 1 : 0;
}
