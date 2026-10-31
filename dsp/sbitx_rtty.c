/* sBitx RTTY (Radio Teletype) modem
 *
 * TX: Baudot encoding + FSK tone generator at 96 kHz.
 * RX: a skimmer. tone_scan finds the FSK signals in the passband (two
 * tones a shift apart); each gets a channel of its own, which decodes it
 * in both polarities and gives out lines from the one that frames.
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

#include "baudot.h"

#include "sbitx_rtty.h"
#include "tone_scan.h"

bool sbitx_rtty_init(int baud, int mark, int shift)
{
    (void) mark;
    (void) shift;
    if (baud <= 0)
        return false;
    sbitx_rtty_rx_reset();
    return true;
}

void sbitx_rtty_shutdown(void)
{
    sbitx_rtty_rx_reset();
}

/* FSK TX state for one message: continuous phase per tone. */
typedef struct {
    float *signal;
    int    max_n;
    int    idx;
    int    samples_per_bit;
    int    slope_n;
    float  phase_mark, phase_space;
    float  dds_mark, dds_space;
} rtty_tx;

/* n samples of mark (1) or space (0). */
static void rtty_tone(rtty_tx *t, bool mark, int n)
{
    for (int i = 0; i < n && t->idx < t->max_n; i++, t->idx++)
    {
        float *ph = mark ? &t->phase_mark : &t->phase_space;
        *ph += mark ? t->dds_mark : t->dds_space;
        if (*ph > 2.0f * (float) M_PI) *ph -= 2.0f * (float) M_PI;
        t->signal[t->idx] = 0.4f * sinf(*ph);
    }
}

/* One 5-bit ITA2 word: start (space), 5 data bits LSB first with 1 = mark,
 * 1.5 stop bits (mark). */
static void rtty_word(rtty_tx *t, unsigned int word)
{
    rtty_tone(t, false, t->samples_per_bit);
    for (int d = 0; d < 5; d++)
        rtty_tone(t, ((word >> d) & 1) != 0, t->samples_per_bit);
    rtty_tone(t, true, t->samples_per_bit + t->samples_per_bit / 2);
}

int sbitx_rtty_encode(const char *message, float *signal, int max_n,
                      int baud, int mark, int shift)
{
    rtty_tx t = {
        .signal = signal, .max_n = max_n, .idx = 0,
        .samples_per_bit = 96000 / baud,
        .dds_mark = 2.0f * (float) M_PI * mark / 96000.0f,
        .dds_space = 2.0f * (float) M_PI * (mark - shift) / 96000.0f,
    };

    /* A mark lead-in lets the receiver find the first start bit, and LTRS
     * puts it in letters whatever it was left in. baudot_encode() writes
     * one or two words (a LTRS/FIGS shift, then the character) into an
     * array: the old code passed a single unsigned int, so every shift
     * overflowed it. It also sent data bits inverted (1 as space), which
     * no RTTY receiver decodes. */
    baudot_reset();
    rtty_tone(&t, true, t.samples_per_bit * 8);          /* ~180 ms */
    rtty_word(&t, 0x1F);                                  /* LTRS */
    size_t len = strlen(message);
    bool has_eol = len && (message[len - 1] == '\n' || message[len - 1] == '\r');
    for (const char *p = message; *p && t.idx < max_n; p++)
    {
        unsigned int words[2] = {0, 0};
        int nw = baudot_encode(words, *p);
        for (int w = 0; w < nw && w < 2; w++)
            rtty_word(&t, words[w]);
    }
    if (!has_eol) {                        /* end the line, as RTTY messages do */
        unsigned int words[2] = {0, 0};
        int nw = baudot_encode(words, '\r');
        for (int w = 0; w < nw && w < 2; w++)
            rtty_word(&t, words[w]);
        nw = baudot_encode(words, '\n');
        for (int w = 0; w < nw && w < 2; w++)
            rtty_word(&t, words[w]);
    }
    rtty_tone(&t, true, t.samples_per_bit * 4);          /* mark tail */
    return t.idx;
}


