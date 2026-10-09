/* digi_log - see digi_log.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "digi_log.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DIGI_LOG_DEFAULT_DIR "/var/spool/hermes-digi"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char     g_dir[256] = DIGI_LOG_DEFAULT_DIR;
static long     g_max_bytes = 1024 * 1024;
static bool     g_dir_made;
static uint64_t g_last_id;

static struct {
    uint64_t id;
    char     json[DIGI_LOG_LINE_MAX];
} g_ring[DIGI_LOG_RING];
static int g_ring_next;    /* slot the next entry goes to */
static int g_ring_count;

void digi_log_set_dir(const char *dir)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_dir, sizeof(g_dir), "%s", dir && dir[0] ? dir : DIGI_LOG_DEFAULT_DIR);
    g_dir_made = false;
    pthread_mutex_unlock(&g_lock);
}

void digi_log_set_max_bytes(long max_bytes)
{
    pthread_mutex_lock(&g_lock);
    g_max_bytes = max_bytes > 4096 ? max_bytes : 4096;
    pthread_mutex_unlock(&g_lock);
}

/* Printable ASCII only: every mode logged here is ASCII on the air, and
 * anything else came from noise, a failed CRC or someone injecting markup
 * or control sequences. Tabs become spaces, runs of spaces stay, the ends
 * are trimmed. Returns the length written. */
size_t digi_log_sanitize(const char *in, char *out, size_t out_size, size_t max_chars)
{
    size_t n = 0;
    if (!out || out_size == 0)
        return 0;
    if (max_chars > out_size - 1)
        max_chars = out_size - 1;
    for (const unsigned char *p = (const unsigned char *) (in ? in : ""); *p && n < max_chars; p++) {
        unsigned char c = *p;
        if (c == '\t')
            c = ' ';
        if (c < 0x20 || c > 0x7e)
            continue;
        if (c == ' ' && n == 0)
            continue;               /* leading blanks */
        out[n++] = (char) c;
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
    return n;
}

/* Append-only writer for one JSON object into a fixed buffer. On overflow
 * it stops writing and marks itself failed, so a truncated object is never
 * emitted. */
typedef struct {
    char  *buf;
    size_t size;
    size_t len;
    bool   failed;
} jw;

static void jw_raw(jw *w, const char *fmt, ...)
{
    if (w->failed)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->len, w->size - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t) n >= w->size - w->len) {
        w->failed = true;
        return;
    }
    w->len += (size_t) n;
}

/* A string value, sanitised and escaped. After sanitising only `"` and `\`
 * need escaping. */
static void jw_str(jw *w, const char *key, const char *value, size_t max_chars, bool comma)
{
    char clean[DIGI_LOG_TEXT_MAX + 1];
    digi_log_sanitize(value, clean, sizeof(clean), max_chars);
    jw_raw(w, "%s\"%s\":\"", comma ? "," : "", key);
    for (const char *p = clean; *p && !w->failed; p++) {
        if (*p == '"' || *p == '\\')
            jw_raw(w, "\\%c", *p);
        else
            jw_raw(w, "%c", *p);
    }
    jw_raw(w, "\"");
}

/* A string that may be absent: null when empty after sanitising. */
static void jw_opt_str(jw *w, const char *key, const char *value, size_t max_chars)
{
    char clean[DIGI_LOG_CALL_MAX + 1];
    if (value && digi_log_sanitize(value, clean, sizeof(clean), max_chars) > 0)
        jw_str(w, key, clean, max_chars, true);
    else
        jw_raw(w, ",\"%s\":null", key);
}

static const char *known(const char *s, const char *const *allowed, const char *fallback)
{
    for (int i = 0; s && allowed[i]; i++)
        if (!strcmp(s, allowed[i]))
            return allowed[i];
    return fallback;
}

