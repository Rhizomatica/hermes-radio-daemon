/*
 * Stateful complex band-pass filter ported from radae/dsp.py.
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "complex_bpf.h"
#include "comp_prim.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float sinc_pi(float x) {
    if (fabsf(x) < 1e-8f) {
        return 1.0f;
    }
    float pix = (float)M_PI * x;
    return sinf(pix) / pix;
}

static void complex_bpf_zero(struct complex_bpf *bpf) {
    bpf->ntap = 0;
    bpf->max_len = 0;
    bpf->mem_len = 0;
    bpf->alpha = 0.0f;
    bpf->phase = comp0();
    bpf->h = NULL;
    bpf->mem = NULL;
    bpf->x_mem = NULL;
    bpf->x_filt = NULL;
    bpf->phase_vec_exp = NULL;
    bpf->phase_vec = NULL;
}

void complex_bpf_destroy(struct complex_bpf *bpf) {
    if (!bpf) return;
    free(bpf->h);
    free(bpf->mem);
    free(bpf->x_mem);
    free(bpf->x_filt);
    free(bpf->phase_vec_exp);
    free(bpf->phase_vec);
    complex_bpf_zero(bpf);
}

void complex_bpf_reset(struct complex_bpf *bpf) {
    if (!bpf) return;
    if (bpf->mem && bpf->ntap > 1) {
        memset(bpf->mem, 0, (size_t)(bpf->ntap - 1) * sizeof(*bpf->mem));
    }
    bpf->mem_len = bpf->ntap > 0 ? bpf->ntap - 1 : 0;
    bpf->phase.real = 1.0f;
    bpf->phase.imag = 0.0f;
}

int complex_bpf_init(struct complex_bpf *bpf, int ntap, float Fs_Hz,
                     float bandwidth_Hz, float centre_freq_Hz, int max_len) {
    if (!bpf || ntap <= 0 || max_len <= 0 || Fs_Hz <= 0.0f) {
        return -1;
    }

    complex_bpf_zero(bpf);

    bpf->ntap = ntap;
    bpf->max_len = max_len;
    bpf->alpha = 2.0f * (float)M_PI * centre_freq_Hz / Fs_Hz;

    size_t mem_cap = (size_t)(ntap > 1 ? ntap - 1 : 1);
    bpf->h = (COMP *)calloc((size_t)ntap, sizeof(*bpf->h));
    bpf->mem = (COMP *)calloc(mem_cap, sizeof(*bpf->mem));
    bpf->x_mem = (COMP *)calloc((size_t)(ntap + max_len - 1), sizeof(*bpf->x_mem));
    bpf->x_filt = (COMP *)calloc((size_t)max_len, sizeof(*bpf->x_filt));
    bpf->phase_vec_exp = (COMP *)calloc((size_t)max_len, sizeof(*bpf->phase_vec_exp));
    bpf->phase_vec = (COMP *)calloc((size_t)max_len, sizeof(*bpf->phase_vec));
    if (!bpf->h || !bpf->mem || !bpf->x_mem || !bpf->x_filt ||
        !bpf->phase_vec_exp || !bpf->phase_vec) {
        complex_bpf_destroy(bpf);
        return -1;
    }

    float B = bandwidth_Hz / Fs_Hz;
    for (int i = 0; i < ntap; i++) {
        float n = (float)i - (float)(ntap - 1) / 2.0f;
        bpf->h[i].real = B * sinc_pi(n * B);
        bpf->h[i].imag = 0.0f;
    }
    for (int i = 0; i < max_len; i++) {
        float phi = -bpf->alpha * (float)(i + 1);
        bpf->phase_vec_exp[i] = comp_exp_j(phi);
    }

    complex_bpf_reset(bpf);
    return 0;
}

int complex_bpf_process(struct complex_bpf *bpf, const COMP x[], int n, COMP y[]) {
    if (!bpf || !x || !y || n < 0 || n > bpf->max_len) {
        return -1;
    }
    if (n == 0) {
        return 0;
    }

    for (int i = 0; i < bpf->mem_len; i++) {
        bpf->x_mem[i] = bpf->mem[i];
    }
    for (int i = 0; i < n; i++) {
        bpf->phase_vec[i] = cmult(bpf->phase, bpf->phase_vec_exp[i]);
        bpf->x_mem[bpf->mem_len + i] = cmult(x[i], bpf->phase_vec[i]);
    }

    for (int i = 0; i < n; i++) {
        COMP acc = comp0();
        for (int j = 0; j < bpf->ntap; j++) {
            acc = cadd(acc, cmult(bpf->x_mem[i + j], bpf->h[j]));
        }
        bpf->x_filt[i] = acc;
    }

    int valid_len = bpf->mem_len + n;
    int target_mem_len = bpf->ntap > 1 ? bpf->ntap - 1 : 0;
    int new_mem_len = valid_len < target_mem_len ? valid_len : target_mem_len;
    if (new_mem_len > 0) {
        memcpy(bpf->mem, &bpf->x_mem[valid_len - new_mem_len],
               (size_t)new_mem_len * sizeof(*bpf->mem));
    }
    bpf->mem_len = new_mem_len;
    bpf->phase = bpf->phase_vec[n - 1];

    for (int i = 0; i < n; i++) {
        y[i] = cmult(bpf->x_filt[i], cconj(bpf->phase_vec[i]));
    }

    return 0;
}
