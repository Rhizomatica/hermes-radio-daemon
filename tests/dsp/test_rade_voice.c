/* rade_voice_test — RADE V2 keyed latent mixing (dsp/rade_voice.c).
 *
 * The transform is unitary per carrier (unmixing is exact, each carrier's
 * pair energy is kept), commutes with a per-carrier channel gain, changes
 * with the frame, the nonce and the key, and does nothing without a key.
 *
 * The signalling, TX to RX on the latent and aux symbols with no modem in
 * between: lock and exact unmixing from the first message, late entry,
 * no key, a wrong key, a clear over, combining messages under noise, a
 * symbol slip, a new over without a reset; and the clipper's PAPR.
 * Channel and speech tests are utils/rade_crypt_sim.c's.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "test_util.h"
#include "dsp/rade_voice.h"
#include "dsp/voice_crypto.h"

#define NC RADE_VOICE_NC

static const uint8_t KEY_A[VOICE_KEY_BYTES] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};
static const uint8_t KEY_B[VOICE_KEY_BYTES] = { 0xff, 0xee, 0xdd };
static const uint8_t R1[VOICE_NONCE_RAND_BYTES] = { 1, 2, 3, 4, 5, 6 };
static const uint8_t R2[VOICE_NONCE_RAND_BYTES] = { 6, 5, 4, 3, 2, 1 };

/* a latent with every value distinct and nonzero */
static void latent(float z[RADE_VOICE_LATENT])
{
    for (int i = 0; i < RADE_VOICE_LATENT; i++)
        z[i] = sinf(0.7f * (float) i + 0.3f) + 0.05f * (float) (i % 7);
}

static double max_diff(const float *a, const float *b)
{
    double m = 0;
    for (int i = 0; i < RADE_VOICE_LATENT; i++)
        m = fmax(m, fabs((double) a[i] - b[i]));
    return m;
}

static double pair_energy(const float *z, int c)
{
    const float *p0 = &z[c * 2], *p1 = &z[(NC + c) * 2];
    return (double) p0[0] * p0[0] + (double) p0[1] * p0[1] + (double) p1[0] * p1[0] + (double) p1[1] * p1[1];
}

void setUp(void)
{
    voice_crypto_set_key(KEY_A);
}

void tearDown(void)
{
    voice_crypto_clear_key();
}

static void test_roundtrip(void)
{
    float x[RADE_VOICE_LATENT], z[RADE_VOICE_LATENT];
    latent(x);
    for (uint32_t frame = 0; frame < 100; frame++) {
        memcpy(z, x, sizeof(z));
        TEST_ASSERT_TRUE(rade_voice_scramble(z, R1, frame, false));
        TEST_ASSERT_TRUE(max_diff(z, x) > 0.1);
        TEST_ASSERT_TRUE(rade_voice_scramble(z, R1, frame, true));
        TEST_ASSERT_TRUE(max_diff(z, x) < 1e-5);
    }
}

static void test_pair_energy_kept(void)
{
    float x[RADE_VOICE_LATENT], z[RADE_VOICE_LATENT];
    latent(x);
    memcpy(z, x, sizeof(z));
    TEST_ASSERT_TRUE(rade_voice_scramble(z, R1, 7, false));
    for (int c = 0; c < NC; c++)
        TEST_ASSERT_DOUBLE_WITHIN(1e-5, pair_energy(x, c), pair_energy(z, c));
}

/* The channel's per-carrier gain h_c, applied between mixing and unmixing,
 * comes out as h_c on the plain latent: what the decoder sees unscrambled. */
