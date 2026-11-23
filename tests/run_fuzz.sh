#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
# Adversarial host tests (ASan + UBSan, -Wall -Wextra) of the JS8 radio sources:
#   fuzz_core   random 87-bit frames + corrupted dictionary files -> js8msg / js8jsc / js8asm
#   fuzz_dec    hostile audio and call parameters -> js8_decode (work area and results in exact malloc blocks)
#   fuzz_shell  the radio shell (shell/js8app.c) against a fake loader that checks the radio limits
#               (DECODE <= 40, app window <= 40, 4 files, handles, text-only logs, one popup per period, SD calls
#               between stop polls, archive ring) and injects SD / RTC / audio faults
# Binaries and logs go to $OUT (default ./build/san); nothing is written into the source tree.
# Needs the dictionary files in $SD (tools/mkjsc.py) and tests/e2e.py's mix.wav ($MIX).  About 3 minutes on 8 cores.
# Exit code 1 if any run fails.  The last blocks are the regression tests of the review of 23 Nov 2026 (stale
# capture handle at exit, stop latency with 45 hostile stations, nominal start out of range, log ring).
set -u
cd "$(dirname "$0")/.."
BUILD=${BUILD:-$PWD/build}
OUT=${OUT:-$BUILD/san}
SD=${SD:-$BUILD/sd}
MIX=${MIX:-$BUILD/e2e/mix.wav}
SDK=${ICAP_SDK:-${SDK:-$PWD/third_party/icap-sdk}}
N=${N:-1}                                   # scale factor for the iteration counts
SAN="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -Wall -Wextra"
CORE="src/js8_constants.c src/js8crc.c src/js8enc.c"
mkdir -p "$OUT"
${CC:-cc} -std=c99 -D_POSIX_C_SOURCE=200809L $SAN tests/fuzz_core.c src/js8msg.c src/js8jsc.c src/js8asm.c -o "$OUT/fuzz_core" || exit 2
${CC:-cc} -std=gnu99 $SAN -fsanitize=float-cast-overflow -fno-math-errno -I"$SDK/apps/digi-common" -Isrc tests/fuzz_dec.c src/js8dec.c $CORE shell/digi_util.c -lm -o "$OUT/fuzz_dec" || exit 2
${CC:-cc} -std=gnu99 -O2 -Wall -Wextra -fno-math-errno -I"$SDK/apps/digi-common" -Isrc tests/fuzz_dec.c src/js8dec.c $CORE shell/digi_util.c -lm -o "$OUT/fuzz_dec_o2" || exit 2
${CC:-cc} -std=gnu99 $SAN -fno-math-errno -DDIGI_HOST -I"$SDK" -Ishell -Isrc tests/fuzz_shell.c src/js8dec.c src/js8msg.c src/js8jsc.c src/js8asm.c src/js8pack.c $CORE shell/digi_util.c -lm -o "$OUT/fuzz_shell" || exit 2
# the same with unsigned plain char, as on ARM
${CC:-cc} -std=gnu99 $SAN -funsigned-char -fno-math-errno -DDIGI_HOST -I"$SDK" -Ishell -Isrc tests/fuzz_shell.c src/js8dec.c src/js8msg.c src/js8jsc.c src/js8asm.c src/js8pack.c $CORE shell/digi_util.c -lm -o "$OUT/fuzz_shell_uc" || exit 2

cd "$OUT" || exit 2
rm -f fz_*.log fz_status
run() { name=$1; shift; ( nice "$@" > "fz_$name.log" 2>&1; rc=$?; echo "$rc $name: $(tail -1 "fz_$name.log" | cut -c1-200)"; echo "$rc $name" >> fz_status ) & }
for s in 1 2 3; do run core$s ./fuzz_core "$SD" $((400000 * N)) $s; done
for s in 1 2 3 4; do run dec$s ./fuzz_dec $((150 * N)) $s; done
for dm in 0 1 2; do run frames$dm ./fuzz_shell frames "$SD" $((50000 * N)) $dm $dm; done
run frames_uc ./fuzz_shell_uc frames "$SD" $((30000 * N)) 9 2
for fl in 0 1 2 4 8 16 31; do run app$fl ./fuzz_shell app "$SD" "$MIX" 60 $fl $fl; done
run app_slow env COST=0.03 ./fuzz_shell app "$SD" "$MIX" 40 5 0
run app_noise ./fuzz_shell app "$SD" - 40 6 31
run ring ./fuzz_shell ring "$SD"
wait > /dev/null 2>&1
one() { name=$1; shift; "$@" > "fz_$name.log" 2>&1; rc=$?; echo "$rc $name" >> fz_status; return $rc; }
./fuzz_shell mkwav "$OUT/hostile45.wav" 45
echo "--- 45 stations, three rare dictionary words per frame: SD calls between two icap_should_stop() polls (limit 40)"
one hostile45 env ASAN_OPTIONS=detect_leaks=0 COST=0.0005 ./fuzz_shell app "$SD" "$OUT/hostile45.wav" 12 1 0; grep "SD calls\|FAIL\|fuzz_shell:" fz_hostile45.log
rm -f "$OUT/hostile45.wav"
echo "--- abort-poll latency of the decoder (x86 CPU time)"
./fuzz_dec_o2 poll
echo "--- regression: log write fails in the last period, stop during a capture (audio used to land in JS8LOG.TXT)"
one exit_stale_fd env END_MS=67000 FAILLOG=61000,65000 COST=0.01 ./fuzz_shell app "$SD" "$MIX" 5 1 0; grep "FAIL\|fuzz_shell:" fz_exit_stale_fd.log
echo "--- regression: stop requested at many different moments (start-up loads, capture, decode, results, log write)"
rc=0; for e in 3 40 200 900 1500 14990 15020 15400 16000 29900 30100 31000 45200 60500; do
  END_MS=$e COST=0.01 ./fuzz_shell app "$SD" "$MIX" 5 $e 0 > fz_stop.log 2>&1 || { rc=1; echo "END_MS=$e: $(grep 'FAIL' fz_stop.log | head -2)"; }
done; echo "$rc stop_sweep" >> fz_status; echo "stop sweep: $([ $rc = 0 ] && echo PASS || echo FAIL)"
echo "--- regression: the app is blocked for 1.3 s from 0.1 s before a period edge (RTC starts 23:59:13, edge at 32.0 s), so second 0 of the period is never seen: the period is counted as skipped"
one stall_edge env STALL=31900,1300 EXPECT_SKIP=1 COST=0.002 ./fuzz_shell app "$SD" "$MIX" 5 1 0; grep "FAIL\|fuzz_shell:\|   app:" fz_stall_edge.log
echo "--- regression (UBSan): nominal start beyond +-2^31 samples must be refused, not converted"
one wildnom env WILDNOM=1 ./fuzz_dec 60 3; grep "runtime error\|fuzz_dec:" fz_wildnom.log | head -2
echo "--- summary"
if grep -v "^0 " fz_status; then echo "run_fuzz: FAIL"; exit 1; fi
echo "run_fuzz: PASS ($(wc -l < fz_status) runs)"
