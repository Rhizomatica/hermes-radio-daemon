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

#include <errno.h>
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
#define CW_GOERTZEL_N 128          /* 10.67 ms per detector decision */

static cw_rec_t *cw_rec = NULL;
static int cw_pitch_hz = 700;
static int cw_wpm = 20;

static float goertzel_coeff;
static float goertzel_q1, goertzel_q2;
static int goertzel_count = 0;

/* Tone detector state. Levels are tone amplitudes (the Goertzel magnitude
 * scaled by 2/N), so they do not depend on the audio's absolute scale.
 * The old detector started "high" at 500 and never let it fall, while a
 * full-scale tone measures ~64 on this Goertzel: no mark was ever seen. */
static float peak_level = 0.0f;       /* recent tone level: fast attack, slow decay */
static float noise_level = 0.0f;      /* between marks: fast fall, slow rise */
static bool  tone_on = false;
static bool  levels_seeded = false;
static int   tone_pending = 0;        /* consecutive decisions disagreeing */

/* Receiver time comes from the sample count, not the wall clock: audio
 * arrives in bursts, and gettimeofday() gave marks that were jittered,
 * or of zero length when a burst held several decisions. */
static uint64_t cw_samples = 0;
static bool  cw_have_marks = false;   /* marks since the last character */
static bool  cw_char_done = false;    /* character given out; word gap pending */

static struct timeval cw_now(void)
{
    struct timeval tv;
    tv.tv_sec = (time_t) (cw_samples / CW_SAMPLE_RATE);
    tv.tv_usec = (suseconds_t) ((cw_samples % CW_SAMPLE_RATE) * 1000000ULL / CW_SAMPLE_RATE);
    return tv;
}

static void cw_detector_reset(void)
{
    float omega = 2.0f * (float) M_PI * cw_pitch_hz / CW_SAMPLE_RATE;
    goertzel_coeff = 2.0f * cosf(omega);
    goertzel_q1 = 0.0f;
    goertzel_q2 = 0.0f;
    goertzel_count = 0;
    peak_level = 0.0f;
    noise_level = 0.0f;
    levels_seeded = false;
    tone_on = false;
    tone_pending = 0;
    cw_have_marks = false;
    cw_char_done = false;
}

bool sbitx_cw_init(int wpm, int pitch)
{
    cw_pitch_hz = pitch;
    if (wpm > 0)
        cw_wpm = wpm;

    if (!cw_rec)
    {
        cw_rec = cw_rec_new();
        if (!cw_rec)
            return false;
    }
    cw_rec_reset_state(cw_rec);
    /* Fixed speed at the configured WPM (libcw tolerates the usual
     * spread). Adaptive mode misread the first character of every
     * transmission while it settled: "HERMES" came out "ETNERMES". */
    cw_rec_disable_adaptive_mode(cw_rec);
    cw_rec_set_speed(cw_rec, cw_wpm);
    cw_detector_reset();
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
    if (wpm <= 0)
        return;
    cw_wpm = wpm;
    if (cw_rec)
        cw_rec_set_speed(cw_rec, cw_wpm);
}

void sbitx_cw_set_pitch(int pitch)
{
    cw_pitch_hz = pitch;
    cw_detector_reset();
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
        /* unsigned: a non-ASCII byte (UTF-8 text from the web panel) was a
         * negative index into morse_tx. */
        unsigned char c = (unsigned char) *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        const char *sym = (c < 128) ? morse_tx[c] : NULL;
        if (!sym) continue;

        if (pi > 0) { pattern[pi++] = ' '; }
        for (const char *s = sym; *s && pi < max_pat - 1; s++)
            pattern[pi++] = *s;
        pattern[pi] = '\0';
    }
    return pi;
}

/* Silence or a keyed tone of n samples at 96 kHz; tones get raised-cosine
 * edges inside their own duration. */
