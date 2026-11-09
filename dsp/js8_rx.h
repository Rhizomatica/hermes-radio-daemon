/* js8_rx - finds and decodes JS8 Normal frames in 12 kHz audio.
 *
 * JS8 Normal has FT8's symbol (1920 samples at 12 kHz, 0.16 s, 6.25 Hz
 * tones) and its frame's shape (79 symbols, Costas arrays at 0, 36 and
 * 72), so ft8_lib's waterfall serves as it is.  What differs is here: the
 * Costas pattern the sync search looks for, tones read as plain 3-bit
 * words (no Gray map), and JS8's own (174,87) code (dsp/js8_frame.c).
 * The frames found are the raw twelve characters and type; the message
 * layer (what a frame means) is above this.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef JS8_RX_H
#define JS8_RX_H

#include <stdbool.h>

#include "js8_frame.h"

#define JS8_SAMPLE_RATE      12000
#define JS8_NORMAL_SYMBOL    1920    /* samples: 0.16 s, 6.25 Hz tones */
#define JS8_NORMAL_PERIOD_S  15

typedef struct {
    char  frame[JS8_CHARS + 1];   /* the frame's twelve characters */
    int   type;                   /* its 3-bit frame type */
    float freq_hz;                /* audio offset of its lowest tone */
    float time_s;                 /* start within the window */
    float snr_db;                 /* approximate, WSJT-X convention; valid if has_snr */
    bool  has_snr;
    int   hard_errors;            /* channel bits the codeword corrected */
} js8_rx_result;

/* Every distinct JS8 Normal frame in nsamples of 12 kHz mono audio (about
 * one 15 s period, or more).  Returns how many, at most max_out. */
int js8_rx_decode(const float *audio_12k, int nsamples, js8_rx_result *out, int max_out);

#endif
