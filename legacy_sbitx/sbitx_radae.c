/*
 * sBitx RADEv2 Digital Voice Integration
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 * RADEv2 - Radio Autoencoder Version 2.
 * Uses the vendored in-process C encoder/decoder and keeps lpcnet_demo
 * only for speech/feature conversion.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "rade_api.h"
#include "sbitx_core.h"
#include "sbitx_radae.h"

/* Enable verbose debug logging when RADAE_DEBUG env var is set to 1 or true */
static int radae_debug = 0;

bool radae_is_debug(void) { return radae_debug != 0; }

// Helper macros for circular buffer operations
#define BUFFER_SIZE(write_idx, read_idx, max_size) \
    (((write_idx) - (read_idx) + (max_size)) % (max_size))

#define BUFFER_FREE(write_idx, read_idx, max_size) \
    ((max_size) - 1 - BUFFER_SIZE(write_idx, read_idx, max_size))

#define RADAE_PCM_FRAME_BYTES        ((size_t)RADAE_FRAME_SIZE * sizeof(int16_t))
#define RADAE_FEATURE_FRAME_FLOATS   36
#define RADAE_FEATURE_ACCUM_BYTES    8192
#define RADAE_PCM_ACCUM_BYTES        8192
#define RADAE_TX_COMP_CAPACITY       1024

// Sample-rate instrumentation: accumulates counts and prints once per second to
// stderr when radae_debug is on. `tag` is a literal string identifying the
// measurement point, `units` is a literal string naming what `n` counts
// (e.g. "samp", "csamp", "B"), `expect` is the nominal rate shown alongside
// the measured one so drift is obvious at a glance.
#define RADAE_RATE_LOG(tag, units, n, expect) do {                             \
    if (radae_debug) {                                                         \
        static uint64_t _st_count = 0;                                         \
        static uint64_t _st_calls = 0;                                         \
        static struct timespec _st_t0 = {0, 0};                                \
        struct timespec _st_now;                                               \
        clock_gettime(CLOCK_MONOTONIC, &_st_now);                              \
        if (_st_t0.tv_sec == 0 && _st_t0.tv_nsec == 0) _st_t0 = _st_now;       \
        _st_count += (uint64_t)(n);                                            \
        _st_calls += 1;                                                        \
        double _st_dt = (_st_now.tv_sec  - _st_t0.tv_sec) +                    \
                        (_st_now.tv_nsec - _st_t0.tv_nsec) / 1e9;              \
        if (_st_dt >= 1.0) {                                                   \
            fprintf(stderr,                                                    \
                "RADAE rate [%s]: %.0f %s/s (%.1f call/s, expect %s)\n",       \
                (tag), (double)_st_count/_st_dt, (units),                      \
                (double)_st_calls/_st_dt, (expect));                           \
            _st_count = 0;                                                     \
            _st_calls = 0;                                                     \
            _st_t0    = _st_now;                                               \
        }                                                                      \
    }                                                                          \
} while (0)

// TX thread: reads speech from lpcnet, runs vendored RADEv2 TX, writes modem IQ.
static void *radae_tx_thread(void *arg);

// RX thread: reads modem IQ, runs vendored RADEv2 RX, writes speech to lpcnet.
static void *radae_rx_thread(void *arg);

static bool set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool spawn_stdio_pipeline(const char *cmd, pid_t *pid_out, int *write_fd_out, int *read_fd_out)
{
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    pid_t pid;

    if (pipe(stdin_pipe) < 0) {
        fprintf(stderr, "RADAE: Failed to create stdin pipe: %s\n", strerror(errno));
        return false;
    }
    if (pipe(stdout_pipe) < 0) {
        fprintf(stderr, "RADAE: Failed to create stdout pipe: %s\n", strerror(errno));
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return false;
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "RADAE: Fork failed: %s\n", strerror(errno));
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return false;
    }

    if (pid == 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(1);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    if (!set_nonblocking(stdout_pipe[0])) {
        fprintf(stderr, "RADAE: Failed to set non-blocking mode: %s\n", strerror(errno));
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return false;
    }

    *pid_out = pid;
    *write_fd_out = stdin_pipe[1];
    *read_fd_out = stdout_pipe[0];
    return true;
}

static void terminate_stdio_pipeline(pid_t *pid, int *write_fd, int *read_fd)
{
    if (write_fd && *write_fd >= 0) {
        close(*write_fd);
        *write_fd = -1;
    }
    if (read_fd && *read_fd >= 0) {
        close(*read_fd);
        *read_fd = -1;
    }
    if (pid && *pid > 0) {
        kill(*pid, SIGTERM);
        waitpid(*pid, NULL, 0);
        *pid = 0;
    }
}

