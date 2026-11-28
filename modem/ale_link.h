/* ale_link - ALE 2G in radiod (modem/ale_engine.h), on modem_audio.
 *
 * With ale:enable_ale the daemon runs ALE's receivers on the radio's RX
 * audio, the same 8 kHz feed Mercury and the RTP stream get: before noise
 * reduction and AGC in SSB and data modes.  A modem_audio RX sink copies each
 * block into a ring; a worker thread feeds the engine, drives it, and runs
 * the operator's commands, so the engine is only ever touched from it.
 *
 * Modes (ale:start at startup, ale_link_set_mode after):
 *  - monitor: the receivers only; each word decoded is logged and kept as an
 *    event.  ALE does not transmit, tune or answer.
 *  - listen, scan: PC-ALE's link layer runs as well, idle on the radio's
 *    channel or scanning ale:channel_file.  It answers calls (after
 *    ale_link_accept, unless ale:auto_respond), places them, and owns the
 *    radio: other clients' tuning and keying are refused (radio:owner) until
 *    the mode goes back to monitor, which tunes the radio back to the
 *    operator's frequency and mode.  A PTT off from anyone also does that.
 *    While a link is up and ale:handoff is voice, others may key on the
 *    channel (RADIO_OWNER_ALE_CHANNEL), in SSB or RADEv2 (ale:voice_mode).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ALE_LINK_H
#define ALE_LINK_H

#include <sched.h>
#include <stdbool.h>
#include <stdint.h>

#include "radio.h"

/* Results of the operator's actions. */
#define ALE_LINK_OK           0
#define ALE_LINK_EBUSY      (-1)   /* the state does not allow it now */
#define ALE_LINK_EINVAL     (-2)   /* bad address, AMD text or mode */
#define ALE_LINK_ENOTRUNNING (-3)  /* ALE is not running, or only monitors */
#define ALE_LINK_ENOCHANNELS (-4)  /* scan with no channel list */
#define ALE_LINK_ETIMEOUT   (-5)   /* the worker did not answer */

/* Start the receivers and, per ale:start, the link layer.  The worker runs
 * on `cpus` (NULL keeps the caller's), as Mercury's threads do.  False if it
 * could not start; the radio keeps running either way. */
bool ale_link_start(radio *radio_h, const cpu_set_t *cpus);

/* Stop everything (the link layer first, releasing the radio); before
 * modem_audio_shutdown. */
void ale_link_stop(void);

bool ale_link_running(void);

/* The operator's actions, run on the worker; ALE_LINK_* */
int ale_link_set_mode(uint8_t mode);                  /* ALE_START_*; monitor releases the radio */
int ale_link_call(const char *to, const char *amd);   /* amd may be NULL or empty */
int ale_link_accept(void);
int ale_link_reject(void);
int ale_link_terminate(void);
int ale_link_sound(void);

typedef struct
{
    uint8_t  mode;                  /* ALE_START_* */
    char     state[16];             /* OFF (monitor), IDLE, SCANNING, CALLING, HANDSHAKE, LINKED, SOUNDING */
    char     self[16];
    char     peer[16];
    uint32_t hz;
    uint8_t  sideband;              /* MODE_USB or MODE_LSB */
    bool     transmitting;
    uint32_t link_seconds;
    uint8_t  votes;                 /* the last word heard: unanimous votes, 0..48 */
    uint16_t channels;
    uint8_t  owner;                 /* RADIO_OWNER_* */
    uint64_t samples_heard;
    uint64_t samples_dropped;
} ale_link_status;

void ale_link_get_status(ale_link_status *out);

/* Events, oldest first, in a ring of ALE_LINK_EVENTS_MAX. */
#define ALE_LINK_EVENTS_MAX   256
#define ALE_LINK_EV_WORD      100   /* a word a receiver decoded (besides ale_engine.h's ALE_EV_*) */

typedef struct
{
    uint64_t seq;                   /* from 1, one up per event */
    int64_t  time_ms;               /* wall clock, ms since the epoch */
    uint8_t  kind;                  /* ALE_EV_* or ALE_LINK_EV_WORD */
    char     peer[16];              /* an address; a word's: its 3 characters */
    char     text[96];              /* AMD, state, why a link ended */
    uint32_t hz;
    uint8_t  combined;              /* a word's receiver: 0 stock, else K */
    uint8_t  preamble;              /* a word's: ALE_PREAMBLE_* */
    uint8_t  unanimous;             /* a word's: 0..48 */
} ale_link_event_rec;

/* Up to max events after seq `after`; returns how many; *last the newest seq. */
size_t ale_link_events_since(uint64_t after, ale_link_event_rec *out, size_t max, uint64_t *last);

#endif
