# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
#
# JS8 for the IC-705 (ICAP) - HOST targets only (PC test bench). Nothing is built into the
# source directory; everything goes to $(BUILD). The radio build (ARM, freestanding) comes
# from shell/build.sh ("make arm"); "make freestanding" only proves on the host that the
# core sources need no libc.
#
#   make            build the tools and tests
#   make test-core  table tests + encoder tests (incl. 1000 random frames vs tests/model.py) + freestanding check
#   make c2         generate WAVs and check them with the spectrum tool
#   make check      test-core + c2
#   make dec        build the decoder test bench (js8wav, js8sweep, ft8ref; needs the SDK checkout)
#   make t2         clean loopback sweep (f0 500..2500 Hz in 1.3 Hz steps, dt -2..+2 s), must be 100 %
#   make t3         white-noise (AWGN) sweep -28..-10 dB (104 trials per step, each pass alone and all), ~3 min on 8 cores
#   make multi      20 signals per period incl. pairs 6.25 Hz and 50 Hz apart, with / without subtraction
#   make false      false decodes: 1000 noise-only periods, 200 periods of FT8-only audio
#   make ft8time    decode time against the unmodified FT8 decoder of the SDK on the same load
#   make real       every recording in $(SAMPLES) through js8wav -a -u
#   make deccheck   t2 + t3 + multi + false + ft8time + real (logs in $(BUILD)/logs, $(BUILD)/real)
#   make real-t0    every recording through js8wav -t 0 (file start = period edge) -> $(BUILD)/real/<name>.t0.txt
#   make apparena   the clean sweep and one noise sweep with the RADIO APP's arena (APP_ARENA bytes), both bands (one pass / two passes)
#   make t3seed     one noise sweep with a seed taken from the clock (independent confirmation of the thresholds)
#   make arena      arena peak (planned layout) for band / arena combinations + guard-page runs -> logs/arena.log
#   make abort      abort test (js8wav -A N) -> logs/abort.log
#   make snrcal     reported SNR against the true SNR, -18 .. +20 dB -> logs/snr_cal.log
#   make osd-exp    the OSD experiment (js8sweep_osd; the radio build has no OSD)
#   make align      capture-start sweep 0..75 ms on one crowded recording of the private set (skipped without it), both bands -> logs/align.log
#   make stream     streaming front end: same frames and candidates with / without it (recordings, clean sweep) -> logs/stream.log
#   make coreextra  real-t0 apparena t3seed arena abort snrcal align (the decoder numbers beyond deccheck)
#   make prof       where a period's work goes: stage times + operation counters (tests/js8prof.c) for an empty band,
#                   a crowded recording of the private set (left out without it), 20 signals per period and noise, app arena and band -> logs/prof.log
#   make kbench     inner kernels: STFT row against a double-precision windowed DFT (cells that differ), time per STFT row,
#                   per BP iteration and per subtracted signal x row; the same with the reference BP (-DJS8_BP_SPA) -> logs/kbench.log
#   make t6 / rv / fuzz   message layer tests / independent protocol model / sanitizer + fault-injection suite
#   make clean      remove the objects and the first test binaries; to start over, delete the build directory (rm -rf build)
# t3, multi, snrcal, t3seed and the noise sweeps of apparena PRINT statistics and fail only when a job crashes;
# ft8time, arena, abort, prof and kbench print through a pipe and never fail; t2, stream, the clean sweeps of
# apparena and false (zero false decodes) are pass / fail.  See docs/BUILDING.md.
# Radio app and transmit path: the two sections at the end of this file (make sim e2e arm, make txsim t7 txe2e fixe2e
# arm-tx); the entry points for a fresh clone (make fetch, test, arm-all, dist): public.mk.

BUILD   ?= $(CURDIR)/build
override BUILD := $(abspath $(BUILD))
# JS8Call reference files (data only, fetched by tools/fetch_refs.sh): $(JS8_REF)/js8call, $(JS8_REF)/js8call-improved
JS8_REF ?= $(CURDIR)/third_party
override JS8_REF := $(abspath $(JS8_REF))
SRCREF  ?= $(JS8_REF)
CC      ?= cc
PYTHON  ?= python3
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
FSFLAGS  = -std=c99 -O2 -Wall -Wextra -Wpedantic -ffreestanding -fno-builtin -nostdinc \
           -isystem $(shell $(CC) -print-file-name=include)

