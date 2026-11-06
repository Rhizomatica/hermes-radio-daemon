/* beacon_tx - the station's position beacons (docs/BEACON-FORMAT.md), sent
 * and received through bcast_link.
 *
 * Sending: while position reports are on (main:gps_report_enabled), one
 * beacon every main:gps_report_interval_s; while an emergency code is set,
 * one every main:beacon_emergency_interval_s whether reports are on or not.
 * The position is the GPS fix, else the station's fixed position
 * (main:station_latitude, main:station_longitude); with neither, nothing is
 * sent.  The callsign is main:callsign.  Over KISS a beacon is the compact
 * layout on DATAC16.
 *
 * Receiving: each valid beacon goes into the digital-message log (mode
 * BEACON) and becomes its callsign's last-known position, kept in
 * <main:state_dir>/beacon-stations.tsv across restarts.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BEACON_TX_H
#define BEACON_TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "beacon.h"
#include "radio.h"
#include "radiod_api.h"

#define BEACON_STATIONS_MAX 256

/* main:beacon_emergency_interval_s: the default and the shortest allowed */
#define BEACON_EMERGENCY_INTERVAL_DEFAULT_S 120
#define BEACON_EMERGENCY_INTERVAL_MIN_S     30

#define BEACON_LAT_MAX_DEG 90.0
#define BEACON_LON_MAX_DEG 180.0

typedef struct {
    char   callsign[BEACON_CALL_MAX + 1];
    bool   has_fixed;               /* the station's fixed position */
    double fixed_lat, fixed_lon;
    int    emergency_interval_s;
    char   state_dir[512];
} beacon_config;

/* What a beacon is built from: a position, its source and the code. */
typedef struct {
    bool   has_position;
    double lat, lon;
    int    fix;                     /* BEACON_FIX_* */
    bool   has_altitude;
    double altitude_m;
    bool   has_time;
    double time;
} beacon_position;

typedef struct {
    char     callsign[BEACON_CALL_MAX + 1];
    double   lat, lon;
    int      emergency, fix;
    bool     has_altitude;
    int      altitude_m;
    double   heard_at;              /* unix seconds */
} beacon_station;

/* Start the sender (its thread) and load the stations heard before. */
bool beacon_service_start(radio *radio_h, const beacon_config *cfg);
void beacon_service_stop(void);

/* bcast_link's on_station_frame. */
void beacon_on_station_frame(const uint8_t *frame, size_t len, void *user);

/* Set the emergency code (1..30) and send at once; 0 clears it, sending one
 * beacon with code 31 (emergency over).  Saved as main:beacon_emergency.
 * EINVAL out of range. */
int  beacon_set_emergency(int code);
int  beacon_emergency(void);
/* Send one beacon now (on top of the schedule). */
void beacon_send_now(void);

/* The stations heard, newest first; returns how many (up to max). */
int  beacon_stations(beacon_station *out, int max);

/* Exposed for tests. */
beacon_position beacon_pick_position(const radiod_api_gps_fix *gps, const beacon_config *cfg);
int  beacon_build_frame(const beacon_config *cfg, const beacon_position *pos, int emergency,
                        uint8_t seq, uint8_t *frame, size_t frame_len);
int  beacon_interval_s(bool reports_on, int report_interval_s, int emergency, int emergency_interval_s);
void beacon_stations_reset(void);
bool beacon_stations_save(const char *path);
bool beacon_stations_load(const char *path);
void beacon_heard(const beacon *b, double at);

#endif /* BEACON_TX_H */