static bool write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;

    while (len > 0) {
        ssize_t written = write(fd, p, len);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec slp = {0, 1000000};
                nanosleep(&slp, NULL);
                continue;
            }
            return false;
        }
        if (written == 0)
            return false;
        p += written;
        len -= (size_t)written;
    }

    return true;
}

static void drain_fd(int fd)
{
    uint8_t sink[512];

    if (fd < 0)
        return;

    for (;;) {
        ssize_t bytes_read = read(fd, sink, sizeof(sink));
        if (bytes_read > 0)
            continue;
        if (bytes_read < 0 && errno == EINTR)
            continue;
        break;
    }
}

static void tx_store_modem_iq(radae_context *ctx, const RADE_COMP *samples, int n_complex)
{
    if (!ctx || !samples || n_complex <= 0)
        return;

    pthread_mutex_lock(&ctx->tx_mutex);

    int free_complex = BUFFER_FREE(ctx->tx_modem_buffer_write_idx,
                                   ctx->tx_modem_buffer_read_idx,
                                   RADAE_MODEM_BUFFER_SIZE * 2) / 2;
    int to_write = (n_complex < free_complex) ? n_complex : free_complex;

    for (int i = 0; i < to_write; i++) {
        ctx->tx_modem_buffer[ctx->tx_modem_buffer_write_idx] = samples[i].real;
        ctx->tx_modem_buffer_write_idx = (ctx->tx_modem_buffer_write_idx + 1) % (RADAE_MODEM_BUFFER_SIZE * 2);
        ctx->tx_modem_buffer[ctx->tx_modem_buffer_write_idx] = samples[i].imag;
        ctx->tx_modem_buffer_write_idx = (ctx->tx_modem_buffer_write_idx + 1) % (RADAE_MODEM_BUFFER_SIZE * 2);
    }

    pthread_mutex_unlock(&ctx->tx_mutex);

    RADAE_RATE_LOG("tx_out ctx->radio", "csamp", to_write, "8000 csamp/s");
}

static void rx_store_speech_pcm(radae_context *ctx, const int16_t *pcm_samples, int n_samples)
{
    if (!ctx || !pcm_samples || n_samples <= 0)
        return;

    pthread_mutex_lock(&ctx->rx_mutex);

    int free_space = BUFFER_FREE(ctx->rx_speech_buffer_write_idx,
                                 ctx->rx_speech_buffer_read_idx,
                                 RADAE_SPEECH_BUFFER_SIZE);
    int to_write = (n_samples < free_space) ? n_samples : free_space;

    for (int i = 0; i < to_write; i++) {
        ctx->rx_speech_buffer[ctx->rx_speech_buffer_write_idx] = (float)pcm_samples[i] / 32768.0f;
        ctx->rx_speech_buffer_write_idx = (ctx->rx_speech_buffer_write_idx + 1) % RADAE_SPEECH_BUFFER_SIZE;
    }

    pthread_mutex_unlock(&ctx->rx_mutex);

    RADAE_RATE_LOG("rx_out ctx->radio", "samp", to_write, "16000 samp/s");
}

static struct rade *radae_open_tx_session(void)
{
    int flags = radae_debug ? 0 : RADE_VERBOSE_0;
    struct rade *tx_rade = rade_tx_v2_pure_c_open(RADAE_MODEL_PATH, flags);

    if (!tx_rade)
        fprintf(stderr, "RADAE TX: failed to open vendored C encoder\n");
    return tx_rade;
}

static struct rade *radae_open_rx_session(void)
{
    int flags = radae_debug ? 0 : RADE_VERBOSE_0;
    struct rade *rx_rade = rade_rx_v2_pure_c_open(RADAE_MODEL_PATH, RADAE_SYNC_MODEL_PATH, flags);

    if (!rx_rade)
        fprintf(stderr, "RADAE RX: failed to open vendored C decoder\n");
    return rx_rade;
}

static bool radae_reset_tx_session(struct rade **tx_rade)
{
    if (*tx_rade)
        rade_close(*tx_rade);
    *tx_rade = radae_open_tx_session();
    return *tx_rade != NULL;
}

static bool radae_reset_rx_session(struct rade **rx_rade)
{
    if (*rx_rade)
        rade_close(*rx_rade);
    *rx_rade = radae_open_rx_session();
    return *rx_rade != NULL;
}

