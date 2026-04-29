/*
 * RADE V2 receiver coarse timing/signal detection helpers ported from rx2.py.
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rx2_coarse_sync.h"

static void rx2_coarse_sync_zero(struct rx2_coarse_sync *cs) {
    cs->M = 0;
    cs->Ncp = 0;
    cs->sym_len = 0;
    cs->Fs = 0.0f;
    cs->ALPHA = 0.0f;
    cs->TSIG = 0.0f;
    cs->TSIN = 0.0f;
    cs->snr_offset_dB = 0.0f;
    cs->snr_corr_a = 0.0f;
    cs->snr_corr_b = 0.0f;
    cs->snr_est_dB = 0.0f;
    cs->delta_hat_g = 0;
    cs->fix_delta_hat = 0;
    cs->Ry_max = 0.0f;
    cs->Ry_min = 0.0f;
    cs->Ry_norm = NULL;
    cs->Ry_smooth = NULL;
}

static float comp_abs(COMP x) {
    return hypotf(x.real, x.imag);
}

int rx2_coarse_sync_init(struct rx2_coarse_sync *cs,
                         int M, int Ncp, float Fs, float B_bpf) {
    if (!cs || M <= 0 || Ncp <= 0 || Fs <= 0.0f || B_bpf <= 0.0f) {
        return -1;
    }

    rx2_coarse_sync_zero(cs);
    cs->M = M;
    cs->Ncp = Ncp;
    cs->sym_len = M + Ncp;
    cs->Fs = Fs;
    cs->ALPHA = 0.95f;
    cs->TSIG = 0.38f;
    cs->TSIN = 4.0f;
    cs->snr_offset_dB = 10.0f * log10f(3000.0f / B_bpf);
    cs->snr_corr_a = 1.24392558f;
    cs->snr_corr_b = 3.33253932f;
    cs->Ry_norm = (COMP *)calloc((size_t)cs->sym_len, sizeof(*cs->Ry_norm));
    cs->Ry_smooth = (COMP *)calloc((size_t)cs->sym_len, sizeof(*cs->Ry_smooth));
    if (!cs->Ry_norm || !cs->Ry_smooth) {
        rx2_coarse_sync_destroy(cs);
        return -1;
    }
    return 0;
}

void rx2_coarse_sync_reset(struct rx2_coarse_sync *cs) {
    if (!cs || !cs->Ry_norm || !cs->Ry_smooth) return;
    memset(cs->Ry_norm, 0, (size_t)cs->sym_len * sizeof(*cs->Ry_norm));
    memset(cs->Ry_smooth, 0, (size_t)cs->sym_len * sizeof(*cs->Ry_smooth));
    cs->delta_hat_g = 0;
    /* fix_delta_hat is config, not runtime state — preserved across resets. */
    cs->Ry_max = 0.0f;
    cs->Ry_min = 0.0f;
    cs->snr_est_dB = 0.0f;
}

void rx2_coarse_sync_destroy(struct rx2_coarse_sync *cs) {
    if (!cs) return;
    free(cs->Ry_norm);
    free(cs->Ry_smooth);
    rx2_coarse_sync_zero(cs);
}

int rx2_coarse_sync_compute(struct rx2_coarse_sync *cs, const COMP rx_buf[]) {
    if (!cs || !cs->Ry_norm || !cs->Ry_smooth || !rx_buf) {
        return -1;
    }

    for (int gamma = 0; gamma < cs->sym_len; gamma++) {
        int idx = cs->sym_len + gamma;
        float Ry_real = 0.0f;
        float Ry_imag = 0.0f;
        float D = 1e-12f;

        for (int n = 0; n < cs->Ncp; n++) {
            COMP y_cp = rx_buf[idx - cs->Ncp + n];
            COMP y_m = rx_buf[idx - cs->Ncp + cs->M + n];
            Ry_real += y_cp.real * y_m.real + y_cp.imag * y_m.imag;
            Ry_imag += y_cp.imag * y_m.real - y_cp.real * y_m.imag;
            D += y_cp.real * y_cp.real + y_cp.imag * y_cp.imag
               + y_m.real * y_m.real + y_m.imag * y_m.imag;
        }

        cs->Ry_norm[gamma].real = 2.0f * Ry_real / fabsf(D);
        cs->Ry_norm[gamma].imag = 2.0f * Ry_imag / fabsf(D);
        cs->Ry_smooth[gamma].real = cs->ALPHA * cs->Ry_smooth[gamma].real
                                  + (1.0f - cs->ALPHA) * cs->Ry_norm[gamma].real;
        cs->Ry_smooth[gamma].imag = cs->ALPHA * cs->Ry_smooth[gamma].imag
                                  + (1.0f - cs->ALPHA) * cs->Ry_norm[gamma].imag;
    }

    return 0;
}

int rx2_coarse_sync_detect(struct rx2_coarse_sync *cs, int *sig_det, int *sine_det) {
    if (!cs || !cs->Ry_smooth || !sig_det || !sine_det) {
        return -1;
    }

    int argmax = 0;
    float true_peak = comp_abs(cs->Ry_smooth[0]);
    cs->Ry_min = true_peak;

    for (int i = 1; i < cs->sym_len; i++) {
        float mag = comp_abs(cs->Ry_smooth[i]);
        if (mag > true_peak) {
            true_peak = mag;
            argmax = i;
        }
        if (mag < cs->Ry_min) {
            cs->Ry_min = mag;
        }
    }

    if (cs->fix_delta_hat != 0) {
        cs->delta_hat_g = cs->fix_delta_hat;
        cs->Ry_max = comp_abs(cs->Ry_smooth[cs->fix_delta_hat]);
    } else {
        cs->delta_hat_g = (int16_t)argmax;
        cs->Ry_max = true_peak;
    }

    *sig_det = cs->Ry_max > cs->TSIG;
    *sine_det = cs->Ry_max / (cs->Ry_min + 1e-12f) < cs->TSIN;

    /* rho follows rx2.py: np.max(abs_Ry), not the pinned Ry_max. */
    float rho = fminf(true_peak, 1.0f - 1e-6f);
    if (rho < 0.0f) {
        rho = 0.0f;
    }
    cs->snr_est_dB = cs->snr_corr_a
                   * (10.0f * log10f(rho / (1.0f - rho) + 1e-12f) - cs->snr_offset_dB)
                   + cs->snr_corr_b;
    return 0;
}

int rx2_coarse_sync_set_fix_delta_hat(struct rx2_coarse_sync *cs, int fix_delta_hat) {
    if (!cs) return -1;
    if (fix_delta_hat < 0 || fix_delta_hat >= cs->sym_len) return -1;
    cs->fix_delta_hat = (int16_t)fix_delta_hat;
    return 0;
}

int rx2_coarse_sync_apply(struct rx2_coarse_sync *cs, const COMP rx_buf[],
                          int *sig_det, int *sine_det) {
    if (rx2_coarse_sync_compute(cs, rx_buf) != 0) {
        return -1;
    }
    return rx2_coarse_sync_detect(cs, sig_det, sine_det);
}
