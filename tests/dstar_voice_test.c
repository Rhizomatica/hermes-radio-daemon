/* dstar_voice_test — D-STAR voice framing and opt-in encryption.
 *
 * Byte-level scenarios (clear, encrypted, late entry, wrong/no key, missed
 * data sync, corrupted sync block, fail-closed, key files) plus one
 * end-to-end run through the real GMSK modulator and demodulator.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dsp/dstar_voice.h"
#include "dsp/sbitx_dstar.h"
#include "dsp/voice_crypto.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

#define NFRAMES (21 * 12)   /* 12 superframes, ~5 s */

static const uint8_t KEY_A[VOICE_KEY_BYTES] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};
static const uint8_t KEY_B[VOICE_KEY_BYTES] = { 0xff, 0xee, 0xdd };

static char    plain[NFRAMES][DSTAR_VOICE_AMBE_BITS];
static uint8_t wire[NFRAMES][SBITX_DSTAR_FRAME_BYTES];
static uint8_t wire_hdr[SBITX_DSTAR_HEADER_BYTES];
static uint8_t wire_r[VOICE_NONCE_RAND_BYTES];   /* the over's nonce */

/* A vowel-like test signal: 120 Hz harmonics under a slow syllable envelope. */
static void make_plain(void)
{
    mbe_parms cur, prev, enh;
    mbe_ambe2400_encoder *enc = mbe_ambe2400EncoderAlloc();
    mbe_initMbeParms(&cur, &prev, &enh);
    for (int f = 0; f < NFRAMES; f++) {
        float pcm[160];
        for (int i = 0; i < 160; i++) {
            double t = (f * 160 + i) / 8000.0;
            double env = 0.5 + 0.5 * sin(2 * M_PI * 3.0 * t);
            double s = 0;
            for (int h = 1; h <= 12; h++)
                s += sin(2 * M_PI * 120.0 * h * t) / h;
            pcm[i] = (float) (0.2 * env * s);
        }
        mbe_encodeAmbe2400Parms(enc, pcm, plain[f], &cur, &prev);
        mbe_moveMbeParms(&cur, &prev);
    }
    mbe_ambe2400EncoderFree(enc);
}

/* The 48 voice bits match; the spare bit is FEC parity on air. */
static bool same_voice(const char *a, const char *b)
{
    for (int i = 0; i < DSTAR_VOICE_AMBE_BITS; i++)
        if (i != DSTAR_VOICE_SPARE_BIT && a[i] != b[i])
            return false;
    return true;
}

static void transmit(bool encrypt)
{
    dstar_voice_tx tx;
    dstar_voice_tx_begin(&tx, "PU2UIT  ", "CQCQCQ  ", encrypt);
    memcpy(wire_hdr, tx.header, sizeof(wire_hdr));
    memcpy(wire_r, tx.r, sizeof(wire_r));
    for (int f = 0; f < NFRAMES; f++)
        dstar_voice_tx_frame(&tx, plain[f], wire[f]);
}

typedef struct {
    int played, muted, match, first_match;
    dstar_vc_status status;
} rx_result;

/* Receive from frame `join` (a superframe boundary, where a real demodulator
 * locks). skip_sync_sf: a superframe whose data sync the demodulator misses,
 * so fc keeps counting past 20 (-1 = none). */
static rx_result receive(int join, bool with_header, int skip_sync_sf)
{
    rx_result res = {0, 0, 0, -1, DSTAR_VC_CLEAR};
    dstar_voice_rx rx;
    dstar_voice_rx_reset(&rx);
    if (with_header)
        dstar_voice_rx_header(&rx, wire_hdr);

    uint16_t fc = 0;
    for (int f = join; f < NFRAMES; f++) {
        int slot = f % DSTAR_VOICE_SF_FRAMES;
        if (slot == 0 && f != join && f / DSTAR_VOICE_SF_FRAMES != skip_sync_sf)
            fc = 0;
        char ambe_d[DSTAR_VOICE_AMBE_BITS];
        mbe_process_result r;
        if (dstar_voice_rx_frame(&rx, wire[f], fc, ambe_d, &r)) {
            res.played++;
            if (same_voice(ambe_d, plain[f])) {
                res.match++;
                if (res.first_match < 0)
                    res.first_match = f;
            }
        } else {
            res.muted++;
        }
        fc++;
    }
    res.status = rx.status;
    return res;
}

