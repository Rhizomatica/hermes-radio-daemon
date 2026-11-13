/* test_dec_beacon - the handler UUCP and NNCP run for a beacon from another
 * station (handlers/dec_beacon.c), run as they run it: the frame on its
 * standard input, the transport in its environment.  It is built for the
 * test with BEACON_DROP_DIR in the test's own directory.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "unity.h"
#include "test_util.h"
#include "modem/beacon.h"

#ifndef DEC_BEACON_BIN
#error "built by tests/tests.mk, which names the handler built for the test"
#endif

void setUp(void)
{
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", BEACON_DROP_DIR, BEACON_DROP_DIR);
    TEST_REQUIRE(system(cmd) == 0);
}
void tearDown(void) {}

/* Runs the handler with in on its standard input, env set (NAME=value or
 * NULL) and one argument if arg; returns its exit status. */
static int run(const void *in, size_t len, const char *env, const char *arg)
{
    int p[2];
    TEST_REQUIRE(pipe(p) == 0);
    pid_t pid = fork();
    TEST_REQUIRE(pid >= 0);
    if (pid == 0)
    {
        dup2(p[0], STDIN_FILENO);
        close(p[0]);
        close(p[1]);
        unsetenv("UU_MACHINE");
        unsetenv("NNCP_SENDER");
        if (env)
            putenv((char *) env);
        execl(DEC_BEACON_BIN, "dec_beacon", arg, (char *) NULL);
        _exit(127);
    }
    close(p[0]);
    TEST_REQUIRE(write(p[1], in, len) == (ssize_t) len);
    close(p[1]);
    int st;
    TEST_REQUIRE(waitpid(pid, &st, 0) == pid);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* The files in the drop directory: how many, and the first one's name. */
static int dropped(char *first, size_t cap)
{
    DIR *d = opendir(BEACON_DROP_DIR);
    TEST_REQUIRE(d);
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (n++ == 0 && first)
            snprintf(first, cap, "%s", e->d_name);
    }
    closedir(d);
    return n;
}

static size_t frame(uint8_t *f, size_t cap)
{
    beacon b = {0};
    strcpy(b.callsign, "B0AC");
    b.lat_e7 = 45000000;
    b.lon_e7 = -757000000;
    b.emergency = 2;
    int n = beacon_encode(&b, f, cap);
    TEST_REQUIRE(n > 0);
    return (size_t) n;
}

static void test_left_for_radiod(void)
{
    uint8_t f[BEACON_COMPACT_BYTES];
    size_t n = frame(f, sizeof f);
    TEST_ASSERT_EQUAL_INT(0, run(f, n, "UU_MACHINE=B0AA", NULL));
    char name[256];
    TEST_ASSERT_EQUAL_INT(1, dropped(name, sizeof name));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(name, "uucp-", 5), name);

    char path[512];
    snprintf(path, sizeof path, "%s/%s", BEACON_DROP_DIR, name);
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(fp);
    uint8_t got[64];
    size_t len = fread(got, 1, sizeof got, fp);
    fclose(fp);
    TEST_ASSERT_EQUAL_size_t(n, len);
    TEST_ASSERT_EQUAL_MEMORY(f, got, n);
    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, stat(path, &st));
    TEST_ASSERT_EQUAL_INT(0660, st.st_mode & 0777);
}

static void test_names_the_transport(void)
{
    uint8_t f[BEACON_FULL_BYTES];
    size_t n = frame(f, sizeof f);   /* the full layout is taken too */
    TEST_ASSERT_EQUAL_INT(0, run(f, n, "NNCP_SENDER=AAAA", NULL));
    TEST_ASSERT_EQUAL_INT(0, run(f, n, NULL, NULL));
    char name[256];
    TEST_ASSERT_EQUAL_INT(2, dropped(name, sizeof name));
    TEST_ASSERT_TRUE_MESSAGE(!strncmp(name, "nncp-", 5) || !strncmp(name, "queue-", 6), name);
}

/* The send time, its one argument, leads the name after the transport, so
 * radiod takes the beacons in the order they were sent. */
static void test_names_the_send_time(void)
{
    uint8_t f[BEACON_COMPACT_BYTES];
    size_t n = frame(f, sizeof f);
    TEST_ASSERT_EQUAL_INT(0, run(f, n, "UU_MACHINE=B0AA", "1794567402356"));
    char name[256];
    TEST_ASSERT_EQUAL_INT(1, dropped(name, sizeof name));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(name, "uucp-1794567402356-", 19), name);
}

static void test_refused(void)
{
    uint8_t f[BEACON_COMPACT_BYTES], big[BEACON_FULL_BYTES + 1] = {0};
    size_t n = frame(f, sizeof f);
    memcpy(big, f, n);
    uint8_t junk[BEACON_COMPACT_BYTES] = { 0xE0 };   /* no callsign */
    TEST_ASSERT_EQUAL_INT(1, run(junk, sizeof junk, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, run(big, sizeof big, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, run("", 0, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(1, run(f, n, NULL, "/etc/passwd"));   /* arguments come from the sender */
    TEST_ASSERT_EQUAL_INT(1, run(f, n, NULL, "12-3"));          /* only digits */
    TEST_ASSERT_EQUAL_INT(1, run(f, n, NULL, "1234567890123456")); /* too long for a time */
    TEST_ASSERT_EQUAL_INT(0, dropped(NULL, 0));

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", BEACON_DROP_DIR);
    TEST_REQUIRE(system(cmd) == 0);
    TEST_ASSERT_EQUAL_INT(1, run(f, n, NULL, NULL));   /* no drop directory */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_left_for_radiod);
    RUN_TEST(test_names_the_transport);
    RUN_TEST(test_names_the_send_time);
    RUN_TEST(test_refused);
    int rc = UNITY_END();
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", BEACON_DROP_DIR);
    if (system(cmd) != 0)
        return 1;
    return rc;
}
