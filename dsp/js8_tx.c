/* js8_tx - a JS8 frame's audio, in its period.  See js8_tx.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "js8_tx.h"

#include <math.h>
#include <stddef.h>

#define TWO_PI       6.283185307179586
#define RAMP_S       0.005           /* raised-cosine ends: no click */

/* JS8Call's start delays (JS8_Include/commons.h), in seconds. */
static const double START_DELAY_S[JS8_SUBMODES] = {
    [JS8_SUB_NORMAL] = 0.5, [JS8_SUB_FAST] = 0.2, [JS8_SUB_TURBO] = 0.1, [JS8_SUB_SLOW] = 0.5,
};

js8_submode js8_tx_submode(unsigned mask)
{
    static const js8_submode ORDER[] = { JS8_SUB_NORMAL, JS8_SUB_FAST, JS8_SUB_TURBO, JS8_SUB_SLOW };
    for (size_t i = 0; i < sizeof ORDER / sizeof ORDER[0]; i++)
        if (mask & (1u << ORDER[i]))
            return ORDER[i];
    return JS8_SUB_NORMAL;
}

double js8_tx_next_start(js8_submode sub, double now)
{
    const js8_submode_params *p = js8_submode_get(sub);
    double period = p ? p->period_s : JS8_NORMAL_PERIOD_S;
    double delay = START_DELAY_S[p ? sub : JS8_SUB_NORMAL];
    double start = floor((now - delay) / period) * period + delay;
    return start < now ? start + period : start;
}

int js8_tx_frame_audio(const js8_tx_frame *f, js8_submode sub, float tone_hz, float *out, int cap)
{
    const js8_submode_params *p = js8_submode_get(sub);
    if (!f || !p || !out)
        return 0;
    int tones[JS8_NN];
    if (!js8_encode(f->frame, f->type, p->costas, tones))
        return 0;
    int n = JS8_NN * p->symbol;
    if (n > cap)
        return 0;
    double spacing = (double) JS8_SAMPLE_RATE / p->symbol, phase = 0;
    int ramp = (int) (RAMP_S * JS8_SAMPLE_RATE);
    for (int i = 0; i < n; i++) {
        phase += TWO_PI * (tone_hz + tones[i / p->symbol] * spacing) / JS8_SAMPLE_RATE;
        if (phase > TWO_PI)
            phase -= TWO_PI;
        double gain = 1.0;
        if (i < ramp)
            gain = 0.5 - 0.5 * cos(M_PI * i / ramp);
        else if (i >= n - ramp)
            gain = 0.5 - 0.5 * cos(M_PI * (n - 1 - i) / ramp);
        out[i] = (float) (gain * sin(phase));
    }
    return n;
}
