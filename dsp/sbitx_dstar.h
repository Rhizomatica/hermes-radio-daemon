/* sBitx D-STAR DV modem (GMSK 4800 baud at 24 kHz)
 *
 * Port of the MMDVM firmware D-Star modulator/demodulator (DStarTX.cpp /
 * DStarRX.cpp, Copyright Jonathan Naylor G4KLX, GPLv2) to plain C for the
 * hermes-radio-daemon DSP.
 *
 * RX: expects FM-discriminator audio at 24 kHz (5 samples per symbol) and
 * runs the full frame state machine: frame-sync search, header decode
 * (descramble, deinterleave, Viterbi, CRC16), data-frame collection with
 * data-sync correlation, and end-of-transmission detection. Decoded
 * header/data frames and loss of lock are delivered via callbacks.
 *
 * TX: accepts 12-byte DV frames (24-bit sync word + 72-bit AMBE+FEC data)
 * and GMSK-modulates them (BT=0.35, 3-symbol Gaussian filter, identical to
 * MMDVM's arm_fir_interpolate_q15 polyphase structure) into 24 kHz baseband
 * samples.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SBITX_DSTAR_H_
#define SBITX_DSTAR_H_

#include <stdbool.h>
#include <stdint.h>

#define SBITX_DSTAR_RX_RATE   24000
#define SBITX_DSTAR_TX_RATE   24000

#define SBITX_DSTAR_HEADER_BYTES 41
#define SBITX_DSTAR_FRAME_BYTES  12   /* 24-bit sync + 72-bit data */
#define SBITX_DSTAR_DATA_BYTES   9

/* TX queue depth: enough for a header + preamble + ~4 s of voice. */
#define SBITX_DSTAR_TX_BUF_BYTES 8192

/* TX timing: preamble bytes of 0xAA before the header (MMDVM default 100ms) */
#define SBITX_DSTAR_TX_PREAMBLE_BYTES 60

/* ── RX ───────────────────────────────────────────────────────────── */

typedef void (*sbitx_dstar_header_cb)(void *user, const uint8_t *header);
typedef void (*sbitx_dstar_data_cb)(void *user, const uint8_t *frame);
typedef void (*sbitx_dstar_lost_cb)(void *user);
typedef void (*sbitx_dstar_eot_cb)(void *user);

typedef struct sbitx_dstar_rx sbitx_dstar_rx;

sbitx_dstar_rx *sbitx_dstar_rx_new(void);
void            sbitx_dstar_rx_free(sbitx_dstar_rx *rx);

void sbitx_dstar_rx_set_cbs(sbitx_dstar_rx *rx,
                            sbitx_dstar_header_cb hdr_cb,
                            sbitx_dstar_data_cb   data_cb,
                            sbitx_dstar_lost_cb   lost_cb,
                            sbitx_dstar_eot_cb    eot_cb,
                            void *user);

/* Set the discriminator polarity: +1 (default) or -1 (inverted). Some
 * rigs/demodulators present the FM discriminator inverted. */
void sbitx_dstar_rx_set_polarity(sbitx_dstar_rx *rx, float polarity);

void sbitx_dstar_rx_reset(sbitx_dstar_rx *rx);

/* Feed n samples of discriminator audio at 24 kHz. Callbacks may fire. */
void sbitx_dstar_rx_process(sbitx_dstar_rx *rx, const float *audio, int n);

/* ── TX ───────────────────────────────────────────────────────────── */

typedef struct sbitx_dstar_tx sbitx_dstar_tx;

sbitx_dstar_tx *sbitx_dstar_tx_new(void);
void            sbitx_dstar_tx_free(sbitx_dstar_tx *tx);

/* Reset the modulator and byte queue. */
void sbitx_dstar_tx_reset(sbitx_dstar_tx *tx);

/* Queue a header burst: (optional) preamble + sync + FEC'd header.
 * A preamble is only emitted when the queue was idle. Returns 0 on
 * success, negative when the queue is full. */
int sbitx_dstar_tx_header(sbitx_dstar_tx *tx, const uint8_t *header41);

/* Queue one 12-byte DV frame (3 sync bytes + 9 data bytes). */
int sbitx_dstar_tx_frame(sbitx_dstar_tx *tx, const uint8_t *frame12);

/* Queue the end-of-transmission pattern (3x 12-byte end sync). */
int sbitx_dstar_tx_eot(sbitx_dstar_tx *tx);

/* Non-zero when queued bytes or modulator state remain. */
int sbitx_dstar_tx_pending(const sbitx_dstar_tx *tx);

/* Fill up to n 24 kHz baseband samples. Returns samples written
 * (<= n; 0 when idle). */
int sbitx_dstar_tx_generate(sbitx_dstar_tx *tx, float *out24k, int n);

/* CCITT-16 CRC as used in the D-STAR header (over the first 39 bytes
 * of the 41-byte header; returns the little-endian pair for bytes
 * 39 and 40). */
uint16_t sbitx_dstar_crc16(const uint8_t *data, int len);

/* Build a 41-byte DV voice header (flags 0x10). Callers fill the
 * callsign fields; this appends the CRC. */
void sbitx_dstar_header_finalize(uint8_t *header41);

#endif /* SBITX_DSTAR_H_ */
