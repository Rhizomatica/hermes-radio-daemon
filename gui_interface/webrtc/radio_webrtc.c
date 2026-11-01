/* radio_webrtc - see radio_webrtc.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "radio_webrtc.h"
#include "radiod_api.h"

#ifndef HAVE_WEBRTC

bool radio_webrtc_init(radio *radio_h) { (void) radio_h; return false; }
void radio_webrtc_shutdown(void) {}
int radio_webrtc_offer(long conn_id, const char *sdp, bool rx, bool tx,
                       char *answer, size_t answer_len)
{
    (void) conn_id; (void) sdp; (void) rx; (void) tx; (void) answer; (void) answer_len;
    return RADIOD_API_ENOTSUP;
}
bool radio_webrtc_close(long conn_id) { (void) conn_id; return false; }
void radio_webrtc_push_rx(const int16_t *samples, size_t n) { (void) samples; (void) n; }

#else

#include <errno.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#include <opus/opus.h>
#include <rtc/rtc.h>

#include "radio_backend.h"
#include "radio_media.h"
#include "radiod_log.h"
#include "webrtc_sdp.h"

#define LOG_C "webrtc"

#define SESSIONS_MAX      16
#define RTP_CLOCK         48000                 /* Opus's, whatever the input rate */
#define FRAME_TICKS       (RTP_CLOCK / 50)      /* 20 ms */
#define OPUS_BITRATE      32000
#define OPUS_PACKET_MAX   1276
#define PCM_MAX           5760                  /* 120 ms at 48 kHz, Opus's longest */
#define GATHER_TIMEOUT_MS 3000
#define NACK_PACKETS      64                    /* 1.3 s of 20 ms packets */

typedef struct {
    long conn;              /* the hermes.v1 connection; 0: a free slot */
    int pc, tr;             /* libdatachannel's ids */
    bool rx, tx;
    int pt;                 /* the payload type the offer gave Opus */
    uint32_t ts;            /* RTP timestamp of the next receive frame */
    OpusDecoder *dec;       /* the microphone, at the ring rate */
    bool have_seq;
    uint16_t seq;
    bool gathered;          /* host candidates all in the local description */
    bool dead;              /* the peer connection failed or closed */
} session;

static radio *s_radio;
static uint32_t s_rate;
static _Atomic bool s_ready;

/* s_lock guards the table, for the callbacks and the receive audio; one
 * negotiation (or close) at a time holds s_neg as well, so a peer
 * connection is created and deleted with s_lock released. */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_gathered = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t s_neg = PTHREAD_MUTEX_INITIALIZER;
static session s_sess[SESSIONS_MAX];

/* The receive side: the push_rx thread's only. */
static OpusEncoder *s_enc;
static int16_t s_frame[PCM_MAX];
static size_t s_frame_n;

static session *find(long conn)
{
    for (int i = 0; i < SESSIONS_MAX; i++)
        if (s_sess[i].conn == conn)
            return &s_sess[i];
    return NULL;
}

static uint32_t random32(void)
{
    uint32_t v;
    if (getrandom(&v, sizeof(v), 0) != sizeof(v))
        v = (uint32_t) time(NULL) * 2654435761u;
    return v;
}

static void on_log(rtcLogLevel level, const char *message)
{
    if (level <= RTC_LOG_ERROR)
        HLOGE(LOG_C, "%s", message);
    else if (level == RTC_LOG_WARNING)
        HLOGW(LOG_C, "%s", message);
    else
        HLOGD(LOG_C, "%s", message);
}

static void on_gathering(int pc, rtcGatheringState state, void *ptr)
{
    if (state != RTC_GATHERING_COMPLETE)
        return;
    pthread_mutex_lock(&s_lock);
    session *s = find((long) (intptr_t) ptr);
    if (s && s->pc == pc) {
        s->gathered = true;
        pthread_cond_broadcast(&s_gathered);
    }
    pthread_mutex_unlock(&s_lock);
}

static void on_state(int pc, rtcState state, void *ptr)
{
    long conn = (long) (intptr_t) ptr;

    if (state == RTC_CONNECTED)
        HLOGI(LOG_C, "connection %ld: audio connected", conn);
    if (state != RTC_FAILED && state != RTC_CLOSED)
        return;
    pthread_mutex_lock(&s_lock);
    session *s = find(conn);
    if (s && s->pc == pc)
        s->dead = true;
    pthread_mutex_unlock(&s_lock);
    HLOGI(LOG_C, "connection %ld: audio %s", conn, state == RTC_FAILED ? "failed" : "closed");
}

