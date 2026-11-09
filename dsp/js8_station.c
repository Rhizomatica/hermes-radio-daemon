/* js8_station - who this station is, for JS8.  See js8_station.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "js8_station.h"
#include "gps.h"
#include "pskreporter_ipfix.h"
#include "js8_tx.h"

#include <stdio.h>
#include <string.h>

bool js8_station(radio *radio_h, char *call, size_t call_size, char grid[JS8_GRID_LEN + 1])
{
    bool fixed;
    double lat, lon;
    pthread_mutex_lock(&radio_h->cfg_mutex);
    snprintf(call, call_size, "%s", radio_h->callsign);
    fixed = radio_h->has_station_position;
    lat = radio_h->station_latitude;
    lon = radio_h->station_longitude;
    pthread_mutex_unlock(&radio_h->cfg_mutex);

    radiod_api_gps g;
    gps_latest(&g);
    if (g.fix.has_position) {
        lat = g.fix.latitude;
        lon = g.fix.longitude;
        fixed = true;
    }
    char loc[PSKR_LOCATOR_MAX];
    grid[0] = '\0';
    if (fixed && pskr_locator(lat, lon, loc, sizeof loc))
        snprintf(grid, JS8_GRID_LEN + 1, "%.*s", JS8_GRID_LEN, loc);
    return call[0] != '\0';
}

bool js8_can_send(radio *radio_h, const char *text)
{
    char call[JS8_MSG_CALL_MAX], grid[JS8_GRID_LEN + 1];
    js8_tx_frame f[JS8_TX_MAX_FRAMES];
    return text && js8_station(radio_h, call, sizeof call, grid) &&
           js8_msg_pack(call, grid, text, js8_tx_submode(radio_h->js8_submodes) != JS8_SUB_NORMAL,
                        f, JS8_TX_MAX_FRAMES) > 0;
}
