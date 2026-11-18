/* capture_levels: a tone's rms and peak in dBFS, the HF ratio telling the
 * receiver's IF (24 kHz at 96 kHz) from audio-band content, each channel
 * on its own, digital silence, and blocks added in pieces counting as one.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "test_util.h"
#include "dsp/capture_levels.h"

#define FS_HZ        96000
#define IF_HZ        24000.0   /* the sBitx receiver's IF */
#define AUDIO_HZ     1000.0    /* a microphone-band tone */
#define FRAMES       FS_HZ     /* one second */
#define FULL_SCALE   2147483647.0
#define AMPL_DBFS    (-20.0)
#define RMS_SINE_DB  (-3.0103)  /* a sine's rms below its peak */
#define TOL_DB       0.05
#define TOL_RATIO    0.01
#define IF_RATIO     1.4142     /* 2 sin(pi * 24000 / 96000) */
#define AUDIO_RATIO  0.0654     /* 2 sin(pi * 1000 / 96000) */
#define PIECE        1000       /* frames per call in the split test */

static int32_t frames[FRAMES * CAPTURE_LEVELS_CHANNELS];

void setUp(void) { memset(frames, 0, sizeof(frames)); }
void tearDown(void) { }

/* Channel ch carries a tone at hz, the other one is left as it is. */
static void tone(int ch, double hz, double dbfs)
{
    double a = FULL_SCALE * pow(10.0, dbfs / 20.0);
    for (int i = 0; i < FRAMES; i++)
        frames[i * CAPTURE_LEVELS_CHANNELS + ch] = (int32_t) lrint(a * sin(2.0 * M_PI * hz * i / FS_HZ));
}

void test_if_and_mic_tones_read_their_level_and_ratio(void)
{
    tone(0, IF_HZ, AMPL_DBFS);
    tone(1, AUDIO_HZ, AMPL_DBFS - 20.0);
    capture_levels c;
    capture_levels_reset(&c);
    capture_levels_add(&c, frames, FRAMES);

    capture_channel_level r, m;
    capture_levels_get(&c, 0, &r);
    capture_levels_get(&c, 1, &m);
    TEST_ASSERT_DOUBLE_WITHIN(TOL_DB, AMPL_DBFS + RMS_SINE_DB, r.rms_dbfs);
    TEST_ASSERT_DOUBLE_WITHIN(TOL_DB, AMPL_DBFS, r.peak_dbfs);
    TEST_ASSERT_DOUBLE_WITHIN(TOL_RATIO, IF_RATIO, r.hf_ratio);
    TEST_ASSERT_DOUBLE_WITHIN(TOL_DB, AMPL_DBFS - 20.0 + RMS_SINE_DB, m.rms_dbfs);
    TEST_ASSERT_DOUBLE_WITHIN(TOL_RATIO, AUDIO_RATIO, m.hf_ratio);
}

/* Swapped channels read swapped: the figures follow the samples, not a slot. */
void test_swapped_channels_read_swapped(void)
{
    tone(0, AUDIO_HZ, AMPL_DBFS - 20.0);
    tone(1, IF_HZ, AMPL_DBFS);
    capture_levels c;
    capture_levels_reset(&c);
    capture_levels_add(&c, frames, FRAMES);

    capture_channel_level r, m;
    capture_levels_get(&c, 0, &r);
    capture_levels_get(&c, 1, &m);
    TEST_ASSERT_DOUBLE_WITHIN(TOL_RATIO, AUDIO_RATIO, r.hf_ratio);
    TEST_ASSERT_DOUBLE_WITHIN(TOL_RATIO, IF_RATIO, m.hf_ratio);
}

void test_silence_reads_minus_infinity_and_no_ratio(void)
{
    tone(0, IF_HZ, AMPL_DBFS);
    capture_levels c;
    capture_levels_reset(&c);
    capture_levels_add(&c, frames, FRAMES);

    capture_channel_level m;
    capture_levels_get(&c, 1, &m);
    TEST_ASSERT_TRUE(isinf(m.rms_dbfs) && m.rms_dbfs < 0);
    TEST_ASSERT_TRUE(isinf(m.peak_dbfs) && m.peak_dbfs < 0);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, m.hf_ratio);

    capture_levels empty;
    capture_levels_reset(&empty);
    capture_levels_get(&empty, 0, &m);
    TEST_ASSERT_TRUE(isinf(m.rms_dbfs) && m.rms_dbfs < 0);
}

/* The capture thread adds one period at a time: the sum of the pieces is
 * the whole, the difference across a piece boundary included. */
void test_pieces_add_up_to_the_whole(void)
{
    tone(0, IF_HZ, AMPL_DBFS);
    tone(1, AUDIO_HZ, AMPL_DBFS);
    capture_levels whole, split;
    capture_levels_reset(&whole);
    capture_levels_reset(&split);
    capture_levels_add(&whole, frames, FRAMES);
    for (int i = 0; i < FRAMES; i += PIECE)
        capture_levels_add(&split, frames + (size_t) i * CAPTURE_LEVELS_CHANNELS, PIECE);

    for (int ch = 0; ch < CAPTURE_LEVELS_CHANNELS; ch++)
    {
        capture_channel_level a, b;
        capture_levels_get(&whole, ch, &a);
        capture_levels_get(&split, ch, &b);
        TEST_ASSERT_EQUAL_DOUBLE(a.rms_dbfs, b.rms_dbfs);
        TEST_ASSERT_EQUAL_DOUBLE(a.peak_dbfs, b.peak_dbfs);
        TEST_ASSERT_EQUAL_DOUBLE(a.hf_ratio, b.hf_ratio);
    }
}

/* The most negative sample is full scale, not an overflow. */
void test_most_negative_sample_peaks_at_full_scale(void)
{
    frames[0] = INT32_MIN;
    capture_levels c;
    capture_levels_reset(&c);
    capture_levels_add(&c, frames, FRAMES);
    capture_channel_level r;
    capture_levels_get(&c, 0, &r);
    TEST_ASSERT_DOUBLE_WITHIN(TOL_DB, 0.0, r.peak_dbfs);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_if_and_mic_tones_read_their_level_and_ratio);
    RUN_TEST(test_swapped_channels_read_swapped);
    RUN_TEST(test_silence_reads_minus_infinity_and_no_ratio);
    RUN_TEST(test_pieces_add_up_to_the_whole);
    RUN_TEST(test_most_negative_sample_peaks_at_full_scale);
    return UNITY_END();
}
