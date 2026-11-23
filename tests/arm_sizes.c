/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * arm_sizes.c - size probe: compiled (never linked, never run) with the ARM compiler and the flags of
 * shell/build.sh; the object's symbol sizes (nm -S) are the struct sizes ON THE RADIO.  The result
 * files quoted the PC's sizes as the radio's; tests/e2e.py reads these instead:
 *   shell reserve = align8(js8asm_t) + align8(js8jsc_t) + JS8_HEAD_MAX + JS8_LOG_SZ + JS8_MSG_SZ + JS8_RDBLK (4096)
 *   decoder arena = 245760 - shell reserve
 */
#include "../src/js8asm.h"
#include "../src/js8jsc.h"
#include "../src/js8dec.h"

char js8sz_asm[sizeof(js8asm_t)];
char js8sz_asm_msg[sizeof(js8asm_msg_t)];
char js8sz_asm_stream[sizeof(js8asm_stream_t)];
char js8sz_jsc[sizeof(js8jsc_t)];
char js8sz_msg[sizeof(js8_msg_t)];
char js8sz_result[sizeof(js8_result_t)];
