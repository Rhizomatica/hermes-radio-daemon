/* hermes-radio-daemon - native CAT gateway
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CAT_SERVER_H_
#define CAT_SERVER_H_

#include <stdbool.h>

#include "radio.h"

/* CAT gateway mode, from core.ini "cat_server_mode".
 *
 *   PASSTHROUGH  relay the client's bytes to the rig's own CAT port
 *   EMULATE      answer a Kenwood TS-2000 dialect from the daemon's state
 *   AUTO         passthrough when the backend has a real CAT rig, else emulate
 */
typedef enum {
    CAT_SERVER_MODE_AUTO = 0,
    CAT_SERVER_MODE_PASSTHROUGH,
    CAT_SERVER_MODE_EMULATE,
} cat_server_mode;

bool cat_server_start(radio *radio_h);
void cat_server_stop(void);

/* Mode actually in effect once the rig is open ("passthrough" / "emulate"),
 * or "disabled". */
const char *cat_server_active_mode_name(void);

#endif /* CAT_SERVER_H_ */
