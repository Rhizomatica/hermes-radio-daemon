/* pskreporter_ipfix - PSKReporter's reception report as IPFIX (RFC 5101),
 * after https://pskreporter.info/pskdev.html.  See pskreporter_ipfix.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pskreporter_ipfix.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define IPFIX_VERSION          10
#define IPFIX_HEADER_BYTES     16
#define IPFIX_SET_HEADER_BYTES 4
#define IPFIX_TEMPLATE_SET     2
#define IPFIX_OPTIONS_SET      3
#define IPFIX_VARLEN           0xFFFF  /* a field's length: variable */
#define IPFIX_ENTERPRISE_BIT   0x8000
#define IPFIX_ALIGN            4       /* sets are padded to this */

#define PSKR_ENTERPRISE        30351   /* PSKReporter's enterprise number */
/* Template ids: any above 255, ours alone for these field lists. */
#define PSKR_RX_TEMPLATE_ID    0x4852
#define PSKR_TX_TEMPLATE_ID    0x4853
#define RX_FIELDS              4       /* receiver: call, locator, software, antenna */
#define RX_SCOPE_FIELDS        1       /* of which the scope: the call */
#define TX_FIELDS              7       /* sender: call, frequency, SNR, mode, locator, source, time */

/* PSKReporter's fields (enterprise 30351), and IPFIX's flowStartSeconds. */
#define F_SENDER_CALL          1
#define F_RECEIVER_CALL        2
#define F_SENDER_LOCATOR       3
#define F_RECEIVER_LOCATOR     4
#define F_FREQUENCY            5
#define F_SNR                  6
#define F_SOFTWARE             8
#define F_ANTENNA              9
#define F_MODE                 10
#define F_INFO_SOURCE          11
#define IE_FLOW_START_SECONDS  150

#define FREQUENCY_BYTES        4
#define SNR_BYTES              1
#define INFO_SOURCE_BYTES      1
#define FLOW_START_BYTES       4
#define INFO_SOURCE_AUTOMATIC  1       /* "automatically extracted": the spec's only one mapped as a spot */

/* Maidenhead: 18 fields of 20 x 10 degrees, 10 squares of 2 x 1 in each,
 * 24 subsquares of 5 x 2.5 minutes in each. */
#define MH_FIELD_LON_DEG       20.0
#define MH_FIELD_LAT_DEG       10.0
#define MH_SQUARE_LON_DEG      2.0
#define MH_SQUARE_LAT_DEG      1.0
#define MH_SUBSQUARES          24
#define MH_FIELDS              18
#define MH_SQUARES             10
#define LAT_MAX_DEG            90.0
#define LON_MAX_DEG            180.0
#define HZ_PER_MHZ             1000000u

bool pskr_call_ok(const char *call)
{
    if (!call)
        return false;
    size_t len = strlen(call);
    if (len < PSKR_CALL_MIN_LEN || len >= PSKR_CALL_MAX)
        return false;
    bool letter = false, digit = false;
    for (const char *p = call; *p; p++) {
        if (isupper((unsigned char) *p))
            letter = true;
        else if (isdigit((unsigned char) *p))
            digit = true;
        else if (*p != '/')
            return false;
    }
    return letter && digit;
}

