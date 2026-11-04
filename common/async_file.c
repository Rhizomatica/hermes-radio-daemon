/* async_file - file I/O for the real-time threads, done by a thread of its own
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "async_file.h"
#include "radiod_log.h"

#define LOG_C "async"

/* ---- one producer, one consumer: head and tail count bytes ever put and
 * taken, so head - tail is what is queued, and neither side ever waits ---- */

typedef struct {
    uint8_t *buf;
    size_t cap;
    _Atomic size_t head, tail;
} spsc;

static bool spsc_init(spsc *r, size_t cap)
{
    r->buf = malloc(cap);
    r->cap = cap;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return r->buf != NULL;
}

static size_t spsc_used(spsc *r)
{
    return atomic_load_explicit(&r->head, memory_order_acquire) -
           atomic_load_explicit(&r->tail, memory_order_acquire);
}

/* the producer's side: as much of s as fits */
static size_t spsc_put(spsc *r, const void *s, size_t n)
{
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t room = r->cap - (head - atomic_load_explicit(&r->tail, memory_order_acquire));
    size_t k = n < room ? n : room, at = head % r->cap, first = r->cap - at < k ? r->cap - at : k;

    memcpy(r->buf + at, s, first);
    memcpy(r->buf, (const uint8_t *) s + first, k - first);
    atomic_store_explicit(&r->head, head + k, memory_order_release);
    return k;
}

/* the consumer's side: up to n */
static size_t spsc_get(spsc *r, void *s, size_t n)
{
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t used = atomic_load_explicit(&r->head, memory_order_acquire) - tail;
    size_t k = n < used ? n : used, at = tail % r->cap, first = r->cap - at < k ? r->cap - at : k;

    memcpy(s, r->buf + at, first);
    memcpy((uint8_t *) s + first, r->buf, k - first);
    atomic_store_explicit(&r->tail, tail + k, memory_order_release);
    return k;
}

