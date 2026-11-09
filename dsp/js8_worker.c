/* js8_worker - JS8 receive for the radio paths.  See js8_worker.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE     /* sched_getcpu, CPU sets, thread affinity */
#include "js8_worker.h"
#include "js8_decode.h"
#include "radiod_cpus.h"
#include "radiod_log.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_C "js8"

#define JS8_RING_SECONDS   32      /* Slow's 30 s period, and room */
#define JS8_RING           (JS8_RING_SECONDS * JS8_SAMPLE_RATE)
#define JS8_STEP_NUM       2       /* a window every 2/15 of a period */
#define JS8_STEP_DEN       15
#define JS8_POLL_NS        250000000L   /* how often the thread looks */
#define JS8_GAP_S          1.0     /* audio this much later than the ring's end: start again */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_wake = PTHREAD_COND_INITIALIZER;
static pthread_t       g_tid;
static bool            g_started, g_tried, g_stop;
static atomic_uint     g_submodes = JS8_SUBMODES_DEFAULT;

/* Under g_lock: the newest JS8_RING samples, oldest first from g_head. */
static float    g_ring[JS8_RING];
static int      g_head, g_count;
static double   g_end_at;          /* unix time of the newest sample */
static uint64_t g_dial_hz;
static uint64_t g_generation;      /* bumped when the ring starts again */

static void *js8_thread(void *arg);

void js8_worker_set_submodes(unsigned mask)
{
    atomic_store(&g_submodes, mask & ((1u << JS8_SUBMODES) - 1));
}

static bool start_thread(void)
{
    pthread_attr_t attr;
    cpu_set_t cpus;
    int dsp_cpu = sched_getcpu();
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    /* off the DSP core, on what radiod may use, as the FT8 decoder */
    if (radiod_unpinned_cpus(&cpus)) {
        if (dsp_cpu >= 0)
            CPU_CLR(dsp_cpu, &cpus);
        if (CPU_COUNT(&cpus) > 0)
            pthread_attr_setaffinity_np(&attr, sizeof cpus, &cpus);
    }
    bool ok = pthread_create(&g_tid, &attr, js8_thread, NULL) == 0;
    pthread_attr_destroy(&attr);
    if (ok)
        HLOGI(LOG_C, "JS8 decodes on their own thread, off CPU %d", dsp_cpu);
    else
        HLOGE(LOG_C, "cannot start the JS8 decoder thread");
    return ok;
}

void js8_worker_feed(const float *audio_12k, int n, double newest_at, uint64_t dial_hz)
{
    if (!audio_12k || n <= 0)
        return;
    pthread_mutex_lock(&g_lock);
    if (!g_tried) {
        g_tried = true;
        g_started = start_thread();
    }
    double first_at = newest_at - (double) n / JS8_SAMPLE_RATE;
    if (dial_hz != g_dial_hz || first_at - g_end_at > JS8_GAP_S) {
        g_head = g_count = 0;   /* another frequency, or audio after a gap */
        g_dial_hz = dial_hz;
        g_generation++;
    }
    if (n > JS8_RING) {
        audio_12k += n - JS8_RING;
        n = JS8_RING;
    }
    for (int i = 0; i < n; i++) {
        g_ring[(g_head + g_count) % JS8_RING] = audio_12k[i];
        if (g_count < JS8_RING)
            g_count++;
        else
            g_head = (g_head + 1) % JS8_RING;
    }
    g_end_at = newest_at;
    pthread_mutex_unlock(&g_lock);
}

static void *js8_thread(void *arg)
{
    (void) arg;
    static float window[JS8_RING];
    double due[JS8_SUBMODES] = { 0 };
    uint64_t gen = 0;

    pthread_mutex_lock(&g_lock);
    while (!g_stop) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += JS8_POLL_NS;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&g_wake, &g_lock, &until);
        if (g_stop)
            break;
        if (gen != g_generation) {   /* the ring started again: so do the windows */
            gen = g_generation;
            memset(due, 0, sizeof due);
        }
        unsigned mask = atomic_load(&g_submodes);
        for (int sub = 0; sub < JS8_SUBMODES && !g_stop; sub++) {
            const js8_submode_params *p = js8_submode_get((js8_submode) sub);
            int need = p->period_s * JS8_SAMPLE_RATE;
            if (!(mask & (1u << sub)) || g_count < need || g_end_at < due[sub])
                continue;
            /* The newest period, copied out so feeding goes on meanwhile. */
            int from = g_head + g_count - need;
            for (int i = 0; i < need; i++)
                window[i] = g_ring[(from + i) % JS8_RING];
            double start = g_end_at - (double) need / JS8_SAMPLE_RATE;
            uint64_t dial = g_dial_hz;
            due[sub] = g_end_at + (double) p->period_s * JS8_STEP_NUM / JS8_STEP_DEN;
            pthread_mutex_unlock(&g_lock);
            js8_decode_and_log((js8_submode) sub, window, need, start, dial);
            pthread_mutex_lock(&g_lock);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

void js8_worker_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    bool started = g_started;
    g_stop = true;
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
    if (started)
        pthread_join(g_tid, NULL);
}
