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
#include <time.h>
#include <unistd.h>

#define USE_FFTW
#define LIBCSDR_GPL
#include <fft_fftw.h>
#include <libcsdr.h>

#include "hamlib_digi.h"
#include "../dsp/stream_resampler.h"
#include "../dsp/sbitx_cw.h"
#include "../dsp/sbitx_ft8.h"
#include "../dsp/sbitx_rtty.h"
#include "../dsp/sbitx_radae.h"
#include "../dsp/sbitx_dstar.h"
#include "../dsp/dstar_voice.h"
#include <mbelib-neo/mbelib.h>
#include <specbleach_denoiser.h>
#include "../radio_backend.h"   /* radio_backend_set_txrx_state for auto-PTT */

/* Hard cap on an auto-keyed digital transmission. PTT is dropped the instant
 * the queue empties and the TX ring drains; this only fires if something stalls
 * (e.g. the codec), guaranteeing we never hold the rig in TX indefinitely. */
#define DIGI_TX_MAX_SECS 45

extern _Atomic bool shutdown_;

#define DIGI_DECODE_RATE 12000     /* CW/FT8/RTTY decoders all expect 12 kHz */
#define FT8_SLOT_SECONDS 15

/* RADAE modem audio rate (matches the 8 kHz IQ baseband the encoder/decoder
 * use internally; see dsp/sbitx_dsp.c). After freq-shift to the SSB
 * passband centre this becomes 8 kHz real audio that we resample up to the
 * ring rate. */
#define RADAE_MODEM_RATE   8000
#define RADAE_SPEECH_RATE  16000
#define RADAE_CARRIER_HZ   1500.0f   /* centre in SSB audio passband */

/* One direction of rate conversion. Wraps stream_resampler, which carries
 * the input csdr's rational_resampler_ff() leaves unconsumed. Calling that
 * per block with only the new block, as this did, dropped the look-ahead
 * tail every block: 48 -> 12 kHz FT8/CW/RTTY lost samples and timing on
 * every block, and RADE / D-STAR here the same. taps_len stays 0 until a
 * rate change is set up (callers test it); out holds the last results. */
typedef struct {
    int      taps_len;
    stream_resampler rs;
    float   *out;
    size_t   out_cap;
} resamp_state;

static bool resamp_init(resamp_state *r, uint32_t from_rate, uint32_t to_rate)
{
    memset(r, 0, sizeof(*r));
    if (from_rate == to_rate)
        return true;
    /* 0.02 of the higher rate: at 48 kHz a 960 Hz transition, sharp
     * enough for 8 kHz RADE/D-STAR (0.05 folded 5-7 kHz into the band). */
    if (!stream_resampler_init(&r->rs, (int) to_rate, (int) from_rate, 0.02f))
        return false;
    r->taps_len = r->rs.taps_len;
    return true;
}

static void resamp_free(resamp_state *r)
{
    stream_resampler_free(&r->rs);
    free(r->out);
    memset(r, 0, sizeof(*r));
}

