/* mercury_link - mercury linked into the daemon, as a modem_audio transport.
 *
 * With main:enable_mercury, the daemon runs mercury in-process instead of as
 * a separate modem.service over RTP: mercury (built MERCURY_HOST=daemon, see
 * vendor/mercury/host/mercury_host.h) hears the radio through a modem_audio
 * RX sink, answers each block with its TX audio, keys through
 * PTT_SRC_MODEM, and reads the dial frequency from the active profile.
 * While it transmits on a radio whose capture stops (audio_half_duplex),
 * a 20 ms clock of its own drains its TX audio instead.  Its UI goes
 * through ui_hub.h to the daemon's UI transports (its websocket, later the
 * Go layer): status, lists, history and spectrum (binary type
 * UI_HUB_STREAM_MERCURY_SPECTRUM) to every client, and clients'
 * {"command": ...} messages to mercury.  Its
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
#include <stddef.h>
#include <stdint.h>

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

/* Mercury's last {"type":"status",...} JSON into out; false when it is not
 * running or has published none yet.  Any thread. */
bool mercury_link_last_status(char *out, size_t out_len);

/* ── the running engine: chat, the listen mode (mercury_host.h) ──
 *
 * Any thread.  Each returns MERCURY_LINK_DOWN, and calls nothing, while the
 * engine is not up (starting, stopped, or never started); otherwise
 * mercury_host.h's MERCURY_HOST_* result.  mercury_link_stop waits for calls
 * in flight. */
#define MERCURY_LINK_DOWN (-100)

/* The listen mode's name (e.g. "DATAC3") into out. */
int mercury_link_listen_mode(char *out, size_t out_len);

/* Set it by name: MERCURY_HOST_BAD for a mode it cannot switch to at run
 * time, MERCURY_HOST_BUSY while an ARQ session or a carousel holds the
 * modem (mercury_link_busy says which). */
int mercury_link_set_listen_mode(const char *name);

/* What holds the modem: "arq", "broadcast", or "" (free) into out. */
int mercury_link_busy(char *out, size_t out_len);

/* Chat lines after seq after (0: all held), mercury's JSONL, whole lines,
 * NUL-terminated; *len their bytes, *last the seq of the last one. */
int mercury_link_chat_since(uint64_t after, char *out, size_t out_len, size_t *len, uint64_t *last);

/* One chat line on plane "arq" or "bcast" (mercury_host_chat_send). */
int mercury_link_chat_send(const char *plane, const char *text);

/* One of mercury's UI commands ({"command": ...}, its docs/WEBSOCKET_API.md),
 * as a UI client's: 0 when carried out. */
int mercury_link_command(const char *json);

#endif /* MERCURY_LINK_H */
