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
/* Robust lock: the last DSTAR_VC_DIST_HIST (5) checks within this many
 * bits of the expected ones in total. A wrong key gives each 8-bit check 4
 * wrong bits on average, so 40 bits within 3 happens once in ~10^8 (the
 * clean fast lock: once in 65536); the right key passes ~92% of the time
 * at 4% bit errors, ~99% at 2%. */
#define RX_LOCK_MAX_BITS 3
/* Unlock: CUSUM of ln(P_wrong(d) / P_right(d)) for each check distance d,
 * P_wrong = Binomial(8, 0.5), P_right = Binomial(8, 0.04), in tenths of a
 * nat, floored at 0. A wrong key adds ~7 nats a block (unlocks in ~2), the
 * right key at 4% bit errors drifts down and crosses the threshold about
 * once in 10^5 blocks (~12 h of continuous talk). */
static const int16_t RX_LLR[9] = { -52, -20, 11, 43, 75, 107, 130, 140, 150 };
#define RX_CUSUM_UNLOCK  120
/* A check this many bits off counts against the key. The right key at 3%
 * bit errors misses by 3+ bits in ~0.13% of blocks, a wrong key in ~85%.
 * Two such blocks among the last three unlock a locked receiver (right
 * key: once in ~2e5 superframes, ~55 h; wrong key: ~94% per window), and
 * half or more of the recent ones make an unlocked receiver read "bad
 * key". */
#define RX_BAD_BITS      3
/* Votes kept per bit of R before halving, so a new R can win quickly. */
#define RX_VOTE_CAP      16
/* The sync-block type byte, allowing one flipped bit (0x55, the header
 * type, is three bits away). */
#define RX_TYPE_MAX_BITS 1

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

static int popc8(uint8_t x)
{
    return __builtin_popcount(x);
}

static void rx_forget_votes(dstar_voice_rx *r)
{
    memset(r->vote_one, 0, sizeof(r->vote_one));
    r->vote_n = 0;
    r->off_n = 0;
    r->dist_n = 0;
    r->last_exact = false;
    r->bad_key = false;
    r->blk_n = 0;
}

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
        rx_forget_votes(r);
    }
}

/* Of the last `last` blocks (fewer if fewer were seen), how many had a
 * check RX_BAD_BITS or more off; *n receives how many were looked at. */
static int rx_bad_blocks(const dstar_voice_rx *r, int last, int *n)
{
    int have = r->dist_n < DSTAR_VC_DIST_HIST ? r->dist_n : DSTAR_VC_DIST_HIST;
    if (last > have)
        last = have;
    int bad = 0;
    for (int k = 1; k <= last; k++)
        bad += r->dist_hist[(r->dist_n - k) % DSTAR_VC_DIST_HIST] >= RX_BAD_BITS;
    *n = last;
    return bad;
}

/* Once seen, the verdict holds for the rest of the over (until a lock). */
static bool bad_key_evidence(dstar_voice_rx *r)
{
    int n, bad = rx_bad_blocks(r, DSTAR_VC_DIST_HIST, &n);
    if (n >= 2 && 2 * bad >= n)          /* a lock that went bad */
        r->bad_key = true;
    return r->bad_key;
}

static void rx_status_unsynced(dstar_voice_rx *r)
{
    if (!r->hdr_enc && !r->enc_seen)
        r->status = DSTAR_VC_CLEAR;
    else if (!voice_crypto_have_key())
        r->status = DSTAR_VC_NO_KEY;
    else if (bad_key_evidence(r))
        r->status = DSTAR_VC_BAD_KEY;
    else
        r->status = DSTAR_VC_ACQUIRING;
}

/* Add a block's R to the vote and write the current majority to out. A tie
 * takes the block's own bit. */
static void rx_vote_r(dstar_voice_rx *r, const uint8_t *rb, uint8_t out[VOICE_NONCE_RAND_BYTES])
{
    if (r->vote_n >= RX_VOTE_CAP) {
        for (int i = 0; i < VOICE_NONCE_RAND_BYTES * 8; i++)
            r->vote_one[i] = (uint8_t) ((r->vote_one[i] + 1) / 2);
        r->vote_n = RX_VOTE_CAP / 2;
    }
    r->vote_n++;
    memset(out, 0, VOICE_NONCE_RAND_BYTES);
    for (int i = 0; i < VOICE_NONCE_RAND_BYTES * 8; i++) {
        int bit = (rb[i >> 3] >> (7 - (i & 7))) & 1;
        r->vote_one[i] = (uint8_t) (r->vote_one[i] + bit);
        int two = 2 * r->vote_one[i];
        int maj = two > r->vote_n ? 1 : two < r->vote_n ? 0 : bit;
        out[i >> 3] |= (uint8_t) (maj << (7 - (i & 7)));
    }
}

/* Add a block's counter offset and return the most common recent offset
 * (the block's own when none repeats yet). */
static uint16_t rx_vote_off(dstar_voice_rx *r, uint16_t off)
{
    r->off_hist[r->off_n % DSTAR_VC_OFF_HIST] = off;
    r->off_n++;
    int n = r->off_n < DSTAR_VC_OFF_HIST ? r->off_n : DSTAR_VC_OFF_HIST;
    uint16_t best = off;
    int best_c = 1;
    for (int i = 0; i < n; i++) {
        int c = 0;
        for (int j = 0; j < n; j++)
            c += r->off_hist[j] == r->off_hist[i];
        if (c > best_c) {
            best_c = c;
            best = r->off_hist[i];
        }
    }
    return best;
}

static void rx_push_dist(dstar_voice_rx *r, int d)
{
    r->dist_hist[r->dist_n % DSTAR_VC_DIST_HIST] = (uint8_t) d;
    if (r->dist_n < 0xFFU)
        r->dist_n++;
}

