/* modem_audio_test - the transport-independent modem audio path.
 *
 * What rtp_audio_test does not reach: RX sinks (fan-out, removal), one
 * modem holding PTT against another, keying from one thread while feeding
 * from another (as a linked-in modem does), and an immediate unkey. */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "radio.h"
#include "radio_backend.h"
#include "radio_media.h"
#include "modem_audio.h"
#include "unity.h"
#include "fff.h"

/* The daemon side modem_audio drives, faked: fff records each call and its
 * arguments; pushing also counts the samples. */
DEFINE_FFF_GLOBALS;
FAKE_VOID_FUNC(radio_backend_set_ptt, radio *, bool, ptt_source, long);
FAKE_VOID_FUNC(radio_backend_end_ptt, radio *, ptt_source, long);
FAKE_VOID_FUNC(radio_media_push_tx_audio, radio *, const int16_t *, size_t);

static size_t npushed;
static void count_pushed(radio *radio_h, const int16_t *samples, size_t nsamples)
{
    (void) radio_h; (void) samples;
    npushed += nsamples;
}

static size_t sink_a, sink_b;
static void count_a(const int16_t *pcm, size_t n, void *user) { (void) pcm; (void) user; sink_a += n; }
static void count_b(const int16_t *pcm, size_t n, void *user) { (void) pcm; (void) user; sink_b += n; }

static void *feeder(void *arg)
{
    int16_t pcm[MODEM_AUDIO_FRAME] = { 0 };
    for (int i = 0; i < 50; i++)
    {
        modem_audio_tx_feed(pcm, MODEM_AUDIO_FRAME);
        usleep(1000);
    }
    (void) arg;
    return NULL;
}

static radio r;
static int16_t blk[480];

void setUp(void)
{
    RESET_FAKE(radio_backend_set_ptt);
    RESET_FAKE(radio_backend_end_ptt);
    RESET_FAKE(radio_media_push_tx_audio);
    FFF_RESET_HISTORY();
    radio_media_push_tx_audio_fake.custom_fake = count_pushed;
    npushed = sink_a = sink_b = 0;
    modem_audio_init(&r);
}

void tearDown(void)
{
    modem_audio_shutdown();
    modem_audio_remove_rx_sink(count_a, NULL);
    modem_audio_remove_rx_sink(count_b, NULL);
}

static void test_rx_ignored_while_stopped(void)
{
    modem_audio_shutdown();
    modem_audio_add_rx_sink(count_a, NULL);
    modem_audio_push_rx(blk, 480, 48000);
    TEST_ASSERT_FALSE(modem_audio_running());
    TEST_ASSERT_EQUAL_size_t(0, sink_a);
}

/* RX: every sink gets the 8 kHz block; a removed one gets nothing. */
static void test_rx_sinks(void)
{
    modem_audio_add_rx_sink(count_a, NULL);
    modem_audio_add_rx_sink(count_b, NULL);
    modem_audio_push_rx(blk, 480, 48000);
    TEST_ASSERT_EQUAL_size_t(80, sink_a);
    TEST_ASSERT_EQUAL_size_t(80, sink_b);
    modem_audio_remove_rx_sink(count_a, NULL);
    modem_audio_push_rx(blk, 480, 48000);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(80, sink_a, "a removed sink still gets audio");
    TEST_ASSERT_EQUAL_size_t(160, sink_b);
}

/* One modem at a time. */
static void test_one_modem_at_a_time(void)
{
    TEST_ASSERT_TRUE_MESSAGE(modem_audio_tx_begin(-1), "first keyer refused");
    TEST_ASSERT_EQUAL_INT(1, radio_backend_set_ptt_fake.call_count);
    TEST_ASSERT_TRUE(radio_backend_set_ptt_fake.arg1_val == IN_TX);
    TEST_ASSERT_EQUAL_INT(PTT_SRC_MODEM, radio_backend_set_ptt_fake.arg2_val);
    TEST_ASSERT_EQUAL_INT64(-1, radio_backend_set_ptt_fake.arg3_val);
    TEST_ASSERT_TRUE_MESSAGE(modem_audio_tx_begin(-1), "re-key by the holder refused");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, radio_backend_set_ptt_fake.call_count, "a re-key switched PTT again");
    TEST_ASSERT_FALSE_MESSAGE(modem_audio_tx_begin(4242), "a second modem keyed over the first");
    TEST_ASSERT_TRUE(modem_audio_tx_keyed_by(-1));
    TEST_ASSERT_FALSE(modem_audio_tx_keyed_by(4242));
}

/* Key on one thread, feed from another: all of it reaches the radio. */
static void test_feed_from_another_thread(void)
{
    TEST_ASSERT_TRUE(modem_audio_tx_begin(-1));
    pthread_t t;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, feeder, NULL));
    pthread_join(t, NULL);
    TEST_ASSERT_EQUAL_size_t(50 * MODEM_AUDIO_FRAME * 6, npushed);
}

/* Immediate unkey (a linked modem's key(false)): no drain wait, and what
 * comes after it is dropped. */
static void test_immediate_unkey(void)
{
    TEST_ASSERT_TRUE(modem_audio_tx_begin(-1));
    modem_audio_tx_unkey("ended");
    TEST_ASSERT_EQUAL_INT(1, radio_backend_end_ptt_fake.call_count);
    TEST_ASSERT_EQUAL_INT64(-1, radio_backend_end_ptt_fake.arg2_val);
    TEST_ASSERT_FALSE(modem_audio_tx_active());
    modem_audio_tx_feed(blk, 160);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, npushed, "fed while unkeyed");
    TEST_ASSERT_TRUE_MESSAGE(modem_audio_tx_begin(4242), "another modem refused after the unkey");
}

/* Shutdown never leaves a modem keyed, and nothing keys after it. */
static void test_shutdown_unkeys(void)
{
    TEST_ASSERT_TRUE(modem_audio_tx_begin(4242));
    modem_audio_shutdown();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, radio_backend_end_ptt_fake.call_count, "shutdown left PTT on");
    TEST_ASSERT_EQUAL_INT(PTT_SRC_MODEM, radio_backend_end_ptt_fake.arg1_val);
    TEST_ASSERT_EQUAL_INT64(4242, radio_backend_end_ptt_fake.arg2_val);
    TEST_ASSERT_FALSE_MESSAGE(modem_audio_tx_begin(1), "keyed after shutdown");
}

int main(void)
{
    r.backend_kind = RADIO_BACKEND_HAMLIB;
    r.audio_sample_rate = 48000;
    pthread_mutex_init(&r.tx_audio_ring.mutex, NULL);

    UNITY_BEGIN();
    RUN_TEST(test_rx_ignored_while_stopped);
    RUN_TEST(test_rx_sinks);
    RUN_TEST(test_one_modem_at_a_time);
    RUN_TEST(test_feed_from_another_thread);
    RUN_TEST(test_immediate_unkey);
    RUN_TEST(test_shutdown_unkeys);
    return UNITY_END();
}
