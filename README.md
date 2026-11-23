# icap-js8 - a JS8 receiver app for the Icom IC-705 (ICAP)

A JS8Call-compatible **receiver** (submode Normal, 15 s periods) that runs inside the IC-705 as an app for the ICAP loader of the custom firmware by Dawid SQ6EMM. It decodes JS8 frames from the radio's own audio, turns them into text, joins multi-frame messages and shows them on the radio's DECODE screen, with no computer attached.

This release is **receive only**. The app contains no transmit code and no network code. Transmitting is on the [roadmap](ROADMAP.md), not in this repository.

The code, the tests and the documentation were written by an AI coding assistant (Claude, by Anthropic) under the direction of Tomas Brincil OK1BT, who tested it on his radio. No human has reviewed the code line by line. See [How this was made](#how-this-was-made) and [`AUTHORS.md`](AUTHORS.md).

**Read this first:** the project is new (November 2026). It has run on one radio. The table below says what was measured on that radio and what is known only from a PC simulator.

## Status

Date of this status: 23 November 2026.

| Feature | Status | Notes |
|---|---|---|
| Receive JS8 Normal: decode, text on the DECODE screen, multi-frame messages joined | **Tested on the radio** | One IC-705 (J version), firmware 20261119v14, live traffic on 7.078 MHz. |
| Dictionary (JSC) on the SD card for compressed text | **Simulator; on the radio not recorded** | The dictionary lookup is tested in the simulator. The notes of the on-air runs do not say which dictionary files were on the card or how much of the live text went through them, so it is not listed here as tested on the radio. |
| Decode time with the default settings | **Measured on the radio** | 35 periods on 40 m: 2.7 s on average, 1.1 s for an empty period. See [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md). |
| `JS8.CFG` keys `scope=`, decoder tuning keys, `JS8PERF.TXT` | **Used on the radio** | The measured run used `DEFAULT.CFG`. The `FAST` and `DEEP` presets of this code were not timed on the radio. |
| Sensitivity, false decodes, behaviour with many signals | **Simulator only** | Generated signals and 19 recordings on a PC. See [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md). |
| Comparison with JS8Call on the same audio | **Not done** | No claim of equal performance is made. |
| Submodes Fast, Turbo, Slow | Not done | Only Normal is decoded. |
| Transmit | Not in this release | See [`ROADMAP.md`](ROADMAP.md). |
| Other IC-705 versions (E, ...) | Not tried | Only the J version was used. |
| IC-9700 | Not supported by this build | The app is packed for the IC-705 only (`--models 1` in `shell/build.sh`). |
| `make fetch` over the network, a build on a second machine | Not verified | See [`docs/BUILDING.md`](docs/BUILDING.md). |

## Photos

None yet. The owner will add photos of the DECODE screen and of the app window; `docs/img/README.md` lists what is planned.

## Requirements

- **Icom IC-705 with the custom firmware by Dawid SQ6EMM, release 20261119v14**, which contains the ICAP app loader. Firmware: <https://github.com/sq6emm/icom-custom-firmware>. SDK: <https://github.com/sq6emm/icap-sdk>. The app is built against SDK commit `3e3af4c` ("SDK update for firmware 20261119v14"). A newer firmware may need a rebuild.
- **Radio versions tried: the J (Japan) version only.**
- **An SD card** in the radio. The app, its dictionary and its files live in `\IC-705\Apps\`.
- The radio's clock must be set accurately. JS8 periods start at :00, :15, :30 and :45.

## Quick install on the SD card

Read [Safety and legal](#safety-and-legal) before installing the custom firmware. This repository ships **no binaries**: you build the app and generate the dictionary yourself ([`docs/BUILDING.md`](docs/BUILDING.md)).

You need `make`, a C compiler, `python3`, `git`, `curl` and an `arm-none-eabi-gcc` bare-metal toolchain (we used Arm GNU Toolchain 12.2.rel1). `make dist` builds the app, so it stops with a message when the ARM compiler is missing.

```
make fetch     # ICAP SDK, seven JS8Call source files and two licence texts into third_party/ (network, once)
make dist      # build/dist/IC-705/Apps/ and build/icap-js8-sdcard.zip
               # if the ARM compiler is not in PATH: make dist CROSS=/path/to/bin/arm-none-eabi-
```

`make fetch` has not been run over the network by us (see the status table); if it fails, `docs/BUILDING.md` says how to supply the files by hand.

1. Copy the content of `build/dist/IC-705/Apps/` to `\IC-705\Apps\` on the radio's SD card with a card reader. That is:
   - `JS8.APP` - the app (59 kB);
   - `JSCHEAD.BIN` and `JSCD000.BIN` .. `JSCD0FF.BIN` - the JS8Call dictionary for receiving, 257 files, 1.95 MB;
   - `JS8.CFG` - optional configuration (the sample from `sd/JS8.CFG`).
2. On the radio: MENU > SET > Apps should list `JS8`.
3. Tune to a JS8 frequency (for example 7.078 or 14.078 MHz dial) and select the mode **JS8**: on the MODE screen press the RTTY button repeatedly; the loader adds the app's mode to the cycle after RTTY and RTTY-R. The radio switches to USB-D, opens the DECODE screen and starts the app. The first decodes appear after the first complete 15 s period.

Things to know:

- **The Apps list shows at most 8 apps** (seen on the radio). Further `.APP` files on the card are silently missing from the list. Remove one if `JS8` does not show up.
- **Many files in the Apps folder slow the app rescan down** (seen on the radio). The dictionary adds 257 files to that folder. Moving it out of the Apps folder is on the roadmap.
- **Without the dictionary the app still runs.** Compressed frames are then shown raw, as `#` and 12 characters, and the app window says `JS8: no dictionary (JSCHEAD.BIN)`. (Simulator.)
- **An incomplete copy of the dictionary is not reported.** If `JSCHEAD.BIN` is there but some `JSCDxxx.BIN` files are missing or damaged, the words from those files appear as `{?}` in the text and in the logs, and the app window still says `no errors`. Check that all 257 files were copied and that line 6 of the app window reads `dict 5740`. (Simulator.)
- **Copying over Wi-Fi instead of a card reader:** the SDK has an app, `FETCH.APP`, that downloads files from an HTTP server of your own. Its source limits a server folder to 64 files (`MAX_FILES` in the SDK's `apps/fetch/fetch.c`), so the dictionary would have to be spread over several folders. This route is not described here; see the SDK.

## Configuration: JS8.CFG

Optional text file in the Apps folder: `key=value` entries separated by blanks or line ends. There is no comment syntax. The app reads the first 399 bytes, once, at start.

The parser is strict and silent: keys are lower case, there must be no blank on either side of `=`, and the file must be plain ASCII without a byte-order mark (some Windows editors add one; it hides the first key). An entry it does not understand is ignored without a note. Lines 0 and 1 of the app window show the call, the band and the tuning that were really applied; look there after changing the file. (Simulator.)

| Key | Meaning | Default |
|---|---|---|
| `scope=450-2550` | read by the **loader**, not by the app: span of the mini-scope for the mode (works on the radio) | - |
| `flo=`, `fhi=` | search range for the audio frequency of the lowest tone, Hz. A wider range works but needs two passes over the band and takes longer; a range narrower than 100 Hz is refused. | 450, 2550 |
| `call=` | own call sign, used when the radio has none set | the radio's |
| `groups=` | group calls marked as "to my group" in the message log, e.g. `groups=@ALLCALL,@OK` | none |
| `popup=0` | no pop-up window for messages addressed to the own call | 1 |
| `keep=` | archive files kept per log, 2..999 | 200 |
| `perf=0` | do not write `JS8PERF.TXT` | 1 |

Decoder tuning keys (out-of-range values are clamped):

| Key | Meaning | Default |
|---|---|---|
| `minscore=0..60` | lowest sync score of a candidate | 10 |
| `nsync=0..21` | Costas tones (of 21) a candidate needs to be tried | 7 |
| `ns34=0..21` | Costas tones needed for the decoding attempts 2, 3 and 4 | 10 |
| `passes=1..31` | bit mask of the decoding attempts per candidate | 31 |
| `iters=5..50` | belief-propagation iterations per attempt (the decoder makes half as many layered sweeps) | 30 |
| `sub=0..2` | further residual passes: re-read the capture, subtract what was decoded, search again | 1 |
| `shift=0\|1` | one residual pass on a time grid shifted by half a row | 1 |
| `wide=0\|1` | 1: residual passes only on evidence of a station under a decoded one; 0: the exhaustive scheme, use with `sub=2` | 1 |
| `stream=0\|1` | build the first waterfall while the period is being captured | 1 |
| `budget=0\|1000..14500` | ms; no residual pass is started that cannot end by then, and the decode ends at that time with what it has (0: only the hard limit, the next capture) | 8000 |
| `maxcand=10..300` | sync candidates kept per search | 300 |

### Presets

Three ready-made settings are in `sd/presets/`. To use one, copy it to the card as `JS8.CFG` (add `scope=450-2550` if you want the scope span set).

| File | Settings | What it is |
|---|---|---|
| `DEFAULT.CFG` | `minscore=10 nsync=7 ns34=10 passes=31 iters=30 sub=1 shift=1 wide=1 stream=1 budget=8000` | the same as no tuning keys at all. **This is the setting that was measured on the radio.** |
| `FAST.CFG` | as DEFAULT with `sub=0 budget=6000` | less work in busy periods. Simulator only. |
| `DEEP.CFG` | `minscore=8 nsync=7 ns34=7 passes=31 iters=30 sub=2 shift=1 wide=0 stream=1 budget=12000` | the exhaustive candidate scheme of the first build, here with the streaming front end and a 12 s budget. Simulator only; expect long decode times on a busy band. |

`sd/JS8.CFG` is a sample: `scope=450-2550`, the default band, `popup=1 keep=200` and the DEFAULT tuning keys.

## Reading the screen

### DECODE rows

Every row is at most 40 characters, because longer rows wrap on the radio when the mini-scope is shown.

Example (simulator run with a generated two-frame message):

```
1200 -10 1500 OK7AB: HELLO
1200 -10 1500 FROM THE SIMULATOR
1200 -10 1500 = OK7AB: HELLO FROM THE
     SIMULATOR
```

| Part | Meaning |
|---|---|
| `1200` | start of the 15 s period, radio clock, hhmm (the logs keep the seconds) |
| `-10` | signal-to-noise ratio in dB, referred to 2500 Hz |
| `1500` | audio frequency of the lowest tone, Hz (JS8Call's "offset") |
| text | the text of this frame; the word `HEARTBEAT` is shortened to `HB` on the screen only |
| `=` before the text | a complete message of more than one frame, joined |
| `>` before the text | a complete message addressed to the own call; it also raises a pop-up (at most one per period, none for a message with a wrong checksum) |
| a row starting with blanks | continuation of a text that did not fit the row |

The SNR is the decoder's own estimate. It has not been compared with what JS8Call reports for the same signal.

### The app window (8 lines of 40 characters, times in ms)

| Line | Example | Content |
|---|---|---|
| 0 | `JS8 RX OK1ABC 450-2550 Hz` | own call, search band |
| 1 | `ms10 n7/10 p31 it30 su1 sh1 w1 s1 b8000` | tuning in use: minscore, nsync/ns34, passes, iters, sub, shift, wide, stream, budget |
| 2 | `no errors` | the last note or error |
| 3 | `last 120030 0d 8ms t0 b0` | last period: frames, decode time, candidates tried, BP runs; `CUT` = ended by the hard limit, `BUD` = ended by `budget=` |
| 4 | `r0 f1 s7 d0 u0 p0` | last period per stage: capture read, FFT, sync search, decoding, subtraction, post (text and logs) |
| 5 | `R0 F2 S8 D1 U0 P1` | running maxima of line 4 |
| 6 | `dict 5740 rd 3 fr 2 msg 1 me 0` | dictionary words in memory, SD reads, frames, messages, messages to me |
| 7 | `c0/3 k0 g0 av9 mx13 T2 B6` | periods ended early (by the hard limit or by `budget=`) / periods, skipped periods, audio gaps, average and maximum decode time, maxima of tried candidates and BP runs; `LOGERR` first after a failed log write |

The examples are from a run in the PC simulator (`docs/SIMULATOR.md`), not from the radio: the format is the same, the times are PC times.

### Files the app writes into the Apps folder

| File | Content |
|---|---|
| `JS8CAPA.PCM`, `JS8CAPB.PCM` | the two capture files (14.6 s of audio each, about 234 kB) |
| `JS8LOG.TXT`, archives `JS8L0001.TXT` ... | every frame: `R <yyyymmddThhmmss> <snr> <dt> <freq> <22 hex digits> <text>`; notes: `N <hhmmss> <text>` |
| `JS8MSG.TXT`, archives `JS8M0001.TXT` ... | every complete message: `M <time> <snr> <freq> <frames> <flags> <text>` |
| `JS8PERF.TXT` | decode times of the last 16 periods and sums since the start, see [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md) |

The logs contain the call signs heard on the air. The archive is a ring of `keep=` files per log. Formats are described at the top of `shell/js8app.c`.

## Limitations and known issues

- **Receive only.** Nothing in this app can key the transmitter.
- **JS8 Normal only.** Fast, Turbo and Slow signals are not decoded.
- **No comparison with JS8Call.** Sensitivity figures are simulator results; nobody has run both on the same audio.
- **One radio.** Everything measured comes from one IC-705 (J version) on one day (23 November 2026) on 40 m.
- **Overlapping stations.** Two stations on nearly the same audio frequency: sometimes only one is read. Messages are joined per audio frequency, so two stations on top of each other can have their texts joined into one message.
- **A period that takes longer than `budget=`** ends with the frames found so far and the note `decode out of time`.
- **8-app limit and slow rescan** of the Apps folder, see above.
- **A message that is being received when the app is left** is closed as incomplete.
- **A full SD card stops decoding.** The app needs about 0.5 MB for its two capture files and, with `keep=200`, up to about 2 MB for log archives. The decoder reads the capture back from the card, so when writes fail nothing is decoded (the app window shows `JS8: SD write error`), and in the simulator the log file that was being rewritten ended up empty. Not tried on the radio.
- **Silent configuration mistakes**, see "Configuration" above.
- **One note is too long for the app window:** `JS8: JS8.CFG band < 100 Hz, using 450-2550` is 42 characters and loses its last two on the 40-character line. The full text is in `JS8LOG.TXT`. To be fixed with the next build.
- **Power cuts.** The log lines of the last period live in memory until the period ends. What a power cut during a write leaves on the card was not tested.

## Safety and legal

- **The custom firmware is not from Icom.** Installing it voids the warranty, and a failed installation can leave the radio unable to start. This project is not the firmware and cannot help with it. Read the firmware project's own instructions and warnings, and decide for yourself.
- **This repository contains no Icom code**, no firmware images and nothing derived from them. It builds against the public ICAP SDK, which you fetch yourself.
- **An ICAP app is not sandboxed in any strong sense.** Install apps only from sources you trust, and that includes this one: it was written with an AI assistant and no human has reviewed it line by line.
- **The app cannot transmit.** It contains no call to the loader's transmit or network services, and the test suite checks the sources for that (`make e2e`; a check of the source text, with or without an ARM compiler).
- **What does change on the radio:** selecting the mode JS8 makes the *loader* switch the radio to USB-D, because the app registers its mode that way. The app itself changes no radio setting (the SDK gives an app no service for setting the frequency or the mode), does not start by itself, and writes only into the Apps folder of the SD card.
- **No warranty.** See `LICENSE`.

## Building and testing

Short version (details in [`docs/BUILDING.md`](docs/BUILDING.md)):

```
make fetch                       # once, network
make test                        # host test suite, generated signals only; needs cc, make, python3
make arm ICAP_SDK=/path/to/icap-sdk CROSS=arm-none-eabi-    # build/arm/JS8.APP
make dist                        # the SD-card file set (builds the app as well)
```

The tests run the same C sources that go into the radio against a simulated loader on the PC ([`docs/SIMULATOR.md`](docs/SIMULATOR.md)). Nothing in the tests transmits or touches a radio. Tests that need off-air recordings print `SKIP` and succeed when there are none; no recordings are shipped.

Layout: `src/` the app modules, `shell/` the loader binding and the ARM build script, `tests/` unit tests, the host simulator and the suites, `tools/` build and analysis tools, `sd/` the sample configuration and the presets, `docs/`.

## Roadmap

See [`ROADMAP.md`](ROADMAP.md): transmit (heartbeat, CQ, directed messages), text entry with the radio's own keyboard, a terminal link, a generic ICAP host simulator if there is interest, a head-to-head comparison with JS8Call, and moving the dictionary out of the Apps folder.

## Reporting problems

Use the issue tracker of this repository. Helpful in a report:

- radio version (E, J, ...) and firmware release;
- which build (commit) and your `JS8.CFG`;
- lines 1, 2, 3 and 7 of the app window;
- `JS8PERF.TXT`, and the `JS8LOG.TXT` of the time in question if you are willing to share it (it contains call signs heard on the air);
- for a decoding problem, a recording (8000 Hz, mono, 16 bit) that shows it is the most useful thing of all.

Problems with the firmware or the loader itself belong to the firmware project, not here.

## How this was made

- **The idea, the direction, the testing on the radio and the photos** are by Tomas Brincil OK1BT. He set the goals and the rules, installed the builds on his own IC-705, ran them on the air and reported what the radio really did.
- **The code, the tests and the documentation were written by Claude**, Anthropic's AI model, working as a coding assistant under his direction in November 2026. Separate runs of the assistant reviewed the work; the reviews found real faults, which were corrected. No human has reviewed the code line by line.
- **It is built on Dawid SQ6EMM's ICAP loader, SDK and FT8 app.** The decoder is a fork of the SDK's FT8 decoder, the app shell reuses the capture loop of his `digiapp.c`, and two of his files are used unmodified. Through the FT8 decoder it also stands on ft8_lib by Kārlis Goba YL3JG.
- **The protocol knowledge comes from JS8Call** by Jordan Sherer KN4CRD and from the JS8Call and JS8Call-improved contributors: frame formats, tables and the dictionary. JS8 in turn stands on FT8 and the work of the WSJT-X team.

Who did what: [`AUTHORS.md`](AUTHORS.md). Whose work it builds on: [`CREDITS.md`](CREDITS.md). None of the people named there has reviewed or endorsed this project.

## Licence

GPL-3.0-or-later for the files written for this project; the project as a whole is conveyed under the GNU General Public License, version 3 - see [`LICENSE`](LICENSE). Which licence applies to which file, why the three files that hold upstream data are GPL-3.0-only, and the notices of the MIT-licensed code this derives from, are in [`NOTICE.md`](NOTICE.md). No warranty.

More documentation: [`docs/BUILDING.md`](docs/BUILDING.md), [`docs/SIMULATOR.md`](docs/SIMULATOR.md), [`docs/PROTOCOL.md`](docs/PROTOCOL.md), [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md), [`ROADMAP.md`](ROADMAP.md), [`CHANGELOG.md`](CHANGELOG.md), [`CONTRIBUTING.md`](CONTRIBUTING.md).
