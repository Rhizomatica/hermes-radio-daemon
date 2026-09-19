/* ambe_roundtrip — encode and decode 8 kHz speech through mbelib, no radio.
 *
 * Separates "our AMBE encode is bad" from "the D-STAR air path is bad":
 * this is the same encoder and decoder the daemon uses, wired mouth to ear.
 *
 * usage: ambe_roundtrip in8k.wav out8k.wav
 * Copyright (C) 2026 Rhizomatica — SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <mbelib-neo/mbelib.h>

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s in8k.wav out8k.wav\n", argv[0]); return 1; }
    FILE *fi = fopen(argv[1], "rb"); if (!fi) { perror("in"); return 1; }
    unsigned char hdr[44];
    if (fread(hdr, 1, 44, fi) != 44) return 1;
    FILE *fo = fopen(argv[2], "wb"); if (!fo) { perror("out"); return 1; }
    fwrite(hdr, 1, 44, fo);

    mbe_parms ecur, eprev, edummy, dcur, dprev, denh;
    mbe_initMbeParms(&ecur, &eprev, &edummy);
    mbe_initMbeParms(&dcur, &dprev, &denh);

    short pcm_in[160], pcm_out[160];
    float f_in[160];
    char ambe_d[49], fr[4][24], ambe_d2[49];
    unsigned char frame9[9];
    long frames = 0, bytes = 0, lvl_n = 0;
    double err_sum = 0, lvl_sum = 0;

    while (fread(pcm_in, 2, 160, fi) == 160) {
        for (int i = 0; i < 160; i++) f_in[i] = pcm_in[i] / 32768.0f;

        mbe_encodeAmbe2400Parms(f_in, ambe_d, &ecur, &eprev);
        mbe_encodeAmbe3600x2400Frame(ambe_d, (char(*)[24])fr);
        mbe_encodeDStarDVData((const char(*)[24])fr, frame9);

        /* ...and straight back, exactly as the receiver does */
        char fr2[4][24];
        mbe_process_result res;
        memset(&res, 0, sizeof(res));
        mbe_decodeDStarDVData(frame9, (char(*)[24])fr2);
        mbe_processAmbe3600x2400Frame(pcm_out, &res, (const char(*)[24])fr2, ambe_d2,
                                      &dcur, &dprev, &denh);
        err_sum += res.total_errors;
        {   /* per-frame level error: what the encoder's gain loop must cancel */
            double ei = 0, eo = 0;
            for (int i = 0; i < 160; i++) { ei += (double)pcm_in[i]*pcm_in[i]; eo += (double)pcm_out[i]*pcm_out[i]; }
            if (ei > 1e4 && eo > 1e4) { lvl_sum += 10.0*log10(eo/ei); lvl_n++; }
        }
        fwrite(pcm_out, 2, 160, fo);
        frames++; bytes += 9;
    }
    /* Patch the RIFF/data sizes: the header was copied from the input, whose
     * length does not match what we wrote. */
    long end = ftell(fo);
    unsigned int riff = (unsigned int)(end - 8), data = (unsigned int)(end - 44);
    fseek(fo, 4, SEEK_SET);  fwrite(&riff, 4, 1, fo);
    fseek(fo, 40, SEEK_SET); fwrite(&data, 4, 1, fo);

    if (lvl_n) printf("level error: %+.1f dB (output vs input, %ld speech frames)\n", lvl_sum/lvl_n, lvl_n);
    printf("round trip: %ld frames (%.1f s), mean FEC errors %.2f\n",
           frames, frames*0.02, frames ? err_sum/frames : 0.0);
    fclose(fi); fclose(fo);
    return 0;
}
