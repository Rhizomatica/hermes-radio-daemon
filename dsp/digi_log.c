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

/* Two locks, always taken in this order (g_file_lock, then g_lock):
 *
 *   g_file_lock  serialises writers' appends and the rotation, and covers
 *                a reader only while it opens the two files. Never held
 *                across a read.
 *   g_lock       the ring, the ids and the settings; held only for memory
 *                work, never across any I/O.
 *
 * digi_messages used to read up to ~2 MiB of log from the SD card while
 * holding the one lock every decoder needs to log, so a single request
 * could stall the DSP thread for tens of milliseconds -- longer than the
 * codec's ~21 ms of headroom. */
static pthread_mutex_t g_file_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char     g_dir[256] = DIGI_LOG_DEFAULT_DIR;
static long     g_max_bytes = 1024 * 1024;
static bool     g_dir_made;
static bool     g_id_seeded;
static uint64_t g_last_id;

static struct {
    uint64_t id;
    char     json[DIGI_LOG_LINE_MAX];
} g_ring[DIGI_LOG_RING];
static int g_ring_next;    /* slot the next entry goes to */
static int g_ring_count;

void digi_log_set_dir(const char *dir)
{
    pthread_mutex_lock(&g_file_lock);   /* file paths are read under it */
    pthread_mutex_lock(&g_lock);
    snprintf(g_dir, sizeof(g_dir), "%s", dir && dir[0] ? dir : DIGI_LOG_DEFAULT_DIR);
    g_dir_made = false;
    g_id_seeded = false;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_file_lock);
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

static const char *const g_modes[] = { "FT8", "CW", "RTTY", "DSTAR", NULL };
static const char *const g_dirs[]  = { "rx", "tx", NULL };

/* The matching entry of `allowed`, or NULL. Returning the table's own
 * string means nothing from the caller reaches the output unescaped. */
static const char *known(const char *s, const char *const *allowed)
{
    for (int i = 0; s && allowed[i]; i++)
        if (!strcmp(s, allowed[i]))
            return allowed[i];
    return NULL;
}

size_t digi_log_format(const digi_log_entry *e, uint64_t id, double at, char *out, size_t out_size)
{
    jw w = { out, out_size, 0, false };

    /* An unknown mode or direction is refused, not relabelled: falling back
     * to "FT8"/"rx" would log a future mode (RADE, PSK...) or a typo as
     * FT8 traffic with no error anywhere. */
    const char *mode = known(e->mode, g_modes);
    const char *dir  = known(e->dir, g_dirs);
    if (!mode || !dir) {
        if (out_size) out[0] = '\0';
        return 0;
    }

    time_t secs = (time_t) floor(at);
    int ms = (int) ((at - floor(at)) * 1000.0);
    if (ms > 999) ms = 999;
    struct tm tm;
    gmtime_r(&secs, &tm);

    jw_raw(&w, "{\"id\":%llu,\"at\":\"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ\",\"mode\":\"%s\",\"dir\":\"%s\","
               "\"frequency_hz\":%llu",
           (unsigned long long) id, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec, ms, mode, dir,
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

double digi_log_now(void)
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

/* The id of the last complete entry in `fd`'s file, 0 if none. Only the
 * tail is read: an entry is at most DIGI_LOG_LINE_MAX bytes. */
static uint64_t last_id_in(int fd)
{
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0)
        return 0;
    size_t win = 4 * DIGI_LOG_LINE_MAX;
    if ((off_t) win > st.st_size)
        win = (size_t) st.st_size;
    char buf[4 * DIGI_LOG_LINE_MAX + 1];
    ssize_t got = pread(fd, buf, win, st.st_size - (off_t) win);
    if (got <= 0)
        return 0;
    buf[got] = '\0';
    /* Walk complete lines from the end; the first one may be a fragment
     * when the window does not start at the top of the file, and parsing
     * it would need it to start with the key, which a fragment won't. */
    for (char *end = buf + got; end > buf; ) {
        while (end > buf && (end[-1] == '\n' || end[-1] == '\r'))
            end--;
        char *start = end;
        while (start > buf && start[-1] != '\n')
            start--;
        static const char key[] = "{\"id\":";
        if ((size_t) (end - start) > sizeof(key) && !strncmp(start, key, sizeof(key) - 1)) {
            uint64_t id = strtoull(start + sizeof(key) - 1, NULL, 10);
            if (id > 0)
                return id;
        }
        end = start;
    }
    return 0;
}

/* Continue the id sequence from what is already on disk. Without this,
 * g_last_id restarted at 0 every run and ids came from the wall clock
 * alone, so a Pi with no RTC booting behind its previous run (before NTP,
 * or after NTP stepped the clock back) wrote ids lower than ones already
 * in the file, and a "since id" reader skipped or misordered them. Called
 * with g_file_lock held. */
static void seed_last_id(void)
{
    char path[320];
    uint64_t on_disk = 0;
    static const char *const names[] = { "digi.jsonl", "digi.jsonl.1" };
    for (size_t i = 0; i < 2 && on_disk == 0; i++) {
        path_of(path, sizeof(path), names[i]);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            on_disk = last_id_in(fd);
            close(fd);
        }
    }
    pthread_mutex_lock(&g_lock);
    if (on_disk > g_last_id)
        g_last_id = on_disk;
    g_id_seeded = true;
    pthread_mutex_unlock(&g_lock);
}

