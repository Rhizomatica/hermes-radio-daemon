/* rtp_audio_test - the RX stream as a ka9q-radio receiver sees it.
 *
 * Joins the test group on lo, feeds rtp_audio 48 kHz audio (a 1 kHz tone,
 * then a 5 kHz tone that must not alias into the 8 kHz stream), and checks
 * the RTP packets and the status TLVs against docs/RTP-AUDIO.md; then plays
 * the modem, sending TX streams and checking the in-stream PTT (marker keys,
 * empty packet ends, 200 ms dead-keyer, SSRC lock) and the 8 -> 48 kHz TX
 * audio on both the Hamlib (tx_audio_ring) and sBitx (pop) paths. */

#include <arpa/inet.h>
#include <math.h>
#include <net/if.h>
#include <pthread.h>
#include <stdatomic.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "radio.h"
#include "radio_backend.h"
#include "radio_media.h"
#include "rtp_audio.h"
#include "modem_audio.h"
#include "unity.h"
#include "test_util.h"

#define GROUP "239.255.72.99"
#define TX_GROUP "239.255.72.98"
#define IN_RATE 48000
#define SEG_SAMPLES 24000          /* 0.5 s per tone at 48 kHz */
#define MAX_PKTS 200

/* Stubs for the daemon side the module drives, called from rtp_audio's
 * threads: atomics and a lock, as the test thread reads them. */
static _Atomic int ptt_on_count, ptt_off_count;
static _Atomic long ptt_id;
static pthread_mutex_t pushed_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t pushed[48000 * 4];
static size_t npushed;

void radio_backend_set_ptt(radio *radio_h, bool txrx_state, ptt_source src, long id)
{
    (void) radio_h;
    if (txrx_state == IN_TX && src == PTT_SRC_MODEM)
    {
        ptt_on_count++;
        ptt_id = id;
    }
}

void radio_backend_end_ptt(radio *radio_h, ptt_source src, long id)
{
    (void) radio_h;
    if (src == PTT_SRC_MODEM && id == ptt_id)
        ptt_off_count++;
}

void radio_media_push_tx_audio(radio *radio_h, const int16_t *samples, size_t nsamples)
{
    (void) radio_h;
    pthread_mutex_lock(&pushed_lock);
    for (size_t i = 0; i < nsamples && npushed < sizeof(pushed) / sizeof(pushed[0]); i++)
        pushed[npushed++] = samples[i];
    pthread_mutex_unlock(&pushed_lock);
}

static size_t pushed_count(void)
{
    pthread_mutex_lock(&pushed_lock);
    size_t n = npushed;
    pthread_mutex_unlock(&pushed_lock);
    return n;
}

static int tx_fd;
static uint16_t tx_seq;
static uint32_t tx_ts;

static void msleep(int ms)
{
    nanosleep(&(struct timespec){ ms / 1000, (ms % 1000) * 1000000L }, NULL);
}

/* One modem TX packet: n samples of a 1 kHz tone (n = 0: end packet). */
static void send_tx(uint32_t ssrc, int marker, int n)
{
    uint8_t pkt[12 + 2 * RTP_AUDIO_FRAME];
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(RTP_AUDIO_DATA_PORT) };
    inet_pton(AF_INET, TX_GROUP, &sa.sin_addr);
    pkt[0] = 0x80;
    pkt[1] = (uint8_t) ((marker ? 0x80 : 0) | RTP_AUDIO_PT);
    pkt[2] = (uint8_t) (tx_seq >> 8);
    pkt[3] = (uint8_t) tx_seq;
    for (int i = 0; i < 4; i++)
    {
        pkt[4 + i] = (uint8_t) (tx_ts >> (24 - 8 * i));
        pkt[8 + i] = (uint8_t) (ssrc >> (24 - 8 * i));
    }
    for (int i = 0; i < n; i++)
    {
        int16_t v = (int16_t) lrint(16000.0 * sin(2 * M_PI * 1000.0 * (tx_ts + (uint32_t) i) / 8000.0));
        pkt[12 + 2 * i] = (uint8_t) ((uint16_t) v >> 8);
        pkt[13 + 2 * i] = (uint8_t) v;
    }
    sendto(tx_fd, pkt, 12 + 2 * (size_t) n, 0, (struct sockaddr *) &sa, sizeof(sa));
    tx_seq++;
    tx_ts += (uint32_t) n;
}

static int open_tx_sender(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    unsigned char ttl = 0;
    struct ip_mreqn m = { .imr_ifindex = (int) if_nametoindex("lo") };
    m.imr_address.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &m, sizeof(m));
    return fd;
}

