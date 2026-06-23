/* hermes-radio-daemon - generic media bridge
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define _GNU_SOURCE

#include <alsa/asoundlib.h>
#include <errno.h>
#include <fftw3.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "radio_media.h"
#include "radio_pipeline.h"
#include "audio_bridge.h"

extern _Atomic bool shutdown_;

#define DEFAULT_PERIOD_FRAMES 160
#define DEFAULT_QUEUE_SAMPLES 16000
/* 2048-pt FFT at 48 kHz = 23.4 Hz/bin; the first WATERFALL_BINS (128) bins
 * then span 0..3 kHz — i.e. the SSB/data audio passband fills the whole
 * waterfall instead of being squeezed into the left 1/8 of a 0..24 kHz view. */
#define SPECTRUM_FFT_SIZE 2048

typedef struct {
    radio *radio_h;
    bool capture;
} media_thread_ctx;

/* Spectrum FFT plan is built once per process (capture and playback threads
 * both run compute_spectrum, so the plan + I/O buffers are guarded by a
 * mutex). Rebuilding the plan per audio frame as the previous code did
 * caused mutex contention inside FFTW's planner and pointless allocations. */
static fftwf_plan        g_spectrum_plan;
static float             g_spectrum_in[SPECTRUM_FFT_SIZE];
static fftwf_complex     g_spectrum_out[(SPECTRUM_FFT_SIZE / 2) + 1];
static pthread_mutex_t   g_spectrum_plan_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool              g_spectrum_plan_ready = false;

static bool stream_matches(const char *stream_name, const char *candidate);

static void wav_write_header(FILE *fp, uint32_t sample_rate, uint32_t data_bytes)
{
    uint16_t audio_format = 1;
    uint16_t num_channels = 1;
    uint16_t bits_per_sample = 16;
    uint32_t byte_rate = sample_rate * num_channels * bits_per_sample / 8;
    uint16_t block_align = num_channels * bits_per_sample / 8;
    uint32_t riff_size = 36 + data_bytes;

    rewind(fp);
    fwrite("RIFF", 1, 4, fp);
    fwrite(&riff_size, sizeof(riff_size), 1, fp);
    fwrite("WAVEfmt ", 1, 8, fp);

    uint32_t fmt_chunk_size = 16;
    fwrite(&fmt_chunk_size, sizeof(fmt_chunk_size), 1, fp);
    fwrite(&audio_format, sizeof(audio_format), 1, fp);
    fwrite(&num_channels, sizeof(num_channels), 1, fp);
    fwrite(&sample_rate, sizeof(sample_rate), 1, fp);
    fwrite(&byte_rate, sizeof(byte_rate), 1, fp);
    fwrite(&block_align, sizeof(block_align), 1, fp);
    fwrite(&bits_per_sample, sizeof(bits_per_sample), 1, fp);
    fwrite("data", 1, 4, fp);
    fwrite(&data_bytes, sizeof(data_bytes), 1, fp);
}

static bool ensure_directory(const char *path)
{
    if (!path[0])
        return false;

    if (mkdir(path, 0775) == 0 || errno == EEXIST)
        return true;

    fprintf(stderr, "radio_media: mkdir(%s) failed: %s\n", path, strerror(errno));
    return false;
}

static bool ring_init(audio_ring_buffer *ring, size_t capacity)
{
    ring->samples = calloc(capacity, sizeof(int16_t));
    if (!ring->samples)
        return false;

    ring->capacity = capacity;
    ring->read_pos = 0;
    ring->write_pos = 0;
    ring->count = 0;
    pthread_mutex_init(&ring->mutex, NULL);
    pthread_cond_init(&ring->cond, NULL);
    return true;
}

static void ring_destroy(audio_ring_buffer *ring)
{
    if (ring->samples)
        free(ring->samples);
    ring->samples = NULL;
    ring->capacity = 0;
    ring->count = 0;
    pthread_mutex_destroy(&ring->mutex);
    pthread_cond_destroy(&ring->cond);
}

