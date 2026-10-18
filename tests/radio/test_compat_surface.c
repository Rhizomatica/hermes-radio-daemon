/* Regression test for the SHM command dispatch surface.
 *
 * After the websocket consolidation, the websocket transport tests that used
 * to live here are obsolete (they exercised the old raw-socket implementation,
 * now replaced by the in-tree websocket/ module). State-JSON content is now
 * covered end-to-end by the Pi smoke test described in the plan; this file
 * keeps coverage of the SHM protocol semantics, which has not changed.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "unity.h"
#include "radio.h"
#include "radio_backend.h"
#include "radio_pipeline.h"
#include "include/radio_cmds.h"
#include "include/sbitx_io.h"

_Atomic bool shutdown_ = false;

static struct {
    uint32_t frequency;
    uint16_t mode;
    uint32_t speaker_level;
    uint16_t power_level;
    uint32_t step_size;
    uint32_t bfo;
    uint32_t serial;
    uint32_t ref_threshold;
    int32_t  timeout;
    bool     digital_voice;
    bool     tone_generation;
    bool     txrx_state;
    int      txrx_calls;
    uint32_t profile;
    bool     set_profile_called;
} backend_call;

void radio_backend_set_frequency(radio *radio_h, uint32_t f, uint32_t p)
{ backend_call.frequency = f; backend_call.profile = p;
  if (p < radio_h->profiles_count) radio_h->profiles[p].freq = f; }

void radio_backend_set_mode(radio *radio_h, uint16_t m, uint32_t p)
{ backend_call.mode = m; backend_call.profile = p;
  if (p < radio_h->profiles_count) radio_h->profiles[p].mode = m; }

void radio_backend_set_txrx_state(radio *radio_h, bool s)
{ backend_call.txrx_state = s; backend_call.txrx_calls++; radio_h->txrx_state = s; }

/* Owner bookkeeping as radio_backend.c does it, minus the lock. */
static struct { ptt_source src; long id; int releases; } ptt_owner;

void radio_backend_set_ptt(radio *radio_h, bool s, ptt_source src, long id)
{
    ptt_owner.src = s ? src : PTT_SRC_NONE;
    ptt_owner.id  = s ? id : 0;
    radio_backend_set_txrx_state(radio_h, s);
}

bool radio_backend_release_ptt(radio *radio_h, ptt_source src, long id, const char *why)
{
    (void) why;
    if (src == PTT_SRC_NONE || ptt_owner.src != src || ptt_owner.id != id)
        return false;
    ptt_owner.src = PTT_SRC_NONE;
    ptt_owner.id  = 0;
    ptt_owner.releases++;
    radio_backend_set_txrx_state(radio_h, IN_RX);
    return true;
}

void radio_backend_set_bfo(radio *radio_h, uint32_t f)
{ backend_call.bfo = f; radio_h->bfo_frequency = f; }

void radio_backend_set_reflected_threshold(radio *radio_h, uint32_t t)
{ backend_call.ref_threshold = t; radio_h->reflected_threshold = t; }

void radio_backend_set_speaker_volume(radio *radio_h, uint32_t v, uint32_t p)
{ backend_call.speaker_level = v; backend_call.profile = p;
  if (p < radio_h->profiles_count) radio_h->profiles[p].speaker_level = v; }

void radio_backend_set_serial(radio *radio_h, uint32_t s)
{ backend_call.serial = s; radio_h->serial_number = s; }

void radio_backend_set_profile_timeout(radio *radio_h, int32_t t)
{ backend_call.timeout = t; radio_h->profile_timeout = t; }

void radio_backend_set_power_level(radio *radio_h, uint16_t pwr, uint32_t p)
{ backend_call.power_level = pwr; backend_call.profile = p;
  if (p < radio_h->profiles_count) radio_h->profiles[p].power_level_percentage = pwr; }

bool radio_backend_set_digital_voice(radio *radio_h, bool dv, uint32_t p)
{ backend_call.digital_voice = dv; backend_call.profile = p;
  if (p >= radio_h->profiles_count) return false;
  radio_h->profiles[p].digital_voice = dv;
  radio_pipeline_refresh(radio_h); return true; }

