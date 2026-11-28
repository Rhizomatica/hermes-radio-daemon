/* test_ale_handshake - the ALE link layer (modem/ale_engine.cpp over PC-ALE's
 * controller), two stations on one simulated channel: a call, its three-way
 * handshake, AMD, a manual accept and a reject, and terminating a link.
 *
 * Each station is an engine with radio callbacks: what one transmits is
 * played at 8 kHz, 20 ms a step, and heard by the other, as the radio's
 * modem_audio TX and RX paths would carry it. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ale_engine.h"
#include "unity.h"

#define STEP_SAMPLES   160               /* 20 ms at 8 kHz, as modem_audio's blocks */
#define STEP_MS        20
#define TXQ_SAMPLES    (ALE_ENGINE_RATE * 30)
#define MAX_EVENTS     64
#define CHANNEL_HZ     7050000
/* A Hamlib rig's TX queue (radio_media's ring): 16000 samples at 48 kHz,
 * 333 ms, which drops what does not fit; ALE must never queue more. */
#define RIG_TXQ_SAMPLES (ALE_ENGINE_RATE / 3)
/* Long enough for a call, its handshake and a termination at the
 * controller's timing (a call is a few seconds of words). */
#define CALL_SECONDS   60

typedef struct
{
    const char *name;
    ale_engine *e;
    bool keyed;
    uint32_t hz;
    int16_t txq[TXQ_SAMPLES];
    size_t txq_rd, txq_n;
    uint64_t played;                      /* this over */
    size_t txq_max;                       /* the most ever queued */
    ale_link_event ev[MAX_EVENTS];
    int nev;
} station;

static station sam, bob;

static bool st_retune(uint32_t hz, uint8_t mode, void *user)
{
    (void) mode;
    ((station *) user)->hz = hz;
    return true;
}

static bool st_key(bool on, void *user)
{
    station *s = user;
    s->keyed = on;
    if (on)
        s->played = 0;
    return true;
}

static void st_tx_audio(const int16_t *pcm, size_t n, void *user)
{
    station *s = user;
    for (size_t i = 0; i < n && s->txq_n < TXQ_SAMPLES; i++)
        s->txq[(s->txq_rd + s->txq_n++) % TXQ_SAMPLES] = pcm[i];
    if (s->txq_n > s->txq_max)
        s->txq_max = s->txq_n;
}

static uint64_t st_tx_played(void *user)
{
    return ((station *) user)->played;
}

static void st_event(const ale_link_event *ev, void *user)
{
    station *s = user;
    if (ev->kind != ALE_EV_STATUS && s->nev < MAX_EVENTS)
        s->ev[s->nev++] = *ev;
}

static void start(station *s, const char *name, bool manual_accept)
{
    memset(s, 0, sizeof(*s));
    s->name = name;
    s->hz = CHANNEL_HZ;
    s->e = ale_engine_create((const uint8_t[]) { 4 }, 1, NULL, NULL);
    TEST_ASSERT_NOT_NULL(s->e);
    ale_link_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.self, name);
    cfg.manual_accept = manual_accept;
    cfg.accept_timeout_ms = 20000;
    cfg.start_hz = CHANNEL_HZ;
    cfg.tx_tail_ms = 120;
    cfg.start_mode = ALE_MODE_USB;
    ale_radio_ops ops = { st_retune, st_key, st_tx_audio, st_tx_played, s };
    TEST_ASSERT_EQUAL_INT(0, ale_engine_link_start(s->e, &cfg, &ops, st_event, s));
}

/* What s's radio sends in this step: its queued audio while keyed, else nothing. */
static void play(station *s, int16_t *out)
{
    memset(out, 0, STEP_SAMPLES * sizeof(*out));
    if (!s->keyed)
        return;
    for (size_t i = 0; i < STEP_SAMPLES && s->txq_n; i++)
    {
        out[i] = s->txq[s->txq_rd];
        s->txq_rd = (s->txq_rd + 1) % TXQ_SAMPLES;
        s->txq_n--;
        s->played++;
    }
}

static uint32_t now_ms;

static void run(unsigned ms)
{
    int16_t a[STEP_SAMPLES], b[STEP_SAMPLES];
    for (unsigned t = 0; t < ms; t += STEP_MS)
    {
        play(&sam, a);
        play(&bob, b);
        /* each hears the other when both are on one channel (and not itself) */
        if (sam.hz == bob.hz)
        {
            ale_engine_feed(bob.e, a, STEP_SAMPLES);
            ale_engine_feed(sam.e, b, STEP_SAMPLES);
        }
        now_ms += STEP_MS;
        ale_engine_tick(sam.e, now_ms);
        ale_engine_tick(bob.e, now_ms);
    }
}

static const ale_link_event *find(const station *s, uint8_t kind)
{
    for (int i = 0; i < s->nev; i++)
        if (s->ev[i].kind == kind)
            return &s->ev[i];
    return NULL;
}

