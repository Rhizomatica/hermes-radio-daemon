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
 * The transport is the in-tree websocket/ frame codec (ws_frame/ws_crypto)
 * over plain sockets, with OpenSSL for wss://.  It used to link Mongoose;
 * that dependency is gone.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "websocket/ws_frame.h"
#include "websocket/ws_crypto.h"

static const char *g_url = "wss://127.0.0.1:8080";
static const char *g_cmd = NULL;
static const char *g_arg = NULL;
static int g_profile = -1;
static int g_done = 0;
static int g_ok = 1;

/* ---- link: TCP socket plus optional TLS ---- */

typedef struct {
    int      fd;
    SSL     *ssl;
    SSL_CTX *ctx;
} link_t;

static int link_write(link_t *l, const void *buf, size_t len)
{
    if (l->ssl != NULL)
    {
        int n = SSL_write(l->ssl, buf, (int) len);
        return (n > 0) ? n : -1;
    }
    {
        int n = (int) send(l->fd, buf, len, MSG_NOSIGNAL);
        return (n > 0) ? n : -1;
    }
}

static int link_read(link_t *l, void *buf, size_t len)
{
    if (l->ssl != NULL)
    {
        int n = SSL_read(l->ssl, buf, (int) len);
        if (n > 0) return n;
        {
            int e = SSL_get_error(l->ssl, n);
            if (e == SSL_ERROR_ZERO_RETURN) return 0;
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return -2;
            return -1;
        }
    }
    {
        ssize_t n = recv(l->fd, buf, len, 0);
        if (n > 0) return (int) n;
        if (n == 0) return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        return -1;
    }
}

static void link_close(link_t *l)
{
    if (l->ssl != NULL) { SSL_shutdown(l->ssl); SSL_free(l->ssl); }
    if (l->ctx != NULL) SSL_CTX_free(l->ctx);
    if (l->fd >= 0) close(l->fd);
}

