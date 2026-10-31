/* sBitx CW (Morse code) modem
 *
 * TX: DDS sine oscillator with raised-cosine envelope.
 * RX: a skimmer. tone_scan finds the keyed carriers in the passband; each
 * gets a channel of its own (a mixer to its pitch, an envelope filter half
 * a dot long, levels and speed of its own), and gives out lines of text.
 *
 * Copyright (C) 2024-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sbitx_cw.h"
#include "tone_scan.h"

static int cw_pitch_hz = 700;
static int cw_wpm = 20;

bool sbitx_cw_init(int wpm, int pitch)
{
    if (pitch > 0)
        cw_pitch_hz = pitch;
    if (wpm > 0)
        cw_wpm = wpm;
    sbitx_cw_rx_reset();
    return true;
}

void sbitx_cw_shutdown(void)
{
    sbitx_cw_rx_reset();
}

void sbitx_cw_set_wpm(int wpm)
{
    if (wpm > 0)
        cw_wpm = wpm;
}

void sbitx_cw_set_pitch(int pitch)
{
    if (pitch > 0)
        cw_pitch_hz = pitch;
}

static int lookup_morse(const char *text, char *pattern, int max_pat)
{
    static const char *morse_tx[128] = {
        ['a'] = ".-",    ['b'] = "-...",  ['c'] = "-.-.",  ['d'] = "-..",
        ['e'] = ".",     ['f'] = "..-.",  ['g'] = "--.",   ['h'] = "....",
        ['i'] = "..",    ['j'] = ".---",  ['k'] = "-.-",   ['l'] = ".-..",
        ['m'] = "--",    ['n'] = "-.",    ['o'] = "---",   ['p'] = ".--.",
        ['q'] = "--.-",  ['r'] = ".-.",   ['s'] = "...",   ['t'] = "-",
        ['u'] = "..-",   ['v'] = "...-",  ['w'] = ".--",   ['x'] = "-..-",
        ['y'] = "-.--",  ['z'] = "--..",
        ['0'] = "-----", ['1'] = ".----", ['2'] = "..---", ['3'] = "...--",
        ['4'] = "....-", ['5'] = ".....", ['6'] = "-....", ['7'] = "--...",
        ['8'] = "---..", ['9'] = "----.",
        ['/'] = "-..-.", ['?'] = "..--..",['='] = "-...-", ['.'] = ".-.-.-",
        [','] = "--..--",[' '] = "/",
    };

    int pi = 0;
    for (const char *p = text; *p && pi < max_pat - 2; p++)
    {
        /* unsigned: a non-ASCII byte (UTF-8 text from the web panel) was a
         * negative index into morse_tx. */
        unsigned char c = (unsigned char) *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        const char *sym = (c < 128) ? morse_tx[c] : NULL;
        if (!sym) continue;

        if (pi > 0) { pattern[pi++] = ' '; }
        for (const char *s = sym; *s && pi < max_pat - 1; s++)
            pattern[pi++] = *s;
        pattern[pi] = '\0';
    }
    return pi;
}

/* Silence or a keyed tone of n samples at 96 kHz; tones get raised-cosine
 * edges inside their own duration. */
static int cw_emit(float *signal, int idx, int max_n, int n, bool key_on,
                   int slope_n, float *phase, float dds_step)
{
    for (int i = 0; i < n && idx < max_n; i++, idx++)
    {
        if (!key_on) {
            signal[idx] = 0.0f;
            continue;
        }
        float env = 1.0f;
        if (i < slope_n)
            env = 0.5f - 0.5f * cosf((float) M_PI * i / slope_n);
        else if (i >= n - slope_n)
            env = 0.5f + 0.5f * cosf((float) M_PI * (i - n + slope_n) / slope_n);
        *phase += dds_step;
        if (*phase > 2.0f * (float) M_PI) *phase -= 2.0f * (float) M_PI;
        signal[idx] = env * sinf(*phase);
    }
    if (!key_on)
        *phase = 0.0f;
    return idx;
}

