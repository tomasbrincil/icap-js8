# icap-js8 - a JS8 receiver app for the Icom IC-705 (ICAP)

A JS8Call-compatible **receiver** (submode Normal, 15 s periods) that runs inside the IC-705 as an app for the ICAP loader of the custom firmware by Dawid SQ6EMM. It decodes JS8 frames from the radio's own audio, turns them into text, joins multi-frame messages and shows them on the radio's DECODE screen, with no computer attached.

This release is **receive only**. The app contains no transmit code and no network code. Transmitting is on the [roadmap](ROADMAP.md), not in this repository.

The code, the tests and the documentation were written by an AI coding assistant (Claude, by Anthropic) under the direction of Tomas Brincil OK1BT, who tested it on his radio. No human has reviewed the code line by line. See [How this was made](#how-this-was-made) and [`AUTHORS.md`](AUTHORS.md).

**Read this first:** the project is new (November 2026). It has run on one radio, on one day, on one band, and the only timed run was 35 periods of 15 s - **under 9 minutes on the air**. The table below says what was measured on that radio and what is known only from a PC simulator.

## Status

Date of this status: 23 November 2026.

| Feature | Status | Notes |
|---|---|---|
| Receive JS8 Normal: decode, text on the DECODE screen, multi-frame messages joined | **Tested on the radio** | One IC-705 (J version), firmware 20261119v14, live traffic on 7.078 MHz. |
| Dictionary (JSC) on the SD card for compressed text | **Tested on the radio** | All 257 dictionary files were on the SD card during the on-air runs; dictionary-compressed text was shown as words and the app window showed `dict 5740`. How much of the live text went through the dictionary was not counted. |
| Decode time with the default settings | **Measured on the radio** | One run of 35 periods (under 9 minutes) on 40 m: 2.7 s on average, 1.1 s for an empty period. See [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md). |
| `JS8.CFG` keys `scope=`, decoder tuning keys, `JS8PERF.TXT` | **Used on the radio** | The measured run used `DEFAULT.CFG`. The `FAST` and `DEEP` presets of this code were not timed on the radio. |
| Sensitivity, false decodes, behaviour with many signals | **Simulator only** | Generated signals and 19 recordings on a PC. See [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md). |
| Comparison with JS8Call on the same audio | **Not done** | No claim of equal performance is made. |
| Submodes Fast, Turbo, Slow | Not done | Only Normal is decoded. |
| Transmit | Not in this release | See [`ROADMAP.md`](ROADMAP.md). |
| Other IC-705 versions (E, ...) | Not tried | Only the J version was used. |
| IC-9700 | Not supported by this build | The app is packed for the IC-705 only (`--models 1` in `shell/build.sh`). |
| `make fetch` over the network, a build on a second machine | Not verified | See [`docs/BUILDING.md`](docs/BUILDING.md). |

## Photos

None yet; photos of the DECODE screen and of the app window are to follow. `docs/img/README.md` lists what is planned.

## Requirements

- **Icom IC-705 with the custom firmware by Dawid SQ6EMM, release 20261119v14**, which contains the ICAP app loader. Firmware (the author's patcher page, as his SDK documentation gives it): <https://sq6emm.github.io/icom-custom-firmware/>. SDK: <https://github.com/sq6emm/icap-sdk>. The app is built against SDK commit `3e3af4c` ("SDK update for firmware 20261119v14"). A newer firmware may need a rebuild.
- **Radio versions tried: the J (Japan) version only.**
- **An SD card** in the radio. The app, its dictionary and its files live in `\IC-705\Apps\`.
- **The radio's clock must be accurate, to within about a second.** JS8 periods start at :00, :15, :30 and :45, and the decoder looks for a frame only from about 2.2 s before to 2.4 s after its expected start (`docs/PROTOCOL.md`), so a clock that is off by more than about 2 s loses every decode. The easy way is to let the radio set its time from its GPS receiver; the setting is in the radio's time menu (see the Icom manual; we do not quote a menu label we did not check). The tolerance is taken from the code, it was not measured on the radio.
- **A receive filter that passes the search band.** The app looks for signals between 450 and 2550 Hz of audio (the default; `flo=` / `fhi=`). A narrower filter in the data mode cuts stations off at the edges. Which filter setting the owner's radio had during the test was not recorded, and no filter setting was compared.

## Quick install on the SD card

Read [Safety and legal](#safety-and-legal) before installing the custom firmware. This repository ships **no binaries**: you build the app and generate the dictionary yourself ([`docs/BUILDING.md`](docs/BUILDING.md)).

You need `make`, a C compiler, `python3`, `git`, `curl` and an `arm-none-eabi-gcc` bare-metal toolchain (we used Arm GNU Toolchain 12.2.rel1). `make dist` builds the app, so it stops with a message when the ARM compiler is missing.

```
make fetch     # ICAP SDK, seven JS8Call source files and two licence texts into third_party/ (network, once)
make dist      # build/dist/IC-705/Apps/ and build/icap-js8-sdcard.zip
               # if the ARM compiler is not in PATH: make dist CROSS=/path/to/bin/arm-none-eabi-
```

`make fetch` has not been run over the network by us (see the status table); if it fails, `docs/BUILDING.md` says how to supply the files by hand.

1. **Make room in the app list first.** On the owner's radio the Apps list showed at most 8 apps; further `.APP` files on the card did not appear in it. The SDK's app table alone names nine apps (Q65, FT8, JT4, PI4, WSPR, FETCH, CLOCK, SPEED, SYSINFO) and the firmware brings `APRS.APP`, so a card that holds the whole bundle may have no free place and `JS8` may not show up. If that happens:
   - **move** the `.APP` file of one or two apps you do not use from `\IC-705\Apps\` to your computer, so that you can put them back later;
   - touch only files whose name ends in `.APP` and that you know to be apps. **Leave every other file of the firmware release where it is**: the SDK says that parts of the loader runtime are files in this same folder and must match the firmware image. Leave `APRS.APP` alone if you use the firmware's APRS mode, which starts it by itself.

   What we do not know: which apps were on the owner's card (it was not recorded), by which rule the list picks its 8 entries, and whether an app that is missing from the list still gets its mode. We saw the limit; we did not examine it.
2. Copy the content of `build/dist/IC-705/Apps/` to `\IC-705\Apps\` on the radio's SD card with a card reader. These are the files of this app, 259 in all:
   - `JS8.APP` - the app (59 kB);
   - `JSCHEAD.BIN` and `JSCD000.BIN` .. `JSCD0FF.BIN` - the JS8Call dictionary for receiving, 257 files, 1.95 MB;
   - `JS8.CFG` - optional configuration (the sample from `sd/JS8.CFG`).
3. On the radio, open the app list: MENU, then the Apps entry that the custom firmware adds (the SDK documentation gives the path as MENU > SET > Apps). It should list `JS8`. You do not have to start the app there.
4. Tune to a JS8 frequency (for example 7.078 or 14.078 MHz dial) and select the mode **JS8**: press the RTTY mode button repeatedly; it cycles RTTY, RTTY-R, the decoder modes of the installed apps and among them JS8. The radio switches to USB-D, shows the DECODE screen and starts the app. The first decodes appear after the first complete 15 s period.
5. To stop the app, select another mode (the SDK: leaving the mode stops the app). To look at its status window, see [The app window](#the-app-window-8-lines-of-40-characters-times-in-ms).

**Uninstall:** select another mode, then delete from `\IC-705\Apps\` the files of this app and the files it wrote: `JS8.APP`, `JS8.CFG`, `JSCHEAD.BIN`, `JSCD000.BIN` .. `JSCD0FF.BIN`, `JS8CAPA.PCM`, `JS8CAPB.PCM`, `JS8LOG.TXT`, `JS8MSG.TXT`, `JS8PERF.TXT` and the archives `JS8L0001.TXT` ..., `JS8M0001.TXT` ... (in short `JS8.APP`, `JS8.CFG`, `JSC*.BIN`, `JS8*.PCM`, `JS8*.TXT`). The app writes nowhere else and changes no setting of the radio. Put back any `.APP` file you moved away in step 1.

Things to know:

- **The app rescan was noticeably slow with the dictionary in the Apps folder** (seen on the owner's radio, with about 260 extra files there). Moving the dictionary out of the Apps folder is on the roadmap.
- **Without the dictionary the app still runs.** Compressed frames are then shown raw, as `#` and 12 characters, and the app window says `JS8: no dictionary (JSCHEAD.BIN)`. (Simulator.)
- **An incomplete copy of the dictionary is not reported.** If `JSCHEAD.BIN` is there but some `JSCDxxx.BIN` files are missing or damaged, the words from those files appear as `{?}` in the text and in the logs, and the app window still says `no errors`. Check that all 257 files were copied and that line 6 of the app window reads `dict 5740`. (Simulator.)
- **Copying over Wi-Fi instead of a card reader:** the SDK has an app, `FETCH.APP`, that downloads files from an HTTP server of your own. Its source limits a server folder to 64 files (`MAX_FILES` in the SDK's `apps/fetch/fetch.c`), so the dictionary would have to be spread over several folders. This route is not described here; see the SDK.

## Configuration: JS8.CFG

Optional text file in the Apps folder: `key=value` entries separated by blanks or line ends. There is no comment syntax. The app reads the first 399 bytes, once, at start. Put `scope=` on a line of its own: that key is read by the loader, the SDK describes `.CFG` files as one `key=value` per line, and a line of its own is the only form that was tried on the radio.

The parser is strict and silent: keys are lower case, there must be no blank on either side of `=`, and the file must be plain ASCII without a byte-order mark (some Windows editors add one; it hides the first key). An entry it does not understand is ignored without a note. Lines 0 and 1 of the app window show the call, the band and the tuning that were really applied; look there after changing the file. (Simulator.)

| Key | Meaning | Default |
|---|---|---|
| `scope=450-2550` | read by the **loader**, not by the app: span of the mini-scope for the mode. Worked on the radio as a line of its own, as in `sd/JS8.CFG`; no other form was tried. | - |
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

Three ready-made settings are in `sd/presets/`. To use one, copy it to the card as `JS8.CFG`. If you want the scope span set, add `scope=450-2550` as a line of its own, as in `sd/JS8.CFG`.

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

The DECODE screen shows decodes only; the app's status is in its own window. To open it while the mode JS8 is active: MENU, then the Apps entry that the custom firmware adds, then tap `JS8` in the list. (The SDK documentation gives the path as MENU > SET > Apps; we describe the entry instead of quoting a label we did not write down.) EXIT hides the window again and the app keeps running: it is packed as a background app, for which the SDK describes exactly that. To stop the app, select another mode.

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
- **8 apps in the list and a slow rescan** of the Apps folder, as seen on one radio; see "Quick install".
- **Firmware features of a decoder mode that this app does not use.** The firmware offers every decoder mode more than this app takes up. The following comes from the app's source code and the SDK documentation; what the radio shows when you try was not recorded.
  - *No PSKReporter spots.* The app never writes the firmware's `@SPOT` file, so nothing is reported, whatever PSKREPORT is set to. The SDK's own decoders do report.
  - *DECODE > SET is ignored.* The app does not read `@MODECFG`, where the firmware hands over the settings made there (SUBMODE, PSKREPORT, a search range). Use `JS8.CFG`.
  - *Arming the beacon does nothing.* A long press of the mode tile arms the firmware's beacon for a decoder mode (the SDK: a red frame on the emission button). This app has no transmit code and never asks for a transmission, so nothing is sent.
  - *Tapping a decoded row does nothing.* The QSO mode of the SDK's FT8, Q65 and JT4 apps starts from such a tap; this app does not read the taps.
- **SD card writes.** Every 15 s period is stored on the card before it is decoded (as the SDK's reference decoders do): one capture file of about 234 kB per period, which is about 56 MB per hour or 1.3 GB per day of listening, written over the same two files again and again, plus `JS8PERF.TXT` and the logs. `perf=0` saves one small file write per period. The figures are arithmetic from the capture size, not a measurement; keep them in mind before leaving the app on for days.
- **A message that is being received when the app is left** is closed as incomplete.
- **A full SD card stops decoding.** The app needs about 0.5 MB for its two capture files and, with `keep=200`, up to about 2 MB for log archives. The decoder reads the capture back from the card, so when writes fail nothing is decoded (the app window shows `JS8: SD write error`), and in the simulator the log file that was being rewritten ended up empty. Not tried on the radio.
- **Silent configuration mistakes**, see "Configuration" above.
- **One note is too long for the app window:** `JS8: JS8.CFG band < 100 Hz, using 450-2550` is 42 characters and loses its last two on the 40-character line. The full text is in `JS8LOG.TXT`. To be fixed with the next build.
- **Power cuts.** The log lines of the last period live in memory until the period ends. What a power cut during a write leaves on the card was not tested.

## Safety and legal

- **The custom firmware is not from Icom.** Its author states that modifying radio firmware is done at your own risk and your own responsibility for complying with local regulations and your licence; do not expect Icom to support a modified radio. This project is not the firmware and cannot help with it. Read the firmware project's own instructions first, and decide for yourself.
- **This repository contains no Icom code**, no firmware images and nothing derived from them. It builds against the public ICAP SDK, which you fetch yourself.
- **Install apps only from sources you trust, and that includes this one.** The SDK describes the app task as sandboxed and of the lowest priority, says that a fault in an app ends only the app, and also says that an app which breaks its rules can freeze the radio. We have not tested what the loader prevents. This app was written with an AI assistant and no human has reviewed it line by line.
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
- lines 1, 2, 3 and 7 of the app window (how to open it: "The app window" above);
- `JS8PERF.TXT`, and the `JS8LOG.TXT` of the time in question if you are willing to share it (it contains call signs heard on the air);
- for a decoding problem, a recording (8000 Hz, mono, 16 bit) that shows it is the most useful thing of all.

Problems with the firmware or the loader itself belong to the firmware project, not here.

## How this was made

- **The idea, the direction and the testing on the radio** are by Tomas Brincil OK1BT. He set the goals and the rules, installed the builds on his own IC-705, ran them on the air and reported what the radio really did.
- **The code, the tests and the documentation were written by Claude**, Anthropic's AI model, working as a coding assistant under his direction in November 2026. Separate runs of the assistant reviewed the work; the reviews found real faults, which were corrected. No human has reviewed the code line by line.
- **It is built on Dawid SQ6EMM's ICAP loader, SDK and FT8 app.** The decoder is a fork of the SDK's FT8 decoder, the app shell reuses the capture loop of his `digiapp.c`, and two of his files are used unmodified. Through the FT8 decoder it also stands on ft8_lib by Kārlis Goba YL3JG.
- **The protocol knowledge comes from the published sources of JS8Call** by Jordan Sherer KN4CRD and its contributors, and of the JS8Call-improved project: frame formats, tables and the dictionary. JS8 in turn stands on FT8 and the work of the WSJT-X team.

Who did what: [`AUTHORS.md`](AUTHORS.md). Whose work it builds on: [`CREDITS.md`](CREDITS.md). None of the people named there has reviewed or endorsed this project.

## Licence

GPL-3.0-or-later for the files written for this project; the project as a whole is conveyed under the GNU General Public License, version 3 - see [`LICENSE`](LICENSE). Which licence applies to which file, why the three files that hold upstream data are GPL-3.0-only, and the notices of the MIT-licensed code this derives from, are in [`NOTICE.md`](NOTICE.md). No warranty.

More documentation: [`docs/BUILDING.md`](docs/BUILDING.md), [`docs/SIMULATOR.md`](docs/SIMULATOR.md), [`docs/PROTOCOL.md`](docs/PROTOCOL.md), [`docs/PERFORMANCE.md`](docs/PERFORMANCE.md), [`ROADMAP.md`](ROADMAP.md), [`CHANGELOG.md`](CHANGELOG.md), [`CONTRIBUTING.md`](CONTRIBUTING.md).
