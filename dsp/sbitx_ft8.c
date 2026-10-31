/* sBitx FT8 modem - encode/decode using vendored ft8_lib
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ft8/constants.h"
#include "ft8/encode.h"
#include "ft8/decode.h"
#include "ft8/message.h"
#include "ft8_js8.h"
#include "ft8/text.h"
#include "common/wave.h"
#include "common/monitor.h"

#include "sbitx_ft8.h"
#include "digi_log.h"

#define FT8_SYMBOL_PERIOD 0.160f

/* ft8_lib's candidate time runs late by a constant: its monitor's FFT
 * covers two symbols. Measured 0.20-0.24 s (its 0.08 s resolution) for
 * signals placed from 0 to 2 s into the window. */
#define FT8_LIB_TIME_BIAS_S 0.22f
#define FT8_SYMBOL_BT 2.0f
#define GFSK_CONST_K 5.336446f

static void synth_gfsk(const uint8_t *symbols, int n_sym, float f0,
                       int signal_rate, float *signal, int *num_samples)
{
    float tone_spacing = 1.0f / FT8_SYMBOL_PERIOD;
    float pi2 = (float)(2.0 * M_PI);
    int sym_samples = (int)(FT8_SYMBOL_PERIOD * signal_rate);
    float ts = 1.0f / signal_rate;
    float phase = 0.0f;
    int idx = 0;

    float gauss_a = GFSK_CONST_K * FT8_SYMBOL_BT / FT8_SYMBOL_PERIOD;

    for (int i = 0; i < n_sym; i++)
    {
        float freq = f0 + symbols[i] * tone_spacing;

        if (i == 0 || symbols[i] != symbols[i - 1])
        {
            float sum = 0.0f;
            for (int j = 0; j < sym_samples; j++)
            {
                float t = (j - sym_samples / 2.0f) * ts;
                sum += expf(-(t * t) * gauss_a * gauss_a / 2.0f);
            }
            float norm = 1.0f / sum;

            float accum = 0.0f;
            for (int j = 0; j < sym_samples && idx < *num_samples; j++, idx++)
            {
                float t = (j - sym_samples / 2.0f) * ts;
                accum += expf(-(t * t) * gauss_a * gauss_a / 2.0f) * norm;
                float inst_freq = f0 + symbols[i] * tone_spacing;
                if (i > 0 && j < sym_samples / 2 && accum < 0.5f)
                    inst_freq = f0 + symbols[i - 1] * tone_spacing;
                phase += pi2 * inst_freq * ts;
                if (phase > M_PI)  phase -= pi2;
                if (phase < -M_PI) phase += pi2;
                signal[idx] = sinf(phase);
            }
        }
        else
        {
            for (int j = 0; j < sym_samples && idx < *num_samples; j++, idx++)
            {
                phase += pi2 * freq * ts;
                if (phase > M_PI)  phase -= pi2;
                if (phase < -M_PI) phase += pi2;
                signal[idx] = sinf(phase);
            }
        }
    }

    *num_samples = idx;
}

bool sbitx_ft8_init(void)
{
    /* Nothing to set up: the log directory belongs to digi_log, which
     * creates it on the first entry. */
    return true;
}

void sbitx_ft8_shutdown(void) {}

bool sbitx_ft8_can_encode(const char *message)
{
    ftx_message_t msg;
    return message && ftx_message_encode(&msg, NULL, message) == FTX_MESSAGE_RC_OK;
}

int sbitx_ft8_encode(const char *message, float *signal, int max_samples,
                     float tone_freq)
{
    ftx_message_t msg;

    /* Pass NULL for the callsign-hash interface. ft8_lib guards every
     * callback with `if (hash_if != NULL)`, so NULL safely disables the
     * hash table (we don't need it for plain CQ/standard messages). The
     * old code passed a zeroed struct, which is non-NULL but has NULL
     * function pointers — pack28() then called save_hash() == NULL and
     * segfaulted. */
    ftx_message_rc_t rc = ftx_message_encode(&msg, NULL, message);
    if (rc != FTX_MESSAGE_RC_OK)
        return -1;

    uint8_t tones[FT8_NN];
    ft8_encode(msg.payload, tones);

    int n = max_samples;
    synth_gfsk(tones, FT8_NN, tone_freq, 12000, signal, &n);

    return n;
}

