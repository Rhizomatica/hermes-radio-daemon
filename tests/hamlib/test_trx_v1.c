/*
 * The HERMES trx_v1 on the hamlib backend, without libhamlib.
 *
 * A fake trx_v1 firmware answers on a pty as hermes-net's trx_v1-firmware
 * (ubitx_cat.cpp) does: 5-byte commands, a little-endian argument then the
 * command code, and replies of one byte or five. Against it:
 *
 *   - the driver: frequency, sideband, PTT (keying twice and unkeying in
 *     receive are not errors), the detectors (a 16-bit reading in a 4-byte
 *     reply), SWR from them, the protection latch and its reset, and a
 *     command left unanswered, after which the next one still works;
 *   - the backend with main:radio_model = trx_v1: start-up connects, unkeys
 *     and applies the profile; frequency, mode (a PKTUSB profile reads
 *     PKTUSB back from a radio that has only USB and LSB), PTT, metering;
 *     the firmware's protection trip reaches radiod's flag, a reset clears
 *     both, and a refused key sets the flag; shutdown and radio_daemon
 *     --ptt-off unkey;
 *   - Hamlib's own NET rigctl client (rigctl -m 2, WSJT-X) opens the radio
 *     through radiod's rigctld server, which describes it (dump_state),
 *     and tunes and keys it.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <hamlib/rig.h>

#include "unity.h"
#include "test_util.h"
#include "radio/radio.h"
#include "radio/radio_backend.h"

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

/* Short waits: the fake has no bootloader and answers at once. */
#define TRXV1_BOOT_MS      20
#define TRXV1_BOOT_MAX_MS  1000
#define TRXV1_PROBE_GAP_MS 20
#define TRXV1_REPLY_MS     100
#define TRXV1_RESYNC_MS    20

#include "common/cfg_utils.c"
#undef LOG_C
#include "radio/radio_backend.c"
#include "radio/radio_controls.c"
#undef LOG_C
#include "hamlib/trx_v1.c"
#undef LOG_C
#include "hamlib/radio_hamlib.c"

/* ── the fake firmware ─────────────────────────────────────────── */

static struct {
    pthread_mutex_t lock;
    uint32_t freq;
    bool usb, in_tx, protect;
    bool v22;                       /* speak the 2022 firmware's codes */
    uint16_t fwd, ref;
    int ignore;                     /* commands to leave unanswered */
    int ptt_on, ptt_off;            /* PTT commands received */
    int resets;                     /* protection resets received */
} fw = { .lock = PTHREAD_MUTEX_INITIALIZER, .freq = 7100000, .usb = true };

static int master_fd = -1;
static char slave_path[64];
static int slave_keep = -1;         /* keeps the master from hanging up */
static pthread_t fw_thread;

static void put32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t) v; b[1] = (uint8_t) (v >> 8);
    b[2] = (uint8_t) (v >> 16); b[3] = (uint8_t) (v >> 24);
}

