/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8feat - the station features of the JS8 radio app that need no loader service:
 *   heard-stations table   call, grid, last SNR, last heard, audio offset, the report the station
 *                          sent us / we sent it; JS8F_HEARD entries, the one heard longest ago is
 *                          replaced (LRU)
 *   inbox                  the newest JS8F_INBOX complete messages addressed to MYCALL or to one
 *                          of my groups (not the CQ / heartbeat calls), with an unread count
 *   band activity          the last JS8F_BAND decoded lines, for the app window
 *   views                  the text of the 8 x 40 app window for the views BAND / INBOX / HEARD
 *                          (the STATUS view is the shell's own)
 *   prepared replies       the answer to an incoming query to MYCALL (SNR?, GRID?, INFO?, STATUS?,
 *                          HEARING?) as TEXT.  Nothing here transmits and nothing here asks the
 *                          transmit path for anything: the text is only offered to the operator,
 *                          who must confirm it through the TX menu (js8tx.c: "TX? ..." row, then
 *                          the "SEND?" row).  A prepared reply expires after JS8F_PREP_TTL seconds.
 *   ADIF                   one record for an exchange the operator marked as complete
 * Fixed tables, no allocation, freestanding C99, no libc.
 *
 * Times are "absolute seconds" (js8f_time: days since 2000-03-01 * 86400 + second of the day of
 * the radio clock), so that ages survive a restart and midnight.
 */
#ifndef JS8FEAT_H
#define JS8FEAT_H
#include <stdint.h>
#include "js8asm.h"

#define JS8F_HEARD     24
#define JS8F_INBOX     6
#define JS8F_ITEXT     72      /* characters of a message kept in the inbox (71 + NUL): two window rows */
#define JS8F_BAND      7
#define JS8F_W         40      /* app window: 40 bytes per line */
#define JS8F_PREP      3       /* prepared replies waiting for the operator */
#define JS8F_REPLY     56      /* size of a prepared-reply text (= JS8TX_TEXT) */
#define JS8F_REPLY_ROW 34      /* longest reply text really prepared: "SEND? " + it is one 40-character DECODE row */
#define JS8F_INFO      40      /* info= / status= of JS8.CFG */
#define JS8F_PREP_TTL  600u    /* s: an unanswered query is forgotten */
#define JS8F_ADIF_MAX  240     /* longest ADIF record (the loader's @QLOG takes 240) */

enum { JS8F_V_STATUS, JS8F_V_BAND, JS8F_V_INBOX, JS8F_V_HEARD, JS8F_NVIEWS };

#define JS8F_I_NEW    1        /* not seen yet (the INBOX view was not shown since it came) */
#define JS8F_I_GROUP  2        /* addressed to one of my groups, not to MYCALL */
#define JS8F_I_BAD    4        /* wrong checksum / incomplete (closed by timeout, beginning missing, cut) */

typedef struct {
    char     call[JS8_CALL_MAX];   /* "" = free entry */
    char     grid[5];
    int8_t   snr;                  /* of the last frame heard */
    int8_t   rcvd, sent;           /* the SNR report the station sent to MYCALL / we sent it; 127 = none */
    uint8_t  pad;
    uint16_t freq;                 /* audio offset of the last frame, Hz */
    uint32_t t;                    /* last heard, absolute seconds */
} js8f_heard_t;

typedef struct {
    uint32_t t;
    int16_t  freq;
    int8_t   snr;
    uint8_t  fl;                   /* JS8F_I_* */
    char     text[JS8F_ITEXT];     /* "FROM: MYCALL ..." cut to 71 characters */
} js8f_in_t;

/* the part that is kept over a restart (written to the card as it is, see shell/js8app_store.h) */
typedef struct {
    js8f_heard_t heard[JS8F_HEARD];
    js8f_in_t    in[JS8F_INBOX];   /* newest first */
    uint8_t      in_n, unread, view, pad;
} js8f_keep_t;

typedef struct {
    js8f_keep_t k;
    const char *mycall;
    char     band[JS8F_BAND][JS8F_W + 1];
    uint8_t  band_n, band_pos;
    char     prep[JS8F_PREP][JS8F_REPLY + 1];   /* "" = free; the transmit menu offers these as "TX? <text>" */
    uint32_t prep_t[JS8F_PREP];
    char     info[JS8F_INFO + 1], status[JS8F_INFO + 1];
} js8feat_t;

/* f must be zeroed by the caller (the app's data region is); mycall must stay valid. */
void js8f_init(js8feat_t *f, const char *mycall);
uint32_t js8f_time(uint32_t yyyymmdd, int second_of_day);
/* One decoded frame: updates the heard table when the frame names its sender. */
void js8f_frame(js8feat_t *f, const js8_msg_t *m, int snr, int freq, uint32_t now);
js8f_heard_t *js8f_find(js8feat_t *f, const char *call);
/* A text that WAS transmitted (after a human confirmed it): "CALL ... SNR -12" -> the report we sent. */
void js8f_sent(js8feat_t *f, const char *text);
/* One complete message.  To MYCALL: into the inbox (returns 1); a complete query to MYCALL also prepares
 * the reply text (returns 2; mygrid = my locator or "").  To one of my groups: into the inbox (returns 3),
 * except CQ / heartbeat calls.  0 = not for me. */
int  js8f_message(js8feat_t *f, const js8asm_msg_t *m, const char *mygrid, uint32_t now);
/* The newest prepared reply, or 0. */
const char *js8f_last_reply(const js8feat_t *f);
/* Once per period: forget prepared replies older than JS8F_PREP_TTL. */
void js8f_tick(js8feat_t *f, uint32_t now);
/* The first line of a decoded text on the DECODE screen ("HHMM TEXT") and its audio offset -> the band activity list
 * ("HHMM FFFF TEXT"). */
void js8f_band(js8feat_t *f, const char *decode_line, int freq);
/* Row `row` of a view (0 = its title) into out (<= 40 characters + NUL).  Returns 1, or 0 when the view
 * has no such row (out = "").  The app window shows rows 0..7; INBOX and HEARD have more. */
int  js8f_row(const js8feat_t *f, int view, int row, uint32_t now, char *out);
/* The INBOX view was shown: nothing is new any more. */
void js8f_read(js8feat_t *f);
const char *js8f_view_name(int view);
/* ADIF record for an exchange with `call` (the heard table supplies grid and reports when it knows the
 * station).  out holds JS8F_ADIF_MAX + 1 bytes.  Returns the length, 0 if call is not a plausible call. */
int  js8f_adif(js8feat_t *f, const char *call, uint32_t yyyymmdd, int second_of_day, uint32_t dial_hz,
               const char *mygrid, char *out);

#endif
