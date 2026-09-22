/* hermes-radio-daemon — D-STAR DV voice framing and opt-in encryption.
 * See dstar_voice.h for the scheme.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "dstar_voice.h"

/* The fixed pattern D-STAR scrambles every frame's slow-data bytes with. */
static const uint8_t SLOW_SCRAMBLE[3] = {0x70U, 0x4FU, 0x93U};
/* Data sync, sent unscrambled in the slow-data field of slot 0. */
static const uint8_t DATA_SYNC[3] = {0x55U, 0x2DU, 0x16U};
/* The standard D-STAR null (silence) AMBE frame, FEC included. */
static const uint8_t NULL_AMBE[9] = {0x9EU, 0x8DU, 0x32U, 0x88U, 0x26U, 0x1AU, 0x3FU, 0x61U, 0xE8U};

/* Slow-data units: slots 1..20 carry ten 6-byte units, unit k in slots
 * 2k+1 and 2k+2. Units 0..8 carry the 41-byte header; the sync block rides
 * in unit 8's four bytes after header[40] and in all five of unit 9. */
#define SYNC_UNIT_A      8
#define SYNC_UNIT_B      9
#define SYNC_FIRST_SLOT  (2 * SYNC_UNIT_A + 1)   /* 17 */
#define SYNC_LAST_SLOT   (2 * SYNC_UNIT_B + 2)   /* 20 */

/* A synced receiver that sees no sync-block unit at all for this many
 * superframes is hearing a clear over (a missed EOT): drop the sync. */
#define RX_MAX_MISS 8
/* Sync blocks failing the key check before the status reads "bad key". */
#define RX_BAD_KEY_AFTER 3

static bool keystream_xor(const uint8_t r[VOICE_NONCE_RAND_BYTES], uint32_t frame_idx,
                          char ambe_d[DSTAR_VOICE_AMBE_BITS])
{
    uint8_t ks[VOICE_BLOCK_BYTES];
    if (!voice_crypto_block(VOICE_DOMAIN_VOICE, r, frame_idx, ks))
        return false;
    for (int i = 0; i < DSTAR_VOICE_AMBE_BITS; i++)
        if (i != DSTAR_VOICE_SPARE_BIT)
            ambe_d[i] ^= (char) ((ks[i >> 3] >> (7 - (i & 7))) & 1);
    return true;
}

static bool sync_check(const uint8_t r[VOICE_NONCE_RAND_BYTES], uint16_t sf, uint8_t *check)
{
    uint8_t blk[VOICE_BLOCK_BYTES];
    if (!voice_crypto_block(VOICE_DOMAIN_CHECK, r, sf, blk))
        return false;
    *check = blk[0];
    return true;
}

/* ── TX ─────────────────────────────────────────────────────────── */

static void tx_make_sync(dstar_voice_tx *t)
{
    memcpy(t->sync, t->r, VOICE_NONCE_RAND_BYTES);
    t->sync[6] = (uint8_t) (t->sf >> 8);
    t->sync[7] = (uint8_t) t->sf;
    if (!sync_check(t->r, (uint16_t) t->sf, &t->sync[8]))
        t->fail_closed = true;   /* the key vanished mid-over */
}

void dstar_voice_tx_begin(dstar_voice_tx *t, const char *mycall8, const char *urcall8,
                          bool encrypt)
{
    memset(t, 0, sizeof(*t));

    if (encrypt) {
        if (voice_crypto_have_key() && voice_crypto_new_nonce(t->r)) {
            t->encrypt = true;
        } else {
            t->fail_closed = true;
            fprintf(stderr, "DSTAR tx: encryption requested but no voice key is loaded -- "
                            "sending silence, not clear voice\n");
        }
    }

    t->header[0] = 0x10;   /* DV voice, repeater off */
    t->header[2] = t->encrypt ? DSTAR_HDR_FLAG3_ENCRYPTED : 0x00;
    memcpy(t->header + 3, mycall8, 8);
    memcpy(t->header + 11, mycall8, 8);
    memcpy(t->header + 19, urcall8, 8);
    memcpy(t->header + 27, mycall8, 8);
    t->header[35] = 'A';
    sbitx_dstar_header_finalize(t->header);

    if (t->encrypt)
        tx_make_sync(t);
}

