/*
 * Hamlib backend PTT regression test.
 *
 * On the bench an IC-7100 stayed keyed for hours: the daemon's cached
 * txrx_state read RX while the rig transmitted, and every PTT-off was then
 * skipped as redundant, at run time and at shutdown. This drives the real
 * Hamlib backend (radio_hamlib.c) through Hamlib's NET rigctl client against
 * the daemon's own rigctld server, whose "radio" records every PTT change:
 *
 *   - start-up unkeys a rig left keyed by a daemon that died;
 *   - PTT-off reaches the rig even when the cache already reads RX;
 *   - a repeated PTT-on is still deduplicated (never re-keys);
 *   - shutdown unkeys whatever the cache says;
 *   - radio_daemon --ptt-off (force_ptt_off) unkeys with nothing running.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <hamlib/rig.h>

#include "../radio.h"
#include "../radio_backend.h"
#include "../hamlib/rig_server.h"

_Atomic bool shutdown_ = false;

const radio_backend_ops sbitx_backend_ops = { .name = "hfsignals" };

int radio_daemon_core_run(const radio_backend_selection *selection,
                          const radio_daemon_runtime *runtime)
{
    (void) selection; (void) runtime;
    return 0;
}

/* The digital-mode pump and the pipeline are not under test. */
bool hamlib_digi_start(radio *radio_h) { (void) radio_h; return true; }
void hamlib_digi_stop(radio *radio_h) { (void) radio_h; }
void radio_pipeline_refresh(radio *radio_h) { (void) radio_h; }

#include "../cfg_utils.c"
#include "../radio_backend.c"
#include "../radio_controls.c"
#include "../hamlib/radio_hamlib.c"

#define TEST_PORT 45331

/* The far end: what the transmitter actually does. */
static radio *remote;
static radio_backend_ops remote_ops;
static _Atomic int remote_ptt_calls = 0;

static void remote_set_txrx_state(radio *radio_h, bool txrx_state)
{
    remote_ptt_calls++;
    radio_h->txrx_state = txrx_state;
}

static void start_remote(void)
{
    remote = calloc(1, sizeof(*remote));
    assert(remote);

    /* Hamlib's dummy rig describes itself through the real backend's
     * dump_state, so the NET client opens as it would against the daemon;
     * PTT is recorded. Nothing else: the backend's CAT lock is one per
     * process, and here both ends share it. */
    remote_ops.name = "remote";
    remote_ops.dump_state = hamlib_backend_ops.dump_state;
    remote_ops.set_txrx_state = remote_set_txrx_state;
    remote->backend_kind = RADIO_BACKEND_HAMLIB;
    remote->backend_ops = &remote_ops;
    remote->profiles_count = 1;
    remote->profiles[0].freq = 7050000;
    remote->profiles[0].mode = MODE_USB;
    remote->rig_server_enable = true;
    remote->rig_server_port = TEST_PORT;

    RIG *dummy = rig_init(RIG_MODEL_DUMMY);
    assert(dummy && rig_open(dummy) == RIG_OK);
    remote->rig = dummy;

    assert(rig_server_start(remote));
}

static void init_local(radio *r)
{
    memset(r, 0, sizeof(*r));
    r->backend_kind = RADIO_BACKEND_HAMLIB;
    r->backend_ops = &hamlib_backend_ops;
    r->hamlib_model = RIG_MODEL_NETRIGCTL;
    snprintf(r->rig_pathname, sizeof(r->rig_pathname), "127.0.0.1:%d", TEST_PORT);
    r->ptt_type = PTT_RIG;
    r->profiles_count = 0;
}

static void test_init_unkeys_a_keyed_rig(void)
{
    radio local;
    init_local(&local);

    remote->txrx_state = IN_TX;            /* left keyed by a dead daemon */
    int calls = remote_ptt_calls;
    assert(radio_hamlib_init(&local));
    assert(remote_ptt_calls > calls && remote->txrx_state == IN_RX);
    assert(local.txrx_state == IN_RX);
    radio_hamlib_shutdown(&local);
    printf("  start-up unkeys a keyed rig .......... ok\n");
}

static void test_ptt_off_ignores_stale_cache(void)
{
    radio local;
    init_local(&local);
    assert(radio_hamlib_init(&local));

    tr_switch(&local, IN_TX);
    assert(remote->txrx_state == IN_TX && local.txrx_state == IN_TX);

    /* Keying twice must not send a second PTT-on. */
    int calls = remote_ptt_calls;
    tr_switch(&local, IN_TX);
    assert(remote_ptt_calls == calls);

    /* The cache goes stale: it reads RX while the rig transmits. */
    local.txrx_state = IN_RX;
    tr_switch(&local, IN_RX);
    assert(remote->txrx_state == IN_RX && "PTT-off skipped on a stale cache");

    radio_hamlib_shutdown(&local);
    printf("  PTT off ignores a stale RX cache ..... ok\n");
}

static void test_shutdown_unkeys_whatever_the_cache(void)
{
    radio local;
    init_local(&local);
    assert(radio_hamlib_init(&local));

    tr_switch(&local, IN_TX);
    assert(remote->txrx_state == IN_TX);
    /* Take PTT ownership off this connection, or the server's own release
     * on disconnect would unkey and hide what shutdown did. */
    radio_backend_set_ptt(remote, IN_TX, PTT_SRC_INTERNAL, 0);
    local.txrx_state = IN_RX;
    radio_hamlib_shutdown(&local);
    assert(remote->txrx_state == IN_RX && "shutdown left the rig keyed");
    printf("  shutdown unkeys whatever the cache ... ok\n");
}

static void test_force_ptt_off(void)
{
    radio local;
    init_local(&local);

    remote->txrx_state = IN_TX;
    assert(radio_hamlib_force_ptt_off(&local));
    assert(remote->txrx_state == IN_RX);
    printf("  --ptt-off unkeys with nothing running  ok\n");
}

int main(void)
{
    rig_set_debug(RIG_DEBUG_NONE);
    start_remote();

    printf("hamlib backend PTT:\n");
    test_init_unkeys_a_keyed_rig();
    test_ptt_off_ignores_stale_cache();
    test_shutdown_unkeys_whatever_the_cache();
    test_force_ptt_off();

    rig_server_stop();
    printf("hamlib_ptt_test: all ok\n");
    return 0;
}
