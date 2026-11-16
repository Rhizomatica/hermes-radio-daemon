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
    c->confirmed = -1;
}

/* One step's reading into the climb's state. */
static void account(txcal *c, int k)
{
    double drive = c->step_drive[k], fwd = c->step_fwd[k];
    if (fwd > c->best_fwd)
    {
        c->best_fwd = fwd;
        c->best_drive = drive;
        c->best_index = k;
        c->below = 0;
    }
    else if (fwd < c->best_fwd * pow(10.0, -TXCAL_FALL_DB / 10.0))
        c->below++;
    else
        c->below = 0;
    if (fwd > c->climb_fwd * pow(10.0, TXCAL_CLIMB_DB / 10.0) || c->climb_drive == 0.0)
    {
        c->climb_fwd = fwd;
        c->climb_drive = drive;
    }
    /* A step that added less power than it would below saturation: near
     * the peak, so look closer. Readings of zero (under the bridge's
     * threshold) say nothing about it. */
    if (!c->fine && c->last_fwd > 0.0 && fwd > 0.0 &&
        10.0 * log10(fwd / c->last_fwd) < TXCAL_FINE_GAIN_DB)
        c->fine = true;
    c->last_fwd = fwd;
}

/* The climb's state again from the readings kept, one of them corrected. */
static void replay(txcal *c)
{
    int n = c->steps < TXCAL_STEPS_MAX ? c->steps : TXCAL_STEPS_MAX;
    c->best_fwd = c->best_drive = c->last_fwd = c->climb_drive = c->climb_fwd = 0.0;
    c->best_index = c->below = 0;
    c->fine = false;
    for (int k = 0; k < n; k++)
        account(c, k);
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
    if (c->confirming)
    {
        c->confirming = false;
        if (fwd >= c->best_fwd * pow(10.0, -TXCAL_FALL_DB / 10.0))
            c->confirmed = c->best_index;
        else
        {
            c->step_fwd[c->best_index] = fwd;    /* it read high */
            replay(c);
        }
    }
    else
    {
        if (c->steps < TXCAL_STEPS_MAX)
        {
            c->step_drive[c->steps] = c->drive;
            c->step_fwd[c->steps] = fwd;
            account(c, c->steps);
        }
        c->steps++;
        c->codec_full = c->codec_full || codec_full;
    }

    int last = (c->steps < TXCAL_STEPS_MAX ? c->steps : TXCAL_STEPS_MAX) - 1;
    double next = c->step_drive[last] * drive_db(c->fine ? TXCAL_FINE_DB : TXCAL_COARSE_DB);
    bool flat = c->climb_fwd > 0.0 && next > c->climb_drive * drive_db(TXCAL_FLAT_DB);
    if (c->below >= TXCAL_FALL_STEPS || flat || c->codec_full || next > TXCAL_MAX_DRIVE ||
        c->steps >= TXCAL_STEPS_MAX)
    {
        if (!(c->best_fwd > 0.0))
            return TXCAL_NO_POWER;
        if (c->confirmed != c->best_index)
        {
            c->confirming = true;
            c->drive = c->best_drive;
            return TXCAL_NEXT;
        }
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
