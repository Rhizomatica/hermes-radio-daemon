/* HERMES sbitx controller
 *
 * Copyright (C) 2023-2024 Rhizomatica
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
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <sys/time.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <math.h>

#include "cfg_utils.h"
#include "sbitx_core.h"
#include "sbitx_i2c.h"
#include "sbitx_gpio.h"
#include "sbitx_si5351.h"
#include "sbitx_alsa.h"
#include "sbitx_dsp.h"
#include "../radio_backend.h"
#include "../radio_controls.h"

extern _Atomic bool shutdown_;
extern _Atomic bool tx_starting;
extern _Atomic bool rx_starting;


/* sbitx-side profile-fallback timer state. The hamlib backend has its own
 * sbitx_timer_reset / sbitx_timeout_counter — both are file-local statics so they don't
 * collide at link time. */
static _Atomic bool   sbitx_timer_reset    = true;
static _Atomic time_t sbitx_timeout_counter = 0;

/* Forward declarations for the file-local backend ops used by other backend
 * functions in this TU (e.g. set_profile calls set_frequency / set_mode,
 * swr_protection_check calls tr_switch). */
static void set_frequency(radio *radio_h, uint32_t frequency, uint32_t profile);
static void set_mode(radio *radio_h, uint16_t mode, uint32_t profile);
static void tr_switch(radio *radio_h, bool txrx_state);

void radio_apply_defaults(radio *radio_h)
{
    if (radio_h->hw_profile == HW_PROFILE_UNKNOWN)
        radio_h->hw_profile = HW_PROFILE_SBITX;

    if (!radio_h->bfo_frequency)
        radio_h->bfo_frequency = radio_is_zbitx(radio_h) ? ZBITX_BFO_FREQUENCY : SBITX_BFO_FREQUENCY;
}

bool radio_is_zbitx(const radio *radio_h)
{
    return radio_h->hw_profile == HW_PROFILE_ZBITX;
}

bool hw_init(radio *radio_h, pthread_t *hw_tids)
{
    radio_apply_defaults(radio_h);
    // I2C SETUP
    i2c_open(radio_h);

    // GPIO SETUP
    gpio_init(radio_h);

    // Si5351 SETUP
    setup_oscillators(radio_h);


    // start hw io monitor thread, ref/pwr readings, volume and freq changes
    pthread_create(&hw_tids[0], NULL, hw_thread, (void *) radio_h);

    // thread that just polls the gpios
    pthread_create(&hw_tids[1], NULL, do_gpio_poll, (void *) radio_h);

    return true;
}


bool hw_shutdown(radio *radio_h, pthread_t *hw_tids)
{
    pthread_join(hw_tids[0], NULL);

    pthread_join(hw_tids[1], NULL);

    /* Leave the transmitter off whatever txrx_state says. */
    gpio_tx_off();

    i2c_close(radio_h);

    return true;
}

void *hw_thread(void *radio_h_v)
{
    radio *radio_h = (radio *) radio_h_v;

    // starts our 10ms timer
    int res = start_periodic_timer(10000);

    if (res < 0)
    {
        printf("Fatal error: Start Periodic Timer\n");
        shutdown_ = true;
        return false;
    }

    while(!shutdown_)
    {
        wait_next_activation();
        io_tick(radio_h);
    }

    return NULL;
}

// reads the power measurements from I2C bus
bool update_power_measurements(radio *radio_h)
{
    uint8_t response[4];
    uint16_t vfwd, vref;

    int count = i2c_read_pwr_levels(radio_h, response);

    if(count != 4)
        return false;

    memcpy(&vfwd, response, 2);
    memcpy(&vref, response+2, 2);

    radio_h->fwd_power = vfwd;
    radio_h->ref_power = vref;

    return true;
}

// returns power * 10
static uint32_t get_fwd_power(radio *radio_h)
{
    // 40 should be we are using 40W as end of scale
    uint32_t fwdvoltage =  (radio_h->fwd_power * 40) / radio_h->bridge_compensation;
    uint32_t fwdpower = (fwdvoltage * fwdvoltage) / 400;

    return fwdpower;
}

uint32_t get_ref_power(radio *radio_h)
{
	uint32_t refvoltage =  (radio_h->ref_power * 40) / radio_h->bridge_compensation;
	uint32_t refpower = (refvoltage * refvoltage) / 400;

    return refpower;

}

static uint32_t get_swr(radio *radio_h)
{
    uint32_t vfwd = radio_h->fwd_power;
    uint32_t vref = radio_h->ref_power;
    uint32_t vswr;

    // no division by zero
    if (vref == vfwd)
        vfwd++;

    if (vref > vfwd)
		vswr = 100;
	else
		vswr = (10*(vfwd + vref))/(vfwd-vref);

    return vswr;
}

static void set_reflected_threshold(radio *radio_h, uint32_t ref_threshold)
{
    if (ref_threshold == radio_h->reflected_threshold)
        return;

    radio_h->reflected_threshold = ref_threshold;

    char tmp[64];
    sprintf(tmp, "%u", radio_h->reflected_threshold);
    int rc = cfg_set(radio_h, radio_h->cfg_radio, "main:reflected_threshold", tmp);
    if (rc != 0)
        printf("Error modifying config file\n");

    radio_h->cfg_radio_dirty = true;
}

static void set_power_knob(radio *radio_h, uint16_t power_level, uint32_t profile)
{
    if (profile > radio_h->profiles_count)
    {
        printf("Error: Profile index out of bounds\n");
        return;
    }

    _Atomic uint16_t *power_level_percentage = &radio_h->profiles[profile].power_level_percentage;

    if (*power_level_percentage == power_level)
        return;

    if (power_level > 100)
        power_level = 100;
    if (power_level < 0)
        power_level = 0;

    radio_h->profiles[profile].power_level_percentage = power_level;

    char tmp1[64]; char tmp2[64];
    sprintf(tmp1, "profile%u:power_level_percentage", profile);
    sprintf(tmp2, "%u", *power_level_percentage);
    int rc = cfg_set(radio_h, radio_h->cfg_user, tmp1, tmp2);
    if (rc != 0)
        printf("Error modifying config file\n");

    radio_h->cfg_user_dirty = true;
}

