/* band_clock - the station clock's error, as the FT8 and JS8 stations it
 * hears show it.  See band_clock.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "band_clock.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hermes_log.h"

#define LOG_C "dsp"

typedef struct {
    double at_s;
    double offset_s;
} decode;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static decode g_ring[BAND_CLOCK_MAX_DECODES];
static int    g_count;       /* entries in g_ring */
static int    g_next;        /* where the next one goes */
static bool   g_warned;      /* the last log line was the warning */

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *) a, y = *(const double *) b;
    return (x > y) - (x < y);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* The estimate from what is in the ring; called with g_lock held. */
static bool estimate_locked(double now, double *offset_s, int *decodes)
{
    double newest = -INFINITY;
    for (int i = 0; i < g_count; i++)
        if (g_ring[i].at_s > newest)
            newest = g_ring[i].at_s;
    if (g_count == 0 || now - newest > BAND_CLOCK_KEEP_S)
        return false;

    static double off[BAND_CLOCK_MAX_DECODES];
    int n = 0;
    for (int i = 0; i < g_count; i++)
        if (newest - g_ring[i].at_s <= BAND_CLOCK_WINDOW_S)
            off[n++] = g_ring[i].offset_s;
    if (n < BAND_CLOCK_MIN_DECODES)
        return false;
    qsort(off, (size_t) n, sizeof(off[0]), cmp_double);

    /* The largest run of sorted offsets within BAND_CLOCK_CLUSTER_S. */
    int best_lo = 0, best_n = 0;
    for (int lo = 0, hi = 0; lo < n; lo++) {
        if (hi < lo)
            hi = lo;
        while (hi + 1 < n && off[hi + 1] - off[lo] <= BAND_CLOCK_CLUSTER_S)
            hi++;
        if (hi - lo + 1 > best_n) {
            best_n = hi - lo + 1;
            best_lo = lo;
        }
    }
    if (best_n < BAND_CLOCK_MIN_DECODES || best_n * BAND_CLOCK_MIN_SHARE < n)
        return false;

    const double *c = off + best_lo;
    *offset_s = best_n % 2 ? c[best_n / 2] : (c[best_n / 2 - 1] + c[best_n / 2]) / 2.0;
    *decodes = best_n;
    return true;
}

void band_clock_add(double offset_s, double at_s)
{
    if (!isfinite(offset_s) || !isfinite(at_s))
        return;
    pthread_mutex_lock(&g_lock);
    g_ring[g_next] = (decode) { .at_s = at_s, .offset_s = offset_s };
    g_next = (g_next + 1) % BAND_CLOCK_MAX_DECODES;
    if (g_count < BAND_CLOCK_MAX_DECODES)
        g_count++;

    double e;
    int n;
    if (estimate_locked(at_s, &e, &n)) {
        if (!g_warned && fabs(e) > BAND_CLOCK_WARN_S) {
            g_warned = true;
            HLOGW(LOG_C, "clock: %d FT8/JS8 decodes put this station's clock %.1f s %s",
                  n, fabs(e), e > 0 ? "fast" : "slow");
        } else if (g_warned && fabs(e) < BAND_CLOCK_OK_S) {
            g_warned = false;
            HLOGI(LOG_C, "clock: %d FT8/JS8 decodes put this station's clock within %.1f s again",
                  n, BAND_CLOCK_OK_S);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

bool band_clock_estimate(double now, double *offset_s, int *decodes)
{
    pthread_mutex_lock(&g_lock);
    bool ok = estimate_locked(now, offset_s, decodes);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

bool band_clock_get(double *offset_s, int *decodes)
{
    return band_clock_estimate(now_s(), offset_s, decodes);
}

void band_clock_reset(void)
{
    pthread_mutex_lock(&g_lock);
    memset(g_ring, 0, sizeof(g_ring));
    g_count = g_next = 0;
    g_warned = false;
    pthread_mutex_unlock(&g_lock);
}
