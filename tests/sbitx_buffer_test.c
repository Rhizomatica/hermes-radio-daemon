/*
 * sBitx audio ring regression test: the waits must end at shutdown.
 *
 * The capture, DSP and playback threads block in read_buffer()/
 * write_buffer() for data from the thread before them. At shutdown that
 * data stops (capture exits first), and with an unbounded wait the DSP and
 * playback threads never returned, so radiod ignored SIGTERM until systemd
 * killed it after 90 s (station2, 24 Sep 2026).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

_Atomic bool shutdown_ = false;

#include "../sbitx/ring_buffer.c"
#include "../sbitx/sbitx_buffer.c"

#define BLOCK 4096

static buffer ring;
static _Atomic bool returned;
static uint8_t out[BLOCK];

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *reader(void *arg)
{
    (void) arg;
    memset(out, 0xA5, sizeof(out));
    read_buffer(&ring, out, BLOCK);
    returned = true;
    return NULL;
}

static void *writer(void *arg)
{
    (void) arg;
    static uint8_t in[BLOCK];
    write_buffer(&ring, in, BLOCK);
    returned = true;
    return NULL;
}

/* Data still flows normally. */
static void test_round_trip(void)
{
    uint8_t in[BLOCK], back[BLOCK];
    for (int i = 0; i < BLOCK; i++)
        in[i] = (uint8_t) i;
    write_buffer(&ring, in, BLOCK);
    read_buffer(&ring, back, BLOCK);
    assert(memcmp(in, back, BLOCK) == 0);
    printf("  data round trip ....................... ok\n");
}

/* A reader on an empty ring waits while running, and returns (with
 * silence) soon after shutdown is set. */
static void test_reader_ends_at_shutdown(void)
{
    pthread_t t;
    shutdown_ = false;
    returned = false;
    assert(pthread_create(&t, NULL, reader, NULL) == 0);

    usleep(300000);
    assert(!returned && "read_buffer returned on an empty ring while running");

    double t0 = now_s();
    shutdown_ = true;
    pthread_join(t, NULL);
    double took = now_s() - t0;
    assert(returned);
    assert(took < 0.5 && "read_buffer did not notice shutdown");
    for (int i = 0; i < BLOCK; i++)
        assert(out[i] == 0);
    printf("  blocked reader ends at shutdown ....... ok (%.0f ms)\n", took * 1000);
}

/* A writer on a full ring waits while running, and gives up soon after
 * shutdown is set. */
static void test_writer_ends_at_shutdown(void)
{
    static uint8_t fill[BLOCK];
    shutdown_ = false;
    while (free_size_buffer(&ring) >= BLOCK)
        write_buffer(&ring, fill, BLOCK);

    pthread_t t;
    returned = false;
    assert(pthread_create(&t, NULL, writer, NULL) == 0);

    usleep(300000);
    assert(!returned && "write_buffer returned on a full ring while running");

    double t0 = now_s();
    shutdown_ = true;
    pthread_join(t, NULL);
    double took = now_s() - t0;
    assert(returned);
    assert(took < 0.5 && "write_buffer did not notice shutdown");
    printf("  blocked writer ends at shutdown ....... ok (%.0f ms)\n", took * 1000);
}

int main(void)
{
    initialize_buffer(&ring, 16);   /* 64 KiB, as the daemon's rings */

    printf("sbitx audio rings:\n");
    test_round_trip();
    test_reader_ends_at_shutdown();
    clear_buffer(&ring);
    test_writer_ends_at_shutdown();
    printf("sbitx_buffer_test: all ok\n");
    return 0;
}
