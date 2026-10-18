/* mercury_link - mercury linked into the daemon, as a modem_audio transport.
 *
 * With main:enable_mercury, the daemon runs mercury in-process instead of as
 * a separate modem.service over RTP: mercury (built MERCURY_HOST=daemon, see
 * vendor/mercury/host/mercury_host.h) hears the radio through a modem_audio
 * RX sink, answers each block with its TX audio, keys through
 * PTT_SRC_MODEM, and reads the dial frequency from the active profile.
 * While it transmits on a radio whose capture stops (audio_half_duplex),
 * a 20 ms clock of its own drains its TX audio instead.  Its
 * TNC ports (JNOS, uucpd) and configuration are mercury's own:
 * main:mercury_config names the mercury.ini, main:mercury_args adds
 * command-line options.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MERCURY_LINK_H
#define MERCURY_LINK_H

#include <sched.h>
#include <stdbool.h>

#include "radio.h"

/* mercury's PTT id with modem_audio (an RTP modem's is its SSRC, >= 0). */
#define MERCURY_LINK_PTT_ID (-1L)

/* Start mercury.  Its threads run on `cpus` (the daemon's mask before -c
 * pinned it; NULL keeps the caller's), so the modem does not share the
 * daemon's DSP core.  Returns false if it could not be started; the radio
 * keeps running either way. */
bool mercury_link_start(radio *radio_h, const cpu_set_t *cpus);

/* Stop mercury, unkeying if it holds PTT.  Call before modem_audio_shutdown. */
void mercury_link_stop(void);

#endif /* MERCURY_LINK_H */
