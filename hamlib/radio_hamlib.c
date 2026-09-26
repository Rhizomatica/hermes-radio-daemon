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
#include <inttypes.h>
#include <poll.h>
#include <strings.h>

#include <hamlib/rig.h>

#include "radio.h"
#include "radio_hamlib.h"
#include "radio_pipeline.h"
#include "cfg_utils.h"
#include "radio_backend.h"
#include "radio_controls.h"
#include "hamlib_digi.h"

_Atomic bool timer_reset = true;
_Atomic time_t timeout_counter = 0;

static rmode_t mode_to_hamlib(uint16_t mode, bool data_path);
static uint16_t hamlib_to_mode(rmode_t hmode);
static bool profile_data_path(const radio_profile *p);
static pbwidth_t mode_passband(rmode_t hmode, const radio_profile *p);
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

/* Set on a profile/mode change to pause hamlib_poll_vfo_state for a moment so
 * its rig read-back can't write a stale/mid-transition mode into the profile
 * being switched to (apply_profile would then re-apply that wrong mode — the
 * mode used to lag one switch behind). The io thread ticks it back down. */
static _Atomic int vfo_poll_suppress = 0;

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

static bool hamlib_read_level_int(RIG *rig, setting_t level, int *out)
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

    *out = val.i;
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

    /* Mode BEFORE frequency: on the FT-710 a mode change shifts the displayed
     * carrier (~1.4 kHz offset), so the frequency must be asserted last or the
     * dial lands off by that offset.
     * Digital-text modes (CW/RTTY/FT8) are keyed in software as audio tones
     * into the data port; the operator keeps the rig in a DATA/SSB mode, so we
     * must NOT drive the rig into CW/RTTY here (that suppresses the tone). Set
     * frequency only and leave the rig's mode alone. */
    uint16_t pm = radio_h->profiles[profile].mode;
    if (pm != MODE_CW && pm != MODE_RTTY && pm != MODE_FT8)
    {
        rmode_t hm = mode_to_hamlib(pm, profile_data_path(&radio_h->profiles[profile]));
        ret = rig_set_mode(rig, RIG_VFO_CURR, hm,
                           mode_passband(hm, &radio_h->profiles[profile]));
        if (ret != RIG_OK)
            fprintf(stderr, "hamlib_apply_profile: rig_set_mode failed: %s\n",
                    rigerror(ret));
    }

    ret = rig_set_freq(rig, RIG_VFO_CURR, (freq_t) radio_h->profiles[profile].freq);
    if (ret != RIG_OK)
        fprintf(stderr, "hamlib_apply_profile: rig_set_freq failed: %s\n",
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
 *   - RTTY: same logic, PKTUSB on the data path. Our AFSK puts mark above
 *     space in audio (rtty_mark 1585, space 1415), so USB puts mark on the
 *     higher RF frequency, the amateur convention. (LSB is right for the
 *     classic 2125/2295 tones, where mark is the lower audio tone.) This
 *     used PKTLSB, which inverted mark and space and put CW and RTTY on
 *     the other sideband from the sBitx (USB): the two could not hear each
 *     other. If data_path is off the rig's internal FSK keyer is used →
 *     RIG_MODE_RTTY.
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
    case MODE_RTTY: return data_path ? RIG_MODE_PKTUSB : RIG_MODE_RTTY;
    case MODE_DRM:  return RIG_MODE_PKTUSB;   /* digital only, no voice DRM   */
    case MODE_FT8:  return RIG_MODE_PKTUSB;   /* USB worldwide by convention  */
    case MODE_DSTAR: return RIG_MODE_DSTAR; /* rig-native DV (IC-7100 etc.) */
    default:        return data_path ? RIG_MODE_PKTUSB : RIG_MODE_USB;
    }
}

/* Passband to ask for with a rig mode. SSB data modes (PKTUSB/PKTLSB) get
 * the profile's filter_width, or 3 kHz: RIG_PASSBAND_NORMAL gave the
 * IC-7100 a 250 Hz filter centred on 1500 Hz in USB-D, so the data modem
 * (and CW at 700 Hz) heard next to nothing. FM/AM data use filter_width
 * when set and otherwise the rig's normal passband: a 3 kHz default would
 * cut an FM or AM channel. Voice, the rig's own CW/RTTY and D-STAR (rig
 * DV) keep the rig's normal passband. */