static size_t resamp_apply(resamp_state *r, const float *in, size_t n_in)
{
    const float *src = in;
    size_t n = n_in;
    if (r->taps_len) {
        n = stream_resampler_run(&r->rs, in, n_in);
        src = r->rs.out;
    }
    if (n > r->out_cap) {
        float *p = realloc(r->out, n * sizeof(float));
        if (!p) return 0;
        r->out = p;
        r->out_cap = n;
    }
    if (n)
        memcpy(r->out, src, n * sizeof(float));
    return n;
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

static void ring_push_f_gain(audio_ring_buffer *ring, const float *in, size_t n,
                             float gain)
{
    if (!ring->samples || !ring->capacity || !in || !n)
        return;
    pthread_mutex_lock(&ring->mutex);
    for (size_t i = 0; i < n; i++) {
        if (ring->count == ring->capacity) {
            ring->read_pos = (ring->read_pos + 1) % ring->capacity;
            ring->count--;
        }
        float v = in[i] * gain * 32767.0f;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        ring->samples[ring->write_pos] = (int16_t) v;
        ring->write_pos = (ring->write_pos + 1) % ring->capacity;
        ring->count++;
    }
    pthread_cond_signal(&ring->cond);
    pthread_mutex_unlock(&ring->mutex);
}

static void ring_push_f(audio_ring_buffer *ring, const float *in, size_t n)
{
    ring_push_f_gain(ring, in, n, 1.0f);
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

    /* RADAE: resamplers between the ring rate and the modem/speech rates. */
    resamp_state ring_to_radae_speech;   /* ring rate -> 16k speech (TX in) */
    resamp_state radae_modem_to_ring;    /* 8k real audio -> ring rate (TX out) */
    resamp_state ring_to_radae_modem;    /* ring rate -> 8k real audio (RX in) */
    resamp_state radae_speech_to_ring;   /* 16k speech -> ring rate (RX out) */
    radae_context radae_ctx;
    bool radae_inited;
    bool radae_tx_running;
    bool radae_rx_running;

    /* D-STAR DV: resamplers + modem + AMBE state.
     * TX: tx_audio_ring (mic) -> 8k -> AMBE -> GMSK 24k -> tx_dstar_ring.
     * RX: rx_audio_ring -> 24k discriminator -> modem -> AMBE -> rx_dstar_ring. */
    resamp_state ring_to_dstar_8k;    /* ring rate -> 8k (TX mic in) */
    resamp_state dstar_24k_to_ring;   /* 24k GMSK -> ring rate (TX out) */
    resamp_state ring_to_dstar_24k;   /* ring rate -> 24k (RX in) */
    resamp_state dstar_8k_to_ring;    /* 8k PCM -> ring rate (RX out) */
    sbitx_dstar_rx *dstar_rx;
    sbitx_dstar_tx *dstar_tx;
    mbe_parms dstar_rx_cur, dstar_rx_prev, dstar_rx_enh, dstar_rx_prevsyn;
    mbe_parms dstar_tx_cur, dstar_tx_prev, dstar_tx_enh;
    float dstar_mic8k[160];
    int   dstar_mic8k_n;
    bool  dstar_tx_keyed;
    bool  dstar_inited;
    dstar_voice_tx dstar_vtx;   /* over being sent: header, slot, encryption */
    mbe_ambe2400_encoder *dstar_tx_enc;   /* AMBE analysis state, reset per over */
    dstar_voice_rx dstar_vrx;   /* over being received */

    /* Phase accumulators for the freq-shift (mixer) at 1500 Hz, 8 kHz fs.
     * Carried across calls so consecutive blocks stay phase-continuous. */
    double radae_tx_phase;
    double radae_rx_phase;
    /* Auto-PTT for typed digital TX (CW/RTTY/FT8): when text is queued we key
     * PTT, transmit, then drop PTT once the queue empties and the TX ring has
     * drained. digi_auto_tx distinguishes our keying from a manual operator PTT
     * (which we never auto-unkey). */
    bool   digi_auto_tx;
    time_t digi_tx_start;
    int    digi_drain_ticks;
    /* The message being sent (FT8/CW/RTTY), at the ring rate, fed into
     * tx_audio_ring as it drains: the ring holds 1/3 s at 48 kHz, and
     * pushing a whole 12.6 s FT8 message at once overwrote all but its
     * last third of a second. */
    float *tx_pend;
    size_t tx_pend_len, tx_pend_pos, tx_pend_cap;
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

/* Convert one whole message to the ring rate into tx_pend. The resampler
 * starts fresh and is flushed with zeros, so a message neither begins with
 * the previous one's tail nor loses its own end in the filter. */
static bool tx_pend_set(hamlib_digi_state *s, resamp_state *rs, const float *audio,
                        size_t n, uint32_t from_rate, uint32_t ring_rate)
{
    const float *src = audio;
    size_t out_n = n;
    static float zeros[4096];

    if (from_rate != ring_rate) {
        if (rs->taps_len == 0 && !resamp_init(rs, from_rate, ring_rate))
            return false;
        stream_resampler_reset(&rs->rs);
        size_t need = n * ring_rate / from_rate + 4096;
        if (need > s->tx_pend_cap) {
            float *p = realloc(s->tx_pend, need * sizeof(float));
            if (!p) return false;
            s->tx_pend = p;
            s->tx_pend_cap = need;
        }
        out_n = resamp_apply(rs, audio, n);
        memcpy(s->tx_pend, rs->out, out_n * sizeof(float));
        size_t flush = (size_t) (rs->rs.taps_len / rs->rs.interp + 2);
        while (flush > 0) {
            size_t k = flush < sizeof(zeros) / sizeof(zeros[0]) ? flush : sizeof(zeros) / sizeof(zeros[0]);
            size_t m = resamp_apply(rs, zeros, k);
            if (out_n + m > s->tx_pend_cap)
                m = s->tx_pend_cap - out_n;
            memcpy(s->tx_pend + out_n, rs->out, m * sizeof(float));
            out_n += m;
            flush -= k;
        }
    } else {
        if (n > s->tx_pend_cap) {
            float *p = realloc(s->tx_pend, n * sizeof(float));
            if (!p) return false;
            s->tx_pend = p;
            s->tx_pend_cap = n;
        }
        memcpy(s->tx_pend, src, n * sizeof(float));
    }
    s->tx_pend_len = out_n;
    s->tx_pend_pos = 0;
    return true;
}

static bool tx_pend_busy(const hamlib_digi_state *s)
{
    return s->tx_pend_pos < s->tx_pend_len;
}

/* Top tx_audio_ring up from tx_pend, never past its capacity. */
static void tx_pend_feed(hamlib_digi_state *s)
{
    if (!tx_pend_busy(s))
        return;
    audio_ring_buffer *ring = &s->radio_h->tx_audio_ring;
    pthread_mutex_lock(&ring->mutex);
    size_t space = ring->capacity > ring->count ? ring->capacity - ring->count : 0;
    pthread_mutex_unlock(&ring->mutex);
    size_t n = s->tx_pend_len - s->tx_pend_pos;
    if (n > space)
        n = space;
    if (n) {
        ring_push_f_gain(ring, s->tx_pend + s->tx_pend_pos, n, s->radio_h->digi_tx_gain);
        s->tx_pend_pos += n;
    }
}

static void do_cw_tx(hamlib_digi_state *s, uint32_t ring_rate, uint32_t freq_hz,
                     int wpm, int pitch)
{
    char text[DIGI_TX_MSG_MAX];
    if (tx_pend_busy(s) || !digi_tx_queue_pop(&s->radio_h->digi_tx, text, sizeof(text)))
        return;

    /* CW encodes at 96 kHz; bound the buffer at 8 s. */
    static float cw_audio[96000 * 8];
    int n96 = sbitx_cw_encode(text, cw_audio,
                              (int)(sizeof(cw_audio) / sizeof(cw_audio[0])),
                              wpm, pitch);
    if (n96 <= 0 || !tx_pend_set(s, &s->cw_to_ring, cw_audio, (size_t) n96, 96000, ring_rate))
        return;
    digi_spool_log("CW", "tx", freq_hz, text);
}

static void do_ft8_tx(hamlib_digi_state *s, uint32_t ring_rate, uint32_t freq_hz)
{
    char text[DIGI_TX_MSG_MAX];
    if (tx_pend_busy(s) || !digi_tx_queue_pop(&s->radio_h->digi_tx, text, sizeof(text)))
        return;

    /* 16 s at 12 kHz is plenty for one FT8 slot. */
    static float ft8_audio[192000];
    int n12 = sbitx_ft8_encode(text, ft8_audio,
                               (int)(sizeof(ft8_audio) / sizeof(ft8_audio[0])),
                               1500.0f);
    if (n12 <= 0) {
        fprintf(stderr, "FT8 tx: cannot encode \"%s\"\n", text);
        return;
    }
    if (!tx_pend_set(s, &s->ft8_to_ring, ft8_audio, (size_t) n12, 12000, ring_rate))
        return;
    digi_spool_log("FT8", "tx", freq_hz, text);
}

static void do_rtty_tx(hamlib_digi_state *s, uint32_t ring_rate, uint32_t freq_hz,
                       int baud, int mark, int shift)
{
    char text[DIGI_TX_MSG_MAX];
    if (tx_pend_busy(s) || !digi_tx_queue_pop(&s->radio_h->digi_tx, text, sizeof(text)))
        return;

    /* RTTY encodes at 96 kHz; bound at 8 s. */
    static float rtty_audio[96000 * 8];
    int n96 = sbitx_rtty_encode(text, rtty_audio,
                                (int)(sizeof(rtty_audio) / sizeof(rtty_audio[0])),
                                baud, mark, shift);
    if (n96 <= 0 || !tx_pend_set(s, &s->rtty_to_ring, rtty_audio, (size_t) n96, 96000, ring_rate))
        return;
    digi_spool_log("RTTY", "tx", freq_hz, text);
}

/* ─── RX paths ───────────────────────────────────────────────── */

/* Pull one period from rx_digi_ring at ring_rate, downsample to 12 kHz
 * floats into *out (caller-supplied), return count. */
static size_t pull_rx_12k(hamlib_digi_state *s, uint32_t ring_rate,
                          size_t period_frames, float *out12k, size_t out_cap)
{
    static int16_t pull[8192];
    size_t want = period_frames;
    if (want > sizeof(pull)/sizeof(pull[0])) want = sizeof(pull)/sizeof(pull[0]);

    size_t got = ring_pop_i16(&s->radio_h->rx_digi_ring, pull, want);
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
            digi_spool_log("CW", "rx", (uint32_t) g_rtty_freq_hz_cache, line);
            pos = 0;
        }
        return;
    }
    if (pos < (int) sizeof(line) - 1)
        line[pos++] = c;
}