static void set_digital_voice(radio *radio_h, bool digital_voice, uint32_t profile)
{
    if (profile > radio_h->profiles_count)
    {
        printf("Error: Profile index out of bounds\n");
        return;
    }

    _Atomic bool *dv = &radio_h->profiles[profile].digital_voice;

    if (*dv == digital_voice)
        return;

    radio_h->profiles[profile].digital_voice = digital_voice;

    char tmp1[64]; char tmp2[64];
    sprintf(tmp1, "profile%u:digital_voice", profile);
    sprintf(tmp2, "%d", digital_voice ? 1 : 0);
    int rc = cfg_set(radio_h, radio_h->cfg_user, tmp1, tmp2);
    if (rc != 0)
        printf("Error modifying config file\n");

    radio_h->cfg_user_dirty = true;
}

static void set_profile(radio *radio_h, uint32_t profile)
{
    if (radio_h->profile_active_idx == profile)
        return;

    radio_h->profile_active_idx = profile;

    // set the frequency and mode
    set_frequency(radio_h, radio_h->profiles[profile].freq, profile);
    set_mode(radio_h, radio_h->profiles[profile].mode, profile);

    // this sets the bpf
    dsp_set_filters();

    // and now the alsa levels
    if (radio_h->txrx_state == IN_TX)
    {
        set_speaker_level(0);
        set_tx_level(radio_h->profiles[profile].tx_level);
    }
    else
    {
        set_speaker_level(radio_h->profiles[profile].speaker_level);
        set_tx_level(0);
    }
    set_mic_level(radio_h->profiles[profile].mic_level);
    set_rx_level(radio_h->profiles[profile].rx_level);

    // and save the new current_profile
    char tmp[64];
    sprintf(tmp, "%u", radio_h->profile_active_idx);
    int rc = cfg_set(radio_h, radio_h->cfg_user, "main:current_profile", tmp);
    if (rc != 0)
        printf("Error modifying config file\n");

    radio_h->cfg_user_dirty = true;
}

static void set_speaker_volume(radio *radio_h, uint32_t speaker_level, uint32_t profile)
{
    _Atomic uint32_t *volume = &radio_h->profiles[profile].speaker_level;

    if (*volume == speaker_level)
        return;

    radio_h->profiles[profile].speaker_level = speaker_level;

    if (profile == radio_h->profile_active_idx)
    {
        set_speaker_level(speaker_level);
    }

    char tmp1[64]; char tmp2[64];
    sprintf(tmp1, "profile%u:speaker_level", profile);
    sprintf(tmp2, "%u", *volume);
    int rc = cfg_set(radio_h, radio_h->cfg_user, tmp1, tmp2);
    if (rc != 0)
        printf("Error modifying config file\n");

    radio_h->cfg_user_dirty = true;
}

static void set_serial(radio *radio_h, uint32_t serial)
{
    if (serial == radio_h->serial_number)
        return;

    radio_h->serial_number = serial;

    char tmp[64];
    sprintf(tmp, "%u", radio_h->serial_number);
    int rc = cfg_set(radio_h, radio_h->cfg_radio, "main:serial_number", tmp);
    if (rc != 0)
        printf("Error modifying config file\n");

    radio_h->cfg_radio_dirty = true;
}

static void set_profile_timeout(radio *radio_h, int32_t timeout)
{
    if (timeout == radio_h->profile_timeout)
        return;

    radio_h->profile_timeout = timeout;

    char tmp[64];
    sprintf(tmp, "%d", radio_h->profile_timeout);
    int rc = cfg_set(radio_h, radio_h->cfg_user, "main:default_profile_fallback_timeout", tmp);
    if (rc != 0)
        printf("Error modifying config file\n");

    radio_h->cfg_user_dirty = true;
}

static void set_frequency(radio *radio_h, uint32_t frequency, uint32_t profile)
{
    _Atomic uint32_t *radio_freq = &radio_h->profiles[profile].freq;

    if ( (frequency > 30000000) || (frequency < 500000) )
        return;

    /* Whether the CACHED value moved. This used to gate the whole function,
     * which had two consequences:
     *
     *  - set_profile() calls us with the frequency the profile already holds,
     *    so the early return fired and switching profiles never actually
     *    retuned the synthesiser. The radio stayed on the previous profile's
     *    frequency while every interface reported the new one.
     *
     *  - Once the synthesiser diverged from our cache -- another process
     *    driving the same hardware, which happens on a bench -- asking for the
     *    frequency the cache already held did nothing, so the divergence could
     *    not be corrected through the API at all. Recovery needed a detour to
     *    a different frequency and back, which is not something a caller
     *    should have to know.
     *
     * So program the hardware whenever the call targets the active profile,
     * and use this flag only to decide whether the config needs rewriting.
     * Redundant programming is cheap and rare: the callers are operator
     * actions (websocket, SHM, CAT, rigctld), the tuning knob (which only
     * calls us when it has actually moved), and set_profile. */
    const bool value_changed = (*radio_freq != frequency);

    *radio_freq = frequency;

    if (profile == radio_h->profile_active_idx)
    {
        if (radio_h->profiles[radio_h->profile_active_idx].operating_mode == OPERATING_MODE_CONTROLS_ONLY)
            si5351bx_setfreq(2, *radio_freq + radio_h->bfo_frequency - 15000); // here we set the real frequency of the radio (in USB, which is the current setup) - 15000 which is the carrier offset in Mercury in sbitx mode
        else
            si5351bx_setfreq(2, *radio_freq + radio_h->bfo_frequency - 24000); // 24 kHz offset to provide the user the "real" dial frequency after the DSP processing (just "- 24000")
    }

    if (!value_changed)
        return;             /* hardware re-asserted above; nothing to persist */

    char tmp1[64]; char tmp2[64];
    sprintf(tmp1, "profile%u:freq", profile);
    sprintf(tmp2, "%u", *radio_freq);
    int rc = cfg_set(radio_h, radio_h->cfg_user, tmp1, tmp2);
    if (rc != 0)
        printf("Error modifying config file\n");

    radio_h->cfg_user_dirty = true;
}

