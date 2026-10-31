/* sBitx CW (Morse code) modem
 *
 * TX: a DDS tone generator with raised-cosine envelope, at cw_pitch.
 * RX: a skimmer: every keyed carrier in the passband is decoded on a
 * channel of its own, at its own speed, and given out a line at a time.
 *
 * Copyright (C) 2024-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SBITX_CW_H_
#define SBITX_CW_H_

#include <stdbool.h>
#include <stdint.h>

/* The TX speed and pitch; wpm is also each RX channel's first guess. */
bool sbitx_cw_init(int wpm, int pitch);
void sbitx_cw_shutdown(void);

int  sbitx_cw_encode(const char *message, float *signal, int max_n,
                     int wpm, int pitch);

void sbitx_cw_set_wpm(int wpm);
void sbitx_cw_set_pitch(int pitch);

/* A decoded line: ended by a 1.5 s pause, or at a word space after ~40
 * characters. */
typedef struct {
    const char *text;
    float       pitch_hz;   /* the station's audio frequency */
    int         wpm;        /* its speed */
    float       snr_db;     /* in 2500 Hz, approximate */
} sbitx_cw_rx_line;

typedef void (*sbitx_cw_rx_cb)(const sbitx_cw_rx_line *line, void *user);

/* Decode 12 kHz audio, any amount at a time; cb gets each line. */
void sbitx_cw_rx_feed(const float *audio_12k, int n, sbitx_cw_rx_cb cb, void *user);

/* Give out every channel's text so far (the end of a recording). */
void sbitx_cw_rx_flush(sbitx_cw_rx_cb cb, void *user);

/* Forget every channel. */
void sbitx_cw_rx_reset(void);

#endif /* SBITX_CW_H_ */
