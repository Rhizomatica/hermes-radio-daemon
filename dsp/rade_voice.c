/* hermes-radio-daemon — opt-in encryption for RADE V2 digital voice
 * (see rade_voice.h)
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
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

/* ── signalling (see rade_voice.h) ─────────────────────────────────── */

#define MSG_BYTES   12                 /* r[6], gray(j) 12 bits, check 20 bits, crc[2] */
#define MSG_BITS    (MSG_BYTES * 8)
#define GRAY_BIT0   (6 * 8 + 11)       /* message bit of gray(j)'s bit 0 */
#define CHECK_MASK  0xFFFFFU           /* the keyed check, 20 bits */
#define CONV_K      7
#define CODED_BITS  (2 * (MSG_BITS + CONV_K - 1))   /* 204 */
#define MSG_SYMS    (RADE_VOICE_MSG_FRAMES * RADE_VOICE_NS)
#define ILV_STEP    77                 /* coprime with RADE_VOICE_MSG_SLOTS */
#define GAP_RESET   100                /* symbols (2 s) without a call: new over */
#define J_BITS      12                 /* message index j < 4096 (65 min) */
#define COMBINE     4                  /* messages combined, at most */
#define RX_DECIDE   10                 /* symbols before an over counts as clear */
#define RX_AUX_ON   0.6f               /* aux power against the data carriers: on (~0.9 when on) */
#define RX_AUX_OFF  0.45f              /* and back off (~0.2-0.35 when off) */
#define RX_AUX_AVG  50                 /* symbols the power test averages, ~1 s */
#define TRACK_SPAN  4                  /* symbols either side a locked receiver looks */
#define TRACK_GOOD  0.25f              /* normalised correlation: the message is there */
#define TRACK_GONE  0.10f              /* below: not this over's message, mute now */
#define TRACK_LOST  3                  /* messages missing in a row: unlock */
_Static_assert(CODED_BITS <= RADE_VOICE_MSG_SLOTS, "message does not fit");
_Static_assert(RADE_VOICE_RING >= COMBINE * MSG_SYMS, "ring shorter than the messages combined");

static uint16_t crc16_from(uint16_t c, const uint8_t *p, int n)   /* CRC-16/CCITT */
{
    for (int i = 0; i < n; i++) {
        c ^= (uint16_t) (p[i] << 8);
        for (int b = 0; b < 8; b++)
            c = (uint16_t) (c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1);
    }
    return c;
}
#define crc16(p, n) crc16_from(0xFFFF, p, n)

/* The keyed check on the nonce, the same in every message of the over (so
 * messages combine). 20 bits: a receiver with another key takes an over
 * for its own once in ~10^6. */
static bool msg_check(const uint8_t r[VOICE_NONCE_RAND_BYTES], uint32_t *out)
{
    uint8_t ks[VOICE_BLOCK_BYTES];
    bool ok = voice_crypto_block(VOICE_DOMAIN_RADE_SIGNAL, r, 0, ks);
    *out = ((uint32_t) ks[0] << 16 | (uint32_t) ks[1] << 8 | ks[2]) & CHECK_MASK;
    OPENSSL_cleanse(ks, sizeof(ks));
    return ok;
}

static int parity(unsigned x)
{
    return __builtin_parity(x);
}

/* K=7, (171, 133) octal; the register's newest bit is bit 6. Linear:
 * coding m ^ d gives the coding of m ^ the coding of d. */
#define POLY_A 0x79U
#define POLY_B 0x5BU

static void conv_encode(const uint8_t msg[MSG_BYTES], uint8_t coded[CODED_BITS])
{
    unsigned sr = 0;
    for (int i = 0; i < MSG_BITS + CONV_K - 1; i++) {
        unsigned bit = i < MSG_BITS ? (msg[i / 8] >> (7 - i % 8)) & 1U : 0U;
        sr = ((sr >> 1) | (bit << (CONV_K - 1))) & 0x7FU;
        coded[2 * i] = (uint8_t) parity(sr & POLY_A);
        coded[2 * i + 1] = (uint8_t) parity(sr & POLY_B);
    }
}

