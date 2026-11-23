# Contributing

Thank you for looking. This is a small hobby project maintained by one person, Tomas Brincil OK1BT, with the code written by an AI coding assistant under his direction (see `AUTHORS.md`). Reports from real radios are worth more to it than anything else.

## What helps most

1. **Reports from a radio.** Which IC-705 version and firmware release, which build, what the app window showed, your `JS8PERF.TXT`. See "Reporting problems" in the README.
2. **Recordings that show a decoding problem.** 8000 Hz, mono, 16 bit. Say whether you allow the recording to be kept as a test case and under which terms; recordings without a clear licence are used locally and never redistributed.
3. **A comparison with JS8Call on the same audio.** Nobody has done it yet.
4. **Corrections to anything we got wrong** about JS8, JS8Call, the loader or other people's work, including credits and licence notices.

## Ground rules for changes

- **This repository is a receiver.** The app must stay free of any call to the loader's transmit or network services; the tests check the sources for this. Transmit work is not accepted here until the maintainer publishes the transmit engine (see `ROADMAP.md`). The rule for that work is fixed: nothing transmits unless the owner has armed the beacon on the radio and a human has requested exactly that message.
- **Say what was tested and how.** "Measured on the radio", "simulator only" and "not tested" are different things, and the documentation keeps them apart. A pull request should say which applies. Numbers need an origin.
- **Run the tests.** `make test` before a pull request; `make test-full` for changes to the decoder. Do not loosen a check to make it pass; if a check is wrong, say why.
- **No recordings, no firmware, no binaries** in the repository. No Icom code and nothing derived from it.
- **Keep the limits in mind.** The app image (code + data + bss) must fit 80 KiB; the data region is 240 KiB. `make arm` prints the sizes.
- **Plain C99, freestanding** for everything that goes into the radio: no libc, no malloc.
- **Documentation in plain English**, without promotional language.

## Licence of contributions

Contributions are accepted under the licence of the file they change (see `NOTICE.md`): `GPL-3.0-or-later` for the files written for this project. Keep existing copyright and licence notices intact. If you bring in code or data from elsewhere, name its source, author and licence in the file and in `NOTICE.md`; it must be compatible with GPL version 3.

By sending a contribution you state that you have the right to submit it under that licence. Add yourself to `AUTHORS.md` under "Contributors" if you wish.

## AI-assisted contributions

They are welcome; most of this repository is one. Say so in the pull request, and say what you checked yourself. The person who submits a change is responsible for it.

## Conduct

Be civil. The people whose work this builds on (see `CREDITS.md`) did not ask to be associated with it; do not take problems with this app to their issue trackers unless the problem is really theirs.
