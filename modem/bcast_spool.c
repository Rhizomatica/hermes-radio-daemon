/* bcast_spool - see bcast_spool.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "bcast_spool.h"

#include "bcast_link.h"
#include "radiod_log.h"

#define LOG_C "bcast"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SPOOL_MAX_BYTES (2u * 1024u * 1024u)   /* above the codec's 1.44 MB: it says why */
#define SPOOL_POLL_MS   500     /* how often tx is looked at */
#define SPOOL_TICK_MS   100     /* how often a wait checks for shutdown */
#define RX_NAME_TRIES   999     /* "<name>.1" .. ".999" before giving up */
#define SPOOL_DIR_MODE  02770   /* setgid, as the installer makes them */
#define US_PER_MS       1000

static bcast_spool_config g_cfg;

/* The file on the air, for the API (bcast_spool_current). */
static pthread_mutex_t g_cur_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_current[1024];

static void set_current(const char *path)
{
    pthread_mutex_lock(&g_cur_lock);
    snprintf(g_current, sizeof g_current, "%s", path ? path : "");
    pthread_mutex_unlock(&g_cur_lock);
}

bool bcast_spool_current(char *out, size_t cap)
{
    pthread_mutex_lock(&g_cur_lock);
    snprintf(out, cap, "%s", g_current);
    pthread_mutex_unlock(&g_cur_lock);
    return out[0] != '\0';
}
static pthread_t   g_tid;
static atomic_bool g_running;
static bool        g_started;

static const char *base_of(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

int64_t bcast_spool_frames_limit(const char *path)
{
    const char *base = base_of(path);
    const char *suffix = strstr(base, "_frames");
    if (!suffix)
        return -1;
    const char *p = suffix;
    while (p > base && p[-1] >= '0' && p[-1] <= '9')
        p--;
    if (p == suffix || p - 1 <= base || p[-1] != '-')   /* digits, a dash, and a name before it */
        return -1;
    long long n = strtoll(p, NULL, 10);
    return n > 0 ? (int64_t) n : -1;
}

void bcast_spool_send_name(const char *path, char *out, size_t cap)
{
    const char *base = base_of(path);
    const char *suffix = strstr(base, "_frames");
    const char *p = suffix;
    while (p && p > base && p[-1] >= '0' && p[-1] <= '9')
        p--;
    if (!suffix || p == suffix || p - 1 <= base || p[-1] != '-')
    {
        snprintf(out, cap, "%s", base);
        return;
    }
    /* "<stem>-<N>_frames<ext>" -> "<stem><ext>" */
    snprintf(out, cap, "%.*s%s", (int) (p - 1 - base), base, suffix + strlen("_frames"));
}

bool bcast_spool_pick(const char *dir, char *out, size_t cap, time_t *mtime)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    bool found = false;
    time_t best_t = 0;
    char best[256] = "";
    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.')
            continue;
        char path[1024];
        if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int) sizeof path)
            continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (!found || st.st_mtime < best_t ||
            (st.st_mtime == best_t && strcmp(e->d_name, best) < 0))
        {
            found = true;
            best_t = st.st_mtime;
            snprintf(best, sizeof best, "%s", e->d_name);
        }
    }
    closedir(d);
    if (!found)
        return false;
    snprintf(out, cap, "%s/%s", dir, best);
    if (mtime)
        *mtime = best_t;
    return true;
}

bool bcast_spool_rx_target(const char *rx_dir, const char *name, char *out, size_t cap)
{
    char clean[256];
    size_t n = 0;
    for (const char *s = name ? base_of(name) : ""; *s && n < sizeof clean - 1; s++)
        clean[n++] = (*s == '/' || (unsigned char) *s < 0x20) ? '_' : *s;
    clean[n] = '\0';
    while (clean[0] == '.')
        memmove(clean, clean + 1, strlen(clean));
    if (!clean[0])
        snprintf(clean, sizeof clean, "broadcast_%ld.bin", (long) time(NULL));

    struct stat st;
    if (snprintf(out, cap, "%s/%s", rx_dir, clean) >= (int) cap)
        return false;
    for (int i = 1; stat(out, &st) == 0; i++)
    {
        if (i > RX_NAME_TRIES || snprintf(out, cap, "%s/%s.%d", rx_dir, clean, i) >= (int) cap)
            return false;
    }
    return true;
}

void bcast_spool_incoming_dir(const bcast_spool_config *cfg, char *out, size_t cap)
{
    snprintf(out, cap, "%s/.incoming", cfg->rx_dir);
}

