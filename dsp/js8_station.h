/* js8_station - who this station is, for what it sends in JS8: main:callsign
 * and the 4-character locator of the GPS fix, else of the station's fixed
 * position (none if neither).
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef JS8_STATION_H
#define JS8_STATION_H

#include <stdbool.h>
#include <stddef.h>

#include "radio.h"

#define JS8_GRID_LEN 4

/* False if there is no callsign; grid is "" with no position. */
bool js8_station(radio *radio_h, char *call, size_t call_size, char grid[JS8_GRID_LEN + 1]);

/* Whether this text can go out in JS8 from this station: a callsign, and
 * the text packs into frames (dsp/js8_msg.h). */
bool js8_can_send(radio *radio_h, const char *text);

#endif