static bool tx_drain_lpcnet_features(radae_context *ctx,
                                     struct rade *tx_rade,
                                     int read_fd,
                                     uint8_t feature_accum[],
                                     size_t *feature_accum_len,
                                     RADE_COMP tx_out[])
{
    const size_t feature_bytes_needed = (size_t)rade_n_features_in_out(tx_rade) * sizeof(float);
    float features[256];

    if (feature_bytes_needed > sizeof(features) || feature_bytes_needed > RADAE_FEATURE_ACCUM_BYTES) {
        fprintf(stderr, "RADAE TX: unexpected feature payload size %zu\n", feature_bytes_needed);
        return false;
    }

    for (;;) {
        ssize_t bytes_read = read(read_fd,
                                  feature_accum + *feature_accum_len,
                                  RADAE_FEATURE_ACCUM_BYTES - *feature_accum_len);
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;
            fprintf(stderr, "RADAE TX: lpcnet feature read error: %s\n", strerror(errno));
            return false;
        }
        if (bytes_read == 0) {
            fprintf(stderr, "RADAE TX: lpcnet feature extractor closed stdout\n");
            return false;
        }

        *feature_accum_len += (size_t)bytes_read;
        while (*feature_accum_len >= feature_bytes_needed) {
            memcpy(features, feature_accum, feature_bytes_needed);
            int n_out = rade_tx(tx_rade, tx_out, features);
            if (n_out > 0)
                tx_store_modem_iq(ctx, tx_out, n_out);
            memmove(feature_accum,
                    feature_accum + feature_bytes_needed,
                    *feature_accum_len - feature_bytes_needed);
            *feature_accum_len -= feature_bytes_needed;
        }

        if (*feature_accum_len == RADAE_FEATURE_ACCUM_BYTES) {
            fprintf(stderr, "RADAE TX: feature accumulator overflow\n");
            return false;
        }
    }
}

static bool rx_drain_lpcnet_speech(radae_context *ctx,
                                   int read_fd,
                                   uint8_t pcm_accum[],
                                   size_t *pcm_accum_len)
{
    int16_t pcm_samples[RADAE_PCM_ACCUM_BYTES / sizeof(int16_t)];

    for (;;) {
        ssize_t bytes_read = read(read_fd,
                                  pcm_accum + *pcm_accum_len,
                                  RADAE_PCM_ACCUM_BYTES - *pcm_accum_len);
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;
            fprintf(stderr, "RADAE RX: lpcnet speech read error: %s\n", strerror(errno));
            return false;
        }
        if (bytes_read == 0) {
            fprintf(stderr, "RADAE RX: lpcnet synthesizer closed stdout\n");
            return false;
        }

        *pcm_accum_len += (size_t)bytes_read;
        size_t full_bytes = (*pcm_accum_len / sizeof(int16_t)) * sizeof(int16_t);
        if (full_bytes > 0) {
            int n_samples = (int)(full_bytes / sizeof(int16_t));
            memcpy(pcm_samples, pcm_accum, full_bytes);
            rx_store_speech_pcm(ctx, pcm_samples, n_samples);
            memmove(pcm_accum, pcm_accum + full_bytes, *pcm_accum_len - full_bytes);
            *pcm_accum_len -= full_bytes;
        }

        if (*pcm_accum_len == RADAE_PCM_ACCUM_BYTES) {
            fprintf(stderr, "RADAE RX: speech accumulator overflow\n");
            return false;
        }
    }
}

