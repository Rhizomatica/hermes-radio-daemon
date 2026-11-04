/* dream_proc - a Dream (DRM) process behind two pipes
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE                     /* pipe2 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dream_proc.h"
#include "radiod_log.h"

#define LOG_C "drm"

typedef struct {
    int16_t *buf;
    size_t   cap, head, len;        /* head: the oldest sample */
    uint64_t dropped;
} ring;

struct dream_proc {
    pid_t pid;
    int in_fd, out_fd;              /* Dream's stdin, its stdout */
    pthread_t writer, reader;
    pthread_mutex_t lock;
    pthread_cond_t  more;           /* the writer has samples, or is to end */
    ring in, out;
    bool finish, stop, exited;
    uint8_t odd;                    /* a byte of a sample cut by a read */
    bool has_odd;
};

static bool ring_init(ring *r, size_t cap)
{
    r->buf = calloc(cap, sizeof(int16_t));
    r->cap = cap;
    r->head = r->len = 0;
    r->dropped = 0;
    return r->buf != NULL;
}

/* as many of s as fit; the rest dropped */
static size_t ring_put(ring *r, const int16_t *s, size_t n)
{
    size_t room = r->cap - r->len, k = n < room ? n : room;

    for (size_t i = 0; i < k; i++)
        r->buf[(r->head + r->len + i) % r->cap] = s[i];
    r->len += k;
    r->dropped += n - k;
    return k;
}

static size_t ring_get(ring *r, int16_t *s, size_t n)
{
    size_t k = n < r->len ? n : r->len;

    for (size_t i = 0; i < k; i++)
        s[i] = r->buf[(r->head + i) % r->cap];
    r->head = (r->head + k) % r->cap;
    r->len -= k;
    return k;
}

/* What the DSP queued, to Dream's stdin; at finish, once it has all gone,
 * stdin is closed: Dream sees the end of its input. */
static void *writer_main(void *arg)
{
    dream_proc *p = arg;
    int16_t chunk[4096];

    for (;;) {
        pthread_mutex_lock(&p->lock);
        while (!p->in.len && !p->finish && !p->stop)
            pthread_cond_wait(&p->more, &p->lock);
        if (p->stop || (!p->in.len && p->finish)) {
            pthread_mutex_unlock(&p->lock);
            break;
        }
        size_t n = ring_get(&p->in, chunk, sizeof(chunk) / sizeof(chunk[0]));
        pthread_mutex_unlock(&p->lock);

        const uint8_t *b = (const uint8_t *) chunk;
        size_t left = n * sizeof(int16_t);
        while (left) {
            ssize_t w = write(p->in_fd, b, left);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                HLOGW(LOG_C, "Dream's input closed: %s", w < 0 ? strerror(errno) : "EOF");
                goto out;
            }
            b += w;
            left -= (size_t) w;
        }
    }
out:
    pthread_mutex_lock(&p->lock);
    if (p->in_fd >= 0) {
        close(p->in_fd);
        p->in_fd = -1;
    }
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

