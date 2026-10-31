/* sBitx controller
 * Copyright (C) 2023-2024 Rhizomatica
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

#include <stdio.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sbitx_alsa.h"
#include "sbitx_dsp.h"
#include "sbitx_buffer.h"
#include "radio_media.h"
#include "audio_bridge.h"
#include "modem_audio.h"
#include "../dsp/mic_filter.h"

#include "radiod_log.h"

#define LOG_C "alsa"
char *radio_capture_dev = "hw:0,0";
char *radio_playback_dev = "hw:0,0";
char *loop_capture_dev = "hw:2,1";
char *loop_playback_dev = "hw:1,0";

#define MIC_INJECT_PATH "/tmp/sbitx_mic_inject.s32"
#define RX_SPEAKER_DUMP_TRIGGER "/tmp/sbitx_rx_speaker_dump"
#define RX_SPEAKER_DUMP_PATH "/tmp/sbitx_rx_speaker_dump.s32"
#define MIC_DUMP_TRIGGER "/tmp/sbitx_mic_dump"
#define MIC_DUMP_PATH "/tmp/sbitx_mic_dump.s32"

// mixer device
char *radio_ctl = "hw:0";

snd_pcm_t *pcm_capture_handle;
snd_pcm_t *pcm_play_handle;
snd_pcm_t *loopback_capture_handle;
snd_pcm_t *loopback_play_handle;

static int mic_inject_fd = -1;
static bool mic_inject_missing_logged = false;
static FILE *rx_speaker_dump_fp = NULL;
static FILE *mic_dump_fp = NULL;

unsigned int hw_rate = 96000; /* Sample rate */
snd_pcm_uframes_t hw_period_size = 512; // in frames
uint64_t hw_n_periods = 4; // number of periods
/* Codec playback: 8 periods (43 ms) with 4 primed, so a stall of up to
 * ~21 ms does not underrun. The codec DMA hands over two periods at a
 * time (every ~10.7 ms), so 4 periods left one hand-over of slack. The
 * extra latency is played out at PTT-off (sound_tx_pipeline_ms). */
static const unsigned hw_play_n_periods = 8;

/* Frames queued in the codec playback at its last write. */
static _Atomic int32_t play_queued_frames = 0;

unsigned int loopback_rate = 48000; /* Sample rate */
enum { LOOPBACK_PERIOD = 256 }; // frames: at 48 kHz, one 512-frame period of the 96 kHz codec
uint64_t loopback_n_periods = 4; // number of periods

snd_pcm_format_t format = SND_PCM_FORMAT_S32_LE;
uint32_t channels = 2;

// local radio handle used to easy parameter passing
static radio *radio_h_snd;
extern _Atomic bool shutdown_;

// should we allow level control even in IO-ONLY mode?
#define ALLOW_ALSA_LEVELS_IN_IO_ONLY 1

static void close_mic_inject(void)
{
    if (mic_inject_fd >= 0)
        close(mic_inject_fd);

    mic_inject_fd = -1;
}

static void close_rx_speaker_dump(void)
{
    if (rx_speaker_dump_fp)
        fclose(rx_speaker_dump_fp);

    rx_speaker_dump_fp = NULL;
    if (mic_dump_fp)
        fclose(mic_dump_fp);
    mic_dump_fp = NULL;
}

