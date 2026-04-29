/*
 * RADE V2 receiver end-of-over detector ported from rx2.py.
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rx2_eoo.h"
#include "comp_prim.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void rx2_eoo_zero(struct rx2_eoo *eoo) {
    eoo->M = 0;
    eoo->Ncp = 0;
    eoo->TEOO = 0.0f;
    eoo->ALPHA_EOO = 0.0f;
    eoo->eoo_smooth = 0.0f;
    eoo->eoo_corr = 0.0f;
    eoo->pend_fd = NULL;
    eoo->active = NULL;
    eoo->fft_fwd = NULL;
    eoo->fft_inv = NULL;
    eoo->rx_fd = NULL;
    eoo->H_est = NULL;
    eoo->h_est = NULL;
}

static float comp_abs(COMP x) {
    return hypotf(x.real, x.imag);
}

static COMP comp_div(COMP a, COMP b) {
    double denom = (double)b.real * b.real + (double)b.imag * b.imag;
    COMP out;
    out.real = (float)(((double)a.real * b.real + (double)a.imag * b.imag) / denom);
    out.imag = (float)(((double)a.imag * b.real - (double)a.real * b.imag) / denom);
    return out;
}

static void dft_apply(const COMP *mat, int M, const COMP in[], COMP out[]) {
    for (int k = 0; k < M; k++) {
        double acc_real = 0.0;
        double acc_imag = 0.0;
        for (int n = 0; n < M; n++) {
            COMP w = mat[k * M + n];
            acc_real += (double)in[n].real * w.real - (double)in[n].imag * w.imag;
            acc_imag += (double)in[n].real * w.imag + (double)in[n].imag * w.real;
        }
        out[k].real = (float)acc_real;
        out[k].imag = (float)acc_imag;
    }
}

int rx2_eoo_init(struct rx2_eoo *eoo, int M, int Ncp, const COMP pend[]) {
    if (!eoo || !pend || M <= 0 || Ncp <= 0 || Ncp > M) {
        return -1;
    }

    rx2_eoo_zero(eoo);
    eoo->M = M;
    eoo->Ncp = Ncp;
    eoo->TEOO = 0.75f;
    eoo->ALPHA_EOO = 0.70f;

    eoo->pend_fd = (COMP *)calloc((size_t)M, sizeof(*eoo->pend_fd));
    eoo->active = (uint8_t *)calloc((size_t)M, sizeof(*eoo->active));
    eoo->fft_fwd = (COMP *)calloc((size_t)M * (size_t)M, sizeof(*eoo->fft_fwd));
    eoo->fft_inv = (COMP *)calloc((size_t)M * (size_t)M, sizeof(*eoo->fft_inv));
    eoo->rx_fd = (COMP *)calloc((size_t)M, sizeof(*eoo->rx_fd));
    eoo->H_est = (COMP *)calloc((size_t)M, sizeof(*eoo->H_est));
    eoo->h_est = (COMP *)calloc((size_t)M, sizeof(*eoo->h_est));
    if (!eoo->pend_fd || !eoo->active || !eoo->fft_fwd || !eoo->fft_inv ||
        !eoo->rx_fd || !eoo->H_est || !eoo->h_est) {
        rx2_eoo_destroy(eoo);
        return -1;
    }

    for (int k = 0; k < M; k++) {
        for (int n = 0; n < M; n++) {
            float phi = 2.0f * (float)M_PI * (float)(k * n) / (float)M;
            eoo->fft_fwd[k * M + n] = comp_exp_j(-phi);
            eoo->fft_inv[k * M + n] = fcmult(1.0f / (float)M, comp_exp_j(phi));
        }
    }
    dft_apply(eoo->fft_fwd, M, pend, eoo->pend_fd);

    {
        float max_abs = 0.0f;
        for (int k = 0; k < M; k++) {
            float mag = comp_abs(eoo->pend_fd[k]);
            if (mag > max_abs) {
                max_abs = mag;
            }
        }
        for (int k = 0; k < M; k++) {
            eoo->active[k] = comp_abs(eoo->pend_fd[k]) > max_abs * 1e-3f;
        }
    }

    rx2_eoo_reset(eoo);
    return 0;
}

void rx2_eoo_reset(struct rx2_eoo *eoo) {
    if (!eoo) return;
    eoo->eoo_smooth = 0.0f;
    eoo->eoo_corr = 0.0f;
    if (eoo->rx_fd) memset(eoo->rx_fd, 0, (size_t)eoo->M * sizeof(*eoo->rx_fd));
    if (eoo->H_est) memset(eoo->H_est, 0, (size_t)eoo->M * sizeof(*eoo->H_est));
    if (eoo->h_est) memset(eoo->h_est, 0, (size_t)eoo->M * sizeof(*eoo->h_est));
}

void rx2_eoo_clear_smoothing(struct rx2_eoo *eoo) {
    if (!eoo) return;
    eoo->eoo_smooth = 0.0f;
}

void rx2_eoo_destroy(struct rx2_eoo *eoo) {
    if (!eoo) return;
    free(eoo->pend_fd);
    free(eoo->active);
    free(eoo->fft_fwd);
    free(eoo->fft_inv);
    free(eoo->rx_fd);
    free(eoo->H_est);
    free(eoo->h_est);
    rx2_eoo_zero(eoo);
}

int rx2_eoo_apply(struct rx2_eoo *eoo, const COMP rx_sym_td[]) {
    double e_total = 1e-12;
    double e_cp = 0.0;

    if (!eoo || !rx_sym_td || !eoo->pend_fd || !eoo->active || !eoo->fft_fwd ||
        !eoo->fft_inv || !eoo->rx_fd || !eoo->H_est || !eoo->h_est) {
        return -1;
    }

    dft_apply(eoo->fft_fwd, eoo->M, rx_sym_td, eoo->rx_fd);
    for (int k = 0; k < eoo->M; k++) {
        if (eoo->active[k]) {
            eoo->H_est[k] = comp_div(eoo->rx_fd[k], eoo->pend_fd[k]);
        } else {
            eoo->H_est[k] = comp0();
        }
    }
    dft_apply(eoo->fft_inv, eoo->M, eoo->H_est, eoo->h_est);

    for (int n = 0; n < eoo->M; n++) {
        double mag2 = (double)eoo->h_est[n].real * eoo->h_est[n].real
                    + (double)eoo->h_est[n].imag * eoo->h_est[n].imag;
        e_total += mag2;
        if (n < eoo->Ncp || n >= eoo->M - eoo->Ncp) {
            e_cp += mag2;
        }
    }

    eoo->eoo_corr = (float)(e_cp / e_total);
    eoo->eoo_smooth = eoo->ALPHA_EOO * eoo->eoo_smooth + (1.0f - eoo->ALPHA_EOO) * eoo->eoo_corr;
    return eoo->eoo_smooth > eoo->TEOO;
}
