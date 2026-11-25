/* hermes-radio-daemon - HERMES trx_v1 (uBitx v6) CAT driver
 *
 * Copyright (C) 2021-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The protocol of hermes-net's trx_v1-firmware (ubitx_cat.cpp), as its
 * trx_v1-userland drove it (ubitx_controller.c, ubitx_client.c).
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "trx_v1.h"
#include "radio_cmds.h"

#include "radiod_log.h"

#define LOG_C "trx_v1"

#define TRXV1_CMD_LEN    5          /* 4 argument bytes + the command code */
#define TRXV1_REPLY_LONG 5          /* a reading: the reply code + 4 bytes */

/* The waits below can be set at build time (the tests make them short). */

/* Opening the port raises DTR, which resets the Nano; its bootloader waits
 * for an upload for up to about 2 s before the firmware starts. The
 * firmware is then probed until it answers, for at most TRXV1_BOOT_MAX_MS. */
#ifndef TRXV1_BOOT_MS
#define TRXV1_BOOT_MS      2000
#endif
#ifndef TRXV1_BOOT_MAX_MS
#define TRXV1_BOOT_MAX_MS  6000
#endif
#ifndef TRXV1_PROBE_GAP_MS
#define TRXV1_PROBE_GAP_MS 250
#endif

/* The firmware answers within its main loop, but a frequency or mode
 * command first saves the VFOs to EEPROM (3.3 ms a byte), so the wait for a
 * reply's first byte is generous; the rest of a reading follows at once at
 * 38400 baud. */
#ifndef TRXV1_REPLY_MS
#define TRXV1_REPLY_MS     250
#endif
#ifndef TRXV1_REST_MS
#define TRXV1_REST_MS      50
#endif

/* After a command that drew no reply the firmware may hold part of a
 * frame; it drops a frame that stays incomplete for CAT_RECEIVE_TIMEOUT
 * (500 ms, ubitx_cat.h), so wait that out before the next command. */
#ifndef TRXV1_RESYNC_MS
#define TRXV1_RESYNC_MS    600
#endif

#define TRXV1_PATH_MAX 256

