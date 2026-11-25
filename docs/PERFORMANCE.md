# Performance

Two kinds of numbers are in this file and they must not be mixed:

- **Measured on the radio**: one Icom IC-705 (J version), firmware 20261119v14, 23 November 2026, live traffic on 7.078 MHz (40 m). These are the only numbers that say what the radio does.
- **Simulator results**: the same C code compiled for a PC, fed with generated signals and with recordings. They say what the algorithm does. They say nothing reliable about the radio's speed.

The numbers are those of release 0.1.0 (receive only). The decoder of 0.2.0 and 0.2.1 is the same source (`src/js8dec.c` unchanged); the transmit app `FT8.APP` builds its shell as Thumb code, and its decode times were not measured separately.

No side-by-side comparison with JS8Call has been made. Nothing here is a claim that this receiver hears what JS8Call hears.

## 1. Measured on the radio

A 15 s period leaves about 14.6 s to decode the previous one; a decode that is not finished when the next capture ends is cut.

| Build | Settings | Decode time per period |
|---|---|---|
| First build | exhaustive decoding, no tuning keys | 7.7 to 9.8 s on average; cut at 15 s in busy periods |
| Instrumented build | its reduced FAST preset | about 4 s on average |
| **This release** (streaming front end, faster kernels) | `DEFAULT.CFG`: `minscore=10 nsync=7 ns34=10 passes=31 iters=30 sub=1 shift=1 wide=1 stream=1 budget=8000` | **2.7 s on average over one run of 35 periods (under 9 minutes); 1.1 s for an empty period; 8.0 s once; 0 periods cut; 0 audio gaps; no errors** |

Notes on the last row:

- The single 8.0 s period sits at the `budget=8000` limit. It was not counted as ended early: the counter `c` stayed 0 and no note was shown, and `c` includes periods ended by `budget=`. By our reading of the code the decode therefore finished on its own just under the limit, after the budget had kept further residual passes from starting. It was most likely the first period after the app started, when the first waterfall cannot be built during the capture; that explanation is an inference, not a measurement.
- "0 periods cut" means: no period ended early, neither by the hard limit (the next capture) nor by `budget=`.
- First live decodes on 7.078 MHz, including multi-frame messages joined on the DECODE screen, were seen on the same day. The 257 dictionary files were on the SD card during the on-air runs; dictionary-compressed text was shown as words, and the app window showed `dict 5740`.
- The first two rows are earlier builds that are not in this repository. They are listed to show where the time went; the FAST preset of that instrumented build is not the `FAST.CFG` of this release.
- `FAST.CFG` and `DEEP.CFG` of this release have **not** been timed on the radio.
- One radio, one band, one day (23 November 2026), one timed run of 35 periods of 15 s: under 9 minutes. How busy the band was in each period was not recorded.
- Comments in the sources (`src/js8dec.c`, `src/js8dec.h`, `tests/js8synth.h`) quote times of single decoder stages on the radio. Those are development notes from the two earlier builds, taken once on the same day to decide where to save time and to calibrate the timing model of section 2. They are not part of the measurements published here.

Seen on the same radio, and not examined further: the Apps list showed at most 8 apps; the app rescan was noticeably slow with about 260 extra files in the Apps folder; a `scope=450-2550` line of its own in `JS8.CFG` set the mini-scope span (this app does not parse that key; that the loader or the firmware reads it is our inference); DECODE rows longer than about 40 characters wrapped when the mini-scope was shown (with an earlier build, which printed longer rows).

## 2. Simulator results (PC)

All of the following was run on a PC with the decoder's arena and band as in the app (189,824 bytes, 450 to 2550 Hz).

| Test | DEFAULT | FAST | DEEP |
|---|---|---|---|
| Sensitivity in white noise, ideal signal: 50 % / 90 % of frames decoded (dB in 2500 Hz, 400 trials per dB) | -19.7 / -18.7 | -19.7 / -18.7 | -19.7 / -18.7 |
| Transmitter that keys late (first 23 symbols missing) | -17.5 / -16.2 | -17.5 / -16.2 | -17.6 / -16.5 |
| Clean loopback sweep over frequency and time offset | 1539 of 1539 | | |
| 20 signals in one period, -18 to +5 dB (4000 frames) | 3966 | 3959 | 3966 |
| 19 recordings: frames found | 136 | 136 | 136 |
| False decodes in 1000 periods of noise | 0 | 0 | 0 |
| False decodes in 200 periods of FT8-only audio (4000 FT8 signals) | 0 | | |

