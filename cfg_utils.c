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
#include <stdio.h>

#include "cfg_utils.h"
#include "radio.h"

extern _Atomic bool shutdown_;

bool cfg_init(radio *radio_h, const char *cfg_radio, const char *cfg_user,
              pthread_t *config_tid)
{
    pthread_mutex_init(&radio_h->cfg_mutex, NULL);
    snprintf(radio_h->cfg_radio_path, sizeof(radio_h->cfg_radio_path), "%s", cfg_radio);
    snprintf(radio_h->cfg_user_path, sizeof(radio_h->cfg_user_path), "%s", cfg_user);

    init_config_radio(radio_h, cfg_radio);
    init_config_user(radio_h, cfg_user);

    radio_h->cfg_radio_dirty = false;
    radio_h->cfg_user_dirty  = false;

    pthread_create(config_tid, NULL, config_thread, (void *) radio_h);

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

    /* Hamlib rig model */
    i = iniparser_getint(ini, "main:radio_model", 1);
    radio_h->hamlib_model = i;

    /* Rig port */
    s = iniparser_getstring(ini, "main:rig_pathname", "");
    strncpy(radio_h->rig_pathname, s, sizeof(radio_h->rig_pathname) - 1);

    /* Serial rate */
    i = iniparser_getint(ini, "main:serial_rate", 9600);
    radio_h->serial_rate = i;

    /* PTT type */
    i = iniparser_getint(ini, "main:ptt_type", PTT_NONE);
    radio_h->ptt_type = i;

    /* PTT port */
    s = iniparser_getstring(ini, "main:ptt_pathname", "");
    strncpy(radio_h->ptt_pathname, s, sizeof(radio_h->ptt_pathname) - 1);

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
    if (sec_count < 0) sec_count = 0;
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