int sbitx_cw_encode(const char *message, float *signal, int max_n,
                    int wpm, int pitch)
{
    char pattern[1024];
    lookup_morse(message, pattern, sizeof(pattern));

    /* PARIS: 1 dot = 1.2 s / WPM. Gaps: 1 dot between the elements of a
     * character, 3 between characters, 7 between words. The pattern holds
     * characters separated by ' ' and words as " / ", so ' ' is 3 dots and
     * '/' 1 more (3 + 1 + 3 = 7). The element gap was missing: "H" went out
     * as one long tone, and no receiver could decode anything. */
    const int dot_n = (int) (1.2f / (float) wpm * 96000.0f);
    const int slope_n = (int) (0.005f * 96000.0f);
    float phase = 0.0f;
    const float dds_step = 2.0f * (float) M_PI * pitch / 96000.0f;
    int idx = 0;
    bool prev_element = false;

    for (int pos = 0; pattern[pos] && idx < max_n; pos++)
    {
        char c = pattern[pos];
        if (c == '.' || c == '-') {
            if (prev_element)
                idx = cw_emit(signal, idx, max_n, dot_n, false, slope_n, &phase, dds_step);
            idx = cw_emit(signal, idx, max_n, c == '.' ? dot_n : 3 * dot_n, true,
                          slope_n, &phase, dds_step);
            prev_element = true;
        } else {
            idx = cw_emit(signal, idx, max_n, c == '/' ? dot_n : 3 * dot_n, false,
                          slope_n, &phase, dds_step);
            prev_element = false;
        }
    }
    /* A character gap at the end, so a receiver sees it finish. */
    idx = cw_emit(signal, idx, max_n, 3 * dot_n, false, slope_n, &phase, dds_step);
    return idx;
}
/* ─── RX: the skimmer ──────────────────────────────────────────────
 *
 * The old receiver listened on one Goertzel bin at cw_pitch at a fixed
 * speed: on a rig in USB with a 3 kHz filter the stations are wherever
 * they are in the passband, at their own speeds, and it decoded almost
 * nothing on air.
 *
 * Time here is in ticks of 2 ms (24 samples). A channel's envelope is the
 * magnitude of its baseband over its last k ticks, Hann-weighted, k a dot
 * (12 to 150 ms): as quick as a boxcar of half a dot, and with sidelobes
 * low enough (below -30 dB 80 Hz away at 20 WPM) that a strong neighbour
 * or a carrier does not key the channel. Its marks and the gaps after
 * them are kept until the line ends, and only then read as text, each
 * with the speed measured at the time: so the first characters of a line,
 * sent before the speed was known, are read at the speed found later. */

#define CW_RATE        12000
#define CW_TICK        24
#define CW_TICK_S      ((float) CW_TICK / CW_RATE)
#define CW_MAX_CH      16
#define CW_MAX_K       76
#define CW_MIN_MARK    5              /* ticks: shorter is a glitch, not a dot */
#define CW_MAX_EL      400            /* marks a line holds: ~90 characters */
#define CW_HIST        10
#define CW_LO_HZ       200.0f
#define CW_HI_HZ       3000.0f
#define CW_SEP_HZ      30.0f          /* a candidate nearer a channel is that channel */
#define CW_CARRIER_S   4.0f           /* a mark this long is a carrier, not keying */
#define CW_IDLE_S      30.0f          /* a channel this long without a mark is freed */
#define CW_FLUSH_S     1.5f           /* a gap this long ends a line */
#define CW_LINE_EL     150            /* after this many marks, a word space ends it */

typedef struct {
    uint16_t mark, gap;               /* ticks; gap 0: the line's last mark */
    float    dot;                     /* the speed then, 0 before it was known */
} cw_el;

typedef struct {
    bool     used, on, carrier, first;
    uint32_t first_mark_t;            /* when the channel's first mark began */
    float    pitch;
    float    rot[2], osc[2];
    float    tick[2];
    int      tick_n;
    float    ring[CW_MAX_K][2];
    float    hann[CW_MAX_K], hann_sum;
    int      ring_i, k;
    float    noise, peak;
    int      pending;
    uint32_t t, edge_t, last_mark_t;
    float    dot;                     /* ticks */
    bool     locked;                  /* dot measured, not the first guess */
    float    hist[CW_HIST];
    int      nhist;
    cw_el    el[CW_MAX_EL];
    int      nel;
    float    snr_sum;
    char     text[CW_MAX_EL / 2];
} cw_chan;

