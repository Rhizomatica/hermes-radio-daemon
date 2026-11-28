/* ale_link - see ale_link.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include "ale_link.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ale_engine.h"
#include "modem_audio.h"
#include "radio_backend.h"
#include "radiod_log.h"

#define LOG_C "ale"

_Static_assert(MODEM_AUDIO_RATE == ALE_ENGINE_RATE, "modem_audio and the ALE engine share a rate");
_Static_assert(ALE_COMBINE_MAX <= ALE_ENGINE_MAX_KS, "ale:combine holds no more K values than the engine takes");
_Static_assert(ALE_COMBINE_K_MIN >= 2 && ALE_COMBINE_K_MAX <= ALE_ENGINE_MAX_K, "ale:combine K values are ones the engine takes");
_Static_assert(ALE_MODE_USB == MODE_USB && ALE_MODE_LSB == MODE_LSB, "ALE's sidebands are radio.h's modes");

/* RX audio between the sink (the radio's audio thread) and the worker. */
#define ALE_RING_SECONDS     2
#define ALE_RING_SAMPLES     (ALE_ENGINE_RATE * ALE_RING_SECONDS)
/* The worker's period: it drives the link layer's timers and keeps the
 * transmitter fed (the engine keeps 200 ms queued ahead). */
#define ALE_WORKER_PERIOD_MS 5
/* Dropped audio is reported at most this often. */
#define ALE_DROP_LOG_MS      10000
/* Samples the worker feeds the engine at a time. */
#define ALE_FEED_BLOCK       1024
/* How long an operator's action waits for the worker. */
#define ALE_CMD_TIMEOUT_MS   3000
#define MS_PER_S             1000
#define NS_PER_MS            1000000L

static radio           *s_radio;
static int16_t          s_ring[ALE_RING_SAMPLES];
static _Atomic uint64_t s_head;          /* written by the sink */
static _Atomic uint64_t s_tail;          /* read by the worker */
static _Atomic uint64_t s_dropped;

static ale_engine     *s_engine;
static pthread_t       s_tid;
static _Atomic bool    s_run;
static bool            s_started;
static uint8_t         s_mode;           /* ALE_START_*, the worker's */

/* The operator's channel, put back when ALE releases the radio. */
static uint32_t        s_saved_hz;
static uint16_t        s_saved_mode;
static bool            s_saved_dv;
static bool            s_dv_by_ale;      /* RADEv2 switched on for a voice hand-off */
/* Listen mode's channel: a call tries the channel list (PC-ALE calls on
 * each in turn), and the station comes back here after it. */
static uint32_t        s_listen_hz;

/* ── events ── */
static pthread_mutex_t    s_ev_mutex = PTHREAD_MUTEX_INITIALIZER;
static ale_link_event_rec s_ev[ALE_LINK_EVENTS_MAX];
static uint64_t           s_ev_seq;      /* the newest */

static int64_t wall_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t) t.tv_sec * MS_PER_S + t.tv_nsec / NS_PER_MS;
}

static uint32_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t) ((uint64_t) t.tv_sec * MS_PER_S + (uint64_t) t.tv_nsec / NS_PER_MS);
}

static void push_event(const ale_link_event_rec *in)
{
    pthread_mutex_lock(&s_ev_mutex);
    ale_link_event_rec *r = &s_ev[s_ev_seq % ALE_LINK_EVENTS_MAX];
    *r = *in;
    r->seq = ++s_ev_seq;
    r->time_ms = wall_ms();
    pthread_mutex_unlock(&s_ev_mutex);
}

size_t ale_link_events_since(uint64_t after, ale_link_event_rec *out, size_t max, uint64_t *last)
{
    size_t n = 0;
    pthread_mutex_lock(&s_ev_mutex);
    uint64_t oldest = s_ev_seq > ALE_LINK_EVENTS_MAX ? s_ev_seq - ALE_LINK_EVENTS_MAX + 1 : 1;
    for (uint64_t seq = after + 1 < oldest ? oldest : after + 1; seq <= s_ev_seq && n < max; seq++)
        out[n++] = s_ev[seq % ALE_LINK_EVENTS_MAX];
    if (last)
        *last = s_ev_seq;
    pthread_mutex_unlock(&s_ev_mutex);
    return n;
}

/* ── status ── */
static pthread_mutex_t s_st_mutex = PTHREAD_MUTEX_INITIALIZER;
static ale_link_status s_st;

