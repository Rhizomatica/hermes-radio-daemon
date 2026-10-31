/* sBitx FT8 modem
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SBITX_FT8_H_
#define SBITX_FT8_H_

#include <stdbool.h>
#include <stdint.h>

bool sbitx_ft8_init(void);
void sbitx_ft8_shutdown(void);

/* The audio offset both backends transmit FT8 at, and log for it. */
#define FT8_TX_TONE_HZ 1500

int  sbitx_ft8_encode(const char *message, float *signal, int max_samples, float tone_freq);
/* The text is a valid FT8 message (standard, or free text of at most 13
 * characters of A-Z 0-9 space + - . / ?). */
bool sbitx_ft8_can_encode(const char *message);
int  sbitx_ft8_decode(float *audio_12k, int nsamples, char *decoded, int max_decoded_len);

/* One decoded FT8 message. `to` is the addressee (a callsign, or a token
 * such as CQ), `from` the sender; either may be empty, as may `grid`. */
typedef struct {
    char  text[48];
    char  from[16];
    char  to[16];
    char  grid[8];
    float freq_hz;   /* audio offset */
    float time_s;    /* start within the decoded window */
    float snr_db;    /* approximate, WSJT-X convention; valid if has_snr */
    bool  has_snr;
    int   pass;       /* which pass decoded it: 0 ft8_lib's, else an FT8_JS8_* (ft8_js8.h) */
    int   receptions; /* receptions combined to decode it (1 unless FT8_JS8_COMBINE) */
} sbitx_ft8_decode_result;

int  sbitx_ft8_decode_ex(float *audio_12k, int nsamples, sbitx_ft8_decode_result *out, int max_out);

/* The same, for a window starting at start_s seconds (Unix time, or any
 * steady clock): with it, a weak message is also decoded from its earlier
 * receptions added together (FT8_JS8_COMBINE). sbitx_ft8_decode_ex()
 * passes -1, no combining. */
int  sbitx_ft8_decode_at(float *audio_12k, int nsamples, double start_s,
                         sbitx_ft8_decode_result *out, int max_out);

/* Log a window's decodes to the digi log: one entry per message, timed
 * from `window_start` (unix seconds of the window's first sample), dt
 * against the UTC 15 s slot, and a message logged only once per
 * transmission across the overlapping windows. */
void sbitx_ft8_log_decodes(const sbitx_ft8_decode_result *res, int n,
                           double window_start, uint64_t dial_hz);


#endif /* SBITX_FT8_H_ */
