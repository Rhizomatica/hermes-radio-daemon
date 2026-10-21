/* rade_crypt_sim — RADE V2 voice scrambling (dsp/rade_voice.c) over a
 * simulated HF channel, no radio.
 *
 * speech -> LPCNet features -> RADE V2 TX (the daemon's rade_api session,
 * SSB BPF on) with the keyed latent mixing -> channel -> RADE V2 RX with the
 * unmixing -> FARGAN -> speech. The receiver learns the nonce and frame
 * counter from the aux carriers' signalling (rade_voice.h), and plays only
 * what rade_voice_rx_play() allows. Prints the frames decoded and played,
 * when playing started, the messages decoded, the signal's PAPR, and a
 * cepstral distance between the input and played features (lower is
 * better; ~1-3 is good speech, >30 is unintelligible).
 *
 * usage: rade_crypt_sim MODE in16k.wav out16k.wav [snr3k_dB] [foff_Hz] [CHAN] [seed] [join_s]
 *   MODE: plain (stock RADE V2) | key | nokey (TX keyed, RX none) | wrongkey |
 *         clear (TX clear, RX keyed: must play)
 *   join_s: seconds into the over at which the receiver starts listening
 *
 * Encrypted overs go through rade_voice's clipper, as the daemon sends them.
 *
 * Environment: AUX_AMP, the aux carriers' level against the data carriers
 * (1); CLIP=0, no clipper; SNR_PEAK=1, take snr3k against the over's peak
 * power instead of its mean (a transmitter that is peak-limited, as a PA
 * is).
 *   CHAN: awgn | mpg | mpp | mpd (two equal Rayleigh paths: 1 ms 0.1 Hz,
 *         2 ms 1 Hz, 4 ms 2 Hz; Gaussian Doppler), static (2 ms, frozen),
 *         flat (one path, 1 Hz)
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rade_api.h"
#include "radae_vocoder.h"
#include "rade_voice.h"

_Static_assert(RADE_VOICE_NC == RADE_V2_NC && RADE_VOICE_NS == RADE_V2_NS &&
               RADE_VOICE_LATENT == RADE_V2_LATENT_DIM, "rade_voice.h: RADE V2 layout");

_Static_assert(RADE_VOICE_NAUX == RADE_V2_NAUX && sizeof(RADE_COMP) == 2 * sizeof(float),
               "rade_voice.h: RADE V2 aux carriers");

static const uint8_t KEY_A[VOICE_KEY_BYTES] = "hermes rade_crypt_sim key A....";
static const uint8_t KEY_B[VOICE_KEY_BYTES] = "hermes rade_crypt_sim key B....";

static rade_voice_tx vtx;
static rade_voice_rx vrx;
static const uint8_t *rx_key;
static double lat_pow;   /* the data carriers' mean symbol power, before mixing */
static long lat_n;

/* TX and RX share this process's one voice key: each hook sets its own. */
static void tx_hook(void *user, rade_v2_hook_frame *f)
{
    (void) user;
    for (int i = 0; i < RADE_V2_LATENT_DIM; i += 2)
        lat_pow += f->z[i] * f->z[i] + f->z[i + 1] * f->z[i + 1];
    lat_n += RADE_V2_LATENT_DIM / 2;
    voice_crypto_set_key(KEY_A);
    rade_voice_tx_frame(&vtx, f->z, (float *) f->aux);
}

static void rx_hook(void *user, rade_v2_hook_frame *f)
{
    (void) user;
    if (rx_key)
        voice_crypto_set_key(rx_key);
    else
        voice_crypto_clear_key();
    f->align = rade_voice_rx_symbol(&vrx, f->z, (const float *) f->aux, f->sym);
}

