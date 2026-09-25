/* sBitx DRM (Digital Radio Mondiale) support via Dream subprocess
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define USE_FFTW
#define LIBCSDR_GPL
#include <fft_fftw.h>
#include <libcsdr.h>
#include <libcsdr_gpl.h>

#include "sbitx_drm.h"
#include "stream_resampler.h"

static pid_t dream_pid = -1;
static FILE *dream_in = NULL;
static FILE *dream_out = NULL;

/* Rate conversion with the state kept across blocks. csdr's
 * rational_resampler_ff() called per block with only that block dropped its
 * look-ahead tail every block, and the audio path read up to a whole block
 * of 8 kHz audio, upsampled it 12x and kept one block of it (11/12 lost). */
static stream_resampler rs_i, rs_q;          /* 96k -> 48k signal */
static stream_resampler rs_up;               /* 8k -> 96k audio */
static bool rs_ready = false;
static float audio_fifo[4096];
static int audio_fifo_n = 0;

static float audio_8k[2048];

static int dmode_audio_available = 0;

static void init_resamplers(void)
{
    if (rs_ready)
        return;
    rs_ready = stream_resampler_init(&rs_i, 1, 2, 0.05f) &&
               stream_resampler_init(&rs_q, 1, 2, 0.05f) &&
               stream_resampler_init(&rs_up, 12, 1, 0.01f);
    audio_fifo_n = 0;
}

bool sbitx_drm_init(const char *dream_path, uint32_t sigsrate, uint32_t audsrate)
{
    char sigsrate_str[16], audsrate_str[16];
    int pipe_stdin[2], pipe_stdout[2];

    if (!dream_path || !dream_path[0])
    {
        fprintf(stderr, "DRM: dream_path not set, DRM disabled\n");
        return false;
    }

    if (pipe(pipe_stdin) < 0 || pipe(pipe_stdout) < 0)
    {
        fprintf(stderr, "DRM: pipe failed: %s\n", strerror(errno));
        return false;
    }

    dream_pid = fork();
    if (dream_pid < 0)
    {
        fprintf(stderr, "DRM: fork failed: %s\n", strerror(errno));
        close(pipe_stdin[0]); close(pipe_stdin[1]);
        close(pipe_stdout[0]); close(pipe_stdout[1]);
        return false;
    }

    if (dream_pid == 0)
    {
        dup2(pipe_stdin[0], STDIN_FILENO);
        dup2(pipe_stdout[1], STDOUT_FILENO);
        close(pipe_stdin[0]); close(pipe_stdin[1]);
        close(pipe_stdout[0]); close(pipe_stdout[1]);

        for (int fd = 3; fd < 256; fd++)
            close(fd);

        snprintf(sigsrate_str, sizeof(sigsrate_str), "%u", sigsrate);
        snprintf(audsrate_str, sizeof(audsrate_str), "%u", audsrate);

        execlp(dream_path, dream_path,
               "--console",
               "-I", "-",
               "-O", "-",
               "--sigsrate", sigsrate_str,
               "--inchansel", "6",
               "--audsrate", audsrate_str,
               (char *) NULL);

        fprintf(stderr, "DRM: exec dream failed: %s\n", strerror(errno));
        _exit(1);
    }

    close(pipe_stdin[0]);
    close(pipe_stdout[1]);

    dream_in = fdopen(pipe_stdin[1], "w");
    dream_out = fdopen(pipe_stdout[0], "r");
    if (!dream_in || !dream_out)
    {
        fprintf(stderr, "DRM: fdopen failed\n");
        sbitx_drm_shutdown();
        return false;
    }

    init_resamplers();

    fprintf(stderr, "DRM: Dream subprocess started (pid=%d, sig=%u aud=%u)\n",
            dream_pid, sigsrate, audsrate);
    return true;
}

