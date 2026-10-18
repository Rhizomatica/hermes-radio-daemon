/* ui_hub: modules, sinks and the routing between them, with no transport.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "unity.h"
#include "fff.h"
#include "ui_hub.h"

DEFINE_FFF_GLOBALS;
FAKE_VALUE_FUNC(int, mod_command, const char *, size_t, void *);
FAKE_VOID_FUNC(mod_connected, void *);

static const ui_hub_module mod = {
    .name = "mercury", .command_key = "command", .stream_type = UI_HUB_STREAM_MERCURY_SPECTRUM,
    .command = mod_command, .client_connected = mod_connected, .user = (void *) 0x1234,
};

/* A sink that records what it got. */
static char   got_text[512];
static int    ntext, nbinary;
static uint8_t got_bin[64];
static size_t got_bin_len;

static void sink_text(const char *json, size_t len, void *user)
{
    (void) user;
    snprintf(got_text, sizeof(got_text), "%.*s", (int) len, json);
    ntext++;
}

static void sink_binary(const uint8_t *frame, size_t len, void *user)
{
    (void) user;
    got_bin_len = len < sizeof(got_bin) ? len : sizeof(got_bin);
    memcpy(got_bin, frame, got_bin_len);
    nbinary++;
}

void setUp(void)
{
    RESET_FAKE(mod_command);
    RESET_FAKE(mod_connected);
    FFF_RESET_HISTORY();
    ntext = nbinary = 0;
    got_text[0] = '\0';
    got_bin_len = 0;
}

void tearDown(void)
{
    ui_hub_unregister_module("mercury");
    ui_hub_unregister_module("other");
}

static void test_register(void)
{
    TEST_ASSERT_TRUE(ui_hub_register_module(&mod));
    TEST_ASSERT_FALSE_MESSAGE(ui_hub_register_module(&mod), "a name registered twice");
    ui_hub_unregister_module("mercury");
    TEST_ASSERT_TRUE_MESSAGE(ui_hub_register_module(&mod), "registering again after unregistering");
    TEST_ASSERT_FALSE(ui_hub_register_module(NULL));
}

/* A message with the module's key goes to it, and the reply carries the
 * command's name and the outcome. */
