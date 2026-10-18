/* main:hamlib_conf parsing (hamlib/hamlib_conf.h). The same syntax and
 * rules as Mercury's [ptt] hamlib_conf (mercury#334). */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "hamlib/hamlib_conf.h"

typedef struct { char keys[8][32]; char vals[8][32]; int n; } pairs_t;
static pairs_t p;

static void collect(const char *k, const char *v, void *ctx)
{
    pairs_t *pp = (pairs_t *) ctx;
    if (pp->n < 8) {
        snprintf(pp->keys[pp->n], sizeof pp->keys[0], "%s", k);
        snprintf(pp->vals[pp->n], sizeof pp->vals[0], "%s", v);
        pp->n++;
    }
}

void setUp(void) { memset(&p, 0, sizeof p); }
void tearDown(void) {}

/* rigctl --set-conf style */
static void test_comma_pairs(void)
{
    TEST_ASSERT_EQUAL_INT(2, hamlib_conf_pairs("dtr_state=ON,civaddr=0x94", collect, &p));
    TEST_ASSERT_EQUAL_STRING("dtr_state", p.keys[0]);
    TEST_ASSERT_EQUAL_STRING("ON", p.vals[0]);
    TEST_ASSERT_EQUAL_STRING("civaddr", p.keys[1]);
    TEST_ASSERT_EQUAL_STRING("0x94", p.vals[1]);
}

/* other separators, an empty value, nothing at all */
static void test_separators_and_empty(void)
{
    TEST_ASSERT_EQUAL_INT(3, hamlib_conf_pairs(" rts_state=OFF ; dtr_state=OFF\tptt_pathname= ", collect, &p));
    TEST_ASSERT_EQUAL_STRING("ptt_pathname", p.keys[2]);
    TEST_ASSERT_EQUAL_STRING("", p.vals[2]);
    TEST_ASSERT_EQUAL_INT(0, hamlib_conf_pairs("", collect, &p));
    TEST_ASSERT_EQUAL_INT(0, hamlib_conf_pairs(NULL, collect, &p));
}

/* a malformed item applies nothing, not the pairs before it */
static void test_malformed_applies_nothing(void)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, hamlib_conf_pairs("dtr_state=ON,civaddr", collect, &p), "missing '='");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, hamlib_conf_pairs("=ON", collect, &p), "empty key");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, p.n, "a malformed string applied pairs");

    char big[HAMLIB_CONF_MAX + 8];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    big[1] = '=';
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, hamlib_conf_pairs(big, collect, &p), "too long");
}

/* which lines the operator took over, case-insensitively */
static void test_sets(void)
{
    TEST_ASSERT_TRUE(hamlib_conf_sets("civaddr=0x94,DTR_State=ON", "dtr_state"));
    TEST_ASSERT_FALSE(hamlib_conf_sets("civaddr=0x94", "rts_state"));
    TEST_ASSERT_FALSE_MESSAGE(hamlib_conf_sets("dtr_state", "dtr_state"), "malformed sets nothing");
    TEST_ASSERT_FALSE(hamlib_conf_sets(NULL, "rts_state"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_comma_pairs);
    RUN_TEST(test_separators_and_empty);
    RUN_TEST(test_malformed_applies_nothing);
    RUN_TEST(test_sets);
    return UNITY_END();
}
