/* hermes-radio-daemon - configuration utilities
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 */

#include <iniparser.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "cfg_utils.h"
#include "radio.h"

extern _Atomic bool shutdown_;

static int cfg_getint_alias(dictionary *ini,
                            const char *primary,
                            const char *fallback,
                            int default_value)
{
    if (primary && iniparser_find_entry(ini, primary))
        return iniparser_getint(ini, primary, default_value);

    if (fallback && iniparser_find_entry(ini, fallback))
        return iniparser_getint(ini, fallback, default_value);

    return default_value;
}

static const char *cfg_getstring_alias(dictionary *ini,
                                       const char *primary,
                                       const char *fallback,
                                       const char *default_value)
{
    if (primary && iniparser_find_entry(ini, primary))
        return iniparser_getstring(ini, primary, (char *) default_value);

    if (fallback && iniparser_find_entry(ini, fallback))
        return iniparser_getstring(ini, fallback, (char *) default_value);

    return default_value;
}

static void cfg_copy_string(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0)
        return;

    snprintf(dst, dst_len, "%s", src ? src : "");
}

static int cfg_ptt_type_from_string(const char *value, int default_value)
{
    char *end = NULL;
    long parsed;

    if (!value || !value[0])
        return default_value;

    parsed = strtol(value, &end, 10);
    if (end && *end == '\0')
        return (int) parsed;

    if (!strcasecmp(value, "NONE"))
        return PTT_NONE;
    if (!strcasecmp(value, "RIG") || !strcasecmp(value, "CAT"))
        return PTT_RIG;
    if (!strcasecmp(value, "SERIAL_RTS") || !strcasecmp(value, "RTS"))
        return PTT_SERIAL_RTS;
    if (!strcasecmp(value, "SERIAL_DTR") || !strcasecmp(value, "DTR"))
        return PTT_SERIAL_DTR;
    if (!strcasecmp(value, "PARALLEL"))
        return PTT_PARALLEL;
    if (!strcasecmp(value, "CM108"))
        return PTT_CM108;
    if (!strcasecmp(value, "GPIO"))
        return PTT_GPIO;
    if (!strcasecmp(value, "RIG_MICDATA") ||
        !strcasecmp(value, "MICDATA") ||
        !strcasecmp(value, "DATA"))
        return PTT_RIG_MICDATA;

    return default_value;
}

radio_backend_kind cfg_backend_kind_from_string(const char *backend_name)
{
    if (!backend_name)
        return RADIO_BACKEND_HAMLIB;

    if (!strcmp(backend_name, "hfsignals") ||
        !strcmp(backend_name, "sbitx") ||
        !strcmp(backend_name, "zbitx"))
        return RADIO_BACKEND_HFSIGNALS;

    return RADIO_BACKEND_HAMLIB;
}

bool cfg_detect_backend(const char *cfg_radio, radio_backend_kind *backend_kind,
                        char *controller_path, size_t controller_path_len)
{
    dictionary *ini;
    const char *backend;
    const char *path;

    if (!backend_kind || !controller_path || controller_path_len == 0)
        return false;

    *backend_kind = RADIO_BACKEND_HAMLIB;
    snprintf(controller_path, controller_path_len, "%s", "sbitx_controller");

    ini = iniparser_load(cfg_radio);
    if (!ini)
        return false;

    backend = iniparser_getstring(ini, "main:radio_backend", "hamlib");
    *backend_kind = cfg_backend_kind_from_string(backend);

    path = iniparser_getstring(ini, "main:hfsignals_controller_path", "sbitx_controller");
    snprintf(controller_path, controller_path_len, "%s", path);

    iniparser_freedict(ini);
    return true;
}

bool cfg_init(radio *radio_h, const char *cfg_radio, const char *cfg_user,
              pthread_t *config_tid)
{
    pthread_mutex_init(&radio_h->cfg_mutex, NULL);
    snprintf(radio_h->cfg_radio_path, sizeof(radio_h->cfg_radio_path), "%s", cfg_radio);
    snprintf(radio_h->cfg_user_path, sizeof(radio_h->cfg_user_path), "%s", cfg_user);

    if (!init_config_radio(radio_h, cfg_radio) ||
        !init_config_user(radio_h, cfg_user))
    {
        close_config_radio(radio_h);
        close_config_user(radio_h);
        pthread_mutex_destroy(&radio_h->cfg_mutex);
        return false;
    }

    radio_h->cfg_radio_dirty = false;
    radio_h->cfg_user_dirty  = false;

    if (pthread_create(config_tid, NULL, config_thread, (void *) radio_h) != 0)
    {
        close_config_radio(radio_h);
        close_config_user(radio_h);
        pthread_mutex_destroy(&radio_h->cfg_mutex);
        return false;
    }

    return true;
}

bool cfg_shutdown(radio *radio_h, pthread_t *config_tid)
{
    pthread_join(*config_tid, NULL);

    close_config_radio(radio_h);
    close_config_user(radio_h);

    return true;
}