/* Soft Viterbi: soft[i] > 0 means coded bit i is 0. The state is the
 * encoder register's six newest bits (newest at bit 5); a step with input
 * bit b makes the register s | b << 6 and the next state s >> 1 | b << 5,
 * so state ns is reached from (ns << 1) & 63 and that | 1, with input
 * bit ns >> 5. */
static void viterbi(const float soft[CODED_BITS], uint8_t msg[MSG_BYTES])
{
    enum { NST = 1 << (CONV_K - 1), STEPS = MSG_BITS + CONV_K - 1 };
    static int8_t ea[2 * NST], eb[2 * NST];   /* +-1 per register value */
    static bool tables;
    if (!tables) {
        for (unsigned reg = 0; reg < 2 * NST; reg++) {
            ea[reg] = (int8_t) (parity(reg & POLY_A) ? -1 : 1);
            eb[reg] = (int8_t) (parity(reg & POLY_B) ? -1 : 1);
        }
        tables = true;
    }
    float m[NST], nm[NST];
    uint64_t from[STEPS];    /* per state: which predecessor survived */
    for (int st = 0; st < NST; st++)
        m[st] = st == 0 ? 0.0f : -1e30f;
    for (int i = 0; i < STEPS; i++) {
        const float sa = soft[2 * i], sb = soft[2 * i + 1];
        uint64_t f = 0;
        const int top = i < MSG_BITS ? NST : NST / 2;   /* the tail is zeros */
        for (int ns = 0; ns < top; ns++) {
            const unsigned bit = (unsigned) ns >> (CONV_K - 2);
            const unsigned p0 = ((unsigned) ns << 1) & (NST - 1), p1 = p0 | 1U;
            const unsigned r0 = p0 | bit << (CONV_K - 1), r1 = p1 | bit << (CONV_K - 1);
            const float v0 = m[p0] + ea[r0] * sa + eb[r0] * sb;
            const float v1 = m[p1] + ea[r1] * sa + eb[r1] * sb;
            if (v1 > v0) {
                nm[ns] = v1;
                f |= 1ULL << ns;
            } else {
                nm[ns] = v0;
            }
        }
        for (int ns = top; ns < NST; ns++)
            nm[ns] = -1e30f;
        from[i] = f;
        memcpy(m, nm, sizeof(m));
    }
    memset(msg, 0, MSG_BYTES);
    unsigned st = 0;                     /* the tail ends in state 0 */
    for (int i = STEPS - 1; i >= 0; i--) {
        if (i < MSG_BITS && (st >> (CONV_K - 2) & 1U))
            msg[i / 8] |= (uint8_t) (1U << (7 - i % 8));
        st = ((st << 1) & (NST - 1)) | (unsigned) (from[i] >> st & 1U);
    }
}

static int ilv(int i)
{
    return (i * ILV_STEP) % RADE_VOICE_MSG_SLOTS;
}

static uint16_t gray(uint32_t j)
{
    return (uint16_t) (j ^ (j >> 1));
}

static void msg_build(const uint8_t r[VOICE_NONCE_RAND_BYTES], uint32_t j, uint32_t chk,
                      uint8_t msg[MSG_BYTES])
{
    const uint16_t g = gray(j);
    memcpy(msg, r, VOICE_NONCE_RAND_BYTES);
    msg[6] = (uint8_t) (g >> 4);
    msg[7] = (uint8_t) ((g & 0xFU) << 4 | (chk >> 16 & 0xFU));
    msg[8] = (uint8_t) (chk >> 8);
    msg[9] = (uint8_t) chk;
    uint16_t c = crc16(msg, 10);
    msg[10] = (uint8_t) (c >> 8);
    msg[11] = (uint8_t) c;
}

