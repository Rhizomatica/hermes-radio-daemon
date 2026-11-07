/* dstar_clock_cal: the codec's clock error, measured from the overs heard.
 * Readings like the bench's (+300 ppm, a few ppm of scatter) move a
 * correction held at 0 to +300 once there are enough; a correction already
 * close is left alone; readings scattered like a weak signal's move
 * nothing; readings taken under a correction add to it; and nothing moves
 * twice on the same readings.
 */
#include <stdlib.h>

#include "unity.h"
#include "dsp/dstar_clock_cal.h"

#define TRUE_PPM     300.0
#define BENCH_SCATTER  7.0     /* +-ppm, a 2 s window on the bench */
#define WEAK_SCATTER 400.0     /* +-ppm, a weak signal on the air */

static dstar_clock_cal c;

static double scatter(double s)
{
    return s * (2.0 * rand() / RAND_MAX - 1.0);
}

void setUp(void) { srand(1); dstar_clock_cal_reset(&c); }
void tearDown(void) {}

static void test_bench_readings_calibrate(void)
{
    long out = 0;
    double m, sp;
    for (int i = 0; i < DSTAR_CAL_MIN - 1; i++) {
        dstar_clock_cal_push(&c, 0.0, TRUE_PPM + scatter(BENCH_SCATTER));
        TEST_ASSERT_EQUAL_INT(DSTAR_CAL_HOLD, dstar_clock_cal_decide(&c, 0.0, &out, &m, &sp));
    }
    dstar_clock_cal_push(&c, 0.0, TRUE_PPM + scatter(BENCH_SCATTER));
    TEST_ASSERT_EQUAL_INT(DSTAR_CAL_MOVE, dstar_clock_cal_decide(&c, 0.0, &out, &m, &sp));
    TEST_ASSERT_INT_WITHIN((int) BENCH_SCATTER, (int) TRUE_PPM, (int) out);
}

static void test_close_enough_is_left_alone(void)
{
    long out = 0;
    double m, sp;
    for (int i = 0; i < DSTAR_CAL_KEEP; i++)
        dstar_clock_cal_push(&c, 0.0, TRUE_PPM + scatter(BENCH_SCATTER));
    TEST_ASSERT_EQUAL_INT(DSTAR_CAL_HOLD,
                          dstar_clock_cal_decide(&c, TRUE_PPM - DSTAR_CAL_DEADBAND / 2, &out, &m, &sp));
}

static void test_weak_scatter_moves_nothing(void)
{
    long out = 0;
    double m, sp;
    for (int i = 0; i < DSTAR_CAL_KEEP; i++)
        dstar_clock_cal_push(&c, 0.0, TRUE_PPM + scatter(WEAK_SCATTER));
    TEST_ASSERT_EQUAL_INT(DSTAR_CAL_DISAGREE, dstar_clock_cal_decide(&c, 0.0, &out, &m, &sp));
}

/* Taken with +250 in force, the line shows the +50 left: the error is 300. */
static void test_readings_under_a_correction(void)
{
    long out = 0;
    double m, sp;
    for (int i = 0; i < DSTAR_CAL_MIN; i++)
        dstar_clock_cal_push(&c, 250.0, 50.0 + scatter(BENCH_SCATTER));
    TEST_ASSERT_EQUAL_INT(DSTAR_CAL_MOVE, dstar_clock_cal_decide(&c, 250.0, &out, &m, &sp));
    TEST_ASSERT_INT_WITHIN((int) BENCH_SCATTER, (int) TRUE_PPM, (int) out);
}

static void test_no_second_move_without_new_readings(void)
{
    long out = 0;
    double m, sp;
    for (int i = 0; i < DSTAR_CAL_MIN; i++)
        dstar_clock_cal_push(&c, 0.0, TRUE_PPM);
    TEST_ASSERT_EQUAL_INT(DSTAR_CAL_MOVE, dstar_clock_cal_decide(&c, 0.0, &out, &m, &sp));
    TEST_ASSERT_EQUAL_INT(DSTAR_CAL_HOLD, dstar_clock_cal_decide(&c, 0.0, &out, &m, &sp));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bench_readings_calibrate);
    RUN_TEST(test_close_enough_is_left_alone);
    RUN_TEST(test_weak_scatter_moves_nothing);
    RUN_TEST(test_readings_under_a_correction);
    RUN_TEST(test_no_second_move_without_new_readings);
    return UNITY_END();
}