static void ring_push(audio_ring_buffer *ring, const int16_t *samples, size_t nsamples)
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

static size_t ring_pop(audio_ring_buffer *ring, int16_t *samples, size_t max_samples)
{
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

static void recording_init(wav_recording *rec)
{
    memset(rec, 0, sizeof(*rec));
    pthread_mutex_init(&rec->mutex, NULL);
}

static void recording_close(wav_recording *rec)
{
    pthread_mutex_lock(&rec->mutex);
    if (rec->fp)
    {
        wav_write_header(rec->fp, rec->sample_rate, rec->data_bytes);
        fclose(rec->fp);
        rec->fp = NULL;
    }
    rec->path[0] = '\0';
    rec->sample_rate = 0;
    rec->data_bytes = 0;
    rec->active = false;
    pthread_mutex_unlock(&rec->mutex);
}

static bool recording_open(wav_recording *rec, const char *dir_path, const char *prefix,
                           uint32_t sample_rate)
{
    time_t now = time(NULL);
    struct tm tm_now;
    char timestamp[64];

    if (!ensure_directory(dir_path))
        return false;

    localtime_r(&now, &tm_now);
    strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &tm_now);

    pthread_mutex_lock(&rec->mutex);
    if (rec->fp)
    {
        pthread_mutex_unlock(&rec->mutex);
        return true;
    }

    snprintf(rec->path, sizeof(rec->path), "%s/%s-%s.wav", dir_path, prefix, timestamp);
    rec->fp = fopen(rec->path, "wb");
    if (!rec->fp)
    {
        fprintf(stderr, "radio_media: cannot open %s: %s\n", rec->path, strerror(errno));
        rec->path[0] = '\0';
        pthread_mutex_unlock(&rec->mutex);
        return false;
    }

    rec->sample_rate = sample_rate;
    rec->data_bytes = 0;
    rec->active = true;
    wav_write_header(rec->fp, rec->sample_rate, 0);
    pthread_mutex_unlock(&rec->mutex);
    return true;
}

static void recording_write(wav_recording *rec, const int16_t *samples, size_t nsamples)
{
    pthread_mutex_lock(&rec->mutex);
    if (rec->fp && rec->active && nsamples > 0)
    {
        size_t wrote = fwrite(samples, sizeof(int16_t), nsamples, rec->fp);
        rec->data_bytes += (uint32_t) (wrote * sizeof(int16_t));
        fflush(rec->fp);
    }
    pthread_mutex_unlock(&rec->mutex);
}

static void recording_destroy(wav_recording *rec)
{
    recording_close(rec);
    pthread_mutex_destroy(&rec->mutex);
}

static snd_pcm_t *open_pcm_device(const char *device, snd_pcm_stream_t stream,
                                  uint32_t sample_rate)
{
    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, device, stream, 0);
    if (err < 0)
    {
        fprintf(stderr, "radio_media: snd_pcm_open(%s) failed: %s\n",
                device, snd_strerror(err));
        return NULL;
    }

    /* Negotiate format/channels/rate explicitly but let ALSA choose the
     * period/buffer. Forcing a concrete period/buffer here breaks shared
     * devices: when this is the first client to open a dsnoop/dmix it pins the
     * shared buffer to our size and later clients (e.g. mercury) fail to
     * attach with -ENODEV. The field HERMES asound.conf likewise sets no
     * period/buffer on its dsnoop/dmix slaves. (The old -EIO this code worked
     * around came from snd_pcm_set_params' latency arg, not from leaving the
     * period unset.) S16_LE / mono / RW_INTERLEAVED is the daemon ring format;
     * plug/dsnoop convert where the codec differs. */
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);

    unsigned int rate = sample_rate;

    if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
        (err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(pcm, hw, 1)) < 0 ||
        (err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0)) < 0 ||
        (err = snd_pcm_hw_params(pcm, hw)) < 0)
    {
        fprintf(stderr, "radio_media: hw_params(%s) failed: %s\n",
                device, snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }

    snd_pcm_prepare(pcm);
    return pcm;
}

