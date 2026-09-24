/* rtp_audio - radio <-> modem audio as ka9q-radio style RTP multicast.
 * See rtp_audio.h and docs/RTP-AUDIO.md. */

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "rtp_audio.h"
#include "radio_backend.h"
#include "radio_media.h"

/* ka9q-radio status.h type codes (only those we send). */
enum {
    TLV_EOL = 0,
    TLV_GPS_TIME = 3,
    TLV_DESCRIPTION = 4,
    TLV_RTP_TIMESNAP = 8,
    TLV_OUTPUT_SSRC = 18,
    TLV_OUTPUT_SAMPRATE = 20,
    TLV_RADIO_FREQUENCY = 33,
    TLV_OUTPUT_CHANNELS = 49,
    TLV_RTP_PT = 105,
    TLV_OUTPUT_ENCODING = 107,
};
#define PKT_STATUS     0         /* first byte of a status packet */
#define ENC_S16BE      2         /* ka9q enum encoding */

#define STATUS_INTERVAL_NS 500000000LL
#define RX_RING_SAMPLES    8000   /* 1 s at 8 kHz between audio and sender thread */

/* GPS epoch on the unix time scale, and GPS-UTC leap seconds (ka9q misc.h). */
#define GPS_EPOCH_UNIX 315964800LL
#define GPS_UTC_OFFSET 18LL

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
    if (rate < RTP_AUDIO_RATE || rate % RTP_AUDIO_RATE)
        return false;

    d->factor = (int) (rate / RTP_AUDIO_RATE);
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
    if (rate < RTP_AUDIO_RATE || rate % RTP_AUDIO_RATE)
        return false;
    ip->factor = (int) (rate / RTP_AUDIO_RATE);
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

static radio          *s_radio;
static _Atomic bool    s_started;
static volatile bool   s_run;
static pthread_t       s_tid;
static int             s_fd = -1;
static struct sockaddr_in s_data_addr, s_status_addr;

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cond;
static int16_t         s_ring[RX_RING_SAMPLES];
static size_t          s_rd, s_count;
static uint32_t        s_dropped;       /* samples lost to a full ring */
static decimator       s_decim;         /* only touched by the audio thread */
static bool            s_bad_rate_logged;

static uint32_t        s_ssrc;
static uint16_t        s_seq;
static uint32_t        s_ts;            /* timestamp of the next packet */
static bool            s_marker;
static int64_t         s_snap_ns;       /* wall clock at the last packet sent */
static uint32_t        s_snap_ts;       /* ...and its RTP timestamp */

static int64_t now_ns(clockid_t clk)
{
    struct timespec t;
    clock_gettime(clk, &t);
    return (int64_t) t.tv_sec * 1000000000LL + t.tv_nsec;
}

static uint32_t random32(void)
{
    uint32_t r;
    if (getrandom(&r, sizeof(r), 0) != sizeof(r))
        r = (uint32_t) now_ns(CLOCK_REALTIME) ^ (uint32_t) getpid();
    return r;
}

/* ── status TLVs (ka9q-radio encoding) ── */

static void tlv_int(uint8_t **bp, int type, uint64_t x)
{
    uint8_t *cp = *bp;
    int len = 8;

    *cp++ = (uint8_t) type;
    while (len > 0 && (x >> 56) == 0)   /* strip leading zero bytes; 0 -> length 0 */
    {
        x <<= 8;
        len--;
    }
    *cp++ = (uint8_t) len;
    for (int i = 0; i < len; i++, x <<= 8)
        *cp++ = (uint8_t) (x >> 56);
    *bp = cp;
}

static void tlv_double(uint8_t **bp, int type, double v)
{
    uint64_t x;
    memcpy(&x, &v, sizeof(x));
    tlv_int(bp, type, x);
}

static void tlv_string(uint8_t **bp, int type, const char *s)
{
    size_t len = strlen(s);
    uint8_t *cp = *bp;

    if (len > 127)
        len = 127;
    *cp++ = (uint8_t) type;
    *cp++ = (uint8_t) len;
    memcpy(cp, s, len);
    *bp = cp + len;
}

