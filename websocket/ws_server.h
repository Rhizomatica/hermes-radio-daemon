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

typedef struct {
    uint16_t port;
    const char *bind_addr;        /* host, IPv4 or IPv6 literal; NULL or "" = any */
    const char *ws_path;          /* informational; upgrade is header-driven */
    size_t max_message_size;      /* inbound cap; 0 means the built-in default */

    bool tls_enabled;
    const char *tls_cert_path;
    const char *tls_key_path;

    ws_asset_lookup_fn asset_lookup;   /* NULL disables static serving */

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

/* Queue a message for every connected WebSocket client.  Thread-safe; this
 * is the only call other threads may make.  Copies `data`.  Returns 0 when
 * queued, -1 on error. */
int ws_server_broadcast(ws_server_t *s, const void *data, size_t len,
                        bool is_text);

/* Send on one connection.  Server thread only -- use it from a callback. */
int ws_conn_send(ws_conn_t *c, const void *data, size_t len, bool is_text);

/* Stable, small identifier for a connection, valid for its whole lifetime
 * (including the on_close callback).  Used to attribute PTT ownership. */
long ws_conn_id(ws_conn_t *c);

#endif /* WS_SERVER_H_ */