static void tx_new_message(rade_voice_tx *t, uint32_t j)
{
    uint8_t msg[MSG_BYTES], coded[CODED_BITS];
    uint32_t chk;
    if (!msg_check(t->r, &chk))
        t->fail_closed = true;           /* the key vanished mid-over */
    msg_build(t->r, j, chk, msg);
    conv_encode(msg, coded);
    for (int i = 0; i < RADE_VOICE_MSG_SLOTS; i++)
        t->slots[i] = (uint8_t) (i & 1);   /* padding */
    for (int i = 0; i < CODED_BITS; i++)
        t->slots[ilv(i)] = coded[i];
}

bool rade_voice_tx_begin(rade_voice_tx *t, bool encrypt, float aux_amp)
{
    memset(t, 0, sizeof(*t));
    if (!encrypt)
        return true;
    t->gain = aux_amp;
    for (int a = 0; a < RADE_VOICE_NAUX; a++)
        t->ref[a][0] = 1.0f;
    if (!voice_crypto_have_key() || !voice_crypto_new_nonce(t->r)) {
        t->fail_closed = true;
        return false;
    }
    t->encrypt = true;
    return true;
}

void rade_voice_tx_frame(rade_voice_tx *t, float z[RADE_VOICE_LATENT], float aux[RADE_VOICE_AUX])
{
    if (t->fail_closed) {
        memset(z, 0, RADE_VOICE_LATENT * sizeof(float));
        return;
    }
    if (!t->encrypt)
        return;
    if (t->k % RADE_VOICE_MSG_FRAMES == 0) {
        uint32_t j = t->k / RADE_VOICE_MSG_FRAMES;
        if (j >= (1U << J_BITS)) {       /* the index rolls over: a new nonce */
            if (!voice_crypto_new_nonce(t->r)) {
                t->fail_closed = true;
                memset(z, 0, RADE_VOICE_LATENT * sizeof(float));
                return;
            }
            t->k = 0;
            j = 0;
        }
        tx_new_message(t, j);
    }
    /* the aux carriers follow the data carriers' RMS (the scale of the
     * latent is the encoder's, not 1) */
    float p = 0;
    for (int i = 0; i < RADE_VOICE_LATENT; i++)
        p += z[i] * z[i];
    p /= RADE_VOICE_NC * RADE_VOICE_NS;
    t->amp = t->amp > 0 ? 0.9f * t->amp + 0.1f * sqrtf(p) : sqrtf(p);
    if (!rade_voice_scramble(z, t->r, t->k, false)) {
        t->fail_closed = true;
        memset(z, 0, RADE_VOICE_LATENT * sizeof(float));
        return;
    }

    /* the frame's aux symbols: differential QPSK, the slot's two bits
     * choosing the quadrant of the phase step */
    const int f = (int) (t->k % RADE_VOICE_MSG_FRAMES);
    for (int s = 0; s < RADE_VOICE_NS; s++)
        for (int a = 0; a < RADE_VOICE_NAUX; a++) {
            const int slot = ((f * RADE_VOICE_NS + s) * RADE_VOICE_NAUX + a) * 2;
            const float dr = t->slots[slot] ? -(float) M_SQRT1_2 : (float) M_SQRT1_2;
            const float di = t->slots[slot + 1] ? -(float) M_SQRT1_2 : (float) M_SQRT1_2;
            float yr = t->ref[a][0] * dr - t->ref[a][1] * di;
            float yi = t->ref[a][0] * di + t->ref[a][1] * dr;
            const float mag = sqrtf(yr * yr + yi * yi);
            if (mag > 0) {               /* unit phasor: no drift */
                yr /= mag;
                yi /= mag;
            }
            t->ref[a][0] = yr;
            t->ref[a][1] = yi;
            aux[(s * RADE_VOICE_NAUX + a) * 2] = yr * t->amp * t->gain;
            aux[(s * RADE_VOICE_NAUX + a) * 2 + 1] = yi * t->amp * t->gain;
        }
    t->k++;
}

/* Combining: consecutive messages differ only in gray(j), by one bit, at
 * tz(j) (trailing zeros) between messages j-1 and j, and in the CRC that
 * follows from it. flip[t] is the coded difference of flipping gray bit t:
 * a sign mask that maps message j-1's soft bits onto message j's. */