void bcast_spool_on_object(const char *path, const char *name, void *user)
{
    (void) user;
    char target[1024];
    if (!bcast_spool_rx_target(g_cfg.rx_dir, name, target, sizeof target))
    {
        HLOGW(LOG_C, "no name in %s for \"%s\"; left in %s", g_cfg.rx_dir, name, path);
        return;
    }
    if (rename(path, target) != 0)
        HLOGW(LOG_C, "cannot move %s to %s: %s", path, target, strerror(errno));
    else
        HLOGI(LOG_C, "received %s", target);
}

static uint8_t *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *buf = malloc(SPOOL_MAX_BYTES);
    size_t n = buf ? fread(buf, 1, SPOOL_MAX_BYTES, f) : 0;
    fclose(f);
    if (!buf || n == 0)
    {
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

static void set_aside(const char *path, const char *why)
{
    char failed[2200];
    const char *base = base_of(path);
    if (snprintf(failed, sizeof failed, "%.*s.%s.failed", (int) (base - path), path, base)
        >= (int) sizeof failed)
    {
        unlink(path);
        return;
    }
    HLOGW(LOG_C, "cannot broadcast %s (%s); renamed to %s", path, why, failed);
    if (rename(path, failed) != 0)
        unlink(path);
}

static void *spool_thread(void *arg)
{
    (void) arg;
    char current[1024] = "";
    time_t current_mtime = 0;

    while (atomic_load(&g_running))
    {
        bcast_tx_stats st;
        bcast_link_stats(&st);

        if (current[0])
        {
            struct stat sb;
            if (st.state == BCAST_TX_DONE)
            {
                unlink(current);
                current[0] = '\0';
            }
            else if (st.state == BCAST_TX_ERROR)
            {
                set_aside(current, "the carousel failed");
                current[0] = '\0';
            }
            else if (stat(current, &sb) != 0)
            {
                HLOGI(LOG_C, "%s was removed; stopping it", current);
                bcast_link_stop_object();
                current[0] = '\0';
            }
            else if (sb.st_mtime != current_mtime)
            {
                HLOGI(LOG_C, "%s changed; starting it over", current);
                bcast_link_stop_object();
                current[0] = '\0';
            }
            else if (st.state != BCAST_TX_SENDING)
                current[0] = '\0';   /* stopped from elsewhere: the next pick takes it again */
        }

        if (!current[0] && st.state != BCAST_TX_SENDING)
        {
            char path[1024];
            time_t mtime;
            if (bcast_spool_pick(g_cfg.tx_dir, path, sizeof path, &mtime))
            {
                size_t len = 0;
                uint8_t *data = read_all(path, &len);
                if (!data)
                    set_aside(path, "empty or unreadable");
                else
                {
                    char name[256], err[256] = "";
                    bcast_spool_send_name(path, name, sizeof name);
                    int rc = bcast_link_send_object(name, data, len, 0,
                                                    bcast_spool_frames_limit(path), err, sizeof err);
                    if (rc == 0)
                    {
                        snprintf(current, sizeof current, "%s", path);
                        current_mtime = mtime;
                        set_current(current);
                    }
                    else if (rc == EINVAL)
                        set_aside(path, err);
                    free(data);
                }
            }
        }

        if (!current[0])
            set_current(NULL);
        for (int i = 0; i < SPOOL_POLL_MS / SPOOL_TICK_MS && atomic_load(&g_running); i++)
            usleep(SPOOL_TICK_MS * US_PER_MS);
    }
    return NULL;
}

bool bcast_spool_start(const bcast_spool_config *cfg)
{
    if (g_started)
        return true;
    g_cfg = *cfg;
    mkdir(g_cfg.tx_dir, SPOOL_DIR_MODE);
    mkdir(g_cfg.rx_dir, SPOOL_DIR_MODE);
    atomic_store(&g_running, true);
    if (pthread_create(&g_tid, NULL, spool_thread, NULL) != 0)
    {
        atomic_store(&g_running, false);
        return false;
    }
    g_started = true;
    HLOGI(LOG_C, "spool: sending from %s, receiving into %s", g_cfg.tx_dir, g_cfg.rx_dir);
    return true;
}

void bcast_spool_stop(void)
{
    if (!g_started)
        return;
    atomic_store(&g_running, false);
    pthread_join(g_tid, NULL);
    g_started = false;
}