static void set_mode(radio *radio_h, uint16_t mode, uint32_t profile)
{
    _Atomic uint16_t *radio_mode = &radio_h->profiles[profile].mode;

    /* Same reasoning as set_frequency(): apply to the hardware/DSP whenever the
     * call targets the active profile, and use the cached comparison only to
     * decide whether the config needs rewriting. set_profile() passes the mode
     * the profile already holds, so gating on the cache meant the filters were
     * never re-tuned for it -- harmless only because set_profile() happens to
     * call dsp_set_filters() itself straight afterwards. Relying on that is
     * fragile, and any other caller asking for the mode already cached got
     * nothing applied at all. */
    const bool value_changed = (*radio_mode != mode);

    *radio_mode = mode;

    if (profile == radio_h->profile_active_idx)
    {
        dsp_set_filters();
    }

    if (!value_changed)
        return;             /* filters re-applied above; nothing to persist */

    char tmp1[64];
    const char *mode_str;
    switch (mode)
    {
    case MODE_USB:  mode_str = "USB";  break;
    case MODE_LSB:  mode_str = "LSB";  break;
    case MODE_CW:   mode_str = "CW";   break;
    case MODE_FM:   mode_str = "FM";   break;
    case MODE_AM:   mode_str = "AM";   break;
    case MODE_DRM:  mode_str = "DRM";  break;
    case MODE_FT8:  mode_str = "FT8";  break;
    case MODE_RTTY: mode_str = "RTTY"; break;
    /* MODE_DSTAR was missing here while cfg_utils.c happily PARSES "DSTAR", so
     * selecting D-STAR silently persisted "USB": the mode survived until the
     * next restart and then quietly reverted. Keep this switch in step with the
     * parser in cfg_utils.c. */
    case MODE_DSTAR: mode_str = "DSTAR"; break;
    default:        mode_str = "USB";  break;
    }
    sprintf(tmp1, "profile%u:mode", profile);
    int rc = cfg_set(radio_h, radio_h->cfg_user, tmp1, mode_str);
    if (rc != 0)
        printf("Error modifying config file\n");

    radio_h->cfg_user_dirty = true;
}


static void set_bfo(radio *radio_h, uint32_t frequency)
{
    if (frequency == radio_h->bfo_frequency)
        return;

    radio_h->bfo_frequency = frequency;
    si5351bx_setfreq(1, radio_h->bfo_frequency);

    char tmp[64];
    sprintf(tmp, "%u", radio_h->bfo_frequency);
    int rc = cfg_set(radio_h, radio_h->cfg_radio, "main:bfo", tmp);
    if (rc != 0)
        printf("Error modifying config file\n");

    radio_h->cfg_radio_dirty = true;
}


void lpf_off(radio *radio_h)
{
    set_drive(LPF_A, DRIVE_LOW);
    set_drive(LPF_B, DRIVE_LOW);
    set_drive(LPF_C, DRIVE_LOW);
    set_drive(LPF_D, DRIVE_LOW);
    if (radio_is_zbitx(radio_h))
        set_drive(ZBITX_LPF_E, DRIVE_LOW);
}

void lpf_set(radio *radio_h)
{
    _Atomic uint32_t *radio_freq = &radio_h->profiles[radio_h->profile_active_idx].freq;

    int lpf = 0;

    if (*radio_freq < 5700000)
        lpf = LPF_D;
    else if (*radio_freq < 8000000)
        lpf = LPF_C;
    else if (*radio_freq < 18500000)
        lpf = LPF_B;
    else if (*radio_freq < 35000000)
        lpf = LPF_A;

    if (lpf)
        set_drive(lpf, DRIVE_HIGH);
}

/* Set around the SWR trip: unkey at once, without playing out the tail. */
static _Atomic bool tr_switch_urgent = false;

void swr_protection_check(radio *radio_h)
{
    uint32_t vswr = get_swr(radio_h);

    static uint16_t peak_removal_counter = 0;

    if (vswr > radio_h->reflected_threshold && radio_h->ref_power)
        peak_removal_counter++;
    else
        peak_removal_counter = 0;

    if (peak_removal_counter > REF_PEAK_REMOVAL)
    {
        tr_switch_urgent = true;
        tr_switch(radio_h, IN_RX);
        tr_switch_urgent = false;
        radio_h->swr_protection_enabled = true;
        peak_removal_counter = 0;
        radio_h->send_ws_update = true;
        radio_h->tone_generation = 0;
    }
}