static void test_commutes_with_carrier_gain(void)
{
    float x[RADE_VOICE_LATENT], z[RADE_VOICE_LATENT], want[RADE_VOICE_LATENT];
    latent(x);
    memcpy(z, x, sizeof(z));
    TEST_ASSERT_TRUE(rade_voice_scramble(z, R1, 3, false));
    for (int s = 0; s < RADE_VOICE_NS; s++)
        for (int c = 0; c < NC; c++) {
            float hr = 0.3f + 0.1f * (float) c, hi = -0.5f + 0.07f * (float) c;
            float *p = &z[(s * NC + c) * 2], *w = &want[(s * NC + c) * 2];
            const float *q = &x[(s * NC + c) * 2];
            float r = p[0], i = p[1];
            p[0] = hr * r - hi * i;
            p[1] = hr * i + hi * r;
            w[0] = hr * q[0] - hi * q[1];
            w[1] = hr * q[1] + hi * q[0];
        }
    TEST_ASSERT_TRUE(rade_voice_scramble(z, R1, 3, true));
    TEST_ASSERT_TRUE(max_diff(z, want) < 1e-5);
}

static void test_frame_nonce_key_change_it(void)
{
    float x[RADE_VOICE_LATENT], a[RADE_VOICE_LATENT], b[RADE_VOICE_LATENT];
    latent(x);
    memcpy(a, x, sizeof(a));
    TEST_ASSERT_TRUE(rade_voice_scramble(a, R1, 0, false));

    memcpy(b, x, sizeof(b));
    TEST_ASSERT_TRUE(rade_voice_scramble(b, R1, 1, false));
    TEST_ASSERT_TRUE(max_diff(a, b) > 0.1);

    memcpy(b, x, sizeof(b));
    TEST_ASSERT_TRUE(rade_voice_scramble(b, R2, 0, false));
    TEST_ASSERT_TRUE(max_diff(a, b) > 0.1);

    memcpy(b, x, sizeof(b));
    voice_crypto_set_key(KEY_B);
    TEST_ASSERT_TRUE(rade_voice_scramble(b, R1, 0, false));
    TEST_ASSERT_TRUE(max_diff(a, b) > 0.1);

    /* unmixing with the wrong key or frame does not recover the latent */
    TEST_ASSERT_TRUE(rade_voice_scramble(a, R1, 0, true));   /* KEY_B */
    TEST_ASSERT_TRUE(max_diff(a, x) > 0.1);
    voice_crypto_set_key(KEY_A);
    memcpy(a, x, sizeof(a));
    TEST_ASSERT_TRUE(rade_voice_scramble(a, R1, 5, false));
    TEST_ASSERT_TRUE(rade_voice_scramble(a, R1, 6, true));
    TEST_ASSERT_TRUE(max_diff(a, x) > 0.1);
}

/* The same key, nonce and frame give the same transform: two stations
 * agree. */
static void test_deterministic(void)
{
    float a[RADE_VOICE_LATENT], b[RADE_VOICE_LATENT];
    latent(a);
    latent(b);
    TEST_ASSERT_TRUE(rade_voice_scramble(a, R2, 12345, false));
    voice_crypto_clear_key();
    voice_crypto_set_key(KEY_A);
    TEST_ASSERT_TRUE(rade_voice_scramble(b, R2, 12345, false));
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(a));
}

static void test_no_key(void)
{
    float x[RADE_VOICE_LATENT], z[RADE_VOICE_LATENT];
    latent(x);
    memcpy(z, x, sizeof(z));
    voice_crypto_clear_key();
    TEST_ASSERT_FALSE(rade_voice_scramble(z, R1, 0, false));
    TEST_ASSERT_EQUAL_MEMORY(x, z, sizeof(z));
}

/* ── signalling: TX to RX, symbol by symbol ── */

#define NS RADE_VOICE_NS
#define NA RADE_VOICE_NAUX
#define MSG RADE_VOICE_MSG_FRAMES

typedef struct {
    const uint8_t *tx_key, *rx_key;   /* NULL: none */
    bool encrypt;
    int frames, join;                  /* the RX starts at frame join */
    float noise;                       /* per float of the aux symbols */
    int slip_at;                       /* frame where the RX loses a symbol (-1: none) */
    int new_over_at;                   /* frame where the TX starts a new over (-1) */
} link_cfg;

