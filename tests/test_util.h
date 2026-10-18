/* test_util.h - small helpers shared by radiod's Unity tests.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* A formatted message for Unity's *_MESSAGE asserts, e.g.
 *   TEST_ASSERT_EQUAL_INT_MESSAGE(0, n, test_msg("pass %d, frame %d", pass, f));
 * One buffer: valid until the next call. */
__attribute__((format(printf, 1, 2)))
static inline const char *test_msg(const char *fmt, ...)
{
    static char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

/* For one-time setup in main(), outside any test: Unity's asserts may only
 * run inside RUN_TEST (they longjmp back to it), so setup that cannot go on
 * says why and exits. */
#define TEST_REQUIRE(cond) do { if (!(cond)) { \
        fprintf(stderr, "%s:%d: setup failed: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); } } while (0)

#endif /* TEST_UTIL_H */
