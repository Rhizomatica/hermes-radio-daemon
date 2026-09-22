/* sBitx CW (Morse code) modem
 *
 * TX: DDS sine oscillator with raised-cosine envelope.
 * RX: Goertzel single-bin detector + unixcw receiver state machine.
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <libcw2.h>

#include "sbitx_cw.h"

#define CW_SAMPLE_RATE 12000
#define CW_GOERTZEL_N 128

static cw_rec_t *cw_rec = NULL;
static int cw_pitch_hz = 700;

static float goertzel_coeff;
static float goertzel_q1, goertzel_q2;
static int goertzel_count = 0;

static float high_level = 500.0f;
static float noise_floor = 100.0f;
static uint32_t denoise_history = 0;
static bool last_state = false;
static uint32_t cw_ticker = 0;
static struct timeval last_mark_time;

bool sbitx_cw_init(int wpm, int pitch)
{
    (void) wpm;
    cw_pitch_hz = pitch;

    if (!cw_rec)
    {
        cw_rec = cw_rec_new();
        if (!cw_rec)
            return false;
        cw_rec_enable_adaptive_mode(cw_rec);
    }

    float omega = 2.0f * (float) M_PI * cw_pitch_hz / CW_SAMPLE_RATE;
    goertzel_coeff = 2.0f * cosf(omega);
    goertzel_q1 = 0.0f;
    goertzel_q2 = 0.0f;
    goertzel_count = 0;
    high_level = 500.0f;
    noise_floor = 100.0f;
    denoise_history = 0;
    last_state = false;
    cw_ticker = 0;
    gettimeofday(&last_mark_time, NULL);

    return true;
}

void sbitx_cw_shutdown(void)
{
    if (cw_rec)
    {
        cw_rec_delete(&cw_rec);
        cw_rec = NULL;
    }
}

void sbitx_cw_set_wpm(int wpm)
{
    (void) wpm;
}

void sbitx_cw_set_pitch(int pitch)
{
    cw_pitch_hz = pitch;
    float omega = 2.0f * (float) M_PI * cw_pitch_hz / CW_SAMPLE_RATE;
    goertzel_coeff = 2.0f * cosf(omega);
    goertzel_q1 = 0.0f;
    goertzel_q2 = 0.0f;
    goertzel_count = 0;
}

static int lookup_morse(const char *text, char *pattern, int max_pat)
{
    static const char *morse_tx[128] = {
        ['a'] = ".-",    ['b'] = "-...",  ['c'] = "-.-.",  ['d'] = "-..",
        ['e'] = ".",     ['f'] = "..-.",  ['g'] = "--.",   ['h'] = "....",
        ['i'] = "..",    ['j'] = ".---",  ['k'] = "-.-",   ['l'] = ".-..",
        ['m'] = "--",    ['n'] = "-.",    ['o'] = "---",   ['p'] = ".--.",
        ['q'] = "--.-",  ['r'] = ".-.",   ['s'] = "...",   ['t'] = "-",
        ['u'] = "..-",   ['v'] = "...-",  ['w'] = ".--",   ['x'] = "-..-",
        ['y'] = "-.--",  ['z'] = "--..",
        ['0'] = "-----", ['1'] = ".----", ['2'] = "..---", ['3'] = "...--",
        ['4'] = "....-", ['5'] = ".....", ['6'] = "-....", ['7'] = "--...",
        ['8'] = "---..", ['9'] = "----.",
        ['/'] = "-..-.", ['?'] = "..--..",['='] = "-...-", ['.'] = ".-.-.-",
        [','] = "--..--",[' '] = "/",
    };

    int pi = 0;
    for (const char *p = text; *p && pi < max_pat - 2; p++)
    {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        const char *sym = (c < 128) ? morse_tx[(int)c] : NULL;
        if (!sym) continue;

        if (pi > 0) { pattern[pi++] = ' '; }
        for (const char *s = sym; *s && pi < max_pat - 1; s++)
            pattern[pi++] = *s;
        pattern[pi] = '\0';
    }
    return pi;
}

int sbitx_cw_encode(const char *message, float *signal, int max_n,
                    int wpm, int pitch)
{
    char pattern[1024];
    lookup_morse(message, pattern, sizeof(pattern));

    /* PARIS standard: 1 word = 50 dot units, so 1 dot = 1200 ms / WPM
     * = 1_200_000 us / WPM. The previous constant 60_000_000 made one
     * dot last 50× longer (3 s at 20 WPM). */
    float dot_us = 1200000.0f / (float) wpm;
    float dash_us = dot_us * 3.0f;
    float ele_space_us = dot_us;
    float char_space_us = dot_us * 3.0f;
    float word_space_us = dot_us * 7.0f;
    float slope_us = 5000.0f;

    float phase = 0.0f;
    float dds_step = 2.0f * (float) M_PI * pitch / 96000.0f;
    int idx = 0;
    int pos = 0;

    while (pattern[pos] && idx < max_n)
    {
        float duration_us;
        int key_on;

        switch (pattern[pos])
        {
        case '.': duration_us = dot_us;        key_on = 1; break;
        case '-': duration_us = dash_us;        key_on = 1; break;
        case ' ':
        default:  duration_us = char_space_us;  key_on = 0; break;
        case '/':  duration_us = word_space_us; key_on = 0; break;
        }

        /* Sample count at the 96 kHz TX rate (the DDS below steps at
         * 2π·pitch/96000). 96000 samples/s = 0.096 samples/µs; the old
         * factor 96/1e6 was 1000× too small (~5 samples per dot), so the
         * keyer emitted only a few ms of tone then silence. */
        int n_samples = (int)(duration_us * 96000.0f / 1000000.0f);
        int slope_n = (int)(slope_us * 96000.0f / 1000000.0f);

        for (int i = 0; i < n_samples && idx < max_n; i++, idx++)
        {
            float env;
            if (!key_on && i >= slope_n)
            {
                env = 0.0f;
            }
            else if (!key_on)
            {
                env = 0.5f + 0.5f * cosf((float) M_PI * i / slope_n);
            }
            else if (i < slope_n)
            {
                env = 0.5f - 0.5f * cosf((float) M_PI * i / slope_n);
            }
            else if (i >= n_samples - slope_n)
            {
                env = 0.5f + 0.5f * cosf((float) M_PI * (i - n_samples + slope_n) / slope_n);
            }
            else
            {
                env = 1.0f;
            }

            phase += dds_step;
            if (phase > 2.0f * (float) M_PI) phase -= 2.0f * (float) M_PI;
            float tone = env * sinf(phase);

            if (!key_on && i < slope_n && env < 0.01f)
            {
                float remain = 0.0f;
                signal[idx] = remain;
                phase = 0.0f;
            }
            else
            {
                signal[idx] = tone;
            }
        }
        pos++;
    }

    return idx;
}

