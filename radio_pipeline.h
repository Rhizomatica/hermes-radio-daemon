/* hermes-radio-daemon - media/modem pipeline registry
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RADIO_PIPELINE_H_
#define RADIO_PIPELINE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radio.h"

typedef enum {
    RADIO_PIPELINE_DOMAIN_ANALOG = 0,
    RADIO_PIPELINE_DOMAIN_RADEV2 = 1,
} radio_pipeline_domain;

typedef enum {
    RADIO_PIPELINE_MEDIA_DAEMON = 0,
    RADIO_PIPELINE_MEDIA_LEGACY = 1,
} radio_pipeline_media_owner;

typedef enum {
    RADIO_PIPELINE_RUNTIME_DAEMON_CORE = 0,
    RADIO_PIPELINE_RUNTIME_BACKEND_EMBEDDED = 1,
} radio_pipeline_runtime_kind;

enum {
    RADIO_PIPELINE_CAP_WS_RX_AUDIO = 1u << 0,
    RADIO_PIPELINE_CAP_WS_TX_AUDIO = 1u << 1,
    RADIO_PIPELINE_CAP_RX_SPECTRUM = 1u << 2,
    RADIO_PIPELINE_CAP_TX_SPECTRUM = 1u << 3,
    RADIO_PIPELINE_CAP_RX_RECORDING = 1u << 4,
    RADIO_PIPELINE_CAP_TX_RECORDING = 1u << 5,
    RADIO_PIPELINE_CAP_DAEMON_AUDIO_BRIDGE = 1u << 6,
    RADIO_PIPELINE_CAP_EXTERNAL_MODEM = 1u << 7,
    RADIO_PIPELINE_CAP_EMBEDDED_DSP = 1u << 8,
};

typedef struct radio_pipeline_descriptor {
    const char *name;
    const char *summary;
    radio_backend_kind backend_kind;
    radio_pipeline_domain domain;
    radio_pipeline_media_owner media_owner;
    radio_pipeline_runtime_kind runtime_kind;
    uint32_t capabilities;
} radio_pipeline_descriptor;

size_t radio_pipeline_registry_size(void);
const radio_pipeline_descriptor *radio_pipeline_registry_get(size_t index);
const radio_pipeline_descriptor *radio_pipeline_lookup(const char *name);
const radio_pipeline_descriptor *radio_pipeline_lookup_for_backend(radio_backend_kind backend_kind,
                                                                  bool digital_voice);

void radio_pipeline_refresh(radio *radio_h);
const radio_pipeline_descriptor *radio_pipeline_active(const radio *radio_h);

const char *radio_pipeline_name(const radio *radio_h);
const char *radio_pipeline_summary(const radio *radio_h);
const char *radio_pipeline_domain_name(const radio *radio_h);
const char *radio_pipeline_media_owner_name(const radio *radio_h);
const char *radio_pipeline_runtime_name(const radio *radio_h);

uint32_t radio_pipeline_capabilities(const radio *radio_h);
bool radio_pipeline_has_capability(const radio *radio_h, uint32_t capability);
bool radio_pipeline_supports_websocket_rx_audio(const radio *radio_h);
bool radio_pipeline_supports_websocket_tx_audio(const radio *radio_h);
bool radio_pipeline_supports_spectrum(const radio *radio_h, bool tx);
bool radio_pipeline_uses_daemon_audio_bridge(const radio *radio_h);

#endif /* RADIO_PIPELINE_H_ */