static void test_command_routed_and_answered(void)
{
    char reply[256];
    const char *msg = "{\"command\":\"set_tx_gain\",\"value\":\"6\"}";

    ui_hub_register_module(&mod);
    mod_command_fake.return_val = 0;
    TEST_ASSERT_TRUE(ui_hub_command(msg, strlen(msg), reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_INT(1, mod_command_fake.call_count);
    TEST_ASSERT_EQUAL_STRING(msg, mod_command_fake.arg0_val);
    TEST_ASSERT_EQUAL_size_t(strlen(msg), mod_command_fake.arg1_val);
    TEST_ASSERT_EQUAL_PTR(mod.user, mod_command_fake.arg2_val);
    TEST_ASSERT_EQUAL_STRING("{\"command\":\"set_tx_gain\",\"status\":\"ok\"}", reply);

    mod_command_fake.return_val = 5;
    TEST_ASSERT_TRUE(ui_hub_command(msg, strlen(msg), reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("{\"command\":\"set_tx_gain\",\"status\":\"error\",\"code\":5}", reply);
}

/* The name a client sent is escaped before it goes back into JSON. */
static void test_reply_escapes_the_name(void)
{
    char reply[256];
    const char *msg = "{\"command\":\"a\\\"b\"}";       /* {"command":"a\"b"} */

    ui_hub_register_module(&mod);
    TEST_ASSERT_TRUE(ui_hub_command(msg, strlen(msg), reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("{\"command\":\"a\\\"b\",\"status\":\"ok\"}", reply);
}

/* Mercury's key with mercury not running: "no handler"; a message with
 * neither radiod's nor a module's key is not the hub's. */
static void test_no_handler_and_not_ours(void)
{
    char reply[256];
    const char *merc = "{\"command\":\"set_callsign\",\"value\":\"X\"}";
    const char *other = "{\"nocmd\":1}";

    TEST_ASSERT_TRUE(ui_hub_command(merc, strlen(merc), reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_STRING("{\"command\":\"set_callsign\",\"error\":\"no handler\"}", reply);
    TEST_ASSERT_FALSE(ui_hub_command(other, strlen(other), reply, sizeof(reply)));
    TEST_ASSERT_EQUAL_INT(0, mod_command_fake.call_count);
}

/* Broadcasts reach every sink; binary frames get the stream type byte;
 * a removed sink gets nothing more. */
static void test_broadcasts_to_sinks(void)
{
    ui_hub_sink sink = { .text = sink_text, .binary = sink_binary };
    int h = ui_hub_add_sink(&sink);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, h);

    ui_hub_broadcast_text("{\"type\":\"status\"}");
    TEST_ASSERT_EQUAL_INT(1, ntext);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"status\"}", got_text);

    const uint8_t frame[] = { 'Y', 'R', 'C', 'M', 1, 0 };
    ui_hub_broadcast_stream(UI_HUB_STREAM_MERCURY_SPECTRUM, frame, sizeof(frame));
    TEST_ASSERT_EQUAL_INT(1, nbinary);
    TEST_ASSERT_EQUAL_size_t(sizeof(frame) + 1, got_bin_len);
    TEST_ASSERT_EQUAL_HEX8(UI_HUB_STREAM_MERCURY_SPECTRUM, got_bin[0]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, got_bin + 1, sizeof(frame));

    ui_hub_remove_sink(h);
    ui_hub_broadcast_text("{\"type\":\"status\"}");
    ui_hub_broadcast_stream(4, frame, sizeof(frame));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, ntext, "a removed sink got text");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, nbinary, "a removed sink got binary");
}

static void test_hello_fields(void)
{
    char out[256];

    ui_hub_hello_fields(out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("\"modules\":[]", out);

    ui_hub_register_module(&mod);
    ui_hub_hello_fields(out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("\"modules\":[\"mercury\"],\"mercury_binary_type\":4", out);

    ui_hub_module plain = { .name = "other", .command_key = "op" };
    ui_hub_register_module(&plain);
    ui_hub_hello_fields(out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("\"modules\":[\"mercury\",\"other\"],\"mercury_binary_type\":4", out);

    char tiny[8];
    TEST_ASSERT_LESS_THAN_size_t(sizeof(tiny), ui_hub_hello_fields(tiny, sizeof(tiny)));
}

static void test_client_connected_reaches_modules(void)
{
    ui_hub_client_connected();
    TEST_ASSERT_EQUAL_INT(0, mod_connected_fake.call_count);
    ui_hub_register_module(&mod);
    ui_hub_client_connected();
    TEST_ASSERT_EQUAL_INT(1, mod_connected_fake.call_count);
    TEST_ASSERT_EQUAL_PTR(mod.user, mod_connected_fake.arg0_val);
}

/* Unregistering waits for a command in flight: once it returns, the module
 * can shut down (mercury_host.h asks for no UI call during shutdown). */
static atomic_bool in_command, command_done;

static int slow_command(const char *json, size_t len, void *user)
{
    (void) json; (void) len; (void) user;
    in_command = true;
    usleep(200000);
    command_done = true;
    return 0;
}

static void *send_command(void *arg)
{
    (void) arg;
    char reply[128];
    const char *msg = "{\"command\":\"slow\"}";
    ui_hub_command(msg, strlen(msg), reply, sizeof(reply));
    return NULL;
}

static void test_unregister_waits_for_command(void)
{
    ui_hub_module m = mod;
    m.command = slow_command;
    in_command = command_done = false;
    TEST_ASSERT_TRUE(ui_hub_register_module(&m));

    pthread_t t;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, send_command, NULL));
    while (!in_command)
        usleep(1000);
    ui_hub_unregister_module("mercury");
    bool done_when_unregistered = command_done;
    pthread_join(t, NULL);
    TEST_ASSERT_TRUE_MESSAGE(done_when_unregistered, "unregister returned with a command in flight");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_register);
    RUN_TEST(test_command_routed_and_answered);
    RUN_TEST(test_reply_escapes_the_name);
    RUN_TEST(test_no_handler_and_not_ours);
    RUN_TEST(test_broadcasts_to_sinks);
    RUN_TEST(test_hello_fields);
    RUN_TEST(test_client_connected_reaches_modules);
    RUN_TEST(test_unregister_waits_for_command);
    return UNITY_END();
}
