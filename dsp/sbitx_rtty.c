/* sBitx RTTY (Radio Teletype) modem
 *
 * TX: Baudot encoding + FSK tone generator at 96 kHz.
 * RX: SSB demod → 12 kHz → minimodem FSK detector → Baudot decode.
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

#include "baudot.h"

#include "sbitx_rtty.h"

#define RTTY_RX_SAMPLE_RATE 12000

static int rtty_baud_rate = 45;
static int rtty_mark_hz = 1585;
static int rtty_shift_hz = 170;

static int rtty_samples_per_bit = 0;

/* RX: mark and space tone detectors (mixer + half-bit moving average), a
 * discriminator, and an asynchronous UART that finds the mark -> space
 * start edge and samples each bit at its centre. The minimodem frame search
 * used before looked for frames only at fixed block starts, read the
 * stop/start bits as data, and overran its FFT buffer (bw 50 Hz gave 240
 * samples for 266-sample bits) on every attempt. */
#define RTTY_Q_MIN 0.6f            /* mean |mark - space| / (mark + space) */
#define RTTY_MAX_L 1024

static float  rx_spb;              /* samples per bit (12 kHz) */
static int    rx_L;                /* moving-average length: half a bit */
static float  rx_rot_m[2], rx_rot_s[2], rx_osc_m[2], rx_osc_s[2];
static float  rx_buf_m[RTTY_MAX_L][2], rx_buf_s[RTTY_MAX_L][2];
static float  rx_sum_m[2], rx_sum_s[2];
static int    rx_i;
static uint64_t rx_t;              /* samples since init */
static int    rx_mark_run;         /* samples of mark before a start edge */
static bool   rx_in_frame;
static double rx_start_t;          /* sample time of the start bit's edge */
static int    rx_bitno;            /* next bit to sample: 0 start .. 6 stop */
static unsigned rx_word;
static float  rx_q;
static float  rx_prev_d;

bool sbitx_rtty_init(int baud, int mark, int shift)
{
    if (baud <= 0)
        return false;
    rtty_baud_rate = baud;
    rtty_mark_hz = mark;
    rtty_shift_hz = shift;
    rtty_samples_per_bit = RTTY_RX_SAMPLE_RATE / baud;

    rx_spb = (float) RTTY_RX_SAMPLE_RATE / (float) baud;
    rx_L = (int) (rx_spb / 2.0f);
    if (rx_L < 4) rx_L = 4;
    if (rx_L > RTTY_MAX_L) rx_L = RTTY_MAX_L;
    float wm = 2.0f * (float) M_PI * mark / RTTY_RX_SAMPLE_RATE;
    float ws = 2.0f * (float) M_PI * (mark - shift) / RTTY_RX_SAMPLE_RATE;
    rx_rot_m[0] = cosf(wm); rx_rot_m[1] = -sinf(wm);
    rx_rot_s[0] = cosf(ws); rx_rot_s[1] = -sinf(ws);
    rx_osc_m[0] = rx_osc_s[0] = 1.0f;
    rx_osc_m[1] = rx_osc_s[1] = 0.0f;
    memset(rx_buf_m, 0, sizeof(rx_buf_m));
    memset(rx_buf_s, 0, sizeof(rx_buf_s));
    rx_sum_m[0] = rx_sum_m[1] = rx_sum_s[0] = rx_sum_s[1] = 0.0f;
    rx_i = 0;
    rx_t = 0;
    rx_mark_run = 0;
    rx_in_frame = false;
    rx_prev_d = 0.0f;
    baudot_reset();
    return true;
}

void sbitx_rtty_shutdown(void)
{
}

/* FSK TX state for one message: continuous phase per tone. */
typedef struct {
    float *signal;
    int    max_n;
    int    idx;
    int    samples_per_bit;
    int    slope_n;
    float  phase_mark, phase_space;
    float  dds_mark, dds_space;
} rtty_tx;

/* n samples of mark (1) or space (0). */
static void rtty_tone(rtty_tx *t, bool mark, int n)
{
    for (int i = 0; i < n && t->idx < t->max_n; i++, t->idx++)
    {
        float *ph = mark ? &t->phase_mark : &t->phase_space;
        *ph += mark ? t->dds_mark : t->dds_space;
        if (*ph > 2.0f * (float) M_PI) *ph -= 2.0f * (float) M_PI;
        t->signal[t->idx] = 0.4f * sinf(*ph);
    }
}

/* One 5-bit ITA2 word: start (space), 5 data bits LSB first with 1 = mark,
 * 1.5 stop bits (mark). */
static void rtty_word(rtty_tx *t, unsigned int word)
{
    rtty_tone(t, false, t->samples_per_bit);
    for (int d = 0; d < 5; d++)
        rtty_tone(t, ((word >> d) & 1) != 0, t->samples_per_bit);
    rtty_tone(t, true, t->samples_per_bit + t->samples_per_bit / 2);
}