static void rx_lock(dstar_voice_rx *r, const uint8_t rr[VOICE_NONCE_RAND_BYTES], uint16_t sf)
{
    memcpy(r->r, rr, VOICE_NONCE_RAND_BYTES);
    r->sf = sf;
    r->synced = true;
    r->bad_key = false;
    r->cusum = 0;
    r->dist_n = 0;           /* judge the lock from here on */
}

/* Slots 17..20 of the current superframe are in: look for a sync block. */
static void rx_try_sync(dstar_voice_rx *r)
{
    uint8_t ua[6], ub[6], sync[DSTAR_VC_SYNC_BYTES];
    memcpy(ua, r->slow[0], 3); memcpy(ua + 3, r->slow[1], 3);
    memcpy(ub, r->slow[2], 3); memcpy(ub + 3, r->slow[3], 3);

    if (popc8((uint8_t) (ub[0] ^ DSTAR_VC_SLOW_TYPE)) > RX_TYPE_MAX_BITS)
        return;

    r->enc_seen = true;
    r->miss = 0;
    memcpy(sync, ua + 2, 4);
    memcpy(sync + 4, ub + 1, 5);

    const uint16_t sf_blk = (uint16_t) ((sync[6] << 8) | sync[7]);
    uint8_t check;
    if (!sync_check(sync, sf_blk, &check))
        return;                              /* no key: status says so */
    const bool exact = check == sync[8];

    /* Two consecutive exact blocks, same R, counters one superframe apart
     * per our own count: the clean-link fast lock. */
    const bool fast = exact && r->last_exact &&
                      memcmp(r->last_r, sync, VOICE_NONCE_RAND_BYTES) == 0 &&
                      (uint16_t) (sf_blk - r->last_sf) == (uint16_t) (r->local_sf - r->last_local);
    r->last_exact = exact;
    memcpy(r->last_r, sync, VOICE_NONCE_RAND_BYTES);
    r->last_sf = sf_blk;
    r->last_local = r->local_sf;

    if (r->synced) {
        /* Judge the lock against its own R and counter. */
        uint8_t want;
        if (!sync_check(r->r, (uint16_t) r->sf, &want))
            return;
        /* Re-align the counter only on two consecutive exact blocks that
         * agree: with bit errors, one block whose counter field is corrupt
         * still passes the check once in 256 times, and following it
         * decrypted the next superframes with the wrong keystream. */
        if (fast && memcmp(r->r, sync, VOICE_NONCE_RAND_BYTES) == 0)
            r->sf = sf_blk;
        if (fast && memcmp(r->r, sync, VOICE_NONCE_RAND_BYTES) != 0) {
            rx_forget_votes(r);              /* a new over, confirmed twice */
            rx_lock(r, sync, sf_blk);
            return;
        }
        const int d = popc8((uint8_t) (want ^ sync[8]));
        rx_push_dist(r, d);
        int s = r->cusum + RX_LLR[d];
        r->cusum = (int16_t) (s < 0 ? 0 : s);
        if (r->cusum >= RX_CUSUM_UNLOCK) {
            /* The key does not match (changed mid-over, or a false lock).
             * The status reads "bad key" right away; R has not changed, so
             * the votes stay valid. */
            r->synced = false;
            r->bad_key = true;
        }
        return;
    }

    if (fast) {
        rx_lock(r, sync, sf_blk);
        return;
    }

    /* Robust path: majority R and the most common counter offset, then
     * re-score every kept block's check against both -- blocks scored
     * before the vote converged would otherwise hold the lock back. */
    uint8_t vr[VOICE_NONCE_RAND_BYTES];
    rx_vote_r(r, sync, vr);
    const uint16_t off = rx_vote_off(r, (uint16_t) (sf_blk - (uint16_t) r->local_sf));
    r->blk_chk[r->blk_n % DSTAR_VC_DIST_HIST] = sync[8];
    r->blk_local[r->blk_n % DSTAR_VC_DIST_HIST] = r->local_sf;
    if (r->blk_n < 0xFFU)
        r->blk_n++;

    const int n = r->blk_n < DSTAR_VC_DIST_HIST ? r->blk_n : DSTAR_VC_DIST_HIST;
    int bits = 0, bad = 0;
    for (int i = 0; i < n; i++) {
        uint8_t want;
        if (!sync_check(vr, (uint16_t) (r->blk_local[i] + off), &want))
            return;
        int d = popc8((uint8_t) (want ^ r->blk_chk[i]));
        bits += d;
        bad += d >= RX_BAD_BITS;
    }
    if (n >= 2 && 2 * bad >= n)
        r->bad_key = true;               /* the status reads "bad key" */
    if (n >= DSTAR_VC_DIST_HIST && bits <= RX_LOCK_MAX_BITS)
        rx_lock(r, vr, (uint16_t) (r->local_sf + off));
}

bool dstar_voice_rx_frame(dstar_voice_rx *r, const uint8_t frame[SBITX_DSTAR_FRAME_BYTES],
                          uint16_t fc, char ambe_d[DSTAR_VOICE_AMBE_BITS],
                          mbe_process_result *res)
{
    char fr[4][24];

    if (fc == 0) {
        /* A new superframe. A missed data sync lets fc run past 20, so
         * advance by however many superframes actually went by. */
        const uint32_t elapsed = (uint32_t) (r->last_fc / DSTAR_VOICE_SF_FRAMES) + 1U;
        r->local_sf += elapsed;
        if (r->synced) {
            r->sf += elapsed;
            if (++r->miss > RX_MAX_MISS) {
                r->synced = false;
                r->enc_seen = false;
                rx_forget_votes(r);
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
