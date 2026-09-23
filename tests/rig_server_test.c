/*
 * rig_server protocol regression test.
 *
 * Covers two faults found on the bench with Hamlib 4.6.2 talking to this
 * daemon over NET rigctl:
 *
 *   1. "q" drew no reply and the server left the socket open. hamlib's
 *      netrigctl_close() sends "q\n" and then WAITS for an answer, so every
 *      one-shot "rigctl -m 2" paid a 20 s read timeout on exit. It looked
 *      like whatever command had been issued was hanging.
 *
 *   2. "\chk_vfo" answered "CHKVFO 1", the extended/prompt-mode form.
 *      A normal client logged "unknown value returned from
 *      netrigctl_transaction=9" -- 9 being the length of that string.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "../radio.h"
#include "../radio_backend.h"
#include "../radio_controls.h"
#include "../hamlib/rig_server.h"

/* The daemon owns these; the server reads them. */
_Atomic bool shutdown_ = false;
_Atomic bool timer_reset = false;

/* radio_backend.c reaches for both real vtables at link time. The hamlib
 * one records PTT changes, standing in for the rig. */
static _Atomic int ptt_calls = 0;
static void fake_set_txrx_state(radio *radio_h, bool txrx_state)
{
    ptt_calls++;
    radio_h->txrx_state = txrx_state;
}
const radio_backend_ops hamlib_backend_ops = {
    .name = "hamlib",
    .set_txrx_state = fake_set_txrx_state,
};
const radio_backend_ops sbitx_backend_ops  = { .name = "hfsignals" };

int radio_daemon_core_run(const radio_backend_selection *selection,
                          const radio_daemon_runtime *runtime)
{
    (void) selection; (void) runtime;
    return 0;
}

#include "../cfg_utils.c"
#include "../radio_backend.c"
#include "../radio_controls.c"

#define TEST_PORT 45329

static int connect_server(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port   = htons(TEST_PORT),
    };
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    for (int tries = 0; tries < 200; tries++)
    {
        if (connect(fd, (struct sockaddr *) &a, sizeof(a)) == 0)
        {
            /* Never block forever: the bug being tested is a MISSING reply,
             * and without this the test would hang exactly like hamlib did
             * rather than failing with a usable message. */
            struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            return fd;
        }
        usleep(10000);
    }
    assert(!"could not connect to rig_server");
    return -1;
}

/* Read until a newline or the peer closes. Returns bytes read (0 on EOF). */
static size_t read_line(int fd, char *buf, size_t max)
{
    size_t n = 0;
    while (n + 1 < max)
    {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) break;
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = '\0';
    return n;
}

static void send_line(int fd, const char *s)
{
    assert(write(fd, s, strlen(s)) == (ssize_t) strlen(s));
}

/* "q" must be answered AND must close the connection. */
static void test_quit_replies_and_closes(void)
{
    int fd = connect_server();
    char buf[128];

    send_line(fd, "q\n");
    size_t n = read_line(fd, buf, sizeof(buf));
    assert(n > 0 && "server must answer q -- silence costs hamlib a 20 s timeout");
    assert(strcmp(buf, "RPRT 0\n") == 0);

    /* and then the server must drop us, so netrigctl_close() returns at once */
    char c;
    ssize_t r = read(fd, &c, 1);
    assert(r == 0 && "server must close the connection after q");
    close(fd);
    printf("  quit replies RPRT 0 and closes ....... ok\n");
}

/* "\chk_vfo" must answer a bare value, not the extended "CHKVFO n" form. */
static void test_chk_vfo_is_a_bare_value(void)
{
    int fd = connect_server();
    char buf[128];

    send_line(fd, "\\chk_vfo\n");
    size_t n = read_line(fd, buf, sizeof(buf));
    assert(n > 0);
    assert(strncmp(buf, "CHKVFO", 6) != 0 &&
           "extended form makes hamlib log 'unknown value ... =9'");
    assert((strcmp(buf, "0\n") == 0 || strcmp(buf, "1\n") == 0));
    close(fd);
    printf("  chk_vfo answers a bare value ......... ok (\"%c\")\n", buf[0]);
}

