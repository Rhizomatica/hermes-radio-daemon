/*
 * FrameSyncNet pure-C port.
 *
 * 3-layer MLP used by the RADE V2 receiver to decide which of the two
 * OFDM-frame alignments is correct.  See models_sync.py for the
 * reference PyTorch model.
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#ifndef FRAME_SYNC_H
#define FRAME_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "frame_sync_data.h"

/*
 * Run one forward pass.  `z_hat` must have FRAME_SYNC_INPUT_DIM elements.
 * Returns the sigmoid of the scalar output, in [0, 1].
 *
 * Stateless and thread-safe — no globals touched.
 */
float frame_sync_forward(const float z_hat[FRAME_SYNC_INPUT_DIM]);

#ifdef __cplusplus
}
#endif

#endif /* FRAME_SYNC_H */
