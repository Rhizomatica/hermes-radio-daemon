/* gps_track - station tracks: position fixes, recorded to files and read
 * back by time.
 *
 * With main:gps_logging, each fix the GPS reader gets (gps.c) may become a
 * point of this station's track: at most one every gps_sample_interval_s,
 * and none while the station stays where it was, but for a heartbeat point
 * every few minutes, a new fix type, and the first fix after start or
 * after a lost fix.  So a fixed station writes a few kilobytes a day and a
 * moving one its whole path.  Every position beacon heard from another
 * station (beacon_tx.c) is a point of that station's track.
 *
 * Files.  A track is a directory of files, one per period aligned to UTC,
 * named by the period's start: gps-YYYYMMDDTHHMMSSZ.trk.  This station's
 * are in main:gps_log_dir itself, a gps_file_period_s each (the API's
 * /gps/files); another station's in <dir>/stations/<CALLSIGN>/, a day each
 * ('/' in a callsign is '_' there).  Past main:gps_log_max_mb, counted over
 * all of them, the oldest files go.
 *
 * A file is an 8-byte header ("HGPS", the format version, the record size,
 * two zero bytes) and the points, oldest first, at strictly increasing
 * times, in fixed records of GPS_TRACK_RECORD bytes, little-endian:
 *
 *    0  int64   at, unix milliseconds (UTC)
 *    8  int32   latitude, 1e-7 degrees
 *   12  int32   longitude, 1e-7 degrees
 *   16  int32   altitude, decimetres; GPS_TRACK_NO_ALT if unknown
 *   20  uint16  horizontal accuracy, decimetres; GPS_TRACK_NO_ACC if unknown
 *   22  uint8   fix: GPS_TRACK_FIX_*
 *   23  uint8   emergency code (beacons; 0 none)
 *
 * So the files of a time range are picked by their names, a time inside a
 * file is found by binary search, and a range's point count is two
 * searches; a point costs 24 bytes on disk and on the air.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GPS_TRACK_H
#define GPS_TRACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPS_TRACK_DIR_DEFAULT    "/var/spool/hermes-gps"
#define GPS_TRACK_MAX_MB_DEFAULT 64
#define GPS_STILL_M              10.0   /* closer than this to the last point: not moved (GPS wanders ~5 m) */
#define GPS_HEARTBEAT_S          300    /* a point at least this often, moved or not */
#define GPS_TRACK_PERIOD_MIN_S   60     /* GpsSettings.file_period_s's minimum */
#define GPS_TRACK_STATION_PERIOD_S 86400 /* another station's files: a day each, its beacons come minutes apart */
#define GPS_TRACK_PREFIX         "gps-"
#define GPS_TRACK_SUFFIX         ".trk"
#define GPS_TRACK_STAMP_LEN      16     /* YYYYMMDDTHHMMSSZ */
#define GPS_TRACK_NAME_LEN       (sizeof GPS_TRACK_PREFIX - 1 + GPS_TRACK_STAMP_LEN + sizeof GPS_TRACK_SUFFIX - 1)
#define GPS_TRACK_STATIONS       "stations"
#define GPS_TRACK_DIR_MAX        256
#define GPS_TRACK_CALL_MAX       15     /* a station's name: a callsign, SSID included */

#define GPS_TRACK_MAGIC          "HGPS"
#define GPS_TRACK_VERSION        1
#define GPS_TRACK_HEADER         8
#define GPS_TRACK_RECORD         24
#define GPS_TRACK_NO_ALT         INT32_MIN
#define GPS_TRACK_NO_ACC         UINT16_MAX

#define GPS_TRACK_FIX_NONE       0      /* the last known position */
#define GPS_TRACK_FIX_2D         2
#define GPS_TRACK_FIX_3D         3
#define GPS_TRACK_FIX_MANUAL     4      /* set by hand: a fixed station */

typedef struct {
    bool logging;               /* main:gps_logging */
    int  sample_interval_s;     /* main:gps_sample_interval_s (>= 1) */
    int  file_period_s;         /* main:gps_file_period_s (>= GPS_TRACK_PERIOD_MIN_S) */
    long max_bytes;             /* main:gps_log_max_mb, in bytes */
    char dir[GPS_TRACK_DIR_MAX];/* main:gps_log_dir */
} gps_track_config;

/* A point, as a record holds it. */
typedef struct {
    int64_t  at_ms;
    int32_t  lat_e7, lon_e7;
    int32_t  alt_dm;            /* GPS_TRACK_NO_ALT */
    uint16_t accuracy_dm;       /* GPS_TRACK_NO_ACC */
    uint8_t  fix;               /* GPS_TRACK_FIX_* */
    uint8_t  emergency;
} gps_track_point;

/* The settings; applies to the next fix.  Also trims the files to the cap. */
void gps_track_configure(const gps_track_config *c);

/* One fix of this station: fix 2 or 3 with a position (lat, lon; alt if
 * has_alt; the horizontal accuracy in metres, NAN if unknown) at `at` (unix
 * seconds, UTC); fix 0, the fix lost.  Records it if it is due. */
void gps_track_fix(int fix, double lat, double lon, double alt, bool has_alt, double accuracy_m,
                   double at);

/* A point of another station's track (a beacon heard from it), recorded as
 * it is.  0; -1 if not logging, the name is no callsign, the point is not
 * later than its track's last, or it cannot be written. */
int gps_track_add(const char *station, const gps_track_point *p);

/* The points of a track (station NULL or "": this station's) at times
 * since_ms..until_ms (both included), oldest first, at most max of them
 * into out.  How many; -1 for a name that is no callsign. */
int gps_track_read(const char *station, int64_t since_ms, int64_t until_ms,
                   gps_track_point *out, int max);

/* How many points a track has at times since_ms..until_ms; -1 as above. */
long gps_track_count(const char *station, int64_t since_ms, int64_t until_ms);

/* This station's track files, full paths, oldest first, one a line, into
 * out.  False if they do not fit or the directory cannot be read. */
bool gps_track_files(char *out, size_t out_len);

/* Deletes one of them, by name (gps-...trk, no path); 0, or -1 if that is
 * no track file's name or it cannot be removed. */
int gps_track_delete(const char *name);

#endif
