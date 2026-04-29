/* hermes-radio-daemon - media/modem pipeline registry
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "radio_pipeline.h"

static const radio_pipeline_descriptor pipeline_registry[] = {
    {
        .name = "hamlib-analog",
        .summary = "Hamlib RF control with daemon-managed analog audio bridge",
        .backend_kind = RADIO_BACKEND_HAMLIB,
        .domain = RADIO_PIPELINE_DOMAIN_ANALOG,
        .media_owner = RADIO_PIPELINE_MEDIA_DAEMON,
        .runtime_kind = RADIO_PIPELINE_RUNTIME_DAEMON_CORE,
        .capabilities = RADIO_PIPELINE_CAP_WS_RX_AUDIO |
                        RADIO_PIPELINE_CAP_WS_TX_AUDIO |
                        RADIO_PIPELINE_CAP_RX_SPECTRUM |
                        RADIO_PIPELINE_CAP_TX_SPECTRUM |
                        RADIO_PIPELINE_CAP_RX_RECORDING |
                        RADIO_PIPELINE_CAP_TX_RECORDING |
                        RADIO_PIPELINE_CAP_DAEMON_AUDIO_BRIDGE,
    },
    {
        .name = "hamlib-radev2",
        .summary = "Hamlib RF control with daemon audio bridge and explicit RADEv2 slot",
        .backend_kind = RADIO_BACKEND_HAMLIB,
        .domain = RADIO_PIPELINE_DOMAIN_RADEV2,
        .media_owner = RADIO_PIPELINE_MEDIA_DAEMON,
        .runtime_kind = RADIO_PIPELINE_RUNTIME_DAEMON_CORE,
        .capabilities = RADIO_PIPELINE_CAP_WS_RX_AUDIO |
                        RADIO_PIPELINE_CAP_WS_TX_AUDIO |
                        RADIO_PIPELINE_CAP_RX_SPECTRUM |
                        RADIO_PIPELINE_CAP_TX_SPECTRUM |
                        RADIO_PIPELINE_CAP_RX_RECORDING |
                        RADIO_PIPELINE_CAP_TX_RECORDING |
                        RADIO_PIPELINE_CAP_DAEMON_AUDIO_BRIDGE |
                        RADIO_PIPELINE_CAP_EXTERNAL_MODEM,
    },
    {
        .name = "hfsignals-analog",
        .summary = "Embedded legacy_sbitx analog DSP and ALSA path",
        .backend_kind = RADIO_BACKEND_HFSIGNALS,
        .domain = RADIO_PIPELINE_DOMAIN_ANALOG,
        .media_owner = RADIO_PIPELINE_MEDIA_LEGACY,
        .runtime_kind = RADIO_PIPELINE_RUNTIME_BACKEND_EMBEDDED,
        .capabilities = RADIO_PIPELINE_CAP_EMBEDDED_DSP,
    },
    {
        .name = "hfsignals-radev2",
        .summary = "Embedded legacy_sbitx RADEv2 DSP and ALSA path",
        .backend_kind = RADIO_BACKEND_HFSIGNALS,
        .domain = RADIO_PIPELINE_DOMAIN_RADEV2,
        .media_owner = RADIO_PIPELINE_MEDIA_LEGACY,
        .runtime_kind = RADIO_PIPELINE_RUNTIME_BACKEND_EMBEDDED,
        .capabilities = RADIO_PIPELINE_CAP_EMBEDDED_DSP,
    },
};

static bool active_profile_uses_digital_voice(const radio *radio_h)
{
    uint32_t active;

    if (!radio_h || radio_h->profiles_count == 0)
        return false;

    active = radio_h->profile_active_idx;
    if (active >= radio_h->profiles_count)
        active = 0;

    return radio_h->profiles[active].digital_voice;
}

static const char *pipeline_domain_name_from_descriptor(const radio_pipeline_descriptor *pipeline)
{
    if (!pipeline)
        return "unknown";

    return pipeline->domain == RADIO_PIPELINE_DOMAIN_RADEV2 ? "radev2" : "analog";
}

static const char *pipeline_media_owner_name_from_descriptor(const radio_pipeline_descriptor *pipeline)
{
    if (!pipeline)
        return "unknown";

    return pipeline->media_owner == RADIO_PIPELINE_MEDIA_LEGACY ?
           "legacy-embedded" : "daemon";
}

static const char *pipeline_runtime_name_from_descriptor(const radio_pipeline_descriptor *pipeline)
{
    if (!pipeline)
        return "unknown";

    return pipeline->runtime_kind == RADIO_PIPELINE_RUNTIME_BACKEND_EMBEDDED ?
           "embedded-backend" : "daemon-core";
}

size_t radio_pipeline_registry_size(void)
{
    return sizeof(pipeline_registry) / sizeof(pipeline_registry[0]);
}

const radio_pipeline_descriptor *radio_pipeline_registry_get(size_t index)
{
    if (index >= radio_pipeline_registry_size())
        return NULL;

    return &pipeline_registry[index];
}

const radio_pipeline_descriptor *radio_pipeline_lookup(const char *name)
{
    size_t i;

    if (!name || name[0] == '\0')
        return NULL;

    for (i = 0; i < radio_pipeline_registry_size(); i++)
    {
        const radio_pipeline_descriptor *pipeline = &pipeline_registry[i];
        if (!strcmp(pipeline->name, name))
            return pipeline;
    }

    return NULL;
}

const radio_pipeline_descriptor *radio_pipeline_lookup_for_backend(radio_backend_kind backend_kind,
                                                                  bool digital_voice)
{
    size_t i;
    const radio_pipeline_descriptor *fallback = NULL;
    radio_pipeline_domain wanted = digital_voice ?
                                   RADIO_PIPELINE_DOMAIN_RADEV2 :
                                   RADIO_PIPELINE_DOMAIN_ANALOG;

    for (i = 0; i < radio_pipeline_registry_size(); i++)
    {
        const radio_pipeline_descriptor *pipeline = &pipeline_registry[i];
        if (pipeline->backend_kind != backend_kind)
            continue;

        if (!fallback)
            fallback = pipeline;
        if (pipeline->domain == wanted)
            return pipeline;
    }

    return fallback;
}

void radio_pipeline_refresh(radio *radio_h)
{
    const radio_pipeline_descriptor *pipeline;
    const radio_pipeline_descriptor *previous;

    if (!radio_h)
        return;

    pipeline = radio_pipeline_lookup_for_backend(radio_h->backend_kind,
                                                 active_profile_uses_digital_voice(radio_h));
    previous = atomic_load(&radio_h->pipeline);

    if (previous != pipeline && pipeline)
    {
        fprintf(stderr,
                "radio_pipeline: active=%s mode=%s media=%s runtime=%s\n",
                pipeline->name,
                pipeline_domain_name_from_descriptor(pipeline),
                pipeline_media_owner_name_from_descriptor(pipeline),
                pipeline_runtime_name_from_descriptor(pipeline));
    }

    atomic_store(&radio_h->pipeline, pipeline);
}

const radio_pipeline_descriptor *radio_pipeline_active(const radio *radio_h)
{
    if (!radio_h)
        return NULL;

    return atomic_load(&radio_h->pipeline);
}

const char *radio_pipeline_name(const radio *radio_h)
{
    const radio_pipeline_descriptor *pipeline = radio_pipeline_active(radio_h);
    return pipeline ? pipeline->name : "unknown";
}

const char *radio_pipeline_summary(const radio *radio_h)
{
    const radio_pipeline_descriptor *pipeline = radio_pipeline_active(radio_h);
    return pipeline ? pipeline->summary : "Unknown radio pipeline";
}

const char *radio_pipeline_domain_name(const radio *radio_h)
{
    return pipeline_domain_name_from_descriptor(radio_pipeline_active(radio_h));
}

const char *radio_pipeline_media_owner_name(const radio *radio_h)
{
    return pipeline_media_owner_name_from_descriptor(radio_pipeline_active(radio_h));
}

const char *radio_pipeline_runtime_name(const radio *radio_h)
{
    return pipeline_runtime_name_from_descriptor(radio_pipeline_active(radio_h));
}

uint32_t radio_pipeline_capabilities(const radio *radio_h)
{
    const radio_pipeline_descriptor *pipeline = radio_pipeline_active(radio_h);
    return pipeline ? pipeline->capabilities : 0;
}

bool radio_pipeline_has_capability(const radio *radio_h, uint32_t capability)
{
    return (radio_pipeline_capabilities(radio_h) & capability) != 0;
}

bool radio_pipeline_supports_websocket_rx_audio(const radio *radio_h)
{
    return radio_pipeline_has_capability(radio_h, RADIO_PIPELINE_CAP_WS_RX_AUDIO);
}

bool radio_pipeline_supports_websocket_tx_audio(const radio *radio_h)
{
    return radio_pipeline_has_capability(radio_h, RADIO_PIPELINE_CAP_WS_TX_AUDIO);
}

bool radio_pipeline_supports_spectrum(const radio *radio_h, bool tx)
{
    return radio_pipeline_has_capability(
        radio_h, tx ? RADIO_PIPELINE_CAP_TX_SPECTRUM : RADIO_PIPELINE_CAP_RX_SPECTRUM);
}

bool radio_pipeline_uses_daemon_audio_bridge(const radio *radio_h)
{
    return radio_h &&
           radio_h->enable_audio_bridge &&
           radio_pipeline_has_capability(radio_h, RADIO_PIPELINE_CAP_DAEMON_AUDIO_BRIDGE);
}