void sbitx_drm_shutdown(void)
{
    if (dream_pid > 0)
    {
        kill(dream_pid, SIGTERM);
        int status;
        for (int i = 0; i < 20; i++)
        {
            if (waitpid(dream_pid, &status, WNOHANG) > 0)
                break;
            usleep(100000);
        }
        if (waitpid(dream_pid, &status, WNOHANG) == 0)
        {
            kill(dream_pid, SIGKILL);
            waitpid(dream_pid, &status, 0);
        }
        dream_pid = -1;
    }

    if (dream_in) { fclose(dream_in); dream_in = NULL; }
    if (dream_out) { fclose(dream_out); dream_out = NULL; }

    stream_resampler_free(&rs_i);
    stream_resampler_free(&rs_q);
    stream_resampler_free(&rs_up);
    rs_ready = false;
    audio_fifo_n = 0;

    fprintf(stderr, "DRM: shutdown complete\n");
}

void sbitx_drm_process(const float *iq_i, const float *iq_q, int n,
                       float *audio_out, int *out_n)
{
    *out_n = 0;
    if (!dream_in || !dream_out || !rs_ready)
        return;

    size_t ni = stream_resampler_run(&rs_i, iq_i, (size_t) n);
    size_t nq = stream_resampler_run(&rs_q, iq_q, (size_t) n);
    int m = (int) (ni < nq ? ni : nq);
    const float *i_48k = rs_i.out, *q_48k = rs_q.out;

    if (m > 0)
    {
        static int16_t s16_buf[4096];
        if (m > (int) (sizeof(s16_buf) / sizeof(s16_buf[0]) / 2))
            m = (int) (sizeof(s16_buf) / sizeof(s16_buf[0]) / 2);
        for (int k = 0; k < m; k++)
        {
            float iv = i_48k[k] * 32767.0f;
            float qv = q_48k[k] * 32767.0f;
            if (iv > 32767.0f) iv = 32767.0f;
            if (iv < -32768.0f) iv = -32768.0f;
            if (qv > 32767.0f) qv = 32767.0f;
            if (qv < -32768.0f) qv = -32768.0f;
            s16_buf[k * 2] = (int16_t) iv;
            s16_buf[k * 2 + 1] = (int16_t) qv;
        }

        fwrite(s16_buf, sizeof(int16_t), (size_t)m * 2, dream_in);
    }

    /* Read only the 8 kHz audio this block needs (~n/12), upsample it into
     * the FIFO, and hand out up to n samples. */
    clearerr(dream_out);
    while (audio_fifo_n < n)
    {
        int want = (n - audio_fifo_n) / 12 + 2;
        if (want > (int) (sizeof(audio_8k) / sizeof(audio_8k[0])))
            want = (int) (sizeof(audio_8k) / sizeof(audio_8k[0]));
        size_t got = fread(audio_8k, sizeof(float), (size_t) want, dream_out);
        if (got == 0)
            break;
        for (size_t k = 0; k < got; k++)
        {
            float v = audio_8k[k];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            audio_8k[k] = v;
        }
        size_t up = stream_resampler_run(&rs_up, audio_8k, got);
        if (up > sizeof(audio_fifo) / sizeof(audio_fifo[0]) - (size_t) audio_fifo_n)
            up = sizeof(audio_fifo) / sizeof(audio_fifo[0]) - (size_t) audio_fifo_n;
        memcpy(audio_fifo + audio_fifo_n, rs_up.out, up * sizeof(float));
        audio_fifo_n += (int) up;
    }

    if (audio_fifo_n > 0)
    {
        *out_n = audio_fifo_n < n ? audio_fifo_n : n;
        memcpy(audio_out, audio_fifo, (size_t) *out_n * sizeof(float));
        audio_fifo_n -= *out_n;
        if (audio_fifo_n > 0)
            memmove(audio_fifo, audio_fifo + *out_n, (size_t) audio_fifo_n * sizeof(float));

        float max_amp = 0.01f;
        for (int k = 0; k < *out_n; k++)
        {
            float a = audio_out[k] > 0 ? audio_out[k] : -audio_out[k];
            if (a > max_amp) max_amp = a;
        }
        if (max_amp < 0.001f) max_amp = 0.001f;
        float gain = 0.9f / max_amp;
        for (int k = 0; k < *out_n; k++)
            audio_out[k] *= gain;

        dmode_audio_available = 1;
    }
}

int sbitx_drm_audio_available(void)
{
    return dmode_audio_available;
}
