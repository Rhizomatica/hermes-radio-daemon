/* hermes-radio-daemon - embedded legacy sBitx bootstrap
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "radio.h"
#include "legacy_sbitx_bootstrap.h"

extern int legacy_sbitx_main(int argc, char *argv[]);
extern void legacy_sbitx_cfg_set_runtime_paths(const char *cfg_core,
                                               const char *cfg_user,
                                               const char *web_path);

static void derive_legacy_web_path(const char *cfg_radio_path,
                                   char *web_path,
                                   size_t web_path_len)
{
    const char *cfg_path = cfg_radio_path;
    const char *slash;

    if (!cfg_path || cfg_path[0] == '\0')
        cfg_path = CFG_RADIO_PATH;

    slash = strrchr(cfg_path, '/');
    if (!slash)
        snprintf(web_path, web_path_len, "%s", "web");
    else if (slash == cfg_path)
        snprintf(web_path, web_path_len, "%s", "/web");
    else
        snprintf(web_path, web_path_len, "%.*s/web", (int) (slash - cfg_path), cfg_path);
}

int legacy_sbitx_bootstrap(const char *cfg_radio_path,
                           const char *cfg_user_path,
                           const char *controller_path,
                           bool cpu_arg_provided,
                           int cpu_nr)
{
    const char *radio_path = cfg_radio_path ? cfg_radio_path : CFG_RADIO_PATH;
    const char *user_path = cfg_user_path ? cfg_user_path : CFG_USER_PATH;
    char cpu_arg[32];
    char web_path[CONFIG_PATH_MAX];
    char *legacy_argv[4];
    int legacy_argc = 0;

    derive_legacy_web_path(radio_path, web_path, sizeof(web_path));
    legacy_sbitx_cfg_set_runtime_paths(radio_path, user_path, web_path);

    fprintf(stderr,
            "radio_daemon: hfsignals backend bootstraps the vendored legacy_sbitx "
            "controller in-process using radio=%s, user=%s, web=%s.\n",
            radio_path,
            user_path,
            web_path);

    if (controller_path &&
        controller_path[0] != '\0' &&
        strcmp(controller_path, "sbitx_controller") != 0)
    {
        fprintf(stderr,
                "radio_daemon: main:hfsignals_controller_path=%s is ignored in "
                "embedded mode.\n",
                controller_path);
    }

    legacy_argv[legacy_argc++] = (char *) "legacy_sbitx_controller";

    if (cpu_arg_provided)
    {
        snprintf(cpu_arg, sizeof(cpu_arg), "%d", cpu_nr);
        legacy_argv[legacy_argc++] = (char *) "-c";
        legacy_argv[legacy_argc++] = cpu_arg;
    }

    legacy_argv[legacy_argc] = NULL;

    optind = 1;
    return legacy_sbitx_main(legacy_argc, legacy_argv);
}
