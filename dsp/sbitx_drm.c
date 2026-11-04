/* sBitx DRM (Digital Radio Mondiale) through Dream subprocesses
 *
 * Copyright (C) 2024-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dream_proc.h"
#include "sbitx_drm.h"

#include "radiod_log.h"

#define LOG_C "drm"

/* 4 s of each stream: Dream's frames are 400 ms, its interleaver 2.4 s */
#define DRM_RING (SBITX_DRM_RATE * 2 * 4)

static dream_proc *rx, *tx;
static double tx_phase;

/* Dream hands out its audio and its I/Q a 400 ms frame at a time, the DSP
 * takes it 10 ms at a time: half a second is queued before any goes out,
 * and again whenever the queue runs dry, so the frames do not leave gaps. */
#define DRM_PRIME (SBITX_DRM_RATE / 2)
static bool rx_primed, tx_primed;

static int16_t to_s16(float v)
{
    v *= 32767.0f;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    return (int16_t) lrintf(v);
}

/* ---- receive ---- */

bool sbitx_drm_rx_start(const char *dream_path, const char *dir)
{
    /* "I/Q input positive, 0 Hz IF": the IQ is the dial's; audio out mono.
     * An empty Dream.ini each time: what a previous run saved does not
     * carry over. */
    static const char *const args[] = {
        "dream", "-n", "-I", "-", "-c", "6", "-O", "-mono", NULL
    };

    if (rx)
        return true;
    rx_primed = false;
    rx = dream_proc_start(dream_path, dir, "", args, DRM_RING);
    if (!rx)
        HLOGE(LOG_C, "cannot start Dream's receiver (%s)", dream_path);
    return rx != NULL;
}

void sbitx_drm_rx_stop(void)
{
    if (!rx)
        return;
    dream_proc_stop(rx);
    rx = NULL;
}

bool sbitx_drm_rx_running(void)
{
    return rx && dream_proc_running(rx);
}

void sbitx_drm_rx_iq(const float *i, const float *q, size_t n)
{
    int16_t s[2048];

    while (rx && n) {
        size_t k = n < sizeof(s) / sizeof(s[0]) / 2 ? n : sizeof(s) / sizeof(s[0]) / 2;

        for (size_t j = 0; j < k; j++) {
            s[2 * j] = to_s16(i[j]);
            s[2 * j + 1] = to_s16(q[j]);
        }
        dream_proc_write(rx, s, 2 * k);
        i += k;
        q += k;
        n -= k;
    }
}

size_t sbitx_drm_rx_audio(float *out, size_t n)
{
    int16_t s[2048];
    size_t got = 0;

    if (!rx)
        return 0;
    if (!rx_primed) {
        if (dream_proc_readable(rx) < DRM_PRIME)
            return 0;
        rx_primed = true;
    }
    while (got < n) {
        size_t want = n - got < sizeof(s) / sizeof(s[0]) ? n - got : sizeof(s) / sizeof(s[0]);
        size_t k = dream_proc_read(rx, s, want);

        if (!k) {
            rx_primed = false;          /* ran dry: queue again */
            break;
        }
        for (size_t j = 0; j < k; j++)
            out[got + j] = (float) s[j] / 32768.0f;
        got += k;
    }
    return got;
}

/* ---- transmit ---- */

int sbitx_drm_tx_ini(const sbitx_drm_tx_config *c, char *buf, size_t len)
{
    char robustness = c->robustness >= 'A' && c->robustness <= 'D' ? c->robustness : 'B';
    int spectrum = c->spectrum >= 0 && c->spectrum <= 5 ? c->spectrum : 3;
    int protection = c->protection >= 0 && c->protection <= 3 ? c->protection : 1;
    int n;

    /* the SDC in 4-QAM, as robust as it gets: it is what a receiver needs
     * first, and its capacity is the least of the channel */
    n = snprintf(buf, len,
                 "[Transmitter]\n"
                 "robustness=RM_ROBUSTNESS_MODE_%c\n"
                 "spectocc=SO_%d\n"
                 "msc=%s\n"
                 "sdc=CS_1_SM\n"
                 "protlevel=%d\n"
                 "interleaver=SI_LONG\n"
                 "ifformat=OF_IQ_POS\n"
                 "iffreq=%d\n"
                 "hqiq=1\n"
                 "\n"
                 "[Transmitter Service 1]\n"
                 "codec=%s\n"
                 "audiomode=mono\n"
                 "label=%s\n"
                 "id=1\n",
                 robustness, spectrum, c->msc_qam == 16 ? "CS_2_SM" : "CS_3_SM",
                 protection, (int) SBITX_DRM_TX_IF,
                 c->codec[0] ? c->codec : "xHE-AAC",
                 c->label[0] ? c->label : "HERMES");
    return n > 0 && (size_t) n < len ? n : -1;
}