void *config_thread(void *radio_h_v)
{
    radio *radio_h = (radio *) radio_h_v;

    while (!shutdown_)
    {
        if (radio_h->cfg_radio_dirty)
        {
            write_config_radio(radio_h, radio_h->cfg_radio_path);
            radio_h->cfg_radio_dirty = false;
        }

        if (radio_h->cfg_user_dirty)
        {
            write_config_user(radio_h, radio_h->cfg_user_path);
            radio_h->cfg_user_dirty = false;
        }

        sleep(2);
    }

    return NULL;
}

bool init_config_radio(radio *radio_h, const char *ini_name)
{
    dictionary *ini;
    const char *s;
    int i;

    radio_h->cfg_radio = NULL;
    ini = iniparser_load(ini_name);
    if (!ini)
    {
        fprintf(stderr, "cfg: cannot parse radio config: %s\n", ini_name);
        return false;
    }
    radio_h->cfg_radio = ini;
    radio_h->backend_kind = cfg_backend_kind_from_string(
        iniparser_getstring(ini, "main:radio_backend", "hamlib"));
    snprintf(radio_h->hfsignals_controller_path,
             sizeof(radio_h->hfsignals_controller_path),
             "%s",
             iniparser_getstring(ini, "main:hfsignals_controller_path", "sbitx_controller"));

    /* Hamlib rig model */
    i = cfg_getint_alias(ini, "main:radio_model", "main:hamlib_model", 1);
    radio_h->hamlib_model = i;

    /* Rig port */
    s = cfg_getstring_alias(ini, "main:rig_pathname", "main:rig_path", "");
    cfg_copy_string(radio_h->rig_pathname, sizeof(radio_h->rig_pathname), s);

    /* Serial rate */
    i = iniparser_getint(ini, "main:serial_rate", 9600);
    radio_h->serial_rate = i;

    /* PTT type */
    s = cfg_getstring_alias(ini, "main:ptt_type", "main:ptt_mode", "0");
    radio_h->ptt_type = cfg_ptt_type_from_string(s, PTT_NONE);

    /* PTT port */
    s = cfg_getstring_alias(ini, "main:ptt_pathname", "main:ptt_path", "");
    cfg_copy_string(radio_h->ptt_pathname, sizeof(radio_h->ptt_pathname), s);

    /* Serial number (informational) */
    i = iniparser_getint(ini, "main:serial_number", 0);
    radio_h->serial_number = (uint32_t) i;

    /* SWR reflected threshold (vswr * 10) */
    i = iniparser_getint(ini, "main:reflected_threshold", 25);
    radio_h->reflected_threshold = (uint32_t) i;

    /* BFO (stored for API compat; not used with Hamlib) */
    i = iniparser_getint(ini, "main:bfo", 0);
    radio_h->bfo_frequency = (uint32_t) i;

    /* SHM control */
    int b = iniparser_getboolean(ini, "main:enable_shm_control", 1);
    radio_h->enable_shm_control = (bool) b;

    /* Websocket / media bridge */
    b = iniparser_getboolean(ini, "main:enable_websocket", 0);
    radio_h->enable_websocket = (bool) b;

    s = iniparser_getstring(ini, "main:websocket_bind", "0.0.0.0:8080");
    snprintf(radio_h->websocket_bind, sizeof(radio_h->websocket_bind), "%s", s);

    b = iniparser_getboolean(ini, "main:enable_audio_bridge", 0);
    radio_h->enable_audio_bridge = (bool) b;

    s = iniparser_getstring(ini, "main:capture_device", "default");
    snprintf(radio_h->capture_device, sizeof(radio_h->capture_device), "%s", s);

    s = iniparser_getstring(ini, "main:playback_device", "default");
    snprintf(radio_h->playback_device, sizeof(radio_h->playback_device), "%s", s);

    i = iniparser_getint(ini, "main:audio_sample_rate", 8000);
    radio_h->audio_sample_rate = (uint32_t) i;

    i = iniparser_getint(ini, "main:audio_period_size", 160);
    radio_h->audio_period_size = (uint32_t) i;

    i = iniparser_getint(ini, "main:audio_queue_samples", 16000);
    radio_h->audio_queue_samples = (uint32_t) i;

    s = iniparser_getstring(ini, "main:recording_dir", "/var/lib/hermes-radio-daemon");
    snprintf(radio_h->recording_dir, sizeof(radio_h->recording_dir), "%s", s);

    return true;
}

