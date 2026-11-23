/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8enc.h - JS8 frame builder and encoder (87-bit frame -> 79 tones).
 */
#ifndef JS8ENC_H
#define JS8ENC_H

#include <stdint.h>
#include "js8_constants.h"

/* Build the 11-byte / 87-bit frame from 12 alphabet characters and the 3-bit frame type.
 * txt12 need not be NUL terminated; exactly 12 characters are read.
 * Returns 0 on success, -(1+index) for the first character outside the alphabet,
 * -100 if i3 is outside 0..7. out is only written on success. */
int js8_frame_build(const char txt12[12], int i3, uint8_t out[11]);

/* Inverse of the payload part: 12 characters (no NUL) and i3bit. Never fails. */
void js8_frame_text(const uint8_t frame[11], char txt12[12], int *i3);

/* LDPC (174,87) encode: 87 frame bits -> 174 codeword bits packed MSB first in 22 bytes,
 * on-air order: bits 0..86 parity, bits 87..173 the message. The 88th frame bit is ignored;
 * the two spare bits of cw[21] are 0. */
void js8_encode174(const uint8_t frame87[11], uint8_t cw[22]);

/* Frame -> 79 tones (0..7): Costas at 0..6, 36..42, 72..78, parity in 7..35, message in
 * 43..71; tone = 4*b0 + 2*b1 + b2 (natural binary, no Gray map). The frame is encoded as
 * given (the CRC is NOT recomputed). For submode Normal pass js8_costas_normal. */
void js8_encode(const uint8_t frame87[11], const uint8_t costas[3][7], uint8_t tones[79]);

#endif /* JS8ENC_H */