/* the consumer's side: everything queued, unread */
static void spsc_discard(spsc *r)
{
    atomic_store_explicit(&r->tail, atomic_load_explicit(&r->head, memory_order_acquire),
                          memory_order_release);
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- writer ---- */

struct async_writer {
    spsc ring;
    pthread_t thread;
    pthread_mutex_t lock;           /* path and want, between non-real-time callers and the thread */
    pthread_cond_t kick;            /* turned on, off, or to quit */
    char name[32], path[512], trigger[512];
    bool want, quit;
    unsigned gen;                   /* bumped by every on: an off and on between two looks reopens */
    _Atomic bool on;                /* the real-time side writes only while on */
    atomic_uint_least64_t dropped;
};

static void writer_drain(async_writer *w, FILE *fp)
{
    uint8_t chunk[16384];
    size_t n;

    while ((n = spsc_get(&w->ring, chunk, sizeof(chunk))) > 0)
        if (fp)
            fwrite(chunk, 1, n, fp);
}

static void *writer_main(void *arg)
{
    async_writer *w = arg;
    FILE *fp = NULL;
    unsigned tick = 0, open_gen = 0;
    bool failed = false;            /* until turned off and on again */
    char open_path[512] = "";

    for (;;) {
        char path[512];
        bool want, quit;
        unsigned gen;

        /* the trigger, every 250 ms */
        if (w->trigger[0] && (!fp || tick++ % 12 == 0)) {
            bool exists = access(w->trigger, F_OK) == 0;

            pthread_mutex_lock(&w->lock);
            if (exists && !w->want)
                w->gen++;
            w->want = exists;
            pthread_mutex_unlock(&w->lock);
        }
        pthread_mutex_lock(&w->lock);
        want = w->want;
        quit = w->quit;
        gen = w->gen;
        snprintf(path, sizeof(path), "%s", w->path);
        pthread_mutex_unlock(&w->lock);

        if (!want || gen != open_gen)
            failed = false;
        /* off, quitting, or turned off and on since it was opened: close */
        if (fp && (!want || quit || gen != open_gen)) {
            atomic_store(&w->on, false);
            writer_drain(w, fp);
            fclose(fp);
            fp = NULL;
            HLOGI(LOG_C, "%s: closed %s", w->name, open_path);
        }
        if (quit)
            break;
        if (want && !fp && !failed) {
            spsc_discard(&w->ring);  /* what came after the last off */
            open_gen = gen;
            if (!(fp = fopen(path, "wb"))) {
                HLOGE(LOG_C, "%s: cannot open %s (%s)", w->name, path, strerror(errno));
                failed = true;
            } else {
                HLOGI(LOG_C, "%s: writing %s", w->name, path);
                snprintf(open_path, sizeof(open_path), "%s", path);
                atomic_store(&w->on, true);
            }
        }
        if (fp) {
            writer_drain(w, fp);
            sleep_ms(20);
        } else {
            /* idle: until turned on, or the next look at the trigger */
            struct timespec until;

            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 250 * 1000000L;
            if (until.tv_nsec >= 1000000000L) {
                until.tv_sec++;
                until.tv_nsec -= 1000000000L;
            }
            pthread_mutex_lock(&w->lock);
            if (w->gen == gen && w->want == want && !w->quit)
                pthread_cond_timedwait(&w->kick, &w->lock, &until);
            pthread_mutex_unlock(&w->lock);
        }
    }
    return NULL;
}

async_writer *async_writer_new(const char *name, size_t ring_bytes,
                               const char *trigger, const char *path)
{
    async_writer *w = calloc(1, sizeof(*w));

    if (!w)
        return NULL;
    if (!spsc_init(&w->ring, ring_bytes)) {
        free(w);
        return NULL;
    }
    snprintf(w->name, sizeof(w->name), "%s", name);
    if (trigger && path) {
        snprintf(w->trigger, sizeof(w->trigger), "%s", trigger);
        snprintf(w->path, sizeof(w->path), "%s", path);
    }
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->kick, NULL);
    atomic_init(&w->on, false);
    atomic_init(&w->dropped, 0);
    if (pthread_create(&w->thread, NULL, writer_main, w) != 0) {
        pthread_cond_destroy(&w->kick);
        pthread_mutex_destroy(&w->lock);
        free(w->ring.buf);
        free(w);
        return NULL;
    }
    return w;
}

void async_writer_on(async_writer *w, const char *path)
{
    if (!w)
        return;
    pthread_mutex_lock(&w->lock);
    snprintf(w->path, sizeof(w->path), "%s", path);
    w->want = true;
    w->gen++;
    pthread_cond_signal(&w->kick);
    pthread_mutex_unlock(&w->lock);
}

void async_writer_off(async_writer *w)
{
    if (!w)
        return;
    pthread_mutex_lock(&w->lock);
    w->want = false;
    pthread_cond_signal(&w->kick);
    pthread_mutex_unlock(&w->lock);
}

void async_writer_write(async_writer *w, const void *data, size_t n)
{
    if (!w || !n || !atomic_load_explicit(&w->on, memory_order_acquire))
        return;
    size_t k = spsc_put(&w->ring, data, n);
    if (k < n)
        atomic_fetch_add_explicit(&w->dropped, n - k, memory_order_relaxed);
}

uint64_t async_writer_dropped(async_writer *w)
{
    return w ? atomic_load(&w->dropped) : 0;
}

void async_writer_free(async_writer *w)
{
    if (!w)
        return;
    pthread_mutex_lock(&w->lock);
    w->quit = true;
    pthread_cond_signal(&w->kick);
    pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread, NULL);
    pthread_cond_destroy(&w->kick);
    pthread_mutex_destroy(&w->lock);
    free(w->ring.buf);
    free(w);
}

/* ---- loop ---- */

struct async_loop {
    spsc ring;
    pthread_t thread;
    char path[512];
    _Atomic bool active;            /* the file is there: the real-time side reads */
    _Atomic bool flush;             /* the consumer is to discard what is queued */
    _Atomic bool quit;
};