static cw_chan    cw_ch[CW_MAX_CH];
static tone_scan *cw_scan;
static float      cw_replay[TONE_SCAN_HISTORY];

static const struct { const char *p; char c; } cw_table[] = {
    { ".-", 'A' }, { "-...", 'B' }, { "-.-.", 'C' }, { "-..", 'D' }, { ".", 'E' },
    { "..-.", 'F' }, { "--.", 'G' }, { "....", 'H' }, { "..", 'I' }, { ".---", 'J' },
    { "-.-", 'K' }, { ".-..", 'L' }, { "--", 'M' }, { "-.", 'N' }, { "---", 'O' },
    { ".--.", 'P' }, { "--.-", 'Q' }, { ".-.", 'R' }, { "...", 'S' }, { "-", 'T' },
    { "..-", 'U' }, { "...-", 'V' }, { ".--", 'W' }, { "-..-", 'X' }, { "-.--", 'Y' },
    { "--..", 'Z' },
    { "-----", '0' }, { ".----", '1' }, { "..---", '2' }, { "...--", '3' }, { "....-", '4' },
    { ".....", '5' }, { "-....", '6' }, { "--...", '7' }, { "---..", '8' }, { "----.", '9' },
    { "-..-.", '/' }, { "..--..", '?' }, { "-...-", '=' }, { ".-.-.-", '.' }, { "--..--", ',' },
    { ".-.-.", '+' }, { "-....-", '-' }, { "-.--.", '(' }, { "-.--.-", ')' }, { ".--.-.", '@' },
};

static char cw_char(const char *el)
{
    for (size_t i = 0; i < sizeof cw_table / sizeof cw_table[0]; i++)
        if (!strcmp(cw_table[i].p, el))
            return cw_table[i].c;
    return 0;
}

void sbitx_cw_rx_reset(void)
{
    memset(cw_ch, 0, sizeof cw_ch);
    tone_scan_free(cw_scan);
    cw_scan = NULL;
}

static void chan_k(cw_chan *c)
{
    int k = (int) lrintf(c->dot);
    if (k < 6) k = 6;
    if (k > CW_MAX_K) k = CW_MAX_K;
    if (k == c->k)
        return;
    c->k = k;
    c->hann_sum = 0.0f;
    for (int j = 0; j < k; j++) {
        c->hann[j] = 0.5f - 0.5f * cosf(2.0f * (float) M_PI * (j + 0.5f) / k);
        c->hann_sum += c->hann[j];
    }
}

static void chan_open(cw_chan *c, float pitch, float floor_power)
{
    memset(c, 0, sizeof *c);
    c->used = true;
    c->first = true;
    c->pitch = pitch;
    float w = 2.0f * (float) M_PI * pitch / CW_RATE;
    c->rot[0] = cosf(w);
    c->rot[1] = -sinf(w);
    c->osc[0] = 1.0f;
    c->dot = 1.2f / (float) cw_wpm / CW_TICK_S;
    chan_k(c);
    /* The scan's floor is white noise of variance floor / sum(w^2) per
     * sample (w its Hann window); the envelope's mean is then
     * sqrt(pi / 4) * sqrt(variance * sum(h^2)) / sum(h), h this channel's
     * weights over its samples, and sum(h^2) = 3/8 k CW_TICK * hann's peak. */
    float var = floor_power / (3.0f * TONE_SCAN_N / 8.0f);
    c->noise = 0.886f * sqrtf(var * 0.375f * (float) (c->k * CW_TICK)) / (c->hann_sum * CW_TICK);
    c->peak = c->noise;
}

/* Speed: the marks of the last few characters fall in two groups, dots and
 * dashes three times as long. A 2-means split of the recent mark lengths
 * finds both once a dash and a dot have been seen, at any speed. */