bool radae_init(radae_context *ctx, radio *radio_h, const char *radae_dir)
{
    memset(ctx, 0, sizeof(radae_context));

    ctx->radio_h = radio_h;
    strncpy(ctx->radae_dir, radae_dir, sizeof(ctx->radae_dir) - 1);

    pthread_mutex_init(&ctx->tx_mutex, NULL);
    pthread_mutex_init(&ctx->rx_mutex, NULL);
    pthread_cond_init(&ctx->tx_cond, NULL);
    pthread_cond_init(&ctx->rx_cond, NULL);

    ctx->tx_speech_buffer = (float *)calloc(RADAE_SPEECH_BUFFER_SIZE, sizeof(float));
    if (!ctx->tx_speech_buffer) {
        fprintf(stderr, "RADAE: Failed to allocate TX speech buffer\n");
        goto cleanup_mutex;
    }

    ctx->tx_modem_buffer = (float *)calloc(RADAE_MODEM_BUFFER_SIZE * 2, sizeof(float));
    if (!ctx->tx_modem_buffer) {
        fprintf(stderr, "RADAE: Failed to allocate TX modem buffer\n");
        goto cleanup_tx_speech;
    }

    ctx->rx_modem_buffer = (float *)calloc(RADAE_MODEM_BUFFER_SIZE * 2, sizeof(float));
    if (!ctx->rx_modem_buffer) {
        fprintf(stderr, "RADAE: Failed to allocate RX modem buffer\n");
        goto cleanup_tx_modem;
    }

    ctx->rx_speech_buffer = (float *)calloc(RADAE_SPEECH_BUFFER_SIZE, sizeof(float));
    if (!ctx->rx_speech_buffer) {
        fprintf(stderr, "RADAE: Failed to allocate RX speech buffer\n");
        goto cleanup_rx_modem;
    }

    char *dbg = getenv("RADAE_DEBUG");
    if (dbg && (strcmp(dbg, "1") == 0 || dbg[0] == 't' || dbg[0] == 'T')) {
        radae_debug = 1;
        fprintf(stderr, "RADAE: debug logging enabled\n");
    }

    rade_initialize();

    ctx->tx_running = false;
    ctx->tx_eoo_only = false;
    ctx->tx_eoo_pending = false;
    ctx->tx_reset_requested = true;
    ctx->rx_running = false;
    ctx->rx_reset_requested = false;
    ctx->shutdown_requested = false;
    ctx->initialized = true;

    if (pthread_create(&ctx->tx_thread, NULL, radae_tx_thread, ctx) != 0) {
        fprintf(stderr, "RADAE: Failed to create TX thread\n");
        goto cleanup_rade;
    }

    fprintf(stderr, "RADAE: Initialized with radae_dir=%s\n", ctx->radae_dir);
    return true;

cleanup_rade:
    ctx->initialized = false;
    rade_finalize();
    free(ctx->rx_speech_buffer);
cleanup_rx_modem:
    free(ctx->rx_modem_buffer);
cleanup_tx_modem:
    free(ctx->tx_modem_buffer);
cleanup_tx_speech:
    free(ctx->tx_speech_buffer);
cleanup_mutex:
    pthread_mutex_destroy(&ctx->tx_mutex);
    pthread_mutex_destroy(&ctx->rx_mutex);
    pthread_cond_destroy(&ctx->tx_cond);
    pthread_cond_destroy(&ctx->rx_cond);
    return false;
}

void radae_shutdown(radae_context *ctx)
{
    if (!ctx->initialized)
        return;

    ctx->shutdown_requested = true;
    radae_tx_stop(ctx);
    radae_rx_stop(ctx);

    pthread_cond_signal(&ctx->tx_cond);
    if (ctx->tx_feature_pid > 0)
        kill(ctx->tx_feature_pid, SIGTERM);
    pthread_join(ctx->tx_thread, NULL);

    pthread_mutex_destroy(&ctx->tx_mutex);
    pthread_mutex_destroy(&ctx->rx_mutex);
    pthread_cond_destroy(&ctx->tx_cond);
    pthread_cond_destroy(&ctx->rx_cond);

    free(ctx->tx_speech_buffer);
    free(ctx->tx_modem_buffer);
    free(ctx->rx_modem_buffer);
    free(ctx->rx_speech_buffer);

    ctx->initialized = false;
    rade_finalize();
    fprintf(stderr, "RADAE: Shutdown complete\n");
}

bool radae_tx_start(radae_context *ctx)
{
    if (!ctx->initialized || ctx->tx_running)
        return false;

    pthread_mutex_lock(&ctx->tx_mutex);
    ctx->tx_speech_buffer_write_idx = 0;
    ctx->tx_speech_buffer_read_idx = 0;
    ctx->tx_modem_buffer_write_idx = 0;
    ctx->tx_modem_buffer_read_idx = 0;
    ctx->tx_eoo_only = false;
    ctx->tx_eoo_pending = false;
    ctx->tx_reset_requested = true;
    ctx->tx_running = true;
    pthread_cond_signal(&ctx->tx_cond);
    pthread_mutex_unlock(&ctx->tx_mutex);

    fprintf(stderr, "RADAE TX: flow enabled\n");
    return true;
}

void radae_tx_stop(radae_context *ctx)
{
    if (!ctx->tx_running)
        return;

    ctx->tx_eoo_only = false;
    ctx->tx_eoo_pending = false;
    ctx->tx_running = false;
    pthread_cond_signal(&ctx->tx_cond);
    fprintf(stderr, "RADAE TX: flow disabled\n");
}

