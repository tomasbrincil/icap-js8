# Building and testing

Nothing here transmits or touches a radio. Every "radio" in the tests is the simulator `tests/js8sim.c`.

## What you need

| For | Tools |
|---|---|
| Host tests | a C compiler (`cc`, or name it with `CC=`; developed with gcc, clang not tried), `make` (GNU Make), `python3` (3.8 or newer, standard library only), a POSIX shell with `grep`, `sed`, `nm`, `cksum`, and `sha256sum` or `shasum` (every `make test` checks the reference files) |
| `make fetch` | `git`, `curl`, network access |
| The fuzz suite (`make fuzz`, part of `make test-full`) | a compiler with `-fsanitize=address,undefined` |
| The app for the radio | an `arm-none-eabi-gcc` bare-metal toolchain (we used Arm GNU Toolchain 12.2.rel1) and a clone of the ICAP SDK |

Two things are needed that this repository does not contain and that you fetch yourself:

1. **The ICAP SDK** by Dawid SQ6EMM, <https://github.com/sq6emm/icap-sdk>. The app was built and tested against commit `3e3af4c7181bde704deb7ce2cdd015d1c5a8e401` ("SDK update for firmware 20261119v14"). From it the build uses `icap.h`, `tools/app.ld`, `tools/icap_start.S`, `tools/mkapp.py`; the host bench also compiles `apps/digi-common/digi_util.c` and, for the FT8 comparison targets only, the SDK's FT8 app.
2. **Seven source files of JS8Call and JS8Call-improved** at pinned commits (about 13 MB). They are read as data only: the tests compare our tables with them, and the dictionary files are generated from one of them. Nothing in them is compiled or run. `make fetch` also downloads the two upstream licence texts (`js8call/COPYING`, `js8call-improved/LICENSE`) for your reference; no test needs them.

## Fetching

```
make fetch
```

clones the SDK into `third_party/icap-sdk` at the pinned commit and downloads the seven JS8Call source files (and the two licence texts) into `third_party/js8call` and `third_party/js8call-improved`. The seven are checked against `tools/refs.sha256`.

If you already have them:

```
make test ICAP_SDK=/path/to/icap-sdk JS8_REF=/path/to/dir-with-js8call-and-js8call-improved
```

`REFS_FROM=<dir> sh tools/fetch_refs.sh js8call` copies the JS8Call files from a local directory with the same layout instead of downloading them.

## Settings

Given on the make command line or in the environment.

| Variable | Meaning | Default |
|---|---|---|
| `BUILD` | build directory; nothing is written into the source tree | `./build` |
| `ICAP_SDK` | your clone of the ICAP SDK | `./third_party/icap-sdk` |
| `JS8_REF` | directory with `js8call/` and `js8call-improved/` | `./third_party` |
| `CROSS` | prefix of the ARM toolchain | `arm-none-eabi-` (from `PATH`) |
| `TC` | alternative to `CROSS`: the toolchain directory that holds `bin/` | - |
| `ARM_GCC` | alternative to `CROSS`: the full path of `arm-none-eabi-gcc` (make targets only) | - |
| `RECORDINGS` | directory with your own recordings, `8k/*.8k.wav` | none |
| `CC`, `PYTHON` | host compiler and Python, used by the Makefile and passed on to the test scripts | `cc`, `python3` |

`make clean` removes only the objects and the first test binaries. To start over, delete the build directory (`rm -rf build`).

Do not run two `make` targets of this tree in parallel: the suites rebuild the same binaries. A checkout path that contains blanks breaks the ARM build script.

## Host tests

```
make test          # generated signals only
make test-full     # plus decoder statistics, kernel bench, profile, sanitizer and fuzz suite
```

`make test` runs, in this order:

| Target | What it checks |
|---|---|
| `check` | LDPC tables, Costas array and alphabet against the JS8Call sources; the encoder against an independent Python model on 1000 random frames; the core and the decoder compile freestanding with no undefined symbol; the spectrum of generated signals |
| `dict` | generates the dictionary files into `build/sd/` |
| `t6` | the message layer, also under AddressSanitizer / UBSan |
| `rv` | an independent Python model of the protocol against the C code |
| `dec`, `t2` | builds the decoder bench; clean loopback sweep over frequency and time offset, must be 100 % |
| `stream` | the streaming front end gives the same frames as the non-streamed decode |
| `e2e` | the receive-only app in the loader simulator on generated scenarios: DECODE rows, pop-ups and log files are compared with what was sent. It checks the sources: the loader's transmit services are called only in `shell/js8app_tx.h` (compiled into `FT8.APP` only), and no source calls a network service. With an ARM compiler it also builds `JS8.APP` and checks its size and its symbols |
| `t7` | the transmit engine against a mock loader, plain and under ASan / UBSan: selection and timing of frames, tap credit, beacon and CQ arming, HB beacon, every loader error, the caps, the cancel and stop rules; the 79 tones of 1000 random frames against the Python model |
| `txe2e` | the RX+TX app in the simulator: what it sends is decoded again by the receiver (loopback), slot timing, the safety cases (zero samples on the air when nothing may go out), both strategies, an accelerated 2-hour soak, ASan runs. With an ARM compiler it builds `JS8.APP` and `FT8.APP`, checks them against the 80 KiB window, checks that `JS8.APP` holds no transmit object or symbol, and that a second build is byte-identical |
| `fixe2e` | the keyboard control at app level: the prefilled keyboard, the wait after the tap, tap credit and red frame, EXIT, the cancel, one waiting message, HB only on a fresh arming, the green frame, taps while armed, the dictionary on the card root |

