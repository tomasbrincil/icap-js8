/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8crc.h - JS8 CRC-12 (polynomial 0xC06, init 0, MSB first, no reflection, xor 0x02A).
 */
#ifndef JS8CRC_H
#define JS8CRC_H

#include <stdint.h>

#define JS8_CRC12_POLY 0xC06u
#define JS8_CRC12_XOR  0x02Au

/* CRC of the first 75 bits of an 11-byte frame (payload + i3bit); the CRC field and the
 * spare bit in frame[9..10] are ignored. Returns 0..0xFFF. */
uint16_t js8_crc12(const uint8_t frame[11]);

/* CRC field stored in the frame (bits 75..86). */
uint16_t js8_crc12_extract(const uint8_t frame[11]);

/* Write the CRC field (keeps i3bit, clears the spare last bit). */
void js8_crc12_store(uint8_t frame[11], uint16_t crc);

/* 1 if the stored CRC matches and the spare last bit is 0, else 0. Does not modify frame. */
int js8_crc12_check(const uint8_t frame[11]);

#endif /* JS8CRC_H */
