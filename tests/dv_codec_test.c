/* Digital voice ON/OFF in each main:digital_voice_codec: RADEv2 is the
 * profile's digital_voice flag, D-STAR is the profile in MODE_DSTAR. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#include "radio.h"
#include "radio_backend.h"

_Atomic bool shutdown_ = false;
_Atomic bool timer_reset = false;
_Atomic time_t timeout_counter = 0;

const radio_backend_ops hamlib_backend_ops = { .name = "hamlib" };
const radio_backend_ops sbitx_backend_ops = { .name = "hfsignals" };

int radio_daemon_core_run(const radio_backend_selection *selection,
                          const radio_daemon_runtime *runtime)
{
    (void) selection;
    (void) runtime;
    return 0;
}

#include "../cfg_utils.c"
#include "../radio_backend.c"

/* the backend side as the sbitx does it: store the value */
static void stub_set_mode(radio *radio_h, uint16_t mode, uint32_t profile)
{
    radio_h->profiles[profile].mode = mode;
}

static void stub_set_digital_voice(radio *radio_h, bool dv, uint32_t profile)
{
    radio_h->profiles[profile].digital_voice = dv;
}

static const radio_backend_ops stub_ops = {
    .name = "stub",
    .set_mode = stub_set_mode,
    .set_digital_voice = stub_set_digital_voice,
};

static radio radio_h;

static void setup(uint16_t codec)
{
    if (radio_h.cfg_user)
        iniparser_freedict(radio_h.cfg_user);
    if (radio_h.cfg_radio)
        iniparser_freedict(radio_h.cfg_radio);
    memset(&radio_h, 0, sizeof(radio_h));
    pthread_mutex_init(&radio_h.cfg_mutex, NULL);
    radio_h.cfg_user = dictionary_new(0);
    radio_h.cfg_radio = dictionary_new(0);
    iniparser_set(radio_h.cfg_radio, "main", NULL);
    iniparser_set(radio_h.cfg_user, "profile0", NULL);
    iniparser_set(radio_h.cfg_user, "profile1", NULL);
    radio_h.backend_kind = RADIO_BACKEND_HFSIGNALS;
    radio_h.backend_ops = &stub_ops;
    radio_h.digital_voice_codec = codec;
    radio_h.profiles_count = 2;
    radio_h.profiles[0].mode = MODE_USB;
    radio_h.profiles[1].mode = MODE_LSB;
    radio_h.profiles[1].dv_restore_mode = MODE_USB;
    radio_h.txrx_state = IN_RX;
}

static const char *user_key(const char *key)
{
    return iniparser_getstring(radio_h.cfg_user, key, "");
}

static void test_radev2(void)
{
    setup(DV_CODEC_RADEV2);
    assert(radio_backend_set_digital_voice(&radio_h, true, 1));
    assert(radio_h.profiles[1].digital_voice);
    assert(radio_h.profiles[1].mode == MODE_LSB);
    assert(radio_backend_get_digital_voice(&radio_h, 1));
    assert(radio_backend_set_digital_voice(&radio_h, false, 1));
    assert(!radio_backend_get_digital_voice(&radio_h, 1));
}

static void test_dstar_round_trip(void)
{
    setup(DV_CODEC_DSTAR);
    assert(radio_backend_set_digital_voice(&radio_h, true, 1));
    assert(radio_h.profiles[1].mode == MODE_DSTAR);
    assert(!radio_h.profiles[1].digital_voice);
    assert(radio_backend_get_digital_voice(&radio_h, 1));
    assert(!strcmp(user_key("profile1:dv_restore_mode"), "LSB"));
    /* profile 0 is untouched */
    assert(radio_h.profiles[0].mode == MODE_USB);
    assert(!radio_backend_get_digital_voice(&radio_h, 0));

    assert(radio_backend_set_digital_voice(&radio_h, false, 1));
    assert(radio_h.profiles[1].mode == MODE_LSB);
    assert(!radio_backend_get_digital_voice(&radio_h, 1));
    assert(!strcmp(user_key("profile1:dv_restore_mode"), ""));
}

