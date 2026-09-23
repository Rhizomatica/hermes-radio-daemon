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
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>

#include "radio_backend.h"
#include "radio.h"
#include "radio_shm.h"
#include "shm_utils.h"
#include "include/sbitx_io.h"
#include "include/radio_cmds.h"

extern _Atomic bool shutdown_;

static controller_conn *connector_local;
static radio *radio_h_shm;

/* The client process that keyed PTT over this connector, watched so that
 * PTT is released if it exits without unkeying (killed mid-transmission,
 * crashed, forced exit). pidfd when the kernel has it, else pid polling. */
static pid_t shm_keyer_pid = 0;
static int shm_keyer_pidfd = -1;

/* Which process sent the command being processed. The protocol carries no
 * pid, but radio_cmd() holds response_mutex from before it posts the
 * command until it has read the reply, and glibc records the holder's
 * thread id in the mutex; map that to its process. 0 when unknown. */
static pid_t shm_command_sender(controller_conn *conn)
{
#ifdef __GLIBC__
    pid_t tid = conn->response_mutex.__data.__owner;
    if (tid <= 0)
        return 0;

    char path[64], line[128];
    snprintf(path, sizeof(path), "/proc/%d/status", (int) tid);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    pid_t pid = 0;
    while (fgets(line, sizeof(line), f))
    {
        if (sscanf(line, "Tgid: %d", &pid) == 1)
            break;
    }
    fclose(f);
    return pid;
#else
    (void) conn;
    return 0;
#endif
}

static void shm_keyer_forget(void)
{
    if (shm_keyer_pidfd >= 0)
        close(shm_keyer_pidfd);
    shm_keyer_pidfd = -1;
    shm_keyer_pid = 0;
}

/* The command-line clients send one command and exit. The API keys the
 * radio that way (the web panel's PTT button runs "sbitx_client -c
 * ptt_on", later "ptt_off"), so their PTT is latched until a PTT-off, as
 * with a front-panel switch; releasing it when they exit would unkey the
 * radio 200 ms after every such key. Old installs run hermes-net's
 * sbitx_client, hence names rather than a protocol flag. */
static bool shm_client_is_one_shot(const char *comm)
{
    return !strcmp(comm, "sbitx_client") || !strcmp(comm, "radio_client") ||
           !strcmp(comm, "ubitx_client");
}

static void shm_keyer_watch(pid_t pid)
{
    shm_keyer_forget();
    if (pid <= 0)
    {
        fprintf(stderr, "radio_shm: PTT ON from an unknown client; "
                        "it will not be released if the client dies\n");
        return;
    }

    char path[64], comm[32] = "?";
    snprintf(path, sizeof(path), "/proc/%d/comm", (int) pid);
    FILE *f = fopen(path, "r");
    if (f)
    {
        if (fgets(comm, sizeof(comm), f))
            comm[strcspn(comm, "\n")] = '\0';
        fclose(f);
    }

    if (shm_client_is_one_shot(comm))
    {
        printf("radio_shm: PTT ON from %s (pid %d), latched until PTT off\n",
               comm, (int) pid);
        return;
    }

    /* Name the process once per keyer, not on every over. */
    static pid_t last_named = 0;
    if (pid != last_named)
    {
        printf("radio_shm: PTT keyed by pid %d (%s)\n", (int) pid, comm);
        last_named = pid;
    }

    shm_keyer_pid = pid;
#ifdef SYS_pidfd_open
    shm_keyer_pidfd = (int) syscall(SYS_pidfd_open, pid, 0);
#endif
}

static bool shm_keyer_gone(void)
{
    if (shm_keyer_pid <= 0)
        return false;

    if (shm_keyer_pidfd >= 0)
    {
        struct pollfd pfd = { .fd = shm_keyer_pidfd, .events = POLLIN };
        return poll(&pfd, 1, 0) > 0;
    }

    return kill(shm_keyer_pid, 0) < 0 && errno == ESRCH;
}

