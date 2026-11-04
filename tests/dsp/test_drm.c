/* test_drm - Dream behind its pipes (dream_proc), and DRM's own arithmetic
 *
 * dream_proc is tested with cat in Dream's place: what goes in comes back,
 * in order, the end of the input ends it, and a program that is not there
 * only stops running. The mix-down onto the dial and the transmitter's
 * Dream.ini need no Dream either.
 *
 * With DREAM set to a Dream (the xhe-aac-tx branch's console build, with
 * fdk-xhe-aac), the whole loop runs through radiod's code: a tone pattern
 * into the transmitter, its I/Q (centred on the dial) into the receiver,
 * and the tones back.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "unity.h"
#include "dsp/dream_proc.h"
#include "dsp/sbitx_drm.h"

static char dir[256];

void setUp(void)
{
    snprintf(dir, sizeof(dir), "%s/test_drm.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
}

void tearDown(void)
{
    char cmd[300];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) { /* a leftover directory */ }
}

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void test_cat_echoes_in_order(void)
{
    static const char *const args[] = { "cat", NULL };
    static int16_t in[20000], out[20000];
    size_t got = 0, sent = 0;
    char ini[300];
    FILE *f;
    double end;

    dream_proc *p = dream_proc_start("cat", dir, "[Receiver]\n", args, 8192);
    TEST_ASSERT_NOT_NULL(p);
    snprintf(ini, sizeof(ini), "%s/Dream.ini", dir);
    f = fopen(ini, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "Dream.ini not written");
    fclose(f);

    for (int i = 0; i < 20000; i++)
        in[i] = (int16_t) (i * 7 - 30000);

    /* more than a ring holds: written as it drains */
    end = now() + 5;
    while (got < 20000 && now() < end) {
        if (sent < 20000)
            sent += dream_proc_write(p, in + sent, 20000 - sent);
        got += dream_proc_read(p, out + got, 20000 - got);
        usleep(1000);
    }
    TEST_ASSERT_EQUAL_UINT(20000, got);
    TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, 20000);

    /* the end of the input ends cat */
    dream_proc_finish(p);
    end = now() + 3;
    while (dream_proc_running(p) && now() < end)
        usleep(1000);
    TEST_ASSERT_FALSE_MESSAGE(dream_proc_running(p), "the end of the input did not end it");
    dream_proc_stop(p);
}

static void test_missing_program_stops_running(void)
{
    static const char *const args[] = { "no-such-dream", NULL };
    double end;

    dream_proc *p = dream_proc_start("/nonexistent/no-such-dream", dir, NULL, args, 1024);
    TEST_ASSERT_NOT_NULL(p);
    end = now() + 3;
    while (dream_proc_running(p) && now() < end)
        usleep(1000);
    TEST_ASSERT_FALSE(dream_proc_running(p));
    /* writes go nowhere, quietly; stop still tidies up */
    int16_t s[16] = { 0 };
    dream_proc_write(p, s, 16);
    dream_proc_stop(p);
}

/* +6 kHz in, DC out, and the phase carries from call to call */
static void test_mix_down(void)
{
    enum { N = 4800 };
    static float i1[N], q1[N], i2[N], q2[N];
    double ph1 = 0, ph2 = 0;

    for (int n = 0; n < N; n++) {
        double a = 2 * M_PI * 6000.0 * n / 48000.0 + 0.3;
        i1[n] = i2[n] = (float) cos(a);
        q1[n] = q2[n] = (float) sin(a);
    }
    sbitx_drm_mix(i1, q1, N, 6000.0, 48000.0, &ph1);
    sbitx_drm_mix(i2, q2, 1000, 6000.0, 48000.0, &ph2);
    sbitx_drm_mix(i2 + 1000, q2 + 1000, N - 1000, 6000.0, 48000.0, &ph2);

    for (int n = 0; n < N; n++) {
        TEST_ASSERT_FLOAT_WITHIN(1e-4, cos(0.3), i1[n]);
        TEST_ASSERT_FLOAT_WITHIN(1e-4, sin(0.3), q1[n]);
        TEST_ASSERT_FLOAT_WITHIN(1e-5, i1[n], i2[n]);
        TEST_ASSERT_FLOAT_WITHIN(1e-5, q1[n], q2[n]);
    }
}

static void test_tx_ini(void)
{
    sbitx_drm_tx_config c = { .robustness = 'B', .spectrum = 3, .msc_qam = 64, .protection = 1,
                              .codec = "xHE-AAC", .label = "HERMES" };
    char ini[1024];

    TEST_ASSERT_GREATER_THAN(0, sbitx_drm_tx_ini(&c, ini, sizeof(ini)));
    TEST_ASSERT_NOT_NULL(strstr(ini, "robustness=RM_ROBUSTNESS_MODE_B\n"));
    TEST_ASSERT_NOT_NULL(strstr(ini, "spectocc=SO_3\n"));
    TEST_ASSERT_NOT_NULL(strstr(ini, "msc=CS_3_SM\n"));
    TEST_ASSERT_NOT_NULL(strstr(ini, "ifformat=OF_IQ_POS\n"));
    TEST_ASSERT_NOT_NULL(strstr(ini, "iffreq=6000\n"));
    TEST_ASSERT_NOT_NULL(strstr(ini, "codec=xHE-AAC\n"));
    TEST_ASSERT_NOT_NULL(strstr(ini, "label=HERMES\n"));

    /* out of range: the defaults, never a Dream.ini Dream would misread */
    c.robustness = 'Z';
    c.spectrum = 9;
    c.msc_qam = 32;
    c.protection = 7;
    c.codec[0] = c.label[0] = '\0';
    TEST_ASSERT_GREATER_THAN(0, sbitx_drm_tx_ini(&c, ini, sizeof(ini)));
    TEST_ASSERT_NOT_NULL(strstr(ini, "robustness=RM_ROBUSTNESS_MODE_B\n"));
    TEST_ASSERT_NOT_NULL(strstr(ini, "spectocc=SO_3\n"));
    TEST_ASSERT_NOT_NULL(strstr(ini, "msc=CS_3_SM\n"));
    TEST_ASSERT_NOT_NULL(strstr(ini, "protlevel=1\n"));
    TEST_ASSERT_NOT_NULL(strstr(ini, "codec=xHE-AAC\n"));
    TEST_ASSERT_EQUAL_INT(-1, sbitx_drm_tx_ini(&c, ini, 40));
}