size_t digi_log_format(const digi_log_entry *e, uint64_t id, double at, char *out, size_t out_size)
{
    static const char *const modes[] = { "FT8", "CW", "RTTY", "DSTAR", NULL };
    static const char *const dirs[]  = { "rx", "tx", NULL };
    jw w = { out, out_size, 0, false };

    time_t secs = (time_t) floor(at);
    int ms = (int) ((at - floor(at)) * 1000.0);
    if (ms > 999) ms = 999;
    struct tm tm;
    gmtime_r(&secs, &tm);

    const char *mode = known(e->mode, modes, "FT8");
    jw_raw(&w, "{\"id\":%llu,\"at\":\"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ\",\"mode\":\"%s\",\"dir\":\"%s\","
               "\"frequency_hz\":%llu",
           (unsigned long long) id, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec, ms, mode, known(e->dir, dirs, "rx"),
           (unsigned long long) e->freq_hz);
    if (e->has_offset) jw_raw(&w, ",\"offset_hz\":%d", e->offset_hz);
    else               jw_raw(&w, ",\"offset_hz\":null");
    if (e->has_snr && isfinite(e->snr_db)) jw_raw(&w, ",\"snr_db\":%.0f", e->snr_db);
    else                                   jw_raw(&w, ",\"snr_db\":null");
    if (e->has_dt && isfinite(e->dt_s))    jw_raw(&w, ",\"dt_s\":%.1f", e->dt_s);
    else                                   jw_raw(&w, ",\"dt_s\":null");
    jw_str(&w, "text", e->text ? e->text : "", DIGI_LOG_TEXT_MAX, true);
    jw_opt_str(&w, "from", e->from, DIGI_LOG_CALL_MAX);
    jw_opt_str(&w, "to", e->to, DIGI_LOG_CALL_MAX);
    jw_opt_str(&w, "grid", e->grid, 8);
    if (!strcmp(mode, "DSTAR") && e->dstar) {
        const digi_log_dstar *d = e->dstar;
        jw_raw(&w, ",\"dstar\":{");
        jw_str(&w, "mycall", d->mycall, 8, false);
        jw_str(&w, "suffix", d->suffix, 4, true);
        jw_str(&w, "urcall", d->urcall, 8, true);
        jw_str(&w, "rpt1", d->rpt1, 8, true);
        jw_str(&w, "rpt2", d->rpt2, 8, true);
        jw_raw(&w, ",\"encrypted\":%s,\"crc_ok\":%s}",
               d->encrypted ? "true" : "false", d->crc_ok ? "true" : "false");
    } else {
        jw_raw(&w, ",\"dstar\":null");
    }
    jw_raw(&w, "}");
    if (w.failed) {
        if (out_size) out[0] = '\0';
        return 0;
    }
    return w.len;
}

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double) tv.tv_sec + tv.tv_usec / 1e6;
}

static void path_of(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", g_dir, name);
}

static void append_line(const char *name, const char *line, size_t len)
{
    char path[320];
    path_of(path, sizeof(path), name);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    /* One write per line: a reader never sees half an entry from us. */
    ssize_t r = write(fd, line, len);
    (void) r;
    close(fd);
}

/* The old format, for one release: "<MODE> <dir> <MHz>.<kHz>: <text>". */
static void append_legacy(const digi_log_entry *e)
{
    char text[DIGI_LOG_TEXT_MAX + 1], line[DIGI_LOG_TEXT_MAX + 96];
    int n;
    if (e->mode && !strcmp(e->mode, "DSTAR") && e->dstar) {
        char ur[9], my[9];
        digi_log_sanitize(e->dstar->urcall, ur, sizeof(ur), 8);
        digi_log_sanitize(e->dstar->mycall, my, sizeof(my), 8);
        n = snprintf(line, sizeof(line), "DSTAR %s header: ur=%s my=%s\n",
                     e->dir ? e->dir : "rx", ur, my);
    } else {
        unsigned khz = (unsigned) (e->freq_hz / 1000);
        digi_log_sanitize(e->text, text, sizeof(text), DIGI_LOG_TEXT_MAX);
        n = snprintf(line, sizeof(line), "%s %s %u.%03u: %s\n",
                     e->mode ? e->mode : "?", e->dir ? e->dir : "rx",
                     khz / 1000, khz % 1000, text);
    }
    if (n > 0)
        append_line("spool.log", line, (size_t) n < sizeof(line) ? (size_t) n : sizeof(line) - 1);
}

static void rotate_if_needed(void)
{
    char path[320], old[320];
    struct stat st;
    path_of(path, sizeof(path), "digi.jsonl");
    if (stat(path, &st) != 0 || st.st_size < g_max_bytes)
        return;
    path_of(old, sizeof(old), "digi.jsonl.1");
    rename(path, old);              /* keeps two files: current and .1 */
}

