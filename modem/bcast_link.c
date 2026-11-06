/* bcast_link - see bcast_link.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "bcast_link.h"

#include "bcast_file.h"
#include "radiod_log.h"

#define LOG_C "bcast"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KISS_FEND   0xC0
#define KISS_FESC   0xDB
#define KISS_TFEND  0xDC
#define KISS_TFESC  0xDD
#define KISS_CMD_MODEM_FRAME 0x03

#define BCAST_SYMBOL_BYTES 41     /* Mercury's fixed RaptorQ symbol (bcast_modes.h) */
#define STATION_FRAME_MAX 14     /* DATAC16: what Mercury sends a KISS beacon on */
#define RECONNECT_MAX_S   10     /* the longest pause between connection attempts */
#define RECONNECT_STEP_MS 100    /* how often a pause checks for shutdown */
#define RX_POLL_MS        200    /* the reader's wait, so it notices shutdown */
#define TX_IDLE_WAIT_MS   200    /* the sender's wait for an object or a connection */
#define RX_BUF_BYTES      4096
#define INCOMING_DIR_MODE 0770
#define HOST_POLL_MS      200    /* how often the in-process backend reads Mercury's carousel */
#define HOST_TICK_MS      50     /* how often its wait checks for shutdown */
#define HOST_FILE_TEMPLATE "/.received-XXXXXX"   /* a received file, in incoming_dir */
#define PACKET_TYPE_SHIFT   5    /* a frame's packet type: its first byte's top 3 bits */
#define PACKET_TYPE_STATION 0x07

#define MS_PER_S  1000
#define NS_PER_MS 1000000L
#define NS_PER_S  1000000000L

/* ── KISS ─────────────────────────────────────────────────────── */

size_t bcast_kiss_encode(uint8_t cmd, const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    size_t n = 0;
#define PUT(b) do { if (n >= cap) return 0; out[n++] = (uint8_t) (b); } while (0)
    PUT(KISS_FEND);
    PUT(cmd);
    for (size_t i = 0; i < len; i++)
    {
        if (in[i] == KISS_FEND)      { PUT(KISS_FESC); PUT(KISS_TFEND); }
        else if (in[i] == KISS_FESC) { PUT(KISS_FESC); PUT(KISS_TFESC); }
        else                         PUT(in[i]);
    }
    PUT(KISS_FEND);
#undef PUT
    return n;
}

void bcast_kiss_feed(bcast_kiss_decoder *d, const uint8_t *in, size_t len,
                     void (*cb)(const uint8_t *frame, size_t len, void *user), void *user)
{
    for (size_t i = 0; i < len; i++)
    {
        uint8_t b = in[i];
        if (b == KISS_FEND)
        {
            if (d->in_frame && d->len && !d->overflow)
                cb(d->buf, d->len, user);
            d->len = 0;
            d->in_frame = true;
            d->escaped = d->overflow = false;
            continue;
        }
        if (!d->in_frame)
            continue;
        if (d->escaped)
        {
            b = b == KISS_TFEND ? KISS_FEND : b == KISS_TFESC ? KISS_FESC : b;
            d->escaped = false;
        }
        else if (b == KISS_FESC)
        {
            d->escaped = true;
            continue;
        }
        if (d->len < sizeof d->buf)
            d->buf[d->len++] = b;
        else
            d->overflow = true;
    }
}

/* ── the link ─────────────────────────────────────────────────── */

static bcast_link_config g_cfg;
static bcast_link_ops    g_ops;

static pthread_t   g_io_tid, g_tx_tid;
static atomic_bool g_running;
static atomic_bool g_started;   /* set once g_cfg and the threads are up */

/* The socket: g_fd_lock guards g_fd; g_write_lock serialises whole frames
 * on it (the carousel's and the beacons'), so two never interleave. */
static pthread_mutex_t g_fd_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_write_lock = PTHREAD_MUTEX_INITIALIZER;
static int  g_fd = -1;
static atomic_bool g_connected;

