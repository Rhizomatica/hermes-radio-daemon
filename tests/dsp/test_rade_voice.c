/* rade_voice_test — RADE V2 keyed latent mixing (dsp/rade_voice.c).
 *
 * The transform is unitary per carrier (unmixing is exact, each carrier's
 * pair energy is kept), commutes with a per-carrier channel gain, changes
 * with the frame, the nonce and the key, and does nothing without a key.
 * Channel and speech tests are utils/rade_crypt_sim.c's.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <string.h>

#include "unity.h"
#include "test_util.h"
#include "dsp/rade_voice.h"
#include "dsp/voice_crypto.h"

#define NC RADE_VOICE_NC

static const uint8_t KEY_A[VOICE_KEY_BYTES] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};
static const uint8_t KEY_B[VOICE_KEY_BYTES] = { 0xff, 0xee, 0xdd };
static const uint8_t R1[VOICE_NONCE_RAND_BYTES] = { 1, 2, 3, 4, 5, 6 };
static const uint8_t R2[VOICE_NONCE_RAND_BYTES] = { 6, 5, 4, 3, 2, 1 };

/* a latent with every value distinct and nonzero */
static void latent(float z[RADE_VOICE_LATENT])
{
    for (int i = 0; i < RADE_VOICE_LATENT; i++)
        z[i] = sinf(0.7f * (float) i + 0.3f) + 0.05f * (float) (i % 7);
}

static double max_diff(const float *a, const float *b)
{
    double m = 0;
    for (int i = 0; i < RADE_VOICE_LATENT; i++)
        m = fmax(m, fabs((double) a[i] - b[i]));
    return m;
}

static double pair_energy(const float *z, int c)
{
    const float *p0 = &z[c * 2], *p1 = &z[(NC + c) * 2];
    return (double) p0[0] * p0[0] + (double) p0[1] * p0[1] + (double) p1[0] * p1[0] + (double) p1[1] * p1[1];
}

void setUp(void)
{
    voice_crypto_set_key(KEY_A);
}

void tearDown(void)
{
    voice_crypto_clear_key();
}

static void test_roundtrip(void)
{
    float x[RADE_VOICE_LATENT], z[RADE_VOICE_LATENT];
    latent(x);
    for (uint32_t frame = 0; frame < 100; frame++) {
        memcpy(z, x, sizeof(z));
        TEST_ASSERT_TRUE(rade_voice_scramble(z, R1, frame, false));
        TEST_ASSERT_TRUE(max_diff(z, x) > 0.1);
        TEST_ASSERT_TRUE(rade_voice_scramble(z, R1, frame, true));
        TEST_ASSERT_TRUE(max_diff(z, x) < 1e-5);
    }
}

static void test_pair_energy_kept(void)
{
    float x[RADE_VOICE_LATENT], z[RADE_VOICE_LATENT];
    latent(x);
    memcpy(z, x, sizeof(z));
    TEST_ASSERT_TRUE(rade_voice_scramble(z, R1, 7, false));
    for (int c = 0; c < NC; c++)
        TEST_ASSERT_DOUBLE_WITHIN(1e-5, pair_energy(x, c), pair_energy(z, c));
}

/* The channel's per-carrier gain h_c, applied between mixing and unmixing,
 * comes out as h_c on the plain latent: what the decoder sees unscrambled. */
static void test_commutes_with_carrier_gain(void)
{
    float x[RADE_VOICE_LATENT], z[RADE_VOICE_LATENT], want[RADE_VOICE_LATENT];
    latent(x);
    memcpy(z, x, sizeof(z));
    TEST_ASSERT_TRUE(rade_voice_scramble(z, R1, 3, false));
    for (int s = 0; s < RADE_VOICE_NS; s++)
        for (int c = 0; c < NC; c++) {
            float hr = 0.3f + 0.1f * (float) c, hi = -0.5f + 0.07f * (float) c;
            float *p = &z[(s * NC + c) * 2], *w = &want[(s * NC + c) * 2];
            const float *q = &x[(s * NC + c) * 2];
            float r = p[0], i = p[1];
            p[0] = hr * r - hi * i;
            p[1] = hr * i + hi * r;
            w[0] = hr * q[0] - hi * q[1];
            w[1] = hr * q[1] + hi * q[0];
        }
    TEST_ASSERT_TRUE(rade_voice_scramble(z, R1, 3, true));
    TEST_ASSERT_TRUE(max_diff(z, want) < 1e-5);
}

static void test_frame_nonce_key_change_it(void)
{
    float x[RADE_VOICE_LATENT], a[RADE_VOICE_LATENT], b[RADE_VOICE_LATENT];
    latent(x);
    memcpy(a, x, sizeof(a));
    TEST_ASSERT_TRUE(rade_voice_scramble(a, R1, 0, false));

    memcpy(b, x, sizeof(b));
    TEST_ASSERT_TRUE(rade_voice_scramble(b, R1, 1, false));
    TEST_ASSERT_TRUE(max_diff(a, b) > 0.1);

    memcpy(b, x, sizeof(b));
    TEST_ASSERT_TRUE(rade_voice_scramble(b, R2, 0, false));
    TEST_ASSERT_TRUE(max_diff(a, b) > 0.1);

    memcpy(b, x, sizeof(b));
    voice_crypto_set_key(KEY_B);
    TEST_ASSERT_TRUE(rade_voice_scramble(b, R1, 0, false));
    TEST_ASSERT_TRUE(max_diff(a, b) > 0.1);

    /* unmixing with the wrong key or frame does not recover the latent */
    TEST_ASSERT_TRUE(rade_voice_scramble(a, R1, 0, true));   /* KEY_B */
    TEST_ASSERT_TRUE(max_diff(a, x) > 0.1);
    voice_crypto_set_key(KEY_A);
    memcpy(a, x, sizeof(a));
    TEST_ASSERT_TRUE(rade_voice_scramble(a, R1, 5, false));
    TEST_ASSERT_TRUE(rade_voice_scramble(a, R1, 6, true));
    TEST_ASSERT_TRUE(max_diff(a, x) > 0.1);
}

/* The same key, nonce and frame give the same transform: two stations
 * agree. */
static void test_deterministic(void)
{
    float a[RADE_VOICE_LATENT], b[RADE_VOICE_LATENT];
    latent(a);
    latent(b);
    TEST_ASSERT_TRUE(rade_voice_scramble(a, R2, 12345, false));
    voice_crypto_clear_key();
    voice_crypto_set_key(KEY_A);
    TEST_ASSERT_TRUE(rade_voice_scramble(b, R2, 12345, false));
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(a));
}

static void test_no_key(void)
{
    float x[RADE_VOICE_LATENT], z[RADE_VOICE_LATENT];
    latent(x);
    memcpy(z, x, sizeof(z));
    voice_crypto_clear_key();
    TEST_ASSERT_FALSE(rade_voice_scramble(z, R1, 0, false));
    TEST_ASSERT_EQUAL_MEMORY(x, z, sizeof(z));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_pair_energy_kept);
    RUN_TEST(test_commutes_with_carrier_gain);
    RUN_TEST(test_frame_nonce_key_change_it);
    RUN_TEST(test_deterministic);
    RUN_TEST(test_no_key);
    return UNITY_END();
}