/* Fill a frame's 3 slow-data bytes.
 *
 * Frame 0 of each superframe carries the data sync, unscrambled, exactly as
 * the receiver looks for it. The other 20 frames carry the header as 6-byte
 * units spanning 2 frames each -- a type byte (0x55: header segment, five
 * data bytes) followed by five header bytes -- scrambled with the fixed
 * 0x70 0x4F 0x93 pattern. Nine units cover the 41-byte header; anything past
 * it is 0x66 filler, which is what a real rig sends (verified against the
 * IC-7100's own stream) -- unless the over is encrypted, when the filler
 * bytes carry the sync block. */
static void tx_slow_fill(const dstar_voice_tx *t, uint8_t *out3)
{
    if (t->slot == 0) {
        memcpy(out3, DATA_SYNC, 3);
        return;
    }

    const int unit = (t->slot - 1) / 2;
    const int half = (t->slot - 1) % 2;

    uint8_t u[6];
    u[0] = 0x55U;
    for (int i = 0; i < 5; i++) {
        int off = unit * 5 + i;
        u[1 + i] = (off < SBITX_DSTAR_HEADER_BYTES) ? t->header[off] : 0x66U;
    }

    if (t->encrypt) {
        if (unit == SYNC_UNIT_A) {
            memcpy(u + 2, t->sync, 4);          /* u[1] is header[40] */
        } else if (unit == SYNC_UNIT_B) {
            u[0] = DSTAR_VC_SLOW_TYPE;
            memcpy(u + 1, t->sync + 4, 5);
        }
    }

    for (int i = 0; i < 3; i++)
        out3[i] = u[half * 3 + i] ^ SLOW_SCRAMBLE[i];
}

void dstar_voice_tx_frame(dstar_voice_tx *t, const char ambe_d[DSTAR_VOICE_AMBE_BITS],
                          uint8_t frame[SBITX_DSTAR_FRAME_BYTES])
{
    /* Wire layout is 9 bytes of AMBE followed by 3 bytes of sync-or-slow-data
     * -- NOT sync first. The old order put the sync where the voice belongs
     * and repeated it in every frame, so a receiving rig saw neither valid
     * voice nor a superframe structure. Confirmed from the air: every frame
     * the IC-7100 sends ends with 55 2D 16 only once per 21 frames. */
    if (t->fail_closed) {
        memcpy(frame, NULL_AMBE, sizeof(NULL_AMBE));
    } else {
        char bits[DSTAR_VOICE_AMBE_BITS];
        char fr[4][24];
        memcpy(bits, ambe_d, sizeof(bits));
        if (t->encrypt && !keystream_xor(t->r, t->sf * DSTAR_VOICE_SF_FRAMES + (uint32_t) t->slot, bits)) {
            t->fail_closed = true;
            memcpy(frame, NULL_AMBE, sizeof(NULL_AMBE));
        } else {
            mbe_encodeAmbe3600x2400Frame(bits, fr);
            mbe_encodeDStarDVData((const char(*)[24])fr, frame);
        }
    }
    tx_slow_fill(t, frame + 9);

    t->slot++;
    if (t->slot >= DSTAR_VOICE_SF_FRAMES) {
        t->slot = 0;
        t->sf++;
        if (t->encrypt) {
            /* The sync block carries 16 bits of superframe counter: start a
             * fresh nonce before it wraps (~7.6 h into one over) rather
             * than reuse keystream. */
            if (t->sf > 0xFFFFU) {
                t->sf = 0;
                if (!voice_crypto_new_nonce(t->r))
                    t->fail_closed = true;
            }
            tx_make_sync(t);
        }
    }
}

/* ── RX ─────────────────────────────────────────────────────────── */

void dstar_voice_rx_reset(dstar_voice_rx *r)
{
    memset(r, 0, sizeof(*r));
    r->status = DSTAR_VC_CLEAR;
}

