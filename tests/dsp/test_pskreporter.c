/* pskreporter_ipfix: the reception report's bytes against
 * https://pskreporter.info/pskdev.html's examples, the receiver's
 * locator, which decoded callsigns are reported, how much one datagram
 * takes, and the once-an-hour rule.
 */
#include <string.h>

#include "unity.h"
#include "dsp/pskreporter_ipfix.h"

void setUp(void) {}
void tearDown(void) {}

#define HEADER_BYTES 16

/* The spec's options template B (receiverCallsign, receiverLocator,
 * decodingSoftware, antennaInformation), with our template id 0x4852. */
static const uint8_t RX_TEMPLATE[] = {
    0x00, 0x03, 0x00, 0x2C, 0x48, 0x52, 0x00, 0x04, 0x00, 0x01,
    0x80, 0x02, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
    0x80, 0x04, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
    0x80, 0x08, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
    0x80, 0x09, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
    0x00, 0x00,
};

/* Ours for the senders: S3's fields without iMD, plus senderLocator. */
static const uint8_t TX_TEMPLATE[] = {
    0x00, 0x02, 0x00, 0x3C, 0x48, 0x53, 0x00, 0x07,
    0x80, 0x01, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
    0x80, 0x05, 0x00, 0x04, 0x00, 0x00, 0x76, 0x8F,
    0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,
    0x80, 0x0A, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
    0x80, 0x03, 0xFF, 0xFF, 0x00, 0x00, 0x76, 0x8F,
    0x80, 0x0B, 0x00, 0x01, 0x00, 0x00, 0x76, 0x8F,
    0x00, 0x96, 0x00, 0x04,
};

/* The spec's receiver record for N1DQ, FN42hn, "Homebrew v5.6": its two
 * padding bytes are our empty antenna field and one padding byte. */
static const uint8_t RX_RECORD[] = {
    0x48, 0x52, 0x00, 0x20,
    0x04, 'N', '1', 'D', 'Q',
    0x06, 'F', 'N', '4', '2', 'h', 'n',
    0x0D, 'H', 'o', 'm', 'e', 'b', 'r', 'e', 'w', ' ', 'v', '5', '.', '6',
    0x00, 0x00,
};

static const pskr_receiver N1DQ = { "N1DQ", "FN42hn", "Homebrew v5.6", "" };

static void test_locator(void)
{
    char loc[PSKR_LOCATOR_MAX];
    TEST_ASSERT_TRUE(pskr_locator(42.56, -71.37, loc, sizeof loc));
    TEST_ASSERT_EQUAL_STRING("FN42hn", loc);
    TEST_ASSERT_TRUE(pskr_locator(0, 0, loc, sizeof loc));
    TEST_ASSERT_EQUAL_STRING("JJ00aa", loc);
    TEST_ASSERT_TRUE(pskr_locator(-90, -180, loc, sizeof loc));
    TEST_ASSERT_EQUAL_STRING("AA00aa", loc);
    TEST_ASSERT_TRUE(pskr_locator(90, 180, loc, sizeof loc));     /* the edges: the last subsquare */
    TEST_ASSERT_EQUAL_STRING("RR99xx", loc);
    TEST_ASSERT_TRUE(pskr_locator(-22.9, -43.2, loc, sizeof loc));
    TEST_ASSERT_EQUAL_STRING("GG87jc", loc);
    TEST_ASSERT_FALSE(pskr_locator(90.5, 0, loc, sizeof loc));
    TEST_ASSERT_EQUAL_STRING("", loc);
    TEST_ASSERT_FALSE(pskr_locator(0, 0, loc, PSKR_LOCATOR_LEN));  /* no room for the NUL */
}

static void test_calls(void)
{
    TEST_ASSERT_TRUE(pskr_call_ok("CR7BYQ"));
    TEST_ASSERT_TRUE(pskr_call_ok("EA1A"));
    TEST_ASSERT_TRUE(pskr_call_ok("PJ2/K1ABC"));
    TEST_ASSERT_TRUE(pskr_call_ok("K1ABC/P"));
    TEST_ASSERT_FALSE(pskr_call_ok(NULL));
    TEST_ASSERT_FALSE(pskr_call_ok("<...>"));          /* hashed: never heard in full */
    TEST_ASSERT_FALSE(pskr_call_ok("<PJ2/K1ABC>"));    /* hashed, looked up */
    TEST_ASSERT_FALSE(pskr_call_ok("CQ"));
    TEST_ASSERT_FALSE(pskr_call_ok("ABCD"));           /* no digit */
    TEST_ASSERT_FALSE(pskr_call_ok("1234"));           /* no letter */
    TEST_ASSERT_FALSE(pskr_call_ok("ea1a"));
    TEST_ASSERT_FALSE(pskr_call_ok("TOOLONGCALL12345"));
}

static void test_header_and_receiver_record(void)
{
    uint8_t d[PSKR_DATAGRAM_MAX];
    int taken = -1;
    size_t n = pskr_encode(d, sizeof d, &N1DQ, NULL, 0, false, 0x47953272, 1, 0xAABBCCDD, &taken);
    const uint8_t header[] = { 0x00, 0x0A, 0x00, 0x30, 0x47, 0x95, 0x32, 0x72,
                               0x00, 0x00, 0x00, 0x01, 0xAA, 0xBB, 0xCC, 0xDD };
    TEST_ASSERT_EQUAL_size_t(HEADER_BYTES + sizeof RX_RECORD, n);
    TEST_ASSERT_EQUAL_INT(0, taken);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(header, d, HEADER_BYTES);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(RX_RECORD, d + HEADER_BYTES, sizeof RX_RECORD);
}

