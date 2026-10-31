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

#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <errno.h>
#include "cfg_utils.h"
#include "radiod_log.h"
#include "radio_backend.h"
#include "radio_daemon_core.h"
#include "radio_media.h"
#include "radio_pipeline.h"
#include "radio_shm.h"
#include "radio_websocket.h"
#include "audio_headset.h"
#include "shm_audio.h"
#include "loop_audio.h"
#include "rtp_audio.h"
#include "modem_audio.h"
#ifdef HAVE_MERCURY
#include "mercury_link.h"
#endif
#include "voice_crypto.h"
#include "gps.h"
#include "sbitx/sbitx_alsa.h"

#define LOG_C "radiod"

/* systemd's sd_notify("READY=1") without libsystemd: one datagram to
 * $NOTIFY_SOCKET (a leading '@' is an abstract socket). No-op when not
 * started by systemd with Type=notify. */
static void notify_systemd_ready(void)
{
    const char *path = getenv("NOTIFY_SOCKET");
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    size_t len;
    int fd;

    if (!path || (path[0] != '/' && path[0] != '@') || (len = strlen(path)) >= sizeof(sa.sun_path))
        return;
    memcpy(sa.sun_path, path, len);
    if (sa.sun_path[0] == '@')
        sa.sun_path[0] = '\0';
    if ((fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0)
        return;
    if (sendto(fd, "READY=1", 7, 0, (struct sockaddr *) &sa,
               (socklen_t) (offsetof(struct sockaddr_un, sun_path) + len)) < 0)
        HLOGW(LOG_C, "sd_notify READY failed: %s", strerror(errno));
    close(fd);
}

extern _Atomic bool shutdown_;

/* Async-signal-safe handler. POSIX guarantees only a small set of
 * functions are safe to call from a signal handler — printf, sleep,
 * exit, and most stdio are NOT on that list. The previous handler
 * called all three and forcibly exited after 5s, dropping any unflushed
 * config writes. The new handler just sets the atomic flag and writes
 * a short status line via write(2); the main thread observes the flag
 * and runs orderly teardown (config flush + thread joins). */
static void exit_radio(int sig)
{
    static const char msg[] = "radio_daemon: caught signal, shutting down\n";
    (void) sig;
    shutdown_ = true;
    /* write(2) is async-signal-safe; ignore short writes. */
    ssize_t r = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void) r;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = exit_radio;
    sigemptyset(&sa.sa_mask);
    /* Don't restart syscalls; we want blocking reads/poll to return
     * EINTR so the worker threads can observe shutdown_ promptly.
     * SA_ONSTACK: with the Go API layer linked in, a signal can land on a
     * Go thread, whose own stack is small; the handler must run on the
     * alternate signal stack Go gives each thread. */
    sa.sa_flags = SA_ONSTACK;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ign, NULL);
}

static void maybe_pin_cpu(int cpu_nr)
{
    if (cpu_nr < 0)
        return;

    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu_nr, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) == 0)
        HLOGI(LOG_C, "running on CPU %d", sched_getcpu());
    else
        HLOGE(LOG_C, "cannot pin to CPU %d: %s", cpu_nr, strerror(errno));
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
    bool shm_audio_started = false;
    bool loop_audio_started = false;
    bool rtp_audio_started = false;
#ifdef HAVE_MERCURY
    bool mercury_started = false;
