/* bcast_link's in-process backend (Mercury inside radiod) against a fake
 * Mercury call table: the receiver starts once the engine is up, received
 * files and beacons reach the link's callbacks, beacons go out on DATAC16,
 * and the carousel ends at its frame count, on a stop, and when Mercury
 * stops it (an ARQ session).
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "test_util.h"
#include "modem/bcast_link.h"

#define MODE        1      /* main:broadcast_mode: DATAC3 */
#define LISTEN_MODE 0      /* the fake engine's listen mode: DATAC1 */
#define WAIT_MS     3000   /* the longest a test waits for the link's thread */
#define POLL_US     10000
#define LINK_POLL_US (200 * 1000)  /* bcast_link.c's HOST_POLL_MS: one look at the engine */
#define LINK_POLLS  3      /* how many of them a test lets pass before it looks */

static char g_dir[256];

/* ── the fake engine ── */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool g_up;
static atomic_int  g_send_rc;
static bcast_host_status g_car;          /* under g_lock */
static char        g_sent_name[256];
static size_t      g_sent_len;
static atomic_int  g_sends, g_stops;
static uint8_t     g_beacon[32];
static size_t      g_beacon_len;
static atomic_int  g_beacon_mode, g_beacons;
static void (*g_rx_object)(const uint8_t *, size_t, const char *);
static void (*g_rx_frame)(const uint8_t *, size_t);
static atomic_int  g_receive_calls;

static int fake_send(const char *name, const uint8_t *data, size_t len, char *err, size_t errlen)
{
    (void) data;
    if (!atomic_load(&g_up))
        return EAGAIN;
    int rc = atomic_load(&g_send_rc);
    if (rc)
    {
        snprintf(err, errlen, "refused");
        return rc;
    }
    pthread_mutex_lock(&g_lock);
    snprintf(g_sent_name, sizeof g_sent_name, "%s", name);
    g_sent_len = len;
    g_car.state = BCAST_TX_SENDING;
    g_car.frames_sent = 0;
    pthread_mutex_unlock(&g_lock);
    atomic_fetch_add(&g_sends, 1);
    return 0;
}

static void fake_stop(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_car.state == BCAST_TX_SENDING)
        g_car.state = BCAST_TX_STOPPED;
    pthread_mutex_unlock(&g_lock);
    atomic_fetch_add(&g_stops, 1);
}

static bool fake_status(bcast_host_status *out)
{
    if (!atomic_load(&g_up))
        return false;
    pthread_mutex_lock(&g_lock);
    *out = g_car;
    pthread_mutex_unlock(&g_lock);
    return true;
}

static int fake_station_frame(const uint8_t *frame, size_t len, int mode)
{
    if (!atomic_load(&g_up))
        return ENOTCONN;
    pthread_mutex_lock(&g_lock);
    memcpy(g_beacon, frame, len);
    g_beacon_len = len;
    pthread_mutex_unlock(&g_lock);
    atomic_store(&g_beacon_mode, mode);
    atomic_fetch_add(&g_beacons, 1);
    return 0;
}

static int fake_listen_mode(void)
{
    return atomic_load(&g_up) ? LISTEN_MODE : -1;
}