bool radio_backend_get_digital_voice(const radio *radio_h, uint32_t p)
{ if (p >= radio_h->profiles_count) return false;
  if (radio_h->digital_voice_codec == DV_CODEC_DSTAR)
      return radio_h->profiles[p].mode == MODE_DSTAR;
  return radio_h->profiles[p].digital_voice; }

void radio_backend_set_step_size(radio *radio_h, uint32_t s)
{ backend_call.step_size = s; radio_h->step_size = s; }

void radio_backend_set_tone_generation(radio *radio_h, bool t)
{ backend_call.tone_generation = t; radio_h->tone_generation = t; }

void radio_backend_set_profile(radio *radio_h, uint32_t p)
{ backend_call.profile = p; backend_call.set_profile_called = true;
  radio_h->profile_active_idx = p; radio_pipeline_refresh(radio_h); }

uint32_t radio_backend_get_fwd_power(radio *radio_h) { return radio_h->fwd_power; }
uint32_t radio_backend_get_swr(radio *radio_h)       { return radio_h->ref_power; }

static bool timeout_reset_called;
void radio_backend_reset_timeout_timer(void) { timeout_reset_called = true; }

bool shm_is_created(key_t k, size_t s) { (void) k; (void) s; return false; }
bool shm_create(key_t k, size_t s)     { (void) k; (void) s; return true; }
bool shm_destroy(key_t k, size_t s)    { (void) k; (void) s; return true; }
void *shm_attach(key_t k, size_t s)
{ static controller_conn connector; (void) k; (void) s; return &connector; }

#include "radio/radio_pipeline.c"
#include "control_interfaces/radio_shm.c"

static void reset_backend_call(void)
{
    memset(&backend_call, 0, sizeof(backend_call));
    timeout_reset_called = false;
}

static void init_test_radio(radio *r, radio_backend_kind backend_kind, bool dv)
{
    memset(r, 0, sizeof(*r));
    pthread_mutex_init(&r->message_mutex, NULL);
    r->profiles_count = 2;
    r->profile_active_idx = 0;
    r->profiles[0].freq = 7100000;
    r->profiles[1].freq = 7200000;
    r->profiles[0].mode = MODE_USB;
    r->profiles[1].mode = MODE_USB;
    r->profiles[0].digital_voice = false;
    r->profiles[1].digital_voice = dv;
    r->backend_kind = backend_kind;
    radio_pipeline_refresh(r);
}

static void destroy_test_radio(radio *r) { pthread_mutex_destroy(&r->message_mutex); }