// TODO: all DSP and ALSA calls here or tr_switch? or not? lets keep things separate?
static void tr_switch(radio *radio_h, bool txrx_state)
{
    if (txrx_state == radio_h->txrx_state)
    {
        /* Nothing to sequence, but for RX still drive the transmit line low
         * rather than trusting the cached flag. If the two ever disagree --
         * a process killed mid-transmission leaves TX_LINE high, and a fresh
         * one starts with txrx_state == IN_RX -- the old unconditional return
         * made RX impossible to command: ptt_off answered NOK because the
         * flag already read IN_RX while the hardware stayed keyed. Asserting
         * the safe direction is idempotent and costs one GPIO write.
         *
         * The TX direction is deliberately NOT asserted here: a redundant
         * call must never be able to key the radio. */
        if (txrx_state == IN_RX)
            set_drive(TX_LINE, DRIVE_LOW);
        return;
    }

    if (radio_h->swr_protection_enabled)
    {
        printf("Warning: tx_on trigger with SWR protection on, not turning tx on\n");
        return;
    }

    // TODO: put down the rx_level on tx
    if (txrx_state == IN_TX)
    {
        // printf("IN_TX\n");
        tx_starting = true;
        radio_h->txrx_state = IN_TX;

        set_speaker_level(0);
        set_tx_level(radio_h->profiles[radio_h->profile_active_idx].tx_level);

        if (radio_is_zbitx(radio_h))
        {
            set_drive(ZBITX_RX_LINE, DRIVE_LOW);
            usleep(2000);
        }
        lpf_off(radio_h);
        usleep(2000);
        set_drive(TX_LINE, DRIVE_HIGH);
        usleep(6000);
        lpf_set(radio_h);
    }
    else
    {
        // printf("IN_RX\n");

        // Digital-voice: ask the RADAE TX pipeline to emit its V2
        // end-of-over frame BEFORE we drop PA drive.  Without this the
        // receiver only sees the carrier disappear, so its decoder
        // keeps its sync state and the next over starts with a stale
        // tracker.  The DSP is still in IN_TX here (txrx_state flips
        // at the end of this branch), so dsp_process_tx keeps draining
        // the TX modem buffer into the DAC while the EOO IQ arrives.
        // Wait until the EOO frame -- 960 samples, 120 ms, in V2 -- and
        // any modem IQ queued ahead of it have left the RADAE buffer;
        // the codec/ALSA tail is waited out below. A fixed 150 ms here
        // (sized for V1's ~30 ms EOO) cut the V2 frame short, so the
        // receiver missed the end of over and went on decoding noise.
        bool dv_eoo_sent = dsp_radae_tx_emit_eoo_if_dv();
        if (dv_eoo_sent)
            dsp_radae_tx_wait_drained(500);

        /* D-STAR: queue the end-of-transmission pattern the same way. */
        bool dstar_eot_sent = dsp_dstar_tx_emit_eot_if_active();
        if (dstar_eot_sent)
            usleep(150000);

        /* Let the audio already on its way play out before the transmitter
         * is muted. Mercury drops PTT once its own buffer is empty, and the
         * loopback ring, the DSP block and the codec queue (some 40-60 ms)
         * still hold the end of the burst: cutting it cost the last symbols
         * of every frame. At least the 10 ms this waited before, at most
         * 100 ms. */
        uint32_t tail_ms = sound_tx_pipeline_ms();
        if (tail_ms < 10)
            tail_ms = 10;
        if (tail_ms > 100)
            tail_ms = 100;
        if (tr_switch_urgent)   /* SWR trip: stop transmitting now */
            tail_ms = 10;
        usleep(tail_ms * 1000);

        set_speaker_level(radio_h->profiles[radio_h->profile_active_idx].speaker_level);
        set_tx_level(0);

        usleep(1000);
        lpf_off(radio_h);
        usleep(1000);
        set_drive(TX_LINE, DRIVE_LOW);
        usleep(1000);
        if (radio_is_zbitx(radio_h))
            set_drive(ZBITX_RX_LINE, DRIVE_HIGH);
        else
            lpf_set(radio_h);

        rx_starting = true;
        radio_h->txrx_state = IN_RX;

        // Clear RADAE TX flow state AFTER txrx_state flips to IN_RX, so
        // no further dsp_process_tx block can spuriously re-fire
        // radae_tx_start via the lazy-start path in
        // dsp_prepare_digital_voice_tx.
        if (dv_eoo_sent)
            dsp_radae_tx_end_over();
        if (dstar_eot_sent)
            dsp_dstar_tx_end_over();
    }

    radio_h->send_ws_update = true;
}

// this is our main 10ms period io loop
void io_tick(radio *radio_h)
{
    static uint64_t ticks = 0;
    static bool last_key_state = false;
    _Atomic uint32_t freq = radio_h->profiles[radio_h->profile_active_idx].freq;
    _Atomic uint32_t volume = radio_h->profiles[radio_h->profile_active_idx].speaker_level;
    _Atomic uint32_t tuning_step = radio_h->step_size;

    bool set_dirty_ws = false;

    ticks++;

    if (last_key_state != radio_h->key_down)
    {
        if (radio_h->profiles[radio_h->profile_active_idx].enable_ptt)
        {
            if (radio_h->key_down)
                tr_switch(radio_h, IN_TX);
            else
                tr_switch(radio_h, IN_RX);

            sbitx_timer_reset = true; // reset the profile timer
        }
        last_key_state = radio_h->key_down;
    }

    // a speed up if one tunes the knob fast
    if(radio_h->tuning_ticks)
    {
        if (radio_h->profiles[radio_h->profile_active_idx].enable_knob_frequency)
        {
            if (abs(radio_h->tuning_ticks) > 50)
                radio_h->tuning_ticks *= 4;

            while (radio_h->tuning_ticks > 0)
            {
                radio_h->tuning_ticks--;
                freq -= tuning_step;
            }
            while (radio_h->tuning_ticks < 0)
            {
                radio_h->tuning_ticks++;
                freq += tuning_step;
            }
            set_frequency(radio_h, freq, radio_h->profile_active_idx);
            set_dirty_ws = true;
            sbitx_timer_reset = true; // reset the profile timer
        }
        else
        {
            radio_h->tuning_ticks = 0;
        }
    }

    if (radio_h->volume_ticks)
    {
        if (radio_h->profiles[radio_h->profile_active_idx].enable_knob_volume)
        {
            if (abs(radio_h->volume_ticks) > 50)
                radio_h->volume_ticks *= 2;

            while (radio_h->volume_ticks > 0)
            {
                radio_h->volume_ticks--;
                if (volume < 3)
                    volume = 0;
                else
                    volume -= 2;
            }
            while (radio_h->volume_ticks < 0)
            {
                radio_h->volume_ticks++;
                if (volume > 97)
                    volume = 100;
                else
                    volume += 2;
            }
            set_speaker_volume(radio_h, volume, radio_h->profile_active_idx);
            set_dirty_ws = true;
            sbitx_timer_reset = true; // reset the profile timer
        }
        else
        {
            radio_h->volume_ticks = 0;
        }
    }

    // period * 3, read power over i2c
    if ( !(ticks % 3) && radio_h->txrx_state == IN_TX )
    {
        update_power_measurements(radio_h);
        swr_protection_check(radio_h);
    }
    if ( !(ticks % 3) && radio_h->txrx_state == IN_RX )
    {
        // we hold the power values in case of high-swr protection enabled
        if (radio_h->swr_protection_enabled != true)
        {
            radio_h->ref_power = 0;
            radio_h->fwd_power = 0;
        }
    }


    // we are not using the button presses for nothing up to now
#if 0
	if (!(ticks % 10))
    {
		if (radio_h->knob_a_pressed)
        {
            printf("Button A pressed\n");
            radio_h->knob_a_pressed = 0;
        }
		if (radio_h->knob_b_pressed)
        {
            printf("Button B pressed\n");
            radio_h->knob_b_pressed = 0;
        }
    }
#endif

    if (set_dirty_ws)
        radio_h->send_ws_update = true;

    // the stop watch for reverting to default profile
    static time_t last_time = 0;

    if ( (radio_h->profile_default_idx != radio_h->profile_active_idx) &&
         (radio_h->profile_timeout >= 0) )
    {
        if (sbitx_timer_reset)
        {
            last_time = time(NULL);
            sbitx_timer_reset = false;
            sbitx_timeout_counter = radio_h->profile_timeout;
        }
        else
        {
            time_t curr_time = time(NULL);

            if (curr_time > last_time)
            {
                sbitx_timeout_counter -= curr_time - last_time;
                last_time = curr_time;
                if (sbitx_timeout_counter <= 0)
                {
                    set_profile(radio_h, radio_h->profile_default_idx);
                    sbitx_timer_reset = true;
                }
            }
        }
    }
    else
    {
        sbitx_timer_reset = true;
        sbitx_timeout_counter = radio_h->profile_timeout;
    }

}


