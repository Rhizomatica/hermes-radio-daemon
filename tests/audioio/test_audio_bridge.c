/* audio_bridge_test - rational resampling across block boundaries, and the
 * listening audio's noise reduction.
 *
 * csdr's rational_resampler_ff consumes only part of each block (the filter
 * look-ahead stays behind); audio_bridge must carry it into the next call.
 * Feeds a 1 kHz tone in codec-sized blocks through 48k -> 8k (the capture
 * side) and 8k -> 48k (the playback side) and checks that no samples are
 * lost and the tone comes out clean.
 *
 * Noise reduction: on a Hamlib profile with noise_reduction ON, the
 * listening ring gets less noise and keeps the tone, the decoders' ring
 * gets the audio as it was; off, or in a digital mode, both get it as it
 * was. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "audio_bridge.h"

/* Stubs for the rings the bridge feeds: sink is the listening ring,
 * digi the decoders'. */
static int16_t sink[200000], digi[200000];
static size_t nsink;
static int16_t src[200000];
static size_t nsrc, src_pos;

void sbitx_bridge_push_rx_split(radio *r, const int16_t *listen, const int16_t *d, size_t n)
{
    (void) r;
    for (size_t i = 0; i < n && nsink < sizeof(sink) / sizeof(sink[0]); i++) {
        sink[nsink] = listen[i];
        digi[nsink++] = d[i];
    }
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

static radio r;

void setUp(void)
{
    nsink = nsrc = src_pos = 0;
    memset(&r, 0, sizeof r);
}
void tearDown(void) {}

/* Capture side: codec 48 kHz -> 8 kHz, 480-frame blocks, 2 s. */
static void test_capture_48k_to_8k(void)
{
    audio_bridge b;
    TEST_ASSERT_TRUE(audio_bridge_init(&b, 48000, 8000));
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
    TEST_ASSERT_GREATER_THAN_size_t_MESSAGE(16000 - 200, nsink, "samples lost");
    TEST_ASSERT_LESS_OR_EQUAL_size_t(16000, nsink);
    TEST_ASSERT_GREATER_THAN_DOUBLE_MESSAGE(40.0, snr, "tone SNR (dB)");
}

/* Playback side: 8 kHz ring -> codec 48 kHz, pulled in 480-frame blocks. */
static void test_playback_8k_to_48k(void)
{
    audio_bridge b;
    for (nsrc = 0; nsrc < 2 * 8000; nsrc++)
        src[nsrc] = (int16_t) lrint(16000.0 * sin(2 * M_PI * 1000.0 * nsrc / 8000.0));
    TEST_ASSERT_TRUE(audio_bridge_init(&b, 48000, 8000));
    static int16_t out[2 * 48000];
    size_t nout = 0;
    for (int i = 0; i < 2 * 48000 / 480; i++)
        nout += audio_bridge_pop_tx_native(&b, &r, out + nout, 480);
    audio_bridge_shutdown(&b);
    double snr = tone_snr(out + 4800, nout - 9600, 48000.0);
    printf("8k -> 48k: %zu samples out of 96000, tone SNR %.1f dB\n", nout, snr);
    TEST_ASSERT_GREATER_THAN_size_t_MESSAGE(96000 - 1200, nout, "samples lost");
    TEST_ASSERT_LESS_OR_EQUAL_size_t(96000, nout);
    TEST_ASSERT_GREATER_THAN_DOUBLE_MESSAGE(40.0, snr, "tone SNR (dB)");
}

static unsigned rng = 1;
static double gauss(void)
{
    double u1, u2;
    do { rng = rng * 1103515245u + 12345u; u1 = (rng >> 8) / 16777216.0; } while (u1 <= 1e-9);
    rng = rng * 1103515245u + 12345u; u2 = (rng >> 8) / 16777216.0;
    return sqrt(-2.0 * log(u1)) * cos(2 * M_PI * u2);
}

static double rms(const int16_t *x, size_t n)
{
    double s = 0;
    for (size_t i = 0; i < n; i++)
        s += (double) x[i] * x[i];
    return sqrt(s / n);
}

/* 4 s at 48 kHz, in 480-frame blocks: noise alone for 2 s, then a 1 kHz
 * tone in it, 150 ms on and 150 ms off, as speech or CW come and go (a
 * steady tone is stationary, which is what the denoiser takes away as
 * noise). What was fed is kept in fed. */
#define KEY 7200
static int16_t fed[4 * 48000];
static uint32_t latency;            /* the denoiser's, while it ran */

static void feed_noise_then_tone(void)
{
    audio_bridge b;
    TEST_ASSERT_TRUE(audio_bridge_init(&b, 48000, 48000));
    int16_t blk[480];
    for (int n = 0; n < 4 * 48000; n += 480) {
        for (int i = 0; i < 480; i++) {
            bool on = n + i >= 2 * 48000 && (n + i) / KEY % 2 == 0;
            double t = on ? 8000.0 * sin(2 * M_PI * 1000.0 * (n + i) / 48000.0) : 0.0;
            blk[i] = fed[n + i] = (int16_t) lrint(t + 1500.0 * gauss());
        }
        audio_bridge_push_rx_native(&b, &r, blk, 480);
    }
    latency = rx_denoise_latency(&b.nr);
    audio_bridge_shutdown(&b);
    TEST_ASSERT_EQUAL_size_t(4 * 48000, nsink);
}

static void hamlib_profile(uint16_t mode, uint16_t nr)
{
    r.backend_kind = RADIO_BACKEND_HAMLIB;
    r.profile_active_idx = 0;
    r.profiles[0].mode = mode;
    r.profiles[0].noise_reduction = nr;
    r.rx_nr_reduction_db = RX_DENOISE_REDUCTION_DB;
    r.rx_nr_strength = RX_DENOISE_STRENGTH;
}

/* The power in the middles of the tone's on (or off) stretches from 3 s,
 * the middle 50 ms of each, delay samples later. */
static double keyed(const int16_t *x, bool on, size_t delay)
{
    double s = 0;
    size_t n = 0;
    for (size_t k = 3 * 48000 / KEY + 1; (k + 1) * KEY + delay <= 4 * 48000; k++)
        if ((k % 2 == 0) == on)
            for (size_t i = k * KEY + 2400; i < k * KEY + 4800; i++, n++)
                s += (double) x[i + delay] * x[i + delay];
    return sqrt(s / n);
}

static void test_noise_reduction_listening_only(void)
{
    hamlib_profile(MODE_USB, NOISE_REDUCTION_ON);
    feed_noise_then_tone();
    /* the noise alone, once learned (1-2 s); the tone and the gaps, 3-4 s */
    double noise_listen = rms(sink + 48000, 48000), noise_digi = rms(digi + 48000, 48000);
    double on_listen = keyed(sink, true, latency), on_digi = keyed(digi, true, 0);
    double gap_listen = keyed(sink, false, latency), gap_digi = keyed(digi, false, 0);
    printf("NR on, %u samples late: noise %.1f dB, tone %.1f dB, gaps %.1f dB\n", latency,
           20 * log10(noise_listen / noise_digi), 20 * log10(on_listen / on_digi),
           20 * log10(gap_listen / gap_digi));
    TEST_ASSERT_GREATER_THAN_UINT32(0, latency);
    TEST_ASSERT_LESS_THAN_DOUBLE_MESSAGE(noise_digi / 1.41, noise_listen, "noise not 3 dB down");
    TEST_ASSERT_LESS_THAN_DOUBLE_MESSAGE(gap_digi / 1.41, gap_listen, "gaps not 3 dB down");
    TEST_ASSERT_GREATER_THAN_DOUBLE_MESSAGE(on_digi / 1.41, on_listen, "the tone lost 3 dB");
    /* the decoders' audio is the rig's */
    TEST_ASSERT_EQUAL_INT16_ARRAY_MESSAGE(fed, digi, nsink, "the decoders' audio changed");
}

static void test_no_noise_reduction_off_or_digital(void)
{
    hamlib_profile(MODE_USB, NOISE_REDUCTION_OFF);
    feed_noise_then_tone();
    TEST_ASSERT_EQUAL_INT16_ARRAY_MESSAGE(digi, sink, nsink, "NR off");

    setUp();
    hamlib_profile(MODE_FT8, NOISE_REDUCTION_ON);
    feed_noise_then_tone();
    TEST_ASSERT_EQUAL_INT16_ARRAY_MESSAGE(digi, sink, nsink, "FT8 is not denoised");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_noise_reduction_listening_only);
    RUN_TEST(test_no_noise_reduction_off_or_digital);
    RUN_TEST(test_capture_48k_to_8k);
    RUN_TEST(test_playback_8k_to_48k);
    return UNITY_END();
}
