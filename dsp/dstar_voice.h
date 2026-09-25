/* hermes-radio-daemon — D-STAR DV voice framing, shared by the sBitx DSP
 * and the Hamlib digital paths: DV header, superframe slot counter, slow
 * data (header repetition), AMBE FEC, and opt-in voice encryption.
 *
 * Encryption (HERMES extension, not part of the D-STAR standard, which
 * defines none):
 *
 * - Only the 48 AMBE voice-parameter bits of each 20 ms frame are
 *   encrypted, before the AMBE FEC is applied, by XOR with ChaCha20
 *   keystream (voice_crypto.c). Nothing is added to the frame, the FEC
 *   protects the ciphertext exactly as it would protect plaintext, and a
 *   residual bit error stays a single bit error.
 * - The DV header (callsigns) and the slow-data header copy stay in the
 *   clear, so station identification and routing work unchanged. Flag 3
 *   bit 0 of the header marks the over as encrypted.
 * - Each over draws a random 48-bit nonce R. Frame n of the over (n =
 *   superframe * 21 + slot) uses keystream block n of nonce R.
 * - For late entry and resync, every superframe carries a 9-byte sync
 *   block in slow data -- R[6], the 16-bit superframe counter, and a keyed
 *   8-bit check -- in the spare bytes of slow-data unit 8 (after the last
 *   header byte) and in unit 9, marked with slow-data type 0xE5.
 * - Slow data has no FEC, and at a few percent bit errors (which the AMBE
 *   FEC shrugs off) most sync blocks carry a flipped bit, so the receiver
 *   does not need clean blocks. It locks either on two consecutive blocks
 *   that pass the check exactly with the same R and consecutive counters
 *   (clean link: ~0.8 s into an over, once in 65536 for a wrong key), or
 *   on a bit-by-bit majority vote of R over the blocks, the counter from
 *   their offset to its own superframe count, and the last five checks
 *   within three bits in total of the expected ones, re-scored as the
 *   vote converges (once in ~10^8 for a wrong key). Once locked it follows
 *   its own counter through corrupted blocks and unlocks on a CUSUM of
 *   each check's likelihood ratio, wrong key against right key at a few
 *   percent bit errors: ~2 blocks for a wrong key, about once in 10^5
 *   blocks for the right one -- a flipped bit is not a wrong key.
 * - A receiver without the key, or with the wrong key, mutes the encrypted
 *   over instead of playing garbage.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DSTAR_VOICE_H_
#define DSTAR_VOICE_H_

#include <stdbool.h>
#include <stdint.h>

#include <mbelib-neo/mbelib.h>

#include "sbitx_dstar.h"
#include "voice_crypto.h"

#define DSTAR_VOICE_AMBE_BITS     49
/* ambe_d[24] is the AMBE 2400 spare bit: the FEC layer overwrites it on air
 * with Golay parity and the decoder never reads it. Not encrypted, and not
 * part of the voice for comparison purposes. */
#define DSTAR_VOICE_SPARE_BIT     24
#define DSTAR_VOICE_SF_FRAMES     21   /* frames per superframe */
#define DSTAR_VC_SYNC_BYTES       9
#define DSTAR_VC_SLOW_TYPE        0xE5U
#define DSTAR_HDR_FLAG3_ENCRYPTED 0x01U
#define DSTAR_VC_OFF_HIST         6
#define DSTAR_VC_DIST_HIST        5

/* ── TX ─────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  header[SBITX_DSTAR_HEADER_BYTES];
    int      slot;          /* 0..20 within the superframe */
    uint32_t sf;            /* superframe counter within the over */
    bool     encrypt;       /* this over is encrypted */
    bool     fail_closed;   /* encryption asked for without a key: send silence */
    uint8_t  r[VOICE_NONCE_RAND_BYTES];
    uint8_t  sync[DSTAR_VC_SYNC_BYTES];
} dstar_voice_tx;

/* Start an over: build the DV header from the 8-character callsign fields
 * and, if encrypt is set, draw a fresh nonce. If encryption is asked for
 * but no key is loaded (or no randomness is available), the over fails
 * closed: it is sent as silence rather than in the clear. */
