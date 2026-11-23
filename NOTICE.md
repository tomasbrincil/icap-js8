# NOTICE - licences and third-party material

This file states which licence applies to what in this repository, and reproduces
the copyright notices and licence texts that must travel with the code.
It was compiled on 2026-11-23 from the upstream LICENSE/COPYING files and source
headers named below. It is a careful reading by non-lawyers, not legal advice.

## 1. Licence of this project

- The project as a whole, and every binary built from it (`JS8.APP`), is
  distributed under the **GNU General Public License, version 3** - full text in
  [`LICENSE`](LICENSE).
- The files written for this project carry `SPDX-License-Identifier: GPL-3.0-or-later`:
  for *our own* contribution you may also choose any later version of the GPL.
- Not everything we build on allows "or any later version" (see section 2, JS8Call
  and WSJT-X). The files that hold upstream material copied as data are therefore
  marked `GPL-3.0-only`: `src/js8_ldpc_174_87_tables.h` (LDPC tables) and
  `src/js8_constants.c`, `src/js8_constants.h` (Costas arrays, alphabet, frame geometry).
- Files in which upstream *logic* was re-implemented from reading those sources
  (`js8crc`, `js8enc`, `js8asm`, the JS8 parts of `js8dec`) contain no copied
  upstream text and are marked `GPL-3.0-or-later` as our own writing. Whether a
  re-implementation is a derivative work of the source it was written from is a
  legal question we cannot settle; if it is, the upstream terms in section 2.3
  (version 3, no "or later") govern those files as well.
- The combined work is consequently offered under GPL **version 3**; do not assume
  that a later version can be applied to the whole.
- Some files derive from MIT-licensed code. Their MIT notices are kept in the files
  and repeated in section 3; the combination is still conveyed under the GPL.
  `src/js8dec.c` and `src/js8dec.h`, a fork of MIT code, are marked
  `GPL-3.0-or-later AND MIT`.

Copyright (C) 2026 Tomas Brincil OK1BT, for the new code in this repository, to the
extent copyright subsists in it. The code was written by Claude, Anthropic's AI
model, working as a coding agent under Tomas Brincil's direction (see
[`AUTHORS.md`](AUTHORS.md)). How far copyright protects AI-written code differs
between jurisdictions; whatever rights exist are licensed as stated above.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

## 2. What this project derives from

### 2.1 ICAP SDK - Dawid SQ6EMM

<https://github.com/sq6emm/icap-sdk>, checkout used: commit `3e3af4c` (2026-11-20).
Copyright (c) 2026 Dawid SQ6EMM.

| Our file | SDK origin | SDK licence (as stated in the SDK) |
|---|---|---|
| `src/js8dec.c`, `src/js8dec.h` | fork of `apps/ft8/ft8dec.c`, `ft8dec.h` | MIT (SPDX header; `apps/ft8/README.md`: "The MIT parts (`ft8dec.c/.h`, `ft8/`) stay MIT") |
| `shell/js8app.c` | capture state machine, SD sample source and abort poll copied from `apps/digi-common/digiapp.c`; the two-slot scheduling (a finished capture is decoded while the next one is recorded) follows `apps/wspr/wsprapp.c` | GPL-3.0-or-later (SPDX headers of both files) |
| `shell/digi_util.c`, `shell/digi_util.h` | unmodified copies of `apps/digi-common/digi_util.c/.h` | GPL-3.0-or-later (SPDX header) |
| (not copied; compiled from an SDK checkout) `icap.h` | SDK root | MIT (header: "Copyright (c) 2026 Dawid SQ6EMM. SPDX-License-Identifier: MIT") |
| (not copied; linked into every `.APP`) `tools/icap_start.S`, `tools/app.ld`; packer `tools/mkapp.py` | SDK `tools/` | `mkapp.py` states "MIT license (see LICENSE)"; `icap_start.S` and `app.ld` carry no licence line - we treat them as MIT like the rest of the SDK (not confirmed by the author) |
| (not copied; compiled from an SDK checkout into the PC reference bench `tests/ft8ref.c` only, never into the app) the SDK's FT8 app `apps/ft8/**`, `apps/digi-common/**` | SDK | GPL-3.0-or-later as a whole, with MIT parts (see below) |