static void send_status(void)
{
    uint8_t pkt[256], *bp = pkt;
    char desc[96];
    const char *backend = !s_radio ? "radio"
                        : s_radio->backend_kind == RADIO_BACKEND_HAMLIB ? "hamlib" : "sbitx";
    double freq = 0.0;

    if (s_radio)
        freq = (double) s_radio->profiles[s_radio->profile_active_idx].freq;
    snprintf(desc, sizeof(desc), "hermes-radio-daemon %s", backend);

    pthread_mutex_lock(&s_mutex);
    int64_t snap_ns = s_snap_ns;
    uint32_t snap_ts = s_snap_ts;
    pthread_mutex_unlock(&s_mutex);
    if (!snap_ns)                       /* nothing sent yet: the next packet's */
    {
        snap_ns = now_ns(CLOCK_REALTIME);
        snap_ts = s_ts;
    }

    *bp++ = PKT_STATUS;
    tlv_int(&bp, TLV_GPS_TIME,
            (uint64_t) (snap_ns - (GPS_EPOCH_UNIX - GPS_UTC_OFFSET) * 1000000000LL));
    tlv_string(&bp, TLV_DESCRIPTION, desc);
    tlv_int(&bp, TLV_RTP_TIMESNAP, snap_ts);
    tlv_int(&bp, TLV_OUTPUT_SSRC, s_ssrc);
    tlv_int(&bp, TLV_OUTPUT_SAMPRATE, RTP_AUDIO_RATE);
    tlv_double(&bp, TLV_RADIO_FREQUENCY, freq);
    tlv_int(&bp, TLV_OUTPUT_CHANNELS, 1);
    tlv_int(&bp, TLV_RTP_PT, RTP_AUDIO_PT);
    tlv_int(&bp, TLV_OUTPUT_ENCODING, ENC_S16BE);
    *bp++ = TLV_EOL;

    if (sendto(s_fd, pkt, (size_t) (bp - pkt), 0,
               (struct sockaddr *) &s_status_addr, sizeof(s_status_addr)) < 0)
        fprintf(stderr, "rtp_audio: status send: %s\n", strerror(errno));
}

static void send_frame(const int16_t *pcm, uint32_t skipped)
{
    uint8_t pkt[12 + RTP_AUDIO_FRAME * 2];

    s_ts += skipped;                    /* the modem sees the gap, not a splice */
    pkt[0] = 0x80;                      /* V=2, no padding/extension/CSRC */
    pkt[1] = (uint8_t) ((s_marker ? 0x80 : 0) | RTP_AUDIO_PT);
    pkt[2] = (uint8_t) (s_seq >> 8);
    pkt[3] = (uint8_t) s_seq;
    pkt[4] = (uint8_t) (s_ts >> 24);
    pkt[5] = (uint8_t) (s_ts >> 16);
    pkt[6] = (uint8_t) (s_ts >> 8);
    pkt[7] = (uint8_t) s_ts;
    pkt[8] = (uint8_t) (s_ssrc >> 24);
    pkt[9] = (uint8_t) (s_ssrc >> 16);
    pkt[10] = (uint8_t) (s_ssrc >> 8);
    pkt[11] = (uint8_t) s_ssrc;
    for (int i = 0; i < RTP_AUDIO_FRAME; i++)
    {
        pkt[12 + 2 * i] = (uint8_t) ((uint16_t) pcm[i] >> 8);
        pkt[13 + 2 * i] = (uint8_t) pcm[i];
    }

    if (sendto(s_fd, pkt, sizeof(pkt), 0,
               (struct sockaddr *) &s_data_addr, sizeof(s_data_addr)) < 0
        && errno != EAGAIN && errno != ENOBUFS)
        fprintf(stderr, "rtp_audio: send: %s\n", strerror(errno));

    pthread_mutex_lock(&s_mutex);
    s_snap_ns = now_ns(CLOCK_REALTIME);
    s_snap_ts = s_ts;
    pthread_mutex_unlock(&s_mutex);

    s_marker = false;
    s_seq++;
    s_ts += RTP_AUDIO_FRAME;
}

static void *sender_thread(void *arg)
{
    (void) arg;
    int16_t frame[RTP_AUDIO_FRAME];
    int64_t next_status = now_ns(CLOCK_MONOTONIC);

    while (s_run)
    {
        if (now_ns(CLOCK_MONOTONIC) >= next_status)
        {
            send_status();
            next_status += STATUS_INTERVAL_NS;
        }

        pthread_mutex_lock(&s_mutex);
        while (s_run && s_count < RTP_AUDIO_FRAME)
        {
            struct timespec dl = { (time_t) (next_status / 1000000000LL),
                                   (long) (next_status % 1000000000LL) };
            if (pthread_cond_timedwait(&s_cond, &s_mutex, &dl) == ETIMEDOUT)
                break;
        }
        bool have = s_count >= RTP_AUDIO_FRAME;
        uint32_t skipped = 0;
        if (have)
        {
            for (int i = 0; i < RTP_AUDIO_FRAME; i++)
            {
                frame[i] = s_ring[s_rd];
                s_rd = (s_rd + 1) % RX_RING_SAMPLES;
            }
            s_count -= RTP_AUDIO_FRAME;
            skipped = s_dropped;
            s_dropped = 0;
        }
        pthread_mutex_unlock(&s_mutex);

        if (have)
            send_frame(frame, skipped);
    }
    return NULL;
}

