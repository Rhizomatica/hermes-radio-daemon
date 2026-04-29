/*
 * Symbol extraction and OFDM demod state for the RADE V2 receiver.
 *
 * This ports the rx2.py symbol path:
 *   - _extract_symbol
 *   - RADAE.receiver(..., run_decoder=False)
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#ifndef RX2_DEMOD_H
#define RX2_DEMOD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "comp.h"

struct rx2_demod {
    int M;
    int Ncp;
    int Ns;
    int Nc;
    int sym_len;
    int rx_i_len;
    int latent_dim;
    int time_offset;
    int correct_time_offset;
    float Fs;
    COMP rx_phase;
    COMP *rx_phase_vec;
    COMP *rx_i;
    COMP *rx_sym_td;
    COMP *Wfwd;
    COMP *phase_corr;
};

/*
 * Initialise the demod state.
 *
 * w is the length-Nc vector of carrier frequencies in radians/sample from the
 * Python RADAE model.
 *
 * Returns 0 on success, -1 on failure.
 */
int rx2_demod_init(struct rx2_demod *dm,
                   int M, int Ncp, int Ns, int Nc,
                   float Fs, int time_offset, int correct_time_offset,
                   const float w[]);

/* Reset rolling state to the Python constructor state. */
void rx2_demod_reset(struct rx2_demod *dm);

/* Release dynamic storage held by the demod state. */
void rx2_demod_destroy(struct rx2_demod *dm);

/*
 * Run one _extract_symbol + receiver(..., run_decoder=False) step.
 *
 * rx_buf must point to the 3*sym_len rolling buffer from the frontend.
 * z_hat_out must point to latent_dim floats.
 */
int rx2_demod_apply(struct rx2_demod *dm,
                    const COMP rx_buf[],
                    float delta_hat,
                    float freq_offset,
                    float z_hat_out[]);

#ifdef __cplusplus
}
#endif

#endif /* RX2_DEMOD_H */
