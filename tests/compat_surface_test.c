#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "radio.h"
#include "radio_pipeline.h"
#include "include/radio_cmds.h"
#include "include/sbitx_io.h"

_Atomic bool shutdown_ = false;

static struct {
    uint32_t frequency;
    uint16_t mode;
    uint32_t speaker_level;
    uint16_t power_level;
    uint32_t step_size;
    uint32_t bfo;
    uint32_t serial;
    uint32_t ref_threshold;
    int32_t timeout;
    bool digital_voice;
    bool tone_generation;
    bool txrx_state;
    uint32_t profile;
    bool set_profile_called;
} backend_call;

static struct {
    size_t nsamples;
    int16_t samples[32];
} pushed_tx_audio;

static struct {
    bool available;
    int16_t samples[256];
    size_t nsamples;
} queued_rx_audio;

static struct {
    bool available;
    bool tx;
    float bins[WATERFALL_BINS];
    uint32_t seq;
    uint32_t sample_rate;
} queued_spectrum;

static bool recording_start_ok = true;
static char last_recording_stream[16];
static bool timeout_reset_called;

void radio_backend_set_frequency(radio *radio_h, uint32_t frequency, uint32_t profile)
{
    backend_call.frequency = frequency;
    backend_call.profile = profile;
    if (profile < radio_h->profiles_count)
        radio_h->profiles[profile].freq = frequency;
}

void radio_backend_set_mode(radio *radio_h, uint16_t mode, uint32_t profile)
{
    backend_call.mode = mode;
    backend_call.profile = profile;
    if (profile < radio_h->profiles_count)
        radio_h->profiles[profile].mode = mode;
}

void radio_backend_set_txrx_state(radio *radio_h, bool txrx_state)
{
    backend_call.txrx_state = txrx_state;
    radio_h->txrx_state = txrx_state;
}

void radio_backend_set_bfo(radio *radio_h, uint32_t frequency)
{
    backend_call.bfo = frequency;
    radio_h->bfo_frequency = frequency;
}

void radio_backend_set_reflected_threshold(radio *radio_h, uint32_t ref_threshold)
{
    backend_call.ref_threshold = ref_threshold;
    radio_h->reflected_threshold = ref_threshold;
}

void radio_backend_set_speaker_volume(radio *radio_h, uint32_t speaker_level, uint32_t profile)
{
    backend_call.speaker_level = speaker_level;
    backend_call.profile = profile;
    if (profile < radio_h->profiles_count)
        radio_h->profiles[profile].speaker_level = speaker_level;
}

void radio_backend_set_serial(radio *radio_h, uint32_t serial)
{
    backend_call.serial = serial;
    radio_h->serial_number = serial;
}

void radio_backend_set_profile_timeout(radio *radio_h, int32_t timeout)
{
    backend_call.timeout = timeout;
    radio_h->profile_timeout = timeout;
}

void radio_backend_set_power_level(radio *radio_h, uint16_t power_level, uint32_t profile)
{
    backend_call.power_level = power_level;
    backend_call.profile = profile;
    if (profile < radio_h->profiles_count)
        radio_h->profiles[profile].power_level_percentage = power_level;
}

void radio_backend_set_digital_voice(radio *radio_h, bool digital_voice, uint32_t profile)
{
    backend_call.digital_voice = digital_voice;
    backend_call.profile = profile;
    if (profile < radio_h->profiles_count)
        radio_h->profiles[profile].digital_voice = digital_voice;
    radio_pipeline_refresh(radio_h);
}

void radio_backend_set_step_size(radio *radio_h, uint32_t step_size)
{
    backend_call.step_size = step_size;
    radio_h->step_size = step_size;
}

void radio_backend_set_tone_generation(radio *radio_h, bool tone_generation)
{
    backend_call.tone_generation = tone_generation;
    radio_h->tone_generation = tone_generation;
}

void radio_backend_set_profile(radio *radio_h, uint32_t profile)
{
    backend_call.profile = profile;
    backend_call.set_profile_called = true;
    radio_h->profile_active_idx = profile;
    radio_pipeline_refresh(radio_h);
}

