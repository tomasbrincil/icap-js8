/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8msg - JS8 message layer in the radio: 11-byte frame (72 payload bits,
 * 3 i3bit flags, CRC-12) -> the text JS8Call shows for that frame.
 * Re-implementation of the logic in JS8Call varicode.cpp / decodedtext.cpp /
 * jsc.cpp ((C) 2018 Jordan Sherer KN4CRD, GPL-3.0-or-later); tables are derived data.
 * Freestanding C99, no libc, no 64-bit division.
 */
#ifndef JS8MSG_H
#define JS8MSG_H
#include <stdint.h>
#include "js8jsc.h"

#define JS8_FRAME_BYTES 11
#define JS8_CALL_MAX    13     /* 11 characters (compound call) + NUL          */
#define JS8_TEXT_MAX    96     /* longest text of one frame is 82 characters   */

/* i3bit = transmission flags (byte 9, top 3 bits) */
#define JS8_I3_FIRST 1
#define JS8_I3_LAST  2
#define JS8_I3_DATA  4

/* frame types (first payload bits), values as in JS8Call Varicode::FrameType */
#define JS8_FT_HEARTBEAT          0   /* heartbeat or CQ (is_cq)               */
#define JS8_FT_COMPOUND           1
#define JS8_FT_COMPOUND_DIRECTED  2
#define JS8_FT_DIRECTED           3
#define JS8_FT_DATA               4   /* Huffman text                          */
#define JS8_FT_DATA_COMPRESSED    6   /* JSC text                              */
#define JS8_FT_DATA_FAST          7   /* i3bit data flag, JSC text (ours)      */
#define JS8_FT_UNKNOWN            255 /* nothing unpacked: text = 12 raw chars */

#define JS8_MF_NODICT  1   /* a JSC word could not be read: shown as "{?}"     */
#define JS8_MF_TRUNC   2   /* text did not fit JS8_TEXT_MAX                    */

#define JS8_NBASECALL  262177560u   /* 37*36*10*27*27*27 */
#define JS8_NBASEGRID  32400u
#define JS8_NUSERGRID  32410u
#define JS8_NMAXGRID   32767u
#define JS8_NGROUPS    54

typedef struct {
    uint8_t type;            /* JS8_FT_*                                        */
    uint8_t i3;              /* JS8_I3_* flags                                  */
    uint8_t is_cq;           /* heartbeat frame: 1 = CQ ("alt"), 0 = heartbeat  */
    uint8_t bits3;           /* heartbeat frame: CQ / HB variant                */
    int8_t  cmd;             /* directed command code 0..127, -1 = none         */
    uint8_t has_num;
    int8_t  num;             /* SNR / number (-31..32)                          */
    uint8_t flags;           /* JS8_MF_*                                        */
    char from[JS8_CALL_MAX];
    char to[JS8_CALL_MAX];
    char grid[5];
    char text[JS8_TEXT_MAX]; /* exactly DecodedText::message(), NUL terminated  */
} js8_msg_t;

/* Decode one frame.  jsc may be NULL (JSC words then show as "{?}").
 * Returns the frame type (also in out->type). */
int js8_msg_decode(const uint8_t frame[JS8_FRAME_BYTES], js8jsc_t *jsc, js8_msg_t *out);

/* CRC-12 of JS8 (poly 0xC06, augmented over the 11 bytes with the CRC field
 * zeroed, xor 42).  js8_msg_crc_ok checks, js8_msg_crc_set writes it. */
unsigned js8_msg_crc12(const uint8_t frame[JS8_FRAME_BYTES]);
int      js8_msg_crc_ok(const uint8_t frame[JS8_FRAME_BYTES]);
void     js8_msg_crc_set(uint8_t frame[JS8_FRAME_BYTES]);

/* the 12 six-bit words as characters (JS8Call's frame string), NUL terminated */
void js8_frame_chars(const uint8_t frame[JS8_FRAME_BYTES], char out[13]);

/* ---- codec pieces shared with js8pack.c / js8asm.c and the tests ---------- */
extern const char js8_alnum[40];                 /* "0-9A-Z /@"                 */
typedef struct { char c; uint8_t len; uint8_t code; } js8_huff_t;
extern const js8_huff_t js8_huff[44];

const char *js8_group_name(int n);               /* n = 1..54 -> "<....>", "@ALLCALL", ... */
int         js8_group_find(const char *name);    /* -> 1..54 or 0                */
const char *js8_cmd_str(int code);               /* 0..31 -> " SNR?", ...; else "" */
const char *js8_cq_str(int bits3);               /* 0..7 -> "CQ CQ CQ", ...      */
int  js8_cmd_is_snr(int code);                   /* 25, 29                       */
int  js8_cmd_checksum(int code);                 /* 16 for 5,9,10,11,12,13,24; else 0 */

int  js8_call28_unpack(uint32_t v, int portable, char out[JS8_CALL_MAX]);  /* 0 = v is not a 28-bit value */
void js8_call50_unpack(const uint16_t limb[4], char out[JS8_CALL_MAX]);    /* limb[0] = low 16 bits */
int  js8_grid_unpack(unsigned v, char out[5]);                              /* 0 = no grid (v > 32400) */
int  js8_snr_format(int snr, char *out);                                    /* "+05", "-12"; returns length */
/* "FROM: TO CMD NUM" without the trailing blank; returns length */
int  js8_format_directed(const char *from, const char *to, int cmd, int has_num, int num,
                         char *out, int max);
/* CRC-16/KERMIT of a text as 3 base-41 characters (Varicode::checksum16) */
void js8_checksum16(const char *s, int len, char out[3]);

#endif
