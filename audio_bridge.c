/* hermes-radio-daemon - generic audio bridge implementation.
 *
 * Uses csdr's rational_resampler_ff (polyphase windowed-sinc taps via
 * rational_resampler_get_lowpass_f + firdes_filter_len). Same call pattern
 * as the existing 48->96 kHz loopback in dsp/sbitx_dsp.c:772-787.
 *
 * Copyright (C) 2024-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_bridge.h"
#include "radio_media.h"
#include "sbitx/sbitx_core.h"   /* sbitx_bridge_push_rx / sbitx_bridge_pop_tx */

static unsigned gcd_u32(unsigned a, unsigned b)
{
    while (b) { unsigned t = b; b = a % b; a = t; }
    return a ? a : 1;
}

static void i16_to_f(const int16_t *in, float *out, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = (float) in[i] * (1.0f / 32768.0f);
}

static void f_to_i16(const float *in, int16_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float v = in[i] * 32768.0f;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        out[i] = (int16_t) v;
    }
}

static bool grow_scratch(audio_bridge *b, size_t needed_in, size_t needed_out)
{
    if (needed_in > b->scratch_cap_in) {
        float *p = realloc(b->scratch_f_in, needed_in * sizeof(float));
        if (!p) return false;
        b->scratch_f_in = p;
        b->scratch_cap_in = needed_in;
    }
    if (needed_out > b->scratch_cap_out) {
        float   *po = realloc(b->scratch_f_out, needed_out * sizeof(float));
        int16_t *pi = realloc(b->scratch_i16,   needed_out * sizeof(int16_t));
        if (!po || !pi) {
            free(po); free(pi);
            return false;
        }
        b->scratch_f_out = po;
        b->scratch_i16   = pi;
        b->scratch_cap_out = needed_out;
    }
    return true;
}

static bool build_taps(float **out_taps, int *out_len, int interp, int decim)
{
    float transition_bw = 0.05f;
    int taps_len = firdes_filter_len(transition_bw);
    float *taps = malloc(taps_len * sizeof(float));
    if (!taps)
        return false;
    rational_resampler_get_lowpass_f(taps, taps_len, interp, decim, WINDOW_BLACKMAN);
    *out_taps = taps;
    *out_len  = taps_len;
    return true;
}

bool audio_bridge_init(audio_bridge *b, uint32_t native_rate, uint32_t dsp_rate)
{
    if (!b || !native_rate || !dsp_rate)
        return false;

    memset(b, 0, sizeof(*b));
    b->native_rate = native_rate;
    b->dsp_rate    = dsp_rate;

    if (native_rate == dsp_rate)
        return true;

    unsigned g = gcd_u32(native_rate, dsp_rate);
    b->interp_up   = (int)(dsp_rate    / g);
    b->decim_up    = (int)(native_rate / g);
    b->interp_down = (int)(native_rate / g);
    b->decim_down  = (int)(dsp_rate    / g);

    /* Refuse pathological rate pairs (huge gcd-reduced factors blow up taps
     * and CPU). For our practical 48k/96k pair this is 2/1, well below. */
    if (b->interp_up > 256 || b->decim_up > 256) {
        fprintf(stderr,
                "audio_bridge: rate pair %u<->%u reduces to %d/%d — too "
                "large, falling back to pass-through (no resample)\n",
                native_rate, dsp_rate, b->interp_up, b->decim_up);
        b->native_rate = b->dsp_rate;
        return true;
    }

    if (!build_taps(&b->taps_up,   &b->taps_up_len,   b->interp_up,   b->decim_up) ||
        !build_taps(&b->taps_down, &b->taps_down_len, b->interp_down, b->decim_down)) {
        fprintf(stderr, "audio_bridge: tap allocation failed\n");
        free(b->taps_up);   b->taps_up   = NULL;
        free(b->taps_down); b->taps_down = NULL;
        return false;
    }
    return true;
}

