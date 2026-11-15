/* band_clock: the station clock's error from the FT8/JS8 decodes' offsets.
 * Stations on the slots with a little scatter give the error; too few of
 * them give nothing; HERMES traffic off the slots, spread over the period,
 * gives nothing on its own and does not move the estimate when slot
 * stations are among it; one loud station off by seconds is outvoted; the
 * estimate follows a corrected clock once the old decodes leave the
 * window, and is dropped when the band has been quiet too long.
 */
#include <stdlib.h>

#include "unity.h"
#include "test_util.h"
#include "dsp/band_clock.h"

#define T0           1.7e9   /* Unix seconds: any time will do */
#define FT8_SLOT_S   15.0
#define CLOCK_ERR_S  1.8     /* the error the slot stations show */
#define SCATTER_S    0.15    /* +- their own sound cards and decoders */
#define SLOT_DECODES 6       /* decodes a slot on a quiet band */

void setUp(void) { srand(1); band_clock_reset(); }
void tearDown(void) {}

static double scatter(double s)
{
    return s * (2.0 * rand() / RAND_MAX - 1.0);
}

/* n decodes from slot stations in the slot starting at t. */
static void slot_stations(double t, int n, double err)
{
    for (int i = 0; i < n; i++)
        band_clock_add(err + scatter(SCATTER_S), t + i * 0.01);
}

/* n decodes from HERMES stations, off the slots: anywhere in the period. */
static void off_slot_stations(double t, int n)
{
    for (int i = 0; i < n; i++)
        band_clock_add(FT8_SLOT_S * (1.0 * rand() / RAND_MAX - 0.5), t + i * 0.01);
}

static void test_slot_stations_give_the_error(void)
{
    for (int s = 0; s < 4; s++)
        slot_stations(T0 + s * FT8_SLOT_S, SLOT_DECODES, CLOCK_ERR_S);
    double e = 0;
    int n = 0;
    TEST_ASSERT_TRUE(band_clock_estimate(T0 + 4 * FT8_SLOT_S, &e, &n));
    TEST_ASSERT_DOUBLE_WITHIN(SCATTER_S, CLOCK_ERR_S, e);
    TEST_ASSERT_EQUAL_INT(4 * SLOT_DECODES, n);
}

static void test_too_few_give_nothing(void)
{
    slot_stations(T0, BAND_CLOCK_MIN_DECODES - 1, CLOCK_ERR_S);
    double e;
    int n;
    TEST_ASSERT_FALSE(band_clock_estimate(T0 + FT8_SLOT_S, &e, &n));
}

static void test_off_slot_traffic_alone_gives_nothing(void)
{
    for (int s = 0; s < 8; s++)
        off_slot_stations(T0 + s * FT8_SLOT_S, SLOT_DECODES);
    double e;
    int n;
    TEST_ASSERT_FALSE_MESSAGE(band_clock_estimate(T0 + 8 * FT8_SLOT_S, &e, &n),
                              test_msg("estimated %+.2f s from %d off-slot decodes", e, n));
}

static void test_off_slot_traffic_does_not_move_it(void)
{
    for (int s = 0; s < 8; s++) {
        slot_stations(T0 + s * FT8_SLOT_S, SLOT_DECODES / 2, CLOCK_ERR_S);
        off_slot_stations(T0 + s * FT8_SLOT_S + 1.0, SLOT_DECODES / 2);
    }
    double e = 0;
    int n = 0;
    TEST_ASSERT_TRUE(band_clock_estimate(T0 + 8 * FT8_SLOT_S, &e, &n));
    TEST_ASSERT_DOUBLE_WITHIN(SCATTER_S, CLOCK_ERR_S, e);
}

static void test_one_station_off_is_outvoted(void)
{
    for (int s = 0; s < 4; s++) {
        slot_stations(T0 + s * FT8_SLOT_S, SLOT_DECODES, 0.0);
        band_clock_add(-3.0, T0 + s * FT8_SLOT_S + 1.0);   /* its own clock is off */
    }
    double e = 1;
    int n = 0;
    TEST_ASSERT_TRUE(band_clock_estimate(T0 + 4 * FT8_SLOT_S, &e, &n));
    TEST_ASSERT_DOUBLE_WITHIN(SCATTER_S, 0.0, e);
}

static void test_follows_a_corrected_clock(void)
{
    for (int s = 0; s < 8; s++)
        slot_stations(T0 + s * FT8_SLOT_S, SLOT_DECODES, CLOCK_ERR_S);
    double later = T0 + 8 * FT8_SLOT_S + BAND_CLOCK_WINDOW_S;   /* the clock was set */
    for (int s = 0; s < 4; s++)
        slot_stations(later + s * FT8_SLOT_S, SLOT_DECODES, 0.0);
    double e = 1;
    int n = 0;
    TEST_ASSERT_TRUE(band_clock_estimate(later + 4 * FT8_SLOT_S, &e, &n));
    TEST_ASSERT_DOUBLE_WITHIN(SCATTER_S, 0.0, e);
}

static void test_dropped_after_a_quiet_band(void)
{
    for (int s = 0; s < 4; s++)
        slot_stations(T0 + s * FT8_SLOT_S, SLOT_DECODES, CLOCK_ERR_S);
    double e;
    int n;
    double last = T0 + 3 * FT8_SLOT_S;
    TEST_ASSERT_TRUE(band_clock_estimate(last + BAND_CLOCK_KEEP_S - 1.0, &e, &n));
    TEST_ASSERT_FALSE(band_clock_estimate(last + BAND_CLOCK_KEEP_S + 1.0, &e, &n));
}

static void test_ignores_what_is_not_a_number(void)
{
    band_clock_add(0.0 / 0.0, T0);
    band_clock_add(1.0 / 0.0, T0);
    slot_stations(T0, BAND_CLOCK_MIN_DECODES, CLOCK_ERR_S);
    double e = 0;
    int n = 0;
    TEST_ASSERT_TRUE(band_clock_estimate(T0 + 1.0, &e, &n));
    TEST_ASSERT_EQUAL_INT(BAND_CLOCK_MIN_DECODES, n);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_slot_stations_give_the_error);
    RUN_TEST(test_too_few_give_nothing);
    RUN_TEST(test_off_slot_traffic_alone_gives_nothing);
    RUN_TEST(test_off_slot_traffic_does_not_move_it);
    RUN_TEST(test_one_station_off_is_outvoted);
    RUN_TEST(test_follows_a_corrected_clock);
    RUN_TEST(test_dropped_after_a_quiet_band);
    RUN_TEST(test_ignores_what_is_not_a_number);
    return UNITY_END();
}
