/* hermes-radio-daemon - radio backend abstraction
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RADIO_BACKEND_H_
#define RADIO_BACKEND_H_

#include <stdbool.h>
#include <stdint.h>

#include "radio.h"

typedef struct {
    const char *cfg_radio_path;
    const char *cfg_user_path;
    bool cpu_arg_provided;
    int cpu_nr;
} radio_daemon_runtime;

typedef struct radio_backend_ops {
    const char *name;
    bool launches_embedded;
    int (*launch)(const radio_daemon_runtime *runtime, const char *controller_path);
    bool (*init)(radio *radio_h);
    void (*shutdown)(radio *radio_h);
    void *(*io_thread)(void *radio_h_v);
    void (*set_frequency)(radio *radio_h, uint32_t frequency, uint32_t profile);
    void (*set_mode)(radio *radio_h, uint16_t mode, uint32_t profile);
    void (*set_txrx_state)(radio *radio_h, bool txrx_state);
    void (*set_bfo)(radio *radio_h, uint32_t frequency);
    void (*set_reflected_threshold)(radio *radio_h, uint32_t ref_threshold);
    void (*set_speaker_volume)(radio *radio_h, uint32_t speaker_level, uint32_t profile);
    void (*set_serial)(radio *radio_h, uint32_t serial);
    void (*set_profile_timeout)(radio *radio_h, int32_t timeout);
    void (*set_power_level)(radio *radio_h, uint16_t power_level, uint32_t profile);
    void (*set_digital_voice)(radio *radio_h, bool digital_voice, uint32_t profile);
    void (*set_step_size)(radio *radio_h, uint32_t step_size);
    void (*set_tone_generation)(radio *radio_h, bool tone_generation);
    void (*set_profile)(radio *radio_h, uint32_t profile);
    uint32_t (*get_fwd_power)(radio *radio_h);
    uint32_t (*get_swr)(radio *radio_h);
} radio_backend_ops;

typedef struct {
    radio_backend_kind kind;
    const radio_backend_ops *ops;
    char controller_path[BACKEND_PATH_MAX];
} radio_backend_selection;

bool radio_backend_detect(const char *cfg_radio_path, radio_backend_selection *selection);
void radio_backend_configure(radio *radio_h, const radio_backend_selection *selection);
int radio_backend_run(const radio_backend_selection *selection,
                      const radio_daemon_runtime *runtime);

bool radio_backend_init(radio *radio_h);
void radio_backend_shutdown(radio *radio_h);
void *radio_backend_io_thread(void *radio_h_v);

void radio_backend_set_frequency(radio *radio_h, uint32_t frequency, uint32_t profile);
void radio_backend_set_mode(radio *radio_h, uint16_t mode, uint32_t profile);
void radio_backend_set_txrx_state(radio *radio_h, bool txrx_state);
void radio_backend_set_bfo(radio *radio_h, uint32_t frequency);
void radio_backend_set_reflected_threshold(radio *radio_h, uint32_t ref_threshold);
void radio_backend_set_speaker_volume(radio *radio_h, uint32_t speaker_level, uint32_t profile);
void radio_backend_set_serial(radio *radio_h, uint32_t serial);
void radio_backend_set_profile_timeout(radio *radio_h, int32_t timeout);
void radio_backend_set_power_level(radio *radio_h, uint16_t power_level, uint32_t profile);
void radio_backend_set_digital_voice(radio *radio_h, bool digital_voice, uint32_t profile);
void radio_backend_set_step_size(radio *radio_h, uint32_t step_size);
void radio_backend_set_tone_generation(radio *radio_h, bool tone_generation);
void radio_backend_set_profile(radio *radio_h, uint32_t profile);
uint32_t radio_backend_get_fwd_power(radio *radio_h);
uint32_t radio_backend_get_swr(radio *radio_h);
void radio_backend_reset_timeout_timer(void);

#endif /* RADIO_BACKEND_H_ */
