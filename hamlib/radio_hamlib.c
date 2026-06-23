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
#include <pthread.h>

#include <hamlib/rig.h>

#include "radio.h"
#include "radio_hamlib.h"
#include "radio_pipeline.h"
#include "cfg_utils.h"
#include "radio_backend.h"
#include "hamlib_digi.h"

_Atomic bool timer_reset = true;
_Atomic time_t timeout_counter = 0;

static rmode_t mode_to_hamlib(uint16_t mode, bool data_path);
static uint16_t hamlib_to_mode(rmode_t hmode);
static bool profile_data_path(const radio_profile *p);
static void wait_next_activation(void);
static int  start_periodic_timer(uint64_t offset_us);

/* Serializes ALL access to the rig's serial port. Several threads reach this
 * backend concurrently — the 100 ms meter poll in radio_io_thread, the SHM
 * control thread, and the websocket thread — and a Hamlib RIG handle is not
 * thread-safe. Without this lock a meter read (RM5;/RM6;/SWR) from the poll
 * can interleave on the wire with a PTT command from the control thread,
 * desyncing the serial buffer and dropping the PTT. That was the real cause
 * of the "FT-710 breaks PTT" symptom previously worked around by disabling
 * meter reads for that model. Recursive, so an entry point may call a helper
 * that locks again on the same thread (e.g. get_swr -> read_level_float,
 * tr_switch -> sync_txrx_state). */
static pthread_mutex_t hl_serial_lock;

static void hl_serial_lock_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&hl_serial_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

#define RIG_LOCK()   pthread_mutex_lock(&hl_serial_lock)
#define RIG_UNLOCK() pthread_mutex_unlock(&hl_serial_lock)

/* PTT type as the string token rig_set_conf("ptt_type", ...) expects
 * (see src/conf.c TOK_PTT_TYPE). */
static const char *hamlib_ptt_type_conf(uint8_t ptt_type)
{
    switch (ptt_type)
    {
    case PTT_RIG:          return "RIG";
    case PTT_RIG_MICDATA:  return "RIGMICDATA";
    case PTT_SERIAL_DTR:   return "DTR";
    case PTT_SERIAL_RTS:   return "RTS";
    case PTT_PARALLEL:     return "Parallel";
    case PTT_CM108:        return "CM108";
    case PTT_GPIO:         return "GPIO";
    default:               return "None";
    }
}

static void hamlib_set_conf(RIG *rig, const char *name, const char *val)
{
    int ret = rig_set_conf(rig, rig_token_lookup(rig, name), val);
    if (ret != RIG_OK)
        fprintf(stderr, "hamlib_configure_ports: set_conf %s=\"%s\" failed: %s\n",
                name, val, rigerror(ret));
}

/* Configure the CAT/PTT ports through the public rig_set_conf() API rather
 * than writing rig->state.rigport/pttport directly. Hamlib 4.7 removed the
 * embedded `state` member from struct rig and the RIGPORT()/PTTPORT() macros
 * are gated behind IN_HAMLIB (backend-only), so direct access no longer
 * compiles for an application. rig_set_conf() with the "rig_pathname",
 * "serial_speed", "ptt_type" and "ptt_pathname" tokens is the supported,
 * version-stable path (present in 4.6.x and 4.7.x) and is exactly what
 * rigctl's -r/-p/-P options use. Must be called after rig_init, before
 * rig_open. */
static void hamlib_configure_ports(RIG *rig, const radio *radio_h)
{
    const char *ptt_path = radio_h->ptt_pathname;

    if (radio_h->rig_pathname[0])
        hamlib_set_conf(rig, "rig_pathname", radio_h->rig_pathname);

    if (radio_h->serial_rate > 0)
    {
        char rate[16];
        snprintf(rate, sizeof(rate), "%d", radio_h->serial_rate);
        hamlib_set_conf(rig, "serial_speed", rate);
    }

    hamlib_set_conf(rig, "ptt_type", hamlib_ptt_type_conf(radio_h->ptt_type));

    if ((radio_h->ptt_type == PTT_SERIAL_RTS || radio_h->ptt_type == PTT_SERIAL_DTR) &&
        !ptt_path[0])
        ptt_path = radio_h->rig_pathname;

    if (ptt_path[0] && radio_h->ptt_type != PTT_NONE &&
        radio_h->ptt_type != PTT_RIG && radio_h->ptt_type != PTT_RIG_MICDATA)
        hamlib_set_conf(rig, "ptt_pathname", ptt_path);
}