// Optional test hook: if /tmp/sbitx_mic_inject.s32 exists, use it as the TX
// source for the normal mic path. Format must be raw S32_LE, 96 kHz, mono.
static bool read_mic_inject(uint8_t *buffer, uint32_t size)
{
    size_t offset = 0;

    /* Follow the path, not the open file: deleting or replacing the file
     * must end or change the injection. Holding the descriptor kept a
     * deleted file looping, so every later transmission sent it in place
     * of the real mic -- silence, from a test that had injected zeros. */
    if (mic_inject_fd >= 0)
    {
        struct stat path_st, fd_st;
        if (stat(MIC_INJECT_PATH, &path_st) != 0 || fstat(mic_inject_fd, &fd_st) != 0 ||
            path_st.st_ino != fd_st.st_ino || path_st.st_dev != fd_st.st_dev)
        {
            HLOGI(LOG_C, "Mic inject ended: %s removed or replaced", MIC_INJECT_PATH);
            close_mic_inject();
        }
    }

    while (offset < size)
    {
        if (mic_inject_fd < 0)
        {
            struct stat st;

            mic_inject_fd = open(MIC_INJECT_PATH, O_RDONLY | O_CLOEXEC);
            if (mic_inject_fd < 0)
            {
                if (!mic_inject_missing_logged && errno != ENOENT)
                    HLOGE(LOG_C, "Could not open mic inject file %s (%s)", MIC_INJECT_PATH, strerror(errno));

                mic_inject_missing_logged = true;
                return false;
            }

            if (fstat(mic_inject_fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 0)
            {
                HLOGW(LOG_C, "Mic inject file %s is empty", MIC_INJECT_PATH);
                close_mic_inject();
                mic_inject_missing_logged = true;
                return false;
            }

            HLOGI(LOG_C, "Mic inject active: %s", MIC_INJECT_PATH);
            mic_inject_missing_logged = false;
        }

        ssize_t n = read(mic_inject_fd, buffer + offset, size - offset);
        if (n > 0)
        {
            offset += (size_t) n;
            continue;
        }

        if (n == 0)
        {
            if (lseek(mic_inject_fd, 0, SEEK_SET) >= 0)
                continue;

            HLOGI(LOG_C, "Mic inject stream ended for %s", MIC_INJECT_PATH);
            close_mic_inject();
            return false;
        }

        HLOGE(LOG_C, "Mic inject read failed for %s (%s)", MIC_INJECT_PATH, strerror(errno));
        close_mic_inject();
        return false;
    }

    return true;
}

/* Optional test hooks. While the trigger file exists, the matching 96 kHz
 * mono S32_LE buffer is appended to the dump file; removing the trigger
 * closes it. Each hook checks its trigger about every 250 ms (24 of its
 * own blocks: one counter per hook -- a shared one advanced twice a block,
 * so only one of the two hooks ever reached a check).
 *   /tmp/sbitx_rx_speaker_dump -> the speaker buffer, receiving only
 *   /tmp/sbitx_mic_dump        -> the raw mic buffer, always (the mic is
 *                                 read while receiving too, so no need to
 *                                 key to record an idle mic) */
static void dump_hook(FILE **fp, unsigned *tick, const char *trigger, const char *path,
                      const char *name, const uint8_t *buffer, uint32_t size)
{
    if (((*tick)++ % 24) == 0)
    {
        bool on = access(trigger, F_OK) == 0;
        if (on && !*fp)
        {
            *fp = fopen(path, "wb");
            if (!*fp)
                HLOGE(LOG_C, "Could not open %s dump file %s (%s)", name, path, strerror(errno));
            else
                HLOGI(LOG_C, "%s dump active: %s", name, path);
        }
        else if (!on && *fp)
        {
            fclose(*fp);
            *fp = NULL;
            HLOGI(LOG_C, "%s dump closed: %s", name, path);
        }
    }
    if (*fp && buffer && size)
        fwrite(buffer, 1, size, *fp);
}

static void maybe_dump_rx_speaker(const uint8_t *buffer, uint32_t size, bool active_rx)
{
    static unsigned tick;
    if (active_rx)
        dump_hook(&rx_speaker_dump_fp, &tick, RX_SPEAKER_DUMP_TRIGGER, RX_SPEAKER_DUMP_PATH,
                  "RX speaker", buffer, size);
}

static void maybe_dump_mic(const uint8_t *buffer, uint32_t size)
{
    static unsigned tick;
    dump_hook(&mic_dump_fp, &tick, MIC_DUMP_TRIGGER, MIC_DUMP_PATH, "Mic", buffer, size);
}

void show_alsa(snd_pcm_t *handle, snd_pcm_hw_params_t *params)
{
    unsigned int val, val2;
    int dir = 0;
    snd_pcm_uframes_t frames;

    /* Display information about the PCM interface */

    HLOGD(LOG_C, "PCM handle name = '%s'", snd_pcm_name(handle));

    HLOGD(LOG_C, "PCM state = %s", snd_pcm_state_name(snd_pcm_state(handle)));

    snd_pcm_hw_params_get_access(params, (snd_pcm_access_t *) &val);
    HLOGD(LOG_C, "access type = %s",snd_pcm_access_name((snd_pcm_access_t)val));

    snd_pcm_hw_params_get_format(params, (snd_pcm_format_t *)&val);
    HLOGD(LOG_C, "format = '%s' (%s)",
           snd_pcm_format_name((snd_pcm_format_t)val),
           snd_pcm_format_description(
               (snd_pcm_format_t)val));

    snd_pcm_hw_params_get_subformat(params, (snd_pcm_subformat_t *)&val);
    HLOGD(LOG_C, "subformat = '%s' (%s)",
           snd_pcm_subformat_name((snd_pcm_subformat_t)val),
           snd_pcm_subformat_description(
               (snd_pcm_subformat_t)val));

    snd_pcm_hw_params_get_channels(params, &val);
    HLOGD(LOG_C, "channels = %d", val);

    snd_pcm_hw_params_get_rate(params, &val, &dir);
    HLOGD(LOG_C, "rate = %d Hz", val);

    snd_pcm_hw_params_get_periods(params, &val, &dir);
    HLOGD(LOG_C, "periods per buffer = %d frames", val);

    snd_pcm_hw_params_get_period_size(params, &frames, &dir);
    HLOGD(LOG_C, "period size = %d frames", (int)frames);

    snd_pcm_hw_params_get_buffer_size(params, (snd_pcm_uframes_t *) &val);
    HLOGD(LOG_C, "buffer size = %d frames", val);

    snd_pcm_hw_params_get_period_time(params, &val, &dir);
    HLOGD(LOG_C, "period time = %d us", val);

    snd_pcm_hw_params_get_buffer_time(params, &val, &dir);
    HLOGD(LOG_C, "buffer time = %d us", val);

    snd_pcm_hw_params_get_rate_numden(params, &val, &val2);
    HLOGD(LOG_C, "exact rate = %d/%d Hz", val, val2);

    val = snd_pcm_hw_params_get_sbits(params);
    HLOGD(LOG_C, "significant bits = %d", val);

    val = snd_pcm_hw_params_is_batch(params);
    HLOGD(LOG_C, "is batch double buffering = %d", val);

    val = snd_pcm_hw_params_is_block_transfer(params);
    HLOGD(LOG_C, "is block transfer = %d", val);

    val = snd_pcm_hw_params_is_double(params);
    HLOGD(LOG_C, "is double buffered = %d", val);

    val = snd_pcm_hw_params_is_half_duplex(params);
    HLOGD(LOG_C, "is half duplex = %d", val);

    val = snd_pcm_hw_params_is_joint_duplex(params);
    HLOGD(LOG_C, "is joint duplex = %d", val);

    val = snd_pcm_hw_params_can_overrange(params);
    HLOGD(LOG_C, "can overrange = %d", val);

    val = snd_pcm_hw_params_can_mmap_sample_resolution(params);
    HLOGD(LOG_C, "can mmap = %d", val);

    val = snd_pcm_hw_params_can_pause(params);
    HLOGD(LOG_C, "can pause = %d", val);

    val = snd_pcm_hw_params_can_resume(params);
    HLOGD(LOG_C, "can resume = %d", val);

    val = snd_pcm_hw_params_can_sync_start(params);
    HLOGD(LOG_C, "can sync start = %d", val);

    val = snd_pcm_hw_params_get_fifo_size(params);
    HLOGD(LOG_C, "fifo size = %d", val);

    val = snd_pcm_hw_params_is_monotonic(params);
    HLOGD(LOG_C, "is monotonic = %d", val);

}

// this is the radio rx level
void set_rx_level(uint32_t rx_level)
{
#if ALLOW_ALSA_LEVELS_IN_IO_ONLY == 0
    if (radio_h_snd->profiles[radio_h_snd->profile_active_idx].operating_mode == OPERATING_MODE_CONTROLS_ONLY)
        return;
#endif

    long min, max;
    snd_mixer_t *handle;
    snd_mixer_selem_id_t *sid;

    // TODO: lets keep these handle open?
    snd_mixer_open(&handle, 0);
    snd_mixer_attach(handle, radio_ctl);
    snd_mixer_selem_register(handle, NULL, NULL);
    snd_mixer_load(handle);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "Capture");
    snd_mixer_elem_t *elem = snd_mixer_find_selem(handle, sid);

    long volume = rx_level;
    snd_mixer_selem_get_capture_volume_range(elem, &min, &max);

    // left channel of the audio codec
    snd_mixer_selem_set_capture_volume(elem, SND_MIXER_SCHN_FRONT_LEFT,  volume * max / 100);

    snd_mixer_close(handle);
}

void set_mic_level(uint32_t mic_level)
{
#if ALLOW_ALSA_LEVELS_IN_IO_ONLY == 0
    if (radio_h_snd->profiles[radio_h_snd->profile_active_idx].operating_mode == OPERATING_MODE_CONTROLS_ONLY)
        return;
#endif

    long min, max;
    snd_mixer_t *handle;
    snd_mixer_selem_id_t *sid;

    // TODO: lets keep these handle open?
    snd_mixer_open(&handle, 0);
    snd_mixer_attach(handle, radio_ctl);
    snd_mixer_selem_register(handle, NULL, NULL);
    snd_mixer_load(handle);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "Capture");
    snd_mixer_elem_t *elem = snd_mixer_find_selem(handle, sid);

    long volume = mic_level;
    snd_mixer_selem_get_capture_volume_range(elem, &min, &max);

    // right channel of the audio codec
    snd_mixer_selem_set_capture_volume(elem, SND_MIXER_SCHN_FRONT_RIGHT,  volume * max / 100);

    snd_mixer_close(handle);
}

void set_speaker_level(uint32_t speaker_level)
{
#if ALLOW_ALSA_LEVELS_IN_IO_ONLY == 0
    if (radio_h_snd->profiles[radio_h_snd->profile_active_idx].operating_mode == OPERATING_MODE_CONTROLS_ONLY)
        return;
#endif

    long min, max;
    snd_mixer_t *handle;
    snd_mixer_selem_id_t *sid;

    // TODO: lets keep these handle open?
    snd_mixer_open(&handle, 0);
    snd_mixer_attach(handle, radio_ctl);
    snd_mixer_selem_register(handle, NULL, NULL);
    snd_mixer_load(handle);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "Master");
    snd_mixer_elem_t *elem = snd_mixer_find_selem(handle, sid);

    long volume = speaker_level;
    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);

    // left channel of the audio codec
    snd_mixer_selem_set_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT,  volume * max / 100);

    snd_mixer_close(handle);
}

