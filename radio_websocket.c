/* hermes-radio-daemon - websocket control and media service
 *
 * Copyright (C) 2024-2025 Rhizomatica
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
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "radio_backend.h"
#include "radio_media.h"
#include "radio_pipeline.h"
#include "radio_websocket.h"

extern _Atomic bool shutdown_;

#define WS_MAX_CLIENTS 8
#define WS_RX_CHUNK_SAMPLES 256
#define WS_OPCODE_TEXT 0x1
#define WS_OPCODE_BINARY 0x2
#define WS_OPCODE_CLOSE 0x8
#define WS_OPCODE_PING 0x9
#define WS_OPCODE_PONG 0xA

#define WS_STREAM_RX_AUDIO 0x01
#define WS_STREAM_RX_SPECTRUM 0x02
#define WS_STREAM_TX_SPECTRUM 0x03
#define WS_STREAM_SERVER_INFO 0x10

typedef struct {
    int fd;
    time_t last_status;
    uint32_t last_rx_spectrum_seq;
    uint32_t last_tx_spectrum_seq;
} ws_client;

typedef struct {
    radio *radio_h;
    int server_fd;
    ws_client clients[WS_MAX_CLIENTS];
} websocket_ctx;

static websocket_ctx ws_ctx;

static void close_client(ws_client *client)
{
    if (client->fd >= 0)
        close(client->fd);
    client->fd = -1;
    client->last_status = 0;
    client->last_rx_spectrum_seq = 0;
    client->last_tx_spectrum_seq = 0;
}

static const char *mode_to_string(uint16_t mode)
{
    switch (mode)
    {
    case MODE_LSB: return "LSB";
    case MODE_CW:  return "CW";
    default:       return "USB";
    }
}

static const char *backend_to_string(radio_backend_kind backend_kind)
{
    switch (backend_kind)
    {
    case RADIO_BACKEND_HFSIGNALS:
        return "hfsignals";
    case RADIO_BACKEND_HAMLIB:
    default:
        return "hamlib";
    }
}

static int parse_bind(const char *bind, char *host, size_t host_len, int *port)
{
    const char *colon = strrchr(bind, ':');
    if (!colon)
        return -1;

    snprintf(host, host_len, "%.*s", (int) (colon - bind), bind);
    *port = atoi(colon + 1);
    return (*port > 0) ? 0 : -1;
}

static int create_server_socket(const char *bind_addr)
{
    char host[64];
    int port;
    int yes = 1;
    int fd;
    struct sockaddr_in addr;

    if (parse_bind(bind_addr, host, sizeof(host), &port) != 0)
    {
        fprintf(stderr, "radio_websocket: invalid bind '%s'\n", bind_addr);
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) port);
    if (!host[0] || !strcmp(host, "0.0.0.0"))
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        fprintf(stderr, "radio_websocket: invalid host '%s'\n", host);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
        listen(fd, WS_MAX_CLIENTS) != 0)
    {
        fprintf(stderr, "radio_websocket: bind/listen failed on %s: %s\n",
                bind_addr, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static char *find_header_value(char *request, const char *header)
{
    size_t header_len = strlen(header);
    char *line = request;

    while (line && *line)
    {
        char *eol = strstr(line, "\r\n");
        if (!eol)
            break;
        if (strncasecmp(line, header, header_len) == 0)
        {
            char *value = line + header_len;
            while (*value == ' ' || *value == '\t')
                value++;
            *eol = '\0';
            return value;
        }
        line = eol + 2;
    }

    return NULL;
}

static bool websocket_handshake(int fd)
{
    char request[4096];
    char accept_src[256];
    char accept_raw[SHA_DIGEST_LENGTH];
    char accept_b64[128];
    const char *magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    ssize_t n = recv(fd, request, sizeof(request) - 1, 0);
    char *key;
    char response[512];

    if (n <= 0)
        return false;
    request[n] = '\0';

    key = find_header_value(request, "Sec-WebSocket-Key:");
    if (!key)
        return false;

    snprintf(accept_src, sizeof(accept_src), "%s%s", key, magic);
    SHA1((unsigned char *) accept_src, strlen(accept_src), (unsigned char *) accept_raw);
    EVP_EncodeBlock((unsigned char *) accept_b64, (unsigned char *) accept_raw, SHA_DIGEST_LENGTH);

    snprintf(response, sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n",
             accept_b64);

    return send(fd, response, strlen(response), 0) == (ssize_t) strlen(response);
}

static ssize_t send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0)
            return -1;
        sent += (size_t) n;
    }
    return (ssize_t) sent;
}

static bool websocket_send_frame(int fd, uint8_t opcode, const void *payload, size_t len)
{
    uint8_t header[10];
    size_t header_len = 0;

    header[header_len++] = 0x80u | (opcode & 0x0fu);
    if (len < 126)
    {
        header[header_len++] = (uint8_t) len;
    }
    else if (len <= 0xffff)
    {
        header[header_len++] = 126;
        header[header_len++] = (uint8_t) ((len >> 8) & 0xff);
        header[header_len++] = (uint8_t) (len & 0xff);
    }
    else
    {
        return false;
    }

    if (send_all(fd, header, header_len) < 0)
        return false;
    if (len > 0 && send_all(fd, payload, len) < 0)
        return false;
    return true;
}

static bool websocket_recv_frame(int fd, uint8_t *opcode, uint8_t *payload, size_t payload_cap,
                                 size_t *payload_len)
{
    uint8_t hdr[2];
    uint8_t mask[4];
    uint64_t len;

    if (recv(fd, hdr, sizeof(hdr), MSG_WAITALL) != (ssize_t) sizeof(hdr))
        return false;

    *opcode = hdr[0] & 0x0f;
    len = hdr[1] & 0x7f;
    if (len == 126)
    {
        uint8_t ext[2];
        if (recv(fd, ext, sizeof(ext), MSG_WAITALL) != (ssize_t) sizeof(ext))
            return false;
        len = ((uint64_t) ext[0] << 8) | ext[1];
    }
    else if (len == 127)
    {
        return false;
    }

    if (!(hdr[1] & 0x80))
        return false;
    if (len > payload_cap)
        return false;
    if (recv(fd, mask, sizeof(mask), MSG_WAITALL) != (ssize_t) sizeof(mask))
        return false;
    if (len > 0 && recv(fd, payload, len, MSG_WAITALL) != (ssize_t) len)
        return false;

    for (uint64_t i = 0; i < len; i++)
        payload[i] ^= mask[i & 3u];

    *payload_len = (size_t) len;
    return true;
}

static bool extract_json_string(const char *json, const char *key, char *out, size_t out_len)
{
    char pattern[64];
    char *start;
    char *end;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    start = strstr((char *) json, pattern);
    if (!start)
        return false;
    start = strchr(start + strlen(pattern), ':');
    if (!start)
        return false;
    start = strchr(start, '"');
    if (!start)
        return false;
    start++;
    end = strchr(start, '"');
    if (!end)
        return false;

    snprintf(out, out_len, "%.*s", (int) (end - start), start);
    return true;
}

static bool extract_json_int(const char *json, const char *key, long *out)
{
    char pattern[64];
    char *start;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    start = strstr((char *) json, pattern);
    if (!start)
        return false;
    start = strchr(start + strlen(pattern), ':');
    if (!start)
        return false;
    start++;
    while (*start == ' ' || *start == '\t')
        start++;
    *out = strtol(start, NULL, 10);
    return true;
}

static bool extract_json_int_any(const char *json, const char *key,
                                 const char *fallback_key, long *out)
{
    return extract_json_int(json, key, out) || extract_json_int(json, fallback_key, out);
}

static bool extract_json_string_any(const char *json, const char *key,
                                    const char *fallback_key,
                                    char *out, size_t out_len)
{
    return extract_json_string(json, key, out, out_len) ||
           extract_json_string(json, fallback_key, out, out_len);
}

static void send_json_response(ws_client *client, const char *json)
{
    if (!websocket_send_frame(client->fd, WS_OPCODE_TEXT, json, strlen(json)))
        close_client(client);
}

static void send_cmd_result(ws_client *client, const char *cmd, bool ok, const char *detail)
{
    char json[256];

    if (detail)
        snprintf(json, sizeof(json), "{\"ok\":%s,\"cmd\":\"%s\",\"status\":\"%s\"}",
                 ok ? "true" : "false", cmd, detail);
    else
        snprintf(json, sizeof(json), "{\"ok\":%s,\"cmd\":\"%s\"}",
                 ok ? "true" : "false", cmd);
    send_json_response(client, json);
}

static void send_cmd_error(ws_client *client, const char *cmd, const char *error)
{
    char json[256];

    snprintf(json, sizeof(json), "{\"ok\":false,\"cmd\":\"%s\",\"error\":\"%s\"}",
             cmd, error);
    send_json_response(client, json);
}

static void send_value_number(ws_client *client, const char *cmd, long long value)
{
    char json[256];

    snprintf(json, sizeof(json), "{\"ok\":true,\"cmd\":\"%s\",\"value\":%lld}",
             cmd, value);
    send_json_response(client, json);
}

static void send_value_string(ws_client *client, const char *cmd, const char *value)
{
    char json[256];

    snprintf(json, sizeof(json), "{\"ok\":true,\"cmd\":\"%s\",\"value\":\"%s\"}",
             cmd, value);
    send_json_response(client, json);
}

static void build_status_json(radio *radio_h, char *json, size_t json_len)
{
    uint32_t active = radio_h->profile_active_idx;
    if (active >= radio_h->profiles_count)
        active = 0;

    snprintf(json, json_len,
             "{\"type\":\"state\",\"profile\":%u,\"frequency\":%u,"
              "\"mode\":\"%s\",\"tx\":%s,\"bitrate\":%u,\"snr\":%d,"
              "\"bytes_transmitted\":%u,\"bytes_received\":%u,"
              "\"system_is_connected\":%s,\"system_is_ok\":%s,"
              "\"bfo\":%u,\"serial\":%u,\"step_size\":%u,\"tone\":%s,"
              "\"reflected_threshold\":%u,\"timeout\":%d,"
              "\"recording_rx\":%s,\"recording_tx\":%s,"
              "\"audio_sample_rate\":%u,\"message_available\":%s,"
              "\"backend\":\"%s\",\"digital_voice\":%s,"
              "\"pipeline\":\"%s\",\"pipeline_mode\":\"%s\","
              "\"pipeline_media\":\"%s\",\"pipeline_runtime\":\"%s\","
              "\"stream_rx_audio\":%s,\"stream_tx_audio\":%s,"
              "\"stream_spectrum\":%s,\"stream_recording\":%s,"
              "\"audio_bridge\":%s}",
              active,
              radio_h->profiles[active].freq,
              mode_to_string(radio_h->profiles[active].mode),
             radio_h->txrx_state == IN_TX ? "true" : "false",
             radio_h->bitrate,
             radio_h->snr,
             radio_h->bytes_transmitted,
             radio_h->bytes_received,
             radio_h->system_is_connected ? "true" : "false",
             radio_h->system_is_ok ? "true" : "false",
             radio_h->bfo_frequency,
             radio_h->serial_number,
             radio_h->step_size,
             radio_h->tone_generation ? "true" : "false",
             radio_h->reflected_threshold,
             radio_h->profile_timeout,
              radio_h->rx_recording.active ? "true" : "false",
              radio_h->tx_recording.active ? "true" : "false",
              radio_h->audio_sample_rate,
              radio_h->message_available ? "true" : "false",
              backend_to_string(radio_h->backend_kind),
              radio_h->profiles[active].digital_voice ? "true" : "false",
              radio_pipeline_name(radio_h),
              radio_pipeline_domain_name(radio_h),
              radio_pipeline_media_owner_name(radio_h),
              radio_pipeline_runtime_name(radio_h),
              radio_pipeline_supports_websocket_rx_audio(radio_h) ? "true" : "false",
              radio_pipeline_supports_websocket_tx_audio(radio_h) ? "true" : "false",
              (radio_pipeline_supports_spectrum(radio_h, false) ||
               radio_pipeline_supports_spectrum(radio_h, true)) ? "true" : "false",
              (radio_pipeline_has_capability(radio_h, RADIO_PIPELINE_CAP_RX_RECORDING) ||
               radio_pipeline_has_capability(radio_h, RADIO_PIPELINE_CAP_TX_RECORDING)) ?
              "true" : "false",
              radio_pipeline_uses_daemon_audio_bridge(radio_h) ? "true" : "false");
}

static long normalize_profile(radio *radio_h, const char *payload)
{
    long profile = radio_h->profile_active_idx;

    extract_json_int(payload, "profile", &profile);
    if (profile < 0 || profile >= (long) radio_h->profiles_count)
        profile = radio_h->profile_active_idx;

    return profile;
}

static void handle_ws_command(radio *radio_h, ws_client *client, const char *payload)
{
    char cmd[64];
    char message[RADIO_MESSAGE_MAX];
    char status_json[1024];
    long value;
    long profile = normalize_profile(radio_h, payload);

    if (!extract_json_string(payload, "cmd", cmd, sizeof(cmd)))
    {
        send_json_response(client, "{\"ok\":false,\"error\":\"missing cmd\"}");
        return;
    }

    if (!strcmp(cmd, "get_state"))
    {
        build_status_json(radio_h, status_json, sizeof(status_json));
        send_json_response(client, status_json);
        return;
    }

    if (!strcmp(cmd, "ptt_on"))
    {
        if (radio_h->swr_protection_enabled)
            send_cmd_result(client, cmd, false, "SWR");
        else if (radio_h->txrx_state == IN_TX)
            send_cmd_result(client, cmd, false, "NOK");
        else
        {
            radio_backend_set_txrx_state(radio_h, IN_TX);
            send_cmd_result(client, cmd, true, "OK");
        }
        return;
    }

    if (!strcmp(cmd, "ptt_off"))
    {
        if (radio_h->swr_protection_enabled)
            send_cmd_result(client, cmd, false, "SWR");
        else if (radio_h->txrx_state == IN_RX)
            send_cmd_result(client, cmd, false, "NOK");
        else
        {
            radio_backend_set_txrx_state(radio_h, IN_RX);
            send_cmd_result(client, cmd, true, "OK");
        }
        return;
    }

    if (!strcmp(cmd, "get_profile"))
    {
        send_value_number(client, cmd, radio_h->profile_active_idx);
        return;
    }

    if (!strcmp(cmd, "set_profile"))
    {
        if (!extract_json_int_any(payload, "profile", "value", &value))
        {
            send_cmd_error(client, cmd, "missing profile");
            return;
        }
        if (value < 0 || value >= (long) radio_h->profiles_count)
        {
            send_cmd_error(client, cmd, "invalid profile");
            return;
        }
        radio_backend_set_profile(radio_h, (uint32_t) value);
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_frequency"))
    {
        send_value_number(client, cmd, radio_h->profiles[profile].freq);
        return;
    }

    if (!strcmp(cmd, "set_frequency") &&
        extract_json_int_any(payload, "frequency", "value", &value))
    {
        radio_backend_set_frequency(radio_h, (uint32_t) value, (uint32_t) profile);
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_mode"))
    {
        send_value_string(client, cmd, mode_to_string(radio_h->profiles[profile].mode));
        return;
    }

    if (!strcmp(cmd, "set_mode"))
    {
        char mode[16];
        uint16_t mode_v = MODE_USB;
        if (!extract_json_string_any(payload, "mode", "value", mode, sizeof(mode)))
        {
            send_cmd_error(client, cmd, "missing mode");
            return;
        }
        if (!strcasecmp(mode, "LSB"))
            mode_v = MODE_LSB;
        else if (!strcasecmp(mode, "CW"))
            mode_v = MODE_CW;
        radio_backend_set_mode(radio_h, mode_v, (uint32_t) profile);
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_power"))
    {
        send_value_number(client, cmd, radio_h->profiles[profile].power_level_percentage);
        return;
    }

    if (!strcmp(cmd, "set_power") && extract_json_int_any(payload, "power", "value", &value))
    {
        if (value < 0 || value > 100)
        {
            send_cmd_error(client, cmd, "power out of range");
            return;
        }
        radio_backend_set_power_level(radio_h, (uint16_t) value, (uint32_t) profile);
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_volume"))
    {
        send_value_number(client, cmd, radio_h->profiles[profile].speaker_level);
        return;
    }

    if (!strcmp(cmd, "set_volume") && extract_json_int_any(payload, "volume", "value", &value))
    {
        if (value < 0 || value > 100)
        {
            send_cmd_error(client, cmd, "volume out of range");
            return;
        }
        radio_backend_set_speaker_volume(radio_h, (uint32_t) value, (uint32_t) profile);
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_digital_voice"))
    {
        send_value_string(client, cmd, radio_h->profiles[profile].digital_voice ? "ON" : "OFF");
        return;
    }

    if (!strcmp(cmd, "set_digital_voice") &&
        extract_json_int_any(payload, "enabled", "value", &value))
    {
        radio_backend_set_digital_voice(radio_h, value != 0, (uint32_t) profile);
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_txrx_status"))
    {
        send_value_string(client, cmd, radio_h->txrx_state == IN_TX ? "INTX" : "INRX");
        return;
    }

    if (!strcmp(cmd, "get_protection_status"))
    {
        send_value_string(client, cmd,
                          radio_h->swr_protection_enabled ? "PROTECTION_ON"
                                                          : "PROTECTION_OFF");
        return;
    }

    if (!strcmp(cmd, "reset_protection"))
    {
        radio_h->swr_protection_enabled = false;
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "reset_timeout"))
    {
        radio_backend_reset_timeout_timer();
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_bfo"))
    {
        send_value_number(client, cmd, radio_h->bfo_frequency);
        return;
    }

    if (!strcmp(cmd, "set_bfo") && extract_json_int_any(payload, "frequency", "value", &value))
    {
        radio_backend_set_bfo(radio_h, (uint32_t) value);
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_fwd"))
    {
        send_value_number(client, cmd, radio_backend_get_fwd_power(radio_h));
        return;
    }

    if (!strcmp(cmd, "get_ref"))
    {
        send_value_number(client, cmd, radio_backend_get_swr(radio_h));
        return;
    }

    if (!strcmp(cmd, "get_led_status"))
    {
        send_value_string(client, cmd, radio_h->system_is_ok ? "LED_ON" : "LED_OFF");
        return;
    }

    if (!strcmp(cmd, "set_led_status") &&
        extract_json_int_any(payload, "enabled", "value", &value))
    {
        radio_h->system_is_ok = value != 0;
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_connected_status"))
    {
        send_value_string(client, cmd,
                          radio_h->system_is_connected ? "LED_ON" : "LED_OFF");
        return;
    }

    if (!strcmp(cmd, "set_connected_status") &&
        extract_json_int_any(payload, "enabled", "value", &value))
    {
        if (!value)
        {
            pthread_mutex_lock(&radio_h->message_mutex);
            radio_h->message[0] = '\0';
            pthread_mutex_unlock(&radio_h->message_mutex);
            radio_h->message_available = true;
        }
        radio_h->system_is_connected = value != 0;
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_serial"))
    {
        send_value_number(client, cmd, radio_h->serial_number);
        return;
    }

    if (!strcmp(cmd, "set_serial") && extract_json_int_any(payload, "serial", "value", &value))
    {
        radio_backend_set_serial(radio_h, (uint32_t) value);
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_freqstep"))
    {
        send_value_number(client, cmd, radio_h->step_size);
        return;
    }

    if (!strcmp(cmd, "set_freqstep") &&
        extract_json_int_any(payload, "step_size", "value", &value))
    {
        radio_backend_set_step_size(radio_h, (uint32_t) value);
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_tone"))
    {
        send_value_number(client, cmd, radio_h->tone_generation ? 1 : 0);
        return;
    }

    if (!strcmp(cmd, "set_tone") && extract_json_int_any(payload, "tone", "value", &value))
    {
        radio_backend_set_tone_generation(radio_h, value != 0);
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_timeout"))
    {
        send_value_number(client, cmd, radio_h->profile_timeout);
        return;
    }

    if (!strcmp(cmd, "set_timeout") &&
        extract_json_int_any(payload, "timeout", "value", &value))
    {
        radio_backend_set_profile_timeout(radio_h, (int32_t) value);
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_ref_threshold"))
    {
        send_value_number(client, cmd, radio_h->reflected_threshold);
        return;
    }

    if (!strcmp(cmd, "set_ref_threshold") &&
        extract_json_int_any(payload, "ref_threshold", "value", &value))
    {
        radio_backend_set_reflected_threshold(radio_h, (uint32_t) value);
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_bitrate"))
    {
        send_value_number(client, cmd, radio_h->bitrate);
        return;
    }

    if (!strcmp(cmd, "set_bitrate") &&
        extract_json_int_any(payload, "bitrate", "value", &value))
    {
        radio_h->bitrate = (uint32_t) value;
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_snr"))
    {
        send_value_number(client, cmd, radio_h->snr);
        return;
    }

    if (!strcmp(cmd, "set_snr") && extract_json_int_any(payload, "snr", "value", &value))
    {
        radio_h->snr = (int32_t) value;
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_bytes_rx"))
    {
        send_value_number(client, cmd, radio_h->bytes_received);
        return;
    }

    if (!strcmp(cmd, "set_bytes_rx") &&
        extract_json_int_any(payload, "bytes", "value", &value))
    {
        radio_h->bytes_received = (uint32_t) value;
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_bytes_tx"))
    {
        send_value_number(client, cmd, radio_h->bytes_transmitted);
        return;
    }

    if (!strcmp(cmd, "set_bytes_tx") &&
        extract_json_int_any(payload, "bytes", "value", &value))
    {
        radio_h->bytes_transmitted = (uint32_t) value;
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_message"))
    {
        pthread_mutex_lock(&radio_h->message_mutex);
        snprintf(message, sizeof(message), "%s", radio_h->message);
        pthread_mutex_unlock(&radio_h->message_mutex);
        snprintf(status_json, sizeof(status_json),
                 "{\"ok\":true,\"cmd\":\"%s\",\"message\":\"%s\",\"message_available\":%s}",
                 cmd, message, radio_h->message_available ? "true" : "false");
        send_json_response(client, status_json);
        return;
    }

    if (!strcmp(cmd, "set_radio_defaults"))
    {
        radio_h->cfg_radio_dirty = true;
        radio_h->cfg_user_dirty = true;
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "radio_reset"))
    {
        send_cmd_result(client, cmd, true, "OK");
        shutdown_ = true;
        return;
    }

    if (!strcmp(cmd, "start_recording"))
    {
        char stream[16];
        if (!extract_json_string_any(payload, "stream", "value", stream, sizeof(stream)))
            snprintf(stream, sizeof(stream), "both");
        if (radio_media_start_recording(radio_h, stream))
            send_cmd_result(client, cmd, true, "OK");
        else
            send_cmd_error(client, cmd, "recording start failed");
        return;
    }

    if (!strcmp(cmd, "stop_recording"))
    {
        char stream[16];
        if (!extract_json_string_any(payload, "stream", "value", stream, sizeof(stream)))
            snprintf(stream, sizeof(stream), "both");
        radio_media_stop_recording(radio_h, stream);
        send_cmd_result(client, cmd, true, "OK");
        return;
    }

    send_cmd_error(client, cmd, "unsupported cmd");
}

static void handle_client_frame(radio *radio_h, ws_client *client)
{
    uint8_t opcode;
    uint8_t payload[8193];
    size_t payload_len = 0;

    if (!websocket_recv_frame(client->fd, &opcode, payload, sizeof(payload), &payload_len))
    {
        close_client(client);
        return;
    }

    if (opcode == WS_OPCODE_CLOSE)
    {
        close_client(client);
        return;
    }

    if (opcode == WS_OPCODE_PING)
    {
        if (!websocket_send_frame(client->fd, WS_OPCODE_PONG, payload, payload_len))
            close_client(client);
        return;
    }

    if (opcode == WS_OPCODE_TEXT)
    {
        payload[payload_len] = '\0';
        handle_ws_command(radio_h, client, (const char *) payload);
        return;
    }

    if (opcode == WS_OPCODE_BINARY && payload_len > 1 && payload[0] == WS_STREAM_RX_AUDIO)
    {
        if (!radio_pipeline_supports_websocket_tx_audio(radio_h))
            return;
        size_t nsamples = (payload_len - 1) / sizeof(int16_t);
        radio_media_push_tx_audio(radio_h, (const int16_t *) (payload + 1), nsamples);
        return;
    }
}

static void broadcast_status(websocket_ctx *ctx)
{
    char json[1024];
    time_t now = time(NULL);

    build_status_json(ctx->radio_h, json, sizeof(json));
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++)
    {
        ws_client *client = &ctx->clients[i];
        if (client->fd < 0)
            continue;
        if (client->last_status == now)
            continue;
        if (!websocket_send_frame(client->fd, WS_OPCODE_TEXT, json, strlen(json)))
            close_client(client);
        else
            client->last_status = now;
    }
}

static void broadcast_rx_audio(websocket_ctx *ctx)
{
    int16_t samples[WS_RX_CHUNK_SAMPLES];
    uint8_t frame[1 + sizeof(samples)];

    if (!radio_pipeline_supports_websocket_rx_audio(ctx->radio_h))
        return;

    size_t nsamples = radio_media_pop_rx_audio(ctx->radio_h, samples, WS_RX_CHUNK_SAMPLES);

    if (nsamples == 0)
        return;

    frame[0] = WS_STREAM_RX_AUDIO;
    memcpy(frame + 1, samples, nsamples * sizeof(int16_t));

    for (size_t i = 0; i < WS_MAX_CLIENTS; i++)
    {
        ws_client *client = &ctx->clients[i];
        if (client->fd < 0)
            continue;
        if (!websocket_send_frame(client->fd, WS_OPCODE_BINARY, frame,
                                  1 + (nsamples * sizeof(int16_t))))
            close_client(client);
    }
}

static void broadcast_spectrum(websocket_ctx *ctx, bool tx)
{
    float bins[WATERFALL_BINS];
    uint32_t seq = 0;
    uint32_t sample_rate = 0;
    uint8_t frame[1 + sizeof(sample_rate) + sizeof(uint16_t) + sizeof(bins)];
    uint16_t nbins = WATERFALL_BINS;

    if (!radio_media_get_spectrum(ctx->radio_h, tx, bins, WATERFALL_BINS, &seq, &sample_rate))
        return;

    frame[0] = tx ? WS_STREAM_TX_SPECTRUM : WS_STREAM_RX_SPECTRUM;
    memcpy(frame + 1, &sample_rate, sizeof(sample_rate));
    memcpy(frame + 1 + sizeof(sample_rate), &nbins, sizeof(nbins));
    memcpy(frame + 1 + sizeof(sample_rate) + sizeof(nbins), bins, sizeof(bins));

    for (size_t i = 0; i < WS_MAX_CLIENTS; i++)
    {
        ws_client *client = &ctx->clients[i];
        uint32_t *last_seq = tx ? &client->last_tx_spectrum_seq : &client->last_rx_spectrum_seq;
        if (client->fd < 0 || *last_seq == seq)
            continue;
        if (!websocket_send_frame(client->fd, WS_OPCODE_BINARY, frame, sizeof(frame)))
            close_client(client);
        else
            *last_seq = seq;
    }
}

static void accept_client(websocket_ctx *ctx)
{
    int fd = accept(ctx->server_fd, NULL, NULL);
    char status_json[1024];

    if (fd < 0)
        return;

    if (!websocket_handshake(fd))
    {
        close(fd);
        return;
    }

    for (size_t i = 0; i < WS_MAX_CLIENTS; i++)
    {
        if (ctx->clients[i].fd < 0)
        {
            ctx->clients[i].fd = fd;
            ctx->clients[i].last_status = 0;
            ctx->clients[i].last_rx_spectrum_seq = 0;
            ctx->clients[i].last_tx_spectrum_seq = 0;
            websocket_send_frame(fd, WS_OPCODE_TEXT,
                                 "{\"type\":\"hello\",\"api\":\"sbitx_client_compatible\","
                                 "\"audio_binary_type\":1,"
                                 "\"rx_spectrum_binary_type\":2,"
                                 "\"tx_spectrum_binary_type\":3}",
                                 strlen("{\"type\":\"hello\",\"api\":\"sbitx_client_compatible\","
                                        "\"audio_binary_type\":1,"
                                        "\"rx_spectrum_binary_type\":2,"
                                        "\"tx_spectrum_binary_type\":3}"));
            build_status_json(ctx->radio_h, status_json, sizeof(status_json));
            websocket_send_frame(fd, WS_OPCODE_TEXT, status_json, strlen(status_json));
            return;
        }
    }

    close(fd);
}

static void *websocket_thread(void *ctx_v)
{
    websocket_ctx *ctx = (websocket_ctx *) ctx_v;

    while (!shutdown_)
    {
        fd_set readfds;
        int max_fd = ctx->server_fd;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };

        FD_ZERO(&readfds);
        FD_SET(ctx->server_fd, &readfds);

        for (size_t i = 0; i < WS_MAX_CLIENTS; i++)
        {
            if (ctx->clients[i].fd >= 0)
            {
                FD_SET(ctx->clients[i].fd, &readfds);
                if (ctx->clients[i].fd > max_fd)
                    max_fd = ctx->clients[i].fd;
            }
        }

        if (select(max_fd + 1, &readfds, NULL, NULL, &tv) < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        if (FD_ISSET(ctx->server_fd, &readfds))
            accept_client(ctx);

        for (size_t i = 0; i < WS_MAX_CLIENTS; i++)
        {
            if (ctx->clients[i].fd >= 0 && FD_ISSET(ctx->clients[i].fd, &readfds))
                handle_client_frame(ctx->radio_h, &ctx->clients[i]);
        }

        broadcast_status(ctx);
        broadcast_rx_audio(ctx);
        broadcast_spectrum(ctx, false);
        broadcast_spectrum(ctx, true);
    }

    for (size_t i = 0; i < WS_MAX_CLIENTS; i++)
        close_client(&ctx->clients[i]);
    return NULL;
}

bool radio_websocket_init(radio *radio_h, pthread_t *websocket_tid)
{
    memset(&ws_ctx, 0, sizeof(ws_ctx));
    ws_ctx.radio_h = radio_h;
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++)
        ws_ctx.clients[i].fd = -1;

    if (!radio_h->enable_websocket)
        return true;

    ws_ctx.server_fd = create_server_socket(radio_h->websocket_bind);
    if (ws_ctx.server_fd < 0)
        return false;

    if (pthread_create(websocket_tid, NULL, websocket_thread, &ws_ctx) != 0)
    {
        close(ws_ctx.server_fd);
        ws_ctx.server_fd = -1;
        return false;
    }

    return true;
}

void radio_websocket_shutdown(pthread_t *websocket_tid)
{
    if (!ws_ctx.radio_h || !ws_ctx.radio_h->enable_websocket)
        return;

    if (ws_ctx.server_fd >= 0)
        close(ws_ctx.server_fd);
    pthread_join(*websocket_tid, NULL);
}
