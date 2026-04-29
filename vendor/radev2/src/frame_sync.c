/*
 * FrameSyncNet pure-C port.
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#include <math.h>

#include "frame_sync.h"

static inline float relu(float x) {
    return x > 0.0f ? x : 0.0f;
}

static inline float sigmoid(float x) {
    return 1.0f / (1.0f + expf(-x));
}

float frame_sync_forward(const float z_hat[FRAME_SYNC_INPUT_DIM]) {
    float h0[FRAME_SYNC_HIDDEN_DIM];
    float h1[FRAME_SYNC_HIDDEN_DIM];

    for (int i = 0; i < FRAME_SYNC_HIDDEN_DIM; i++) {
        float acc = frame_sync_b0[i];
        for (int j = 0; j < FRAME_SYNC_INPUT_DIM; j++) {
            acc += frame_sync_w0[i][j] * z_hat[j];
        }
        h0[i] = relu(acc);
    }

    for (int i = 0; i < FRAME_SYNC_HIDDEN_DIM; i++) {
        float acc = frame_sync_b2[i];
        for (int j = 0; j < FRAME_SYNC_HIDDEN_DIM; j++) {
            acc += frame_sync_w2[i][j] * h0[j];
        }
        h1[i] = relu(acc);
    }

    float acc = frame_sync_b4;
    for (int j = 0; j < FRAME_SYNC_HIDDEN_DIM; j++) {
        acc += frame_sync_w4[j] * h1[j];
    }
    return sigmoid(acc);
}
