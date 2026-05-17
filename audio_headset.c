/* hermes-radio-daemon - optional local-headset audio path implementation.
 *
 * Copyright (C) 2024-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE

#include <alsa/asoundlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_headset.h"
#include "audio_bridge.h"

extern _Atomic bool shutdown_;

static pthread_t hs_capture_tid;
static pthread_t hs_playback_tid;
static bool hs_capture_started = false;
static bool hs_playback_started = false;

static snd_pcm_t *headset_open(const char *device, snd_pcm_stream_t stream,
                               uint32_t rate, uint32_t period_frames)
{
    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, device, stream, 0);
    if (err < 0) {
        fprintf(stderr, "audio_headset: snd_pcm_open(%s, %s) failed: %s\n",
                device, stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
                snd_strerror(err));
        return NULL;
    }

    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             1, rate, 1, 100000);
    if (err < 0) {
        fprintf(stderr, "audio_headset: snd_pcm_set_params failed: %s\n",
                snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }
    (void) period_frames;
    return pcm;
}

static void *headset_capture_thread(void *radio_h_v)
{
    radio *radio_h = (radio *) radio_h_v;
    uint32_t ring_rate   = radio_h->audio_sample_rate ? radio_h->audio_sample_rate : 48000;
    uint32_t native_rate = radio_h->headset_sample_rate ? radio_h->headset_sample_rate : 48000;
    uint32_t frames = radio_h->audio_period_size ? radio_h->audio_period_size : 480;
    int16_t *buffer = calloc(frames, sizeof(int16_t));
    snd_pcm_t *pcm;
    audio_bridge bridge;

    if (!buffer)
        return NULL;

    if (!audio_bridge_init(&bridge, native_rate, ring_rate)) {
        free(buffer);
        return NULL;
    }

    pcm = headset_open(radio_h->headset_capture_device, SND_PCM_STREAM_CAPTURE,
                       native_rate, frames);
    if (!pcm) {
        audio_bridge_shutdown(&bridge);
        free(buffer);
        return NULL;
    }

    fprintf(stderr, "audio_headset: capture %s @ %u Hz -> tx_audio_ring @ %u Hz\n",
            radio_h->headset_capture_device, native_rate, ring_rate);

    while (!shutdown_) {
        snd_pcm_sframes_t got = snd_pcm_readi(pcm, buffer, frames);
        if (got == -EPIPE) {
            snd_pcm_prepare(pcm);
            continue;
        }
        if (got == -EAGAIN || got == 0) {
            usleep(2000);
            continue;
        }
        if (got < 0) {
            usleep(20000);
            continue;
        }

        audio_bridge_push_tx_native(&bridge, radio_h, buffer, (size_t) got);
    }

    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    audio_bridge_shutdown(&bridge);
    free(buffer);
    return NULL;
}

static void *headset_playback_thread(void *radio_h_v)
{
    radio *radio_h = (radio *) radio_h_v;
    uint32_t ring_rate   = radio_h->audio_sample_rate ? radio_h->audio_sample_rate : 48000;
    uint32_t native_rate = radio_h->headset_sample_rate ? radio_h->headset_sample_rate : 48000;
    uint32_t frames = radio_h->audio_period_size ? radio_h->audio_period_size : 480;
    int16_t *buffer = calloc(frames, sizeof(int16_t));
    snd_pcm_t *pcm;
    audio_bridge bridge;

    if (!buffer)
        return NULL;

    if (!audio_bridge_init(&bridge, native_rate, ring_rate)) {
        free(buffer);
        return NULL;
    }

    pcm = headset_open(radio_h->headset_playback_device, SND_PCM_STREAM_PLAYBACK,
                       native_rate, frames);
    if (!pcm) {
        audio_bridge_shutdown(&bridge);
        free(buffer);
        return NULL;
    }

    fprintf(stderr, "audio_headset: playback %s @ %u Hz <- rx_audio_ring @ %u Hz\n",
            radio_h->headset_playback_device, native_rate, ring_rate);

    while (!shutdown_) {
        size_t got = audio_bridge_pop_rx_native(&bridge, radio_h, buffer, frames);
        if (got == 0) {
            memset(buffer, 0, frames * sizeof(int16_t));
            got = frames;
        } else if (got < frames) {
            memset(buffer + got, 0, (frames - got) * sizeof(int16_t));
            got = frames;
        }

        snd_pcm_sframes_t wrote = snd_pcm_writei(pcm, buffer, got);
        if (wrote == -EPIPE) {
            snd_pcm_prepare(pcm);
            continue;
        }
        if (wrote == -EAGAIN || wrote == 0) {
            usleep(2000);
            continue;
        }
        if (wrote < 0)
            usleep(20000);
    }

    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    audio_bridge_shutdown(&bridge);
    free(buffer);
    return NULL;
}

bool audio_headset_init(radio *radio_h)
{
    if (!radio_h ||
        !radio_h->headset_capture_device[0] ||
        !radio_h->headset_playback_device[0])
        return false;

    if (pthread_create(&hs_capture_tid, NULL, headset_capture_thread, radio_h) != 0) {
        fprintf(stderr, "audio_headset: cannot start capture thread\n");
        return false;
    }
    hs_capture_started = true;

    if (pthread_create(&hs_playback_tid, NULL, headset_playback_thread, radio_h) != 0) {
        fprintf(stderr, "audio_headset: cannot start playback thread\n");
        return true; /* capture is still useful on its own */
    }
    hs_playback_started = true;
    return true;
}

void audio_headset_shutdown(radio *radio_h)
{
    (void) radio_h;
    if (hs_capture_started) {
        pthread_cond_broadcast(&radio_h->tx_audio_ring.cond);
        pthread_join(hs_capture_tid, NULL);
        hs_capture_started = false;
    }
    if (hs_playback_started) {
        pthread_cond_broadcast(&radio_h->rx_audio_ring.cond);
        pthread_join(hs_playback_tid, NULL);
        hs_playback_started = false;
    }
}
