/* hamlib_conf - the main:hamlib_conf pass-through.
 *
 * "key=value[,key=value...]", pairs separated by commas, semicolons or
 * whitespace, as rigctl --set-conf takes them (dtr_state=ON, civaddr=0x94,
 * ...). Free of Hamlib types so it is unit-tested on any host; the same
 * syntax as Mercury's [ptt] hamlib_conf.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HAMLIB_CONF_H
#define HAMLIB_CONF_H

#include <stdbool.h>
#include <stddef.h>

#define HAMLIB_CONF_MAX 256

typedef void (*hamlib_conf_pair_cb)(const char *key, const char *value, void *ctx);

/* Calls cb for every pair and returns how many there were, or -1 (calling cb
 * for none) if an item has no '=' or an empty key, or the string is too long:
 * a half-applied configuration is worse than none. */
static inline int hamlib_conf_pairs(const char *s, hamlib_conf_pair_cb cb, void *ctx)
{
    char buf[HAMLIB_CONF_MAX];
    int n = 0;

    if (!s)
        return 0;
    size_t len = 0;
    while (s[len])
        len++;
    if (len >= sizeof(buf))
        return -1;

    /* pass 0 validates, pass 1 calls back */
    for (int pass = 0; pass < 2; pass++)
    {
        size_t i = 0;
        for (size_t k = 0; k <= len; k++)
            buf[k] = s[k];
        n = 0;
        while (buf[i])
        {
            while (buf[i] == ',' || buf[i] == ';' || buf[i] == ' ' || buf[i] == '\t')
                i++;
            if (!buf[i])
                break;
            size_t start = i, eq = 0;
            while (buf[i] && buf[i] != ',' && buf[i] != ';' && buf[i] != ' ' && buf[i] != '\t')
            {
                if (buf[i] == '=' && !eq)
                    eq = i;
                i++;
            }
            if (!eq || eq == start)
                return -1;
            char sep = buf[i];
            buf[i] = '\0';
            buf[eq] = '\0';
            if (pass == 1 && cb)
                cb(buf + start, buf + eq + 1, ctx);
            n++;
            if (sep)
                i++;
        }
    }
    return n;
}

static inline bool hamlib_conf_ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
    {
        char x = (*a >= 'A' && *a <= 'Z') ? (char) (*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char) (*b + 32) : *b;
        if (x != y)
            return false;
    }
    return *a == *b;
}

typedef struct { const char *key; bool found; } hamlib_conf_find_;

static inline void hamlib_conf_find_cb_(const char *key, const char *value, void *ctx)
{
    hamlib_conf_find_ *f = (hamlib_conf_find_ *) ctx;
    (void) value;
    if (hamlib_conf_ieq(key, f->key))
        f->found = true;
}

/* The (well-formed) string sets `key`, case-insensitively. */
static inline bool hamlib_conf_sets(const char *s, const char *key)
{
    hamlib_conf_find_ f = { key, false };
    if (hamlib_conf_pairs(s, hamlib_conf_find_cb_, &f) < 0)
        return false;
    return f.found;
}

#endif /* HAMLIB_CONF_H */