static bool hamlib_read_level_float(RIG *rig, setting_t level, float *out)
{
    value_t val;

    if (!rig || !out || !rig_has_get_level(rig, level))
        return false;

    memset(&val, 0, sizeof(val));
    RIG_LOCK();
    int ret = rig_get_level(rig, RIG_VFO_CURR, level, &val);
    RIG_UNLOCK();
    if (ret != RIG_OK)
        return false;

    *out = val.f;
    return true;
}

static bool hamlib_set_level_float(RIG *rig,
                                   setting_t level,
                                   float value,
                                   const char *label)
{
    value_t val;
    int ret;

    if (!rig || !rig_has_set_level(rig, level))
        return false;

    memset(&val, 0, sizeof(val));
    val.f = value;
    RIG_LOCK();
    ret = rig_set_level(rig, RIG_VFO_CURR, level, val);
    RIG_UNLOCK();
    if (ret != RIG_OK)
        fprintf(stderr, "%s: rig_set_level failed: %s\n",
                label ? label : "hamlib_set_level_float",
                rigerror(ret));

    return ret == RIG_OK;
}

static void hamlib_update_reflected_from_swr(radio *radio_h, float swr)
{
    float gamma;

    if (!radio_h || swr <= 0.0f)
        return;

    if (swr <= 1.0f || radio_h->fwd_power == 0)
    {
        radio_h->ref_power = 0;
        return;
    }

    gamma = (swr - 1.0f) / (swr + 1.0f);
    if (gamma < 0.0f)
        gamma = 0.0f;
    if (gamma > 1.0f)
        gamma = 1.0f;

    radio_h->ref_power = (uint32_t) lrintf((float) radio_h->fwd_power *
                                           gamma * gamma);
}

static bool hamlib_update_measurements(radio *radio_h)
{
    RIG *rig;
    float meter_value = 0.0f;
    float swr = 0.0f;
    bool updated = false;

    if (!radio_h || !radio_h->rig)
        return false;

    rig = (RIG *) radio_h->rig;

    /* Group the PO + SWR reads under one lock hold so a single poll sees a
     * consistent pair and no control command splits them. */
    RIG_LOCK();

    if (hamlib_read_level_float(rig, RIG_LEVEL_RFPOWER_METER_WATTS, &meter_value) &&
        meter_value >= 0.0f)
    {
        radio_h->fwd_power = (uint32_t) lrintf(meter_value * 10.0f);
        updated = true;
    }
    else if (hamlib_read_level_float(rig, RIG_LEVEL_RFPOWER_METER, &meter_value) &&
             meter_value >= 0.0f)
    {
        if (meter_value > 1.0f)
            meter_value = 1.0f;
        radio_h->fwd_power = (uint32_t) lrintf(meter_value * 1000.0f);
        updated = true;
    }

    if (hamlib_read_level_float(rig, RIG_LEVEL_SWR, &swr) && swr > 0.0f)
    {
        hamlib_update_reflected_from_swr(radio_h, swr);
        updated = true;
    }

    RIG_UNLOCK();

    return updated;
}

/* noinline: called from the radio_io_thread hot loop; letting -Ofast inline it
 * there makes GCC's object-size analysis on the _Atomic radio_h fields go
 * sideways and emit bogus -Wstringop-overflow warnings. Keeping it out-of-line
 * costs one call per ~200 ms and keeps the build clean. */
__attribute__((noinline))
static void hamlib_sync_txrx_state(radio *radio_h, bool fallback_state)
{
    ptt_t ptt_state = RIG_PTT_OFF;
    RIG *rig;

    if (!radio_h || !radio_h->rig)
    {
        if (radio_h)
            radio_h->txrx_state = fallback_state;
        return;
    }

    rig = (RIG *) radio_h->rig;
    RIG_LOCK();
    int ret = rig_get_ptt(rig, RIG_VFO_CURR, &ptt_state);
    RIG_UNLOCK();
    if (ret == RIG_OK)
        radio_h->txrx_state = (ptt_state == RIG_PTT_OFF) ? IN_RX : IN_TX;
    else
        radio_h->txrx_state = fallback_state;
}

