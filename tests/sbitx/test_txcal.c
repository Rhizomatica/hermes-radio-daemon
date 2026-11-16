/* sbitx_txcal: the power level is a share of the band's peak power; the
 * calibration finds the peak of a PA that rises, saturates and falls, or
 * goes flat, and is not ended by one low reading; and the shipped band table
 * loads, covers 1-35 MHz without a gap and keeps each band inside one
 * low-pass filter.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#include "unity.h"
#include "test_util.h"
#include "radio.h"
#include "radio_backend.h"
#include "sbitx/sbitx_txcal.h"

_Atomic bool shutdown_ = false;
_Atomic bool timer_reset = false;
_Atomic time_t timeout_counter = 0;

const radio_backend_ops hamlib_backend_ops = { .name = "hamlib" };
const radio_backend_ops sbitx_backend_ops = { .name = "hfsignals" };

int radio_daemon_core_run(const radio_backend_selection *selection,
                          const radio_daemon_runtime *runtime)
{
    (void) selection;
    (void) runtime;
    return 0;
}

#include "common/cfg_utils.c"
#undef LOG_C
#include "radio/radio_backend.c"

#define SHIPPED_CONFIG  "config/core.ini"
#define SHIPPED_BANDS   10
#define LOWEST_HZ       1000000
#define PEAK_W          21.0     /* about the bench sBitx's peak on 40 m */
#define TRUE_PEAK       0.30     /* the model PA's peak drive */
/* How close the found drive must be: half a fine step either side. */
#define FOUND_TOLERANCE_DB (TXCAL_FINE_DB / 2.0 + 1e-9)
/* The model PA's output past its peak: as the bench radios, about 0.7 dB
 * per 3 dB of drive. */
#define FALL_DB_PER_DB  0.23
/* The model bridge reads nothing under this. */
#define BRIDGE_FLOOR_W  0.1

/* A PA that rises as a linear amplifier, saturates into the peak at
 * TRUE_PEAK and falls past it. */
static double pa_falling(double drive)
{
    if (drive <= TRUE_PEAK)
    {
        double x = drive / TRUE_PEAK;
        double p = PEAK_W * tanh(2.0 * x * x) / tanh(2.0);
        return p < BRIDGE_FLOOR_W ? 0.0 : p;
    }
    double over_db = 20.0 * log10(drive / TRUE_PEAK);
    return PEAK_W * pow(10.0, -FALL_DB_PER_DB * over_db / 10.0);
}

/* One that saturates at TRUE_PEAK and stays there. */
static double pa_flat(double drive)
{
    return drive >= TRUE_PEAK ? PEAK_W : pa_falling(drive);
}

typedef double (*pa_model)(double drive);

/* The calibration against a model PA; glitch_at > 0 reads that step low
 * once, as a bridge refresh caught at the wrong moment might. */
static txcal_verdict run(pa_model pa, int glitch_at, txcal *c)
{
    txcal_start(c);
    for (int step = 1; step < 200; step++)
    {
        double fwd = pa(c->drive);
        if (step == glitch_at)
            fwd *= 0.5;
        txcal_verdict v = txcal_step(c, fwd, false);
        if (v != TXCAL_NEXT)
            return v;
    }
    TEST_FAIL_MESSAGE("never ended");
    return TXCAL_NO_POWER;
}

void setUp(void) {}
void tearDown(void) {}

static void test_power_is_a_share_of_the_peak(void)
{
    const double scale = 0.37;
    for (uint32_t pct = 0; pct <= 100; pct += 5)
    {
        double d = tx_power_drive(scale, pct);
        TEST_ASSERT_DOUBLE_WITHIN(1e-12, pct / 100.0, (d / scale) * (d / scale));
    }
    TEST_ASSERT_EQUAL_DOUBLE(scale, tx_power_drive(scale, 250));
}

/* The drive found gives the peak power, less TXCAL_PEAK_MARGIN_DB at most,
 * and is no higher than the peak's (a fine step's rounding allowed). */
static void assert_at_peak(const txcal *c, pa_model pa)
{
    double need = PEAK_W * pow(10.0, -TXCAL_PEAK_MARGIN_DB / 10.0);
    TEST_ASSERT_TRUE_MESSAGE(pa(c->best_drive) >= need,
                             test_msg("found %.4f, %.2f W of %.2f", c->best_drive, pa(c->best_drive), PEAK_W));
    TEST_ASSERT_TRUE_MESSAGE(c->best_drive <= TRUE_PEAK * pow(10.0, FOUND_TOLERANCE_DB / 20.0),
                             test_msg("found %.4f past the peak %.4f", c->best_drive, TRUE_PEAK));
}

static void test_finds_the_peak_of_a_pa_that_falls_past_it(void)
{
    txcal c;
    TEST_ASSERT_EQUAL_INT(TXCAL_FOUND, run(pa_falling, 0, &c));
    assert_at_peak(&c, pa_falling);
    /* and not far past it */
    TEST_ASSERT_TRUE_MESSAGE(c.drive < TRUE_PEAK * pow(10.0, (TXCAL_FLAT_DB + TXCAL_FINE_DB) / 20.0),
                             test_msg("drove to %.4f", c.drive));
}

static void test_a_flat_pa_ends_within_the_flat_span(void)
{
    txcal c;
    TEST_ASSERT_EQUAL_INT(TXCAL_FOUND, run(pa_flat, 0, &c));
    /* where it went flat, not the top of the flat span */
    assert_at_peak(&c, pa_flat);
    TEST_ASSERT_TRUE_MESSAGE(c.drive <= TRUE_PEAK * pow(10.0, (TXCAL_FLAT_DB + TXCAL_COARSE_DB) / 20.0),
                             test_msg("drove to %.4f", c.drive));
}