/* Noise floor of the waterfall in dB: the median bin. The waterfall stores
 * 2 * (dB + 120) clamped to 0..255 (common/monitor.c), so a 256-bin
 * histogram gives the median exactly. */
static float waterfall_noise_db(const ftx_waterfall_t *wf)
{
    uint32_t hist[256] = {0};
    size_t n = (size_t) wf->num_blocks * (size_t) wf->block_stride;
    if (n == 0)
        return NAN;
    for (size_t i = 0; i < n; i++)
        hist[wf->mag[i]]++;
    size_t half = n / 2, acc = 0;
    for (int v = 0; v < 256; v++) {
        acc += hist[v];
        if (acc > half)
            return v / 2.0f - 120.0f;
    }
    return NAN;
}

/* An SNR estimate in the WSJT-X convention (2500 Hz noise bandwidth): the
 * mean power of the decoded message's 79 tones, against the median bin,
 * less 10*log10(2500 / 6.25) = 26 dB for the bin width. Approximate: the
 * waterfall is 8-bit and the tones may straddle bins. */
static bool estimate_snr(const ftx_waterfall_t *wf, const ftx_candidate_t *cand,
                         const ftx_message_t *msg, float noise_db, float *snr_db)
{
    if (!isfinite(noise_db))
        return false;
    uint8_t tones[FT8_NN];
    ft8_encode(msg->payload, tones);
    double sum = 0;
    int used = 0;
    for (int k = 0; k < FT8_NN; k++) {
        int block = cand->time_offset + k;
        int bin = cand->freq_offset + tones[k];
        if (block < 0 || block >= wf->num_blocks || bin < 0 || bin >= wf->num_bins)
            continue;
        size_t idx = (((size_t) block * wf->time_osr + cand->time_sub) * wf->freq_osr
                      + cand->freq_sub) * wf->num_bins + bin;
        float db = wf->mag[idx] / 2.0f - 120.0f;
        sum += pow(10.0, db / 10.0);
        used++;
    }
    if (used < FT8_NN / 2)
        return false;
    float snr = (float) (10.0 * log10(sum / used)) - noise_db - 26.0f;
    if (snr < -30.0f) snr = -30.0f;
    if (snr >  30.0f) snr =  30.0f;
    *snr_db = roundf(snr);
    return true;
}

static void copy_field(char *dst, size_t n, const char *text, int start, int end)
{
    while (start < end && text[start] == ' ') start++;
    while (end > start && text[end - 1] == ' ') end--;
    int len = end - start;
    if (len <= 0 || (size_t) len >= n) { dst[0] = '\0'; return; }
    memcpy(dst, text + start, (size_t) len);
    dst[len] = '\0';
    /* A hashed callsign we could not resolve prints as <...>; a resolved
     * one as <CALL>. */
    if (dst[0] == '<') {
        if (!strcmp(dst, "<...>")) { dst[0] = '\0'; return; }
        size_t l = strlen(dst);
        if (l > 2 && dst[l - 1] == '>') {
            memmove(dst, dst + 1, l - 2);
            dst[l - 2] = '\0';
        }
    }
}

/* From the decoder's field map: in "CQ PU2UIT GG66" the token is the
 * addressee and the call the sender; in "PU2UIT PY2XYZ -12" the first call
 * is the addressee and the second the sender. */
static void parse_fields(sbitx_ft8_decode_result *r, const ftx_message_offsets_t *off)
{
    int len = (int) strlen(r->text);
    r->from[0] = r->to[0] = r->grid[0] = '\0';
    for (int i = 0; i < FTX_MAX_MESSAGE_FIELDS && off->offsets[i] >= 0; i++) {
        int start = off->offsets[i];
        int end = (i + 1 < FTX_MAX_MESSAGE_FIELDS && off->offsets[i + 1] >= 0) ? off->offsets[i + 1] : len;
        if (start > len) break;
        if (end > len) end = len;
        switch (off->types[i]) {
        case FTX_FIELD_TOKEN:
        case FTX_FIELD_TOKEN_WITH_ARG:
            if (i == 0)
                copy_field(r->to, sizeof(r->to), r->text, start, end);
            break;
        case FTX_FIELD_CALL:
            if (i == 0)
                copy_field(r->to, sizeof(r->to), r->text, start, end);
            else if (!r->from[0])
                copy_field(r->from, sizeof(r->from), r->text, start, end);
            break;
        case FTX_FIELD_GRID:
            copy_field(r->grid, sizeof(r->grid), r->text, start, end);
            break;
        default:
            break;
        }
    }
}