/* ubitx_cat.cpp's processCATCommand, as far as radiod uses it. */
static size_t fw_reply(const uint8_t *cmd, uint8_t *r)
{
    uint32_t arg = (uint32_t) cmd[0] | (uint32_t) cmd[1] << 8 |
                   (uint32_t) cmd[2] << 16 | (uint32_t) cmd[3] << 24;

    switch (cmd[4])
    {
    case CMD_PTT_ON:
        fw.ptt_on++;
        if (fw.protect)      r[0] = CMD_ALERT_PROTECTION_ON;
        else if (!fw.in_tx) { r[0] = CMD_RESP_ACK; fw.in_tx = true; }
        else                 r[0] = CMD_RESP_PTT_ON_NACK;
        return 1;
    case CMD_PTT_OFF:
        fw.ptt_off++;
        if (fw.protect)      r[0] = CMD_ALERT_PROTECTION_ON;
        else if (fw.in_tx) { r[0] = CMD_RESP_ACK; fw.in_tx = false; }
        else                 r[0] = CMD_RESP_PTT_OFF_NACK;
        return 1;
    case CMD_RESET_PROTECTION:
        fw.resets++;
        fw.protect = false;
        r[0] = CMD_RESP_ACK;
        return 1;
    case CMD_GET_FREQ:
        r[0] = CMD_RESP_GET_FREQ_ACK; put32(r + 1, fw.freq);
        return 5;
    case CMD_SET_FREQ:
        fw.freq = arg; r[0] = CMD_RESP_ACK;
        return 1;
    case CMD_SET_MODE:
        fw.usb = !(cmd[0] == 0x00 || cmd[0] == 0x03); r[0] = CMD_RESP_ACK;
        return 1;
    case CMD_GET_MODE:
        r[0] = fw.usb ? CMD_RESP_GET_MODE_USB : CMD_RESP_GET_MODE_LSB;
        return 1;
    case CMD_GET_TXRX_STATUS:
        r[0] = fw.in_tx ? CMD_RESP_GET_TXRX_INTX : CMD_RESP_GET_TXRX_INRX;
        return 1;
    case CMD_GET_PROTECTION_STATUS:
        r[0] = fw.protect ? CMD_RESP_GET_PROTECTION_ON : CMD_RESP_GET_PROTECTION_OFF;
        return 1;
    case CMD_GET_FWD:
        /* memcpy(response+1, &forward, 2): the last two bytes are whatever
         * the firmware's stack held */
        r[0] = CMD_RESP_GET_FWD_ACK; put32(r + 1, 0xA5A50000u | fw.fwd);
        return 5;
    case CMD_GET_REF:
        r[0] = CMD_RESP_GET_REF_ACK; put32(r + 1, 0x5A5A0000u | fw.ref);
        return 5;
    default:
        r[0] = CMD_RESP_WRONG_COMMAND;
        return 1;
    }
}

/* The same, as the 2022 firmware the radios in the field run: its own
 * codes, one acknowledgement per command. */
static size_t fw_reply_v22(const uint8_t *cmd, uint8_t *r)
{
    uint32_t arg = (uint32_t) cmd[0] | (uint32_t) cmd[1] << 8 |
                   (uint32_t) cmd[2] << 16 | (uint32_t) cmd[3] << 24;

    switch (cmd[4])
    {
    case V22_PTT_ON:
        fw.ptt_on++;
        if (fw.protect)      r[0] = V22_ALERT_PROTECTION_ON;
        else if (!fw.in_tx) { r[0] = V22_RESP_PTT_ON_ACK; fw.in_tx = true; }
        else                 r[0] = V22_RESP_PTT_ON_NACK;
        return 1;
    case V22_PTT_OFF:
        fw.ptt_off++;
        if (fw.protect)      r[0] = V22_ALERT_PROTECTION_ON;
        else if (fw.in_tx) { r[0] = V22_RESP_PTT_OFF_ACK; fw.in_tx = false; }
        else                 r[0] = V22_RESP_PTT_OFF_NACK;
        return 1;
    case V22_RESET_PROTECTION:
        fw.resets++;
        fw.protect = false;
        r[0] = V22_RESP_RESET_PROTECTION_ACK;
        return 1;
    case V22_GET_FREQ:
        r[0] = V22_RESP_GET_FREQ_ACK; put32(r + 1, fw.freq);
        return 5;
    case V22_SET_FREQ:
        fw.freq = arg; r[0] = V22_RESP_SET_FREQ_ACK;
        return 1;
    case V22_SET_MODE:
        fw.usb = !(cmd[0] == 0x00 || cmd[0] == 0x03); r[0] = V22_RESP_SET_MODE_ACK;
        return 1;
    case V22_GET_MODE:
        r[0] = fw.usb ? V22_RESP_GET_MODE_USB : V22_RESP_GET_MODE_LSB;
        return 1;
    case V22_GET_TXRX_STATUS:
        r[0] = fw.in_tx ? V22_RESP_GET_TXRX_INTX : V22_RESP_GET_TXRX_INRX;
        return 1;
    case V22_GET_PROTECTION_STATUS:
        r[0] = fw.protect ? V22_RESP_GET_PROTECTION_ON : V22_RESP_GET_PROTECTION_OFF;
        return 1;
    case V22_GET_FWD:       /* as seen on a radio: the padding is the frequency's */
        r[0] = V22_RESP_GET_FWD_ACK; put32(r + 1, (fw.freq & 0xFFFF0000u) | fw.fwd);
        return 5;
    case V22_GET_REF:
        r[0] = V22_RESP_GET_REF_ACK; put32(r + 1, (fw.freq & 0xFFFF0000u) | fw.ref);
        return 5;
    default:
        r[0] = V22_RESP_WRONG_COMMAND;
        return 1;
    }
}