/* ---- channel ---- */
static double gauss(void)
{
    double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = rand() / (RAND_MAX + 1.0);
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

/* Rayleigh path, Gaussian Doppler spectrum (Watterson): a sum of sinusoids
 * with frequencies from N(0, (spread/2)^2), unit mean power. */
#define NSIN 64
typedef struct { double f[NSIN], ph[NSIN]; } fader;
static void fader_init(fader *f, double spread_hz)
{
    for (int k = 0; k < NSIN; k++) {
        f->f[k] = gauss() * spread_hz / 2;
        f->ph[k] = 2 * M_PI * rand() / (RAND_MAX + 1.0);
    }
}
static void fader_at(const fader *f, double t, double *re, double *im)
{
    double sr = 0, si = 0;
    for (int k = 0; k < NSIN; k++) {
        double a = 2 * M_PI * f->f[k] * t + f->ph[k];
        sr += cos(a);
        si += sin(a);
    }
    *re = sr / sqrt(NSIN);
    *im = si / sqrt(NSIN);
}

static RADE_COMP *sig;
static size_t sig_n, sig_cap;
static void push(const RADE_COMP *s, int n)
{
    if (sig_n + (size_t) n > sig_cap) {
        sig_cap = (sig_n + (size_t) n) * 2 + 4096;
        sig = realloc(sig, sig_cap * sizeof(*sig));
    }
    memcpy(sig + sig_n, s, (size_t) n * sizeof(*s));
    sig_n += (size_t) n;
}

/* the first 18 features (cepstrum) of every 10 ms frame, in and out */
typedef struct { float *v; size_t n, cap; } feats;
static void keep(feats *b, const float *f)
{
    if (b->n == b->cap) {
        b->cap = b->cap * 2 + 1024;
        b->v = realloc(b->v, b->cap * 18 * sizeof(float));
    }
    memcpy(b->v + b->n * 18, f, 18 * sizeof(float));
    b->n++;
}

/* minimal 16-bit mono WAV: skip to the data chunk */
static FILE *wav_open_in(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char h[12], ck[8];
    if (!f || fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return f ? (fclose(f), NULL) : NULL;
    while (fread(ck, 1, 8, f) == 8) {
        unsigned len = ck[4] | ck[5] << 8 | ck[6] << 16 | (unsigned) ck[7] << 24;
        if (!memcmp(ck, "data", 4))
            return f;
        fseek(f, (long) (len + (len & 1)), SEEK_CUR);
    }
    fclose(f);
    return NULL;
}

static void wav_header(FILE *f, unsigned samples)
{
    unsigned rate = 16000, bytes = samples * 2;
    unsigned char h[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\0\0\0\0\0\0\0\0\x02\0\x10\0data";
    unsigned v[] = {36 + bytes, rate, rate * 2, bytes};
    int at[] = {4, 24, 28, 40};
    for (int i = 0; i < 4; i++)
        for (int b = 0; b < 4; b++)
            h[at[i] + b] = (unsigned char) (v[i] >> (8 * b));
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s plain|key|nokey|wrongkey in16k.wav out16k.wav "
                        "[snr3k_dB] [foff_Hz] [awgn|mpg|mpp|mpd|static|flat] [seed] [join_s]\n", argv[0]);
        return 1;
    }
    const char *mode = argv[1];
    double snr = argc > 4 ? atof(argv[4]) : 100, foff = argc > 5 ? atof(argv[5]) : 0;
    const char *chan = argc > 6 ? argv[6] : "awgn";
    srand(argc > 7 ? (unsigned) atoi(argv[7]) : 1);
    double join_s = argc > 8 ? atof(argv[8]) : 0;
    const double aux_amp = getenv("AUX_AMP") ? atof(getenv("AUX_AMP")) : 1.0;
    int plain = !strcmp(mode, "plain"), keyed_tx = !plain && strcmp(mode, "clear");
    rx_key = !strcmp(mode, "key") || !strcmp(mode, "clear") ? KEY_A : !strcmp(mode, "wrongkey") ? KEY_B : NULL;
    if (!plain && strcmp(mode, "key") && strcmp(mode, "nokey") && strcmp(mode, "wrongkey") &&
        strcmp(mode, "clear")) {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 1;
    }

    FILE *fi = wav_open_in(argv[2]), *fo = fopen(argv[3], "wb");
    if (!fi || !fo) {
        fprintf(stderr, "cannot open %s or %s (16 kHz mono 16-bit WAV in)\n", argv[2], argv[3]);
        return 1;
    }
    wav_header(fo, 0);

    rade_initialize();
    struct rade *tx = rade_open("builtin", RADE_MODE_V2 | RADE_VERBOSE_0);
    struct rade *rx = rade_open("builtin", RADE_MODE_V2 | RADE_VERBOSE_0);
    if (!tx || !rx)
        return 1;
    voice_crypto_set_key(KEY_A);
    rade_voice_tx_begin(&vtx, keyed_tx, (float) aux_amp);
    rade_voice_rx_reset(&vrx);
    if (!plain) {
        rade_set_latent_hook(tx, tx_hook, NULL);
        rade_set_latent_hook(rx, rx_hook, NULL);
    }

    /* TX: a little quiet, the over, its end-of-over, quiet to drain */
    int per_tx = rade_n_features_in_out(tx);
    float *feat = calloc((size_t) per_tx, sizeof(float));
    RADE_COMP *txo = calloc((size_t) (rade_n_tx_out(tx) + rade_n_tx_eoo_out(tx)), sizeof(RADE_COMP));
    RADE_COMP zero[800] = {{0}};
    radae_analysis *an = radae_analysis_new();
    feats fin = {0}, fout = {0};
    push(zero, 800);
    size_t on_start = sig_n;
    int16_t pcm[RADAE_VOCODER_FRAME];
    int nf = 0;
    while (fread(pcm, sizeof(int16_t), RADAE_VOCODER_FRAME, fi) == RADAE_VOCODER_FRAME) {
        radae_analysis_frame(an, pcm, &feat[nf]);
        keep(&fin, &feat[nf]);
        nf += RADAE_VOCODER_FEATURES;
        if (nf == per_tx) {
            push(txo, rade_tx(tx, txo, feat));
            nf = 0;
        }
    }
    size_t on_end = sig_n;
    push(txo, rade_tx_eoo(tx, txo));
    for (int i = 0; i < 20; i++)
        push(zero, 800);

    /* channel: paths, frequency offset, AWGN at the over's power */
    double S = 0, fs = RADE_MODEM_SAMPLE_RATE, dly_ms = 0, spread = 0;
    int two_paths = 1, fading = 1;
    /* the transmitter's clipper (rade_voice.h), on encrypted overs */
    const int clip = keyed_tx && !(getenv("CLIP") && !atoi(getenv("CLIP")));
    if (clip) {
        rade_voice_clip cl;
        rade_voice_clip_reset(&cl);
        rade_voice_clip_run(&cl, (float *) sig, (int) sig_n);   /* the delay sits in the quiet after */
    }
    double peak = 0;
    for (size_t i = on_start; i < on_end; i++) {
        double p = sig[i].real * sig[i].real + sig[i].imag * sig[i].imag;
        S += p;
        peak = p > peak ? p : peak;
    }
    S /= (double) (on_end - on_start);
    const double S_mean = S;
    if (getenv("SNR_PEAK") && atoi(getenv("SNR_PEAK")))
        S = peak;
    if (!strcmp(chan, "mpg"))         { dly_ms = 1; spread = 0.1; }
    else if (!strcmp(chan, "mpp"))    { dly_ms = 2; spread = 1; }
    else if (!strcmp(chan, "mpd"))    { dly_ms = 4; spread = 2; }
    else if (!strcmp(chan, "static")) { dly_ms = 2; spread = 0; }
    else if (!strcmp(chan, "flat"))   { two_paths = 0; spread = 1; }
    else                              { fading = 0; }
    int dly = (int) lround(dly_ms * fs / 1000);
    fader f1, f2;
    fader_init(&f1, spread);
    fader_init(&f2, spread);
    double sigma = sqrt(S * fs / (3000 * pow(10, snr / 10)) / 2);
    RADE_COMP *ch = calloc(sig_n, sizeof(*ch));
    for (size_t i = 0; i < sig_n; i++) {
        double re = sig[i].real, im = sig[i].imag;
        if (fading) {
            double t = (double) i / fs, g1r, g1i, g2r = 0, g2i = 0;
            fader_at(&f1, t, &g1r, &g1i);
            double dr = i >= (size_t) dly ? sig[i - dly].real : 0;
            double di = i >= (size_t) dly ? sig[i - dly].imag : 0;
            double k = 1;
            if (two_paths) {
                fader_at(&f2, t, &g2r, &g2i);
                k = 1 / sqrt(2);   /* two unit-power paths, unit total */
            }
            re = k * (g1r * sig[i].real - g1i * sig[i].imag + g2r * dr - g2i * di);
            im = k * (g1r * sig[i].imag + g1i * sig[i].real + g2r * di + g2i * dr);
        }
        double ph = 2 * M_PI * foff * (double) i / fs, c = cos(ph), s = sin(ph);
        ch[i].real = (float) (re * c - im * s + sigma * gauss());
        ch[i].imag = (float) (re * s + im * c + sigma * gauss());
    }

    /* RX */
    radae_synthesis *syn = radae_synthesis_new();
    float *rfeat = calloc((size_t) rade_n_features_in_out(rx), sizeof(float));
    size_t pos = 0;
    unsigned written = 0;
    long calls = 0, synced = 0, played = 0, first_play = -1;
    pos = on_start + (size_t) (join_s * fs);
    unsigned char *was_played = NULL;
    for (;;) {
        int nin = rade_nin(rx), has_eoo = 0;
        if (pos + (size_t) nin > sig_n)
            break;
        int n = rade_rx(rx, rfeat, &has_eoo, NULL, ch + pos);
        int play = plain || rade_voice_rx_play(&vrx);
        pos += (size_t) nin;
        calls++;
        synced += rade_sync(rx) != 0;
        for (int k = 0; k + RADAE_VOCODER_FEATURES <= n; k += RADAE_VOCODER_FEATURES) {
            int16_t out[RADAE_VOCODER_FRAME] = {0};
            keep(&fout, &rfeat[k]);
            was_played = realloc(was_played, fout.n);
            was_played[fout.n - 1] = (unsigned char) play;
            if (play) {
                played++;
                if (first_play < 0)
                    first_play = (long) (pos - on_start);
            }
            int m = play ? radae_synthesis_frame(syn, &rfeat[k], out) : RADAE_VOCODER_FRAME;
            if (m > 0) {
                fwrite(out, sizeof(int16_t), (size_t) m, fo);
                written += (unsigned) m;
            }
        }
    }
    wav_header(fo, written);

    /* cepstral distance at the best alignment over the frames decoded */
    double best = INFINITY;
    const long lag0 = -400 - (long) (join_s * 100);   /* 100 feature frames a second */
    for (long lag = lag0; lag <= 60; lag++) {
        double d = 0;
        long n = 0;
        for (size_t i = 0; i < fin.n; i++) {
            long j = (long) i + lag;
            if (j < 0 || (size_t) j >= fout.n || !was_played[j])
                continue;
            for (int k = 0; k < 18; k++) {
                double e = fin.v[i * 18 + k] - fout.v[j * 18 + k];
                d += e * e;
            }
            n++;
        }
        if (n > 0 && n >= played * 9 / 10 && d / n < best)
            best = d / n;
    }
    static const char *st[] = {"clear", "decrypting", "acquiring", "no key", "bad key"};
    printf("%-8s %-6s snr3k %5.1f dB  foff %5.1f Hz  join %.1f s  in %zu out %zu played %ld "
           "from %.2f s  msgs %u bad %u  status %s  papr %.1f dB  latent rms %.2f  cepdist %.2f\n",
           mode, chan, snr, foff, join_s, fin.n, fout.n, played,
           first_play < 0 ? -1.0 : first_play / fs, vrx.msgs, vrx.msgs_bad,
           plain ? "-" : st[vrx.status], 10 * log10(peak / S_mean),
           lat_n ? sqrt(lat_pow / lat_n) : 0.0, played ? best : INFINITY);
    free(was_played);

    radae_analysis_free(an);
    radae_synthesis_free(syn);
    rade_close(tx);
    rade_close(rx);
    fclose(fi);
    fclose(fo);
    return fout.n > 0 ? 0 : 2;
}
