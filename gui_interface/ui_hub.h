/* ui_hub - the daemon's UI traffic, independent of any one transport
 *
 * Modules publish to the hub and take commands from it; transports carry
 * its traffic to clients.  The C websocket (radio_websocket.c) is one
 * transport; the Go UI layer (websocket and/or REST, over CGo) is meant to
 * be another, calling the same functions.
 *
 *   module     mercury linked in (modem/mercury_link.c): its commands are
 *              the messages that carry its command_key ("command"); its
 *              binary frames are tagged with its stream type byte.
 *   transport  subscribes with ui_hub_add_sink() to receive every broadcast,
 *              passes each client message without radiod's "cmd" key to
 *              ui_hub_command(), and calls ui_hub_client_connected() for
 *              every new client.
 *
 * Wire format, the same for every transport:
 *   text      a module's JSON, as is (e.g. mercury's {"type":"status",...});
 *   binary    the module's stream type byte, then its frame (radiod's own
 *             streams are 1-3; mercury's spectrum is 4);
 *   replies   {"<key>":"<name>","status":"ok"}, or "status":"error" with
 *             "code"; "error":"no handler" when no module takes the key.
 *
 * Threads: modules broadcast from any thread; sinks are called with the
 * sink list read-locked, so they must only queue (no call back into the
 * hub).  Commands and client_connected run on the calling transport's
 * thread with the module list read-locked; unregistering a module, or
 * removing a sink, waits for the calls in flight.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_HUB_H
#define UI_HUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Binary stream types radiod hands out to modules (radiod's own are 1-3). */
#define UI_HUB_STREAM_MERCURY_SPECTRUM 0x04

/* ── modules ── */

typedef struct {
    const char *name;           /* listed to clients: "modules", "<name>_binary_type" */
    const char *command_key;    /* the JSON key that marks its commands */
    uint8_t     stream_type;    /* its binary frames' type byte; 0: none */
    int  (*command)(const char *json, size_t len, void *user);  /* 0: done */
    void (*client_connected)(void *user);
    void *user;
} ui_hub_module;

/* Copied; name and command_key must stay valid.  False if full or taken. */
bool ui_hub_register_module(const ui_hub_module *m);
void ui_hub_unregister_module(const char *name);

/* From a module, any thread: to every sink. */
void ui_hub_broadcast_text(const char *json);
void ui_hub_broadcast_stream(uint8_t stream_type, const void *data, size_t len);

/* ── transports ── */

typedef struct {
    void (*text)(const char *json, size_t len, void *user);
    void (*binary)(const uint8_t *frame, size_t len, void *user);  /* type byte first */
    void *user;
} ui_hub_sink;

/* Returns a handle for ui_hub_remove_sink(), or -1 if full. */
int  ui_hub_add_sink(const ui_hub_sink *sink);
void ui_hub_remove_sink(int handle);

/* A client message that is not radiod's own: if a module takes it (or it
 * carries mercury's "command" key with no module to take it), writes the
 * reply for that client into reply and returns true. */
bool ui_hub_command(const char *json, size_t len, char *reply, size_t reply_len);

/* A new client: each module sends its one-time state (as broadcasts). */
void ui_hub_client_connected(void);

/* The hello fragment describing the modules, for a transport's greeting:
 * "\"modules\":[\"mercury\"],\"mercury_binary_type\":4" (no braces). */
size_t ui_hub_hello_fields(char *out, size_t out_len);

#endif /* UI_HUB_H */
