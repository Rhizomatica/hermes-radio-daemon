/* js8_msg: frames built as JS8Call's Varicode packs them (the packers
 * below follow packCompoundFrame, packAlphaNumeric50, packCallsign,
 * packGrid and packHuffMessage), read back as JS8Call's DecodedText
 * shows them; and a frame through the whole chain, tones to message.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "dsp/js8_msg.h"

void setUp(void) {}
void tearDown(void) {}

#define ALNUM      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /@"
#define ALPHABET   "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-+"
#define NBASECALL  (37u * 36 * 10 * 27 * 27 * 27)
#define NBASEGRID  (180 * 180)
#define NUSERGRID  (NBASEGRID + 10)
#define GRID_IN51  17051u      /* IN51: west 10, lat 41 (see unpack_grid) */

typedef struct { uint8_t b[72]; int n; } bits;

static void put(bits *v, uint64_t x, int n)
{
    for (int i = n - 1; i >= 0; i--)
        v->b[v->n++] = (uint8_t) ((x >> i) & 1);
}

static void to_frame(const bits *v, char out[13])
{
    for (int i = 0; i < 12; i++) {
        int w = 0;
        for (int k = 0; k < 6; k++)
            w = w << 1 | v->b[i * 6 + k];
        out[i] = ALPHABET[w];
    }
    out[12] = '\0';
}

static int idx(char c) { return (int) (strchr(ALNUM, c) - ALNUM); }

/* packAlphaNumeric50, for a callsign of the form XXX/XXX or plain. */
static uint64_t alnum50(const char *call)
{
    char w[16] = "";
    snprintf(w, sizeof w, "%s", call);
    char v[12];
    int j = 0;
    for (int i = 0; w[i] && j < 11; i++) {
        if ((j == 3 || j == 7) && w[i] != '/')
            v[j++] = ' ';
        v[j++] = w[i];
    }
    while (j < 11)
        v[j++] = ' ';
    uint64_t p = idx(v[0]);
    p = p * 38 + idx(v[1]);
    p = p * 38 + idx(v[2]);
    p = p * 2 + (v[3] == '/');
    p = p * 38 + idx(v[4]);
    p = p * 38 + idx(v[5]);
    p = p * 38 + idx(v[6]);
    p = p * 2 + (v[7] == '/');
    p = p * 38 + idx(v[8]);
    p = p * 38 + idx(v[9]);
    p = p * 38 + idx(v[10]);
    return p;
}

/* packCallsign for a six-character standard call (" X9XXX" padded). */
static uint32_t call28(const char *six)
{
    uint32_t p = (uint32_t) idx(six[0]);
    p = p * 36 + (uint32_t) idx(six[1]);
    p = p * 10 + (uint32_t) idx(six[2]);
    for (int i = 3; i < 6; i++)
        p = p * 27 + (uint32_t) (idx(six[i]) - 10);
    return p;
}

/* packCompoundFrame: [3][50][11],[5][3]. */
static void compound(char out[13], int type, const char *call, unsigned num, unsigned bits3)
{
    bits v = { .n = 0 };
    put(&v, (uint64_t) type, 3);
    put(&v, alnum50(call), 50);
    put(&v, num >> 5, 11);
    put(&v, num & 31, 5);
    put(&v, bits3, 3);
    to_frame(&v, out);
}

/* packDirectedMessage's frame: [3][28][28][5],[2][6]. */
static void directed(char out[13], uint32_t from, uint32_t to, unsigned cmd, unsigned num)
{
    bits v = { .n = 0 };
    put(&v, 3, 3);
    put(&v, from, 28);
    put(&v, to, 28);
    put(&v, cmd, 5);
    put(&v, 0, 2);
    put(&v, num, 6);
    to_frame(&v, out);
}

static void unpack(const char *frame, int type, js8_msg *m) { js8_msg_unpack(frame, type, m); }

static void test_heartbeat_and_cq(void)
{
    char f[13];
    js8_msg m;
    compound(f, 0, "CR7BYQ", GRID_IN51, 0);
    unpack(f, JS8_TX_FIRST | JS8_TX_LAST, &m);
    TEST_ASSERT_EQUAL_INT(JS8_MSG_HEARTBEAT, m.kind);
    TEST_ASSERT_EQUAL_STRING("CR7BYQ: @HB HEARTBEAT IN51", m.text);
    TEST_ASSERT_EQUAL_STRING("CR7BYQ", m.from);
    TEST_ASSERT_EQUAL_STRING("IN51", m.grid);
    TEST_ASSERT_TRUE(m.first && m.last);

    compound(f, 0, "PJ2/K1ABC", GRID_IN51 | (1u << 15), 1);
    unpack(f, JS8_TX_FIRST | JS8_TX_LAST, &m);
    TEST_ASSERT_EQUAL_INT(JS8_MSG_CQ, m.kind);
    TEST_ASSERT_EQUAL_STRING("PJ2/K1ABC: @ALLCALL CQ DX IN51", m.text);
    TEST_ASSERT_EQUAL_STRING("PJ2/K1ABC", m.from);
}

