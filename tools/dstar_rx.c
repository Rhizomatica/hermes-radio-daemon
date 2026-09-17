/* dstar_rx — offline D-STAR DV decoder.
 *
 * Reads a WAV of FM-discriminator audio (the IC-7100 DV data port, or an
 * SDR FM-demod capture) and decodes it: GMSK demod -> DV frames -> AMBE
 * decode -> 8 kHz speech WAV.
 *
 * usage: dstar_rx input_disc.wav output.wav
 *   The input is resampled to 24 kHz (the modem rate). If decoding fails,
 *   try flipping the polarity with POLARITY=1 (some data ports invert the
 *   discriminator sign).
 *
 * Copyright (C) 2026 Rhizomatica — SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "dsp/sbitx_dstar.h"
#include <mbelib-neo/mbelib.h>

static FILE *fout;
static mbe_parms cur, prev, enh;
static int  first_frame = 1;

static void header_cb(void *user, const uint8_t *h) {
    (void)user;
    char ur[9] = {0}, my[9] = {0};
    memcpy(ur, h + 19, 8); memcpy(my, h + 27, 8);
    fprintf(stderr, "DSTAR header: ur=%s my=%s\n", ur, my);
}

static void data_cb(void *user, const uint8_t *f) {
    (void)user;
    char fr[4][24], ambe_d[49];
    short pcm[160];
    mbe_decodeDStarDVData(f + 3, (char(*)[24])fr);
    mbe_processAmbe3600x2400Frame(pcm, NULL, (const char(*)[24])fr, ambe_d, &cur, &prev, &enh);
    if (ambe_d[0] && ambe_d[1] && ambe_d[2] && ambe_d[3] && ambe_d[4] && ambe_d[5] && ambe_d[48])
        memset(pcm, 0, sizeof(pcm));
    for (int i = 0; i < 160; i++) {
        int v = (int)pcm[i] * 20;
        if (v > 31128) v = 31128;
        if (v < -31128) v = -31128;
        pcm[i] = (short)v;
    }
    fwrite(pcm, 2, 160, fout);
    first_frame = 0;
}

static void lost_cb(void *user) { (void)user; fprintf(stderr, "DSTAR: lost sync\n"); }
static void eot_cb(void *user)  { (void)user; fprintf(stderr, "DSTAR: end of transmission\n"); }

static void w32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void w16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

/* Minimal WAV reader: returns sample rate and points `data` to the PCM. */
static int read_wav(const char *path, short **data, int *rate, int *nsamp) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t u32; uint16_t u16;
    fread(&u32, 4, 1, f); fread(&u32, 4, 1, f); fread(&u32, 4, 1, f); /* RIFF size WAVE */
    int ch = 0, bits = 0, sr = 0; uint32_t dsize = 0;
    for (;;) {
        char tag[4] = {0};
        uint32_t csize;
        if (fread(tag, 4, 1, f) != 1) break;
        fread(&csize, 4, 1, f);
        if (!memcmp(tag, "fmt ", 4)) {
            fread(&u16, 2, 1, f); fread(&u16, 2, 1, f); ch = u16;
            fread(&u32, 4, 1, f); sr = u32;
            fread(&u32, 4, 1, f); fread(&u16, 2, 1, f); fread(&u16, 2, 1, f); bits = u16;
            if (csize > 16) fseek(f, csize - 16, SEEK_CUR);
        } else if (!memcmp(tag, "data", 4)) {
            dsize = csize; break;
        } else {
            fseek(f, (csize & 1) ? csize + 1 : csize, SEEK_CUR);
        }
    }
    if (ch != 1 || bits != 16 || dsize == 0) { fclose(f); return -1; }
    *rate = sr;
    *nsamp = dsize / 2;
    *data = malloc(dsize);
    fread(*data, 1, dsize, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s input_disc.wav output.wav\n", argv[0]); return 1; }
    short *in; int rate, nsamp;
    if (read_wav(argv[1], &in, &rate, &nsamp) < 0) {
        fprintf(stderr, "could not read %s (need mono 16-bit WAV)\n", argv[1]); return 1;
    }
    int polarity = getenv("POLARITY") && atoi(getenv("POLARITY")) ? -1 : 1;

    fout = fopen(argv[2], "wb");
    w32(fout, 0x46464952); w32(fout, 36); w32(fout, 0x45564157);
    w32(fout, 0x20746d66); w32(fout, 16); w16(fout, 1); w16(fout, 1);
    w32(fout, 8000); w32(fout, 16000); w16(fout, 2); w16(fout, 16);
    w32(fout, 0x61746164); w32(fout, 0);

    mbe_initMbeParms(&cur, &prev, &enh);

    sbitx_dstar_rx *rx = sbitx_dstar_rx_new();
    sbitx_dstar_rx_set_cbs(rx, header_cb, data_cb, lost_cb, eot_cb, NULL);

    /* resample input -> 24 kHz, feed the modem in chunks */
    float buf[24000];
    int  pos = 0;
    double rstep = (double)rate / 24000.0;
    double rpos = 0.0;
    while ((int)rpos < nsamp) {
        int i0 = (int)rpos;
        int i1 = i0 + 1; if (i1 >= nsamp) i1 = nsamp - 1;
        float frac = (float)(rpos - (double)i0);
        float s = ((float)in[i0] * (1.0f - frac) + (float)in[i1] * frac) / 32768.0f * (float)polarity;
        buf[pos++] = s;
        if (pos == 24000) {
            sbitx_dstar_rx_process(rx, buf, 24000);
            pos = 0;
        }
        rpos += rstep;
    }
    if (pos > 0) sbitx_dstar_rx_process(rx, buf, pos);

    sbitx_dstar_rx_free(rx);

    /* patch the WAV data size */
    long total = ftell(fout) - 44;
    fseek(fout, 4, SEEK_SET); w32(fout, 36 + (uint32_t)total);
    fseek(fout, 40, SEEK_SET); w32(fout, (uint32_t)total);
    fclose(fout);
    free(in);
    fprintf(stderr, "done\n");
    return 0;
}
