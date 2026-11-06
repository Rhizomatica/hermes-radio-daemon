/* bcast_link: KISS framing, then the real link against a fake Mercury on a
 * loopback TCP port -- beacons out and in, a carousel both ways through
 * Mercury's codec (byte-identical file and name), the frame limit, and a
 * reconnect after Mercury goes away.
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "unity.h"
#include "test_util.h"
#include "modem/bcast_link.h"
#include "modem/beacon.h"
#include "bcast_file.h"

#define MODE 1   /* DATAC3, 126-byte frames */

static char g_dir[256];
static int  g_listen = -1, g_port;

/* ── what the link hands up ── */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_station[64];
static size_t  g_station_len;
static atomic_int g_station_n;
static char    g_obj_path[600], g_obj_name[256];
static atomic_int g_obj_n;

static void on_station(const uint8_t *f, size_t len, void *u)
{
    (void) u;
    pthread_mutex_lock(&g_lock);
    g_station_len = len < sizeof g_station ? len : sizeof g_station;
    memcpy(g_station, f, g_station_len);
    pthread_mutex_unlock(&g_lock);
    atomic_fetch_add(&g_station_n, 1);
}

static void on_object(const char *path, const char *name, void *u)
{
    (void) u;
    pthread_mutex_lock(&g_lock);
    snprintf(g_obj_path, sizeof g_obj_path, "%s", path);
    snprintf(g_obj_name, sizeof g_obj_name, "%s", name);
    pthread_mutex_unlock(&g_lock);
    atomic_fetch_add(&g_obj_n, 1);
}

/* ── the fake Mercury ── */
static int listen_any(void)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    bind(fd, (struct sockaddr *) &a, sizeof a);
    socklen_t al = sizeof a;
    getsockname(fd, (struct sockaddr *) &a, &al);
    g_port = ntohs(a.sin_port);
    listen(fd, 4);
    return fd;
}

static int accept_within(int ms)
{
    struct pollfd p = { .fd = g_listen, .events = POLLIN };
    if (poll(&p, 1, ms) <= 0)
        return -1;
    return accept(g_listen, NULL, NULL);
}

typedef struct {
    uint8_t frames[400][BCAST_FILE_MAX_FRAME];
    size_t  lens[400];
    int     n;
} frame_log;

static void collect(const uint8_t *f, size_t len, void *u)
{
    frame_log *fl = u;
    TEST_ASSERT_EQUAL_HEX8(0x03, f[0]);            /* CMD_MODEM_FRAME */
    if (fl->n < 400 && len - 1 <= BCAST_FILE_MAX_FRAME)
    {
        memcpy(fl->frames[fl->n], f + 1, len - 1);
        fl->lens[fl->n++] = len - 1;
    }
}

/* Read from the client for up to ms, or until want frames arrived. */
static void read_frames(int fd, frame_log *fl, int want, int ms)
{
    bcast_kiss_decoder d = {0};
    uint8_t buf[4096];
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (fl->n < want)
    {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        int left = ms - (int) ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0)
            break;
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, left) <= 0)
            break;
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r <= 0)
            break;
        bcast_kiss_feed(&d, buf, (size_t) r, collect, fl);
    }
}

static void send_kiss(int fd, const uint8_t *frame, size_t len)
{
    uint8_t wire[2 * BCAST_FILE_MAX_FRAME + 8];
    size_t n = bcast_kiss_encode(0x03, frame, len, wire, sizeof wire);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_INT((int) n, (int) send(fd, wire, n, MSG_NOSIGNAL));
}

static bool wait_for(atomic_int *v, int want, int ms)
{
    for (int i = 0; i < ms / 10; i++)
    {
        if (atomic_load(v) >= want)
            return true;
        usleep(10000);
    }
    return atomic_load(v) >= want;
}