static void update_spectrum_locked(radio *radio_h, bool tx, const float *bins)
{
    float *dst = tx ? radio_h->tx_spectrum : radio_h->rx_spectrum;

    pthread_mutex_lock(&radio_h->spectrum_mutex);
    memcpy(dst, bins, sizeof(float) * WATERFALL_BINS);
    if (tx)
    {
        radio_h->tx_spectrum_seq++;
        radio_h->tx_spectrum_valid = true;
    }
    else
    {
        radio_h->rx_spectrum_seq++;
        radio_h->rx_spectrum_valid = true;
    }
    pthread_mutex_unlock(&radio_h->spectrum_mutex);
}

static void compute_spectrum(radio *radio_h, bool tx, const int16_t *samples, size_t nsamples)
{
    static const float floor_db = -120.0f;
    float bins[WATERFALL_BINS];

    if (nsamples < SPECTRUM_FFT_SIZE)
        return;

    pthread_mutex_lock(&g_spectrum_plan_mutex);

    if (!g_spectrum_plan_ready)
    {
        pthread_mutex_unlock(&g_spectrum_plan_mutex);
        return;
    }

    for (size_t i = 0; i < SPECTRUM_FFT_SIZE; i++)
    {
        float window = 0.5f - 0.5f * cosf((2.0f * (float) M_PI * i) /
                                          (float) (SPECTRUM_FFT_SIZE - 1));
        g_spectrum_in[i] = ((float) samples[i] / 32768.0f) * window;
    }

    fftwf_execute(g_spectrum_plan);

    for (size_t i = 0; i < WATERFALL_BINS; i++)
    {
        float re = g_spectrum_out[i][0];
        float im = g_spectrum_out[i][1];
        float mag = (re * re) + (im * im);
        float db = 10.0f * log10f(mag + 1.0e-12f);
        bins[i] = db < floor_db ? floor_db : db;
    }

    pthread_mutex_unlock(&g_spectrum_plan_mutex);

    radio_h->spectrum_sample_rate = radio_h->audio_sample_rate;
    update_spectrum_locked(radio_h, tx, bins);
}

