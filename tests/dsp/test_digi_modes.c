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
#include "unity.h"
#include "test_util.h"


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
static void check_cw(float noise_rms)
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
    TEST_ASSERT_EQUAL_STRING_MESSAGE("HERMES TEST CW DE PU2UIT K", got, test_msg("CW at noise %.3f decoded [%s]", noise_rms, got));
}

static void check_rtty(float noise_rms)
{
    static float x[96000 * 12], y[12000 * 12];
    memset(x, 0, sizeof x);
    int n = sbitx_rtty_encode("RYRY HERMES TEST RTTY 73", x + 9600, 96000 * 10, 45, 1585, 170);
    int m = to12k(x, 9600 + n + 9600, noise_rms, y);
    const char *got = rtty_decode(y, m);
    printf("RTTY noise %.3f: [%s]\n", noise_rms, got);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("RYRY HERMES TEST RTTY 73", got, test_msg("RTTY at noise %.3f decoded [%s]", noise_rms, got));
}

static void check_ft8(float noise_rms)
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
    TEST_ASSERT_EQUAL_STRING_MESSAGE("HERMES TEST.9", out, test_msg("FT8 at noise %.3f decoded [%s]", noise_rms, out));
}

/* One structured decode: the fields come from ft8_lib's field map, the
 * offset from the decoder, and the SNR estimate falls as the noise rises. */
static float check_ft8_fields(const char *msg, float noise_rms, const char *want_to,
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
    TEST_ASSERT_EQUAL_INT64_MESSAGE(1, n, test_msg("FT8 [%s] at noise %.3f: %d decodes", msg, noise_rms, n));
    if (n < 1)
        return NAN;
    printf("FT8  [%s] noise %.3f: to=[%s] from=[%s] grid=[%s] offset %.1f Hz t %.2f s snr %s%.0f dB\n",
           r[0].text, noise_rms, r[0].to, r[0].from, r[0].grid, r[0].freq_hz, r[0].time_s,
           r[0].has_snr ? "" : "(none) ", r[0].snr_db);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(msg, r[0].text, test_msg("FT8 text [%s]", r[0].text));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want_to, r[0].to, test_msg("FT8 [%s] to [%s], want [%s]", msg, r[0].to, want_to));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want_from, r[0].from, test_msg("FT8 [%s] from [%s], want [%s]", msg, r[0].from, want_from));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want_grid, r[0].grid, test_msg("FT8 [%s] grid [%s], want [%s]", msg, r[0].grid, want_grid));
    TEST_ASSERT_TRUE_MESSAGE(fabsf(r[0].freq_hz - 1500.0f) < 10.0f, test_msg("FT8 offset %.1f Hz, want about 1500", r[0].freq_hz));
    /* The signal starts 0.2 s into the window; ft8_lib resolves 0.08 s. */
    TEST_ASSERT_TRUE_MESSAGE(fabsf(r[0].time_s - 0.2f) < 0.1f, test_msg("FT8 start %.2f s into the window, want 0.2", r[0].time_s));
    do { TEST_ASSERT_TRUE_MESSAGE(r[0].has_snr, test_msg("FT8 SNR %.0f dB", r[0].snr_db)); TEST_ASSERT_GREATER_OR_EQUAL_INT64_MESSAGE(-30, r[0].snr_db, test_msg("FT8 SNR %.0f dB", r[0].snr_db)); TEST_ASSERT_LESS_OR_EQUAL_INT64_MESSAGE(30, r[0].snr_db, test_msg("FT8 SNR %.0f dB", r[0].snr_db)); } while (0);
    return r[0].snr_db;
}

/* The windows slide by 2 s, so one transmission decodes in several: it is
 * logged once, timed from the wall clock, with dt against the UTC slot. */