static void hamlib_apply_profile(radio *radio_h, uint32_t profile)
{
    RIG *rig;
    int ret;

    if (!radio_h || !radio_h->rig || profile >= radio_h->profiles_count)
        return;

    rig = (RIG *) radio_h->rig;

    /* Apply freq + mode + power as one transaction so a concurrent poll can't
     * slip a meter read between the commands. */
    RIG_LOCK();

    ret = rig_set_freq(rig, RIG_VFO_CURR, (freq_t) radio_h->profiles[profile].freq);
    if (ret != RIG_OK)
        fprintf(stderr, "hamlib_apply_profile: rig_set_freq failed: %s\n",
                rigerror(ret));

    ret = rig_set_mode(rig, RIG_VFO_CURR,
                       mode_to_hamlib(radio_h->profiles[profile].mode,
                                      profile_data_path(&radio_h->profiles[profile])),
                       RIG_PASSBAND_NORMAL);
    if (ret != RIG_OK)
        fprintf(stderr, "hamlib_apply_profile: rig_set_mode failed: %s\n",
                rigerror(ret));

    hamlib_set_level_float(rig,
                           RIG_LEVEL_RFPOWER,
                           (float) radio_h->profiles[profile].power_level_percentage / 100.0f,
                           "hamlib_apply_profile");

    RIG_UNLOCK();
}

/* True when the profile feeds the rig from the digital path (rear-panel DATA
 * input / USB codec) rather than the mic input. Two independent signals can
 * select the data path:
 *
 *   - `operating_mode != FULL_VOICE` — hfsignals-style routing override; also
 *     used on the hamlib backend to request DATA-mode on the rig.
 *   - `digital_voice == true` — RADAE / digital-voice codec output: the audio
 *     is codec-generated, not mic-sourced, so it must go through DATA-U even
 *     when the operator left operating_mode at FULL_VOICE.
 *
 * Either signal flips the rig to its data-side mode (PKTUSB/PKTLSB/PKTFM/…). */
static bool profile_data_path(const radio_profile *p)
{
    return (p->operating_mode != OPERATING_MODE_FULL_VOICE) || p->digital_voice;
}

/* Map internal MODE_* to Hamlib rmode_t.
 *
 * Two questions decide the rig mode:
 *   (a) Does this internal MODE_* have a voice equivalent at all?
 *   (b) When the daemon's software modem is generating the audio (data_path
 *       true), the rig must be a transparent SSB/FM/AM data path — NOT in its
 *       own internal CW/RTTY/FT8 mode, which would replace our audio with the
 *       rig's own keyer/FSK/etc.
 *
 *   - FT8, DRM, RADAE: no voice variant exists. FT8 is USB worldwide
 *     (WSJT-X enforces this). DRM/RADAE TX audio is OFDM/codec — always USB-
 *     side. All three → RIG_MODE_PKTUSB regardless of data_path.
 *   - CW: with our software keyer (sbitx_cw, audio-domain DDS into data port)
 *     the rig must be PKTUSB so it passes our tone unmodified. Only when the
 *     daemon is NOT pushing audio (data_path false, e.g. operator wired a
 *     mechanical key into the rig) do we switch the rig into RIG_MODE_CW so
 *     its internal keyer handles transmission.
 *   - RTTY: same logic. Our minimodem-based AFSK lives in audio; on the data
 *     path → PKTLSB (AFSK RTTY convention is LSB so mark>space comes out
 *     true-FSK on air). If data_path is off the rig's internal FSK keyer is
 *     used → RIG_MODE_RTTY.
 *   - USB/LSB/FM/AM: voice when data_path false, DATA-* variant otherwise.
 *
 * Narrow data variants (RIG_MODE_PKTFMN, FMN, AMN) need a separate narrow-
 * bandwidth flag in our MODE_* set; not wired yet. */
