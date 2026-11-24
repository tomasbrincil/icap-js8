#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
# Build the JS8 app for the IC-705 ICAP loader.  Flags as in the SDK's
# apps/digi-common/flags.sh; linked with the SDK's app.ld + icap_start.S, packed with mkapp.py.
# Nothing is written into the source tree: objects and the app go to $OUT (default
# ./build/arm).  SDK: $ICAP_SDK (default ./third_party/icap-sdk).  Compiler: ${CROSS}gcc, CROSS default
# "arm-none-eabi-" from PATH (or TC=<toolchain directory>).
#   sh shell/build.sh               RX only  -> $OUT/JS8.APP (objects in $OUT/obj), -O2 as the SDK builds FT8
#   OPT_MSG=-Os sh shell/build.sh   message layer + shell for size
#   FLAVOUR=tx sh shell/build.sh    RX + TX  -> $OUT/FT8.APP (objects in $OUT/obj-tx); message layer + shell -Os Thumb
#                                   by default (the shell as ARM code no longer fits since the gesture menu)
# RX flavour: registered as decoder mode "JS8" (mkapp --mode JS8), background, ABI 3, IC-705 only, 16 KiB stack.
# TX flavour: the same sources with -DJS8_TX plus src/js8tx.c, src/js8pack.c and the SDK's txsynth.c (unmodified, compiled
#   from the SDK checkout).  The three TX-only objects are cold code and are built -Os as Thumb (as the SDK does
#   for its QSO helper and the Q65 shim); txsynth.c and src/js8tx.c with -ffp-contract=off like the SDK's encoders.
#   Packed as header name "FT8", decoder mode "FT8", file FT8.APP: the loader gives 15 s slots (@CLOCK P=15000
#   O=500) and beacon arming only to a FILE named FT8.APP (measured 23 Nov 2026 with a small probe app, which
#   had exactly this name / mode / file combination; mode or header name alone were not enough).  On the radio
#   the mode tile therefore reads "FT8" and the original FT8.APP must be off the card.  The app still reads
#   JS8.CFG and writes the JS8L* / JS8M* log segments.
# The terminal-link flavours of the development tree (-DJS8_LINK) are not part of this release: FLAVOUR is rx or tx.
# Receiver speed-up (merged 23 Nov 2026): the cold modules were already -Os here (OPT_COLD), which is what the
#   speed-up build calls "message layer at -Os"; the decoder stays -O2 in every flavour.
#   DEFS=-DJS8_PERF_FILE=0 sh shell/build.sh   without JS8PERF.TXT (the panel's stage times, the tuning keys of
#                                              JS8.CFG and budget= stay)
set -e
cd "$(dirname "$0")/.."
SDK=${ICAP_SDK:-${SDK:-$PWD/third_party/icap-sdk}}
CROSS=${CROSS:-${TC:+$TC/bin/}arm-none-eabi-}
OUT=${OUT:-${BUILD:-$PWD/build}/arm}
FLAVOUR=${FLAVOUR:-rx}
# OPT_MSG: the shell.  OPT_COLD: the message layer, dictionary, reassembly and the station features - code that
# runs once per decoded frame or less.  RX flavour: shell -O2, cold modules -Os.  TX flavour: shell -Os, the cold
# modules -Os as Thumb (as the TX-only objects below).
APPDIR=$OUT
case "$FLAVOUR" in rx|tx) ;; *) echo "build.sh: FLAVOUR=$FLAVOUR - use rx (JS8.APP) or tx (FT8.APP)" >&2; exit 1;; esac
# INTERWORKING RULE (found 23 Nov 2026, terminal stage): an ARM-mode tail call ("b func") into a Thumb function makes
# the linker insert a veneer "ldr pc, [pc, #-4]; .word <absolute address>".  That word has NO relocation record
# (--emit-relocs lists only the compiler's relocations), so mkapp.py does not list it and the loader does not
# relocate it: the first call through such a veneer jumps to the link-time address.  The TX flavour as built before
# had one (p_abs() -> js8f_time, reached at the first decoded frame).  Therefore: ARM objects that call Thumb code
# are compiled with -fno-optimize-sibling-calls (a plain "bl" becomes "blx", which is PC-relative), and the build
# FAILS if the linked image contains any "*_from_arm" or "*_veneer" symbol (check below).
if [ "$FLAVOUR" = tx ]; then
  # app_main stays ARM (the SDK's start stub jumps to it with "b") and calls the Thumb main with "blx".
  # RX + TX: since the gesture menu (23 Nov 2026) the shell is Thumb here too, as in the full app: with an ARM shell the image +
  # bss is 88 KiB of the 80 KiB window.  OPT_MSG="-Os -fno-optimize-sibling-calls" builds the ARM shell of before (does not fit).
  OPT_MSG=${OPT_MSG:--Os -mthumb -DJS8_SHELL_THUMB}; OPT_COLD=${OPT_COLD:--Os -mthumb}