static const char *state(station *s)
{
    static ale_link_state st;
    ale_engine_link_get_state(s->e, &st);
    return st.state;
}

/* Run until both are in state `want` (or the time is up). */
static bool run_until_both(const char *want, unsigned max_ms)
{
    for (unsigned t = 0; t < max_ms; t += 100)
    {
        run(100);
        if (!strcmp(state(&sam), want) && !strcmp(state(&bob), want))
            return true;
    }
    return false;
}

void setUp(void) { now_ms = 1000; }
void tearDown(void)
{
    ale_engine_destroy(sam.e);
    ale_engine_destroy(bob.e);
}

static void test_call_links_and_terminates(void)
{
    start(&sam, "SAM", false);
    start(&bob, "BOB", false);
    run(1000);
    TEST_ASSERT_TRUE(ale_engine_call(sam.e, "BOB", NULL));
    TEST_ASSERT_TRUE_MESSAGE(run_until_both("LINKED", CALL_SECONDS * 1000), "no link");
    const ale_link_event *ls = find(&sam, ALE_EV_LINKED), *lb = find(&bob, ALE_EV_LINKED);
    TEST_ASSERT_NOT_NULL(ls);
    TEST_ASSERT_NOT_NULL(lb);
    TEST_ASSERT_EQUAL_STRING("BOB", ls->peer);
    TEST_ASSERT_EQUAL_STRING("SAM", lb->peer);
    TEST_ASSERT_NOT_NULL(find(&bob, ALE_EV_CALL));
    TEST_ASSERT_FALSE(sam.keyed);
    TEST_ASSERT_FALSE(bob.keyed);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(RIG_TXQ_SAMPLES, sam.txq_max);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(RIG_TXQ_SAMPLES, bob.txq_max);

    TEST_ASSERT_TRUE(ale_engine_terminate(sam.e));
    run(CALL_SECONDS * 1000 / 4);
    TEST_ASSERT_NOT_NULL(find(&sam, ALE_EV_TERMINATED));
    TEST_ASSERT_FALSE(sam.keyed);
    TEST_ASSERT_NOT_EQUAL(0, strcmp("LINKED", state(&sam)));
    TEST_ASSERT_NOT_NULL_MESSAGE(find(&bob, ALE_EV_TERMINATED), "the called station missed the termination");
    TEST_ASSERT_NOT_EQUAL(0, strcmp("LINKED", state(&bob)));
}

/* Two addresses that share their first three characters (as a network's
 * ABCA ... ABCL do), each two words long: the termination's TWAS carries the
 * caller's address, which must not read as a call to the called station. */
static void test_shared_prefix_link_and_terminate(void)
{
    start(&sam, "ABCA", false);
    start(&bob, "ABCH", false);
    run(1000);
    TEST_ASSERT_TRUE(ale_engine_call(sam.e, "ABCH", NULL));
    TEST_ASSERT_TRUE_MESSAGE(run_until_both("LINKED", CALL_SECONDS * 1000), "no link");
    TEST_ASSERT_EQUAL_STRING("ABCH", find(&sam, ALE_EV_LINKED)->peer);
    TEST_ASSERT_TRUE(ale_engine_terminate(sam.e));
    run(CALL_SECONDS * 1000 / 4);
    TEST_ASSERT_NOT_NULL_MESSAGE(find(&bob, ALE_EV_TERMINATED), "the called station missed the termination");
    TEST_ASSERT_NOT_EQUAL(0, strcmp("LINKED", state(&bob)));
    TEST_ASSERT_FALSE(sam.keyed);
    TEST_ASSERT_FALSE(bob.keyed);
}

/* A sounding goes out, the other station hears it, and the sounding station
 * is back to idle (the last frame's completion comes: a stuck one leaves it
 * in SOUNDING). */
static void test_sounding_ends(void)
{
    start(&sam, "SAM", false);
    start(&bob, "BOB", false);
    run(1000);
    TEST_ASSERT_TRUE(ale_engine_sound(sam.e));
    run(10000);
    /* PC-ALE goes on to SCANNING after a sounding (ale_link puts a station
     * that only listens back) */
    TEST_ASSERT_NOT_EQUAL(0, strcmp("SOUNDING", state(&sam)));
    TEST_ASSERT_FALSE(sam.keyed);
    TEST_ASSERT_TRUE(sam.played > 0);
}

/* The radio is keyed only for audio: no key-up without a word to send. */
static int keyups;
static bool count_key(bool on, void *user)
{
    if (on)
        keyups++;
    return st_key(on, user);
}