static void test_clear(void)
{
    voice_crypto_clear_key();
    transmit(false);
    CHECK((wire_hdr[2] & DSTAR_HDR_FLAG3_ENCRYPTED) == 0, "clear header flagged encrypted");
    rx_result r = receive(0, true, -1);
    CHECK(r.played == NFRAMES && r.match == NFRAMES, "clear over, no key: played %d match %d", r.played, r.match);
    CHECK(r.status == DSTAR_VC_CLEAR, "clear status %d", r.status);

    voice_crypto_set_key(KEY_A);   /* a keyed receiver still plays clear overs */
    r = receive(0, true, -1);
    CHECK(r.played == NFRAMES && r.match == NFRAMES, "clear over, keyed rx: played %d match %d", r.played, r.match);
}

static void test_encrypted(void)
{
    voice_crypto_set_key(KEY_A);
    transmit(true);
    CHECK(wire_hdr[2] & DSTAR_HDR_FLAG3_ENCRYPTED, "encrypted header not flagged");

    /* The ciphertext really differs: FEC-decode the wire frames as-is. */
    long diff = 0;
    for (int f = 0; f < NFRAMES; f++) {
        char fr[4][24], d[DSTAR_VOICE_AMBE_BITS];
        mbe_decodeDStarDVData(wire[f], fr);
        mbe_decodeAmbe3600x2400Frame((const char(*)[24])fr, d, NULL);
        for (int i = 0; i < DSTAR_VOICE_AMBE_BITS; i++)
            if (i != DSTAR_VOICE_SPARE_BIT)
                diff += d[i] != plain[f][i];
    }
    double frac = (double) diff / (NFRAMES * (DSTAR_VOICE_AMBE_BITS - 1));
    CHECK(frac > 0.45 && frac < 0.55, "ciphertext differs in %.1f%% of bits, want ~50%%", 100 * frac);

    /* From the start of the over: the header carries R under its FEC and
     * CRC, so every frame decrypts from the first one. */
    rx_result r = receive(0, true, -1);
    CHECK(r.first_match == 0, "decrypt starts at frame %d, want 0", r.first_match);
    CHECK(r.muted == 0 && r.played == NFRAMES && r.match == NFRAMES,
          "start of over: muted %d played %d match %d", r.muted, r.played, r.match);
    CHECK(r.status == DSTAR_VC_DECRYPTING, "status %d, want decrypting", r.status);

    /* Late entry at superframe 5, no header yet: the first 20 frames can't be
     * told from a clear over and play as-is (garbage, not speech); from the
     * first sync block the over is known to be encrypted and mutes; from
     * the confirming block (superframe 6), exact. */
    r = receive(5 * 21, false, -1);
    CHECK(r.first_match == 6 * 21 + 20, "late entry decrypt starts at %d", r.first_match);
    CHECK(r.match == NFRAMES - (6 * 21 + 20), "late entry match %d", r.match);

    /* A missed data sync (fc runs to 41) keeps the frame count right. */
    r = receive(0, true, 4);
    CHECK(r.match == r.played && r.played == NFRAMES,
          "missed data sync: played %d match %d", r.played, r.match);

    /* A header that fails its CRC is not trusted: the sync blocks lock as
     * before (superframe 1's block confirms superframe 0's). */
    uint8_t saved = wire_hdr[20];
    wire_hdr[20] ^= 0x10;                      /* a callsign bit: CRC fails */
    r = receive(0, true, -1);
    wire_hdr[20] = saved;
    CHECK(r.first_match == 41 && r.match == r.played,
          "corrupt header: decrypt starts at %d (want 41), played %d match %d",
          r.first_match, r.played, r.match);

    /* A second over draws a new nonce: same speech, different ciphertext. */
    uint8_t first[SBITX_DSTAR_FRAME_BYTES];
    memcpy(first, wire[30], sizeof(first));
    transmit(true);
    CHECK(memcmp(first, wire[30], 9) != 0, "two overs produced identical ciphertext");
}

static void test_wrong_and_no_key(void)
{
    voice_crypto_set_key(KEY_A);
    transmit(true);

    voice_crypto_set_key(KEY_B);
    rx_result r = receive(0, true, -1);
    CHECK(r.played == 0, "wrong key played %d frames", r.played);
    CHECK(r.status == DSTAR_VC_BAD_KEY, "wrong key status %d", r.status);

    voice_crypto_clear_key();
    r = receive(0, true, -1);
    CHECK(r.played == 0, "no key played %d frames", r.played);
    CHECK(r.status == DSTAR_VC_NO_KEY, "no key status %d", r.status);
}

/* The sync block's keyed check is one byte, so a receiver holding the wrong
 * key sees a block pass it about once in 256 superframes. Taken alone, that
 * block locked the receiver onto a keystream it cannot produce, and it
 * played garbage until the over ended. Find overs where that happens and
 * check the receiver stays muted. */