struct trxv1 {
    char path[TRXV1_PATH_MAX];
    int fd;                         /* -1 while disconnected */
    bool resync;                    /* the last command went unanswered */
};

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long) (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static int open_port(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0)
    {
        HLOGE(LOG_C, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) < 0)
    {
        HLOGE(LOG_C, "%s is not a serial port: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, B38400);
    cfsetospeed(&tio, B38400);
    if (tcsetattr(fd, TCSANOW, &tio) < 0)
    {
        HLOGE(LOG_C, "cannot set up %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* Reads exactly len bytes within ms. */
static int read_full(int fd, uint8_t *buf, size_t len, int ms)
{
    size_t got = 0;
    int64_t end = now_ms() + ms;

    while (got < len)
    {
        int left = (int) (end - now_ms());
        if (left <= 0)
            return TRXV1_ETIMEOUT;

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int r = poll(&pfd, 1, left);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return TRXV1_EIO;
        }
        if (r == 0)
            return TRXV1_ETIMEOUT;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return TRXV1_EIO;

        ssize_t n = read(fd, buf + got, len - got);
        if (n < 0)
        {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return TRXV1_EIO;
        }
        if (n == 0)
            return TRXV1_EIO;
        got += (size_t) n;
    }
    return TRXV1_OK;
}

/* The replies that carry a 4-byte argument. */
static bool reply_is_long(uint8_t code)
{
    switch (code)
    {
    case CMD_RESP_GET_FREQ_ACK:
    case CMD_RESP_GET_BFO_ACK:
    case CMD_RESP_GET_FWD_ACK:
    case CMD_RESP_GET_REF_ACK:
    case CMD_RESP_GET_SERIAL_ACK:
    case CMD_RESP_GET_REF_THRESHOLD_ACK:
    case CMD_RESP_GET_STATUS_ACK:
    case CMD_RESP_GET_MASTERCAL_ACK:
        return true;
    default:
        return false;
    }
}

/* One command and its reply: reply[0] is the reply code, reply[1..4] a
 * reading's argument (little-endian). */
static int transact(trxv1 *t, uint8_t code, uint32_t arg,
                    uint8_t reply[TRXV1_REPLY_LONG])
{
    uint8_t cmd[TRXV1_CMD_LEN] = {
        (uint8_t) arg, (uint8_t) (arg >> 8), (uint8_t) (arg >> 16),
        (uint8_t) (arg >> 24), code,
    };

    if (!t || t->fd < 0)
        return TRXV1_EIO;

    if (t->resync)
    {
        sleep_ms(TRXV1_RESYNC_MS);
        t->resync = false;
    }
    /* a late reply to an earlier command would be read as this one's */
    tcflush(t->fd, TCIFLUSH);

    size_t sent = 0;
    while (sent < sizeof(cmd))
    {
        ssize_t n = write(t->fd, cmd + sent, sizeof(cmd) - sent);
        if (n < 0)
        {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return TRXV1_EIO;
        }
        sent += (size_t) n;
    }

    memset(reply, 0, TRXV1_REPLY_LONG);
    int ret = read_full(t->fd, reply, 1, TRXV1_REPLY_MS);
    if (ret == TRXV1_OK && reply_is_long(reply[0]))
        ret = read_full(t->fd, reply + 1, TRXV1_REPLY_LONG - 1, TRXV1_REST_MS);
    if (ret == TRXV1_ETIMEOUT)
        t->resync = true;
    return ret;
}

static uint32_t le32(const uint8_t *b)
{
    return (uint32_t) b[0] | (uint32_t) b[1] << 8 |
           (uint32_t) b[2] << 16 | (uint32_t) b[3] << 24;
}

/* A command whose reply is the general ACK. */
static int command_ack(trxv1 *t, uint8_t code, uint32_t arg)
{
    uint8_t reply[TRXV1_REPLY_LONG];
    int ret = transact(t, code, arg, reply);
    if (ret != TRXV1_OK)
        return ret;
    return reply[0] == CMD_RESP_ACK ? TRXV1_OK : TRXV1_EPROTO;
}

/* A reading: the reply code it must carry, and its argument. */
static int reading(trxv1 *t, uint8_t code, uint8_t want, uint32_t *out)
{
    uint8_t reply[TRXV1_REPLY_LONG];
    int ret = transact(t, code, 0, reply);
    if (ret != TRXV1_OK)
        return ret;
    if (reply[0] != want)
        return TRXV1_EPROTO;
    *out = le32(reply + 1);
    return TRXV1_OK;
}

trxv1 *trxv1_new(const char *path)
{
    trxv1 *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->fd = -1;
    snprintf(t->path, sizeof(t->path), "%s", path ? path : "");
    return t;
}

void trxv1_free(trxv1 *t)
{
    if (!t)
        return;
    trxv1_disconnect(t);
    free(t);
}

const char *trxv1_path(const trxv1 *t)
{
    return t ? t->path : "";
}

int trxv1_connect(trxv1 *t)
{
    if (!t)
        return TRXV1_EIO;
    trxv1_disconnect(t);
    if (!t->path[0])
    {
        HLOGE(LOG_C, "no serial port: set main:rig_pathname");
        return TRXV1_EIO;
    }

    t->fd = open_port(t->path);
    if (t->fd < 0)
        return TRXV1_EIO;

    sleep_ms(TRXV1_BOOT_MS);
    int64_t end = now_ms() + TRXV1_BOOT_MAX_MS - TRXV1_BOOT_MS;
    uint32_t hz = 0;
    int ret;
    while ((ret = trxv1_get_freq(t, &hz)) != TRXV1_OK && now_ms() < end)
    {
        t->resync = false;
        sleep_ms(TRXV1_PROBE_GAP_MS);
    }
    t->resync = false;
    if (ret != TRXV1_OK)
    {
        HLOGE(LOG_C, "no answer from the radio on %s: %s",
              t->path, trxv1_strerror(ret));
        trxv1_disconnect(t);
        return ret;
    }
    HLOGI(LOG_C, "trx_v1 on %s answers, at %u Hz", t->path, hz);
    return TRXV1_OK;
}

void trxv1_disconnect(trxv1 *t)
{
    if (!t || t->fd < 0)
        return;
    close(t->fd);
    t->fd = -1;
}

int trxv1_set_freq(trxv1 *t, uint32_t hz)
{
    return command_ack(t, CMD_SET_FREQ, hz);
}

int trxv1_get_freq(trxv1 *t, uint32_t *hz)
{
    return reading(t, CMD_GET_FREQ, CMD_RESP_GET_FREQ_ACK, hz);
}

/* CMD_SET_MODE's first byte: 0 LSB, 1 USB (ubitx_client). */
#define TRXV1_MODE_LSB 0x00
#define TRXV1_MODE_USB 0x01

int trxv1_set_usb(trxv1 *t, bool usb)
{
    return command_ack(t, CMD_SET_MODE, usb ? TRXV1_MODE_USB : TRXV1_MODE_LSB);
}

int trxv1_get_usb(trxv1 *t, bool *usb)
{
    uint8_t reply[TRXV1_REPLY_LONG];
    int ret = transact(t, CMD_GET_MODE, 0, reply);
    if (ret != TRXV1_OK)
        return ret;
    if (reply[0] == CMD_RESP_GET_MODE_USB)
        *usb = true;
    else if (reply[0] == CMD_RESP_GET_MODE_LSB)
        *usb = false;
    else
        return TRXV1_EPROTO;
    return TRXV1_OK;
}

int trxv1_set_ptt(trxv1 *t, bool on)
{
    uint8_t reply[TRXV1_REPLY_LONG];
    int ret = transact(t, on ? CMD_PTT_ON : CMD_PTT_OFF, 0, reply);
    if (ret != TRXV1_OK)
        return ret;

    switch (reply[0])
    {
    case CMD_RESP_ACK:
        return TRXV1_OK;
    case CMD_RESP_PTT_ON_NACK:      /* already transmitting */
        return on ? TRXV1_OK : TRXV1_EPROTO;
    case CMD_RESP_PTT_OFF_NACK:     /* already receiving */
        return on ? TRXV1_EPROTO : TRXV1_OK;
    case CMD_ALERT_PROTECTION_ON:
        /* the trip unkeyed it: a PTT off finds it in receive */
        return on ? TRXV1_EPROTECT : TRXV1_OK;
    default:
        return TRXV1_EPROTO;
    }
}

int trxv1_get_ptt(trxv1 *t, bool *on)
{
    uint8_t reply[TRXV1_REPLY_LONG];
    int ret = transact(t, CMD_GET_TXRX_STATUS, 0, reply);
    if (ret != TRXV1_OK)
        return ret;
    if (reply[0] == CMD_RESP_GET_TXRX_INTX)
        *on = true;
    else if (reply[0] == CMD_RESP_GET_TXRX_INRX)
        *on = false;
    else
        return TRXV1_EPROTO;
    return TRXV1_OK;
}

static int adc_reading(trxv1 *t, uint8_t code, uint8_t want, uint16_t *adc)
{
    uint32_t v;
    int ret = reading(t, code, want, &v);
    if (ret != TRXV1_OK)
        return ret;
    v &= UINT16_MAX;                /* a 16-bit value; the rest is padding */
    *adc = (uint16_t) (v > TRXV1_ADC_MAX ? TRXV1_ADC_MAX : v);
    return TRXV1_OK;
}

int trxv1_get_fwd(trxv1 *t, uint16_t *adc)
{
    return adc_reading(t, CMD_GET_FWD, CMD_RESP_GET_FWD_ACK, adc);
}

int trxv1_get_ref(trxv1 *t, uint16_t *adc)
{
    return adc_reading(t, CMD_GET_REF, CMD_RESP_GET_REF_ACK, adc);
}

int trxv1_get_protection(trxv1 *t, bool *latched)
{
    uint8_t reply[TRXV1_REPLY_LONG];
    int ret = transact(t, CMD_GET_PROTECTION_STATUS, 0, reply);
    if (ret != TRXV1_OK)
        return ret;
    if (reply[0] == CMD_RESP_GET_PROTECTION_ON)
        *latched = true;
    else if (reply[0] == CMD_RESP_GET_PROTECTION_OFF)
        *latched = false;
    else
        return TRXV1_EPROTO;
    return TRXV1_OK;
}

int trxv1_reset_protection(trxv1 *t)
{
    return command_ack(t, CMD_RESET_PROTECTION, 0);
}

uint32_t trxv1_swr_x10(uint16_t fwd, uint16_t ref)
{
    if (fwd == 0)
        return TRXV1_SWR_SCALE;
    if (ref >= fwd)
        return TRXV1_SWR_MAX_X10;
    uint32_t swr = (TRXV1_SWR_SCALE * ((uint32_t) fwd + ref) + (fwd - ref) / 2) /
                   (fwd - ref);
    return swr > TRXV1_SWR_MAX_X10 ? TRXV1_SWR_MAX_X10 : swr;
}

const char *trxv1_strerror(int err)
{
    switch (err)
    {
    case TRXV1_OK:       return "ok";
    case TRXV1_ETIMEOUT: return "no reply";
    case TRXV1_EIO:      return "serial port error";
    case TRXV1_EPROTO:   return "unexpected reply";
    case TRXV1_EPROTECT: return "SWR protection latched";
    default:             return "unknown error";
    }
}