bool pskr_locator(double lat, double lon, char *out, size_t out_size)
{
    if (!out || out_size < PSKR_LOCATOR_LEN + 1)
        return false;
    out[0] = '\0';
    if (!isfinite(lat) || !isfinite(lon) || fabs(lat) > LAT_MAX_DEG || fabs(lon) > LON_MAX_DEG)
        return false;
    /* From the south-west corner; the north pole and the antimeridian fall
     * in the last subsquare, not past it. */
    double x = lon + LON_MAX_DEG, y = lat + LAT_MAX_DEG;
    int fx = (int) (x / MH_FIELD_LON_DEG), fy = (int) (y / MH_FIELD_LAT_DEG);
    if (fx >= MH_FIELDS) fx = MH_FIELDS - 1;
    if (fy >= MH_FIELDS) fy = MH_FIELDS - 1;
    x -= fx * MH_FIELD_LON_DEG;
    y -= fy * MH_FIELD_LAT_DEG;
    int sx = (int) (x / MH_SQUARE_LON_DEG), sy = (int) (y / MH_SQUARE_LAT_DEG);
    if (sx >= MH_SQUARES) sx = MH_SQUARES - 1;
    if (sy >= MH_SQUARES) sy = MH_SQUARES - 1;
    x -= sx * MH_SQUARE_LON_DEG;
    y -= sy * MH_SQUARE_LAT_DEG;
    int ux = (int) (x / MH_SQUARE_LON_DEG * MH_SUBSQUARES);
    int uy = (int) (y / MH_SQUARE_LAT_DEG * MH_SUBSQUARES);
    if (ux >= MH_SUBSQUARES) ux = MH_SUBSQUARES - 1;
    if (uy >= MH_SUBSQUARES) uy = MH_SUBSQUARES - 1;
    snprintf(out, out_size, "%c%c%c%c%c%c", 'A' + fx, 'A' + fy, '0' + sx, '0' + sy,
             'a' + ux, 'a' + uy);
    return true;
}

/* ── the datagram ─────────────────────────────────────────────────── */

typedef struct {
    uint8_t *p;
    size_t   n, cap;
    bool     full;
} buf;

static void put8(buf *b, uint8_t v)
{
    if (b->n + 1 > b->cap) { b->full = true; return; }
    b->p[b->n++] = v;
}
static void put16(buf *b, uint16_t v) { put8(b, v >> 8); put8(b, v & 0xff); }
static void put32(buf *b, uint32_t v) { put16(b, v >> 16); put16(b, v & 0xffff); }
static void set16(buf *b, size_t at, uint16_t v)
{
    b->p[at] = v >> 8;
    b->p[at + 1] = v & 0xff;
}

/* A string field: its length in one byte, then the bytes. */
static void put_str(buf *b, const char *s)
{
    size_t len = s ? strlen(s) : 0;
    if (len > PSKR_STRING_MAX)
        len = PSKR_STRING_MAX;
    put8(b, (uint8_t) len);
    for (size_t i = 0; i < len; i++)
        put8(b, (uint8_t) s[i]);
}

/* A PSKReporter field spec in a template: id, length, enterprise. */
static void put_field(buf *b, uint16_t id, uint16_t len)
{
    put16(b, IPFIX_ENTERPRISE_BIT | id);
    put16(b, len);
    put32(b, PSKR_ENTERPRISE);
}

static void pad(buf *b, size_t set_start)
{
    while ((b->n - set_start) % IPFIX_ALIGN)
        put8(b, 0);
}

static void put_templates(buf *b)
{
    /* The receiver's: an options template, receiverCallsign as its scope. */
    size_t s = b->n;
    put16(b, IPFIX_OPTIONS_SET);
    put16(b, 0);
    put16(b, PSKR_RX_TEMPLATE_ID);
    put16(b, RX_FIELDS);
    put16(b, RX_SCOPE_FIELDS);
    put_field(b, F_RECEIVER_CALL, IPFIX_VARLEN);
    put_field(b, F_RECEIVER_LOCATOR, IPFIX_VARLEN);
    put_field(b, F_SOFTWARE, IPFIX_VARLEN);
    put_field(b, F_ANTENNA, IPFIX_VARLEN);
    pad(b, s);
    if (!b->full)
        set16(b, s + 2, (uint16_t) (b->n - s));

    /* The senders'. */
    s = b->n;
    put16(b, IPFIX_TEMPLATE_SET);
    put16(b, 0);
    put16(b, PSKR_TX_TEMPLATE_ID);
    put16(b, TX_FIELDS);
    put_field(b, F_SENDER_CALL, IPFIX_VARLEN);
    put_field(b, F_FREQUENCY, FREQUENCY_BYTES);
    put_field(b, F_SNR, SNR_BYTES);
    put_field(b, F_MODE, IPFIX_VARLEN);
    put_field(b, F_SENDER_LOCATOR, IPFIX_VARLEN);
    put_field(b, F_INFO_SOURCE, INFO_SOURCE_BYTES);
    put16(b, IE_FLOW_START_SECONDS);    /* IPFIX's own: no enterprise */
    put16(b, FLOW_START_BYTES);
    pad(b, s);
    if (!b->full)
        set16(b, s + 2, (uint16_t) (b->n - s));
}