void rtp_audio_push_rx(const int16_t *samples, size_t nsamples, uint32_t rate)
{
    int16_t out[1024];

    if (!s_started || !samples || !nsamples)
        return;

    if (rate != s_decim.rate && !decim_setup(&s_decim, rate))
    {
        if (!s_bad_rate_logged)
            fprintf(stderr, "rtp_audio: RX rate %u Hz is not a multiple of %d Hz; "
                            "RX stream disabled\n", rate, RTP_AUDIO_RATE);
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

        pthread_mutex_lock(&s_mutex);
        for (size_t i = 0; i < n; i++)
        {
            if (s_count == RX_RING_SAMPLES)          /* sender stalled: drop oldest */
            {
                s_rd = (s_rd + 1) % RX_RING_SAMPLES;
                s_count--;
                s_dropped++;
            }
            s_ring[(s_rd + s_count) % RX_RING_SAMPLES] = out[i];
            s_count++;
        }
        if (s_count >= RTP_AUDIO_FRAME)
            pthread_cond_signal(&s_cond);
        pthread_mutex_unlock(&s_mutex);
    }
}

/* ── socket ── */

static bool parse_group(const char *s, uint16_t port, struct sockaddr_in *sa)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    return inet_pton(AF_INET, s, &sa->sin_addr) == 1
           && IN_MULTICAST(ntohl(sa->sin_addr.s_addr));
}

/* Output socket for `group` on `iface` (lo when empty or ttl == 0, as
 * ka9q-radio does: TTL 0 never leaves the host). */
