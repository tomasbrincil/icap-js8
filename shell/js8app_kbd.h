/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8app_kbd.h - how the owner sends from the radio alone (owner decision 24 Nov 2026, replaces the gesture menu of 23 Nov 2026):
 * a tap on a row of the DECODE screen opens the RADIO'S OWN KEYBOARD with a text; ENT on the keyboard is the confirmation of
 * exactly that text; EXIT sends nothing.  Nothing else is on the DECODE screen: decoded rows and a few status rows.
 * Included by shell/js8app.c in the TX flavour only (the RX build has no taps to handle).  Not a stand-alone header: it uses
 * the statics of js8app.c defined above its #include.
 *
 *   tap on ...                                the keyboard opens with
 *   a decoded row of a station                "<CALL> " (the owner writes the rest, or deletes the call); a continuation
 *                                             row ("     TEXT") takes the call of the row above it (U6, 24 Nov 2026)
 *   the query of a station to MYCALL, or the  the prepared reply ("Q1ABC SNR -08", "Q7AB HEARING Q4XYZ Q5ABC") - the
 *     "TX? <reply>" row after it              owner sees it whole and presses ENT, or edits it
 *   any other row                             "" (empty)
 *   (after a GO from the terminal)            the terminal's text, whatever row is tapped (kept until ENT; EXIT loses it:
 *                                             "TX: terminal text dropped")
 *   (after a text refused for its encoding)   that text, to be edited (once)
 *   (TXTEST)                                  "HB <grid>"
 * A text over the keyboard's 43 characters is refused, never cut (a keyer memory up to 70 / a preset: "TX: M2 has 56 chars,
 * max 43"; a prepared reply, a terminal text: "TX: text over 43 characters").  A tap while a tap grant is open and not yet keyed
 * gives it back ("TX: cancelled before the slot", S3); a tap while the owner abort is latched is refused ("TX: aborted - restart
 * the app to send", S2); a tap is ignored while a beacon transmission is due within 4 s (on the air the app does not poll at
 * all); a second tap before the keyboard opened retargets it.  Taps are not delivered while the beacon is armed: after a frame
 * under it the row "TX done - red frame off to tap again" (after an HB of txhb=: "HB sent, next in N min - red off stops").
 * ENT with ...                                happens
 *   a text                                    js8tx_send: it REPLACES the owner's message that waits (ONE waiting message:
 *                                             "TX: replaced the waiting message" / "TX: dropped the message in progress");
 *                                             ONE frame -> on the loader's tap credit ("@QTX", the tap on the row; no
 *                                             arming); else queued: "TX queued: arm beacon to send k frames"
 *   only a callsign, the untouched "<CALL> ", "TX: nothing to send - type a message" - nothing changes
 *     or under 2 characters
 *   M1..M8 / P1..P4 alone                     the keyboard opens again with keyer memory n (JS8MEM.TXT of MEM.APP, read in the
 *                                             main loop only) / preset tx1..tx4 of JS8.CFG, to be seen whole and sent with ENT
 *   "<CALL> M1" / "<CALL> P1"                 (the call filled in by the app) the keyboard opens again with "<CALL> <text>"
 *   an empty text, "X" or "CANCEL"            the queue is cleared ("TX: cancelled, queue cleared") - the cancel; also
 *     (also "<CALL> X" / "<CALL> CANCEL")     after the call the app filled in (U3)
 *   a byte outside 32..126                    refused ("TX refused: unusual character"), the next tap reopens it (S5)
 * EXIT: "TX: keyboard closed, nothing sent" / "TX: keyboard closed, 3 frames wait" (U5).
 * The keyboard holds 43 characters: what goes out is never more than the owner saw.
 *
 * The keyboard (705J 1.40 / SQ6EMM v14 J ONLY; verified on the radio 24 Nov 2026 with ../kbd.c, also from the DECODE screen of
 * mode FT8, which it returns to after ENT, the app running on meanwhile):
 *   0x200cd828()                          close the current selector
 *   0x200f1600(349)                       open the stock D-PRS comment editor screen
 *   0x20055a0c(62, 0, text, 43, 2, cb)    our text and OUR callback into the keyboard (the stock comment is neither loaded
 *                                         nor saved)
 *   guard: the first 4 words of each of the three functions must be those of the v14 J image (= stock 1.40), else no keyboard
 *   (note, one row; S6 24 Nov 2026)
 *   cb runs on the GUI task at ENT: copy the text, *action = 4 (commit, back), return 1
 *   open: the halfword at 0x203e84c9 (the open menu, BI1BIF emulator) reads 349; another value twice in a row = the keyboard
 *   was left without ENT (EXIT): nothing is sent.  Not 349 within 300 ms of the open (S8): "KBD: cannot see EXIT - finish with
 *   ENT", the keyboard is never taken as closed by the menu id, and a stop request closes it and waits 2 s.
 *   The menu id is also read at the tap: the keyboard opens only if it is the same at open time ("TX: screen changed - tap
 *   again", S7), and only from the main loop - never inside the decoder's abort callback (a tap during a decode opens after it)
 * Risk known from the emulator: a touch on the screen while the keyboard opens can crash the radio.  Therefore the app opens
 * it only after the finger is OFF the screen (the raw touch byte at 0x203e1f23 reads 1 = released, for 300 ms), and with
 * kbdirq=1 (JS8.CFG; default 0 since 24 Nov 2026 = the path proven on the radio; 1 is untested there) the three calls run with IRQs
 * disabled (the app runs in System mode, CPSR 0x1f).  Traced in the emulator (stock and v14, 24 Nov 2026): the three calls make no
 * SVC; the only call into the RTOS region is a veneer to a memset.  The callback is in this app: while the
 * keyboard is open (menu id 349) the app never returns from app_main - on a stop request it closes the keyboard itself with
 * 0x200cd828() (same guard word, same IRQ rule) and waits until the menu id is no longer 349, however long.  A keyboard open
 * for 120 s with no ENT (also an EXIT that the menu id did not show) is closed the same way ("KBD: keyboard closed by the app").
 */
#define KBD_LEN      43
#define KBD_PROLOGUE 0xe92d47f0u
#define KBD_MENU_ID  349u
#define KBD_IDLE_MS  120000u                     /* open this long with no ENT: the app closes the keyboard itself */
#define F_MEM        MODE_NAME "MEM.TXT"
#define MEM_N        8
#define MEM_W        70                          /* MEM.APP: up to 70 characters per memory (over 43: refused, never cut) */

static char kbd_buf[KBD_LEN];                    /* the keyboard's text, blank-padded; valid while the keyboard is open */
static char kbd_got[KBD_LEN + 1];                /* what ENT returned */
static char kbd_pre[KBD_LEN + 1];                /* the text the keyboard opens with */
static volatile uint8_t kbd_state;               /* 0 idle, 1 waiting for the finger to leave the screen, 2 open, 3 ENT came */
static uint8_t kbd_off, kbd_gone, kbd_init, kbd_pcall, kbd_ever, kbd_keep;   /* kbd_keep: kbd_got was refused for its encoding - the next
                                                                                * tap opens the keyboard with it (kbd_got stays until then) */
static uint16_t kbd_seq;
static uint32_t kbd_t, kbd_poll, kbd_rel;
static volatile uint8_t kbd_odd;                 /* S5: ENT returned a byte outside 32..126 (shown as '?'): the text is refused */
static uint8_t kbd_noid;                         /* S8: 0 the menu id shows the keyboard, 2 not yet seen 349 after the open, 1 never
                                                  * (EXIT cannot be seen: the keyboard is never taken as closed by the menu id) */
static unsigned kbd_mid;                         /* S7: the menu id when the tap was seen: the keyboard opens only on that screen */

static void krow(const char *s) { char l[DEC_W + 1]; int k = 0; while (s[k] && k < DEC_W) { l[k] = clean(s[k]); k++; } l[k] = 0; icap_decode_out(l); }

#ifdef __arm__
#define KBD_CB_ATTR __attribute__((target("arm"), noinline))   /* ARM code: called by the firmware's GUI task */
#else
#define KBD_CB_ATTR __attribute__((noinline))
#endif
KBD_CB_ATTR static unsigned kbd_cb(const char *t, uint8_t *action)   /* the GUI task: tiny, ARM code */
{
    int n = 0;
    while (n < KBD_LEN && t[n]) { char c = t[n]; if (c < 32 || c > 126) { kbd_odd = 1; c = '?'; } kbd_got[n] = c; n++; }
    while (n && kbd_got[n - 1] == ' ') n--;
    kbd_got[n] = 0;
    __asm__ volatile("" ::: "memory");           /* kbd_got is written before the task can see kbd_state == 3 */
    kbd_state = 3;
    *action = 4;
    return 1;
}
#if defined JS8_HOST_KBD                        /* the test bench (tests/js8sim.c) */
#define KBD_FW_OK()   1
#define KBD_TOUCHED() JS8_HOST_TOUCHED()
#define KBD_MENU()    JS8_HOST_KBDID()
#elif !defined JS8_KBD                           /* a build without the keyboard: taps do nothing */
#define KBD_FW_OK()   0
#define KBD_TOUCHED() 0
#define KBD_MENU()    0u
#else
/* S6 (24 Nov 2026): the first 4 words of each of the three functions, as in the v14 J image (load address 0x20005000) and
 * identically in the stock 705J 1.40 image (fw/dec/v14/main.bin, fw/dec/orig/main.bin) */
static const uint32_t kbd_guard[3][5] = {
    { 0x200cd828u, 0xe92d4010u, 0xe3a02000u, 0xe59f4548u, 0xe1d400b0u },   /* close the current selector */
    { 0x200f1600u, 0xe92d4070u, 0xe1a06000u, 0xe51f53e4u, 0xe1d500bau },   /* open the comment editor screen */
    { 0x20055a0cu, KBD_PROLOGUE, 0xe1a06000u, 0xe1a07001u, 0xe1a09002u },  /* text + callback into the keyboard */
};
static int kbd_fw_ok(void)
{
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++)
        if (*(volatile const uint32_t *)(uintptr_t)(kbd_guard[i][0] + 4u * (uint32_t)j) != kbd_guard[i][j + 1]) return 0;
    return 1;
}
#define KBD_FW_OK()   kbd_fw_ok()
/* 24 Nov 2026 on the radio: the raw touch byte 0x203e1f23 did NOT read "released" after a tap ("keyboard not opened (screen held)"),
 * so the emulator's meaning of it is wrong for the real radio. The radio build no longer reads it: the keyboard opens
 * KBD_REL_MS after the tap was seen (a tap is reported when the finger lifts). */