static void *capture_thread(void *ctx_v)
{
    media_thread_ctx *ctx = (media_thread_ctx *) ctx_v;
    radio *radio_h = ctx->radio_h;
    uint32_t ring_rate   = radio_h->audio_sample_rate ? radio_h->audio_sample_rate : 48000;
    uint32_t native_rate = radio_h->rig_audio_rate ? radio_h->rig_audio_rate : ring_rate;
    uint32_t frames = radio_h->audio_period_size ? radio_h->audio_period_size : DEFAULT_PERIOD_FRAMES;
    int16_t *buffer = calloc(frames, sizeof(int16_t));
    snd_pcm_t *pcm;
    audio_bridge bridge;

    if (!buffer)
        return NULL;

    if (!audio_bridge_init(&bridge, native_rate, ring_rate)) {
        fprintf(stderr, "radio_media: audio_bridge_init(capture) failed\n");
        free(buffer);
        return NULL;
    }

    pcm = open_pcm_device(radio_h->capture_device, SND_PCM_STREAM_CAPTURE, native_rate);
    if (!pcm)
    {
        audio_bridge_shutdown(&bridge);
        free(buffer);
        return NULL;
    }

    fprintf(stderr, "radio_media: capture %s @ %u Hz -> ring @ %u Hz\n",
            radio_h->capture_device, native_rate, ring_rate);

    while (!shutdown_)
    {
        snd_pcm_sframes_t got = snd_pcm_readi(pcm, buffer, frames);
        if (got == -EPIPE)
        {
            snd_pcm_prepare(pcm);
            continue;
        }
        if (got == -EAGAIN || got == 0)
        {
            /* Virtual ALSA devices (e.g. `null`) return 0 immediately
             * instead of blocking for the period; sleep one period to
             * avoid a 100% CPU spin. Real codecs never hit this path. */
            usleep(2000);
            continue;
        }
        if (got < 0)
        {
            /* snd_pcm_recover handles xrun (-EPIPE) and suspend (-ESTRPIPE).
             * For harder errors — e.g. -EIO from a USB codec that needs a
             * fresh stream — it returns the error, so close and reopen rather
             * than spin forever on a dead handle (the old code looped on EIO
             * at 50 Hz, producing no audio and flooding the log). */
            if (snd_pcm_recover(pcm, (int) got, 1) == 0)
                continue;

            fprintf(stderr, "radio_media: capture read failed: %s — reopening %s\n",
                    snd_strerror((int) got), radio_h->capture_device);
            snd_pcm_close(pcm);
            pcm = NULL;
            while (!shutdown_ && !pcm)
            {
                usleep(500000);
                pcm = open_pcm_device(radio_h->capture_device,
                                      SND_PCM_STREAM_CAPTURE, native_rate);
            }
            continue;
        }

        /* Pushes to rx_audio_ring at ring_rate (after resample), taps recording
         * and feeds the spectrum FFT. */
        audio_bridge_push_rx_native(&bridge, radio_h, buffer, (size_t) got);
    }

    if (pcm)
    {
        snd_pcm_drain(pcm);
        snd_pcm_close(pcm);
    }
    audio_bridge_shutdown(&bridge);
    free(buffer);
    return NULL;
}

static void *playback_thread(void *ctx_v)
{
    media_thread_ctx *ctx = (media_thread_ctx *) ctx_v;
    radio *radio_h = ctx->radio_h;
    uint32_t ring_rate   = radio_h->audio_sample_rate ? radio_h->audio_sample_rate : 48000;
    uint32_t native_rate = radio_h->rig_audio_rate ? radio_h->rig_audio_rate : ring_rate;
    uint32_t frames = radio_h->audio_period_size ? radio_h->audio_period_size : DEFAULT_PERIOD_FRAMES;
    int16_t *buffer = calloc(frames, sizeof(int16_t));
    snd_pcm_t *pcm;
    audio_bridge bridge;

    if (!buffer)
        return NULL;

    if (!audio_bridge_init(&bridge, native_rate, ring_rate)) {
        fprintf(stderr, "radio_media: audio_bridge_init(playback) failed\n");
        free(buffer);
        return NULL;
    }

    pcm = open_pcm_device(radio_h->playback_device, SND_PCM_STREAM_PLAYBACK, native_rate);
    if (!pcm)
    {
        audio_bridge_shutdown(&bridge);
        free(buffer);
        return NULL;
    }

    fprintf(stderr, "radio_media: playback %s @ %u Hz <- ring @ %u Hz\n",
            radio_h->playback_device, native_rate, ring_rate);

    while (!shutdown_)
    {
        /* When digital_voice is active on the hamlib backend, the
         * RADAE pump produces the rig-bound modulated audio into
         * tx_radae_ring; bypass tx_audio_ring (which carries raw
         * browser speech destined for the RADAE encoder). */
        uint32_t prof = radio_h->profile_active_idx;
        bool radae_active = (radio_h->backend_kind == RADIO_BACKEND_HAMLIB) &&
                            radio_h->profiles[prof].digital_voice;
        size_t got;
        if (radae_active) {
            audio_ring_buffer *r = &radio_h->tx_radae_ring;
            pthread_mutex_lock(&r->mutex);
            size_t take = 0;
            while (take < frames && r->count > 0) {
                buffer[take++] = r->samples[r->read_pos];
                r->read_pos = (r->read_pos + 1) % r->capacity;
                r->count--;
            }
            pthread_mutex_unlock(&r->mutex);
            got = take;
            if (got > 0)
                radio_media_tap_tx_audio(radio_h, buffer, got);
        } else {
            /* Pops from tx_audio_ring (ring_rate), resamples to
             * native_rate, taps TX recording, returns native-rate
             * samples in `buffer`. */
            got = audio_bridge_pop_tx_native(&bridge, radio_h, buffer, frames);
        }
        if (got == 0)
        {
            memset(buffer, 0, frames * sizeof(int16_t));
            got = frames;
        }
        else if (got < frames)
        {
            memset(buffer + got, 0, (frames - got) * sizeof(int16_t));
            got = frames;
        }

        snd_pcm_sframes_t wrote = snd_pcm_writei(pcm, buffer, got);
        if (wrote == -EPIPE)
        {
            snd_pcm_prepare(pcm);
            continue;
        }
        if (wrote == -EAGAIN || wrote == 0)
        {
            /* See capture_thread: virtual devices don't block on write. */
            usleep(2000);
            continue;
        }
        if (wrote < 0)
        {
            fprintf(stderr, "radio_media: playback write failed: %s\n",
                    snd_strerror((int) wrote));
            usleep(20000);
        }
    }

    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    audio_bridge_shutdown(&bridge);
    free(buffer);
    return NULL;
}