static void test_shm_profile_and_message_semantics(void)
{
    radio radio_h;
    controller_conn connector;
    uint8_t cmd[5] = {0};
    uint8_t response[5] = {0};
    uint32_t value = 0;

    init_test_radio(&radio_h, RADIO_BACKEND_HAMLIB, false);
    memset(&connector, 0, sizeof(connector));
    connector_local = &connector;
    radio_h_shm = &radio_h;

    cmd[4] = CMD_GET_FREQ | (1u << 6);
    process_radio_command(cmd, response);
    memcpy(&value, response + 1, sizeof(value));
    TEST_ASSERT_EQUAL_INT64(CMD_RESP_GET_FREQ_ACK, response[0]);
    TEST_ASSERT_EQUAL_INT64(7200000, value);

    memset(cmd, 0, sizeof(cmd));
    value = 7215000;
    memcpy(cmd, &value, sizeof(value));
    cmd[4] = CMD_SET_FREQ | (1u << 6);
    process_radio_command(cmd, response);
    TEST_ASSERT_EQUAL_INT64(CMD_RESP_ACK, response[0]);
    TEST_ASSERT_EQUAL_INT64(7215000, backend_call.frequency);
    TEST_ASSERT_EQUAL_INT64(1, backend_call.profile);
    TEST_ASSERT_EQUAL_INT64(7215000, radio_h.profiles[1].freq);

    /* SET_MODE wire encoding (see radio_cmds.h):
     *   0x00 LSB  0x01 USB  0x02 FM  0x03 AM
     *   0x04 CW   0x05 DRM  0x06 FT8 0x07 RTTY
     */
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x00;
    cmd[4] = CMD_SET_MODE | (1u << 6);
    process_radio_command(cmd, response);
    TEST_ASSERT_EQUAL_INT64(CMD_RESP_ACK, response[0]);
    TEST_ASSERT_EQUAL_INT64(MODE_LSB, backend_call.mode);
    TEST_ASSERT_EQUAL_INT64(MODE_LSB, radio_h.profiles[1].mode);

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x02;
    cmd[4] = CMD_SET_MODE | (1u << 6);
    process_radio_command(cmd, response);
    TEST_ASSERT_EQUAL_INT64(CMD_RESP_ACK, response[0]);
    TEST_ASSERT_EQUAL_INT64(MODE_FM, backend_call.mode);
    TEST_ASSERT_EQUAL_INT64(MODE_FM, radio_h.profiles[1].mode);

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x07;
    cmd[4] = CMD_SET_MODE | (1u << 6);
    process_radio_command(cmd, response);
    TEST_ASSERT_EQUAL_INT64(CMD_RESP_ACK, response[0]);
    TEST_ASSERT_EQUAL_INT64(MODE_RTTY, backend_call.mode);

    memset(cmd, 0, sizeof(cmd));
    cmd[4] = CMD_GET_MODE | (1u << 6);
    process_radio_command(cmd, response);
    TEST_ASSERT_EQUAL_INT64(CMD_RESP_GET_MODE_RTTY, response[0]);

    /* Unknown mode index NACKs without changing the profile. */
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x42;
    cmd[4] = CMD_SET_MODE | (1u << 6);
    process_radio_command(cmd, response);
    TEST_ASSERT_EQUAL_INT64(CMD_RESP_WRONG_COMMAND, response[0]);
    TEST_ASSERT_EQUAL_INT64(MODE_RTTY, radio_h.profiles[1].mode);

    /* SET_CONNECTED_STATUS=false clears both buffer slots. */
    pthread_mutex_lock(&radio_h.message_mutex);
    snprintf(radio_h.message, sizeof(radio_h.message), "%s", "stale");
    pthread_mutex_unlock(&radio_h.message_mutex);
    snprintf(connector.message, sizeof(connector.message), "%s", "stale");
    radio_h.system_is_connected = true;
    radio_h.message_available = false;
    connector.message_available = false;

    memset(cmd, 0, sizeof(cmd));
    cmd[4] = CMD_SET_CONNECTED_STATUS;
    process_radio_command(cmd, response);
    TEST_ASSERT_EQUAL_INT64(CMD_RESP_ACK, response[0]);
    TEST_ASSERT_FALSE(radio_h.system_is_connected);
    TEST_ASSERT_TRUE(radio_h.message_available);
    TEST_ASSERT_TRUE(connector.message_available);
    TEST_ASSERT_EQUAL_INT64('\0', radio_h.message[0]);
    TEST_ASSERT_EQUAL_INT64('\0', connector.message[0]);

    /* GET_DIGITAL_VOICE for an out-of-range profile NACKs. */
    memset(cmd, 0, sizeof(cmd));
    cmd[4] = CMD_GET_DIGITAL_VOICE | (3u << 6);
    process_radio_command(cmd, response);
    TEST_ASSERT_EQUAL_INT64(CMD_RESP_WRONG_COMMAND, response[0]);

    destroy_test_radio(&radio_h);
}

/* CMD_PTT_OFF must reach the backend even when the cached state already
 * reads RX (the radio may still be keyed: the estacao8 incident) and while
 * SWR protection is on. The replies keep their old meaning. */