uint32_t radio_backend_get_fwd_power(radio *radio_h)
{
    return radio_h->fwd_power;
}

uint32_t radio_backend_get_swr(radio *radio_h)
{
    return radio_h->ref_power;
}

void radio_backend_reset_timeout_timer(void)
{
    timeout_reset_called = true;
}

void radio_media_push_tx_audio(radio *radio_h, const int16_t *samples, size_t nsamples)
{
    (void) radio_h;
    pushed_tx_audio.nsamples = nsamples > 32 ? 32 : nsamples;
    memcpy(pushed_tx_audio.samples, samples, pushed_tx_audio.nsamples * sizeof(samples[0]));
}

size_t radio_media_pop_rx_audio(radio *radio_h, int16_t *samples, size_t max_samples)
{
    size_t to_copy;

    (void) radio_h;
    if (!queued_rx_audio.available)
        return 0;

    to_copy = queued_rx_audio.nsamples < max_samples ? queued_rx_audio.nsamples : max_samples;
    memcpy(samples, queued_rx_audio.samples, to_copy * sizeof(samples[0]));
    queued_rx_audio.available = false;
    return to_copy;
}

bool radio_media_start_recording(radio *radio_h, const char *stream_name)
{
    (void) radio_h;
    snprintf(last_recording_stream, sizeof(last_recording_stream), "%s", stream_name);
    return recording_start_ok;
}

bool radio_media_stop_recording(radio *radio_h, const char *stream_name)
{
    (void) radio_h;
    snprintf(last_recording_stream, sizeof(last_recording_stream), "%s", stream_name);
    return true;
}

bool radio_media_get_spectrum(radio *radio_h, bool tx, float *out_bins, size_t bins,
                              uint32_t *seq, uint32_t *sample_rate)
{
    (void) radio_h;

    if (!queued_spectrum.available || queued_spectrum.tx != tx || bins < WATERFALL_BINS)
        return false;

    memcpy(out_bins, queued_spectrum.bins, sizeof(queued_spectrum.bins));
    *seq = queued_spectrum.seq;
    *sample_rate = queued_spectrum.sample_rate;
    queued_spectrum.available = false;
    return true;
}

bool shm_is_created(key_t key, size_t size)
{
    (void) key;
    (void) size;
    return false;
}

bool shm_create(key_t key, size_t size)
{
    (void) key;
    (void) size;
    return true;
}

bool shm_destroy(key_t key, size_t size)
{
    (void) key;
    (void) size;
    return true;
}

void *shm_attach(key_t key, size_t size)
{
    static controller_conn connector;

    (void) key;
    (void) size;
    return &connector;
}

#include "../radio_pipeline.c"
#include "../radio_shm.c"
#include "../radio_websocket.c"

typedef struct {
    uint8_t opcode;
    size_t payload_len;
    uint8_t payload[4096];
} frame_result;

static void reset_backend_call(void)
{
    memset(&backend_call, 0, sizeof(backend_call));
    memset(&pushed_tx_audio, 0, sizeof(pushed_tx_audio));
    memset(&queued_rx_audio, 0, sizeof(queued_rx_audio));
    memset(&queued_spectrum, 0, sizeof(queued_spectrum));
    memset(last_recording_stream, 0, sizeof(last_recording_stream));
    timeout_reset_called = false;
}

