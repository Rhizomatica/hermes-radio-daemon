/* sBitx DRM (Digital Radio Mondiale) through Dream subprocesses
 *
 * Copyright (C) 2024-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "async_file.h"
#include "dream_proc.h"
#include "sbitx_drm.h"

#include "radiod_log.h"

#define LOG_C "drm"

/* 4 s of each stream: Dream's frames are 400 ms, its interleaver 2.4 s */
#define DRM_RING (SBITX_DRM_RATE * 2 * 4)

/* Dream is started and stopped by a thread of its own: starting one is a
 * fork, a directory and a Dream.ini written, stopping one waits for it to
 * exit (up to 2 s), and none of that may happen on the DSP thread, whose
 * every missed period is silence on the air. The DSP side asks (the _start
 * and _stop calls), and finds the running Dream through rx and tx: only the
 * control thread sets them, only the DSP side clears them. */
static _Atomic(dream_proc *) rx, tx;
static double tx_phase;

typedef struct {
    bool want;                  /* a start asked for, not yet taken */
    unsigned gen;               /* bumped by every start and stop asked for */
    char path[256], dir[300];
    char ini[1024];             /* the transmitter's */
} drm_req;

typedef struct {
    dream_proc *p;
    bool is_rx;
} drm_stop;

static pthread_mutex_t ctl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ctl_more = PTHREAD_COND_INITIALIZER;
static pthread_once_t ctl_once = PTHREAD_ONCE_INIT;
static drm_req rx_req, tx_req;
static drm_stop to_stop[8];
static int n_to_stop;
/* a start asked for or under way: what lets a _stop with nothing to do
 * return without the lock (tx_stop runs on every receive block) */
static atomic_bool rx_pending, tx_pending;

/* Samples a ring had no room for, into Dream or out of it, logged at most
 * every 10 s: a Dream that falls behind gets a broken stream, and a
 * receiver fed one does not lock. */
typedef struct {
    const char *what;
    uint64_t in, out;
    time_t   when;
} drop_log;

static drop_log rx_drops = { .what = "receiver" }, tx_drops = { .what = "transmitter" };

static void log_drops(dream_proc *p, drop_log *d)
{
    uint64_t in, out;
    time_t now = time(NULL);

    if (!p || now - d->when < 10)
        return;
    d->when = now;
    dream_proc_dropped(p, &in, &out);
    if (in != d->in || out != d->out)
        HLOGW(LOG_C, "Dream's %s: %llu samples dropped going in, %llu coming out, in %d s",
              d->what, (unsigned long long) (in - d->in), (unsigned long long) (out - d->out), 10);
    d->in = in;
    d->out = out;
}

/* The receiver's status report: Dream rewrites it about once a second, on
 * the RAM disk rather than the SD card; one per radiod, by its pid. */
static atomic_bool status_supported;   /* set by init, read by the control thread */
static char status_path[64];
static atomic_bool status_live;

/* Dream hands out its audio and its I/Q a 400 ms frame at a time, the DSP
 * takes it 10 ms at a time: half a second is queued before any goes out,
 * and again whenever the queue runs dry, so the frames do not leave gaps. */
#define DRM_PRIME (SBITX_DRM_RATE / 2)
static bool rx_primed, tx_primed;

/* Test hook: with RADIOD_DRM_DUMP set to a path prefix, what the receiver
 * hands Dream (<prefix>.iq, s16le I/Q at 48 kHz) and what it gets back
 * (<prefix>.audio, s16le mono at 48 kHz) are written there, and the
 * transmitter's audio in (<prefix>.txaudio) and I/Q out, on the dial
 * (<prefix>.txiq), to replay Dream on them offline. The DSP thread only
 * queues them; writer threads do the files (async_file). */
static async_writer *dump_iq, *dump_audio, *dump_txaudio, *dump_txiq;

static void dump_on(async_writer *w, const char *ext)
{
    const char *p = getenv("RADIOD_DRM_DUMP");
    char path[512];

    if (!w || !p || !p[0])
        return;
    snprintf(path, sizeof(path), "%s.%s", p, ext);
    async_writer_on(w, path);
}

static int16_t to_s16(float v)
{
    v *= 32767.0f;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    return (int16_t) lrintf(v);
}

/* ---- receive ---- */

/* ---- the control thread ---- */