/* The object on the air. */
static pthread_mutex_t g_tx_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_tx_cond = PTHREAD_COND_INITIALIZER;
static bcast_file_tx_t *g_tx;
static bcast_tx_stats   g_stats;
static bool             g_stop_req;

static bcast_file_rx_t *g_rx;

/* In process: the carousel is Mercury's; g_host_end is how this one ends
 * once Mercury says it stopped (0: as Mercury says). */
static pthread_t g_host_tid;
static bool      g_host_on_air;
static bcast_tx_state g_host_end;

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / MS_PER_S, (long) (ms % MS_PER_S) * NS_PER_MS };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

static int connect_mercury(void)
{
    char port[16];
    snprintf(port, sizeof port, "%d", g_cfg.port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(g_cfg.host, port, &hints, &res) != 0 || !res)
        return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next)
    {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0)
    {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}

/* One whole KISS frame, or -1 when the socket is down. */
static int send_frame(const uint8_t *frame, size_t len)
{
    uint8_t wire[2 * BCAST_FILE_MAX_FRAME + 8];
    size_t n = bcast_kiss_encode(KISS_CMD_MODEM_FRAME, frame, len, wire, sizeof wire);
    if (!n)
        return -1;

    pthread_mutex_lock(&g_write_lock);
    pthread_mutex_lock(&g_fd_lock);
    int fd = g_fd;
    pthread_mutex_unlock(&g_fd_lock);
    int rc = fd < 0 ? -1 : 0;
    for (size_t off = 0; rc == 0 && off < n; )
    {
        ssize_t w = send(fd, wire + off, n - off, MSG_NOSIGNAL);
        if (w > 0)
            off += (size_t) w;
        else if (w < 0 && errno == EINTR)
            continue;
        else
            rc = -1;
    }
    pthread_mutex_unlock(&g_write_lock);
    return rc;
}

static void on_kiss_frame(const uint8_t *f, size_t len, void *user)
{
    (void) user;
    if (len < 2)
        return;
    const uint8_t *frame = f + 1;   /* past the KISS command */
    size_t n = len - 1;

    if ((frame[0] >> PACKET_TYPE_SHIFT) == PACKET_TYPE_STATION)
    {
        if (g_ops.on_station_frame)
            g_ops.on_station_frame(frame, n, g_ops.user);
        return;
    }
    if (!g_rx)
        return;
    bcast_rx_result_t r = bcast_file_rx_frame(g_rx, frame, n);
    if (r == BCAST_RX_COMPLETE)
    {
        HLOGI(LOG_C, "received \"%s\"", bcast_file_rx_last_name(g_rx));
        if (g_ops.on_object)
            g_ops.on_object(bcast_file_rx_last_path(g_rx), bcast_file_rx_last_name(g_rx), g_ops.user);
    }
    else if (r == BCAST_RX_ERROR)
        HLOGW(LOG_C, "receive: %s", bcast_file_rx_error(g_rx));
}

/* Connects, reads, and reconnects with a growing pause while Mercury is away. */
static void *io_thread(void *arg)
{
    (void) arg;
    int backoff = 1;
    bool said_down = false;
    while (atomic_load(&g_running))
    {
        int fd = connect_mercury();
        if (fd < 0)
        {
            if (!said_down)
                HLOGW(LOG_C, "Mercury's broadcast port %s:%d is not answering; retrying",
                      g_cfg.host, g_cfg.port);
            said_down = true;
            for (int i = 0; i < backoff * MS_PER_S / RECONNECT_STEP_MS && atomic_load(&g_running); i++)
                sleep_ms(RECONNECT_STEP_MS);
            if (backoff < RECONNECT_MAX_S)
                backoff *= 2;
            continue;
        }
        HLOGI(LOG_C, "connected to Mercury's broadcast port %s:%d", g_cfg.host, g_cfg.port);
        said_down = false;
        backoff = 1;
        pthread_mutex_lock(&g_fd_lock);
        g_fd = fd;
        pthread_mutex_unlock(&g_fd_lock);
        atomic_store(&g_connected, true);
        pthread_cond_broadcast(&g_tx_cond);

        bcast_kiss_decoder dec = {0};
        uint8_t buf[RX_BUF_BYTES];
        while (atomic_load(&g_running))
        {
            struct pollfd p = { .fd = fd, .events = POLLIN };
            int pr = poll(&p, 1, RX_POLL_MS);
            if (pr == 0 || (pr < 0 && errno == EINTR))
                continue;
            if (pr < 0)
                break;
            ssize_t r = recv(fd, buf, sizeof buf, 0);
            if (r <= 0)
            {
                if (r < 0 && errno == EINTR)
                    continue;
                break;
            }
            bcast_kiss_feed(&dec, buf, (size_t) r, on_kiss_frame, NULL);
        }

        atomic_store(&g_connected, false);
        pthread_mutex_lock(&g_write_lock);   /* no frame half-written on a closed fd */
        pthread_mutex_lock(&g_fd_lock);
        g_fd = -1;
        pthread_mutex_unlock(&g_fd_lock);
        pthread_mutex_unlock(&g_write_lock);
        close(fd);
        if (atomic_load(&g_running))
            HLOGW(LOG_C, "Mercury's broadcast port closed; reconnecting");
    }
    return NULL;
}

