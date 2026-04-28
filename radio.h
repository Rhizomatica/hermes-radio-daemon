/* hermes-radio-daemon - generic radio control daemon
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

#ifndef RADIO_H_
#define RADIO_H_

#define CFG_RADIO_PATH "/etc/hermes/radio.ini"
#define CFG_USER_PATH  "/etc/hermes/user.ini"

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#include <iniparser.h>

/* Radio mode definitions */
#define MODE_LSB 0
#define MODE_USB 1
#define MODE_CW  2

/* AGC settings */
#define AGC_OFF    0
#define AGC_SLOW   1
#define AGC_MEDIUM 2
#define AGC_FAST   3

/* Compressor settings */
#define COMPRESSOR_OFF 0
#define COMPRESSOR_ON  1

/* TX/RX states */
#define IN_RX 0
#define IN_TX 1

/* Maximum number of radio profiles */
#define MAX_RADIO_PROFILES 4
#define AUDIO_DEVICE_NAME_MAX 256
#define WEBSOCKET_BIND_MAX 128
#define RECORDING_PATH_MAX 512
#define WATERFALL_BINS 128
#define RADIO_MESSAGE_MAX 128
#define CONFIG_PATH_MAX 512

typedef struct {
    int16_t *samples;
    size_t capacity;
    size_t read_pos;
    size_t write_pos;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} audio_ring_buffer;

typedef struct {
    FILE *fp;
    char path[RECORDING_PATH_MAX];
    uint32_t sample_rate;
    uint32_t data_bytes;
    pthread_mutex_t mutex;
    bool active;
} wav_recording;

/* PTT type values (mirrors Hamlib ptt_type_t values) */
#define PTT_NONE    0  /* No PTT available */
#define PTT_RIG     1  /* PTT via CAT command */
#define PTT_SERIAL_RTS 2  /* PTT via serial RTS */
#define PTT_SERIAL_DTR 3  /* PTT via serial DTR */
#define PTT_PARALLEL 4    /* PTT via parallel port */
#define PTT_CM108    5    /* PTT via CM108 GPIO */
#define PTT_GPIO     6    /* PTT via GPIO */
#define PTT_RIG_MICDATA 7 /* PTT via Mic data port */

/* Per-profile radio configuration */
typedef struct {
    _Atomic uint32_t freq;
    _Atomic uint16_t mode;         /* MODE_* */
    _Atomic uint16_t agc;          /* AGC_* */
    _Atomic uint16_t compressor;   /* COMPRESSOR_* */
    _Atomic uint32_t speaker_level;       /* 0 - 100 */
    _Atomic uint16_t power_level_percentage; /* 0 - 100 */
    _Atomic bool digital_voice;
} radio_profile;

/* Main radio handle structure */
typedef struct {
    /* Hamlib configuration */
    int hamlib_model;          /* Hamlib rig model number */
    char rig_pathname[256];    /* Serial port or hostname:port */
    int serial_rate;           /* Baud rate for serial port */
    int ptt_type;              /* PTT type (PTT_*) */
    char ptt_pathname[256];    /* PTT device path (if separate from rig) */

    /* Hamlib private handle (RIG*) */
    void *rig;

    /* Radio status */
    _Atomic bool txrx_state;              /* IN_RX or IN_TX */
    _Atomic uint32_t reflected_threshold; /* vswr * 10 */
    _Atomic bool swr_protection_enabled;
    _Atomic bool tone_generation;
    pthread_mutex_t message_mutex;
    char message[RADIO_MESSAGE_MAX];
    _Atomic bool message_available;

    /* Power measurements */
    _Atomic uint32_t fwd_power;
    _Atomic uint32_t ref_power;

    /* Informational / status fields */
    _Atomic uint32_t serial_number;
    _Atomic bool system_is_connected; /* VARA / modem connection status */
    _Atomic bool system_is_ok;        /* uucp / system health indicator */

    /* Modem statistics written by the modem application */
    _Atomic uint32_t bitrate;
    _Atomic int32_t  snr;
    _Atomic uint32_t bytes_transmitted;
    _Atomic uint32_t bytes_received;

    /* Frequency tuning step */
    _Atomic uint32_t step_size;

    /* BFO frequency (kept for API compatibility; not used with Hamlib) */
    _Atomic uint32_t bfo_frequency;

    /* SHM control enable flag */
    _Atomic bool enable_shm_control;
    _Atomic bool enable_websocket;
    char websocket_bind[WEBSOCKET_BIND_MAX];

    /* Generic ALSA media bridge */
    _Atomic bool enable_audio_bridge;
    char capture_device[AUDIO_DEVICE_NAME_MAX];
    char playback_device[AUDIO_DEVICE_NAME_MAX];
    _Atomic uint32_t audio_sample_rate;
    _Atomic uint32_t audio_period_size;
    _Atomic uint32_t audio_queue_samples;
    char recording_dir[RECORDING_PATH_MAX];

    /* Profile management */
    _Atomic uint32_t profile_active_idx;
    _Atomic int32_t  profile_timeout; /* -1 disables auto-return to default */
    _Atomic uint32_t profile_default_idx;
    _Atomic uint32_t profiles_count;
    radio_profile profiles[MAX_RADIO_PROFILES];

    /* Configuration dirty flags and handles */
    pthread_mutex_t cfg_mutex;
    dictionary *cfg_radio; /* core / hardware config */
    dictionary *cfg_user;  /* user / profile config */
    char cfg_radio_path[CONFIG_PATH_MAX];
    char cfg_user_path[CONFIG_PATH_MAX];
    _Atomic bool cfg_radio_dirty;
    _Atomic bool cfg_user_dirty;

    /* Media queues and recording */
    audio_ring_buffer rx_audio_ring;
    audio_ring_buffer tx_audio_ring;
    wav_recording rx_recording;
    wav_recording tx_recording;

    /* Waterfall / spectrum publication */
    pthread_mutex_t spectrum_mutex;
    float rx_spectrum[WATERFALL_BINS];
    float tx_spectrum[WATERFALL_BINS];
    _Atomic uint32_t rx_spectrum_seq;
    _Atomic uint32_t tx_spectrum_seq;
    _Atomic bool rx_spectrum_valid;
    _Atomic bool tx_spectrum_valid;
    _Atomic uint32_t spectrum_sample_rate;

} radio;

#endif /* RADIO_H_ */