/* ─── RX: the skimmer ──────────────────────────────────────────────
 *
 * The old receiver listened for one mark tone at the configured
 * frequency, so it decoded only a station tuned exactly there; on air
 * they are wherever they are in the passband.
 *
 * Each channel mixes its two tones to DC and averages each over a bit
 * (the matched filter for a keyed tone; half a bit, as before, gave up
 * 3 dB), read when the average covers the bit just ended. The discriminator compares each tone to its own recent
 * peak (automatic threshold correction): under selective fading one tone
 * can be well below the other, and a plain mark - space then reads every
 * bit as the stronger tone. A UART finds the start bit's edge and samples
 * each bit at its centre, once with the higher tone as mark (the RTTY
 * convention, on USB) and once reversed; the polarity whose frames check
 * gives the text. */

#define RTTY_RATE       12000
#define RTTY_MAX_CH     8
#define RTTY_MAX_L      1024
#define RTTY_LINE_MAX   80
#define RTTY_LO_HZ      200.0f
#define RTTY_HI_HZ      3000.0f
#define RTTY_SEP_HZ     40.0f          /* a candidate nearer a channel is that channel */
#define RTTY_IDLE_S     30.0f          /* a channel this long without a character is freed */
#define RTTY_FLUSH_S    2.0f           /* this long without a character ends a line */
#define RTTY_Q_MIN      0.5f           /* mean |d| over a frame's bits, d in -1..1 */
#define RTTY_SCORE_MIN  4              /* good frames net before a polarity gives text */
#define RTTY_SCORE_LOW  -3             /* noise between overs: a station back is in text in ~7 frames */

typedef struct {
    bool     in_frame;
    int      mark_run;                 /* samples of mark before a start edge */
    double   start_t;                  /* sample time of the start bit's edge */
    int      bitno;                    /* next bit to sample: 0 start .. 6 stop */
    unsigned word;
    float    q;
    float    prev_d;
    bool     figs;
    int      score;                    /* good frames minus bad, recent */
    int      best;                     /* the best score since the line began */
    char     text[RTTY_LINE_MAX];
    int      len;
} rtty_uart;

typedef struct {
    bool     used;
    float    hi_hz, lo_hz;             /* the two tones */
    int      baud;
    float    spb;                      /* samples per bit */
    int      L;                        /* moving average: a bit */
    float    rot_h[2], rot_l[2], osc_h[2], osc_l[2];
    float    buf_h[RTTY_MAX_L][2], buf_l[RTTY_MAX_L][2];
    float    sum_h[2], sum_l[2];
    int      i;
    uint64_t t, last_char_t;
    float    pk_h, pk_l;               /* each tone's recent peak */
    rtty_uart u[2];                    /* [0] higher tone is mark, [1] reversed */
} rtty_chan;

static rtty_chan  rtty_ch[RTTY_MAX_CH];
static tone_scan *rtty_scan;
static int        rtty_scan_shift;
static float      rtty_replay[TONE_SCAN_HISTORY];

/* ITA2, US figures. */
static const char rtty_ltrs[32] = {
    0, 'E', '\n', 'A', ' ', 'S', 'I', 'U', '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K',
    'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q', 'O', 'B', 'G', 0, 'M', 'X', 'V', 0,
};
static const char rtty_figs[32] = {
    0, '3', '\n', '-', ' ', '\'', '8', '7', '\r', '$', '4', '\'', ',', '!', ':', '(',
    '5', '"', ')', '2', '#', '6', '0', '1', '9', '?', '&', 0, '.', '/', ';', 0,
};

void sbitx_rtty_rx_reset(void)
{
    memset(rtty_ch, 0, sizeof rtty_ch);
    tone_scan_free(rtty_scan);
    rtty_scan = NULL;
}

