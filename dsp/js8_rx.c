/* js8_rx - finds and decodes JS8 Normal frames.  See js8_rx.h.
 *
 * The search and soft bits follow ft8_lib's ft8_sync_score,
 * ftx_find_candidates and ft8_extract_likelihood (MIT), with JS8's
 * Costas arrays and without the Gray map, as JS8Call reads tones
 * (JS8_Mode/whitening_processor.h: each tone's bits, MSB first).
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "js8_rx.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ft8_subtract.h"
#include "common/monitor.h"
#include "ft8/decode.h"

#define JS8_TONES          8
#define JS8_NUM_COSTAS     3
#define JS8_TIME_OSR       2
#define JS8_FREQ_OSR       2
#define JS8_F_MIN_HZ       100.0f
#define JS8_F_MAX_HZ       3000.0f
#define JS8_MAX_CANDIDATES 140      /* as radiod's FT8 */
#define JS8_PASSES         3        /* JS8Call's: two subtractions */
#define JS8_MIN_SCORE      10
#define JS8_START_MIN      (-10)    /* candidate start, in symbols, as ft8_lib */
#define JS8_START_MAX      20
#define JS8_LLR_VARIANCE   24.0f    /* ft8_lib's normalisation of the soft bits */
#define JS8_TIME_BIAS_SYMBOLS 1.375f   /* ft8_lib's waterfall delay: radiod's FT8's 0.22 s */
#define JS8_SNR_BW_HZ      2500.0f  /* WSJT-X's SNR bandwidth */
#define JS8_SNR_MIN_DB     (-30.0f)
#define JS8_SNR_MAX_DB     30.0f
#define DB_PER_STEP        0.5f     /* the waterfall's 8-bit magnitudes */
#define DB_FLOOR           (-120.0f)

static float mag_db(uint8_t v) { return v * DB_PER_STEP + DB_FLOOR; }

static const uint8_t *cand_mag(const ftx_waterfall_t *wf, const ftx_candidate_t *c)
{
    size_t off = (size_t) c->time_offset;
    off = off * wf->time_osr + c->time_sub;
    off = off * wf->freq_osr + c->freq_sub;
    off = off * wf->num_bins + c->freq_offset;
    return wf->mag + off;
}

/* ft8_lib's sync score: each Costas tone against its neighbours in
 * frequency and time, averaged over the symbols inside the window. */
static int sync_score(const ftx_waterfall_t *wf, const ftx_candidate_t *c,
                      const int (*costas)[JS8_COSTAS_LEN])
{
    int score = 0, n = 0;
    const uint8_t *m0 = cand_mag(wf, c);
    for (int a = 0; a < JS8_NUM_COSTAS; a++)
        for (int k = 0; k < JS8_COSTAS_LEN; k++) {
            int block = JS8_COSTAS_STEP * a + k;
            int abs = c->time_offset + block;
            if (abs < 0)
                continue;
            if (abs >= wf->num_blocks)
                break;
            const uint8_t *p = m0 + block * wf->block_stride;
            int sm = costas[a][k];
            if (sm > 0) { score += p[sm] - p[sm - 1]; n++; }
            if (sm < JS8_TONES - 1) { score += p[sm] - p[sm + 1]; n++; }
            if (k > 0 && abs > 0) { score += p[sm] - p[sm - wf->block_stride]; n++; }
            if (k + 1 < JS8_COSTAS_LEN && abs + 1 < wf->num_blocks) {
                score += p[sm] - p[sm + wf->block_stride];
                n++;
            }
        }
    return n ? score / n : 0;
}

/* The best max candidates, best first (insertion: there are few). */
static int find_candidates(const ftx_waterfall_t *wf, const int (*costas)[JS8_COSTAS_LEN],
                           ftx_candidate_t *out, int max)
{
    int n = 0;
    ftx_candidate_t c;
    for (c.time_sub = 0; c.time_sub < wf->time_osr; c.time_sub++)
        for (c.freq_sub = 0; c.freq_sub < wf->freq_osr; c.freq_sub++)
            for (c.time_offset = JS8_START_MIN; c.time_offset < JS8_START_MAX; c.time_offset++)
                for (c.freq_offset = 0; c.freq_offset + JS8_TONES - 1 < wf->num_bins; c.freq_offset++) {
                    c.score = (int16_t) sync_score(wf, &c, costas);
                    if (c.score < JS8_MIN_SCORE || (n == max && c.score <= out[n - 1].score))
                        continue;
                    int i = n < max ? n++ : n - 1;
                    while (i > 0 && out[i - 1].score < c.score) {
                        out[i] = out[i - 1];
                        i--;
                    }
                    out[i] = c;
                }
    return n;
}

