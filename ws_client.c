/*
 * ws_client - hermes-radio-daemon control CLI over the websocket API.
 *
 * Drop-in companion to radio_client (which talks SHM directly): same
 * command style, but everything goes through the daemon's websocket
 * interface, so it works across the network and on all backends.
 *
 *   ws_client -u wss://host:8080 -c get_mode
 *   ws_client -c set_mode -a DSTAR
 *   ws_client -c set_frequency -a 7045000
 *   ws_client -c ptt_on
 *   ws_client -c digi_set_config -a dstar_verbose=1
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mongoose.h"

static const char *g_url = "wss://127.0.0.1:8080";
static const char *g_cmd = NULL;
static const char *g_arg = NULL;
static int g_profile = -1;
static int g_done = 0;
static int g_ok = 1;

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -c command [-a argument] [-p profile] [-u url] [-h]\n"
            "\n"
            "  -c command    Command to run (see list below)\n"
            "  -a argument   Argument for commands that take one\n"
            "  -p profile    Profile number to apply the command to\n"
            "  -u url        Daemon websocket URL (default: %s)\n"
            "  -h            Show this help\n"
            "\n"
            "Commands:\n"
            "  get_state                       Full state dump\n"
            "  get_frequency | set_frequency   Set requires -a <Hz>\n"
            "  get_mode | set_mode             Set requires -a <LSB|USB|CW|FM|AM|DRM|FT8|RTTY|DSTAR>\n"
            "  ptt_on | ptt_off\n"
            "  get_profile | set_profile      Set requires -a <number>\n"
            "  get_power | set_power          Set requires -a <0..100>\n"
            "  get_volume | set_volume        Set requires -a <0..100>\n"
            "  get_digital_voice | set_digital_voice   Set requires -a <ON|OFF>\n"
            "  get_txrx_status\n"
            "  get_protection_status | reset_protection\n"
            "  get_fwd | get_ref | get_ref_power\n"
            "  get_led_status | set_led_status        Set requires -a <ON|OFF>\n"
            "  get_connected_status | set_connected_status\n"
            "  get_serial | set_serial        Set requires -a <number>\n"
            "  get_bfo | set_bfo              Set requires -a <Hz>\n"
            "  get_filter_width | set_filter_width    Set requires -a <Hz>\n"
            "  digi_get_config | digi_set_config      Set requires -a <key=value>\n"
            "  digi_send                      Queue text for TX in the active mode (FT8/CW/RTTY), -a <text>\n"
            "  digi_messages                  Decoded/sent digi messages (-a <count>, default 20)\n"
            "  get_message | get_timeout | reset_timeout\n",
            prog, g_url);
}

/* Build the JSON payload for a command. Returns NULL if the command is
 * unknown or its argument is missing/invalid. */
static char *build_payload(void)
{
    char *json = calloc(1, 512);
    char profile[32] = "";
    if (g_profile >= 0)
        snprintf(profile, sizeof(profile), ",\"profile\":%d", g_profile);

#define SIMPLE(cmdstr) do { snprintf(json, 512, "{\"cmd\":\"%s\"%s}", cmdstr, profile); return json; } while (0)
#define NUMBER(cmdstr) do { \
        if (!g_arg) return NULL; \
        snprintf(json, 512, "{\"cmd\":\"%s\",\"value\":%ld%s}", cmdstr, atol(g_arg), profile); \
        return json; } while (0)