static void chan_open(rtty_chan *c, float lo_hz, float hi_hz, int baud)
{
    memset(c, 0, sizeof *c);
    c->used = true;
    c->lo_hz = lo_hz;
    c->hi_hz = hi_hz;
    c->baud = baud;
    c->spb = (float) RTTY_RATE / (float) baud;
    c->L = (int) lrintf(c->spb);
    if (c->L < 4) c->L = 4;
    if (c->L > RTTY_MAX_L) c->L = RTTY_MAX_L;
    float wh = 2.0f * (float) M_PI * hi_hz / RTTY_RATE;
    float wl = 2.0f * (float) M_PI * lo_hz / RTTY_RATE;
    c->rot_h[0] = cosf(wh); c->rot_h[1] = -sinf(wh);
    c->rot_l[0] = cosf(wl); c->rot_l[1] = -sinf(wl);
    c->osc_h[0] = c->osc_l[0] = 1.0f;
}

static void line_flush(rtty_chan *c, rtty_uart *u, int pol, sbitx_rtty_rx_cb cb, void *user)
{
    while (u->len > 0 && u->text[u->len - 1] == ' ')
        u->len--;
    int start = 0;
    while (start < u->len && u->text[start] == ' ')
        start++;
    u->text[u->len] = '\0';
    /* noise frames now and then: text only from a polarity that framed
     * during the line (not at its end, after the pause's noise frames) */
    if (u->len > start && u->best >= RTTY_SCORE_MIN && cb) {
        sbitx_rtty_rx_line line = {
            .text = u->text + start,
            .mark_hz = pol ? c->lo_hz : c->hi_hz,
            .shift_hz = c->hi_hz - c->lo_hz,
            .reversed = pol != 0,
        };
        cb(&line, user);
    }
    u->len = 0;
    u->best = u->score;
}

/* The polarity that frames: its score leads the other's. */
static int chan_pol(const rtty_chan *c)
{
    return c->u[1].score > c->u[0].score ? 1 : 0;
}

static void uart_char(rtty_chan *c, int pol, unsigned word, sbitx_rtty_rx_cb cb, void *user)
{
    rtty_uart *u = &c->u[pol];
    char ch = 0;

    if (word == 0x1F) { u->figs = false; return; }       /* LTRS */
    if (word == 0x1B) { u->figs = true; return; }        /* FIGS */
    ch = u->figs ? rtty_figs[word] : rtty_ltrs[word];
    if (ch == ' ')
        u->figs = false;                                  /* unshift on space */
    if (!ch)
        return;
    if (pol != chan_pol(c)) {
        u->len = 0;                   /* the other polarity's text is the line */
        return;
    }
    c->last_char_t = c->t;
    if (ch == '\r' || ch == '\n') {
        line_flush(c, u, pol, cb, user);
        return;
    }
    if (u->len < RTTY_LINE_MAX - 1)
        u->text[u->len++] = ch;
    if (ch == ' ' && u->len >= RTTY_LINE_MAX - 10)
        line_flush(c, u, pol, cb, user);
}

/* One UART step; d > 0 is mark for this polarity. */
static void uart_step(rtty_chan *c, int pol, float d, sbitx_rtty_rx_cb cb, void *user)
{
    rtty_uart *u = &c->u[pol];

    if (!u->in_frame) {
        /* A start bit: space after at least a bit of steady mark. The
         * averaging delays the crossing by half its length. */
        if (d > 0.0f) {
            u->mark_run++;
        } else {
            if (u->prev_d > 0.0f && u->mark_run >= (int) c->spb) {
                u->in_frame = true;
                u->start_t = (double) c->t - c->L / 2.0;
                u->bitno = 0;
                u->word = 0;
                u->q = 0.0f;
            }
            u->mark_run = 0;
        }
    } else if ((double) c->t >= u->start_t + (u->bitno + 0.5) * c->spb + c->L / 2.0) {
        bool is_mark = d > 0.0f;
        bool ok = true;
        u->q += fabsf(d);
        if (u->bitno == 0)
            ok = !is_mark;            /* start bit is space */
        else if (u->bitno <= 5)
            u->word |= (is_mark ? 1u : 0u) << (u->bitno - 1);   /* LSB first, 1 = mark */
        else {
            ok = is_mark;             /* stop bit is mark */
            if (ok && u->q / 7.0f >= RTTY_Q_MIN) {
                if (u->score < 20) u->score++;
                if (u->score > u->best) u->best = u->score;
                uart_char(c, pol, u->word, cb, user);
            } else {
                ok = false;
            }
            u->in_frame = false;
            u->mark_run = ok ? (int) c->spb : 0;   /* the stop bit counts as idle mark */
        }
        if (!ok) {
            if (u->score > RTTY_SCORE_LOW) u->score--;
            u->in_frame = false;
            u->mark_run = 0;
        } else if (u->in_frame) {
            u->bitno++;
        }
    }
    u->prev_d = d;
}

