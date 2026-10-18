/* main:hamlib_conf parsing (hamlib/hamlib_conf.h). The same syntax and
 * rules as Mercury's [ptt] hamlib_conf (mercury#334). */
#include <stdio.h>
#include <string.h>

#include "hamlib/hamlib_conf.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                         printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { char keys[8][32]; char vals[8][32]; int n; } pairs_t;
static void collect(const char *k, const char *v, void *ctx)
{
    pairs_t *p = (pairs_t *) ctx;
    if (p->n < 8) {
        snprintf(p->keys[p->n], sizeof p->keys[0], "%s", k);
        snprintf(p->vals[p->n], sizeof p->vals[0], "%s", v);
        p->n++;
    }
}

int main(void)
{
    pairs_t p;

    /* rigctl --set-conf style */
    memset(&p, 0, sizeof p);
    CHECK(hamlib_conf_pairs("dtr_state=ON,civaddr=0x94", collect, &p) == 2, "two pairs");
    CHECK(!strcmp(p.keys[0], "dtr_state") && !strcmp(p.vals[0], "ON"), "first pair %s=%s", p.keys[0], p.vals[0]);
    CHECK(!strcmp(p.keys[1], "civaddr") && !strcmp(p.vals[1], "0x94"), "second pair %s=%s", p.keys[1], p.vals[1]);

    /* other separators, an empty value, nothing at all */
    memset(&p, 0, sizeof p);
    CHECK(hamlib_conf_pairs(" rts_state=OFF ; dtr_state=OFF\tptt_pathname= ", collect, &p) == 3, "three pairs");
    CHECK(!strcmp(p.keys[2], "ptt_pathname") && !strcmp(p.vals[2], ""), "empty value");
    CHECK(hamlib_conf_pairs("", collect, &p) == 0, "empty string");
    CHECK(hamlib_conf_pairs(NULL, collect, &p) == 0, "NULL");

    /* a malformed item applies nothing, not the pairs before it */
    memset(&p, 0, sizeof p);
    CHECK(hamlib_conf_pairs("dtr_state=ON,civaddr", collect, &p) == -1, "missing '='");
    CHECK(hamlib_conf_pairs("=ON", collect, &p) == -1, "empty key");
    CHECK(p.n == 0, "malformed applied %d pairs", p.n);
    char big[HAMLIB_CONF_MAX + 8];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    big[1] = '=';
    CHECK(hamlib_conf_pairs(big, collect, &p) == -1, "too long");

    /* which lines the operator took over, case-insensitively */
    CHECK(hamlib_conf_sets("civaddr=0x94,DTR_State=ON", "dtr_state"), "sets dtr_state");
    CHECK(!hamlib_conf_sets("civaddr=0x94", "rts_state"), "does not set rts_state");
    CHECK(!hamlib_conf_sets("dtr_state", "dtr_state"), "malformed sets nothing");
    CHECK(!hamlib_conf_sets(NULL, "rts_state"), "NULL sets nothing");

    if (failures) {
        printf("hamlib_conf_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("hamlib_conf_test: ok\n");
    return 0;
}