# checkout of https://github.com/sq6emm/icap-sdk (tools/fetch_refs.sh)
ICAP_SDK ?= $(CURDIR)/third_party/icap-sdk
override ICAP_SDK := $(abspath $(ICAP_SDK))
SDKAPPS ?= $(ICAP_SDK)/apps
# off-air recordings are NOT part of the repository: RECORDINGS=<dir with 8k/*.8k.wav> (make test-recordings)
RECORDINGS ?=
SAMPLES ?= $(RECORDINGS)/8k
# targets that need recordings print SKIP and succeed when there are none (public.mk)
HAVE_REC := $(wildcard $(SAMPLES)/*.wav)
DIGI     = $(SDKAPPS)/digi-common
# the decoder is a fork of the SDK's ft8dec.c and keeps its style (no -Wconversion / -Wpedantic)
DECFLAGS ?= -std=gnu99 -O2 -Wall -Wextra -Wshadow -fno-math-errno -I$(DIGI) -Isrc
FT8FLAGS  = -std=gnu99 -O2 -Wall -fno-math-errno -I$(DIGI) -I$(SDKAPPS)/ft8 -I$(SDKAPPS)/ft8/ft8 -Isrc
FT8PORT   = -include $(SDKAPPS)/ft8/ft8/ft8_port.h
DEC_O    = $(BUILD)/obj/js8dec.o $(BUILD)/obj/digi_util.o
DECBINS  = $(BUILD)/js8wav $(BUILD)/js8sweep $(BUILD)/js8sweep_osd $(BUILD)/ft8ref
FT8_O    = $(addprefix $(BUILD)/obj/ft8/,f_constants.o f_crc.o f_message.o f_text.o f_encode.o f_port.o ft8dec.o ft8enc.o txsynth.o msg77.o)
PAR      = BUILD=$(BUILD) sh tools/js8par.sh
LOGS     = $(BUILD)/logs

CORE_C  = src/js8_constants.c src/js8crc.c src/js8enc.c
CORE_H  = src/js8_constants.h src/js8crc.h src/js8enc.h src/js8_port.h src/js8_ldpc_174_87_tables.h
CORE_O  = $(CORE_C:src/%.c=$(BUILD)/obj/%.o)
BINS    = $(BUILD)/t0_tables $(BUILD)/t1_encoder $(BUILD)/js8gen $(BUILD)/js8spec

.PHONY: all test-core t0 t1 tables freestanding c2 check clean dec t2 t3 multi false ft8time real deccheck \
        real-t0 apparena t3seed arena abort snrcal osd-exp align coreextra t6 rv fuzz prof kbench stream
all: $(BINS)
dec: $(DECBINS)

$(BUILD)/obj:
	mkdir -p $@

$(BUILD)/obj/%.o: src/%.c $(CORE_H) | $(BUILD)/obj
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/t0_tables: tests/t0_tables.c $(CORE_O)
	$(CC) $(CFLAGS) $^ -o $@
$(BUILD)/t1_encoder: tests/t1_encoder.c $(CORE_O)
	$(CC) $(CFLAGS) $^ -o $@
$(BUILD)/js8gen: tests/js8gen.c $(CORE_O)
	$(CC) $(CFLAGS) $^ -lm -o $@
$(BUILD)/js8spec: tests/js8spec.c | $(BUILD)/obj
	$(CC) $(CFLAGS) $< -lm -o $@

# ---- decoder (src/js8dec.c) and its host test bench
$(BUILD)/obj/js8dec.o: src/js8dec.c src/js8dec.h $(CORE_H) | $(BUILD)/obj
	$(CC) $(DECFLAGS) -c $< -o $@
$(BUILD)/obj/js8dec_osd.o: src/js8dec.c src/js8dec.h $(CORE_H) | $(BUILD)/obj
	$(CC) $(DECFLAGS) -DJS8_WITH_OSD -c $< -o $@
$(BUILD)/obj/digi_util.o: $(DIGI)/digi_util.c | $(BUILD)/obj
	$(CC) $(DECFLAGS) -c $< -o $@
$(BUILD)/js8wav: tests/js8wav.c tests/js8synth.h $(DEC_O) $(CORE_O)
	$(CC) $(DECFLAGS) $< $(DEC_O) $(CORE_O) -lm -o $@
$(BUILD)/js8sweep: tests/js8sweep.c tests/js8synth.h $(DEC_O) $(CORE_O)
	$(CC) $(DECFLAGS) $< $(DEC_O) $(CORE_O) -lm -o $@
# profiler: stage times (decoder's own clock) + operation counters per scenario, see tests/js8prof.c (make prof)
$(BUILD)/js8prof: tests/js8prof.c tests/js8synth.h $(DEC_O) $(CORE_O)
	$(CC) $(DECFLAGS) $< $(DEC_O) $(CORE_O) -lm -o $@
# experiment only: the same driver with the OSD compiled in (-T osd=1|2); the radio build has no OSD
$(BUILD)/js8sweep_osd: tests/js8sweep.c tests/js8synth.h $(BUILD)/obj/js8dec_osd.o $(BUILD)/obj/digi_util.o $(CORE_O)
	$(CC) $(DECFLAGS) $< $(BUILD)/obj/js8dec_osd.o $(BUILD)/obj/digi_util.o $(CORE_O) -lm -o $@

# the unmodified FT8 decoder / encoder of the SDK, compiled from the SDK checkout (reference bench only)
$(BUILD)/obj/ft8:
	mkdir -p $@
$(BUILD)/obj/ft8/f_%.o: $(SDKAPPS)/ft8/ft8/ft8/%.c | $(BUILD)/obj/ft8
	$(CC) $(FT8FLAGS) -Wno-unused-variable -Wno-unused-const-variable -Wno-unused-function $(FT8PORT) -c $< -o $@
$(BUILD)/obj/ft8/f_port.o: $(SDKAPPS)/ft8/ft8/ft8_port.c | $(BUILD)/obj/ft8
	$(CC) $(FT8FLAGS) $(FT8PORT) -c $< -o $@
$(BUILD)/obj/ft8/ft8dec.o: $(SDKAPPS)/ft8/ft8dec.c | $(BUILD)/obj/ft8
	$(CC) $(FT8FLAGS) -c $< -o $@
$(BUILD)/obj/ft8/ft8enc.o: $(SDKAPPS)/ft8/ft8enc.c | $(BUILD)/obj/ft8
	$(CC) $(FT8FLAGS) -ffp-contract=off $(FT8PORT) -c $< -o $@
$(BUILD)/obj/ft8/%.o: $(DIGI)/%.c | $(BUILD)/obj/ft8
	$(CC) $(FT8FLAGS) -ffp-contract=off -c $< -o $@
$(BUILD)/ft8ref: tests/ft8ref.c tests/js8synth.h $(FT8_O) $(DEC_O) $(CORE_O)
	$(CC) $(FT8FLAGS) -Wextra $< $(FT8_O) $(DEC_O) $(CORE_O) -lm -o $@

$(LOGS):
	mkdir -p $@

# t2: exit code 1 unless every frame decodes with |df| <= 1 Hz and |ddt| <= 0.05 s
t2: $(BUILD)/js8sweep | $(LOGS)
	$(BUILD)/js8sweep clean > $(LOGS)/t2_clean_nonoise.log; r=$$?; tail -1 $(LOGS)/t2_clean_nonoise.log; [ $$r = 0 ]
	$(BUILD)/js8sweep clean -N 300 > $(LOGS)/t2_clean_noise300.log; r=$$?; tail -1 $(LOGS)/t2_clean_noise300.log; [ $$r = 0 ]

# t3: 8 jobs x 13 trials = 104 trials per 1 dB step; passmask 1,2,4,8,16 = each pass alone, 31 = default
t3: $(BUILD)/js8sweep | $(LOGS)
	for m in 1 2 4 8 16 31; do echo "== passmask=$$m"; $(PAR) --thr -o $(LOGS)/t3_awgn_mask$$m.log -- awgn -s -28,-10,1 -n 13 -T passmask=$$m > /dev/null || exit 1; grep "^#\|^F" $(LOGS)/t3_awgn_mask$$m.log; done
	echo "== late-keyed transmitter (first 23 symbols missing), passes 1+5 vs all"; \
	for m in 17 31; do $(PAR) --thr -o $(LOGS)/t3_cut23_mask$$m.log -- awgn -s -22,-8,2 -n 13 -c 23 -T passmask=$$m > /dev/null || exit 1; grep "^#\|^F" $(LOGS)/t3_cut23_mask$$m.log; done

multi: $(BUILD)/js8sweep | $(LOGS)
	for sp in 1 2 3; do $(PAR) -o $(LOGS)/multi_subpasses$$sp.log -- multi -n 25 -T subpasses=$$sp || exit 1; done
	$(PAR) -o $(LOGS)/multi_strong.log -- multi -n 25 -l -5,15

false: $(BUILD)/js8sweep $(BUILD)/ft8ref | $(LOGS)
	$(PAR) -o $(LOGS)/false_noise.log -- noise -n 125
	@grep -q "^S noise .* false=0 " $(LOGS)/false_noise.log || { echo "FAIL  false: false decodes in noise-only periods, or no summary line"; exit 1; }
	$(BUILD)/ft8ref cross -t 200 -n 20 > $(LOGS)/false_ft8audio.log; r=$$?; cat $(LOGS)/false_ft8audio.log; [ $$r = 0 ]
	@grep -q "^S cross .* js8_false=0 " $(LOGS)/false_ft8audio.log || { echo "FAIL  false: false JS8 decodes in FT8-only audio, or no summary line"; exit 1; }
	@echo "PASS  false: no false decode in the noise-only periods and in the FT8-only audio"

ft8time: $(BUILD)/ft8ref | $(LOGS)
	( $(BUILD)/ft8ref noise -t 40; $(BUILD)/ft8ref load -t 40 -n 5; $(BUILD)/ft8ref load -t 40 -n 20; $(BUILD)/ft8ref load -t 40 -n 40 -l -15,10 ) | tee $(LOGS)/ft8time.log

real-rec: $(BUILD)/js8wav
	mkdir -p $(BUILD)/real
	for f in $(SAMPLES)/*.wav; do n=`basename $$f .8k.wav`; $(BUILD)/js8wav $$f -a -u > $(BUILD)/real/$$n.txt; \
	  echo "$$n: `grep -c '^U' $(BUILD)/real/$$n.txt` distinct frames (union), `grep -c '^R' $(BUILD)/real/$$n.txt` at the best alignment"; done

deccheck: t2 t3 multi false ft8time real

# ---- further decoder measurements ---------------------------------------------------------------
# APP_ARENA = the radio app's decoder arena on the host: 245760 - the shell's reserve (js8sim prints it)
APP_ARENA ?= 189824
APP_RES   ?= $(shell expr 245760 - $(APP_ARENA))
# a crowded one-minute off-air recording of the private set, found by the end of its file name
AIR        = $(firstword $(wildcard $(SAMPLES)/*__decode_air_test_1min.8k.wav))
DEMO       = $(BUILD)/c2/demo_multi_3periods.wav

real-t0-rec: $(BUILD)/js8wav | $(LOGS)
	mkdir -p $(BUILD)/real
	for f in $(SAMPLES)/*.wav; do n=`basename $$f .8k.wav`; $(BUILD)/js8wav $$f -t 0 > $(BUILD)/real/$$n.t0.txt; \
	  echo "$$n: `grep -c '^R' $(BUILD)/real/$$n.t0.txt` frames; `tail -1 $(BUILD)/real/$$n.t0.txt`"; done | tee $(LOGS)/real_t0.log

apparena: $(BUILD)/js8sweep | $(LOGS)
	$(BUILD)/js8sweep clean -W $(APP_ARENA) -b 450,2550 > $(LOGS)/t2_app_default.log; r=$$?; tail -1 $(LOGS)/t2_app_default.log; [ $$r = 0 ]
	$(BUILD)/js8sweep clean -W $(APP_ARENA) -b 200,3000 -N 300 > $(LOGS)/t2_app_wide.log; r=$$?; tail -1 $(LOGS)/t2_app_wide.log; [ $$r = 0 ]
	$(BUILD)/js8sweep clean -W 170000 -b 200,3000 -N 300 > $(LOGS)/t2_split170k.log; r=$$?; tail -1 $(LOGS)/t2_split170k.log; [ $$r = 0 ]
	$(PAR) --thr -o $(LOGS)/t3_app_default.log -- awgn -s -23,-15,1 -n 13 -W $(APP_ARENA) -b 450,2550 > /dev/null && grep "^#\|^F" $(LOGS)/t3_app_default.log
	$(PAR) --thr -o $(LOGS)/t3_app_wide.log -- awgn -s -23,-15,1 -n 13 -W $(APP_ARENA) -b 200,3000 > /dev/null && grep "^#\|^F" $(LOGS)/t3_app_wide.log

t3seed: $(BUILD)/js8sweep | $(LOGS)
	s=`date +%s`; echo "seed $$s"; $(PAR) -s $$s --thr -o $(LOGS)/t3_seed.log -- awgn -s -23,-15,1 -n 25 > /dev/null || exit 1; grep "^#\|^F" $(LOGS)/t3_seed.log; echo "# seed $$s" >> $(LOGS)/t3_seed.log

$(BUILD)/arena_guard: tests/arena_guard.c tests/js8synth.h $(DEC_O) $(CORE_O)
	$(CC) $(DECFLAGS) $< $(DEC_O) $(CORE_O) -lm -o $@

arena: $(BUILD)/js8wav $(BUILD)/arena_guard $(DEMO) | $(LOGS)
	( for a in "500,2500 245760" "200,3000 245760" "200,3000 200000" "100,3900 245760" "450,2550 $(APP_ARENA)" "200,3000 $(APP_ARENA)"; do set -- $$a; \
	    echo "band $$1 arena $$2: `$(BUILD)/js8wav $(DEMO) -q -b $$1 -W $$2 | tail -1`"; done; \
	  echo "guard pages (highest byte really written):"; \
	  $(if $(wildcard $(AIR)),$(BUILD)/arena_guard $(AIR) $(APP_ARENA) 450 2550; $(BUILD)/arena_guard $(AIR) $(APP_ARENA) 200 3000;,echo "(no off-air recording: guard run on the generated file only)";) \
	  $(BUILD)/arena_guard $(DEMO) $(APP_ARENA) 200 3000 ) | tee $(LOGS)/arena.log

abort: $(BUILD)/js8wav $(DEMO) | $(LOGS)
	( $(BUILD)/js8wav $(DEMO) | grep "^E\|^#"; \
	  for n in 1 50 300 800 1500; do echo "abort at poll $$n: `$(BUILD)/js8wav $(DEMO) -q -A $$n | grep -c '^R'` frames; `$(BUILD)/js8wav $(DEMO) -q -A $$n | grep '^#' | tail -2 | paste -s -d' '`"; done ) | tee $(LOGS)/abort.log

snrcal: $(BUILD)/js8sweep | $(LOGS)
	$(PAR) -o $(LOGS)/snr_cal.log -- awgn -s -18,20,2 -n 5 > /dev/null && grep "^S" $(LOGS)/snr_cal.log

osd-exp: $(BUILD)/js8sweep_osd | $(LOGS)
	for o in 1 2; do for i in 0 1 2 3 4 5 6 7; do $(BUILD)/js8sweep_osd awgn -s -23,-15,1 -n 13 -T osd=$$o -S `expr 1000 + $$i` > $(LOGS)/.osd$$i.out & done; wait; \
	  $(PYTHON) tools/js8agg.py --thr $(LOGS)/.osd?.out > $(LOGS)/exp_osd$${o}_awgn.log; grep "^#" $(LOGS)/exp_osd$${o}_awgn.log; \
	  for i in 0 1 2 3 4 5 6 7; do $(BUILD)/js8sweep_osd noise -n 125 -T osd=$$o -S `expr 1000 + $$i` > $(LOGS)/.osd$$i.out & done; wait; \
	  $(PYTHON) tools/js8agg.py $(LOGS)/.osd?.out > $(LOGS)/exp_osd$${o}_noise.log; tail -1 $(LOGS)/exp_osd$${o}_noise.log; done
	for i in 0 1 2 3 4 5 6 7; do rm -f $(LOGS)/.osd$$i.out; done

align-rec: $(BUILD)/js8wav | $(LOGS)
	( for b in "200,3000" "450,2550"; do for k in shift=0 shift=1; do printf "band $$b $$k:"; \
	    for t in 0 0.005 0.010 0.015 0.020 0.025 0.030 0.035 0.040 0.045 0.050 0.055 0.060 0.065 0.070 0.075; do \
	      printf " %s" `$(BUILD)/js8wav $(AIR) -r $(APP_RES) -b $$b -q -t $$t -T $$k | grep -c '^R '`; done; echo; done; done ) | tee $(LOGS)/align.log

coreextra: real-t0 apparena t3seed arena abort snrcal align

# streaming front end (js8_stream_*): frames AND candidate counts must be the same with and without it, wherever the
# stream starts in the capture, for late captures (-n < 0.5), short captures and a band that needs two band passes
stream: $(BUILD)/js8wav $(BUILD)/js8sweep | $(LOGS)
	@[ -n "$(HAVE_REC)" ] || echo "stream: no recordings (RECORDINGS=<dir>) - only the generated clean sweep is compared"
	@bad=0; n=0; for f in $(HAVE_REC); do for o in "" "-n -1.0" "-c 100000" "-b 200,3000"; do \
	    ref=`$(BUILD)/js8wav $$f -t 0 -W $(APP_ARENA) -b 450,2550 $$o | grep '^R\|^E' | sed 's/^\(E [0-9]* [0-9]* [0-9]*\) .*/\1/' | cksum`; \
	    for p in 0 1 640 40000 116800; do n=`expr $$n + 1`; \
	      [ "`$(BUILD)/js8wav $$f -t 0 -W $(APP_ARENA) -b 450,2550 $$o -s $$p | grep '^R\|^E' | sed 's/^\(E [0-9]* [0-9]* [0-9]*\) .*/\1/' | cksum`" = "$$ref" ] || { echo "DIFF $$f $$o -s $$p"; bad=`expr $$bad + 1`; }; \
	    done; done; done > $(LOGS)/stream.log; \
	  echo "stream: $$n runs (recordings x 4 set-ups x 5 start positions), different from the run without stream: $$bad" | tee -a $(LOGS)/stream.log; [ $$bad = 0 ]
	$(BUILD)/js8sweep clean -W $(APP_ARENA) -b 450,2550 -z 0 > $(LOGS)/t2_stream.log; r=$$?; tail -1 $(LOGS)/t2_stream.log; [ $$r = 0 ]

prof: $(BUILD)/js8prof | $(LOGS)
	$(BUILD)/js8prof -W $(APP_ARENA) -b 450,2550 -s auto empty:20 $(if $(wildcard $(AIR)),file:$(AIR)) multi:20 noise:20 | tee $(LOGS)/prof.log

KB_O = $(BUILD)/obj/digi_util.o $(CORE_O)
$(BUILD)/js8kbench: tests/js8kbench.c src/js8dec.c src/js8dec.h $(KB_O)
	$(CC) $(DECFLAGS) $< $(KB_O) -lm -o $@
$(BUILD)/js8kbench_spa: tests/js8kbench.c src/js8dec.c src/js8dec.h $(KB_O)
	$(CC) $(DECFLAGS) -DJS8_BP_SPA $< $(KB_O) -lm -o $@
kbench: $(BUILD)/js8kbench $(BUILD)/js8kbench_spa | $(LOGS)
	( $(BUILD)/js8kbench && $(BUILD)/js8kbench_spa | grep "^BP" ) | tee $(LOGS)/kbench.log

t6:
	BUILD=$(BUILD) sh tests/run_t6.sh asan
rv:
	$(PYTHON) -I -B tests/rv_protocol.py
fuzz:
	BUILD=$(BUILD) sh tests/run_fuzz.sh

test-core: tables t0 t1 freestanding

tables:
	$(PYTHON) tests/model.py --src $(SRCREF) tables src/js8_ldpc_174_87_tables.h

t0: $(BUILD)/t0_tables
	$(BUILD)/t0_tables

$(BUILD)/model_vectors.txt: tests/model.py | $(BUILD)/obj
	$(PYTHON) tests/model.py --src $(SRCREF) gen 1000 20261123 > $@.tmp && mv $@.tmp $@

t1: $(BUILD)/t1_encoder $(BUILD)/model_vectors.txt
	$(BUILD)/t1_encoder $(BUILD)/model_vectors.txt

# the core must compile without libc headers and must not reference any external symbol
freestanding: | $(BUILD)/obj
	@mkdir -p $(BUILD)/obj/fs
	@for f in $(CORE_C); do $(CC) $(FSFLAGS) -c $$f -o $(BUILD)/obj/fs/`basename $${f%.c}`.o || exit 1; done
	@$(CC) -r -nostdlib $(CORE_C:src/%.c=$(BUILD)/obj/fs/%.o) -o $(BUILD)/obj/fs/core.o
	@u=`nm -u $(BUILD)/obj/fs/core.o`; if [ -n "$$u" ]; then echo "FAIL  freestanding: undefined symbols:"; echo "$$u"; exit 1; \
	 else echo "PASS  freestanding: core compiles with -ffreestanding -fno-builtin -nostdinc, no undefined symbols"; fi
	@$(CC) $(FSFLAGS) -std=gnu99 -fno-math-errno -I$(DIGI) -c src/js8dec.c -o $(BUILD)/obj/fs/js8dec.o || exit 1
	@$(CC) $(FSFLAGS) -std=gnu99 -fno-math-errno -I$(DIGI) -c $(DIGI)/digi_util.c -o $(BUILD)/obj/fs/digi_util.o || exit 1
	@$(CC) -r -nostdlib $(BUILD)/obj/fs/core.o $(BUILD)/obj/fs/js8dec.o $(BUILD)/obj/fs/digi_util.o -o $(BUILD)/obj/fs/dec.o
	@u=`nm -u $(BUILD)/obj/fs/dec.o`; if [ -n "$$u" ]; then echo "FAIL  freestanding: decoder has undefined symbols:"; echo "$$u"; exit 1; \
	 else echo "PASS  freestanding: core + src/js8dec.c + SDK digi_util.c, no undefined symbols"; fi

# c2: clean signals through the independent spectrum tool, plus demo files
$(DEMO): $(BUILD)/js8gen $(BUILD)/js8spec
	sh tests/c2_spectrum.sh $(BUILD)
c2: $(BUILD)/js8gen $(BUILD)/js8spec
	sh tests/c2_spectrum.sh $(BUILD)

check: test-core c2

clean:
	rm -rf $(BUILD)/obj $(BINS) $(DECBINS) $(BUILD)/model_vectors.txt $(BUILD)/c2

# ---- radio app: host simulator of the shell, end-to-end test, ARM build ----------------
#   make sim        build the loader simulator with the radio shell inside (js8sim) and js8mk
#   make e2e        scenarios + all real recordings through the simulator -> $(BUILD)/results-e2e.md (~4 min)
#   make arm        JS8.APP for the IC-705 -> $(BUILD)/arm/JS8.APP (needs the ARM toolchain, see shell/build.sh)
SDKROOT ?= $(SDKAPPS)/..
SIM_SRC  = src/js8dec.c src/js8msg.c src/js8jsc.c src/js8asm.c src/js8feat.c src/js8_constants.c src/js8crc.c src/js8enc.c shell/digi_util.c
SHELL_H  = shell/js8app_store.h shell/js8app_tx.h shell/js8app_kbd.h src/js8feat.h src/js8jsc.h src/js8asm.h src/js8msg.h src/js8tx.h
.PHONY: sim e2e arm
sim: $(BUILD)/js8sim $(BUILD)/js8mk
$(BUILD)/js8sim: tests/js8sim.c shell/js8app.c $(SIM_SRC) $(SHELL_H) | $(BUILD)/obj
	$(CC) -std=gnu99 -O2 -Wall -Wextra -Wshadow -fno-math-errno -DDIGI_HOST -I$(SDKROOT) -Ishell -Isrc tests/js8sim.c $(SIM_SRC) -lm -o $@
$(BUILD)/js8mk: tests/js8mk.c src/js8pack.c src/js8msg.c src/js8jsc.c | $(BUILD)/obj
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra tests/js8mk.c src/js8pack.c src/js8msg.c src/js8jsc.c -o $@
e2e: deps dict $(BUILD)/js8gen
	$(if $(strip $(RECORDINGS)),SAMPLES="$(abspath $(RECORDINGS))") nice $(PYTHON) -I -B tests/e2e.py $(SKIP_ARM)
arm:
	OUT=$(BUILD)/arm sh shell/build.sh

# ---- transmit path.  NOTHING HERE TRANSMITS: the TX-capable shell runs only in the simulator. ----
#   make txsim      js8simtx = the shell built with -DJS8_TX (src/js8tx.c, src/js8pack.c, SDK txsynth.c) inside the simulator
#   make t7         engine unit tests against a mock loader (plain and ASan/UBSan), the log-record length test t10 (ASan),
#                   79 tones of 1000 random frames vs tests/model.py
#   make txe2e      loopback / timing / safety / strategies / 2 h soak in the simulator -> $(BUILD)/tx/checks.md (~10 min)
#   make arm-tx     the RX+TX flavour for the IC-705 -> $(BUILD)/arm/FT8.APP (see shell/build.sh for the name)
TXS      = $(DIGI)/txsynth.c
.PHONY: txsim t7 txe2e arm-tx
txsim: $(BUILD)/js8simtx $(BUILD)/txchk
$(BUILD)/js8simtx: tests/js8sim.c shell/js8app.c src/js8tx.c src/js8tx.h src/js8pack.c $(SIM_SRC) $(SHELL_H) | $(BUILD)/obj
	$(CC) -std=gnu99 -O2 -Wall -Wextra -Wshadow -fno-math-errno -ffp-contract=off -DDIGI_HOST -DJS8_TX -DJS8_KBD -I$(SDKROOT) -I$(DIGI) -Ishell -Isrc tests/js8sim.c $(SIM_SRC) src/js8tx.c src/js8pack.c $(TXS) -lm -o $@
$(BUILD)/txchk: tests/txchk.c | $(BUILD)/obj
	$(CC) -std=gnu99 -O2 -Wall -Wextra $< -lm -o $@
# -DJS8_QTX: the tap-grant path (txpath=1), which the radio builds do not contain (src/js8tx.h rule 1); t7 tests it too.
# t7_tx_link: the engine with the entry points of the experimental terminal link compiled in (src/js8tx.c only; the link
# itself is not part of this release)
T7SRC = tests/t7_tx.c src/js8tx.c src/js8pack.c src/js8msg.c src/js8jsc.c $(CORE_C) $(TXS)
$(BUILD)/t7_tx: tests/t7_tx.c src/js8tx.c src/js8tx.h src/js8pack.c src/js8msg.c src/js8jsc.c $(CORE_C) | $(BUILD)/obj
	$(CC) -std=gnu99 -D_GNU_SOURCE -DJS8_QTX -O2 -Wall -Wextra -ffp-contract=off -Isrc -I$(DIGI) $(T7SRC) -lm -o $@
$(BUILD)/san/t7_tx_asan: tests/t7_tx.c src/js8tx.c src/js8tx.h src/js8pack.c src/js8msg.c src/js8jsc.c $(CORE_C) | $(BUILD)/obj
	mkdir -p $(BUILD)/san
	$(CC) -std=gnu99 -D_GNU_SOURCE -DJS8_QTX $(SAN) -Wall -Wextra -ffp-contract=off -Isrc -I$(DIGI) $(T7SRC) -lm -o $@
$(BUILD)/san/t10_txlog_asan: tests/t10_txlog.c src/js8tx.c src/js8tx.h src/js8pack.c src/js8msg.c src/js8jsc.c $(CORE_C) | $(BUILD)/obj
	mkdir -p $(BUILD)/san
	$(CC) -std=gnu99 $(SAN) -Wall -Wextra -ffp-contract=off -Isrc -I$(DIGI) tests/t10_txlog.c src/js8tx.c src/js8pack.c src/js8msg.c src/js8jsc.c $(CORE_C) $(TXS) -lm -o $@
SAN = -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
$(BUILD)/tx/tx_model_vectors.txt: tests/model.py
	mkdir -p $(BUILD)/tx
	$(PYTHON) tests/model.py --src $(SRCREF) gen 1000 20261124 > $@.tmp && mv $@.tmp $@
# the same unit tests with the terminal's entry points compiled in (-DJS8_LINK: js8tx_item / select / confirm)
$(BUILD)/t7_tx_link: tests/t7_tx.c src/js8tx.c src/js8tx.h src/js8pack.c src/js8msg.c src/js8jsc.c $(CORE_C) | $(BUILD)/obj
	$(CC) -std=gnu99 -D_GNU_SOURCE -DJS8_QTX -DJS8_LINK -O2 -Wall -Wextra -ffp-contract=off -Isrc -I$(DIGI) $(T7SRC) -lm -o $@
t7: $(BUILD)/t7_tx $(BUILD)/t7_tx_link $(BUILD)/san/t7_tx_asan $(BUILD)/san/t10_txlog_asan $(BUILD)/tx/tx_model_vectors.txt
	$(BUILD)/t7_tx unit
	$(BUILD)/t7_tx_link unit
	$(BUILD)/san/t7_tx_asan unit
	$(BUILD)/san/t10_txlog_asan
	$(BUILD)/t7_tx vectors $(BUILD)/tx/tx_model_vectors.txt
txe2e: deps dict $(BUILD)/js8gen
	@[ -f $(BUILD)/e2e/mix.wav ] || $(MAKE) e2e
	$(if $(strip $(RECORDINGS)),SAMPLES="$(abspath $(RECORDINGS))") nice -n 19 $(PYTHON) -I -B tests/tx_e2e.py $(SKIP_ARM)
# the keyboard control, the review regressions at shell level, green frame, HB beacon (tests/fix_e2e.py) -> $(BUILD)/rfix/checks.md
fixe2e: deps dict
	@[ -f $(BUILD)/e2e/mix.wav ] || $(MAKE) e2e
	nice -n 19 $(PYTHON) -I -B tests/fix_e2e.py
arm-tx:
	FLAVOUR=tx OUT=$(BUILD)/arm sh shell/build.sh


# ---- portable entry points for a fresh clone: fetch, test, test-full, test-recordings, dist
include public.mk
