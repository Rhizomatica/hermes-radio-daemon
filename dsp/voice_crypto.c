/* hermes-radio-daemon — opt-in encryption for digital voice codecs
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include "voice_crypto.h"

/* The key lives here, process-private -- deliberately not in the radio
 * struct, which other processes can see through the status/SHM surfaces. */
static uint8_t         g_key[VOICE_KEY_BYTES];
static bool            g_have_key;
static pthread_mutex_t g_key_mutex = PTHREAD_MUTEX_INITIALIZER;

void voice_crypto_set_key(const uint8_t key[VOICE_KEY_BYTES])
{
    pthread_mutex_lock(&g_key_mutex);
    memcpy(g_key, key, VOICE_KEY_BYTES);
    g_have_key = true;
    pthread_mutex_unlock(&g_key_mutex);
}

void voice_crypto_clear_key(void)
{
    pthread_mutex_lock(&g_key_mutex);
    OPENSSL_cleanse(g_key, sizeof(g_key));
    g_have_key = false;
    pthread_mutex_unlock(&g_key_mutex);
}

bool voice_crypto_have_key(void)
{
    pthread_mutex_lock(&g_key_mutex);
    bool have = g_have_key;
    pthread_mutex_unlock(&g_key_mutex);
    return have;
}

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool voice_crypto_load_key_file(const char *path)
{
    uint8_t buf[256];
    uint8_t key[VOICE_KEY_BYTES];
    bool ok = false;

    voice_crypto_clear_key();

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "voice_crypto: cannot open key file %s: %s\n", path, strerror(errno));
        return false;
    }

    struct stat st;
    if (fstat(fileno(f), &st) == 0 && (st.st_mode & (S_IRWXG | S_IRWXO)))
        fprintf(stderr, "voice_crypto: WARNING: key file %s is accessible by group/others "
                        "(mode %03o); chmod 600 it\n", path, (unsigned) (st.st_mode & 0777));

    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    /* 32 raw bytes, exactly. */
    if (n == VOICE_KEY_BYTES) {
        memcpy(key, buf, VOICE_KEY_BYTES);
        ok = true;
    } else {
        /* Otherwise 64 hex digits, whitespace ignored around them. */
        size_t lo = 0, hi = n;
        while (lo < hi && isspace(buf[lo])) lo++;
        while (hi > lo && isspace(buf[hi - 1])) hi--;
        if (hi - lo == 2 * VOICE_KEY_BYTES) {
            ok = true;
            for (size_t i = 0; i < VOICE_KEY_BYTES; i++) {
                int h = hex_nibble(buf[lo + 2 * i]), l = hex_nibble(buf[lo + 2 * i + 1]);
                if (h < 0 || l < 0) { ok = false; break; }
                key[i] = (uint8_t) ((h << 4) | l);
            }
        }
    }

    if (ok) {
        voice_crypto_set_key(key);
        fprintf(stderr, "voice_crypto: voice key loaded from %s\n", path);
    } else {
        fprintf(stderr, "voice_crypto: %s is not a 32-byte raw or 64-hex-digit key\n", path);
    }
    OPENSSL_cleanse(buf, sizeof(buf));
    OPENSSL_cleanse(key, sizeof(key));
    return ok;
}

bool voice_crypto_fingerprint(char out[17])
{
    static const char label[] = "hermes-voice-fingerprint";
    uint8_t md[32];
    unsigned int md_len = 0;
    bool ok = false;

    out[0] = '\0';
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == NULL)
        return false;

    pthread_mutex_lock(&g_key_mutex);
    if (g_have_key &&
        EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
        EVP_DigestUpdate(ctx, label, sizeof(label) - 1) == 1 &&
        EVP_DigestUpdate(ctx, g_key, VOICE_KEY_BYTES) == 1 &&
        EVP_DigestFinal_ex(ctx, md, &md_len) == 1)
        ok = true;
    pthread_mutex_unlock(&g_key_mutex);
    EVP_MD_CTX_free(ctx);

    if (ok)
        for (int i = 0; i < 8; i++)
            snprintf(out + 2 * i, 3, "%02x", md[i]);
    return ok;
}

bool voice_crypto_new_nonce(uint8_t r[VOICE_NONCE_RAND_BYTES])
{
    return getrandom(r, VOICE_NONCE_RAND_BYTES, 0) == VOICE_NONCE_RAND_BYTES;
}

bool voice_crypto_block(uint8_t domain, const uint8_t r[VOICE_NONCE_RAND_BYTES],
                        uint32_t counter, uint8_t out[VOICE_BLOCK_BYTES])
{
    static const uint8_t zeros[VOICE_BLOCK_BYTES];
    /* OpenSSL's EVP_chacha20 IV is the 32-bit block counter (little-endian)
     * followed by the 96-bit RFC 8439 nonce. */
    uint8_t iv[16];
    iv[0] = (uint8_t) counter;
    iv[1] = (uint8_t) (counter >> 8);
    iv[2] = (uint8_t) (counter >> 16);
    iv[3] = (uint8_t) (counter >> 24);
    iv[4] = domain;
    memcpy(iv + 5, r, VOICE_NONCE_RAND_BYTES);
    memcpy(iv + 11, "HVC1", 4);
    iv[15] = 0;

    bool ok = false;
    int len = 0;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL)
        return false;

    pthread_mutex_lock(&g_key_mutex);
    if (g_have_key &&
        EVP_EncryptInit_ex(ctx, EVP_chacha20(), NULL, g_key, iv) == 1 &&
        EVP_EncryptUpdate(ctx, out, &len, zeros, VOICE_BLOCK_BYTES) == 1 &&
        len == VOICE_BLOCK_BYTES)
        ok = true;
    pthread_mutex_unlock(&g_key_mutex);

    EVP_CIPHER_CTX_free(ctx);
    return ok;
}