static int start_link(void)
{
    bcast_link_config c = { .host = "127.0.0.1", .port = g_port, .mode = MODE };
    snprintf(c.incoming_dir, sizeof c.incoming_dir, "%s/in", g_dir);
    bcast_link_ops ops = { .on_object = on_object, .on_station_frame = on_station };
    TEST_ASSERT_TRUE(bcast_link_start(&c, &ops));
    int fd = accept_within(3000);
    TEST_ASSERT_TRUE_MESSAGE(fd >= 0, "the link did not connect");
    for (int i = 0; i < 100; i++)           /* until it calls itself connected */
    {
        bcast_tx_stats st;
        bcast_link_stats(&st);
        if (st.connected)
            break;
        usleep(10000);
    }
    return fd;
}

/* A test that closes its end before tearDown stops the link sees the link
 * reconnect at once, as it should: that connection waits in the listen
 * queue, and the next test must not take it for its own link's. */
static void flush_listen_queue(void)
{
    int fl = fcntl(g_listen, F_GETFL);
    fcntl(g_listen, F_SETFL, fl | O_NONBLOCK);
    int fd;
    while ((fd = accept(g_listen, NULL, NULL)) >= 0)
        close(fd);
    fcntl(g_listen, F_SETFL, fl);
}

void setUp(void)
{
    atomic_store(&g_station_n, 0);
    atomic_store(&g_obj_n, 0);
    flush_listen_queue();
}

void tearDown(void)
{
    bcast_link_stop();
}

/* ── tests ── */

static void kiss_cb(const uint8_t *f, size_t len, void *u)
{
    frame_log *fl = u;
    memcpy(fl->frames[fl->n], f, len);
    fl->lens[fl->n++] = len;
}