static void *fw_run(void *arg)
{
    (void) arg;
    uint8_t cmd[5];
    size_t have = 0;

    for (;;)
    {
        struct pollfd pfd = { .fd = master_fd, .events = POLLIN };
        if (poll(&pfd, 1, 50) <= 0)
            continue;
        ssize_t n = read(master_fd, cmd + have, sizeof(cmd) - have);
        if (n <= 0)
        {
            if (n < 0 && errno == EBADF)
                return NULL;
            usleep(1000);
            continue;
        }
        have += (size_t) n;
        if (have < sizeof(cmd))
            continue;
        have = 0;

        uint8_t r[5];
        pthread_mutex_lock(&fw.lock);
        bool drop = fw.ignore > 0;
        if (drop)
            fw.ignore--;
        size_t len = drop ? 0 : fw.v22 ? fw_reply_v22(cmd, r) : fw_reply(cmd, r);
        pthread_mutex_unlock(&fw.lock);
        if (len && write(master_fd, r, len) != (ssize_t) len)
            return NULL;
    }
}

static void start_firmware(void)
{
    master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    TEST_REQUIRE(master_fd >= 0);
    TEST_REQUIRE(grantpt(master_fd) == 0 && unlockpt(master_fd) == 0);
    snprintf(slave_path, sizeof(slave_path), "%s", ptsname(master_fd));
    slave_keep = open(slave_path, O_RDWR | O_NOCTTY);
    TEST_REQUIRE(slave_keep >= 0);
    TEST_REQUIRE(pthread_create(&fw_thread, NULL, fw_run, NULL) == 0);
}

#define FW(stmt) do { pthread_mutex_lock(&fw.lock); stmt; pthread_mutex_unlock(&fw.lock); } while (0)
/* One field of the fake's state, read under its lock. */
#define FWV(field) __extension__ ({ pthread_mutex_lock(&fw.lock); \
                                     __typeof__(fw.field) v_ = fw.field; \
                                     pthread_mutex_unlock(&fw.lock); v_; })

static void fw_reset_state(void)
{
    FW(fw.freq = 7100000; fw.usb = true; fw.in_tx = false; fw.protect = false; fw.v22 = false;
       fw.fwd = 0; fw.ref = 0; fw.ignore = 0; fw.ptt_on = 0; fw.ptt_off = 0;
       fw.resets = 0);
}

/* ── the driver ────────────────────────────────────────────────── */

static trxv1 *drv;

void setUp(void) { fw_reset_state(); }
void tearDown(void) {}

static void test_driver_connects(void)
{
    drv = trxv1_new(slave_path);
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_connect(drv));
}

static void test_driver_frequency_and_sideband(void)
{
    uint32_t hz = 0;
    bool usb = true;

    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_set_freq(drv, 6980000));
    TEST_ASSERT_EQUAL_UINT32(6980000, FWV(freq));
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_get_freq(drv, &hz));
    TEST_ASSERT_EQUAL_UINT32(6980000, hz);

    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_set_usb(drv, false));
    TEST_ASSERT_FALSE(FWV(usb));
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_get_usb(drv, &usb));
    TEST_ASSERT_FALSE(usb);
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_set_usb(drv, true));
    TEST_ASSERT_TRUE(FWV(usb));
}

