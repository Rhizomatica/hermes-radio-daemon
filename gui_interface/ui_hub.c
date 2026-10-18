/* ui_hub - the daemon's UI traffic, independent of any one transport.
 * See ui_hub.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_hub.h"

#define MAX_MODULES 4
#define MAX_SINKS   4

static ui_hub_module    s_modules[MAX_MODULES];
static int              s_nmodules;
static pthread_rwlock_t s_modules_lock = PTHREAD_RWLOCK_INITIALIZER;

static ui_hub_sink      s_sinks[MAX_SINKS];
static bool             s_sink_used[MAX_SINKS];
static pthread_rwlock_t s_sinks_lock = PTHREAD_RWLOCK_INITIALIZER;

/* ── modules ── */

bool ui_hub_register_module(const ui_hub_module *m)
{
    bool ok = false;

    if (!m || !m->name || !m->command_key)
        return false;
    pthread_rwlock_wrlock(&s_modules_lock);
    bool taken = false;
    for (int i = 0; i < s_nmodules; i++)
        taken |= !strcmp(s_modules[i].name, m->name);
    if (!taken && s_nmodules < MAX_MODULES)
    {
        s_modules[s_nmodules++] = *m;
        ok = true;
    }
    pthread_rwlock_unlock(&s_modules_lock);
    return ok;
}

void ui_hub_unregister_module(const char *name)
{
    pthread_rwlock_wrlock(&s_modules_lock);
    for (int i = 0; i < s_nmodules; i++)
        if (!strcmp(s_modules[i].name, name))
        {
            s_modules[i] = s_modules[--s_nmodules];
            break;
        }
    pthread_rwlock_unlock(&s_modules_lock);
}

void ui_hub_broadcast_text(const char *json)
{
    if (!json || !json[0])
        return;
    size_t len = strlen(json);
    pthread_rwlock_rdlock(&s_sinks_lock);
    for (int i = 0; i < MAX_SINKS; i++)
        if (s_sink_used[i] && s_sinks[i].text)
            s_sinks[i].text(json, len, s_sinks[i].user);
    pthread_rwlock_unlock(&s_sinks_lock);
}

void ui_hub_broadcast_stream(uint8_t stream_type, const void *data, size_t len)
{
    if (!data || !len)
        return;
    uint8_t *frame = malloc(len + 1);
    if (!frame)
        return;
    frame[0] = stream_type;
    memcpy(frame + 1, data, len);
    pthread_rwlock_rdlock(&s_sinks_lock);
    for (int i = 0; i < MAX_SINKS; i++)
        if (s_sink_used[i] && s_sinks[i].binary)
            s_sinks[i].binary(frame, len + 1, s_sinks[i].user);
    pthread_rwlock_unlock(&s_sinks_lock);
    free(frame);
}

/* ── transports ── */

int ui_hub_add_sink(const ui_hub_sink *sink)
{
    int handle = -1;

    if (!sink)
        return -1;
    pthread_rwlock_wrlock(&s_sinks_lock);
    for (int i = 0; i < MAX_SINKS && handle < 0; i++)
        if (!s_sink_used[i])
        {
            s_sinks[i] = *sink;
            s_sink_used[i] = true;
            handle = i;
        }
    pthread_rwlock_unlock(&s_sinks_lock);
    return handle;
}

void ui_hub_remove_sink(int handle)
{
    if (handle < 0 || handle >= MAX_SINKS)
        return;
    pthread_rwlock_wrlock(&s_sinks_lock);       /* waits for broadcasts in flight */
    s_sink_used[handle] = false;
    pthread_rwlock_unlock(&s_sinks_lock);
}

/* The string value of a top-level "key":"value" in a flat JSON object,
 * without its escapes resolved (command names have none). */