static void init_test_radio(radio *radio_h, radio_backend_kind backend_kind, bool digital_voice)
{
    memset(radio_h, 0, sizeof(*radio_h));
    pthread_mutex_init(&radio_h->message_mutex, NULL);
    radio_h->profiles_count = 2;
    radio_h->profile_active_idx = 0;
    radio_h->profiles[0].freq = 7100000;
    radio_h->profiles[1].freq = 7200000;
    radio_h->profiles[0].mode = MODE_USB;
    radio_h->profiles[1].mode = MODE_USB;
    radio_h->profiles[0].speaker_level = 55;
    radio_h->profiles[1].speaker_level = 66;
    radio_h->profiles[0].power_level_percentage = 90;
    radio_h->profiles[1].power_level_percentage = 80;
    radio_h->profiles[0].digital_voice = false;
    radio_h->profiles[1].digital_voice = digital_voice;
    radio_h->backend_kind = backend_kind;
    radio_h->enable_audio_bridge = true;
    radio_h->audio_sample_rate = 8000;
    radio_h->step_size = 100;
    radio_h->profile_timeout = 30;
    radio_h->serial_number = 42;
    radio_h->reflected_threshold = 25;
    radio_h->fwd_power = 12;
    radio_h->ref_power = 13;
    radio_h->system_is_connected = true;
    radio_h->system_is_ok = true;
    radio_h->message_available = false;
    radio_pipeline_refresh(radio_h);
}

static void destroy_test_radio(radio *radio_h)
{
    pthread_mutex_destroy(&radio_h->message_mutex);
}

static void expect_contains(const char *haystack, const char *needle)
{
    assert(strstr(haystack, needle) != NULL);
}

static void read_exact(int fd, void *buf, size_t len)
{
    uint8_t *pos = buf;
    size_t total = 0;

    while (total < len)
    {
        ssize_t got = recv(fd, pos + total, len - total, 0);
        assert(got > 0);
        total += (size_t) got;
    }
}

static void read_http_response(int fd, char *buf, size_t buf_len)
{
    size_t used = 0;
    char last4[4] = {0};

    while (used + 1 < buf_len)
    {
        read_exact(fd, buf + used, 1);
        last4[0] = last4[1];
        last4[1] = last4[2];
        last4[2] = last4[3];
        last4[3] = buf[used];
        used++;
        if (used >= 4 && memcmp(last4, "\r\n\r\n", 4) == 0)
            break;
    }

    buf[used] = '\0';
}

static frame_result read_server_frame(int fd)
{
    frame_result frame;
    uint8_t hdr[2];
    uint64_t len;

    memset(&frame, 0, sizeof(frame));
    read_exact(fd, hdr, sizeof(hdr));
    frame.opcode = hdr[0] & 0x0f;
    len = hdr[1] & 0x7f;

    if (len == 126)
    {
        uint8_t ext[2];
        read_exact(fd, ext, sizeof(ext));
        len = ((uint64_t) ext[0] << 8) | ext[1];
    }
    else if (len == 127)
    {
        uint8_t ext[8];
        read_exact(fd, ext, sizeof(ext));
        len = 0;
        for (size_t i = 0; i < sizeof(ext); i++)
            len = (len << 8) | ext[i];
    }

    assert((hdr[1] & 0x80) == 0);
    assert(len < sizeof(frame.payload));
    frame.payload_len = (size_t) len;
    read_exact(fd, frame.payload, frame.payload_len);
    return frame;
}

static void send_client_frame(int fd, uint8_t opcode, const uint8_t *payload, size_t payload_len)
{
    uint8_t hdr[14];
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    uint8_t masked[4096];
    size_t hdr_len = 0;

    assert(payload_len < sizeof(masked));

    hdr[hdr_len++] = 0x80u | opcode;
    if (payload_len < 126)
    {
        hdr[hdr_len++] = 0x80u | (uint8_t) payload_len;
    }
    else
    {
        hdr[hdr_len++] = 0x80u | 126u;
        hdr[hdr_len++] = (uint8_t) ((payload_len >> 8) & 0xff);
        hdr[hdr_len++] = (uint8_t) (payload_len & 0xff);
    }
    memcpy(hdr + hdr_len, mask, sizeof(mask));
    hdr_len += sizeof(mask);

    for (size_t i = 0; i < payload_len; i++)
        masked[i] = payload[i] ^ mask[i & 3u];

    assert(send(fd, hdr, hdr_len, 0) == (ssize_t) hdr_len);
    if (payload_len > 0)
        assert(send(fd, masked, payload_len, 0) == (ssize_t) payload_len);
}

