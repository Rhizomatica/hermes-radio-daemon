/* hermes-radio-daemon - hamlib digital-mode pump
 *
 * Copyright (C) 2024-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One thread, gated on the active profile's mode + txrx_state. On
 * digital modes (FT8/CW/RTTY) it drives the corresponding encoder when
 * the rig is keyed and the decoder when it's receiving. RADAE is handled
 * in radio_media.c (it needs SSB mod/demod which radio_media's
 * capture/playback threads are the right place for).
 */

#define _GNU_SOURCE

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define USE_FFTW
#define LIBCSDR_GPL
#include <fft_fftw.h>
#include <libcsdr.h>

#include "hamlib_digi.h"
#include "../dsp/sbitx_cw.h"
#include "../dsp/sbitx_ft8.h"
#include "../dsp/sbitx_rtty.h"

extern _Atomic bool shutdown_;

#define DIGI_DECODE_RATE 12000     /* CW/FT8/RTTY decoders all expect 12 kHz */
#define FT8_SLOT_SECONDS 15

/* Small wrapper around csdr's rational_resampler_ff for one direction. */
typedef struct {
    int      interp;
    int      decim;
    float   *taps;
    int      taps_len;
    rational_resampler_ff_t state;
    float   *out;          /* scratch */
    size_t   out_cap;
} resamp_state;

static unsigned gcd_u32(unsigned a, unsigned b)
{
    while (b) { unsigned t = b; b = a % b; a = t; }
    return a ? a : 1;
}

static bool resamp_init(resamp_state *r, uint32_t from_rate, uint32_t to_rate)
{
    memset(r, 0, sizeof(*r));
    if (from_rate == to_rate)
        return true;

    unsigned g = gcd_u32(from_rate, to_rate);
    r->interp = (int)(to_rate   / g);
    r->decim  = (int)(from_rate / g);

    float transition_bw = 0.05f;
    r->taps_len = firdes_filter_len(transition_bw);
    r->taps = malloc(r->taps_len * sizeof(float));
    if (!r->taps)
        return false;
    rational_resampler_get_lowpass_f(r->taps, r->taps_len,
                                     r->interp, r->decim, WINDOW_BLACKMAN);
    return true;
}

static void resamp_free(resamp_state *r)
{
    free(r->taps);
    free(r->out);
    memset(r, 0, sizeof(*r));
}

static size_t resamp_apply(resamp_state *r, const float *in, size_t n_in)
{
    if (!r->taps) {
        /* Pass-through: caller may use `in` directly; we still copy to
         * keep the same out-pointer contract. */
        if (n_in > r->out_cap) {
            float *p = realloc(r->out, n_in * sizeof(float));
            if (!p) return 0;
            r->out = p;
            r->out_cap = n_in;
        }
        memcpy(r->out, in, n_in * sizeof(float));
        return n_in;
    }
    size_t cap = (size_t)((double) n_in * (double) r->interp / (double) r->decim) + 32;
    if (cap > r->out_cap) {
        float *p = realloc(r->out, cap * sizeof(float));
        if (!p) return 0;
        r->out = p;
        r->out_cap = cap;
    }
    r->state = rational_resampler_ff((float *) in, r->out,
                                     (int) n_in,
                                     r->interp, r->decim,
                                     r->taps, r->taps_len,
                                     r->state.last_taps_delay);
    return (size_t) r->state.output_size;
}

/* ─── ring helpers ────────────────────────────────────────────── */