void set_tx_level(uint32_t tx_level)
{
#if ALLOW_ALSA_LEVELS_IN_IO_ONLY == 0
    if (radio_h_snd->profiles[radio_h_snd->profile_active_idx].operating_mode == OPERATING_MODE_CONTROLS_ONLY)
        return;
#endif

    long min, max;
    snd_mixer_t *handle;
    snd_mixer_selem_id_t *sid;

    // TODO: lets keep these handle open?
    snd_mixer_open(&handle, 0);
    snd_mixer_attach(handle, radio_ctl);
    snd_mixer_selem_register(handle, NULL, NULL);
    snd_mixer_load(handle);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "Master");
    snd_mixer_elem_t *elem = snd_mixer_find_selem(handle, sid);

    long volume = tx_level;
    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);

    // right channel of the audio codec
    snd_mixer_selem_set_playback_volume(elem, SND_MIXER_SCHN_FRONT_RIGHT,  volume * max / 100);

    snd_mixer_close(handle);
}


void setup_audio_codec()
{
#if ALLOW_ALSA_LEVELS_IN_IO_ONLY == 0
    if (radio_h_snd->profiles[radio_h_snd->profile_active_idx].operating_mode == OPERATING_MODE_CONTROLS_ONLY)
        return;
#endif

    //configure mixer controls
    sound_mixer(radio_ctl, "Line", 1);
    sound_mixer(radio_ctl, "Mic", 0);
    sound_mixer(radio_ctl, "Mic Boost", 0);
    sound_mixer(radio_ctl, "Playback Deemphasis", 0);
    sound_mixer(radio_ctl, "Input Mux", 0);

    sound_mixer(radio_ctl, "ADC High Pass Filter", 0);
    sound_mixer(radio_ctl, "Output Mixer HiFi", 1);
    sound_mixer(radio_ctl, "Master Playback ZC", 0);
    sound_mixer(radio_ctl, "Sidetone", 0);
    sound_mixer(radio_ctl, "Output Mixer Mic Sidetone", 0);
    sound_mixer(radio_ctl, "Output Mixer Line Bypass", 0);
    sound_mixer(radio_ctl, "Store DC Offset", 0);

    if (radio_h_snd->txrx_state == IN_TX)
    {
        set_speaker_level(0);
        set_tx_level(radio_h_snd->profiles[radio_h_snd->profile_active_idx].tx_level);
    }
    else
    {
        set_speaker_level(radio_h_snd->profiles[radio_h_snd->profile_active_idx].speaker_level);
        set_tx_level(0);
    }
    set_mic_level(radio_h_snd->profiles[radio_h_snd->profile_active_idx].mic_level);
    set_rx_level(radio_h_snd->profiles[radio_h_snd->profile_active_idx].rx_level);
}

void sound_mixer(char *card_name, char *element, int make_on)
{
    // alsa-less operation
    if (radio_h_snd->profiles[radio_h_snd->profile_active_idx].operating_mode == OPERATING_MODE_CONTROLS_ONLY)
        return;

    long min, max;
    snd_mixer_t *handle;
    snd_mixer_selem_id_t *sid;
    char *card = card_name;

    // TODO: lets keep these handle open?
    snd_mixer_open(&handle, 0);
    snd_mixer_attach(handle, card);
    snd_mixer_selem_register(handle, NULL, NULL);
    snd_mixer_load(handle);

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, element);
    snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);

    //find out if the his element is capture side or plaback
    if(snd_mixer_selem_has_capture_switch(elem))
    {
        snd_mixer_selem_set_capture_switch_all(elem, make_on);
    }
    else if (snd_mixer_selem_has_playback_switch(elem))
    {
        snd_mixer_selem_set_playback_switch_all(elem, make_on);
    }
    else if (snd_mixer_selem_has_playback_volume(elem))
    {
        long volume = make_on;
        snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
        snd_mixer_selem_set_playback_volume_all(elem, volume * max / 100);
    }
    else if (snd_mixer_selem_has_capture_volume(elem))
    {
        long volume = make_on;
        snd_mixer_selem_get_capture_volume_range(elem, &min, &max);
        snd_mixer_selem_set_capture_volume_all(elem, volume * max / 100);
    }
    else if (snd_mixer_selem_is_enumerated(elem))
    {
        snd_mixer_selem_set_enum_item(elem, 0, make_on);
    }
    snd_mixer_close(handle);
}


/* Playback that keeps running through an underrun, as OSS does. With the
 * stop threshold at the boundary ALSA never stops the stream, and with the
 * silence size at the boundary it keeps the part of the buffer we have not
 * written silent. A late period then plays as silence for exactly the time
 * it was late, and the device stays in step with capture, instead of stop,
 * prepare, restart and a fresh prime on every underrun. Playback starts
 * once `prime` periods of silence and the first real period are queued. */
static void play_setup_sw(snd_pcm_t *pcm, snd_pcm_uframes_t period,
                          unsigned prime, const char *name)
{
    snd_pcm_sw_params_t *sw;
    snd_pcm_uframes_t boundary = 0;
    int e;

    snd_pcm_sw_params_alloca(&sw);
    if ((e = snd_pcm_sw_params_current(pcm, sw)) < 0 ||
        (e = snd_pcm_sw_params_get_boundary(sw, &boundary)) < 0 ||
        (e = snd_pcm_sw_params_set_start_threshold(pcm, sw, (prime + 1) * period)) < 0 ||
        (e = snd_pcm_sw_params_set_avail_min(pcm, sw, period)) < 0 ||
        (e = snd_pcm_sw_params_set_stop_threshold(pcm, sw, boundary)) < 0 ||
        (e = snd_pcm_sw_params_set_silence_threshold(pcm, sw, 0)) < 0 ||
        (e = snd_pcm_sw_params_set_silence_size(pcm, sw, boundary)) < 0 ||
        (e = snd_pcm_sw_params(pcm, sw)) < 0)
        HLOGE(LOG_C, "%s: sw_params failed (%s)", name, snd_strerror(e));
}

static void play_prime(snd_pcm_t *pcm, const uint8_t *silence,
                       snd_pcm_uframes_t period, unsigned prime)
{
    for (unsigned p = 0; p < prime; p++)
        snd_pcm_mmap_writei(pcm, silence, period);
}

/* When the device has played past what we wrote (an underrun that did not
 * stop it), move our write position to where it is, plus `prime` frames of
 * the silence ALSA keeps there, so that we are ahead again. Returns the
 * frames that were played as silence, 0 when on time. Logs at most every
 * 100 ms, with a count, so a starved loopback cannot flood the journal. */
static snd_pcm_sframes_t play_catch_up(snd_pcm_t *pcm, snd_pcm_uframes_t buffer_frames,
                                       snd_pcm_uframes_t prime, unsigned rate,
                                       const char *name)
{
    static _Thread_local struct timespec last_log;
    static _Thread_local unsigned pending;
    snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm);

    if (avail < 0 || (snd_pcm_uframes_t) avail <= buffer_frames)
        return 0;

    snd_pcm_sframes_t late = avail - (snd_pcm_sframes_t) buffer_frames;
    snd_pcm_forward(pcm, (snd_pcm_uframes_t) late + prime);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pending++;
    if ((now.tv_sec - last_log.tv_sec) * 1000 + (now.tv_nsec - last_log.tv_nsec) / 1000000 >= 100)
    {
        HLOGW(LOG_C, "%s: underrun, %.1f ms played as silence (stream kept running%s)",
                name, late * 1000.0 / rate,
                pending > 1 ? ", more in the last 100 ms" : "");
        last_log = now;
        pending = 0;
    }
    return late;
}

/* Open a PCM, waiting while another process still holds it: a radiod (or a
 * test daemon) that is still shutting down keeps the codec for a moment, and
 * giving up at once left the daemon running with no audio at all. Bounded; any other error returns at once. */
