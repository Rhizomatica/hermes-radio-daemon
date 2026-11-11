/* js8_rx - finds and decodes JS8 frames in 12 kHz audio.
 *
 * Every JS8 submode has FT8's frame shape (79 symbols, Costas arrays at
 * 0, 36 and 72); Normal also has its symbol (1920 samples at 12 kHz,
 * 0.16 s, 6.25 Hz tones), the others longer or shorter ones.  ft8_lib's
 * waterfall serves, set to the submode's symbol.  What differs from FT8
 * is here: the Costas pattern the sync search looks for (Normal keeps
 * FT8's first set, the others a modified one), tones read as plain 3-bit
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

/* JS8Call's submodes (JS8_Include/commons.h): samples a symbol and the
 * period, in seconds. */
typedef enum { JS8_SUB_NORMAL, JS8_SUB_FAST, JS8_SUB_TURBO, JS8_SUB_SLOW, JS8_SUBMODES } js8_submode;

/* Their names, in that order (the settings take them, any case). */
#define JS8_SUBMODE_NAMES { "NORMAL", "FAST", "TURBO", "SLOW" }

typedef struct {
    const char     *name;
    int             symbol;      /* samples at 12 kHz */
    int             period_s;
    js8_costas_type costas;
} js8_submode_params;

const js8_submode_params *js8_submode_get(js8_submode sub);

typedef struct {
    char  frame[JS8_CHARS + 1];   /* the frame's twelve characters */
    int   type;                   /* its 3-bit frame type */
    float freq_hz;                /* audio offset of its lowest tone */
    float time_s;                 /* start within the window */
    float snr_db;                 /* approximate, WSJT-X convention; valid if has_snr */
    bool  has_snr;
    int   hard_errors;            /* channel bits the codeword corrected */
} js8_rx_result;

/* Every distinct frame of submode sub in nsamples of 12 kHz mono audio
 * (about one of its periods, or more).  Returns how many, at most max_out. */
int js8_rx_decode_sub(js8_submode sub, const float *audio_12k, int nsamples,
                      js8_rx_result *out, int max_out);

/* The same, for Normal. */
int js8_rx_decode(const float *audio_12k, int nsamples, js8_rx_result *out, int max_out);

#endif