static inline void osc_step(float osc[2], const float rot[2])
{
    float re = osc[0] * rot[0] - osc[1] * rot[1];
    float im = osc[0] * rot[1] + osc[1] * rot[0];
    osc[0] = re;
    osc[1] = im;
}

static void chan_sample(rtty_chan *c, float x, sbitx_rtty_rx_cb cb, void *user)
{
    float ph[2] = { x * c->osc_h[0], x * c->osc_h[1] };
    float pl[2] = { x * c->osc_l[0], x * c->osc_l[1] };
    osc_step(c->osc_h, c->rot_h);
    osc_step(c->osc_l, c->rot_l);
    c->sum_h[0] += ph[0] - c->buf_h[c->i][0];
    c->sum_h[1] += ph[1] - c->buf_h[c->i][1];
    c->sum_l[0] += pl[0] - c->buf_l[c->i][0];
    c->sum_l[1] += pl[1] - c->buf_l[c->i][1];
    c->buf_h[c->i][0] = ph[0]; c->buf_h[c->i][1] = ph[1];
    c->buf_l[c->i][0] = pl[0]; c->buf_l[c->i][1] = pl[1];
    if (++c->i == c->L)
        c->i = 0;
    c->t++;
    if ((c->t & 1023) == 0) {         /* keep the oscillators on the unit circle */
        float gh = 1.0f / hypotf(c->osc_h[0], c->osc_h[1]);
        float gl = 1.0f / hypotf(c->osc_l[0], c->osc_l[1]);
        c->osc_h[0] *= gh; c->osc_h[1] *= gh;
        c->osc_l[0] *= gl; c->osc_l[1] *= gl;
    }

    float eh = hypotf(c->sum_h[0], c->sum_h[1]);
    float el = hypotf(c->sum_l[0], c->sum_l[1]);
    /* each tone's peak: instant attack, ~1 s decay */
    const float decay = 1.0f - 1.0f / RTTY_RATE;
    c->pk_h = eh > c->pk_h ? eh : c->pk_h * decay;
    c->pk_l = el > c->pk_l ? el : c->pk_l * decay;
    /* Each tone against its own peak, but a peak no lower than 0.3 of
     * the other's (10 dB of correction at most): a tone not yet seen, or
     * faded into the noise, has a peak of noise, and its noise read as a
     * full-strength tone. */
    float top = c->pk_h > c->pk_l ? c->pk_h : c->pk_l;
    float pkh = c->pk_h > 0.3f * top ? c->pk_h : 0.3f * top;
    float pkl = c->pk_l > 0.3f * top ? c->pk_l : 0.3f * top;
    float d = top > 1e-6f * c->L ? eh / pkh - el / pkl : 0.0f;   /* -120 dBFS */

    uart_step(c, 0, d, cb, user);
    uart_step(c, 1, -d, cb, user);

    if (c->t - c->last_char_t == (uint64_t) (RTTY_FLUSH_S * RTTY_RATE)) {
        int pol = chan_pol(c);
        line_flush(c, &c->u[pol], pol, cb, user);
    }
}

