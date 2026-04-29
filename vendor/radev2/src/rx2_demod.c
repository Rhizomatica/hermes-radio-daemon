/*
 * RADE V2 receiver symbol extraction and OFDM demod helpers ported from rx2.py.
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rx2_demod.h"
#include "comp_prim.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void rx2_demod_zero(struct rx2_demod *dm) {
    dm->M = 0;
    dm->Ncp = 0;
    dm->Ns = 0;
    dm->Nc = 0;
    dm->sym_len = 0;
    dm->rx_i_len = 0;
    dm->latent_dim = 0;
    dm->time_offset = 0;
    dm->correct_time_offset = 0;
    dm->Fs = 0.0f;
    dm->rx_phase = comp0();
    dm->rx_phase_vec = NULL;
    dm->rx_i = NULL;
    dm->rx_sym_td = NULL;
    dm->Wfwd = NULL;
    dm->phase_corr = NULL;
}

int rx2_demod_init(struct rx2_demod *dm,
                   int M, int Ncp, int Ns, int Nc,
                   float Fs, int time_offset, int correct_time_offset,
                   const float w[]) {
    if (!dm || !w || M <= 0 || Ncp <= 0 || Ns <= 0 || Nc <= 0 || Fs <= 0.0f) {
        return -1;
    }

    rx2_demod_zero(dm);
    dm->M = M;
    dm->Ncp = Ncp;
    dm->Ns = Ns;
    dm->Nc = Nc;
    dm->sym_len = M + Ncp;
    dm->rx_i_len = Ns * dm->sym_len;
    dm->latent_dim = 2 * Ns * Nc;
    dm->time_offset = time_offset;
    dm->correct_time_offset = correct_time_offset;
    dm->Fs = Fs;

    dm->rx_phase_vec = (COMP *)calloc((size_t)dm->sym_len, sizeof(*dm->rx_phase_vec));
    dm->rx_i = (COMP *)calloc((size_t)dm->rx_i_len, sizeof(*dm->rx_i));
    dm->rx_sym_td = (COMP *)calloc((size_t)dm->M, sizeof(*dm->rx_sym_td));
    dm->Wfwd = (COMP *)calloc((size_t)dm->M * (size_t)dm->Nc, sizeof(*dm->Wfwd));
    dm->phase_corr = (COMP *)calloc((size_t)dm->Nc, sizeof(*dm->phase_corr));
    if (!dm->rx_phase_vec || !dm->rx_i || !dm->rx_sym_td || !dm->Wfwd || !dm->phase_corr) {
        rx2_demod_destroy(dm);
        return -1;
    }

    for (int n = 0; n < dm->M; n++) {
        for (int c = 0; c < dm->Nc; c++) {
            dm->Wfwd[n * dm->Nc + c] = comp_exp_j(-n * w[c]);
        }
    }
    for (int c = 0; c < dm->Nc; c++) {
        dm->phase_corr[c] = comp_exp_j(-dm->correct_time_offset * w[c]);
    }

    rx2_demod_reset(dm);
    return 0;
}

void rx2_demod_reset(struct rx2_demod *dm) {
    if (!dm || !dm->rx_phase_vec || !dm->rx_i || !dm->rx_sym_td) return;
    dm->rx_phase.real = 1.0f;
    dm->rx_phase.imag = 0.0f;
    memset(dm->rx_phase_vec, 0, (size_t)dm->sym_len * sizeof(*dm->rx_phase_vec));
    memset(dm->rx_i, 0, (size_t)dm->rx_i_len * sizeof(*dm->rx_i));
    memset(dm->rx_sym_td, 0, (size_t)dm->M * sizeof(*dm->rx_sym_td));
}

void rx2_demod_destroy(struct rx2_demod *dm) {
    if (!dm) return;
    free(dm->rx_phase_vec);
    free(dm->rx_i);
    free(dm->rx_sym_td);
    free(dm->Wfwd);
    free(dm->phase_corr);
    rx2_demod_zero(dm);
}

int rx2_demod_apply(struct rx2_demod *dm,
                    const COMP rx_buf[],
                    float delta_hat,
                    float freq_offset,
                    float z_hat_out[]) {
    if (!dm || !rx_buf || !z_hat_out || !dm->rx_phase_vec || !dm->rx_i ||
        !dm->rx_sym_td || !dm->Wfwd || !dm->phase_corr) {
        return -1;
    }

    int delta_hat_rx = (int)(delta_hat - (float)dm->Ncp);
    int st = dm->sym_len + delta_hat_rx;
    int en = st + dm->sym_len;
    int cp_st = dm->Ncp + dm->time_offset;
    COMP step;

    if (st < 0 || en > 3 * dm->sym_len || cp_st < 0 || cp_st + dm->M > dm->sym_len) {
        return -1;
    }

    step = comp_exp_j(-2.0f * (float)M_PI * freq_offset / dm->Fs);
    for (int n = 0; n < dm->sym_len; n++) {
        dm->rx_phase = cmult(dm->rx_phase, step);
        dm->rx_phase_vec[n] = dm->rx_phase;
    }
    memmove(dm->rx_i, dm->rx_i + dm->sym_len,
            (size_t)(dm->rx_i_len - dm->sym_len) * sizeof(*dm->rx_i));
    for (int n = 0; n < dm->sym_len; n++) {
        COMP rotated = cmult(dm->rx_phase_vec[n], rx_buf[st + n]);
        dm->rx_i[dm->rx_i_len - dm->sym_len + n] = rotated;
        if (n >= dm->Ncp) {
            dm->rx_sym_td[n - dm->Ncp] = rotated;
        }
    }

    for (int s = 0; s < dm->Ns; s++) {
        const COMP *sym = dm->rx_i + s * dm->sym_len + cp_st;
        for (int c = 0; c < dm->Nc; c++) {
            double acc_real = 0.0;
            double acc_imag = 0.0;
            COMP acc;
            int k = s * dm->Nc + c;
            for (int n = 0; n < dm->M; n++) {
                COMP wv = dm->Wfwd[n * dm->Nc + c];
                acc_real += (double)sym[n].real * wv.real - (double)sym[n].imag * wv.imag;
                acc_imag += (double)sym[n].real * wv.imag + (double)sym[n].imag * wv.real;
            }
            acc.real = (float)acc_real;
            acc.imag = (float)acc_imag;
            acc = cmult(acc, dm->phase_corr[c]);
            z_hat_out[2 * k] = acc.real;
            z_hat_out[2 * k + 1] = acc.imag;
        }
    }

    return 0;
}
