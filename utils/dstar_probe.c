/* dstar_probe — feed a 24 kHz s16 discriminator dump to the modem exactly the
 * way the daemon does (set_polarity + small blocks), and count what comes out.
 * Isolates "the modem cannot decode these samples" from "the daemon is not
 * feeding/reporting them properly".
 *
 * usage: dstar_probe dump.s16 [polarity] [block]
 *
 * Copyright (C) 2026 Rhizomatica — SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp/sbitx_dstar.h"

static long n_hdr, n_data, n_lost, n_eot;

static int slow_dumped;
static void slow_dbg(void *u, const uint8_t *h, bool ok)
{
    (void)u;
    if (slow_dumped++ >= 3) return;
    printf("  slow-assembled (crc %s):", ok ? "OK" : "BAD");
    for (int i = 0; i < 41; i++) printf(" %02x", h[i]);
    printf("\n     as text: |");
    for (int i = 0; i < 41; i++) putchar((h[i] >= 32 && h[i] < 127) ? h[i] : '.');
    printf("|\n");
}

static void hdr_cb(void *u, const uint8_t *h)
{
    printf("  burst  header       :");
    for (int i = 0; i < 41; i++) printf(" %02x", h[i]);
    printf("\n     as text: |");
    for (int i = 0; i < 41; i++) putchar((h[i] >= 32 && h[i] < 127) ? h[i] : '.');
    printf("|\n");
    (void)u; char ur[9]={0}, my[9]={0};
    memcpy(ur, h+19, 8); memcpy(my, h+27, 8);
    printf("  HEADER  ur=%s my=%s flags=%02x\n", ur, my, h[0]);
    n_hdr++;
}
/* The D-STAR null (silence) AMBE frame. A real transmission is full of these
 * between syllables, so finding it tells us exactly where the AMBE payload
 * starts inside the 12-byte frame. */
static const uint8_t NULL_AMBE[9] = {0x9E,0x8D,0x32,0x88,0x26,0x1A,0x3F,0x61,0xE8};
static long n_null_at0, n_null_at3;

/* D-STAR scrambles the 3 slow-data bytes of every frame with this repeating
 * pattern; frame 0 of each superframe carries the sync instead. */
static const uint8_t SLOW_SCRAMBLE[3] = {0x70, 0x4F, 0x93};
static int slow_idx;

static void data_cb(void *u, const uint8_t *f)
{
    (void)u;
    /* A frame whose slow-data field is the sync pattern starts a superframe. */
    if (f[9] == 0x55 && f[10] == 0x2D && f[11] == 0x16)
        slow_idx = 0;
    else
        slow_idx++;

    if (n_data < 24) {
        uint8_t d0 = f[9] ^ SLOW_SCRAMBLE[0];
        uint8_t d1 = f[10] ^ SLOW_SCRAMBLE[1];
        uint8_t d2 = f[11] ^ SLOW_SCRAMBLE[2];
        printf("  frame %2ld (slot %2d): raw %02x %02x %02x -> descrambled %02x %02x %02x  |%c%c%c|\n",
               n_data, slow_idx, f[9], f[10], f[11], d0, d1, d2,
               (d0 >= 32 && d0 < 127) ? d0 : '.',
               (d1 >= 32 && d1 < 127) ? d1 : '.',
               (d2 >= 32 && d2 < 127) ? d2 : '.');
    }
    if (!memcmp(f + 0, NULL_AMBE, 9)) n_null_at0++;
    if (!memcmp(f + 3, NULL_AMBE, 9)) n_null_at3++;
    n_data++;
}
static void lost_cb(void *u) { (void)u; n_lost++; }
static void eot_cb(void *u)  { (void)u; n_eot++; }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s dump.s16 [polarity] [block]\n", argv[0]); return 1; }
    float pol = (argc > 2) ? (float)atof(argv[2]) : -1.0f;
    int   blk = (argc > 3) ? atoi(argv[3]) : 256;    /* the daemon's n24 */

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f) / 2; fseek(f, 0, SEEK_SET);
    short *s = malloc((size_t)n * sizeof(short));
    if (fread(s, 2, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    sbitx_dstar_rx *rx = sbitx_dstar_rx_new();
    sbitx_dstar_rx_set_cbs(rx, hdr_cb, data_cb, lost_cb, eot_cb, NULL);
    sbitx_dstar_rx_set_polarity(rx, pol);
    sbitx_dstar_rx_set_slow_debug(rx, slow_dbg);

    float *buf = malloc((size_t)blk * sizeof(float));
    long fed = 0;
    while (fed < n) {
        int m = (int)((n - fed) < blk ? (n - fed) : blk);
        for (int i = 0; i < m; i++)
            buf[i] = (float)s[fed + i] / 32768.0f;
        sbitx_dstar_rx_process(rx, buf, m);
        fed += m;
    }

    sbitx_dstar_rx_stats st;
    sbitx_dstar_rx_get_stats(rx, &st);
    printf("  header path: preamble found %u, CRC ok %u (soft %u), CRC bad %u; "
           "slow-data headers %u; data-sync locks %u\n",
           st.frame_sync, st.header_ok, st.header_soft_ok, st.header_bad,
           st.header_slow, st.data_sync);
    printf("polarity %+.0f, block %d, %.2f s fed: header=%ld data=%ld lost=%ld eot=%ld\n",
           pol, blk, n / 24000.0, n_hdr, n_data, n_lost, n_eot);
    printf("  null-AMBE pattern found at offset 0: %ld frames, at offset 3: %ld frames\n",
           n_null_at0, n_null_at3);
    sbitx_dstar_rx_free(rx);
    return 0;
}
