/* bcast_spool - the broadcast spool directories, as hermes-broadcast's
 * broadcast_daemon kept them, on top of bcast_link.
 *
 * tx (main:broadcast_tx_dir, /var/spool/hermes-broadcast/tx): the oldest
 * file there (hidden files aside) goes on the air.  A name ending in
 * "-<N>_frames" before its extension sends N frames and then removes the
 * file; without that the carousel runs until the file is removed.  A file
 * that is changed starts over; one the codec refuses is renamed to a hidden
 * ".<name>.failed".  Receivers file it under its name without the budget.
 *
 * rx (main:broadcast_rx_dir, /var/spool/hermes-broadcast/rx): each file the
 * carousel completes appears there whole, by a rename, under the name it
 * was sent with (made unique).  The codec decodes in the hidden
 * "<rx>/.incoming".
 *
 * This is what the installer's hermes-broadcast-send (an NNCP bundle with a
 * frame budget into tx) and hermes-broadcast-rx (rx into NNCP) use.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BCAST_SPOOL_H
#define BCAST_SPOOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* main:broadcast_tx_dir / broadcast_rx_dir defaults: hermes-broadcast's spool */
#define BCAST_SPOOL_DEFAULT_TX_DIR "/var/spool/hermes-broadcast/tx"
#define BCAST_SPOOL_DEFAULT_RX_DIR "/var/spool/hermes-broadcast/rx"

typedef struct {
    char tx_dir[512];
    char rx_dir[512];
} bcast_spool_config;

/* The spool's thread (tx); false when it could not start. */
bool bcast_spool_start(const bcast_spool_config *cfg);
void bcast_spool_stop(void);

/* bcast_link's on_object: move a completed file into rx. */
void bcast_spool_on_object(const char *path, const char *name, void *user);

/* The incoming directory for bcast_link: "<rx>/.incoming". */
void bcast_spool_incoming_dir(const bcast_spool_config *cfg, char *out, size_t cap);

/* Exposed for tests. */
int64_t bcast_spool_frames_limit(const char *path);           /* -1: none */
void    bcast_spool_send_name(const char *path, char *out, size_t cap);
bool    bcast_spool_pick(const char *dir, char *out, size_t cap, time_t *mtime);
bool    bcast_spool_rx_target(const char *rx_dir, const char *name, char *out, size_t cap);

#endif /* BCAST_SPOOL_H */