bool init_config_user(radio *radio_h, const char *ini_name)
{
    dictionary *ini;
    const char *s;
    int i;
    int b;

    radio_h->cfg_user = NULL;
    ini = iniparser_load(ini_name);
    if (!ini)
    {
        fprintf(stderr, "cfg: cannot parse user config: %s\n", ini_name);
        return false;
    }
    radio_h->cfg_user = ini;

    i = iniparser_getint(ini, "main:current_profile", 0);
    radio_h->profile_active_idx = (uint32_t) i;

    i = iniparser_getint(ini, "main:default_profile", 0);
    radio_h->profile_default_idx = (uint32_t) i;

    i = iniparser_getint(ini, "main:default_profile_fallback_timeout", -1);
    radio_h->profile_timeout = (int32_t) i;

    i = iniparser_getint(ini, "main:step_size", 100);
    radio_h->step_size = (uint32_t) i;

    b = iniparser_getboolean(ini, "main:tone_generation", 0);
    radio_h->tone_generation = (bool) b;

    /* Count profile sections (everything except [main]) */
    int sec_count = iniparser_getnsec(ini) - 1;
    if (sec_count <= 0)
        sec_count = 1;
    if (sec_count > MAX_RADIO_PROFILES)
        sec_count = MAX_RADIO_PROFILES;
    radio_h->profiles_count = (uint32_t) sec_count;

    for (int k = 0; k < sec_count && k < MAX_RADIO_PROFILES; k++)
    {
        char key[64];

        snprintf(key, sizeof(key), "profile%d:freq", k);
        i = iniparser_getint(ini, key, 7000000);
        radio_h->profiles[k].freq = (uint32_t) i;

        snprintf(key, sizeof(key), "profile%d:mode", k);
        s = iniparser_getstring(ini, key, "USB");
        if (!strcmp(s, "LSB"))
            radio_h->profiles[k].mode = MODE_LSB;
        else if (!strcmp(s, "CW"))
            radio_h->profiles[k].mode = MODE_CW;
        else
            radio_h->profiles[k].mode = MODE_USB;

        snprintf(key, sizeof(key), "profile%d:speaker_level", k);
        i = iniparser_getint(ini, key, 50);
        radio_h->profiles[k].speaker_level = (uint32_t) i;

        snprintf(key, sizeof(key), "profile%d:power_level_percentage", k);
        i = iniparser_getint(ini, key, 100);
        radio_h->profiles[k].power_level_percentage = (uint16_t) i;

        snprintf(key, sizeof(key), "profile%d:agc", k);
        s = iniparser_getstring(ini, key, "OFF");
        if (!strcmp(s, "SLOW"))        radio_h->profiles[k].agc = AGC_SLOW;
        else if (!strcmp(s, "MEDIUM")) radio_h->profiles[k].agc = AGC_MEDIUM;
        else if (!strcmp(s, "FAST"))   radio_h->profiles[k].agc = AGC_FAST;
        else                           radio_h->profiles[k].agc = AGC_OFF;

        snprintf(key, sizeof(key), "profile%d:compressor", k);
        s = iniparser_getstring(ini, key, "OFF");
        radio_h->profiles[k].compressor =
            (!strcmp(s, "ON")) ? COMPRESSOR_ON : COMPRESSOR_OFF;

        snprintf(key, sizeof(key), "profile%d:digital_voice", k);
        int b = iniparser_getboolean(ini, key, 0);
        radio_h->profiles[k].digital_voice = (bool) b;
    }

    if (radio_h->profile_active_idx >= radio_h->profiles_count)
        radio_h->profile_active_idx = 0;

    if (radio_h->profile_default_idx >= radio_h->profiles_count)
        radio_h->profile_default_idx = 0;

    return true;
}

static bool write_config_common(radio *radio_h, dictionary *dict,
                                const char *ini_name)
{
    char *bp = NULL;
    size_t sz = 0;

    FILE *stream = open_memstream(&bp, &sz);
    if (!stream)
        return false;

    pthread_mutex_lock(&radio_h->cfg_mutex);
    iniparser_dump_ini(dict, stream);
    pthread_mutex_unlock(&radio_h->cfg_mutex);

    fclose(stream);

    FILE *f = fopen(ini_name, "w");
    if (!f)
    {
        free(bp);
        return false;
    }
    fwrite(bp, sz, 1, f);
    fclose(f);
    free(bp);

    return true;
}

bool write_config_radio(radio *radio_h, const char *ini_name)
{
    return write_config_common(radio_h, radio_h->cfg_radio, ini_name);
}

bool write_config_user(radio *radio_h, const char *ini_name)
{
    return write_config_common(radio_h, radio_h->cfg_user, ini_name);
}

int cfg_set(radio *radio_h, dictionary *ini, const char *entry, const char *val)
{
    pthread_mutex_lock(&radio_h->cfg_mutex);
    int ret = iniparser_set(ini, entry, val);
    pthread_mutex_unlock(&radio_h->cfg_mutex);
    return ret;
}

bool close_config_radio(radio *radio_h)
{
    if (radio_h->cfg_radio)
        iniparser_freedict(radio_h->cfg_radio);
    return true;
}

bool close_config_user(radio *radio_h)
{
    if (radio_h->cfg_user)
        iniparser_freedict(radio_h->cfg_user);
    return true;
}