The SDK's FT8 app as a whole and `apps/digi-common/` as a whole are
GPL-3.0-or-later (`apps/ft8/README.md`, `apps/digi-common/README.md`).

### 2.2 ft8_lib - Karlis Goba YL3JG

<https://github.com/kgoba/ft8_lib>, MIT, Copyright (c) 2018 Kārlis Goba.
The SDK's `ft8dec.c` "carries adapted copies of decode.c's ft8_sync_score /
ftx_find_candidates ... and ldpc.c's bp_decode" (SDK `apps/ft8/ft8/UPSTREAM`,
ft8_lib commit `9fec6ca`). Through the fork these parts are in our `src/js8dec.c`
(candidate search, sync score, belief propagation, `fast_tanh` / `fast_atanh`).
The layout of `src/js8_ldpc_174_87_tables.h` mirrors ft8_lib's `constants.c`.
No ft8_lib file is copied into this repository unmodified.

### 2.3 JS8Call and JS8Call-improved

<https://github.com/js8call/js8call> (commit `a7ff1be`, branch `main`, 2025-12-05) and
<https://github.com/JS8Call-improved/JS8Call-improved>.

Licence, exactly as upstream states it:

- Project level (both repositories): "JS8Call is a derivative work licensed under
  and in accordance with the terms of the GPLv3 license" (README, About dialog);
  the licence file is the GPL version 3 text. No project-level "or any later
  version" statement exists.
- File level (js8call/js8call): `varicode.cpp`, `jsc.cpp`, `jsc_list.cpp`,
  `jsc_map.cpp`, `jsc_checker.cpp` carry the standard notice "either version 3 of
  the License, or (at your option) any later version" and
  "(C) 2018 Jordan Sherer" (followed by his e-mail address, not repeated
  here) => **GPL-3.0-or-later**.
  (`varicode.h` and `jsc.h` carry only the copyright line.)
- The files inherited from WSJT-X (`lib/ft8/*.f90`, `lib/js8/*.f90`, `lib/crc12.cpp`)
  and `decodedtext.cpp`, `mainwindow.cpp` carry no licence notice; `JS8.cpp` carries
  only "(C) 2025 Allan Bazinet" (and his e-mail address). For these only the project-level
  statement applies => we treat them as **GPL-3.0-only**.

What we took:

| Our file | JS8Call origin | Kind of use | Upstream terms |
|---|---|---|---|
| `src/js8_ldpc_174_87_tables.h` | `lib/ft8/ldpc_174_87_params.f90` (generator matrix, column order), `lib/ft8/bpdecode174.f90` (Nm / Mn tables) | data converted by a script | GPLv3, no "or later" (WSJT-X origin) |
| `src/js8_constants.c/.h` | `lib/js8/genjs8.f90`, `lib/js8/js8_params.f90`, `JS8.cpp` | Costas arrays, alphabet, frame geometry | GPLv3, no "or later" |
| `src/js8crc.c/.h` | `lib/crc12.cpp`, `JS8.cpp` | CRC-12 parameters; re-implemented | GPLv3, no "or later" |
| `src/js8enc.c/.h` | `lib/js8/genjs8.f90`, `lib/ft8/encode174.f90`, `JS8.cpp` | logic re-implemented, no code copied | GPLv3, no "or later" |
| `src/js8dec.c` (JS8 parts) | `lib/js8/js8dec.f90`, `JS8.cpp` | decoding passes and acceptance limits re-implemented | GPLv3, no "or later" |
| `src/js8msg.c/.h` | `varicode.cpp`, `jsc.cpp`; receive order from `decodedtext.cpp` | Huffman table, directed commands, group calls (derived tables); logic re-implemented | GPL-3.0-or-later (C) 2018 Jordan Sherer; `decodedtext.cpp`: GPLv3 |
| `src/js8pack.c/.h` (host test tools only, not in the app) | `varicode.cpp` (pack*, buildMessageFrames), `jsc.cpp` (compress) | logic re-implemented | GPL-3.0-or-later (C) 2018 Jordan Sherer |
| `src/js8asm.c/.h` | `mainwindow.cpp` (processDecodeEvent, processBufferedActivity) | receive logic re-implemented | GPLv3, no "or later" |
| `src/js8jsc.c/.h`, `tools/mkjsc.py`, the generated `JSC*.BIN` dictionary files | `jsc_list.cpp`, `jsc_map.cpp` | the 262144-entry word list, converted to a file format the radio can read | GPL-3.0-or-later (C) 2018 Jordan Sherer |
| test models `tests/model.py`, `tests/rv_protocol.py` | the files above, read as data or ported | independent models for cross-checking | as the respective origin |