static void tx_finish_locked(bcast_tx_state st)
{
    bcast_file_tx_close(g_tx);
    g_tx = NULL;
    g_stats.state = st;
    g_stop_req = false;
}

/* Puts the object on the air one frame at a time, as fast as Mercury takes them. */
static void *tx_thread(void *arg)
{
    (void) arg;
    uint8_t frame[BCAST_FILE_MAX_FRAME];
    pthread_mutex_lock(&g_tx_lock);
    while (atomic_load(&g_running))
    {
        if (!g_tx || !atomic_load(&g_connected))
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += TX_IDLE_WAIT_MS * NS_PER_MS;
            if (ts.tv_nsec >= NS_PER_S) { ts.tv_sec++; ts.tv_nsec -= NS_PER_S; }
            pthread_cond_timedwait(&g_tx_cond, &g_tx_lock, &ts);
            continue;
        }
        if (g_stop_req)
        {
            HLOGI(LOG_C, "stopped \"%s\" after %llu frames", g_stats.name,
                  (unsigned long long) g_stats.frames_sent);
            tx_finish_locked(BCAST_TX_STOPPED);
            continue;
        }
        if (g_stats.frames_limit > 0 && (int64_t) g_stats.frames_sent >= g_stats.frames_limit)
        {
            HLOGI(LOG_C, "\"%s\": its %lld frames are sent", g_stats.name,
                  (long long) g_stats.frames_limit);
            tx_finish_locked(BCAST_TX_DONE);
            continue;
        }
        int n = bcast_file_tx_next(g_tx, frame, sizeof frame);
        if (n <= 0)
        {
            if (n < 0)
                HLOGE(LOG_C, "\"%s\": the carousel failed", g_stats.name);
            else
                HLOGI(LOG_C, "\"%s\": done, %llu frames", g_stats.name,
                      (unsigned long long) g_stats.frames_sent);
            tx_finish_locked(n < 0 ? BCAST_TX_ERROR : BCAST_TX_DONE);
            continue;
        }
        pthread_mutex_unlock(&g_tx_lock);
        int rc = send_frame(frame, (size_t) n);   /* blocks while Mercury's queue is full */
        pthread_mutex_lock(&g_tx_lock);
        if (rc == 0 && g_tx)
        {
            g_stats.frames_sent++;
            if (g_stats.frames_per_pass > 0)
                g_stats.pass = (int) ((int64_t) (g_stats.frames_sent - 1) / g_stats.frames_per_pass) + 1;
        }
    }
    if (g_tx)
        tx_finish_locked(BCAST_TX_STOPPED);
    pthread_mutex_unlock(&g_tx_lock);
    return NULL;
}

/* ── in process: Mercury inside radiod ───────────────────────── */