static float max4(float a, float b, float c, float d)
{
    float m = a > b ? a : b, n = c > d ? c : d;
    return m > n ? m : n;
}

/* Max-log soft bits of the 58 data symbols, positive for a 1, each tone's
 * bits MSB first, normalised as ft8_lib does. */
static void extract_llr(const ftx_waterfall_t *wf, const ftx_candidate_t *c, float llr[JS8_N])
{
    const uint8_t *m0 = cand_mag(wf, c);
    for (int k = 0; k < JS8_ND; k++) {
        int sym = k + (k < JS8_ND / 2 ? JS8_PARITY_AT : JS8_PARITY_AT * 2);
        int block = c->time_offset + sym;
        float *l = llr + 3 * k;
        if (block < 0 || block >= wf->num_blocks) {
            l[0] = l[1] = l[2] = 0;
            continue;
        }
        const uint8_t *p = m0 + sym * wf->block_stride;
        float s[JS8_TONES];
        for (int t = 0; t < JS8_TONES; t++)
            s[t] = mag_db(p[t]);
        l[0] = max4(s[4], s[5], s[6], s[7]) - max4(s[0], s[1], s[2], s[3]);
        l[1] = max4(s[2], s[3], s[6], s[7]) - max4(s[0], s[1], s[4], s[5]);
        l[2] = max4(s[1], s[3], s[5], s[7]) - max4(s[0], s[2], s[4], s[6]);
    }
    float sum = 0, sum2 = 0;
    for (int i = 0; i < JS8_N; i++) {
        sum += llr[i];
        sum2 += llr[i] * llr[i];
    }
    float var = (sum2 - sum * sum / JS8_N) / JS8_N;
    if (var > 0) {
        float g = sqrtf(JS8_LLR_VARIANCE / var);
        for (int i = 0; i < JS8_N; i++)
            llr[i] *= g;
    }
}

static float noise_db(const ftx_waterfall_t *wf)
{
    uint32_t hist[256] = { 0 };
    size_t n = (size_t) wf->num_blocks * (size_t) wf->block_stride;
    if (n == 0)
        return NAN;
    for (size_t i = 0; i < n; i++)
        hist[wf->mag[i]]++;
    size_t acc = 0;
    for (int v = 0; v < 256; v++)
        if ((acc += hist[v]) > n / 2)
            return mag_db((uint8_t) v);
    return NAN;
}

/* As radiod's FT8: the mean power of the frame's 79 tones against the
 * median bin, less the bin's width (the tone spacing) against 2500 Hz:
 * 26 dB for Normal's 6.25 Hz. */
static bool estimate_snr(const ftx_waterfall_t *wf, const ftx_candidate_t *c,
                         const int tones[JS8_NN], float noise, float bin_hz, float *snr)
{
    if (!isfinite(noise))
        return false;
    double sum = 0;
    int used = 0;
    const uint8_t *m0 = cand_mag(wf, c);
    for (int k = 0; k < JS8_NN; k++) {
        int block = c->time_offset + k;
        if (block < 0 || block >= wf->num_blocks)
            continue;
        sum += pow(10.0, mag_db(m0[k * wf->block_stride + tones[k]]) / 10.0);
        used++;
    }
    if (used < JS8_NN / 2)
        return false;
    float s = (float) (10.0 * log10(sum / used)) - noise - 10.0f * log10f(JS8_SNR_BW_HZ / bin_hz);
    *snr = roundf(s < JS8_SNR_MIN_DB ? JS8_SNR_MIN_DB : s > JS8_SNR_MAX_DB ? JS8_SNR_MAX_DB : s);
    return true;
}

static const char *const NAMES[JS8_SUBMODES] = JS8_SUBMODE_NAMES;
static const js8_submode_params SUBMODES[JS8_SUBMODES] = {
    [JS8_SUB_NORMAL] = { NAMES[JS8_SUB_NORMAL], 1920, 15, JS8_COSTAS_ORIGINAL },
    [JS8_SUB_FAST]   = { NAMES[JS8_SUB_FAST],   1200, 10, JS8_COSTAS_MODIFIED },
    [JS8_SUB_TURBO]  = { NAMES[JS8_SUB_TURBO],  600,  6,  JS8_COSTAS_MODIFIED },
    [JS8_SUB_SLOW]   = { NAMES[JS8_SUB_SLOW],   3840, 30, JS8_COSTAS_MODIFIED },
};