static dream_proc *start_rx(const drm_req *r)
{
    /* "I/Q input positive, 0 Hz IF": the IQ is the dial's; audio out mono
     * (both channels mixed, in a Dream that does). An empty Dream.ini each
     * time: what a previous run saved does not carry over. */
    const char *args[12] = { "dream", "-n", "-I", "-", "-c", "6", "-O", "-mono" };
    int n = 8;
    dream_proc *p;

    if (atomic_load(&status_supported)) {
        args[n++] = "--status-file";
        args[n++] = status_path;
    }
    args[n] = NULL;
    dump_on(dump_iq, "iq");
    dump_on(dump_audio, "audio");
    p = dream_proc_start(r->path, r->dir, "", args, DRM_RING);
    if (!p)
        HLOGE(LOG_C, "cannot start Dream's receiver (%s)", r->path);
    atomic_store(&status_live, p != NULL && atomic_load(&status_supported));
    return p;
}

static dream_proc *start_tx(const drm_req *r)
{
    /* audio in mono, I/Q out on the two channels */
    static const char *const args[] = {
        "dream", "-n", "-t", "-I", "-mono", "-O", "-", NULL
    };
    dream_proc *p;

    dump_on(dump_txaudio, "txaudio");
    dump_on(dump_txiq, "txiq");
    p = dream_proc_start(r->path, r->dir, r->ini, args, DRM_RING);
    if (!p)
        HLOGE(LOG_C, "cannot start Dream's transmitter (%s)", r->path);
    return p;
}

static void stop_one(const drm_stop *s)
{
    dream_proc_stop(s->p);
    if (s->is_rx) {
        if (atomic_load(&status_supported))
            unlink(status_path);
        async_writer_off(dump_iq);
        async_writer_off(dump_audio);
    } else {
        async_writer_off(dump_txaudio);
        async_writer_off(dump_txiq);
    }
}

static void *ctl_main(void *arg)
{
    (void) arg;
    pthread_mutex_lock(&ctl_lock);
    for (;;) {
        while (!rx_req.want && !tx_req.want && !n_to_stop)
            pthread_cond_wait(&ctl_more, &ctl_lock);

        /* stops first: a Dream stopped and started again goes in that order */
        if (n_to_stop) {
            drm_stop st = to_stop[--n_to_stop];

            pthread_mutex_unlock(&ctl_lock);
            stop_one(&st);
            pthread_mutex_lock(&ctl_lock);
            continue;
        }

        bool is_rx = rx_req.want;
        drm_req *q = is_rx ? &rx_req : &tx_req;
        drm_req r = *q;
        _Atomic(dream_proc *) *slot = is_rx ? &rx : &tx;

        q->want = false;
        pthread_mutex_unlock(&ctl_lock);
        dream_proc *p = is_rx ? start_rx(&r) : start_tx(&r);
        pthread_mutex_lock(&ctl_lock);

        /* still wanted (no stop or newer start meanwhile): it is the one */
        if (p && q->gen == r.gen && !atomic_load(slot)) {
            atomic_store(slot, p);
        } else if (p) {
            drm_stop st = { p, is_rx };

            pthread_mutex_unlock(&ctl_lock);
            stop_one(&st);
            pthread_mutex_lock(&ctl_lock);
        }
        if (!q->want)
            atomic_store(is_rx ? &rx_pending : &tx_pending, false);
    }
    return NULL;
}

static void ctl_start(void)
{
    pthread_t t;

    if (pthread_create(&t, NULL, ctl_main, NULL) == 0)
        pthread_detach(t);
    else
        HLOGE(LOG_C, "cannot start the DRM control thread");
}

/* from the DSP side: ask, signal, never wait */
static void ctl_ask_start(drm_req *q, atomic_bool *pending, const char *path,
                          const char *dir, const char *ini)
{
    pthread_once(&ctl_once, ctl_start);
    pthread_mutex_lock(&ctl_lock);
    if (!q->want) {
        q->want = true;
        q->gen++;
        snprintf(q->path, sizeof(q->path), "%s", path);
        snprintf(q->dir, sizeof(q->dir), "%s", dir);
        snprintf(q->ini, sizeof(q->ini), "%s", ini ? ini : "");
        atomic_store(pending, true);
        pthread_cond_signal(&ctl_more);
    }
    pthread_mutex_unlock(&ctl_lock);
}

static void ctl_ask_stop(drm_req *q, atomic_bool *pending, _Atomic(dream_proc *) *slot, bool is_rx)
{
    dream_proc *p;

    if (!atomic_load(slot) && !atomic_load(pending))
        return;                         /* nothing running, nothing asked */
    p = atomic_exchange(slot, NULL);
    pthread_mutex_lock(&ctl_lock);
    q->want = false;
    q->gen++;
    if (p && n_to_stop < (int) (sizeof(to_stop) / sizeof(to_stop[0]))) {
        to_stop[n_to_stop++] = (drm_stop) { p, is_rx };
        pthread_cond_signal(&ctl_more);
    } else if (p) {
        HLOGE(LOG_C, "too many Dreams to stop: one left running");
    }
    pthread_mutex_unlock(&ctl_lock);
}