static rmode_t mode_to_hamlib(uint16_t mode, bool data_path)
{
    switch (mode)
    {
    case MODE_USB:  return data_path ? RIG_MODE_PKTUSB : RIG_MODE_USB;
    case MODE_LSB:  return data_path ? RIG_MODE_PKTLSB : RIG_MODE_LSB;
    case MODE_FM:   return data_path ? RIG_MODE_PKTFM  : RIG_MODE_FM;
    case MODE_AM:   return data_path ? RIG_MODE_PKTAM  : RIG_MODE_AM;
    case MODE_CW:   return data_path ? RIG_MODE_PKTUSB : RIG_MODE_CW;
    case MODE_RTTY: return data_path ? RIG_MODE_PKTLSB : RIG_MODE_RTTY;
    case MODE_DRM:  return RIG_MODE_PKTUSB;   /* digital only, no voice DRM   */
    case MODE_FT8:  return RIG_MODE_PKTUSB;   /* USB worldwide by convention  */
    default:        return data_path ? RIG_MODE_PKTUSB : RIG_MODE_USB;
    }
}

/* Map Hamlib rmode_t to internal MODE_*. PKTUSB → USB / PKTLSB → LSB: the
 * data/voice distinction is carried by the profile's `operating_mode`, not by
 * the user-facing MODE_USB label, so reading PKTUSB back from the rig must
 * stay as MODE_USB (otherwise applying a "USB on digi profile" would round-
 * trip to MODE_FT8 and overwrite the operator's selection). MODE_FT8 is a
 * daemon-side modulator choice, not something inferred from rig state. */
static uint16_t hamlib_to_mode(rmode_t hmode)
{
    if (hmode == RIG_MODE_USB || hmode == RIG_MODE_PKTUSB)
        return MODE_USB;
    if (hmode == RIG_MODE_LSB || hmode == RIG_MODE_PKTLSB)
        return MODE_LSB;
    if (hmode == RIG_MODE_CW || hmode == RIG_MODE_CWR)
        return MODE_CW;
    if (hmode == RIG_MODE_FM || hmode == RIG_MODE_FMN ||
        hmode == RIG_MODE_PKTFM || hmode == RIG_MODE_PKTFMN)
        return MODE_FM;
    if (hmode == RIG_MODE_AM || hmode == RIG_MODE_AMS ||
        hmode == RIG_MODE_AMN || hmode == RIG_MODE_PKTAM)
        return MODE_AM;
    if (hmode == RIG_MODE_RTTY || hmode == RIG_MODE_RTTYR)
        return MODE_RTTY;
    return MODE_USB;
}

static const char *mode_to_string(uint16_t mode)
{
    switch (mode)
    {
    case MODE_LSB:  return "LSB";
    case MODE_USB:  return "USB";
    case MODE_CW:   return "CW";
    case MODE_FM:   return "FM";
    case MODE_AM:   return "AM";
    case MODE_DRM:  return "DRM";
    case MODE_FT8:  return "FT8";
    case MODE_RTTY: return "RTTY";
    default:        return "USB";
    }
}

static bool radio_hamlib_init(radio *radio_h)
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

    hamlib_configure_ports(rig, radio_h);

    hl_serial_lock_init();

    int ret = rig_open(rig);
    if (ret != RIG_OK)
    {
        fprintf(stderr, "radio_hamlib_init: rig_open failed: %s\n",
                rigerror(ret));
        rig_cleanup(rig);
        return false;
    }

    radio_h->rig = (void *) rig;

    /* Apply the selected profile to the rig, then mirror the resulting state. */
    freq_t hfreq = 0;
    rmode_t hmode = RIG_MODE_NONE;
    pbwidth_t width = 0;
    uint32_t profile = radio_h->profile_active_idx;

    if (profile >= radio_h->profiles_count)
        profile = 0;

    if (radio_h->profiles_count > 0)
        hamlib_apply_profile(radio_h, profile);

    RIG_LOCK();
    if (rig_get_freq(rig, RIG_VFO_CURR, &hfreq) == RIG_OK && hfreq > 0)
        radio_h->profiles[profile].freq = (uint32_t) hfreq;

    if (rig_get_mode(rig, RIG_VFO_CURR, &hmode, &width) == RIG_OK)
        radio_h->profiles[profile].mode = hamlib_to_mode(hmode);
    RIG_UNLOCK();

    hamlib_sync_txrx_state(radio_h, IN_RX);
    hamlib_update_measurements(radio_h);

    printf("radio_hamlib_init: rig model %d opened successfully\n",
           radio_h->hamlib_model);

    /* FT8 / CW / RTTY pump (text-mode encoders + decoders against the
     * daemon audio rings). Idle when the active mode isn't digital. */
    hamlib_digi_start(radio_h);

    return true;
}

