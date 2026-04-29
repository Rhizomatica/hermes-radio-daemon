/* hermes-radio-daemon - embedded legacy sBitx bootstrap
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LEGACY_SBITX_BOOTSTRAP_H_
#define LEGACY_SBITX_BOOTSTRAP_H_

#include <stdbool.h>

int legacy_sbitx_bootstrap(const char *cfg_radio_path,
                           const char *cfg_user_path,
                           const char *controller_path,
                           bool cpu_arg_provided,
                           int cpu_nr);

#endif /* LEGACY_SBITX_BOOTSTRAP_H_ */
