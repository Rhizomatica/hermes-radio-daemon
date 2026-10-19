/* mercury_link - mercury linked into the daemon, as a modem_audio transport.
 * See mercury_link.h; the contract is vendor/mercury/host/mercury_host.h. */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mercury_link.h"
#include "modem_audio.h"
#include "radio_backend.h"
#include "ui_hub.h"
#include "mercury_host.h"     /* host API and lifecycle (vendor/mercury/host) */

#define MAX_ARGS 32

/* The TX clock: a block every CLOCK_MS, while mercury holds PTT and the
 * radio's capture has gone quiet for RX_QUIET_MS (half-duplex closes it
 * for the whole transmission). */
#define CLOCK_MS     20
#define CLOCK_BLOCK  (8000 / 1000 * CLOCK_MS)
#define RX_QUIET_MS  40

static radio          *s_radio;
static bool            s_started;
static pthread_t       s_launcher;
static _Atomic int     s_init_rc = 1;          /* 1 = still starting */
static char            s_argbuf[sizeof(((radio *) 0)->mercury_args)];
static char           *s_argv[MAX_ARGS + 1];
static int             s_argc;
static pthread_mutex_t s_exchange_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int64_t s_last_rx_ms;
static pthread_t       s_clock;
static _Atomic bool    s_clock_run;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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

/* One exchange, from the RX sink or the TX clock; the lock keeps the two
 * from interleaving mercury's TX audio. */
static void exchange(const int16_t *rx, size_t n)
{
    int16_t tx[1024];

    pthread_mutex_lock(&s_exchange_lock);
    mercury_host_audio_exchange(rx, tx, n);
    /* Mercury's audio then silence, all of it while mercury holds PTT. */
    if (modem_audio_tx_keyed_by(MERCURY_LINK_PTT_ID))
        modem_audio_tx_feed(tx, n);
    pthread_mutex_unlock(&s_exchange_lock);
}

static void rx_sink(const int16_t *pcm8k, size_t n, void *user)
{
    (void) user;
    s_last_rx_ms = now_ms();
    while (n)
    {
        size_t chunk = n > 1024 ? 1024 : n;
        exchange(pcm8k, chunk);
        pcm8k += chunk;
        n -= chunk;
    }
}

/* Mercury's TX drains only through the exchange.  A half-duplex radio
 * (main:audio_half_duplex) stops capture while keyed, so the RX sink stops
 * with it: this clock carries the exchange on, at 8 kHz of wall time, until
 * RX blocks flow again. */
static void *tx_clock(void *arg)
{
    struct timespec next;

    (void) arg;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (s_clock_run)
    {
        next.tv_nsec += CLOCK_MS * 1000000L;
        if (next.tv_nsec >= 1000000000L)
        {
            next.tv_sec++;
            next.tv_nsec -= 1000000000L;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        if (modem_audio_tx_keyed_by(MERCURY_LINK_PTT_ID) &&
            now_ms() - s_last_rx_ms > RX_QUIET_MS)
            exchange(NULL, CLOCK_BLOCK);
    }
    return NULL;
}

/* ── UI: everything mercury's own websocket would carry, on radiod's ──
 *
 * Through ui_hub.h, so on every transport radiod has (its websocket now,
 * the Go UI layer later): status, device and radio lists, history (JSON,
 * as is), the spectrum (binary, after UI_HUB_STREAM_MERCURY_SPECTRUM), and
 * the clients' {"command": ...} messages and connects.  The broadcasts run
 * with mercury's UI lock held and only queue a frame, never calling back
 * into mercury_host.h. */

static void ui_broadcast_json(const char *json, void *user)
{
    (void) user;
    ui_hub_broadcast_text(json);
}

static void ui_broadcast_binary(const void *data, size_t len, void *user)
{
    (void) user;
    ui_hub_broadcast_stream(UI_HUB_STREAM_MERCURY_SPECTRUM, data, len);
}

static int ui_command(const char *json, size_t len, void *user)
{
    (void) user;
    return mercury_host_ui_command_json(json, len);
}

static void ui_client_connected(void *user)
{
    (void) user;
    mercury_host_ui_client_connected();
}

static const ui_hub_module s_ui_module = {
    .name = "mercury",
    .command_key = "command",           /* mercury's websocket API */
    .stream_type = UI_HUB_STREAM_MERCURY_SPECTRUM,
    .command = ui_command,
    .client_connected = ui_client_connected,
};

static void ui_detach(void)
{
    ui_hub_unregister_module(s_ui_module.name);   /* waits for calls in flight */
    mercury_host_set_ui(NULL);
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
    /* mercury opens its TNC ports before it sets its UI hooks, and returns
     * from mercury_init only after both: a UI client that connected in
     * between got no history or initial state. Give it them now (to every
     * client: mercury's connect frames are state, so a second copy is
     * harmless). Not from ui_broadcast_json: mercury holds its UI lock there. */
    if (rc == 0)
        mercury_host_ui_client_connected();
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
    mercury_host_ui_ops ui_ops = { .broadcast_json = ui_broadcast_json,
                                   .broadcast_binary = ui_broadcast_binary, .user = NULL };
    mercury_host_set_ui(&ui_ops);
    if (!ui_hub_register_module(&s_ui_module))
        fprintf(stderr, "mercury_link: cannot register with ui_hub; mercury's UI is off\n");

    if (!modem_audio_init(radio_h) || !modem_audio_add_rx_sink(rx_sink, NULL))
    {
        fprintf(stderr, "mercury_link: no modem audio path\n");
        ui_detach();
        mercury_host_set_radio(NULL);
        return false;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (cpus)
        pthread_attr_setaffinity_np(&attr, sizeof(*cpus), cpus);
    s_init_rc = 1;
    s_clock_run = true;
    int err = pthread_create(&s_clock, &attr, tx_clock, NULL);
    if (err == 0)
    {
        err = pthread_create(&s_launcher, &attr, launcher, NULL);
        if (err != 0)
        {
            s_clock_run = false;
            pthread_join(s_clock, NULL);
        }
    }
    pthread_attr_destroy(&attr);
    if (err != 0)
    {
        fprintf(stderr, "mercury_link: pthread_create: %s\n", strerror(err));
        modem_audio_remove_rx_sink(rx_sink, NULL);
        ui_detach();
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

    /* No exchange or UI call after these, as mercury_host.h asks of a host. */
    ui_hub_unregister_module(s_ui_module.name);
    modem_audio_remove_rx_sink(rx_sink, NULL);
    s_clock_run = false;
    pthread_join(s_clock, NULL);
    mercury_request_shutdown();                  /* ends a blocking test mode */
    pthread_join(s_launcher, NULL);
    mercury_shutdown();                          /* no-op if it never started */

    if (modem_audio_tx_keyed_by(MERCURY_LINK_PTT_ID))
        modem_audio_tx_unkey("unkeyed at shutdown");
    mercury_host_set_ui(NULL);
    mercury_host_set_radio(NULL);
    fprintf(stderr, "mercury_link: mercury stopped\n");
}
