# The host simulator

`tests/js8sim.c` is a PC program that stands in for the ICAP loader of the radio. The app's own source (`shell/js8app.c` and the modules in `src/`) is compiled for the PC and runs against it. Most test results of this project come from it.

This document is for people who want to understand our test results, and for developers of other ICAP apps who are looking for a way to run an app on a PC. On that question see also [sq6emm/icap-sdk#1](https://github.com/sq6emm/icap-sdk/issues/1) and the roadmap.

The simulator ships as it is: a test harness for this one app.

## What it is not

Stated first, so that nobody is misled:

- **It does not run the ARM binary.** The app is recompiled for the PC. Faults that exist only in the ARM build (compiler differences, relocation, alignment, stack depth inside loader services) are invisible to it. There is no emulator.
- **It does not know how fast the radio is.** It has two models of a slow processor, both arbitrary. Treat decode times derived from the PC as a rough guess; what a timing model predicted and what the radio measured is in `docs/PERFORMANCE.md`.
- **It is a model of the loader written from the SDK's documentation and from a few observations**, not from the loader's source, which we have not seen. Where the real behaviour was not known, the simulator guesses.
- **It is tied to the JS8 app.** See "Using it for another app" below.

Nothing in it transmits or touches a radio.

## How it works

The SDK hands an app one thing: a table of function pointers (`struct icap_svc`) passed to `app_main`. The simulator fills that table with its own functions and calls `app_main`. The app cannot tell the difference at source level. The shell is included into the simulator's source file with `-DDIGI_HOST`, which only switches the freestanding `memcpy`/`memset` for libc's.

| Service | Model |
|---|---|
| Audio in | Samples from a WAV or raw PCM file (8000 Hz, mono, 16 bit) on a virtual clock. Sample 0 sits on a 15 s period edge of the fake clock. Silence before and after the file. A ring of limited size: unread audio beyond it is an overrun, as on the radio. |
| Clock, sleep | Virtual time. A sleep advances the clock without waiting, so an hour of operation runs in seconds. |
| Files | A directory on the PC with the loader's rules as the SDK documents them: names of `A-Z 0-9 _ - .` up to 48 characters, at most 4 files open, no append, no seek, opening for writing empties the file. A read-only overlay directory (`-J`) lets many runs share one copy of the dictionary. |
| Virtual files | Only `@MYCALL` and `@CLOCK` are modelled. The other virtual files of the firmware (`@MYLOC`, `@FREQ`, `@TX`, `@MODECFG`, `@SPOT`, `@QSO` ...) fail here with `ICAP_E_NAME`, which is the answer the SDK documents for firmware without them. The JS8 app opens only `@MYCALL`. |
| DECODE screen, app window, pop-up | Printed to standard output as `D`, `W<n>` / `L<n>` and `U` lines. |
| Stop request | Polled by the app; the simulator ends the run after the file plus a tail (`-T`). |
| Transmit and network services | Present in the table because the SDK's table has them, but they only **count** calls. The summary line must show `tx_calls=0 net_calls=0`, and the test suite checks that. |

### Time and the slow-processor models

- **Accelerated (default).** Sleeps cost no real time. CPU time spent in the app is multiplied by a factor (`-k K`) and added to the virtual clock.
- **Deterministic (`-k 0 -q MS`).** No CPU time is charged. Instead every stop poll made while a decode runs costs `MS` virtual milliseconds. This is exactly repeatable on a loaded machine. It is an arbitrary model: SD-card time is not in it.
- **Real time (`-r`).** Sleeps really sleep. One scenario of the suite runs this way; its timing check can fail on a heavily loaded machine.

### Other switches

Pop-up always busy (`-B`), a small audio ring (`-R`), a smaller data region (`-H`), the app starting late or the file starting off the period edge (`-l`, `-e`). All options are listed at the top of `tests/js8sim.c`.

## One scenario by hand

After `make fetch` (or with `ICAP_SDK` and `JS8_REF` pointing at your own copies):

```
make all sim dict

# 1. a message as frames (js8mk is the test frame builder; -J: the dictionary files)
# (Q7AB is a made-up call sign that cannot be issued; N0CALL below is the usual placeholder)
./build/js8mk -c Q7AB -g JO70 -i -J build/sd "HELLO FROM THE SIMULATOR"
#   ec58a242ad2f170b7f2164 6 1 Q7AB: HELLO
#   f7062700767e14ffff5ae0 6 2 FROM THE SIMULATOR

# 2. two periods of audio: frame 1 in period 0, frame 2 in period 1, 1500 Hz, -10 dB, with noise
./build/js8gen -o build/demo.wav -p 2 -n 300 -S 1 \
    -s 1500,0.1,-10,xec58a242ad2f170b7f2164,0,0 \
    -s 1500,0.1,-10,xf7062700767e14ffff5ae0,0,1

# 3. run the app on it
mkdir -p build/demo-sd
./build/js8sim -d build/demo-sd -J build/sd -c N0CALL -T 20 build/demo.wav
```

Output of that run on our PC (the wall time and the decode times will differ on yours):

```
D 1200 -10 1500 Q7AB: HELLO
D 1200 -10 1500 FROM THE SIMULATOR
D 1200 -10 1500 = Q7AB: HELLO FROM THE
D      SIMULATOR
W0 JS8 RX N0CALL 450-2550 Hz
W1 ms10 n7/10 p31 it30 su1 sh1 w1 s1 b8000
W2 no errors
W3 last 120030 0d 13ms t0 b0
W4 r0 f1 s9 d0 u0 p0
W5 R0 F2 S12 D3 U0 P1
W6 dict 5740 rd 3 fr 2 msg 1 me 0
W7 c0/3 k0 g0 av16 mx21 T2 B6
# sim rc=0 file=build/demo.wav samples=240000 (30.0 s) virtual=51.5 s wall=0.19 s k=1 q=0
# files opens=20 failed=3 max_open=1 still_open=0 nofd=0 badname=0 read=51045 B written=782142 B
# audio calls=6841 overruns=0 app_gaps=0 yields=4 max_step_ms=2.09
# decode_out lines=4 over40=0 popups=0 tx_calls=0 net_calls=0 win_over40=0
# app periods=3 frames=2 msgs=1 to_me=0 skipped=0 notes=0 raw=0 popup_busy=0 log_err=0
# decode ms last=13 avg=16 max=21 (virtual) arena peak=177840 of 189824 (heap 245760, shell 55936)
# dict head_words=5740 head_bytes=34645 opens=3 bytes=38757 max_lookup=34661 cache_hits=0 head_hits=9
```

`build/demo-sd/` then holds what the app would have left on the card: the two capture files, `JS8LOG.TXT`, `JS8MSG.TXT` and `JS8PERF.TXT`. The decode times printed here are PC times and say nothing about the radio.

## How the suites use it

1. A **generator** (`tests/js8gen.c`, with `tests/js8mk.c` for the frames) makes JS8 signals with noise, several stations at once, known messages. This is the only use of the encoder in this repository: it writes WAV files.
2. The **simulator** feeds them to the app; a Python script compares what appeared on the "screen" and in the files with what was sent, exactly (`tests/e2e.py`).
3. **Hostile cases:** corrupt and truncated files, hostile frames, memory and file faults, under AddressSanitizer and UBSan (`tests/run_fuzz.sh`, `tests/fuzz_shell.c`).
4. **Real recordings**, if you have some, go the same way (`make test-recordings RECORDINGS=<dir>`). None are shipped.

The generated-signal tests share a weakness that should be kept in mind: generator and receiver are the same code base, so an error made on both sides cancels out. That is why the encoder and the message packing are also compared with independent Python models built from the JS8Call sources (`tests/model.py`, `tests/rv_protocol.py`), and why the receiver was also run on recordings made with other software.

## Using it for another app

Today this needs editing, and we have not tried it with any other app. What is generic and what is not:

| Generic (about the loader) | Tied to JS8 |
|---|---|
| the service table and its fake functions | the line that includes `shell/js8app.c`, and the build rule |
| virtual clock and the time models | the 15 s period is a constant |
| the directory as SD card, with the file rules and the overlay | the summary lines read the JS8 shell's own counters |
| audio from a file, with the ring and overruns | services our app does not use are minimal or missing in the fake table |

To try it with your app as things stand: copy `tests/js8sim.c`, replace the include of our shell with your app's source (it must be compilable for the host), remove the summary lines that read our shell's variables, and adjust the period constant.

**Planned, if there is interest:** split the generic core into a stand-alone directory with a hello-world example, so that any ICAP app can be run on a PC without touching JS8 code. It is on the [roadmap](../ROADMAP.md). If you would use it, say so in an issue; that decides whether it gets done.

## What the simulator got wrong or could not tell us

- **Speed.** For this release a timing model predicted 2.4 s for a busy recording and 0.2 s for an empty band; the radio measured 2.7 s on average and 1.1 s for an empty period (`docs/PERFORMANCE.md`). PC-derived times are a rough guess.
- **Screen width.** The simulator accepted DECODE rows of up to 63 characters, the limit of the service. On the radio, rows longer than about 40 characters wrapped when the mini-scope was shown (seen with an earlier build, which printed longer rows); the app and the simulator's check were changed to 40.
- **The app list and the rescan.** That the Apps list showed at most 8 apps and that the rescan was slow with about 260 extra files in the folder was seen on the owner's radio only; the simulator has no model of either.
