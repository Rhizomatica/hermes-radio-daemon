/* loop_audio - ALSA snd-aloop bridge to the modem. See loop_audio.h.
 *
 * Two dedicated threads do the blocking ALSA I/O (paced by the genlocked
 * loopback clock), decoupled from the codec capture/playback threads by small
 * rings so a slow/stalled modem end can never stall the codec:
 *
 *   rx_thread: drain rx ring -> write loop_playback (hw:1,0)  -> mercury reads hw:1,1
 *   tx_thread: read loop_capture (hw:1,0 capture) <- mercury hw:1,1 -> tx_audio_ring
 *
 * loop_audio_push_rx() (called from the capture tap) only does a non-blocking
 * ring write, so the capture thread never blocks on ALSA.
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loop_audio.h"
#include "radio_media.h"

extern _Atomic bool shutdown_;

#define LOOP_CHANNELS    2
#define LOOP_PERIOD      480u            /* frames @48k = 10 ms */
#define LOOP_FRAMES      480u            /* read/write chunk (frames) */
#define RXRING_FRAMES    24000u          /* ~0.5 s mono int16 RX staging ring */

static snd_pcm_t *s_play;                /* loop_playback (hw:1,0): we write RX */
static snd_pcm_t *s_cap;                 /* loop_capture  (hw:1,0): we read TX  */
static pthread_t  s_tx_tid, s_rx_tid;
static volatile bool s_run;
static bool       s_started;
static radio     *s_radio;
static unsigned   s_rate = 48000;

/* RX staging ring (mono int16), filled by the capture tap, drained by rx_thread. */
static int16_t    s_rxring[RXRING_FRAMES];
static size_t     s_rx_head, s_rx_tail, s_rx_count;
static pthread_mutex_t s_rx_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_rx_cnd = PTHREAD_COND_INITIALIZER;

static snd_pcm_t *loop_open(const char *dev, snd_pcm_stream_t stream, unsigned rate)
{
    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, dev, stream, 0);
    if (err < 0)
    {
        fprintf(stderr, "loop_audio: snd_pcm_open(%s) failed: %s\n", dev, snd_strerror(err));
        return NULL;
    }
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    unsigned int r = rate;
    snd_pcm_uframes_t period = LOOP_PERIOD;
    unsigned int periods = 4;
    if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
        (err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(pcm, hw, LOOP_CHANNELS)) < 0 ||
        (err = snd_pcm_hw_params_set_rate_near(pcm, hw, &r, 0)) < 0 ||
        (err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0)) < 0 ||
        (err = snd_pcm_hw_params_set_periods_near(pcm, hw, &periods, 0)) < 0 ||
        (err = snd_pcm_hw_params(pcm, hw)) < 0)
    {
        fprintf(stderr, "loop_audio: hw_params(%s) failed: %s\n", dev, snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }
    snd_pcm_prepare(pcm);
    return pcm;
}

/* Drain the RX staging ring and write it (blocking) to the loopback playback.
 * Blocking here is fine: this is a dedicated thread and the genlocked loopback
 * paces it at the codec rate. */
static void *rx_thread(void *arg)
{
    (void) arg;
    int16_t mono[LOOP_FRAMES];
    int32_t st[LOOP_FRAMES * LOOP_CHANNELS];

    while (s_run)
    {
        pthread_mutex_lock(&s_rx_mtx);
        while (s_run && s_rx_count == 0)
            pthread_cond_wait(&s_rx_cnd, &s_rx_mtx);
        size_t n = s_rx_count < LOOP_FRAMES ? s_rx_count : LOOP_FRAMES;
        for (size_t i = 0; i < n; i++)
        {
            mono[i] = s_rxring[s_rx_tail];
            s_rx_tail = (s_rx_tail + 1) % RXRING_FRAMES;
        }
        s_rx_count -= n;
        pthread_mutex_unlock(&s_rx_mtx);

        if (!s_run)
            break;
        if (n == 0)
            continue;

        for (size_t i = 0; i < n; i++)
        {
            int32_t s = ((int32_t) mono[i]) << 16;
            st[i * LOOP_CHANNELS]     = s;
            st[i * LOOP_CHANNELS + 1] = s;
        }
        snd_pcm_sframes_t wrote = snd_pcm_writei(s_play, st, n);
        if (wrote == -EPIPE)
            snd_pcm_prepare(s_play);
        else if (wrote < 0)
            snd_pcm_recover(s_play, (int) wrote, 1);
    }
    return NULL;
}

