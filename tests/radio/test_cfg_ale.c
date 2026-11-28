/* ale:combine, the K values of the ALE combining receiver (cfg_parse_ale_combine). */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#include "unity.h"
#include "radio.h"
#include "radio_backend.h"

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
#include "radio/radio_backend.c"

void setUp(void) {}
void tearDown(void) {}

static void test_default_list(void)
{
    uint8_t k[ALE_COMBINE_MAX];
    TEST_ASSERT_EQUAL_size_t(3, cfg_parse_ale_combine(ALE_COMBINE_DEFAULT, k, ALE_COMBINE_MAX));
    TEST_ASSERT_EQUAL_UINT8(4, k[0]);
    TEST_ASSERT_EQUAL_UINT8(8, k[1]);
    TEST_ASSERT_EQUAL_UINT8(16, k[2]);
}

static void test_bad_values_and_repeats_are_skipped(void)
{
    uint8_t k[ALE_COMBINE_MAX];
    TEST_ASSERT_EQUAL_size_t(2, cfg_parse_ale_combine("1, 4, x, 17, 4, 8", k, ALE_COMBINE_MAX));
    TEST_ASSERT_EQUAL_UINT8(4, k[0]);
    TEST_ASSERT_EQUAL_UINT8(8, k[1]);
}

static void test_at_most_max_values(void)
{
    uint8_t k[ALE_COMBINE_MAX];
    TEST_ASSERT_EQUAL_size_t(ALE_COMBINE_MAX, cfg_parse_ale_combine("2,3,4,5,6,7", k, ALE_COMBINE_MAX));
    TEST_ASSERT_EQUAL_UINT8(5, k[ALE_COMBINE_MAX - 1]);
}

static void test_empty_list(void)
{
    uint8_t k[ALE_COMBINE_MAX];
    TEST_ASSERT_EQUAL_size_t(0, cfg_parse_ale_combine("", k, ALE_COMBINE_MAX));
    TEST_ASSERT_EQUAL_size_t(0, cfg_parse_ale_combine(NULL, k, ALE_COMBINE_MAX));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_list);
    RUN_TEST(test_bad_values_and_repeats_are_skipped);
    RUN_TEST(test_at_most_max_values);
    RUN_TEST(test_empty_list);
    return UNITY_END();
}