static int cw_emit(float *signal, int idx, int max_n, int n, bool key_on,
                   int slope_n, float *phase, float dds_step)
{
    for (int i = 0; i < n && idx < max_n; i++, idx++)
    {
        if (!key_on) {
            signal[idx] = 0.0f;
            continue;
        }
        float env = 1.0f;
        if (i < slope_n)
            env = 0.5f - 0.5f * cosf((float) M_PI * i / slope_n);
        else if (i >= n - slope_n)
            env = 0.5f + 0.5f * cosf((float) M_PI * (i - n + slope_n) / slope_n);
        *phase += dds_step;
        if (*phase > 2.0f * (float) M_PI) *phase -= 2.0f * (float) M_PI;
        signal[idx] = env * sinf(*phase);
    }
    if (!key_on)
        *phase = 0.0f;
    return idx;
}

int sbitx_cw_encode(const char *message, float *signal, int max_n,
                    int wpm, int pitch)
{
    char pattern[1024];
    lookup_morse(message, pattern, sizeof(pattern));

    /* PARIS: 1 dot = 1.2 s / WPM. Gaps: 1 dot between the elements of a
     * character, 3 between characters, 7 between words. The pattern holds
     * characters separated by ' ' and words as " / ", so ' ' is 3 dots and
     * '/' 1 more (3 + 1 + 3 = 7). The element gap was missing: "H" went out
     * as one long tone, and no receiver could decode anything. */
    const int dot_n = (int) (1.2f / (float) wpm * 96000.0f);
    const int slope_n = (int) (0.005f * 96000.0f);
    float phase = 0.0f;
    const float dds_step = 2.0f * (float) M_PI * pitch / 96000.0f;
    int idx = 0;
    bool prev_element = false;

    for (int pos = 0; pattern[pos] && idx < max_n; pos++)
    {
        char c = pattern[pos];
        if (c == '.' || c == '-') {
            if (prev_element)
                idx = cw_emit(signal, idx, max_n, dot_n, false, slope_n, &phase, dds_step);
            idx = cw_emit(signal, idx, max_n, c == '.' ? dot_n : 3 * dot_n, true,
                          slope_n, &phase, dds_step);
            prev_element = true;
        } else {
            idx = cw_emit(signal, idx, max_n, c == '/' ? dot_n : 3 * dot_n, false,
                          slope_n, &phase, dds_step);
            prev_element = false;
        }
    }
    /* A character gap at the end, so a receiver sees it finish. */
    idx = cw_emit(signal, idx, max_n, 3 * dot_n, false, slope_n, &phase, dds_step);
    return idx;
}

int sbitx_cw_rx_samples_per_block(void)
{
    return CW_GOERTZEL_N;
}