static void test_shm_ptt_off_always_unkeys(void)
{
    radio radio_h;
    controller_conn connector;
    uint8_t cmd[5] = {0};
    uint8_t response[5] = {0};

    init_test_radio(&radio_h, RADIO_BACKEND_HAMLIB, false);
    memset(&connector, 0, sizeof(connector));
    connector_local = &connector;
    radio_h_shm = &radio_h;

    reset_backend_call();
    radio_h.txrx_state = IN_RX;
    cmd[4] = CMD_PTT_OFF;
    process_radio_command(cmd, response);
    do { TEST_ASSERT_EQUAL_INT64(1, backend_call.txrx_calls); TEST_ASSERT_EQUAL_INT64(IN_RX, backend_call.txrx_state); } while (0);
    TEST_ASSERT_EQUAL_INT64(CMD_RESP_PTT_OFF_NACK, response[0]);

    reset_backend_call();
    radio_h.txrx_state = IN_TX;
    radio_h.swr_protection_enabled = true;
    process_radio_command(cmd, response);
    do { TEST_ASSERT_EQUAL_INT64(1, backend_call.txrx_calls); TEST_ASSERT_EQUAL_INT64(IN_RX, radio_h.txrx_state); } while (0);
    TEST_ASSERT_EQUAL_INT64(CMD_ALERT_PROTECTION_ON, response[0]);
    radio_h.swr_protection_enabled = false;

    reset_backend_call();
    radio_h.txrx_state = IN_TX;
    process_radio_command(cmd, response);
    do { TEST_ASSERT_EQUAL_INT64(1, backend_call.txrx_calls); TEST_ASSERT_EQUAL_INT64(IN_RX, radio_h.txrx_state); } while (0);
    TEST_ASSERT_EQUAL_INT64(CMD_RESP_ACK, response[0]);

    destroy_test_radio(&radio_h);
    puts("  shm PTT off always unkeys ............ ok");
}

/* A client that keys over hermes_shm and dies without unkeying (killed
 * mid-transmission) must not leave the radio in TX; one that is alive, or
 * that unkeyed before exiting, must not be touched. The child plays
 * radio_cmd(): it holds response_mutex while its command is processed. */
static int keyer_fork_named(controller_conn *conn, int *ctl_w, const char *name)
{
    int up[2], down[2];
    do { TEST_ASSERT_EQUAL_INT64(0, pipe(up)); TEST_ASSERT_EQUAL_INT64(0, pipe(down)); } while (0);
    pid_t pid = fork();
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(0, pid);
    if (pid == 0)
    {
        char c;
        close(up[0]);
        close(down[1]);
        if (name)
            prctl(PR_SET_NAME, name, 0, 0, 0);
        pthread_mutex_lock(&conn->response_mutex);
        if (write(up[1], "L", 1) != 1)                  /* no Unity asserts in the child */
            _exit(1);
        if (read(down[0], &c, 1) == 1 && c == 'U')      /* reply read */
            pthread_mutex_unlock(&conn->response_mutex);
        (void) read(down[0], &c, 1);                    /* wait to die */
        _exit(0);
    }
    char c;
    do { TEST_ASSERT_EQUAL_INT64(1, read(up[0], &c, 1)); TEST_ASSERT_EQUAL_INT64('L', c); } while (0);
    close(up[0]); close(up[1]); close(down[0]);
    *ctl_w = down[1];
    return pid;
}

static int keyer_fork(controller_conn *conn, int *ctl_w)
{
    return keyer_fork_named(conn, ctl_w, NULL);
}

static void keyer_reap(pid_t pid, int ctl_w)
{
    close(ctl_w);                       /* EOF: the child exits */
    TEST_ASSERT_EQUAL_INT(pid, waitpid(pid, NULL, 0));
}

