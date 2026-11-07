/* dstar_clock_cal — the codec's sampling-clock error, measured from the
 * D-STAR overs heard.
 *
 * D-STAR's 4800 baud needs the receiver's (and transmitter's) sampling
 * clock right to a few tens of ppm; an sBitx codec runs ~+300 ppm fast,
 * and each crystal differs. The error belongs to the crystal, so it is
 * measured, not tracked: baud-line readings taken while frames decode are
 * kept as absolute errors, and between overs the correction moves to their
 * median when enough of them agree.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DSTAR_CLOCK_CAL_H
#define DSTAR_CLOCK_CAL_H

#include <stdbool.h>

#define DSTAR_CAL_KEEP      32         /* readings kept (~1 min of overs) */
#define DSTAR_CAL_MIN       6          /* needed to move the correction */
#define DSTAR_CAL_MAD_MAX   25.0       /* their median absolute deviation, ppm */
#define DSTAR_CAL_DEADBAND  10.0       /* closer than this: leave it */

typedef struct {
    double ppm[DSTAR_CAL_KEEP];        /* absolute errors, a ring */
    int    n, pos, fresh;
} dstar_clock_cal;

void dstar_clock_cal_reset(dstar_clock_cal *c);

/* One baud-line reading, residual_ppm, taken with correction_ppm in force. */
void dstar_clock_cal_push(dstar_clock_cal *c, double correction_ppm, double residual_ppm);

typedef enum {
    DSTAR_CAL_HOLD,                    /* nothing new, too few, or close enough */
    DSTAR_CAL_DISAGREE,                /* the readings scatter too widely */
    DSTAR_CAL_MOVE,                    /* *out is the new correction */
} dstar_cal_result;

/* Between overs: what the correction should be, given correction_ppm now.
 * *median and *spread (median absolute deviation) are set whenever there
 * were enough readings to judge. */
dstar_cal_result dstar_clock_cal_decide(dstar_clock_cal *c, double correction_ppm,
                                        long *out, double *median, double *spread);

#endif
