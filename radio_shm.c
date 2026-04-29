/* hermes-radio-daemon - SHM command processing
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

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>

#include "radio_backend.h"
#include "radio.h"
#include "radio_shm.h"
#include "shm_utils.h"
#include "include/sbitx_io.h"
#include "include/radio_cmds.h"

extern _Atomic bool shutdown_;

static controller_conn *connector_local;
static radio *radio_h_shm;

static void process_radio_command(uint8_t *cmd, uint8_t *response)
{
    uint32_t frequency = 0, power = 0;
    uint8_t profile;

    radio *radio_h = radio_h_shm;
    memset(response, 0, 5);

    /* Upper 2 bits of cmd[4] carry the profile index;
     * lower 6 bits carry the command. */
    switch (cmd[4] & 0x3f)
    {

    case CMD_PTT_ON:
        if (radio_h->swr_protection_enabled)
        {
            response[0] = CMD_ALERT_PROTECTION_ON;
        }
        else if (radio_h->txrx_state == IN_RX)
        {
            radio_backend_set_txrx_state(radio_h, IN_TX);
            response[0] = CMD_RESP_ACK;
        }
        else
        {
            response[0] = CMD_RESP_PTT_ON_NACK;
        }
        break;

    case CMD_PTT_OFF:
        if (radio_h->swr_protection_enabled)
        {
            response[0] = CMD_ALERT_PROTECTION_ON;
        }
        else if (radio_h->txrx_state == IN_TX)
        {
            radio_backend_set_txrx_state(radio_h, IN_RX);
            response[0] = CMD_RESP_ACK;
        }
        else
        {
            response[0] = CMD_RESP_PTT_OFF_NACK;
        }
        break;

    case CMD_GET_TXRX_STATUS:
        response[0] = (radio_h->txrx_state == IN_TX)
                      ? CMD_RESP_GET_TXRX_INTX
                      : CMD_RESP_GET_TXRX_INRX;
        break;

    case CMD_RESET_PROTECTION:
        response[0] = CMD_RESP_ACK;
        radio_h->swr_protection_enabled = false;
        break;

    case CMD_TIMEOUT_RESET:
        response[0] = CMD_RESP_ACK;
        radio_backend_reset_timeout_timer();
        break;

    case CMD_GET_PROTECTION_STATUS:
        response[0] = radio_h->swr_protection_enabled
                      ? CMD_RESP_GET_PROTECTION_ON
                      : CMD_RESP_GET_PROTECTION_OFF;
        break;

    case CMD_GET_BFO:
        response[0] = CMD_RESP_GET_BFO_ACK;
        memcpy(response + 1, &radio_h->bfo_frequency, 4);
        break;

    case CMD_SET_BFO:
    {
        response[0] = CMD_RESP_ACK;
        uint32_t bfo_freq;
        memcpy(&bfo_freq, cmd, 4);
        radio_backend_set_bfo(radio_h, bfo_freq);
        break;
    }

    case CMD_GET_FWD:
    {
        response[0] = CMD_RESP_GET_FWD_ACK;
        uint16_t fwdpower = (uint16_t) radio_backend_get_fwd_power(radio_h);
        memcpy(response + 1, &fwdpower, 2);
        break;
    }

    case CMD_GET_REF:
    {
        response[0] = CMD_RESP_GET_REF_ACK;
        uint16_t vswr = (uint16_t) radio_backend_get_swr(radio_h);
        memcpy(response + 1, &vswr, 2);
        break;
    }

    case CMD_GET_LED_STATUS:
        response[0] = radio_h->system_is_ok
                      ? CMD_RESP_GET_LED_STATUS_ON
                      : CMD_RESP_GET_LED_STATUS_OFF;
        break;

    case CMD_SET_LED_STATUS:
        response[0] = CMD_RESP_ACK;
        radio_h->system_is_ok = cmd[0];
        break;

    case CMD_GET_CONNECTED_STATUS:
        response[0] = radio_h->system_is_connected
                      ? CMD_RESP_GET_CONNECTED_STATUS_ON
                      : CMD_RESP_GET_CONNECTED_STATUS_OFF;
        break;

    case CMD_SET_CONNECTED_STATUS:
        response[0] = CMD_RESP_ACK;
        if (!cmd[0])
        {
            connector_local->message_available = true;
            connector_local->message[0] = 0;
            pthread_mutex_lock(&radio_h->message_mutex);
            radio_h->message[0] = 0;
            pthread_mutex_unlock(&radio_h->message_mutex);
            radio_h->message_available = true;
        }
        radio_h->system_is_connected = cmd[0];
        break;

    case CMD_GET_DIGITAL_VOICE:
        profile = cmd[4] >> 6;
        if (profile < radio_h->profiles_count)
        {
            response[0] = radio_h->profiles[profile].digital_voice
                          ? CMD_RESP_GET_DIGITAL_VOICE_ON
                          : CMD_RESP_GET_DIGITAL_VOICE_OFF;
        }
        else
        {
            response[0] = CMD_RESP_WRONG_COMMAND;
        }
        break;

    case CMD_SET_DIGITAL_VOICE:
        response[0] = CMD_RESP_ACK;
        profile = cmd[4] >> 6;
        if (profile < radio_h->profiles_count)
            radio_backend_set_digital_voice(radio_h, cmd[0], profile);
        break;

    case CMD_GET_SERIAL:
        response[0] = CMD_RESP_GET_SERIAL_ACK;
        memcpy(response + 1, &radio_h->serial_number, 4);
        break;

    case CMD_SET_SERIAL:
        response[0] = CMD_RESP_ACK;
        radio_backend_set_serial(radio_h, *(uint32_t *) cmd);
        break;

    case CMD_GET_STEPHZ:
        response[0] = CMD_RESP_GET_STEPHZ_ACK;
        memcpy(response + 1, &radio_h->step_size, 4);
        break;

    case CMD_SET_STEPHZ:
        response[0] = CMD_RESP_ACK;
        memcpy(&frequency, cmd, 4);
        radio_backend_set_step_size(radio_h, frequency);
        break;

    case CMD_GET_REF_THRESHOLD:
    {
        response[0] = CMD_RESP_GET_REF_THRESHOLD_ACK;
        uint16_t thr = (uint16_t) radio_h->reflected_threshold;
        memcpy(response + 1, &thr, 2);
        break;
    }

    case CMD_SET_REF_THRESHOLD:
    {
        response[0] = CMD_RESP_ACK;
        uint16_t thr;
        memcpy(&thr, cmd, 2);
        radio_backend_set_reflected_threshold(radio_h, (uint32_t) thr);
        break;
    }

    case CMD_GET_PROFILE:
        response[0] = CMD_RESP_GET_PROFILE;
        response[1] = (uint8_t) radio_h->profile_active_idx;
        break;

    case CMD_SET_PROFILE:
        response[0] = CMD_RESP_ACK;
        {
            uint32_t prof_set = (uint32_t) cmd[0];
            if (prof_set < radio_h->profiles_count)
                radio_backend_set_profile(radio_h, prof_set);
        }
        break;

    case CMD_RADIO_RESET:
        response[0] = CMD_RESP_WRONG_COMMAND;
        shutdown_ = true;
        break;

    case CMD_GET_FREQ:
        response[0] = CMD_RESP_GET_FREQ_ACK;
        profile = cmd[4] >> 6;
        if (profile < radio_h->profiles_count)
            frequency = radio_h->profiles[profile].freq;
        memcpy(response + 1, &frequency, 4);
        break;

    case CMD_SET_FREQ:
        response[0] = CMD_RESP_ACK;
        profile = cmd[4] >> 6;
        if (profile < radio_h->profiles_count)
        {
            memcpy(&frequency, cmd, 4);
            radio_backend_set_frequency(radio_h, frequency, profile);
        }
        break;

    case CMD_GET_POWER:
        response[0] = CMD_RESP_GET_POWER;
        profile = cmd[4] >> 6;
        if (profile < radio_h->profiles_count)
            power = radio_h->profiles[profile].power_level_percentage;
        memcpy(response + 1, &power, 4);
        break;

    case CMD_SET_POWER:
        response[0] = CMD_RESP_ACK;
        profile = cmd[4] >> 6;
        if (profile < radio_h->profiles_count)
        {
            memcpy(&power, cmd, 4);
            radio_backend_set_power_level(radio_h, (uint16_t) power, profile);
        }
        break;

    case CMD_SET_MODE:
        response[0] = CMD_RESP_ACK;
        profile = cmd[4] >> 6;
        if (cmd[0] == 0x00 || cmd[0] == 0x03)
            radio_backend_set_mode(radio_h, MODE_LSB, profile);
        else if (cmd[0] == 0x01)
            radio_backend_set_mode(radio_h, MODE_USB, profile);
        else if (cmd[0] == 0x04)
            radio_backend_set_mode(radio_h, MODE_CW, profile);
        break;

    case CMD_GET_MODE:
        profile = cmd[4] >> 6;
        if (radio_h->profiles[profile].mode == MODE_USB)
            response[0] = CMD_RESP_GET_MODE_USB;
        else if (radio_h->profiles[profile].mode == MODE_LSB)
            response[0] = CMD_RESP_GET_MODE_LSB;
        else
            response[0] = CMD_RESP_GET_MODE_CW;
        break;

    case CMD_GET_VOLUME:
        response[0] = CMD_RESP_GET_VOLUME_ACK;
        profile = cmd[4] >> 6;
        if (profile < radio_h->profiles_count)
            memcpy(response + 1, &radio_h->profiles[profile].speaker_level, 4);
        break;

    case CMD_SET_VOLUME:
    {
        response[0] = CMD_RESP_ACK;
        profile = cmd[4] >> 6;
        uint32_t speaker_level;
        memcpy(&speaker_level, cmd, 4);
        if (speaker_level > 100)
            speaker_level = 100;
        radio_backend_set_speaker_volume(radio_h, speaker_level, profile);
        break;
    }

    case CMD_GET_TIMEOUT:
        response[0] = CMD_RESP_GET_TIMEOUT_ACK;
        memcpy(response + 1, &radio_h->profile_timeout, 4);
        break;

    case CMD_SET_TIMEOUT:
        response[0] = CMD_RESP_ACK;
        radio_backend_set_profile_timeout(radio_h, *(int32_t *) cmd);
        break;

    case CMD_GET_TONE:
        response[0] = CMD_RESP_GET_TONE_ACK;
        memcpy(response + 1, &radio_h->tone_generation, 1);
        break;

    case CMD_SET_TONE:
        response[0] = CMD_RESP_ACK;
        radio_backend_set_tone_generation(radio_h, cmd[0] != 0);
        break;

    case CMD_GET_BITRATE:
        response[0] = CMD_RESP_GET_BITRATE;
        memcpy(response + 1, &radio_h->bitrate, 4);
        break;

    case CMD_SET_BITRATE:
        response[0] = CMD_RESP_ACK;
        memcpy(&radio_h->bitrate, cmd, 4);
        break;

    case CMD_GET_SNR:
        response[0] = CMD_RESP_GET_SNR;
        memcpy(response + 1, &radio_h->snr, 4);
        break;

    case CMD_SET_SNR:
        response[0] = CMD_RESP_ACK;
        memcpy(&radio_h->snr, cmd, 4);
        break;

    case CMD_SET_BYTES_RX:
        response[0] = CMD_RESP_ACK;
        memcpy(&radio_h->bytes_received, cmd, 4);
        break;

    case CMD_GET_BYTES_RX:
        response[0] = CMD_RESP_GET_BYTES_RX;
        memcpy(response + 1, &radio_h->bytes_received, 4);
        break;

    case CMD_SET_BYTES_TX:
        response[0] = CMD_RESP_ACK;
        memcpy(&radio_h->bytes_transmitted, cmd, 4);
        break;

    case CMD_GET_BYTES_TX:
        response[0] = CMD_RESP_GET_BYTES_TX;
        memcpy(response + 1, &radio_h->bytes_transmitted, 4);
        break;

    case CMD_SET_RADIO_DEFAULTS:
        response[0] = CMD_RESP_ACK;
        /* Writing current config to disk is handled by the config thread */
        radio_h->cfg_radio_dirty = true;
        radio_h->cfg_user_dirty  = true;
        break;

    default:
        response[0] = CMD_RESP_WRONG_COMMAND;
    }
}

