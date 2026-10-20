/* radiod_log - radiod's logging, set up from core.ini. See radiod_log.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "radiod_log.h"

#define LOG_C "radiod"

void radiod_log_start(void)
{
    if (hermes_log_init(1024) != 0)
        fprintf(stderr, "radiod: the logger did not start; log lines are lost\n");
}

void radiod_log_stop(void)
{
    hermes_log_shutdown();
}

bool radiod_log_level(const char *name, hermes_log_level_t *out)
{
    static const struct { const char *name; hermes_log_level_t level; } levels[] = {
        { "trace", HERMES_LOG_LEVEL_TRACE }, { "debug", HERMES_LOG_LEVEL_DEBUG },
        { "timing", HERMES_LOG_LEVEL_TIMING },
        { "info", HERMES_LOG_LEVEL_INFO },   { "warn", HERMES_LOG_LEVEL_WARN },
        { "error", HERMES_LOG_LEVEL_ERROR },
    };
    for (size_t i = 0; name && i < sizeof(levels) / sizeof(levels[0]); i++)
        if (!strcasecmp(name, levels[i].name))
        {
            *out = levels[i].level;
            return true;
        }
    return false;
}

/* Copy one trimmed token of [b, e) into out. */
static void token(const char *b, const char *e, char *out, size_t len)
{
    while (b < e && isspace((unsigned char) *b))
        b++;
    while (e > b && isspace((unsigned char) e[-1]))
        e--;
    size_t n = (size_t) (e - b) < len - 1 ? (size_t) (e - b) : len - 1;
    memcpy(out, b, n);
    out[n] = '\0';
}

int radiod_log_components(const char *spec,
                          void (*set)(const char *component, hermes_log_level_t level, void *user),
                          void *user)
{
    int n = 0;
    const char *p = spec ? spec : "";

    while (*p)
    {
        const char *end = strchr(p, ',');
        if (!end)
            end = p + strlen(p);
        const char *eq = memchr(p, '=', (size_t) (end - p));
        char comp[32], lvl[16];
        hermes_log_level_t level;

        token(p, end, comp, sizeof(comp));
        if (comp[0] != '\0')                       /* skip empty items: "a=b,," */
        {
            if (!eq)
                return -1;
            token(p, eq, comp, sizeof(comp));
            token(eq + 1, end, lvl, sizeof(lvl));
            if (comp[0] == '\0' || !radiod_log_level(lvl, &level))
                return -1;
            set(comp, level, user);
            n++;
        }
        p = *end ? end + 1 : end;
    }
    return n;
}

static void set_component(const char *component, hermes_log_level_t level, void *user)
{
    (void) user;
    hermes_log_set_component_level(component, level);
}

void radiod_log_configure(const radio *radio_h)
{
    hermes_log_level_t level = HERMES_LOG_LEVEL_INFO;

    if (!radiod_log_level(radio_h->log_level, &level))
        HLOGW(LOG_C, "main:log_level = %s is not trace, debug, timing, info, warn or error; using info",
              radio_h->log_level);
    hermes_log_set_level(level);

    if (radiod_log_components(radio_h->log_component_levels, set_component, NULL) < 0)
        HLOGW(LOG_C, "main:log_component_levels = %s: want \"component=level, ...\"; "
                     "the pairs after the first bad one are ignored",
              radio_h->log_component_levels);

    if (radio_h->log_file[0] == '\0')
        hermes_log_close_file();
    else                        /* hermes_log reports where it writes, or why not */
        hermes_log_set_file(radio_h->log_file, level, radio_h->log_jsonl);
}