void dstar_voice_rx_header(dstar_voice_rx *r, const uint8_t header[SBITX_DSTAR_HEADER_BYTES])
{
    r->hdr_enc = (header[2] & DSTAR_HDR_FLAG3_ENCRYPTED) != 0;
    /* A header that says "clear" means a clear over began: never keep
     * decrypting it with an earlier over's keystream. A header that says
     * "encrypted" keeps the sync -- this over's sync blocks correct the
     * nonce if it is in fact a new over. */
    if (!r->hdr_enc) {
        r->synced = false;
        r->enc_seen = false;
    }
}

static void rx_status_unsynced(dstar_voice_rx *r)
{
    if (!r->hdr_enc && !r->enc_seen)
        r->status = DSTAR_VC_CLEAR;
    else if (!voice_crypto_have_key())
        r->status = DSTAR_VC_NO_KEY;
    else if (r->bad >= RX_BAD_KEY_AFTER)
        r->status = DSTAR_VC_BAD_KEY;
    else
        r->status = DSTAR_VC_ACQUIRING;
}

/* Slots 17..20 of the current superframe are in: look for a sync block. */
static void rx_try_sync(dstar_voice_rx *r)
{
    uint8_t ua[6], ub[6], sync[DSTAR_VC_SYNC_BYTES];
    memcpy(ua, r->slow[0], 3); memcpy(ua + 3, r->slow[1], 3);
    memcpy(ub, r->slow[2], 3); memcpy(ub + 3, r->slow[3], 3);

    if (ub[0] != DSTAR_VC_SLOW_TYPE)
        return;

    r->enc_seen = true;
    r->miss = 0;
    memcpy(sync, ua + 2, 4);
    memcpy(sync + 4, ub + 1, 5);

    const uint16_t sf = (uint16_t) ((sync[6] << 8) | sync[7]);
    uint8_t check;
    if (!sync_check(sync, sf, &check) || check != sync[8]) {
        if (r->bad < 0xFFU)
            r->bad++;
        return;
    }

    r->bad = 0;
    memcpy(r->r, sync, VOICE_NONCE_RAND_BYTES);
    r->sf = sf;
    r->synced = true;
}

bool dstar_voice_rx_frame(dstar_voice_rx *r, const uint8_t frame[SBITX_DSTAR_FRAME_BYTES],
                          uint16_t fc, char ambe_d[DSTAR_VOICE_AMBE_BITS],
                          mbe_process_result *res)
{
    char fr[4][24];

    if (fc == 0) {
        /* A new superframe. A missed data sync lets fc run past 20, so
         * advance by however many superframes actually went by. */
        if (r->synced) {
            r->sf += (uint32_t) (r->last_fc / DSTAR_VOICE_SF_FRAMES) + 1U;
            if (++r->miss > RX_MAX_MISS) {
                r->synced = false;
                r->enc_seen = false;
            }
        }
        r->slow_mask = 0;
    }
    r->last_fc = fc;

    if (fc >= SYNC_FIRST_SLOT && fc <= SYNC_LAST_SLOT) {
        for (int i = 0; i < 3; i++)
            r->slow[fc - SYNC_FIRST_SLOT][i] = frame[9 + i] ^ SLOW_SCRAMBLE[i];
        r->slow_mask |= (uint8_t) (1U << (fc - SYNC_FIRST_SLOT));
        if (fc == SYNC_LAST_SLOT && r->slow_mask == 0x0FU)
            rx_try_sync(r);
    }

    if (res != NULL)
        memset(res, 0, sizeof(*res));
    mbe_decodeDStarDVData(frame, fr);
    mbe_decodeAmbe3600x2400Frame((const char(*)[24])fr, ambe_d, res);

    if (r->synced) {
        if (keystream_xor(r->r, r->sf * DSTAR_VOICE_SF_FRAMES + fc, ambe_d)) {
            r->status = DSTAR_VC_DECRYPTING;
            return true;
        }
        r->synced = false;   /* the key went away */
    }

    rx_status_unsynced(r);
    return r->status == DSTAR_VC_CLEAR;
}