static uint8_t check_byte(const uint8_t key[VOICE_KEY_BYTES], uint32_t sf)
{
    uint8_t blk[VOICE_BLOCK_BYTES];
    voice_crypto_set_key(key);
    voice_crypto_block(VOICE_DOMAIN_CHECK, wire_r, sf, blk);
    return blk[0];
}

static void test_wrong_key_false_check(void)
{
    const int nsf = NFRAMES / DSTAR_VOICE_SF_FRAMES;
    int cases = 0, overs = 0;

    while (cases < 8 && overs < 5000) {
        voice_crypto_set_key(KEY_A);
        transmit(true);
        overs++;

        int hit = -1, adjacent = 0;
        for (int sf = 0; sf < nsf; sf++) {
            if (check_byte(KEY_A, sf) != check_byte(KEY_B, sf))
                continue;
            if (hit == sf - 1 && hit >= 0)
                adjacent = 1;   /* two in a row: the 1-in-65536 case */
            hit = sf;
        }
        if (hit < 0 || adjacent)
            continue;

        cases++;
        voice_crypto_set_key(KEY_B);
        rx_result r = receive(0, true, -1);
        CHECK(r.played == 0, "wrong key, check collision at superframe %d: played %d frames",
              hit, r.played);
    }
    CHECK(cases == 8, "found only %d false-check overs in %d", cases, overs);
}

/* A receiver whose key stops matching mid-over (the key was switched during
 * the over) must go quiet within a few superframes, not keep playing
 * garbage. Three failing blocks unlock it; a chance pass (1 in 256) can add
 * a superframe, so allow two of those. */
static void test_key_lost_mid_over(void)
{
    voice_crypto_set_key(KEY_A);
    transmit(true);

    dstar_voice_rx rx;
    dstar_voice_rx_reset(&rx);
    dstar_voice_rx_header(&rx, wire_hdr);
    int played_after = 0;
    uint16_t fc = 0;
    for (int f = 0; f < NFRAMES; f++) {
        if (f % DSTAR_VOICE_SF_FRAMES == 0 && f)
            fc = 0;
        if (f == 4 * DSTAR_VOICE_SF_FRAMES)
            voice_crypto_set_key(KEY_B);
        char ambe_d[DSTAR_VOICE_AMBE_BITS];
        bool play = dstar_voice_rx_frame(&rx, wire[f], fc, ambe_d, NULL);
        if (play && f >= 10 * DSTAR_VOICE_SF_FRAMES)
            played_after++;
        fc++;
    }
    CHECK(played_after == 0, "key lost mid-over: still playing %d frames 6 superframes later",
          played_after);
    CHECK(rx.status == DSTAR_VC_BAD_KEY, "key lost mid-over: status %d, want key mismatch",
          rx.status);
}

static void test_corrupt_sync(void)
{
    voice_crypto_set_key(KEY_A);
    transmit(true);
    wire[19][10] ^= 0x04;   /* one bit of superframe 0's sync block */
    /* With the header, the sync blocks do not matter at the start. */
    rx_result r = receive(0, true, -1);
    CHECK(r.first_match == 0 && r.match == NFRAMES, "corrupt sync, header: starts at %d match %d",
          r.first_match, r.match);
    /* Joined mid-over (superframe 0 is before the join, so its corrupt
     * block is not seen): superframes 1 and 2 confirm each other. */
}

/* Slow data has no FEC. At a few percent bit errors most 72-bit sync
 * blocks carry a flipped bit, and a receiver that needed clean blocks
 * never locked, while the voice itself (AMBE FEC) was still fine. It must
 * lock on the vote instead, and a wrong key must still play nothing. */
static void test_bit_errors(void)
{
    static const double ber[] = { 0.02, 0.04 };
    for (unsigned k = 0; k < sizeof(ber) / sizeof(ber[0]); k++) {
        int locked = 0, runs = 10, worst = 0;
        for (int run = 0; run < runs; run++) {
            voice_crypto_set_key(KEY_A);
            transmit(true);
            srand(1000 * k + run);
            for (int f = 0; f < NFRAMES; f++)
                for (int b = 9; b < 12; b++)
                    for (int bit = 0; bit < 8; bit++)
                        if (rand() < ber[k] * RAND_MAX)
                            wire[f][b] ^= (uint8_t) (1U << bit);
            rx_result r = receive(0, true, -1);
            CHECK(r.match == r.played, "BER %.0f%%: played %d but only %d exact",
                  100 * ber[k], r.played, r.match);
            if (r.status == DSTAR_VC_DECRYPTING && r.first_match >= 0) {
                locked++;
                if (r.first_match > worst)
                    worst = r.first_match;
            }

            voice_crypto_set_key(KEY_B);
            r = receive(0, true, -1);
            CHECK(r.played == 0, "BER %.0f%%, wrong key: played %d frames", 100 * ber[k], r.played);
        }
        printf("slow-data BER %.0f%%: locked in %d of %d overs, latest at frame %d (superframe %d)\n",
               100 * ber[k], locked, runs, worst, worst / DSTAR_VOICE_SF_FRAMES);
        CHECK(locked == runs, "BER %.0f%%: locked in only %d of %d overs", 100 * ber[k], locked, runs);
        if (ber[k] <= 0.02)
            CHECK(worst <= 6 * DSTAR_VOICE_SF_FRAMES, "BER %.0f%%: locked as late as frame %d",
                  100 * ber[k], worst);
    }
}