/* SHM command dispatcher thread */
static void *process_radio_command_thread(void *arg)
{
    controller_conn *conn = arg;

    pthread_mutex_lock(&conn->cmd_mutex);

    while (!shutdown_)
    {
        pthread_cond_wait(&conn->cmd_condition, &conn->cmd_mutex);

        if (shutdown_)
            goto exit_local;

        process_radio_command(conn->service_command, conn->response_service);

        if (conn->service_command[4] == CMD_RADIO_RESET)
        {
            shutdown_ = true;
            pthread_mutex_unlock(&conn->cmd_mutex);
            fprintf(stderr, "\nReset command. Exiting\n");
            return NULL;
        }

        conn->response_available = true;
    }

exit_local:
    pthread_mutex_unlock(&conn->cmd_mutex);
    return NULL;
}

static bool initialize_connector(controller_conn *connector)
{
    pthread_mutexattr_t attr;
    pthread_condattr_t  cond_attr;

    if (pthread_mutexattr_init(&attr))                                 goto err_mutexattr;
    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED))  goto err_mutexattr;
    if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST))     goto err_mutexattr;
    if (pthread_mutex_init(&connector->cmd_mutex, &attr))             goto err_mutexattr;
    if (pthread_mutex_init(&connector->response_mutex, &attr))        goto err_mutexattr;
    pthread_mutexattr_destroy(&attr);

    if (pthread_condattr_init(&cond_attr))                             return false;
    if (pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED)) goto err_condattr;
    if (pthread_cond_init(&connector->cmd_condition, &cond_attr))     goto err_condattr;
    pthread_condattr_destroy(&cond_attr);

    connector->response_available = false;
    connector->message_available  = false;

    return true;