#define STRING(cmdstr) do { \
        if (!g_arg) return NULL; \
        snprintf(json, 512, "{\"cmd\":\"%s\",\"value\":\"%s\"%s}", cmdstr, g_arg, profile); \
        return json; } while (0)

    if (!strcmp(g_cmd, "get_state"))              SIMPLE("get_state");
    if (!strcmp(g_cmd, "get_frequency"))          SIMPLE("get_frequency");
    if (!strcmp(g_cmd, "set_frequency"))          NUMBER("set_frequency");
    if (!strcmp(g_cmd, "get_mode"))               SIMPLE("get_mode");
    if (!strcmp(g_cmd, "set_mode"))               STRING("set_mode");
    if (!strcmp(g_cmd, "ptt_on"))                 SIMPLE("ptt_on");
    if (!strcmp(g_cmd, "ptt_off"))                SIMPLE("ptt_off");
    if (!strcmp(g_cmd, "get_profile"))            SIMPLE("get_profile");
    if (!strcmp(g_cmd, "set_profile"))            NUMBER("set_profile");
    if (!strcmp(g_cmd, "get_power"))              SIMPLE("get_power");
    if (!strcmp(g_cmd, "set_power"))              NUMBER("set_power");
    if (!strcmp(g_cmd, "get_volume"))             SIMPLE("get_volume");
    if (!strcmp(g_cmd, "set_volume"))             NUMBER("set_volume");
    if (!strcmp(g_cmd, "get_digital_voice"))      SIMPLE("get_digital_voice");
    if (!strcmp(g_cmd, "set_digital_voice"))      STRING("set_digital_voice");
    if (!strcmp(g_cmd, "get_txrx_status"))        SIMPLE("get_txrx_status");
    if (!strcmp(g_cmd, "get_protection_status"))  SIMPLE("get_protection_status");
    if (!strcmp(g_cmd, "reset_protection"))       SIMPLE("reset_protection");
    if (!strcmp(g_cmd, "reset_timeout"))          SIMPLE("reset_timeout");
    if (!strcmp(g_cmd, "get_bfo"))                SIMPLE("get_bfo");
    if (!strcmp(g_cmd, "set_bfo"))                NUMBER("set_bfo");
    if (!strcmp(g_cmd, "get_fwd"))                SIMPLE("get_fwd");
    if (!strcmp(g_cmd, "get_ref"))                SIMPLE("get_ref");
    if (!strcmp(g_cmd, "get_ref_power"))          SIMPLE("get_ref_power");
    if (!strcmp(g_cmd, "get_led_status"))         SIMPLE("get_led_status");
    if (!strcmp(g_cmd, "set_led_status"))         STRING("set_led_status");
    if (!strcmp(g_cmd, "get_connected_status"))   SIMPLE("get_connected_status");
    if (!strcmp(g_cmd, "set_connected_status"))   STRING("set_connected_status");
    if (!strcmp(g_cmd, "get_serial"))             SIMPLE("get_serial");
    if (!strcmp(g_cmd, "set_serial"))             NUMBER("set_serial");
    if (!strcmp(g_cmd, "get_filter_width"))       SIMPLE("get_filter_width");
    if (!strcmp(g_cmd, "set_filter_width"))       NUMBER("set_filter_width");
    if (!strcmp(g_cmd, "digi_get_config"))        SIMPLE("digi_get_config");
    if (!strcmp(g_cmd, "digi_set_config")) {
        /* key=value argument */
        char key[64], value[64];
        if (!g_arg || sscanf(g_arg, "%63[^=]=%63s", key, value) != 2)
            return NULL;
        snprintf(json, 512, "{\"cmd\":\"digi_set_config\",\"key\":\"%s\",\"value\":\"%s\"%s}",
                 key, value, profile);
        return json;
    }
    if (!strcmp(g_cmd, "digi_send")) {
        /* Free text: escape what JSON needs escaped. */
        if (!g_arg || !*g_arg)
            return NULL;
        char text[256];
        size_t k = 0;
        for (const char *q = g_arg; *q && k + 2 < sizeof(text); q++) {
            if (*q == '"' || *q == '\\')
                text[k++] = '\\';
            text[k++] = *q;
        }
        text[k] = '\0';
        snprintf(json, 512, "{\"cmd\":\"digi_send\",\"text\":\"%s\"%s}", text, profile);
        return json;
    }
    if (!strcmp(g_cmd, "digi_messages")) {
        snprintf(json, 512, "{\"cmd\":\"digi_messages\",\"count\":%ld%s}",
                 g_arg ? atol(g_arg) : 20L, profile);
        return json;
    }
    if (!strcmp(g_cmd, "get_message"))            SIMPLE("get_message");
    if (!strcmp(g_cmd, "get_timeout"))            SIMPLE("get_timeout");

    return NULL;
}

/* Print the response compactly: for the state dump pass it through as-is,
 * for ok/status/value responses print the salient field. */