// auxiliary functions for timer functionality
static struct timespec r;
static uint64_t period;
#define NSEC_PER_SEC 1000000000ULL

static inline void timespec_add_us(struct timespec *t, uint64_t d)
{
    d *= 1000;
    t->tv_nsec += d;
    t->tv_sec += t->tv_nsec / NSEC_PER_SEC;
    t->tv_nsec %= NSEC_PER_SEC;
}

void wait_next_activation(void)
{
    // check with clock_gettime is abs time to sleep is already no passed.. go to the next one
    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &r, NULL);
    timespec_add_us(&r, period);
}

int start_periodic_timer(uint64_t offset)
{
    clock_gettime(CLOCK_REALTIME, &r);
    timespec_add_us(&r, offset);
    period = offset;

    return 0;
}

/* ─────────────────────── hfsignals backend ops ─────────────────────────
 *
 * The embedded sBitx is no longer a separate program with its own main();
 * its hardware/DSP/ALSA/IO threads run inside the unified daemon, behind
 * the radio_backend_ops vtable. The state below holds the thread IDs we
 * need to join on shutdown. */

static pthread_t hfs_hw_tids[2];
static pthread_t hfs_control_tid;
static pthread_t hfs_radio_capture_tid;
static pthread_t hfs_radio_playback_tid;
static pthread_t hfs_loop_capture_tid;
static pthread_t hfs_loop_playback_tid;
static bool      hfs_audio_bridge_started = false;
static bool      hfs_dsp_started          = false;
static bool      hfs_sound_started        = false;
static bool      hfs_hw_started           = false;

static bool sbitx_op_init(radio *radio_h)
{
    radio_apply_defaults(radio_h);

    if (radio_h->enable_audio_bridge)
    {
        if (!sbitx_bridge_init(radio_h))
        {
            fprintf(stderr, "sbitx_op_init: sbitx_bridge_init failed\n");
            return false;
        }
        hfs_audio_bridge_started = true;
    }

    if (!hw_init(radio_h, hfs_hw_tids))
    {
        fprintf(stderr, "sbitx_op_init: hw_init failed\n");
        return false;
    }
    hfs_hw_started = true;

    dsp_init(radio_h);
    hfs_dsp_started = true;

    sound_system_init(radio_h,
                      &hfs_control_tid,
                      &hfs_radio_capture_tid,
                      &hfs_radio_playback_tid,
                      &hfs_loop_capture_tid,
                      &hfs_loop_playback_tid);
    hfs_sound_started = true;

    return true;
}

static void *sbitx_op_io_thread(void *radio_h_v)
{
    radio *radio_h = (radio *) radio_h_v;
    /* Block until the hardware threads exit (which happens when shutdown_
     * is set). hw_shutdown does the pthread_join. */
    if (hfs_hw_started)
        hw_shutdown(radio_h, hfs_hw_tids);
    hfs_hw_started = false;
    return NULL;
}

static bool sbitx_op_force_ptt_off(radio *radio_h)
{
    radio_apply_defaults(radio_h);
    return gpio_force_rx(radio_h);
}

static void sbitx_op_shutdown(radio *radio_h)
{
    if (hfs_sound_started)
    {
        sound_system_shutdown(radio_h,
                              &hfs_control_tid,
                              &hfs_radio_capture_tid,
                              &hfs_radio_playback_tid,
                              &hfs_loop_capture_tid,
                              &hfs_loop_playback_tid);
        hfs_sound_started = false;
    }
    if (hfs_dsp_started)
    {
        dsp_free(radio_h);
        hfs_dsp_started = false;
    }
    if (hfs_audio_bridge_started)
    {
        sbitx_bridge_shutdown(radio_h);
        hfs_audio_bridge_started = false;
    }
}

/* set_step_size / set_tone_generation: the hardware reads these atomic
 * fields directly, so the backend op just writes the field and persists. */
static void sbitx_set_step_size(radio *radio_h, uint32_t step_size)
{
    if (radio_h->step_size == step_size)
        return;
    radio_h->step_size = step_size;
    char val[32];
    snprintf(val, sizeof(val), "%u", step_size);
    cfg_set(radio_h, radio_h->cfg_user, "main:step_size", val);
    radio_h->cfg_user_dirty = true;
}

static void sbitx_set_tone_generation(radio *radio_h, bool tone_generation)
{
    if (radio_h->tone_generation == tone_generation)
        return;
    radio_h->tone_generation = tone_generation;
    cfg_set(radio_h, radio_h->cfg_user, "main:tone_generation",
            tone_generation ? "1" : "0");
    radio_h->cfg_user_dirty = true;
}


