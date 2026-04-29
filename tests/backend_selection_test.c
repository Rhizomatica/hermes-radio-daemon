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

static int bootstrap_calls;
static int daemon_core_calls;
static struct {
    char cfg_radio_path[CONFIG_PATH_MAX];
    char cfg_user_path[CONFIG_PATH_MAX];
    char controller_path[BACKEND_PATH_MAX];
    bool cpu_arg_provided;
    int cpu_nr;
} last_bootstrap_call;
static struct {
    radio_backend_selection selection;
    radio_daemon_runtime runtime;
} last_daemon_core_call;

bool radio_hamlib_init(radio *radio_h)
{
    (void) radio_h;
    return true;
}

void radio_hamlib_shutdown(radio *radio_h)
{
    (void) radio_h;
}

void *radio_io_thread(void *radio_h_v)
{
    (void) radio_h_v;
    return NULL;
}

void set_frequency(radio *radio_h, uint32_t frequency, uint32_t profile)
{
    radio_h->profiles[profile].freq = frequency;
}

void set_mode(radio *radio_h, uint16_t mode, uint32_t profile)
{
    radio_h->profiles[profile].mode = mode;
}

void tr_switch(radio *radio_h, bool txrx_state)
{
    radio_h->txrx_state = txrx_state;
}

void set_bfo(radio *radio_h, uint32_t frequency)
{
    radio_h->bfo_frequency = frequency;
}

void set_reflected_threshold(radio *radio_h, uint32_t ref_threshold)
{
    radio_h->reflected_threshold = ref_threshold;
}

void set_speaker_volume(radio *radio_h, uint32_t speaker_level, uint32_t profile)
{
    radio_h->profiles[profile].speaker_level = speaker_level;
}

void set_serial(radio *radio_h, uint32_t serial)
{
    radio_h->serial_number = serial;
}

void set_profile_timeout(radio *radio_h, int32_t timeout)
{
    radio_h->profile_timeout = timeout;
}

void set_power_knob(radio *radio_h, uint16_t power_level, uint32_t profile)
{
    radio_h->profiles[profile].power_level_percentage = power_level;
}

void set_digital_voice(radio *radio_h, bool digital_voice, uint32_t profile)
{
    radio_h->profiles[profile].digital_voice = digital_voice;
}

void set_step_size(radio *radio_h, uint32_t step_size)
{
    radio_h->step_size = step_size;
}

void set_tone_generation(radio *radio_h, bool tone_generation)
{
    radio_h->tone_generation = tone_generation;
}

void set_profile(radio *radio_h, uint32_t profile)
{
    radio_h->profile_active_idx = profile;
}

uint32_t get_fwd_power(radio *radio_h)
{
    return radio_h->fwd_power;
}

uint32_t get_swr(radio *radio_h)
{
    return radio_h->ref_power;
}

int legacy_sbitx_bootstrap(const char *cfg_radio_path,
                           const char *cfg_user_path,
                           const char *controller_path,
                           bool cpu_arg_provided,
                           int cpu_nr)
{
    bootstrap_calls++;
    snprintf(last_bootstrap_call.cfg_radio_path, sizeof(last_bootstrap_call.cfg_radio_path),
             "%s", cfg_radio_path);
    snprintf(last_bootstrap_call.cfg_user_path, sizeof(last_bootstrap_call.cfg_user_path),
             "%s", cfg_user_path);
    snprintf(last_bootstrap_call.controller_path, sizeof(last_bootstrap_call.controller_path),
             "%s", controller_path);
    last_bootstrap_call.cpu_arg_provided = cpu_arg_provided;
    last_bootstrap_call.cpu_nr = cpu_nr;
    return 11;
}

int radio_daemon_core_run(const radio_backend_selection *selection,
                          const radio_daemon_runtime *runtime)
{
    daemon_core_calls++;
    last_daemon_core_call.selection = *selection;
    last_daemon_core_call.runtime = *runtime;
    return 22;
}

#include "../cfg_utils.c"
#include "../radio_backend.c"

static void test_backend_detect_defaults(void)
{
    radio_backend_selection selection;

    memset(&selection, 0, sizeof(selection));
    assert(radio_backend_detect("tests/fixtures/backend-default.ini", &selection));
    assert(selection.kind == RADIO_BACKEND_HAMLIB);
    assert(selection.ops != NULL);
    assert(strcmp(selection.ops->name, "hamlib") == 0);
    assert(strcmp(selection.controller_path, "sbitx_controller") == 0);
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
    assert(strcmp(selection.controller_path, "custom_legacy_controller") == 0);
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
        .cpu_arg_provided = true,
        .cpu_nr = 3,
    };
    radio radio_h;

    snprintf(hamlib.controller_path, sizeof(hamlib.controller_path), "%s", "rigctl");
    snprintf(hfsignals.controller_path, sizeof(hfsignals.controller_path), "%s",
             "embedded-controller");

    memset(&radio_h, 0, sizeof(radio_h));
    radio_backend_configure(&radio_h, &hfsignals);
    assert(radio_h.backend_kind == RADIO_BACKEND_HFSIGNALS);
    assert(radio_h.backend_ops == hfsignals.ops);
    assert(strcmp(radio_h.hfsignals_controller_path, "embedded-controller") == 0);

    bootstrap_calls = 0;
    daemon_core_calls = 0;
    assert(radio_backend_run(&hamlib, &runtime) == 22);
    assert(daemon_core_calls == 1);
    assert(bootstrap_calls == 0);
    assert(last_daemon_core_call.selection.kind == RADIO_BACKEND_HAMLIB);
    assert(last_daemon_core_call.runtime.cpu_nr == 3);

    assert(radio_backend_run(&hfsignals, &runtime) == 11);
    assert(bootstrap_calls == 1);
    assert(strcmp(last_bootstrap_call.cfg_radio_path, "radio.ini") == 0);
    assert(strcmp(last_bootstrap_call.cfg_user_path, "user.ini") == 0);
    assert(strcmp(last_bootstrap_call.controller_path, "embedded-controller") == 0);
    assert(last_bootstrap_call.cpu_arg_provided);
    assert(last_bootstrap_call.cpu_nr == 3);
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
