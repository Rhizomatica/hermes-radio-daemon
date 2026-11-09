/* js8_decode - JS8 receive for the radio paths: a window of audio in,
 * the frames heard into the digital-mode log (mode "JS8").
 *
 * The windows overlap (15 s, every 2 s, as FT8's), so one transmission
 * decodes several times: it is logged once.  Each entry carries the text
 * JS8Call would show and the callsigns and grid the frame names, which is
 * what PSKReporter's spots take.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef JS8_DECODE_H
#define JS8_DECODE_H

#include <stdint.h>

#define JS8_WINDOW_SECONDS 15   /* a Normal transmission is 12.64 s */

/* Decodes nsamples of 12 kHz audio whose first sample was at
 * window_start (unix seconds), heard at dial_hz, and logs what is new. */
void js8_decode_and_log(const float *audio_12k, int nsamples, double window_start,
                        uint64_t dial_hz);

#endif
