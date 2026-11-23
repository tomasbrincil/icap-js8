#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
# Build JS8.APP (RX only) for the IC-705 ICAP loader.  Flags as in the SDK's
# apps/digi-common/flags.sh; linked with the SDK's app.ld + icap_start.S, packed with mkapp.py.
# Nothing is written into the source tree: objects and the app go to $OUT (default
# ./build/arm).  SDK: $ICAP_SDK (default ./third_party/icap-sdk).  Compiler: ${CROSS}gcc, CROSS default
# "arm-none-eabi-" from PATH (or TC=<toolchain directory>).
#   sh shell/build.sh            decoder, shell and core at -O2 (as the SDK builds FT8 for the IC-705); the message layer
#                                (js8msg, js8jsc, js8asm: text, dictionary, reassembly - not time critical) at -Os,
#                                which pays for the streaming front end and the faster kernels (-4.3 KB)
#   OPT_MSG=-O2 sh shell/build.sh   message layer at -O2 too (the first build)
#   OPT_APP=-Os sh shell/build.sh   the shell for size as well (-2.6 KB more)
#   DEFS=-DJS8_PERF_FILE=0 sh shell/build.sh   without JS8PERF.TXT (the panel's stage times, the tuning keys of JS8.CFG and budget= stay)
# Registered as decoder mode "JS8" (mkapp --mode JS8), background, ABI 3, IC-705 only, 16 KiB stack.
set -e
cd "$(dirname "$0")/.."
SDK=${ICAP_SDK:-${SDK:-$PWD/third_party/icap-sdk}}
CROSS=${CROSS:-${TC:+$TC/bin/}arm-none-eabi-}
OUT=${OUT:-${BUILD:-$PWD/build}/arm}
OPT_MSG=${OPT_MSG:--Os}
OPT_APP=${OPT_APP:--O2}
GCC=${CROSS}gcc
command -v "$GCC" > /dev/null 2>&1 || { echo "build.sh: $GCC not found - install an arm-none-eabi GCC (built with: Arm GNU Toolchain 12.2.rel1) or set CROSS=<prefix>" >&2; exit 1; }
[ -f "$SDK/icap.h" ] && [ -f "$SDK/tools/mkapp.py" ] || { echo "build.sh: ICAP SDK not found at $SDK - run 'make fetch' or set ICAP_SDK=<checkout of github.com/sq6emm/icap-sdk>" >&2; exit 1; }
T=$SDK/tools
CF="-mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard -O2 -ffreestanding -nostdlib -fno-builtin -fno-pic -mword-relocations -fno-common -fno-tree-loop-distribute-patterns -fno-section-anchors -Wall -I$SDK"
LF="-nostdlib -T $T/app.ld -Wl,--emit-relocs -Wl,--no-dynamic-linker -Wl,--no-warn-rwx-segments"
DF="$CF -fbuiltin -fno-math-errno -Ishell -Isrc"      # decoder and the app shell (SDK: DF)
mkdir -p "$OUT/obj"
O="$OUT/obj"
$GCC $CF -c $T/icap_start.S -o $O/icap_start.o
$GCC $DF -fstack-usage -c src/js8dec.c -o $O/js8dec.o
$GCC $DF -c shell/digi_util.c -o $O/digi_util.o
for f in js8_constants js8crc js8enc; do $GCC $CF -Isrc -fstack-usage -c src/$f.c -o $O/$f.o; done
for f in js8msg js8jsc js8asm; do $GCC $CF $OPT_MSG -Isrc -fstack-usage -c src/$f.c -o $O/$f.o; done
$GCC $DF $OPT_APP $DEFS -fstack-usage -c shell/js8app.c -o $O/js8app.o
OBJS="$O/js8app.o $O/js8dec.o $O/digi_util.o $O/js8_constants.o $O/js8crc.o $O/js8enc.o $O/js8msg.o $O/js8jsc.o $O/js8asm.o"
$GCC $CF $LF $O/icap_start.o $OBJS -lgcc -Wl,-Map=$O/js8.map -o $O/js8.elf
${PYTHON:-python3} -I $T/mkapp.py $O/js8.elf "$OUT/JS8.APP" --name JS8 --models 1 --stack 16384 --abi 3 --background --mode JS8
${CROSS}size $O/icap_start.o $OBJS $O/js8.elf