bool radae_tx_emit_eoo(radae_context *ctx)
{
    if (!ctx || !ctx->initialized || !ctx->tx_running)
        return false;

    pthread_mutex_lock(&ctx->tx_mutex);
    ctx->tx_speech_buffer_read_idx = ctx->tx_speech_buffer_write_idx;
    ctx->tx_eoo_only = true;
    ctx->tx_eoo_pending = true;
    pthread_cond_signal(&ctx->tx_cond);
    pthread_mutex_unlock(&ctx->tx_mutex);

    if (radae_debug)
        fprintf(stderr, "RADAE TX: EOO requested (queued for in-process encoder)\n");
    return true;
}

bool radae_rx_start(radae_context *ctx)
{
    if (!ctx->initialized || ctx->rx_running)
        return false;

    ctx->rx_running = true;
    ctx->rx_reset_requested = false;
    ctx->rx_modem_buffer_write_idx = 0;
    ctx->rx_modem_buffer_read_idx = 0;
    ctx->rx_speech_buffer_write_idx = 0;
    ctx->rx_speech_buffer_read_idx = 0;

    if (pthread_create(&ctx->rx_thread, NULL, radae_rx_thread, ctx) != 0) {
        fprintf(stderr, "RADAE: Failed to create RX thread\n");
        ctx->rx_running = false;
        return false;
    }

    fprintf(stderr, "RADAE RX: Started\n");
    return true;
}

void radae_rx_stop(radae_context *ctx)
{
    if (!ctx->rx_running)
        return;

    ctx->rx_running = false;
    pthread_cond_signal(&ctx->rx_cond);
    if (ctx->rx_synth_pid > 0)
        kill(ctx->rx_synth_pid, SIGTERM);
    pthread_join(ctx->rx_thread, NULL);

    fprintf(stderr, "RADAE RX: Stopped\n");
}

void radae_rx_flush(radae_context *ctx)
{
    if (!ctx)
        return;

    pthread_mutex_lock(&ctx->rx_mutex);
    ctx->rx_modem_buffer_write_idx = 0;
    ctx->rx_modem_buffer_read_idx = 0;
    ctx->rx_speech_buffer_write_idx = 0;
    ctx->rx_speech_buffer_read_idx = 0;
    ctx->rx_reset_requested = true;
    pthread_cond_signal(&ctx->rx_cond);
    pthread_mutex_unlock(&ctx->rx_mutex);
}

int radae_tx_write_speech(radae_context *ctx, const float *samples, int n_samples)
{
    if (!ctx->tx_running || ctx->tx_eoo_only || !samples || n_samples <= 0)
        return 0;

    pthread_mutex_lock(&ctx->tx_mutex);

    int free_space = BUFFER_FREE(ctx->tx_speech_buffer_write_idx,
                                 ctx->tx_speech_buffer_read_idx,
                                 RADAE_SPEECH_BUFFER_SIZE);
    int to_write = (n_samples < free_space) ? n_samples : free_space;

    for (int i = 0; i < to_write; i++) {
        ctx->tx_speech_buffer[ctx->tx_speech_buffer_write_idx] = samples[i];
        ctx->tx_speech_buffer_write_idx = (ctx->tx_speech_buffer_write_idx + 1) % RADAE_SPEECH_BUFFER_SIZE;
    }

    pthread_cond_signal(&ctx->tx_cond);
    pthread_mutex_unlock(&ctx->tx_mutex);

    RADAE_RATE_LOG("tx_in  radio->ctx", "samp", to_write, "16000 samp/s");
    return to_write;
}

int radae_tx_read_modem_iq(radae_context *ctx, float *iq_samples, int max_samples)
{
    if (!ctx->tx_running || !iq_samples || max_samples <= 0)
        return 0;

    pthread_mutex_lock(&ctx->tx_mutex);

    int available = BUFFER_SIZE(ctx->tx_modem_buffer_write_idx,
                                ctx->tx_modem_buffer_read_idx,
                                RADAE_MODEM_BUFFER_SIZE * 2) / 2;
    int to_read = (max_samples < available) ? max_samples : available;

    for (int i = 0; i < to_read * 2; i++) {
        iq_samples[i] = ctx->tx_modem_buffer[ctx->tx_modem_buffer_read_idx];
        ctx->tx_modem_buffer_read_idx = (ctx->tx_modem_buffer_read_idx + 1) % (RADAE_MODEM_BUFFER_SIZE * 2);
    }

    pthread_mutex_unlock(&ctx->tx_mutex);

    RADAE_RATE_LOG("tx_out ctx->radio", "csamp", to_read, "8000 csamp/s");
    return to_read;
}

