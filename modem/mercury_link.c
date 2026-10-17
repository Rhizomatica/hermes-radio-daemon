/* mercury_link - mercury linked into the daemon, as a modem_audio transport.
 * See mercury_link.h; the contract is vendor/mercury/host/mercury_host.h. */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mercury_link.h"
#include "modem_audio.h"
#include "radio_backend.h"
#include "mercury_host.h"     /* host API and lifecycle (vendor/mercury/host) */

#define MAX_ARGS 32

static radio          *s_radio;
static bool            s_started;
static pthread_t       s_launcher;
static _Atomic int     s_init_rc = 1;          /* 1 = still starting */
static char            s_argbuf[sizeof(((radio *) 0)->mercury_args)];
static char           *s_argv[MAX_ARGS + 1];
static int             s_argc;

/* ── radio ops (mercury's TX thread, its UI publisher) ── */

static int key(bool tx, void *user)
{
    (void) user;
    if (tx)
        return modem_audio_tx_begin(MERCURY_LINK_PTT_ID) ? 0 : -1;
    /* Mercury has already waited out the frame and its tail: unkey now.  Only
     * if mercury holds PTT: it also unkeys once at shutdown, and after a
     * key(true) refused because an RTP modem was on the air. */
    if (modem_audio_tx_keyed_by(MERCURY_LINK_PTT_ID))
        modem_audio_tx_unkey("ended");
    return 0;
}

static bool get_frequency(uint64_t *hz, void *user)
{
    (void) user;
    uint32_t active = s_radio->profile_active_idx;
    if (active >= s_radio->profiles_count)
        return false;
    *hz = s_radio->profiles[active].freq;
    return true;
}

/* ── audio: one exchange per block of the radio's audio clock ── */

static void rx_sink(const int16_t *pcm8k, size_t n, void *user)
{
    int16_t tx[1024];

    (void) user;
    while (n)
    {
        size_t chunk = n > 1024 ? 1024 : n;
        mercury_host_audio_exchange(pcm8k, tx, chunk);
        /* Mercury's audio then silence, all of it while mercury holds PTT. */
        if (modem_audio_tx_keyed_by(MERCURY_LINK_PTT_ID))
            modem_audio_tx_feed(tx, chunk);
        pcm8k += chunk;
        n -= chunk;
    }
}

/* ── lifecycle ── */

/* mercury_init returns once the engine is up, or blocks until shutdown in
 * its test modes (-t / -r), so it runs on a thread of its own; the engine's
 * threads inherit this thread's CPU mask. */
static void *launcher(void *arg)
{
    (void) arg;
    int rc = mercury_init(s_argc, s_argv, s_radio->mercury_config, NULL);
    s_init_rc = rc;
    if (rc == MERCURY_INIT_CONFIG_ERROR)
        fprintf(stderr, "mercury_link: cannot read %s; the radio runs without mercury\n",
                s_radio->mercury_config);
    else if (rc != 0)
        fprintf(stderr, "mercury_link: mercury did not start (%d); the radio runs without it\n", rc);
    return NULL;
}

static void build_argv(const radio *radio_h)
{
    s_argc = 0;
    s_argv[s_argc++] = "mercury";
    s_argv[s_argc++] = "-C";
    s_argv[s_argc++] = (char *) radio_h->mercury_config;

    snprintf(s_argbuf, sizeof(s_argbuf), "%s", radio_h->mercury_args);
    char *save = NULL;
    for (char *tok = strtok_r(s_argbuf, " \t", &save); tok && s_argc < MAX_ARGS;
         tok = strtok_r(NULL, " \t", &save))
        s_argv[s_argc++] = tok;
    s_argv[s_argc] = NULL;
}

bool mercury_link_start(radio *radio_h, const cpu_set_t *cpus)
{
    if (s_started)
        return true;

    s_radio = radio_h;
    build_argv(radio_h);

    mercury_host_radio_ops ops = { .key = key, .get_frequency = get_frequency, .user = NULL };
    mercury_host_set_radio(&ops);

    if (!modem_audio_init(radio_h) || !modem_audio_add_rx_sink(rx_sink, NULL))
    {
        fprintf(stderr, "mercury_link: no modem audio path\n");
        mercury_host_set_radio(NULL);
        return false;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (cpus)
        pthread_attr_setaffinity_np(&attr, sizeof(*cpus), cpus);
    s_init_rc = 1;
    int err = pthread_create(&s_launcher, &attr, launcher, NULL);
    pthread_attr_destroy(&attr);
    if (err != 0)
    {
        fprintf(stderr, "mercury_link: pthread_create: %s\n", strerror(err));
        modem_audio_remove_rx_sink(rx_sink, NULL);
        mercury_host_set_radio(NULL);
        return false;
    }

    s_started = true;
    fprintf(stderr, "mercury_link: mercury starting (-C %s%s%s)\n", radio_h->mercury_config,
            radio_h->mercury_args[0] ? " " : "", radio_h->mercury_args);
    return true;
}

void mercury_link_stop(void)
{
    if (!s_started)
        return;
    s_started = false;

    modem_audio_remove_rx_sink(rx_sink, NULL);   /* no exchange after this */
    mercury_request_shutdown();                  /* ends a blocking test mode */
    pthread_join(s_launcher, NULL);
    mercury_shutdown();                          /* no-op if it never started */

    if (modem_audio_tx_keyed_by(MERCURY_LINK_PTT_ID))
        modem_audio_tx_unkey("unkeyed at shutdown");
    mercury_host_set_radio(NULL);
    fprintf(stderr, "mercury_link: mercury stopped\n");
}
