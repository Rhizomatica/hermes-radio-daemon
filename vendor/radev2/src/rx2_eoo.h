/*
 * End-of-over detector state for the RADE V2 receiver.
 *
 * This ports rx2.py _detect_eoo().
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#ifndef RX2_EOO_H
#define RX2_EOO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "comp.h"

struct rx2_eoo {
    int M;
    int Ncp;
    float TEOO;
    float ALPHA_EOO;
    float eoo_smooth;
    float eoo_corr;
    COMP *pend_fd;
    uint8_t *active;
    COMP *fft_fwd;
    COMP *fft_inv;
    COMP *rx_fd;
    COMP *H_est;
    COMP *h_est;
};

/*
 * Initialise the EOO detector from the time-domain pend symbol (length M).
 *
 * Returns 0 on success, -1 on failure.
 */
int rx2_eoo_init(struct rx2_eoo *eoo, int M, int Ncp, const COMP pend[]);

/* Reset the IIR-smoothed detector state to the Python constructor state. */
void rx2_eoo_reset(struct rx2_eoo *eoo);

/*
 * Zero the IIR smoother without touching DFT buffers or the last raw
 * correlation.  Mirrors rx2.py's `self.eoo_smooth = 0.0` on resync, which
 * intentionally leaves `_eoo_corr` at its most recent value.
 */
void rx2_eoo_clear_smoothing(struct rx2_eoo *eoo);

/* Release dynamic storage held by the detector state. */
void rx2_eoo_destroy(struct rx2_eoo *eoo);

/* Run one EOO detection step. Returns 1 when the threshold is crossed, else 0. */
int rx2_eoo_apply(struct rx2_eoo *eoo, const COMP rx_sym_td[]);

#ifdef __cplusplus
}
#endif

#endif /* RX2_EOO_H */