static void test_kiss_round_trip(void)
{
    const uint8_t in[] = { 0x00, 0xC0, 0x41, 0xDB, 0xDB, 0xC0, 0xDC, 0xDD, 0xFF };
    uint8_t wire[64];
    size_t n = bcast_kiss_encode(0x03, in, sizeof in, wire, sizeof wire);
    TEST_ASSERT_EQUAL_size_t(2 + 1 + sizeof in + 4, n);    /* 2 FEND, cmd, 4 escapes */
    for (size_t i = 1; i + 1 < n; i++)
        TEST_ASSERT_NOT_EQUAL(0xC0, wire[i]);

    static frame_log fl;
    fl.n = 0;
    bcast_kiss_decoder d = {0};
    const uint8_t noise[] = { 0x11, 0x22 };                 /* before the first FEND: dropped */
    bcast_kiss_feed(&d, noise, sizeof noise, kiss_cb, &fl);
    for (size_t i = 0; i < n; i++)                          /* one byte at a time */
        bcast_kiss_feed(&d, wire + i, 1, kiss_cb, &fl);
    TEST_ASSERT_EQUAL_INT(1, fl.n);
    TEST_ASSERT_EQUAL_size_t(1 + sizeof in, fl.lens[0]);
    TEST_ASSERT_EQUAL_HEX8(0x03, fl.frames[0][0]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(in, fl.frames[0] + 1, sizeof in);

    TEST_ASSERT_EQUAL_size_t(0, bcast_kiss_encode(0x03, in, sizeof in, wire, 5));  /* too small */
}

static void make_beacon(uint8_t f[14])
{
    beacon b = {0};
    strcpy(b.callsign, "N0CALL");
    b.lat_e7 = 514779000;
    b.lon_e7 = -15000;
    b.emergency = 8;
    TEST_ASSERT_EQUAL_INT(14, beacon_encode(&b, f, 14));
}

static void test_beacon_out(void)
{
    int fd = start_link();
    uint8_t f[14];
    make_beacon(f);
    TEST_ASSERT_EQUAL_INT(0, bcast_link_send_station_frame(f, sizeof f));
    static frame_log fl;
    fl.n = 0;
    read_frames(fd, &fl, 1, 2000);
    TEST_ASSERT_EQUAL_INT(1, fl.n);
    TEST_ASSERT_EQUAL_size_t(14, fl.lens[0]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(f, fl.frames[0], 14);

    uint8_t chat[14] = { 0x04 << 5 };                      /* not a station frame */
    TEST_ASSERT_EQUAL_INT(EINVAL, bcast_link_send_station_frame(chat, sizeof chat));
    uint8_t big[26] = { 0xE1 };
    TEST_ASSERT_EQUAL_INT(EINVAL, bcast_link_send_station_frame(big, sizeof big));
    close(fd);
}

static void test_beacon_in(void)
{
    int fd = start_link();
    uint8_t f[14];
    make_beacon(f);
    send_kiss(fd, f, sizeof f);
    TEST_ASSERT_TRUE(wait_for(&g_station_n, 1, 2000));
    pthread_mutex_lock(&g_lock);
    TEST_ASSERT_EQUAL_size_t(14, g_station_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(f, g_station, 14);
    pthread_mutex_unlock(&g_lock);
    close(fd);
}

static uint8_t *random_bytes(size_t n)
{
    uint8_t *d = malloc(n);
    for (size_t i = 0; i < n; i++)
        d[i] = (uint8_t) rand();
    return d;
}

static void test_carousel_out(void)
{
    int fd = start_link();
    size_t len = 3000;
    uint8_t *data = random_bytes(len);
    char err[256] = "";
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, bcast_link_send_object("hello.bin", data, len, 2, -1, err, sizeof err), err);
    TEST_ASSERT_EQUAL_INT(EBUSY, bcast_link_send_object("other.bin", data, 10, 1, -1, err, sizeof err));

    char rxdir[300];
    snprintf(rxdir, sizeof rxdir, "%s/codec-rx", g_dir);
    mkdir(rxdir, 0700);
    bcast_file_rx_t *rx = bcast_file_rx_open(MODE, rxdir, err, sizeof err);
    TEST_ASSERT_NOT_NULL_MESSAGE(rx, err);

    static frame_log fl;
    fl.n = 0;
    read_frames(fd, &fl, 400, 5000);
    bcast_tx_stats st;
    bcast_link_stats(&st);
    TEST_ASSERT_EQUAL_INT(BCAST_TX_DONE, st.state);
    TEST_ASSERT_EQUAL_INT(fl.n, (int) st.frames_sent);
    /* 3000 bytes + "hello.bin" bundle = 3014 B = 74 symbols of 41, 2 a DATAC3 frame */
    TEST_ASSERT_EQUAL_INT64(37, st.frames_per_pass);
    TEST_ASSERT_EQUAL_INT(2 * 37, fl.n);
    TEST_ASSERT_EQUAL_INT(2, st.pass);

    bool done = false;
    for (int i = 0; i < fl.n && !done; i++)
    {
        TEST_ASSERT_EQUAL_size_t(126, fl.lens[i]);
        done = bcast_file_rx_frame(rx, fl.frames[i], fl.lens[i]) == BCAST_RX_COMPLETE;
    }
    TEST_ASSERT_TRUE_MESSAGE(done, "the codec did not decode the carousel");
    TEST_ASSERT_EQUAL_STRING("hello.bin", bcast_file_rx_last_name(rx));
    FILE *f = fopen(bcast_file_rx_last_path(rx), "rb");
    TEST_ASSERT_NOT_NULL(f);
    uint8_t *back = malloc(len + 1);
    TEST_ASSERT_EQUAL_size_t(len, fread(back, 1, len + 1, f));
    fclose(f);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, back, len);
    free(back);
    free(data);
    bcast_file_rx_close(rx);
    close(fd);
}

static void test_carousel_in(void)
{
    int fd = start_link();
    size_t len = 2000;
    uint8_t *data = random_bytes(len);
    char err[256] = "";
    /* the codec's cycles count frames per block: 0 runs it until we stop */
    bcast_file_tx_t *tx = bcast_file_tx_open_mem("in.dat", data, len, MODE, 0, 0, err, sizeof err);
    TEST_ASSERT_NOT_NULL_MESSAGE(tx, err);
    uint8_t frame[BCAST_FILE_MAX_FRAME];
    int n, sent = 0;
    while (sent++ < 400 && (n = bcast_file_tx_next(tx, frame, sizeof frame)) > 0 &&
           atomic_load(&g_obj_n) == 0)
    {
        send_kiss(fd, frame, (size_t) n);
        usleep(2000);
    }
    bcast_file_tx_close(tx);
    TEST_ASSERT_TRUE_MESSAGE(wait_for(&g_obj_n, 1, 2000), "the link decoded nothing");

    pthread_mutex_lock(&g_lock);
    TEST_ASSERT_EQUAL_STRING("in.dat", g_obj_name);
    FILE *f = fopen(g_obj_path, "rb");
    pthread_mutex_unlock(&g_lock);
    TEST_ASSERT_NOT_NULL(f);
    uint8_t *back = malloc(len + 1);
    TEST_ASSERT_EQUAL_size_t(len, fread(back, 1, len + 1, f));
    fclose(f);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, back, len);
    free(back);
    free(data);
    close(fd);
}

