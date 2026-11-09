/* pskreporter_ipfix - what a PSKReporter reception report is made of: the
 * IPFIX datagram (https://pskreporter.info/pskdev.html), the receiver's
 * Maidenhead locator, which decoded callsigns may be reported, and the
 * once-an-hour rule.  No sockets and no threads, so it is tested on its
 * own; dsp/pskreporter.c sends what this builds.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PSKREPORTER_IPFIX_H
#define PSKREPORTER_IPFIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PSKR_CALL_MAX        16    /* a callsign, NUL included */
#define PSKR_CALL_MIN_LEN    3     /* shorter is no callsign */
#define PSKR_LOCATOR_LEN     6     /* "FN42hn" */
#define PSKR_LOCATOR_MAX     8     /* room for one, NUL included */
#define PSKR_MODE_MAX        8
#define PSKR_STRING_MAX      254   /* the spec's cap on one string field */
#define PSKR_DATAGRAM_MAX    1400  /* bytes: under a 1500-byte MTU with IP and UDP */
#define PSKR_REPEAT_S        3600  /* a callsign again on the same band: after an hour */
#define PSKR_SEEN_MAX        1024  /* callsigns remembered for that; the oldest go */

/* One station heard. */
typedef struct {
    char     call[PSKR_CALL_MAX];
    char     locator[PSKR_LOCATOR_MAX];   /* the sender's, "" if it gave none */
    char     mode[PSKR_MODE_MAX];         /* ADIF mode, e.g. "FT8" */
    uint32_t freq_hz;                     /* dial plus audio offset */
    int8_t   snr_db;
    uint32_t at;                          /* unix seconds it was decoded */
} pskr_spot;

/* This station. */
typedef struct {
    const char *call;
    const char *locator;
    const char *software;
    const char *antenna;                  /* "" for none */
} pskr_receiver;

/* The callsign a decoder's "from" field names, if it may be reported:
 * letters, digits and '/', with a letter and a digit, and not a hashed
 * one ("<...>", "<PJ2/K1ABC>", which the decoder only looked up). */
bool pskr_call_ok(const char *call);

/* The 6-character Maidenhead locator of a position, into out (> PSKR_LOCATOR_LEN bytes):
 * false, and "", if the position is not on the globe. */
bool pskr_locator(double lat, double lon, char *out, size_t out_size);

/* One datagram: the receiver record and spots[0..n), with the templates
 * first when `templates`.  Spots are taken while they fit in out_size;
 * *taken says how many.  sequence is the count of data records sent
 * before this datagram, id the sender's random identifier.  Returns the
 * datagram's length, or 0 if not even the receiver record fits. */
size_t pskr_encode(uint8_t *out, size_t out_size, const pskr_receiver *rx,
                   const pskr_spot *spots, int n, bool templates,
                   uint32_t now, uint32_t sequence, uint32_t id, int *taken);

/* The once-an-hour rule: a callsign on one band is reported again only
 * PSKR_REPEAT_S after it last was. */
typedef struct {
    struct {
        char     call[PSKR_CALL_MAX];
        uint32_t band;                    /* MHz: 14 for 14.074 */
        uint32_t at;
    } e[PSKR_SEEN_MAX];
    int n, next;
} pskr_seen;

/* Whether a spot of call at freq_hz, at time now, is due; when it is, it
 * is remembered as reported. */
bool pskr_seen_due(pskr_seen *s, const char *call, uint32_t freq_hz, uint32_t now);

#endif