static void test_no_empty_keyups(void)
{
    start(&sam, "SAM", false);
    start(&bob, "BOB", false);
    ale_link_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.self, "BOB");
    cfg.accept_timeout_ms = 20000;
    cfg.start_hz = CHANNEL_HZ;
    cfg.tx_tail_ms = 120;
    cfg.start_mode = ALE_MODE_USB;
    ale_radio_ops ops = { st_retune, count_key, st_tx_audio, st_tx_played, &bob };
    TEST_ASSERT_EQUAL_INT(0, ale_engine_link_start(bob.e, &cfg, &ops, st_event, &bob));
    keyups = 0;
    run(1000);
    TEST_ASSERT_TRUE(ale_engine_call(sam.e, "BOB", NULL));
    TEST_ASSERT_TRUE_MESSAGE(run_until_both("LINKED", CALL_SECONDS * 1000), "no link");
    run(2000);
    /* BOB sent one transmission, its response */
    TEST_ASSERT_EQUAL_INT(1, keyups);
}

static void test_amd_reaches_the_called_station(void)
{
    start(&sam, "SAM", false);
    start(&bob, "BOB", false);
    run(1000);
    TEST_ASSERT_TRUE(ale_engine_call(sam.e, "BOB", "HELLO BOB"));
    TEST_ASSERT_TRUE_MESSAGE(run_until_both("LINKED", CALL_SECONDS * 1000), "no link");
    const ale_link_event *amd = find(&bob, ALE_EV_AMD);
    TEST_ASSERT_NOT_NULL_MESSAGE(amd, "no AMD");
    TEST_ASSERT_EQUAL_STRING("HELLO BOB", amd->text);
    TEST_ASSERT_EQUAL_STRING("SAM", amd->peer);
}

static void test_manual_accept_waits_then_links(void)
{
    start(&sam, "SAM", false);
    start(&bob, "BOB", true);
    run(1000);
    TEST_ASSERT_TRUE(ale_engine_call(sam.e, "BOB", NULL));
    for (unsigned t = 0; t < CALL_SECONDS * 1000 && !find(&bob, ALE_EV_CALL); t += 100)
        run(100);
    TEST_ASSERT_NOT_NULL_MESSAGE(find(&bob, ALE_EV_CALL), "BOB heard no call");
    TEST_ASSERT_FALSE(bob.keyed);                       /* no answer before accept */
    TEST_ASSERT_TRUE(ale_engine_accept(bob.e));
    TEST_ASSERT_TRUE_MESSAGE(run_until_both("LINKED", CALL_SECONDS * 1000), "no link after accept");
}

static void test_reject_gives_no_link(void)
{
    start(&sam, "SAM", false);
    start(&bob, "BOB", true);
    run(1000);
    TEST_ASSERT_TRUE(ale_engine_call(sam.e, "BOB", NULL));
    for (unsigned t = 0; t < CALL_SECONDS * 1000 && !find(&bob, ALE_EV_CALL); t += 100)
        run(100);
    TEST_ASSERT_NOT_NULL(find(&bob, ALE_EV_CALL));
    TEST_ASSERT_TRUE(ale_engine_reject(bob.e));
    run(CALL_SECONDS * 1000);
    TEST_ASSERT_NULL(find(&sam, ALE_EV_LINKED));
    TEST_ASSERT_NULL(find(&bob, ALE_EV_LINKED));
    TEST_ASSERT_FALSE(sam.keyed);
    TEST_ASSERT_FALSE(bob.keyed);
}

static void test_bad_addresses_and_text_are_refused(void)
{
    start(&sam, "SAM", false);
    start(&bob, "BOB", false);
    TEST_ASSERT_FALSE(ale_engine_call(sam.e, "bob", NULL));
    TEST_ASSERT_FALSE(ale_engine_call(sam.e, "", NULL));
    TEST_ASSERT_FALSE(ale_engine_call(sam.e, "ABCDEFGHIJKLMNOP", NULL));
    TEST_ASSERT_FALSE(ale_engine_call(sam.e, "BOB", "lower case"));
    TEST_ASSERT_TRUE(ale_address_valid("K1ABC"));
    TEST_ASSERT_FALSE(ale_address_valid("K1-AB"));
    TEST_ASSERT_TRUE(ale_amd_valid("QSY 7102 USB, OK?"));
    TEST_ASSERT_FALSE(ale_amd_valid("caf\xc3\xa9"));
    TEST_ASSERT_FALSE(ale_engine_terminate(sam.e));      /* no link */
    TEST_ASSERT_FALSE(ale_engine_accept(bob.e));          /* no call */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bad_addresses_and_text_are_refused);
    RUN_TEST(test_call_links_and_terminates);
    RUN_TEST(test_shared_prefix_link_and_terminate);
    RUN_TEST(test_no_empty_keyups);
    RUN_TEST(test_sounding_ends);
    RUN_TEST(test_amd_reaches_the_called_station);
    RUN_TEST(test_manual_accept_waits_then_links);
    RUN_TEST(test_reject_gives_no_link);
    return UNITY_END();
}
