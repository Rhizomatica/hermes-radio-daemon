/* ws_server: HTTP requests for a REST handler, upgrade routing by path and
 * subprotocol, and messages by connection class or to one connection.
 * Talks to a real server on 127.0.0.1 over plain sockets.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "unity.h"
#include "test_util.h"
#include "websocket/ws_server.h"

static ws_server_t *srv, *tls_srv;
static uint16_t port, tls_port;
static char cert_path[256], key_path[256];
static _Atomic long last_conn_id = -1;   /* set on the server thread */

/* ── the server's owner ── */

static bool on_http(ws_conn_t *c, const ws_http_request_t *req, ws_http_response_t *resp, void *u)
{
    (void) c; (void) u;
    if (strncmp(req->path, "/api/", 5) != 0)
        return false;                       /* static pages */
    char hdr[64] = "";
    ws_http_header(req, "X-Test", hdr, sizeof(hdr));
    char *body = malloc(512);
    int n = snprintf(body, 512, "{\"method\":\"%s\",\"path\":\"%s\",\"query\":\"%s\",\"x_test\":\"%s\","
                     "\"body\":\"%.*s\",\"peer\":\"%s\"}", req->method, req->path, req->query, hdr,
                     (int) req->body_len, (const char *) req->body, req->peer);
    resp->status = !strcmp(req->method, "POST") ? 201 : 200;
    resp->content_type = "application/json";
    resp->extra_headers = "X-Request-Id: t1\r\n";
    resp->body = body;
    resp->body_len = (size_t) n;
    return true;
}

static int on_upgrade(ws_conn_t *c, const ws_http_request_t *req, unsigned *cls,
                      char *protocol, size_t protocol_len, void *u)
{
    (void) u;
    if (strcmp(req->path, "/api/v2/ws") != 0)
        return 0;                           /* legacy, class 0 */
    char want[128] = "";
    if (!ws_http_header(req, "Sec-WebSocket-Protocol", want, sizeof(want)) || !strstr(want, "hermes.v1"))
        return 426;
    snprintf(protocol, protocol_len, "hermes.v1");
    *cls = 1;
    last_conn_id = ws_conn_id(c);
    return 0;
}

static const void *assets(const char *path, size_t *size)
{
    if (strcmp(path, "/index.html") != 0)
        return NULL;
    *size = 5;
    return "hello";
}

/* ── a minimal client ── */

static int dial(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, connect(fd, (struct sockaddr *) &a, sizeof(a)), "connect");
    return fd;
}

/* Everything the server sends until it closes or goes quiet for 1 s. */
static size_t read_all(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    struct pollfd p = { .fd = fd, .events = POLLIN };
    while (n + 1 < cap && poll(&p, 1, 1000) > 0)
    {
        ssize_t r = recv(fd, buf + n, cap - 1 - n, 0);
        if (r <= 0)
            break;
        n += (size_t) r;
    }
    buf[n] = '\0';
    return n;
}

static char reply[8192];

static const char *request(const char *raw)
{
    int fd = dial();
    TEST_ASSERT_EQUAL_INT((int) strlen(raw), (int) send(fd, raw, strlen(raw), 0));
    read_all(fd, reply, sizeof(reply));
    close(fd);
    return reply;
}

#define UPGRADE(path, proto) \
    "GET " path " HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" \
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" proto "\r\n"

/* Upgrades and returns the socket, after the 101 (and whatever followed). */
static int ws_open(const char *raw)
{
    int fd = dial();
    send(fd, raw, strlen(raw), 0);
    char buf[1024] = "";
    size_t n = 0;
    struct pollfd p = { .fd = fd, .events = POLLIN };
    while (!strstr(buf, "\r\n\r\n") && n + 1 < sizeof(buf) && poll(&p, 1, 1000) > 0)
    {
        ssize_t r = recv(fd, buf + n, sizeof(buf) - 1 - n, 0);
        if (r <= 0)
            break;
        n += (size_t) r;
        buf[n] = '\0';
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(buf, "101 Switching Protocols"), buf);
    snprintf(reply, sizeof(reply), "%s", buf);
    return fd;
}