static void test_one_low_reading_does_not_end_it(void)
{
    txcal c;
    /* the fifth step, still well under the peak */
    TEST_ASSERT_EQUAL_INT(TXCAL_FOUND, run(pa_falling, 5, &c));
    assert_at_peak(&c, pa_falling);
}

/* The fifth step read 1 dB high, as a bridge refresh caught at the wrong
 * moment might: the climb must not stop on it. */
static void test_one_high_reading_does_not_end_it(void)
{
    txcal c;
    txcal_start(&c);
    for (int step = 1; step < 200; step++)
    {
        double fwd = pa_falling(c.drive);
        if (step == 5 && !c.confirming)
            fwd *= pow(10.0, 1.0 / 10.0);
        txcal_verdict v = txcal_step(&c, fwd, false);
        if (v == TXCAL_FOUND)
        {
            assert_at_peak(&c, pa_falling);
            return;
        }
        TEST_ASSERT_EQUAL_INT(TXCAL_NEXT, v);
    }
    TEST_FAIL_MESSAGE("never ended");
}

static double pa_dead(double drive) { (void) drive; return 0.0; }

static void test_no_power_is_no_calibration(void)
{
    txcal c;
    TEST_ASSERT_EQUAL_INT(TXCAL_NO_POWER, run(pa_dead, 0, &c));
}

static void test_the_codec_range_ends_it(void)
{
    txcal c;
    txcal_start(&c);
    TEST_ASSERT_EQUAL_INT(TXCAL_NEXT, txcal_step(&c, 1.0, false));
    double first = c.best_drive;
    TEST_ASSERT_EQUAL_INT(TXCAL_NEXT, txcal_step(&c, 2.0, true));   /* the re-read */
    TEST_ASSERT_TRUE(c.confirming);
    TEST_ASSERT_EQUAL_INT(TXCAL_FOUND, txcal_step(&c, 2.0, false));
    TEST_ASSERT_TRUE(c.best_drive > first);
}

static void test_band_lookup(void)
{
    power_settings b[2] = {
        { .f_start = 1000000, .f_stop = 2000000, .scale = 1.0 },
        { .f_start = 2000000, .f_stop = 4000000, .scale = 0.5 },
    };
    TEST_ASSERT_EQUAL_INT(-1, tx_band_index(b, 2, 999999));
    TEST_ASSERT_EQUAL_INT(0, tx_band_index(b, 2, 1000000));
    TEST_ASSERT_EQUAL_INT(1, tx_band_index(b, 2, 2000000));
    TEST_ASSERT_EQUAL_INT(-1, tx_band_index(b, 2, 4000000));
}

static void test_shipped_table_loads_and_checks(void)
{
    static radio r;
    memset(&r, 0, sizeof(r));
    pthread_mutex_init(&r.cfg_mutex, NULL);
    TEST_ASSERT_TRUE(init_config_radio(&r, SHIPPED_CONFIG));
    TEST_ASSERT_EQUAL_UINT32(SHIPPED_BANDS, r.band_power_count);
    char why[RADIO_MESSAGE_MAX];
    TEST_ASSERT_TRUE_MESSAGE(cfg_tx_bands_check(r.band_power, r.band_power_count, why, sizeof(why)), why);
    TEST_ASSERT_EQUAL_INT(LOWEST_HZ, r.band_power[0].f_start);
    TEST_ASSERT_EQUAL_INT((int) LPF_A_MAX_HZ, r.band_power[r.band_power_count - 1].f_stop);
    for (uint32_t k = 0; k < r.band_power_count; k++)
        TEST_ASSERT_FALSE(r.band_power[k].calibrated);
    close_config_radio(&r);
}

static void test_check_refuses_a_band_across_a_filter(void)
{
    power_settings b[1] = { { .f_start = 7500000, .f_stop = 12500000, .scale = 1.0 } };
    char why[RADIO_MESSAGE_MAX];
    TEST_ASSERT_FALSE(cfg_tx_bands_check(b, 1, why, sizeof(why)));
    TEST_ASSERT_NOT_NULL(strstr(why, "low-pass filter"));
}

static void test_check_refuses_a_gap(void)
{
    power_settings b[2] = {
        { .f_start = 1000000, .f_stop = 2000000, .scale = 1.0 },
        { .f_start = 3000000, .f_stop = 4000000, .scale = 1.0 },
    };
    char why[RADIO_MESSAGE_MAX];
    TEST_ASSERT_FALSE(cfg_tx_bands_check(b, 2, why, sizeof(why)));
    TEST_ASSERT_NOT_NULL(strstr(why, "gap"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_power_is_a_share_of_the_peak);
    RUN_TEST(test_finds_the_peak_of_a_pa_that_falls_past_it);
    RUN_TEST(test_a_flat_pa_ends_within_the_flat_span);
    RUN_TEST(test_one_low_reading_does_not_end_it);
    RUN_TEST(test_one_high_reading_does_not_end_it);
    RUN_TEST(test_no_power_is_no_calibration);
    RUN_TEST(test_the_codec_range_ends_it);
    RUN_TEST(test_band_lookup);
    RUN_TEST(test_shipped_table_loads_and_checks);
    RUN_TEST(test_check_refuses_a_band_across_a_filter);
    RUN_TEST(test_check_refuses_a_gap);
    return UNITY_END();
}
