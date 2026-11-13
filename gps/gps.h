/* gps - the station's GPS: a reader thread over gpsd's JSON socket and the
 * logging settings, read from core.ini ([main]).
 *
 * radiod_api.h owns the wire types (radiod_api_gps*); this module fills
 * them.  The reader keeps the latest fix in one mutex-guarded struct, so
 * radiod_api_gps_get never blocks on gpsd.  With gps_enable off the fix is
 * "none" and only the settings are served.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPS_H
#define GPS_H

#include <stdbool.h>

#include "radio.h"
#include "radiod_api.h"

/* Load the settings from core.ini and, when main:gps_enable is set, start
 * the reader thread.  Called once, from radio_daemon_core_run, after the
 * configuration is loaded. */
void gps_init(radio *radio_h);

/* Stop the reader thread (a no-op if it never started). */
void gps_shutdown(void);

/* The latest fix and the settings, safe from any thread. */
void gps_latest(radiod_api_gps *out);

/* Now, unix seconds, on the track's clock: the clock its points are stamped
 * with. The system clock corrected by the GPS time of the last gpsd fix with
 * a time (the correction is kept after the fix is lost), the system clock
 * before the first. Trips (api/internal/gps/trip.go) take their bounds from
 * it, so a station whose system clock is off still finds its own points. */
double gps_clock(void);

/* Validate the settings against the spec's ranges and, if valid, apply and
 * save them to core.ini.  Returns a RADIOD_API_* code. */
int gps_settings_apply(const radiod_api_gps_settings *s);

#endif /* GPS_H */
