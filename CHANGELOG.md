# Changelog

## 0.1.0 - first public release: receiver only

State of 23 November 2026.

**What is in it**

- `JS8.APP`, a receive-only app for the Icom IC-705 with the ICAP loader (custom firmware by Dawid SQ6EMM, release 20261119v14): JS8 submode Normal, text on the DECODE screen in rows of at most 40 characters, multi-frame messages joined, JSC dictionary on the SD card, frame and message logs, `JS8PERF.TXT` with decode times.
- The decoder with a streaming front end (the first waterfall is built while the period is captured), faster FFT and LDPC kernels, residual passes made only on evidence, and a time budget per period.
- `JS8.CFG` tuning keys and three presets (`sd/presets/DEFAULT.CFG`, `FAST.CFG`, `DEEP.CFG`).
- The host simulator of the loader (`tests/js8sim.c`), a test signal generator and the test suites.
- Tools: dictionary generator, fetch script for the SDK and the JS8Call reference files, SD-card file set.

**Measured on the radio** (one IC-705 J, 7.078 MHz, 23 November 2026, default settings, one run of 35 periods = under 9 minutes): decode time 2.7 s on average, 1.1 s for an empty period, 8.0 s once, no period cut, no audio gaps, no errors. Details and the history of earlier builds: `docs/PERFORMANCE.md`.

**Not in it**

- No transmit code, no terminal link, no network code. Transmit is under review and not published, see `ROADMAP.md`.
- No Icom firmware or anything derived from it. No recordings. No binaries, no dictionary files (you generate them).

**Known issues**

- Only submode Normal. No comparison with JS8Call yet.
- The Apps list of the test radio showed at most 8 apps, and the app rescan was noticeably slow with the dictionary's 257 files in the Apps folder.
- The firmware's PSKReporter spotting, DECODE > SET, beacon arming and row taps are not used by this app.
- See "Limitations and known issues" in the README.
