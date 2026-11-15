/* sbitx_txcal - see sbitx_txcal.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sbitx_txcal.h"

#include <string.h>

/* dB of drive (amplitude) as a factor */
static double drive_db(double db)
{
    return pow(10.0, db / 20.0);
}

void txcal_start(txcal *c)
{
    memset(c, 0, sizeof(*c));
    c->drive = TXCAL_START_DRIVE;
}

/* The lowest drive within TXCAL_PEAK_MARGIN_DB of the most power. */
static void settle_on_peak(txcal *c)
{
    int n = c->steps < TXCAL_STEPS_MAX ? c->steps : TXCAL_STEPS_MAX;
    double floor = c->best_fwd * pow(10.0, -TXCAL_PEAK_MARGIN_DB / 10.0);
    for (int k = 0; k < n; k++)
        if (c->step_fwd[k] >= floor)
        {
            c->best_drive = c->step_drive[k];
            c->best_fwd = c->step_fwd[k];
            return;
        }
}

txcal_verdict txcal_step(txcal *c, double fwd, bool codec_full)
{
    if (c->steps < TXCAL_STEPS_MAX)
    {
        c->step_drive[c->steps] = c->drive;
        c->step_fwd[c->steps] = fwd;
    }
    c->steps++;
    if (fwd > c->best_fwd)
    {
        c->best_fwd = fwd;
        c->best_drive = c->drive;
        c->below = 0;
    }
    else if (fwd < c->best_fwd * pow(10.0, -TXCAL_FALL_DB / 10.0))
        c->below++;
    else
        c->below = 0;
    if (fwd > c->climb_fwd * pow(10.0, TXCAL_CLIMB_DB / 10.0) || c->climb_drive == 0.0)
    {
        c->climb_fwd = fwd;
        c->climb_drive = c->drive;
    }

    /* A step that added less power than it would below saturation: near
     * the peak, so look closer. Readings of zero (under the bridge's
     * threshold) say nothing about it. */
    if (!c->fine && c->last_fwd > 0.0 && fwd > 0.0 &&
        10.0 * log10(fwd / c->last_fwd) < TXCAL_FINE_GAIN_DB)
        c->fine = true;
    c->last_fwd = fwd;

    double next = c->drive * drive_db(c->fine ? TXCAL_FINE_DB : TXCAL_COARSE_DB);
    bool flat = c->climb_fwd > 0.0 && next > c->climb_drive * drive_db(TXCAL_FLAT_DB);
    if (c->below >= TXCAL_FALL_STEPS || flat || codec_full || next > TXCAL_MAX_DRIVE ||
        c->steps >= TXCAL_STEPS_MAX)
    {
        if (!(c->best_fwd > 0.0))
            return TXCAL_NO_POWER;
        settle_on_peak(c);
        return TXCAL_FOUND;
    }
    c->drive = next;
    return TXCAL_NEXT;
}

int tx_band_index(const power_settings *bands, uint32_t n, uint32_t hz)
{
    for (uint32_t k = 0; k < n; k++)
        if ((int64_t) hz >= bands[k].f_start && (int64_t) hz < bands[k].f_stop)
            return (int) k;
    return -1;
}