static pbwidth_t mode_passband(rmode_t hmode, const radio_profile *p)
{
    if (hmode == RIG_MODE_PKTUSB || hmode == RIG_MODE_PKTLSB)
        return p->filter_width ? (pbwidth_t) p->filter_width : 3000;
    if ((hmode == RIG_MODE_PKTFM || hmode == RIG_MODE_PKTAM) && p->filter_width)
        return (pbwidth_t) p->filter_width;
    return RIG_PASSBAND_NORMAL;
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

    /* Publish the CAT serial lock so the media threads can serialise codec
     * open/close against CAT on a shared USB hub (see radio.h cat_bus_lock). */
    radio_h->cat_bus_lock = &hl_serial_lock;

    int ret = rig_open(rig);
    if (ret != RIG_OK)
    {
        fprintf(stderr, "radio_hamlib_init: rig_open failed: %s\n",
                rigerror(ret));
        rig_cleanup(rig);
        return false;
    }

    /* Unkey first, whatever state the rig is in: a daemon that died while
     * transmitting (crash, SIGKILL, power cut on the Pi) leaves the rig
     * keyed, and nothing else would ever release it. */
    ret = rig_set_ptt(rig, RIG_VFO_CURR, RIG_PTT_OFF);
    if (ret != RIG_OK)
        fprintf(stderr, "radio_hamlib_init: PTT off failed: %s\n",
                rigerror(ret));

    radio_h->rig = (void *) rig;
    radio_h->s_meter_db = -200;   /* no S-meter reading until the first RX poll */

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

static bool radio_hamlib_force_ptt_off(radio *radio_h)
{
    rig_set_debug(RIG_DEBUG_WARN);

    RIG *rig = rig_init(radio_h->hamlib_model);
    if (!rig)
        return false;

    hamlib_configure_ports(rig, radio_h);

    int ret = rig_open(rig);
    if (ret != RIG_OK)
    {
        fprintf(stderr, "radio_hamlib_force_ptt_off: rig_open failed: %s\n",
                rigerror(ret));
        rig_cleanup(rig);
        return false;
    }

    ret = rig_set_ptt(rig, RIG_VFO_CURR, RIG_PTT_OFF);
    if (ret != RIG_OK)
        fprintf(stderr, "radio_hamlib_force_ptt_off: rig_set_ptt failed: %s\n",
                rigerror(ret));

    rig_close(rig);
    rig_cleanup(rig);
    return ret == RIG_OK;
}

static void radio_hamlib_shutdown(radio *radio_h)
{
    hamlib_digi_stop(radio_h);

    if (!radio_h->rig)
        return;

    RIG *rig = (RIG *) radio_h->rig;

    /* Make sure we are in RX before closing, whatever the cached state
     * says (see tr_switch). */
    RIG_LOCK();
    int ret = rig_set_ptt(rig, RIG_VFO_CURR, RIG_PTT_OFF);
    if (ret != RIG_OK)
        fprintf(stderr, "radio_hamlib_shutdown: PTT off failed: %s\n",
                rigerror(ret));

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

    if (profile == radio_h->profile_active_idx)
        vfo_poll_suppress = 15;   /* same read-back race as set_profile */

    *radio_mode = mode;

    /* Apply to rig only when this is the active profile */
    if (profile == radio_h->profile_active_idx && radio_h->rig)
    {
        RIG *rig = (RIG *) radio_h->rig;
        rmode_t hmode = mode_to_hamlib(mode,
                                       profile_data_path(&radio_h->profiles[profile]));
        RIG_LOCK();
        int ret = rig_set_mode(rig, RIG_VFO_CURR, hmode,
                               mode_passband(hmode, &radio_h->profiles[profile]));
        if (ret != RIG_OK)
            fprintf(stderr, "set_mode: rig_set_mode failed: %s\n",
                    rigerror(ret));
        /* The FT-710 shifts the displayed carrier (~1.4 kHz) on a mode change,
         * so a bare USB<->LSB toggle moves the dial. Re-assert the profile's
         * frequency right after the mode (same lock hold) so a mode change
         * alone never changes the frequency. */
        ret = rig_set_freq(rig, RIG_VFO_CURR,
                           (freq_t) radio_h->profiles[profile].freq);
        if (ret != RIG_OK)
            fprintf(stderr, "set_mode: rig_set_freq failed: %s\n",
                    rigerror(ret));
        RIG_UNLOCK();
    }

    char key[64];
    snprintf(key, sizeof(key), "profile%u:mode", profile);
    cfg_set(radio_h, radio_h->cfg_user, key, mode_to_string(mode));
    radio_h->cfg_user_dirty = true;
}

static void tr_switch(radio *radio_h, bool txrx_state)
{
    /* Keying is deduplicated against the cached state, unkeying never is:
     * RX always reaches the rig, as on the sBitx (60148cc). The cache can
     * read IN_RX while the rig transmits (a failed rig_get_ptt keeps the old
     * value, a keying request can race the 200 ms poll), and a skipped
     * PTT-off then leaves the transmitter keyed with every client told it
     * is not. A redundant PTT-off costs one CAT command; a redundant call
     * can never key the radio. */
    if (txrx_state == IN_TX && radio_h->txrx_state == IN_TX)
        return;

    if (txrx_state == IN_RX && radio_h->txrx_state == IN_RX)
        printf("tr_switch: PTT off while already in RX, sending it anyway\n");

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
            fprintf(stderr, "tr_switch: rig_set_ptt(%s) failed: %s\n",
                    txrx_state == IN_TX ? "ON" : "OFF", rigerror(ret));
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
        int ret = rig_set_mode(rig, RIG_VFO_CURR, hmode,
                               mode_passband(hmode, &radio_h->profiles[profile]));
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

    vfo_poll_suppress = 15;   /* freeze read-back ~1.5 s (100 ms io tick) */
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

    /* Preserve a digital-text profile's mode (CW/RTTY/FT8): the digi engine
     * keys off it, while the rig sits in a PKT/SSB mode that reads back as
     * USB/LSB and would silently disable digital TX (radio never keys). Sync
     * frequency only for those; sync both for voice/SSB profiles. */
    uint16_t pmode = radio_h->profiles[profile].mode;
    if (mr == RIG_OK &&
        pmode != MODE_CW && pmode != MODE_RTTY && pmode != MODE_FT8)
        radio_h->profiles[profile].mode = hamlib_to_mode(hmode);
}

/* RX S-meter: hamlib RIG_LEVEL_STRENGTH is the calibrated signal strength in
 * dB relative to S9 (S9 = 0), derived by the backend from the rig's raw meter.
 * RX-only and serialized via the rig lock like every other CAT access. */
static void hamlib_poll_s_meter(radio *radio_h)
{
    RIG *rig = (RIG *) radio_h->rig;
    int strength = 0;

    if (!rig)
        return;

    if (hamlib_read_level_int(rig, RIG_LEVEL_STRENGTH, &strength))
        radio_h->s_meter_db = strength;
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

            /* Sync freq/mode from the rig ~every 200 ms (every 2nd 100 ms
             * tick) so front-panel changes show up in the daemon state with
             * little lag. CAT reads are serialised by the rig lock. Skipped for
             * ~1.5 s right after a profile/mode change so the read-back can't
             * race the switch and write back a stale mode. */
            if (vfo_poll_suppress > 0)
            {
                vfo_poll_suppress--;
            }
            else
            {
                static int vfo_tick = 0;
                if (++vfo_tick >= 2)
                {
                    vfo_tick = 0;
                    hamlib_poll_vfo_state(radio_h);
                }
            }

            /* Poll the RX S-meter ~every 300 ms (every 3rd tick) for a
             * responsive signal readout without flooding the CAT bus. */
            static int smeter_tick = 0;
            if (++smeter_tick >= 3)
            {
                smeter_tick = 0;
                hamlib_poll_s_meter(radio_h);
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


/* ═══════════════════ generic control surface ═══════════════════════════
 *
 * Everything below forwards the daemon's backend-neutral control names
 * (radio_controls.h — which are the Hamlib token names) straight to the
 * open rig. Nothing here is rig-specific: the control set an IC-7300, an
 * IC-7100 or an FT-710 exposes comes from that rig's own Hamlib capability
 * masks, so adding a radio is a matter of Hamlib knowing it, not of code
 * here. Every entry point takes RIG_LOCK so control traffic can never
 * interleave with the meter poll on the CAT wire.
 */

static RIG *hl_rig(radio *radio_h)
{
    return (radio_h && radio_h->rig) ? (RIG *) radio_h->rig : NULL;
}

/* Map a Hamlib return code onto the daemon's backend-neutral codes. */
static int hl_rc(int ret)
{
    switch (ret)
    {
    case RIG_OK:        return RADIO_CTRL_OK;
    case -RIG_ENAVAIL:
    case -RIG_ENIMPL:
    case -RIG_ENTARGET:  return RADIO_CTRL_ENOTSUP;
    case -RIG_EINVAL:
    case -RIG_EDOM:      return RADIO_CTRL_EINVAL;
    default:             return RADIO_CTRL_EIO;
    }
}

static int hl_get_level(radio *radio_h, const char *name, double *out)
{
    RIG *rig = hl_rig(radio_h);
    value_t val;
    setting_t level;
    int ret;

    if (!rig || !name || !*name || !out)
        return RADIO_CTRL_EINVAL;

    level = rig_parse_level(name);
    if (level == RIG_LEVEL_NONE)
        return RADIO_CTRL_EINVAL;
    if (!rig_has_get_level(rig, level))
        return RADIO_CTRL_ENOTSUP;

    memset(&val, 0, sizeof(val));
    RIG_LOCK();
    ret = rig_get_level(rig, RIG_VFO_CURR, level, &val);
    RIG_UNLOCK();
    if (ret != RIG_OK)
        return hl_rc(ret);

    double value = RIG_LEVEL_IS_FLOAT(level) ? (double) val.f : (double) val.i;

    /* A backend can hand back NaN or an infinity when the rig answers a
     * meter read with something it cannot make sense of (a garbled reply,
     * an unset power calibration). Passing that on would print "-nan" to a
     * rigctld client and emit invalid JSON to the websocket, so report it
     * as the read failure it is. (Not isfinite(): this file is built -Ofast,
     * where GCC folds that to true — see radio_controls_value_ok.) */
    if (!radio_controls_value_ok(value))
        return RADIO_CTRL_EIO;

    *out = value;
    return RADIO_CTRL_OK;
}

static int hl_set_level(radio *radio_h, const char *name, double value)
{
    RIG *rig = hl_rig(radio_h);
    value_t val;
    setting_t level;
    int ret;

    if (!rig || !name || !*name)
        return RADIO_CTRL_EINVAL;

    level = rig_parse_level(name);
    if (level == RIG_LEVEL_NONE)
        return RADIO_CTRL_EINVAL;
    if (!rig_has_set_level(rig, level))
        return RADIO_CTRL_ENOTSUP;

    memset(&val, 0, sizeof(val));
    if (RIG_LEVEL_IS_FLOAT(level))
        val.f = (float) value;
    else
        val.i = (int) lrint(value);

    RIG_LOCK();
    ret = rig_set_level(rig, RIG_VFO_CURR, level, val);
    RIG_UNLOCK();

    /* Keep the daemon's own view of the two levels it also tracks per
     * profile in step, so the websocket status and the web UI don't drift
     * away from a change made through rigctld or the rig's front panel. */
    if (ret == RIG_OK)
    {
        uint32_t p = radio_h->profile_active_idx;
        if (p < radio_h->profiles_count)
        {
            if (level == RIG_LEVEL_RFPOWER)
                radio_h->profiles[p].power_level_percentage =
                    (uint16_t) lrint(value * 100.0);
            else if (level == RIG_LEVEL_AF)
                radio_h->profiles[p].speaker_level = (uint32_t) lrint(value * 100.0);
        }
    }

    return hl_rc(ret);
}

static int hl_get_func(radio *radio_h, const char *name, int *out)
{
    RIG *rig = hl_rig(radio_h);
    setting_t func;
    int status = 0, ret;

    if (!rig || !name || !*name || !out)
        return RADIO_CTRL_EINVAL;

    func = rig_parse_func(name);
    if (func == RIG_FUNC_NONE)
        return RADIO_CTRL_EINVAL;
    if (!rig_has_get_func(rig, func))
        return RADIO_CTRL_ENOTSUP;

    RIG_LOCK();
    ret = rig_get_func(rig, RIG_VFO_CURR, func, &status);
    RIG_UNLOCK();
    if (ret != RIG_OK)
        return hl_rc(ret);

    *out = status ? 1 : 0;
    return RADIO_CTRL_OK;
}

static int hl_set_func(radio *radio_h, const char *name, int on)
{
    RIG *rig = hl_rig(radio_h);
    setting_t func;
    int ret;

    if (!rig || !name || !*name)
        return RADIO_CTRL_EINVAL;

    func = rig_parse_func(name);
    if (func == RIG_FUNC_NONE)
        return RADIO_CTRL_EINVAL;
    if (!rig_has_set_func(rig, func))
        return RADIO_CTRL_ENOTSUP;

    RIG_LOCK();
    ret = rig_set_func(rig, RIG_VFO_CURR, func, on ? 1 : 0);
    RIG_UNLOCK();

    return hl_rc(ret);
}

static int hl_get_parm(radio *radio_h, const char *name, double *out)
{
    RIG *rig = hl_rig(radio_h);
    value_t val;
    setting_t parm;
    int ret;

    if (!rig || !name || !*name || !out)
        return RADIO_CTRL_EINVAL;

    parm = rig_parse_parm(name);
    if (parm == RIG_PARM_NONE)
        return RADIO_CTRL_EINVAL;
    if (!rig_has_get_parm(rig, parm))
        return RADIO_CTRL_ENOTSUP;

    memset(&val, 0, sizeof(val));
    RIG_LOCK();
    ret = rig_get_parm(rig, parm, &val);
    RIG_UNLOCK();
    if (ret != RIG_OK)
        return hl_rc(ret);

    double value = RIG_PARM_IS_FLOAT(parm) ? (double) val.f : (double) val.i;
    if (!radio_controls_value_ok(value))
        return RADIO_CTRL_EIO;

    *out = value;
    return RADIO_CTRL_OK;
}

static int hl_set_parm(radio *radio_h, const char *name, double value)
{
    RIG *rig = hl_rig(radio_h);
    value_t val;
    setting_t parm;
    int ret;

    if (!rig || !name || !*name)
        return RADIO_CTRL_EINVAL;

    parm = rig_parse_parm(name);
    if (parm == RIG_PARM_NONE)
        return RADIO_CTRL_EINVAL;
    if (!rig_has_set_parm(rig, parm))
        return RADIO_CTRL_ENOTSUP;

    memset(&val, 0, sizeof(val));
    if (RIG_PARM_IS_FLOAT(parm))
        val.f = (float) value;
    else
        val.i = (int) lrint(value);

    RIG_LOCK();
    ret = rig_set_parm(rig, parm, val);
    RIG_UNLOCK();

    return hl_rc(ret);
}

/* Walk the rig's capability masks and describe every control it has.
 * Purely local (caps + granularity tables), so this costs no CAT traffic
 * and can be answered on every websocket connect. */
static size_t hl_enumerate_controls(radio *radio_h, radio_ctrl_info *out, size_t max)
{
    RIG *rig = hl_rig(radio_h);
    size_t n = 0;

    if (!rig || !rig->caps || !out || max == 0)
        return 0;

    for (int i = 0; i < RIG_SETTING_MAX && n < max; i++)
    {
        setting_t s = (setting_t) 1 << i;
        const char *name = rig_strlevel(s);
        if (!name || !*name)
            continue;
        bool can_get = rig_has_get_level(rig, s) != 0;
        bool can_set = rig_has_set_level(rig, s) != 0;
        if (!can_get && !can_set)
            continue;

        radio_ctrl_info *info = &out[n++];
        memset(info, 0, sizeof(*info));
        snprintf(info->name, sizeof(info->name), "%s", name);
        info->kind = RADIO_CTRL_LEVEL;
        info->is_float = RIG_LEVEL_IS_FLOAT(s) != 0;
        info->can_get = can_get;
        info->can_set = can_set;

        const gran_t *g = &rig->caps->level_gran[i];
        if (info->is_float)
        {
            info->min  = g->min.f;
            info->max  = g->max.f;
            info->step = g->step.f;
            /* Hamlib leaves the gran zeroed for plain 0..1 float levels. */
            if (info->min == 0.0 && info->max == 0.0)
                info->max = 1.0;
        }
        else
        {
            info->min  = g->min.i;
            info->max  = g->max.i;
            info->step = g->step.i;
        }
    }

    for (int i = 0; i < RIG_SETTING_MAX && n < max; i++)
    {
        setting_t s = (setting_t) 1 << i;
        const char *name = rig_strfunc(s);
        if (!name || !*name)
            continue;
        bool can_get = rig_has_get_func(rig, s) != 0;
        bool can_set = rig_has_set_func(rig, s) != 0;
        if (!can_get && !can_set)
            continue;

        radio_ctrl_info *info = &out[n++];
        memset(info, 0, sizeof(*info));
        snprintf(info->name, sizeof(info->name), "%s", name);
        info->kind = RADIO_CTRL_FUNC;
        info->is_float = false;
        info->can_get = can_get;
        info->can_set = can_set;
        info->min = 0.0;
        info->max = 1.0;
        info->step = 1.0;
    }

    for (int i = 0; i < RIG_SETTING_MAX && n < max; i++)
    {
        setting_t s = (setting_t) 1 << i;
        const char *name = rig_strparm(s);
        if (!name || !*name)
            continue;
        bool can_get = rig_has_get_parm(rig, s) != 0;
        bool can_set = rig_has_set_parm(rig, s) != 0;
        if (!can_get && !can_set)
            continue;

        radio_ctrl_info *info = &out[n++];
        memset(info, 0, sizeof(*info));
        snprintf(info->name, sizeof(info->name), "%s", name);
        info->kind = RADIO_CTRL_PARM;
        info->is_float = RIG_PARM_IS_FLOAT(s) != 0;
        info->can_get = can_get;
        info->can_set = can_set;

        const gran_t *g = &rig->caps->parm_gran[i];
        if (info->is_float)
        {
            info->min  = g->min.f;
            info->max  = g->max.f;
            info->step = g->step.f;
            if (info->min == 0.0 && info->max == 0.0)
                info->max = 1.0;
        }
        else
        {
            info->min  = g->min.i;
            info->max  = g->max.i;
            info->step = g->step.i;
        }
    }

    return n;
}

/* ── typed rig state ───────────────────────────────────────────────── */

static int hl_get_vfo(radio *radio_h, char *out, size_t out_len)
{
    RIG *rig = hl_rig(radio_h);
    vfo_t vfo = RIG_VFO_NONE;
    int ret;

    if (!rig || !out || out_len == 0)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_get_vfo(rig, &vfo);
    RIG_UNLOCK();
    if (ret != RIG_OK)
        return hl_rc(ret);

    snprintf(out, out_len, "%s", rig_strvfo(vfo));
    return RADIO_CTRL_OK;
}

static int hl_set_vfo(radio *radio_h, const char *vfo_name)
{
    RIG *rig = hl_rig(radio_h);
    vfo_t vfo;
    int ret;

    if (!rig || !vfo_name || !*vfo_name)
        return RADIO_CTRL_EINVAL;

    vfo = rig_parse_vfo(vfo_name);
    if (vfo == RIG_VFO_NONE)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_set_vfo(rig, vfo);
    RIG_UNLOCK();

    return hl_rc(ret);
}

static int hl_get_split(radio *radio_h, int *on, char *tx_vfo, size_t tx_vfo_len)
{
    RIG *rig = hl_rig(radio_h);
    split_t split = RIG_SPLIT_OFF;
    vfo_t vfo = RIG_VFO_NONE;
    int ret;

    if (!rig || !on)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_get_split_vfo(rig, RIG_VFO_CURR, &split, &vfo);
    RIG_UNLOCK();
    if (ret != RIG_OK)
        return hl_rc(ret);

    *on = (split == RIG_SPLIT_ON) ? 1 : 0;
    if (tx_vfo && tx_vfo_len)
        snprintf(tx_vfo, tx_vfo_len, "%s", rig_strvfo(vfo));

    return RADIO_CTRL_OK;
}

static int hl_set_split(radio *radio_h, int on, const char *tx_vfo)
{
    RIG *rig = hl_rig(radio_h);
    vfo_t vfo = RIG_VFO_B;
    int ret;

    if (!rig)
        return RADIO_CTRL_EINVAL;

    if (tx_vfo && *tx_vfo)
    {
        vfo = rig_parse_vfo(tx_vfo);
        if (vfo == RIG_VFO_NONE)
            return RADIO_CTRL_EINVAL;
    }

    RIG_LOCK();
    ret = rig_set_split_vfo(rig, RIG_VFO_CURR,
                            on ? RIG_SPLIT_ON : RIG_SPLIT_OFF, vfo);
    RIG_UNLOCK();

    return hl_rc(ret);
}

static int hl_get_split_freq(radio *radio_h, uint32_t *hz)
{
    RIG *rig = hl_rig(radio_h);
    freq_t freq = 0;
    int ret;

    if (!rig || !hz)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_get_split_freq(rig, RIG_VFO_CURR, &freq);
    RIG_UNLOCK();
    if (ret != RIG_OK)
        return hl_rc(ret);

    *hz = (uint32_t) freq;
    return RADIO_CTRL_OK;
}

static int hl_set_split_freq(radio *radio_h, uint32_t hz)
{
    RIG *rig = hl_rig(radio_h);
    int ret;

    if (!rig)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_set_split_freq(rig, RIG_VFO_CURR, (freq_t) hz);
    RIG_UNLOCK();

    return hl_rc(ret);
}

static int hl_get_split_mode(radio *radio_h, char *mode, size_t mode_len, uint32_t *width)
{
    RIG *rig = hl_rig(radio_h);
    rmode_t rmode = RIG_MODE_NONE;
    pbwidth_t pb = 0;
    int ret;

    if (!rig || !mode || mode_len == 0)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_get_split_mode(rig, RIG_VFO_CURR, &rmode, &pb);
    RIG_UNLOCK();
    if (ret != RIG_OK)
        return hl_rc(ret);

    snprintf(mode, mode_len, "%s", rig_strrmode(rmode));
    if (width)
        *width = (uint32_t) (pb > 0 ? pb : 0);

    return RADIO_CTRL_OK;
}

static int hl_set_split_mode(radio *radio_h, const char *mode, uint32_t width)
{
    RIG *rig = hl_rig(radio_h);
    rmode_t rmode;
    int ret;

    if (!rig || !mode || !*mode)
        return RADIO_CTRL_EINVAL;

    rmode = rig_parse_mode(mode);
    if (rmode == RIG_MODE_NONE)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_set_split_mode(rig, RIG_VFO_CURR, rmode,
                             width ? (pbwidth_t) width : RIG_PASSBAND_NORMAL);
    RIG_UNLOCK();

    return hl_rc(ret);
}

static int hl_get_rit(radio *radio_h, int32_t *hz)
{
    RIG *rig = hl_rig(radio_h);
    shortfreq_t rit = 0;
    int ret;

    if (!rig || !hz)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_get_rit(rig, RIG_VFO_CURR, &rit);
    RIG_UNLOCK();
    if (ret != RIG_OK)
        return hl_rc(ret);

    *hz = (int32_t) rit;
    return RADIO_CTRL_OK;
}

static int hl_set_rit(radio *radio_h, int32_t hz)
{
    RIG *rig = hl_rig(radio_h);
    int ret;

    if (!rig)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_set_rit(rig, RIG_VFO_CURR, (shortfreq_t) hz);
    RIG_UNLOCK();

    return hl_rc(ret);
}

static int hl_get_xit(radio *radio_h, int32_t *hz)
{
    RIG *rig = hl_rig(radio_h);
    shortfreq_t xit = 0;
    int ret;

    if (!rig || !hz)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_get_xit(rig, RIG_VFO_CURR, &xit);
    RIG_UNLOCK();
    if (ret != RIG_OK)
        return hl_rc(ret);

    *hz = (int32_t) xit;
    return RADIO_CTRL_OK;
}

static int hl_set_xit(radio *radio_h, int32_t hz)
{
    RIG *rig = hl_rig(radio_h);
    int ret;

    if (!rig)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_set_xit(rig, RIG_VFO_CURR, (shortfreq_t) hz);
    RIG_UNLOCK();

    return hl_rc(ret);
}

static int hl_get_width(radio *radio_h, uint32_t *hz)
{
    RIG *rig = hl_rig(radio_h);
    rmode_t rmode = RIG_MODE_NONE;
    pbwidth_t pb = 0;
    int ret;

    if (!rig || !hz)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_get_mode(rig, RIG_VFO_CURR, &rmode, &pb);
    RIG_UNLOCK();
    if (ret != RIG_OK)
        return hl_rc(ret);

    /* Report only. This used to copy the read-back into the profile's
     * filter_width, which is the width radiod asks for on the next mode
     * change: every rigctld "m" poll (WSJT-X, fldigi) or CAT SH/SL query
     * then decided the next data-mode filter from whatever mode the rig
     * was in. filter_width changes only when a width is set (set_width,
     * rigctld "M <mode> <width>"). */
    *hz = (uint32_t) (pb > 0 ? pb : 0);
    return RADIO_CTRL_OK;
}

/* Set the receiver filter passband. Hamlib carries width as the second
 * argument of rig_set_mode, so the current mode is read back first and
 * re-asserted with the new width. */
static int hl_set_width(radio *radio_h, uint32_t hz)
{
    RIG *rig = hl_rig(radio_h);
    rmode_t rmode = RIG_MODE_NONE;
    pbwidth_t pb = 0;
    int ret;

    if (!rig)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_get_mode(rig, RIG_VFO_CURR, &rmode, &pb);
    if (ret == RIG_OK)
        ret = rig_set_mode(rig, RIG_VFO_CURR, rmode,
                           hz ? (pbwidth_t) hz : RIG_PASSBAND_NORMAL);
    RIG_UNLOCK();

    if (ret == RIG_OK)
    {
        uint32_t p = radio_h->profile_active_idx;
        if (p < radio_h->profiles_count)
            radio_h->profiles[p].filter_width = hz;
    }

    return hl_rc(ret);
}

/* The rig's own mode name, so a client that asked for PKTUSB reads PKTUSB
 * back rather than the daemon's internal MODE_USB flattened to "USB". */
static int hl_get_mode_name(radio *radio_h, char *out, size_t out_len, uint32_t *width)
{
    RIG *rig = hl_rig(radio_h);
    rmode_t rmode = RIG_MODE_NONE;
    pbwidth_t pb = 0;
    int ret;

    if (!rig || !out || out_len == 0)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_get_mode(rig, RIG_VFO_CURR, &rmode, &pb);
    RIG_UNLOCK();
    if (ret != RIG_OK)
        return hl_rc(ret);

    snprintf(out, out_len, "%s", rig_strrmode(rmode));
    if (width)
        *width = (uint32_t) (pb > 0 ? pb : 0);

    return RADIO_CTRL_OK;
}

/* Set exactly the mode the client named. The daemon's internal MODE_* is
 * refreshed from it so the web UI and status frames follow, but the
 * profile's operating_mode (which selects the daemon's own audio routing)
 * is deliberately left alone — a logger changing the rig's submode must not
 * silently re-route the station's audio. */
static int hl_set_mode_name(radio *radio_h, const char *mode, uint32_t width)
{
    RIG *rig = hl_rig(radio_h);
    rmode_t rmode;
    int ret;

    if (!rig || !mode || !*mode)
        return RADIO_CTRL_EINVAL;

    rmode = rig_parse_mode(mode);
    if (rmode == RIG_MODE_NONE)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_set_mode(rig, RIG_VFO_CURR, rmode,
                       width ? (pbwidth_t) width : RIG_PASSBAND_NORMAL);
    RIG_UNLOCK();
    if (ret != RIG_OK)
        return hl_rc(ret);

    uint32_t p = radio_h->profile_active_idx;
    if (p < radio_h->profiles_count)
    {
        radio_h->profiles[p].mode = hamlib_to_mode(rmode);
        if (width)
            radio_h->profiles[p].filter_width = width;
    }

    /* The read-back poll must not overwrite this with a mid-transition
     * mode, exactly as on a profile switch. */
    vfo_poll_suppress = 5;

    return RADIO_CTRL_OK;
}

static int hl_get_ant(radio *radio_h, int *ant)
{
    RIG *rig = hl_rig(radio_h);
    ant_t ant_curr = RIG_ANT_NONE, ant_tx = RIG_ANT_NONE, ant_rx = RIG_ANT_NONE;
    value_t option;
    int ret;

    if (!rig || !ant)
        return RADIO_CTRL_EINVAL;

    memset(&option, 0, sizeof(option));
    RIG_LOCK();
    ret = rig_get_ant(rig, RIG_VFO_CURR, RIG_ANT_CURR, &option,
                      &ant_curr, &ant_tx, &ant_rx);
    RIG_UNLOCK();
    if (ret != RIG_OK)
        return hl_rc(ret);

    /* Hamlib reports the antenna as a bit in an ant_t mask; clients speak
     * the 1-based antenna number, which is that bit's index + 1. */
    *ant = 0;
    for (int i = 0; i < 32; i++)
    {
        if (ant_curr & ((ant_t) 1 << i))
        {
            *ant = i + 1;
            break;
        }
    }

    return RADIO_CTRL_OK;
}

static int hl_set_ant(radio *radio_h, int ant)
{
    RIG *rig = hl_rig(radio_h);
    value_t option;
    int ret;

    if (!rig || ant < 1 || ant > 32)
        return RADIO_CTRL_EINVAL;

    memset(&option, 0, sizeof(option));
    RIG_LOCK();
    ret = rig_set_ant(rig, RIG_VFO_CURR, (ant_t) 1 << (ant - 1), option);
    RIG_UNLOCK();

    return hl_rc(ret);
}

static int hl_get_mem(radio *radio_h, int *ch)
{
    RIG *rig = hl_rig(radio_h);
    int ret;

    if (!rig || !ch)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_get_mem(rig, RIG_VFO_CURR, ch);
    RIG_UNLOCK();

    return hl_rc(ret);
}

static int hl_set_mem(radio *radio_h, int ch)
{
    RIG *rig = hl_rig(radio_h);
    int ret;

    if (!rig)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_set_mem(rig, RIG_VFO_CURR, ch);
    RIG_UNLOCK();

    return hl_rc(ret);
}

static int hl_get_powerstat(radio *radio_h, int *on)
{
    RIG *rig = hl_rig(radio_h);
    powerstat_t status = RIG_POWER_OFF;
    int ret;

    if (!rig || !on)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_get_powerstat(rig, &status);
    RIG_UNLOCK();
    if (ret != RIG_OK)
        return hl_rc(ret);

    *on = (int) status;
    return RADIO_CTRL_OK;
}

static int hl_set_powerstat(radio *radio_h, int on)
{
    RIG *rig = hl_rig(radio_h);
    int ret;

    if (!rig)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_set_powerstat(rig, on ? RIG_POWER_ON : RIG_POWER_OFF);
    RIG_UNLOCK();

    return hl_rc(ret);
}

static int hl_vfo_op(radio *radio_h, const char *op_name)
{
    RIG *rig = hl_rig(radio_h);
    vfo_op_t op;
    int ret;

    if (!rig || !op_name || !*op_name)
        return RADIO_CTRL_EINVAL;

    op = rig_parse_vfo_op(op_name);
    if (op == RIG_OP_NONE)
        return RADIO_CTRL_EINVAL;
    if (!rig_has_vfo_op(rig, op))
        return RADIO_CTRL_ENOTSUP;

    RIG_LOCK();
    ret = rig_vfo_op(rig, RIG_VFO_CURR, op);
    RIG_UNLOCK();

    /* An antenna tuner cycle (RIG_OP_TUNE) keys the rig for a few seconds;
     * let the read-back poll settle rather than latch a mid-tune state. */
    if (ret == RIG_OK && op == RIG_OP_TUNE)
        vfo_poll_suppress = 50;

    return hl_rc(ret);
}

static int hl_send_morse(radio *radio_h, const char *text)
{
    RIG *rig = hl_rig(radio_h);
    int ret;

    if (!rig || !text || !*text)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_send_morse(rig, RIG_VFO_CURR, text);
    RIG_UNLOCK();

    return hl_rc(ret);
}

static int hl_stop_morse(radio *radio_h)
{
    RIG *rig = hl_rig(radio_h);
    int ret;

    if (!rig)
        return RADIO_CTRL_EINVAL;

    RIG_LOCK();
    ret = rig_stop_morse(rig, RIG_VFO_CURR);
    RIG_UNLOCK();

    return hl_rc(ret);
}

/* Build the rigctld \dump_state payload from the rig Hamlib actually
 * opened, so a remote WSJT-X / fldigi / VARA sees this rig's real
 * frequency ranges, modes, filters and level masks instead of a
 * hardcoded guess. Protocol version 0 — the classic block every
 * NET rigctl client understands.
 *
 * Frequency ranges live per ITU region in caps (list1 = region 1,
 * list2 = region 2); most backends fill both, so we publish whichever
 * is populated and report the matching region number. */
/* ── byte-transparent CAT passthrough ──────────────────────────────────
 *
 * This is what lets a Windows logger that only knows how to open a COM port
 * (N1MM+ among them) drive the real radio across the network: cat_server.c
 * hands us a frame exactly as the client wrote it, we put it on the rig's
 * own serial port and hand back whatever the rig said, byte for byte. No
 * interpretation, so rig-specific commands the daemon knows nothing about
 * work as they do on a direct cable.
 *
 * The transaction runs under the same lock as every other CAT access here,
 * so a passthrough frame can never interleave with the meter poll or a
 * websocket command on the wire. We drive the port fd directly rather than
 * through rig_send_raw() because a "set" command draws no reply at all, and
 * rig_send_raw() would spend the rig's full timeout waiting for one on every
 * such command — unusable for a logger polling several times a second.
 * Reads therefore use a short first-byte wait and an even shorter idle gap.
 */

#define HL_CAT_FIRST_BYTE_MS 250   /* default; core.ini cat_reply_timeout_ms */
#define HL_CAT_IDLE_MS        30   /* gap that ends a reply once it started */

static uint8_t hl_cat_terminator(radio *radio_h)
{
    RIG *rig = hl_rig(radio_h);
    hamlib_port_t *port;

    if (!rig || !rig->caps)
        return 0;

    /* Answering with a terminator is what tells the gateway that raw CAT is
     * available here, so it must be a real, open serial port. A network or
     * "none" port (the dummy rig, an rpc backend) has no wire to pass bytes
     * through, and the gateway must fall back to emulation instead. */
    if (rig->caps->port_type != RIG_PORT_SERIAL)
        return 0;

    port = HAMLIB_RIGPORT(rig);
    if (!port || port->fd < 0)
        return 0;

    /* Icom's CI-V frames end with 0xFD; the Yaesu/Kenwood/Elecraft family
     * of ASCII dialects ends with ';'. */
    if (rig->caps->mfg_name && !strcasecmp(rig->caps->mfg_name, "Icom"))
        return 0xFD;

    return ';';
}

static int hl_cat_raw(radio *radio_h, const uint8_t *req, size_t req_len,
                      uint8_t *reply, size_t reply_max, size_t *reply_len)
{
    RIG *rig = hl_rig(radio_h);
    hamlib_port_t *port;
    uint8_t term;
    size_t got = 0;
    int fd;

    if (!rig || !req || req_len == 0 || !reply || !reply_len)
        return RADIO_CTRL_EINVAL;

    port = HAMLIB_RIGPORT(rig);
    if (!port || port->fd < 0)
        return RADIO_CTRL_ENOTSUP;

    fd = port->fd;
    term = hl_cat_terminator(radio_h);
    *reply_len = 0;

    RIG_LOCK();

    /* Drop anything left over from an earlier transaction so the client
     * cannot be handed a stale rig answer. */
    for (;;)
    {
        uint8_t drain[64];
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) <= 0)
            break;
        if (read(fd, drain, sizeof(drain)) <= 0)
            break;
    }

    size_t sent = 0;
    while (sent < req_len)
    {
        ssize_t w = write(fd, req + sent, req_len - sent);
        if (w <= 0)
        {
            if (w < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            RIG_UNLOCK();
            return RADIO_CTRL_EIO;
        }
        sent += (size_t) w;
    }

    while (got < reply_max)
    {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int first_ms = radio_h->cat_reply_timeout_ms > 0
                     ? radio_h->cat_reply_timeout_ms : HL_CAT_FIRST_BYTE_MS;
        int timeout = (got == 0) ? first_ms : HL_CAT_IDLE_MS;
        int pr = poll(&pfd, 1, timeout);

        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            break;   /* no reply (a set command) or the rig finished talking */

        ssize_t r = read(fd, reply + got, reply_max - got);
        if (r <= 0)
        {
            if (r < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            break;
        }
        got += (size_t) r;

        if (term && reply[got - 1] == term)
            break;
    }

    RIG_UNLOCK();

    *reply_len = got;
    return RADIO_CTRL_OK;
}

static int hl_dump_state(radio *radio_h, char *out, size_t out_len)
{
    RIG *rig = hl_rig(radio_h);
    size_t off = 0;
    int region = 1;

    if (!rig || !rig->caps || !out || out_len == 0)
        return RADIO_CTRL_ENOTSUP;

    const struct rig_caps *caps = rig->caps;
    const freq_range_t *rx = caps->rx_range_list1;
    const freq_range_t *tx = caps->tx_range_list1;

    if (RIG_IS_FRNG_END(rx[0]))
    {
        rx = caps->rx_range_list2;
        tx = caps->tx_range_list2;
        region = 2;
    }

#define DS_APPEND(...)                                                        \
    do {                                                                      \
        int _n = snprintf(out + off, out_len - off, __VA_ARGS__);              \
        if (_n < 0 || (size_t) _n >= out_len - off)                           \
            return RADIO_CTRL_EIO;                                            \
        off += (size_t) _n;                                                   \
    } while (0)

    DS_APPEND("0\n");                       /* protocol version */
    DS_APPEND("%u\n", (unsigned) caps->rig_model);
    DS_APPEND("%d\n", region);

    for (int i = 0; i < HAMLIB_FRQRANGESIZ && !RIG_IS_FRNG_END(rx[i]); i++)
        DS_APPEND("%.0f %.0f 0x%" PRIx64 " %d %d 0x%x 0x%x\n",
                  rx[i].startf, rx[i].endf, (uint64_t) rx[i].modes,
                  rx[i].low_power, rx[i].high_power,
                  (unsigned) rx[i].vfo, (unsigned) rx[i].ant);
    DS_APPEND("0 0 0 0 0 0 0\n");

    for (int i = 0; i < HAMLIB_FRQRANGESIZ && !RIG_IS_FRNG_END(tx[i]); i++)
        DS_APPEND("%.0f %.0f 0x%" PRIx64 " %d %d 0x%x 0x%x\n",
                  tx[i].startf, tx[i].endf, (uint64_t) tx[i].modes,
                  tx[i].low_power, tx[i].high_power,
                  (unsigned) tx[i].vfo, (unsigned) tx[i].ant);
    DS_APPEND("0 0 0 0 0 0 0\n");

    for (int i = 0; i < HAMLIB_TSLSTSIZ && !RIG_IS_TS_END(caps->tuning_steps[i]); i++)
        DS_APPEND("0x%" PRIx64 " %ld\n",
                  (uint64_t) caps->tuning_steps[i].modes,
                  (long) caps->tuning_steps[i].ts);
    DS_APPEND("0 0\n");

    for (int i = 0; i < HAMLIB_FLTLSTSIZ && !RIG_IS_FLT_END(caps->filters[i]); i++)
        DS_APPEND("0x%" PRIx64 " %ld\n",
                  (uint64_t) caps->filters[i].modes,
                  (long) caps->filters[i].width);
    DS_APPEND("0 0\n");

    DS_APPEND("%ld\n", (long) caps->max_rit);
    DS_APPEND("%ld\n", (long) caps->max_xit);
    DS_APPEND("%ld\n", (long) caps->max_ifshift);
    DS_APPEND("%d\n", (int) caps->announces);

    for (int i = 0; i < HAMLIB_MAXDBLSTSIZ && caps->preamp[i] != 0; i++)
        DS_APPEND("%d ", caps->preamp[i]);
    DS_APPEND("0\n");

    for (int i = 0; i < HAMLIB_MAXDBLSTSIZ && caps->attenuator[i] != 0; i++)
        DS_APPEND("%d ", caps->attenuator[i]);
    DS_APPEND("0\n");

    DS_APPEND("0x%" PRIx64 "\n", (uint64_t) caps->has_get_func);
    DS_APPEND("0x%" PRIx64 "\n", (uint64_t) caps->has_set_func);
    DS_APPEND("0x%" PRIx64 "\n", (uint64_t) caps->has_get_level);
    DS_APPEND("0x%" PRIx64 "\n", (uint64_t) caps->has_set_level);
    DS_APPEND("0x%" PRIx64 "\n", (uint64_t) caps->has_get_parm);
    DS_APPEND("0x%" PRIx64 "\n", (uint64_t) caps->has_set_parm);

#undef DS_APPEND

    return RADIO_CTRL_OK;
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

    .get_level               = hl_get_level,
    .set_level               = hl_set_level,
    .get_func                = hl_get_func,
    .set_func                = hl_set_func,
    .get_parm                = hl_get_parm,
    .set_parm                = hl_set_parm,
    .enumerate_controls      = hl_enumerate_controls,

    .get_vfo                 = hl_get_vfo,
    .set_vfo                 = hl_set_vfo,
    .get_split               = hl_get_split,
    .set_split               = hl_set_split,
    .get_split_freq          = hl_get_split_freq,
    .set_split_freq          = hl_set_split_freq,
    .get_split_mode          = hl_get_split_mode,
    .set_split_mode          = hl_set_split_mode,
    .get_rit                 = hl_get_rit,
    .set_rit                 = hl_set_rit,
    .get_xit                 = hl_get_xit,
    .set_xit                 = hl_set_xit,
    .get_mode_name           = hl_get_mode_name,
    .set_mode_name           = hl_set_mode_name,
    .get_width               = hl_get_width,
    .set_width               = hl_set_width,
    .get_ant                 = hl_get_ant,
    .set_ant                 = hl_set_ant,
    .get_mem                 = hl_get_mem,
    .set_mem                 = hl_set_mem,
    .get_powerstat           = hl_get_powerstat,
    .set_powerstat           = hl_set_powerstat,
    .vfo_op                  = hl_vfo_op,
    .send_morse              = hl_send_morse,
    .stop_morse              = hl_stop_morse,
    .cat_raw                 = hl_cat_raw,
    .cat_terminator          = hl_cat_terminator,
    .dump_state              = hl_dump_state,
    .force_ptt_off           = radio_hamlib_force_ptt_off,
};