static void test_driver_ptt(void)
{
    bool on = false;

    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_set_ptt(drv, true));
    TEST_ASSERT_TRUE(FWV(in_tx));
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_set_ptt(drv, true));   /* NACK: keyed already */
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_get_ptt(drv, &on));
    TEST_ASSERT_TRUE(on);
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_set_ptt(drv, false));
    TEST_ASSERT_FALSE(FWV(in_tx));
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_set_ptt(drv, false));  /* NACK: in receive */
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_get_ptt(drv, &on));
    TEST_ASSERT_FALSE(on);
}

static void test_driver_detectors_and_swr(void)
{
    uint16_t f = 0, r = 0;

    FW(fw.fwd = 600; fw.ref = 200);
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_get_fwd(drv, &f));
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_get_ref(drv, &r));
    TEST_ASSERT_EQUAL_UINT16(600, f);     /* the padding bytes are not part of it */
    TEST_ASSERT_EQUAL_UINT16(200, r);

    TEST_ASSERT_EQUAL_UINT32(20, trxv1_swr_x10(600, 200));   /* 800 / 400 */
    TEST_ASSERT_EQUAL_UINT32(10, trxv1_swr_x10(500, 0));
    TEST_ASSERT_EQUAL_UINT32(10, trxv1_swr_x10(0, 0));
    TEST_ASSERT_EQUAL_UINT32(TRXV1_SWR_MAX_X10, trxv1_swr_x10(300, 300));
    TEST_ASSERT_EQUAL_UINT32(TRXV1_SWR_MAX_X10, trxv1_swr_x10(1000, 990));
}

static void test_driver_protection(void)
{
    bool latched = false;

    FW(fw.protect = true);
    TEST_ASSERT_EQUAL_INT(TRXV1_EPROTECT, trxv1_set_ptt(drv, true));
    TEST_ASSERT_FALSE(FWV(in_tx));
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_set_ptt(drv, false));  /* the trip unkeyed it */
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_get_protection(drv, &latched));
    TEST_ASSERT_TRUE(latched);
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_reset_protection(drv));
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_get_protection(drv, &latched));
    TEST_ASSERT_FALSE(latched);
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_set_ptt(drv, true));
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_set_ptt(drv, false));
}

/* The 2022 firmware (Daniel's radio answered every current code with its
 * WRONG_COMMAND, 0xc0): told apart when connecting, then driven in its own
 * codes. */
static void test_driver_2022_firmware(void)
{
    uint32_t hz = 0;
    bool usb = true, on = true, latched = false;
    uint16_t f = 0, r = 0;

    FW(fw.v22 = true; fw.freq = 7094003; fw.usb = false);
    trxv1 *old = trxv1_new(slave_path);
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_connect(old));
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_get_freq(old, &hz));
    TEST_ASSERT_EQUAL_UINT32(7094003, hz);
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_get_usb(old, &usb));
    TEST_ASSERT_FALSE(usb);

    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_set_freq(old, 6985000));
    TEST_ASSERT_EQUAL_UINT32(6985000, FWV(freq));
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_set_usb(old, true));
    TEST_ASSERT_TRUE(FWV(usb));

    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_set_ptt(old, true));
    TEST_ASSERT_TRUE(FWV(in_tx));
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_set_ptt(old, true));   /* NACK: keyed already */
    FW(fw.fwd = 600; fw.ref = 200);
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_get_fwd(old, &f));
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_get_ref(old, &r));
    TEST_ASSERT_EQUAL_UINT16(600, f);
    TEST_ASSERT_EQUAL_UINT16(200, r);
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_set_ptt(old, false));
    TEST_ASSERT_FALSE(FWV(in_tx));
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_get_ptt(old, &on));
    TEST_ASSERT_FALSE(on);

    FW(fw.protect = true);
    TEST_ASSERT_EQUAL_INT(TRXV1_EPROTECT, trxv1_set_ptt(old, true));
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_get_protection(old, &latched));
    TEST_ASSERT_TRUE(latched);
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_reset_protection(old));
    TEST_ASSERT_EQUAL_INT(1, FWV(resets));
    TEST_ASSERT_FALSE(FWV(protect));

    trxv1_free(old);
}

