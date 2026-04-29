/*
 * Thin RADE V2 receiver coordinator built from the validated C slices.
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rx2_receiver.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void rx2_receiver_zero(struct rx2_receiver *rx) {
    rx->M = 0;
    rx->Ncp = 0;
    rx->Ns = 0;
    rx->Nc = 0;
    rx->sym_len = 0;
    rx->max_nin = 0;
    rx->Fs = 0.0f;
    rx->state = RX2_RECEIVER_IDLE;
    rx->count = 0;
    rx->count1 = 0;
    rx->n_acq = 0;
    rx->s = 0;
    rx->i = 0;
    rx->timing_adj = 0;
    rx->timing_adj_at = 0;
    rx->hangover = 0;
    rx->reset_output_on_resync = 0;
    rx->delta_hat = 0.0f;
    rx->freq_offset = 0.0f;
    rx->freq_offset_g = 0.0f;
    rx->new_sig_delta_hat = 0;
    rx->new_sig_f_hat = 0;
    memset(rx->z_hat, 0, sizeof(rx->z_hat));
    rx->timing_tmp = NULL;
    rx->timing_shift = 0;
    memset(&rx->frontend, 0, sizeof(rx->frontend));
    memset(&rx->coarse_sync, 0, sizeof(rx->coarse_sync));
    memset(&rx->demod, 0, sizeof(rx->demod));
    memset(&rx->eoo, 0, sizeof(rx->eoo));
    memset(&rx->frame_sync, 0, sizeof(rx->frame_sync));
}

static float comp_angle(COMP x) {
    return atan2f(x.imag, x.real);
}

static int rx2_receiver_adjust_timing(struct rx2_receiver *rx, int nin) {
    if (!rx || !rx->timing_tmp || !rx->coarse_sync.Ry_smooth || !rx->timing_adj ||
        rx->coarse_sync.fix_delta_hat != 0) {
        return nin;
    }

    if (rx->delta_hat > 3 * rx->sym_len / 4) {
        rx->delta_hat -= rx->timing_shift;
        memcpy(rx->timing_tmp, rx->coarse_sync.Ry_smooth,
               (size_t)rx->timing_shift * sizeof(*rx->timing_tmp));
        memmove(rx->coarse_sync.Ry_smooth,
                rx->coarse_sync.Ry_smooth + rx->timing_shift,
                (size_t)(rx->sym_len - rx->timing_shift) * sizeof(*rx->coarse_sync.Ry_smooth));
        memcpy(rx->coarse_sync.Ry_smooth + rx->sym_len - rx->timing_shift,
               rx->timing_tmp,
               (size_t)rx->timing_shift * sizeof(*rx->timing_tmp));
        nin = rx->sym_len + rx->timing_shift;
    }
    if (rx->delta_hat < rx->sym_len / 4) {
        rx->delta_hat += rx->timing_shift;
        memcpy(rx->timing_tmp,
               rx->coarse_sync.Ry_smooth + rx->sym_len - rx->timing_shift,
               (size_t)rx->timing_shift * sizeof(*rx->timing_tmp));
        memmove(rx->coarse_sync.Ry_smooth + rx->timing_shift,
                rx->coarse_sync.Ry_smooth,
                (size_t)(rx->sym_len - rx->timing_shift) * sizeof(*rx->coarse_sync.Ry_smooth));
        memcpy(rx->coarse_sync.Ry_smooth,
               rx->timing_tmp,
               (size_t)rx->timing_shift * sizeof(*rx->timing_tmp));
        nin = rx->sym_len - rx->timing_shift;
    }
    return nin;
}

int rx2_receiver_init(struct rx2_receiver *rx, const struct rx2_receiver_config *cfg) {
    if (!rx || !cfg || !cfg->w || !cfg->pend ||
        cfg->M <= 0 || cfg->Ncp <= 0 || cfg->Ns <= 0 || cfg->Nc <= 0 ||
        cfg->Fs <= 0.0f || cfg->B_bpf <= 0.0f || 2 * cfg->Ns * cfg->Nc != RADE_LATENT_DIM) {
        return -1;
    }

    rx2_receiver_zero(rx);
    rx->M = cfg->M;
    rx->Ncp = cfg->Ncp;
    rx->Ns = cfg->Ns;
    rx->Nc = cfg->Nc;
    rx->sym_len = cfg->M + cfg->Ncp;
    rx->max_nin = 3 * rx->sym_len;
    rx->Fs = cfg->Fs;
    rx->timing_adj_at = cfg->timing_adj_at;
    rx->hangover = cfg->hangover;
    rx->reset_output_on_resync = cfg->reset_output_on_resync != 0;
    rx->timing_shift = rx->sym_len / 4;
    rx->timing_tmp = (COMP *)calloc((size_t)rx->timing_shift, sizeof(*rx->timing_tmp));
    if (!rx->timing_tmp) {
        rx2_receiver_destroy(rx);
        return -1;
    }
    if (rx2_frontend_init(&rx->frontend, rx->sym_len, rx->max_nin, cfg->agc) != 0 ||
        rx2_coarse_sync_init(&rx->coarse_sync, cfg->M, cfg->Ncp, cfg->Fs, cfg->B_bpf) != 0 ||
        rx2_demod_init(&rx->demod, cfg->M, cfg->Ncp, cfg->Ns, cfg->Nc,
                       cfg->Fs, cfg->time_offset, cfg->correct_time_offset, cfg->w) != 0 ||
        rx2_eoo_init(&rx->eoo, cfg->M, cfg->Ncp, cfg->pend) != 0 ||
        rx2_frame_sync_init(&rx->frame_sync, cfg->auxdata, cfg->limit_pitch, cfg->mute) != 0) {
        rx2_receiver_destroy(rx);
        return -1;
    }
    if (cfg->fix_delta_hat != 0 &&
        rx2_coarse_sync_set_fix_delta_hat(&rx->coarse_sync, cfg->fix_delta_hat) != 0) {
        rx2_receiver_destroy(rx);
        return -1;
    }
    rx2_receiver_reset(rx);
    return 0;
}

void rx2_receiver_reset(struct rx2_receiver *rx) {
    if (!rx) return;
    rx->state = RX2_RECEIVER_IDLE;
    rx->count = 0;
    rx->count1 = 0;
    rx->n_acq = 0;
    rx->s = 0;
    rx->i = 0;
    rx->timing_adj = 0;
    rx->delta_hat = 0.0f;
    rx->freq_offset = 0.0f;
    rx->freq_offset_g = 0.0f;
    rx->new_sig_delta_hat = 0;
    rx->new_sig_f_hat = 0;
    memset(rx->z_hat, 0, sizeof(rx->z_hat));
    if (rx->timing_tmp) {
        memset(rx->timing_tmp, 0, (size_t)rx->timing_shift * sizeof(*rx->timing_tmp));
    }
    rx2_frontend_reset(&rx->frontend);
    rx2_coarse_sync_reset(&rx->coarse_sync);
    rx2_demod_reset(&rx->demod);
    rx2_eoo_reset(&rx->eoo);
    rx2_frame_sync_reset(&rx->frame_sync);
}

void rx2_receiver_destroy(struct rx2_receiver *rx) {
    if (!rx) return;
    rx2_frontend_destroy(&rx->frontend);
    rx2_coarse_sync_destroy(&rx->coarse_sync);
    rx2_demod_destroy(&rx->demod);
    rx2_eoo_destroy(&rx->eoo);
    rx2_frame_sync_destroy(&rx->frame_sync);
    free(rx->timing_tmp);
    rx2_receiver_zero(rx);
}

int rx2_receiver_process(struct rx2_receiver *rx,
                         const COMP rx_in[],
                         int nin,
                         float features_out[],
                         struct rx2_receiver_step *step) {
    int sig_det;
    int sine_det;
    int next_state;
    int next_nin;
    float gain;

    if (!rx || !rx_in || !features_out || !step || nin <= 0 || nin > rx->max_nin) {
        return -1;
    }

    memset(features_out, 0, (size_t)rx->frame_sync.output_dim * sizeof(*features_out));
    memset(step, 0, sizeof(*step));

    rx->s += 1;
    if (rx2_frontend_apply(&rx->frontend, rx_in, nin, &gain) != 0) {
        return -1;
    }
    if (rx2_coarse_sync_apply(&rx->coarse_sync, rx->frontend.rx_buf, &sig_det, &sine_det) != 0) {
        return -1;
    }

    step->gain = gain;
    step->sig_det = sig_det;
    step->sine_det = sine_det;
    next_state = rx->state;
    next_nin = rx->sym_len;

    if (rx->state == RX2_RECEIVER_IDLE) {
        if (sig_det && !sine_det) {
            rx->count += 1;
        } else {
            rx->count = 0;
        }

        if (rx->count == 5) {
            float delta_phi = comp_angle(rx->coarse_sync.Ry_smooth[rx->coarse_sync.delta_hat_g]);
            rx->delta_hat = (float)rx->coarse_sync.delta_hat_g;
            rx->freq_offset = -delta_phi * rx->Fs / (2.0f * (float)M_PI * rx->M);
            rx->count = 0;
            rx->count1 = 0;
            rx->frame_sync.frame_sync_even = 0.0f;
            rx->frame_sync.frame_sync_odd = 0.0f;
            rx2_eoo_clear_smoothing(&rx->eoo);
            if (rx->reset_output_on_resync) {
                rx->i = 0;
            }
            rx->n_acq += 1;
            next_state = RX2_RECEIVER_SYNC;
        }
    } else {
        float delta_phi = comp_angle(rx->coarse_sync.Ry_smooth[rx->coarse_sync.delta_hat_g]);
        rx->freq_offset_g = -delta_phi * rx->Fs / (2.0f * (float)M_PI * rx->M);
        rx->delta_hat = rx->frame_sync.BETA * rx->delta_hat
                      + (1.0f - rx->frame_sync.BETA) * (float)rx->coarse_sync.delta_hat_g;
        rx->freq_offset = rx->frame_sync.BETA * rx->freq_offset
                        + (1.0f - rx->frame_sync.BETA) * rx->freq_offset_g;

        if (!sig_det || sine_det) {
            rx->count += 1;
        } else {
            rx->count = 0;
        }
        if (rx->count == rx->hangover) {
            next_state = RX2_RECEIVER_IDLE;
            rx->count = 0;
            rx->count1 = 0;
        }

        rx->new_sig_delta_hat = fabsf((float)rx->coarse_sync.delta_hat_g - rx->delta_hat) > rx->Ncp;
        rx->new_sig_f_hat = fabsf(rx->freq_offset_g - rx->freq_offset) > 5.0f;
        if (sig_det && (rx->new_sig_delta_hat || rx->new_sig_f_hat)) {
            rx->count1 += 1;
        } else {
            rx->count1 = 0;
        }
        if (rx->count1 == 5) {
            next_state = RX2_RECEIVER_IDLE;
            rx->count = 0;
            rx->count1 = 0;
        }

        if (rx2_demod_apply(&rx->demod, rx->frontend.rx_buf, rx->delta_hat, rx->freq_offset,
                            rx->z_hat) != 0) {
            return -1;
        }
        if (rx2_eoo_apply(&rx->eoo, rx->demod.rx_sym_td)) {
            rx->count = 0;
            rx->count1 = 0;
            rx2_eoo_clear_smoothing(&rx->eoo);
            memset(rx->coarse_sync.Ry_smooth, 0,
                   (size_t)rx->coarse_sync.sym_len * sizeof(*rx->coarse_sync.Ry_smooth));
            next_state = RX2_RECEIVER_IDLE;
        } else {
            int decoded_valid = rx2_frame_sync_apply(&rx->frame_sync,
                                                     rx->z_hat,
                                                     rx->s,
                                                     sig_det,
                                                     sine_det,
                                                     features_out);
            if (decoded_valid < 0) {
                return -1;
            }
            step->decoded_valid = decoded_valid;
        }
        next_nin = rx2_receiver_adjust_timing(rx, next_nin);
    }

    step->next_nin = next_nin;
    rx->state = next_state;
    if (step->decoded_valid) {
        rx->i += 1;
    }
    if (rx->s > rx->timing_adj_at) {
        rx->timing_adj = 1;
    }

    return 0;
}