/* ═══════════════════ generic control surface ═══════════════════════════
 *
 * The sBitx/zBitx is not a CAT rig, but it must answer the same control
 * vocabulary as the hamlib backend (radio_controls.h) so that one web
 * panel, one websocket API and one network rig server serve every radio
 * this daemon drives.
 *
 * What appears here is only what the hardware and the embedded DSP really
 * have: the four ALSA/DSP gain stages, the AGC, the noise reduction and
 * compressor switches, and the DSP band-pass width. Controls the sBitx
 * does not have (split, RIT/XIT, antenna relays, memory channels, a rig
 * keyer) are absent from the enumeration and answer ENOTSUP, so clients
 * render an honest panel instead of dead knobs.
 */

/* Hamlib's agc_level_e values, spelled out so this backend needs no
 * Hamlib header: OFF=0, FAST=2, SLOW=3, MEDIUM=5. */
#define SB_AGC_HL_OFF    0
#define SB_AGC_HL_FAST   2
#define SB_AGC_HL_SLOW   3
#define SB_AGC_HL_MEDIUM 5

static uint16_t sb_agc_from_hamlib(int hl_agc)
{
    switch (hl_agc)
    {
    case SB_AGC_HL_FAST:   return AGC_FAST;
    case SB_AGC_HL_SLOW:   return AGC_SLOW;
    case SB_AGC_HL_MEDIUM: return AGC_MEDIUM;
    case SB_AGC_HL_OFF:
    default:               return AGC_OFF;
    }
}

static int sb_agc_to_hamlib(uint16_t agc)
{
    switch (agc)
    {
    case AGC_FAST:   return SB_AGC_HL_FAST;
    case AGC_SLOW:   return SB_AGC_HL_SLOW;
    case AGC_MEDIUM: return SB_AGC_HL_MEDIUM;
    case AGC_OFF:
    default:         return SB_AGC_HL_OFF;
    }
}

/* Persist one profile field and mark the user config dirty, the same way
 * the dedicated setters above do. */
static void sb_persist_profile_u32(radio *radio_h, uint32_t profile,
                                   const char *field, uint32_t value)
{
    char key[64], val[64];

    snprintf(key, sizeof(key), "profile%u:%s", profile, field);
    snprintf(val, sizeof(val), "%u", value);
    if (cfg_set(radio_h, radio_h->cfg_user, key, val) != 0)
        printf("Error modifying config file\n");

    radio_h->cfg_user_dirty = true;
}

static int sb_get_level(radio *radio_h, const char *name, double *out)
{
    if (!radio_h || !name || !out)
        return RADIO_CTRL_EINVAL;

    uint32_t p = radio_h->profile_active_idx;
    if (p >= radio_h->profiles_count)
        return RADIO_CTRL_EIO;

    const radio_profile *prof = &radio_h->profiles[p];

    if (!strcmp(name, "AF"))      { *out = prof->speaker_level / 100.0;            return RADIO_CTRL_OK; }
    if (!strcmp(name, "RF"))      { *out = prof->rx_level / 100.0;                 return RADIO_CTRL_OK; }
    if (!strcmp(name, "MICGAIN")) { *out = prof->mic_level / 100.0;                return RADIO_CTRL_OK; }
    if (!strcmp(name, "RFPOWER")) { *out = prof->power_level_percentage / 100.0;   return RADIO_CTRL_OK; }
    if (!strcmp(name, "AGC"))     { *out = sb_agc_to_hamlib(prof->agc);            return RADIO_CTRL_OK; }
    if (!strcmp(name, "STRENGTH"))
    {
        if (radio_h->s_meter_db == -200)
            return RADIO_CTRL_EIO;   /* no reading yet — do not invent one */
        *out = radio_h->s_meter_db;
        return RADIO_CTRL_OK;
    }
    if (!strcmp(name, "SWR"))     { *out = radio_backend_get_swr(radio_h) / 10.0;  return RADIO_CTRL_OK; }
    if (!strcmp(name, "RFPOWER_METER_WATTS"))
    {
        *out = radio_backend_get_fwd_power(radio_h) / 10.0;
        return RADIO_CTRL_OK;
    }

    return RADIO_CTRL_ENOTSUP;
}

static int sb_set_level(radio *radio_h, const char *name, double value)
{
    if (!radio_h || !name)
        return RADIO_CTRL_EINVAL;

    uint32_t p = radio_h->profile_active_idx;
    if (p >= radio_h->profiles_count)
        return RADIO_CTRL_EIO;

    radio_profile *prof = &radio_h->profiles[p];

    if (!strcmp(name, "AF"))
    {
        if (value < 0.0) value = 0.0;
        if (value > 1.0) value = 1.0;
        set_speaker_volume(radio_h, (uint32_t) lrint(value * 100.0), p);
        return RADIO_CTRL_OK;
    }
    if (!strcmp(name, "RFPOWER"))
    {
        if (value < 0.0) value = 0.0;
        if (value > 1.0) value = 1.0;
        set_power_knob(radio_h, (uint16_t) lrint(value * 100.0), p);
        return RADIO_CTRL_OK;
    }
    if (!strcmp(name, "RF"))
    {
        if (value < 0.0) value = 0.0;
        if (value > 1.0) value = 1.0;
        prof->rx_level = (uint32_t) lrint(value * 100.0);
        if (p == radio_h->profile_active_idx)
            set_rx_level(prof->rx_level);
        sb_persist_profile_u32(radio_h, p, "rx_level", prof->rx_level);
        return RADIO_CTRL_OK;
    }
    if (!strcmp(name, "MICGAIN"))
    {
        if (value < 0.0) value = 0.0;
        if (value > 1.0) value = 1.0;
        prof->mic_level = (uint32_t) lrint(value * 100.0);
        if (p == radio_h->profile_active_idx)
            set_mic_level(prof->mic_level);
        sb_persist_profile_u32(radio_h, p, "mic_level", prof->mic_level);
        return RADIO_CTRL_OK;
    }
    if (!strcmp(name, "AGC"))
    {
        prof->agc = sb_agc_from_hamlib((int) lrint(value));
        sb_persist_profile_u32(radio_h, p, "agc", prof->agc);
        return RADIO_CTRL_OK;
    }

    /* Meters are read-only, and everything else this rig does not have. */
    if (!strcmp(name, "STRENGTH") || !strcmp(name, "SWR") ||
        !strcmp(name, "RFPOWER_METER_WATTS"))
        return RADIO_CTRL_EINVAL;

    return RADIO_CTRL_ENOTSUP;
}

