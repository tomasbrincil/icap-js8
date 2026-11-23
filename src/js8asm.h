/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8asm - per-frequency reassembly of JS8 frames into complete messages,
 * in the radio.  Follows the receive logic of JS8Call(-improved)
 * processDecodeEvent / processBufferedActivity (JS8Call / JS8Call-improved authors, GPLv3):
 * streams are keyed by audio frequency (+-10 Hz for submode Normal, the key
 * follows drift), a frame with the First flag starts a new message, the Last
 * flag ends it, compound-call frames are joined with the following frames,
 * checksummed commands (">", MSG, MSG TO:, QUERY*, CMD) are verified with
 * CRC-16/KERMIT, 60 s without a frame closes a message.
 * Fixed tables, no allocation, freestanding C99.
 *
 * Memory: the text of all messages under construction shares one pool of
 * JS8ASM_POOL bytes (each message a contiguous piece, moved when a neighbour
 * grows), so one long message can use up to JS8ASM_TEXT_MAX - 1 characters
 * while the usual short ones cost only their length.  JS8Call has no length
 * limit; here a message longer than that, or one that finds the pool full, is
 * cut and flagged JS8ASM_F_TRUNC (a cut message is not checksum-verified).
 * The emitted text is printable: after the checksum test newline / CR / tab
 * (dictionary word 69 is a newline) become a blank, other control characters
 * and DEL become '?'.  Bytes >= 0x80 (Latin-1 words of the dictionary) are
 * passed on.
 */
#ifndef JS8ASM_H
#define JS8ASM_H
#include <stdint.h>
#include "js8msg.h"

#define JS8ASM_STREAMS    16
#define JS8ASM_TEXT_MAX   1024    /* longest message: header + body + NUL */
#define JS8ASM_POOL       4096    /* text of all messages under construction */
#define JS8ASM_RANGE_HZ   10      /* JS8::Submode::rxThreshold(Normal) */
#define JS8ASM_TIMEOUT_S  60      /* no frame for this long: close the message */
#define JS8ASM_DROP_S     90      /* idle compound-call-only stream is dropped */
#define JS8ASM_GROUPS_MAX 48      /* "@ALLCALL @OK ..." */

#define JS8ASM_F_TO_ME     0x01   /* directed to MYCALL (or its base call)       */
#define JS8ASM_F_TO_GROUP  0x02   /* directed to one of my groups                */
#define JS8ASM_F_MENTION   0x04   /* MYCALL appears in the text                  */
#define JS8ASM_F_TIMEOUT   0x08   /* closed by timeout / eviction, Last not seen */
#define JS8ASM_F_PARTIAL   0x10   /* beginning missing (no First frame seen)     */
#define JS8ASM_F_TRUNC     0x20   /* text did not fit (longer than 1023 characters, or pool full) */
#define JS8ASM_F_CRC_OK    0x40   /* checksum present and valid (removed from text) */
#define JS8ASM_F_CRC_BAD   0x80   /* checksum expected but wrong (text left as is) */

typedef struct {
    int16_t  freq;                /* Hz, audio offset of the last frame          */
    int8_t   snr;                 /* dB, lowest of the frames                    */
    int8_t   cmd;                 /* directed command 0..31 or -1                */
    uint8_t  flags;               /* JS8ASM_F_*                                  */
    uint8_t  frames;              /* number of frames                            */
    uint32_t t_first, t_last;     /* seconds, as given to js8asm_feed            */
    char     from[JS8_CALL_MAX];
    char     to[JS8_CALL_MAX];
    char     text[JS8ASM_TEXT_MAX];  /* whole line, e.g. "OK1BT: OK2XYZ MSG HELLO" */
} js8asm_msg_t;

typedef void (*js8asm_emit_t)(void *user, const js8asm_msg_t *msg);

typedef struct {                  /* js8asm_msg_t without the text                */
    int16_t  freq;
    int8_t   snr, cmd;
    uint8_t  flags, frames;
    uint32_t t_first, t_last;
    char     from[JS8_CALL_MAX];
    char     to[JS8_CALL_MAX];
} js8asm_meta_t;

typedef struct {
    uint8_t  used, has_hdr, got_first, has_cmpd;
    uint8_t  hdr_len;             /* text[0..hdr_len) = header, then body        */
    uint8_t  trunc;
    uint16_t off, len;            /* text = pool[off .. off+len); off is valid only while len > 0 */
    js8asm_meta_t m;              /* message under construction                  */
    char     cmpd[JS8_CALL_MAX];  /* compound call announced on this frequency   */
} js8asm_stream_t;

typedef struct {
    js8asm_emit_t emit;
    void *user;
    char mycall[JS8_CALL_MAX];
    char groups[JS8ASM_GROUPS_MAX];
    js8asm_stream_t s[JS8ASM_STREAMS];
    uint16_t pool_len;
    char pool[JS8ASM_POOL];
    js8asm_msg_t out;             /* the message handed to emit()                */
} js8asm_t;

/* groups: blank separated list of group calls I belong to, e.g. "@ALLCALL @OK" */
void js8asm_init(js8asm_t *a, const char *mycall, const char *groups,
                 js8asm_emit_t emit, void *user);
/* one decoded frame; now_s = any monotonic seconds counter (period start) */
void js8asm_feed(js8asm_t *a, const js8_msg_t *m, int freq_hz, int snr_db, uint32_t now_s);
/* call once per period: closes messages that timed out */
void js8asm_tick(js8asm_t *a, uint32_t now_s);
/* is `to` me or one of my groups?  returns JS8ASM_F_TO_ME / _TO_GROUP / 0 */
int  js8asm_to_me(const js8asm_t *a, const char *to);

#endif
