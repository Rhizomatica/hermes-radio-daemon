/* main:capture_channel: one channel of a stereo rig codec kept as mono. */
#include <stdint.h>
#include <stdio.h>
#include "unity.h"
#include "radio/radio_media.h"

void setUp(void) {}
void tearDown(void) {}

enum { FRAMES = 4 };

static void test_left_and_right(void)
{
    /* left 1..4, right -1..-4, interleaved */
    int16_t l[FRAMES * MEDIA_STEREO] = { 1, -1, 2, -2, 3, -3, 4, -4 };
    int16_t r[FRAMES * MEDIA_STEREO] = { 1, -1, 2, -2, 3, -3, 4, -4 };
    const int16_t want_l[FRAMES] = { 1, 2, 3, 4 }, want_r[FRAMES] = { -1, -2, -3, -4 };

    media_pick_channel(l, FRAMES, 0);
    media_pick_channel(r, FRAMES, 1);
    TEST_ASSERT_EQUAL_INT16_ARRAY(want_l, l, FRAMES);
    TEST_ASSERT_EQUAL_INT16_ARRAY(want_r, r, FRAMES);
    puts("  capture channel: left and right ...... ok");
}

/* The case it is for: the radio on the left, the other input carrying the
 * same signal inverted, which averaging (the mono open) cancelled. */
static void test_not_cancelled(void)
{
    int16_t b[FRAMES * MEDIA_STEREO] = { 1000, -1000, -500, 500, 250, -250, 0, 0 };
    media_pick_channel(b, FRAMES, 0);
    TEST_ASSERT_EQUAL_INT16(1000, b[0]);
    TEST_ASSERT_EQUAL_INT16(-500, b[1]);
    TEST_ASSERT_EQUAL_INT16(250, b[2]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_left_and_right);
    RUN_TEST(test_not_cancelled);
    return UNITY_END();
}
