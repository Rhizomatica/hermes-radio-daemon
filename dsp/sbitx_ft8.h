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

int  sbitx_ft8_encode(const char *message, float *signal, int max_samples, float tone_freq);
/* The text is a valid FT8 message (standard, or free text of at most 13
 * characters of A-Z 0-9 space + - . / ?). */
bool sbitx_ft8_can_encode(const char *message);
int  sbitx_ft8_decode(float *audio_12k, int nsamples, char *decoded, int max_decoded_len);

int  sbitx_ft8_spool_count(void);
int  sbitx_ft8_spool_read(int index, char *text, int max_len);
void sbitx_ft8_spool_add(const char *mode, const char *dir, int freq_khz, const char *text);

extern const char *digi_spool_dir;

#endif /* SBITX_FT8_H_ */