static int fake_receive(void (*on_object)(const uint8_t *, size_t, const char *),
                        void (*on_frame)(const uint8_t *, size_t))
{
    atomic_fetch_add(&g_receive_calls, 1);
    if (!atomic_load(&g_up))
        return ENOTCONN;
    pthread_mutex_lock(&g_lock);
    g_rx_object = on_object;
    g_rx_frame = on_frame;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static const bcast_link_host g_fake = {
    .send = fake_send, .stop = fake_stop, .status = fake_status,
    .station_frame = fake_station_frame, .listen_mode = fake_listen_mode,
    .receive = fake_receive,
};

/* The carousel moves on n frames. */
static void advance(uint64_t n)
{
    pthread_mutex_lock(&g_lock);
    if (g_car.state == BCAST_TX_SENDING)
        g_car.frames_sent += n;
    pthread_mutex_unlock(&g_lock);
}

/* ── what the link hands up ── */

static char       g_obj_path[600], g_obj_name[256];
static atomic_int g_objs, g_stations;
static uint8_t    g_station[32];
static size_t     g_station_len;

static void on_object(const char *path, const char *name, void *u)
{
    (void) u;
    pthread_mutex_lock(&g_lock);
    snprintf(g_obj_path, sizeof g_obj_path, "%s", path);
    snprintf(g_obj_name, sizeof g_obj_name, "%s", name);
    pthread_mutex_unlock(&g_lock);
    atomic_fetch_add(&g_objs, 1);
}

static void on_station(const uint8_t *f, size_t len, void *u)
{
    (void) u;
    pthread_mutex_lock(&g_lock);
    g_station_len = len < sizeof g_station ? len : sizeof g_station;
    memcpy(g_station, f, g_station_len);
    pthread_mutex_unlock(&g_lock);
    atomic_fetch_add(&g_stations, 1);
}

static bool wait_state(bcast_tx_state want)
{
    for (int i = 0; i < WAIT_MS * 1000 / POLL_US; i++)
    {
        bcast_tx_stats st;
        bcast_link_stats(&st);
        if (st.state == want)
            return true;
        usleep(POLL_US);
    }
    return false;
}

static bool wait_connected(void)
{
    for (int i = 0; i < WAIT_MS * 1000 / POLL_US; i++)
    {
        bcast_tx_stats st;
        bcast_link_stats(&st);
        if (st.connected)
            return true;
        usleep(POLL_US);
    }
    return false;
}

static void start_link(void)
{
    bcast_link_config c = { .mode = MODE, .in_process = &g_fake };
    snprintf(c.incoming_dir, sizeof c.incoming_dir, "%s/in", g_dir);
    bcast_link_ops ops = { .on_object = on_object, .on_station_frame = on_station };
    TEST_ASSERT_TRUE(bcast_link_start(&c, &ops));
}

void setUp(void)
{
    atomic_store(&g_up, true);
    atomic_store(&g_send_rc, 0);
    memset(&g_car, 0, sizeof g_car);
    atomic_store(&g_sends, 0);
    atomic_store(&g_stops, 0);
    atomic_store(&g_beacons, 0);
    atomic_store(&g_objs, 0);
    atomic_store(&g_stations, 0);
    atomic_store(&g_receive_calls, 0);
    g_rx_object = NULL;
    g_rx_frame = NULL;
}

void tearDown(void)
{
    bcast_link_stop();
}

/* ── tests ── */

/* The engine comes up after the link: the receiver starts then, and the
 * link calls itself connected only from then. */
static void test_waits_for_the_engine(void)
{
    atomic_store(&g_up, false);
    start_link();
    usleep(LINK_POLLS * LINK_POLL_US);
    bcast_tx_stats st;
    bcast_link_stats(&st);
    TEST_ASSERT_FALSE(st.connected);
    TEST_ASSERT_TRUE(atomic_load(&g_receive_calls) > 1);   /* it keeps asking */
    uint8_t f[14] = { 0xE0 };
    TEST_ASSERT_EQUAL_INT(ENOTCONN, bcast_link_send_station_frame(f, sizeof f));
    TEST_ASSERT_EQUAL_INT(MODE, bcast_link_mode(-1));       /* main:broadcast_mode until known */

    atomic_store(&g_up, true);
    TEST_ASSERT_TRUE(wait_connected());
    TEST_ASSERT_NOT_NULL(g_rx_object);
    TEST_ASSERT_NOT_NULL(g_rx_frame);
    TEST_ASSERT_EQUAL_INT(LISTEN_MODE, bcast_link_mode(-1));
}

static void test_beacon_out_on_datac16(void)
{
    start_link();
    TEST_ASSERT_TRUE(wait_connected());
    uint8_t f[14] = { 0xE0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
    TEST_ASSERT_EQUAL_INT(0, bcast_link_send_station_frame(f, sizeof f));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_beacons));
    TEST_ASSERT_EQUAL_INT(BCAST_LINK_BEACON_MODE, atomic_load(&g_beacon_mode));
    TEST_ASSERT_EQUAL_size_t(sizeof f, g_beacon_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(f, g_beacon, sizeof f);
    uint8_t chat[14] = { 0x20 };                            /* not a station frame */
    TEST_ASSERT_EQUAL_INT(EINVAL, bcast_link_send_station_frame(chat, sizeof chat));
}

/* Station frames go to on_station_frame, other frames nowhere; a file goes
 * into incoming_dir, byte for byte, under its name. */
static void test_receives(void)
{
    start_link();
    TEST_ASSERT_TRUE(wait_connected());
    uint8_t beacon[14] = { 0xE0, 9 }, chat[16] = { 0x20, 1 };
    g_rx_frame(chat, sizeof chat);
    g_rx_frame(beacon, sizeof beacon);
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_stations));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(beacon, g_station, sizeof beacon);

    uint8_t data[300];
    for (size_t i = 0; i < sizeof data; i++)
        data[i] = (uint8_t) (i * 7);
    g_rx_object(data, sizeof data, "news.txt");
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_objs));
    TEST_ASSERT_EQUAL_STRING("news.txt", g_obj_name);
    FILE *fp = fopen(g_obj_path, "rb");
    TEST_ASSERT_NOT_NULL(fp);
    uint8_t back[sizeof data + 1];
    size_t n = fread(back, 1, sizeof back, fp);
    fclose(fp);
    TEST_ASSERT_EQUAL_size_t(sizeof data, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, back, sizeof data);
    TEST_ASSERT_TRUE(strncmp(g_obj_path, g_dir, strlen(g_dir)) == 0);
    unlink(g_obj_path);
}