/* Read the modem's TX audio from the loopback (stereo S32), left channel down
 * to mono int16, into the playback path (tx_audio_ring -> codec). */
static void *tx_thread(void *arg)
{
    (void) arg;
    int32_t in[LOOP_FRAMES * LOOP_CHANNELS];
    int16_t out[LOOP_FRAMES];

    while (s_run)
    {
        snd_pcm_sframes_t got = snd_pcm_readi(s_cap, in, LOOP_FRAMES);
        if (got == -EPIPE) { snd_pcm_prepare(s_cap); continue; }
        if (got < 0)
        {
            if (snd_pcm_recover(s_cap, (int) got, 1) < 0)
                usleep(20000);
            continue;
        }
        for (snd_pcm_sframes_t i = 0; i < got; i++)
            out[i] = (int16_t) (in[i * LOOP_CHANNELS] >> 16);
        radio_media_push_tx_audio(s_radio, out, (size_t) got);
    }
    return NULL;
}

bool loop_audio_init(radio *radio_h)
{
    if (s_started)
        return true;

    s_radio = radio_h;
    s_rate = radio_h->rig_audio_rate ? radio_h->rig_audio_rate : 48000;

    if (!radio_h->loop_playback_device[0] || !radio_h->loop_capture_device[0])
    {
        fprintf(stderr, "loop_audio: loop devices not configured\n");
        return false;
    }

    s_play = loop_open(radio_h->loop_playback_device, SND_PCM_STREAM_PLAYBACK, s_rate);
    s_cap  = loop_open(radio_h->loop_capture_device,  SND_PCM_STREAM_CAPTURE,  s_rate);
    if (!s_play || !s_cap)
    {
        if (s_play) snd_pcm_close(s_play);
        if (s_cap)  snd_pcm_close(s_cap);
        s_play = s_cap = NULL;
        return false;
    }

    s_rx_head = s_rx_tail = s_rx_count = 0;
    s_run = true;
    if (pthread_create(&s_rx_tid, NULL, rx_thread, NULL) != 0 ||
        pthread_create(&s_tx_tid, NULL, tx_thread, NULL) != 0)
    {
        fprintf(stderr, "loop_audio: failed to start threads\n");
        s_run = false;
        snd_pcm_close(s_play); snd_pcm_close(s_cap);
        s_play = s_cap = NULL;
        return false;
    }

    s_started = true;
    fprintf(stderr, "loop_audio: bridge up (RX->%s, TX<-%s, stereo S32 @ %u Hz)\n",
            radio_h->loop_playback_device, radio_h->loop_capture_device, s_rate);
    return true;
}

void loop_audio_push_rx(const int16_t *samples, size_t nsamples)
{
    if (!s_started || !samples || !nsamples)
        return;

    /* Non-blocking ring write from the capture tap; drop oldest on overflow so
     * the capture thread never stalls. rx_thread does the blocking loop write. */
    pthread_mutex_lock(&s_rx_mtx);
    for (size_t i = 0; i < nsamples; i++)
    {
        if (s_rx_count == RXRING_FRAMES)
        {
            s_rx_tail = (s_rx_tail + 1) % RXRING_FRAMES;   /* drop oldest */
            s_rx_count--;
        }
        s_rxring[s_rx_head] = samples[i];
        s_rx_head = (s_rx_head + 1) % RXRING_FRAMES;
        s_rx_count++;
    }
    pthread_cond_signal(&s_rx_cnd);
    pthread_mutex_unlock(&s_rx_mtx);
}

void loop_audio_shutdown(void)
{
    if (!s_started)
        return;
    s_run = false;
    pthread_mutex_lock(&s_rx_mtx);
    pthread_cond_broadcast(&s_rx_cnd);
    pthread_mutex_unlock(&s_rx_mtx);
    pthread_cancel(s_tx_tid);
    pthread_join(s_rx_tid, NULL);
    pthread_join(s_tx_tid, NULL);
    if (s_play) { snd_pcm_drop(s_play); snd_pcm_close(s_play); }
    if (s_cap)  { snd_pcm_drop(s_cap);  snd_pcm_close(s_cap); }
    s_play = s_cap = NULL;
    s_started = false;
}