/* An ordinary command still works, and an unknown one still answers. */
static void test_ordinary_and_unknown(void)
{
    int fd = connect_server();
    char buf[128];

    send_line(fd, "f\n");
    assert(read_line(fd, buf, sizeof(buf)) > 0);
    assert(strtoul(buf, NULL, 10) == 14200000);

    send_line(fd, "zzz\n");
    assert(read_line(fd, buf, sizeof(buf)) > 0);
    assert(strcmp(buf, "RPRT -11\n") == 0);

    /* several lines in one write must all be answered */
    send_line(fd, "f\nf\n");
    assert(read_line(fd, buf, sizeof(buf)) > 0);
    assert(read_line(fd, buf, sizeof(buf)) > 0);

    close(fd);
    printf("  ordinary + unknown commands .......... ok\n");
}

static radio *test_radio;

static bool expect_ptt(bool state, int ms)
{
    for (int i = 0; i < ms / 10; i++)
    {
        if (test_radio->txrx_state == state)
            return true;
        usleep(10000);
    }
    return test_radio->txrx_state == state;
}

static void cmd_ok(int fd, const char *line)
{
    char buf[128];
    send_line(fd, line);
    assert(read_line(fd, buf, sizeof(buf)) > 0);
    assert(strcmp(buf, "RPRT 0\n") == 0);
}

/* A client that keys with "T 1" and drops the connection without "T 0"
 * (a modem killed mid-transmission) must not leave the radio keyed. */
static void test_ptt_released_when_keyer_disconnects(void)
{
    int fd = connect_server();
    cmd_ok(fd, "T 1\n");
    assert(expect_ptt(IN_TX, 100));

    close(fd);
    assert(expect_ptt(IN_RX, 1000) && "keyer dropped: PTT must be released");
    printf("  keyer disconnect releases PTT ........ ok\n");
}

/* Another client connecting and leaving must not cut a transmission, and a
 * keyer that unkeyed before leaving costs no extra PTT command. */
static void test_ptt_kept_for_other_clients(void)
{
    char buf[128];
    int keyer = connect_server();
    cmd_ok(keyer, "T 1\n");
    assert(expect_ptt(IN_TX, 100));

    int other = connect_server();
    send_line(other, "t\n");
    assert(read_line(other, buf, sizeof(buf)) > 0 && strcmp(buf, "1\n") == 0);
    close(other);
    usleep(300000);
    assert(test_radio->txrx_state == IN_TX && "a reader left: keep transmitting");

    cmd_ok(keyer, "T 0\n");
    int calls = ptt_calls;
    close(keyer);
    usleep(300000);
    assert(ptt_calls == calls && "unkeyed before leaving: nothing to release");
    printf("  other clients leave PTT alone ........ ok\n");
}

/* When a second client takes PTT over, the first one leaving must not unkey
 * it; the second one leaving must. */
static void test_ptt_follows_last_keyer(void)
{
    int a = connect_server();
    int b = connect_server();
    cmd_ok(a, "T 1\n");
    cmd_ok(b, "T 1\n");

    close(a);
    usleep(300000);
    assert(test_radio->txrx_state == IN_TX);

    close(b);
    assert(expect_ptt(IN_RX, 1000));
    printf("  PTT follows the last keyer ........... ok\n");
}

int main(void)
{
    radio *r = calloc(1, sizeof(*r));
    assert(r);
    test_radio = r;
    r->backend_kind     = RADIO_BACKEND_HAMLIB;
    r->backend_ops      = &hamlib_backend_ops;
    r->profiles_count   = 1;
    r->rig_server_enable = true;
    r->rig_server_port  = TEST_PORT;
    r->profiles[0].freq = 14200000;

    assert(rig_server_start(r) && "rig_server_start failed");

    printf("rig_server protocol:\n");
    test_ordinary_and_unknown();
    test_chk_vfo_is_a_bare_value();
    test_quit_replies_and_closes();
    test_ptt_released_when_keyer_disconnects();
    test_ptt_kept_for_other_clients();
    test_ptt_follows_last_keyer();

    rig_server_stop();
    free(r);
    printf("rig_server_test: all ok\n");
    return 0;
}