static void radio_hamlib_shutdown(radio *radio_h)
{
    hamlib_digi_stop(radio_h);

    if (!radio_h->rig)
        return;

    RIG *rig = (RIG *) radio_h->rig;

    /* Make sure we are in RX before closing */
    RIG_LOCK();
    if (radio_h->txrx_state == IN_TX)
        rig_set_ptt(rig, RIG_VFO_CURR, RIG_PTT_OFF);

    rig_close(rig);
    rig_cleanup(rig);
    RIG_UNLOCK();
    radio_h->rig = NULL;
}

static void set_frequency(radio *radio_h, uint32_t frequency, uint32_t profile)
{
    if (profile >= radio_h->profiles_count)
        return;

    _Atomic uint32_t *radio_freq = &radio_h->profiles[profile].freq;

    if (*radio_freq == frequency)
        return;

    *radio_freq = frequency;

    /* Apply to rig only when this is the active profile */
    if (profile == radio_h->profile_active_idx && radio_h->rig)
    {
        RIG *rig = (RIG *) radio_h->rig;
        RIG_LOCK();
        int ret = rig_set_freq(rig, RIG_VFO_CURR, (freq_t) frequency);
        RIG_UNLOCK();
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

static void set_mode(radio *radio_h, uint16_t mode, uint32_t profile)
{
    if (profile >= radio_h->profiles_count)
        return;

    _Atomic uint16_t *radio_mode = &radio_h->profiles[profile].mode;

    if (*radio_mode == mode)
        return;

    *radio_mode = mode;

    /* Apply to rig only when this is the active profile */
    if (profile == radio_h->profile_active_idx && radio_h->rig)
    {
        RIG *rig = (RIG *) radio_h->rig;
        rmode_t hmode = mode_to_hamlib(mode,
                                       profile_data_path(&radio_h->profiles[profile]));
        RIG_LOCK();
        int ret = rig_set_mode(rig, RIG_VFO_CURR, hmode, RIG_PASSBAND_NORMAL);
        RIG_UNLOCK();
        if (ret != RIG_OK)
            fprintf(stderr, "set_mode: rig_set_mode failed: %s\n",
                    rigerror(ret));
    }

    char key[64];
    snprintf(key, sizeof(key), "profile%u:mode", profile);
    cfg_set(radio_h, radio_h->cfg_user, key, mode_to_string(mode));
    radio_h->cfg_user_dirty = true;
}

static void tr_switch(radio *radio_h, bool txrx_state)
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
        ptt_t ptt_val = RIG_PTT_OFF;
        if (txrx_state == IN_TX)
            ptt_val = (radio_h->ptt_type == PTT_RIG_MICDATA) ? RIG_PTT_ON_DATA
                                                             : RIG_PTT_ON;
        RIG_LOCK();
        int ret = rig_set_ptt(rig, RIG_VFO_CURR, ptt_val);
        RIG_UNLOCK();
        if (ret != RIG_OK)
        {
            fprintf(stderr, "tr_switch: rig_set_ptt failed: %s\n",
                    rigerror(ret));
            return;
        }

        hamlib_sync_txrx_state(radio_h, txrx_state);
        return;
    }

    radio_h->txrx_state = txrx_state;
}

static void set_bfo(radio *radio_h, uint32_t frequency)
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

static void set_reflected_threshold(radio *radio_h, uint32_t ref_threshold)
{
    if (ref_threshold == radio_h->reflected_threshold)
        return;

    radio_h->reflected_threshold = ref_threshold;

    char val[32];
    snprintf(val, sizeof(val), "%u", ref_threshold);
    cfg_set(radio_h, radio_h->cfg_radio, "main:reflected_threshold", val);
    radio_h->cfg_radio_dirty = true;
}

static void set_speaker_volume(radio *radio_h, uint32_t speaker_level, uint32_t profile)
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

static void set_serial(radio *radio_h, uint32_t serial)
{
    if (serial == radio_h->serial_number)
        return;

    radio_h->serial_number = serial;

    char val[32];
    snprintf(val, sizeof(val), "%u", serial);
    cfg_set(radio_h, radio_h->cfg_radio, "main:serial_number", val);
    radio_h->cfg_radio_dirty = true;
}