/* The browser's microphone: RTP packets carrying Opus. Decoded always, so
 * the decoder follows the stream, but transmitted only while this
 * connection holds PTT. */
static void on_track_message(int tr, const char *message, int size, void *ptr)
{
    long conn = (long) (intptr_t) ptr;
    uint8_t pt;
    uint16_t seq;
    const uint8_t *payload;
    size_t payload_len;
    int16_t pcm[PCM_MAX];
    int n = 0;

    if (size <= 0 ||
        !webrtc_rtp_payload((const uint8_t *) message, (size_t) size, &pt, &seq, &payload, &payload_len))
        return;

    pthread_mutex_lock(&s_lock);
    session *s = find(conn);
    if (s && s->tx && s->tr == tr && s->dec && pt == s->pt) {
        uint16_t gap = (uint16_t) (seq - s->seq);
        if (!s->have_seq || (gap >= 1 && gap < 0x8000)) {
            /* up to three lost packets are concealed; more starts afresh */
            if (s->have_seq && gap >= 2 && gap <= 4)
                for (int k = 1; k < gap && n + (int) (s_rate / 50) <= PCM_MAX; k++) {
                    int c = opus_decode(s->dec, NULL, 0, pcm + n, (int) (s_rate / 50), 0);
                    if (c > 0)
                        n += c;
                }
            s->have_seq = true;
            s->seq = seq;
            int c = opus_decode(s->dec, payload, (opus_int32) payload_len, pcm + n, PCM_MAX - n, 0);
            if (c > 0)
                n += c;
        }
    }
    pthread_mutex_unlock(&s_lock);

    if (n > 0 && radio_backend_holds_ptt(s_radio, PTT_SRC_WEBSOCKET, conn))
        radio_media_push_tx_audio(s_radio, pcm, (size_t) n);
}

/* Delete what the table no longer holds a live session for. With s_neg. */
static void delete_session(session *gone)
{
    if (gone->pc >= 0)
        rtcDeletePeerConnection(gone->pc);  /* waits for its callbacks */
    if (gone->dec)
        opus_decoder_destroy(gone->dec);
}

/* Take conn's slot out of the table into *out. With s_neg. */
static bool take(long conn, session *out)
{
    pthread_mutex_lock(&s_lock);
    session *s = find(conn);
    if (s) {
        *out = *s;
        memset(s, 0, sizeof(*s));
    }
    pthread_mutex_unlock(&s_lock);
    return s != NULL;
}

/* Sessions whose peer connection died: their slots back. With s_neg. */
static void reap(void)
{
    for (int i = 0; i < SESSIONS_MAX; i++) {
        session gone;
        bool dead = false;
        pthread_mutex_lock(&s_lock);
        if (s_sess[i].conn && s_sess[i].dead) {
            gone = s_sess[i];
            memset(&s_sess[i], 0, sizeof(s_sess[i]));
            dead = true;
        }
        pthread_mutex_unlock(&s_lock);
        if (dead)
            delete_session(&gone);
    }
}

static int create_pc(long conn)
{
    rtcConfiguration cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bindAddress = s_radio->webrtc_bind_address[0] ? s_radio->webrtc_bind_address : NULL;
    cfg.portRangeBegin = s_radio->webrtc_udp_min;
    cfg.portRangeEnd = s_radio->webrtc_udp_max;
    cfg.disableAutoNegotiation = true;      /* the answer is set below */

    int pc = rtcCreatePeerConnection(&cfg);
    if (pc < 0)
        return pc;
    rtcSetUserPointer(pc, (void *) (intptr_t) conn);
    rtcSetGatheringStateChangeCallback(pc, on_gathering);
    rtcSetStateChangeCallback(pc, on_state);
    return pc;
}

/* The Opus track in the offer's audio section, set up both ways once:
 * packetizer and sender reports for receive audio, NACK answers, and the
 * receiving session for the microphone. */
