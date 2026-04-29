#define VERSION 2

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comp.h"
#include "complex_bpf.h"
#include "rade_api.h"
#include "rx2_model_data.h"
#include "rx2_receiver.h"
#include "tx2_encode.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum {
    RADE_TOTAL_FEATURES = 36,
    RADE_USED_FEATURES = 20,
    RADE_AUXDATA_FEATURES = 1,
    RADE_MAX_FEATURES_PER_FRAME = RADE_USED_FEATURES + RADE_AUXDATA_FEATURES
};

enum rade_backend {
    RADE_BACKEND_NONE = 0,
    RADE_BACKEND_RX_V2_PURE_C = 1,
    RADE_BACKEND_TX_V2_PURE_C = 2
};

struct rade {
    int backend;
    int flags;
    int auxdata;
    int nb_total_features;
    int num_used_features;
    int num_features;

    int Nmf;
    int Neoo;
    int nin;
    int nin_max;
    int n_features_in;
    int n_features_out;
    int n_eoo_bits;
    int sync;
    int snr;
    float freq_offset;

    struct complex_bpf rx2_bpf;
    int rx2_bpf_en;
    COMP *rx2_bpf_out;
    struct rx2_receiver rx2_receiver;
    struct tx2_encode tx2_encode;
};

static void rade_set_common_feature_meta(struct rade *r) {
    r->auxdata = RADE_AUXDATA_FEATURES;
    r->nb_total_features = RADE_TOTAL_FEATURES;
    r->num_used_features = RADE_USED_FEATURES;
    r->num_features = r->num_used_features + r->auxdata;
    assert(r->num_features <= RADE_MAX_FEATURES_PER_FRAME);
}

static int rade_is_compiled_identifier(const char requested[], const char compiled_in[]) {
    return requested == NULL || requested[0] == '\0' || strcmp(requested, compiled_in) == 0;
}

static int rade_validate_tx_v2_model_paths(const char model_file[]) {
    if (!rade_is_compiled_identifier(model_file, RADE_TX_V2_COMPILED_MODEL_NAME)) {
        fprintf(stderr,
                "rade_tx_v2_pure_c_open: requested model \"%s\" does not match compiled-in model \"%s\"\n",
                model_file, RADE_TX_V2_COMPILED_MODEL_NAME);
        return -1;
    }
    return 0;
}

static int rade_validate_rx_v2_model_paths(const char model_file[], const char frame_sync_model_file[]) {
    if (!rade_is_compiled_identifier(model_file, RADE_RX_V2_COMPILED_MODEL_NAME)) {
        fprintf(stderr,
                "rade_rx_v2_pure_c_open: requested model \"%s\" does not match compiled-in model \"%s\"\n",
                model_file, RADE_RX_V2_COMPILED_MODEL_NAME);
        return -1;
    }
    if (!rade_is_compiled_identifier(frame_sync_model_file, RADE_RX_V2_COMPILED_FRAME_SYNC_MODEL_NAME)) {
        fprintf(stderr,
                "rade_rx_v2_pure_c_open: requested frame sync model \"%s\" does not match compiled-in model \"%s\"\n",
                frame_sync_model_file, RADE_RX_V2_COMPILED_FRAME_SYNC_MODEL_NAME);
        return -1;
    }
    return 0;
}

void rade_initialize(void) {}
void rade_finalize(void) {}

struct rade *rade_open(char model_file[], int flags) {
    (void)model_file;
    (void)flags;
    fprintf(stderr,
            "rade_open: legacy Python-backed API is not vendored here; use rade_tx_v2_pure_c_open() / rade_rx_v2_pure_c_open()\n");
    return NULL;
}

