/* hermes-radio-daemon - radio backend abstraction
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "cfg_utils.h"
#include "legacy_sbitx_bootstrap.h"
#include "radio_backend.h"
#include "radio_daemon_core.h"
#include "radio_hamlib.h"

static int launch_hfsignals_backend(const radio_daemon_runtime *runtime,
                                    const char *controller_path)
{
    return legacy_sbitx_bootstrap(runtime->cfg_radio_path,
                                  runtime->cfg_user_path,
                                  controller_path,
                                  runtime->cpu_arg_provided,
                                  runtime->cpu_nr);
}

static const radio_backend_ops hamlib_backend_ops = {
    .name = "hamlib",
    .launches_embedded = false,
    .launch = NULL,
    .init = radio_hamlib_init,
    .shutdown = radio_hamlib_shutdown,
    .io_thread = radio_io_thread,
    .set_frequency = set_frequency,
    .set_mode = set_mode,
    .set_txrx_state = tr_switch,
    .set_bfo = set_bfo,
    .set_reflected_threshold = set_reflected_threshold,
    .set_speaker_volume = set_speaker_volume,
    .set_serial = set_serial,
    .set_profile_timeout = set_profile_timeout,
    .set_power_level = set_power_knob,
    .set_digital_voice = set_digital_voice,
    .set_step_size = set_step_size,
    .set_tone_generation = set_tone_generation,
    .set_profile = set_profile,
    .get_fwd_power = get_fwd_power,
    .get_swr = get_swr,
};

static const radio_backend_ops hfsignals_backend_ops = {
    .name = "hfsignals",
    .launches_embedded = true,
    .launch = launch_hfsignals_backend,
};

static const radio_backend_ops *radio_backend_ops_for_kind(radio_backend_kind kind)
{
    switch (kind)
    {
    case RADIO_BACKEND_HFSIGNALS:
        return &hfsignals_backend_ops;
    case RADIO_BACKEND_HAMLIB:
    default:
        return &hamlib_backend_ops;
    }
}

static const radio_backend_ops *radio_backend_ops_from_radio(const radio *radio_h)
{
    if (!radio_h || !radio_h->backend_ops)
        return NULL;

    return radio_h->backend_ops;
}

bool radio_backend_detect(const char *cfg_radio_path, radio_backend_selection *selection)
{
    if (!selection)
        return false;

    selection->kind = RADIO_BACKEND_HAMLIB;
    snprintf(selection->controller_path, sizeof(selection->controller_path),
             "%s", "sbitx_controller");
    cfg_detect_backend(cfg_radio_path, &selection->kind,
                       selection->controller_path,
                       sizeof(selection->controller_path));
    selection->ops = radio_backend_ops_for_kind(selection->kind);
    return selection->ops != NULL;
}

void radio_backend_configure(radio *radio_h, const radio_backend_selection *selection)
{
    if (!radio_h || !selection)
        return;

    radio_h->backend_kind = selection->kind;
    radio_h->backend_ops = selection->ops;
    snprintf(radio_h->hfsignals_controller_path,
             sizeof(radio_h->hfsignals_controller_path),
             "%s",
             selection->controller_path);
}

int radio_backend_run(const radio_backend_selection *selection,
                      const radio_daemon_runtime *runtime)
{
    if (!selection || !selection->ops || !runtime)
        return -1;

    if (selection->ops->launches_embedded)
        return selection->ops->launch(runtime, selection->controller_path);

    return radio_daemon_core_run(selection, runtime);
}

bool radio_backend_init(radio *radio_h)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    return ops && ops->init ? ops->init(radio_h) : false;
}

void radio_backend_shutdown(radio *radio_h)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->shutdown)
        ops->shutdown(radio_h);
}

void *radio_backend_io_thread(void *radio_h_v)
{
    radio *radio_h = (radio *) radio_h_v;
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);

    if (!ops || !ops->io_thread)
        return NULL;

    return ops->io_thread(radio_h_v);
}

void radio_backend_set_frequency(radio *radio_h, uint32_t frequency, uint32_t profile)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->set_frequency)
        ops->set_frequency(radio_h, frequency, profile);
}

void radio_backend_set_mode(radio *radio_h, uint16_t mode, uint32_t profile)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->set_mode)
        ops->set_mode(radio_h, mode, profile);
}

void radio_backend_set_txrx_state(radio *radio_h, bool txrx_state)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->set_txrx_state)
        ops->set_txrx_state(radio_h, txrx_state);
}

void radio_backend_set_bfo(radio *radio_h, uint32_t frequency)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->set_bfo)
        ops->set_bfo(radio_h, frequency);
}

void radio_backend_set_reflected_threshold(radio *radio_h, uint32_t ref_threshold)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->set_reflected_threshold)
        ops->set_reflected_threshold(radio_h, ref_threshold);
}

void radio_backend_set_speaker_volume(radio *radio_h, uint32_t speaker_level, uint32_t profile)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->set_speaker_volume)
        ops->set_speaker_volume(radio_h, speaker_level, profile);
}

void radio_backend_set_serial(radio *radio_h, uint32_t serial)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->set_serial)
        ops->set_serial(radio_h, serial);
}

void radio_backend_set_profile_timeout(radio *radio_h, int32_t timeout)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->set_profile_timeout)
        ops->set_profile_timeout(radio_h, timeout);
}

void radio_backend_set_power_level(radio *radio_h, uint16_t power_level, uint32_t profile)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->set_power_level)
        ops->set_power_level(radio_h, power_level, profile);
}

void radio_backend_set_digital_voice(radio *radio_h, bool digital_voice, uint32_t profile)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->set_digital_voice)
        ops->set_digital_voice(radio_h, digital_voice, profile);
}

void radio_backend_set_step_size(radio *radio_h, uint32_t step_size)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->set_step_size)
        ops->set_step_size(radio_h, step_size);
}

void radio_backend_set_tone_generation(radio *radio_h, bool tone_generation)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->set_tone_generation)
        ops->set_tone_generation(radio_h, tone_generation);
}

void radio_backend_set_profile(radio *radio_h, uint32_t profile)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->set_profile)
        ops->set_profile(radio_h, profile);
}

uint32_t radio_backend_get_fwd_power(radio *radio_h)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->get_fwd_power)
        return ops->get_fwd_power(radio_h);

    return radio_h ? radio_h->fwd_power : 0;
}

uint32_t radio_backend_get_swr(radio *radio_h)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->get_swr)
        return ops->get_swr(radio_h);

    return 10;
}

void radio_backend_reset_timeout_timer(void)
{
    timer_reset = true;
}