static int add_track(int pc, long conn, const char *mid, int pt, bool rx, bool tx, bool fresh)
{
    uint32_t ssrc = random32();
    rtcTrackInit ti;
    memset(&ti, 0, sizeof(ti));
    ti.direction = rx && tx ? RTC_DIRECTION_SENDRECV : rx ? RTC_DIRECTION_SENDONLY : RTC_DIRECTION_RECVONLY;
    ti.codec = RTC_CODEC_OPUS;
    ti.payloadType = pt;
    ti.ssrc = ssrc;
    ti.mid = mid;
    ti.name = "radiod";
    ti.msid = "radiod";
    ti.trackId = "radio";

    int tr = rtcAddTrackEx(pc, &ti);        /* the same mid: replaces its description */
    if (tr < 0 || !fresh)
        return tr;

    rtcPacketizerInit pi;
    memset(&pi, 0, sizeof(pi));
    pi.ssrc = ssrc;
    pi.cname = "radiod";
    pi.payloadType = (uint8_t) pt;
    pi.clockRate = RTP_CLOCK;
    pi.sequenceNumber = (uint16_t) random32();
    pi.timestamp = random32();
    if (rtcSetOpusPacketizer(tr, &pi) < 0 || rtcChainRtcpSrReporter(tr) < 0 ||
        rtcChainRtcpNackResponder(tr, NACK_PACKETS) < 0 || rtcChainRtcpReceivingSession(tr) < 0)
        return RTC_ERR_FAILURE;
    rtcSetUserPointer(tr, (void *) (intptr_t) conn);
    rtcSetMessageCallback(tr, on_track_message);
    return tr;
}

int radio_webrtc_offer(long conn, const char *sdp, bool rx, bool tx,
                       char *answer, size_t answer_len)
{
    char mid[64];
    int pt, rc = RADIOD_API_EIO;

    if (!s_ready)
        return RADIOD_API_ENOTSUP;
    if (conn <= 0 || (!rx && !tx) || !answer || answer_len < 2 ||
        !webrtc_sdp_opus(sdp, mid, sizeof(mid), &pt))
        return RADIOD_API_EINVAL;

    pthread_mutex_lock(&s_neg);
    reap();

    /* this connection's session, or a slot for a new one */
    pthread_mutex_lock(&s_lock);
    session *s = find(conn);
    bool fresh = !s;
    int pc = -1, tr = -1;
    if (fresh) {
        int used = 0;
        for (int i = 0; i < SESSIONS_MAX; i++)
            used += s_sess[i].conn != 0;
        s = used < s_radio->webrtc_max_sessions ? find(0) : NULL;
        if (s) {
            memset(s, 0, sizeof(*s));
            s->conn = conn;
            s->pc = s->tr = -1;
        }
    } else {
        pc = s->pc;
        tr = s->tr;
    }
    pthread_mutex_unlock(&s_lock);
    if (!s) {
        pthread_mutex_unlock(&s_neg);
        return RADIOD_API_EBUSY;
    }

    OpusDecoder *dec = NULL;
    if (fresh) {
        int err;
        dec = opus_decoder_create((opus_int32) s_rate, 1, &err);
        pc = dec ? create_pc(conn) : -1;
        pthread_mutex_lock(&s_lock);
        s->pc = pc;
        s->dec = dec;
        pthread_mutex_unlock(&s_lock);
        if (pc < 0)
            goto out;
    }

    if (rtcSetRemoteDescription(pc, sdp, "offer") < 0) {
        HLOGW(LOG_C, "connection %ld: the offer was refused", conn);
        rc = RADIOD_API_EINVAL;
        goto out;
    }
    int t = add_track(pc, conn, mid, pt, rx, tx, tr < 0);
    if (t < 0)
        goto out;
    pthread_mutex_lock(&s_lock);
    s->tr = t;
    s->pt = pt;
    s->rx = rx;
    s->tx = tx;
    if (tr < 0)
        s->ts = random32();
    pthread_mutex_unlock(&s_lock);

    if (rtcSetLocalDescription(pc, "answer") < 0)
        goto out;

    /* host candidates only, so gathering is quick */
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += GATHER_TIMEOUT_MS / 1000;
    pthread_mutex_lock(&s_lock);
    while (!s->gathered && !s->dead &&
           pthread_cond_timedwait(&s_gathered, &s_lock, &until) != ETIMEDOUT)
        ;
    bool ready = s->gathered && !s->dead;
    pthread_mutex_unlock(&s_lock);
    if (!ready) {
        HLOGW(LOG_C, "connection %ld: no host candidates", conn);
        goto out;
    }

    int len = rtcGetLocalDescription(pc, answer, (int) answer_len);
    if (len < 0)
        goto out;
    HLOGI(LOG_C, "connection %ld: audio %s%s%s", conn, fresh ? "offered" : "renegotiated",
          rx ? ", receive" : "", tx ? ", microphone" : "");
    rc = RADIOD_API_OK;

out:
    if (rc != RADIOD_API_OK && fresh) {
        session gone;
        if (take(conn, &gone))
            delete_session(&gone);
    }
    pthread_mutex_unlock(&s_neg);
    return rc;
}

