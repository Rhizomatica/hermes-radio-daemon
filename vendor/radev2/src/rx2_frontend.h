/*
 * Front-end state for the RADE V2 receiver.
 *
 * This is the first non-BPF DSP slice from rx2.py:
 *   - AGC gain computation (_compute_gain)
 *   - rolling RX input buffer update (_update_rx_buf)
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#ifndef RX2_FRONTEND_H
#define RX2_FRONTEND_H

#ifdef __cplusplus
extern "C" {
#endif

#include "comp.h"

struct rx2_frontend {
    int sym_len;
    int max_nin;
    int rx_buf_len;
    int agc_en;
    float agc_target;
    COMP *rx_buf;
};

/*
 * Initialise the RX front-end state.
 *
 * sym_len must match the Python receiver's Ncp + M.
 * max_nin is the maximum chunk size expected from the caller.
 *
 * Returns 0 on success, -1 on failure.
 */
int rx2_frontend_init(struct rx2_frontend *fe, int sym_len, int max_nin, int agc_en);

/* Reset rolling state to the Python constructor state. */
void rx2_frontend_reset(struct rx2_frontend *fe);

/* Release dynamic storage held by the front-end. Safe on zeroed structs. */
void rx2_frontend_destroy(struct rx2_frontend *fe);

/* Toggle AGC on/off to mirror args.agc in rx2.py. */
void rx2_frontend_set_agc(struct rx2_frontend *fe, int agc_en);

/* Compute the AGC gain for one input chunk. */
float rx2_frontend_compute_gain(const struct rx2_frontend *fe, const COMP rx_in[], int nin);

/* Update the rolling rx_buf with a new chunk, applying gain first. */
int rx2_frontend_update_rx_buf(struct rx2_frontend *fe, const COMP rx_in[], int nin, float gain);

/* Convenience wrapper: compute gain, update buffer, return 0 on success. */
int rx2_frontend_apply(struct rx2_frontend *fe, const COMP rx_in[], int nin, float *gain_out);

#ifdef __cplusplus
}
#endif

#endif /* RX2_FRONTEND_H */
