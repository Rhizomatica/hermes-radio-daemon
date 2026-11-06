/* bcast_link - radiod on Mercury's broadcast plane: files sent as a RaptorQ
 * carousel, files received, and position beacons both ways.
 *
 * radiod is Mercury's broadcast client, the role hermes-broadcast's
 * broadcast_daemon had, with two backends and the same features on both:
 *
 * - Mercury as its own process (modem.service): a TCP client on Mercury's
 *   KISS broadcast port (main:broadcast_host, main:broadcast_port,
 *   127.0.0.1:8100).  Frames go out as KISS CMD_MODEM_FRAME, and the socket
 *   paces them, since Mercury stops reading while its queue is full.  It
 *   reconnects whenever Mercury restarts.  The carousel is encoded here.
 * - Mercury inside radiod (main:enable_mercury): direct calls through
 *   bcast_link_host (mercury_link_bcast_host).  Mercury runs the carousel
 *   itself, at its listen mode, and hands up what it receives.
 *
 * The carousel is Mercury's own codec either way (vendor/mercury/
 * datalink_broadcast/bcast_file.h, linked from libmercury_host.a), so
 * nothing here is a second implementation of the wire format.  One object is
 * on the air at a time.
 *
 * Beacons (docs/BEACON-FORMAT.md) are frames of packet type 0x07, the
 * compact layout on DATAC16.  Over KISS no other mode is possible: Mercury
 * picks a beacon's mode on its control port, which belongs to uucpd.
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

/* What the in-process backend asks of Mercury.  Each call answers as below
 * while the engine is down (not started yet, or stopping). */
typedef struct {
    int  state;              /* BCAST_TX_*: the carousel's */
    uint64_t frames_sent;
} bcast_host_status;

typedef struct {
    /* Start a carousel at the listen mode, until stopped: 0, EBUSY (one on
     * the air, or an ARQ session), EINVAL (err says why), EAGAIN down. */
    int  (*send)(const char *name, const uint8_t *data, size_t len, char *err, size_t errlen);
    void (*stop)(void);
    /* false while down */
    bool (*status)(bcast_host_status *out);
    /* One station frame on Mercury's mode index mode: 0, EBUSY (Mercury's
     * queue is full), EINVAL, ENOTCONN down. */
    int  (*station_frame)(const uint8_t *frame, size_t len, int mode);
    /* The listen mode's index, -1 down. */
    int  (*listen_mode)(void);
    /* Hand what Mercury receives to these (NULL both: stop): 0, EINVAL when
     * the receiver could not start, ENOTCONN down.  They run on Mercury's
     * thread. */
    int  (*receive)(void (*on_object)(const uint8_t *data, size_t len, const char *name),
                    void (*on_frame)(const uint8_t *frame, size_t len));
} bcast_link_host;

typedef struct {
    char host[64];
    int  port;
    int  mode;               /* Mercury's listen mode index: the carousel's frame size */
    char incoming_dir[512];  /* where the codec decodes; not the spool's rx dir */
    /* Mercury inside radiod: its calls (host, port and mode unused, the mode
     * being Mercury's listen mode); NULL for the KISS port. */
    const bcast_link_host *in_process;
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

/* The mode the carousel goes out at: main:broadcast_mode over KISS,
 * Mercury's listen mode in process (main:broadcast_mode until it is known);
 * fallback before the link has started. */
int bcast_link_mode(int fallback);

/* Frames one pass of an object of len bytes filed under name takes at the
 * mode, 0 for a mode that cannot carry files. */
int64_t bcast_link_frames_per_pass(int mode, const char *name, size_t len);
/* Stop the object on the air at the next frame (no-op when idle). */
void bcast_link_stop_object(void);
void bcast_link_stats(bcast_tx_stats *out);

/* Send one station frame (type 0x07, at most 14 bytes) on DATAC16: 0,
 * ENOTCONN while Mercury is not connected, EBUSY when Mercury's queue is
 * full (in process), EINVAL for a frame of another type or size. */
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
