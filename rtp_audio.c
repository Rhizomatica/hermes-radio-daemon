/* rtp_audio - radio <-> modem audio as ka9q-radio style RTP multicast.
 * See rtp_audio.h and docs/RTP-AUDIO.md. */

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
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

/* ── decimator: windowed-sinc low-pass + integer decimation to 8 kHz ── */

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

    double fc = DECIM_CUTOFF_HZ / rate;           /* cycles per sample */
    int mid = d->ntaps / 2;
    double sum = 0.0;
    for (int i = 0; i < d->ntaps; i++)
    {
        int n = i - mid;
        double sinc = n ? sin(2.0 * M_PI * fc * n) / (M_PI * n) : 2.0 * fc;
        double w = 0.42 - 0.5 * cos(2.0 * M_PI * i / (d->ntaps - 1))
                        + 0.08 * cos(4.0 * M_PI * i / (d->ntaps - 1));   /* Blackman */
        d->taps[i] = (float) (sinc * w);
        sum += sinc * w;
    }
    for (int i = 0; i < d->ntaps; i++)
        d->taps[i] = (float) (d->taps[i] / sum);  /* unity gain at DC */
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
