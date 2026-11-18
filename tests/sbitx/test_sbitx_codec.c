/* sbitx_codec: the WM8731 is found by its name among the kernel's I2C
 * devices (bus entries and other devices skipped, the address read as hex),
 * not found when absent, and a register write is encoded as the codec
 * expects (7-bit register, the value's ninth bit, its low byte).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "unity.h"
#include "sbitx/sbitx_codec.h"

#define CODEC_BUS   1
#define CODEC_ADDR  0x1a
#define ROOT_MAX    256
#define PATH_MAX_T  (ROOT_MAX + 64)

static char root[ROOT_MAX];

static void add_device(const char *entry, const char *name)
{
    char p[PATH_MAX_T];
    snprintf(p, sizeof(p), "%s/%s", root, entry);
    TEST_ASSERT_EQUAL_INT(0, mkdir(p, 0755));
    if (!name)
        return;
    snprintf(p, sizeof(p), "%s/%s/name", root, entry);
    FILE *f = fopen(p, "w");
    TEST_ASSERT_NOT_NULL(f);
    fprintf(f, "%s\n", name);
    fclose(f);
}

void setUp(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof(root), "%s/test_sbitx_codec.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    TEST_ASSERT_NOT_NULL(mkdtemp(root));
}

void tearDown(void)
{
    char cmd[PATH_MAX_T + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    TEST_ASSERT_EQUAL_INT(0, system(cmd));
}

void test_finds_the_wm8731_among_other_devices(void)
{
    add_device("i2c-1", NULL);                /* a bus entry, no name file */
    add_device("10-005d", "Goodix-TS");
    add_device("22-0068", "ds3231");
    add_device("1-001a", "wm8731");
    int bus = -1, addr = -1;
    TEST_ASSERT_TRUE(sbitx_codec_find(root, &bus, &addr));
    TEST_ASSERT_EQUAL_INT(CODEC_BUS, bus);
    TEST_ASSERT_EQUAL_HEX(CODEC_ADDR, addr);
}

void test_no_wm8731_is_not_found(void)
{
    add_device("i2c-1", NULL);
    add_device("10-005d", "Goodix-TS");
    int bus = -1, addr = -1;
    TEST_ASSERT_FALSE(sbitx_codec_find(root, &bus, &addr));
    TEST_ASSERT_FALSE(sbitx_codec_find("/nonexistent/i2c/devices", &bus, &addr));
}

void test_register_write_encoding(void)
{
    uint8_t b[2];
    sbitx_codec_encode(9, 0x001, b);          /* ACTIVE */
    TEST_ASSERT_EQUAL_HEX8(0x12, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, b[1]);
    sbitx_codec_encode(9, 0x000, b);
    TEST_ASSERT_EQUAL_HEX8(0x12, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, b[1]);
    sbitx_codec_encode(6, 0x19f, b);          /* the ninth bit rides on the first byte */
    TEST_ASSERT_EQUAL_HEX8(0x0d, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0x9f, b[1]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_finds_the_wm8731_among_other_devices);
    RUN_TEST(test_no_wm8731_is_not_found);
    RUN_TEST(test_register_write_encoding);
    return UNITY_END();
}