static void test_driver_survives_a_lost_reply(void)
{
    uint32_t hz = 0;

    FW(fw.ignore = 1);
    TEST_ASSERT_EQUAL_INT(TRXV1_ETIMEOUT, trxv1_get_freq(drv, &hz));
    TEST_ASSERT_EQUAL_INT(TRXV1_OK, trxv1_get_freq(drv, &hz));
    TEST_ASSERT_EQUAL_UINT32(7100000, hz);

    trxv1_free(drv);
    drv = NULL;
}

/* ── the backend ───────────────────────────────────────────────── */

static radio r;

#define TEST_RIGCTLD_PORT 45341

static void init_radio(void)
{
    memset(&r, 0, sizeof(r));
    r.backend_kind = RADIO_BACKEND_HAMLIB;
    r.backend_ops = &hamlib_backend_ops;
    r.hamlib_model = RADIO_MODEL_TRXV1;
    snprintf(r.rig_pathname, sizeof(r.rig_pathname), "%s", slave_path);
    r.ptt_type = PTT_RIG;
    r.reflected_threshold = 0;      /* radiod's own SWR check: not this test's */
    r.profiles_count = 1;
    r.profiles[0].freq = 6980000;
    r.profiles[0].mode = MODE_LSB;
    r.profiles[0].operating_mode = OPERATING_MODE_FULL_VOICE;
    r.rig_server_enable = true;
    r.rig_server_port = TEST_RIGCTLD_PORT;
    pthread_mutex_init(&r.cfg_mutex, NULL);
}

static void test_backend_start(void)
{
    init_radio();
    FW(fw.in_tx = true);            /* left keyed */

    TEST_ASSERT_TRUE(radio_backend_init(&r));
    TEST_ASSERT_NOT_NULL(r.trxv1);
    TEST_ASSERT_NULL(r.rig);
    TEST_ASSERT_FALSE(FWV(in_tx));
    TEST_ASSERT_EQUAL_UINT32(6980000, FWV(freq));
    TEST_ASSERT_FALSE(FWV(usb));
    TEST_ASSERT_EQUAL(IN_RX, r.txrx_state);
}

static void test_backend_frequency_and_mode(void)
{
    char name[16];
    uint32_t width = 1;

    radio_backend_set_frequency(&r, 7050000, 0);
    TEST_ASSERT_EQUAL_UINT32(7050000, FWV(freq));

    radio_backend_set_mode(&r, MODE_USB, 0);
    TEST_ASSERT_TRUE(FWV(usb));
    TEST_ASSERT_EQUAL_INT(RADIO_CTRL_OK, radio_backend_get_mode_name(&r, name, sizeof(name), &width));
    TEST_ASSERT_EQUAL_STRING("USB", name);

    /* a data profile asks for PKTUSB: the radio goes to USB and PKTUSB
     * reads back, so the band check does not set it again */
    TEST_ASSERT_EQUAL_INT(RADIO_CTRL_OK, radio_backend_set_mode_name(&r, "PKTUSB", 0));
    TEST_ASSERT_EQUAL_INT(RADIO_CTRL_OK, radio_backend_get_mode_name(&r, name, sizeof(name), &width));
    TEST_ASSERT_EQUAL_STRING("PKTUSB", name);
    TEST_ASSERT_EQUAL_UINT32(0, width);

    /* someone moved the radio to LSB: that is what reads back */
    FW(fw.usb = false);
    TEST_ASSERT_EQUAL_INT(RADIO_CTRL_OK, radio_backend_get_mode_name(&r, name, sizeof(name), &width));
    TEST_ASSERT_EQUAL_STRING("LSB", name);
    radio_backend_set_mode(&r, MODE_LSB, 0);
}

static void test_backend_ptt_and_meters(void)
{
    radio_backend_set_txrx_state(&r, IN_TX);
    TEST_ASSERT_TRUE(FWV(in_tx));
    TEST_ASSERT_EQUAL(IN_TX, r.txrx_state);

    FW(fw.fwd = 1023; fw.ref = 341);
    TEST_ASSERT_EQUAL_UINT32(1000, radio_backend_get_fwd_power(&r));   /* full scale */
    TEST_ASSERT_EQUAL_UINT32(20, radio_backend_get_swr(&r));           /* 1364 / 682 */

    radio_backend_set_txrx_state(&r, IN_RX);
    TEST_ASSERT_FALSE(FWV(in_tx));
    TEST_ASSERT_EQUAL(IN_RX, r.txrx_state);
}

