/* Control-surface regression test.
 *
 * Drives radio_controls.c and the backend control dispatch against a fake
 * backend, so the parts that every interface (websocket, rigctld server,
 * web panel) depends on are checked without a rig: capability enumeration,
 * value clamping to the rig's advertised range and step, the JSON reports,
 * the name filter, and the "this rig does not have that control" path.
 */

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#include "radio.h"
#include "radio_backend.h"
#include "radio_controls.h"

_Atomic bool shutdown_ = false;
_Atomic bool timer_reset = false;
_Atomic time_t timeout_counter = 0;

/* The fake rig: two levels and one func, with values that persist across
 * set/get so round trips are observable. */
static double fake_rfpower = 0.0;
static int    fake_agc     = 0;
static int    fake_nb      = 0;

static const radio_ctrl_info fake_controls[] = {
    { "RFPOWER", RADIO_CTRL_LEVEL, true,  true, true, 0.0, 1.0, 0.0 },
    { "AGC",     RADIO_CTRL_LEVEL, false, true, true, 0.0, 5.0, 1.0 },
    { "NB",      RADIO_CTRL_FUNC,  false, true, true, 0.0, 1.0, 1.0 },
    /* Read-only meter, to prove can_set is honoured in the reports. */
    { "SWR",     RADIO_CTRL_LEVEL, true,  true, false, 1.0, 10.0, 0.1 },
};

static size_t fake_enumerate(radio *radio_h, radio_ctrl_info *out, size_t max)
{
    (void) radio_h;
    size_t n = sizeof(fake_controls) / sizeof(fake_controls[0]);
    if (n > max) n = max;
    memcpy(out, fake_controls, n * sizeof(*out));
    return n;
}

/* Set to make SWR answer NaN, standing in for a rig whose meter read comes
 * back as nonsense (a garbled reply, an unset power calibration). */
static bool fake_swr_nan = false;

static int fake_get_level(radio *radio_h, const char *name, double *out)
{
    (void) radio_h;
    if (!strcmp(name, "RFPOWER")) { *out = fake_rfpower; return RADIO_CTRL_OK; }
    if (!strcmp(name, "AGC"))     { *out = fake_agc;     return RADIO_CTRL_OK; }
    if (!strcmp(name, "SWR"))
    {
        *out = fake_swr_nan ? (0.0 / 0.0) : 1.5;
        return RADIO_CTRL_OK;
    }
    return RADIO_CTRL_ENOTSUP;
}

static int fake_set_level(radio *radio_h, const char *name, double value)
{
    (void) radio_h;
    if (!strcmp(name, "RFPOWER")) { fake_rfpower = value;            return RADIO_CTRL_OK; }
    if (!strcmp(name, "AGC"))     { fake_agc = (int) lrint(value);   return RADIO_CTRL_OK; }
    return RADIO_CTRL_ENOTSUP;
}

static int fake_get_func(radio *radio_h, const char *name, int *out)
{
    (void) radio_h;
    if (!strcmp(name, "NB")) { *out = fake_nb; return RADIO_CTRL_OK; }
    return RADIO_CTRL_ENOTSUP;
}

static int fake_set_func(radio *radio_h, const char *name, int on)
{
    (void) radio_h;
    if (!strcmp(name, "NB")) { fake_nb = on ? 1 : 0; return RADIO_CTRL_OK; }
    return RADIO_CTRL_ENOTSUP;
}

static const radio_backend_ops fake_ops = {
    .name               = "fake",
    .enumerate_controls = fake_enumerate,
    .get_level          = fake_get_level,
    .set_level          = fake_set_level,
    .get_func           = fake_get_func,
    .set_func           = fake_set_func,
};

/* radio_backend.c reaches for both real vtables at link time. */
const radio_backend_ops hamlib_backend_ops = { .name = "hamlib" };
const radio_backend_ops sbitx_backend_ops  = { .name = "hfsignals" };

int radio_daemon_core_run(const radio_backend_selection *selection,
                          const radio_daemon_runtime *runtime)
{
    (void) selection; (void) runtime;
    return 0;
}

#include "../cfg_utils.c"
#include "../radio_backend.c"
#include "../radio_controls.c"