static void *loop_main(void *arg)
{
    async_loop *l = arg;
    uint8_t chunk[65536];
    int fd = -1;
    bool missing_logged = false;
    unsigned tick = 0;

    while (!atomic_load(&l->quit)) {
        /* follow the path, every 100 ms: removed or replaced, the loop ends */
        if (fd >= 0 && tick++ % 10 == 0) {
            struct stat ps, fs;

            if (stat(l->path, &ps) != 0 || fstat(fd, &fs) != 0 ||
                    ps.st_ino != fs.st_ino || ps.st_dev != fs.st_dev) {
                HLOGI(LOG_C, "Loop ended: %s removed or replaced", l->path);
                close(fd);
                fd = -1;
                atomic_store(&l->active, false);
                atomic_store(&l->flush, true);
            }
        }
        if (fd < 0) {
            struct stat st;

            if (atomic_load(&l->flush)) {      /* the consumer has not discarded yet */
                sleep_ms(10);
                continue;
            }
            fd = open(l->path, O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                if (!missing_logged && errno != ENOENT)
                    HLOGE(LOG_C, "Cannot open %s (%s)", l->path, strerror(errno));
                missing_logged = true;
                sleep_ms(100);
                continue;
            }
            if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 0) {
                if (!missing_logged)
                    HLOGW(LOG_C, "%s is empty", l->path);
                missing_logged = true;
                close(fd);
                fd = -1;
                sleep_ms(100);
                continue;
            }
            HLOGI(LOG_C, "Loop active: %s", l->path);
            missing_logged = false;
            tick = 1;
        }
        /* keep the ring full */
        while (l->ring.cap - spsc_used(&l->ring) >= sizeof(chunk)) {
            ssize_t n = read(fd, chunk, sizeof(chunk));

            if (n == 0 && lseek(fd, 0, SEEK_SET) >= 0)
                continue;
            if (n <= 0) {
                HLOGE(LOG_C, "Reading %s failed (%s)", l->path, n < 0 ? strerror(errno) : "end");
                close(fd);
                fd = -1;
                atomic_store(&l->active, false);
                atomic_store(&l->flush, true);
                break;
            }
            spsc_put(&l->ring, chunk, (size_t) n);
        }
        if (fd >= 0)
            atomic_store(&l->active, true);
        sleep_ms(10);
    }
    if (fd >= 0)
        close(fd);
    return NULL;
}

async_loop *async_loop_new(const char *path, size_t ring_bytes)
{
    async_loop *l = calloc(1, sizeof(*l));

    if (!l)
        return NULL;
    if (ring_bytes < 131072)
        ring_bytes = 131072;            /* room for two reads */
    if (!spsc_init(&l->ring, ring_bytes)) {
        free(l);
        return NULL;
    }
    snprintf(l->path, sizeof(l->path), "%s", path);
    atomic_init(&l->active, false);
    atomic_init(&l->flush, false);
    atomic_init(&l->quit, false);
    if (pthread_create(&l->thread, NULL, loop_main, l) != 0) {
        free(l->ring.buf);
        free(l);
        return NULL;
    }
    return l;
}

bool async_loop_read(async_loop *l, void *out, size_t n)
{
    if (!l)
        return false;
    if (atomic_load_explicit(&l->flush, memory_order_acquire)) {
        spsc_discard(&l->ring);
        atomic_store_explicit(&l->flush, false, memory_order_release);
        return false;
    }
    if (!atomic_load_explicit(&l->active, memory_order_acquire) || spsc_used(&l->ring) < n)
        return false;
    spsc_get(&l->ring, out, n);
    return true;
}

void async_loop_idle(async_loop *l)
{
    if (l && atomic_load_explicit(&l->flush, memory_order_acquire)) {
        spsc_discard(&l->ring);
        atomic_store_explicit(&l->flush, false, memory_order_release);
    }
}

void async_loop_free(async_loop *l)
{
    if (!l)
        return;
    atomic_store(&l->quit, true);
    pthread_join(l->thread, NULL);
    free(l->ring.buf);
    free(l);
}