#define KBD_TOUCHED() 0
#define KBD_REL_MS    800u
#define KBD_MENU()    ((unsigned)*(volatile const uint8_t *)0x203e84c9u | (unsigned)*(volatile const uint8_t *)0x203e84cau << 8)
#endif
#if defined JS8_KBD && !defined JS8_HOST_KBD
#define KBD_IRQ_OFF(ps) do { if (cfg_kbdirq) __asm__ volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(ps) :: "memory"); } while (0)
#define KBD_IRQ_ON(ps)  do { if (cfg_kbdirq) __asm__ volatile("msr cpsr_c, %0" :: "r"(ps) : "memory"); } while (0)
#endif
static COLD void kbd_open(void)
{
    int i;
    for (i = 0; i < KBD_LEN; i++) kbd_buf[i] = ' ';
    for (i = 0; kbd_pre[i] && i < KBD_LEN; i++) kbd_buf[i] = kbd_pre[i];
    kbd_got[0] = 0; kbd_gone = 0; kbd_keep = 0; kbd_ever = 1; kbd_odd = 0;
#if defined JS8_HOST_KBD
    JS8_HOST_KBD(kbd_buf, KBD_LEN, kbd_cb);
#elif !defined JS8_KBD
    (void)kbd_cb;
#else
    {
        uint32_t ps = 0;
        KBD_IRQ_OFF(ps);
        ((void (*)(void))0x200cd828u)();
        ((void (*)(unsigned))0x200f1600u)(KBD_MENU_ID);
        ((void (*)(unsigned, unsigned, const char *, unsigned, unsigned, unsigned (*)(const char *, uint8_t *)))0x20055a0cu)(62u, 0u, kbd_buf, KBD_LEN, 2u, kbd_cb);
        KBD_IRQ_ON(ps);
    }
#endif
    kbd_state = 2; kbd_t = icap_ticks();
    kbd_noid = KBD_MENU() == KBD_MENU_ID ? 0 : 2;   /* S8: decided within 300 ms (the next polls) */
}
/* the app closes the keyboard (a stop request while it is open, or open KBD_IDLE_MS with no ENT): 0x200cd828() closes the
 * current selector - only on the firmware whose keyboard the app opened (the same guard word) */
