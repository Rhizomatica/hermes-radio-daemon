/* radiod_log: core.ini's log settings, on mercury's hermes_log.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "test_util.h"
#include "common/radiod_log.h"

static char tmpdir[200];
static radio r;

void setUp(void)
{
    memset(&r, 0, sizeof r);
    snprintf(r.log_level, sizeof r.log_level, "info");
}

void tearDown(void) {}

/* The whole file, NUL-terminated (static, so read it before the next call). */
static const char *slurp(const char *path)
{
    static char buf[16384];
    FILE *f = fopen(path, "r");
    size_t n = 0;
    if (f)
    {
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
    return buf;
}

static void test_level_names(void)
{
    hermes_log_level_t l;
    const struct { const char *name; hermes_log_level_t want; } ok[] = {
        { "trace", HERMES_LOG_LEVEL_TRACE },
        { "debug", HERMES_LOG_LEVEL_DEBUG }, { "TIMING", HERMES_LOG_LEVEL_TIMING },
        { "Info", HERMES_LOG_LEVEL_INFO },   { "warn", HERMES_LOG_LEVEL_WARN },
        { "error", HERMES_LOG_LEVEL_ERROR },
    };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
    {
        TEST_ASSERT_TRUE_MESSAGE(radiod_log_level(ok[i].name, &l), ok[i].name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(ok[i].want, l, ok[i].name);
    }
    TEST_ASSERT_FALSE(radiod_log_level("verbose", &l));
    TEST_ASSERT_FALSE(radiod_log_level("", &l));
    TEST_ASSERT_FALSE(radiod_log_level(NULL, &l));
}

static char seen[8][48];
static int nseen;

static void record(const char *component, hermes_log_level_t level, void *user)
{
    (void) user;
    if (nseen < 8)
        snprintf(seen[nseen++], sizeof seen[0], "%s=%d", component, (int) level);
}

static void test_component_list(void)
{
    nseen = 0;
    TEST_ASSERT_EQUAL_INT(2, radiod_log_components("hamlib=debug, ws = warn", record, NULL));
    TEST_ASSERT_EQUAL_STRING("hamlib=0", seen[0]);
    TEST_ASSERT_EQUAL_STRING("ws=3", seen[1]);

    nseen = 0;
    TEST_ASSERT_EQUAL_INT(1, radiod_log_components(" , ,api=error,,", record, NULL));
    TEST_ASSERT_EQUAL_STRING("api=4", seen[0]);
    TEST_ASSERT_EQUAL_INT(0, radiod_log_components("", record, NULL));
    TEST_ASSERT_EQUAL_INT(0, radiod_log_components(NULL, record, NULL));

    /* a bad pair stops the list; the pairs before it are set */
    nseen = 0;
    TEST_ASSERT_EQUAL_INT(-1, radiod_log_components("sbitx=info, hamlib", record, NULL));
    TEST_ASSERT_EQUAL_INT(1, nseen);
    TEST_ASSERT_EQUAL_INT(-1, radiod_log_components("hamlib=loud", record, NULL));
    TEST_ASSERT_EQUAL_INT(-1, radiod_log_components("=debug", record, NULL));
}

/* The file sink gets hermes_log's lines, at the configured level: the same
 * format as mercury's. */
static void test_file_sink(void)
{
    char path[sizeof r.log_file];
    snprintf(path, sizeof path, "%s/radiod.log", tmpdir);
    snprintf(r.log_level, sizeof r.log_level, "debug");
    snprintf(r.log_file, sizeof r.log_file, "%s", path);

    radiod_log_start();
    radiod_log_configure(&r);
    HLOGI("radiod", "hello %d", 42);
    HLOGD("hamlib", "a detail");
    radiod_log_stop();

    const char *log = slurp(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log, "] [INF] [radiod] hello 42\n"), test_msg("file: %s", log));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log, "] [DBG] [hamlib] a detail\n"), test_msg("file: %s", log));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log, "writing log to "), test_msg("file: %s", log));
    /* YYYY-MM-DD hh:mm:ss.mmm [+s.mmms] [LVL] [component] message, local time */
    int y, mo, d, hh, mm, ss, ms;
    unsigned up_s, up_ms;
    const char *line = strstr(log, "hello 42");
    while (line > log && line[-1] != '\n')
        line--;
    TEST_ASSERT_EQUAL_INT_MESSAGE(9, sscanf(line, "%4d-%2d-%2d %2d:%2d:%2d.%3d [+%u.%3us]",
                                            &y, &mo, &d, &hh, &mm, &ss, &ms, &up_s, &up_ms),
                                  test_msg("line: %.80s", line));
    TEST_ASSERT_EQUAL_CHAR_MESSAGE(' ', line[23], test_msg("local time has no Z: %.80s", line));
}

