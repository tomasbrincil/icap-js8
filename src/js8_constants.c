/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8_constants.c - JS8 tables. The LDPC tables come from the machine-generated header
 * (derived from js8call/js8call@a7ff1be, originally WSJT-X; GPLv3 with no "or later" statement
 * upstream). The Costas arrays and the alphabet below are upstream material copied as data, so
 * this file is GPL-3.0-only like that header - see NOTICE.md. Freestanding C99, no libc.
 */
#include "js8_constants.h"
#include "js8_ldpc_174_87_tables.h" /* defines kJS8_LDPC_* (only here) */

/* genjs8.f90:24-27, JS8.hpp:36-38 */
const uint8_t js8_costas_normal[3][7] = {
    { 4, 2, 5, 6, 1, 3, 0 },
    { 4, 2, 5, 6, 1, 3, 0 },
    { 4, 2, 5, 6, 1, 3, 0 },
};

#ifdef JS8_WITH_MODIFIED_COSTAS
/* genjs8.f90:29-31, JS8.hpp:42-44 */
const uint8_t js8_costas_modified[3][7] = {
    { 0, 6, 2, 3, 5, 4, 1 },
    { 1, 5, 0, 2, 3, 6, 4 },
    { 2, 5, 0, 6, 4, 1, 3 },
};
#endif

/* JS8.cpp:849 */
const char js8_alphabet[65] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+";

int js8_alphabet_index(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c >= 'a' && c <= 'z') return c - 'a' + 36;
    if (c == '-') return 62;
    if (c == '+') return 63;
    return -1;
}
