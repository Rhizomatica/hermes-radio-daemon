/* hermes-radio-daemon - rigctld-compatible TCP server for hfsignals backend
 *
 * Emulates a hamlib rig on localhost:4532 so external applications
 * (fldigi, WSJT-X, QLog, etc.) can control the sBitx/zBitx as if
 * it were a hamlib-compatible radio.
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RIG_SERVER_H_
#define RIG_SERVER_H_

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "radio.h"

bool rig_server_start(radio *radio_h);
void rig_server_stop(void);

#endif /* RIG_SERVER_H_ */