bool sbitx_drm_tx_start(const char *dream_path, const char *dir, const sbitx_drm_tx_config *cfg)
{
    /* audio in mono, I/Q out on the two channels */
    static const char *const args[] = {
        "dream", "-n", "-t", "-I", "-mono", "-O", "-", NULL
    };
    char ini[1024];

    if (tx)
        return true;
    if (sbitx_drm_tx_ini(cfg, ini, sizeof(ini)) < 0)
        return false;
    tx = dream_proc_start(dream_path, dir, ini, args, DRM_RING);
    if (!tx)
        HLOGE(LOG_C, "cannot start Dream's transmitter (%s)", dream_path);
    tx_phase = 0.0;
    tx_primed = false;
    return tx != NULL;
}

void sbitx_drm_tx_stop(void)
{
    if (!tx)
        return;
    dream_proc_stop(tx);
    tx = NULL;
}

bool sbitx_drm_tx_running(void)
{
    return tx && dream_proc_running(tx);
}

void sbitx_drm_tx_audio(const float *in, size_t n)
{
    int16_t s[2048];

    while (tx && n) {
        size_t k = n < sizeof(s) / sizeof(s[0]) ? n : sizeof(s) / sizeof(s[0]);

        for (size_t j = 0; j < k; j++)
            s[j] = to_s16(in[j]);
        dream_proc_write(tx, s, k);
        in += k;
        n -= k;
    }
}

void sbitx_drm_mix(float *i, float *q, size_t n, double hz, double rate, double *phase)
{
    const double step = 2.0 * M_PI * hz / rate;
    double ph = *phase;

    for (size_t j = 0; j < n; j++) {
        double c = cos(ph), s = sin(ph);
        double re = i[j], im = q[j];

        /* (re + j im) * e^(-j ph) */
        i[j] = (float) (re * c + im * s);
        q[j] = (float) (im * c - re * s);
        ph += step;
        if (ph > M_PI)
            ph -= 2.0 * M_PI;
    }
    *phase = ph;
}

size_t sbitx_drm_tx_iq(float *i, float *q, size_t n)
{
    int16_t s[2048];
    size_t got = 0;

    if (!tx)
        return 0;
    if (!tx_primed) {
        if (dream_proc_readable(tx) < 2 * DRM_PRIME)
            return 0;
        tx_primed = true;
    }
    /* whole I/Q pairs only */
    if (dream_proc_readable(tx) < 2)
        tx_primed = false;              /* ran dry: queue again */
    while (got < n && dream_proc_readable(tx) >= 2) {
        size_t want = n - got < sizeof(s) / sizeof(s[0]) / 2 ? n - got : sizeof(s) / sizeof(s[0]) / 2;
        size_t avail = dream_proc_readable(tx) / 2;
        size_t k;

        if (want > avail)
            want = avail;
        k = dream_proc_read(tx, s, 2 * want) / 2;
        for (size_t j = 0; j < k; j++) {
            i[got + j] = (float) s[2 * j] / 32768.0f;
            q[got + j] = (float) s[2 * j + 1] / 32768.0f;
        }
        got += k;
        if (!k)
            break;
    }
    sbitx_drm_mix(i, q, got, SBITX_DRM_TX_IF, SBITX_DRM_RATE, &tx_phase);
    return got;
}