static void test_frame_limit_and_stop(void)
{
    int fd = start_link();
    uint8_t *data = random_bytes(5000);
    char err[256] = "";
    TEST_ASSERT_EQUAL_INT(0, bcast_link_send_object("five.bin", data, 5000, 0, 5, err, sizeof err));
    static frame_log fl;
    fl.n = 0;
    read_frames(fd, &fl, 50, 1500);
    TEST_ASSERT_EQUAL_INT(5, fl.n);
    bcast_tx_stats st;
    bcast_link_stats(&st);
    TEST_ASSERT_EQUAL_INT(BCAST_TX_DONE, st.state);
    TEST_ASSERT_EQUAL_UINT64(5, st.frames_sent);

    /* endless, then stopped */
    TEST_ASSERT_EQUAL_INT(0, bcast_link_send_object("endless.bin", data, 5000, 0, -1, err, sizeof err));
    fl.n = 0;
    read_frames(fd, &fl, 20, 2000);
    TEST_ASSERT_TRUE(fl.n >= 20);            /* one read can carry more than asked for */
    bcast_link_stop_object();
    for (int i = 0; i < 100; i++)
    {
        bcast_link_stats(&st);
        if (st.state != BCAST_TX_SENDING)
            break;
        /* keep reading so a send blocked on a full socket can finish */
        fl.n = 0;
        read_frames(fd, &fl, 400, 20);
    }
    TEST_ASSERT_EQUAL_INT(BCAST_TX_STOPPED, st.state);
    free(data);
    close(fd);
}

static void test_reconnects(void)
{
    int fd = start_link();
    close(fd);                                   /* Mercury restarts */
    int fd2 = accept_within(5000);
    TEST_ASSERT_TRUE_MESSAGE(fd2 >= 0, "the link did not come back");
    uint8_t f[14];
    make_beacon(f);
    int rc = ENOTCONN;
    for (int i = 0; i < 200 && rc != 0; i++)
    {
        rc = bcast_link_send_station_frame(f, sizeof f);
        if (rc)
            usleep(10000);
    }
    TEST_ASSERT_EQUAL_INT(0, rc);
    static frame_log fl;
    fl.n = 0;
    read_frames(fd2, &fl, 1, 2000);
    TEST_ASSERT_EQUAL_INT(1, fl.n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(f, fl.frames[0], 14);
    close(fd2);
}

int main(void)
{
    snprintf(g_dir, sizeof g_dir, "%s/bcast-link-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    TEST_REQUIRE(mkdtemp(g_dir));
    g_listen = listen_any();
    srand(1);

    UNITY_BEGIN();
    RUN_TEST(test_kiss_round_trip);
    RUN_TEST(test_beacon_out);
    RUN_TEST(test_beacon_in);
    RUN_TEST(test_carousel_out);
    RUN_TEST(test_carousel_in);
    RUN_TEST(test_frame_limit_and_stop);
    RUN_TEST(test_reconnects);
    int rc = UNITY_END();

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd)) {}
    return rc;
}
