/* sBitx audio bridge - ring buffers for websocket audio streaming
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "sbitx_core.h"

#define DEFAULT_QUEUE_SAMPLES 16000

bool sbitx_bridge_init(radio *radio_h)
{
    if (!radio_h)
        return false;

    radio_h->rx_audio_ring.samples = NULL;
    radio_h->rx_audio_ring.capacity = 0;
    radio_h->rx_audio_ring.read_pos = 0;
    radio_h->rx_audio_ring.write_pos = 0;
    radio_h->rx_audio_ring.count = 0;
    pthread_mutex_init(&radio_h->rx_audio_ring.mutex, NULL);
    pthread_cond_init(&radio_h->rx_audio_ring.cond, NULL);

    radio_h->tx_audio_ring.samples = NULL;
    radio_h->tx_audio_ring.capacity = 0;
    radio_h->tx_audio_ring.read_pos = 0;
    radio_h->tx_audio_ring.write_pos = 0;
    radio_h->tx_audio_ring.count = 0;
    pthread_mutex_init(&radio_h->tx_audio_ring.mutex, NULL);
    pthread_cond_init(&radio_h->tx_audio_ring.cond, NULL);

    uint32_t queue_samples = DEFAULT_QUEUE_SAMPLES;

    radio_h->rx_audio_ring.samples = calloc(queue_samples, sizeof(int16_t));
    if (!radio_h->rx_audio_ring.samples)
        return false;
    radio_h->rx_audio_ring.capacity = queue_samples;

    radio_h->tx_audio_ring.samples = calloc(queue_samples, sizeof(int16_t));
    if (!radio_h->tx_audio_ring.samples)
        return false;
    radio_h->tx_audio_ring.capacity = queue_samples;

    return true;
}

void sbitx_bridge_shutdown(radio *radio_h)
{
    if (!radio_h)
        return;

    free(radio_h->rx_audio_ring.samples);
    radio_h->rx_audio_ring.samples = NULL;
    radio_h->rx_audio_ring.capacity = 0;
    pthread_mutex_destroy(&radio_h->rx_audio_ring.mutex);
    pthread_cond_destroy(&radio_h->rx_audio_ring.cond);

    free(radio_h->tx_audio_ring.samples);
    radio_h->tx_audio_ring.samples = NULL;
    radio_h->tx_audio_ring.capacity = 0;
    pthread_mutex_destroy(&radio_h->tx_audio_ring.mutex);
    pthread_cond_destroy(&radio_h->tx_audio_ring.cond);
}

static void push_ring(audio_ring_buffer *ring, const int16_t *samples, size_t nsamples);

void sbitx_bridge_push_rx(radio *radio_h, const int16_t *samples, size_t nsamples)
{
    push_ring(&radio_h->rx_audio_ring, samples, nsamples);
    push_ring(&radio_h->rx_digi_ring, samples, nsamples);   /* hamlib digi pump */
}

static void push_ring(audio_ring_buffer *ring, const int16_t *samples, size_t nsamples)
{
    if (!ring->samples || !ring->capacity || !samples || !nsamples)
        return;

    pthread_mutex_lock(&ring->mutex);
    for (size_t i = 0; i < nsamples; i++)
    {
        if (ring->count == ring->capacity)
        {
            ring->read_pos = (ring->read_pos + 1) % ring->capacity;
            ring->count--;
        }
        ring->samples[ring->write_pos] = samples[i];
        ring->write_pos = (ring->write_pos + 1) % ring->capacity;
        ring->count++;
    }
    pthread_cond_signal(&ring->cond);
    pthread_mutex_unlock(&ring->mutex);
}

size_t sbitx_bridge_pop_tx(radio *radio_h, int16_t *samples, size_t max_samples)
{
    audio_ring_buffer *ring = &radio_h->tx_audio_ring;
    size_t out = 0;

    if (!ring->samples || !ring->capacity || !samples || !max_samples)
        return 0;

    pthread_mutex_lock(&ring->mutex);
    while (out < max_samples && ring->count > 0)
    {
        samples[out++] = ring->samples[ring->read_pos];
        ring->read_pos = (ring->read_pos + 1) % ring->capacity;
        ring->count--;
    }
    pthread_mutex_unlock(&ring->mutex);
    return out;
}