int sbitx_rtty_encode(const char *message, float *signal, int max_n,
                      int baud, int mark, int shift)
{
    rtty_tx t = {
        .signal = signal, .max_n = max_n, .idx = 0,
        .samples_per_bit = 96000 / baud,
        .dds_mark = 2.0f * (float) M_PI * mark / 96000.0f,
        .dds_space = 2.0f * (float) M_PI * (mark - shift) / 96000.0f,
    };

    /* A mark lead-in lets the receiver find the first start bit, and LTRS
     * puts it in letters whatever it was left in. baudot_encode() writes
     * one or two words (a LTRS/FIGS shift, then the character) into an
     * array: the old code passed a single unsigned int, so every shift
     * overflowed it. It also sent data bits inverted (1 as space), which
     * no RTTY receiver decodes. */
    baudot_reset();
    rtty_tone(&t, true, t.samples_per_bit * 8);          /* ~180 ms */
    rtty_word(&t, 0x1F);                                  /* LTRS */
    size_t len = strlen(message);
    bool has_eol = len && (message[len - 1] == '\n' || message[len - 1] == '\r');
    for (const char *p = message; *p && t.idx < max_n; p++)
    {
        unsigned int words[2] = {0, 0};
        int nw = baudot_encode(words, *p);
        for (int w = 0; w < nw && w < 2; w++)
            rtty_word(&t, words[w]);
    }
    if (!has_eol) {                        /* end the line, as RTTY messages do */
        unsigned int words[2] = {0, 0};
        int nw = baudot_encode(words, '\r');
        for (int w = 0; w < nw && w < 2; w++)
            rtty_word(&t, words[w]);
        nw = baudot_encode(words, '\n');
        for (int w = 0; w < nw && w < 2; w++)
            rtty_word(&t, words[w]);
    }
    rtty_tone(&t, true, t.samples_per_bit * 4);          /* mark tail */
    return t.idx;
}

int sbitx_rtty_rx_samples_per_block(void)
{
    return rtty_samples_per_bit * 2;
}

static inline void osc_step(float osc[2], const float rot[2])
{
    float re = osc[0] * rot[0] - osc[1] * rot[1];
    float im = osc[0] * rot[1] + osc[1] * rot[0];
    osc[0] = re;
    osc[1] = im;
}

void sbitx_rtty_rx_process(const float *audio_12k, int n,
                           int baud, int mark, int shift,
                           void (*char_cb)(char))
{
    if (!char_cb || n <= 0)
        return;
    if (rx_spb <= 0.0f || baud != rtty_baud_rate || mark != rtty_mark_hz || shift != rtty_shift_hz)
        if (!sbitx_rtty_init(baud, mark, shift))
            return;

    for (int k = 0; k < n; k++) {
        const float x = audio_12k[k];

        /* Mix each tone to DC and average over half a bit. */
        float pm[2] = { x * rx_osc_m[0], x * rx_osc_m[1] };
        float ps[2] = { x * rx_osc_s[0], x * rx_osc_s[1] };
        osc_step(rx_osc_m, rx_rot_m);
        osc_step(rx_osc_s, rx_rot_s);
        rx_sum_m[0] += pm[0] - rx_buf_m[rx_i][0];
        rx_sum_m[1] += pm[1] - rx_buf_m[rx_i][1];
        rx_sum_s[0] += ps[0] - rx_buf_s[rx_i][0];
        rx_sum_s[1] += ps[1] - rx_buf_s[rx_i][1];
        rx_buf_m[rx_i][0] = pm[0]; rx_buf_m[rx_i][1] = pm[1];
        rx_buf_s[rx_i][0] = ps[0]; rx_buf_s[rx_i][1] = ps[1];
        if (++rx_i == rx_L)
            rx_i = 0;
        rx_t++;
        if ((rx_t & 1023) == 0) {        /* keep the oscillators on the unit circle */
            float gm = 1.0f / hypotf(rx_osc_m[0], rx_osc_m[1]);
            float gs = 1.0f / hypotf(rx_osc_s[0], rx_osc_s[1]);
            rx_osc_m[0] *= gm; rx_osc_m[1] *= gm;
            rx_osc_s[0] *= gs; rx_osc_s[1] *= gs;
        }

        float em = hypotf(rx_sum_m[0], rx_sum_m[1]);
        float es = hypotf(rx_sum_s[0], rx_sum_s[1]);
        float d = em - es;                       /* > 0: mark */
        float sum = em + es;

        if (!rx_in_frame) {
            /* A start bit: space after at least a bit of steady mark (half a
             * bit let the transmitter's rise start a false frame that ate
             * the leading LTRS). The averaging delays the crossing by half
             * its length. */
            if (d > 0.0f) {
                rx_mark_run++;
            } else {
                if (rx_prev_d > 0.0f && rx_mark_run >= (int) rx_spb) {
                    rx_in_frame = true;
                    rx_start_t = (double) rx_t - rx_L / 2.0;
                    rx_bitno = 0;
                    rx_word = 0;
                    rx_q = 0.0f;
                }
                rx_mark_run = 0;
            }
        } else if ((double) rx_t >= rx_start_t + (rx_bitno + 0.5) * rx_spb + rx_L / 2.0) {
            /* The centre of bit rx_bitno has left the averager. */
            bool is_mark = d > 0.0f;
            rx_q += sum > 1e-9f ? fabsf(d) / sum : 0.0f;
            bool ok = true;
            if (rx_bitno == 0)
                ok = !is_mark;                   /* start bit is space */
            else if (rx_bitno <= 5)
                rx_word |= (is_mark ? 1u : 0u) << (rx_bitno - 1);   /* LSB first, 1 = mark */
            else {
                ok = is_mark;                    /* stop bit is mark */
                if (ok && rx_q / 7.0f >= RTTY_Q_MIN) {
                    char ch = 0;
                    if (baudot_decode(&ch, (unsigned char) rx_word))
                        char_cb(ch);
                }
                rx_in_frame = false;
                rx_mark_run = (int) rx_spb;   /* the stop bit counts as idle mark */
            }
            if (!ok) {
                rx_in_frame = false;
                rx_mark_run = 0;
            } else if (rx_in_frame) {
                rx_bitno++;
            }
        }
        rx_prev_d = d;
    }
}