static void chan_speed(cw_chan *c, float len)
{
    if (c->nhist == CW_HIST)
        memmove(c->hist, c->hist + 1, (CW_HIST - 1) * sizeof(float));
    else
        c->nhist++;
    c->hist[c->nhist - 1] = len;
    if (c->nhist < 4)
        return;

    float lo = c->hist[0], hi = c->hist[0];
    for (int i = 1; i < c->nhist; i++) {
        if (c->hist[i] < lo) lo = c->hist[i];
        if (c->hist[i] > hi) hi = c->hist[i];
    }
    for (int it = 0; it < 6; it++) {
        float th = sqrtf(lo * hi), sl = 0, sh = 0;
        int nl = 0, nh = 0;
        for (int i = 0; i < c->nhist; i++)
            if (c->hist[i] < th) { sl += c->hist[i]; nl++; } else { sh += c->hist[i]; nh++; }
        if (!nl || !nh)
            return;
        lo = sl / nl;
        hi = sh / nh;
    }
    float r = hi / lo;
    if (r < 2.0f || r > 4.5f)
        return;                       /* one kind only, or not Morse */
    float dot = 0.5f * (lo + hi / 3.0f);
    c->dot = c->locked ? c->dot + 0.5f * (dot - c->dot) : dot;
    if (c->dot < 10.0f) c->dot = 10.0f;     /* 60 WPM */
    if (c->dot > 75.0f) c->dot = 75.0f;     /* 8 WPM */
    c->locked = true;
    chan_k(c);
}

/* Read the line's marks as text, give it out, and start a new line. */
static void chan_flush(cw_chan *c, sbitx_cw_rx_cb cb, void *user)
{
    int n = c->nel, marks = 0, good = 0, len = 0, i = 0;
    char el[8];
    int nel = 0;

    c->nel = 0;
    if (n == 0)
        return;
    if (!c->locked)
        return;                       /* never saw both dots and dashes */
    float first_dot = c->dot;
    for (int j = 0; j < n; j++)
        if (c->el[j].dot > 0.0f) { first_dot = c->el[j].dot; break; }

    /* A channel that opened mid-character (its audio began less than a
     * character gap before its first mark) has that character partial. */
    if (c->first && c->first_mark_t < 3.0f * first_dot) {
        while (i < n && c->el[i].gap && c->el[i].gap <= 2.0f * (c->el[i].dot > 0 ? c->el[i].dot : first_dot))
            i++;
        i++;
    }
    c->first = false;
    for (; i < n; i++) {
        const cw_el *e = &c->el[i];
        float dot = e->dot > 0.0f ? e->dot : first_dot;
        float x = e->mark / dot;
        marks++;
        if ((x > 0.5f && x < 1.7f) || (x > 2.2f && x < 4.3f))
            good++;
        if (nel < (int) sizeof el - 1)
            el[nel++] = x > 2.0f ? '-' : '.';
        else
            nel = (int) sizeof el;    /* too long: not a character */
        if (e->gap && e->gap <= 2.0f * dot)
            continue;
        if (nel < (int) sizeof el) {
            el[nel] = '\0';
            char ch = cw_char(el);
            if (len < (int) sizeof c->text - 2)
                c->text[len++] = ch ? ch : '*';
        }
        nel = 0;
        if (e->gap > 5.0f * dot && len > 0 && c->text[len - 1] != ' ')
            c->text[len++] = ' ';
    }
    while (len > 0 && c->text[len - 1] == ' ')
        len--;
    c->text[len] = '\0';

    /* A line is Morse when its marks were: at least a few, nearly all near
     * one dot or three. Voice, data tones and noise on a channel are not. */
    if (len > 0 && marks >= 4 && good * 5 >= marks * 4 && cb) {
        sbitx_cw_rx_line line = {
            .text = c->text,
            .pitch_hz = c->pitch,
            .wpm = (int) lrintf(1.2f / (c->dot * CW_TICK_S)),
            .snr_db = c->snr_sum / (float) n,
        };
        cb(&line, user);
    }
    c->snr_sum = 0.0f;
}