/* The payload of the next server frame (unmasked, < 126 bytes), or "" on timeout. */
static const char *ws_next(int fd, int timeout_ms)
{
    static char payload[256];
    uint8_t h[2];
    struct pollfd p = { .fd = fd, .events = POLLIN };
    payload[0] = '\0';
    if (poll(&p, 1, timeout_ms) <= 0 || recv(fd, h, 2, MSG_WAITALL) != 2)
        return payload;
    size_t len = h[1] & 0x7f;
    if (len >= 126 || recv(fd, payload, len, MSG_WAITALL) != (ssize_t) len)
        return payload;
    payload[len] = '\0';
    return payload;
}

/* The next server frame's opcode and payload (< 126 bytes); -1 on timeout. */
static int ws_frame(int fd, int timeout_ms, char *payload, size_t cap, size_t *len)
{
    uint8_t h[2];
    struct pollfd p = { .fd = fd, .events = POLLIN };
    *len = 0;
    if (poll(&p, 1, timeout_ms) <= 0 || recv(fd, h, 2, MSG_WAITALL) != 2)
        return -1;
    size_t n = h[1] & 0x7f;
    if (n >= 126 || n > cap || recv(fd, payload, n, MSG_WAITALL) != (ssize_t) n)
        return -1;
    *len = n;
    return h[0] & 0x0f;
}

void setUp(void) {}
void tearDown(void) {}

/* ── tests ── */

static void test_rest_get_with_query_and_headers(void)
{
    request("GET /api/v2/radio?limit=5 HTTP/1.1\r\nHost: x\r\nX-Test: hi\r\n\r\n");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "HTTP/1.1 200 OK"), reply);
    TEST_ASSERT_NOT_NULL(strstr(reply, "Content-Type: application/json"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "X-Request-Id: t1"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "\"path\":\"/api/v2/radio\",\"query\":\"limit=5\",\"x_test\":\"hi\""), reply);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "\"peer\":\"127.0.0.1\""), reply);
}

static void test_rest_methods_and_bodies(void)
{
    request("POST /api/v2/x HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\n\r\n{\"a\":true}\n");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "HTTP/1.1 201 Created"), reply);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "\"body\":\"{\"a\":true}\n\""), reply);

    const char *methods[] = { "PUT", "PATCH", "DELETE" };
    for (int i = 0; i < 3; i++)
    {
        char raw[128];
        snprintf(raw, sizeof(raw), "%s /api/v2/x HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n{}", methods[i]);
        request(raw);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "HTTP/1.1 200 OK"), test_msg("%s: %s", methods[i], reply));
        char want[32];
        snprintf(want, sizeof(want), "\"method\":\"%s\"", methods[i]);
        TEST_ASSERT_NOT_NULL(strstr(reply, want));
    }
}

/* A body sent in two pieces is waited for, not cut. */
static void test_rest_body_in_pieces(void)
{
    int fd = dial();
    const char *head = "POST /api/v2/x HTTP/1.1\r\nHost: x\r\nContent-Length: 6\r\n\r\nabc";
    send(fd, head, strlen(head), 0);
    usleep(100000);
    send(fd, "def", 3, 0);
    read_all(fd, reply, sizeof(reply));
    close(fd);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "\"body\":\"abcdef\""), reply);
}

static void test_rest_refusals(void)
{
    request("POST /api/v2/x HTTP/1.1\r\nHost: x\r\nContent-Length: 999999\r\n\r\n");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "413"), reply);
    request("POST /api/v2/x HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "501"), reply);
    request("POST /index.html HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "405"), reply);
}

static void test_static_pages_and_head(void)
{
    request("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    TEST_ASSERT_NOT_NULL(strstr(reply, "200 OK"));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\r\n\r\nhello"));
    request("GET /nothing HTTP/1.1\r\nHost: x\r\n\r\n");
    TEST_ASSERT_NOT_NULL(strstr(reply, "404"));
    request("HEAD /api/v2/x HTTP/1.1\r\nHost: x\r\n\r\n");
    TEST_ASSERT_NOT_NULL(strstr(reply, "200 OK"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "\r\n\r\n"), reply);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", strstr(reply, "\r\n\r\n") + 4, "HEAD sent a body");
}

