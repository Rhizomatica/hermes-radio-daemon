/* capture_levels - see capture_levels.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "capture_levels.h"

/* Full scale of an S32 sample. */
#define CAPTURE_FULL_SCALE 2147483648.0

void capture_levels_reset(capture_levels *c)
{
    memset(c, 0, sizeof(*c));
}

void capture_levels_add(capture_levels *c, const int32_t *frames, size_t n_frames)
{
    for (size_t i = 0; i < n_frames; i++)
    {
        for (int ch = 0; ch < CAPTURE_LEVELS_CHANNELS; ch++)
        {
            int32_t v = frames[i * CAPTURE_LEVELS_CHANNELS + ch];
            double x = (double) v;
            uint32_t a = v < 0 ? (uint32_t) (-(int64_t) v) : (uint32_t) v;
            c->sum_sq[ch] += x * x;
            /* The first frame has no previous one: no difference counted. */
            if (c->frames || i)
            {
                double d = x - (double) c->prev[ch];
                c->diff_sq[ch] += d * d;
            }
            if (a > c->peak[ch])
                c->peak[ch] = a;
            c->prev[ch] = v;
        }
    }
    c->frames += n_frames;
}

void capture_levels_get(const capture_levels *c, int channel, capture_channel_level *out)
{
    out->rms_dbfs = -INFINITY;
    out->peak_dbfs = -INFINITY;
    out->hf_ratio = 0.0;
    if (channel < 0 || channel >= CAPTURE_LEVELS_CHANNELS || c->frames == 0)
        return;
    double ms = c->sum_sq[channel] / (double) c->frames;
    if (ms <= 0.0)
        return;
    out->rms_dbfs = 10.0 * log10(ms / (CAPTURE_FULL_SCALE * CAPTURE_FULL_SCALE));
    out->peak_dbfs = 20.0 * log10((double) c->peak[channel] / CAPTURE_FULL_SCALE);
    if (c->frames > 1)
        out->hf_ratio = sqrt((c->diff_sq[channel] / (double) (c->frames - 1)) / ms);
}
