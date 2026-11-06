/* sBitx DRM (Digital Radio Mondiale) through Dream subprocesses
 *
 * The DRM channel is centred on the dial, as broadcast DRM is: the 10 kHz
 * HF channel (spectrum occupancy 3) is the dial +- 5 kHz. Everything here
 * is at 48 kHz; the caller resamples to its own rate.
 *
 * Receive: IQ around the dial into Dream ("I/Q positive, 0 Hz IF"), the
 * decoded audio out, mono.
 * Transmit: mono audio into Dream's transmitter, whose I/Q leaves it at a
 * 6 kHz IF (its band-pass is a real filter around the IF, which folds at
 * 0 Hz); here it is mixed down by 6 kHz onto the dial.
 *
 * Dream runs in a directory of its own, with the Dream.ini written here
 * (dream_proc.h); the DSP thread never waits on it.
 *
 * Copyright (C) 2024-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SBITX_DRM_H_
#define SBITX_DRM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SBITX_DRM_RATE  48000
#define SBITX_DRM_TX_IF 6000.0           /* Dream's transmit IF, in Hz */

/* A transmission's parameters, as core.ini gives them. */
typedef struct {
    char robustness;                     /* 'A'..'D' */
    int  spectrum;                       /* occupancy 0..5; 3 is 10 kHz */
    int  msc_qam;                        /* 16 (or 64, for some 6 dB more SNR) */
    int  protection;                     /* 0..3 */
    char codec[16];                      /* xHE-AAC, HE-AAC, AAC */
    char label[17];                      /* the service's, up to 16 bytes */
} sbitx_drm_tx_config;

/* Once, before the first start and not on the DSP thread: whether this
 * Dream can report its receiver's status (--status-file). */
void   sbitx_drm_init(const char *dream_path);

/* Receive. dir is Dream's working directory (made if missing). */
bool   sbitx_drm_rx_start(const char *dream_path, const char *dir);
void   sbitx_drm_rx_stop(void);
bool   sbitx_drm_rx_running(void);
/* n IQ samples (+-1.0) at 48 kHz, centred on the dial */
void   sbitx_drm_rx_iq(const float *i, const float *q, size_t n);
/* up to n samples of decoded audio (+-1.0) at 48 kHz; how many */
size_t sbitx_drm_rx_audio(float *out, size_t n);
/* The receiver's status, as Dream reports it: one JSON object (lock, SNR,
 * the mode, the services with label, codec and bitrate), its length, or 0
 * with none running, a Dream without --status-file, or a report older than
 * 5 s or not valid JSON. Any thread. */
size_t sbitx_drm_rx_status(char *out, size_t len);
/* Whether s[0..n) is exactly one JSON object (whitespace around it). */
bool   sbitx_drm_json_object(const char *s, size_t n);

/* Transmit. */
bool   sbitx_drm_tx_start(const char *dream_path, const char *dir,
                          const sbitx_drm_tx_config *cfg);
void   sbitx_drm_tx_stop(void);
/* Both stopped, waiting (up to 5 s) for the Dreams to exit; at the DSP's
 * shutdown, when nothing else will ask for one. */
void   sbitx_drm_shutdown(void);
bool   sbitx_drm_tx_running(void);
/* n samples of the audio to send (+-1.0) at 48 kHz */
void   sbitx_drm_tx_audio(const float *in, size_t n);
/* up to n IQ samples (+-1.0) at 48 kHz, centred on the dial; how many */
size_t sbitx_drm_tx_iq(float *i, float *q, size_t n);

/* The transmitter's Dream.ini for cfg, into buf; its length, or -1. */
int    sbitx_drm_tx_ini(const sbitx_drm_tx_config *cfg, char *buf, size_t len);

/* Mix n IQ samples down by hz at rate, in place, carrying the phase from
 * call to call in *phase (radians). */
void   sbitx_drm_mix(float *i, float *q, size_t n, double hz, double rate, double *phase);

#endif /* SBITX_DRM_H_ */
