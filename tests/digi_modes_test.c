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
#include "dsp/digi_log.h"
#include <unistd.h>

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

/* One structured decode: the fields come from ft8_lib's field map, the
 * offset from the decoder, and the SNR estimate falls as the noise rises. */
static float test_ft8_fields(const char *msg, float noise_rms, const char *want_to,
                             const char *want_from, const char *want_grid)
{
    static float s12[12000 * 16], x[96000 * 16], y[12000 * 16];
    memset(x, 0, sizeof x);
    int n12 = sbitx_ft8_encode(msg, s12, 12000 * 16, 1500.0f);
    for (int i = 0; i < n12 && (i + 1) * 8 < 96000 * 15; i++)
        for (int k = 0; k < 8; k++)
            x[9600 * 2 + i * 8 + k] = 0.3f * s12[i];
    int m = to12k(x, 96000 * 15, noise_rms, y);
    sbitx_ft8_decode_result r[8];
    int n = sbitx_ft8_decode_ex(y, m < 12000 * 15 ? m : 12000 * 15, r, 8);
    CHECK(n == 1, "FT8 [%s] at noise %.3f: %d decodes", msg, noise_rms, n);
    if (n < 1)
        return NAN;
    printf("FT8  [%s] noise %.3f: to=[%s] from=[%s] grid=[%s] offset %.1f Hz t %.2f s snr %s%.0f dB\n",
           r[0].text, noise_rms, r[0].to, r[0].from, r[0].grid, r[0].freq_hz, r[0].time_s,
           r[0].has_snr ? "" : "(none) ", r[0].snr_db);
    CHECK(strcmp(r[0].text, msg) == 0, "FT8 text [%s]", r[0].text);
    CHECK(strcmp(r[0].to, want_to) == 0, "FT8 [%s] to [%s], want [%s]", msg, r[0].to, want_to);
    CHECK(strcmp(r[0].from, want_from) == 0, "FT8 [%s] from [%s], want [%s]", msg, r[0].from, want_from);
    CHECK(strcmp(r[0].grid, want_grid) == 0, "FT8 [%s] grid [%s], want [%s]", msg, r[0].grid, want_grid);
    CHECK(fabsf(r[0].freq_hz - 1500.0f) < 10.0f, "FT8 offset %.1f Hz, want about 1500", r[0].freq_hz);
    /* The signal starts 0.2 s into the window; ft8_lib resolves 0.08 s. */
    CHECK(fabsf(r[0].time_s - 0.2f) < 0.1f, "FT8 start %.2f s into the window, want 0.2", r[0].time_s);
    CHECK(r[0].has_snr && r[0].snr_db >= -30 && r[0].snr_db <= 30, "FT8 SNR %.0f dB", r[0].snr_db);
    return r[0].snr_db;
}

/* The windows slide by 2 s, so one transmission decodes in several: it is
 * logged once, timed from the wall clock, with dt against the UTC slot. */
static void test_ft8_log_once(void)
{
    char dir[256];
    const char *t = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/ft8_log_test.XXXXXX", t && t[0] ? t : "/tmp");
    if (!mkdtemp(dir)) { CHECK(0, "mkdtemp"); return; }
    digi_log_set_dir(dir);

    /* 2026-10-09 18:30:15 UTC is a slot start; the signal starts 0.5 s in. */
    const double slot = 1791570615.0, start = slot + 0.5;
    sbitx_ft8_decode_result r = { .text = "CQ PU2UIT GG66", .from = "PU2UIT", .to = "CQ",
                                  .grid = "GG66", .freq_hz = 1500.0f, .snr_db = -7.0f, .has_snr = true };
    for (int w = 0; w < 3; w++) {             /* windows starting 4, 2 and 0 s before it */
        double window_start = start - 4.0 + 2.0 * w;
        r.time_s = (float) (start - window_start);
        sbitx_ft8_log_decodes(&r, 1, window_start, 14074000);
    }
    char *arr = digi_log_last_json(10);
    int entries = 0;
    for (const char *p = arr; p && (p = strstr(p, "\"id\":")); p++) entries++;
    printf("FT8 log after 3 overlapping windows: %s\n", arr);
    CHECK(entries == 1, "one transmission logged %d times", entries);
    CHECK(arr && strstr(arr, "\"at\":\"2026-10-09T18:30:15.500Z\""), "start time");
    CHECK(arr && strstr(arr, "\"dt_s\":0.0") && strstr(arr, "\"snr_db\":-7") && strstr(arr, "\"offset_hz\":1500"),
          "dt, snr and offset");
    CHECK(arr && strstr(arr, "\"from\":\"PU2UIT\",\"to\":\"CQ\",\"grid\":\"GG66\""), "fields");
    free(arr);

    /* The same text 15 s later is a new transmission. */
    r.time_s = 4.0f;
    sbitx_ft8_log_decodes(&r, 1, start + 15.0 - 4.0, 14074000);
    arr = digi_log_last_json(10);
    entries = 0;
    for (const char *p = arr; p && (p = strstr(p, "\"id\":")); p++) entries++;
    CHECK(entries == 2, "a repeat 15 s later should be logged: %d entries", entries);
    free(arr);

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) { }
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
    float snr_quiet = test_ft8_fields("CQ PU2UIT GG66", 0.02f, "CQ", "PU2UIT", "GG66");
    float snr_noisy = test_ft8_fields("CQ PU2UIT GG66", 0.2f, "CQ", "PU2UIT", "GG66");
    CHECK(snr_noisy < snr_quiet - 3, "FT8 SNR should fall with noise: %.0f dB quiet, %.0f dB noisy",
          snr_quiet, snr_noisy);
    test_ft8_fields("PU2UIT PY2XYZ -12", 0.05f, "PU2UIT", "PY2XYZ", "");
    test_ft8_fields("HERMES TEST.9", 0.05f, "", "", "");
    test_ft8_log_once();

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