static size_t ring_pop_i16(audio_ring_buffer *ring, int16_t *out, size_t n_want)
{
    if (!ring->samples || !ring->capacity || !out || !n_want)
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

static void ring_push_f(audio_ring_buffer *ring, const float *in, size_t n)
{
    if (!ring->samples || !ring->capacity || !in || !n)
        return;
    pthread_mutex_lock(&ring->mutex);
    for (size_t i = 0; i < n; i++) {
        if (ring->count == ring->capacity) {
            ring->read_pos = (ring->read_pos + 1) % ring->capacity;
            ring->count--;
        }
        float v = in[i] * 32767.0f;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        ring->samples[ring->write_pos] = (int16_t) v;
        ring->write_pos = (ring->write_pos + 1) % ring->capacity;
        ring->count++;
    }
    pthread_cond_signal(&ring->cond);
    pthread_mutex_unlock(&ring->mutex);
}

/* ─── per-mode init/teardown ──────────────────────────────────── */

typedef struct {
    radio *radio_h;
    pthread_t tid;
    bool started;

    /* Encoder rates (TX): CW/RTTY produce 96k, FT8 produces 12k. */
    resamp_state cw_to_ring;
    resamp_state rtty_to_ring;
    resamp_state ft8_to_ring;

    /* Decoder rate (RX): ring_rate -> 12k. */
    resamp_state ring_to_12k;

    /* Per-mode init guards. */
    bool cw_inited;
    bool ft8_inited;
    bool rtty_inited;

    /* CW RX accumulator (Goertzel block size from sbitx_cw_rx_samples_per_block). */
    float    *cw_rx_buf;
    int       cw_rx_buf_n;
    int       cw_rx_buf_block;

    /* FT8 RX 15-second slot accumulator @ 12 kHz. */
    float    *ft8_rx_buf;
    int       ft8_rx_buf_n;
    int       ft8_rx_buf_cap;

    /* RTTY RX block accumulator. */
    float    *rtty_rx_buf;
    int       rtty_rx_buf_n;
    int       rtty_rx_buf_block;
} hamlib_digi_state;

static hamlib_digi_state g_state;

static void digi_spool_log(const char *mode, const char *dir,
                           uint32_t freq_hz, const char *text)
{
    /* Mirror the format used elsewhere (radio_websocket digi_messages reads
     * /var/spool/hermes-digi/spool.log). */
    FILE *f = fopen("/var/spool/hermes-digi/spool.log", "a");
    if (!f) return;
    uint32_t khz = freq_hz / 1000;
    fprintf(f, "%s %s %u.%03u: %s%s",
            mode, dir, khz / 1000, khz % 1000, text,
            (text[0] && text[strlen(text)-1] == '\n') ? "" : "\n");
    fclose(f);
}

/* ─── TX paths ───────────────────────────────────────────────── */

static void do_cw_tx(hamlib_digi_state *s, uint32_t ring_rate, uint32_t freq_hz,
                     int wpm, int pitch)
{
    char text[DIGI_TX_MSG_MAX];
    if (!digi_tx_queue_pop(&s->radio_h->digi_tx, text, sizeof(text)))
        return;

    /* CW encodes at 96 kHz; bound the buffer at 8 s. */
    static float cw_audio[96000 * 8];
    int n96 = sbitx_cw_encode(text, cw_audio,
                              (int)(sizeof(cw_audio) / sizeof(cw_audio[0])),
                              wpm, pitch);
    if (n96 <= 0)
        return;

    if (s->cw_to_ring.taps_len == 0 && 96000 != ring_rate)
        if (!resamp_init(&s->cw_to_ring, 96000, ring_rate))
            return;

    size_t out_n;
    const float *ring_audio;
    if (96000 == ring_rate) {
        ring_audio = cw_audio;
        out_n = (size_t) n96;
    } else {
        out_n = resamp_apply(&s->cw_to_ring, cw_audio, (size_t) n96);
        ring_audio = s->cw_to_ring.out;
    }
    ring_push_f(&s->radio_h->tx_audio_ring, ring_audio, out_n);
    digi_spool_log("CW", "tx", freq_hz, text);
}

static void do_ft8_tx(hamlib_digi_state *s, uint32_t ring_rate, uint32_t freq_hz)
{
    char text[DIGI_TX_MSG_MAX];
    if (!digi_tx_queue_pop(&s->radio_h->digi_tx, text, sizeof(text)))
        return;

    /* 16 s at 12 kHz is plenty for one FT8 slot. */
    static float ft8_audio[192000];
    int n12 = sbitx_ft8_encode(text, ft8_audio,
                               (int)(sizeof(ft8_audio) / sizeof(ft8_audio[0])),
                               1500.0f);
    if (n12 <= 0)
        return;

    if (s->ft8_to_ring.taps_len == 0 && 12000 != ring_rate)
        if (!resamp_init(&s->ft8_to_ring, 12000, ring_rate))
            return;

    size_t out_n;
    const float *ring_audio;
    if (12000 == ring_rate) {
        ring_audio = ft8_audio;
        out_n = (size_t) n12;
    } else {
        out_n = resamp_apply(&s->ft8_to_ring, ft8_audio, (size_t) n12);
        ring_audio = s->ft8_to_ring.out;
    }
    ring_push_f(&s->radio_h->tx_audio_ring, ring_audio, out_n);
    digi_spool_log("FT8", "tx", freq_hz, text);
}

static void do_rtty_tx(hamlib_digi_state *s, uint32_t ring_rate, uint32_t freq_hz,
                       int baud, int mark, int shift)
{
    char text[DIGI_TX_MSG_MAX];
    if (!digi_tx_queue_pop(&s->radio_h->digi_tx, text, sizeof(text)))
        return;

    /* RTTY encodes at 96 kHz; bound at 8 s. */
    static float rtty_audio[96000 * 8];
    int n96 = sbitx_rtty_encode(text, rtty_audio,
                                (int)(sizeof(rtty_audio) / sizeof(rtty_audio[0])),
                                baud, mark, shift);
    if (n96 <= 0)
        return;

    if (s->rtty_to_ring.taps_len == 0 && 96000 != ring_rate)
        if (!resamp_init(&s->rtty_to_ring, 96000, ring_rate))
            return;

    size_t out_n;
    const float *ring_audio;
    if (96000 == ring_rate) {
        ring_audio = rtty_audio;
        out_n = (size_t) n96;
    } else {
        out_n = resamp_apply(&s->rtty_to_ring, rtty_audio, (size_t) n96);
        ring_audio = s->rtty_to_ring.out;
    }
    ring_push_f(&s->radio_h->tx_audio_ring, ring_audio, out_n);
    digi_spool_log("RTTY", "tx", freq_hz, text);
}

/* ─── RX paths ───────────────────────────────────────────────── */

/* Pull one period from rx_audio_ring at ring_rate, downsample to 12 kHz
 * floats into *out (caller-supplied), return count. */
static size_t pull_rx_12k(hamlib_digi_state *s, uint32_t ring_rate,
                          size_t period_frames, float *out12k, size_t out_cap)
{
    static int16_t pull[8192];
    size_t want = period_frames;
    if (want > sizeof(pull)/sizeof(pull[0])) want = sizeof(pull)/sizeof(pull[0]);

    size_t got = ring_pop_i16(&s->radio_h->rx_audio_ring, pull, want);
    if (got == 0)
        return 0;

    /* int16 -> float */
    static float scratch[8192];
    for (size_t i = 0; i < got; i++)
        scratch[i] = (float) pull[i] / 32768.0f;

    if (ring_rate == DIGI_DECODE_RATE) {
        size_t copy = got < out_cap ? got : out_cap;
        memcpy(out12k, scratch, copy * sizeof(float));
        return copy;
    }
    if (s->ring_to_12k.taps_len == 0)
        if (!resamp_init(&s->ring_to_12k, ring_rate, DIGI_DECODE_RATE))
            return 0;

    size_t n = resamp_apply(&s->ring_to_12k, scratch, got);
    if (n > out_cap) n = out_cap;
    memcpy(out12k, s->ring_to_12k.out, n * sizeof(float));
    return n;
}

static int g_rtty_freq_hz_cache;
static void rtty_rx_char_cb(char c)
{
    static char line[256];
    static int  pos = 0;
    if (c == '\r' || c == '\n') {
        if (pos > 0) {
            line[pos] = '\0';
            digi_spool_log("RTTY", "rx", (uint32_t) g_rtty_freq_hz_cache, line);
            pos = 0;
        }
        return;
    }
    if (pos < (int) sizeof(line) - 1)
        line[pos++] = c;
}

static void cw_rx_char_cb(char c)
{
    static char line[256];
    static int  pos = 0;
    if (c == ' ' || c == '\n') {
        if (pos > 0) {
            line[pos] = '\0';
            digi_spool_log("CW", "rx", 0, line);
            pos = 0;
        }
        return;
    }
    if (pos < (int) sizeof(line) - 1)
        line[pos++] = c;
}

static void do_cw_rx(hamlib_digi_state *s, uint32_t ring_rate, int wpm, int pitch)
{
    if (s->cw_rx_buf_block == 0)
        s->cw_rx_buf_block = sbitx_cw_rx_samples_per_block();

    if (!s->cw_rx_buf) {
        s->cw_rx_buf = calloc(s->cw_rx_buf_block * 2, sizeof(float));
        if (!s->cw_rx_buf) return;
    }

    /* Pull ~50 ms of audio. */
    size_t n = pull_rx_12k(s, ring_rate, ring_rate / 20,
                           s->cw_rx_buf + s->cw_rx_buf_n,
                           s->cw_rx_buf_block * 2 - s->cw_rx_buf_n);
    if (!n)
        return;
    s->cw_rx_buf_n += (int) n;

    while (s->cw_rx_buf_n >= s->cw_rx_buf_block) {
        char decoded[64] = {0};
        int got = sbitx_cw_rx_process(s->cw_rx_buf, s->cw_rx_buf_block,
                                      decoded, sizeof(decoded), wpm, pitch);
        if (got > 0) {
            for (int i = 0; i < got && decoded[i]; i++)
                cw_rx_char_cb(decoded[i]);
        }
        memmove(s->cw_rx_buf, s->cw_rx_buf + s->cw_rx_buf_block,
                (s->cw_rx_buf_n - s->cw_rx_buf_block) * sizeof(float));
        s->cw_rx_buf_n -= s->cw_rx_buf_block;
    }
}

static void do_ft8_rx(hamlib_digi_state *s, uint32_t ring_rate, uint32_t freq_hz)
{
    if (s->ft8_rx_buf_cap == 0) {
        s->ft8_rx_buf_cap = DIGI_DECODE_RATE * (FT8_SLOT_SECONDS + 1);
        s->ft8_rx_buf = calloc(s->ft8_rx_buf_cap, sizeof(float));
        if (!s->ft8_rx_buf) { s->ft8_rx_buf_cap = 0; return; }
    }

    /* Pull ~100 ms. */
    size_t n = pull_rx_12k(s, ring_rate, ring_rate / 10,
                           s->ft8_rx_buf + s->ft8_rx_buf_n,
                           s->ft8_rx_buf_cap - s->ft8_rx_buf_n);
    if (!n)
        return;
    s->ft8_rx_buf_n += (int) n;

    /* When we've accumulated >= 15 s of audio, run a decode pass and slide. */
    int slot_samples = DIGI_DECODE_RATE * FT8_SLOT_SECONDS;
    if (s->ft8_rx_buf_n >= slot_samples) {
        char decoded[1024] = {0};
        sbitx_ft8_decode(s->ft8_rx_buf, slot_samples, decoded, sizeof(decoded));
        if (decoded[0])
            digi_spool_log("FT8", "rx", freq_hz, decoded);
        /* Slide forward by half a slot so successive decodes overlap and
         * catch off-boundary transmissions. */
        int shift = slot_samples / 2;
        memmove(s->ft8_rx_buf, s->ft8_rx_buf + shift,
                (s->ft8_rx_buf_n - shift) * sizeof(float));
        s->ft8_rx_buf_n -= shift;
    }
}

static void do_rtty_rx(hamlib_digi_state *s, uint32_t ring_rate, uint32_t freq_hz,
                       int baud, int mark, int shift)
{
    if (s->rtty_rx_buf_block == 0)
        s->rtty_rx_buf_block = sbitx_rtty_rx_samples_per_block();
    if (!s->rtty_rx_buf) {
        s->rtty_rx_buf = calloc(s->rtty_rx_buf_block * 2, sizeof(float));
        if (!s->rtty_rx_buf) return;
    }

    size_t n = pull_rx_12k(s, ring_rate, ring_rate / 20,
                           s->rtty_rx_buf + s->rtty_rx_buf_n,
                           s->rtty_rx_buf_block * 2 - s->rtty_rx_buf_n);
    if (!n)
        return;
    s->rtty_rx_buf_n += (int) n;

    while (s->rtty_rx_buf_n >= s->rtty_rx_buf_block) {
        g_rtty_freq_hz_cache = (int) freq_hz;
        sbitx_rtty_rx_process(s->rtty_rx_buf, s->rtty_rx_buf_block,
                              baud, mark, shift, rtty_rx_char_cb);
        memmove(s->rtty_rx_buf, s->rtty_rx_buf + s->rtty_rx_buf_block,
                (s->rtty_rx_buf_n - s->rtty_rx_buf_block) * sizeof(float));
        s->rtty_rx_buf_n -= s->rtty_rx_buf_block;
    }
}

/* ─── thread loop ────────────────────────────────────────────── */

static void *hamlib_digi_thread(void *radio_h_v)
{
    radio *radio_h = (radio *) radio_h_v;
    hamlib_digi_state *s = &g_state;
    s->radio_h = radio_h;

    while (!shutdown_) {
        uint32_t profile = radio_h->profile_active_idx;
        uint16_t mode = radio_h->profiles[profile].mode;
        uint32_t freq_hz = radio_h->profiles[profile].freq;
        bool in_tx = (radio_h->txrx_state == IN_TX);
        uint32_t ring_rate = radio_h->audio_sample_rate ?
                             radio_h->audio_sample_rate : 48000;

        if (mode != MODE_FT8 && mode != MODE_CW && mode != MODE_RTTY) {
            usleep(50000);
            continue;
        }

        /* Lazy per-mode init (sbitx_*_init is idempotent — the libcw etc.
         * libraries handle re-init OK but we still gate to avoid spam). */
        if (mode == MODE_CW && !s->cw_inited) {
            sbitx_cw_init(radio_h->cw_wpm, radio_h->cw_pitch);
            s->cw_inited = true;
        }
        if (mode == MODE_FT8 && !s->ft8_inited) {
            sbitx_ft8_init();
            s->ft8_inited = true;
        }
        if (mode == MODE_RTTY && !s->rtty_inited) {
            sbitx_rtty_init(radio_h->rtty_baud, radio_h->rtty_mark, radio_h->rtty_shift);
            s->rtty_inited = true;
        }

        if (in_tx) {
            switch (mode) {
            case MODE_CW:
                do_cw_tx(s, ring_rate, freq_hz, radio_h->cw_wpm, radio_h->cw_pitch);
                break;
            case MODE_FT8:
                do_ft8_tx(s, ring_rate, freq_hz);
                break;
            case MODE_RTTY:
                do_rtty_tx(s, ring_rate, freq_hz,
                           radio_h->rtty_baud, radio_h->rtty_mark, radio_h->rtty_shift);
                break;
            }
            /* TX paths push a whole message worth of audio at once; idle
             * a beat so the playback thread can drain before we check the
             * queue again. */
            usleep(20000);
        } else {
            switch (mode) {
            case MODE_CW:
                do_cw_rx(s, ring_rate, radio_h->cw_wpm, radio_h->cw_pitch);
                break;
            case MODE_FT8:
                do_ft8_rx(s, ring_rate, freq_hz);
                break;
            case MODE_RTTY:
                do_rtty_rx(s, ring_rate, freq_hz,
                           radio_h->rtty_baud, radio_h->rtty_mark, radio_h->rtty_shift);
                break;
            }
            usleep(5000);
        }
    }

    /* Cleanup */
    resamp_free(&s->cw_to_ring);
    resamp_free(&s->rtty_to_ring);
    resamp_free(&s->ft8_to_ring);
    resamp_free(&s->ring_to_12k);
    free(s->cw_rx_buf);
    free(s->ft8_rx_buf);
    free(s->rtty_rx_buf);
    if (s->cw_inited) sbitx_cw_shutdown();
    if (s->ft8_inited) sbitx_ft8_shutdown();
    if (s->rtty_inited) sbitx_rtty_shutdown();
    memset(s, 0, sizeof(*s));

    return NULL;
}

bool hamlib_digi_start(radio *radio_h)
{
    if (!radio_h || g_state.started)
        return false;
    memset(&g_state, 0, sizeof(g_state));
    if (pthread_create(&g_state.tid, NULL, hamlib_digi_thread, radio_h) != 0) {
        fprintf(stderr, "hamlib_digi: cannot spawn thread\n");
        return false;
    }
    g_state.started = true;
    fprintf(stderr, "hamlib_digi: pump up\n");
    return true;
}

void hamlib_digi_stop(radio *radio_h)
{
    (void) radio_h;
    if (!g_state.started)
        return;
    pthread_join(g_state.tid, NULL);
    g_state.started = false;
}
