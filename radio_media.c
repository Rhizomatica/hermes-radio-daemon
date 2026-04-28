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

extern _Atomic bool shutdown_;

#define DEFAULT_PERIOD_FRAMES 160
#define DEFAULT_QUEUE_SAMPLES 16000
#define SPECTRUM_FFT_SIZE 256

typedef struct {
    radio *radio_h;
    bool capture;
} media_thread_ctx;

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

    err = snd_pcm_set_params(pcm,
                             SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             1,
                             sample_rate,
                             1,
                             500000);
    if (err < 0)
    {
        fprintf(stderr, "radio_media: snd_pcm_set_params(%s) failed: %s\n",
                device, snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }

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
    float input[SPECTRUM_FFT_SIZE];
    fftwf_complex output[(SPECTRUM_FFT_SIZE / 2) + 1];
    float bins[WATERFALL_BINS];
    fftwf_plan plan;

    if (nsamples < SPECTRUM_FFT_SIZE)
        return;

    for (size_t i = 0; i < SPECTRUM_FFT_SIZE; i++)
    {
        float window = 0.5f - 0.5f * cosf((2.0f * (float) M_PI * i) /
                                          (float) (SPECTRUM_FFT_SIZE - 1));
        input[i] = ((float) samples[i] / 32768.0f) * window;
    }

    plan = fftwf_plan_dft_r2c_1d(SPECTRUM_FFT_SIZE, input, output, FFTW_ESTIMATE);
    if (!plan)
        return;

    fftwf_execute(plan);
    fftwf_destroy_plan(plan);

    for (size_t i = 0; i < WATERFALL_BINS; i++)
    {
        size_t src = i;
        float re = output[src][0];
        float im = output[src][1];
        float mag = (re * re) + (im * im);
        float db = 10.0f * log10f(mag + 1.0e-12f);
        bins[i] = db < floor_db ? floor_db : db;
    }

    radio_h->spectrum_sample_rate = radio_h->audio_sample_rate;
    update_spectrum_locked(radio_h, tx, bins);
}

static void *capture_thread(void *ctx_v)
{
    media_thread_ctx *ctx = (media_thread_ctx *) ctx_v;
    radio *radio_h = ctx->radio_h;
    uint32_t sample_rate = radio_h->audio_sample_rate ? radio_h->audio_sample_rate : 8000;
    uint32_t frames = radio_h->audio_period_size ? radio_h->audio_period_size : DEFAULT_PERIOD_FRAMES;
    int16_t *buffer = calloc(frames, sizeof(int16_t));
    int16_t spectrum_window[SPECTRUM_FFT_SIZE] = {0};
    size_t spectrum_fill = 0;
    snd_pcm_t *pcm;

    if (!buffer)
        return NULL;

    pcm = open_pcm_device(radio_h->capture_device, SND_PCM_STREAM_CAPTURE, sample_rate);
    if (!pcm)
    {
        free(buffer);
        return NULL;
    }

    while (!shutdown_)
    {
        snd_pcm_sframes_t got = snd_pcm_readi(pcm, buffer, frames);
        if (got == -EPIPE)
        {
            snd_pcm_prepare(pcm);
            continue;
        }
        if (got < 0)
        {
            fprintf(stderr, "radio_media: capture read failed: %s\n", snd_strerror((int) got));
            usleep(20000);
            continue;
        }
        if (got == 0)
            continue;

        ring_push(&radio_h->rx_audio_ring, buffer, (size_t) got);
        recording_write(&radio_h->rx_recording, buffer, (size_t) got);
        for (size_t i = 0; i < (size_t) got; i++)
        {
            if (spectrum_fill < SPECTRUM_FFT_SIZE)
            {
                spectrum_window[spectrum_fill++] = buffer[i];
            }
            else
            {
                memmove(spectrum_window, spectrum_window + 1,
                        (SPECTRUM_FFT_SIZE - 1) * sizeof(int16_t));
                spectrum_window[SPECTRUM_FFT_SIZE - 1] = buffer[i];
            }
        }
        if (spectrum_fill == SPECTRUM_FFT_SIZE)
            compute_spectrum(radio_h, false, spectrum_window, SPECTRUM_FFT_SIZE);
    }

    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    free(buffer);
    return NULL;
}

