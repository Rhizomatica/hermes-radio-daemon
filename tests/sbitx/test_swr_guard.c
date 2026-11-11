/* The SWR protection's decision (sbitx/swr_guard.h): a bad antenna trips
 * it for a steady carrier and for a signal with gaps alike, a good one
 * never does, and readings in the noise change nothing. */
#include <stdbool.h>
#include <stdint.h>

#include "unity.h"
#include "swr_guard.h"

enum { LIMIT_X10 = 18, TRIP_AFTER = 10 };

/* bridge readings, raw counts */
enum { FWD = 600, REF_OPEN = 580, REF_MATCHED = 30, QUIET = 5 };

void setUp(void) {}
void tearDown(void) {}

static void test_swr_values(void)
{
    TEST_ASSERT_EQUAL_UINT32(10, swr_guard_swr_x10(0, 0));
    TEST_ASSERT_EQUAL_UINT32(10, swr_guard_swr_x10(FWD, 0));
    TEST_ASSERT_EQUAL_UINT32(30, swr_guard_swr_x10(600, 300));
    TEST_ASSERT_EQUAL_UINT32(SWR_GUARD_MAX_X10, swr_guard_swr_x10(FWD, REF_OPEN));
    TEST_ASSERT_EQUAL_UINT32(SWR_GUARD_MAX_X10, swr_guard_swr_x10(FWD, FWD));
    TEST_ASSERT_EQUAL_UINT32(SWR_GUARD_MAX_X10, swr_guard_swr_x10(FWD, FWD + 50));
}

static void test_steady_carrier_into_open_trips(void)
{
    uint16_t count = 0;
    for (int i = 0; i < TRIP_AFTER; i++)
        TEST_ASSERT_FALSE(swr_guard_step(&count, FWD, REF_OPEN, LIMIT_X10, TRIP_AFTER));
    TEST_ASSERT_TRUE(swr_guard_step(&count, FWD, REF_OPEN, LIMIT_X10, TRIP_AFTER));
}

/* What the old count missed: two bad readings, then a gap, over and over. */
static void test_signal_with_gaps_into_open_trips(void)
{
    uint16_t count = 0;
    bool tripped = false;
    for (int i = 0; i < 40 && !tripped; i++)
    {
        bool gap = i % 3 == 2;
        tripped = swr_guard_step(&count, gap ? QUIET : FWD, gap ? QUIET : REF_OPEN,
                                 LIMIT_X10, TRIP_AFTER);
    }
    TEST_ASSERT_TRUE(tripped);
}

static void test_matched_antenna_never_trips(void)
{
    uint16_t count = 0;
    for (int i = 0; i < 1000; i++)
    {
        bool gap = i % 3 == 2;
        TEST_ASSERT_FALSE(swr_guard_step(&count, gap ? QUIET : FWD, gap ? QUIET : REF_MATCHED,
                                         LIMIT_X10, TRIP_AFTER));
    }
    TEST_ASSERT_EQUAL_UINT16(0, count);
}

/* A few bad readings among good ones (a peak) are forgotten. */
static void test_a_peak_is_forgotten(void)
{
    uint16_t count = 0;
    for (int i = 0; i < 1000; i++)
    {
        bool peak = i % 10 < 3;
        TEST_ASSERT_FALSE(swr_guard_step(&count, FWD, peak ? REF_OPEN : REF_MATCHED,
                                         LIMIT_X10, TRIP_AFTER));
    }
}

static void test_noise_changes_nothing(void)
{
    uint16_t count = 4;
    for (int i = 0; i < 100; i++)
        TEST_ASSERT_FALSE(swr_guard_step(&count, QUIET, SWR_GUARD_FLOOR - 1, LIMIT_X10, TRIP_AFTER));
    TEST_ASSERT_EQUAL_UINT16(4, count);
}

/* Reflected power with next to no forward reading still counts. */
static void test_reflected_alone_counts(void)
{
    uint16_t count = 0;
    swr_guard_step(&count, QUIET, FWD, LIMIT_X10, TRIP_AFTER);
    TEST_ASSERT_EQUAL_UINT16(1, count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_swr_values);
    RUN_TEST(test_steady_carrier_into_open_trips);
    RUN_TEST(test_signal_with_gaps_into_open_trips);
    RUN_TEST(test_matched_antenna_never_trips);
    RUN_TEST(test_a_peak_is_forgotten);
    RUN_TEST(test_noise_changes_nothing);
    RUN_TEST(test_reflected_alone_counts);
    return UNITY_END();
}