static void chan_mark_end(cw_chan *c, uint32_t len, sbitx_cw_rx_cb cb, void *user)
{
    if (c->nel > 0) {
        uint32_t gap = c->edge_t - len - c->last_mark_t;
        cw_el *p = &c->el[c->nel - 1];
        p->gap = (uint16_t) (gap > 65535 ? 65535 : gap ? gap : 1);
        /* a long line ends at a word space */
        if (c->nel >= CW_LINE_EL && p->gap > 5.0f * c->dot)
            chan_flush(c, cb, user);
        else if (c->nel == CW_MAX_EL)
            chan_flush(c, cb, user);
    }
    chan_speed(c, (float) len);
    cw_el *e = &c->el[c->nel++];
    e->mark = (uint16_t) (len > 65535 ? 65535 : len);
    e->gap = 0;
    e->dot = c->locked ? c->dot : 0.0f;
    float snr = c->peak > c->noise ? 20.0f * log10f(c->peak / c->noise) : 0.0f;
    /* in 2500 Hz, as the FT8 SNR, from the envelope filter's bandwidth */
    c->snr_sum += snr - 10.0f * log10f(2500.0f * c->k * CW_TICK_S);
}

/* One envelope reading, every tick. */
static void chan_tick(cw_chan *c, float a, sbitx_cw_rx_cb cb, void *user)
{
    c->t++;
    /* The noise is the mean envelope in the gaps, readings well above it
     * (a station's edges, a neighbour's burst) all but left out. A tracker
     * that fell faster than it rose sat near the noise's 4th percentile,
     * a fifth of its mean, and the noise itself then passed for marks. */
    if (!c->on)
        c->noise += (a < 2.5f * c->noise ? 0.01f : 0.0005f) * (a - c->noise);
    if (c->noise < 1e-6f)
        c->noise = 1e-6f;             /* -120 dBFS: digital silence is not quiet noise */
    if (a > c->peak)
        c->peak = a;
    else
        c->peak += 0.0015f * (c->noise - c->peak);      /* ~1.3 s */

    bool want;
    if (c->peak < 4.0f * c->noise)
        want = false;                 /* nothing above the noise (it gets to 4x ~4e-6 of the time) */
    else if (c->on)
        want = a > c->noise + 0.35f * (c->peak - c->noise);
    else
        want = a > c->noise + 0.5f * (c->peak - c->noise);

    if (want != c->on) {
        if (++c->pending < 2)
            want = c->on;
    } else {
        c->pending = 0;
    }

    if (want && !c->on) {
        c->edge_t = c->t;
        if (c->first && !c->first_mark_t)
            c->first_mark_t = c->t;
    } else if (!want && c->on) {
        uint32_t len = c->t - c->edge_t;
        if (c->carrier) {
            c->carrier = false;       /* the carrier went: listen again */
            c->edge_t = c->last_mark_t = c->t;
        } else if (len < CW_MIN_MARK) {
            c->edge_t = c->last_mark_t;   /* never there: the gap goes on */
        } else {
            c->edge_t = c->t;
            chan_mark_end(c, len, cb, user);
            c->last_mark_t = c->t;
        }
    } else if (want && c->on) {
        if (!c->carrier && (float) (c->t - c->edge_t) * CW_TICK_S > CW_CARRIER_S) {
            c->carrier = true;
            c->nel = 0;
            c->first = true;
        }
    } else if (c->nel > 0 && (float) (c->t - c->edge_t) * CW_TICK_S > CW_FLUSH_S) {
        chan_flush(c, cb, user);
    }
    c->on = want;
}