/* Amplitude of the 1 kHz component of x at `rate`. */
static double tone_amp_at(const int16_t *x, int n, double rate)
{
    double re = 0, im = 0;
    for (int i = 0; i < n; i++)
    {
        re += x[i] * cos(2 * M_PI * 1000.0 * i / rate);
        im -= x[i] * sin(2 * M_PI * 1000.0 * i / rate);
    }
    return 2 * sqrt(re * re + im * im) / n;
}

static int join(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0), one = 1;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
    struct ip_mreqn m = { .imr_ifindex = (int) if_nametoindex("lo") };
    struct timeval tv = { 0, 300000 };

    inet_pton(AF_INET, GROUP, &sa.sin_addr);
    m.imr_multiaddr = sa.sin_addr;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &(int){ 1 << 20 }, sizeof(int));
    if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0
        || setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) < 0)
    {
        perror("join");
        exit(2);
    }
    return fd;
}

static uint64_t get_int(const uint8_t *p, int len)
{
    uint64_t v = 0;
    for (int i = 0; i < len; i++)
        v = v << 8 | p[i];
    return v;
}

static double rms(const int16_t *x, int n)
{
    double e = 0;
    for (int i = 0; i < n; i++)
        e += (double) x[i] * x[i];
    return sqrt(e / n);
}

/* Amplitude of the `f` Hz component of x (8 kHz). */
static double tone_amp(const int16_t *x, int n, double f)
{
    double re = 0, im = 0;
    for (int i = 0; i < n; i++)
    {
        re += x[i] * cos(2 * M_PI * f * i / 8000.0);
        im -= x[i] * sin(2 * M_PI * f * i / 8000.0);
    }
    return 2 * sqrt(re * re + im * im) / n;
}

static radio r;
static int data_fd, status_fd;
static int16_t pcm[MAX_PKTS * RTP_AUDIO_FRAME];
static int npkt;
static uint32_t rx_ssrc;
static struct sockaddr_in data_from;

void setUp(void)
{
    ptt_on_count = ptt_off_count = 0;
    pthread_mutex_lock(&pushed_lock);
    npushed = 0;
    pthread_mutex_unlock(&pushed_lock);
}

void tearDown(void) {}

/* The RX stream: 1 s of 48 kHz audio in, RTP packets out per docs/RTP-AUDIO.md. */
static void test_rx_packets(void)
{
    /* Feed 1 s in 10 ms blocks, a little faster than real time. */
    int16_t blk[480];
    for (int n = 0; n < 2 * SEG_SAMPLES; n += 480)
    {
        for (int i = 0; i < 480; i++)
        {
            double f = (n + i) < SEG_SAMPLES ? 1000.0 : 5000.0;
            blk[i] = (int16_t) lrint(16000.0 * sin(2 * M_PI * f * (n + i) / IN_RATE));
        }
        modem_audio_push_rx(blk, 480, IN_RATE);
        nanosleep(&(struct timespec){ 0, 5000000 }, NULL);
    }

    uint8_t buf[2048];
    struct sockaddr_in from;
    socklen_t fl;
    uint32_t ts0 = 0;
    uint16_t seq0 = 0;
    for (;;)
    {
        fl = sizeof(from);
        ssize_t len = recvfrom(data_fd, buf, sizeof(buf), 0, (struct sockaddr *) &from, &fl);
        if (len < 0)
            break;
        TEST_ASSERT_EQUAL_INT_MESSAGE(12 + 2 * RTP_AUDIO_FRAME, (int) len, test_msg("packet %d length", npkt));
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x80, buf[0] & 0xc0, "RTP version");
        TEST_ASSERT_EQUAL_INT_MESSAGE(RTP_AUDIO_PT, buf[1] & 0x7f, "payload type");
        TEST_ASSERT_EQUAL_INT_MESSAGE(npkt == 0, !!(buf[1] & 0x80), test_msg("marker on packet %d", npkt));
        uint16_t seq = (uint16_t) get_int(buf + 2, 2);
        uint32_t ts = (uint32_t) get_int(buf + 4, 4), s = (uint32_t) get_int(buf + 8, 4);
        if (npkt == 0)
        {
            rx_ssrc = s;
            seq0 = seq;
            ts0 = ts;
            data_from = from;
        }
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(rx_ssrc, s, "SSRC changed");
        TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t) (seq0 + npkt), seq, "sequence number");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(ts0 + (uint32_t) npkt * RTP_AUDIO_FRAME, ts, "timestamp step");
        if (npkt < MAX_PKTS)
            for (int i = 0; i < RTP_AUDIO_FRAME; i++)
                pcm[npkt * RTP_AUDIO_FRAME + i] = (int16_t) get_int(buf + 12 + 2 * i, 2);
        npkt++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(2 * SEG_SAMPLES / (IN_RATE / 8000) / RTP_AUDIO_FRAME, npkt, "packets received");
}