static void host_on_object(const uint8_t *data, size_t len, const char *name)
{
    char path[sizeof g_cfg.incoming_dir + sizeof HOST_FILE_TEMPLATE];
    snprintf(path, sizeof path, "%s" HOST_FILE_TEMPLATE, g_cfg.incoming_dir);
    int fd = mkstemp(path);
    if (fd < 0)
    {
        HLOGW(LOG_C, "cannot file \"%s\" in %s: %s", name, g_cfg.incoming_dir, strerror(errno));
        return;
    }
    size_t off = 0;
    while (off < len)
    {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break;
        off += (size_t) w;
    }
    close(fd);
    if (off < len)
    {
        HLOGW(LOG_C, "cannot write \"%s\" to %s", name, path);
        unlink(path);
        return;
    }
    HLOGI(LOG_C, "received \"%s\"", name);
    if (g_ops.on_object)
        g_ops.on_object(path, name, g_ops.user);
    else
        unlink(path);
}

static void host_on_frame(const uint8_t *frame, size_t len)
{
    if (len && (frame[0] >> PACKET_TYPE_SHIFT) == PACKET_TYPE_STATION && g_ops.on_station_frame)
        g_ops.on_station_frame(frame, len, g_ops.user);
}

/* Starts the receiver once the engine is up, follows the carousel, and ends
 * it at its frame count (Mercury's runs until stopped). */
static void *host_thread(void *arg)
{
    (void) arg;
    const bcast_link_host *h = g_cfg.in_process;
    bool receiving = false, said_rx_error = false;
    while (atomic_load(&g_running))
    {
        if (!receiving)
        {
            int rc = h->receive(host_on_object, host_on_frame);
            if (rc == 0)
            {
                receiving = true;
                HLOGI(LOG_C, "on Mercury inside radiod");
            }
            else if (rc == EINVAL && !said_rx_error)
            {
                HLOGW(LOG_C, "Mercury's broadcast receiver did not start; retrying");
                said_rx_error = true;
            }
        }
        bcast_host_status hs;
        bool up = h->status(&hs);
        atomic_store(&g_connected, up && receiving);

        bool stop = false;
        pthread_mutex_lock(&g_tx_lock);
        if (g_host_on_air && up)
        {
            g_stats.frames_sent = hs.frames_sent;
            if (g_stats.frames_per_pass > 0 && hs.frames_sent > 0)
                g_stats.pass = (int) ((int64_t) (hs.frames_sent - 1) / g_stats.frames_per_pass) + 1;
            if (hs.state == BCAST_TX_SENDING)
            {
                if (!g_host_end && g_stop_req)
                    g_host_end = BCAST_TX_STOPPED;
                else if (!g_host_end && g_stats.frames_limit > 0 &&
                         (int64_t) hs.frames_sent >= g_stats.frames_limit)
                    g_host_end = BCAST_TX_DONE;
                stop = g_host_end != 0;
            }
            else
            {
                bcast_tx_state st = g_host_end ? g_host_end
                                  : hs.state == BCAST_TX_DONE || hs.state == BCAST_TX_ERROR ? (bcast_tx_state) hs.state
                                  : BCAST_TX_STOPPED;   /* stopped by Mercury: an ARQ session */
                if (st == BCAST_TX_DONE)
                    HLOGI(LOG_C, "\"%s\": done, %llu frames", g_stats.name, (unsigned long long) hs.frames_sent);
                else if (st == BCAST_TX_ERROR)
                    HLOGE(LOG_C, "\"%s\": the carousel failed", g_stats.name);
                else
                    HLOGI(LOG_C, "stopped \"%s\" after %llu frames", g_stats.name,
                          (unsigned long long) hs.frames_sent);
                g_stats.state = st;
                g_host_on_air = false;
                g_host_end = 0;
                g_stop_req = false;
            }
        }
        pthread_mutex_unlock(&g_tx_lock);
        if (stop)
            h->stop();   /* returns once Mercury's sender is gone: not under the lock */

        for (int i = 0; i < HOST_POLL_MS / HOST_TICK_MS && atomic_load(&g_running); i++)
            sleep_ms(HOST_TICK_MS);
    }
    pthread_mutex_lock(&g_tx_lock);
    bool on_air = g_host_on_air;
    if (on_air)
        g_stats.state = BCAST_TX_STOPPED;
    g_host_on_air = false;
    pthread_mutex_unlock(&g_tx_lock);
    if (on_air)
        h->stop();
    if (receiving)
        h->receive(NULL, NULL);
    atomic_store(&g_connected, false);
    return NULL;
}