static int8_t flip[J_BITS][CODED_BITS];
static bool flip_ready;

static void flip_init(void)
{
    if (flip_ready)
        return;
    for (int t = 0; t < J_BITS; t++) {
        uint8_t d[MSG_BYTES] = {0}, coded[CODED_BITS];
        const int bit = GRAY_BIT0 - t;
        d[bit / 8] = (uint8_t) (1U << (7 - bit % 8));
        uint16_t c = crc16_from(0, d, 10);  /* the CRC's linear part */
        d[10] = (uint8_t) (c >> 8);
        d[11] = (uint8_t) c;
        conv_encode(d, coded);
        for (int i = 0; i < CODED_BITS; i++)
            flip[t][i] = (int8_t) (coded[i] ? -1 : 1);
    }
    flip_ready = true;
}

static int tz(uint32_t j)
{
    return j ? __builtin_ctz(j) : 32;
}

void rade_voice_rx_reset(rade_voice_rx *r)
{
    memset(r, 0, sizeof(*r));
    r->status = RADE_VC_ACQUIRING;      /* until the first symbols say */
}

static void rx_status(rade_voice_rx *r)
{
    if (r->locked && !r->hold)
        r->status = RADE_VC_DECRYPTING;
    else if (r->locked)
        r->status = RADE_VC_ACQUIRING;  /* the expected message is not there */
    else if (!r->enc_seen && r->n_sym < RX_DECIDE)
        r->status = RADE_VC_ACQUIRING;  /* not yet known: muted */
    else if (!r->enc_seen)
        r->status = RADE_VC_CLEAR;
    else if (!voice_crypto_have_key())
        r->status = RADE_VC_NO_KEY;
    else if (r->bad_key)
        r->status = RADE_VC_BAD_KEY;
    else
        r->status = RADE_VC_ACQUIRING;
}

/* Soft bits of the message ending at symbol end; returns how many were
 * received (the rest are 0: erased). */
static int rx_soft(const rade_voice_rx *r, uint32_t end, float soft[CODED_BITS])
{
    const float scale = r->d_pow > 0 ? 1.0f / r->d_pow : 1.0f;
    int have = 0;
    for (int i = 0; i < CODED_BITS; i++) {
        const int slot = ilv(i);
        const int b = slot & 1, a = (slot >> 1) % RADE_VOICE_NAUX;
        const int q = (slot >> 1) / RADE_VOICE_NAUX;          /* symbol in the message */
        const uint32_t at = end - (uint32_t) (MSG_SYMS - 1) + (uint32_t) q;
        const int ri = (int) (at % RADE_VOICE_RING);
        if (r->d_sym[ri] == at + 1) {
            soft[i] = r->d[ri][a][b] * scale;
            have++;
        } else {
            soft[i] = 0.0f;
        }
    }
    return have;
}

/* A message ends at sym: an encrypted over, and where its messages end. */
static void rx_message_at(rade_voice_rx *r, uint32_t sym)
{
    r->enc_seen = true;
    r->msgs++;
    r->align_sym = sym;
    r->aligned = true;
}

/* A decoded message: CRC, then whether its j fits the combining it was
 * decoded with (t[n]: the gray bit flipped between messages j-n and j-n-1,
 * n < ncomb), then the key. Returns true when it locked. */