/* Called every command-loop pass (at least every 200 ms). */
static void shm_check_keyer(radio *radio_h)
{
    if (!shm_keyer_gone())
        return;

    pid_t pid = shm_keyer_pid;
    shm_keyer_forget();
    radio_backend_release_ptt(radio_h, PTT_SRC_SHM, pid, "exited");
}

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
            pid_t pid = shm_command_sender(connector_local);
            shm_keyer_watch(pid);
            radio_backend_set_ptt(radio_h, IN_TX, PTT_SRC_SHM, pid);
            response[0] = CMD_RESP_ACK;
        }
        else
        {
            response[0] = CMD_RESP_PTT_ON_NACK;
        }
        break;

    case CMD_PTT_OFF:
    {
        /* Always unkey, whatever the cached state or SWR protection say: a
         * PTT-off that is refused because the cache already reads RX (a
         * client restarted to clear a stuck transmitter, say) leaves the
         * radio keyed. The replies keep their old meaning. */
        bool was_tx = radio_h->txrx_state == IN_TX;
        shm_keyer_forget();
        radio_backend_set_ptt(radio_h, IN_RX, PTT_SRC_SHM,
                              shm_command_sender(connector_local));
        if (radio_h->swr_protection_enabled)
            response[0] = CMD_ALERT_PROTECTION_ON;
        else if (was_tx)
            response[0] = CMD_RESP_ACK;
        else
            response[0] = CMD_RESP_PTT_OFF_NACK;
        break;
    }

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
        if (profile < radio_h->profiles_count)
        {
            /* See radio_cmds.h for the cmd[0] -> MODE_* mapping. */
            uint16_t mode_v;
            switch (cmd[0])
            {
            case 0x00: mode_v = MODE_LSB;  break;
            case 0x01: mode_v = MODE_USB;  break;
            case 0x02: mode_v = MODE_FM;   break;
            case 0x03: mode_v = MODE_AM;   break;
            case 0x04: mode_v = MODE_CW;   break;
            case 0x05: mode_v = MODE_DRM;  break;
            case 0x06: mode_v = MODE_FT8;  break;
            case 0x07: mode_v = MODE_RTTY; break;
            case 0x08: mode_v = MODE_DSTAR; break;
            default:
                response[0] = CMD_RESP_WRONG_COMMAND;
                break;
            }
            if (response[0] == CMD_RESP_ACK)
                radio_backend_set_mode(radio_h, mode_v, profile);
        }
        break;

    case CMD_GET_MODE:
        profile = cmd[4] >> 6;
        if (profile >= radio_h->profiles_count)
        {
            response[0] = CMD_RESP_WRONG_COMMAND;
            break;
        }
        switch (radio_h->profiles[profile].mode)
        {
        case MODE_LSB:  response[0] = CMD_RESP_GET_MODE_LSB;  break;
        case MODE_USB:  response[0] = CMD_RESP_GET_MODE_USB;  break;
        case MODE_CW:   response[0] = CMD_RESP_GET_MODE_CW;   break;
        case MODE_FM:   response[0] = CMD_RESP_GET_MODE_FM;   break;
        case MODE_AM:   response[0] = CMD_RESP_GET_MODE_AM;   break;
        case MODE_DRM:  response[0] = CMD_RESP_GET_MODE_DRM;  break;
        case MODE_FT8:  response[0] = CMD_RESP_GET_MODE_FT8;  break;
        case MODE_RTTY: response[0] = CMD_RESP_GET_MODE_RTTY; break;
        case MODE_DSTAR: response[0] = CMD_RESP_GET_MODE_DSTAR; break;
        default:        response[0] = CMD_RESP_GET_MODE_USB;  break;
        }
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

/* Latch a client-written station message into the radio struct, where the
 * websocket state frames pick it up. The writer is another process with no
 * lock on message[] (patched uucico sprintfs transfer progress like
 * "Sending: <file>." straight into the connector SHM and flips
 * message_available) — same consumer contract as the legacy
 * sbitx_websocket.c loop: copy, force termination, clear the flag. */
static void pump_connector_message(controller_conn *conn, radio *radio_h)
{
    if (!conn->message_available)
        return;

    size_t n = MAX_MESSAGE_SIZE < RADIO_MESSAGE_MAX
               ? MAX_MESSAGE_SIZE : RADIO_MESSAGE_MAX;
    pthread_mutex_lock(&radio_h->message_mutex);
    memcpy(radio_h->message, (const void *) conn->message, n);
    radio_h->message[n - 1] = '\0';
    pthread_mutex_unlock(&radio_h->message_mutex);

    conn->message_available = false;
    radio_h->message_available = true;
}

