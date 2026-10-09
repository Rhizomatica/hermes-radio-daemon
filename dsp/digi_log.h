/* digi_log - the structured log of digital-mode traffic (FT8, CW, RTTY,
 * D-STAR), sent and received.
 *
 * Every path that decodes or sends a digital-mode message calls
 * digi_log_add(). It appends one JSON object per line to
 * <dir>/digi.jsonl (rotated to digi.jsonl.1 at a size cap), keeps the last
 * entries in memory for live WebSocket pushes, and still writes the old
 * one-line-per-entry spool.log for one release so nothing reading it breaks.
 *
 * The JSON fields match DigiEntry in hermes-documentation
 * tech_docs/api-redesign/openapi.yaml. Text that came over the air is
 * attacker-controlled: it is reduced to printable ASCII and capped before
 * it is stored, and every line written is valid JSON.
 *
 * Thread-safe: decoders run on the DSP threads, readers on the WebSocket
 * thread.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DIGI_LOG_H
#define DIGI_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DIGI_LOG_TEXT_MAX   512   /* characters of text kept */
#define DIGI_LOG_CALL_MAX   16
#define DIGI_LOG_LINE_MAX   1536  /* one JSON object, worst case */
#define DIGI_LOG_RING       64    /* entries kept in memory for live pushes */

typedef struct {
    const char *mycall;
    const char *suffix;
    const char *urcall;
    const char *rpt1;
    const char *rpt2;
    bool        encrypted;
    bool        crc_ok;
} digi_log_dstar;

typedef struct {
    const char *mode;        /* "FT8", "CW", "RTTY", "DSTAR" */
    const char *dir;         /* "rx" or "tx" */
    uint64_t    freq_hz;     /* dial frequency */
    double      at;          /* unix time in seconds; 0 = now */
    bool        has_offset;  int   offset_hz;   /* audio offset */
    bool        has_snr;     float snr_db;      /* FT8, approximate */
    bool        has_dt;      float dt_s;        /* FT8, against the UTC slot */
    const char *text;        /* NULL or "" for a D-STAR header */
    const char *from;        /* callsign, NULL if none */
    const char *to;
    const char *grid;
    const digi_log_dstar *dstar;  /* mode DSTAR only */
} digi_log_entry;

/* Where the log lives (default /var/spool/hermes-digi) and when it rotates
 * (default 1 MiB). For tests; call before the first digi_log_add(). */
void digi_log_set_dir(const char *dir);
void digi_log_set_max_bytes(long max_bytes);

/* Record one entry: digi.jsonl, the in-memory ring and the legacy
 * spool.log. Never blocks for long; a write error drops the entry. */
void digi_log_add(const digi_log_entry *e);

/* The last `count` entries (1..1000), newest first, as a JSON array of
 * objects. Returns a malloc'd string the caller frees; "[]" when empty;
 * NULL only when out of memory. */
char *digi_log_last_json(int count);

/* Entries added after id `after`, oldest first, from the in-memory ring.
 * Copies up to `max` JSON objects into `lines` (each DIGI_LOG_LINE_MAX
 * bytes) and their ids into `ids`; returns how many. Pass the last id you
 * received; 0 at start gives every entry still in the ring. */
int digi_log_since(uint64_t after, char lines[][DIGI_LOG_LINE_MAX],
                   uint64_t *ids, int max);

/* The id of the newest entry, 0 if none since start. */
uint64_t digi_log_latest_id(void);

/* Exposed for tests. */
size_t digi_log_sanitize(const char *in, char *out, size_t out_size, size_t max_chars);
size_t digi_log_format(const digi_log_entry *e, uint64_t id, double at,
                       char *out, size_t out_size);

#endif /* DIGI_LOG_H */
