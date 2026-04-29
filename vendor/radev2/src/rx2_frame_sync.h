/*
 * Frame-sync integration state for the RADE V2 receiver.
 *
 * This ports rx2.py _update_frame_sync():
 *   - FrameSyncNet metric update
 *   - odd/even winner tracking
 *   - winning az_hat retention
 *   - stateful core decoder handoff
 *   - pitch clamp and mute post-processing
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#ifndef RX2_FRAME_SYNC_H
#define RX2_FRAME_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "frame_sync.h"
#include "rade_constants.h"
#include "rade_core.h"
#include "rade_dec.h"
#include "rade_dec_data.h"

struct rx2_frame_sync {
    int auxdata;
    int limit_pitch;
    int mute;
    int num_features;
    int output_dim;
    float BETA;
    float metric;
    float frame_sync_even;
    float frame_sync_odd;
    float az_hat[FRAME_SYNC_INPUT_DIM];
    RADEDec dec_model;
    RADEDecState dec_state;
};

/*
 * Initialise the frame-sync integration state.
 *
 * Returns 0 on success, -1 on failure.
 */
int rx2_frame_sync_init(struct rx2_frame_sync *fs, int auxdata, int limit_pitch, int mute);

/* Reset rolling state to the Python constructor state. */
void rx2_frame_sync_reset(struct rx2_frame_sync *fs);

/* Release dynamic storage or other state. Safe on zeroed structs. */
void rx2_frame_sync_destroy(struct rx2_frame_sync *fs);

/*
 * Update odd/even sync metrics and decode if the current symbol wins.
 *
 * sym_index is the receiver symbol counter self.s from rx2.py.
 * features_out must point to output_dim floats.
 *
 * Returns 1 when a decoded feature slice was written, 0 otherwise, -1 on error.
 */
int rx2_frame_sync_apply(struct rx2_frame_sync *fs,
                         const float z_hat[FRAME_SYNC_INPUT_DIM],
                         int sym_index, int sig_det, int sine_det,
                         float features_out[]);

#ifdef __cplusplus
}
#endif

#endif /* RX2_FRAME_SYNC_H */
