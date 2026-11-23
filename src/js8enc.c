/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8enc.c - JS8 frame builder and encoder. Freestanding C99, no libc (js8_port.h only).
 * Re-implemented from the logic of JS8Call lib/js8/genjs8.f90, lib/ft8/encode174.f90 and
 * JS8.cpp:2650-2760 (WSJT-X / JS8Call authors, JS8.cpp (C) 2025 Allan Bazinet W6BAZ; GPLv3 with
 * no "or later" statement upstream); no code copied.
 */
#include "js8enc.h"
#include "js8crc.h"
#include "js8_port.h"

int js8_frame_build(const char txt12[12], int i3, uint8_t out[11])
{
    uint8_t f[JS8_FRAME_BYTES];
    int i;

    if (i3 < 0 || i3 > 7) return -100;
    js8_memset(f, 0, sizeof f);
    for (i = 0; i < JS8_PAYLOAD_CHARS; i++) {
        int w = js8_alphabet_index((unsigned char)txt12[i]);
        int bit = 6 * i, b;
        if (w < 0) return -(1 + i);
        for (b = 0; b < 6; b++, bit++)
            if (w & (0x20 >> b)) f[bit >> 3] |= (uint8_t)(0x80u >> (bit & 7));
    }
    f[9] = (uint8_t)(i3 << 5);
    js8_crc12_store(f, js8_crc12(f));
    js8_memcpy(out, f, sizeof f);
    return 0;
}

void js8_frame_text(const uint8_t frame[11], char txt12[12], int *i3)
{
    int i;
    for (i = 0; i < JS8_PAYLOAD_CHARS; i++) {
        int bit = 6 * i, b, w = 0;
        for (b = 0; b < 6; b++, bit++)
            w = (w << 1) | ((frame[bit >> 3] >> (7 - (bit & 7))) & 1);
        txt12[i] = js8_alphabet[w];
    }
    if (i3) *i3 = frame[9] >> 5;
}

/* parity (0/1) of a byte */
static unsigned parity8(unsigned x)
{
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return x & 1u;
}

void js8_encode174(const uint8_t frame87[11], uint8_t cw[22])
{
    uint8_t m[JS8_FRAME_BYTES];
    int p, i;

    js8_memcpy(m, frame87, sizeof m);
    m[10] &= 0xFEu; /* only 87 bits */
    js8_memset(cw, 0, JS8_LDPC_N_BYTES);

    /* parity bit p = <generator row p, message> over GF(2) -> codeword bit p */
    for (p = 0; p < JS8_LDPC_M; p++) {
        unsigned x = 0;
        for (i = 0; i < JS8_LDPC_K_BYTES; i++) x ^= (unsigned)(kJS8_LDPC_generator[p][i] & m[i]);
        if (parity8(x)) cw[p >> 3] |= (uint8_t)(0x80u >> (p & 7));
    }
    /* message bit k -> codeword bit 87 + k */
    for (i = 0; i < JS8_LDPC_K; i++) {
        if (m[i >> 3] & (0x80u >> (i & 7))) {
            int n = JS8_LDPC_M + i;
            cw[n >> 3] |= (uint8_t)(0x80u >> (n & 7));
        }
    }
}

void js8_encode(const uint8_t frame87[11], const uint8_t costas[3][7], uint8_t tones[79])
{
    uint8_t cw[JS8_LDPC_N_BYTES];
    int j, g;

    js8_encode174(frame87, cw);
    for (g = 0; g < 3; g++)
        for (j = 0; j < 7; j++) tones[36 * g + j] = costas[g][j];
    for (j = 0; j < JS8_ND; j++) {
        int n = 3 * j, b;
        unsigned t = 0;
        for (b = 0; b < 3; b++, n++) t = (t << 1) | ((unsigned)(cw[n >> 3] >> (7 - (n & 7))) & 1u);
        tones[JS8_DATA_SYM_POS(j)] = (uint8_t)t;
    }
}
