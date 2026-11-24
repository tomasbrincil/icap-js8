/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8app.c - JS8 (JS8Call compatible, submode Normal) RECEIVER app for the IC-705 ICAP loader.
 *
 * Modified 2026 for JS8 by this project.
 * JS8 variant of the SDK app shell apps/digi-common/digiapp.c (Copyright (c) 2026 Dawid SQ6EMM,
 * GPL-3.0-or-later): the capture state machine (cap_*, pump), the SD-backed sample source
 * (src_read) and the abort poll (dec_abort) are copied from it with the FT8 parameters (15 s
 * period, capture 14.6 s from the period edge, two alternating capture files, a finished capture
 * is decoded while the next period is recorded).  The JT4 / PI4 / FT8 blocks, the beacon, the QSO
 * mode and the @SPOT glue are NOT in this file.
 * The two-slot scheduling (a finished capture is decoded while the next period is captured into
 * the other slot) follows apps/wspr/wsprapp.c of the same SDK (same author, GPL-3.0-or-later).
 *
 * Two build flavours of this one source:
 *   (default)   RX ONLY: no transmit, beacon or network code is compiled in - the app never calls
 *               icap_tx_* or icap_net_* (JS8.APP).
 *   -DJS8_TX    RX + TX: adds the transmit path js8tx.c (outbox, safety rules, the loader's grants; see js8tx.h), its
 *               loader binding shell/js8app_tx.h (the only place that calls icap_tx_*) and the control
 *               shell/js8app_kbd.h (a tap on a row opens the radio's own keyboard, ENT sends; -DJS8_KBD: the real
 *               keyboard of 705J 1.40 / v14 J).  Deployed under the file name FT8.APP, because the loader gives 15 s
 *               slots and beacon arming only to a file of that name.  A frame is keyed only for a text the owner saw
 *               whole on the keyboard and confirmed with ENT, on the loader's tap credit (one frame) or the owner's
 *               arming; with an empty queue it never calls icap_tx_begin.  Still no network code.
 *   -DJS8_LINK  (with or without JS8_TX) adds the terminal link: one outbound TCP connection to the Cardputer
 *               (js8link.h, shell/js8app_link.h - the only place that calls icap_net_*; PROTOCOL.md).  The
 *               terminal shows what the radio shows and can HAND a text to the radio: its GO (with the nonce of the
 *               connection) keeps it until the owner's next tap opens the radio's keyboard WITH it, and only his ENT
 *               there sends it (safety review 23 Nov 2026).
 *               Extra JS8.CFG keys: term=<host[:port]> (NO default: without it the link is off and the app makes
 *               no network call; the Cardputer's soft-AP address is 192.168.4.1), term2=<host[:port]> (a second
 *               address, e.g. at home), txterm=0|1 (default 0: the terminal can look but not request a
 *               transmission - with txterm=1 and the beacon armed, WHATEVER ACCEPTS THE CONNECTION at the term=
 *               address can hand a text of its choice to the radio - never on the air by itself).
 *               While a slot is transmitted the receiver does not capture or decode that period; the
 *               capture of the period before is closed 1 s early and decoded after the transmission.
 *               Extra JS8.CFG keys: tx=0|1 (TX is OFF unless tx=1), txtest=0|1 (both also on/off/yes/no; a value that
 *               is not understood switches TX OFF resp. TXTEST ON), txstrat=A|B, txf=<Hz, 0 auto>, txmaxf=, txsess=,
 *               txttl=, txjsc=0|1, txhb=<minutes 10..240, 0 off: HB beacon while the beacon is armed>, txtap=0|1
 *               (default 1: a one-frame message on the loader's tap credit), kbdirq=0|1 (default 0; 1: open the keyboard
 *               with IRQs disabled - untested on the radio), popup= defaults to 0 in this flavour (a tap that dismisses
 *               the modal popup might reach the app as a row tap), tx1= .. tx4=<preset text, the rest of the line; "P1".."P4" on the keyboard>.  A
 *               bad or out-of-range value of another tx* key keeps the default and gives a note; an unknown tx* key
 *               (txpath= included, gone since 24 Nov 2026) is refused with a note.  The CW keyer memories of the radio
 *               come from JS8MEM.TXT (written by MEM.APP; "m1=<text>" .. "m8=<text>"): "M1".."M8" on the keyboard.
 *               The frame log gets one more record: "T <yyyymmddThhmmss> <f0> <k/n> <samples> <rc> <hex22> <text>".
 *
 * Everything runs in the radio:
 *   js8dec  audio -> 87-bit frames (LDPC + CRC-12 verified)
 *   js8msg  frame -> the text JS8Call shows for it (JSC dictionary = files JSC*.BIN in the app
 *           folder, tools/mkjsc.py; without them the app still runs and compressed frames are
 *           shown raw as "#<12 chars>")
 *   js8asm  frames -> complete messages (per audio frequency)
 *
 * DECODE screen (icap_decode_out, EVERY row <= 40 characters - decodes, status rows of the transmitter, views: longer
 * rows wrap on the radio when the mini-scope is shown; in decoded text the word HEARTBEAT is shown as HB here, the logs
 * keep the full text and the seconds):
 *   "HHMM sNN FFFF TEXT"          one decoded frame (period start, SNR dB / 2500 Hz, tone-0 Hz)
 *   "HHMM sNN FFFF = TEXT"        a complete reassembled message of more than one frame
 *   "HHMM sNN FFFF > TEXT"        a complete message addressed to MYCALL; a one-frame message to
 *                                 MYCALL is shown only in this form.  It is also raised as a popup -
 *                                 at most ONE per 15 s period, at most 4 in a row and then one per 20
 *                                 periods, and none for a message with a wrong checksum or an incomplete
 *                                 one (any station can address us; the popup is modal)
 *   "     TEXT"                   continuation of a text that did not fit the line
 *   "TX? <answer>"                (TX flavour) the answer to a query to MYCALL (SNR? GRID? INFO? STATUS? HEARING?),
 *                                 PREPARED only: a tap on it or on the query opens the radio's keyboard with it,
 *                                 and only ENT sends it (shell/js8app_kbd.h)
 *   "TX ..." rows                 (TX flavour) what the transmitter does: "TX queued: arm beacon to send 3 frames",
 *                                 "TX: on air at the next slot (tap)", "TX refused: ...", "HHMM TX <f0> k/n <text>"
 *   A tap on any row opens the radio's keyboard (TX flavour; shell/js8app_kbd.h).
 * MYCALL = call= of JS8.CFG, else the radio's "@MYCALL".  Locator = the radio's "@MYLOC", else loc=.
 *
 * Files in the app folder (storage layer: shell/js8app_store.h):
 *   JS8CAPA.PCM / JS8CAPB.PCM  the two capture files (8 kHz s16)
 *   JS8.CFG      optional.  Key names in any case, blanks around '=' allowed, a UTF-8 BOM is skipped, a line starting
 *                with '#' or ';' is a comment (and the rest of a line from " #" / " ;", except in the text keys info=
 *                status= tx1=..tx4=, whose value is the rest of the line); a file longer than the app reads (CFG_SZ)
 *                gives a note and switches TXTEST on.  Keys: flo= fhi= (tone-0 search range, Hz; default 450..2550 - a wider range
 *                works too, the decoder then goes over the band in two passes and takes longer; a range
 *                narrower than 100 Hz is refused), call= (overrides @MYCALL), loc= (used when the radio has
 *                no position), groups=@ALLCALL,@OK (group calls shown as "to my group"), popup=0,
 *                keep=N (segment files per log stream, 2..9999, default 600), flush=N (periods a log line
 *                may wait in RAM, 1..40, default 4), info=<text> / status=<text> (answers to INFO? / STATUS?,
 *                up to 40 characters, the rest of the line; the prepared answer is cut to the 34 characters of a row)
 *   JS8Lnnnn.TXT frame log segments:   "R <yyyymmddThhmmss> <snr> <dt> <freq> <hex22> <text>" every frame,
 *                                      "N <hhmmss> <text>" notes / errors, "T ..." transmitted frames
 *   JS8Mnnnn.TXT message log segments: "M <yyyymmddThhmmss> <snr> <freq> <frames> <flags> <text>"
 *                Decoder tuning (decimal numbers, out-of-range values are clamped, read once at start):
 *                  maxcand=10..300   sync candidates kept per search                      (default 300)
 *                  minscore=0..60    lowest sync score of a candidate                     (default 10)
 *                  nsync=0..21       Costas tones (of 21) a candidate needs to be tried   (default 7)
 *                  passes=1..31      bit mask of the decoding attempts per candidate: 1 = first metric,
 *                                    2 = dB metric, 4 = first 24 LLRs erased, 8 = first 48 erased,
 *                                    16 = interpolated tone powers                        (default 31)
 *                  ns34=0..21        Costas tones needed for the attempts 2, 3 and 4      (default 10)
 *                  iters=5..50       BP iterations per attempt; the decoder makes half as many
 *                                    layered sweeps (30 -> 15)                            (default 30)
 *                  stream=0|1        build the first waterfall while the period is captured, so that
 *                                    only search + decoding are left afterwards           (default 1)
 *                  shift=0|1         residual pass on the half-row time grid              (default 1)
 *                  sub=0..2          further residual passes on the normal time grid      (default 1)
 *                                    (a residual pass re-reads the capture from the card, subtracts
 *                                    what was decoded and searches again next to it)
 *                  wide=0|1          1: one kind of residual pass (next to every new signal and the strong
 *                                    failures), made only on evidence of a station under a decoded one;
 *                                    0: the scheme of the first build, use it with sub=2  (default 1)
 *                  budget=0|1000..14500  ms: no further residual pass is started that cannot end by then,
 *                                    and at that time the decode of the period is ended; what was decoded
 *                                    is used, with the same note as for the hard limit (0 = only the hard
 *                                    limit: the next capture)                             (default 8000)
 *                  perf=0|1          write JS8PERF.TXT                                    (default 1)
 *                The first build's decoding scheme (with the new kernels):
 *                  minscore=8 ns34=7 sub=2 wide=0 budget=0 stream=0
 *   JS8PERF.TXT  rewritten after every period (bounded: 3 lines + the last 16 periods), times in ms:
 *                "JS8PERF 1 <panel line 0> | <panel line 1> arena <peak>/<size>"
 *                "S n<periods> c<cut> ca.. tr.. bp.. it.. rd.. su.. ff.. mg.. sr.. ll.. b1.. b2.. b3.. b4.. b5..
 *                   fi.. po.. k0.. k1.. k2.. rc.. sc.. sb.. i1..i5.. r1..r5.. a1..a5.. nf.. lv.. pl.. sg.. w0.. w1..
 *                   w2.. nb.. T.. pt.. sd.. pm.. pw.."   SUMS since the start (they wrap at 2^32):
 *                   ca candidates, tr tried, bp BP runs, it BP iterations; TIMES per decoder stage (js8dec.h):
 *                   rd capture read, su subtract, ff window+FFT, mg power->dB, sr sync search, ll candidate
 *                   LLRs, b1..b5 BP by attempt, fi re-encode/refine, po abort polls (capture pump); k0/k1/k2
 *                   time in the first pass / the passes after a subtraction / the shifted-grid passes;
 *                   COUNTERS: rc capture chunks read (640 samples each), sc sync scores, sb signal x row
 *                   subtractions, i1..i5 BP iterations, r1..r5 BP runs, a1..a5 frames accepted by attempt,
 *                   nf FFT rows, lv LLR vectors, pl polls, sg signals, w0/w1/w2 waterfalls (first / after
 *                   subtraction / shifted), nb band passes; then T decode total, pt result phase (text,
 *                   dictionary, logs), sd pure SD read calls, pm capture pump during the decode, pw writing
 *                   this file
 *                "X T.. r.. f.. s.. d.. u.. p.. t.. b.."   maxima of the P fields
 *                "P <hhmmss> d<frames> <-|D|B> T<decode> r<read> f<STFT> s<search> d<LLR+BP> u<subtract>
 *                   p<post> t<tried> b<BP runs> o<poll> i<BP iterations> w<normal>/<shifted waterfalls>
 *                   g<signals>"   one period; D = cut by the hard limit, B = cut by budget=, T = left for a transmission
 *                flags: > to me, g to my group, m my call mentioned, c checksum ok, x checksum BAD,
 *                t closed by timeout, p beginning missing, l truncated (longer than 1023 characters),
 *                e closed because the app left (the message was still being received), - none
 *   JS8Annnn.ADI ADIF records of marked exchanges, where the loader has no "@QLOG" (-> ICAPQSO.ADI)
 *   JS8STA.BIN / JS8STB.BIN  the state, written in turn: ring positions, heard table, inbox, open view
 *   Every segment is written ONCE ("H <stream> <seq> <EOH>", lines, "E <lines> <crc>") and never opened
 *   for writing again; each stream is a ring of keep= files.  See js8app_store.h for the rules and for
 *   what a power cut can cost (the lines still waiting in RAM, and the one file being written).
 *   Text in the logs, on the DECODE screen and in the popup is printable ASCII: every other byte a
 *   frame or a dictionary word may carry (newline, 0x1A, Latin-1) is shown as a blank or '?', so a
 *   received frame cannot start a new log line.
 *   JSC*.BIN     the dictionary - on the SD card ROOT (ICAP_O_ROOT; preferred: the loader does not list apps whose
 *                directory entries come after ~257 files in IC-705/Apps), else in the app folder; decided at start
 *                by JSCHEAD.BIN.  With JSCSUM.BIN (of the same place) every file is checked on its first use
 *                (js8jsc.h): a corrupt file is not used and the frames that need it are shown raw.
 * At most 2 files are open at any time (capture + one of: decode read, dictionary file, segment / state).
 *
 * The 8-line app window has four views (js8feat.h; chosen by the terminal); every line is cut to
 * the window's 40 bytes, times in ms:
 *   STATUS  0 "JS8 RX <call> <flo>-<fhi> Hz" ("JS8 RX+TX ..." in the TX flavour)
 *           1 "ms10 n7/10 p31 it30 su1 sh1 w1 s1 b8000"   the decoder tuning in use (JS8.CFG: minscore nsync/ns34
 *             passes iters sub shift wide stream budget; maxcand is not shown).  TX FLAVOUR: the transmit status
 *             instead (js8tx_status); the tuning is then in the first line of JS8PERF.TXT
 *           2 last note or error
 *           3 "last HHMMSS 12d 9845ms t65 b281 CUT"   last period: frames, decode time, candidates tried, BP runs;
 *             CUT = ended by the hard limit, BUD = by budget=, TX = left for a transmission
 *           4 "r1200 f3100 s560 d4100 u670 p300"      last period: capture read, STFT, sync search, decode (LLR +
 *             BP), subtraction, post (text + logs)
 *           5 "R.. F.. S.. D.. U.. P.."               running maxima of line 4
 *           6 "dict <words> [BAD <files>] rd <SD reads> fr <frames> msg <messages> me <to me> [new <unread>]"
 *             (or "no dictionary, raw # ...")
 *           7 "c<cut>/<periods> k<skipped> g<gaps> av<avg decode> mx<max> T<max tried> B<max BP runs>", "LOGERR" first
 *   BAND    the last 7 decoded lines        INBOX  the newest messages for me / my groups, unread marked
 *   HEARD   stations heard: call, grid, SNR, audio offset, age, the report they sent us
 * The app starts on STATUS; on the other views " !" at the end of the title says that a note or an error
 * came which STATUS has not shown yet.
 *
 * Stop request (icap_should_stop): polled in the main loop, by the decoder's abort callback, before every
 * decoded frame is processed, in every dictionary read and between the start-up loads.  The only stretch
 * that is not interruptible is the write of one segment (<= 4 KB) and of the state file (1.2 KB).
 *
 * Memory: the loader's data region (icap_heap) holds, in this order, the reassembly state, the
 * dictionary state + head table, the two segment buffers, the station features (heard table, inbox,
 * band lines, prepared replies) - together JS8_RESERVE bytes - and then the decoder work arena.
 * Built only against the public SDK icap.h.
 */
#include "icap.h"
#include "js8dec.h"
#include "js8msg.h"
#include "js8asm.h"
#include "js8feat.h"
#ifdef JS8_TX
#include "js8tx.h"
#define CFG_SZ 800                      /* JS8.CFG may carry four preset texts, info= and status= */
#else
#define CFG_SZ 400
#endif

#ifdef JS8_LINK
#include "js8link.h"                    /* the terminal link (compile-time optional; binding: shell/js8app_link.h) */
static js8link_t *lnk;                  /* in the data region; 0 until app_main has set it up */
static void link_row(const char *row) { icap_decode_out(row); if (lnk) js8link_put(lnk, "D ", row); }
#define icap_decode_out link_row        /* every DECODE row also goes to the terminal, exactly as shown */
static void link_note(const char *s);
static void link_msg(int inbox, const char *logline);
static void link_service(void);
#ifdef JS8_TX
static void link_queued(void);
static void link_tx_start(const char *row);
static void link_tx_end(const char *line);
static int link_stop(void);
#endif
#endif

#define MODE_NAME  "JS8"
#define PERIOD     15                   /* s: :00 :15 :30 :45 */
#define PREROLL    0u                   /* capture from the period start (TX starts 0.5 s into it) */
#define FS         8000u
#define CAP_SAMP   (146u * FS / 10u)    /* 14.6 s: TX 12.64 s from :00.5 + DT up to ~+1.4 s, ends before the next edge */
#define CAP_TMO_MS 14800u               /* by time: still before the next period's edge */
#define NOM_START  0.5f                 /* DT = 0 <=> signal starts 0.5 s into the period */
#define NOM_MIN    (-1.0f)              /* edge not seen: the decoder searches DT -2.2..+2.4 s around the nominal */
#define E_ABORT    JS8_ERR_ABORT
#define E_IO       JS8_ERR_IO
#define F_MAXCFG   3900                 /* tone 0 below 4 kHz - 50 Hz */
#define MAXOUT     50
#define NKEYS      (86400 / PERIOD)     /* periods per day */
#define F_CFG      MODE_NAME ".CFG"
#define F_LOG      MODE_NAME "LOG.TXT"
#define F_MSG      MODE_NAME "MSG.TXT"
#define F_PERF     MODE_NAME "PERF.TXT"
#ifndef JS8_PERF_FILE
#define JS8_PERF_FILE 1                 /* 0: build without JS8PERF.TXT (panel timing, tuning keys and budget= stay; 2 KiB less) */
#endif
#define PERF_N     (JS8_PERF_FILE ? 16 : 1)   /* periods kept in JS8PERF.TXT */

#ifndef JS8_HEAD_MAX
#define JS8_HEAD_MAX 35840u             /* RAM for the dictionary head table (JSCHEAD.BIN is 34661 B) */
#endif
#ifndef JS8_LOG_SZ
#define JS8_LOG_SZ   4096u              /* frame-log lines waiting in RAM = the largest JS8L segment (~45 frames) */
#endif
#ifndef JS8_MSG_SZ
#define JS8_MSG_SZ   1536u              /* message-log lines waiting in RAM = the largest JS8M segment (one message line is <= 1064) */
#endif
#ifndef JS8_RESERVE
#define JS8_RESERVE  55936u             /* the shell's part of the data region; the decoder arena is the rest (189824 of 245760) */
#endif
#define DEC_W       40                  /* longest DECODE line: the native DECODE screen wraps longer rows when its mini-scope is shown */
#define WIN_W       40                  /* app window: 40 bytes per line */
#define KEEP_DFL    600                 /* segment files per log stream (JS8.CFG keep=): at a busy band (a segment per 4 periods) about 10 hours */
#define KEEP_MAX    9999
#define FLUSH_DFL   4                   /* periods a log line may wait in RAM (JS8.CFG flush=) */
#define POP_BURST   4                   /* popups: at most this many in a row ... */
#define POP_REFILL  20u                 /* ... and one more every 20 periods (5 minutes) */
#define BAND_MIN    100                 /* Hz: narrowest tone-0 range accepted from JS8.CFG */
#define PFX_W       14                  /* "HHMM sNN FFFF " */
#define CONT_W      5                   /* indent of a continuation line */

#ifdef DIGI_HOST
#include <string.h>
#else                                            /* freestanding on the radio; the host harness links libc */
void *memcpy(void *d, const void *s, unsigned n) { char *p = d; const char *q = s; while (n--) *p++ = *q++; return d; }
void *memset(void *d, int c, unsigned n) { char *p = d; while (n--) *p++ = (char)c; return d; }
#endif

static const char *const F_CAP[2] = { MODE_NAME "CAPA.PCM", MODE_NAME "CAPB.PCM" };

#define COLD __attribute__((cold, noinline))    /* status / log code: built for size, never inlined */
static char *cat(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static void mvdown(char *d, const char *s, uint32_t n) { while (n--) *d++ = *s++; }   /* overlapping, towards lower addresses */
static char *num(char *d, int v, int w, char pad)
{
    char t[12]; int n = 0; unsigned u = v < 0 ? (unsigned)-v : (unsigned)v;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 10);
    if (v < 0) t[n++] = '-';
    while (n < w && n < 11) t[n++] = pad;
    while (n) *d++ = t[--n];
    *d = 0; return d;
}
static char *hhmmss(char *d, int hh, int mm, int ss) { d = num(d, hh, 2, '0'); d = num(d, mm, 2, '0'); return num(d, ss, 2, '0'); }
static char *snr3(char *d, int v)                /* "+05", "-12" */
{
    if (v > 99) v = 99; else if (v < -99) v = -99;
    *d++ = v < 0 ? '-' : '+';
    return num(d, v < 0 ? -v : v, 2, '0');
}
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int quit;                                 /* stop was requested (sticky) */
static int stop(void) { if (!quit && icap_should_stop()) quit = 1; return quit; }
static void pump(void);
/* printable ASCII only: newline / CR / tab -> blank, every other byte outside 32..126 -> '?' */
static char clean(char ch)
{
    unsigned char u = (unsigned char)ch;
    return (u == '\n' || u == '\r' || u == '\t') ? ' ' : (u < 32 || u > 126) ? '?' : ch;
}
static char *cat_clean(char *d, const char *s) { while (*s) *d++ = clean(*s++); *d = 0; return d; }
static void set_line(int row, const char *s)     /* the app window takes 40 bytes per line */
{
    char b[WIN_W + 1]; int k = 0;
    while (s[k] && k < WIN_W) { b[k] = clean(s[k]); k++; }
    b[k] = 0;
    icap_set_line(row, b);
}

/* ---- config: JS8.CFG (key=value text); MYCALL from the radio */
static float cfg_flo = 450.0f, cfg_fhi = 2550.0f;   /* widest range the decoder does in ONE pass in the arena left */
#ifdef JS8_TX
static int cfg_popup = 0;                        /* U7: no popup by default - its dismissing tap might reach the app as a row tap */
#else
static int cfg_popup = 1;
#endif
static int cfg_keep = KEEP_DFL, cfg_flush = FLUSH_DFL, cfg_band_bad;
static char cfg_call[JS8_CALL_MAX], cfg_groups[JS8ASM_GROUPS_MAX], cfg_loc[7];
static char mycall[JS8_CALL_MAX], mygrid[7];     /* mygrid: "" or a valid locator of 4 or 6 characters */
static js8feat_t *feat;                          /* heard table, inbox, views, prepared replies (in the data region) */
static int cfg_sub = 1, cfg_budget = 8000, cfg_perf = 1, cfg_stream = 1, cfg_long;
#ifdef JS8_TX
static int cfg_kbdirq = 0;                       /* 1: open the radio's keyboard with IRQs disabled (js8app_kbd.h; untested on the radio, S10) */
#endif
/* decoder tuning keys: value clamped to lo..hi */
static const struct { const char *k; int *v; int16_t lo, hi; } tkeys[] = {
    { "maxcand", &js8_tune.max_cand, 10, 300 }, { "minscore", &js8_tune.min_score, 0, 60 },
    { "passes", &js8_tune.passmask, 1, 31 }, { "iters", &js8_tune.iters, 5, 50 }, { "sub", &cfg_sub, 0, 2 },
    { "shift", &js8_tune.shift, 0, 1 }, { "nsync", &js8_tune.nsync_min, 0, 21 },
    { "budget", &cfg_budget, 0, 14500 }, { "perf", &cfg_perf, 0, 1 }, { "stream", &cfg_stream, 0, 1 },
    { "wide", &js8_tune.swide, 0, 1 }, { "ns34", &js8_tune.nsync34, 0, 21 },
#ifdef JS8_TX
    { "kbdirq", &cfg_kbdirq, 0, 1 },
#endif
};
static int kmatch(const char *s, const char *k) { int i = 0; while (k[i] && s[i] == k[i]) i++; return k[i] == 0 && s[i] == '='; }
static int kint(const char *s)                   /* digits after '=' */
{ int v = 0, nd = 0; while (*s && *s != '=') s++; if (*s) s++; for (; *s >= '0' && *s <= '9' && nd < 6; s++, nd++) v = v * 10 + (*s - '0'); return nd ? v : -1; }
static void ktok(const char *s, char *out, int cap)  /* token after '=' (stops at blank/eol), upper-cased; ',' -> ' ' */
{
    int k = 0; while (*s && *s != '=') s++; if (*s) s++; while (*s == ' ') s++;
    for (; *s > ' ' && k < cap - 1; s++) out[k++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s == ',' ? ' ' : *s;
    out[k] = 0;
}
/* free text after '=' up to the end of the line: printable, upper-cased, blanks trimmed.  Returns the last character
 * of the line, so that words inside the text are not read as keys. */
static char *ktext(char *s, char *out, int cap)
{
    int k = 0;
    while (*s != '=') s++;
    while (s[1] == ' ') s++;
    while (s[1] && s[1] != '\n' && s[1] != '\r') { s++; if (k < cap - 1) out[k++] = clean((*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s); }
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
    return s;
}
static int lc_c(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int keych(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
/* JS8.CFG as people write it -> the form the key matchers read (in place; returns the new length):
 *   a UTF-8 BOM is skipped; a line whose first non-blank character is '#' or ';' is a comment, and so is the rest of a
 *   line from a blank followed by '#' / ';' (but not inside a text value: info= status= tx1= .. tx4= take the whole
 *   rest of their line); blanks around '=' are removed; key names are lower-cased (keys are case-insensitive). */
static COLD int cfg_norm(char *b, int n)
{
    int i = 0, o = 0, ls = 1, txt = 0;
    if (n >= 3 && (uint8_t)b[0] == 0xEF && (uint8_t)b[1] == 0xBB && (uint8_t)b[2] == 0xBF) i = 3;
    while (i < n) {
        char c = b[i];
        if (c == '\n' || c == '\r') { b[o++] = c; i++; ls = 1; txt = 0; continue; }
        if (txt) { b[o++] = c; i++; continue; }
        if ((c == '#' || c == ';') && (ls || b[o - 1] == ' ' || b[o - 1] == '\t')) { while (i < n && b[i] != '\n' && b[i] != '\r') i++; continue; }
        if (c == ' ' || c == '\t') { b[o++] = c; i++; continue; }
        if (c == '=') {
            int k;
            while (o && (b[o - 1] == ' ' || b[o - 1] == '\t')) o--;
            for (k = o; k && keych(b[k - 1]); k--) ;
            /* a text key: the rest of the line is its value, verbatim */
            txt = (o - k == 4 && b[k] == 'i' && b[k + 1] == 'n' && b[k + 2] == 'f' && b[k + 3] == 'o') ||
                  (o - k == 6 && b[k] == 's' && b[k + 1] == 't' && b[k + 2] == 'a' && b[k + 3] == 't' && b[k + 4] == 'u' && b[k + 5] == 's') ||
                  (o - k == 3 && b[k] == 't' && b[k + 1] == 'x' && b[k + 2] >= '1' && b[k + 2] <= '4');
            b[o++] = '='; i++;
            while (i < n && (b[i] == ' ' || b[i] == '\t')) i++;
            ls = 0;
            continue;
        }
        if (keych(c) && (o == 0 || !keych(b[o - 1]))) {   /* a token that is a key (name, blanks, '='): lower case */
            int j = i, e;
            while (j < n && keych(b[j])) j++;
            for (e = j; e < n && (b[e] == ' ' || b[e] == '\t'); e++) ;
            if (e < n && b[e] == '=') { while (i < j) { b[o++] = (char)lc_c(b[i]); i++; } ls = 0; continue; }
        }
        b[o++] = c; i++; ls = 0;
    }
    b[o] = 0;
    return o;
}
static int grid_ok(const char *g) { return g[0] >= 'A' && g[0] <= 'R' && g[1] >= 'A' && g[1] <= 'R' && g[2] >= '0' && g[2] <= '9' && g[3] >= '0' && g[3] <= '9'; }
#ifdef JS8_TX
static js8tx_t txq;
#endif
static COLD void read_cfg(void)
{
    char b[CFG_SZ];
    int fd = icap_open(F_CFG, ICAP_O_READ), n;
    if (fd >= 0) {
        int r;
        n = 0;
        while (n < (int)sizeof b - 1 && (r = icap_read(fd, b + n, (uint32_t)(sizeof b - 1 - n))) > 0) n += r;
        if (n == (int)sizeof b - 1) {            /* longer than we read: the keys after the limit are NOT used - said after the start */
            char x;
            if (icap_read(fd, &x, 1) > 0) { cfg_long = 1; while (n && b[n - 1] != '\n' && b[n - 1] != '\r') n--; }   /* (the cut line is dropped whole) */
        }
        icap_close(fd);
        n = cfg_norm(b, n);
        for (char *s = b; *s; s++) {             /* keys are matched only at a token start */
            if (s != b && s[-1] != '\n' && s[-1] != '\r' && s[-1] != ' ' && s[-1] != '\t') continue;
            if (kmatch(s, "flo")) { int v = kint(s); if (v >= 0 && v <= F_MAXCFG) cfg_flo = (float)v; }
            else if (kmatch(s, "fhi")) { int v = kint(s); if (v >= 0 && v <= F_MAXCFG) cfg_fhi = (float)v; }
            else if (kmatch(s, "call")) ktok(s, cfg_call, sizeof cfg_call);
            else if (kmatch(s, "groups")) ktok(s, cfg_groups, sizeof cfg_groups);
            else if (kmatch(s, "popup")) { int v = kint(s); if (v >= 0) cfg_popup = v != 0; }
            else if (kmatch(s, "keep")) { int v = kint(s); if (v >= 2 && v <= KEEP_MAX) cfg_keep = v; }
            else if (kmatch(s, "flush")) { int v = kint(s); if (v >= 1 && v <= 40) cfg_flush = v; }
            else if (kmatch(s, "loc")) ktok(s, cfg_loc, sizeof cfg_loc);
            else if (kmatch(s, "info")) s = ktext(s, feat->info, sizeof feat->info);
            else if (kmatch(s, "status")) s = ktext(s, feat->status, sizeof feat->status);
#ifdef JS8_LINK
            else if (js8link_cfg(lnk, s)) { }
#endif
#ifdef JS8_TX
            else if (js8tx_cfg(&txq, s) == 2) { while (s[1] && s[1] != '\n' && s[1] != '\r') s++; }   /* a preset text: skip the line */
#endif
            else for (unsigned i = 0; i < sizeof tkeys / sizeof tkeys[0]; i++)
                if (kmatch(s, tkeys[i].k)) { int v = kint(s); if (v >= 0) *tkeys[i].v = v < tkeys[i].lo ? tkeys[i].lo : v > tkeys[i].hi ? tkeys[i].hi : v; }
        }
    }
    js8_tune.subpasses = 1 + cfg_sub; js8_tune.nsync2 = js8_tune.nsync34;
    if (!js8_tune.swide) js8_tune.foot = js8_tune.nbr = js8_tune.erounds = 0;   /* wide=0: candidate handling of the first build too */
    if (cfg_budget && cfg_budget < 1000) cfg_budget = 1000;
    js8_tune.budget = cfg_budget;                /* the decoder: no further pass that cannot end in time; dec_abort: the cut */
    /* the decoder refuses a band of fewer than 16 bins; a typing error must not silence the receiver */
    if (cfg_fhi - cfg_flo < (float)BAND_MIN) { cfg_band_bad = cfg_flo != 450.0f || cfg_fhi != 2550.0f; cfg_flo = 450.0f; cfg_fhi = 2550.0f; }
    /* MYCALL: call= of JS8.CFG overrides the radio's own ("@MYCALL", empty = not set) */
    mycall[0] = 0;
    fd = icap_open("@MYCALL", ICAP_O_READ);
    if (fd >= 0) {
        n = icap_read(fd, b, 31); icap_close(fd);
        if (n > 0) { b[n] = 0; b[n + 1] = 0; for (int i = n; i >= 0; i--) b[i + 1] = b[i]; b[0] = '='; ktok(b, mycall, sizeof mycall); }
    }
    if (cfg_call[0]) cat(mycall, cfg_call);
    /* the locator: the radio's position ("@MYLOC", 6 characters, empty = no fix), else loc= of JS8.CFG */
    fd = icap_open("@MYLOC", ICAP_O_READ);
    if (fd >= 0) {
        n = icap_read(fd, b + 1, 31); icap_close(fd);
        if (n >= 4) { b[0] = '='; b[n + 1] = 0; ktok(b, mygrid, sizeof mygrid); }
    }
    if (!grid_ok(mygrid)) { mygrid[0] = 0; if (grid_ok(cfg_loc)) cat(mygrid, cfg_loc); }
    if (slen(mygrid) != 6) mygrid[4] = 0;
#ifdef JS8_TX
    txq.grid[0] = 0; js8tx_set_grid(&txq, mygrid);
#endif
}

/* ---- logs: segment files written once, a ring per stream, the state file pair (see js8app_store.h) */
#include "js8app_store.h"
static char last_note[41];
static int n_notes;
static void note_at(int hh, int mm, int ss, const char *s)    /* status / error: app window line 2 + the frame log */
{
    char b[120], *q = b;
    int k = 0; while (s[k] && k < 40) { last_note[k] = clean(s[k]); k++; } last_note[k] = 0;
    if (!feat || !feat->k.view) set_line(2, last_note);
#ifdef JS8_LINK
    link_note(last_note);
#endif
    *q++ = 'N'; *q++ = ' ';
    if (hh >= 0) q = hhmmss(q, hh, mm, ss); else q = cat(q, "------");
    *q++ = ' '; k = 0; while (s[k] && k < 100) *q++ = clean(s[k++]); *q = 0;
    log_add(&lg_frames, b); lg_frames.urgent = 1; n_notes++;   /* written at the end of this period / from the idle loop */
}
static void note(const char *s) { note_at(-1, 0, 0, s); }

/* ---- capture: a state machine driven by pump(), which runs from the main loop AND from inside a decode
 * (copied from the SDK shell, FT8 timing; added: the date of the period for the log) */
static int16_t abuf[800];                        /* 0.1 s */
static struct {
    int fd, slot;                                /* file being written (-1 none), its slot 0/1 */
    uint32_t n, t0;                              /* samples written, tick at start */
    int hh, mm;                                  /* the period this capture belongs to */
    int last_sec, clock_bad, last_key;           /* last_key: the period (of the day) last captured */
    float late;                                  /* start lateness (s) -> subtracted from NOM_START */
    int werr, ovr, gap_ms;                       /* SD write error / overruns / audio padded with zeros (ms) */
    int ss, key;
    uint32_t ymd;                                /* yyyymmdd of the period */
} cap = { -1, 0, 0, 0, 0, 0, -1, 0, -1, 0.0f, 0, 0, 0, 0, 0, 0 };
static struct {
    int ready, hh, mm; uint32_t n; float nom; int werr, gap_ms;
    int ss, key; uint32_t ymd;
} done[2];                                       /* captures waiting */
static uint32_t gaps;
static int open_err, lost;                       /* capture file could not be opened / periods not captured or overwritten */
static int decoding = -1;                        /* slot being decoded, -1 none */
/* Streaming front end (js8dec.h): the waterfall of the first pass is built from the samples as cap_put() writes
 * them, so after the period only search + decoding remain.  The arena is the decoder's while a decode runs, so a
 * stream starts with the capture only if nothing is being decoded or waiting; otherwise it starts when that decode
 * returns, at the sample the capture has reached (the decoder builds the rows before it from the file). */
static void *wk; static uint32_t wk_len;         /* decoder arena */
static int st_key = -1;                          /* period whose rows are in the arena, -1 none */
static float cap_nom(void);
static __attribute__((noinline)) void st_begin(void)
{
    st_key = cfg_stream && js8_stream_begin(wk, wk_len, FS, CAP_SAMP, cfg_flo, cfg_fhi, cap_nom(), cap.n) ? cap.key : -1;
}
static uint32_t cur_ymd;                         /* date at the last clock read */

static void cap_finish(void)
{
    icap_close(cap.fd); cap.fd = -1;
    done[cap.slot].ready = 1; done[cap.slot].hh = cap.hh; done[cap.slot].mm = cap.mm; done[cap.slot].n = cap.n;
    done[cap.slot].ss = cap.ss; done[cap.slot].key = cap.key; done[cap.slot].ymd = cap.ymd;
    done[cap.slot].nom = cap_nom(); done[cap.slot].werr = cap.werr; done[cap.slot].gap_ms = cap.gap_ms;
}
static float cap_nom(void) { return NOM_START - cap.late < NOM_MIN ? NOM_MIN : NOM_START - cap.late; }
static void cap_start(int key, float late)
{
    for (int tries = 0; tries < 2; tries++) {
        int slot = cap.slot ^ 1;
        if (slot == decoding) slot ^= 1;         /* never overwrite the file being decoded */
        if (slot == decoding) return;
        int fd = icap_open(F_CAP[slot], ICAP_O_WRITE);
        if (fd < 0) { open_err = 1; cap.slot = slot; continue; }   /* try the other slot next */
        if (done[slot].ready) lost++;            /* an undecoded period is being replaced */
        done[slot].ready = 0;
        cap.fd = fd; cap.slot = slot; cap.n = 0; cap.t0 = icap_ticks();
        cap.hh = key * PERIOD / 3600; cap.mm = key * PERIOD / 60 % 60; cap.ss = key * PERIOD % 60; cap.late = late;
        cap.key = key; cap.ymd = cur_ymd;
        cap.werr = 0; cap.ovr = 0; cap.gap_ms = 0;
        if (decoding < 0 && !done[0].ready && !done[1].ready) st_begin();
        return;
    }
}
static void cap_put(const int16_t *x, uint32_t k)
{
    if (cap.n + k > CAP_SAMP) k = CAP_SAMP - cap.n;
    if (!k) return;
    if (x) { if (icap_write(cap.fd, x, k * 2) != (int)(k * 2)) { cap.werr = 1; return; } js8_stream_put(x, k); }   /* keep n == file */
    else {                                       /* zeros for lost audio, so later samples keep their time.  The read buffer is
                                                  * free here (pump: the ring is drained), so it is the block of zeros (no 400 B
                                                  * constant: the full app needs the room since 24 Nov 2026) */
        uint32_t left = k, m;
        for (m = 0; m < sizeof abuf / sizeof abuf[0]; m++) abuf[m] = 0;
        while (left) { m = left > sizeof abuf / sizeof abuf[0] ? sizeof abuf / sizeof abuf[0] : left; if (icap_write(cap.fd, abuf, m * 2) != (int)(m * 2)) { cap.werr = 1; return; } left -= m; cap.n += m; js8_stream_put(0, m); }
        return;
    }
    cap.n += k;
}
#ifdef JS8_TX
/* the radio clock has whole seconds only: pump() notes the tick at which it saw a second begin */
static struct { int sec, edge, sod; uint32_t tick; } txc = { -1, 0, 0, 0 };
static int tx_busy;                              /* inside tx_slot(): the capture is frozen, pump() does nothing */
#endif
static void pump_(void)
{
#ifdef JS8_TX
    if (tx_busy) return;
#endif
    for (int guard = 0; guard < 64; guard++) {   /* bounded: never spin on a stuck overrun */
        int r = icap_audio_read(abuf, (int)(sizeof abuf / sizeof abuf[0]));
        if (r == ICAP_E_OVERRUN) { gaps++; continue; }
        if (r <= 0) {
            /* ring drained: the file must hold ~(now - start) of audio.  More than 250 ms missing (overrun,
             * stall, TX) -> fill the hole with zeros so later samples keep their time (DT stays right) */
            if (cap.fd >= 0 && !cap.werr) {
                uint32_t want = (icap_ticks() - cap.t0) * (FS / 1000u);
                if (want > cap.n + 2000u) {
                    uint32_t pad = want - cap.n - 80u;
                    if (pad > CAP_SAMP - cap.n) pad = CAP_SAMP - cap.n;
                    if (pad) { cap_put(0, pad); cap.gap_ms += (int)(pad / (FS / 1000u)); }
                }
            }
            break;
        }
        if (cap.fd >= 0 && !cap.werr) cap_put(abuf, (uint32_t)r);
    }
    /* end of a capture: by samples, or by time (no audio) */
    if (cap.fd >= 0 && (cap.n >= CAP_SAMP || icap_ticks() - cap.t0 > CAP_TMO_MS)) cap_finish();
    struct icap_time t;
    if (icap_clock_now(&t) != 0 || t.year < 2020) { cap.clock_bad = 1; return; }
    cap.clock_bad = 0;
    cur_ymd = (uint32_t)t.year * 10000u + (uint32_t)t.mon * 100u + t.day;
#ifdef JS8_TX
    if (t.sec != txc.sec) { txc.edge = txc.sec >= 0; txc.sec = t.sec; txc.tick = icap_ticks(); txc.sod = (t.hour * 60 + t.min) * 60 + t.sec; }
#endif
    /* start of a capture: the second edge of the period (late: a missed second edge, a stall, an RTC step) */
    int sp = t.sec % PERIOD;
    if (cap.fd < 0 && (sp >= (int)(PERIOD - PREROLL) || sp == 0)) {
        int key = (t.hour * (3600 / PERIOD) + t.min * (60 / PERIOD) + t.sec / PERIOD + (sp ? 1 : 0)) % NKEYS;
        if (key != cap.last_key) {
            float late = 0.0f;
            /* periods whose edge second was never seen (pump() not called for a second: a slow SD call, a long
             * result phase).  A jump of more than 8 periods is a clock change, not lost reception. */
            if (cap.last_key >= 0) { int d = (key - cap.last_key + NKEYS) % NKEYS; if (d >= 2 && d <= 8) lost += d - 1; }
            int ds = (sp - (int)(PERIOD - PREROLL) + PERIOD) % PERIOD;
            if (!(ds == 0 && cap.last_sec == (int)(PERIOD - 1 - PREROLL))) late = (float)ds + 0.5f;
            cap.last_key = key;
            cap_start(key, late);
        }
    }
    cap.last_sec = sp;
}

static uint32_t t_pump, t_sd;                    /* ms in pump() / in the SD reads of the capture (reset per decode) */
static void pump(void)                           /* timed only while a decode runs (JS8PERF.TXT "pm") */
{
    if (decoding < 0) { pump_(); return; }
    uint32_t t = icap_ticks(); pump_(); t_pump += icap_ticks() - t;
}

/* ---- SD-backed sample source for the decoder (no seek in the API: reopen + skip when reading backwards).
 * The capture is read in whole blocks of JS8_RDBLK bytes at block-aligned file positions (8 sectors per call
 * instead of the 1024 + 256 bytes per decoder chunk of the first build: 57 SD calls per pass over a period
 * instead of about 370), into a block at the top of the data region (not in the code window). */
#ifndef JS8_RDBLK
#define JS8_RDBLK 4096u                          /* bytes, a power of two */
#endif
static struct { int fd; uint32_t fpos, len; int slot; uint32_t b0, bn; } rd = { -1, 0, 0, 0, 0, 0 };
static uint8_t *rblk;                            /* file bytes [rd.b0, rd.b0 + rd.bn) */
/* the next block of the file into rblk.  A read may return any byte count: the block is completed with further
 * reads, so samples never lose their byte alignment.  Returns bytes (less than a block = end of file), -1 = error. */
static int rd_fill(void)
{
    uint32_t have = 0;
    int r;
    rd.bn = 0;
    while (have < JS8_RDBLK) {
        /* a card that hands out a few bytes per call must not keep the app from seeing a stop request (the decoder
         * polls only between its own read calls); the decoder then gives up with an I/O error */
        if (have && stop()) r = -1;
        else { uint32_t t = icap_ticks(); r = icap_read(rd.fd, rblk + have, JS8_RDBLK - have); t_sd += icap_ticks() - t; }
        if (r < 0) { rd.fpos = ~0u; return -1; }      /* file position unknown: the next read reopens */
        if (r == 0) break;                       /* end of file (a single odd byte at the end is dropped) */
        have += (uint32_t)r;
    }
    rd.b0 = rd.fpos; rd.bn = have; rd.fpos += have;
    return (int)have;
}
static int src_read(void *ctx, uint32_t start, float *out, uint32_t n)
{
    (void)ctx;
    if (start >= rd.len) return 0;
    if (start + n > rd.len) n = rd.len - start;
    uint32_t got = 0;
    while (got < n) {
        uint32_t bp = (start + got) * 2u;        /* byte position of the next sample */
        if (rd.fd < 0 || bp < rd.b0 || bp + 1u >= rd.b0 + rd.bn) {
            uint32_t want = bp & ~(JS8_RDBLK - 1u);
            if (rd.fd >= 0 && want == rd.b0 && rd.bn) break;         /* the file ends inside this block */
            if (rd.fd < 0 || want < rd.fpos) {
                if (rd.fd >= 0) { icap_close(rd.fd); rd.fd = -1; }
                rd.fd = icap_open(F_CAP[rd.slot], ICAP_O_READ); rd.fpos = 0; rd.bn = 0;
                if (rd.fd < 0) return -1;
            }
            int r = JS8_RDBLK;
            while (rd.fpos <= want && r == (int)JS8_RDBLK) {         /* skip forward block by block */
                int skip = rd.fpos < want;
                r = rd_fill();
                pump();                          /* keep capturing the next period while decoding */
                if (r < 0 || (skip && r < (int)JS8_RDBLK)) { if (skip) return -1; break; }
            }
            if (r < 0 || bp < rd.b0 || bp + 1u >= rd.b0 + rd.bn) break;
        }
        uint32_t k = (rd.b0 + rd.bn - bp) / 2u;
        if (k > n - got) k = n - got;
        const int16_t *x = (const int16_t *)(void *)(rblk + (bp - rd.b0));
        for (uint32_t i = 0; i < k; i++) out[got + i] = (float)x[i] * (1.0f / 32768.0f);
        got += k;
    }
    return (int)got;
}
/* one scratch buffer for two users that never overlap: on_message() builds its message-log line in it, and
 * perf_write() (main loop, after the results of a period) the text of JS8PERF.TXT */
static char iob[40 + JS8ASM_TEXT_MAX];
#ifdef JS8_TX
static void kbd_poll1(int inmain);
#endif
#ifdef JS8_TX
static void kbd_resync(void);
static void kbd_cont(const char *line, int first);
#endif
#ifdef JS8_TX
#include "js8app_tx.h"                           /* transmit glue: txio, tx_slot(), tx_service() */
#endif

static uint32_t last_yield, dec_t0;              /* dec_t0: tick at the start of the decode in work */
static int timed_out, bud_cut;
static void polite(void)
{
    pump();
    if (icap_ticks() - last_yield > 100) { icap_yield(); last_yield = icap_ticks(); }   /* stay polite */
}
static int dec_abort(void)
{
    polite();
    if (stop()) return 1;
    if (decoding >= 0 && done[decoding ^ 1].ready) { timed_out = 1; return 1; }   /* next period complete */
    if (cfg_budget && icap_ticks() - dec_t0 >= (uint32_t)cfg_budget) { bud_cut = 1; return 1; }   /* JS8.CFG budget= */
#ifdef JS8_TX
    if (txpoll() == 0) { tx_cut = 1; return 1; }   /* a confirmed transmission is due: leave the decoder */
    kbd_poll1(0);                                /* a tap on a row: the radio's keyboard (no file read of JS8MEM.TXT in here) */
#endif
#ifdef JS8_LINK
    link_service();
#endif
    return 0;
}

/* fresh capture / decode state at start.  A capture in progress is dropped. */
static void dec_reset(void)
{
    if (cap.fd >= 0) { icap_close(cap.fd); cap.fd = -1; }
    if (rd.fd >= 0) { icap_close(rd.fd); rd.fd = -1; }
    cap.last_key = -1; cap.last_sec = -1; cap.werr = 0; cap.ovr = 0; cap.gap_ms = 0;
    rd.fd = -1; done[0].ready = done[1].ready = 0; decoding = -1; lost = 0; open_err = 0;
}

/* ---- dictionary files (one bucket file open per lookup; audio keeps being pumped) */
/* Where the dictionary is (loader limit seen on the radio 23 Nov 2026: .APP files whose directory entries come after the
 * ~257 dictionary files in IC-705/Apps are not listed): the SD card ROOT first (ICAP_O_ROOT: JSCHEAD.BIN, JSCD*.BIN, JSCE*.BIN,
 * JSCSUM.BIN), else the app folder as before.  Decided once at start by JSCHEAD.BIN; every file is still checked against
 * JSCSUM.BIN of the same place. */
static int jsc_where;                            /* 0 = the app folder, ICAP_O_ROOT = the card root */
static int jio_open(void *u, const char *name) { (void)u; return icap_open(name, ICAP_O_READ | jsc_where); }
/* a stop request ends a lookup at once (the read fails, the frame in work is dropped by on_frame) */
static int jio_read(void *u, int fd, void *buf, uint32_t len) { (void)u; if (stop()) return -1; int r = icap_read(fd, buf, len); polite(); return r; }
static int jio_close(void *u, int fd) { (void)u; return icap_close(fd); }
static const js8_io_t jio = { jio_open, jio_read, jio_close, 0 };

/* ---- result sink: DECODE screen, logs, popup */
static js8asm_t *asmb;
static js8jsc_t *jsc;                            /* 0 = no dictionary */
static uint32_t n_periods, n_frames, n_msgs, n_tome, n_popbusy, n_popskip, n_skipped, n_raw;
static uint32_t pop_period = 0xFFFFFFFFu;        /* n_periods when the last popup was raised */
static struct { int hh, mm, ss; uint32_t ymd; } now_p;      /* the period being reported */
static char pend[104];                /* frame text waiting for its DECODE line */
static int pend_snr, pend_freq, pend_on;

/* "HHMM sNN FFFF [m ]TEXT", continuation lines indented by CONT_W; every line <= DEC_W characters.  On the screen
 * only, the word HEARTBEAT is shown as HB (the logs keep the full text).  A line that is full is broken at a blank in
 * its second half if there is one. */
static void dec_lines(int snr, int freq, char mark, const char *text)
{
    static const char hbw[] = "HEARTBEAT";
    char line[DEC_W + 1], *q;
    int n = slen(text), pos = 0, first = 1;
    while (n > 0 && text[n - 1] == ' ') n--;
    do {
        q = line;
        if (first) {
            q = num(q, now_p.hh, 2, '0'); q = num(q, now_p.mm, 2, '0'); *q++ = ' '; q = snr3(q, snr); *q++ = ' ';
            q = num(q, freq > 9999 ? 9999 : freq < 0 ? 0 : freq, 4, ' '); *q++ = ' ';
            if (mark) { *q++ = mark; *q++ = ' '; }
        } else for (int i = 0; i < CONT_W; i++) *q++ = ' ';
        const int room = DEC_W - (int)(q - line);
        int w = 0, bpos = -1; char *bq = 0;
        while (pos < n) {
            int hb = 0;
            while (hb < 9 && pos + hb < n && text[pos + hb] == hbw[hb]) hb++;
            hb = hb == 9;
            if (w + (hb ? 2 : 1) > room) {       /* full: back to the last blank of the second half, unless a blank follows */
                if (text[pos] != ' ' && bq) { q = bq; pos = bpos; }
                break;
            }
            if (text[pos] == ' ' && w > room / 2) { bq = q; bpos = pos; }
            if (hb) { *q++ = 'H'; *q++ = 'B'; pos += 9; w += 2; }
            else { *q++ = clean(text[pos++]); w++; }
        }
        *q = 0;
        while (pos < n && text[pos] == ' ') pos++;   /* a continuation line does not start with the blank */
        icap_decode_out(line);
#ifdef JS8_TX
        kbd_cont(line, first);                   /* a tap on a continuation row takes this call (U6) */
#endif
        if (first) js8f_band(feat, line);        /* the BAND view of the app window */
        first = 0;
    } while (pos < n);
}
static void pend_flush(void) { if (pend_on) { pend_on = 0; dec_lines(pend_snr, pend_freq, 0, pend); } }

static char *stamp(char *q) { q = num(q, (int)now_p.ymd, 8, '0'); *q++ = 'T'; return hhmmss(q, now_p.hh, now_p.mm, now_p.ss); }
static uint32_t p_abs(void) { return js8f_time(now_p.ymd, (now_p.hh * 60 + now_p.mm) * 60 + now_p.ss); }   /* the period being reported */
static uint32_t t_now(void)                      /* the radio clock as absolute seconds (ages in the views) */
{
    struct icap_time t;
    if (icap_clock_now(&t) != 0 || t.year < 2020) return p_abs();
    return js8f_time((uint32_t)t.year * 10000u + (uint32_t)t.mon * 100u + t.day, (t.hour * 60 + t.min) * 60 + t.sec);
}
static int leaving;                              /* the app is leaving: messages still open are logged ('e'), nothing is shown */
static uint32_t pop_tok = POP_BURST, pop_refill; /* popup budget */

static void on_message(void *user, const js8asm_msg_t *m)   /* js8asm: one complete message */
{
    char *const b = iob;
    char *q = b;
    int tome = (m->flags & JS8ASM_F_TO_ME) != 0;
    (void)user;
    n_msgs++;
    /* DECODE screen.  A message closed by timeout / a new message on its frequency comes BEFORE the frame that
     * closed it; a message completed by the current frame comes after that frame's line. */
    if (leaving) { }
    else if (m->flags & JS8ASM_F_TIMEOUT) {      /* (a one-frame message was already shown as its frame line) */
        if (m->frames > 1 || tome) dec_lines(m->snr, m->freq, tome ? '>' : (m->flags & JS8ASM_F_CRC_BAD) ? '?' : '=', m->text);
    }
    else if (m->frames == 1 && pend_on) {        /* the frame is the message: one line */
        if (tome) { pend_on = 0; dec_lines(m->snr, m->freq, '>', m->text); }
        else pend_flush();
    } else { pend_flush(); dec_lines(m->snr, m->freq, tome ? '>' : (m->flags & JS8ASM_F_CRC_BAD) ? '?' : '=', m->text); }
    /* message log */
    *q++ = 'M'; *q++ = ' '; q = stamp(q); *q++ = ' '; q = snr3(q, m->snr); *q++ = ' '; q = num(q, m->freq, 0, ' '); *q++ = ' ';
    q = num(q, m->frames, 0, ' '); *q++ = ' ';
    {
        char *f0 = q;
        if (tome) *q++ = '>';
        if (m->flags & JS8ASM_F_TO_GROUP) *q++ = 'g';
        if (m->flags & JS8ASM_F_MENTION) *q++ = 'm';
        if (m->flags & JS8ASM_F_CRC_OK) *q++ = 'c';
        if (m->flags & JS8ASM_F_CRC_BAD) *q++ = 'x';
        if (m->flags & JS8ASM_F_TIMEOUT) *q++ = 't';
        if (m->flags & JS8ASM_F_PARTIAL) *q++ = 'p';
        if (m->flags & JS8ASM_F_TRUNC) *q++ = 'l';
        if (leaving) *q++ = 'e';
        if (q == f0) *q++ = '-';
    }
    *q++ = ' '; cat_clean(q, m->text);
    log_add(&lg_msgs, b);
    {   /* inbox; a query to MYCALL: the answer is PREPARED as a "TX? ..." row - sending it needs the operator's taps */
        int fr = js8f_message(feat, m, mygrid, p_abs());
        if (fr) snap_dirty = 1;
        if (fr == 1 || fr == 2) lg_msgs.urgent = 1;
#ifdef JS8_LINK
        link_msg(fr != 0, b);
#endif
#ifdef JS8_TX
        if (fr == 2 && !leaving && txq.enable && !txq.dead && !txq.test) {   /* a tap on it (or on the query) opens the keyboard with it */
            char l[8 + JS8F_REPLY];
            cat(cat(l, "TX? "), js8f_last_reply(feat));     /* (4 + at most 34 characters: one row) */
            icap_decode_out(l);
        }
#endif
    }
    if (tome) n_tome++;
    if (tome && !leaving) {                      /* raise it once: up to 7 lines of up to 31 characters */
        char p[7 * 32 + 1], *w = p;
        int n = slen(m->text), pos = 0, lines = 1;
        w = cat(w, "JS8 message for "); w = cat(w, asmb->mycall);
        while (pos < n && lines < 7) {
            int k = n - pos;
            if (k > 31) { k = 31; for (int j = 31; j > 15; j--) if (m->text[pos + j] == ' ') { k = j; break; } }
            *w++ = '\n';
            for (int i = 0; i < k; i++) *w++ = clean(m->text[pos + i]);
            pos += k; lines++;
            while (pos < n && m->text[pos] == ' ') pos++;
        }
        *w = 0;
        /* the popup is modal and any station can address us: at most one per period, at most POP_BURST in a row and
         * then one per POP_REFILL periods, and none for a message whose checksum is wrong or that is incomplete
         * (closed by timeout, beginning missing, cut).  All of them are still marked '>' on the DECODE screen and in
         * the message log, and are in the inbox. */
        if (!cfg_popup) { }
        else if ((m->flags & (JS8ASM_F_CRC_BAD | JS8ASM_F_TIMEOUT | JS8ASM_F_PARTIAL | JS8ASM_F_TRUNC)) || pop_period == n_periods || !pop_tok) n_popskip++;
        else { pop_period = n_periods; pop_tok--; if (icap_popup(p) != 0) n_popbusy++; }
    }
}

static void put_hex(char **q, const uint8_t *f)
{
    static const char hx[] = "0123456789abcdef";
    for (int j = 0; j < JS8_FRAME_BYTES; j++) { *(*q)++ = hx[f[j] >> 4]; *(*q)++ = hx[f[j] & 15]; }
}
static void on_frame(const js8_result_t *r, uint32_t now_s)
{
    static char b[64 + JS8_TEXT_MAX];
    js8_msg_t m;
    char *q = b;
    int snr = (int)(r->snr_db + (r->snr_db < 0 ? -0.5f : 0.5f)), freq = (int)(r->freq_hz + 0.5f);
    int dt = (int)(r->dt_s * 100.0f + (r->dt_s < 0 ? -0.5f : 0.5f)), f10 = (int)(r->freq_hz * 10.0f + 0.5f);

    js8_msg_decode(r->frame, jsc, &m);
    if (quit) return;                            /* stop came during a dictionary lookup: the text is not complete */
    n_frames++;
    js8f_frame(feat, &m, snr, freq, p_abs());    /* heard table */
    /* frame log: R <utc> <snr> <dt> <freq> <hex22> <text> */
    *q++ = 'R'; *q++ = ' '; q = stamp(q); *q++ = ' '; q = snr3(q, snr); *q++ = ' ';
    if (dt > 999) dt = 999; else if (dt < -999) dt = -999;
    *q++ = dt < 0 ? '-' : '+'; if (dt < 0) dt = -dt;
    q = num(q, dt / 100, 0, ' '); *q++ = '.'; q = num(q, dt % 100, 2, '0'); *q++ = ' ';
    q = num(q, f10 / 10, 0, ' '); *q++ = '.'; *q++ = (char)('0' + f10 % 10); *q++ = ' ';
    put_hex(&q, r->frame); *q++ = ' '; cat_clean(q, m.text);
    log_add(&lg_frames, b);
    /* DECODE line text: what JS8Call shows for the frame; a compressed frame whose words could not be read
     * (no dictionary on the card) and an unknown frame are shown raw as '#' + the 12 frame characters */
    if ((m.flags & JS8_MF_NODICT) || m.type == JS8_FT_UNKNOWN) {
        pend[0] = '#'; js8_frame_chars(r->frame, pend + 1); n_raw++;
    } else {
        int k = 0; while (m.text[k] && k < (int)sizeof pend - 1) { pend[k] = clean(m.text[k]); k++; } pend[k] = 0;
    }
    pend_snr = snr; pend_freq = freq; pend_on = 1;
#ifdef JS8_TX
    js8tx_heard(&txq, freq);                     /* the automatic TX offset keeps away from what was just decoded */
#endif
    js8asm_feed(asmb, &m, freq, snr, now_s);     /* may call on_message() */
    pend_flush();
}

/* ---- status view, per-stage timing, JS8PERF.TXT */
static uint32_t dec_ms_last, dec_ms_max, dec_ms_sum, arena_peak, arena_len;
static int last_nd;
static unsigned dict_bad;                        /* corrupt dictionary files already reported */
static int notes_seen;                           /* n_notes when the STATUS view was last drawn */
#ifdef DIGI_HOST
static char *spare_p; static uint32_t spare_n;   /* the unused end of the shell's part of the data region */
#endif
enum { V_TOT, V_RD, V_FFT, V_SRCH, V_DEC, V_SUB, V_POST, V_TRIED, V_NBP, PV_N };   /* per period: panel + "P" line */
static const char pvn[PV_N + 1] = "Trfsduptb";
typedef struct { uint16_t key; uint8_t nd, fl; uint16_t v[PV_N + 2]; uint8_t wf, sig; } prec_t;   /* v[PV_N] = poll ms, v[PV_N + 1] = BP iterations */
typedef char stats_are_words[sizeof(js8_stats_t) == 4 * JS8_NSTAT ? 1 : -1];
static prec_t pring[PERF_N];                     /* the last PERF_N periods */
static uint16_t pv_max[PV_N];
static uint32_t pr_n, n_cut;                     /* periods recorded / cut short */
#define NSUM (JS8_NSTAT + 5)
#if JS8_PERF_FILE
static uint32_t psum[NSUM];                      /* sums since the start: js8_stats word by word, then T pt sd pm pw */
static uint32_t perf_wr_ms;                      /* ms the last JS8PERF.TXT write took */
static const char psn[2 * NSUM + 1] = "catrbpitrdsuffmgsrllb1b2b3b4b5fipok0k1k2rcscsbi1i2i3i4i5r1r2r3r4r5a1a2a3a4a5nflvplsgw0w1w2nbT ptsdpmpw";
#endif
static uint32_t app_clock(void) { return icap_ticks(); }

static COLD void perf_add(int key, int nd, int fl, uint32_t ms, uint32_t post, uint32_t sd, uint32_t pm)
{
    const js8_stats_t *st = &js8_stats;
    uint32_t v[PV_N + 2], dec = 0;
#if JS8_PERF_FILE
    uint32_t *x = psum + JS8_NSTAT;
    for (int i = 0; i < JS8_NSTAT; i++) psum[i] += ((const uint32_t *)st)[i];
    x[0] += ms; x[1] += post; x[2] += sd; x[3] += pm; x[4] += perf_wr_ms;
#else
    (void)sd; (void)pm;
#endif
    for (int k = JS8_T_LLR; k <= JS8_T_FIN; k++) dec += st->t[k];
    prec_t *r = &pring[pr_n++ % PERF_N];
    r->key = (uint16_t)key; r->nd = (uint8_t)(nd > 0 ? nd : 0); r->fl = (uint8_t)fl;
    v[V_TOT] = ms; v[V_RD] = st->t[JS8_T_READ]; v[V_FFT] = st->t[JS8_T_FFT] + st->t[JS8_T_MAG]; v[V_SRCH] = st->t[JS8_T_SEARCH];
    v[V_DEC] = dec; v[V_SUB] = st->t[JS8_T_SUB]; v[V_POST] = post; v[V_TRIED] = st->ntried; v[V_NBP] = st->nbp;
    v[PV_N] = st->t[JS8_T_POLL]; v[PV_N + 1] = st->bp_iters;
    for (int i = 0; i < PV_N + 2; i++) {         /* v[], poll, it are consecutive uint16_t in prec_t; saturate */
        uint16_t u = v[i] > 65535u ? 65535u : (uint16_t)v[i];
        r->v[i] = u;
        if (i < PV_N && u > pv_max[i]) pv_max[i] = u;
    }
    r->wf = (uint8_t)(((st->nwf[0] + st->nwf[1]) << 4) | (st->nwf[2] & 15)); r->sig = (uint8_t)st->nsig;
    if (fl) n_cut++;
}
static COLD char *kv(char *q, const char *k, int v) { q = cat(q, k); return num(q, v, 0, ' '); }
static COLD char *head_line(char *q)                  /* panel line 0 */
{
#ifdef JS8_TX
    q = cat(q, MODE_NAME " RX+TX ");
#else
    q = cat(q, MODE_NAME " RX ");
#endif
    q = cat(q, mycall[0] ? mycall : "(no call)");
    q = kv(q, " ", (int)cfg_flo); q = kv(q, "-", (int)cfg_fhi); q = cat(q, " Hz");
#ifdef JS8_BUILD
    q = cat(cat(q, " "), JS8_BUILD);              /* the build (git commit) - visible on every photo of the window */
#endif
    return q;
}
static COLD __attribute__((unused)) char *tune_line(char *q)                  /* panel line 1: the decoder tuning in use */
{
    q = kv(q, "ms", js8_tune.min_score); q = kv(q, " n", js8_tune.nsync_min); q = kv(q, "/", js8_tune.nsync34);
    q = kv(q, " p", js8_tune.passmask); q = kv(q, " it", js8_tune.iters); q = kv(q, " su", js8_tune.subpasses - 1);
    q = kv(q, " sh", js8_tune.shift); q = kv(q, " w", js8_tune.swide); q = kv(q, " s", cfg_stream);
    return kv(q, " b", cfg_budget);
}
static COLD void panel(void)
{
    char l[128], *q;
    const prec_t *r = &pring[(pr_n ? pr_n - 1 : 0) % PERF_N];
    if (feat->k.view) {                          /* BAND / INBOX / HEARD take the whole window */
        uint32_t now = t_now();
        for (int i = 0; i < 8; i++) {
            js8f_row(feat, feat->k.view, i, now, l);
            /* a note or an error came that the STATUS view has not shown yet: mark the title */
            if (!i && (n_notes != notes_seen || log_err) && slen(l) <= WIN_W - 2) cat(l + slen(l), " !");
            set_line(i, l);
        }
        return;
    }
    notes_seen = n_notes;
    head_line(l); set_line(0, l);
#ifndef JS8_TX
    tune_line(l); set_line(1, l);
#endif                                           /* TX flavour: line 1 is the transmit status (tx_panel) */
    set_line(2, last_note[0] ? last_note : "no errors");
    q = cat(l, "last "); q = hhmmss(q, now_p.hh, now_p.mm, now_p.ss); q = kv(q, " ", last_nd); q = kv(q, "d ", (int)dec_ms_last);
    q = kv(q, "ms t", r->v[V_TRIED]); q = kv(q, " b", r->v[V_NBP]); cat(q, r->fl & 1 ? " CUT" : r->fl & 2 ? " BUD" : r->fl & 4 ? " TX" : "");
    set_line(3, l);
    for (int up = 0; up < 2; up++) {             /* line 4: last period, line 5: maxima (capital letters) */
        const uint16_t *v = up ? pv_max : r->v;
        q = l;
        for (int i = V_RD; i <= V_POST; i++) { if (i > V_RD) *q++ = ' '; *q++ = (char)(pvn[i] - (up ? 32 : 0)); q = num(q, v[i], 0, ' '); }
        set_line(4 + up, l);
    }
    if (jsc) { q = kv(l, "dict ", (int)jsc->head_n); if (jsc->st_bad) q = kv(q, " BAD ", jsc->st_bad); q = kv(q, " rd ", (int)jsc->st_opens); }
    else q = kv(l, "no dictionary, raw # ", (int)n_raw);
    q = kv(q, " fr ", (int)n_frames); q = kv(q, " msg ", (int)n_msgs); q = kv(q, " me ", (int)n_tome);
    if (feat->k.unread) kv(q, " new ", feat->k.unread);
    set_line(6, l);
    q = cat(l, log_err ? "LOGERR " : "");      /* first: the flag must stay visible */
    q = kv(q, "c", (int)n_cut); q = kv(q, "/", (int)n_periods); q = kv(q, " k", (int)n_skipped); q = kv(q, " g", (int)gaps);
    q = kv(q, " av", n_periods ? (int)(dec_ms_sum / n_periods) : 0); q = kv(q, " mx", (int)dec_ms_max);
    q = kv(q, " T", pv_max[V_TRIED]); kv(q, " B", pv_max[V_NBP]);
    set_line(7, l);
}


#ifdef JS8_TX
static char txp_last[WIN_W + 8];
static void tx_panel(void)                       /* app window line 1 of the STATUS view, whenever it changes */
{
    char l[WIN_W + 24];
    int i;
    if (feat->k.view) return;
    js8tx_status(&txq, l);
    for (i = 0; l[i] && l[i] == txp_last[i]; i++) ;
    if (l[i] || txp_last[i]) { for (i = 0; l[i] && i < WIN_W; i++) txp_last[i] = l[i]; txp_last[i] = 0; set_line(1, l); }
}
#endif

#if JS8_PERF_FILE
/* JS8PERF.TXT: rewritten after every period (the file API has no append): header, sums, maxima, the last
 * PERF_N periods.  About 2 KB in 4..6 writes; the capture keeps being pumped; a failure is ignored. */
static int pf_fd;
static COLD char *pf_chk(char *q)                     /* write the buffer out when the next piece (<= 44 bytes) may not fit */
{
    if (q - iob > (int)sizeof iob - 48) { icap_write(pf_fd, iob, (uint32_t)(q - iob)); pump(); q = iob; }
    return q;
}
static COLD char *pf_kv(char *q, char a, char b, uint32_t v) { *q++ = ' '; *q++ = a; if (b != ' ') *q++ = b; return pf_chk(num(q, (int)v, 0, ' ')); }
static COLD char *pf_eol(char *q) { *q++ = '\r'; *q++ = '\n'; return pf_chk(q); }
static COLD void perf_write(void)
{
    uint32_t t = icap_ticks();
    char *q = iob;
    pf_fd = icap_open(F_PERF, ICAP_O_WRITE);
    if (pf_fd < 0) return;
    q = pf_chk(head_line(cat(q, MODE_NAME "PERF 1 "))); q = pf_chk(tune_line(cat(q, " | ")));
    q = kv(q, " arena ", (int)arena_peak); q = pf_eol(kv(q, "/", (int)arena_len));
    *q++ = 'S'; q = pf_kv(q, 'n', ' ', pr_n); q = pf_kv(q, 'c', ' ', n_cut);
    for (int i = 0; i < NSUM; i++) q = pf_kv(q, psn[2 * i], psn[2 * i + 1], psum[i]);
    q = pf_eol(q);
    *q++ = 'X';
    for (int i = 0; i < PV_N; i++) q = pf_kv(q, pvn[i], ' ', pv_max[i]);
    q = pf_eol(q);
    for (uint32_t k = pr_n > PERF_N ? pr_n - PERF_N : 0; k < pr_n; k++) {
        const prec_t *r = &pring[k % PERF_N];
        int sec = r->key * PERIOD;
        *q++ = 'P'; *q++ = ' '; q = hhmmss(q, sec / 3600, sec / 60 % 60, sec % 60);
        q = pf_kv(q, 'd', ' ', r->nd); *q++ = ' '; *q++ = r->fl & 1 ? 'D' : r->fl & 2 ? 'B' : r->fl & 4 ? 'T' : '-';
        for (int i = 0; i < PV_N; i++) q = pf_kv(q, pvn[i], ' ', r->v[i]);
        q = pf_kv(q, 'o', ' ', r->v[PV_N]); q = pf_kv(q, 'i', ' ', r->v[PV_N + 1]); q = pf_kv(q, 'w', ' ', r->wf >> 4);
        q = kv(q, "/", r->wf & 15); q = pf_kv(q, 'g', ' ', r->sig);
        q = pf_eol(q);
    }
    if (q != iob) icap_write(pf_fd, iob, (uint32_t)(q - iob));
    icap_close(pf_fd);
    pump();
    perf_wr_ms = icap_ticks() - t;
}
#endif

#if defined JS8_LINK || defined JS8_HOST_CMD          /* (only the terminal and the test bench give commands) */
/* ---- operator commands.  They come from the
 * terminal; none of them transmits.
 *   VIEW [STATUS|BAND|INBOX|HEARD]   show that view of the app window (no name: the next one); the rows of the
 *                                    INBOX / HEARD view are also printed on the DECODE screen; showing the INBOX
 *                                    marks its messages as read
 *   LOG <CALL>                       the exchange with CALL is complete: one ADIF record (the loader's ICAPQSO.ADI
 *                                    through "@QLOG"; where the loader has none, a JS8Annnn.ADI segment) */
static COLD void ui_cmd(const char *c)
{
    char l[8 + JS8F_ADIF_MAX + 4], *q;
    if (c[0] == 'V') {
        int v = (feat->k.view + 1) % JS8F_NVIEWS;
        while (*c > ' ') c++;
        if (*c == ' ') for (int i = 0; i < JS8F_NVIEWS; i++) if (js8f_view_name(i)[0] == c[1]) v = i;
        feat->k.view = (uint8_t)v; snap_dirty = 1;
#ifdef JS8_TX
        txp_last[0] = 0;
#endif
        panel();
        if (v >= JS8F_V_INBOX) {
            uint32_t now = t_now();
            for (int r = 0; r < 1 + JS8F_HEARD && js8f_row(feat, v, r, now, l); r++) if (l[0]) icap_decode_out(l);
            if (v == JS8F_V_INBOX) js8f_read(feat);
        }
    } else if (c[0] == 'L') {
        struct icap_time t;
        struct icap_radio rs;
        int n, fd, r = -1;
        while (*c > ' ') c++;
        while (*c == ' ') c++;
        rs.freq_hz = 0; icap_radio_state(&rs);
        if (icap_clock_now(&t) != 0) { t.year = 0; t.mon = t.day = t.hour = t.min = t.sec = 0; }
        q = cat(l, "@QLOG ");
        n = js8f_adif(feat, c, (uint32_t)t.year * 10000u + (uint32_t)t.mon * 100u + t.day, (t.hour * 60 + t.min) * 60 + t.sec, rs.freq_hz, mygrid, q);
        if (!n) { icap_decode_out("LOG: not a call"); return; }
        fd = icap_open(l, ICAP_O_READ);          /* opening it IS the request; the answer is "r=0" */
        if (fd >= 0) { char a[8]; r = icap_read(fd, a, sizeof a); icap_close(fd); r = (r >= 3 && a[2] == '0') ? 0 : -1; }
        if (r) {
            int w, k = 0;
            q[n] = '\r'; q[n + 1] = '\n';
            do w = seg_write(&lg_adif, q, (uint32_t)n + 2u, 1); while (w < 0 && ++k < 8 && !stop());   /* (-1: still looking for a free number) */
            r = w > 0 ? 0 : -1;
        }
        q = cat(l, r ? "LOG FAILED " : "LOGGED "); cat(q, c);   /* (the record is no longer needed) */
        l[DEC_W] = 0;
        icap_decode_out(l);
    }
}
#endif
#ifdef JS8_TX
#include "js8app_kbd.h"                          /* a tap on a row opens the radio's keyboard; ENT sends */
#endif
#ifdef JS8_LINK
#include "js8app_link.h"                         /* terminal link glue: what the radio says, what it does for a command */
#endif

#ifdef JS8_SHELL_THUMB
/* The shell is built as Thumb code (shell/build.sh, the RX+TX+link flavour).  The SDK's start stub enters with an
 * ARM "b app_main", and an ARM jump into Thumb code would need a linker veneer whose absolute address the loader
 * does not relocate - so app_main stays a small ARM function that CALLS the Thumb main ("blx", PC-relative). */
static int js8_main(const struct icap_svc *svc);
__attribute__((target("arm"), optimize("no-optimize-sibling-calls"))) int app_main(const struct icap_svc *svc)
{
    volatile int r = js8_main(svc);              /* (volatile: keeps the call from becoming a jump) */
    return r;
}
static __attribute__((noinline)) int js8_main(const struct icap_svc *svc)
#else
int app_main(const struct icap_svc *svc)
#endif
{
    icap_use(svc);
    static js8_result_t res[MAXOUT];
    char line[100], *q;
    char *hp = icap_heap(); uint32_t hlen = icap_heap_size();
    /* the shell's part of the data region is a fixed JS8_RESERVE bytes (the pieces need a little less: the rest is
     * spare), so that the decoder arena is the same on the radio and in the simulator */
    typedef char reserve_fits[(((sizeof(js8asm_t) + 7u) & ~7u) + ((sizeof(js8jsc_t) + 7u) & ~7u) + JS8_HEAD_MAX + JS8_LOG_SZ + JS8_MSG_SZ + JS8_RDBLK
                               + ((sizeof(js8feat_t) + 7u) & ~7u) <= JS8_RESERVE) ? 1 : -1];
    typedef char reply_fits[JS8F_REPLY_ROW <= DEC_W - 6 ? 1 : -1];
#ifdef JS8_LINK
    /* the link's state lives in the spare end of the shell's part of the data region: the decoder arena stays as it is */
    typedef char link_fits[(((sizeof(js8asm_t) + 7u) & ~7u) + ((sizeof(js8jsc_t) + 7u) & ~7u) + JS8_HEAD_MAX + JS8_LOG_SZ + JS8_MSG_SZ + JS8_RDBLK
                            + ((sizeof(js8feat_t) + 7u) & ~7u) + ((sizeof(js8link_t) + 7u) & ~7u) <= JS8_RESERVE) ? 1 : -1];
#endif
    const uint32_t reserve = JS8_RESERVE;
    int ok = icap_audio_rate() == (int)FS && hp && !((uintptr_t)hp & 7u) && hlen >= reserve + JS8_WORK_MIN;
    int clock_warned = 0, last_key = -1, last_err = 0;
    uint32_t mono = 0;                           /* seconds, for the reassembly timeouts */

    if (!ok) {
        set_line(2, MODE_NAME ": needs 8 kHz audio + app region");
        while (!stop()) icap_sleep_ms(200);
        return 1;
    }
    js8_clock = app_clock;                       /* per-stage decode times in ms (js8_stats.t) */
    /* carve the data region (zeroed by the loader at launch) */
    asmb = (js8asm_t *)hp; hp += (sizeof(js8asm_t) + 7u) & ~7u;
    js8jsc_t *jc = (js8jsc_t *)hp; hp += (sizeof(js8jsc_t) + 7u) & ~7u;
    uint8_t *head = (uint8_t *)hp; hp += JS8_HEAD_MAX;
    lg_frames.buf = hp; lg_frames.cap = JS8_LOG_SZ - SEG_HDR - SEG_TRL; hp += JS8_LOG_SZ;   /* file = header + lines + trailer <= JS8_LOG_SZ */
    lg_msgs.buf = hp; lg_msgs.cap = JS8_MSG_SZ - SEG_HDR - SEG_TRL; hp += JS8_MSG_SZ;
    feat = (js8feat_t *)hp; hp += (sizeof(js8feat_t) + 7u) & ~7u;
    rblk = (uint8_t *)hp; hp += JS8_RDBLK;       /* the read block of the capture (src_read) */
#ifdef JS8_LINK
    js8link_t *lk = (js8link_t *)hp; hp += (sizeof(js8link_t) + 7u) & ~7u;
    (void)sizeof(link_fits);
    js8link_init(lk, &lio, lk_ident); lnk = lk;  /* before read_cfg: it takes term= / term2= */
#endif
#ifdef DIGI_HOST
    spare_p = hp; spare_n = (uint32_t)((char *)icap_heap() + reserve - hp);   /* the test bench checks that nothing writes here */
#endif
    void *work = wk = (char *)icap_heap() + reserve; uint32_t wlen = wk_len = hlen - reserve;
    arena_len = wlen;
    (void)sizeof(reserve_fits); (void)sizeof(reply_fits);
#ifdef JS8_TX
    js8tx_init(&txq, &txio, mycall, 0, abuf);    /* before read_cfg: it takes the tx* keys */
    if (!tx_available()) txq.dead = 1;
    txq.prep = feat->prep; txq.prep_n = JS8F_PREP;
    { typedef char prep_width[JS8F_REPLY + 1 == JS8TX_PREP_W ? 1 : -1]; (void)sizeof(prep_width); }   /* prepared replies: offered on the keyboard, sent only on the owner's ENT */
#endif
    read_cfg();
#ifdef JS8_LINK
    {
        char *iq = cat(lk_ident, mycall[0] ? mycall : "-"); *iq++ = ' ';
        iq = cat(iq, mygrid[0] ? mygrid : "-");
#ifdef JS8_TX
        cat(iq, " TX");
#else
        cat(iq, " RX");
#endif
    }
#endif

    js8asm_init(asmb, mycall, cfg_groups[0] ? cfg_groups : 0, on_message, 0);
    /* the start-up loads read up to 34 KB (dictionary head) + 9 KB (log tails) from the card: every read polls
     * the stop request, and a stop during start-up leaves without touching any file */
    if (stop()) return 0;
    { int fd = icap_open("JSCHEAD.BIN", ICAP_O_READ | ICAP_O_ROOT); if (fd >= 0) { icap_close(fd); jsc_where = ICAP_O_ROOT; } }
    int jrc = js8jsc_init(jc, &jio, head, JS8_HEAD_MAX);
    jsc = jrc == 0 ? jc : 0;
    if (stop()) return 0;
    snap_load(lg_frames.buf, JS8_LOG_SZ);        /* ring positions, heard table, inbox of the last run (the buffer is free now) */
    if (stop()) return 0;
    js8f_init(feat, mycall);
    feat->k.view = JS8F_V_STATUS;                /* always start on the STATUS view: that is where start-up errors show */
#ifdef JS8_TX
    txq.jsc = jsc;                               /* JSC compression for TX when the dictionary is on the card */
#endif
    icap_clear();
    panel();
    if (jrc == -5) note(MODE_NAME ": dictionary CORRUPT - not used");
    else if (!jsc) note(MODE_NAME ": no dictionary (JSCHEAD.BIN)");
    else if (!jsc->sums) note(MODE_NAME ": dictionary not checked (JSCSUM.BIN)");
    if (jsc_where) note(MODE_NAME ": dictionary on the card root");
    if (!mycall[0]) note(MODE_NAME ": no call (@MYCALL / call=)");
    if (cfg_band_bad) note(MODE_NAME ": JS8.CFG band < 100 Hz, using 450-2550");
    if (cfg_long) {                              /* keys after the read limit are not used: TX must not be more open than meant */
#ifdef JS8_TX
        txq.test = 1;
        note("JS8.CFG: too long, rest ignored, TXTEST on");
#else
        note("JS8.CFG: too long, rest ignored");
#endif
    }
#ifdef JS8_TX
    kbd_start();                                 /* one row to tap */
#endif
    icap_audio_read(abuf, 1);                    /* start the capture service */
    dec_reset();

    while (!stop()) {
        pump();
        if (open_err == 1) { note(MODE_NAME ": SD error - no capture file"); open_err = 2; }
        if (lost) { note(MODE_NAME ": a period was skipped"); n_skipped += (uint32_t)lost; lost = 0; }
        if (pending_note) { const char *pn = pending_note; pending_note = 0; note(pn); }
        if (jsc && jsc->st_bad != dict_bad) { dict_bad = jsc->st_bad; note(MODE_NAME ": dictionary file corrupt, shown raw"); }
#ifdef JS8_HOST_CMD
        { const char *hc = JS8_HOST_CMD(); if (hc) ui_cmd(hc); }   /* test bench: an operator command without a tap */
#endif
        if (cap.clock_bad && !clock_warned) { note(MODE_NAME ": radio clock not set"); clock_warned = 1; }
        int s = done[0].ready ? 0 : done[1].ready ? 1 : -1;
        if (s >= 0 && done[s ^ 1].ready) {      /* both waiting: decode the older one only if it is not stale */
            int a = done[s].key, b = done[s ^ 1].key;
            if ((b - a + NKEYS) % NKEYS > NKEYS / 2) s ^= 1;
            note_at(done[s].hh, done[s].mm, done[s].ss, MODE_NAME ": skipped (decoder behind)");
            n_skipped++; done[s].ready = 0; s ^= 1;
        }
#ifdef JS8_TX
        {   /* transmit when a confirmed message is due; do not start a decode that would be cut right away */
            int due = txpoll();
            tx_panel();
            if (due == 0) { tx_slot(); continue; }
            if (due > 0 && due < 4000) s = -1;
        }
        kbd_poll1(1);                            /* a tap on a row: the radio's keyboard */
#endif
#ifdef JS8_LINK
        link_service();                          /* the terminal: bounded, and only when capture and TX leave the time */
#endif
        if (s < 0) {                             /* idle: write what must not wait for the end of a period (error notes, a marked
                                                  * exchange), and the state file after it, so that the state is never more than
                                                  * one segment behind the card */
            log_tick(&lg_frames, 0); log_tick(&lg_msgs, 0); snap_tick();
            icap_sleep_ms(20); continue;
        }
        /* decode slot s while the next period is captured into the other slot */
        decoding = s; done[s].ready = 0;
        rd.slot = s; rd.len = done[s].n; rd.bn = 0;
        int hh = done[s].hh, mm = done[s].mm, ss = done[s].ss, werr = done[s].werr, gap_ms = done[s].gap_ms, key = done[s].key;
        uint32_t ymd = done[s].ymd;
        float nom = done[s].nom;
        dsrc_t src = { 0, src_read, done[s].n, FS };
        timed_out = bud_cut = 0; t_pump = t_sd = 0;
#ifdef JS8_TX
        tx_cut = 0;
#endif
        if (st_key != key) js8_stream_stop();    /* the rows in the arena are not this period's */
        uint32_t t0 = dec_t0 = icap_ticks();
        int nd = js8_decode(&src, cfg_flo, cfg_fhi, nom, res, MAXOUT, work, wlen, dec_abort);
        uint32_t ms = icap_ticks() - t0, sd_ms = t_sd, pump_ms = t_pump;
#ifdef JS8_TX
        if (tx_cut && !stop()) { decoding = -1; tx_slot(); }   /* first the transmission (no SD access before it), then the results */
#endif
        if (rd.fd >= 0) { icap_close(rd.fd); rd.fd = -1; }
        int late = timed_out, cutn = late || bud_cut;   /* cut by the next capture / by budget=: both are reported */
        decoding = -1; st_key = -1;
        if (cap.fd >= 0 && !cap.werr) st_begin();    /* the capture in progress: stream from here on */
        if (stop()) break;
        now_p.hh = hh; now_p.mm = mm; now_p.ss = ss; now_p.ymd = ymd;
        mono += (uint32_t)(last_key < 0 ? 1 : (key - last_key + NKEYS) % NKEYS) * PERIOD; last_key = key;
        n_periods++; dec_ms_last = ms; dec_ms_sum += ms; if (ms > dec_ms_max) dec_ms_max = ms;
        if (js8_work_peak() > arena_peak) arena_peak = js8_work_peak();
        last_nd = nd > 0 ? nd : 0;
        if (werr || gap_ms >= 100) {
            if (werr) cat(line, MODE_NAME ": SD write error (capture)");
            else { q = cat(line, MODE_NAME ": audio gap "); q = num(q, gap_ms, 0, ' '); cat(q, " ms"); }
            note_at(hh, mm, ss, line);
        }
#ifdef JS8_TX
        if (tx_cut) { note_at(hh, mm, ss, MODE_NAME ": decode cut for TX"); if (nd == E_ABORT) nd = 0; }
#endif
        if (nd < 0 && (nd != E_ABORT || cutn)) {
            q = cat(line, nd == E_ABORT ? MODE_NAME ": decode out of time" : nd == E_IO ? MODE_NAME ": SD read error" : MODE_NAME ": decoder error ");
            if (nd != E_ABORT && nd != E_IO) num(q, nd, 0, ' ');
            /* an error that is not about this one period (bad arguments) is reported once, not every 15 s */
            if (nd == E_ABORT || nd == E_IO || nd != last_err) note_at(hh, mm, ss, line);
            last_err = nd;
        } else if (nd >= 0) last_err = 0;
        /* results.  A compressed frame may need SD reads (dictionary) and a full log tail an archive write: the stop
         * request is checked before every frame and inside every dictionary read */
        for (int i = 0; i < nd && i < MAXOUT && !stop(); i++) {
            on_frame(&res[i], mono); polite();
#ifdef JS8_TX
            tx_service();
#endif
#ifdef JS8_LINK
            link_service();
#endif
        }
        if (!quit) {
            js8asm_tick(asmb, mono);             /* close messages that timed out */
            js8f_tick(feat, p_abs());            /* forget prepared replies nobody confirmed */
#ifdef JS8_TX
            js8tx_period(&txq);
#endif
            if (nd > 0 && cutn) note_at(hh, mm, ss, MODE_NAME ": partial, decode out of time");
            if (pop_tok >= POP_BURST) pop_refill = n_periods;
            else if (n_periods - pop_refill >= POP_REFILL) { pop_tok++; pop_refill = n_periods; }
        }
        log_tick(&lg_frames, 1); log_tick(&lg_msgs, 1); snap_tick();
#ifdef JS8_TX
        perf_add(key, nd, late ? 1 : bud_cut ? 2 : tx_cut ? 4 : 0, ms, icap_ticks() - t0 - ms, sd_ms, pump_ms);
#else
        perf_add(key, nd, late ? 1 : bud_cut ? 2 : 0, ms, icap_ticks() - t0 - ms, sd_ms, pump_ms);
#endif
        panel();
#if JS8_PERF_FILE
#ifdef JS8_TX
        {   /* no SD work of ours right before a transmission: the file is written again after the next period */
            int due = txpoll();
            if (due >= 0 && due < 4000) continue;
        }
#endif
        if (cfg_perf && !stop()) perf_write();   /* the stop poll also keeps the SD calls between two polls bounded */
#endif
    }
#ifdef JS8_TX
    js8tx_abort(&txq, MODE_NAME ": exit, TX queue cleared");   /* ends a granted transmission, clears the queue */
#endif
#ifdef JS8_TX
    kbd_leave();                                 /* the radio's keyboard holds a callback into this app: close it, wait until it is gone */
#endif
    /* leave: messages still being received are closed and logged (flag 'e'), nothing is shown any more; then the
     * logs (seg_write keeps pumping audio, so the capture handle must still be valid - a closed handle number is
     * given out again by the loader and audio would land in the log file), the state, then the capture */
    leaving = 1;
    js8asm_tick(asmb, mono + 100000u);
    log_flush_now(&lg_frames); log_flush_now(&lg_msgs); snap_dirty = 1; snap_save();   /* (snap_save: at once, whatever the last try did) */
    if (cap.fd >= 0) { icap_close(cap.fd); cap.fd = -1; }
    if (rd.fd >= 0) { icap_close(rd.fd); rd.fd = -1; }
#ifdef JS8_LINK
    js8link_flush(lnk); js8link_drop(lnk);
#endif
    return 0;
}
