/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8pack - text -> JS8 frames (the reverse of js8msg).  NOT part of the radio app: it is
 * compiled only into the host test tools (tests/js8mk.c, tests/t6_msg.c) to make test frames.
 * Re-implementation of JS8Call varicode.cpp pack* / buildMessageFrames and
 * jsc.cpp compress ((C) 2018 Jordan Sherer KN4CRD, GPL-3.0-or-later).  Submode Normal
 * only (data frames carry the in-payload header, i3bit data flag clear).
 * Nothing here transmits; it only fills 11-byte frames (CRC-12 included).
 * Freestanding C99, no libc.
 */
#ifndef JS8PACK_H
#define JS8PACK_H
#include <stdint.h>
#include "js8msg.h"

#define JS8_LINE_MAX 240          /* longest message text js8_build_message accepts */

/* 0 = not a callsign, 1 = standard or group call (28-bit), 2 = compound (50-bit) */
int      js8_call_kind(const char *call);
int      js8_call28_pack(const char *call, uint32_t *v, int *portable);   /* 1 = ok */
int      js8_call50_pack(const char *call, uint16_t limb[4]);             /* 1 = ok */
unsigned js8_grid_pack(const char *grid);                                 /* 32767 = none */

/* Single frames.  i3 = JS8_I3_* flags.  Return 1 on success, 0 if not packable. */
int js8_pack_heartbeat(const char *mycall, const char *grid, int cq, int bits3,
                       int i3, uint8_t out[JS8_FRAME_BYTES]);
int js8_pack_compound(const char *call, const char *grid, int i3, uint8_t out[JS8_FRAME_BYTES]);
int js8_pack_compound_directed(const char *call, int cmd, int has_num, int num,
                               int i3, uint8_t out[JS8_FRAME_BYTES]);
int js8_pack_directed(const char *from, const char *to, int cmd, int has_num, int num,
                      int i3, uint8_t out[JS8_FRAME_BYTES]);
/* One data frame from the start of text[0..len); returns the number of
 * characters consumed (0 = nothing fits / not encodable).  Chooses Huffman or
 * JSC like JS8Call (Huffman only if it carries more characters).  jsc may be
 * NULL: Huffman only. */
int js8_pack_data(const char *text, int len, js8jsc_t *jsc, int i3, uint8_t out[JS8_FRAME_BYTES]);

typedef struct {
    const char *mycall;
    const char *mygrid;
    uint8_t identify;             /* prepend "MYCALL: " to plain free text */
    js8jsc_t *jsc;                /* may be NULL */
} js8_build_t;

/* Whole message -> frames, following Varicode::buildMessageFrames:
 *   "CQ CQ CQ JO70", "HB JO70", "@ALLCALL CQ ..."          heartbeat / CQ frame
 *   "Q2XYZ SNR?", "Q2XYZ SNR -12", "Q2XYZ MSG text"         directed (+ data, checksum)
 *   anything else                                           free text data frames
 * Text is upper-cased.  Returns the number of frames (first / last flags set),
 * or -1 if max_frames is too small. */
int js8_build_message(const js8_build_t *cfg, const char *text,
                      uint8_t frames[][JS8_FRAME_BYTES], int max_frames);

#endif
