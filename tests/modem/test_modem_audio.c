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

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                         printf(__VA_ARGS__); printf("\n"); } } while (0)

static int ptt_on, ptt_off;
static long ptt_owner = -999;

void radio_backend_set_ptt(radio *radio_h, bool txrx_state, ptt_source src, long id)
{
    (void) radio_h;
    if (txrx_state == IN_TX && src == PTT_SRC_MODEM)
    {
        ptt_on++;
        ptt_owner = id;
    }
}

void radio_backend_end_ptt(radio *radio_h, ptt_source src, long id)
{
    (void) radio_h;
    if (src == PTT_SRC_MODEM && id == ptt_owner)
    {
        ptt_off++;
        ptt_owner = -999;
    }
}

static size_t npushed;
void radio_media_push_tx_audio(radio *radio_h, const int16_t *samples, size_t nsamples)
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

int main(void)
{
    static radio r;
    int16_t blk[480] = { 0 };

    r.backend_kind = RADIO_BACKEND_HAMLIB;
    r.audio_sample_rate = 48000;
    pthread_mutex_init(&r.tx_audio_ring.mutex, NULL);

    modem_audio_push_rx(blk, 480, 48000);          /* before init: ignored */
    CHECK(!modem_audio_running(), "running before init");
    modem_audio_init(&r);

    /* RX: every sink gets the 8 kHz block; a removed one gets nothing. */
    modem_audio_add_rx_sink(count_a, NULL);
    modem_audio_add_rx_sink(count_b, NULL);
    modem_audio_push_rx(blk, 480, 48000);
    CHECK(sink_a == 80 && sink_b == 80, "sinks got %zu/%zu, want 80", sink_a, sink_b);
    modem_audio_remove_rx_sink(count_a, NULL);
    modem_audio_push_rx(blk, 480, 48000);
    CHECK(sink_a == 80 && sink_b == 160, "after removal %zu/%zu", sink_a, sink_b);

    /* One modem at a time. */
    CHECK(modem_audio_tx_begin(-1), "first keyer refused");
    CHECK(modem_audio_tx_begin(-1), "re-key by the holder refused");
    CHECK(ptt_on == 1, "re-key switched PTT again (%d)", ptt_on);
    CHECK(!modem_audio_tx_begin(4242), "a second modem keyed over the first");
    CHECK(modem_audio_tx_keyed_by(-1) && !modem_audio_tx_keyed_by(4242), "owner");

    /* Key on one thread, feed from another: all of it reaches the radio. */
    pthread_t t;
    pthread_create(&t, NULL, feeder, NULL);
    pthread_join(t, NULL);
    CHECK(npushed == 50 * MODEM_AUDIO_FRAME * 6, "pushed %zu", npushed);

    /* Immediate unkey (a linked modem's key(false)): no drain wait. */
    modem_audio_tx_unkey("ended");
    CHECK(ptt_off == 1 && !modem_audio_tx_active(), "unkey (%d)", ptt_off);
    modem_audio_tx_feed(blk, 160);                 /* unkeyed: dropped */
    CHECK(npushed == 50 * MODEM_AUDIO_FRAME * 6, "fed while unkeyed");

    /* The other modem may key now; shutdown never leaves it keyed. */
    CHECK(modem_audio_tx_begin(4242), "second modem refused after unkey");
    modem_audio_shutdown();
    CHECK(ptt_off == 2 && ptt_owner == -999, "shutdown left PTT on");
    CHECK(!modem_audio_tx_begin(1), "keyed after shutdown");

    printf("modem_audio_test: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
