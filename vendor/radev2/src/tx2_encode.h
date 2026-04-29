/*
 * RADE V2 transmitter wrapper (pure C).
 *
 * Ports radae_txe2.py:radae_tx_v2 around the already-C stateful encoder:
 *   - feature pack (36 -> 20 used + auxdata beacon bit)
 *   - stateful encoder (rade_core_encoder)
 *   - QPSK mapping
 *   - IDFT via Winv
 *   - cyclic prefix insertion
 *   - bottleneck=3 constant-envelope clipper
 *   - optional complex_bpf
 *   - V2 end-of-over frame emission
 *
 * Production invariants are hard-coded: latent_dim=56, bottleneck=3,
 * Nzmf=1, auxdata=True, peak=True, model 250725.  See C_TX_MIGRATION.md.
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#ifndef TX2_ENCODE_H
#define TX2_ENCODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "comp.h"
#include "complex_bpf.h"
#include "rade_enc.h"
#include "tx2_model_data.h"

/* FARGAN/lpcnet feature vector size (on the wire).  */
#define TX2_NB_TOTAL_FEATURES 36

/* Features actually consumed by the encoder before auxdata. */
#define TX2_NUM_USED_FEATURES 20

/*
 * Per-call input count in floats: Nzmf * enc_stride * 36.
 *
 * enc_stride == RADE_FRAMES_PER_STEP from the compiled-in encoder.
 */
#define TX2_N_FEATURES_IN \
    (TX2_MODEL_NZMF * RADE_FRAMES_PER_STEP * TX2_NB_TOTAL_FEATURES)

struct tx2_encode {
    RADEEnc enc_model;
    RADEEncState enc_state;
    int auxdata;            /* 1 = append aux beacon; production = 1 */
    int txbpf_en;           /* 1 = filter IQ through complex_bpf */
    struct complex_bpf bpf;
};

/*
 * Initialise a TX session.
 *
 * auxdata must be 1 for the 250725 production weights (the compiled
 * encoder was trained with aux bits present).  txbpf_en enables the
 * optional transmit BPF; Hermes production leaves it off.
 *
 * Returns 0 on success, -1 on allocation or parameter failure.
 */
int tx2_encode_init(struct tx2_encode *tx, int auxdata, int txbpf_en);

/* Reset streaming state (stateful encoder + BPF memory) without re-init. */
void tx2_encode_reset(struct tx2_encode *tx);

/* Release dynamic storage. Safe on zeroed / failed-init structs. */
void tx2_encode_destroy(struct tx2_encode *tx);

/*
 * Produce one modem frame: TX2_N_FEATURES_IN floats on stdin →
 * TX2_MODEL_NMF complex samples on stdout.
 *
 * Returns 0 on success, -1 on invalid args.
 */
int tx2_encode_frame(struct tx2_encode *tx,
                     const float features_in[TX2_N_FEATURES_IN],
                     COMP tx_out[TX2_MODEL_NMF]);

/*
 * Emit the V2 end-of-over waveform (constant 6×pend_cp, pre-scaled),
 * with optional BPF post-filter when txbpf_en.  TX2_MODEL_NEOO samples.
 *
 * Returns 0 on success, -1 on invalid args.
 */
int tx2_encode_eoo(struct tx2_encode *tx,
                   COMP eoo_out[TX2_MODEL_NEOO]);

#ifdef __cplusplus
}
#endif

#endif /* TX2_ENCODE_H */