static int open_output(const struct sockaddr_in *group, const char *iface, int ttl)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    bool loopback = ttl <= 0 || !iface || !iface[0] || !strcmp(iface, "lo");
    unsigned char t = (unsigned char) (ttl < 0 ? 0 : ttl > 255 ? 255 : ttl);
    unsigned char loop = 1;
    struct ip_mreqn mreqn = { .imr_multiaddr = group->sin_addr };

    mreqn.imr_ifindex = (int) if_nametoindex(loopback ? "lo" : iface);
    mreqn.imr_address.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
    if (!mreqn.imr_ifindex)
    {
        fprintf(stderr, "rtp_audio: no interface %s\n", loopback ? "lo" : iface);
        close(fd);
        return -1;
    }
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &t, sizeof(t)) < 0
        || setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0
        || setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &mreqn, sizeof(mreqn)) < 0)
    {
        fprintf(stderr, "rtp_audio: multicast setup on %s: %s\n",
                loopback ? "lo" : iface, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* ── TX stream (modem -> radio) ──
 *
 * The modem answers each RX packet with one TX packet while it transmits,
 * so TX arrives at the radio's own sample rate.  PTT is the stream: a
 * marker packet keys, an empty packet ends the transmission (the radio
 * unkeys once what it holds has been played), and 200 ms without a TX
 * packet while keyed unkeys it anyway (a modem that died keyed).  While
 * keyed, only the keying SSRC is heard.
 *
 * The samples are interpolated to the radio's rate.  On a Hamlib rig they
 * go to the playback path (tx_audio_ring); the sBitx DSP loop pulls them
 * with rtp_audio_pop_tx() in place of the loopback capture. */

#define TX_DEAD_KEYER_MS  200
#define TX_DRAIN_MAX_MS   1000   /* longest wait for queued TX after an end packet */
#define TX_PREBUFFER_MS   40     /* sBitx: queue this much before playing */
#define TX_RING_SAMPLES   96000  /* 1 s at 96 kHz */

static int             s_tx_fd = -1;
static pthread_t       s_tx_tid;
static volatile bool   s_tx_run;
static bool            s_tx_started;
static interpolator    s_interp;        /* TX thread only */

static pthread_mutex_t s_tx_mutex = PTHREAD_MUTEX_INITIALIZER;
static int16_t         s_tx_ring[TX_RING_SAMPLES];
static size_t          s_tx_rd, s_tx_count;
static bool            s_tx_playing;    /* prebuffer reached */
static uint32_t        s_tx_underruns;
static _Atomic bool    s_tx_keyed;
static _Atomic bool    s_tx_ending;     /* end packet seen: the ring is draining */
static uint32_t        s_tx_ssrc;       /* the keying SSRC */

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
    int16_t out[RTP_AUDIO_FRAME * 12];

    if (n > RTP_AUDIO_FRAME)
        n = RTP_AUDIO_FRAME;
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

size_t rtp_audio_pop_tx(int16_t *out, size_t n)
{
    if (!s_tx_started || !s_tx_keyed)
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

/* Minimal RTP parse: version, CSRCs, extension, padding. */
static bool rtp_parse(const uint8_t *p, size_t len, bool *marker, uint32_t *ssrc,
                      const uint8_t **payload, size_t *plen)
{
    if (len < 12 || (p[0] >> 6) != 2 || (p[1] & 0x7f) != RTP_AUDIO_PT)
        return false;
    size_t off = 12 + 4u * (p[0] & 0x0f);
    if (off > len)
        return false;
    if (p[0] & 0x10)
    {
        if (off + 4 > len)
            return false;
        off += 4 + 4u * (size_t) ((p[off + 2] << 8) | p[off + 3]);
        if (off > len)
            return false;
    }
    size_t end = len;
    if (p[0] & 0x20)
    {
        if (p[len - 1] == 0 || p[len - 1] > len - off)
            return false;
        end -= p[len - 1];
    }
    *marker = (p[1] & 0x80) != 0;
    *ssrc = (uint32_t) p[8] << 24 | (uint32_t) p[9] << 16 | (uint32_t) p[10] << 8 | p[11];
    *payload = p + off;
    *plen = end - off;
    return true;
}

static void tx_unkey(const char *why)
{
    s_tx_keyed = false;
    s_tx_ending = false;
    radio_backend_end_ptt(s_radio, PTT_SRC_RTP, (long) s_tx_ssrc);
    fprintf(stderr, "rtp_audio: TX from ssrc %u %s", s_tx_ssrc, why);
    if (s_tx_underruns)
        fprintf(stderr, " (%u underruns)", s_tx_underruns);
    fprintf(stderr, "\n");
    tx_ring_clear();
}

static void *tx_thread(void *arg)
{
    (void) arg;
    uint8_t pkt[2048];
    int64_t last_ms = 0, end_ms = 0;     /* end_ms != 0: draining after an end packet */

    while (s_tx_run)
    {
        struct pollfd pfd = { .fd = s_tx_fd, .events = POLLIN };
        int ready = poll(&pfd, 1, 10);
        int64_t now = now_ns(CLOCK_MONOTONIC) / 1000000;

        if (ready > 0)
        {
            ssize_t len = recv(s_tx_fd, pkt, sizeof(pkt), 0);
            bool marker;
            uint32_t ssrc;
            const uint8_t *pl;
            size_t plen;
            if (len > 0 && rtp_parse(pkt, (size_t) len, &marker, &ssrc, &pl, &plen)
                && (!s_tx_keyed || ssrc == s_tx_ssrc))
            {
                if (!s_tx_keyed && marker && plen > 0)
                {
                    if (tx_rate() != s_interp.rate && !interp_setup(&s_interp, tx_rate()))
                    {
                        fprintf(stderr, "rtp_audio: TX rate %u Hz is not a multiple of %d Hz\n",
                                tx_rate(), RTP_AUDIO_RATE);
                        continue;
                    }
                    interp_reset(&s_interp);
                    tx_ring_clear();
                    s_tx_underruns = 0;
                    s_tx_ssrc = ssrc;
                    s_tx_keyed = true;
                    radio_backend_set_ptt(s_radio, IN_TX, PTT_SRC_RTP, (long) ssrc);
                    fprintf(stderr, "rtp_audio: TX from ssrc %u keyed\n", ssrc);
                }
                if (s_tx_keyed)
                {
                    last_ms = now;
                    if (plen == 0)
                    {
                        end_ms = end_ms ? end_ms : now;
                        s_tx_ending = true;
                    }
                    else
                    {
                        end_ms = 0;         /* the modem went on after all */
                        s_tx_ending = false;
                        int16_t pcm[RTP_AUDIO_FRAME];
                        size_t n = plen / 2 > RTP_AUDIO_FRAME ? RTP_AUDIO_FRAME : plen / 2;
                        for (size_t i = 0; i < n; i++)
                            pcm[i] = (int16_t) ((uint16_t) pl[2 * i] << 8 | pl[2 * i + 1]);
                        tx_deliver(pcm, n);
                    }
                }
            }
        }

        if (!s_tx_keyed)
            continue;
        if (end_ms)
        {
            if (tx_queued() == 0 || now - end_ms >= TX_DRAIN_MAX_MS)
            {
                tx_unkey("ended");
                end_ms = 0;
            }
        }
        else if (now - last_ms >= TX_DEAD_KEYER_MS)
            tx_unkey("stopped sending while keyed: unkeyed by the 200 ms dead-keyer");
    }

    if (s_tx_keyed)                     /* never leave the radio keyed */
        tx_unkey("unkeyed at shutdown");
    return NULL;
}

static int open_input(const struct sockaddr_in *group, const char *iface, int ttl)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), one = 1;
    if (fd < 0)
        return -1;
    bool loopback = ttl <= 0 || !iface || !iface[0] || !strcmp(iface, "lo");
    struct ip_mreqn m = { .imr_multiaddr = group->sin_addr };
    m.imr_ifindex = (int) if_nametoindex(loopback ? "lo" : iface);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    /* Bind to the group, not INADDR_ANY: the RX group uses the same port. */
    if (bind(fd, (const struct sockaddr *) group, sizeof(*group)) < 0
        || setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) < 0)
    {
        fprintf(stderr, "rtp_audio: listen on TX group: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static bool tx_start(radio *radio_h)
{
    struct sockaddr_in tx_addr;
    if (!parse_group(radio_h->rtp_tx_group, RTP_AUDIO_DATA_PORT, &tx_addr))
    {
        fprintf(stderr, "rtp_audio: rtp_tx_group '%s' is not an IPv4 multicast address\n",
                radio_h->rtp_tx_group);
        return false;
    }
    s_tx_fd = open_input(&tx_addr, radio_h->rtp_iface, radio_h->rtp_ttl);
    if (s_tx_fd < 0)
        return false;
    s_tx_keyed = false;
    tx_ring_clear();
    s_tx_run = true;
    if (pthread_create(&s_tx_tid, NULL, tx_thread, NULL) != 0)
    {
        s_tx_run = false;
        close(s_tx_fd);
        s_tx_fd = -1;
        return false;
    }
    s_tx_started = true;
    fprintf(stderr, "rtp_audio: TX stream from %s:%d\n", radio_h->rtp_tx_group, RTP_AUDIO_DATA_PORT);
    return true;
}

static void tx_stop(void)
{
    if (!s_tx_started)
        return;
    s_tx_run = false;
    pthread_join(s_tx_tid, NULL);
    s_tx_started = false;
    close(s_tx_fd);
    s_tx_fd = -1;
}

bool rtp_audio_init(radio *radio_h)
{
    if (s_started)
        return true;

    if (!parse_group(radio_h->rtp_rx_group, RTP_AUDIO_DATA_PORT, &s_data_addr))
    {
        fprintf(stderr, "rtp_audio: rtp_rx_group '%s' is not an IPv4 multicast address\n",
                radio_h->rtp_rx_group);
        return false;
    }
    s_status_addr = s_data_addr;
    s_status_addr.sin_port = htons(RTP_AUDIO_STATUS_PORT);

    s_fd = open_output(&s_data_addr, radio_h->rtp_iface, radio_h->rtp_ttl);
    if (s_fd < 0)
        return false;

    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&s_cond, &ca);
    pthread_condattr_destroy(&ca);

    s_radio = radio_h;
    s_rd = s_count = 0;
    s_dropped = 0;
    s_ssrc = random32();
    s_seq = (uint16_t) random32();
    s_ts = random32();
    s_marker = true;
    s_snap_ns = 0;
    s_bad_rate_logged = false;

    s_run = true;
    if (pthread_create(&s_tid, NULL, sender_thread, NULL) != 0)
    {
        perror("rtp_audio: pthread_create");
        s_run = false;
        close(s_fd);
        s_fd = -1;
        pthread_cond_destroy(&s_cond);
        return false;
    }
    s_started = true;
    if (!tx_start(radio_h))
    {
        rtp_audio_shutdown();
        return false;
    }
    fprintf(stderr, "rtp_audio: RX stream to %s:%d (status :%d), ssrc %u, iface %s ttl %d\n",
            radio_h->rtp_rx_group, RTP_AUDIO_DATA_PORT, RTP_AUDIO_STATUS_PORT, s_ssrc,
            radio_h->rtp_ttl <= 0 || !radio_h->rtp_iface[0] ? "lo" : radio_h->rtp_iface,
            radio_h->rtp_ttl);
    return true;
}

void rtp_audio_shutdown(void)
{
    if (!s_started)
        return;
    s_started = false;                  /* audio thread stops pushing */
    tx_stop();

    pthread_mutex_lock(&s_mutex);
    s_run = false;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_mutex);
    pthread_join(s_tid, NULL);

    close(s_fd);
    s_fd = -1;
    pthread_cond_destroy(&s_cond);
    /* s_decim stays allocated: an audio thread may still be inside
     * rtp_audio_push_rx; a later init reuses it. */
}