bool radio_webrtc_close(long conn)
{
    session gone;

    if (!s_ready)
        return false;
    pthread_mutex_lock(&s_neg);
    bool had = take(conn, &gone);
    if (had)
        delete_session(&gone);
    reap();
    pthread_mutex_unlock(&s_neg);
    if (had)
        HLOGI(LOG_C, "connection %ld: audio ended", conn);
    return had;
}

/* One 20 ms frame to every session receiving: encoded once, and only when
 * someone listens. */
static void send_frame(size_t frame)
{
    struct { int tr; uint32_t ts; } to[SESSIONS_MAX];
    int k = 0;

    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < SESSIONS_MAX; i++) {
        session *s = &s_sess[i];
        if (s->conn && s->rx && !s->dead && s->tr >= 0) {
            to[k].tr = s->tr;
            to[k++].ts = s->ts;
            s->ts += FRAME_TICKS;
        }
    }
    pthread_mutex_unlock(&s_lock);
    if (!k)
        return;

    unsigned char pkt[OPUS_PACKET_MAX];
    opus_int32 len = opus_encode(s_enc, s_frame, (int) frame, pkt, sizeof(pkt));
    if (len <= 0)
        return;
    /* a track deleted meanwhile just refuses the send */
    for (int i = 0; i < k; i++)
        if (rtcIsOpen(to[i].tr)) {
            rtcSetTrackRtpTimestamp(to[i].tr, to[i].ts);
            rtcSendMessage(to[i].tr, (const char *) pkt, (int) len);
        }
}

void radio_webrtc_push_rx(const int16_t *samples, size_t n)
{
    size_t frame = s_rate / 50;

    if (!s_ready)
        return;
    while (n) {
        size_t take_n = frame - s_frame_n < n ? frame - s_frame_n : n;
        memcpy(s_frame + s_frame_n, samples, take_n * sizeof(*samples));
        s_frame_n += take_n;
        samples += take_n;
        n -= take_n;
        if (s_frame_n == frame) {
            s_frame_n = 0;
            send_frame(frame);
        }
    }
}

bool radio_webrtc_init(radio *radio_h)
{
    int err;

    s_radio = radio_h;
    s_rate = radio_h->audio_sample_rate ? radio_h->audio_sample_rate : 48000;
    if (s_rate != 8000 && s_rate != 12000 && s_rate != 16000 && s_rate != 24000 && s_rate != 48000) {
        HLOGW(LOG_C, "browser audio off: Opus has no %u Hz rate (audio_sample_rate)", s_rate);
        return false;
    }
    s_enc = opus_encoder_create((opus_int32) s_rate, 1, OPUS_APPLICATION_AUDIO, &err);
    if (!s_enc) {
        HLOGE(LOG_C, "browser audio off: %s", opus_strerror(err));
        return false;
    }
    opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(OPUS_BITRATE));
    rtcInitLogger(RTC_LOG_WARNING, on_log);
    rtcPreload();
    s_ready = true;
    HLOGI(LOG_C, "browser audio: up to %d sessions, UDP %u-%u%s%s", radio_h->webrtc_max_sessions,
          radio_h->webrtc_udp_min, radio_h->webrtc_udp_max,
          radio_h->webrtc_bind_address[0] ? " on " : "", radio_h->webrtc_bind_address);
    return true;
}

void radio_webrtc_shutdown(void)
{
    if (!s_ready)
        return;
    pthread_mutex_lock(&s_neg);
    s_ready = false;
    for (int i = 0; i < SESSIONS_MAX; i++) {
        session gone;
        if (s_sess[i].conn && take(s_sess[i].conn, &gone))
            delete_session(&gone);
    }
    pthread_mutex_unlock(&s_neg);
    rtcCleanup();
    opus_encoder_destroy(s_enc);
    s_enc = NULL;
}

#endif /* HAVE_WEBRTC */
