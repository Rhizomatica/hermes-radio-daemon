/* radiod_api_attach - the daemon side of radiod_api.h: what the facade
 * reads and where hermes.v1 messages go.  Not for the Go layer.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RADIOD_API_ATTACH_H
#define RADIOD_API_ATTACH_H

/* The radio the facade reads and changes (radio *), set once at start. */
void radiod_api_attach(void *radio_h);

/* The websocket server hermes.v1 messages go through (ws_server_t *), or
 * NULL before it stops: sends after that are dropped. */
void radiod_api_attach_ws(void *server);

#endif /* RADIOD_API_ATTACH_H */
