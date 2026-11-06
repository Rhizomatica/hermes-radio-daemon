/* bcast_plane - starts radiod's broadcast plane: bcast_link (Mercury's
 * broadcast port), bcast_spool (the spool directories) and beacon_tx (the
 * position beacons), from the station's configuration.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BCAST_PLANE_H
#define BCAST_PLANE_H

#include <stdbool.h>

#include "radio.h"

/* With main:broadcast_enable, and Mercury as its own process: false when it
 * could not start (the daemon runs on without it). */
bool bcast_plane_start(radio *radio_h);
void bcast_plane_stop(void);
bool bcast_plane_running(void);

#endif /* BCAST_PLANE_H */