/* In-process FT8 decode using the linked ft8_lib (monitor + find_candidates
 * + decode). Input is 12 kHz mono float. One result per distinct message,
 * with its audio offset, start time within the window, an SNR estimate and
 * the callsigns and grid it names. Returns the number of results. */
int sbitx_ft8_decode_ex(float *audio_12k, int nsamples,
                        sbitx_ft8_decode_result *out, int max_out)
{
    return sbitx_ft8_decode_at(audio_12k, nsamples, -1.0, out, max_out);
}

int sbitx_ft8_decode_at(float *audio_12k, int nsamples, double start_s,
                        sbitx_ft8_decode_result *out, int max_out)
{
    const int   kSampleRate    = 12000;
    const int   kTimeOSR       = 2;
    const int   kFreqOSR       = 2;
    const int   kMaxCandidates = 140;
    const int   kMinScore      = 10;
    const int   kLDPCIters     = 25;
    const int   kMaxDecoded    = 50;

    if (!audio_12k || nsamples <= 0 || !out || max_out <= 0)
        return 0;

    monitor_config_t mon_cfg = {
        .f_min       = 100.0f,
        .f_max       = 3000.0f,
        .sample_rate = kSampleRate,
        .time_osr    = kTimeOSR,
        .freq_osr    = kFreqOSR,
        .protocol    = FTX_PROTOCOL_FT8,
    };

    monitor_t mon;
    monitor_init(&mon, &mon_cfg);

    /* Feed the slot audio one symbol-block at a time. */
    int frame_pos = 0;
    while (frame_pos + mon.block_size <= nsamples) {
        monitor_process(&mon, audio_12k + frame_pos);
        frame_pos += mon.block_size;
    }

    ftx_candidate_t candidates[kMaxCandidates];
    int num_candidates = ftx_find_candidates(&mon.wf, kMaxCandidates, candidates, kMinScore);
    float noise_db = waterfall_noise_db(&mon.wf);

    uint16_t seen_hash[kMaxDecoded];
    int      num_seen = 0;
    int      count = 0;

    for (int i = 0; i < num_candidates && num_seen < kMaxDecoded && count < max_out; i++) {
        ftx_message_t       msg;
        ftx_decode_status_t status;
        ft8_js8_how         how;
        /* ft8_lib leaves status.freq and status.time unset; these are its
         * demo's formulas: bins from min_bin at 1/symbol_period Hz, blocks
         * of one symbol period, plus the sub-bin and sub-block shifts. */
        const ftx_candidate_t *cand = &candidates[i];
        float freq_hz = (mon.min_bin + cand->freq_offset + (float) cand->freq_sub / mon.wf.freq_osr)
                        / mon.symbol_period;
        float time_s = (cand->time_offset + (float) cand->time_sub / mon.wf.time_osr) * mon.symbol_period
                       - FT8_LIB_TIME_BIAS_S;
        if (!ft8_js8_decode(&mon.wf, cand, kLDPCIters, start_s >= 0.0 ? start_s + time_s : -1.0,
                            freq_hz, &msg, &status, &how))
            continue;

        bool dup = false;
        for (int j = 0; j < num_seen; j++)
            if (seen_hash[j] == msg.hash) { dup = true; break; }
        if (dup)
            continue;
        seen_hash[num_seen++] = msg.hash;

        sbitx_ft8_decode_result *r = &out[count];
        memset(r, 0, sizeof(*r));
        ftx_message_offsets_t offsets;   /* ftx_message_decode dereferences
                                          * this without a NULL check. */
        memset(&offsets, 0xff, sizeof(offsets.offsets));
        if (ftx_message_decode(&msg, NULL, r->text, &offsets) != FTX_MESSAGE_RC_OK)
            continue;
        r->freq_hz = freq_hz;
        r->time_s = time_s;
        r->pass = how.pass;
        r->receptions = how.receptions;
        r->has_snr = estimate_snr(&mon.wf, &candidates[i], &msg, noise_db, &r->snr_db);
        parse_fields(r, &offsets);
        count++;
    }

    monitor_free(&mon);
    return count;
}

/* The decoded messages, newline-separated, in `decoded`; returns the
 * length. Kept for callers that only want the text. */