static radio *make_radio(void)
{
    radio *r = calloc(1, sizeof(*r));
    r->backend_kind = RADIO_BACKEND_HAMLIB;
    r->backend_ops = &fake_ops;
    r->profiles_count = 1;
    return r;
}

static void test_enumerate(radio *r)
{
    radio_ctrl_info list[RADIO_CTRL_MAX];
    size_t n = radio_controls_enumerate(r, list, RADIO_CTRL_MAX);

    assert(n == 4);
    assert(!strcmp(list[0].name, "RFPOWER"));
    assert(list[0].kind == RADIO_CTRL_LEVEL && list[0].is_float);
    assert(list[2].kind == RADIO_CTRL_FUNC);

    radio_ctrl_info found;
    assert(radio_controls_find(r, "AGC", &found));
    assert(!found.is_float && found.max == 5.0);
    assert(!radio_controls_find(r, "VOXGAIN", &found));
}

static void test_clamp(void)
{
    radio_ctrl_info f = { "RFPOWER", RADIO_CTRL_LEVEL, true, true, true, 0.0, 1.0, 0.0 };
    assert(radio_controls_clamp(&f, 1.7) == 1.0);
    assert(radio_controls_clamp(&f, -0.5) == 0.0);
    assert(fabs(radio_controls_clamp(&f, 0.42) - 0.42) < 1e-9);

    /* Stepped integer level: rounds onto the step and stays in range. */
    radio_ctrl_info i = { "AGC", RADIO_CTRL_LEVEL, false, true, true, 0.0, 5.0, 1.0 };
    assert(radio_controls_clamp(&i, 2.4) == 2.0);
    assert(radio_controls_clamp(&i, 9.0) == 5.0);

    /* A func is on or off, whatever the client sent. */
    radio_ctrl_info fn = { "NB", RADIO_CTRL_FUNC, false, true, true, 0.0, 1.0, 1.0 };
    assert(radio_controls_clamp(&fn, 7.0) == 1.0);
    assert(radio_controls_clamp(&fn, 0.0) == 0.0);

    /* No advertised granularity: the value passes through untouched. */
    radio_ctrl_info u = { "KEYSPD", RADIO_CTRL_LEVEL, false, true, true, 0.0, 0.0, 0.0 };
    assert(radio_controls_clamp(&u, 25.0) == 25.0);
}

static void test_dispatch(radio *r)
{
    double v = 0.0;
    int on = 0;

    assert(radio_backend_set_level(r, "RFPOWER", 0.25) == RADIO_CTRL_OK);
    assert(radio_backend_get_level(r, "RFPOWER", &v) == RADIO_CTRL_OK);
    assert(fabs(v - 0.25) < 1e-9);

    assert(radio_backend_set_func(r, "NB", 1) == RADIO_CTRL_OK);
    assert(radio_backend_get_func(r, "NB", &on) == RADIO_CTRL_OK);
    assert(on == 1);

    /* A control this rig does not have, and an op the backend leaves NULL,
     * both report "not supported" rather than a fabricated value. */
    assert(radio_backend_get_level(r, "VOXGAIN", &v) == RADIO_CTRL_ENOTSUP);
    assert(radio_backend_get_rit(r, NULL) == RADIO_CTRL_ENOTSUP);
    assert(radio_backend_dump_state(r, NULL, 0) == RADIO_CTRL_ENOTSUP);
}