static COLD void kbd_close(void)
{
#if defined JS8_HOST_KBD
    JS8_HOST_KBDCLOSE();
#elif defined JS8_KBD
    if (KBD_FW_OK()) {
        uint32_t ps = 0;
        KBD_IRQ_OFF(ps);
        ((void (*)(void))0x200cd828u)();
        KBD_IRQ_ON(ps);
    }
#endif
}
/* the keyboard went without ENT: a text of the terminal that it showed is lost (it is kept only until ENT) */
static COLD void kbd_lost(const char *why)
{
    kbd_state = 0; kbd_init = 0;
    krow(why);
#ifdef JS8_LINK
    if (txq.tpend[0]) { txq.tpend[0] = 0; krow("TX: terminal text dropped"); }
#endif
}
/* EXIT: what still waits (U5) */
static COLD void kbd_exit(void)
{
    char l[DEC_W + 1], *q;
    int i, k = 0;
    for (i = 0; i < txq.qn; i++) if (txq.q[i].src != JS8TX_SRC_CQ) k += txq.q[i].n - txq.q[i].k;
    if (!k) { kbd_lost("TX: keyboard closed, nothing sent"); return; }
    q = num(cat(l, "TX: keyboard closed, "), k, 0, ' '); cat(q, k == 1 ? " frame waits" : " frames wait");
    kbd_lost(l);
}
/* U6: a continuation row ("     TEXT") takes the call of the row above it.  The rows the app printed last: hash -> call */
#define KCONT_N 8
static struct { uint32_t h; char c[JS8_CALL_MAX]; } kcont[KCONT_N];
static uint8_t kcont_i;
static char kcont_call[JS8_CALL_MAX];
static uint32_t khash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static int row_call(const char *row, char *call);
static void kbd_cont(const char *line, int first)  /* every DECODE line of a decoded text (dec_lines) */
{
    if (first) { if (!row_call(line, kcont_call)) kcont_call[0] = 0; return; }
    if (!kcont_call[0]) return;
    kcont[kcont_i].h = khash(line); cat(kcont[kcont_i].c, kcont_call); kcont_i = (uint8_t)((kcont_i + 1) % KCONT_N);
}
/* the text the keyboard opens with.  A text over the keyboard's 43 characters (a keyer memory, a preset, a prepared reply, a
 * text of the terminal) is refused, never cut: what could go out is only what the owner saw whole. */
