/* dstar_crypt_sim — D-STAR voice encryption (dsp/dstar_voice.c) through the
 * daemon's GMSK modem and a simulated FM channel, no radio.
 *
 * speech -> AMBE (mbelib-neo) -> dstar_voice TX (header with the nonce,
 * keystream, sync blocks) -> sbitx_dstar TX (GMSK, 24 kHz) -> FM at the
 * configured deviation -> channel -> +-3.2 kHz IF filter -> discriminator
 * (Hz, scaled as dstar_rx_gain = 1 does) -> sbitx_dstar RX -> dstar_voice RX
 * -> AMBE -> speech. The receiver may join late (no header, so it locks on
 * the sync blocks), as when a station tunes in mid-over.
 *
 * Prints how many frames the modem delivered, how many were played, how many
 * of those carry exactly the transmitted voice bits, the first one that did
 * (the lock time), and the receiver's final crypto status.
 *
 * usage: dstar_crypt_sim MODE in.wav out8k.wav [snr3k_dB] [foff_Hz] [CHAN] [seed] [join_s]
 *   MODE: plain | key | nokey (TX keyed, RX none) | wrongkey
 *   in.wav: 8 or 16 kHz mono 16-bit
 *   CHAN: awgn | good | mpg | mpp | mpd (two equal Rayleigh paths: 0.5 ms
 *         0.1 Hz, 1 ms 0.1 Hz, 2 ms 1 Hz, 4 ms 2 Hz; Gaussian Doppler),
 *         flat (one path, 1 Hz)
 *   join_s: seconds into the over at which the receiver starts listening
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <complex.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbelib-neo/mbelib.h>

#include "dstar_voice.h"
#include "sbitx_dstar.h"
#include "voice_crypto.h"

#define FS        24000.0     /* the modem's rate */
#define DEV_HZ    1200.0      /* main:dstar_deviation's default */
#define MOD_PEAK  0.0257      /* sbitx_dstar_tx_generate's full scale */
#define RX_SCALE  (1.0 / 20000.0)   /* dstar_rx_gain = 1 */
#define MAXF      (50 * 600)  /* 10 minutes of 20 ms frames */

static const uint8_t KEY_A[VOICE_KEY_BYTES] = "hermes dstar_crypt_sim key A...";
static const uint8_t KEY_B[VOICE_KEY_BYTES] = "hermes dstar_crypt_sim key B...";

