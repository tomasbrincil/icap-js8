/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8tx - the transmit path of the JS8 radio app: outbox, safety rules, the grants of the loader.
 *
 *   the owner taps a row of the DECODE screen -> the radio's own keyboard opens with a text (shell/js8app_kbd.h) -> ENT
 *   -> js8tx_send(text) -> frames (js8pack) -> queue -> one frame per 15 s slot: frame -> 79 tones (js8enc)
 *   -> audio (SDK txsynth, gfsk = 0: plain continuous-phase FSK as JS8Call) -> io->tx_write.
 *
 * SAFETY RULES (all enforced here, none left to the caller):
 *   1. A frame is keyed only for a text the owner saw WHOLE and confirmed, and only on the radio's own permission:
 *      - Confirmed means: ENT on the radio's keyboard, which showed exactly that text (the keyboard holds 43 characters;
 *        it opens only on the owner's tap on a row).  A text from the terminal is only handed to the radio (js8tx_confirm):
 *        the owner's next tap opens the keyboard WITH it, and only his ENT sends it.  The CQ of the green frame and the HB
 *        of the beacon (txhb=) are the owner's arming itself (rules 8, 9).
 *      - Permission means either (a) the loader's TAP CREDIT ("@QTX0" / "@QTX1", SDK QSO mode: one tap on a text row = at
 *        most one granted transmission, only if nothing was armed or on the air when the tap was made; the loader decides):
 *        used only for a message of ONE frame with nothing else queued, the beacon not armed and txtap=1 (default); or
 *        (b) the beacon armed by the owner (red frame: "@TX" beacon=1 owner=0) - every other message waits for it
 *        ("3 parts: MODE > hold FT8 (red), 2 min").  The owner sets the frame in the radio's MODE menu, not on the main
 *        screen: tap the mode tile -> MODE menu -> hold FT8 = red frame (hold longer = green frame, CQ; rule 8); the same
 *        again = off.  On the DECODE screen a JS8 frame is called a "part" (the word "frame" there is the red / green
 *        frame); notes, logs and this code keep "frame".  The arming must have been SEEN to happen while this app runs: a
 *        beacon=1 (or cq=1) that is already there at the first read does not count until beacon=0 (cq=0) was read once,
 *        and neither does one that "comes back" when an owner abort clears with the beacon still armed (O4, 24 Nov 2026).
 *        A grant (r >= 0) the app does not use - a later period, a refusal - is handed back at once with tx_end (S4).
 *        Taps are not delivered to the app while the beacon is armed (SDK): the keyboard cannot be opened then.
 *   2. An empty queue never transmits: io->tx_begin is not even called.  No automatic replies, no heartbeats, no
 *      retransmissions - a frame that went out (or failed) is never sent again.
 *   3. Caps: frames per human action (txmaxf, default 6, at most JS8TX_FMAX), frames per app run (txsess, default
 *      20), messages waiting (JS8TX_QMAX), life of a request (txttl, default 120 s from the request / the last frame
 *      sent - an old request is dropped, not sent hours later).
 *   3a. ONE waiting message (review 24 Nov 2026): an ENT replaces every message of the owner that waits, also one that has
 *      sent a frame already (ONE row: "replaced: MODE > hold FT8 (red), k parts" / "TX replaced: on air at the next slot"), also when
 *      its own text is then refused; the row counts what will really go.  Only the CQ frame of the
 *      green frame (rule 8) is not the owner's ENT and stays.  An ENT with nothing to send - under 2 characters, or one word
 *      that is a callsign (the untouched "<CALL> " of a tap on a station's row) - is refused ("TX: nothing to send - type a
 *      message") and changes nothing.
 *   4. Cancel and abort.  The ways to stop:
 *      - DISARM (red frame off: MODE menu -> hold FT8 again; row "red off = MODE > hold FT8 again"): seen within 0.5 s.  During a frame it ends the frame at once
 *        (io->tx_end) and clears the whole queue.  Outside a frame it drops every message of the owner - also one held
 *        after a PERM and a strategy-B hold (S1, 24 Nov 2026; row "TX: red frame off - message dropped", note "... waiting
 *        messages dropped").  So: confirm, arm, disarm = nothing further goes out, whatever comes later.
 *      - A TAP while a tap grant is open and not yet keyed gives the grant back (S3, 24 Nov 2026: io->cancel while js8tx_run
 *        waits for the slot, the shell's tap poll before that): "TX: cancelled before the slot".
 *      - THE CANCEL: ENT on the keyboard with the text deleted (empty), or "X", or "CANCEL", clears the queue (shell).
 *      - An owner abort ("@TX" owner=1: mic PTT, STOP or EXIT while a frame is on the air, a failed write) ends the
 *        transmission and clears the queue; while "@TX" says owner=1 (SDK: until the app restarts) every new request and
 *        every tap is refused (S2, 24 Nov 2026: "TX: aborted - restart the app to send").
 *      - A stop request (the app is told to leave) ends a transmission and clears the queue.
 *      EXIT alone does NOT stop the app while nothing is on the air: the app runs in the background and a confirmed
 *      message stays queued (it still needs the arming).  To be sure nothing goes out: ENT on an empty keyboard, and disarm.
 *      "@TX" is read again every 0.5 s between tx_begin and the end of the frame (JS8TX_TXPOLL_MS), so the app does
 *      not depend on the loader failing the write.  A "@TX" read that FAILS changes nothing: it is neither a disarm
 *      nor a (new) arming.  tx_begin answering more than JS8TX_BEGIN_MAX ms is not accepted (tx_end, one try per slot
 *      as for BUSY): the app never sits on a grant for many seconds.
 *   5. Loader errors: PERM -> the message waits for a NEW arming (never asked again before one is seen); BUSY -> at
 *      most one try per slot, JS8TX_BUSY_TRIES in all, then the message is dropped; anything else -> TX is off for
 *      this run.  Every case is one status line.
 *   6. What goes on the air is exactly the confirmed text: a text of which a character fits no frame is refused
 *      (js8_build_message_ex), so is a missing / invalid own call or locator, and a text that starts with a backtick
 *      (JS8Call's raw compound syntax, which js8pack does not build).  The dictionary is used for compression only
 *      when its files are checked (JSCSUM.BIN on the card, each file verified on first use); an unchecked dictionary
 *      is not trusted with the text: Huffman only.
 *   7. JS8.CFG fails safe: TX is OFF unless JS8.CFG says tx=1 (also on / yes).  tx= and txtest= take exactly 0 / 1 / on /
 *      off / yes / no (any case, S9 24 Nov 2026: "2", "00", "onn" are not understood); any other value switches TX OFF (tx=) or
 *      TXTEST ON (txtest=); another tx* key with a bad or
 *      out-of-range value keeps its default; an unknown tx* key (a typing error) is refused.  Each case is one note
 *      after the start.  Key names are case-insensitive (the shell lower-cases them).
 *   8. NO CQ (owner decision 24 Nov 2026; until then: CQ by the green frame, below).  An arming of the green frame
 *      queues nothing and is answered with the row "green not used - hold FT8 again"; the app never asks @QTXC.
 *      Former rule, kept for the record:  The SDK: beacon and CQ are never armed together, and icap_tx_begin answers PERM while
 *      the beacon is not armed - so a CQ cannot go through the beacon path.  It goes through the loader's CQ path
 *      "@QTXC" instead (granted only while the owner has CQ armed; the loader keys the period itself, the app writes
 *      the audio and nothing else).  Each new arming of CQ (cq 0 -> 1, seen to happen) queues exactly ONE frame
 *      "CQ CQ CQ <grid>" at the head of the queue; the arming IS the human confirmation of that one text and of
 *      nothing else.  @QTXC is asked once per period at most, never less than 2 s before the slot; a disarm of CQ
 *      drops that frame (and ends it on the air within 0.5 s).  TXTEST does not do it.  Every other rule applies
 *      as for the beacon path (caps, stop, owner abort, log record).
 *   9. HB beacon (txhb=<minutes>, 10..240, default 0 = off; owner decision 23 Nov 2026, like JS8Call's HB timer): with txhb
 *      set, arming the beacon (red frame, seen to happen during this run, owner=0) is the owner's standing request for
 *      "HB <grid>": queued at once, then every txhb minutes, the interval counted from the last queued HB (so a re-arming
 *      does not send again early).  Only an arming made with the queue EMPTY starts it (review 24 Nov 2026): an arming
 *      that sends a queued message sends no HB, nor does the first arming within JS8TX_LOST_MS after a message of the owner
 *      expired or was dropped by a disarm (U2, 24 Nov 2026: that arming was meant for the message) - the HB starts at the next
 *      fresh arming.  Only HB, only while armed, only
 *      with an empty queue, never in TXTEST; the caps (txsess) and every other rule apply; a disarm drops it like any
 *      waiting message.
 *
 * Consecutive slots (unknown on the real loader, both implemented):
 *   strategy A  frames of one message go out in consecutive slots under one arming (tx_begin per slot);
 *               a PERM for a later frame degrades to B for that message
 *   strategy B  one frame per arming: after each frame the rest waits ("TX B: held, red frame off drops k/n"), and since
 *               24 Nov 2026 the disarm drops it (rule 4) - under B only the first frame of a message goes out
 * TXTEST (txtest=1): only the heartbeat (the keyboard opens with "HB <grid>"); exactly ONE frame per app run; no CQ by
 * the green frame, no HB beacon.
 *
 * The keyboard and the DECODE screen are the shell's (shell/js8app_kbd.h).  Every row this module prints through io->say
 * is at most 40 characters.  Everything that changes what goes on the air is said on the DECODE screen too, not only as a
 * note (U1, 24 Nov 2026): expiry, PERM, disarm, owner abort, armed at start, CQ armed / refused / not granted, HB, a missed tap
 * slot, the TX wait reasons, an arming that sends nothing ("TX: red frame on, nothing to send").
 *
 * Freestanding C99, no libc; all platform access goes through js8tx_io_t.
 */
#ifndef JS8TX_H
#define JS8TX_H
#include <stdint.h>
#include "js8pack.h"
#include "txsynth.h"

#define JS8TX_QMAX       2       /* messages waiting: ONE message of the owner (an ENT replaces it) + the CQ frame of the green frame */
#define JS8TX_FMAX       12      /* frames per message, hard limit (txmaxf <= this) */
#define JS8TX_TEXT       70      /* longest message text accepted (the unit tests and the log record up to 70) */
#define JS8TX_KBD        43      /* the radio's keyboard holds 43 characters: no text over it is offered to the owner (review 24 Nov 2026) */
#define JS8TX_PREP_W     57      /* row width of the prepared-reply table (js8feat: JS8F_REPLY + 1) */
#define JS8TX_PRESETS    4       /* tx1= .. tx4= of JS8.CFG */
#define JS8TX_ITEMS      (2 + JS8TX_PRESETS + 3)   /* js8tx_item indices: 0 HB, 1 CQ, 2..5 presets, 6.. replies (the terminal's "p") */
#define JS8TX_HEARD      12      /* recently decoded audio frequencies kept for the offset choice */
#define JS8TX_BUSY_TRIES 4
#define JS8TX_F_MIN      500     /* automatic audio offset range (tone 0), Hz */
#define JS8TX_F_MAX      2450
#define JS8TX_F_DEFAULT  1500
#define JS8TX_LEAD_MS    1500    /* tx_begin is called this long before the slot start (RTC estimate) */
#define JS8TX_WRITE_MS   400     /* the first audio is written this long before the slot start */
#define JS8TX_EARLY_MS   1000    /* a grant: leave the decoder this long before the slot start */
#define JS8TX_CQ_ASK_MS  2000    /* @QTXC is asked only when the next slot is at least this far away */
#define JS8TX_SEL_MS     60000   /* a text from the terminal waits this long for the owner's tap */
#define JS8TX_CHUNK      400     /* samples per tx_write (50 ms): the stop request is polled in between */
#define JS8TX_TXPOLL_MS  500     /* "@TX" (and "@QSO" cq=) are read this often: disarm / owner abort */
#define JS8TX_BEGIN_MAX  2500    /* tx_begin may answer at most this many ms to the slot start (it is called JS8TX_LEAD_MS before) */
#define JS8TX_LOG_LINE   128     /* buffer for the "T ..." log record: "T 2450 12/12 4294967295 -2147483648 <hex22> <text 70>" = 123 + NUL */

/* js8tx_request() results */
#define JS8TX_OK       0
#define JS8TX_E_OFF  (-1)   /* TX disabled (tx=0, no TX service, dead after an unknown error) */
#define JS8TX_E_CALL (-2)   /* own call missing / not a callsign */
#define JS8TX_E_GRID (-3)   /* own locator missing / invalid */
#define JS8TX_E_TEXT (-4)   /* empty, too long, or a non-printable character */
#define JS8TX_E_ENC  (-5)   /* a character fits no frame (e.g. ':' without the dictionary) / self-check */
#define JS8TX_E_LONG (-6)   /* more frames than txmaxf */
#define JS8TX_E_FULL (-7)   /* JS8TX_QMAX messages are waiting */
#define JS8TX_E_SESS (-8)   /* the frames-per-run cap would be exceeded */
#define JS8TX_E_TEST (-9)   /* TXTEST: only the heartbeat, only once */
#define JS8TX_E_NOSEL (-10) /* (terminal) nothing loaded, or loaded too long ago */
#define JS8TX_E_BUSY  (-12) /* (terminal) a text of the terminal is already waiting for the owner's tap */
#define JS8TX_E_EMPTY (-13) /* (ENT) nothing to send: shorter than 2 characters, or only a callsign */
#define JS8TX_E_ABORT (-14) /* the owner abort is latched ("@TX" owner=1): nothing new until the app restarts (S2, 24 Nov 2026) */

/* where a message came from */
#define JS8TX_SRC_MENU 0    /* the owner's ENT (or a unit test) */
#define JS8TX_SRC_CQ   2    /* the green frame (rule 8) */
#define JS8TX_SRC_HB   3    /* the HB beacon of txhb= (rule 9); otherwise as a message of the owner */
#define JS8TX_LOST_MS  120000u   /* U2: an arming within this time after a message expired / was dropped sends no HB */

/* loader error codes as in icap.h (kept here so that the module needs no icap.h) */
#define JS8TX_L_NAME  (-4)
#define JS8TX_L_PERM  (-5)
#define JS8TX_L_BUSY  (-8)

typedef struct {
    int      (*tx_begin)(void);                           /* icap_tx_begin */
    int      (*tx_write)(const int16_t *b, int n);        /* icap_tx_write */
    void     (*tx_end)(void);                             /* icap_tx_end */
    uint32_t (*ticks)(void);                              /* monotonic ms */
    int      (*stop)(void);                               /* 1 = the app must leave (sticky) */
    void     (*wait)(uint32_t ms);                        /* sleep */
    int      (*vfile)(const char *name, char *b, int cap);/* read a loader virtual file: length, or the open error (< 0) */
    int      (*cycle_ms)(void);                           /* ms since the last :00 / :30 of the radio clock, < 0 = not known */
    void     (*say)(const char *row, int stamp);          /* stamp 1: a DECODE row "TX <f0> k/n <text>" (the shell prefixes the time
                                                           * of the next slot); stamp 0: a short result row ("TX queued ...") */
    void     (*note)(const char *line);                   /* status / error line (app window + log); may use the SD card */
    void     (*log)(const char *line);                    /* one log record of a transmitted frame; may use the SD card */
    int      (*cancel)(void);                             /* optional (0 = none): 1 = the owner tapped a row - a tap grant not yet
                                                           * keyed is given back (S3, 24 Nov 2026).  Polled while js8tx_run waits for
                                                           * the slot of a grant (tap credit or CQ); never on the air */
} js8tx_io_t;

typedef struct {
    uint8_t  frames[JS8TX_FMAX + 2][JS8_FRAME_BYTES];     /* +2: js8_build_message wants two rows of headroom */
    uint8_t  n, k;                                        /* frames, frames already sent */
    uint8_t  hold;                                        /* 0 ready, 1 waits for a new arming, 2 waits for a tap grant */
    uint8_t  busy;                                        /* BUSY answers so far */
    uint8_t  gen;                                         /* arming generation at the time of the hold */
    uint8_t  src;                                         /* JS8TX_SRC_* */
    uint8_t  bh;                                          /* the hold is strategy B's (a disarm drops it; a PERM hold survives it) */
    uint16_t f0;                                          /* audio offset of the whole message, 0 = not chosen yet */
    uint32_t t_req;                                       /* tick of the request / of the last frame sent */
    char     text[JS8TX_TEXT + 1];
} js8tx_msg_t;

typedef struct {
    const js8tx_io_t *io;
    const char *mycall;
    js8jsc_t   *jsc;                                      /* dictionary for JSC compression, 0 = Huffman only */
    char      (*prep)[JS8TX_PREP_W];                      /* prepared replies to incoming queries (js8feat), "" = none.  Only offered
                                                           * (the keyboard opens with one on a tap on the query): sent only on ENT */
    uint8_t     prep_n;
    int16_t    *buf;                                      /* >= JS8TX_CHUNK samples of scratch */
    /* configuration (js8tx_cfg) */
    uint8_t  enable, test, strat_b, maxf, use_jsc;
    uint8_t  cfg_err;                                     /* JS8.CFG values refused: 1 tx=, 2 txtest=, 4 another tx* key, 8 txpath, 16 unknown tx* key */
    uint8_t  term;                                        /* txterm=1: the terminal may hand texts to the radio (link builds; default 0) */
    uint8_t  tap;                                         /* txtap=1 (default): one-frame messages on the loader's tap credit (rule 1a) */
    uint16_t sess_cap, ttl_s, f_cfg;
    uint8_t  hb_min;                                      /* txhb=<min>: while the owner has the beacon armed, one HB every hb_min minutes (0 = off; rule 9) */
    uint8_t  hb_arm;                                      /* this arming may send HB: it was made with the queue empty (rule 9) */
    uint32_t t_hb;                                        /* when the last automatic HB was queued, 0 = none yet */
    uint32_t t_lost;                                      /* U2: when a message of the owner last expired / was dropped by a disarm, 0 = not lately */
    uint8_t  ab0;                                         /* an owner abort was seen: an arming counts only after beacon=0 was read again (O4) */
    uint8_t  arm_new, hb_said;                            /* a new arming to report in js8tx_poll / its HB row was given */
    uint8_t  last_src;                                    /* src of the last message that went out whole (its "SENT" / "HB SENT" row, U4) */
    char     grid[5];
    char     preset[JS8TX_PRESETS][JS8TX_TEXT + 1];
    /* state */
    js8tx_msg_t q[JS8TX_QMAX];
    uint8_t  qn;
    uint8_t  dead, test_used, armed, owner, have_tx, gen, grant;
    uint8_t  seen0, said_stale;                           /* "@TX" was read with beacon=0 at least once / the "armed at start" note was given */
    uint8_t  cq, cq_seen0, cq_off;                        /* CQ armed (green frame, seen to happen) / cq=0 was read once / no "@QSO" */
    uint8_t  in_run;                                      /* inside js8tx_run: a disarm is handled there, not by the queue rule of poll_tx */
    uint8_t  tx_any, gen_tx;                              /* a frame went out (not on a grant) / the arming generation it went out under */
    int8_t   gate, clk;
    uint16_t sess_frames;
    uint32_t t_tx, t_slot, t_next;                        /* poll time, granted slot, earliest next try */
    uint32_t n_begin, n_frames_ok, n_frames_cut, n_refused, n_aborts;   /* statistics */
    struct { uint16_t f; uint8_t age; } heard[JS8TX_HEARD];
    txs_mfsk_t wave;
    uint8_t  tones[79];
#ifdef JS8_LINK
    char     lsel[JS8TX_KBD + 1];                         /* the text the TERMINAL loaded, waiting for its GO (over 43 characters: refused) */
    char     tpend[JS8TX_KBD + 1];                        /* after the GO: waiting for the owner's tap (the keyboard opens with it), kept until ENT */
    uint32_t t_lsel, t_tpend;
#endif
} js8tx_t;

/* buf: scratch of at least JS8TX_CHUNK samples (may be shared with the receiver's read buffer).  TX starts OFF. */
void js8tx_init(js8tx_t *t, const js8tx_io_t *io, const char *mycall, js8jsc_t *jsc, int16_t *buf);
/* One "key=value" token of JS8.CFG (s points at the key, lower case): tx= txtest= txstrat= txpath= (refused) txf= txmaxf=
 * txsess= txttl= txjsc= txterm= txhb= txtap= loc= tx1= .. tx4=, and any other tx* key (refused).  Returns 1 if the key was one of
 * them, 2 for a preset (its value is the rest of the line: the caller must not look for keys in it).  Never calls
 * io->note (the shell's log is not ready while JS8.CFG is read): refused values are remembered in cfg_err and
 * reported by the first js8tx_poll. */
int  js8tx_cfg(js8tx_t *t, const char *s);
/* The locator from the radio ("@MYLOC"); used when JS8.CFG has no loc=. */
void js8tx_set_grid(js8tx_t *t, const char *loc);
/* Queue a confirmed message for the beacon path; JS8TX_OK or JS8TX_E_*.  Called by js8tx_send() (and by the unit tests,
 * which stand for the owner's ENT). */
int  js8tx_request(js8tx_t *t, const char *text);
const char *js8tx_err(int code);
/* Would this text be accepted?  Frames (> 0) or JS8TX_E_*; nothing is queued. */
int  js8tx_check(js8tx_t *t, const char *text);
/* ENT on the radio's keyboard: the owner saw exactly this text and confirmed it.  Queued; a one-frame message with nothing
 * else waiting is sent on the loader's tap credit when it grants (*granted = 1), everything else waits for the beacon.
 * Frames (> 0) or JS8TX_E_*.  Says what happens ("TX: on air at the next slot (tap)", "3 parts: MODE > hold FT8 (red),
 * 2 min", "TX refused: ...") through io->say.  js8tx_run says "SENT: <text>" (after an HB of txhb=: "HB SENT, next in N
 * min") when a message has gone out whole, and while the beacon is armed also "red frame off to tap again". */
int  js8tx_send(js8tx_t *t, const char *text, int *granted);
/* Item i the terminal can load (0 HB, 1 CQ, 2..5 presets tx1..tx4, 6.. prepared replies) -> text; 0 = none. */
int  js8tx_item(const js8tx_t *t, int i, char *o);
/* The receiver decoded a frame at this audio frequency / a period has passed (ages the list). */
void js8tx_heard(js8tx_t *t, int freq_hz);
void js8tx_period(js8tx_t *t);
/* Cheap; call it often (main loop, decoder abort callback), never from inside a dictionary read.  Polls "@TX" and
 * the cq= of "@QSO", asks "@QTXC" for a CQ frame, drops expired requests (and an old text of the terminal).  Returns the ms
 * until js8tx_run() must be called (0 = now), or -1 when nothing is to be sent. */
int  js8tx_poll(js8tx_t *t);
/* One slot: tx_begin (or the grant), the audio of ONE frame, tx_end.  Blocks up to ~15 s, polls the stop request at
 * least every JS8TX_CHUNK samples.  Returns 1 if audio was written (the receiver must skip this period), 0 if
 * nothing was keyed. */
int  js8tx_run(js8tx_t *t);
/* Stop request / leaving / the cancel (ENT on an empty keyboard): end a transmission in progress, clear the queue. */
void js8tx_abort(js8tx_t *t, const char *why);
/* One line (<= 40 characters) for the app window. */
void js8tx_status(const js8tx_t *t, char *out);

#ifdef JS8_LINK
/* ---- the terminal path (js8link.h; compiled only with -DJS8_LINK) ---------------------------
 * The terminal can only HAND a text to the radio: js8tx_select() loads a text (nothing is queued, nothing is keyed) and
 * says how many frames it would be; js8tx_confirm() - the shell calls it only for a GO line that carried the connection's
 * nonce - keeps exactly that text in tpend: the owner's next tap on a row opens the radio's keyboard with it, and only his
 * ENT there sends it (safety review 23 Nov 2026).  tpend is forgotten after JS8TX_SEL_MS and when the link goes. */
int  js8tx_select(js8tx_t *t, const char *text);       /* frames (> 0), or JS8TX_E_* */
int  js8tx_confirm(js8tx_t *t);                        /* frames handed to the radio (> 0), or JS8TX_E_* */
#endif

/* ---- pieces exposed for the tests --------------------------------------------------------- */
/* frame -> tones -> synthesizer state: Normal submode, 1280 samples per symbol, 6.25 Hz spacing, SDK default
 * level (-2 dBFS) and ramp (5 ms raised cosine at both ends).  0 or a TXE_* code. */
int  js8tx_wave(txs_mfsk_t *w, uint8_t tones[79], const uint8_t frame[JS8_FRAME_BYTES], int f0_hz);
/* the audio offset the next message would get */
int  js8tx_pick_offset(const js8tx_t *t);

#endif
