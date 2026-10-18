/* Minimal HTTP + WebSocket server for the radio daemon
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Based on Mercury's gui_interface/websocket/ws_server.h, MIT/BSD-free and
 * GPL-3-compatible, replacing Mongoose (GPL-2.0-only) as the transport.
 *
 * The scope is deliberately the traffic this daemon generates: one WebSocket
 * endpoint on any request that carries the handshake headers, static pages
 * out of a directory, a handful of concurrent clients on a LAN, optional
 * wss:// via OpenSSL.
 *
 * All sockets live on one thread, created by ws_server_start().  Every
 * callback below runs on that thread.  ws_server_broadcast() is the only
 * entry point other threads may call.
 */

#ifndef WS_SERVER_H_
#define WS_SERVER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ws_server ws_server_t;
typedef struct ws_conn ws_conn_t;

/* Look up a static asset by request path ("/index.html").  Return the bytes
 * and set *size, or NULL when there is no such asset. */
typedef const void *(*ws_asset_lookup_fn)(const char *path, size_t *size);

/* One HTTP request, every string NUL-terminated.  headers is the raw header
 * block ("Name: value\r\n" lines); body has body_len bytes (Content-Length;
 * chunked bodies are refused before this is called). */
typedef struct {
    const char *method;
    const char *path;             /* without the query */
    const char *query;            /* after '?', or "" */
    const char *headers;
    const void *body;
    size_t body_len;
    const char *peer;             /* the client's address, "127.0.0.1", "::1", ... */
} ws_http_request_t;

/* A reply: body is malloc'd by the handler and freed by the server;
 * extra_headers is "Name: value\r\n" lines or NULL. */
typedef struct {
    int status;
    const char *content_type;
    const char *extra_headers;
    void *body;
    size_t body_len;
} ws_http_response_t;

/* Find a header in a request's block (case-insensitive name): its value
 * copied into out, NUL-terminated.  False if absent. */
bool ws_http_header(const ws_http_request_t *req, const char *name, char *out, size_t out_len);

typedef struct {
    uint16_t port;
    const char *bind_addr;        /* host, IPv4 or IPv6 literal; NULL or "" = any */
    const char *ws_path;          /* informational; upgrade is header-driven */
    size_t max_message_size;      /* inbound cap; 0 means the built-in default */

    bool tls_enabled;
    const char *tls_cert_path;
    const char *tls_key_path;

    ws_asset_lookup_fn asset_lookup;   /* NULL disables static serving */
    size_t max_http_body;         /* request body cap; 0 means the built-in default */

    /* A non-upgrade request, ahead of the static pages: return true with
     * *resp filled to answer it, false to fall through to the assets. */
    bool (*on_http)(ws_conn_t *c, const ws_http_request_t *req,
                    ws_http_response_t *resp, void *user_data);

    /* A websocket upgrade: return 0 to accept it, setting *cls (the
     * connection's class, for broadcasts; 0 = the default) and, to answer
     * with a subprotocol, protocol; or an HTTP status (401, 403, 426, ...)
     * to refuse it.  NULL accepts every upgrade, class 0, no subprotocol. */
    int (*on_upgrade)(ws_conn_t *c, const ws_http_request_t *req, unsigned *cls,
                      char *protocol, size_t protocol_len, void *user_data);

    /* Callbacks, all on the server thread.  Any may be NULL. */
    void (*on_connect)(ws_conn_t *c, void *user_data);
    void (*on_message)(ws_conn_t *c, const void *data, size_t len,
                       bool is_text, void *user_data);
    void (*on_close)(ws_conn_t *c, void *user_data);
    void *user_data;
} ws_server_opts_t;

/* Start the listener and its thread.  Returns 0 on success,
 * WS_SERVER_ERR_TLS when wss:// was asked for and TLS could not be set up
 * (no TLS in this build, unreadable or bad cert/key), -1 on any other
 * failure.  On failure nothing is left running and *out is untouched. */
#define WS_SERVER_ERR_TLS (-2)
int ws_server_start(ws_server_t **out, const ws_server_opts_t *opts);

/* Stop the thread, drop every connection and free the server. */
void ws_server_stop(ws_server_t *s);

/* Queue a message for every connected WebSocket client of class 0.
 * Thread-safe; this and the two below are the only calls other threads may
 * make.  Copies `data`.  Returns 0 when queued, -1 on error. */
int ws_server_broadcast(ws_server_t *s, const void *data, size_t len,
                        bool is_text);

/* The same for the clients of one class (see on_upgrade). */
int ws_server_broadcast_class(ws_server_t *s, unsigned cls, const void *data,
                              size_t len, bool is_text);

/* Queue a message for one connection, by ws_conn_id(); dropped if it has
 * gone by the time the server thread gets to it. */
int ws_server_send_to(ws_server_t *s, long conn_id, const void *data,
                      size_t len, bool is_text);

/* Send on one connection.  Server thread only -- use it from a callback. */
int ws_conn_send(ws_conn_t *c, const void *data, size_t len, bool is_text);

/* Stable, small identifier for a connection, valid for its whole lifetime
 * (including the on_close callback).  Used to attribute PTT ownership. */
long ws_conn_id(ws_conn_t *c);

/* The class on_upgrade gave the connection (0 before or without it). */
unsigned ws_conn_class(ws_conn_t *c);

#endif /* WS_SERVER_H_ */