static bool rx_accept(rade_voice_rx *r, const uint8_t msg[MSG_BYTES], uint32_t sym,
                      const int *t, int ncomb)
{
    const uint16_t c = crc16(msg, 10);
    if (msg[10] != (uint8_t) (c >> 8) || msg[11] != (uint8_t) c)
        return false;
    uint32_t g = (uint32_t) msg[6] << 4 | msg[7] >> 4, j = 0;
    for (; g; g >>= 1)
        j ^= g;                          /* gray to binary */
    if (j >= (1U << J_BITS))
        return false;
    for (int n = 0; n < ncomb; n++)
        if (j < (uint32_t) n + 1 || tz(j - (uint32_t) n) != t[n])
            return false;                /* not the combining it was decoded with */

    uint32_t want;
    if (!msg_check(msg, &want)) {
        rx_message_at(r, sym);           /* no key: the status says so */
        return false;
    }
    if (want != ((uint32_t) (msg[7] & 0xFU) << 16 | (uint32_t) msg[8] << 8 | msg[9])) {
        /* Another key, or a false decode: the combining search tries a few
         * hundred patterns at every symbol, so now and then one passes the
         * CRC by chance. That one must not move the alignment, or the
         * search keeps to the wrong symbols for the rest of the over. Two
         * a message apart are another key's. */
        r->msgs_bad++;
        if (r->bad_checks > 0 && ((sym - r->bad_sym) % MSG_SYMS) == 0)
            r->bad_checks++;
        else
            r->bad_checks = 1;
        r->bad_sym = sym;
        if (r->bad_checks >= 2) {
            r->bad_key = true;
            r->locked = false;
            rx_message_at(r, sym);
        }
        return false;
    }
    rx_message_at(r, sym);
    r->bad_checks = 0;
    r->bad_key = false;
    r->hold = false;
    r->track_miss = 0;
#ifdef RADE_VOICE_DEBUG
    fprintf(stderr, "rade_voice: message at sym %u, j %u, %d combined%s\n", sym, j, ncomb + 1,
            r->locked ? (r->ref_k + (sym - r->ref_sym) / 2 == (j + 1) * RADE_VOICE_MSG_FRAMES - 1 ?
                         " (as tracked)" : " (MOVED)") : " (lock)");
#endif
    memcpy(r->r, msg, VOICE_NONCE_RAND_BYTES);
    r->ref_sym = sym;
    r->ref_k = (j + 1) * RADE_VOICE_MSG_FRAMES - 1;
    r->locked = true;
    return true;
}

/* Locked: the next message is known exactly (nonce, index, check), so
 * correlate its coded bits with what arrived, at the expected end and up
 * to TRACK_SPAN symbols either side. The receiver's symbol counter can slip
 * when RADE loses sync and acquires again; this follows it. Called
 * TRACK_SPAN symbols after an expected end. */
static void rx_track(rade_voice_rx *r, uint32_t sym)
{
    const uint32_t end = sym - TRACK_SPAN;
    const uint32_t frame = r->ref_k + (uint32_t) ((int32_t) (end - r->ref_sym) / 2);
    const uint32_t j = frame / RADE_VOICE_MSG_FRAMES;
    if (j >= (1U << J_BITS)) {
        r->locked = false;               /* the transmitter has a new nonce */
        r->aligned = false;
        return;
    }
    uint8_t msg[MSG_BYTES], coded[CODED_BITS];
    uint32_t chk;
    if (!msg_check(r->r, &chk)) {
        r->locked = false;
        return;
    }
    msg_build(r->r, j, chk, msg);
    conv_encode(msg, coded);

    float best = -1e30f;
    int best_d = 0;
    float at0 = 0;
    for (int d = -TRACK_SPAN; d <= TRACK_SPAN; d++) {
        float soft[CODED_BITS], c = 0, n = 0;
        rx_soft(r, end + (uint32_t) d, soft);
        for (int i = 0; i < CODED_BITS; i++) {
            c += coded[i] ? -soft[i] : soft[i];
            n += fabsf(soft[i]);
        }
        c = n > 0 ? c / n : 0;
        if (d == 0)
            at0 = c;
        if (c > best) {
            best = c;
            best_d = d;
        }
    }
    if (best < TRACK_GOOD) {
        if (best < TRACK_GONE)
            r->hold = true;              /* another over, or none: mute */
        if (++r->track_miss >= TRACK_LOST) {
            r->locked = false;           /* gone, or another over: search */
            r->aligned = false;
            r->track_miss = 0;
        }
        return;
    }
    r->track_miss = 0;
    r->hold = false;
    if (best_d != 0 && best > at0 + 0.1f) {
#ifdef RADE_VOICE_DEBUG
        fprintf(stderr, "rade_voice: slip of %+d symbols at sym %u (%.2f against %.2f)\n",
                best_d, sym, (double) best, (double) at0);
#endif
        r->ref_sym += (uint32_t) best_d;
        r->align_sym += (uint32_t) best_d;
    }
}

