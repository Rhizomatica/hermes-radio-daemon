/* modem_audio - the radio's audio as a data modem sees it, whatever carries it.
 * See modem_audio.h. */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "modem_audio.h"
#include "radio_backend.h"
#include "radio_media.h"

/* ── resampling: windowed-sinc low-pass + integer decimation / interpolation ── */

#define DECIM_TAPS_PER_FACTOR 32  /* ntaps = 32*D + 1 */
#define DECIM_CUTOFF_HZ       3400.0

typedef struct {
    uint32_t rate;       /* input rate, 0 = not configured */
    int      factor;     /* rate / 8000 */
    int      ntaps;
    float   *taps;
    float   *hist;       /* 2*ntaps, so a window is always contiguous */
    int      pos;
    int      phase;      /* input samples since the last output */
} decimator;

/* Windowed-sinc (Blackman) low-pass, fc in cycles per sample, unity DC gain. */
static void fir_design(float *taps, int ntaps, double fc)
{
    int mid = ntaps / 2;
    double sum = 0.0;
    for (int i = 0; i < ntaps; i++)
    {
        int n = i - mid;
        double sinc = n ? sin(2.0 * M_PI * fc * n) / (M_PI * n) : 2.0 * fc;
        double w = 0.42 - 0.5 * cos(2.0 * M_PI * i / (ntaps - 1))
                        + 0.08 * cos(4.0 * M_PI * i / (ntaps - 1));
        taps[i] = (float) (sinc * w);
        sum += sinc * w;
    }
    for (int i = 0; i < ntaps; i++)
        taps[i] = (float) (taps[i] / sum);
}

static void decim_free(decimator *d)
{
    free(d->taps);
    free(d->hist);
    memset(d, 0, sizeof(*d));
}

static bool decim_setup(decimator *d, uint32_t rate)
{
    decim_free(d);
    if (rate < MODEM_AUDIO_RATE || rate % MODEM_AUDIO_RATE)
        return false;

    d->factor = (int) (rate / MODEM_AUDIO_RATE);
    if (d->factor == 1)
    {
        d->rate = rate;
        return true;
    }
    d->ntaps = DECIM_TAPS_PER_FACTOR * d->factor + 1;
    d->taps = calloc((size_t) d->ntaps, sizeof(float));
    d->hist = calloc((size_t) d->ntaps * 2, sizeof(float));
    if (!d->taps || !d->hist)
    {
        decim_free(d);
        return false;
    }

    fir_design(d->taps, d->ntaps, DECIM_CUTOFF_HZ / rate);
    d->rate = rate;
    return true;
}

