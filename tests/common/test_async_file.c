/* async_file: the test hooks' file I/O, off the real-time threads.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "unity.h"
#include "test_util.h"
#include "common/async_file.h"

static char tmpdir[200];

void setUp(void) {}
void tearDown(void) {}

static const char *in_tmp(const char *name)
{
    static char path[2][300];
    static int k;

    k ^= 1;
    snprintf(path[k], sizeof path[k], "%s/%s", tmpdir, name);
    return path[k];
}

static long file_size(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 ? (long) st.st_size : -1;
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* until the size of path is want, or 2 s */
static bool size_becomes(const char *path, long want)
{
    for (int i = 0; i < 200; i++) {
        if (file_size(path) == want)
            return true;
        sleep_ms(10);
    }
    return false;
}

static void test_writer_on_off(void)
{
    const char *a = in_tmp("a.bin");
    async_writer *w = async_writer_new("test", 4096, NULL, NULL);
    char buf[100];

    TEST_ASSERT_NOT_NULL(w);
    memset(buf, 'x', sizeof buf);
    async_writer_write(w, buf, sizeof buf);     /* off: dropped, not counted */
    async_writer_on(w, a);
    TEST_ASSERT_TRUE(size_becomes(a, 0));
    sleep_ms(50);                               /* it is on once the file is open */
    async_writer_write(w, buf, sizeof buf);
    async_writer_off(w);
    TEST_ASSERT_TRUE_MESSAGE(size_becomes(a, 100), test_msg("size %ld", file_size(a)));
    TEST_ASSERT_EQUAL_UINT64(0, async_writer_dropped(w));
    async_writer_free(w);
}

/* An off and an on within one look of the thread still close the file and
 * open the new path afresh. */
static void test_writer_off_on_reopens(void)
{
    const char *a = in_tmp("first.bin"), *b = in_tmp("second.bin");
    async_writer *w = async_writer_new("test", 4096, NULL, NULL);
    char buf[64];

    TEST_ASSERT_NOT_NULL(w);
    memset(buf, 'y', sizeof buf);
    async_writer_on(w, a);
    TEST_ASSERT_TRUE(size_becomes(a, 0));
    sleep_ms(50);
    async_writer_write(w, buf, sizeof buf);
    async_writer_off(w);
    async_writer_on(w, b);                      /* back to back */
    TEST_ASSERT_TRUE_MESSAGE(size_becomes(a, 64), test_msg("first: %ld", file_size(a)));
    TEST_ASSERT_TRUE_MESSAGE(size_becomes(b, 0), test_msg("second: %ld", file_size(b)));
    sleep_ms(50);
    async_writer_write(w, buf, 10);
    async_writer_free(w);                       /* writes what is queued */
    TEST_ASSERT_EQUAL_INT(64, file_size(a));
    TEST_ASSERT_EQUAL_INT(10, file_size(b));
}

/* A trigger file turns the writer on while it exists. */
static void test_writer_trigger(void)
{
    const char *trig = in_tmp("trigger"), *out = in_tmp("dump.bin");
    async_writer *w = async_writer_new("test", 4096, trig, out);
    char buf[32];
    FILE *f;

    TEST_ASSERT_NOT_NULL(w);
    sleep_ms(300);
    TEST_ASSERT_EQUAL_INT(-1, file_size(out));
    TEST_REQUIRE((f = fopen(trig, "w")) != NULL);
    fclose(f);
    TEST_ASSERT_TRUE(size_becomes(out, 0));
    sleep_ms(50);
    memset(buf, 'z', sizeof buf);
    async_writer_write(w, buf, sizeof buf);
    unlink(trig);
    TEST_ASSERT_TRUE_MESSAGE(size_becomes(out, 32), test_msg("size %ld", file_size(out)));
    async_writer_free(w);
}

/* bytes of the loop, as they come, for up to 2 s */
static bool loop_read(async_loop *l, char *out, size_t n)
{
    for (int i = 0; i < 200; i++) {
        if (async_loop_read(l, out, n))
            return true;
        sleep_ms(10);
    }
    return false;
}

static void put_file(const char *path, int c, size_t n)
{
    char *buf = malloc(n);
    char tmp[320];
    FILE *f;

    TEST_REQUIRE(buf != NULL);
    memset(buf, c, n);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    TEST_REQUIRE((f = fopen(tmp, "wb")) != NULL);
    TEST_REQUIRE(fwrite(buf, 1, n, f) == n);
    fclose(f);
    TEST_REQUIRE(rename(tmp, path) == 0);       /* a new inode: replaced */
    free(buf);
}

static void test_loop_follows_the_file(void)
{
    const char *path = in_tmp("inject.s32");
    async_loop *l = async_loop_new(path, 0);
    char buf[1000];

    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_FALSE(async_loop_read(l, buf, sizeof buf));     /* no file */
    put_file(path, 'a', 3000);
    TEST_ASSERT_TRUE(loop_read(l, buf, sizeof buf));
    TEST_ASSERT_EACH_EQUAL_CHAR('a', buf, sizeof buf);
    TEST_ASSERT_TRUE(loop_read(l, buf, sizeof buf));            /* and round again */
    TEST_ASSERT_EACH_EQUAL_CHAR('a', buf, sizeof buf);

    /* replaced while not read (receiving): idle takes it up, so the first
     * read after it is the new file's */
    put_file(path, 'b', 3000);
    for (int i = 0; i < 50; i++) {
        async_loop_idle(l);
        sleep_ms(10);
    }
    TEST_ASSERT_TRUE(loop_read(l, buf, sizeof buf));
    TEST_ASSERT_EACH_EQUAL_CHAR('b', buf, sizeof buf);

    unlink(path);
    for (int i = 0; i < 50; i++) {
        async_loop_idle(l);
        sleep_ms(10);
    }
    TEST_ASSERT_FALSE(async_loop_read(l, buf, sizeof buf));
    async_loop_free(l);
}

int main(void)
{
    snprintf(tmpdir, sizeof tmpdir, "/tmp/async_file_test.XXXXXX");
    const char *t = getenv("TMPDIR");
    if (t && t[0])
        snprintf(tmpdir, sizeof tmpdir, "%s/async_file_test.XXXXXX", t);
    TEST_REQUIRE(mkdtemp(tmpdir));

    UNITY_BEGIN();
    RUN_TEST(test_writer_on_off);
    RUN_TEST(test_writer_off_on_reopens);
    RUN_TEST(test_writer_trigger);
    RUN_TEST(test_loop_follows_the_file);
    int rc = UNITY_END();

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", tmpdir);
    return rc;
}