/* ---- receive ---- */

void sbitx_drm_init(const char *dream_path)
{
    const char *dump = getenv("RADIOD_DRM_DUMP");

    if (!status_path[0])        /* once: the control thread reads it */
        snprintf(status_path, sizeof(status_path), "/dev/shm/radiod-drm-%d.json", (int) getpid());
    atomic_store(&status_supported, dream_proc_has_option(dream_path, "--status-file"));
    if (dump && dump[0] && !dump_iq) {
        /* 4 s of each, as the rings to Dream */
        dump_iq = async_writer_new("DRM receiver I/Q dump", DRM_RING * 2, NULL, NULL);
        dump_audio = async_writer_new("DRM receiver audio dump", DRM_RING, NULL, NULL);
        dump_txaudio = async_writer_new("DRM transmitter audio dump", DRM_RING, NULL, NULL);
        dump_txiq = async_writer_new("DRM transmitter I/Q dump", DRM_RING * 2, NULL, NULL);
        HLOGI(LOG_C, "dumping DRM's I/Q and audio to %s.*", dump);
    }
    pthread_once(&ctl_once, ctl_start);
}

bool sbitx_drm_rx_start(const char *dream_path, const char *dir)
{
    if (atomic_load(&rx))
        return true;
    rx_primed = false;
    rx_drops.in = rx_drops.out = 0;
    rx_drops.when = time(NULL);
    ctl_ask_start(&rx_req, &rx_pending, dream_path, dir, NULL);
    return true;
}

void sbitx_drm_rx_stop(void)
{
    atomic_store(&status_live, false);
    ctl_ask_stop(&rx_req, &rx_pending, &rx, true);
}

/* A strict JSON check, enough that a report cannot break the state frame
 * it is copied into: values, nesting (to a depth of 8), strings with their
 * escapes and no raw control characters, numbers. */
static const char *json_ws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

static const char *json_value(const char *p, const char *e, int depth);

static const char *json_string(const char *p, const char *e)
{
    if (p >= e || *p != '"')
        return NULL;
    for (p++; p < e; p++) {
        unsigned char c = (unsigned char) *p;

        if (c == '"')
            return p + 1;
        if (c < 0x20)
            return NULL;
        if (c == '\\') {
            if (++p >= e)
                return NULL;
            if (*p == 'u') {
                for (int k = 0; k < 4; k++)
                    if (++p >= e || !strchr("0123456789abcdefABCDEF", *p))
                        return NULL;
            } else if (!strchr("\"\\/bfnrt", *p)) {
                return NULL;
            }
        }
    }
    return NULL;
}

static const char *json_number(const char *p, const char *e)
{
    const char *d;

    if (p < e && *p == '-')
        p++;
    for (d = p; p < e && *p >= '0' && *p <= '9'; p++) {}
    if (p == d)
        return NULL;
    if (p < e && *p == '.') {
        for (d = ++p; p < e && *p >= '0' && *p <= '9'; p++) {}
        if (p == d)
            return NULL;
    }
    if (p < e && (*p == 'e' || *p == 'E')) {
        if (++p < e && (*p == '+' || *p == '-'))
            p++;
        for (d = p; p < e && *p >= '0' && *p <= '9'; p++) {}
        if (p == d)
            return NULL;
    }
    return p;
}

static const char *json_list(const char *p, const char *e, int depth, char close, bool keys)
{
    p = json_ws(p + 1, e);
    if (p < e && *p == close)
        return p + 1;
    for (;;) {
        if (keys) {
            if (!(p = json_string(p, e)))
                return NULL;
            p = json_ws(p, e);
            if (p >= e || *p != ':')
                return NULL;
            p = json_ws(p + 1, e);
        }
        if (!(p = json_value(p, e, depth + 1)))
            return NULL;
        p = json_ws(p, e);
        if (p < e && *p == ',') {
            p = json_ws(p + 1, e);
            continue;
        }
        return p < e && *p == close ? p + 1 : NULL;
    }
}

