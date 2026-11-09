/* gps_track - the station's track: its position fixes, recorded to files.
 *
 * With main:gps_logging, each fix the GPS reader gets (gps.c) may become a
 * point: at most one every gps_sample_interval_s, and none while the
 * station stays where it was, but for a heartbeat point every few minutes,
 * a new fix type, and the first fix after start or after a lost fix.  So a
 * fixed station writes a few kilobytes a day and a moving one its whole
 * path.
 *
 * A point is one JSON line (GpsPoint in api-rest.yaml, and the fix type),
 * in a file per gps_file_period_s, aligned to UTC: gps-YYYYMMDDTHHMMSSZ.jsonl,
 * named by its period's start, so a reader picks the files of a time range
 * by their names.  They are the API's /gps/files; past main:gps_log_max_mb
 * the oldest go.  The API's GET /gps/track reads them.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GPS_TRACK_H
#define GPS_TRACK_H

#include <stdbool.h>
#include <stddef.h>

#define GPS_TRACK_DIR_DEFAULT    "/var/spool/hermes-gps"
#define GPS_TRACK_MAX_MB_DEFAULT 64
#define GPS_STILL_M              10.0   /* closer than this to the last point: not moved (GPS wanders ~5 m) */
#define GPS_HEARTBEAT_S          300    /* a point at least this often, moved or not */
#define GPS_TRACK_PREFIX         "gps-"
#define GPS_TRACK_SUFFIX         ".jsonl"
#define GPS_TRACK_NAME_LEN       (sizeof GPS_TRACK_PREFIX - 1 + 16 + sizeof GPS_TRACK_SUFFIX - 1)
#define GPS_TRACK_DIR_MAX        256

typedef struct {
    bool logging;               /* main:gps_logging */
    int  sample_interval_s;     /* main:gps_sample_interval_s (>= 1) */
    int  file_period_s;         /* main:gps_file_period_s (>= 60) */
    long max_bytes;             /* main:gps_log_max_mb, in bytes */
    char dir[GPS_TRACK_DIR_MAX];/* main:gps_log_dir */
} gps_track_config;

/* The settings; applies to the next fix.  Also trims the files to the cap. */
void gps_track_configure(const gps_track_config *c);

/* One fix: fix 2 or 3 with a position (lat, lon; alt if has_alt; the
 * horizontal accuracy in metres, NAN if unknown) at `at` (unix seconds,
 * UTC); fix 0, the fix lost.  Records it if it is due. */
void gps_track_fix(int fix, double lat, double lon, double alt, bool has_alt, double accuracy_m,
                   double at);

/* The track's files, full paths, oldest first, one a line, into out.  False
 * if they do not fit or the directory cannot be read. */
bool gps_track_files(char *out, size_t out_len);

/* Deletes one of them, by name (gps-...jsonl, no path); 0, or -1 if that is
 * no track file's name or it cannot be removed. */
int gps_track_delete(const char *name);

#endif