static void chan_sample(cw_chan *c, float x, sbitx_cw_rx_cb cb, void *user)
{
    c->tick[0] += x * c->osc[0];
    c->tick[1] += x * c->osc[1];
    float re = c->osc[0] * c->rot[0] - c->osc[1] * c->rot[1];
    c->osc[1] = c->osc[0] * c->rot[1] + c->osc[1] * c->rot[0];
    c->osc[0] = re;
    if (++c->tick_n < CW_TICK)
        return;

    float g = 1.0f / hypotf(c->osc[0], c->osc[1]);
    c->osc[0] *= g;
    c->osc[1] *= g;
    c->ring[c->ring_i][0] = c->tick[0];
    c->ring[c->ring_i][1] = c->tick[1];
    c->ring_i = (c->ring_i + 1) % CW_MAX_K;
    c->tick[0] = c->tick[1] = 0.0f;
    c->tick_n = 0;

    float s[2] = { 0.0f, 0.0f };
    for (int j = 0; j < c->k; j++) {
        const float *r = c->ring[(c->ring_i - 1 - j + CW_MAX_K) % CW_MAX_K];
        s[0] += c->hann[j] * r[0];
        s[1] += c->hann[j] * r[1];
    }
    chan_tick(c, hypotf(s[0], s[1]) / (c->hann_sum * CW_TICK), cb, user);
}

/* New channels on the narrow peaks well above the floor that no channel
 * has yet. Narrow: over a second, voice smears across the band, a keyed
 * carrier does not. */
static void cw_find(void)
{
    const float *m = tone_scan_mean(cw_scan);
    const float fl = tone_scan_floor(cw_scan);
    const int lo = tone_scan_lo_bin(cw_scan) + 10, hi = tone_scan_hi_bin(cw_scan) - 10;

    for (int b = lo; b <= hi; b++) {
        if (m[b] < 6.0f * fl)
            continue;
        bool top = true;
        for (int d = -5; d <= 5 && top; d++)
            if (d && m[b + d] > m[b])
                top = false;
        if (!top || m[b] < 4.0f * m[b - 10] || m[b] < 4.0f * m[b + 10])
            continue;
        float hz = tone_scan_peak_hz(m, b);
        cw_chan *free_ch = NULL;
        bool known = false;
        for (int i = 0; i < CW_MAX_CH; i++) {
            if (!cw_ch[i].used) { if (!free_ch) free_ch = &cw_ch[i]; continue; }
            if (fabsf(cw_ch[i].pitch - hz) < CW_SEP_HZ) known = true;
        }
        if (!known && free_ch)
            chan_open(free_ch, hz, fl);
    }
}

void sbitx_cw_rx_feed(const float *audio_12k, int n, sbitx_cw_rx_cb cb, void *user)
{
    if (!cw_scan && !(cw_scan = tone_scan_new(12, CW_LO_HZ, CW_HI_HZ)))
        return;

    while (n > 0) {
        int k = n < TONE_SCAN_HOP ? n : TONE_SCAN_HOP;
        if (tone_scan_feed(cw_scan, audio_12k, k)) {
            bool was[CW_MAX_CH];
            for (int i = 0; i < CW_MAX_CH; i++)
                was[i] = cw_ch[i].used;
            cw_find();
            /* a new channel starts from the scan's history */
            int nh = -1;
            for (int i = 0; i < CW_MAX_CH; i++)
                if (cw_ch[i].used && !was[i]) {
                    if (nh < 0)
                        nh = tone_scan_history(cw_scan, cw_replay);
                    for (int j = 0; j < nh; j++)
                        chan_sample(&cw_ch[i], cw_replay[j], cb, user);
                }
        }
        for (int i = 0; i < CW_MAX_CH; i++) {
            cw_chan *c = &cw_ch[i];
            if (!c->used)
                continue;
            for (int j = 0; j < k; j++)
                chan_sample(c, audio_12k[j], cb, user);
            if (!c->on && (float) (c->t - c->last_mark_t) * CW_TICK_S > CW_IDLE_S) {
                chan_flush(c, cb, user);
                c->used = false;
            }
        }
        audio_12k += k;
        n -= k;
    }
}

void sbitx_cw_rx_flush(sbitx_cw_rx_cb cb, void *user)
{
    for (int i = 0; i < CW_MAX_CH; i++)
        if (cw_ch[i].used && !cw_ch[i].carrier)
            chan_flush(&cw_ch[i], cb, user);
}
