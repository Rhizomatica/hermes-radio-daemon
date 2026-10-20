/* hermes-radio-daemon - main daemon
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 */

#include <getopt.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "radio.h"
#include "radio_backend.h"
#include "radiod_log.h"

_Atomic bool shutdown_ = false;

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s [-r radio.ini] [-u user.ini] [-c cpu_nr] [-P] [-h]\n\n"
            "Options:\n"
            "  -r radio.ini   Path to radio/hardware config  (default: %s)\n"
            "  -u user.ini    Path to user/profile config    (default: %s)\n"
            "  -c cpu_nr      Pin process to CPU cpu_nr      (default: no pinning)\n"
            "                 Use -1 to disable CPU pinning\n"
            "  -P, --ptt-off  Put the radio in receive and exit (run by\n"
            "                 systemd after the daemon stops or dies)\n"
            "  -h             Show this help\n",
             prog, CFG_RADIO_PATH, CFG_USER_PATH);
}

int main(int argc, char *argv[])
{
    const char *cfg_radio_path = CFG_RADIO_PATH;
    const char *cfg_user_path  = CFG_USER_PATH;
    int cpu_nr = -1; /* -1 = no pinning */
    radio_backend_selection backend_selection;
    radio_daemon_runtime runtime;

    bool ptt_off_only = false;
    static const struct option long_opts[] = {
        { "ptt-off", no_argument, NULL, 'P' },
        { "help",    no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "hr:u:c:P", long_opts, NULL)) != -1)
    {
        switch (opt)
        {
        case 'P':
            ptt_off_only = true;
            break;
        case 'r':
            cfg_radio_path = optarg;
            break;
        case 'u':
            cfg_user_path = optarg;
            break;
        case 'c':
            cpu_nr = atoi(optarg);
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    /* From here on everything logs through the logger, mercury's included;
     * core.ini's log settings are applied once it is read. */
    radiod_log_start();
    int rc;
    if (!radio_backend_detect(cfg_radio_path, &backend_selection))
    {
        HLOGE("radiod", "cannot tell the radio backend from %s; exiting", cfg_radio_path);
        rc = EXIT_FAILURE;
    }
    else if (ptt_off_only)
        rc = radio_backend_force_ptt_off(&backend_selection, cfg_radio_path) == 0
             ? EXIT_SUCCESS : EXIT_FAILURE;
    else
    {
        runtime.cfg_radio_path = cfg_radio_path;
        runtime.cfg_user_path = cfg_user_path;
        runtime.cpu_nr = cpu_nr;
        rc = radio_backend_run(&backend_selection, &runtime);
    }
    radiod_log_stop();
    return rc;
}
