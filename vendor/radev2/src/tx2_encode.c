/*
 * RADE V2 transmitter wrapper ported from radae_txe2.py.
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#include <math.h>
#include <string.h>

#include "tx2_encode.h"
#include "comp_prim.h"
#include "rade_core.h"
#include "rade_enc_data.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Python radae_txe2 defaults: 101-tap BPF, 1.2× carrier spread. */
#define TX2_BPF_NTAP 101
#define TX2_BPF_WIDTH_FACTOR 1.2f

static void tx2_encode_zero(struct tx2_encode *tx) {
    memset(tx, 0, sizeof(*tx));
}

int tx2_encode_init(struct tx2_encode *tx, int auxdata, int txbpf_en) {
    if (!tx || auxdata == 0) {
        /* The compiled encoder was trained with aux bits.  auxdata=0
         * would leave feature slot 20 uninitialised; refuse explicitly. */
        return -1;
    }

    tx2_encode_zero(tx);
    tx->auxdata = 1;
    tx->txbpf_en = txbpf_en ? 1 : 0;

    if (init_radeenc(&tx->enc_model, radeenc_arrays) != 0) {
        return -1;
    }
    rade_init_encoder(&tx->enc_state);

    if (tx->txbpf_en) {
        float w_lo = tx2_model_w[0];
        float w_hi = tx2_model_w[TX2_MODEL_NC - 1];
        float bandwidth = TX2_BPF_WIDTH_FACTOR * (w_hi - w_lo)
                        * TX2_MODEL_FS / (2.0f * (float)M_PI);
        float centre    = (w_hi + w_lo) * TX2_MODEL_FS
                        / (4.0f * (float)M_PI);
        int max_len = TX2_MODEL_NMF > TX2_MODEL_NEOO ?
                      TX2_MODEL_NMF : TX2_MODEL_NEOO;
        if (complex_bpf_init(&tx->bpf, TX2_BPF_NTAP, TX2_MODEL_FS,
                             bandwidth, centre, max_len) != 0) {
            return -1;
        }
    }

    return 0;
}

void tx2_encode_reset(struct tx2_encode *tx) {
    if (!tx) return;
    rade_init_encoder(&tx->enc_state);
    if (tx->txbpf_en) {
        complex_bpf_reset(&tx->bpf);
    }
}

void tx2_encode_destroy(struct tx2_encode *tx) {
    if (!tx) return;
    if (tx->txbpf_en) {
        complex_bpf_destroy(&tx->bpf);
    }
    tx2_encode_zero(tx);
}

/*
 * Build one (num_features * RADE_FRAMES_PER_STEP)-float encoder input
 * from (RADE_FRAMES_PER_STEP * TX2_NB_TOTAL_FEATURES) FARGAN features:
 * keep the first TX2_NUM_USED_FEATURES from each frame, append aux bit
 * (-1) in slot num_used when auxdata is on.
 */
static void tx2_pack_features(const float features_in[], float features_packed[],
                              int auxdata) {
    int num_features = TX2_NUM_USED_FEATURES + (auxdata ? 1 : 0);
    for (int i = 0; i < RADE_FRAMES_PER_STEP; i++) {
        const float *src = &features_in[i * TX2_NB_TOTAL_FEATURES];
        float *dst = &features_packed[i * num_features];
        for (int j = 0; j < TX2_NUM_USED_FEATURES; j++) {
            dst[j] = src[j];
        }
        if (auxdata) {
            dst[TX2_NUM_USED_FEATURES] = -1.0f;
        }
    }
}

/*
 * IDFT one OFDM symbol: sym[Nc] × Winv[Nc][M] -> td[M].
 * Uses double accumulation to stay within float32 tolerance vs PyTorch.
 */
static void tx2_idft_symbol(const COMP sym[TX2_MODEL_NC], COMP td[TX2_MODEL_M]) {
    for (int k = 0; k < TX2_MODEL_M; k++) {
        double acc_real = 0.0;
        double acc_imag = 0.0;
        for (int c = 0; c < TX2_MODEL_NC; c++) {
            COMP wv = tx2_model_Winv[c * TX2_MODEL_M + k];
            acc_real += (double)sym[c].real * wv.real - (double)sym[c].imag * wv.imag;
            acc_imag += (double)sym[c].real * wv.imag + (double)sym[c].imag * wv.real;
        }
        td[k].real = (float)acc_real;
        td[k].imag = (float)acc_imag;
    }
}

