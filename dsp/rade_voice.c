/* hermes-radio-daemon — opt-in encryption for RADE V2 digital voice
 * (see rade_voice.h)
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <string.h>

#include <openssl/crypto.h>

#include "rade_voice.h"

/* 16 keystream bytes per carrier: 4 blocks of 64 for 14 carriers. */
#define BLOCKS_PER_FRAME 4

typedef struct { double ar, ai, br, bi; } su2;   /* [[a, -b*], [b, a*]] */

/* Two uniform 32-bit words to two standard normals (Box-Muller). The
 * transform is analogue, so libm's last-bit differences between stations
 * only perturb U by ~1e-16, far below the channel's noise. */
static void normals(const uint8_t w[8], double out[2])
{
    uint32_t u1, u2;
    memcpy(&u1, w, 4);
    memcpy(&u2, w + 4, 4);
    double x1 = ((double) u1 + 1.0) / 4294967297.0;   /* (0, 1] */
    double x2 = (double) u2 / 4294967296.0;           /* [0, 1) */
    double m = sqrt(-2.0 * log(x1));
    out[0] = m * cos(2.0 * M_PI * x2);
    out[1] = m * sin(2.0 * M_PI * x2);
}

/* Haar-uniform SU(2): a normalised 4-vector of normals is a uniform unit
 * quaternion. */
static bool frame_unitaries(const uint8_t r[VOICE_NONCE_RAND_BYTES],
                            uint32_t frame, su2 U[RADE_VOICE_NC])
{
    uint8_t ks[BLOCKS_PER_FRAME * VOICE_BLOCK_BYTES];
    for (uint32_t b = 0; b < BLOCKS_PER_FRAME; b++)
        if (!voice_crypto_block(VOICE_DOMAIN_RADE_LATENT, r, frame * BLOCKS_PER_FRAME + b,
                                ks + b * VOICE_BLOCK_BYTES)) {
            OPENSSL_cleanse(ks, sizeof(ks));
            return false;
        }

    for (int c = 0; c < RADE_VOICE_NC; c++) {
        double g[4];
        normals(ks + 16 * c, g);
        normals(ks + 16 * c + 8, g + 2);
        double n = sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2] + g[3] * g[3]);
        if (n < 1e-12)   /* all four normals ~0: never, but keep U unitary */
            U[c] = (su2) {1, 0, 0, 0};
        else
            U[c] = (su2) {g[0] / n, g[1] / n, g[2] / n, g[3] / n};
    }
    OPENSSL_cleanse(ks, sizeof(ks));
    return true;
}

bool rade_voice_scramble(float z[RADE_VOICE_LATENT],
                         const uint8_t r[VOICE_NONCE_RAND_BYTES],
                         uint32_t frame, bool inverse)
{
    su2 U[RADE_VOICE_NC];
    if (!frame_unitaries(r, frame, U))
        return false;

    for (int c = 0; c < RADE_VOICE_NC; c++) {
        float *p0 = &z[(0 * RADE_VOICE_NC + c) * 2];
        float *p1 = &z[(1 * RADE_VOICE_NC + c) * 2];
        double x0r = p0[0], x0i = p0[1], x1r = p1[0], x1i = p1[1];
        double ar = U[c].ar, ai = U[c].ai, br = U[c].br, bi = U[c].bi;
        double y0r, y0i, y1r, y1i;
        if (!inverse) {
            /* y0 = a x0 - b* x1, y1 = b x0 + a* x1 */
            y0r = ar * x0r - ai * x0i - (br * x1r + bi * x1i);
            y0i = ar * x0i + ai * x0r - (br * x1i - bi * x1r);
            y1r = br * x0r - bi * x0i + (ar * x1r + ai * x1i);
            y1i = br * x0i + bi * x0r + (ar * x1i - ai * x1r);
        } else {
            /* U^H = [[a*, b*], [-b, a]] */
            y0r = ar * x0r + ai * x0i + (br * x1r + bi * x1i);
            y0i = ar * x0i - ai * x0r + (br * x1i - bi * x1r);
            y1r = -(br * x0r - bi * x0i) + (ar * x1r - ai * x1i);
            y1i = -(br * x0i + bi * x0r) + (ar * x1i + ai * x1r);
        }
        p0[0] = (float) y0r; p0[1] = (float) y0i;
        p1[0] = (float) y1r; p1[1] = (float) y1i;
    }
    OPENSSL_cleanse(U, sizeof(U));
    return true;
}
