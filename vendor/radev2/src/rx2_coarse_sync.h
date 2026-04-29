/*
 * Coarse timing/signal detection state for the RADE V2 receiver.
 *
 * This ports the rx2.py autocorrelation and signal detection helpers:
 *   - _compute_autocorr
 *   - _detect_signal
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#ifndef RX2_COARSE_SYNC_H
#define RX2_COARSE_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "comp.h"

struct rx2_coarse_sync {
    int M;
    int Ncp;
    int sym_len;
    float Fs;
    float ALPHA;
    float TSIG;
    float TSIN;
    float snr_offset_dB;
    float snr_corr_a;
    float snr_corr_b;
    float snr_est_dB;
    int16_t delta_hat_g;
    int16_t fix_delta_hat;
    float Ry_max;
    float Ry_min;
    COMP *Ry_norm;
    COMP *Ry_smooth;
};

/*
 * Initialise the coarse sync state.
 *
 * B_bpf is the effective receiver bandwidth used in rx2.py when deriving the
 * SNR3k correction from the CP correlator.
 *
 * Returns 0 on success, -1 on failure.
 */
int rx2_coarse_sync_init(struct rx2_coarse_sync *cs,
                         int M, int Ncp, float Fs, float B_bpf);

/* Reset the smoothed correlator state and derived estimates. */
void rx2_coarse_sync_reset(struct rx2_coarse_sync *cs);

/* Release dynamic storage held by the coarse sync state. */
void rx2_coarse_sync_destroy(struct rx2_coarse_sync *cs);

/* Update Ry_norm/Ry_smooth from the latest 3*sym_len rx_buf window. */
int rx2_coarse_sync_compute(struct rx2_coarse_sync *cs, const COMP rx_buf[]);

/*
 * Detect signal/sine conditions from the smoothed correlator.
 *
 * sig_det and sine_det are output booleans expressed as 0/1 integers.
 */
int rx2_coarse_sync_detect(struct rx2_coarse_sync *cs, int *sig_det, int *sine_det);

/* Convenience wrapper: compute then detect. */
int rx2_coarse_sync_apply(struct rx2_coarse_sync *cs, const COMP rx_buf[],
                          int *sig_det, int *sine_det);

/*
 * Pin delta_hat_g to a constant instead of argmax(|Ry_smooth|).  Mirrors
 * rx2.py's --fix_delta_hat test knob.  A value of 0 disables pinning
 * (i.e. argmax is used), so 0 is the production default.  Values must be
 * in [0, sym_len-1]; returns -1 otherwise.  rho used for SNR estimation
 * stays on the true peak magnitude, matching the Python reference.
 */
int rx2_coarse_sync_set_fix_delta_hat(struct rx2_coarse_sync *cs, int fix_delta_hat);

#ifdef __cplusplus
}
#endif

#endif /* RX2_COARSE_SYNC_H */