/* /api/v2/ws needs hermes.v1 (426 without), is answered with it, and is
 * class 1; a legacy upgrade is class 0.  Broadcasts reach their class only,
 * and send_to one connection only. */
static void test_upgrade_routing_and_classes(void)
{
    request(UPGRADE("/api/v2/ws", ""));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "426"), reply);

    int legacy = ws_open(UPGRADE("/", ""));
    TEST_ASSERT_NULL_MESSAGE(strstr(reply, "Sec-WebSocket-Protocol"), "legacy got a subprotocol");
    last_conn_id = -1;
    int v2 = ws_open(UPGRADE("/api/v2/ws", "Sec-WebSocket-Protocol: chat, hermes.v1\r\n"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "Sec-WebSocket-Protocol: hermes.v1\r\n"), reply);
    long v2_id = last_conn_id;
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(0, v2_id);

    TEST_ASSERT_EQUAL_INT(0, ws_server_broadcast(srv, "legacy", 6, true));
    TEST_ASSERT_EQUAL_STRING("legacy", ws_next(legacy, 1000));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", ws_next(v2, 300), "class 1 got a class 0 broadcast");

    TEST_ASSERT_EQUAL_INT(0, ws_server_broadcast_class(srv, 1, "v2", 2, true));
    TEST_ASSERT_EQUAL_STRING("v2", ws_next(v2, 1000));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", ws_next(legacy, 300), "class 0 got a class 1 broadcast");

    TEST_ASSERT_EQUAL_INT(0, ws_server_send_to(srv, v2_id, "just you", 8, true));
    TEST_ASSERT_EQUAL_STRING("just you", ws_next(v2, 1000));
    TEST_ASSERT_EQUAL_STRING("", ws_next(legacy, 300));

    close(legacy);
    close(v2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ws_server_send_to(srv, v2_id, "gone", 4, true),
                                  "a send to a closed connection is still accepted (and dropped)");
}

/* Another thread can close a connection: after what was queued before it,
 * with the code and reason it gave; later sends to it are dropped. */
static void test_close_conn_from_another_thread(void)
{
    last_conn_id = -1;
    int v2 = ws_open(UPGRADE("/api/v2/ws", "Sec-WebSocket-Protocol: hermes.v1\r\n"));
    long id = last_conn_id;
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(0, id);

    TEST_ASSERT_EQUAL_INT(-1, ws_server_close_conn(srv, id, 999, "bad code"));
    TEST_ASSERT_EQUAL_INT(0, ws_server_send_to(srv, id, "last", 4, true));
    TEST_ASSERT_EQUAL_INT(0, ws_server_close_conn(srv, id, 1008, "session ended"));
    TEST_ASSERT_EQUAL_INT(0, ws_server_send_to(srv, id, "after", 5, true));

    char pl[128];
    size_t n;
    TEST_ASSERT_EQUAL_INT(0x1, ws_frame(v2, 1000, pl, sizeof(pl), &n));
    TEST_ASSERT_EQUAL_MEMORY("last", pl, 4);
    TEST_ASSERT_EQUAL_INT(0x8, ws_frame(v2, 1000, pl, sizeof(pl), &n));
    TEST_ASSERT_EQUAL_size_t(2 + strlen("session ended"), n);
    TEST_ASSERT_EQUAL_INT(1008, ((uint8_t) pl[0] << 8) | (uint8_t) pl[1]);
    TEST_ASSERT_EQUAL_MEMORY("session ended", pl + 2, n - 2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, ws_frame(v2, 300, pl, sizeof(pl), &n),
                                  "a frame came after the close");
    close(v2);
}

/* ── HTTPS: the same server with TLS ── */

