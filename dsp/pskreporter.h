/* pskreporter - reports the stations this one decodes to PSKReporter
 * (https://pskreporter.info), as WSJT-X and JS8Call do.
 *
 * With main:pskreporter_enable, every FT8 sender decoded (digi_log's rx
 * entries with a callsign) is queued, once an hour per band at most, and
 * the queue goes to the server in one UDP datagram every five minutes or
 * so, never in step with the clock: the spec's rules.  The receiver is
 * main:callsign at the GPS fix, else at main:station_latitude/longitude,
 * as a Maidenhead locator; with no callsign or no position nothing is
 * sent.  The datagram is built by pskreporter_ipfix.c.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PSKREPORTER_H
#define PSKREPORTER_H

#include <stdbool.h>

#include "radio.h"

/* The spec's server; its test server is pskreporter.info:14739, whose
 * reports show at https://pskreporter.info/cgi-bin/psk-analysis.pl. */
#define PSKREPORTER_DEFAULT_SERVER  "report.pskreporter.info:4739"
#define PSKREPORTER_DEFAULT_PORT    "4739"

/* Starts reporting if main:pskreporter_enable is set; false if it could
 * not.  The station's callsign and position are read at each send, so the
 * API's changes to them apply. */
bool pskreporter_start(radio *radio_h);

/* Sends what is queued and stops. Safe when it never started. */
void pskreporter_shutdown(void);

#endif
