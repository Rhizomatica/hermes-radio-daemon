/*
 * Thin RADE V2 receiver coordinator built from the validated C slices.
 *
 * This ports the rx2.py symbol-step orchestration:
 *   - _process_symbol
 *   - _process_idle
 *   - _process_sync
 *   - _adjust_timing
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#ifndef RX2_RECEIVER_H
#define RX2_RECEIVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "rx2_coarse_sync.h"
#include "rx2_demod.h"
#include "rx2_eoo.h"
#include "rx2_frame_sync.h"
#include "rx2_frontend.h"

enum rx2_receiver_state {
    RX2_RECEIVER_IDLE = 0,
    RX2_RECEIVER_SYNC = 1
};

struct rx2_receiver_config {
    int M;
    int Ncp;
    int Ns;
    int Nc;
    float Fs;
    float B_bpf;
    int time_offset;
    int correct_time_offset;
    int auxdata;
    int limit_pitch;
    int mute;
    int agc;
    int hangover;
    int timing_adj_at;
    int reset_output_on_resync;
    int fix_delta_hat;
    const float *w;
    const COMP *pend;
};

struct rx2_receiver_step {
    int next_nin;
    int sig_det;
    int sine_det;
    int decoded_valid;
    float gain;
};

struct rx2_receiver {
    int M;
    int Ncp;
    int Ns;
    int Nc;
    int sym_len;
    int max_nin;
    float Fs;
    int state;
    int count;
    int count1;
    int n_acq;
    int s;
    int i;
    int timing_adj;
    int timing_adj_at;
    int hangover;
    int reset_output_on_resync;
    float delta_hat;
    float freq_offset;
    float freq_offset_g;
    int new_sig_delta_hat;
    int new_sig_f_hat;
    float z_hat[RADE_LATENT_DIM];
    COMP *timing_tmp;
    int timing_shift;
    struct rx2_frontend frontend;
    struct rx2_coarse_sync coarse_sync;
    struct rx2_demod demod;
    struct rx2_eoo eoo;
    struct rx2_frame_sync frame_sync;
};

int rx2_receiver_init(struct rx2_receiver *rx, const struct rx2_receiver_config *cfg);
void rx2_receiver_reset(struct rx2_receiver *rx);
void rx2_receiver_destroy(struct rx2_receiver *rx);

/*
 * Process one input chunk, updating the receiver state and optionally writing a
 * decoded feature slice.
 *
 * features_out must point to frame_sync.output_dim floats.
 * Returns 0 on success, -1 on error.
 */
int rx2_receiver_process(struct rx2_receiver *rx,
                         const COMP rx_in[],
                         int nin,
                         float features_out[],
                         struct rx2_receiver_step *step);

#ifdef __cplusplus
}
#endif

#endif /* RX2_RECEIVER_H */
