/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8_constants.h - JS8 (JS8Call compatible) constants: frame geometry, Costas arrays,
 * frame alphabet, LDPC(174,87) tables. Values derived from the JS8Call sources
 * (js8call/js8call@a7ff1be lib/js8/genjs8.f90, JS8.cpp; JS8Call / WSJT-X authors, JS8.cpp
 * (C) 2025 Allan Bazinet W6BAZ; GPLv3 with no "or later" statement upstream, hence GPL-3.0-only
 * for this file - see NOTICE.md).
 */
#ifndef JS8_CONSTANTS_H
#define JS8_CONSTANTS_H

#include <stdint.h>

/* ---- frame geometry (all submodes) ---- */
#define JS8_NN 79            /* channel symbols: S7 D29 S7 D29 S7 */
#define JS8_ND 58            /* data symbols (29 parity + 29 message) */
#define JS8_NS 21            /* sync symbols */
#define JS8_NTONES 8
#define JS8_FRAME_BITS 87    /* 72 payload + 3 i3bit + 12 CRC */
#define JS8_FRAME_BYTES 11   /* 87 bits MSB first, last bit always 0 */
#define JS8_PAYLOAD_CHARS 12 /* 12 x 6-bit words */

/* ---- submode Normal (JS8A) at 8000 Hz ---- */
#define JS8_FS 8000
#define JS8A_SPS 1280                 /* samples per symbol, 0.160 s */
#define JS8A_PERIOD_SAMPLES 120000    /* 15 s */
#define JS8A_START_SAMPLES 4000       /* 0.5 s start delay inside the period */
#define JS8A_FRAME_SAMPLES (JS8_NN * JS8A_SPS) /* 101120 = 12.64 s */
/* tone spacing = JS8_FS / JS8A_SPS = 6.25 Hz; f0 = frequency of tone 0 */

/* ---- LDPC (174,87); codeword on air = [87 parity | 87 message] ---- */
#define JS8_LDPC_N 174
#define JS8_LDPC_K 87
#define JS8_LDPC_M 87
#define JS8_LDPC_K_BYTES 11
#define JS8_LDPC_N_BYTES 22

/* generator: row p = parity bit p (on-air order), 87 bits MSB first */
extern const uint8_t kJS8_LDPC_generator[JS8_LDPC_M][JS8_LDPC_K_BYTES];
/* check -> bits, 1-origin, 0 = unused */
extern const uint8_t kJS8_LDPC_Nm[JS8_LDPC_M][7];
/* bit -> checks, 1-origin */
extern const uint8_t kJS8_LDPC_Mn[JS8_LDPC_N][3];
extern const uint8_t kJS8_LDPC_Num_rows[JS8_LDPC_M];

/* Costas arrays, 3 x 7 from day one (start, middle, end). */
extern const uint8_t js8_costas_normal[3][7];
#ifdef JS8_WITH_MODIFIED_COSTAS
extern const uint8_t js8_costas_modified[3][7]; /* Fast/Turbo/Slow/Ultra, later */
#endif

/* frame alphabet: 6-bit word -> char, 64 chars + NUL */
extern const char js8_alphabet[65];
/* char -> 6-bit word, -1 if the char is not in the alphabet */
int js8_alphabet_index(int c);

/* data symbol j (0..57, codeword bits 3j..3j+2) -> channel symbol index (0..78) */
#define JS8_DATA_SYM_POS(j) ((j) + ((j) < 29 ? 7 : 14))

#endif /* JS8_CONSTANTS_H */
