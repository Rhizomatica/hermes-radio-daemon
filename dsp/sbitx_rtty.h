/* sBitx RTTY (Radio Teletype) modem
 *
 * TX: Baudot encoding + FSK tone generator (DDS-based), at rtty_mark.
 * RX: a skimmer: every FSK signal of the given shift in the passband is
 * decoded on a channel of its own, in whichever polarity it frames, and
 * given out a line at a time.
 *
 * Copyright (C) 2024-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SBITX_RTTY_H_
#define SBITX_RTTY_H_

#include <stdbool.h>
#include <stdint.h>

bool sbitx_rtty_init(int baud, int mark, int shift);
void sbitx_rtty_shutdown(void);

int  sbitx_rtty_encode(const char *message, float *signal, int max_n,
                       int baud, int mark, int shift);

/* A decoded line: ended by CR/LF, 2 s without a character, or ~70
 * characters. */
typedef struct {
    const char *text;
    float       mark_hz;    /* the station's mark tone */
    float       shift_hz;
    bool        reversed;   /* mark is the lower tone */
} sbitx_rtty_rx_line;

typedef void (*sbitx_rtty_rx_cb)(const sbitx_rtty_rx_line *line, void *user);

/* Decode 12 kHz audio, any amount at a time; cb gets each line. */
void sbitx_rtty_rx_feed(const float *audio_12k, int n, int baud, int shift,
                        sbitx_rtty_rx_cb cb, void *user);

/* Give out every channel's text so far (the end of a recording). */
void sbitx_rtty_rx_flush(sbitx_rtty_rx_cb cb, void *user);

/* Forget every channel. */
void sbitx_rtty_rx_reset(void);

#endif /* SBITX_RTTY_H_ */
