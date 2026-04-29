/* hermes-radio-daemon - backend-neutral daemon core
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE

#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cfg_utils.h"
#include "radio_backend.h"
#include "radio_daemon_core.h"
#include "radio_media.h"
#include "radio_pipeline.h"
#include "radio_shm.h"
#include "radio_websocket.h"

extern _Atomic bool shutdown_;

static void exit_radio(int sig)
{
    (void) sig;
    printf("Caught signal – shutting down...\n");
    shutdown_ = true;

    sleep(5);
    exit(EXIT_FAILURE);
}

static void install_signal_handlers(void)
{
    signal(SIGINT, exit_radio);
    signal(SIGQUIT, exit_radio);
    signal(SIGTERM, exit_radio);
    signal(SIGPIPE, SIG_IGN);
}

static void maybe_pin_cpu(int cpu_nr)
{
    if (cpu_nr < 0)
        return;

    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu_nr, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) == 0)
        printf("Running on CPU %d\n", sched_getcpu());
    else
        perror("sched_setaffinity");
}

int radio_daemon_core_run(const radio_backend_selection *selection,
                          const radio_daemon_runtime *runtime)
{
    radio radio_h;
    pthread_t cfg_tid = 0;
    pthread_t io_tid = 0;
    pthread_t shm_tid = 0;
    pthread_t capture_tid = 0;
    pthread_t playback_tid = 0;
    pthread_t websocket_tid = 0;
    bool cfg_started = false;
    bool backend_started = false;
    bool io_started = false;
    bool media_started = false;
    bool websocket_started = false;
    bool shm_started = false;

    shutdown_ = false;
    install_signal_handlers();

    memset(&radio_h, 0, sizeof(radio_h));
    pthread_mutex_init(&radio_h.message_mutex, NULL);
    radio_backend_configure(&radio_h, selection);

    maybe_pin_cpu(runtime->cpu_nr);

    if (!cfg_init(&radio_h, runtime->cfg_radio_path, runtime->cfg_user_path, &cfg_tid))
    {
        fprintf(stderr, "Failed to load configuration. Exiting.\n");
        goto fail;
    }
    cfg_started = true;
    radio_backend_configure(&radio_h, selection);
    radio_pipeline_refresh(&radio_h);

    if (!radio_backend_init(&radio_h))
    {
        fprintf(stderr, "Failed to initialize %s backend. Exiting.\n",
                radio_h.backend_ops ? radio_h.backend_ops->name : "radio");
        goto fail;
    }
    backend_started = true;

    if (pthread_create(&io_tid, NULL, radio_backend_io_thread, &radio_h) != 0)
    {
        perror("pthread_create(io)");
        shutdown_ = true;
        goto fail;
    }
    io_started = true;

    if (!radio_media_init(&radio_h, &capture_tid, &playback_tid))
    {
        fprintf(stderr, "Failed to initialize media bridge. Exiting.\n");
        shutdown_ = true;
        goto fail;
    }
    media_started = true;

    if (!radio_websocket_init(&radio_h, &websocket_tid))
    {
        fprintf(stderr, "Failed to initialize websocket service. Exiting.\n");
        shutdown_ = true;
        goto fail;
    }
    websocket_started = radio_h.enable_websocket;

    if (radio_h.enable_shm_control)
    {
        shm_controller_init(&radio_h, &shm_tid);
        shm_started = true;
    }

    pthread_join(io_tid, NULL);
    io_started = false;

    if (shm_started)
        shm_controller_shutdown(&shm_tid);
    if (websocket_started)
        radio_websocket_shutdown(&websocket_tid);
    if (media_started)
        radio_media_shutdown(&radio_h, &capture_tid, &playback_tid);
    if (backend_started)
        radio_backend_shutdown(&radio_h);
    if (cfg_started)
        cfg_shutdown(&radio_h, &cfg_tid);
    pthread_mutex_destroy(&radio_h.message_mutex);

    printf("radio_daemon: clean shutdown complete.\n");
    return EXIT_SUCCESS;

fail:
    if (io_started)
        pthread_join(io_tid, NULL);
    if (shm_started)
        shm_controller_shutdown(&shm_tid);
    if (websocket_started)
        radio_websocket_shutdown(&websocket_tid);
    if (media_started)
        radio_media_shutdown(&radio_h, &capture_tid, &playback_tid);
    if (backend_started)
        radio_backend_shutdown(&radio_h);
    if (cfg_started)
        cfg_shutdown(&radio_h, &cfg_tid);
    pthread_mutex_destroy(&radio_h.message_mutex);
    return EXIT_FAILURE;
}