typedef struct {
    int first_aligned;                 /* first frame unmixed (-1: never) */
    int exact, wrong, misaligned_ok;   /* aligned calls right / wrong; the other alignment said 0 */
    rade_vc_status status;
    bool played;                       /* rade_voice_rx_play at the end */
} link_res;

static double gauss1(void)
{
    double u1 = (rand() + 1.0) / (RAND_MAX + 2.0), u2 = rand() / (RAND_MAX + 1.0);
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

static void set_key(const uint8_t *k)
{
    if (k)
        voice_crypto_set_key(k);
    else
        voice_crypto_clear_key();
}

static link_res run_link(link_cfg c)
{
    link_res res = {-1, 0, 0, 0, RADE_VC_CLEAR, false};
    static rade_voice_tx tx;
    static rade_voice_rx rx;
    set_key(c.tx_key);
    rade_voice_tx_begin(&tx, c.encrypt, 1.0f);
    rade_voice_rx_reset(&rx);

    float prev_z[2] = {0}, prev_aux[NA][2] = {{0}};   /* the RX's last symbol */
    float prev_plain[RADE_VOICE_LATENT];
    uint32_t sym = 1000;               /* the receiver's counter, any start */
    srand(7);
    for (int f = 0; f < c.frames; f++) {
        if (f == c.new_over_at) {
            set_key(c.tx_key);
            rade_voice_tx_begin(&tx, c.encrypt, 1.0f);
        }
        float plain[RADE_VOICE_LATENT], z[RADE_VOICE_LATENT], aux[RADE_VOICE_AUX] = {0};
        for (int i = 0; i < RADE_VOICE_LATENT; i++)
            plain[i] = (float) (23.6 * gauss1() / sqrt(2));
        memcpy(z, plain, sizeof(z));
        set_key(c.tx_key);
        rade_voice_tx_frame(&tx, z, aux);
        for (int i = 0; i < RADE_VOICE_AUX; i++)
            aux[i] += (float) (c.noise * gauss1());

        if (f < c.join) {
            memcpy(prev_plain, plain, sizeof(plain));
            continue;
        }
        set_key(c.rx_key);
        for (int s = 0; s < NS; s++) {
            if (f == c.slip_at && s == 0)
                continue;              /* this symbol never reaches the hook */
            /* the hook's view: the previous symbol and this one */
            float hz[RADE_VOICE_LATENT], ha[RADE_VOICE_AUX];
            if (s == 1) {
                memcpy(hz, z, sizeof(hz));
                memcpy(ha, aux, sizeof(ha));
            } else {
                memset(hz, 0, sizeof(hz));
                memcpy(&hz[NC * 2], z, NC * 2 * sizeof(float));   /* this symbol second */
                for (int a = 0; a < NA; a++) {
                    ha[a * 2] = prev_aux[a][0];
                    ha[a * 2 + 1] = prev_aux[a][1];
                    ha[(NA + a) * 2] = aux[a * 2];
                    ha[(NA + a) * 2 + 1] = aux[a * 2 + 1];
                }
            }
            int al = rade_voice_rx_symbol(&rx, hz, ha, sym++);
            if (s == 1 && al == 1) {
                if (max_diff(hz, plain) < 1e-3) {
                    res.exact++;
                    if (res.first_aligned < 0)
                        res.first_aligned = f;
                } else {
                    res.wrong++;
                }
            } else if (s == 0 && al == 0) {
                res.misaligned_ok++;
            } else if (s == 0 && al == 1) {
                res.wrong++;           /* the wrong alignment taken as a frame */
            }
            for (int a = 0; a < NA; a++) {
                prev_aux[a][0] = aux[(s * NA + a) * 2];
                prev_aux[a][1] = aux[(s * NA + a) * 2 + 1];
            }
        }
        (void) prev_z;
        memcpy(prev_plain, plain, sizeof(plain));
    }
    res.status = rx.status;
    res.played = rade_voice_rx_play(&rx);
    return res;
}

static link_cfg cfg_default(void)
{
    link_cfg c = {KEY_A, KEY_A, true, 10 * MSG, 0, 0.0f, -1, -1};
    return c;
}

static void test_link_locks(void)
{
    link_res r = run_link(cfg_default());
    /* the first message ends with frame MSG-1: from there on, exact */
    TEST_ASSERT_EQUAL_INT64_MESSAGE(MSG - 1, r.first_aligned, test_msg("first unmixed frame %d", r.first_aligned));
    TEST_ASSERT_EQUAL_INT64_MESSAGE(0, r.wrong, test_msg("%d frames unmixed wrong", r.wrong));
    TEST_ASSERT_EQUAL_INT64_MESSAGE(9 * MSG + 1, r.exact, test_msg("%d frames exact", r.exact));
    TEST_ASSERT_EQUAL_INT64(RADE_VC_DECRYPTING, r.status);
    TEST_ASSERT_TRUE(r.played);
}

static void test_link_late_join(void)
{
    link_cfg c = cfg_default();
    c.join = 3 * MSG + 7;              /* mid-message */
    link_res r = run_link(c);
    /* the message it joined in, decoded with its first 7 frames erased, or
     * the next one */
    TEST_ASSERT_LESS_OR_EQUAL_INT64_MESSAGE(5 * MSG - 1, r.first_aligned, test_msg("late: first unmixed frame %d", r.first_aligned));
    TEST_ASSERT_GREATER_OR_EQUAL_INT64_MESSAGE(4 * MSG - 1, r.first_aligned, test_msg("late: first unmixed frame %d", r.first_aligned));
    TEST_ASSERT_EQUAL_INT64(0, r.wrong);
    TEST_ASSERT_EQUAL_INT64(RADE_VC_DECRYPTING, r.status);
}

static void test_link_no_key(void)
{
    link_cfg c = cfg_default();
    c.rx_key = NULL;
    link_res r = run_link(c);
    TEST_ASSERT_EQUAL_INT64(-1, r.first_aligned);
    TEST_ASSERT_EQUAL_INT64(RADE_VC_NO_KEY, r.status);
    TEST_ASSERT_FALSE(r.played);
}

static void test_link_wrong_key(void)
{
    link_cfg c = cfg_default();
    c.rx_key = KEY_B;
    link_res r = run_link(c);
    TEST_ASSERT_EQUAL_INT64(-1, r.first_aligned);
    TEST_ASSERT_EQUAL_INT64(RADE_VC_BAD_KEY, r.status);
    TEST_ASSERT_FALSE(r.played);
}

static void test_link_clear_over(void)
{
    link_cfg c = cfg_default();
    c.encrypt = false;
    c.noise = 2.0f;                    /* the aux bins hold only noise */
    link_res r = run_link(c);
    TEST_ASSERT_EQUAL_INT64(-1, r.first_aligned);
    TEST_ASSERT_EQUAL_INT64(RADE_VC_CLEAR, r.status);
    TEST_ASSERT_TRUE(r.played);
}

/* Noise at which one message alone rarely decodes: combining has to. */
static void test_link_combining(void)
{
    link_cfg c = cfg_default();
    c.frames = 12 * MSG;
    c.noise = 16.0f;                   /* locks on the 3rd message, combining 3 */
    link_res r = run_link(c);
    TEST_ASSERT_GREATER_OR_EQUAL_INT64_MESSAGE(2 * MSG - 1, r.first_aligned, test_msg("locked at frame %d: not noisy enough to need combining", r.first_aligned));
    TEST_ASSERT_LESS_OR_EQUAL_INT64_MESSAGE(6 * MSG - 1, r.first_aligned, test_msg("locked at frame %d", r.first_aligned));
    TEST_ASSERT_EQUAL_INT64_MESSAGE(0, r.wrong, test_msg("%d frames unmixed wrong", r.wrong));
    printf("combining: locked at frame %d (message %d)\n", r.first_aligned, (r.first_aligned + 1) / MSG);
}

/* The receiver's counter slips a symbol (RADE lost sync and acquired
 * again): the next message's correlation finds it, and frames come out
 * right again. */
static void test_link_slip(void)
{
    link_cfg c = cfg_default();
    c.frames = 8 * MSG;
    c.slip_at = 2 * MSG + 5;
    link_res r = run_link(c);
    /* wrong between the slip and the next message end, then exact */
    TEST_ASSERT_LESS_OR_EQUAL_INT64_MESSAGE(2 * (MSG - 5) + 2, r.wrong, test_msg("%d wrong after the slip", r.wrong));
    TEST_ASSERT_GREATER_OR_EQUAL_INT64_MESSAGE(8 * MSG - 2 * MSG - 2, r.exact, test_msg("%d exact", r.exact));
    TEST_ASSERT_EQUAL_INT64(RADE_VC_DECRYPTING, r.status);
}

/* A new over (new nonce) with no end of over in between: the known
 * messages stop matching, the receiver unlocks and locks on the new one. */
static void test_link_new_over(void)
{
    link_cfg c = cfg_default();
    c.frames = 12 * MSG;
    c.new_over_at = 4 * MSG;
    link_res r = run_link(c);
    TEST_ASSERT_EQUAL_INT64(RADE_VC_DECRYPTING, r.status);
    /* frames of the new over unmixed with the old nonce would count wrong */
    TEST_ASSERT_LESS_OR_EQUAL_INT64_MESSAGE(4 * MSG * 2, r.wrong, test_msg("%d wrong", r.wrong));
    TEST_ASSERT_GREATER_OR_EQUAL_INT64_MESSAGE(4 * MSG, r.exact - 4 * MSG, test_msg("%d exact", r.exact));
}

/* Random-phase multicarrier: ~10 dB PAPR in, about stock RADE V2's out. */
static void test_clip_papr(void)
{
    enum { N = 8000 * 4, NCAR = 16 };
    static float iq[2 * N];
    double ph[NCAR];
    srand(3);
    for (int k = 0; k < N; k++) {
        if (k % 160 == 0)
            for (int c = 0; c < NCAR; c++)
                ph[c] = 2 * M_PI * rand() / (RAND_MAX + 1.0);
        double re = 0, im = 0;
        for (int c = 0; c < NCAR; c++) {
            double a = 2 * M_PI * (1000.0 + 62.5 * c) * k / 8000 + ph[c];
            re += cos(a);
            im += sin(a);
        }
        iq[2 * k] = (float) re;
        iq[2 * k + 1] = (float) im;
    }
    rade_voice_clip cl;
    rade_voice_clip_reset(&cl);
    rade_voice_clip_run(&cl, iq, N);
    double p = 0, pk = 0;
    for (int k = 8000; k < N; k++) {   /* past the reference's first second */
        double m = (double) iq[2 * k] * iq[2 * k] + (double) iq[2 * k + 1] * iq[2 * k + 1];
        p += m;
        pk = fmax(pk, m);
    }
    p /= N - 8000;
    double papr = 10 * log10(pk / p);
    printf("clipper: PAPR %.1f dB\n", papr);
    TEST_ASSERT_LESS_OR_EQUAL_DOUBLE_MESSAGE(4.5, papr, test_msg("PAPR %.1f dB after the clipper", papr));
    TEST_ASSERT_GREATER_THAN_DOUBLE(1.0, p);   /* the signal is still there */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_pair_energy_kept);
    RUN_TEST(test_commutes_with_carrier_gain);
    RUN_TEST(test_frame_nonce_key_change_it);
    RUN_TEST(test_deterministic);
    RUN_TEST(test_no_key);
    RUN_TEST(test_link_locks);
    RUN_TEST(test_link_late_join);
    RUN_TEST(test_link_no_key);
    RUN_TEST(test_link_wrong_key);
    RUN_TEST(test_link_clear_over);
    RUN_TEST(test_link_combining);
    RUN_TEST(test_link_slip);
    RUN_TEST(test_link_new_over);
    RUN_TEST(test_clip_papr);
    return UNITY_END();
}
