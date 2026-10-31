/* ft8_js8 - FT8 decoding passes after ft8_lib's own, from JS8Call
 *
 * When ft8_lib's decode of a candidate fails, these try again with the same
 * candidate, as JS8Call-improved's JS8 decoder does (JS8 is FT8's
 * waveform):
 *
 *   FT8_JS8_FEEDBACK  LDPC feedback: boost the bits the failed decode is
 *                     sure of, shrink the rest, and decode again
 *                     (JS8_Mode/ldpc_feedback.h)
 *   FT8_JS8_WHITEN    bit probabilities weighted by the noise of each tone
 *                     and each symbol, from log amplitudes
 *                     (JS8_Mode/whitening_processor.h)
 *   FT8_JS8_LINEAR    the same from linear amplitudes, WSJT-X's other metric
 *   FT8_JS8_COMBINE   add the bit probabilities of earlier receptions of the
 *                     same message (same frequency, same time in the slot,
 *                     at least 14 s apart, and correlated) and decode the
 *                     sum, so a station repeating a message too weak to
 *                     decode once is decoded on its 2nd or 3rd try
 *                     (JS8_Mode/soft_combiner.h, matching by correlation
 *                     rather than its 32-bit signature)
 *
 * On 566 s of 40 m audio, radiod's 2 s sliding windows decoded 141
 * messages with ft8_lib alone and 158 with every pass (+12 %), combining
 * the most of it, for about 3.5x the decode time (80 ms a window on an
 * x86 PC or an aarch64 phone). tests/dsp/ft8_bench.c measures it.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FT8_JS8_H
#define FT8_JS8_H

#include <stdbool.h>

#include "ft8/decode.h"

enum {
    FT8_JS8_FEEDBACK = 1,
    FT8_JS8_WHITEN   = 2,
    FT8_JS8_LINEAR   = 4,
    FT8_JS8_COMBINE  = 8,
    FT8_JS8_ALL      = 15,
};

/* The passes in use (FT8_JS8_ALL). */
extern unsigned ft8_js8_passes;

/* LDPC feedback's rounds per decode (2). */
extern int ft8_js8_feedback_rounds;

/* Which pass decoded a message: 0 ft8_lib's own, else the FT8_JS8_* bit. */
typedef struct {
    int pass;
    int receptions;     /* combined receptions, with FT8_JS8_COMBINE */
} ft8_js8_how;

/* Decode one candidate: ft8_lib's decode, then the passes in use.
 * start_s is when the candidate's signal starts, in seconds of any steady
 * clock (radiod: Unix time), and freq_hz its audio frequency; combining
 * needs both (start_s < 0: no combining). */
bool ft8_js8_decode(const ftx_waterfall_t *wf, const ftx_candidate_t *cand, int max_iterations,
                    double start_s, float freq_hz, ftx_message_t *message,
                    ftx_decode_status_t *status, ft8_js8_how *how);

/* Forget every stored reception (FT8_JS8_COMBINE). */
void ft8_js8_reset(void);

#endif /* FT8_JS8_H */