static void set_profile_timeout(radio *radio_h, int32_t timeout)
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

static void set_power_knob(radio *radio_h, uint16_t power_level, uint32_t profile)
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
        hamlib_set_level_float(rig, RIG_LEVEL_RFPOWER,
                               (float) power_level / 100.0f,
                               "set_power_knob");
    }

    char key[64];
    char val[32];
    snprintf(key, sizeof(key), "profile%u:power_level_percentage", profile);
    snprintf(val, sizeof(val), "%u", power_level);
    cfg_set(radio_h, radio_h->cfg_user, key, val);
    radio_h->cfg_user_dirty = true;
}

static void set_digital_voice(radio *radio_h, bool digital_voice, uint32_t profile)
{
    if (profile >= radio_h->profiles_count)
        return;

    radio_h->profiles[profile].digital_voice = digital_voice;
    radio_pipeline_refresh(radio_h);

    /* Toggling RADAE flips data_path → re-apply the rig mode so the rig
     * follows the audio source (USB ↔ PKTUSB on USB-side profiles, etc.).
     * Only meaningful for the active profile; others get applied on switch. */
    if (profile == radio_h->profile_active_idx && radio_h->rig)
    {
        RIG *rig = (RIG *) radio_h->rig;
        rmode_t hmode = mode_to_hamlib(radio_h->profiles[profile].mode,
                                       profile_data_path(&radio_h->profiles[profile]));
        RIG_LOCK();
        int ret = rig_set_mode(rig, RIG_VFO_CURR, hmode, RIG_PASSBAND_NORMAL);
        RIG_UNLOCK();
        if (ret != RIG_OK)
            fprintf(stderr, "set_digital_voice: rig_set_mode failed: %s\n",
                    rigerror(ret));
    }

    char key[64];
    char val[4];
    snprintf(key, sizeof(key), "profile%u:digital_voice", profile);
    snprintf(val, sizeof(val), "%d", digital_voice ? 1 : 0);
    cfg_set(radio_h, radio_h->cfg_user, key, val);
    radio_h->cfg_user_dirty = true;
}

static void set_step_size(radio *radio_h, uint32_t step_size)
{
    if (radio_h->step_size == step_size)
        return;

    radio_h->step_size = step_size;

    char val[32];
    snprintf(val, sizeof(val), "%u", step_size);
    cfg_set(radio_h, radio_h->cfg_user, "main:step_size", val);
    radio_h->cfg_user_dirty = true;
}

static void set_tone_generation(radio *radio_h, bool tone_generation)
{
    if (radio_h->tone_generation == tone_generation)
        return;

    radio_h->tone_generation = tone_generation;

    cfg_set(radio_h, radio_h->cfg_user, "main:tone_generation",
            tone_generation ? "1" : "0");
    radio_h->cfg_user_dirty = true;
}

static void set_profile(radio *radio_h, uint32_t profile)
{
    if (radio_h->profile_active_idx == profile)
        return;

    if (profile >= radio_h->profiles_count)
        return;

    radio_h->profile_active_idx = profile;
    radio_pipeline_refresh(radio_h);
    hamlib_apply_profile(radio_h, profile);

    /* Save current profile index */
    char val[32];
    snprintf(val, sizeof(val), "%u", profile);
    cfg_set(radio_h, radio_h->cfg_user, "main:current_profile", val);
    radio_h->cfg_user_dirty = true;
}

static uint32_t get_fwd_power(radio *radio_h)
{
    if (!radio_h->rig)
        return radio_h->fwd_power;

    hamlib_update_measurements(radio_h);

    return radio_h->fwd_power;
}

static uint32_t get_ref_power(radio *radio_h)
{
    if (!radio_h || !radio_h->rig)
        return 0;
    hamlib_update_measurements(radio_h);
    return radio_h->ref_power;
}

static uint32_t get_swr(radio *radio_h)
{
    if (!radio_h->rig)
        return 10; /* 1.0 SWR */

    RIG *rig = (RIG *) radio_h->rig;
    float swr = 0.0f;

    if (hamlib_read_level_float(rig, RIG_LEVEL_SWR, &swr) && swr > 0.0f)
    {
        hamlib_update_reflected_from_swr(radio_h, swr);
        return (uint32_t) lrintf(swr * 10.0f);
    }

    /* Fallback: compute from fwd/ref voltages if available */
    uint32_t vfwd = radio_h->fwd_power;
    uint32_t vref = radio_h->ref_power;

    if (vfwd == 0)
        return 10;

    if (vref >= vfwd)
        return 100;

    return (10 * (vfwd + vref)) / (vfwd - vref);
}