/* Joining mid-over: the slow-data repetition of the header (bit-voted and
 * CRC-checked by the modem) gives R and proves the key; the next sync block
 * whose check passes on its own counter gives the counter. */
static void test_late_header(void)
{
    voice_crypto_set_key(KEY_A);
    transmit(true);
    dstar_voice_rx rx;
    dstar_voice_rx_reset(&rx);
    int first = -1, played = 0, match = 0;
    uint16_t fc = 0;
    for (int f = 3 * 21; f < NFRAMES; f++) {
        if (f % 21 == 0 && f != 3 * 21)
            fc = 0;
        if (f == 3 * 21 + 10)
            dstar_voice_rx_header(&rx, wire_hdr);     /* the slow-data copy arrives */
        char ambe_d[DSTAR_VOICE_AMBE_BITS];
        if (dstar_voice_rx_frame(&rx, wire[f], fc, ambe_d, NULL) && f > 3 * 21 + 10) {
            played++;
            if (same_voice(ambe_d, plain[f])) {
                match++;
                if (first < 0)
                    first = f;
            }
        }
        fc++;
    }
    CHECK(first == 3 * 21 + 20, "late header: decrypt starts at %d, want %d", first, 3 * 21 + 20);
    CHECK(match == played && rx.status == DSTAR_VC_DECRYPTING,
          "late header: played %d match %d status %d", played, match, rx.status);

    /* ...and with the wrong key the header proves it at once: nothing plays. */
    voice_crypto_set_key(KEY_B);
    rx_result r = receive(0, true, -1);
    CHECK(r.played == 0 && r.status == DSTAR_VC_BAD_KEY, "wrong key, header: played %d status %d",
          r.played, r.status);
}

static void test_fail_closed(void)
{
    static const uint8_t null_ambe[9] = {0x9E, 0x8D, 0x32, 0x88, 0x26, 0x1A, 0x3F, 0x61, 0xE8};
    voice_crypto_clear_key();
    transmit(true);
    CHECK((wire_hdr[2] & DSTAR_HDR_FLAG3_ENCRYPTED) == 0, "fail-closed header flagged encrypted");
    int silent = 0;
    for (int f = 0; f < NFRAMES; f++)
        silent += memcmp(wire[f], null_ambe, 9) == 0;
    CHECK(silent == NFRAMES, "fail-closed: %d/%d frames silent", silent, NFRAMES);
}

static bool load_key_text(const char *text, size_t len)
{
    char path[] = "/tmp/dstar_voice_test_keyXXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return false;
    bool ok = write(fd, text, len) == (ssize_t) len;
    close(fd);
    ok = ok && voice_crypto_load_key_file(path);
    unlink(path);
    return ok;
}

static void test_key_files(void)
{
    const char *hex = "000102030405060708090a0b0c0d0e0f101112131415161718191A1B1C1D1E1F\n";
    CHECK(load_key_text(hex, strlen(hex)), "hex key rejected");
    CHECK(voice_crypto_have_key(), "hex key not installed");
    CHECK(load_key_text((const char *) KEY_A, sizeof(KEY_A)), "raw key rejected");
    /* Must match hermes-voice-key's "show" (vector from Python hashlib). */
    char fp[17];
    CHECK(voice_crypto_fingerprint(fp) && strcmp(fp, "b730d4e0ada7e438") == 0,
          "fingerprint %s, want b730d4e0ada7e438", fp);
    CHECK(!load_key_text("0011", 4), "short key accepted");
    CHECK(!voice_crypto_have_key(), "failed load left a key installed");
    char bad[64];
    memset(bad, 'g', sizeof(bad));
    CHECK(!load_key_text(bad, sizeof(bad)), "non-hex key accepted");
}

/* ── end to end through the GMSK modem ─────────────────────────── */