void ale_link_get_status(ale_link_status *out)
{
    pthread_mutex_lock(&s_st_mutex);
    *out = s_st;
    pthread_mutex_unlock(&s_st_mutex);
    out->samples_heard = atomic_load(&s_tail);
    out->samples_dropped = atomic_load(&s_dropped);
}

static void update_status(void)
{
    ale_link_state ls;
    ale_engine_link_get_state(s_engine, &ls);
    pthread_mutex_lock(&s_st_mutex);
    s_st.mode = s_mode;
    snprintf(s_st.state, sizeof(s_st.state), "%s", ls.state);
    snprintf(s_st.self, sizeof(s_st.self), "%s", s_radio->ale_self);
    snprintf(s_st.peer, sizeof(s_st.peer), "%s", ls.peer);
    s_st.hz = s_mode == ALE_START_MONITOR ? s_radio->profiles[s_radio->profile_active_idx].freq : ls.hz;
    s_st.sideband = s_mode == ALE_START_MONITOR ? s_radio->profiles[s_radio->profile_active_idx].mode : ls.mode;
    s_st.transmitting = ls.transmitting;
    s_st.link_seconds = ls.link_seconds;
    s_st.votes = ls.votes;
    s_st.channels = ls.channels;
    s_st.owner = s_radio->owner;
    pthread_mutex_unlock(&s_st_mutex);
}

/* ── the engine's callbacks (on the worker) ── */

static void on_word(const ale_word_event *ev, void *user)
{
    (void) user;
    ale_link_event_rec r;
    memset(&r, 0, sizeof(r));
    r.kind = ALE_LINK_EV_WORD;
    snprintf(r.peer, sizeof(r.peer), "%s", ev->text);
    r.combined = ev->combined;
    r.preamble = ev->preamble;
    r.unanimous = ev->unanimous;
    push_event(&r);
    if (ev->combined)
        HLOGI(LOG_C, "%s %s: combining %u words, %u/48 unanimous votes, tone %.1f dB over the others",
              ale_preamble_name(ev->preamble), ev->text, ev->combined, ev->unanimous, ev->tone_snr_db);
    else
        HLOGI(LOG_C, "%s %s: stock receiver, %u/48 unanimous votes, %u corrected",
              ale_preamble_name(ev->preamble), ev->text, ev->unanimous, ev->fec_errors);
}

static void on_link_event(const ale_link_event *ev, void *user)
{
    (void) user;
    if (ev->kind == ALE_EV_STATUS)
    {
        HLOGD(LOG_C, "%s", ev->text);
        return;
    }
    ale_link_event_rec r;
    memset(&r, 0, sizeof(r));
    r.kind = ev->kind;
    snprintf(r.peer, sizeof(r.peer), "%s", ev->peer);
    snprintf(r.text, sizeof(r.text), "%s", ev->text);
    r.hz = ev->hz;
    push_event(&r);
    switch (ev->kind)
    {
    case ALE_EV_STATE:      HLOGI(LOG_C, "state %s", ev->text); break;
    case ALE_EV_CALL:       HLOGI(LOG_C, "call from %s%s", ev->peer,
                                  s_radio->ale_auto_respond ? "" : " (ale.accept answers it)"); break;
    case ALE_EV_LINKED:     HLOGI(LOG_C, "linked with %s", ev->peer); break;
    case ALE_EV_TERMINATED: HLOGI(LOG_C, "link with %s ended: %s", ev->peer, ev->text); break;
    case ALE_EV_AMD:        HLOGI(LOG_C, "AMD from %s: %s", ev->peer, ev->text); break;
    case ALE_EV_CHANNEL:    HLOGD(LOG_C, "tuned to %u Hz", ev->hz); break;
    default: break;
    }
}

/* ── the radio, as the engine drives it ── */

/* Retunes timed (the scan dwell has to be longer than them). */
static uint32_t s_retunes, s_retune_max_ms, s_retune_sum_ms;

static bool op_retune(uint32_t hz, uint8_t mode, void *user)
{
    (void) user;
    uint32_t t0 = mono_ms();
    bool ok = radio_backend_retune(s_radio, hz, mode == ALE_MODE_LSB ? MODE_LSB : MODE_USB);
    uint32_t ms = mono_ms() - t0;
    s_retunes++;
    s_retune_sum_ms += ms;
    if (ms > s_retune_max_ms)
        s_retune_max_ms = ms;
    HLOGD(LOG_C, "tuned to %u Hz in %u ms%s", hz, ms, ok ? "" : " (failed)");
    return ok;
}

