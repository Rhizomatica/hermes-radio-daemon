/* Browser audio end to end (webrtc/radio_webrtc.h): a libdatachannel peer
 * in this process plays the browser. It offers one Opus track; radiod
 * answers for receive audio, the two connect over DTLS-SRTP and receive
 * audio reaches the peer. A renegotiation adds the microphone, which reaches
 * the transmitter only while the connection holds PTT. Closing leaves no
 * track behind. Built only with libdatachannel (HAVE_WEBRTC).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include <opus/opus.h>
#include <rtc/rtc.h>

#include "unity.h"
#include "radio.h"
#include "radio_backend.h"
#include "radiod_api.h"
#include "gui_interface/webrtc/radio_webrtc.h"
#include "gui_interface/webrtc/webrtc_sdp.h"

#define CONN      7
#define RING_RATE 8000

/* radio_webrtc's way to the radio */
static atomic_bool holds_ptt;
static atomic_long tx_samples;

bool radio_backend_holds_ptt(radio *radio_h, ptt_source src, long id)
{
    (void) radio_h;
    return src == PTT_SRC_WEBSOCKET && id == CONN && holds_ptt;
}

void radio_media_push_tx_audio(radio *radio_h, const int16_t *samples, size_t nsamples)
{
    (void) radio_h;
    (void) samples;
    tx_samples += (long) nsamples;
}

/* the browser */
static radio station;
static int pc = -1, tr = -1;
static atomic_int state = -1;
static atomic_bool gathered;
static atomic_long rx_packets;
static double rx_energy;
static long rx_decoded;
static OpusDecoder *dec;

static void on_state(int id, rtcState s, void *ptr) { (void) id; (void) ptr; state = s; }

static void on_gathering(int id, rtcGatheringState s, void *ptr)
{
    (void) id;
    (void) ptr;
    if (s == RTC_GATHERING_COMPLETE)
        gathered = true;
}

static void on_message(int id, const char *message, int size, void *ptr)
{
    uint8_t pt;
    uint16_t seq;
    const uint8_t *payload;
    size_t len;
    int16_t pcm[5760];

    (void) id;
    (void) ptr;
    if (!webrtc_rtp_payload((const uint8_t *) message, (size_t) size, &pt, &seq, &payload, &len))
        return;
    int n = opus_decode(dec, payload, (opus_int32) len, pcm, 5760, 0);
    if (n <= 0)
        return;
    for (int i = 0; i < n; i++)
        rx_energy += (double) pcm[i] * pcm[i];
    rx_decoded += n;
    rx_packets++;
}

static bool wait_until(atomic_bool *flag, int ms)
{
    for (; ms > 0 && !*flag; ms -= 10)
        usleep(10000);
    return *flag;
}

static void offer(char *sdp, size_t len)
{
    rtcConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    pc = rtcCreatePeerConnection(&cfg);
    TEST_ASSERT_GREATER_OR_EQUAL(0, pc);
    rtcSetStateChangeCallback(pc, on_state);
    rtcSetGatheringStateChangeCallback(pc, on_gathering);

    rtcTrackInit ti;
    memset(&ti, 0, sizeof(ti));
    ti.direction = RTC_DIRECTION_SENDRECV;
    ti.codec = RTC_CODEC_OPUS;
    ti.payloadType = 111;
    ti.ssrc = 4242;
    ti.mid = "0";
    ti.name = "mic";
    ti.msid = "browser";
    ti.trackId = "mic";
    tr = rtcAddTrackEx(pc, &ti);
    TEST_ASSERT_GREATER_OR_EQUAL(0, tr);

    rtcPacketizerInit pi;
    memset(&pi, 0, sizeof(pi));
    pi.ssrc = 4242;
    pi.cname = "browser";
    pi.payloadType = 111;
    pi.clockRate = 48000;
    TEST_ASSERT_GREATER_OR_EQUAL(0, rtcSetOpusPacketizer(tr, &pi));
    rtcChainRtcpSrReporter(tr);
    rtcChainRtcpReceivingSession(tr);
    rtcSetMessageCallback(tr, on_message);

    rtcSetLocalDescription(pc, "offer");
    TEST_ASSERT_TRUE_MESSAGE(wait_until(&gathered, 5000), "the offer's candidates");
    TEST_ASSERT_GREATER_THAN(0, rtcGetLocalDescription(pc, sdp, (int) len));
}

/* The session and the peer, whatever an assertion skipped: rtcCleanup
 * hangs on a peer connection still open. */
static void hang_up(void)
{
    radio_webrtc_close(CONN);
    if (tr >= 0)
        rtcDeleteTrack(tr);
    if (pc >= 0)
        rtcDeletePeerConnection(pc);
    pc = tr = -1;
}

void setUp(void) {}
void tearDown(void) { hang_up(); }

