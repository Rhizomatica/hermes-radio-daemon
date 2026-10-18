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

static int daemon_core_calls;
static struct {
    radio_backend_selection selection;
    radio_daemon_runtime runtime;
} last_daemon_core_call;

/* Both backend ops vtables now live in their respective backend TUs
 * (hamlib/radio_hamlib.c and sbitx/sbitx_core.c). The test stubs them out
 * because we don't link those TUs in. */
const radio_backend_ops hamlib_backend_ops = {
    .name = "hamlib",
};
const radio_backend_ops sbitx_backend_ops = {
    .name = "hfsignals",
};

int radio_daemon_core_run(const radio_backend_selection *selection,
                          const radio_daemon_runtime *runtime)
{
    daemon_core_calls++;
    last_daemon_core_call.selection = *selection;
    last_daemon_core_call.runtime = *runtime;
    return 22;
}

#include "common/cfg_utils.c"
#include "radio/radio_backend.c"

static void test_backend_detect_defaults(void)
{
    radio_backend_selection selection;

    memset(&selection, 0, sizeof(selection));
    TEST_ASSERT_TRUE(radio_backend_detect("tests/fixtures/backend-default.ini", &selection));
    TEST_ASSERT_EQUAL_INT64(RADIO_BACKEND_HAMLIB, selection.kind);
    TEST_ASSERT_NOT_NULL(selection.ops);
    TEST_ASSERT_EQUAL_STRING("hamlib", selection.ops->name);
}

static void test_backend_detect_aliases(void)
{
    radio_backend_selection selection;

    memset(&selection, 0, sizeof(selection));
    TEST_ASSERT_EQUAL_INT64(RADIO_BACKEND_HFSIGNALS, cfg_backend_kind_from_string("sbitx"));
    TEST_ASSERT_EQUAL_INT64(RADIO_BACKEND_HFSIGNALS, cfg_backend_kind_from_string("zbitx"));
    TEST_ASSERT_EQUAL_INT64(RADIO_BACKEND_HAMLIB, cfg_backend_kind_from_string("hamlib"));

    TEST_ASSERT_TRUE(radio_backend_detect("tests/fixtures/backend-zbitx.ini", &selection));
    TEST_ASSERT_EQUAL_INT64(RADIO_BACKEND_HFSIGNALS, selection.kind);
    TEST_ASSERT_NOT_NULL(selection.ops);
    TEST_ASSERT_EQUAL_STRING("hfsignals", selection.ops->name);
}

static void test_backend_configure_and_run_dispatch(void)
{
    radio_backend_selection hamlib = {
        .kind = RADIO_BACKEND_HAMLIB,
        .ops = radio_backend_ops_for_kind(RADIO_BACKEND_HAMLIB),
    };
    radio_backend_selection hfsignals = {
        .kind = RADIO_BACKEND_HFSIGNALS,
        .ops = radio_backend_ops_for_kind(RADIO_BACKEND_HFSIGNALS),
    };
    radio_daemon_runtime runtime = {
        .cfg_radio_path = "radio.ini",
        .cfg_user_path = "user.ini",
        .cpu_nr = 3,
    };
    radio radio_h;

    memset(&radio_h, 0, sizeof(radio_h));
    radio_backend_configure(&radio_h, &hfsignals);
    TEST_ASSERT_EQUAL_INT64(RADIO_BACKEND_HFSIGNALS, radio_h.backend_kind);
    TEST_ASSERT_TRUE(radio_h.backend_ops == hfsignals.ops);

    /* Both backends now route through radio_daemon_core_run; there is no
     * separate launch path for hfsignals. */
    daemon_core_calls = 0;
    TEST_ASSERT_EQUAL_INT64(22, radio_backend_run(&hamlib, &runtime));
    TEST_ASSERT_EQUAL_INT64(1, daemon_core_calls);
    TEST_ASSERT_EQUAL_INT64(RADIO_BACKEND_HAMLIB, last_daemon_core_call.selection.kind);
    TEST_ASSERT_EQUAL_INT64(3, last_daemon_core_call.runtime.cpu_nr);

    TEST_ASSERT_EQUAL_INT64(22, radio_backend_run(&hfsignals, &runtime));
    TEST_ASSERT_EQUAL_INT64(2, daemon_core_calls);
    TEST_ASSERT_EQUAL_INT64(RADIO_BACKEND_HFSIGNALS, last_daemon_core_call.selection.kind);
}

static void test_timeout_reset_passthrough(void)
{
    timer_reset = false;
    radio_backend_reset_timeout_timer();
    TEST_ASSERT_TRUE(timer_reset);
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_backend_detect_defaults);
    RUN_TEST(test_backend_detect_aliases);
    RUN_TEST(test_backend_configure_and_run_dispatch);
    RUN_TEST(test_timeout_reset_passthrough);
    return UNITY_END();
}
