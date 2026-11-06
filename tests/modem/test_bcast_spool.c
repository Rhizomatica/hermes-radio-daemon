/* bcast_spool: the spool rules (frame budgets, names, the oldest file, unique
 * receive names), then the spool with the link against a fake Mercury -- a
 * budgeted file sent and removed, an endless one stopped when it is removed,
 * and a received file appearing whole in rx.
 */
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "unity.h"
#include "test_util.h"
#include "modem/bcast_link.h"
#include "modem/bcast_spool.h"
#include "bcast_file.h"

#define MODE 1

static char g_dir[256], g_tx[300], g_rx[300];
static int  g_listen = -1, g_port;

void setUp(void) {}
void tearDown(void) {}

static void write_file(const char *path, const void *data, size_t len, time_t mtime)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_size_t(len, fwrite(data, 1, len, f));
    fclose(f);
    if (mtime)
    {
        struct utimbuf u = { mtime, mtime };
        utime(path, &u);
    }
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* ── the rules ── */

static void test_frames_limit(void)
{
    TEST_ASSERT_EQUAL_INT64(500, bcast_spool_frames_limit("/x/20261106-area-500_frames.nncp"));
    TEST_ASSERT_EQUAL_INT64(7, bcast_spool_frames_limit("a-7_frames"));
    TEST_ASSERT_EQUAL_INT64(-1, bcast_spool_frames_limit("plain.bin"));
    TEST_ASSERT_EQUAL_INT64(-1, bcast_spool_frames_limit("a_frames.bin"));     /* no number */
    TEST_ASSERT_EQUAL_INT64(-1, bcast_spool_frames_limit("a500_frames.bin"));  /* no dash */
    TEST_ASSERT_EQUAL_INT64(-1, bcast_spool_frames_limit("-500_frames.bin"));  /* no stem */
    TEST_ASSERT_EQUAL_INT64(-1, bcast_spool_frames_limit("a-0_frames.bin"));
    TEST_ASSERT_EQUAL_INT64(-1, bcast_spool_frames_limit("/dir-9_frames/plain.bin"));  /* the dir's */
}

static void test_send_name(void)
{
    char n[256];
    bcast_spool_send_name("/s/20261106T1200Z-area-500_frames.nncp", n, sizeof n);
    TEST_ASSERT_EQUAL_STRING("20261106T1200Z-area.nncp", n);
    bcast_spool_send_name("/s/report.pdf", n, sizeof n);
    TEST_ASSERT_EQUAL_STRING("report.pdf", n);
    bcast_spool_send_name("x-3_frames", n, sizeof n);
    TEST_ASSERT_EQUAL_STRING("x", n);
}

static void test_pick_oldest(void)
{
    char d[300], p[400], out[600];
    snprintf(d, sizeof d, "%s/pick", g_dir);
    mkdir(d, 0700);
    TEST_ASSERT_FALSE(bcast_spool_pick(d, out, sizeof out, NULL));
    snprintf(p, sizeof p, "%s/new", d);      write_file(p, "n", 1, 2000000000);
    snprintf(p, sizeof p, "%s/old-b", d);    write_file(p, "b", 1, 1000000000);
    snprintf(p, sizeof p, "%s/old-a", d);    write_file(p, "a", 1, 1000000000);
    snprintf(p, sizeof p, "%s/.hidden", d);  write_file(p, "h", 1, 1);
    snprintf(p, sizeof p, "%s/sub", d);      mkdir(p, 0700);
    time_t mt = 0;
    TEST_ASSERT_TRUE(bcast_spool_pick(d, out, sizeof out, &mt));
    snprintf(p, sizeof p, "%s/old-a", d);
    TEST_ASSERT_EQUAL_STRING(p, out);        /* oldest; the name breaks the tie */
    TEST_ASSERT_EQUAL_INT64(1000000000, (int64_t) mt);
}

static void test_rx_target(void)
{
    char d[300], out[600], p[400];
    snprintf(d, sizeof d, "%s/rxt", g_dir);
    mkdir(d, 0700);
    TEST_ASSERT_TRUE(bcast_spool_rx_target(d, "file.txt", out, sizeof out));
    snprintf(p, sizeof p, "%s/file.txt", d);
    TEST_ASSERT_EQUAL_STRING(p, out);
    write_file(p, "x", 1, 0);
    TEST_ASSERT_TRUE(bcast_spool_rx_target(d, "file.txt", out, sizeof out));
    snprintf(p, sizeof p, "%s/file.txt.1", d);
    TEST_ASSERT_EQUAL_STRING(p, out);         /* never over an existing file */
    TEST_ASSERT_TRUE(bcast_spool_rx_target(d, "../../etc/passwd", out, sizeof out));
    snprintf(p, sizeof p, "%s/passwd", d);
    TEST_ASSERT_EQUAL_STRING(p, out);         /* basename only */
    TEST_ASSERT_TRUE(bcast_spool_rx_target(d, ".hidden", out, sizeof out));
    snprintf(p, sizeof p, "%s/hidden", d);
    TEST_ASSERT_EQUAL_STRING(p, out);         /* never hidden from hermes-broadcast-rx */
    TEST_ASSERT_TRUE(bcast_spool_rx_target(d, "", out, sizeof out));
    TEST_ASSERT_NOT_NULL(strstr(out, "/broadcast_"));
}

/* ── with the link ── */

static int g_fd = -1;

static void count_frame(const uint8_t *f, size_t len, void *u)
{
    (void) f; (void) len;
    (*(int *) u)++;
}