struct rade *rade_rx_v2_pure_c_open(const char model_file[],
                                    const char frame_sync_model_file[],
                                    int flags) {
    struct rade *r;
    struct rx2_receiver_config cfg;
    float B_bpf;

    if (rade_validate_rx_v2_model_paths(model_file, frame_sync_model_file) != 0) {
        return NULL;
    }

    r = (struct rade *)calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->backend = RADE_BACKEND_RX_V2_PURE_C;
    r->flags = flags | RADE_USE_C_DECODER;
    rade_set_common_feature_meta(r);

    B_bpf = 1.2f * (rx2_model_w[RX2_MODEL_NC - 1] - rx2_model_w[0]) * RX2_MODEL_FS / (2.0f * (float)M_PI);
    memset(&cfg, 0, sizeof(cfg));
    cfg.M = RX2_MODEL_M;
    cfg.Ncp = RX2_MODEL_NCP;
    cfg.Ns = RX2_MODEL_NS;
    cfg.Nc = RX2_MODEL_NC;
    cfg.Fs = RX2_MODEL_FS;
    cfg.B_bpf = B_bpf;
    cfg.time_offset = RX2_MODEL_TIME_OFFSET;
    cfg.correct_time_offset = RX2_MODEL_CORRECT_TIME_OFFSET;
    cfg.auxdata = r->auxdata;
    cfg.limit_pitch = 1;
    cfg.mute = 1;
    cfg.agc = 1;
    cfg.hangover = 75;
    cfg.timing_adj_at = 16;
    cfg.reset_output_on_resync = 0;
    cfg.fix_delta_hat = 0;
    cfg.w = rx2_model_w;
    cfg.pend = rx2_model_pend;
    if (rx2_receiver_init(&r->rx2_receiver, &cfg) != 0) {
        free(r);
        return NULL;
    }

    r->n_features_in = r->nb_total_features * RADE_FRAMES_PER_STEP;
    r->n_features_out = r->n_features_in;
    r->n_eoo_bits = 0;
    r->Nmf = 0;
    r->Neoo = 0;
    r->nin = r->rx2_receiver.sym_len;
    r->nin_max = r->rx2_receiver.max_nin;
    r->sync = 0;
    r->snr = 0;
    r->freq_offset = 0.0f;
    r->rx2_bpf_en = 1;
    r->rx2_bpf_out = (COMP *)malloc(sizeof(*r->rx2_bpf_out) * (size_t)r->nin_max);
    if (r->rx2_bpf_out == NULL) {
        rx2_receiver_destroy(&r->rx2_receiver);
        free(r);
        return NULL;
    }
    if (complex_bpf_init(&r->rx2_bpf, 101, RX2_MODEL_FS, B_bpf,
                         (rx2_model_w[RX2_MODEL_NC - 1] + rx2_model_w[0]) * RX2_MODEL_FS / (4.0f * (float)M_PI),
                         r->nin_max) != 0) {
        free(r->rx2_bpf_out);
        rx2_receiver_destroy(&r->rx2_receiver);
        free(r);
        return NULL;
    }

    return r;
}

struct rade *rade_tx_v2_pure_c_open(const char model_file[], int flags) {
    struct rade *r;
    int txbpf_en;

    if (rade_validate_tx_v2_model_paths(model_file) != 0) {
        return NULL;
    }

    r = (struct rade *)calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->backend = RADE_BACKEND_TX_V2_PURE_C;
    r->flags = flags;
    rade_set_common_feature_meta(r);

    txbpf_en = (flags & RADE_TX_V2_USE_BPF) ? 1 : 0;
    if (tx2_encode_init(&r->tx2_encode, 1, txbpf_en) != 0) {
        free(r);
        return NULL;
    }

    r->n_features_in = TX2_N_FEATURES_IN;
    r->n_features_out = 0;
    r->n_eoo_bits = 0;
    r->Nmf = TX2_MODEL_NMF;
    r->Neoo = TX2_MODEL_NEOO;
    r->nin = 0;
    r->nin_max = 0;
    r->sync = 0;
    r->snr = 0;
    r->freq_offset = 0.0f;

    return r;
}

void rade_close(struct rade *r) {
    if (!r) return;

    if (r->backend == RADE_BACKEND_RX_V2_PURE_C) {
        complex_bpf_destroy(&r->rx2_bpf);
        free(r->rx2_bpf_out);
        rx2_receiver_destroy(&r->rx2_receiver);
    } else if (r->backend == RADE_BACKEND_TX_V2_PURE_C) {
        tx2_encode_destroy(&r->tx2_encode);
    }
    free(r);
}