/* SHM command dispatcher thread.
 *
 * Uses pthread_cond_timedwait so the worker periodically observes shutdown_
 * even when no client is signalling cmd_condition. */
static void *process_radio_command_thread(void *arg)
{
    controller_conn *conn = arg;

    /* cmd_mutex is robust; a previous daemon (or client) may have died while
     * holding it. Recover instead of deadlocking forever. */
    if (pthread_mutex_lock(&conn->cmd_mutex) == EOWNERDEAD)
        pthread_mutex_consistent(&conn->cmd_mutex);

    while (!shutdown_)
    {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 200 * 1000 * 1000L; /* 200 ms */
        if (deadline.tv_nsec >= 1000000000L)
        {
            deadline.tv_sec  += 1;
            deadline.tv_nsec -= 1000000000L;
        }

        int rc = pthread_cond_timedwait(&conn->cmd_condition,
                                        &conn->cmd_mutex,
                                        &deadline);
        if (rc == EOWNERDEAD)
            pthread_mutex_consistent(&conn->cmd_mutex);

        if (shutdown_)
            break;

        /* Runs at least every 200 ms (each timedwait expiry). */
        pump_connector_message(conn, radio_h_shm);
        shm_check_keyer(radio_h_shm);

        /* Don't key off rc alone: a client can slip its command in exactly at
         * the timeout boundary (it grabs cmd_mutex the moment the expiring
         * wait releases it, writes service_command, signals — no waiter, so
         * the signal is lost — and unlocks; we then reacquire with
         * rc==ETIMEDOUT). Commands are detected by a pending opcode instead:
         * opcode 0x00 is unused by the protocol and cleared after each
         * command below, so nonzero == work to do. */
        if (conn->service_command[4] == 0)
            continue;

        process_radio_command(conn->service_command, conn->response_service);

        if (conn->service_command[4] == CMD_RADIO_RESET)
        {
            shutdown_ = true;
            fprintf(stderr, "\nReset command. Exiting\n");
            break;
        }

        conn->service_command[4] = 0;   /* consumed */
        conn->response_available = true;
    }

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

    controller_conn *connector = NULL;

    /* Reuse an existing segment instead of destroy+recreate. Long-running
     * clients (uucpd sets connected status / SNR / bitrate over this
     * connector) attach once at startup; recreating the segment here would
     * orphan their mapping, and every radio_cmd() they issue afterwards
     * lands in the dead segment and is silently lost — the classic symptom
     * is the UI never showing "connected" after a daemon restart. */
    if (shm_is_created(SYSV_SHM_CONTROLLER_KEY_STR, sizeof(controller_conn)))
    {
        connector = shm_attach(SYSV_SHM_CONTROLLER_KEY_STR, sizeof(controller_conn));
        if (connector)
        {
            fprintf(stderr, "Connector SHM already exists – reusing "
                            "(keeps attached clients alive).\n");
            /* Drop any half-written stale command from a previous run; the
             * worker treats a nonzero opcode as pending work. */
            connector->service_command[4] = 0;
            connector->response_available = false;
        }
        else
        {
            /* Attach failed: stale segment from an incompatible (smaller)
             * layout. Size 0 matches whatever segment holds the key. */
            fprintf(stderr, "Connector SHM exists but is unusable – recreating.\n");
            shm_destroy(SYSV_SHM_CONTROLLER_KEY_STR, 0);
        }
    }

    if (!connector)
    {
        shm_create(SYSV_SHM_CONTROLLER_KEY_STR, sizeof(controller_conn));
        connector = shm_attach(SYSV_SHM_CONTROLLER_KEY_STR, sizeof(controller_conn));
        initialize_connector(connector);
    }

    connector_local = connector;

    pthread_create(shm_tid, NULL, process_radio_command_thread, (void *) connector);
}

void shm_controller_shutdown(pthread_t *shm_tid)
{
    controller_conn *connector = connector_local;

    /* shutdown_ is already set by the caller; nudge the worker so it doesn't
     * have to wait for its next 200 ms timeout to observe the flag. The worker
     * is the sole owner of cmd_mutex, so we must not unlock it from here. */
    if (connector)
        pthread_cond_signal(&connector->cmd_condition);

    pthread_join(*shm_tid, NULL);
}