static void test_json(radio *r)
{
    char buf[8192];

    assert(radio_controls_caps_json(r, buf, sizeof(buf)) == RADIO_CTRL_OK);
    assert(strstr(buf, "\"count\":4"));
    assert(strstr(buf, "\"name\":\"RFPOWER\",\"kind\":\"level\",\"type\":\"float\""));
    assert(strstr(buf, "\"name\":\"NB\",\"kind\":\"func\""));
    assert(strstr(buf, "\"name\":\"SWR\",\"kind\":\"level\",\"type\":\"float\",\"get\":true,\"set\":false"));

    assert(radio_backend_set_level(r, "AGC", 3) == RADIO_CTRL_OK);
    assert(radio_controls_values_json(r, NULL, buf, sizeof(buf)) == RADIO_CTRL_OK);
    assert(strstr(buf, "\"AGC\":3"));
    assert(strstr(buf, "\"SWR\":1.5"));

    /* Values are grouped by kind, because Hamlib gives a level and a func
     * the same name (NR, RF): a flat map would drop one of each pair. */
    const char *levels = strstr(buf, "\"levels\":{");
    const char *funcs  = strstr(buf, "\"funcs\":{");
    const char *parms  = strstr(buf, "\"parms\":{");
    assert(levels && funcs && parms);
    assert(levels < funcs && funcs < parms);
    assert(strstr(levels, "\"AGC\":3") < funcs);   /* the level side */
    assert(strstr(funcs, "\"NB\":1") < parms);     /* the func side  */

    /* The name filter answers only what was asked for, tolerating spaces. */
    assert(radio_controls_values_json(r, "AGC, NB", buf, sizeof(buf)) == RADIO_CTRL_OK);
    assert(strstr(buf, "\"AGC\":3"));
    assert(strstr(buf, "\"NB\":1"));
    assert(!strstr(buf, "SWR"));

    /* A non-finite value is left out entirely: NaN and Infinity are not JSON
     * numbers, and one of them in the document would make the whole frame
     * unparseable for the client. */
    fake_swr_nan = true;
    assert(radio_controls_values_json(r, NULL, buf, sizeof(buf)) == RADIO_CTRL_OK);
    assert(!strstr(buf, "nan") && !strstr(buf, "NaN") && !strstr(buf, "inf"));
    assert(!strstr(buf, "\"SWR\""));
    assert(strstr(buf, "\"AGC\":3"));   /* the rest still reported */
    fake_swr_nan = false;

    /* A buffer too small fails loudly instead of emitting truncated JSON. */
    char tiny[32];
    assert(radio_controls_caps_json(r, tiny, sizeof(tiny)) != RADIO_CTRL_OK);
}

/* The daemon builds with -Ofast (-ffinite-math-only), where isfinite() folds
 * to a constant true. This is the check that has to keep working there. */
static void test_value_ok(void)
{
    assert(radio_controls_value_ok(0.0));
    assert(radio_controls_value_ok(-1.5));
    assert(radio_controls_value_ok(1e300));
    assert(!radio_controls_value_ok(0.0 / 0.0));    /* NaN  */
    assert(!radio_controls_value_ok(1.0 / 0.0));    /* +Inf */
    assert(!radio_controls_value_ok(-1.0 / 0.0));   /* -Inf */
}

static void test_rprt(void)
{
    assert(radio_controls_rprt(RADIO_CTRL_OK) == 0);
    assert(radio_controls_rprt(RADIO_CTRL_ENOTSUP) == -11);
    assert(radio_controls_rprt(RADIO_CTRL_EINVAL) == -1);
    assert(radio_controls_rprt(RADIO_CTRL_EIO) == -8);
}

/* hermes-voice-key adds voice_key_file to core.ini while the daemon runs:
 * the reload must pick up the new path, and a later config write-back must
 * keep the line rather than dump the startup copy without it. */
static void test_voice_key_refresh(void)
{
    char path[] = "/tmp/controls_test_coreXXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);

    FILE *f = fopen(path, "w");
    fputs("[main]\ncw_wpm = 20\n\n[tx_band0]\nscale = 1.0\n", f);
    fclose(f);

    radio *r = calloc(1, sizeof(*r));
    pthread_mutex_init(&r->cfg_mutex, NULL);
    snprintf(r->cfg_radio_path, sizeof(r->cfg_radio_path), "%s", path);
    assert(init_config_radio(r, path));
    assert(r->voice_key_file[0] == '\0');

    f = fopen(path, "w");   /* what the tool writes: inside [main] */
    fputs("[main]\ncw_wpm = 20\nvoice_key_file = /etc/hermes/voice.key\n\n"
          "[tx_band0]\nscale = 1.0\n", f);
    fclose(f);

    assert(cfg_refresh_voice_key_file(r));
    assert(!strcmp(r->voice_key_file, "/etc/hermes/voice.key"));

    assert(write_config_radio(r, path));
    dictionary *back = iniparser_load(path);
    assert(back != NULL);
    assert(!strcmp(iniparser_getstring(back, "main:voice_key_file", ""), "/etc/hermes/voice.key"));
    iniparser_freedict(back);

    close_config_radio(r);
    pthread_mutex_destroy(&r->cfg_mutex);
    free(r);
    unlink(path);
}

