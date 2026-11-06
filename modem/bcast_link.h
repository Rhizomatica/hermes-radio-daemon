/* bcast_link - radiod on Mercury's broadcast plane: files sent as a RaptorQ
 * carousel, files received, and position beacons both ways.
 *
 * radiod is Mercury's broadcast client, the role hermes-broadcast's
 * broadcast_daemon had.  With Mercury as its own process (modem.service) the
 * link is a TCP client on Mercury's KISS broadcast port (main:broadcast_host,
 * main:broadcast_port, 127.0.0.1:8100): frames go out as KISS
 * CMD_MODEM_FRAME, and the socket paces them, since Mercury stops reading
 * while its queue is full.  It reconnects whenever Mercury restarts.
 *
 * The carousel is Mercury's own codec (vendor/mercury/datalink_broadcast/
 * bcast_file.h, linked from libmercury_host.a), so nothing here is a second
 * implementation of the wire format.  One object is on the air at a time.
 *
 * Beacons (docs/BEACON-FORMAT.md) are frames of packet type 0x07.  Mercury
 * sends them on DATAC16 unless told otherwise, and its control port, where
 * that is told, belongs to uucpd, so over KISS a beacon is always the compact
 * layout on DATAC16.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BCAST_LINK_H
#define BCAST_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BCAST_LINK_BEACON_MODE 8   /* DATAC16, Mercury's mode index */

/* main:broadcast_* defaults */
#define BCAST_LINK_DEFAULT_HOST "127.0.0.1"
#define BCAST_LINK_DEFAULT_PORT 8100     /* Mercury's KISS broadcast port */
#define BCAST_LINK_DEFAULT_MODE 1        /* DATAC3, broadcast_daemon's -m 1 */

typedef struct {
    char host[64];
    int  port;
    int  mode;               /* Mercury's listen mode index: the carousel's frame size */
    char incoming_dir[512];  /* where the codec decodes; not the spool's rx dir */
} bcast_link_config;

typedef struct {
    /* A file the carousel completed: decoded at path (in incoming_dir), sent
     * under name.  The callee moves it away or deletes it. */
    void (*on_object)(const char *path, const char *name, void *user);
    /* A frame of packet type 0x07, as received (padded to its mode's size). */
    void (*on_station_frame)(const uint8_t *frame, size_t len, void *user);
    void *user;
} bcast_link_ops;

typedef enum {
    BCAST_TX_IDLE = 0,
    BCAST_TX_SENDING,
    BCAST_TX_DONE,       /* the cycles ran out, or the frame limit was reached */
    BCAST_TX_STOPPED,    /* bcast_link_stop_object */
    BCAST_TX_ERROR,
} bcast_tx_state;

typedef struct {
    bcast_tx_state state;
    char     name[256];
    size_t   bytes;
    int      pass, passes;          /* the pass on the air, from 1; passes 0: until stopped */
    int64_t  frames_per_pass;
    uint64_t frames_sent;
    int64_t  frames_limit;          /* -1: none */
    bool     connected;
} bcast_tx_stats;

/* Start the link (its threads).  false when it could not start; the daemon
 * runs on without a broadcast plane. */
bool bcast_link_start(const bcast_link_config *cfg, const bcast_link_ops *ops);
void bcast_link_stop(void);

/* Put one object on the air: name is what receivers file it under (a plain
 * name).  passes is how many times the whole object goes out (a pass is
 * every one of its RaptorQ symbols once; a receiver needs a little more than
 * one), 0 for until stopped; frames_limit (> 0) ends it after that many
 * frames, whichever comes first.  The bytes are copied.  0, EAGAIN before
 * the link has started, EBUSY while another is on the air, EINVAL (err says why) for a mode, name or size the
 * codec refuses. */
int  bcast_link_send_object(const char *name, const uint8_t *data, size_t len,
                            int passes, int64_t frames_limit, char *err, size_t errlen);

/* The mode's name as `mercury -l` reports it ("DATAC3"), "" out of range. */
const char *bcast_link_mode_name(int mode);

/* Frames one pass of an object of len bytes filed under name takes at the
 * mode, 0 for a mode that cannot carry files. */
int64_t bcast_link_frames_per_pass(int mode, const char *name, size_t len);
/* Stop the object on the air at the next frame (no-op when idle). */
void bcast_link_stop_object(void);
void bcast_link_stats(bcast_tx_stats *out);

/* Send one station frame (type 0x07, at most 14 bytes over KISS): 0,
 * ENOTCONN while Mercury is not connected, EINVAL for a frame of another
 * type or size. */
int  bcast_link_send_station_frame(const uint8_t *frame, size_t len);

/* KISS framing (exposed for tests).  encode returns the bytes written, 0 when
 * out is too small; the decoder hands each complete frame (command byte
 * first) to cb. */
size_t bcast_kiss_encode(uint8_t cmd, const uint8_t *in, size_t len, uint8_t *out, size_t cap);

typedef struct {
    uint8_t buf[2048];
    size_t  len;
    bool    in_frame, escaped, overflow;
} bcast_kiss_decoder;

void bcast_kiss_feed(bcast_kiss_decoder *d, const uint8_t *in, size_t len,
                     void (*cb)(const uint8_t *frame, size_t len, void *user), void *user);

#endif /* BCAST_LINK_H */