static void send_client_text_frame(int fd, const char *json)
{
    send_client_frame(fd, WS_OPCODE_TEXT, (const uint8_t *) json, strlen(json));
}

static void send_client_binary_frame(int fd, const uint8_t *payload, size_t payload_len)
{
    send_client_frame(fd, WS_OPCODE_BINARY, payload, payload_len);
}

static void set_socket_timeout(int fd)
{
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
}

static void test_shm_profile_and_message_semantics(void)
{
    radio radio_h;
    controller_conn connector;
    uint8_t cmd[5] = {0};
    uint8_t response[5] = {0};
    uint32_t value = 0;

    init_test_radio(&radio_h, RADIO_BACKEND_HAMLIB, false);
    memset(&connector, 0, sizeof(connector));
    connector_local = &connector;
    radio_h_shm = &radio_h;

    cmd[4] = CMD_GET_FREQ | (1u << 6);
    process_radio_command(cmd, response);
    memcpy(&value, response + 1, sizeof(value));
    assert(response[0] == CMD_RESP_GET_FREQ_ACK);
    assert(value == 7200000);

    memset(cmd, 0, sizeof(cmd));
    value = 7215000;
    memcpy(cmd, &value, sizeof(value));
    cmd[4] = CMD_SET_FREQ | (1u << 6);
    process_radio_command(cmd, response);
    assert(response[0] == CMD_RESP_ACK);
    assert(backend_call.frequency == 7215000);
    assert(backend_call.profile == 1);
    assert(radio_h.profiles[1].freq == 7215000);

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x03;
    cmd[4] = CMD_SET_MODE | (1u << 6);
    process_radio_command(cmd, response);
    assert(response[0] == CMD_RESP_ACK);
    assert(backend_call.mode == MODE_LSB);
    assert(radio_h.profiles[1].mode == MODE_LSB);

    pthread_mutex_lock(&radio_h.message_mutex);
    snprintf(radio_h.message, sizeof(radio_h.message), "%s", "stale");
    pthread_mutex_unlock(&radio_h.message_mutex);
    snprintf(connector.message, sizeof(connector.message), "%s", "stale");
    radio_h.system_is_connected = true;
    radio_h.message_available = false;
    connector.message_available = false;

    memset(cmd, 0, sizeof(cmd));
    cmd[4] = CMD_SET_CONNECTED_STATUS;
    process_radio_command(cmd, response);
    assert(response[0] == CMD_RESP_ACK);
    assert(!radio_h.system_is_connected);
    assert(radio_h.message_available);
    assert(connector.message_available);
    assert(radio_h.message[0] == '\0');
    assert(connector.message[0] == '\0');

    memset(cmd, 0, sizeof(cmd));
    cmd[4] = CMD_GET_DIGITAL_VOICE | (3u << 6);
    process_radio_command(cmd, response);
    assert(response[0] == CMD_RESP_WRONG_COMMAND);

    destroy_test_radio(&radio_h);
}