/* The digi parameter ranges are defined once and enforced on BOTH paths.
 * digi_config refused rtty_shift = 0 or cw_wpm = 0 over the WebSocket, but
 * the same values in the config file went straight to the encoder. */
static void test_digi_param_ranges(void)
{
    /* the shared predicate, at and around every boundary */
    assert( cfg_digi_param_valid("cw_wpm", 5)    &&  cfg_digi_param_valid("cw_wpm", 60));
    assert(!cfg_digi_param_valid("cw_wpm", 4)    && !cfg_digi_param_valid("cw_wpm", 61));
    assert(!cfg_digi_param_valid("cw_wpm", 0));
    assert( cfg_digi_param_valid("cw_pitch", 300) && cfg_digi_param_valid("cw_pitch", 1200));
    assert(!cfg_digi_param_valid("cw_pitch", 299) && !cfg_digi_param_valid("cw_pitch", 1201));
    assert( cfg_digi_param_valid("rtty_baud", 45) && cfg_digi_param_valid("rtty_baud", 50) &&
            cfg_digi_param_valid("rtty_baud", 75));
    assert(!cfg_digi_param_valid("rtty_baud", 46) && !cfg_digi_param_valid("rtty_baud", 0));
    assert( cfg_digi_param_valid("rtty_mark", 300) && cfg_digi_param_valid("rtty_mark", 3000));
    assert(!cfg_digi_param_valid("rtty_mark", 299) && !cfg_digi_param_valid("rtty_mark", 3001));
    assert( cfg_digi_param_valid("rtty_shift", 170) && cfg_digi_param_valid("rtty_shift", 425) &&
            cfg_digi_param_valid("rtty_shift", 850));
    assert(!cfg_digi_param_valid("rtty_shift", 0) && !cfg_digi_param_valid("rtty_shift", 171));
    assert( cfg_digi_param_valid("some_other_key", -1));     /* not ours to judge */

    /* and the config file can no longer bypass it: bad values fall back */
    char path[] = "/tmp/controls_test_digiXXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    FILE *f = fopen(path, "w");
    fputs("[main]\ncw_wpm = 0\ncw_pitch = 5000\nrtty_baud = 46\nrtty_mark = 10\nrtty_shift = 0\n\n"
          "[tx_band0]\nscale = 1.0\n", f);
    fclose(f);
    radio *r = calloc(1, sizeof(*r));
    pthread_mutex_init(&r->cfg_mutex, NULL);
    snprintf(r->cfg_radio_path, sizeof(r->cfg_radio_path), "%s", path);
    assert(init_config_radio(r, path));
    assert(r->cw_wpm == 20 && r->cw_pitch == 700);
    assert(r->rtty_baud == 45 && r->rtty_mark == 1585 && r->rtty_shift == 170);
    close_config_radio(r);

    /* valid values from the file are kept as given */
    f = fopen(path, "w");
    fputs("[main]\ncw_wpm = 35\ncw_pitch = 600\nrtty_baud = 75\nrtty_mark = 2125\nrtty_shift = 850\n\n"
          "[tx_band0]\nscale = 1.0\n", f);
    fclose(f);
    assert(init_config_radio(r, path));
    assert(r->cw_wpm == 35 && r->cw_pitch == 600);
    assert(r->rtty_baud == 75 && r->rtty_mark == 2125 && r->rtty_shift == 850);
    close_config_radio(r);

    pthread_mutex_destroy(&r->cfg_mutex);
    free(r);
    unlink(path);
}

int main(void)
{
    radio *r = make_radio();

    test_enumerate(r);
    test_clamp();
    test_dispatch(r);
    test_json(r);
    test_value_ok();
    test_rprt();
    test_voice_key_refresh();
    test_digi_param_ranges();

    free(r);
    printf("controls_test: all assertions passed\n");
    return 0;
}
