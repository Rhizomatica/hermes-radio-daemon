/* js8_frame - JS8's frame, as JS8Call sends it: twelve characters of a
 * 64-character alphabet and a 3-bit frame type, a 12-bit CRC, a (174,87)
 * LDPC code (WSJT-X's original FT8 code, not FT8's current (174,91)), and
 * 79 8-FSK tones: three 7-tone Costas arrays around 29 parity and 29 data
 * symbols.  Ported from JS8Call-improved's JS8_Mode/JS8.cpp (GPL-3.0).
 *
 * The receiver (dsp/js8_rx.c) finds the tones and their soft bits; this
 * turns 174 soft bits back into the frame, and a frame into tones.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef JS8_FRAME_H
#define JS8_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#define JS8_N             174   /* codeword bits */
#define JS8_K             87    /* message bits: 72 payload, 3 type, 12 CRC */
#define JS8_M             (JS8_N - JS8_K)  /* parity bits */
#define JS8_NN            79    /* channel symbols */
#define JS8_ND            58    /* data symbols: 29 parity, 29 message */
#define JS8_COSTAS_LEN    7
#define JS8_CHARS         12    /* a frame's characters */
#define JS8_TYPE_BITS     3
#define JS8_MN_MAX        3     /* checks per bit */
#define JS8_NM_MAX        7     /* bits per check */
#define JS8_BP_ITERATIONS 30    /* JS8Call's normal belief-propagation bound */

/* Symbol positions: Costas at 0, 36 and 72; parity at 7, message at 43. */
#define JS8_PARITY_AT     7
#define JS8_DATA_AT       43
#define JS8_COSTAS_STEP   36

/* Which Costas arrays: Normal uses the original (FT8's first), the faster
 * and slower submodes a modified set. */
typedef enum { JS8_COSTAS_ORIGINAL, JS8_COSTAS_MODIFIED } js8_costas_type;

/* The three 7-tone arrays of a type. */
const int (*js8_costas(js8_costas_type type))[JS8_COSTAS_LEN];

/* The 6-bit word of an alphabet character ("0-9A-Za-z-+"), or -1. */
int js8_char_word(char c);

/* JS8's CRC-12 of the 11 bytes holding 87 message bits (MSB first), with
 * the CRC's own 12 bits (75..86) and bit 87 zero. */
uint16_t js8_crc12(const uint8_t bytes[11]);

/* Twelve alphabet characters and a frame type into 79 tones (0..7).
 * False if a character is not in the alphabet. */
bool js8_encode(const char msg[JS8_CHARS], int type, js8_costas_type costas, int tones[JS8_NN]);

/* Belief propagation over 174 log-likelihood ratios (positive: a 1), in
 * the codeword's order (parity bits, then message bits).  On success the
 * 87 message bits go to `bits` and the count of channel bits that
 * disagree with the codeword is returned; -1 if it does not converge. */
int js8_ldpc_decode(const float llr[JS8_N], int max_iterations, uint8_t bits[JS8_K]);

/* The frame in 87 message bits: its twelve characters (NUL-terminated
 * into msg, >= JS8_CHARS + 1) and type, if its CRC holds. */
bool js8_unpack(const uint8_t bits[JS8_K], char *msg, int *type);

#endif
