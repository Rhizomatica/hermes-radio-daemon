/* The rules radio.h derives from a profile: which audio feeds the
 * transmitter, and which received audio is noise-reduced.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdbool.h>
#include <string.h>

#include "unity.h"
#include "radio.h"

void setUp(void) {}
void tearDown(void) {}

static radio_profile profile(uint16_t mode)
{
    radio_profile p;
    memset(&p, 0, sizeof(p));
    p.mode = mode;
    return p;
}

/* Typed text (FT8, CW, RTTY) is radiod's own transmission: the loopback's
 * audio stays out of the TX ring. Every other mode takes it. */
static void test_tx_is_text(void)
{
    uint16_t text[] = { MODE_FT8, MODE_CW, MODE_RTTY };
    uint16_t other[] = { MODE_USB, MODE_LSB, MODE_AM, MODE_FM, MODE_DRM, MODE_DSTAR };

    for (size_t i = 0; i < sizeof(text) / sizeof(text[0]); i++) {
        radio_profile p = profile(text[i]);
        TEST_ASSERT_TRUE(radio_profile_tx_is_text(&p));
    }
    for (size_t i = 0; i < sizeof(other) / sizeof(other[0]); i++) {
        radio_profile p = profile(other[i]);
        TEST_ASSERT_FALSE(radio_profile_tx_is_text(&p));
    }
}

/* A voice profile with noise reduction on is denoised; the same profile on
 * the loopback (the modem's) or in FT8 is not. */
static void test_denoised(void)
{
    radio_profile p = profile(MODE_USB);
    p.noise_reduction = NOISE_REDUCTION_ON;
    TEST_ASSERT_TRUE(radio_profile_denoised(&p));
    p.operating_mode = OPERATING_MODE_FULL_LOOPBACK;
    TEST_ASSERT_FALSE(radio_profile_denoised(&p));
    p.operating_mode = OPERATING_MODE_FULL_VOICE;
    p.mode = MODE_FT8;
    TEST_ASSERT_FALSE(radio_profile_denoised(&p));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tx_is_text);
    RUN_TEST(test_denoised);
    return UNITY_END();
}
