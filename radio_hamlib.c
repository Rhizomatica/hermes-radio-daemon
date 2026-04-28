/* hermes-radio-daemon - Hamlib radio backend
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <errno.h>

#include <hamlib/rig.h>

#include "radio.h"
#include "radio_hamlib.h"
#include "cfg_utils.h"

_Atomic bool timer_reset = true;
_Atomic time_t timeout_counter = 0;

/* Map internal MODE_* to Hamlib rmode_t */
static rmode_t mode_to_hamlib(uint16_t mode)
{
    switch (mode)
    {
    case MODE_USB: return RIG_MODE_USB;
    case MODE_LSB: return RIG_MODE_LSB;
    case MODE_CW:  return RIG_MODE_CW;
    default:       return RIG_MODE_USB;
    }
}

/* Map Hamlib rmode_t to internal MODE_* */
static uint16_t hamlib_to_mode(rmode_t hmode)
{
    if (hmode == RIG_MODE_USB || hmode == RIG_MODE_PKTUSB)
        return MODE_USB;
    if (hmode == RIG_MODE_LSB || hmode == RIG_MODE_PKTLSB)
        return MODE_LSB;
    if (hmode == RIG_MODE_CW || hmode == RIG_MODE_CWR)
        return MODE_CW;
    return MODE_USB;
}

bool radio_hamlib_init(radio *radio_h)
{
    RIG *rig;

    rig_set_debug(RIG_DEBUG_WARN);

    rig = rig_init(radio_h->hamlib_model);
    if (!rig)
    {
        fprintf(stderr, "radio_hamlib_init: rig_init failed for model %d\n",
                radio_h->hamlib_model);
        return false;
    }

    /* Serial port / network address */
    if (radio_h->rig_pathname[0])
        strncpy(rig->state.rigport.pathname, radio_h->rig_pathname,
                HAMLIB_FILPATHLEN - 1);

    /* Serial baud rate */
    if (radio_h->serial_rate > 0)
        rig->state.rigport.parm.serial.rate = radio_h->serial_rate;

    /* PTT type */
    switch (radio_h->ptt_type)
    {
    case PTT_RIG:          rig->state.pttport.type.ptt = RIG_PTT_RIG;         break;
    case PTT_SERIAL_RTS:   rig->state.pttport.type.ptt = RIG_PTT_SERIAL_RTS;  break;
    case PTT_SERIAL_DTR:   rig->state.pttport.type.ptt = RIG_PTT_SERIAL_DTR;  break;
    case PTT_PARALLEL:     rig->state.pttport.type.ptt = RIG_PTT_PARALLEL;    break;
    case PTT_CM108:        rig->state.pttport.type.ptt = RIG_PTT_CM108;       break;
    case PTT_GPIO:         rig->state.pttport.type.ptt = RIG_PTT_GPIO;        break;
    case PTT_RIG_MICDATA:  rig->state.pttport.type.ptt = RIG_PTT_RIG_MICDATA; break;
    default:               rig->state.pttport.type.ptt = RIG_PTT_NONE;        break;
    }

    if (radio_h->ptt_pathname[0] && radio_h->ptt_type != PTT_NONE &&
        radio_h->ptt_type != PTT_RIG && radio_h->ptt_type != PTT_RIG_MICDATA)
    {
        strncpy(rig->state.pttport.pathname, radio_h->ptt_pathname,
                HAMLIB_FILPATHLEN - 1);
    }

    int ret = rig_open(rig);
    if (ret != RIG_OK)
    {
        fprintf(stderr, "radio_hamlib_init: rig_open failed: %s\n",
                rigerror(ret));
        rig_cleanup(rig);
        return false;
    }

    radio_h->rig = (void *) rig;

    /* Sync initial frequency and mode from radio to our profile state */
    freq_t hfreq = 0;
    rmode_t hmode = RIG_MODE_NONE;
    pbwidth_t width = 0;
    uint32_t profile = radio_h->profile_active_idx;

    if (rig_get_freq(rig, RIG_VFO_CURR, &hfreq) == RIG_OK && hfreq > 0)
        radio_h->profiles[profile].freq = (uint32_t) hfreq;

    if (rig_get_mode(rig, RIG_VFO_CURR, &hmode, &width) == RIG_OK)
        radio_h->profiles[profile].mode = hamlib_to_mode(hmode);

    /* Ensure we start in RX */
    radio_h->txrx_state = IN_RX;

    printf("radio_hamlib_init: rig model %d opened successfully\n",
           radio_h->hamlib_model);

    return true;
}