/* One detector decision (a Goertzel block). Returns chars written. */
static int cw_decide(float amp, char *decoded, int max_len)
{
    int out = 0;

    /* Levels: the noise is the mean level in the gaps; the peak is the
     * recent mark level (instant attack, slow decay). A mark starts above
     * 4x the noise mean (a Rayleigh noise sample gets there with p ~ 4e-6,
     * twice in a row ~1e-11), halfway to the peak, and a tiny absolute
     * floor (digital silence has no noise to measure against); it ends
     * 35% of the way from noise to peak. */
    if (!levels_seeded) {                   /* digital silence reads 0: seed once */
        noise_level = amp;
        levels_seeded = true;
    }
    if (!tone_on) {
        /* Readings from a tone's edges (gaps between elements are only ~5
         * decisions long) are not noise: they only nudge it up. */
        if (amp < 2.0f * noise_level || noise_level < 1e-6f)
            noise_level += 0.05f * (amp - noise_level);
        else
            noise_level *= 1.01f;
    }
    if (amp > peak_level)
        peak_level = amp;
    else
        peak_level *= 0.998f;                      /* ~17%/s */

    float on_th = noise_level + 0.5f * (peak_level - noise_level);
    if (on_th < 4.0f * noise_level)
        on_th = 4.0f * noise_level;
    if (on_th < 1e-4f)
        on_th = 1e-4f;
    float off_th = noise_level + 0.35f * (peak_level - noise_level);
    bool want = tone_on ? amp > off_th : amp > on_th;

    /* Two agreeing decisions (~21 ms) to change state: rejects clicks. */
    if (want != tone_on) {
        if (++tone_pending < 2)
            want = tone_on;
    } else {
        tone_pending = 0;
    }

    struct timeval now = cw_now();
    if (want && !tone_on) {
        /* A new mark after a finished character: the gap was only
         * between characters, so drop the character we already gave. */
        if (cw_char_done) {
            cw_rec_reset_state(cw_rec);
            cw_char_done = false;
        }
        if (cw_rec_mark_begin(cw_rec, &now) != CW_SUCCESS) {
            /* libcw refuses marks while in an error state; start over. */
            cw_rec_reset_state(cw_rec);
            cw_rec_mark_begin(cw_rec, &now);
        }
        cw_have_marks = true;
    } else if (!want && tone_on) {
        /* A mark of no valid length (a noise burst, a fading dot) puts
         * libcw in an error state that refuses every later mark until a
         * reset: one bad mark used to silence CW decoding for good. */
        if (cw_rec_mark_end(cw_rec, &now) != CW_SUCCESS) {
            cw_rec_reset_state(cw_rec);
            cw_have_marks = false;
            cw_char_done = false;
        }
    } else if (!want && cw_have_marks) {
        /* In a gap: ask libcw whether it now holds a character, and
         * later whether the gap has grown into a word space. */
        char ch = 0;
        bool is_iws = false, is_err = false;
        cw_ret_t prc = cw_rec_poll_character(cw_rec, &now, &ch, &is_iws, &is_err);
        if (prc != CW_SUCCESS && errno != EAGAIN && errno != ERANGE) {
            /* Not "wait, the gap is still short": a broken character. */
            cw_rec_reset_state(cw_rec);
            cw_have_marks = false;
            cw_char_done = false;
        } else if (prc == CW_SUCCESS) {
            if (!cw_char_done) {
                if (!is_err && ch && out < max_len - 1)
                    decoded[out++] = ch;
                cw_char_done = true;
            }
            if (is_iws) {
                if (out < max_len - 1)
                    decoded[out++] = ' ';
                cw_rec_reset_state(cw_rec);
                cw_have_marks = false;
                cw_char_done = false;
            }
        }
    }
    tone_on = want;
    decoded[out] = '\0';
    return out;
}

int sbitx_cw_rx_process(const float *audio_12k, int n, char *decoded, int max_len,
                         int wpm, int pitch)
{
    decoded[0] = '\0';
    if (!cw_rec || max_len < 2) return 0;

    /* Follow the configured speed and pitch (digi_config cw_wpm/cw_pitch);
     * they used to be ignored after init. */
    if (wpm > 0 && wpm != cw_wpm)
        sbitx_cw_set_wpm(wpm);
    if (pitch > 0 && pitch != cw_pitch_hz)
        sbitx_cw_set_pitch(pitch);

    int out = 0;
    for (int i = 0; i < n; i++)
    {
        float q0 = goertzel_coeff * goertzel_q1 - goertzel_q2 + audio_12k[i];
        goertzel_q2 = goertzel_q1;
        goertzel_q1 = q0;
        cw_samples++;
        if (++goertzel_count < CW_GOERTZEL_N)
            continue;

        float mag2 = goertzel_q1 * goertzel_q1 + goertzel_q2 * goertzel_q2 -
                     goertzel_coeff * goertzel_q1 * goertzel_q2;
        float amp = sqrtf(mag2 > 0.0f ? mag2 : 0.0f) * 2.0f / CW_GOERTZEL_N;
        goertzel_q1 = 0.0f;
        goertzel_q2 = 0.0f;
        goertzel_count = 0;

        out += cw_decide(amp, decoded + out, max_len - out);
    }
    return out;
}