/* main:log_utc: the same stamp in UTC, with a Z. */
static void test_file_sink_utc(void)
{
    char path[sizeof r.log_file];
    snprintf(path, sizeof path, "%s/radiod-utc.log", tmpdir);
    snprintf(r.log_file, sizeof r.log_file, "%s", path);
    r.log_utc = true;

    radiod_log_start();
    radiod_log_configure(&r);
    HLOGI("radiod", "in utc");
    radiod_log_stop();
    r.log_utc = false;

    const char *log = slurp(path);
    const char *line = strstr(log, "in utc");
    TEST_ASSERT_NOT_NULL_MESSAGE(line, test_msg("file: %s", log));
    while (line > log && line[-1] != '\n')
        line--;
    int y, mo, d, hh, mm, ss, ms;
    char z = 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(8, sscanf(line, "%4d-%2d-%2d %2d:%2d:%2d.%3d%c", &y, &mo, &d, &hh, &mm, &ss, &ms, &z),
                                  test_msg("line: %.80s", line));
    TEST_ASSERT_EQUAL_CHAR_MESSAGE('Z', z, test_msg("line: %.80s", line));
}

static void test_file_sink_jsonl(void)
{
    char path[sizeof r.log_file];
    snprintf(path, sizeof path, "%s/radiod.jsonl", tmpdir);
    snprintf(r.log_file, sizeof r.log_file, "%s", path);
    r.log_jsonl = true;

    radiod_log_start();
    radiod_log_configure(&r);
    HLOGW("ws", "a \"quoted\" word");
    radiod_log_stop();

    const char *log = slurp(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log, "\"lv\":\"WRN\",\"c\":\"ws\",\"m\":\"a \\\"quoted\\\" word\"}"),
                                 test_msg("file: %s", log));
}

/* A bad level is reported and info is used: debug lines are not written. */
static void test_bad_level(void)
{
    char path[sizeof r.log_file];
    snprintf(path, sizeof path, "%s/bad-level.log", tmpdir);
    snprintf(r.log_level, sizeof r.log_level, "loud");
    snprintf(r.log_file, sizeof r.log_file, "%s", path);

    radiod_log_start();
    radiod_log_configure(&r);
    HLOGI("radiod", "kept");
    HLOGD("radiod", "dropped");
    radiod_log_stop();

    const char *log = slurp(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log, "[INF] [radiod] kept"), test_msg("file: %s", log));
    TEST_ASSERT_NULL_MESSAGE(strstr(log, "dropped"), test_msg("file: %s", log));
}

int main(void)
{
    snprintf(tmpdir, sizeof tmpdir, "/tmp/radiod_log_test.XXXXXX");
    const char *t = getenv("TMPDIR");
    if (t && t[0])
        snprintf(tmpdir, sizeof tmpdir, "%s/radiod_log_test.XXXXXX", t);
    TEST_REQUIRE(mkdtemp(tmpdir));

    UNITY_BEGIN();
    RUN_TEST(test_level_names);
    RUN_TEST(test_component_list);
    RUN_TEST(test_file_sink);
    RUN_TEST(test_file_sink_utc);
    RUN_TEST(test_file_sink_jsonl);
    RUN_TEST(test_bad_level);
    int rc = UNITY_END();

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", tmpdir);
    return rc;
}
