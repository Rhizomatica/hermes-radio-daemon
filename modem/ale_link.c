/* ale_link - see ale_link.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include "ale_link.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "ale_engine.h"
#include "modem_audio.h"
#include "radiod_log.h"

#define LOG_C "ale"

_Static_assert(MODEM_AUDIO_RATE == ALE_ENGINE_RATE, "modem_audio and the ALE engine share a rate");
_Static_assert(ALE_COMBINE_MAX <= ALE_ENGINE_MAX_KS, "ale:combine holds no more K values than the engine takes");
_Static_assert(ALE_COMBINE_K_MIN >= 2 && ALE_COMBINE_K_MAX <= ALE_ENGINE_MAX_K, "ale:combine K values are ones the engine takes");

/* RX audio between the sink (the radio's audio thread) and the worker. */
#define ALE_RING_SECONDS     2
#define ALE_RING_SAMPLES     (ALE_ENGINE_RATE * ALE_RING_SECONDS)
/* The worker drains the ring this often. */
#define ALE_WORKER_PERIOD_MS 20
/* Dropped audio is reported at most this often. */
#define ALE_DROP_LOG_MS      10000
/* Samples the worker feeds the engine at a time. */
#define ALE_FEED_BLOCK       1024

static int16_t          s_ring[ALE_RING_SAMPLES];
static _Atomic uint64_t s_head;          /* written by the sink */
static _Atomic uint64_t s_tail;          /* read by the worker */
static _Atomic uint64_t s_dropped;

static ale_engine     *s_engine;
static pthread_t       s_tid;
static _Atomic bool    s_run;
static bool            s_started;

static void rx_sink(const int16_t *pcm, size_t n, void *user)
{
    (void) user;
    uint64_t head = atomic_load_explicit(&s_head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&s_tail, memory_order_acquire);
    uint64_t room = ALE_RING_SAMPLES - (head - tail);
    if (n > room)
    {
        atomic_fetch_add_explicit(&s_dropped, n - room, memory_order_relaxed);
        n = (size_t) room;
    }
    for (size_t i = 0; i < n; i++)
        s_ring[(head + i) % ALE_RING_SAMPLES] = pcm[i];
    atomic_store_explicit(&s_head, head + n, memory_order_release);
}

static void on_word(const ale_word_event *ev, void *user)
{
    (void) user;
    double at_s = (double) ev->end_sample / ALE_ENGINE_RATE;
    if (ev->combined)
        HLOGI(LOG_C, "%s %s: combining %u words, %u/48 unanimous votes, tone %.1f dB over the others (%.2f s into the audio)",
              ale_preamble_name(ev->preamble), ev->text, ev->combined, ev->unanimous,
              ev->tone_snr_db, at_s);
    else
        HLOGI(LOG_C, "%s %s: stock receiver, %u/48 unanimous votes, %u corrected (%.2f s into the audio)",
              ale_preamble_name(ev->preamble), ev->text, ev->unanimous, ev->fec_errors, at_s);
}

static void *worker(void *arg)
{
    (void) arg;
    int16_t block[ALE_FEED_BLOCK];
    uint64_t dropped_logged = 0;
    struct timespec last_drop_log = { 0, 0 };
    const struct timespec period = { 0, ALE_WORKER_PERIOD_MS * 1000000L };

    while (atomic_load(&s_run))
    {
        uint64_t head = atomic_load_explicit(&s_head, memory_order_acquire);
        uint64_t tail = atomic_load_explicit(&s_tail, memory_order_relaxed);
        while (tail < head)
        {
            size_t n = head - tail > ALE_FEED_BLOCK ? ALE_FEED_BLOCK : (size_t) (head - tail);
            for (size_t i = 0; i < n; i++)
                block[i] = s_ring[(tail + i) % ALE_RING_SAMPLES];
            tail += n;
            atomic_store_explicit(&s_tail, tail, memory_order_release);
            ale_engine_feed(s_engine, block, n);
        }

        uint64_t dropped = atomic_load_explicit(&s_dropped, memory_order_relaxed);
        if (dropped != dropped_logged)
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long since_ms = (now.tv_sec - last_drop_log.tv_sec) * 1000L
                          + (now.tv_nsec - last_drop_log.tv_nsec) / 1000000L;
            if (since_ms >= ALE_DROP_LOG_MS)
            {
                HLOGW(LOG_C, "the receiver fell behind: %llu samples of RX audio dropped so far",
                      (unsigned long long) dropped);
                dropped_logged = dropped;
                last_drop_log = now;
            }
        }
        nanosleep(&period, NULL);
    }
    return NULL;
}

bool ale_link_start(radio *radio_h, const cpu_set_t *cpus)
{
    if (s_started)
        return true;

    s_engine = ale_engine_create(radio_h->ale_combine, radio_h->ale_combine_count,
                                 on_word, NULL);
    if (!s_engine)
    {
        HLOGE(LOG_C, "cannot create the ALE receiver");
        return false;
    }
    if (!radio_h->ale_stock_receiver && !radio_h->ale_combine_count)
        HLOGW(LOG_C, "ale:stock_receiver is off and ale:combine is empty: nothing listens");
    ale_engine_set_stock(s_engine, radio_h->ale_stock_receiver);

    atomic_store(&s_head, 0);
    atomic_store(&s_tail, 0);
    atomic_store(&s_dropped, 0);
    if (!modem_audio_init(radio_h) || !modem_audio_add_rx_sink(rx_sink, NULL))
    {
        HLOGE(LOG_C, "cannot attach to the RX audio");
        ale_engine_destroy(s_engine);
        s_engine = NULL;
        return false;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (cpus)
        pthread_attr_setaffinity_np(&attr, sizeof(*cpus), cpus);
    atomic_store(&s_run, true);
    int rc = pthread_create(&s_tid, &attr, worker, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
    {
        HLOGE(LOG_C, "cannot start the ALE receiver's thread");
        atomic_store(&s_run, false);
        modem_audio_remove_rx_sink(rx_sink, NULL);
        ale_engine_destroy(s_engine);
        s_engine = NULL;
        return false;
    }
    pthread_setname_np(s_tid, "ale");
    s_started = true;

    char ks[ALE_COMBINE_MAX * 4 + 1] = "";
    size_t len = 0;
    for (unsigned i = 0; i < radio_h->ale_combine_count; i++)
        len += (size_t) snprintf(ks + len, sizeof(ks) - len, "%s%u", i ? "," : "",
                                 radio_h->ale_combine[i]);
    HLOGI(LOG_C, "listening: stock receiver %s, combining K = %s",
          radio_h->ale_stock_receiver ? "on" : "off", len ? ks : "none");
    return true;
}

void ale_link_stop(void)
{
    if (!s_started)
        return;
    modem_audio_remove_rx_sink(rx_sink, NULL);
    atomic_store(&s_run, false);
    pthread_join(s_tid, NULL);
    HLOGI(LOG_C, "stopped after %.0f s of RX audio (%llu samples dropped)",
          (double) ale_engine_position(s_engine) / ALE_ENGINE_RATE,
          (unsigned long long) atomic_load(&s_dropped));
    ale_engine_destroy(s_engine);
    s_engine = NULL;
    s_started = false;
}

uint64_t ale_link_samples_heard(void)
{
    return atomic_load(&s_tail);
}

uint64_t ale_link_samples_dropped(void)
{
    return atomic_load(&s_dropped);
}