static bool daemon_audio_bridge_enabled(radio *radio_h)
{
    if (!radio_h->enable_audio_bridge)
        return false;

    if (!radio_pipeline_uses_daemon_audio_bridge(radio_h))
    {
        fprintf(stderr,
                "radio_media: ignoring enable_audio_bridge for pipeline %s; "
                "media remains on the %s path.\n",
                radio_pipeline_name(radio_h),
                radio_pipeline_media_owner_name(radio_h));
        return false;
    }

    return true;
}

static bool recording_supported(radio *radio_h, const char *stream_name)
{
    uint32_t caps = radio_pipeline_capabilities(radio_h);

    if (stream_matches(stream_name, "rx"))
        return (caps & RADIO_PIPELINE_CAP_RX_RECORDING) != 0;

    if (stream_matches(stream_name, "tx"))
        return (caps & RADIO_PIPELINE_CAP_TX_RECORDING) != 0;

    if (stream_matches(stream_name, "both"))
        return (caps & (RADIO_PIPELINE_CAP_RX_RECORDING |
                        RADIO_PIPELINE_CAP_TX_RECORDING)) ==
               (RADIO_PIPELINE_CAP_RX_RECORDING |
                RADIO_PIPELINE_CAP_TX_RECORDING);

    return false;
}

