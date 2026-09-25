/* FT8, CW and RTTY through the sBitx digital-mode chain: encode at the TX
 * rate, add noise, decimate 96 -> 12 kHz with the stream resampler in DSP
 * blocks (as dsp_digi_rx_decode does), decode. Every mode must decode the
 * text, and noise alone must not print text.
 *
 * None of this worked before: the RX decimator dropped 7% of each block,
 * the CW keyer sent no gaps between elements and its detector never
 * triggered on normalized audio, and RTTY looked for frames only at fixed
 * block starts, read the stop/start bits as data and sent 1 as space.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp/stream_resampler.h"
#include "dsp/sbitx_cw.h"
#include "dsp/sbitx_rtty.h"
#include "dsp/sbitx_ft8.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                         printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned rng = 12345;
static float gauss(void)
{
    float u1, u2;
    do { rng = rng * 1103515245u + 12345u; u1 = (rng >> 8) / 16777216.0f; } while (u1 <= 1e-7f);
    rng = rng * 1103515245u + 12345u; u2 = (rng >> 8) / 16777216.0f;
    return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * (float) M_PI * u2);
}

/* 96 kHz signal (with noise of the given RMS) -> 12 kHz, in 1024 blocks. */
static int to12k(const float *x96, int n, float noise_rms, float *y)
{
    stream_resampler r;
    stream_resampler_init(&r, 1, 8, 0.02f);
    float blk[1024];
    int m = 0;
    for (int p = 0; p < n; p += 1024) {
        int b = n - p < 1024 ? n - p : 1024;
        for (int i = 0; i < b; i++)
            blk[i] = x96[p + i] + noise_rms * gauss();
        size_t k = stream_resampler_run(&r, blk, (size_t) b);
        memcpy(y + m, r.out, k * sizeof(float));
        m += (int) k;
    }
    stream_resampler_free(&r);
    return m;
}

static char rx_text[512];
static int  rx_len;
static void rtty_cb(char c)
{
    if (c != '\r' && c != '\n' && rx_len < (int) sizeof(rx_text) - 1)
        rx_text[rx_len++] = c;
    rx_text[rx_len] = '\0';
}

static const char *cw_decode(const float *y, int m)
{
    rx_len = 0; rx_text[0] = '\0';
    sbitx_cw_init(20, 700);
    int blk = sbitx_cw_rx_samples_per_block();
    for (int p = 0; p + blk <= m; p += blk) {
        char d[64];
        int g = sbitx_cw_rx_process(y + p, blk, d, sizeof d, 20, 700);
        for (int i = 0; i < g && rx_len < (int) sizeof(rx_text) - 1; i++)
            rx_text[rx_len++] = d[i];
    }
    rx_text[rx_len] = '\0';
    while (rx_len > 0 && rx_text[rx_len - 1] == ' ')
        rx_text[--rx_len] = '\0';
    return rx_text;
}

static const char *rtty_decode(const float *y, int m)
{
    rx_len = 0; rx_text[0] = '\0';
    sbitx_rtty_init(45, 1585, 170);
    int blk = sbitx_rtty_rx_samples_per_block();
    for (int p = 0; p + blk <= m; p += blk)
        sbitx_rtty_rx_process(y + p, blk, 45, 1585, 170, rtty_cb);
    return rx_text;
}

/* Signal amplitudes: CW 1.0, RTTY 0.4 (the encoders'). Noise RMS is over
 * the full 48 kHz band at 96 kHz; the decoders see ~1/16 of it (3 kHz). */
static void test_cw(float noise_rms)
{
    /* One continuous stream, as on air: 1 s, message, 2 s, message, 1 s. */
    static float x[96000 * 20], y[12000 * 20];
    memset(x, 0, sizeof x);
    int p = 96000;
    p += sbitx_cw_encode("HERMES TEST CW", x + p, 96000 * 8, 20, 700);
    p += 96000 * 2;
    p += sbitx_cw_encode("DE PU2UIT K", x + p, 96000 * 6, 20, 700);
    p += 96000;
    int m = to12k(x, p, noise_rms, y);
    const char *got = cw_decode(y, m);
    printf("CW   noise %.3f: [%s]\n", noise_rms, got);
    CHECK(strcmp(got, "HERMES TEST CW DE PU2UIT K") == 0, "CW at noise %.3f decoded [%s]", noise_rms, got);
}

static void test_rtty(float noise_rms)
{
    static float x[96000 * 12], y[12000 * 12];
    memset(x, 0, sizeof x);
    int n = sbitx_rtty_encode("RYRY HERMES TEST RTTY 73", x + 9600, 96000 * 10, 45, 1585, 170);
    int m = to12k(x, 9600 + n + 9600, noise_rms, y);
    const char *got = rtty_decode(y, m);
    printf("RTTY noise %.3f: [%s]\n", noise_rms, got);
    CHECK(strcmp(got, "RYRY HERMES TEST RTTY 73") == 0, "RTTY at noise %.3f decoded [%s]", noise_rms, got);
}

static void test_ft8(float noise_rms)
{
    static float s12[12000 * 16], x[96000 * 16], y[12000 * 16];
    memset(x, 0, sizeof x);
    int n12 = sbitx_ft8_encode("HERMES TEST.9", s12, 12000 * 16, 1500.0f);
    for (int i = 0; i < n12 && (i + 1) * 8 < 96000 * 15; i++)      /* crude 8x hold, */
        for (int k = 0; k < 8; k++)                                  /* filtered by RX */
            x[9600 * 2 + i * 8 + k] = 0.3f * s12[i];
    int m = to12k(x, 96000 * 15, noise_rms, y);
    char out[1024];
    sbitx_ft8_decode(y, m < 12000 * 15 ? m : 12000 * 15, out, sizeof out);
    printf("FT8  noise %.3f: [%s]\n", noise_rms, out);
    CHECK(strcmp(out, "HERMES TEST.9") == 0, "FT8 at noise %.3f decoded [%s]", noise_rms, out);
}

static void test_noise_only(void)
{
    static float x[96000 * 10], y[12000 * 10];
    memset(x, 0, sizeof x);
    int m = to12k(x, 96000 * 10, 0.3f, y);
    char cw[512];
    snprintf(cw, sizeof cw, "%s", cw_decode(y, m));
    int cw_n = (int) strlen(cw);
    const char *rt = rtty_decode(y, m);
    int rt_n = (int) strlen(rt);
    printf("noise only, 10 s: CW [%s] RTTY [%s]\n", cw, rt);
    CHECK(cw_n <= 2, "CW printed %d characters from noise", cw_n);
    CHECK(rt_n <= 2, "RTTY printed %d characters from noise", rt_n);
}

int main(void)
{
    test_cw(0.0f);
    test_cw(0.4f);
    test_rtty(0.0f);
    test_rtty(0.3f);
    test_ft8(0.0f);
    test_ft8(0.5f);
    test_noise_only();
    if (failures) {
        printf("digi_modes_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("digi_modes_test: ok\n");
    return 0;
}