/* Frames the link sends within ms. */
static int drain(int ms)
{
    bcast_kiss_decoder d = {0};
    uint8_t buf[8192];
    int n = 0;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;)
    {
        clock_gettime(CLOCK_MONOTONIC, &t);
        int left = ms - (int) ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0)
            return n;
        struct pollfd p = { .fd = g_fd, .events = POLLIN };
        if (poll(&p, 1, left) <= 0)
            return n;
        ssize_t r = recv(g_fd, buf, sizeof buf, 0);
        if (r <= 0)
            return n;
        bcast_kiss_feed(&d, buf, (size_t) r, count_frame, &n);
    }
}

static void test_plane_starts(void)
{
    bcast_spool_config sc = {0};
    snprintf(sc.tx_dir, sizeof sc.tx_dir, "%s", g_tx);
    snprintf(sc.rx_dir, sizeof sc.rx_dir, "%s", g_rx);
    TEST_ASSERT_TRUE(bcast_spool_start(&sc));
    bcast_link_config lc = { .host = "127.0.0.1", .port = g_port, .mode = MODE };
    bcast_spool_incoming_dir(&sc, lc.incoming_dir, sizeof lc.incoming_dir);
    bcast_link_ops ops = { .on_object = bcast_spool_on_object };
    TEST_ASSERT_TRUE(bcast_link_start(&lc, &ops));
    struct pollfd p = { .fd = g_listen, .events = POLLIN };
    TEST_ASSERT_TRUE(poll(&p, 1, 3000) > 0);
    g_fd = accept(g_listen, NULL, NULL);
    TEST_ASSERT_TRUE(g_fd >= 0);
}

static void test_budget_then_removed(void)
{
    char p[400];
    snprintf(p, sizeof p, "%s/bundle-6_frames.nncp", g_tx);
    uint8_t data[1500];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t) (i * 7);
    write_file(p, data, sizeof data, 0);

    int n = 0;
    for (int i = 0; i < 40 && exists(p); i++)
        n += drain(100);
    n += drain(200);
    TEST_ASSERT_FALSE_MESSAGE(exists(p), "the budgeted file was not removed");
    TEST_ASSERT_EQUAL_INT(6, n);
}

static void test_endless_until_removed(void)
{
    char p[400];
    snprintf(p, sizeof p, "%s/notice.txt", g_tx);
    write_file(p, "a notice for the whole area\n", 28, 0);
    int n = 0;
    for (int i = 0; i < 30 && n < 30; i++)
        n += drain(100);
    TEST_ASSERT_TRUE(n >= 30);
    unlink(p);
    for (int i = 0; i < 30; i++)       /* the spool notices within a second */
    {
        drain(100);
        bcast_tx_stats st;
        bcast_link_stats(&st);
        if (st.state == BCAST_TX_STOPPED)
            break;
    }
    drain(300);
    TEST_ASSERT_EQUAL_INT(0, drain(700));
}

static void test_received_into_rx(void)
{
    uint8_t data[900];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t) (i ^ 0x5A);
    char err[256] = "";
    bcast_file_tx_t *tx = bcast_file_tx_open_mem("bulletin.txt", data, sizeof data, MODE, 0, 0, err, sizeof err);
    TEST_ASSERT_NOT_NULL_MESSAGE(tx, err);
    uint8_t frame[BCAST_FILE_MAX_FRAME], wire[2 * BCAST_FILE_MAX_FRAME + 8];
    int n;
    char want[400];
    snprintf(want, sizeof want, "%s/bulletin.txt", g_rx);
    int sent = 0;
    while (sent++ < 400 && (n = bcast_file_tx_next(tx, frame, sizeof frame)) > 0 && !exists(want))
    {
        size_t w = bcast_kiss_encode(0x03, frame, (size_t) n, wire, sizeof wire);
        TEST_ASSERT_EQUAL_INT((int) w, (int) send(g_fd, wire, w, MSG_NOSIGNAL));
        usleep(3000);
    }
    bcast_file_tx_close(tx);
    for (int i = 0; i < 100 && !exists(want); i++)
        usleep(10000);
    TEST_ASSERT_TRUE_MESSAGE(exists(want), "nothing appeared in rx");
    FILE *f = fopen(want, "rb");
    uint8_t back[1000];
    TEST_ASSERT_EQUAL_size_t(sizeof data, fread(back, 1, sizeof back, f));
    fclose(f);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, back, sizeof data);
}

int main(void)
{
    const char *t = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/bcast-spool-XXXXXX", t && t[0] ? t : "/tmp");
    TEST_REQUIRE(mkdtemp(g_dir));
    snprintf(g_tx, sizeof g_tx, "%s/tx", g_dir);
    snprintf(g_rx, sizeof g_rx, "%s/rx", g_dir);

    g_listen = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    TEST_REQUIRE(bind(g_listen, (struct sockaddr *) &a, sizeof a) == 0);
    socklen_t al = sizeof a;
    getsockname(g_listen, (struct sockaddr *) &a, &al);
    g_port = ntohs(a.sin_port);
    listen(g_listen, 4);

    UNITY_BEGIN();
    RUN_TEST(test_frames_limit);
    RUN_TEST(test_send_name);
    RUN_TEST(test_pick_oldest);
    RUN_TEST(test_rx_target);
    RUN_TEST(test_plane_starts);       /* asserts only inside RUN_TEST */
    RUN_TEST(test_budget_then_removed);
    RUN_TEST(test_endless_until_removed);
    RUN_TEST(test_received_into_rx);
    bcast_link_stop();
    bcast_spool_stop();
    int rc = UNITY_END();

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd)) {}
    return rc;
}