/* 1 kHz passes at unity gain; 5 kHz (above the 4 kHz Nyquist) is gone. */
static void test_rx_tone(void)
{
    if (npkt == 0)
        TEST_IGNORE_MESSAGE("needs test_rx_packets");
    int seg = SEG_SAMPLES / 6, guard = 400;
    double a1 = tone_amp(pcm + guard, seg - 2 * guard, 1000.0);
    double r1 = rms(pcm + guard, seg - 2 * guard);
    double r5 = rms(pcm + seg + guard, seg - 2 * guard);
    printf("1 kHz amplitude %.0f (rms %.0f), 5 kHz leak rms %.1f (%.1f dB)\n",
           a1, r1, r5, 20 * log10(r5 / r1 + 1e-12));
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(160.0, 16000.0, a1, "1 kHz amplitude");
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(160.0, 16000.0 / sqrt(2), r1, "1 kHz is not a clean tone (rms)");
    TEST_ASSERT_LESS_THAN_DOUBLE_MESSAGE(r1 * 0.001, r5, "5 kHz aliases into the stream (rms)");
}

/* Status: TLVs as ka9q decode_status reads them, from the data socket. */
static void test_rx_status(void)
{
    if (npkt == 0)
        TEST_IGNORE_MESSAGE("needs test_rx_packets");
    uint8_t buf[2048];
    struct sockaddr_in from;
    socklen_t fl;
    int nstat = 0;
    for (;;)
    {
        fl = sizeof(from);
        ssize_t len = recvfrom(status_fd, buf, sizeof(buf), 0, (struct sockaddr *) &from, &fl);
        if (len < 0)
            break;
        nstat++;
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, buf[0], "status packet type");
        TEST_ASSERT_TRUE_MESSAGE(from.sin_port == data_from.sin_port &&
                                 from.sin_addr.s_addr == data_from.sin_addr.s_addr,
                                 "status and data come from different sockets");
        uint64_t got_ssrc = 1, rate = 0, pt = 0, enc = 0, ch = 0, fbits = 0;
        char desc[128] = "";
        int eol = 0;
        for (ssize_t i = 1; i < len;)
        {
            int type = buf[i++];
            if (type == 0)
            {
                eol = 1;
                break;
            }
            int l = buf[i++];
            TEST_ASSERT_TRUE_MESSAGE(l < 128 && i + l <= len, test_msg("TLV %d length %d", type, l));
            uint64_t v = get_int(buf + i, l > 8 ? 0 : l);
            switch (type)
            {
            case 4: snprintf(desc, sizeof(desc), "%.*s", l, (char *) buf + i); break;
            case 18: got_ssrc = v; break;
            case 20: rate = v; break;
            case 33: fbits = v; break;
            case 49: ch = v; break;
            case 105: pt = v; break;
            case 107: enc = v; break;
            }
            i += l;
        }
        double freq;
        memcpy(&freq, &fbits, sizeof(freq));
        TEST_ASSERT_TRUE_MESSAGE(eol, "no EOL");
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(rx_ssrc, got_ssrc, "status SSRC");
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(8000, rate, "sample rate");
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(1, ch, "channels");
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(RTP_AUDIO_PT, pt, "payload type");
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(2, enc, "encoding");
        TEST_ASSERT_EQUAL_DOUBLE_MESSAGE(7050000.0, freq, "frequency");
        TEST_ASSERT_EQUAL_STRING("hermes-radio-daemon hamlib", desc);
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(2, nstat, "status packets received");
}

/* Hamlib path: TX goes to radio_media_push_tx_audio at 48 kHz; a marker
 * keys, another SSRC is ignored while keyed, an empty packet unkeys. */