/* ---- channel, as in rade_crypt_sim ---- */
static double gauss(void)
{
    double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = rand() / (RAND_MAX + 1.0);
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

#define NSIN 64
typedef struct { double f[NSIN], ph[NSIN]; } fader;
static void fader_init(fader *f, double spread_hz)
{
    for (int k = 0; k < NSIN; k++) {
        f->f[k] = gauss() * spread_hz / 2;
        f->ph[k] = 2 * M_PI * rand() / (RAND_MAX + 1.0);
    }
}
static double complex fader_at(const fader *f, double t)
{
    double sr = 0, si = 0;
    for (int k = 0; k < NSIN; k++) {
        double a = 2 * M_PI * f->f[k] * t + f->ph[k];
        sr += cos(a);
        si += sin(a);
    }
    return (sr + I * si) / sqrt(NSIN);
}

/* windowed-sinc low-pass, cutoff fc (Hz) at rate fs, n taps (odd) */
static void lowpass(double *h, int n, double fc, double fs)
{
    double sum = 0;
    for (int i = 0; i < n; i++) {
        double m = i - (n - 1) / 2.0, x = 2 * fc / fs;
        double w = 0.42 - 0.5 * cos(2 * M_PI * i / (n - 1)) + 0.08 * cos(4 * M_PI * i / (n - 1));
        h[i] = (m == 0 ? x : sin(M_PI * x * m) / (M_PI * m)) * w;
        sum += h[i];
    }
    for (int i = 0; i < n; i++)
        h[i] /= sum;
}

/* ---- WAV ---- */
static FILE *wav_open_in(const char *path, unsigned *rate)
{
    FILE *f = fopen(path, "rb");
    unsigned char h[12], ck[8], fmt[16];
    *rate = 0;
    if (!f || fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return f ? (fclose(f), NULL) : NULL;
    while (fread(ck, 1, 8, f) == 8) {
        unsigned len = ck[4] | ck[5] << 8 | ck[6] << 16 | (unsigned) ck[7] << 24;
        if (!memcmp(ck, "data", 4))
            return *rate ? f : (fclose(f), NULL);
        if (!memcmp(ck, "fmt ", 4) && len >= 16 && fread(fmt, 1, 16, f) == 16) {
            if (fmt[2] == 1 && fmt[14] == 16)   /* mono, 16-bit */
                *rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (unsigned) fmt[7] << 24;
            len -= 16;
        }
        fseek(f, (long) (len + (len & 1)), SEEK_CUR);
    }
    fclose(f);
    return NULL;
}

static void wav_header(FILE *f, unsigned rate, unsigned samples)
{
    unsigned bytes = samples * 2;
    unsigned char h[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\0\0\0\0\0\0\0\0\x02\0\x10\0data";
    unsigned v[] = {36 + bytes, rate, rate * 2, bytes};
    int at[] = {4, 24, 28, 40};
    for (int i = 0; i < 4; i++)
        for (int b = 0; b < 4; b++)
            h[at[i] + b] = (unsigned char) (v[i] >> (8 * b));
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

/* ---- the over ---- */
static char plain[MAXF][DSTAR_VOICE_AMBE_BITS];
static int nplain;

static float *sig;      /* modulator output, 24 kHz */
static size_t sig_n, sig_cap;
static void push(const float *s, int n)
{
    if (sig_n + (size_t) n > sig_cap) {
        sig_cap = (sig_n + (size_t) n) * 2 + 65536;
        sig = realloc(sig, sig_cap * sizeof(*sig));
    }
    memcpy(sig + sig_n, s, (size_t) n * sizeof(*s));
    sig_n += (size_t) n;
}

/* ---- receiver ---- */
static sbitx_dstar_rx *demod;
static dstar_voice_rx vrx;
static mbe_parms rcur, rprev, renh;
static FILE *fo;
static unsigned out_n;
static int delivered, played, exact, first_exact = -1, headers;
static dstar_vc_status last_status;   /* before the EOT resets it */
static long offset = LONG_MIN;
/* the daemon's gate on top of dstar_voice (sbitx_dsp.c, dstar_rx_confirmed):
 * play only after a CRC-good header or the second data sync */
static bool confirmed;
static int syncs, garbage;
static long biterrs;      /* TX frame index - delivery index */

static void on_header(void *u, const uint8_t *h)
{
    (void) u;
    headers++;
    dstar_voice_rx_header(&vrx, h);
    uint16_t crc = sbitx_dstar_crc16(h, SBITX_DSTAR_HEADER_BYTES - 2U);
    if (h[SBITX_DSTAR_HEADER_BYTES - 2U] == (uint8_t) (crc & 0xFFU) &&
        h[SBITX_DSTAR_HEADER_BYTES - 1U] == (uint8_t) (crc >> 8))
        confirmed = true;
}
static void on_end(void *u)
{
    (void) u;
    dstar_voice_rx_reset(&vrx);
    confirmed = false;
    syncs = 0;
}

static int voice_diff(const char *a, const char *b)
{
    int d = 0;
    for (int i = 0; i < DSTAR_VOICE_AMBE_BITS; i++)
        d += i != DSTAR_VOICE_SPARE_BIT && a[i] != b[i];
    return d;
}
#define same_voice(a, b) (voice_diff(a, b) == 0)

static void on_data(void *u, const uint8_t *frame)
{
    (void) u;
    char ambe_d[DSTAR_VOICE_AMBE_BITS];
    mbe_process_result res;
    short pcm[160] = {0};
    uint16_t fc = sbitx_dstar_rx_frame_index(demod);
    delivered++;
    bool play = dstar_voice_rx_frame(&vrx, frame, fc, ambe_d, &res);
    last_status = vrx.status;
    if (fc == 0 && syncs < 2 && ++syncs == 2)
        confirmed = true;
    if (!confirmed)
        play = false;
    if (play) {
        played++;
        /* which TX frame this is: found once by search, then the modem
         * delivers one per frame in order (a miss would show as a mismatch) */
        if (offset == LONG_MIN)
            for (long g = 0; g < nplain && offset == LONG_MIN; g++)
                if (same_voice(ambe_d, plain[g]) && g + 1 < nplain)
                    offset = g - (delivered - 1);
        long g = offset == LONG_MIN ? -1 : delivered - 1 + offset;
        int dbits = g >= 0 && g < nplain ? voice_diff(ambe_d, plain[g]) : 48;
        biterrs += dbits;
        /* ciphertext, or a wrong frame, differs in about half the bits;
         * residual channel errors in far fewer */
        if (dbits >= 12)
            garbage++;
        if (dbits == 0) {
            exact++;
            if (first_exact < 0)
                first_exact = (int) g;
        } else if (getenv("DSTAR_SIM_DEBUG")) {
            fprintf(stderr, "delivery %d (tx frame %ld, slot %u): %d voice bits differ\n",
                    delivered - 1, g, fc, dbits);
        }
        mbe_processAmbe2400Data(pcm, &res, ambe_d, &rcur, &rprev, &renh);
    }
    fwrite(pcm, sizeof(short), 160, fo);
    out_n += 160;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s plain|key|nokey|wrongkey in.wav out8k.wav "
                        "[snr3k_dB] [foff_Hz] [awgn|good|mpg|mpp|mpd|flat] [seed] [join_s]\n", argv[0]);
        return 1;
    }
    const char *mode = argv[1];
    double snr = argc > 4 ? atof(argv[4]) : 100, foff = argc > 5 ? atof(argv[5]) : 0;
    const char *chan = argc > 6 ? argv[6] : "awgn";
    srand(argc > 7 ? (unsigned) atoi(argv[7]) : 1);
    double join_s = argc > 8 ? atof(argv[8]) : 0;
    int keyed_tx = strcmp(mode, "plain") != 0;
    const uint8_t *rx_key = !strcmp(mode, "key") ? KEY_A : !strcmp(mode, "wrongkey") ? KEY_B : NULL;
    if (strcmp(mode, "plain") && strcmp(mode, "key") && strcmp(mode, "nokey") && strcmp(mode, "wrongkey")) {
        fprintf(stderr, "unknown mode %s\n", mode);
        return 1;
    }

    unsigned rate;
    FILE *fi = wav_open_in(argv[2], &rate);
    fo = fopen(argv[3], "wb");
    if (!fi || !fo || (rate != 8000 && rate != 16000)) {
        fprintf(stderr, "cannot open %s (8 or 16 kHz mono 16-bit WAV) or %s\n", argv[2], argv[3]);
        return 1;
    }
    wav_header(fo, 8000, 0);

    /* speech at 8 kHz */
    size_t pn = 0, pcap = 0;
    float *pcm8 = NULL;
    double h16[63], hist[63] = {0};
    lowpass(h16, 63, 3600, 16000);
    int16_t s;
    long k = 0;
    while (fread(&s, 2, 1, fi) == 1) {
        double x = s / 32768.0;
        if (rate == 16000) {
            memmove(hist + 1, hist, sizeof(hist) - sizeof(hist[0]));
            hist[0] = x;
            if (k++ & 1)
                continue;
            x = 0;
            for (int i = 0; i < 63; i++)
                x += h16[i] * hist[i];
        }
        if (pn == pcap)
            pcm8 = realloc(pcm8, (pcap = pcap * 2 + 8000) * sizeof(float));
        pcm8[pn++] = (float) x;
    }
    fclose(fi);

    /* TX: AMBE, then the voice glue keyed with KEY_A, into the modulator */
    voice_crypto_set_key(KEY_A);
    mbe_ambe2400_encoder *enc = mbe_ambe2400EncoderAlloc();
    mbe_parms cur, prev, enh;
    mbe_initMbeParms(&cur, &prev, &enh);
    dstar_voice_tx vtx;
    dstar_voice_tx_begin(&vtx, "N0CALL  ", "CQCQCQ  ", keyed_tx);
    sbitx_dstar_tx *mod = sbitx_dstar_tx_new();
    float quiet[2400] = {0}, buf[4096];
    push(quiet, 2400);                       /* 100 ms before keying */
    size_t over_start = sig_n;
    sbitx_dstar_tx_header(mod, vtx.header);
    for (size_t at = 0; at + 160 <= pn && nplain < MAXF; at += 160) {
        uint8_t frame[SBITX_DSTAR_FRAME_BYTES];
        mbe_encodeAmbe2400Parms(enc, pcm8 + at, plain[nplain], &cur, &prev);
        mbe_moveMbeParms(&cur, &prev);
        dstar_voice_tx_frame(&vtx, plain[nplain++], frame);
        sbitx_dstar_tx_frame(mod, frame);
        int n;
        while ((n = sbitx_dstar_tx_generate(mod, buf, 4096)) > 0)
            push(buf, n);
    }
    sbitx_dstar_tx_eot(mod);
    int n;
    while ((n = sbitx_dstar_tx_generate(mod, buf, 4096)) > 0)
        push(buf, n);
    size_t over_end = sig_n;
    for (int i = 0; i < 10; i++)
        push(quiet, 2400);                   /* and 1 s after */
    sbitx_dstar_tx_free(mod);
    mbe_ambe2400EncoderFree(enc);

    /* channel */
    int two = 0;
    double spread = 0, delay_s = 0;
    if (!strcmp(chan, "good"))      { two = 1; delay_s = 0.5e-3; spread = 0.1; }
    else if (!strcmp(chan, "mpg"))  { two = 1; delay_s = 1e-3; spread = 0.1; }
    else if (!strcmp(chan, "mpp"))  { two = 1; delay_s = 2e-3; spread = 1.0; }
    else if (!strcmp(chan, "mpd"))  { two = 1; delay_s = 4e-3; spread = 2.0; }
    else if (!strcmp(chan, "flat")) { spread = 1.0; }
    else if (strcmp(chan, "awgn"))  { fprintf(stderr, "unknown channel %s\n", chan); return 1; }
    fader fa, fb;
    fader_init(&fa, spread);
    fader_init(&fb, spread);
    int d = (int) lround(delay_s * FS);

    /* noise per 3 kHz: unit carrier, complex noise over the 24 kHz band */
    double sigma = sqrt(8.0 / pow(10, snr / 10) / 2);
    double hif[65];
    lowpass(hif, 65, 3200, FS);
    double complex ifh[65] = {0}, *tx = malloc(sig_n * sizeof(*tx));
    double phi = 0;
    for (size_t i = 0; i < sig_n; i++) {
        int on = i >= over_start && i < over_end;
        phi += 2 * M_PI * (on ? sig[i] / MOD_PEAK * DEV_HZ : 0) / FS;
        tx[i] = on ? cexp(I * phi) : 0;
    }

    voice_crypto_clear_key();
    if (rx_key)
        voice_crypto_set_key(rx_key);
    demod = sbitx_dstar_rx_new();
    sbitx_dstar_rx_set_cbs(demod, on_header, on_data, on_end, on_end, NULL);
    /* here frequency follows the modulator's sign; on air the FM chain
     * inverts it, which the daemon's default polarity expects */
    sbitx_dstar_rx_set_polarity(demod, -1.0f);
    dstar_voice_rx_reset(&vrx);
    mbe_initMbeParms(&rcur, &rprev, &renh);

    size_t join = over_start + (size_t) (join_s * FS);
    double complex last = 1;
    float disc[480];
    int dn = 0;
    for (size_t i = 0; i < sig_n; i++) {
        double t = i / FS;
        double complex y;
        if (spread == 0)
            y = tx[i];
        else if (!two)
            y = fader_at(&fa, t) * tx[i];
        else
            y = (fader_at(&fa, t) * tx[i] + fader_at(&fb, t) * (i >= (size_t) d ? tx[i - d] : 0)) / sqrt(2);
        y *= cexp(I * 2 * M_PI * foff * t);
        y += sigma * (gauss() + I * gauss());
        memmove(ifh + 1, ifh, sizeof(ifh) - sizeof(ifh[0]));
        ifh[0] = y;
        double complex z = 0;
        for (int j = 0; j < 65; j++)
            z += hif[j] * ifh[j];
        double dphi = carg(z * conj(last));
        last = z;
        if (i < join)
            continue;
        disc[dn++] = (float) (dphi * FS / (2 * M_PI) * RX_SCALE);
        if (dn == 480) {
            sbitx_dstar_rx_process(demod, disc, dn);
            dn = 0;
        }
    }
    if (dn) {
        sbitx_dstar_rx_process(demod, disc, dn);
    }

    static const char *st[] = {"clear", "decrypting", "acquiring", "no key", "bad key"};
    int ds = (int) last_status;
    int want = nplain - (int) (join_s * 50);
    printf("%s %s snr %.1f foff %.0f join %.1fs: headers %d, delivered %d of %d, played %d, "
           "exact %d, garbage %d, voice BER %.4f, first exact frame %d, status %s\n",
           mode, chan, snr, foff, join_s, headers, delivered, want, played, exact, garbage,
           played ? biterrs / (47.0 * played) : 0.0, first_exact, ds >= 0 && ds < 5 ? st[ds] : "?");
    wav_header(fo, 8000, out_n);
    fclose(fo);
    sbitx_dstar_rx_free(demod);
    free(tx);
    free(sig);
    free(pcm8);
    return 0;
}