void audio_bridge_shutdown(audio_bridge *b)
{
    if (!b) return;
    free(b->taps_up);
    free(b->taps_down);
    free(b->scratch_f_in);
    free(b->scratch_f_out);
    free(b->scratch_i16);
    memset(b, 0, sizeof(*b));
}

/* Resample n_in samples (native_rate, int16) up to dsp_rate; write into the
 * bridge scratch_i16, return count. Caller pushes from scratch_i16. */
static size_t resample_up(audio_bridge *b, const int16_t *samples, size_t n_in)
{
    size_t n_out_cap = (size_t)((double) n_in *
                                (double) b->interp_up / (double) b->decim_up) + 32;
    if (!grow_scratch(b, n_in, n_out_cap))
        return 0;

    i16_to_f(samples, b->scratch_f_in, n_in);

    b->state_up = rational_resampler_ff(b->scratch_f_in, b->scratch_f_out,
                                        (int) n_in,
                                        b->interp_up, b->decim_up,
                                        b->taps_up, b->taps_up_len,
                                        b->state_up.last_taps_delay);
    int n_out = b->state_up.output_size;
    if (n_out <= 0)
        return 0;
    f_to_i16(b->scratch_f_out, b->scratch_i16, (size_t) n_out);
    return (size_t) n_out;
}

/* Resample n_in samples (dsp_rate, int16) down to native_rate; write into
 * the bridge scratch_i16, return count. Caller consumes from scratch_i16. */
static size_t resample_down(audio_bridge *b, const int16_t *samples, size_t n_in)
{
    size_t n_out_cap = (size_t)((double) n_in *
                                (double) b->interp_down / (double) b->decim_down) + 32;
    if (!grow_scratch(b, n_in, n_out_cap))
        return 0;

    i16_to_f(samples, b->scratch_f_in, n_in);

    b->state_down = rational_resampler_ff(b->scratch_f_in, b->scratch_f_out,
                                          (int) n_in,
                                          b->interp_down, b->decim_down,
                                          b->taps_down, b->taps_down_len,
                                          b->state_down.last_taps_delay);
    int n_out = b->state_down.output_size;
    if (n_out <= 0)
        return 0;
    f_to_i16(b->scratch_f_out, b->scratch_i16, (size_t) n_out);
    return (size_t) n_out;
}

void audio_bridge_push_rx_native(audio_bridge *b, radio *radio_h,
                                 const int16_t *samples, size_t n_native)
{
    if (!b || !radio_h || !samples || !n_native)
        return;

    if (b->native_rate == b->dsp_rate) {
        sbitx_bridge_push_rx(radio_h, samples, n_native);
        radio_media_tap_rx_audio(radio_h, samples, n_native);
        return;
    }

    size_t n_out = resample_up(b, samples, n_native);
    if (!n_out)
        return;
    sbitx_bridge_push_rx(radio_h, b->scratch_i16, n_out);
    radio_media_tap_rx_audio(radio_h, b->scratch_i16, n_out);
}

size_t audio_bridge_pop_tx_native(audio_bridge *b, radio *radio_h,
                                  int16_t *out, size_t n_native)
{
    if (!b || !radio_h || !out || !n_native)
        return 0;

    if (b->native_rate == b->dsp_rate) {
        size_t got = sbitx_bridge_pop_tx(radio_h, out, n_native);
        if (got > 0)
            radio_media_tap_tx_audio(radio_h, out, got);
        return got;
    }

    /* Pull exactly n_native * dsp/native samples from the ring so the resampler
     * produces ~n_native output. The old "+16" margin over-pulled: it drained
     * the 8 kHz ring faster than mercury fills it AND the surplus resampled
     * output was capped+discarded below, chopping ~17% out of every period —
     * which shredded the modem signal (TX finished early, far end couldn't
     * decode). Exact ratio keeps drain == fill and plays the whole frame. */
    size_t n_in_want = (size_t)((double) n_native *
                                (double) b->decim_down /
                                (double) b->interp_down);
    if (n_in_want == 0)
        n_in_want = 1;

    /* Use a scratch from the bridge for the int16 ring pull. */
    if (!grow_scratch(b, n_in_want, n_native + 32))
        return 0;

    size_t got_in = sbitx_bridge_pop_tx(radio_h, b->scratch_i16, n_in_want);
    if (got_in == 0)
        return 0;

    /* resample_down writes its int16 result into scratch_i16, overwriting
     * what we just pulled — that's fine because we are not done reading
     * the input until after i16_to_f copied it into scratch_f_in. */
    size_t n_out = resample_down(b, b->scratch_i16, got_in);
    if (n_out > n_native)
        n_out = n_native;
    if (n_out)
        memcpy(out, b->scratch_i16, n_out * sizeof(int16_t));

    if (n_out > 0)
        radio_media_tap_tx_audio(radio_h, out, n_out);
    return n_out;
}

