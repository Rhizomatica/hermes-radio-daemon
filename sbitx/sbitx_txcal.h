/* sbitx_txcal - the sBitx's TX power: what the power level drives, and the
 * calibration that finds, per band, the drive at which the PA gives its
 * most power.
 *
 * The TX output is the signal times a drive. A band's scale (tx_bandN) is
 * the drive at which this radio's PA peaks there: past it the output only
 * falls (measured on two sBitx at 7 MHz, it sank about 15 % over 9 dB more
 * drive). The power level is a share of that power, so its drive is the
 * scale times the square root of the level.
 *
 * The calibration keys a full-scale tone for a second at a time, raising
 * the drive in steps, reads the forward power over the second half of each
 * step, and stops once two steps in a row read clearly under the best, or
 * 3 dB more drive added nothing. The drive of the best step is the band's
 * scale. Only where the readings peak
 * matters, so the bridge's error and its weak low end do not.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SBITX_TXCAL_H_
#define SBITX_TXCAL_H_

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "radio.h"

/* The first drive tried: under any sBitx's peak (the two measured peaked
 * at 0.3 and 0.45 at 7 MHz), so the climb to it is seen. */
#define TXCAL_START_DRIVE   0.05
/* No drive past this: the highest shipped scale was 4.0 (1-2 MHz), and
 * the codec's range ends not far beyond. */
#define TXCAL_MAX_DRIVE     4.0
/* The steps, in dB of drive. Coarse while each step still adds power as a
 * linear amplifier would; fine once a step adds less than TXCAL_FINE_GAIN_DB,
 * near the peak. */
#define TXCAL_COARSE_DB     1.0
#define TXCAL_FINE_DB       0.5
#define TXCAL_FINE_GAIN_DB  0.5
/* Past the peak: TXCAL_FALL_STEPS steps in a row more than TXCAL_FALL_DB
 * under the best, so one low reading (the bridge refreshes every ~100 ms)
 * does not end it. */
#define TXCAL_FALL_DB       0.3
#define TXCAL_FALL_STEPS    2
/* Or flat: TXCAL_FLAT_DB more drive brought no step up by TXCAL_CLIMB_DB.
 * A PA that saturates without falling would otherwise be driven on to
 * TXCAL_MAX_DRIVE. */
#define TXCAL_CLIMB_DB      0.1
#define TXCAL_FLAT_DB       3.0

typedef struct {
    double drive;        /* the step being measured */
    double best_drive;
    double best_fwd;
    double last_fwd;
    double climb_drive;  /* the last step up by TXCAL_CLIMB_DB on the one before */
    double climb_fwd;
    int    below;        /* steps in a row under the best */
    int    steps;
    bool   fine;
} txcal;

typedef enum {
    TXCAL_NEXT,          /* measure c->drive next */
    TXCAL_FOUND,         /* c->best_drive is the peak */
    TXCAL_NO_POWER,      /* nothing read at any drive */
} txcal_verdict;

void txcal_start(txcal *c);

/* The forward power read at c->drive (any unit proportional to watts), and
 * whether the codec's range was reached there (no cleaner drive above). */
txcal_verdict txcal_step(txcal *c, double fwd, bool codec_full);

/* The tx_band row hz falls in, -1 for none. */
int tx_band_index(const power_settings *bands, uint32_t n, uint32_t hz);

/* The drive for a power level: a share of the band's peak power, so the
 * scale times the level's square root. */
static inline double tx_power_drive(double scale, uint32_t level_percent)
{
    if (level_percent > 100)
        level_percent = 100;
    return scale * sqrt(level_percent / 100.0);
}

/* A calibration's state, as the API and the websocket name it. */
static inline const char *tx_cal_state_name(int state)
{
    switch (state)
    {
    case TX_CAL_RUNNING: return "running";
    case TX_CAL_DONE:    return "done";
    case TX_CAL_FAILED:  return "failed";
    default:             return "idle";
    }
}

#endif /* SBITX_TXCAL_H_ */