static dstar_voice_rx e2e_rx;
static sbitx_dstar_rx *e2e_demod;
static int e2e_frames, e2e_played, e2e_match;

static void e2e_header(void *u, const uint8_t *h) { (void) u; dstar_voice_rx_header(&e2e_rx, h); }
static void e2e_nop(void *u) { (void) u; }
static void e2e_data(void *u, const uint8_t *frame)
{
    (void) u;
    char ambe_d[DSTAR_VOICE_AMBE_BITS];
    uint16_t fc = sbitx_dstar_rx_frame_index(e2e_demod);
    int f = e2e_frames++;
    if (dstar_voice_rx_frame(&e2e_rx, frame, fc, ambe_d, NULL)) {
        e2e_played++;
        if (f < NFRAMES && same_voice(ambe_d, plain[f]))
            e2e_match++;
    }
}

static void test_modem_e2e(void)
{
    voice_crypto_set_key(KEY_A);
    transmit(true);

    sbitx_dstar_tx *mod = sbitx_dstar_tx_new();
    e2e_demod = sbitx_dstar_rx_new();
    dstar_voice_rx_reset(&e2e_rx);
    sbitx_dstar_rx_set_cbs(e2e_demod, e2e_header, e2e_data, e2e_nop, e2e_nop, NULL);
    /* Straight from modulator to demodulator there is no FM chain, whose
     * inversion the demodulator's default polarity expects. */
    sbitx_dstar_rx_set_polarity(e2e_demod, -1.0f);

    float buf[4096];
    sbitx_dstar_tx_header(mod, wire_hdr);
    for (int f = 0; f <= NFRAMES; f++) {
        if (f < NFRAMES)
            sbitx_dstar_tx_frame(mod, wire[f]);
        else
            sbitx_dstar_tx_eot(mod);
        int n;
        while ((n = sbitx_dstar_tx_generate(mod, buf, 4096)) > 0)
            sbitx_dstar_rx_process(e2e_demod, buf, n);
    }
    memset(buf, 0, sizeof(buf));
    sbitx_dstar_rx_process(e2e_demod, buf, 4096);

    printf("modem e2e: %d frames delivered, %d played, %d exact, status %d\n",
           e2e_frames, e2e_played, e2e_match, e2e_rx.status);
    CHECK(e2e_frames == NFRAMES, "modem delivered %d of %d frames", e2e_frames, NFRAMES);
    /* The header comes through the real modulator and demodulator (FEC,
     * CRC), so decryption starts with the first voice frame. */
    CHECK(e2e_match == NFRAMES && e2e_played == e2e_match,
          "modem e2e: played %d exact %d, want %d", e2e_played, e2e_match, NFRAMES);
    sbitx_dstar_tx_free(mod);
    sbitx_dstar_rx_free(e2e_demod);
}

/* After an EOT the modem must drain completely: tr_switch waits for it
 * before unkeying, and 18 end-sync bytes left behind in the queue kept it
 * "pending" forever, so every unkey waited out the full timeout. */
static void test_tx_eot_drains(void)
{
    sbitx_dstar_tx *tx = sbitx_dstar_tx_new();
    uint8_t frame[SBITX_DSTAR_FRAME_BYTES] = {0};
    float buf[4096];
    CHECK(tx != NULL, "sbitx_dstar_tx_new");
    if (!tx)
        return;
    sbitx_dstar_tx_header(tx, wire_hdr);
    for (int f = 0; f < 5; f++)
        sbitx_dstar_tx_frame(tx, frame);
    sbitx_dstar_tx_eot(tx);
    long samples = 0;
    int got;
    while ((got = sbitx_dstar_tx_generate(tx, buf, 4096)) > 0)
        samples += got;
    CHECK(!sbitx_dstar_tx_pending(tx), "modem still pending after the EOT went out");
    /* preamble 60 + header 85 + 5 x 12 data + 18 EOT bytes, 40 samples each */
    CHECK(samples == (60 + 85 + 5 * 12 + 18) * 40L, "generated %ld samples, want %ld",
          samples, (60 + 85 + 5 * 12 + 18) * 40L);
    sbitx_dstar_tx_free(tx);
}

int main(void)
{
    make_plain();
    test_clear();
    test_encrypted();
    test_wrong_and_no_key();
    test_wrong_key_false_check();
    test_bit_errors();
    test_tx_eot_drains();
    test_late_header();
    test_key_lost_mid_over();
    test_corrupt_sync();
    test_fail_closed();
    test_key_files();
    test_modem_e2e();

    if (failures) {
        fprintf(stderr, "dstar_voice_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("dstar_voice_test: ok\n");
    return 0;
}