#define PCM_BUSY_WAIT_MS 10000
static int pcm_open_wait(snd_pcm_t **pcm, const char *device, snd_pcm_stream_t stream)
{
    for (int waited_ms = 0; ; waited_ms += 250)
    {
        int e = snd_pcm_open(pcm, device, stream, 0);
        if (e != -EBUSY || waited_ms >= PCM_BUSY_WAIT_MS || shutdown_)
        {
            if (e == 0 && waited_ms > 0)
                HLOGI(LOG_C, "audio device %s: free after %d ms", device, waited_ms);
            return e;
        }
        if (waited_ms == 0)
            HLOGW(LOG_C, "audio device %s is busy (another process has it); "
                            "waiting up to %d s", device, PCM_BUSY_WAIT_MS / 1000);
        usleep(250 * 1000);
    }
}

static void codec_opened(void);

static void *radio_capture_thread(void *device_ptr)
{
    char *device = (char *) device_ptr;
    uint32_t exact_rate;

    int e;
    snd_pcm_hw_params_t *hwparams;

    if ((e = snd_pcm_hw_params_malloc (&hwparams)) < 0)
    {
        HLOGE(LOG_C, "Can not allocate hardware parameter structure (%s)", snd_strerror (e));
        return NULL;
    }

    if ((e = pcm_open_wait(&pcm_capture_handle, device, SND_PCM_STREAM_CAPTURE)) < 0)
    {
        HLOGE(LOG_C, "Can not open audio device %s (%s)", device, snd_strerror (e));
        return NULL;
    }

    HLOGI(LOG_C, "ALSA Capture device is: %s", device);

    if ((e = snd_pcm_hw_params_any(pcm_capture_handle, hwparams)) < 0)
    {
        HLOGE(LOG_C, "Error setting capture access (%d)", e);
        return NULL;
    }

    if ((e = snd_pcm_hw_params_set_access(pcm_capture_handle, hwparams, SND_PCM_ACCESS_MMAP_INTERLEAVED)) < 0)
    {
        HLOGE(LOG_C, "Error setting capture access.");
        return NULL;
    }

    e = snd_pcm_hw_params_set_format(pcm_capture_handle, hwparams, format);
    if (e < 0)
    {
        HLOGE(LOG_C, "Error setting capture format.");
        return NULL;
    }


    exact_rate = hw_rate;
    e = snd_pcm_hw_params_set_rate_near(pcm_capture_handle, hwparams, &exact_rate, 0);
    if ( e < 0)
    {
        HLOGE(LOG_C, "Error setting capture rate.");
        return NULL;
    }

    if (hw_rate != exact_rate)
        HLOGI(LOG_C, "Capture rate %d changed to %d Hz", hw_rate, exact_rate);

    if ((e = snd_pcm_hw_params_set_channels(pcm_capture_handle, hwparams, channels)) < 0)
    {
        HLOGE(LOG_C, "Error setting capture channels.");
        return NULL;
    }

    if ((e = snd_pcm_hw_params_set_period_size_near(pcm_capture_handle,hwparams, &hw_period_size, 0)) < 0)
    {
        HLOGE(LOG_C, "Can not set period size (%s)",snd_strerror (e));
        return NULL;
    }

    if ((e = snd_pcm_hw_params_set_periods(pcm_capture_handle, hwparams, hw_n_periods, 0)) < 0)
    {
        HLOGE(LOG_C, "Error setting capture periods.");
        return NULL;
    }

    if ((e = snd_pcm_hw_params(pcm_capture_handle, hwparams)) < 0)
    {
        HLOGE(LOG_C, "Error setting capture HW params.");
        return NULL;
    }

#ifdef DEBUG_
    HLOGD(LOG_C, "============= REPORT RADIO CAPTURE DEVICE %s =================", device);
    show_alsa(pcm_capture_handle, hwparams);
    HLOGD(LOG_C, "==============================================================");
#endif

    int sample_size = snd_pcm_format_width(format) / 8;
    uint32_t buffer_size = hw_period_size * sample_size * channels;

    uint8_t *buffer = malloc(buffer_size);
    uint8_t *radio = (uint8_t *) malloc(buffer_size/2);
    uint8_t *mic = (uint8_t *) malloc(buffer_size/2);

    snd_pcm_prepare(pcm_capture_handle);
    snd_pcm_drop(pcm_capture_handle);
    snd_pcm_prepare(pcm_capture_handle);
    codec_opened();

    while (!shutdown_)
    {

        if ((e = snd_pcm_mmap_readi(pcm_capture_handle, buffer, hw_period_size)) != hw_period_size)
        {
            HLOGE(LOG_C, "read from audio interface %s failed (%s)", device, snd_strerror (e));
            if (e == -EPIPE)
            {
                HLOGW(LOG_C, "%s: overrun", device);
            }
            else if (e < 0)
            {
                HLOGE(LOG_C, "%s: error from readi: %s", device, snd_strerror(e));
            } else if (e != hw_period_size)
            {
                HLOGW(LOG_C, "%s: short read, read %d frames", device, e);
            }
            snd_pcm_prepare (pcm_capture_handle);
            continue;
        }

        for (int j = 0; j < hw_period_size; j++)
        {
            memcpy(&radio[j*sample_size], &buffer[j * sample_size * channels], sample_size);
            memcpy(&mic[j*sample_size], &buffer[j * sample_size * channels + sample_size], sample_size);
        }

        sbitx_buf_write(radio_to_dsp, radio, buffer_size/2);
        sbitx_buf_write(mic_to_dsp, mic, buffer_size/2);
    }

    snd_pcm_hw_params_free(hwparams);
    free(buffer);
    free(radio);
    free(mic);

    return NULL;
}


/* Audio already on its way to the transmitter, in ms: Mercury's samples in
 * the loopback ring (48 kHz stereo S32), the DSP block in flight (1024
 * samples at 96 kHz), the samples waiting for the playback thread (96 kHz
 * mono S32) and the codec's queue. Both rings hold 384 bytes per ms. */
uint32_t sound_tx_pipeline_ms(void)
{
    unsigned long bytes = 0;
    if (loopback_to_dsp)
        bytes += sbitx_buf_size(loopback_to_dsp);
    if (dsp_to_radio)
        bytes += sbitx_buf_size(dsp_to_radio);
    return (uint32_t) (bytes / 384 + 11 + play_queued_frames / 96);
}

