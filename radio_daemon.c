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

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "radio.h"
#include "radio_backend.h"

_Atomic bool shutdown_ = false;

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s [-r radio.ini] [-u user.ini] [-c cpu_nr] [-h]\n\n"
            "Options:\n"
            "  -r radio.ini   Path to radio/hardware config  (default: %s)\n"
            "  -u user.ini    Path to user/profile config    (default: %s)\n"
            "  -c cpu_nr      Pin process to CPU cpu_nr      (default: no pinning)\n"
            "                 Use -1 to disable CPU pinning\n"
            "  -h             Show this help\n",
             prog, CFG_RADIO_PATH, CFG_USER_PATH);
}

int main(int argc, char *argv[])
{
    const char *cfg_radio_path = CFG_RADIO_PATH;
    const char *cfg_user_path  = CFG_USER_PATH;
    int cpu_nr = -1; /* -1 = no pinning */
    bool cpu_arg_provided = false;
    radio_backend_selection backend_selection;
    radio_daemon_runtime runtime;

    int opt;
    while ((opt = getopt(argc, argv, "hr:u:c:")) != -1)
    {
        switch (opt)
        {
        case 'r':
            cfg_radio_path = optarg;
            break;
        case 'u':
            cfg_user_path = optarg;
            break;
        case 'c':
            cpu_nr = atoi(optarg);
            cpu_arg_provided = true;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    if (!radio_backend_detect(cfg_radio_path, &backend_selection))
    {
        fprintf(stderr, "Failed to resolve radio backend. Exiting.\n");
        return EXIT_FAILURE;
    }

    runtime.cfg_radio_path = cfg_radio_path;
    runtime.cfg_user_path = cfg_user_path;
    runtime.cpu_arg_provided = cpu_arg_provided;
    runtime.cpu_nr = cpu_nr;

    return radio_backend_run(&backend_selection, &runtime);
}
