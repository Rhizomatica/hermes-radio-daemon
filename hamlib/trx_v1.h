/* hermes-radio-daemon - HERMES trx_v1 (uBitx v6) CAT driver
 *
 * Copyright (C) 2021-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The trx_v1 is Rhizomatica's HF transceiver built on the uBitx v6. Its
 * Arduino (Nano) firmware, in hermes-net's trx_v1-firmware, speaks a CAT
 * protocol of its own over the USB serial port, 38400 8N1: every command is
 * 5 bytes, a little-endian 32-bit argument and then the command code; the
 * reply is one byte, or five (a reply code and a little-endian argument)
 * for the readings. Hamlib does not know it, so radiod drives it here the
 * way hermes-net's trx_v1-userland (ubitx_controller) did. The command and
 * reply codes are hermes-net's include/radio_cmds.h, which radiod's
 * hermes_shm interface shares (include/radio_cmds.h).
 *
 * The hamlib backend uses this driver in place of libhamlib when
 * main:radio_model is trx_v1. The calls are not thread-safe: the caller
 * serialises them (the backend's CAT lock).
 */

#ifndef TRX_V1_H_
#define TRX_V1_H_

#include <stdbool.h>
#include <stdint.h>

#define TRXV1_BAUD 38400            /* the firmware's Serial.begin() */

/* Results: 0, or one of these. */
#define TRXV1_OK        0
#define TRXV1_ETIMEOUT  (-1)        /* no reply */
#define TRXV1_EIO       (-2)        /* the port failed */
#define TRXV1_EPROTO    (-3)        /* a reply that is not this command's */
#define TRXV1_EPROTECT  (-4)        /* refused: the SWR protection is latched */

typedef struct trxv1 trxv1;

/* A handle for the radio on the serial port at path, not yet connected
 * (NULL only when out of memory). */
trxv1 *trxv1_new(const char *path);
void trxv1_free(trxv1 *t);
/* Opens the port and waits for the firmware to answer: opening the port
 * resets the Nano, whose bootloader runs first, so this takes a few
 * seconds. Connecting again closes the port first. */
int trxv1_connect(trxv1 *t);
/* Closes the port: every command fails with TRXV1_EIO until the next
 * trxv1_connect. */
void trxv1_disconnect(trxv1 *t);
const char *trxv1_path(const trxv1 *t);

int trxv1_set_freq(trxv1 *t, uint32_t hz);
int trxv1_get_freq(trxv1 *t, uint32_t *hz);
/* The sideband: the radio has USB and LSB only. */
int trxv1_set_usb(trxv1 *t, bool usb);
int trxv1_get_usb(trxv1 *t, bool *usb);
/* Keying a radio already keyed, or unkeying one in receive, is not an
 * error. Keying while the protection is latched is TRXV1_EPROTECT. */
int trxv1_set_ptt(trxv1 *t, bool on);
int trxv1_get_ptt(trxv1 *t, bool *on);
/* The forward and reflected detectors, as the firmware's 10-bit ADC reads
 * them (0..TRXV1_ADC_MAX), refreshed while transmitting. */
#define TRXV1_ADC_MAX 1023
int trxv1_get_fwd(trxv1 *t, uint16_t *adc);
int trxv1_get_ref(trxv1 *t, uint16_t *adc);
/* The firmware's own SWR protection: it unkeys when the reflected reading
 * stays over its threshold (kept in its EEPROM), and refuses to key until
 * it is reset. */
int trxv1_get_protection(trxv1 *t, bool *latched);
int trxv1_reset_protection(trxv1 *t);

/* SWR x TRXV1_SWR_SCALE (radiod's SWR unit: 10 is 1.0) from the two
 * detector readings: (fwd + ref) / (fwd - ref), TRXV1_SWR_MAX_X10 when ref
 * reaches fwd, 1.0 with no forward. */
#define TRXV1_SWR_SCALE   10
#define TRXV1_SWR_MAX_X10 100
uint32_t trxv1_swr_x10(uint16_t fwd, uint16_t ref);

const char *trxv1_strerror(int err);

#endif /* TRX_V1_H_ */
