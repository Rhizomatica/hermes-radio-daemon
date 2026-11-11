/* js8_worker: audio fed in 100 ms pieces, as the backends feed it, comes
 * out of the worker's thread as a log entry: JS8Call's Slow recording,
 * with Normal and Slow both enabled, is logged once, as Slow alone hears
 * it; then a new dial frequency starts the ring again.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "unity.h"
#include "dsp/digi_log.h"
#include "dsp/js8_worker.h"

#define PIECE       (JS8_SAMPLE_RATE / 10)
#define SECONDS     32
#define WAIT_TRIES  200          /* x 50 ms */
#define DIAL_HZ     7078000u

/* radiod_cpus.h: none known here, so the worker runs anywhere. */
bool radiod_unpinned_cpus(cpu_set_t *out) { (void) out; return false; }

static float audio[SECONDS * JS8_SAMPLE_RATE];
static char dir[] = "/tmp/test_js8_worker.XXXXXX";
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int entries;
static char text[128];
static uint64_t freq;

static void listener(const digi_log_entry *e, double at)
{
    (void) at;
    pthread_mutex_lock(&lock);
    entries++;
    snprintf(text, sizeof text, "%s", e->text ? e->text : "");
    freq = e->freq_hz;
    pthread_mutex_unlock(&lock);
}

void setUp(void) {}
void tearDown(void) {}

static int load_wav(const char *path)
{
    static uint8_t buf[2 * SECONDS * JS8_SAMPLE_RATE + 4096];
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    size_t sz = fread(buf, 1, sizeof buf, f);
    fclose(f);
    for (size_t off = 12; off + 8 <= sz;) {
        uint32_t len = buf[off + 4] | buf[off + 5] << 8 | buf[off + 6] << 16 | (uint32_t) buf[off + 7] << 24;
        if (!memcmp(buf + off, "data", 4)) {
            int n = 0;
            for (size_t i = off + 8; i + 1 < sz && i + 1 < off + 8 + len; i += 2)
                audio[n++] = (int16_t) (buf[i] | buf[i + 1] << 8) / 32768.0f;
            return n;
        }
        off += 8 + len + (len & 1);
    }
    return 0;
}

static int count(void)
{
    pthread_mutex_lock(&lock);
    int n = entries;
    pthread_mutex_unlock(&lock);
    return n;
}

/* Feeds n samples as if they were heard from t0 on, then 2 s of silence
 * to let the windows past it be decoded. */
static void feed(int n, double t0, uint64_t dial)
{
    static const float quiet[PIECE];
    int at = 0;
    for (; at + PIECE <= n; at += PIECE)
        js8_worker_feed(audio + at, PIECE, t0 + (double) (at + PIECE) / JS8_SAMPLE_RATE, dial);
    for (int i = 0; i < 2 * JS8_SAMPLE_RATE / PIECE; i++, at += PIECE)
        js8_worker_feed(quiet, PIECE, t0 + (double) (at + PIECE) / JS8_SAMPLE_RATE, dial);
}

static void test_slow_through_the_worker(void)
{
    int n = load_wav("tests/fixtures/js8/E_1_1.wav");
    TEST_ASSERT_TRUE(n > 0);
    digi_log_set_listener(listener);
    js8_worker_set_submodes(1u << JS8_SUB_NORMAL | 1u << JS8_SUB_SLOW);
    double t0 = (double) time(NULL);
    feed(n, t0, DIAL_HZ);
    for (int i = 0; i < WAIT_TRIES && count() == 0; i++)
        usleep(50000);
    usleep(500000);   /* any second entry would have come by now */
    TEST_ASSERT_EQUAL_INT(1, count());
    TEST_ASSERT_EQUAL_STRING("KN4CRD: TEST", text);
    TEST_ASSERT_EQUAL_UINT64(DIAL_HZ, freq);

    /* The same audio on another dial: the ring starts again, and it is a
     * new transmission there. */
    feed(n, t0 + SECONDS, DIAL_HZ + 1000);
    for (int i = 0; i < WAIT_TRIES && count() < 2; i++)
        usleep(50000);
    TEST_ASSERT_EQUAL_INT(2, count());
    TEST_ASSERT_EQUAL_UINT64(DIAL_HZ + 1000, freq);
    js8_worker_shutdown();
    digi_log_set_listener(NULL);
}

int main(void)
{
    if (!mkdtemp(dir))
        return 1;
    digi_log_set_dir(dir);
    UNITY_BEGIN();
    RUN_TEST(test_slow_through_the_worker);
    int rc = UNITY_END();
    char cmd[sizeof dir + 16];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    (void) !system(cmd);
    return rc;
}