void radio_hamlib_shutdown(radio *radio_h)
{
    if (!radio_h->rig)
        return;

    RIG *rig = (RIG *) radio_h->rig;

    /* Make sure we are in RX before closing */
    if (radio_h->txrx_state == IN_TX)
        rig_set_ptt(rig, RIG_VFO_CURR, RIG_PTT_OFF);

    rig_close(rig);
    rig_cleanup(rig);
    radio_h->rig = NULL;
}

void set_frequency(radio *radio_h, uint32_t frequency, uint32_t profile)
{
    _Atomic uint32_t *radio_freq = &radio_h->profiles[profile].freq;

    if (*radio_freq == frequency)
        return;

    *radio_freq = frequency;

    /* Apply to rig only when this is the active profile */
    if (profile == radio_h->profile_active_idx && radio_h->rig)
    {
        RIG *rig = (RIG *) radio_h->rig;
        int ret = rig_set_freq(rig, RIG_VFO_CURR, (freq_t) frequency);
        if (ret != RIG_OK)
            fprintf(stderr, "set_frequency: rig_set_freq failed: %s\n",
                    rigerror(ret));
    }

    char key[64];
    char val[32];
    snprintf(key, sizeof(key), "profile%u:freq", profile);
    snprintf(val, sizeof(val), "%u", frequency);
    cfg_set(radio_h, radio_h->cfg_user, key, val);
    radio_h->cfg_user_dirty = true;
}

void set_mode(radio *radio_h, uint16_t mode, uint32_t profile)
{
    _Atomic uint16_t *radio_mode = &radio_h->profiles[profile].mode;

    if (*radio_mode == mode)
        return;

    *radio_mode = mode;

    /* Apply to rig only when this is the active profile */
    if (profile == radio_h->profile_active_idx && radio_h->rig)
    {
        RIG *rig = (RIG *) radio_h->rig;
        rmode_t hmode = mode_to_hamlib(mode);
        int ret = rig_set_mode(rig, RIG_VFO_CURR, hmode, RIG_PASSBAND_NORMAL);
        if (ret != RIG_OK)
            fprintf(stderr, "set_mode: rig_set_mode failed: %s\n",
                    rigerror(ret));
    }

    char key[64];
    const char *mode_str = (mode == MODE_USB) ? "USB" :
                           (mode == MODE_LSB) ? "LSB" : "CW";
    snprintf(key, sizeof(key), "profile%u:mode", profile);
    cfg_set(radio_h, radio_h->cfg_user, key, mode_str);
    radio_h->cfg_user_dirty = true;
}

void tr_switch(radio *radio_h, bool txrx_state)
{
    if (txrx_state == radio_h->txrx_state)
        return;

    if (radio_h->swr_protection_enabled && txrx_state == IN_TX)
    {
        printf("tr_switch: TX blocked – SWR protection active\n");
        return;
    }

    if (radio_h->rig)
    {
        RIG *rig = (RIG *) radio_h->rig;
        ptt_t ptt_val = (txrx_state == IN_TX) ? RIG_PTT_ON : RIG_PTT_OFF;
        int ret = rig_set_ptt(rig, RIG_VFO_CURR, ptt_val);
        if (ret != RIG_OK)
            fprintf(stderr, "tr_switch: rig_set_ptt failed: %s\n",
                    rigerror(ret));
    }

    radio_h->txrx_state = txrx_state;
}

void set_bfo(radio *radio_h, uint32_t frequency)
{
    /* BFO is an sBitx-specific oscillator – no-op for Hamlib radios.
     * We keep the value in the config for API compatibility. */
    if (frequency == radio_h->bfo_frequency)
        return;

    radio_h->bfo_frequency = frequency;

    char val[32];
    snprintf(val, sizeof(val), "%u", frequency);
    cfg_set(radio_h, radio_h->cfg_radio, "main:bfo", val);
    radio_h->cfg_radio_dirty = true;
}

void set_reflected_threshold(radio *radio_h, uint32_t ref_threshold)
{
    if (ref_threshold == radio_h->reflected_threshold)
        return;

    radio_h->reflected_threshold = ref_threshold;

    char val[32];
    snprintf(val, sizeof(val), "%u", ref_threshold);
    cfg_set(radio_h, radio_h->cfg_radio, "main:reflected_threshold", val);
    radio_h->cfg_radio_dirty = true;
}

