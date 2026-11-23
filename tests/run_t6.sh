#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
# Host test of the JS8 message layer (T6) + ARM object sizes.  Outputs go to $OUT, never into git.
#   sh tests/run_t6.sh            (needs the SD files: python3 -I tools/mkjsc.py)
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-${BUILD:-$PWD/build}}      # BUILD is honoured like in the Makefile and tests/e2e.py
SRC=${SRC:-${JS8_REF:-$PWD/third_party}/js8call}
SDK=${ICAP_SDK:-${SDK:-$PWD/third_party/icap-sdk}}
CROSS=${CROSS:-${TC:+$TC/bin/}arm-none-eabi-}
FILES="src/js8msg.c src/js8jsc.c src/js8pack.c src/js8asm.c"
mkdir -p "$OUT"
[ -f "$OUT/sd/JSCHEAD.BIN" ] || ${PYTHON:-python3} -I tools/mkjsc.py --out "$OUT/sd"
${CC:-cc} -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -pedantic -O2 $FILES tests/t6_msg.c -o "$OUT/t6_msg"
nice "$OUT/t6_msg" "$SRC" "$OUT/sd" | tee "$OUT/t6_msg.log" | tail -3
if [ "$1" = asan ]; then
  ${CC:-cc} -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -O1 -g -fsanitize=address,undefined \
     -fno-sanitize-recover=undefined $FILES tests/t6_msg.c -o "$OUT/t6_msg_asan"
  nice "$OUT/t6_msg_asan" "$SRC" "$OUT/sd" > "$OUT/t6_msg_asan.log"; tail -1 "$OUT/t6_msg_asan.log"
fi
# freestanding ARM objects with the flags of shell/build.sh (-O2) and with -Os
if command -v "${CROSS}gcc" > /dev/null 2>&1; then
  for O in O2 Os; do
    mkdir -p "$OUT/arm-$O"
    for f in $FILES; do
      "${CROSS}gcc" -mcpu=cortex-a9 -marm -mfpu=neon-vfpv3 -mfloat-abi=hard -$O -ffreestanding \
        -nostdlib -fno-builtin -fno-pic -mword-relocations -fno-common -fno-tree-loop-distribute-patterns \
        -fno-section-anchors -Wall -I"$SDK" -c $f -o "$OUT/arm-$O/$(basename "${f%.c}").o"
    done
    echo "ARM -$O:"; "${CROSS}size" -t "$OUT"/arm-$O/js8msg.o "$OUT"/arm-$O/js8jsc.o \
        "$OUT"/arm-$O/js8pack.o "$OUT"/arm-$O/js8asm.o
  done
fi
