/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Tomas Brincil OK1BT. Written by Claude (Anthropic) under his direction; see AUTHORS.md.
 *
 * snr_dump - review tool (PC only): writes to a WAV file exactly what "js8sweep awgn" feeds to the
 * decoder (same js8synth.h calls in the same order: rand_frame, js8_encode, sy_add_cpfsk with
 * sy_amp_for_snr(snr, 300), sy_add_noise(300), sy_quantize), so that tests/snr_measure.py can
 * measure the SNR of the file without using the generator's formula.
 *   snr_dump OUT.wav SNR_DB F0 PERIODS [SEED]      (dt = 0: every frame starts 0.5 s into its period)
 */
#include "../src/js8_constants.h"
#include "../src/js8crc.h"
#include "../src/js8enc.h"
#include "js8synth.h"
#define PERIOD 120000
static double fbuf[PERIOD];
int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: snr_dump OUT.wav SNR_DB F0 PERIODS [SEED]\n"); return 2; }
    double snr = atof(argv[2]), f0 = atof(argv[3]);
    int np = atoi(argv[4]);
    int16_t *pcm = (int16_t *)malloc((size_t)np * PERIOD * 2);
    sy_rng_s = argc > 5 ? strtoull(argv[5], NULL, 0) : 1;
    for (int p = 0; p < np; p++) {
        uint8_t f[11], tones[79];
        for (int i = 0; i < 11; i++) f[i] = (uint8_t)(sy_rng64() >> 24);
        f[9] &= 0xE0; f[10] = 0;
        js8_crc12_store(f, js8_crc12(f));
        js8_encode(f, js8_costas_normal, tones);
        memset(fbuf, 0, sizeof fbuf);
        sy_add_cpfsk(fbuf, PERIOD, tones, f0, 0.5 * SY_FS, sy_amp_for_snr(snr, 300.0));
        sy_add_noise(fbuf, PERIOD, 300.0);
        sy_quantize(fbuf, pcm + (size_t)p * PERIOD, PERIOD);
    }
    return sy_save_wav(argv[1], pcm, (long)np * PERIOD) ? 1 : 0;
}