static void test_tx_hamlib(void)
{
    send_tx(111, 0, RTP_AUDIO_FRAME);           /* no marker: does not key */
    msleep(50);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ptt_on_count, "an unmarked packet keyed");
    TEST_ASSERT_EQUAL_size_t(0, pushed_count());

    send_tx(111, 1, RTP_AUDIO_FRAME);
    for (int i = 1; i < 50; i++)
    {
        send_tx(111, 0, RTP_AUDIO_FRAME);
        if (i == 25)
            send_tx(222, 1, RTP_AUDIO_FRAME);   /* another SSRC while keyed: ignored */
        msleep(20);
    }
    msleep(50);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, ptt_on_count, "PTT ON count");
    TEST_ASSERT_EQUAL_INT_MESSAGE(111, (int) ptt_id, "PTT holder");
    TEST_ASSERT_TRUE_MESSAGE(modem_audio_tx_active(), "not active while keyed");
    pthread_mutex_lock(&pushed_lock);
    size_t n = npushed;
    double a = tone_amp_at(pushed + 2000, (int) n - 4000, 48000.0);
    pthread_mutex_unlock(&pushed_lock);
    TEST_ASSERT_EQUAL_size_t(50 * RTP_AUDIO_FRAME * 6, n);
    printf("TX 1 kHz at 48 kHz: amplitude %.0f\n", a);
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(200.0, 16000.0, a, "TX tone amplitude");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ptt_off_count, "unkeyed while the stream ran");

    send_tx(111, 0, 0);                          /* end packet */
    msleep(60);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, ptt_off_count, "the end packet did not unkey");
    TEST_ASSERT_FALSE_MESSAGE(modem_audio_tx_active(), "still active after the end packet");
}

/* Dead keyer: key, then go quiet; unkeyed after 200 ms. */
static void test_tx_dead_keyer(void)
{
    send_tx(333, 1, RTP_AUDIO_FRAME);
    msleep(100);
    TEST_ASSERT_EQUAL_INT(1, ptt_on_count);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ptt_off_count, "unkeyed before 200 ms of silence");
    msleep(250);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, ptt_off_count, "the dead keyer did not unkey after 200 ms");
}

/* The backend as rtp_audio starts with it: its threads read backend_kind,
 * so it changes only while they are stopped. */
static void restart_as(radio_backend_kind kind)
{
    rtp_audio_shutdown();
    r.backend_kind = kind;
    TEST_ASSERT_TRUE(rtp_audio_init(&r));
}

/* sBitx path: the DSP loop pops 48 kHz blocks, after a 40 ms prebuffer. */
static void test_tx_sbitx(void)
{
    int16_t blk[480];
    restart_as(RADIO_BACKEND_HFSIGNALS);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, modem_audio_pop_tx(blk, 480), "pop while unkeyed");
    send_tx(444, 1, RTP_AUDIO_FRAME);
    msleep(30);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(480, modem_audio_pop_tx(blk, 480), "pop while keyed");
    int pre_silent = 1;
    for (int i = 0; i < 480; i++)
        pre_silent &= blk[i] == 0;
    TEST_ASSERT_TRUE_MESSAGE(pre_silent, "played before the 40 ms prebuffer");
    send_tx(444, 0, RTP_AUDIO_FRAME);
    msleep(30);
    static int16_t got[1920];
    modem_audio_pop_tx(got, 1920);                  /* 2 packets = 1920 samples at 48 kHz */
    double b = tone_amp_at(got + 400, 1920 - 800, 48000.0);
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(400.0, 16000.0, b, "sBitx TX tone amplitude");
    send_tx(444, 0, 0);
    msleep(60);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, ptt_off_count, "the end packet did not unkey once drained");
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, modem_audio_pop_tx(blk, 480), "pop after unkey");
    restart_as(RADIO_BACKEND_HAMLIB);
}

int main(void)
{
    data_fd = join(RTP_AUDIO_DATA_PORT);
    status_fd = join(RTP_AUDIO_STATUS_PORT);
    tx_fd = open_tx_sender();

    r.backend_kind = RADIO_BACKEND_HAMLIB;
    r.profiles[0].freq = 7050000;
    snprintf(r.rtp_rx_group, sizeof(r.rtp_rx_group), GROUP);
    snprintf(r.rtp_tx_group, sizeof(r.rtp_tx_group), TX_GROUP);
    r.audio_sample_rate = 48000;
    snprintf(r.rtp_iface, sizeof(r.rtp_iface), "lo");
    r.rtp_ttl = 0;
    if (!rtp_audio_init(&r))
    {
        printf("rtp_audio_init failed\n");
        return 1;
    }

    UNITY_BEGIN();
    RUN_TEST(test_rx_packets);
    RUN_TEST(test_rx_tone);
    RUN_TEST(test_rx_status);
    RUN_TEST(test_tx_hamlib);
    RUN_TEST(test_tx_dead_keyer);
    RUN_TEST(test_tx_sbitx);
    int failed = UNITY_END();

    rtp_audio_shutdown();
    close(tx_fd);
    return failed;
}