static bool update_power_measurements(radio *radio_h)
{
    return hamlib_update_measurements(radio_h);
}

static void swr_protection_check(radio *radio_h)
{
    if (radio_h->reflected_threshold == 0)
        return;

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

/* Refresh the active profile's cached freq/mode from the rig so that manual
 * front-panel changes on the radio are reflected in the daemon's reported
 * state (clients read these atomics directly; nothing else reads them back).
 * RX-only: in TX the serial link is busy with metering and the operator
 * shouldn't be retuning anyway. Serialized via the rig lock like every other
 * CAT access. In-memory only — we deliberately do not rewrite user.ini here,
 * so spinning the VFO knob doesn't churn the config; the configured profile
 * value is still what's restored on restart. */
static void hamlib_poll_vfo_state(radio *radio_h)
{
    RIG *rig = (RIG *) radio_h->rig;
    uint32_t profile = radio_h->profile_active_idx;

    if (!rig || profile >= radio_h->profiles_count)
        return;

    freq_t    hfreq = 0;
    rmode_t   hmode = RIG_MODE_NONE;
    pbwidth_t width = 0;

    RIG_LOCK();
    int fr = rig_get_freq(rig, RIG_VFO_CURR, &hfreq);
    int mr = rig_get_mode(rig, RIG_VFO_CURR, &hmode, &width);
    RIG_UNLOCK();

    if (fr == RIG_OK && hfreq > 0)
        radio_h->profiles[profile].freq = (uint32_t) hfreq;

    if (mr == RIG_OK)
        radio_h->profiles[profile].mode = hamlib_to_mode(hmode);
}

static void *radio_io_thread(void *radio_h_v)
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

        /* Reflect the rig's ACTUAL PTT — including manual front-panel or mic
         * keying the daemon didn't initiate — by polling rig_get_ptt ~every
         * 200 ms. Without this, txrx_state only changed when the daemon keyed
         * via tr_switch, so a hand-keyed TX never showed up in the daemon or
         * its clients. Once txrx_state flips to IN_TX the block below starts
         * metering FWD/SWR for that manual transmission too. */
        static int ptt_tick = 0;
        if (++ptt_tick >= 2)
        {
            ptt_tick = 0;
            bool cur_state = radio_h->txrx_state;
            hamlib_sync_txrx_state(radio_h, cur_state);
        }

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

            /* Sync freq/mode from the rig ~every 500 ms (every 5th 100 ms
             * tick) so front-panel changes show up in the daemon state. */
            static int vfo_tick = 0;
            if (++vfo_tick >= 5)
            {
                vfo_tick = 0;
                hamlib_poll_vfo_state(radio_h);
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

static void wait_next_activation(void)
{
    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &timer_next, NULL);
    timespec_add_us(&timer_next, timer_period_us);
}

static int start_periodic_timer(uint64_t offset_us)
{
    clock_gettime(CLOCK_REALTIME, &timer_next);
    timespec_add_us(&timer_next, offset_us);
    timer_period_us = offset_us;
    return 0;
}

const radio_backend_ops hamlib_backend_ops = {
    .name                    = "hamlib",
    .init                    = radio_hamlib_init,
    .shutdown                = radio_hamlib_shutdown,
    .io_thread               = radio_io_thread,
    .set_frequency           = set_frequency,
    .set_mode                = set_mode,
    .set_txrx_state          = tr_switch,
    .set_bfo                 = set_bfo,
    .set_reflected_threshold = set_reflected_threshold,
    .set_speaker_volume      = set_speaker_volume,
    .set_serial              = set_serial,
    .set_profile_timeout     = set_profile_timeout,
    .set_power_level         = set_power_knob,
    .set_digital_voice       = set_digital_voice,
    .set_step_size           = set_step_size,
    .set_tone_generation     = set_tone_generation,
    .set_profile             = set_profile,
    .get_fwd_power           = get_fwd_power,
    .get_ref_power           = get_ref_power,
    .get_swr                 = get_swr,
};
