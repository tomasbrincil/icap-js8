/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8crc.c - JS8 CRC-12. Freestanding C99, no libc.
 *
 * JS8Call (lib/crc12.cpp, JS8.cpp:896-926) computes boost::augmented_crc<12,0xc06> over the
 * 11 frame bytes with the CRC field zeroed, then xors 42. The frame is 75 message bits
 * followed by 13 zero bits, so the augmented form over 88 bits equals a plain (non-augmented)
 * bitwise CRC over the first 76 bits (75 message bits + one zero bit). That is what we do.
 */
#include "js8crc.h"

uint16_t js8_crc12(const uint8_t frame[11])
{
    uint16_t rem = 0;
    int i;
    for (i = 0; i < 76; i++) {
        /* bits 0..74 from the frame, bit 75 is a constant 0 */
        unsigned bit = (i < 75) ? (unsigned)((frame[i >> 3] >> (7 - (i & 7))) & 1) : 0u;
        unsigned top = ((unsigned)(rem >> 11) & 1u) ^ bit;
        rem = (uint16_t)((rem << 1) & 0xFFFu);
        if (top) rem ^= JS8_CRC12_POLY;
    }
    return (uint16_t)(rem ^ JS8_CRC12_XOR);
}

uint16_t js8_crc12_extract(const uint8_t frame[11])
{
    return (uint16_t)((((unsigned)frame[9] & 0x1Fu) << 7) | ((unsigned)frame[10] >> 1));
}

void js8_crc12_store(uint8_t frame[11], uint16_t crc)
{
    frame[9] = (uint8_t)((frame[9] & 0xE0u) | ((crc >> 7) & 0x1Fu));
    frame[10] = (uint8_t)((crc & 0x7Fu) << 1);
}

int js8_crc12_check(const uint8_t frame[11])
{
    if (frame[10] & 1u) return 0;
    return js8_crc12_extract(frame) == js8_crc12(frame);
}