void digi_log_add(const digi_log_entry *e)
{
    if (!e)
        return;
    double at = e->at > 0 ? e->at : now_s();
    char line[DIGI_LOG_LINE_MAX + 1];

    pthread_mutex_lock(&g_lock);
    if (!g_dir_made) {
        mkdir(g_dir, 0755);
        g_dir_made = true;
    }
    /* Ids are microseconds since the epoch, strictly increasing, so they
     * stay unique and ordered across restarts. */
    uint64_t id = (uint64_t) (now_s() * 1e6);
    if (id <= g_last_id)
        id = g_last_id + 1;

    /* At most DIGI_LOG_LINE_MAX - 1 characters, so the line fits a ring
     * slot and the newline still fits `line`. */
    size_t len = digi_log_format(e, id, at, line, DIGI_LOG_LINE_MAX);
    if (len > 0) {
        g_last_id = id;
        memcpy(g_ring[g_ring_next].json, line, len + 1);
        g_ring[g_ring_next].id = id;
        g_ring_next = (g_ring_next + 1) % DIGI_LOG_RING;
        if (g_ring_count < DIGI_LOG_RING)
            g_ring_count++;

        line[len] = '\n';
        rotate_if_needed();
        append_line("digi.jsonl", line, len + 1);
    }
    append_legacy(e);
    pthread_mutex_unlock(&g_lock);
}

uint64_t digi_log_latest_id(void)
{
    pthread_mutex_lock(&g_lock);
    uint64_t id = g_last_id;
    pthread_mutex_unlock(&g_lock);
    return id;
}

int digi_log_since(uint64_t after, char lines[][DIGI_LOG_LINE_MAX], uint64_t *ids, int max)
{
    int n = 0;
    pthread_mutex_lock(&g_lock);
    int first = (g_ring_next - g_ring_count + DIGI_LOG_RING) % DIGI_LOG_RING;
    for (int k = 0; k < g_ring_count && n < max; k++) {
        int i = (first + k) % DIGI_LOG_RING;
        if (g_ring[i].id <= after)
            continue;
        snprintf(lines[n], DIGI_LOG_LINE_MAX, "%s", g_ring[i].json);
        if (ids)
            ids[n] = g_ring[i].id;
        n++;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* Read a whole file (the log is capped, so this is at most about 1 MiB). */
static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    *len = 0;
    if (!f)
        return NULL;
    char *buf = NULL;
    size_t cap = 0;
    for (;;) {
        if (cap - *len < 65536) {
            size_t ncap = cap ? cap * 2 : 65536;
            char *p = realloc(buf, ncap + 1);
            if (!p) { free(buf); fclose(f); *len = 0; return NULL; }
            buf = p;
            cap = ncap;
        }
        size_t r = fread(buf + *len, 1, cap - *len, f);
        *len += r;
        if (r == 0)
            break;
    }
    fclose(f);
    buf[*len] = '\0';
    return buf;
}

/* Walk `buf` from its end, appending complete JSON-object lines to `out`
 * until `want` are taken. Lines not starting with '{' (a partial line from
 * a crash, or garbage) are skipped. */
static int take_from_end(char *buf, size_t len, int want, char **out, size_t *out_len, size_t *out_cap)
{
    int taken = 0;
    size_t end = len;
    while (end > 0 && taken < want) {
        while (end > 0 && (buf[end - 1] == '\n' || buf[end - 1] == '\r'))
            end--;
        if (end == 0)
            break;
        size_t start = end;
        while (start > 0 && buf[start - 1] != '\n')
            start--;
        size_t n = end - start;
        if (n > 1 && buf[start] == '{' && buf[end - 1] == '}') {
            if (*out_len + n + 2 >= *out_cap) {
                size_t ncap = (*out_cap + n + 2) * 2;
                char *p = realloc(*out, ncap);
                if (!p)
                    return taken;
                *out = p;
                *out_cap = ncap;
            }
            if (*out_len > 1)
                (*out)[(*out_len)++] = ',';
            memcpy(*out + *out_len, buf + start, n);
            *out_len += n;
            taken++;
        }
        end = start;
    }
    return taken;
}

char *digi_log_last_json(int count)
{
    if (count < 1) count = 1;
    if (count > 1000) count = 1000;

    size_t cap = 4096, len = 1;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    out[0] = '[';

    char path[320];
    pthread_mutex_lock(&g_lock);
    path_of(path, sizeof(path), "digi.jsonl");
    size_t blen;
    char *buf = slurp(path, &blen);
    int taken = buf ? take_from_end(buf, blen, count, &out, &len, &cap) : 0;
    free(buf);
    if (taken < count) {
        path_of(path, sizeof(path), "digi.jsonl.1");
        buf = slurp(path, &blen);
        if (buf)
            take_from_end(buf, blen, count - taken, &out, &len, &cap);
        free(buf);
    }
    pthread_mutex_unlock(&g_lock);

    if (len + 2 > cap) {
        char *p = realloc(out, len + 2);
        if (!p) { free(out); return NULL; }
        out = p;
    }
    out[len++] = ']';
    out[len] = '\0';
    return out;
}