static void test_ft8_log_once(void)
{
    char dir[256];
    const char *t = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/ft8_log_test.XXXXXX", t && t[0] ? t : "/tmp");
    if (!mkdtemp(dir)) { TEST_FAIL_MESSAGE("mkdtemp"); return; }
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
    TEST_ASSERT_EQUAL_INT64_MESSAGE(1, entries, test_msg("one transmission logged %d times", entries));
    do { TEST_ASSERT_TRUE_MESSAGE(arr, "start time"); TEST_ASSERT_NOT_NULL_MESSAGE(strstr(arr, "\"at\":\"2026-10-09T18:30:15.500Z\""), "start time"); } while (0);
    do { TEST_ASSERT_TRUE_MESSAGE(arr, "dt, snr and offset"); TEST_ASSERT_NOT_NULL_MESSAGE(strstr(arr, "\"dt_s\":0.0"), "dt, snr and offset"); TEST_ASSERT_NOT_NULL_MESSAGE(strstr(arr, "\"snr_db\":-7"), "dt, snr and offset"); TEST_ASSERT_NOT_NULL_MESSAGE(strstr(arr, "\"offset_hz\":1500"), "dt, snr and offset"); } while (0);
    do { TEST_ASSERT_TRUE_MESSAGE(arr, "fields"); TEST_ASSERT_NOT_NULL_MESSAGE(strstr(arr, "\"from\":\"PU2UIT\",\"to\":\"CQ\",\"grid\":\"GG66\""), "fields"); } while (0);
    free(arr);

    /* The same text 15 s later is a new transmission. */
    r.time_s = 4.0f;
    sbitx_ft8_log_decodes(&r, 1, start + 15.0 - 4.0, 14074000);
    arr = digi_log_last_json(10);
    entries = 0;
    for (const char *p = arr; p && (p = strstr(p, "\"id\":")); p++) entries++;
    TEST_ASSERT_EQUAL_INT64_MESSAGE(2, entries, test_msg("a repeat 15 s later should be logged: %d entries", entries));
    free(arr);

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) { }
}

static int count_entries(const char *arr)
{
    int n = 0;
    for (const char *p = arr; p && (p = strstr(p, "\"id\":")); p++) n++;
    return n;
}

/* Two DIFFERENT stations sending identical text at the same moment -- "73",
 * "RR73", a beacon -- are two entries. They are told apart by audio offset:
 * keying the dedup on text and time alone merged them into one. One
 * transmission seen in overlapping windows keeps its offset, so it must
 * still collapse to one entry. */
static void test_ft8_same_text_two_stations(void)
{
    char dir[256];
    const char *t = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/ft8_dedup_test.XXXXXX", t && t[0] ? t : "/tmp");
    if (!mkdtemp(dir)) { TEST_FAIL_MESSAGE("mkdtemp"); return; }
    digi_log_set_dir(dir);

    const double start = 1791570615.5;
    sbitx_ft8_decode_result a = { .text = "73", .freq_hz = 1200.0f };
    sbitx_ft8_decode_result b = { .text = "73", .freq_hz = 1850.0f };
    for (int w = 0; w < 3; w++) {              /* three overlapping windows */
        double window_start = start - 4.0 + 2.0 * w;
        a.time_s = b.time_s = (float) (start - window_start);
        /* the same transmission decodes a sub-bin apart in each window */
        a.freq_hz = 1200.0f + 3.125f * (w % 2);
        sbitx_ft8_decode_result both[2] = { a, b };
        sbitx_ft8_log_decodes(both, 2, window_start, 14074000);
    }
    char *arr = digi_log_last_json(10);
    int n = count_entries(arr);
    printf("same text, two stations, 3 windows: %s\n", arr);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(2, n, test_msg("two stations sending \"73\" at once should be 2 entries, got %d", n));
    do { TEST_ASSERT_TRUE_MESSAGE(arr, "both offsets kept"); TEST_ASSERT_NOT_NULL_MESSAGE(strstr(arr, "\"offset_hz\":1200"), "both offsets kept"); TEST_ASSERT_NOT_NULL_MESSAGE(strstr(arr, "\"offset_hz\":1850"), "both offsets kept"); } while (0);
    free(arr);

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) { }
}

/* dt folds symmetrically into [-7.5, 7.5) as WSJT-X does. The old fold at
 * +7 made 6.9 stay and 7.1 jump to -7.9. */
