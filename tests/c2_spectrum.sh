#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
# c2: generate JS8 Normal test signals with js8gen and verify the clean ones with
# the independent spectrum tool js8spec (8 tones, 6.25 Hz apart, 12.64 s, Costas, tones equal
# to the encoder output). Usage: c2_spectrum.sh BUILD_DIR
set -u
B=${1:?build dir}
O=$B/c2
mkdir -p "$O"
rc=0
run() { # name f0 dt msg i3
  name=$1; f0=$2; dt=$3; msg=$4; i3=$5
  "$B/js8gen" -o "$O/$name.wav" -s "$f0,$dt,A10000,$msg,$i3" || { rc=1; return; }
  tones=$(awk '$1=="S"{print $NF}' "$O/$name.wav.txt")
  echo "--- js8spec $name.wav (f0 $f0, dt $dt, $msg i3=$i3)"
  "$B/js8spec" "$O/$name.wav" -e "$tones" -f "$f0" -d "$dt" > "$O/$name.spec.txt" 2>&1 || rc=1
  grep -E '^(PASS|FAIL|envelope|least|tones|js8spec|wav)' "$O/$name.spec.txt"
}
run clean_1500   1500     0      HELLOWORLD++ 5
run clean_0500   500      0.3125 0123456789AB 0
run clean_2437   2437.5   -0.4   HELLOWORLD++ 5
run clean_1001   1001.3   1.2345 t0vYTDe3231m 3

echo "--- raw PCM output is the WAV without the 44-byte header"
"$B/js8gen" -o "$O/clean_1500.pcm" -s "1500,0,A10000,HELLOWORLD++,5" || rc=1
if tail -c +45 "$O/clean_1500.wav" | cmp -s - "$O/clean_1500.pcm"; then echo "PASS  .pcm == .wav data"; else echo "FAIL  .pcm differs from .wav data"; rc=1; fi

echo "--- determinism: same seed -> identical file, other seed -> different"
"$B/js8gen" -o "$O/n1.wav" -S 7 -n 300 -s "1500,0,-10,HELLOWORLD++,5" >/dev/null || rc=1
"$B/js8gen" -o "$O/n2.wav" -S 7 -n 300 -s "1500,0,-10,HELLOWORLD++,5" >/dev/null || rc=1
"$B/js8gen" -o "$O/n3.wav" -S 8 -n 300 -s "1500,0,-10,HELLOWORLD++,5" >/dev/null || rc=1
if cmp -s "$O/n1.wav" "$O/n2.wav" && ! cmp -s "$O/n1.wav" "$O/n3.wav"; then echo "PASS  seeded PRNG reproducible"; else echo "FAIL  seeded PRNG"; rc=1; fi
rm -f "$O/n2.wav" "$O/n2.wav.txt" "$O/n3.wav" "$O/n3.wav.txt"

echo "--- demo files for the decoder tests: noise, overlap, several periods"
"$B/js8gen" -o "$O/demo_snr-10.wav" -S 1 -n 300 -s "1500,0.2,-10,HELLOWORLD++,5" || rc=1
"$B/js8gen" -o "$O/demo_multi_3periods.wav" -S 2 -n 300 -p 3 \
   -s "1000,0,-5,HELLOWORLD++,5" -s "1006.25,0.1,-8,0123456789AB,0,1" -s "1800,-0.4,-12,t0vYTDe3231m,3" \
   -R "20,500,2500,-10,0.5" || rc=1
exit $rc