const js8_submode_params *js8_submode_get(js8_submode sub)
{
    return sub >= 0 && sub < JS8_SUBMODES ? &SUBMODES[sub] : NULL;
}

int js8_rx_decode(const float *audio_12k, int nsamples, js8_rx_result *out, int max_out)
{
    return js8_rx_decode_sub(JS8_SUB_NORMAL, audio_12k, nsamples, out, max_out);
}

/* One pass over the audio: what decodes that is not in out[0..count)
 * yet is added there; the new count. */
static int decode_pass(const js8_submode_params *p, const float *audio, int nsamples,
                       js8_rx_result *out, int count, int max_out)
{
    monitor_config_t cfg = {
        .f_min = JS8_F_MIN_HZ, .f_max = JS8_F_MAX_HZ, .sample_rate = JS8_SAMPLE_RATE,
        .time_osr = JS8_TIME_OSR, .freq_osr = JS8_FREQ_OSR,
        .protocol = FTX_PROTOCOL_FT8,
        .symbol_period = (float) p->symbol / JS8_SAMPLE_RATE,
        .slot_time = (float) nsamples / JS8_SAMPLE_RATE,
    };
    monitor_t mon;
    monitor_init(&mon, &cfg);
    for (int at = 0; at + mon.block_size <= nsamples; at += mon.block_size)
        monitor_process(&mon, audio + at);

    const int (*costas)[JS8_COSTAS_LEN] = js8_costas(p->costas);
    ftx_candidate_t cands[JS8_MAX_CANDIDATES];
    int ncand = find_candidates(&mon.wf, costas, cands, JS8_MAX_CANDIDATES);
    float noise = noise_db(&mon.wf);

    for (int i = 0; i < ncand && count < max_out; i++) {
        float llr[JS8_N];
        uint8_t bits[JS8_K];
        js8_rx_result r = { 0 };
        extract_llr(&mon.wf, &cands[i], llr);
        r.hard_errors = js8_ldpc_decode(llr, JS8_BP_ITERATIONS, bits);
        if (r.hard_errors < 0 || !js8_unpack(bits, r.frame, &r.type))
            continue;
        bool dup = false;
        for (int j = 0; j < count && !dup; j++)
            dup = !strcmp(out[j].frame, r.frame) && out[j].type == r.type;
        if (dup)
            continue;
        const ftx_candidate_t *c = &cands[i];
        r.freq_hz = (mon.min_bin + c->freq_offset + (float) c->freq_sub / mon.wf.freq_osr)
                    / mon.symbol_period;
        r.time_s = (c->time_offset + (float) c->time_sub / mon.wf.time_osr - JS8_TIME_BIAS_SYMBOLS)
                   * mon.symbol_period;
        int tones[JS8_NN];
        if (js8_encode(r.frame, r.type, p->costas, tones))
            r.has_snr = estimate_snr(&mon.wf, c, tones, noise, 1.0f / mon.symbol_period, &r.snr_db);
        out[count++] = r;
    }
    monitor_free(&mon);
    return count;
}

/* JS8Call's passes: after each, what it decoded is subtracted from the
 * audio (dsp/ft8_subtract.c, plain FSK), and the next finds the weaker
 * signals those hid; a pass that finds nothing new ends it. */
int js8_rx_decode_sub(js8_submode sub, const float *audio_12k, int nsamples,
                      js8_rx_result *out, int max_out)
{
    const js8_submode_params *p = js8_submode_get(sub);
    if (!p || !audio_12k || nsamples <= 0 || !out || max_out <= 0)
        return 0;
    float *work = malloc((size_t) nsamples * sizeof(float));
    if (!work)
        return decode_pass(p, audio_12k, nsamples, out, 0, max_out);
    memcpy(work, audio_12k, (size_t) nsamples * sizeof(float));
    int count = 0;
    for (int pass = 0; pass < JS8_PASSES && count < max_out; pass++) {
        int first_new = count;
        count = decode_pass(p, work, nsamples, out, count, max_out);
        if (count == first_new || pass + 1 == JS8_PASSES)
            break;
        for (int i = first_new; i < count; i++) {
            int t[JS8_NN];
            uint8_t tones[JS8_NN];
            if (!js8_encode(out[i].frame, out[i].type, p->costas, t))
                continue;
            for (int k = 0; k < JS8_NN; k++)
                tones[k] = (uint8_t) t[k];
            js8_subtract(work, nsamples, tones, p->symbol, out[i].freq_hz, out[i].time_s);
        }
    }
    free(work);
    return count;
}