static void test_websocket_handshake_and_state_metadata(void)
{
    radio radio_h;
    websocket_ctx ctx;
    int listen_fd;
    int client_fd;
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    char http_response[512];
    frame_result hello;
    frame_result state;

    init_test_radio(&radio_h, RADIO_BACKEND_HAMLIB, false);
    memset(&ctx, 0, sizeof(ctx));

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listen_fd >= 0);
    set_socket_timeout(listen_fd);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(listen_fd, (struct sockaddr *) &addr, sizeof(addr)) == 0);
    assert(listen(listen_fd, 1) == 0);
    assert(getsockname(listen_fd, (struct sockaddr *) &addr, &addr_len) == 0);

    client_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(client_fd >= 0);
    set_socket_timeout(client_fd);
    assert(connect(client_fd, (struct sockaddr *) &addr, sizeof(addr)) == 0);
    assert(send(client_fd,
                "GET / HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n",
                154, 0) == 154);

    ctx.radio_h = &radio_h;
    ctx.server_fd = listen_fd;
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++)
        ctx.clients[i].fd = -1;

    accept_client(&ctx);
    read_http_response(client_fd, http_response, sizeof(http_response));
    expect_contains(http_response, "101 Switching Protocols");
    expect_contains(http_response, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    hello = read_server_frame(client_fd);
    state = read_server_frame(client_fd);

    assert(hello.opcode == WS_OPCODE_TEXT);
    hello.payload[hello.payload_len] = '\0';
    expect_contains((char *) hello.payload, "\"type\":\"hello\"");
    expect_contains((char *) hello.payload, "\"api\":\"sbitx_client_compatible\"");
    expect_contains((char *) hello.payload, "\"audio_binary_type\":1");
    expect_contains((char *) hello.payload, "\"rx_spectrum_binary_type\":2");
    expect_contains((char *) hello.payload, "\"tx_spectrum_binary_type\":3");

    assert(state.opcode == WS_OPCODE_TEXT);
    state.payload[state.payload_len] = '\0';
    expect_contains((char *) state.payload, "\"type\":\"state\"");
    expect_contains((char *) state.payload, "\"backend\":\"hamlib\"");
    expect_contains((char *) state.payload, "\"pipeline\":\"hamlib-analog\"");
    expect_contains((char *) state.payload, "\"stream_rx_audio\":true");
    expect_contains((char *) state.payload, "\"stream_tx_audio\":true");
    expect_contains((char *) state.payload, "\"audio_bridge\":true");

    close(ctx.clients[0].fd);
    close(client_fd);
    close(listen_fd);
    destroy_test_radio(&radio_h);
}

static void test_websocket_commands_and_media_semantics(void)
{
    radio radio_h;
    int sockets[2];
    ws_client client = {0};
    frame_result frame;
    uint8_t audio_payload[1 + (3 * sizeof(int16_t))];
    int16_t tx_audio[3] = {101, -202, 303};

    init_test_radio(&radio_h, RADIO_BACKEND_HAMLIB, false);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    set_socket_timeout(sockets[0]);
    set_socket_timeout(sockets[1]);
    client.fd = sockets[0];

    send_client_text_frame(sockets[1], "{\"cmd\":\"set_frequency\",\"profile\":1,\"value\":7234000}");
    handle_client_frame(&radio_h, &client);
    frame = read_server_frame(sockets[1]);
    frame.payload[frame.payload_len] = '\0';
    expect_contains((char *) frame.payload, "\"cmd\":\"set_frequency\"");
    expect_contains((char *) frame.payload, "\"status\":\"OK\"");
    assert(backend_call.frequency == 7234000);
    assert(backend_call.profile == 1);

    send_client_text_frame(sockets[1], "{\"cmd\":\"get_frequency\",\"profile\":1}");
    handle_client_frame(&radio_h, &client);
    frame = read_server_frame(sockets[1]);
    frame.payload[frame.payload_len] = '\0';
    expect_contains((char *) frame.payload, "\"cmd\":\"get_frequency\"");
    expect_contains((char *) frame.payload, "\"value\":7234000");

    send_client_text_frame(sockets[1], "{\"cmd\":\"set_profile\",\"value\":9}");
    handle_client_frame(&radio_h, &client);
    frame = read_server_frame(sockets[1]);
    frame.payload[frame.payload_len] = '\0';
    expect_contains((char *) frame.payload, "\"error\":\"invalid profile\"");

    audio_payload[0] = WS_STREAM_RX_AUDIO;
    memcpy(audio_payload + 1, tx_audio, sizeof(tx_audio));
    send_client_binary_frame(sockets[1], audio_payload, sizeof(audio_payload));
    handle_client_frame(&radio_h, &client);
    assert(pushed_tx_audio.nsamples == 3);
    assert(pushed_tx_audio.samples[0] == 101);
    assert(pushed_tx_audio.samples[1] == -202);
    assert(pushed_tx_audio.samples[2] == 303);

    radio_h.backend_kind = RADIO_BACKEND_HFSIGNALS;
    radio_h.profiles[0].digital_voice = true;
    radio_pipeline_refresh(&radio_h);
    pushed_tx_audio.nsamples = 0;
    send_client_binary_frame(sockets[1], audio_payload, sizeof(audio_payload));
    handle_client_frame(&radio_h, &client);
    assert(pushed_tx_audio.nsamples == 0);

    close(sockets[0]);
    close(sockets[1]);
    destroy_test_radio(&radio_h);
}