int radae_rx_write_modem_iq(radae_context *ctx, const float *iq_samples, int n_samples)
{
    if (!ctx->rx_running || !iq_samples || n_samples <= 0)
        return 0;

    pthread_mutex_lock(&ctx->rx_mutex);

    int free_space = BUFFER_FREE(ctx->rx_modem_buffer_write_idx,
                                 ctx->rx_modem_buffer_read_idx,
                                 RADAE_MODEM_BUFFER_SIZE * 2) / 2;
    int to_write = (n_samples < free_space) ? n_samples : free_space;

    for (int i = 0; i < to_write * 2; i++) {
        ctx->rx_modem_buffer[ctx->rx_modem_buffer_write_idx] = iq_samples[i];
        ctx->rx_modem_buffer_write_idx = (ctx->rx_modem_buffer_write_idx + 1) % (RADAE_MODEM_BUFFER_SIZE * 2);
    }

    pthread_cond_signal(&ctx->rx_cond);
    pthread_mutex_unlock(&ctx->rx_mutex);

    RADAE_RATE_LOG("rx_in  radio->ctx", "csamp", to_write, "8000 csamp/s");
    return to_write;
}

int radae_rx_read_speech(radae_context *ctx, float *samples, int max_samples)
{
    if (!ctx->rx_running || !samples || max_samples <= 0)
        return 0;

    pthread_mutex_lock(&ctx->rx_mutex);

    int available = BUFFER_SIZE(ctx->rx_speech_buffer_write_idx,
                                ctx->rx_speech_buffer_read_idx,
                                RADAE_SPEECH_BUFFER_SIZE);
    int to_read = (max_samples < available) ? max_samples : available;

    for (int i = 0; i < to_read; i++) {
        samples[i] = ctx->rx_speech_buffer[ctx->rx_speech_buffer_read_idx];
        ctx->rx_speech_buffer_read_idx = (ctx->rx_speech_buffer_read_idx + 1) % RADAE_SPEECH_BUFFER_SIZE;
    }

    pthread_mutex_unlock(&ctx->rx_mutex);

    RADAE_RATE_LOG("rx_out ctx->radio", "samp", to_read, "16000 samp/s");
    return to_read;
}

static void *radae_tx_thread(void *arg)
{
    radae_context *ctx = (radae_context *)arg;
    struct rade *tx_rade = NULL;
    RADE_COMP tx_out[RADAE_TX_COMP_CAPACITY];
    uint8_t feature_accum[RADAE_FEATURE_ACCUM_BYTES];
    size_t feature_accum_len = 0;
    int16_t pcm_buffer[RADAE_FRAME_SIZE];
    float speech_buffer[RADAE_FRAME_SIZE];
    int lpcnet_write_fd = -1;
    int lpcnet_read_fd = -1;
    bool session_ready = false;
    char cmd[1024];

    snprintf(cmd, sizeof(cmd),
             "cd %s && stdbuf -o0 %s -features - -",
             ctx->radae_dir,
             RADAE_LPCNET_BINARY_PATH);

    if (!spawn_stdio_pipeline(cmd, &ctx->tx_feature_pid, &lpcnet_write_fd, &lpcnet_read_fd))
        goto cleanup;

    tx_rade = radae_open_tx_session();
    if (!tx_rade)
        goto cleanup;
    if (rade_n_tx_eoo_out(tx_rade) > RADAE_TX_COMP_CAPACITY) {
        fprintf(stderr, "RADAE TX: output buffer too small for EOO frame\n");
        goto cleanup;
    }

    fprintf(stderr, "RADAE TX: in-process RADEv2 encoder ready\n");

    while (!ctx->shutdown_requested) {
        if (!ctx->tx_running) {
            drain_fd(lpcnet_read_fd);
            feature_accum_len = 0;
            session_ready = false;
            pthread_mutex_lock(&ctx->tx_mutex);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&ctx->tx_cond, &ctx->tx_mutex, &ts);
            pthread_mutex_unlock(&ctx->tx_mutex);
            continue;
        }

        if (!session_ready || ctx->tx_reset_requested) {
            if (!radae_reset_tx_session(&tx_rade))
                break;
            ctx->tx_reset_requested = false;
            feature_accum_len = 0;
            drain_fd(lpcnet_read_fd);
            session_ready = true;
        }

        pthread_mutex_lock(&ctx->tx_mutex);
        bool emit_eoo = ctx->tx_eoo_pending;
        bool eoo_only = ctx->tx_eoo_only;
        if (emit_eoo)
            ctx->tx_eoo_pending = false;
        pthread_mutex_unlock(&ctx->tx_mutex);

        if (emit_eoo) {
            drain_fd(lpcnet_read_fd);
            feature_accum_len = 0;
            int n_out = rade_tx_eoo(tx_rade, tx_out);
            if (n_out > 0)
                tx_store_modem_iq(ctx, tx_out, n_out);
            continue;
        }

        if (!eoo_only) {
            bool have_frame = false;

            pthread_mutex_lock(&ctx->tx_mutex);
            int available = BUFFER_SIZE(ctx->tx_speech_buffer_write_idx,
                                        ctx->tx_speech_buffer_read_idx,
                                        RADAE_SPEECH_BUFFER_SIZE);
            if (available >= RADAE_FRAME_SIZE) {
                for (int i = 0; i < RADAE_FRAME_SIZE; i++) {
                    speech_buffer[i] = ctx->tx_speech_buffer[ctx->tx_speech_buffer_read_idx];
                    ctx->tx_speech_buffer_read_idx = (ctx->tx_speech_buffer_read_idx + 1) % RADAE_SPEECH_BUFFER_SIZE;
                }
                have_frame = true;
            } else {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 10 * 1000 * 1000;
                if (ts.tv_nsec >= 1000000000) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000;
                }
                pthread_cond_timedwait(&ctx->tx_cond, &ctx->tx_mutex, &ts);
            }
            pthread_mutex_unlock(&ctx->tx_mutex);

            if (have_frame) {
                for (int i = 0; i < RADAE_FRAME_SIZE; i++) {
                    float s = speech_buffer[i] * 32767.0f;
                    if (s > 32767.0f)
                        s = 32767.0f;
                    if (s < -32768.0f)
                        s = -32768.0f;
                    pcm_buffer[i] = (int16_t)s;
                }
                if (!write_all(lpcnet_write_fd, pcm_buffer, RADAE_PCM_FRAME_BYTES)) {
                    fprintf(stderr, "RADAE TX: lpcnet feature write error: %s\n", strerror(errno));
                    break;
                }
                RADAE_RATE_LOG("tx_lpcnet stdin", "samp", RADAE_FRAME_SIZE, "16000 samp/s");
            }
        } else {
            drain_fd(lpcnet_read_fd);
            struct timespec slp = {0, 10 * 1000 * 1000};
            nanosleep(&slp, NULL);
        }

        if (!tx_drain_lpcnet_features(ctx,
                                      tx_rade,
                                      lpcnet_read_fd,
                                      feature_accum,
                                      &feature_accum_len,
                                      tx_out))
            break;
    }

