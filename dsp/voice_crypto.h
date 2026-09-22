/* hermes-radio-daemon — opt-in encryption for digital voice codecs
 *
 * Codec-agnostic core: one process-wide 256-bit voice key and a ChaCha20
 * (RFC 8439) keystream generator. The codec glue (dstar_voice.c) decides
 * what to XOR the keystream onto and how to carry the per-over nonce.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef VOICE_CRYPTO_H_
#define VOICE_CRYPTO_H_

#include <stdbool.h>
#include <stdint.h>

#define VOICE_KEY_BYTES        32
#define VOICE_NONCE_RAND_BYTES 6   /* per-over random part of the nonce */
#define VOICE_BLOCK_BYTES      64  /* one ChaCha20 block */

/* Keystream domains: the same key and over nonce produce independent streams
 * for the voice bits and for the sync-block check value. */
#define VOICE_DOMAIN_VOICE 0x01
#define VOICE_DOMAIN_CHECK 0x02

/* Load the voice key from a file holding either 32 raw bytes or 64 hex
 * digits (surrounding whitespace allowed). Replaces any previous key. On
 * failure the previous key is cleared too, so a broken key file never leaves
 * a stale key in use. Warns if the file is group/world readable. */
bool voice_crypto_load_key_file(const char *path);

/* Install a key directly (tests, or a key-derivation helper). */
void voice_crypto_set_key(const uint8_t key[VOICE_KEY_BYTES]);
void voice_crypto_clear_key(void);
bool voice_crypto_have_key(void);

/* Key fingerprint for operators to compare stations without revealing the
 * key: the first 16 hex digits of SHA-256("hermes-voice-fingerprint" || key),
 * as hermes-voice-key prints it. Returns false (out = "") with no key. */
bool voice_crypto_fingerprint(char out[17]);

/* Fill r with fresh random bytes for a new over. */
bool voice_crypto_new_nonce(uint8_t r[VOICE_NONCE_RAND_BYTES]);

/* One 64-byte ChaCha20 keystream block for (key, domain, r, counter).
 * The 96-bit nonce is domain || r[6] || "HVC1" || 0. Returns false if no key
 * is loaded or the cipher fails. */
bool voice_crypto_block(uint8_t domain, const uint8_t r[VOICE_NONCE_RAND_BYTES],
                        uint32_t counter, uint8_t out[VOICE_BLOCK_BYTES]);

#endif /* VOICE_CRYPTO_H_ */
