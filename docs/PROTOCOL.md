# The JS8 protocol as this receiver implements it

This is a description of what the code in `src/` does, written for people who want to read or check it. It is **not** a specification of JS8. The authority on JS8 is JS8Call by Jordan Sherer KN4CRD (<https://github.com/js8call/js8call>); the community fork JS8Call-improved (<https://github.com/JS8Call-improved/JS8Call-improved>) was read as well. Everything here was learned from their published sources, and where this text and JS8Call disagree, JS8Call is right. The licences of what was taken from there are in `NOTICE.md`.

Only submode **Normal** is implemented, and only **receiving**.

## 1. Signal

| Property | Value |
|---|---|
| Period | 15 s, starting at :00, :15, :30, :45 |
| Start of a frame | 0.5 s into the period (plus the sender's clock error, "DT") |
| Modulation | continuous-phase 8-FSK, no shaping |
| Tone spacing | 6.25 Hz; the signal is 50 Hz wide |
| Symbol length | 0.160 s (1280 samples at 8000 Hz) |
| Symbols per frame | 79, that is 12.64 s |
| Frequency named for a signal | the lowest tone, as audio offset in Hz |

A frame is laid out as `S7 D29 S7 D29 S7`: three blocks of 7 synchronisation symbols (positions 0-6, 36-42, 72-78) and 58 data symbols. In submode Normal all three blocks are the Costas array `4 2 5 6 1 3 0`. Each data symbol carries 3 bits, tone = 4 b0 + 2 b1 + b2, without a Gray map.

## 2. Frame and error correction

- A frame is **87 bits**: 72 payload bits, 3 flag bits ("i3bit") and a 12-bit CRC.
- The payload is also written as 12 characters of a 64-character alphabet, `0-9 A-Z a-z - +`, 6 bits each.
- **CRC-12**: polynomial 0xC06, initial value 0, most significant bit first, final XOR 0x02A, computed over the first 75 bits.
- **LDPC (174,87)**: 87 parity bits are sent first, then the 87 frame bits. The generator and parity-check tables (`src/js8_ldpc_174_87_tables.h`) are data converted from JS8Call; `make check` compares every table with the JS8Call source files.

The i3bit flags are: 1 = first frame of a message, 2 = last frame, 4 = data frame.

## 3. Frame types

The first payload bits select the type (values 0 to 6 as in JS8Call's `Varicode::FrameType`; 7 is a number used only inside this code):

| Type | Content |
|---|---|
| 0 | heartbeat or CQ, with call sign and 4-character locator |
| 1 | compound call sign |
| 2 | compound call sign, directed |
| 3 | directed message: from, to, command, optional number (for example an SNR report) |
| 4 | data: free text, Huffman coded |
| 6 | data: free text compressed with the JSC dictionary |
| 7 | "fast" data, JSC text. Not a JS8Call `FrameType` value: JS8Call marks these frames with the data flag (i3bit 4), and this code gives them the internal number 7 |

`src/js8msg.c` turns one frame into text; it is meant to produce the text JS8Call shows, which was compared with our reading of `varicode.cpp` and not with the running program. Its tables (Huffman code, the directed commands, the 54 group calls) are derived from JS8Call's `varicode.cpp`.

## 4. The JSC dictionary

JS8Call compresses free text with a dictionary of 262,144 words. It does not fit into the app's memory, and the loader's file service has no seek, so `tools/mkjsc.py` cuts it into many small files that are read front to back:

| Files | Content | Used by |
|---|---|---|
| `JSCHEAD.BIN` | header and the 5740 most frequent words; loaded into memory at start | the app |
| `JSCD000.BIN` .. `JSCD0FF.BIN` | the whole list in 256 buckets of 1024 words | the app, one file per rare word, with a 16-word cache |
| `JSCE000.BIN` .. `JSCE0FF.BIN`, `JSCES.BIN` | the list arranged for looking up text | only the PC test tool that builds test frames; the app never opens them |

The dictionary is (C) 2018 Jordan Sherer KN4CRD, GPL-3.0-or-later. The files are not in this repository; `make dict` generates them from JS8Call's `jsc_list.cpp`, which `make fetch` downloads. For two entries JS8Call and JS8Call-improved differ in how many characters of the word are used; this code follows JS8Call-improved.

## 5. Messages

`src/js8asm.c` joins frames into messages the way JS8Call's receive logic does:

- streams are keyed by audio frequency, within 10 Hz, and follow drift;
- a frame with the "first" flag starts a new message, the "last" flag ends it;
- compound-call frames are joined with the frames that follow;
- commands that carry a checksum (`>`, `MSG`, `MSG TO:`, `QUERY`, `CMD`) are verified with CRC-16/KERMIT;
- 60 s without a frame closes a message as incomplete;
- a message is at most 1023 characters here (we found no such limit in our reading of the JS8Call sources).

Because streams are keyed by frequency only, two stations on the same audio frequency can have their frames joined into one message.

## 6. What the decoder does with a period

`src/js8dec.c` is a fork of the FT8 decoder of the ICAP SDK (which adapts parts of ft8_lib). For each 15 s period:

1. **Waterfall.** The 14.6 s capture is turned into a time-frequency map with two time offsets and two frequency offsets per symbol and bin. With `stream=1` this happens while the period is still being recorded.
2. **Search.** Every position is scored against the three Costas blocks; the best candidates are kept (`minscore`, `maxcand`, `nsync`). DT is searched from about -2.2 to +2.4 s.
3. **Decoding attempts.** For a candidate up to five attempts are made, each a soft-decision belief-propagation run over the LDPC code followed by the CRC check (`passes`, `ns34`, `iters`): two bit metrics, two with the first 24 or 48 soft bits erased (for transmitters that key late), one with interpolated tone powers.
4. **Residual passes.** A decoded signal is re-encoded, subtracted from the audio, and the search is repeated next to it (`sub`, `shift`, `wide`), so that a weaker station under a stronger one can be found.
5. **Budget.** No further pass is started that could not end within `budget` milliseconds.

A frame is reported only when the CRC matches. In the simulator this gave no false decode in 1000 periods of noise (`docs/PERFORMANCE.md`); a false decode is still possible in principle with a 12-bit CRC.

## 7. The encoder in this repository

`src/js8enc.c` (frame to 79 tones) is linked into the app because the decoder needs it: a decoded frame is re-encoded to subtract its signal and to measure its SNR. `src/js8pack.c` (text to frames) is **not** part of the app; together with `tests/js8gen.c` it is the **test signal generator** that writes WAV files for the tests. Nothing in this repository sends audio to a transmitter.

## 8. Checks against JS8Call

- `make check`: the LDPC tables, the Costas array and the alphabet are compared with the JS8Call source files; the encoder is compared with an independent Python model (`tests/model.py`) on 1000 random frames.
- `make rv`: an independent Python model of the protocol (`tests/rv_protocol.py`), built from the JS8Call sources, is compared with the C code.
- `make t6`: the message layer against expectations taken from JS8Call's `varicode.cpp`.

These compare our code with our reading of the JS8Call *sources*. No JS8Call *program* was run against this receiver; that comparison is on the roadmap.
