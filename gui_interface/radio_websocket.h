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

#endif /* RADIO_WEBSOCKET_H_ */
