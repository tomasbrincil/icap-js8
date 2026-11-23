## How to read this report (hand-written, tests/e2e_notes.md)

**What is tested.** `shell/js8app.c` is the file that is linked into `JS8.APP`; the simulator includes it
unchanged (`-DDIGI_HOST` only removes the app's own memcpy/memset). Decoder, message layer, reassembly and
dictionary access are the same C files in both builds. The ARM binary itself is NOT executed by this
suite (no emulator).

**Nothing here says how fast the radio is.** There is no timing check in this report. Decode times printed
by the simulator are PC times or times of an arbitrary model. Times measured on a radio are in
`docs/PERFORMANCE.md`. The simulator has two models of a slow processor:

- accelerated (`-k K`): CPU time spent in the app counts K-fold;
- deterministic (`-k 0 -q MS`): MS virtual milliseconds are charged for every abort poll the decoder
  makes. It is used to exercise the paths "decode overlaps the next capture" (`-q 3`), "decode cut off,
  frames found so far kept" (`-q 40`) and "decode cut off before anything was found, frames lost"
  (`-q 60`). SD-card time is not in it.

**Deterministic runs.** The real-recording runs and most scenarios use `-k 0`: no CPU time is charged, the
capture starts exactly on the period edge, and a re-run gives the same frames. The older scenario runs of
the "mix" file use the CPU-time clock (`-k 1`) on purpose, as does the real-time run; on a heavily loaded
machine the real-time run can fail its timing check.

**Part (b), real recordings**, runs only when a directory with recordings and an `expected.json` is given
(`make e2e RECORDINGS=<dir>`). No recordings are shipped; without them part (b) is skipped and says so.

**What the checks cannot show.**

- The scenario frames come from the project's own packer (`js8pack`), except the "control characters"
  scenario (independent packer of `tests/rv_protocol.py`). An error shared by packer and unpacker would not
  show in the scenarios; the independent evidence is real recordings and `tests/rv_protocol.py`.
- No JS8Call program was run at any point.
- Arena figures in the simulator output are x86-64 struct sizes; the section "Radio build" prints the ARM
  struct sizes and the arena that follows from them.

**Design points the checks rely on.**

- *Append*: the ICAP file API has no append and no seek. The tail of each log lives in RAM and the file is
  rewritten once per period that added lines; a full tail goes out as a numbered archive file
  (`JS8L0001.TXT ...`, `JS8M0001.TXT ...`). The archive is a ring of `keep=` files per log.
- *DECODE line*: `HHMM sNN FFFF TEXT`, at most 40 characters (longer rows wrap on the radio when the
  mini-scope is shown; `HEARTBEAT` is shown as `HB` on the screen only). Complete messages of more than one
  frame get their own line with `=` after the prefix, messages to MYCALL `>`; long text continues on
  indented lines.
- *Default band 450..2550 Hz*: the widest range the decoder covers in one pass in the arena left after the
  shell's reserve. A wider range in `JS8.CFG` works (two passes); a range narrower than 100 Hz is refused
  with a note and the default is used.
- *Dictionary head in RAM*: all of `JSCHEAD.BIN` (5740 words), so that common words never touch the SD
  card; rarer words open one `JSCDxxx.BIN` each (16-word LRU).
- *Logs and popup are printable*: every byte outside 32 .. 126 that a frame or a dictionary word may carry
  is shown as a blank or `?`, so a received frame cannot start a new log line.
- *Popup*: at most one per period, none for a message with a wrong checksum.
- No beacon, no QSO mode, no network link in this app.