cleanup:
    terminate_stdio_pipeline(&ctx->tx_feature_pid, &lpcnet_write_fd, &lpcnet_read_fd);
    if (tx_rade)
        rade_close(tx_rade);
    fprintf(stderr, "RADAE TX: Thread exiting\n");
    return NULL;
}

static void *radae_rx_thread(void *arg)
{
    radae_context *ctx = (radae_context *)arg;
    struct rade *rx_rade = NULL;
    uint8_t pcm_accum[RADAE_PCM_ACCUM_BYTES];
    size_t pcm_accum_len = 0;
    int lpcnet_write_fd = -1;
    int lpcnet_read_fd = -1;
    char cmd[1024];
    RADE_COMP *rx_in = NULL;
    float *features_out = NULL;

    snprintf(cmd, sizeof(cmd),
             "cd %s && stdbuf -o0 %s -fargan-synthesis - -",
             ctx->radae_dir,
             RADAE_LPCNET_BINARY_PATH);

    if (!spawn_stdio_pipeline(cmd, &ctx->rx_synth_pid, &lpcnet_write_fd, &lpcnet_read_fd))
        goto cleanup;

    rx_rade = radae_open_rx_session();
    if (!rx_rade)
        goto cleanup;

    int rx_frame_capacity = rade_nin_max(rx_rade);
    int feature_capacity = rade_n_features_in_out(rx_rade);
    rx_in = (RADE_COMP *)calloc((size_t)rx_frame_capacity, sizeof(*rx_in));
    features_out = (float *)calloc((size_t)feature_capacity, sizeof(*features_out));
    if (!rx_in || !features_out) {
        fprintf(stderr, "RADAE RX: Failed to allocate decoder buffers\n");
        goto cleanup;
    }

    fprintf(stderr, "RADAE RX: in-process RADEv2 decoder ready\n");

    while (ctx->rx_running && !ctx->shutdown_requested) {
        if (ctx->rx_reset_requested) {
            if (!radae_reset_rx_session(&rx_rade))
                break;
            ctx->rx_reset_requested = false;
            pcm_accum_len = 0;
            drain_fd(lpcnet_read_fd);
        }

        int nin = rade_nin(rx_rade);
        bool have_frame = false;

        pthread_mutex_lock(&ctx->rx_mutex);
        int available = BUFFER_SIZE(ctx->rx_modem_buffer_write_idx,
                                    ctx->rx_modem_buffer_read_idx,
                                    RADAE_MODEM_BUFFER_SIZE * 2) / 2;
        if (available >= nin) {
            for (int i = 0; i < nin; i++) {
                rx_in[i].real = ctx->rx_modem_buffer[ctx->rx_modem_buffer_read_idx];
                ctx->rx_modem_buffer_read_idx = (ctx->rx_modem_buffer_read_idx + 1) % (RADAE_MODEM_BUFFER_SIZE * 2);
                rx_in[i].imag = ctx->rx_modem_buffer[ctx->rx_modem_buffer_read_idx];
                ctx->rx_modem_buffer_read_idx = (ctx->rx_modem_buffer_read_idx + 1) % (RADAE_MODEM_BUFFER_SIZE * 2);
            }
            have_frame = true;
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 10 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&ctx->rx_cond, &ctx->rx_mutex, &ts);
        }
        pthread_mutex_unlock(&ctx->rx_mutex);

        if (have_frame) {
            int has_eoo_out = 0;
            int n_features = rade_rx_v2_pure_c(rx_rade, features_out, &has_eoo_out, NULL, rx_in);
            (void)has_eoo_out;
            if (n_features > 0) {
                if (!write_all(lpcnet_write_fd, features_out, (size_t)n_features * sizeof(float))) {
                    fprintf(stderr, "RADAE RX: lpcnet synth write error: %s\n", strerror(errno));
                    break;
                }
                RADAE_RATE_LOG("rx_lpcnet stdin", "feat", n_features, "decoded features");
            }
        }

        if (!rx_drain_lpcnet_speech(ctx, lpcnet_read_fd, pcm_accum, &pcm_accum_len))
            break;
    }

