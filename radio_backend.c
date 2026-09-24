/* hermes-radio-daemon - radio backend abstraction
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "cfg_utils.h"
#include "radio_controls.h"
#include "radio_backend.h"
#include "radio_daemon_core.h"
#include "rig_server.h"
#include "cat_server.h"

/* Timer reset flag — defined by hamlib/radio_hamlib.c (used only by hamlib's
 * profile-timeout io thread; harmless when hfsignals backend is selected). */
extern _Atomic bool timer_reset;

/* Each backend TU defines its own const radio_backend_ops; we just pick one
 * here based on the radio_backend kind from cfg. No backend-implementation
 * function is exposed by name — the vtable is the only contract. */
extern const radio_backend_ops hamlib_backend_ops;
extern const radio_backend_ops sbitx_backend_ops;

static const radio_backend_ops *radio_backend_ops_for_kind(radio_backend_kind kind)
{
    switch (kind)
    {
    case RADIO_BACKEND_HFSIGNALS:
        return &sbitx_backend_ops;
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
    cfg_detect_backend(cfg_radio_path, &selection->kind);
    selection->ops = radio_backend_ops_for_kind(selection->kind);
    return selection->ops != NULL;
}

void radio_backend_configure(radio *radio_h, const radio_backend_selection *selection)
{
    if (!radio_h || !selection)
        return;

    radio_h->backend_kind = selection->kind;
    radio_h->backend_ops = selection->ops;
}

int radio_backend_run(const radio_backend_selection *selection,
                      const radio_daemon_runtime *runtime)
{
    if (!selection || !selection->ops || !runtime)
        return -1;

    /* Both backends now run through the same daemon-core loop. The
     * backend's init op handles backend-specific bring-up
     * (hamlib_init / sbitx hw_init+dsp_init+sound_system_init). */
    return radio_daemon_core_run(selection, runtime);
}

int radio_backend_force_ptt_off(const radio_backend_selection *selection,
                                const char *cfg_radio_path)
{
    static radio radio_h;

    if (!selection || !selection->ops || !selection->ops->force_ptt_off)
        return -1;

    memset(&radio_h, 0, sizeof(radio_h));
    radio_backend_configure(&radio_h, selection);
    pthread_mutex_init(&radio_h.cfg_mutex, NULL);
    if (!init_config_radio(&radio_h, cfg_radio_path))
        return -1;
    radio_backend_configure(&radio_h, selection);

    bool ok = selection->ops->force_ptt_off(&radio_h);
    close_config_radio(&radio_h);
    printf("radio_daemon: PTT off %s\n", ok ? "sent" : "FAILED");
    return ok ? 0 : -1;
}

bool radio_backend_init(radio *radio_h)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    bool ok = ops && ops->init ? ops->init(radio_h) : false;
    /* Both network servers come up after the backend, because each asks the
     * open rig what it can do. */
    rig_server_start(radio_h);
    cat_server_start(radio_h);
    return ok;
}

void radio_backend_shutdown(radio *radio_h)
{
    cat_server_stop();
    rig_server_stop();
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

/* Serialises every PTT change with the owner bookkeeping, so a release can
 * never unkey a transmission that another client started in between. */
static pthread_mutex_t ptt_lock = PTHREAD_MUTEX_INITIALIZER;
static ptt_source ptt_owner_src = PTT_SRC_NONE;
static long ptt_owner_id = 0;

static const char *ptt_source_name(ptt_source src)
{
    switch (src)
    {
    case PTT_SRC_INTERNAL:  return "internal";
    case PTT_SRC_SHM:       return "shm pid";
    case PTT_SRC_RIGCTLD:   return "rigctld fd";
    case PTT_SRC_CAT:       return "cat fd";
    case PTT_SRC_WEBSOCKET: return "websocket conn";
    case PTT_SRC_RTP:       return "rtp ssrc";
    default:                return "none";
    }
}

static void ptt_apply(radio *radio_h, bool txrx_state)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->set_txrx_state)
        ops->set_txrx_state(radio_h, txrx_state);
}

