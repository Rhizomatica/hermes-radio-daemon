/* rade_afc: the coarse frequency correction in front of the RADE V2
 * decoder. A RADE-like over (14 carriers 62.5 Hz apart about 1500 Hz, each
 * phase wandering slowly, as band-limited as RADE's own) with noise, offset as two stations' oscillators
 * put it: the estimate finds the offset, a searching decoder follows it, a
 * decoder locked a carrier off is restarted, one locked right is left
 * alone, and noise alone moves nothing.
 */
#include <math.h>
#include <stdlib.h>

#include "unity.h"
#include "dsp/rade_afc.h"

#define FS          8000.0
#define NC          14
#define CARRIER_HZ  62.5
/* RADE V2's grid starts at 1062.5 Hz, but its power centres on 1500 Hz
 * (rade_afc.c): these 14 equal carriers are centred there instead. */
#define FIRST_HZ    1093.75
#define PHASE_STEP  0.02     /* phase wander per sample (rad): ~0.5 Hz wide */
#define BLOCK       960      /* samples per decoder call, as rade_nin() */
#define SECONDS     4        /* of over fed before each check */
#define TOL_HZ      8.0f     /* the estimate's tolerance */
/* ...beyond this offset, where the over nears the floor's bins, the one the
 * wrong-carrier test allows (AFC_BRANCH_TOL_HZ) */
#define WIDE_HZ     100.0
#define TOL_WIDE_HZ 15.0f
/* A searching decoder is moved to another whole carrier only when the
 * estimate is this far off (AFC_SEARCH_HZ): what is left, within half a
 * carrier, is RADE's own to track */
#define SEARCH_HZ   45.0

static double gauss(void)
{
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * v);
}

/* n samples of over (or noise only, with over = 0) at foff Hz, at an SNR
 * in 3 kHz; *t carries the time across calls */
static void make(RADE_COMP *x, int n, double foff, double snr_db, int over, long *t)
{
    static double ph[NC];
    const double sigma = sqrt(1.0 / pow(10.0, snr_db / 10.0) * (FS / 3000.0) / 2.0);
    for (int i = 0; i < n; i++, (*t)++) {
        for (int c = 0; c < NC; c++)
            ph[c] += PHASE_STEP * gauss();
        double re = 0, im = 0;
        if (over)
            for (int c = 0; c < NC; c++) {
                const double a = 2.0 * M_PI * (FIRST_HZ + c * CARRIER_HZ + foff) * (double) *t / FS + ph[c];
                re += cos(a) / sqrt(NC);
                im += sin(a) / sqrt(NC);
            }
        x[i].real = (float) (re + sigma * gauss());
        x[i].imag = (float) (im + sigma * gauss());
    }
}

/* Feed SECONDS of over to a fresh AFC, as a searching decoder would. */
static rade_afc *feed(double foff, double snr_db, int over)
{
    rade_afc *a = rade_afc_new();
    TEST_ASSERT_NOT_NULL(a);
    RADE_COMP x[BLOCK];
    long t = 0;
    for (int k = 0; k < (int) (SECONDS * FS / BLOCK); k++) {
        make(x, BLOCK, foff, snr_db, over, &t);
        rade_afc_process(a, x, BLOCK);
        TEST_ASSERT_FALSE(rade_afc_update(a, false, 0.0f));
    }
    return a;
}

void setUp(void) { srand(1); }
void tearDown(void) {}

/* At 6 dB the estimate holds to TOL_HZ; lower, its scatter grows towards
 * the 15 Hz the decisions allow (test_low_snr, test_right_lock_left_alone). */
#define ACCURACY_SNR_DB 6.0

