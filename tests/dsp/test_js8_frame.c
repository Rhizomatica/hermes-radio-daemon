/* js8_frame: JS8's frame against JS8Call itself.  The tones and CRCs
 * below came from JS8Call-improved's own encode() and boost's
 * augmented_crc, compiled outside this tree; the LDPC tables must form a
 * consistent code that the encoder's codewords satisfy; and decoding
 * gives back the frame, from clean and from noisy soft bits.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "dsp/js8_frame.h"

void setUp(void) {}
void tearDown(void) {}

#define SYMBOL_BITS 3
#define CLEAN_LLR   4.0f    /* a confident soft bit */

static const struct {
    const char *msg;
    int type;
    js8_costas_type costas;
    const char *tones;
} REF[] = {
    { "CR7BYQ+IN51x", 0, JS8_COSTAS_ORIGINAL,
      "4256130127730451032620770455426725224256130143307134232772227050173034764256130" },
    { "000000000000", 7, JS8_COSTAS_ORIGINAL,
      "4256130507675122441343374656051576164256130000000000000000000000000700324256130" },
    { "++++++++++++", 3, JS8_COSTAS_MODIFIED,
      "0623541577225477230024720047604451631502364777777777777777777777777312522506413" },
    { "Hello-World0", 5, JS8_COSTAS_ORIGINAL,
      "4256130375243360763264602260744713264256130215057576276406265574700575004256130" },
};
#define NREF (int) (sizeof REF / sizeof REF[0])

/* The codeword the tones carry, as soft bits: parity symbols, then data. */
static void tones_to_llr(const int tones[JS8_NN], float llr[JS8_N], float conf)
{
    for (int s = 0; s < JS8_ND / 2; s++)
        for (int b = 0; b < SYMBOL_BITS; b++) {
            int p = (tones[JS8_PARITY_AT + s] >> (2 - b)) & 1;
            int d = (tones[JS8_DATA_AT + s] >> (2 - b)) & 1;
            llr[s * SYMBOL_BITS + b] = p ? conf : -conf;
            llr[JS8_M + s * SYMBOL_BITS + b] = d ? conf : -conf;
        }
}

static void test_crc_against_boost(void)
{
    const uint8_t v[][11] = {
        { 0 },
        { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x11, 0x20, 0x00 },
        { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xe0, 0x00 },
        { 0x80 },
    };
    const uint16_t want[] = { 0x02a, 0x042, 0xe84, 0xbd6 };
    for (int i = 0; i < 4; i++)
        TEST_ASSERT_EQUAL_HEX16(want[i], js8_crc12(v[i]));
}

static void test_encode_matches_js8call(void)
{
    for (int r = 0; r < NREF; r++) {
        int t[JS8_NN];
        TEST_ASSERT_TRUE(js8_encode(REF[r].msg, REF[r].type, REF[r].costas, t));
        char got[JS8_NN + 1];
        for (int i = 0; i < JS8_NN; i++)
            got[i] = (char) ('0' + t[i]);
        got[JS8_NN] = '\0';
        TEST_ASSERT_EQUAL_STRING_MESSAGE(REF[r].tones, got, REF[r].msg);
    }
    int t[JS8_NN];
    TEST_ASSERT_FALSE(js8_encode("CR7BYQ IN51x", 0, JS8_COSTAS_ORIGINAL, t));   /* space: no */
}

/* The encoder's codeword passes every check of the decoder's tables: it
 * converges before the first iteration, with no bit against the channel. */
static void test_code_is_consistent(void)
{
    int t[JS8_NN];
    float llr[JS8_N];
    TEST_ASSERT_TRUE(js8_encode(REF[0].msg, REF[0].type, REF[0].costas, t));
    tones_to_llr(t, llr, CLEAN_LLR);
    uint8_t bits[JS8_K];
    TEST_ASSERT_EQUAL_INT(0, js8_ldpc_decode(llr, 0, bits));
}

static void test_decode_clean(void)
{
    for (int r = 0; r < NREF; r++) {
        int t[JS8_NN];
        float llr[JS8_N];
        uint8_t bits[JS8_K];
        char msg[JS8_CHARS + 1];
        int type = -1;
        js8_encode(REF[r].msg, REF[r].type, REF[r].costas, t);
        tones_to_llr(t, llr, CLEAN_LLR);
        TEST_ASSERT_TRUE(js8_ldpc_decode(llr, JS8_BP_ITERATIONS, bits) >= 0);
        TEST_ASSERT_TRUE(js8_unpack(bits, msg, &type));
        TEST_ASSERT_EQUAL_STRING(REF[r].msg, msg);
        TEST_ASSERT_EQUAL_INT(REF[r].type, type);
    }
}

/* Gaussian noise on the soft bits, with some of them wrong: still the
 * frame; and a frame with a bit flipped after decoding fails its CRC. */
static void test_decode_noisy(void)
{
    srand(7);
    int t[JS8_NN];
    js8_encode(REF[0].msg, REF[0].type, REF[0].costas, t);
    float llr[JS8_N];
    tones_to_llr(t, llr, 1.0f);
    int wrong = 0;
    for (int i = 0; i < JS8_N; i++) {
        float u1 = (rand() + 1.0f) / (RAND_MAX + 2.0f), u2 = (rand() + 1.0f) / (RAND_MAX + 2.0f);
        float n = sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2) * 0.6f;
        llr[i] = 2.0f * (llr[i] + n) / (0.6f * 0.6f);
        wrong += (llr[i] > 0) != ((i < JS8_M ? (t[JS8_PARITY_AT + i / 3] >> (2 - i % 3)) & 1
                                             : (t[JS8_DATA_AT + (i - JS8_M) / 3] >> (2 - (i - JS8_M) % 3)) & 1) != 0);
    }
    TEST_ASSERT_TRUE(wrong > 0);
    uint8_t bits[JS8_K];
    char msg[JS8_CHARS + 1];
    TEST_ASSERT_TRUE(js8_ldpc_decode(llr, JS8_BP_ITERATIONS, bits) >= 0);
    TEST_ASSERT_TRUE(js8_unpack(bits, msg, NULL));
    TEST_ASSERT_EQUAL_STRING(REF[0].msg, msg);
    bits[5] ^= 1;
    TEST_ASSERT_FALSE(js8_unpack(bits, msg, NULL));
}

static void test_alphabet(void)
{
    TEST_ASSERT_EQUAL_INT(0, js8_char_word('0'));
    TEST_ASSERT_EQUAL_INT(10, js8_char_word('A'));
    TEST_ASSERT_EQUAL_INT(36, js8_char_word('a'));
    TEST_ASSERT_EQUAL_INT(62, js8_char_word('-'));
    TEST_ASSERT_EQUAL_INT(63, js8_char_word('+'));
    TEST_ASSERT_EQUAL_INT(-1, js8_char_word(' '));
    TEST_ASSERT_EQUAL_INT(-1, js8_char_word('\0'));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_alphabet);
    RUN_TEST(test_crc_against_boost);
    RUN_TEST(test_encode_matches_js8call);
    RUN_TEST(test_code_is_consistent);
    RUN_TEST(test_decode_clean);
    RUN_TEST(test_decode_noisy);
    return UNITY_END();
}
