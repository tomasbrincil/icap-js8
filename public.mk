# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
#
# public.mk - entry points for a fresh clone (included at the end of the Makefile).
#
#   make fetch            get the ICAP SDK and the JS8Call reference files into third_party/ (network; once)
#   make test             the host test suite on generated signals only (see "test:" below)
#   make test-full        test + decoder statistics, kernel bench, profile and the sanitizer / fuzz suite
#                         (the statistics are printed, not judged; only "false" requires zero false decodes)
#   make test-recordings RECORDINGS=<dir>
#                         your own off-air recordings through the decoder bench (no recordings are shipped)
#   make arm              the receiver app for the radio -> $(BUILD)/arm/JS8.APP (needs arm-none-eabi-gcc)
#   make dist             the file set for the SD card -> $(BUILD)/dist/ and $(BUILD)/icap-js8-sdcard.zip
#                         (builds the app: needs arm-none-eabi-gcc as well)
#   make clean            objects and the first test binaries only; to start over: rm -rf build
#
# Settings (environment or make command line):
#   BUILD      build directory                              default ./build
#   ICAP_SDK   checkout of github.com/sq6emm/icap-sdk       default ./third_party/icap-sdk
#   JS8_REF    directory with js8call/ and js8call-improved/ reference files   default ./third_party
#   CROSS      prefix of the ARM bare-metal toolchain       default arm-none-eabi- (looked up in PATH)
#              (or ARM_GCC=/path/to/arm-none-eabi-gcc, or TC=<toolchain directory holding bin/>)
#   RECORDINGS directory with 8k/*.8k.wav (8000 Hz, mono, 16 bit) and optionally expected.json
#   CC, PYTHON host compiler (default cc; the fuzz suite needs -fsanitize=address,undefined) and
#              Python (default python3; 3.8+, standard library only); passed on to the test scripts
# NOTHING HERE TRANSMITS OR TOUCHES A RADIO: every "radio" is the simulator tests/js8sim.c.
# Targets that need recordings (real, real-t0, align, part (b) of e2e) print SKIP and succeed without them.

ifdef ARM_GCC
CROSS ?= $(patsubst %gcc,%,$(ARM_GCC))
endif
ifdef TC
CROSS ?= $(TC)/bin/arm-none-eabi-
endif
CROSS ?= arm-none-eabi-
export BUILD ICAP_SDK JS8_REF CROSS CC PYTHON
HAVE_ARM := $(shell command -v $(CROSS)gcc 2>/dev/null)
# tests/e2e.py takes --skip-arm: without the cross compiler its host parts still run
SKIP_ARM  = $(if $(HAVE_ARM),,--skip-arm)

# the suites rebuild the same host binaries in $(BUILD): never run two targets of this Makefile in parallel
.NOTPARALLEL:
.PHONY: help fetch deps dict test test-full test-recordings dist real-rec real-t0-rec align-rec

help:
	@sed -n '4,28p' public.mk

fetch:
	sh tools/fetch_refs.sh all

deps:
	@[ -f "$(ICAP_SDK)/icap.h" ] && [ -f "$(ICAP_SDK)/tools/mkapp.py" ] && [ -f "$(ICAP_SDK)/apps/digi-common/digi_util.c" ] || { \
	  echo "ICAP SDK not found at $(ICAP_SDK)"; \
	  echo "  run 'make fetch', or set ICAP_SDK=<your checkout of https://github.com/sq6emm/icap-sdk>"; exit 1; }
	@sh tools/fetch_refs.sh verify > /dev/null 2>&1 || { \
	  echo "JS8Call reference files missing or changed in $(JS8_REF) (tools/refs.sha256)"; \
	  echo "  run 'make fetch' (downloads 7 source files, 13 MB, and 2 licence texts from github.com/js8call and JS8Call-improved)"; \
  echo "  (the check needs sha256sum or shasum; run 'sh tools/fetch_refs.sh verify' to see what it reports)"; exit 1; }
	@[ -n "$(HAVE_ARM)" ] || echo "note: $(CROSS)gcc not found - host tests only, the ARM build checks are skipped"

# the dictionary files (generated from JS8Call's jsc_list.cpp; the radio needs JSCHEAD.BIN + JSCD*.BIN only)
dict: $(BUILD)/sd/JSCHEAD.BIN
$(BUILD)/sd/JSCHEAD.BIN: tools/mkjsc.py
	$(PYTHON) -I -B tools/mkjsc.py --out $(BUILD)/sd

# recordings are optional: with RECORDINGS=<dir> these run, without they say so and succeed
REC_TARGETS = real real-t0
ifneq ($(HAVE_REC),)
$(REC_TARGETS): %: %-rec
else
$(REC_TARGETS):
	@echo "SKIP  $@: needs off-air recordings (make $@ RECORDINGS=<dir with 8k/*.8k.wav>); none are shipped"
endif
# align, and parts of arena and prof, use one particular crowded recording ($(AIR), found by the end of its file
# name); without it they skip
ifneq ($(wildcard $(AIR)),)
align: align-rec
else
align:
	@echo "SKIP  $@: needs one particular recording (*__decode_air_test_1min.8k.wav) in RECORDINGS/8k; it is not shipped"
endif

# make test: everything that needs no recording.
#   check      tables against the JS8Call sources, encoder against the Python model (1000 frames),
#              freestanding core and decoder, spectrum of generated signals
#   dict       dictionary files
#   t6 rv      message layer (plain + ASan/UBSan), independent protocol model
#   dec t2     decoder bench, clean loopback sweep (must be 100 %)
#   stream     streaming front end gives the same frames as the non-streamed decode (generated sweep;
#              with RECORDINGS also every recording)
#   e2e        the receiver app in the loader simulator on generated scenarios, + the ARM build and
#              its checks if a cross compiler is there, + the recordings if RECORDINGS has expected.json
test: deps
	$(MAKE) check
	$(MAKE) dict
	$(MAKE) t6
	$(MAKE) rv
	$(MAKE) dec
	$(MAKE) t2
	$(MAKE) stream
	$(MAKE) e2e
	@echo "make test: all passed$(if $(HAVE_ARM),, (host only: no ARM toolchain))$(if $(HAVE_REC),, (generated signals only: no recordings))"

test-full: test
	$(MAKE) t3 multi false ft8time
	$(MAKE) apparena snrcal abort arena
	$(MAKE) kbench prof
	$(MAKE) real real-t0 align
	$(MAKE) fuzz
	@echo "make test-full: all passed$(if $(HAVE_ARM),, (host only: no ARM toolchain))$(if $(HAVE_REC),, (generated signals only: no recordings))"

# Your own recordings: $(RECORDINGS)/8k/<name>.8k.wav, 8000 Hz mono 16 bit.  With $(RECORDINGS)/expected.json
# (the format tests/e2e.py reads in run_real(); the tool that wrote ours is not part of this release) the
# end-to-end comparison runs as well.
test-recordings: deps
	@[ -n "$(HAVE_REC)" ] || { \
	  echo "usage: make test-recordings RECORDINGS=<dir with 8k/*.8k.wav>   (no recordings are part of this repository)"; exit 1; }
	$(MAKE) dec dict
	$(MAKE) real real-t0 stream
	@if [ -f "$(RECORDINGS)/expected.json" ]; then $(MAKE) e2e; \
	 else echo "no $(RECORDINGS)/expected.json: decoder bench only (results in $(BUILD)/real), end-to-end comparison skipped"; fi

# the SD-card file set: receiver app (RX only), receive dictionary, configuration presets
dist: deps dict
	sh tools/mkdist.sh
