/* hermes-radio-daemon - main daemon
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

#define _GNU_SOURCE

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <sched.h>
#include <pthread.h>

#include "radio.h"
#include "radio_hamlib.h"
#include "radio_media.h"
#include "radio_shm.h"
#include "radio_websocket.h"
#include "cfg_utils.h"

_Atomic bool shutdown_ = false;

static void exit_radio(int sig)
{
    (void) sig;
    printf("Caught signal – shutting down...\n");
    shutdown_ = true;

    /* Force exit if shutdown takes too long */
    sleep(5);
    exit(EXIT_FAILURE);
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s [-r radio.ini] [-u user.ini] [-c cpu_nr] [-h]\n\n"
            "Options:\n"
            "  -r radio.ini   Path to radio/hardware config  (default: %s)\n"
            "  -u user.ini    Path to user/profile config    (default: %s)\n"
            "  -c cpu_nr      Pin process to CPU cpu_nr      (default: no pinning)\n"
            "                 Use -1 to disable CPU pinning\n"
            "  -h             Show this help\n",
            prog, CFG_RADIO_PATH, CFG_USER_PATH);
}

int main(int argc, char *argv[])
{
    radio     radio_h;
    pthread_t cfg_tid;      /* configuration writer thread */
    pthread_t io_tid;       /* periodic I/O / timer thread */
    pthread_t shm_tid;      /* SHM command thread */
    pthread_t capture_tid;  /* ALSA capture thread */
    pthread_t playback_tid; /* ALSA playback thread */
    pthread_t websocket_tid;/* websocket server thread */

    const char *cfg_radio_path = CFG_RADIO_PATH;
    const char *cfg_user_path  = CFG_USER_PATH;
    int cpu_nr = -1; /* -1 = no pinning */

    int opt;
    while ((opt = getopt(argc, argv, "hr:u:c:")) != -1)
    {
        switch (opt)
        {
        case 'r':
            cfg_radio_path = optarg;
            break;
        case 'u':
            cfg_user_path = optarg;
            break;
        case 'c':
            cpu_nr = atoi(optarg);
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    signal(SIGINT,  exit_radio);
    signal(SIGQUIT, exit_radio);
    signal(SIGTERM, exit_radio);
    signal(SIGPIPE, SIG_IGN);

    memset(&radio_h, 0, sizeof(radio));
    pthread_mutex_init(&radio_h.message_mutex, NULL);

    if (cpu_nr >= 0)
    {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(cpu_nr, &mask);
        if (sched_setaffinity(0, sizeof(mask), &mask) == 0)
            printf("Running on CPU %d\n", sched_getcpu());
        else
            perror("sched_setaffinity");
    }

    /* 1. Load configuration */
    if (!cfg_init(&radio_h, cfg_radio_path, cfg_user_path, &cfg_tid))
    {
        fprintf(stderr, "Failed to load configuration. Exiting.\n");
        return EXIT_FAILURE;
    }

    /* 2. Open the Hamlib rig */
    if (!radio_hamlib_init(&radio_h))
    {
        fprintf(stderr, "Failed to initialize Hamlib rig. Exiting.\n");
        cfg_shutdown(&radio_h, &cfg_tid);
        return EXIT_FAILURE;
    }

    /* 3. Start the periodic I/O thread */
    pthread_create(&io_tid, NULL, radio_io_thread, (void *) &radio_h);

    /* 4. Start media services */
    if (!radio_media_init(&radio_h, &capture_tid, &playback_tid))
    {
        fprintf(stderr, "Failed to initialize media bridge. Exiting.\n");
        shutdown_ = true;
        pthread_join(io_tid, NULL);
        radio_hamlib_shutdown(&radio_h);
        cfg_shutdown(&radio_h, &cfg_tid);
        return EXIT_FAILURE;
    }

    if (!radio_websocket_init(&radio_h, &websocket_tid))
    {
        fprintf(stderr, "Failed to initialize websocket service. Exiting.\n");
        shutdown_ = true;
        pthread_join(io_tid, NULL);
        radio_media_shutdown(&radio_h, &capture_tid, &playback_tid);
        radio_hamlib_shutdown(&radio_h);
        cfg_shutdown(&radio_h, &cfg_tid);
        return EXIT_FAILURE;
    }

    /* 5. Start the SHM control interface */
    if (radio_h.enable_shm_control)
        shm_controller_init(&radio_h, &shm_tid);

    /* 6. Wait for shutdown signal */
    pthread_join(io_tid, NULL);

    /* 7. Teardown in reverse order */
    if (radio_h.enable_shm_control)
        shm_controller_shutdown(&shm_tid);

    radio_websocket_shutdown(&websocket_tid);
    radio_media_shutdown(&radio_h, &capture_tid, &playback_tid);
    radio_hamlib_shutdown(&radio_h);
    cfg_shutdown(&radio_h, &cfg_tid);
    pthread_mutex_destroy(&radio_h.message_mutex);

    printf("radio_daemon: clean shutdown complete.\n");
    return EXIT_SUCCESS;
}
