# Contributing

Thank you for looking. The maintainer is one person, Tomas Brincil OK1BT (see `AUTHORS.md`). Reports from real radios are worth more to this project than anything else.

## What to expect

From the maintainer: "This is a hobby project, written by an AI assistant under my direction in my spare time. Bug reports and ideas are welcome and I read them, but please do not expect a fix or a feature on any schedule - many will stay open. If something matters to you, a fork or a pull request is the surest way, and you are very welcome to it."

## What helps most

1. **Reports from a radio.** Which IC-705 version and firmware release, which build, what the app window showed, your `JS8PERF.TXT`. See "Reporting problems" in the README.
2. **Recordings that show a decoding problem.** 8000 Hz, mono, 16 bit. Say whether you allow the recording to be kept as a test case and under which terms; recordings without a clear licence are used locally and never redistributed.
3. **A comparison with JS8Call on the same audio.** Nobody has done it yet.
4. **Corrections to anything we got wrong** about JS8, JS8Call, the loader or other people's work, including credits and licence notices.

## Ground rules for changes

- **Transmit code is safety code.** Every frame needs the operator's own action on the radio (ENT on a text seen whole on the radio's keyboard, the loader's tap credit or the operator's arming; rules at the top of `src/js8tx.h`). A change that adds a way to transmit without it will not be accepted. Only `shell/js8app_tx.h` calls the loader's transmit services; the released apps call no network service, and `make test` checks both. Changes to the transmit path need a test in `tests/t7_tx.c`, `tests/tx_e2e.py` or `tests/fix_e2e.py` that shows what goes on the air and what does not.
- **Say what was tested and how.** "Measured on the radio", "simulator only" and "not tested" are different things, and the documentation keeps them apart. A pull request should say which applies. Numbers need an origin.
- **Run the tests.** `make test` before a pull request; `make test-full` for changes to the decoder. Do not loosen a check to make it pass; if a check is wrong, say why.
- **No recordings, no firmware, no program binaries** in the repository. The only files that are not text are the photos in `docs/img/`; a new photo must be free of metadata (the export check refuses a JPEG with Exif or other extra segments) and must not show more of other people than what they sent on the air. No Icom code and nothing derived from it.
- **Keep the limits in mind.** The app image (code + data + bss) must fit 80 KiB; the data region is 240 KiB. `make arm` prints the sizes.
- **Plain C99, freestanding** for everything that goes into the radio: no libc, no malloc.
- **Documentation in plain English**, without promotional language.
- **Call signs in tests and examples are made up so that they cannot be real:** `N0CALL` where one call is enough, otherwise Q + digit (`Q1ABC`, `Q2XYZ`; never Q + letter, which the packer reads as a 3X call) or a leading zero (`0A1AA`). `OK1BT` is the maintainer's own. Do not add a call sign you heard on the air to a test or a document.

## Licence of contributions

Contributions are accepted under the licence of the file they change (see `NOTICE.md`): `GPL-3.0-or-later` for the files written for this project. Keep existing copyright and licence notices intact. If you bring in code or data from elsewhere, name its source, author and licence in the file and in `NOTICE.md`; it must be compatible with GPL version 3.

By sending a contribution you state that you have the right to submit it under that licence. Add yourself to `AUTHORS.md` under "Contributors" if you wish.

## AI-assisted contributions

They are welcome; most of this repository is one. Say so in the pull request, and say what you checked yourself. The person who submits a change is responsible for it.

## Conduct

Be civil. The people whose work this builds on (see `CREDITS.md`) did not ask to be associated with it; do not take problems with this app to their issue trackers unless the problem is really theirs.
