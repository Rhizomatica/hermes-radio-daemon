/* beacon_tx - the station's position beacons (docs/BEACON-FORMAT.md), sent
 * and received through bcast_link.
 *
 * Sending: with main:beacon_enable, one beacon every main:beacon_interval_s;
 * while an emergency code is set, one every main:beacon_emergency_interval_s
 * whether beacons are enabled or not.
 * The position is the GPS fix, else the station's fixed position
 * (main:station_latitude, main:station_longitude); with neither, nothing is
 * sent.  The callsign is main:callsign.  Over KISS a beacon is the compact
 * layout on DATAC16.
 *
 * Receiving: each valid beacon goes into the digital-message log (mode
 * BEACON) and becomes its callsign's last-known position, kept in
 * <main:state_dir>/beacon-stations.tsv across restarts.  Beacons also
 * arrive over UUCP or NNCP (an SOS sent to main:sos_recipients): the
 * dec_beacon handler drops each in BEACON_DROP_DIR, and the beacon thread
 * takes them from there as if heard on the air.
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

/* beacon_build_frame and beacon_build_now: no callsign, or no position to
 * send (neither a GPS fix nor a fixed position). Apart from the codec's
 * BEACON_E_* (beacon.h), which mean a value it cannot carry. */
#define BEACON_E_NO_SOURCE  (-16)

typedef struct {
    bool   enabled;                 /* main:beacon_enable: the regular beacons */
    int    interval_s;              /* main:beacon_interval_s */
    char   callsign[BEACON_CALL_MAX + 1];
    bool   has_fixed;               /* the station's fixed position */
    double fixed_lat, fixed_lon;
    int    emergency_interval_s;
    char   state_dir[512];          /* where beacon-stations.tsv is kept */
} beacon_config;

/* The beacon settings radiod has (core.ini). */
void beacon_config_of(const radio *radio_h, beacon_config *out);

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

/* The receiver, started by radiod whether or not the broadcast plane runs:
 * it loads the stations heard before and keeps their table saved, sets the
 * emergency code saved in core.ini, and takes the beacons dec_beacon drops
 * in BEACON_DROP_DIR (an SOS over UUCP or NNCP needs no broadcast plane). */
bool beacon_receiver_start(radio *radio_h);
void beacon_receiver_stop(void);

/* What sends a beacon frame on the air: the broadcast plane's link
 * (bcast_link_send_station_frame), set while the plane runs, NULL when not.
 * 0, or an errno (ENOTCONN when Mercury is not connected). */
typedef int (*beacon_sender)(const uint8_t *frame, size_t len);
void beacon_set_sender(beacon_sender send);

/* Start the sender (its thread), part of the broadcast plane. */
bool beacon_service_start(radio *radio_h, const beacon_config *cfg);
void beacon_service_stop(void);

/* Change the switch, the intervals, the callsign or the fixed position
 * (the API does). */
void beacon_reconfigure(const beacon_config *cfg);
bool beacon_service_running(void);

/* bcast_link's on_station_frame. */
void beacon_on_station_frame(const uint8_t *frame, size_t len, void *user);

/* Set the emergency code (1..30) and send at once; 0 clears it, sending one
 * beacon with code 31 (emergency over).  Saved as main:beacon_emergency,
 * also while the sender is not running (the SOS then goes over UUCP or NNCP
 * only).  EINVAL out of range. */
int  beacon_set_emergency(int code);
int  beacon_emergency(void);
/* Send one beacon now (on top of the schedule). */
void beacon_send_now(void);

/* The compact beacon this station would send now with code (0..31), for the
 * SOS that goes over UUCP or NNCP; returns its length, BEACON_E_NO_SOURCE
 * without a callsign or a position, or another BEACON_E_* for a position
 * the beacon cannot carry. */
int  beacon_build_now(int code, uint8_t *frame, size_t frame_len);

/* A beacon that came over a store-and-forward transport (via: "uucp",
 * "nncp", or NULL when not known), sent at sent_at (unix seconds; 0 when
 * not known, taken as now): logged, and taken as its station's position
 * as one heard on the air at sent_at, so one that arrives after a newer
 * report from its station doesn't replace it.  0, or a BEACON_E_*. */
int  beacon_on_queued_frame(const uint8_t *frame, size_t len, const char *via, double sent_at);

/* Take the beacons waiting in dir (BEACON_DROP_DIR), at most max, oldest
 * sent first: each is handled as beacon_on_queued_frame does and removed.
 * A file that cannot be read or removed is logged once and left alone.
 * Returns how many were taken.  The receiver's thread calls it. */
int  beacon_drop_scan(const char *dir, int max);

/* The stations heard, newest first; returns how many (up to max). */
int  beacon_stations(beacon_station *out, int max);

/* Exposed for tests. */
beacon_position beacon_pick_position(const radiod_api_gps_fix *gps, const beacon_config *cfg);
int  beacon_build_frame(const beacon_config *cfg, const beacon_position *pos, int emergency,
                        uint8_t seq, uint8_t *frame, size_t frame_len);
int  beacon_interval_s(bool enabled, int interval_s, int emergency, int emergency_interval_s);
void beacon_stations_reset(void);
bool beacon_stations_save(const char *path);
bool beacon_stations_load(const char *path);
void beacon_heard(const beacon *b, double at);

#endif /* BEACON_TX_H */