About the recordings: they are 19 files of JS8 audio made by other people (sources and their terms are in `CREDITS.md`). They are **private test material and are not in this repository**; several have no licence that would allow redistribution. "136" is the number of frames the exhaustive settings find in them; all three presets find the same 136. It is not a count of what was really transmitted, and JS8Call was not run on them.

The sensitivity row is from development runs with 400 trials per dB, which no make target repeats. The shipped `make apparena` uses 104 trials per dB; in the check of this release it printed -19.7 / -18.6 for the app's band.

The white-noise figures are for an ideal signal in Gaussian noise. Real channels fade and drift; expect worse on the air.

### What cannot be taken from the simulator: speed

For the decoder of this release a timing model calibrated on stage times of the earlier builds on the radio predicted about 2.4 s on a busy recording and 0.2 s for an empty band; the radio measured 2.7 s on average and 1.1 s empty. Treat any PC-derived time as a rough guess.

For orientation only, PC times of the decoder alone (one core of an x86 PC, default settings): about 5 ms for an empty band, 28 ms for a busy recording, 70 ms for 20 signals in a period.

### Size and memory (ARM build, from the build tools, not measured on the radio)

| Item | Value |
|---|---|
| `JS8.APP` (0.2.1, receive only) | 66,064 bytes; load size + bss 70,056 of 81,920 bytes |
| `FT8.APP` (0.2.1, receive + transmit) | 75,144 bytes; load size + bss 79,448 of 81,920 bytes |
| `JS8.APP` of 0.1.0 | 59,344 bytes; load size + bss 64,128 of 81,920 bytes |
| Decoder arena: peak / size | 177,840 / 189,824 bytes (PC run with the app's layout) |
| Stack requested | 16 KiB |

## 3. JS8PERF.TXT

With `perf=1` (the default) the app rewrites `JS8PERF.TXT` in the Apps folder after every period: three summary lines (`JS8PERF`, `S`, `X`) and one `P` line for each of the last 16 periods. All times are milliseconds on the radio's clock. This file is how the numbers in section 1 were taken; if you report a problem, attach it.

```
JS8PERF 1 <app window line 0> | <app window line 1> arena <peak>/<size>
S n<periods> c<cut> ...
X T.. r.. f.. s.. d.. u.. p.. t.. b..
P <hhmmss> d<frames> <-|D|B> T<decode> r<read> f<FFT> s<search> d<decoding> u<subtract> p<post> t<tried> b<BP runs> o<poll> i<BP iterations> w<normal>/<shifted> g<signals>
```

For example, from a **simulator** run of two generated frames (the times are PC times, far below what the radio needs; the long `S` line is left out):

```
JS8PERF 1 JS8 RX N0CALL 450-2550 Hz | ms10 n7/10 p31 it30 su1 sh1 w1 s1 b8000 arena 177840/189824
X T21 r0 f2 s12 d3 u0 p1 t2 b6
P 120000 d1 - T21 r0 f2 s12 d3 u0 p1 t2 b6 o4 i76 w1/0 g1
P 120015 d1 - T16 r0 f2 s12 d0 u0 p1 t1 b1 o2 i1 w1/0 g1
P 120030 d0 - T8 r0 f1 s7 d0 u0 p0 t0 b0 o0 i0 w1/0 g0
```

| Line | Meaning |
|---|---|
| `P` | one period. `d` frames decoded; `-` complete, `D` cut by the hard limit, `B` ended by `budget=`; `T` total decode time; then the stage times: `r` reading the capture back from the card, `f` window and FFT, `s` sync search, `d` soft bits and belief propagation, `u` subtraction, `p` post-processing (text, dictionary, logs); `t` candidates tried, `b` BP runs, `o` time in the abort poll (the capture of the next period runs there), `i` BP iterations, `w` waterfalls built (normal / shifted time grid), `g` signals |
| `X` | the maxima of the `P` fields since the start |
| `S` | sums since the start: `n` periods, `c` periods ended early (by the hard limit or by `budget=`), then counters and stage times with two-letter names. They are listed at the top of `shell/js8app.c` |

With `stream=1` the first waterfall is built during the capture, so its FFT time is not part of `T`.

To get an average decode time: take `T` of the `P` lines, or `T` of the `S` line divided by `n`. The app window shows the same as `av` and `mx` in line 7.