static void test_compound(void)
{
    char f[13];
    js8_msg m;
    compound(f, 1, "EA1A", (1u << 15) - 1, 0);
    unpack(f, JS8_TX_FIRST, &m);
    TEST_ASSERT_EQUAL_INT(JS8_MSG_COMPOUND, m.kind);
    TEST_ASSERT_EQUAL_STRING("EA1A:", m.text);
    TEST_ASSERT_TRUE(m.first && !m.last);
    /* Directed: an SNR of -12, as packCmd puts it: [1][0][snr + 31]. */
    compound(f, 2, "EA1A", NUSERGRID + (0x80 | (19 & 0x3f)), 0);
    unpack(f, 0, &m);
    TEST_ASSERT_EQUAL_INT(JS8_MSG_COMPOUND_DIRECTED, m.kind);
    TEST_ASSERT_EQUAL_STRING("EA1A SNR -12", m.text);
    compound(f, 2, "EA1A", NUSERGRID + 28, 0);   /* 73 */
    unpack(f, 0, &m);
    TEST_ASSERT_EQUAL_STRING("EA1A 73", m.text);
}

static void test_directed(void)
{
    char f[13];
    js8_msg m;
    uint32_t cr7byq = call28("CR7BYQ"), ea1a = call28("EA1A  ");   /* the digit third, as packCallsign fits it */
    directed(f, cr7byq, ea1a, 25, 31 - 12);   /* SNR -12 */
    unpack(f, JS8_TX_FIRST | JS8_TX_LAST, &m);
    TEST_ASSERT_EQUAL_INT(JS8_MSG_DIRECTED, m.kind);
    TEST_ASSERT_EQUAL_STRING("CR7BYQ: EA1A SNR -12", m.text);
    TEST_ASSERT_EQUAL_STRING("CR7BYQ", m.from);
    TEST_ASSERT_EQUAL_STRING("EA1A", m.to);
    directed(f, cr7byq, NBASECALL + 2, 0, 0);  /* @ALLCALL SNR? */
    unpack(f, 0, &m);
    TEST_ASSERT_EQUAL_STRING("CR7BYQ: @ALLCALL SNR?", m.text);
    TEST_ASSERT_EQUAL_STRING("@ALLCALL", m.to);
    directed(f, cr7byq, ea1a, 31, 0);          /* free text follows */
    unpack(f, JS8_TX_FIRST, &m);
    TEST_ASSERT_EQUAL_STRING("CR7BYQ: EA1A", m.text);
}

/* packHuffMessage: [1][0][codes...][0][1...]. */
static void test_data(void)
{
    static const char *codes[] = { "00011", "100", "110011", "110011", "11111" };   /* HELLO */
    bits v = { .n = 0 };
    put(&v, 1, 1);
    put(&v, 0, 1);
    for (int i = 0; i < 5; i++)
        for (const char *c = codes[i]; *c; c++)
            put(&v, (uint64_t) (*c - '0'), 1);
    put(&v, 0, 1);
    while (v.n < 72)
        put(&v, 1, 1);
    char f[13];
    to_frame(&v, f);
    js8_msg m;
    unpack(f, 0, &m);
    TEST_ASSERT_EQUAL_INT(JS8_MSG_DATA, m.kind);
    TEST_ASSERT_EQUAL_STRING("HELLO", m.text);
    TEST_ASSERT_EQUAL_STRING("", m.from);
    /* Compressed text, and data-only frames: the raw frame. */
    v.b[1] = 1;
    to_frame(&v, f);
    unpack(f, 0, &m);
    TEST_ASSERT_EQUAL_INT(JS8_MSG_RAW, m.kind);
    TEST_ASSERT_EQUAL_STRING(f, m.text);
    unpack(f, JS8_TX_DATA, &m);
    TEST_ASSERT_EQUAL_INT(JS8_MSG_RAW, m.kind);
}

/* Through the whole chain: frame to tones to soft bits to message. */
static void test_chain(void)
{
    char f[13];
    compound(f, 0, "CR7BYQ", GRID_IN51, 0);
    int tones[JS8_NN];
    TEST_ASSERT_TRUE(js8_encode(f, JS8_TX_FIRST | JS8_TX_LAST, JS8_COSTAS_ORIGINAL, tones));
    float llr[JS8_N];
    for (int s = 0; s < JS8_ND / 2; s++)
        for (int b = 0; b < 3; b++) {
            llr[s * 3 + b] = (tones[JS8_PARITY_AT + s] >> (2 - b)) & 1 ? 4.0f : -4.0f;
            llr[JS8_M + s * 3 + b] = (tones[JS8_DATA_AT + s] >> (2 - b)) & 1 ? 4.0f : -4.0f;
        }
    uint8_t b[JS8_K];
    char got[JS8_CHARS + 1];
    int type;
    TEST_ASSERT_TRUE(js8_ldpc_decode(llr, JS8_BP_ITERATIONS, b) >= 0);
    TEST_ASSERT_TRUE(js8_unpack(b, got, &type));
    js8_msg m;
    unpack(got, type, &m);
    TEST_ASSERT_EQUAL_STRING("CR7BYQ: @HB HEARTBEAT IN51", m.text);
}

static void test_not_a_frame(void)
{
    js8_msg m;
    unpack("SHORT", 0, &m);
    TEST_ASSERT_EQUAL_INT(JS8_MSG_RAW, m.kind);
    unpack("HAS A SPACE!", 0, &m);
    TEST_ASSERT_EQUAL_INT(JS8_MSG_RAW, m.kind);
    unpack(NULL, 0, &m);
    TEST_ASSERT_EQUAL_INT(JS8_MSG_RAW, m.kind);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_heartbeat_and_cq);
    RUN_TEST(test_compound);
    RUN_TEST(test_directed);
    RUN_TEST(test_data);
    RUN_TEST(test_chain);
    RUN_TEST(test_not_a_frame);
    return UNITY_END();
}
