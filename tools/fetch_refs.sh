#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
#
# fetch_refs.sh - get the two things this repository needs but does not contain:
#
#   sdk      the ICAP SDK by Dawid SQ6EMM (https://github.com/sq6emm/icap-sdk), cloned with git at the
#            commit this app was built and tested against -> $ICAP_SDK (default third_party/icap-sdk).
#            Used from it: icap.h, tools/{app.ld,icap_start.S,mkapp.py}, apps/digi-common/digi_util.[ch]
#            and, for the FT8 reference bench on the PC only ("make ft8time", "make false"), apps/ft8 +
#            apps/digi-common.  No SDK file is copied into this repository unmodified except
#            shell/digi_util.[ch]; shell/js8app.c and src/js8dec.c are modified derivatives of SDK files
#            (see shell/UPSTREAM and NOTICE.md).
#   js8call  seven source files of JS8Call (GPL-3.0, (C) Jordan Sherer KN4CRD and contributors) and of
#            JS8Call-improved, at pinned commits -> $JS8_REF/js8call, $JS8_REF/js8call-improved
#            (default third_party/).  They are read as DATA only (tables, word list); nothing in them is
#            compiled or run.  The dictionary files for the SD card are generated from jsc_list.cpp
#            (tools/mkjsc.py), and the tests compare the tables in this repository against them.
#            Every one of the seven is checked against tools/refs.sha256 (about 13 MB in total).  The two
#            upstream licence texts (js8call/COPYING, js8call-improved/LICENSE) are fetched with them for
#            your reference; they are not needed by any test and are not checked.
#            The check uses sha256sum, or shasum -a 256 where there is no sha256sum.
#
#   sh tools/fetch_refs.sh [all|sdk|js8call|verify]
#
# REFS_FROM=<dir>: copy the JS8Call files from a local directory with the same layout instead of
# downloading (offline use); the checksums are verified in the same way.
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
ICAP_SDK=${ICAP_SDK:-${SDK:-$ROOT/third_party/icap-sdk}}
JS8_REF=${JS8_REF:-$ROOT/third_party}
SDK_URL=https://github.com/sq6emm/icap-sdk
SDK_REV=3e3af4c7181bde704deb7ce2cdd015d1c5a8e401          # "SDK update for firmware 20261119v14"
ORIG_REV=a7ff1be0b389d287fdc56e2ea0d06962aa68127d         # js8call/js8call
IMPR_REV=a4233d362ff511c02c3d27441e95f315cfe3e977         # JS8Call-improved/JS8Call-improved
ORIG_RAW=https://raw.githubusercontent.com/js8call/js8call/$ORIG_REV
IMPR_RAW=https://raw.githubusercontent.com/JS8Call-improved/JS8Call-improved/$IMPR_REV
SUMS=$ROOT/tools/refs.sha256

sdk() {
    if [ -f "$ICAP_SDK/icap.h" ]; then
        rev=$(git -C "$ICAP_SDK" rev-parse HEAD 2>/dev/null || echo unknown)
        echo "ICAP SDK already at $ICAP_SDK (commit $rev)"
        [ "$rev" = "$SDK_REV" ] || echo "  note: this app was built and tested against $SDK_REV"
        return 0
    fi
    mkdir -p "$(dirname "$ICAP_SDK")"
    git clone "$SDK_URL" "$ICAP_SDK"
    git -C "$ICAP_SDK" -c advice.detachedHead=false checkout "$SDK_REV"
}

get() {     # get BASE-URL UPSTREAM-PATH DEST-DIR
    mkdir -p "$(dirname "$JS8_REF/$3/$2")"
    if [ -n "${REFS_FROM:-}" ]; then cp "$REFS_FROM/$3/$2" "$JS8_REF/$3/$2"
    else echo "  $1/$2"; curl -fsSL -o "$JS8_REF/$3/$2" "$1/$2"; fi
}

verify() {
    if command -v sha256sum > /dev/null 2>&1; then ( cd "$JS8_REF" && sha256sum -c "$SUMS" )
    elif command -v shasum > /dev/null 2>&1; then ( cd "$JS8_REF" && shasum -a 256 -c "$SUMS" )
    else echo "fetch_refs.sh: neither sha256sum nor shasum found - cannot check the reference files" >&2; return 1; fi
}

js8call() {
    if verify > /dev/null 2>&1; then echo "JS8Call reference files already in $JS8_REF (checksums match)"; return 0; fi
    echo "JS8Call reference files -> $JS8_REF"
    for f in lib/ft8/ldpc_174_87_params.f90 lib/ft8/bpdecode174.f90 JS8.cpp varicode.cpp jsc_list.cpp jsc_map.cpp COPYING; do
        get "$ORIG_RAW" "$f" js8call
    done
    for f in JS8_Mode/JS8.cpp LICENSE; do get "$IMPR_RAW" "$f" js8call-improved; done
    verify
}

case "${1:-all}" in
    sdk) sdk;;
    js8call) js8call;;
    verify) verify;;
    all) sdk; js8call;;
    *) sed -n '5,27p' "$0"; exit 2;;
esac
