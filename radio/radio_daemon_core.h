/* hermes-radio-daemon - backend-neutral daemon core
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RADIO_DAEMON_CORE_H_
#define RADIO_DAEMON_CORE_H_

#include "radio_backend.h"

int radio_daemon_core_run(const radio_backend_selection *selection,
                          const radio_daemon_runtime *runtime);

#endif /* RADIO_DAEMON_CORE_H_ */