/* Is there a message whose last symbol is sym? Alone, or with up to three
 * before it, for every pattern of gray flips they can differ by. */
static void rx_try_message(rade_voice_rx *r, uint32_t sym)
{
    float v[COMBINE][CODED_BITS], acc[CODED_BITS];
    int have[COMBINE];
    uint8_t msg[MSG_BYTES];
    flip_init();
    for (int n = 0; n < COMBINE; n++)
        have[n] = rx_soft(r, sym - (uint32_t) (n * MSG_SYMS), v[n]);
    if (have[0] < CODED_BITS / 2)
        return;

    viterbi(v[0], msg);
    if (rx_accept(r, msg, sym, NULL, 0))
        return;
    if (!voice_crypto_have_key())
        return;                          /* nothing to unlock */

    /* the flips between j and j-1, j-1 and j-2, j-2 and j-3: tz(j), tz(j-1),
     * tz(j-2). Among consecutive integers every other one is odd (tz 0),
     * and of two consecutive even ones one has tz 1. */
    int t[3];
    for (int ncomb = 1; ncomb < COMBINE; ncomb++) {
        if (have[ncomb] < CODED_BITS / 2)
            break;
        for (int a = 0; a < J_BITS; a++)
            for (int b = 0; b < (ncomb >= 2 ? J_BITS : 1); b++)
                for (int c = 0; c < (ncomb >= 3 ? J_BITS : 1); c++) {
                    t[0] = a; t[1] = b; t[2] = c;
                    /* only the patterns tz can make */
                    bool ok = true;
                    for (int n = 0; n + 1 < ncomb; n++)
                        if ((t[n] == 0) == (t[n + 1] == 0))
                            ok = false;  /* odd and even alternate */
                    if (ncomb == 3 && t[1] == 0 && t[0] != 1 && t[2] != 1)
                        ok = false;      /* j and j-2 even: one has tz 1 */
                    if (!ok)
                        continue;
                    memcpy(acc, v[0], sizeof(acc));
                    int8_t mask[CODED_BITS];
                    memset(mask, 1, sizeof(mask));
                    for (int n = 1; n <= ncomb; n++) {
                        for (int i = 0; i < CODED_BITS; i++) {
                            mask[i] = (int8_t) (mask[i] * flip[t[n - 1]][i]);
                            acc[i] += mask[i] * v[n][i];
                        }
                    }
                    viterbi(acc, msg);
                    if (rx_accept(r, msg, sym, t, ncomb))
                        return;
                }
    }
}

