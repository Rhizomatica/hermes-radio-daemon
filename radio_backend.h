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
#include <stddef.h>

#include "radio.h"
#include "radio_controls.h"

typedef struct {
    const char *cfg_radio_path;
    const char *cfg_user_path;
    /* CPU pinning: -1 = no pin, otherwise pin to that CPU. */
    int cpu_nr;
} radio_daemon_runtime;

typedef struct radio_backend_ops {
    const char *name;
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
    uint32_t (*get_ref_power)(radio *radio_h);
    uint32_t (*get_swr)(radio *radio_h);

    /* ── generic named controls ───────────────────────────────────────
     * Named with the Hamlib token names (see radio_controls.h). The
     * hamlib backend forwards straight to rig_get_level/rig_set_level &
     * friends; the hfsignals backend maps the subset its DSP really has.
     * All return RADIO_CTRL_*. */
    int (*get_level)(radio *radio_h, const char *name, double *out);
    int (*set_level)(radio *radio_h, const char *name, double value);
    int (*get_func)(radio *radio_h, const char *name, int *out);
    int (*set_func)(radio *radio_h, const char *name, int on);
    int (*get_parm)(radio *radio_h, const char *name, double *out);
    int (*set_parm)(radio *radio_h, const char *name, double value);
    size_t (*enumerate_controls)(radio *radio_h, radio_ctrl_info *out, size_t max);

    /* ── typed rig state (the things Hamlib does not model as levels) ── */
    int (*get_vfo)(radio *radio_h, char *out, size_t out_len);
    int (*set_vfo)(radio *radio_h, const char *vfo);
    int (*get_split)(radio *radio_h, int *on, char *tx_vfo, size_t tx_vfo_len);
    int (*set_split)(radio *radio_h, int on, const char *tx_vfo);
    int (*get_split_freq)(radio *radio_h, uint32_t *hz);
    int (*set_split_freq)(radio *radio_h, uint32_t hz);
    int (*get_split_mode)(radio *radio_h, char *mode, size_t mode_len, uint32_t *width);
    int (*set_split_mode)(radio *radio_h, const char *mode, uint32_t width);
    int (*get_rit)(radio *radio_h, int32_t *hz);
    int (*set_rit)(radio *radio_h, int32_t hz);
    int (*get_xit)(radio *radio_h, int32_t *hz);
    int (*set_xit)(radio *radio_h, int32_t hz);
    /* The rig's mode as the rig itself names it, so a data submode
     * (PKTUSB/PKTLSB/DIGU) round-trips instead of being flattened onto the
     * daemon's internal MODE_*. Width in Hz, 0 = rig default. */
    int (*get_mode_name)(radio *radio_h, char *out, size_t out_len, uint32_t *width);
    int (*set_mode_name)(radio *radio_h, const char *mode, uint32_t width);
    /* Receiver filter passband in Hz (0 = rig default / "normal"). */
    int (*get_width)(radio *radio_h, uint32_t *hz);
    int (*set_width)(radio *radio_h, uint32_t hz);
    int (*get_ant)(radio *radio_h, int *ant);
    int (*set_ant)(radio *radio_h, int ant);
    int (*get_mem)(radio *radio_h, int *ch);
    int (*set_mem)(radio *radio_h, int ch);
    int (*get_powerstat)(radio *radio_h, int *on);
    int (*set_powerstat)(radio *radio_h, int on);
    /* Hamlib VFO operation token: "TUNE", "CPY", "XCHG", "TOGGLE",
     * "BAND_UP", "BAND_DOWN", "UP", "DOWN", "FROM_VFO", "TO_VFO", ... */
    int (*vfo_op)(radio *radio_h, const char *op);
    /* Rig-side CW keyer (not the daemon's software CW). */
    int (*send_morse)(radio *radio_h, const char *text);
    int (*stop_morse)(radio *radio_h);
    /* rigctld \dump_state payload describing the REAL rig, so remote
     * clients (WSJT-X, fldigi, VARA, Winlink) see true capabilities. */
    int (*dump_state)(radio *radio_h, char *out, size_t out_len);
} radio_backend_ops;

typedef struct {
    radio_backend_kind kind;
    const radio_backend_ops *ops;
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
uint32_t radio_backend_get_ref_power(radio *radio_h);
uint32_t radio_backend_get_swr(radio *radio_h);

/* Generic named controls. See radio_controls.h for the vocabulary. */
int radio_backend_get_level(radio *radio_h, const char *name, double *out);
int radio_backend_set_level(radio *radio_h, const char *name, double value);
int radio_backend_get_func(radio *radio_h, const char *name, int *out);
int radio_backend_set_func(radio *radio_h, const char *name, int on);
int radio_backend_get_parm(radio *radio_h, const char *name, double *out);
int radio_backend_set_parm(radio *radio_h, const char *name, double value);
size_t radio_backend_enumerate_controls(radio *radio_h, radio_ctrl_info *out, size_t max);

int radio_backend_get_vfo(radio *radio_h, char *out, size_t out_len);
int radio_backend_set_vfo(radio *radio_h, const char *vfo);
int radio_backend_get_split(radio *radio_h, int *on, char *tx_vfo, size_t tx_vfo_len);
int radio_backend_set_split(radio *radio_h, int on, const char *tx_vfo);
int radio_backend_get_split_freq(radio *radio_h, uint32_t *hz);
int radio_backend_set_split_freq(radio *radio_h, uint32_t hz);
int radio_backend_get_split_mode(radio *radio_h, char *mode, size_t mode_len, uint32_t *width);
int radio_backend_set_split_mode(radio *radio_h, const char *mode, uint32_t width);
int radio_backend_get_rit(radio *radio_h, int32_t *hz);
int radio_backend_set_rit(radio *radio_h, int32_t hz);
int radio_backend_get_xit(radio *radio_h, int32_t *hz);
int radio_backend_set_xit(radio *radio_h, int32_t hz);
int radio_backend_get_mode_name(radio *radio_h, char *out, size_t out_len, uint32_t *width);
int radio_backend_set_mode_name(radio *radio_h, const char *mode, uint32_t width);
int radio_backend_get_width(radio *radio_h, uint32_t *hz);
int radio_backend_set_width(radio *radio_h, uint32_t hz);
int radio_backend_get_ant(radio *radio_h, int *ant);
int radio_backend_set_ant(radio *radio_h, int ant);
int radio_backend_get_mem(radio *radio_h, int *ch);
int radio_backend_set_mem(radio *radio_h, int ch);
int radio_backend_get_powerstat(radio *radio_h, int *on);
int radio_backend_set_powerstat(radio *radio_h, int on);
int radio_backend_vfo_op(radio *radio_h, const char *op);
int radio_backend_send_morse(radio *radio_h, const char *text);
int radio_backend_stop_morse(radio *radio_h);
int radio_backend_dump_state(radio *radio_h, char *out, size_t out_len);
void radio_backend_reset_timeout_timer(void);

#endif /* RADIO_BACKEND_H_ */