else OPT_MSG=${OPT_MSG:--O2}; OPT_COLD=${OPT_COLD:--Os}; fi
GCC=${CROSS}gcc
command -v "$GCC" > /dev/null 2>&1 || { echo "build.sh: $GCC not found - install an arm-none-eabi GCC (built with: Arm GNU Toolchain 12.2.rel1) or set CROSS=<prefix>" >&2; exit 1; }
[ -f "$SDK/icap.h" ] && [ -f "$SDK/tools/mkapp.py" ] || { echo "build.sh: ICAP SDK not found at $SDK - run 'make fetch' or set ICAP_SDK=<checkout of github.com/sq6emm/icap-sdk>" >&2; exit 1; }
T=$SDK/tools
CF="-mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard -O2 -ffreestanding -nostdlib -fno-builtin -fno-pic -mword-relocations -fno-common -fno-tree-loop-distribute-patterns -fno-section-anchors -Wall -I$SDK"
LF="-nostdlib -T $T/app.ld -Wl,--emit-relocs -Wl,--no-dynamic-linker -Wl,--no-warn-rwx-segments"
DF="$CF -fbuiltin -fno-math-errno -Ishell -Isrc"      # decoder and the app shell (SDK: DF)
# GS: every module except the decoder and the shell is compiled with one section per function / object and the image
# is linked with --gc-sections, so what nothing references is left out (since the receiver speed-up: the SDK FFT
# dg_rfft_power, the reader dg_rdr_* and the table kJS8_LDPC_Mn, which the new decoder kernels no longer use; about
# 2.5 KiB).  The decoder object is compiled exactly as in the speed-up build.
GS="-ffunction-sections -fdata-sections"
LF="$LF -Wl,--gc-sections"
if [ "$FLAVOUR" = tx ]; then
  O="$OUT/obj-tx"; APP=FT8.APP; NAME=FT8; MODE=FT8
  XD="-DJS8_TX -I$SDK/apps/digi-common"
  # the radio's own keyboard (a tap on a row opens it, ENT sends; 705J 1.40 / v14 J only, guarded at run time): both TX
  # builds, never the RX builds
  XD="$XD -DJS8_KBD"
  # build id, shown on line 0 of the app window: by default the commit of the development tree this release was
  # exported from (a65e), so that this build and the one made there are byte-identical.  Set JS8_BUILD_ID=<text>
  # (letters, digits, '+', '-'; a few characters) when you build changed sources.
  BID=${JS8_BUILD_ID:-a65e}
  case "$BID" in *[!A-Za-z0-9+-]*|'') echo "build.sh: JS8_BUILD_ID may hold letters, digits, + and - only" >&2; exit 1;; esac
  XD="$XD -DJS8_BUILD=\"$BID\""
  TXF="$CF -Os -mthumb -ffp-contract=off -fstack-usage -Isrc -I$SDK/apps/digi-common"
else
  O="$OUT/obj"; APP=JS8.APP; NAME=JS8; MODE=JS8; XD=
fi
mkdir -p "$O" "$APPDIR"
$GCC $CF -c $T/icap_start.S -o $O/icap_start.o
$GCC $DF -fstack-usage -c src/js8dec.c -o $O/js8dec.o
$GCC $DF $GS -c shell/digi_util.c -o $O/digi_util.o
for f in js8_constants js8crc js8enc; do $GCC $CF $GS -Isrc -fstack-usage -c src/$f.c -o $O/$f.o; done
for f in js8msg js8jsc js8asm js8feat; do $GCC $CF $GS $OPT_COLD -Isrc -fstack-usage -c src/$f.c -o $O/$f.o; done
$GCC $DF $OPT_MSG $XD $DEFS -fstack-usage -c shell/js8app.c -o $O/js8app.o
OBJS="$O/js8app.o $O/js8dec.o $O/digi_util.o $O/js8_constants.o $O/js8crc.o $O/js8enc.o $O/js8msg.o $O/js8jsc.o $O/js8asm.o $O/js8feat.o"
if [ "$FLAVOUR" = tx ]; then
  $GCC $TXF $GS -c src/js8tx.c -o $O/js8tx.o
  $GCC $TXF $GS -c src/js8pack.c -o $O/js8pack.o
  $GCC $TXF $GS -c $SDK/apps/digi-common/txsynth.c -o $O/txsynth.o
  OBJS="$OBJS $O/js8tx.o $O/js8pack.o $O/txsynth.o"
fi
$GCC $CF $LF $O/icap_start.o $OBJS -lgcc -Wl,-Map=$O/js8.map -o $O/js8.elf
${CROSS}nm $O/js8.elf > $O/js8.nm
if grep -E '_from_arm$|_veneer$' $O/js8.nm; then
  echo "build.sh: FAIL - linker veneer(s) with an unrelocated absolute address (see the INTERWORKING RULE above)" >&2; exit 1
fi
${PYTHON:-python3} -I $T/mkapp.py $O/js8.elf "$APPDIR/$APP" --name $NAME --models 1 --stack 16384 --abi 3 --background --mode $MODE
${CROSS}size $O/icap_start.o $OBJS $O/js8.elf
