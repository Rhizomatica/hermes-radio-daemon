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
 * The frame counter and per-over nonce come from the caller; how a
 * receiver learns them on air is the signalling's job, not this file's.
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

#endif /* RADE_VOICE_H_ */
