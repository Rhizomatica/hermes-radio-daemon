/* hermes-radio-daemon — opt-in encryption for RADE V2 digital voice
 *
 * HERMES extension, not part of RADE. RADE V2 sends each 40 ms modem frame
 * as a 56-float latent vector on 14 OFDM carriers x 2 symbols, with no
 * pilots and no bits to XOR. The scrambling here is analogue:
 *
 * - Per carrier c, the frame's two symbols (x0, x1) are mixed by a keyed
 *   2x2 unitary U_c: (y0, y1) = U_c (x0, x1). The receiver applies U_c^H to
 *   the demodulated latent before frame sync and the decoder (the latent
 *   hook in vendor/rade_c).
 * - The channel multiplies each carrier by one complex gain h_c, the same on
 *   both symbols of a frame while the channel is slow against 40 ms; h_c is
 *   a scalar on the pair, so it commutes with U_c and the decoder sees
 *   h_c (x0, x1) exactly as without scrambling. Noise stays white.
 * - U_c is drawn Haar-uniform from SU(2) per carrier and per frame, from
 *   ChaCha20 keystream (voice_crypto.c, domain VOICE_DOMAIN_RADE_LATENT,
 *   counter = frame * 4 + block). Without the key the pair's direction is
 *   uniform on the 3-sphere: what leaks is each carrier's pair energy
 *   |x0|^2 + |x1|^2, 14 values per frame.
 *
 * Fresh keys every frame are required, not just a hardening: a fixed U_c
 * leaves a repeating pattern that, under multipath, the receiver's
 * end-of-over detector mistakes for the EOO pilot.
 *
 * Signalling: a receiver has to learn the over's nonce and the frame
 * counter, with no clock shared between stations and no gap in the speech.
 * RADE V2's 14 carriers fill 1062-1937 Hz; two aux carriers on the same
 * OFDM grid, one bin below and one above (vendor/rade_c: RADE_V2_NAUX), carry
 * them in the clear, continuously, orthogonal to the speech:
 *
 * - A message every RADE_VOICE_MSG_FRAMES frames (1.04 s): the nonce (48
 *   bits), the message's index j in Gray code (12; the frame counter is
 *   j * RADE_VOICE_MSG_FRAMES + the frame in the message), a keyed check of
 *   the nonce (20: VOICE_DOMAIN_RADE_SIGNAL) and a CRC-16. K=7 rate 1/2
 *   convolutional code, interleaved over both carriers and the whole
 *   message, differential QPSK symbol to symbol on each carrier (no
 *   channel estimate needed).
 * - The receiver tries a decode on every symbol, of the message alone and
 *   combined with up to three before it: consecutive messages differ only
 *   in one Gray bit, and the code is linear, so their soft bits add once
 *   that bit's coded difference is undone, for each place it can be. Four
 *   messages give ~6 dB over one. A CRC-good message whose check matches
 *   locks: the nonce, and which received symbol is which frame (so it also
 *   picks the frame alignment). A CRC-good message whose check fails is a
 *   wrong key; aux carriers carrying power at all mark the over encrypted,
 *   within a few symbols, so a receiver mutes it until it locks instead of
 *   playing scrambled noise.
 * - j rolls over at 4096 (65 min): the transmitter then starts a new
 *   nonce, which the next message carries.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RADE_VOICE_H_
#define RADE_VOICE_H_

#include <stdbool.h>
#include <stdint.h>

#include "voice_crypto.h"

/* RADE V2's latent layout (vendor/rade_c/src/rade_v2_ofdm.h), checked
 * against it where both are included. */
#define RADE_VOICE_NC     14
#define RADE_VOICE_NS     2
#define RADE_VOICE_LATENT (RADE_VOICE_NC * RADE_VOICE_NS * 2)

/* Mix one modem frame's latent in place with frame `frame`'s keyed
 * unitaries of the over with nonce r; inverse: unmix. Returns false, z
 * untouched, without a key. */
bool rade_voice_scramble(float z[RADE_VOICE_LATENT],
                         const uint8_t r[VOICE_NONCE_RAND_BYTES],
                         uint32_t frame, bool inverse);

/* Aux carriers (vendor/rade_c RADE_V2_NAUX), symbols as (re, im) pairs at
 * aux[(s*NAUX + a)*2]. */
#define RADE_VOICE_NAUX       2
#define RADE_VOICE_AUX        (RADE_VOICE_NS * RADE_VOICE_NAUX * 2)
#define RADE_VOICE_MSG_FRAMES 26
#define RADE_VOICE_MSG_SLOTS  (RADE_VOICE_MSG_FRAMES * RADE_VOICE_NS * RADE_VOICE_NAUX * 2)
#define RADE_VOICE_RING       256  /* received symbols kept: 4 messages and more */

typedef enum {
    RADE_VC_CLEAR = 0,     /* over not encrypted (aux carriers silent) */
    RADE_VC_DECRYPTING,    /* locked, key matches, playing */
    RADE_VC_ACQUIRING,     /* encrypted, waiting for a message */
    RADE_VC_NO_KEY,        /* encrypted, no key loaded: muted */
    RADE_VC_BAD_KEY,       /* encrypted, messages fail the key check: muted */
} rade_vc_status;