static void test_websocket_broadcast_media_frames(void)
{
    radio radio_h;
    websocket_ctx ctx;
    int sockets[2];
    frame_result frame;
    uint32_t sample_rate;
    uint16_t bins;
    int16_t rx_audio[2] = {111, -222};

    init_test_radio(&radio_h, RADIO_BACKEND_HAMLIB, false);
    memset(&ctx, 0, sizeof(ctx));
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    set_socket_timeout(sockets[0]);
    set_socket_timeout(sockets[1]);

    ctx.radio_h = &radio_h;
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++)
        ctx.clients[i].fd = -1;
    ctx.clients[0].fd = sockets[0];

    queued_rx_audio.available = true;
    queued_rx_audio.nsamples = 2;
    memcpy(queued_rx_audio.samples, rx_audio, sizeof(rx_audio));
    broadcast_rx_audio(&ctx);
    frame = read_server_frame(sockets[1]);
    assert(frame.opcode == WS_OPCODE_BINARY);
    assert(frame.payload[0] == WS_STREAM_RX_AUDIO);
    assert(memcmp(frame.payload + 1, rx_audio, sizeof(rx_audio)) == 0);

    queued_spectrum.available = true;
    queued_spectrum.tx = false;
    queued_spectrum.seq = 7;
    queued_spectrum.sample_rate = 8000;
    queued_spectrum.bins[0] = 1.25f;
    queued_spectrum.bins[WATERFALL_BINS - 1] = -2.5f;
    broadcast_spectrum(&ctx, false);
    frame = read_server_frame(sockets[1]);
    assert(frame.opcode == WS_OPCODE_BINARY);
    assert(frame.payload[0] == WS_STREAM_RX_SPECTRUM);
    memcpy(&sample_rate, frame.payload + 1, sizeof(sample_rate));
    memcpy(&bins, frame.payload + 1 + sizeof(sample_rate), sizeof(bins));
    assert(sample_rate == 8000);
    assert(bins == WATERFALL_BINS);

    close(sockets[0]);
    close(sockets[1]);
    destroy_test_radio(&radio_h);
}

static void test_state_json_reflects_hfsignals_pipeline(void)
{
    radio radio_h;
    char json[1024];

    init_test_radio(&radio_h, RADIO_BACKEND_HFSIGNALS, true);
    radio_h.profile_active_idx = 1;
    radio_pipeline_refresh(&radio_h);
    build_status_json(&radio_h, json, sizeof(json));
    expect_contains(json, "\"backend\":\"hfsignals\"");
    expect_contains(json, "\"digital_voice\":true");
    expect_contains(json, "\"pipeline\":\"hfsignals-radev2\"");
    expect_contains(json, "\"pipeline_media\":\"legacy-embedded\"");
    expect_contains(json, "\"pipeline_runtime\":\"embedded-backend\"");
    expect_contains(json, "\"stream_rx_audio\":false");
    expect_contains(json, "\"stream_tx_audio\":false");
    expect_contains(json, "\"audio_bridge\":false");
    destroy_test_radio(&radio_h);
}

int main(void)
{
    reset_backend_call();
    test_shm_profile_and_message_semantics();
    reset_backend_call();
    test_websocket_handshake_and_state_metadata();
    reset_backend_call();
    test_websocket_commands_and_media_semantics();
    reset_backend_call();
    test_websocket_broadcast_media_frames();
    reset_backend_call();
    test_state_json_reflects_hfsignals_pipeline();
    puts("compat_surface_test: ok");
    return 0;
}
