/* pskreporter - reports the stations this one decodes to PSKReporter.
 * See pskreporter.h; the rules quoted are https://pskreporter.info/pskdev.html.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pskreporter.h"
#include "pskreporter_ipfix.h"
#include "digi_log.h"
#include "gps.h"
#include "radiod_log.h"

#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LOG_C "pskreporter"

#define PSKR_INTERVAL_S         300   /* "no more than one datagram every five minutes" */
#define PSKR_JITTER_S           60    /* added at random to each: not in step with the clock */
#define PSKR_TEMPLATES_FIRST    3     /* "the first three datagrams" carry the templates */
#define PSKR_TEMPLATES_EVERY_S  3600  /* and then "at least once per hour" */
#define PSKR_QUEUE_MAX          1024  /* spots waiting; past it the newest are dropped */
#define PSKR_SEND_EARLY         80    /* spots that fill a datagram: send them now */
#define PSKR_SOFTWARE           "HERMES radiod"
#define PSKR_SOFTWARE_MAX       64
#define PSKR_HOST_MAX           128

static radio          *g_radio;
static pthread_t       g_tid;
static atomic_bool     g_started;
static bool            g_stop;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_wake = PTHREAD_COND_INITIALIZER;

/* Under g_lock. */
static pskr_spot g_queue[PSKR_QUEUE_MAX];
static int       g_queued;
static unsigned  g_dropped;
static pskr_seen g_seen;

/* The sender's; the thread's alone. */
static int      g_sock = -1;
static int      g_sock_family;
static uint32_t g_id;
static uint32_t g_sequence;
static unsigned g_datagrams;
static time_t   g_templates_at;
static unsigned g_jitter_seed;

/* digi_log's listener: an FT8 sender heard, queued if it is due. */
static void heard(const digi_log_entry *e, double at)
{
    if (!e->dir || strcmp(e->dir, "rx") || !e->mode || strcmp(e->mode, "FT8") ||
        !e->has_snr || !pskr_call_ok(e->from))
        return;
    uint64_t hz = e->freq_hz + (e->has_offset ? (int64_t) e->offset_hz : 0);
    if (hz == 0 || hz > UINT32_MAX)
        return;
    pskr_spot s = {0};
    snprintf(s.call, sizeof s.call, "%s", e->from);
    if (e->grid && strlen(e->grid) < sizeof s.locator)
        snprintf(s.locator, sizeof s.locator, "%s", e->grid);
    snprintf(s.mode, sizeof s.mode, "%s", e->mode);
    s.freq_hz = (uint32_t) hz;
    long snr = lroundf(e->snr_db);
    s.snr_db = (int8_t) (snr < INT8_MIN ? INT8_MIN : snr > INT8_MAX ? INT8_MAX : snr);
    s.at = (uint32_t) at;

    pthread_mutex_lock(&g_lock);
    if (pskr_seen_due(&g_seen, s.call, s.freq_hz, s.at)) {
        if (g_queued < PSKR_QUEUE_MAX)
            g_queue[g_queued++] = s;
        else
            g_dropped++;
        if (g_queued >= PSKR_SEND_EARLY)
            pthread_cond_signal(&g_wake);
    }
    pthread_mutex_unlock(&g_lock);
}

/* This station as the server sees it, or false (said once) if it has no
 * callsign or no position yet. */
static bool receiver_now(char *call, size_t call_size, char *locator, size_t loc_size)
{
    static bool said_call, said_position;
    bool fixed;
    double lat, lon;
    pthread_mutex_lock(&g_radio->cfg_mutex);
    snprintf(call, call_size, "%s", g_radio->callsign);
    fixed = g_radio->has_station_position;
    lat = g_radio->station_latitude;
    lon = g_radio->station_longitude;
    pthread_mutex_unlock(&g_radio->cfg_mutex);

    radiod_api_gps g;
    gps_latest(&g);
    if (g.fix.has_position) {
        lat = g.fix.latitude;
        lon = g.fix.longitude;
        fixed = true;
    }
    if (!call[0]) {
        if (!said_call)
            HLOGW(LOG_C, "no main:callsign; nothing reported");
        said_call = true;
        return false;
    }
    said_call = false;
    if (!fixed || !pskr_locator(lat, lon, locator, loc_size)) {
        if (!said_position)
            HLOGW(LOG_C, "no GPS fix and no main:station_latitude/longitude; nothing reported");
        said_position = true;
        return false;
    }
    said_position = false;
    return true;
}

/* The server's address, looked up at each send (it may move), and one
 * socket kept for it: "the same UDP source port for every datagram". */
static bool server_now(struct sockaddr_storage *to, socklen_t *to_len)
{
    char host[PSKR_HOST_MAX];
    snprintf(host, sizeof host, "%s", g_radio->pskreporter_server);
    const char *port = PSKREPORTER_DEFAULT_PORT;
    char *colon = strrchr(host, ':');
    if (colon && !strchr(host, ']') && strchr(host, ':') == colon) {
        *colon = '\0';
        port = colon + 1;
    }
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM }, *ai = NULL;
    int rc = getaddrinfo(host, port, &hints, &ai);
    if (rc != 0 || !ai) {
        HLOGW(LOG_C, "cannot look up %s: %s; trying again next time", host, gai_strerror(rc));
        return false;
    }
    if (g_sock >= 0 && g_sock_family != ai->ai_family) {
        close(g_sock);
        g_sock = -1;
    }
    if (g_sock < 0) {
        g_sock = socket(ai->ai_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        g_sock_family = ai->ai_family;
    }
    bool ok = g_sock >= 0 && ai->ai_addrlen <= sizeof *to;
    if (ok) {
        memcpy(to, ai->ai_addr, ai->ai_addrlen);
        *to_len = ai->ai_addrlen;
    } else {
        HLOGW(LOG_C, "no UDP socket: %s", strerror(errno));
    }
    freeaddrinfo(ai);
    return ok;
}