static const char *json_value(const char *p, const char *e, int depth)
{
    if (depth > 8 || p >= e)
        return NULL;
    switch (*p) {
    case '{': return json_list(p, e, depth, '}', true);
    case '[': return json_list(p, e, depth, ']', false);
    case '"': return json_string(p, e);
    case 't': return e - p >= 4 && !strncmp(p, "true", 4) ? p + 4 : NULL;
    case 'f': return e - p >= 5 && !strncmp(p, "false", 5) ? p + 5 : NULL;
    case 'n': return e - p >= 4 && !strncmp(p, "null", 4) ? p + 4 : NULL;
    default:  return json_number(p, e);
    }
}

bool sbitx_drm_json_object(const char *s, size_t n)
{
    const char *e = s + n, *p = json_ws(s, e);

    if (p >= e || *p != '{' || !(p = json_value(p, e, 0)))
        return false;
    return json_ws(p, e) == e;
}

size_t sbitx_drm_rx_status(char *out, size_t len)
{
    struct stat st;
    FILE *f;
    size_t n;

    if (len < 2 || !atomic_load(&status_live) || stat(status_path, &st) < 0 ||
        time(NULL) - st.st_mtime > 5 || st.st_size <= 0 || (size_t) st.st_size >= len)
        return 0;
    if (!(f = fopen(status_path, "r")))
        return 0;
    n = fread(out, 1, len - 1, f);
    fclose(f);
    out[n] = '\0';
    return sbitx_drm_json_object(out, n) ? n : 0;
}

/* a start under way counts as running: the DSP's retry must not cancel it */
bool sbitx_drm_rx_running(void)
{
    dream_proc *p = atomic_load(&rx);

    return p ? dream_proc_running(p) : atomic_load(&rx_pending);
}

void sbitx_drm_rx_iq(const float *i, const float *q, size_t n)
{
    int16_t s[2048];
    dream_proc *p = atomic_load(&rx);

    log_drops(p, &rx_drops);

    while (p && n) {
        size_t k = n < sizeof(s) / sizeof(s[0]) / 2 ? n : sizeof(s) / sizeof(s[0]) / 2;

        for (size_t j = 0; j < k; j++) {
            s[2 * j] = to_s16(i[j]);
            s[2 * j + 1] = to_s16(q[j]);
        }
        dream_proc_write(p, s, 2 * k);
        async_writer_write(dump_iq, s, 2 * k * sizeof(int16_t));
        i += k;
        q += k;
        n -= k;
    }
}

size_t sbitx_drm_rx_audio(float *out, size_t n)
{
    int16_t s[2048];
    size_t got = 0;
    dream_proc *p = atomic_load(&rx);

    if (!p)
        return 0;
    if (!rx_primed) {
        if (dream_proc_readable(p) < DRM_PRIME)
            return 0;
        rx_primed = true;
    }
    while (got < n) {
        size_t want = n - got < sizeof(s) / sizeof(s[0]) ? n - got : sizeof(s) / sizeof(s[0]);
        size_t k = dream_proc_read(p, s, want);

        if (!k) {
            rx_primed = false;          /* ran dry: queue again */
            break;
        }
        async_writer_write(dump_audio, s, k * sizeof(int16_t));
        for (size_t j = 0; j < k; j++)
            out[got + j] = (float) s[j] / 32768.0f;
        got += k;
    }
    return got;
}

/* ---- transmit ---- */

int sbitx_drm_tx_ini(const sbitx_drm_tx_config *c, char *buf, size_t len)
{
    char robustness = c->robustness >= 'A' && c->robustness <= 'D' ? c->robustness : 'B';
    int spectrum = c->spectrum >= 0 && c->spectrum <= 5 ? c->spectrum : 3;
    int protection = c->protection >= 0 && c->protection <= 3 ? c->protection : 1;
    const char *codec = c->codec[0] ? c->codec : "xHE-AAC";
    char aac[48] = "";
    int n;

    /* AAC's own settings: HE-AAC is Dream's AAC with SBR, on a 12 kHz core
     * (24 kHz out), as DRM uses it on narrow channels */
    if (!strcmp(codec, "HE-AAC") || !strcmp(codec, "AAC")) {
        bool he = codec[0] == 'H';

        snprintf(aac, sizeof(aac), "sbr=%d\naudiosamplerate=%d\n", he, he ? 12000 : 24000);
        codec = "AAC";
    }

    /* The SDC in 4-QAM, as robust as it gets: it is what a receiver needs
     * first. In the 4.5 and 5 kHz channels a 4-QAM SDC has 104-120 bits,
     * and Dream drops what does not fit: the label, and xHE-AAC's audio
     * entity (88 bits) with it, so a receiver finds no audio. There it is
     * 16-QAM (224-296 bits), as the MSC is anyway. */
    const char *sdc = spectrum <= 1 ? "CS_2_SM" : "CS_1_SM";

    n = snprintf(buf, len,
                 "[Transmitter]\n"
                 "robustness=RM_ROBUSTNESS_MODE_%c\n"
                 "spectocc=SO_%d\n"
                 "msc=%s\n"
                 "sdc=%s\n"
                 "protlevel=%d\n"
                 "interleaver=SI_LONG\n"
                 "ifformat=OF_IQ_POS\n"
                 "iffreq=%d\n"
                 "hqiq=1\n"
                 "\n"
                 "[Transmitter Service 1]\n"
                 "codec=%s\n"
                 "%s"
                 "audiomode=mono\n"
                 "label=%s\n"
                 "id=1\n",
                 robustness, spectrum, c->msc_qam == 64 ? "CS_3_SM" : "CS_2_SM", sdc,
                 protection, (int) SBITX_DRM_TX_IF,
                 codec, aac,
                 c->label[0] ? c->label : "HERMES");
    return n > 0 && (size_t) n < len ? n : -1;
}

