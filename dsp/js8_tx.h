/* js8_tx - a JS8 frame's audio, in its period, as JS8Call sends it.
 *
 * JS8Call's receivers decode the UTC periods of a submode (Normal: every
 * 15 s from the minute), and JS8Call starts each frame a little into its
 * period (Normal 0.5 s, Fast 0.2 s, Turbo 0.1 s, Slow 0.5 s).  A message
 * of several frames goes out one frame a period.  The tones are plain
 * continuous-phase FSK (JS8Call's Modulator), with a short ramp at each
 * end so the keying does not click.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef JS8_TX_H
#define JS8_TX_H

#include "js8_msg.h"
#include "js8_rx.h"

#define JS8_TX_TONE_HZ   1500.0f   /* tone 0's audio frequency, as FT8's here */
#define JS8_TX_MAX_FRAMES 16       /* a message: 4 minutes in Normal */
#define JS8_PTT_LEAD_S   0.3       /* key this long before a frame's slot, not sooner */
#define JS8_TX_GAP_MAX_S 31        /* silence before a frame: up to a Slow period */
#define JS8_SLOW_TX_S    26        /* a Slow frame: 79 x 0.32 s = 25.3 s */

/* The submode to send in: the first of mask (js8_worker's) in JS8Call's
 * order of preference, Normal first. */
js8_submode js8_tx_submode(unsigned mask);

/* When the next frame of submode sub may start, at or after `now` (unix
 * seconds): the next period's start plus JS8Call's start delay. */
double js8_tx_next_start(js8_submode sub, double now);

/* The 12 kHz audio of one frame, tone 0 at tone_hz: 79 symbols of the
 * submode, amplitude 1 less the ramps.  Returns the samples written, 0 if
 * it does not fit in cap or does not encode. */
int js8_tx_frame_audio(const js8_tx_frame *f, js8_submode sub, float tone_hz, float *out, int cap);

#endif
