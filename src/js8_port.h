/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8_port.h - the only place where the JS8 core touches "libc".
 * The core (js8_constants.c, js8crc.c, js8enc.c, later js8dec.c/js8msg.c) must build
 * freestanding for the radio (ARM, -nostdlib -fno-builtin) and hosted for the PC test bench.
 * It therefore never calls memset/memcpy/strlen directly, only these inline replacements.
 * (On ARM the build uses -fno-tree-loop-distribute-patterns so the loops are not turned
 * back into libc calls.)
 */
#ifndef JS8_PORT_H
#define JS8_PORT_H

#include <stdint.h>
#include <stddef.h>

static inline void js8_memset(void *dst, int v, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)v;
}

static inline void js8_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static inline int js8_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (; n--; x++, y++)
        if (*x != *y) return (int)*x - (int)*y;
    return 0;
}

#endif /* JS8_PORT_H */
