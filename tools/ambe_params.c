/* ambe_params — compare the AMBE parameters our encoder chooses against the
 * ones a real rig's encoder produces, through the same decoder.
 *
 *   -e in8k.wav        encode speech here, report the quantised parameters
 *   -d disc24k.s16     demodulate a captured D-STAR signal, report the same
 *
 * Reports per frame: pitch, harmonic count L, voiced fraction, and the
 * spectral tilt of the harmonic amplitudes (upper third vs lower third, dB).
 * A more negative tilt means the encoder is putting less energy up high —
 * which is what "muffled" sounds like.
 *
 * Copyright (C) 2026 Rhizomatica — SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp/sbitx_dstar.h"
#include <mbelib-neo/mbelib.h>

static long n_frames;
static double sum_w0, sum_L, sum_voiced, sum_tilt, sum_gamma, sum_gamma2;
static long n_tilt;

static void account(const mbe_parms *p)
{
    if (p->L <= 3) return;
    double f0 = (double) p->w0 * 8000.0 / (2.0 * M_PI);
    if (f0 < 50.0 || f0 > 500.0) return;

    int lo_n = p->L / 3, hi_lo = p->L - p->L / 3;
    double lo = 0, hi = 0;
    int nlo = 0, nhi = 0;
    for (int l = 1; l <= p->L; l++) {
        double m = p->Ml[l];
        if (m <= 0) continue;
        if (l <= lo_n)      { lo += m * m; nlo++; }
        else if (l > hi_lo) { hi += m * m; nhi++; }
    }
    int voiced = 0;
    for (int l = 1; l <= p->L; l++) voiced += p->Vl[l] ? 1 : 0;

    sum_w0 += f0; sum_L += p->L; sum_gamma += p->gamma; sum_gamma2 += (double)p->gamma*p->gamma;
    sum_voiced += (double) voiced / (double) p->L;
    if (nlo && nhi && lo > 0 && hi > 0) {
        sum_tilt += 10.0 * log10((hi / nhi) / (lo / nlo));
        n_tilt++;
    }
    n_frames++;
}

static mbe_parms dcur, dprev, denh;

/* How much louder the decoder's synthesis is than sum(Ml^2/2) predicts. The
 * encoder needs this constant to close its gain loop; measuring it from real
 * frames beats deriving it from the FFT/window/synthesis conventions. */
static double syn_pcm_e, syn_pred_e, syn_db, syn_db2;
static long   syn_n;

static void data_cb(void *u, const uint8_t *f)
{
    (void) u;
    char fr[4][24], ambe_d[49];
    short pcm[160];
    mbe_process_result res;
    memset(&res, 0, sizeof(res));
    mbe_decodeDStarDVData(f, (char(*)[24])fr);
    mbe_processAmbe3600x2400Frame(pcm, &res, (const char(*)[24])fr, ambe_d, &dcur, &dprev, &denh);

    if (dcur.L > 3 && dcur.w0 > 0.0f) {
        /* Ml already carries the decoder's unvc attenuation. */
        double pred = 0.0;
        for (int l = 1; l <= dcur.L; l++) {
            pred += 0.5 * (double) dcur.Ml[l] * dcur.Ml[l];
        }
        double e = 0.0;
        for (int i = 0; i < 160; i++) e += (double) pcm[i] * pcm[i];
        e /= 160.0;
        if (pred > 1e-12 && e > 1.0) {
            syn_pcm_e += e; syn_pred_e += pred; syn_n++;
            double db = 10.0 * log10(e / pred);
            syn_db += db; syn_db2 += db * db;
        }
    }
    account(&dcur);
}
static void nop_hdr(void *u, const uint8_t *h) { (void)u; (void)h; }
static void nop_v(void *u) { (void)u; }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s -e in8k.wav | -d disc24k.s16\n", argv[0]); return 1; }

    if (!strcmp(argv[1], "-e")) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) { perror("in"); return 1; }
        unsigned char hdr[44];
        if (fread(hdr, 1, 44, f) != 44) return 1;
        mbe_parms ecur, eprev, edummy;
        mbe_initMbeParms(&ecur, &eprev, &edummy);
        short pcm[160]; float fin[160]; char ambe_d[49];
        while (fread(pcm, 2, 160, f) == 160) {
            for (int i = 0; i < 160; i++) fin[i] = pcm[i] / 32768.0f;
            mbe_encodeAmbe2400Parms(fin, ambe_d, &ecur, &eprev);
            account(&ecur);     /* cur_mp holds the quantised parameters */
        }
        fclose(f);
    } else {
        FILE *f = fopen(argv[2], "rb");
        if (!f) { perror("in"); return 1; }
        fseek(f, 0, SEEK_END); long n = ftell(f) / 2; fseek(f, 0, SEEK_SET);
        short *s = malloc((size_t) n * 2);
        if (fread(s, 2, (size_t) n, f) != (size_t) n) return 1;
        fclose(f);
        mbe_initMbeParms(&dcur, &dprev, &denh);
        sbitx_dstar_rx *rx = sbitx_dstar_rx_new();
        sbitx_dstar_rx_set_cbs(rx, nop_hdr, data_cb, nop_v, nop_v, NULL);
        sbitx_dstar_rx_set_polarity(rx, -1.0f);
        float buf[256];
        for (long fed = 0; fed < n; ) {
            int m = (int) ((n - fed) < 256 ? (n - fed) : 256);
            for (int i = 0; i < m; i++) buf[i] = (float) s[fed + i] / 32768.0f;
            sbitx_dstar_rx_process(rx, buf, m);
            fed += m;
        }
        sbitx_dstar_rx_free(rx);
    }

    if (!n_frames) { printf("no usable frames\n"); return 1; }
    printf("frames %4ld  pitch %5.1f Hz  L %4.1f  voiced %4.1f%%  tilt %+6.2f dB  gamma %+6.2f\n",
           n_frames, sum_w0 / n_frames, sum_L / n_frames,
           100.0 * sum_voiced / n_frames, n_tilt ? sum_tilt / n_tilt : 0.0,
           sum_gamma / n_frames);
    { double m = sum_gamma/n_frames;
      if (syn_n) {
        double m = syn_db / syn_n;
        printf("            decoder synthesis gain: %+.1f dB over sum(Ml^2/2), per-frame spread %.1f dB (%ld frames)\n",
               m, sqrt(syn_db2 / syn_n - m * m), syn_n);
    }
    printf("            gamma spread (std) %.2f  <- how much the level tracks the speech\n",
             sqrt(sum_gamma2/n_frames - m*m)); }
    return 0;
}
