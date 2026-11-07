/* dstar_clock_cal — the codec's sampling-clock error, measured from the
 * D-STAR overs heard (see dstar_clock_cal.h).
 *
 * On the bench two sBitx held at 0 read their codecs at +297 and +302 ppm
 * (hand-calibrated: +300 and +301) within +-7 ppm per 2 s window; weak
 * signals on the air scatter far more (+-400 ppm), which the median and
 * the agreement test absorb.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "dstar_clock_cal.h"

static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *) a, y = *(const double *) b;
    return (x > y) - (x < y);
}

static double median_of(double *v, int n)
{
    qsort(v, (size_t) n, sizeof(double), cmp_double);
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

void dstar_clock_cal_reset(dstar_clock_cal *c)
{
    memset(c, 0, sizeof(*c));
}

void dstar_clock_cal_push(dstar_clock_cal *c, double correction_ppm, double residual_ppm)
{
    c->ppm[c->pos] = correction_ppm + residual_ppm;
    c->pos = (c->pos + 1) % DSTAR_CAL_KEEP;
    if (c->n < DSTAR_CAL_KEEP)
        c->n++;
    c->fresh++;
}

dstar_cal_result dstar_clock_cal_decide(dstar_clock_cal *c, double correction_ppm,
                                        long *out, double *median, double *spread)
{
    if (c->fresh == 0 || c->n < DSTAR_CAL_MIN)
        return DSTAR_CAL_HOLD;
    c->fresh = 0;
    double v[DSTAR_CAL_KEEP], dev[DSTAR_CAL_KEEP];
    memcpy(v, c->ppm, (size_t) c->n * sizeof(double));
    const double m = median_of(v, c->n);
    for (int i = 0; i < c->n; i++)
        dev[i] = fabs(c->ppm[i] - m);
    const double mad = median_of(dev, c->n);
    *median = m;
    *spread = mad;
    if (mad > DSTAR_CAL_MAD_MAX)
        return DSTAR_CAL_DISAGREE;
    if (fabs(m - correction_ppm) < DSTAR_CAL_DEADBAND)
        return DSTAR_CAL_HOLD;
    *out = lround(m);
    return DSTAR_CAL_MOVE;
}