static void print_response(const char *resp)
{
    /* Minimal JSON field extraction, no library. */
    const char *ok = strstr(resp, "\"ok\":true");
    const char *bad = strstr(resp, "\"ok\":false");
    const char *valn = strstr(resp, "\"value\":");
    const char *vals = strstr(resp, "\"value\":\"");
    const char *sta = strstr(resp, "\"status\":\"");
    const char *type = strstr(resp, "\"type\":\"state\"");

    if (type || strstr(resp, "\"cmd\":\"digi_messages\""))
        printf("%s\n", resp);
    else if (bad) {
        if (sta) {
            char s[128];
            sscanf(sta + 10, "%127[^\"]", s);
            printf("%s\n", s);
        } else {
            printf("ERROR\n");
        }
        g_ok = 0;
    } else if (ok) {
        if (sta) {
            char s[128];
            sscanf(sta + 10, "%127[^\"]", s);
            printf("%s\n", s);
        } else if (vals) {
            char s[128];
            sscanf(vals + 9, "%127[^\"]", s);
            printf("%s\n", s);
        } else if (valn) {
            printf("%.*s\n", (int) strcspn(valn + 8, ",}"), valn + 8);
        } else {
            printf("OK\n");
        }
    } else {
        printf("%s\n", resp);
    }
}

static void on_event(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_CONNECT && mg_url_is_ssl(g_url)) {
        /* The daemon terminates wss:// with a self-signed certificate:
         * encrypt, but do not verify. Without this the TLS handshake never
         * started and every wss:// command closed silently. */
        struct mg_tls_opts opts = {.skip_verification = 1};
        mg_tls_init(c, &opts);
    } else if (ev == MG_EV_WS_OPEN) {
        char *payload = build_payload();
        if (!payload) {
            fprintf(stderr, "unknown command '%s' or missing/invalid argument\n", g_cmd);
            g_ok = 0;
            c->is_draining = 1;
            return;
        }
        mg_ws_send(c, payload, strlen(payload), WEBSOCKET_OP_TEXT);
        free(payload);
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
        if ((wm->flags & 0x0F) == WEBSOCKET_OP_BINARY)
            return; /* audio/spectrum stream frames, not our reply */
        char buf[16384];
        size_t n = wm->data.len < sizeof(buf) - 1 ? wm->data.len : sizeof(buf) - 1;
        memcpy(buf, wm->data.buf, n);
        buf[n] = '\0';
        if (strstr(buf, "\"type\":\"hello\""))
            return; /* skip the server hello, wait for our response */
        /* Skip unsolicited state pushes and replies to other commands. */
        if (strstr(buf, "\"type\":\"state\"") && strcmp(g_cmd, "get_state") != 0)
            return;
        {
            char want[80];
            snprintf(want, sizeof(want), "\"cmd\":\"%s\"", g_cmd);
            if (strstr(buf, "\"cmd\":\"") && !strstr(buf, want))
                return;
        }
        print_response(buf);
        g_done = 1;
        c->is_draining = 1;
    } else if (ev == MG_EV_ERROR) {
        fprintf(stderr, "ws error: %s\n", (char *) ev_data);
        g_ok = 0;
        g_done = 1;
        c->is_draining = 1;
    } else if (ev == MG_EV_CLOSE) {
        if (!g_done) {
            fprintf(stderr, "connection closed before a response\n");
            g_ok = 0;
        }
        g_done = 1;
    }
}

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "c:a:p:u:h")) != -1) {
        switch (opt) {
        case 'c': g_cmd = optarg; break;
        case 'a': g_arg = optarg; break;
        case 'p': g_profile = atoi(optarg); break;
        case 'u': g_url = optarg; break;
        case 'h':
        default:
            print_usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    if (!g_cmd) {
        print_usage(argv[0]);
        return 1;
    }

    struct mg_mgr mgr;
    mg_log_set(MG_LL_ERROR);
    mg_mgr_init(&mgr);
    if (mg_ws_connect(&mgr, g_url, on_event, NULL, NULL) == NULL) {
        fprintf(stderr, "cannot connect to %s\n", g_url);
        return 1;
    }

    int ticks = 0;
    while (!g_done && ticks++ < 500) { /* ~5 s timeout */
        mg_mgr_poll(&mgr, 10);
    }
    mg_mgr_free(&mgr);

    if (!g_done) {
        fprintf(stderr, "timed out waiting for response\n");
        return 1;
    }
    return g_ok ? 0 : 1;
}