cleanup:
    terminate_stdio_pipeline(&ctx->rx_synth_pid, &lpcnet_write_fd, &lpcnet_read_fd);
    free(rx_in);
    free(features_out);
    if (rx_rade)
        rade_close(rx_rade);
    fprintf(stderr, "RADAE RX: Thread exiting\n");
    return NULL;
}

// Sample rate conversion utilities

// Simple linear interpolation resampler
static void resample_linear(const double *in, int in_len, double *out, int out_len)
{
    if (in_len <= 1 || out_len <= 1) return;
    
    double ratio = (double)(in_len - 1) / (double)(out_len - 1);
    
    for (int i = 0; i < out_len; i++) {
        double pos = i * ratio;
        int idx = (int)pos;
        double frac = pos - idx;
        
        if (idx >= in_len - 1) {
            out[i] = in[in_len - 1];
        } else {
            out[i] = in[idx] * (1.0 - frac) + in[idx + 1] * frac;
        }
    }
}

void resample_96k_to_16k(const double *in, int in_len, float *out, int *out_len)
{
    // 96kHz to 16kHz = 6:1 decimation
    *out_len = in_len / 6;
    double temp[*out_len];
    resample_linear(in, in_len, temp, *out_len);
    for (int i = 0; i < *out_len; i++) {
        out[i] = (float)temp[i];
    }
}

void resample_16k_to_96k(const float *in, int in_len, double *out, int *out_len)
{
    // 16kHz to 96kHz = 1:6 interpolation
    *out_len = in_len * 6;
    double temp[in_len];
    for (int i = 0; i < in_len; i++) {
        temp[i] = (double)in[i];
    }
    resample_linear(temp, in_len, out, *out_len);
}

void resample_48k_to_16k(const double *in, int in_len, float *out, int *out_len)
{
    // 48kHz to 16kHz = 3:1 decimation
    *out_len = in_len / 3;
    double temp[*out_len];
    resample_linear(in, in_len, temp, *out_len);
    for (int i = 0; i < *out_len; i++) {
        out[i] = (float)temp[i];
    }
}

void resample_96k_to_8k(const double *in, int in_len, float *out, int *out_len)
{
    // 96kHz to 8kHz = 12:1 decimation
    *out_len = in_len / 12;
    double temp[*out_len];
    resample_linear(in, in_len, temp, *out_len);
    for (int i = 0; i < *out_len; i++) {
        out[i] = (float)temp[i];
    }
}

void resample_8k_to_96k(const float *in, int in_len, double *out, int *out_len)
{
    // 8kHz to 96kHz = 1:12 interpolation
    *out_len = in_len * 12;
    double temp[in_len];
    for (int i = 0; i < in_len; i++) {
        temp[i] = (double)in[i];
    }
    resample_linear(temp, in_len, out, *out_len);
}
