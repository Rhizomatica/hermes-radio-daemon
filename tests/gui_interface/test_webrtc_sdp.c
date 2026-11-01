/* What radiod's WebRTC audio reads of SDP and RTP (webrtc/webrtc_sdp.h):
 * a browser's offer gives the audio section's mid and Opus's payload type,
 * and the microphone's RTP packets give their Opus frames, with RTCP on the
 * same track set aside.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "unity.h"
#include "gui_interface/webrtc/webrtc_sdp.h"

void setUp(void) {}
void tearDown(void) {}

/* Chrome's shape: a data channel section first, audio with several codecs */
static void test_offer_audio_section(void)
{
    const char *sdp =
        "v=0\r\no=- 46117 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
        "a=group:BUNDLE 0 1\r\n"
        "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\na=mid:0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111 63 9 0\r\n"
        "a=rtpmap:63 red/48000/2\r\n"
        "a=mid:1\r\n"
        "a=rtpmap:111 opus/48000/2\r\na=fmtp:111 minptime=10;useinbandfec=1\r\n"
        "a=rtpmap:9 G722/8000\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 109\r\na=mid:2\r\na=rtpmap:109 opus/48000/2\r\n";
    char mid[16];
    int pt = -1;
    TEST_ASSERT_TRUE(webrtc_sdp_opus(sdp, mid, sizeof(mid), &pt));
    TEST_ASSERT_EQUAL_STRING("1", mid);
    TEST_ASSERT_EQUAL_INT(111, pt);
}

/* Firefox's: LF line ends, no trailing newline, OPUS in capitals */
static void test_offer_lf_and_case(void)
{
    const char *sdp = "v=0\nm=audio 9 UDP/TLS/RTP/SAVPF 109\na=rtpmap:109 OPUS/48000/2\na=mid:audio0";
    char mid[16];
    int pt = -1;
    TEST_ASSERT_TRUE(webrtc_sdp_opus(sdp, mid, sizeof(mid), &pt));
    TEST_ASSERT_EQUAL_STRING("audio0", mid);
    TEST_ASSERT_EQUAL_INT(109, pt);
}

static void test_offer_refused(void)
{
    char mid[4];
    int pt = -1;
    TEST_ASSERT_FALSE(webrtc_sdp_opus("v=0\r\nm=video 9 RTP/SAVPF 96\r\na=mid:0\r\n", mid, sizeof(mid), &pt));
    TEST_ASSERT_FALSE(webrtc_sdp_opus("v=0\r\nm=audio 9 RTP/SAVPF 0\r\na=mid:0\r\na=rtpmap:0 PCMU/8000\r\n",
                                      mid, sizeof(mid), &pt));
    TEST_ASSERT_FALSE(webrtc_sdp_opus("v=0\r\nm=audio 9 RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\n",
                                      mid, sizeof(mid), &pt));       /* no mid */
    TEST_ASSERT_FALSE(webrtc_sdp_opus("m=audio 9 RTP/SAVPF 111\r\na=mid:toolong\r\na=rtpmap:111 opus/48000/2\r\n",
                                      mid, sizeof(mid), &pt));       /* mid past the buffer */
    /* Opus only in a later audio section: the first one is what is answered */
    TEST_ASSERT_FALSE(webrtc_sdp_opus("m=audio 9 RTP/SAVPF 0\r\na=mid:0\r\na=rtpmap:0 PCMU/8000\r\n"
                                      "m=audio 9 RTP/SAVPF 111\r\na=mid:1\r\na=rtpmap:111 opus/48000/2\r\n",
                                      mid, sizeof(mid), &pt));
    TEST_ASSERT_FALSE(webrtc_sdp_opus(NULL, mid, sizeof(mid), &pt));
    TEST_ASSERT_EQUAL_INT(-1, pt);
}

static void test_rtp_payload(void)
{
    /* V=2, X=1 with one extension word, CC=1, then the Opus bytes */
    const uint8_t pkt[] = {
        0x91, 0x6f, 0x12, 0x34,  0, 0, 0, 1,  0xaa, 0xbb, 0xcc, 0xdd,
        1, 2, 3, 4,                         /* CSRC */
        0xbe, 0xde, 0x00, 0x01,  5, 6, 7, 8,/* extension, one word */
        0xfc, 0xff, 0xfe,
    };
    uint8_t pt;
    uint16_t seq;
    const uint8_t *p;
    size_t n;
    TEST_ASSERT_TRUE(webrtc_rtp_payload(pkt, sizeof(pkt), &pt, &seq, &p, &n));
    TEST_ASSERT_EQUAL_UINT8(111, pt);
    TEST_ASSERT_EQUAL_UINT16(0x1234, seq);
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_PTR(pkt + 24, p);

    /* padding: the last byte counts the padding bytes, itself included */
    const uint8_t padded[] = { 0xa0, 0x6f, 0, 7,  0, 0, 0, 0,  0, 0, 0, 0,  0x11, 0x22,  0, 0, 3 };
    TEST_ASSERT_TRUE(webrtc_rtp_payload(padded, sizeof(padded), &pt, &seq, &p, &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT8(0x11, p[0]);
}

static void test_rtp_refused(void)
{
    uint8_t pt;
    uint16_t seq;
    const uint8_t *p;
    size_t n;
    const uint8_t rtcp_rr[] = { 0x81, 201, 0, 7,  0, 0, 0, 1,  0, 0, 0, 0 };
    const uint8_t v1[] = { 0x40, 0x6f, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  9 };
    const uint8_t short_ext[] = { 0x90, 0x6f, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  0xbe, 0xde, 0, 9 };
    const uint8_t bad_pad[] = { 0xa0, 0x6f, 0, 1,  0, 0, 0, 0,  0, 0, 0, 0,  1, 40 };
    TEST_ASSERT_FALSE(webrtc_rtp_payload(rtcp_rr, sizeof(rtcp_rr), &pt, &seq, &p, &n));
    TEST_ASSERT_FALSE(webrtc_rtp_payload(v1, sizeof(v1), &pt, &seq, &p, &n));
    TEST_ASSERT_FALSE(webrtc_rtp_payload(short_ext, sizeof(short_ext), &pt, &seq, &p, &n));
    TEST_ASSERT_FALSE(webrtc_rtp_payload(bad_pad, sizeof(bad_pad), &pt, &seq, &p, &n));
    TEST_ASSERT_FALSE(webrtc_rtp_payload(rtcp_rr, 11, &pt, &seq, &p, &n));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_offer_audio_section);
    RUN_TEST(test_offer_lf_and_case);
    RUN_TEST(test_offer_refused);
    RUN_TEST(test_rtp_payload);
    RUN_TEST(test_rtp_refused);
    return UNITY_END();
}
