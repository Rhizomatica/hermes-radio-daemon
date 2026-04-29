/*
 * RADE V2 receiver front-end helpers ported from rx2.py.
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rx2_frontend.h"
#include "comp_prim.h"

static float clipf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static void rx2_frontend_zero(struct rx2_frontend *fe) {
    fe->sym_len = 0;
    fe->max_nin = 0;
    fe->rx_buf_len = 0;
    fe->agc_en = 0;
    fe->agc_target = 0.0f;
    fe->rx_buf = NULL;
}

int rx2_frontend_init(struct rx2_frontend *fe, int sym_len, int max_nin, int agc_en) {
    if (!fe || sym_len <= 0 || max_nin <= 0 || max_nin > 3 * sym_len) {
        return -1;
    }

    rx2_frontend_zero(fe);
    fe->sym_len = sym_len;
    fe->max_nin = max_nin;
    fe->rx_buf_len = 3 * sym_len;
    fe->agc_en = agc_en != 0;
    fe->agc_target = powf(10.0f, -3.0f / 20.0f);
    fe->rx_buf = (COMP *)calloc((size_t)fe->rx_buf_len, sizeof(*fe->rx_buf));
    if (!fe->rx_buf) {
        rx2_frontend_zero(fe);
        return -1;
    }
    return 0;
}

void rx2_frontend_reset(struct rx2_frontend *fe) {
    if (!fe || !fe->rx_buf) return;
    memset(fe->rx_buf, 0, (size_t)fe->rx_buf_len * sizeof(*fe->rx_buf));
}

void rx2_frontend_destroy(struct rx2_frontend *fe) {
    if (!fe) return;
    free(fe->rx_buf);
    rx2_frontend_zero(fe);
}

void rx2_frontend_set_agc(struct rx2_frontend *fe, int agc_en) {
    if (!fe) return;
    fe->agc_en = agc_en != 0;
}

float rx2_frontend_compute_gain(const struct rx2_frontend *fe, const COMP rx_in[], int nin) {
    if (!fe || !rx_in || nin <= 0 || nin > fe->max_nin) {
        return 1.0f;
    }
    if (!fe->agc_en) {
        return 1.0f;
    }

    double sumsq = 0.0;
    for (int i = 0; i < nin; i++) {
        sumsq += (double)rx_in[i].real * rx_in[i].real
               + (double)rx_in[i].imag * rx_in[i].imag;
    }
    float rms = sqrtf((float)(sumsq / (double)nin));
    float gain = fe->agc_target / (rms + 1e-6f);
    return clipf(gain, 0.1f, 10.0f);
}

int rx2_frontend_update_rx_buf(struct rx2_frontend *fe, const COMP rx_in[], int nin, float gain) {
    if (!fe || !fe->rx_buf || !rx_in || nin < 0 || nin > fe->max_nin) {
        return -1;
    }
    if (nin == 0) {
        return 0;
    }

    memmove(fe->rx_buf, fe->rx_buf + nin,
            (size_t)(fe->rx_buf_len - nin) * sizeof(*fe->rx_buf));
    for (int i = 0; i < nin; i++) {
        fe->rx_buf[fe->rx_buf_len - nin + i] = fcmult(gain, rx_in[i]);
    }
    return 0;
}

int rx2_frontend_apply(struct rx2_frontend *fe, const COMP rx_in[], int nin, float *gain_out) {
    if (!fe || !gain_out) {
        return -1;
    }
    float gain = rx2_frontend_compute_gain(fe, rx_in, nin);
    if (rx2_frontend_update_rx_buf(fe, rx_in, nin, gain) != 0) {
        return -1;
    }
    *gain_out = gain;
    return 0;
}