int rade_voice_rx_symbol(rade_voice_rx *r, float z[RADE_VOICE_LATENT],
                         const float aux[RADE_VOICE_AUX], uint32_t sym)
{
    if (r->any && (uint32_t) (sym - r->last_sym) > GAP_RESET)
        rade_voice_rx_reset(r);
    if (r->any && sym == r->last_sym)
        return -1;                       /* the same symbol again */
    r->any = true;
    r->last_sym = sym;

    /* the newer symbol against the older, per aux carrier */
    const int ri = (int) (sym % RADE_VOICE_RING);
    float pall = 0;
    for (int c = 0; c < RADE_VOICE_NC; c++) {
        const float *x = &z[(1 * RADE_VOICE_NC + c) * 2];
        pall += x[0] * x[0] + x[1] * x[1];
    }
    pall /= RADE_VOICE_NC;
    const float w5 = r->n_sym < 5 ? 1.0f / (float) (r->n_sym + 1) : 0.2f;
    const float wl = r->n_sym < RX_AUX_AVG ? 1.0f / (float) (r->n_sym + 1) : 1.0f / RX_AUX_AVG;
    r->pall5 += w5 * (pall - r->pall5);
    for (int a = 0; a < RADE_VOICE_NAUX; a++) {
        const float *y0 = &aux[(0 * RADE_VOICE_NAUX + a) * 2];
        const float *y1 = &aux[(1 * RADE_VOICE_NAUX + a) * 2];
        const float dr = y1[0] * y0[0] + y1[1] * y0[1];
        const float di = y1[1] * y0[0] - y1[0] * y0[1];
        /* the steps are (+-1 +-j)/sqrt(2): the real part is the first
         * bit, the imaginary the second */
        r->d[ri][a][0] = dr;
        r->d[ri][a][1] = di;
        r->d_pow = 0.98f * r->d_pow + 0.02f * sqrtf(dr * dr + di * di);

        /* Is the aux carrier on? Against the data carriers' mean power, and
         * only while the channel at the aux carrier is strong, as the data
         * carrier next to it says (62.5 Hz apart, it fades with it): there a
         * clear over's aux bin is plainly noise, an encrypted over's as
         * strong as the data carriers. Powers smoothed over ~5 symbols,
         * shorter than a fade, longer than the symbols' own spread. */
        const int c = a == 0 ? 0 : RADE_VOICE_NC - 1;
        const float *x = &z[(1 * RADE_VOICE_NC + c) * 2];
        const float pa = y1[0] * y1[0] + y1[1] * y1[1], pd = x[0] * x[0] + x[1] * x[1];
        r->pa5[a] += w5 * (pa - r->pa5[a]);
        r->pd5[a] += w5 * (pd - r->pd5[a]);
        r->pd_long[a] += wl * (r->pd5[a] - r->pd_long[a]);
        if (r->pd5[a] >= r->pd_long[a]) {
            r->p_aux += wl * (r->pa5[a] - r->p_aux);
            r->p_data += wl * (r->pall5 - r->p_data);
        }
    }
    r->d_sym[ri] = sym + 1;
    if (r->n_sym < UINT8_MAX)
        r->n_sym++;
    /* A decoded message settles it for the over; until then the test, with
     * hysteresis. Wrongly "clear" plays noise until the lock; wrongly
     * "encrypted" would mute speech. */
    if (r->p_aux > RX_AUX_ON * r->p_data && r->p_data > 0)
        r->aux_on = true;
    else if (r->p_aux < RX_AUX_OFF * r->p_data)
        r->aux_on = false;
    if (r->msgs == 0)
        r->enc_seen = r->aux_on;
#ifdef RADE_VOICE_DEBUG
    if (sym % 25 == 0)
        fprintf(stderr, "rade_voice: ratio %.2f (aux %.1f data %.1f) %s\n",
                r->p_data > 0 ? (double) (r->p_aux / r->p_data) : -1.0, (double) r->p_aux,
                (double) r->p_data, r->aux_on ? "on" : "off");
#endif

    /* Locked: follow the known messages. Else where a message ends, once
     * one has been seen, or wherever one might; not on a clear over (no aux
     * power). */
    if (r->locked) {
        /* a message of another over (a new nonce) takes over at once */
        if (((sym - r->ref_sym) % MSG_SYMS) == 0) {
            float soft[CODED_BITS];
            uint8_t msg[MSG_BYTES];
            if (rx_soft(r, sym, soft) >= CODED_BITS / 2) {
                viterbi(soft, msg);
                if (memcmp(msg, r->r, VOICE_NONCE_RAND_BYTES) != 0)
                    rx_accept(r, msg, sym, NULL, 0);
            }
        }
        if (r->locked && ((sym - r->ref_sym - TRACK_SPAN) % MSG_SYMS) == 0)
            rx_track(r, sym);
    } else if (r->enc_seen && !r->aligned) {
        rx_try_message(r, sym);
    } else if (r->enc_seen && ((sym - r->align_sym) % MSG_SYMS) == 0) {
        /* where the messages ended; after TRACK_LOST with none there (a
         * new over, at other symbols), search everywhere again */
        const uint32_t seen = r->msgs + r->msgs_bad;
        rx_try_message(r, sym);
        if (r->msgs + r->msgs_bad != seen)
            r->align_miss = 0;
        else if (++r->align_miss >= TRACK_LOST && !r->locked) {
            r->aligned = false;
            r->align_miss = 0;
        }
    }
    rx_status(r);

    if (!r->locked)
        return -1;
    if ((sym - r->ref_sym) & 1U)
        return 0;                        /* the other alignment */
    const uint32_t k = r->ref_k + (uint32_t) ((int32_t) (sym - r->ref_sym) / 2);
    if (!rade_voice_scramble(z, r->r, k, true)) {
        r->locked = false;               /* the key went away */
        rx_status(r);
        return -1;
    }
    return 1;
}

