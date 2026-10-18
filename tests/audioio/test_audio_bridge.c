/* audio_bridge_test - rational resampling across block boundaries.
 *
 * csdr's rational_resampler_ff consumes only part of each block (the filter
 * look-ahead stays behind); audio_bridge must carry it into the next call.
 * Feeds a 1 kHz tone in codec-sized blocks through 48k -> 8k (the capture
 * side) and 8k -> 48k (the playback side) and checks that no samples are
 * lost and the tone comes out clean. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio_bridge.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                         printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Stubs for the rings the bridge feeds. */
static int16_t sink[200000];
static size_t nsink;
static int16_t src[200000];
static size_t nsrc, src_pos;

void sbitx_bridge_push_rx(radio *r, const int16_t *s, size_t n)
{
    (void) r;
    for (size_t i = 0; i < n && nsink < sizeof(sink) / sizeof(sink[0]); i++)
        sink[nsink++] = s[i];
}
size_t sbitx_bridge_pop_tx(radio *r, int16_t *out, size_t n)
{
    (void) r;
    size_t k = 0;
    while (k < n && src_pos < nsrc)
        out[k++] = src[src_pos++];
    return k;
}
void radio_media_tap_rx_audio(radio *r, const int16_t *s, size_t n) { (void) r; (void) s; (void) n; }
void radio_media_tap_tx_audio(radio *r, const int16_t *s, size_t n) { (void) r; (void) s; (void) n; }

/* SNR of a 1 kHz tone of known frequency (least-squares fit). */
static double tone_snr(const int16_t *x, size_t n, double rate)
{
    double cc = 0, ss = 0, cs = 0, xc = 0, xs = 0, xx = 0;
    for (size_t i = 0; i < n; i++)
    {
        double c = cos(2 * M_PI * 1000.0 * i / rate), s = sin(2 * M_PI * 1000.0 * i / rate);
        cc += c * c; ss += s * s; cs += c * s; xc += x[i] * c; xs += x[i] * s; xx += (double) x[i] * x[i];
    }
    double det = cc * ss - cs * cs, a = (xc * ss - xs * cs) / det, b = (xs * cc - xc * cs) / det;
    double sig = a * xc + b * xs;
    return 10 * log10(sig / (xx - sig));
}

int main(void)
{
    static radio r;
    audio_bridge b;

    /* Capture side: codec 48 kHz -> 8 kHz, 480-frame blocks, 2 s. */
    CHECK(audio_bridge_init(&b, 48000, 8000), "init 48k->8k");
    int16_t blk[480];
    for (int n = 0; n < 2 * 48000; n += 480)
    {
        for (int i = 0; i < 480; i++)
            blk[i] = (int16_t) lrint(16000.0 * sin(2 * M_PI * 1000.0 * (n + i) / 48000.0));
        audio_bridge_push_rx_native(&b, &r, blk, 480);
    }
    audio_bridge_shutdown(&b);
    double snr = tone_snr(sink + 800, nsink - 1600, 8000.0);
    printf("48k -> 8k: %zu samples out of 16000, tone SNR %.1f dB\n", nsink, snr);
    CHECK(nsink > 16000 - 200 && nsink <= 16000, "lost samples: %zu of 16000", nsink);
    CHECK(snr > 40.0, "tone SNR %.1f dB", snr);

    /* Playback side: 8 kHz ring -> codec 48 kHz, pulled in 480-frame blocks. */
    for (nsrc = 0; nsrc < 2 * 8000; nsrc++)
        src[nsrc] = (int16_t) lrint(16000.0 * sin(2 * M_PI * 1000.0 * nsrc / 8000.0));
    CHECK(audio_bridge_init(&b, 48000, 8000), "init 8k->48k");
    static int16_t out[2 * 48000];
    size_t nout = 0;
    for (int i = 0; i < 2 * 48000 / 480; i++)
        nout += audio_bridge_pop_tx_native(&b, &r, out + nout, 480);
    audio_bridge_shutdown(&b);
    snr = tone_snr(out + 4800, nout - 9600, 48000.0);
    printf("8k -> 48k: %zu samples out of 96000, tone SNR %.1f dB\n", nout, snr);
    CHECK(nout > 96000 - 1200 && nout <= 96000, "lost samples: %zu of 96000", nout);
    CHECK(snr > 40.0, "tone SNR %.1f dB", snr);

    printf("audio_bridge_test: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