static void test_shm_keyer_death_releases_ptt(void)
{
    radio radio_h;
    uint8_t on[5] = {0, 0, 0, 0, CMD_PTT_ON};
    uint8_t off[5] = {0, 0, 0, 0, CMD_PTT_OFF};
    uint8_t response[5] = {0};

    controller_conn *conn = mmap(NULL, sizeof(*conn), PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT_TRUE(conn != MAP_FAILED);
    memset(conn, 0, sizeof(*conn));
    TEST_ASSERT_TRUE(initialize_connector(conn));

    init_test_radio(&radio_h, RADIO_BACKEND_HAMLIB, false);
    connector_local = conn;
    radio_h_shm = &radio_h;

    /* Keys, then dies holding PTT: released on the next loop pass. */
    reset_backend_call();
    int ctl;
    pid_t pid = keyer_fork(conn, &ctl);
    process_radio_command(on, response);
    do { TEST_ASSERT_EQUAL_INT64(CMD_RESP_ACK, response[0]); TEST_ASSERT_EQUAL_INT64(IN_TX, radio_h.txrx_state); } while (0);
    do { TEST_ASSERT_EQUAL_INT64(PTT_SRC_SHM, ptt_owner.src); TEST_ASSERT_EQUAL_INT64(pid, ptt_owner.id); } while (0);
    TEST_ASSERT_EQUAL_INT64(1, write(ctl, "U", 1));

    shm_check_keyer(&radio_h);          /* alive: leave it keyed */
    do { TEST_ASSERT_EQUAL_INT64(IN_TX, radio_h.txrx_state); TEST_ASSERT_EQUAL_INT64(0, ptt_owner.releases); } while (0);

    keyer_reap(pid, ctl);
    shm_check_keyer(&radio_h);
    do { TEST_ASSERT_EQUAL_INT64(IN_RX, radio_h.txrx_state); TEST_ASSERT_EQUAL_INT64(1, ptt_owner.releases); } while (0);
    shm_check_keyer(&radio_h);          /* once only */
    TEST_ASSERT_EQUAL_INT64(1, ptt_owner.releases);

    /* Keys, unkeys, then exits: nothing to release. */
    pid = keyer_fork(conn, &ctl);
    process_radio_command(on, response);
    TEST_ASSERT_EQUAL_INT64(1, write(ctl, "U", 1));
    process_radio_command(off, response);
    TEST_ASSERT_EQUAL_INT64(IN_RX, radio_h.txrx_state);
    int calls = backend_call.txrx_calls;
    keyer_reap(pid, ctl);
    shm_check_keyer(&radio_h);
    do { TEST_ASSERT_EQUAL_INT(calls, backend_call.txrx_calls); TEST_ASSERT_EQUAL_INT64(1, ptt_owner.releases); } while (0);

    /* Keys; another source takes PTT over; the first then dies: the
     * other source's transmission must go on. */
    pid = keyer_fork(conn, &ctl);
    process_radio_command(on, response);
    TEST_ASSERT_EQUAL_INT64(1, write(ctl, "U", 1));
    ptt_owner.src = PTT_SRC_WEBSOCKET;
    ptt_owner.id = 7;
    keyer_reap(pid, ctl);
    shm_check_keyer(&radio_h);
    do { TEST_ASSERT_EQUAL_INT64(IN_TX, radio_h.txrx_state); TEST_ASSERT_EQUAL_INT64(1, ptt_owner.releases); } while (0);

    /* The command-line client keys and exits by design (the web panel's
     * PTT button): its PTT stays latched until a PTT-off. */
    process_radio_command(off, response);
    ptt_owner.releases = 0;
    pid = keyer_fork_named(conn, &ctl, "sbitx_client");
    process_radio_command(on, response);
    do { TEST_ASSERT_EQUAL_INT64(CMD_RESP_ACK, response[0]); TEST_ASSERT_EQUAL_INT64(IN_TX, radio_h.txrx_state); } while (0);
    TEST_ASSERT_EQUAL_INT64(1, write(ctl, "U", 1));
    keyer_reap(pid, ctl);
    shm_check_keyer(&radio_h);
    do { TEST_ASSERT_EQUAL_INT64_MESSAGE(IN_TX, radio_h.txrx_state, "the web panel's PTT was released when sbitx_client exited"); TEST_ASSERT_EQUAL_INT64_MESSAGE(0, ptt_owner.releases, "the web panel's PTT was released when sbitx_client exited"); } while (0);
    process_radio_command(off, response);
    TEST_ASSERT_EQUAL_INT64(IN_RX, radio_h.txrx_state);

    shm_keyer_forget();
    destroy_test_radio(&radio_h);
    munmap(conn, sizeof(*conn));
    puts("  shm keyer death releases PTT ......... ok");
}

void setUp(void) { reset_backend_call(); }
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_shm_profile_and_message_semantics);
    RUN_TEST(test_shm_ptt_off_always_unkeys);
    RUN_TEST(test_shm_keyer_death_releases_ptt);
    return UNITY_END();
}