static void test_backend_firmware_trip_and_reset(void)
{
    radio_backend_set_txrx_state(&r, IN_TX);
    TEST_ASSERT_EQUAL(IN_TX, r.txrx_state);

    /* the firmware's checkREF trips: it unkeys and latches */
    FW(fw.protect = true; fw.in_tx = false);
    hamlib_sync_txrx_state(&r, IN_TX);          /* the io thread's PTT poll */
    TEST_ASSERT_EQUAL(IN_RX, r.txrx_state);
    TEST_ASSERT_TRUE(r.swr_protection_enabled);

    int keyed = FWV(ptt_on);
    radio_backend_set_txrx_state(&r, IN_TX);    /* radiod refuses, the radio is not asked */
    TEST_ASSERT_EQUAL_INT(keyed, FWV(ptt_on));
    TEST_ASSERT_FALSE(FWV(in_tx));

    radio_backend_reset_protection(&r);
    TEST_ASSERT_FALSE(r.swr_protection_enabled);
    TEST_ASSERT_EQUAL_INT(1, FWV(resets));
    TEST_ASSERT_FALSE(FWV(protect));

    radio_backend_set_txrx_state(&r, IN_TX);
    TEST_ASSERT_TRUE(FWV(in_tx));
    radio_backend_set_txrx_state(&r, IN_RX);
    TEST_ASSERT_FALSE(FWV(in_tx));
}

static void test_backend_refused_key_sets_the_flag(void)
{
    /* latched between two polls: the key itself is refused */
    FW(fw.protect = true);
    radio_backend_set_txrx_state(&r, IN_TX);
    TEST_ASSERT_FALSE(FWV(in_tx));
    TEST_ASSERT_EQUAL(IN_RX, r.txrx_state);
    TEST_ASSERT_TRUE(r.swr_protection_enabled);
    radio_backend_reset_protection(&r);
    TEST_ASSERT_FALSE(FWV(protect));
}

static void test_rigctld_client(void)
{
    char addr[32];
    freq_t f = 0;
    rmode_t m = RIG_MODE_NONE;
    pbwidth_t w = 0;
    ptt_t ptt = RIG_PTT_ON;

    radio_backend_set_mode(&r, MODE_USB, r.profile_active_idx);
    TEST_ASSERT_TRUE(FWV(usb));

    RIG *net = rig_init(RIG_MODEL_NETRIGCTL);
    TEST_ASSERT_NOT_NULL(net);
    snprintf(addr, sizeof(addr), "127.0.0.1:%d", TEST_RIGCTLD_PORT);
    TEST_ASSERT_EQUAL_INT(RIG_OK, rig_set_conf(net, rig_token_lookup(net, "rig_pathname"), addr));
    TEST_ASSERT_EQUAL_INT(RIG_OK, rig_open(net));

    TEST_ASSERT_EQUAL_INT(RIG_OK, rig_set_freq(net, RIG_VFO_CURR, 7095000));
    TEST_ASSERT_EQUAL_UINT32(7095000, FWV(freq));
    TEST_ASSERT_EQUAL_INT(RIG_OK, rig_get_freq(net, RIG_VFO_CURR, &f));
    TEST_ASSERT_EQUAL_UINT32(7095000, (uint32_t) f);

    /* Every set is a change: Hamlib's client does not send a mode it holds
     * as already set, so a set equal to its cached mode never reached the
     * radio, which then kept the sideband an earlier test left (seen on a
     * slower CI runner). The profile starts at USB, then LSB, then USB. */
    TEST_ASSERT_EQUAL_INT(RIG_OK, rig_set_mode(net, RIG_VFO_CURR, RIG_MODE_LSB, RIG_PASSBAND_NOCHANGE));
    TEST_ASSERT_FALSE(FWV(usb));
    TEST_ASSERT_EQUAL_INT(RIG_OK, rig_get_mode(net, RIG_VFO_CURR, &m, &w));
    TEST_ASSERT_EQUAL_INT((int) RIG_MODE_LSB, (int) m);
    TEST_ASSERT_EQUAL_INT(RIG_OK, rig_set_mode(net, RIG_VFO_CURR, RIG_MODE_USB, RIG_PASSBAND_NOCHANGE));
    TEST_ASSERT_TRUE(FWV(usb));
    TEST_ASSERT_EQUAL_INT(RIG_OK, rig_get_mode(net, RIG_VFO_CURR, &m, &w));
    TEST_ASSERT_EQUAL_INT((int) RIG_MODE_USB, (int) m);

    TEST_ASSERT_EQUAL_INT(RIG_OK, rig_set_ptt(net, RIG_VFO_CURR, RIG_PTT_ON));
    TEST_ASSERT_TRUE(FWV(in_tx));
    TEST_ASSERT_EQUAL_INT(RIG_OK, rig_set_ptt(net, RIG_VFO_CURR, RIG_PTT_OFF));
    TEST_ASSERT_FALSE(FWV(in_tx));
    TEST_ASSERT_EQUAL_INT(RIG_OK, rig_get_ptt(net, RIG_VFO_CURR, &ptt));
    TEST_ASSERT_EQUAL_INT((int) RIG_PTT_OFF, (int) ptt);

    rig_close(net);
    rig_cleanup(net);
}

