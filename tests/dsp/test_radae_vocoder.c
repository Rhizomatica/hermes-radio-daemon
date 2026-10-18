/* radae_vocoder_test — in-process LPCNet feature extraction and FARGAN
 * synthesis (dsp/radae_vocoder.c), the speech side of RADE.
 *
 * Bit-exactness against rade_c's lpcnet_demo was checked when this module
 * replaced the lpcnet_demo helper processes (synthesis identical; features
 * identical to an Opus build without SIMD intrinsics). These checks keep
 * the wrapper's own contract: frame sizes, FARGAN priming, reset, and sane
 * output.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "test_util.h"
#include "dsp/radae_vocoder.h"

#define NFRAMES 200   /* 2 s */

static int16_t speech[NFRAMES][RADAE_VOCODER_FRAME];
static float   feats[NFRAMES][RADAE_VOCODER_FEATURES];
static bool    have_feats;

/* A vowel-like test signal: 140 Hz harmonics under a slow syllable envelope. */
static void make_speech(void)
{
    for (int f = 0; f < NFRAMES; f++)
        for (int i = 0; i < RADAE_VOCODER_FRAME; i++) {
            double t = (f * RADAE_VOCODER_FRAME + i) / 16000.0;
            double env = 0.55 + 0.45 * sin(2 * M_PI * 2.5 * t);
            double s = 0;
            for (int h = 1; h <= 20; h++)
                s += sin(2 * M_PI * 140.0 * h * t) / h;
            speech[f][i] = (int16_t) (6000.0 * env * s);
        }
}

static double rms16(const int16_t *x, int n)
{
    double e = 0;
    for (int i = 0; i < n; i++)
        e += (double) x[i] * x[i];
    return sqrt(e / n);
}

static void test_analysis(void)
{
    radae_analysis *a = radae_analysis_new();
    TEST_ASSERT_NOT_NULL(a);

    int finite = 1;
    for (int f = 0; f < NFRAMES; f++) {
        radae_analysis_frame(a, speech[f], feats[f]);
        for (int k = 0; k < RADAE_VOCODER_FEATURES; k++)
            finite &= isfinite(feats[f][k]);
    }
    TEST_ASSERT_TRUE_MESSAGE(finite, "non-finite feature");

    /* After a reset the same speech gives the same features. */
    float again[RADAE_VOCODER_FEATURES];
    radae_analysis_reset(a);
    int same = 1;
    for (int f = 0; f < NFRAMES; f++) {
        radae_analysis_frame(a, speech[f], again);
        same &= memcmp(again, feats[f], sizeof(again)) == 0;
    }
    radae_analysis_free(a);
    TEST_ASSERT_TRUE_MESSAGE(same, "reset analysis does not reproduce the first pass");
    have_feats = true;
}

static void test_synthesis(void)
{
    if (!have_feats)
        TEST_IGNORE_MESSAGE("needs test_analysis's features");
    radae_synthesis *s = radae_synthesis_new();
    TEST_ASSERT_NOT_NULL(s);

    int16_t pcm[RADAE_VOCODER_FRAME];
    static int16_t out[NFRAMES][RADAE_VOCODER_FRAME];

    for (int pass = 0; pass < 2; pass++) {
        int produced = 0;
        for (int f = 0; f < NFRAMES; f++) {
            int n = radae_synthesis_frame(s, feats[f], pcm);
            if (f < 5)
                TEST_ASSERT_EQUAL_INT_MESSAGE(0, n, test_msg("pass %d: priming frame %d", pass, f));
            else
                TEST_ASSERT_EQUAL_INT_MESSAGE(RADAE_VOCODER_FRAME, n, test_msg("pass %d: frame %d", pass, f));
            if (n == RADAE_VOCODER_FRAME)
                memcpy(out[produced++], pcm, sizeof(pcm));
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(NFRAMES - 5, produced, test_msg("pass %d: frames out", pass));

        /* Speech in, speech out: level within a factor of 4 over the voiced
         * part (FARGAN does not reproduce the waveform, only the sound). */
        double in = rms16(&speech[20][0], (NFRAMES - 20) * RADAE_VOCODER_FRAME);
        double got = rms16(&out[15][0], (NFRAMES - 20) * RADAE_VOCODER_FRAME);
        TEST_ASSERT_TRUE_MESSAGE(got > in / 4 && got < in * 4,
                                 test_msg("pass %d: output rms %.0f vs input %.0f", pass, got, in));

        radae_synthesis_reset(s);   /* the second pass must prime again */
    }
    radae_synthesis_free(s);
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    make_speech();
    UNITY_BEGIN();
    RUN_TEST(test_analysis);
    RUN_TEST(test_synthesis);
    return UNITY_END();
}
