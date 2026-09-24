/* rtp_audio_test - the RX stream as a ka9q-radio receiver sees it.
 *
 * Joins the test group on lo, feeds rtp_audio 48 kHz audio (a 1 kHz tone,
 * then a 5 kHz tone that must not alias into the 8 kHz stream), and checks
 * the RTP packets and the status TLVs against docs/RTP-AUDIO.md. */

#include <arpa/inet.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "radio.h"
#include "rtp_audio.h"

#define GROUP "239.255.72.99"
#define IN_RATE 48000
#define SEG_SAMPLES 24000          /* 0.5 s per tone at 48 kHz */
#define MAX_PKTS 200

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                         printf(__VA_ARGS__); printf("\n"); } } while (0)

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

int main(void)
{
    static radio r;
    static int16_t pcm[MAX_PKTS * RTP_AUDIO_FRAME];
    int data_fd = join(RTP_AUDIO_DATA_PORT), status_fd = join(RTP_AUDIO_STATUS_PORT);

    r.backend_kind = RADIO_BACKEND_HAMLIB;
    r.profiles[0].freq = 7050000;
    snprintf(r.rtp_rx_group, sizeof(r.rtp_rx_group), GROUP);
    snprintf(r.rtp_iface, sizeof(r.rtp_iface), "lo");
    r.rtp_ttl = 0;
    if (!rtp_audio_init(&r))
    {
        printf("FAIL: rtp_audio_init\n");
        return 1;
    }

    /* Feed 1 s in 10 ms blocks, a little faster than real time. */
    int16_t blk[480];
    for (int n = 0; n < 2 * SEG_SAMPLES; n += 480)
    {
        for (int i = 0; i < 480; i++)
        {
            double f = (n + i) < SEG_SAMPLES ? 1000.0 : 5000.0;
            blk[i] = (int16_t) lrint(16000.0 * sin(2 * M_PI * f * (n + i) / IN_RATE));
        }
        rtp_audio_push_rx(blk, 480, IN_RATE);
        nanosleep(&(struct timespec){ 0, 5000000 }, NULL);
    }

    /* Audio packets. */
    uint8_t buf[2048];
    struct sockaddr_in from, data_from = { 0 };
    socklen_t fl;
    int npkt = 0;
    uint32_t ssrc = 0, ts0 = 0;
    uint16_t seq0 = 0;
    for (;;)
    {
        fl = sizeof(from);
        ssize_t len = recvfrom(data_fd, buf, sizeof(buf), 0, (struct sockaddr *) &from, &fl);
        if (len < 0)
            break;
        CHECK(len == 12 + 2 * RTP_AUDIO_FRAME, "packet length %zd", len);
        CHECK((buf[0] & 0xc0) == 0x80, "RTP version");
        CHECK((buf[1] & 0x7f) == RTP_AUDIO_PT, "PT %d", buf[1] & 0x7f);
        CHECK(!!(buf[1] & 0x80) == (npkt == 0), "marker on packet %d", npkt);
        uint16_t seq = (uint16_t) get_int(buf + 2, 2);
        uint32_t ts = (uint32_t) get_int(buf + 4, 4), s = (uint32_t) get_int(buf + 8, 4);
        if (npkt == 0)
        {
            ssrc = s;
            seq0 = seq;
            ts0 = ts;
            data_from = from;
        }
        CHECK(s == ssrc, "SSRC changed");
        CHECK(seq == (uint16_t) (seq0 + npkt), "sequence %u, want %u", seq, (uint16_t) (seq0 + npkt));
        CHECK(ts == ts0 + (uint32_t) npkt * RTP_AUDIO_FRAME, "timestamp step");
        if (npkt < MAX_PKTS && len == 12 + 2 * RTP_AUDIO_FRAME)
            for (int i = 0; i < RTP_AUDIO_FRAME; i++)
                pcm[npkt * RTP_AUDIO_FRAME + i] = (int16_t) get_int(buf + 12 + 2 * i, 2);
        npkt++;
    }
    CHECK(npkt == 2 * SEG_SAMPLES / (IN_RATE / 8000) / RTP_AUDIO_FRAME, "got %d packets", npkt);

    /* 1 kHz passes at unity gain; 5 kHz (above the 4 kHz Nyquist) is gone. */
    int seg = SEG_SAMPLES / 6, guard = 400;
    double a1 = tone_amp(pcm + guard, seg - 2 * guard, 1000.0);
    double r1 = rms(pcm + guard, seg - 2 * guard);
    double r5 = rms(pcm + seg + guard, seg - 2 * guard);
    printf("1 kHz amplitude %.0f (rms %.0f), 5 kHz leak rms %.1f (%.1f dB)\n",
           a1, r1, r5, 20 * log10(r5 / r1 + 1e-12));
    CHECK(fabs(a1 - 16000.0) < 160.0, "1 kHz amplitude %.0f", a1);
    CHECK(fabs(r1 - 16000.0 / sqrt(2)) < 160.0, "1 kHz is not a clean tone");
    CHECK(r5 < r1 * 0.001, "5 kHz aliases at %.1f rms", r5);

    /* Status: TLVs as ka9q decode_status reads them, same source as data. */
    int nstat = 0;
    for (;;)
    {
        fl = sizeof(from);
        ssize_t len = recvfrom(status_fd, buf, sizeof(buf), 0, (struct sockaddr *) &from, &fl);
        if (len < 0)
            break;
        nstat++;
        CHECK(buf[0] == 0, "status packet type %d", buf[0]);
        CHECK(from.sin_port == data_from.sin_port && from.sin_addr.s_addr == data_from.sin_addr.s_addr,
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
            CHECK(l < 128 && i + l <= len, "TLV %d length %d", type, l);
            if (l >= 128 || i + l > len)
                break;
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
        CHECK(eol, "no EOL");
        CHECK(got_ssrc == ssrc, "status SSRC %llu", (unsigned long long) got_ssrc);
        CHECK(rate == 8000 && ch == 1 && pt == RTP_AUDIO_PT && enc == 2,
              "rate %llu ch %llu pt %llu enc %llu", (unsigned long long) rate,
              (unsigned long long) ch, (unsigned long long) pt, (unsigned long long) enc);
        CHECK(freq == 7050000.0, "frequency %f", freq);
        CHECK(!strcmp(desc, "hermes-radio-daemon hamlib"), "description '%s'", desc);
    }
    CHECK(nstat >= 2, "got %d status packets", nstat);

    rtp_audio_shutdown();
    printf("rtp_audio_test: %d packets, %d status, %s\n", npkt, nstat, failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