static void test_refused_while_transmitting(void)
{
    setup(DV_CODEC_DSTAR);
    radio_h.txrx_state = IN_TX;
    assert(!radio_backend_set_digital_voice(&radio_h, true, 1));
    assert(radio_h.profiles[1].mode == MODE_LSB);
    /* asking for the state it is already in is not a change */
    assert(radio_backend_set_digital_voice(&radio_h, false, 1));
    assert(!radio_backend_set_digital_voice_codec(&radio_h, DV_CODEC_RADEV2));
    assert(radio_h.digital_voice_codec == DV_CODEC_DSTAR);
}

static void test_codec_switch_keeps_digital_voice(void)
{
    setup(DV_CODEC_RADEV2);
    assert(radio_backend_set_digital_voice(&radio_h, true, 1));
    assert(radio_backend_set_digital_voice_codec(&radio_h, DV_CODEC_DSTAR));
    assert(radio_h.profiles[1].mode == MODE_DSTAR);
    assert(!radio_h.profiles[1].digital_voice);
    assert(radio_backend_get_digital_voice(&radio_h, 1));
    assert(!radio_backend_get_digital_voice(&radio_h, 0));
    assert(!strcmp(iniparser_getstring(radio_h.cfg_radio, "main:digital_voice_codec", ""),
                   "DSTAR"));

    assert(radio_backend_set_digital_voice_codec(&radio_h, DV_CODEC_RADEV2));
    assert(radio_h.profiles[1].mode == MODE_LSB);
    assert(radio_h.profiles[1].digital_voice);
    assert(radio_backend_get_digital_voice(&radio_h, 1));
}

static void test_hamlib_has_no_dstar_digital_voice(void)
{
    setup(DV_CODEC_RADEV2);
    radio_h.backend_kind = RADIO_BACKEND_HAMLIB;
    assert(!radio_backend_dv_codec_supported(&radio_h, DV_CODEC_DSTAR));
    assert(!radio_backend_set_digital_voice_codec(&radio_h, DV_CODEC_DSTAR));
    assert(radio_h.digital_voice_codec == DV_CODEC_RADEV2);
}

static void test_startup_carries_digital_voice(void)
{
    /* saved under RADEv2, started under DSTAR */
    setup(DV_CODEC_DSTAR);
    radio_h.profiles[1].digital_voice = true;
    cfg_carry_digital_voice(&radio_h);
    assert(radio_h.profiles[1].mode == MODE_DSTAR);
    assert(radio_h.profiles[1].dv_restore_mode == MODE_LSB);
    assert(radio_backend_get_digital_voice(&radio_h, 1));
    assert(!strcmp(user_key("profile1:dv_restore_mode"), "LSB"));
    assert(!strcmp(user_key("profile1:mode"), "DSTAR"));

    /* then started under RADEV2 again */
    radio_h.digital_voice_codec = DV_CODEC_RADEV2;
    cfg_carry_digital_voice(&radio_h);
    assert(radio_h.profiles[1].mode == MODE_LSB);
    assert(radio_h.profiles[1].digital_voice);
    assert(!strcmp(user_key("profile1:mode"), "LSB"));
    assert(!strcmp(user_key("profile1:dv_restore_mode"), ""));

    /* DSTAR picked as a mode, not as digital voice, stays DSTAR */
    setup(DV_CODEC_RADEV2);
    radio_h.profiles[1].mode = MODE_DSTAR;
    cfg_carry_digital_voice(&radio_h);
    assert(radio_h.profiles[1].mode == MODE_DSTAR);
    assert(!radio_h.profiles[1].digital_voice);
}

int main(void)
{
    test_radev2();
    test_dstar_round_trip();
    test_refused_while_transmitting();
    test_codec_switch_keeps_digital_voice();
    test_hamlib_has_no_dstar_digital_voice();
    test_startup_carries_digital_voice();
    puts("dv_codec_test: ok");
    return 0;
}