/* Everything queued, in as many datagrams as it takes.  With no callsign,
 * position or server yet the queue waits (up to PSKR_QUEUE_MAX), so what
 * was heard before the first GPS fix is still reported. */
static bool send_queued(void)
{
    static pskr_spot spots[PSKR_QUEUE_MAX];
    char call[PSKR_CALL_MAX], locator[PSKR_LOCATOR_MAX], software[PSKR_SOFTWARE_MAX];
    struct sockaddr_storage to;
    socklen_t to_len = 0;

    pthread_mutex_lock(&g_lock);
    int n = g_queued;
    pthread_mutex_unlock(&g_lock);
    if (n == 0)
        return true;
    if (!receiver_now(call, sizeof call, locator, sizeof locator) || !server_now(&to, &to_len))
        return false;

    pthread_mutex_lock(&g_lock);
    n = g_queued;
    unsigned dropped = g_dropped;
    memcpy(spots, g_queue, (size_t) n * sizeof spots[0]);
    g_queued = 0;
    g_dropped = 0;
    pthread_mutex_unlock(&g_lock);
    if (dropped)
        HLOGW(LOG_C, "%u spots dropped: the queue was full", dropped);
    snprintf(software, sizeof software, "%s %s", PSKR_SOFTWARE, radiod_api_version());
    pskr_receiver rx = { .call = call, .locator = locator, .software = software,
                         .antenna = g_radio->pskreporter_antenna };

    int sent = 0;
    while (sent < n) {
        time_t now = time(NULL);
        bool templates = g_datagrams < PSKR_TEMPLATES_FIRST ||
                         now - g_templates_at >= PSKR_TEMPLATES_EVERY_S;
        uint8_t dgram[PSKR_DATAGRAM_MAX];
        int taken = 0;
        size_t len = pskr_encode(dgram, sizeof dgram, &rx, spots + sent, n - sent, templates,
                                 (uint32_t) now, g_sequence, g_id, &taken);
        if (len == 0 || taken == 0) {
            HLOGW(LOG_C, "cannot fit a report in %d bytes", PSKR_DATAGRAM_MAX);
            return false;
        }
        if (sendto(g_sock, dgram, len, 0, (struct sockaddr *) &to, to_len) != (ssize_t) len) {
            HLOGW(LOG_C, "sending to %s: %s", g_radio->pskreporter_server, strerror(errno));
            return false;
        }
        g_sequence += (uint32_t) taken + 1;     /* the data records: spots and the receiver's */
        g_datagrams++;
        if (templates)
            g_templates_at = now;
        sent += taken;
    }
    HLOGI(LOG_C, "reported %d stations as %s (%s)", sent, call, locator);
    return true;
}

static time_t next_send(time_t from)
{
    return from + PSKR_INTERVAL_S + rand_r(&g_jitter_seed) % (PSKR_JITTER_S + 1);
}

static void *pskreporter_thread(void *arg)
{
    (void) arg;
    time_t due = next_send(time(NULL));
    bool sending = true;   /* the last send went: a full queue may go early */
    pthread_mutex_lock(&g_lock);
    while (!g_stop) {
        struct timespec until = { .tv_sec = due };
        while (!g_stop && (!sending || g_queued < PSKR_SEND_EARLY) && time(NULL) < due)
            if (pthread_cond_timedwait(&g_wake, &g_lock, &until) == ETIMEDOUT)
                break;
        pthread_mutex_unlock(&g_lock);
        sending = send_queued();
        due = next_send(time(NULL));
        pthread_mutex_lock(&g_lock);
    }
    pthread_mutex_unlock(&g_lock);
    send_queued();
    return NULL;
}

bool pskreporter_start(radio *radio_h)
{
    if (!radio_h || !radio_h->pskreporter_enable || atomic_load(&g_started))
        return false;
    g_radio = radio_h;
    /* "A random identifier, constant for a sender within a session". */
    if (getrandom(&g_id, sizeof g_id, 0) != (ssize_t) sizeof g_id)
        g_id = (uint32_t) time(NULL) ^ (uint32_t) getpid();
    g_jitter_seed = g_id;
    g_stop = false;
    if (pthread_create(&g_tid, NULL, pskreporter_thread, NULL) != 0) {
        HLOGE(LOG_C, "cannot start its thread");
        return false;
    }
    atomic_store(&g_started, true);
    digi_log_set_listener(heard);
    HLOGI(LOG_C, "reporting FT8 stations heard to %s", radio_h->pskreporter_server);
    return true;
}

void pskreporter_shutdown(void)
{
    if (!atomic_exchange(&g_started, false))
        return;
    digi_log_set_listener(NULL);
    pthread_mutex_lock(&g_lock);
    g_stop = true;
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_tid, NULL);
    if (g_sock >= 0)
        close(g_sock);
    g_sock = -1;
}