static void *playback_thread(void *ctx_v)
{
    media_thread_ctx *ctx = (media_thread_ctx *) ctx_v;
    radio *radio_h = ctx->radio_h;
    uint32_t sample_rate = radio_h->audio_sample_rate ? radio_h->audio_sample_rate : 8000;
    uint32_t frames = radio_h->audio_period_size ? radio_h->audio_period_size : DEFAULT_PERIOD_FRAMES;
    int16_t *buffer = calloc(frames, sizeof(int16_t));
    int16_t spectrum_window[SPECTRUM_FFT_SIZE] = {0};
    size_t spectrum_fill = 0;
    snd_pcm_t *pcm;

    if (!buffer)
        return NULL;

    pcm = open_pcm_device(radio_h->playback_device, SND_PCM_STREAM_PLAYBACK, sample_rate);
    if (!pcm)
    {
        free(buffer);
        return NULL;
    }

    while (!shutdown_)
    {
        size_t got = ring_pop(&radio_h->tx_audio_ring, buffer, frames);
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

        recording_write(&radio_h->tx_recording, buffer, got);
        for (size_t i = 0; i < got; i++)
        {
            if (spectrum_fill < SPECTRUM_FFT_SIZE)
            {
                spectrum_window[spectrum_fill++] = buffer[i];
            }
            else
            {
                memmove(spectrum_window, spectrum_window + 1,
                        (SPECTRUM_FFT_SIZE - 1) * sizeof(int16_t));
                spectrum_window[SPECTRUM_FFT_SIZE - 1] = buffer[i];
            }
        }
        if (spectrum_fill == SPECTRUM_FFT_SIZE)
            compute_spectrum(radio_h, true, spectrum_window, SPECTRUM_FFT_SIZE);

        snd_pcm_sframes_t wrote = snd_pcm_writei(pcm, buffer, got);
        if (wrote == -EPIPE)
        {
            snd_pcm_prepare(pcm);
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
    free(buffer);
    return NULL;
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

    if (!ring_init(&radio_h->rx_audio_ring, queue_samples) ||
        !ring_init(&radio_h->tx_audio_ring, queue_samples))
    {
        fprintf(stderr, "radio_media: failed to allocate audio queues\n");
        return false;
    }

    if (!radio_h->enable_audio_bridge)
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
    if (radio_h->enable_audio_bridge)
    {
        pthread_cond_broadcast(&radio_h->tx_audio_ring.cond);
        pthread_join(*capture_tid, NULL);
        pthread_join(*playback_tid, NULL);
    }

    recording_destroy(&radio_h->rx_recording);
    recording_destroy(&radio_h->tx_recording);
    ring_destroy(&radio_h->rx_audio_ring);
    ring_destroy(&radio_h->tx_audio_ring);
    pthread_mutex_destroy(&radio_h->spectrum_mutex);
}

void radio_media_push_tx_audio(radio *radio_h, const int16_t *samples, size_t nsamples)
{
    ring_push(&radio_h->tx_audio_ring, samples, nsamples);

    if (!radio_h->enable_audio_bridge)
    {
        recording_write(&radio_h->tx_recording, samples, nsamples);
        if (nsamples >= SPECTRUM_FFT_SIZE)
            compute_spectrum(radio_h, true, samples, nsamples);
    }
}

size_t radio_media_pop_rx_audio(radio *radio_h, int16_t *samples, size_t max_samples)
{
    return ring_pop(&radio_h->rx_audio_ring, samples, max_samples);
}

static bool stream_matches(const char *stream_name, const char *candidate)
{
    return stream_name && !strcmp(stream_name, candidate);
}

bool radio_media_start_recording(radio *radio_h, const char *stream_name)
{
    bool ok = false;
    uint32_t sample_rate = radio_h->audio_sample_rate ? radio_h->audio_sample_rate : 8000;

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