static void test_estimate_finds_the_offset(void)
{
    static const double offs[] = {-150, -60, -41, 0, 20, 41, 100, 150};
    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        rade_afc *a = feed(offs[i], ACCURACY_SNR_DB, 1);
        float est = 1e9f;
        const float tol = fabs(offs[i]) > WIDE_HZ ? TOL_WIDE_HZ : TOL_HZ;
        TEST_ASSERT_TRUE(rade_afc_estimate(a, &est));
        TEST_ASSERT_FLOAT_WITHIN(tol, (float) offs[i], est);
        /* a searching decoder is left alone well inside SEARCH_HZ and put
         * on the over's whole carrier well outside it (what is left within
         * RADE's own +-31.25 Hz); within the estimate's tolerance of the
         * threshold, either */
        const float nearest = (float) (CARRIER_HZ * round(offs[i] / CARRIER_HZ));
        const float shift = rade_afc_shift(a);
        if (fabs(offs[i]) < SEARCH_HZ - tol)
            TEST_ASSERT_EQUAL_FLOAT(0.0f, shift);
        else if (fabs(offs[i]) > SEARCH_HZ + tol)
            TEST_ASSERT_EQUAL_FLOAT(nearest, shift);
        else
            TEST_ASSERT_TRUE(shift == 0.0f || shift == nearest);
        rade_afc_free(a);
    }
}

static void test_low_snr(void)
{
    rade_afc *a = feed(41.0, -3.0, 1);
    float est = 1e9f;
    TEST_ASSERT_TRUE(rade_afc_estimate(a, &est));
    TEST_ASSERT_FLOAT_WITHIN(TOL_HZ * 2, 41.0f, est);
    rade_afc_free(a);
}

static void test_noise_alone_moves_nothing(void)
{
    rade_afc *a = feed(0.0, 0.0, 0);
    float est = 1e9f;
    TEST_ASSERT_FALSE(rade_afc_estimate(a, &est));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, rade_afc_shift(a));
    rade_afc_free(a);
}

/* Locked with RADE's own offset on the wrong side of a carrier: the true
 * 41 Hz read as 41 - 62.5. */
static void test_wrong_carrier_restarts(void)
{
    rade_afc *a = rade_afc_new();
    RADE_COMP x[BLOCK];
    long t = 0;
    int restarts = 0;
    for (int k = 0; k < (int) (SECONDS * FS / BLOCK); k++) {
        make(x, BLOCK, 41.0, 6.0, 1, &t);
        rade_afc_process(a, x, BLOCK);
        const float shift = rade_afc_shift(a);
        /* the decoder sees the over at 41 - shift, and reads it mod 62.5 */
        float seen = 41.0f - shift;
        seen -= (float) CARRIER_HZ * roundf(seen / (float) CARRIER_HZ);
        const float rade_foff = restarts ? seen : 41.0f - (float) CARRIER_HZ - shift;
        if (rade_afc_update(a, true, rade_foff))
            restarts++;
    }
    TEST_ASSERT_EQUAL_INT(1, restarts);
    TEST_ASSERT_FLOAT_WITHIN(TOL_HZ, (float) CARRIER_HZ, rade_afc_shift(a));
    rade_afc_free(a);
}

static void test_right_lock_left_alone(void)
{
    static const double offs[] = {-41, 0, 41};
    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        rade_afc *a = feed(offs[i], 0.0, 1);
        RADE_COMP x[BLOCK];
        long t = 0;
        for (int k = 0; k < (int) (SECONDS * FS / BLOCK); k++) {
            make(x, BLOCK, offs[i], 0.0, 1, &t);
            rade_afc_process(a, x, BLOCK);
            const float rade_foff = (float) offs[i] - rade_afc_shift(a);
            TEST_ASSERT_FALSE(rade_afc_update(a, true, rade_foff));
        }
        rade_afc_free(a);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_estimate_finds_the_offset);
    RUN_TEST(test_low_snr);
    RUN_TEST(test_noise_alone_moves_nothing);
    RUN_TEST(test_wrong_carrier_restarts);
    RUN_TEST(test_right_lock_left_alone);
    return UNITY_END();
}