void digi_log_add(const digi_log_entry *e)
{
    if (!e)
        return;
    if (!known(e->mode, g_modes) || !known(e->dir, g_dirs)) {
        static bool warned;
        if (!warned) {
            fprintf(stderr, "digi_log: refusing an entry with mode \"%s\" dir \"%s\" "
                    "(not one the log schema defines)\n",
                    e->mode ? e->mode : "(null)", e->dir ? e->dir : "(null)");
            warned = true;
        }
        return;
    }
    double at = e->at > 0 ? e->at : digi_log_now();
    char line[DIGI_LOG_LINE_MAX + 1];

    /* The file lock is taken first and held across this writer's append,
     * so entries reach the file in id order. Only other writers wait on it;
     * readers take it just long enough to open the files. */
    pthread_mutex_lock(&g_file_lock);
    if (!g_dir_made) {
        mkdir(g_dir, 0755);
        g_dir_made = true;
    }
    if (!g_id_seeded)
        seed_last_id();

    pthread_mutex_lock(&g_lock);
    /* Ids are the wall clock at log time, in microseconds, forced strictly
     * increasing and continued from the file (seed_last_id), so they stay
     * unique and ordered across restarts. For FT8 RX `at` is the
     * transmission's start, up to ~13 s before the id's time: ids order the
     * log, `at` says when the message was on the air. */
    uint64_t id = (uint64_t) (digi_log_now() * 1e6);
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
    }
    pthread_mutex_unlock(&g_lock);

    if (len > 0) {
        line[len] = '\n';
        rotate_if_needed();
        append_line("digi.jsonl", line, len + 1);
    }
    append_legacy(e);
    pthread_mutex_unlock(&g_file_lock);
}

void digi_log_dstar_header(const uint8_t *header, size_t header_len, const char *dir,
                           uint64_t freq_hz, bool encrypted, bool crc_ok)
{
    if (!header || header_len < DIGI_LOG_DSTAR_HEADER_MIN)
        return;
    /* Raw callsign bytes; digi_log_format sanitises them like any text from
     * the air, so a failed header cannot inject anything. */
    char rpt1[9], rpt2[9], urcall[9], mycall[9], suffix[5];
    memcpy(rpt1,   header + 3,  8); rpt1[8]   = '\0';
    memcpy(rpt2,   header + 11, 8); rpt2[8]   = '\0';
    memcpy(urcall, header + 19, 8); urcall[8] = '\0';
    memcpy(mycall, header + 27, 8); mycall[8] = '\0';
    memcpy(suffix, header + 35, 4); suffix[4] = '\0';

    digi_log_dstar hdr = {
        .mycall = mycall, .suffix = suffix, .urcall = urcall,
        .rpt1 = rpt1, .rpt2 = rpt2, .encrypted = encrypted, .crc_ok = crc_ok,
    };
    digi_log_entry e = {
        .mode = "DSTAR", .dir = dir, .freq_hz = freq_hz, .text = "",
        .from = crc_ok ? mycall : NULL, .to = crc_ok ? urcall : NULL, .dstar = &hdr,
    };
    digi_log_add(&e);
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

/* pread until `n` bytes or end of file. */
static ssize_t pread_full(int fd, char *buf, size_t n, off_t off)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, buf + got, n - got, off + (off_t) got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        got += (size_t) r;
    }
    return (ssize_t) got;
}

/* Take up to `want` entries from the end of `fd`'s file into `out`, reading
 * only as much of the tail as that needs: a 64 KiB window first, doubled
 * while it holds too few complete entries. digi_messages used to read the
 * whole of both files (up to ~2 MiB, and ~3 MiB of buffers after doubling)
 * to return at most 100 entries. */
static int take_tail(int fd, int want, char **out, size_t *out_len, size_t *out_cap)
{
    struct stat st;
    if (fd < 0 || want <= 0 || fstat(fd, &st) != 0 || st.st_size <= 0)
        return 0;

    const size_t base_len = *out_len;
    size_t win = 65536;
    for (;;) {
        if ((off_t) win > st.st_size)
            win = (size_t) st.st_size;
        const off_t off = st.st_size - (off_t) win;
        char *buf = malloc(win + 1);
        if (!buf)
            return 0;
        ssize_t got = pread_full(fd, buf, win, off);
        if (got < 0) {
            free(buf);
            return 0;
        }
        size_t blen = (size_t) got;
        char *p = buf;
        /* A window that does not start at the top of the file begins with
         * the tail of some line. Drop it: it can end in '}' and even start
         * with '{' (the nested dstar object), so it would pass for an entry. */
        if (off > 0) {
            char *nl = memchr(buf, '\n', blen);
            blen = nl ? blen - (size_t) (nl + 1 - buf) : 0;
            p = nl ? nl + 1 : buf;
        }
        *out_len = base_len;            /* a retry starts the copy over */
        int taken = take_from_end(p, blen, want, out, out_len, out_cap);
        free(buf);
        if (taken >= want || off == 0)
            return taken;
        win *= 2;                       /* too few complete entries yet */
    }
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

    /* Open both files together, under the file lock, so a rotation cannot
     * land between the two opens and the pair is consistent. The reads then
     * run with no lock at all: appends are single write() calls and a
     * rotation is a rename, so an open descriptor stays valid either way. */
    char path[320];
    pthread_mutex_lock(&g_file_lock);
    path_of(path, sizeof(path), "digi.jsonl");
    int cur = open(path, O_RDONLY | O_CLOEXEC);
    path_of(path, sizeof(path), "digi.jsonl.1");
    int prev = open(path, O_RDONLY | O_CLOEXEC);
    pthread_mutex_unlock(&g_file_lock);

    int taken = take_tail(cur, count, &out, &len, &cap);
    if (taken < count)
        take_tail(prev, count - taken, &out, &len, &cap);
    if (cur >= 0) close(cur);
    if (prev >= 0) close(prev);

    if (len + 2 > cap) {
        char *p = realloc(out, len + 2);
        if (!p) { free(out); return NULL; }
        out = p;
    }
    out[len++] = ']';
    out[len] = '\0';
    return out;
}