static void *radio_playback_thread(void *device_ptr)
{
    char *device = (char *) device_ptr;
    uint32_t exact_rate;

    int e;
    snd_pcm_hw_params_t *hwparams;

    if ((e = snd_pcm_hw_params_malloc (&hwparams)) < 0)
    {
        HLOGE(LOG_C, "Can not allocate hardware parameter structure (%s)", snd_strerror (e));
        return NULL;
    }

    if ((e = pcm_open_wait(&pcm_play_handle, device, SND_PCM_STREAM_PLAYBACK)) < 0)
    {
        HLOGE(LOG_C, "Can not open audio device %s (%s)", device, snd_strerror (e));
        return NULL;
    }


    HLOGI(LOG_C, "ALSA Playback device is: %s", device);

    if ((e = snd_pcm_hw_params_any(pcm_play_handle, hwparams)) < 0)
    {
        HLOGE(LOG_C, "Error getting hw playback params (%d)", e);
        return NULL;
    }

    if ((e = snd_pcm_hw_params_set_access(pcm_play_handle, hwparams, SND_PCM_ACCESS_MMAP_INTERLEAVED)) < 0)
    {
        HLOGE(LOG_C, "Error setting playback access.");
        return NULL;
    }

    if ((e = snd_pcm_hw_params_set_format(pcm_play_handle, hwparams, format)) < 0)
    {
        HLOGE(LOG_C, "Error setting playback format.");
        return NULL;
    }

    exact_rate = hw_rate;
    e = snd_pcm_hw_params_set_rate_near(pcm_play_handle, hwparams, &exact_rate, 0);
    if ( e < 0)
    {
        HLOGE(LOG_C, "Error setting playback rate.");
        return NULL;
    }

    if (hw_rate != exact_rate)
        HLOGI(LOG_C, "Playback rate %d changed to %d Hz", hw_rate, exact_rate);

        /* Set number of channels */
    if ((e = snd_pcm_hw_params_set_channels(pcm_play_handle, hwparams, channels)) < 0)
    {
        HLOGE(LOG_C, "Error setting playback channels.");
        return NULL;
    }

    /* Set period size. */
    if ((e = snd_pcm_hw_params_set_period_size_near(pcm_play_handle,hwparams, &hw_period_size, 0)) < 0)
    {
        HLOGE(LOG_C, "Can not set period size (%s)", snd_strerror(e));
        return NULL;
    }

    /* nr. of periods */
    if ((e = snd_pcm_hw_params_set_periods(pcm_play_handle, hwparams, hw_play_n_periods, 0)) < 0)
    {
        HLOGE(LOG_C, "Error setting playback periods.");
        return NULL;
    }

    if ((e = snd_pcm_hw_params(pcm_play_handle, hwparams)) < 0)
    {
        HLOGE(LOG_C, "Error setting playback HW params.");
        return NULL;
    }

#ifdef DEBUG_
    HLOGD(LOG_C, "============= REPORT RADIO PLAYBACK DEVICE %s ================", device);
    show_alsa(pcm_play_handle, hwparams);
    HLOGD(LOG_C, "==============================================================");
#endif

    int sample_size = snd_pcm_format_width(format) / 8;
    uint32_t buffer_size = hw_period_size * sample_size * channels;

    uint8_t *buffer = malloc(buffer_size);
    uint8_t *radio = (uint8_t *) malloc(buffer_size/2);
    uint8_t *speaker = (uint8_t *) malloc(buffer_size/2);
    uint8_t *silence = (uint8_t *) calloc(1, buffer_size);

    /* Slack. The capture, DSP and playback threads run in lockstep on the
     * codec's clock, so the playback queue only holds what was written
     * ahead of it; ALSA's default start threshold starts playback on the
     * first period, leaving one period (5.3 ms) of slack, and any hiccup
     * past it underran -- in the middle of transmissions, cutting data
     * bursts the peer then could not decode (bench, 23 Sep 2026). Queue
     * PLAY_PRIME_PERIODS of silence first and start once the first real
     * period is behind them: ~10.7 ms more latency, twice the slack. The
     * same holds after an underrun, which re-primes. */
    enum { PLAY_PRIME_PERIODS = 4 };
    play_setup_sw(pcm_play_handle, hw_period_size, PLAY_PRIME_PERIODS, "radio playback");

    snd_pcm_prepare(pcm_play_handle);
    snd_pcm_drop(pcm_play_handle);
    snd_pcm_prepare(pcm_play_handle);
    play_prime(pcm_play_handle, silence, hw_period_size, PLAY_PRIME_PERIODS);
    codec_opened();

    while (!shutdown_)
    {

        sbitx_buf_read(dsp_to_radio, radio, buffer_size/2);
        sbitx_buf_read(dsp_to_speaker, speaker, buffer_size/2);

        for (int j = 0; j < hw_period_size; j++)
        {
            memcpy(&buffer[j*sample_size*channels], &speaker[j*sample_size], sample_size);
            memcpy(&buffer[j*sample_size*channels + sample_size], &radio[j*sample_size], sample_size);
        }

        play_catch_up(pcm_play_handle, hw_period_size * hw_play_n_periods,
                      PLAY_PRIME_PERIODS * hw_period_size, hw_rate, "radio playback");

        {
            snd_pcm_sframes_t queued;
            if (snd_pcm_delay(pcm_play_handle, &queued) == 0 && queued >= 0)
                play_queued_frames = (int32_t) queued;
        }

    try_again_radio_play:
        if ((e = snd_pcm_mmap_writei(pcm_play_handle, buffer, hw_period_size)) != hw_period_size)
        {
            HLOGE(LOG_C, "write to audio interface %s failed (%s)", device, snd_strerror (e));
            if (e == -EPIPE)
            {
                HLOGW(LOG_C, "%s: underrun", device);
            }
            else if (e < 0)
            {
                HLOGE(LOG_C, "%s: error from writei: %s", device, snd_strerror(e));
            } else if (e != hw_period_size)
            {
                HLOGW(LOG_C, "%s: short write, wrote %d frames", device, e);
            }
            /* Only reached if the stream stopped anyway (an error other
             * than the underruns play_catch_up absorbs). */
            snd_pcm_prepare (pcm_play_handle);
            play_prime(pcm_play_handle, silence, hw_period_size, PLAY_PRIME_PERIODS);
            goto try_again_radio_play;
        }
    }

    snd_pcm_hw_params_free(hwparams);
    free(buffer);
    free(silence);

    return NULL;
}

/* The snd-aloop cables run with timer_source=hw:0,0: the codec's period
 * clocks them, and both ends of a cable must use the same period size, set
 * by whoever opens it first. radiod's LOOPBACK_PERIOD at 48 kHz is exactly
 * one codec period; a cable the modem opened first keeps the modem's period
 * (e.g. 288 frames), no longer lines up with the timer, and the stream
 * XRUNs on every tick, which no re-prepare can fix. Say so plainly. */
static void loop_check_period(const char *device, snd_pcm_uframes_t period)
{
    if (period != LOOPBACK_PERIOD)
        HLOGW(LOG_C, "loopback %s: the cable is already set to %lu-frame periods, not %d "
                        "(the modem opened it first?). It will not run in step with the "
                        "codec: restart the modem now that radiod is up.",
                device, (unsigned long) period, LOOPBACK_PERIOD);
}

/* The codec and loop threads report here once their device is configured,
 * so that sound_system_wait_ready() can tell systemd the radio has audio and
 * the cables are open. */
static pthread_mutex_t loop_ready_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  loop_ready_cond = PTHREAD_COND_INITIALIZER;
static int             loop_ready_count, loop_ready_expected;
/* The snd-aloop cables run only with main:enable_loop_audio: a modem in
 * radiod or on RTP takes its audio from modem_audio, and with nothing on
 * the cables their buffer only fills. */
static bool            loop_started;
static int             codec_ready_count, codec_ready_expected;

static void loop_opened(void)
{
    pthread_mutex_lock(&loop_ready_lock);
    loop_ready_count++;
    pthread_cond_broadcast(&loop_ready_cond);
    pthread_mutex_unlock(&loop_ready_lock);
}

static void codec_opened(void)
{
    pthread_mutex_lock(&loop_ready_lock);
    codec_ready_count++;
    pthread_cond_broadcast(&loop_ready_cond);
    pthread_mutex_unlock(&loop_ready_lock);
}

bool sound_system_wait_ready(int timeout_ms, bool *codec_ok)
{
    struct timespec dl;
    bool ready;

    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += timeout_ms / 1000;
    dl.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L)
    {
        dl.tv_sec++;
        dl.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&loop_ready_lock);
    while ((loop_ready_count < loop_ready_expected || codec_ready_count < codec_ready_expected) &&
           pthread_cond_timedwait(&loop_ready_cond, &loop_ready_lock, &dl) == 0)
        ;
    ready = loop_ready_count >= loop_ready_expected;
    *codec_ok = codec_ready_count >= codec_ready_expected;
    pthread_mutex_unlock(&loop_ready_lock);
    return ready;
}