static void test_ft8_dt_fold(void)
{
    struct { double offset_in_slot; float want; } cases[] = {
        { 0.5,  0.0f },     /* on time */
        { 7.4,  6.9f },     /* below the fold: stays positive */
        { 7.6,  7.1f },     /* the old code wrapped this to -7.9 */
        { 7.99, 7.49f },    /* just under +7.5 */
        { 8.0, -7.5f },     /* exactly at the fold wraps */
        { 14.9, -0.6f },    /* late, i.e. early for the next slot */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char dir[256];
        const char *t = getenv("TMPDIR");
        snprintf(dir, sizeof dir, "%s/ft8_dt_test.XXXXXX", t && t[0] ? t : "/tmp");
        if (!mkdtemp(dir)) { TEST_FAIL_MESSAGE("mkdtemp"); return; }
        digi_log_set_dir(dir);

        const double slot = 1791570615.0 + 30.0 * (double) i;  /* fresh dedup key */
        sbitx_ft8_decode_result r = { .text = "CQ PU2UIT GG66", .freq_hz = 1000.0f + 100.0f * (float) i,
                                      .time_s = 0.0f };
        sbitx_ft8_log_decodes(&r, 1, slot + cases[i].offset_in_slot, 14074000);
        char *arr = digi_log_last_json(1);
        const char *p = arr ? strstr(arr, "\"dt_s\":") : NULL;
        float got = p ? strtof(p + 7, NULL) : 99.0f;
        do { TEST_ASSERT_TRUE_MESSAGE(p, test_msg("dt at %.2f s into the slot: got %.1f, want %.1f", cases[i].offset_in_slot, got, cases[i].want)); TEST_ASSERT_TRUE_MESSAGE(fabsf(got - cases[i].want) < 0.051f, test_msg("dt at %.2f s into the slot: got %.1f, want %.1f", cases[i].offset_in_slot, got, cases[i].want)); } while (0);
        /* dt is folded into [-7.5, 7.5) but logged with one decimal, so
         * 7.45..7.49 prints as 7.5: check the printed range inclusively. */
        do { TEST_ASSERT_TRUE_MESSAGE(p, test_msg("dt %.1f outside [-7.5, 7.5]", got)); TEST_ASSERT_TRUE_MESSAGE(got >= -7.5f, test_msg("dt %.1f outside [-7.5, 7.5]", got)); TEST_ASSERT_TRUE_MESSAGE(got <= 7.5f, test_msg("dt %.1f outside [-7.5, 7.5]", got)); } while (0);
        free(arr);

        char cmd[300];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0) { }
    }
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
    TEST_ASSERT_LESS_OR_EQUAL_INT64_MESSAGE(2, cw_n, test_msg("CW printed %d characters from noise", cw_n));
    TEST_ASSERT_LESS_OR_EQUAL_INT64_MESSAGE(2, rt_n, test_msg("RTTY printed %d characters from noise", rt_n));
}

void setUp(void) {}
void tearDown(void) {}

static void test_ft8_snr_falls_with_noise(void)
{
    float snr_quiet = check_ft8_fields("CQ PU2UIT GG66", 0.02f, "CQ", "PU2UIT", "GG66");
    float snr_noisy = check_ft8_fields("CQ PU2UIT GG66", 0.2f, "CQ", "PU2UIT", "GG66");
    TEST_ASSERT_LESS_THAN_FLOAT_MESSAGE(snr_quiet - 3, snr_noisy,
        test_msg("FT8 SNR should fall with noise: %.0f dB quiet, %.0f dB noisy", snr_quiet, snr_noisy));
}

static void test_ft8_fields_report(void)    { check_ft8_fields("PU2UIT PY2XYZ -12", 0.05f, "PU2UIT", "PY2XYZ", ""); }
static void test_ft8_fields_free_text(void) { check_ft8_fields("HERMES TEST.9", 0.05f, "", "", ""); }
static void test_cw_clean(void)             { check_cw(0.0f); }
static void test_cw_noisy(void)             { check_cw(0.4f); }
static void test_rtty_clean(void)           { check_rtty(0.0f); }
static void test_rtty_noisy(void)           { check_rtty(0.3f); }
static void test_ft8_clean(void)            { check_ft8(0.0f); }
static void test_ft8_noisy(void)            { check_ft8(0.5f); }

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ft8_snr_falls_with_noise);
    RUN_TEST(test_ft8_fields_report);
    RUN_TEST(test_ft8_fields_free_text);
    RUN_TEST(test_ft8_log_once);
    RUN_TEST(test_ft8_same_text_two_stations);
    RUN_TEST(test_ft8_dt_fold);
    RUN_TEST(test_cw_clean);
    RUN_TEST(test_cw_noisy);
    RUN_TEST(test_rtty_clean);
    RUN_TEST(test_rtty_noisy);
    RUN_TEST(test_ft8_clean);
    RUN_TEST(test_ft8_noisy);
    RUN_TEST(test_noise_only);
    return UNITY_END();
}
