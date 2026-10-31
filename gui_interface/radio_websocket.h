/* hermes-radio-daemon - websocket control and media service
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 */

#ifndef RADIO_WEBSOCKET_H_
#define RADIO_WEBSOCKET_H_

#include <pthread.h>

#include "radio.h"

bool radio_websocket_init(radio *radio_h, pthread_t *websocket_tid);
void radio_websocket_shutdown(pthread_t *websocket_tid);

/* hermes.v1 spectrum topics, by the spectrum their binary frames carry. */
#define RADIO_WS_SPECTRUM_RX    0   /* radio.spectrum.rx, kind 3 */
#define RADIO_WS_SPECTRUM_TX    1   /* radio.spectrum.tx, kind 4 */
#define RADIO_WS_SPECTRUM_MODEM 2   /* modem.spectrum.rx, kind 3: mercury's */

/* Subscribe (on) or unsubscribe connection conn_id to a RADIO_WS_SPECTRUM_*:
 * this server then sends it the spec's binary frames of that spectrum.
 * Thread-safe. 0, or -1 when every slot is taken. A closed connection's
 * subscriptions are dropped. */
int radio_websocket_spectrum_subscribe(long conn_id, int spectrum, bool on);

/* Module traffic (mercury's UI) reaches this websocket through ui_hub.h,
 * of which this server is one transport. */

#endif /* RADIO_WEBSOCKET_H_ */