/* Returns the number of 8 kHz samples written to out (<= n/factor + 1). */
static size_t decim_run(decimator *d, const int16_t *in, size_t n, int16_t *out)
{
    size_t nout = 0;

    if (d->factor == 1)
    {
        memcpy(out, in, n * sizeof(*in));
        return n;
    }
    for (size_t i = 0; i < n; i++)
    {
        /* Write each sample twice so hist[pos .. pos+ntaps) is the window. */
        d->hist[d->pos] = d->hist[d->pos + d->ntaps] = (float) in[i];
        if (++d->pos == d->ntaps)
            d->pos = 0;
        if (++d->phase < d->factor)
            continue;
        d->phase = 0;

        const float *h = d->hist + d->pos;       /* oldest .. newest */
        float acc = 0.0f;
        for (int k = 0; k < d->ntaps; k++)
            acc += h[k] * d->taps[k];
        long v = lrintf(acc);
        out[nout++] = (int16_t) (v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    return nout;
}

/* 8 kHz -> rate (a multiple of 8000): zero-stuff by L and low-pass, as a
 * polyphase filter so only the non-zero inputs are multiplied. */
typedef struct {
    uint32_t rate;
    int      factor;
    int      ntaps;      /* multiple of factor */
    float   *taps;       /* scaled by factor to keep unity gain */
    float   *hist;       /* 2 * ntaps/factor */
    int      pos;
} interpolator;

static void interp_free(interpolator *ip)
{
    free(ip->taps);
    free(ip->hist);
    memset(ip, 0, sizeof(*ip));
}

static bool interp_setup(interpolator *ip, uint32_t rate)
{
    interp_free(ip);
    if (rate < MODEM_AUDIO_RATE || rate % MODEM_AUDIO_RATE)
        return false;
    ip->factor = (int) (rate / MODEM_AUDIO_RATE);
    ip->ntaps = DECIM_TAPS_PER_FACTOR * ip->factor;
    int nhist = ip->ntaps / ip->factor;
    ip->taps = calloc((size_t) ip->ntaps, sizeof(float));
    ip->hist = calloc((size_t) nhist * 2, sizeof(float));
    if (!ip->taps || !ip->hist)
    {
        interp_free(ip);
        return false;
    }
    if (ip->factor > 1)
    {
        fir_design(ip->taps, ip->ntaps, DECIM_CUTOFF_HZ / rate);
        for (int i = 0; i < ip->ntaps; i++)
            ip->taps[i] *= (float) ip->factor;
    }
    ip->rate = rate;
    return true;
}

static void interp_reset(interpolator *ip)
{
    if (ip->hist)
        memset(ip->hist, 0, sizeof(float) * 2 * (size_t) (ip->ntaps / ip->factor));
    ip->pos = 0;
}

/* n 8 kHz samples in, n * factor out. */
static size_t interp_run(interpolator *ip, const int16_t *in, size_t n, int16_t *out)
{
    if (ip->factor == 1)
    {
        memcpy(out, in, n * sizeof(*in));
        return n;
    }
    int nhist = ip->ntaps / ip->factor;
    size_t nout = 0;
    for (size_t i = 0; i < n; i++)
    {
        ip->hist[ip->pos] = ip->hist[ip->pos + nhist] = (float) in[i];
        if (++ip->pos == nhist)
            ip->pos = 0;
        const float *h = ip->hist + ip->pos;     /* oldest .. newest */
        for (int ph = 0; ph < ip->factor; ph++)
        {
            float acc = 0.0f;
            for (int k = 0; k < nhist; k++)      /* newest input meets tap ph */
                acc += h[nhist - 1 - k] * ip->taps[k * ip->factor + ph];
            long v = lrintf(acc);
            out[nout++] = (int16_t) (v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    return nout;
}

/* ── state ── */

#define MAX_RX_SINKS 4

static radio          *s_radio;
static _Atomic bool    s_started;

static decimator       s_decim;         /* only touched by the audio thread */
static bool            s_bad_rate_logged;

static pthread_mutex_t s_sink_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct { modem_audio_rx_sink fn; void *user; } s_sinks[MAX_RX_SINKS];
static int             s_nsinks;

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t) t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

bool modem_audio_init(radio *radio_h)
{
    if (s_started)
        return true;
    s_radio = radio_h;
    s_bad_rate_logged = false;
    s_started = true;
    return true;
}

bool modem_audio_running(void)
{
    return s_started;
}

/* ── RX ── */

bool modem_audio_add_rx_sink(modem_audio_rx_sink fn, void *user)
{
    bool ok = false;

    pthread_mutex_lock(&s_sink_mutex);
    if (s_nsinks < MAX_RX_SINKS)
    {
        s_sinks[s_nsinks].fn = fn;
        s_sinks[s_nsinks].user = user;
        s_nsinks++;
        ok = true;
    }
    pthread_mutex_unlock(&s_sink_mutex);
    return ok;
}

void modem_audio_remove_rx_sink(modem_audio_rx_sink fn, void *user)
{
    pthread_mutex_lock(&s_sink_mutex);
    for (int i = 0; i < s_nsinks; i++)
    {
        if (s_sinks[i].fn == fn && s_sinks[i].user == user)
        {
            s_sinks[i] = s_sinks[--s_nsinks];
            break;
        }
    }
    pthread_mutex_unlock(&s_sink_mutex);
}

void modem_audio_push_rx(const int16_t *samples, size_t nsamples, uint32_t rate)
{
    int16_t out[1024];

    if (!s_started || !samples || !nsamples)
        return;

    if (rate != s_decim.rate && !decim_setup(&s_decim, rate))
    {
        if (!s_bad_rate_logged)
            fprintf(stderr, "modem_audio: RX rate %u Hz is not a multiple of %d Hz; "
                            "RX to the modem disabled\n", rate, MODEM_AUDIO_RATE);
        s_bad_rate_logged = true;
        return;
    }

    while (nsamples)
    {
        size_t chunk = sizeof(out) / sizeof(out[0]) * (size_t) s_decim.factor;
        if (chunk > nsamples)
            chunk = nsamples;
        size_t n = decim_run(&s_decim, samples, chunk, out);
        samples += chunk;
        nsamples -= chunk;

        /* Held across the calls, so a removed sink is never running. */
        pthread_mutex_lock(&s_sink_mutex);
        for (int i = 0; i < s_nsinks; i++)
            s_sinks[i].fn(out, n, s_sinks[i].user);
        pthread_mutex_unlock(&s_sink_mutex);
    }
}

/* ── TX ──
 *
 * The samples are interpolated to the radio's rate.  On a Hamlib rig they
 * go to the playback path (tx_audio_ring); the sBitx DSP loop pulls them
 * with modem_audio_pop_tx() in place of the loopback capture. */

#define TX_DRAIN_MAX_MS   1000   /* longest wait for queued TX after an end */
#define TX_PREBUFFER_MS   40     /* sBitx: queue this much before playing */
#define TX_RING_SAMPLES   96000  /* 1 s at 96 kHz */

/* Keying and feeding may come from different threads (mercury keys from
 * its TX thread while its audio arrives on the radio's audio thread), so
 * the keying state changes and feeding serialise on s_key_mutex, and
 * s_interp is only touched under it.  PTT itself is switched outside it. */
static pthread_mutex_t s_key_mutex = PTHREAD_MUTEX_INITIALIZER;
static interpolator    s_interp;

static pthread_mutex_t s_tx_mutex = PTHREAD_MUTEX_INITIALIZER;
static int16_t         s_tx_ring[TX_RING_SAMPLES];
static size_t          s_tx_rd, s_tx_count;
static bool            s_tx_playing;    /* prebuffer reached */
static uint32_t        s_tx_underruns;
static _Atomic bool    s_tx_keyed;
static _Atomic bool    s_tx_ending;     /* end seen: the ring is draining */
static _Atomic long    s_tx_id;         /* the keyer */
static int64_t         s_tx_end_ms;

static bool tx_to_sbitx(void)
{
    return s_radio->backend_kind != RADIO_BACKEND_HAMLIB;
}

static uint32_t tx_rate(void)
{
    return tx_to_sbitx() ? 48000 : s_radio->audio_sample_rate;
}

static void tx_ring_clear(void)
{
    pthread_mutex_lock(&s_tx_mutex);
    s_tx_rd = s_tx_count = 0;
    s_tx_playing = false;
    pthread_mutex_unlock(&s_tx_mutex);
}

static size_t tx_queued(void)
{
    if (tx_to_sbitx())
    {
        pthread_mutex_lock(&s_tx_mutex);
        size_t n = s_tx_count;
        pthread_mutex_unlock(&s_tx_mutex);
        return n;
    }
    audio_ring_buffer *ring = &s_radio->tx_audio_ring;
    pthread_mutex_lock(&ring->mutex);
    size_t n = ring->count;
    pthread_mutex_unlock(&ring->mutex);
    return n;
}

static void tx_deliver(const int16_t *pcm8k, size_t n)
{
    int16_t out[MODEM_AUDIO_FRAME * 12];

    if (n > MODEM_AUDIO_FRAME)
        n = MODEM_AUDIO_FRAME;
    size_t nout = interp_run(&s_interp, pcm8k, n, out);

    if (!tx_to_sbitx())
    {
        radio_media_push_tx_audio(s_radio, out, nout);
        return;
    }
    pthread_mutex_lock(&s_tx_mutex);
    for (size_t i = 0; i < nout && s_tx_count < TX_RING_SAMPLES; i++)
    {
        s_tx_ring[(s_tx_rd + s_tx_count) % TX_RING_SAMPLES] = out[i];
        s_tx_count++;
    }
    pthread_mutex_unlock(&s_tx_mutex);
}

bool modem_audio_tx_begin(long id)
{
    bool ok = false, keyed_now = false;

    if (!s_started)
        return false;
    pthread_mutex_lock(&s_key_mutex);
    if (s_tx_keyed)
        ok = s_tx_id == id;
    else if (tx_rate() != s_interp.rate && !interp_setup(&s_interp, tx_rate()))
        fprintf(stderr, "modem_audio: TX rate %u Hz is not a multiple of %d Hz\n",
                tx_rate(), MODEM_AUDIO_RATE);
    else
    {
        interp_reset(&s_interp);
        tx_ring_clear();
        s_tx_underruns = 0;
        s_tx_id = id;
        s_tx_ending = false;
        s_tx_end_ms = 0;
        s_tx_keyed = true;
        ok = keyed_now = true;
    }
    pthread_mutex_unlock(&s_key_mutex);
    /* Outside the lock: a CAT round trip must not stall the audio thread
     * in modem_audio_tx_feed. */
    if (keyed_now)
        radio_backend_set_ptt(s_radio, IN_TX, PTT_SRC_MODEM, id);
    return ok;
}

void modem_audio_tx_feed(const int16_t *pcm8k, size_t n)
{
    pthread_mutex_lock(&s_key_mutex);
    if (s_tx_keyed)
    {
        s_tx_ending = false;            /* the modem went on after all */
        s_tx_end_ms = 0;
        while (n)
        {
            size_t chunk = n > MODEM_AUDIO_FRAME ? MODEM_AUDIO_FRAME : n;
            tx_deliver(pcm8k, chunk);
            pcm8k += chunk;
            n -= chunk;
        }
    }
    pthread_mutex_unlock(&s_key_mutex);
}

void modem_audio_tx_end(void)
{
    if (!s_tx_keyed || s_tx_ending)
        return;
    s_tx_end_ms = now_ms();
    s_tx_ending = true;
}

void modem_audio_tx_unkey(const char *why)
{
    bool was_keyed;
    long id;
    uint32_t underruns;

    pthread_mutex_lock(&s_key_mutex);
    was_keyed = s_tx_keyed;
    id = s_tx_id;
    underruns = s_tx_underruns;
    if (was_keyed)
    {
        s_tx_keyed = false;
        s_tx_ending = false;
        tx_ring_clear();
    }
    pthread_mutex_unlock(&s_key_mutex);
    if (!was_keyed)
        return;

    /* Outside the lock, as in begin.  If another modem keyed in between,
     * the backend's owner check makes this a no-op for it. */
    radio_backend_end_ptt(s_radio, PTT_SRC_MODEM, id);
    fprintf(stderr, "modem_audio: TX from modem %ld %s", id, why);
    if (underruns)
        fprintf(stderr, " (%u underruns)", underruns);
    fprintf(stderr, "\n");
}

bool modem_audio_tx_poll(void)
{
    if (s_tx_keyed && s_tx_ending
        && (tx_queued() == 0 || now_ms() - s_tx_end_ms >= TX_DRAIN_MAX_MS))
        modem_audio_tx_unkey("ended");
    return s_tx_keyed;
}

bool modem_audio_tx_keyed_by(long id)
{
    return s_tx_keyed && s_tx_id == id;
}

bool modem_audio_tx_ending(void)
{
    return s_tx_keyed && s_tx_ending;
}

bool modem_audio_tx_active(void)
{
    return s_started && s_tx_keyed;
}

size_t modem_audio_pop_tx(int16_t *out, size_t n)
{
    if (!s_started || !s_tx_keyed)
        return 0;

    pthread_mutex_lock(&s_tx_mutex);
    if (!s_tx_playing && s_tx_count < 48000 / 1000 * TX_PREBUFFER_MS)
    {
        pthread_mutex_unlock(&s_tx_mutex);
        memset(out, 0, n * sizeof(*out));
        return n;
    }
    s_tx_playing = true;
    size_t got = n < s_tx_count ? n : s_tx_count;
    for (size_t i = 0; i < got; i++)
    {
        out[i] = s_tx_ring[s_tx_rd];
        s_tx_rd = (s_tx_rd + 1) % TX_RING_SAMPLES;
    }
    s_tx_count -= got;
    if (got < n && !s_tx_ending)       /* the final drain is not an underrun */
        s_tx_underruns++;
    pthread_mutex_unlock(&s_tx_mutex);
    memset(out + got, 0, (n - got) * sizeof(*out));
    return n;
}

void modem_audio_shutdown(void)
{
    if (!s_started)
        return;
    modem_audio_tx_unkey("unkeyed at shutdown");   /* never leave the radio keyed */
    s_started = false;                  /* audio thread stops pushing */
    /* s_decim stays allocated: an audio thread may still be inside
     * modem_audio_push_rx; a later init reuses it. */
}