void dstar_voice_tx_begin(dstar_voice_tx *t, const char *mycall8, const char *urcall8,
                          bool encrypt);

/* One 20 ms frame: encrypt (if the over is encrypted), apply the AMBE FEC
 * and fill the 12-byte DV frame (9 AMBE bytes + 3 sync/slow-data bytes). */
void dstar_voice_tx_frame(dstar_voice_tx *t, const char ambe_d[DSTAR_VOICE_AMBE_BITS],
                          uint8_t frame[SBITX_DSTAR_FRAME_BYTES]);

/* ── RX ─────────────────────────────────────────────────────────── */

typedef enum {
    DSTAR_VC_CLEAR = 0,     /* over is not encrypted */
    DSTAR_VC_DECRYPTING,    /* encrypted, key matches, playing */
    DSTAR_VC_ACQUIRING,     /* encrypted, waiting for a clean sync block */
    DSTAR_VC_NO_KEY,        /* encrypted, no key loaded: muted */
    DSTAR_VC_BAD_KEY,       /* encrypted, sync blocks fail the key check: muted */
} dstar_vc_status;

typedef struct {
    bool     hdr_enc;       /* header flagged the over as encrypted */
    bool     enc_seen;      /* a sync block was seen this over */
    bool     synced;
    uint8_t  r[VOICE_NONCE_RAND_BYTES];
    uint32_t sf;            /* superframe counter of the current superframe */
    uint16_t last_fc;
    uint8_t  slow[4][3];    /* descrambled slow data of slots 17..20 */
    uint8_t  slow_mask;
    uint8_t  miss;          /* superframes since the last valid sync block */
    uint32_t local_sf;      /* superframes heard this over, by our own count */
    /* Majority vote of R, bit by bit, over the blocks of this over. */
    uint8_t  vote_one[VOICE_NONCE_RAND_BYTES * 8];
    uint8_t  vote_n;
    /* Recent (block counter - local_sf) offsets; the most common one wins. */
    uint16_t off_hist[DSTAR_VC_OFF_HIST];
    uint8_t  off_n;
    /* Bit distance of the last blocks' check from the expected one. */
    uint8_t  dist_hist[DSTAR_VC_DIST_HIST];
    uint8_t  dist_n;
    /* The previous block, for the two-exact-blocks fast lock. */
    bool     last_exact;
    uint8_t  last_r[VOICE_NONCE_RAND_BYTES];
    uint16_t last_sf;
    uint32_t last_local;
    bool     bad_key;       /* evidence of a wrong key seen, and no lock since */
    int16_t  cusum;         /* locked: accumulated wrong-key evidence (x0.1 nats) */
    /* The last blocks' check bytes and our superframe count at each, so
     * they can all be re-scored as the vote of R converges. */
    uint8_t  blk_chk[DSTAR_VC_DIST_HIST];
    uint32_t blk_local[DSTAR_VC_DIST_HIST];
    uint8_t  blk_n;
    dstar_vc_status status;
} dstar_voice_rx;

/* Forget the current over (call on EOT and on loss of lock). */
void dstar_voice_rx_reset(dstar_voice_rx *r);

/* A DV header arrived: a new over (or a new station) begins. */
void dstar_voice_rx_header(dstar_voice_rx *r, const uint8_t header[SBITX_DSTAR_HEADER_BYTES]);

/* FEC-decode one DV frame into ambe_d and, for an encrypted over, decrypt
 * it. fc is the frame's index since the last data sync, from
 * sbitx_dstar_rx_frame_index(). res receives the FEC status for
 * mbe_processAmbe2400Data*(). Returns true if ambe_d holds speech to play,
 * false if the frame must be muted. */
bool dstar_voice_rx_frame(dstar_voice_rx *r, const uint8_t frame[SBITX_DSTAR_FRAME_BYTES],
                          uint16_t fc, char ambe_d[DSTAR_VOICE_AMBE_BITS],
                          mbe_process_result *res);

#endif /* DSTAR_VOICE_H_ */