int sbitx_cw_rx_samples_per_block(void)
{
    return CW_GOERTZEL_N;
}

int sbitx_cw_rx_process(const float *audio_12k, int n, char *decoded, int max_len,
                         int wpm, int pitch)
{
    (void) wpm;
    (void) pitch;

    decoded[0] = '\0';
    if (!cw_rec) return 0;

    float mag = 0.0f;

    for (int i = 0; i < n; i++)
    {
        float q0 = goertzel_coeff * goertzel_q1 - goertzel_q2 + audio_12k[i];
        goertzel_q2 = goertzel_q1;
        goertzel_q1 = q0;
        goertzel_count++;
    }

    if (goertzel_count >= CW_GOERTZEL_N)
    {
        float real_part = goertzel_q1 - goertzel_q2 * cosf(2.0f * (float) M_PI *
                              cw_pitch_hz * CW_GOERTZEL_N / CW_SAMPLE_RATE);
        float imag_part = goertzel_q2 * sinf(2.0f * (float) M_PI *
                              cw_pitch_hz * CW_GOERTZEL_N / CW_SAMPLE_RATE);
        mag = sqrtf(real_part * real_part + imag_part * imag_part);

        goertzel_q1 = 0.0f;
        goertzel_q2 = 0.0f;
        goertzel_count = 0;
    }
    else
    {
        return 0;
    }

    float decay = 0.99f;
    high_level = decay * high_level + (1.0f - decay) * (mag > high_level ? mag : high_level);
    noise_floor = decay * noise_floor + (1.0f - decay) * (mag < noise_floor ? mag : noise_floor);
    if (noise_floor < 50.0f) noise_floor = 50.0f;

    float mark_thresh = noise_floor + (high_level - noise_floor) * 0.6f;
    float space_thresh = noise_floor + (high_level - noise_floor) * 0.4f;

    denoise_history = ((denoise_history << 1) | (mag > mark_thresh ? 1 : 0)) & 0xFFFFFFFF;

    bool tone_present = last_state;
    if (!last_state && (denoise_history & 0xF) >= 3)
        tone_present = true;
    else if (last_state && (denoise_history & 0xF) <= 1)
        tone_present = false;

    cw_ticker++;

    int result = 0;
    struct timeval now;
    gettimeofday(&now, NULL);

    if (tone_present && !last_state)
    {
        last_mark_time = now;
        cw_rec_mark_begin(cw_rec, &last_mark_time);
    }
    else if (!tone_present && last_state)
    {
        cw_rec_mark_end(cw_rec, &now);
        char ch;
        bool is_iws = false, is_err = false;
        int rc = cw_rec_poll_character(cw_rec, &now, &ch, &is_iws, &is_err);
        if (rc == 1 && !is_err && !is_iws)
        {
            decoded[0] = ch;
            decoded[1] = '\0';
            result = 1;
        }
        else if (rc == 1 && is_iws)
        {
            decoded[0] = ' ';
            decoded[1] = '\0';
            result = 1;
        }
    }

    last_state = tone_present;
    return result;
}