The JSC dictionary files (`JSCHEAD.BIN`, `JSCDxxx.BIN`, `JSCExxx.BIN`, `JSCES.BIN`)
are **not part of this repository**: you generate them yourself with
`tools/mkjsc.py` from the JS8Call source file that `make fetch` downloads. They
are derived data of `jsc_list.cpp`, (C) 2018 Jordan Sherer,
GPL-3.0-or-later. Whoever distributes them must make the source available with
them: `jsc_list.cpp` / `jsc_map.cpp` of js8call/js8call at commit `a7ff1be`
(sha256 `677b6a87...466ccc6b` and `63dcea03...c232927`) plus `tools/mkjsc.py`.

### 2.4 WSJT-X

JS8Call "is a derivative of the WSJT-X application ... the WSJT-X group maintains
copyright over the original work and code" (JS8Call README). The (174,87) LDPC
code, the Costas synchronisation and the 8-FSK frame structure come from FT8 as
implemented in WSJT-X.

WSJT-X is "licensed under the terms of Version 3 of the GNU General Public License
(GPL)". Copyright, as its About dialog states it: "(c) 2001-2025 by Joe Taylor,
K1JT, Bill Somerville, G4WJS, Steve Franke, K9AN, Nico Palermo, IV3NWV, Uwe Risse,
DG2YCB, and Brian Moran, N9ADG". <https://sourceforge.net/p/wsjt/wsjtx/>

### 2.5 Compiler runtime

The ARM binaries are linked with `libgcc` (GCC; GPL-3.0-or-later with the GCC
Runtime Library Exception 3.1).

## 3. MIT notices that must be kept

### ICAP SDK (parts under MIT: `icap.h`, `ft8dec.c/.h`, tools)

```
MIT License

Copyright (c) 2026 Dawid SQ6EMM

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### ft8_lib

```
MIT License

Copyright (c) 2018 Kārlis Goba

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## 4. Not included in this repository

- **No Icom code.** Nothing here is, contains or is derived from Icom firmware.
  "Icom" and "IC-705" are trademarks of Icom Inc.; this project is independent and
  not affiliated with or endorsed by Icom.
- **No custom firmware.** The ICAP loader the app runs under is part of the custom
  firmware by Dawid SQ6EMM (<https://github.com/sq6emm/icom-custom-firmware>) and
  is obtained from there.
- **No test recordings.** The receiver was tested with recordings made by others.
  They are not redistributed; see [`CREDITS.md`](CREDITS.md) for where they come
  from and under which terms.
- **No dictionary files and no binaries.** `JS8.APP` and the `JSC*.BIN` files are
  build products (see section 2.3 for what applies when you distribute them).
- **No transmit code.** This release is a receiver. The app contains no call to the
  loader's transmit or network services.

JS8Call, WSJT-X and the other names are used only to say what
this software is compatible with or built on. None of the projects named in this
file has reviewed or endorsed this one.