/* Dream's stdout into the out ring, until it ends */
static void *reader_main(void *arg)
{
    dream_proc *p = arg;
    uint8_t b[8192 + 1];
    int16_t s[4096];

    for (;;) {
        size_t off = 0;

        pthread_mutex_lock(&p->lock);
        if (p->has_odd) {
            b[0] = p->odd;
            off = 1;
            p->has_odd = false;
        }
        pthread_mutex_unlock(&p->lock);

        ssize_t r = read(p->out_fd, b + off, sizeof(b) - 1 - off);
        if (r < 0 && errno == EINTR) {
            if (off) {
                p->odd = b[0];
                p->has_odd = true;
            }
            continue;
        }
        if (r <= 0)
            break;

        size_t bytes = off + (size_t) r, n = bytes / 2;
        memcpy(s, b, n * 2);

        pthread_mutex_lock(&p->lock);
        if (bytes & 1) {
            p->odd = b[bytes - 1];
            p->has_odd = true;
        }
        ring_put(&p->out, s, n);
        pthread_mutex_unlock(&p->lock);
    }

    pthread_mutex_lock(&p->lock);
    p->exited = true;
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

/* mkdir -p */
static int make_dir(const char *dir)
{
    char path[512];

    snprintf(path, sizeof(path), "%s", dir);
    for (char *c = path + 1; *c; c++) {
        if (*c != '/')
            continue;
        *c = '\0';
        if (mkdir(path, 0755) < 0 && errno != EEXIST)
            return -1;
        *c = '/';
    }
    return mkdir(path, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

static int write_ini(const char *dir, const char *ini)
{
    char path[600];
    FILE *f;

    snprintf(path, sizeof(path), "%s/Dream.ini", dir);
    f = fopen(path, "w");
    if (!f)
        return -1;
    fputs(ini, f);
    return fclose(f);
}

dream_proc *dream_proc_start(const char *dream_path, const char *dir, const char *ini,
                             const char *const args[], size_t ring_cap)
{
    int to[2] = { -1, -1 }, from[2] = { -1, -1 };
    dream_proc *p;

    if (!dream_path || !dream_path[0] || !dir || !args || !args[0])
        return NULL;
    if (make_dir(dir) < 0 || (ini && write_ini(dir, ini) < 0)) {
        HLOGE(LOG_C, "cannot prepare %s for Dream: %s", dir, strerror(errno));
        return NULL;
    }

    p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->pid = -1;
    p->in_fd = p->out_fd = -1;
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->more, NULL);
    if (!ring_init(&p->in, ring_cap) || !ring_init(&p->out, ring_cap))
        goto fail_sync;

    if (pipe2(to, O_CLOEXEC) < 0 || pipe2(from, O_CLOEXEC) < 0) {
        HLOGE(LOG_C, "pipe: %s", strerror(errno));
        goto fail_sync;
    }

    p->pid = fork();
    if (p->pid < 0) {
        HLOGE(LOG_C, "fork: %s", strerror(errno));
        goto fail_sync;
    }
    if (p->pid == 0) {
        /* Dream's messages go to its own log, beside its ini */
        char log[600];
        int fd;

        if (chdir(dir) < 0)
            _exit(126);
        snprintf(log, sizeof(log), "%s/dream.log", dir);
        fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(to[0], STDIN_FILENO);
        dup2(from[1], STDOUT_FILENO);
        if (fd >= 0)
            dup2(fd, STDERR_FILENO);
        signal(SIGPIPE, SIG_DFL);
        execvp(dream_path, (char *const *) args);
        _exit(127);
    }

    close(to[0]);
    close(from[1]);
    to[0] = from[1] = -1;
    p->in_fd = to[1];
    p->out_fd = from[0];
    to[1] = from[0] = -1;

    if (pthread_create(&p->writer, NULL, writer_main, p) != 0)
        goto fail_child;
    if (pthread_create(&p->reader, NULL, reader_main, p) != 0) {
        pthread_mutex_lock(&p->lock);
        p->stop = true;
        pthread_cond_signal(&p->more);
        pthread_mutex_unlock(&p->lock);
        kill(p->pid, SIGKILL);              /* a writer blocked on the pipe ends */
        pthread_join(p->writer, NULL);
        goto fail_child;
    }

    HLOGI(LOG_C, "Dream started (pid %d) in %s", (int) p->pid, dir);
    return p;

fail_child:
    HLOGE(LOG_C, "cannot start Dream's I/O threads");
    kill(p->pid, SIGKILL);
    waitpid(p->pid, NULL, 0);
fail_sync:
    for (int i = 0; i < 2; i++) {
        if (to[i] >= 0) close(to[i]);
        if (from[i] >= 0) close(from[i]);
    }
    if (p->in_fd >= 0) close(p->in_fd);
    if (p->out_fd >= 0) close(p->out_fd);
    pthread_cond_destroy(&p->more);
    pthread_mutex_destroy(&p->lock);
    free(p->in.buf);
    free(p->out.buf);
    free(p);
    return NULL;
}

size_t dream_proc_write(dream_proc *p, const int16_t *s, size_t n)
{
    size_t k;

    if (!p || !n)
        return 0;
    pthread_mutex_lock(&p->lock);
    k = p->finish ? 0 : ring_put(&p->in, s, n);
    pthread_cond_signal(&p->more);
    pthread_mutex_unlock(&p->lock);
    return k;
}

size_t dream_proc_read(dream_proc *p, int16_t *s, size_t n)
{
    size_t k;

    if (!p || !n)
        return 0;
    pthread_mutex_lock(&p->lock);
    k = ring_get(&p->out, s, n);
    pthread_mutex_unlock(&p->lock);
    return k;
}

size_t dream_proc_readable(dream_proc *p)
{
    size_t k;

    if (!p)
        return 0;
    pthread_mutex_lock(&p->lock);
    k = p->out.len;
    pthread_mutex_unlock(&p->lock);
    return k;
}

void dream_proc_finish(dream_proc *p)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->lock);
    p->finish = true;
    pthread_cond_signal(&p->more);
    pthread_mutex_unlock(&p->lock);
}

