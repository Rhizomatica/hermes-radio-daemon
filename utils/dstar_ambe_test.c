/* dstar_ambe_test — find the correct AMBE bit/byte packing objectively.
 *
 * mbelib reports the Golay/Hamming error count it had to correct per frame.
 * With the right packing that count is low; with the wrong one the FEC is
 * decoding noise and the count sits near its maximum. So feed every received
 * frame through several candidate transforms and compare — no listening
 * required.
 *
 * usage: dstar_ambe_test dump.s16 [polarity]
 *
 * Copyright (C) 2026 Rhizomatica — SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp/sbitx_dstar.h"
#include <mbelib-neo/mbelib.h>

#define NVAR 6

static const char *names[NVAR] = {
    "bytes[0..8] as-is",
    "bytes[3..11] as-is",
    "bytes[0..8] bit-reversed",
    "bytes[3..11] bit-reversed",
    "bytes[0..8] reversed order",
    "bytes[0..8] nibble-swapped",
};

static mbe_parms cur[NVAR], prev[NVAR], enh[NVAR];
static double err_sum[NVAR];
static long   err_n[NVAR];
/* Pitch plausibility: real speech has a fundamental in roughly 60-400 Hz that
 * moves smoothly frame to frame. Decoded garbage jumps around the whole
 * range, so the fraction of frames whose pitch is both in range and within
 * 25%% of the previous frame separates a correct unpacking from a wrong one
 * far more sharply than the FEC error count does. */
static double prev_f0[NVAR];
static long   n_plausible[NVAR], n_voiced[NVAR];
static FILE  *wav[NVAR];

static uint8_t bitrev(uint8_t b)
{
    b = (uint8_t)((b & 0xF0U) >> 4 | (b & 0x0FU) << 4);
    b = (uint8_t)((b & 0xCCU) >> 2 | (b & 0x33U) << 2);
    b = (uint8_t)((b & 0xAAU) >> 1 | (b & 0x55U) << 1);
    return b;
}

static void variant(int v, const uint8_t *f, uint8_t *out9)
{
    switch (v) {
    case 0: memcpy(out9, f, 9); break;
    case 1: memcpy(out9, f + 3, 9); break;
    case 2: for (int i = 0; i < 9; i++) out9[i] = bitrev(f[i]); break;
    case 3: for (int i = 0; i < 9; i++) out9[i] = bitrev(f[i + 3]); break;
    case 4: for (int i = 0; i < 9; i++) out9[i] = f[8 - i]; break;
    case 5: for (int i = 0; i < 9; i++) out9[i] = (uint8_t)((f[i] << 4) | (f[i] >> 4)); break;
    }
}

static void data_cb(void *u, const uint8_t *f)
{
    (void)u;
    for (int v = 0; v < NVAR; v++) {
        uint8_t b9[9];
        char fr[4][24], ambe_d[49];
        short pcm[160];
        mbe_process_result res;
        memset(&res, 0, sizeof(res));
        variant(v, f, b9);
        mbe_decodeDStarDVData(b9, (char(*)[24])fr);
        mbe_processAmbe3600x2400Frame(pcm, &res, (const char(*)[24])fr, ambe_d,
                                      &cur[v], &prev[v], &enh[v]);
        err_sum[v] += res.total_errors;
        err_n[v]++;

        float w0 = cur[v].w0;
        if (w0 > 0.0f) {
            double f0 = (double) w0 * 8000.0 / (2.0 * 3.14159265358979);
            if (f0 > 60.0 && f0 < 400.0) {
                n_voiced[v]++;
                if (prev_f0[v] > 0.0) {
                    double r = f0 / prev_f0[v];
                    if (r > 0.75 && r < 1.333)
                        n_plausible[v]++;
                }
                prev_f0[v] = f0;
            } else {
                prev_f0[v] = 0.0;
            }
        }
        if (wav[v]) fwrite(pcm, 2, 160, wav[v]);
    }
}
static void hdr_cb(void *u, const uint8_t *h) { (void)u; (void)h; }
static void lost_cb(void *u) { (void)u; }
static void eot_cb(void *u) { (void)u; }

static void w32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void w16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s dump.s16 [polarity]\n", argv[0]); return 1; }
    float pol = (argc > 2) ? (float)atof(argv[2]) : -1.0f;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f) / 2; fseek(f, 0, SEEK_SET);
    short *s = malloc((size_t)n * sizeof(short));
    if (fread(s, 2, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);

    for (int v = 0; v < NVAR; v++) {
        mbe_initMbeParms(&cur[v], &prev[v], &enh[v]);
        char path[64];
        snprintf(path, sizeof(path), "/tmp/ambe_var%d.wav", v);
        wav[v] = fopen(path, "wb");
        if (wav[v]) {   /* 8 kHz mono 16-bit header, patched at exit */
            fwrite("RIFF", 1, 4, wav[v]); w32(wav[v], 0); fwrite("WAVEfmt ", 1, 8, wav[v]);
            w32(wav[v], 16); w16(wav[v], 1); w16(wav[v], 1); w32(wav[v], 8000);
            w32(wav[v], 16000); w16(wav[v], 2); w16(wav[v], 16);
            fwrite("data", 1, 4, wav[v]); w32(wav[v], 0);
        }
    }

    sbitx_dstar_rx *rx = sbitx_dstar_rx_new();
    sbitx_dstar_rx_set_cbs(rx, hdr_cb, data_cb, lost_cb, eot_cb, NULL);
    sbitx_dstar_rx_set_polarity(rx, pol);

    float buf[256];
    for (long fed = 0; fed < n; ) {
        int m = (int)((n - fed) < 256 ? (n - fed) : 256);
        for (int i = 0; i < m; i++) buf[i] = (float)s[fed + i] / 32768.0f;
        sbitx_dstar_rx_process(rx, buf, m);
        fed += m;
    }

    printf("frames: %ld\n", err_n[0]);
    printf("%-30s %10s %10s %12s\n", "variant", "FEC errs", "in-range", "smooth pitch");
    for (int v = 0; v < NVAR; v++) {
        printf("%-30s %10.2f %9.1f%% %11.1f%%   -> /tmp/ambe_var%d.wav\n",
               names[v], err_n[v] ? err_sum[v] / err_n[v] : -1.0,
               err_n[v] ? 100.0 * n_voiced[v] / err_n[v] : 0.0,
               n_voiced[v] ? 100.0 * n_plausible[v] / n_voiced[v] : 0.0, v);
        if (wav[v]) {
            long sz = ftell(wav[v]);
            fseek(wav[v], 4, SEEK_SET);  w32(wav[v], (uint32_t)(sz - 8));
            fseek(wav[v], 40, SEEK_SET); w32(wav[v], (uint32_t)(sz - 44));
            fclose(wav[v]);
        }
    }
    sbitx_dstar_rx_free(rx);
    return 0;
}
