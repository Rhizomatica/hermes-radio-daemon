/* mic_filter - high-pass on the sBitx microphone input. See mic_filter.h. */

#include <math.h>
#include <string.h>

#include "mic_filter.h"

/* RBJ audio-EQ-cookbook high-pass section. */
static void design_hpf(mic_biquad *s, double fc, double fs, double q)
{
    double w = 2.0 * M_PI * fc / fs, c = cos(w), alpha = sin(w) / (2.0 * q);
    double a0 = 1.0 + alpha;

    s->b0 = (float) ((1.0 + c) / 2.0 / a0);
    s->b1 = (float) (-(1.0 + c) / a0);
    s->b2 = s->b0;
    s->a1 = (float) (-2.0 * c / a0);
    s->a2 = (float) ((1.0 - alpha) / a0);
    s->z1 = s->z2 = 0.0f;
}

void mic_hpf_setup(mic_hpf *f, unsigned cutoff_hz, unsigned rate)
{
    memset(f, 0, sizeof(*f));
    f->cutoff_hz = cutoff_hz;
    f->rate = rate;
    if (!cutoff_hz || !rate || cutoff_hz >= rate / 2)
    {
        f->cutoff_hz = 0;
        return;
    }
    /* 4th-order Butterworth: Q of the two sections = 1/(2 cos(pi/8)), 1/(2 cos(3pi/8)) */
    design_hpf(&f->st[0], cutoff_hz, rate, 0.54119610);
    design_hpf(&f->st[1], cutoff_hz, rate, 1.30656296);
}

void mic_hpf_run_s32(mic_hpf *f, int32_t *buf, size_t n)
{
    if (!f->cutoff_hz)
        return;

    for (size_t i = 0; i < n; i++)
    {
        float x = (float) buf[i] * (1.0f / 2147483648.0f);
        for (int k = 0; k < 2; k++)          /* transposed direct form II */
        {
            mic_biquad *s = &f->st[k];
            float y = s->b0 * x + s->z1;
            s->z1 = s->b1 * x - s->a1 * y + s->z2;
            s->z2 = s->b2 * x - s->a2 * y;
            x = y;
        }
        float v = x * 2147483648.0f;
        buf[i] = v >= 2147483520.0f ? INT32_MAX : v <= -2147483648.0f ? INT32_MIN : (int32_t) v;
    }
}