void *loop_capture_thread(void *device_ptr)
{
    /* Per thread: both loop threads negotiate a period, and on a cable the
     * modem opened first the result can differ from LOOPBACK_PERIOD. */
    snd_pcm_uframes_t period = LOOPBACK_PERIOD;
    char *device = (char *) device_ptr;
    uint32_t exact_rate;

    int e;
    snd_pcm_hw_params_t *hloop_params;

    if ((e = snd_pcm_hw_params_malloc (&hloop_params)) < 0)
    {
        HLOGE(LOG_C, "Can not allocate hardware parameter structure (%s)", snd_strerror (e));
        return NULL;
    }

    if ((e = pcm_open_wait(&loopback_capture_handle, device, SND_PCM_STREAM_CAPTURE)) < 0)
    {
        HLOGE(LOG_C, "Can not open audio device %s (%s)", device, snd_strerror (e));
        return NULL;
    }

    HLOGI(LOG_C, "ALSA Loopback Capture device at: %s", device);

    if ((e = snd_pcm_hw_params_any(loopback_capture_handle, hloop_params)) < 0)
    {
        HLOGE(LOG_C, "Error setting capture access (%d)", e);
        return NULL;
    }

    if ((e = snd_pcm_hw_params_set_access(loopback_capture_handle, hloop_params, SND_PCM_ACCESS_MMAP_INTERLEAVED)) < 0)
    {
        HLOGE(LOG_C, "Error setting capture access.");
        return NULL;
    }

    /* Set sample format */
    if ((e = snd_pcm_hw_params_set_format(loopback_capture_handle, hloop_params, format)) < 0)
    {
        HLOGE(LOG_C, "Error setting loopback capture format.");
        return NULL;
    }

    exact_rate = loopback_rate;
    if ((e = snd_pcm_hw_params_set_rate_near(loopback_capture_handle, hloop_params, &exact_rate, 0)) < 0)
    {
        HLOGE(LOG_C, "Error setting loopback capture rate.");
        return NULL;
    }

    if (loopback_rate != exact_rate)
        HLOGI(LOG_C, "The loopback capture rate set to %d Hz", exact_rate);

    /* Set number of channels */
    if ((e = snd_pcm_hw_params_set_channels(loopback_capture_handle, hloop_params, channels)) < 0)
    {
        HLOGE(LOG_C, "Error setting loopback capture channels.");
        return NULL;
    }

    /* Set period size. */
    if ((e = snd_pcm_hw_params_set_period_size_near(loopback_capture_handle, hloop_params, &period, 0)) < 0)
    {
        HLOGE(LOG_C, "cannot set period size (%s)",snd_strerror (e));
        return NULL;
    }

    if ((e = snd_pcm_hw_params_set_periods(loopback_capture_handle, hloop_params, loopback_n_periods, 0)) < 0) {
        HLOGE(LOG_C, "Error setting loopback capture periods.");
        return NULL;
    }


    if ((e = snd_pcm_hw_params(loopback_capture_handle, hloop_params)) < 0)
    {
        HLOGE(LOG_C, "Error setting capture HW params.");
        return NULL;
    }
    loop_check_period(device, period);
    loop_opened();

#ifdef DEBUG_
    HLOGD(LOG_C, "============= REPORT LOOPBACK CAPTURE DEVICE %s ==============", device);
    show_alsa(loopback_capture_handle, hloop_params);
    HLOGD(LOG_C, "==============================================================");
#endif
    // TODO: apply sw parameters... ?

    int sample_size = snd_pcm_format_width(format) / 8;
    uint32_t buffer_size = period * sample_size * channels;

    uint8_t *buffer = malloc(buffer_size);

    snd_pcm_prepare(loopback_capture_handle);
    snd_pcm_drop(loopback_capture_handle);
    snd_pcm_prepare(loopback_capture_handle);

    while (!shutdown_)
    {

        if ((e = snd_pcm_mmap_readi(loopback_capture_handle, buffer, period)) != period)
        {

            HLOGE(LOG_C, "read from audio interface %s failed (%s)", device, snd_strerror (e));
            if (e == -EPIPE)
            {
                HLOGW(LOG_C, "%s: overrun", device);
            }
            else if (e < 0) {
                HLOGE(LOG_C, "%s: error from readi: %s", device, snd_strerror(e));
            } else if (e != period)
            {
                HLOGW(LOG_C, "%s: short read, read %d frames", device, e);
            }
            snd_pcm_prepare (loopback_capture_handle);
            continue;
        }

        sbitx_buf_write(loopback_to_dsp, buffer, buffer_size);
    }

    snd_pcm_hw_params_free(hloop_params);
    free(buffer);

    return NULL;
}

void *loop_playback_thread(void *device_ptr)
{
    /* Per thread: both loop threads negotiate a period, and on a cable the
     * modem opened first the result can differ from LOOPBACK_PERIOD. */
    snd_pcm_uframes_t period = LOOPBACK_PERIOD;
    char *device = (char *) device_ptr;
    uint32_t exact_rate;

    int e;
    snd_pcm_hw_params_t *hloop_params;


    if ((e = snd_pcm_hw_params_malloc (&hloop_params)) < 0)
    {
        HLOGE(LOG_C, "Can not allocate hardware parameter structure (%s)", snd_strerror (e));
        return NULL;
    }

    if ((e = pcm_open_wait(&loopback_play_handle, device, SND_PCM_STREAM_PLAYBACK)) < 0)
    {
        HLOGE(LOG_C, "Can not open audio device %s (%s)", device, snd_strerror (e));
        return NULL;
    }

    HLOGI(LOG_C, "ALSA Loopback Playback device at: %s", device);

    if ((e = snd_pcm_hw_params_any(loopback_play_handle, hloop_params)) < 0)
    {
        HLOGE(LOG_C, "Error getting loopback playback params (%d)", e);
        return NULL;
    }

    if ((e = snd_pcm_hw_params_set_access(loopback_play_handle, hloop_params, SND_PCM_ACCESS_MMAP_INTERLEAVED)) < 0)
    {
        HLOGE(LOG_C, "Error setting loopback access.");
        return NULL;
    }

    /* Set sample format */
    if ((e = snd_pcm_hw_params_set_format(loopback_play_handle, hloop_params, format)) < 0)
    {
        HLOGE(LOG_C, "Error setting loopback format.");
        return NULL;
    }

    exact_rate = loopback_rate;
    if ((e = snd_pcm_hw_params_set_rate_near(loopback_play_handle, hloop_params, &exact_rate, 0)) < 0)
    {
        HLOGE(LOG_C, "Error setting playback rate.");
        return NULL;
    }

    if (loopback_rate != exact_rate)
        HLOGI(LOG_C, "Loopback playback rate %d changed to %d Hz", loopback_rate, exact_rate);


    /* Set number of channels */
    if ((e = snd_pcm_hw_params_set_channels(loopback_play_handle, hloop_params, channels)) < 0)
    {
        HLOGE(LOG_C, "Error setting playback channels.");
        return NULL;
    }

    /* Set period size. */
    if ((e = snd_pcm_hw_params_set_period_size_near(loopback_play_handle, hloop_params, &period, 0)) < 0)
    {
        HLOGE(LOG_C, "cannot set period size (%s)",snd_strerror (e));
        return NULL;
    }


    if ((e = snd_pcm_hw_params_set_periods(loopback_play_handle, hloop_params, loopback_n_periods, 0)) < 0)
    {
        HLOGE(LOG_C, "Error setting playback periods.");
        return NULL;
    }

    if ((e = snd_pcm_hw_params(loopback_play_handle, hloop_params)) < 0)
    {
        HLOGE(LOG_C, "Error setting loopback playback HW params.");
        return NULL;
    }
    loop_check_period(device, period);
    loop_opened();

#ifdef DEBUG_
    HLOGD(LOG_C, "============= REPORT LOOPBACK PLAYBACK DEVICE %s =============", device);
    show_alsa(loopback_play_handle, hloop_params);
    HLOGD(LOG_C, "==============================================================");
#endif

    int sample_size = snd_pcm_format_width(format) / 8;
    uint32_t buffer_size = period * sample_size * channels;

    uint8_t *buffer = malloc(buffer_size);
    uint8_t *silence = (uint8_t *) calloc(1, buffer_size);

    /* The receive audio to Mercury. At every switch back to RX the rings
     * are cleared, and the loopback then ran dry: it restarted on one
     * period after each underrun and underran again, dozens of times in a
     * row. Same treatment as the codec: 2 periods of slack, and keep
     * running through an underrun. */
    enum { LOOP_PRIME_PERIODS = 2 };
    play_setup_sw(loopback_play_handle, period, LOOP_PRIME_PERIODS,
                  "loopback playback");

    snd_pcm_prepare(loopback_play_handle);
    snd_pcm_drop(loopback_play_handle);
    snd_pcm_prepare(loopback_play_handle);
    play_prime(loopback_play_handle, silence, period, LOOP_PRIME_PERIODS);

    while (!shutdown_)
    {
        sbitx_buf_read(dsp_to_loopback, buffer, buffer_size);

        play_catch_up(loopback_play_handle, period * loopback_n_periods,
                      LOOP_PRIME_PERIODS * period, loopback_rate,
                      "loopback playback");

    try_again_loop_play:
        if ((e = snd_pcm_mmap_writei(loopback_play_handle, buffer, period)) != period)
        {
            HLOGE(LOG_C, "write to audio interface %s failed (%s)", device, snd_strerror (e));
            if (e == -EPIPE)
            {
                HLOGW(LOG_C, "%s: overrun", device);
            }
            else if (e < 0)
            {
                HLOGE(LOG_C, "%s: error from writei: %s", device, snd_strerror(e));
            } else if (e != period)
            {
                HLOGW(LOG_C, "%s: short write, wrote %d frames", device, e);
            }

            snd_pcm_prepare (loopback_play_handle);
            play_prime(loopback_play_handle, silence, period, LOOP_PRIME_PERIODS);
            goto try_again_loop_play;
        }
    }

    snd_pcm_hw_params_free(hloop_params);
    free(buffer);
    free(silence);

    return NULL;
}

