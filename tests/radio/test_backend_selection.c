#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

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

#include "../common/cfg_utils.c"
#include "../radio/radio_backend.c"

static void test_backend_detect_defaults(void)
{
    radio_backend_selection selection;

    memset(&selection, 0, sizeof(selection));
    assert(radio_backend_detect("tests/fixtures/backend-default.ini", &selection));
    assert(selection.kind == RADIO_BACKEND_HAMLIB);
    assert(selection.ops != NULL);
    assert(strcmp(selection.ops->name, "hamlib") == 0);
}

static void test_backend_detect_aliases(void)
{
    radio_backend_selection selection;

    memset(&selection, 0, sizeof(selection));
    assert(cfg_backend_kind_from_string("sbitx") == RADIO_BACKEND_HFSIGNALS);
    assert(cfg_backend_kind_from_string("zbitx") == RADIO_BACKEND_HFSIGNALS);
    assert(cfg_backend_kind_from_string("hamlib") == RADIO_BACKEND_HAMLIB);

    assert(radio_backend_detect("tests/fixtures/backend-zbitx.ini", &selection));
    assert(selection.kind == RADIO_BACKEND_HFSIGNALS);
    assert(selection.ops != NULL);
    assert(strcmp(selection.ops->name, "hfsignals") == 0);
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
    assert(radio_h.backend_kind == RADIO_BACKEND_HFSIGNALS);
    assert(radio_h.backend_ops == hfsignals.ops);

    /* Both backends now route through radio_daemon_core_run; there is no
     * separate launch path for hfsignals. */
    daemon_core_calls = 0;
    assert(radio_backend_run(&hamlib, &runtime) == 22);
    assert(daemon_core_calls == 1);
    assert(last_daemon_core_call.selection.kind == RADIO_BACKEND_HAMLIB);
    assert(last_daemon_core_call.runtime.cpu_nr == 3);

    assert(radio_backend_run(&hfsignals, &runtime) == 22);
    assert(daemon_core_calls == 2);
    assert(last_daemon_core_call.selection.kind == RADIO_BACKEND_HFSIGNALS);
}

static void test_timeout_reset_passthrough(void)
{
    timer_reset = false;
    radio_backend_reset_timeout_timer();
    assert(timer_reset);
}

int main(void)
{
    test_backend_detect_defaults();
    test_backend_detect_aliases();
    test_backend_configure_and_run_dispatch();
    test_timeout_reset_passthrough();
    puts("backend_selection_test: ok");
    return 0;
}