/* Reverse-direction helpers for the headset. Internal helpers operate on
 * the rings directly (the sbitx_bridge_* helpers only target the canonical
 * rx/tx; here we want rx as a source and tx as a sink). */
static size_t pop_ring_to_buf(audio_ring_buffer *ring,
                              int16_t *out, size_t n_want)
{
    if (!ring || !ring->samples || !ring->capacity || !out || !n_want)
        return 0;
    pthread_mutex_lock(&ring->mutex);
    size_t got = 0;
    while (got < n_want && ring->count > 0) {
        out[got++] = ring->samples[ring->read_pos];
        ring->read_pos = (ring->read_pos + 1) % ring->capacity;
        ring->count--;
    }
    pthread_mutex_unlock(&ring->mutex);
    return got;
}

static void push_buf_to_ring(audio_ring_buffer *ring,
                             const int16_t *samples, size_t n)
{
    if (!ring || !ring->samples || !ring->capacity || !samples || !n)
        return;
    pthread_mutex_lock(&ring->mutex);
    for (size_t i = 0; i < n; i++) {
        if (ring->count == ring->capacity) {
            ring->read_pos = (ring->read_pos + 1) % ring->capacity;
            ring->count--;
        }
        ring->samples[ring->write_pos] = samples[i];
        ring->write_pos = (ring->write_pos + 1) % ring->capacity;
        ring->count++;
    }
    pthread_cond_signal(&ring->cond);
    pthread_mutex_unlock(&ring->mutex);
}

size_t audio_bridge_pop_rx_native(audio_bridge *b, radio *radio_h,
                                  int16_t *out, size_t n_native)
{
    if (!b || !radio_h || !out || !n_native)
        return 0;

    if (b->native_rate == b->dsp_rate)
        return pop_ring_to_buf(&radio_h->rx_audio_ring, out, n_native);

    size_t n_in_want = (size_t)((double) n_native *
                                (double) b->decim_down /
                                (double) b->interp_down) + 16;
    if (!grow_scratch(b, n_in_want, n_native + 32))
        return 0;

    size_t got_in = pop_ring_to_buf(&radio_h->rx_audio_ring,
                                    b->scratch_i16, n_in_want);
    if (!got_in)
        return 0;

    size_t n_out = resample_down(b, b->scratch_i16, got_in);
    if (n_out > n_native)
        n_out = n_native;
    if (n_out)
        memcpy(out, b->scratch_i16, n_out * sizeof(int16_t));
    return n_out;
}

void audio_bridge_push_tx_native(audio_bridge *b, radio *radio_h,
                                 const int16_t *samples, size_t n_native)
{
    if (!b || !radio_h || !samples || !n_native)
        return;

    if (b->native_rate == b->dsp_rate) {
        push_buf_to_ring(&radio_h->tx_audio_ring, samples, n_native);
        return;
    }

    size_t n_out = resample_up(b, samples, n_native);
    if (!n_out)
        return;
    push_buf_to_ring(&radio_h->tx_audio_ring, b->scratch_i16, n_out);
}