typedef struct {
    bool     encrypt;        /* this over is scrambled and signalled */
    bool     fail_closed;    /* encryption asked for, no key: send nothing */
    uint8_t  r[VOICE_NONCE_RAND_BYTES];
    uint32_t k;              /* the next frame's counter */
    float    gain;           /* aux carriers against the data carriers' RMS */
    float    amp;            /* the data carriers' running RMS */
    float    ref[RADE_VOICE_NAUX][2];   /* last aux phase per carrier, unit */
    uint8_t  slots[RADE_VOICE_MSG_SLOTS];
} rade_voice_tx;

/* Start an over. encrypt without a key fails closed (and returns false):
 * every frame is then silence. aux_amp: the aux carriers' amplitude against
 * the data carriers' RMS (1: the same). */
bool rade_voice_tx_begin(rade_voice_tx *t, bool encrypt, float aux_amp);
/* One frame from the TX latent hook: scramble z, write the aux symbols. */
void rade_voice_tx_frame(rade_voice_tx *t, float z[RADE_VOICE_LATENT], float aux[RADE_VOICE_AUX]);

typedef struct {
    float    d[RADE_VOICE_RING][RADE_VOICE_NAUX][2];  /* differential products */
    uint32_t d_sym[RADE_VOICE_RING];                   /* which symbol, + 1 (0: none) */
    float    d_pow;          /* running mean |d|, to scale the soft bits */
    float    pa5[RADE_VOICE_NAUX], pd5[RADE_VOICE_NAUX];   /* aux and adjacent data
                                * carrier power, ~5 symbols */
    float    pd_long[RADE_VOICE_NAUX];   /* and the data carrier's, ~1 s */
    float    pall5;          /* all data carriers' mean power, ~5 symbols */
    float    p_aux, p_data;  /* their means while the channel is strong */
    uint32_t last_sym;
    bool     any;            /* a symbol has been seen */
    bool     aux_on;         /* the aux carriers carry power (with hysteresis) */
    bool     enc_seen;       /* aux_on, or a message came (then for the over) */
    uint8_t  n_sym;          /* symbols seen, up to 255 */
    bool     aligned;        /* a message was seen: where they end */
    uint32_t align_sym;
    int      align_miss;     /* aligned, unlocked: message ends with none, in a row */
    bool     locked;
    uint8_t  r[VOICE_NONCE_RAND_BYTES];
    uint32_t ref_sym, ref_k; /* symbol ref_sym is frame ref_k's second one */
    int      track_miss;     /* locked: expected messages missing in a row */
    bool     hold;           /* locked, but the last expected message was absent: muted */
    int      bad_checks;     /* CRC-good messages failing the check, a message apart */
    uint32_t bad_sym;        /* where the last of them ended */
    bool     bad_key;
    uint32_t msgs, msgs_bad; /* messages taken; CRC-good ones failing the check */
    rade_vc_status status;
} rade_voice_rx;

void rade_voice_rx_reset(rade_voice_rx *r);
/* One RX latent hook call: the frame's two symbols, sym the receiver's
 * counter of the newer one. Returns 1 when locked and the two symbols are
 * a frame (z is then unmixed in place), 0 when locked and they are not,
 * -1 when not locked: the hook's align. A gap of over 2 s in sym (the
 * receiver lost the signal) starts afresh. */
int rade_voice_rx_symbol(rade_voice_rx *r, float z[RADE_VOICE_LATENT],
                         const float aux[RADE_VOICE_AUX], uint32_t sym);
/* Whether decoded speech should be played: a clear over, or decrypting. */
bool rade_voice_rx_play(const rade_voice_rx *r);

/* The mixing undoes the encoder's low PAPR (3.4 dB to ~10 dB), and a
 * transmitter is peak-limited: unclipped, an encrypted over goes out ~6 dB
 * weaker. The clipper takes it back to stock RADE V2's PAPR: RADE_VOICE_CLIP_
 * PASSES times, clip the envelope at RADE_VOICE_CLIP_DB over the running
 * mean power, then band-pass 780..2220 Hz (the data and aux carriers) to
 * remove what the clip spreads. Streaming, on RADE's 8 kHz complex output;
 * it delays it by RADE_VOICE_CLIP_DELAY samples, so feed it that many zeros
 * after the end of over. */
#define RADE_VOICE_CLIP_PASSES 3
#define RADE_VOICE_CLIP_DB     2.0f
#define RADE_VOICE_CLIP_TAPS   121
#define RADE_VOICE_CLIP_DELAY  (RADE_VOICE_CLIP_PASSES * (RADE_VOICE_CLIP_TAPS - 1) / 2)

typedef struct {
    float hist[RADE_VOICE_CLIP_PASSES][RADE_VOICE_CLIP_TAPS][2];
    int   at;                /* where the next sample goes in hist */
    float power[RADE_VOICE_CLIP_PASSES];   /* each pass's running input power */
    int   n;                 /* samples since the first sound, up to 8000 */
} rade_voice_clip;

void rade_voice_clip_reset(rade_voice_clip *c);
/* iq: n complex samples as (re, im) pairs, in place. */
void rade_voice_clip_run(rade_voice_clip *c, float *iq, int n);

#endif /* RADE_VOICE_H_ */
