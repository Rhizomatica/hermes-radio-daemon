/* Rhizomatica HERMES transceiver serial commands
 * Copyright (C) 2021-2022 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 * Copied from hermes-net's trx_v1-userland (include/radio_cmds.h at
 * 8be4c57), the definitions the trx_v1 firmware in the field speaks. Only
 * the prefix changed, CMD_ -> TRXV1_, so they live beside radiod's own
 * include/radio_cmds.h (current hermes-net, shared with the sBitx).
 */

#ifndef TRX_V1_CMDS_H_
#define TRX_V1_CMDS_H_


// we need a 8bit clean serial connection
// PTT commands have priority
// We don't care about endianess (little endian assumed)
// IT MIGHT NOT WORK ON BIG ENDIAN MACHINES RIGHT NOW!


// radio responses type
#define TRXV1_RESP_SHORT 1
#define TRXV1_RESP_LONG 2


// the radio commands - don't change this for compatibility purposes!
#define TRXV1_PTT_ON 0x08
#define TRXV1_PTT_OFF 0x88

#define TRXV1_RESET_PROTECTION 0x90

#define TRXV1_GET_FREQ 0x91
#define TRXV1_SET_FREQ 0x92

#define TRXV1_GET_MODE 0x93
#define TRXV1_SET_MODE 0x94

#define TRXV1_GET_TXRX_STATUS 0x95

#define TRXV1_GET_PROTECTION_STATUS 0x96

#define TRXV1_GET_MASTERCAL 0x97
#define TRXV1_SET_MASTERCAL 0x98

#define TRXV1_GET_BFO 0x99
#define TRXV1_SET_BFO 0x9a

#define TRXV1_GET_FWD 0x9b
#define TRXV1_GET_REF 0x9c

#define TRXV1_GET_LED_STATUS 0x9d
#define TRXV1_SET_LED_STATUS 0x9e

#define TRXV1_GET_CONNECTED_STATUS 0x9f
#define TRXV1_SET_CONNECTED_STATUS 0xa0

#define TRXV1_SET_SERIAL 0xa1
#define TRXV1_GET_SERIAL 0xa2

#define TRXV1_SET_REF_THRESHOLD 0xe0
#define TRXV1_GET_REF_THRESHOLD 0xe1

#define TRXV1_SET_RADIO_DEFAULTS 0xea
#define TRXV1_RESTORE_RADIO_DEFAULTS 0xeb

#define TRXV1_RADIO_RESET 0xef

#define TRXV1_GPS_CALIBRATE 0xd0

#define TRXV1_GET_STATUS 0xd3


// radio responses
// 5 bytes responses
#define TRXV1_RESP_GET_FREQ_ACK 0xa3
#define TRXV1_RESP_GET_MASTERCAL_ACK 0xa4
#define TRXV1_RESP_GET_BFO_ACK 0xa5

#define TRXV1_RESP_GET_FWD_ACK 0xa6
#define TRXV1_RESP_GET_REF_ACK 0xa7

#define TRXV1_RESP_GET_SERIAL_ACK 0xa8

#define TRXV1_RESP_GET_REF_THRESHOLD_ACK 0xe2

#define TRXV1_RESP_GET_STATUS_ACK 0xd4

// 1 byte responses
#define TRXV1_RESP_PTT_ON_ACK 0xa9
#define TRXV1_RESP_PTT_ON_NACK 0xaa

#define TRXV1_RESP_PTT_OFF_ACK 0xab
#define TRXV1_RESP_PTT_OFF_NACK 0xac

#define TRXV1_RESP_SET_FREQ_ACK 0xad

#define TRXV1_RESP_SET_MODE_ACK 0xaf

#define TRXV1_RESP_GET_MODE_USB 0xb0
#define TRXV1_RESP_GET_MODE_LSB 0xb1

#define TRXV1_RESP_GET_TXRX_INTX 0xb2
#define TRXV1_RESP_GET_TXRX_INRX 0xb3

#define TRXV1_RESP_GET_PROTECTION_ON 0xb4
#define TRXV1_RESP_GET_PROTECTION_OFF 0xb5

#define TRXV1_RESP_SET_MASTERCAL_ACK 0xb6

#define TRXV1_RESP_SET_BFO_ACK 0xb7

#define TRXV1_RESP_GET_LED_STATUS_ON 0xb8
#define TRXV1_RESP_GET_LED_STATUS_OFF 0xb9

#define TRXV1_RESP_SET_LED_STATUS_ACK 0xba

#define TRXV1_RESP_GET_CONNECTED_STATUS_ON 0xbb
#define TRXV1_RESP_GET_CONNECTED_STATUS_OFF 0xbc

#define TRXV1_RESP_SET_CONNECTED_STATUS_ACK 0xbd

#define TRXV1_RESP_RESET_PROTECTION_ACK 0xbe

#define TRXV1_RESP_SET_SERIAL_ACK 0xbf

#define TRXV1_RESP_SET_REF_THRESHOLD_ACK 0xe3

#define TRXV1_RESP_SET_RADIO_DEFAULTS_ACK 0xec

#define TRXV1_RESP_RESTORE_RADIO_DEFAULTS_ACK 0xed

#define TRXV1_RESP_GPS_CALIBRATE_ACK 0xd1

#define TRXV1_RESP_GPS_NOT_PRESENT 0xd2

#define TRXV1_RESP_WRONG_COMMAND 0xc0

// used for PTT responses
#define TRXV1_ALERT_PROTECTION_ON 0xe7

#endif /* TRX_V1_CMDS_H_ */
