/* PTT ownership: radio_backend_claim_ptt() keys only when nobody else holds
 * PTT, radio_backend_unkey_ptt() unkeys only for the holder, and a holder's
 * release on disconnect still works (radio_backend.c). */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#include "unity.h"
#include "radio.h"
#include "radio_backend.h"

_Atomic bool shutdown_ = false;
_Atomic bool timer_reset = false;
_Atomic time_t timeout_counter = 0;

const radio_backend_ops hamlib_backend_ops = { .name = "hamlib" };
const radio_backend_ops sbitx_backend_ops = { .name = "hfsignals" };

int radio_daemon_core_run(const radio_backend_selection *selection,
                          const radio_daemon_runtime *runtime)
{
    (void) selection;
    (void) runtime;
    return 0;
}

#include "common/cfg_utils.c"
#include "radio/radio_backend.c"

static int keyings;

static void stub_set_txrx_state(radio *radio_h, bool state)
{
    radio_h->txrx_state = state;
    keyings++;
}

static const radio_backend_ops stub_ops = {
    .name = "stub",
    .set_txrx_state = stub_set_txrx_state,
};

static radio radio_h;

void setUp(void)
{
    memset(&radio_h, 0, sizeof(radio_h));
    radio_h.backend_ops = &stub_ops;
    radio_h.txrx_state = IN_RX;
    keyings = 0;
    /* no owner left over from the previous test */
    ptt_owner_src = PTT_SRC_NONE;
    ptt_owner_id = 0;
}
void tearDown(void) {}

static void test_claim_keys_once_and_refuses_others(void)
{
    TEST_ASSERT_EQUAL_INT(0, radio_backend_claim_ptt(&radio_h, PTT_SRC_WEBSOCKET, 7));
    TEST_ASSERT_EQUAL_INT(IN_TX, radio_h.txrx_state);
    TEST_ASSERT_EQUAL_INT(0, radio_backend_claim_ptt(&radio_h, PTT_SRC_WEBSOCKET, 7));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, keyings, "the holder claiming again keyed twice");
    TEST_ASSERT_EQUAL_INT(-1, radio_backend_claim_ptt(&radio_h, PTT_SRC_WEBSOCKET, 8));
    TEST_ASSERT_EQUAL_INT(-1, radio_backend_claim_ptt(&radio_h, PTT_SRC_RIGCTLD, 7));
    TEST_ASSERT_EQUAL_INT(IN_TX, radio_h.txrx_state);
}

static void test_unkey_only_by_the_holder(void)
{
    radio_backend_claim_ptt(&radio_h, PTT_SRC_WEBSOCKET, 7);
    TEST_ASSERT_FALSE(radio_backend_unkey_ptt(&radio_h, PTT_SRC_WEBSOCKET, 8));
    TEST_ASSERT_EQUAL_INT(IN_TX, radio_h.txrx_state);
    TEST_ASSERT_TRUE(radio_backend_unkey_ptt(&radio_h, PTT_SRC_WEBSOCKET, 7));
    TEST_ASSERT_EQUAL_INT(IN_RX, radio_h.txrx_state);
    TEST_ASSERT_FALSE(radio_backend_unkey_ptt(&radio_h, PTT_SRC_WEBSOCKET, 7));
    TEST_ASSERT_EQUAL_INT(0, radio_backend_claim_ptt(&radio_h, PTT_SRC_WEBSOCKET, 8));
}

static void test_keyed_by_someone_else_or_the_daemon(void)
{
    radio_backend_set_ptt(&radio_h, IN_TX, PTT_SRC_SHM, 1234);
    TEST_ASSERT_EQUAL_INT(-1, radio_backend_claim_ptt(&radio_h, PTT_SRC_WEBSOCKET, 7));
    radio_backend_set_ptt(&radio_h, IN_RX, PTT_SRC_SHM, 1234);

    /* keyed with no owner on record (a backend's own state) */
    radio_h.txrx_state = IN_TX;
    TEST_ASSERT_EQUAL_INT(-1, radio_backend_claim_ptt(&radio_h, PTT_SRC_WEBSOCKET, 7));
}

static void test_disconnect_releases_the_claim(void)
{
    radio_backend_claim_ptt(&radio_h, PTT_SRC_WEBSOCKET, 7);
    TEST_ASSERT_FALSE(radio_backend_release_ptt(&radio_h, PTT_SRC_WEBSOCKET, 9, "disconnected"));
    TEST_ASSERT_TRUE(radio_backend_release_ptt(&radio_h, PTT_SRC_WEBSOCKET, 7, "disconnected"));
    TEST_ASSERT_EQUAL_INT(IN_RX, radio_h.txrx_state);
}

/* Whose microphone may transmit (radio_webrtc): only the holder's, and only
 * while the radio is keyed. */
static void test_holds_only_while_keyed(void)
{
    TEST_ASSERT_FALSE(radio_backend_holds_ptt(&radio_h, PTT_SRC_WEBSOCKET, 7));
    radio_backend_claim_ptt(&radio_h, PTT_SRC_WEBSOCKET, 7);
    TEST_ASSERT_TRUE(radio_backend_holds_ptt(&radio_h, PTT_SRC_WEBSOCKET, 7));
    TEST_ASSERT_FALSE(radio_backend_holds_ptt(&radio_h, PTT_SRC_WEBSOCKET, 8));
    TEST_ASSERT_FALSE(radio_backend_holds_ptt(&radio_h, PTT_SRC_RIGCTLD, 7));
    /* the daemon unkeyed (an SWR trip): the owner is on record, not keyed */
    radio_h.txrx_state = IN_RX;
    TEST_ASSERT_FALSE(radio_backend_holds_ptt(&radio_h, PTT_SRC_WEBSOCKET, 7));
    radio_h.txrx_state = IN_TX;
    radio_backend_unkey_ptt(&radio_h, PTT_SRC_WEBSOCKET, 7);
    TEST_ASSERT_FALSE(radio_backend_holds_ptt(&radio_h, PTT_SRC_WEBSOCKET, 7));
    TEST_ASSERT_FALSE(radio_backend_holds_ptt(&radio_h, PTT_SRC_NONE, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_claim_keys_once_and_refuses_others);
    RUN_TEST(test_unkey_only_by_the_holder);
    RUN_TEST(test_keyed_by_someone_else_or_the_daemon);
    RUN_TEST(test_disconnect_releases_the_claim);
    RUN_TEST(test_holds_only_while_keyed);
    return UNITY_END();
}