Without an ARM compiler the ARM part of `e2e` is skipped (`--skip-arm`) and the last line says "host only".

What the extra targets of `make test-full` judge and what they only print:

| Target | Pass / fail on | Printed only |
|---|---|---|
| `false` | any false decode in 1000 noise-only periods or in 200 periods of FT8-only audio | - |
| `apparena` | the clean sweeps must be 100 % | the sensitivity sweeps |
| `abort`, `arena`, `kbench`, `prof`, `ft8time` | nothing: their output goes through a pipe into the log, so even a crash shows only in the log and does not fail `make` | all numbers |
| `t3`, `multi`, `snrcal` | a decoder job that crashes or exits with an error | the decode rates and thresholds: **no limit is asserted**, read them |
| `fuzz` | sanitizer reports, crashes, wrong results under fault injection | - |

So "make test-full: all passed" does not mean the sensitivity figures were compared with anything.

On our machine (8 cores, shared with other jobs) `make test` took about 15 minutes (0.1.0, receive only: 7 minutes; `make test-full` of 0.1.0: 17 minutes). The suites use up to 8 processes. One scenario of `e2e` runs in real time and can fail its timing check on a heavily loaded machine; run it again before suspecting the code.

### Tests that need recordings

No recordings are shipped: the ones the receiver was developed with were made by other people and several may not be redistributed (`CREDITS.md`). The targets `real`, `real-t0`, `align` and part (b) of `e2e` therefore print `SKIP` and **succeed** when there are no recordings. `stream`, `arena` and `prof` run on generated signals only and say so.

With your own recordings (8000 Hz, mono, 16 bit, named `<name>.8k.wav` in a subdirectory `8k/`):

```
make test-recordings RECORDINGS=/path/to/dir
```

runs them through the decoder bench (`build/real/<name>.txt`) and through the streaming comparison. `align` and parts of `arena` and `prof` use one particular recording of our private set by file name; without that file they skip.

If the directory also holds an `expected.json`, the end-to-end comparison in the simulator (part (b) of `tests/e2e.py`) runs as well. That part was written for our private set of 19 recordings and refers to some of its file names; the scripts that produced our `expected.json` describe those recordings and are not part of this release. Read `run_real()` in `tests/e2e.py` for the format; with a different set expect to adapt it.

## The apps for the radio

```
make arm-all ICAP_SDK=/path/to/icap-sdk CROSS=/path/to/toolchain/bin/arm-none-eabi-
```

writes `build/arm/FT8.APP` (receive + transmit, `FLAVOUR=tx sh shell/build.sh`, 75,240 bytes) and `build/arm/JS8.APP` (receive only, `sh shell/build.sh`, 66,140 bytes), with Arm GNU Toolchain 12.2.rel1 and the pinned SDK commit, and prints the section sizes. `make arm-tx` and `make arm` build one of them. In `FT8.APP` the shell and the cold modules are Thumb code (otherwise it does not fit the loader's 80 KiB window); the build fails if the linker inserts an interworking veneer, whose absolute address the loader would not relocate (see the top of `shell/build.sh`).

**Build id.** `FT8.APP` shows a build id at the end of line 0 of its app window. By default `shell/build.sh` sets it to `a65e`, the commit of the development tree this release was exported from; with that id the two files built here are byte-identical to the ones built there. When you build changed sources, set your own: `JS8_BUILD_ID=my1 make arm-tx` (letters, digits, `+` and `-`).

```
make dist
```

needs the ARM toolchain as well (it builds both apps) and assembles the SD-card file set in `build/dist/`, laid out like the card: the 515 dictionary files `JSC*.BIN` for the card root, `IC-705/Apps/` with `FT8.APP` and the sample `JS8.CFG`, `receive-only/JS8.APP`, `presets/`, checksums, a `README.txt` with the licence and source statement, `LICENSE`, `NOTICE.md`, `AUTHORS.md`, `CREDITS.md`; and the same as `build/icap-js8-sdcard.zip`. Installing it is described in the README.

If you pass the dictionary files or the app on to somebody else, the GPL applies to them: see `NOTICE.md`, section 2.3.

## What was verified, and what was not

Verified for release 0.2.0 on 24 November 2026, on one machine (Debian 13, x86-64, gcc 14.2, GNU Make 4.4.1, Python 3.13), from a fresh `git clone` of this repository with an empty environment (`env -i`), the SDK and the JS8Call reference files given by `ICAP_SDK` and `JS8_REF`, and no recording present:

- `make test` passed (receive and transmit suites);
- `make arm-all` produced `FT8.APP` and `JS8.APP` byte-identical to the files built from the development tree at the commit the release was exported from (build id `a65e`), the files whose builds ran on the radio;
- `make dist` wrote the SD-card set (515 dictionary files, both apps) and the archive.

From release 0.1.0 (receive side, not repeated for 0.2.0): `make test-full`, `make test-recordings` with our private set of 19 recordings, `make e2e` without an ARM compiler, and `make fetch` over the network once.

**Not verified:**

- a build on any other machine, operating system or compiler; clang; macOS; Windows;
- any toolchain other than Arm GNU Toolchain 12.2.rel1 for the ARM build;
- `make test-full` and `make fetch` over the network for 0.2.0.

## Other targets

`make help` lists the entry points; the comment at the top of the `Makefile` lists the single test targets (`t3`, `multi`, `false`, `ft8time`, `apparena`, `snrcal`, `abort`, `arena`, `kbench`, `prof`, `fuzz`, ...). Their logs go to `build/logs/`.