int sbitx_ft8_decode(float *audio_12k, int nsamples, char *decoded,
                     int max_decoded_len)
{
    sbitx_ft8_decode_result res[50];
    int len = 0;

    if (!decoded || max_decoded_len <= 0)
        return 0;
    decoded[0] = '\0';
    int n = sbitx_ft8_decode_ex(audio_12k, nsamples, res, 50);
    for (int i = 0; i < n; i++) {
        int w = snprintf(decoded + len, max_decoded_len - len,
                         "%s%s", (len > 0 ? "\n" : ""), res[i].text);
        if (w > 0) len += w;
        if (len >= max_decoded_len - 1) break;
    }
    return len;
}

/* The windows slide by 2 s and overlap by 13 s, so one transmission
 * decodes in several of them. It is the same transmission when the text
 * matches, the computed start times agree to within 1.5 s, AND the audio
 * offsets agree to within FT8_SAME_OFFSET_HZ; that holds whether or not
 * the sender is aligned to the UTC slots (HERMES stations transmit when a
 * message is queued, other FT8 stations on the slot).
 *
 * The offset is what tells apart two DIFFERENT stations sending the same
 * text at once -- repeated "73"/"RR73", a beacon, free text. Keying on
 * text and time alone merged them into one entry. Adding the sender
 * callsign would not have helped: `from` is parsed out of the text, so
 * identical text always means an identical `from`. Two simultaneous
 * transmissions must sit at different audio offsets (an FT8 signal is
 * 50 Hz wide), while one transmission decoded in overlapping windows keeps
 * its offset to within a sub-bin (3.125 Hz here). */
#define FT8_RECENT          64
#define FT8_SAME_START_S    1.5
#define FT8_SAME_OFFSET_HZ  10.0f
static pthread_mutex_t ft8_recent_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { char text[48]; double start; float offset_hz; } ft8_recent[FT8_RECENT];
static int ft8_recent_next;

static bool ft8_seen_recently(const char *text, double start, float offset_hz)
{
    for (int i = 0; i < FT8_RECENT; i++)
        if (ft8_recent[i].text[0] &&
            fabs(ft8_recent[i].start - start) < FT8_SAME_START_S &&
            fabsf(ft8_recent[i].offset_hz - offset_hz) < FT8_SAME_OFFSET_HZ &&
            !strcmp(ft8_recent[i].text, text))
            return true;
    snprintf(ft8_recent[ft8_recent_next].text, sizeof(ft8_recent[0].text), "%s", text);
    ft8_recent[ft8_recent_next].start = start;
    ft8_recent[ft8_recent_next].offset_hz = offset_hz;
    ft8_recent_next = (ft8_recent_next + 1) % FT8_RECENT;
    return false;
}

void sbitx_ft8_log_decodes(const sbitx_ft8_decode_result *res, int n,
                           double window_start, uint64_t dial_hz)
{
    pthread_mutex_lock(&ft8_recent_lock);
    for (int i = 0; i < n; i++) {
        const sbitx_ft8_decode_result *r = &res[i];
        double start = window_start + r->time_s;
        if (!r->text[0] || ft8_seen_recently(r->text, start, r->freq_hz))
            continue;
        /* dt as WSJT-X reports it: against 0.5 s into the UTC 15 s slot,
         * folded symmetrically into [-7.5, 7.5) as WSJT-X does. The old
         * fold at +7 left an odd boundary (6.9 stayed, 7.1 became -7.9).
         * HERMES stations are not slot-aligned, so for them this is only an
         * approximation of "how far off the slot". */
        double dt = start - floor(start / 15.0) * 15.0 - 0.5;
        if (dt >= 7.5)
            dt -= 15.0;
        else if (dt < -7.5)
            dt += 15.0;
        digi_log_entry e = {
            .mode = "FT8", .dir = "rx", .freq_hz = dial_hz, .at = start,
            .has_offset = true, .offset_hz = (int) lroundf(r->freq_hz),
            .has_snr = r->has_snr, .snr_db = r->snr_db,
            .has_dt = true, .dt_s = (float) dt,
            .text = r->text,
            .from = r->from[0] ? r->from : NULL,
            .to = r->to[0] ? r->to : NULL,
            .grid = r->grid[0] ? r->grid : NULL,
        };
        digi_log_add(&e);
    }
    pthread_mutex_unlock(&ft8_recent_lock);
}