bool rade_voice_rx_play(const rade_voice_rx *r)
{
    return r->status == RADE_VC_CLEAR || r->status == RADE_VC_DECRYPTING;
}

/* ── clipper (see rade_voice.h) ────────────────────────────────────── */

static float clip_h[RADE_VOICE_CLIP_TAPS][2];   /* the complex band-pass taps */
static bool clip_h_ready;

static void clip_taps(void)
{
    if (clip_h_ready)
        return;
    const int nt = RADE_VOICE_CLIP_TAPS;
    double h[RADE_VOICE_CLIP_TAPS], sum = 0;
    for (int i = 0; i < nt; i++) {
        double m = i - (nt - 1) / 2.0, x = 2 * 720.0 / 8000;   /* +-720 Hz */
        double w = 0.42 - 0.5 * cos(2 * M_PI * i / (nt - 1)) + 0.08 * cos(4 * M_PI * i / (nt - 1));
        h[i] = (m == 0 ? x : sin(M_PI * x * m) / (M_PI * m)) * w;
        sum += h[i];
    }
    for (int i = 0; i < nt; i++) {   /* moved up to 1500 Hz */
        double ph = 2 * M_PI * 1500.0 * (i - (nt - 1) / 2.0) / 8000;
        clip_h[i][0] = (float) (h[i] / sum * cos(ph));
        clip_h[i][1] = (float) (h[i] / sum * sin(ph));
    }
    clip_h_ready = true;
}

void rade_voice_clip_reset(rade_voice_clip *c)
{
    memset(c, 0, sizeof(*c));
    clip_taps();
}

void rade_voice_clip_run(rade_voice_clip *c, float *iq, int n)
{
    const int nt = RADE_VOICE_CLIP_TAPS;
    const float lim2 = powf(10.0f, RADE_VOICE_CLIP_DB / 10.0f);
    for (int k = 0; k < n; k++) {
        float xr = iq[2 * k], xi = iq[2 * k + 1];
        for (int pass = 0; pass < RADE_VOICE_CLIP_PASSES; pass++) {
            /* each pass against its own input's mean power, slowly (~1 s) */
            const float m2 = xr * xr + xi * xi;
            float *pw = &c->power[pass];
            const float a = c->n < 8000 ? 1.0f / (float) (c->n + 1) : 0.00012f;
            *pw += a * (m2 - *pw);       /* the mean so far, then over ~1 s */
            const float a2 = *pw * lim2;
            if (m2 > a2 && m2 > 0) {
                const float g = sqrtf(a2 / m2);
                xr *= g;
                xi *= g;
            }
            c->hist[pass][c->at][0] = xr;
            c->hist[pass][c->at][1] = xi;
            float yr = 0, yi = 0;
            for (int t = 0; t < nt; t++) {
                const float *x = c->hist[pass][(c->at - t + nt) % nt];
                yr += clip_h[t][0] * x[0] - clip_h[t][1] * x[1];
                yi += clip_h[t][0] * x[1] + clip_h[t][1] * x[0];
            }
            xr = yr;
            xi = yi;
        }
        c->at = (c->at + 1) % nt;
        if (c->n < 8000 && (xr != 0 || xi != 0 || c->n > 0))
            c->n++;                      /* counting from the first sound */
        iq[2 * k] = xr;
        iq[2 * k + 1] = xi;
    }
}