static void test_templates_and_spots(void)
{
    pskr_spot s[2] = {
        { .call = "N1DQ", .mode = "PSK", .freq_hz = 0x00D6B327, .snr_db = -5, .at = 0x47953254 },
        { .call = "KB1MBX", .locator = "FN42", .mode = "FT8", .freq_hz = 0x00D6B4CB, .snr_db = 12,
          .at = 0x47953268 },
    };
    uint8_t d[PSKR_DATAGRAM_MAX];
    int taken = 0;
    size_t n = pskr_encode(d, sizeof d, &N1DQ, s, 2, true, 0x47953272, 1, 0, &taken);
    TEST_ASSERT_EQUAL_INT(2, taken);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(RX_TEMPLATE, d + HEADER_BYTES, sizeof RX_TEMPLATE);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(TX_TEMPLATE, d + HEADER_BYTES + sizeof RX_TEMPLATE, sizeof TX_TEMPLATE);
    size_t at = HEADER_BYTES + sizeof RX_TEMPLATE + sizeof TX_TEMPLATE;
    TEST_ASSERT_EQUAL_HEX8_ARRAY(RX_RECORD, d + at, sizeof RX_RECORD);
    at += sizeof RX_RECORD;
    const uint8_t spots[] = {
        0x48, 0x53, 0x00, 0x34,
        0x04, 'N', '1', 'D', 'Q', 0x00, 0xD6, 0xB3, 0x27, 0xFB, 0x03, 'P', 'S', 'K',
        0x00, 0x01, 0x47, 0x95, 0x32, 0x54,
        0x06, 'K', 'B', '1', 'M', 'B', 'X', 0x00, 0xD6, 0xB4, 0xCB, 0x0C, 0x03, 'F', 'T', '8',
        0x04, 'F', 'N', '4', '2', 0x01, 0x47, 0x95, 0x32, 0x68,
        0x00, 0x00,
    };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(spots, d + at, sizeof spots);
    TEST_ASSERT_EQUAL_size_t(at + sizeof spots, n);
    TEST_ASSERT_EQUAL_UINT8(n >> 8, d[2]);              /* the header's length */
    TEST_ASSERT_EQUAL_UINT8(n & 0xff, d[3]);
}

/* More than fit: a full datagram, every set's length right and aligned,
 * and the rest left for the next one. */
static void test_full_datagram(void)
{
    enum { MANY = 200 };
    static pskr_spot s[MANY];
    for (int i = 0; i < MANY; i++) {
        snprintf(s[i].call, sizeof s[i].call, "PY%dXYZ", i);
        snprintf(s[i].locator, sizeof s[i].locator, "GG66");
        snprintf(s[i].mode, sizeof s[i].mode, "FT8");
        s[i].freq_hz = 14074000 + i;
    }
    uint8_t d[PSKR_DATAGRAM_MAX];
    int taken = 0;
    size_t n = pskr_encode(d, sizeof d, &N1DQ, s, MANY, true, 0, 0, 0, &taken);
    TEST_ASSERT_TRUE(taken > 0 && taken < MANY);
    TEST_ASSERT_TRUE(n <= sizeof d);
    TEST_ASSERT_EQUAL_size_t(0, n % 4);
    size_t at = HEADER_BYTES, sets = 0;
    while (at < n) {                                   /* the sets add up to the datagram */
        size_t len = (size_t) d[at + 2] << 8 | d[at + 3];
        TEST_ASSERT_TRUE(len >= 4 && len % 4 == 0);
        at += len;
        sets++;
    }
    TEST_ASSERT_EQUAL_size_t(n, at);
    TEST_ASSERT_EQUAL_size_t(4, sets);
    /* No room for one spot after the receiver record: the receiver alone. */
    n = pskr_encode(d, HEADER_BYTES + sizeof RX_RECORD + 8, &N1DQ, s, MANY, false, 0, 0, 0, &taken);
    TEST_ASSERT_EQUAL_INT(0, taken);
    TEST_ASSERT_EQUAL_size_t(HEADER_BYTES + sizeof RX_RECORD, n);
    TEST_ASSERT_EQUAL_size_t(0, pskr_encode(d, HEADER_BYTES + 8, &N1DQ, s, MANY, false, 0, 0, 0, &taken));
}

static void test_once_an_hour(void)
{
    static pskr_seen seen;
    memset(&seen, 0, sizeof seen);
    TEST_ASSERT_TRUE(pskr_seen_due(&seen, "EA1A", 14074500, 1000));
    TEST_ASSERT_FALSE(pskr_seen_due(&seen, "EA1A", 14075900, 1300));      /* same band */
    TEST_ASSERT_TRUE(pskr_seen_due(&seen, "EA1A", 7074500, 1300));        /* another band */
    TEST_ASSERT_FALSE(pskr_seen_due(&seen, "EA1A", 14074500, 1000 + PSKR_REPEAT_S - 1));
    TEST_ASSERT_TRUE(pskr_seen_due(&seen, "EA1A", 14074500, 1000 + PSKR_REPEAT_S));
    /* Full: the oldest is forgotten, so it is due again. */
    char call[PSKR_CALL_MAX];
    for (int i = 0; i < PSKR_SEEN_MAX; i++) {
        snprintf(call, sizeof call, "K%dAA", i);
        TEST_ASSERT_TRUE(pskr_seen_due(&seen, call, 14074000, 2000));
    }
    TEST_ASSERT_TRUE(pskr_seen_due(&seen, "EA1A", 14074500, 2000));
    TEST_ASSERT_FALSE(pskr_seen_due(&seen, "K1023AA", 14074000, 2000));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_locator);
    RUN_TEST(test_calls);
    RUN_TEST(test_header_and_receiver_record);
    RUN_TEST(test_templates_and_spots);
    RUN_TEST(test_full_datagram);
    RUN_TEST(test_once_an_hour);
    return UNITY_END();
}