void radio_backend_set_ptt(radio *radio_h, bool txrx_state,
                           ptt_source src, long id)
{
    pthread_mutex_lock(&ptt_lock);
    if (txrx_state == IN_TX)
    {
        ptt_owner_src = src;
        ptt_owner_id = id;
    }
    else
    {
        ptt_owner_src = PTT_SRC_NONE;
        ptt_owner_id = 0;
    }
    if (src != PTT_SRC_INTERNAL)
        printf("radio: PTT %s by %s %ld\n",
               txrx_state == IN_TX ? "ON" : "OFF", ptt_source_name(src), id);
    ptt_apply(radio_h, txrx_state);
    pthread_mutex_unlock(&ptt_lock);
}

void radio_backend_end_ptt(radio *radio_h, ptt_source src, long id)
{
    pthread_mutex_lock(&ptt_lock);
    if (src != PTT_SRC_NONE && ptt_owner_src == src && ptt_owner_id == id)
    {
        ptt_owner_src = PTT_SRC_NONE;
        ptt_owner_id = 0;
        printf("radio: PTT OFF by %s %ld\n", ptt_source_name(src), id);
        ptt_apply(radio_h, IN_RX);
    }
    pthread_mutex_unlock(&ptt_lock);
}

bool radio_backend_release_ptt(radio *radio_h, ptt_source src, long id,
                               const char *why)
{
    bool released = false;

    pthread_mutex_lock(&ptt_lock);
    if (src != PTT_SRC_NONE && ptt_owner_src == src && ptt_owner_id == id)
    {
        ptt_owner_src = PTT_SRC_NONE;
        ptt_owner_id = 0;
        printf("radio: PTT OFF, %s %ld %s while keyed\n",
               ptt_source_name(src), id, why);
        ptt_apply(radio_h, IN_RX);
        released = true;
    }
    pthread_mutex_unlock(&ptt_lock);

    return released;
}

void radio_backend_set_txrx_state(radio *radio_h, bool txrx_state)
{
    radio_backend_set_ptt(radio_h, txrx_state, PTT_SRC_INTERNAL, 0);
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

uint32_t radio_backend_get_ref_power(radio *radio_h)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->get_ref_power)
        return ops->get_ref_power(radio_h);

    return radio_h ? radio_h->ref_power : 0;
}

uint32_t radio_backend_get_swr(radio *radio_h)
{
    const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h);
    if (ops && ops->get_swr)
        return ops->get_swr(radio_h);

    return 10;
}

/* ── generic control dispatch ─────────────────────────────────────────
 * A backend that does not implement an op simply leaves the slot NULL and
 * the control reports "not supported" rather than lying with a fake value.
 * That is what lets the websocket UI and rigctld clients render only what
 * the connected rig can really do. */

#define BACKEND_OPS() const radio_backend_ops *ops = radio_backend_ops_from_radio(radio_h)

int radio_backend_get_level(radio *radio_h, const char *name, double *out)
{
    BACKEND_OPS();
    if (!ops || !ops->get_level) return RADIO_CTRL_ENOTSUP;
    return ops->get_level(radio_h, name, out);
}

int radio_backend_set_level(radio *radio_h, const char *name, double value)
{
    BACKEND_OPS();
    if (!ops || !ops->set_level) return RADIO_CTRL_ENOTSUP;
    return ops->set_level(radio_h, name, value);
}

int radio_backend_get_func(radio *radio_h, const char *name, int *out)
{
    BACKEND_OPS();
    if (!ops || !ops->get_func) return RADIO_CTRL_ENOTSUP;
    return ops->get_func(radio_h, name, out);
}

int radio_backend_set_func(radio *radio_h, const char *name, int on)
{
    BACKEND_OPS();
    if (!ops || !ops->set_func) return RADIO_CTRL_ENOTSUP;
    return ops->set_func(radio_h, name, on);
}

int radio_backend_get_parm(radio *radio_h, const char *name, double *out)
{
    BACKEND_OPS();
    if (!ops || !ops->get_parm) return RADIO_CTRL_ENOTSUP;
    return ops->get_parm(radio_h, name, out);
}