static bool op_key(bool on, void *user)
{
    (void) user;
    if (!on)
    {
        modem_audio_tx_end();
        return true;
    }
    if (!modem_audio_tx_begin(ALE_LINK_PTT_ID))
    {
        HLOGW(LOG_C, "cannot key: another client holds PTT");
        return false;
    }
    return true;
}

static void op_tx_audio(const int16_t *pcm, size_t n, void *user)
{
    (void) user;
    modem_audio_tx_feed(pcm, n);
}

static uint64_t op_tx_played(void *user)
{
    (void) user;
    return modem_audio_tx_played();
}

/* ── taking and releasing the radio (on the worker) ── */

static int take_radio(uint8_t mode)
{
    if (!ale_address_valid(s_radio->ale_self))
    {
        HLOGE(LOG_C, "ale:self_address \"%s\" is not an ALE address (1-15 of A-Z 0-9 @ ?)",
              s_radio->ale_self);
        return ALE_LINK_EINVAL;
    }
    radio_profile *p = &s_radio->profiles[s_radio->profile_active_idx];
    s_saved_hz = p->freq;
    s_saved_mode = p->mode;
    s_saved_dv = radio_backend_get_digital_voice(s_radio, s_radio->profile_active_idx);
    if (s_saved_dv)
        radio_backend_set_digital_voice(s_radio, false, s_radio->profile_active_idx);

    /* ALE signals in USB (channels in ale:channel_file may say otherwise) */
    uint8_t side = ALE_MODE_USB;
    ale_link_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.self, sizeof(cfg.self), "%s", s_radio->ale_self);
    cfg.channel_file = s_radio->ale_channel_file;
    cfg.lqa_file = s_radio->ale_lqa_file;
    cfg.scan_dwell_ms = s_radio->ale_scan_dwell_ms;
    cfg.manual_accept = !s_radio->ale_auto_respond;
    cfg.accept_timeout_ms = s_radio->ale_accept_timeout_s * MS_PER_S;
    cfg.auto_sounding = s_radio->ale_auto_sounding;
    cfg.tx_tail_ms = s_radio->ale_tx_tail_ms;
    cfg.start_hz = p->freq;
    cfg.start_mode = side;
    ale_radio_ops ops = { op_retune, op_key, op_tx_audio, op_tx_played, NULL };
    s_radio->owner = RADIO_OWNER_ALE_FULL;
    radio_backend_retune(s_radio, p->freq, side == ALE_MODE_LSB ? MODE_LSB : MODE_USB);
    ale_engine_set_stock(s_engine, 1);            /* the link needs every word */
    if (ale_engine_link_start(s_engine, &cfg, &ops, on_link_event, NULL) != 0)
    {
        s_radio->owner = RADIO_OWNER_NONE;
        return ALE_LINK_EINVAL;
    }
    s_listen_hz = p->freq;
    if (mode == ALE_START_SCAN && !ale_engine_scan(s_engine))
    {
        HLOGW(LOG_C, "no channels to scan in %s; listening on %u Hz", s_radio->ale_channel_file, p->freq);
        mode = ALE_START_LISTEN;
    }
    s_mode = mode;
    HLOGI(LOG_C, "%s as %s; the radio is ALE's until ale.stop",
          mode == ALE_START_SCAN ? "scanning" : "listening", s_radio->ale_self);
    return ALE_LINK_OK;
}

static void release_radio(const char *why)
{
    if (!ale_engine_link_running(s_engine))
        return;
    ale_engine_link_stop(s_engine);
    if (s_retunes)
        HLOGI(LOG_C, "%u retunes, %u ms on average, %u ms at most", s_retunes,
              s_retune_sum_ms / s_retunes, s_retune_max_ms);
    s_retunes = s_retune_sum_ms = s_retune_max_ms = 0;
    if (modem_audio_tx_keyed_by(ALE_LINK_PTT_ID))
        modem_audio_tx_unkey("ALE stopped");
    if (s_dv_by_ale)
    {
        radio_backend_set_digital_voice(s_radio, false, s_radio->profile_active_idx);
        s_dv_by_ale = false;
    }
    radio_backend_retune(s_radio, s_saved_hz, s_saved_mode);
    s_radio->owner = RADIO_OWNER_NONE;
    if (s_saved_dv)
        radio_backend_set_digital_voice(s_radio, true, s_radio->profile_active_idx);
    ale_engine_set_stock(s_engine, s_radio->ale_stock_receiver);
    s_mode = ALE_START_MONITOR;
    HLOGI(LOG_C, "%s: the radio is back on %u Hz %s", why, s_saved_hz,
          s_saved_mode == MODE_LSB ? "LSB" : s_saved_mode == MODE_USB ? "USB" : "(its mode)");
}