static bool json_string(const char *json, size_t len, const char *key, char *out, size_t out_len)
{
    char pat[48];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n < 0 || (size_t) n >= sizeof(pat))
        return false;
    const char *end = json + len;
    for (const char *p = json; p + n <= end; p++)
    {
        if (memcmp(p, pat, (size_t) n) != 0)
            continue;
        const char *q = p + n;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n'))
            q++;
        if (q >= end || *q != ':')
            continue;
        q++;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n'))
            q++;
        if (q >= end || *q != '"')
            return false;           /* the key, but not a string */
        q++;
        size_t o = 0;
        while (q < end && *q != '"')
        {
            if (*q == '\\' && q + 1 < end)
                q++;
            if (o + 1 < out_len)
                out[o++] = *q;
            q++;
        }
        if (q >= end)
            return false;
        out[o] = '\0';
        return true;
    }
    return false;
}

static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (; *in && o + 7 < out_len; in++)
    {
        unsigned char c = (unsigned char) *in;
        if (c == '"' || c == '\\')
        {
            out[o++] = '\\';
            out[o++] = (char) c;
        }
        else if (c < 0x20)
            o += (size_t) snprintf(out + o, out_len - o, "\\u%04x", c);
        else
            out[o++] = (char) c;
    }
    out[o] = '\0';
}

bool ui_hub_command(const char *json, size_t len, char *reply, size_t reply_len)
{
    char name[64], esc[160];
    bool handled = false;

    if (!json || !len || !reply || !reply_len)
        return false;

    pthread_rwlock_rdlock(&s_modules_lock);
    for (int i = 0; i < s_nmodules && !handled; i++)
    {
        ui_hub_module *m = &s_modules[i];
        if (!m->command || !json_string(json, len, m->command_key, name, sizeof(name)))
            continue;
        int rc = m->command(json, len, m->user);
        json_escape(name, esc, sizeof(esc));
        if (rc == 0)
            snprintf(reply, reply_len, "{\"%s\":\"%s\",\"status\":\"ok\"}", m->command_key, esc);
        else
            snprintf(reply, reply_len, "{\"%s\":\"%s\",\"status\":\"error\",\"code\":%d}",
                     m->command_key, esc, rc);
        handled = true;
    }
    pthread_rwlock_unlock(&s_modules_lock);

    /* Mercury's key with no module to take it (mercury not running):
     * answered as mercury's own server answers a command it cannot route. */
    if (!handled && json_string(json, len, "command", name, sizeof(name)))
    {
        json_escape(name, esc, sizeof(esc));
        snprintf(reply, reply_len, "{\"command\":\"%s\",\"error\":\"no handler\"}", esc);
        handled = true;
    }
    return handled;
}

void ui_hub_client_connected(void)
{
    pthread_rwlock_rdlock(&s_modules_lock);
    for (int i = 0; i < s_nmodules; i++)
        if (s_modules[i].client_connected)
            s_modules[i].client_connected(s_modules[i].user);
    pthread_rwlock_unlock(&s_modules_lock);
}

size_t ui_hub_hello_fields(char *out, size_t out_len)
{
    size_t n = 0;

    if (!out || !out_len)
        return 0;
#define APPEND(...) do { if (n < out_len) { \
        int w_ = snprintf(out + n, out_len - n, __VA_ARGS__); \
        if (w_ > 0) n += (size_t) w_; } } while (0)
    pthread_rwlock_rdlock(&s_modules_lock);
    APPEND("\"modules\":[");
    for (int i = 0; i < s_nmodules; i++)
        APPEND("%s\"%s\"", i ? "," : "", s_modules[i].name);
    APPEND("]");
    for (int i = 0; i < s_nmodules; i++)
        if (s_modules[i].stream_type)
            APPEND(",\"%s_binary_type\":%u", s_modules[i].name, s_modules[i].stream_type);
    pthread_rwlock_unlock(&s_modules_lock);
#undef APPEND
    if (n >= out_len)
        n = out_len - 1;
    return n;
}