static void do_cw_rx(hamlib_digi_state *s, uint32_t ring_rate, uint32_t freq_hz,
                     int wpm, int pitch)
{
    g_rtty_freq_hz_cache = (int) freq_hz;       /* for the CW spool line too */
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
        /* Windows overlap by 13 s, so one transmission can decode in two
         * of them: log the same text once per 15 s. */
        static char     last_decoded[1024];
        static uint64_t last_decoded_at = 0;          /* in 2 s steps */
        static uint64_t window_no = 0;
        window_no++;
        if (decoded[0] && (strcmp(decoded, last_decoded) != 0 ||
                           window_no - last_decoded_at > FT8_SLOT_SECONDS / 2)) {
            digi_spool_log("FT8", "rx", freq_hz, decoded);
            snprintf(last_decoded, sizeof(last_decoded), "%s", decoded);
            last_decoded_at = window_no;
        }
        /* Slide by 2 s. Transmissions here are not UTC-slot aligned, and a
         * 12.64 s burst fits a 15 s window only if it starts within its
         * first 2.36 s; the old half-slot (7.5 s) step missed most of
         * them. Same as the sBitx path. */
        int shift = DIGI_DECODE_RATE * 2;
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

/* ─── D-STAR DV: GMSK modem + AMBE over the rig audio path ───────
 *
 * The rig (an FM/DV-capable radio with a data port, e.g. an IC-7100 in
 * DV mode) carries the 4800 baud GMSK baseband on its audio interface.
 * TX: browser/mic audio from tx_audio_ring -> 8 kHz -> AMBE frames ->
 * GMSK at 24 kHz -> tx_dstar_ring (drained by playback in place of
 * tx_audio_ring). RX: rx_audio_ring -> 24 kHz -> modem -> AMBE decode
 * -> 8 kHz PCM -> rx_dstar_ring for the websocket broadcast.
 */

static void dstar_hamlib_data_cb(void *user, const uint8_t *frame);

static void dstar_hamlib_header_cb(void *user, const uint8_t *header)
{
    hamlib_digi_state *s = (hamlib_digi_state *) user;
    dstar_voice_rx_header(&s->dstar_vrx, header);
}

/* EOT or loss of lock: the over is gone. */
static void dstar_hamlib_end_cb(void *user)
{
    hamlib_digi_state *s = (hamlib_digi_state *) user;
    dstar_voice_rx_reset(&s->dstar_vrx);
    s->radio_h->dstar_rx_crypto = DSTAR_VC_CLEAR;
}

/* Start the AMBE encoder afresh, as mbelib-neo prescribes for restarting a
 * stream: reset the analysis context and the parameter prediction state. */
static void
dstar_hamlib_reset_encoder(hamlib_digi_state *s)
{
    mbe_ambe2400EncoderReset(s->dstar_tx_enc);
    mbe_initMbeParms(&s->dstar_tx_cur, &s->dstar_tx_prev, &s->dstar_tx_enh);
    memset(&s->dstar_tx_prev, 0, sizeof(s->dstar_tx_prev));
    s->dstar_tx_prev.mutingThreshold = MBE_MUTING_THRESHOLD_AMBE;
}

static void
dstar_hamlib_init(hamlib_digi_state *s)
{
    if (s->dstar_inited)
        return;
    s->dstar_rx = sbitx_dstar_rx_new();
    sbitx_dstar_rx_set_cbs(s->dstar_rx, dstar_hamlib_header_cb, dstar_hamlib_data_cb,
                           dstar_hamlib_end_cb, dstar_hamlib_end_cb, s);
    dstar_voice_rx_reset(&s->dstar_vrx);
    sbitx_dstar_rx_set_polarity(s->dstar_rx, s->radio_h->dstar_rx_polarity);
    s->dstar_tx = sbitx_dstar_tx_new();
    mbe_initMbeParms(&s->dstar_rx_cur, &s->dstar_rx_prev, &s->dstar_rx_enh);
    memset(&s->dstar_rx_prevsyn, 0, sizeof(s->dstar_rx_prevsyn));
    s->dstar_rx_prevsyn.L = 15;
    s->dstar_rx_prevsyn.mutingThreshold = MBE_MUTING_THRESHOLD_AMBE;
    s->dstar_tx_enc = mbe_ambe2400EncoderAlloc();
    if (s->dstar_tx_enc == NULL)
        fprintf(stderr, "DSTAR: cannot allocate the AMBE encoder; TX will be silent\n");
    dstar_hamlib_reset_encoder(s);
    s->dstar_inited = true;
}

static uint32_t g_dstar_ring_rate = 48000;

static void dstar_hamlib_data_cb(void *user, const uint8_t *frame)
{
    hamlib_digi_state *s = (hamlib_digi_state *) user;
    char ambe_d[49];
    short pcm[160];
    float pcmf[160];
    mbe_process_result res;

    /* FEC decode, then decrypt if the over is encrypted; an encrypted over
     * this station cannot decrypt is muted rather than played as noise. */
    bool play = dstar_voice_rx_frame(&s->dstar_vrx, frame, sbitx_dstar_rx_frame_index(s->dstar_rx),
                                     ambe_d, &res);
    s->radio_h->dstar_rx_crypto = (uint16_t) s->dstar_vrx.status;
    if (play)
        mbe_processAmbe2400Data(pcm, &res, ambe_d,
                                &s->dstar_rx_cur, &s->dstar_rx_prev, &s->dstar_rx_enh);

    /* Silence frames (b0 == 127) would render as loud comfort noise; emit
     * true silence instead. */
    if (!play ||
        (ambe_d[0] && ambe_d[1] && ambe_d[2] && ambe_d[3] && ambe_d[4] && ambe_d[5] && ambe_d[48])) {
        for (int i = 0; i < 160; i++)
            pcmf[i] = 0.0f;
    } else {
        for (int i = 0; i < 160; i++)
            pcmf[i] = ((float)pcm[i] / 32768.0f);
    }

    /* Push the decoded 8 kHz PCM into rx_dstar_ring at the ring rate. */
    if (s->dstar_8k_to_ring.taps_len == 0 && 8000 != g_dstar_ring_rate)
        resamp_init(&s->dstar_8k_to_ring, 8000, g_dstar_ring_rate);

    const float *ring_audio;
    size_t n_ring;
    if (8000 == g_dstar_ring_rate) {
        ring_audio = pcmf;
        n_ring = 160;
    } else if (s->dstar_8k_to_ring.taps_len == 0) {
        return;
    } else {
        n_ring = resamp_apply(&s->dstar_8k_to_ring, pcmf, 160);
        ring_audio = s->dstar_8k_to_ring.out;
    }
    ring_push_f(&s->radio_h->rx_dstar_ring, ring_audio, n_ring);
}

static void do_dstar_tx(hamlib_digi_state *s, uint32_t ring_rate)
{
    static int16_t mic_i16[4096];

    /* Pull mic audio from tx_audio_ring and AMBE-encode it. */
    size_t want = ring_rate / 50;   /* 20 ms */
    if (want > sizeof(mic_i16)/sizeof(mic_i16[0])) want = sizeof(mic_i16)/sizeof(mic_i16[0]);
    size_t got = ring_pop_i16(&s->radio_h->tx_audio_ring, mic_i16, want);
    if (got) {
        static float mic_f[4096];
        for (size_t i = 0; i < got; i++)
            mic_f[i] = (float) mic_i16[i] / 32768.0f;

        const float *mic_8k;
        size_t n_8k;
        if (s->ring_to_dstar_8k.taps_len == 0 && ring_rate != 8000)
            if (!resamp_init(&s->ring_to_dstar_8k, ring_rate, 8000))
                return;
        if (ring_rate == 8000) {
            mic_8k = mic_f;
            n_8k = got;
        } else {
            n_8k = resamp_apply(&s->ring_to_dstar_8k, mic_f, got);
            mic_8k = s->ring_to_dstar_8k.out;
        }

        for (size_t i = 0; i < n_8k; i++) {
            s->dstar_mic8k[s->dstar_mic8k_n++] = mic_8k[i];
            if (s->dstar_mic8k_n == 160) {
                char ambe_d[49];
                uint8_t frame[SBITX_DSTAR_FRAME_BYTES];

                if (!s->dstar_tx_keyed) {
                    /* A new over is a new stream. */
                    dstar_hamlib_reset_encoder(s);
                    dstar_voice_tx_begin(&s->dstar_vtx, s->radio_h->dstar_mycall,
                                         s->radio_h->dstar_urcall,
                                         s->radio_h->dstar_encrypt != 0);
                    sbitx_dstar_tx_header(s->dstar_tx, s->dstar_vtx.header);
                    s->dstar_tx_keyed = true;
                }

                /* In-speech noise reduction front-end (libspecbleach, 8 kHz).
                 * Runtime tunable via dstar_denoise. */
                {
                    static SpectralBleachHandle nr;
                    static uint32_t nr_latency;
                    static uint32_t nr_total;
                    const float *src = s->dstar_mic8k;
                    float denoised[160];
                    if (s->radio_h->dstar_denoise && nr == NULL) {
                        nr = specbleach_initialize(8000, 20.0f);
                        if (nr != NULL) {
                            SpectralBleachDenoiserParameters p;
                            memset(&p, 0, sizeof(p));
                            p.reduction_amount = 12.0f;
                            p.smoothing_factor = 30.0f;
                            p.whitening_factor = 15.0f;
                            p.adaptive_noise = 1;
                            p.noise_estimation_method = 2;   /* Martin Min Statistics */
                            p.masking_depth = 0.5f;
                            p.suppression_strength = 0.6f;
                            specbleach_load_parameters(nr, p);
                            nr_latency = specbleach_get_latency(nr);
                        }
                    }
                    if (s->radio_h->dstar_denoise && nr != NULL) {
                        specbleach_process(nr, 160, s->dstar_mic8k, denoised);
                        nr_total += 160;
                        if (nr_total > nr_latency)
                            src = denoised;
                    }
                    if (mbe_encodeAmbe2400Parms(s->dstar_tx_enc, src, ambe_d,
                                                &s->dstar_tx_cur, &s->dstar_tx_prev) < 0) {
                        s->dstar_mic8k_n = 0;   /* no encoder: send nothing */
                        continue;
                    }
                }
                /* Shared framing: 9 AMBE bytes then sync/slow data, with
                 * the header repeated in slow data and, when enabled, the
                 * voice bits encrypted. */
                dstar_voice_tx_frame(&s->dstar_vtx, ambe_d, frame);
                sbitx_dstar_tx_frame(s->dstar_tx, frame);
                mbe_moveMbeParms(&s->dstar_tx_cur, &s->dstar_tx_prev);
                s->dstar_mic8k_n = 0;
            }
        }
    }

    /* Generate GMSK at 24 kHz and push to tx_dstar_ring at ring_rate. */
    static float gmsk24[2048];
    int n24 = sbitx_dstar_tx_generate(s->dstar_tx, gmsk24, 24000 / 50);
    if (n24 > 0) {
        const float *ring_audio;
        size_t n_ring;
        if (s->dstar_24k_to_ring.taps_len == 0 && 24000 != ring_rate)
            if (!resamp_init(&s->dstar_24k_to_ring, 24000, ring_rate))
                return;
        if (24000 == ring_rate) {
            ring_audio = gmsk24;
            n_ring = (size_t) n24;
        } else {
            n_ring = resamp_apply(&s->dstar_24k_to_ring, gmsk24, (size_t) n24);
            ring_audio = s->dstar_24k_to_ring.out;
        }
        ring_push_f_gain(&s->radio_h->tx_dstar_ring, ring_audio, n_ring,
                         s->radio_h->dstar_tx_gain);
    }
}

static void do_dstar_rx(hamlib_digi_state *s, uint32_t ring_rate)
{
    g_dstar_ring_rate = ring_rate;

    static int16_t pull_i16[4096];
    size_t want = ring_rate / 50;   /* 20 ms */
    if (want > sizeof(pull_i16)/sizeof(pull_i16[0])) want = sizeof(pull_i16)/sizeof(pull_i16[0]);
    size_t got = ring_pop_i16(&s->radio_h->rx_digi_ring, pull_i16, want);
    if (!got)
        return;

    static float real_f[4096];
    for (size_t i = 0; i < got; i++)
        real_f[i] = (float) pull_i16[i] / 32768.0f;

    const float *audio_24k;
    size_t n_24k;
    if (s->ring_to_dstar_24k.taps_len == 0 && ring_rate != 24000)
        if (!resamp_init(&s->ring_to_dstar_24k, ring_rate, 24000))
            return;
    if (ring_rate == 24000) {
        audio_24k = real_f;
        n_24k = got;
    } else {
        n_24k = resamp_apply(&s->ring_to_dstar_24k, real_f, got);
        audio_24k = s->ring_to_dstar_24k.out;
    }
    if (!n_24k)
        return;

    sbitx_dstar_rx_process(s->dstar_rx, audio_24k, (int) n_24k);
}


/* ─── RADAE: complex IQ ↔ real audio at the SSB passband centre ──
 *
 * The rig itself does the SSB modulation/demodulation. All we need is to
 * place the RADAE modem signal inside the audio passband (USB sideband)
 * as a real-valued audio stream, and inverse on RX. That's a single
 * complex-to-real frequency shift; no Hilbert / SSB modulator required.
 *
 * TX shift (USB): real[n] = I[n]*cos(2π·fc·n/fs) − Q[n]*sin(2π·fc·n/fs)
 * RX shift (USB demod): I'[n] + jQ'[n] = real[n] * exp(−j·2π·fc·n/fs)
 */

static void mix_iq_to_real(const float *iq_interleaved, int n_complex,
                           float *real_out, double *phase_io,
                           uint32_t fs)
{
    double dphi = 2.0 * M_PI * RADAE_CARRIER_HZ / (double) fs;
    double phi = *phase_io;
    for (int n = 0; n < n_complex; n++) {
        float I = iq_interleaved[2*n];
        float Q = iq_interleaved[2*n + 1];
        real_out[n] = (float)(I * cos(phi) - Q * sin(phi));
        phi += dphi;
        if (phi > 2.0 * M_PI) phi -= 2.0 * M_PI;
    }
    *phase_io = phi;
}

static void demod_real_to_iq(const float *real_in, int n,
                             float *iq_interleaved, double *phase_io,
                             uint32_t fs)
{
    double dphi = 2.0 * M_PI * RADAE_CARRIER_HZ / (double) fs;
    double phi = *phase_io;
    for (int i = 0; i < n; i++) {
        /* multiply by exp(-j*phi) = cos(phi) - j*sin(phi) */
        float c = (float) cos(phi);
        float s = (float) sin(phi);
        iq_interleaved[2*i]     = real_in[i] * c;
        iq_interleaved[2*i + 1] = real_in[i] * (-s);
        phi += dphi;
        if (phi > 2.0 * M_PI) phi -= 2.0 * M_PI;
    }
    *phase_io = phi;
}

static void do_radae_tx(hamlib_digi_state *s, uint32_t ring_rate)
{
    /* Pull a chunk of speech (browser mic) from tx_audio_ring. ~40 ms. */
    static int16_t pull_i16[4096];
    size_t want = ring_rate / 25;  /* 40 ms */
    if (want > sizeof(pull_i16)/sizeof(pull_i16[0])) want = sizeof(pull_i16)/sizeof(pull_i16[0]);
    size_t got_speech = ring_pop_i16(&s->radio_h->tx_audio_ring, pull_i16, want);
    if (!got_speech)
        return;

    /* int16 -> float */
    static float speech_f[4096];
    for (size_t i = 0; i < got_speech; i++)
        speech_f[i] = (float) pull_i16[i] / 32768.0f;

    /* Resample ring_rate -> 16 kHz. */
    if (s->ring_to_radae_speech.taps_len == 0 && ring_rate != RADAE_SPEECH_RATE)
        if (!resamp_init(&s->ring_to_radae_speech, ring_rate, RADAE_SPEECH_RATE))
            return;
    const float *speech_16k;
    size_t n_16k;
    if (ring_rate == RADAE_SPEECH_RATE) {
        speech_16k = speech_f;
        n_16k = got_speech;
    } else {
        n_16k = resamp_apply(&s->ring_to_radae_speech, speech_f, got_speech);
        speech_16k = s->ring_to_radae_speech.out;
    }
    if (!n_16k)
        return;

    radae_tx_write_speech(&s->radae_ctx, speech_16k, (int) n_16k);

    /* Read modem IQ (interleaved float at 8 kHz). RADAE emits in
     * fixed-size frames; we may get nothing on any given call. */
    static float iq_buf[4096];
    int n_complex = radae_tx_read_modem_iq(&s->radae_ctx, iq_buf,
                                           (int)(sizeof(iq_buf)/sizeof(iq_buf[0])));
    if (n_complex <= 0)
        return;

    /* IQ at 8 kHz -> real audio at 8 kHz, shifted to RADAE_CARRIER_HZ. */
    static float real_8k[4096];
    if (n_complex > (int)(sizeof(real_8k)/sizeof(real_8k[0])))
        n_complex = (int)(sizeof(real_8k)/sizeof(real_8k[0]));
    mix_iq_to_real(iq_buf, n_complex, real_8k, &s->radae_tx_phase, RADAE_MODEM_RATE);

    /* Resample 8 kHz -> ring rate and push to tx_radae_ring. */
    if (s->radae_modem_to_ring.taps_len == 0 && ring_rate != RADAE_MODEM_RATE)
        if (!resamp_init(&s->radae_modem_to_ring, RADAE_MODEM_RATE, ring_rate))
            return;
    const float *ring_audio;
    size_t n_ring;
    if (ring_rate == RADAE_MODEM_RATE) {
        ring_audio = real_8k;
        n_ring = (size_t) n_complex;
    } else {
        n_ring = resamp_apply(&s->radae_modem_to_ring, real_8k, (size_t) n_complex);
        ring_audio = s->radae_modem_to_ring.out;
    }
    ring_push_f_gain(&s->radio_h->tx_radae_ring, ring_audio, n_ring,
                     s->radio_h->digi_tx_gain);
}

static void do_radae_rx(hamlib_digi_state *s, uint32_t ring_rate)
{
    /* Pull a chunk of rig audio (post-SSB-demod) from rx_audio_ring. */
    static int16_t pull_i16[4096];
    size_t want = ring_rate / 25;  /* 40 ms */
    if (want > sizeof(pull_i16)/sizeof(pull_i16[0])) want = sizeof(pull_i16)/sizeof(pull_i16[0]);
    size_t got = ring_pop_i16(&s->radio_h->rx_digi_ring, pull_i16, want);
    if (!got)
        return;

    static float real_f[4096];
    for (size_t i = 0; i < got; i++)
        real_f[i] = (float) pull_i16[i] / 32768.0f;

    /* Resample ring_rate -> 8 kHz so the freq-shift / RADAE rate matches. */
    if (s->ring_to_radae_modem.taps_len == 0 && ring_rate != RADAE_MODEM_RATE)
        if (!resamp_init(&s->ring_to_radae_modem, ring_rate, RADAE_MODEM_RATE))
            return;
    const float *real_8k;
    size_t n_8k;
    if (ring_rate == RADAE_MODEM_RATE) {
        real_8k = real_f;
        n_8k = got;
    } else {
        n_8k = resamp_apply(&s->ring_to_radae_modem, real_f, got);
        real_8k = s->ring_to_radae_modem.out;
    }
    if (!n_8k)
        return;

    /* Real -> complex baseband at 8 kHz via freq-shift down from 1500 Hz. */
    static float iq_buf[8192];
    if (n_8k * 2 > sizeof(iq_buf)/sizeof(iq_buf[0]))
        n_8k = (sizeof(iq_buf)/sizeof(iq_buf[0])) / 2;
    demod_real_to_iq(real_8k, (int) n_8k, iq_buf, &s->radae_rx_phase, RADAE_MODEM_RATE);

    radae_rx_write_modem_iq(&s->radae_ctx, iq_buf, (int) n_8k);

    /* Read decoded speech (16 kHz float). */
    static float speech_16k[8192];
    int n_speech = radae_rx_read_speech(&s->radae_ctx, speech_16k,
                                        (int)(sizeof(speech_16k)/sizeof(speech_16k[0])));
    if (n_speech <= 0)
        return;

    /* Resample 16 kHz -> ring rate and push to rx_radae_ring. */
    if (s->radae_speech_to_ring.taps_len == 0 && ring_rate != RADAE_SPEECH_RATE)
        if (!resamp_init(&s->radae_speech_to_ring, RADAE_SPEECH_RATE, ring_rate))
            return;
    const float *ring_audio;
    size_t n_ring;
    if (ring_rate == RADAE_SPEECH_RATE) {
        ring_audio = speech_16k;
        n_ring = (size_t) n_speech;
    } else {
        n_ring = resamp_apply(&s->radae_speech_to_ring, speech_16k, (size_t) n_speech);
        ring_audio = s->radae_speech_to_ring.out;
    }
    ring_push_f(&s->radio_h->rx_radae_ring, ring_audio, n_ring);
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

        bool digital_voice = radio_h->profiles[profile].digital_voice;

        /* RADAE digital-voice path. Runs in parallel with whatever rig
         * mode is selected — typically the operator parks the rig on a
         * USB voice frequency and toggles digital_voice on. */
        if (digital_voice) {
            if (!s->radae_inited) {
                if (radae_init(&s->radae_ctx, radio_h)) {
                    s->radae_inited = true;
                    radae_rx_start(&s->radae_ctx);
                    s->radae_rx_running = true;
                    fprintf(stderr, "hamlib_digi: RADAE up\n");
                } else {
                    fprintf(stderr, "hamlib_digi: radae_init failed; "
                                    "digital_voice disabled\n");
                    /* avoid retry spam */
                    usleep(500000);
                    continue;
                }
            }
            if (in_tx && !s->radae_tx_running) {
                radae_tx_start(&s->radae_ctx);
                s->radae_tx_running = true;
            }
            if (!in_tx && s->radae_tx_running) {
                radae_tx_emit_eoo(&s->radae_ctx);
                radae_tx_stop(&s->radae_ctx);
                s->radae_tx_running = false;
            }

            if (in_tx)
                do_radae_tx(s, ring_rate);
            else
                do_radae_rx(s, ring_rate);

            usleep(5000);
            continue;
        }

        if (mode != MODE_FT8 && mode != MODE_CW && mode != MODE_RTTY && mode != MODE_DSTAR) {
            /* Left the digital modes while still auto-keyed (e.g. profile
             * switched mid-TX): drop PTT so we never stick in transmit. */
            if (s->digi_auto_tx) {
                radio_backend_set_txrx_state(radio_h, IN_RX);
                s->digi_auto_tx = false;
                s->digi_drain_ticks = 0;
            }
            s->tx_pend_len = s->tx_pend_pos = 0;
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
        if (mode == MODE_DSTAR)
            dstar_hamlib_init(s);

        /* Auto-PTT: text queued while receiving -> key PTT, then let the next
         * iteration transmit (the brief sleep lets PTT engage before audio). A
         * manual operator PTT also transmits the queue and is left alone
         * (digi_auto_tx stays false, so it is never auto-unkeyed). */
        if (!in_tx && !s->digi_auto_tx &&
            digi_tx_queue_pending(&radio_h->digi_tx) &&
            !radio_h->swr_protection_enabled) {
            radio_backend_set_txrx_state(radio_h, IN_TX);
            s->digi_auto_tx = true;
            s->digi_tx_start = time(NULL);
            s->digi_drain_ticks = 0;
            usleep(30000);
            continue;
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
            case MODE_DSTAR:
                do_dstar_tx(s, ring_rate);
                break;
            }
            tx_pend_feed(s);
            /* Let the playback thread drain before topping up again. */
            usleep(20000);

            /* Auto-unkey our own PTT once the queue is empty and the TX ring
             * has fully drained (held a few ticks so the codec's buffered tail
             * flushes). DIGI_TX_MAX_SECS is a hard backstop against a stall. */
            if (s->digi_auto_tx) {
                pthread_mutex_lock(&radio_h->tx_audio_ring.mutex);
                size_t txfill = radio_h->tx_audio_ring.count;
                pthread_mutex_unlock(&radio_h->tx_audio_ring.mutex);

                if (digi_tx_queue_pending(&radio_h->digi_tx) || txfill > 0 || tx_pend_busy(s))
                    s->digi_drain_ticks = 0;
                else
                    s->digi_drain_ticks++;

                bool timed_out = (time(NULL) - s->digi_tx_start) > DIGI_TX_MAX_SECS;
                if (s->digi_drain_ticks >= 8 || timed_out) {
                    if (s->dstar_tx_keyed) {
                        sbitx_dstar_tx_eot(s->dstar_tx);
                        s->dstar_tx_keyed = false;
                        usleep(50000);
                    }
                    radio_backend_set_txrx_state(radio_h, IN_RX);
                    s->digi_auto_tx = false;
                    s->digi_drain_ticks = 0;
                    if (timed_out) {   /* flush a stuck queue so we don't re-key */
                        s->tx_pend_len = s->tx_pend_pos = 0;
                        char junk[DIGI_TX_MSG_MAX];
                        while (digi_tx_queue_pop(&radio_h->digi_tx, junk, sizeof(junk))) {}
                    }
                }
            }
        } else {
            /* Unkeyed mid-message (by hand, or the timeout): drop the rest
             * rather than start the next transmission with it. */
            s->tx_pend_len = s->tx_pend_pos = 0;
            switch (mode) {
            case MODE_CW:
                do_cw_rx(s, ring_rate, freq_hz, radio_h->cw_wpm, radio_h->cw_pitch);
                break;
            case MODE_FT8:
                do_ft8_rx(s, ring_rate, freq_hz);
                break;
            case MODE_RTTY:
                do_rtty_rx(s, ring_rate, freq_hz,
                           radio_h->rtty_baud, radio_h->rtty_mark, radio_h->rtty_shift);
                break;
            case MODE_DSTAR:
                do_dstar_rx(s, ring_rate);
                break;
            }
            usleep(5000);
        }
    }

    /* Cleanup */
    free(s->tx_pend);
    s->tx_pend = NULL;
    s->tx_pend_len = s->tx_pend_pos = s->tx_pend_cap = 0;
    resamp_free(&s->cw_to_ring);
    resamp_free(&s->rtty_to_ring);
    resamp_free(&s->ft8_to_ring);
    resamp_free(&s->ring_to_12k);
    resamp_free(&s->ring_to_radae_speech);
    resamp_free(&s->radae_modem_to_ring);
    resamp_free(&s->ring_to_radae_modem);
    resamp_free(&s->radae_speech_to_ring);
    resamp_free(&s->ring_to_dstar_8k);
    resamp_free(&s->dstar_24k_to_ring);
    resamp_free(&s->ring_to_dstar_24k);
    resamp_free(&s->dstar_8k_to_ring);
    free(s->cw_rx_buf);
    free(s->ft8_rx_buf);
    free(s->rtty_rx_buf);
    if (s->cw_inited) sbitx_cw_shutdown();
    if (s->ft8_inited) sbitx_ft8_shutdown();
    if (s->rtty_inited) sbitx_rtty_shutdown();
    if (s->dstar_inited) {
        if (s->dstar_rx) sbitx_dstar_rx_free(s->dstar_rx);
        if (s->dstar_tx) sbitx_dstar_tx_free(s->dstar_tx);
        mbe_ambe2400EncoderFree(s->dstar_tx_enc);
    }
    if (s->radae_inited) {
        if (s->radae_tx_running) radae_tx_stop(&s->radae_ctx);
        if (s->radae_rx_running) radae_rx_stop(&s->radae_ctx);
        radae_shutdown(&s->radae_ctx);
    }
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