/* PC-ALE scans after a sounding whatever it did before, and a call may end
 * on another channel of the list: a station that only listens goes back to
 * listening, on its own channel. */
static void keep_listening(void)
{
    if (s_mode != ALE_START_LISTEN)
        return;
    ale_link_state ls;
    ale_engine_link_get_state(s_engine, &ls);
    if (!strcmp(ls.state, "SCANNING"))
        ale_engine_listen(s_engine);
    else if (!strcmp(ls.state, "IDLE") && ls.hz != s_listen_hz
             && ale_engine_tune(s_engine, s_listen_hz, ALE_MODE_USB))
        HLOGI(LOG_C, "back on %u Hz", s_listen_hz);
}

/* A link up and handed to voice: others may key on the channel; back to
 * ALE alone when it ends. */
static void follow_handoff(void)
{
    if (s_mode == ALE_START_MONITOR || s_radio->ale_handoff != ALE_HANDOFF_VOICE)
        return;
    ale_link_state ls;
    ale_engine_link_get_state(s_engine, &ls);
    bool linked = !strcmp(ls.state, "LINKED");
    if (linked && s_radio->owner == RADIO_OWNER_ALE_FULL && !ls.transmitting)
    {
        s_radio->owner = RADIO_OWNER_ALE_CHANNEL;
        if (s_radio->ale_voice_mode == ALE_VOICE_RADE
            && radio_backend_set_digital_voice(s_radio, true, s_radio->profile_active_idx))
            s_dv_by_ale = true;
        HLOGI(LOG_C, "voice on %u Hz with %s (%s): PTT is free", ls.hz, ls.peer,
              s_dv_by_ale ? "RADEv2" : "SSB");
    }
    else if (!linked && s_radio->owner == RADIO_OWNER_ALE_CHANNEL)
    {
        if (s_dv_by_ale)
        {
            radio_backend_set_digital_voice(s_radio, false, s_radio->profile_active_idx);
            s_dv_by_ale = false;
        }
        s_radio->owner = RADIO_OWNER_ALE_FULL;
    }
}

/* ── the operator's actions: a mailbox to the worker ── */

enum { CMD_NONE, CMD_MODE, CMD_CALL, CMD_ACCEPT, CMD_REJECT, CMD_TERMINATE, CMD_SOUND };

static pthread_mutex_t s_post_mutex = PTHREAD_MUTEX_INITIALIZER;   /* one caller at a time */
static pthread_mutex_t s_cmd_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cmd_cond = PTHREAD_COND_INITIALIZER;
static struct
{
    int     op;
    uint8_t mode;
    char    to[ALE_ADDRESS_MAX + 1];
    char    amd[ALE_AMD_MAX + 1];
    int     result;
    bool    done;
} s_cmd;

static int run_command(void)
{
    if (s_cmd.op == CMD_MODE)
    {
        if (s_cmd.mode == ALE_START_MONITOR)
        {
            release_radio("ale.stop");
            return ALE_LINK_OK;
        }
        if (!ale_engine_link_running(s_engine))
            return take_radio(s_cmd.mode);
        if (s_cmd.mode == ALE_START_SCAN)
        {
            if (!ale_engine_scan(s_engine))
                return ALE_LINK_ENOCHANNELS;
        }
        else if (!ale_engine_listen(s_engine))
            return ALE_LINK_EBUSY;
        s_mode = s_cmd.mode;
        return ALE_LINK_OK;
    }
    if (!ale_engine_link_running(s_engine))
        return ALE_LINK_ENOTRUNNING;
    bool ok = false;
    switch (s_cmd.op)
    {
    case CMD_CALL:      ok = ale_engine_call(s_engine, s_cmd.to, s_cmd.amd[0] ? s_cmd.amd : NULL); break;
    case CMD_ACCEPT:    ok = ale_engine_accept(s_engine); break;
    case CMD_REJECT:    ok = ale_engine_reject(s_engine); break;
    case CMD_TERMINATE: ok = ale_engine_terminate(s_engine); break;
    case CMD_SOUND:     ok = ale_engine_sound(s_engine); break;
    default: break;
    }
    if (ok && s_cmd.op == CMD_CALL)
        HLOGI(LOG_C, "calling %s%s%s", s_cmd.to, s_cmd.amd[0] ? " with AMD: " : "", s_cmd.amd);
    return ok ? ALE_LINK_OK : ALE_LINK_EBUSY;
}