bool radio_media_init(radio *radio_h, pthread_t *capture_tid, pthread_t *playback_tid)
{
    static media_thread_ctx capture_ctx;
    static media_thread_ctx playback_ctx;
    uint32_t queue_samples = radio_h->audio_queue_samples ?
                             radio_h->audio_queue_samples : DEFAULT_QUEUE_SAMPLES;

    recording_init(&radio_h->rx_recording);
    recording_init(&radio_h->tx_recording);
    pthread_mutex_init(&radio_h->spectrum_mutex, NULL);
    radio_h->rx_spectrum_seq = 0;
    radio_h->tx_spectrum_seq = 0;
    radio_h->rx_spectrum_valid = false;
    radio_h->tx_spectrum_valid = false;

    pthread_mutex_lock(&g_spectrum_plan_mutex);
    if (!g_spectrum_plan_ready)
    {
        g_spectrum_plan = fftwf_plan_dft_r2c_1d(SPECTRUM_FFT_SIZE,
                                                g_spectrum_in,
                                                g_spectrum_out,
                                                FFTW_ESTIMATE);
        g_spectrum_plan_ready = (g_spectrum_plan != NULL);
    }
    pthread_mutex_unlock(&g_spectrum_plan_mutex);
    if (!g_spectrum_plan_ready)
        fprintf(stderr, "radio_media: warning: spectrum FFT plan unavailable\n");

    if (!ring_init(&radio_h->rx_audio_ring, queue_samples) ||
        !ring_init(&radio_h->tx_audio_ring, queue_samples) ||
        !ring_init(&radio_h->rx_radae_ring, queue_samples) ||
        !ring_init(&radio_h->tx_radae_ring, queue_samples))
    {
        fprintf(stderr, "radio_media: failed to allocate audio queues\n");
        return false;
    }

    if (!daemon_audio_bridge_enabled(radio_h))
        return true;

    capture_ctx.radio_h = radio_h;
    capture_ctx.capture = true;
    playback_ctx.radio_h = radio_h;
    playback_ctx.capture = false;

    if (pthread_create(capture_tid, NULL, capture_thread, &capture_ctx) != 0)
    {
        fprintf(stderr, "radio_media: cannot start capture thread\n");
        return false;
    }
    if (pthread_create(playback_tid, NULL, playback_thread, &playback_ctx) != 0)
    {
        fprintf(stderr, "radio_media: cannot start playback thread\n");
        shutdown_ = true;
        pthread_join(*capture_tid, NULL);
        return false;
    }

    return true;
}

void radio_media_shutdown(radio *radio_h, pthread_t *capture_tid, pthread_t *playback_tid)
{
    if (daemon_audio_bridge_enabled(radio_h))
    {
        pthread_cond_broadcast(&radio_h->tx_audio_ring.cond);
        pthread_join(*capture_tid, NULL);
        pthread_join(*playback_tid, NULL);
    }

    recording_destroy(&radio_h->rx_recording);
    recording_destroy(&radio_h->tx_recording);
    ring_destroy(&radio_h->rx_audio_ring);
    ring_destroy(&radio_h->tx_audio_ring);
    ring_destroy(&radio_h->rx_radae_ring);
    ring_destroy(&radio_h->tx_radae_ring);
    pthread_mutex_destroy(&radio_h->spectrum_mutex);

    pthread_mutex_lock(&g_spectrum_plan_mutex);
    if (g_spectrum_plan_ready)
    {
        fftwf_destroy_plan(g_spectrum_plan);
        g_spectrum_plan = NULL;
        g_spectrum_plan_ready = false;
    }
    pthread_mutex_unlock(&g_spectrum_plan_mutex);
}

void radio_media_push_tx_audio(radio *radio_h, const int16_t *samples, size_t nsamples)
{
    if (!radio_pipeline_supports_websocket_tx_audio(radio_h))
        return;

    ring_push(&radio_h->tx_audio_ring, samples, nsamples);

    if (!daemon_audio_bridge_enabled(radio_h))
    {
        recording_write(&radio_h->tx_recording, samples, nsamples);
        if (nsamples >= SPECTRUM_FFT_SIZE)
            compute_spectrum(radio_h, true, samples, nsamples);
    }
}

size_t radio_media_pop_rx_audio(radio *radio_h, int16_t *samples, size_t max_samples)
{
    if (!radio_pipeline_supports_websocket_rx_audio(radio_h))
        return 0;

    return ring_pop(&radio_h->rx_audio_ring, samples, max_samples);
}

/* Embedded-side audio taps. The sbitx ALSA path already writes RX into
 * radio_h->rx_audio_ring via sbitx_bridge_push_rx (and reads TX via
 * sbitx_bridge_pop_tx); these helpers just add the recording write +
 * spectrum compute on top of that flow. The wav_recording mutex makes
 * recording_write zero-overhead when no recording is active. */
/* Accumulate samples up to one FFT window before computing, since a single
 * capture period (480 frames) is now smaller than SPECTRUM_FFT_SIZE (2048). */
