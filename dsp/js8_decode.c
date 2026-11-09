/* js8_decode - JS8 receive for the radio paths.  See js8_decode.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "js8_decode.h"

#include <math.h>
#include <pthread.h>
#include <string.h>

#include "digi_log.h"
#include "js8_msg.h"
#include "js8_rx.h"

#define JS8_MAX_DECODES   50
#define JS8_RECENT        64      /* transmissions remembered, to log each once */
#define JS8_SAME_START_S  1.5     /* the same transmission in two windows: as FT8's */
#define JS8_SAME_OFFSET_HZ 10.0f

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char   frame[JS8_CHARS + 1];
    double start;
    float  offset_hz;
} g_recent[JS8_RECENT];
static int g_recent_next;

/* Whether this frame, at this time and offset, was logged already; if
 * not, it is remembered. */
static bool seen_recently(const char *frame, double start, float offset_hz)
{
    for (int i = 0; i < JS8_RECENT; i++)
        if (g_recent[i].frame[0] && !strcmp(g_recent[i].frame, frame) &&
            fabs(g_recent[i].start - start) < JS8_SAME_START_S &&
            fabsf(g_recent[i].offset_hz - offset_hz) < JS8_SAME_OFFSET_HZ)
            return true;
    strcpy(g_recent[g_recent_next].frame, frame);
    g_recent[g_recent_next].start = start;
    g_recent[g_recent_next].offset_hz = offset_hz;
    g_recent_next = (g_recent_next + 1) % JS8_RECENT;
    return false;
}

void js8_decode_and_log(const float *audio_12k, int nsamples, double window_start,
                        uint64_t dial_hz)
{
    js8_rx_result res[JS8_MAX_DECODES];
    int n = js8_rx_decode(audio_12k, nsamples, res, JS8_MAX_DECODES);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < n; i++) {
        const js8_rx_result *r = &res[i];
        double start = window_start + r->time_s;
        if (seen_recently(r->frame, start, r->freq_hz))
            continue;
        js8_msg m;
        js8_msg_unpack(r->frame, r->type, &m);
        digi_log_entry e = {
            .mode = "JS8", .dir = "rx", .freq_hz = dial_hz, .at = start,
            .has_offset = true, .offset_hz = (int) lroundf(r->freq_hz),
            .has_snr = r->has_snr, .snr_db = r->snr_db,
            .text = m.text,
            .from = m.from[0] ? m.from : NULL,
            .to = m.to[0] ? m.to : NULL,
            .grid = m.grid[0] ? m.grid : NULL,
        };
        digi_log_add(&e);
    }
    pthread_mutex_unlock(&g_lock);
}