/* New channels where two narrow peaks stand a shift apart, well above the
 * floor, with less between them: an FSK signal's two tones. */
static void rtty_find(int baud, int shift)
{
    const float *m = tone_scan_mean(rtty_scan);
    const float fl = tone_scan_floor(rtty_scan);
    const int sb = (int) lrintf((float) shift / TONE_SCAN_HZ);
    const int lo = tone_scan_lo_bin(rtty_scan) + 3, hi = tone_scan_hi_bin(rtty_scan) - 3 - sb;

    for (int b = lo; b <= hi; b++) {
        if (m[b] < 5.0f * fl)
            continue;
        bool top = true;
        for (int d = -3; d <= 3 && top; d++)
            if (d && m[b + d] > m[b])
                top = false;
        if (!top)
            continue;
        /* the other tone: the highest bin within 2 of a shift above */
        int h = b + sb;
        for (int d = -2; d <= 2; d++)
            if (m[b + sb + d] > m[h])
                h = b + sb + d;
        if (m[h] < 5.0f * fl || m[b + sb / 2] * 3.0f > (m[b] < m[h] ? m[b] : m[h]))
            continue;
        float lo_hz = tone_scan_peak_hz(m, b), hi_hz = tone_scan_peak_hz(m, h);
        rtty_chan *free_ch = NULL;
        bool known = false;
        for (int i = 0; i < RTTY_MAX_CH; i++) {
            if (!rtty_ch[i].used) { if (!free_ch) free_ch = &rtty_ch[i]; continue; }
            if (fabsf(rtty_ch[i].lo_hz - lo_hz) < RTTY_SEP_HZ) known = true;
        }
        if (!known && free_ch)
            chan_open(free_ch, lo_hz, hi_hz, baud);
    }
}

void sbitx_rtty_rx_feed(const float *audio_12k, int n, int baud, int shift,
                        sbitx_rtty_rx_cb cb, void *user)
{
    if (baud <= 0 || shift <= 0)
        return;
    if (rtty_scan && shift != rtty_scan_shift)
        sbitx_rtty_rx_reset();
    if (!rtty_scan) {
        if (!(rtty_scan = tone_scan_new(12, RTTY_LO_HZ, RTTY_HI_HZ)))
            return;
        rtty_scan_shift = shift;
    }

    while (n > 0) {
        int k = n < TONE_SCAN_HOP ? n : TONE_SCAN_HOP;
        if (tone_scan_feed(rtty_scan, audio_12k, k)) {
            bool was[RTTY_MAX_CH];
            for (int i = 0; i < RTTY_MAX_CH; i++)
                was[i] = rtty_ch[i].used;
            rtty_find(baud, shift);
            /* a new channel starts from the scan's history */
            int nh = -1;
            for (int i = 0; i < RTTY_MAX_CH; i++)
                if (rtty_ch[i].used && !was[i]) {
                    if (nh < 0)
                        nh = tone_scan_history(rtty_scan, rtty_replay);
                    for (int j = 0; j < nh; j++)
                        chan_sample(&rtty_ch[i], rtty_replay[j], cb, user);
                }
        }
        for (int i = 0; i < RTTY_MAX_CH; i++) {
            rtty_chan *c = &rtty_ch[i];
            if (!c->used)
                continue;
            if (c->baud != baud) {
                c->used = false;
                continue;
            }
            for (int j = 0; j < k; j++)
                chan_sample(c, audio_12k[j], cb, user);
            if ((float) (c->t - c->last_char_t) > RTTY_IDLE_S * RTTY_RATE)
                c->used = false;
        }
        audio_12k += k;
        n -= k;
    }
}

void sbitx_rtty_rx_flush(sbitx_rtty_rx_cb cb, void *user)
{
    for (int i = 0; i < RTTY_MAX_CH; i++)
        if (rtty_ch[i].used) {
            int pol = chan_pol(&rtty_ch[i]);
            line_flush(&rtty_ch[i], &rtty_ch[i].u[pol], pol, cb, user);
        }
}