/* Unit-magnitude clipper: x -> x / |x| (or 1+0j when x is exactly zero). */
static void tx2_unit_clipper(COMP buf[], int n) {
    for (int i = 0; i < n; i++) {
        float mag = hypotf(buf[i].real, buf[i].imag);
        if (mag > 0.0f) {
            buf[i].real /= mag;
            buf[i].imag /= mag;
        } else {
            buf[i].real = 1.0f;
            buf[i].imag = 0.0f;
        }
    }
}

int tx2_encode_frame(struct tx2_encode *tx,
                     const float features_in[TX2_N_FEATURES_IN],
                     COMP tx_out[TX2_MODEL_NMF]) {
    if (!tx || !features_in || !tx_out) return -1;

    int num_features = TX2_NUM_USED_FEATURES + (tx->auxdata ? 1 : 0);
    int enc_input_dim = num_features * RADE_FRAMES_PER_STEP;
    float features_packed[TX2_NUM_USED_FEATURES * RADE_FRAMES_PER_STEP
                        + RADE_FRAMES_PER_STEP];   /* room for aux slot */
    float latents[TX2_MODEL_LATENT_DIM];

    tx2_pack_features(features_in, features_packed, tx->auxdata);

    /* arch=0 (generic), bottleneck=3 matches radae_txe2.py production. */
    rade_core_encoder(&tx->enc_state, &tx->enc_model, latents,
                      features_packed, 0, 3);
    (void)enc_input_dim;   /* bounds checked by TX2_N_FEATURES_IN at call site */

    /* QPSK map: latent pair (re, im) -> one complex symbol.  56 floats ->
     * 28 complex symbols, reshape row-major to (Ns, Nc) = (2, 14). */
    COMP sym[TX2_MODEL_NS][TX2_MODEL_NC];
    for (int s = 0; s < TX2_MODEL_NS; s++) {
        for (int c = 0; c < TX2_MODEL_NC; c++) {
            int k = s * TX2_MODEL_NC + c;   /* 0..27 */
            sym[s][c].real = latents[2 * k];
            sym[s][c].imag = latents[2 * k + 1];
        }
    }

    /* IDFT per symbol and CP insert: each symbol produces M+Ncp samples. */
    COMP td[TX2_MODEL_M];
    for (int s = 0; s < TX2_MODEL_NS; s++) {
        tx2_idft_symbol(sym[s], td);
        COMP *dst = &tx_out[s * (TX2_MODEL_M + TX2_MODEL_NCP)];
        for (int k = 0; k < TX2_MODEL_NCP; k++) {
            dst[k] = td[TX2_MODEL_M - TX2_MODEL_NCP + k];   /* CP = symbol tail */
        }
        for (int k = 0; k < TX2_MODEL_M; k++) {
            dst[TX2_MODEL_NCP + k] = td[k];
        }
    }

    /* bottleneck=3: drive to constant envelope before any BPF. */
    tx2_unit_clipper(tx_out, TX2_MODEL_NMF);

    if (tx->txbpf_en) {
        COMP tmp[TX2_MODEL_NMF];
        if (complex_bpf_process(&tx->bpf, tx_out, TX2_MODEL_NMF, tmp) != 0) {
            return -1;
        }
        /* radae_txe2.py post-clamps magnitude to 1.0 after BPF so peak
         * stays within the PA headroom; preserve phase. */
        for (int i = 0; i < TX2_MODEL_NMF; i++) {
            float mag = hypotf(tmp[i].real, tmp[i].imag);
            if (mag > 1.0f) {
                float inv = 1.0f / mag;
                tx_out[i].real = tmp[i].real * inv;
                tx_out[i].imag = tmp[i].imag * inv;
            } else {
                tx_out[i] = tmp[i];
            }
        }
    }

    return 0;
}

int tx2_encode_eoo(struct tx2_encode *tx,
                   COMP eoo_out[TX2_MODEL_NEOO]) {
    if (!tx || !eoo_out) return -1;

    if (!tx->txbpf_en) {
        memcpy(eoo_out, tx2_model_eoo_v2, sizeof(*eoo_out) * TX2_MODEL_NEOO);
        return 0;
    }

    COMP tmp[TX2_MODEL_NEOO];
    if (complex_bpf_process(&tx->bpf, tx2_model_eoo_v2,
                            TX2_MODEL_NEOO, tmp) != 0) {
        return -1;
    }
    for (int i = 0; i < TX2_MODEL_NEOO; i++) {
        float mag = hypotf(tmp[i].real, tmp[i].imag);
        if (mag > 1.0f) {
            float inv = 1.0f / mag;
            eoo_out[i].real = tmp[i].real * inv;
            eoo_out[i].imag = tmp[i].imag * inv;
        } else {
            eoo_out[i] = tmp[i];
        }
    }
    return 0;
}
