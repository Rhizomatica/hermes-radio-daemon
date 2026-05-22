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

#include "cfg_utils.h"
#include "sbitx_core.h"
#include "sbitx_i2c.h"
#include "sbitx_gpio.h"
#include "sbitx_si5351.h"
#include "sbitx_alsa.h"
#include "sbitx_dsp.h"
#include "../radio_backend.h"

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

    if (*radio_freq == frequency)
        return;

    if ( (frequency > 30000000) || (frequency < 500000) )
        return;

    *radio_freq = frequency;

    if (profile == radio_h->profile_active_idx)
    {
        if (radio_h->profiles[radio_h->profile_active_idx].operating_mode == OPERATING_MODE_CONTROLS_ONLY)
            si5351bx_setfreq(2, *radio_freq + radio_h->bfo_frequency - 15000); // here we set the real frequency of the radio (in USB, which is the current setup) - 15000 which is the carrier offset in Mercury in sbitx mode
        else
            si5351bx_setfreq(2, *radio_freq + radio_h->bfo_frequency - 24000); // 24 kHz offset to provide the user the "real" dial frequency after the DSP processing (just "- 24000")
    }

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

    if (*radio_mode == mode)
        return;

    *radio_mode = mode;

    if (profile == radio_h->profile_active_idx)
    {
        dsp_set_filters();
    }

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
        tr_switch(radio_h, IN_RX);
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
        return;

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
        // 150 ms covers ~30 ms of EOO IQ + the ALSA+DSP tail.
        bool dv_eoo_sent = dsp_radae_tx_emit_eoo_if_dv();
        if (dv_eoo_sent)
            usleep(150000);

        usleep(10000);

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
};