#endif
    cpu_set_t unpinned_cpus;
    bool websocket_started = false;
    bool shm_started = false;
    bool headset_started = false;

    shutdown_ = false;
    install_signal_handlers();

    memset(&radio_h, 0, sizeof(radio_h));
    pthread_mutex_init(&radio_h.message_mutex, NULL);
    radio_backend_configure(&radio_h, selection);

    /* What the process may run on before -c pins this thread (and every
     * thread it starts) to one CPU: mercury runs on this, off the DSP core. */
    CPU_ZERO(&unpinned_cpus);
    sched_getaffinity(0, sizeof(unpinned_cpus), &unpinned_cpus);
    maybe_pin_cpu(runtime->cpu_nr);

    if (!cfg_init(&radio_h, runtime->cfg_radio_path, runtime->cfg_user_path, &cfg_tid))
    {
        HLOGE(LOG_C, "cannot load the configuration (%s, %s); exiting",
              runtime->cfg_radio_path, runtime->cfg_user_path);
        goto fail;
    }
    cfg_started = true;
    radiod_log_configure(&radio_h);

    if (radio_h.voice_key_file[0] != '\0')
        voice_crypto_load_key_file(radio_h.voice_key_file);
    if (radio_h.dstar_encrypt && !voice_crypto_have_key())
        HLOGW(LOG_C, "dstar_encrypt = 1 but no voice key is loaded (voice_key_file): "
                     "encrypted overs will go out as silence, never in the clear");
    if (radio_h.rade_encrypt && !voice_crypto_have_key())
        HLOGW(LOG_C, "rade_encrypt = 1 but no voice key is loaded (voice_key_file): "
                     "encrypted overs will go out as silence, never in the clear");
    radio_backend_configure(&radio_h, selection);
    radio_pipeline_refresh(&radio_h);

    if (!radio_backend_init(&radio_h))
    {
        HLOGE(LOG_C, "cannot start the %s backend; exiting",
              radio_h.backend_ops ? radio_h.backend_ops->name : "radio");
        goto fail;
    }
    backend_started = true;

    if (pthread_create(&io_tid, NULL, radio_backend_io_thread, &radio_h) != 0)
    {
        HLOGE(LOG_C, "cannot start the radio I/O thread: %s", strerror(errno));
        shutdown_ = true;
        goto fail;
    }
    io_started = true;

    if (!radio_media_init(&radio_h, &capture_tid, &playback_tid))
    {
        HLOGE(LOG_C, "cannot start the media bridge; exiting");
        shutdown_ = true;
        goto fail;
    }
    media_started = true;

    if (radio_h.enable_shm_audio)
        shm_audio_started = shm_audio_init(&radio_h);

    /* The sBitx runs its own snd-aloop cables (sbitx_alsa.c), under the
     * same switch. */
    if (radio_h.enable_loop_audio && radio_h.backend_kind != RADIO_BACKEND_HFSIGNALS)
        loop_audio_started = loop_audio_init(&radio_h);

    if (radio_h.enable_rtp_audio)
        rtp_audio_started = rtp_audio_init(&radio_h);

    if (radio_h.enable_mercury)
    {
#ifdef HAVE_MERCURY
        mercury_started = mercury_link_start(&radio_h, &unpinned_cpus);
#else
        HLOGE(LOG_C, "main:enable_mercury is set, but this radio_daemon was built "
                     "without mercury (vendor/mercury not checked out); running without the modem");
#endif
    }

    /* The station's GPS reader (gpsd's JSON socket).  Started here, next to
     * the other modules, so it does not depend on the websocket: recording
     * and the over-the-air position reports need it too. */
    gps_init(&radio_h);

    if (!radio_websocket_init(&radio_h, &websocket_tid))
    {
        HLOGE(LOG_C, "cannot start the websocket service; exiting");
        shutdown_ = true;
        goto fail;
    }
    websocket_started = radio_h.enable_websocket;

    if (radio_h.enable_shm_control)
    {
        shm_controller_init(&radio_h, &shm_tid);
        shm_started = true;
    }

    /* Optional local-headset path. No-op when devices aren't configured. */
    if (audio_headset_init(&radio_h))
        headset_started = true;

    /* Tell systemd we are up (Type=notify). On the sBitx, wait until the
     * codec and the snd-aloop cables (when enable_loop_audio runs them) are
     * configured first: the modem must
     * open the cables after radiod, which sets the period the codec timer
     * needs (see loop_check_period). Bounded (past the codec's busy wait),
     * so a missing loopback cannot hold the unit in "activating"; but a
     * radio without its codec hears and sends nothing, so that is fatal:
     * the unit fails where it can be seen, and systemd's restart applies. */
    if (radio_h.backend_kind == RADIO_BACKEND_HFSIGNALS)
    {
        bool codec_ok;
        bool loop_ok = sound_system_wait_ready(15000, &codec_ok);

        if (!codec_ok)
        {
            HLOGE(LOG_C, "the audio codec could not be opened (see above); "
                         "a radio without it can neither hear nor transmit; exiting");
            shutdown_ = true;
            goto fail;
        }
        if (!loop_ok)
            HLOGW(LOG_C, "loopback devices not configured after 15 s; reporting ready anyway");
    }
    notify_systemd_ready();

    pthread_join(io_tid, NULL);
    io_started = false;

    gps_shutdown();

    if (headset_started)
        audio_headset_shutdown(&radio_h);
    if (shm_started)
        shm_controller_shutdown(&shm_tid);
    if (websocket_started)
        radio_websocket_shutdown(&websocket_tid);
    if (shm_audio_started)
        shm_audio_shutdown();
    if (loop_audio_started)
        loop_audio_shutdown();
    if (rtp_audio_started)
        rtp_audio_shutdown();
#ifdef HAVE_MERCURY
    if (mercury_started)
        mercury_link_stop();
#endif
    modem_audio_shutdown();         /* after every modem transport */
    if (media_started)
        radio_media_shutdown(&radio_h, &capture_tid, &playback_tid);
    if (backend_started)
        radio_backend_shutdown(&radio_h);
    if (cfg_started)
        cfg_shutdown(&radio_h, &cfg_tid);
    pthread_mutex_destroy(&radio_h.message_mutex);

    HLOGI(LOG_C, "clean shutdown complete");
    return EXIT_SUCCESS;

fail:
    if (io_started)
        pthread_join(io_tid, NULL);
    gps_shutdown();
    if (headset_started)
        audio_headset_shutdown(&radio_h);
    if (shm_started)
        shm_controller_shutdown(&shm_tid);
    if (websocket_started)
        radio_websocket_shutdown(&websocket_tid);
    if (shm_audio_started)
        shm_audio_shutdown();
    if (loop_audio_started)
        loop_audio_shutdown();
    if (rtp_audio_started)
        rtp_audio_shutdown();
#ifdef HAVE_MERCURY
    if (mercury_started)
        mercury_link_stop();
#endif
    modem_audio_shutdown();         /* after every modem transport */
    if (media_started)
        radio_media_shutdown(&radio_h, &capture_tid, &playback_tid);
    if (backend_started)
        radio_backend_shutdown(&radio_h);
    if (cfg_started)
        cfg_shutdown(&radio_h, &cfg_tid);
    pthread_mutex_destroy(&radio_h.message_mutex);
    return EXIT_FAILURE;
}