static void test_backend_shutdown_unkeys(void)
{
    radio_backend_set_txrx_state(&r, IN_TX);
    TEST_ASSERT_TRUE(FWV(in_tx));
    r.txrx_state = IN_RX;               /* a stale cache */
    radio_backend_shutdown(&r);
    TEST_ASSERT_FALSE(FWV(in_tx));
    TEST_ASSERT_NULL(r.trxv1);
}

static void test_force_ptt_off(void)
{
    init_radio();
    FW(fw.in_tx = true);
    TEST_ASSERT_TRUE(hamlib_backend_ops.force_ptt_off(&r));
    TEST_ASSERT_FALSE(FWV(in_tx));
}

static void test_config_names_the_model(void)
{
    char path[] = "test_trx_v1_radio_XXXXXX";
    int fd = mkstemp(path);
    TEST_ASSERT_TRUE(fd >= 0);
    const char ini[] = "[main]\nradio_model = trx_v1\nrig_pathname = /dev/ttyUSB0\n";
    TEST_ASSERT_EQUAL_INT((int) sizeof(ini) - 1, (int) write(fd, ini, sizeof(ini) - 1));
    close(fd);

    radio c;
    memset(&c, 0, sizeof(c));
    pthread_mutex_init(&c.cfg_mutex, NULL);
    TEST_ASSERT_TRUE(init_config_radio(&c, path));
    TEST_ASSERT_EQUAL_INT(RADIO_MODEL_TRXV1, c.hamlib_model);
    close_config_radio(&c);
    unlink(path);
}

int main(void)
{
    rig_set_debug(RIG_DEBUG_NONE);
    start_firmware();

    UNITY_BEGIN();
    RUN_TEST(test_driver_connects);
    RUN_TEST(test_driver_frequency_and_sideband);
    RUN_TEST(test_driver_ptt);
    RUN_TEST(test_driver_detectors_and_swr);
    RUN_TEST(test_driver_protection);
    RUN_TEST(test_driver_survives_a_lost_reply);
    RUN_TEST(test_driver_2022_firmware);
    RUN_TEST(test_backend_start);
    RUN_TEST(test_backend_frequency_and_mode);
    RUN_TEST(test_backend_ptt_and_meters);
    RUN_TEST(test_backend_firmware_trip_and_reset);
    RUN_TEST(test_backend_refused_key_sets_the_flag);
    RUN_TEST(test_rigctld_client);
    RUN_TEST(test_backend_shutdown_unkeys);
    RUN_TEST(test_force_ptt_off);
    RUN_TEST(test_config_names_the_model);
    return UNITY_END();
}
