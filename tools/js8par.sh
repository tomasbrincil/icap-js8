#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
# js8par.sh - run a js8sweep command on several cores with different seeds and merge the summaries.
# usage: js8par.sh [-j JOBS] [-o LOG] [-s SEED0] -- js8sweep-arguments...   (options first, then "--")
#   BUILD=/path  build directory with js8sweep (default ./build)
# Exit code 1 if a job or the aggregation failed (the summary of the jobs that ran is still printed).
# Each job gets "-S SEED0+i"; -n in the arguments is per job. Extra arguments after "--thr" go to js8agg.py.
BUILD=${BUILD:-$(dirname "$0")/../build}
HERE=$(dirname "$0")
J=8; LOG=; S0=1000; THR=
while [ $# -gt 0 ] && [ "$1" != "--" ]; do
  case "$1" in -j) J=$2; shift;; -o) LOG=$2; shift;; -s) S0=$2; shift;; --thr) THR=--thr;; esac
  shift
done
shift
T=$(mktemp -d)
i=0; pids=
while [ $i -lt $J ]; do
  nice -n 10 "$BUILD/js8sweep" "$@" -S $((S0 + i)) > "$T/$i.out" &
  pids="$pids $!"
  i=$((i + 1))
done
bad=0
for p in $pids; do wait "$p" || bad=$((bad + 1)); done
${PYTHON:-python3} "$HERE/js8agg.py" $THR "$T"/*.out > "$T/summary" || bad=$((bad + 1))
if [ -n "$LOG" ]; then tee "$LOG" < "$T/summary"; else cat "$T/summary"; fi
rm -rf "$T"
[ $bad = 0 ] || { echo "js8par.sh: $bad job(s) failed (js8sweep $*)" >&2; exit 1; }
