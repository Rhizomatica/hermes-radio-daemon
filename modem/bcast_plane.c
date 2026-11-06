/* bcast_plane - see bcast_plane.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "bcast_plane.h"

#include "bcast_link.h"
#include "bcast_spool.h"
#include "beacon_tx.h"
#include "mercury_link.h"
#include "radiod_log.h"

#define LOG_C "bcast"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static atomic_bool g_running;
static bool g_link, g_spool, g_beacon;

bool bcast_plane_start(radio *radio_h)
{
    bcast_spool_config sc = {0};
    snprintf(sc.tx_dir, sizeof sc.tx_dir, "%s", radio_h->broadcast_tx_dir);
    snprintf(sc.rx_dir, sizeof sc.rx_dir, "%s", radio_h->broadcast_rx_dir);

    bcast_link_config lc = {0};
    snprintf(lc.host, sizeof lc.host, "%s", radio_h->broadcast_host);
    lc.port = radio_h->broadcast_port;
    lc.mode = radio_h->broadcast_mode;
    bcast_spool_incoming_dir(&sc, lc.incoming_dir, sizeof lc.incoming_dir);
    /* Mercury inside radiod: direct calls, not its KISS port */
    lc.in_process = radio_h->enable_mercury ? mercury_link_bcast_host() : NULL;

    beacon_config bc = {0};
    bc.enabled = radio_h->beacon_enable;
    bc.interval_s = radio_h->beacon_interval_s;
    snprintf(bc.callsign, sizeof bc.callsign, "%.9s", radio_h->callsign);
    bc.has_fixed = radio_h->has_station_position;
    bc.fixed_lat = radio_h->station_latitude;
    bc.fixed_lon = radio_h->station_longitude;
    bc.emergency_interval_s = radio_h->beacon_emergency_interval_s;
    snprintf(bc.state_dir, sizeof bc.state_dir, "%s", radio_h->recording_dir);

    /* the receivers first, so nothing the link hears finds them missing */
    g_spool = bcast_spool_start(&sc);
    g_beacon = beacon_service_start(radio_h, &bc);
    bcast_link_ops ops = {
        .on_object = bcast_spool_on_object,
        .on_station_frame = beacon_on_station_frame,
    };
    g_link = bcast_link_start(&lc, &ops);
    if (!g_link)
    {
        HLOGE(LOG_C, "the broadcast plane could not start");
        bcast_plane_stop();
        return false;
    }
    atomic_store(&g_running, true);
    return true;
}

bool bcast_plane_running(void)
{
    return atomic_load(&g_running);
}

void bcast_plane_stop(void)
{
    atomic_store(&g_running, false);
    if (g_link)
        bcast_link_stop();
    if (g_beacon)
        beacon_service_stop();
    if (g_spool)
        bcast_spool_stop();
    g_link = g_spool = g_beacon = false;
}
