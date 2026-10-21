/*---------------------------------------------------------------------------*\

  rade_v2_ofdm.h

  OFDM modulator for RADE V2 (C port).

  V2 frame structure (no pilots):
    [data_sym_0][data_sym_1]   Ns=2 symbols, each M+Ncp=160 samples

  V2 EOO: 6 x pend_cp symbols, scaled by pilot_gain_eoo_v2

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2025 David Rowe

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

  - Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  - Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef RADE_V2_OFDM_H
#define RADE_V2_OFDM_H

#include <stdint.h>
#include "rade_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------*\
                            V2 OFDM CONSTANTS
\*---------------------------------------------------------------------------*/

#define RADE_V2_NC          14               /* Number of OFDM carriers */
#define RADE_V2_M           128              /* Samples per OFDM symbol (Fs/Rs') */
#define RADE_V2_NCP         32               /* Cyclic prefix samples (4ms * 8000) */
#define RADE_V2_NS          2                /* Data symbols per modem frame */
#define RADE_V2_SYM_LEN     (RADE_V2_M + RADE_V2_NCP)           /* 160 */
#define RADE_V2_NMF         (RADE_V2_NS * RADE_V2_SYM_LEN)      /* 320 */
#define RADE_V2_N_EOO_SYMS  6
#define RADE_V2_NEOO        (RADE_V2_N_EOO_SYMS * RADE_V2_SYM_LEN)  /* 960 */

#define RADE_V2_NB_TOTAL_FEATURES  36

/* Aux carriers (daemon-local, not RADE): one OFDM carrier just below and one
   just above the Nc data carriers, on the same grid, symbols and cyclic
   prefix, so they stay orthogonal to them. Off (all zero) unless a latent
   hook writes them, when the signal is stock RADE V2. */
#define RADE_V2_NAUX        2

/* Latent hook: once per modem frame on TX, once per received symbol in sync
   on RX (the last two symbols, either frame alignment). */
typedef struct {
    float     *z;    /* RADE_V2_LATENT_DIM floats, in place: symbol s, carrier c
                        at z[(s*Nc + c)*2] (re, im) */
    RADE_COMP *aux;  /* RADE_V2_NS * RADE_V2_NAUX aux symbols, aux[s*NAUX + a]:
                        TX: the hook writes them (zeroed before); RX: as
                        demodulated, with the data carriers' timing and
                        frequency correction */
    uint32_t   sym;  /* TX: frames sent * RADE_V2_NS; RX: the receiver's
                        symbol counter, which advances by one per symbol,
                        in sync or not */
    int        align;/* RX, set by the hook: 1 these two symbols are a frame,
                        0 they are not, -1 (as called) frame sync decides */
} rade_v2_hook_frame;
typedef void (*rade_v2_latent_hook)(void *user, rade_v2_hook_frame *f);
#define RADE_V2_NUM_USED_FEATURES  20

/*---------------------------------------------------------------------------*\
                              V2 OFDM STATE
\*---------------------------------------------------------------------------*/

typedef struct {
    float     w[RADE_V2_NC];
    float     w_aux[RADE_V2_NAUX];
    RADE_COMP Winv_aux[RADE_V2_M][RADE_V2_NAUX];
    RADE_COMP Wfwd_aux[RADE_V2_NAUX][RADE_V2_M];
    RADE_COMP phase_corr_aux[RADE_V2_NAUX];
    RADE_COMP Winv[RADE_V2_M][RADE_V2_NC];     /* IDFT matrix: Nc freq -> M time */
    RADE_COMP Wfwd[RADE_V2_NC][RADE_V2_M];     /* DFT matrix:  M time  -> Nc freq */
    RADE_COMP phase_corr[RADE_V2_NC];          /* Per-carrier phase correction exp(j*8*w[c]) */
    RADE_COMP pend[RADE_V2_NC];                /* EOO pilot symbols (freq domain) */
    RADE_COMP pend_td[RADE_V2_M];              /* EOO pilot time domain (no CP) */
    RADE_COMP pend_cp[RADE_V2_SYM_LEN];        /* EOO pilot with CP */
    RADE_COMP eoo[RADE_V2_NEOO];               /* Pre-computed EOO frame */
} rade_v2_ofdm;

/*---------------------------------------------------------------------------*\
                            FUNCTIONS
\*---------------------------------------------------------------------------*/

/* Initialise V2 OFDM state (carrier freqs, DFT/IDFT matrices, EOO frame) */
void rade_v2_ofdm_init(rade_v2_ofdm *ofdm);

/* Modulate one modem frame: z[RADE_V2_LATENT_DIM] -> tx_out[RADE_V2_NMF]
   aux: RADE_V2_NS * RADE_V2_NAUX aux carrier symbols, or NULL (off).
   Returns number of output samples (RADE_V2_NMF). */
int rade_v2_ofdm_mod_frame(const rade_v2_ofdm *ofdm, RADE_COMP *tx_out, const float *z,
                           const RADE_COMP *aux);

/* Demodulate one modem frame.
   rx_i: Ns * SYM_LEN freq-corrected IQ samples (last Ns symbols, shifted each call)
   z_hat: LATENT_DIM output floats
   aux_hat: RADE_V2_NS * RADE_V2_NAUX aux carrier symbols out, or NULL
   time_offset: fine timing offset into CP (default -16)
   correct_time_offset: per-carrier phase correction samples (default -8) */
void rade_v2_ofdm_demod_frame(const rade_v2_ofdm *ofdm, float *z_hat, RADE_COMP *aux_hat,
                               const RADE_COMP *rx_i, int time_offset);

/* Compute EOO channel sparsity metric from one time-domain symbol (M samples).
   Returns instantaneous e_cp / e_total ratio. */
float rade_v2_ofdm_eoo_metric(const rade_v2_ofdm *ofdm, const RADE_COMP *rx_sym_td);

/* Return pointer to pre-computed EOO frame, set *n_out = RADE_V2_NEOO */
const RADE_COMP *rade_v2_ofdm_get_eoo(const rade_v2_ofdm *ofdm, int *n_out);

#ifdef __cplusplus
}
#endif

#endif /* RADE_V2_OFDM_H */