err_mutexattr:
    perror("initialize_connector (mutex)");
    pthread_mutexattr_destroy(&attr);
    return false;

err_condattr:
    perror("initialize_connector (cond)");
    pthread_condattr_destroy(&cond_attr);
    return false;
}

void shm_controller_init(radio *radio_h, pthread_t *shm_tid)
{
    radio_h_shm = radio_h;

    if (shm_is_created(SYSV_SHM_CONTROLLER_KEY_STR, sizeof(controller_conn)))
    {
        fprintf(stderr, "Connector SHM already exists – destroying and recreating.\n");
        shm_destroy(SYSV_SHM_CONTROLLER_KEY_STR, sizeof(controller_conn));
    }

    shm_create(SYSV_SHM_CONTROLLER_KEY_STR, sizeof(controller_conn));

    controller_conn *connector =
        shm_attach(SYSV_SHM_CONTROLLER_KEY_STR, sizeof(controller_conn));

    connector_local = connector;
    initialize_connector(connector);

    pthread_create(shm_tid, NULL, process_radio_command_thread, (void *) connector);
}

void shm_controller_shutdown(pthread_t *shm_tid)
{
    controller_conn *connector = connector_local;

    pthread_cond_signal(&connector->cmd_condition);
    pthread_mutex_unlock(&connector->cmd_mutex);

    pthread_join(*shm_tid, NULL);
}