static COLD int pre_set(const char *s)
{
    int k = 0;
    while (s[k]) k++;
    if (k > KBD_LEN) { krow("TX: text over 43 characters"); return 0; }
    for (k = 0; (kbd_pre[k] = s[k]) != 0; k++) ;
    return 1;
}

/* JS8MEM.TXT: "m1=<text>" .. "m8=<text>", one per line, written by MEM.APP.  Untrusted like JS8.CFG: one bounded read, lines
 * not of that form are ignored, a text is cut at MEM_W (and then refused as over 43 characters), a byte outside printable
 * ASCII becomes '?' (js8pack then refuses it on ENT).  Read when "Mn" is typed - in the main loop only, never inside the
 * decoder's abort poll; one small file read, never near a transmission (the keyboard is not open then). */
static COLD int mem_get(int m, char *o)
{
    char b[MEM_N * (MEM_W + 6) + 32];
    int fd = icap_open(F_MEM, ICAP_O_READ), n = 0, r, i, k = 0, ls = 1;
    o[0] = 0;
    if (fd < 0) return 0;
    while (n < (int)sizeof b - 1 && (r = icap_read(fd, b + n, (uint32_t)(sizeof b - 1 - n))) > 0) n += r;
    icap_close(fd);
    b[n] = 0;
    for (i = 0; i < n; i++) {
        char c = b[i];
        if (c == '\n' || c == '\r') { ls = 1; continue; }
        if (!ls) continue;
        ls = 0;
        if ((c == 'm' || c == 'M') && b[i + 1] == '1' + m && b[i + 2] == '=') {
            for (i += 3, k = 0; i < n && b[i] != '\n' && b[i] != '\r'; i++) if (k < MEM_W + 1) { unsigned char u = (unsigned char)b[i]; o[k++] = (char)(u < 32 || u > 126 ? '?' : u); }
            o[k] = 0;
            i--;
        }
    }
    while (k && o[k - 1] == ' ') o[--k] = 0;
    return o[0] != 0;
}