static bool host_start(void)
{
    pthread_mutex_lock(&g_tx_lock);
    memset(&g_stats, 0, sizeof g_stats);
    g_stats.frames_limit = -1;
    g_host_on_air = false;
    g_host_end = 0;
    pthread_mutex_unlock(&g_tx_lock);
    atomic_store(&g_running, true);
    if (pthread_create(&g_host_tid, NULL, host_thread, NULL) != 0)
    {
        atomic_store(&g_running, false);
        return false;
    }
    atomic_store(&g_started, true);
    HLOGI(LOG_C, "broadcast plane on Mercury inside radiod, at its listen mode");
    return true;
}

bool bcast_link_start(const bcast_link_config *cfg, const bcast_link_ops *ops)
{
    if (atomic_load(&g_started))
        return true;
    g_cfg = *cfg;
    g_ops = ops ? *ops : (bcast_link_ops) {0};
    if (g_cfg.in_process)
    {
        mkdir(g_cfg.incoming_dir, INCOMING_DIR_MODE);
        return host_start();
    }

    if (!bcast_file_mode_usable(g_cfg.mode) || bcast_file_mode_symbols(g_cfg.mode) == 0)
    {
        HLOGE(LOG_C, "broadcast mode %d cannot carry files", g_cfg.mode);
        return false;
    }
    mkdir(g_cfg.incoming_dir, INCOMING_DIR_MODE);
    char err[256] = "";
    g_rx = bcast_file_rx_open(g_cfg.mode, g_cfg.incoming_dir, err, sizeof err);
    if (!g_rx)
    {
        HLOGE(LOG_C, "cannot receive into %s: %s", g_cfg.incoming_dir, err);
        return false;
    }

    pthread_mutex_lock(&g_tx_lock);   /* the spool may already be asking */
    memset(&g_stats, 0, sizeof g_stats);
    g_stats.frames_limit = -1;
    pthread_mutex_unlock(&g_tx_lock);
    atomic_store(&g_running, true);
    if (pthread_create(&g_io_tid, NULL, io_thread, NULL) != 0)
    {
        atomic_store(&g_running, false);
        bcast_file_rx_close(g_rx);
        g_rx = NULL;
        return false;
    }
    if (pthread_create(&g_tx_tid, NULL, tx_thread, NULL) != 0)
    {
        atomic_store(&g_running, false);
        pthread_join(g_io_tid, NULL);
        bcast_file_rx_close(g_rx);
        g_rx = NULL;
        return false;
    }
    atomic_store(&g_started, true);
    HLOGI(LOG_C, "broadcast plane on %s:%d, mode %s", g_cfg.host, g_cfg.port,
          bcast_file_mode_name(g_cfg.mode));
    return true;
}

void bcast_link_stop(void)
{
    if (!atomic_exchange(&g_started, false))
        return;
    atomic_store(&g_running, false);
    if (g_cfg.in_process)
    {
        pthread_join(g_host_tid, NULL);
        return;
    }
    pthread_mutex_lock(&g_fd_lock);
    if (g_fd >= 0)
        shutdown(g_fd, SHUT_RDWR);   /* unblocks a send waiting on Mercury */
    pthread_mutex_unlock(&g_fd_lock);
    pthread_cond_broadcast(&g_tx_cond);
    pthread_join(g_tx_tid, NULL);
    pthread_join(g_io_tid, NULL);
    bcast_file_rx_close(g_rx);
    g_rx = NULL;
}

const char *bcast_link_mode_name(int mode)
{
    return bcast_file_mode_name(mode);
}

int bcast_link_mode(int fallback)
{
    if (!atomic_load(&g_started))
        return fallback;
    if (g_cfg.in_process)
    {
        int m = g_cfg.in_process->listen_mode();
        return m >= 0 ? m : g_cfg.mode;
    }
    return g_cfg.mode;
}