int radio_backend_set_parm(radio *radio_h, const char *name, double value)
{
    BACKEND_OPS();
    if (!ops || !ops->set_parm) return RADIO_CTRL_ENOTSUP;
    return ops->set_parm(radio_h, name, value);
}

size_t radio_backend_enumerate_controls(radio *radio_h, radio_ctrl_info *out, size_t max)
{
    BACKEND_OPS();
    if (!ops || !ops->enumerate_controls) return 0;
    return ops->enumerate_controls(radio_h, out, max);
}

int radio_backend_get_vfo(radio *radio_h, char *out, size_t out_len)
{
    BACKEND_OPS();
    if (!ops || !ops->get_vfo) return RADIO_CTRL_ENOTSUP;
    return ops->get_vfo(radio_h, out, out_len);
}

int radio_backend_set_vfo(radio *radio_h, const char *vfo)
{
    BACKEND_OPS();
    if (!ops || !ops->set_vfo) return RADIO_CTRL_ENOTSUP;
    return ops->set_vfo(radio_h, vfo);
}

int radio_backend_get_split(radio *radio_h, int *on, char *tx_vfo, size_t tx_vfo_len)
{
    BACKEND_OPS();
    if (!ops || !ops->get_split) return RADIO_CTRL_ENOTSUP;
    return ops->get_split(radio_h, on, tx_vfo, tx_vfo_len);
}

int radio_backend_set_split(radio *radio_h, int on, const char *tx_vfo)
{
    BACKEND_OPS();
    if (!ops || !ops->set_split) return RADIO_CTRL_ENOTSUP;
    return ops->set_split(radio_h, on, tx_vfo);
}

int radio_backend_get_split_freq(radio *radio_h, uint32_t *hz)
{
    BACKEND_OPS();
    if (!ops || !ops->get_split_freq) return RADIO_CTRL_ENOTSUP;
    return ops->get_split_freq(radio_h, hz);
}

int radio_backend_set_split_freq(radio *radio_h, uint32_t hz)
{
    BACKEND_OPS();
    if (!ops || !ops->set_split_freq) return RADIO_CTRL_ENOTSUP;
    return ops->set_split_freq(radio_h, hz);
}

int radio_backend_get_split_mode(radio *radio_h, char *mode, size_t mode_len, uint32_t *width)
{
    BACKEND_OPS();
    if (!ops || !ops->get_split_mode) return RADIO_CTRL_ENOTSUP;
    return ops->get_split_mode(radio_h, mode, mode_len, width);
}

int radio_backend_set_split_mode(radio *radio_h, const char *mode, uint32_t width)
{
    BACKEND_OPS();
    if (!ops || !ops->set_split_mode) return RADIO_CTRL_ENOTSUP;
    return ops->set_split_mode(radio_h, mode, width);
}

int radio_backend_get_rit(radio *radio_h, int32_t *hz)
{
    BACKEND_OPS();
    if (!ops || !ops->get_rit) return RADIO_CTRL_ENOTSUP;
    return ops->get_rit(radio_h, hz);
}

int radio_backend_set_rit(radio *radio_h, int32_t hz)
{
    BACKEND_OPS();
    if (!ops || !ops->set_rit) return RADIO_CTRL_ENOTSUP;
    return ops->set_rit(radio_h, hz);
}

int radio_backend_get_xit(radio *radio_h, int32_t *hz)
{
    BACKEND_OPS();
    if (!ops || !ops->get_xit) return RADIO_CTRL_ENOTSUP;
    return ops->get_xit(radio_h, hz);
}

int radio_backend_set_xit(radio *radio_h, int32_t hz)
{
    BACKEND_OPS();
    if (!ops || !ops->set_xit) return RADIO_CTRL_ENOTSUP;
    return ops->set_xit(radio_h, hz);
}

int radio_backend_get_mode_name(radio *radio_h, char *out, size_t out_len, uint32_t *width)
{
    BACKEND_OPS();
    if (!ops || !ops->get_mode_name) return RADIO_CTRL_ENOTSUP;
    return ops->get_mode_name(radio_h, out, out_len, width);
}