static int sb_get_func(radio *radio_h, const char *name, int *out)
{
    if (!radio_h || !name || !out)
        return RADIO_CTRL_EINVAL;

    uint32_t p = radio_h->profile_active_idx;
    if (p >= radio_h->profiles_count)
        return RADIO_CTRL_EIO;

    const radio_profile *prof = &radio_h->profiles[p];

    if (!strcmp(name, "NR"))   { *out = prof->noise_reduction == NOISE_REDUCTION_ON; return RADIO_CTRL_OK; }
    if (!strcmp(name, "COMP")) { *out = prof->compressor == COMPRESSOR_ON;           return RADIO_CTRL_OK; }

    return RADIO_CTRL_ENOTSUP;
}

static int sb_set_func(radio *radio_h, const char *name, int on)
{
    if (!radio_h || !name)
        return RADIO_CTRL_EINVAL;

    uint32_t p = radio_h->profile_active_idx;
    if (p >= radio_h->profiles_count)
        return RADIO_CTRL_EIO;

    radio_profile *prof = &radio_h->profiles[p];

    if (!strcmp(name, "NR"))
    {
        prof->noise_reduction = on ? NOISE_REDUCTION_ON : NOISE_REDUCTION_OFF;
        sb_persist_profile_u32(radio_h, p, "noise_reduction", prof->noise_reduction);
        return RADIO_CTRL_OK;
    }
    if (!strcmp(name, "COMP"))
    {
        prof->compressor = on ? COMPRESSOR_ON : COMPRESSOR_OFF;
        sb_persist_profile_u32(radio_h, p, "compressor", prof->compressor);
        return RADIO_CTRL_OK;
    }

    return RADIO_CTRL_ENOTSUP;
}

static size_t sb_enumerate_controls(radio *radio_h, radio_ctrl_info *out, size_t max)
{
    /* name, kind, is_float, can_get, can_set, min, max, step */
    static const radio_ctrl_info table[] = {
        { "AF",                  RADIO_CTRL_LEVEL, true,  true, true,  0.0,   1.0, 0.01 },
        { "RF",                  RADIO_CTRL_LEVEL, true,  true, true,  0.0,   1.0, 0.01 },
        { "MICGAIN",             RADIO_CTRL_LEVEL, true,  true, true,  0.0,   1.0, 0.01 },
        { "RFPOWER",             RADIO_CTRL_LEVEL, true,  true, true,  0.0,   1.0, 0.01 },
        /* Hamlib agc_level_e: OFF/FAST/SLOW/MEDIUM (0/2/3/5). */
        { "AGC",                 RADIO_CTRL_LEVEL, false, true, true,  0.0,   5.0, 1.0  },
        { "STRENGTH",            RADIO_CTRL_LEVEL, false, true, false, -54.0, 60.0, 1.0 },
        { "SWR",                 RADIO_CTRL_LEVEL, true,  true, false, 1.0,   10.0, 0.1 },
        { "RFPOWER_METER_WATTS", RADIO_CTRL_LEVEL, true,  true, false, 0.0,  100.0, 0.1 },
        { "NR",                  RADIO_CTRL_FUNC,  false, true, true,  0.0,   1.0, 1.0  },
        { "COMP",                RADIO_CTRL_FUNC,  false, true, true,  0.0,   1.0, 1.0  },
    };

    if (!radio_h || !out || max == 0)
        return 0;

    size_t n = sizeof(table) / sizeof(table[0]);
    if (n > max)
        n = max;

    memcpy(out, table, n * sizeof(*out));
    return n;
}

/* The sBitx has a single VFO. Report it rather than failing, so clients
 * that always ask for the current VFO (WSJT-X does) work unchanged. */
static int sb_get_vfo(radio *radio_h, char *out, size_t out_len)
{
    (void) radio_h;

    if (!out || out_len == 0)
        return RADIO_CTRL_EINVAL;

    snprintf(out, out_len, "VFOA");
    return RADIO_CTRL_OK;
}

static int sb_set_vfo(radio *radio_h, const char *vfo)
{
    (void) radio_h;

    if (!vfo || !*vfo)
        return RADIO_CTRL_EINVAL;
    if (!strcmp(vfo, "VFOA") || !strcmp(vfo, "VFO") || !strcmp(vfo, "currVFO"))
        return RADIO_CTRL_OK;

    return RADIO_CTRL_ENOTSUP;
}

/* Filter width is the DSP band-pass span. The low edge is the operator's
 * setting; the width moves the high edge and retunes the running filters. */
static int sb_get_width(radio *radio_h, uint32_t *hz)
{
    if (!radio_h || !hz)
        return RADIO_CTRL_EINVAL;

    uint32_t p = radio_h->profile_active_idx;
    if (p >= radio_h->profiles_count)
        return RADIO_CTRL_EIO;

    const radio_profile *prof = &radio_h->profiles[p];
    *hz = prof->bpf_high > prof->bpf_low ? prof->bpf_high - prof->bpf_low : 0;

    return RADIO_CTRL_OK;
}

static int sb_set_width(radio *radio_h, uint32_t hz)
{
    if (!radio_h)
        return RADIO_CTRL_EINVAL;

    uint32_t p = radio_h->profile_active_idx;
    if (p >= radio_h->profiles_count)
        return RADIO_CTRL_EIO;

    /* Wider than the 96 kHz DSP Nyquist band is meaningless, and a zero
     * width would collapse the filter. */
    if (hz < 100 || hz > 20000)
        return RADIO_CTRL_EINVAL;

    radio_profile *prof = &radio_h->profiles[p];
    prof->bpf_high = prof->bpf_low + hz;
    prof->filter_width = hz;
    sb_persist_profile_u32(radio_h, p, "bpf_high", prof->bpf_high);
    sb_persist_profile_u32(radio_h, p, "filter_width", hz);

    dsp_set_filters();

    return RADIO_CTRL_OK;
}

/* Mode by name. The sBitx has no rig-side submode, so the data variants
 * (PKTUSB/DIGU/...) collapse onto the same sideband the DSP produces; the
 * data path itself is the profile's operating_mode, set by the operator. */