void *control_thread(void *device_ptr)
{
    int sample_size = snd_pcm_format_width(format) / 8;

    /* hfsignals: native ALSA capture is 96 kHz mono int16, daemon DSP rings
     * are 96 kHz → the bridge is a pass-through (no resampler). It only adds
     * the rx/tx tap (recording + spectrum) and the ring push/pop. */
    audio_bridge bridge;
    if (!audio_bridge_init(&bridge, hw_rate, hw_rate))
        HLOGE(LOG_C, "audio_bridge_init failed; bridge disabled");

    // TODO: DSP with 512 sample window?
    // we have 96 kHz in the radio soundcard, and 48 kHz in the loopback soundcard
    // we define our block transfer size as the minimum of both, in order to try to reduce latency a bit
    // uint32_t block_size = hw_period_size;
    // As Farhan's DSP code needs 1024 samples window to work (which we are currently using) we force 1024
    uint32_t block_size = 1024;

    // as we are hardcoding block sizes... this gets false
#if 0
    if (hw_period_size != (LOOPBACK_PERIOD * 2))
    {
        HLOGE(LOG_C, "Hardware 96 kHz sound period size != (Loopback 48 kHz period size * 2)");
        block_size = hw_period_size;
    }
#endif

    uint32_t buffer_size = block_size * sample_size;

    uint8_t *buffer_radio_to_dsp = malloc(buffer_size);
    uint8_t *buffer_mic_to_dsp = malloc(buffer_size);
    uint8_t *buffer_mic_inject = malloc(buffer_size);
    uint8_t *buffer_loop_to_dsp = malloc(buffer_size);

    uint8_t *signal_to_tx;
    uint8_t *output_speaker; uint8_t *output_loopback; uint8_t *output_tx;

    output_tx = malloc(buffer_size);
    output_speaker = malloc(buffer_size);
    output_loopback = malloc(buffer_size);

    uint8_t *buffer_null = malloc(buffer_size);
    memset(buffer_null, 0, buffer_size);

    while (!shutdown_)
    {
        // TODO: finish external DSP integration
        // halt this loop on external DSP
        // check_external_dsp(radio_h_snd);

        _Atomic bool use_loopback = (radio_h_snd->profiles[radio_h_snd->profile_active_idx].operating_mode == OPERATING_MODE_FULL_LOOPBACK) ? true : false;

        sbitx_buf_read(radio_to_dsp, buffer_radio_to_dsp, buffer_size); // mono
        sbitx_buf_read(mic_to_dsp, buffer_mic_to_dsp, buffer_size); // mono
        maybe_dump_mic(buffer_mic_to_dsp, buffer_size);

        /* High-pass the mic before any TX voice path sees it (DC, mains
         * hum). Runs on every block so the filter state stays continuous;
         * the cutoff follows core.ini mic_highpass_hz (0 = off). */
        {
            static mic_hpf mic_filter;
            static bool mic_filter_ready;
            unsigned hz = radio_h_snd->mic_highpass_hz;
            if (!mic_filter_ready || hz != mic_filter.cutoff_hz)
            {
                mic_hpf_setup(&mic_filter, hz, 96000);
                mic_filter_ready = true;
                if (mic_filter.cutoff_hz)
                    HLOGI(LOG_C, "mic high-pass: %u Hz, 4th-order Butterworth", mic_filter.cutoff_hz);
                else
                    HLOGI(LOG_C, "mic high-pass: off");
            }
            mic_hpf_run_s32(&mic_filter, (int32_t *) buffer_mic_to_dsp, block_size);
        }

        static int16_t modem_tx[2048];
        size_t modem_n = block_size / 2;        /* stereo 48 kHz frames per block */
        if (modem_n > sizeof(modem_tx) / sizeof(modem_tx[0]))
            modem_n = sizeof(modem_tx) / sizeof(modem_tx[0]);

        if (use_loopback && modem_audio_running() &&
            modem_audio_pop_tx(modem_tx, modem_n) == modem_n)
        {
            /* A modem holds PTT (modem_audio): its audio replaces the
             * loopback capture, in the same stereo S32 layout. */
            int32_t *lb = (int32_t *) buffer_loop_to_dsp;
            for (size_t k = 0; k < modem_n; k++)
                lb[2 * k] = lb[2 * k + 1] = (int32_t) ((uint32_t) (uint16_t) modem_tx[k] << 16);
            sbitx_buf_clear(loopback_to_dsp);
            signal_to_tx = buffer_loop_to_dsp;
        }
        else if (use_loopback)      /* no modem transmission: the loopback, as before */
        {
            // in case the alsa loopback device is not started, it will block in the read()
            if (sbitx_buf_size(loopback_to_dsp) >= buffer_size)
            {
                sbitx_buf_read(loopback_to_dsp, buffer_loop_to_dsp, buffer_size); // stereo interleaved
                signal_to_tx = buffer_loop_to_dsp;
            }
            else
            {
                HLOGD(LOG_C, "no data from the loopback capture device; skipping");
                signal_to_tx = buffer_null;
            }
        }
        else
        {
            sbitx_buf_clear(loopback_to_dsp);
            if (radio_h_snd->txrx_state == IN_TX && read_mic_inject(buffer_mic_inject, buffer_size))
                signal_to_tx = buffer_mic_inject;
            else
                signal_to_tx = buffer_mic_to_dsp;
        }

        if (radio_h_snd->txrx_state == IN_RX)
        {
            dsp_process_rx(buffer_radio_to_dsp, output_speaker, output_loopback, output_tx, block_size);
            maybe_dump_rx_speaker(output_speaker, buffer_size, true);

            if (radio_h_snd->enable_audio_bridge)
            {
                static int16_t bridge_rx_buf[1024];
                int32_t *spk = (int32_t *) output_speaker;
                for (uint32_t k = 0; k < block_size; k++)
                    bridge_rx_buf[k] = (int16_t) (spk[k] >> 16);
                /* Pushes to rx_audio_ring + taps recording/spectrum. */
                audio_bridge_push_rx_native(&bridge, radio_h_snd,
                                            bridge_rx_buf, block_size);
            }
        }
        else
        {
            if (radio_h_snd->enable_audio_bridge)
            {
                static int16_t bridge_tx_buf[1024];
                /* Pulls from tx_audio_ring + taps TX recording. */
                size_t got = audio_bridge_pop_tx_native(&bridge, radio_h_snd,
                                                       bridge_tx_buf, block_size);
                if (got > 0)
                {
                    static uint8_t tx_bridge_buf_raw[4096];
                    int32_t *tx_samples = (int32_t *) tx_bridge_buf_raw;
                    for (size_t k = 0; k < got; k++)
                        tx_samples[k] = (int32_t) bridge_tx_buf[k] << 16;
                    if (got < block_size)
                        memset(tx_samples + got, 0, (block_size - got) * sizeof(int32_t));
                    signal_to_tx = tx_bridge_buf_raw;
                }
            }

            dsp_process_tx(signal_to_tx, output_speaker, output_loopback, output_tx, block_size, use_loopback);
        }

        /* The modem feed (output_loopback: 48 kHz stereo S32, both channels
         * equal, at fm * 2^27) also goes out as the RTP RX stream, scaled so
         * the demodulator's full scale is int16 full scale. */
        if (modem_audio_running())
        {
            static int16_t modem_rx[2048];
            const int32_t *lb = (const int32_t *) output_loopback;
            size_t n = block_size / 2;
            if (n > sizeof(modem_rx) / sizeof(modem_rx[0]))
                n = sizeof(modem_rx) / sizeof(modem_rx[0]);
            for (size_t k = 0; k < n; k++)
            {
                int32_t v = lb[2 * k] >> 12;
                modem_rx[k] = (int16_t) (v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
            modem_audio_push_rx(modem_rx, n, 48000);
        }

        if (!loop_started)
            ;   /* no cable to play to */
        else if (sbitx_buf_free_size(dsp_to_loopback) >= buffer_size)
            sbitx_buf_write(dsp_to_loopback, output_loopback, buffer_size); // stereo 48 kHz interleaved
        else
        {
            /* Nothing reads the cable (no modem on it, or it failed to
             * open): once every 10 s, with the count, not once a block,
             * which flooded the journal and pushed everything else out. */
            static unsigned full;
            static time_t last;
            time_t now = time(NULL);
            full++;
            if (now - last >= 10)
            {
                HLOGW(LOG_C, "Buffer full dsp_to_loopback, cleaning it (%u times)", full);
                full = 0;
                last = now;
            }
            sbitx_buf_clear(dsp_to_loopback);
        }

        if (sbitx_buf_free_size(dsp_to_radio) >= buffer_size)
            sbitx_buf_write(dsp_to_radio, output_tx, buffer_size); // mono 96 kHz
        else
            HLOGW(LOG_C, "Buffer full dsp_to_radio!");

        if (sbitx_buf_free_size(dsp_to_speaker) >= buffer_size)
            sbitx_buf_write(dsp_to_speaker, output_speaker, buffer_size); // mono 96 kHz
        else
            HLOGW(LOG_C, "Buffer full dsp_to_speaker!");
    }

    free(buffer_null);
    free(output_tx);
    free(output_speaker);
    free(output_loopback);
    free(buffer_mic_inject);
    close_mic_inject();
    close_rx_speaker_dump();
    audio_bridge_shutdown(&bridge);

    return NULL;
}


void clear_buffers()
{
    sbitx_buf_clear(radio_to_dsp);
    sbitx_buf_clear(dsp_to_radio);
    sbitx_buf_clear(mic_to_dsp);
    sbitx_buf_clear(dsp_to_speaker);
    sbitx_buf_clear(dsp_to_loopback);
    sbitx_buf_clear(loopback_to_dsp);
}

// initialize the ALSA sound system
void sound_system_init(radio *radio_h, pthread_t *control_tid, pthread_t *radio_capture,
                       pthread_t *radio_playback, pthread_t *loop_capture, pthread_t *loop_playback)
{
    radio_h_snd = radio_h;

    setup_audio_codec();

    if (radio_h->profiles[radio_h->profile_active_idx].operating_mode == OPERATING_MODE_CONTROLS_ONLY)
        return;

    sbitx_buf_init_all();

    loop_started = radio_h->enable_loop_audio;
    pthread_mutex_lock(&loop_ready_lock);
    loop_ready_expected = loop_started ? 2 : 0;  /* loop capture + loop playback */
    codec_ready_expected = 2;           /* codec capture + codec playback */
    pthread_mutex_unlock(&loop_ready_lock);

    pthread_create(radio_playback, NULL, radio_playback_thread, (void*)radio_playback_dev);
    if (loop_started)
        pthread_create(loop_playback, NULL, loop_playback_thread, (void*)loop_playback_dev);

    pthread_create(control_tid, NULL, control_thread, NULL);

    pthread_create(radio_capture, NULL, radio_capture_thread, (void*)radio_capture_dev);
    if (loop_started)
        pthread_create(loop_capture, NULL, loop_capture_thread, (void*)loop_capture_dev);
    else
        HLOGI(LOG_C, "snd-aloop cables off (main:enable_loop_audio = 0)");


    struct sched_param sch;
    sch.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(*radio_capture, SCHED_FIFO, &sch);
    pthread_setschedparam(*radio_playback, SCHED_FIFO, &sch);
    if (loop_started)
    {
        pthread_setschedparam(*loop_capture, SCHED_FIFO, &sch);
        pthread_setschedparam(*loop_playback, SCHED_FIFO, &sch);
    }

    /* The DSP thread sits between the capture and the playback threads and
     * has to keep pace with them, but it ran at normal priority: anything
     * else on the daemon's core (spectra for the websocket, CAT, the I2C
     * power polling while transmitting, RADAE) could hold it past the
     * codec's ~21 ms of buffer, and the codec underran in the middle of
     * transmissions. Just below the I/O threads, which only move buffers;
     * it blocks on its input, so it cannot starve the core. */
    sch.sched_priority = sched_get_priority_max(SCHED_FIFO) - 1;
    pthread_setschedparam(*control_tid, SCHED_FIFO, &sch);
}

// shutdown the ALSA sound system
void sound_system_shutdown(radio *radio_h, pthread_t *control_tid, pthread_t *radio_capture,
                           pthread_t *radio_playback, pthread_t *loop_capture, pthread_t *loop_playback)
{
    if (radio_h->profiles[radio_h->profile_active_idx].operating_mode == OPERATING_MODE_CONTROLS_ONLY)
        return;

    pthread_join(*radio_playback, NULL);
    if (loop_started)
        pthread_join(*loop_playback, NULL);

    pthread_join(*control_tid, NULL);

    pthread_join(*radio_capture, NULL);
    if (loop_started)
        pthread_join(*loop_capture, NULL);
}