/* the station of a decoded DECODE row "HHMM sNN FFFF [= |> ]CALL: ...", or 0 */
static int row_call(const char *row, char *call)
{
    int i;
    for (i = 0; i < 4 && row[i] >= '0' && row[i] <= '9'; i++) ;
    if (i < 4 || row[4] != ' ' || (row[5] != '+' && row[5] != '-') || row[6] < '0' || row[6] > '9') return 0;
    row += 8;
    while (*row == ' ') row++;
    while (*row >= '0' && *row <= '9') row++;    /* the frequency */
    while (*row == ' ') row++;
    if ((row[0] == '=' || row[0] == '>') && row[1] == ' ') row += 2;
    for (i = 0; i < JS8_CALL_MAX - 1 && row[i] > ' ' && row[i] != ':'; i++) call[i] = row[i];
    call[i] = 0;
    return row[i] == ':' && i >= 3 && call[0] != '@';
}
/* a tap on this row: the text the keyboard opens with (kbd_pre); 0 = the keyboard does not open (a row says why) */
static COLD int kbd_prefill(const char *row)
{
    char c[JS8_CALL_MAX];
    int i, k;
    kbd_pre[0] = 0; kbd_pcall = 0;
#ifdef JS8_LINK
    if (txq.tpend[0]) { if (pre_set(txq.tpend)) return 1; txq.tpend[0] = 0; return 0; }   /* the terminal's text: kept until ENT */
#endif
    if (kbd_keep) return pre_set(kbd_got);       /* the text refused for its encoding, to be edited */
    if (txq.test) { char *q = cat(kbd_pre, "HB "); cat(q, txq.grid); return 1; }
    if (row[0] == 'T' && row[1] == 'X' && row[2] == '?' && row[3] == ' ') return pre_set(row + 4);   /* a prepared reply row */
    for (i = 0; i < CONT_W && row[i] == ' '; i++) ;
    if (i == CONT_W && row[i] > ' ') {          /* a continuation row: the call of the row above it (U6) */
        uint32_t h = khash(row);
        for (k = 1; k <= KCONT_N; k++) {
            int j = (kcont_i + KCONT_N - k) % KCONT_N;
            if (kcont[j].c[0] && kcont[j].h == h) { cat(cat(kbd_pre, kcont[j].c), " "); kbd_pcall = 1; return 1; }
        }
        return 1;
    }
    if (!row_call(row, c)) return 1;
    for (i = 0; i < JS8F_PREP; i++) {            /* a query of this station to me: its prepared reply */
        const char *p = feat->prep[i];
        for (k = 0; c[k] && p[k] == c[k]; k++) ;
        if (p[0] && !c[k] && p[k] == ' ' && row[PFX_W] == '>') return pre_set(p);
    }
    cat(cat(kbd_pre, c), " "); kbd_pcall = 1;
    return 1;
}
static int is_mem(const char *t) { return (t[0] == 'M' || t[0] == 'm') && t[1] >= '1' && t[1] <= '0' + MEM_N && !t[2]; }
static int is_pre(const char *t) { return (t[0] == 'P' || t[0] == 'p') && t[1] >= '1' && t[1] < '1' + JS8TX_PRESETS && !t[2]; }
/* "", "X" or "CANCEL" (any case): the cancel */
static int is_cancel(const char *t)
{
    static const char cw[] = "CANCEL";
    int i;
    if (!t[0] || ((t[0] | 32) == 'x' && !t[1])) return 1;
    for (i = 0; cw[i] && (t[i] & ~32) == cw[i]; i++) ;
    return !cw[i] && !t[i];
}
/* after the "<CALL> " the app filled in (untouched): the rest the owner typed; else the text itself (U3) */
static const char *kbd_rest(const char *t)
{
    int i;
    while (*t == ' ') t++;
    if (!kbd_pcall) return t;
    for (i = 0; kbd_pre[i] && t[i] == kbd_pre[i]; i++) ;
    if (kbd_pre[i]) return t;
    for (t += i; *t == ' '; t++) ;
    return t;
}
static COLD void kbd_take(void)                  /* ENT came */
{
    char *t = kbd_got, m[MEM_W + 2];
    const char *r;
    int g = 0, rc, i, cl = 0;
    kbd_state = 0; kbd_init = 0;                 /* (taps made while the keyboard was up are not requests) */
#ifdef JS8_LINK
    txq.tpend[0] = 0;                            /* the terminal's text was on the keyboard: ENT takes what it showed */
#endif
    while (*t == ' ') t++;
    {   /* 24 Nov 2026 on the radio: the screen shows HEARTBEAT as HB, so the owner types "<CALL> HB SNR -14"; JS8's command is
         * "HEARTBEAT SNR" (one frame) - "HB SNR" would go as two frames of free text.  Expand it in place. */
        static char x[2 * KBD_LEN + 2];
        int a = 0, b = 0;
        while (t[a] && b < (int)sizeof x - 1) {
            if ((a == 0 || t[a - 1] == ' ') && t[a] == 'H' && t[a + 1] == 'B' && t[a + 2] == ' ' && t[a + 3] == 'S' && t[a + 4] == 'N' && t[a + 5] == 'R'
                && b + 9 < (int)sizeof x - 1) { const char *h = "HEARTBEAT"; while (*h) x[b++] = *h++; a += 2; continue; }
            x[b++] = t[a++];
        }
        x[b] = 0;
        if (b > KBD_LEN) { krow("TX: text over 43 characters"); return; }
        t = x;
    }
    if (kbd_odd) {                               /* S5: a byte the keyboard should not give: refused, shown again on the next tap */
        kbd_odd = 0; kbd_pcall = 0; kbd_keep = 1;
        krow("TX refused: unusual character");
        return;
    }
    r = kbd_rest(t);
    if (r != t) cl = slen(kbd_pre);              /* the untouched "<CALL> " of the app, then the owner's text r */
    if (is_cancel(t) || (cl && r[0] && is_cancel(r))) {   /* empty, "X", "CANCEL" (also after the app's "<CALL> "): the cancel */
        kbd_pcall = 0;
        if (txq.qn || txq.grant) js8tx_abort(&txq, "TX: cancelled, queue cleared"), krow("TX: cancelled, queue cleared");
        else krow("TX: nothing sent");
        return;
    }
    for (i = 0; kbd_pcall && t[i] && t[i] == kbd_pre[i]; i++) ;
    if (kbd_pcall && !t[i] && kbd_pre[i] == ' ' && !kbd_pre[i + 1]) { kbd_pcall = 0; krow("TX: nothing to send - type a message"); return; }   /* the untouched "<CALL> " */
    kbd_pcall = 0;
    if (is_mem(r) || is_pre(r)) {
        /* keyer memory Mn (JS8MEM.TXT) / preset Pn (tx1..tx4): the keyboard opens again WITH it ("<CALL> " + it after the app's
         * call) - seen whole, then ENT.  Over 43 characters: refused, never cut */
        const char *p = m;
        int n;
        if (is_pre(r)) { p = txq.preset[r[1] - '1']; if (!p[0]) { krow("TX: no such preset (JS8.CFG tx1..tx4)"); return; } }
        else if (!mem_get(r[1] - '1', m)) { krow("TX: no such keyer memory (JS8MEM.TXT)"); return; }
        n = slen(p);
        if (cl + n > KBD_LEN) {                  /* "TX: M2 has 56 chars, max 43" / "TX: Q7AB M2 has 62 chars, max 43" */
            char l[DEC_W + 1], *q = cat(l, "TX: ");
            for (i = 0; i < cl && i < JS8_CALL_MAX; i++) *q++ = kbd_pre[i];
            *q++ = (char)(r[0] & ~32); *q++ = r[1];
            q = num(cat(q, " has "), cl + n, 0, ' '); cat(q, " chars, max 43");
            krow(l);
            return;
        }
        for (i = 0; i < n; i++) kbd_pre[cl + i] = p[i];
        kbd_pre[cl + n] = 0;
        kbd_state = 1; kbd_rel = 0; kbd_t = icap_ticks();
        return;
    }
    rc = js8tx_send(&txq, t, &g);
    kbd_keep = rc == JS8TX_E_ENC || rc == JS8TX_E_TEXT || rc == JS8TX_E_LONG;   /* the next tap reopens the keyboard with it */
    if (rc == JS8TX_E_ENC) {                     /* 24 Nov 2026: say WHICH character (without the dictionary only the 44 Huffman ones) */
        extern const js8_huff_t js8_huff[44];
        for (i = 0; t[i]; i++) {
            int k, ok = t[i] == ' ';
            for (k = 0; k < 44 && !ok; k++) ok = js8_huff[k].c == t[i];
            if (!ok) {
                char l[DEC_W + 1], *q = cat(l, "TX: '");
                *q++ = t[i]; cat(q, "' cannot be sent - tap to edit");
                krow(l);
                break;
            }
        }
    }
#ifdef JS8_LINK
    if (rc > 0) link_queued();
#endif
}
/* S3: js8tx_run waits for the slot of a tap grant - a new tap (the "@QSO" n= moved) gives the grant back */
#ifndef KBD_REL_MS
#define KBD_REL_MS 300u
#endif
static int kbd_tapped(void)
{
    char b[112];
    int fd, n, i, v = 0;
    uint32_t now = icap_ticks();
    if (kbd_off || kbd_state || now - kbd_poll < 100u) return 0;
    kbd_poll = now;
    fd = icap_open("@QSO", ICAP_O_READ);
    if (fd < 0) return 0;
    n = icap_read(fd, b, sizeof b - 1); icap_close(fd);
    if (n < 3 || b[0] != 'n' || b[1] != '=') return 0;
    for (i = 2; i < n && b[i] >= '0' && b[i] <= '9'; i++) v = (v * 10 + (b[i] - '0')) & 0xffff;
    if (!kbd_init) { kbd_init = 1; kbd_seq = (uint16_t)v; return 0; }
    if (v == kbd_seq) return 0;
    kbd_seq = (uint16_t)v;
    return 1;
}
/* every 100 ms from the main loop and the decoder's abort poll (inmain 0): "@QSO" n= (a tap) and t= (the tapped row); the keyboard.
 * The keyboard is opened and closed only from the main loop (inmain 1), never inside a decode (S7, 24 Nov 2026) */