/* the strongest of 440 and 880 Hz in a second of audio, or 0 if quiet */
static int tone_of(const float *a, int n)
{
    double p440r = 0, p440i = 0, p880r = 0, p880i = 0, e = 0;

    for (int k = 0; k < n; k++) {
        double t = (double) k / 48000.0;
        p440r += a[k] * cos(2 * M_PI * 440 * t);
        p440i += a[k] * sin(2 * M_PI * 440 * t);
        p880r += a[k] * cos(2 * M_PI * 880 * t);
        p880i += a[k] * sin(2 * M_PI * 880 * t);
        e += a[k] * a[k];
    }
    if (e / n < 1e-6)
        return 0;
    return p440r * p440r + p440i * p440i > p880r * p880r + p880i * p880i ? 440 : 880;
}

/* Dream's transmitter into its receiver, through radiod's code */
static void test_loopback_through_dream(void)
{
    const char *dream = getenv("DREAM");
    char txdir[300], rxdir[300];
    enum { SECONDS = 16 };
    static float tone[48000 * SECONDS], audio[48000 * (SECONDS + 8)];
    static float i_[4096], q_[4096];
    sbitx_drm_tx_config c = { .robustness = 'B', .spectrum = 3, .msc_qam = 64, .protection = 1,
                              .codec = "xHE-AAC", .label = "HERMES TEST" };
    size_t fed = 0, got = 0;
    double end, peak = 0, off = 0;
    long iq_n = 0;

    if (!dream || !dream[0])
        TEST_IGNORE_MESSAGE("DREAM not set: no Dream to loop through");

    for (int n = 0; n < 48000 * SECONDS; n++)
        tone[n] = 0.35f * (float) sin(2 * M_PI * ((n / 48000) % 2 ? 880 : 440) * n / 48000.0);

    snprintf(txdir, sizeof(txdir), "%s/tx", dir);
    snprintf(rxdir, sizeof(rxdir), "%s/rx", dir);
    TEST_ASSERT_TRUE(sbitx_drm_tx_start(dream, txdir, &c));
    TEST_ASSERT_TRUE(sbitx_drm_rx_start(dream, rxdir));

    end = now() + 90;
    while (got < sizeof(audio) / sizeof(audio[0]) && now() < end) {
        if (fed < 48000 * SECONDS) {
            size_t k = 48000 * SECONDS - fed < 2400 ? 48000 * SECONDS - fed : 2400;

            sbitx_drm_tx_audio(tone + fed, k);
            fed += k;
        } else {
            static const float silence[2400];

            sbitx_drm_tx_audio(silence, 2400);  /* the interleaver's tail out */
        }

        size_t n = sbitx_drm_tx_iq(i_, q_, 4096);
        for (size_t k = 0; k < n; k++) {
            /* centred on the dial: no 6 kHz carrier left in the mean */
            off += i_[k];
            double m = fabs(i_[k]) > fabs(q_[k]) ? fabs(i_[k]) : fabs(q_[k]);
            if (m > peak) peak = m;
        }
        iq_n += (long) n;
        sbitx_drm_rx_iq(i_, q_, n);
        got += sbitx_drm_rx_audio(audio + got, sizeof(audio) / sizeof(audio[0]) - got);
        usleep(12000);                  /* 50 ms of audio each 12 ms: 4x real time */
    }
    sbitx_drm_tx_stop();
    sbitx_drm_rx_stop();

    printf("DRM loopback: %ld I/Q samples (peak %.2f), %zu audio samples\n", iq_n, peak, got);
    TEST_ASSERT_GREATER_THAN_MESSAGE(48000 * 8, iq_n, "the transmitter gave little I/Q");
    TEST_ASSERT_LESS_THAN_DOUBLE_MESSAGE(1.0, peak, "the I/Q clipped");

    /* the tones, second by second, once the receiver has locked */
    int match = 0, seconds = 0;
    char pattern[256] = "";

    for (int s = 0; (size_t) (s + 1) * 48000 <= got && s < 40; s++) {
        int t = tone_of(audio + s * 48000, 48000);
        char w[8];

        snprintf(w, sizeof(w), " %d", t);
        strncat(pattern, w, sizeof(pattern) - strlen(pattern) - 1);
        if (t) {
            seconds++;
            /* the receiver lags the transmitter: the pattern alternates */
            if (s > 0 && t != tone_of(audio + (s - 1) * 48000, 48000))
                match++;
        }
    }
    printf("DRM loopback tones:%s\n", pattern);
    TEST_ASSERT_GREATER_THAN_MESSAGE(8, seconds, "the receiver decoded too little");
    TEST_ASSERT_GREATER_THAN_MESSAGE(6, match, "the decoded audio is not the tones");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cat_echoes_in_order);
    RUN_TEST(test_missing_program_stops_running);
    RUN_TEST(test_mix_down);
    RUN_TEST(test_tx_ini);
    RUN_TEST(test_loopback_through_dream);
    return UNITY_END();
}