static void serve_command(void)
{
    pthread_mutex_lock(&s_cmd_mutex);
    if (s_cmd.op != CMD_NONE && !s_cmd.done)
    {
        s_cmd.result = run_command();
        s_cmd.done = true;
        pthread_cond_broadcast(&s_cmd_cond);
    }
    pthread_mutex_unlock(&s_cmd_mutex);
}

static int post(int op, uint8_t mode, const char *to, const char *amd)
{
    if (!s_started)
        return ALE_LINK_ENOTRUNNING;
    pthread_mutex_lock(&s_post_mutex);
    pthread_mutex_lock(&s_cmd_mutex);
    s_cmd.op = op;
    s_cmd.mode = mode;
    snprintf(s_cmd.to, sizeof(s_cmd.to), "%s", to ? to : "");
    snprintf(s_cmd.amd, sizeof(s_cmd.amd), "%s", amd ? amd : "");
    s_cmd.done = false;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ALE_CMD_TIMEOUT_MS / MS_PER_S;
    int rc = 0;
    while (!s_cmd.done && rc == 0)
        rc = pthread_cond_timedwait(&s_cmd_cond, &s_cmd_mutex, &until);
    int result = s_cmd.done ? s_cmd.result : ALE_LINK_ETIMEOUT;
    s_cmd.op = CMD_NONE;
    pthread_mutex_unlock(&s_cmd_mutex);
    pthread_mutex_unlock(&s_post_mutex);
    return result;
}

int ale_link_set_mode(uint8_t mode)
{
    if (mode > ALE_START_SCAN)
        return ALE_LINK_EINVAL;
    return post(CMD_MODE, mode, NULL, NULL);
}

int ale_link_call(const char *to, const char *amd)
{
    if (!ale_address_valid(to) || (amd && !ale_amd_valid(amd)))
        return ALE_LINK_EINVAL;
    return post(CMD_CALL, 0, to, amd);
}

int ale_link_accept(void)    { return post(CMD_ACCEPT, 0, NULL, NULL); }
int ale_link_reject(void)    { return post(CMD_REJECT, 0, NULL, NULL); }
int ale_link_terminate(void) { return post(CMD_TERMINATE, 0, NULL, NULL); }
int ale_link_sound(void)     { return post(CMD_SOUND, 0, NULL, NULL); }

/* ── RX audio and the worker ── */

static void rx_sink(const int16_t *pcm, size_t n, void *user)
{
    (void) user;
    uint64_t head = atomic_load_explicit(&s_head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&s_tail, memory_order_acquire);
    uint64_t room = ALE_RING_SAMPLES - (head - tail);
    if (n > room)
    {
        atomic_fetch_add_explicit(&s_dropped, n - room, memory_order_relaxed);
        n = (size_t) room;
    }
    for (size_t i = 0; i < n; i++)
        s_ring[(head + i) % ALE_RING_SAMPLES] = pcm[i];
    atomic_store_explicit(&s_head, head + n, memory_order_release);
}