int64_t bcast_link_frames_per_pass(int mode, const char *name, size_t len)
{
    int per_frame = bcast_file_mode_symbols(mode);
    if (per_frame <= 0)
        return 0;
    size_t bundle = len + BCAST_BUNDLE_OVERHEAD(strlen(name ? name : ""));
    int64_t symbols = (int64_t) ((bundle + BCAST_SYMBOL_BYTES - 1) / BCAST_SYMBOL_BYTES);
    return (symbols + per_frame - 1) / per_frame;
}

int bcast_link_send_object(const char *name, const uint8_t *data, size_t len,
                           int passes, int64_t frames_limit, char *err, size_t errlen)
{
    if (!atomic_load(&g_started))
        return EAGAIN;             /* the link is not up yet (or any more) */
    int mode = bcast_link_mode(g_cfg.mode);
    pthread_mutex_lock(&g_tx_lock);
    if (g_tx || g_host_on_air)
    {
        pthread_mutex_unlock(&g_tx_lock);
        return EBUSY;
    }
    /* The codec's "cycle" is one frame per RaptorQ block; passes are counted
     * here, so it runs endless and the frame count ends it.  In process the
     * codec is Mercury's carousel. */
    if (g_cfg.in_process)
    {
        int rc = g_cfg.in_process->send(name, data, len, err, errlen);
        if (rc != 0)
        {
            pthread_mutex_unlock(&g_tx_lock);
            return rc;
        }
        g_host_on_air = true;
        g_host_end = 0;
    }
    else
    {
        bcast_file_tx_t *tx = bcast_file_tx_open_mem(name, data, len, g_cfg.mode, 0, 0, err, errlen);
        if (!tx)
        {
            pthread_mutex_unlock(&g_tx_lock);
            return EINVAL;
        }
        g_tx = tx;
    }
    memset(&g_stats, 0, sizeof g_stats);
    g_stats.state = BCAST_TX_SENDING;
    snprintf(g_stats.name, sizeof g_stats.name, "%s", name);
    g_stats.bytes = len;
    g_stats.passes = passes > 0 ? passes : 0;
    g_stats.frames_per_pass = bcast_link_frames_per_pass(mode, name, len);
    int64_t limit = frames_limit > 0 ? frames_limit : -1;
    if (g_stats.passes && g_stats.frames_per_pass > 0)
    {
        int64_t by_passes = g_stats.passes * g_stats.frames_per_pass;
        if (limit < 0 || by_passes < limit)
            limit = by_passes;
    }
    g_stats.frames_limit = limit;
    g_stop_req = false;
    pthread_cond_broadcast(&g_tx_cond);
    pthread_mutex_unlock(&g_tx_lock);
    if (limit > 0)
        HLOGI(LOG_C, "sending \"%s\" (%zu bytes, %lld frames)", name, len, (long long) limit);
    else
        HLOGI(LOG_C, "sending \"%s\" (%zu bytes, until stopped)", name, len);
    return 0;
}

void bcast_link_stop_object(void)
{
    pthread_mutex_lock(&g_tx_lock);
    if (g_tx || g_host_on_air)
        g_stop_req = true;
    pthread_cond_broadcast(&g_tx_cond);
    pthread_mutex_unlock(&g_tx_lock);
}

void bcast_link_stats(bcast_tx_stats *out)
{
    pthread_mutex_lock(&g_tx_lock);
    *out = g_stats;
    pthread_mutex_unlock(&g_tx_lock);
    out->connected = atomic_load(&g_connected);
}

int bcast_link_send_station_frame(const uint8_t *frame, size_t len)
{
    if (!frame || len == 0 || len > STATION_FRAME_MAX ||
        (frame[0] >> PACKET_TYPE_SHIFT) != PACKET_TYPE_STATION)
        return EINVAL;
    if (!atomic_load(&g_connected))
        return ENOTCONN;
    if (g_cfg.in_process)
        return g_cfg.in_process->station_frame(frame, len, BCAST_LINK_BEACON_MODE);
    return send_frame(frame, len) == 0 ? 0 : ENOTCONN;
}
