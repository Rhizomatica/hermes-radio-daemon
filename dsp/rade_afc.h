/* rade_afc — coarse frequency correction in front of the RADE V2 decoder.
 *
 * RADE V2 measures its frequency offset from the cyclic prefix, which is
 * only unambiguous within +-Fs/2M = +-31.25 Hz: two stations further apart
 * than that (an IC-7100 and an sBitx were 41 Hz apart on the air) lock one
 * carrier (62.5 Hz) off and play noise. This finds the over's centre in an
 * averaged spectrum (RADE's carriers, and the encrypted overs' aux carriers,
 * are symmetric about 1500 Hz), shifts the decoder's input by it, and says
 * when a decoder that locked on the wrong carrier must start again.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef RADE_AFC_H
#define RADE_AFC_H

#include <stdbool.h>

#include "rade_api.h"

/* How far off a station may be and still be corrected, either way (Hz). */
#define RADE_AFC_RANGE_HZ 200.0f

typedef struct rade_afc rade_afc;

rade_afc *rade_afc_new(void);
void rade_afc_free(rade_afc *a);
/* Forget the spectrum and the correction (a new receiver). */
void rade_afc_reset(rade_afc *a);

/* Take n samples of the decoder's input (8 kHz complex) into the spectrum
 * and shift them, in place, by the current correction. */
void rade_afc_process(rade_afc *a, RADE_COMP *x, int n);

/* After each rade_rx(): synced and rade_foff are rade_sync() and
 * rade_freq_offset(). Moves the correction while the decoder searches;
 * returns true when the decoder is locked on the wrong carrier and must be
 * reset (the correction has moved already). */
bool rade_afc_update(rade_afc *a, bool synced, float rade_foff);

/* The correction now applied (Hz). */
float rade_afc_shift(const rade_afc *a);
/* The spectrum's own estimate of the over's offset (Hz) into *hz; false,
 * *hz untouched, while it sees no over. */
bool rade_afc_estimate(const rade_afc *a, float *hz);

#endif