int rade_version(void) { return VERSION; }
int rade_n_tx_out(struct rade *r) { assert(r != NULL); return r->Nmf; }
int rade_n_tx_eoo_out(struct rade *r) { assert(r != NULL); return r->Neoo; }
int rade_nin_max(struct rade *r) { assert(r != NULL); return r->nin_max; }
int rade_nin(struct rade *r) { assert(r != NULL); return r->nin; }
int rade_n_features_in_out(struct rade *r) { assert(r != NULL); return r->n_features_in; }
int rade_n_eoo_bits(struct rade *r) { assert(r != NULL); return r->n_eoo_bits; }

void rade_tx_set_eoo_bits(struct rade *r, float eoo_bits[]) {
    (void)r;
    (void)eoo_bits;
}

int rade_tx(struct rade *r, RADE_COMP tx_out[], float floats_in[]) {
    assert(r != NULL);
    assert(r->backend == RADE_BACKEND_TX_V2_PURE_C);
    assert(tx_out != NULL);
    assert(floats_in != NULL);

    if (tx2_encode_frame(&r->tx2_encode, floats_in, (COMP *)tx_out) != 0) {
        return 0;
    }
    return r->Nmf;
}

int rade_tx_eoo(struct rade *r, RADE_COMP tx_eoo_out[]) {
    assert(r != NULL);
    assert(r->backend == RADE_BACKEND_TX_V2_PURE_C);
    assert(tx_eoo_out != NULL);

    if (tx2_encode_eoo(&r->tx2_encode, (COMP *)tx_eoo_out) != 0) {
        return 0;
    }
    return r->Neoo;
}

int rade_rx_v2_pure_c(struct rade *r, float features_out[], int *has_eoo_out, float eoo_out[], RADE_COMP rx_in[]) {
    struct rx2_receiver_step step;
    float raw_features[RADE_FRAMES_PER_STEP * RADE_MAX_FEATURES_PER_FRAME];
    const COMP *rx_step = (const COMP *)rx_in;

    (void)eoo_out;
    assert(r != NULL);
    assert(r->backend == RADE_BACKEND_RX_V2_PURE_C);
    assert(features_out != NULL);
    assert(has_eoo_out != NULL);
    assert(rx_in != NULL);

    memset(features_out, 0, sizeof(float) * (size_t)r->n_features_out);
    *has_eoo_out = 0;

    if (r->rx2_bpf_en) {
        if (complex_bpf_process(&r->rx2_bpf, (const COMP *)rx_in, r->nin, r->rx2_bpf_out) != 0) {
            return 0;
        }
        rx_step = r->rx2_bpf_out;
    }
    if (rx2_receiver_process(&r->rx2_receiver, rx_step, r->nin, raw_features, &step) != 0) {
        return 0;
    }

    r->nin = step.next_nin;
    r->sync = r->rx2_receiver.state == RX2_RECEIVER_SYNC;
    r->snr = (int)r->rx2_receiver.coarse_sync.snr_est_dB;
    r->freq_offset = r->rx2_receiver.freq_offset;

    if (step.decoded_valid) {
        for (int i = 0; i < RADE_FRAMES_PER_STEP; i++) {
            memcpy(&features_out[i * r->nb_total_features],
                   &raw_features[i * r->num_features],
                   sizeof(float) * (size_t)r->num_features);
        }
        return r->n_features_out;
    }
    return 0;
}

int rade_rx(struct rade *r, float features_out[], int *has_eoo_out, float eoo_out[], RADE_COMP rx_in[]) {
    assert(r != NULL);
    assert(r->backend == RADE_BACKEND_RX_V2_PURE_C);
    return rade_rx_v2_pure_c(r, features_out, has_eoo_out, eoo_out, rx_in);
}

int rade_sync(struct rade *r) {
    assert(r != NULL);
    return r->sync;
}

float rade_freq_offset(struct rade *r) {
    assert(r != NULL);
    return r->freq_offset;
}

int rade_snrdB_3k_est(struct rade *r) {
    assert(r != NULL);
    return r->snr;
}