bool sbitx_drm_tx_start(const char *dream_path, const char *dir, const sbitx_drm_tx_config *cfg)
{
    char ini[1024];

    if (atomic_load(&tx))
        return true;
    if (sbitx_drm_tx_ini(cfg, ini, sizeof(ini)) < 0)
        return false;
    tx_drops.in = tx_drops.out = 0;
    tx_drops.when = time(NULL);
    tx_phase = 0.0;
    tx_primed = false;
    ctl_ask_start(&tx_req, &tx_pending, dream_path, dir, ini);
    return true;
}

void sbitx_drm_tx_stop(void)
{
    ctl_ask_stop(&tx_req, &tx_pending, &tx, false);
}

bool sbitx_drm_tx_running(void)
{
    dream_proc *p = atomic_load(&tx);

    return p ? dream_proc_running(p) : atomic_load(&tx_pending);
}

void sbitx_drm_tx_audio(const float *in, size_t n)
{
    int16_t s[2048];
    dream_proc *p = atomic_load(&tx);

    log_drops(p, &tx_drops);

    while (p && n) {
        size_t k = n < sizeof(s) / sizeof(s[0]) ? n : sizeof(s) / sizeof(s[0]);

        for (size_t j = 0; j < k; j++)
            s[j] = to_s16(in[j]);
        dream_proc_write(p, s, k);
        async_writer_write(dump_txaudio, s, k * sizeof(int16_t));
        in += k;
        n -= k;
    }
}

void sbitx_drm_mix(float *i, float *q, size_t n, double hz, double rate, double *phase)
{
    const double step = 2.0 * M_PI * hz / rate;
    double ph = *phase;

    for (size_t j = 0; j < n; j++) {
        double c = cos(ph), s = sin(ph);
        double re = i[j], im = q[j];

        /* (re + j im) * e^(-j ph) */
        i[j] = (float) (re * c + im * s);
        q[j] = (float) (im * c - re * s);
        ph += step;
        if (ph > M_PI)
            ph -= 2.0 * M_PI;
    }
    *phase = ph;
}

size_t sbitx_drm_tx_iq(float *i, float *q, size_t n)
{
    int16_t s[2048];
    size_t got = 0;
    dream_proc *p = atomic_load(&tx);

    if (!p)
        return 0;
    if (!tx_primed) {
        if (dream_proc_readable(p) < 2 * DRM_PRIME)
            return 0;
        tx_primed = true;
    }
    /* whole I/Q pairs only */
    if (dream_proc_readable(p) < 2)
        tx_primed = false;              /* ran dry: queue again */
    while (got < n && dream_proc_readable(p) >= 2) {
        size_t want = n - got < sizeof(s) / sizeof(s[0]) / 2 ? n - got : sizeof(s) / sizeof(s[0]) / 2;
        size_t avail = dream_proc_readable(p) / 2;
        size_t k;

        if (want > avail)
            want = avail;
        k = dream_proc_read(p, s, 2 * want) / 2;
        for (size_t j = 0; j < k; j++) {
            i[got + j] = (float) s[2 * j] / 32768.0f;
            q[got + j] = (float) s[2 * j + 1] / 32768.0f;
        }
        got += k;
        if (!k)
            break;
    }
    sbitx_drm_mix(i, q, got, SBITX_DRM_TX_IF, SBITX_DRM_RATE, &tx_phase);
    for (size_t j = 0; dump_txiq && j < got; j++) {
        int16_t pair[2] = { to_s16(i[j]), to_s16(q[j]) };

        async_writer_write(dump_txiq, pair, sizeof(pair));
    }
    return got;
}
