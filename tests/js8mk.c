/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * js8mk - PC-only: text of one JS8 message -> its frames (js8pack), for building test scenarios
 * with js8gen.  Nothing transmits; it prints one line per frame:
 *     <hex22> <type> <i3> <text js8msg gives for the frame>
 * type: 0 heartbeat/CQ, 1 compound, 2 compound directed, 3 directed, 4 Huffman data,
 *       6 JSC (compressed) data, 7 fast data.
 * usage: js8mk -c MYCALL [-g GRID] [-i] [-J SD_DIR] TEXT
 *   -i        identify: prepend "MYCALL: " to plain free text (JS8Call does this for typed text)
 *   -J DIR    directory with the JSC dictionary files (tools/mkjsc.py); without it only Huffman
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../src/js8pack.h"

static const char *sd_dir;
static FILE *fds[4];
static int io_open(void *u, const char *name)
{
    char path[1024]; int i; (void)u;
    for (i = 0; i < 4 && fds[i]; i++) ;
    if (i == 4 || !sd_dir) return -2;
    snprintf(path, sizeof path, "%s/%s", sd_dir, name);
    fds[i] = fopen(path, "rb");
    return fds[i] ? i : -1;
}
static int io_read(void *u, int fd, void *buf, uint32_t len) { (void)u; return (int)fread(buf, 1, len, fds[fd]); }
static int io_close(void *u, int fd) { (void)u; fclose(fds[fd]); fds[fd] = 0; return 0; }
static const js8_io_t io = { io_open, io_read, io_close, 0 };

int main(int argc, char **argv)
{
    static uint8_t fr[64][JS8_FRAME_BYTES], head[40000];
    static js8jsc_t jsc;
    js8_build_t cfg = { "", 0, 0, 0 };
    int o, n, i, j;
    while ((o = getopt(argc, argv, "c:g:iJ:")) != -1) {
        if (o == 'c') cfg.mycall = optarg; else if (o == 'g') cfg.mygrid = optarg;
        else if (o == 'i') cfg.identify = 1; else if (o == 'J') sd_dir = optarg; else return 2;
    }
    if (optind >= argc) { fprintf(stderr, "usage: js8mk -c MYCALL [-g GRID] [-i] [-J SD_DIR] TEXT\n"); return 2; }
    if (sd_dir) {
        if (js8jsc_init(&jsc, &io, head, sizeof head)) { fprintf(stderr, "js8mk: no dictionary in %s\n", sd_dir); return 1; }
        cfg.jsc = &jsc;
    }
    n = js8_build_message(&cfg, argv[optind], fr, 64);
    if (n <= 0) { fprintf(stderr, "js8mk: cannot build (%d)\n", n); return 1; }
    for (i = 0; i < n; i++) {
        js8_msg_t m;
        js8_msg_decode(fr[i], cfg.jsc, &m);
        for (j = 0; j < JS8_FRAME_BYTES; j++) printf("%02x", fr[i][j]);
        printf(" %d %d %s\n", m.type, m.i3, m.text);
    }
    return 0;
}
