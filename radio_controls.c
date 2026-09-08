/* hermes-radio-daemon - generic radio control surface
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Backend-neutral helpers over the control vtable: capability enumeration
 * shaped into JSON for the web UI, value clamping, and the RADIO_CTRL_* to
 * rigctld RPRT mapping used by the network rig server.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "radio_backend.h"
#include "radio_controls.h"

size_t radio_controls_enumerate(radio *radio_h, radio_ctrl_info *out, size_t max)
{
    if (!radio_h || !out || max == 0)
        return 0;

    return radio_backend_enumerate_controls(radio_h, out, max);
}

const char *radio_controls_kind_name(radio_ctrl_kind kind)
{
    switch (kind)
    {
    case RADIO_CTRL_FUNC:  return "func";
    case RADIO_CTRL_PARM:  return "parm";
    case RADIO_CTRL_LEVEL:
    default:               return "level";
    }
}

bool radio_controls_kind_from_name(const char *name, radio_ctrl_kind *out)
{
    if (!name || !out)
        return false;

    if (!strcmp(name, "level")) { *out = RADIO_CTRL_LEVEL; return true; }
    if (!strcmp(name, "func"))  { *out = RADIO_CTRL_FUNC;  return true; }
    if (!strcmp(name, "parm"))  { *out = RADIO_CTRL_PARM;  return true; }

    return false;
}

bool radio_controls_find(radio *radio_h, const char *name, radio_ctrl_info *out)
{
    radio_ctrl_info list[RADIO_CTRL_MAX];

    if (!radio_h || !name || !*name)
        return false;

    size_t n = radio_controls_enumerate(radio_h, list, RADIO_CTRL_MAX);
    for (size_t i = 0; i < n; i++)
    {
        if (strcmp(list[i].name, name) != 0)
            continue;
        if (out)
            *out = list[i];
        return true;
    }

    return false;
}

double radio_controls_clamp(const radio_ctrl_info *info, double value)
{
    if (!info)
        return value;

    if (info->kind == RADIO_CTRL_FUNC)
        return value != 0.0 ? 1.0 : 0.0;

    /* min==max==0 means the backend reported no granularity: pass through. */
    if (info->min != 0.0 || info->max != 0.0)
    {
        if (value < info->min) value = info->min;
        if (value > info->max) value = info->max;
    }

    if (info->step > 0.0)
    {
        double base = info->min;
        value = base + round((value - base) / info->step) * info->step;
        if (info->max != 0.0 && value > info->max) value = info->max;
        if (value < info->min) value = info->min;
    }

    if (!info->is_float)
        value = round(value);

    return value;
}

int radio_controls_rprt(int rc)
{
    switch (rc)
    {
    case RADIO_CTRL_OK:      return 0;
    case RADIO_CTRL_ENOTSUP: return -11;  /* RIG_ENAVAIL */
    case RADIO_CTRL_EINVAL:  return -1;   /* RIG_EINVAL */
    case RADIO_CTRL_EIO:
    default:                 return -8;   /* RIG_EIO */
    }
}

/* Append to a bounded buffer, tracking the write offset. Returns false once
 * the buffer is full so callers can stop early instead of silently cutting a
 * JSON document in half. */
static bool json_append(char *out, size_t out_len, size_t *off, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static bool json_append(char *out, size_t out_len, size_t *off, const char *fmt, ...)
{
    if (*off >= out_len)
        return false;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *off, out_len - *off, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t) n >= out_len - *off)
    {
        *off = out_len;
        return false;
    }

    *off += (size_t) n;
    return true;
}

int radio_controls_caps_json(radio *radio_h, char *out, size_t out_len)
{
    radio_ctrl_info list[RADIO_CTRL_MAX];
    size_t off = 0;

    if (!radio_h || !out || out_len == 0)
        return RADIO_CTRL_EINVAL;

    size_t n = radio_controls_enumerate(radio_h, list, RADIO_CTRL_MAX);

    if (!json_append(out, out_len, &off,
                     "{\"cmd\":\"get_controls\",\"ok\":true,\"count\":%zu,\"controls\":[", n))
        return RADIO_CTRL_EINVAL;

    for (size_t i = 0; i < n; i++)
    {
        if (!json_append(out, out_len, &off,
                         "%s{\"name\":\"%s\",\"kind\":\"%s\",\"type\":\"%s\","
                         "\"get\":%s,\"set\":%s,\"min\":%g,\"max\":%g,\"step\":%g}",
                         i ? "," : "",
                         list[i].name,
                         radio_controls_kind_name(list[i].kind),
                         list[i].is_float ? "float" : "int",
                         list[i].can_get ? "true" : "false",
                         list[i].can_set ? "true" : "false",
                         list[i].min, list[i].max, list[i].step))
            return RADIO_CTRL_EINVAL;
    }

    if (!json_append(out, out_len, &off, "]}"))
        return RADIO_CTRL_EINVAL;

    return RADIO_CTRL_OK;
}

/* True when `names` (comma-separated, whitespace tolerated) contains name. */
static bool name_selected(const char *names, const char *name)
{
    if (!names || !*names)
        return true;

    size_t len = strlen(name);
    const char *p = names;

    while (*p)
    {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        size_t n = (size_t) (p - start);
        while (n > 0 && (start[n - 1] == ' ' || start[n - 1] == '\t')) n--;
        if (n == len && !strncmp(start, name, len))
            return true;
    }

    return false;
}

int radio_controls_values_json(radio *radio_h, const char *names,
                               char *out, size_t out_len)
{
    radio_ctrl_info list[RADIO_CTRL_MAX];
    size_t off = 0;
    bool first = true;

    if (!radio_h || !out || out_len == 0)
        return RADIO_CTRL_EINVAL;

    size_t n = radio_controls_enumerate(radio_h, list, RADIO_CTRL_MAX);

    if (!json_append(out, out_len, &off,
                     "{\"cmd\":\"get_control_values\",\"ok\":true,\"values\":{"))
        return RADIO_CTRL_EINVAL;

    for (size_t i = 0; i < n; i++)
    {
        if (!list[i].can_get)
            continue;
        if (!name_selected(names, list[i].name))
            continue;

        double value = 0.0;
        int rc;

        switch (list[i].kind)
        {
        case RADIO_CTRL_FUNC:
        {
            int on = 0;
            rc = radio_backend_get_func(radio_h, list[i].name, &on);
            value = on ? 1.0 : 0.0;
            break;
        }
        case RADIO_CTRL_PARM:
            rc = radio_backend_get_parm(radio_h, list[i].name, &value);
            break;
        case RADIO_CTRL_LEVEL:
        default:
            rc = radio_backend_get_level(radio_h, list[i].name, &value);
            break;
        }

        /* A control the rig advertises but refuses to report right now is
         * skipped rather than published as a zero. */
        if (rc != RADIO_CTRL_OK)
            continue;

        if (!json_append(out, out_len, &off, "%s\"%s\":%g",
                         first ? "" : ",", list[i].name, value))
            return RADIO_CTRL_EINVAL;
        first = false;
    }

    if (!json_append(out, out_len, &off, "}}"))
        return RADIO_CTRL_EINVAL;

    return RADIO_CTRL_OK;
}