static void spectrum_accumulate(radio *radio_h, bool tx,
                                const int16_t *samples, size_t nsamples)
{
    static int16_t rx_acc[SPECTRUM_FFT_SIZE], tx_acc[SPECTRUM_FFT_SIZE];
    static size_t  rx_n = 0, tx_n = 0;
    int16_t *acc = tx ? tx_acc : rx_acc;
    size_t  *n   = tx ? &tx_n  : &rx_n;

    for (size_t i = 0; i < nsamples; i++)
    {
        acc[(*n)++] = samples[i];
        if (*n >= SPECTRUM_FFT_SIZE)
        {
            compute_spectrum(radio_h, tx, acc, *n);
            *n = 0;
        }
    }
}

void radio_media_tap_rx_audio(radio *radio_h, const int16_t *samples, size_t nsamples)
{
    if (radio_h->rx_recording.active)
        recording_write(&radio_h->rx_recording, samples, nsamples);
    if (radio_pipeline_supports_spectrum(radio_h, false))
        spectrum_accumulate(radio_h, false, samples, nsamples);
}

void radio_media_tap_tx_audio(radio *radio_h, const int16_t *samples, size_t nsamples)
{
    if (radio_h->tx_recording.active)
        recording_write(&radio_h->tx_recording, samples, nsamples);
    if (radio_pipeline_supports_spectrum(radio_h, true))
        spectrum_accumulate(radio_h, true, samples, nsamples);
}

static bool stream_matches(const char *stream_name, const char *candidate)
{
    return stream_name && !strcmp(stream_name, candidate);
}

bool radio_media_start_recording(radio *radio_h, const char *stream_name)
{
    bool ok = false;
    uint32_t sample_rate = radio_h->audio_sample_rate ? radio_h->audio_sample_rate : 8000;

    if (!recording_supported(radio_h, stream_name))
        return false;

    if (stream_matches(stream_name, "rx"))
        return recording_open(&radio_h->rx_recording, radio_h->recording_dir, "rx", sample_rate);

    if (stream_matches(stream_name, "tx"))
        return recording_open(&radio_h->tx_recording, radio_h->recording_dir, "tx", sample_rate);

    if (stream_matches(stream_name, "both"))
    {
        ok = recording_open(&radio_h->rx_recording, radio_h->recording_dir, "rx", sample_rate);
        ok = recording_open(&radio_h->tx_recording, radio_h->recording_dir, "tx", sample_rate) && ok;
        return ok;
    }

    return false;
}

bool radio_media_stop_recording(radio *radio_h, const char *stream_name)
{
    if (stream_matches(stream_name, "rx"))
    {
        recording_close(&radio_h->rx_recording);
        return true;
    }

    if (stream_matches(stream_name, "tx"))
    {
        recording_close(&radio_h->tx_recording);
        return true;
    }

    if (stream_matches(stream_name, "both"))
    {
        recording_close(&radio_h->rx_recording);
        recording_close(&radio_h->tx_recording);
        return true;
    }

    return false;
}

bool radio_media_get_spectrum(radio *radio_h, bool tx, float *out_bins, size_t bins,
                              uint32_t *seq, uint32_t *sample_rate)
{
    bool valid;

    if (!radio_pipeline_supports_spectrum(radio_h, tx))
        return false;

    if (bins < WATERFALL_BINS)
        return false;

    pthread_mutex_lock(&radio_h->spectrum_mutex);
    valid = tx ? radio_h->tx_spectrum_valid : radio_h->rx_spectrum_valid;
    if (valid)
    {
        memcpy(out_bins, tx ? radio_h->tx_spectrum : radio_h->rx_spectrum,
               sizeof(float) * WATERFALL_BINS);
        if (seq)
            *seq = tx ? radio_h->tx_spectrum_seq : radio_h->rx_spectrum_seq;
        if (sample_rate)
            *sample_rate = radio_h->spectrum_sample_rate;
    }
    pthread_mutex_unlock(&radio_h->spectrum_mutex);

    return valid;
}