static void *worker(void *arg)
{
    (void) arg;
    int16_t block[ALE_FEED_BLOCK];
    uint64_t dropped_logged = 0;
    uint32_t last_drop_log = 0;
    const struct timespec period = { 0, ALE_WORKER_PERIOD_MS * NS_PER_MS };

    if (s_radio->ale_start != ALE_START_MONITOR)
        take_radio(s_radio->ale_start);

    while (atomic_load(&s_run))
    {
        uint64_t head = atomic_load_explicit(&s_head, memory_order_acquire);
        uint64_t tail = atomic_load_explicit(&s_tail, memory_order_relaxed);
        while (tail < head)
        {
            size_t n = head - tail > ALE_FEED_BLOCK ? ALE_FEED_BLOCK : (size_t) (head - tail);
            for (size_t i = 0; i < n; i++)
                block[i] = s_ring[(tail + i) % ALE_RING_SAMPLES];
            tail += n;
            atomic_store_explicit(&s_tail, tail, memory_order_release);
            ale_engine_feed(s_engine, block, n);
        }

        uint32_t now = mono_ms();
        ale_engine_tick(s_engine, now);
        if (modem_audio_tx_keyed_by(ALE_LINK_PTT_ID))
            modem_audio_tx_poll();      /* unkeys once an ended over has drained */

        if (atomic_exchange(&s_radio->ale_stop_asked, false))
            release_radio("PTT off from another client");
        follow_handoff();
        keep_listening();
        serve_command();
        update_status();

        uint64_t dropped = atomic_load_explicit(&s_dropped, memory_order_relaxed);
        if (dropped != dropped_logged && now - last_drop_log >= ALE_DROP_LOG_MS)
        {
            HLOGW(LOG_C, "the receiver fell behind: %llu samples of RX audio dropped so far",
                  (unsigned long long) dropped);
            dropped_logged = dropped;
            last_drop_log = now;
        }
        nanosleep(&period, NULL);
    }
    release_radio("shutting down");
    return NULL;
}

bool ale_link_start(radio *radio_h, const cpu_set_t *cpus)
{
    if (s_started)
        return true;

    s_radio = radio_h;
    s_engine = ale_engine_create(radio_h->ale_combine, radio_h->ale_combine_count, on_word, NULL);
    if (!s_engine)
    {
        HLOGE(LOG_C, "cannot create the ALE receiver");
        return false;
    }
    if (!radio_h->ale_stock_receiver && !radio_h->ale_combine_count)
        HLOGW(LOG_C, "ale:stock_receiver is off and ale:combine is empty: nothing listens");
    ale_engine_set_stock(s_engine, radio_h->ale_stock_receiver);
    s_mode = ALE_START_MONITOR;
    radio_h->owner = RADIO_OWNER_NONE;
    atomic_store(&radio_h->ale_stop_asked, false);

    atomic_store(&s_head, 0);
    atomic_store(&s_tail, 0);
    atomic_store(&s_dropped, 0);
    if (!modem_audio_init(radio_h) || !modem_audio_add_rx_sink(rx_sink, NULL))
    {
        HLOGE(LOG_C, "cannot attach to the RX audio");
        ale_engine_destroy(s_engine);
        s_engine = NULL;
        return false;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (cpus)
        pthread_attr_setaffinity_np(&attr, sizeof(*cpus), cpus);
    atomic_store(&s_run, true);
    s_started = true;
    int rc = pthread_create(&s_tid, &attr, worker, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
    {
        HLOGE(LOG_C, "cannot start the ALE receiver's thread");
        s_started = false;
        atomic_store(&s_run, false);
        modem_audio_remove_rx_sink(rx_sink, NULL);
        ale_engine_destroy(s_engine);
        s_engine = NULL;
        return false;
    }
    pthread_setname_np(s_tid, "ale");

    char ks[ALE_COMBINE_MAX * 4 + 1] = "";
    size_t len = 0;
    for (unsigned i = 0; i < radio_h->ale_combine_count; i++)
        len += (size_t) snprintf(ks + len, sizeof(ks) - len, "%s%u", i ? "," : "",
                                 radio_h->ale_combine[i]);
    HLOGI(LOG_C, "receivers on: stock %s, combining K = %s; starting in %s",
          radio_h->ale_stock_receiver ? "on" : "off", len ? ks : "none",
          radio_h->ale_start == ALE_START_SCAN ? "scan" :
          radio_h->ale_start == ALE_START_LISTEN ? "listen" : "monitor");
    return true;
}

void ale_link_stop(void)
{
    if (!s_started)
        return;
    modem_audio_remove_rx_sink(rx_sink, NULL);
    atomic_store(&s_run, false);
    pthread_join(s_tid, NULL);
    HLOGI(LOG_C, "stopped after %.0f s of RX audio (%llu samples dropped)",
          (double) ale_engine_position(s_engine) / ALE_ENGINE_RATE,
          (unsigned long long) atomic_load(&s_dropped));
    ale_engine_destroy(s_engine);
    s_engine = NULL;
    s_started = false;
}

bool ale_link_running(void)
{
    return s_started;
}