/* ---- command payload / response printing (transport-independent) ---- */

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
            "  get_message | get_timeout | reset_timeout\n"
            "  raw -a '<json>'                Send a JSON command as is and print the reply\n",
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
        snprintf(json, 512, "{\"cmd\":\"digi_config\",\"key\":\"%s\",\"value\":\"%s\"%s}",
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
    if (!strcmp(g_cmd, "raw")) {
        if (!g_arg || g_arg[0] != '{')
            return NULL;
        snprintf(json, 512, "%s", g_arg);
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
        const char *err = strstr(resp, "\"error\":\"");
        if (err) {
            char e[256];
            sscanf(err + 9, "%255[^\"]", e);
            printf("ERROR: %s\n", e);
        } else if (sta) {
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

/* Decide whether a received text frame is the reply we are waiting for. */
static void handle_text(const char *buf)
{
    if (strstr(buf, "\"type\":\"hello\""))
        return; /* skip the server hello, wait for our response */
    /* Skip unsolicited state pushes and replies to other commands. */
    if (strstr(buf, "\"type\":\"state\"") && strcmp(g_cmd, "get_state") != 0)
        return;
    if (!strcmp(g_cmd, "raw")) {
        printf("%s\n", buf);
        g_done = 1;
        return;
    }
    {
        char want[80];
        snprintf(want, sizeof(want), "\"cmd\":\"%s\"", g_cmd);
        if (strstr(buf, "\"cmd\":\"") && !strstr(buf, want))
            return;
    }
    print_response(buf);
    g_done = 1;
}

/* ---- URL parsing, handshake, framing ---- */

static int parse_url(const char *url, int *tls, char *host, size_t host_len,
                     char *port, size_t port_len, char *path, size_t path_len)
{
    const char *p = url;
    const char *slash, *colon;
    size_t hostpart;

    *tls = 0;
    if (strncmp(p, "ws://", 5) == 0)      p += 5;
    else if (strncmp(p, "wss://", 6) == 0) { *tls = 1; p += 6; }
    else return -1;

    slash = strchr(p, '/');
    hostpart = slash ? (size_t) (slash - p) : strlen(p);

    /* An IPv6 literal is bracketed, "[::1]:8080". */
    const char *hstart = p;
    size_t hlen;
    if (hostpart > 0 && p[0] == '[') {
        const char *close = memchr(p, ']', hostpart);
        if (close == NULL) return -1;
        hstart = p + 1;
        hlen = (size_t) (close - hstart);
        colon = (close + 1 < p + hostpart) ? close + 1 : NULL;
        if (colon != NULL && *colon != ':') return -1;
    } else {
        colon = memchr(p, ':', hostpart);
        hlen = colon ? (size_t) (colon - p) : hostpart;
    }

    if (colon != NULL) {
        size_t plen = hostpart - (size_t) (colon - p) - 1;
        if (plen == 0 || plen >= port_len) return -1;
        memcpy(port, colon + 1, plen);
        port[plen] = '\0';
    } else {
        snprintf(port, port_len, "8080");
    }
    if (hlen == 0 || hlen >= host_len) return -1;
    memcpy(host, hstart, hlen);
    host[hlen] = '\0';

    snprintf(path, path_len, "%s", (slash && slash[1]) ? slash : "/");
    return 0;
}

static int connect_tcp(const char *host, const char *port)
{
    struct addrinfo hints, *res, *rp;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0)
        return -1;

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Send one client frame (always masked, per RFC 6455). */
static int link_send_frame(link_t *l, int opcode, const void *data, size_t len)
{
    uint8_t hdr[14];
    uint8_t mask[4];
    size_t hlen;
    const uint8_t *p = (const uint8_t *) data;
    uint8_t *masked = NULL;
    size_t i;
    int rc = 0;

    if (RAND_bytes(mask, sizeof(mask)) != 1) {
        for (i = 0; i < sizeof(mask); i++) mask[i] = (uint8_t) rand();
    }

    hdr[0] = (uint8_t) (0x80 | (opcode & 0x0F));
    if (len < 126) {
        hdr[1] = (uint8_t) (0x80 | len);
        hlen = 2;
    } else if (len <= 0xFFFF) {
        hdr[1] = 0x80 | 126;
        hdr[2] = (uint8_t) (len >> 8);
        hdr[3] = (uint8_t) len;
        hlen = 4;
    } else {
        hdr[1] = 0x80 | 127;
        for (i = 0; i < 8; i++)
            hdr[2 + i] = (uint8_t) ((uint64_t) len >> (56 - i * 8));
        hlen = 10;
    }
    memcpy(hdr + hlen, mask, 4);
    hlen += 4;

    if (len > 0) {
        masked = malloc(len);
        if (masked == NULL) return -1;
        for (i = 0; i < len; i++) masked[i] = p[i] ^ mask[i & 3];
    }

    if (link_write(l, hdr, hlen) != (int) hlen) rc = -1;
    if (rc == 0 && len > 0 && link_write(l, masked, len) != (int) len) rc = -1;

    free(masked);
    return rc;
}

/* How long to wait for the reply to our command, in total. */
#define WS_REPLY_TIMEOUT_S 5

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Perform the HTTP upgrade and read frames until the reply arrives.
 * Returns 0 on a reply, -1 on a failed or closed connection, and -2 after
 * WS_REPLY_TIMEOUT_S without a reply (already reported). */
static int run_session(link_t *l, const char *host, const char *port,
                       const char *path)
{
    uint8_t key_bytes[16];
    char key_b64[32];
    char req[512];
    char resp[1024];
    size_t rlen = 0;
    int got_hdr = 0;
    char *payload = NULL;

    if (RAND_bytes(key_bytes, sizeof(key_bytes)) != 1)
        return -1;
    if (ws_base64_encode(key_bytes, sizeof(key_bytes), key_b64, sizeof(key_b64)) == 0)
        return -1;

    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s%s%s:%s\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "\r\n",
             path, strchr(host, ':') ? "[" : "", host,
             strchr(host, ':') ? "]" : "", port, key_b64);
    if (link_write(l, req, strlen(req)) != (int) strlen(req))
        return -1;

    /* Read the HTTP response headers, then any bytes that follow. The
     * daemon sends the 101, its hello and the full status JSON (well over
     * 1 KiB) back to back, so the first read can fill `resp` -- that is only
     * an error if the header block itself has not ended by then. Failing
     * whenever the buffer filled rejected every real handshake. Bytes past
     * the header stay in `resp` and are handed to the frame parser below;
     * anything that did not fit is still in the socket and is read there. */
    while (!got_hdr) {
        int n = link_read(l, resp + rlen, sizeof(resp) - 1 - rlen);
        if (n == -2) {
            fprintf(stderr, "timed out waiting for the websocket handshake\n");
            return -2;
        }
        if (n <= 0) return -1;
        rlen += (size_t) n;
        resp[rlen] = '\0';
        if (strstr(resp, "\r\n\r\n") != NULL)
            got_hdr = 1;
        else if (rlen >= sizeof(resp) - 1)
            return -1;                 /* headers alone overflow the buffer */
    }

    if (strncmp(resp, "HTTP/1.1 101", 12) != 0 && strncmp(resp, "HTTP/1.0 101", 12) != 0) {
        fprintf(stderr, "ws handshake failed: %.40s\n", resp);
        return -1;
    }

    payload = build_payload();
    if (payload == NULL) {
        fprintf(stderr, "unknown command '%s' or missing/invalid argument\n", g_cmd);
        g_ok = 0;
        return -1;
    }
    if (link_send_frame(l, WS_OP_TEXT, payload, strlen(payload)) != 0) {
        free(payload);
        return -1;
    }
    free(payload);

    /* Bytes already read past the header block, if any, are frame data. */
    {
        char *body = strstr(resp, "\r\n\r\n") + 4;
        size_t body_len = rlen - (size_t) (body - resp);
        static uint8_t rbuf[262144];
        size_t have = 0;

        if (body_len > 0) {
            if (body_len > sizeof(rbuf)) body_len = sizeof(rbuf);
            memcpy(rbuf, body, body_len);
            have = body_len;
        }

        /* The deadline is checked on every pass, not only when a read times
         * out: the daemon broadcasts status and audio continuously, so reads
         * keep succeeding and a missing reply would never produce a timeout
         * tick. Retrying ticks with no deadline at all hung forever. */
        const double deadline = mono_now() + WS_REPLY_TIMEOUT_S;
        while (!g_done) {
            if (mono_now() >= deadline) {
                fprintf(stderr, "timed out waiting for response\n");
                g_ok = 0;
                return -2;
            }
            ws_frame_t f;
            int rc = ws_frame_parse(rbuf, have, false, &f);

            if (rc == WS_FRAME_PROTO_ERROR) {
                fprintf(stderr, "ws protocol error\n");
                return -1;
            }
            if (rc == WS_FRAME_INCOMPLETE || have < f.total_len) {
                if (have >= sizeof(rbuf)) {
                    fprintf(stderr, "ws frame too large\n");
                    return -1;
                }
                {
                    int n = link_read(l, rbuf + have, sizeof(rbuf) - have);
                    if (n == -2) continue;      /* timeout tick */
                    if (n <= 0) return -1;
                    have += (size_t) n;
                }
                continue;
            }

            /* Full frame available. */
            {
                uint8_t *pl = rbuf + f.header_len;
                if (f.masked) ws_frame_unmask(pl, (size_t) f.payload_len, f.mask);

                if (f.opcode == WS_OP_TEXT) {
                    char text[16384];
                    size_t n = f.payload_len < sizeof(text) - 1
                             ? (size_t) f.payload_len : sizeof(text) - 1;
                    memcpy(text, pl, n);
                    text[n] = '\0';
                    handle_text(text);
                } else if (f.opcode == WS_OP_CLOSE) {
                    return g_done ? 0 : -1;
                }
                /* BINARY (audio/spectrum) and PING are ignored. */
            }

            memmove(rbuf, rbuf + f.total_len, have - f.total_len);
            have -= f.total_len;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    int opt;
    int tls;
    char host[256], port[16], path[256];
    link_t link;

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

    if (parse_url(g_url, &tls, host, sizeof(host), port, sizeof(port),
                  path, sizeof(path)) != 0) {
        fprintf(stderr, "bad url: %s\n", g_url);
        return 1;
    }

    memset(&link, 0, sizeof(link));
    link.fd = -1;

    link.fd = connect_tcp(host, port);
    if (link.fd < 0) {
        fprintf(stderr, "cannot connect to %s\n", g_url);
        return 1;
    }

    /* Bound the whole exchange. */
    {
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(link.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    if (tls) {
        /* The daemon terminates wss:// with a self-signed certificate:
         * encrypt, but do not verify. */
        link.ctx = SSL_CTX_new(TLS_client_method());
        if (link.ctx == NULL) { link_close(&link); return 1; }
        SSL_CTX_set_verify(link.ctx, SSL_VERIFY_NONE, NULL);
        link.ssl = SSL_new(link.ctx);
        if (link.ssl == NULL) { link_close(&link); return 1; }
        SSL_set_fd(link.ssl, link.fd);
        if (SSL_connect(link.ssl) != 1) {
            fprintf(stderr, "TLS handshake failed\n");
            link_close(&link);
            return 1;
        }
    }

    int rc = run_session(&link, host, port, path);
    if (rc != 0) {
        link_close(&link);
        if (rc == -2)
            return 1;                  /* timeout, already reported */
        if (!g_done) {
            fprintf(stderr, "connection closed before a response\n");
            return 1;
        }
    }

    link_close(&link);
    return g_ok ? 0 : 1;
}