void set_speaker_volume(radio *radio_h, uint32_t speaker_level, uint32_t profile)
{
    if (profile >= radio_h->profiles_count)
        return;

    radio_h->profiles[profile].speaker_level = speaker_level;

    char key[64];
    char val[32];
    snprintf(key, sizeof(key), "profile%u:speaker_level", profile);
    snprintf(val, sizeof(val), "%u", speaker_level);
    cfg_set(radio_h, radio_h->cfg_user, key, val);
    radio_h->cfg_user_dirty = true;
}

void set_serial(radio *radio_h, uint32_t serial)
{
    if (serial == radio_h->serial_number)
        return;

    radio_h->serial_number = serial;

    char val[32];
    snprintf(val, sizeof(val), "%u", serial);
    cfg_set(radio_h, radio_h->cfg_radio, "main:serial_number", val);
    radio_h->cfg_radio_dirty = true;
}

void set_profile_timeout(radio *radio_h, int32_t timeout)
{
    if (timeout == radio_h->profile_timeout)
        return;

    radio_h->profile_timeout = timeout;

    char val[32];
    snprintf(val, sizeof(val), "%d", timeout);
    cfg_set(radio_h, radio_h->cfg_user,
            "main:default_profile_fallback_timeout", val);
    radio_h->cfg_user_dirty = true;
}

void set_power_knob(radio *radio_h, uint16_t power_level, uint32_t profile)
{
    if (profile >= radio_h->profiles_count)
        return;

    if (power_level > 100)
        power_level = 100;

    radio_h->profiles[profile].power_level_percentage = power_level;

    /* Optionally apply RF power level via Hamlib */
    if (profile == radio_h->profile_active_idx && radio_h->rig)
    {
        RIG *rig = (RIG *) radio_h->rig;
        value_t val;
        val.f = (float) power_level / 100.0f;
        rig_set_level(rig, RIG_VFO_CURR, RIG_LEVEL_RFPOWER, val);
    }

    char key[64];
    char val[32];
    snprintf(key, sizeof(key), "profile%u:power_level_percentage", profile);
    snprintf(val, sizeof(val), "%u", power_level);
    cfg_set(radio_h, radio_h->cfg_user, key, val);
    radio_h->cfg_user_dirty = true;
}

void set_digital_voice(radio *radio_h, bool digital_voice, uint32_t profile)
{
    if (profile >= radio_h->profiles_count)
        return;

    radio_h->profiles[profile].digital_voice = digital_voice;

    char key[64];
    char val[4];
    snprintf(key, sizeof(key), "profile%u:digital_voice", profile);
    snprintf(val, sizeof(val), "%d", digital_voice ? 1 : 0);
    cfg_set(radio_h, radio_h->cfg_user, key, val);
    radio_h->cfg_user_dirty = true;
}

void set_step_size(radio *radio_h, uint32_t step_size)
{
    if (radio_h->step_size == step_size)
        return;

    radio_h->step_size = step_size;

    char val[32];
    snprintf(val, sizeof(val), "%u", step_size);
    cfg_set(radio_h, radio_h->cfg_user, "main:step_size", val);
    radio_h->cfg_user_dirty = true;
}

void set_tone_generation(radio *radio_h, bool tone_generation)
{
    if (radio_h->tone_generation == tone_generation)
        return;

    radio_h->tone_generation = tone_generation;

    cfg_set(radio_h, radio_h->cfg_user, "main:tone_generation",
            tone_generation ? "1" : "0");
    radio_h->cfg_user_dirty = true;
}

void set_profile(radio *radio_h, uint32_t profile)
{
    if (radio_h->profile_active_idx == profile)
        return;

    radio_h->profile_active_idx = profile;

    /* Apply the new profile's frequency and mode to the rig */
    uint32_t freq = radio_h->profiles[profile].freq;
    uint16_t mode = radio_h->profiles[profile].mode;

    if (radio_h->rig)
    {
        RIG *rig = (RIG *) radio_h->rig;
        rig_set_freq(rig, RIG_VFO_CURR, (freq_t) freq);
        rig_set_mode(rig, RIG_VFO_CURR, mode_to_hamlib(mode),
                     RIG_PASSBAND_NORMAL);

        value_t rfval;
        rfval.f = (float) radio_h->profiles[profile].power_level_percentage / 100.0f;
        rig_set_level(rig, RIG_VFO_CURR, RIG_LEVEL_RFPOWER, rfval);
    }

    /* Save current profile index */
    char val[32];
    snprintf(val, sizeof(val), "%u", profile);
    cfg_set(radio_h, radio_h->cfg_user, "main:current_profile", val);
    radio_h->cfg_user_dirty = true;
}