/* Mercury's carousel runs until stopped: the link stops it at the frame
 * count and calls it done. */
static void test_frame_limit(void)
{
    start_link();
    TEST_ASSERT_TRUE(wait_connected());
    uint8_t data[100] = { 1 };
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(0, bcast_link_send_object("a.bin", data, sizeof data, 0, 5, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("a.bin", g_sent_name);
    TEST_ASSERT_EQUAL_INT(EBUSY, bcast_link_send_object("b.bin", data, sizeof data, 0, 5, err, sizeof err));

    advance(3);
    usleep(LINK_POLLS * LINK_POLL_US);
    bcast_tx_stats st;
    bcast_link_stats(&st);
    TEST_ASSERT_EQUAL_INT(BCAST_TX_SENDING, st.state);
    TEST_ASSERT_EQUAL_UINT64(3, st.frames_sent);
    TEST_ASSERT_EQUAL_INT64(5, st.frames_limit);
    TEST_ASSERT_EQUAL_INT64(bcast_link_frames_per_pass(LISTEN_MODE, "a.bin", sizeof data), st.frames_per_pass);

    advance(2);
    TEST_ASSERT_TRUE(wait_state(BCAST_TX_DONE));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_stops));
    bcast_link_stats(&st);
    TEST_ASSERT_EQUAL_UINT64(5, st.frames_sent);

    /* the next one may go */
    TEST_ASSERT_EQUAL_INT(0, bcast_link_send_object("b.bin", data, sizeof data, 0, 0, err, sizeof err));
}

static void test_stop_and_arq(void)
{
    start_link();
    TEST_ASSERT_TRUE(wait_connected());
    uint8_t data[100] = { 1 };
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(0, bcast_link_send_object("a.bin", data, sizeof data, 0, 0, err, sizeof err));
    advance(1);
    bcast_link_stop_object();
    TEST_ASSERT_TRUE(wait_state(BCAST_TX_STOPPED));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_stops));

    /* An ARQ session stops Mercury's carousel: stopped, not done. */
    TEST_ASSERT_EQUAL_INT(0, bcast_link_send_object("b.bin", data, sizeof data, 0, 0, err, sizeof err));
    TEST_ASSERT_TRUE(wait_state(BCAST_TX_SENDING));
    pthread_mutex_lock(&g_lock);
    g_car.state = BCAST_TX_STOPPED;
    pthread_mutex_unlock(&g_lock);
    TEST_ASSERT_TRUE(wait_state(BCAST_TX_STOPPED));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_stops));        /* nothing more to stop */

    /* Mercury refusing (an ARQ session up, a mode without files) is the caller's to see. */
    atomic_store(&g_send_rc, EBUSY);
    TEST_ASSERT_EQUAL_INT(EBUSY, bcast_link_send_object("c.bin", data, sizeof data, 0, 0, err, sizeof err));
    atomic_store(&g_send_rc, EINVAL);
    TEST_ASSERT_EQUAL_INT(EINVAL, bcast_link_send_object("c.bin", data, sizeof data, 0, 0, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("refused", err);
}

/* Stopping the link stops what is on the air and the receiver. */
static void test_stop_link(void)
{
    start_link();
    TEST_ASSERT_TRUE(wait_connected());
    uint8_t data[100] = { 1 };
    char err[64] = "";
    TEST_ASSERT_EQUAL_INT(0, bcast_link_send_object("a.bin", data, sizeof data, 0, 0, err, sizeof err));
    bcast_link_stop();
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&g_stops));
    TEST_ASSERT_NULL(g_rx_object);
    TEST_ASSERT_NULL(g_rx_frame);
}

int main(void)
{
    snprintf(g_dir, sizeof g_dir, "%s/bcast-host-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    TEST_REQUIRE(mkdtemp(g_dir));

    UNITY_BEGIN();
    RUN_TEST(test_waits_for_the_engine);
    RUN_TEST(test_beacon_out_on_datac16);
    RUN_TEST(test_receives);
    RUN_TEST(test_frame_limit);
    RUN_TEST(test_stop_and_arq);
    RUN_TEST(test_stop_link);
    int rc = UNITY_END();

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd)) {}
    return rc;
}