/* A throwaway self-signed certificate, through OpenSSL's API (no CLI). */
static void make_cert(const char *dir)
{
    snprintf(cert_path, sizeof(cert_path), "%s/cert.pem", dir);
    snprintf(key_path, sizeof(key_path), "%s/key.pem", dir);
    EVP_PKEY *pkey = EVP_EC_gen("P-256");
    TEST_REQUIRE(pkey);
    X509 *x = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, pkey);
    X509_NAME *name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *) "localhost", -1, -1, 0);
    X509_set_issuer_name(x, name);
    TEST_REQUIRE(X509_sign(x, pkey, EVP_sha256()) > 0);
    FILE *f = fopen(cert_path, "w");
    TEST_REQUIRE(f && PEM_write_X509(f, x));
    fclose(f);
    f = fopen(key_path, "w");
    TEST_REQUIRE(f && PEM_write_PrivateKey(f, pkey, NULL, NULL, 0, NULL, NULL));
    fclose(f);
    X509_free(x);
    EVP_PKEY_free(pkey);
}

/* Sends raw over TLS to the TLS server; returns what came back. */
static const char *tls_request(const char *raw)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    TEST_ASSERT_NOT_NULL(ctx);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);     /* self-signed, on loopback */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(tls_port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TEST_ASSERT_EQUAL_INT(0, connect(fd, (struct sockaddr *) &a, sizeof(a)));
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, SSL_connect(ssl), "TLS handshake");
    TEST_ASSERT_EQUAL_INT((int) strlen(raw), SSL_write(ssl, raw, (int) strlen(raw)));
    size_t n = 0;
    struct pollfd p = { .fd = fd, .events = POLLIN };
    while (n + 1 < sizeof(reply) && (SSL_pending(ssl) > 0 || poll(&p, 1, 1000) > 0))
    {
        int r = SSL_read(ssl, reply + n, (int) (sizeof(reply) - 1 - n));
        if (r <= 0)
            break;
        n += (size_t) r;
        if (strstr(reply, "101 Switching") && strstr(reply, "\r\n\r\n"))
            break;                      /* an upgrade stays open */
    }
    reply[n] = '\0';
    SSL_free(ssl);
    close(fd);
    SSL_CTX_free(ctx);
    return reply;
}

/* REST and the hermes.v1 upgrade work the same over HTTPS. */
static void test_https_rest_and_upgrade(void)
{
    tls_request("GET /api/v2/radio?x=1 HTTP/1.1\r\nHost: x\r\nX-Test: tls\r\n\r\n");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "HTTP/1.1 200 OK"), reply);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "\"query\":\"x=1\",\"x_test\":\"tls\""), reply);

    tls_request("POST /api/v2/x HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nbody");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "201 Created"), reply);
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"body\":\"body\""));

    tls_request(UPGRADE("/api/v2/ws", "Sec-WebSocket-Protocol: hermes.v1\r\n"));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(reply, "101 Switching Protocols"), reply);
    TEST_ASSERT_NOT_NULL(strstr(reply, "Sec-WebSocket-Protocol: hermes.v1"));
}

int main(void)
{
    port = (uint16_t) (20000 + getpid() % 20000);
    ws_server_opts_t o = {
        .port = port, .bind_addr = "127.0.0.1", .asset_lookup = assets,
        .max_http_body = 4096, .on_http = on_http, .on_upgrade = on_upgrade,
    };
    TEST_REQUIRE(ws_server_start(&srv, &o) == 0);

    char dir[] = "/tmp/test_ws_server.XXXXXX";
    TEST_REQUIRE(mkdtemp(dir));
    make_cert(dir);
    tls_port = (uint16_t) (port + 1);
    ws_server_opts_t t = o;
    t.port = tls_port;
    t.tls_enabled = true;
    t.tls_cert_path = cert_path;
    t.tls_key_path = key_path;
    TEST_REQUIRE(ws_server_start(&tls_srv, &t) == 0);

    UNITY_BEGIN();
    RUN_TEST(test_rest_get_with_query_and_headers);
    RUN_TEST(test_rest_methods_and_bodies);
    RUN_TEST(test_rest_body_in_pieces);
    RUN_TEST(test_rest_refusals);
    RUN_TEST(test_static_pages_and_head);
    RUN_TEST(test_upgrade_routing_and_classes);
    RUN_TEST(test_close_conn_from_another_thread);
    RUN_TEST(test_https_rest_and_upgrade);
    int failed = UNITY_END();
    ws_server_stop(srv);
    ws_server_stop(tls_srv);
    unlink(cert_path);
    unlink(key_path);
    rmdir(dir);
    return failed;
}