bool dream_proc_running(dream_proc *p)
{
    bool r;

    if (!p)
        return false;
    pthread_mutex_lock(&p->lock);
    r = !p->exited;
    pthread_mutex_unlock(&p->lock);
    return r;
}

void dream_proc_dropped(dream_proc *p, uint64_t *in, uint64_t *out)
{
    pthread_mutex_lock(&p->lock);
    if (in) *in = p->in.dropped;
    if (out) *out = p->out.dropped;
    pthread_mutex_unlock(&p->lock);
}

void dream_proc_stop(dream_proc *p)
{
    int status;

    if (!p)
        return;

    pthread_mutex_lock(&p->lock);
    p->stop = true;
    pthread_cond_signal(&p->more);
    pthread_mutex_unlock(&p->lock);

    /* the child goes first: a writer blocked on a full pipe gets EPIPE, the
     * reader its EOF */
    kill(p->pid, SIGTERM);
    for (int i = 0; i < 20 && waitpid(p->pid, &status, WNOHANG) == 0; i++)
        usleep(100 * 1000);
    if (waitpid(p->pid, &status, WNOHANG) == 0) {
        kill(p->pid, SIGKILL);
        waitpid(p->pid, &status, 0);
    }

    pthread_join(p->writer, NULL);
    pthread_join(p->reader, NULL);
    if (p->in_fd >= 0)
        close(p->in_fd);
    close(p->out_fd);
    pthread_cond_destroy(&p->more);
    pthread_mutex_destroy(&p->lock);
    free(p->in.buf);
    free(p->out.buf);
    HLOGI(LOG_C, "Dream (pid %d) stopped", (int) p->pid);
    free(p);
}

bool dream_proc_has_option(const char *dream_path, const char *option)
{
    char help[65536];
    size_t got = 0;
    int out[2];
    pid_t pid;
    bool found;
    struct timespec t0, t;

    if (pipe2(out, O_CLOEXEC) < 0)
        return false;
    pid = fork();
    if (pid < 0) {
        close(out[0]);
        close(out[1]);
        return false;
    }
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);

        if (null >= 0)
            dup2(null, STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);    /* Dream prints its help here */
        execlp(dream_path, "dream", "--help", (char *) NULL);
        _exit(127);
    }
    close(out[1]);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct pollfd pfd = { .fd = out[0], .events = POLLIN };
        long left;
        ssize_t n;

        clock_gettime(CLOCK_MONOTONIC, &t);
        left = 3000 - ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0 || got >= sizeof(help) - 1 || poll(&pfd, 1, (int) left) <= 0)
            break;
        n = read(out[0], help + got, sizeof(help) - 1 - got);
        if (n <= 0)
            break;
        got += (size_t) n;
    }
    close(out[0]);
    kill(pid, SIGKILL);                 /* gone already, or hung */
    waitpid(pid, NULL, 0);
    help[got] = '\0';
    found = strstr(help, option) != NULL;
    HLOGI(LOG_C, "%s %s %s", dream_path, found ? "has" : "has no", option);
    return found;
}
