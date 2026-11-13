/* dec_beacon - the handler UUCP (uux) and NNCP (nncp-exec) run for a
 * position beacon sent from another station, an SOS most often
 * (docs/api-design/store-and-forward.md, "Receiving").
 *
 * It reads one beacon frame (docs/BEACON-FORMAT.md) from standard input and
 * leaves it in BEACON_DROP_DIR, where radiod takes it as if heard on the
 * air.  Its one argument, optional, is when the sender set it, unix
 * milliseconds: radiod takes the beacons in that order, so an SOS and its
 * "emergency over" apply in the order they were set however the transports
 * deliver them.  The argument comes from the other station, so it is only
 * ever digits and never chooses a path.  The file is written under a name
 * starting with '.' and renamed when complete, so radiod never reads half
 * of one.  Its name, <via>-<sent ms>-<received>-<pid>, says which transport
 * brought it (uuxqt sets UU_MACHINE, nncp-exec NNCP_SENDER) and when it was
 * sent (the time it came when the sender didn't say).
 *
 * Exit status 0 when the beacon was left for radiod, 1 otherwise; the
 * transport then reports the failure to the sender as it does for any
 * handler.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "beacon.h"

/* The most read from standard input: a frame longer than the full layout
 * is refused. */
#define FRAME_MAX BEACON_FULL_BYTES

/* Owner and group read and write: radiod and the transports share the
 * directory through its group. */
#define FILE_MODE 0660

/* The send time's digits: unix milliseconds have 13 until the year 2286. */
#define SENT_DIGITS_MAX 15

#define MS_PER_S  1000LL
#define NS_PER_MS 1000000L

/* The send time, unix ms, from the argument; -1 when it is not one. */
static long long sent_ms(const char *arg)
{
    size_t n = strlen(arg);
    if (n == 0 || n > SENT_DIGITS_MAX || strspn(arg, "0123456789") != n)
        return -1;
    return strtoll(arg, NULL, 10);
}

static const char *via(void)
{
    if (getenv("UU_MACHINE"))
        return "uucp";
    if (getenv("NNCP_SENDER"))
        return "nncp";
    return "queue";
}

int main(int argc, char **argv)
{
    long long sent = 0;
    if (argc > 2 || (argc == 2 && (sent = sent_ms(argv[1])) < 0))
    {
        fprintf(stderr, "dec_beacon: takes one argument at most, the send time in unix milliseconds\n");
        return 1;
    }
    unsigned char buf[FRAME_MAX + 1];
    size_t n = 0;
    for (;;)
    {
        ssize_t r = read(STDIN_FILENO, buf + n, sizeof buf - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
        {
            fprintf(stderr, "dec_beacon: %s\n", strerror(errno));
            return 1;
        }
        if (r == 0)
            break;
        n += (size_t) r;
        if (n > FRAME_MAX)
        {
            fprintf(stderr, "dec_beacon: more than %d bytes: not a beacon\n", FRAME_MAX);
            return 1;
        }
    }
    beacon b;
    int rc = beacon_decode(buf, n, &b);
    if (rc != 0)
    {
        fprintf(stderr, "dec_beacon: not a beacon (%zu bytes, error %d)\n", n, rc);
        return 1;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (sent == 0)
        sent = (long long) ts.tv_sec * MS_PER_S + ts.tv_nsec / NS_PER_MS;
    char name[80], tmp[sizeof name + 1], path[sizeof BEACON_DROP_DIR + sizeof tmp + 1];
    char final[sizeof path];
    snprintf(name, sizeof name, "%s-%lld-%lld.%09ld-%ld", via(), sent, (long long) ts.tv_sec,
             ts.tv_nsec, (long) getpid());
    snprintf(tmp, sizeof tmp, ".%s", name);
    snprintf(path, sizeof path, "%s/%s", BEACON_DROP_DIR, tmp);
    snprintf(final, sizeof final, "%s/%s", BEACON_DROP_DIR, name);

    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, FILE_MODE);
    if (fd < 0)
    {
        fprintf(stderr, "dec_beacon: %s: %s\n", path, strerror(errno));
        return 1;
    }
    int ok = fchmod(fd, FILE_MODE) == 0;   /* whatever the transport's umask */
    ok = write(fd, buf, n) == (ssize_t) n && ok;
    ok = close(fd) == 0 && ok;
    if (!ok || rename(path, final) != 0)
    {
        fprintf(stderr, "dec_beacon: %s: %s\n", final, strerror(errno));
        unlink(path);
        return 1;
    }
    return 0;
}