static void put_spot(buf *b, const pskr_spot *sp)
{
    put_str(b, sp->call);
    put32(b, sp->freq_hz);
    put8(b, (uint8_t) sp->snr_db);
    put_str(b, sp->mode);
    put_str(b, sp->locator);
    put8(b, INFO_SOURCE_AUTOMATIC);
    put32(b, sp->at);
}

size_t pskr_encode(uint8_t *out, size_t out_size, const pskr_receiver *rx,
                   const pskr_spot *spots, int n, bool templates,
                   uint32_t now, uint32_t sequence, uint32_t id, int *taken)
{
    buf b = { .p = out, .cap = out_size };
    if (taken)
        *taken = 0;
    if (!out || !rx)
        return 0;

    put16(&b, IPFIX_VERSION);
    put16(&b, 0);                       /* length, below */
    put32(&b, now);
    put32(&b, sequence);
    put32(&b, id);
    if (templates)
        put_templates(&b);

    size_t s = b.n;
    put16(&b, PSKR_RX_TEMPLATE_ID);
    put16(&b, 0);
    put_str(&b, rx->call);
    put_str(&b, rx->locator);
    put_str(&b, rx->software);
    put_str(&b, rx->antenna);
    pad(&b, s);
    if (b.full)
        return 0;
    set16(&b, s + 2, (uint16_t) (b.n - s));

    /* Spots while they fit, the set's padding included. */
    int k = 0;
    if (n > 0 && spots) {
        s = b.n;
        put16(&b, PSKR_TX_TEMPLATE_ID);
        put16(&b, 0);
        size_t kept = b.n;
        for (; k < n; k++) {
            put_spot(&b, &spots[k]);
            size_t padded = (b.n - s + IPFIX_ALIGN - 1) / IPFIX_ALIGN * IPFIX_ALIGN;
            if (b.full || s + padded > b.cap) {
                b.full = false;
                b.n = kept;
                break;
            }
            kept = b.n;
        }
        if (k == 0) {
            b.n = s;                    /* not one fits: no empty set */
        } else {
            pad(&b, s);
            set16(&b, s + 2, (uint16_t) (b.n - s));
        }
    }
    set16(&b, 2, (uint16_t) b.n);
    if (taken)
        *taken = k;
    return b.n;
}

/* ── once an hour ─────────────────────────────────────────────────── */

bool pskr_seen_due(pskr_seen *s, const char *call, uint32_t freq_hz, uint32_t now)
{
    uint32_t band = freq_hz / HZ_PER_MHZ;
    for (int i = 0; i < s->n; i++) {
        if (s->e[i].band != band || strcmp(s->e[i].call, call))
            continue;
        if (now - s->e[i].at < PSKR_REPEAT_S)
            return false;
        s->e[i].at = now;
        return true;
    }
    /* New: in a free place, else over the oldest-added. */
    int i = s->n < PSKR_SEEN_MAX ? s->n++ : s->next;
    if (s->n == PSKR_SEEN_MAX)
        s->next = (i + 1) % PSKR_SEEN_MAX;
    snprintf(s->e[i].call, sizeof s->e[i].call, "%s", call);
    s->e[i].band = band;
    s->e[i].at = now;
    return true;
}