static COLD void kbd_poll_(uint32_t now, int inmain)
{
    char b[112];
    int fd, n, v = 0, i;
    kbd_poll = now;
    if (kbd_state == 2) {                        /* open: left without ENT? */
        static const char w[] = "KBD: keyboard closed by the app", nw[] = "KBD: cannot see EXIT - finish with ENT";
        unsigned id = KBD_MENU();
        if (id == KBD_MENU_ID) { kbd_gone = 0; if (kbd_noid == 2) kbd_noid = 0; }
        else if (kbd_noid == 2) { if (now - kbd_t >= 300u) { kbd_noid = 1; note(nw); krow(nw); } }   /* S8 */
        else if (!kbd_noid && ++kbd_gone >= 2) { kbd_exit(); return; }
        if (inmain && now - kbd_t > KBD_IDLE_MS) {   /* open 120 s with no ENT (an EXIT the menu id did not show): closed */
            kbd_close(); note(w); kbd_lost(w);
        }
        return;
    }
    if (kbd_state == 3) {                        /* ENT.  JS8MEM.TXT is read in the main loop only */
        __asm__ volatile("" ::: "memory");       /* kbd_got is read after kbd_state */
        if (inmain || !is_mem(kbd_rest(kbd_got))) kbd_take();
        return;
    }
    fd = icap_open("@QSO", ICAP_O_READ);         /* idle, or waiting for the finger: a new tap (re)targets the keyboard */
    if (fd == ICAP_E_NAME) { kbd_off = 1; return; }
    n = -1;
    if (fd >= 0) { n = icap_read(fd, b, sizeof b - 1); icap_close(fd); }
    if (n >= 4 && b[0] == 'n' && b[1] == '=') {
        b[n] = 0;
        while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
        for (i = 2; b[i] >= '0' && b[i] <= '9'; i++) v = (v * 10 + (b[i] - '0')) & 0xffff;
        if (!kbd_init) { kbd_init = 1; kbd_seq = (uint16_t)v; }
        else if (v != kbd_seq) {
            kbd_seq = (uint16_t)v;
            /* a tap is no request while TX is off, while a beacon transmission is due within 4 s (on the air the app does not
             * poll: kbd_resync), or on another firmware.  While the owner abort is latched it is refused (S2); while a tap
             * grant is open and not yet keyed it gives the grant back (S3) */
            if (!txq.enable || txq.dead) v = -1;
            else if (txq.owner) { kbd_state = 0; krow("TX: aborted - restart the app to send"); return; }
            else if (txq.grant) { kbd_state = 0; js8tx_abort(&txq, "TX: cancelled before the slot"); krow("TX: cancelled before the slot"); return; }
            else if (tx_due >= 0 && (int32_t)(tx_due_t + (uint32_t)tx_due - now) < 4000) v = -1;
            else if (!KBD_FW_OK()) { if (kbd_off != 2) { kbd_off = 2; note("KBD: not 705J 1.40 / v14 J - no keyboard"); krow("TX: no keyboard in this firmware"); } return; }
            if (v >= 0) {
                for (; b[i]; i++) if (b[i] == ' ' && b[i + 1] == 't' && b[i + 2] == '=') break;
                kbd_mid = KBD_MENU();            /* S7: the screen the tap was made on */
                if (kbd_prefill(b[i] ? b + i + 3 : "")) { kbd_state = 1; kbd_t = now; kbd_rel = 0; }
                else kbd_state = 0;
            }
        }
    }
    if (kbd_state == 1) {                        /* the finger must be off the screen for 300 ms before the keyboard opens */
        if (KBD_TOUCHED() || (kbd_ever && KBD_MENU() == KBD_MENU_ID)) kbd_rel = 0;   /* (also: the keyboard of the last ENT is still closing) */
        else if (!kbd_rel) kbd_rel = now ? now : 1;
        else if (inmain && now - kbd_rel >= KBD_REL_MS) {
            if (KBD_MENU() != kbd_mid) { kbd_state = 0; kbd_init = 0; krow("TX: screen changed - tap again"); }   /* S7: MENU pressed meanwhile */
            else kbd_open();
        }
        if (kbd_state == 1 && !kbd_rel && now - kbd_t > 10000u) { kbd_state = 0; kbd_init = 0; krow("TX: keyboard not opened (screen held)"); }
    }
}
static void kbd_poll1(int inmain)                /* (the decoder's abort poll calls this every 0.1 ms: the check is cheap) */
{
    uint32_t now = icap_ticks();
    if (!kbd_off && now - kbd_poll >= 100u) kbd_poll_(now, inmain);
}
/* after a transmission: taps made while a frame was keyed are no request */
static void kbd_resync(void) { kbd_init = 0; }
/* the first row (TX enabled): there must be a text row to tap */
static void kbd_start(void)
{
    if (!txq.enable) krow("JS8 RX only (tx=1 in JS8.CFG)");   /* U8 */
    else if (!txq.dead) krow(txq.test ? "JS8 TXTEST: tap a row - keyboard: HB" : "JS8 TX: tap a row to write a message");
}
/* leaving: never while the keyboard is open (its callback is in this app).  The app closes it, then waits until the menu id
 * says it is gone - however long that takes.  S8: where the menu id does not show the keyboard, a keyboard that may be open is
 * closed and the app waits 2 s */
static COLD void kbd_leave(void)
{
    if (!kbd_ever) return;
    if (kbd_noid) {
        uint32_t t0 = icap_ticks();
        if (kbd_state != 2) return;
        note("KBD open at exit: closing it");
        kbd_close();
        while (icap_ticks() - t0 < 2000u) {
            while (icap_audio_read(abuf, (int)(sizeof abuf / sizeof abuf[0])) > 0) ;
            icap_sleep_ms(100);
        }
        return;
    }
    if (KBD_MENU() != KBD_MENU_ID) return;
    note("KBD open at exit: closing it");
    kbd_close();
    while (KBD_MENU() == KBD_MENU_ID) {
        while (icap_audio_read(abuf, (int)(sizeof abuf / sizeof abuf[0])) > 0) ;   /* (the audio is not wanted any more: keep the ring empty) */
        icap_sleep_ms(100);
    }
}