uint32_t get_fwd_power(radio *radio_h)
{
    if (!radio_h->rig)
        return radio_h->fwd_power;

    RIG *rig = (RIG *) radio_h->rig;
    value_t val;
    val.f = 0.0f;

    int ret = rig_get_level(rig, RIG_VFO_CURR, RIG_LEVEL_RFPOWER_METER, &val);
    if (ret == RIG_OK)
    {
        /* Hamlib returns 0..1 normalized; scale to W*10 (assume 100W max) */
        radio_h->fwd_power = (uint32_t)(val.f * 1000.0f);
    }

    return radio_h->fwd_power;
}

uint32_t get_ref_power(radio *radio_h)
{
    return radio_h->ref_power;
}

uint32_t get_swr(radio *radio_h)
{
    if (!radio_h->rig)
        return 10; /* 1.0 SWR */

    RIG *rig = (RIG *) radio_h->rig;
    value_t val;
    val.f = 0.0f;

    int ret = rig_get_level(rig, RIG_VFO_CURR, RIG_LEVEL_SWR, &val);
    if (ret == RIG_OK)
        return (uint32_t)(val.f * 10.0f);

    /* Fallback: compute from fwd/ref voltages if available */
    uint32_t vfwd = radio_h->fwd_power;
    uint32_t vref = radio_h->ref_power;

    if (vfwd == 0)
        return 10;

    if (vref >= vfwd)
        return 100;

    return (10 * (vfwd + vref)) / (vfwd - vref);
}

bool update_power_measurements(radio *radio_h)
{
    /* For radios that expose the SWR meter via Hamlib */
    get_fwd_power(radio_h);
    return true;
}

void swr_protection_check(radio *radio_h)
{
    uint32_t vswr = get_swr(radio_h);

    static _Atomic uint16_t peak_counter = 0;

    if (vswr > radio_h->reflected_threshold && radio_h->fwd_power > 0)
        peak_counter++;
    else
        peak_counter = 0;

    /* Require several consecutive readings above threshold (~300 ms at 100 ms poll) */
    if (peak_counter > 3)
    {
        tr_switch(radio_h, IN_RX);
        radio_h->swr_protection_enabled = true;
        peak_counter = 0;
    }
}

void *radio_io_thread(void *radio_h_v)
{
    radio *radio_h = (radio *) radio_h_v;

    int res = start_periodic_timer(100000); /* 100 ms period */
    if (res < 0)
    {
        fprintf(stderr, "radio_io_thread: start_periodic_timer failed\n");
        shutdown_ = true;
        return NULL;
    }

    while (!shutdown_)
    {
        wait_next_activation();

        /* Poll power measurements while transmitting */
        if (radio_h->txrx_state == IN_TX)
        {
            update_power_measurements(radio_h);
            swr_protection_check(radio_h);
        }
        else
        {
            if (!radio_h->swr_protection_enabled)
            {
                radio_h->fwd_power = 0;
                radio_h->ref_power = 0;
            }
        }

        /* Profile auto-return timer */
        static time_t last_time = 0;

        if (radio_h->profile_default_idx != radio_h->profile_active_idx &&
            radio_h->profile_timeout >= 0)
        {
            if (timer_reset)
            {
                last_time = time(NULL);
                timer_reset = false;
                timeout_counter = radio_h->profile_timeout;
            }
            else
            {
                time_t curr_time = time(NULL);
                if (curr_time > last_time)
                {
                    timeout_counter -= curr_time - last_time;
                    last_time = curr_time;
                    if (timeout_counter <= 0)
                    {
                        set_profile(radio_h, radio_h->profile_default_idx);
                        timer_reset = true;
                    }
                }
            }
        }
        else
        {
            timer_reset = true;
            timeout_counter = radio_h->profile_timeout;
        }
    }

    return NULL;
}

/* ---- Periodic timer helpers ---- */

static struct timespec timer_next;
static uint64_t timer_period_us;
#define NSEC_PER_SEC 1000000000ULL

static inline void timespec_add_us(struct timespec *t, uint64_t us)
{
    uint64_t ns = us * 1000ULL;
    t->tv_nsec += (long) ns;
    t->tv_sec  += t->tv_nsec / (long) NSEC_PER_SEC;
    t->tv_nsec %= (long) NSEC_PER_SEC;
}

void wait_next_activation(void)
{
    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &timer_next, NULL);
    timespec_add_us(&timer_next, timer_period_us);
}

int start_periodic_timer(uint64_t offset_us)
{
    clock_gettime(CLOCK_REALTIME, &timer_next);
    timespec_add_us(&timer_next, offset_us);
    timer_period_us = offset_us;
    return 0;
}
