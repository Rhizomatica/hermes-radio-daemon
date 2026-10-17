/* dstar_tx_gen — generate a D-STAR DV transmission with the real modulator.
 *
 * Writes the 24 kHz GMSK baseband (the signal that drives an FM modulator's
 * deviation) as float32, so the RX chain can be tested end to end without a
 * radio: header burst + N data frames + EOT.
 *
 * usage: dstar_tx_gen out.f32 [frames]
 *
 * Copyright (C) 2026 Rhizomatica — SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp/sbitx_dstar.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s out.f32 [frames]\n", argv[0]); return 1; }
    int frames = (argc > 2) ? atoi(argv[2]) : 100;   /* 100 frames = 2 s of voice */

    FILE *f = fopen(argv[1], "wb");
    if (!f) { perror("fopen"); return 1; }

    sbitx_dstar_tx *tx = sbitx_dstar_tx_new();
    if (!tx) { fprintf(stderr, "tx_new failed\n"); return 1; }

    /* A DV voice header: flags 0x10, callsigns as a real station would send. */
    uint8_t hdr[SBITX_DSTAR_HEADER_BYTES];
    memset(hdr, ' ', sizeof(hdr));
    hdr[0] = 0x10; hdr[1] = 0x00; hdr[2] = 0x00;
    memcpy(hdr +  3, "DIRECT  ", 8);   /* RPT2 */
    memcpy(hdr + 11, "DIRECT  ", 8);   /* RPT1 */
    memcpy(hdr + 19, "CQCQCQ  ", 8);   /* UR   */
    memcpy(hdr + 27, "PU2UIT  ", 8);   /* MY   */
    memcpy(hdr + 35, "TEST", 4);       /* MY suffix */
    sbitx_dstar_header_finalize(hdr);

    if (sbitx_dstar_tx_header(tx, hdr) != 0) { fprintf(stderr, "queue header failed\n"); return 1; }

    /* The standard D-STAR null AMBE frame (silence) plus the sync/slow-data
     * bytes the modem expects at the head of each 12-byte frame. */
    static const uint8_t null_ambe[9] = {
        0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8
    };

    long total = 0;
    float buf[8192];

    /* Drain whatever the header burst produced first. */
    for (;;) {
        int n = sbitx_dstar_tx_generate(tx, buf, (int)(sizeof(buf)/sizeof(buf[0])));
        if (n <= 0) break;
        fwrite(buf, sizeof(float), (size_t)n, f); total += n;
    }

    for (int i = 0; i < frames; i++) {
        uint8_t frame[SBITX_DSTAR_FRAME_BYTES];
        /* 9 bytes of AMBE, then the sync (frame 0 of each superframe) or
         * slow data. Filler here; the daemon sends the real header. */
        memcpy(frame, null_ambe, 9);
        if ((i % 21) == 0) {
            frame[9] = 0x55; frame[10] = 0x2D; frame[11] = 0x16;
        } else {
            frame[9] = 0x66 ^ 0x70; frame[10] = 0x66 ^ 0x4F; frame[11] = 0x66 ^ 0x93;
        }
        if (sbitx_dstar_tx_frame(tx, frame) != 0) { fprintf(stderr, "queue frame %d failed\n", i); break; }
        for (;;) {
            int n = sbitx_dstar_tx_generate(tx, buf, (int)(sizeof(buf)/sizeof(buf[0])));
            if (n <= 0) break;
            fwrite(buf, sizeof(float), (size_t)n, f); total += n;
        }
    }

    sbitx_dstar_tx_eot(tx);
    for (;;) {
        int n = sbitx_dstar_tx_generate(tx, buf, (int)(sizeof(buf)/sizeof(buf[0])));
        if (n <= 0) break;
        fwrite(buf, sizeof(float), (size_t)n, f); total += n;
    }

    fclose(f);
    sbitx_dstar_tx_free(tx);
    fprintf(stderr, "wrote %ld samples (%.2f s at 24 kHz) to %s\n",
            total, total / 24000.0, argv[1]);
    return 0;
}