int radio_backend_set_mode_name(radio *radio_h, const char *mode, uint32_t width)
{
    BACKEND_OPS();
    if (!ops || !ops->set_mode_name) return RADIO_CTRL_ENOTSUP;
    return ops->set_mode_name(radio_h, mode, width);
}

int radio_backend_get_width(radio *radio_h, uint32_t *hz)
{
    BACKEND_OPS();
    if (!ops || !ops->get_width) return RADIO_CTRL_ENOTSUP;
    return ops->get_width(radio_h, hz);
}

int radio_backend_set_width(radio *radio_h, uint32_t hz)
{
    BACKEND_OPS();
    if (!ops || !ops->set_width) return RADIO_CTRL_ENOTSUP;
    return ops->set_width(radio_h, hz);
}

int radio_backend_get_ant(radio *radio_h, int *ant)
{
    BACKEND_OPS();
    if (!ops || !ops->get_ant) return RADIO_CTRL_ENOTSUP;
    return ops->get_ant(radio_h, ant);
}

int radio_backend_set_ant(radio *radio_h, int ant)
{
    BACKEND_OPS();
    if (!ops || !ops->set_ant) return RADIO_CTRL_ENOTSUP;
    return ops->set_ant(radio_h, ant);
}

int radio_backend_get_mem(radio *radio_h, int *ch)
{
    BACKEND_OPS();
    if (!ops || !ops->get_mem) return RADIO_CTRL_ENOTSUP;
    return ops->get_mem(radio_h, ch);
}

int radio_backend_set_mem(radio *radio_h, int ch)
{
    BACKEND_OPS();
    if (!ops || !ops->set_mem) return RADIO_CTRL_ENOTSUP;
    return ops->set_mem(radio_h, ch);
}

int radio_backend_get_powerstat(radio *radio_h, int *on)
{
    BACKEND_OPS();
    if (!ops || !ops->get_powerstat) return RADIO_CTRL_ENOTSUP;
    return ops->get_powerstat(radio_h, on);
}

int radio_backend_set_powerstat(radio *radio_h, int on)
{
    BACKEND_OPS();
    if (!ops || !ops->set_powerstat) return RADIO_CTRL_ENOTSUP;
    return ops->set_powerstat(radio_h, on);
}

int radio_backend_vfo_op(radio *radio_h, const char *op)
{
    BACKEND_OPS();
    if (!ops || !ops->vfo_op) return RADIO_CTRL_ENOTSUP;
    return ops->vfo_op(radio_h, op);
}

int radio_backend_send_morse(radio *radio_h, const char *text)
{
    BACKEND_OPS();
    if (!ops || !ops->send_morse) return RADIO_CTRL_ENOTSUP;
    return ops->send_morse(radio_h, text);
}

int radio_backend_stop_morse(radio *radio_h)
{
    BACKEND_OPS();
    if (!ops || !ops->stop_morse) return RADIO_CTRL_ENOTSUP;
    return ops->stop_morse(radio_h);
}

int radio_backend_dump_state(radio *radio_h, char *out, size_t out_len)
{
    BACKEND_OPS();
    if (!ops || !ops->dump_state) return RADIO_CTRL_ENOTSUP;
    return ops->dump_state(radio_h, out, out_len);
}

int radio_backend_cat_raw(radio *radio_h, const uint8_t *req, size_t req_len,
                          uint8_t *reply, size_t reply_max, size_t *reply_len)
{
    BACKEND_OPS();
    if (!ops || !ops->cat_raw) return RADIO_CTRL_ENOTSUP;
    return ops->cat_raw(radio_h, req, req_len, reply, reply_max, reply_len);
}

uint8_t radio_backend_cat_terminator(radio *radio_h)
{
    BACKEND_OPS();
    if (!ops || !ops->cat_terminator) return 0;
    return ops->cat_terminator(radio_h);
}

void radio_backend_reset_timeout_timer(void)
{
    timer_reset = true;
}