static uint16_t sb_mode_from_name(const char *s, bool *ok)
{
    *ok = true;
    if (!strcmp(s, "LSB")  || !strcmp(s, "PKTLSB") || !strcmp(s, "DIGL")) return MODE_LSB;
    if (!strcmp(s, "USB")  || !strcmp(s, "PKTUSB") || !strcmp(s, "DIGU")) return MODE_USB;
    if (!strcmp(s, "CW")   || !strcmp(s, "CWR"))                          return MODE_CW;
    if (!strcmp(s, "FM")   || !strcmp(s, "PKTFM")  || !strcmp(s, "FMN"))  return MODE_FM;
    if (!strcmp(s, "AM")   || !strcmp(s, "AMS"))                          return MODE_AM;
    if (!strcmp(s, "RTTY") || !strcmp(s, "RTTYR"))                        return MODE_RTTY;
    *ok = false;
    return MODE_USB;
}

static int sb_get_mode_name(radio *radio_h, char *out, size_t out_len, uint32_t *width)
{
    if (!radio_h || !out || out_len == 0)
        return RADIO_CTRL_EINVAL;

    uint32_t p = radio_h->profile_active_idx;
    if (p >= radio_h->profiles_count)
        return RADIO_CTRL_EIO;

    const char *name;
    switch (radio_h->profiles[p].mode)
    {
    case MODE_LSB:  name = "LSB";    break;
    case MODE_CW:   name = "CW";     break;
    case MODE_FM:   name = "FM";     break;
    case MODE_AM:   name = "AM";     break;
    case MODE_RTTY: name = "RTTY";   break;
    case MODE_FT8:  name = "PKTUSB"; break;
    case MODE_DRM:  name = "USB";    break;
    case MODE_USB:
    default:        name = "USB";    break;
    }

    snprintf(out, out_len, "%s", name);
    if (width)
        sb_get_width(radio_h, width);

    return RADIO_CTRL_OK;
}

static int sb_set_mode_name(radio *radio_h, const char *mode, uint32_t width)
{
    bool ok = false;

    if (!radio_h || !mode || !*mode)
        return RADIO_CTRL_EINVAL;

    uint16_t m = sb_mode_from_name(mode, &ok);
    if (!ok)
        return RADIO_CTRL_EINVAL;

    set_mode(radio_h, m, radio_h->profile_active_idx);
    if (width)
        sb_set_width(radio_h, width);

    return RADIO_CTRL_OK;
}

/* rigctld \dump_state for the sBitx/zBitx: the coverage and modes this
 * radio really has, in the same protocol-0 block the hamlib backend emits
 * from the rig's own caps. Level/func masks list exactly what
 * sb_enumerate_controls serves: AF|RF|AGC|MICGAIN|RFPOWER|STRENGTH|SWR|
 * RFPOWER_METER_WATTS for get, the settable subset for set, and NR|COMP
 * for funcs. */
static int sb_dump_state(radio *radio_h, char *out, size_t out_len)
{
    (void) radio_h;

    if (!out || out_len == 0)
        return RADIO_CTRL_EINVAL;

    /* Hamlib mode bits: AM|CW|USB|LSB|RTTY|FM|CWR|RTTYR|PKTLSB|PKTUSB */
    static const char *modes = "0x2ef";

    int n = snprintf(out, out_len,
        "0\n"                       /* protocol version */
        "2\n"                       /* model: NET rigctl */
        "1\n"                       /* ITU region */
        "500000 30000000 %s -1 -1 0x1 0x0\n"
        "0 0 0 0 0 0 0\n"
        "500000 30000000 %s 100 40000 0x1 0x0\n"
        "0 0 0 0 0 0 0\n"
        "%s 1\n"                    /* tuning steps: 1 Hz */
        "0 0\n"
        "0x82 500\n"                /* CW 500 Hz */
        "0x21 2700\n"               /* SSB 2700 Hz */
        "0x40 7000\n"               /* FM 7 kHz */
        "0x10 10000\n"              /* AM 10 kHz */
        "0 0\n"
        "0\n"                       /* max_rit */
        "0\n"                       /* max_xit */
        "0\n"                       /* max_ifshift */
        "0\n"                       /* announces */
        "0\n"                       /* preamp list */
        "0\n"                       /* attenuator list */
        "0x204\n"                   /* has_get_func: COMP|NR */
        "0x204\n"                   /* has_set_func */
        "0x8050023018\n"            /* has_get_level: AF|RF|RFPOWER|MICGAIN|AGC|SWR|STRENGTH|RFPOWER_METER_WATTS */
        "0x23018\n"                 /* has_set_level: AF|RF|RFPOWER|MICGAIN|AGC */
        "0\n"                       /* has_get_parm */
        "0\n",                      /* has_set_parm */
        modes, modes, modes);

    if (n < 0 || (size_t) n >= out_len)
        return RADIO_CTRL_EIO;

    return RADIO_CTRL_OK;
}

const radio_backend_ops sbitx_backend_ops = {
    .name                    = "hfsignals",
    .init                    = sbitx_op_init,
    .shutdown                = sbitx_op_shutdown,
    .io_thread               = sbitx_op_io_thread,
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
    .set_step_size           = sbitx_set_step_size,
    .set_tone_generation     = sbitx_set_tone_generation,
    .set_profile             = set_profile,
    .get_fwd_power           = get_fwd_power,
    .get_ref_power           = get_ref_power,
    .get_swr                 = get_swr,

    .get_level               = sb_get_level,
    .set_level               = sb_set_level,
    .get_func                = sb_get_func,
    .set_func                = sb_set_func,
    .enumerate_controls      = sb_enumerate_controls,

    .get_vfo                 = sb_get_vfo,
    .set_vfo                 = sb_set_vfo,
    .get_mode_name           = sb_get_mode_name,
    .set_mode_name           = sb_set_mode_name,
    .get_width               = sb_get_width,
    .set_width               = sb_set_width,
    .dump_state              = sb_dump_state,
    .force_ptt_off           = sbitx_op_force_ptt_off,
};