/* 0.5 s of the browser's microphone; what reached the transmitter */
static long speak(OpusEncoder *enc, uint32_t *ts)
{
    long before = tx_samples;
    for (int f = 0; f < 25; f++) {
        int16_t pcm[960];
        unsigned char pkt[1276];
        for (int i = 0; i < 960; i++)
            pcm[i] = (int16_t) (8000 * sin(2 * M_PI * 700 * (f * 960 + i) / 48000.0));
        int len = opus_encode(enc, pcm, 960, pkt, sizeof(pkt));
        rtcSetTrackRtpTimestamp(tr, *ts);
        *ts += 960;
        rtcSendMessage(tr, (const char *) pkt, len);
        usleep(20000);
    }
    usleep(300000);
    return tx_samples - before;
}

/* Track ids libdatachannel still holds, the peer's own included */
static int tracks_left(void)
{
    char mid[64];
    int n = 0;
    for (int id = 1; id < 1000; id++)
        n += rtcGetTrackMid(id, mid, sizeof(mid)) >= 0;
    return n;
}

static void test_audio_both_ways(void)
{
    static char sdp[16384], with_dc[16384 + 128], answer[16384];

    offer(sdp, sizeof(sdp));

    /* a data channel would start SCTP, which radiod refuses */
    snprintf(with_dc, sizeof(with_dc), "%sm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\na=mid:1\r\n", sdp);
    TEST_ASSERT_EQUAL_INT(RADIOD_API_EINVAL, radio_webrtc_offer(CONN, with_dc, true, true, answer, sizeof(answer)));

    /* receive audio first, as a page does before it asks for the microphone */
    TEST_ASSERT_EQUAL_INT(RADIOD_API_OK, radio_webrtc_offer(CONN, sdp, true, false, answer, sizeof(answer)));
    TEST_ASSERT_NOT_NULL(strstr(answer, "a=sendonly"));
    TEST_ASSERT_NOT_NULL(strstr(answer, "a=rtpmap:111 opus/48000"));
    TEST_ASSERT_NOT_NULL(strstr(answer, "typ host"));
    TEST_ASSERT_EQUAL_INT(0, rtcSetRemoteDescription(pc, answer, "answer"));
    for (int ms = 10000; ms > 0 && !(state == RTC_CONNECTED && rtcIsOpen(tr)); ms -= 10)
        usleep(10000);
    TEST_ASSERT_TRUE_MESSAGE(state == RTC_CONNECTED && rtcIsOpen(tr), "connected");

    /* 1 s of 440 Hz at the ring rate, in 20 ms chunks */
    int16_t chunk[RING_RATE / 50];
    for (int f = 0; f < 50; f++) {
        for (int i = 0; i < RING_RATE / 50; i++)
            chunk[i] = (int16_t) (8000 * sin(2 * M_PI * 440 * (f * (RING_RATE / 50) + i) / RING_RATE));
        radio_webrtc_push_rx(chunk, RING_RATE / 50);
        usleep(20000);
    }
    usleep(300000);
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(45, rx_packets, "receive packets at the browser");
    double rms = sqrt(rx_energy / (double) rx_decoded);
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1500, 8000 / sqrt(2), rms, "receive level");

    int err;
    OpusEncoder *enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
    TEST_ASSERT_NOT_NULL(enc);
    uint32_t ts = 0;
    holds_ptt = true;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, speak(enc, &ts), "microphone not offered");

    /* the microphone, by renegotiation: discarded without PTT, sent with it */
    TEST_ASSERT_EQUAL_INT(0, rtcSetLocalDescription(pc, "offer"));
    TEST_ASSERT_GREATER_THAN(0, rtcGetLocalDescription(pc, sdp, (int) sizeof(sdp)));
    TEST_ASSERT_EQUAL_INT(RADIOD_API_OK, radio_webrtc_offer(CONN, sdp, true, true, answer, sizeof(answer)));
    TEST_ASSERT_NOT_NULL(strstr(answer, "a=sendrecv"));
    TEST_ASSERT_EQUAL_INT(0, rtcSetRemoteDescription(pc, answer, "answer"));
    holds_ptt = false;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, speak(enc, &ts), "microphone without PTT");
    holds_ptt = true;
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(24 * RING_RATE / 50, speak(enc, &ts), "microphone with PTT");
    holds_ptt = false;
    opus_encoder_destroy(enc);

    TEST_ASSERT_TRUE(radio_webrtc_close(CONN));
    TEST_ASSERT_FALSE(radio_webrtc_close(CONN));
    hang_up();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, tracks_left(), "tracks left after the session");
}

int main(void)
{
    int err;

    station.audio_sample_rate = RING_RATE;
    station.webrtc_max_sessions = 4;
    if (!radio_webrtc_init(&station))
        return 1;
    dec = opus_decoder_create(48000, 1, &err);

    UNITY_BEGIN();
    RUN_TEST(test_audio_both_ways);
    int failures = UNITY_END();

    opus_decoder_destroy(dec);
    radio_webrtc_shutdown();
    return failures;
}
