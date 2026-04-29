/*
 * RADE V2 receiver frame-sync integration ported from rx2.py.
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#include <string.h>

#include "rx2_frame_sync.h"

int rx2_frame_sync_init(struct rx2_frame_sync *fs, int auxdata, int limit_pitch, int mute) {
    if (!fs || FRAME_SYNC_INPUT_DIM != RADE_LATENT_DIM) {
        return -1;
    }

    memset(fs, 0, sizeof(*fs));
    fs->auxdata = auxdata != 0;
    fs->limit_pitch = limit_pitch != 0;
    fs->mute = mute != 0;
    fs->num_features = 20 + (fs->auxdata ? 1 : 0);
    fs->output_dim = fs->num_features * RADE_FRAMES_PER_STEP;
    fs->BETA = 0.999f;
    if (init_radedec(&fs->dec_model, radedec_arrays) != 0) {
        return -1;
    }
    rade_init_decoder(&fs->dec_state);
    return 0;
}

void rx2_frame_sync_reset(struct rx2_frame_sync *fs) {
    if (!fs) return;
    fs->metric = 0.0f;
    fs->frame_sync_even = 0.0f;
    fs->frame_sync_odd = 0.0f;
    memset(fs->az_hat, 0, sizeof(fs->az_hat));
    rade_init_decoder(&fs->dec_state);
}

void rx2_frame_sync_destroy(struct rx2_frame_sync *fs) {
    if (!fs) return;
    memset(fs, 0, sizeof(*fs));
}

int rx2_frame_sync_apply(struct rx2_frame_sync *fs,
                         const float z_hat[FRAME_SYNC_INPUT_DIM],
                         int sym_index, int sig_det, int sine_det,
                         float features_out[]) {
    int winning = 0;

    if (!fs || !z_hat || !features_out || FRAME_SYNC_INPUT_DIM != RADE_LATENT_DIM) {
        return -1;
    }

    fs->metric = frame_sync_forward(z_hat);
    if (sym_index & 1) {
        fs->frame_sync_odd = fs->BETA * fs->frame_sync_odd + (1.0f - fs->BETA) * fs->metric;
        winning = fs->frame_sync_odd > fs->frame_sync_even;
    } else {
        fs->frame_sync_even = fs->BETA * fs->frame_sync_even + (1.0f - fs->BETA) * fs->metric;
        winning = fs->frame_sync_even > fs->frame_sync_odd;
    }

    if (!winning) {
        return 0;
    }

    memcpy(fs->az_hat, z_hat, sizeof(fs->az_hat));
    rade_core_decoder(&fs->dec_state, &fs->dec_model, features_out, fs->az_hat, 0);
    if (fs->limit_pitch) {
        for (int i = 0; i < RADE_FRAMES_PER_STEP; i++) {
            int pitch_idx = i * fs->num_features + 18;
            if (features_out[pitch_idx] < -1.4f) {
                features_out[pitch_idx] = -1.4f;
            }
        }
    }
    if (fs->mute && (!sig_det || sine_det)) {
        for (int i = 0; i < RADE_FRAMES_PER_STEP; i++) {
            features_out[i * fs->num_features] = -5.0f;
        }
    }
    return 1;
}
